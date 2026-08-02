#!/bin/sh
set -eu

# Cross-compiles SDL_ttf 2.0.11 (the last SDL1-series release) for the
# Zaurus SL-C760 (PXA255, ARMv5TE, soft-float, uClibc) as a SHARED library
# (libSDL_ttf-2.0.so.0), against libSDL (tools/build-sdl.sh) and freetype
# (tools/build-thirdparty-deps.sh, already staged for the X11/Matchbox
# stack's font rendering).
#
# WHY A SYNTHETIC freetype-config: SDL_ttf 2.0.11's configure.in only knows
# how to ask a `freetype-config` script for --cflags/--libs (pkg-config
# support for freetype came later upstream). tools/build-thirdparty-deps.sh
# staged freetype via plain autotools `make install`, which for this
# release does not install a freetype-config binary (superseded by the
# freetype2.pc it does install) -- so there is nothing at
# $THIRDPARTY_STAGE/usr/bin/freetype-config to point SDL_ttf's configure
# at. Rather than patch freetype's own build to emit one, this script
# writes a tiny wrapper that answers freetype-config's handful of flags by
# shelling out to the (host, cross-target-agnostic) `pkg-config` binary
# against the staged freetype2.pc -- same underlying data, different
# calling convention.
#
# WHY SHARED: matches libSDL's own convention (see build-sdl.sh).
#
# Usage:
#   tools/build-sdl-ttf.sh [--force]
#
# Env overrides:
#   SDL_TTF_VERSION     default 2.0.11
#   TOOLCHAIN_BIN_DIR   default <repo>/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin
#   CROSS_COMPILE       default arm-unknown-linux-uclibcgnueabi-
#   SDL_STAGE_DIR       default <repo>/userspace/stage-sdl (libSDL + sdl-config)
#   THIRDPARTY_STAGE    default <repo>/userspace/stage-target (freetype)
#   STAGE_DIR           default <repo>/userspace/stage-sdl (installed alongside libSDL)
#   RUNTIME_DIR         default <repo>/userspace/stage-sdl-runtime
#   JOBS                default: nproc
#
# Exit codes:
#   0   $RUNTIME_DIR/usr/lib/libSDL_ttf-2.0.so.0.* assembled successfully
#   1   a hard failure (missing prerequisite stage, download, checksum,
#       configure, build, install, or ELF-verification failure)

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
SDL_TTF_VERSION="${SDL_TTF_VERSION:-2.0.11}"
SRC_DIR="$REPO/userspace/src"

SDLTTF_SRC_DIR="${SDLTTF_SRC_DIR:-$SRC_DIR/SDL_ttf-$SDL_TTF_VERSION}"
SDLTTF_TARBALL="${SDLTTF_TARBALL:-$SRC_DIR/SDL_ttf-$SDL_TTF_VERSION.tar.gz}"
SDLTTF_URL="https://www.libsdl.org/projects/SDL_ttf/release/SDL_ttf-$SDL_TTF_VERSION.tar.gz"
SDLTTF_SHA256="724cd895ecf4da319a3ef164892b72078bd92632a5d812111261cde248ebcdb7"

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
    echo "tools/build-sdl-ttf.sh: ${CROSS_COMPILE}gcc not found in PATH." >&2
    echo "Set TOOLCHAIN_BIN_DIR to your toolchain bin path, or export CROSS_COMPILE explicitly." >&2
    exit 1
fi
CC="${CROSS_COMPILE}gcc"
AR="${CROSS_COMPILE}ar"
RANLIB="${CROSS_COMPILE}ranlib"
STRIP="${CROSS_COMPILE}strip"
READELF="${CROSS_COMPILE}readelf"

if [ ! -x "$SDL_STAGE_DIR/usr/bin/sdl-config" ]; then
    echo "tools/build-sdl-ttf.sh: $SDL_STAGE_DIR/usr/bin/sdl-config not found." >&2
    echo "Run tools/build-sdl.sh first." >&2
    exit 1
fi
if [ ! -f "$THIRDPARTY_STAGE/usr/lib/pkgconfig/freetype2.pc" ]; then
    echo "tools/build-sdl-ttf.sh: freetype not staged in $THIRDPARTY_STAGE." >&2
    echo "Run tools/build-thirdparty-deps.sh freetype first." >&2
    exit 1
fi
if ! command -v pkg-config >/dev/null 2>&1; then
    echo "tools/build-sdl-ttf.sh: host pkg-config not found (needed to read the staged freetype2.pc)." >&2
    exit 1
fi

if [ "$FORCE" -eq 1 ] && [ -d "$SDLTTF_SRC_DIR" ]; then
    echo "==> --force: removing existing $SDLTTF_SRC_DIR"
    rm -rf "$SDLTTF_SRC_DIR"
fi

if [ ! -f "$SDLTTF_TARBALL" ]; then
    echo "==> downloading $SDLTTF_URL"
    curl -fL -o "$SDLTTF_TARBALL.partial" "$SDLTTF_URL"
    mv "$SDLTTF_TARBALL.partial" "$SDLTTF_TARBALL"
else
    echo "==> reusing cached $SDLTTF_TARBALL"
fi

echo "==> verifying sha256"
actual_sha256="$(sha256sum "$SDLTTF_TARBALL" | cut -d' ' -f1)"
if [ "$actual_sha256" != "$SDLTTF_SHA256" ]; then
    echo "tools/build-sdl-ttf.sh: SHA-256 mismatch for $SDLTTF_TARBALL" >&2
    echo "  expected: $SDLTTF_SHA256" >&2
    echo "  actual:   $actual_sha256" >&2
    exit 1
fi

if [ ! -d "$SDLTTF_SRC_DIR" ]; then
    echo "==> extracting SDL_ttf-$SDL_TTF_VERSION to $SRC_DIR"
    tar xzf "$SDLTTF_TARBALL" -C "$SRC_DIR"
fi
if [ ! -f "$SDLTTF_SRC_DIR/configure" ]; then
    echo "tools/build-sdl-ttf.sh: $SDLTTF_SRC_DIR doesn't look like a configure-based tree" >&2
    exit 1
fi

# See header comment: sdl-config needs a --prefix override to report where
# libSDL is actually staged right now (it was installed with --prefix=/usr
# baked in for the on-device path, per build-sdl.sh).
SDL_CONFIG_WRAPPER="$SRC_DIR/.sdl-config-wrapper-ttf"
cat > "$SDL_CONFIG_WRAPPER" <<EOF
#!/bin/sh
exec "$SDL_STAGE_DIR/usr/bin/sdl-config" --prefix="$SDL_STAGE_DIR/usr" "\$@"
EOF
chmod +x "$SDL_CONFIG_WRAPPER"

# Synthetic freetype-config -- see header comment.
export PKG_CONFIG_SYSROOT_DIR="$THIRDPARTY_STAGE"
export PKG_CONFIG_LIBDIR="$THIRDPARTY_STAGE/usr/lib/pkgconfig:$THIRDPARTY_STAGE/usr/share/pkgconfig"
export PKG_CONFIG_PATH=
FREETYPE_CONFIG_WRAPPER="$SRC_DIR/.freetype-config-wrapper"
cat > "$FREETYPE_CONFIG_WRAPPER" <<EOF
#!/bin/sh
export PKG_CONFIG_SYSROOT_DIR="$PKG_CONFIG_SYSROOT_DIR"
export PKG_CONFIG_LIBDIR="$PKG_CONFIG_LIBDIR"
export PKG_CONFIG_PATH=
case "\$1" in
  --cflags) exec pkg-config freetype2 --cflags ;;
  --libs) exec pkg-config freetype2 --libs ;;
  --prefix) exec pkg-config freetype2 --variable=prefix ;;
  --version) exec pkg-config freetype2 --modversion ;;
  *) echo "freetype-config: unknown arg \$1" >&2; exit 1 ;;
esac
EOF
chmod +x "$FREETYPE_CONFIG_WRAPPER"

export CPPFLAGS="-I$THIRDPARTY_STAGE/usr/include"
export LDFLAGS="-L$THIRDPARTY_STAGE/usr/lib -Wl,-rpath-link=$THIRDPARTY_STAGE/usr/lib"

echo "==> configuring SDL_ttf $SDL_TTF_VERSION"
(
    cd "$SDLTTF_SRC_DIR"
    [ -f Makefile ] && make distclean >/dev/null 2>&1
    ./configure \
        --host=arm-unknown-linux-uclibcgnueabi \
        --build="$(./config.guess 2>/dev/null || echo x86_64-pc-linux-gnu)" \
        --prefix=/usr \
        --disable-static --enable-shared \
        --disable-sdltest \
        CC="$CC" AR="$AR" RANLIB="$RANLIB" STRIP="$STRIP" \
        SDL_CONFIG="$SDL_CONFIG_WRAPPER" \
        FREETYPE_CONFIG="$FREETYPE_CONFIG_WRAPPER"
    echo "==> building SDL_ttf"
    make -j"$JOBS"
    echo "==> installing SDL_ttf to $STAGE_DIR"
    make install DESTDIR="$STAGE_DIR"
)

# Same libtool trap as tools/build-sdl.sh / tools/build-alsa.sh.
LIBSDLTTF_LA="$STAGE_DIR/usr/lib/libSDL_ttf.la"
if [ -f "$LIBSDLTTF_LA" ]; then
    sed -i "s|^libdir='/usr/lib'|libdir='$STAGE_DIR/usr/lib'|" "$LIBSDLTTF_LA"
fi

SDLTTF_SO_REAL="$(cd "$STAGE_DIR/usr/lib" && ls libSDL_ttf-2.0.so.0.* 2>/dev/null | head -1)"
if [ -z "$SDLTTF_SO_REAL" ]; then
    echo "tools/build-sdl-ttf.sh: build finished but no libSDL_ttf-2.0.so.0.* found in $STAGE_DIR/usr/lib" >&2
    exit 1
fi

echo "==> verifying ELF class of $SDLTTF_SO_REAL"
elf_flags="$("$READELF" -h "$STAGE_DIR/usr/lib/$SDLTTF_SO_REAL" | sed -n 's/^ *Flags: *//p')"
case "$elf_flags" in
    0x5000200*) : ;;
    *)
        echo "tools/build-sdl-ttf.sh: unexpected ELF Flags: $elf_flags (want 0x5000200, Version5 EABI, soft-float ABI)" >&2
        exit 1
        ;;
esac
echo "    Flags: $elf_flags"

echo "==> verifying $SDLTTF_SO_REAL links against libfreetype"
needed="$("$READELF" -d "$STAGE_DIR/usr/lib/$SDLTTF_SO_REAL" 2>/dev/null | awk '/NEEDED/{print $NF}' | tr -d '[]')"
echo "    NEEDED: $(echo "$needed" | tr '\n' ' ')"
case "$needed" in
    *libfreetype.so*) : ;;
    *)
        echo "tools/build-sdl-ttf.sh: $SDLTTF_SO_REAL does not NEED libfreetype.so" >&2
        exit 1
        ;;
esac

# --- assemble the device-payload runtime tree --------------------------
mkdir -p "$RUNTIME_DIR/usr/lib"
cp "$STAGE_DIR/usr/lib/$SDLTTF_SO_REAL" "$RUNTIME_DIR/usr/lib/"
chmod u+w "$RUNTIME_DIR/usr/lib/$SDLTTF_SO_REAL"
"$STRIP" --strip-unneeded "$RUNTIME_DIR/usr/lib/$SDLTTF_SO_REAL"
ln -sf "$SDLTTF_SO_REAL" "$RUNTIME_DIR/usr/lib/libSDL_ttf-2.0.so.0"

# freetype is needed at runtime too -- staged for the X11 stack
# (userspace/stage-target), mirror it into the SDL runtime payload.
real="$(cd "$THIRDPARTY_STAGE/usr/lib" && ls libfreetype.so.6.* 2>/dev/null | head -1)"
if [ -n "$real" ] && [ -f "$THIRDPARTY_STAGE/usr/lib/$real" ]; then
    cp "$THIRDPARTY_STAGE/usr/lib/$real" "$RUNTIME_DIR/usr/lib/"
    chmod u+w "$RUNTIME_DIR/usr/lib/$real"
    "$STRIP" --strip-unneeded "$RUNTIME_DIR/usr/lib/$real" 2>/dev/null || true
    ln -sf "$real" "$RUNTIME_DIR/usr/lib/libfreetype.so.6"
fi

echo ""
echo "==> done: $RUNTIME_DIR assembled"
echo "    $RUNTIME_DIR/usr/lib/$SDLTTF_SO_REAL"
echo "    $RUNTIME_DIR/usr/lib/libSDL_ttf-2.0.so.0 -> $SDLTTF_SO_REAL"
echo "    + libfreetype.so.6 (runtime dep mirrored from stage-target)"
