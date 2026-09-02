#ifndef FRFT_PROTOCOL_H
#define FRFT_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <arpa/inet.h>

#define FRFT_MAGIC 0x46524654u /* 'FRFT' */
#define FRFT_VERSION 1u
#define FRFT_MAX_DGRAM 9200

#define FRFT_TYPE_META      1u
#define FRFT_TYPE_META_ACK  2u
#define FRFT_TYPE_DATA      3u
#define FRFT_TYPE_ACK       4u
#define FRFT_TYPE_NACK      5u
#define FRFT_TYPE_DONE      6u
#define FRFT_TYPE_DONE_ACK  7u

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  type;
    uint16_t header_len;
    uint64_t session_id;
    uint32_t seq;
    uint32_t payload_len;
    uint32_t crc32;
    uint32_t reserved;
} frft_wire_hdr_t;

typedef struct {
    uint64_t file_size;
    uint32_t payload_size;
    uint32_t packet_count;
} frft_meta_t;

typedef struct {
    uint32_t ack_seq;
    uint32_t recv_base;
} frft_ack_t;

typedef struct {
    uint32_t missing_seq;
    uint32_t recv_base;
} frft_nack_t;

typedef struct {
    uint64_t receiver_end_realtime_ns;
    uint64_t bytes_received;
} frft_done_t;
#pragma pack(pop)

static inline uint64_t frft_htonll(uint64_t x) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return ((uint64_t)htonl((uint32_t)(x & 0xffffffffULL)) << 32) |
           htonl((uint32_t)(x >> 32));
#else
    return x;
#endif
}

static inline uint64_t frft_ntohll(uint64_t x) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return ((uint64_t)ntohl((uint32_t)(x & 0xffffffffULL)) << 32) |
           ntohl((uint32_t)(x >> 32));
#else
    return x;
#endif
}

uint64_t frft_now_monotonic_ns(void);
uint64_t frft_now_realtime_ns(void);
void frft_sleep_until_ns(uint64_t target_ns);
uint32_t frft_crc32(const void *data, size_t len);
uint64_t frft_random_u64(void);
int frft_set_socket_buffers(int fd, int bytes);

ssize_t frft_send_packet(int fd, const struct sockaddr_in *peer,
                         uint8_t type, uint64_t session, uint32_t seq,
                         const void *payload, uint32_t payload_len,
                         uint32_t crc32);
ssize_t frft_send_packet_timed(int fd, const struct sockaddr_in *peer,
                               uint8_t type, uint64_t session, uint32_t seq,
                               const void *payload, uint32_t payload_len,
                               uint32_t crc32, uint64_t *start_realtime_ns,
                               uint64_t *start_monotonic_ns);
/* Returns 1 for a valid packet, 0 for a malformed packet, and -1 on I/O error.
 * payload may be NULL when the caller only needs the decoded header. */
int frft_recv_packet(int fd, unsigned char *buf, size_t buf_size,
                     struct sockaddr_in *from, frft_wire_hdr_t *header,
                     unsigned char **payload);

void frft_encode_hdr(frft_wire_hdr_t *h, uint8_t type, uint64_t session,
                     uint32_t seq, uint32_t payload_len, uint32_t crc32);
int frft_decode_hdr(const frft_wire_hdr_t *wire, frft_wire_hdr_t *host);

#endif
