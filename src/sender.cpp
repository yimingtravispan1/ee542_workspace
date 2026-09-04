#include <iostream>
#include <fstream>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#include "protocol.h"

// Store a packet until its ACK is received
struct OutstandingPacket
{
    std::shared_ptr<const std::vector<char>> packet;
    std::chrono::steady_clock::time_point sent_time;
    bool retransmit_scheduled = false;
};

// ------------------------------------------------------------
// Packet pacing
// ------------------------------------------------------------

// Limit the sending speed so UDP packets do not overflow
// the rate-limited network queue
void pace_packet(
    std::chrono::steady_clock::time_point& next_send_time,
    std::size_t packet_size,
    double rate_mbps)
{
    // IPv4 header (20 bytes) + UDP header (8 bytes)
    constexpr std::size_t NETWORK_OVERHEAD = 28;

    std::size_t wire_bytes = packet_size + NETWORK_OVERHEAD;

    double interval_seconds =
        (static_cast<double>(wire_bytes) * 8.0) /
        (rate_mbps * 1'000'000.0);

    auto interval = std::chrono::duration<double>(interval_seconds);

    next_send_time +=
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(interval);

    auto now = std::chrono::steady_clock::now();

    if (next_send_time > now)
    {
        std::this_thread::sleep_until(next_send_time);
    }
    else
    {
        // Reset the schedule if the sender is already behind
        next_send_time = now;
    }
}

// ------------------------------------------------------------
// Wait for control packet
// ------------------------------------------------------------

// Used when waiting for META_ACK or FIN_ACK
bool wait_for_packet_type(
    int sockfd,
    PacketType expected_type,
    int timeout_ms,
    PacketHeader& received_header)
{
    auto start = std::chrono::steady_clock::now();

    while (true)
    {
        ssize_t received = recvfrom(sockfd, &received_header, sizeof(received_header),
                                    MSG_DONTWAIT, nullptr, nullptr);

        if (received >= static_cast<ssize_t>(sizeof(PacketHeader)))
        {
            if (received_header.protocol_id == PROTOCOL_ID &&
                received_header.type == static_cast<uint8_t>(expected_type))
            {
                return true;
            }
        }

        auto now = std::chrono::steady_clock::now();

        auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - start
            ).count();

        if (elapsed_ms >= timeout_ms)
        {
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// ------------------------------------------------------------
// ACK receiver thread
// ------------------------------------------------------------

// DATA ACKs are received independently from the paced sending loop. This lets
// acknowledged packets leave the window immediately instead of waiting until
// the sender has finished a whole window of sends.
void receive_acks(
    int sockfd,
    std::atomic<bool>& stop_requested,
    std::unordered_map<uint32_t, OutstandingPacket>& outstanding,
    std::mutex& outstanding_mutex,
    std::condition_variable& window_changed)
{
    while (!stop_requested.load(std::memory_order_relaxed))
    {
        pollfd descriptor{};
        descriptor.fd = sockfd;
        descriptor.events = POLLIN;

        int ready = poll(&descriptor, 1, 20);

        if (ready <= 0 || (descriptor.revents & POLLIN) == 0)
        {
            continue;
        }

        // Drain every ACK already queued by the kernel before polling again.
        while (true)
        {
            PacketHeader ack{};

            ssize_t received = recvfrom(sockfd, &ack, sizeof(ack),
                                        MSG_DONTWAIT, nullptr, nullptr);

            if (received < static_cast<ssize_t>(sizeof(PacketHeader)))
            {
                break;
            }

            if (ack.protocol_id != PROTOCOL_ID ||
                ack.type != static_cast<uint8_t>(PacketType::ACK))
            {
                continue;
            }

            bool removed = false;

            {
                std::lock_guard<std::mutex> lock(outstanding_mutex);
                removed = outstanding.erase(ack.seq) != 0;
            }

            if (removed)
            {
                window_changed.notify_one();
            }
        }
    }
}

int main(int argc, char* argv[])
{
    // Check command line arguments
    if (argc < 4 || argc > 7)
    {
        std::cerr << "Usage: " << argv[0]
                  << " <receiver_ip> <port> <input_file>"
                  << " [payload_size] [window_size] [pacing_rate_mbps]\n";
        return 1;
    }

    const char* receiver_ip = argv[1];
    int port = std::atoi(argv[2]);
    const char* input_file = argv[3];

    std::size_t payload_size = PAYLOAD_MTU_1500;
    std::size_t window_size = DEFAULT_WINDOW_SIZE;

    // Use a lower rate than the 100 Mbit/s link to avoid large bursts
    double pacing_rate_mbps = 90.0;

    if (argc >= 5)
    {
        payload_size = static_cast<std::size_t>(std::stoul(argv[4]));
    }

    if (argc >= 6)
    {
        window_size = static_cast<std::size_t>(std::stoul(argv[5]));
    }

    if (argc >= 7)
    {
        pacing_rate_mbps = std::stod(argv[6]);
    }

    if (payload_size == 0 || payload_size > MAX_PAYLOAD_SIZE)
    {
        std::cerr << "Invalid payload size\n";
        return 1;
    }

    if (window_size == 0)
    {
        std::cerr << "Invalid window size\n";
        return 1;
    }

    if (pacing_rate_mbps <= 0.0)
    {
        std::cerr << "Invalid pacing rate\n";
        return 1;
    }

    // ------------------------------------------------------------
    // 1. Open input file
    // ------------------------------------------------------------

    std::ifstream input(input_file, std::ios::binary | std::ios::ate);

    if (!input)
    {
        std::cerr << "Cannot open input file: " << input_file << "\n";
        return 1;
    }

    uint64_t file_size = static_cast<uint64_t>(input.tellg());

    input.seekg(0, std::ios::beg);

    uint32_t total_packets =
        static_cast<uint32_t>((file_size + payload_size - 1) / payload_size);

    std::cout << "File information:\n";
    std::cout << "  File size: " << file_size << " bytes\n";
    std::cout << "  Payload size: " << payload_size << " bytes\n";
    std::cout << "  Total packets: " << total_packets << "\n";
    std::cout << "  Window size: " << window_size << " packets\n";
    std::cout << "  Pacing rate: " << pacing_rate_mbps << " Mbit/s\n";

    // ------------------------------------------------------------
    // 2. Create UDP socket
    // ------------------------------------------------------------

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);

    if (sockfd < 0)
    {
        perror("socket");
        return 1;
    }

    // Increase socket buffers for high-bandwidth transfers
    int socket_buffer_size = 16 * 1024 * 1024;

    setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF,
               &socket_buffer_size, sizeof(socket_buffer_size));

    setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF,
               &socket_buffer_size, sizeof(socket_buffer_size));

    // ------------------------------------------------------------
    // 3. Configure receiver address
    // ------------------------------------------------------------

    sockaddr_in receiver_addr{};

    receiver_addr.sin_family = AF_INET;
    receiver_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, receiver_ip, &receiver_addr.sin_addr) != 1)
    {
        std::cerr << "Invalid receiver IP address\n";
        close(sockfd);
        return 1;
    }

    // ------------------------------------------------------------
    // 4. META handshake
    // ------------------------------------------------------------

    // META packet tells the receiver about the incoming file
    std::vector<char> meta_packet(sizeof(PacketHeader) + sizeof(MetaPayload));

    PacketHeader meta_header{};

    meta_header.protocol_id = PROTOCOL_ID;
    meta_header.seq = 0;
    meta_header.length = sizeof(MetaPayload);
    meta_header.type = static_cast<uint8_t>(PacketType::META);
    meta_header.reserved = 0;

    MetaPayload meta{};

    meta.file_size = file_size;
    meta.payload_size = static_cast<uint32_t>(payload_size);
    meta.total_packets = total_packets;

    std::memcpy(meta_packet.data(), &meta_header, sizeof(meta_header));

    std::memcpy(meta_packet.data() + sizeof(meta_header),
                &meta, sizeof(meta));

    std::cout << "Starting META handshake...\n";

    while (true)
    {
        sendto(sockfd, meta_packet.data(), meta_packet.size(), 0,
               reinterpret_cast<sockaddr*>(&receiver_addr), sizeof(receiver_addr));

        PacketHeader response{};

        if (wait_for_packet_type(sockfd, PacketType::META_ACK,
                                 DEFAULT_RTO_MS, response))
        {
            break;
        }

        std::cout << "META timeout, retransmitting...\n";
    }

    std::cout << "META handshake complete.\n";

    // ------------------------------------------------------------
    // 5. Start transfer
    // ------------------------------------------------------------

    auto transfer_start = std::chrono::steady_clock::now();
    auto next_send_time = transfer_start;

    uint32_t next_seq = 0;

    // Packets that were sent but have not received an ACK yet
    std::unordered_map<uint32_t, OutstandingPacket> outstanding;

    // The ACK thread is the only thread that receives from the socket during
    // the DATA phase. The main thread remains the only DATA sender.
    std::mutex outstanding_mutex;
    std::condition_variable window_changed;
    std::atomic<bool> stop_ack_receiver{false};

    std::thread ack_receiver(
        receive_acks,
        sockfd,
        std::ref(stop_ack_receiver),
        std::ref(outstanding),
        std::ref(outstanding_mutex),
        std::ref(window_changed));

    uint64_t retransmissions = 0;
    uint64_t packets_sent = 0;

    // ------------------------------------------------------------
    // 6. Sliding-window transfer
    // ------------------------------------------------------------

    // Bound how long new DATA can delay the retransmission scan. ACK reception
    // remains continuous in the other thread.
    constexpr std::size_t SEND_BATCH_SIZE = 64;

    while (true)
    {
        // --------------------------------------------------------
        // Fill the sliding window
        // --------------------------------------------------------

        // Keep sending new packets while there is space in the window
        std::size_t sent_in_batch = 0;

        while (next_seq < total_packets &&
               sent_in_batch < SEND_BATCH_SIZE)
        {
            {
                std::lock_guard<std::mutex> lock(outstanding_mutex);

                if (outstanding.size() >= window_size)
                {
                    break;
                }
            }

            uint64_t offset =
                static_cast<uint64_t>(next_seq) * payload_size;

            std::size_t current_payload_size =
                static_cast<std::size_t>(
                    std::min<uint64_t>(payload_size, file_size - offset)
                );

            std::vector<char> packet(sizeof(PacketHeader) + current_payload_size);

            PacketHeader header{};

            header.protocol_id = PROTOCOL_ID;
            header.seq = next_seq;
            header.length = static_cast<uint16_t>(current_payload_size);
            header.type = static_cast<uint8_t>(PacketType::DATA);
            header.reserved = 0;

            std::memcpy(packet.data(), &header, sizeof(header));

            input.read(packet.data() + sizeof(PacketHeader),
                       current_payload_size);

            auto stored_packet =
                std::make_shared<const std::vector<char>>(std::move(packet));

            // Publish the packet before sending it. Otherwise the ACK thread
            // could receive a very fast ACK before the packet is in the map.
            {
                std::lock_guard<std::mutex> lock(outstanding_mutex);

                OutstandingPacket state{};
                state.packet = stored_packet;
                state.sent_time = std::chrono::steady_clock::now();

                outstanding.emplace(next_seq, std::move(state));
            }

            // Send one DATA packet. Only this thread sends DATA, preserving the
            // configured pacing order.
            sendto(sockfd, stored_packet->data(), stored_packet->size(), 0,
                   reinterpret_cast<sockaddr*>(&receiver_addr), sizeof(receiver_addr));

            // Control the sending rate
            pace_packet(next_send_time, stored_packet->size(), pacing_rate_mbps);

            ++next_seq;
            ++packets_sent;
            ++sent_in_batch;
        }

        // --------------------------------------------------------
        // Retransmit timed-out packets
        // --------------------------------------------------------

        auto now = std::chrono::steady_clock::now();

        struct Retransmission
        {
            uint32_t seq;
            std::shared_ptr<const std::vector<char>> packet;
        };

        std::vector<Retransmission> due_retransmissions;

        {
            std::lock_guard<std::mutex> lock(outstanding_mutex);

            for (auto& entry : outstanding)
            {
                OutstandingPacket& state = entry.second;

                auto elapsed_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - state.sent_time
                    ).count();

                if (!state.retransmit_scheduled &&
                    elapsed_ms >= DEFAULT_RTO_MS)
                {
                    state.retransmit_scheduled = true;
                    due_retransmissions.push_back({entry.first, state.packet});
                }
            }
        }

        for (const Retransmission& retransmission : due_retransmissions)
        {
            bool still_outstanding = false;

            {
                std::lock_guard<std::mutex> lock(outstanding_mutex);

                auto it = outstanding.find(retransmission.seq);

                if (it != outstanding.end() &&
                    it->second.packet == retransmission.packet)
                {
                    it->second.sent_time = std::chrono::steady_clock::now();
                    it->second.retransmit_scheduled = false;
                    still_outstanding = true;
                }
            }

            // The ACK thread may have removed this packet while the timeout
            // list was being built.
            if (!still_outstanding)
            {
                continue;
            }

            sendto(sockfd,
                   retransmission.packet->data(),
                   retransmission.packet->size(),
                   0,
                   reinterpret_cast<sockaddr*>(&receiver_addr),
                   sizeof(receiver_addr));

            pace_packet(next_send_time,
                        retransmission.packet->size(),
                        pacing_rate_mbps);

            ++retransmissions;
            ++packets_sent;
        }

        std::unique_lock<std::mutex> lock(outstanding_mutex);

        if (next_seq >= total_packets && outstanding.empty())
        {
            break;
        }

        // If no packet was sent, wait briefly for an ACK or the next timeout.
        // The timeout also ensures lost packets are periodically reconsidered.
        if (sent_in_batch == 0 && due_retransmissions.empty())
        {
            window_changed.wait_for(lock, std::chrono::milliseconds(1));
        }
    }

    stop_ack_receiver.store(true, std::memory_order_relaxed);
    ack_receiver.join();

    // ------------------------------------------------------------
    // 7. FIN handshake
    // ------------------------------------------------------------

    // All DATA packets are acknowledged before sending FIN
    PacketHeader fin{};

    fin.protocol_id = PROTOCOL_ID;
    fin.seq = total_packets;
    fin.length = 0;
    fin.type = static_cast<uint8_t>(PacketType::FIN);
    fin.reserved = 0;

    std::cout << "All DATA packets acknowledged.\n";

    while (true)
    {
        sendto(sockfd, &fin, sizeof(fin), 0,
               reinterpret_cast<sockaddr*>(&receiver_addr), sizeof(receiver_addr));

        PacketHeader response{};

        if (wait_for_packet_type(sockfd, PacketType::FIN_ACK,
                                 DEFAULT_RTO_MS, response))
        {
            break;
        }

        std::cout << "FIN timeout, retransmitting...\n";
    }

    auto transfer_end = std::chrono::steady_clock::now();

    // ------------------------------------------------------------
    // 8. Transfer statistics
    // ------------------------------------------------------------

    double elapsed_seconds =
        std::chrono::duration<double>(
            transfer_end - transfer_start
        ).count();

    double throughput_mbps =
        (static_cast<double>(file_size) * 8.0) /
        elapsed_seconds /
        1'000'000.0;

    std::cout << "\nTransfer complete.\n";
    std::cout << "Time: " << elapsed_seconds << " seconds\n";
    std::cout << "Throughput: " << throughput_mbps << " Mbit/s\n";
    std::cout << "Total UDP transmissions: " << packets_sent << "\n";
    std::cout << "Retransmissions: " << retransmissions << "\n";

    // ------------------------------------------------------------
    // 9. Cleanup
    // ------------------------------------------------------------

    input.close();
    close(sockfd);

    return 0;
}
