# 10 Mpps benchmark investigation

This document records every measured optimisation attempt on the `benchmark`
branch. The target is 10 Mpps bidirectional raw-packet benchmark throughput;
until both hosts can use VFIO, the peer is DPDK `testpmd` on `sdn-svr5` and the
source/receiver is this repository's driver on `sdn-svr6`.

## Test topology and metric

```text
mlxnicd raw-bench (sdn-svr6, 0000:01:00.0)
  -> 100 GbE link -> DPDK testpmd macswap (sdn-svr5, 0000:01:00.1)
  -> 100 GbE link -> mlxnicd raw-bench
```

The reported end-to-end Mpps is completed request/reply pairs divided by the
span from first transmit to last receive. Frames are minimum Ethernet-sized
frames (60 bytes before FCS). `--throughput-only` intentionally reports zero
latency statistics: it removes per-packet timestamping to measure packet-rate
headroom rather than RTT distribution.

## Measured results

| Step | Change | Command mode | Result | Conclusion |
|---|---|---|---:|---|
| Baseline | Initial 256-entry queues, per-packet RX repost | L2, window 128 | 0.314 Mpps | Baseline before datapath optimisation. |
| 1 | Suppress quiet-mode WQE/CQE dump loops; remove 4 KiB RX-buffer clear on every repost | L2, window 128 | 0.633 Mpps | Effective; debug work and cache writes were on the hot path. |
| 2 | RX burst of 32, one batched RX repost, nonblocking trailing CQ probes | L2, window 128 | 2.035 Mpps | Highly effective; removes per-packet repost barriers and polling overhead. |
| 3 | DPDK `--burst=128 --rxd=1024 --txd=1024` | L2, window 128 | 2.054 Mpps | Ineffective (+0.9%). |
| 4 | SQ=2048, CQ=2048, RQ=4096, window=1024 | L2, window 128 | 2.160 Mpps | Queue expansion is correct and removes the old size limit. |
| 5 | Same queues, window 1024, one DPDK forwarding core | L2 | 1.201 Mpps, 832 us RTT | Ineffective: peer backlog dominates. |
| 6 | IPv4/UDP frames with four `testpmd` RX/TX queues and four forwarding cores | RSS, window 1024 | 1.152 Mpps | Ineffective; testpmd consumes about 385% CPU but does not improve this loopback workload. |
| 7 | Direct cyclic slot lookup (`seq % window`) instead of O(window) slot scan | RSS, window 1024 | 2.275 Mpps | Effective; out-of-order replies require waiting for a cyclic slot rather than overwriting it. |
| 8 | Prebuilt benchmark frame template; patch only sequence/timestamp/UDP flow key | RSS, window 1024 | 2.469 Mpps | Effective (+8.5%). |
| 9 | `--throughput-only`: no per-packet timestamp or latency accounting | RSS, window 1024 | **2.708 Mpps** | Effective (+9.7%); current best result. |
| 10 | Four queue runtime, queue-specific burst API, four-RQ RQT and round-robin receive poll | RSS, window 128 | 2.708 Mpps | Correct but intentionally not faster: one thread still owns all polling. |
| 11 | Four queue-specific worker threads, atomic global window/reply accounting | RSS throughput-only, window 1024 | **8.320 Mpps** | Effective (+207% over one worker); all four queues received 500,000 frames in a one-million-pair run. |
| 12 | Four workers, window 2048 | RSS throughput-only | 8.667 Mpps | Small improvement, still below target. |
| 13 | Eight-core DPDK peer | Four-worker source, window 2048 | 8.483 Mpps | Ineffective; peer used about 770% CPU but rate fell. |
| 14 | Eight queue/eight worker source | window 1024 | 7.806 Mpps | Correct and evenly distributed, but slower than four workers. |
| 15 | Eight queue/eight worker source | window 2048 | failed at 69,889/67,841 | Unrecovered global-window packet loss; do not use this setting. |
| 16 | Four workers, window 3072 | Eight-core peer | 8.308 Mpps | Ineffective; larger window increases queueing rather than throughput. |
| 17 | Pin each source worker to CPU equal to its queue ID | Four workers, window 2048 | 8.110 Mpps | Ineffective (-6.4%); scheduler placement is better on this host. Reverted. |
| 18 | Paper-inspired peer: one DPDK forwarding core polls four RX/TX queues, burst 256, 2,048 descriptors | Four-worker source, window 2048 | 8.197 Mpps | Ineffective. This host does not reproduce the paper's single-core multi-queue gain in a MAC-swap request/reply path. |
| 19 | Align peer burst with the source's 32-packet TX batch (`--burst=32`, 2,048 descriptors) | Four-worker source, window 2048 | 8.125 Mpps | Ineffective; smaller peer batches reduce throughput. |
| 20 | Restore 4 forwarding cores, 4 RX/TX queues, burst 128, 1,024 descriptors | Four-worker source, window 2048 | 8.291 Mpps (8.667 Mpps historical best) | Current best peer configuration. The difference from step 12 is normal run-to-run variation; both runs completed all one million pairs. |
| 21 | Replace `testpmd` with repository `dpdk-macswap-peer`: dedicated RX burst → in-place MAC swap → TX burst loop | Four DPDK workers/queues, window 2048 | 8.315, 8.350 Mpps | Correct and reproducible, but not faster than the 8.667 Mpps `testpmd` peak. `testpmd` control-plane overhead is not the limiting factor. |
| 22 | Give each source worker a private sequence range, reply counter, and one-quarter of the global outstanding window; remove per-packet shared `next_seq`/`replies` atomics | Four source workers/queues, dedicated DPDK peer, window 2048 | **10.617, 10.588 Mpps** | Effective (+27% over the 8.350 Mpps dedicated-peer baseline). Both runs completed 1,000,000/1,000,000 pairs; every worker completed exactly 250,000 sends and replies. **10 Mpps target achieved.** |
| 23 | DPDK-style asynchronous TX: ring the SQ doorbell immediately; consume at most one ready TX CQE before the next burst rather than waiting for the current batch | Four workers/queues, dedicated peer, window 2048 | **16.056 Mpps** | Highly effective (+51% over step 22). Synchronous TX completion waiting was the main software limiter. |
| 24 | Increase source TX burst from 32 to 128 packets | Four workers/queues, window 2048 | 16.214 Mpps | Marginal improvement (+1.0%). |
| 25 | Increase window from 2048 to 3072 | Four workers/queues | failed at 356,163/353,091 | Unsafe: every worker stopped with exactly 768 unreturned packets. Do not exceed window 2048 before explicit SQ completion/reclaim tracking is implemented. |
| 26 | Batch RX CQ consumer-index notification once per burst instead of once per CQE | Four workers/queues, window 2048 | 16.141 Mpps | Correct but no measurable improvement; received CQEs are usually not sufficiently accumulated. |
| 27 | Deep source TX burst up to the per-worker window (512 packets) | Four workers/queues, window 2048 | 16.087 Mpps | Ineffective; larger bursts add queueing and do not improve the rate. |
| 28 | Eight source workers/queues and eight-core `testpmd` peer | window 2048 | 15.137 Mpps | Ineffective; four queues/workers remain faster. |

## Confirmed bottlenecks

### Outstanding-window limit

At the early 51--60 us RTT, window 128 has a ceiling near 2.1--2.5 Mpps
(`window / RTT`). The measured 2.035 Mpps matched that bound. Increasing the
window is necessary, but not sufficient: at window 1024 the peer can queue and
increase RTT substantially.

### Single-core driver datapath

`perf stat` on the current throughput-only run measured 1,461,992,660 cycles
and 5,065,826,457 instructions for one million completed pairs (about 1,462
cycles/pair, 3.47 IPC). A single Atom core cannot sustain 10 Mpps at this cost.
The next architectural step is multiple SQ/RQ/CQ pairs and worker ownership;
the benchmark packet's `--rss-udp` mode provides distinct UDP flows needed for
hardware RSS distribution.

### Multi-worker atomic contention (resolved)

The first four-worker implementation used shared atomic `next_seq` and
`replies` counters in every hot-loop iteration.  Sending a packet performed a
load/CAS sequence on `next_seq`; receiving a reply performed an atomic
increment on `replies`.  All four workers therefore repeatedly invalidated the
same cache lines even though each already owned a separate SQ and RX queue.

The parallel benchmark now partitions the packet-count range evenly among the
workers.  A worker owns its `seq_begin..seq_end` range, its local next-sequence
value, and its reply counter.  It may transmit while its local in-flight count
is below `window / queue_count`; the aggregate outstanding limit remains 2048.
Only the rare error flag is shared.  With the dedicated DPDK peer, two
one-million-pair runs reached 10.617 and 10.588 Mpps, respectively.  Each
worker reported exactly 250,000 transmitted packets and 250,000 replies.
This is both a correctness check for the partitioning and evidence that cache
line contention, rather than the DPDK peer, was the final blocker to 10 Mpps.

### Line-rate investigation (in progress)

The wire link is 100 GbE full duplex.  Minimum 64-byte Ethernet frames have a
theoretical one-way line rate of approximately 148.8 Mpps.  The current
request/reply metric counts one completed pair, so 16.214 Mpps corresponds to
about 8.56 Gbps in the reported 60-byte-frame accounting and is far from the
wire limit.

The first DPDK-inspired change was the important one: `mlxnicd_tx_burst_q()`
no longer blocks for the TX CQE created by the burst it has just doorbelled.
Instead it polls one already-ready CQE before preparing the next burst.  This
preserves a bounded benchmark SQ occupancy while allowing hardware DMA and
wire transmission to overlap frame preparation, RX polling, and peer work.
It raised the sustained million-pair rate from 10.6 to 16.1 Mpps.

`perf record` on a ten-million-pair run still attributes substantial sampled
time to `mlx5_poll_cq_once`, with `mlxnicd_tx_burst_q()` the next largest
driver function.  CQ consumer-index updates were therefore batched for RX,
but this had no measurable effect because the current reply path typically
does not accumulate a large CQ burst before it is polled.  Increasing software
burst size, window depth, or queue/core count also did not improve the rate;
all measured outcomes are listed above.

#### Physical blocker: `sdn-svr5` PCIe downtraining

The DPDK peer host is not currently capable of a 100 GbE host-DMA rate.  Its
NIC (`0000:01:00.1`) reports `current_link_speed=2.5 GT/s PCIe` and
`current_link_width=16`, while its maximum is 16.0 GT/s x16.  Kernel boot logs
identify root port `0000:00:01.0` as the limit and quantify only 32.000 Gb/s
available PCIe bandwidth, versus 252.048 Gb/s for Gen4 x16.  This makes a
100 GbE DPDK forwarding peer physically impossible regardless of poll-loop
optimisation.

Before interpreting further software experiments as a line-rate limit, set
the `sdn-svr5` slot/root-port policy to PCIe Gen4/Auto in firmware (and check
physical seating/riser quality), then reboot and verify:

```sh
cat /sys/bus/pci/devices/0000:01:00.1/current_link_speed
cat /sys/bus/pci/devices/0000:01:00.1/current_link_width
```

The required result is `16.0 GT/s PCIe` and width `16`.  Only then can a
100 GbE line-rate peer experiment be meaningful.

### Research-paper comparison and DPDK peer conclusion

`references/main.pdf` reports 38 to 94 Mpps per flow on a ConnectX-7 (200 GbE,
PCIe Gen5 x16) system.  Its key observation is not a generic `testpmd` flag:
when a *single* core polls four or more hardware RX queues, independent DMA
transactions can progress in parallel and each visit can consume a full
256-packet batch.  To make one flow eligible for that distribution, the paper
changes a per-packet 5-tuple field and uses RSS/Flow Director to steer packet
batches round-robin across the queues.

The benchmark already implements the corresponding steering prerequisite:
`--rss-udp` makes each packet an IPv4/UDP packet and varies its UDP source
port, while the peer enables `--rss-ip --rss-udp` on four RX queues.  The
driver's indirect TIR hashes IPv4/UDP 4-tuples and its RQT uses all four RQs,
so the source-side verification has shown an even 25% distribution.

The remaining paper-inspired peer experiment was therefore to replace the
four forwarding cores with one forwarding core that polls all four queues.
It reached 8.197 Mpps, below the four-core peer's observed 8.291--8.667 Mpps.
Reducing `testpmd`'s burst from 128 to the source's 32-packet transmit batch
also fell to 8.125 Mpps.  These measurements keep the same source, traffic,
window, and 4-queue RSS setup, so they isolate the peer configuration.

The paper's absolute result should not be used as a direct target for this
machine: its evaluation hardware is substantially newer and it reports
receive-only and one-way forwarding scenarios as well as loopback.  Here a
completed benchmark packet needs both a custom-driver transmit/receive path
on `sdn-svr6` and a DPDK MAC-swap receive/transmit path on `sdn-svr5`.
Accordingly, the active peer uses four forwarding cores rather than the
paper's single-core receive-only configuration.  Further peer-only `testpmd`
tuning is not currently justified; the next throughput gain should remove
contention in the source's multi-worker benchmark datapath (notably the
global atomic sequence and reply counters).

To verify that conclusion independently of `testpmd`, this branch provides
`peer/dpdk-macswap-peer`.  It creates four RSS RX/TX queue pairs and gives one
DPDK worker to each pair.  Its entire hot loop is `rte_eth_rx_burst`, an
in-place Ethernet source/destination MAC exchange, then `rte_eth_tx_burst`;
unsent mbufs are freed.  Two one-million-pair runs reached 8.315 and 8.350
Mpps.  Thus the purpose-built peer works, but does not exceed the generic
peer's 8.667 Mpps peak.

## Multi-queue bring-up notes

The driver now supports up to four queue pairs through `queue_count` in
`mlxnicd_dev_config` and the queue-specific APIs `mlxnicd_tx_burst_q`,
`mlxnicd_rx_burst_q`, and `mlxnicd_rx_release_q`. The original APIs remain
queue-0 wrappers.

The first four-queue run exposed an IOVA overlap: queue 1's SQ region collided
with queue 2's TX CQ region, and VFIO correctly rejected the second mapping
with `VFIO_IOMMU_MAP_DMA: File exists`. TX CQs and SQs now use disjoint IOVA
ranges. A second issue was a 30-second blocking poll on queue 0; RSS may place
the next completion on another queue. Multiqueue mode now probes each queue
nonblocking in round-robin order. The corrected four-queue test completed
10,000 IPv4/UDP RSS request/reply pairs without loss. A subsequent 100,000-pair
distribution run showed all 200,000 received frames (local copies plus replies)
on RX queue 0 and none on queues 1--3. The current indirect TIR sets a hash
function but does not program the TIRC `rx_hash_field_select` / L3/L4 protocol
field selectors, so the hardware hash is effectively fixed. Worker threading
must wait until those IFC fields are implemented and every queue receives a
nonzero share of the UDP flows.

The selector layout was then taken from the Linux mlx5 IFC definition. An
initial attempt used byte `0x48`; firmware rejected the TIR because that byte
belongs to the Toeplitz-key region while `INVERTED_XOR8` is selected. The outer
selector actually begins at TIRC byte `0x50` (bit `0x280`). Programming IPv4,
UDP and the four source/destination IP/port fields there fixed the distribution:
a 100,000-pair run received exactly 50,000 frames on each of four RX queues.
This establishes hardware RSS as a working prerequisite for one worker per
queue.

This is a correctness milestone, not yet a parallel benchmark. A worker must
own both an SQ and an RX queue, but RSS chooses the RX queue from the packet
flow rather than from the source TX queue. Before assigning one worker per
queue, the next experiment records the flow-to-RQ mapping and partitions
benchmark flows accordingly; otherwise workers would need a shared completion
table and lose much of the intended cache locality.

### Temporary peer limitation

`sdn-svr5` presently cannot bind its NIC to VFIO because no IOMMU groups are
exported, even though the kernel command line includes `intel_iommu=on
iommu=pt`. DPDK is a temporary peer only. The paper-inspired one-core,
four-queue configuration and the smaller peer-burst configuration are recorded
above; neither improved this machine's bidirectional benchmark.

## Current commands

Start the current best four-core RSS peer:

```sh
ssh sdn-svr5 "setsid -f sh -c 'tail -f /dev/null | sudo dpdk-testpmd \
  -l 1,2,3,4,5 -n 4 -a 0000:01:00.1 -- \
  --nb-cores=4 --rxq=4 --txq=4 --rss-ip --rss-udp \
  --forward-mode=macswap --port-topology=loop --burst=128 \
  --rxd=1024 --txd=1024 --auto-start' \
  >/tmp/testpmd-mlxnicd.log 2>&1"
```

Run the current best source benchmark:

```sh
ssh sdn-svr6 'cd ~/work/takagi/nicd && sudo ./mlxnicd raw-bench \
  --bdf 0000:01:00.0 --peer-if eth2 \
  --src-mac 02:00:00:00:00:06 --dst-mac ff:ff:ff:ff:ff:ff \
  --ethertype 0x0800 --rss-udp --throughput-only \
  --queues 4 --count 1000000 --window 2048 --timeout-ms 30000'
```

Build and start the dedicated peer alternative (on `sdn-svr5`):

```sh
ssh sdn-svr5 'cd ~/work/takagi/nicd && make dpdk-peer'
ssh sdn-svr5 "setsid -f sh -c 'sudo ~/work/takagi/nicd/peer/dpdk-macswap-peer \\
  -l 1,2,3,4,5 -n 4 -a 0000:01:00.1' \\
  >/tmp/dpdk-macswap-peer.log 2>&1"
```

`dpdk-macswap-peer` intentionally defaults to the comparison configuration:
four queues, burst 128, and 1,024 RX/TX descriptors.  It requires exactly one
allowed NIC and one main lcore plus one worker lcore per queue.

## PCIe Gen1 x16 effective-limit milestone (2026-07-14)

The host-DMA target was changed from 100 GbE wire rate to the maximum rate of
the currently downtrained `sdn-svr5` PCIe link.  `raw-bench` is not suitable
for this measurement: it counts a request/reply pair and spends source-side
work receiving its own reflected traffic and replies.  The new `raw-flood`
command is a one-way TX measurement:

```text
mlxnicd raw-flood / sdn-svr6 / 0000:01:00.0
  -> 100 GbE wire -> DPDK testpmd RX-only sink / sdn-svr5 / 0000:01:00.1
```

It creates TX-only mlxnicd queues, gives each worker a disjoint packet sequence
range, varies the IPv4/UDP source port with `--rss-udp`, and calls
`mlxnicd_tx_flush_q()` before reporting completion.  A TX CQE is requested only
for the final WQE of a posted burst.  The SQ consumer index is reconstructed
from that CQE's WQE counter, so it safely reclaims every preceding WQE in the
burst.  This supports one-way traffic without source RX/reply work.

The TX SQ was expanded to 8,192 64-byte WQEBBs (512 KiB, 128 PAS entries).
Each supported full-inline frame uses two WQEBBs.  `mlxnicd_tx_burst_q()` now
waits for a TX CQE only when there is no room for another WQE; it does not wait
for a just-doorbelled burst.  `raw-flood` batches up to 4,096 frames per queue
and keeps those worker-private buffers on the heap rather than a pthread stack.

Measured source results, eight source queues and an eight-worker DPDK RX-only
sink:

| Frame length | Source rate | Peer result | Interpretation |
| --- | ---: | --- | --- |
| 66 B | 32.998 Mpps / 17.423 Gb/s | 50,000,000 received, `RX-missed=0` | Small-frame, lossless baseline. |
| 98 B | 25.747 Mpps / 20.186 Gb/s | 50,000,000 received, `RX-missed=0` | First lossless maximum-frame run after peer tuning. |
| 98 B | **25.827 Mpps / 20.248 Gb/s** | **100,000,000 received, `RX-missed=0`** | Reproduced effective-limit result. |

The `32.000 Gb/s` reported by the kernel for Gen1 x16 is a physical PCIe link
rate, not application payload bandwidth.  Gen1's 8b/10b encoding and the PCIe
TLP/DLLP framing for many short NIC DMA writes reduce usable Ethernet payload
bandwidth substantially.  The 20.248 Gb/s 98-byte lossless run is therefore
the verified effective PCIe limit for the present hardware, frame format, and
driver's maximum 98-byte inline WQE.  It is not a claim that the 100 GbE wire
is saturated.

The peer settings are part of the result.  An earlier RX-only command used
`-l 1,2,3,4,5,6,7,8,9` and `--rxd=2048`; its main lcore and a worker shared a
physical core and the 98-byte run lost 12,971 of 50,000,000 frames.  Use CPU 0
as main lcore, CPUs 1--8 as workers, and 8,192 RX descriptors instead:

```sh
ssh sdn-svr5 "setsid -f sh -c 'sudo dpdk-testpmd \
  -l 0,1,2,3,4,5,6,7,8 -n 4 -a 0000:01:00.1 -- \
  --nb-cores=8 --rxq=8 --txq=8 --rss-ip --rss-udp \
  --forward-mode=rxonly --burst=128 --rxd=8192 --txd=2048 \
  --stats-period=1 --auto-start' \
  >/tmp/testpmd-mlxnicd.log 2>&1"

ssh sdn-svr6 'cd ~/work/takagi/nicd && sudo ./mlxnicd raw-flood \
  --bdf 0000:01:00.0 --peer-if eth2 \
  --src-mac 02:00:00:00:00:06 --dst-mac ff:ff:ff:ff:ff:ff \
  --ethertype 0x0800 \
  --payload-hex aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa \
  --rss-udp --queues 8 --count 100000000'
```

### Final validation log

The final investigation proceeded as follows.

1. The original `raw-bench` rate of 16.1 Mpps was separated from a one-way
   limit by adding `raw-flood`.  With 66-byte frames, this reached 33.0 Mpps
   (17.42 Gb/s) losslessly, showing that request/reply accounting had hidden
   much of the available one-way capacity.
2. Increasing the SQ from 2,048 to 8,192 WQEBBs and moving TX reclamation to
   explicit producer/consumer tracking made sustained flood traffic safe.  It
   did not by itself move the 33 Mpps small-frame plateau, which is expected:
   the receiver's PCIe DMA capacity rather than an unsafe SQ limit was then
   dominant.
3. At the driver's maximum 98-byte inline frame, the first RX-only peer
   configuration achieved 20.084 Gb/s but recorded 12,971 `RX-missed` packets
   out of 50M.  The NIC reported no physical discards and DPDK reported no
   mbuf allocation failures, isolating the loss to the software RX descriptor
   ring/lcore scheduling.
4. Moving `testpmd`'s main lcore to CPU 0, keeping its eight workers on CPUs
   1--8, and increasing `--rxd` from 2048 to 8192 removed that loss.  The
   configuration then reproduced 20.186 Gb/s for 50M packets and 20.248 Gb/s
   for 100M packets, both with `RX-missed=0`.

The implementation and this result were committed on the `benchmark` branch
as `0caeda0` (`Benchmark PCIe effective throughput`).  `references/` remains
local research material and is intentionally not part of that commit.

## 100 GbE line-rate campaign with `sdn-svr7` (2026-07-15)

`sdn-svr5` became unavailable because of an apparent board failure.  The cable
from the custom-driver port on `sdn-svr6` (`0000:01:00.0`) was moved to
`sdn-svr7:0000:01:00.1` (`eth2`, MAC `ec:0d:9a:44:2d:15`).  Both ports reported
100 GbE link-up; the peer's active PCIe link is **Gen4 x16 (16.0 GT/s x16)**.
This removes the old peer's Gen1 x16 DMA limit from the line-rate experiment.
The peer uses DPDK 25.11 `testpmd` in RX-only mode:

```sh
ssh sdn-svr7 "setsid -f sh -c 'sudo dpdk-testpmd --file-prefix=svr7flood \\
  -l 0,1,2,3,4,5,6,7,8 -n 4 -a 0000:01:00.1 -- \\
  --nb-cores=8 --rxq=8 --txq=8 --rss-ip --rss-udp \\
  --forward-mode=rxonly --burst=128 --rxd=8192 --txd=2048 \\
  --mbuf-size=8192 --max-pkt-len=4096 \\
  --stats-period=1 --auto-start' >/tmp/testpmd-svr7-flood.log 2>&1"
```

### Effective: Gen4 peer raises the 98-byte baseline

With the existing full-inline TX WQE (maximum 98-byte Ethernet frame), eight
source queues sent 100 million packets without a peer RX miss:

```text
raw-flood frame_len=98: 41.575 Mpps / 32.595 Gb/s
sdn-svr7 testpmd: RX-packets=100000000, RX-missed=0
```

This is a useful control result: the previous 20.248 Gb/s ceiling was not a
100 GbE-wire or source-CPU ceiling.  It was strongly constrained by the old
Gen1 peer's PCIe receive path.  A 98-byte frame cannot itself reach 100 GbE
payload throughput at 41.575 Mpps, so large-frame DMA TX is the required next
step.

### Path to DMA-backed jumbo SEND

`raw-flood --frame-len N` now accepts frames up to 4092 bytes.  Frames at most
98 bytes keep the established all-inline WQE.  Larger frames use a per-SQ DMA
packet ring with 4096-byte slots, inline the mandatory 14-byte L2 header, and
describe the remaining bytes in a data segment starting at a 16-byte-aligned DMA
offset.  The MKey must permit both local write (RX) and local read (TX): the old
value `0x18` enabled `rr|lw`, not `lr|lw`.  Changing it to `0x0c` fixed the
local-read permission.

The following attempts and outcomes must be retained when continuing this
work:

| Attempt | Result | Conclusion |
| --- | --- | --- |
| First data-segment WQE | CQE error `syndrome=0x53 vendor=0x04` | Local-protection failure; RX-only MKey permissions were insufficient. |
| Set MKey start/length to full 64-bit range | `CREATE_MKEY` firmware status 3 | Invalid encoding for this PA-MKey form; reverted. |
| Move TX IOVA from `0x30000000` to `0x0a000000` | Same local-protection CQE | IOVA placement was not the cause. |
| Enable `lr|lw` (`mkc[0x02] = 0x0c`) | Frames are accepted/transmitted; no protection CQE | Correctly enables device DMA reads of TX buffers. |
| Increase DMA ring from 4096 to 8192 slots | Still times out after source accepts 44,032--45,056 packets | Buffer reuse is not the remaining fault. |
| Reclaim every DMA API burst | 100,000 packets complete, but 34.472 Gb/s (1 queue) | Confirms WQE correctness, but synchronous completion waits are too expensive. |
| Account for real WQEBBs (DMA=1, inline=2) | 100,000 packets complete at 59.238 Gb/s (1 queue) | Fixes delayed-reclamation timeout and eliminates unnecessary SQ splitting. |
| Prebuild 4,096 RSS-diverse frames per worker | 70.599 Gb/s (8 queues, 10M packets) | Removes timed packet-template copy/checksum work. |
| Preload DMA packet rings and reuse them | **75.716 Gb/s** (8 queues, 10M packets) | Removes the driver-side 1514-byte copy; then-current best sustained result. |
| Pin queue workers to CPUs 0--7 | 75.934 Gb/s (8 queues, 10M packets) | +0.3%, within run-to-run noise; CPU migration is not material. |
| Re-test after moving the peer cable to `sdn-svr7` | 75.969 Gb/s (8 queues, 5M packets) | Confirms the `sdn-svr7` path is valid and consistent with the previous best. |
| 4096-byte jumbo WQE, 8 queues | 98.992 Gb/s reported by source | **Not a valid line-rate result:** peer RX stayed 0. |
| DPDK jumbo peer with 4096-byte mbufs | Port configuration fails | RX buffer needs room for 4092-byte packet plus 128-byte headroom. |
| DPDK jumbo peer with 8192-byte mbufs | Starts successfully | Required peer configuration. |
| Set custom-driver vport MTU to 4096 | Source reports 95.616 Gb/s; peer RX still 0 | Vport MTU alone did not make jumbo frames reach the wire. |
| Set physical port PMTU (`ACCESS_REG PMTU=0x5003`) to 4096 | 1522B/2048B/4088B/4092B reach `sdn-svr7` | Effective for jumbo egress. Vport MTU alone was insufficient. |
| Try 4096-byte Ethernet frame with peer `max-pkt-len=4096` | Source completes; peer RX stays unchanged | Ineffective/invalid. The peer accepts 4092-byte frames, which become 4096 bytes with FCS. |
| 4092-byte frame, 8 queues, 5M packets | 2.233 Mpps / 73.114 Gb/s, peer receives all 5M with no discard | Valid jumbo result, but below line rate. |
| 4092-byte queue scaling: 1/2/4/8 queues | 71.421 / 73.081 / 72.862 / 72.699 Gb/s | Ineffective beyond 2 queues. Queue count and CPU parallelism are not the present limiter. |
| Move DMA payload from `tx_iova+14` to 16-byte-aligned `tx_iova+16` | Invalid source-only 99.944 Gb/s when PMTU was accidentally lowered to 4092; valid PMTU4096 rerun stayed 73.114 Gb/s | Ineffective. Peer counters are required; TX CQ completion alone can overstate throughput. |
| Increase raw-flood burst from 4096 to 8192 packets | 4092B: 1 queue 68.045 Gb/s, 8 queues 72.238 Gb/s | Ineffective. Doorbell/CQE frequency is not the dominant limiter. Reverted to 4096. |
| Try DPDK `testpmd txonly` on `sdn-svr6:0000:01:00.0` with default mlx5 PMD settings | Port fails to start: `Failed to register unique umem for all SQs` / `Tx queues memory allocation failed` | Ineffective. The default PMD TxQ memory alignment is incompatible with this host/driver combination. |
| Force DPDK IOVA mode to PA | Same TX queue allocation failure | Ineffective for the DPDK baseline problem. |
| Add mlx5 devarg `txq_mem_algn=0` | DPDK `txonly` starts. 1514B run reports about 98.4 Gb/s. | Effective. This disables/relaxes the problematic TxQ umem alignment path. |
| DPDK `txonly`, `set txpkts 4092`, `txq_mem_algn=0` | Peer receives 16,936,864 packets / 69,373,394,944 bytes during the 6s command window; about 92.5 Gb/s by peer counters | Effective baseline. Same source port and peer path can exceed this driver's 73 Gb/s jumbo result. |
| Increase source endpoint PCIe MaxReadReq from 512B to 4096B (`setpci CAP_EXP+8.w 0x293f -> 0x593f`) | 4092B valid run: 73.241 Gb/s, peer receives all 5M | Ineffective; DMA read request size is not the visible limiter. Reverted to 512B (`0x293f`). |
| Inline first 64 bytes of jumbo frame, DMA-read the rest | 4092B valid run: 73.067 Gb/s, peer receives all 5M | Ineffective. Reducing host-memory DMA read by 50 bytes per packet does not move the 73 Gb/s ceiling. Reverted. |
| Split jumbo payload into two DMA data segments | 4092B valid run: 73.075 Gb/s, peer receives all 5M | Ineffective. HCA DMA-read scheduling does not improve from two data segments. Reverted. |
| Re-run on `sdn-svr7` after remote rebuild, unicast to peer MAC, IPv4/RSS, 4092B | 5M: 98.653 Gb/s; 50M: **99.357 Gb/s**, peer receives all packets, discard 0 | Effective. This is the current valid custom-driver line-rate result. |

2026-07-15 re-check after physically moving the cable to `sdn-svr7`: the peer
host still reports `eth2` as `0000:01:00.1`, link `100000Mb/s`, and PCIe
`16.0 GT/s x16`.  A 5M-packet 1514-byte run completed at **6.272 Mpps /
75.969 Gb/s**:

```text
raw-flood: ok tx=5000000
raw-flood: tx rate 6.272 Mpps 75.969 Gbps
sdn-svr7 eth2 counters: rx_packets_phy +5,000,000,
                         rx_bytes_phy +7,590,000,000,
                         rx_discards_phy +0
```

This validates the new peer path with hardware counters.  The received-byte
delta is 1,518 bytes per packet, matching the 1,514-byte Ethernet frame plus
FCS as counted by the NIC.

Short runs that remain below the completion-timeout boundary do complete:

| Source queues | Packets | Result | Rate |
| ---: | ---: | --- | ---: |
| 1 | 40,000 | complete | 4.436 Mpps / 53.734 Gb/s |
| 8 | 320,000 (40,000 per queue) | complete | 5.094 Mpps / 61.696 Gb/s |

At that stage, the initial eight-worker result improved the large-frame rate
only 14.8%.  `sdn-svr6` has eight distinct online CPU cores (Intel i9-12900KS),
and its source NIC is also at **Gen4 x16 (16.0 GT/s x16)**.  Removing packet
construction and both timed full-frame copies raised the sustained rate from
64.170 to 75.934 Gb/s, so memory/CPU work was material.  The later 4092-byte
line-rate result shows that this was not a fundamental cable, peer PCIe, or
source PCIe limit.

### Jumbo-frame investigation

The driver now constructs jumbo DMA-backed WQEs, makes them reach the wire, and
has a validated line-rate run under the reproduction conditions below.  A
source-only 4096-byte attempt once reported 98.992 Gb/s, but that older result
is **not accepted as a line-rate result**:
after resetting the `sdn-svr7` jumbo `testpmd` sink, its RX packet counter
remained zero.  A successful TX CQE means the HCA consumed the WQE, not that
the packet was emitted on the wire.

The missing setting was the physical port MTU register, not only the NIC vport
MTU.  Linux mlx5 uses `ACCESS_REG` with `MLX5_REG_PMTU = 0x5003`; the register
contains `local_port`, `max_mtu`, `admin_mtu`, and `oper_mtu`.  Setting
`admin_mtu` to 4096 and then setting the vport MTU to 4096 makes jumbo frames
up to 4092 bytes reach `sdn-svr7`.  With peer `testpmd --max-pkt-len=4096`,
a 4092-byte Ethernet frame is the practical maximum because the peer's physical
byte counter includes the 4-byte FCS.

Earlier valid jumbo run:

```sh
ssh sdn-svr6 'cd ~/work/takagi/nicd && sudo ./mlxnicd raw-flood \
  --bdf 0000:01:00.0 --peer-if eth2 \
  --src-mac 02:00:00:00:00:06 --dst-mac ff:ff:ff:ff:ff:ff \
  --ethertype 0x0800 --rss-udp --frame-len 4092 --queues 8 --count 5000000'
# raw-flood: tx rate 2.233 Mpps 73.114 Gbps
# sdn-svr7 eth2 counters: rx_packets_phy +5,000,000,
#                          rx_bytes_phy +20,480,000,000,
#                          rx_discards_phy +0
```

Current valid jumbo line-rate run:

```sh
# If the tree was synced from a Nix-built local environment, rebuild on sdn-svr6.
# Otherwise sudo may fail with "unable to execute ./mlxnicd: No such file or
# directory" because the binary's ELF interpreter points into /nix/store.
ssh -F /home/sora/.ssh/config sdn-svr6 'cd ~/work/takagi/nicd && make clean && make'

ssh -F /home/sora/.ssh/config sdn-svr6 'cd ~/work/takagi/nicd && \
  sudo ./mlxnicd raw-flood --bdf 0000:01:00.0 --peer-if eth2 \
  --src-mac 02:00:00:00:00:01 --dst-mac ec:0d:9a:44:2d:15 \
  --ethertype 0x0800 --rss-udp --frame-len 4092 --queues 8 --count 50000000'
# raw-flood: tx rate 3.035 Mpps 99.357 Gbps
# sdn-svr7 eth2 counters: rx_packets_phy +50,000,000,
#                          rx_bytes_phy +204,800,000,000,
#                          rx_discards_phy +0
```

This is the current accepted line-rate result for the custom driver.  It uses
4092-byte Ethernet frames, which the peer counts as 4096 physical bytes per
packet including FCS.  The source reports 99.357 Gb/s and the peer hardware
counter confirms all 50 million packets with no discard.

The earlier 73 Gb/s jumbo results should be retained as cautionary data, but
they are no longer the current best.  The known effective reproduction
conditions are: `sdn-svr7` RX-only DPDK peer, peer unicast destination MAC
`ec:0d:9a:44:2d:15`, IPv4 EtherType `0x0800`, `--rss-udp`, source PMTU/vport MTU
4096, and a binary rebuilt on `sdn-svr6`.

The working DPDK baseline sequence is:

```sh
# Source: temporarily restore the source function to mlx5_core.
ssh -F /home/sora/.ssh/config sdn-svr6 'cd ~/work/takagi/nicd && \
  sudo ./mlxnicd vfio-restore 0000:01:00.0 && \
  sudo ip link set eth1 mtu 4074 up'

# Source: run txonly and set the generated packet length interactively.
ssh -F /home/sora/.ssh/config sdn-svr6 'bash -lc '"'"'{
  printf "set txpkts 4092\nstart\n";
  sleep 6;
  printf "show port stats all\nstop\nquit\n";
} | sudo dpdk-testpmd --file-prefix=tx4092cmd -l 0,1 -n 4 \
  -a 0000:01:00.0,txq_mem_algn=0 -- --nb-cores=1 --txq=1 --rxq=1 \
  --forward-mode=txonly --mbuf-size=8192 --max-pkt-len=4096 \
  --stats-period=1 -i'"'"''

# Restore source for mlxnicd afterward.
ssh -F /home/sora/.ssh/config sdn-svr6 'cd ~/work/takagi/nicd && \
  sudo ./mlxnicd vfio-bind 0000:01:00.0'
```

## Next work

The original 10 Mpps target and the later 100 GbE line-rate TX target are both
met under the documented reproduction conditions.  Follow-on work should retain
the 50M-packet 4092-byte run as a regression test, add latency measurement that
does not perturb the throughput path, and extend the same level of validation
to RX and custom-driver-to-custom-driver operation when a second VFIO-capable
host is available.
