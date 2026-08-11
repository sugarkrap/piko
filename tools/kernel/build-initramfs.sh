#!/bin/sh
set -eu

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
BUSYBOX_VERSION="${BUSYBOX_VERSION:-1.36.1}"
INITRAMFS_DIR="${INITRAMFS_DIR:-$REPO/initramfs}"
BUSYBOX_SRC_DIR="${BUSYBOX_SRC_DIR:-$INITRAMFS_DIR/busybox-$BUSYBOX_VERSION}"
BUSYBOX_TARBALL="${BUSYBOX_TARBALL:-$INITRAMFS_DIR/busybox-$BUSYBOX_VERSION.tar.bz2}"
BUSYBOX_URL="https://busybox.net/downloads/busybox-$BUSYBOX_VERSION.tar.bz2"
ROOTFS_BUILD_DIR="${ROOTFS_BUILD_DIR:-$INITRAMFS_DIR/rootfs-build}"
OUT_CPIO="${OUT_CPIO:-$INITRAMFS_DIR/initramfs-minimal-built.cpio.gz}"
TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin}"
CROSS_COMPILE="${CROSS_COMPILE:-arm-unknown-linux-uclibcgnueabi-}"
JOBS="${JOBS:-$(command -v nproc >/dev/null 2>&1 && nproc || echo 4)}"

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

mkdir -p "$INITRAMFS_DIR"

if [ -n "$TOOLCHAIN_BIN_DIR" ] && [ -d "$TOOLCHAIN_BIN_DIR" ]; then
    PATH="$TOOLCHAIN_BIN_DIR:$PATH"
fi
if ! command -v "${CROSS_COMPILE}gcc" >/dev/null 2>&1; then
    echo "tools/kernel/build-initramfs.sh: ${CROSS_COMPILE}gcc not found in PATH." >&2
    echo "Set TOOLCHAIN_BIN_DIR to your toolchain bin path, or export CROSS_COMPILE explicitly." >&2
    exit 1
fi

if [ ! -f "$BUSYBOX_TARBALL" ]; then
    echo "==> downloading $BUSYBOX_URL"
    curl -fL -o "$BUSYBOX_TARBALL.partial" "$BUSYBOX_URL"
    mv "$BUSYBOX_TARBALL.partial" "$BUSYBOX_TARBALL"
else
    echo "==> reusing cached $BUSYBOX_TARBALL"
fi

if [ "$FORCE" -eq 1 ] && [ -d "$BUSYBOX_SRC_DIR" ]; then
    echo "==> --force: removing existing $BUSYBOX_SRC_DIR"
    rm -rf "$BUSYBOX_SRC_DIR"
fi

if [ ! -d "$BUSYBOX_SRC_DIR" ]; then
    echo "==> extracting to $INITRAMFS_DIR"
    tar xjf "$BUSYBOX_TARBALL" -C "$INITRAMFS_DIR"
else
    echo "==> reusing existing source tree $BUSYBOX_SRC_DIR"
fi

if [ ! -f "$BUSYBOX_SRC_DIR/Makefile" ]; then
    echo "tools/kernel/build-initramfs.sh: $BUSYBOX_SRC_DIR doesn't look like a busybox tree (no Makefile)" >&2
    exit 1
fi

BB_CONFIG_SRC="$REPO/modules/initramfs/busybox.config"
INIT_SRC="$REPO/modules/initramfs/init"
SPLASH_SRC="$REPO/modules/initramfs/splash.ppm.gz"
if [ ! -f "$BB_CONFIG_SRC" ]; then
    echo "tools/kernel/build-initramfs.sh: missing tracked input: $BB_CONFIG_SRC" >&2
    exit 1
fi
if [ ! -f "$INIT_SRC" ]; then
    echo "tools/kernel/build-initramfs.sh: missing tracked input: $INIT_SRC" >&2
    exit 1
fi
if [ ! -f "$SPLASH_SRC" ]; then
    echo "tools/kernel/build-initramfs.sh: missing tracked input: $SPLASH_SRC" >&2
    exit 1
fi

BUILD_DIR="$INITRAMFS_DIR/.build-$BUSYBOX_VERSION"
echo "==> configuring busybox $BUSYBOX_VERSION (O=$BUILD_DIR)"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cp "$BB_CONFIG_SRC" "$BUILD_DIR/.config"

make -C "$BUSYBOX_SRC_DIR" O="$BUILD_DIR" ARCH=arm CROSS_COMPILE="$CROSS_COMPILE" \
    oldconfig </dev/null

echo "==> building busybox (static, -j$JOBS)"
make -C "$BUSYBOX_SRC_DIR" O="$BUILD_DIR" ARCH=arm CROSS_COMPILE="$CROSS_COMPILE" \
    -j"$JOBS" busybox

BB_BIN="$BUILD_DIR/busybox_unstripped"
if [ ! -f "$BB_BIN" ]; then
    echo "tools/kernel/build-initramfs.sh: expected build output missing: $BB_BIN" >&2
    exit 1
fi
"${CROSS_COMPILE}strip" -o "$BUILD_DIR/busybox" "$BB_BIN"

echo "==> assembling rootfs at $ROOTFS_BUILD_DIR"
rm -rf "$ROOTFS_BUILD_DIR"
mkdir -p "$ROOTFS_BUILD_DIR/bin" "$ROOTFS_BUILD_DIR/sbin" \
         "$ROOTFS_BUILD_DIR/usr/bin" "$ROOTFS_BUILD_DIR/usr/sbin" \
         "$ROOTFS_BUILD_DIR/dev" "$ROOTFS_BUILD_DIR/proc" \
         "$ROOTFS_BUILD_DIR/sys" "$ROOTFS_BUILD_DIR/tmp" \
         "$ROOTFS_BUILD_DIR/root"

cp "$BUILD_DIR/busybox" "$ROOTFS_BUILD_DIR/bin/busybox"
chmod 755 "$ROOTFS_BUILD_DIR/bin/busybox"

cp "$INIT_SRC" "$ROOTFS_BUILD_DIR/init"
chmod 755 "$ROOTFS_BUILD_DIR/init"

gzip -dc "$SPLASH_SRC" > "$ROOTFS_BUILD_DIR/splash.ppm"
chmod 644 "$ROOTFS_BUILD_DIR/splash.ppm"

BIN_APPLETS="ash cat chmod chown cp cttyhack date dd df dmesg echo fbsplash grep hostname ln ls mkdir mknod mount mountpoint mv ps pwd rm rmdir sed sh sleep stat sync touch umount uname vi"
SBIN_APPLETS="halt init mdev poweroff reboot switch_root"
USR_BIN_APPLETS="basename clear dirname env find free hd head hexdump reset setsid tail test tr wc which"
USR_SBIN_APPLETS="fbset"

for a in $BIN_APPLETS; do ln -sf busybox "$ROOTFS_BUILD_DIR/bin/$a"; done
for a in $SBIN_APPLETS; do ln -sf ../bin/busybox "$ROOTFS_BUILD_DIR/sbin/$a"; done
for a in $USR_BIN_APPLETS; do ln -sf ../../bin/busybox "$ROOTFS_BUILD_DIR/usr/bin/$a"; done
for a in $USR_SBIN_APPLETS; do ln -sf ../../bin/busybox "$ROOTFS_BUILD_DIR/usr/sbin/$a"; done

echo "==> packing $OUT_CPIO"
find "$ROOTFS_BUILD_DIR" -exec touch -h -t 202001010000 {} +
( cd "$ROOTFS_BUILD_DIR" && find . | cpio -H newc -o ) 2>/dev/null | gzip -9 -n > "$OUT_CPIO.partial"
mv "$OUT_CPIO.partial" "$OUT_CPIO"

echo "==> built $OUT_CPIO"
ls -la "$OUT_CPIO"

REFERENCE="$INITRAMFS_DIR/initramfs-minimal-v2.cpio.gz"
if [ -f "$REFERENCE" ]; then
    if cmp -s "$OUT_CPIO" "$REFERENCE"; then
        echo "==> byte-identical to $REFERENCE"
    else
        echo "==> NOT byte-identical to $REFERENCE (expected -- see the"
        echo "    'Reproducibility note' at the top of this script: the"
        echo "    reference was built with a different GCC/toolchain)."
        echo "    Sizes: built=$(wc -c < "$OUT_CPIO") reference=$(wc -c < "$REFERENCE")"
    fi
else
    echo "==> no reference $REFERENCE present, skipping comparison"
fi
