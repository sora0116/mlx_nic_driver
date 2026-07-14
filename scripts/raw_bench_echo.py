#!/usr/bin/env python3

import argparse
import os
import select
import socket
import sys


BENCH_MAGIC = 0x4D4C5842
BENCH_HDR_LEN = 24
BENCH_F_REPLY = 0x0001


def parse_args():
    p = argparse.ArgumentParser(description="mlxnicd raw-bench echo helper")
    p.add_argument("--ifname", required=True, help="interface name on peer host")
    p.add_argument("--ethertype", required=True, help="ethertype, e.g. 0x88b5")
    p.add_argument("--count", type=int, default=0,
                   help="number of requests to echo; 0 means unlimited")
    p.add_argument("--verbose", action="store_true")
    return p.parse_args()


def parse_ethertype(text: str) -> int:
    value = int(text, 0)
    if value < 0 or value > 0xFFFF:
        raise ValueError(f"invalid ethertype: {text}")
    return value


def load_if_mac(ifname: str) -> bytes:
    path = f"/sys/class/net/{ifname}/address"
    with open(path, "r", encoding="ascii") as f:
        return bytes.fromhex(f.read().strip().replace(":", ""))


def be16(data: bytes, off: int) -> int:
    return int.from_bytes(data[off:off + 2], "big")


def be32(data: bytes, off: int) -> int:
    return int.from_bytes(data[off:off + 4], "big")


def put_be16(buf: bytearray, off: int, value: int) -> None:
    buf[off:off + 2] = value.to_bytes(2, "big")


def main() -> int:
    args = parse_args()
    proto = parse_ethertype(args.ethertype)
    if_mac = load_if_mac(args.ifname)
    echoed = 0

    sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(proto))
    sock.bind((args.ifname, proto))

    print(
        f"raw-bench-echo: ifname={args.ifname} ethertype=0x{proto:04x} "
        f"if_mac={':'.join(f'{b:02x}' for b in if_mac)} count={args.count or 'unlimited'}",
        flush=True,
    )

    while args.count == 0 or echoed < args.count:
        ready, _, _ = select.select([sock], [], [], 30.0)
        if not ready:
            print("raw-bench-echo: timeout waiting for request", file=sys.stderr, flush=True)
            return 1

        frame = bytearray(sock.recv(4096))
        if len(frame) < 14 + BENCH_HDR_LEN:
            continue
        if be16(frame, 12) != proto:
            continue
        if be32(frame, 14) != BENCH_MAGIC:
            continue

        flags = be16(frame, 20)
        if flags & BENCH_F_REPLY:
            continue

        dst = bytes(frame[0:6])
        src = bytes(frame[6:12])
        frame[0:6] = src
        frame[6:12] = if_mac
        put_be16(frame, 20, flags | BENCH_F_REPLY)

        sent = sock.send(frame)
        echoed += 1

        if args.verbose:
            seq = be32(frame, 22)
            print(
                f"raw-bench-echo: echoed seq={seq} req_src={':'.join(f'{b:02x}' for b in src)} "
                f"req_dst={':'.join(f'{b:02x}' for b in dst)} len={sent}",
                flush=True,
            )

    print(f"raw-bench-echo: done echoed={echoed}", flush=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"raw-bench-echo: {exc}", file=sys.stderr, flush=True)
        raise SystemExit(1)
