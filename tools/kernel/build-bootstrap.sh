#!/bin/sh
set -eu

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
KERNEL_VERSION="${KERNEL_VERSION:-7.1.4}"
KERNEL_SRC_DIR="${KERNEL_SRC_DIR:-$REPO/kernel-src-bootstrap}"
KERNEL_DIR="$KERNEL_SRC_DIR/linux-$KERNEL_VERSION"
INITRAMFS_CPIO="${INITRAMFS_CPIO:-$REPO/initramfs/initramfs-minimal-built.cpio.gz}"

TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin}"
JOBS="${JOBS:-$(command -v nproc >/dev/null 2>&1 && nproc || echo 4)}"

FORCE=0
OUT="$REPO/flash/zImage"
for arg in "$@"; do
    case "$arg" in
        --force) FORCE=1 ;;
        *) OUT="$arg" ;;
    esac
done

if [ ! -f "$INITRAMFS_CPIO" ]; then
    echo "==> no built initramfs at $INITRAMFS_CPIO -- building it"
    "$REPO/tools/kernel/build-initramfs.sh"
fi
if [ ! -f "$INITRAMFS_CPIO" ]; then
    echo "tools/kernel/build-bootstrap.sh: still no $INITRAMFS_CPIO after build-initramfs.sh -- check OUT_CPIO" >&2
    exit 1
fi

echo "==> preparing $KERNEL_DIR from kernel.config-corgi-$KERNEL_VERSION-minimal"
FORCE_ARG=""
[ "$FORCE" -eq 1 ] && FORCE_ARG="--force"
KERNEL_SRC_DIR="$KERNEL_SRC_DIR" \
KERNEL_CONFIG="kernel.config-corgi-$KERNEL_VERSION-minimal" \
    "$REPO/tools/kernel/setup-kernel-src.sh" $FORCE_ARG

if [ -n "${TOOLCHAIN_BIN_DIR}" ] && [ -d "$TOOLCHAIN_BIN_DIR" ]; then
    PATH="$TOOLCHAIN_BIN_DIR:$PATH"
fi
if [ -z "${CROSS_COMPILE:-}" ]; then
    for prefix in arm-unknown-linux-uclibcgnueabi- arm-buildroot-linux-uclibcgnueabi- arm-linux-gnueabi- arm-unknown-linux-gnueabi-; do
        if command -v "${prefix}gcc" >/dev/null 2>&1; then
            CROSS_COMPILE="$prefix"
            break
        fi
    done
fi
if [ -z "${CROSS_COMPILE:-}" ]; then
    echo "tools/kernel/build-bootstrap.sh: no ARM cross compiler found in PATH." >&2
    echo "Set TOOLCHAIN_BIN_DIR to your toolchain bin path, or export CROSS_COMPILE explicitly." >&2
    exit 1
fi

echo "==> pointing CONFIG_INITRAMFS_SOURCE at $INITRAMFS_CPIO"
( cd "$KERNEL_DIR" && ./scripts/config --set-str CONFIG_INITRAMFS_SOURCE "$INITRAMFS_CPIO" )
( cd "$KERNEL_DIR" && make ARCH=arm CROSS_COMPILE="$CROSS_COMPILE" olddefconfig >/dev/null )

echo "==> building the bootstrap zImage (-j$JOBS)"
( cd "$KERNEL_DIR" && make ARCH=arm CROSS_COMPILE="$CROSS_COMPILE" -j"$JOBS" zImage )

if [ ! -f "$KERNEL_DIR/arch/arm/boot/zImage" ]; then
    echo "tools/kernel/build-bootstrap.sh: build finished but no arch/arm/boot/zImage -- something failed silently" >&2
    exit 1
fi

mkdir -p "$(dirname "$OUT")"
cp "$KERNEL_DIR/arch/arm/boot/zImage" "$OUT.partial"
mv "$OUT.partial" "$OUT"

SIZE="$(stat -c '%s' "$OUT")"
echo ""
echo "==> done: $OUT ($SIZE bytes)"
if [ "$SIZE" -gt 1294336 ]; then
    echo "tools/kernel/build-bootstrap.sh: WARNING -- $OUT is $SIZE bytes, over the" >&2
    echo "  1294336-byte mtd1 slot budget. It will not flash. Trim the minimal config" >&2
    echo "  or the initramfs (see modules/initramfs/) before shipping this." >&2
fi
