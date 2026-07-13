# mlxnicd 基礎知識

この文書は、`README.md` や `docs/agent-handoff.md` に出てくる用語を
理解するための背景資料です。コードを読む前提知識を整理することが目的で、
Linux カーネルや NIC driver 実装の全体像を、mlxnicd の文脈に合わせて
絞って説明します。

## このプロジェクトで何をやっているのか

mlxnicd は、Linux kernel の `mlx5_core` driver や DPDK を使わずに、
userspace から Mellanox/NVIDIA ConnectX-5 Ex を直接制御して raw packet
を送受信する実験用 driver です。

経路は次です。

```text
process
  -> VFIO
  -> PCIe device BAR0 / DMA
  -> mlx5 command path
  -> SQ / RQ / CQ
  -> wire
```

この文書では、この各要素が何を意味しているかを説明します。

## 1. PCIe の基礎

### PCIe device とは

NIC は PCIe bus にぶら下がる device です。Linux からは通常、

- BDF
  - Bus:Device.Function
  - 例: `0000:01:00.0`

で識別します。

このプロジェクトでは:

- `0000:01:00.0`
  - userspace + VFIO 側
- `0000:01:00.1`
  - Linux `mlx5_core` 側

として使い分けています。

### BAR とは

BAR は Base Address Register の略です。PCIe device が host に対して
公開する MMIO 領域や I/O 領域の窓です。

NIC driver は BAR を通して device register を読み書きします。

このプロジェクトで主に使うのは:

- BAR0
  - ConnectX-5 Ex の register 空間
  - command interface register
  - doorbell/UAR 関連領域
  - initialization segment

### MMIO とは

MMIO は Memory-Mapped I/O です。device register がメモリのように
見える方式です。通常の RAM と違い、read/write がそのまま device との
通信になります。

このため driver 実装では:

- register 幅を間違えない
- endian を間違えない
- 書き込み順序を守る

ことが重要です。

### config space とは

PCIe device には BAR とは別に PCI config space があります。
ここには device ID, vendor ID, command register などが入っています。

`vfio.c` や `pci.c` はこの config space や sysfs 情報も使って、
device の状態を確認します。

## 2. IOMMU と VFIO

### IOMMU とは

IOMMU は device から見えるアドレス空間を変換・保護する仕組みです。
CPU に対する MMU の device 版だと考えるとわかりやすいです。

NIC は DMA で host memory にアクセスしますが、IOMMU がないと:

- device が任意の物理メモリを触れてしまう
- userspace に device を直接渡すのが危険

になります。

そのため userspace driver では通常、

- IOMMU を有効にする
- VFIO を使う

のが前提です。

### IOMMU group とは

IOMMU group は「安全にまとめて隔離できる device の単位」です。

ある device を userspace に渡したいとき、その group 全体が
一貫して扱える必要があります。

`vfio-check` が見ている `iommu_group` はこの値です。

### VFIO とは

VFIO は Virtual Function I/O の略で、userspace に device を安全に
渡すための Linux kernel の仕組みです。

主な役割:

- device file 経由で userspace に device を開かせる
- BAR mmap を許可する
- DMA mapping/unmapping を許可する
- IOMMU と組み合わせて isolation を保つ

### `vfio-pci` driver とは

通常 NIC は `mlx5_core` のような kernel driver に bind されています。
userspace から直接扱うには、その device を `vfio-pci` に bind し直します。

つまり:

- kernel datapath を外れる
- userspace 側が register と DMA を直接扱う

という意味です。

### container / group / device

VFIO の API では概念が 3 層あります。

- container
  - IOMMU backend との紐付け
- group
  - IOMMU group 単位の管理
- device
  - 個別 device の file descriptor

`vfio_device_open()` はこの 3 層を順に開いていきます。

### BAR mmap と DMA map

VFIO を使うと userspace から:

- BAR0 を mmap できる
- 確保したメモリを IOVA に map できる

ようになります。

これが userspace driver 実装の最低条件です。

## 3. DMA の基礎

### DMA とは

DMA は Direct Memory Access の略です。device が CPU を介さずに host memory
を read/write する仕組みです。

NIC の送受信では必須です。

- TX
  - host memory に置いた packet を NIC が読む
- RX
  - NIC が host memory に packet を書く

### IOVA とは

IOVA は I/O Virtual Address です。device が見る仮想アドレスです。

CPU が見る userspace address と、device が見る IOVA は別です。
VFIO + IOMMU が両者を結びます。

### なぜページ単位なのか

DMA mapping は通常 page 単位です。理由は:

- IOMMU が page table 的な構造で管理する
- alignment を揃えやすい
- unmap/reuse しやすい

ためです。

### doorbell record と data buffer

queue を動かすための DMA memory は大きく分けて:

- queue 本体
  - WQE/CQE ring
- doorbell record
  - producer/consumer index の共有領域
- packet buffer
  - 実 packet を置く領域

に分かれます。

## 4. mlx5 の大づかみ

### mlx5 とは

mlx5 は Mellanox/NVIDIA の ConnectX 世代 NIC の hardware/software
interface の総称です。Linux kernel では `mlx5_core` driver がこれを扱います。

このプロジェクトでは、その kernel driver がやっていることの一部を
userspace で最小実装しています。

### command path と datapath

mlx5 では大きく 2 種類の経路があります。

- command path
  - device object を作る/壊す
  - capability を問い合わせる
  - HCA を初期化する
- datapath
  - queue を動かして packet を送受信する

このプロジェクトではまず command path を実装し、その上に datapath を
積んでいます。

### HCA とは

HCA は Host Channel Adapter です。Mellanox 系 NIC documentation では
NIC 本体を HCA と呼ぶことが多いです。

`INIT_HCA`, `ENABLE_HCA` はこの HCA を動作状態に持っていく command です。

### UAR とは

UAR は User Access Region です。doorbell を叩くための MMIO 領域です。

送信/受信 queue に WQE を積んだあと、device に「新しい仕事がある」と
知らせる必要があります。その通知先が UAR です。

### PD とは

PD は Protection Domain です。mlx5 object や memory region を
グルーピングする保護単位です。

簡単には:

- この queue はどの memory domain に属するか
- この memory key はどの domain で使うか

を表します。

### MKEY / LKEY とは

MKEY は memory region を NIC に見せるための key です。
WQE では packet buffer や queue buffer を参照する際に key が必要です。

文脈によって:

- MKEY
  - object 全体
- LKEY
  - local access に使う key 値

という言い方をします。

このプロジェクトでは packet buffer を RQ/SQ から参照するために使います。

### TIS / TIR

- TIS
  - Transport Interface Send
  - 送信側の interface object
- TIR
  - Transport Interface Receive
  - 受信側の interface object

`SQ` は TIS と結びついて送信し、`TIR` は `RQ` / `RQT` と結びついて
受信先になります。

### SQ / RQ / CQ

mlx5 datapath の基本 queue です。

- SQ
  - Send Queue
  - 送信 WQE を積む ring
- RQ
  - Receive Queue
  - 受信 buffer を積む ring
- CQ
  - Completion Queue
  - SQ/RQ の completion を受ける ring

driver 実装では:

1. SQ に SEND WQE を積む
2. UAR を叩く
3. CQ で TX 完了を見る

RX では:

1. RQ に受信 buffer を積む
2. packet が来る
3. CQ に RX completion が出る
4. driver が packet を読む
5. RQ を replenish する

### EQ

EQ は Event Queue です。CQ より上位の event 配送単位です。
このプロジェクトでは最小構成のため、まず EQ を作り、その上に CQ を作ります。

### RQT

RQT は Receive Queue Table です。複数の RQ を束ねて参照する table です。

最小構成でも、flow steering から indirect receive に流すために使います。

### flow table / flow group / FTE

mlx5 受信側では、packet をどの TIR に流すかを flow steering で決めます。

- flow table
  - ルール群の table
- flow group
  - rule のまとまり
- FTE
  - Flow Table Entry
  - 個別 rule

このプロジェクトでは最小実装として、

- broad な受信 rule
- その rule で packet を TIR に流す

という形を取っています。

### direct TIR と indirect TIR

- direct TIR
  - TIR が直接 1 個の RQ を指す
- indirect TIR
  - TIR が RQT を指し、その先で RQ を選ぶ

このプロジェクトでは RQT + indirect TIR を使う流れが中心です。

### WQE / CQE

- WQE
  - Work Queue Element
  - NIC に渡す仕事 1 件
- CQE
  - Completion Queue Element
  - 仕事完了通知 1 件

たとえば:

- SEND WQE
  - packet を送ってくれ
- RX WQE
  - この buffer を受信用に使ってよい
- CQE
  - 送信完了した
  - 受信 packet がこの buffer に入った

### full-inline SEND

packet 本体を WQE 内に直接埋め込む送信方式です。

利点:

- 実装が単純
- 小さい packet で最小実験をしやすい

制約:

- WQE に入るサイズまでに限られる

このプロジェクトではまず full-inline SEND を使って TX を成立させています。

## 5. README / handoff に出てくる重要用語

### `raw-loop`

最小の end-to-end 確認コマンドです。

意味:

- 1 回送る
- RX を待つ
- 必要なら自分の TX を skip
- peer packet を数える

quiet 既定では summary だけを出し、`--verbose` で全 trace を出します。

### `mlx5-rx-wait-test`

RX-only の確認コマンドです。peer host から packet を送って、
RX datapath が成立していることを確認するのに使います。

### `pre-rx-delay`

RX poll に入る前の待機時間です。手動テストで peer 側送信タイミングを
合わせやすくするためのオプションです。

### `skip_local`

`raw-loop` では自分が broadcast 送信した frame を自分で受けることがあります。
その self-receive を数えないようにする処理です。

### `accept filter` / `skip filter`

RX wait primitive に渡す packet filter です。

- accept filter
  - 条件に一致する packet だけ数える
- skip filter
  - 条件に一致した packet は見なかったことにする

現在は primitive 化だけされていて、今後 source MAC や payload prefix に
よる filter に拡張していく想定です。

### `runtime`

1 回の実行に必要な state 一式です。queue object, command context,
DMA-backed memory, TX frame 情報などをまとめて持ちます。

### `profile`

その実行が:

- TX-only なのか
- RX-only なのか
- TX+RX なのか

を決める mode 設定です。実装上は複数コマンドを 1 つの実行モデルに
載せるための struct です。

## 6. コードを読むときの見方

### 最初に読むべき順番

1. `README.md`
2. `docs/agent-handoff.md`
3. `src/main.c`
4. `src/raw.c`
5. `src/vfio.c`
6. `src/mlx5.c`

### `src/mlx5.c` の読み順

おすすめは次の順です。

1. helper
   - endian / dump / MMIO
2. command path
   - `mlx5_cmd_exec()`
3. object create/destroy
   - CQ/SQ/RQ/RQT/TIR/FT
4. packet I/O primitive
   - `build_tx_test_frame()`
   - `mlx5_sq_post_send_raw()`
   - `mlx5_rx_poll_one()`
   - `mlx5_wait_for_rx_packets()`
5. runtime/profile/filter
   - `mlx5_test_runtime_*`
   - `mlx5_run_profile()`
6. public wrapper
   - `mlx5_tx_test()`
   - `mlx5_rx_wait_test()`
   - `mlx5_raw_loop()`

## 7. このプロジェクト特有の注意点

- Linux の `mlx5_core` と userspace 実装は同じ NIC family を扱うが、
  このプロジェクトは kernel datapath を借りない
- peer traffic 注入のために AF_PACKET を使っても、それは peer 側だけ
- userspace 側 datapath 検証はあくまで VFIO + mlx5 object 経由
- queue が動いても flow steering が無いと packet が desired RQ に来ない
- TX ができても RX は別経路なので、片方が動いてももう片方が未完成であり得る

## 8. いま自然に次に学ぶべきこと

この文書を読んだ次は:

1. `README.md` の関数説明を読む
2. `src/vfio.c` の `vfio_device_open()` を読む
3. `src/mlx5.c` の `mlx5_cmd_exec()` を読む
4. `mlx5_run_profile()` と `mlx5_raw_loop()` を読む

この順で追うと、背景知識と実装がつながりやすいです。
