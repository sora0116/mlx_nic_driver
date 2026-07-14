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

## Next work

1. Remove global atomic contention from the four source workers by assigning
   each worker a private sequence range and a private completion counter.
2. Measure the resulting 1, 2, and 4 worker rates against the unchanged peer.
3. Compare the repository's `peer/dpdk-macswap-peer` (a dedicated DPDK
   MAC-swap loop) with `testpmd` fairly before attributing any remaining limit
   to the peer.
4. Record every result here, including regressions.
