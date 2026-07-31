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
#   tools/build-and-deploy.sh [--adapter IFACE] [--force-kernel-src] [--kernel-only] [--skip-userspace] [--build-only] [user@host]
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
# --skip-userspace skips building the cross-compiled userspace
# (tools/build-userspace.sh: md5sum + ALSA + MPlayer + SDL + st + FLTK) and forwards
# --no-userspace to chunked-deploy.sh so it does not ship a stale staged
# payload either. The userspace build is idempotent and therefore cheap once
# built, so this is mainly for when the toolchain or a vendored source tree
# is in a knowingly broken state and you just need the kernel out.
# --build-only builds everything this script would normally build (kernel,
# modules, userspace, the X11/Matchbox payload) and then STOPS, without
# contacting the device at all -- no SSH reachability probe up front and no
# chunked-deploy.sh handoff at the end. No target argument is needed or used.
#
# That exists because CI has no device to deploy to, and because building
# and shipping are genuinely separate concerns: without it, the only way to
# exercise this build path was to also flash a board. Anything that wants
# "build exactly what a real deploy would build, then let me package it
# myself" -- CI, an offline update package, a dev with the board in a
# drawer -- should use this rather than reimplementing the build order and
# letting the two drift.

ADAPTER=""
FORCE_KERNEL_SRC=0
KERNEL_ONLY=0
SKIP_USERSPACE=0
SKIP_X11=0
BUILD_ONLY=0
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
        --skip-x11)
            SKIP_X11=1
            shift
            ;;
        --skip-userspace)
            SKIP_USERSPACE=1
            shift
            ;;
        --build-only)
            BUILD_ONLY=1
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
# The toolchain tools/build-uclibc-toolchain.sh produces, same default every
# other build script here uses. This used to point at one developer's
# buildroot checkout, which exists on no other machine -- including CI.
TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin}"
BUILD_LOG="/tmp/kbuild-$(date +%Y%m%d-%H%M%S).log"
JOBS="$(nproc 2>/dev/null || echo 4)"

# --build-only never touches the device, so the probe that exists purely to
# fail fast before a long build would be both pointless and, in CI, a
# guaranteed failure.
if [ "$BUILD_ONLY" -eq 1 ]; then
    echo "==> --build-only: building without contacting any device"
else
    echo "==> checking $TARGET is reachable over SSH before spending time building..."
    if ! ssh $SSH_OPTS -i "$KEY" "$TARGET" "uname -a"; then
        echo "FAILED: $TARGET is not reachable over SSH." >&2
        echo "This script only handles the routine SSH-based redeploy path." >&2
        echo "If the device is unreachable/unbootable, or you need to change" >&2
        echo "the bootstrap partition (mtd1/smf), use the recovery flash" >&2
        echo "procedure instead: docs/FLASH-MTD1-MTD3-SAFE.md" >&2
        exit 1
    fi
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

# Userspace (md5sum + ALSA + MPlayer + SDL + st + FLTK). Delegated to
# tools/build-userspace.sh
# rather than open-coded here -- it is the single entry point for every
# cross-built userspace component, and each step it runs is idempotent, so
# this is cheap on every subsequent invocation once things are built.
#
# md5sum in particular has to exist before chunked-deploy.sh runs: it is
# deployed first so every later transfer is content-verified rather than
# only byte-counted (silent truncation over this WiFi link is a real,
# repeatedly-observed failure mode, not a hypothetical).
#
# Skipped for --kernel-only, which deploys nothing but the zImage anyway.
if [ "$KERNEL_ONLY" -eq 1 ]; then
    echo "==> --kernel-only: skipping the userspace build"
elif [ "$SKIP_USERSPACE" -eq 1 ]; then
    echo "==> --skip-userspace: not building userspace components"
else
    echo "==> building userspace (md5sum + ALSA + MPlayer + SDL + st + FLTK) via tools/build-userspace.sh..."
    if ! (
        export PATH TOOLCHAIN_BIN_DIR CROSS_COMPILE
        sh "$REPO/tools/build-userspace.sh"
    ); then
        echo "FAILED: userspace build did not complete." >&2
        echo "Re-run tools/build-userspace.sh directly to see the full output," >&2
        echo "or pass --skip-userspace to deploy the kernel without it." >&2
        exit 1
    fi
    echo "==> userspace build OK"
fi

# --- X11 + Matchbox desktop -------------------------------------------
# Repackages the already-built X stack into the single tar that
# chunked-deploy ships (see section 9 there). This only *collects*: the
# X submodules and Matchbox components are built separately, because a
# from-scratch X build is long and almost never what you want on a
# routine kernel redeploy. See docs/HOWTO-MATCHBOX-DESKTOP.md.
#
# Missing pieces are not fatal here -- the payload script fails loudly if
# a component is absent, and a kernel-only or X-less deploy is a
# perfectly normal thing to want.
if [ "$KERNEL_ONLY" -eq 0 ] && [ "$SKIP_X11" -eq 0 ]; then
    echo "==> repacking the X11/Matchbox payload..."
    if sh "$REPO/tools/build-matchbox-payload.sh" > /tmp/x11-payload-build.log 2>&1; then
        echo "==> X11 payload OK ($(wc -c < /tmp/matchbox-payload.tar) bytes)"
    else
        echo "==> X11 payload NOT built -- deploying without it" >&2
        echo "    (see /tmp/x11-payload-build.log; pass --skip-x11 to silence)" >&2
        tail -3 /tmp/x11-payload-build.log >&2
    fi
fi

if [ "$BUILD_ONLY" -eq 1 ]; then
    echo ""
    echo "==> --build-only: everything built, nothing deployed."
    # Report what actually exists rather than what was requested: a step
    # that degraded (the X11 payload prints a warning and carries on) must
    # not be reported here as if it had succeeded.
    if [ -f "$KERNEL_DIR/arch/arm/boot/zImage" ]; then
        echo "    zImage:      $KERNEL_DIR/arch/arm/boot/zImage ($(wc -c < "$KERNEL_DIR/arch/arm/boot/zImage") bytes)"
    fi
    if [ -f /tmp/matchbox-payload.tar ]; then
        echo "    X11 payload: /tmp/matchbox-payload.tar ($(wc -c < /tmp/matchbox-payload.tar) bytes)"
    fi
    echo ""
    echo "    Deploy it later with:  tools/chunked-deploy.sh [user@host]"
    exit 0
fi

if [ "$KERNEL_ONLY" -eq 1 ]; then
    echo "==> deploying to $TARGET (zImage only)..."
else
    echo "==> deploying to $TARGET (zImage + modules + X11/Matchbox)..."
fi
set -- "$TARGET"
if [ -n "$ADAPTER" ]; then
    set -- --adapter "$ADAPTER" "$TARGET"
fi
if [ "$KERNEL_ONLY" -eq 1 ]; then
    set -- --kernel-only "$@"
fi
# Nothing was built, so don't let chunked-deploy ship a stale staged payload.
if [ "$SKIP_USERSPACE" -eq 1 ]; then
    set -- --no-userspace "$@"
fi
export REPO KERNEL_DIR
exec "$REPO/tools/chunked-deploy.sh" "$@"
