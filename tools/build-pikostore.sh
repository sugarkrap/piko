#!/bin/sh
set -eu

# Cross-builds pikostore ("Software Center", userspace/src/pikostore -- a
# tracked git submodule) against the FLTK/X11 tree staged in
# userspace/stage-target, and drops the binary there for
# tools/build-matchbox-payload.sh to ship to /usr/local/bin.
#
# WHY IT IS SEPARATE FROM tools/build-fltk.sh: that script builds the
# toolkit plus fltktest, which is a smoke test for the toolkit. pikostore
# is an application that happens to use the toolkit -- it has its own repo,
# its own release cadence and (unlike fltktest) its own host-side tests. It
# depends on build-fltk.sh having run, and says so loudly rather than
# silently skipping, same policy as tools/build-st.sh.
#
# WHY THE HOST TESTS RUN HERE: userspace/src/pikostore/tests/ covers the
# manifest/history parsing and the piko-update progress protocol, and needs
# no FLTK, no X and no device. It is the only part of this app that can be
# exercised before it reaches the one spare Zaurus, so it runs on every
# build rather than being something to remember. Set PIKOSTORE_SKIP_TESTS=1
# to skip it if you are iterating on the GUI and know what you are doing.
#
# Exit status:
#   0   pikostore is staged in $STAGE/usr/bin
#   1   missing toolchain, missing FLTK stage, test failure, or build error

REPO="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$REPO/userspace/src/pikostore"
STAGE="${STAGE:-$REPO/userspace/stage-target}"

TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin}"
HOST="${HOST:-arm-unknown-linux-uclibcgnueabi}"
FL_DSO_VERSION="${FL_DSO_VERSION:-1.3}"

if [ ! -f "$SRC/pikostore.cxx" ]; then
    echo "tools/build-pikostore.sh: $SRC is empty" >&2
    echo "  it is a git submodule -- run:" >&2
    echo "    git submodule update --init userspace/src/pikostore" >&2
    exit 1
fi

if ! command -v "$TOOLCHAIN_BIN_DIR/$HOST-g++" >/dev/null 2>&1; then
    echo "tools/build-pikostore.sh: no C++ cross compiler at $TOOLCHAIN_BIN_DIR/$HOST-g++" >&2
    exit 1
fi

if [ ! -f "$STAGE/usr/lib/libfltk.so.$FL_DSO_VERSION" ]; then
    echo "tools/build-pikostore.sh: no staged libfltk at $STAGE" >&2
    echo "  run tools/build-fltk.sh first (see docs/HOWTO-FLTK.md)" >&2
    exit 1
fi

# --- host-side tests ----------------------------------------------------
if [ "${PIKOSTORE_SKIP_TESTS:-0}" = "0" ]; then
    echo "==> running host tests (romstate: manifest, history, progress protocol)"
    HOSTCXX="${HOSTCXX:-g++}"
    if command -v "$HOSTCXX" >/dev/null 2>&1; then
        testbin="$(mktemp -d)/romstate-test"
        "$HOSTCXX" -O2 -Wall -Wextra -o "$testbin" "$SRC/tests/romstate-test.cxx"
        "$testbin"
        rm -rf "$(dirname "$testbin")"
    else
        echo "    no host $HOSTCXX -- skipping (cross build continues)"
    fi
fi

# --- the app ------------------------------------------------------------
# LDLIBS is the exact library list FLTK's configure settled on (Xft,
# Xrender, fontconfig, X11, ...). Reusing it keeps this in step with
# build-fltk.sh's --disable-* choices instead of re-deriving the set here.
# libfltk.so records those in its own DT_NEEDED anyway, so an empty value
# (unbuilt submodule, pruned tree) is not fatal -- just less explicit.
FLTK_LDLIBS=""
if [ -f "$REPO/userspace/src/fltk/makeinclude" ]; then
    FLTK_LDLIBS="$(sed -n 's/^LDLIBS[[:space:]]*=[[:space:]]*//p' \
        "$REPO/userspace/src/fltk/makeinclude")"
fi

echo "==> cross-building pikostore against $STAGE"
mkdir -p "$STAGE/usr/bin"
make -C "$SRC" clean >/dev/null 2>&1 || true
make -C "$SRC" \
    CXX="$TOOLCHAIN_BIN_DIR/$HOST-g++" \
    STAGE="$STAGE" \
    FLTK_LDLIBS="$FLTK_LDLIBS"

cp "$SRC/pikostore" "$STAGE/usr/bin/pikostore"

# Same check build-fltk.sh makes for fltktest: a binary that linked
# statically, or against the build host's own FLTK, would "work" here and
# fail on the device.
needed="$("$TOOLCHAIN_BIN_DIR/$HOST-readelf" -d "$STAGE/usr/bin/pikostore" \
    | grep -oE '\[lib[^]]+\]' | tr -d '[]' | tr '\n' ' ')"
echo "    NEEDED: $needed"
case " $needed " in
    *" libfltk.so.$FL_DSO_VERSION "*) : ;;
    *)
        echo "tools/build-pikostore.sh: pikostore does not NEED libfltk.so.$FL_DSO_VERSION" >&2
        echo "-- it linked statically or against the wrong library." >&2
        exit 1
        ;;
esac

"$TOOLCHAIN_BIN_DIR/$HOST-strip" "$STAGE/usr/bin/pikostore" 2>/dev/null || true

echo ""
echo "==> done: $STAGE/usr/bin/pikostore ($(du -h "$STAGE/usr/bin/pikostore" | cut -f1))"
echo "    ship it with tools/build-matchbox-payload.sh"
echo ""
