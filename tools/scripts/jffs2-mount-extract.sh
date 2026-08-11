#!/bin/bash
set -euo pipefail

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
