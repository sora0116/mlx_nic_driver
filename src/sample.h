#ifndef MLXNICD_SAMPLE_H
#define MLXNICD_SAMPLE_H

#include "mlx5.h"
#include "raw.h"

int sample_mlx5_seq_basic(const char *bdf);
int sample_mlx5_tx_test(const char *bdf, unsigned int count);
int sample_mlx5_tx_test_opts(const char *bdf,
                             const struct mlx5_tx_test_opts *opts);
int sample_mlx5_rx_objects(const char *bdf);
int sample_mlx5_rx_post_test(const char *bdf);
int sample_mlx5_rx_steer_test(const char *bdf);
int sample_mlx5_rx_wait_test(const char *bdf);
int sample_raw_loop(const struct raw_loop_opts *opts);
int sample_raw_bench(const struct raw_bench_opts *opts);
int sample_raw_echo(const struct raw_echo_opts *opts);

#endif
