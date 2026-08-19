#!/bin/sh
set -eu

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$REPO/userspace/src/piko-sync-deploy"
HOSTCXX="${HOSTCXX:-g++}"

if [ ! -f "$SRC/piko-sync-deploy.cxx" ]; then
    echo "tools/userspace/build-piko-sync-deploy.sh: $SRC/piko-sync-deploy.cxx is missing" >&2
    exit 1
fi

if ! command -v "$HOSTCXX" >/dev/null 2>&1; then
    echo "tools/userspace/build-piko-sync-deploy.sh: no host C++ compiler ($HOSTCXX)" >&2
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
