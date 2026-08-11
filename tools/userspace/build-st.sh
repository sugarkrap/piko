#!/bin/sh
set -eu

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
ST_SRC_DIR="$REPO/userspace/src/st"
STAGE="${STAGE:-$REPO/userspace/stage-target}"

TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin}"
CROSS_COMPILE="${CROSS_COMPILE:-arm-unknown-linux-uclibcgnueabi-}"

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

if [ ! -d "$ST_SRC_DIR" ]; then
    echo "tools/userspace/build-st.sh: $ST_SRC_DIR missing" >&2
    echo "Run: git submodule update --init userspace/src/st" >&2
    exit 1
fi

if [ -n "$TOOLCHAIN_BIN_DIR" ] && [ -d "$TOOLCHAIN_BIN_DIR" ]; then
    PATH="$TOOLCHAIN_BIN_DIR:$PATH"
fi
if ! command -v "${CROSS_COMPILE}gcc" >/dev/null 2>&1; then
    echo "tools/userspace/build-st.sh: ${CROSS_COMPILE}gcc not found in PATH." >&2
    echo "Set TOOLCHAIN_BIN_DIR to your toolchain bin path, or export CROSS_COMPILE explicitly." >&2
    exit 1
fi
CC="${CROSS_COMPILE}gcc"
STRIP="${CROSS_COMPILE}strip"

if [ ! -f "$STAGE/usr/include/X11/Xlib.h" ] || [ ! -f "$STAGE/usr/lib/libXft.so" ]; then
    echo "tools/userspace/build-st.sh: no X11 stack staged at $STAGE" >&2
    echo "st links against libX11/libXft/fontconfig/freetype from there --" >&2
    echo "build the X11/Matchbox stack first." >&2
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
