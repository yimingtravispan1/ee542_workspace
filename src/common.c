#define _POSIX_C_SOURCE 200809L
#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

uint64_t frft_now_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

uint64_t frft_now_realtime_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

void frft_sleep_until_ns(uint64_t target_ns) {
    struct timespec ts;
    ts.tv_sec = (time_t)(target_ns / 1000000000ULL);
    ts.tv_nsec = (long)(target_ns % 1000000000ULL);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {
    }
}

static uint32_t crc_table[256];
static int crc_ready = 0;

static void crc_init(void) {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int j = 0; j < 8; ++j) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        crc_table[i] = c;
    }
    crc_ready = 1;
}

uint32_t frft_crc32(const void *data, size_t len) {
    if (!crc_ready) crc_init();
    const unsigned char *p = (const unsigned char *)data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        c = crc_table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

uint64_t frft_random_u64(void) {
    uint64_t v = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, &v, sizeof(v));
        close(fd);
        if (n == (ssize_t)sizeof(v)) return v;
    }
    v = frft_now_realtime_ns() ^ ((uint64_t)getpid() << 32);
    v ^= (uintptr_t)&v;
    return v;
}

int frft_set_socket_buffers(int fd, int bytes) {
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes)) < 0) return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes)) < 0) return -1;
    return 0;
}

static ssize_t send_packet_impl(int fd, const struct sockaddr_in *peer,
                                uint8_t type, uint64_t session, uint32_t seq,
                                const void *payload, uint32_t payload_len,
                                uint32_t crc32, uint64_t *start_realtime_ns,
                                uint64_t *start_monotonic_ns) {
    unsigned char buf[FRFT_MAX_DGRAM];
    size_t packet_len = sizeof(frft_wire_hdr_t) + payload_len;

    if (packet_len > sizeof(buf)) {
        errno = EMSGSIZE;
        return -1;
    }

    frft_wire_hdr_t *header = (frft_wire_hdr_t *)buf;
    frft_encode_hdr(header, type, session, seq, payload_len, crc32);
    if (payload_len) memcpy(buf + sizeof(*header), payload, payload_len);

    int capture_start = start_realtime_ns && start_monotonic_ns;
    uint64_t realtime_ns = 0;
    uint64_t monotonic_ns = 0;
    if (capture_start) {
        realtime_ns = frft_now_realtime_ns();
        monotonic_ns = frft_now_monotonic_ns();
    }

    ssize_t sent = sendto(fd, buf, packet_len, 0,
                          (const struct sockaddr *)peer, sizeof(*peer));
    if (sent >= 0 && capture_start) {
        *start_realtime_ns = realtime_ns;
        *start_monotonic_ns = monotonic_ns;
    }
    return sent;
}

ssize_t frft_send_packet(int fd, const struct sockaddr_in *peer,
                         uint8_t type, uint64_t session, uint32_t seq,
                         const void *payload, uint32_t payload_len,
                         uint32_t crc32) {
    return send_packet_impl(fd, peer, type, session, seq, payload, payload_len,
                            crc32, NULL, NULL);
}

ssize_t frft_send_packet_timed(int fd, const struct sockaddr_in *peer,
                               uint8_t type, uint64_t session, uint32_t seq,
                               const void *payload, uint32_t payload_len,
                               uint32_t crc32, uint64_t *start_realtime_ns,
                               uint64_t *start_monotonic_ns) {
    return send_packet_impl(fd, peer, type, session, seq, payload, payload_len,
                            crc32, start_realtime_ns, start_monotonic_ns);
}

int frft_recv_packet(int fd, unsigned char *buf, size_t buf_size,
                     struct sockaddr_in *from, frft_wire_hdr_t *header,
                     unsigned char **payload) {
    socklen_t from_len = sizeof(*from);
    ssize_t received = recvfrom(fd, buf, buf_size, 0,
                                (struct sockaddr *)from, &from_len);
    if (received < 0) return -1;
    if ((size_t)received < sizeof(frft_wire_hdr_t)) return 0;
    if (frft_decode_hdr((const frft_wire_hdr_t *)buf, header) < 0) return 0;
    if ((size_t)received < sizeof(frft_wire_hdr_t) + header->payload_len) return 0;

    if (payload) *payload = buf + sizeof(frft_wire_hdr_t);
    return 1;
}

void frft_encode_hdr(frft_wire_hdr_t *h, uint8_t type, uint64_t session,
                     uint32_t seq, uint32_t payload_len, uint32_t crc32) {
    memset(h, 0, sizeof(*h));
    h->magic = htonl(FRFT_MAGIC);
    h->version = FRFT_VERSION;
    h->type = type;
    h->header_len = htons((uint16_t)sizeof(*h));
    h->session_id = frft_htonll(session);
    h->seq = htonl(seq);
    h->payload_len = htonl(payload_len);
    h->crc32 = htonl(crc32);
}

int frft_decode_hdr(const frft_wire_hdr_t *wire, frft_wire_hdr_t *host) {
    host->magic = ntohl(wire->magic);
    host->version = wire->version;
    host->type = wire->type;
    host->header_len = ntohs(wire->header_len);
    host->session_id = frft_ntohll(wire->session_id);
    host->seq = ntohl(wire->seq);
    host->payload_len = ntohl(wire->payload_len);
    host->crc32 = ntohl(wire->crc32);
    host->reserved = ntohl(wire->reserved);

    if (host->magic != FRFT_MAGIC || host->version != FRFT_VERSION ||
        host->header_len != sizeof(frft_wire_hdr_t)) {
        return -1;
    }
    if (host->payload_len > FRFT_MAX_DGRAM - sizeof(frft_wire_hdr_t)) return -1;
    return 0;
}
