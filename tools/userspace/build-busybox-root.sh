#!/bin/sh
set -eu

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
STAGE="${BUSYBOX_ROOT_STAGE:-$REPO/build/stage-busybox}"
SRC_DIR="${BUSYBOX_SRC_DIR:-$REPO/build/initramfs/busybox-1.36.1}"
BUILD_DIR="${BUSYBOX_ROOT_BUILD:-$REPO/build/initramfs/busybox-build-root}"
APPLETS="${BUSYBOX_APPLETS:-$REPO/modules/rootfs/busybox-applets.list}"
TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin}"
CROSS_COMPILE="${CROSS_COMPILE:-arm-unknown-linux-uclibcgnueabi-}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
FORCE=0

while [ $# -gt 0 ]; do
    case "$1" in
        --force) FORCE=1; shift ;;
        *) echo "tools/userspace/build-busybox-root.sh: unknown argument '$1'" >&2; exit 1 ;;
    esac
done

if [ ! -f "$SRC_DIR/Makefile" ]; then
    echo "build-busybox-root: no busybox tree at $SRC_DIR" >&2
    exit 1
fi
if [ ! -f "$APPLETS" ]; then
    echo "build-busybox-root: missing applet list $APPLETS" >&2
    exit 1
fi
if [ -d "$TOOLCHAIN_BIN_DIR" ]; then
    PATH="$TOOLCHAIN_BIN_DIR:$PATH"
fi
if ! command -v "${CROSS_COMPILE}gcc" >/dev/null 2>&1; then
    echo "build-busybox-root: no ${CROSS_COMPILE}gcc in PATH" >&2
    exit 1
fi

if [ "$FORCE" -eq 1 ] || [ ! -f "$BUILD_DIR/busybox_unstripped" ]; then
    echo "==> configuring busybox for the root filesystem (O=$BUILD_DIR)"
    rm -rf "$BUILD_DIR"
    mkdir -p "$BUILD_DIR"
    make -C "$SRC_DIR" O="$BUILD_DIR" ARCH=arm CROSS_COMPILE="$CROSS_COMPILE" \
        defconfig >/dev/null
    sed -i 's/^CONFIG_TC=y/# CONFIG_TC is not set/' "$BUILD_DIR/.config"
    make -C "$SRC_DIR" O="$BUILD_DIR" ARCH=arm CROSS_COMPILE="$CROSS_COMPILE" \
        oldconfig </dev/null >/dev/null
    echo "==> building busybox (-j$JOBS)"
    make -C "$SRC_DIR" O="$BUILD_DIR" ARCH=arm CROSS_COMPILE="$CROSS_COMPILE" \
        -j"$JOBS" busybox >/dev/null
else
    echo "==> reusing $BUILD_DIR/busybox_unstripped"
fi

if [ ! -f "$BUILD_DIR/busybox_unstripped" ]; then
    echo "build-busybox-root: make reported success but there is no $BUILD_DIR/busybox_unstripped" >&2
    exit 1
fi

rm -rf "$STAGE"
mkdir -p "$STAGE/bin"
"${CROSS_COMPILE}strip" -o "$STAGE/bin/busybox" "$BUILD_DIR/busybox_unstripped"
chmod 0755 "$STAGE/bin/busybox"

missing=""
while read -r applet; do
    [ -n "$applet" ] || continue
    name="${applet##*/}"
    if ! grep -qF "\"$name\" " "$BUILD_DIR/include/applet_tables.h" 2>/dev/null; then
        missing="$missing $name"
    fi
    mkdir -p "$STAGE/$(dirname "$applet")"
    ln -sf /bin/busybox "$STAGE/$applet"
done < "$APPLETS"

if [ -n "$missing" ]; then
    echo "build-busybox-root: this busybox config provides no applet for:$missing" >&2
    exit 1
fi

echo "==> $STAGE/bin/busybox ($(stat -c '%s' "$STAGE/bin/busybox") bytes)"
echo "    applets linked: $(grep -c . "$APPLETS")"
