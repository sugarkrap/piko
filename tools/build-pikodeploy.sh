#!/bin/sh
set -eu

# Builds pikodeploy: the host-side CLI that replaced
# tools/chunked-deploy.sh's SSH-chunking with pikoxfer's protocol. Plain
# host g++ -- no FLTK, no cross toolchain, no staged X11/FLTK tree needed
# (unlike pikoxfer-server/-client) -- it's a synchronous CLI tool, not a
# GUI, talking over a plain TCP socket. See
# userspace/src/pikodeploy/manifest.yaml and pikodeploy.cxx's own header.
#
# WHY THE HOST TESTS RUN HERE: tests/manifest-test.cxx covers yaml_lite.h's
# parser and manifest.h's interpreter against the REAL tracked
# manifest.yaml plus fixture repo trees, and pikoxfer's own
# tests/protocol-test.cxx covers the deploy message set this all rides on
# -- both need no socket and no device, so both run on every build, same
# policy as tools/build-pikoxfer.sh has for its own tests. Set
# PIKODEPLOY_SKIP_TESTS=1 to skip if you are iterating and know what you
# are doing.
#
# Usage:
#   tools/build-pikodeploy.sh
#
# Exit status:
#   0   pikodeploy is built at userspace/src/pikodeploy/pikodeploy
#   1   missing host g++, test failure, or a build error

REPO="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$REPO/userspace/src/pikodeploy"
HOSTCXX="${HOSTCXX:-g++}"

if [ ! -f "$SRC/pikodeploy.cxx" ]; then
    echo "tools/build-pikodeploy.sh: $SRC/pikodeploy.cxx is missing" >&2
    exit 1
fi

if ! command -v "$HOSTCXX" >/dev/null 2>&1; then
    echo "tools/build-pikodeploy.sh: no host C++ compiler ($HOSTCXX)" >&2
    exit 1
fi

if [ "${PIKODEPLOY_SKIP_TESTS:-0}" = "0" ]; then
    echo "==> running host tests (pikoxfer protocol + yaml_lite + manifest)"
    protocol_testbin="$(mktemp -d)/protocol-test"
    "$HOSTCXX" -O2 -Wall -Wextra -std=c++98 \
        -o "$protocol_testbin" "$REPO/userspace/src/pikoxfer/tests/protocol-test.cxx"
    "$protocol_testbin"
    rm -rf "$(dirname "$protocol_testbin")"

    manifest_testbin="$(mktemp -d)/manifest-test"
    "$HOSTCXX" -O2 -Wall -Wextra -std=c++98 -I"$SRC" \
        -o "$manifest_testbin" "$SRC/tests/manifest-test.cxx"
    ( cd "$SRC/tests" && "$manifest_testbin" )
    rm -rf "$(dirname "$manifest_testbin")"
fi

echo "==> building pikodeploy"
"$HOSTCXX" -O2 -Wall -Wextra -std=c++98 -o "$SRC/pikodeploy" "$SRC/pikodeploy.cxx"

echo "==> $SRC/pikodeploy"
