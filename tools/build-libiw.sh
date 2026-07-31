#!/bin/sh
set -eu

# Cross-builds libiw (the wireless-extensions library from wireless-tools)
# into userspace/stage-target, which is what makes matchbox-panel's
# configure enable mb-applet-wireless -- it needs iwlib.h and -liw and
# silently drops the applet from bin_PROGRAMS without them.
#
# Source is the already-vendored userspace/wireless_tools.29. That tree
# also carries ARM binaries (iwconfig, iwlist, ifrename) built in July
# 2026 with a *different* toolchain and statically linked, so its libiw.a
# is deliberately NOT reused here: mixing objects from another libc into a
# uclibc link is a good way to get subtle breakage. We rebuild the one
# source file with the same compiler as the rest of the X/Matchbox stack
# and leave the vendored tree untouched.
#
# Usage:
#   tools/build-libiw.sh [--force]
#
# Idempotent: skips the build when the installed libiw.a is newer than
# iwlib.c. --force rebuilds regardless.

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
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

# Build wireless_tools' OWN wireless.h, not the sysroot's linux/wireless.h.
# iwlib.c references the IW_MODUL_* modulation constants, which are
# wireless-tools additions the kernel's uapi header has never carried --
# against the sysroot copy the build dies on ~15 undeclared identifiers.
# The vendored header is WE21 and the kernel is WE22, which is fine: the
# difference is in-kernel compat handling, the 32-bit ioctl structs are
# unchanged, iwlib re-checks we_version_compiled at runtime, and the
# iwconfig already running on the device was built from this same header
# against this same kernel.
cp "$WT/iwlib.c" "$WT/iwlib.h" "$WT/wireless.h" "$BUILD/"

# -DWE_NOLIBM swaps iw_float2freq()/iw_freq2float()'s floor/log10/pow for
# integer loops. Without it libiw drags in libm, which would add a
# libm.so.0 DT_NEEDED that the device does not have and the Matchbox
# payload does not ship -- for two frequency-conversion helpers.
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
# iwlib.h does #include "wireless.h" (quoted), so the header has to sit
# next to it or every consumer fails to preprocess.
install -m 644 "$WT/wireless.h" "$STAGE/usr/include/wireless.h"

echo "==> libiw ready:"
echo "    $STAGE/usr/lib/libiw.a"
echo "    $STAGE/usr/include/iwlib.h + wireless.h"
echo "Now reconfigure matchbox-panel; its summary must say"
echo "    Building mb-applet-wireless:        yes"
