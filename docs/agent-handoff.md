# Agent handoff: mlxnicd raw packet driver

This document is the operational handoff for the next agent/session. It records
the current repo state, host state, design direction, and exact commands to run.

## Goal and direction

Final goal: implement a minimal userspace Mellanox/NVIDIA ConnectX-5 Ex driver
that can send and receive raw Ethernet frames.

Chosen approach:

- C userspace program, no DPDK, no AF_PACKET fallback for datapath validation.
- Use IOMMU + VFIO for PCI ownership, BAR mmap, and DMA mapping.
- Implement the mlx5 command path directly, then create the minimum CQ/SQ/RQ
  objects needed for raw packet TX/RX.
- Keep one port/function under Linux `mlx5_core` as the peer while one
  port/function is controlled by `mlxnicd`.

Target datapath:

```text
mlxnicd -> VFIO -> ConnectX-5 Ex BAR0 + DMA -> mlx5 SQ/RQ/CQ -> wire
```

## Current repository state

Working directory:

```text
/home/sora/work/mlxnicd
```

Remote working directory:

```text
sdn-svr6:~/work/takagi/nicd
```

Implemented CLI:

```sh
./mlxnicd list
./mlxnicd inspect <BDF>
./mlxnicd bar-read <BDF> <bar_index> <offset> [8|16|32|64]
./mlxnicd vfio-check [BDF]
./mlxnicd vfio-bind <BDF>
./mlxnicd vfio-restore <BDF>
./mlxnicd vfio-probe <BDF>
./mlxnicd mlx5-info <BDF>
./mlxnicd mlx5-query-issi <BDF>
./mlxnicd mlx5-set-issi <BDF>
./mlxnicd mlx5-enable-hca <BDF>
./mlxnicd mlx5-query-pages <BDF> <boot|init>
./mlxnicd mlx5-seq-basic <BDF>
./mlxnicd mlx5-tx-test <BDF> [--count N] [--dst MAC] [--src MAC] [--ethertype HEX] [--payload-hex HEX]
./mlxnicd mlx5-rx-objects <BDF>
./mlxnicd mlx5-rx-post-test <BDF>
./mlxnicd mlx5-rx-steer-test <BDF>
./mlxnicd mlx5-rx-wait-test <BDF>
./mlxnicd raw-loop --bdf <BDF> --peer-if <ifname> \
  --src-mac <mac> --dst-mac <mac> --ethertype <hex> \
  [--payload-hex HEX] [--rx-count N] [--pre-rx-delay-ms N] [--timeout-ms N] [--verbose]
```

Important files:

- `src/pci.c`: sysfs PCI discovery, config/resource inspection, BAR read.
- `src/vfio.c`: IOMMU/VFIO readiness check, sysfs bind/restore, VFIO
  container/group/device open, BAR0 mmap, DMA map/unmap smoke test.
- `src/raw.c`: raw-loop CLI validation and dispatch into mlx5 datapath.
- `src/mlx5.c`: mlx5 VFIO command interface, HCA init, TX/RX object setup,
  full-inline TX, RX poll/replenish, and raw-loop backend.
- `docs/raw-v1-design.md`: next-stage mlx5 bring-up design.

`raw-loop` is now an end-to-end mlx5/VFIO datapath check. It creates TX and RX
objects, posts a full-inline TX frame, optionally suppresses detailed logging,
skips the locally looped-back TX frame on RX, and counts peer packets on the
self-managed RX ring.

Internal implementation state:

- runtime state is grouped in `struct mlx5_test_runtime`
- execution mode is grouped in `struct mlx5_run_profile`
- RX filtering is grouped in `struct mlx5_rx_filter`
- the internal orchestration entry point is `mlx5_run_profile()`
- reusable helpers now exist for:
  - TX posting: `mlx5_test_runtime_post_tx()`
  - RX wait: `mlx5_test_runtime_wait_rx()`
  - teardown: `mlx5_test_runtime_cleanup()`
- RX wait stats are collected in `struct mlx5_rx_wait_stats`

`README.md` now contains the human-oriented code-level explanation of the
implementation, including function roles, argument meaning, return values, and
how the runtime/profile/filter pieces fit together.

## Current sdn-svr6 state

Verified after reboot:

```text
kernel cmdline includes: intel_iommu=on iommu=pt
IOMMU groups count: 18
/dev/vfio/vfio: present
vfio-pci: present after sudo modprobe vfio-pci
```

Initial post-reboot NIC state:

```text
0000:01:00.0  ConnectX-5 Ex  15b3:1019  mlx5_core  eth1  iommu_group=15
0000:01:00.1  ConnectX-5 Ex  15b3:1019  mlx5_core  eth2  iommu_group=16
```

Current development NIC state after the VFIO probe milestone:

```text
0000:01:00.0  ConnectX-5 Ex  15b3:1019  vfio-pci   no netdev  iommu_group=15
0000:01:00.1  ConnectX-5 Ex  15b3:1019  mlx5_core  eth2       iommu_group=16
```

Known BAR info:

```text
0000:01:00.0 BAR0 len=0x02000000
BAR0[0x0] u32 = 0x10002300  # read with sudo
```

VFIO probe milestone result:

```text
vfio-probe ok
  bdf: 0000:01:00.0
  group: 15
  device regions: 9 irqs: 5
  iommu page sizes bitmap: 0x40201000
  BAR0 size: 0x2000000 offset: 0x0 flags: 0xf
  BAR0[0x0] u32: 0x10002300
  DMA mapped: vaddr=<process address> iova=0x100000000 size=0x1000
```

mlx5 init segment milestone:

```text
mlx5-info ok
  fw_rev raw: 0x00230010
  cmdif_rev_fw_sub raw: 0x00050fbe
  cmdif_rev: 5
  fw_sub: 4030
  log_cmdq_size: 5
  log_cmdq_stride: 6
  nic_interface: 1
  initializing bit31: 0
```

mlx5 command-interface milestone:

```text
mlx5-query-issi debug
  pci command: 0x0006
  cmdq original: h=0x00000001 l=0x0baca156
  cmdq programmed: h=0x00000000 l=0x01000056
  desc xor before doorbell: 0xff
  desc status_own before doorbell: 0x01
mlx5-query-issi ok
  status_own: 0x00
  delivery_status: 0
  out.status: 0
  out.syndrome: 0x00000000
  out.current_issi: 0
  out.supported_issi_dw0: 0x00000002
```

Fixes/requirements discovered in that path:

- If repeated attempts leave the device in a bad state, restore to `mlx5_core`
  once, let it probe successfully, then bind back to `vfio-pci`.
- VFIO enables PCI bus master; config command becomes `0x0006`.
- MMIO writes use 32-bit big-endian transactions, not byte stores.
- Command descriptor offsets match `struct mlx5_cmd_layout`.
- `status_own` polling uses volatile reads.
- `cmdq_addr_l_sz` write is reflected in BAR0 readback.

ISSI result:

- `supported_issi_dw0 = 0x2`, so ISSI 1 is supported.
- Next command-interface step is `SET_ISSI current_issi=1`, then
  `QUERY_HCA_CAP`.

Primary kernel references on `sdn-svr6`:

```text
/usr/src/kernels/$(uname -r)/include/linux/mlx5/mlx5_ifc.h
/usr/src/kernels/$(uname -r)/include/linux/mlx5/device.h
/usr/src/kernels/$(uname -r)/include/linux/mlx5/doorbell.h
/usr/src/kernels/$(uname -r)/drivers/net/ethernet/mellanox/mlx5/core
```

## Commands to re-establish state

From local workspace:

```sh
cd /home/sora/work/mlxnicd
make clean && make
make sync
```

On remote:

```sh
ssh sdn-svr6 'cd ~/work/takagi/nicd && make && ./mlxnicd list'
ssh sdn-svr6 'sudo modprobe vfio-pci && cd ~/work/takagi/nicd && ./mlxnicd vfio-check 0000:01:00.0'
ssh sdn-svr6 'cd ~/work/takagi/nicd && sudo ./mlxnicd mlx5-info 0000:01:00.0'
ssh sdn-svr6 'cd ~/work/takagi/nicd && sudo ./mlxnicd mlx5-query-issi 0000:01:00.0'
```

Expected `vfio-check` success while bound to `vfio-pci`:

```text
iommu_groups: present
vfio-pci driver: present
vfio device node: present
bdf: 0000:01:00.0
vendor: 0x15b3
device: 0x1019
driver: vfio-pci
iommu_group: 15
status: ready for vfio bind checks
```

## Safe next execution steps

Use `0000:01:00.0` as the first VFIO-controlled function and keep
`0000:01:00.1` / `eth2` under Linux as the peer.

Before binding, confirm SSH is not using `eth1`:

```sh
ssh sdn-svr6 'ip route get "$(who am i | awk '"'"'{print $5}'"'"' | tr -d '"'"'()'"'"')" 2>/dev/null || true'
```

Bind the target to `vfio-pci` if it is not already bound:

```sh
ssh sdn-svr6 'sudo modprobe vfio-pci'
ssh sdn-svr6 'cd ~/work/takagi/nicd && sudo ./mlxnicd vfio-bind 0000:01:00.0'
ssh sdn-svr6 'cd ~/work/takagi/nicd && sudo ./mlxnicd vfio-check 0000:01:00.0'
ssh sdn-svr6 'cd ~/work/takagi/nicd && sudo ./mlxnicd vfio-probe 0000:01:00.0'
```

Restore if needed:

```sh
ssh sdn-svr6 'cd ~/work/takagi/nicd && sudo ./mlxnicd vfio-restore 0000:01:00.0'
```

Current `raw-loop` command:

```sh
ssh sdn-svr6 'cd ~/work/takagi/nicd && sudo ./mlxnicd raw-loop \
  --bdf 0000:01:00.0 \
  --peer-if eth2 \
  --src-mac 02:00:00:00:00:06 \
  --dst-mac ff:ff:ff:ff:ff:ff \
  --ethertype 0x88b5'
```

## Latest milestone: raw TX SEND WQE completion

Current date of this note: 2026-07-13.

`mlx5-seq-basic` now brings the VFIO-controlled function far enough to post a
raw Ethernet SEND WQE and receive a non-error CQE.

Run:

```sh
cd /home/sora/work/mlxnicd
make clean && make
make sync
ssh sdn-svr6 'cd ~/work/takagi/nicd && make clean && make && \
  sudo ./mlxnicd mlx5-seq-basic 0000:01:00.0; echo seq_rc=$?'
```

Expected current result:

```text
seq-query-sq-ready ok
  sq state: 1
  sq cqn: 1024
  sq tis_lst_sz: 1
  sq tis_num_0: 0
  post SEND WQE: sqn=130 tisn=0 inline_len=60 pi=0 new_pi=2 ...
seq-post-send CQE
  op_own: 0x00
  opcode: 0x00
seq_rc=0
```

Important fixes discovered during TX bring-up:

- `QUERY_SQ` output SQ context starts at byte `0x20`, not byte `0x100`.
- `sqc.tis_lst_sz` is at SQ context byte `0x20` (`mlx5_ifc_sqc_bits`
  bit offset `0x100`). A previous version incorrectly used byte `0x1e`,
  which only made the diagnostic appear correct because the query decoded the
  same wrong offset.
- `sqc.tis_num_0` is at byte `0x2c`.
- `CREATE_TIS` for Ethernet TX should match mlx5e: set `transport_domain`;
  do not set `pd` unless a later feature explicitly requires it.
- Current successful TX path uses a full-inline 60-byte Ethernet frame:
  `ds=6`, ctrl segment + Ethernet segment + inline payload continuation.
- A mixed L2-inline + PA mkey data segment reached a later error
  (`syndrome=0x04`, vendor `0x53`) after fixing `tis_lst_sz`; defer PA-mkey
  debugging because full-inline is enough for the smallest raw TX milestone.

Current next steps:

1. Verify the transmitted frame on the peer machine/port with tcpdump or a
   simple raw socket receiver. The test frame is broadcast destination,
   source `02:00:00:00:00:01`, ethertype `0x88b5`, payload prefix
   `mlxnicd-tx-0001`.
2. Replace the hard-coded test frame with a user-provided raw packet buffer.
3. Keep full-inline TX for the first usable raw TX API; revisit DMA data
   segments later for packets larger than the inline budget.
4. Start RX bring-up: create RQ, RQT/TIR or direct receive path as required,
   post receive WQEs, and poll CQ for received raw Ethernet frames.

## Latest milestone: wire-level TX verified on sdn-svr5

The peer machine connected to the VFIO-controlled port is `sdn-svr5`.

Observed interfaces on `sdn-svr5`:

```text
eth1  98:03:9b:99:88:32  172.16.0.5/24
eth2  98:03:9b:99:88:33  172.16.10.5/24
```

After running `mlx5-seq-basic` 10 times on `sdn-svr6`, only `sdn-svr5` `eth2`
RX counters increased:

```text
before eth2:
  RX packets: 26
  RX bytes:   1560
  rx_packets_phy: 26
  rx_bytes_phy:   1664
  rx_broadcast_phy: 26
  dropped: 3

after eth2:
  RX packets: 36
  RX bytes:   2160
  rx_packets_phy: 36
  rx_bytes_phy:   2304
  rx_broadcast_phy: 36
  dropped: 13
```

Delta:

```text
+10 RX packets
+600 RX bytes at netdev level
+640 RX bytes at PHY level
+10 broadcast packets
+10 dropped packets
```

Interpretation:

- The `mlxnicd` full-inline SEND WQE is not merely completing internally; frames
  are reaching the peer NIC over the cable.
- The connected peer port is `sdn-svr5:eth2`.
- The +10 `dropped` count is expected for now: the test frame is broadcast with
  experimental EtherType `0x88b5`, and no userspace raw receiver is attached on
  `sdn-svr5`.
- Tcpdump attempts with `ether proto 0x88b5` did not print packet bodies even
  though filter counters sometimes moved. Treat NIC counters as the current
  positive wire-level evidence; later use an explicit AF_PACKET receiver on
  `sdn-svr5` if packet contents need to be validated.

Reproduce:

```sh
ssh sdn-svr5 'ip -s link show eth2; ethtool -S eth2 | grep -E "rx_packets:|rx_bytes:|rx_packets_phy|rx_bytes_phy|rx_broadcast_phy|rx_discards_phy"'

ssh sdn-svr6 'cd ~/work/takagi/nicd && for i in $(seq 1 10); do \
  sudo ./mlxnicd mlx5-seq-basic 0000:01:00.0 >/tmp/mlxnicd_tx_$i.log || exit $?; \
done; echo sent_10'

ssh sdn-svr5 'ip -s link show eth2; ethtool -S eth2 | grep -E "rx_packets:|rx_bytes:|rx_packets_phy|rx_bytes_phy|rx_broadcast_phy|rx_discards_phy"'
```

Dedicated TX test command added:

```sh
ssh sdn-svr6 'cd ~/work/takagi/nicd && sudo ./mlxnicd mlx5-tx-test 0000:01:00.0 --count 3; echo tx_rc=$?'
```

Verified result:

```text
mlx5-tx-test: persistent command context
  post full-inline SEND WQE(s): count=3
  tx-test iteration: 1/3
seq-post-send CQE
  opcode: 0x00
  tx-test iteration: 2/3
seq-post-send CQE
  opcode: 0x00
  tx-test iteration: 3/3
seq-post-send CQE
  opcode: 0x00
tx_rc=0
```

Current next implementation steps:

1. Keep the current known-good full-inline path for packets up to the inline
   budget.
2. Reduce verbose command-path dumps for the TX command once debugging no longer
   needs them.
3. Then move to RX bring-up.

## Latest milestone: parameterized TX test command

`mlx5-tx-test` now accepts:

```text
--count N
--dst MAC
--src MAC
--ethertype HEX
--payload-hex HEX
```

Current constraints:

- full-inline only;
- Ethernet frame length is padded to 60 bytes;
- maximum frame length is 98 bytes for now, to keep each SEND WQE at exactly
  two 64-byte WQEBBs and avoid broader ring-wrap handling;
- maximum payload is 84 bytes.

Default command still sends the original broadcast test frame:

```sh
ssh sdn-svr6 'cd ~/work/takagi/nicd && sudo ./mlxnicd mlx5-tx-test 0000:01:00.0 --count 2; echo tx_rc=$?'
```

Verified custom unicast command:

```sh
ssh sdn-svr6 'cd ~/work/takagi/nicd && sudo ./mlxnicd mlx5-tx-test 0000:01:00.0 \
  --count 2 \
  --dst 98:03:9b:99:88:33 \
  --src 02:00:00:00:00:06 \
  --ethertype 0x88b6 \
  --payload-hex 0102030405060708090a; echo tx_rc=$?'
```

Verified result on `sdn-svr5:eth2`:

```text
before:
  RX packets: 41
  RX bytes:   2460

after:
  RX packets: 43
  RX bytes:   2580
  rx_vport_unicast_packets: 2
  rx_vport_unicast_bytes:   120
```

Interpretation: parameterized unicast TX reaches `sdn-svr5:eth2` over the wire.

`raw-loop` is no longer a preflight stub; it now exercises both full-inline TX
and RX poll/replenish in one command. `mlx5-tx-test` remains the simpler
TX-only entry point.

## Next coding stage

Done:

- VFIO container/group/device ioctl setup.
- VFIO region discovery and BAR0 mmap through VFIO.
- Page-aligned DMA allocation and VFIO DMA map/unmap smoke test.
- mlx5 initialization segment discovery.
- mlx5 command queue, mailbox chaining, HCA init, pages, PAOS query.
- UAR, PD, TD, TIS, MKEY, EQ, CQ, SQ creation/destruction.
- SQ RDY transition and `QUERY_SQ`.
- Full-inline SEND WQE completion.
- Wire-level TX verified on `sdn-svr5:eth2`.

Implement next in this order:

1. Add TX packet parameter options.
2. Add a quieter TX mode suitable for repeated testing.
3. Start RX bring-up: create receive objects, post RX buffers, poll CQE, print
   received frame bytes.
4. Wrap TX/RX into `raw-loop`. Done: initial verbose raw-loop now exists.

Do not validate packet I/O by adding an AF_PACKET fallback. It would test the
Linux network stack, not the self-managed mlx5/VFIO datapath.

## Latest milestone: RX object creation

Current date of this note: 2026-07-13.

`mlx5-rx-objects` now creates and destroys the minimum RX-side objects:

- UAR
- PD
- TD
- q_counter
- EQ
- CQ
- RQ
- direct TIR

Verified command:

```sh
cd /home/sora/work/mlxnicd
make clean && make
make sync
ssh sdn-svr6 'cd ~/work/takagi/nicd && make clean && make && \
  sudo ./mlxnicd mlx5-rx-objects 0000:01:00.0; echo rxobj_rc=$?'
```

Expected result:

```text
seq-create-rq ok
  out.status: 0
  rqn: <nonzero>
seq-modify-rq-rdy ok
  out.status: 0
seq-create-direct-tir ok
  out.status: 0
  tirn: <number>
rxobj_rc=0
```

Important implementation notes:

- For the command-queue path in this repo, `CREATE_RQ` uses the same effective
  context style as the already-working `CREATE_SQ`: RQC starts at input offset
  `0x20`, not the full Linux `create_rq_in` struct offset `0x100`.
- Correct RQ offsets currently used:
  - `CREATE_RQ_RQC_OFF = 0x20`
  - `RQC_WQ_OFF = 0x30`
  - `CREATE_RQ_PAS_OFF = 0x110`
  - `MODIFY_RQ_RQC_OFF = 0x20`
- `log_wq_sz` is a 5-bit mlx5 IFC field and must be encoded as `4 << 3`, not
  raw byte value `4`.
- A q_counter is allocated with `ALLOC_Q_COUNTER` and placed in
  `rqc.counter_set_id`.
- `mlx5-rx-objects` now avoids creating TX-only resources when doing RX object
  validation; it creates only the RX-side minimum.
- `CREATE_TIR` also uses effective TIRC offset `0x20` for this command path.

Next RX datapath steps:

1. Add RX DMA packet buffers and a receive mkey/lkey strategy.
2. Initialize cyclic RQ WQEs with data segments pointing at those buffers.
3. Ring the RQ doorbell / producer counter.
4. Add a steering object or rule that routes packets to the direct TIR/RQ.
5. Poll RX CQEs and dump received Ethernet frame bytes.
6. Verify by sending from `sdn-svr5:eth2` to the VFIO-controlled port.
7. Fold TX and RX into `raw-loop`.

## Latest milestone: RX WQE post

Current date of this note: 2026-07-13.

`mlx5-rx-post-test` was added and verified on `sdn-svr6`.

Run:

```sh
cd /home/sora/work/mlxnicd
make clean && make
make sync
ssh sdn-svr6 'cd ~/work/takagi/nicd && make clean && make && \
  sudo ./mlxnicd mlx5-rx-post-test 0000:01:00.0; echo rxpost_rc=$?'
```

Verified result:

```text
seq-create-rq ok
  out.status: 0
seq-modify-rq-rdy ok
  out.status: 0
seq-create-direct-tir ok
  out.status: 0
  tirn: 0
  post RX WQE: rqn=<rqn> pi=0 new_pi=1 rx_iova=0x7800000 len=2048 lkey=0x00004000
  rx wqe (16 bytes):
  0000: 80 00 08 00 00 00 40 00 00 00 00 00 07 80 00 00
rxpost_rc=0
```

Implementation details:

- A PA mkey is created for RX post as well as TX.
- One 4 KiB RX DMA page is mapped at IOVA `0x07800000`.
- The posted cyclic RQ WQE is one `mlx5_wqe_data_seg`:
  - `byte_count = MLX5_HW_START_PADDING | 2048`
  - `lkey = PA mkey`
  - `addr = 0x07800000`
- Later RX debugging changed current source to `byte_count = 2048` as an
  experiment. If returning to the original posted-WQE milestone, account for
  that difference.
- RQ producer counter is written to DBR receive slot at offset `0`.
- This milestone does not yet prove packet reception; no steering rule has been
  installed yet. It proves RX buffers can be posted to a ready RQ.

Next RX datapath step:

Create minimal flow steering so packets arriving on the port are delivered to
the direct TIR/RQ, then poll RX CQEs and dump the RX buffer bytes.

## Latest milestone: RX flow steering objects

Current date of this note: 2026-07-13.

`mlx5-rx-steer-test` now creates and destroys the minimal NIC RX steering chain:

- CREATE_FLOW_TABLE
- CREATE_FLOW_GROUP
- SET_FLOW_TABLE_ENTRY forwarding to direct TIR
- DELETE_FLOW_TABLE_ENTRY
- DESTROY_FLOW_GROUP
- DESTROY_FLOW_TABLE

Verified command:

```sh
cd /home/sora/work/mlxnicd
make clean && make
make sync
ssh sdn-svr6 'cd ~/work/takagi/nicd && make clean && make && \
  sudo ./mlxnicd mlx5-rx-steer-test 0000:01:00.0; echo rxsteer_rc=$?'
```

Expected result:

```text
seq-create-rx-flow-table ok
  rx flow table id: 0
seq-create-rx-flow-group ok
  rx flow group id: 0
seq-set-rx-fte-tir ok
  rx fte index: 0 -> tirn 0
rxsteer_rc=0
```

Important implementation notes:

- The previous `SET_FLOW_TABLE_ENTRY` failure
  `out.status=3 syndrome=0x00144b7a` was caused by `flow_context.action`
  encoding.
- `flow_context.action` starts at bit offset `0x70` and occupies the low
  16 bits of the dword at `flow_context + 0x0c`; encode it with
  `put_be32(fc + 0x0c, MLX5_FLOW_CONTEXT_ACTION_FWD_DEST)`, not `put_be16`.
- `MODIFY_NIC_VPORT_CONTEXT` was added for RX tests that install flow steering.
  It enables `promisc_uc`, `promisc_mc`, and `promisc_all`; command succeeds.
- `mlx5-rx-wait-test <BDF>` was added. It creates RX objects, posts all 16 RQ
  entries to 16 separate 2048-byte RX buffers, installs the steering rule, waits
  up to 30 seconds per CQE, decodes CQE `byte_cnt`/`wqe_counter`, dumps the
  received frame bytes from the matching buffer slot, and replenishes one WQE
  after each received CQE.

## RX status: one raw packet receive works

Current date of this note: 2026-07-13.

Current command:

```sh
ssh sdn-svr6 'cd ~/work/takagi/nicd && make clean && make && \
  sudo ./mlxnicd mlx5-rx-wait-test 0000:01:00.0; echo rxwait_rc=$?'
```

Peer transmit command that actually increments `sdn-svr5:eth2` TX counters:

```sh
ssh sdn-svr5 'sudo python3 -c '"'"'import socket,time
iface="eth2"; proto=0x88b5
dst=bytes.fromhex("ffffffffffff")
src=bytes.fromhex("020000000055")
payload=b"mlxnicd-rx-test"
pkt=dst+src+proto.to_bytes(2,"big")+payload+bytes(max(0,60-14-len(payload)))
s=socket.socket(socket.AF_PACKET,socket.SOCK_RAW,socket.htons(proto))
s.bind((iface,proto))
for i in range(10):
    s.send(pkt); time.sleep(0.1)
print("sent",len(pkt),"bytes on",iface)
'"'"''
```

Old observed result before the successful RQT/indirect-TIR/WQE fixes:

```text
seq-set-rx-fte-tir ok
  rx fte index: 0 -> tirn 0
  wait for RX CQE: timeout_ms=10000
seq-rx-wait timeout waiting for CQE
seq-rx-timeout CQ slots dump
  slot 0 op_own=0xff
  slot 1 op_own=0xff
  slot 2 op_own=0xff
  rx buffer after timeout:
  0000: cc cc cc cc ...
rxwait_rc=1
```

Successful RX result:

```sh
make clean && make
make sync
ssh sdn-svr6 'cd ~/work/takagi/nicd && make clean && make && sudo ./mlxnicd mlx5-rx-wait-test 0000:01:00.0; echo rxwait_rc=$?'

# While mlx5-rx-wait-test is waiting:
ssh sdn-svr5 'sudo python3 -c '"'"'import socket,time
iface="eth2"; proto=0x88b5
dst=bytes.fromhex("ffffffffffff")
src=bytes.fromhex("020000000055")
payload=b"mlxnicd-rx-test"
pkt=dst+src+proto.to_bytes(2,"big")+payload+bytes(max(0,60-14-len(payload)))
s=socket.socket(socket.AF_PACKET,socket.SOCK_RAW,socket.htons(proto))
s.bind((iface,proto))
time.sleep(8)
for i in range(20):
    s.send(pkt); time.sleep(0.1)
print("sent",len(pkt),"bytes on",iface)
'"'"''
```

Successful one-packet run highlights:

```text
seq-query-ppcnt-before-rx ok
  ppcnt rx_packets_phy: 0
  ppcnt rx_broadcast_phy: 0
  wait for RX CQE: timeout_ms=30000
seq-rx-wait CQE
  op_own: 0x20
  opcode: 0x02
  byte_cnt: 60
  wqe_counter: 0
  signature: 0x13
  rx frame length: 60
  rx frame:
  0000: ff ff ff ff ff ff 02 00 00 00 00 55 88 b5 6d 6c
  0010: 78 6e 69 63 64 2d 72 78 2d 74 65 73 74 00 00 00
seq-query-ppcnt-after-rx ok
  ppcnt rx_packets_phy: 1
  ppcnt rx_bytes_phy: 64
  ppcnt rx_broadcast_phy: 1
seq-query-rq-after-rx ok
  rq wq hw_counter: 1
  rq wq sw_counter: 1
rxwait_rc=0
```

Successful two-WQE/two-CQE run highlights:

```text
post RX WQE: ... pi=0 new_pi=1 wqe_slot=0 buf_slot=0 rx_iova=0x7800000
post RX WQE: ... pi=1 new_pi=2 wqe_slot=1 buf_slot=1 rx_iova=0x7800800
wait for RX CQE: packet=1/2 timeout_ms=30000
seq-rx-wait CQE
  byte_cnt: 60
  wqe_counter: 0
  rx frame length: 60
  rx buffer slot: 0
  rx frame:
  0000: ff ff ff ff ff ff 02 00 00 00 00 55 88 b5 6d 6c
  0010: 78 6e 69 63 64 2d 72 78 2d 74 65 73 74 2d 30 00
wait for RX CQE: packet=2/2 timeout_ms=30000
seq-rx-wait CQE
  byte_cnt: 60
  wqe_counter: 1
  rx frame length: 60
  rx buffer slot: 1
  rx frame:
  0000: ff ff ff ff ff ff 02 00 00 00 00 55 88 b5 6d 6c
  0010: 78 6e 69 63 64 2d 72 78 2d 74 65 73 74 2d 31 00
seq-query-ppcnt-after-rx ok
  ppcnt rx_packets_phy: 2
  ppcnt rx_bytes_phy: 128
  ppcnt rx_broadcast_phy: 2
seq-query-rq-after-rx ok
  rq wq hw_counter: 2
  rq wq sw_counter: 2
rxwait_rc=0
```

Successful 16-entry RQ + replenish run highlights:

```text
post RX WQE: ... pi=0  new_pi=1  wqe_slot=0  buf_slot=0  rx_iova=0x7800000
...
post RX WQE: ... pi=15 new_pi=16 wqe_slot=15 buf_slot=15 rx_iova=0x780f000

wait for RX CQE: packet=16/20 timeout_ms=30000
seq-rx-wait CQE
  cqe index: 15
  op_own: 0x20
  byte_cnt: 60
  wqe_counter: 15
  rx buffer slot: 15
  rx frame:
  0010: 78 6e 69 63 64 2d 72 78 2d 72 69 6e 67 2d 31 35
  rx replenish complete: prod_index=32

wait for RX CQE: packet=17/20 timeout_ms=30000
seq-rx-wait CQE
  cqe index: 16
  op_own: 0x21
  byte_cnt: 60
  wqe_counter: 16
  rx buffer slot: 0
  rx frame:
  0010: 78 6e 69 63 64 2d 72 78 2d 72 69 6e 67 2d 31 36
  rx replenish complete: prod_index=33

wait for RX CQE: packet=20/20 timeout_ms=30000
seq-rx-wait CQE
  cqe index: 19
  op_own: 0x21
  byte_cnt: 60
  wqe_counter: 19
  rx buffer slot: 3
  rx frame:
  0010: 78 6e 69 63 64 2d 72 78 2d 72 69 6e 67 2d 31 39
  rx replenish complete: prod_index=36

seq-query-ppcnt-after-rx ok
  ppcnt rx_packets_phy: 20
  ppcnt rx_bytes_phy: 1280
  ppcnt rx_broadcast_phy: 20
seq-query-rq-after-rx ok
  rq wq hw_counter: 20
rxwait_rc=0
```

Facts already checked:

- `sdn-svr6` PAOS reports `local_port=1 admin_status=1 oper_status=1`.
- `sdn-svr6` VFIO TX still completes successfully.
- `sdn-svr6 -> sdn-svr5` is physically on `sdn-svr5:eth2`; verified with
  `tcpdump -i any -e -n ether proto 0x88b5 -c 1` on `sdn-svr5`.
- A raw socket created without an explicit AF_PACKET protocol caused TX drops
  on `sdn-svr5:eth2`. Use `socket(AF_PACKET, SOCK_RAW, htons(0x88b5))` and
  `bind((iface, 0x88b5))`; this increments TX packets without increasing drops.
- Enabling NIC vport promisc succeeds but does not by itself produce RX CQEs.
- The current RQ post path writes the producer to both RQ DBR offset `0` and
  offset `4`. This worked for the successful RX test. Linux appears to require
  only offset `0`, so this can be simplified later after a regression test.
- RX WQE `byte_count` must include `MLX5_HW_START_PADDING`, i.e.
  `0x80000800` for a 2048-byte buffer. After steering was fixed, restoring
  this value produced a successful RX CQE.
- `QUERY_RQ` was added. It shows:
  - RQ state is RDY (`rq state: 1`)
  - `cqn=1024`
  - `counter_set_id=1`
  - WQ type is cyclic (`rq wq_type: 1`)
  - `pd=16`
  - DBR address is `0x07700000`
  - `log_stride=4`
  - `log_size=4`
  - `hw_counter=0` and `sw_counter=0` both after post and after timeout
- `QUERY_TIR` was added. For the working indirect TIR path it shows:
  - `disp_type=1`
  - `transport_domain=1`
- `log_wq_sz` encoding for RQ was corrected from `wq[0x23] = 4 << 3` to
  `wq[0x23] = 4`; `QUERY_RQ` confirms `log_size=4`.
- `SET_FLOW_TABLE_ROOT` is required after `CREATE_FLOW_TABLE` and succeeds.
- `flow_context.flow_source=UPLINK` is used in the FTE and succeeds.
- RQT + indirect TIR was added and verified:
  - `CREATE_RQT` works after fixing `rqtc` field offsets to match the kernel
    IFC layout:
    - `rqt_context` input offset: `0x20`
    - `rqt_max_size`: `rqtc+0x16`
    - `rqt_actual_size`: `rqtc+0x1a`
    - `rq_num[0]`: `rqtc+0xf0`
  - Current implementation creates 16 RQT entries, all pointing at the same RQ.
  - The extra probe RQT was removed. `rqtn=0` was retested and receives packets
    successfully; the previous post-cleanup timeout was a peer-send timing issue.
  - `CREATE_TIR` with `disp_type=INDIRECT` failed until
    `rx_hash_fn=INVERTED_XOR8` was set at `tirc[0x24]=0x10`.
  - With `rx_hash_fn=INVERTED_XOR8`, indirect TIR creation succeeds and
    `QUERY_TIR` reports `disp_type=1` and `transport_domain=1`.
  - With RQT + indirect TIR + root RX flow table + FTE to TIR, and RX WQE
    `byte_count=0x80000800`, RX succeeds.
  - `QUERY_RQT` currently prints `rq_num[0]: 0`; this is not blocking because
    the same RQT receives packets. Treat it as a query/decode visibility issue,
    not as evidence that the RQT programming failed.
- PPCNT query was added via `ACCESS_REG(PPCNT=0x5008)`, IEEE 802.3 group. It
  shows physical RX counters increasing when the test frame is received:
  `rx_packets_phy 0 -> 1`, `rx_broadcast_phy 0 -> 1`.
- RX CQE decode now prints `byte_cnt`, `wqe_counter`, and CQE signature on
  successful CQEs. Error CQEs still print `syndrome` and `vendor syndrome`.
- RX wait now uses `wqe_counter & 15` to select one of 16 RX buffers. After each
  CQE it calls the post path once to replenish the cyclic RQ slot. This proves
  the minimal RX ring wrap/replenish path, but the API is still test-oriented.
- RX poll/replenish is now split internally:
  - `struct mlx5_rx_packet` carries `data`, `len`, `slot`, and `wqe_counter`.
  - `mlx5_rx_poll_one()` polls one CQE and returns the matching RX buffer.
  - `mlx5_rx_replenish_one()` reposts one WQE after the caller is done with the
    packet buffer.
  - `mlx5_wait_for_rx_packets()` is now mostly a test wrapper around those two
    helpers and still performs frame dumps.

Most likely next implementation targets:

1. Clean up experimental instrumentation:
   - reduce query dumps or gate them behind verbose mode
   - consider lowering RX wait timeout after tests are stable, but keep enough
     slack when peer send is started manually from `sdn-svr5`
2. Extract RX setup/teardown into a reusable helper bundle. Poll/replenish is
   already split; setup still lives inside `mlx5_run_tx_test()`.
3. Combine TX and RX into the first raw packet send/receive command, likely by
   replacing the current `raw-loop` stub with a thin wrapper around the mlx5
   setup, full-inline TX, `mlx5_rx_poll_one()`, and replenish helpers.
4. Cleanup flow table/RQT/TIR setup into smaller, less experimental helpers.

## Latest milestone: raw-loop end-to-end TX/RX verified with peer traffic

Current date of this note: 2026-07-13.

`raw-loop` now works as an end-to-end raw packet test against
`sdn-svr5:eth2`. It currently:

- validates CLI options in `src/raw.c`;
- builds a full-inline TX frame from `--dst-mac`, `--src-mac`, and
  `--ethertype`;
- accepts optional `--payload-hex`, `--rx-count`, `--pre-rx-delay-ms`, and
  `--timeout-ms`;
- accepts `--verbose`; without it, `raw-loop` now prints a short summary rather
  than the full mlx5/VFIO trace;
- uses payload `mlxnicd-raw-loop` when `--payload-hex` is omitted;
- creates both SQ/TIS and RX RQ/RQT/TIR/flow steering;
- posts TX before RX wait when both TX and RX are enabled;
- skips the locally transmitted TX frame on RX by matching the meaningful TX
  prefix, so peer traffic can be counted cleanly;
- polls RX CQEs through `mlx5_rx_poll_one()`;
- replenishes each consumed RX WQE through `mlx5_rx_replenish_one()`;
- tears all objects down.

Verified command:

```sh
ssh sdn-svr6 'cd ~/work/takagi/nicd && make clean >/dev/null && make >/dev/null && \
  sudo ./mlxnicd raw-loop \
    --bdf 0000:01:00.0 \
    --peer-if eth2 \
    --src-mac 02:00:00:00:00:06 \
    --dst-mac ff:ff:ff:ff:ff:ff \
    --ethertype 0x88b5; echo rawloop_rc=$?'
```

Peer traffic used during the RX wait:

```sh
ssh sdn-svr5 'sudo python3 -c '"'"'import socket,time
iface="eth2"; proto=0x88b5
dst=bytes.fromhex("ffffffffffff")
src=bytes.fromhex("020000000055")
base=b"mlxnicd-raw-loop-rx-"
s=socket.socket(socket.AF_PACKET,socket.SOCK_RAW,socket.htons(proto))
s.bind((iface,proto))
time.sleep(5)
for i in range(30):
    payload=base+("%02d"%i).encode()
    pkt=dst+src+proto.to_bytes(2,"big")+payload+bytes(max(0,60-14-len(payload)))
    s.send(pkt); time.sleep(0.05)
print("sent",30,"frames on",iface)
'"'"''
```

Verified result:

```text
raw-loop: TX dst=ff:ff:ff:ff:ff:ff src=02:00:00:00:00:06 ethertype=0x88b5
raw-loop: persistent command context
  tx frame length: 60
seq-post-send CQE
  opcode: 0x00
...
wait for RX CQE: packet=20/20 timeout_ms=30000
seq-rx-wait CQE
  byte_cnt: 60
  wqe_counter: 19
  rx buffer slot: 3
  rx frame:
  0010: 78 6e 69 63 64 2d 72 61 77 2d 6c 6f 6f 70 2d 72
seq-query-rq-after-rx ok
  rq wq hw_counter: 20
  rq wq sw_counter: 30
rawloop_rc=0
```

Peer RX-only check:

```sh
ssh sdn-svr6 'cd ~/work/takagi/nicd && sudo ./mlxnicd mlx5-rx-wait-test 0000:01:00.0'
```

Peer traffic used during the RX-only wait:

```sh
ssh sdn-svr5 'sudo python3 -c '"'"'import socket,time
iface="eth2"; proto=0x88b5
dst=bytes.fromhex("ffffffffffff")
src=bytes.fromhex("020000000055")
base=b"mlxnicd-rxonly-"
s=socket.socket(socket.AF_PACKET,socket.SOCK_RAW,socket.htons(proto))
s.bind((iface,proto))
time.sleep(5)
for i in range(20):
    payload=base+("%02d"%i).encode()
    pkt=dst+src+proto.to_bytes(2,"big")+payload+bytes(max(0,60-14-len(payload)))
    s.send(pkt)
    time.sleep(0.05)
print("sent",20,"frames on",iface)
'"'"''
```

Observed result in verbose mode:

```text
wait for RX CQE: packet=20/20 timeout_ms=30000
seq-query-ppcnt-after-rx ok
  ppcnt rx_packets_phy: 20
  ppcnt rx_broadcast_phy: 20
seq-query-rq-after-rx ok
  rq wq hw_counter: 20
  rq wq sw_counter: 30
```

This established that external ingress from `sdn-svr5:eth2` is visible and the
RX datapath is functional.

raw-loop end-to-end check:

```sh
ssh sdn-svr6 'cd ~/work/takagi/nicd && \
  sudo ./mlxnicd raw-loop \
    --bdf 0000:01:00.0 \
    --peer-if eth2 \
    --src-mac 02:00:00:00:00:06 \
    --dst-mac ff:ff:ff:ff:ff:ff \
    --ethertype 0x88b5 \
    --payload-hex 01020304aabbccdd \
    --rx-count 4 \
    --pre-rx-delay-ms 5000 \
    --timeout-ms 10000; echo rawloop_rc=$?'
```

Observed result:

```text
raw-loop: RX waits for 4 packet(s), pre-rx-delay=5000 ms, timeout=10000 ms
RX armed; sleeping 5000 ms before RX poll
rx frame matched local TX frame prefix; skipped
wait for RX CQE: packet=1/4 timeout_ms=10000
...
  rx frame:
  0010: 78 6e 69 63 64 2d 72 61 77 70 65 65 72 2d 30 30
...
seq-query-ppcnt-after-rx ok
  ppcnt rx_packets_phy: 4
  ppcnt rx_broadcast_phy: 4
rawloop_rc=0
```

Important caveats:

- `raw-loop` defaults to concise output now, but `--verbose` is still
  test-oriented and prints the full mlx5/VFIO trace.
- Manual peer timing still matters. `--pre-rx-delay-ms` is currently the
  simplest way to make peer injection line up with the RX window.
- `raw-loop` now skips the local broadcast TX frame, but the implementation is
  still aimed at testing rather than general-purpose filtering.
- The peer machine does not echo automatically. During manual tests, start
  peer traffic from `sdn-svr5:eth2` while `raw-loop` is waiting.
- TX and RX share one CQ. TX completion is consumed by the TX helper before RX
  polling starts.

Next raw-loop-specific steps:

Quiet-summary check:

```sh
ssh sdn-svr6 'cd ~/work/takagi/nicd && \
  sudo ./mlxnicd raw-loop \
    --bdf 0000:01:00.0 \
    --peer-if eth2 \
    --src-mac 02:00:00:00:00:06 \
    --dst-mac ff:ff:ff:ff:ff:ff \
    --ethertype 0x88b5 \
    --payload-hex 01020304aabbccdd \
    --rx-count 4 \
    --pre-rx-delay-ms 5000 \
    --timeout-ms 10000; echo rawloop_rc=$?'
```

Observed result:

```text
raw-loop: peer-if=eth2 bdf=0000:01:00.0
raw-loop: TX dst=ff:ff:ff:ff:ff:ff src=02:00:00:00:00:06 ethertype=0x88b5
raw-loop: RX waits for 4 packet(s), pre-rx-delay=5000 ms, timeout=10000 ms
raw-loop: ok tx=1 rx=4 skipped_local=1
rawloop_rc=0
```

Next raw-loop-specific steps:

1. Decide whether raw-loop should send once then receive N, receive-only, or
   support both modes explicitly.
2. If needed, replace the current TX-prefix skip with a more explicit packet
   filter model.
3. If this needs to become a reusable tool rather than a test command, separate
   protocol-independent RX/TX primitives from the current test harness.
