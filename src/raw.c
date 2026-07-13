#include "raw.h"
#include "sample.h"

#include <stdio.h>

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
