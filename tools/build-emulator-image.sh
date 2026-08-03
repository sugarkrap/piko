#!/bin/sh
set -eu

# Assembles a bootable SD-card image for the QEMU emulator
# (tools/build-and-emulate.sh --sd), containing a full root filesystem rather
# than the throwaway initramfs the smoke test uses.
#
# WHY AN SD IMAGE AND NOT AN INITRAMFS:
# `-M husky` hard-codes 64 MB of guest RAM, same as real hardware, and an
# initramfs is unpacked into RAM. The graphical userland is ~95 MB before it
# is even stripped, so it cannot ever fit. pxa255_init() already instantiates
# the PXA MMC controller and picks up `drive_get(IF_SD, 0, 0)`, so an SD image
# needs no extra device modelling -- just `-drive if=sd,format=raw,file=...`.
#
# WHAT GOES IN, AND IN WHICH ORDER (later layers win):
#   1. initramfs/rootfs        busybox base userland: /bin, /sbin, /lib
#   2. userspace/stage-target  the X11 stack (Xfbdev, matchbox-*, libs)
#   3. rootfs/                 piko's own overlay: inittab, init.d/xsession...
#
# Note that piko has no full base userland committed anywhere -- see
# flash/build-mtd3-jffs2.sh's header, which unpacks an existing mtd3.jffs2 for
# exactly this reason. initramfs/rootfs is used here because it is
# regenerable from tools/build-initramfs.sh, unlike a device image.
#
# FILE MODES COME FROM THE MANIFEST, NOT FROM GIT:
# rootfs/etc/init.d/xsession is committed 0644 and is made executable only at
# deploy time, by piko-sync-deploy reading its manifest.yaml (`mode: "0755"`).
# A plain `cp` therefore produces an image whose init loops forever printing
# "can't run '/etc/init.d/xsession': Permission denied". This script applies
# the manifest's own modes so the image matches what a real deploy produces,
# and keeps matching it when the manifest changes.
#
# KNOWN GAP -- THIS DOES NOT YET REACH A MATCHBOX DESKTOP:
# xsession runs and then reports
#   "graphical session unavailable: /usr/bin/matchbox-session missing"
# because /usr/bin/matchbox-session comes from matchbox-common, which is not
# in userspace/stage-target. tools/build-matchbox-payload.sh installs only
# piko's *session script* (modules/x11/matchbox-session -> /etc/matchbox/session,
# read BY that binary) into a throwaway /tmp payload dir. Closing this means
# running piko's X11 build chain (tools/build-x11-stack.sh +
# tools/build-matchbox-payload.sh) and staging matchbox-common into the image;
# everything up to that point -- SD root mount, busybox init, inittab, xsession
# exec -- is verified working under QEMU.
#
# Usage:
#   tools/build-emulator-image.sh [--out IMAGE] [--size 512M] [--keep-tree]

REPO="$(cd "$(dirname "$0")/.." && pwd)"

OUT="$REPO/piko-emulator-sd.img"
SIZE=512M
KEEP_TREE=0

while [ $# -gt 0 ]; do
    case "$1" in
        --out)       OUT="$2"; shift 2 ;;
        --size)      SIZE="$2"; shift 2 ;;
        --keep-tree) KEEP_TREE=1; shift ;;
        -h|--help)   sed -n '3,45p' "$0"; exit 0 ;;
        *) echo "$0: unknown argument '$1' (try --help)" >&2; exit 1 ;;
    esac
done

command -v mkfs.ext2 >/dev/null 2>&1 || {
    echo "$0: mkfs.ext2 not found (apt install e2fsprogs)" >&2; exit 1; }
# mke2fs -d populates the filesystem from a directory entirely in userspace,
# so none of this needs root or a loop mount.
mkfs.ext2 -h 2>&1 | grep -q -- "-d " || {
    echo "$0: this mke2fs has no -d (need e2fsprogs >= 1.43)" >&2; exit 1; }

BASE="$REPO/initramfs/rootfs"
XSTAGE="$REPO/userspace/stage-target"
[ -d "$BASE" ]   || { echo "$0: no $BASE (run tools/build-initramfs.sh)" >&2; exit 1; }
[ -d "$XSTAGE" ] || { echo "$0: no $XSTAGE (run tools/build-userspace.sh)" >&2; exit 1; }

TREE="$REPO/.emulator-sd-root"
rm -rf "$TREE"; mkdir -p "$TREE"
[ "$KEEP_TREE" = 1 ] || trap 'rm -rf "$TREE"' EXIT

echo "==> staging base userland + X11 stack + rootfs overlay"
cp -a "$BASE/."   "$TREE"/
cp -a "$XSTAGE/." "$TREE"/
# Build-time only: static libs, headers and cmake packages are dead weight in
# an image that has to fit on a card and be read by a 400 MHz CPU.
rm -rf "$TREE/.piko-build-stamps" "$TREE/usr/lib/cmake" "$TREE/usr/include"
find "$TREE/usr/lib" -name '*.a' -delete 2>/dev/null || true
cp -a "$REPO/rootfs/." "$TREE"/

mkdir -p "$TREE"/dev "$TREE"/proc "$TREE"/sys "$TREE"/tmp "$TREE"/var/log \
         "$TREE"/sbin "$TREE"/usr/local/bin
# busybox init: the kernel is told init=/sbin/init, and CONFIG_DEVTMPFS_MOUNT
# means /dev is populated for us, so no device nodes are needed in the image.
[ -e "$TREE/sbin/init" ] || ln -sf ../bin/busybox "$TREE/sbin/init"
# xsession looks for the X server at /usr/local/bin/Xfbdev; the stage puts it
# in /usr/bin. Bridge the two rather than editing a file the device also uses.
[ -e "$TREE/usr/local/bin/Xfbdev" ] || ln -sf ../../bin/Xfbdev "$TREE/usr/local/bin/Xfbdev"

echo "==> applying file modes from piko-sync-deploy's manifest"
python3 - "$REPO" "$TREE" <<'PY'
import os, re, sys
repo, tree = sys.argv[1], sys.argv[2]
manifest = os.path.join(repo, "userspace/src/piko-sync-deploy/manifest.yaml")
if not os.path.exists(manifest):
    print("   (no manifest found; leaving modes as copied)"); raise SystemExit(0)
applied = 0
for m in re.finditer(r'remote:\s*(\S+)\s*\n\s*mode:\s*"(\d+)"', open(manifest).read()):
    remote, mode = m.group(1), int(m.group(2), 8)
    path = os.path.join(tree, remote.lstrip("/"))
    if os.path.isfile(path):
        os.chmod(path, mode)
        applied += 1
print("   applied %d modes" % applied)
PY

echo "==> building ext2 image ($SIZE)"
rm -f "$OUT"
# QEMU rejects an SD image whose size is not a power of two, so SIZE must be
# one; truncate gives a sparse file, and mke2fs only writes what it needs.
truncate -s "$SIZE" "$OUT"
mkfs.ext2 -F -q -d "$TREE" "$OUT"

echo "==> wrote $OUT ($(du -h "$OUT" | cut -f1) on disk)"
echo "    boot it with: tools/build-and-emulate.sh --sd $OUT"
