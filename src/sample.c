#include "sample.h"
#include "mlxnicd.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum {
    MLXNICD_SAMPLE_TX_FRAME_CAPACITY = 98,
    MLXNICD_SAMPLE_RX_BUFFER_SIZE = 2048,
    MLXNICD_SAMPLE_RX_WAIT_COUNT = 20,
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
