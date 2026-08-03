#!/bin/sh
set -eu

# Builds pikoxfer: pikoxfer-server (cross-compiled, runs on the Zaurus)
# and pikoxfer-client (built against the HOST's own FLTK, runs on the
# build machine). See userspace/src/pikoxfer/README.md for what the app
# actually does.
#
# WHY THE SERVER IS A STANDALONE .ipk AND NOT FOLDED INTO THE MATCHBOX
# PAYLOAD: pikostore is assembled into the shared X11/Matchbox tar by
# tools/build-matchbox-payload.sh, but that script is the working,
# routinely-exercised path for the project's one spare board -- adding a
# brand new app to it is a change to a script other things depend on
# being stable, not something to fold in as a side effect of this app's
# first version. A standalone .ipk (tools/make-ipk.sh) installs and
# removes independently, and folding pikoxfer into the shared payload
# later, once it has actually proven itself on hardware, is a small,
# separate, deliberate change to make then.
#
# WHY THE HOST TESTS RUN HERE: userspace/src/pikoxfer/tests/ covers the
# wire protocol, the resume/collision decision and the transfer queue
# bookkeeping, and needs no FLTK, no X, no socket and no device -- same
# reasoning tools/build-pikostore.sh has for romstate-test.cxx. Set
# PIKOXFER_SKIP_TESTS=1 to skip it if you are iterating and know what
# you are doing.
#
# Usage:
#   tools/build-pikoxfer.sh [--server-only|--client-only] [--deploy user@host]
#
# --deploy user@host also scp's the built .ipk to the device and installs
# it onto the SD card (pkgadd ... card) over SSH. Without it, the script
# only builds and leaves the .ipk in $OUTDIR for you to ship by hand.
#
# Exit status:
#   0   everything requested is built (and, with --deploy, installed)
#   1   missing toolchain/FLTK stage, missing host FLTK, test failure,
#       or a build/deploy error

REPO="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$REPO/userspace/src/pikoxfer"
STAGE="${STAGE:-$REPO/userspace/stage-target}"
OUTDIR="${OUTDIR:-$REPO}"

TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin}"
HOST="${HOST:-arm-unknown-linux-uclibcgnueabi}"
FL_DSO_VERSION="${FL_DSO_VERSION:-1.3}"
VERSION="${PIKOXFER_VERSION:-1.0}"

BUILD_SERVER=1
BUILD_CLIENT=1
DEPLOY_TARGET=""

while [ $# -gt 0 ]; do
    case "$1" in
        --server-only) BUILD_CLIENT=0; shift ;;
        --client-only) BUILD_SERVER=0; shift ;;
        --deploy)      DEPLOY_TARGET="${2:?--deploy needs user@host}"; shift 2 ;;
        -h|--help)     sed -n '3,30p' "$0"; exit 0 ;;
        *) echo "FAILED: unknown option: $1" >&2; exit 1 ;;
    esac
done

if [ ! -f "$SRC/protocol.h" ]; then
    echo "tools/build-pikoxfer.sh: $SRC is missing protocol.h" >&2
    exit 1
fi

# --- host-side tests -----------------------------------------------------
if [ "${PIKOXFER_SKIP_TESTS:-0}" = "0" ]; then
    echo "==> running host tests (protocol, resume/collision, transfer queue)"
    HOSTCXX="${HOSTCXX:-g++}"
    if command -v "$HOSTCXX" >/dev/null 2>&1; then
        testbin="$(mktemp -d)/pikoxfer-protocol-test"
        "$HOSTCXX" -O2 -Wall -Wextra -o "$testbin" "$SRC/tests/protocol-test.cxx"
        "$testbin"
        rm -rf "$(dirname "$testbin")"
    else
        echo "    no host $HOSTCXX -- skipping (build continues)"
    fi
fi

IPK_PATH=""

# --- the server (cross-compiled) ------------------------------------------
if [ "$BUILD_SERVER" = "1" ]; then
    if ! command -v "$TOOLCHAIN_BIN_DIR/$HOST-g++" >/dev/null 2>&1; then
        echo "tools/build-pikoxfer.sh: no C++ cross compiler at $TOOLCHAIN_BIN_DIR/$HOST-g++" >&2
        exit 1
    fi
    if [ ! -f "$STAGE/usr/lib/libfltk.so.$FL_DSO_VERSION" ]; then
        echo "tools/build-pikoxfer.sh: no staged libfltk at $STAGE" >&2
        echo "  run tools/build-fltk.sh first (see docs/HOWTO-FLTK.md)" >&2
        exit 1
    fi

    FLTK_LDLIBS=""
    if [ -f "$REPO/userspace/src/fltk/makeinclude" ]; then
        FLTK_LDLIBS="$(sed -n 's/^LDLIBS[[:space:]]*=[[:space:]]*//p' \
            "$REPO/userspace/src/fltk/makeinclude")"
    fi

    echo "==> cross-building pikoxfer-server against $STAGE"
    make -C "$SRC" clean >/dev/null 2>&1 || true
    make -C "$SRC" server \
        CXX="$TOOLCHAIN_BIN_DIR/$HOST-g++" \
        STAGE="$STAGE" \
        FLTK_LDLIBS="$FLTK_LDLIBS"

    needed="$("$TOOLCHAIN_BIN_DIR/$HOST-readelf" -d "$SRC/pikoxfer-server" \
        | grep -oE '\[lib[^]]+\]' | tr -d '[]' | tr '\n' ' ')"
    echo "    NEEDED: $needed"
    case " $needed " in
        *" libfltk.so.$FL_DSO_VERSION "*) : ;;
        *)
            echo "tools/build-pikoxfer.sh: pikoxfer-server does not NEED libfltk.so.$FL_DSO_VERSION" >&2
            echo "-- it linked statically or against the wrong library." >&2
            exit 1
            ;;
    esac

    echo "==> packaging pikoxfer-server as an .ipk"
    PKGROOT="$(mktemp -d)"
    trap 'rm -rf "$PKGROOT"' EXIT INT TERM
    mkdir -p "$PKGROOT/usr/bin" "$PKGROOT/usr/share/applications"
    cp "$SRC/pikoxfer-server" "$PKGROOT/usr/bin/pikoxfer-server"
    cp "$REPO/userspace/desktop/pikoxfer-server.desktop" \
        "$PKGROOT/usr/share/applications/pikoxfer-server.desktop"

    "$REPO/tools/make-ipk.sh" --name pikoxfer-server --version "$VERSION" \
        --root "$PKGROOT" --desc "Resilient file transfer -- receiver" \
        --out "$OUTDIR"
    IPK_PATH="$OUTDIR/pikoxfer-server_${VERSION}_piko.ipk"
fi

# --- the client (host FLTK) ------------------------------------------------
if [ "$BUILD_CLIENT" = "1" ]; then
    if ! command -v fltk-config >/dev/null 2>&1; then
        echo "tools/build-pikoxfer.sh: no host fltk-config -- skipping pikoxfer-client" >&2
        echo "  install FLTK 1.3 development files, or build FLTK for the host from" >&2
        echo "  userspace/src/fltk, then re-run with --client-only." >&2
    else
        echo "==> building pikoxfer-client against the host's FLTK"
        make -C "$SRC" client \
            HOSTCXX="${HOSTCXX:-g++}" \
            HOST_FLTK_CXXFLAGS="$(fltk-config --cxxflags)" \
            HOST_FLTK_LDFLAGS="$(fltk-config --ldflags)"
        echo "    built: $SRC/pikoxfer-client"
        echo "    run it from the piko repo root (or set PIKOXFER_REPO_ROOT) so"
        echo "    the Build && Deploy tab can find tools/build-and-deploy.sh"
    fi
fi

# --- optional live deploy ---------------------------------------------------
if [ -n "$DEPLOY_TARGET" ]; then
    if [ -z "$IPK_PATH" ]; then
        echo "tools/build-pikoxfer.sh: --deploy needs the server to have been built" >&2
        exit 1
    fi
    echo "==> shipping $(basename "$IPK_PATH") to $DEPLOY_TARGET"
    scp "$IPK_PATH" "$DEPLOY_TARGET:/tmp/"
    ssh "$DEPLOY_TARGET" "pkgadd /tmp/$(basename "$IPK_PATH") card"
    echo "==> installed on the SD card. Launch pikoxfer-server from the Matchbox desktop."
fi

echo "==> done"
