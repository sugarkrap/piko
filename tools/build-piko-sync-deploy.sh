#!/bin/sh
set -eu

# Builds piko-sync-deploy: the host-side CLI that replaced
# tools/chunked-deploy.sh's SSH-chunking with piko-sync's protocol. Plain
# host g++ -- no FLTK, no cross toolchain, no staged X11/FLTK tree needed
# (unlike piko-sync-server/-client) -- it's a synchronous CLI tool, not a
# GUI, talking over a plain TCP socket. See
# userspace/src/piko-sync-deploy/manifest.yaml and piko-sync-deploy.cxx's own header.
#
# WHY THE HOST TESTS RUN HERE: tests/manifest-test.cxx covers yaml_lite.h's
# parser and manifest.h's interpreter against the REAL tracked
# manifest.yaml plus fixture repo trees, and piko-sync's own
# tests/protocol-test.cxx covers the deploy message set this all rides on
# -- both need no socket and no device, so both run on every build, same
# policy as tools/build-piko-sync.sh has for its own tests. Set
# PIKO_SYNC_DEPLOY_SKIP_TESTS=1 to skip if you are iterating and know what you
# are doing.
#
# Usage:
#   tools/build-piko-sync-deploy.sh
#
# Exit status:
#   0   piko-sync-deploy is built at userspace/src/piko-sync-deploy/piko-sync-deploy
#   1   missing host g++, test failure, or a build error

REPO="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$REPO/userspace/src/piko-sync-deploy"
HOSTCXX="${HOSTCXX:-g++}"

if [ ! -f "$SRC/piko-sync-deploy.cxx" ]; then
    echo "tools/build-piko-sync-deploy.sh: $SRC/piko-sync-deploy.cxx is missing" >&2
    exit 1
fi

if ! command -v "$HOSTCXX" >/dev/null 2>&1; then
    echo "tools/build-piko-sync-deploy.sh: no host C++ compiler ($HOSTCXX)" >&2
    exit 1
fi

if [ "${PIKO_SYNC_DEPLOY_SKIP_TESTS:-0}" = "0" ]; then
    echo "==> running host tests (piko-sync protocol + yaml_lite + manifest)"
    protocol_testbin="$(mktemp -d)/protocol-test"
    "$HOSTCXX" -O2 -Wall -Wextra -std=c++98 \
        -o "$protocol_testbin" "$REPO/userspace/src/piko-sync/tests/protocol-test.cxx"
    "$protocol_testbin"
    rm -rf "$(dirname "$protocol_testbin")"

    manifest_testbin="$(mktemp -d)/manifest-test"
    "$HOSTCXX" -O2 -Wall -Wextra -std=c++98 -I"$SRC" \
        -o "$manifest_testbin" "$SRC/tests/manifest-test.cxx"
    ( cd "$SRC/tests" && "$manifest_testbin" )
    rm -rf "$(dirname "$manifest_testbin")"
fi

echo "==> building piko-sync-deploy"
"$HOSTCXX" -O2 -Wall -Wextra -std=c++98 -o "$SRC/piko-sync-deploy" "$SRC/piko-sync-deploy.cxx"

echo "==> $SRC/piko-sync-deploy"
