#!/bin/sh
set -eu

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$REPO/userspace/src/pikostore"
STAGE="${STAGE:-$REPO/userspace/stage-target}"

TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin}"
HOST="${HOST:-arm-unknown-linux-uclibcgnueabi}"
FL_DSO_VERSION="${FL_DSO_VERSION:-1.3}"

if [ ! -f "$SRC/pikostore.cxx" ]; then
    echo "tools/userspace/build-pikostore.sh: $SRC is empty" >&2
    echo "  it is a git submodule -- run:" >&2
    echo "    git submodule update --init userspace/src/pikostore" >&2
    exit 1
fi

if ! command -v "$TOOLCHAIN_BIN_DIR/$HOST-g++" >/dev/null 2>&1; then
    echo "tools/userspace/build-pikostore.sh: no C++ cross compiler at $TOOLCHAIN_BIN_DIR/$HOST-g++" >&2
    exit 1
fi

if [ ! -f "$STAGE/usr/lib/libfltk.so.$FL_DSO_VERSION" ]; then
    echo "tools/userspace/build-pikostore.sh: no staged libfltk at $STAGE" >&2
    echo "  run tools/userspace/build-fltk.sh first" >&2
    exit 1
fi

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

needed="$("$TOOLCHAIN_BIN_DIR/$HOST-readelf" -d "$STAGE/usr/bin/pikostore" \
    | grep -oE '\[lib[^]]+\]' | tr -d '[]' | tr '\n' ' ')"
echo "    NEEDED: $needed"
case " $needed " in
    *" libfltk.so.$FL_DSO_VERSION "*) : ;;
    *)
        echo "tools/userspace/build-pikostore.sh: pikostore does not NEED libfltk.so.$FL_DSO_VERSION" >&2
        echo "-- it linked statically or against the wrong library." >&2
        exit 1
        ;;
esac

"$TOOLCHAIN_BIN_DIR/$HOST-strip" "$STAGE/usr/bin/pikostore" 2>/dev/null || true

echo ""
echo "==> done: $STAGE/usr/bin/pikostore ($(du -h "$STAGE/usr/bin/pikostore" | cut -f1))"
echo "    ship it with tools/userspace/build-matchbox-payload.sh"
echo ""
