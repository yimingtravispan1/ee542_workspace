#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using Clock = std::chrono::steady_clock;

namespace {
constexpr uint32_t MAGIC = 0x52554450; // RUDP
constexpr size_t PAYLOAD = 1200;
constexpr size_t WINDOW = 4096;
constexpr int RTO_MS = 350;
constexpr double TOTAL_RATE_MBPS = 98.0;

enum Type : uint8_t {
    META = 1, META_ACK = 2, HELLO = 3, HELLO_ACK = 4,
    DATA = 5, ACK = 6, FIN = 7, FIN_ACK = 8
};

#pragma pack(push, 1)
struct Header {
    uint32_t magic;
    uint8_t type;
    uint8_t stream;
    uint16_t length;
    uint64_t sequence;
    uint64_t offset;
};
struct Metadata {
    Header header;
    uint64_t file_size;
    uint32_t streams;
};
#pragma pack(pop)

struct Pending {
    uint64_t offset;
    uint16_t length;
    Clock::time_point sent_at;
};

uint64_t hton64(uint64_t value) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return (uint64_t(htonl(uint32_t(value))) << 32) | htonl(uint32_t(value >> 32));
#else
    return value;
#endif
}
uint64_t ntoh64(uint64_t value) { return hton64(value); }

void require(bool condition, const char* operation) {
    if (!condition)
        throw std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
}

Header make_header(Type type, int stream = 0, uint16_t length = 0,
                   uint64_t sequence = 0, uint64_t offset = 0) {
    Header header{};
    header.magic = htonl(MAGIC);
    header.type = type;
    header.stream = static_cast<uint8_t>(stream);
    header.length = htons(length);
    header.sequence = hton64(sequence);
    header.offset = hton64(offset);
    return header;
}

bool valid(const Header& header, Type type) {
    return ntohl(header.magic) == MAGIC && header.type == type;
}

int make_socket() {
    int fd = socket(AF_INET, SOCK_DGRAM, 0); // UDP only
    require(fd >= 0, "socket");
    int buffer_size = 8 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size));
    return fd;
}

sockaddr_in make_address(const std::string& ip, int port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (ip.empty()) address.sin_addr.s_addr = htonl(INADDR_ANY);
    else require(inet_pton(AF_INET, ip.c_str(), &address.sin_addr) == 1, "inet_pton");
    return address;
}

void bind_socket(int fd, int port) {
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    auto address = make_address("", port);
    require(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "bind");
}

void set_timeout(int fd, int milliseconds) {
    timeval timeout{milliseconds / 1000, (milliseconds % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

void send_control(int fd, const sockaddr_in& peer, Type type, int stream,
                  uint64_t sequence = 0) {
    Header header = make_header(type, stream, 0, sequence);
    sendto(fd, &header, sizeof(header), 0,
           reinterpret_cast<const sockaddr*>(&peer), sizeof(peer));
}

uint64_t get_file_size(int fd) {
    struct stat status{};
    require(fstat(fd, &status) == 0, "fstat");
    return static_cast<uint64_t>(status.st_size);
}

// Client-side sender worker. Each worker sends every nth packet and maintains
// an independent selective-repeat window.
void sender_worker(const std::string& server_ip, int source_fd, uint64_t file_size,
                   int base_port, int id, int streams,
                   std::atomic<uint64_t>& acknowledged_bytes,
                   std::atomic<uint64_t>& wire_bytes,
                   std::atomic<uint64_t>& retransmissions) {
    int fd = make_socket();
    auto server = make_address(server_ip, base_port + 1 + id);
    set_timeout(fd, 40);

    // Repeat HELLO because it is also UDP and can be lost.
    bool ready = false;
    while (!ready) {
        send_control(fd, server, HELLO, id);
        Header reply{};
        ssize_t n = recvfrom(fd, &reply, sizeof(reply), 0, nullptr, nullptr);
        ready = n == static_cast<ssize_t>(sizeof(reply)) &&
                valid(reply, HELLO_ACK) && reply.stream == id;
    }

    const uint64_t packet_count = (file_size + PAYLOAD - 1) / PAYLOAD;
    uint64_t next_sequence = id;
    std::unordered_map<uint64_t, Pending> pending;
    std::vector<char> packet(sizeof(Header) + PAYLOAD);

    const double bytes_per_second = TOTAL_RATE_MBPS * 1000000.0 / 8.0 / streams;
    auto pacing_start = Clock::now();
    uint64_t paced_bytes = 0;

    auto transmit = [&](uint64_t sequence, Pending& item, bool retry) {
        Header header = make_header(DATA, id, item.length, sequence, item.offset);
        std::memcpy(packet.data(), &header, sizeof(header));
        ssize_t bytes = pread(source_fd, packet.data() + sizeof(header),
                              item.length, item.offset);
        require(bytes == item.length, "pread");
        ssize_t sent = sendto(fd, packet.data(), sizeof(header) + item.length, 0,
                              reinterpret_cast<const sockaddr*>(&server), sizeof(server));
        require(sent == static_cast<ssize_t>(sizeof(header) + item.length), "sendto DATA");
        wire_bytes += item.length;
        if (retry) ++retransmissions;
        item.sent_at = Clock::now();
    };

    while (next_sequence < packet_count || !pending.empty()) {
        while (next_sequence < packet_count && pending.size() < WINDOW) {
            uint64_t offset = next_sequence * PAYLOAD;
            uint16_t length = static_cast<uint16_t>(
                std::min<uint64_t>(PAYLOAD, file_size - offset));

            auto due = pacing_start + std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(paced_bytes / bytes_per_second));
            if (due > Clock::now()) std::this_thread::sleep_until(due);

            Pending item{offset, length, Clock::now()};
            transmit(next_sequence, item, false);
            paced_bytes += sizeof(Header) + length;
            pending.emplace(next_sequence, item);
            next_sequence += streams;
        }

        Header ack{};
        ssize_t n = recvfrom(fd, &ack, sizeof(ack), 0, nullptr, nullptr);
        if (n == static_cast<ssize_t>(sizeof(ack)) && valid(ack, ACK) &&
            ack.stream == id) {
            uint64_t sequence = ntoh64(ack.sequence);
            auto item = pending.find(sequence);
            if (item != pending.end()) {
                acknowledged_bytes += item->second.length;
                pending.erase(item);
            }
        }

        auto now = Clock::now();
        for (auto& [sequence, item] : pending) {
            if (std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - item.sent_at).count() >= RTO_MS)
                transmit(sequence, item, true);
        }
    }

    // FIN is retransmitted until the receiver confirms it.
    bool finished = false;
    for (int attempt = 0; attempt < 300 && !finished; ++attempt) {
        send_control(fd, server, FIN, id);
        Header reply{};
        ssize_t n = recvfrom(fd, &reply, sizeof(reply), 0, nullptr, nullptr);
        finished = n == static_cast<ssize_t>(sizeof(reply)) &&
                   valid(reply, FIN_ACK) && reply.stream == id;
    }
    close(fd);
    if (!finished) throw std::runtime_error("FIN handshake timed out");
}

// Server-side receiver worker. pwrite allows all threads to safely place
// packets at their correct offsets in the same destination file.
void receiver_worker(int output_fd, uint64_t file_size, int base_port,
                     int id, int streams, std::atomic<uint64_t>& received_bytes) {
    int fd = make_socket();
    bind_socket(fd, base_port + 1 + id);
    set_timeout(fd, 100);

    sockaddr_in client{};
    socklen_t client_length = sizeof(client);
    Header hello{};
    while (true) {
        ssize_t n = recvfrom(fd, &hello, sizeof(hello), 0,
                             reinterpret_cast<sockaddr*>(&client), &client_length);
        if (n == static_cast<ssize_t>(sizeof(hello)) &&
            valid(hello, HELLO) && hello.stream == id) {
            send_control(fd, client, HELLO_ACK, id);
            break;
        }
    }

    const uint64_t packet_count = (file_size + PAYLOAD - 1) / PAYLOAD;
    const uint64_t expected = packet_count <= static_cast<uint64_t>(id)
        ? 0 : (packet_count - 1 - id) / streams + 1;
    std::vector<uint8_t> received(expected, 0);
    uint64_t completed = 0;
    std::vector<char> packet(sizeof(Header) + PAYLOAD);

    while (completed < expected) {
        ssize_t n = recvfrom(fd, packet.data(), packet.size(), 0, nullptr, nullptr);
        if (n < static_cast<ssize_t>(sizeof(Header))) continue;

        Header header{};
        std::memcpy(&header, packet.data(), sizeof(header));
        if (!valid(header, DATA) || header.stream != id) continue;

        uint64_t sequence = ntoh64(header.sequence);
        uint64_t offset = ntoh64(header.offset);
        uint16_t length = ntohs(header.length);
        if (sequence < static_cast<uint64_t>(id) ||
            (sequence - id) % streams != 0 || offset + length > file_size ||
            n != static_cast<ssize_t>(sizeof(Header) + length)) continue;

        uint64_t local_sequence = (sequence - id) / streams;
        if (local_sequence < received.size() && !received[local_sequence]) {
            require(pwrite(output_fd, packet.data() + sizeof(Header), length, offset) == length,
                    "pwrite");
            received[local_sequence] = 1;
            ++completed;
            received_bytes += length;
        }
        // ACK duplicates too, because an earlier ACK may have been lost.
        send_control(fd, client, ACK, id, sequence);
    }

    // Continue acknowledging duplicate final packets until FIN arrives.
    bool saw_fin = false;
    while (!saw_fin) {
        ssize_t n = recvfrom(fd, packet.data(), packet.size(), 0, nullptr, nullptr);
        if (n < static_cast<ssize_t>(sizeof(Header))) continue;
        Header header{};
        std::memcpy(&header, packet.data(), sizeof(header));
        if (ntohl(header.magic) != MAGIC || header.stream != id) continue;
        if (header.type == DATA)
            send_control(fd, client, ACK, id, ntoh64(header.sequence));
        else if (header.type == FIN) {
            send_control(fd, client, FIN_ACK, id);
            saw_fin = true;
        }
    }

    // Briefly answer repeated FIN packets in case FIN_ACK was lost.
    auto linger_until = Clock::now() + std::chrono::seconds(2);
    while (Clock::now() < linger_until) {
        Header header{};
        ssize_t n = recvfrom(fd, &header, sizeof(header), 0, nullptr, nullptr);
        if (n == static_cast<ssize_t>(sizeof(header)) &&
            valid(header, FIN) && header.stream == id)
            send_control(fd, client, FIN_ACK, id);
    }
    close(fd);
}

int run_server(int port, const std::string& output_path, int requested_streams) {
    int control = make_socket();
    bind_socket(control, port);
    set_timeout(control, 500);
    std::cout << "Waiting for sender on UDP port " << port << "..." << std::endl;

    sockaddr_in client{};
    socklen_t client_length = sizeof(client);
    Metadata metadata{};
    while (true) {
        ssize_t n = recvfrom(control, &metadata, sizeof(metadata), 0,
                             reinterpret_cast<sockaddr*>(&client), &client_length);
        if (n == static_cast<ssize_t>(sizeof(metadata)) &&
            valid(metadata.header, META)) break;
    }
    uint64_t file_size = ntoh64(metadata.file_size);
    int streams = static_cast<int>(ntohl(metadata.streams));
    require(streams == requested_streams && streams > 0 && streams <= 32,
            "stream-count mismatch");

    // Keep replying briefly so a lost META_ACK cannot deadlock startup.
    for (int i = 0; i < 10; ++i)
        send_control(control, client, META_ACK, 0);
    close(control);

    int output_fd = open(output_path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0644);
    require(output_fd >= 0, "open destination");
    require(ftruncate(output_fd, file_size) == 0, "ftruncate");

    std::atomic<uint64_t> received_bytes{0};
    std::atomic<bool> stop_monitor{false};
    auto start = Clock::now();
    std::thread monitor([&] {
        while (!stop_monitor) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!stop_monitor) {
                double percent = file_size ? 100.0 * received_bytes / file_size : 100.0;
                std::cout << "Received " << received_bytes << " / " << file_size
                          << " bytes (" << percent << "%)" << std::endl;
            }
        }
    });

    std::vector<std::thread> workers;
    for (int i = 0; i < streams; ++i)
        workers.emplace_back(receiver_worker, output_fd, file_size, port,
                             i, streams, std::ref(received_bytes));
    for (auto& worker : workers) worker.join();
    stop_monitor = true;
    monitor.join();
    fsync(output_fd);
    close(output_fd);

    double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    double throughput = received_bytes * 8.0 / seconds / 1e6;
    bool complete = received_bytes == file_size;
    std::cout << "Received: " << received_bytes << " / " << file_size
              << " bytes\nTime: " << seconds
              << " s\nUseful throughput: " << throughput
              << " Mbps\nCompleteness: " << (complete ? "PASS" : "FAIL")
              << "\n20 Mbps requirement: "
              << (complete && throughput >= 20.0 ? "PASS" : "FAIL") << '\n';
    return complete ? 0 : 2;
}

int run_client(const std::string& server_ip, int port,
               const std::string& source_path, int streams) {
    require(streams > 0 && streams <= 32, "invalid stream count");
    int source_fd = open(source_path.c_str(), O_RDONLY);
    require(source_fd >= 0, "open source");
    uint64_t file_size = get_file_size(source_fd);

    int control = make_socket();
    auto server = make_address(server_ip, port);
    set_timeout(control, 300);
    Metadata metadata{make_header(META), hton64(file_size), htonl(streams)};
    bool accepted = false;
    while (!accepted) {
        sendto(control, &metadata, sizeof(metadata), 0,
               reinterpret_cast<const sockaddr*>(&server), sizeof(server));
        Header reply{};
        ssize_t n = recvfrom(control, &reply, sizeof(reply), 0, nullptr, nullptr);
        accepted = n == static_cast<ssize_t>(sizeof(reply)) && valid(reply, META_ACK);
    }
    close(control);

    std::atomic<uint64_t> acknowledged_bytes{0}, wire_bytes{0}, retransmissions{0};
    std::atomic<bool> stop_monitor{false};
    auto start = Clock::now();
    std::thread monitor([&] {
        while (!stop_monitor) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!stop_monitor) {
                double percent = file_size ? 100.0 * acknowledged_bytes / file_size : 100.0;
                std::cout << "Acknowledged " << acknowledged_bytes << " / " << file_size
                          << " bytes (" << percent << "%), retransmissions "
                          << retransmissions << std::endl;
            }
        }
    });

    std::vector<std::thread> workers;
    for (int i = 0; i < streams; ++i)
        workers.emplace_back(sender_worker, server_ip, source_fd, file_size,
                             port, i, streams, std::ref(acknowledged_bytes),
                             std::ref(wire_bytes), std::ref(retransmissions));
    for (auto& worker : workers) worker.join();
    stop_monitor = true;
    monitor.join();
    close(source_fd);

    double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    double throughput = acknowledged_bytes * 8.0 / seconds / 1e6;
    std::cout << "Acknowledged file bytes: " << acknowledged_bytes
              << "\nTime: " << seconds
              << " s\nUseful throughput: " << throughput
              << " Mbps\nUDP payload sent: " << wire_bytes
              << " bytes\nRetransmissions: " << retransmissions
              << "\n20 Mbps requirement: "
              << (acknowledged_bytes == file_size && throughput >= 20.0 ? "PASS" : "FAIL")
              << '\n';
    return acknowledged_bytes == file_size ? 0 : 2;
}

void usage(const char* program) {
    std::cerr << "Usage:\n"
              << "  Receiver/server: " << program
              << " server <port> <destination-file> <threads>\n"
              << "  Sender/client:   " << program
              << " client <server-ip> <port> <source-file> <threads>\n";
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 5 && std::string(argv[1]) == "server")
            return run_server(std::stoi(argv[2]), argv[3], std::stoi(argv[4]));
        if (argc == 6 && std::string(argv[1]) == "client")
            return run_client(argv[2], std::stoi(argv[3]), argv[4], std::stoi(argv[5]));
        usage(argv[0]);
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}