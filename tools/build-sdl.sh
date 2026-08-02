#!/bin/sh
set -eu

# Cross-compiles SDL 1.2 (the last 1.x release, 1.2.15) for the Zaurus
# SL-C760 (PXA255, ARMv5TE, soft-float, uClibc) as a SHARED library
# (libSDL-1.2.so.0), plus a tiny dummy smoke-test app that opens the
# framebuffer video mode and draws to it -- see userspace/src/sdltest.c.
#
# WHY SHARED, UNLIKE ALSA/MPLAYER: this project's rootfs shipped with no
# dynamic linker at all (see tools/build-alsa.sh's header), so every
# component built so far is static. That changed with the X11/matchbox
# stack (tools/deploy-x11.sh): Xfbdev/libX11/etc are dynamically linked,
# and deploying them bootstraps /lib/ld-uClibc*.so + /lib/libc.so onto the
# device for the first time. SDL is meant to become a shared library other
# things link against later (game/UI code, maybe SDL_image/SDL_mixer), so
# it follows the X11 stack's convention (dynamic, SONAME-versioned .so),
# not ALSA/MPlayer's (static). tools/chunked-deploy.sh's SDL step (part of
# section 8) bootstraps the same ld-uClibc/libc runtime independently, so
# it works even before/without ever deploying the X11 payload.
#
# WHY FBCON, NOT X11: the goal right now is the smallest thing that proves
# SDL can drive this hardware at all -- open /dev/fb0, set a video mode,
# blit pixels. SDL's own X11 backend would need libX11/libXext headers
# and would run as a client of the existing Xfbdev server; that's a
# reasonable follow-up (SDL_VIDEODRIVER=x11) but adds a real dependency
# chain (userspace/stage-target) for no benefit to "does SDL work at
# all," so this build disables it explicitly rather than let configure's
# host pkg-config accidentally find something. Same reasoning MPlayer
# used for -vo fbdev over X11 video extensions.
#
# WHY AUDIO IS OFF FOR NOW: SDL_INIT_AUDIO is not needed by the dummy
# video smoke test, and wiring SDL's ALSA backend against
# userspace/stage-alsa's *static-only* libasound.a into a *shared*
# libSDL.so is its own can of worms (a shared lib pulling in a static
# archive works, but bakes ALSA's ABI into every future libSDL.so
# rebuild). Deferred to when something actually needs SDL_mixer/audio;
# --disable-audio keeps this build's scope to video only.
#
# WHY JOYSTICK IS ON despite this board having no joystick: the symbol
# table matters more than the hardware here. SDL_Init(SDL_INIT_JOYSTICK)
# and the SDL_Joystick* calls are things application code (e.g. gmenunx's
# InputManager) links against unconditionally, gated at runtime, not link
# time, by whether it finds any device. A libSDL built --disable-joystick
# simply doesn't export those symbols at all, so any such app fails to
# link, not just to detect a joystick. Enabled = SDL_NumJoysticks() returns
# 0 and everything else no-ops; disabled = a hard link error for every
# consumer that touches the joystick API, whether or not it's ever called.
#
# Cross-compile notes:
#   - Toolchain defaults already target -march=armv5tej -mfloat-abi=soft
#     (matching the PXA255 exactly, confirmed via `gcc -Q --help=target`
#     in tools/build-mplayer.sh) -- no extra -march/-mfpu flags needed.
#   - configure's --disable-* flags for GGI/SVGA/AAlib/CACA/DirectFB/etc
#     are largely redundant with cross-compiling (their headers don't
#     exist in this sysroot so configure autodetects them off anyway) but
#     are passed explicitly so a future host with those dev packages
#     installed can't accidentally change what gets built.
#
# Usage:
#   tools/build-sdl.sh [--force]
#
# --force wipes and re-extracts the source tree even if already present
# (the configure/build/install steps always rerun regardless -- cheap).
#
# Env overrides:
#   SDL_VERSION         default 1.2.15
#   TOOLCHAIN_BIN_DIR   default <repo>/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin
#   CROSS_COMPILE       default arm-unknown-linux-uclibcgnueabi-
#   STAGE_DIR           default <repo>/userspace/stage-sdl (full dev install)
#   RUNTIME_DIR         default <repo>/userspace/stage-sdl-runtime (device payload)
#   JOBS                default: nproc
#
# Exit codes:
#   0   $RUNTIME_DIR was assembled successfully (libSDL-1.2.so.0.11.4 +
#       SONAME symlinks + the sdltest binary)
#   1   a hard failure (download, checksum, configure, build, install, or
#       ELF-verification failure)

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
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
    echo "tools/build-sdl.sh: ${CROSS_COMPILE}gcc not found in PATH." >&2
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
    echo "tools/build-sdl.sh: SHA-256 mismatch for $SDL_TARBALL" >&2
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
    echo "tools/build-sdl.sh: $SDL_SRC_DIR doesn't look like a configure-based tree" >&2
    exit 1
fi

# NOTE: this wipes the whole of $STAGE_DIR, not just libSDL's own files.
# tools/build-sdl-image.sh and tools/build-sdl-ttf.sh install into this
# same directory (see their own headers for why) -- after rebuilding SDL
# here, re-run those two as well, or their libraries/headers are gone.
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR"

# --- 1. SDL itself -----------------------------------------------------
# --prefix=/usr (not a staging path) so sdl-config and any future
# dependent build gets the real on-device install location baked in.
# --disable-static --enable-shared: see header comment for why this one
# component goes dynamic against this project's established static-only
# convention.
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

# Same libtool trap as tools/build-alsa.sh (see its header comment,
# trap #1): rewrite the installed .la's libdir= to the real staging
# location so anything cross-linking against libSDL.la next (e.g. the
# sdltest build below) resolves OUR cross-built libSDL.so, not a
# same-path native one on the build host.
LIBSDL_LA="$STAGE_DIR/usr/lib/libSDL.la"
if [ -f "$LIBSDL_LA" ]; then
    sed -i "s|^libdir='/usr/lib'|libdir='$STAGE_DIR/usr/lib'|" "$LIBSDL_LA"
fi

SDL_SO_REAL="$(cd "$STAGE_DIR/usr/lib" && ls libSDL-1.2.so.0.* 2>/dev/null | head -1)"
if [ -z "$SDL_SO_REAL" ]; then
    echo "tools/build-sdl.sh: build finished but no libSDL-1.2.so.0.* found in $STAGE_DIR/usr/lib" >&2
    exit 1
fi

echo "==> verifying ELF class of $SDL_SO_REAL"
elf_flags="$("$READELF" -h "$STAGE_DIR/usr/lib/$SDL_SO_REAL" | sed -n 's/^ *Flags: *//p')"
case "$elf_flags" in
    0x5000200*) : ;;
    *)
        echo "tools/build-sdl.sh: unexpected ELF Flags: $elf_flags (want 0x5000200, Version5 EABI, soft-float ABI -- matching Xfbdev/libX11)" >&2
        exit 1
        ;;
esac
echo "    Flags: $elf_flags"

# --- 2. sdltest (dummy smoke-test app, see userspace/src/sdltest.c) -----
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
            echo "tools/build-sdl.sh: sdltest does not NEED libSDL-1.2.so.0 -- something linked wrong" >&2
            exit 1
            ;;
    esac
else
    echo "==> skipping sdltest (no $SDLTEST_SRC)"
fi

# --- 2b. pikalibrate (touchscreen calibration app, see userspace/src/pikalibrate.c) --
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
            echo "tools/build-sdl.sh: pikalibrate does not NEED libSDL-1.2.so.0 -- something linked wrong" >&2
            exit 1
            ;;
    esac
else
    echo "==> skipping pikalibrate (no $PIKALIBRATE_SRC)"
fi

# --- 3. assemble the device-payload runtime tree ------------------------
# Mirrors tools/build-alsa.sh's stage/stage-runtime split: STAGE_DIR keeps
# the full dev install (headers, .la, sdl-config) for anything that wants
# to link against libSDL next; RUNTIME_DIR is the pruned, wholesale-
# copyable payload tools/chunked-deploy.sh actually ships.
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
