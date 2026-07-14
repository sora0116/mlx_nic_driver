#include "mlx5.h"
#include "mlx5_priv.h"

#include <inttypes.h>
#include <stdio.h>

enum {
    MLX5_INIT_FW_REV = 0x000,
    MLX5_INIT_CMDIF_REV_FW_SUB = 0x004,
    MLX5_INIT_CMDQ_ADDR_H = 0x010,
    MLX5_INIT_CMDQ_ADDR_L_SZ = 0x014,
    MLX5_INIT_CMD_DBELL = 0x018,
    MLX5_INIT_INITIALIZING = 0x1fc,
    MLX5_INIT_HEALTH_COUNTER = 0x101c,
};

static uint32_t mlx5_debug_mmio_read32_be(void *base, uint64_t off) {
    volatile uint8_t *p = (volatile uint8_t *)base + off;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static unsigned int mlx5_debug_cmdq_log_size(uint32_t v) {
    return v & 0x1fu;
}

static unsigned int mlx5_debug_cmdq_log_stride(uint32_t v) {
    return (v >> 5) & 0x07u;
}

static unsigned int mlx5_debug_cmdq_nic_interface(uint32_t v) {
    return (v >> 8) & 0x0fu;
}

static void mlx5_debug_print_cmdq_fields(const char *prefix, uint32_t h,
                                         uint32_t l) {
    printf("%s: h=0x%08" PRIx32 " l=0x%08" PRIx32
           " addr=0x%08" PRIx32 "%05" PRIx32
           " nic_interface=%u log_size=%u log_stride=%u\n",
           prefix, h, l, h, (l >> 12) & 0xfffffu,
           mlx5_debug_cmdq_nic_interface(l), mlx5_debug_cmdq_log_size(l),
           mlx5_debug_cmdq_log_stride(l));
}

int mlx5_info(const char *bdf) {
    struct vfio_device dev;
    int rc = vfio_device_open(bdf, &dev);
    uint32_t fw_rev;
    uint32_t cmdif;
    uint32_t cmdq_h;
    uint32_t cmdq_l_sz;
    uint32_t initializing;
    uint32_t health_counter;

    if (rc != 0) {
        return rc;
    }
    fw_rev = mlx5_debug_mmio_read32_be(dev.bar0, MLX5_INIT_FW_REV);
    cmdif = mlx5_debug_mmio_read32_be(dev.bar0, MLX5_INIT_CMDIF_REV_FW_SUB);
    cmdq_h = mlx5_debug_mmio_read32_be(dev.bar0, MLX5_INIT_CMDQ_ADDR_H);
    cmdq_l_sz =
        mlx5_debug_mmio_read32_be(dev.bar0, MLX5_INIT_CMDQ_ADDR_L_SZ);
    initializing =
        mlx5_debug_mmio_read32_be(dev.bar0, MLX5_INIT_INITIALIZING);
    health_counter =
        mlx5_debug_mmio_read32_be(dev.bar0, MLX5_INIT_HEALTH_COUNTER);

    printf("mlx5-info ok\n");
    printf("  bdf: %s\n", bdf);
    printf("  BAR0 size: 0x%" PRIx64 "\n", dev.bar0_size);
    printf("  fw_rev raw: 0x%08" PRIx32 "\n", fw_rev);
    printf("  cmdif_rev_fw_sub raw: 0x%08" PRIx32 "\n", cmdif);
    printf("  cmdif_rev: %u\n", cmdif >> 16);
    printf("  fw_sub: %u\n", cmdif & 0xffff);
    printf("  cmdq_addr_h: 0x%08" PRIx32 "\n", cmdq_h);
    printf("  cmdq_addr_l_sz: 0x%08" PRIx32 "\n", cmdq_l_sz);
    printf("  log_cmdq_size: %u\n", mlx5_debug_cmdq_log_size(cmdq_l_sz));
    printf("  log_cmdq_stride: %u\n", mlx5_debug_cmdq_log_stride(cmdq_l_sz));
    printf("  nic_interface: %u\n",
           mlx5_debug_cmdq_nic_interface(cmdq_l_sz));
    printf("  cmd_dbell raw: 0x%08" PRIx32 "\n",
           mlx5_debug_mmio_read32_be(dev.bar0, MLX5_INIT_CMD_DBELL));
    printf("  initializing raw: 0x%08" PRIx32 "\n", initializing);
    printf("  initializing bit31: %u\n", initializing >> 31);
    printf("  health_counter: %" PRIu32 "\n", health_counter);
    mlx5_debug_print_cmdq_fields("cmdq decoded", cmdq_h, cmdq_l_sz);
    vfio_device_close(&dev);
    return 0;
}

int mlx5_query_issi(const char *bdf) {
    struct mlx5_cmd_ctx ctx;
    int rc;

    if (mlx5_cmd_ctx_open(bdf, &ctx) != 0) {
        return -1;
    }
    rc = mlx5_ctx_query_issi(&ctx);
    mlx5_cmd_ctx_close(&ctx);
    return rc;
}

int mlx5_set_issi(const char *bdf) {
    struct mlx5_cmd_ctx ctx;
    int rc;

    if (mlx5_cmd_ctx_open(bdf, &ctx) != 0) {
        return -1;
    }
    rc = mlx5_ctx_set_issi(&ctx);
    mlx5_cmd_ctx_close(&ctx);
    return rc;
}

int mlx5_enable_hca(const char *bdf) {
    struct mlx5_cmd_ctx ctx;
    int rc;

    if (mlx5_cmd_ctx_open(bdf, &ctx) != 0) {
        return -1;
    }
    rc = mlx5_ctx_enable_hca(&ctx);
    mlx5_cmd_ctx_close(&ctx);
    return rc;
}

int mlx5_query_pages(const char *bdf, int boot) {
    struct mlx5_cmd_ctx ctx;
    int rc;

    if (mlx5_cmd_ctx_open(bdf, &ctx) != 0) {
        return -1;
    }
    rc = mlx5_ctx_query_pages(&ctx, boot);
    mlx5_cmd_ctx_close(&ctx);
    return rc;
}

int mlx5_query_hca_cap(const char *bdf) {
    struct mlx5_cmd_ctx ctx;
    int rc;

    if (mlx5_cmd_ctx_open(bdf, &ctx) != 0) {
        return -1;
    }
    rc = mlx5_ctx_query_hca_cap(&ctx);
    mlx5_cmd_ctx_close(&ctx);
    return rc;
}
