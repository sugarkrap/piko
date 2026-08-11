#!/bin/sh
set -eu

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
SRC_DIR="$REPO/userspace/src"

KEXEC_VERSION="${KEXEC_VERSION:-2.0.31}"
KEXEC_SRC_DIR="$SRC_DIR/kexec-tools-$KEXEC_VERSION"
KEXEC_TARBALL="$SRC_DIR/kexec-tools-$KEXEC_VERSION.tar.xz"
KEXEC_URL="http://archive.ubuntu.com/ubuntu/pool/main/k/kexec-tools/kexec-tools_$KEXEC_VERSION.orig.tar.xz"
KEXEC_SHA256="8a8f350ddc66e1c905a3ab525a7e9ba96c81e04e70ef69397b0155b67b922c31"

STAGE_DIR="${STAGE_DIR:-$REPO/userspace/stage-kexec}"
TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin}"
CROSS_COMPILE="${CROSS_COMPILE:-arm-unknown-linux-uclibcgnueabi-}"
CROSS_HOST="${CROSS_HOST:-arm-unknown-linux-uclibcgnueabi}"
JOBS="${JOBS:-$(command -v nproc >/dev/null 2>&1 && nproc || echo 4)}"
TARGET_CFLAGS="${TARGET_CFLAGS:--march=armv5te -mfloat-abi=soft -O2}"

FORCE=0
for arg in "$@"; do
    case "$arg" in
        --force) FORCE=1 ;;
        -h|--help)
            sed -n '3,80p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "tools/userspace/build-kexec.sh: unknown argument: $arg" >&2
            echo "Usage: tools/userspace/build-kexec.sh [--force]" >&2
            exit 1
            ;;
    esac
done

mkdir -p "$SRC_DIR"

if [ -n "$TOOLCHAIN_BIN_DIR" ] && [ -d "$TOOLCHAIN_BIN_DIR" ]; then
    PATH="$TOOLCHAIN_BIN_DIR:$PATH"
fi
if ! command -v "${CROSS_COMPILE}gcc" >/dev/null 2>&1; then
    echo "tools/userspace/build-kexec.sh: ${CROSS_COMPILE}gcc not found in PATH." >&2
    echo "Set TOOLCHAIN_BIN_DIR to your toolchain bin path, or export CROSS_COMPILE explicitly." >&2
    echo "A fresh machine builds it with tools/toolchain/build-uclibc-toolchain.sh." >&2
    exit 1
fi
CC="${CROSS_COMPILE}gcc"
AR="${CROSS_COMPILE}ar"
RANLIB="${CROSS_COMPILE}ranlib"
STRIP="${CROSS_COMPILE}strip"
READELF="${CROSS_COMPILE}readelf"
BUILD_TRIPLET="$(uname -m)-pc-linux-gnu"

if [ "$FORCE" -eq 1 ] && [ -d "$KEXEC_SRC_DIR" ]; then
    echo "==> --force: removing existing $KEXEC_SRC_DIR"
    rm -rf "$KEXEC_SRC_DIR"
fi

if [ ! -f "$KEXEC_TARBALL" ]; then
    echo "==> downloading $KEXEC_URL"
    curl -fL -o "$KEXEC_TARBALL.partial" "$KEXEC_URL"
    mv "$KEXEC_TARBALL.partial" "$KEXEC_TARBALL"
else
    echo "==> reusing cached $KEXEC_TARBALL"
fi

echo "==> verifying sha256 of $(basename "$KEXEC_TARBALL")"
actual="$(sha256sum "$KEXEC_TARBALL" | cut -d' ' -f1)"
if [ "$actual" != "$KEXEC_SHA256" ]; then
    echo "tools/userspace/build-kexec.sh: SHA-256 mismatch for $KEXEC_TARBALL" >&2
    echo "  expected: $KEXEC_SHA256" >&2
    echo "  actual:   $actual" >&2
    echo "Refusing to build from a tarball that doesn't match -- remove it and" >&2
    echo "rerun if you deliberately changed the version/URL above." >&2
    exit 1
fi

if [ ! -d "$KEXEC_SRC_DIR" ]; then
    echo "==> extracting $(basename "$KEXEC_TARBALL") to $SRC_DIR"
    tar xJf "$KEXEC_TARBALL" -C "$SRC_DIR"
fi
if [ ! -f "$KEXEC_SRC_DIR/configure" ]; then
    echo "tools/userspace/build-kexec.sh: $KEXEC_SRC_DIR doesn't look like a configure-based tree" >&2
    exit 1
fi

echo "==> configuring kexec-tools $KEXEC_VERSION (kexec/vmcore-dmesg only, static, no zlib/lzma/zstd/xen)"
(
    cd "$KEXEC_SRC_DIR"
    [ -f Makefile ] && make clean >/dev/null 2>&1
    ./configure \
        --host="$CROSS_HOST" \
        --build="$BUILD_TRIPLET" \
        --prefix=/ \
        --sbindir=/sbin \
        --without-zlib \
        --without-lzma \
        --without-zstd \
        --without-xen \
        CC="$CC" AR="$AR" RANLIB="$RANLIB" STRIP="$STRIP" \
        CFLAGS="$TARGET_CFLAGS" \
        LDFLAGS="-static" \
        >/dev/null
)

echo "==> building kexec"
( cd "$KEXEC_SRC_DIR" && make -j"$JOBS" >/dev/null )

rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR/sbin"

install_bin() {
    src="$1"; dst="$STAGE_DIR$2"
    if [ ! -f "$src" ]; then
        echo "tools/userspace/build-kexec.sh: build finished but $src is missing" >&2
        exit 1
    fi
    cp "$src" "$dst"
    chmod 0755 "$dst"
    "$STRIP" --strip-unneeded "$dst"
}

install_bin "$KEXEC_SRC_DIR/build/sbin/kexec" /sbin/kexec

echo "==> verifying staged binary"
bin="$STAGE_DIR/sbin/kexec"
flags="$("$READELF" -h "$bin" | sed -n 's/^ *Flags: *//p')"
case "$flags" in
    0x5000200*) : ;;
    *)
        echo "tools/userspace/build-kexec.sh: /sbin/kexec has unexpected ELF Flags: $flags" >&2
        echo "  want 0x5000200 (Version5 EABI, soft-float ABI) as on every other" >&2
        echo "  binary this project ships." >&2
        exit 1
        ;;
esac
if "$READELF" -d "$bin" 2>/dev/null | grep -q NEEDED; then
    echo "tools/userspace/build-kexec.sh: /sbin/kexec is dynamically linked (has NEEDED entries)." >&2
    "$READELF" -d "$bin" | awk '/NEEDED/{print "    " $NF}' >&2
    echo "  It must be static: the bootstrap initramfs kexecs into stage 2 with no" >&2
    echo "  dynamic loader or shared libs available at all." >&2
    exit 1
fi
printf '    /sbin/kexec  %8s bytes  Flags: %s  static\n' "$(wc -c < "$bin")" "$flags"

echo ""
echo "==> done: $STAGE_DIR assembled"
echo "    $STAGE_DIR/sbin/kexec"
echo ""
echo "    Picked up by tools/build-mtd3-jffs2.sh."
