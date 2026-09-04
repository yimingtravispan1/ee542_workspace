#include <iostream>
#include <fstream>
#include <vector>
#include <map>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "protocol.h"

int main(int argc, char* argv[])
{
    if (argc != 3)
    {
        std::cerr << "Usage: " << argv[0] << " <port> <output_file>\n";
        return 1;
    }

    int port = std::atoi(argv[1]);
    const char* output_file = argv[2];

    // ------------------------------------------------------------
    // 1. Create UDP socket
    // ------------------------------------------------------------

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);

    if (sockfd < 0)
    {
        perror("socket");
        return 1;
    }

    // Leave enough room for packets received while the network thread briefly
    // updates the reorder and writer queues.
    int socket_buffer_size = 16 * 1024 * 1024;

    setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF,
               &socket_buffer_size, sizeof(socket_buffer_size));

    // ------------------------------------------------------------
    // 2. Bind socket to local UDP port
    // ------------------------------------------------------------

    sockaddr_in receiver_addr{};

    receiver_addr.sin_family = AF_INET;
    receiver_addr.sin_addr.s_addr = INADDR_ANY;
    receiver_addr.sin_port = htons(port);

    if (bind(sockfd, reinterpret_cast<sockaddr*>(&receiver_addr), sizeof(receiver_addr)) < 0)
    {
        perror("bind");
        close(sockfd);
        return 1;
    }

    std::cout << "Receiver listening on UDP port " << port << "...\n";

    // ------------------------------------------------------------
    // 3. Wait for META packet
    // ------------------------------------------------------------

    // Buffer is large enough for both protocol header and payload
    std::vector<char> packet_buffer(sizeof(PacketHeader) + MAX_PAYLOAD_SIZE);

    sockaddr_in sender_addr{};
    socklen_t sender_addr_len = sizeof(sender_addr);

    ssize_t received = recvfrom(sockfd, packet_buffer.data(), packet_buffer.size(), 0,
    reinterpret_cast<sockaddr*>(&sender_addr), &sender_addr_len);

    // META packet should contain both PacketHeader and MetaPayload
    if (received < static_cast<ssize_t>(sizeof(PacketHeader) + sizeof(MetaPayload)))
    {
        std::cerr << "Invalid META packet\n";
        close(sockfd);
        return 1;
    }

    PacketHeader meta_header{};

    std::memcpy(&meta_header, packet_buffer.data(), sizeof(PacketHeader));

    // Make sure this is a valid META packet from our protocol
    if (meta_header.protocol_id != PROTOCOL_ID ||
        meta_header.type != static_cast<uint8_t>(PacketType::META))
    {
        std::cerr << "Expected META packet\n";
        close(sockfd);
        return 1;
    }

    MetaPayload meta{};

    std::memcpy(&meta,packet_buffer.data() + sizeof(PacketHeader),sizeof(MetaPayload));

    std::cout << "Incoming file information:\n";
    std::cout << "  File size: " << meta.file_size << " bytes\n";
    std::cout << "  Payload size: " << meta.payload_size << " bytes\n";
    std::cout << "  Total packets: " << meta.total_packets << "\n";

    // ------------------------------------------------------------
    // 4. Open output file
    // ------------------------------------------------------------

    std::ofstream output(output_file, std::ios::binary);

    if (!output)
    {
        std::cerr << "Cannot open output file\n";
        close(sockfd);
        return 1;
    }

    // The network thread only receives, validates, acknowledges, and orders
    // packets. A dedicated writer keeps file I/O from delaying UDP reception.
    std::deque<std::vector<char>> write_queue;
    std::mutex write_queue_mutex;
    std::condition_variable data_ready;
    bool writer_stop = false;
    std::atomic<bool> writer_failed{false};
    uint64_t bytes_written = 0;

    std::thread writer([&]()
    {
        while (true)
        {
            std::vector<char> data;

            {
                std::unique_lock<std::mutex> lock(write_queue_mutex);

                data_ready.wait(lock, [&]()
                {
                    return writer_stop || !write_queue.empty();
                });

                if (write_queue.empty())
                {
                    if (writer_stop)
                    {
                        break;
                    }

                    continue;
                }

                data = std::move(write_queue.front());
                write_queue.pop_front();
            }

            output.write(data.data(), data.size());

            if (!output)
            {
                writer_failed.store(true, std::memory_order_relaxed);
                break;
            }

            bytes_written += data.size();
        }
    });

    // ------------------------------------------------------------
    // 5. Send META_ACK
    // ------------------------------------------------------------

    PacketHeader meta_ack{};

    meta_ack.protocol_id = PROTOCOL_ID;
    meta_ack.seq = 0;
    meta_ack.length = 0;
    meta_ack.type = static_cast<uint8_t>(PacketType::META_ACK);
    meta_ack.reserved = 0;

    sendto(sockfd, &meta_ack, sizeof(meta_ack), 0,
    reinterpret_cast<sockaddr*>(&sender_addr), sender_addr_len);

    // ------------------------------------------------------------
    // 6. Receive DATA packets
    // ------------------------------------------------------------

    uint32_t expected_seq = 0;

    // Store packets that arrive before earlier packets
    std::map<uint32_t, std::vector<char>> out_of_order_buffer;

    bool transfer_finished = false;

    while (!transfer_finished)
    {
        received = recvfrom(sockfd, packet_buffer.data(), packet_buffer.size(), 0,
                            reinterpret_cast<sockaddr*>(&sender_addr), &sender_addr_len);

        if (received < static_cast<ssize_t>(sizeof(PacketHeader)))
        {
            continue;
        }

        PacketHeader header{};

        std::memcpy(&header, packet_buffer.data(), sizeof(PacketHeader));

        // Ignore packets that do not belong to our protocol
        if (header.protocol_id != PROTOCOL_ID)
        {
            continue;
        }

        PacketType type = static_cast<PacketType>(header.type);

        // --------------------------------------------------------
        // Handle duplicate META
        // --------------------------------------------------------

        // META may be retransmitted when META_ACK is lost.
        // The receiver should resend META_ACK without resetting the file.
        if (type == PacketType::META)
        {
            PacketHeader duplicate_meta_ack{};

            duplicate_meta_ack.protocol_id = PROTOCOL_ID;
            duplicate_meta_ack.seq = 0;
            duplicate_meta_ack.length = 0;
            duplicate_meta_ack.type = static_cast<uint8_t>(PacketType::META_ACK);
            duplicate_meta_ack.reserved = 0;

            sendto(sockfd, &duplicate_meta_ack, sizeof(duplicate_meta_ack), 0,
                   reinterpret_cast<sockaddr*>(&sender_addr), sender_addr_len);

            continue;
        }

        // --------------------------------------------------------
        // Handle DATA packet
        // --------------------------------------------------------

        if (type == PacketType::DATA)
        {
            // Check whether the payload length is valid
            if (header.length > meta.payload_size ||
                header.length > MAX_PAYLOAD_SIZE)
            {
                continue;
            }

            if (received < static_cast<ssize_t>(sizeof(PacketHeader) + header.length))
            {
                continue;
            }

            // Save the packet only if it has not already been written
            // or stored in the out-of-order buffer
            if (header.seq >= expected_seq &&
                out_of_order_buffer.find(header.seq) == out_of_order_buffer.end())
            {
                std::vector<char> data(header.length);

                std::memcpy(data.data(),
                            packet_buffer.data() + sizeof(PacketHeader),
                            header.length);

                out_of_order_buffer.emplace(header.seq, std::move(data));
            }

            // Send ACK for every valid DATA packet.
            // Duplicate DATA packets also receive another ACK.
            PacketHeader ack{};

            ack.protocol_id = PROTOCOL_ID;
            ack.seq = header.seq;
            ack.length = 0;
            ack.type = static_cast<uint8_t>(PacketType::ACK);
            ack.reserved = 0;

            sendto(sockfd, &ack, sizeof(ack), 0,
            reinterpret_cast<sockaddr*>(&sender_addr), sender_addr_len);

            // Move every newly contiguous packet to the writer thread.
            {
                std::lock_guard<std::mutex> lock(write_queue_mutex);

                while (true)
                {
                    auto it = out_of_order_buffer.find(expected_seq);

                    if (it == out_of_order_buffer.end())
                    {
                        break;
                    }

                    write_queue.push_back(std::move(it->second));

                    out_of_order_buffer.erase(it);
                    ++expected_seq;
                }
            }

            data_ready.notify_one();
        }

        // --------------------------------------------------------
        // Handle FIN packet
        // --------------------------------------------------------

        else if (type == PacketType::FIN)
        {
            // Only finish after every DATA packet has been received and queued
            // in order. The writer thread is joined before the process exits.
            if (expected_seq == meta.total_packets)
            {
                PacketHeader fin_ack{};

                fin_ack.protocol_id = PROTOCOL_ID;
                fin_ack.seq = expected_seq;
                fin_ack.length = 0;
                fin_ack.type = static_cast<uint8_t>(PacketType::FIN_ACK);
                fin_ack.reserved = 0;

                sendto(sockfd, &fin_ack, sizeof(fin_ack), 0,
                       reinterpret_cast<sockaddr*>(&sender_addr), sender_addr_len);

                transfer_finished = true;
            }
        }
    }

    // ------------------------------------------------------------
    // 7. FIN grace period
    // ------------------------------------------------------------

    // FIN_ACK can also be lost because UDP is unreliable.
    // Keep the receiver alive for a short time so it can reply
    // again if the sender retransmits FIN.
    constexpr int FIN_GRACE_MS = 2000;

    auto grace_start = std::chrono::steady_clock::now();

    while (true)
    {
        auto now = std::chrono::steady_clock::now();

        auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - grace_start
            ).count();

        if (elapsed_ms >= FIN_GRACE_MS)
        {
            break;
        }

        PacketHeader header{};

        ssize_t grace_received = recvfrom(sockfd, &header, sizeof(header), MSG_DONTWAIT,
                                          reinterpret_cast<sockaddr*>(&sender_addr),
                                          &sender_addr_len);

        if (grace_received >= static_cast<ssize_t>(sizeof(PacketHeader)))
        {
            if (header.protocol_id == PROTOCOL_ID &&
                header.type == static_cast<uint8_t>(PacketType::FIN))
            {
                PacketHeader fin_ack{};

                fin_ack.protocol_id = PROTOCOL_ID;
                fin_ack.seq = expected_seq;
                fin_ack.length = 0;
                fin_ack.type = static_cast<uint8_t>(PacketType::FIN_ACK);
                fin_ack.reserved = 0;

                sendto(sockfd, &fin_ack, sizeof(fin_ack), 0,
                       reinterpret_cast<sockaddr*>(&sender_addr), sender_addr_len);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    {
        std::lock_guard<std::mutex> lock(write_queue_mutex);
        writer_stop = true;
    }

    data_ready.notify_one();
    writer.join();

    // ------------------------------------------------------------
    // 8. Cleanup
    // ------------------------------------------------------------

    output.close();
    close(sockfd);

    std::cout << "Transfer complete.\n";
    std::cout << "Bytes written: " << bytes_written << "\n";
    std::cout << "Saved to: " << output_file << "\n";

    if (writer_failed.load(std::memory_order_relaxed))
    {
        std::cerr << "Failed while writing output file\n";
        return 1;
    }

    return 0;
}
