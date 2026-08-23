#!/bin/sh
set -eu

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
STAGE="$REPO/build/target"
CACHE="${THIRDPARTY_CACHE:-$REPO/build/dl}"
. "$REPO/tools/userspace/dl-cache.sh"
piko_seed_dl_cache "$REPO" "${CACHE:-${CACHE_DIR:-}}"
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
[ -n "$PKGS" ] || PKGS="zlib expat libpng freetype fontconfig xkeyboard-config dejavu libarchive"

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
export LDFLAGS="-L$STAGE/usr/lib -Wl,-rpath-link=$STAGE/usr/lib"

mkdir -p "$CACHE" "$BUILD" "$STAGE"

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
        echo "2.13.2 https://download.sourceforge.net/freetype/freetype-2.13.2.tar.gz \
1ac27e16c134a7f2ccea177faba19801131116fd682efc1f5737037c5db224b5 \
usr/lib/pkgconfig/freetype2.pc"
        ;;
    fontconfig)
        echo "2.14.2 https://www.freedesktop.org/software/fontconfig/release/fontconfig-2.14.2.tar.gz \
3ba2dd92158718acec5caaf1a716043b5aa055c27b081d914af3ccb40dce8a55 \
usr/lib/pkgconfig/fontconfig.pc"
        ;;
    xkeyboard-config)
        echo "2.32 https://www.x.org/releases/individual/data/xkeyboard-config/xkeyboard-config-2.32.tar.bz2 \
1feee317ba39b91902b0cbd2987c0c73e6afbfc8f4c096367a5c86c216c036a8 \
usr/share/X11/xkb/rules/base"
        ;;
    libarchive)
        echo "3.7.7 https://github.com/libarchive/libarchive/releases/download/v3.7.7/libarchive-3.7.7.tar.gz \
4cc540a3e9a1eebdefa1045d2e4184831100667e6d7d5b315bb1cbc951f8ddff \
usr/lib/pkgconfig/libarchive.pc"
        ;;
    dejavu)
        echo "2.37 https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/dejavu-fonts-ttf-2.37.tar.bz2 \
fa9ca4d13871dd122f61258a80d01751d603b4d3ee14095d65453b4e846e17d7 \
usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
        ;;
    *)
        echo "FAILED: unknown package: $1" >&2
        exit 1
        ;;
    esac
}

configure_args() {
    case "$1" in
    libpng)     echo "--disable-static" ;;
    xkeyboard-config)
        echo "--disable-runtime-deps"
        ;;
    freetype)
        echo "--disable-static --with-harfbuzz=no --with-brotli=no --with-bzip2=no --with-png=no --with-zlib=yes"
        ;;
    fontconfig)
        echo "--disable-static --disable-docs --with-arch=arm --sysconfdir=/etc --localstatedir=/var"
        ;;
    expat)      echo "--disable-static --without-docbook --without-examples --without-tests" ;;
    libarchive)
        echo "--disable-shared --enable-static \
--disable-bsdtar --disable-bsdcpio --disable-bsdcat --disable-bsdunzip \
--disable-acl --disable-xattr --disable-rpath \
--with-zlib --without-bz2lib --without-libb2 --without-iconv \
--without-lz4 --without-zstd --without-lzma --without-lzo2 \
--without-cng --without-openssl --without-xml2 --without-expat" ;;
    xkeyboard-config)
        echo "--disable-nls --with-xkb-rules-symlink=xorg" ;;
    *)          echo "--disable-static" ;;
    esac
}

fetch() {
    url="$1"; want="$2"; out="$3"
    if [ ! -f "$out" ]; then
        echo "    downloading $(basename "$out")"
        curl -fL --http1.1 --retry 3 -o "$out.partial" "$url"
        mv "$out.partial" "$out"
    fi
    got="$(sha256sum "$out" | cut -d' ' -f1)"
    if [ "$got" != "$want" ]; then
        echo "FAILED: sha256 mismatch for $(basename "$out")" >&2
        echo "  expected $want" >&2
        echo "  got      $got" >&2
        echo "Refusing to build an unverified tarball. Delete it and retry," >&2
        echo "or update the pin in tools/userspace/build-thirdparty-deps.sh." >&2
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

    if [ "$name" = dejavu ]; then
        fontdir="$STAGE/usr/share/fonts/truetype/dejavu"
        mkdir -p "$fontdir"
        for face in DejaVuSans.ttf DejaVuSans-Bold.ttf; do
            src="$(find "$BUILD" -name "$face" -path "*/ttf/*" | head -1)"
            if [ -z "$src" ]; then
                echo "FAILED: $face not found in the dejavu tarball" >&2
                exit 1
            fi
            cp "$src" "$fontdir/$face"
            echo "    installed: usr/share/fonts/truetype/dejavu/$face"
        done
        return 0
    fi

    ( cd "$srcdir"
      if [ "$name" = zlib ]; then
          ./configure --prefix=/usr
      else
          if [ "$name" = xkeyboard-config ]; then
              PKG_CONFIG_LIBDIR="$PKG_CONFIG_LIBDIR:/usr/share/pkgconfig"
              export PKG_CONFIG_LIBDIR
          fi
          ./configure --host="$HOST" --build="$(uname -m)-pc-linux-gnu" \
                      --prefix=/usr $(configure_args "$name")
      fi
      make -j"$(nproc 2>/dev/null || echo 4)"
      make install DESTDIR="$STAGE"
    )

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
