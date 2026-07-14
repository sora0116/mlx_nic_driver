#include "mlxnicd.h"

#include <stdint.h>
#include <stdio.h>

int main(void) {
    struct mlxnicd_dev *dev = NULL;
    struct mlxnicd_dev_config cfg;
    struct mlxnicd_l2_frame_spec spec = {
        .dst_mac = "ff:ff:ff:ff:ff:ff",
        .src_mac = "02:00:00:00:00:06",
        .ethertype = "0x88b5",
        .payload_hex = "01020304aabbccdd",
    };
    struct mlxnicd_pkt tx_pkt;
    struct mlxnicd_pkt rx_pkt;
    uint8_t frame[98];
    uint32_t frame_len = 0;
    int rc = 1;

    mlxnicd_dev_config_init(&cfg);
    cfg.flags = MLXNICD_DEV_F_TX | MLXNICD_DEV_F_RX | MLXNICD_DEV_F_PROMISC;

    if (mlxnicd_frame_build(&spec, frame, sizeof(frame), &frame_len) != 0) {
        fprintf(stderr, "frame build failed\n");
        return 1;
    }
    if (mlxnicd_dev_open(&dev, "0000:01:00.0") != 0) {
        fprintf(stderr, "dev open failed\n");
        return 1;
    }
    if (mlxnicd_dev_configure(dev, &cfg) != 0) {
        fprintf(stderr, "dev configure failed: %s\n",
                mlxnicd_strerror(mlxnicd_dev_last_error(dev)));
        goto out;
    }
    if (mlxnicd_dev_start(dev) != 0) {
        fprintf(stderr, "dev start failed: %s\n",
                mlxnicd_strerror(mlxnicd_dev_last_error(dev)));
        goto out;
    }

    tx_pkt.data = frame;
    tx_pkt.len = frame_len;
    if (mlxnicd_tx_burst(dev, &tx_pkt, 1) != 1) {
        fprintf(stderr, "tx burst failed: %s\n",
                mlxnicd_strerror(mlxnicd_dev_last_error(dev)));
        goto out;
    }

    if (mlxnicd_rx_burst(dev, &rx_pkt, 1, 10000) != 1) {
        fprintf(stderr, "rx burst failed: %s\n",
                mlxnicd_strerror(mlxnicd_dev_last_error(dev)));
        goto out;
    }

    printf("received one packet: len=%u\n", rx_pkt.len);
    if (mlxnicd_rx_release(dev, 1) != 0) {
        fprintf(stderr, "rx release failed: %s\n",
                mlxnicd_strerror(mlxnicd_dev_last_error(dev)));
        goto out;
    }
    rc = 0;

out:
    mlxnicd_dev_close(dev);
    return rc;
}
