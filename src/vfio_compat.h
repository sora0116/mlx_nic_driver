#ifndef MLXNICD_VFIO_COMPAT_H
#define MLXNICD_VFIO_COMPAT_H

#if defined(__has_include)
#if __has_include(<linux/vfio.h>)
#include <linux/vfio.h>
#define MLXNICD_HAVE_SYSTEM_VFIO_H 1
#endif
#endif

#ifndef MLXNICD_HAVE_SYSTEM_VFIO_H

#include <stdint.h>
#include <sys/ioctl.h>

typedef uint8_t __u8;
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef uint64_t __aligned_u64;

#define VFIO_API_VERSION 0
#define VFIO_TYPE1_IOMMU 1
#define VFIO_TYPE1v2_IOMMU 3
#define VFIO_TYPE (';')
#define VFIO_BASE 100

#define VFIO_GET_API_VERSION _IO(VFIO_TYPE, VFIO_BASE + 0)
#define VFIO_CHECK_EXTENSION _IO(VFIO_TYPE, VFIO_BASE + 1)
#define VFIO_SET_IOMMU _IO(VFIO_TYPE, VFIO_BASE + 2)

struct vfio_group_status {
    __u32 argsz;
    __u32 flags;
#define VFIO_GROUP_FLAGS_VIABLE (1 << 0)
#define VFIO_GROUP_FLAGS_CONTAINER_SET (1 << 1)
};
#define VFIO_GROUP_GET_STATUS _IO(VFIO_TYPE, VFIO_BASE + 3)
#define VFIO_GROUP_SET_CONTAINER _IO(VFIO_TYPE, VFIO_BASE + 4)
#define VFIO_GROUP_UNSET_CONTAINER _IO(VFIO_TYPE, VFIO_BASE + 5)
#define VFIO_GROUP_GET_DEVICE_FD _IO(VFIO_TYPE, VFIO_BASE + 6)

struct vfio_device_info {
    __u32 argsz;
    __u32 flags;
#define VFIO_DEVICE_FLAGS_RESET (1 << 0)
#define VFIO_DEVICE_FLAGS_PCI (1 << 1)
    __u32 num_regions;
    __u32 num_irqs;
    __u32 cap_offset;
    __u32 pad;
};
#define VFIO_DEVICE_GET_INFO _IO(VFIO_TYPE, VFIO_BASE + 7)

struct vfio_region_info {
    __u32 argsz;
    __u32 flags;
#define VFIO_REGION_INFO_FLAG_READ (1 << 0)
#define VFIO_REGION_INFO_FLAG_WRITE (1 << 1)
#define VFIO_REGION_INFO_FLAG_MMAP (1 << 2)
#define VFIO_REGION_INFO_FLAG_CAPS (1 << 3)
    __u32 index;
    __u32 cap_offset;
    __aligned_u64 size;
    __aligned_u64 offset;
};
#define VFIO_DEVICE_GET_REGION_INFO _IO(VFIO_TYPE, VFIO_BASE + 8)

enum {
    VFIO_PCI_BAR0_REGION_INDEX,
    VFIO_PCI_BAR1_REGION_INDEX,
    VFIO_PCI_BAR2_REGION_INDEX,
    VFIO_PCI_BAR3_REGION_INDEX,
    VFIO_PCI_BAR4_REGION_INDEX,
    VFIO_PCI_BAR5_REGION_INDEX,
    VFIO_PCI_ROM_REGION_INDEX,
    VFIO_PCI_CONFIG_REGION_INDEX,
    VFIO_PCI_VGA_REGION_INDEX,
    VFIO_PCI_NUM_REGIONS = 9
};

#define VFIO_DEVICE_RESET _IO(VFIO_TYPE, VFIO_BASE + 11)

struct vfio_iommu_type1_info {
    __u32 argsz;
    __u32 flags;
#define VFIO_IOMMU_INFO_PGSIZES (1 << 0)
    __aligned_u64 iova_pgsizes;
    __u32 cap_offset;
    __u32 pad;
};
#define VFIO_IOMMU_GET_INFO _IO(VFIO_TYPE, VFIO_BASE + 12)

struct vfio_iommu_type1_dma_map {
    __u32 argsz;
    __u32 flags;
#define VFIO_DMA_MAP_FLAG_READ (1 << 0)
#define VFIO_DMA_MAP_FLAG_WRITE (1 << 1)
    __u64 vaddr;
    __u64 iova;
    __u64 size;
};
#define VFIO_IOMMU_MAP_DMA _IO(VFIO_TYPE, VFIO_BASE + 13)

struct vfio_iommu_type1_dma_unmap {
    __u32 argsz;
    __u32 flags;
    __u64 iova;
    __u64 size;
    __u8 data[];
};
#define VFIO_IOMMU_UNMAP_DMA _IO(VFIO_TYPE, VFIO_BASE + 14)

#endif

#endif
