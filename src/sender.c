#define _POSIX_C_SOURCE 200809L
#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_MTU 1500
#define DEFAULT_WINDOW 4096u
#define DEFAULT_RATE_MBPS 90.0
#define AUTO_PACE_BURST_BYTES 8192u
#define MAX_PACE_BATCH 64u
#define PACE_SPIN_GUARD_NS 120000ULL
#define MIN_RTO_MS 20.0
#define MAX_RTO_MS 1500.0
#define META_RETRY_NS 200000000ULL
#define TIMEOUT_SCAN_NS 2000000ULL

typedef struct {
    int mtu;
    uint32_t window;
    uint32_t pace_batch;
    double rate_mbps;
} options_t;

typedef struct {
    uint64_t next_batch_ns;
    uint32_t packets_in_batch;
    uint32_t batch_packets;
} frft_pacer_t;

typedef struct {
    int fd;
    struct sockaddr_in peer;
    uint64_t session;
    const unsigned char *map;
    uint64_t file_size;
    uint32_t payload_size;
    uint32_t packet_count;
    double rate_mbps;
    frft_pacer_t pacer;
    unsigned char *acked;
    uint64_t *sent_ns;
    unsigned char *retries;
    double srtt_ms;
    double rttvar_ms;
    double rto_ms;
    uint64_t total_tx_packets;
    uint64_t total_retx;
    uint64_t start_real_ns;
    uint64_t start_mono_ns;
} sender_t;

static void usage(const char *program) {
    fprintf(stderr,
            "Usage: %s <receiver_ip> <port> <input_file> "
            "[--mtu 1500|9000|9001] [--rate Mbps] "
            "[--window packets] [--batch packets]\n"
            "Example: %s 192.168.10.100 5001 data.bin --mtu 1500 "
            "--rate 95 --window 4096 --batch 5\n",
            program, program);
}

static int parse_long(const char *text, long min, long max, long *value) {
    char *end = NULL;
    errno = 0;
    long parsed = strtol(text, &end, 10);
    if (errno || !end || *end != '\0' || parsed < min || parsed > max) return -1;
    *value = parsed;
    return 0;
}

static int parse_u32(const char *text, uint32_t min, uint32_t max,
                     uint32_t *value) {
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 10);
    if (errno || !end || *end != '\0' || parsed < min || parsed > max) return -1;
    *value = (uint32_t)parsed;
    return 0;
}

static int parse_rate(const char *text, double *value) {
    char *end = NULL;
    errno = 0;
    double parsed = strtod(text, &end);
    if (errno || !end || *end != '\0' || !isfinite(parsed) || parsed < 0.0) {
        return -1;
    }
    *value = parsed;
    return 0;
}

static int parse_options(int argc, char **argv, int *port, options_t *options) {
    long parsed_port;
    if (parse_long(argv[2], 1025, 65535, &parsed_port) < 0) return -1;
    *port = (int)parsed_port;

    *options = (options_t){DEFAULT_MTU, DEFAULT_WINDOW, 0, DEFAULT_RATE_MBPS};
    for (int i = 4; i < argc; ++i) {
        if (i + 1 >= argc) return -1;

        if (strcmp(argv[i], "--mtu") == 0) {
            long mtu;
            if (parse_long(argv[++i], 576, 9001, &mtu) < 0) return -1;
            options->mtu = (int)mtu;
        } else if (strcmp(argv[i], "--rate") == 0) {
            if (parse_rate(argv[++i], &options->rate_mbps) < 0) return -1;
        } else if (strcmp(argv[i], "--window") == 0) {
            if (parse_u32(argv[++i], 32, UINT32_MAX, &options->window) < 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--batch") == 0) {
            if (parse_u32(argv[++i], 0, MAX_PACE_BATCH,
                          &options->pace_batch) < 0) {
                return -1;
            }
        } else {
            return -1;
        }
    }

    if (options->pace_batch == 0) {
        options->pace_batch = AUTO_PACE_BURST_BYTES / (uint32_t)options->mtu;
        if (options->pace_batch == 0) options->pace_batch = 1;
        if (options->pace_batch > MAX_PACE_BATCH) {
            options->pace_batch = MAX_PACE_BATCH;
        }
    }
    return 0;
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int same_peer(const struct sockaddr_in *a, const struct sockaddr_in *b) {
    return a->sin_family == b->sin_family && a->sin_port == b->sin_port &&
           a->sin_addr.s_addr == b->sin_addr.s_addr;
}

/* Sleep most of the interval, then briefly spin to reduce VM scheduler overshoot. */
static void precise_wait_until_ns(uint64_t target_ns) {
    uint64_t now = frft_now_monotonic_ns();
    if (target_ns <= now) return;

    if (target_ns - now > PACE_SPIN_GUARD_NS) {
        frft_sleep_until_ns(target_ns - PACE_SPIN_GUARD_NS);
    }
    while (frft_now_monotonic_ns() < target_ns) {
        /* Intentional short busy spin. */
    }
}

/* Pace small packet batches instead of sleeping after every packet. */
static void pace_before_send(frft_pacer_t *pacer, double rate_mbps,
                             size_t wire_bytes) {
    if (rate_mbps <= 0.0 || pacer->batch_packets == 0) return;

    uint64_t now = frft_now_monotonic_ns();
    if (pacer->next_batch_ns == 0) pacer->next_batch_ns = now;

    if (pacer->packets_in_batch >= pacer->batch_packets) {
        if (pacer->next_batch_ns > now) {
            precise_wait_until_ns(pacer->next_batch_ns);
            now = frft_now_monotonic_ns();
        }
        /* Do not generate a large catch-up burst after a scheduling delay. */
        if (pacer->next_batch_ns < now) pacer->next_batch_ns = now;
        pacer->packets_in_batch = 0;
    }

    double seconds = ((double)wire_bytes * 8.0) / (rate_mbps * 1000000.0);
    uint64_t delta = (uint64_t)(seconds * 1e9);
    if (delta < 1000) delta = 1000;
    pacer->next_batch_ns += delta;
    pacer->packets_in_batch++;
}

static int transmit_data(sender_t *sender, uint32_t seq, int retransmission) {
    uint64_t offset = (uint64_t)seq * sender->payload_size;
    if (seq >= sender->packet_count || offset >= sender->file_size) {
        errno = EINVAL;
        return -1;
    }

    uint32_t length = sender->payload_size;
    if (offset + length > sender->file_size) {
        length = (uint32_t)(sender->file_size - offset);
    }

    uint32_t crc = frft_crc32(sender->map + offset, length);
    pace_before_send(&sender->pacer, sender->rate_mbps,
                     sizeof(frft_wire_hdr_t) + length + 28u);

    int first_data = sender->start_real_ns == 0;
    ssize_t sent = first_data
        ? frft_send_packet_timed(sender->fd, &sender->peer, FRFT_TYPE_DATA,
                                 sender->session, seq, sender->map + offset,
                                 length, crc, &sender->start_real_ns,
                                 &sender->start_mono_ns)
        : frft_send_packet(sender->fd, &sender->peer, FRFT_TYPE_DATA,
                           sender->session, seq, sender->map + offset,
                           length, crc);
    if (sent < 0) {
        return -1;
    }

    if (first_data) {
        printf("START_REALTIME_NS=%llu\n",
               (unsigned long long)sender->start_real_ns);
    }
    sender->sent_ns[seq] = frft_now_monotonic_ns();
    sender->total_tx_packets++;
    if (retransmission) {
        if (sender->retries[seq] < UINT8_MAX) sender->retries[seq]++;
        sender->total_retx++;
    }
    return 0;
}

static int perform_meta_handshake(sender_t *sender, const frft_meta_t *meta,
                                  unsigned char *buffer) {
    uint64_t last_meta_ns = 0;

    for (int tries = 0; tries < 100;) {
        uint64_t now = frft_now_monotonic_ns();
        if (last_meta_ns == 0 || now - last_meta_ns >= META_RETRY_NS) {
            if (frft_send_packet(sender->fd, &sender->peer, FRFT_TYPE_META,
                                 sender->session, 0, meta, sizeof(*meta), 0) < 0 &&
                errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("send META");
            }
            last_meta_ns = now;
            tries++;
        }

        for (;;) {
            struct sockaddr_in from;
            frft_wire_hdr_t header;
            int status = frft_recv_packet(sender->fd, buffer, FRFT_MAX_DGRAM,
                                          &from, &header, NULL);
            if (status < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                perror("recvfrom");
                return -1;
            }
            if (status == 0) continue;
            if (same_peer(&from, &sender->peer) &&
                header.session_id == sender->session &&
                header.type == FRFT_TYPE_META_ACK) {
                return 0;
            }
        }

        struct timespec delay = {0, 1000000};
        nanosleep(&delay, NULL);
    }

    fprintf(stderr, "Receiver did not acknowledge META.\n");
    return -1;
}

static void update_rto(sender_t *sender, uint32_t ack_seq, uint64_t now) {
    if (!sender->sent_ns[ack_seq] || sender->retries[ack_seq] != 0) return;

    double sample = (double)(now - sender->sent_ns[ack_seq]) / 1e6;
    if (sender->srtt_ms == 0.0) {
        sender->srtt_ms = sample;
        sender->rttvar_ms = sample / 2.0;
    } else {
        double error = fabs(sender->srtt_ms - sample);
        sender->rttvar_ms = 0.75 * sender->rttvar_ms + 0.25 * error;
        sender->srtt_ms = 0.875 * sender->srtt_ms + 0.125 * sample;
    }

    sender->rto_ms = sender->srtt_ms + 4.0 * sender->rttvar_ms;
    if (sender->rto_ms < MIN_RTO_MS) sender->rto_ms = MIN_RTO_MS;
    if (sender->rto_ms > MAX_RTO_MS) sender->rto_ms = MAX_RTO_MS;
}

static void handle_ack(sender_t *sender, const unsigned char *payload,
                       uint32_t *base, uint32_t next_seq) {
    frft_ack_t ack;
    memcpy(&ack, payload, sizeof(ack));
    uint32_t ack_seq = ntohl(ack.ack_seq);
    uint32_t recv_base = ntohl(ack.recv_base);

    if (ack_seq < sender->packet_count && !sender->acked[ack_seq]) {
        update_rto(sender, ack_seq, frft_now_monotonic_ns());
        sender->acked[ack_seq] = 1;
    }

    if (recv_base > sender->packet_count) recv_base = sender->packet_count;
    while (*base < recv_base) sender->acked[(*base)++] = 1;
    while (*base < next_seq && sender->acked[*base]) (*base)++;
}

static void handle_nack(sender_t *sender, const unsigned char *payload,
                        uint32_t next_seq) {
    frft_nack_t nack;
    memcpy(&nack, payload, sizeof(nack));
    uint32_t missing = ntohl(nack.missing_seq);

    if (missing >= next_seq || missing >= sender->packet_count ||
        sender->acked[missing]) {
        return;
    }
    if (transmit_data(sender, missing, 1) < 0 &&
        errno != EAGAIN && errno != EWOULDBLOCK) {
        perror("retransmit DATA");
    }
}

static int receive_feedback(sender_t *sender, unsigned char *buffer,
                            uint32_t *base, uint32_t next_seq, int *got_done,
                            uint64_t *receiver_end_ns, uint64_t *bytes_received) {
    for (;;) {
        struct sockaddr_in from;
        frft_wire_hdr_t header;
        unsigned char *payload;
        int status = frft_recv_packet(sender->fd, buffer, FRFT_MAX_DGRAM,
                                      &from, &header, &payload);
        if (status < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            perror("recvfrom");
            return -1;
        }
        if (status == 0 || !same_peer(&from, &sender->peer) ||
            header.session_id != sender->session) {
            continue;
        }

        if (header.type == FRFT_TYPE_ACK &&
            header.payload_len == sizeof(frft_ack_t)) {
            handle_ack(sender, payload, base, next_seq);
        } else if (header.type == FRFT_TYPE_NACK &&
                   header.payload_len == sizeof(frft_nack_t)) {
            handle_nack(sender, payload, next_seq);
        } else if (header.type == FRFT_TYPE_DONE &&
                   header.payload_len == sizeof(frft_done_t)) {
            frft_done_t done;
            memcpy(&done, payload, sizeof(done));
            *receiver_end_ns = frft_ntohll(done.receiver_end_realtime_ns);
            *bytes_received = frft_ntohll(done.bytes_received);
            *got_done = 1;
            (void)frft_send_packet(sender->fd, &sender->peer,
                                   FRFT_TYPE_DONE_ACK, sender->session,
                                   0, NULL, 0, 0);
            return 0;
        }
    }
}

static void retransmit_timeouts(sender_t *sender, uint32_t base,
                                uint32_t next_seq, uint64_t now) {
    uint64_t rto_ns = (uint64_t)(sender->rto_ms * 1e6);
    for (uint32_t seq = base; seq < next_seq; ++seq) {
        if (sender->acked[seq] || !sender->sent_ns[seq] ||
            now - sender->sent_ns[seq] < rto_ns) {
            continue;
        }
        if (transmit_data(sender, seq, 1) < 0 &&
            errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("retransmit DATA");
        }
    }
}

static void print_configuration(const char *input_path, const options_t *options,
                                const sender_t *sender) {
    printf("FRFT sender\n"
           "  file           : %s\n"
           "  bytes          : %llu\n"
           "  MTU            : %d\n"
           "  payload/packet : %u bytes\n"
           "  packets        : %u\n"
           "  window         : %u packets\n"
           "  pacing rate    : %.2f Mbps\n"
           "  pacing batch   : %u packets\n"
           "  session        : %llu\n",
           input_path, (unsigned long long)sender->file_size, options->mtu,
           sender->payload_size, sender->packet_count, options->window,
           options->rate_mbps, options->pace_batch,
           (unsigned long long)sender->session);
}

static void print_results(const sender_t *sender, uint64_t receiver_end_ns,
                          uint64_t bytes_received) {
    uint64_t done_mono_ns = frft_now_monotonic_ns();
    double sender_elapsed = sender->start_mono_ns
        ? (double)(done_mono_ns - sender->start_mono_ns) / 1e9 : 0.0;
    double one_way = receiver_end_ns > sender->start_real_ns &&
                     sender->start_real_ns
        ? (double)(receiver_end_ns - sender->start_real_ns) / 1e9 : 0.0;
    double goodput = one_way > 0.0
        ? ((double)sender->file_size * 8.0 / 1e6) / one_way : 0.0;

    printf("\nTransfer complete.\n"
           "RECEIVER_END_REALTIME_NS=%llu\n"
           "RECEIVER_BYTES=%llu\n"
           "ONE_WAY_SECONDS=%.6f  (requires synchronized VM clocks)\n"
           "ONE_WAY_GOODPUT_MBPS=%.3f\n"
           "SENDER_TO_DONE_SECONDS=%.6f  (includes final DONE return path)\n"
           "TOTAL_DATA_TX=%llu\n"
           "RETRANSMISSIONS=%llu\n"
           "FINAL_SRTT_MS=%.3f\n"
           "FINAL_RTO_MS=%.3f\n"
           "Run md5sum on the original and received files and compare the hashes.\n",
           (unsigned long long)receiver_end_ns,
           (unsigned long long)bytes_received, one_way, goodput, sender_elapsed,
           (unsigned long long)sender->total_tx_packets,
           (unsigned long long)sender->total_retx,
           sender->srtt_ms, sender->rto_ms);
}

int main(int argc, char **argv) {
    int exit_code = 1;
    int input_fd = -1;
    unsigned char *map = MAP_FAILED;
    sender_t sender = {.fd = -1, .rto_ms = 300.0};

    if (argc < 4) {
        usage(argv[0]);
        return 1;
    }

    int port;
    options_t options;
    if (parse_options(argc, argv, &port, &options) < 0) {
        usage(argv[0]);
        return 1;
    }

    sender.payload_size = (uint32_t)(options.mtu - 20 - 8 -
                                              (int)sizeof(frft_wire_hdr_t));
    if (sender.payload_size < 256 ||
        sender.payload_size > FRFT_MAX_DGRAM - sizeof(frft_wire_hdr_t)) {
        fprintf(stderr, "MTU produces invalid payload size.\n");
        return 1;
    }

    input_fd = open(argv[3], O_RDONLY);
    if (input_fd < 0) {
        perror("open input");
        goto cleanup;
    }

    struct stat file_info;
    if (fstat(input_fd, &file_info) < 0) {
        perror("fstat");
        goto cleanup;
    }
    if (file_info.st_size <= 0) {
        fprintf(stderr, "Input file must not be empty.\n");
        goto cleanup;
    }

    sender.file_size = (uint64_t)file_info.st_size;
    uint64_t packet_count = (sender.file_size + sender.payload_size - 1) /
                            sender.payload_size;
    if (packet_count > UINT32_MAX) {
        fprintf(stderr, "File too large for this protocol version.\n");
        goto cleanup;
    }
    sender.packet_count = (uint32_t)packet_count;

    map = mmap(NULL, (size_t)sender.file_size, PROT_READ, MAP_PRIVATE,
               input_fd, 0);
    if (map == MAP_FAILED) {
        perror("mmap");
        goto cleanup;
    }
    sender.map = map;

    sender.fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sender.fd < 0) {
        perror("socket");
        goto cleanup;
    }
    (void)frft_set_socket_buffers(sender.fd, 2 * 1024 * 1024);
    if (set_nonblocking(sender.fd) < 0) {
        perror("fcntl");
        goto cleanup;
    }

    sender.peer.sin_family = AF_INET;
    sender.peer.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, argv[1], &sender.peer.sin_addr) != 1) {
        fprintf(stderr, "Invalid IPv4 address: %s\n", argv[1]);
        goto cleanup;
    }

    sender.session = frft_random_u64();
    sender.rate_mbps = options.rate_mbps;
    sender.pacer.batch_packets = options.pace_batch;

    frft_meta_t meta = {
        .file_size = frft_htonll(sender.file_size),
        .payload_size = htonl(sender.payload_size),
        .packet_count = htonl(sender.packet_count)
    };
    print_configuration(argv[3], &options, &sender);

    unsigned char buffer[FRFT_MAX_DGRAM];
    if (perform_meta_handshake(&sender, &meta, buffer) < 0) goto cleanup;

    sender.acked = calloc(sender.packet_count, 1);
    sender.sent_ns = calloc(sender.packet_count, sizeof(*sender.sent_ns));
    sender.retries = calloc(sender.packet_count, 1);
    if (!sender.acked || !sender.sent_ns || !sender.retries) {
        fprintf(stderr, "Out of memory.\n");
        goto cleanup;
    }

    uint32_t base = 0;
    uint32_t next_seq = 0;
    uint64_t receiver_end_ns = 0;
    uint64_t bytes_received = 0;
    uint64_t last_timeout_scan = 0;
    int got_done = 0;

    while (!got_done) {
        if (receive_feedback(&sender, buffer, &base, next_seq, &got_done,
                             &receiver_end_ns, &bytes_received) < 0) {
            goto cleanup;
        }
        if (got_done) break;

        if (next_seq < sender.packet_count && next_seq - base < options.window) {
            if (transmit_data(&sender, next_seq, 0) == 0) {
                next_seq++;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("send DATA");
            }
            continue;
        }

        uint64_t now = frft_now_monotonic_ns();
        if (now - last_timeout_scan >= TIMEOUT_SCAN_NS) {
            last_timeout_scan = now;
            retransmit_timeouts(&sender, base, next_seq, now);
        }

        struct timespec delay = {0, 200000};
        nanosleep(&delay, NULL);
    }

    print_results(&sender, receiver_end_ns, bytes_received);
    exit_code = 0;

cleanup:
    free(sender.acked);
    free(sender.sent_ns);
    free(sender.retries);
    if (sender.fd >= 0) close(sender.fd);
    if (map != MAP_FAILED) munmap(map, (size_t)sender.file_size);
    if (input_fd >= 0) close(input_fd);
    return exit_code;
}
