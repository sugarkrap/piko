#!/bin/sh
set -eu

REPO="$(cd "$(dirname "$0")/../.." && pwd)"

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
mkfs.ext2 -h 2>&1 | grep -q -- "-d " || {
    echo "$0: this mke2fs has no -d (need e2fsprogs >= 1.43)" >&2; exit 1; }

BASE="$REPO/initramfs/rootfs"
XSTAGE="$REPO/userspace/stage-target"
[ -d "$BASE" ]   || { echo "$0: no $BASE (run tools/kernel/build-initramfs.sh)" >&2; exit 1; }
[ -d "$XSTAGE" ] || { echo "$0: no $XSTAGE (run tools/userspace/build-userspace.sh)" >&2; exit 1; }

TREE="$REPO/.emulator-sd-root"
rm -rf "$TREE"; mkdir -p "$TREE"
[ "$KEEP_TREE" = 1 ] || trap 'rm -rf "$TREE"' EXIT

echo "==> staging base userland + X11 stack + rootfs overlay"
cp -a "$BASE/."   "$TREE"/
cp -a "$XSTAGE/." "$TREE"/
rm -rf "$TREE/.piko-build-stamps" "$TREE/usr/lib/cmake" "$TREE/usr/include"
find "$TREE/usr/lib" -name '*.a' -delete 2>/dev/null || true
cp -a "$REPO/rootfs/." "$TREE"/

mkdir -p "$TREE"/dev "$TREE"/proc "$TREE"/sys "$TREE"/tmp "$TREE"/var/log \
         "$TREE"/sbin "$TREE"/usr/local/bin
[ -e "$TREE/sbin/init" ] || ln -sf ../bin/busybox "$TREE/sbin/init"
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
truncate -s "$SIZE" "$OUT"
mkfs.ext2 -F -q -d "$TREE" "$OUT"

echo "==> wrote $OUT ($(du -h "$OUT" | cut -f1) on disk)"
echo "    boot it with: tools/emulator/build-and-emulate.sh --sd $OUT"
