#!/bin/sh
set -eu

# Cross-builds userspace/src/xsha1-compat (a small public-domain SHA1
# implementation, see that directory's sha1.h) into a shared libmd.so.1,
# and stages it into userspace/stage-target.
#
# xorg-server's os/xsha1.c has a --with-sha1=libmd path that expects
# SHA1Init/SHA1Update/SHA1Final from a library named "md" (-lmd). Nothing
# in this ARM/uclibc cross toolchain's sysroot provides that -- there is
# no libc SHA1, no libgcrypt, no OpenSSL, no libsha1 in this minimal
# stack -- so configure fails outright with "No suitable SHA1
# implementation found" unless something supplies -lmd first. This is
# that something.
#
# Named libmd.so.1 (not .so.0) so it never collides with a real libbsd's
# libmd (SONAME 0) if that is ever cross-built into the same staging
# tree too -- see tools/build-matchbox-payload.sh's LIBS comment on the
# two coexisting.
#
# Usage:
#   tools/build-xsha1-compat.sh [--force]
#
# Idempotent: skips the build when the staged .so is newer than sha1.c.
# --force rebuilds regardless. Must run before tools/build-x11-stack.sh
# configures xserver (it calls this itself; direct use is for iterating
# on the shim alone).

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
SRC="$REPO/userspace/src/xsha1-compat"
STAGE="${STAGE:-$REPO/userspace/stage-target}"
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
# xserver's os/xsha1.c does `#include <sha1.h>` (angle brackets, the
# HAVE_SHA1_IN_LIBMD branch expects it on the include path same as the
# library is on the link path) -- it is not enough to only ship the .so.
install -m 644 "$SRC/sha1.h" "$STAGE/usr/include/sha1.h"

echo "==> xsha1-compat ready: $SO"
echo "    (+ $STAGE/usr/lib/libmd.so symlink, for -lmd at link time)"
echo "    (+ $STAGE/usr/include/sha1.h, for os/xsha1.c's #include <sha1.h>)"
