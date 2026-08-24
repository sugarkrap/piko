#!/bin/sh
set -eu

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
SRC="$REPO/userspace/src/xsha1-compat"
STAGE="${STAGE:-$REPO/build/target}"
HOST_TRIPLET="${CROSS_HOST:-arm-unknown-linux-uclibcgnueabi}"
TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/$HOST_TRIPLET/bin}"

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

CC="$TOOLCHAIN_BIN_DIR/$HOST_TRIPLET-gcc"
STRIP="$TOOLCHAIN_BIN_DIR/$HOST_TRIPLET-strip"

[ -x "$CC" ] || { echo "FAILED: no compiler at $CC" >&2; exit 1; }
for f in sha1.c sha1.h; do
    [ -f "$SRC/$f" ] || { echo "FAILED: missing $SRC/$f" >&2; exit 1; }
done

SO="$STAGE/usr/lib/libmd.so.1"
if [ "$FORCE" -eq 0 ] && [ -f "$SO" ] && [ -f "$STAGE/usr/include/sha1.h" ] \
   && [ "$SO" -nt "$SRC/sha1.c" ]; then
    echo "==> xsha1-compat already built (pass --force to rebuild)"
    exit 0
fi

mkdir -p "$STAGE/usr/lib" "$STAGE/usr/include"
echo "==> compiling xsha1-compat for $HOST_TRIPLET"
"$CC" -O2 -fPIC -shared -Wl,-soname,libmd.so.1 \
    -o "$SO" "$SRC/sha1.c"
"$STRIP" --strip-unneeded "$SO"
ln -sf libmd.so.1 "$STAGE/usr/lib/libmd.so"
install -m 644 "$SRC/sha1.h" "$STAGE/usr/include/sha1.h"

echo "==> xsha1-compat ready: $SO"
echo "    (+ $STAGE/usr/lib/libmd.so symlink, for -lmd at link time)"
echo "    (+ $STAGE/usr/include/sha1.h, for os/xsha1.c's #include <sha1.h>)"
