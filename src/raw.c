#include "raw.h"
#include "sample.h"

#include <stdio.h>
#include <string.h>

static int missing(const char *name, const char *value) {
    if (value == NULL || value[0] == '\0') {
        fprintf(stderr, "missing required option: %s\n", name);
        return 1;
    }
    return 0;
}

int raw_loop_run(const struct raw_loop_opts *opts) {
    int bad = 0;

    bad |= missing("--bdf", opts->bdf);
    bad |= missing("--peer-if", opts->peer_if);
    bad |= missing("--src-mac", opts->src_mac);
    bad |= missing("--dst-mac", opts->dst_mac);
    bad |= missing("--ethertype", opts->ethertype);
    if (bad) {
        return -1;
    }

    return sample_raw_loop(opts);
}

int raw_bench_run(const struct raw_bench_opts *opts) {
    int bad = 0;

    bad |= missing("--bdf", opts->bdf);
    bad |= missing("--peer-if", opts->peer_if);
    bad |= missing("--src-mac", opts->src_mac);
    bad |= missing("--dst-mac", opts->dst_mac);
    bad |= missing("--ethertype", opts->ethertype);
    if (opts->packet_count == 0) {
        fprintf(stderr, "--count must be greater than zero\n");
        bad = 1;
    }
    if (opts->window == 0) {
        fprintf(stderr, "--window must be greater than zero\n");
        bad = 1;
    }
    if (opts->rss_udp && strcmp(opts->ethertype, "0x0800") != 0 &&
        strcmp(opts->ethertype, "0x800") != 0) {
        fprintf(stderr, "--rss-udp requires --ethertype 0x0800\n");
        bad = 1;
    }
    if (bad) {
        return -1;
    }

    return sample_raw_bench(opts);
}

int raw_echo_run(const struct raw_echo_opts *opts) {
    int bad = 0;

    bad |= missing("--bdf", opts->bdf);
    bad |= missing("--peer-if", opts->peer_if);
    bad |= missing("--ethertype", opts->ethertype);
    if (opts->packet_count == 0) {
        fprintf(stderr, "--count must be greater than zero\n");
        bad = 1;
    }
    if (opts->timeout_ms == 0) {
        fprintf(stderr, "--timeout-ms must be greater than zero\n");
        bad = 1;
    }
    if (bad) {
        return -1;
    }

    return sample_raw_echo(opts);
}
