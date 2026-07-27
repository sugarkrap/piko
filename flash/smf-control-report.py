#!/usr/bin/env python3
"""Report Sharp SL SMF control regions from a logical-address SMF image.

This is a read-only forensic helper to compare images and spot control-word drift
around SHARPSL partinfo logical addresses (0x60000 and 0x64000).

Important: these offsets are logical-address offsets used by nandlogical/FTL.
If you pass a raw physical SMF dump (no OOB/FTL translation), values here are
usually meaningless machine code or 0xFF noise.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

OFFSETS = [0x60014, 0x60020, 0x64014, 0x64020]
MAGIC_OFFSETS = [0x60008, 0x60018, 0x60028, 0x64008, 0x64018, 0x64028]


def u32le(buf: bytes, off: int) -> int:
    return struct.unpack_from("<I", buf, off)[0]


def u32be(buf: bytes, off: int) -> int:
    return struct.unpack_from(">I", buf, off)[0]


def magic_name(v: int) -> str:
    if v == 0x424F4F54:
        return "BOOT"
    if v == 0x4653524F:
        return "FSRO"
    if v == 0x46535257:
        return "FSRW"
    return "?"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("image", help="path to SMF logical-view image (expected around 0x700000 bytes)")
    args = ap.parse_args()

    p = Path(args.image)
    buf = p.read_bytes()

    print(f"image: {p}")
    print(f"size:  0x{len(buf):x} ({len(buf)} bytes)")

    if len(buf) < 0x64030:
        print("warning: image is smaller than expected control region span")

    print("control words (little-endian):")
    for off in OFFSETS:
        if off + 4 <= len(buf):
            v = u32le(buf, off)
            print(f"  0x{off:06x}: 0x{v:08x} ({v})")
        else:
            print(f"  0x{off:06x}: <out of range>")

    print("partinfo magics (big-endian):")
    found_known_magic = False
    for off in MAGIC_OFFSETS:
        if off + 4 <= len(buf):
            v = u32be(buf, off)
            name = magic_name(v)
            if name != "?":
                found_known_magic = True
            print(f"  0x{off:06x}: 0x{v:08x} {name}")
        else:
            print(f"  0x{off:06x}: <out of range>")

    if not found_known_magic:
        print("note: no BOOT/FSRO/FSRW magics found at logical offsets; image may be raw physical view")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
