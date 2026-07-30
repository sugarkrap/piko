#!/bin/sh
set -eu

# Cross-builds the non-X.Org third-party libraries the X11/Matchbox stack
# needs, into the ARM staging tree at userspace/stage-target.
#
# Everything X.Org is a tracked submodule under userspace/src/ (see
# .gitmodules) with local edits applied by tools/setup-x11-src.sh. These
# are the ones that aren't X.Org -- generic libraries pinned by version
# and SHA-256 here rather than vendored, same spirit as
# tools/setup-kernel-src.sh downloading a pristine kernel tarball.
#
# Usage:
#   tools/build-thirdparty-deps.sh [--force] [PKG ...]
#
# With no PKG arguments it builds all of them, in dependency order.
# Already-staged packages are skipped unless --force is given, so this is
# cheap to re-run.
#
#   zlib        -- (no deps)
#   expat       -- (no deps)
#   libpng      -- zlib
#   freetype    -- zlib
#   fontconfig  -- freetype, expat
#
# Dependency order matters and is NOT checked: build them in the order
# listed above (which is what the no-argument default does).

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
STAGE="$REPO/userspace/stage-target"
CACHE="${THIRDPARTY_CACHE:-$REPO/userspace/.thirdparty-cache}"
BUILD="${THIRDPARTY_BUILD:-/tmp/piko-thirdparty-build}"

HOST="${CROSS_HOST:-arm-unknown-linux-uclibcgnueabi}"
TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/$HOST/bin}"

FORCE=0
PKGS=""
for arg in "$@"; do
    case "$arg" in
        --force) FORCE=1 ;;
        -*) echo "FAILED: unknown option: $arg" >&2; exit 1 ;;
        *) PKGS="$PKGS $arg" ;;
    esac
done
[ -n "$PKGS" ] || PKGS="zlib expat libpng freetype fontconfig"

if [ ! -d "$TOOLCHAIN_BIN_DIR" ]; then
    echo "FAILED: toolchain bin dir not found: $TOOLCHAIN_BIN_DIR" >&2
    echo "Set TOOLCHAIN_BIN_DIR, or CROSS_HOST if your triplet differs." >&2
    exit 1
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
# -rpath-link (not -rpath) lets the cross-linker resolve *indirect*
# dependencies -- e.g. fontconfig links libfreetype, which itself needs
# libz, and without this ld reports "libz.so.1, needed by
# libfreetype.so, not found" even though -L points straight at it. It
# affects link-time resolution only; nothing is baked into the binaries,
# so the device still resolves these from /lib at runtime.
export LDFLAGS="-L$STAGE/usr/lib -Wl,-rpath-link=$STAGE/usr/lib"

mkdir -p "$CACHE" "$BUILD" "$STAGE"

# pkg_spec NAME -> "version url sha256 marker"
# marker is a file under $STAGE that proves this package is already staged.
pkg_spec() {
    case "$1" in
    zlib)
        echo "1.3.1 https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz \
9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23 \
usr/lib/pkgconfig/zlib.pc"
        ;;
    expat)
        echo "2.6.2 https://github.com/libexpat/libexpat/releases/download/R_2_6_2/expat-2.6.2.tar.gz \
d4cf38d26e21a56654ffe4acd9cd5481164619626802328506a2869afab29ab3 \
usr/lib/pkgconfig/expat.pc"
        ;;
    libpng)
        echo "1.6.43 https://download.sourceforge.net/libpng/libpng-1.6.43.tar.gz \
e804e465d4b109b5ad285a8fb71f0dd3f74f0068f91ce3cdfde618180c174925 \
usr/lib/pkgconfig/libpng.pc"
        ;;
    freetype)
        echo "2.13.2 https://download.savannah.gnu.org/releases/freetype/freetype-2.13.2.tar.gz \
1ac27e16c134a7f2ccea177faba19801131116fd682efc1f5737037c5db224b5 \
usr/lib/pkgconfig/freetype2.pc"
        ;;
    fontconfig)
        echo "2.14.2 https://www.freedesktop.org/software/fontconfig/release/fontconfig-2.14.2.tar.gz \
3ba2dd92158718acec5caaf1a716043b5aa055c27b081d914af3ccb40dce8a55 \
usr/lib/pkgconfig/fontconfig.pc"
        ;;
    *)
        echo "FAILED: unknown package: $1" >&2
        exit 1
        ;;
    esac
}

# configure_args NAME -- extra flags, kept minimal on purpose: this device
# has 64MB of RAM and no accelerator, so anything optional stays off.
configure_args() {
    case "$1" in
    libpng)     echo "--disable-static" ;;
    freetype)
        # No harfbuzz (would be circular via pango), no brotli/bz2 (only
        # needed for compressed/WOFF2 fonts), no png (that's for colour
        # bitmap fonts, unrelated to libmatchbox's own PNG loading).
        echo "--disable-static --with-harfbuzz=no --with-brotli=no --with-bzip2=no --with-png=no --with-zlib=yes"
        ;;
    fontconfig)
        # Docs need docbook; the cache tools are target binaries so they
        # can't be run here.
        echo "--disable-static --disable-docs --with-arch=arm"
        ;;
    expat)      echo "--disable-static --without-docbook --without-examples --without-tests" ;;
    *)          echo "--disable-static" ;;
    esac
}

fetch() {
    url="$1"; want="$2"; out="$3"
    if [ ! -f "$out" ]; then
        echo "    downloading $(basename "$out")"
        curl -fL --retry 3 -o "$out.partial" "$url"
        mv "$out.partial" "$out"
    fi
    got="$(sha256sum "$out" | cut -d' ' -f1)"
    if [ "$got" != "$want" ]; then
        echo "FAILED: sha256 mismatch for $(basename "$out")" >&2
        echo "  expected $want" >&2
        echo "  got      $got" >&2
        echo "Refusing to build an unverified tarball. Delete it and retry," >&2
        echo "or update the pin in tools/build-thirdparty-deps.sh." >&2
        exit 1
    fi
}

build_one() {
    name="$1"
    set -- $(pkg_spec "$name")
    version="$1"; url="$2"; sha="$3"; marker="$4"

    if [ "$FORCE" -eq 0 ] && [ -f "$STAGE/$marker" ]; then
        echo "==> $name $version: already staged, skipping"
        return 0
    fi

    echo "==> $name $version"
    tarball="$CACHE/$(basename "$url")"
    fetch "$url" "$sha" "$tarball"

    srcdir="$BUILD/$name-$version"
    rm -rf "$srcdir"
    mkdir -p "$BUILD"
    tar xf "$tarball" -C "$BUILD"
    [ -d "$srcdir" ] || srcdir="$(find "$BUILD" -maxdepth 1 -type d -name "$name-*" | head -1)"

    ( cd "$srcdir"
      if [ "$name" = zlib ]; then
          # zlib's configure is a hand-written script, not autoconf: it has
          # no --host and picks the compiler up from $CC.
          ./configure --prefix=/usr
      else
          ./configure --host="$HOST" --build="$(uname -m)-pc-linux-gnu" \
                      --prefix=/usr $(configure_args "$name")
      fi
      make -j"$(nproc 2>/dev/null || echo 4)"
      make install DESTDIR="$STAGE"
    )

    # libtool .la files record an absolute libdir=/usr/lib and make the
    # cross-linker pick the HOST copy of a library over ours. Nothing here
    # needs them.
    rm -f "$STAGE"/usr/lib/*.la

    if [ ! -f "$STAGE/$marker" ]; then
        echo "FAILED: $name built but $marker is missing from the staging tree" >&2
        exit 1
    fi
    echo "    staged: $marker"
}

for p in $PKGS; do
    build_one "$p"
done

echo "==> third-party deps ready in $STAGE"
