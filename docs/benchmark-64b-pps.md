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
source: raw-flood: tx rate 45.933 Mpps / 23.518 Gb/s
peer:   rx_packets_phy +50,000,000
        rx_bytes_phy   +3,400,000,000
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

## Current interpretation

The current custom-driver ceiling for API-visible 64-byte packets is about
45.6--45.9 Mpps on this setup.  The retained optimizations are:

1. keep the packet RSS-friendly at exactly 64 bytes by using a minimal IPv4/UDP
   raw-flood frame instead of the larger benchmark header format;
2. use the one-WQEBB L2-inline + DMA data-segment SEND layout for 64B packets;
3. prebuild the fixed fields of the per-slot flood WQEs.

The main remaining gap to theoretical 100GbE minimum-frame packet rate is not
peer drops: every accepted run above was confirmed with `rx_discards_phy +0`.
The next likely step is a fundamentally different mlx5 TX mode such as
multi-packet WQE / enhanced MPWQE, because ordinary SEND still posts one WQE
per packet.
