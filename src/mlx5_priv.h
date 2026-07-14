#ifndef MLXNICD_MLX5_PRIV_H
#define MLXNICD_MLX5_PRIV_H

#include "vfio.h"

#include <stddef.h>
#include <stdint.h>

#define MLX5_MAX_FW_PAGES 8192

struct mlx5_fw_page {
    void *addr;
    uint64_t iova;
};

struct mlx5_cmd_ctx {
    struct vfio_device dev;
    void *cmdq;
    void *inbox;
    void *outbox;
    uint64_t cmdq_iova;
    uint64_t inbox_iova;
    uint64_t outbox_iova;
    uint32_t cmdq_h_orig;
    uint32_t cmdq_l_sz_orig;
    struct mlx5_fw_page fw_pages[MLX5_MAX_FW_PAGES];
    size_t fw_page_count;
    uint8_t token;
};

int mlx5_cmd_ctx_open(const char *bdf, struct mlx5_cmd_ctx *ctx);
void mlx5_cmd_ctx_close(struct mlx5_cmd_ctx *ctx);

int mlx5_ctx_query_issi(struct mlx5_cmd_ctx *ctx);
int mlx5_ctx_set_issi(struct mlx5_cmd_ctx *ctx);
int mlx5_ctx_enable_hca(struct mlx5_cmd_ctx *ctx);
int mlx5_ctx_query_pages(struct mlx5_cmd_ctx *ctx, int boot);
int mlx5_ctx_query_hca_cap(struct mlx5_cmd_ctx *ctx);

#endif
