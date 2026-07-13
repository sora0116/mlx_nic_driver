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
int mlx5_enable_hca(const char *bdf);
int mlx5_query_pages(const char *bdf, int boot);
int mlx5_seq_basic(const char *bdf);
int mlx5_tx_test(const char *bdf, unsigned int count);
int mlx5_tx_test_opts(const char *bdf, const struct mlx5_tx_test_opts *opts);
int mlx5_rx_objects(const char *bdf);
int mlx5_rx_post_test(const char *bdf);
int mlx5_rx_steer_test(const char *bdf);
int mlx5_rx_wait_test(const char *bdf);
int mlx5_raw_loop(const struct raw_loop_opts *opts);

#endif
