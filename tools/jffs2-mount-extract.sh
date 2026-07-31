#!/bin/bash
set -euo pipefail

# Unpacks a JFFS2 image reliably, using the REAL Linux kernel JFFS2 driver
# via a loopback MTD RAM device -- not jffs2reader.
#
# WHY NOT jffs2reader (mtd-utils): confirmed 2026-07-30, on this project's
# actual mtd3.jffs2 (home partition base image), jffs2reader SILENTLY
# corrupts substantial files (every ELF binary/kernel module over roughly
# 50KB tested) while still exiting 0 -- the extracted file has the RIGHT
# byte count but its content is garbage (e.g. every ELF section header
# entry zeroed out). This is NOT the same as jffs2reader's other, LOUD
# failure mode ("File does not fit into buffer!" for very large files,
# which at least errors visibly) -- this is a silent, undetected
# corruption of moderately-sized files, which is far more dangerous: a
# build using jffs2reader can complete cleanly, look structurally correct
# (jffs2dump still shows the right dirents/inodes -- it only reads node
# METADATA, never decompresses content), and still ship a home partition
# whose kexec/dropbear/wpa_supplicant/etc. binaries silently segfault on
# real hardware. This cost a full combined mtd1+mtd3 flash + a kexec
# segfault on real hardware to catch.
#
# This script instead: loads a real mtdram device sized to the image,
# writes the image into it, mounts it with the kernel's own jffs2 driver
# (the exact same code that runs the real device), and copies the
# mounted files out with plain `cp -a`. Slower and needs root, but this
# is the one method proven to produce byte-correct output.
#
# Usage:
#   sudo tools/jffs2-mount-extract.sh <image.jffs2> <dest-dir>
#
# Requires: root (modprobe + mount), and the mtdram/mtdblock/jffs2 kernel
# modules available (standard on most distro kernels; on Debian/Ubuntu
# CI runners these ship in linux-modules-extra or are already loaded).

if [ "$(id -u)" -ne 0 ]; then
    echo "jffs2-mount-extract: must run as root (modprobe + mount) -- try: sudo $0 $*" >&2
    exit 1
fi

IMAGE="${1:?usage: jffs2-mount-extract.sh <image.jffs2> <dest-dir>}"
DEST="${2:?usage: jffs2-mount-extract.sh <image.jffs2> <dest-dir>}"

if [ ! -f "$IMAGE" ]; then
    echo "jffs2-mount-extract: image not found: $IMAGE" >&2
    exit 1
fi

MNT="$(mktemp -d /tmp/jffs2-mount-extract-mnt.XXXXXX)"
cleanup() {
    umount "$MNT" 2>/dev/null || true
    rmdir "$MNT" 2>/dev/null || true
    rmmod mtdblock mtdram 2>/dev/null || true
}
trap cleanup EXIT

# Clean up any stale mtdram/mtdblock state from a previous (possibly
# interrupted) run before loading fresh with the size this image needs.
rmmod mtdblock mtdram 2>/dev/null || true

SIZE_BYTES="$(stat -c '%s' "$IMAGE")"
SIZE_KB=$(( (SIZE_BYTES + 1023) / 1024 ))

modprobe mtdram total_size="$SIZE_KB" erase_size=16
modprobe mtdblock
modprobe jffs2

MTDNUM=""
for f in /sys/class/mtd/mtd*/name; do
    if grep -q "mtdram" "$f" 2>/dev/null; then
        MTDNUM="$(echo "$f" | grep -oE 'mtd[0-9]+')"
        break
    fi
done
if [ -z "$MTDNUM" ]; then
    echo "jffs2-mount-extract: could not find the mtdram device under /sys/class/mtd" >&2
    exit 1
fi
echo "jffs2-mount-extract: using $MTDNUM"

dd if="$IMAGE" of="/dev/$MTDNUM" bs=4096 conv=notrunc status=none

BLOCKDEV="/dev/mtdblock${MTDNUM#mtd}"
for _ in $(seq 1 20); do
    [ -b "$BLOCKDEV" ] && break
    sleep 0.2
done
if [ ! -b "$BLOCKDEV" ]; then
    echo "jffs2-mount-extract: block device $BLOCKDEV never appeared" >&2
    exit 1
fi

mount -t jffs2 "$BLOCKDEV" "$MNT" -o ro

mkdir -p "$DEST"
cp -a "$MNT/." "$DEST/"
if [ -n "${SUDO_USER:-}" ]; then
    chown -R "$SUDO_USER:$SUDO_USER" "$DEST"
fi

echo "jffs2-mount-extract: extracted $(find "$DEST" | wc -l) entries from $IMAGE -> $DEST"
