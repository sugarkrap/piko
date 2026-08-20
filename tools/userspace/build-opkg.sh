#!/bin/sh
set -eu

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
STAGE="$REPO/userspace/stage-target"
CACHE="${THIRDPARTY_CACHE:-$REPO/userspace/.thirdparty-cache}"
BUILD="${OPKG_BUILD:-/tmp/piko-opkg-build}"

VERSION=0.6.3
URL="https://downloads.yoctoproject.org/releases/opkg/opkg-$VERSION.tar.gz"
SHA256=f3938e359646b406c40d5d442a1467c7e72357f91ab822e442697529641e06de
MARKER="usr/bin/opkg"

HOST="${CROSS_HOST:-arm-unknown-linux-uclibcgnueabi}"
TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/$HOST/bin}"

FORCE=0
for arg in "$@"; do
    case "$arg" in
        --force) FORCE=1 ;;
        -h|--help) sed -n '3,10p' "$0"; exit 0 ;;
        *) echo "FAILED: unknown option: $arg" >&2; exit 1 ;;
    esac
done

if [ ! -d "$TOOLCHAIN_BIN_DIR" ]; then
    echo "FAILED: toolchain bin dir not found: $TOOLCHAIN_BIN_DIR" >&2
    echo "Run tools/toolchain/build-uclibc-toolchain.sh, or set TOOLCHAIN_BIN_DIR." >&2
    exit 1
fi

if [ ! -f "$STAGE/usr/lib/pkgconfig/libarchive.pc" ]; then
    echo "FAILED: libarchive is not staged in $STAGE." >&2
    echo "Run:  tools/userspace/build-thirdparty-deps.sh libarchive" >&2
    exit 1
fi

if [ "$FORCE" -eq 0 ] && [ -f "$STAGE/$MARKER" ]; then
    echo "==> opkg $VERSION: already staged, skipping (--force to rebuild)"
    exit 0
fi

PATH="$TOOLCHAIN_BIN_DIR:$PATH"
export PATH
export CC="${HOST}-gcc"
export AR="${HOST}-ar"
export RANLIB="${HOST}-ranlib"
export STRIP="${HOST}-strip"
export PKG_CONFIG_SYSROOT_DIR="$STAGE"
export PKG_CONFIG_LIBDIR="$STAGE/usr/lib/pkgconfig:$STAGE/usr/share/pkgconfig"
export PKG_CONFIG_PATH=
export CPPFLAGS="-I$STAGE/usr/include"

export LDFLAGS="-L$STAGE/usr/lib -Wl,-rpath-link=$STAGE/usr/lib"

mkdir -p "$CACHE" "$BUILD" "$STAGE"

tarball="$CACHE/opkg-$VERSION.tar.gz"
if [ ! -f "$tarball" ]; then
    echo "    downloading opkg-$VERSION.tar.gz"
    curl -fL --http1.1 --retry 3 -o "$tarball.partial" "$URL"
    mv "$tarball.partial" "$tarball"
fi
got="$(sha256sum "$tarball" | cut -d' ' -f1)"
if [ "$got" != "$SHA256" ]; then
    echo "FAILED: sha256 mismatch for opkg-$VERSION.tar.gz" >&2
    echo "  expected $SHA256" >&2
    echo "  got      $got" >&2
    exit 1
fi

echo "==> opkg $VERSION"
srcdir="$BUILD/opkg-$VERSION"
rm -rf "$srcdir"
tar xf "$tarball" -C "$BUILD"

( cd "$srcdir"

  ./configure --host="$HOST" --build="$(uname -m)-pc-linux-gnu" \
              --prefix=/usr --sysconfdir=/etc --localstatedir=/var \
              --with-static-libopkg --disable-shared --enable-static \
              --disable-curl --disable-gpg --disable-sha256 \
              --disable-ssl-curl --disable-libopkg-api \
              --disable-xz --disable-bzip2 --disable-lz4 --disable-zstd \
              --without-libsolv --without-acl --without-xattr

  make -j"$(nproc 2>/dev/null || echo 4)" LDFLAGS="-L$STAGE/usr/lib -all-static"
  make install DESTDIR="$STAGE" LDFLAGS="-L$STAGE/usr/lib -all-static"
)

rm -f "$STAGE"/usr/lib/*.la

if [ ! -f "$STAGE/$MARKER" ]; then
    echo "FAILED: opkg built but $MARKER is missing from the staging tree" >&2
    exit 1
fi

if "$TOOLCHAIN_BIN_DIR/$HOST-readelf" -d "$STAGE/$MARKER" 2>/dev/null | grep -q NEEDED; then
    echo "FAILED: opkg linked dynamically -- the -all-static link did not take." >&2
    echo "Shared libraries it still needs:" >&2
    "$TOOLCHAIN_BIN_DIR/$HOST-readelf" -d "$STAGE/$MARKER" | grep NEEDED >&2
    exit 1
fi

"$STRIP" "$STAGE/$MARKER" 2>/dev/null || true

echo "    staged: $MARKER ($(wc -c < "$STAGE/$MARKER") bytes, static)"
echo "==> opkg ready in $STAGE"
