#!/usr/bin/env python3
"""Convert a raw framebuffer dump from userspace/src/fbgrab.c into a PNG.

    tools/decode-fb.py screen.raw screen.png [--width 640] [--height 480]
                       [--bpp 16] [--crop X,Y,W,H] [--scale N]

Defaults match the Zaurus SL-C760 panel: 640x480 RGB565. Writes a plain
PNG with no external dependencies (zlib is stdlib).
"""

import argparse
import struct
import sys
import zlib


def unpack_rgb565(data, width, height):
    """RGB565 little-endian -> list of rows of (r, g, b) bytes."""
    rows = []
    stride = width * 2
    for y in range(height):
        row = bytearray()
        base = y * stride
        for x in range(width):
            px = data[base + x * 2] | (data[base + x * 2 + 1] << 8)
            r = (px >> 11) & 0x1F
            g = (px >> 5) & 0x3F
            b = px & 0x1F
            # Replicate high bits into the low ones so full-scale stays full.
            row += bytes(((r << 3) | (r >> 2),
                          (g << 2) | (g >> 4),
                          (b << 3) | (b >> 2)))
        rows.append(row)
    return rows


def unpack_rgb888(data, width, height, bpp):
    rows = []
    nbytes = bpp // 8
    stride = width * nbytes
    for y in range(height):
        row = bytearray()
        base = y * stride
        for x in range(width):
            off = base + x * nbytes
            b, g, r = data[off], data[off + 1], data[off + 2]
            row += bytes((r, g, b))
        rows.append(row)
    return rows


def write_png(path, rows, width, height, scale=1):
    if scale > 1:
        scaled = []
        for row in rows:
            wide = bytearray()
            for x in range(width):
                px = row[x * 3:x * 3 + 3]
                wide += px * scale
            for _ in range(scale):
                scaled.append(wide)
        rows = scaled
        width *= scale
        height *= scale

    raw = bytearray()
    for row in rows:
        raw.append(0)          # filter type 0 (None)
        raw += row

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")

    with open(path, "wb") as fh:
        fh.write(png)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("raw")
    ap.add_argument("png")
    ap.add_argument("--width", type=int, default=640)
    ap.add_argument("--height", type=int, default=480)
    ap.add_argument("--bpp", type=int, default=16)
    ap.add_argument("--crop", help="X,Y,W,H region to extract")
    ap.add_argument("--scale", type=int, default=1, help="integer upscale")
    args = ap.parse_args()

    with open(args.raw, "rb") as fh:
        data = fh.read()

    expect = args.width * args.height * args.bpp // 8
    if len(data) < expect:
        sys.exit(f"short dump: got {len(data)} bytes, expected {expect}")

    if args.bpp == 16:
        rows = unpack_rgb565(data, args.width, args.height)
    elif args.bpp in (24, 32):
        rows = unpack_rgb888(data, args.width, args.height, args.bpp)
    else:
        sys.exit(f"unsupported bpp {args.bpp}")

    w, h = args.width, args.height
    if args.crop:
        cx, cy, cw, ch = (int(v) for v in args.crop.split(","))
        rows = [r[cx * 3:(cx + cw) * 3] for r in rows[cy:cy + ch]]
        w, h = cw, ch

    write_png(args.png, rows, w, h, args.scale)
    print(f"wrote {args.png} ({w}x{h}, scale {args.scale})")


if __name__ == "__main__":
    main()
