#!/bin/sh
set -eu

ADAPTER=""
FORCE_KERNEL_SRC=0
KERNEL_ONLY=0
SKIP_USERSPACE=0
SKIP_ST=0
SKIP_X11=0
BUILD_ONLY=0
SKIP_BUILD=0
SKIP_ROOT_IMAGE=0
DEPLOY_ROOT_IMAGE=0
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
        --skip-build)
            SKIP_BUILD=1
            shift
            ;;
        --skip-root-image)
            SKIP_ROOT_IMAGE=1
            shift
            ;;
        --deploy-root-image)
            DEPLOY_ROOT_IMAGE=1
            shift
            ;;
        --staging)
            echo "FAILED: --staging is gone" >&2
            exit 1
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

if [ "$SKIP_ROOT_IMAGE" -eq 1 ] && [ "$DEPLOY_ROOT_IMAGE" -eq 1 ]; then
    echo "FAILED: --skip-root-image and --deploy-root-image are opposites." >&2
    exit 1
fi

if [ "$SKIP_BUILD" -eq 1 ] && [ "$BUILD_ONLY" -eq 1 ]; then
    echo "FAILED: --skip-build and --build-only are opposites (skip the build" >&2
    echo "        and deploy vs. build and skip the deploy) -- pick one." >&2
    exit 1
fi
REPO="$(cd "$(dirname "$0")/.." && pwd)"
KERNEL_DIR="$REPO/build/kernel/src/linux-7.1.4"
TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin}"
BUILD_LOG="/tmp/kbuild-$(date +%Y%m%d-%H%M%S).log"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
export JOBS

echo "==> building piko-sync-deploy"
"$REPO/tools/userspace/build-piko-sync-deploy.sh"
PIKO_SYNC_DEPLOY="$REPO/build/host/bin/piko-sync-deploy"

if [ "$BUILD_ONLY" -eq 1 ]; then
    echo "==> --build-only: building without contacting any device"
else
    echo "==> checking $TARGET is reachable before spending time building..."
    PROBE_ARGS=""
    if [ -n "$ADAPTER" ]; then
        PROBE_ARGS="--adapter $ADAPTER"
    fi
    if ! "$PIKO_SYNC_DEPLOY" $PROBE_ARGS --probe "$TARGET"; then
        echo "FAILED: $TARGET is not reachable, or piko-sync-server is not open" >&2
        echo "on the device -- deploy needs it open (unlike the old SSH-based" >&2
        echo "chunked-deploy.sh). Open piko-sync-server from the desktop and retry." >&2
        echo "If the device is unreachable/unbootable, or you need to change" >&2
        echo "the bootstrap partition (mtd1/smf), use the recovery flash" >&2
        echo "procedure instead (SD card + recovery menu)." >&2
        exit 1
    fi
fi

if [ "$SKIP_BUILD" -eq 1 ]; then
    echo "==> --skip-build: deploying whatever is already built, nothing recompiled"
    if [ ! -f "$KERNEL_DIR/arch/arm/boot/zImage" ]; then
        echo "FAILED: --skip-build was given but no built kernel exists at" >&2
        echo "        $KERNEL_DIR/arch/arm/boot/zImage" >&2
        echo "        Run a build first (or drop --skip-build)." >&2
        exit 1
    fi
    X11_PAYLOAD_TAR="${PAYLOAD_TAR:-/tmp/matchbox-payload.tar}"
else

echo "==> reconstructing kernel-src (download + apply tracked patches)..."
if [ "$FORCE_KERNEL_SRC" -eq 1 ]; then
    "$REPO/tools/kernel/setup-kernel-src.sh" --force
else
    "$REPO/tools/kernel/setup-kernel-src.sh"
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
    echo "==> building userspace (md5sum + scp/sftp-server + ALSA + MPlayer + SDL + st + FLTK) via tools/userspace/build-userspace.sh..."
    if ! (
        export PATH TOOLCHAIN_BIN_DIR CROSS_COMPILE
        sh "$REPO/tools/userspace/build-userspace.sh" $USERSPACE_ARGS
    ); then
        echo "FAILED: userspace build did not complete." >&2
        echo "Re-run tools/userspace/build-userspace.sh directly to see the full output," >&2
        echo "or pass --skip-userspace to deploy the kernel without it." >&2
        echo "If it died on st (empty userspace/src/st, or git.suckless.org" >&2
        echo "unreachable), --skip-st builds everything else and carries on." >&2
        exit 1
    fi
    echo "==> userspace build OK"
fi

X11_PAYLOAD_TAR="${PAYLOAD_TAR:-/tmp/matchbox-payload.tar}"
if [ "$KERNEL_ONLY" -eq 0 ] && [ "$SKIP_X11" -eq 0 ]; then
    X11_ARGS=""
    if [ "$SKIP_ST" -eq 1 ]; then
        X11_ARGS="--skip-st"
    fi
    echo "==> building the X11/Matchbox stack (tools/userspace/build-x11-stack.sh)..."
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
    if ! sh "$REPO/tools/userspace/build-x11-stack.sh" $X11_ARGS > /tmp/x11-stack-build.log 2>&1; then
        x11_died "the X11/Matchbox stack did not build" /tmp/x11-stack-build.log
    fi
    echo "==> repacking the X11/Matchbox payload..."
    if ! sh "$REPO/tools/userspace/build-matchbox-payload.sh" $X11_ARGS > /tmp/x11-payload-build.log 2>&1; then
        x11_died "the X11/Matchbox payload could not be assembled" /tmp/x11-payload-build.log
    fi
    echo "==> X11 payload OK ($(wc -c < "$X11_PAYLOAD_TAR") bytes)"
fi

if [ "$KERNEL_ONLY" -eq 1 ]; then
    echo "==> --kernel-only: skipping the root image"
elif [ "$SKIP_ROOT_IMAGE" -eq 1 ]; then
    echo "==> --skip-root-image: not rebuilding flash/piko-root.img"
else
    echo "==> building the root image (tools/build-rootfs.sh)..."
    if KERNEL_DIR="$KERNEL_DIR" ROOT_IMG_OUT="$REPO/build/flash/piko-root.img" \
            "$REPO/tools/build-rootfs.sh" > /tmp/rootfs-build.log 2>&1; then
        UPDATE_DIR="${UPDATE_DIR:-$REPO/build/build/sd-card/update/.zaurus}"
        mkdir -p "$UPDATE_DIR"
        cp "$KERNEL_DIR/arch/arm/boot/zImage" "$UPDATE_DIR/zImage-full"
        cp "$REPO/build/flash/piko-root.img"        "$UPDATE_DIR/piko-root.img"
        echo "==> root image OK ($(wc -c < "$REPO/build/flash/piko-root.img") bytes)"
        echo "    update set refreshed: $UPDATE_DIR"
    else
        echo "WARNING: no root image, see /tmp/rootfs-build.log" >&2
        tail -20 /tmp/rootfs-build.log >&2
    fi
fi

fi

if [ "$BUILD_ONLY" -eq 1 ]; then
    echo ""
    echo "==> --build-only: everything built, nothing deployed."
    if [ -f "$KERNEL_DIR/arch/arm/boot/zImage" ]; then
        echo "    zImage:      $KERNEL_DIR/arch/arm/boot/zImage ($(wc -c < "$KERNEL_DIR/arch/arm/boot/zImage") bytes)"
    fi
    if [ -f "$X11_PAYLOAD_TAR" ]; then
        echo "    X11 payload: $X11_PAYLOAD_TAR ($(wc -c < "$X11_PAYLOAD_TAR") bytes)"
    fi
    if [ -f "$REPO/build/flash/piko-root.img" ]; then
        echo "    root image:  $REPO/build/flash/piko-root.img ($(wc -c < "$REPO/build/flash/piko-root.img") bytes)"
    fi
    echo ""
    echo "    Deploy it later with:  build/host/bin/piko-sync-deploy [user@host]"
    echo "    (piko-sync-server must be open on the device first)"
    exit 0
fi

if [ "$KERNEL_ONLY" -eq 1 ]; then
    echo "==> deploying to $TARGET (zImage only)..."
else
    echo "==> deploying to $TARGET (zImage + modules + X11/Matchbox + root payload)..."
fi
set -- "$TARGET"
if [ -n "$ADAPTER" ]; then
    set -- --adapter "$ADAPTER" "$TARGET"
fi
if [ "$KERNEL_ONLY" -eq 1 ]; then
    set -- --kernel-only "$@"
fi
if [ "$SKIP_USERSPACE" -eq 1 ]; then
    set -- --no-userspace "$@"
fi
if [ "$CREATE_BACKUP_FILES" -eq 1 ]; then
    set -- --create-backup-files "$@"
fi
if [ "$DEPLOY_ROOT_IMAGE" -eq 1 ]; then
    if [ ! -f "$REPO/build/flash/piko-root.img" ]; then
        echo "FAILED: no $REPO/build/flash/piko-root.img" >&2
        exit 1
    fi
    echo "==> staging piko-root.img.new"
    set -- --root-image "$@"
fi
X11_PAYLOAD="${X11_PAYLOAD:-$X11_PAYLOAD_TAR}"
export REPO KERNEL_DIR X11_PAYLOAD
exec "$PIKO_SYNC_DEPLOY" "$@"
