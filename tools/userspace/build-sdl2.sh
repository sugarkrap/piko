#!/bin/sh
set -eu

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
SDL2_VERSION="${SDL2_VERSION:-2.30.12}"
SRC_DIR="$REPO/userspace/src"

SDL2_SRC_DIR="${SDL2_SRC_DIR:-$SRC_DIR/SDL2-$SDL2_VERSION}"
SDL2_TARBALL="${SDL2_TARBALL:-$SRC_DIR/SDL2-$SDL2_VERSION.tar.gz}"
SDL2_URL="https://www.libsdl.org/release/SDL2-$SDL2_VERSION.tar.gz"
SDL2_SHA256="ac356ea55e8b9dd0b2d1fa27da40ef7e238267ccf9324704850d5d47375b48ea"

STAGE_DIR="${STAGE_DIR:-$REPO/userspace/stage-sdl2}"
THIRDPARTY_STAGE="${THIRDPARTY_STAGE:-$REPO/userspace/stage-target}"
RUNTIME_DIR="${RUNTIME_DIR:-$REPO/userspace/stage-sdl2-runtime}"

TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin}"
CROSS_COMPILE="${CROSS_COMPILE:-arm-unknown-linux-uclibcgnueabi-}"
JOBS="${JOBS:-$(command -v nproc >/dev/null 2>&1 && nproc || echo 4)}"

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

mkdir -p "$SRC_DIR"

if [ -n "$TOOLCHAIN_BIN_DIR" ] && [ -d "$TOOLCHAIN_BIN_DIR" ]; then
    PATH="$TOOLCHAIN_BIN_DIR:$PATH"
fi
if ! command -v "${CROSS_COMPILE}gcc" >/dev/null 2>&1; then
    echo "tools/userspace/build-sdl2.sh: ${CROSS_COMPILE}gcc not found in PATH." >&2
    echo "Set TOOLCHAIN_BIN_DIR to your toolchain bin path, or export CROSS_COMPILE explicitly." >&2
    exit 1
fi
CC="${CROSS_COMPILE}gcc"
AR="${CROSS_COMPILE}ar"
RANLIB="${CROSS_COMPILE}ranlib"
STRIP="${CROSS_COMPILE}strip"
READELF="${CROSS_COMPILE}readelf"

if [ ! -f "$THIRDPARTY_STAGE/usr/include/X11/Xlib.h" ]; then
    echo "tools/userspace/build-sdl2.sh: no X11 headers under $THIRDPARTY_STAGE" >&2
    echo "  run tools/userspace/build-x11-stack.sh first" >&2
    exit 1
fi

if [ "$FORCE" -eq 1 ] && [ -d "$SDL2_SRC_DIR" ]; then
    echo "==> --force: removing existing $SDL2_SRC_DIR"
    rm -rf "$SDL2_SRC_DIR"
fi

if [ ! -f "$SDL2_TARBALL" ]; then
    echo "==> downloading $SDL2_URL"
    curl -fL --http1.1 -o "$SDL2_TARBALL.partial" "$SDL2_URL"
    mv "$SDL2_TARBALL.partial" "$SDL2_TARBALL"
else
    echo "==> reusing cached $SDL2_TARBALL"
fi

echo "==> verifying sha256"
actual_sha256="$(sha256sum "$SDL2_TARBALL" | cut -d' ' -f1)"
if [ "$actual_sha256" != "$SDL2_SHA256" ]; then
    echo "tools/userspace/build-sdl2.sh: SHA-256 mismatch for $SDL2_TARBALL" >&2
    echo "  expected: $SDL2_SHA256" >&2
    echo "  actual:   $actual_sha256" >&2
    exit 1
fi

if [ ! -d "$SDL2_SRC_DIR" ]; then
    echo "==> extracting SDL2-$SDL2_VERSION"
    tar xzf "$SDL2_TARBALL" -C "$SRC_DIR"
fi

rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR"

echo "==> configuring SDL2 $SDL2_VERSION"
(
    cd "$SDL2_SRC_DIR"
    [ -f Makefile ] && make distclean >/dev/null 2>&1
    ./configure \
        --host=arm-unknown-linux-uclibcgnueabi \
        --build="$(./build-scripts/config.guess 2>/dev/null || echo x86_64-pc-linux-gnu)" \
        --prefix=/usr \
        --disable-static --enable-shared \
        --x-includes="$THIRDPARTY_STAGE/usr/include" \
        --x-libraries="$THIRDPARTY_STAGE/usr/lib" \
        --enable-video-x11 \
        --disable-x11-shared \
        --disable-video-wayland \
        --disable-video-kmsdrm \
        --disable-video-vulkan \
        --disable-video-opengl \
        --disable-video-opengles \
        --disable-video-opengles2 \
        --disable-video-vivante \
        --disable-video-rpi \
        --disable-video-dummy \
        --disable-arm-simd \
        --disable-arm-neon \
        --enable-oss \
        --disable-alsa \
        --disable-pulseaudio \
        --disable-jack \
        --disable-sndio \
        --disable-esd \
        --disable-nas \
        --disable-fusionsound \
        --disable-diskaudio \
        --disable-libsamplerate \
        --disable-dbus \
        --disable-ime \
        --disable-ibus \
        --disable-fcitx \
        --disable-libudev \
        --disable-wayland-shared \
        --disable-hidapi \
        --enable-threads \
        CC="$CC" AR="$AR" RANLIB="$RANLIB" STRIP="$STRIP" \
        PKG_CONFIG=false
    echo "==> building SDL2"
    make -j"$JOBS"
    echo "==> installing SDL2 to $STAGE_DIR"
    make install DESTDIR="$STAGE_DIR"
)

SDL2_SO_REAL="$(cd "$STAGE_DIR/usr/lib" && ls libSDL2-2.0.so.0.* 2>/dev/null | head -1)"
if [ -z "$SDL2_SO_REAL" ]; then
    echo "tools/userspace/build-sdl2.sh: no libSDL2-2.0.so.0.* in $STAGE_DIR/usr/lib" >&2
    exit 1
fi

echo "==> verifying ELF class of $SDL2_SO_REAL"
elf_flags="$("$READELF" -h "$STAGE_DIR/usr/lib/$SDL2_SO_REAL" | sed -n 's/^ *Flags: *//p')"
case "$elf_flags" in
    0x5000200*) : ;;
    *)
        echo "tools/userspace/build-sdl2.sh: unexpected ELF Flags: $elf_flags (want 0x5000200, Version5 EABI, soft-float ABI)" >&2
        exit 1
        ;;
esac
echo "    Flags: $elf_flags"

echo "==> verifying the x11 video driver is linked in, not dlopened"
if ! "$READELF" -d "$STAGE_DIR/usr/lib/$SDL2_SO_REAL" | grep -qi "NEEDED.*libX11"; then
    echo "tools/userspace/build-sdl2.sh: libSDL2 does not NEED libX11 -- x11 video is missing or dynamically loaded" >&2
    exit 1
fi
echo "    NEEDED: $("$READELF" -d "$STAGE_DIR/usr/lib/$SDL2_SO_REAL" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | tr '\n' ' ')"

echo "==> assembling runtime payload at $RUNTIME_DIR"
rm -rf "$RUNTIME_DIR"
mkdir -p "$RUNTIME_DIR/usr/lib"
cp "$STAGE_DIR/usr/lib/$SDL2_SO_REAL" "$RUNTIME_DIR/usr/lib/"
chmod u+w "$RUNTIME_DIR/usr/lib/$SDL2_SO_REAL"
"$STRIP" --strip-unneeded "$RUNTIME_DIR/usr/lib/$SDL2_SO_REAL"
ln -sf "$SDL2_SO_REAL" "$RUNTIME_DIR/usr/lib/libSDL2-2.0.so.0"

echo ""
echo "==> done: $RUNTIME_DIR assembled"
echo "    $RUNTIME_DIR/usr/lib/$SDL2_SO_REAL ($(du -h "$RUNTIME_DIR/usr/lib/$SDL2_SO_REAL" | cut -f1))"
