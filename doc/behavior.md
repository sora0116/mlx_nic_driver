# mlxnicd 動作フロー

この文書は、`mlxnicd` が NIC を初期化し、raw packet を送受信する流れを
図で追うための補助資料です。`doc/overview.svg` が全体図なら、こちらは
`raw-loop` を基準にした時系列の説明です。

前提資料:

- 背景知識: [fundamentals.md](/home/sora/work/mlxnicd/doc/fundamentals.md)
- 全体図: [overview.md](/home/sora/work/mlxnicd/doc/overview.md)
- 実装解説: [README.md](/home/sora/work/mlxnicd/README.md)

## 1. 入口

CLI から見た入口は単純です。

```mermaid
flowchart LR
    A["./mlxnicd raw-loop ..."] --> B["main()"]
    B --> C["raw_loop_run()"]
    C --> D["mlx5_raw_loop()"]
    D --> E["mlx5_run_profile()"]
```

- `main()` が `raw-loop` サブコマンドを解釈する
- `raw_loop_run()` が必須引数を検証する
- 実際の NIC bring-up と datapath 実行は `mlx5_raw_loop()` から
  `mlx5_run_profile()` に集約される

## 2. 初期化シーケンス

`raw-loop` はまず command path で HCA と queue object を立ち上げます。

```mermaid
flowchart TD
    A["mlx5_cmd_ctx_open()<br/>VFIO open + BAR0 mmap + command DMA確保"]
    B["mlx5_ctx_enable_hca()"]
    C["mlx5_ctx_set_issi()"]
    D["mlx5_ctx_query_pages(boot)"]
    E["mlx5_ctx_give_pages()<br/>boot pages"]
    F["mlx5_ctx_query_dtor() / query_hca_cap / set_hca_cap"]
    G["mlx5_ctx_query_pages(init)"]
    H["mlx5_ctx_give_pages()<br/>init pages"]
    I["mlx5_ctx_init_hca()"]
    J["mlx5_ctx_query_paos()"]
    K["mlx5_ctx_enable_vport_promisc()"]
    L["alloc UAR / PD / TD / Q counter"]
    M["create MKEY / TIS / EQ / CQ / SQ"]
    N["modify SQ -> RDY"]
    O["create RQ"]
    P["modify RQ -> RDY"]
    Q["post RX buffers"]
    R["create RQT / TIR"]
    S["create flow table / flow group / FTE"]

    A --> B --> C --> D --> E --> F --> G --> H --> I --> J --> K --> L --> M --> N --> O --> P --> Q --> R --> S
```

この段階で重要なのは次の 3 点です。

- command path の仕事
  - HCA 初期化、cap 設定、各 object 作成はすべて firmware command で行う
- datapath 準備の仕事
  - SQ/CQ ができると TX 可能になる
  - RQ/RQT/TIR/flow table ができると RX を NIC 側で steering できる
- `raw-loop` 固有の設定
  - `create_rx_flow_table = 1`
  - `wait_rx = 1`
  - `skip_local_tx_on_rx = 1`

## 3. TX フロー

TX は full-inline SEND WQE を 1 本だけ投げるシンプルな経路です。

```mermaid
sequenceDiagram
    participant CLI as raw-loop
    participant CPU as userspace
    participant SQ as SQ memory
    participant NIC as ConnectX-5
    participant CQ as CQ memory

    CLI->>CPU: build_tx_test_frame()
    CPU->>SQ: mlx5_sq_post_send_raw()<br/>SEND WQE を書く
    CPU->>SQ: SQ doorbell record 更新
    CPU->>NIC: UAR/BF に MMIO write64
    NIC->>CQ: 送信完了 CQE を書く
    CPU->>CQ: mlx5_poll_cq() で完了確認
```

補足:

- フレームは `build_tx_test_frame()` で組み立てる
- payload は `--payload-hex` を使わなければ既定値が入る
- current 実装は full-inline 前提なので、送信サイズには上限がある

## 4. RX フロー

RX は「RQ に buffer を置く」「flow table で TIR に流す」「CQE を待つ」の
3 段に分かれます。

```mermaid
sequenceDiagram
    participant CPU as userspace
    participant RQ as RQ memory
    participant FT as flow steering
    participant NIC as ConnectX-5
    participant CQ as CQ memory
    participant BUF as RX buffer

    CPU->>RQ: mlx5_rq_post_buffers()
    CPU->>FT: flow table root / group / FTE 作成
    NIC->>FT: 受信 frame を match
    FT->>NIC: TIR/RQT/RQ を選択
    NIC->>BUF: DMA write
    NIC->>CQ: RX CQE write
    CPU->>CQ: mlx5_rx_poll_one()
    CPU->>BUF: frame を読む
    CPU->>RQ: mlx5_rx_replenish_one()
```

`raw-loop` では wait 中に自分が送った TX frame も同じポートで見えることが
あるため、`skip_local_tx_on_rx` で自分自身の送信を除外します。

## 5. raw-loop の end-to-end

`raw-loop` は「自分で 1 発送る」「peer から返ってきた複数 packet を待つ」
という実験コマンドです。

```mermaid
sequenceDiagram
    participant Local as sdn-svr6 raw-loop
    participant NIC0 as 0000:01:00.0
    participant Wire as wire
    participant Peer as sdn-svr5 eth2

    Local->>NIC0: TX frame 1本
    NIC0->>Wire: 送信
    Wire->>Peer: peer host が受信
    Peer->>Wire: peer host が応答 frame を送信
    Wire->>NIC0: 応答 frame が戻る
    NIC0->>Local: RX CQE + DMA buffer
    Local->>Local: local TX echo を除外しつつ rx_count まで待機
```

このコマンドが成功すると、概念的には次を確認できています。

- userspace からの command path 初期化が通った
- full-inline SEND で wire に出せた
- flow steering 付き RX object 群が機能した
- DMA された受信 frame を userspace 側で回収できた

## 6. 実装の対応先

図とコードの対応は次です。

- CLI 入口: [src/main.c](/home/sora/work/mlxnicd/src/main.c)
- `raw-loop` 引数検証: [src/raw.c](/home/sora/work/mlxnicd/src/raw.c)
- bring-up / TX / RX 本体: [src/mlx5.c](/home/sora/work/mlxnicd/src/mlx5.c)

特に追うと分かりやすい関数:

- `mlx5_raw_loop()`
- `mlx5_run_profile()`
- `mlx5_sq_post_send_raw()`
- `mlx5_wait_for_rx_packets()`
- `mlx5_test_runtime_cleanup()`
