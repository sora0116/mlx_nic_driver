#ifndef MLXNICD_VFIO_H
#define MLXNICD_VFIO_H

#include <stddef.h>
#include <stdint.h>

struct vfio_device {
    int container_fd;
    int group_fd;
    int device_fd;
    char bdf[32];
    char group[64];
    void *bar0;
    uint64_t bar0_size;
    uint64_t bar0_offset;
    uint16_t pci_command;
};

int vfio_check(const char *bdf);
int vfio_bind(const char *bdf);
int vfio_restore(const char *bdf);
int vfio_probe(const char *bdf);
void vfio_set_stdout_quiet(int quiet);
int vfio_device_open(const char *bdf, struct vfio_device *dev);
void vfio_device_close(struct vfio_device *dev);
int vfio_dma_map(struct vfio_device *dev, void *addr, uint64_t iova,
                 size_t size);
int vfio_dma_unmap(struct vfio_device *dev, uint64_t iova, size_t size);

#endif
