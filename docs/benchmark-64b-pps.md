# 64-byte packet-rate campaign

Date: 2026-07-15

Goal: fix the application/API-visible packet length at 64 bytes and push packet
rate as high as possible.  The accepted evidence is the source `raw-flood`
rate plus `sdn-svr7:eth2` hardware counters.  `rx_bytes_phy` counts the 4-byte
FCS, so a 64-byte Ethernet frame appears as 68 bytes per packet on the peer.

## Current setup

```text
source: sdn-svr6 / 0000:01:00.0 / vfio-pci / mlxnicd raw-flood
peer:   sdn-svr7 / eth2 / 0000:01:00.1 / DPDK testpmd rxonly
peer MAC: ec:0d:9a:44:2d:15
peer command: dpdk-testpmd --file-prefix=svr7flood ... --rxq=8 --rss-ip --rss-udp
```

The current best command is:

```sh
ssh -F /home/sora/.ssh/config sdn-svr6 'cd ~/work/takagi/nicd && \
  sudo ./mlxnicd raw-flood --bdf 0000:01:00.0 --peer-if eth2 \
  --src-mac 02:00:00:00:00:01 --dst-mac ec:0d:9a:44:2d:15 \
  --ethertype 0x0800 --rss-udp --frame-len 64 --queues 8 --count 50000000'
```

Best verified result so far:

```text
source: raw-flood --mpwqe: tx rate 76.835 Mpps / 39.340 Gb/s
peer:   rx_packets_phy +200,000,000
        rx_bytes_phy   +13,600,000,000
        rx_discards_phy +0
```

Final-code rechecks after retaining WQE template preparation and reverting the
dedicated preloaded-burst API:

```text
source: raw-flood: tx rate 45.592 Mpps / 23.343 Gb/s
peer:   rx_packets_phy +50,000,000
        rx_bytes_phy   +3,400,000,000
        rx_discards_phy +0

source: raw-flood: tx rate 45.775 Mpps / 23.437 Gb/s
peer:   rx_packets_phy +50,000,000
        rx_bytes_phy   +3,400,000,000
        rx_discards_phy +0

After adding the MPWQE-required SQ `allow_multi_pkt_send_wqe` bit, the normal
SEND path still works and produced:

```text
source: raw-flood: tx rate 46.109 Mpps / 23.608 Gb/s
source: raw-flood: tx rate 45.727 Mpps / 23.412 Gb/s
source: raw-flood: tx rate 46.236 Mpps / 23.673 Gb/s
```

After compacting the 64B preloaded DMA packet ring, the normal SEND path
improved substantially:

```text
source: raw-flood: tx rate 67.884 Mpps / 34.757 Gb/s
source: raw-flood: tx rate 67.668 Mpps / 34.646 Gb/s
source: raw-flood: tx rate 67.970 Mpps / 34.801 Gb/s
peer:   rx_packets_phy +50,000,000
        rx_bytes_phy   +3,400,000,000
        rx_discards_phy +0
```

The same compact ring made MPWQE the best path:

```text
source: raw-flood --mpwqe: tx rate 75.777 Mpps / 38.798 Gb/s
source: raw-flood --mpwqe: tx rate 75.917 Mpps / 38.869 Gb/s
source: raw-flood --mpwqe: tx rate 75.692 Mpps / 38.754 Gb/s
source: raw-flood --mpwqe: tx rate 75.704 Mpps / 38.760 Gb/s
peer:   rx_packets_phy +50,000,000
        rx_bytes_phy   +3,400,000,000
        rx_discards_phy +0
```

A longer 200M-packet MPWQE run measured 75.017 Mpps / 38.409 Gb/s.

After requesting MPWQE completions only on the WQE that ends each posted
raw-flood burst, the best result improved again:

```text
source: raw-flood --mpwqe: tx rate 76.611 Mpps / 39.225 Gb/s
source: raw-flood --mpwqe: tx rate 76.586 Mpps / 39.212 Gb/s
source: raw-flood --mpwqe: tx rate 76.481 Mpps / 39.158 Gb/s
source: raw-flood --mpwqe --count 200000000: tx rate 76.835 Mpps / 39.340 Gb/s
peer:   rx_packets_phy +200,000,000
        rx_bytes_phy   +13,600,000,000
        rx_discards_phy +0
```

## Implementation changes kept

### 64B RSS/UDP raw-flood frame

Before this work, `raw-flood --rss-udp --frame-len 64` failed because the
benchmark frame builder always inserted a 24-byte mlxnicd benchmark header:

```text
Ethernet 14 + IPv4 20 + UDP 8 + benchmark header 24 = 66 bytes
```

For `raw-flood` only, the code can now build a minimal IPv4/UDP frame when
`--rss-udp --frame-len 64` is requested.  The packet remains 64 bytes at the
API level while still varying the UDP source port in the prebuilt frame set so
that the peer's RSS configuration can spread traffic across RX queues.

### Use one-WQEBB DMA SEND for 64B and larger frames

A 64-byte full-inline SEND WQE needs two 64-byte WQEBBs:

```text
control segment + Ethernet segment + 64 bytes inline data => 96 bytes of WQE
```

The retained path instead inlines only the mandatory L2 header and places the
remaining payload in one data segment:

```text
control segment 16B + inline L2 16B + data segment 16B = 48B <= one WQEBB
```

This keeps the API-visible packet length at 64 bytes but halves the SQ WQEBB
consumption for 64B traffic.  It also reuses the `raw-flood` DMA preload path,
so timed transmission does not copy packet bytes.

### Prebuild the fixed part of flood WQEs

For preloaded `raw-flood` traffic, the SQ ring now also receives a WQE template
for every ring slot.  The fixed fields are prepared once:

- SQ number and descriptor size;
- TIS number;
- inline L2 header;
- data-segment length, mkey, and DMA address.

The timed path still updates the WQE counter/opcode and CQ-update bit because
those depend on producer index and burst boundary.  This avoids a per-packet
`memset()` plus repeated fixed-field writes.

### Experimental enhanced MPWQE path

An experimental `raw-flood --mpwqe` mode now posts opcode `0x29`
(`MLX5_OPCODE_ENHANCED_MPSW`) for API-visible 64-byte packets.  This path is
not the default because it currently does not beat the ordinary SEND path, but
it is useful as a confirmed reference for the mlx5 WQE format:

```text
control segment 16B
Ethernet segment 16B, no inline L2 bytes
N data segments, each pointing at one complete 64B preloaded packet
```

The important details discovered:

- `create_sq` must set `allow_multi_pkt_send_wqe` in `sqc[0x00]`.
- The first working eMPWQE layout uses base descriptor count 2
  (`control + Ethernet segment`), data segments starting at WQE offset `0x20`,
  and each data segment points to the full 64-byte packet.
- The earlier attempted layout with 14 bytes of inline L2 data plus a 50-byte
  data segment failed with TX error CQE `syndrome=0x68 vendor=0x02`.
- A debug CLI, `mlx5-query-eth-cap`, was added to query ethernet-offloads caps,
  but on this setup the direct query currently returns firmware `status=4`
  / `syndrome=0x002b69b6`, even after `ENABLE_HCA` and `SET_ISSI`.  The MPWQE
  datapath test therefore remains the stronger evidence that opcode `0x29` is
  accepted when the SQ and WQE are encoded correctly.

### Compact 64B preloaded DMA ring

The earlier DMA-backed flood path reused the jumbo-capable TX packet ring
layout:

```text
slot 0 at tx_iova + 0 * 4096
slot 1 at tx_iova + 1 * 4096
...
```

For 64-byte packets this was wasteful: each normal SEND WQE inlined the first
14 bytes and asked the HCA to DMA-read only the remaining 50 bytes, but those
50-byte reads were separated by 4096 bytes.  That is a poor access pattern for
IOMMU translation/cache locality and likely also for PCIe read combining.

For `raw-flood --frame-len 64`, `mlxnicd_tx_flood_prepare_q()` now compacts the
preloaded packet ring:

```text
slot 0 at tx_iova + 0 * 64
slot 1 at tx_iova + 1 * 64
...
```

The normal SEND WQE still presents the same API-visible 64-byte packet and
still uses the required L2 inline header.  The only change is the DMA address
used by the data segment:

```text
before: dseg address = slot_base + 16, length = 50
after:  dseg address = slot_base + 14, length = 50
```

MPWQE also uses the compact slot and points each data segment at the full
64-byte packet at `slot_base`.

### Sparse MPWQE completions

The first working MPWQE implementation requested a TX completion on every
MPWQE.  With the current 15-WQEBB MPWQE format, that means one CQE per roughly
58 packets.  This is much more frequent than the normal SEND flood path, which
requests one completion at the end of a 4096-packet raw-flood burst.

The retained MPWQE path now requests completion only on the MPWQE that ends the
current posted burst or when the SQ/packet ring is about to require reclaim.
To support sparse completions, `mlx5_sq_reclaim_cqe()` no longer assumes the
CQE accounts for only one WQE's packet count.  It advances from the old
`cons_index` through every WQE up to the reported completed WQE, summing
`wqe_pkt_count[]` along the way.  This keeps `flood_pkt_cons` correct even when
many MPWQEs completed without individual CQEs.

### Perf profile after sparse CQE

`perf stat` on the retained best-path command shows that the process is not
using all eight visible CPUs continuously:

```text
raw-flood --frame-len 64 --queues 8 --count 50000000 --mpwqe
source: 76.518 Mpps / 39.177 Gb/s
task-clock: 5.284 s over 1.503 s wall, 3.515 CPUs utilized
cycles: 17.967B
instructions: 106.874B
IPC: 5.95
context-switches: 539
cpu-migrations: 5
```

`perf record/report` on the same path shows the dominant samples in TX reclaim
polling rather than in MPWQE WQE construction:

```text
61.26% mlx5_poll_cq_once_mode
34.02% mlx5_poll_cq.part.0.constprop.0
 2.78% mlxnicd_tx_mpwqe64_burst_q
```

Interpretation: after compact-ring and sparse-CQE changes, the source threads
spend much of the sampled time waiting for CQ reclaim/HCA progress.  The
remaining bottleneck is therefore less likely to be the small C loop that fills
MPWQE data segments and more likely to be outstanding-depth, HCA scheduling,
doorbell/CQ behavior, or another device-side limit.

## Experiments

| Attempt | Result | Conclusion |
| --- | --- | --- |
| Baseline 64B L2, no RSS, old full-inline path, 8 queues | 36.114 Mpps / 18.490 Gb/s; peer +50M packets, discard 0 | Baseline. Peer likely receives on too few RX queues because the frame is not IP/UDP RSS-friendly. |
| 64B L2, no RSS, one-WQEBB DMA path, 8 queues | 36.308 Mpps / 18.590 Gb/s; peer +50M packets, discard 0 | Marginal. One-WQEBB DMA alone is not enough. |
| 64B L2/no-RSS queue scaling with DMA path | 1/2/4/8 queues = 11.515 / 22.234 / 36.287 / 36.306 Mpps | Flat after four queues. Without RSS-friendly traffic, peer/source scaling is limited. |
| Add minimal 64B IPv4/UDP frame for `raw-flood --rss-udp` | 45.039 Mpps first run, then 45.419 and 45.619 Mpps | Effective. RSS-friendly 64B traffic is the biggest improvement so far. |
| 64B RSS/UDP source queue scaling | 1/2/4/8 queues = 11.596 / 22.375 / 41.095 / 45.419 Mpps | Effective up to eight queues, but scaling weakens after four queues. |
| Increase 64B raw-flood burst from 4096 to 8192 packets | 44.506 Mpps | Ineffective; reverted. Filling the whole 8192-WQEBB SQ per doorbell is slower than the 4096-packet burst. |
| Temporarily raise source queue limit to 12/16 | 12 queues: 44.846 Mpps; 16 queues: 44.108 Mpps | Ineffective; reverted to max 8 queues. More source queues add overhead and do not improve peer-confirmed PPS. |
| Force 64B back to full-inline while keeping minimal RSS/UDP | 38.336 Mpps | Ineffective. One-WQEBB DMA is better for 64B despite the small host-memory DMA read. |
| DPDK `testpmd txonly`, one core/one queue, `txq_mem_algn=0`, `set txpkts 64` | Peer +207,312,002 packets during the 6s command window, about 34.6 Mpps | Custom driver is faster than this DPDK one-core baseline. |
| DPDK `testpmd txonly`, requested eight workers | Fails: `lcore 8 unavailable`; `sdn-svr6` exposes lcores 0--7 only | Invalid configuration. |
| DPDK `testpmd txonly`, seven workers/seven queues | Peer +201,897,538 packets during the 6s command window, about 33.6 Mpps | Ineffective. The DPDK txonly path reports many TX drops and is below the custom-driver result here. |
| Prebuild fixed WQE fields in `mlxnicd_tx_flood_prepare_q()` | 45.933 Mpps best observed; follow-up runs 45.592 and 45.775 Mpps | Slightly effective / within normal variance, retained because it removes per-packet WQE initialization work without changing API behavior. |
| Add a new public `mlxnicd_tx_flood_burst_q()` API to bypass the `mlxnicd_pkt` array in timed raw-flood | 45.617 Mpps | Ineffective; reverted. It did not beat the WQE-template path and added API surface area. |
| eMPWQE attempt with 14B inline L2 + 50B data segment | Fails immediately: TX error CQE `syndrome=0x68 vendor=0x02`; peer counter unchanged | Invalid WQE encoding for this SQ/opcode combination. |
| Add SQ `allow_multi_pkt_send_wqe` bit but keep the 14B-inline eMPWQE layout | Still fails with the same TX error CQE | The SQ permission bit is required but not sufficient; the WQE layout is the actual blocker. |
| eMPWQE with no inline L2 and full 64B data segments | Works. 1 queue: 30.736 and 30.665 Mpps; peer +50M packets, +3.4GB, discard 0 | Effective per queue versus ordinary SEND's earlier 11.596 Mpps, but not the best overall result. |
| eMPWQE queue scaling with full 64B data segments | 1/2/4/8 queues = 30.736 / 36.342 / 36.336 / 36.394 Mpps | Ineffective as a replacement for normal SEND. It flattens around 36.3 Mpps and loses to normal SEND with 8 queues. |
| Recheck normal SEND after enabling `allow_multi_pkt_send_wqe` on SQs | 46.109, 45.727, 46.236 Mpps | Normal SEND is still the best path; new best observed is 46.236 Mpps. |
| Compact the 64B preloaded DMA packet ring from 4096B slots to 64B slots, normal SEND | 67.884, 67.668, 67.970 Mpps; peer +50M packets, +3.4GB, discard 0 | Very effective. The previous 4096B stride was a major source-side bottleneck for tiny DMA reads. |
| Compact-ring normal SEND queue scaling | 1/2/4/8 queues = 16.014 / 28.003 / 52.339 / 67.769 Mpps | Effective, but still scales sublinearly after 4 queues. |
| Compact-ring MPWQE queue scaling | 1/2/4/8 queues = 40.164 / 61.404 / 73.728 / 75.917 Mpps; repeat 8q = 75.692 and 75.704 Mpps; 200M-packet 8q = 75.017 Mpps | New best. MPWQE becomes better than normal SEND once packet DMA reads are compact. |
| Tune compact-ring MPWQE queue count | 5/6/7 queues = 74.747 / 75.379 / 75.674 Mpps | 8 queues remains best in observed runs. |
| Remove whole-WQE `memset()` from MPWQE timed path and clear only control/Ethernet segment bytes | 75.813, 75.625, 75.570 Mpps | No improvement over the 75.917 Mpps best; reverted to the simpler full-WQE memset for safety. |
| Request MPWQE CQ_UPDATE only at raw-flood burst boundaries and make reclaim account for sparse CQEs | 8q 50M-packet runs: 76.611 / 76.586 / 76.481 Mpps; 200M-packet run: 76.835 Mpps; peer +200M packets, +13.6GB, discard 0 | Effective and retained. Reducing CQE pressure helps MPWQE slightly. |
| Raise MPWQE size from 15 to 16 WQEBBs | Fails immediately on 1q/1000-packet smoke test: TX error CQE `syndrome=0x68 vendor=0x02` | Invalid. The 15-WQEBB limit is real on this device/format; reverted. |
| Lower MPWQE size from 15 to 14 WQEBBs | 76.579 and 76.495 Mpps | Ineffective. Smaller MPWQEs do not improve scheduling/cache behavior enough to offset the lower packets-per-WQE; reverted. |
| Lower MPWQE size from 15 to 13 WQEBBs | 76.394 and 76.489 Mpps | Ineffective; reverted. |
| Increase raw-flood burst from 4096 to 8192 with compact-ring sparse-CQE MPWQE | 76.073 and 76.167 Mpps | Ineffective. Larger burst does not beat the retained 4096-burst 76.835 Mpps result; reverted. |
| Decrease raw-flood burst from 4096 to 2048 with compact-ring sparse-CQE MPWQE | 76.708 and 76.638 Mpps | Ineffective; reverted. |
| Decrease raw-flood burst from 4096 to 1024 with compact-ring sparse-CQE MPWQE | 76.418 and 76.403 Mpps | Ineffective; reverted. |
| Re-raise source queue limit to 12/16 after compact-ring sparse-CQE MPWQE | 12 queues: 76.318 Mpps; 16 queues: 76.049 Mpps | Ineffective. Oversubscribing the 8 visible CPUs and adding SQs does not beat 8 queues; reverted to max 8 queues. |
| Disable raw-flood worker CPU pinning | 76.708 and 76.825 Mpps | About equal to the best but not a clear improvement; retained explicit queue_id-to-CPU pinning for reproducibility. |
| Reverse raw-flood worker CPU pinning | 76.426 and 76.467 Mpps | Ineffective; reverted. |
| Use a constant 15-WQEBB `memset()` size for full-size MPWQEs, falling back to variable size only near ring wrap/short WQEs | 50M-packet runs: 76.666 and 76.980 Mpps; 200M-packet run: 76.672 Mpps, peer +200M packets, +13.6GB, discard 0 | Not retained. One short run exceeded the old 50M best, but the longer 200M run was below the retained 76.835 Mpps result. |
| Optimize MPWQE data-segment loop by writing `byte_count+mkey` as one 64-bit word and incrementing contiguous IOVA in the non-wrap case | 76.561 and 76.515 Mpps | Ineffective; reverted. The compiler/current store pattern was already adequate or the bottleneck is elsewhere. |
| Build with `-O3 -g` instead of default `-O2 -g` | 49.208 and 48.775 Mpps | Strongly negative. Keep default `-O2 -g`. |
| Build with `-O3 -march=native -g` | 48.647 and 48.384 Mpps | Strongly negative. Keep default `-O2 -g`. |
| Build with `-O2 -march=native -g` | 76.686 and 76.423 Mpps | No improvement over retained best; keep portable default `-O2 -g`. |
| Build with `-O2 -flto -g` and link with `-flto` | 76.474 and 76.811 Mpps | No improvement over retained best; keep default build flags. |
| Build with `-O2 -fno-plt -g` | 76.594 and 76.609 Mpps | No improvement; keep default build flags. |
| Remove the final post-MMIO full barrier from `mlx5_sq_ring_send()` | 50M-packet runs: 76.354 and 76.921 Mpps; 200M-packet run: 76.778 Mpps, peer +200M packets, +13.6GB, discard 0 | Not retained. It did not beat the retained 76.835 Mpps long-run result. |
| Use `perf stat` and `perf record/report` on the retained best path | `task-clock` shows 3.515 CPUs utilized; report samples are dominated by `mlx5_poll_cq*` | Diagnostic. Indicates wait/reclaim/HCA progress dominates over MPWQE WQE construction. |
| Increase TX packet ring slots from 8192 to 16384 and widen TX IOVA queue stride from 32MiB to 128MiB | First attempt without widening IOVA stride failed with `VFIO_IOMMU_MAP_DMA: File exists`; widened-stride runs: 76.612 and 76.620 Mpps | Ineffective; reverted. More outstanding packet slots did not improve throughput. |

## Current interpretation

The current custom-driver ceiling for API-visible 64-byte packets is about
76.5--76.8 Mpps on this setup.  The retained optimizations are:

1. keep the packet RSS-friendly at exactly 64 bytes by using a minimal IPv4/UDP
   raw-flood frame instead of the larger benchmark header format;
2. use the one-WQEBB L2-inline + DMA data-segment SEND layout for 64B packets;
3. prebuild the fixed fields of the per-slot flood WQEs;
4. compact the 64B preloaded DMA packet ring from 4096B slots to 64B slots;
5. allow multi-packet-send WQEs on SQs and use the experimental eMPWQE path for
   the current best result;
6. request MPWQE completions sparsely at posted-burst boundaries rather than
   for every MPWQE.

The main remaining gap to theoretical 100GbE minimum-frame packet rate is not
peer drops: every accepted run above was confirmed with `rx_discards_phy +0`.
The compact-ring result shows that the previous ceiling was largely due to the
source DMA memory layout, not the peer.  The next likely bottleneck is still on
the source side: MPWQE scales from 40 Mpps at one queue to only about 76 Mpps at
eight queues, so per-core TX work, doorbell/CQ behavior, or HCA scheduling
across SQs is now the main area to investigate.
