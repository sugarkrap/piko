#!/bin/sh
set -eu

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$REPO/userspace/src/PocketSNES"
STAGE="${STAGE:-$REPO/userspace/stage-target}"
SDL_STAGE="${SDL_STAGE:-$REPO/userspace/stage-sdl}"

TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin}"
HOST="${HOST:-arm-unknown-linux-uclibcgnueabi}"
JOBS="${JOBS:-$(command -v nproc >/dev/null 2>&1 && nproc || echo 4)}"

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

if [ ! -f "$SRC/Makefile.zaurus" ]; then
    echo "tools/userspace/build-pocketsnes.sh: $SRC is empty" >&2
    echo "  it is a git submodule -- run:" >&2
    echo "    git submodule update --init userspace/src/PocketSNES" >&2
    exit 1
fi

if ! command -v "$TOOLCHAIN_BIN_DIR/$HOST-g++" >/dev/null 2>&1; then
    echo "tools/userspace/build-pocketsnes.sh: no C++ cross compiler at $TOOLCHAIN_BIN_DIR/$HOST-g++" >&2
    exit 1
fi

if [ ! -f "$SDL_STAGE/usr/include/SDL/SDL.h" ]; then
    echo "tools/userspace/build-pocketsnes.sh: no staged SDL headers at $SDL_STAGE" >&2
    echo "  run tools/userspace/build-sdl.sh first" >&2
    exit 1
fi

OUT="$STAGE/usr/bin/PocketSNES"
if [ "$FORCE" -eq 0 ] && [ -f "$OUT" ] && [ -f "$SRC/PocketSNES" ] \
        && [ ! "$SRC/Makefile.zaurus" -nt "$OUT" ] && [ ! "$0" -nt "$OUT" ]; then
    echo "==> $OUT already up to date (pass --force to rebuild)"
    exit 0
fi

echo "==> cross-building PocketSNES against $SDL_STAGE and $STAGE"
PATH="$TOOLCHAIN_BIN_DIR:$PATH"
export PATH
make -C "$SRC" -f Makefile.zaurus PIKO_DIR="$REPO" -j"$JOBS"

if [ ! -f "$SRC/PocketSNES" ]; then
    echo "tools/userspace/build-pocketsnes.sh: make finished but there is no $SRC/PocketSNES" >&2
    exit 1
fi

mkdir -p "$STAGE/usr/bin"
cp "$SRC/PocketSNES" "$OUT"
"$TOOLCHAIN_BIN_DIR/$HOST-strip" "$OUT" 2>/dev/null || true

needed="$("$TOOLCHAIN_BIN_DIR/$HOST-readelf" -d "$OUT" \
    | grep -oE '\[lib[^]]+\]' | tr -d '[]' | tr '\n' ' ')"
echo "    NEEDED: $needed"
case " $needed " in
    *" libSDL-1.2.so.0 "*) : ;;
    *)
        echo "tools/userspace/build-pocketsnes.sh: PocketSNES does not NEED libSDL-1.2.so.0" >&2
        echo "-- it linked statically or against the wrong library." >&2
        exit 1
        ;;
esac

echo ""
echo "==> done: $OUT ($(du -h "$OUT" | cut -f1))"
echo ""
