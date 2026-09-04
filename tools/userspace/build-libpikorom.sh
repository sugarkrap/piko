#!/bin/sh
set -eu

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
SRC="$REPO/userspace/src/libpikorom"
OUT_LIB="${OUT_LIB:-$REPO/build/target/usr/lib}"
STAGE="${STAGE:-$REPO/build/target}"
TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin}"
HOST="${HOST:-arm-unknown-linux-uclibcgnueabi}"
CXX="$TOOLCHAIN_BIN_DIR/$HOST-g++"
SONAME=libpikorom.so.1

[ -x "$CXX" ] || { echo "build-libpikorom.sh: no compiler at $CXX" >&2; exit 1; }
[ -f "$SRC/pikorom.cxx" ] || { echo "build-libpikorom.sh: no $SRC/pikorom.cxx" >&2; exit 1; }
[ -f "$SRC/emulation_db.h" ] || {
    echo "build-libpikorom.sh: no $SRC/emulation_db.h" >&2
    exit 1
}

if [ "${PIKOROM_SKIP_TESTS:-0}" = "0" ]; then
    echo "==> running host test (pikorom C API)"
    HOSTCXX="${HOSTCXX:-g++}"
    if command -v "$HOSTCXX" >/dev/null 2>&1; then
        t="$(mktemp -d)"
        "$HOSTCXX" -O2 -Wall -Wextra -I"$SRC" \
            -o "$t/pikorom-test" "$SRC/tests/pikorom-test.cxx" "$SRC/pikorom.cxx" -lz
        "$t/pikorom-test"
        rm -rf "$t"
    else
        echo "    no host $HOSTCXX -- skipping (build continues)"
    fi
fi

mkdir -p "$OUT_LIB"

echo "==> building $SONAME"
"$CXX" -O2 -Wall -Wextra -fPIC -shared \
    -I"$SRC" -isystem "$STAGE/usr/include" \
    -o "$OUT_LIB/$SONAME" \
    "$SRC/pikorom.cxx" \
    -L"$STAGE/usr/lib" -Wl,-rpath-link="$STAGE/usr/lib" -lz \
    -Wl,-soname,"$SONAME"
ln -sf "$SONAME" "$OUT_LIB/libpikorom.so"

"$TOOLCHAIN_BIN_DIR/$HOST-strip" --strip-unneeded "$OUT_LIB/$SONAME"

for sym in pikorom_install pikorom_db_open pikorom_sync_launchers pikobezel_list; do
    if ! "$TOOLCHAIN_BIN_DIR/$HOST-readelf" -sW --dyn-syms "$OUT_LIB/$SONAME" \
        | grep -q " $sym\$"; then
        echo "build-libpikorom.sh: $SONAME does not export $sym" >&2
        exit 1
    fi
done

echo "    $OUT_LIB/$SONAME"
