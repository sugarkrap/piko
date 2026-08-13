#!/bin/sh
set -eu

REPO="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
exec "$REPO/tools/build-sd-card.sh" "$@"
