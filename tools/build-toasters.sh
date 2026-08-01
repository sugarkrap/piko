#!/bin/sh
set -eu

# Cross-compiles userspace/src/toasters.c (the flying-toasters screensaver)
# for the Zaurus.
#
# Same situation as tools/build-st.sh: this is NOT self-contained, it links
# dynamically against libX11, which lives in userspace/stage-target once
# tools/build-x11-stack.sh has populated it. This script requires that stage
# to already exist and fails loudly rather than silently skipping if it
# doesn't.
#
# You do not normally invoke it yourself: build-x11-stack.sh runs it at the
# end of a full build, for the same reason it runs build-st.sh and
# build-fltk.sh -- tools/build-matchbox-payload.sh needs the binary, so
# producing it is that one script's job rather than every caller's.
# See docs/HOWTO-MATCHBOX-DESKTOP.md.
#
# Unlike st, this is a single tracked source file with no upstream project
# to vendor and no Makefile of its own, so it is just one gcc invocation --
# same shape as the md5sum/brightd/kill steps in tools/build-userspace.sh,
# except linked against the staged X11 headers/libs instead of built
# static-libc-only.
#
# Usage:
#   tools/build-toasters.sh [--force]
#
# --force rebuilds even if the binary looks current.
#
# Env overrides:
#   TOOLCHAIN_BIN_DIR   default <repo>/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin
#   CROSS_COMPILE       default arm-unknown-linux-uclibcgnueabi-
#   STAGE               default <repo>/userspace/stage-target
#
# Exit codes:
#   0   userspace/src/toasters built (or already current)
#   1   a hard failure (missing toolchain, missing X11 stage, build failure)

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
SRC="$REPO/userspace/src/toasters.c"
BIN="$REPO/userspace/src/toasters"
STAGE="${STAGE:-$REPO/userspace/stage-target}"

TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin}"
CROSS_COMPILE="${CROSS_COMPILE:-arm-unknown-linux-uclibcgnueabi-}"

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

if [ ! -f "$SRC" ]; then
    echo "tools/build-toasters.sh: $SRC missing" >&2
    exit 1
fi

if [ -n "$TOOLCHAIN_BIN_DIR" ] && [ -d "$TOOLCHAIN_BIN_DIR" ]; then
    PATH="$TOOLCHAIN_BIN_DIR:$PATH"
fi
if ! command -v "${CROSS_COMPILE}gcc" >/dev/null 2>&1; then
    echo "tools/build-toasters.sh: ${CROSS_COMPILE}gcc not found in PATH." >&2
    echo "Set TOOLCHAIN_BIN_DIR to your toolchain bin path, or export CROSS_COMPILE explicitly." >&2
    exit 1
fi
CC="${CROSS_COMPILE}gcc"
STRIP="${CROSS_COMPILE}strip"

if [ ! -f "$STAGE/usr/include/X11/Xlib.h" ] || [ ! -f "$STAGE/usr/lib/libX11.so" ]; then
    echo "tools/build-toasters.sh: no X11 stack staged at $STAGE" >&2
    echo "toasters links against libX11 from there -- build the X11/Matchbox" >&2
    echo "stack first (docs/HOWTO-MATCHBOX-DESKTOP.md)." >&2
    exit 1
fi
if [ ! -f "$STAGE/usr/include/X11/xpm.h" ] || [ ! -f "$STAGE/usr/lib/libXpm.so" ]; then
    echo "tools/build-toasters.sh: libXpm not staged at $STAGE" >&2
    echo "The sprite sheets are XPM data decoded with XpmCreateImageFromData()." >&2
    echo "tools/build-x11-stack.sh builds libXpm as part of the stack." >&2
    exit 1
fi

# The sprite sheets are the vendored submodule's, included by relative path
# from toasters.c. An uninitialised submodule is an empty directory, and the
# failure that produces is a #include error naming a path nobody recognises.
SPRITES="$REPO/userspace/src/flying-toasters/img/toaster.xpm"
if [ ! -f "$SPRITES" ]; then
    echo "tools/build-toasters.sh: $SPRITES missing" >&2
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
# -lXpm for XpmCreateImageFromData(), which turns the vendored XPM sprite
# sheets into XImages plus their shape masks. The sheets themselves need no
# -I: toasters.c includes them by relative path out of the submodule.
"$CC" -march=armv5te -O2 -Wall -Wextra \
    -I"$STAGE/usr/include" \
    -o "$BIN" "$SRC" \
    -L"$STAGE/usr/lib" -Wl,-rpath-link="$STAGE/usr/lib" -lXpm -lX11
"$STRIP" --strip-unneeded "$BIN" 2>/dev/null || true

echo "==> done: $BIN ($(du -h "$BIN" | cut -f1))"
