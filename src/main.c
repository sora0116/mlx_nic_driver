#include "pci.h"
#include "mlx5.h"
#include "raw.h"
#include "sample.h"
#include "vfio.h"

#include <stdio.h>
#include <string.h>

static void usage(FILE *out, const char *argv0) {
    fprintf(out,
            "usage:\n"
            "  %s list\n"
            "  %s inspect <BDF>\n"
            "  %s bar-read <BDF> <bar_index> <offset> [width_bits]\n"
            "  %s vfio-check [BDF]\n"
            "  %s vfio-bind <BDF>\n"
            "  %s vfio-restore <BDF>\n"
            "  %s vfio-probe <BDF>\n"
            "  %s mlx5-info <BDF>\n"
            "  %s mlx5-query-issi <BDF>\n"
            "  %s mlx5-set-issi <BDF>\n"
            "  %s mlx5-enable-hca <BDF>\n"
            "  %s mlx5-query-pages <BDF> <boot|init>\n"
            "  %s mlx5-seq-basic <BDF>\n"
            "  %s mlx5-tx-test <BDF> [--count N] [--dst MAC] [--src MAC] [--ethertype HEX] [--payload-hex HEX]\n"
            "  %s mlx5-rx-objects <BDF>\n"
            "  %s mlx5-rx-post-test <BDF>\n"
            "  %s mlx5-rx-steer-test <BDF>\n"
            "  %s mlx5-rx-wait-test <BDF>\n"
            "  %s mlx5-query-hca-cap <BDF>\n"
            "  %s raw-loop --bdf <BDF> --peer-if <ifname> --src-mac <mac> --dst-mac <mac> --ethertype <hex> [--payload-hex HEX] [--rx-count N] [--pre-rx-delay-ms N] [--timeout-ms N] [--verbose]\n"
            "\n"
            "examples:\n"
            "  %s list\n"
            "  %s inspect 0000:01:00.0\n"
            "  %s bar-read 0000:01:00.0 0 0x0 32\n",
            argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0,
            argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0,
            argv0,
            argv0, argv0, argv0, argv0);
}

static const char *opt_value(int argc, char **argv, const char *name) {
    int i;
    for (i = 2; i + 1 < argc; i++) {
        if (strcmp(argv[i], name) == 0) {
            return argv[i + 1];
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(stderr, argv[0]);
        return 2;
    }

    if (strcmp(argv[1], "list") == 0) {
        if (argc != 2) {
            usage(stderr, argv[0]);
            return 2;
        }
        return mlx_list_devices() == 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "inspect") == 0) {
        if (argc != 3) {
            usage(stderr, argv[0]);
            return 2;
        }
        return mlx_inspect_device(argv[2]) == 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "bar-read") == 0) {
        uint32_t bar = 0;
        uint64_t offset = 0;
        uint32_t width = 32;

        if (argc != 5 && argc != 6) {
            usage(stderr, argv[0]);
            return 2;
        }
        if (parse_u32(argv[3], &bar) != 0) {
            fprintf(stderr, "invalid bar_index: %s\n", argv[3]);
            return 2;
        }
        if (parse_u64(argv[4], &offset) != 0) {
            fprintf(stderr, "invalid offset: %s\n", argv[4]);
            return 2;
        }
        if (argc == 6 && parse_u32(argv[5], &width) != 0) {
            fprintf(stderr, "invalid width_bits: %s\n", argv[5]);
            return 2;
        }
        return mlx_bar_read(argv[2], bar, offset, width) == 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "vfio-check") == 0) {
        if (argc != 2 && argc != 3) {
            usage(stderr, argv[0]);
            return 2;
        }
        return vfio_check(argc == 3 ? argv[2] : NULL) == 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "vfio-bind") == 0) {
        if (argc != 3) {
            usage(stderr, argv[0]);
            return 2;
        }
        return vfio_bind(argv[2]) == 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "vfio-restore") == 0) {
        if (argc != 3) {
            usage(stderr, argv[0]);
            return 2;
        }
        return vfio_restore(argv[2]) == 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "vfio-probe") == 0) {
        if (argc != 3) {
            usage(stderr, argv[0]);
            return 2;
        }
        return vfio_probe(argv[2]) == 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "mlx5-info") == 0) {
        if (argc != 3) {
            usage(stderr, argv[0]);
            return 2;
        }
        return mlx5_info(argv[2]) == 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "mlx5-query-issi") == 0) {
        if (argc != 3) {
            usage(stderr, argv[0]);
            return 2;
        }
        return mlx5_query_issi(argv[2]) == 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "mlx5-set-issi") == 0) {
        if (argc != 3) {
            usage(stderr, argv[0]);
            return 2;
        }
        return mlx5_set_issi(argv[2]) == 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "mlx5-enable-hca") == 0) {
        if (argc != 3) {
            usage(stderr, argv[0]);
            return 2;
        }
        return mlx5_enable_hca(argv[2]) == 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "mlx5-query-pages") == 0) {
        if (argc != 4) {
            usage(stderr, argv[0]);
            return 2;
        }
        if (strcmp(argv[3], "boot") != 0 && strcmp(argv[3], "init") != 0) {
            fprintf(stderr, "expected boot or init, got: %s\n", argv[3]);
            return 2;
        }
        return mlx5_query_pages(argv[2], strcmp(argv[3], "boot") == 0) == 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "mlx5-seq-basic") == 0) {
        if (argc != 3) {
            usage(stderr, argv[0]);
            return 2;
        }
        return sample_mlx5_seq_basic(argv[2]) == 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "mlx5-tx-test") == 0) {
        struct mlx5_tx_test_opts opts;
        uint32_t count = 1;
        const char *count_s;

        if (argc < 3) {
            usage(stderr, argv[0]);
            return 2;
        }
        memset(&opts, 0, sizeof(opts));
        count_s = opt_value(argc, argv, "--count");
        if (count_s != NULL && parse_u32(count_s, &count) != 0) {
            fprintf(stderr, "invalid --count: %s\n", count_s);
            return 2;
        }
        if (count == 0) {
            fprintf(stderr, "--count must be greater than zero\n");
            return 2;
        }
        opts.count = count;
        opts.dst_mac = opt_value(argc, argv, "--dst");
        opts.src_mac = opt_value(argc, argv, "--src");
        opts.ethertype = opt_value(argc, argv, "--ethertype");
        opts.payload_hex = opt_value(argc, argv, "--payload-hex");
        return sample_mlx5_tx_test_opts(argv[2], &opts) == 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "mlx5-query-hca-cap") == 0) {
        if (argc != 3) {
            usage(stderr, argv[0]);
            return 2;
        }
        return mlx5_query_hca_cap(argv[2]) == 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "mlx5-rx-objects") == 0) {
        if (argc != 3) {
            usage(stderr, argv[0]);
            return 2;
        }
        return sample_mlx5_rx_objects(argv[2]) == 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "mlx5-rx-post-test") == 0) {
        if (argc != 3) {
            usage(stderr, argv[0]);
            return 2;
        }
        return sample_mlx5_rx_post_test(argv[2]) == 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "mlx5-rx-steer-test") == 0) {
        if (argc != 3) {
            usage(stderr, argv[0]);
            return 2;
        }
        return sample_mlx5_rx_steer_test(argv[2]) == 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "mlx5-rx-wait-test") == 0) {
        if (argc != 3) {
            usage(stderr, argv[0]);
            return 2;
        }
        return sample_mlx5_rx_wait_test(argv[2]) == 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "raw-loop") == 0) {
        struct raw_loop_opts opts;
        memset(&opts, 0, sizeof(opts));
        opts.bdf = opt_value(argc, argv, "--bdf");
        opts.peer_if = opt_value(argc, argv, "--peer-if");
        opts.src_mac = opt_value(argc, argv, "--src-mac");
        opts.dst_mac = opt_value(argc, argv, "--dst-mac");
        opts.ethertype = opt_value(argc, argv, "--ethertype");
        opts.payload_hex = opt_value(argc, argv, "--payload-hex");
        opts.verbose = opt_value(argc, argv, "--verbose") != NULL;
        {
            const char *rx_count_s = opt_value(argc, argv, "--rx-count");
            const char *pre_rx_delay_ms_s =
                opt_value(argc, argv, "--pre-rx-delay-ms");
            const char *timeout_ms_s = opt_value(argc, argv, "--timeout-ms");

            if (rx_count_s != NULL && parse_u32(rx_count_s, &opts.rx_count) != 0) {
                fprintf(stderr, "invalid --rx-count: %s\n", rx_count_s);
                return 2;
            }
            if (pre_rx_delay_ms_s != NULL &&
                parse_u32(pre_rx_delay_ms_s, &opts.pre_rx_delay_ms) != 0) {
                fprintf(stderr, "invalid --pre-rx-delay-ms: %s\n",
                        pre_rx_delay_ms_s);
                return 2;
            }
            if (timeout_ms_s != NULL &&
                parse_u32(timeout_ms_s, &opts.timeout_ms) != 0) {
                fprintf(stderr, "invalid --timeout-ms: %s\n", timeout_ms_s);
                return 2;
            }
        }
        return raw_loop_run(&opts) == 0 ? 0 : 1;
    }

    usage(stderr, argv[0]);
    return 2;
}
