#!/usr/bin/env python3
"""Render the bootstrap splash asset: modules/initramfs/splash.ppm.gz.

    tools/make-splash.py [--src FILE] [--out FILE] [--width 640] [--height 480]
                         [--logo-height 360] [--colors 64] [--budget 40000]

The bootstrap kernel's initramfs draws its splash with busybox's `fbsplash`
applet, which takes a plain binary PPM (P6) and blits it at IMG_LEFT/IMG_TOP
-- it does not scale, and it does not clear the screen. So the asset this
script writes is the *whole* 640x480 screen already composited: a white
field with the artwork centred in it. fbsplash then draws it at 0,0 and the
white background comes along for free.

Why this is pre-rendered and tracked rather than built from source art on
every build: the build side (tools/build-initramfs.sh, and CI) only has to
`gunzip` a tracked file, so it needs no image library at all. This script is
a maintenance tool -- run it by hand when the artwork changes, commit the
result. It is the only thing here that wants Pillow, and nothing in the
normal build path calls it.

## The size budget is the whole design constraint

The initramfs is linked *into* the bootstrap kernel (CONFIG_INITRAMFS_SOURCE
in kernel.config-corgi-7.1.4-minimal), and that kernel has to fit the mtd1
NAND slot: 1294336 bytes, enforced in flash/kernel-flash.sh,
flash/run-stage2-smf-update.sh, userspace/src/piko-update.c and CI. As of
this writing the bootstrap zImage is ~1.24 MB, which leaves roughly 55 kB of
headroom for the splash asset *and* the fbsplash applet together.

Both the cpio and the zImage are gzipped, so what actually costs flash is
this file's *compressed* size -- which is why `--colors` matters far more
than it looks. A 640x480 field of pure white compresses to almost nothing;
the artwork is the entire cost. Palettising to 64 colours roughly halves it
and is very close to free visually, because the panel is RGB565 anyway (5-6
bits per channel) -- there is no point shipping 8-bit precision the W100
cannot display. Dithering is deliberately NOT used: it destroys the flat
runs gzip depends on and can double the compressed size.

--budget is a guard rail, not the real limit; the real check is the CI step
that weighs the finished zImage. This one just fails early and loudly rather
than letting an oversized asset get discovered a kernel build later.
"""

import argparse
import gzip
import io
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("make-splash.py needs Pillow (pip install pillow). "
             "Only needed to regenerate the asset; the build itself does not.")


def build(src, width, height, logo_height, colors, background):
    art = Image.open(src).convert("RGB")

    # Scale to the requested height, preserving aspect. LANCZOS because the
    # artwork is downscaled a long way and this is a one-off cost.
    logo_height = min(logo_height, height)
    logo_width = max(1, round(art.width * logo_height / art.height))
    if logo_width > width:
        logo_width = width
        logo_height = max(1, round(art.height * logo_width / art.width))
    art = art.resize((logo_width, logo_height), Image.LANCZOS)

    canvas = Image.new("RGB", (width, height), background)
    canvas.paste(art, ((width - logo_width) // 2, (height - logo_height) // 2))

    if colors:
        # dither=NONE on purpose -- see the module docstring.
        canvas = canvas.quantize(colors=colors, dither=Image.Dither.NONE)
        canvas = canvas.convert("RGB")

    return canvas, (logo_width, logo_height)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--src", default="modules/initramfs/splash-src.png")
    p.add_argument("--out", default="modules/initramfs/splash.ppm.gz")
    p.add_argument("--width", type=int, default=640)
    p.add_argument("--height", type=int, default=480)
    p.add_argument("--logo-height", type=int, default=360)
    p.add_argument("--colors", type=int, default=64,
                   help="palette size; 0 keeps truecolor (much larger)")
    p.add_argument("--budget", type=int, default=40000,
                   help="fail if the compressed asset exceeds this many bytes")
    args = p.parse_args()

    canvas, logo = build(args.src, args.width, args.height,
                         args.logo_height, args.colors, (255, 255, 255))

    raw = io.BytesIO()
    canvas.save(raw, "PPM")
    raw = raw.getvalue()

    # mtime=0 so rebuilding identical art produces an identical file and the
    # tracked asset doesn't churn in git for no reason.
    packed = gzip.compress(raw, compresslevel=9, mtime=0)

    with open(args.out, "wb") as fh:
        fh.write(packed)

    print(f"{args.out}: {args.width}x{args.height} screen, "
          f"logo {logo[0]}x{logo[1]}, "
          f"{args.colors or 'true'}-color")
    print(f"  uncompressed PPM : {len(raw)} bytes")
    print(f"  gzipped (flash)  : {len(packed)} bytes "
          f"(budget {args.budget})")

    if len(packed) > args.budget:
        sys.exit(f"make-splash.py: asset is {len(packed)} bytes compressed, "
                 f"over the {args.budget}-byte budget -- lower --logo-height "
                 f"or --colors.")


if __name__ == "__main__":
    main()
