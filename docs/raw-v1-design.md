# Raw packet TX/RX v1 design

This project deliberately avoids validating raw packet TX/RX through Linux
`AF_PACKET` or DPDK. The target datapath is:

```text
mlxnicd -> VFIO -> ConnectX-5 Ex BAR0 + DMA -> mlx5 SQ/RQ/CQ -> wire
```

## Current implementation boundary

Implemented:

- PCI/sysfs inspection and BAR read.
- VFIO/IOMMU readiness checks.
- `vfio-bind` and `vfio-restore` through sysfs.
- VFIO container/group/device open.
- BAR0 mmap through VFIO.
- Page-aligned DMA allocation and VFIO DMA map/unmap smoke test.
- `raw-loop` CLI with a hard VFIO probe gate.
- mlx5 init segment inspection.
- First `QUERY_ISSI` command descriptor path; verified with
  `supported_issi_dw0=0x00000002`.

Not yet implemented:

- mlx5 command interface.
- mlx5 object creation for PD/UAR/MKEY/CQ/SQ/RQ.
- Doorbell record and UAR writes.
- CQ polling and packet buffer parsing.

## Required host state

`sdn-svr6` has been rebooted with `intel_iommu=on iommu=pt`, and IOMMU
groups are populated. The next implementation stage requires:

- `vfio-pci` loaded after boot when needed;
- one ConnectX function detached from `mlx5_core`;
- the peer function left on Linux for tcpdump/raw-socket verification.

## Minimal mlx5 bring-up sequence

The next code increment should implement and test one step at a time:

1. Open VFIO container, group, and device for the BDF.
2. mmap BAR0 via VFIO region info.
3. Allocate page-aligned DMA memory and map it with VFIO.
4. Locate mlx5 initialization segment and command interface registers.
5. Run basic commands: `QUERY_ISSI`, optional `SET_ISSI`, `QUERY_HCA_CAP`.
6. Initialize HCA if required by device state.
7. Create the minimum objects needed for raw Ethernet queues: PD, UAR, MKEY,
   CQ, SQ, and RQ.
8. Post RX buffers, ring RQ doorbell, post one TX WQE, ring SQ doorbell.
9. Poll CQEs, print TX completion and RX packet bytes.
10. Destroy objects and unmap DMA in reverse order.

Steps 1-5 are partially implemented: `QUERY_ISSI` works, `SET_ISSI` and
`QUERY_HCA_CAP` are next.

Primary reference on `sdn-svr6`:

```text
/usr/src/kernels/$(uname -r)/include/linux/mlx5/mlx5_ifc.h
/usr/src/kernels/$(uname -r)/include/linux/mlx5/device.h
/usr/src/kernels/$(uname -r)/include/linux/mlx5/doorbell.h
/usr/src/kernels/$(uname -r)/drivers/net/ethernet/mellanox/mlx5/core
```
