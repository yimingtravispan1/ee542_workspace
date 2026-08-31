#pragma once

#include <cstdint>
#include <cstddef>

// Protocol identifier used to recognize packets from our protocol.
constexpr uint32_t PROTOCOL_ID = 0x45453534;

// Maximum payload supported by the protocol.
constexpr std::size_t MAX_PAYLOAD_SIZE = 8900;

// Recommended payload sizes for different MTU settings.
constexpr std::size_t PAYLOAD_MTU_1500 = 1400;
constexpr std::size_t PAYLOAD_MTU_9001 = 8900;

// Default sliding-window size.
constexpr std::size_t DEFAULT_WINDOW_SIZE = 4096;

// Default retransmission timeout in milliseconds.
constexpr int DEFAULT_RTO_MS = 300;

enum class PacketType : uint8_t
{
    META = 1,
    META_ACK = 2,
    DATA = 3,
    ACK = 4,
    FIN = 5,
    FIN_ACK = 6
};

#pragma pack(push, 1)

struct PacketHeader
{
    uint32_t protocol_id;
    uint32_t seq;
    uint16_t length;
    uint8_t type;
    uint8_t reserved;
};

struct MetaPayload
{
    uint64_t file_size;
    uint32_t payload_size;
    uint32_t total_packets;
};

#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 12,
              "PacketHeader must be exactly 12 bytes");

static_assert(sizeof(MetaPayload) == 16,
              "MetaPayload must be exactly 16 bytes");