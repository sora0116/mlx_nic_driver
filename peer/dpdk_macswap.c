/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Minimal single-port DPDK peer for mlxnicd raw-bench.
 *
 * One worker owns one RX/TX queue pair.  It receives a burst, swaps the
 * Ethernet source and destination addresses in-place, and transmits the same
 * mbufs.  This intentionally avoids testpmd's command, forwarding-stream,
 * and statistics machinery when measuring the custom driver's datapath.
 */
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rte_eal.h>
#include <rte_cycles.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

#define PEER_MAX_QUEUES 8
#define PEER_DEFAULT_QUEUES 4
#define PEER_DEFAULT_BURST 128
#define PEER_DEFAULT_DESC 1024
#define PEER_MBUF_CACHE 256

struct peer_config {
    uint16_t port_id;
    uint16_t queues;
    uint16_t burst;
    uint16_t rxd;
    uint16_t txd;
};

struct peer_worker {
    const struct peer_config *config;
    uint16_t queue_id;
    uint64_t rx_packets;
    uint64_t tx_packets;
};

static volatile sig_atomic_t peer_keep_running = 1;

static void peer_signal_handler(int signum) {
    (void)signum;
    peer_keep_running = 0;
}

static void peer_usage(const char *program) {
    fprintf(stderr,
            "usage: %s <DPDK EAL options> -- [--queues N] [--burst N] "
            "[--rxd N] [--txd N]\n"
            "\n"
            "Example:\n"
            "  %s -l 1,2,3,4,5 -n 4 -a 0000:01:00.1 -- "
            "--queues 4 --burst 128\n",
            program, program);
}

static int peer_parse_u16(const char *text, uint16_t min, uint16_t max,
                          uint16_t *value) {
    char *end = NULL;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || parsed < min ||
        parsed > max)
        return -1;
    *value = (uint16_t)parsed;
    return 0;
}

static int peer_parse_args(int argc, char **argv, struct peer_config *config) {
    static const struct option options[] = {
        {"queues", required_argument, NULL, 'q'},
        {"burst", required_argument, NULL, 'b'},
        {"rxd", required_argument, NULL, 'r'},
        {"txd", required_argument, NULL, 't'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    int option;

    if (argc <= 1)
        return 0;
    optind = 1;
    while ((option = getopt_long(argc, argv, "q:b:r:t:h", options, NULL)) !=
           -1) {
        int rc = 0;

        switch (option) {
        case 'q':
            rc = peer_parse_u16(optarg, 1, PEER_MAX_QUEUES, &config->queues);
            break;
        case 'b':
            rc = peer_parse_u16(optarg, 1, RTE_MAX_ETHPORTS,
                                &config->burst);
            break;
        case 'r':
            rc = peer_parse_u16(optarg, 64, UINT16_MAX, &config->rxd);
            break;
        case 't':
            rc = peer_parse_u16(optarg, 64, UINT16_MAX, &config->txd);
            break;
        case 'h':
            peer_usage(argv[0]);
            return 1;
        default:
            return -1;
        }
        if (rc != 0)
            return -1;
    }
    return optind == argc ? 0 : -1;
}

static void peer_swap_macs(struct rte_mbuf *packet) {
    struct rte_ether_hdr *ether;
    struct rte_ether_addr temporary;

    ether = rte_pktmbuf_mtod(packet, struct rte_ether_hdr *);
    temporary = ether->dst_addr;
    ether->dst_addr = ether->src_addr;
    ether->src_addr = temporary;
}

static int peer_worker_main(void *argument) {
    struct peer_worker *worker = argument;
    const struct peer_config *config = worker->config;
    struct rte_mbuf *packets[RTE_MAX_ETHPORTS];

    while (peer_keep_running) {
        uint16_t received;
        uint16_t transmitted;

        received = rte_eth_rx_burst(config->port_id, worker->queue_id, packets,
                                    config->burst);
        if (received == 0)
            continue;
        for (uint16_t i = 0; i < received; i++)
            peer_swap_macs(packets[i]);
        transmitted = rte_eth_tx_burst(config->port_id, worker->queue_id,
                                       packets, received);
        for (uint16_t i = transmitted; i < received; i++)
            rte_pktmbuf_free(packets[i]);
        worker->rx_packets += received;
        worker->tx_packets += transmitted;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *program = argv[0];
    struct peer_config config = {
        .port_id = 0,
        .queues = PEER_DEFAULT_QUEUES,
        .burst = PEER_DEFAULT_BURST,
        .rxd = PEER_DEFAULT_DESC,
        .txd = PEER_DEFAULT_DESC,
    };
    struct rte_eth_conf port_conf = {0};
    struct rte_eth_dev_info dev_info;
    struct rte_eth_rxconf rx_conf;
    struct rte_eth_txconf tx_conf;
    struct rte_mempool *pool;
    struct peer_worker workers[PEER_MAX_QUEUES] = {0};
    uint16_t available_ports;
    uint16_t rxd;
    uint16_t txd;
    unsigned worker_count = 0;
    unsigned lcore_id;
    int eal_args;
    int rc = EXIT_FAILURE;

    eal_args = rte_eal_init(argc, argv);
    if (eal_args < 0)
        rte_exit(EXIT_FAILURE, "DPDK EAL initialization failed\n");
    argc -= eal_args;
    argv += eal_args;
    /* DPDK 25.11 leaves the conventional EAL/application separator here. */
    if (argc > 0 && strcmp(argv[0], "--") == 0) {
        argc--;
        argv++;
    }
    if (peer_parse_args(argc, argv, &config) != 0) {
        peer_usage(program);
        return EXIT_FAILURE;
    }
    if (rte_lcore_count() < (unsigned)(config.queues + 1))
        rte_exit(EXIT_FAILURE, "need main lcore plus %u worker lcores\n",
                 config.queues);

    available_ports = rte_eth_dev_count_avail();
    if (available_ports != 1)
        rte_exit(EXIT_FAILURE, "expected exactly one allowed port, found %u\n",
                 available_ports);

    pool = rte_pktmbuf_pool_create("peer_mbuf_pool",
                                   config.queues * (config.rxd + config.txd) +
                                       8192,
                                   PEER_MBUF_CACHE, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
                                   rte_socket_id());
    if (pool == NULL)
        rte_exit(EXIT_FAILURE, "mbuf pool creation failed: %s\n",
                 rte_strerror(rte_errno));

    memset(&dev_info, 0, sizeof(dev_info));
    if (rte_eth_dev_info_get(config.port_id, &dev_info) != 0)
        rte_exit(EXIT_FAILURE, "cannot query port %u\n", config.port_id);
    port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
    port_conf.rx_adv_conf.rss_conf.rss_hf =
        (RTE_ETH_RSS_IP | RTE_ETH_RSS_UDP) & dev_info.flow_type_rss_offloads;
    if (port_conf.rx_adv_conf.rss_conf.rss_hf == 0)
        rte_exit(EXIT_FAILURE, "port does not support IPv4/UDP RSS\n");
    if (rte_eth_dev_configure(config.port_id, config.queues, config.queues,
                              &port_conf) != 0)
        rte_exit(EXIT_FAILURE, "port configuration failed\n");

    rxd = config.rxd;
    txd = config.txd;
    if (rte_eth_dev_adjust_nb_rx_tx_desc(config.port_id, &rxd, &txd) != 0)
        rte_exit(EXIT_FAILURE, "descriptor adjustment failed\n");
    rx_conf = dev_info.default_rxconf;
    tx_conf = dev_info.default_txconf;
    for (uint16_t queue = 0; queue < config.queues; queue++) {
        if (rte_eth_rx_queue_setup(config.port_id, queue, rxd,
                                   rte_eth_dev_socket_id(config.port_id),
                                   &rx_conf, pool) != 0 ||
            rte_eth_tx_queue_setup(config.port_id, queue, txd,
                                   rte_eth_dev_socket_id(config.port_id),
                                   &tx_conf) != 0)
            rte_exit(EXIT_FAILURE, "queue %u setup failed\n", queue);
    }
    if (rte_eth_dev_start(config.port_id) != 0)
        rte_exit(EXIT_FAILURE, "port start failed\n");
    rte_eth_promiscuous_enable(config.port_id);

    signal(SIGINT, peer_signal_handler);
    signal(SIGTERM, peer_signal_handler);
    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        if (worker_count == config.queues)
            break;
        workers[worker_count].config = &config;
        workers[worker_count].queue_id = (uint16_t)worker_count;
        if (rte_eal_remote_launch(peer_worker_main, &workers[worker_count],
                                  lcore_id) != 0)
            rte_exit(EXIT_FAILURE, "worker launch failed\n");
        worker_count++;
    }
    if (worker_count != config.queues)
        rte_exit(EXIT_FAILURE, "only launched %u/%u workers\n", worker_count,
                 config.queues);

    fprintf(stdout,
            "dpdk-macswap-peer: port=%u queues=%u burst=%u rxd=%u txd=%u\n",
            config.port_id, config.queues, config.burst, rxd, txd);
    while (peer_keep_running)
        rte_delay_us_sleep(100000);

    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        if (rte_eal_wait_lcore(lcore_id) < 0)
            rc = EXIT_FAILURE;
    }
    for (uint16_t queue = 0; queue < config.queues; queue++)
        fprintf(stdout, "queue%u rx=%" PRIu64 " tx=%" PRIu64 "\n", queue,
                workers[queue].rx_packets, workers[queue].tx_packets);
    rte_eth_dev_stop(config.port_id);
    rte_eth_dev_close(config.port_id);
    if (rc == EXIT_FAILURE)
        return rc;
    return EXIT_SUCCESS;
}
