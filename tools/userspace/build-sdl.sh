#!/bin/sh
set -eu

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
SDL_VERSION="${SDL_VERSION:-1.2.15}"
SRC_DIR="$REPO/userspace/src"

SDL_SRC_DIR="${SDL_SRC_DIR:-$SRC_DIR/SDL-$SDL_VERSION}"
SDL_TARBALL="${SDL_TARBALL:-$SRC_DIR/SDL-$SDL_VERSION.tar.gz}"
SDL_URL="https://www.libsdl.org/release/SDL-$SDL_VERSION.tar.gz"
SDL_SHA256="d6d316a793e5e348155f0dd93b979798933fb98aa1edebcc108829d6474aad00"

STAGE_DIR="${STAGE_DIR:-$REPO/userspace/stage-sdl}"
RUNTIME_DIR="${RUNTIME_DIR:-$REPO/userspace/stage-sdl-runtime}"

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
    echo "tools/userspace/build-sdl.sh: ${CROSS_COMPILE}gcc not found in PATH." >&2
    echo "Set TOOLCHAIN_BIN_DIR to your toolchain bin path, or export CROSS_COMPILE explicitly." >&2
    exit 1
fi
CC="${CROSS_COMPILE}gcc"
AR="${CROSS_COMPILE}ar"
RANLIB="${CROSS_COMPILE}ranlib"
STRIP="${CROSS_COMPILE}strip"
READELF="${CROSS_COMPILE}readelf"

if [ "$FORCE" -eq 1 ] && [ -d "$SDL_SRC_DIR" ]; then
    echo "==> --force: removing existing $SDL_SRC_DIR"
    rm -rf "$SDL_SRC_DIR"
fi

if [ ! -f "$SDL_TARBALL" ]; then
    echo "==> downloading $SDL_URL"
    curl -fL -o "$SDL_TARBALL.partial" "$SDL_URL"
    mv "$SDL_TARBALL.partial" "$SDL_TARBALL"
else
    echo "==> reusing cached $SDL_TARBALL"
fi

echo "==> verifying sha256"
actual_sha256="$(sha256sum "$SDL_TARBALL" | cut -d' ' -f1)"
if [ "$actual_sha256" != "$SDL_SHA256" ]; then
    echo "tools/userspace/build-sdl.sh: SHA-256 mismatch for $SDL_TARBALL" >&2
    echo "  expected: $SDL_SHA256" >&2
    echo "  actual:   $actual_sha256" >&2
    echo "Refusing to build from a tarball that doesn't match -- remove it and rerun" >&2
    echo "if you deliberately changed SDL_VERSION/SDL_URL." >&2
    exit 1
fi

if [ ! -d "$SDL_SRC_DIR" ]; then
    echo "==> extracting SDL-$SDL_VERSION to $SRC_DIR"
    tar xzf "$SDL_TARBALL" -C "$SRC_DIR"
fi
if [ ! -f "$SDL_SRC_DIR/configure" ]; then
    echo "tools/userspace/build-sdl.sh: $SDL_SRC_DIR doesn't look like a configure-based tree" >&2
    exit 1
fi

rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR"

echo "==> configuring SDL $SDL_VERSION"
(
    cd "$SDL_SRC_DIR"
    [ -f Makefile ] && make distclean >/dev/null 2>&1
    ./configure \
        --host=arm-unknown-linux-uclibcgnueabi \
        --build="$(./config.guess 2>/dev/null || echo x86_64-pc-linux-gnu)" \
        --prefix=/usr \
        --disable-static --enable-shared \
        --disable-audio \
        --disable-video-x11 \
        --disable-video-dga \
        --disable-video-ggi \
        --disable-video-svga \
        --disable-video-aalib \
        --disable-video-caca \
        --disable-video-directfb \
        --disable-video-ps2gs \
        --disable-video-ps3 \
        --disable-video-qtopia \
        --disable-video-picogui \
        --disable-video-photon \
        --disable-video-nanox \
        --disable-video-riscos \
        --enable-video-fbcon \
        --disable-video-opengl \
        --disable-cdrom \
        --enable-joystick \
        --enable-threads \
        --disable-nasm \
        --disable-oss \
        CC="$CC" AR="$AR" RANLIB="$RANLIB" STRIP="$STRIP"
    echo "==> building SDL"
    make -j"$JOBS"
    echo "==> installing SDL to $STAGE_DIR"
    make install DESTDIR="$STAGE_DIR"
)

LIBSDL_LA="$STAGE_DIR/usr/lib/libSDL.la"
if [ -f "$LIBSDL_LA" ]; then
    sed -i "s|^libdir='/usr/lib'|libdir='$STAGE_DIR/usr/lib'|" "$LIBSDL_LA"
fi

SDL_SO_REAL="$(cd "$STAGE_DIR/usr/lib" && ls libSDL-1.2.so.0.* 2>/dev/null | head -1)"
if [ -z "$SDL_SO_REAL" ]; then
    echo "tools/userspace/build-sdl.sh: build finished but no libSDL-1.2.so.0.* found in $STAGE_DIR/usr/lib" >&2
    exit 1
fi

echo "==> verifying ELF class of $SDL_SO_REAL"
elf_flags="$("$READELF" -h "$STAGE_DIR/usr/lib/$SDL_SO_REAL" | sed -n 's/^ *Flags: *//p')"
case "$elf_flags" in
    0x5000200*) : ;;
    *)
        echo "tools/userspace/build-sdl.sh: unexpected ELF Flags: $elf_flags (want 0x5000200, Version5 EABI, soft-float ABI -- matching Xfbdev/libX11)" >&2
        exit 1
        ;;
esac
echo "    Flags: $elf_flags"

SDLTEST_SRC="$REPO/userspace/src/sdltest.c"
if [ -f "$SDLTEST_SRC" ]; then
    echo "==> building sdltest against the freshly staged libSDL"
    "$CC" -O2 -Wall -Wextra \
        -I"$STAGE_DIR/usr/include/SDL" \
        -o "$STAGE_DIR/usr/bin/.sdltest.tmp" \
        "$SDLTEST_SRC" \
        -L"$STAGE_DIR/usr/lib" -lSDL -lpthread -lm -ldl
    mkdir -p "$STAGE_DIR/usr/bin"
    mv "$STAGE_DIR/usr/bin/.sdltest.tmp" "$STAGE_DIR/usr/bin/sdltest"

    echo "==> verifying sdltest links only against expected shared libs"
    needed="$("$READELF" -d "$STAGE_DIR/usr/bin/sdltest" 2>/dev/null | awk '/NEEDED/{print $NF}' | tr -d '[]')"
    echo "    NEEDED: $(echo "$needed" | tr '\n' ' ')"
    case "$needed" in
        *libSDL-1.2.so.0*) : ;;
        *)
            echo "tools/userspace/build-sdl.sh: sdltest does not NEED libSDL-1.2.so.0 -- something linked wrong" >&2
            exit 1
            ;;
    esac
else
    echo "==> skipping sdltest (no $SDLTEST_SRC)"
fi

PIKALIBRATE_SRC="$REPO/userspace/src/pikalibrate.c"
if [ -f "$PIKALIBRATE_SRC" ]; then
    echo "==> building pikalibrate against the freshly staged libSDL"
    "$CC" -O2 -Wall -Wextra \
        -I"$STAGE_DIR/usr/include/SDL" \
        -o "$STAGE_DIR/usr/bin/.pikalibrate.tmp" \
        "$PIKALIBRATE_SRC" \
        -L"$STAGE_DIR/usr/lib" -lSDL -lpthread -lm -ldl
    mkdir -p "$STAGE_DIR/usr/bin"
    mv "$STAGE_DIR/usr/bin/.pikalibrate.tmp" "$STAGE_DIR/usr/bin/pikalibrate"

    echo "==> verifying pikalibrate links only against expected shared libs"
    needed="$("$READELF" -d "$STAGE_DIR/usr/bin/pikalibrate" 2>/dev/null | awk '/NEEDED/{print $NF}' | tr -d '[]')"
    echo "    NEEDED: $(echo "$needed" | tr '\n' ' ')"
    case "$needed" in
        *libSDL-1.2.so.0*) : ;;
        *)
            echo "tools/userspace/build-sdl.sh: pikalibrate does not NEED libSDL-1.2.so.0 -- something linked wrong" >&2
            exit 1
            ;;
    esac
else
    echo "==> skipping pikalibrate (no $PIKALIBRATE_SRC)"
fi

mkdir -p "$RUNTIME_DIR/usr/lib" "$RUNTIME_DIR/usr/bin"
cp "$STAGE_DIR/usr/lib/$SDL_SO_REAL" "$RUNTIME_DIR/usr/lib/"
chmod u+w "$RUNTIME_DIR/usr/lib/$SDL_SO_REAL"
"$STRIP" --strip-unneeded "$RUNTIME_DIR/usr/lib/$SDL_SO_REAL"
ln -sf "$SDL_SO_REAL" "$RUNTIME_DIR/usr/lib/libSDL-1.2.so.0"

if [ -f "$STAGE_DIR/usr/bin/sdltest" ]; then
    cp "$STAGE_DIR/usr/bin/sdltest" "$RUNTIME_DIR/usr/bin/sdltest"
    chmod u+w "$RUNTIME_DIR/usr/bin/sdltest"
    "$STRIP" --strip-unneeded "$RUNTIME_DIR/usr/bin/sdltest"
fi
if [ -f "$STAGE_DIR/usr/bin/pikalibrate" ]; then
    cp "$STAGE_DIR/usr/bin/pikalibrate" "$RUNTIME_DIR/usr/bin/pikalibrate"
    chmod u+w "$RUNTIME_DIR/usr/bin/pikalibrate"
    "$STRIP" --strip-unneeded "$RUNTIME_DIR/usr/bin/pikalibrate"
fi

echo ""
echo "==> done: $RUNTIME_DIR assembled"
echo "    $RUNTIME_DIR/usr/lib/$SDL_SO_REAL"
echo "    $RUNTIME_DIR/usr/lib/libSDL-1.2.so.0 -> $SDL_SO_REAL"
[ -f "$RUNTIME_DIR/usr/bin/sdltest" ] && echo "    $RUNTIME_DIR/usr/bin/sdltest"
[ -f "$RUNTIME_DIR/usr/bin/pikalibrate" ] && echo "    $RUNTIME_DIR/usr/bin/pikalibrate"
echo ""
echo "    Deploy with tools/chunked-deploy.sh (or tools/build-and-deploy.sh,"
echo "    which calls it) -- it ships this payload automatically once staged"
echo "    here, bootstrapping /lib/ld-uClibc*.so + /lib/libc.so if needed."
