#!/bin/sh
set -eu

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
ALSA_LIB_VERSION="${ALSA_LIB_VERSION:-1.2.12}"
ALSA_UTILS_VERSION="${ALSA_UTILS_VERSION:-1.2.12}"
SRC_DIR="${SRC_DIR:-$REPO/build/src}"
DL_DIR="${DL_DIR:-$REPO/build/dl}"
. "$REPO/tools/userspace/dl-cache.sh"
piko_seed_dl_cache "$REPO" "$DL_DIR"

ALSA_LIB_SRC_DIR="${ALSA_LIB_SRC_DIR:-$SRC_DIR/alsa-lib-$ALSA_LIB_VERSION}"
ALSA_LIB_TARBALL="${ALSA_LIB_TARBALL:-$DL_DIR/alsa-lib-$ALSA_LIB_VERSION.tar.bz2}"
ALSA_LIB_URL="https://www.alsa-project.org/files/pub/lib/alsa-lib-$ALSA_LIB_VERSION.tar.bz2"

ALSA_UTILS_SRC_DIR="${ALSA_UTILS_SRC_DIR:-$SRC_DIR/alsa-utils-$ALSA_UTILS_VERSION}"
ALSA_UTILS_TARBALL="${ALSA_UTILS_TARBALL:-$DL_DIR/alsa-utils-$ALSA_UTILS_VERSION.tar.bz2}"
ALSA_UTILS_URL="https://www.alsa-project.org/files/pub/utils/alsa-utils-$ALSA_UTILS_VERSION.tar.bz2"

STAGE_DIR="${STAGE_DIR:-$REPO/build/stage-alsa}"
RUNTIME_DIR="${RUNTIME_DIR:-$REPO/build/stage-alsa-runtime}"
NOPIC_DIR="${NOPIC_DIR:-$REPO/build/stage-alsa-nopic}"

TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin}"
CROSS_COMPILE="${CROSS_COMPILE:-arm-unknown-linux-uclibcgnueabi-}"
JOBS="${JOBS:-$(command -v nproc >/dev/null 2>&1 && nproc || echo 4)}"

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1
PIKO_STAMP="$STAGE_DIR/.piko-stamp"
PIKO_STATE="$(sha256sum "$0" | cut -d' ' -f1) $ALSA_LIB_VERSION $ALSA_UTILS_VERSION"
if [ "$FORCE" -eq 0 ] && [ -f "$PIKO_STAMP" ] \
   && [ "$(cat "$PIKO_STAMP")" = "$PIKO_STATE" ] && [ -f "$STAGE_DIR/usr/lib/libasound.a" ] \
   && [ -f "$NOPIC_DIR/usr/lib/libasound.a" ]; then
    echo "==> alsa $ALSA_LIB_VERSION already staged for these inputs, skipping (--force to rebuild)"
    exit 0
fi


mkdir -p "$SRC_DIR"

if [ -n "$TOOLCHAIN_BIN_DIR" ] && [ -d "$TOOLCHAIN_BIN_DIR" ]; then
    PATH="$TOOLCHAIN_BIN_DIR:$PATH"
fi
if ! command -v "${CROSS_COMPILE}gcc" >/dev/null 2>&1; then
    echo "tools/userspace/build-alsa.sh: ${CROSS_COMPILE}gcc not found in PATH." >&2
    echo "Set TOOLCHAIN_BIN_DIR to your toolchain bin path, or export CROSS_COMPILE explicitly." >&2
    exit 1
fi
CC="${CROSS_COMPILE}gcc"
AR="${CROSS_COMPILE}ar"
RANLIB="${CROSS_COMPILE}ranlib"
STRIP="${CROSS_COMPILE}strip"
READELF="${CROSS_COMPILE}readelf"

fetch_and_extract() {
    _tarball="$1"; _url="$2"; _srcdir="$3"; _label="$4"

    if [ ! -f "$_tarball" ]; then
        echo "==> downloading $_url"
        curl -fL --http1.1 -o "$_tarball.partial" "$_url"
        mv "$_tarball.partial" "$_tarball"
    else
        echo "==> reusing cached $_tarball"
    fi

    if [ "$FORCE" -eq 1 ] && [ -d "$_srcdir" ]; then
        echo "==> --force: removing existing $_srcdir"
        rm -rf "$_srcdir"
    fi

    if [ ! -d "$_srcdir" ]; then
        echo "==> extracting $_label to $SRC_DIR"
        tar xjf "$_tarball" -C "$SRC_DIR"
    else
        echo "==> reusing existing source tree $_srcdir"
    fi

    if [ ! -f "$_srcdir/configure" ]; then
        echo "tools/userspace/build-alsa.sh: $_srcdir doesn't look like a configure-based tree" >&2
        exit 1
    fi
}

fetch_and_extract "$ALSA_LIB_TARBALL" "$ALSA_LIB_URL" "$ALSA_LIB_SRC_DIR" "alsa-lib-$ALSA_LIB_VERSION"
fetch_and_extract "$ALSA_UTILS_TARBALL" "$ALSA_UTILS_URL" "$ALSA_UTILS_SRC_DIR" "alsa-utils-$ALSA_UTILS_VERSION"

rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR"

echo "==> configuring alsa-lib $ALSA_LIB_VERSION"
(
    cd "$ALSA_LIB_SRC_DIR"
    [ -f Makefile ] && make distclean >/dev/null 2>&1
    ./configure \
        --host=arm-unknown-linux-uclibcgnueabi \
        --build="$(./config.guess 2>/dev/null || echo x86_64-pc-linux-gnu)" \
        --prefix=/usr \
        --disable-shared --enable-static --with-pic \
        --without-versioned \
        --disable-python \
        --disable-old-symbols \
        --disable-ucm \
        --disable-topology \
        --with-softfloat \
        CC="$CC" AR="$AR" RANLIB="$RANLIB" STRIP="$STRIP"
    echo "==> building alsa-lib"
    make -j"$JOBS"
    echo "==> installing alsa-lib to $STAGE_DIR"
    make install DESTDIR="$STAGE_DIR"
)

LIBASOUND_LA="$STAGE_DIR/usr/lib/libasound.la"
if [ -f "$LIBASOUND_LA" ]; then
    sed -i "s|^libdir='/usr/lib'|libdir='$STAGE_DIR/usr/lib'|" "$LIBASOUND_LA"
fi

rm -rf "$NOPIC_DIR"
mkdir -p "$NOPIC_DIR"

echo "==> configuring alsa-lib $ALSA_LIB_VERSION (non-PIC, for the fully static consumers)"
(
    cd "$ALSA_LIB_SRC_DIR"
    make distclean >/dev/null 2>&1 || true
    ./configure \
        --host=arm-unknown-linux-uclibcgnueabi \
        --build="$(./config.guess 2>/dev/null || echo x86_64-pc-linux-gnu)" \
        --prefix=/usr \
        --disable-shared --enable-static --without-pic \
        --without-versioned \
        --disable-python \
        --disable-old-symbols \
        --disable-ucm \
        --disable-topology \
        --with-softfloat \
        CC="$CC" AR="$AR" RANLIB="$RANLIB" STRIP="$STRIP"
    echo "==> building alsa-lib (non-PIC)"
    make -j"$JOBS"
    echo "==> installing alsa-lib (non-PIC) to $NOPIC_DIR"
    make install DESTDIR="$NOPIC_DIR"
)

if ! "${CROSS_COMPILE}nm" "$NOPIC_DIR/usr/lib/libasound.a" 2>/dev/null | grep -q "snd_dlsym_start"; then
    echo "tools/userspace/build-alsa.sh: no snd_dlsym_start in $NOPIC_DIR/usr/lib/libasound.a -- built with -DPIC, so a static binary has no way to reach its plugins" >&2
    exit 1
fi

NOPIC_LA="$NOPIC_DIR/usr/lib/libasound.la"
if [ -f "$NOPIC_LA" ]; then
    sed -i "s|^libdir='/usr/lib'|libdir='$NOPIC_DIR/usr/lib'|" "$NOPIC_LA"
fi

echo "==> configuring alsa-utils $ALSA_UTILS_VERSION"
(
    cd "$ALSA_UTILS_SRC_DIR"
    [ -f Makefile ] && make distclean >/dev/null 2>&1
    ./configure \
        --host=arm-unknown-linux-uclibcgnueabi \
        --build="$(./config.guess 2>/dev/null || echo x86_64-pc-linux-gnu)" \
        --prefix=/usr \
        --with-alsa-prefix="$NOPIC_DIR/usr/lib" \
        --with-alsa-inc-prefix="$NOPIC_DIR/usr/include" \
        --disable-alsamixer \
        --disable-bat \
        --disable-alsaloop \
        --disable-nhlt \
        --disable-alsaconf \
        --disable-xmlto \
        --disable-rst2man \
        --disable-nls \
        --disable-rpath \
        CC="$CC" AR="$AR" RANLIB="$RANLIB" STRIP="$STRIP" \
        PKG_CONFIG=false
    if ! grep -rq --include=Makefile -- "-lasound" .; then
        echo "tools/userspace/build-alsa.sh: configure produced no -lasound in the alsa-utils makefiles" >&2
        exit 1
    fi
    find . -name Makefile -exec sed -i "s|-lasound|-Wl,--whole-archive,$NOPIC_DIR/usr/lib/libasound.a,--no-whole-archive|g" {} +
    echo "==> building alsa-utils"
    make -j"$JOBS" LDFLAGS="-all-static"
    if ! "${CROSS_COMPILE}nm" aplay/aplay 2>/dev/null | grep -q "snd_dlsym_start"; then
        echo "tools/userspace/build-alsa.sh: aplay has no snd_dlsym_start -- alsa-lib was not force-linked, every device open will fail with 'Cannot open shared library [builtin]'" >&2
        exit 1
    fi
    for sym in _snd_pcm_hw_open _snd_pcm_plug_open; do
        if ! "${CROSS_COMPILE}nm" aplay/aplay 2>/dev/null | grep -q " $sym$"; then
            echo "tools/userspace/build-alsa.sh: $sym is missing from aplay -- alsa-lib resolves its plugins through dlsym, so every object must be force-linked in" >&2
            exit 1
        fi
    done
    echo "==> installing alsa-utils to $STAGE_DIR"
    make install DESTDIR="$STAGE_DIR"
)

echo "==> stripping bogus build-host RPATH from installed binaries"
python3 - "$STAGE_DIR/usr/lib" "$STAGE_DIR"/usr/bin/* "$STAGE_DIR"/usr/sbin/* <<'PYEOF'
import sys

def strip(path, needle):
    try:
        with open(path, 'rb') as f:
            data = bytearray(f.read())
    except IsADirectoryError:
        return False
    start = 0
    found = False
    while True:
        idx = data.find(needle, start)
        if idx == -1:
            break
        end = data.find(b'\x00', idx)
        if end == -1:
            end = idx + len(needle)
        for i in range(idx, end):
            data[i] = 0
        found = True
        start = end + 1
    if found:
        with open(path, 'wb') as f:
            f.write(data)
    return found

needle = sys.argv[1].encode()
for path in sys.argv[2:]:
    import os
    if os.path.islink(path) or not os.path.isfile(path):
        continue
    strip(path, needle)
PYEOF

echo "==> stripping debug symbols"
for f in "$STAGE_DIR"/usr/bin/* "$STAGE_DIR"/usr/sbin/*; do
    [ -L "$f" ] && continue
    [ -f "$f" ] || continue
    "$STRIP" --strip-unneeded "$f" 2>/dev/null || true
done

echo "==> assembling runtime payload at $RUNTIME_DIR"
rm -rf "$RUNTIME_DIR"
mkdir -p "$RUNTIME_DIR"
cp -a "$STAGE_DIR/usr" "$RUNTIME_DIR/usr"
rm -rf \
    "$RUNTIME_DIR/usr/include" \
    "$RUNTIME_DIR/usr/lib" \
    "$RUNTIME_DIR/usr/share/aclocal" \
    "$RUNTIME_DIR/usr/share/man"
mkdir -p "$RUNTIME_DIR/var/lib/alsa"

echo "==> done"
echo "    full dev install:    $STAGE_DIR"
echo "    device runtime tree: $RUNTIME_DIR"
echo "    total runtime size:  $(du -sh "$RUNTIME_DIR" | cut -f1)"
echo "    ELF check (aplay):"
"$READELF" -h "$RUNTIME_DIR/usr/bin/aplay" 2>/dev/null | grep -iE "type|flags" | sed 's/^/      /'
echo "    dynamic deps (should be none -- fully static):"
"$READELF" -d "$RUNTIME_DIR/usr/bin/aplay" 2>/dev/null | grep -i needed | sed 's/^/      /'

mkdir -p "$STAGE_DIR"
printf '%s\n' "$PIKO_STATE" > "$PIKO_STAMP"
