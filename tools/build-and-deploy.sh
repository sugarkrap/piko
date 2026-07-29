#!/bin/sh
set -eu

# Cross-compiles the stage-2 kernel + all modules with our buildroot
# toolchain, then deploys the result (zImage + sound modules + WiFi/PCMCIA
# modules + helper scripts) to a reachable Zaurus over SSH by calling
# chunked-deploy.sh. This is the ROUTINE path for updating the running
# "home"-partition kernel: no NAND flash, no SD card, no recovery menu,
# no reboot to a service menu. See docs/HOWTO-BUILD-DEPLOY-KERNEL.md.
#
# kernel-src/ itself is reconstructed by flash/setup-kernel-src.sh before
# every build (download a pristine kernel.org tarball + apply every
# tracked patch under modules/, including the mach-pxa/wireless/crypto
# Kconfig+Makefile wiring) rather than assumed to already exist -- same
# pipeline flash/build-update-package.sh and CI use. It's idempotent
# (a marker file skips all of this once a tree is already patched), so
# this is cheap to call on every run; pass --force-kernel-src below if
# you've changed one of the tracked patch files and need it re-applied.
#
# This requires the device to already be reachable over SSH (WiFi up).
# If it is NOT reachable (bricked, unbootable, or WiFi itself broken), or
# if the BOOTSTRAP partition (mtd1/smf) itself needs to change, use the
# SD-card recovery flash procedure instead: docs/FLASH-MTD1-MTD3-SAFE.md
# -- that path is deliberately not automated here, per AGENTS.md ("this is
# the last spare board", never combine mtd1/mtd3 passes).
#
# Usage:
#   tools/build-and-deploy.sh [--adapter IFACE] [--force-kernel-src] [--kernel-only] [user@host]
# Example:
#   tools/build-and-deploy.sh --adapter wlan0 root@10.43.112.72
#
# --adapter IFACE binds the SSH connection to a specific local network
# interface (ssh -B), useful when the build machine has multiple network
# adapters and the Zaurus is only reachable via one of them.
# --force-kernel-src forces tools/setup-kernel-src.sh to re-apply every
# tracked patch even if kernel-src/ already looks patched -- use this if
# you've changed one of the tracked patch files under modules/.
# --kernel-only builds only zImage (skips `make modules`) and forwards
# --kernel-only to chunked-deploy.sh, which then only ships
# /boot/zImage-full and skips every module/script/helper deploy step.
# Faster iteration when you're only touching kernel/.config, e.g. verifying
# a JFFS2 compressor fix, and don't need to redeploy unchanged modules.

ADAPTER=""
FORCE_KERNEL_SRC=0
KERNEL_ONLY=0
TARGET=""
while [ $# -gt 0 ]; do
    case "$1" in
        --adapter)
            ADAPTER="$2"
            shift 2
            ;;
        --force-kernel-src)
            FORCE_KERNEL_SRC=1
            shift
            ;;
        --kernel-only)
            KERNEL_ONLY=1
            shift
            ;;
        *)
            TARGET="$1"
            shift
            ;;
    esac
done
TARGET="${TARGET:-root@10.43.112.72}"
KEY="${HOME}/.ssh/zaurus_ed25519"
SSH_OPTS="-o BatchMode=yes -o ConnectTimeout=30 -o ServerAliveInterval=15 -o ServerAliveCountMax=8 -o StrictHostKeyChecking=accept-new"
if [ -n "$ADAPTER" ]; then
    SSH_OPTS="$SSH_OPTS -B $ADAPTER"
fi
REPO="$(cd "$(dirname "$0")/.." && pwd)"
KERNEL_DIR="$REPO/kernel-src/linux-7.1.4"
TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-/home/makaron/Code/dosbox-armv5-zaurus/buildroot/output/host/bin}"
BUILD_LOG="/tmp/kbuild-$(date +%Y%m%d-%H%M%S).log"
JOBS="$(nproc 2>/dev/null || echo 4)"

echo "==> checking $TARGET is reachable over SSH before spending time building..."
if ! ssh $SSH_OPTS -i "$KEY" "$TARGET" "uname -a"; then
    echo "FAILED: $TARGET is not reachable over SSH." >&2
    echo "This script only handles the routine SSH-based redeploy path." >&2
    echo "If the device is unreachable/unbootable, or you need to change" >&2
    echo "the bootstrap partition (mtd1/smf), use the recovery flash" >&2
    echo "procedure instead: docs/FLASH-MTD1-MTD3-SAFE.md" >&2
    exit 1
fi

echo "==> reconstructing kernel-src (download + apply tracked patches)..."
if [ "$FORCE_KERNEL_SRC" -eq 1 ]; then
    "$REPO/tools/setup-kernel-src.sh" --force
else
    "$REPO/tools/setup-kernel-src.sh"
fi

if [ -n "${TOOLCHAIN_BIN_DIR}" ] && [ -d "$TOOLCHAIN_BIN_DIR" ]; then
    PATH="$TOOLCHAIN_BIN_DIR:$PATH"
fi

if [ -z "${CROSS_COMPILE:-}" ]; then
    for prefix in arm-buildroot-linux-uclibcgnueabi- arm-unknown-linux-uclibcgnueabi- arm-linux-gnueabi- arm-unknown-linux-gnueabi-; do
        if command -v "${prefix}gcc" >/dev/null 2>&1; then
            CROSS_COMPILE="$prefix"
            break
        fi
    done
fi

if [ -z "${CROSS_COMPILE:-}" ]; then
    echo "FAILED: no ARM cross compiler found in PATH." >&2
    echo "Expected one of: arm-buildroot-linux-uclibcgnueabi-gcc, arm-unknown-linux-uclibcgnueabi-gcc, arm-linux-gnueabi-gcc, arm-unknown-linux-gnueabi-gcc" >&2
    echo "Set TOOLCHAIN_BIN_DIR to your toolchain bin path, or export CROSS_COMPILE explicitly." >&2
    exit 1
fi

echo "==> using cross-compiler prefix: $CROSS_COMPILE"

if [ "$KERNEL_ONLY" -eq 1 ]; then
    BUILD_TARGETS="zImage"
    echo "==> --kernel-only: building zImage only (skipping modules) with -j$JOBS (full log: $BUILD_LOG)..."
else
    BUILD_TARGETS="zImage modules"
    echo "==> building zImage + modules with -j$JOBS (full log: $BUILD_LOG)..."
fi
if ! (
    cd "$KERNEL_DIR"
    export PATH
    export ARCH=arm CROSS_COMPILE
    make -j"$JOBS" $BUILD_TARGETS
) > "$BUILD_LOG" 2>&1; then
    echo "FAILED: build did not complete. Last 40 lines of $BUILD_LOG:" >&2
    tail -40 "$BUILD_LOG" >&2
    echo "" >&2
    echo "Full log at $BUILD_LOG -- grep -in error there yourself too," >&2
    echo "the real failing line is often much earlier than the final" >&2
    echo "'Error 2' summary in a parallel (-j) build." >&2
    exit 1
fi
echo "==> build OK"

echo "==> cross-compiling userspace/src/md5sum.c (deployed first, so every"
echo "    subsequent file transfer can be content-verified, not just size-checked)..."
GCC="${CROSS_COMPILE}gcc"
if ! "$GCC" -march=armv5te -O2 -static -Wall -Wextra \
        -o "$REPO/userspace/src/md5sum" "$REPO/userspace/src/md5sum.c"; then
    echo "FAILED: could not build userspace/src/md5sum" >&2
    exit 1
fi
STRIP="${GCC%gcc}strip"
if command -v "$STRIP" >/dev/null 2>&1; then
    "$STRIP" "$REPO/userspace/src/md5sum"
fi
echo "==> md5sum build OK"

if [ "$KERNEL_ONLY" -eq 1 ]; then
    echo "==> deploying to $TARGET (zImage only)..."
else
    echo "==> deploying to $TARGET (zImage + sound + WiFi/PCMCIA modules)..."
fi
set -- "$TARGET"
if [ -n "$ADAPTER" ]; then
    set -- --adapter "$ADAPTER" "$TARGET"
fi
if [ "$KERNEL_ONLY" -eq 1 ]; then
    set -- --kernel-only "$@"
fi
export REPO KERNEL_DIR
exec "$REPO/tools/chunked-deploy.sh" "$@"
