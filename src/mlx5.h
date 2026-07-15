#ifndef MLXNICD_MLX5_H
#define MLXNICD_MLX5_H

struct mlx5_tx_test_opts {
    const char *dst_mac;
    const char *src_mac;
    const char *ethertype;
    const char *payload_hex;
    unsigned int count;
};

struct raw_loop_opts;

int mlx5_info(const char *bdf);
int mlx5_query_issi(const char *bdf);
int mlx5_set_issi(const char *bdf);
int mlx5_query_hca_cap(const char *bdf);
int mlx5_query_eth_cap(const char *bdf);
int mlx5_enable_hca(const char *bdf);
int mlx5_query_pages(const char *bdf, int boot);

#endif
