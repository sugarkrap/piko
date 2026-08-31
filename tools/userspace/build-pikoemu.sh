#!/bin/sh
set -eu

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
SRC="$REPO/userspace/src/pikoemu"
LIBSRC="$REPO/userspace/src/libpikovideo"
ROMSRC="$REPO/userspace/src/libpikorom"
TARGET_STAGE="${TARGET_STAGE:-$REPO/build/target}"
SDL_STAGE="${SDL_STAGE:-$REPO/build/stage-sdl}"
OUT_BIN="${OUT_BIN:-$REPO/build/target/bin/pikoemu}"
OUT_LIB="${OUT_LIB:-$REPO/build/target/usr/lib}"
TARGET_LIB="${TARGET_LIB:-$REPO/build/target/usr/lib}"
TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin}"
HOST="${HOST:-arm-unknown-linux-uclibcgnueabi}"
CC="$TOOLCHAIN_BIN_DIR/$HOST-gcc"

[ -x "$CC" ] || { echo "build-pikoemu.sh: no compiler at $CC" >&2; exit 1; }
[ -f "$TARGET_STAGE/usr/lib/libpikorom.so.1" ] || {
    echo "build-pikoemu.sh: no libpikorom.so.1 at $TARGET_STAGE/usr/lib" >&2
    echo "  run tools/userspace/build-libpikorom.sh first" >&2
    exit 1
}
[ -f "$SDL_STAGE/usr/include/SDL/SDL.h" ] || {
    echo "build-pikoemu.sh: no staged SDL at $SDL_STAGE -- run tools/userspace/build-sdl.sh" >&2
    exit 1
}

if [ "${PIKOEMU_SKIP_TESTS:-0}" = "0" ]; then
    echo "==> running host test (pikoemu config resolution)"
    HOSTCC="${HOSTCC:-gcc}"
    if command -v "$HOSTCC" >/dev/null 2>&1; then
        t="$(mktemp -d)"
        "$HOSTCC" -O2 -Wall -Wextra -I"$SRC" -I"$ROMSRC" -I"$REPO/userspace/src/piko-sync" \
            -o "$t/pikoemu-config-test" "$SRC/tests/config-test.c" "$ROMSRC/pikorom.cxx" \
            -lstdc++ -lz
        "$t/pikoemu-config-test"
        rm -rf "$t"
    else
        echo "    no host $HOSTCC -- skipping (build continues)"
    fi
fi

mkdir -p "$(dirname "$OUT_BIN")" "$OUT_LIB"

echo "==> building libpikovideo"
"$CC" -O2 -Wall -Wextra -fPIC -shared \
    -I"$LIBSRC" \
    -o "$OUT_LIB/libpikovideo.so.1" \
    "$LIBSRC/pikovideo.c" \
    -Wl,-soname,libpikovideo.so.1
"$TOOLCHAIN_BIN_DIR/$HOST-strip" --strip-unneeded "$OUT_LIB/libpikovideo.so.1"
ln -sf libpikovideo.so.1 "$OUT_LIB/libpikovideo.so"

echo "==> building pikoemu"
"$CC" -O2 -Wall -Wextra -march=armv5te \
    -I"$SRC" -I"$LIBSRC" -I"$ROMSRC" -I"$SDL_STAGE/usr/include/SDL" \
    -o "$OUT_BIN" "$SRC/pikoemu.c" "$SRC/pikoemu_ui.c" \
    -L"$OUT_LIB" -lpikovideo -lpikorom \
    -L"$SDL_STAGE/usr/lib" -Wl,-rpath-link="$SDL_STAGE/usr/lib" \
    -Wl,-rpath-link="$TARGET_LIB" -lSDL -lpthread

"$TOOLCHAIN_BIN_DIR/$HOST-strip" "$OUT_BIN" 2>/dev/null || true

echo "==> done"
echo "    $OUT_BIN"
echo "    $OUT_LIB/libpikovideo.so.1"
