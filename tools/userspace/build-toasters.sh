#!/bin/sh
set -eu

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
SRC="$REPO/userspace/src/toasters.c"
BIN="$REPO/build/target/bin/toasters"
STAGE="${STAGE:-$REPO/build/target}"

TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin}"
CROSS_COMPILE="${CROSS_COMPILE:-arm-unknown-linux-uclibcgnueabi-}"

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

if [ ! -f "$SRC" ]; then
    echo "tools/userspace/build-toasters.sh: $SRC missing" >&2
    exit 1
fi

if [ -n "$TOOLCHAIN_BIN_DIR" ] && [ -d "$TOOLCHAIN_BIN_DIR" ]; then
    PATH="$TOOLCHAIN_BIN_DIR:$PATH"
fi
if ! command -v "${CROSS_COMPILE}gcc" >/dev/null 2>&1; then
    echo "tools/userspace/build-toasters.sh: ${CROSS_COMPILE}gcc not found in PATH." >&2
    echo "Set TOOLCHAIN_BIN_DIR to your toolchain bin path, or export CROSS_COMPILE explicitly." >&2
    exit 1
fi
CC="${CROSS_COMPILE}gcc"
STRIP="${CROSS_COMPILE}strip"

if [ ! -f "$STAGE/usr/include/X11/Xlib.h" ] || [ ! -f "$STAGE/usr/lib/libX11.so" ]; then
    echo "tools/userspace/build-toasters.sh: no X11 stack staged at $STAGE" >&2
    echo "toasters links against libX11 from there -- build the X11/Matchbox" >&2
    echo "stack first." >&2
    exit 1
fi
if [ ! -f "$STAGE/usr/include/X11/xpm.h" ] || [ ! -f "$STAGE/usr/lib/libXpm.so" ]; then
    echo "tools/userspace/build-toasters.sh: libXpm not staged at $STAGE" >&2
    echo "The sprite sheets are XPM data decoded with XpmCreateImageFromData()." >&2
    echo "tools/userspace/build-x11-stack.sh builds libXpm as part of the stack." >&2
    exit 1
fi

SPRITES="$REPO/userspace/src/flying-toasters/img/toaster.xpm"
if [ ! -f "$SPRITES" ]; then
    echo "tools/userspace/build-toasters.sh: $SPRITES missing" >&2
    echo "The sprite artwork is the flying-toasters submodule (MIT, see its" >&2
    echo "LICENSE). Run: git submodule update --init userspace/src/flying-toasters" >&2
    exit 1
fi

if [ "$FORCE" -eq 0 ] && [ -x "$BIN" ] && [ ! "$SRC" -nt "$BIN" ] \
   && [ ! "$SPRITES" -nt "$BIN" ]; then
    echo "==> toasters already built and current: $BIN"
    exit 0
fi

echo "==> building toasters against $STAGE"
mkdir -p "$(dirname "$BIN")"
"$CC" -march=armv5te -O2 -Wall -Wextra \
    -I"$STAGE/usr/include" \
    -o "$BIN" "$SRC" \
    -L"$STAGE/usr/lib" -Wl,-rpath-link="$STAGE/usr/lib" -lXpm -lX11
"$STRIP" --strip-unneeded "$BIN" 2>/dev/null || true

echo "==> done: $BIN ($(du -h "$BIN" | cut -f1))"
