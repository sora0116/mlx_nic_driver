#include "sample.h"
#include "mlxnicd.h"

#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum {
    MLXNICD_SAMPLE_TX_FRAME_CAPACITY = 98,
    MLXNICD_SAMPLE_RX_BUFFER_SIZE = 2048,
    MLXNICD_SAMPLE_RX_WAIT_COUNT = 20,
    MLXNICD_SAMPLE_BENCH_MAGIC = 0x4d4c5842,
    MLXNICD_SAMPLE_BENCH_HDR_LEN = 24,
    /* A full-inline frame consumes two 64-byte SQ WQEBBs; SQ has 16. */
    MLXNICD_SAMPLE_BENCH_MAX_WINDOW = 8,
    MLXNICD_SAMPLE_BENCH_RX_POST_COUNT = 16,
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

static void sample_report_dev_error(const char *what, struct mlxnicd_dev *dev) {
    int err = mlxnicd_dev_last_error(dev);

    fprintf(stderr, "%s: %s (%d)\n", what, mlxnicd_strerror(err), err);
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

static int sample_bench_build_frame(const struct raw_bench_opts *opts,
                                    uint32_t seq, uint64_t send_ns,
                                    uint16_t flags,
                                    uint8_t *frame, size_t frame_cap,
                                    uint32_t *frame_len) {
    uint8_t dst[6];
    uint8_t src[6];
    uint8_t extra_payload[MLXNICD_SAMPLE_TX_FRAME_CAPACITY - 14 -
                          MLXNICD_SAMPLE_BENCH_HDR_LEN];
    size_t extra_payload_len = 0;
    uint16_t ethertype = 0;
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
    if (sample_parse_payload_hex(opts->payload_hex, extra_payload,
                                 sizeof(extra_payload),
                                 &extra_payload_len) != 0) {
        fprintf(stderr,
                "invalid --payload-hex or too large; max payload is %zu bytes\n",
                sizeof(extra_payload));
        return -1;
    }

    len = 14 + MLXNICD_SAMPLE_BENCH_HDR_LEN + extra_payload_len;
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
    sample_put_be32(frame + 14, MLXNICD_SAMPLE_BENCH_MAGIC);
    sample_put_be16(frame + 18, 1);
    sample_put_be16(frame + 20, flags);
    sample_put_be32(frame + 22, seq);
    sample_put_be64(frame + 26, send_ns);
    sample_put_be32(frame + 34, (uint32_t)extra_payload_len);
    memcpy(frame + 38, extra_payload, extra_payload_len);
    *frame_len = (uint32_t)len;
    return 0;
}

static int sample_bench_parse_frame(const struct raw_bench_opts *opts,
                                    const struct mlxnicd_pkt *pkt,
                                    uint32_t *seq, uint64_t *send_ns,
                                    uint16_t *flags) {
    uint16_t want_ethertype = 0;
    uint16_t version;

    if (opts == NULL || pkt == NULL || seq == NULL || send_ns == NULL ||
        flags == NULL) {
        return -1;
    }
    if (pkt->len < 14 + MLXNICD_SAMPLE_BENCH_HDR_LEN) {
        return -1;
    }
    if (sample_parse_ethertype16(opts->ethertype, &want_ethertype) != 0) {
        return -1;
    }
    if (sample_get_be16(pkt->data + 12) != want_ethertype) {
        return -1;
    }
    if (sample_get_be32(pkt->data + 14) != MLXNICD_SAMPLE_BENCH_MAGIC) {
        return -1;
    }
    version = sample_get_be16(pkt->data + 18);
    if (version != 1) {
        return -1;
    }
    *flags = sample_get_be16(pkt->data + 20);
    *seq = sample_get_be32(pkt->data + 22);
    *send_ns = sample_get_be64(pkt->data + 26);
    return 0;
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

    for (i = 0; i < slot_count; i++) {
        if (slots[i].active && slots[i].seq == seq) {
            return (int)i;
        }
    }
    return -1;
}

static int sample_bench_slot_alloc(struct sample_bench_slot *slots,
                                   uint32_t slot_count) {
    uint32_t i;

    for (i = 0; i < slot_count; i++) {
        if (!slots[i].active) {
            return (int)i;
        }
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

int sample_raw_bench(const struct raw_bench_opts *opts) {
    struct mlxnicd_dev *dev = NULL;
    struct mlxnicd_dev_config config;
    struct mlxnicd_pkt tx_pkt = {0};
    struct mlxnicd_pkt rx_pkt = {0};
    struct sample_bench_slot slots[MLXNICD_SAMPLE_BENCH_MAX_WINDOW] = {{0}};
    struct sample_bench_stats stats = {0};
    uint8_t frame[MLXNICD_SAMPLE_TX_FRAME_CAPACITY];
    uint32_t sent = 0;
    uint32_t received = 0;
    uint32_t inflight = 0;
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

    mlxnicd_dev_config_init(&config);
    config.flags = MLXNICD_DEV_F_TX | MLXNICD_DEV_F_RX | MLXNICD_DEV_F_PROMISC;
    config.rx_post_count = MLXNICD_SAMPLE_BENCH_RX_POST_COUNT;
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

    if (opts->window == 1) {
        while (sent < opts->packet_count) {
            uint64_t send_ns = sample_now_ns();
            uint32_t frame_len = 0;

            if (sample_bench_build_frame(opts, sent, send_ns, 0, frame,
                                         sizeof(frame), &frame_len) != 0) {
                goto out;
            }
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
                } else if ((flags & MLXNICD_SAMPLE_BENCH_F_REPLY) == 0) {
                    stats.local_frames++;
                } else {
                    uint64_t rtt_ns;

                    (void)seq;
                    (void)pkt_send_ns;
                    now_ns = sample_now_ns();
                    rtt_ns = now_ns >= send_ns ? now_ns - send_ns : 0;
                    if (rtt_ns < opts->min_rtt_ns) {
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
            int slot_index = sample_bench_slot_alloc(slots, opts->window);
            uint64_t send_ns = sample_now_ns();
            uint32_t frame_len = 0;

            if (slot_index < 0) {
                fprintf(stderr, "raw-bench: no free inflight slot\n");
                goto out;
            }
            if (sample_bench_build_frame(opts, sent, send_ns, 0, frame,
                                         sizeof(frame), &frame_len) != 0) {
                goto out;
            }
            tx_pkt.data = frame;
            tx_pkt.len = frame_len;
            if (mlxnicd_tx_burst(dev, &tx_pkt, 1) != 1) {
                sample_report_dev_error("mlxnicd_tx_burst", dev);
                goto out;
            }
            slots[slot_index].seq = sent;
            slots[slot_index].send_ns = send_ns;
            slots[slot_index].active = 1;
            inflight++;
            stats.tx_bytes += frame_len;
            if (stats.tx_first_ns == 0) {
                stats.tx_first_ns = send_ns;
            }
            stats.tx_last_ns = send_ns;
            if (opts->verbose) {
                fprintf(stdout,
                        "raw-bench: tx seq=%" PRIu32 " len=%" PRIu32
                        " inflight=%" PRIu32 " hdr_seq=%" PRIu32
                        " hdr_flags=0x%04" PRIx16 "\n",
                        sent, frame_len, inflight,
                        sample_get_be32(frame + 22),
                        sample_get_be16(frame + 20));
            }
            sent++;
        }

        {
            int poll_timeout_ms =
                (inflight >= opts->window || sent == opts->packet_count)
                    ? (int)opts->timeout_ms
                    : 0;
            uint16_t got = mlxnicd_rx_burst(dev, &rx_pkt, 1, poll_timeout_ms);

            if (got != 1) {
                if (poll_timeout_ms == 0) {
                    continue;
                }
                sample_report_dev_error("mlxnicd_rx_burst", dev);
                goto out;
            }
        }

        {
            uint32_t seq = 0;
            uint64_t send_ns = 0;
            uint16_t flags = 0;
            uint64_t now_ns = sample_now_ns();
            int slot_index;

            if (sample_bench_parse_frame(opts, &rx_pkt, &seq, &send_ns,
                                         &flags) != 0) {
                stats.filtered_frames++;
                if (opts->verbose) {
                    fprintf(stdout,
                            "raw-bench: filtered len=%" PRIu32 "\n",
                            rx_pkt.len);
                }
            } else {
                if ((flags & MLXNICD_SAMPLE_BENCH_F_REPLY) == 0) {
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
                    } else if (now_ns < slots[slot_index].send_ns) {
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

                        if (rtt_ns < opts->min_rtt_ns) {
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
                            sample_bench_stats_note_latency(&stats, rtt_ns);
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
