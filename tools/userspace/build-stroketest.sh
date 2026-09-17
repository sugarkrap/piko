#!/bin/sh
set -eu

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
SRC="$REPO/userspace/src/stroketest.c"
BIN="$REPO/build/target/bin/stroketest"
STAGE="${STAGE:-$REPO/build/target}"

TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin}"
CROSS_COMPILE="${CROSS_COMPILE:-arm-unknown-linux-uclibcgnueabi-}"

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

if [ ! -f "$SRC" ]; then
    echo "tools/userspace/build-stroketest.sh: $SRC missing" >&2
    exit 1
fi

if [ -n "$TOOLCHAIN_BIN_DIR" ] && [ -d "$TOOLCHAIN_BIN_DIR" ]; then
    PATH="$TOOLCHAIN_BIN_DIR:$PATH"
fi
if ! command -v "${CROSS_COMPILE}gcc" >/dev/null 2>&1; then
    echo "tools/userspace/build-stroketest.sh: ${CROSS_COMPILE}gcc not found in PATH." >&2
    echo "Set TOOLCHAIN_BIN_DIR to your toolchain bin path, or export CROSS_COMPILE explicitly." >&2
    exit 1
fi
CC="${CROSS_COMPILE}gcc"
STRIP="${CROSS_COMPILE}strip"

if [ ! -f "$STAGE/usr/include/X11/Xlib.h" ] || [ ! -f "$STAGE/usr/lib/libX11.so" ]; then
    echo "tools/userspace/build-stroketest.sh: no X11 stack staged at $STAGE" >&2
    echo "Build the X11/Matchbox stack first." >&2
    exit 1
fi
if [ ! -f "$STAGE/usr/include/X11/Xft/Xft.h" ] || [ ! -f "$STAGE/usr/lib/libXft.so" ]; then
    echo "tools/userspace/build-stroketest.sh: libXft not staged at $STAGE" >&2
    echo "tools/userspace/build-x11-stack.sh builds libXft as part of the stack." >&2
    exit 1
fi
if [ ! -d "$STAGE/usr/include/freetype2" ]; then
    echo "tools/userspace/build-stroketest.sh: no freetype2 headers at $STAGE" >&2
    echo "Run tools/userspace/build-thirdparty-deps.sh first." >&2
    exit 1
fi

if [ "$FORCE" -eq 0 ] && [ -x "$BIN" ] && [ ! "$SRC" -nt "$BIN" ]; then
    echo "==> stroketest already built and current: $BIN"
    exit 0
fi

echo "==> building stroketest against $STAGE"
mkdir -p "$(dirname "$BIN")"
"$CC" -march=armv5te -O2 -Wall -Wextra \
    -I"$STAGE/usr/include" -I"$STAGE/usr/include/freetype2" \
    -o "$BIN" "$SRC" \
    -L"$STAGE/usr/lib" -Wl,-rpath-link="$STAGE/usr/lib" -lXft -lX11
"$STRIP" --strip-unneeded "$BIN" 2>/dev/null || true

echo "==> done: $BIN ($(du -h "$BIN" | cut -f1))"
