#!/bin/sh
set -eu

# Builds this project's MAIN cross-toolchain from source with
# crosstool-NG: arm-unknown-linux-uclibcgnueabi, GCC 13.4.0, uClibc-ng,
# ARMv5TE, EABI5, **soft-float**. Everything in userspace/ and the kernel
# build is compiled with it.
#
# Why this exists: toolchain/ is gitignored (a multi-GB, machine-specific
# build output, not source) and the toolchain is not vendored anywhere
# fetchable, so a fresh clone previously had NO way to reproduce it -- by
# far the biggest gap between "git clone" and "a working build". What is
# tracked instead is the crosstool-NG .config that produced the known-good
# toolchain (tools/uclibc-toolchain.config, lifted straight out of
# share/arm-unknown-linux-uclibcgnueabi-ct-ng.config.bz2 inside the built
# toolchain, with the two machine-specific absolute paths replaced by an
# @@PIKO_TOOLCHAIN_DIR@@ placeholder this script substitutes).
#
# Same approach as tools/build-oabi-toolchain.sh, which does this for the
# separate OABI toolchain the NAND flash helpers need. The two are NOT
# interchangeable.
#
# Usage:
#   tools/build-uclibc-toolchain.sh [--force]
#
# Exits 0 immediately if a working toolchain is already present, so it is
# cheap to call unconditionally. --force rebuilds anyway.
#
# This takes a long time (tens of minutes to hours) and wants several GB
# of disk. It is a one-off per machine.

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
TOOLCHAIN_DIR="${PIKO_TOOLCHAIN_DIR:-$REPO/toolchain}"
CTNG_VERSION="${CTNG_VERSION:-1.28.0}"
TARGET="${CROSS_HOST:-arm-unknown-linux-uclibcgnueabi}"
CONFIG="$REPO/tools/uclibc-toolchain.config"

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

GCC="$TOOLCHAIN_DIR/x-tools/$TARGET/bin/$TARGET-gcc"
if [ "$FORCE" -eq 0 ] && [ -x "$GCC" ]; then
    echo "==> toolchain already present: $($GCC --version | head -1)"
    echo "    (pass --force to rebuild)"
    exit 0
fi

if [ ! -f "$CONFIG" ]; then
    echo "FAILED: missing $CONFIG" >&2
    exit 1
fi

# crosstool-NG refuses to run as root, and building as root would produce
# a toolchain nobody else can use.
if [ "$(id -u)" = "0" ]; then
    echo "FAILED: do not run crosstool-NG as root." >&2
    exit 1
fi

mkdir -p "$TOOLCHAIN_DIR/src" "$TOOLCHAIN_DIR/build"

CTNG_SRC="$TOOLCHAIN_DIR/src/crosstool-ng-$CTNG_VERSION"
CTNG_TAR="$TOOLCHAIN_DIR/src/crosstool-ng-$CTNG_VERSION.tar.xz"
CTNG_BIN="$TOOLCHAIN_DIR/ctng/bin/ct-ng"

if [ ! -x "$CTNG_BIN" ]; then
    echo "==> building crosstool-NG $CTNG_VERSION"
    if [ ! -f "$CTNG_TAR" ]; then
        curl -fL --retry 3 -o "$CTNG_TAR.partial" \
            "https://github.com/crosstool-ng/crosstool-ng/releases/download/crosstool-ng-$CTNG_VERSION/crosstool-ng-$CTNG_VERSION.tar.xz"
        mv "$CTNG_TAR.partial" "$CTNG_TAR"
    fi
    [ -d "$CTNG_SRC" ] || tar xf "$CTNG_TAR" -C "$TOOLCHAIN_DIR/src"
    ( cd "$CTNG_SRC"
      ./configure --prefix="$TOOLCHAIN_DIR/ctng"
      make -j"$(nproc 2>/dev/null || echo 4)"
      make install )
fi

echo "==> configuring for $TARGET"
BUILD="$TOOLCHAIN_DIR/build/$TARGET"
rm -rf "$BUILD"
mkdir -p "$BUILD"
# Substitute the placeholder back to wherever this checkout actually is.
sed "s|@@PIKO_TOOLCHAIN_DIR@@|$TOOLCHAIN_DIR|g" "$CONFIG" > "$BUILD/.config"

echo "==> building (this takes a while)"
( cd "$BUILD"
  # oldconfig first: a newer crosstool-NG than the one that wrote this
  # config will otherwise stop and ask about new symbols.
  yes "" | "$CTNG_BIN" oldconfig >/dev/null 2>&1 || true
  "$CTNG_BIN" build )

if [ ! -x "$GCC" ]; then
    echo "FAILED: build finished but $GCC is missing" >&2
    exit 1
fi

echo "==> done: $($GCC --version | head -1)"
echo "    $GCC"
# Sanity-check the ABI actually matches what the rest of the project
# assumes: ARMv5TE soft-float. A hard-float or v7 toolchain will build
# fine and then produce binaries this board cannot run.
echo 'int main(void){return 0;}' > "$BUILD/abitest.c"
"$GCC" -march=armv5te -o "$BUILD/abitest" "$BUILD/abitest.c"
if command -v readelf >/dev/null 2>&1; then
    readelf -A "$BUILD/abitest" | grep -E "Tag_CPU_arch:|Tag_ABI_VFP_args:" || true
    echo "    (expect Tag_CPU_arch: v5TE and NO Tag_ABI_VFP_args -- that"
    echo "     tag appearing means a hard-float toolchain, which is wrong)"
fi
