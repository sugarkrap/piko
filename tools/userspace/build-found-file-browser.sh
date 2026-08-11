#!/bin/sh
set -eu

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$REPO/userspace/src/found-file-browser"
STAGE="${STAGE:-$REPO/userspace/stage-target}"

TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin}"
HOST="${HOST:-arm-unknown-linux-uclibcgnueabi}"
FL_DSO_VERSION="${FL_DSO_VERSION:-1.3}"

if [ ! -f "$SRC/found-file-browser.cxx" ]; then
    echo "tools/userspace/build-found-file-browser.sh: $SRC is empty" >&2
    echo "  it is a git submodule -- run:" >&2
    echo "    git submodule update --init userspace/src/found-file-browser" >&2
    exit 1
fi

if ! command -v "$TOOLCHAIN_BIN_DIR/$HOST-g++" >/dev/null 2>&1; then
    echo "tools/userspace/build-found-file-browser.sh: no C++ cross compiler at $TOOLCHAIN_BIN_DIR/$HOST-g++" >&2
    exit 1
fi

if [ ! -f "$STAGE/usr/lib/libfltk.so.$FL_DSO_VERSION" ]; then
    echo "tools/userspace/build-found-file-browser.sh: no staged libfltk at $STAGE" >&2
    echo "  run tools/userspace/build-fltk.sh first" >&2
    exit 1
fi

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

needed="$("$TOOLCHAIN_BIN_DIR/$HOST-readelf" -d "$STAGE/usr/bin/found-file-browser" \
    | grep -oE '\[lib[^]]+\]' | tr -d '[]' | tr '\n' ' ')"
echo "    NEEDED: $needed"
case " $needed " in
    *" libfltk.so.$FL_DSO_VERSION "*) : ;;
    *)
        echo "tools/userspace/build-found-file-browser.sh: found-file-browser does not NEED libfltk.so.$FL_DSO_VERSION" >&2
        echo "-- it linked statically or against the wrong library." >&2
        exit 1
        ;;
esac

"$TOOLCHAIN_BIN_DIR/$HOST-strip" "$STAGE/usr/bin/found-file-browser" 2>/dev/null || true

echo ""
echo "==> done: $STAGE/usr/bin/found-file-browser ($(du -h "$STAGE/usr/bin/found-file-browser" | cut -f1))"
echo "    ship it with tools/userspace/build-matchbox-payload.sh"
echo ""
