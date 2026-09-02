#define _POSIX_C_SOURCE 200809L
#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static void usage(const char *p) {
    fprintf(stderr,
            "Usage: %s <port> <output_file>\n"
            "Example: %s 5001 received.bin\n", p, p);
}

static int same_peer(const struct sockaddr_in *a, const struct sockaddr_in *b) {
    return a->sin_family == b->sin_family && a->sin_port == b->sin_port &&
           a->sin_addr.s_addr == b->sin_addr.s_addr;
}

static void send_ack(int fd, const struct sockaddr_in *peer, uint64_t session,
                     uint32_t ack_seq, uint32_t recv_base) {
    frft_ack_t ack = {htonl(ack_seq), htonl(recv_base)};
    (void)frft_send_packet(fd, peer, FRFT_TYPE_ACK, session, ack_seq,
                           &ack, sizeof(ack), 0);
}

static void send_nack(int fd, const struct sockaddr_in *peer, uint64_t session,
                      uint32_t missing_seq, uint32_t recv_base) {
    frft_nack_t nack = {htonl(missing_seq), htonl(recv_base)};
    (void)frft_send_packet(fd, peer, FRFT_TYPE_NACK, session, missing_seq,
                           &nack, sizeof(nack), 0);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        usage(argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);
    const char *output_path = argv[2];
    if (port <= 1024 || port > 65535) {
        fprintf(stderr, "Invalid port.\n");
        return 1;
    }

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        perror("socket");
        return 1;
    }
    (void)frft_set_socket_buffers(s, 2 * 1024 * 1024);

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons((uint16_t)port);
    if (bind(s, (struct sockaddr *)&local, sizeof(local)) < 0) {
        perror("bind");
        return 1;
    }

    printf("FRFT receiver listening on UDP port %d\n", port);
    printf("Output file: %s\n", output_path);

    unsigned char buf[FRFT_MAX_DGRAM];
    struct sockaddr_in peer;
    memset(&peer, 0, sizeof(peer));
    int have_session = 0;
    uint64_t session = 0;
    uint64_t file_size = 0;
    uint32_t payload_size = 0;
    uint32_t packet_count = 0;
    unsigned char *seen = NULL;
    uint32_t recv_base = 0;
    uint32_t max_seq_seen = 0;
    uint64_t unique_packets = 0;
    uint64_t unique_bytes = 0;
    uint64_t first_data_real_ns = 0;
    uint64_t end_real_ns = 0;
    uint64_t last_nack_ns = 0;
    int outfd = -1;

    for (;;) {
        struct sockaddr_in from;
        frft_wire_hdr_t h;
        unsigned char *payload;
        int status = frft_recv_packet(s, buf, sizeof(buf), &from, &h, &payload);
        if (status < 0) {
            if (errno == EINTR) continue;
            perror("recvfrom");
            return 1;
        }
        if (status == 0) continue;

        if (h.type == FRFT_TYPE_META) {
            if (h.payload_len != sizeof(frft_meta_t)) continue;
            frft_meta_t m;
            memcpy(&m, payload, sizeof(m));
            uint64_t fs = frft_ntohll(m.file_size);
            uint32_t ps = ntohl(m.payload_size);
            uint32_t pc = ntohl(m.packet_count);
            if (fs == 0 || ps < 256 || ps > FRFT_MAX_DGRAM - sizeof(frft_wire_hdr_t) || pc == 0) continue;
            if ((fs + ps - 1) / ps != pc) continue;

            if (!have_session) {
                session = h.session_id;
                peer = from;
                file_size = fs;
                payload_size = ps;
                packet_count = pc;
                seen = calloc(packet_count, 1);
                if (!seen) {
                    fprintf(stderr, "Out of memory.\n");
                    return 1;
                }

                outfd = open(output_path, O_CREAT | O_TRUNC | O_WRONLY, 0666);
                if (outfd < 0) {
                    perror("open output");
                    return 1;
                }
                if (ftruncate(outfd, (off_t)file_size) < 0) {
                    perror("ftruncate");
                    return 1;
                }
                have_session = 1;
                printf("Accepted session %llu from %s:%u\n",
                       (unsigned long long)session, inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
                printf("  bytes          : %llu\n", (unsigned long long)file_size);
                printf("  payload/packet : %u bytes\n", payload_size);
                printf("  packets        : %u\n", packet_count);
            }

            if (h.session_id == session && same_peer(&from, &peer)) {
                (void)frft_send_packet(s, &peer, FRFT_TYPE_META_ACK,
                                       session, 0, NULL, 0, 0);
            }
            continue;
        }

        if (!have_session || h.session_id != session || !same_peer(&from, &peer)) continue;

        if (h.type == FRFT_TYPE_DATA) {
            if (h.seq >= packet_count || h.payload_len == 0 || h.payload_len > payload_size) continue;
            uint64_t off = (uint64_t)h.seq * payload_size;
            uint32_t expected = payload_size;
            if (off + expected > file_size) expected = (uint32_t)(file_size - off);
            if (h.payload_len != expected) continue;
            if (frft_crc32(payload, h.payload_len) != h.crc32) {
                /* Corrupt application payload: request retransmission. */
                send_nack(s, &peer, session, h.seq, recv_base);
                continue;
            }

            if (first_data_real_ns == 0) {
                first_data_real_ns = frft_now_realtime_ns();
                printf("FIRST_DATA_RECEIVED_REALTIME_NS=%llu\n",
                       (unsigned long long)first_data_real_ns);
            }

            if (!seen[h.seq]) {
                ssize_t w = pwrite(outfd, payload, h.payload_len, (off_t)off);
                if (w != (ssize_t)h.payload_len) {
                    perror("pwrite");
                    return 1;
                }
                seen[h.seq] = 1;
                unique_packets++;
                unique_bytes += h.payload_len;
                if (h.seq > max_seq_seen) max_seq_seen = h.seq;

                while (recv_base < packet_count && seen[recv_base]) recv_base++;
            }

            send_ack(s, &peer, session, h.seq, recv_base);

            uint64_t now = frft_now_monotonic_ns();
            if (recv_base < packet_count && recv_base <= max_seq_seen && !seen[recv_base] &&
                now - last_nack_ns >= 5000000ULL) {
                send_nack(s, &peer, session, recv_base, recv_base);
                last_nack_ns = now;
            }

            if (unique_packets == packet_count) {
                end_real_ns = frft_now_realtime_ns(); /* last unique byte has arrived */
                printf("RECEIVER_END_REALTIME_NS=%llu\n", (unsigned long long)end_real_ns);
                printf("All %u packets received; fsync in progress...\n", packet_count);
                if (fsync(outfd) < 0) perror("fsync");

                frft_done_t d;
                d.receiver_end_realtime_ns = frft_htonll(end_real_ns);
                d.bytes_received = frft_htonll(unique_bytes);

                /* Keep replying for 3 seconds in case DONE is lost. */
                uint64_t grace_end = frft_now_monotonic_ns() + 3000000000ULL;
                uint64_t last_done = 0;
                while (frft_now_monotonic_ns() < grace_end) {
                    uint64_t t = frft_now_monotonic_ns();
                    if (last_done == 0 || t - last_done >= 100000000ULL) {
                        (void)frft_send_packet(s, &peer, FRFT_TYPE_DONE,
                                               session, 0, &d, sizeof(d), 0);
                        last_done = t;
                    }

                    struct timeval tv = {0, 50000};
                    fd_set rfds;
                    FD_ZERO(&rfds);
                    FD_SET(s, &rfds);
                    int rc = select(s + 1, &rfds, NULL, NULL, &tv);
                    if (rc > 0 && FD_ISSET(s, &rfds)) {
                        struct sockaddr_in f2;
                        frft_wire_hdr_t h2;
                        int status2 = frft_recv_packet(s, buf, sizeof(buf),
                                                       &f2, &h2, NULL);
                        if (status2 > 0 && h2.session_id == session &&
                            same_peer(&f2, &peer)) {
                            if (h2.type == FRFT_TYPE_DONE_ACK) {
                                printf("DONE acknowledged by sender.\n");
                                break;
                            }
                            (void)frft_send_packet(s, &peer, FRFT_TYPE_DONE,
                                                   session, 0, &d, sizeof(d), 0);
                        }
                    }
                }

                printf("Transfer complete: %llu bytes written to %s\n",
                       (unsigned long long)unique_bytes, output_path);
                printf("Run: md5sum %s\n", output_path);
                break;
            }
        }
    }

    if (outfd >= 0) close(outfd);
    free(seen);
    close(s);
    return 0;
}
