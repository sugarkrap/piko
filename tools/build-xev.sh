#!/bin/sh
set -eu

# Cross-compiles xev (userspace/src/xev) for the Zaurus.
#
# xev is the X event viewer: it opens a window and prints every keyboard
# and pointer event it receives, which is how you read the actual keycodes
# and keysyms this device's matrix keypad produces. That matters here
# because the Zaurus keyboard is not a standard layout -- userspace/xkb/
# symbols/zaurus exists precisely to remap it, and xev is how you check
# what the kernel and the XKB layer really deliver.
#
# Like st, xev is NOT self-contained: it links dynamically against libX11
# from userspace/stage-target, which the X11/Matchbox stack has to have
# populated first (see docs/HOWTO-MATCHBOX-DESKTOP.md -- that stack is
# still a by-hand build on this branch). This script requires that stage
# to already exist and fails loudly rather than silently skipping.
#
# The binary is deliberately left in the build tree rather than installed:
# tools/build-matchbox-payload.sh reads userspace/src/xev/xev directly,
# the same way it reads xkbcomp and Xfbdev out of their build trees.
#
# Usage:
#   tools/build-xev.sh [--force]
#
# --force regenerates configure and rebuilds even if the binary looks
# current.
#
# Env overrides:
#   TOOLCHAIN_BIN_DIR   default <repo>/toolchain/x-tools/$CROSS_HOST/bin
#   CROSS_HOST          default arm-unknown-linux-uclibcgnueabi
#   STAGE               default <repo>/userspace/stage-target
#
# Exit codes:
#   0   userspace/src/xev/xev built (or already current)
#   1   a hard failure (missing toolchain, missing X11 stage, build failure)

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
SRC="$REPO/userspace/src"
XEV_SRC_DIR="$SRC/xev"
STAGE="${STAGE:-$REPO/userspace/stage-target}"

HOST="${CROSS_HOST:-arm-unknown-linux-uclibcgnueabi}"
TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/$HOST/bin}"
BUILD_ARCH="$(uname -m)-pc-linux-gnu"

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

if [ ! -d "$XEV_SRC_DIR" ]; then
    echo "tools/build-xev.sh: $XEV_SRC_DIR missing" >&2
    echo "Run: git submodule update --init userspace/src/xev" >&2
    exit 1
fi

if [ -n "$TOOLCHAIN_BIN_DIR" ] && [ -d "$TOOLCHAIN_BIN_DIR" ]; then
    PATH="$TOOLCHAIN_BIN_DIR:$PATH"
    export PATH
fi
if ! command -v "${HOST}-gcc" >/dev/null 2>&1; then
    echo "tools/build-xev.sh: ${HOST}-gcc not found in PATH." >&2
    echo "Set TOOLCHAIN_BIN_DIR to your toolchain bin path, or export CROSS_HOST." >&2
    exit 1
fi

if [ ! -f "$STAGE/usr/lib/pkgconfig/x11.pc" ]; then
    echo "tools/build-xev.sh: no libX11 staged at $STAGE" >&2
    echo "xev links against libX11 from there -- build the X11/Matchbox" >&2
    echo "stack first (docs/HOWTO-MATCHBOX-DESKTOP.md)." >&2
    exit 1
fi

XEV_BIN="$XEV_SRC_DIR/xev"
if [ "$FORCE" -eq 0 ] && [ -x "$XEV_BIN" ] && [ ! "$XEV_SRC_DIR/xev.c" -nt "$XEV_BIN" ]; then
    echo "==> xev already built and current: $XEV_BIN"
    exit 0
fi

export CC="${HOST}-gcc"
export AR="${HOST}-ar"
export RANLIB="${HOST}-ranlib"
STRIP="${HOST}-strip"
export PKG_CONFIG_SYSROOT_DIR="$STAGE"
# Widened to the host's /usr/share/pkgconfig on purpose: the arch-
# independent X protocol headers (xproto et al, Arch "xorgproto" /
# Debian "x11proto-dev") are not vendored by this submodule and are
# headers only, so there is no host/target contamination risk.
export PKG_CONFIG_LIBDIR="$STAGE/usr/lib/pkgconfig:$STAGE/usr/share/pkgconfig:/usr/share/pkgconfig"
export PKG_CONFIG_PATH=
export CPPFLAGS="-I$STAGE/usr/include"
# -rpath-link (not -rpath): lets the cross-linker resolve indirect
# dependencies at link time without baking a host path into the binary.
export LDFLAGS="-L$STAGE/usr/lib -Wl,-rpath-link=$STAGE/usr/lib"

echo "==> building xev against $STAGE"
[ "$FORCE" -eq 1 ] && rm -f "$XEV_SRC_DIR/configure"

(
    cd "$XEV_SRC_DIR"
    # xorg-macros supplies the XORG_* m4 that xev's configure.ac needs.
    export ACLOCAL_PATH="$SRC/xorg-macros${ACLOCAL_PATH:+:$ACLOCAL_PATH}"

    CONF_ARGS="--host=$HOST --build=$BUILD_ARCH --prefix=/usr --disable-static"

    # The cross args go through autogen.sh, not just ./configure. xev's
    # autogen.sh ALWAYS runs "$srcdir/configure --enable-maintainer-mode
    # "$@"" itself and does not honour NOCONFIGURE at all -- so invoking
    # it bare silently configures a NATIVE build (no --host), which then
    # dies with "cannot run C compiled programs". Found 2026-07-31 doing
    # exactly that on a forced rebuild.
    # shellcheck disable=SC2086
    if [ -f ./configure ]; then
        echo "    configure already generated, running it directly"
        ./configure $CONF_ARGS
    else
        echo "    generating + running configure (autogen.sh)"
        ./autogen.sh $CONF_ARGS
    fi

    make -j"$(nproc 2>/dev/null || echo 4)"
)

"$STRIP" --strip-unneeded "$XEV_BIN" 2>/dev/null || true

if [ ! -x "$XEV_BIN" ]; then
    echo "FAILED: xev built but no binary at $XEV_BIN" >&2
    exit 1
fi
case "$(file -b "$XEV_BIN" 2>/dev/null)" in
    *ARM*) ;;
    *) echo "FAILED: $XEV_BIN is not an ARM binary -- configure fell back to native" >&2
       exit 1 ;;
esac

echo "==> done: $XEV_BIN ($(du -h "$XEV_BIN" | cut -f1))"
