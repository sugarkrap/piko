#!/bin/sh
set -eu

# Cross-compiles SDL_image 1.2.12 (the last SDL1-series release) for the
# Zaurus SL-C760 (PXA255, ARMv5TE, soft-float, uClibc) as a SHARED library
# (libSDL_image-1.2.so.0), against libSDL (tools/build-sdl.sh) and libpng +
# zlib (tools/build-thirdparty-deps.sh, already staged for the X11 stack).
#
# WHY PNG ONLY: the only image loader anything on this device currently
# needs is gmenunx's icon/wallpaper set, which is entirely .png (the
# RetroFW-derived skin it ships, see gmenunx.mk's `dist` target -- the only
# .jpg wallpapers live in an alternate skin that isn't packaged). JPEG, TIFF
# and WEBP support each pull in their own cross-built library (libjpeg,
# libtiff, libwebp) for zero current consumers, so they're disabled
# explicitly rather than left to configure's autodetection.
#
# WHY --disable-png-shared: SDL_image's default PNG backend dlopen()s
# libpng at runtime (so a binary can ship without a hard PNG dependency).
# That's the wrong tradeoff here -- libpng is always staged and always
# present -- and dlopen() on this rootfs is a comparatively new, less-
# exercised path (the X11 stack is the first thing here to need a dynamic
# linker at all, see build-sdl.sh's header). Direct linking is simpler and
# one less runtime failure mode.
#
# WHY SHARED: matches libSDL's own convention (see build-sdl.sh) -- this
# device now has a dynamic linker (bootstrapped by the X11 stack deploy),
# and libSDL_image is meant to be linked by application code the same way
# libSDL itself is.
#
# Usage:
#   tools/build-sdl-image.sh [--force]
#
# Env overrides:
#   SDL_IMAGE_VERSION   default 1.2.12
#   TOOLCHAIN_BIN_DIR   default <repo>/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin
#   CROSS_COMPILE       default arm-unknown-linux-uclibcgnueabi-
#   SDL_STAGE_DIR       default <repo>/userspace/stage-sdl (libSDL + sdl-config)
#   THIRDPARTY_STAGE    default <repo>/userspace/stage-target (libpng, zlib)
#   STAGE_DIR           default <repo>/userspace/stage-sdl (installed alongside libSDL)
#   RUNTIME_DIR         default <repo>/userspace/stage-sdl-runtime
#   JOBS                default: nproc
#
# Exit codes:
#   0   $RUNTIME_DIR/usr/lib/libSDL_image-1.2.so.0.* assembled successfully
#   1   a hard failure (missing prerequisite stage, download, checksum,
#       configure, build, install, or ELF-verification failure)

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
SDL_IMAGE_VERSION="${SDL_IMAGE_VERSION:-1.2.12}"
SRC_DIR="$REPO/userspace/src"

SDLIMG_SRC_DIR="${SDLIMG_SRC_DIR:-$SRC_DIR/SDL_image-$SDL_IMAGE_VERSION}"
SDLIMG_TARBALL="${SDLIMG_TARBALL:-$SRC_DIR/SDL_image-$SDL_IMAGE_VERSION.tar.gz}"
SDLIMG_URL="https://www.libsdl.org/projects/SDL_image/release/SDL_image-$SDL_IMAGE_VERSION.tar.gz"
SDLIMG_SHA256="0b90722984561004de84847744d566809dbb9daf732a9e503b91a1b5a84e5699"

SDL_STAGE_DIR="${SDL_STAGE_DIR:-$REPO/userspace/stage-sdl}"
THIRDPARTY_STAGE="${THIRDPARTY_STAGE:-$REPO/userspace/stage-target}"
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
    echo "tools/build-sdl-image.sh: ${CROSS_COMPILE}gcc not found in PATH." >&2
    echo "Set TOOLCHAIN_BIN_DIR to your toolchain bin path, or export CROSS_COMPILE explicitly." >&2
    exit 1
fi
CC="${CROSS_COMPILE}gcc"
AR="${CROSS_COMPILE}ar"
RANLIB="${CROSS_COMPILE}ranlib"
STRIP="${CROSS_COMPILE}strip"
READELF="${CROSS_COMPILE}readelf"

if [ ! -x "$SDL_STAGE_DIR/usr/bin/sdl-config" ]; then
    echo "tools/build-sdl-image.sh: $SDL_STAGE_DIR/usr/bin/sdl-config not found." >&2
    echo "Run tools/build-sdl.sh first." >&2
    exit 1
fi
if [ ! -f "$THIRDPARTY_STAGE/usr/lib/pkgconfig/libpng.pc" ]; then
    echo "tools/build-sdl-image.sh: libpng not staged in $THIRDPARTY_STAGE." >&2
    echo "Run tools/build-thirdparty-deps.sh libpng first." >&2
    exit 1
fi

if [ "$FORCE" -eq 1 ] && [ -d "$SDLIMG_SRC_DIR" ]; then
    echo "==> --force: removing existing $SDLIMG_SRC_DIR"
    rm -rf "$SDLIMG_SRC_DIR"
fi

if [ ! -f "$SDLIMG_TARBALL" ]; then
    echo "==> downloading $SDLIMG_URL"
    curl -fL -o "$SDLIMG_TARBALL.partial" "$SDLIMG_URL"
    mv "$SDLIMG_TARBALL.partial" "$SDLIMG_TARBALL"
else
    echo "==> reusing cached $SDLIMG_TARBALL"
fi

echo "==> verifying sha256"
actual_sha256="$(sha256sum "$SDLIMG_TARBALL" | cut -d' ' -f1)"
if [ "$actual_sha256" != "$SDLIMG_SHA256" ]; then
    echo "tools/build-sdl-image.sh: SHA-256 mismatch for $SDLIMG_TARBALL" >&2
    echo "  expected: $SDLIMG_SHA256" >&2
    echo "  actual:   $actual_sha256" >&2
    exit 1
fi

if [ ! -d "$SDLIMG_SRC_DIR" ]; then
    echo "==> extracting SDL_image-$SDL_IMAGE_VERSION to $SRC_DIR"
    tar xzf "$SDLIMG_TARBALL" -C "$SRC_DIR"
fi
if [ ! -f "$SDLIMG_SRC_DIR/configure" ]; then
    echo "tools/build-sdl-image.sh: $SDLIMG_SRC_DIR doesn't look like a configure-based tree" >&2
    exit 1
fi

# sdl-config was installed with --prefix=/usr (see build-sdl.sh: the ON-
# DEVICE path is deliberately baked in for anything that links against it
# on the device later). For OUR cross-build we need it to report where
# libSDL is actually sitting right now, which sdl-config supports via a
# --prefix override on each invocation -- wrap it so every caller
# (AM_PATH_SDL's own `$SDL_CONFIG --cflags`/`--libs` probing included) gets
# that override transparently, without editing the installed script.
SDL_CONFIG_WRAPPER="$SRC_DIR/.sdl-config-wrapper-image"
cat > "$SDL_CONFIG_WRAPPER" <<EOF
#!/bin/sh
exec "$SDL_STAGE_DIR/usr/bin/sdl-config" --prefix="$SDL_STAGE_DIR/usr" "\$@"
EOF
chmod +x "$SDL_CONFIG_WRAPPER"

export PKG_CONFIG_SYSROOT_DIR="$THIRDPARTY_STAGE"
export PKG_CONFIG_LIBDIR="$THIRDPARTY_STAGE/usr/lib/pkgconfig:$THIRDPARTY_STAGE/usr/share/pkgconfig"
export PKG_CONFIG_PATH=
export CPPFLAGS="-I$THIRDPARTY_STAGE/usr/include"
export LDFLAGS="-L$THIRDPARTY_STAGE/usr/lib -Wl,-rpath-link=$THIRDPARTY_STAGE/usr/lib"

echo "==> configuring SDL_image $SDL_IMAGE_VERSION"
(
    cd "$SDLIMG_SRC_DIR"
    [ -f Makefile ] && make distclean >/dev/null 2>&1
    ./configure \
        --host=arm-unknown-linux-uclibcgnueabi \
        --build="$(./config.guess 2>/dev/null || echo x86_64-pc-linux-gnu)" \
        --prefix=/usr \
        --disable-static --enable-shared \
        --enable-png --disable-png-shared \
        --disable-jpg \
        --disable-tif \
        --disable-webp \
        --disable-pnm \
        --disable-xpm \
        --disable-xcf \
        --disable-xv \
        --disable-lbm \
        --disable-pcx \
        --disable-tga \
        --disable-gif \
        --disable-sdltest \
        CC="$CC" AR="$AR" RANLIB="$RANLIB" STRIP="$STRIP" \
        SDL_CONFIG="$SDL_CONFIG_WRAPPER"
    echo "==> building SDL_image"
    make -j"$JOBS"
    echo "==> installing SDL_image to $STAGE_DIR"
    make install DESTDIR="$STAGE_DIR"
)

# Same libtool trap as tools/build-sdl.sh / tools/build-alsa.sh: rewrite the
# installed .la's libdir= to the real staging location.
LIBSDLIMG_LA="$STAGE_DIR/usr/lib/libSDL_image.la"
if [ -f "$LIBSDLIMG_LA" ]; then
    sed -i "s|^libdir='/usr/lib'|libdir='$STAGE_DIR/usr/lib'|" "$LIBSDLIMG_LA"
fi

SDLIMG_SO_REAL="$(cd "$STAGE_DIR/usr/lib" && ls libSDL_image-1.2.so.0.* 2>/dev/null | head -1)"
if [ -z "$SDLIMG_SO_REAL" ]; then
    echo "tools/build-sdl-image.sh: build finished but no libSDL_image-1.2.so.0.* found in $STAGE_DIR/usr/lib" >&2
    exit 1
fi

echo "==> verifying ELF class of $SDLIMG_SO_REAL"
elf_flags="$("$READELF" -h "$STAGE_DIR/usr/lib/$SDLIMG_SO_REAL" | sed -n 's/^ *Flags: *//p')"
case "$elf_flags" in
    0x5000200*) : ;;
    *)
        echo "tools/build-sdl-image.sh: unexpected ELF Flags: $elf_flags (want 0x5000200, Version5 EABI, soft-float ABI)" >&2
        exit 1
        ;;
esac
echo "    Flags: $elf_flags"

echo "==> verifying $SDLIMG_SO_REAL links against libpng and libSDL, not jpeg/tiff/webp"
needed="$("$READELF" -d "$STAGE_DIR/usr/lib/$SDLIMG_SO_REAL" 2>/dev/null | awk '/NEEDED/{print $NF}' | tr -d '[]')"
echo "    NEEDED: $(echo "$needed" | tr '\n' ' ')"
case "$needed" in
    *libjpeg*|*libtiff*|*libwebp*)
        echo "tools/build-sdl-image.sh: unexpectedly linked against jpeg/tiff/webp" >&2
        exit 1
        ;;
esac
case "$needed" in
    *libpng16.so*) : ;;
    *)
        echo "tools/build-sdl-image.sh: $SDLIMG_SO_REAL does not NEED libpng16.so -- PNG support did not link in" >&2
        exit 1
        ;;
esac

# --- assemble the device-payload runtime tree --------------------------
mkdir -p "$RUNTIME_DIR/usr/lib"
cp "$STAGE_DIR/usr/lib/$SDLIMG_SO_REAL" "$RUNTIME_DIR/usr/lib/"
chmod u+w "$RUNTIME_DIR/usr/lib/$SDLIMG_SO_REAL"
"$STRIP" --strip-unneeded "$RUNTIME_DIR/usr/lib/$SDLIMG_SO_REAL"
ln -sf "$SDLIMG_SO_REAL" "$RUNTIME_DIR/usr/lib/libSDL_image-1.2.so.0"

# libpng/zlib are needed at runtime too -- they're staged for the X11 stack
# (userspace/stage-target) but that tree isn't itself part of the SDL
# runtime payload, so mirror the two .so's this pulls in.
for lib in libpng16.so.16 libz.so.1; do
    real="$(cd "$THIRDPARTY_STAGE/usr/lib" && ls "$lib".* 2>/dev/null | head -1)"
    [ -z "$real" ] && real="$lib"
    if [ -f "$THIRDPARTY_STAGE/usr/lib/$real" ]; then
        cp "$THIRDPARTY_STAGE/usr/lib/$real" "$RUNTIME_DIR/usr/lib/"
        chmod u+w "$RUNTIME_DIR/usr/lib/$real"
        "$STRIP" --strip-unneeded "$RUNTIME_DIR/usr/lib/$real" 2>/dev/null || true
        ln -sf "$real" "$RUNTIME_DIR/usr/lib/$lib"
    fi
done

echo ""
echo "==> done: $RUNTIME_DIR assembled"
echo "    $RUNTIME_DIR/usr/lib/$SDLIMG_SO_REAL"
echo "    $RUNTIME_DIR/usr/lib/libSDL_image-1.2.so.0 -> $SDLIMG_SO_REAL"
echo "    + libpng16.so.16, libz.so.1 (runtime deps mirrored from stage-target)"
