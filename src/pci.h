#ifndef MLXNICD_PCI_H
#define MLXNICD_PCI_H

#include <stddef.h>
#include <stdint.h>

#define MLX_VENDOR_ID 0x15b3u

int mlx_list_devices(void);
int mlx_inspect_device(const char *bdf);
int mlx_bar_read(const char *bdf, unsigned int bar, uint64_t offset,
                 unsigned int width_bits);

int parse_u64(const char *s, uint64_t *out);
int parse_u32(const char *s, uint32_t *out);

#endif
