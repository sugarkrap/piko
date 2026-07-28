#!/usr/bin/env python3
"""
Extract Sharp Zaurus .dbk whole-NAND images into main-area binaries.

Observed format for systc760.dbk:
- 16-byte global header
- 8192 block records for a 128MiB NAND (16KiB eraseblock)
- each record is 0x4210 bytes: 0x10 per-block header + 0x4200 page payload
- payload contains 32 pages of (512B main + 16B OOB)

This tool reconstructs:
- full_main.bin (128MiB main area)
- smf_main.bin  (first 0x700000 bytes)
- root_main.bin (next 0x3500000 bytes)
- home_main.bin (remaining 0x4400000 bytes)
- optional per-block headers dump for forensics
"""

from __future__ import annotations

import argparse
import os
import struct
from pathlib import Path

GLOBAL_HDR = 0x10
BLOCK_REC = 0x4210
BLOCK_HDR = 0x10
BLOCK_PAYLOAD = 0x4200
PAGE_MAIN = 512
PAGE_OOB = 16
PAGES_PER_BLOCK = 32

NAND_BLOCKS = 8192
FULL_MAIN_SIZE = 0x8000000  # 128MiB
SMF_SIZE = 0x700000
ROOT_SIZE = 0x3500000
HOME_SIZE = 0x4400000


def iter_blocks(buf: bytes):
    off = GLOBAL_HDR
    for idx in range(NAND_BLOCKS):
        rec = buf[off : off + BLOCK_REC]
        if len(rec) != BLOCK_REC:
            raise ValueError(f"short record at block {idx}")
        hdr = rec[:BLOCK_HDR]
        payload = rec[BLOCK_HDR:]
        yield idx, hdr, payload
        off += BLOCK_REC


def payload_to_main(payload: bytes) -> bytes:
    if len(payload) != BLOCK_PAYLOAD:
        raise ValueError("invalid payload size")
    out = bytearray(PAGES_PER_BLOCK * PAGE_MAIN)
    src = 0
    dst = 0
    for _ in range(PAGES_PER_BLOCK):
        out[dst : dst + PAGE_MAIN] = payload[src : src + PAGE_MAIN]
        src += PAGE_MAIN + PAGE_OOB
        dst += PAGE_MAIN
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("dbk", help="path to .dbk file")
    ap.add_argument("--out-dir", default="flash/.cache/dbk", help="output directory")
    args = ap.parse_args()

    dbk_path = Path(args.dbk)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    data = dbk_path.read_bytes()
    expected = GLOBAL_HDR + NAND_BLOCKS * BLOCK_REC
    if len(data) != expected:
        raise SystemExit(
            f"unexpected dbk size: got {len(data)}, expected {expected}"
        )

    # Basic marker sanity.
    if not data.startswith(b"[I]JFFS2TOP:"):
        raise SystemExit("unexpected global header magic")

    full_main = bytearray(FULL_MAIN_SIZE)
    headers_path = out_dir / "block_headers.bin"
    with headers_path.open("wb") as hfp:
        for idx, hdr, payload in iter_blocks(data):
            hfp.write(hdr)
            block_main = payload_to_main(payload)
            start = idx * (PAGES_PER_BLOCK * PAGE_MAIN)
            full_main[start : start + len(block_main)] = block_main

    full_main_path = out_dir / "full_main.bin"
    full_main_path.write_bytes(full_main)

    smf = full_main[:SMF_SIZE]
    root = full_main[SMF_SIZE : SMF_SIZE + ROOT_SIZE]
    home = full_main[SMF_SIZE + ROOT_SIZE : SMF_SIZE + ROOT_SIZE + HOME_SIZE]

    (out_dir / "smf_main.bin").write_bytes(smf)
    (out_dir / "root_main.bin").write_bytes(root)
    (out_dir / "home_main.bin").write_bytes(home)

    print(f"dbk: {dbk_path}")
    print(f"size: {len(data)} bytes")
    print(f"full_main: {full_main_path} ({len(full_main)} bytes)")
    print(f"smf_main: {(out_dir / 'smf_main.bin')} ({len(smf)} bytes)")
    print(f"root_main: {(out_dir / 'root_main.bin')} ({len(root)} bytes)")
    print(f"home_main: {(out_dir / 'home_main.bin')} ({len(home)} bytes)")
    print(f"headers: {headers_path} ({headers_path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
