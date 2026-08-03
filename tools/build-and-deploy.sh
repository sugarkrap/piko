#!/bin/sh
set -eu

# Cross-compiles the stage-2 kernel + all modules with our buildroot
# toolchain, then deploys the result (zImage + sound modules + WiFi/PCMCIA
# modules + helper scripts) to a reachable Zaurus by calling pikodeploy
# (userspace/src/pikodeploy/pikodeploy.cxx). This is the ROUTINE path for
# updating the running "home"-partition kernel: no NAND flash, no
# recovery menu, no reboot to a service menu. See
# docs/HOWTO-BUILD-DEPLOY-KERNEL.md.
#
# pikodeploy replaced tools/chunked-deploy.sh's hand-rolled SSH chunking
# here -- same idea (chunked, resumable, verified transfer) as a real
# protocol instead of shell driving `ssh`/`cat` pipelines by hand. One
# consequence worth knowing: it talks to pikoxfer-server over TCP, not
# SSH, so the device needs pikoxfer-server OPEN (the GUI app, on the
# desktop) for a deploy to work at all -- unlike chunked-deploy.sh, which
# only ever needed dropbear running. See userspace/src/pikodeploy/'s own
# README for the wire protocol and manifest.yaml for the actual file list.
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
#   tools/build-and-deploy.sh [--adapter IFACE] [--force-kernel-src] [--kernel-only] [--skip-userspace] [--skip-st] [--skip-x11] [--build-only] [--create-backup-files] [user@host]
# Example:
#   tools/build-and-deploy.sh --adapter wlan0 root@10.43.112.72
#
# --adapter IFACE binds the SSH connection to a specific local network
# interface (ssh -B), useful when the build machine has multiple network
# adapters and the Zaurus is only reachable via one of them.
# --create-backup-files forwards to pikodeploy, which then keeps a
# "$remote_path.bak" copy of whatever each transferred file replaces.
# Previously only reachable by invoking chunked-deploy.sh directly -- any
# flag this script's own arg parser doesn't recognize silently becomes the
# TARGET argument instead of an error, so there was no way to ask for this
# through the routine build-and-deploy.sh path at all. Off by default, same
# reasoning as chunked-deploy.sh's own default: every file this touches
# ends up duplicated on the ~68 MiB root jffs2 if it's on.
# --force-kernel-src forces tools/setup-kernel-src.sh to re-apply every
# tracked patch even if kernel-src/ already looks patched -- use this if
# you've changed one of the tracked patch files under modules/.
# --kernel-only builds only zImage (skips `make modules`) and forwards
# --kernel-only to pikodeploy, which then only ships
# /boot/zImage-full and skips every module/script/helper deploy step.
# Faster iteration when you're only touching kernel/.config, e.g. verifying
# a JFFS2 compressor fix, and don't need to redeploy unchanged modules.
# --skip-x11 skips tools/build-x11-stack.sh + tools/build-matchbox-payload.sh
# entirely (a from-scratch X11 build needs the toolchain from
# tools/build-uclibc-toolchain.sh plus tools/build-thirdparty-deps.sh already
# staged; skip this if either isn't set up yet, or you just don't want the
# X11/Matchbox payload rebuilt/redeployed this run). Without it, a desktop
# that fails to build now FAILS THE RUN rather than deploying a kernel and
# leaving you to notice the missing desktop later -- see the X11 section
# below for why that changed.
# --skip-userspace skips building the cross-compiled userspace
# (tools/build-userspace.sh: md5sum + scp/sftp-server + ALSA + MPlayer + SDL
# + st + FLTK) and forwards
# --no-userspace to pikodeploy so it does not ship a stale staged
# payload either. The userspace build is idempotent and therefore cheap once
# built, so this is mainly for when the toolchain or a vendored source tree
# is in a knowingly broken state and you just need the kernel out.
# --skip-st forwards --skip-st to tools/build-userspace.sh,
# tools/build-x11-stack.sh AND tools/build-matchbox-payload.sh (all three
# can build or demand st): build every other component as usual, but leave
# st out, including out of the X11 payload and its desktop menu. Narrower
# than --skip-userspace, and the reason it exists is that st is the one
# component whose source is a submodule of an upstream that is not
# GitHub (git.suckless.org) -- when that host is down or the submodule
# was never initialized, userspace/src/st is an empty directory and
# build-st.sh dies with a bare "make: no makefile found", failing the
# whole run at the last step after everything else already succeeded.
# This lets a deploy proceed without it instead of forcing --skip-userspace
# and losing ALSA/MPlayer/SDL/SSH along with it.
# --build-only builds everything this script would normally build (kernel,
# modules, userspace, the X11/Matchbox payload) and then STOPS, without
# contacting the device at all -- no reachability probe up front and no
# pikodeploy handoff at the end. No target argument is needed or used.
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
SKIP_ST=0
SKIP_X11=0
BUILD_ONLY=0
CREATE_BACKUP_FILES=0
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
        --skip-st)
            SKIP_ST=1
            shift
            ;;
        --build-only)
            BUILD_ONLY=1
            shift
            ;;
        --create-backup-files)
            CREATE_BACKUP_FILES=1
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

# pikodeploy itself needs building before either the probe below or the
# final exec can use it -- unlike chunked-deploy.sh, a checked-in script
# that was simply always there, this is a compiled binary. Plain host
# g++, no cross toolchain, no FLTK/X11 stage: see tools/build-pikodeploy.sh.
echo "==> building pikodeploy"
"$REPO/tools/build-pikodeploy.sh"
PIKODEPLOY="$REPO/userspace/src/pikodeploy/pikodeploy"

# --build-only never touches the device, so the probe that exists purely to
# fail fast before a long build would be both pointless and, in CI, a
# guaranteed failure.
if [ "$BUILD_ONLY" -eq 1 ]; then
    echo "==> --build-only: building without contacting any device"
else
    echo "==> checking $TARGET is reachable before spending time building..."
    # Not an SSH probe anymore: pikodeploy talks to pikoxfer-server over
    # its own TCP port, not dropbear, so an SSH reachability check would
    # be testing the wrong protocol -- it could pass while the actual
    # deploy path (pikoxfer-server not open on the device) still fails,
    # or fail while a deploy would have worked fine.
    PROBE_ARGS=""
    if [ -n "$ADAPTER" ]; then
        PROBE_ARGS="--adapter $ADAPTER"
    fi
    if ! "$PIKODEPLOY" $PROBE_ARGS --probe "$TARGET"; then
        echo "FAILED: $TARGET is not reachable, or pikoxfer-server is not open" >&2
        echo "on the device -- deploy needs it open (unlike the old SSH-based" >&2
        echo "chunked-deploy.sh). Open pikoxfer-server from the desktop and retry." >&2
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
    for prefix in arm-unknown-linux-uclibcgnueabi- arm-buildroot-linux-uclibcgnueabi- arm-linux-gnueabi- arm-unknown-linux-gnueabi-; do
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

# Userspace (md5sum + scp/sftp-server + ALSA + MPlayer + SDL + st + FLTK).
# Delegated to
# tools/build-userspace.sh
# rather than open-coded here -- it is the single entry point for every
# cross-built userspace component, and each step it runs is idempotent, so
# this is cheap on every subsequent invocation once things are built.
#
# md5sum used to be deployed first specifically so chunked-deploy.sh could
# content-verify every later transfer instead of only byte-counting it
# (silent truncation over this WiFi link is a real, repeatedly-observed
# failure mode). pikodeploy no longer needs that bootstrap step -- every
# PUT_FILE already carries a whole-file CRC32 the device verifies before
# finalizing, natively, not via a separately-shipped external tool -- so
# this is now just "build userspace", not "build userspace, and also
# prerequisite plumbing for the deploy mechanism itself".
#
# Skipped for --kernel-only, which deploys nothing but the zImage anyway.
if [ "$KERNEL_ONLY" -eq 1 ]; then
    echo "==> --kernel-only: skipping the userspace build"
elif [ "$SKIP_USERSPACE" -eq 1 ]; then
    echo "==> --skip-userspace: not building userspace components"
else
    USERSPACE_ARGS=""
    if [ "$SKIP_ST" -eq 1 ]; then
        USERSPACE_ARGS="--skip-st"
        echo "==> --skip-st: building userspace without st"
    fi
    echo "==> building userspace (md5sum + scp/sftp-server + ALSA + MPlayer + SDL + st + FLTK) via tools/build-userspace.sh..."
    if ! (
        export PATH TOOLCHAIN_BIN_DIR CROSS_COMPILE
        # Unquoted on purpose: empty means "no extra argument" here, and a
        # quoted "" would be passed through as a literal empty argument
        # that build-userspace.sh rejects as an unknown option.
        sh "$REPO/tools/build-userspace.sh" $USERSPACE_ARGS
    ); then
        echo "FAILED: userspace build did not complete." >&2
        echo "Re-run tools/build-userspace.sh directly to see the full output," >&2
        echo "or pass --skip-userspace to deploy the kernel without it." >&2
        echo "If it died on st (empty userspace/src/st, or git.suckless.org" >&2
        echo "unreachable), --skip-st builds everything else and carries on." >&2
        exit 1
    fi
    echo "==> userspace build OK"
fi

# --- X11 + Matchbox desktop -------------------------------------------
# tools/build-x11-stack.sh cross-builds every X.Org/Matchbox submodule
# (idempotent -- skips anything already built, so this is cheap on every
# subsequent run, same as tools/setup-kernel-src.sh/build-userspace.sh
# above), then tools/build-matchbox-payload.sh collects the result into
# the single tar pikodeploy extracts locally and ships file-by-file (see
# manifest.yaml's x11_matchbox section and manifest.h's header for why
# individual resumable transfers replaced shipping+unpacking one tar).
# See docs/HOWTO-MATCHBOX-DESKTOP.md.
#
# EITHER FAILURE IS FATAL. It did not used to be: the rationale was that a
# machine without the X11 toolchain provisioned should still get a kernel
# out. What that actually produced was a deploy which printed one warning
# line, carried on through several minutes of kernel/module transfers, and
# ended with pikodeploy's cheerful
#
#     ==> no X11 payload at /tmp/matchbox-payload.tar -- skipping
#         (build it with tools/build-matchbox-payload.sh)
#
# -- which reads like a configuration choice rather than the tail end of a
# compile error scrolled off the top of the terminal. Reported 2026-08-01
# by someone who ran a deploy expecting the desktop to be in it, which is
# the reasonable expectation: "build and deploy" says nothing about doing
# only part of it.
#
# So: you get the payload, or you get an error. --skip-x11 remains the way
# to ask for a deploy without one, and it is now the ONLY way -- an
# explicit choice rather than an accident, which is the whole point.
# --kernel-only skips this section outright, as before.
X11_PAYLOAD_TAR="${PAYLOAD_TAR:-/tmp/matchbox-payload.tar}"
if [ "$KERNEL_ONLY" -eq 0 ] && [ "$SKIP_X11" -eq 0 ]; then
    X11_ARGS=""
    if [ "$SKIP_ST" -eq 1 ]; then
        X11_ARGS="--skip-st"
    fi
    echo "==> building the X11/Matchbox stack (tools/build-x11-stack.sh)..."
    # Unquoted on purpose, same as USERSPACE_ARGS above: a quoted "" would
    # be passed through as a literal empty argument and rejected.
    # shellcheck disable=SC2086
    # x11_died STEP LOGFILE -- one exit path for both failures, so they
    # cannot drift into saying different things about the same situation.
    x11_died() {
        echo "" >&2
        echo "FAILED: $1 -- see $2" >&2
        echo "" >&2
        tail -15 "$2" >&2
        echo "" >&2
        echo "Not deploying. The desktop is part of what this script builds," >&2
        echo "so a desktop that did not build is a failed run, not a run that" >&2
        echo "quietly ships less than you asked for." >&2
        echo "" >&2
        echo "  * to deploy the kernel and userspace WITHOUT a desktop," >&2
        echo "    re-run with --skip-x11 added:" >&2
        echo "        $0 --skip-x11 ..." >&2
        echo "  * to deploy only the kernel:  --kernel-only" >&2
        if [ -f "$X11_PAYLOAD_TAR" ]; then
            echo "" >&2
            echo "  Note: $X11_PAYLOAD_TAR exists, left over from an earlier" >&2
            echo "  run. --skip-x11 will ship THAT, which does not necessarily" >&2
            echo "  match the tree you just built. Delete it first if you want" >&2
            echo "  a deploy with no desktop at all." >&2
        fi
        exit 1
    }
    # shellcheck disable=SC2086
    if ! sh "$REPO/tools/build-x11-stack.sh" $X11_ARGS > /tmp/x11-stack-build.log 2>&1; then
        x11_died "the X11/Matchbox stack did not build" /tmp/x11-stack-build.log
    fi
    echo "==> repacking the X11/Matchbox payload..."
    # shellcheck disable=SC2086
    if ! sh "$REPO/tools/build-matchbox-payload.sh" $X11_ARGS > /tmp/x11-payload-build.log 2>&1; then
        x11_died "the X11/Matchbox payload could not be assembled" /tmp/x11-payload-build.log
    fi
    echo "==> X11 payload OK ($(wc -c < "$X11_PAYLOAD_TAR") bytes)"
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
    if [ -f "$X11_PAYLOAD_TAR" ]; then
        echo "    X11 payload: $X11_PAYLOAD_TAR ($(wc -c < "$X11_PAYLOAD_TAR") bytes)"
    fi
    echo ""
    echo "    Deploy it later with:  userspace/src/pikodeploy/pikodeploy [user@host]"
    echo "    (pikoxfer-server must be open on the device first)"
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
# Nothing was built, so don't let pikodeploy ship a stale staged payload.
if [ "$SKIP_USERSPACE" -eq 1 ]; then
    set -- --no-userspace "$@"
fi
if [ "$CREATE_BACKUP_FILES" -eq 1 ]; then
    set -- --create-backup-files "$@"
fi
# The two scripts spell the same file with different variable names --
# PAYLOAD_TAR when producing it, X11_PAYLOAD when shipping it. Pin them
# together so overriding the producer cannot leave pikodeploy sending
# whatever happens to be at the default path instead.
X11_PAYLOAD="${X11_PAYLOAD:-$X11_PAYLOAD_TAR}"
export REPO KERNEL_DIR X11_PAYLOAD
exec "$PIKODEPLOY" "$@"
