#!/bin/sh
set -eu

# Reconstructs the bootstrap-kernel initramfs from scratch: downloads a
# pristine busybox.net release tarball for busybox 1.36.1, applies the
# tracked config (modules/initramfs/busybox.config), cross-compiles a
# static busybox with this project's ARM toolchain, assembles a minimal
# rootfs (busybox + applet symlinks + standard dirs + the tracked /init,
# see modules/initramfs/init), and packs it into a .cpio.gz exactly the
# way docs/archive/DEADLETTER.md documents doing it by hand:
#
#   find . | cpio -H newc -o | gzip -9 > ../initramfs-minimal.cpio.gz
#
# This is the automated version of the "unzip a build tree someone emailed
# you" workflow this project doesn't want (see AGENTS.md / handoff docs
# for 2026-07-29/30) -- initramfs/ itself stays gitignored (a full
# busybox source+build tree plus assembled rootfs trees, not source, see
# .gitignore), this script is what makes it reproducible without
# vendoring any of that.
#
# Usage:
#   tools/build-initramfs.sh [--force]
#
# --force re-extracts busybox from the tarball even if a source tree is
# already present at BUSYBOX_SRC_DIR (default: just reuse it -- the
# oldconfig+build steps are cheap and always rerun regardless).
#
# Env overrides:
#   BUSYBOX_VERSION     default 1.36.1 (matches the tracked busybox.config)
#   INITRAMFS_DIR        default <repo>/initramfs (gitignored working dir)
#   BUSYBOX_SRC_DIR       default $INITRAMFS_DIR/busybox-$BUSYBOX_VERSION
#   BUSYBOX_TARBALL       default $INITRAMFS_DIR/busybox-$BUSYBOX_VERSION.tar.bz2
#   ROOTFS_BUILD_DIR      default $INITRAMFS_DIR/rootfs-build (fresh assembly,
#                         never touches the known-good initramfs/rootfs-minimal/)
#   OUT_CPIO              default $INITRAMFS_DIR/initramfs-minimal-built.cpio.gz
#                         (deliberately NOT initramfs-minimal-v2.cpio.gz --
#                         this script never overwrites the known-good
#                         reference artifact)
#   TOOLCHAIN_BIN_DIR     default <repo>/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin
#   CROSS_COMPILE          default arm-unknown-linux-uclibcgnueabi-
#   JOBS                   default: nproc
#
# Exit codes:
#   0   $OUT_CPIO was built successfully
#   1   a hard failure (download, extraction, build, or packing failure)
#
# Reproducibility note: the resulting .cpio.gz will NOT be byte-identical
# to initramfs/initramfs-minimal-v2.cpio.gz. Busybox embeds its own build
# banner ("BusyBox v1.36.1 (<build-date> <build-time> <tz>)") plus a GCC
# version/vendor string in every binary it produces, so two builds of
# *identical* source+config always differ at the byte level even on the
# same machine seconds apart -- and the known-good reference was built
# with `GCC: (Buildroot 2026.02.3) 14.3.0` on a different machine, while
# this project's tracked toolchain is `crosstool-NG 1.28.0, GCC 13.4.0`.
# Config correctness was verified empirically (see the busybox.config
# comment at the top of that file) by comparing applet sets and
# non-version string content, not by chasing a byte-identical rebuild.
#
# This script pins cpio member mtimes and passes `gzip -n` (no embedded
# name/mtime) to eliminate those two sources of non-determinism, but one
# remains even for two back-to-back builds on this exact same machine
# and toolchain: busybox bakes its own build wall-clock time into the
# binary as part of the "BusyBox v1.36.1 (<date> <time> <tz>)" banner
# string, so the compiled busybox (and therefore the packed .cpio.gz)
# necessarily differs byte-for-byte between any two separate build runs,
# by design, upstream, nothing to fix here.

REPO="$(cd "$(dirname "$0")/.." && pwd)"
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
    echo "tools/build-initramfs.sh: ${CROSS_COMPILE}gcc not found in PATH." >&2
    echo "Set TOOLCHAIN_BIN_DIR to your toolchain bin path, or export CROSS_COMPILE explicitly." >&2
    exit 1
fi

# --- 1. obtain busybox source ---------------------------------------
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
    echo "tools/build-initramfs.sh: $BUSYBOX_SRC_DIR doesn't look like a busybox tree (no Makefile)" >&2
    exit 1
fi

BB_CONFIG_SRC="$REPO/modules/initramfs/busybox.config"
INIT_SRC="$REPO/modules/initramfs/init"
# Pre-rendered boot splash: a full-screen 640x480 PPM, stored gzipped
# because that is the form the size budget is actually measured in (see
# tools/make-splash.py). Only the host needs to unpack it -- the device
# never does, and this busybox has no gunzip.
SPLASH_SRC="$REPO/modules/initramfs/splash.ppm.gz"
if [ ! -f "$BB_CONFIG_SRC" ]; then
    echo "tools/build-initramfs.sh: missing tracked input: $BB_CONFIG_SRC" >&2
    exit 1
fi
if [ ! -f "$INIT_SRC" ]; then
    echo "tools/build-initramfs.sh: missing tracked input: $INIT_SRC" >&2
    exit 1
fi
if [ ! -f "$SPLASH_SRC" ]; then
    echo "tools/build-initramfs.sh: missing tracked input: $SPLASH_SRC" >&2
    exit 1
fi

# --- 2. cross-compile a static busybox using an out-of-tree build dir,
# so this never mutates $BUSYBOX_SRC_DIR itself (repeat runs stay clean) --
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
    echo "tools/build-initramfs.sh: expected build output missing: $BB_BIN" >&2
    exit 1
fi
"${CROSS_COMPILE}strip" -o "$BUILD_DIR/busybox" "$BB_BIN"

# --- 3. assemble a fresh rootfs tree ---------------------------------
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

# Splash goes in uncompressed: the cpio is gzipped as a whole immediately
# below, so storing it compressed here would just be gzip-on-gzip (bigger,
# and unreadable to a busybox built without CONFIG_FEATURE_SEAMLESS_GZ).
gzip -dc "$SPLASH_SRC" > "$ROOTFS_BUILD_DIR/splash.ppm"
chmod 644 "$ROOTFS_BUILD_DIR/splash.ppm"

# Applet symlink set, hand-enumerated from the known-good
# initramfs/rootfs-minimal/ tree (busybox in this config has
# CONFIG_FEATURE_INSTALLER disabled, so `busybox --install` isn't
# available at runtime to regenerate this list automatically).
BIN_APPLETS="ash cat chmod chown cp cttyhack date dd df dmesg echo fbsplash grep hostname ln ls mkdir mknod mount mountpoint mv ps pwd rm rmdir sed sh sleep stat sync touch umount uname vi"
SBIN_APPLETS="halt init mdev poweroff reboot switch_root"
USR_BIN_APPLETS="basename clear dirname env find free hd head hexdump reset setsid tail test tr wc which"
USR_SBIN_APPLETS="fbset"

for a in $BIN_APPLETS; do ln -sf busybox "$ROOTFS_BUILD_DIR/bin/$a"; done
for a in $SBIN_APPLETS; do ln -sf ../bin/busybox "$ROOTFS_BUILD_DIR/sbin/$a"; done
for a in $USR_BIN_APPLETS; do ln -sf ../../bin/busybox "$ROOTFS_BUILD_DIR/usr/bin/$a"; done
for a in $USR_SBIN_APPLETS; do ln -sf ../../bin/busybox "$ROOTFS_BUILD_DIR/usr/sbin/$a"; done

# --- 4. pack, pinning timestamps for build-to-build reproducibility --
echo "==> packing $OUT_CPIO"
# Pin every entry's mtime so repeated builds on this toolchain produce
# byte-identical cpio member headers regardless of wall-clock time.
find "$ROOTFS_BUILD_DIR" -exec touch -h -t 202001010000 {} +
( cd "$ROOTFS_BUILD_DIR" && find . | cpio -H newc -o ) 2>/dev/null | gzip -9 -n > "$OUT_CPIO.partial"
mv "$OUT_CPIO.partial" "$OUT_CPIO"

echo "==> built $OUT_CPIO"
ls -la "$OUT_CPIO"

# --- 5. verify against the known-good reference, if present ----------
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
