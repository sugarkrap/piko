#!/bin/sh
set -eu

# Cross-compiles the stage-2 kernel + all modules with our buildroot
# toolchain, then deploys the result (zImage + sound modules + WiFi/PCMCIA
# modules + helper scripts) to a reachable Zaurus over SSH by calling
# chunked-deploy.sh. This is the ROUTINE path for updating the running
# "home"-partition kernel: no NAND flash, no SD card, no recovery menu,
# no reboot to a service menu. See docs/HOWTO-BUILD-DEPLOY-KERNEL.md.
#
# This requires the device to already be reachable over SSH (WiFi up).
# If it is NOT reachable (bricked, unbootable, or WiFi itself broken), or
# if the BOOTSTRAP partition (mtd1/smf) itself needs to change, use the
# SD-card recovery flash procedure instead: flash/FLASH-MTD1-MTD3-SAFE.md
# -- that path is deliberately not automated here, per AGENTS.md ("this is
# the last spare board", never combine mtd1/mtd3 passes).
#
# Usage:
#   flash/build-and-deploy.sh [user@host]
# Example:
#   flash/build-and-deploy.sh root@10.43.112.72

TARGET="${1:-root@10.43.112.72}"
KEY="${HOME}/.ssh/zaurus_ed25519"
SSH_OPTS="-o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=accept-new"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
KERNEL_DIR="$REPO/kernel-src/linux-7.1.4"
TOOLCHAIN="/home/makaron/Code/dosbox-armv5-zaurus/buildroot/output/host/bin"
BUILD_LOG="/tmp/kbuild-$(date +%Y%m%d-%H%M%S).log"
JOBS="$(nproc 2>/dev/null || echo 4)"

echo "==> checking $TARGET is reachable over SSH before spending time building..."
if ! ssh $SSH_OPTS -i "$KEY" "$TARGET" "uname -a" 2>/dev/null; then
    echo "FAILED: $TARGET is not reachable over SSH." >&2
    echo "This script only handles the routine SSH-based redeploy path." >&2
    echo "If the device is unreachable/unbootable, or you need to change" >&2
    echo "the bootstrap partition (mtd1/smf), use the recovery flash" >&2
    echo "procedure instead: flash/FLASH-MTD1-MTD3-SAFE.md" >&2
    exit 1
fi

echo "==> building zImage + modules with -j$JOBS (full log: $BUILD_LOG)..."
if ! (
    cd "$KERNEL_DIR"
    export PATH="$TOOLCHAIN:$PATH"
    export ARCH=arm CROSS_COMPILE=arm-buildroot-linux-uclibcgnueabi-
    make -j"$JOBS" zImage modules
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

echo "==> deploying to $TARGET (zImage + sound + WiFi/PCMCIA modules)..."
exec "$REPO/flash/chunked-deploy.sh" "$TARGET"
