#!/bin/sh
set -eu

# Cross-builds found-file-browser ("Found", userspace/src/found-file-browser
# -- a tracked git submodule) against the FLTK/X11 tree staged in
# userspace/stage-target, and drops the binary there for
# tools/build-matchbox-payload.sh to ship to /usr/local/bin.
#
# WHY IT IS SEPARATE FROM tools/build-fltk.sh: same rationale as
# tools/build-pikostore.sh -- that script builds the toolkit plus
# fltktest, a smoke test for the toolkit itself. found-file-browser is an
# application that happens to use it, with its own repo and release
# cadence. It depends on build-fltk.sh having run, and says so loudly
# rather than silently skipping, same policy as tools/build-st.sh.
#
# Exit status:
#   0   found-file-browser is staged in $STAGE/usr/bin
#   1   missing toolchain, missing FLTK stage, or build error

REPO="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$REPO/userspace/src/found-file-browser"
STAGE="${STAGE:-$REPO/userspace/stage-target}"

TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin}"
HOST="${HOST:-arm-unknown-linux-uclibcgnueabi}"
FL_DSO_VERSION="${FL_DSO_VERSION:-1.3}"

if [ ! -f "$SRC/found-file-browser.cxx" ]; then
    echo "tools/build-found-file-browser.sh: $SRC is empty" >&2
    echo "  it is a git submodule -- run:" >&2
    echo "    git submodule update --init userspace/src/found-file-browser" >&2
    exit 1
fi

if ! command -v "$TOOLCHAIN_BIN_DIR/$HOST-g++" >/dev/null 2>&1; then
    echo "tools/build-found-file-browser.sh: no C++ cross compiler at $TOOLCHAIN_BIN_DIR/$HOST-g++" >&2
    exit 1
fi

if [ ! -f "$STAGE/usr/lib/libfltk.so.$FL_DSO_VERSION" ]; then
    echo "tools/build-found-file-browser.sh: no staged libfltk at $STAGE" >&2
    echo "  run tools/build-fltk.sh first (see docs/HOWTO-FLTK.md)" >&2
    exit 1
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

echo "==> cross-building found-file-browser against $STAGE"
mkdir -p "$STAGE/usr/bin"
make -C "$SRC" clean >/dev/null 2>&1 || true
make -C "$SRC" \
    CXX="$TOOLCHAIN_BIN_DIR/$HOST-g++" \
    STAGE="$STAGE" \
    FLTK_LDLIBS="$FLTK_LDLIBS"

cp "$SRC/found-file-browser" "$STAGE/usr/bin/found-file-browser"

# Same check build-fltk.sh makes for fltktest: a binary that linked
# statically, or against the build host's own FLTK, would "work" here and
# fail on the device.
needed="$("$TOOLCHAIN_BIN_DIR/$HOST-readelf" -d "$STAGE/usr/bin/found-file-browser" \
    | grep -oE '\[lib[^]]+\]' | tr -d '[]' | tr '\n' ' ')"
echo "    NEEDED: $needed"
case " $needed " in
    *" libfltk.so.$FL_DSO_VERSION "*) : ;;
    *)
        echo "tools/build-found-file-browser.sh: found-file-browser does not NEED libfltk.so.$FL_DSO_VERSION" >&2
        echo "-- it linked statically or against the wrong library." >&2
        exit 1
        ;;
esac

"$TOOLCHAIN_BIN_DIR/$HOST-strip" "$STAGE/usr/bin/found-file-browser" 2>/dev/null || true

echo ""
echo "==> done: $STAGE/usr/bin/found-file-browser ($(du -h "$STAGE/usr/bin/found-file-browser" | cut -f1))"
echo "    ship it with tools/build-matchbox-payload.sh"
echo ""
