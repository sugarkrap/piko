#!/bin/sh
set -eu

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
WT="$REPO/userspace/wireless_tools.29"
STAGE="${STAGE:-$REPO/userspace/stage-target}"
HOST_TRIPLET="${CROSS_HOST:-arm-unknown-linux-uclibcgnueabi}"
TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/$HOST_TRIPLET/bin}"

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

CC="$TOOLCHAIN_BIN_DIR/$HOST_TRIPLET-gcc"
AR="$TOOLCHAIN_BIN_DIR/$HOST_TRIPLET-ar"
RANLIB="$TOOLCHAIN_BIN_DIR/$HOST_TRIPLET-ranlib"
NM="$TOOLCHAIN_BIN_DIR/$HOST_TRIPLET-nm"

[ -x "$CC" ] || { echo "FAILED: no compiler at $CC" >&2; exit 1; }
for f in iwlib.c iwlib.h wireless.h; do
    [ -f "$WT/$f" ] || { echo "FAILED: missing $WT/$f" >&2; exit 1; }
done

if [ "$FORCE" -eq 0 ] && [ -f "$STAGE/usr/lib/libiw.a" ] \
   && [ "$STAGE/usr/lib/libiw.a" -nt "$WT/iwlib.c" ]; then
    echo "==> libiw already built (pass --force to rebuild)"
    exit 0
fi

BUILD="$(mktemp -d)"
trap 'rm -rf "$BUILD"' EXIT

cp "$WT/iwlib.c" "$WT/iwlib.h" "$WT/wireless.h" "$BUILD/"

echo "==> compiling libiw for $HOST_TRIPLET (WE_NOLIBM, no libm)"
( cd "$BUILD" && "$CC" -Os -W -Wall -fPIC -DWE_NOLIBM=y -I. -c iwlib.c -o iwlib.o )
"$AR" rcs "$BUILD/libiw.a" "$BUILD/iwlib.o"
"$RANLIB" "$BUILD/libiw.a"

if "$NM" -u "$BUILD/iwlib.o" | grep -qE " (log10|pow|floor)$"; then
    echo "FAILED: libiw still references libm -- WE_NOLIBM did not take" >&2
    exit 1
fi

echo "==> installing into $STAGE"
mkdir -p "$STAGE/usr/lib" "$STAGE/usr/include"
install -m 644 "$BUILD/libiw.a" "$STAGE/usr/lib/libiw.a"
install -m 644 "$WT/iwlib.h"    "$STAGE/usr/include/iwlib.h"
install -m 644 "$WT/wireless.h" "$STAGE/usr/include/wireless.h"

echo "==> libiw ready:"
echo "    $STAGE/usr/lib/libiw.a"
echo "    $STAGE/usr/include/iwlib.h + wireless.h"
echo "Now reconfigure matchbox-panel; its summary must say"
echo "    Building mb-applet-wireless:        yes"
