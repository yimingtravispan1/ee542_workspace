#include <iostream>
#include <fstream>
#include <vector>
#include <map>
#include <cstring>
#include <cstdlib>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "../include/protocol.h"

int main(int argc, char* argv[])
{
    if (argc != 3)
    {
        std::cerr << "Usage: " << argv[0]
                  << " <port> <output_file>\n";
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

    // ------------------------------------------------------------
    // 2. Bind socket to local UDP port
    // ------------------------------------------------------------

    sockaddr_in receiver_addr{};
    receiver_addr.sin_family = AF_INET;
    receiver_addr.sin_addr.s_addr = INADDR_ANY;
    receiver_addr.sin_port = htons(port);

    if (bind(sockfd,reinterpret_cast<sockaddr*>(&receiver_addr),sizeof(receiver_addr)) < 0)
    {
        perror("bind");
        close(sockfd);
        return 1;
    }

    std::cout << "Receiver listening on UDP port "
              << port << "...\n";

    // ------------------------------------------------------------
    // 3. Wait for META packet
    // ------------------------------------------------------------

    std::vector<char> packet_buffer(sizeof(PacketHeader) + MAX_PAYLOAD_SIZE);

    sockaddr_in sender_addr{};
    socklen_t sender_addr_len = sizeof(sender_addr);

    ssize_t received = recvfrom(sockfd,packet_buffer.data(),packet_buffer.size(), 0,reinterpret_cast<sockaddr*>(&sender_addr),&sender_addr_len);

    if (received < static_cast<ssize_t>(
                       sizeof(PacketHeader) + sizeof(MetaPayload)))
    {
        std::cerr << "Invalid META packet\n";
        close(sockfd);
        return 1;
    }

    PacketHeader meta_header{};

    std::memcpy(
        &meta_header,
        packet_buffer.data(),
        sizeof(PacketHeader)
    );

    if (meta_header.protocol_id != PROTOCOL_ID ||
        meta_header.type != static_cast<uint8_t>(PacketType::META))
    {
        std::cerr << "Expected META packet\n";
        close(sockfd);
        return 1;
    }

    MetaPayload meta{};

    std::memcpy(
        &meta,
        packet_buffer.data() + sizeof(PacketHeader),
        sizeof(MetaPayload)
    );

    std::cout << "Incoming file information:\n";
    std::cout << "  File size: "
              << meta.file_size << " bytes\n";
    std::cout << "  Payload size: "
              << meta.payload_size << " bytes\n";
    std::cout << "  Total packets: "
              << meta.total_packets << "\n";

    // ------------------------------------------------------------
    // 4. Open output file
    // ------------------------------------------------------------

    std::ofstream output(
        output_file,
        std::ios::binary
    );

    if (!output)
    {
        std::cerr << "Cannot open output file\n";
        close(sockfd);
        return 1;
    }

    // ------------------------------------------------------------
    // 5. Send META_ACK
    // ------------------------------------------------------------

    PacketHeader meta_ack{};

    meta_ack.protocol_id = PROTOCOL_ID;
    meta_ack.seq = 0;
    meta_ack.length = 0;
    meta_ack.type =
        static_cast<uint8_t>(PacketType::META_ACK);
    meta_ack.reserved = 0;

    sendto(
        sockfd,
        &meta_ack,
        sizeof(meta_ack),
        0,
        reinterpret_cast<sockaddr*>(&sender_addr),
        sender_addr_len
    );

    // ------------------------------------------------------------
    // 6. Receive DATA packets
    // ------------------------------------------------------------

    uint32_t expected_seq = 0;

    std::map<uint32_t, std::vector<char>>
        out_of_order_buffer;

    uint64_t bytes_written = 0;

    bool transfer_finished = false;

    while (!transfer_finished)
    {
        received = recvfrom(
            sockfd,
            packet_buffer.data(),
            packet_buffer.size(),
            0,
            reinterpret_cast<sockaddr*>(&sender_addr),
            &sender_addr_len
        );

        if (received < static_cast<ssize_t>(
                           sizeof(PacketHeader)))
        {
            continue;
        }

        PacketHeader header{};

        std::memcpy(
            &header,
            packet_buffer.data(),
            sizeof(PacketHeader)
        );

        if (header.protocol_id != PROTOCOL_ID)
        {
            continue;
        }

        PacketType type =
            static_cast<PacketType>(header.type);

        // --------------------------------------------------------
        // DATA packet
        // --------------------------------------------------------

        if (type == PacketType::DATA)
        {
            if (header.length > meta.payload_size ||
                header.length > MAX_PAYLOAD_SIZE)
            {
                continue;
            }

            if (received <
                static_cast<ssize_t>(
                    sizeof(PacketHeader) +
                    header.length))
            {
                continue;
            }

            // If this packet has not already been written,
            // save it in the out-of-order buffer.
            if (header.seq >= expected_seq &&
                out_of_order_buffer.find(header.seq) ==
                    out_of_order_buffer.end())
            {
                std::vector<char> data(header.length);

                std::memcpy(
                    data.data(),
                    packet_buffer.data() +
                        sizeof(PacketHeader),
                    header.length
                );

                out_of_order_buffer.emplace(
                    header.seq,
                    std::move(data)
                );
            }

            // ----------------------------------------------------
            // Send ACK for this DATA packet
            // ----------------------------------------------------

            PacketHeader ack{};

            ack.protocol_id = PROTOCOL_ID;
            ack.seq = header.seq;
            ack.length = 0;
            ack.type =
                static_cast<uint8_t>(PacketType::ACK);
            ack.reserved = 0;

            sendto(
                sockfd,
                &ack,
                sizeof(ack),
                0,
                reinterpret_cast<sockaddr*>(&sender_addr),
                sender_addr_len
            );

            // ----------------------------------------------------
            // Write all consecutive packets to disk
            // ----------------------------------------------------

            while (true)
            {
                auto it =
                    out_of_order_buffer.find(expected_seq);

                if (it ==
                    out_of_order_buffer.end())
                {
                    break;
                }

                output.write(
                    it->second.data(),
                    it->second.size()
                );

                bytes_written +=
                    it->second.size();

                out_of_order_buffer.erase(it);

                ++expected_seq;
            }
        }

        // --------------------------------------------------------
        // FIN packet
        // --------------------------------------------------------

        else if (type == PacketType::FIN)
        {
            if (expected_seq ==
                meta.total_packets)
            {
                PacketHeader fin_ack{};

                fin_ack.protocol_id =
                    PROTOCOL_ID;
                fin_ack.seq =
                    expected_seq;
                fin_ack.length = 0;
                fin_ack.type =
                    static_cast<uint8_t>(
                        PacketType::FIN_ACK
                    );
                fin_ack.reserved = 0;

                sendto(
                    sockfd,
                    &fin_ack,
                    sizeof(fin_ack),
                    0,
                    reinterpret_cast<sockaddr*>(
                        &sender_addr
                    ),
                    sender_addr_len
                );

                transfer_finished = true;
            }
        }
    }

    // ------------------------------------------------------------
    // 7. Cleanup
    // ------------------------------------------------------------

    output.close();
    close(sockfd);

    std::cout << "Transfer complete.\n";
    std::cout << "Bytes written: "
              << bytes_written << "\n";
    std::cout << "Saved to: "
              << output_file << "\n";

    return 0;
}