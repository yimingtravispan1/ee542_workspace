#include <iostream>
#include <fstream>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <algorithm>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

#include "protocol.h"

struct OutstandingPacket
{
    std::vector<char> packet;
    std::chrono::steady_clock::time_point sent_time;
};


// ------------------------------------------------------------
// Application-level pacing
//
// This prevents the UDP sender from injecting packets much
// faster than the 100 Mbit/s shaped link can handle.
// ------------------------------------------------------------

void pace_packet(
    std::chrono::steady_clock::time_point& next_send_time,
    std::size_t packet_size,
    double rate_mbps)
{
    // Approximate IPv4 + UDP header overhead.
    constexpr std::size_t NETWORK_OVERHEAD = 28;

    std::size_t wire_bytes =
        packet_size + NETWORK_OVERHEAD;

    double interval_seconds =
        (static_cast<double>(wire_bytes) * 8.0) /
        (rate_mbps * 1'000'000.0);

    auto interval =
        std::chrono::duration<double>(
            interval_seconds
        );

    next_send_time +=
        std::chrono::duration_cast<
            std::chrono::steady_clock::duration
        >(interval);

    auto now =
        std::chrono::steady_clock::now();

    if (next_send_time > now)
    {
        std::this_thread::sleep_until(
            next_send_time
        );
    }
    else
    {
        // If the sender has fallen behind schedule,
        // restart pacing from the current time.
        next_send_time = now;
    }
}


// ------------------------------------------------------------
// Wait for a specific control packet.
// Used by META/META_ACK and FIN/FIN_ACK handshakes.
// ------------------------------------------------------------

bool wait_for_packet_type(
    int sockfd,
    PacketType expected_type,
    int timeout_ms,
    PacketHeader& received_header)
{
    auto start =
        std::chrono::steady_clock::now();

    while (true)
    {
        ssize_t received = recvfrom(
            sockfd,
            &received_header,
            sizeof(received_header),
            MSG_DONTWAIT,
            nullptr,
            nullptr
        );

        if (received >=
            static_cast<ssize_t>(
                sizeof(PacketHeader)))
        {
            if (received_header.protocol_id ==
                    PROTOCOL_ID &&
                received_header.type ==
                    static_cast<uint8_t>(
                        expected_type))
            {
                return true;
            }
        }

        auto now =
            std::chrono::steady_clock::now();

        auto elapsed_ms =
            std::chrono::duration_cast<
                std::chrono::milliseconds>(
                now - start
            ).count();

        if (elapsed_ms >= timeout_ms)
        {
            return false;
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(1)
        );
    }
}


int main(int argc, char* argv[])
{
    if (argc < 4 || argc > 7)
    {
        std::cerr
            << "Usage: " << argv[0]
            << " <receiver_ip> <port> <input_file>"
            << " [payload_size]"
            << " [window_size]"
            << " [pacing_rate_mbps]\n";

        return 1;
    }

    const char* receiver_ip = argv[1];
    int port = std::atoi(argv[2]);
    const char* input_file = argv[3];

    std::size_t payload_size =
        PAYLOAD_MTU_1500;

    std::size_t window_size =
        DEFAULT_WINDOW_SIZE;

    // Start slightly below the 100 Mbit/s shaped link.
    double pacing_rate_mbps = 90.0;

    if (argc >= 5)
    {
        payload_size =
            static_cast<std::size_t>(
                std::stoul(argv[4])
            );
    }

    if (argc >= 6)
    {
        window_size =
            static_cast<std::size_t>(
                std::stoul(argv[5])
            );
    }

    if (argc >= 7)
    {
        pacing_rate_mbps =
            std::stod(argv[6]);
    }

    if (payload_size == 0 ||
        payload_size > MAX_PAYLOAD_SIZE)
    {
        std::cerr
            << "Invalid payload size\n";

        return 1;
    }

    if (window_size == 0)
    {
        std::cerr
            << "Invalid window size\n";

        return 1;
    }

    if (pacing_rate_mbps <= 0.0)
    {
        std::cerr
            << "Invalid pacing rate\n";

        return 1;
    }


    // ------------------------------------------------------------
    // 1. Open input file
    // ------------------------------------------------------------

    std::ifstream input(
        input_file,
        std::ios::binary |
        std::ios::ate
    );

    if (!input)
    {
        std::cerr
            << "Cannot open input file: "
            << input_file << "\n";

        return 1;
    }

    uint64_t file_size =
        static_cast<uint64_t>(
            input.tellg()
        );

    input.seekg(
        0,
        std::ios::beg
    );

    uint32_t total_packets =
        static_cast<uint32_t>(
            (file_size +
             payload_size - 1) /
            payload_size
        );

    std::cout
        << "File information:\n";

    std::cout
        << "  File size: "
        << file_size
        << " bytes\n";

    std::cout
        << "  Payload size: "
        << payload_size
        << " bytes\n";

    std::cout
        << "  Total packets: "
        << total_packets
        << "\n";

    std::cout
        << "  Window size: "
        << window_size
        << " packets\n";

    std::cout
        << "  Pacing rate: "
        << pacing_rate_mbps
        << " Mbit/s\n";


    // ------------------------------------------------------------
    // 2. Create UDP socket
    // ------------------------------------------------------------

    int sockfd = socket(
        AF_INET,
        SOCK_DGRAM,
        0
    );

    if (sockfd < 0)
    {
        perror("socket");
        return 1;
    }


    // ------------------------------------------------------------
    // Increase socket buffers
    // ------------------------------------------------------------

    int socket_buffer_size =
        16 * 1024 * 1024;

    setsockopt(
        sockfd,
        SOL_SOCKET,
        SO_SNDBUF,
        &socket_buffer_size,
        sizeof(socket_buffer_size)
    );

    setsockopt(
        sockfd,
        SOL_SOCKET,
        SO_RCVBUF,
        &socket_buffer_size,
        sizeof(socket_buffer_size)
    );


    // ------------------------------------------------------------
    // 3. Configure Receiver address
    // ------------------------------------------------------------

    sockaddr_in receiver_addr{};

    receiver_addr.sin_family =
        AF_INET;

    receiver_addr.sin_port =
        htons(port);

    if (inet_pton(
            AF_INET,
            receiver_ip,
            &receiver_addr.sin_addr) != 1)
    {
        std::cerr
            << "Invalid receiver IP address\n";

        close(sockfd);
        return 1;
    }


    // ------------------------------------------------------------
    // 4. META handshake
    // ------------------------------------------------------------

    std::vector<char> meta_packet(
        sizeof(PacketHeader) +
        sizeof(MetaPayload)
    );

    PacketHeader meta_header{};

    meta_header.protocol_id =
        PROTOCOL_ID;

    meta_header.seq = 0;

    meta_header.length =
        sizeof(MetaPayload);

    meta_header.type =
        static_cast<uint8_t>(
            PacketType::META
        );

    meta_header.reserved = 0;


    MetaPayload meta{};

    meta.file_size =
        file_size;

    meta.payload_size =
        static_cast<uint32_t>(
            payload_size
        );

    meta.total_packets =
        total_packets;


    std::memcpy(
        meta_packet.data(),
        &meta_header,
        sizeof(meta_header)
    );

    std::memcpy(
        meta_packet.data() +
            sizeof(meta_header),
        &meta,
        sizeof(meta)
    );


    std::cout
        << "Starting META handshake...\n";

    while (true)
    {
        sendto(
            sockfd,
            meta_packet.data(),
            meta_packet.size(),
            0,
            reinterpret_cast<sockaddr*>(
                &receiver_addr
            ),
            sizeof(receiver_addr)
        );

        PacketHeader response{};

        if (wait_for_packet_type(
                sockfd,
                PacketType::META_ACK,
                DEFAULT_RTO_MS,
                response))
        {
            break;
        }

        std::cout
            << "META timeout, "
            << "retransmitting...\n";
    }

    std::cout
        << "META handshake complete.\n";


    // ------------------------------------------------------------
    // 5. Start transfer timing
    // ------------------------------------------------------------

    auto transfer_start =
        std::chrono::steady_clock::now();

    auto next_send_time =
        transfer_start;

    uint32_t next_seq = 0;

    std::unordered_map<
        uint32_t,
        OutstandingPacket
    > outstanding;

    uint64_t retransmissions = 0;
    uint64_t packets_sent = 0;


    // ------------------------------------------------------------
    // 6. Main sliding-window loop
    // ------------------------------------------------------------

    while (next_seq < total_packets ||
           !outstanding.empty())
    {
        // --------------------------------------------------------
        // Fill the sliding window
        // --------------------------------------------------------

        while (next_seq < total_packets &&
               outstanding.size() <
                   window_size)
        {
            uint64_t offset =
                static_cast<uint64_t>(
                    next_seq
                ) * payload_size;

            std::size_t
                current_payload_size =
                    static_cast<std::size_t>(
                        std::min<uint64_t>(
                            payload_size,
                            file_size - offset
                        )
                    );

            std::vector<char> packet(
                sizeof(PacketHeader) +
                current_payload_size
            );

            PacketHeader header{};

            header.protocol_id =
                PROTOCOL_ID;

            header.seq =
                next_seq;

            header.length =
                static_cast<uint16_t>(
                    current_payload_size
                );

            header.type =
                static_cast<uint8_t>(
                    PacketType::DATA
                );

            header.reserved = 0;


            std::memcpy(
                packet.data(),
                &header,
                sizeof(header)
            );

            input.read(
                packet.data() +
                    sizeof(PacketHeader),
                current_payload_size
            );


            // ----------------------------------------------------
            // Send DATA packet
            // ----------------------------------------------------

            sendto(
                sockfd,
                packet.data(),
                packet.size(),
                0,
                reinterpret_cast<sockaddr*>(
                    &receiver_addr
                ),
                sizeof(receiver_addr)
            );


            // ----------------------------------------------------
            // Pace DATA transmission
            // ----------------------------------------------------

            pace_packet(
                next_send_time,
                packet.size(),
                pacing_rate_mbps
            );


            OutstandingPacket state{};

            state.packet =
                std::move(packet);

            state.sent_time =
                std::chrono::steady_clock::now();

            outstanding.emplace(
                next_seq,
                std::move(state)
            );

            ++next_seq;
            ++packets_sent;
        }


        // --------------------------------------------------------
        // Process all available ACKs
        // --------------------------------------------------------

        while (true)
        {
            PacketHeader ack{};

            ssize_t received =
                recvfrom(
                    sockfd,
                    &ack,
                    sizeof(ack),
                    MSG_DONTWAIT,
                    nullptr,
                    nullptr
                );

            if (received <
                static_cast<ssize_t>(
                    sizeof(PacketHeader)))
            {
                break;
            }

            if (ack.protocol_id !=
                    PROTOCOL_ID ||
                ack.type !=
                    static_cast<uint8_t>(
                        PacketType::ACK
                    ))
            {
                continue;
            }

            auto it =
                outstanding.find(
                    ack.seq
                );

            if (it !=
                outstanding.end())
            {
                outstanding.erase(it);
            }
        }


        // --------------------------------------------------------
        // Retransmit timed-out packets
        // --------------------------------------------------------

        auto now =
            std::chrono::steady_clock::now();

        for (auto& entry :
             outstanding)
        {
            OutstandingPacket& state =
                entry.second;

            auto elapsed_ms =
                std::chrono::
                    duration_cast<
                        std::chrono::milliseconds
                    >(
                        now -
                        state.sent_time
                    ).count();

            if (elapsed_ms >=
                DEFAULT_RTO_MS)
            {
                sendto(
                    sockfd,
                    state.packet.data(),
                    state.packet.size(),
                    0,
                    reinterpret_cast<
                        sockaddr*
                    >(
                        &receiver_addr
                    ),
                    sizeof(receiver_addr)
                );


                // ------------------------------------------------
                // Pace retransmission as well.
                // ------------------------------------------------

                pace_packet(
                    next_send_time,
                    state.packet.size(),
                    pacing_rate_mbps
                );


                state.sent_time =
                    std::chrono::
                        steady_clock::now();

                ++retransmissions;
                ++packets_sent;
            }
        }


        std::this_thread::sleep_for(
            std::chrono::
                microseconds(100)
        );
    }


    // ------------------------------------------------------------
    // 7. FIN handshake
    // ------------------------------------------------------------

    PacketHeader fin{};

    fin.protocol_id =
        PROTOCOL_ID;

    fin.seq =
        total_packets;

    fin.length = 0;

    fin.type =
        static_cast<uint8_t>(
            PacketType::FIN
        );

    fin.reserved = 0;


    std::cout
        << "All DATA packets acknowledged.\n";


    while (true)
    {
        sendto(
            sockfd,
            &fin,
            sizeof(fin),
            0,
            reinterpret_cast<sockaddr*>(
                &receiver_addr
            ),
            sizeof(receiver_addr)
        );

        PacketHeader response{};

        if (wait_for_packet_type(
                sockfd,
                PacketType::FIN_ACK,
                DEFAULT_RTO_MS,
                response))
        {
            break;
        }

        std::cout
            << "FIN timeout, "
            << "retransmitting...\n";
    }


    auto transfer_end =
        std::chrono::steady_clock::now();


    // ------------------------------------------------------------
    // 8. Calculate statistics
    // ------------------------------------------------------------

    double elapsed_seconds =
        std::chrono::duration<double>(
            transfer_end -
            transfer_start
        ).count();

    double throughput_mbps =
        (static_cast<double>(
            file_size
         ) * 8.0)
        /
        elapsed_seconds
        /
        1'000'000.0;


    std::cout
        << "\nTransfer complete.\n";

    std::cout
        << "Time: "
        << elapsed_seconds
        << " seconds\n";

    std::cout
        << "Throughput: "
        << throughput_mbps
        << " Mbit/s\n";

    std::cout
        << "Total UDP transmissions: "
        << packets_sent
        << "\n";

    std::cout
        << "Retransmissions: "
        << retransmissions
        << "\n";


    input.close();
    close(sockfd);

    return 0;
}