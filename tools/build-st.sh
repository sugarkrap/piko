#!/bin/sh
set -eu

# Cross-compiles st (suckless terminal, userspace/src/st) for the Zaurus.
#
# Unlike ALSA/MPlayer, st is NOT self-contained: it links dynamically
# against libX11/libXft/libfontconfig/libfreetype, which live in
# userspace/stage-target once the X11/Matchbox stack has been built by hand
# (see docs/HOWTO-MATCHBOX-DESKTOP.md -- there is no scripted build for that
# stack yet). This script requires that stage to already exist and fails
# loudly rather than silently skipping if it doesn't.
#
# st's own config.mk hardcodes X11INC/X11LIB to /usr/X11R6, which does not
# exist on this build machine; override both to the staged tree instead.
# fontconfig/freetype2 are already resolved via pkg-config against $STAGE
# (PKG_CONFIG_SYSROOT_DIR/PKG_CONFIG_LIBDIR below), same as every other
# component in docs/HOWTO-MATCHBOX-DESKTOP.md.
#
# Usage:
#   tools/build-st.sh [--force]
#
# --force does a `make clean` first even if the binary looks current.
#
# Env overrides:
#   TOOLCHAIN_BIN_DIR   default <repo>/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin
#   CROSS_COMPILE       default arm-unknown-linux-uclibcgnueabi-
#   STAGE               default <repo>/userspace/stage-target
#
# Exit codes:
#   0   userspace/src/st/st built (or already current)
#   1   a hard failure (missing toolchain, missing X11 stage, build failure)

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
ST_SRC_DIR="$REPO/userspace/src/st"
STAGE="${STAGE:-$REPO/userspace/stage-target}"

TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin}"
CROSS_COMPILE="${CROSS_COMPILE:-arm-unknown-linux-uclibcgnueabi-}"

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

if [ ! -d "$ST_SRC_DIR" ]; then
    echo "tools/build-st.sh: $ST_SRC_DIR missing" >&2
    echo "Run: git submodule update --init userspace/src/st" >&2
    exit 1
fi

if [ -n "$TOOLCHAIN_BIN_DIR" ] && [ -d "$TOOLCHAIN_BIN_DIR" ]; then
    PATH="$TOOLCHAIN_BIN_DIR:$PATH"
fi
if ! command -v "${CROSS_COMPILE}gcc" >/dev/null 2>&1; then
    echo "tools/build-st.sh: ${CROSS_COMPILE}gcc not found in PATH." >&2
    echo "Set TOOLCHAIN_BIN_DIR to your toolchain bin path, or export CROSS_COMPILE explicitly." >&2
    exit 1
fi
CC="${CROSS_COMPILE}gcc"
STRIP="${CROSS_COMPILE}strip"

if [ ! -f "$STAGE/usr/include/X11/Xlib.h" ] || [ ! -f "$STAGE/usr/lib/libXft.so" ]; then
    echo "tools/build-st.sh: no X11 stack staged at $STAGE" >&2
    echo "st links against libX11/libXft/fontconfig/freetype from there --" >&2
    echo "build the X11/Matchbox stack first (docs/HOWTO-MATCHBOX-DESKTOP.md)." >&2
    exit 1
fi

ST_BIN="$ST_SRC_DIR/st"
if [ "$FORCE" -eq 0 ] && [ -x "$ST_BIN" ] \
   && [ ! "$ST_SRC_DIR/st.c" -nt "$ST_BIN" ] && [ ! "$ST_SRC_DIR/x.c" -nt "$ST_BIN" ]; then
    echo "==> st already built and current: $ST_BIN"
    exit 0
fi

echo "==> building st against $STAGE"
export PKG_CONFIG_SYSROOT_DIR="$STAGE"
export PKG_CONFIG_LIBDIR="$STAGE/usr/lib/pkgconfig:$STAGE/usr/share/pkgconfig"
export PKG_CONFIG_PATH=
export CPPFLAGS="-I$STAGE/usr/include"
export LDFLAGS="-L$STAGE/usr/lib -Wl,-rpath-link=$STAGE/usr/lib"
(
    cd "$ST_SRC_DIR"
    [ "$FORCE" -eq 1 ] && make clean >/dev/null 2>&1
    make CC="$CC" X11INC="$STAGE/usr/include" X11LIB="$STAGE/usr/lib"
)
"$STRIP" --strip-unneeded "$ST_BIN" 2>/dev/null || true

echo "==> done: $ST_BIN ($(du -h "$ST_BIN" | cut -f1))"
