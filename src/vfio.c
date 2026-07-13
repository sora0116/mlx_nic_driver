#include "vfio.h"
#include "vfio_compat.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define STATE_DIR ".mlxnicd-state"

static int g_vfio_stdout_quiet = 0;

static int vfio_printf(const char *fmt, ...) {
    int rc;
    va_list ap;

    if (g_vfio_stdout_quiet) {
        return 0;
    }
    va_start(ap, fmt);
    rc = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return rc;
}

#define printf(...) vfio_printf(__VA_ARGS__)

void vfio_set_stdout_quiet(int quiet) { g_vfio_stdout_quiet = quiet; }

static int pathf(char *buf, size_t len, const char *fmt, const char *arg) {
    int n = snprintf(buf, len, fmt, arg);
    if (n < 0 || (size_t)n >= len) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int readlink_basename(const char *path, char *out, size_t outlen) {
    char link[PATH_MAX];
    ssize_t n = readlink(path, link, sizeof(link) - 1);
    const char *base;
    size_t len;

    if (n < 0) {
        return -1;
    }
    link[n] = '\0';
    base = strrchr(link, '/');
    base = base ? base + 1 : link;
    len = strlen(base);
    if (len >= outlen) {
        len = outlen - 1;
    }
    memcpy(out, base, len);
    out[len] = '\0';
    return 0;
}

static int read_file_trim(const char *path, char *out, size_t outlen) {
    int fd = open(path, O_RDONLY);
    ssize_t n;

    if (fd < 0) {
        return -1;
    }
    n = read(fd, out, outlen - 1);
    close(fd);
    if (n < 0) {
        return -1;
    }
    out[n] = '\0';
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == ' ' ||
                     out[n - 1] == '\t')) {
        out[--n] = '\0';
    }
    return 0;
}

static int write_file(const char *path, const char *value) {
    int fd = open(path, O_WRONLY);
    size_t len = strlen(value);
    ssize_t n;

    if (fd < 0) {
        return -1;
    }
    n = write(fd, value, len);
    close(fd);
    if (n < 0 || (size_t)n != len) {
        errno = n < 0 ? errno : EIO;
        return -1;
    }
    return 0;
}

static bool exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static bool has_iommu_groups(void) {
    DIR *dir = opendir("/sys/kernel/iommu_groups");
    struct dirent *de;
    bool found = false;

    if (dir == NULL) {
        return false;
    }
    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0) {
            found = true;
            break;
        }
    }
    closedir(dir);
    return found;
}

static int get_driver(const char *bdf, char *driver, size_t driver_len) {
    char path[PATH_MAX];
    if (pathf(path, sizeof(path), "/sys/bus/pci/devices/%s/driver", bdf) != 0) {
        return -1;
    }
    return readlink_basename(path, driver, driver_len);
}

static int get_iommu_group(const char *bdf, char *group, size_t group_len) {
    char path[PATH_MAX];
    if (pathf(path, sizeof(path), "/sys/bus/pci/devices/%s/iommu_group", bdf) != 0) {
        return -1;
    }
    return readlink_basename(path, group, group_len);
}

static int get_vendor_device(const char *bdf, char *vendor, size_t vendor_len,
                             char *device, size_t device_len) {
    char path[PATH_MAX];
    if (pathf(path, sizeof(path), "/sys/bus/pci/devices/%s/vendor", bdf) != 0 ||
        read_file_trim(path, vendor, vendor_len) != 0) {
        return -1;
    }
    if (pathf(path, sizeof(path), "/sys/bus/pci/devices/%s/device", bdf) != 0 ||
        read_file_trim(path, device, device_len) != 0) {
        return -1;
    }
    return 0;
}

static int state_path(const char *bdf, char *path, size_t path_len) {
    if (mkdir(STATE_DIR, 0700) != 0 && errno != EEXIST) {
        return -1;
    }
    return pathf(path, path_len, STATE_DIR "/%s.driver", bdf);
}

static void print_restore_hint(const char *bdf, const char *driver) {
    printf("restore hint:\n");
    printf("  echo %s | sudo tee /sys/bus/pci/drivers/vfio-pci/unbind\n", bdf);
    printf("  echo %s | sudo tee /sys/bus/pci/drivers/%s/bind\n", bdf, driver);
}

int vfio_check(const char *bdf) {
    char path[PATH_MAX];
    char driver[128] = "none";
    char vendor[32] = "unknown";
    char device[32] = "unknown";
    bool iommu = has_iommu_groups();
    bool vfio_pci_driver = exists("/sys/bus/pci/drivers/vfio-pci");

    printf("iommu_groups: %s\n", iommu ? "present" : "missing");
    printf("vfio-pci driver: %s\n", vfio_pci_driver ? "present" : "missing");
    printf("vfio device node: %s\n", exists("/dev/vfio/vfio") ? "present" : "missing");

    if (bdf == NULL) {
        if (!iommu) {
            printf("status: not ready; enable IOMMU before VFIO DMA\n");
            printf("sdn-svr6 currently needs boot args like intel_iommu=on iommu=pt\n");
            return 1;
        }
        return vfio_pci_driver ? 0 : 1;
    }

    if (pathf(path, sizeof(path), "/sys/bus/pci/devices/%s", bdf) != 0 ||
        !exists(path)) {
        fprintf(stderr, "device not found: %s\n", bdf);
        return -1;
    }

    if (get_vendor_device(bdf, vendor, sizeof(vendor), device, sizeof(device)) != 0) {
        perror("read vendor/device");
        return -1;
    }
    if (get_driver(bdf, driver, sizeof(driver)) != 0) {
        snprintf(driver, sizeof(driver), "none");
    }

    printf("bdf: %s\n", bdf);
    printf("vendor: %s\n", vendor);
    printf("device: %s\n", device);
    printf("driver: %s\n", driver);
    if (pathf(path, sizeof(path), "/sys/bus/pci/devices/%s/iommu_group", bdf) == 0 &&
        readlink_basename(path, vendor, sizeof(vendor)) == 0) {
        printf("iommu_group: %s\n", vendor);
    } else {
        printf("iommu_group: none\n");
    }

    if (!iommu) {
        printf("status: not ready; IOMMU groups are missing\n");
        return 1;
    }
    if (!vfio_pci_driver) {
        printf("status: not ready; load vfio-pci\n");
        return 1;
    }
    printf("status: ready for vfio bind checks\n");
    return 0;
}

int vfio_bind(const char *bdf) {
    char driver[128] = "none";
    char vendor[32];
    char device[32];
    char path[PATH_MAX];
    char state[PATH_MAX];
    char id[80];
    int fd;

    if (vfio_check(bdf) != 0) {
        fprintf(stderr, "refusing to bind: vfio-check did not pass\n");
        return -1;
    }
    if (get_driver(bdf, driver, sizeof(driver)) != 0) {
        fprintf(stderr, "refusing to bind: device has no current driver to record\n");
        return -1;
    }
    if (strcmp(driver, "vfio-pci") == 0) {
        printf("%s is already bound to vfio-pci\n", bdf);
        return 0;
    }
    if (get_vendor_device(bdf, vendor, sizeof(vendor), device, sizeof(device)) != 0) {
        perror("read vendor/device");
        return -1;
    }
    if (state_path(bdf, state, sizeof(state)) != 0) {
        perror("state path");
        return -1;
    }
    fd = open(state, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        perror(state);
        return -1;
    }
    dprintf(fd, "%s\n", driver);
    close(fd);

    snprintf(id, sizeof(id), "%s %s", vendor, device);
    if (write_file("/sys/bus/pci/drivers/vfio-pci/new_id", id) != 0 &&
        errno != EEXIST) {
        perror("vfio-pci new_id");
        return -1;
    }
    if (pathf(path, sizeof(path), "/sys/bus/pci/drivers/%s/unbind", driver) != 0 ||
        write_file(path, bdf) != 0) {
        perror("unbind original driver");
        print_restore_hint(bdf, driver);
        return -1;
    }
    if (write_file("/sys/bus/pci/drivers/vfio-pci/bind", bdf) != 0) {
        perror("bind vfio-pci");
        print_restore_hint(bdf, driver);
        return -1;
    }
    printf("bound %s to vfio-pci; original driver recorded as %s\n", bdf, driver);
    print_restore_hint(bdf, driver);
    return 0;
}

int vfio_restore(const char *bdf) {
    char state[PATH_MAX];
    char driver[128];
    char current[128] = "none";
    char path[PATH_MAX];

    if (state_path(bdf, state, sizeof(state)) != 0 ||
        read_file_trim(state, driver, sizeof(driver)) != 0) {
        fprintf(stderr, "no recorded driver for %s; expected %s\n", bdf, state);
        return -1;
    }
    if (get_driver(bdf, current, sizeof(current)) == 0 &&
        strcmp(current, "vfio-pci") == 0) {
        if (write_file("/sys/bus/pci/drivers/vfio-pci/unbind", bdf) != 0) {
            perror("unbind vfio-pci");
            return -1;
        }
    }
    if (pathf(path, sizeof(path), "/sys/bus/pci/drivers/%s/bind", driver) != 0 ||
        write_file(path, bdf) != 0) {
        perror("bind original driver");
        return -1;
    }
    printf("restored %s to %s\n", bdf, driver);
    return 0;
}

static void close_if_open(int *fd) {
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static int vfio_enable_bus_master(struct vfio_device *dev) {
    struct vfio_region_info cfg;
    uint8_t cmd_bytes[2];
    uint16_t cmd;

    memset(&cfg, 0, sizeof(cfg));
    cfg.argsz = sizeof(cfg);
    cfg.index = VFIO_PCI_CONFIG_REGION_INDEX;
    if (ioctl(dev->device_fd, VFIO_DEVICE_GET_REGION_INFO, &cfg) != 0) {
        perror("VFIO_DEVICE_GET_REGION_INFO CONFIG");
        return -1;
    }
    if (pread(dev->device_fd, cmd_bytes, sizeof(cmd_bytes),
              (off_t)cfg.offset + 4) != (ssize_t)sizeof(cmd_bytes)) {
        perror("read PCI command");
        return -1;
    }
    cmd = (uint16_t)cmd_bytes[0] | ((uint16_t)cmd_bytes[1] << 8);
    dev->pci_command = cmd;
    if ((cmd & 0x4u) != 0) {
        return 0;
    }
    cmd |= 0x4u;
    cmd_bytes[0] = (uint8_t)cmd;
    cmd_bytes[1] = (uint8_t)(cmd >> 8);
    if (pwrite(dev->device_fd, cmd_bytes, sizeof(cmd_bytes),
               (off_t)cfg.offset + 4) != (ssize_t)sizeof(cmd_bytes)) {
        perror("write PCI command bus master");
        return -1;
    }
    dev->pci_command = cmd;
    return 0;
}

void vfio_device_close(struct vfio_device *dev) {
    if (dev == NULL) {
        return;
    }
    if (dev->bar0 != NULL && dev->bar0 != MAP_FAILED) {
        munmap(dev->bar0, (size_t)dev->bar0_size);
    }
    close_if_open(&dev->device_fd);
    if (dev->group_fd >= 0 && dev->container_fd >= 0) {
        ioctl(dev->group_fd, VFIO_GROUP_UNSET_CONTAINER);
    }
    close_if_open(&dev->group_fd);
    close_if_open(&dev->container_fd);
    dev->bar0 = NULL;
    dev->bar0_size = 0;
    dev->bar0_offset = 0;
}

int vfio_device_open(const char *bdf, struct vfio_device *dev) {
    char driver[128] = "none";
    char group_path[PATH_MAX];
    int version;
    struct vfio_group_status group_status;
    struct vfio_device_info device_info;
    struct vfio_region_info bar0;

    memset(dev, 0, sizeof(*dev));
    dev->container_fd = -1;
    dev->group_fd = -1;
    dev->device_fd = -1;
    dev->bar0 = NULL;
    snprintf(dev->bdf, sizeof(dev->bdf), "%s", bdf);

    if (vfio_check(bdf) != 0) {
        fprintf(stderr, "vfio device open preflight failed\n");
        return -1;
    }
    if (get_driver(bdf, driver, sizeof(driver)) != 0 ||
        strcmp(driver, "vfio-pci") != 0) {
        fprintf(stderr, "device %s must be bound to vfio-pci first; current driver=%s\n",
                bdf, driver);
        return -1;
    }
    if (get_iommu_group(bdf, dev->group, sizeof(dev->group)) != 0) {
        perror("iommu_group");
        return -1;
    }
    if (pathf(group_path, sizeof(group_path), "/dev/vfio/%s", dev->group) != 0) {
        perror("group path");
        return -1;
    }

    dev->container_fd = open("/dev/vfio/vfio", O_RDWR);
    if (dev->container_fd < 0) {
        perror("/dev/vfio/vfio");
        goto fail;
    }
    version = ioctl(dev->container_fd, VFIO_GET_API_VERSION);
    if (version != VFIO_API_VERSION) {
        fprintf(stderr, "unsupported VFIO API version: got %d expected %d\n",
                version, VFIO_API_VERSION);
        goto fail;
    }
    if (ioctl(dev->container_fd, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU) <= 0) {
        fprintf(stderr, "VFIO_TYPE1_IOMMU is not supported\n");
        goto fail;
    }

    dev->group_fd = open(group_path, O_RDWR);
    if (dev->group_fd < 0) {
        perror(group_path);
        goto fail;
    }
    memset(&group_status, 0, sizeof(group_status));
    group_status.argsz = sizeof(group_status);
    if (ioctl(dev->group_fd, VFIO_GROUP_GET_STATUS, &group_status) != 0) {
        perror("VFIO_GROUP_GET_STATUS");
        goto fail;
    }
    if ((group_status.flags & VFIO_GROUP_FLAGS_VIABLE) == 0) {
        fprintf(stderr, "VFIO group %s is not viable\n", dev->group);
        goto fail;
    }
    if (ioctl(dev->group_fd, VFIO_GROUP_SET_CONTAINER, &dev->container_fd) != 0) {
        perror("VFIO_GROUP_SET_CONTAINER");
        goto fail;
    }
    if (ioctl(dev->container_fd, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU) != 0) {
        perror("VFIO_SET_IOMMU");
        goto fail;
    }

    dev->device_fd = ioctl(dev->group_fd, VFIO_GROUP_GET_DEVICE_FD, bdf);
    if (dev->device_fd < 0) {
        perror("VFIO_GROUP_GET_DEVICE_FD");
        goto fail;
    }

    memset(&device_info, 0, sizeof(device_info));
    device_info.argsz = sizeof(device_info);
    if (ioctl(dev->device_fd, VFIO_DEVICE_GET_INFO, &device_info) != 0) {
        perror("VFIO_DEVICE_GET_INFO");
        goto fail;
    }
    if ((device_info.flags & VFIO_DEVICE_FLAGS_PCI) == 0) {
        fprintf(stderr, "VFIO device is not PCI\n");
        goto fail;
    }
    if (vfio_enable_bus_master(dev) != 0) {
        goto fail;
    }

    memset(&bar0, 0, sizeof(bar0));
    bar0.argsz = sizeof(bar0);
    bar0.index = VFIO_PCI_BAR0_REGION_INDEX;
    if (ioctl(dev->device_fd, VFIO_DEVICE_GET_REGION_INFO, &bar0) != 0) {
        perror("VFIO_DEVICE_GET_REGION_INFO BAR0");
        goto fail;
    }
    if ((bar0.flags & VFIO_REGION_INFO_FLAG_MMAP) == 0 || bar0.size == 0) {
        fprintf(stderr, "BAR0 is not mmap-capable or has zero size\n");
        goto fail;
    }
    dev->bar0_size = (uint64_t)bar0.size;
    dev->bar0_offset = (uint64_t)bar0.offset;
    dev->bar0 = mmap(NULL, (size_t)bar0.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                     dev->device_fd, (off_t)bar0.offset);
    if (dev->bar0 == MAP_FAILED) {
        dev->bar0 = NULL;
        perror("mmap BAR0 via VFIO");
        goto fail;
    }

    return 0;

fail:
    vfio_device_close(dev);
    return -1;
}

int vfio_dma_map(struct vfio_device *dev, void *addr, uint64_t iova,
                 size_t size) {
    struct vfio_iommu_type1_dma_map map = {
        .argsz = sizeof(map),
        .flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
        .vaddr = (uint64_t)(uintptr_t)addr,
        .iova = iova,
        .size = size,
    };
    if (ioctl(dev->container_fd, VFIO_IOMMU_MAP_DMA, &map) != 0) {
        perror("VFIO_IOMMU_MAP_DMA");
        return -1;
    }
    return 0;
}

int vfio_dma_unmap(struct vfio_device *dev, uint64_t iova, size_t size) {
    struct vfio_iommu_type1_dma_unmap unmap = {
        .argsz = sizeof(unmap),
        .flags = 0,
        .iova = iova,
        .size = size,
    };
    if (ioctl(dev->container_fd, VFIO_IOMMU_UNMAP_DMA, &unmap) != 0) {
        perror("VFIO_IOMMU_UNMAP_DMA");
        return -1;
    }
    return 0;
}

int vfio_probe(const char *bdf) {
    struct vfio_device dev;
    struct vfio_device_info device_info;
    struct vfio_iommu_type1_info iommu_info;
    void *dma = NULL;
    const size_t dma_size = 4096;
    const uint64_t iova = 0x100000000ULL;
    bool dma_mapped = false;
    int ret = -1;

    if (vfio_device_open(bdf, &dev) != 0) {
        return -1;
    }

    memset(&iommu_info, 0, sizeof(iommu_info));
    iommu_info.argsz = sizeof(iommu_info);
    if (ioctl(dev.container_fd, VFIO_IOMMU_GET_INFO, &iommu_info) != 0) {
        perror("VFIO_IOMMU_GET_INFO");
        goto out;
    }

    memset(&device_info, 0, sizeof(device_info));
    device_info.argsz = sizeof(device_info);
    if (ioctl(dev.device_fd, VFIO_DEVICE_GET_INFO, &device_info) != 0) {
        perror("VFIO_DEVICE_GET_INFO");
        goto out;
    }

    if (posix_memalign(&dma, 4096, dma_size) != 0) {
        fprintf(stderr, "posix_memalign failed\n");
        goto out;
    }
    memset(dma, 0xa5, dma_size);
    if (vfio_dma_map(&dev, dma, iova, dma_size) != 0) {
        goto out;
    }
    dma_mapped = true;

    printf("vfio-probe ok\n");
    printf("  bdf: %s\n", dev.bdf);
    printf("  group: %s\n", dev.group);
    printf("  device regions: %u irqs: %u\n", device_info.num_regions,
           device_info.num_irqs);
    printf("  pci command: 0x%04x\n", dev.pci_command);
    printf("  iommu page sizes bitmap: 0x%" PRIx64 "\n",
           (uint64_t)iommu_info.iova_pgsizes);
    printf("  BAR0 size: 0x%" PRIx64 " offset: 0x%" PRIx64
           "\n",
           dev.bar0_size, dev.bar0_offset);
    printf("  BAR0[0x0] u32: 0x%08" PRIx32 "\n",
           *(volatile uint32_t *)dev.bar0);
    printf("  DMA mapped: vaddr=%p iova=0x%" PRIx64 " size=0x%zx\n",
           dma, iova, dma_size);
    ret = 0;

out:
    if (dma_mapped) {
        if (vfio_dma_unmap(&dev, iova, dma_size) != 0) {
            ret = -1;
        }
    }
    if (dma != NULL) {
        free(dma);
    }
    vfio_device_close(&dev);
    return ret;
}
