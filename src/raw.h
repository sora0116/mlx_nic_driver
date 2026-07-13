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

int raw_loop_run(const struct raw_loop_opts *opts);

#endif
