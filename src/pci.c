#include "pci.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

struct pci_resource {
    uint64_t start;
    uint64_t end;
    uint64_t flags;
};

static int make_path(char *buf, size_t buflen, const char *bdf,
                     const char *leaf) {
    int n = snprintf(buf, buflen, "/sys/bus/pci/devices/%s/%s", bdf, leaf);
    if (n < 0 || (size_t)n >= buflen) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int read_text_file(const char *path, char *buf, size_t buflen) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    ssize_t n = read(fd, buf, buflen - 1);
    int saved = errno;
    close(fd);
    errno = saved;

    if (n < 0) {
        return -1;
    }
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' ' ||
                     buf[n - 1] == '\t')) {
        buf[n - 1] = '\0';
        n--;
    }
    return 0;
}

static int read_hex_file_u32(const char *path, uint32_t *out) {
    char buf[64];
    char *end = NULL;
    unsigned long value;

    if (read_text_file(path, buf, sizeof(buf)) != 0) {
        return -1;
    }
    errno = 0;
    value = strtoul(buf, &end, 0);
    if (errno != 0 || end == buf || *end != '\0' || value > UINT32_MAX) {
        errno = EINVAL;
        return -1;
    }
    *out = (uint32_t)value;
    return 0;
}

int parse_u64(const char *s, uint64_t *out) {
    char *end = NULL;
    unsigned long long value;

    if (s == NULL || *s == '\0') {
        return -1;
    }
    errno = 0;
    value = strtoull(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0') {
        return -1;
    }
    *out = (uint64_t)value;
    return 0;
}

int parse_u32(const char *s, uint32_t *out) {
    uint64_t value = 0;
    if (parse_u64(s, &value) != 0 || value > UINT32_MAX) {
        return -1;
    }
    *out = (uint32_t)value;
    return 0;
}

static void basename_from_link(const char *path, char *out, size_t outlen) {
    char target[PATH_MAX];
    ssize_t n = readlink(path, target, sizeof(target) - 1);
    const char *base;
    size_t len;

    if (n < 0) {
        snprintf(out, outlen, "none");
        return;
    }
    target[n] = '\0';
    base = strrchr(target, '/');
    base = base ? base + 1 : target;
    len = strlen(base);
    if (len >= outlen) {
        len = outlen - 1;
    }
    memcpy(out, base, len);
    out[len] = '\0';
}

static void collect_netdevs(const char *bdf, char *out, size_t outlen) {
    char path[PATH_MAX];
    DIR *dir;
    struct dirent *de;
    size_t used = 0;

    out[0] = '\0';
    if (make_path(path, sizeof(path), bdf, "net") != 0) {
        snprintf(out, outlen, "none");
        return;
    }
    dir = opendir(path);
    if (dir == NULL) {
        snprintf(out, outlen, "none");
        return;
    }

    while ((de = readdir(dir)) != NULL) {
        int n;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        n = snprintf(out + used, outlen - used, "%s%s",
                     used == 0 ? "" : ",", de->d_name);
        if (n < 0 || (size_t)n >= outlen - used) {
            break;
        }
        used += (size_t)n;
    }
    closedir(dir);

    if (out[0] == '\0') {
        snprintf(out, outlen, "none");
    }
}

static int read_resources(const char *bdf, struct pci_resource res[6]) {
    char path[PATH_MAX];
    FILE *fp;
    unsigned int i;

    memset(res, 0, sizeof(struct pci_resource) * 6);
    if (make_path(path, sizeof(path), bdf, "resource") != 0) {
        return -1;
    }
    fp = fopen(path, "r");
    if (fp == NULL) {
        return -1;
    }
    for (i = 0; i < 6; i++) {
        unsigned long long start = 0, end = 0, flags = 0;
        if (fscanf(fp, "%llx %llx %llx", &start, &end, &flags) != 3) {
            break;
        }
        res[i].start = (uint64_t)start;
        res[i].end = (uint64_t)end;
        res[i].flags = (uint64_t)flags;
    }
    fclose(fp);
    return 0;
}

static uint64_t resource_len(const struct pci_resource *res) {
    if (res->start == 0 && res->end == 0) {
        return 0;
    }
    if (res->end < res->start) {
        return 0;
    }
    return res->end - res->start + 1;
}

static bool device_exists(const char *bdf) {
    char path[PATH_MAX];
    struct stat st;
    if (snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s", bdf) < 0) {
        return false;
    }
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int mlx_list_devices(void) {
    DIR *dir = opendir("/sys/bus/pci/devices");
    struct dirent *de;
    int rc = 0;

    if (dir == NULL) {
        perror("opendir /sys/bus/pci/devices");
        return -1;
    }

    printf("%-14s %-8s %-8s %-16s %s\n", "BDF", "vendor", "device",
           "driver", "netdevs");

    while ((de = readdir(dir)) != NULL) {
        char path[PATH_MAX];
        char driver_path[PATH_MAX];
        char driver[128];
        char netdevs[256];
        uint32_t vendor = 0;
        uint32_t device = 0;

        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        if (make_path(path, sizeof(path), de->d_name, "vendor") != 0 ||
            read_hex_file_u32(path, &vendor) != 0) {
            continue;
        }
        if (vendor != MLX_VENDOR_ID) {
            continue;
        }
        if (make_path(path, sizeof(path), de->d_name, "device") != 0 ||
            read_hex_file_u32(path, &device) != 0) {
            device = 0;
        }
        if (make_path(driver_path, sizeof(driver_path), de->d_name,
                      "driver") != 0) {
            snprintf(driver, sizeof(driver), "none");
        } else {
            basename_from_link(driver_path, driver, sizeof(driver));
        }
        collect_netdevs(de->d_name, netdevs, sizeof(netdevs));

        printf("%-14s 0x%04x   0x%04x   %-16s %s\n", de->d_name, vendor,
               device, driver, netdevs);
    }

    if (closedir(dir) != 0) {
        perror("closedir");
        rc = -1;
    }
    return rc;
}

static int print_config_header(const char *bdf) {
    char path[PATH_MAX];
    unsigned char bytes[64];
    int fd;
    ssize_t n;
    size_t i;

    if (make_path(path, sizeof(path), bdf, "config") != 0) {
        perror("config path");
        return -1;
    }
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror(path);
        return -1;
    }
    n = pread(fd, bytes, sizeof(bytes), 0);
    close(fd);
    if (n < 0) {
        perror("read config");
        return -1;
    }

    printf("config[0..%zd):", n);
    for (i = 0; i < (size_t)n; i++) {
        if (i % 16 == 0) {
            printf("\n  %02zx:", i);
        }
        printf(" %02x", bytes[i]);
    }
    printf("\n");
    return 0;
}

int mlx_inspect_device(const char *bdf) {
    char path[PATH_MAX];
    char driver_path[PATH_MAX];
    char driver[128];
    char netdevs[256];
    uint32_t vendor = 0;
    uint32_t device = 0;
    struct pci_resource res[6];
    int rc = 0;

    if (!device_exists(bdf)) {
        fprintf(stderr, "device not found: %s\n", bdf);
        return -1;
    }

    if (make_path(path, sizeof(path), bdf, "vendor") != 0 ||
        read_hex_file_u32(path, &vendor) != 0) {
        perror("read vendor");
        return -1;
    }
    if (make_path(path, sizeof(path), bdf, "device") != 0 ||
        read_hex_file_u32(path, &device) != 0) {
        perror("read device");
        return -1;
    }
    if (make_path(driver_path, sizeof(driver_path), bdf, "driver") != 0) {
        snprintf(driver, sizeof(driver), "none");
    } else {
        basename_from_link(driver_path, driver, sizeof(driver));
    }
    collect_netdevs(bdf, netdevs, sizeof(netdevs));

    printf("bdf:    %s\n", bdf);
    printf("vendor: 0x%04x%s\n", vendor,
           vendor == MLX_VENDOR_ID ? " (Mellanox/NVIDIA)" : "");
    printf("device: 0x%04x\n", device);
    printf("driver: %s\n", driver);
    printf("net:    %s\n", netdevs);

    if (read_resources(bdf, res) != 0) {
        perror("read resource");
        rc = -1;
    } else {
        unsigned int i;
        printf("resources:\n");
        for (i = 0; i < 6; i++) {
            uint64_t len = resource_len(&res[i]);
            printf("  BAR%u start=0x%016" PRIx64 " end=0x%016" PRIx64
                   " len=0x%016" PRIx64 " flags=0x%016" PRIx64 "\n",
                   i, res[i].start, res[i].end, len, res[i].flags);
        }
    }

    if (print_config_header(bdf) != 0) {
        rc = -1;
    }
    return rc;
}

int mlx_bar_read(const char *bdf, unsigned int bar, uint64_t offset,
                 unsigned int width_bits) {
    char leaf[32];
    char path[PATH_MAX];
    struct pci_resource res[6];
    uint64_t len;
    unsigned int width_bytes;
    int fd;
    void *map;

    if (!device_exists(bdf)) {
        fprintf(stderr, "device not found: %s\n", bdf);
        return -1;
    }
    if (bar >= 6) {
        fprintf(stderr, "invalid BAR index %u; expected 0..5\n", bar);
        return -1;
    }
    if (width_bits != 8 && width_bits != 16 && width_bits != 32 &&
        width_bits != 64) {
        fprintf(stderr, "invalid width %u; expected 8, 16, 32, or 64\n",
                width_bits);
        return -1;
    }
    width_bytes = width_bits / 8;
    if ((offset % width_bytes) != 0) {
        fprintf(stderr, "offset 0x%" PRIx64 " is not %u-byte aligned\n",
                offset, width_bytes);
        return -1;
    }
    if (read_resources(bdf, res) != 0) {
        perror("read resource");
        return -1;
    }
    len = resource_len(&res[bar]);
    if (len == 0) {
        fprintf(stderr, "BAR%u has zero length\n", bar);
        return -1;
    }
    if (offset > len || width_bytes > len - offset) {
        fprintf(stderr,
                "read outside BAR%u: offset=0x%" PRIx64 " width=%u len=0x%"
                PRIx64 "\n",
                bar, offset, width_bytes, len);
        return -1;
    }

    if (snprintf(leaf, sizeof(leaf), "resource%u", bar) < 0 ||
        make_path(path, sizeof(path), bdf, leaf) != 0) {
        perror("resource path");
        return -1;
    }

    fd = open(path, O_RDONLY | O_SYNC);
    if (fd < 0) {
        perror(path);
        return -1;
    }
    map = mmap(NULL, (size_t)len, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        perror("mmap BAR");
        return -1;
    }

    switch (width_bits) {
    case 8: {
        volatile uint8_t *p = (volatile uint8_t *)((char *)map + offset);
        printf("BAR%u[0x%" PRIx64 "] u8  = 0x%02" PRIx8 "\n", bar, offset,
               *p);
        break;
    }
    case 16: {
        volatile uint16_t *p = (volatile uint16_t *)((char *)map + offset);
        printf("BAR%u[0x%" PRIx64 "] u16 = 0x%04" PRIx16 "\n", bar, offset,
               *p);
        break;
    }
    case 32: {
        volatile uint32_t *p = (volatile uint32_t *)((char *)map + offset);
        printf("BAR%u[0x%" PRIx64 "] u32 = 0x%08" PRIx32 "\n", bar, offset,
               *p);
        break;
    }
    case 64: {
        volatile uint64_t *p = (volatile uint64_t *)((char *)map + offset);
        printf("BAR%u[0x%" PRIx64 "] u64 = 0x%016" PRIx64 "\n", bar,
               offset, *p);
        break;
    }
    }

    if (munmap(map, (size_t)len) != 0) {
        perror("munmap");
        return -1;
    }
    return 0;
}
