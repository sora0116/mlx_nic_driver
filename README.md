# mlxnicd

最小構成の userspace Mellanox/NVIDIA ConnectX-5 Ex 実験ドライバです。
VFIO + BAR0 + DMA を使って mlx5 command path と TX/RX queue を直接扱い、
raw Ethernet frame の送受信を確認することを目的にしています。

背景知識が必要な場合は、先に [doc/fundamentals.md](/home/sora/work/mlxnicd/doc/fundamentals.md)
を読むと追いやすいです。PCIe, VFIO, DMA, mlx5 用語, queue/object の概念を
このリポジトリの文脈でまとめています。

現時点の到達点は次です。

- userspace から ConnectX-5 Ex を VFIO で所有できる
- mlx5 command path で HCA 初期化と queue object 作成ができる
- full-inline SEND で raw Ethernet frame を送信できる
- RQ/RQT/TIR/flow steering 経由で raw Ethernet frame を受信できる
- `raw-loop` で `sdn-svr6 -> NIC -> wire -> sdn-svr5 -> wire -> NIC -> sdn-svr6`
  の end-to-end を確認できる

## 現在の構成

開発対象:

- host: `sdn-svr6`
- peer host: `sdn-svr5`
- remote dir: `~/work/takagi/nicd`
- VFIO 側 BDF: `0000:01:00.0`
- peer netdev: `eth2` (`0000:01:00.1`)

基本方針:

- DPDK は使わない
- Linux kernel driver の datapath には乗らない
- AF_PACKET は peer traffic 注入にだけ使う
- userspace 側 datapath 検証は mlx5/VFIO のみで行う

## ビルド

```sh
make
```

リモート同期:

```sh
make sync
```

## CLI

現在の主要コマンド:

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
./mlxnicd raw-loop --bdf <BDF> --peer-if <ifname> --src-mac <mac> --dst-mac <mac> --ethertype <hex> \
  [--payload-hex HEX] [--rx-count N] [--pre-rx-delay-ms N] [--timeout-ms N] [--verbose]
```

## 代表的な実行例

### RX-only 確認

`sdn-svr6` 側:

```sh
ssh sdn-svr6 'cd ~/work/takagi/nicd && sudo ./mlxnicd mlx5-rx-wait-test 0000:01:00.0'
```

`sdn-svr5` 側:

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

期待される結果の要点:

```text
seq-query-ppcnt-after-rx ok
  ppcnt rx_packets_phy: 20
seq-query-rq-after-rx ok
  rq wq hw_counter: 20
```

### quiet 既定の raw-loop

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
    --timeout-ms 10000'
```

期待される結果:

```text
raw-loop: peer-if=eth2 bdf=0000:01:00.0
raw-loop: TX dst=ff:ff:ff:ff:ff:ff src=02:00:00:00:00:06 ethertype=0x88b5
raw-loop: RX waits for 4 packet(s), pre-rx-delay=5000 ms, timeout=10000 ms
raw-loop: ok tx=1 rx=4 skipped_local=1 skipped_filtered=0
```

### 詳細トレース付き raw-loop

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
    --timeout-ms 10000 \
    --verbose'
```

## ソースファイルの役割

- `src/main.c`
  - CLI 解析
  - サブコマンドごとの引数検証
  - 各 backend 関数の呼び出し

- `src/raw.c`
  - `raw-loop` 専用の CLI 必須引数チェック
  - `mlx5_raw_loop()` への委譲

- `src/vfio.c`
  - VFIO readiness 確認
  - `vfio-pci` bind / restore
  - VFIO container/group/device open
  - BAR0 mmap
  - DMA map/unmap

- `src/mlx5.c`
  - mlx5 command path 実装
  - HCA 初期化
  - CQ/SQ/RQ/RQT/TIR/flow table/FTE 作成破棄
  - raw TX/RX datapath 実装
  - runtime/profile/filter primitive

## 実装の詳細解説

この節は、コードを追うときの道案内です。どの関数が何を担当し、
引数や返り値が何を意味するかをまとめます。

### 1. CLI 層

#### `src/main.c`

`main()`

- 役割:
  - サブコマンドの分岐
  - 文字列引数を各 option struct に詰める
- 主な入力:
  - `argv`
- 主な出力:
  - backend 関数の終了コードを `0/1/2` に正規化して返す

`opt_value(int argc, char **argv, const char *name)`

- 役割:
  - `--name value` 形式の option から値を抜く
- 引数:
  - `argc`, `argv`: 生の CLI
  - `name`: 探したい option 名
- 返り値:
  - 見つかれば value 文字列
  - 見つからなければ `NULL`

#### `src/raw.c`

`raw_loop_run(const struct raw_loop_opts *opts)`

- 役割:
  - `raw-loop` の必須引数が揃っているかを確認する
  - 実 datapath backend の `mlx5_raw_loop()` に渡す
- 引数:
  - `opts->bdf`
  - `opts->peer_if`
  - `opts->src_mac`
  - `opts->dst_mac`
  - `opts->ethertype`
  - `opts->payload_hex`
  - `opts->verbose`
  - `opts->rx_count`
  - `opts->pre_rx_delay_ms`
  - `opts->timeout_ms`
- 返り値:
  - 成功時 `0`
  - 失敗時 `-1`

### 2. VFIO 層

#### `src/vfio.c`

`vfio_check(const char *bdf)`

- 役割:
  - IOMMU group, `vfio-pci`, `/dev/vfio/vfio`, 対象 BDF の driver 状態を確認
- 引数:
  - `bdf == NULL` のときは host 全体の readiness
  - `bdf != NULL` のときは特定 device の状態まで出す
- 返り値:
  - 準備完了なら `0`
  - 準備不足なら `1`
  - 取得失敗なら `-1`

`vfio_set_stdout_quiet(int quiet)`

- 役割:
  - `raw-loop` quiet 実行時に VFIO の通常出力を抑止する
- 引数:
  - `quiet != 0`: 抑止
  - `quiet == 0`: 通常出力
- 返り値:
  - なし

`vfio_device_open(const char *bdf, struct vfio_device *dev)`

- 役割:
  - VFIO container/group/device を開く
  - BAR0 を mmap する
  - bus master を有効化する
- 引数:
  - `bdf`: 対象 PCI function
  - `dev`: 結果を書き込む `struct vfio_device`
- 返り値:
  - 成功時 `0`
  - 失敗時 `-1`

`vfio_dma_map(...)`, `vfio_dma_unmap(...)`

- 役割:
  - userspace メモリを IOVA に map/unmap する
- 返り値:
  - 成功時 `0`
  - 失敗時 `-1`

### 3. mlx5 command path 層

#### `src/mlx5.c`

`mlx5_cmd_ctx_open(const char *bdf, struct mlx5_cmd_ctx *ctx)`

- 役割:
  - VFIO device を開く
  - command queue 用 DMA メモリを確保
  - command path 実行に必要な context を初期化
- 返り値:
  - 成功時 `0`
  - 失敗時 `-1`

`mlx5_cmd_exec(...)`

- 役割:
  - mlx5 command descriptor を組み立てて command queue に投げる
  - `status_own` をポーリングして completion を待つ
  - outbox を返す
- 引数:
  - opcode
  - in/out buffer
  - 表示用タグ文字列
- 返り値:
  - 成功時 `0`
  - firmware / timeout / transport 失敗時 `-1`

ここが `QUERY_ISSI`, `SET_ISSI`, `QUERY_PAGES`, `INIT_HCA`,
`CREATE_CQ`, `CREATE_SQ`, `CREATE_RQ` など全 command の共通基盤です。

### 4. TX/RX object 層

`mlx5_ctx_create_eq()`, `mlx5_ctx_create_cq()`

- 役割:
  - completion を受ける EQ/CQ を作る
- 返り値:
  - 成功時 `0`
  - 失敗時 `-1`

`mlx5_ctx_create_sq()`, `mlx5_ctx_modify_sq_ready()`

- 役割:
  - SQ を作成し RDY に遷移させる

`mlx5_ctx_create_rq()`, `mlx5_ctx_modify_rq_ready()`

- 役割:
  - RQ を作成し RDY に遷移させる

`mlx5_ctx_create_rqt()`, `mlx5_ctx_create_indirect_tir()`

- 役割:
  - RQ を receive side から参照するための RQT/TIR を作る

`mlx5_ctx_create_rx_flow_table()`, `mlx5_ctx_create_rx_flow_group()`,
`mlx5_ctx_set_rx_fte_to_tir()`

- 役割:
  - RX flow steering を作り、受信フレームを TIR に流す

### 5. packet I/O primitive

`build_tx_test_frame(const struct mlx5_tx_test_opts *opts, uint8_t *frame, size_t frame_cap, size_t *frame_len)`

- 役割:
  - 送信用 Ethernet frame を構築する
- 引数:
  - `dst_mac`
  - `src_mac`
  - `ethertype`
  - `payload_hex`
- 返り値:
  - 成功時 `0`
  - 引数不正やサイズ超過で `-1`

`mlx5_sq_post_send_raw(...)`

- 役割:
  - full-inline SEND WQE を 1 つ組み立てて SQ に書き込む
  - doorbell を叩く
  - TX CQE を待つ
- 返り値:
  - 成功時 `0`
  - 失敗時 `-1`

`mlx5_rx_poll_one(struct mlx5_cq_res *cq, struct mlx5_rq_res *rq, int timeout_ms, struct mlx5_rx_packet *pkt)`

- 役割:
  - RX CQE を 1 つ待って `mlx5_rx_packet` にまとめる
- 引数:
  - `cq`: RX completion を読む CQ
  - `rq`: RX buffer slot 解決に使う RQ
  - `timeout_ms`: CQE 待ち時間
  - `pkt`: 出力先
- 返り値:
  - 成功時 `0`
  - timeout / error 時 `-1`

`struct mlx5_rx_packet`

- `data`
  - 受信 buffer の先頭
- `len`
  - CQE から取った packet 長
- `slot`
  - RQ ring 上の buffer slot
- `wqe_counter`
  - completion に対応する WQE 番号

`mlx5_rx_replenish_one(struct mlx5_cmd_ctx *ctx, struct mlx5_rq_res *rq, uint32_t mkey)`

- 役割:
  - 消費済み RX slot を 1 個 repost する
- 返り値:
  - 成功時 `0`
  - 失敗時 `-1`

### 6. runtime / profile / filter primitive

`struct mlx5_test_runtime`

- 役割:
  - 1 回の実行に必要な runtime state を束ねる
- 中身:
  - VFIO command context
  - UAR/PD/TD/MKEY
  - EQ/CQ/SQ/RQ/RQT/TIR/FT/FG/FTE
  - TX frame buffer
  - skip 用 prefix 長
  - `tx_done` 状態

`struct mlx5_run_profile`

- 役割:
  - コマンドごとの mode 差分をフラグで表す
- 主なフィールド:
  - `banner`
  - `do_tx`
  - `create_rx_objects`
  - `post_rx`
  - `create_rx_flow_table`
  - `wait_rx`
  - `skip_local_tx_on_rx`

これにより `mlx5-tx-test`, `mlx5-rx-wait-test`, `raw-loop` が同じ
実行モデルに乗ります。

`mlx5_test_runtime_post_tx(struct mlx5_test_runtime *rt, unsigned int count)`

- 役割:
  - runtime に入っている frame を使って TX を実行する
- 返り値:
  - 成功時 `0`
  - 失敗時 `-1`

`mlx5_test_runtime_wait_rx(struct mlx5_test_runtime *rt, uint32_t count, int timeout_ms, int pre_delay_ms, const struct mlx5_rx_filter *rx_filter, const char *skip_local_tx_prefix, struct mlx5_rx_wait_stats *stats)`

- 役割:
  - RX window に入る
  - 必要なら pre-delay を入れる
  - accept/skip filter を適用しながら packet を数える
- 引数:
  - `count`: accept すべき packet 数
  - `timeout_ms`: 1 packet あたりの timeout
  - `pre_delay_ms`: RX poll 前の待機
  - `rx_filter`: 任意の RX filter primitive
  - `skip_local_tx_prefix`: local TX skip を使うときの指定
  - `stats`: 集計結果
- 返り値:
  - 成功時 `0`
  - timeout / error 時 `-1`

`struct mlx5_rx_wait_stats`

- `accepted`
  - accept 条件を満たして数えられた packet 数
- `skipped_local`
  - local TX skip により捨てた packet 数
- `skipped_filtered`
  - accept 条件不一致など filter で捨てた packet 数

`struct mlx5_rx_filter`

- `should_accept`
  - 受理条件。`NULL` なら accept 制約なし
- `should_skip`
  - skip 条件。`NULL` なら skip 制約なし
- `opaque`
  - filter 実装に渡す補助データ

今は primitive だけ入っていて、`raw-loop` では主に local TX skip に
使っています。将来的には source MAC / EtherType / payload prefix などの
accept filter をここに足します。

`mlx5_test_runtime_cleanup(struct mlx5_test_runtime *rt, int *rc_io)`

- 役割:
  - 作成済み object を逆順で解放する
  - cleanup failure があれば `*rc_io` に反映する

### 7. 実行の中核

`mlx5_run_profile(const char *bdf, const struct mlx5_tx_test_opts *opts, const struct mlx5_run_profile *profile, uint32_t rx_wait_count, int rx_pre_delay_ms, int rx_timeout_ms, struct mlx5_rx_wait_stats *rx_stats)`

- 役割:
  - 1 回分の mlx5 実行全体を orchestration する中核関数
- 流れ:
  1. TX frame を組む
  2. VFIO + mlx5 command context を開く
  3. HCA を初期化
  4. 必要な queue / steering object を作る
  5. profile に応じて TX, RX post, RX wait を行う
  6. cleanup する
- 返り値:
  - 成功時 `0`
  - 失敗時 `-1`

### 8. raw-loop の位置づけ

`mlx5_raw_loop(const struct raw_loop_opts *raw_opts)`

- 役割:
  - `raw-loop` 専用の UX を作る
  - quiet/verbose の切り替え
  - summary 出力
  - `mlx5_run_profile()` 呼び出し
- 引数:
  - CLI で渡された `raw_loop_opts`
- 返り値:
  - 成功時 `0`
  - 失敗時 `-1`

quiet 既定では次だけ出ます。

```text
raw-loop: peer-if=...
raw-loop: TX dst=... src=... ethertype=...
raw-loop: RX waits for ...
raw-loop: ok tx=1 rx=... skipped_local=... skipped_filtered=...
```

## 現在の設計上の制約

- TX は full-inline SEND 前提
- RX filter は primitive 化済みだが、まだ限定利用
- command path / queue setup / datapath が 1 ファイル `src/mlx5.c` に集中
- `raw-loop` は still test harness であり、汎用 API ではない

## 次の自然なステップ

1. RX accept filter を実際の CLI option に出す
2. TX/RX primitive を `src/mlx5.h` に昇格して外部再利用しやすくする
3. `raw-loop`, `mlx5-rx-wait-test`, `mlx5-tx-test` の役割をさらに整理する
