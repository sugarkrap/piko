#!/bin/sh
set -eu

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$REPO/userspace/src/piko-sync"
STAGE="${STAGE:-$REPO/userspace/stage-target}"
OUTDIR="${OUTDIR:-$REPO}"

TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin}"
HOST="${HOST:-arm-unknown-linux-uclibcgnueabi}"
FL_DSO_VERSION="${FL_DSO_VERSION:-1.3}"
VERSION="${PIKO_SYNC_VERSION:-1.0}"

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
    echo "tools/userspace/build-piko-sync.sh: $SRC is missing protocol.h" >&2
    exit 1
fi

if [ "${PIKO_SYNC_SKIP_TESTS:-0}" = "0" ]; then
    echo "==> running host tests (protocol, resume/collision, transfer queue, settings)"
    HOSTCXX="${HOSTCXX:-g++}"
    if command -v "$HOSTCXX" >/dev/null 2>&1; then
        testdir="$(mktemp -d)"
        for t in protocol settings; do
            "$HOSTCXX" -O2 -Wall -Wextra -o "$testdir/piko-sync-$t-test" \
                "$SRC/tests/$t-test.cxx"
            "$testdir/piko-sync-$t-test"
        done
        rm -rf "$testdir"
    else
        echo "    no host $HOSTCXX -- skipping (build continues)"
    fi
fi

IPK_PATH=""

if [ "$BUILD_SERVER" = "1" ]; then
    if ! command -v "$TOOLCHAIN_BIN_DIR/$HOST-g++" >/dev/null 2>&1; then
        echo "tools/userspace/build-piko-sync.sh: no C++ cross compiler at $TOOLCHAIN_BIN_DIR/$HOST-g++" >&2
        exit 1
    fi
    if [ ! -f "$STAGE/usr/lib/libfltk.so.$FL_DSO_VERSION" ]; then
        echo "tools/userspace/build-piko-sync.sh: no staged libfltk at $STAGE" >&2
        echo "  run tools/userspace/build-fltk.sh first" >&2
        exit 1
    fi

    FLTK_LDLIBS=""
    if [ -f "$REPO/userspace/src/fltk/makeinclude" ]; then
        FLTK_LDLIBS="$(sed -n 's/^LDLIBS[[:space:]]*=[[:space:]]*//p' \
            "$REPO/userspace/src/fltk/makeinclude")"
    fi

    echo "==> cross-building piko-sync-server against $STAGE"
    make -C "$SRC" clean >/dev/null 2>&1 || true
    make -C "$SRC" server \
        CXX="$TOOLCHAIN_BIN_DIR/$HOST-g++" \
        STAGE="$STAGE" \
        FLTK_LDLIBS="$FLTK_LDLIBS"

    needed="$("$TOOLCHAIN_BIN_DIR/$HOST-readelf" -d "$SRC/piko-sync-server" \
        | grep -oE '\[lib[^]]+\]' | tr -d '[]' | tr '\n' ' ')"
    echo "    NEEDED: $needed"
    case " $needed " in
        *" libfltk.so.$FL_DSO_VERSION "*) : ;;
        *)
            echo "tools/userspace/build-piko-sync.sh: piko-sync-server does not NEED libfltk.so.$FL_DSO_VERSION" >&2
            echo "-- it linked statically or against the wrong library." >&2
            exit 1
            ;;
    esac

    echo "==> packaging piko-sync-server as an .ipk"
    PKGROOT="$(mktemp -d)"
    trap 'rm -rf "$PKGROOT"' EXIT INT TERM
    mkdir -p "$PKGROOT/usr/bin" "$PKGROOT/usr/share/applications" "$PKGROOT/usr/share/pixmaps"
    cp "$SRC/piko-sync-server" "$PKGROOT/usr/bin/piko-sync-server"
    cp "$REPO/userspace/desktop/piko-sync-server.desktop" \
        "$PKGROOT/usr/share/applications/piko-sync-server.desktop"
    cp "$REPO/userspace/desktop/piko-sync-server.png" \
        "$PKGROOT/usr/share/pixmaps/piko-sync-server.png"

    "$REPO/tools/userspace/make-ipk.sh" --name piko-sync-server --version "$VERSION" \
        --root "$PKGROOT" --desc "Resilient file transfer -- receiver" \
        --out "$OUTDIR"
    IPK_PATH="$OUTDIR/piko-sync-server_${VERSION}_piko.ipk"
fi

if [ "$BUILD_CLIENT" = "1" ]; then
    if ! command -v fltk-config >/dev/null 2>&1; then
        echo "tools/userspace/build-piko-sync.sh: no host fltk-config -- skipping piko-sync-client" >&2
        echo "  install FLTK 1.3 development files, or build FLTK for the host from" >&2
        echo "  userspace/src/fltk, then re-run with --client-only." >&2
    else
        echo "==> building piko-sync-client against the host's FLTK"
        make -C "$SRC" client \
            HOSTCXX="${HOSTCXX:-g++}" \
            HOST_FLTK_CXXFLAGS="$(fltk-config --cxxflags)" \
            HOST_FLTK_LDFLAGS="$(fltk-config --ldflags)"
        echo "    built: $SRC/piko-sync-client"
        echo "    run it from the piko repo root (or set PIKO_SYNC_REPO_ROOT) so"
        echo "    the Build && Deploy tab can find tools/build-and-deploy.sh"
    fi
fi

if [ -n "$DEPLOY_TARGET" ]; then
    if [ -z "$IPK_PATH" ]; then
        echo "tools/userspace/build-piko-sync.sh: --deploy needs the server to have been built" >&2
        exit 1
    fi
    echo "==> shipping $(basename "$IPK_PATH") to $DEPLOY_TARGET"
    scp "$IPK_PATH" "$DEPLOY_TARGET:/tmp/"
    ssh "$DEPLOY_TARGET" "pkgadd /tmp/$(basename "$IPK_PATH") card"
    echo "==> installed on the SD card. Launch piko-sync-server from the Matchbox desktop."
fi

echo "==> done"
