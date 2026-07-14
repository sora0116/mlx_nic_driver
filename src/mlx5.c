#include "mlxnicd.h"
#include "mlx5.h"
#include "mlx5_priv.h"
#include "vfio.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

static int g_mlx5_stdout_quiet = 0;

static int mlx5_printf(const char *fmt, ...) {
    int rc;
    va_list ap;

    if (g_mlx5_stdout_quiet) {
        return 0;
    }
    va_start(ap, fmt);
    rc = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return rc;
}

#define printf(...) mlx5_printf(__VA_ARGS__)

enum {
    MLX5_INIT_FW_REV = 0x000,
    MLX5_INIT_CMDIF_REV_FW_SUB = 0x004,
    MLX5_INIT_CMDQ_ADDR_H = 0x010,
    MLX5_INIT_CMDQ_ADDR_L_SZ = 0x014,
    MLX5_INIT_CMD_DBELL = 0x018,
    MLX5_INIT_INITIALIZING = 0x1fc,
    MLX5_INIT_HEALTH_COUNTER = 0x101c,

    MLX5_CMD_OP_QUERY_ISSI = 0x10a,
    MLX5_CMD_OP_SET_ISSI = 0x10b,
    MLX5_CMD_OP_QUERY_HCA_CAP = 0x100,
    MLX5_CMD_OP_INIT_HCA = 0x102,
    MLX5_CMD_OP_CREATE_MKEY = 0x200,
    MLX5_CMD_OP_DESTROY_MKEY = 0x202,
    MLX5_CMD_OP_CREATE_EQ = 0x301,
    MLX5_CMD_OP_DESTROY_EQ = 0x302,
    MLX5_CMD_OP_CREATE_TIR = 0x900,
    MLX5_CMD_OP_DESTROY_TIR = 0x902,
    MLX5_CMD_OP_QUERY_TIR = 0x903,
    MLX5_CMD_OP_CREATE_CQ = 0x400,
    MLX5_CMD_OP_DESTROY_CQ = 0x401,
    MLX5_CMD_OP_CREATE_SQ = 0x904,
    MLX5_CMD_OP_MODIFY_SQ = 0x905,
    MLX5_CMD_OP_DESTROY_SQ = 0x906,
    MLX5_CMD_OP_QUERY_SQ = 0x907,
    MLX5_CMD_OP_CREATE_RQ = 0x908,
    MLX5_CMD_OP_MODIFY_RQ = 0x909,
    MLX5_CMD_OP_DESTROY_RQ = 0x90a,
    MLX5_CMD_OP_QUERY_RQ = 0x90b,
    MLX5_CMD_OP_CREATE_RQT = 0x916,
    MLX5_CMD_OP_DESTROY_RQT = 0x918,
    MLX5_CMD_OP_QUERY_RQT = 0x919,
    MLX5_CMD_OP_ENABLE_HCA = 0x104,
    MLX5_CMD_OP_QUERY_PAGES = 0x107,
    MLX5_CMD_OP_MANAGE_PAGES = 0x108,
    MLX5_CMD_OP_SET_HCA_CAP = 0x109,
    MLX5_CMD_OP_ACCESS_REG = 0x805,
    MLX5_CMD_OP_MODIFY_NIC_VPORT_CONTEXT = 0x755,
    MLX5_CMD_OP_ALLOC_Q_COUNTER = 0x771,
    MLX5_CMD_OP_DEALLOC_Q_COUNTER = 0x772,
    MLX5_CMD_OP_ALLOC_PD = 0x800,
    MLX5_CMD_OP_DEALLOC_PD = 0x801,
    MLX5_CMD_OP_ALLOC_UAR = 0x802,
    MLX5_CMD_OP_DEALLOC_UAR = 0x803,
    MLX5_CMD_OP_ALLOC_TRANSPORT_DOMAIN = 0x816,
    MLX5_CMD_OP_DEALLOC_TRANSPORT_DOMAIN = 0x817,
    MLX5_CMD_OP_CREATE_TIS = 0x912,
    MLX5_CMD_OP_DESTROY_TIS = 0x914,
    MLX5_CMD_OP_CREATE_FLOW_TABLE = 0x930,
    MLX5_CMD_OP_SET_FLOW_TABLE_ROOT = 0x92f,
    MLX5_CMD_OP_DESTROY_FLOW_TABLE = 0x931,
    MLX5_CMD_OP_CREATE_FLOW_GROUP = 0x933,
    MLX5_CMD_OP_DESTROY_FLOW_GROUP = 0x934,
    MLX5_CMD_OP_SET_FLOW_TABLE_ENTRY = 0x936,
    MLX5_CMD_OP_DELETE_FLOW_TABLE_ENTRY = 0x938,

    MLX5_CMD_OWNER_HW = 0x1,
    MLX5_PCI_CMD_XPORT = 7,
    MLX5_CMD_DESC_SIZE = 64,
    MLX5_CMDQ_SIZE = 4096,
    MLX5_CMD_BLOCK_SIZE = 512,
    MLX5_CMD_PROT_BLOCK_SIZE = 576,
    MLX5_CMD_MBOX_STRIDE = 4096,
    MLX5_CMD_MBOX_SIZE = 1048576,
    MLX5_NIC_IFC_FULL_DRIVER = 0,
    MLX5_MANAGE_PAGES_GIVE = 1,
    MLX5_HCA_CAP_GENERAL = 0,
    MLX5_HCA_CAP_GET_MAX = 0,
    MLX5_HCA_CAP_GET_CUR = 1,
    MLX5_HCA_CAP_SIZE = 4096,
    MLX5_HCA_CAP_OUT_SIZE = 4112,
    MLX5_HCA_CAP_OUT_CAP_OFF = 0x10,
    MLX5_ACCESS_REG_READ = 1,
    MLX5_REG_DTOR = 0xc00e,
    MLX5_REG_PAOS = 0x5006,
    MLX5_REG_PPCNT = 0x5008,
    MLX5_PPCNT_REG_SIZE = 0x800,
    MLX5_PPCNT_IEEE_802_3_GROUP = 0,
    MLX5_CREATE_MKEY_IN_SIZE = 0x110,
    MLX5_CREATE_MKEY_MKC_OFF = 0x10,
    MLX5_CREATE_EQ_IN_SIZE = 0x118,
    MLX5_CREATE_EQ_EQC_OFF = 0x10,
    MLX5_CREATE_EQ_PAS_OFF = 0x110,
    MLX5_CREATE_CQ_IN_SIZE = 0x118,
    MLX5_CREATE_CQ_CQC_OFF = 0x10,
    MLX5_CREATE_CQ_PAS_OFF = 0x110,
    MLX5_CREATE_SQ_IN_SIZE = 0x118,
    MLX5_CREATE_SQ_SQC_OFF = 0x20,
    MLX5_SQC_WQ_OFF = 0x30,
    MLX5_CREATE_SQ_PAS_OFF = 0x110,
    MLX5_CREATE_RQ_IN_SIZE = 0x118,
    MLX5_CREATE_RQ_RQC_OFF = 0x20,
    MLX5_RQC_WQ_OFF = 0x30,
    MLX5_CREATE_RQ_PAS_OFF = 0x110,
    MLX5_MODIFY_RQ_IN_SIZE = 0x220,
    MLX5_MODIFY_RQ_RQC_OFF = 0x20,
    MLX5_CREATE_TIR_IN_SIZE = 0x110,
    MLX5_CREATE_TIR_TIRC_OFF = 0x20,
    MLX5_QUERY_TIR_OUT_SIZE = 0x500,
    MLX5_QUERY_TIR_TIRC_OFF = 0x20,
    MLX5_RQT_ENTRY_COUNT = 16,
    MLX5_CREATE_RQT_IN_SIZE = 0x110 + MLX5_RQT_ENTRY_COUNT * 4,
    MLX5_CREATE_RQT_RQTC_OFF = 0x20,
    MLX5_QUERY_RQT_OUT_SIZE = 0x320,
    MLX5_QUERY_RQT_RQTC_OFF = 0x20,
    MLX5_MODIFY_SQ_IN_SIZE = 0x220,
    MLX5_MODIFY_SQ_SQC_OFF = 0x20,
    MLX5_QUERY_SQ_OUT_SIZE = 0x300,
    MLX5_QUERY_SQ_SQC_OFF = 0x20,
    MLX5_QUERY_RQ_OUT_SIZE = 0x300,
    MLX5_QUERY_RQ_RQC_OFF = 0x20,
    MLX5_CREATE_TIS_IN_SIZE = 0x600,
    MLX5_CREATE_TIS_CTX_OFF = 0x100,
    MLX5_CREATE_FLOW_TABLE_IN_SIZE = 0x40,
    MLX5_CREATE_FLOW_TABLE_FTC_OFF = 0x18,
    MLX5_CREATE_FLOW_GROUP_IN_SIZE = 0x2000,
    MLX5_SET_FTE_IN_SIZE = 0x348,
    MLX5_UAR_PAGE_SIZE = 4096,
    MLX5_BF_OFFSET = 0x800,
    MLX5_CQ_CQE_COUNT = 16,
    MLX5_CQE_SIZE = 64,
    MLX5_SQ_WQE_COUNT = 16,
    MLX5_RQ_WQE_COUNT = 16,
    MLX5_WQ_TYPE_CYCLIC = 1,
    MLX5_SEND_WQE_BB_LOG = 6,
    MLX5_SQC_STATE_RST = 0,
    MLX5_SQC_STATE_RDY = 1,
    MLX5_RQC_STATE_RST = 0,
    MLX5_RQC_STATE_RDY = 1,
    MLX5_INLINE_MODE_L2 = 1,
    MLX5_OPCODE_NOP = 0x00,
    MLX5_OPCODE_SEND = 0x0a,
    MLX5_WQE_CTRL_CQ_UPDATE = 2 << 2,
    MLX5_CQE_REQ_ERR = 13,
    MLX5_CQE_RESP_ERR = 14,
    MLX5_TX_TEST_MIN_FRAME = 60,
    MLX5_TX_TEST_MAX_INLINE_FRAME = 98,
    MLX5_RX_BUFFER_SIZE = 2048,
    MLX5_RX_TEST_POST_COUNT = MLX5_RQ_WQE_COUNT,
    MLX5_RX_TEST_WAIT_COUNT = 20,
    MLX5_FLOW_TABLE_TYPE_NIC_RX = 0x0,
    MLX5_FLOW_CONTEXT_ACTION_FWD_DEST = 0x4,
    MLX5_FLOW_CONTEXT_SOURCE_UPLINK = 0x1,
    MLX5_FLOW_DESTINATION_TYPE_TIR = 0x2,
};

#define MLX5_HW_START_PADDING UINT32_C(0x80000000)

struct mlx5_dma_page {
    void *addr;
    uint64_t iova;
};

struct mlx5_eq_res {
    struct mlx5_dma_page page;
    uint8_t eqn;
    int valid;
};

struct mlx5_cq_res {
    struct mlx5_dma_page cq_page;
    struct mlx5_dma_page dbr_page;
    uint32_t cqn;
    uint32_t cons_index;
    int valid;
};

struct mlx5_sq_res {
    struct mlx5_dma_page sq_page;
    struct mlx5_dma_page dbr_page;
    struct mlx5_dma_page tx_page;
    uint32_t sqn;
    uint32_t prod_index;
    int valid;
};

struct mlx5_rq_res {
    struct mlx5_dma_page rq_page;
    struct mlx5_dma_page dbr_page;
    struct mlx5_dma_page rx_pages[MLX5_RQ_WQE_COUNT];
    uint32_t rqn;
    uint32_t prod_index;
    int valid;
};

struct mlx5_tir_res {
    uint32_t tirn;
    int valid;
};

struct mlx5_rqt_res {
    uint32_t rqtn;
    int valid;
};

struct mlx5_flow_table_res {
    uint32_t table_id;
    int valid;
};

struct mlx5_flow_group_res {
    uint32_t group_id;
    int valid;
};

struct mlx5_fte_res {
    uint32_t flow_index;
    int valid;
};

struct mlx5_rx_packet {
    uint8_t *data;
    uint32_t len;
    uint32_t slot;
    uint16_t wqe_counter;
};

struct mlx5_test_runtime {
    struct mlx5_cmd_ctx ctx;
    uint32_t uar;
    uint32_t pd;
    uint32_t tdn;
    int have_tdn;
    uint8_t q_counter;
    int have_q_counter;
    uint32_t tisn;
    int have_tisn;
    uint32_t mkey;
    struct mlx5_eq_res eq;
    struct mlx5_cq_res tx_cq;
    struct mlx5_cq_res rx_cq;
    struct mlx5_sq_res sq;
    struct mlx5_rq_res rq;
    struct mlx5_rqt_res rqt;
    struct mlx5_tir_res tir;
    struct mlx5_flow_table_res ft;
    struct mlx5_flow_group_res fg;
    struct mlx5_fte_res fte;
    uint8_t hca_cap[MLX5_HCA_CAP_SIZE];
};

struct mlxnicd_dev {
    char bdf[32];
    struct mlxnicd_dev_config config;
    struct mlx5_test_runtime rt;
    int started;
    int last_error;
    int quiet_saved;
    uint16_t rx_outstanding;
};

static int mlxnicd_err_from_rc(int rc) {
    return rc == 0 ? MLXNICD_OK : MLXNICD_ERR_IO;
}

static int mlxnicd_set_error(struct mlxnicd_dev *dev, int err) {
    if (dev != NULL) {
        dev->last_error = err;
    }
    return err;
}

const char *mlxnicd_strerror(int err) {
    switch (err) {
    case MLXNICD_OK:
        return "ok";
    case MLXNICD_ERR_INVAL:
        return "invalid argument";
    case MLXNICD_ERR_STATE:
        return "invalid state";
    case MLXNICD_ERR_IO:
        return "device I/O error";
    case MLXNICD_ERR_TIMEOUT:
        return "timeout";
    case MLXNICD_ERR_UNSUPPORTED:
        return "unsupported";
    default:
        return "unknown error";
    }
}

static void mlx5_dump_cq_slots(struct mlx5_cq_res *cq, uint32_t first,
                               uint32_t count, const char *tag);

static uint32_t mmio_read32_be(void *base, uint64_t off) {
    volatile uint8_t *p = (volatile uint8_t *)base + off;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void mmio_write32_be(void *base, uint64_t off, uint32_t v) {
    volatile uint32_t *p = (volatile uint32_t *)((uint8_t *)base + off);
    *p = __builtin_bswap32(v);
}

static void put_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void put_be64(uint8_t *p, uint64_t v) {
    put_be32(p, (uint32_t)(v >> 32));
    put_be32(p + 4, (uint32_t)v);
}

static uint8_t load_u8(const void *p) {
    return *(const volatile uint8_t *)p;
}

static void mmio_write64_native(void *base, uint64_t off, uint64_t v) {
    volatile uint64_t *p = (volatile uint64_t *)((uint8_t *)base + off);
    *p = v;
}

static uint16_t get_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t get_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t get_be64(const uint8_t *p) {
    return ((uint64_t)get_be32(p) << 32) | (uint64_t)get_be32(p + 4);
}

static uint8_t xor8(const uint8_t *p, size_t len) {
    uint8_t x = 0;
    for (size_t i = 0; i < len; i++) {
        x ^= p[i];
    }
    return x;
}

static void sleep_ms(long ms) {
    struct timespec ts = {
        .tv_sec = ms / 1000,
        .tv_nsec = (ms % 1000) * 1000000L,
    };
    nanosleep(&ts, NULL);
}

static void dump_hex(const char *name, const uint8_t *p, size_t len) {
    printf("%s (%zu bytes):\n", name, len);
    for (size_t i = 0; i < len; i++) {
        if ((i % 16) == 0) {
            printf("  %04zx:", i);
        }
        printf(" %02x", p[i]);
        if ((i % 16) == 15 || i + 1 == len) {
            printf("\n");
        }
    }
}

static unsigned int cmdq_log_size(uint32_t v) {
    return (v & 0xffu) >> 4;
}

static unsigned int cmdq_log_stride(uint32_t v) {
    return v & 0xfu;
}

static unsigned int cmdq_nic_interface(uint32_t v) {
    return (v >> 8) & 0x7u;
}

static void print_cmdq_fields(const char *prefix, uint32_t h, uint32_t l) {
    printf("  %s: h=0x%08" PRIx32 " l=0x%08" PRIx32
           " addr=0x%08" PRIx32 "%05" PRIx32 "000"
           " nic_interface=%u log_size=%u log_stride=%u\n",
           prefix, h, l, h, (l >> 12) & 0xfffffu, cmdq_nic_interface(l),
           cmdq_log_size(l), cmdq_log_stride(l));
}

static int wait_fw_ready(void *bar0, int timeout_ms) {
    int elapsed = 0;
    while (elapsed <= timeout_ms) {
        uint32_t initializing = mmio_read32_be(bar0, MLX5_INIT_INITIALIZING);
        if ((initializing >> 31) == 0) {
            return 0;
        }
        sleep_ms(10);
        elapsed += 10;
    }
    fprintf(stderr, "firmware still initializing after %d ms; raw=0x%08" PRIx32 "\n",
            timeout_ms, mmio_read32_be(bar0, MLX5_INIT_INITIALIZING));
    return -1;
}

static int alloc_page(void **ptr) {
    int rc = posix_memalign(ptr, 4096, MLX5_CMDQ_SIZE);
    if (rc != 0) {
        *ptr = NULL;
        return -1;
    }
    memset(*ptr, 0, MLX5_CMDQ_SIZE);
    return 0;
}

static int alloc_aligned_zero(void **ptr, size_t size) {
    int rc = posix_memalign(ptr, 4096, size);
    if (rc != 0) {
        *ptr = NULL;
        return -1;
    }
    memset(*ptr, 0, size);
    return 0;
}

static int mlx5_dma_page_alloc(struct mlx5_cmd_ctx *ctx,
                               struct mlx5_dma_page *page, uint64_t iova,
                               const char *name) {
    memset(page, 0, sizeof(*page));
    page->iova = iova;
    if (alloc_aligned_zero(&page->addr, 4096) != 0) {
        fprintf(stderr, "failed to allocate %s DMA page\n", name);
        return -1;
    }
    if (vfio_dma_map(&ctx->dev, page->addr, page->iova, 4096) != 0) {
        free(page->addr);
        memset(page, 0, sizeof(*page));
        return -1;
    }
    return 0;
}

static void mlx5_dma_page_free(struct mlx5_cmd_ctx *ctx,
                               struct mlx5_dma_page *page) {
    if (page->addr != NULL) {
        vfio_dma_unmap(&ctx->dev, page->iova, 4096);
        free(page->addr);
    }
    memset(page, 0, sizeof(*page));
}

void mlx5_cmd_ctx_close(struct mlx5_cmd_ctx *ctx) {
    if (ctx->dev.bar0 != NULL) {
        mmio_write32_be(ctx->dev.bar0, MLX5_INIT_CMDQ_ADDR_H, ctx->cmdq_h_orig);
        mmio_write32_be(ctx->dev.bar0, MLX5_INIT_CMDQ_ADDR_L_SZ,
                        ctx->cmdq_l_sz_orig);
    }
    if (ctx->outbox != NULL) {
        vfio_dma_unmap(&ctx->dev, ctx->outbox_iova, MLX5_CMD_MBOX_SIZE);
    }
    if (ctx->inbox != NULL) {
        vfio_dma_unmap(&ctx->dev, ctx->inbox_iova, MLX5_CMD_MBOX_SIZE);
    }
    if (ctx->cmdq != NULL) {
        vfio_dma_unmap(&ctx->dev, ctx->cmdq_iova, MLX5_CMDQ_SIZE);
    }
    for (size_t i = 0; i < ctx->fw_page_count; i++) {
        if (ctx->fw_pages[i].addr != NULL) {
            vfio_dma_unmap(&ctx->dev, ctx->fw_pages[i].iova, 4096);
        }
    }
    free(ctx->outbox);
    free(ctx->inbox);
    free(ctx->cmdq);
    for (size_t i = 0; i < ctx->fw_page_count; i++) {
        free(ctx->fw_pages[i].addr);
    }
    vfio_device_close(&ctx->dev);
}

int mlx5_cmd_ctx_open(const char *bdf, struct mlx5_cmd_ctx *ctx) {
    uint32_t cmdif;
    unsigned int cmdif_rev;

    memset(ctx, 0, sizeof(*ctx));
    ctx->cmdq_iova = 0x01000000ULL;
    ctx->inbox_iova = 0x01800000ULL;
    ctx->outbox_iova = 0x02000000ULL;

    if (vfio_device_open(bdf, &ctx->dev) != 0) {
        return -1;
    }
    cmdif = mmio_read32_be(ctx->dev.bar0, MLX5_INIT_CMDIF_REV_FW_SUB);
    cmdif_rev = cmdif >> 16;
    if (cmdif_rev != 5) {
        fprintf(stderr, "unsupported cmdif_rev %u; expected 5\n", cmdif_rev);
        goto fail;
    }
    if (wait_fw_ready(ctx->dev.bar0, 5000) != 0) {
        goto fail;
    }
    if (alloc_page(&ctx->cmdq) != 0 ||
        alloc_aligned_zero(&ctx->inbox, MLX5_CMD_MBOX_SIZE) != 0 ||
        alloc_aligned_zero(&ctx->outbox, MLX5_CMD_MBOX_SIZE) != 0) {
        fprintf(stderr, "failed to allocate command DMA pages\n");
        goto fail;
    }
    if (vfio_dma_map(&ctx->dev, ctx->cmdq, ctx->cmdq_iova, MLX5_CMDQ_SIZE) != 0 ||
        vfio_dma_map(&ctx->dev, ctx->inbox, ctx->inbox_iova, MLX5_CMD_MBOX_SIZE) != 0 ||
        vfio_dma_map(&ctx->dev, ctx->outbox, ctx->outbox_iova, MLX5_CMD_MBOX_SIZE) != 0) {
        goto fail;
    }

    ctx->cmdq_h_orig = mmio_read32_be(ctx->dev.bar0, MLX5_INIT_CMDQ_ADDR_H);
    ctx->cmdq_l_sz_orig = mmio_read32_be(ctx->dev.bar0, MLX5_INIT_CMDQ_ADDR_L_SZ);
    mmio_write32_be(ctx->dev.bar0, MLX5_INIT_CMDQ_ADDR_L_SZ,
                    (ctx->cmdq_l_sz_orig & 0xfffff000u) |
                        (MLX5_NIC_IFC_FULL_DRIVER << 8) |
                        (ctx->cmdq_l_sz_orig & 0xffu));
    __sync_synchronize();
    sleep_ms(100);
    mmio_write32_be(ctx->dev.bar0, MLX5_INIT_CMDQ_ADDR_H,
                    (uint32_t)(ctx->cmdq_iova >> 32));
    mmio_write32_be(ctx->dev.bar0, MLX5_INIT_CMDQ_ADDR_L_SZ,
                    (uint32_t)ctx->cmdq_iova);
    __sync_synchronize();
    sleep_ms(10);
    return 0;

fail:
    mlx5_cmd_ctx_close(ctx);
    return -1;
}

static void setup_mailbox_block(uint8_t *block, uint64_t next_iova,
                                uint32_t block_num, uint8_t token) {
    memset(block, 0, MLX5_CMD_PROT_BLOCK_SIZE);
    put_be64(block + MLX5_CMD_BLOCK_SIZE + 48, next_iova);
    put_be32(block + MLX5_CMD_BLOCK_SIZE + 56, block_num);
    block[MLX5_CMD_BLOCK_SIZE + 61] = token;
}

static void sign_mailbox_block(uint8_t *block) {
    block[MLX5_CMD_BLOCK_SIZE + 62] =
        (uint8_t)~xor8(block + MLX5_CMD_BLOCK_SIZE,
                       MLX5_CMD_PROT_BLOCK_SIZE - MLX5_CMD_BLOCK_SIZE - 2);
    block[MLX5_CMD_PROT_BLOCK_SIZE - 1] =
        (uint8_t)~xor8(block, MLX5_CMD_PROT_BLOCK_SIZE - 1);
}

static size_t num_blocks_for_extra(size_t extra) {
    return (extra + MLX5_CMD_BLOCK_SIZE - 1) / MLX5_CMD_BLOCK_SIZE;
}

static int setup_mailbox_chain(uint8_t *mbox, uint64_t base_iova,
                               const uint8_t *src, size_t extra,
                               uint8_t token) {
    size_t nblocks = num_blocks_for_extra(extra);
    if (nblocks * MLX5_CMD_MBOX_STRIDE > MLX5_CMD_MBOX_SIZE) {
        return -1;
    }
    for (size_t i = 0; i < nblocks; i++) {
        uint8_t *block = mbox + i * MLX5_CMD_MBOX_STRIDE;
        uint64_t next = i + 1 < nblocks
                            ? base_iova + (i + 1) * MLX5_CMD_MBOX_STRIDE
                            : 0;
        size_t off = i * MLX5_CMD_BLOCK_SIZE;
        size_t copy = extra - off;
        if (copy > MLX5_CMD_BLOCK_SIZE) {
            copy = MLX5_CMD_BLOCK_SIZE;
        }
        setup_mailbox_block(block, next, (uint32_t)i, token);
        if (src != NULL && copy != 0) {
            memcpy(block, src + off, copy);
        }
        sign_mailbox_block(block);
    }
    return 0;
}

static void copy_from_mailbox_chain(uint8_t *dst, size_t extra,
                                    const uint8_t *mbox) {
    size_t nblocks = num_blocks_for_extra(extra);
    for (size_t i = 0; i < nblocks; i++) {
        const uint8_t *block = mbox + i * MLX5_CMD_MBOX_STRIDE;
        size_t off = i * MLX5_CMD_BLOCK_SIZE;
        size_t copy = extra - off;
        if (copy > MLX5_CMD_BLOCK_SIZE) {
            copy = MLX5_CMD_BLOCK_SIZE;
        }
        memcpy(dst + off, block, copy);
    }
}

static int mlx5_cmd_exec(struct mlx5_cmd_ctx *ctx, uint16_t opcode,
                         const uint8_t *in, size_t in_len, uint8_t *out,
                         size_t out_len, const char *name) {
    uint8_t *desc = (uint8_t *)ctx->cmdq;
    uint8_t *in_block = (uint8_t *)ctx->inbox;
    uint8_t *out_block = (uint8_t *)ctx->outbox;
    volatile uint8_t *status_own = (volatile uint8_t *)(desc + 63);
    uint8_t token = ctx->token++;
    if (ctx->token == 0) {
        ctx->token = 1;
    }
    if (token == 0) {
        token = ctx->token++;
    }
    size_t inline_in = in_len < 16 ? in_len : 16;
    size_t inline_out = out_len < 16 ? out_len : 16;
    size_t extra_in = in_len > 16 ? in_len - 16 : 0;
    size_t extra_out = out_len > 16 ? out_len - 16 : 0;

    if (num_blocks_for_extra(extra_in) * MLX5_CMD_MBOX_STRIDE > MLX5_CMD_MBOX_SIZE ||
        num_blocks_for_extra(extra_out) * MLX5_CMD_MBOX_STRIDE > MLX5_CMD_MBOX_SIZE) {
        fprintf(stderr, "%s too large for minimal command path\n", name);
        return -1;
    }

    memset(desc, 0, MLX5_CMD_DESC_SIZE);
    memset(in_block, 0, MLX5_CMD_MBOX_SIZE);
    memset(out_block, 0, MLX5_CMD_MBOX_SIZE);
    desc[0] = MLX5_PCI_CMD_XPORT;
    put_be32(desc + 4, (uint32_t)in_len);
    if (inline_in != 0) {
        memcpy(desc + 16, in, inline_in);
    }
    if (extra_in != 0) {
        if (setup_mailbox_chain(in_block, ctx->inbox_iova, in + 16, extra_in,
                                token) != 0) {
            return -1;
        }
        put_be64(desc + 8, ctx->inbox_iova);
    }
    if (extra_out != 0) {
        if (setup_mailbox_chain(out_block, ctx->outbox_iova, NULL, extra_out,
                                token) != 0) {
            return -1;
        }
        put_be64(desc + 48, ctx->outbox_iova);
    }
    put_be32(desc + 56, (uint32_t)out_len);
    desc[60] = token;
    desc[63] = MLX5_CMD_OWNER_HW;
    desc[61] = (uint8_t)~xor8(desc, MLX5_CMD_DESC_SIZE);

    printf("%s debug\n", name);
    printf("  opcode: 0x%04x\n", opcode);
    printf("  pci command: 0x%04x\n", ctx->dev.pci_command);
    print_cmdq_fields("cmdq original", ctx->cmdq_h_orig, ctx->cmdq_l_sz_orig);
    print_cmdq_fields("cmdq programmed",
                      mmio_read32_be(ctx->dev.bar0, MLX5_INIT_CMDQ_ADDR_H),
                      mmio_read32_be(ctx->dev.bar0, MLX5_INIT_CMDQ_ADDR_L_SZ));
    printf("  desc xor before doorbell: 0x%02x\n", xor8(desc, MLX5_CMD_DESC_SIZE));
    printf("  desc status_own before doorbell: 0x%02x\n", *status_own);
    fflush(stdout);

    __sync_synchronize();
    mmio_write32_be(ctx->dev.bar0, MLX5_INIT_CMD_DBELL, 1u);
    for (int i = 0; i < 1000000; i++) {
        if ((*status_own & MLX5_CMD_OWNER_HW) == 0) {
            break;
        }
        if ((i % 1000) == 0) {
            sleep_ms(1);
        }
    }
    __sync_synchronize();
    if (*status_own & MLX5_CMD_OWNER_HW) {
        fprintf(stderr, "%s timed out; status_own=0x%02x\n", name, *status_own);
        dump_hex("descriptor", desc, MLX5_CMD_DESC_SIZE);
        dump_hex("outbox", out_block, 128);
        return -1;
    }

    memset(out, 0, out_len);
    if (inline_out != 0) {
        memcpy(out, desc + 32, inline_out);
    }
    if (extra_out != 0) {
        copy_from_mailbox_chain(out + 16, extra_out, out_block);
    }
    printf("%s ok\n", name);
    printf("  status_own: 0x%02x\n", *status_own);
    printf("  delivery_status: %u\n", *status_own >> 1);
    printf("  descriptor_xor: 0x%02x\n", xor8(desc, MLX5_CMD_DESC_SIZE));
    printf("  out.status: %u\n", out_len > 0 ? out[0] : 0);
    printf("  out.syndrome: 0x%08" PRIx32 "\n",
           out_len >= 8 ? get_be32(out + 4) : 0);
    if (out_len > 0 && out[0] != 0) {
        fprintf(stderr, "%s firmware status error: status=%u syndrome=0x%08" PRIx32 "\n",
                name, out[0], out_len >= 8 ? get_be32(out + 4) : 0);
        return -1;
    }
    return 0;
}

int mlx5_ctx_query_issi(struct mlx5_cmd_ctx *ctx) {
    uint8_t in[16] = {0};
    uint8_t out[128] = {0};

    put_be16(in, MLX5_CMD_OP_QUERY_ISSI);
    int rc = mlx5_cmd_exec(ctx, MLX5_CMD_OP_QUERY_ISSI, in, sizeof(in), out,
                           sizeof(out), "mlx5-query-issi");
    if (rc == 0) {
        printf("  out.current_issi: %u\n", (unsigned)get_be16(out + 10));
        printf("  out.supported_issi_dw0: 0x%08" PRIx32 "\n",
               get_be32(out + 108));
    }
    return rc;
}

int mlx5_ctx_enable_hca(struct mlx5_cmd_ctx *ctx) {
    uint8_t in[32] = {0};
    uint8_t out[16] = {0};

    put_be16(in, MLX5_CMD_OP_ENABLE_HCA);
    return mlx5_cmd_exec(ctx, MLX5_CMD_OP_ENABLE_HCA, in, sizeof(in), out,
                         sizeof(out), "seq-enable-hca");
}

int mlx5_ctx_set_issi(struct mlx5_cmd_ctx *ctx) {
    uint8_t in[32] = {0};
    uint8_t out[16] = {0};

    put_be16(in, MLX5_CMD_OP_SET_ISSI);
    put_be16(in + 0x0a, 1);
    return mlx5_cmd_exec(ctx, MLX5_CMD_OP_SET_ISSI, in, sizeof(in), out,
                         sizeof(out), "seq-set-issi");
}

int mlx5_ctx_query_pages(struct mlx5_cmd_ctx *ctx, int boot) {
    uint8_t in[32] = {0};
    uint8_t out[32] = {0};
    int rc;

    put_be16(in, MLX5_CMD_OP_QUERY_PAGES);
    put_be16(in + 0x06, boot ? 1 : 2);
    rc = mlx5_cmd_exec(ctx, MLX5_CMD_OP_QUERY_PAGES, in, sizeof(in), out,
                       sizeof(out), boot ? "seq-query-boot-pages"
                                         : "seq-query-init-pages");
    if (rc == 0) {
        printf("  function_id: 0x%04x\n", get_be16(out + 0x0a));
        printf("  num_pages: %" PRIu32 "\n", get_be32(out + 0x0c));
    }
    return rc;
}

static int mlx5_ctx_give_pages(struct mlx5_cmd_ctx *ctx, uint16_t function_id,
                               uint32_t num_pages, const char *phase) {
    uint8_t in[16 + MLX5_MAX_FW_PAGES * 8] = {0};
    uint8_t out[16] = {0};
    uint64_t base_iova = 0x03000000ULL + ctx->fw_page_count * 4096ULL;

    if (num_pages == 0) {
        printf("  %s pages: none required\n", phase);
        return 0;
    }
    if (num_pages > MLX5_MAX_FW_PAGES ||
        ctx->fw_page_count + num_pages > MLX5_MAX_FW_PAGES) {
        fprintf(stderr, "%s pages request too large: %" PRIu32
                        " requested, %zu already allocated, max %u\n",
                phase, num_pages, ctx->fw_page_count, MLX5_MAX_FW_PAGES);
        return -1;
    }

    for (uint32_t i = 0; i < num_pages; i++) {
        void *page = NULL;
        uint64_t iova = base_iova + i * 4096ULL;

        if (alloc_aligned_zero(&page, 4096) != 0) {
            fprintf(stderr, "failed to allocate %s FW page %" PRIu32 "\n",
                    phase, i);
            return -1;
        }
        if (vfio_dma_map(&ctx->dev, page, iova, 4096) != 0) {
            free(page);
            return -1;
        }
        ctx->fw_pages[ctx->fw_page_count].addr = page;
        ctx->fw_pages[ctx->fw_page_count].iova = iova;
        ctx->fw_page_count++;
        put_be64(in + 0x10 + i * 8, iova);
    }

    put_be16(in, MLX5_CMD_OP_MANAGE_PAGES);
    put_be16(in + 0x06, MLX5_MANAGE_PAGES_GIVE);
    put_be16(in + 0x0a, function_id);
    put_be32(in + 0x0c, num_pages);
    printf("  giving %s pages: function_id=0x%04x num_pages=%" PRIu32 "\n",
           phase, function_id, num_pages);
    return mlx5_cmd_exec(ctx, MLX5_CMD_OP_MANAGE_PAGES, in,
                         16 + (size_t)num_pages * 8, out, sizeof(out),
                         phase);
}

static int mlx5_ctx_query_hca_cap_raw(struct mlx5_cmd_ctx *ctx, uint16_t opmod,
                                      uint8_t *cap, size_t cap_len,
                                      const char *name) {
    uint8_t in[16] = {0};
    uint8_t out[MLX5_HCA_CAP_OUT_SIZE] = {0};
    int rc;

    if (cap_len > MLX5_HCA_CAP_SIZE ||
        MLX5_HCA_CAP_OUT_CAP_OFF + cap_len > sizeof(out)) {
        return -1;
    }
    put_be16(in, MLX5_CMD_OP_QUERY_HCA_CAP);
    put_be16(in + 0x06, opmod);
    rc = mlx5_cmd_exec(ctx, MLX5_CMD_OP_QUERY_HCA_CAP, in, sizeof(in), out,
                       sizeof(out), name);
    if (rc == 0) {
        memcpy(cap, out + MLX5_HCA_CAP_OUT_CAP_OFF, cap_len);
    }
    return rc;
}

static int mlx5_ctx_query_dtor(struct mlx5_cmd_ctx *ctx) {
    uint8_t in[16 + 64] = {0};
    uint8_t out[16 + 64] = {0};

    put_be16(in, MLX5_CMD_OP_ACCESS_REG);
    put_be16(in + 0x06, MLX5_ACCESS_REG_READ);
    put_be16(in + 0x0a, MLX5_REG_DTOR);
    return mlx5_cmd_exec(ctx, MLX5_CMD_OP_ACCESS_REG, in, sizeof(in), out,
                         sizeof(out), "seq-query-dtor");
}

static int mlx5_ctx_query_paos(struct mlx5_cmd_ctx *ctx, uint8_t local_port) {
    uint8_t in[16 + 64] = {0};
    uint8_t out[16 + 64] = {0};
    uint8_t *reg = in + 16;
    const uint8_t *oreg = out + 16;
    int rc;

    put_be16(in, MLX5_CMD_OP_ACCESS_REG);
    put_be16(in + 0x06, MLX5_ACCESS_REG_READ);
    put_be16(in + 0x0a, MLX5_REG_PAOS);
    reg[0x01] = local_port;

    rc = mlx5_cmd_exec(ctx, MLX5_CMD_OP_ACCESS_REG, in, sizeof(in), out,
                       sizeof(out), "seq-query-paos");
    if (rc == 0) {
        printf("  paos local_port: %u\n", oreg[0x01]);
        printf("  paos admin_status: %u\n", oreg[0x02] & 0x0f);
        printf("  paos oper_status: %u\n", oreg[0x03] & 0x0f);
        printf("  paos ase: %u\n", (oreg[0x04] >> 7) & 0x01);
        printf("  paos ee: %u\n", (oreg[0x04] >> 6) & 0x01);
        printf("  paos e: %u\n", oreg[0x07] & 0x03);
    }
    return rc;
}

static __attribute__((unused)) int
mlx5_ctx_query_ppcnt_802_3(struct mlx5_cmd_ctx *ctx, uint8_t local_port,
                           const char *tag) {
    uint8_t in[16 + MLX5_PPCNT_REG_SIZE] = {0};
    uint8_t out[16 + MLX5_PPCNT_REG_SIZE] = {0};
    uint8_t *reg = in + 16;
    const uint8_t *oreg = out + 16;
    const size_t counter_set = 0x08;
    uint64_t rx_packets;
    uint64_t rx_bytes;
    uint64_t rx_multicast;
    uint64_t rx_broadcast;
    uint64_t rx_crc_errors;
    int rc;

    put_be16(in, MLX5_CMD_OP_ACCESS_REG);
    put_be16(in + 0x06, MLX5_ACCESS_REG_READ);
    put_be16(in + 0x0a, MLX5_REG_PPCNT);
    reg[0x01] = local_port;
    reg[0x03] = MLX5_PPCNT_IEEE_802_3_GROUP;

    rc = mlx5_cmd_exec(ctx, MLX5_CMD_OP_ACCESS_REG, in, sizeof(in), out,
                       sizeof(out), tag);
    if (rc != 0) {
        return rc;
    }

    rx_packets = get_be64(oreg + counter_set + 0x08);
    rx_crc_errors = get_be64(oreg + counter_set + 0x10);
    rx_bytes = get_be64(oreg + counter_set + 0x28);
    rx_multicast = get_be64(oreg + counter_set + 0x40);
    rx_broadcast = get_be64(oreg + counter_set + 0x48);
    printf("  ppcnt local_port: %u\n", oreg[0x01]);
    printf("  ppcnt rx_packets_phy: %" PRIu64 "\n", rx_packets);
    printf("  ppcnt rx_bytes_phy: %" PRIu64 "\n", rx_bytes);
    printf("  ppcnt rx_multicast_phy: %" PRIu64 "\n", rx_multicast);
    printf("  ppcnt rx_broadcast_phy: %" PRIu64 "\n", rx_broadcast);
    printf("  ppcnt rx_crc_errors_phy: %" PRIu64 "\n", rx_crc_errors);
    return 0;
}

static int mlx5_ctx_init_hca(struct mlx5_cmd_ctx *ctx) {
    uint8_t in[32] = {0};
    uint8_t out[16] = {0};

    put_be16(in, MLX5_CMD_OP_INIT_HCA);
    return mlx5_cmd_exec(ctx, MLX5_CMD_OP_INIT_HCA, in, sizeof(in), out,
                         sizeof(out), "seq-init-hca");
}

static int mlx5_ctx_alloc_uar(struct mlx5_cmd_ctx *ctx, uint32_t *uar) {
    uint8_t in[16] = {0};
    uint8_t out[16] = {0};
    int rc;

    put_be16(in, MLX5_CMD_OP_ALLOC_UAR);
    rc = mlx5_cmd_exec(ctx, MLX5_CMD_OP_ALLOC_UAR, in, sizeof(in), out,
                       sizeof(out), "seq-alloc-uar");
    if (rc == 0) {
        *uar = get_be32(out + 0x08) & 0x00ffffffu;
        printf("  uar: %" PRIu32 "\n", *uar);
    }
    return rc;
}

static int mlx5_ctx_dealloc_uar(struct mlx5_cmd_ctx *ctx, uint32_t uar) {
    uint8_t in[16] = {0};
    uint8_t out[16] = {0};

    put_be16(in, MLX5_CMD_OP_DEALLOC_UAR);
    put_be32(in + 0x08, uar & 0x00ffffffu);
    printf("  dealloc uar: %" PRIu32 "\n", uar);
    return mlx5_cmd_exec(ctx, MLX5_CMD_OP_DEALLOC_UAR, in, sizeof(in), out,
                         sizeof(out), "seq-dealloc-uar");
}

static int mlx5_ctx_alloc_pd(struct mlx5_cmd_ctx *ctx, uint32_t *pd) {
    uint8_t in[16] = {0};
    uint8_t out[16] = {0};
    int rc;

    put_be16(in, MLX5_CMD_OP_ALLOC_PD);
    rc = mlx5_cmd_exec(ctx, MLX5_CMD_OP_ALLOC_PD, in, sizeof(in), out,
                       sizeof(out), "seq-alloc-pd");
    if (rc == 0) {
        *pd = get_be32(out + 0x08) & 0x00ffffffu;
        printf("  pd: %" PRIu32 "\n", *pd);
    }
    return rc;
}

static int mlx5_ctx_dealloc_pd(struct mlx5_cmd_ctx *ctx, uint32_t pd) {
    uint8_t in[16] = {0};
    uint8_t out[16] = {0};

    put_be16(in, MLX5_CMD_OP_DEALLOC_PD);
    put_be32(in + 0x08, pd & 0x00ffffffu);
    printf("  dealloc pd: %" PRIu32 "\n", pd);
    return mlx5_cmd_exec(ctx, MLX5_CMD_OP_DEALLOC_PD, in, sizeof(in), out,
                         sizeof(out), "seq-dealloc-pd");
}

static int mlx5_ctx_alloc_q_counter(struct mlx5_cmd_ctx *ctx,
                                    uint8_t *counter_set_id) {
    uint8_t in[16] = {0};
    uint8_t out[16] = {0};
    int rc;

    put_be16(in, MLX5_CMD_OP_ALLOC_Q_COUNTER);
    rc = mlx5_cmd_exec(ctx, MLX5_CMD_OP_ALLOC_Q_COUNTER, in, sizeof(in), out,
                       sizeof(out), "seq-alloc-q-counter");
    if (rc == 0) {
        *counter_set_id = out[0x0b];
        printf("  q_counter: %" PRIu8 "\n", *counter_set_id);
    }
    return rc;
}

static int mlx5_ctx_dealloc_q_counter(struct mlx5_cmd_ctx *ctx,
                                      uint8_t counter_set_id) {
    uint8_t in[16] = {0};
    uint8_t out[16] = {0};

    put_be16(in, MLX5_CMD_OP_DEALLOC_Q_COUNTER);
    in[0x0b] = counter_set_id;
    printf("  dealloc q_counter: %" PRIu8 "\n", counter_set_id);
    return mlx5_cmd_exec(ctx, MLX5_CMD_OP_DEALLOC_Q_COUNTER, in, sizeof(in),
                         out, sizeof(out), "seq-dealloc-q-counter");
}

static int mlx5_ctx_alloc_transport_domain(struct mlx5_cmd_ctx *ctx,
                                           uint32_t *tdn) {
    uint8_t in[16] = {0};
    uint8_t out[16] = {0};
    int rc;

    put_be16(in, MLX5_CMD_OP_ALLOC_TRANSPORT_DOMAIN);
    rc = mlx5_cmd_exec(ctx, MLX5_CMD_OP_ALLOC_TRANSPORT_DOMAIN, in, sizeof(in),
                       out, sizeof(out), "seq-alloc-td");
    if (rc == 0) {
        *tdn = get_be32(out + 0x08) & 0x00ffffffu;
        printf("  tdn: %" PRIu32 "\n", *tdn);
    }
    return rc;
}

static int mlx5_ctx_dealloc_transport_domain(struct mlx5_cmd_ctx *ctx,
                                             uint32_t tdn) {
    uint8_t in[16] = {0};
    uint8_t out[16] = {0};

    put_be16(in, MLX5_CMD_OP_DEALLOC_TRANSPORT_DOMAIN);
    put_be32(in + 0x08, tdn & 0x00ffffffu);
    printf("  dealloc tdn: %" PRIu32 "\n", tdn);
    return mlx5_cmd_exec(ctx, MLX5_CMD_OP_DEALLOC_TRANSPORT_DOMAIN, in,
                         sizeof(in), out, sizeof(out), "seq-dealloc-td");
}

static int mlx5_ctx_create_tis(struct mlx5_cmd_ctx *ctx, uint32_t tdn,
                               uint32_t pd, uint32_t *tisn) {
    uint8_t in[MLX5_CREATE_TIS_IN_SIZE] = {0};
    uint8_t out[16] = {0};
    uint8_t *tisc = in + MLX5_CREATE_TIS_CTX_OFF;
    int rc;

    (void)pd;
    put_be16(in, MLX5_CMD_OP_CREATE_TIS);
    put_be32(tisc + 0x120, tdn & 0x00ffffffu);
    rc = mlx5_cmd_exec(ctx, MLX5_CMD_OP_CREATE_TIS, in, sizeof(in), out,
                       sizeof(out), "seq-create-tis");
    if (rc == 0) {
        *tisn = get_be32(out + 0x08) & 0x00ffffffu;
        printf("  tisn: %" PRIu32 "\n", *tisn);
    }
    return rc;
}

static int mlx5_ctx_destroy_tis(struct mlx5_cmd_ctx *ctx, uint32_t tisn) {
    uint8_t in[16] = {0};
    uint8_t out[16] = {0};

    put_be16(in, MLX5_CMD_OP_DESTROY_TIS);
    put_be32(in + 0x08, tisn & 0x00ffffffu);
    printf("  destroy tisn: %" PRIu32 "\n", tisn);
    return mlx5_cmd_exec(ctx, MLX5_CMD_OP_DESTROY_TIS, in, sizeof(in), out,
                         sizeof(out), "seq-destroy-tis");
}

static int mlx5_ctx_create_pa_mkey(struct mlx5_cmd_ctx *ctx, uint32_t pd,
                                   uint32_t *mkey) {
    uint8_t in[MLX5_CREATE_MKEY_IN_SIZE] = {0};
    uint8_t out[16] = {0};
    uint8_t *mkc = in + MLX5_CREATE_MKEY_MKC_OFF;
    uint32_t mkey_index;
    int rc;

    put_be16(in, MLX5_CMD_OP_CREATE_MKEY);
    mkc[0x02] |= 0x18; /* lw=1, lr=1 */
    mkc[0x04] = 0xff; /* qpn = 0xffffff */
    mkc[0x05] = 0xff;
    mkc[0x06] = 0xff;
    mkc[0x0c] |= 0x80; /* length64=1 */
    mkc[0x0d] = (uint8_t)(pd >> 16);
    mkc[0x0e] = (uint8_t)(pd >> 8);
    mkc[0x0f] = (uint8_t)pd;

    rc = mlx5_cmd_exec(ctx, MLX5_CMD_OP_CREATE_MKEY, in, sizeof(in), out,
                       sizeof(out), "seq-create-pa-mkey");
    if (rc == 0) {
        mkey_index = get_be32(out + 0x08) & 0x00ffffffu;
        *mkey = mkey_index << 8;
        printf("  mkey_index: %" PRIu32 "\n", mkey_index);
        printf("  mkey: 0x%08" PRIx32 "\n", *mkey);
    }
    return rc;
}

static int mlx5_ctx_destroy_mkey(struct mlx5_cmd_ctx *ctx, uint32_t mkey) {
    uint8_t in[16] = {0};
    uint8_t out[16] = {0};
    uint32_t mkey_index = mkey >> 8;

    put_be16(in, MLX5_CMD_OP_DESTROY_MKEY);
    put_be32(in + 0x08, mkey_index & 0x00ffffffu);
    printf("  destroy mkey: 0x%08" PRIx32 " index=%" PRIu32 "\n", mkey,
           mkey_index);
    return mlx5_cmd_exec(ctx, MLX5_CMD_OP_DESTROY_MKEY, in, sizeof(in), out,
                         sizeof(out), "seq-destroy-mkey");
}

static int mlx5_ctx_enable_vport_promisc(struct mlx5_cmd_ctx *ctx) {
    uint8_t in[0x200] = {0};
    uint8_t out[0x80] = {0};

    put_be16(in, MLX5_CMD_OP_MODIFY_NIC_VPORT_CONTEXT);
    /*
     * modify_nic_vport_context_in.field_select.promisc:
     * input bit offset 0x60 + field bit 0x1b => dword 0x0c, bit 4.
     */
    put_be32(in + 0x0c, 1u << 4);
    /*
     * nic_vport_context starts at input bit 0x800.  promisc_uc/mc/all are
     * context bits 0x780, 0x781, 0x782, i.e. input byte 0x1f0 bits 31..29.
     */
    put_be32(in + 0x1f0, (1u << 31) | (1u << 30) | (1u << 29));

    printf("  enable nic vport promisc: uc=1 mc=1 all=1\n");
    return mlx5_cmd_exec(ctx, MLX5_CMD_OP_MODIFY_NIC_VPORT_CONTEXT, in,
                         sizeof(in), out, sizeof(out),
                         "seq-enable-vport-promisc");
}

static int mlx5_ctx_create_eq(struct mlx5_cmd_ctx *ctx, uint32_t uar,
                              struct mlx5_eq_res *eq) {
    uint8_t in[MLX5_CREATE_EQ_IN_SIZE] = {0};
    uint8_t out[16] = {0};
    uint8_t *eqc = in + MLX5_CREATE_EQ_EQC_OFF;
    const uint64_t eq_iova = 0x07200000ULL;
    int rc;

    memset(eq, 0, sizeof(*eq));
    if (mlx5_dma_page_alloc(ctx, &eq->page, eq_iova, "EQ") != 0) {
        return -1;
    }

    put_be16(in, MLX5_CMD_OP_CREATE_EQ);
    eqc[0x0c] = 4;                 /* log_eq_size=4 => 16 EQEs */
    eqc[0x0d] = (uint8_t)(uar >> 16);
    eqc[0x0e] = (uint8_t)(uar >> 8);
    eqc[0x0f] = (uint8_t)uar;
    eqc[0x17] = 0;                 /* intr=0 */
    eqc[0x18] = 0;                 /* log_page_size=0 => 4KB */
    put_be64(in + MLX5_CREATE_EQ_PAS_OFF, eq->page.iova);

    rc = mlx5_cmd_exec(ctx, MLX5_CMD_OP_CREATE_EQ, in, sizeof(in), out,
                       sizeof(out), "seq-create-eq");
    if (rc == 0) {
        eq->eqn = (uint8_t)(get_be32(out + 0x08) & 0xffu);
        eq->valid = 1;
        printf("  eqn: %u\n", eq->eqn);
        return 0;
    }
    mlx5_dma_page_free(ctx, &eq->page);
    return rc;
}

static int mlx5_ctx_destroy_eq(struct mlx5_cmd_ctx *ctx, struct mlx5_eq_res *eq) {
    uint8_t in[16] = {0};
    uint8_t out[16] = {0};
    int rc = 0;

    if (!eq->valid) {
        mlx5_dma_page_free(ctx, &eq->page);
        return 0;
    }

    put_be16(in, MLX5_CMD_OP_DESTROY_EQ);
    in[0x0b] = eq->eqn;
    printf("  destroy eqn: %u\n", eq->eqn);
    rc = mlx5_cmd_exec(ctx, MLX5_CMD_OP_DESTROY_EQ, in, sizeof(in), out,
                       sizeof(out), "seq-destroy-eq");
    if (rc == 0) {
        eq->valid = 0;
    }
    mlx5_dma_page_free(ctx, &eq->page);
    return rc;
}

static int mlx5_ctx_create_cq(struct mlx5_cmd_ctx *ctx, uint32_t uar,
                              const struct mlx5_eq_res *eq,
                              struct mlx5_cq_res *cq, uint64_t cq_iova,
                              uint64_t dbr_iova) {
    uint8_t in[MLX5_CREATE_CQ_IN_SIZE] = {0};
    uint8_t out[16] = {0};
    uint8_t *cqc = in + MLX5_CREATE_CQ_CQC_OFF;
    int rc;

    memset(cq, 0, sizeof(*cq));
    if (!eq->valid) {
        fprintf(stderr, "cannot create CQ without a valid EQ\n");
        return -1;
    }
    if (mlx5_dma_page_alloc(ctx, &cq->cq_page, cq_iova, "CQ") != 0) {
        return -1;
    }
    memset(cq->cq_page.addr, 0xff, 4096);
    if (mlx5_dma_page_alloc(ctx, &cq->dbr_page, dbr_iova, "CQ DBR") != 0) {
        mlx5_dma_page_free(ctx, &cq->cq_page);
        return -1;
    }

    put_be16(in, MLX5_CMD_OP_CREATE_CQ);
    cqc[0x0c] = 4;                 /* log_cq_size=4 => 16 CQEs */
    cqc[0x0d] = (uint8_t)(uar >> 16);
    cqc[0x0e] = (uint8_t)(uar >> 8);
    cqc[0x0f] = (uint8_t)uar;
    put_be32(cqc + 0x14, eq->eqn); /* c_eqn_or_apu_element */
    cqc[0x18] = 0;                 /* log_page_size=0 => 4KB */
    put_be64(cqc + 0x38, cq->dbr_page.iova); /* dbr_addr */
    put_be64(in + MLX5_CREATE_CQ_PAS_OFF, cq->cq_page.iova);

    rc = mlx5_cmd_exec(ctx, MLX5_CMD_OP_CREATE_CQ, in, sizeof(in), out,
                       sizeof(out), "seq-create-cq");
    if (rc == 0) {
        cq->cqn = get_be32(out + 0x08) & 0x00ffffffu;
        cq->valid = 1;
        printf("  cqn: %" PRIu32 "\n", cq->cqn);
        return 0;
    }

    mlx5_dma_page_free(ctx, &cq->dbr_page);
    mlx5_dma_page_free(ctx, &cq->cq_page);
    return rc;
}

static int mlx5_ctx_destroy_cq(struct mlx5_cmd_ctx *ctx, struct mlx5_cq_res *cq) {
    uint8_t in[16] = {0};
    uint8_t out[16] = {0};
    int rc = 0;

    if (!cq->valid) {
        mlx5_dma_page_free(ctx, &cq->dbr_page);
        mlx5_dma_page_free(ctx, &cq->cq_page);
        return 0;
    }

    put_be16(in, MLX5_CMD_OP_DESTROY_CQ);
    put_be32(in + 0x08, cq->cqn & 0x00ffffffu);
    printf("  destroy cqn: %" PRIu32 "\n", cq->cqn);
    rc = mlx5_cmd_exec(ctx, MLX5_CMD_OP_DESTROY_CQ, in, sizeof(in), out,
                       sizeof(out), "seq-destroy-cq");
    if (rc == 0) {
        cq->valid = 0;
    }
    mlx5_dma_page_free(ctx, &cq->dbr_page);
    mlx5_dma_page_free(ctx, &cq->cq_page);
    return rc;
}

static int mlx5_ctx_create_sq(struct mlx5_cmd_ctx *ctx, uint32_t uar,
                              uint32_t pd, uint32_t tisn,
                              const struct mlx5_cq_res *cq,
                              struct mlx5_sq_res *sq) {
    uint8_t in[MLX5_CREATE_SQ_IN_SIZE] = {0};
    uint8_t out[16] = {0};
    uint8_t *sqc = in + MLX5_CREATE_SQ_SQC_OFF;
    uint8_t *wq = sqc + MLX5_SQC_WQ_OFF;
    const uint64_t sq_iova = 0x07300000ULL;
    const uint64_t dbr_iova = 0x07400000ULL;
    const uint64_t tx_iova = 0x07500000ULL;
    int rc;

    memset(sq, 0, sizeof(*sq));
    if (!cq->valid) {
        fprintf(stderr, "cannot create SQ without a valid CQ\n");
        return -1;
    }
    if (mlx5_dma_page_alloc(ctx, &sq->sq_page, sq_iova, "SQ") != 0) {
        return -1;
    }
    if (mlx5_dma_page_alloc(ctx, &sq->dbr_page, dbr_iova, "SQ DBR") != 0) {
        mlx5_dma_page_free(ctx, &sq->sq_page);
        return -1;
    }
    if (mlx5_dma_page_alloc(ctx, &sq->tx_page, tx_iova, "TX packet") != 0) {
        mlx5_dma_page_free(ctx, &sq->dbr_page);
        mlx5_dma_page_free(ctx, &sq->sq_page);
        return -1;
    }

    put_be16(in, MLX5_CMD_OP_CREATE_SQ);
    sqc[0x00] |= 0x10 | MLX5_INLINE_MODE_L2;
    put_be32(sqc + 0x08, cq->cqn & 0x00ffffffu);
    put_be16(sqc + 0x20, 1); /* tis_lst_sz */
    put_be32(sqc + 0x2c, tisn & 0x00ffffffu);

    wq[0x00] = (uint8_t)(MLX5_WQ_TYPE_CYCLIC << 4);
    put_be32(wq + 0x08, pd & 0x00ffffffu);
    put_be32(wq + 0x0c, uar & 0x00ffffffu);
    put_be64(wq + 0x10, sq->dbr_page.iova);
    wq[0x21] = MLX5_SEND_WQE_BB_LOG; /* log_wq_stride=6 => 64B */
    wq[0x23] = 4;                    /* log_wq_sz=4 => 16 WQEBBs */
    put_be64(in + MLX5_CREATE_SQ_PAS_OFF, sq->sq_page.iova);

    rc = mlx5_cmd_exec(ctx, MLX5_CMD_OP_CREATE_SQ, in, sizeof(in), out,
                       sizeof(out), "seq-create-sq");
    if (rc == 0) {
        sq->sqn = get_be32(out + 0x08) & 0x00ffffffu;
        sq->valid = 1;
        printf("  sqn: %" PRIu32 "\n", sq->sqn);
        return 0;
    }

    mlx5_dma_page_free(ctx, &sq->tx_page);
    mlx5_dma_page_free(ctx, &sq->dbr_page);
    mlx5_dma_page_free(ctx, &sq->sq_page);
    return rc;
}

static int mlx5_ctx_destroy_sq(struct mlx5_cmd_ctx *ctx, struct mlx5_sq_res *sq) {
    uint8_t in[16] = {0};
    uint8_t out[16] = {0};
    int rc = 0;

    if (!sq->valid) {
        mlx5_dma_page_free(ctx, &sq->tx_page);
        mlx5_dma_page_free(ctx, &sq->dbr_page);
        mlx5_dma_page_free(ctx, &sq->sq_page);
        return 0;
    }

    put_be16(in, MLX5_CMD_OP_DESTROY_SQ);
    put_be32(in + 0x08, sq->sqn & 0x00ffffffu);
    printf("  destroy sqn: %" PRIu32 "\n", sq->sqn);
    rc = mlx5_cmd_exec(ctx, MLX5_CMD_OP_DESTROY_SQ, in, sizeof(in), out,
                       sizeof(out), "seq-destroy-sq");
    if (rc == 0) {
        sq->valid = 0;
    }
    mlx5_dma_page_free(ctx, &sq->tx_page);
    mlx5_dma_page_free(ctx, &sq->dbr_page);
    mlx5_dma_page_free(ctx, &sq->sq_page);
    return rc;
}

static int mlx5_ctx_create_rq(struct mlx5_cmd_ctx *ctx, uint32_t pd,
                              uint8_t counter_set_id,
                              const struct mlx5_cq_res *cq,
                              struct mlx5_rq_res *rq) {
    uint8_t in[MLX5_CREATE_RQ_IN_SIZE] = {0};
    uint8_t out[16] = {0};
    uint8_t *rqc = in + MLX5_CREATE_RQ_RQC_OFF;
    uint8_t *wq = rqc + MLX5_RQC_WQ_OFF;
    const uint64_t rq_iova = 0x07600000ULL;
    const uint64_t dbr_iova = 0x07700000ULL;
    int rc;

    memset(rq, 0, sizeof(*rq));
    if (!cq->valid) {
        fprintf(stderr, "cannot create RQ without a valid CQ\n");
        return -1;
    }
    if (mlx5_dma_page_alloc(ctx, &rq->rq_page, rq_iova, "RQ") != 0) {
        return -1;
    }
    if (mlx5_dma_page_alloc(ctx, &rq->dbr_page, dbr_iova, "RQ DBR") != 0) {
        mlx5_dma_page_free(ctx, &rq->rq_page);
        return -1;
    }

    put_be16(in, MLX5_CMD_OP_CREATE_RQ);
    put_be32(rqc + 0x08, cq->cqn & 0x00ffffffu);
    rqc[0x0c] = counter_set_id;

    wq[0x00] = (uint8_t)(MLX5_WQ_TYPE_CYCLIC << 4);
    put_be32(wq + 0x08, pd & 0x00ffffffu);
    put_be64(wq + 0x10, rq->dbr_page.iova);
    wq[0x21] = 4; /* log_wq_stride=4 => 16B cyclic receive WQE */
    wq[0x23] = 4; /* log_wq_sz=4 => 16 entries */
    put_be64(in + MLX5_CREATE_RQ_PAS_OFF, rq->rq_page.iova);

    rc = mlx5_cmd_exec(ctx, MLX5_CMD_OP_CREATE_RQ, in, sizeof(in), out,
                       sizeof(out), "seq-create-rq");
    if (rc == 0) {
        rq->rqn = get_be32(out + 0x08) & 0x00ffffffu;
        rq->valid = 1;
        printf("  rqn: %" PRIu32 "\n", rq->rqn);
        return 0;
    }

    mlx5_dma_page_free(ctx, &rq->dbr_page);
    mlx5_dma_page_free(ctx, &rq->rq_page);
    return rc;
}

static int mlx5_ctx_modify_rq_ready(struct mlx5_cmd_ctx *ctx,
                                    const struct mlx5_rq_res *rq) {
    uint8_t in[MLX5_MODIFY_RQ_IN_SIZE] = {0};
    uint8_t out[16] = {0};
    uint8_t *rqc = in + MLX5_MODIFY_RQ_RQC_OFF;

    if (!rq->valid) {
        fprintf(stderr, "cannot modify RQ without a valid RQ\n");
        return -1;
    }

    put_be16(in, MLX5_CMD_OP_MODIFY_RQ);
    put_be32(in + 0x08,
             ((uint32_t)MLX5_RQC_STATE_RST << 28) |
                 (rq->rqn & 0x00ffffffu));
    rqc[0x01] = (uint8_t)(MLX5_RQC_STATE_RDY << 4);

    printf("  modify rqn to RDY: %" PRIu32 "\n", rq->rqn);
    return mlx5_cmd_exec(ctx, MLX5_CMD_OP_MODIFY_RQ, in, sizeof(in), out,
                         sizeof(out), "seq-modify-rq-rdy");
}

static int mlx5_rq_post_buffers(struct mlx5_cmd_ctx *ctx, struct mlx5_rq_res *rq,
                                uint32_t mkey, uint32_t count) {
    const uint64_t rx_iova_base = 0x07800000ULL;

    if (!rq->valid) {
        fprintf(stderr, "cannot post RX WQE without a valid RQ\n");
        return -1;
    }
    if (count == 0 || count > MLX5_RQ_WQE_COUNT) {
        fprintf(stderr, "unsupported RX post count %" PRIu32
                        "; current test supports 1..%u\n",
                count, MLX5_RQ_WQE_COUNT);
        return -1;
    }

    for (uint32_t i = 0; i < count; i++) {
        uint32_t pi = rq->prod_index;
        uint32_t wqe_slot = pi & (MLX5_RQ_WQE_COUNT - 1);
        struct mlx5_dma_page *rx_page = &rq->rx_pages[wqe_slot];
        uint64_t buf_iova;
        uint8_t *wqe = (uint8_t *)rq->rq_page.addr + wqe_slot * 16u;

        if (rx_page->addr == NULL) {
            uint64_t page_iova = rx_iova_base + (uint64_t)wqe_slot * 0x1000ULL;
            if (mlx5_dma_page_alloc(ctx, rx_page, page_iova, "RX packet") != 0) {
                return -1;
            }
        }
        memset(rx_page->addr, 0xcc, 4096);
        buf_iova = rx_page->iova;

        memset(wqe, 0, 16);
        put_be32(wqe + 0x00, MLX5_RX_BUFFER_SIZE | MLX5_HW_START_PADDING);
        put_be32(wqe + 0x04, mkey);
        put_be64(wqe + 0x08, buf_iova);
        rq->prod_index++;

        printf("  post RX WQE: rqn=%" PRIu32 " pi=%" PRIu32
               " new_pi=%" PRIu32 " wqe_slot=%" PRIu32
               " buf_slot=%" PRIu32 " rx_iova=0x%" PRIx64
               " len=%u lkey=0x%08" PRIx32 "\n",
               rq->rqn, pi, rq->prod_index, wqe_slot, wqe_slot, buf_iova,
               MLX5_RX_BUFFER_SIZE, mkey);
        dump_hex("  rx wqe", wqe, 16);
    }

    __sync_synchronize();
    /*
     * The mlx5 WQ DB record is a pair of 32-bit counters.  Existing SQ code
     * uses offset +4 for producer.  For RQ, write both offsets while this
     * bring-up code is still validating the exact receive DBR convention.
     */
    put_be32((uint8_t *)rq->dbr_page.addr, rq->prod_index & 0x00ffffffu);
    put_be32((uint8_t *)rq->dbr_page.addr + 4, rq->prod_index & 0x00ffffffu);
    __sync_synchronize();

    if (rq->rx_pages[0].addr != NULL) {
        dump_hex("  rx buffer[0] initial", rq->rx_pages[0].addr, 64);
    }
    return 0;
}

static int mlx5_ctx_destroy_rq(struct mlx5_cmd_ctx *ctx, struct mlx5_rq_res *rq) {
    uint8_t in[16] = {0};
    uint8_t out[16] = {0};
    int rc = 0;

    if (!rq->valid) {
        for (uint32_t i = 0; i < MLX5_RQ_WQE_COUNT; i++) {
            mlx5_dma_page_free(ctx, &rq->rx_pages[i]);
        }
        mlx5_dma_page_free(ctx, &rq->dbr_page);
        mlx5_dma_page_free(ctx, &rq->rq_page);
        return 0;
    }

    put_be16(in, MLX5_CMD_OP_DESTROY_RQ);
    put_be32(in + 0x08, rq->rqn & 0x00ffffffu);
    printf("  destroy rqn: %" PRIu32 "\n", rq->rqn);
    rc = mlx5_cmd_exec(ctx, MLX5_CMD_OP_DESTROY_RQ, in, sizeof(in), out,
                       sizeof(out), "seq-destroy-rq");
    if (rc == 0) {
        rq->valid = 0;
    }
    for (uint32_t i = 0; i < MLX5_RQ_WQE_COUNT; i++) {
        mlx5_dma_page_free(ctx, &rq->rx_pages[i]);
    }
    mlx5_dma_page_free(ctx, &rq->dbr_page);
    mlx5_dma_page_free(ctx, &rq->rq_page);
    return rc;
}

static int mlx5_ctx_create_rqt(struct mlx5_cmd_ctx *ctx,
                               const struct mlx5_rq_res *rq,
                               struct mlx5_rqt_res *rqt) {
    uint8_t in[MLX5_CREATE_RQT_IN_SIZE] = {0};
    uint8_t out[16] = {0};
    uint8_t *rqtc = in + MLX5_CREATE_RQT_RQTC_OFF;
    int rc;

    memset(rqt, 0, sizeof(*rqt));
    if (!rq->valid) {
        fprintf(stderr, "cannot create RQT without a valid RQ\n");
        return -1;
    }

    put_be16(in, MLX5_CMD_OP_CREATE_RQT);
    put_be16(rqtc + 0x16, MLX5_RQT_ENTRY_COUNT); /* rqt_max_size */
    put_be16(rqtc + 0x1a, MLX5_RQT_ENTRY_COUNT); /* rqt_actual_size */
    for (unsigned int i = 0; i < MLX5_RQT_ENTRY_COUNT; i++) {
        put_be32(rqtc + 0xf0 + i * 4, rq->rqn & 0x00ffffffu);
    }
    rc = mlx5_cmd_exec(ctx, MLX5_CMD_OP_CREATE_RQT, in, sizeof(in), out,
                       sizeof(out), "seq-create-rqt");
    if (rc == 0) {
        rqt->rqtn = get_be32(out + 0x08) & 0x00ffffffu;
        rqt->valid = 1;
        printf("  rqtn: %" PRIu32 " -> rqn %" PRIu32 "\n", rqt->rqtn,
               rq->rqn);
    }
    return rc;
}

static int mlx5_ctx_destroy_rqt(struct mlx5_cmd_ctx *ctx,
                                struct mlx5_rqt_res *rqt) {
    uint8_t in[16] = {0};
    uint8_t out[16] = {0};
    int rc = 0;

    if (!rqt->valid) {
        return 0;
    }

    put_be16(in, MLX5_CMD_OP_DESTROY_RQT);
    put_be32(in + 0x08, rqt->rqtn & 0x00ffffffu);
    printf("  destroy rqtn: %" PRIu32 "\n", rqt->rqtn);
    rc = mlx5_cmd_exec(ctx, MLX5_CMD_OP_DESTROY_RQT, in, sizeof(in), out,
                       sizeof(out), "seq-destroy-rqt");
    if (rc == 0) {
        rqt->valid = 0;
    }
    return rc;
}

static __attribute__((unused)) int
mlx5_ctx_query_rqt(struct mlx5_cmd_ctx *ctx, const struct mlx5_rqt_res *rqt,
                   const char *tag) {
    uint8_t in[16] = {0};
    uint8_t out[MLX5_QUERY_RQT_OUT_SIZE] = {0};
    uint8_t *rqtc = out + MLX5_QUERY_RQT_RQTC_OFF;
    int rc;

    if (!rqt->valid) {
        fprintf(stderr, "cannot query RQT without a valid RQT\n");
        return -1;
    }

    put_be16(in, MLX5_CMD_OP_QUERY_RQT);
    put_be32(in + 0x08, rqt->rqtn & 0x00ffffffu);
    rc = mlx5_cmd_exec(ctx, MLX5_CMD_OP_QUERY_RQT, in, sizeof(in), out,
                       sizeof(out), tag);
    if (rc == 0) {
        printf("  queried rqtn: %" PRIu32 "\n", rqt->rqtn);
        printf("  rqt max_size: %u\n", get_be16(rqtc + 0x16));
        printf("  rqt actual_size: %u\n", get_be16(rqtc + 0x1a));
        printf("  rqt rq_num[0]: %" PRIu32 "\n",
               get_be32(rqtc + 0xf0) & 0x00ffffffu);
        dump_hex("  rqtc prefix", rqtc, 128);
    }
    return rc;
}

static __attribute__((unused)) int
mlx5_ctx_create_direct_tir(struct mlx5_cmd_ctx *ctx, uint32_t tdn,
                           const struct mlx5_rq_res *rq,
                           struct mlx5_tir_res *tir) {
    uint8_t in[MLX5_CREATE_TIR_IN_SIZE] = {0};
    uint8_t out[16] = {0};
    uint8_t *tirc = in + MLX5_CREATE_TIR_TIRC_OFF;
    int rc;

    memset(tir, 0, sizeof(*tir));
    if (!rq->valid) {
        fprintf(stderr, "cannot create TIR without a valid RQ\n");
        return -1;
    }

    put_be16(in, MLX5_CMD_OP_CREATE_TIR);
    /* disp_type DIRECT and rx_hash_fn NONE are zero. */
    put_be32(tirc + 0x1c, rq->rqn & 0x00ffffffu); /* inline_rqn */
    tirc[0x25] = (uint8_t)(tdn >> 16);
    tirc[0x26] = (uint8_t)(tdn >> 8);
    tirc[0x27] = (uint8_t)tdn; /* transport_domain, 24-bit field */

    rc = mlx5_cmd_exec(ctx, MLX5_CMD_OP_CREATE_TIR, in, sizeof(in), out,
                       sizeof(out), "seq-create-direct-tir");
    if (rc == 0) {
        tir->tirn = get_be32(out + 0x08) & 0x00ffffffu;
        tir->valid = 1;
        printf("  tirn: %" PRIu32 "\n", tir->tirn);
    }
    return rc;
}

static int mlx5_ctx_create_indirect_tir(struct mlx5_cmd_ctx *ctx, uint32_t tdn,
                                        const struct mlx5_rqt_res *rqt,
                                        struct mlx5_tir_res *tir) {
    uint8_t in[MLX5_CREATE_TIR_IN_SIZE] = {0};
    uint8_t out[16] = {0};
    uint8_t *tirc = in + MLX5_CREATE_TIR_TIRC_OFF;
    int rc;

    memset(tir, 0, sizeof(*tir));
    if (!rqt->valid) {
        fprintf(stderr, "cannot create indirect TIR without a valid RQT\n");
        return -1;
    }

    put_be16(in, MLX5_CMD_OP_CREATE_TIR);
    tirc[0x04] = 0x10; /* disp_type INDIRECT */
    put_be32(tirc + 0x20, rqt->rqtn & 0x00ffffffu); /* indirect_table */
    tirc[0x24] = 0x10; /* rx_hash_fn INVERTED_XOR8 */
    tirc[0x25] = (uint8_t)(tdn >> 16);
    tirc[0x26] = (uint8_t)(tdn >> 8);
    tirc[0x27] = (uint8_t)tdn;

    rc = mlx5_cmd_exec(ctx, MLX5_CMD_OP_CREATE_TIR, in, sizeof(in), out,
                       sizeof(out), "seq-create-indirect-tir");
    if (rc == 0) {
        tir->tirn = get_be32(out + 0x08) & 0x00ffffffu;
        tir->valid = 1;
        printf("  indirect tirn: %" PRIu32 " -> rqtn %" PRIu32 "\n",
               tir->tirn, rqt->rqtn);
    }
    return rc;
}

static int mlx5_ctx_destroy_tir(struct mlx5_cmd_ctx *ctx,
                                struct mlx5_tir_res *tir) {
    uint8_t in[16] = {0};
    uint8_t out[16] = {0};
    int rc = 0;

    if (!tir->valid) {
        return 0;
    }

    put_be16(in, MLX5_CMD_OP_DESTROY_TIR);
    put_be32(in + 0x08, tir->tirn & 0x00ffffffu);
    printf("  destroy tirn: %" PRIu32 "\n", tir->tirn);
    rc = mlx5_cmd_exec(ctx, MLX5_CMD_OP_DESTROY_TIR, in, sizeof(in), out,
                       sizeof(out), "seq-destroy-tir");
    if (rc == 0) {
        tir->valid = 0;
    }
    return rc;
}

static __attribute__((unused)) int
mlx5_ctx_query_tir(struct mlx5_cmd_ctx *ctx, const struct mlx5_tir_res *tir,
                   const char *tag) {
    uint8_t in[16] = {0};
    uint8_t out[MLX5_QUERY_TIR_OUT_SIZE] = {0};
    uint8_t *tirc = out + MLX5_QUERY_TIR_TIRC_OFF;
    int rc;

    if (!tir->valid) {
        fprintf(stderr, "cannot query TIR without a valid TIR\n");
        return -1;
    }

    put_be16(in, MLX5_CMD_OP_QUERY_TIR);
    put_be32(in + 0x08, tir->tirn & 0x00ffffffu);
    rc = mlx5_cmd_exec(ctx, MLX5_CMD_OP_QUERY_TIR, in, sizeof(in), out,
                       sizeof(out), tag);
    if (rc == 0) {
        printf("  queried tirn: %" PRIu32 "\n", tir->tirn);
        printf("  tir disp_type: %u\n", (tirc[0x04] >> 4) & 0x0f);
        printf("  tir inline_rqn: %" PRIu32 "\n",
               get_be32(tirc + 0x1c) & 0x00ffffffu);
        printf("  tir transport_domain: %" PRIu32 "\n",
               get_be32(tirc + 0x24) & 0x00ffffffu);
        dump_hex("  tirc prefix", tirc, 96);
    }
    return rc;
}

static int mlx5_ctx_create_rx_flow_table(struct mlx5_cmd_ctx *ctx,
                                         struct mlx5_flow_table_res *ft) {
    uint8_t in[MLX5_CREATE_FLOW_TABLE_IN_SIZE] = {0};
    uint8_t out[16] = {0};
    uint8_t *ftc = in + MLX5_CREATE_FLOW_TABLE_FTC_OFF;
    int rc;

    memset(ft, 0, sizeof(*ft));

    put_be16(in, MLX5_CMD_OP_CREATE_FLOW_TABLE);
    in[0x10] = MLX5_FLOW_TABLE_TYPE_NIC_RX;
    ftc[0x01] = 0; /* level */
    ftc[0x03] = 0; /* log_size=0 => 1 FTE */

    rc = mlx5_cmd_exec(ctx, MLX5_CMD_OP_CREATE_FLOW_TABLE, in, sizeof(in),
                       out, sizeof(out), "seq-create-rx-flow-table");
    if (rc == 0) {
        ft->table_id = get_be32(out + 0x08) & 0x00ffffffu;
        ft->valid = 1;
        printf("  rx flow table id: %" PRIu32 "\n", ft->table_id);
    }
    return rc;
}

static int mlx5_ctx_destroy_rx_flow_table(struct mlx5_cmd_ctx *ctx,
                                          struct mlx5_flow_table_res *ft) {
    uint8_t in[32] = {0};
    uint8_t out[16] = {0};
    int rc;

    if (!ft->valid) {
        return 0;
    }

    put_be16(in, MLX5_CMD_OP_DESTROY_FLOW_TABLE);
    in[0x10] = MLX5_FLOW_TABLE_TYPE_NIC_RX;
    put_be32(in + 0x14, ft->table_id & 0x00ffffffu);
    printf("  destroy rx flow table id: %" PRIu32 "\n", ft->table_id);
    rc = mlx5_cmd_exec(ctx, MLX5_CMD_OP_DESTROY_FLOW_TABLE, in, sizeof(in),
                       out, sizeof(out), "seq-destroy-rx-flow-table");
    if (rc == 0) {
        ft->valid = 0;
    }
    return rc;
}

static int mlx5_ctx_create_rx_flow_group(struct mlx5_cmd_ctx *ctx,
                                         const struct mlx5_flow_table_res *ft,
                                         struct mlx5_flow_group_res *fg) {
    uint8_t in[MLX5_CREATE_FLOW_GROUP_IN_SIZE] = {0};
    uint8_t out[16] = {0};
    int rc;

    memset(fg, 0, sizeof(*fg));
    if (!ft->valid) {
        fprintf(stderr, "cannot create flow group without a valid flow table\n");
        return -1;
    }

    put_be16(in, MLX5_CMD_OP_CREATE_FLOW_GROUP);
    in[0x10] = MLX5_FLOW_TABLE_TYPE_NIC_RX;
    put_be32(in + 0x14, ft->table_id & 0x00ffffffu);
    put_be32(in + 0x18, 0); /* start_flow_index */
    put_be32(in + 0x20, 0); /* end_flow_index */
    in[0x3f] = 0;           /* match_criteria_enable=0 => match all */

    rc = mlx5_cmd_exec(ctx, MLX5_CMD_OP_CREATE_FLOW_GROUP, in, sizeof(in),
                       out, sizeof(out), "seq-create-rx-flow-group");
    if (rc == 0) {
        fg->group_id = get_be32(out + 0x08) & 0x00ffffffu;
        fg->valid = 1;
        printf("  rx flow group id: %" PRIu32 "\n", fg->group_id);
    }
    return rc;
}

static int mlx5_ctx_destroy_rx_flow_group(struct mlx5_cmd_ctx *ctx,
                                          const struct mlx5_flow_table_res *ft,
                                          struct mlx5_flow_group_res *fg) {
    uint8_t in[0x200] = {0};
    uint8_t out[16] = {0};
    int rc;

    if (!fg->valid) {
        return 0;
    }
    if (!ft->valid) {
        fprintf(stderr, "cannot destroy flow group without a valid flow table\n");
        return -1;
    }

    put_be16(in, MLX5_CMD_OP_DESTROY_FLOW_GROUP);
    in[0x10] = MLX5_FLOW_TABLE_TYPE_NIC_RX;
    put_be32(in + 0x14, ft->table_id & 0x00ffffffu);
    put_be32(in + 0x18, fg->group_id);
    printf("  destroy rx flow group id: %" PRIu32 "\n", fg->group_id);
    rc = mlx5_cmd_exec(ctx, MLX5_CMD_OP_DESTROY_FLOW_GROUP, in, sizeof(in),
                       out, sizeof(out), "seq-destroy-rx-flow-group");
    if (rc == 0) {
        fg->valid = 0;
    }
    return rc;
}

static int mlx5_ctx_set_rx_flow_table_root(struct mlx5_cmd_ctx *ctx,
                                           const struct mlx5_flow_table_res *ft) {
    uint8_t in[0x80] = {0};
    uint8_t out[0x80] = {0};

    if (!ft->valid) {
        fprintf(stderr, "cannot set root without a valid flow table\n");
        return -1;
    }

    put_be16(in, MLX5_CMD_OP_SET_FLOW_TABLE_ROOT);
    in[0x10] = MLX5_FLOW_TABLE_TYPE_NIC_RX;
    put_be32(in + 0x14, ft->table_id & 0x00ffffffu);
    printf("  set rx flow table root: table_id=%" PRIu32 "\n", ft->table_id);
    return mlx5_cmd_exec(ctx, MLX5_CMD_OP_SET_FLOW_TABLE_ROOT, in, sizeof(in),
                         out, sizeof(out), "seq-set-rx-flow-table-root");
}

static int mlx5_ctx_set_rx_fte_to_tir(struct mlx5_cmd_ctx *ctx,
                                      const struct mlx5_flow_table_res *ft,
                                      const struct mlx5_flow_group_res *fg,
                                      const struct mlx5_tir_res *tir,
                                      struct mlx5_fte_res *fte) {
    uint8_t in[MLX5_SET_FTE_IN_SIZE] = {0};
    uint8_t out[16] = {0};
    uint8_t *fc = in + 0x40;
    uint8_t *dst = fc + 0x300;
    int rc;

    memset(fte, 0, sizeof(*fte));
    if (!ft->valid || !fg->valid || !tir->valid) {
        fprintf(stderr, "cannot set FTE without valid FT/FG/TIR\n");
        return -1;
    }

    put_be16(in, MLX5_CMD_OP_SET_FLOW_TABLE_ENTRY);
    in[0x10] = MLX5_FLOW_TABLE_TYPE_NIC_RX;
    put_be32(in + 0x14, ft->table_id & 0x00ffffffu);
    put_be32(in + 0x20, 0); /* flow_index */

    put_be32(fc + 0x04, fg->group_id);
    /*
     * flow_context.action starts at bit offset 0x70 and is 16 bits wide.
     * In mlx5_ifc big-endian bitfield encoding that lands in the low 16 bits
     * of the dword at flow_context+0x0c, not at byte offset 0x0c as a BE16.
     */
    put_be32(fc + 0x0c, MLX5_FLOW_CONTEXT_ACTION_FWD_DEST);
    /*
     * flow_source starts at flow_context bit 0x82 and destination_list_size is
     * the low 24 bits of the same dword.
     */
    put_be32(fc + 0x10,
             ((uint32_t)MLX5_FLOW_CONTEXT_SOURCE_UPLINK << 28) | 1u);

    put_be32(dst + 0x00,
             ((uint32_t)MLX5_FLOW_DESTINATION_TYPE_TIR << 24) |
                 (tir->tirn & 0x00ffffffu));

    rc = mlx5_cmd_exec(ctx, MLX5_CMD_OP_SET_FLOW_TABLE_ENTRY, in, sizeof(in),
                       out, sizeof(out), "seq-set-rx-fte-tir");
    if (rc == 0) {
        fte->flow_index = 0;
        fte->valid = 1;
        printf("  rx fte index: %" PRIu32 " -> tirn %" PRIu32 "\n",
               fte->flow_index, tir->tirn);
    }
    return rc;
}

static int mlx5_ctx_delete_rx_fte(struct mlx5_cmd_ctx *ctx,
                                  const struct mlx5_flow_table_res *ft,
                                  struct mlx5_fte_res *fte) {
    uint8_t in[0x40] = {0};
    uint8_t out[16] = {0};
    int rc;

    if (!fte->valid) {
        return 0;
    }
    if (!ft->valid) {
        fprintf(stderr, "cannot delete FTE without a valid flow table\n");
        return -1;
    }

    put_be16(in, MLX5_CMD_OP_DELETE_FLOW_TABLE_ENTRY);
    in[0x10] = MLX5_FLOW_TABLE_TYPE_NIC_RX;
    put_be32(in + 0x14, ft->table_id & 0x00ffffffu);
    put_be32(in + 0x20, fte->flow_index);
    printf("  delete rx fte index: %" PRIu32 "\n", fte->flow_index);
    rc = mlx5_cmd_exec(ctx, MLX5_CMD_OP_DELETE_FLOW_TABLE_ENTRY, in,
                       sizeof(in), out, sizeof(out), "seq-delete-rx-fte");
    if (rc == 0) {
        fte->valid = 0;
    }
    return rc;
}

static int mlx5_ctx_modify_sq_ready(struct mlx5_cmd_ctx *ctx,
                                    const struct mlx5_sq_res *sq) {
    uint8_t in[MLX5_MODIFY_SQ_IN_SIZE] = {0};
    uint8_t out[16] = {0};
    uint8_t *sqc = in + MLX5_MODIFY_SQ_SQC_OFF;

    if (!sq->valid) {
        fprintf(stderr, "cannot modify SQ without a valid SQ\n");
        return -1;
    }

    put_be16(in, MLX5_CMD_OP_MODIFY_SQ);
    put_be32(in + 0x08,
             ((uint32_t)MLX5_SQC_STATE_RST << 28) |
                 (sq->sqn & 0x00ffffffu));
    sqc[0x01] = (uint8_t)(MLX5_SQC_STATE_RDY << 4);

    printf("  modify sqn to RDY: %" PRIu32 "\n", sq->sqn);
    return mlx5_cmd_exec(ctx, MLX5_CMD_OP_MODIFY_SQ, in, sizeof(in), out,
                         sizeof(out), "seq-modify-sq-rdy");
}

static __attribute__((unused)) int
mlx5_ctx_query_sq(struct mlx5_cmd_ctx *ctx, const struct mlx5_sq_res *sq,
                  const char *tag) {
    uint8_t in[16] = {0};
    uint8_t out[MLX5_QUERY_SQ_OUT_SIZE] = {0};
    uint8_t *sqc = out + MLX5_QUERY_SQ_SQC_OFF;
    int rc;

    if (!sq->valid) {
        fprintf(stderr, "cannot query SQ without a valid SQ\n");
        return -1;
    }

    put_be16(in, MLX5_CMD_OP_QUERY_SQ);
    put_be32(in + 0x08, sq->sqn & 0x00ffffffu);
    rc = mlx5_cmd_exec(ctx, MLX5_CMD_OP_QUERY_SQ, in, sizeof(in), out,
                       sizeof(out), tag);
    if (rc == 0) {
        printf("  queried sqn: %" PRIu32 "\n", sq->sqn);
        printf("  sqc[0x00]: 0x%02x\n", sqc[0x00]);
        printf("  sq state: %u\n", (sqc[0x01] >> 4) & 0x0f);
        printf("  sq cqn: %" PRIu32 "\n", get_be32(sqc + 0x08) & 0x00ffffffu);
        printf("  sq tis_lst_sz: %u\n", get_be16(sqc + 0x20));
        printf("  sq tis_num_0: %" PRIu32 "\n",
               get_be32(sqc + 0x2c) & 0x00ffffffu);
    }
    return rc;
}

static __attribute__((unused)) int
mlx5_ctx_query_rq(struct mlx5_cmd_ctx *ctx, const struct mlx5_rq_res *rq,
                  const char *tag) {
    uint8_t in[16] = {0};
    uint8_t out[MLX5_QUERY_RQ_OUT_SIZE] = {0};
    uint8_t *rqc = out + MLX5_QUERY_RQ_RQC_OFF;
    uint8_t *wq = rqc + MLX5_RQC_WQ_OFF;
    int rc;

    if (!rq->valid) {
        fprintf(stderr, "cannot query RQ without a valid RQ\n");
        return -1;
    }

    put_be16(in, MLX5_CMD_OP_QUERY_RQ);
    put_be32(in + 0x08, rq->rqn & 0x00ffffffu);
    rc = mlx5_cmd_exec(ctx, MLX5_CMD_OP_QUERY_RQ, in, sizeof(in), out,
                       sizeof(out), tag);
    if (rc == 0) {
        printf("  queried rqn: %" PRIu32 "\n", rq->rqn);
        printf("  rqc[0x00]: 0x%02x\n", rqc[0x00]);
        printf("  rq state: %u\n", (rqc[0x01] >> 4) & 0x0f);
        printf("  rq mem_rq_type: %u\n", rqc[0x00] & 0x0f);
        printf("  rq cqn: %" PRIu32 "\n", get_be32(rqc + 0x08) & 0x00ffffffu);
        printf("  rq counter_set_id: %u\n", rqc[0x0c]);
        printf("  rq wq_type: %u\n", (wq[0x00] >> 4) & 0x0f);
        printf("  rq wq pd: %" PRIu32 "\n", get_be32(wq + 0x08) & 0x00ffffffu);
        printf("  rq wq dbr_addr: 0x%016" PRIx64 "\n", get_be64(wq + 0x10));
        printf("  rq wq hw_counter: %" PRIu32 "\n", get_be32(wq + 0x18));
        printf("  rq wq sw_counter: %" PRIu32 "\n", get_be32(wq + 0x1c));
        printf("  rq wq log_stride: %u\n", wq[0x21] & 0x0f);
        printf("  rq wq log_size: %u\n", wq[0x23] & 0x1f);
        dump_hex("  rqc prefix", rqc, 128);
    }
    return rc;
}

static int mlx5_poll_cq_once(struct mlx5_cq_res *cq, const char *tag,
                             uint32_t *byte_cnt_out,
                             uint16_t *wqe_counter_out) {
    uint8_t *cqe = (uint8_t *)cq->cq_page.addr +
                   ((cq->cons_index & (MLX5_CQ_CQE_COUNT - 1)) * MLX5_CQE_SIZE);
    uint8_t op_own = load_u8(cqe + 0x3f);
    uint8_t opcode = op_own >> 4;
    uint8_t owner = op_own & 0x01u;
    uint8_t expected_owner =
        (uint8_t)((cq->cons_index / MLX5_CQ_CQE_COUNT) & 0x01u);
    uint32_t byte_cnt = get_be32(cqe + 0x2c);
    uint16_t wqe_counter = get_be16(cqe + 0x3c);

    if (owner != expected_owner) {
        return 0;
    }

    printf("%s CQE\n", tag);
    printf("  cqe index: %" PRIu32 "\n", cq->cons_index);
    printf("  op_own: 0x%02x\n", op_own);
    printf("  opcode: 0x%02x\n", opcode);
    if (opcode == MLX5_CQE_REQ_ERR || opcode == MLX5_CQE_RESP_ERR) {
        printf("  syndrome: 0x%02x\n", load_u8(cqe + 0x36));
        printf("  vendor syndrome: 0x%02x\n", load_u8(cqe + 0x37));
    } else {
        printf("  byte_cnt: %" PRIu32 "\n", byte_cnt);
        printf("  wqe_counter: %" PRIu16 "\n", wqe_counter);
        printf("  signature: 0x%02x\n", load_u8(cqe + 0x3e));
    }
    dump_hex("  cqe", cqe, MLX5_CQE_SIZE);
    cq->cons_index++;
    put_be32((uint8_t *)cq->dbr_page.addr, cq->cons_index & 0x00ffffffu);
    __sync_synchronize();
    if (opcode == MLX5_CQE_REQ_ERR || opcode == MLX5_CQE_RESP_ERR) {
        fprintf(stderr, "%s got error CQE: syndrome=0x%02x vendor=0x%02x\n",
                tag, load_u8(cqe + 0x36), load_u8(cqe + 0x37));
        return -1;
    }
    if (byte_cnt_out != NULL) {
        *byte_cnt_out = byte_cnt;
    }
    if (wqe_counter_out != NULL) {
        *wqe_counter_out = wqe_counter;
    }
    return 1;
}

static int mlx5_poll_cq(struct mlx5_cq_res *cq, const char *tag,
                        int timeout_ms, uint32_t *byte_cnt_out,
                        uint16_t *wqe_counter_out) {
    int elapsed = 0;

    while (elapsed <= timeout_ms) {
        int got =
            mlx5_poll_cq_once(cq, tag, byte_cnt_out, wqe_counter_out);
        if (got > 0) {
            return 0;
        }
        if (got < 0) {
            return -1;
        }
        sleep_ms(1);
        elapsed++;
    }

    fprintf(stderr, "%s timeout waiting for CQE\n", tag);
    return -1;
}

static int mlx5_rx_poll_one(struct mlx5_cq_res *cq, struct mlx5_rq_res *rq,
                            int timeout_ms, struct mlx5_rx_packet *pkt) {
    int rc;
    uint32_t byte_cnt = 0;
    uint16_t wqe_counter = 0;
    uint32_t slot;
    struct mlx5_dma_page *rx_page;

    if (pkt == NULL) {
        fprintf(stderr, "cannot poll RX into a NULL packet result\n");
        return -1;
    }
    memset(pkt, 0, sizeof(*pkt));
    if (!rq->valid) {
        fprintf(stderr, "cannot poll RX without a valid RQ\n");
        return -1;
    }

    rc = mlx5_poll_cq(cq, "seq-rx-wait", timeout_ms, &byte_cnt, &wqe_counter);
    if (rc != 0) {
        return rc;
    }

    slot = wqe_counter & (MLX5_RQ_WQE_COUNT - 1);
    rx_page = &rq->rx_pages[slot];
    if (rx_page->addr == NULL) {
        fprintf(stderr, "RX CQE referenced unallocated buffer slot %" PRIu32
                        "\n",
                slot);
        return -1;
    }

    pkt->data = (uint8_t *)rx_page->addr;
    pkt->len = byte_cnt;
    pkt->slot = slot;
    pkt->wqe_counter = wqe_counter;
    return 0;
}

static int mlx5_rx_replenish_one(struct mlx5_cmd_ctx *ctx,
                                 struct mlx5_rq_res *rq, uint32_t mkey) {
    int rc = mlx5_rq_post_buffers(ctx, rq, mkey, 1);
    if (rc != 0) {
        return rc;
    }
    printf("  rx replenish complete: prod_index=%" PRIu32 "\n",
           rq->prod_index);
    return 0;
}

static __attribute__((unused)) void
mlx5_dump_cq_slots(struct mlx5_cq_res *cq, uint32_t first, uint32_t count,
                   const char *tag) {
    printf("%s CQ slots dump\n", tag);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t ix = (first + i) & (MLX5_CQ_CQE_COUNT - 1);
        uint8_t *cqe = (uint8_t *)cq->cq_page.addr + ix * MLX5_CQE_SIZE;
        printf("  slot %" PRIu32 " op_own=0x%02x\n", ix, load_u8(cqe + 0x3f));
        dump_hex("  cqe", cqe, MLX5_CQE_SIZE);
    }
}

static __attribute__((unused)) int
mlx5_sq_post_nop_tag(struct mlx5_cmd_ctx *ctx, uint32_t uar,
                     struct mlx5_cq_res *cq, struct mlx5_sq_res *sq,
                     const char *tag) {
    uint8_t *wqe = (uint8_t *)sq->sq_page.addr +
                   ((sq->prod_index & (MLX5_SQ_WQE_COUNT - 1)) <<
                    MLX5_SEND_WQE_BB_LOG);
    uint64_t uar_off = ((uint64_t)uar * MLX5_UAR_PAGE_SIZE) + MLX5_BF_OFFSET;
    uint32_t pi = sq->prod_index;
    uint32_t ds = 1; /* one 16-byte control segment */

    if (uar_off + 64 > ctx->dev.bar0_size) {
        fprintf(stderr,
                "UAR BF offset outside BAR0: uar=%" PRIu32
                " off=0x%" PRIx64 " bar0_size=0x%" PRIx64 "\n",
                uar, uar_off, ctx->dev.bar0_size);
        return -1;
    }

    memset(wqe, 0, 64);
    put_be32(wqe + 0x00, (pi << 8) | MLX5_OPCODE_NOP);
    put_be32(wqe + 0x04, ((sq->sqn & 0x00ffffffu) << 8) | ds);
    wqe[0x0b] = MLX5_WQE_CTRL_CQ_UPDATE;

    sq->prod_index++;
    put_be32((uint8_t *)sq->dbr_page.addr + 4, sq->prod_index & 0x00ffffffu);
    __sync_synchronize();

    printf("  post NOP WQE: sqn=%" PRIu32 " pi=%" PRIu32
           " new_pi=%" PRIu32 " uar_off=0x%" PRIx64 "\n",
           sq->sqn, pi, sq->prod_index, uar_off);

    {
        uint64_t word;
        memcpy(&word, wqe, sizeof(word));
        mmio_write64_native(ctx->dev.bar0, uar_off, word);
    }
    __sync_synchronize();

    return mlx5_poll_cq(cq, tag, 1000, NULL, NULL);
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static int parse_mac_addr(const char *s, uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) {
        int hi;
        int lo;

        if (s == NULL || s[0] == '\0' || s[1] == '\0') {
            return -1;
        }
        hi = hex_nibble(s[0]);
        lo = hex_nibble(s[1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        mac[i] = (uint8_t)((hi << 4) | lo);
        s += 2;
        if (i != 5) {
            if (*s != ':') {
                return -1;
            }
            s++;
        }
    }
    return *s == '\0' ? 0 : -1;
}

static int parse_ethertype16(const char *s, uint16_t *ethertype) {
    char *end = NULL;
    unsigned long value;

    if (s == NULL) {
        return -1;
    }
    value = strtoul(s, &end, 0);
    if (end == s || *end != '\0' || value > 0xfffful) {
        return -1;
    }
    *ethertype = (uint16_t)value;
    return 0;
}

static int parse_payload_hex(const char *s, uint8_t *payload, size_t max_len,
                             size_t *payload_len) {
    size_t len = 0;

    if (s == NULL) {
        *payload_len = 0;
        return 0;
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    while (*s != '\0') {
        int hi;
        int lo;

        if (s[0] == ':' || s[0] == ' ' || s[0] == '_' || s[0] == '-') {
            s++;
            continue;
        }
        if (s[0] == '\0' || s[1] == '\0') {
            return -1;
        }
        if (len >= max_len) {
            return -1;
        }
        hi = hex_nibble(s[0]);
        lo = hex_nibble(s[1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        payload[len++] = (uint8_t)((hi << 4) | lo);
        s += 2;
    }
    *payload_len = len;
    return 0;
}

static int build_tx_test_frame(const struct mlx5_tx_test_opts *opts,
                               uint8_t *frame, size_t frame_cap,
                               size_t *frame_len) {
    static const uint8_t default_payload[46] = {
        'm',  'l',  'x',  'n',  'i',  'c',  'd',  '-',
        't',  'x',  '-',  '0',  '0',  '0',  '1',  0x00,
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d,
    };
    uint8_t dst[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    uint8_t src[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    uint8_t payload[MLX5_TX_TEST_MAX_INLINE_FRAME - 14] = {0};
    size_t payload_len = sizeof(default_payload);
    uint16_t ethertype = 0x88b5;
    size_t len;

    if (frame_cap < MLX5_TX_TEST_MAX_INLINE_FRAME) {
        return -1;
    }
    if (opts != NULL && opts->dst_mac != NULL &&
        parse_mac_addr(opts->dst_mac, dst) != 0) {
        fprintf(stderr, "invalid --dst MAC: %s\n", opts->dst_mac);
        return -1;
    }
    if (opts != NULL && opts->src_mac != NULL &&
        parse_mac_addr(opts->src_mac, src) != 0) {
        fprintf(stderr, "invalid --src MAC: %s\n", opts->src_mac);
        return -1;
    }
    if (opts != NULL && opts->ethertype != NULL &&
        parse_ethertype16(opts->ethertype, &ethertype) != 0) {
        fprintf(stderr, "invalid --ethertype: %s\n", opts->ethertype);
        return -1;
    }
    memcpy(payload, default_payload, sizeof(default_payload));
    if (opts != NULL && opts->payload_hex != NULL) {
        if (parse_payload_hex(opts->payload_hex, payload, sizeof(payload),
                              &payload_len) != 0) {
            fprintf(stderr,
                    "invalid --payload-hex or too large; max payload is %zu bytes\n",
                    sizeof(payload));
            return -1;
        }
    }

    len = 14 + payload_len;
    if (len < MLX5_TX_TEST_MIN_FRAME) {
        len = MLX5_TX_TEST_MIN_FRAME;
    }
    if (len > MLX5_TX_TEST_MAX_INLINE_FRAME) {
        fprintf(stderr, "TX frame too large for current full-inline path: %zu > %u\n",
                len, MLX5_TX_TEST_MAX_INLINE_FRAME);
        return -1;
    }

    memset(frame, 0, frame_cap);
    memcpy(frame + 0, dst, sizeof(dst));
    memcpy(frame + 6, src, sizeof(src));
    put_be16(frame + 12, ethertype);
    memcpy(frame + 14, payload, payload_len);
    *frame_len = len;
    return 0;
}

static __attribute__((unused)) size_t
trim_trailing_zero_bytes(const uint8_t *buf, size_t len, size_t min_len) {
    while (len > min_len && buf[len - 1] == 0x00) {
        len--;
    }
    return len;
}

static int mlx5_sq_post_send_raw(struct mlx5_cmd_ctx *ctx, uint32_t uar,
                                 uint32_t tisn, uint32_t mkey,
                                 struct mlx5_cq_res *cq,
                                 struct mlx5_sq_res *sq, const uint8_t *frame,
                                 size_t frame_len) {
    uint8_t *wqe = (uint8_t *)sq->sq_page.addr +
                   ((sq->prod_index & (MLX5_SQ_WQE_COUNT - 1)) <<
                    MLX5_SEND_WQE_BB_LOG);
    uint64_t uar_off = ((uint64_t)uar * MLX5_UAR_PAGE_SIZE) + MLX5_BF_OFFSET;
    uint32_t pi = sq->prod_index;
    uint32_t ds;
    uint32_t wqebbs;
    (void)mkey;

    if (frame_len < MLX5_TX_TEST_MIN_FRAME ||
        frame_len > MLX5_TX_TEST_MAX_INLINE_FRAME) {
        fprintf(stderr, "invalid TX frame length: %zu\n", frame_len);
        return -1;
    }
    ds = (uint32_t)((30 + frame_len + 15) / 16);
    wqebbs = (ds + 3) / 4;
    if (wqebbs != 2) {
        fprintf(stderr, "unexpected full-inline WQEBB count: ds=%" PRIu32
                " wqebbs=%" PRIu32 "\n",
                ds, wqebbs);
        return -1;
    }
    if (uar_off + 8 > ctx->dev.bar0_size) {
        fprintf(stderr,
                "UAR BF offset outside BAR0: uar=%" PRIu32
                " off=0x%" PRIx64 " bar0_size=0x%" PRIx64 "\n",
                uar, uar_off, ctx->dev.bar0_size);
        return -1;
    }

    memset(wqe, 0, wqebbs << MLX5_SEND_WQE_BB_LOG);
    put_be32(wqe + 0x00, (pi << 8) | MLX5_OPCODE_SEND);
    put_be32(wqe + 0x04, ((sq->sqn & 0x00ffffffu) << 8) | ds);
    wqe[0x0b] = MLX5_WQE_CTRL_CQ_UPDATE;
    put_be32(wqe + 0x0c, tisn & 0x00ffffffu);

    put_be16(wqe + 0x1c, (uint16_t)frame_len);
    memcpy(wqe + 0x1e, frame, 2);
    memcpy(wqe + 0x20, frame + 2, frame_len - 2);

    __sync_synchronize();
    sq->prod_index += wqebbs;
    put_be32((uint8_t *)sq->dbr_page.addr + 4, sq->prod_index & 0x00ffffffu);
    __sync_synchronize();

    printf("  post SEND WQE: sqn=%" PRIu32 " tisn=%" PRIu32
           " inline_len=%zu pi=%" PRIu32
           " new_pi=%" PRIu32 " uar_off=0x%" PRIx64 "\n",
           sq->sqn, tisn, frame_len, pi, sq->prod_index, uar_off);
    dump_hex("  send frame", frame, frame_len);
    dump_hex("  send wqe", wqe, wqebbs << MLX5_SEND_WQE_BB_LOG);

    {
        uint64_t word;

        memcpy(&word, wqe, sizeof(word));
        mmio_write64_native(ctx->dev.bar0, uar_off, word);
    }
    __sync_synchronize();

    return mlx5_poll_cq(cq, "seq-post-send", 1000, NULL, NULL);
}

static int mlx5_ctx_set_hca_cap_raw(struct mlx5_cmd_ctx *ctx, uint16_t cap_type,
                                    const uint8_t *cap, size_t cap_len) {
    uint8_t in[0x10 + MLX5_HCA_CAP_SIZE] = {0};
    uint8_t out[16] = {0};

    if (cap_len > MLX5_HCA_CAP_SIZE) {
        return -1;
    }
    put_be16(in, MLX5_CMD_OP_SET_HCA_CAP);
    put_be16(in + 0x06, cap_type << 1);
    memcpy(in + 0x10, cap, cap_len);
    return mlx5_cmd_exec(ctx, MLX5_CMD_OP_SET_HCA_CAP, in, 0x10 + cap_len,
                         out, sizeof(out), "seq-set-hca-cap-general");
}

static void mlx5_test_runtime_init(struct mlx5_test_runtime *rt) {
    memset(rt, 0, sizeof(*rt));
}

static void mlx5_test_runtime_cleanup(struct mlx5_test_runtime *rt, int *rc_io) {
    int rc = rc_io != NULL ? *rc_io : 0;

    if (rt->fte.valid) {
        int cleanup_rc = mlx5_ctx_delete_rx_fte(&rt->ctx, &rt->ft, &rt->fte);
        if (rc == 0 && cleanup_rc != 0) {
            rc = cleanup_rc;
        }
    }
    if (rt->fg.valid) {
        int cleanup_rc =
            mlx5_ctx_destroy_rx_flow_group(&rt->ctx, &rt->ft, &rt->fg);
        if (rc == 0 && cleanup_rc != 0) {
            rc = cleanup_rc;
        }
    }
    if (rt->ft.valid) {
        int cleanup_rc = mlx5_ctx_destroy_rx_flow_table(&rt->ctx, &rt->ft);
        if (rc == 0 && cleanup_rc != 0) {
            rc = cleanup_rc;
        }
    }
    if (rt->tir.valid) {
        int cleanup_rc = mlx5_ctx_destroy_tir(&rt->ctx, &rt->tir);
        if (rc == 0 && cleanup_rc != 0) {
            rc = cleanup_rc;
        }
    }
    if (rt->rqt.valid) {
        int cleanup_rc = mlx5_ctx_destroy_rqt(&rt->ctx, &rt->rqt);
        if (rc == 0 && cleanup_rc != 0) {
            rc = cleanup_rc;
        }
    }
    if (rt->rq.valid || rt->rq.rq_page.addr != NULL || rt->rq.dbr_page.addr != NULL ||
        rt->rq.rx_pages[0].addr != NULL) {
        int cleanup_rc = mlx5_ctx_destroy_rq(&rt->ctx, &rt->rq);
        if (rc == 0 && cleanup_rc != 0) {
            rc = cleanup_rc;
        }
    }
    if (rt->sq.valid || rt->sq.sq_page.addr != NULL || rt->sq.dbr_page.addr != NULL ||
        rt->sq.tx_page.addr != NULL) {
        int cleanup_rc = mlx5_ctx_destroy_sq(&rt->ctx, &rt->sq);
        if (rc == 0 && cleanup_rc != 0) {
            rc = cleanup_rc;
        }
    }
    if (rt->rx_cq.valid || rt->rx_cq.cq_page.addr != NULL ||
        rt->rx_cq.dbr_page.addr != NULL) {
        int cleanup_rc = mlx5_ctx_destroy_cq(&rt->ctx, &rt->rx_cq);
        if (rc == 0 && cleanup_rc != 0) {
            rc = cleanup_rc;
        }
    }
    if (rt->tx_cq.valid || rt->tx_cq.cq_page.addr != NULL ||
        rt->tx_cq.dbr_page.addr != NULL) {
        int cleanup_rc = mlx5_ctx_destroy_cq(&rt->ctx, &rt->tx_cq);
        if (rc == 0 && cleanup_rc != 0) {
            rc = cleanup_rc;
        }
    }
    if (rt->eq.valid || rt->eq.page.addr != NULL) {
        int cleanup_rc = mlx5_ctx_destroy_eq(&rt->ctx, &rt->eq);
        if (rc == 0 && cleanup_rc != 0) {
            rc = cleanup_rc;
        }
    }
    if (rt->have_tisn) {
        int cleanup_rc = mlx5_ctx_destroy_tis(&rt->ctx, rt->tisn);
        if (rc == 0 && cleanup_rc != 0) {
            rc = cleanup_rc;
        }
    }
    if (rt->mkey != 0) {
        int cleanup_rc = mlx5_ctx_destroy_mkey(&rt->ctx, rt->mkey);
        if (rc == 0 && cleanup_rc != 0) {
            rc = cleanup_rc;
        }
    }
    if (rt->have_tdn) {
        int cleanup_rc = mlx5_ctx_dealloc_transport_domain(&rt->ctx, rt->tdn);
        if (rc == 0 && cleanup_rc != 0) {
            rc = cleanup_rc;
        }
    }
    if (rt->have_q_counter) {
        int cleanup_rc = mlx5_ctx_dealloc_q_counter(&rt->ctx, rt->q_counter);
        if (rc == 0 && cleanup_rc != 0) {
            rc = cleanup_rc;
        }
    }
    if (rt->pd != 0) {
        int cleanup_rc = mlx5_ctx_dealloc_pd(&rt->ctx, rt->pd);
        if (rc == 0 && cleanup_rc != 0) {
            rc = cleanup_rc;
        }
    }
    if (rt->uar != 0) {
        int cleanup_rc = mlx5_ctx_dealloc_uar(&rt->ctx, rt->uar);
        if (rc == 0 && cleanup_rc != 0) {
            rc = cleanup_rc;
        }
    }
    mlx5_cmd_ctx_close(&rt->ctx);
    if (rc_io != NULL) {
        *rc_io = rc;
    }
}

void mlxnicd_dev_config_init(struct mlxnicd_dev_config *config) {
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->rx_post_count = MLX5_RX_TEST_POST_COUNT;
}

int mlxnicd_dev_open(struct mlxnicd_dev **out, const char *bdf) {
    struct mlxnicd_dev *dev;
    size_t len;

    if (out == NULL || bdf == NULL || bdf[0] == '\0') {
        fprintf(stderr, "mlxnicd_dev_open requires output pointer and BDF\n");
        return -1;
    }
    len = strlen(bdf);
    if (len >= sizeof(dev->bdf)) {
        fprintf(stderr, "BDF is too long: %s\n", bdf);
        return -1;
    }

    dev = calloc(1, sizeof(*dev));
    if (dev == NULL) {
        perror("calloc mlxnicd_dev");
        return -1;
    }
    memcpy(dev->bdf, bdf, len + 1);
    mlxnicd_dev_config_init(&dev->config);
    mlx5_test_runtime_init(&dev->rt);
    *out = dev;
    return 0;
}

int mlxnicd_dev_configure(struct mlxnicd_dev *dev,
                          const struct mlxnicd_dev_config *config) {
    if (dev == NULL || config == NULL) {
        fprintf(stderr, "mlxnicd_dev_configure requires device and config\n");
        mlxnicd_set_error(dev, MLXNICD_ERR_INVAL);
        return -1;
    }
    if (dev->started) {
        fprintf(stderr, "cannot configure a started mlxnicd device\n");
        mlxnicd_set_error(dev, MLXNICD_ERR_STATE);
        return -1;
    }
    if ((config->flags & (MLXNICD_DEV_F_TX | MLXNICD_DEV_F_RX)) == 0) {
        fprintf(stderr, "mlxnicd device must enable TX and/or RX\n");
        mlxnicd_set_error(dev, MLXNICD_ERR_INVAL);
        return -1;
    }
    if (config->rx_post_count > MLX5_RQ_WQE_COUNT) {
        fprintf(stderr, "rx_post_count=%" PRIu32 " exceeds max=%u\n",
                config->rx_post_count, MLX5_RQ_WQE_COUNT);
        mlxnicd_set_error(dev, MLXNICD_ERR_INVAL);
        return -1;
    }
    dev->config = *config;
    if (dev->config.rx_post_count == 0) {
        dev->config.rx_post_count = MLX5_RX_TEST_POST_COUNT;
    }
    dev->last_error = MLXNICD_OK;
    return 0;
}

void mlxnicd_dev_stop(struct mlxnicd_dev *dev) {
    int rc = 0;

    if (dev == NULL) {
        return;
    }
    if (dev->started) {
        mlx5_test_runtime_cleanup(&dev->rt, &rc);
        dev->last_error = mlxnicd_err_from_rc(rc);
        dev->started = 0;
        dev->rx_outstanding = 0;
        g_mlx5_stdout_quiet = dev->quiet_saved;
        vfio_set_stdout_quiet(0);
        mlx5_test_runtime_init(&dev->rt);
    }
}

void mlxnicd_dev_close(struct mlxnicd_dev *dev) {
    if (dev == NULL) {
        return;
    }
    mlxnicd_dev_stop(dev);
    free(dev);
}

int mlxnicd_dev_last_error(const struct mlxnicd_dev *dev) {
    return dev != NULL ? dev->last_error : MLXNICD_ERR_INVAL;
}

int mlxnicd_frame_build(const struct mlxnicd_l2_frame_spec *spec,
                        uint8_t *frame, uint32_t frame_capacity,
                        uint32_t *frame_len) {
    struct mlx5_tx_test_opts opts = {0};
    size_t frame_len_local = 0;

    if (spec == NULL || frame == NULL || frame_len == NULL) {
        fprintf(stderr, "mlxnicd_frame_build requires spec/frame/frame_len\n");
        return -1;
    }
    opts.dst_mac = spec->dst_mac;
    opts.src_mac = spec->src_mac;
    opts.ethertype = spec->ethertype;
    opts.payload_hex = spec->payload_hex;
    if (build_tx_test_frame(&opts, frame, frame_capacity, &frame_len_local) != 0) {
        return -1;
    }
    *frame_len = (uint32_t)frame_len_local;
    return 0;
}

int mlxnicd_dev_start(struct mlxnicd_dev *dev) {
    struct mlx5_test_runtime *rt;
    uint16_t function_id;
    uint32_t num_pages;
    int rc = 0;
    int want_tx;
    int want_rx;

    if (dev == NULL) {
        fprintf(stderr, "mlxnicd_dev_start requires device\n");
        return -1;
    }
    if (dev->started) {
        dev->last_error = MLXNICD_OK;
        return 0;
    }

    want_tx = (dev->config.flags & MLXNICD_DEV_F_TX) != 0;
    want_rx = (dev->config.flags & MLXNICD_DEV_F_RX) != 0;
    rt = &dev->rt;
    mlx5_test_runtime_init(rt);
    dev->quiet_saved = g_mlx5_stdout_quiet;
    g_mlx5_stdout_quiet = dev->config.log_verbose ? 0 : 1;
    vfio_set_stdout_quiet(dev->config.log_verbose ? 0 : 1);

    if (mlx5_cmd_ctx_open(dev->bdf, &rt->ctx) != 0) {
        dev->last_error = MLXNICD_ERR_IO;
        g_mlx5_stdout_quiet = dev->quiet_saved;
        vfio_set_stdout_quiet(0);
        return -1;
    }

    rc = mlx5_ctx_enable_hca(&rt->ctx);
    if (rc != 0) {
        goto out;
    }
    rc = mlx5_ctx_set_issi(&rt->ctx);
    if (rc != 0) {
        goto out;
    }
    rc = mlx5_ctx_query_pages(&rt->ctx, 1);
    if (rc != 0) {
        goto out;
    }
    {
        uint8_t in[32] = {0};
        uint8_t out[32] = {0};

        put_be16(in, MLX5_CMD_OP_QUERY_PAGES);
        put_be16(in + 0x06, 1);
        rc = mlx5_cmd_exec(&rt->ctx, MLX5_CMD_OP_QUERY_PAGES, in, sizeof(in), out,
                           sizeof(out), "api-query-boot-pages-for-give");
        if (rc != 0) {
            goto out;
        }
        function_id = get_be16(out + 0x0a);
        num_pages = get_be32(out + 0x0c);
        rc = mlx5_ctx_give_pages(&rt->ctx, function_id, num_pages,
                                 "api-give-boot-pages");
        if (rc != 0) {
            goto out;
        }
    }
    rc = mlx5_ctx_query_dtor(&rt->ctx);
    if (rc != 0) {
        goto out;
    }
    rc = mlx5_ctx_query_hca_cap_raw(
        &rt->ctx, (MLX5_HCA_CAP_GENERAL << 1) | MLX5_HCA_CAP_GET_CUR, rt->hca_cap,
        sizeof(rt->hca_cap), "api-query-hca-cap-general-current");
    if (rc != 0) {
        goto out;
    }
    rc = mlx5_ctx_set_hca_cap_raw(&rt->ctx, MLX5_HCA_CAP_GENERAL, rt->hca_cap,
                                  sizeof(rt->hca_cap));
    if (rc != 0) {
        goto out;
    }
    rc = mlx5_ctx_query_pages(&rt->ctx, 0);
    if (rc != 0) {
        goto out;
    }
    {
        uint8_t in[32] = {0};
        uint8_t out[32] = {0};

        put_be16(in, MLX5_CMD_OP_QUERY_PAGES);
        put_be16(in + 0x06, 2);
        rc = mlx5_cmd_exec(&rt->ctx, MLX5_CMD_OP_QUERY_PAGES, in, sizeof(in), out,
                           sizeof(out), "api-query-init-pages-for-give");
        if (rc != 0) {
            goto out;
        }
        function_id = get_be16(out + 0x0a);
        num_pages = get_be32(out + 0x0c);
        rc = mlx5_ctx_give_pages(&rt->ctx, function_id, num_pages,
                                 "api-give-init-pages");
        if (rc != 0) {
            goto out;
        }
    }
    rc = mlx5_ctx_init_hca(&rt->ctx);
    if (rc != 0) {
        goto out;
    }
    rc = mlx5_ctx_query_paos(&rt->ctx, 1);
    if (rc != 0) {
        goto out;
    }
    if (want_rx) {
        rc = mlx5_ctx_enable_vport_promisc(&rt->ctx);
        if (rc != 0) {
            goto out;
        }
    }
    rc = mlx5_ctx_alloc_uar(&rt->ctx, &rt->uar);
    if (rc != 0) {
        goto out;
    }
    rc = mlx5_ctx_alloc_pd(&rt->ctx, &rt->pd);
    if (rc != 0) {
        goto out;
    }
    rc = mlx5_ctx_alloc_transport_domain(&rt->ctx, &rt->tdn);
    if (rc != 0) {
        goto out;
    }
    rt->have_tdn = 1;
    if (want_rx) {
        rc = mlx5_ctx_alloc_q_counter(&rt->ctx, &rt->q_counter);
        if (rc != 0) {
            goto out;
        }
        rt->have_q_counter = 1;
    }
    if (want_tx || want_rx) {
        rc = mlx5_ctx_create_pa_mkey(&rt->ctx, rt->pd, &rt->mkey);
        if (rc != 0) {
            goto out;
        }
    }
    if (want_tx) {
        rc = mlx5_ctx_create_tis(&rt->ctx, rt->tdn, rt->pd, &rt->tisn);
        if (rc != 0) {
            goto out;
        }
        rt->have_tisn = 1;
    }
    rc = mlx5_ctx_create_eq(&rt->ctx, rt->uar, &rt->eq);
    if (rc != 0) {
        goto out;
    }
    if (want_tx) {
        rc = mlx5_ctx_create_cq(&rt->ctx, rt->uar, &rt->eq, &rt->tx_cq,
                                0x07000000ULL, 0x07100000ULL);
        if (rc != 0) {
            goto out;
        }
        rc = mlx5_ctx_create_sq(&rt->ctx, rt->uar, rt->pd, rt->tisn,
                                &rt->tx_cq, &rt->sq);
        if (rc != 0) {
            goto out;
        }
        rc = mlx5_ctx_modify_sq_ready(&rt->ctx, &rt->sq);
        if (rc != 0) {
            goto out;
        }
    }
    if (want_rx) {
        rc = mlx5_ctx_create_cq(&rt->ctx, rt->uar, &rt->eq, &rt->rx_cq,
                                0x07a00000ULL, 0x07b00000ULL);
        if (rc != 0) {
            goto out;
        }
        rc = mlx5_ctx_create_rq(&rt->ctx, rt->pd, rt->q_counter, &rt->rx_cq,
                                &rt->rq);
        if (rc != 0) {
            goto out;
        }
        rc = mlx5_ctx_modify_rq_ready(&rt->ctx, &rt->rq);
        if (rc != 0) {
            goto out;
        }
        rc = mlx5_ctx_create_rqt(&rt->ctx, &rt->rq, &rt->rqt);
        if (rc != 0) {
            goto out;
        }
        rc = mlx5_ctx_create_indirect_tir(&rt->ctx, rt->tdn, &rt->rqt, &rt->tir);
        if (rc != 0) {
            goto out;
        }
        rc = mlx5_rq_post_buffers(&rt->ctx, &rt->rq, rt->mkey,
                                  dev->config.rx_post_count);
        if (rc != 0) {
            goto out;
        }
        rc = mlx5_ctx_create_rx_flow_table(&rt->ctx, &rt->ft);
        if (rc != 0) {
            goto out;
        }
        rc = mlx5_ctx_set_rx_flow_table_root(&rt->ctx, &rt->ft);
        if (rc != 0) {
            goto out;
        }
        rc = mlx5_ctx_create_rx_flow_group(&rt->ctx, &rt->ft, &rt->fg);
        if (rc != 0) {
            goto out;
        }
        rc = mlx5_ctx_set_rx_fte_to_tir(&rt->ctx, &rt->ft, &rt->fg, &rt->tir,
                                        &rt->fte);
        if (rc != 0) {
            goto out;
        }
    }

    dev->last_error = MLXNICD_OK;
    dev->started = 1;
    dev->rx_outstanding = 0;
    return 0;

out:
    mlx5_test_runtime_cleanup(rt, &rc);
    dev->last_error = mlxnicd_err_from_rc(rc);
    dev->rx_outstanding = 0;
    g_mlx5_stdout_quiet = dev->quiet_saved;
    vfio_set_stdout_quiet(0);
    return -1;
}

uint16_t mlxnicd_tx_burst(struct mlxnicd_dev *dev,
                          const struct mlxnicd_pkt *pkts, uint16_t nb_pkts) {
    uint16_t sent = 0;

    if (dev == NULL || pkts == NULL) {
        return 0;
    }
    if (!dev->started || (dev->config.flags & MLXNICD_DEV_F_TX) == 0) {
        dev->last_error = MLXNICD_ERR_STATE;
        return 0;
    }

    while (sent < nb_pkts) {
        int rc = mlx5_sq_post_send_raw(&dev->rt.ctx, dev->rt.uar, dev->rt.tisn,
                                       dev->rt.mkey, &dev->rt.tx_cq, &dev->rt.sq,
                                       pkts[sent].data, pkts[sent].len);
        if (rc != 0) {
            dev->last_error = mlxnicd_err_from_rc(rc);
            break;
        }
        sent++;
    }
    if (sent == nb_pkts) {
        dev->last_error = MLXNICD_OK;
    }
    return sent;
}

static int mlx5_rx_poll_one_api(struct mlxnicd_dev *dev, int timeout_ms,
                                struct mlx5_rx_packet *pkt) {
    return mlx5_rx_poll_one(&dev->rt.rx_cq, &dev->rt.rq, timeout_ms, pkt);
}

uint16_t mlxnicd_rx_burst(struct mlxnicd_dev *dev, struct mlxnicd_pkt *pkts,
                          uint16_t nb_pkts, int timeout_ms) {
    uint16_t received = 0;

    if (dev == NULL || pkts == NULL) {
        return 0;
    }
    if (!dev->started || (dev->config.flags & MLXNICD_DEV_F_RX) == 0) {
        dev->last_error = MLXNICD_ERR_STATE;
        return 0;
    }

    while (received < nb_pkts) {
        struct mlx5_rx_packet pkt = {0};
        int rc = mlx5_rx_poll_one_api(dev, received == 0 ? timeout_ms : 0, &pkt);

        if (rc != 0) {
            if (received == 0) {
                dev->last_error = MLXNICD_ERR_TIMEOUT;
            }
            break;
        }
        pkts[received].data = pkt.data;
        pkts[received].len = pkt.len;
        received++;
    }
    if (received > 0) {
        dev->last_error = MLXNICD_OK;
        dev->rx_outstanding =
            (uint16_t)(dev->rx_outstanding + received);
    }
    return received;
}

int mlxnicd_rx_release(struct mlxnicd_dev *dev, uint16_t nb_pkts) {
    uint16_t i;

    if (dev == NULL) {
        return -1;
    }
    if (!dev->started || (dev->config.flags & MLXNICD_DEV_F_RX) == 0) {
        dev->last_error = MLXNICD_ERR_STATE;
        return -1;
    }
    if (nb_pkts > dev->rx_outstanding) {
        dev->last_error = MLXNICD_ERR_INVAL;
        return -1;
    }
    for (i = 0; i < nb_pkts; i++) {
        int rc = mlx5_rx_replenish_one(&dev->rt.ctx, &dev->rt.rq, dev->rt.mkey);
        if (rc != 0) {
            dev->last_error = mlxnicd_err_from_rc(rc);
            return -1;
        }
    }
    dev->rx_outstanding = (uint16_t)(dev->rx_outstanding - nb_pkts);
    dev->last_error = MLXNICD_OK;
    return 0;
}
int mlx5_ctx_query_hca_cap(struct mlx5_cmd_ctx *ctx) {
    uint8_t cap[MLX5_HCA_CAP_OUT_SIZE] = {0};
    int rc = mlx5_ctx_query_hca_cap_raw(
        ctx, (MLX5_HCA_CAP_GENERAL << 1) | MLX5_HCA_CAP_GET_CUR, cap,
        MLX5_HCA_CAP_SIZE, "mlx5-query-hca-cap");

    if (rc == 0) {
        printf("  cap.raw[0x00..0x40]:");
        for (size_t i = 0; i < 0x40; i += 4) {
            printf(" %08" PRIx32, get_be32(cap + i));
        }
        printf("\n");
    }
    return rc;
}
