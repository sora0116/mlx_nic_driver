#ifndef MLXNICD_API_H
#define MLXNICD_API_H

#include <stdint.h>

#define MLXNICD_DEV_F_TX UINT32_C(0x0001)
#define MLXNICD_DEV_F_RX UINT32_C(0x0002)
#define MLXNICD_DEV_F_PROMISC UINT32_C(0x0004)

enum mlxnicd_error {
    MLXNICD_OK = 0,
    MLXNICD_ERR_INVAL = 1,
    MLXNICD_ERR_STATE = 2,
    MLXNICD_ERR_IO = 3,
    MLXNICD_ERR_TIMEOUT = 4,
    MLXNICD_ERR_UNSUPPORTED = 5,
};

struct mlxnicd_dev;

struct mlxnicd_dev_config {
    /* MLXNICD_DEV_F_* bitmask */
    uint32_t flags;
    /* Number of RX buffers to pre-post at start time. 0 means default. */
    uint32_t rx_post_count;
    /* Number of independent TX/RX queue pairs. 0 means one queue. */
    uint16_t queue_count;
    /* 0: quiet, non-zero: verbose internal logs */
    int log_verbose;
};

struct mlxnicd_pkt {
    /*
     * RX packets point into driver-owned buffers until mlxnicd_rx_release().
     * The caller must not retain this pointer after releasing the packet.
     */
    const uint8_t *data;
    uint32_t len;
};

struct mlxnicd_l2_frame_spec {
    const char *dst_mac;
    const char *src_mac;
    const char *ethertype;
    const char *payload_hex;
};

void mlxnicd_dev_config_init(struct mlxnicd_dev_config *config);
/* Open a device handle for one PCI BDF such as "0000:01:00.0". */
int mlxnicd_dev_open(struct mlxnicd_dev **out, const char *bdf);
/* Copy runtime configuration into the unopened or stopped device. */
int mlxnicd_dev_configure(struct mlxnicd_dev *dev,
                          const struct mlxnicd_dev_config *config);
/* Bring the device from closed/stopped state to datapath-ready state. */
int mlxnicd_dev_start(struct mlxnicd_dev *dev);
/* Tear down datapath objects and return to stopped state. */
void mlxnicd_dev_stop(struct mlxnicd_dev *dev);
/* Stop if needed and free the device handle. */
void mlxnicd_dev_close(struct mlxnicd_dev *dev);
/* Returns one MLXNICD_* status value describing the last API failure. */
int mlxnicd_dev_last_error(const struct mlxnicd_dev *dev);
const char *mlxnicd_strerror(int err);

/* Best-effort burst transmit. Returns number of packets actually posted. */
uint16_t mlxnicd_tx_burst(struct mlxnicd_dev *dev,
                          const struct mlxnicd_pkt *pkts, uint16_t nb_pkts);
/* Queue-specific variants. Each queue must be owned by one calling thread. */
uint16_t mlxnicd_tx_burst_q(struct mlxnicd_dev *dev, uint16_t queue_id,
                            const struct mlxnicd_pkt *pkts,
                            uint16_t nb_pkts);
/* Wait until all submitted TX WQEs on one queue have completed. */
int mlxnicd_tx_flush_q(struct mlxnicd_dev *dev, uint16_t queue_id);
/*
 * Poll for up to nb_pkts packets. RX buffers stay owned by the driver.
 * Each packet returned here increments the number of packets that must later
 * be released with mlxnicd_rx_release().
 */
uint16_t mlxnicd_rx_burst(struct mlxnicd_dev *dev, struct mlxnicd_pkt *pkts,
                          uint16_t nb_pkts, int timeout_ms);
uint16_t mlxnicd_rx_burst_q(struct mlxnicd_dev *dev, uint16_t queue_id,
                            struct mlxnicd_pkt *pkts, uint16_t nb_pkts,
                            int timeout_ms);
/*
 * Release previously received packets and repost RX buffers.
 * nb_pkts must not exceed the number of packets returned by rx_burst() and not
 * yet released.
 */
int mlxnicd_rx_release(struct mlxnicd_dev *dev, uint16_t nb_pkts);
int mlxnicd_rx_release_q(struct mlxnicd_dev *dev, uint16_t queue_id,
                         uint16_t nb_pkts);

/* Convenience helper for building one L2 frame into an inline TX buffer. */
int mlxnicd_frame_build(const struct mlxnicd_l2_frame_spec *spec,
                        uint8_t *frame, uint32_t frame_capacity,
                        uint32_t *frame_len);

#endif
