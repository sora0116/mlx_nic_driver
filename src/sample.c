#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "sample.h"
#include "mlxnicd.h"

#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <limits.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum {
    MLXNICD_SAMPLE_TX_FRAME_CAPACITY = 1514,
    MLXNICD_SAMPLE_FLOOD_FRAME_CAPACITY = 4096,
    MLXNICD_SAMPLE_RX_BUFFER_SIZE = 2048,
    MLXNICD_SAMPLE_RX_WAIT_COUNT = 20,
    MLXNICD_SAMPLE_BENCH_MAGIC = 0x4d4c5842,
    MLXNICD_SAMPLE_BENCH_HDR_LEN = 24,
    /* A full-inline frame consumes two 64-byte SQ WQEBBs; SQ has 2048. */
    MLXNICD_SAMPLE_BENCH_MAX_WINDOW = 4096,
    MLXNICD_SAMPLE_BENCH_RX_BURST = 128,
    MLXNICD_SAMPLE_BENCH_RX_POST_COUNT = 4096,
    MLXNICD_SAMPLE_FLOOD_BURST = 4096,
    MLXNICD_SAMPLE_BENCH_F_REPLY = 0x0001,
};

struct sample_bench_slot {
    uint32_t seq;
    uint64_t send_ns;
    int active;
};

struct sample_bench_stats {
    uint64_t latency_min_ns;
    uint64_t latency_max_ns;
    uint64_t latency_sum_ns;
    uint64_t tx_bytes;
    uint64_t rx_bytes;
    uint64_t filtered_frames;
    uint64_t local_frames;
    uint64_t unmatched_frames;
    uint64_t tx_first_ns;
    uint64_t tx_last_ns;
    uint64_t rx_first_ns;
    uint64_t rx_last_ns;
};

struct sample_parallel_ctx {
    struct mlxnicd_dev *dev;
    const struct raw_bench_opts *opts;
    const uint8_t *template_frame;
    uint32_t frame_len;
    uint8_t src_mac[6];
    uint8_t dst_mac[6];
    atomic_int failed;
};

struct sample_parallel_worker {
    struct sample_parallel_ctx *ctx;
    uint16_t queue_id;
    uint32_t seq_begin;
    uint32_t seq_next;
    uint32_t seq_end;
    uint32_t window;
    uint32_t replies;
    uint64_t rx_frames;
};

struct sample_flood_ctx {
    struct mlxnicd_dev *dev;
    const struct raw_bench_opts *frame_opts;
    const uint8_t *template_frame;
    uint32_t frame_len;
    atomic_int go;
    atomic_int failed;
};

struct sample_flood_worker {
    struct sample_flood_ctx *ctx;
    uint16_t queue_id;
    uint32_t seq_begin;
    uint32_t seq_next;
    uint32_t seq_end;
};

static void sample_report_dev_error(const char *what, struct mlxnicd_dev *dev) {
    int err = mlxnicd_dev_last_error(dev);

    fprintf(stderr, "%s: %s (%d)\n", what, mlxnicd_strerror(err), err);
}

static void sample_pin_flood_worker(uint16_t queue_id) {
    cpu_set_t set;
    long cpu_count = sysconf(_SC_NPROCESSORS_ONLN);

    if (cpu_count <= 0 || queue_id >= (uint16_t)cpu_count ||
        queue_id >= CPU_SETSIZE) {
        return;
    }
    CPU_ZERO(&set);
    CPU_SET(queue_id, &set);
    (void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

static void sample_dump_hex(const char *label, const void *buf, size_t len) {
    const uint8_t *p = buf;
    size_t i;

    printf("%s (%zu bytes)\n", label, len);
    for (i = 0; i < len; i++) {
        if ((i % 16) == 0) {
            printf("  %04zx:", i);
        }
        printf(" %02x", p[i]);
        if ((i % 16) == 15 || i + 1 == len) {
            printf("\n");
        }
    }
}

static uint16_t sample_get_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t sample_get_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t sample_get_be64(const uint8_t *p) {
    return ((uint64_t)sample_get_be32(p) << 32) | sample_get_be32(p + 4);
}

static void sample_put_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void sample_put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void sample_put_be64(uint8_t *p, uint64_t v) {
    sample_put_be32(p, (uint32_t)(v >> 32));
    sample_put_be32(p + 4, (uint32_t)v);
}

static int sample_hex_nibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

static int sample_parse_mac_addr(const char *s, uint8_t mac[6]) {
    int i;

    if (s == NULL) {
        return -1;
    }
    for (i = 0; i < 6; i++) {
        int hi;
        int lo;

        hi = sample_hex_nibble(s[0]);
        lo = sample_hex_nibble(s[1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        mac[i] = (uint8_t)((hi << 4) | lo);
        s += 2;
        if (i != 5) {
            if (*s != ':') {
                return -1;
            }
            s++;
        }
    }
    return *s == '\0' ? 0 : -1;
}

static int sample_parse_ethertype16(const char *s, uint16_t *ethertype) {
    unsigned long value;
    char *end = NULL;

    if (s == NULL || ethertype == NULL) {
        return -1;
    }
    value = strtoul(s, &end, 0);
    if (end == s || *end != '\0' || value > 0xffffUL) {
        return -1;
    }
    *ethertype = (uint16_t)value;
    return 0;
}

static int sample_parse_payload_hex(const char *s, uint8_t *payload,
                                    size_t max_len, size_t *payload_len) {
    size_t len = 0;

    if (payload_len == NULL) {
        return -1;
    }
    *payload_len = 0;
    if (s == NULL) {
        return 0;
    }
    while (*s != '\0') {
        int hi;
        int lo;

        if (s[0] == ' ' || s[0] == ':' || s[0] == '-' || s[0] == '_') {
            s++;
            continue;
        }
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            s += 2;
            continue;
        }
        if (s[0] == '\0' || s[1] == '\0') {
            return -1;
        }
        hi = sample_hex_nibble(s[0]);
        lo = sample_hex_nibble(s[1]);
        if (hi < 0 || lo < 0 || len >= max_len) {
            return -1;
        }
        payload[len++] = (uint8_t)((hi << 4) | lo);
        s += 2;
    }
    *payload_len = len;
    return 0;
}

static uint64_t sample_now_ns(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static double sample_rate_pps(uint64_t packets, uint64_t span_ns) {
    if (span_ns == 0) {
        return 0.0;
    }
    return ((double)packets * 1e9) / (double)span_ns;
}

static double sample_rate_gbps(uint64_t bytes, uint64_t span_ns) {
    if (span_ns == 0) {
        return 0.0;
    }
    return ((double)bytes * 8.0) / (double)span_ns;
}

static size_t sample_bench_header_offset(const struct raw_bench_opts *opts) {
    return opts->rss_udp ? 14 + 20 + 8 : 14;
}

static uint16_t sample_ipv4_checksum(const uint8_t *header, size_t len) {
    uint32_t sum = 0;

    for (size_t i = 0; i < len; i += 2) {
        sum += sample_get_be16(header + i);
    }
    while ((sum >> 16) != 0) {
        sum = (sum & 0xffffu) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

static int sample_bench_build_frame(const struct raw_bench_opts *opts,
                                    uint32_t seq, uint64_t send_ns,
                                    uint16_t flags,
                                    uint8_t *frame, size_t frame_cap,
                                    uint32_t *frame_len) {
    uint8_t dst[6];
    uint8_t src[6];
    uint8_t extra_payload[MLXNICD_SAMPLE_TX_FRAME_CAPACITY];
    size_t extra_payload_len = 0;
    uint16_t ethertype = 0;
    size_t header_off;
    size_t len;

    if (opts == NULL || frame == NULL || frame_len == NULL) {
        return -1;
    }
    if (frame_cap < MLXNICD_SAMPLE_TX_FRAME_CAPACITY) {
        fprintf(stderr, "benchmark frame buffer too small\n");
        return -1;
    }
    if (sample_parse_mac_addr(opts->dst_mac, dst) != 0) {
        fprintf(stderr, "invalid --dst-mac: %s\n", opts->dst_mac);
        return -1;
    }
    if (sample_parse_mac_addr(opts->src_mac, src) != 0) {
        fprintf(stderr, "invalid --src-mac: %s\n", opts->src_mac);
        return -1;
    }
    if (sample_parse_ethertype16(opts->ethertype, &ethertype) != 0) {
        fprintf(stderr, "invalid --ethertype: %s\n", opts->ethertype);
        return -1;
    }
    if (opts->rss_udp && ethertype != 0x0800) {
        fprintf(stderr, "--rss-udp requires --ethertype 0x0800\n");
        return -1;
    }
    header_off = sample_bench_header_offset(opts);
    if (sample_parse_payload_hex(opts->payload_hex, extra_payload,
                                 sizeof(extra_payload),
                                 &extra_payload_len) != 0) {
        fprintf(stderr,
                "invalid --payload-hex or too large; max payload is %zu bytes\n",
                sizeof(extra_payload));
        return -1;
    }

    len = header_off + MLXNICD_SAMPLE_BENCH_HDR_LEN + extra_payload_len;
    if (len < 60) {
        len = 60;
    }
    if (len > MLXNICD_SAMPLE_TX_FRAME_CAPACITY) {
        fprintf(stderr, "benchmark frame too large: %zu > %u\n", len,
                MLXNICD_SAMPLE_TX_FRAME_CAPACITY);
        return -1;
    }

    memset(frame, 0, frame_cap);
    memcpy(frame + 0, dst, sizeof(dst));
    memcpy(frame + 6, src, sizeof(src));
    sample_put_be16(frame + 12, ethertype);
    if (opts->rss_udp) {
        uint8_t *ip = frame + 14;
        uint8_t *udp = ip + 20;

        ip[0] = 0x45;
        sample_put_be16(ip + 2, (uint16_t)(len - 14));
        sample_put_be16(ip + 4, (uint16_t)seq);
        sample_put_be16(ip + 6, 0x4000);
        ip[8] = 64;
        ip[9] = 17;
        ip[12] = 192; ip[13] = 0; ip[14] = 2; ip[15] = 6;
        ip[16] = 192; ip[17] = 0; ip[18] = 2; ip[19] = 5;
        sample_put_be16(ip + 10, sample_ipv4_checksum(ip, 20));
        sample_put_be16(udp, (uint16_t)(10000u + (seq % 50000u)));
        sample_put_be16(udp + 2, 20000);
        sample_put_be16(udp + 4, (uint16_t)(len - 14 - 20));
    }
    sample_put_be32(frame + header_off, MLXNICD_SAMPLE_BENCH_MAGIC);
    sample_put_be16(frame + header_off + 4, 1);
    sample_put_be16(frame + header_off + 6, flags);
    sample_put_be32(frame + header_off + 8, seq);
    sample_put_be64(frame + header_off + 12, send_ns);
    sample_put_be32(frame + header_off + 20, (uint32_t)extra_payload_len);
    memcpy(frame + header_off + MLXNICD_SAMPLE_BENCH_HDR_LEN, extra_payload,
           extra_payload_len);
    *frame_len = (uint32_t)len;
    return 0;
}

static void sample_bench_build_from_template(const struct raw_bench_opts *opts,
                                             const uint8_t *template_frame,
                                             uint32_t template_len,
                                             uint32_t seq, uint64_t send_ns,
                                             uint16_t flags, uint8_t *frame) {
    size_t header_off = sample_bench_header_offset(opts);

    memcpy(frame, template_frame, template_len);
    if (opts->rss_udp) {
        uint8_t *ip = frame + 14;
        uint8_t *udp = ip + 20;

        sample_put_be16(ip + 4, (uint16_t)seq);
        sample_put_be16(ip + 10, 0);
        sample_put_be16(ip + 10, sample_ipv4_checksum(ip, 20));
        sample_put_be16(udp, (uint16_t)(10000u + (seq % 50000u)));
    }
    sample_put_be16(frame + header_off + 6, flags);
    sample_put_be32(frame + header_off + 8, seq);
    sample_put_be64(frame + header_off + 12, send_ns);
}

static int sample_bench_is_reflection_fast(const struct sample_parallel_ctx *ctx,
                                           const struct mlxnicd_pkt *pkt) {
    return pkt->len >= 14 &&
           memcmp(pkt->data, ctx->src_mac, sizeof(ctx->src_mac)) == 0 &&
           memcmp(pkt->data + 6, ctx->dst_mac, sizeof(ctx->dst_mac)) == 0;
}

static void *sample_bench_parallel_worker(void *arg) {
    struct sample_parallel_worker *worker = arg;
    struct sample_parallel_ctx *ctx = worker->ctx;
    const uint32_t batch_cap = 512;
    uint8_t frames[MLXNICD_SAMPLE_FLOOD_BURST]
                  [MLXNICD_SAMPLE_TX_FRAME_CAPACITY];
    struct mlxnicd_pkt tx[MLXNICD_SAMPLE_FLOOD_BURST];
    struct mlxnicd_pkt rx[128];
    uint32_t idle = 0;

    while (worker->replies < worker->seq_end - worker->seq_begin &&
           !atomic_load_explicit(&ctx->failed, memory_order_relaxed)) {
        uint32_t batch = 0;

        while (batch < batch_cap && worker->seq_next < worker->seq_end &&
               worker->seq_next - worker->seq_begin <
                   worker->replies + worker->window) {
            sample_bench_build_from_template(ctx->opts, ctx->template_frame,
                                             ctx->frame_len, worker->seq_next, 0, 0,
                                             frames[batch]);
            tx[batch].data = frames[batch];
            tx[batch].len = ctx->frame_len;
            worker->seq_next++;
            batch++;
        }
        if (batch != 0 &&
            mlxnicd_tx_burst_q(ctx->dev, worker->queue_id, tx,
                               (uint16_t)batch) != batch) {
            atomic_store(&ctx->failed, 1);
            break;
        }

        {
            uint16_t got = mlxnicd_rx_burst_q(ctx->dev, worker->queue_id, rx,
                                               128, 0);
            if (got != 0) {
                for (uint16_t i = 0; i < got; i++) {
                    worker->rx_frames++;
                    if (sample_bench_is_reflection_fast(ctx, &rx[i])) {
                        worker->replies++;
                    }
                }
                if (mlxnicd_rx_release_q(ctx->dev, worker->queue_id, got) != 0) {
                    atomic_store(&ctx->failed, 1);
                    break;
                }
                idle = 0;
            } else if (batch == 0 && ++idle > 100000000U) {
                atomic_store(&ctx->failed, 1);
                break;
            }
        }
    }
    return NULL;
}

static int sample_raw_bench_parallel(struct mlxnicd_dev *dev,
                                     const struct raw_bench_opts *opts,
                                     const uint8_t *frame_template,
                                     uint32_t frame_len,
                                     struct sample_bench_stats *stats,
                                     uint32_t *sent_out,
                                     uint32_t *received_out) {
    struct sample_parallel_ctx ctx = {.dev = dev, .opts = opts,
                                      .template_frame = frame_template,
                                      .frame_len = frame_len};
    struct sample_parallel_worker workers[8] = {{0}};
    pthread_t threads[8];
    uint64_t start_ns = sample_now_ns();
    uint64_t total_sent = 0;
    uint64_t total_received = 0;
    int rc = -1;

    if (sample_parse_mac_addr(opts->src_mac, ctx.src_mac) != 0 ||
        sample_parse_mac_addr(opts->dst_mac, ctx.dst_mac) != 0) {
        return -1;
    }
    for (uint16_t q = 0; q < opts->queue_count; q++) {
        workers[q].ctx = &ctx;
        workers[q].queue_id = q;
        workers[q].seq_begin =
            (uint32_t)(((uint64_t)opts->packet_count * q) / opts->queue_count);
        workers[q].seq_next = workers[q].seq_begin;
        workers[q].seq_end = (uint32_t)(((uint64_t)opts->packet_count * (q + 1)) /
                                        opts->queue_count);
        workers[q].window = opts->window / opts->queue_count;
        if (workers[q].window == 0) workers[q].window = 1;
        if (pthread_create(&threads[q], NULL, sample_bench_parallel_worker,
                           &workers[q]) != 0) {
            atomic_store(&ctx.failed, 1);
            for (uint16_t i = 0; i < q; i++) pthread_join(threads[i], NULL);
            return -1;
        }
    }
    for (uint16_t q = 0; q < opts->queue_count; q++) pthread_join(threads[q], NULL);
    for (uint16_t q = 0; q < opts->queue_count; q++) {
        total_sent += workers[q].seq_next - workers[q].seq_begin;
        total_received += workers[q].replies;
    }
    *sent_out = (uint32_t)total_sent;
    *received_out = (uint32_t)total_received;
    stats->tx_bytes = (uint64_t)*sent_out * frame_len;
    stats->rx_bytes = (uint64_t)*received_out * frame_len;
    stats->tx_first_ns = stats->rx_first_ns = start_ns;
    stats->tx_last_ns = stats->rx_last_ns = sample_now_ns();
    for (uint16_t q = 0; q < opts->queue_count; q++)
        fprintf(stdout,
                "raw-bench: worker%u sent=%u replies=%u rx_frames=%" PRIu64
                "\n",
                q, workers[q].seq_next - workers[q].seq_begin,
                workers[q].replies, workers[q].rx_frames);
    rc = !atomic_load(&ctx.failed) && *sent_out == opts->packet_count &&
         *received_out == opts->packet_count ? 0 : -1;
    return rc;
}

static void *sample_raw_flood_worker(void *arg) {
    struct sample_flood_worker *worker = arg;
    struct sample_flood_ctx *ctx = worker->ctx;
    uint8_t (*frames)[MLXNICD_SAMPLE_FLOOD_FRAME_CAPACITY];
    struct mlxnicd_pkt *tx;

    sample_pin_flood_worker(worker->queue_id);

    /* Keep the large batching buffers off pthread's comparatively small
     * default stack.  The allocation is per queue and remains private to its
     * sole worker. */
    frames = malloc((size_t)MLXNICD_SAMPLE_FLOOD_BURST * sizeof(*frames));
    tx = malloc((size_t)MLXNICD_SAMPLE_FLOOD_BURST * sizeof(*tx));
    if (frames == NULL || tx == NULL) {
        free(tx);
        free(frames);
        atomic_store_explicit(&ctx->failed, 1, memory_order_relaxed);
        return NULL;
    }
    /* raw-flood measures the TX datapath, not packet construction.  Build a
     * worker-private, RSS-diverse set once and reuse it.  The 4,096 source
     * ports are sufficient to spread traffic across the peer's queues while
     * avoiding a 1514-byte template copy and IPv4 checksum per packet in the
     * timed region. */
    for (uint32_t i = 0; i < MLXNICD_SAMPLE_FLOOD_BURST; i++) {
        sample_bench_build_from_template(
            ctx->frame_opts, ctx->template_frame, ctx->frame_len,
            worker->seq_begin + i, 0, 0, frames[i]);
        tx[i].data = frames[i];
        tx[i].len = ctx->frame_len;
    }
    if (ctx->frame_len > 98 &&
        mlxnicd_tx_flood_prepare_q(ctx->dev, worker->queue_id, tx,
                                   MLXNICD_SAMPLE_FLOOD_BURST) != 0) {
        atomic_store_explicit(&ctx->failed, 1, memory_order_relaxed);
        free(tx);
        free(frames);
        return NULL;
    }
    if (ctx->frame_len > 98) {
        for (uint32_t i = 0; i < MLXNICD_SAMPLE_FLOOD_BURST; i++) {
            tx[i].data = NULL;
        }
    }

    while (!atomic_load_explicit(&ctx->go, memory_order_acquire)) {
    }
    while (worker->seq_next < worker->seq_end &&
           !atomic_load_explicit(&ctx->failed, memory_order_relaxed)) {
        uint32_t batch = worker->seq_end - worker->seq_next;

        if (batch > MLXNICD_SAMPLE_FLOOD_BURST) {
            batch = MLXNICD_SAMPLE_FLOOD_BURST;
        }
        if (mlxnicd_tx_burst_q(ctx->dev, worker->queue_id, tx,
                               (uint16_t)batch) != batch) {
            atomic_store_explicit(&ctx->failed, 1, memory_order_relaxed);
            break;
        }
        worker->seq_next += batch;
    }
    if (!atomic_load_explicit(&ctx->failed, memory_order_relaxed) &&
        mlxnicd_tx_flush_q(ctx->dev, worker->queue_id) != 0) {
        atomic_store_explicit(&ctx->failed, 1, memory_order_relaxed);
    }
    free(tx);
    free(frames);
    return NULL;
}

int sample_raw_flood(const struct raw_flood_opts *opts) {
    struct raw_bench_opts frame_opts = {0};
    struct mlxnicd_dev *dev = NULL;
    struct mlxnicd_dev_config config;
    struct sample_flood_ctx ctx = {0};
    struct sample_flood_worker workers[8] = {{0}};
    pthread_t threads[8];
    uint8_t frame_template[MLXNICD_SAMPLE_FLOOD_FRAME_CAPACITY];
    uint32_t frame_len = 0;
    uint64_t start_ns;
    uint64_t end_ns;
    uint64_t sent = 0;
    int rc = -1;

    if (opts == NULL) return -1;
    frame_opts.bdf = opts->bdf;
    frame_opts.peer_if = opts->peer_if;
    frame_opts.src_mac = opts->src_mac;
    frame_opts.dst_mac = opts->dst_mac;
    frame_opts.ethertype = opts->ethertype;
    frame_opts.payload_hex = opts->payload_hex;
    frame_opts.rss_udp = opts->rss_udp;
    if (sample_bench_build_frame(&frame_opts, 0, 0, 0, frame_template,
                                 sizeof(frame_template), &frame_len) != 0) {
        return -1;
    }
    if (opts->frame_len != 0) {
        uint8_t *ip;
        uint8_t *udp;

        if (opts->frame_len < frame_len ||
            opts->frame_len > MLXNICD_SAMPLE_FLOOD_FRAME_CAPACITY) {
            fprintf(stderr, "--frame-len must be in the range %u..%u\n",
                    frame_len, MLXNICD_SAMPLE_FLOOD_FRAME_CAPACITY);
            return -1;
        }
        memset(frame_template + frame_len, 0, opts->frame_len - frame_len);
        frame_len = opts->frame_len;
        if (opts->rss_udp) {
            ip = frame_template + 14;
            udp = ip + 20;
            sample_put_be16(ip + 2, (uint16_t)(frame_len - 14));
            sample_put_be16(ip + 10, 0);
            sample_put_be16(ip + 10, sample_ipv4_checksum(ip, 20));
            sample_put_be16(udp + 4, (uint16_t)(frame_len - 14 - 20));
        }
    }

    mlxnicd_dev_config_init(&config);
    config.flags = MLXNICD_DEV_F_TX;
    config.queue_count = opts->queue_count;
    config.log_verbose = opts->verbose;
    fprintf(stdout,
            "raw-flood: peer-if=%s bdf=%s count=%" PRIu32
            " queues=%u frame_len=%u\n",
            opts->peer_if, opts->bdf, opts->packet_count, opts->queue_count,
            frame_len);
    if (mlxnicd_dev_open(&dev, opts->bdf) != 0) goto out;
    if (mlxnicd_dev_configure(dev, &config) != 0 ||
        mlxnicd_dev_start(dev) != 0) {
        sample_report_dev_error("raw-flood device start", dev);
        goto out;
    }

    ctx.dev = dev;
    ctx.frame_opts = &frame_opts;
    ctx.template_frame = frame_template;
    ctx.frame_len = frame_len;
    for (uint16_t q = 0; q < opts->queue_count; q++) {
        workers[q].ctx = &ctx;
        workers[q].queue_id = q;
        workers[q].seq_begin =
            (uint32_t)(((uint64_t)opts->packet_count * q) / opts->queue_count);
        workers[q].seq_next = workers[q].seq_begin;
        workers[q].seq_end = (uint32_t)(((uint64_t)opts->packet_count * (q + 1)) /
                                        opts->queue_count);
        if (pthread_create(&threads[q], NULL, sample_raw_flood_worker,
                           &workers[q]) != 0) {
            atomic_store(&ctx.failed, 1);
            atomic_store(&ctx.go, 1);
            for (uint16_t i = 0; i < q; i++) pthread_join(threads[i], NULL);
            goto out;
        }
    }
    start_ns = sample_now_ns();
    atomic_store_explicit(&ctx.go, 1, memory_order_release);
    for (uint16_t q = 0; q < opts->queue_count; q++) {
        pthread_join(threads[q], NULL);
        sent += workers[q].seq_next - workers[q].seq_begin;
    }
    end_ns = sample_now_ns();
    if (!atomic_load(&ctx.failed) && sent == opts->packet_count) {
        uint64_t elapsed = end_ns - start_ns;

        fprintf(stdout, "raw-flood: ok tx=%" PRIu64 "\n", sent);
        fprintf(stdout, "raw-flood: tx rate %.3f Mpps %.3f Gbps\n",
                sample_rate_pps(sent, elapsed) / 1e6,
                sample_rate_gbps(sent * frame_len, elapsed));
        rc = 0;
    } else {
        fprintf(stdout, "raw-flood: failed tx=%" PRIu64 "\n", sent);
    }

out:
    mlxnicd_dev_close(dev);
    return rc;
}

static int sample_bench_parse_frame(const struct raw_bench_opts *opts,
                                    const struct mlxnicd_pkt *pkt,
                                    uint32_t *seq, uint64_t *send_ns,
                                    uint16_t *flags) {
    uint16_t want_ethertype = 0;
    uint16_t version;
    size_t header_off;

    if (opts == NULL || pkt == NULL || seq == NULL || send_ns == NULL ||
        flags == NULL) {
        return -1;
    }
    header_off = sample_bench_header_offset(opts);
    if (pkt->len < header_off + MLXNICD_SAMPLE_BENCH_HDR_LEN) {
        return -1;
    }
    if (sample_parse_ethertype16(opts->ethertype, &want_ethertype) != 0) {
        return -1;
    }
    if (sample_get_be16(pkt->data + 12) != want_ethertype) {
        return -1;
    }
    if (sample_get_be32(pkt->data + header_off) != MLXNICD_SAMPLE_BENCH_MAGIC) {
        return -1;
    }
    version = sample_get_be16(pkt->data + header_off + 4);
    if (version != 1) {
        return -1;
    }
    *flags = sample_get_be16(pkt->data + header_off + 6);
    *seq = sample_get_be32(pkt->data + header_off + 8);
    *send_ns = sample_get_be64(pkt->data + header_off + 12);
    return 0;
}

/* Accept a DPDK macswap loopback that preserves the benchmark payload. */
static int sample_bench_is_l2_reflection(const struct raw_bench_opts *opts,
                                         const struct mlxnicd_pkt *pkt) {
    uint8_t src[6];
    uint8_t dst[6];

    if (opts == NULL || pkt == NULL || pkt->len < 14 ||
        sample_parse_mac_addr(opts->src_mac, src) != 0 ||
        sample_parse_mac_addr(opts->dst_mac, dst) != 0) {
        return 0;
    }
    return memcmp(pkt->data, src, sizeof(src)) == 0 &&
           memcmp(pkt->data + 6, dst, sizeof(dst)) == 0;
}

static void sample_bench_stats_note_latency(struct sample_bench_stats *stats,
                                            uint64_t latency_ns) {
    if (stats->latency_min_ns == 0 || latency_ns < stats->latency_min_ns) {
        stats->latency_min_ns = latency_ns;
    }
    if (latency_ns > stats->latency_max_ns) {
        stats->latency_max_ns = latency_ns;
    }
    stats->latency_sum_ns += latency_ns;
}

static int sample_bench_slot_find(struct sample_bench_slot *slots,
                                  uint32_t slot_count, uint32_t seq) {
    uint32_t i;

    if (slot_count == 0) {
        return -1;
    }
    i = seq % slot_count;
    if (slots[i].active && slots[i].seq == seq) {
        return (int)i;
    }
    return -1;
}

static int sample_bench_slot_alloc(struct sample_bench_slot *slots,
                                   uint32_t slot_count, uint32_t seq) {
    uint32_t i;

    if (slot_count == 0) {
        return -1;
    }
    i = seq % slot_count;
    if (!slots[i].active) {
        return (int)i;
    }
    return -1;
}

static int sample_bench_slot_find_active(struct sample_bench_slot *slots,
                                         uint32_t slot_count) {
    uint32_t i;

    for (i = 0; i < slot_count; i++) {
        if (slots[i].active) {
            return (int)i;
        }
    }
    return -1;
}

int sample_mlx5_seq_basic(const char *bdf) {
    return sample_mlx5_tx_test(bdf, 1);
}

static int sample_start_rx_dev(struct mlxnicd_dev **dev_out, const char *bdf,
                               uint32_t rx_post_count, int promisc,
                               const char *tag) {
    struct mlxnicd_dev *dev = NULL;
    struct mlxnicd_dev_config config;

    mlxnicd_dev_config_init(&config);
    config.flags = MLXNICD_DEV_F_RX | (promisc ? MLXNICD_DEV_F_PROMISC : 0);
    config.rx_post_count = rx_post_count;
    config.log_verbose = 1;

    if (mlxnicd_dev_open(&dev, bdf) != 0) {
        return -1;
    }
    if (mlxnicd_dev_configure(dev, &config) != 0) {
        sample_report_dev_error("mlxnicd_dev_configure", dev);
        mlxnicd_dev_close(dev);
        return -1;
    }
    if (mlxnicd_dev_start(dev) != 0) {
        sample_report_dev_error("mlxnicd_dev_start", dev);
        mlxnicd_dev_close(dev);
        return -1;
    }

    if (config.rx_post_count != 0) {
        printf("%s: started bdf=%s flags=0x%08" PRIx32
               " rx_post_count=%" PRIu32 "\n",
               tag, bdf, config.flags, config.rx_post_count);
    } else {
        printf("%s: started bdf=%s flags=0x%08" PRIx32
               " rx_post_count=default\n",
               tag, bdf, config.flags);
    }
    *dev_out = dev;
    return 0;
}

int sample_mlx5_tx_test(const char *bdf, unsigned int count) {
    struct mlx5_tx_test_opts opts = {0};

    opts.count = count;
    return sample_mlx5_tx_test_opts(bdf, &opts);
}

int sample_mlx5_tx_test_opts(const char *bdf,
                             const struct mlx5_tx_test_opts *opts) {
    struct mlxnicd_dev *dev = NULL;
    struct mlxnicd_dev_config config;
    struct mlxnicd_l2_frame_spec spec = {0};
    struct mlxnicd_pkt pkt = {0};
    uint8_t frame[MLXNICD_SAMPLE_TX_FRAME_CAPACITY];
    uint32_t frame_len = 0;
    unsigned int count = opts != NULL && opts->count != 0 ? opts->count : 1;
    unsigned int sent = 0;
    int rc = -1;

    mlxnicd_dev_config_init(&config);
    config.flags = MLXNICD_DEV_F_TX;
    config.log_verbose = 1;
    spec.dst_mac = opts != NULL ? opts->dst_mac : NULL;
    spec.src_mac = opts != NULL ? opts->src_mac : NULL;
    spec.ethertype = opts != NULL ? opts->ethertype : NULL;
    spec.payload_hex = opts != NULL ? opts->payload_hex : NULL;

    if (mlxnicd_frame_build(&spec, frame, sizeof(frame), &frame_len) != 0) {
        return -1;
    }
    if (mlxnicd_dev_open(&dev, bdf) != 0) {
        return -1;
    }
    if (mlxnicd_dev_configure(dev, &config) != 0) {
        sample_report_dev_error("mlxnicd_dev_configure", dev);
        goto out;
    }
    if (mlxnicd_dev_start(dev) != 0) {
        sample_report_dev_error("mlxnicd_dev_start", dev);
        goto out;
    }

    pkt.data = frame;
    pkt.len = frame_len;
    while (sent < count) {
        if (mlxnicd_tx_burst(dev, &pkt, 1) != 1) {
            sample_report_dev_error("mlxnicd_tx_burst", dev);
            goto out;
        }
        sent++;
    }
    rc = 0;

out:
    mlxnicd_dev_close(dev);
    return rc;
}

int sample_mlx5_rx_objects(const char *bdf) {
    struct mlxnicd_dev *dev = NULL;
    int rc = -1;

    if (sample_start_rx_dev(&dev, bdf, 0, 0, "sample-rx-objects") != 0) {
        return -1;
    }
    printf("sample-rx-objects: RX datapath objects are ready\n");
    rc = 0;
    mlxnicd_dev_close(dev);
    return rc;
}

int sample_mlx5_rx_post_test(const char *bdf) {
    struct mlxnicd_dev *dev = NULL;
    int rc = -1;

    if (sample_start_rx_dev(&dev, bdf, 1, 0, "sample-rx-post-test") != 0) {
        return -1;
    }
    printf("sample-rx-post-test: initial RX buffers are posted\n");
    rc = 0;
    mlxnicd_dev_close(dev);
    return rc;
}

int sample_mlx5_rx_steer_test(const char *bdf) {
    struct mlxnicd_dev *dev = NULL;
    int rc = -1;

    if (sample_start_rx_dev(&dev, bdf, 1, 1, "sample-rx-steer-test") != 0) {
        return -1;
    }
    printf("sample-rx-steer-test: RX flow steering path is ready\n");
    rc = 0;
    mlxnicd_dev_close(dev);
    return rc;
}

int sample_mlx5_rx_wait_test(const char *bdf) {
    struct mlxnicd_dev *dev = NULL;
    struct mlxnicd_dev_config config;
    struct mlxnicd_pkt pkt = {0};
    uint32_t accepted = 0;
    int rc = -1;

    mlxnicd_dev_config_init(&config);
    config.flags = MLXNICD_DEV_F_RX | MLXNICD_DEV_F_PROMISC;
    config.log_verbose = 1;

    if (mlxnicd_dev_open(&dev, bdf) != 0) {
        return -1;
    }
    if (mlxnicd_dev_configure(dev, &config) != 0) {
        sample_report_dev_error("mlxnicd_dev_configure", dev);
        goto out;
    }
    if (mlxnicd_dev_start(dev) != 0) {
        sample_report_dev_error("mlxnicd_dev_start", dev);
        goto out;
    }

    while (accepted < MLXNICD_SAMPLE_RX_WAIT_COUNT) {
        uint16_t got = mlxnicd_rx_burst(dev, &pkt, 1, 30000);
        size_t dump_len;

        if (got != 1) {
            sample_report_dev_error("mlxnicd_rx_burst", dev);
            goto out;
        }
        printf("sample-rx-wait: packet=%" PRIu32 "/%u len=%" PRIu32 "\n",
               accepted + 1, MLXNICD_SAMPLE_RX_WAIT_COUNT, pkt.len);
        dump_len = pkt.len;
        if (dump_len > MLXNICD_SAMPLE_RX_BUFFER_SIZE) {
            dump_len = MLXNICD_SAMPLE_RX_BUFFER_SIZE;
        }
        if (dump_len < 64) {
            dump_len = 64;
        }
        sample_dump_hex("  rx frame", pkt.data, dump_len);
        if (mlxnicd_rx_release(dev, 1) != 0) {
            sample_report_dev_error("mlxnicd_rx_release", dev);
            goto out;
        }
        accepted++;
    }
    rc = 0;

out:
    mlxnicd_dev_close(dev);
    return rc;
}

int sample_raw_loop(const struct raw_loop_opts *raw_opts) {
    struct mlxnicd_dev *dev = NULL;
    struct mlxnicd_dev_config config;
    struct mlxnicd_l2_frame_spec spec = {0};
    struct mlxnicd_pkt tx_pkt = {0};
    struct mlxnicd_pkt rx_pkt = {0};
    uint8_t frame[MLXNICD_SAMPLE_TX_FRAME_CAPACITY];
    uint32_t frame_len = 0;
    uint32_t rx_wait_count;
    int rx_pre_delay_ms;
    int rx_timeout_ms;
    uint32_t accepted = 0;
    uint32_t skipped_local = 0;
    uint32_t skipped_filtered = 0;
    int rc = -1;

    if (raw_opts == NULL) {
        fprintf(stderr, "raw-loop options are required\n");
        return -1;
    }

    mlxnicd_dev_config_init(&config);
    config.flags = MLXNICD_DEV_F_TX | MLXNICD_DEV_F_RX | MLXNICD_DEV_F_PROMISC;
    config.log_verbose = raw_opts->verbose;
    spec.dst_mac = raw_opts->dst_mac;
    spec.src_mac = raw_opts->src_mac;
    spec.ethertype = raw_opts->ethertype;
    spec.payload_hex = raw_opts->payload_hex != NULL ? raw_opts->payload_hex
                                                     : "6d6c786e6963642d7261772d6c6f6f70";
    rx_wait_count =
        raw_opts->rx_count != 0 ? raw_opts->rx_count : MLXNICD_SAMPLE_RX_WAIT_COUNT;
    rx_pre_delay_ms = raw_opts->pre_rx_delay_ms != 0
                          ? (int)raw_opts->pre_rx_delay_ms
                          : 0;
    rx_timeout_ms =
        raw_opts->timeout_ms != 0 ? (int)raw_opts->timeout_ms : 30000;

    fprintf(stdout, "raw-loop: peer-if=%s bdf=%s\n", raw_opts->peer_if,
            raw_opts->bdf);
    fprintf(stdout, "raw-loop: TX dst=%s src=%s ethertype=%s\n",
            raw_opts->dst_mac, raw_opts->src_mac, raw_opts->ethertype);
    fprintf(stdout,
            "raw-loop: RX waits for %" PRIu32
            " packet(s), pre-rx-delay=%d ms, timeout=%d ms\n",
            rx_wait_count, rx_pre_delay_ms, rx_timeout_ms);

    if (mlxnicd_frame_build(&spec, frame, sizeof(frame), &frame_len) != 0) {
        return -1;
    }
    if (mlxnicd_dev_open(&dev, raw_opts->bdf) != 0) {
        return -1;
    }
    if (mlxnicd_dev_configure(dev, &config) != 0) {
        sample_report_dev_error("mlxnicd_dev_configure", dev);
        goto out;
    }
    if (mlxnicd_dev_start(dev) != 0) {
        sample_report_dev_error("mlxnicd_dev_start", dev);
        goto out;
    }
    tx_pkt.data = frame;
    tx_pkt.len = frame_len;
    if (mlxnicd_tx_burst(dev, &tx_pkt, 1) != 1) {
        sample_report_dev_error("mlxnicd_tx_burst", dev);
        goto out;
    }
    if (rx_pre_delay_ms > 0) {
        printf("raw-loop: RX armed; sleeping %d ms before RX poll\n",
               rx_pre_delay_ms);
    }
    while (accepted < rx_wait_count) {
        uint16_t got;

        if (rx_pre_delay_ms > 0 && accepted == 0 && skipped_local == 0) {
            usleep((useconds_t)rx_pre_delay_ms * 1000U);
        }
        got = mlxnicd_rx_burst(dev, &rx_pkt, 1, rx_timeout_ms);
        if (got != 1) {
            sample_report_dev_error("mlxnicd_rx_burst", dev);
            goto out;
        }
        if (rx_pkt.len >= frame_len &&
            memcmp(rx_pkt.data, frame, frame_len) == 0) {
            skipped_local++;
        } else {
            accepted++;
            if (raw_opts->verbose) {
                size_t dump_len = rx_pkt.len < 64 ? 64 : rx_pkt.len;
                if (dump_len > MLXNICD_SAMPLE_RX_BUFFER_SIZE) {
                    dump_len = MLXNICD_SAMPLE_RX_BUFFER_SIZE;
                }
                sample_dump_hex("  raw-loop rx frame", rx_pkt.data, dump_len);
            }
        }
        if (mlxnicd_rx_release(dev, 1) != 0) {
            sample_report_dev_error("mlxnicd_rx_release", dev);
            goto out;
        }
    }
    rc = 0;

out:
    mlxnicd_dev_close(dev);
    if (rc == 0) {
        fprintf(stdout,
                "raw-loop: ok tx=1 rx=%" PRIu32
                " skipped_local=%" PRIu32 " skipped_filtered=%" PRIu32
                "\n",
                accepted, skipped_local, skipped_filtered);
    } else {
        fprintf(stdout,
                "raw-loop: failed tx=1 rx=%" PRIu32
                " skipped_local=%" PRIu32 " skipped_filtered=%" PRIu32
                "\n",
                accepted, skipped_local, skipped_filtered);
    }
    return rc;
}

int sample_raw_echo(const struct raw_echo_opts *opts) {
    struct mlxnicd_dev *dev = NULL;
    struct mlxnicd_dev_config config;
    struct mlxnicd_pkt rx_pkt = {0};
    struct mlxnicd_pkt tx_pkt = {0};
    uint16_t ethertype;
    uint32_t echoed = 0;
    uint32_t filtered = 0;
    uint32_t already_reply = 0;
    int rc = -1;

    if (opts == NULL) {
        fprintf(stderr, "raw-echo options are required\n");
        return -1;
    }
    if (sample_parse_ethertype16(opts->ethertype, &ethertype) != 0) {
        fprintf(stderr, "raw-echo: invalid ethertype: %s\n", opts->ethertype);
        return -1;
    }

    mlxnicd_dev_config_init(&config);
    config.flags = MLXNICD_DEV_F_TX | MLXNICD_DEV_F_RX | MLXNICD_DEV_F_PROMISC;
    config.rx_post_count = MLXNICD_SAMPLE_BENCH_RX_POST_COUNT;
    config.log_verbose = opts->verbose;

    fprintf(stdout,
            "raw-echo: peer-if=%s bdf=%s ethertype=0x%04" PRIx16
            " count=%" PRIu32 " timeout=%" PRIu32 " ms\n",
            opts->peer_if, opts->bdf, ethertype, opts->packet_count,
            opts->timeout_ms);

    if (mlxnicd_dev_open(&dev, opts->bdf) != 0) {
        return -1;
    }
    if (mlxnicd_dev_configure(dev, &config) != 0) {
        sample_report_dev_error("mlxnicd_dev_configure", dev);
        goto out;
    }
    if (mlxnicd_dev_start(dev) != 0) {
        sample_report_dev_error("mlxnicd_dev_start", dev);
        goto out;
    }

    while (echoed < opts->packet_count) {
        uint16_t flags;
        uint8_t mac[6];
        uint8_t tx_frame[MLXNICD_SAMPLE_TX_FRAME_CAPACITY];
        int tx_failed = 0;

        if (mlxnicd_rx_burst(dev, &rx_pkt, 1, (int)opts->timeout_ms) != 1) {
            sample_report_dev_error("mlxnicd_rx_burst", dev);
            goto out;
        }
        if (rx_pkt.len < 14 + MLXNICD_SAMPLE_BENCH_HDR_LEN ||
            sample_get_be16(rx_pkt.data + 12) != ethertype ||
            sample_get_be32(rx_pkt.data + 14) != MLXNICD_SAMPLE_BENCH_MAGIC) {
            filtered++;
            goto release;
        }

        flags = sample_get_be16(rx_pkt.data + 20);
        if ((flags & MLXNICD_SAMPLE_BENCH_F_REPLY) != 0) {
            already_reply++;
            goto release;
        }

        if (rx_pkt.len > sizeof(tx_frame)) {
            filtered++;
            goto release;
        }
        memcpy(tx_frame, rx_pkt.data, rx_pkt.len);
        memcpy(mac, tx_frame, sizeof(mac));
        memcpy(tx_frame, tx_frame + 6, sizeof(mac));
        memcpy(tx_frame + 6, mac, sizeof(mac));
        sample_put_be16(tx_frame + 20,
                        flags | MLXNICD_SAMPLE_BENCH_F_REPLY);
        tx_pkt.data = tx_frame;
        tx_pkt.len = rx_pkt.len;
        if (mlxnicd_tx_burst(dev, &tx_pkt, 1) != 1) {
            sample_report_dev_error("mlxnicd_tx_burst", dev);
            tx_failed = 1;
            goto release;
        }
        echoed++;
        if (opts->verbose) {
            fprintf(stdout, "raw-echo: echoed seq=%" PRIu32 " len=%" PRIu32
                            " flags=0x%04" PRIx16 "\n",
                    sample_get_be32(rx_pkt.data + 22), rx_pkt.len,
                    sample_get_be16(tx_frame + 20));
        }

release:
        if (mlxnicd_rx_release(dev, 1) != 0) {
            sample_report_dev_error("mlxnicd_rx_release", dev);
            goto out;
        }
        if (tx_failed) {
            goto out;
        }
    }

    rc = 0;

out:
    mlxnicd_dev_close(dev);
    fprintf(stdout,
            "raw-echo: %s echoed=%" PRIu32 " filtered=%" PRIu32
            " already_reply=%" PRIu32 "\n",
            rc == 0 ? "ok" : "failed", echoed, filtered, already_reply);
    return rc;
}

int sample_raw_bench(const struct raw_bench_opts *opts) {
    struct mlxnicd_dev *dev = NULL;
    struct mlxnicd_dev_config config;
    struct mlxnicd_pkt tx_pkt = {0};
    struct mlxnicd_pkt rx_pkt = {0};
    struct sample_bench_slot slots[MLXNICD_SAMPLE_BENCH_MAX_WINDOW] = {{0}};
    struct sample_bench_stats stats = {0};
    uint8_t frame[MLXNICD_SAMPLE_TX_FRAME_CAPACITY];
    uint8_t frame_template[MLXNICD_SAMPLE_TX_FRAME_CAPACITY];
    uint32_t frame_template_len = 0;
    uint32_t sent = 0;
    uint32_t received = 0;
    uint32_t inflight = 0;
    uint64_t throughput_start_ns = 0;
    uint16_t rx_queue_cursor = 0;
    uint64_t rx_queue_frames[8] = {0};
    int rc = -1;

    if (opts == NULL) {
        fprintf(stderr, "raw-bench options are required\n");
        return -1;
    }
    if (opts->window > MLXNICD_SAMPLE_BENCH_MAX_WINDOW) {
        fprintf(stderr, "--window=%" PRIu32 " exceeds supported max=%u\n",
                opts->window, MLXNICD_SAMPLE_BENCH_MAX_WINDOW);
        return -1;
    }
    if (sample_bench_build_frame(opts, 0, 0, 0, frame_template,
                                 sizeof(frame_template),
                                 &frame_template_len) != 0) {
        return -1;
    }

    mlxnicd_dev_config_init(&config);
    config.flags = MLXNICD_DEV_F_TX | MLXNICD_DEV_F_RX | MLXNICD_DEV_F_PROMISC;
    config.rx_post_count = MLXNICD_SAMPLE_BENCH_RX_POST_COUNT;
    config.queue_count = opts->queue_count;
    config.log_verbose = opts->verbose;

    fprintf(stdout,
            "raw-bench: peer-if=%s bdf=%s count=%" PRIu32
            " window=%" PRIu32 " timeout=%" PRIu32 " ms min-rtt=%" PRIu32
            " ns\n",
            opts->peer_if, opts->bdf, opts->packet_count, opts->window,
            opts->timeout_ms, opts->min_rtt_ns);
    fprintf(stdout, "raw-bench: TX dst=%s src=%s ethertype=%s\n",
            opts->dst_mac, opts->src_mac, opts->ethertype);

    if (mlxnicd_dev_open(&dev, opts->bdf) != 0) {
        return -1;
    }
    if (mlxnicd_dev_configure(dev, &config) != 0) {
        sample_report_dev_error("mlxnicd_dev_configure", dev);
        goto out;
    }
    if (mlxnicd_dev_start(dev) != 0) {
        sample_report_dev_error("mlxnicd_dev_start", dev);
        goto out;
    }
    if (opts->throughput_only && opts->queue_count > 1) {
        rc = sample_raw_bench_parallel(dev, opts, frame_template,
                                       frame_template_len, &stats, &sent,
                                       &received);
        goto out;
    }
    if (opts->throughput_only) {
        throughput_start_ns = sample_now_ns();
    }

    if (opts->window == 1) {
        while (sent < opts->packet_count) {
            uint64_t send_ns =
                opts->throughput_only ? 0 : sample_now_ns();
            uint32_t frame_len = 0;

            sample_bench_build_from_template(opts, frame_template,
                                             frame_template_len, sent, send_ns,
                                             0, frame);
            frame_len = frame_template_len;
            tx_pkt.data = frame;
            tx_pkt.len = frame_len;
            if (mlxnicd_tx_burst(dev, &tx_pkt, 1) != 1) {
                sample_report_dev_error("mlxnicd_tx_burst", dev);
                goto out;
            }
            stats.tx_bytes += frame_len;
            if (stats.tx_first_ns == 0) {
                stats.tx_first_ns = send_ns;
            }
            stats.tx_last_ns = send_ns;
            sent++;

            for (;;) {
                uint32_t seq = 0;
                uint64_t pkt_send_ns = 0;
                uint16_t flags = 0;
                uint64_t now_ns;
                uint16_t got =
                    mlxnicd_rx_burst(dev, &rx_pkt, 1, (int)opts->timeout_ms);

                if (got != 1) {
                    sample_report_dev_error("mlxnicd_rx_burst", dev);
                    goto out;
                }
                if (sample_bench_parse_frame(opts, &rx_pkt, &seq, &pkt_send_ns,
                                             &flags) != 0) {
                    stats.filtered_frames++;
                } else if ((flags & MLXNICD_SAMPLE_BENCH_F_REPLY) == 0 &&
                           !sample_bench_is_l2_reflection(opts, &rx_pkt)) {
                    stats.local_frames++;
                } else {
                    uint64_t rtt_ns;

                    (void)seq;
                    (void)pkt_send_ns;
                    now_ns = opts->throughput_only ? 0 : sample_now_ns();
                    rtt_ns = now_ns >= send_ns ? now_ns - send_ns : 0;
                    if (!opts->throughput_only && rtt_ns < opts->min_rtt_ns) {
                        stats.local_frames++;
                    } else {
                        received++;
                        stats.rx_bytes += rx_pkt.len;
                        if (stats.rx_first_ns == 0) {
                            stats.rx_first_ns = now_ns;
                        }
                        stats.rx_last_ns = now_ns;
                        sample_bench_stats_note_latency(&stats, rtt_ns);
                        if (mlxnicd_rx_release(dev, 1) != 0) {
                            sample_report_dev_error("mlxnicd_rx_release", dev);
                            goto out;
                        }
                        break;
                    }
                }
                if (mlxnicd_rx_release(dev, 1) != 0) {
                    sample_report_dev_error("mlxnicd_rx_release", dev);
                    goto out;
                }
            }
        }

        rc = 0;
        goto out;
    }

    while (received < opts->packet_count) {
        while (sent < opts->packet_count && inflight < opts->window) {
            struct mlxnicd_pkt tx_batch[MLXNICD_SAMPLE_BENCH_MAX_WINDOW] = {{0}};
            uint8_t tx_frames[MLXNICD_SAMPLE_BENCH_MAX_WINDOW]
                             [MLXNICD_SAMPLE_TX_FRAME_CAPACITY];
            uint64_t send_times[MLXNICD_SAMPLE_BENCH_MAX_WINDOW] = {0};
            uint32_t frame_lens[MLXNICD_SAMPLE_BENCH_MAX_WINDOW] = {0};
            uint32_t batch = 0;

            while (batch < opts->window - inflight &&
                   sent + batch < opts->packet_count) {
                int slot_index = sample_bench_slot_alloc(
                    slots, opts->window, sent + batch);

                if (slot_index < 0) {
                    /* Replies can arrive out of order; wait for this cyclic
                     * slot rather than scanning or overwriting another one. */
                    break;
                }
                send_times[batch] =
                    opts->throughput_only ? 0 : sample_now_ns();
                sample_bench_build_from_template(
                    opts, frame_template, frame_template_len, sent + batch,
                    send_times[batch], 0, tx_frames[batch]);
                frame_lens[batch] = frame_template_len;
                tx_batch[batch].data = tx_frames[batch];
                tx_batch[batch].len = frame_lens[batch];
                slots[slot_index].seq = sent + batch;
                slots[slot_index].send_ns = send_times[batch];
                slots[slot_index].active = 1;
                batch++;
            }
            if (batch == 0) {
                break;
            }
            if (mlxnicd_tx_burst(dev, tx_batch, (uint16_t)batch) != batch) {
                sample_report_dev_error("mlxnicd_tx_burst", dev);
                goto out;
            }
            for (uint32_t i = 0; i < batch; i++) {
                inflight++;
                stats.tx_bytes += frame_lens[i];
                if (stats.tx_first_ns == 0) {
                    stats.tx_first_ns = send_times[i];
                }
                stats.tx_last_ns = send_times[i];
                if (opts->verbose) {
                    fprintf(stdout,
                            "raw-bench: tx seq=%" PRIu32 " len=%" PRIu32
                            " inflight=%" PRIu32 " hdr_seq=%" PRIu32
                            " hdr_flags=0x%04" PRIx16 "\n",
                            sent + i, frame_lens[i], inflight,
                            sample_get_be32(tx_frames[i] + 22),
                            sample_get_be16(tx_frames[i] + 20));
                }
            }
            sent += batch;
        }

        {
            struct mlxnicd_pkt rx_batch[MLXNICD_SAMPLE_BENCH_RX_BURST] = {{0}};
            int poll_timeout_ms =
                (inflight >= opts->window || sent == opts->packet_count)
                    ? (int)opts->timeout_ms
                    : 0;
            uint16_t max_rx = inflight < MLXNICD_SAMPLE_BENCH_RX_BURST
                                  ? (uint16_t)inflight
                                  : MLXNICD_SAMPLE_BENCH_RX_BURST;
            uint16_t got = 0;
            uint16_t rx_queue = rx_queue_cursor;

            for (uint16_t attempt = 0; attempt < opts->queue_count; attempt++) {
                uint16_t q = (uint16_t)((rx_queue_cursor + attempt) %
                                        opts->queue_count);
                int timeout = opts->queue_count == 1 && attempt == 0
                                  ? poll_timeout_ms
                                  : 0;

                got = mlxnicd_rx_burst_q(dev, q, rx_batch, max_rx, timeout);
                if (got != 0) {
                    rx_queue = q;
                    rx_queue_frames[q] += got;
                    rx_queue_cursor = (uint16_t)((q + 1) % opts->queue_count);
                    break;
                }
            }

            if (got == 0) {
                if (opts->queue_count > 1) {
                    continue;
                }
                if (poll_timeout_ms == 0) {
                    continue;
                }
                sample_report_dev_error("mlxnicd_rx_burst", dev);
                goto out;
            }
            for (uint16_t i = 0; i < got; i++) {
                uint32_t seq = 0;
                uint64_t send_ns = 0;
                uint16_t flags = 0;
                uint64_t now_ns =
                    opts->throughput_only ? 0 : sample_now_ns();
                int slot_index;

                rx_pkt = rx_batch[i];
                if (sample_bench_parse_frame(opts, &rx_pkt, &seq, &send_ns,
                                             &flags) != 0) {
                    stats.filtered_frames++;
                    if (opts->verbose) {
                        fprintf(stdout,
                                "raw-bench: filtered len=%" PRIu32 "\n",
                                rx_pkt.len);
                    }
                } else if ((flags & MLXNICD_SAMPLE_BENCH_F_REPLY) == 0 &&
                           !sample_bench_is_l2_reflection(opts, &rx_pkt)) {
                    stats.local_frames++;
                    if (opts->verbose) {
                        fprintf(stdout,
                                "raw-bench: local seq=%" PRIu32
                                " len=%" PRIu32 " flags=0x%04" PRIx16 "\n",
                                seq, rx_pkt.len, flags);
                    }
                } else {
                    slot_index = opts->window == 1
                                     ? sample_bench_slot_find_active(
                                           slots, opts->window)
                                     : sample_bench_slot_find(
                                           slots, opts->window, seq);
                    if (slot_index < 0) {
                        stats.unmatched_frames++;
                        if (opts->verbose) {
                            fprintf(stdout,
                                    "raw-bench: unmatched seq=%" PRIu32
                                    " len=%" PRIu32 " inflight=%" PRIu32
                                    " flags=0x%04" PRIx16 "\n",
                                    seq, rx_pkt.len, inflight, flags);
                        }
                    } else if (!opts->throughput_only &&
                               now_ns < slots[slot_index].send_ns) {
                        stats.unmatched_frames++;
                        if (opts->verbose) {
                            fprintf(stdout,
                                    "raw-bench: unmatched clock seq=%" PRIu32
                                    " send_ns=%" PRIu64 " now_ns=%" PRIu64
                                    " flags=0x%04" PRIx16 "\n",
                                    seq, slots[slot_index].send_ns, now_ns,
                                    flags);
                        }
                    } else {
                        uint64_t rtt_ns = now_ns - slots[slot_index].send_ns;

                        if (!opts->throughput_only &&
                            rtt_ns < opts->min_rtt_ns) {
                            stats.local_frames++;
                            if (opts->verbose) {
                                fprintf(stdout,
                                        "raw-bench: short-rtt seq=%" PRIu32
                                        " len=%" PRIu32 " rtt=%" PRIu64
                                        " ns flags=0x%04" PRIx16 "\n",
                                        seq, rx_pkt.len, rtt_ns, flags);
                            }
                        } else {
                            slots[slot_index].active = 0;
                            inflight--;
                            received++;
                            stats.rx_bytes += rx_pkt.len;
                            if (stats.rx_first_ns == 0) {
                                stats.rx_first_ns = now_ns;
                            }
                            stats.rx_last_ns = now_ns;
                            if (!opts->throughput_only) {
                                sample_bench_stats_note_latency(&stats, rtt_ns);
                            }
                            if (opts->verbose) {
                                fprintf(stdout,
                                        "raw-bench: rx seq=%" PRIu32
                                        " len=%" PRIu32 " rtt=%" PRIu64
                                        " ns inflight=%" PRIu32
                                        " flags=0x%04" PRIx16 "\n",
                                        seq, rx_pkt.len, rtt_ns, inflight,
                                        flags);
                            }
                        }
                    }
                }
            }
            if (mlxnicd_rx_release_q(dev, rx_queue, got) != 0) {
                sample_report_dev_error("mlxnicd_rx_release", dev);
                goto out;
            }
        }
    }

    rc = 0;

out:
    if (rc == 0 && opts->throughput_only && throughput_start_ns != 0) {
        uint64_t end_ns = sample_now_ns();

        stats.tx_first_ns = throughput_start_ns;
        stats.tx_last_ns = end_ns;
        stats.rx_first_ns = throughput_start_ns;
        stats.rx_last_ns = end_ns;
    }
    mlxnicd_dev_close(dev);
    if (rc == 0) {
        uint64_t tx_span_ns = stats.tx_last_ns > stats.tx_first_ns
                                  ? stats.tx_last_ns - stats.tx_first_ns
                                  : 0;
        uint64_t rx_span_ns = stats.rx_last_ns > stats.rx_first_ns
                                  ? stats.rx_last_ns - stats.rx_first_ns
                                  : 0;
        uint64_t end_to_end_ns = stats.rx_last_ns > stats.tx_first_ns
                                     ? stats.rx_last_ns - stats.tx_first_ns
                                     : 0;
        double avg_latency_ns =
            received != 0 ? (double)stats.latency_sum_ns / (double)received : 0.0;

        fprintf(stdout,
                "raw-bench: ok tx=%" PRIu32 " rx=%" PRIu32
                " filtered=%" PRIu64 " local=%" PRIu64
                " unmatched=%" PRIu64 "\n",
                sent, received, stats.filtered_frames, stats.local_frames,
                stats.unmatched_frames);
        fprintf(stdout,
                "raw-bench: latency min=%" PRIu64 " ns avg=%.1f ns max=%" PRIu64
                " ns\n",
                stats.latency_min_ns, avg_latency_ns, stats.latency_max_ns);
        fprintf(stdout,
                "raw-bench: tx rate %.3f Mpps %.3f Gbps\n",
                sample_rate_pps(sent, tx_span_ns) / 1e6,
                sample_rate_gbps(stats.tx_bytes, tx_span_ns));
        fprintf(stdout,
                "raw-bench: rx rate %.3f Mpps %.3f Gbps\n",
                sample_rate_pps(received, rx_span_ns) / 1e6,
                sample_rate_gbps(stats.rx_bytes, rx_span_ns));
        fprintf(stdout,
                "raw-bench: end-to-end %.3f Mpps %.3f Gbps\n",
                sample_rate_pps(received, end_to_end_ns) / 1e6,
                sample_rate_gbps(stats.rx_bytes, end_to_end_ns));
        if (opts->queue_count > 1) {
            for (uint16_t q = 0; q < opts->queue_count; q++) {
                fprintf(stdout, "raw-bench: rxq%u frames=%" PRIu64 "\n", q,
                        rx_queue_frames[q]);
            }
        }
    } else {
        fprintf(stdout,
                "raw-bench: failed tx=%" PRIu32 " rx=%" PRIu32
                " filtered=%" PRIu64 " local=%" PRIu64
                " unmatched=%" PRIu64 "\n",
                sent, received, stats.filtered_frames, stats.local_frames,
                stats.unmatched_frames);
    }
    return rc;
}
