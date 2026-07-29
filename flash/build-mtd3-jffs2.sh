#!/bin/sh
set -eu

# Rebuilds mtd3.jffs2 (the "home" partition image for SD-card recovery
# flashing, see docs/FLASH-MTD1-MTD3-SAFE.md) by taking an EXISTING
# mtd3.jffs2 as a base and appending an incremental update containing the
# latest kernel + modules + rootfs/ overlay, via mkfs.jffs2's -i/--incremental
# mode: it reads the base image to learn existing inode/version numbers and
# emits only new nodes for the paths in the overlay tree, which win over the
# base image's older versions of the same paths once concatenated.
#
# This project has no full base userland (busybox, /bin, /lib, device
# nodes, etc.) committed anywhere -- rootfs/ here is only the same overlay
# tools/build-and-deploy.sh and flash/build-update-package.sh push onto a
# live device. The only place a full "home" tree currently exists is inside
# a previously-built mtd3.jffs2 already staged somewhere (e.g. on the SD
# card) -- that's why this is incremental-onto-an-existing-image rather
# than a from-nothing rootfs build.
#
# Usage:
#   flash/build-mtd3-jffs2.sh <base-mtd3.jffs2> [output.jffs2]
#
# Env overrides (defaults match tools/build-and-deploy.sh / build-update-package.sh):
#   KERNEL_DIR   kernel-src/linux-7.1.4 checkout (gitignored, local only)
#   ERASEBLOCK   NAND eraseblock size (default 0x4000 = 16KiB, matches this
#                device's Samsung 128MiB part -- see docs/DEADLETTER-NAND-RECOVERY.md
#                and piko-install.c's ERASE_SIZE)

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

echo "==> generating incremental appendage against $BASE_JFFS2 (eraseblock=$ERASEBLOCK)"
APPEND="$STAGE/appendage.jffs2"
mkfs.jffs2 -r "$OVERLAY" -i "$BASE_JFFS2" -o "$APPEND" \
    -e "$ERASEBLOCK" -l -q -v 2>&1 | tail -20

echo "==> concatenating base + appendage -> $OUT"
cat "$BASE_JFFS2" "$APPEND" > "$OUT"

md5sum "$BASE_JFFS2" "$APPEND" "$OUT"
echo "==> done: $OUT ($(stat -c '%s' "$OUT") bytes, base was $(stat -c '%s' "$BASE_JFFS2") bytes)"
