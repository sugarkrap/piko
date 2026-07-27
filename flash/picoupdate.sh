#!/bin/sh
# In-system SMF updater for cases where recovery menu is unavailable.
# Usage:
#   picoupdate.sh [--dry-run] /path/to/smf-kernel-zImage [/path/to/initramfs.cpio.gz]

set -e

DRY_RUN=0
if [ "$1" = "--dry-run" ]; then
    DRY_RUN=1
    shift
fi

IMAGE="$1"
INITRAMFS="$2"
SMF_OFFSET=917504
SMF_MAX=1294336
CHUNK_SIZE=524288

if [ -z "$IMAGE" ]; then
    echo "usage: $0 [--dry-run] /path/to/smf-kernel-zImage [/path/to/initramfs.cpio.gz]"
    exit 1
fi

if [ ! -f "$IMAGE" ]; then
    echo "error: image not found: $IMAGE"
    exit 1
fi

SMF_LINE="$(cat /proc/mtd | grep '"smf"' | head -n 1)"
SMF_MTD="${SMF_LINE%%:*}"
if [ -z "$SMF_MTD" ]; then
    echo "error: could not find smf entry in /proc/mtd"
    exit 1
fi
SMF_DEV="/dev/${SMF_MTD}"

IMAGE_SIZE="$(wc -c < "$IMAGE")"
if [ "$IMAGE_SIZE" -gt "$SMF_MAX" ]; then
    echo "error: image too large for smf kernel slot (${IMAGE_SIZE} > ${SMF_MAX})"
    exit 1
fi

BACKUP="/boot/smf-backup-pre-picoup.bin"

have_cacko_tools=0
if [ -x /sbin/nandlogical ] && [ -x /sbin/bcut ]; then
    have_cacko_tools=1
fi

have_pico_writer=0
if [ -x /usr/sbin/pico-smf-write ]; then
    have_pico_writer=1
fi

if [ "$have_cacko_tools" -eq 0 ] && [ "$have_pico_writer" -eq 0 ]; then
    echo "error: neither Cacko nandlogical tools nor /usr/sbin/pico-smf-write are available"
    exit 1
fi

echo "[picoupdate] smf device: $SMF_DEV"
echo "[picoupdate] image: $IMAGE"
echo "[picoupdate] backup: $BACKUP"
if [ "$have_cacko_tools" -eq 1 ]; then
    echo "[picoupdate] writer: cacko-nandlogical"
else
    echo "[picoupdate] writer: pico-smf-write"
fi
if [ "$DRY_RUN" -eq 1 ]; then
    echo "[picoupdate] dry run: no NAND writes will occur"
fi

if [ "$DRY_RUN" -eq 1 ]; then
    echo "[picoupdate] planned write: offset=$SMF_OFFSET max=$SMF_MAX image_bytes=$IMAGE_SIZE"
    echo "[picoupdate] planned backup: $BACKUP"
    if [ "$have_cacko_tools" -eq 1 ]; then
        echo "[picoupdate] planned method: /sbin/bcut + /sbin/nandlogical WRITE in ${CHUNK_SIZE}-byte chunks"
    else
        echo "[picoupdate] planned method: /usr/sbin/pico-smf-write (FTL-aware MEMWRITE AUTO_OOB)"
    fi
    if [ -n "$INITRAMFS" ]; then
        echo "[picoupdate] planned initramfs copy to /boot/initramfs-minimal.cpio.gz and /boot/initrd"
    fi
    exit 0
fi

if [ "$have_cacko_tools" -eq 1 ]; then
    TMPDIR="/tmp/picoupdate"
    TMPCHUNK="$TMPDIR/tmpdata.bin"
    ADDR="$SMF_OFFSET"
    DATAPOS=0

    mkdir -p "$TMPDIR"

    echo "[picoupdate] writing with cacko-style nandlogical"
    while [ "$DATAPOS" -lt "$IMAGE_SIZE" ]; do
        /sbin/bcut -a "$DATAPOS" -s "$CHUNK_SIZE" -o "$TMPCHUNK" "$IMAGE"
        TMPSIZE="$(wc -c < "$TMPCHUNK")"
        /sbin/nandlogical "$SMF_DEV" WRITE "$ADDR" "$TMPSIZE" "$TMPCHUNK" >/dev/null 2>&1
        DATAPOS=$((DATAPOS + TMPSIZE))
        ADDR=$((ADDR + TMPSIZE))
        rm -f "$TMPCHUNK"
        echo "[picoupdate] wrote ${DATAPOS}/${IMAGE_SIZE}"
    done
else
    /usr/sbin/pico-smf-write "$SMF_DEV" "$IMAGE" "$SMF_OFFSET" "$SMF_MAX" "$BACKUP"
fi

if [ -n "$INITRAMFS" ]; then
    if [ ! -f "$INITRAMFS" ]; then
        echo "error: initramfs file not found: $INITRAMFS"
        exit 1
    fi

    echo "[picoupdate] updating initramfs payload copy"
    cp "$INITRAMFS" /boot/initramfs-minimal.cpio.gz
    cp "$INITRAMFS" /boot/initrd
fi

sync
sync

echo "[picoupdate] done"
