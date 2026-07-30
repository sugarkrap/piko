#!/bin/sh
set -eu

# Rebuilds mtd3.jffs2 (the "home" partition image for SD-card recovery
# flashing, see docs/FLASH-MTD1-MTD3-SAFE.md) by fully UNPACKING an
# existing mtd3.jffs2 as a base, overlaying the latest kernel + modules +
# rootfs/ on top, and building a single fresh image with mkfs.jffs2 -r
# (no -i).
#
# This project has no full base userland (busybox, /bin, /lib, device
# nodes, etc.) committed anywhere -- rootfs/ here is only the same overlay
# tools/build-and-deploy.sh and flash/build-update-package.sh push onto a
# live device. The only place a full "home" tree currently exists is inside
# a previously-built mtd3.jffs2 already staged somewhere (e.g. on the SD
# card) -- that's why this unpacks an existing image rather than building
# a rootfs from nothing.
#
# NOT using mkfs.jffs2 -i/--incremental (2026-07-30, confirmed on a real
# base image): its incremental mode reads the base to learn every existing
# inode/version number, an O(n^2)-looking operation over total node
# records (not distinct files) -- a real base image with ~1,550 files but
# ~18,000 accumulated node versions (config files rewritten many times
# over the image's life) made it run 30+ minutes without finishing, while
# `jffs2dump` reads the exact same file end-to-end in under 30 seconds.
# Unpacking + a single fresh non-incremental build sidesteps that
# bookkeeping entirely and is dramatically faster.
#
# NOT using jffs2reader for the unpack either (2026-07-30, found the hard
# way -- a full combined mtd1+mtd3 flash + a real kexec segfault on
# hardware): jffs2reader SILENTLY corrupts substantial files (every ELF
# binary/kernel module tested over ~50KB) while still exiting 0 and
# producing the RIGHT byte count -- only the content is garbage (every
# ELF section header entry zeroed). `jffs2dump`-based verification of the
# built image still looked fine, because jffs2dump only reads node
# metadata, never decompresses content -- it can't catch this class of
# corruption at all. tools/jffs2-mount-extract.sh replaces the unpack
# step: it loads a real mtdram device, writes the base image into it, and
# mounts it with the actual kernel jffs2 driver (the same code that runs
# on real hardware) -- proven byte-correct where jffs2reader was not.
# This needs root (modprobe + mount), which the two prior tools didn't.
#
# Usage:
#   flash/build-mtd3-jffs2.sh <base-mtd3.jffs2> [output.jffs2]
#
# Env overrides (defaults match tools/build-and-deploy.sh / build-update-package.sh):
#   KERNEL_DIR   kernel-src/linux-7.1.4 checkout (gitignored, local only)
#   ERASEBLOCK   NAND eraseblock size (default 0x4000 = 16KiB, matches this
#                device's Samsung 128MiB part -- see docs/DEADLETTER-NAND-RECOVERY.md
#                and piko-install.c's ERASE_SIZE)
#
# Needs root for the unpack step (tools/jffs2-mount-extract.sh) -- either
# run this whole script with sudo, or ensure passwordless sudo is set up
# for that one script; it shells out via `sudo` internally either way.

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BASE_JFFS2="${1:?usage: flash/build-mtd3-jffs2.sh <base-mtd3.jffs2> [output.jffs2]}"
OUT="${2:-$REPO/flash/mtd3.jffs2}"

KERNEL_DIR="${KERNEL_DIR:-$REPO/kernel-src/linux-7.1.4}"
ERASEBLOCK="${ERASEBLOCK:-0x4000}"

if [ ! -f "$BASE_JFFS2" ]; then
    echo "build-mtd3-jffs2: base image not found: $BASE_JFFS2" >&2
    exit 1
fi
case "$(file -b "$BASE_JFFS2" 2>/dev/null)" in
    *jffs2*little*endian*) ;;
    *) echo "build-mtd3-jffs2: $BASE_JFFS2 doesn't look like a little-endian JFFS2 image, refusing to guess" >&2; exit 1 ;;
esac

if ! command -v mkfs.jffs2 >/dev/null 2>&1; then
    echo "build-mtd3-jffs2: mkfs.jffs2 not found (apt install mtd-utils)" >&2
    exit 1
fi

if [ ! -f "$KERNEL_DIR/arch/arm/boot/zImage" ]; then
    echo "build-mtd3-jffs2: $KERNEL_DIR has no built zImage -- build it first:" >&2
    echo "  cd $KERNEL_DIR && ARCH=arm CROSS_COMPILE=... make zImage modules" >&2
    exit 1
fi
KVER="$(cat "$KERNEL_DIR/include/config/kernel.release" 2>/dev/null || true)"
if [ -z "$KVER" ]; then
    echo "build-mtd3-jffs2: cannot determine kernel release (no include/config/kernel.release)" >&2
    exit 1
fi

STAGE="$(mktemp -d /tmp/piko-mtd3-jffs2.XXXXXX)"
trap 'rm -rf "$STAGE"' EXIT
OVERLAY="$STAGE/overlay"
mkdir -p "$OVERLAY"

echo "==> staging overlay: kernel ($KVER) + modules + rootfs/"
mkdir -p "$OVERLAY/boot"
cp "$KERNEL_DIR/arch/arm/boot/zImage" "$OVERLAY/boot/zImage-full"

# Same module set tools/chunked-deploy.sh and flash/build-update-package.sh
# ship -- shared via tools/kernel-modules.sh so there's only one place this
# list can go stale relative to the kernel .config.
. "$REPO/tools/kernel-modules.sh"
for relpath in $AUDIO_MODULES; do
    dst="$OVERLAY/lib/modules/$KVER/zaurus-audio/$(basename "$relpath")"
    mkdir -p "$(dirname "$dst")"
    cp "$KERNEL_DIR/$relpath" "$dst"
done

WIFI_PCMCIA_SPI_SD_MODULES="$WIFI_MODULES
$SPI_MODULES
$SD_MODULES"
for relpath in $WIFI_PCMCIA_SPI_SD_MODULES; do
    src_rel="$(echo "$relpath" | sed 's#^kernel/##')"
    dst="$OVERLAY/lib/modules/$KVER/$relpath"
    mkdir -p "$(dirname "$dst")"
    cp "$KERNEL_DIR/$src_rel" "$dst"
done

( cd "$REPO/rootfs" && find . -type f ) | sed 's#^\./##' | while read -r rel; do
    dst="$OVERLAY/$rel"
    mkdir -p "$(dirname "$dst")"
    cp "$REPO/rootfs/$rel" "$dst"
    mode="$(stat -c '%a' "$REPO/rootfs/$rel")"
    chmod "$mode" "$dst"
done

echo "==> unpacking base image $BASE_JFFS2 (via the real kernel jffs2 driver -- needs sudo)"
MERGED="$STAGE/merged"
sudo "$REPO/tools/jffs2-mount-extract.sh" "$BASE_JFFS2" "$MERGED"

echo "==> overlaying kernel + modules + rootfs/ on top of the unpacked base"
cp -a "$OVERLAY/." "$MERGED/"

echo "==> building fresh image from merged tree (eraseblock=$ERASEBLOCK)"
# -U/--squash-uids: the unpack above can't preserve the original root
# ownership (jffs2reader needs no privilege to read, but we have none to
# chown to arbitrary uid/gid on extraction) -- force everything back to
# root:root at pack time instead, which is what every file in this image
# actually needs to be owned as on the real device.
#
# -n/--no-cleanmarkers (2026-07-30, found from real hardware's own kernel
# log): fs/jffs2/os-linux.h defines jffs2_cleanmarker_oob(c) as
# `c->mtd->type == MTD_NANDFLASH` -- true for this device, meaning the
# running kernel expects clean markers written in the OOB area, not
# inline in the data area. mkfs.jffs2's default behavior writes an
# inline CLEANMARKER node to every eraseblock, which the NAND-aware scan
# correctly flags as wrong-format noise on every single eraseblock at
# boot ("CLEANMARKER node found ... has totlen 0xc != normal 0x10") --
# harmless (jffs2 falls back to treating the block as normal data and
# still mounts), but very noisy, and not what should be shipped. -n
# suppresses the inline markers entirely; the kernel manages OOB-based
# clean marking itself as it erases/reuses blocks at runtime.
mkfs.jffs2 -r "$MERGED" -o "$OUT.partial" \
    -e "$ERASEBLOCK" -l -U -n -q -v 2>&1 | tail -20
mv "$OUT.partial" "$OUT"

md5sum "$BASE_JFFS2" "$OUT"
echo "==> done: $OUT ($(stat -c '%s' "$OUT") bytes, base was $(stat -c '%s' "$BASE_JFFS2") bytes)"
