#ifndef MLXNICD_RAW_H
#define MLXNICD_RAW_H

#include <stdint.h>

struct raw_loop_opts {
    const char *bdf;
    const char *peer_if;
    const char *src_mac;
    const char *dst_mac;
    const char *ethertype;
    const char *payload_hex;
    int verbose;
    uint32_t rx_count;
    uint32_t pre_rx_delay_ms;
    uint32_t timeout_ms;
};

struct raw_bench_opts {
    const char *bdf;
    const char *peer_if;
    const char *src_mac;
    const char *dst_mac;
    const char *ethertype;
    const char *payload_hex;
    uint32_t packet_count;
    uint32_t window;
    uint32_t timeout_ms;
    uint32_t min_rtt_ns;
    uint16_t queue_count;
    int rss_udp;
    int throughput_only;
    int verbose;
};

/* Reflect raw-bench request frames using the same userspace driver. */
struct raw_echo_opts {
    const char *bdf;
    const char *peer_if;
    const char *ethertype;
    uint32_t packet_count;
    uint32_t timeout_ms;
    int verbose;
};

int raw_loop_run(const struct raw_loop_opts *opts);
int raw_bench_run(const struct raw_bench_opts *opts);
int raw_echo_run(const struct raw_echo_opts *opts);

#endif
