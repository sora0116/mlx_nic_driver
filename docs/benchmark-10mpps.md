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

### Temporary peer limitation

`sdn-svr5` presently cannot bind its NIC to VFIO because no IOMMU groups are
exported, even though the kernel command line includes `intel_iommu=on
iommu=pt`. DPDK is a temporary peer only. Its one-core configuration reaches
the current source rate; the four-core RSS `testpmd` experiment did not improve
it and is recorded above rather than treated as a solution.

## Current commands

Start the four-core RSS peer:

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
  --count 1000000 --window 1024 --timeout-ms 30000'
```

## Next work

1. Generalise the driver runtime from one SQ/RQ/CQ to multiple queue pairs.
2. Expose queue-specific burst APIs and give each queue to one polling worker.
3. Populate the RQT with multiple RQ numbers and verify hardware RSS across
   the UDP flows.
4. Compare 1, 2, and 4 worker throughput against the same peer and record all
   results here.
