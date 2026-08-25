#!/bin/sh
set -eu

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
DL_DIR="${DL_DIR:-$REPO/build/dl}"
. "$REPO/tools/userspace/dl-cache.sh"
piko_seed_dl_cache "$REPO" "$DL_DIR"

BEZEL_VERSION="${BEZEL_VERSION:-v1.0.0}"
BEZEL_ZIP="$DL_DIR/Starman99x-Mega-Bezel_$BEZEL_VERSION.zip"
BEZEL_URL="https://github.com/Starman99x/Starman99x-shader-presets/releases/download/Starman99x-Mega-Bezel/Starman99x-Mega-Bezel_$BEZEL_VERSION.zip"
BEZEL_SHA256="939105cb129e1b7509b690e4bde700290b451148291c13833b8dd2370338ab21"

STAGE_DIR="${STAGE_DIR:-$REPO/build/stage-bezels}"
CONVERTER="$REPO/tools/scripts/starman-to-pkbz.js"
IMAGE_LIB="$REPO/tools/scripts/piko-image.js"
OUT_DIR="$STAGE_DIR/usr/local/.zaurus/bezels"

FORCE=0
for arg in "$@"; do
    case "$arg" in
        --force) FORCE=1 ;;
        *)
            echo "tools/userspace/build-bezels.sh: unknown argument: $arg" >&2
            echo "Usage: tools/userspace/build-bezels.sh [--force]" >&2
            exit 1
            ;;
    esac
done

PIKO_STAMP="$STAGE_DIR/.piko-stamp"
PIKO_STATE="$(sha256sum "$0" "$CONVERTER" "$IMAGE_LIB" "$REPO/package-lock.json" \
    | cut -d' ' -f1 | tr '\n' ' ')$BEZEL_SHA256"
if [ "$FORCE" -eq 0 ] && [ -f "$PIKO_STAMP" ] \
   && [ "$(cat "$PIKO_STAMP")" = "$PIKO_STATE" ] && [ -d "$OUT_DIR" ]; then
    echo "==> bezels already staged for these inputs, skipping (--force to rebuild)"
    exit 0
fi

if ! command -v node >/dev/null 2>&1; then
    echo "tools/userspace/build-bezels.sh: node not found." >&2
    exit 1
fi
if [ ! -d "$REPO/node_modules" ]; then
    echo "==> installing host tooling deps (npm ci)"
    ( cd "$REPO" && npm ci --no-audit --no-fund )
fi

mkdir -p "$DL_DIR"
if [ ! -f "$BEZEL_ZIP" ]; then
    echo "==> downloading $BEZEL_URL"
    curl -fL --http1.1 -o "$BEZEL_ZIP.partial" "$BEZEL_URL"
    mv "$BEZEL_ZIP.partial" "$BEZEL_ZIP"
else
    echo "==> reusing cached $BEZEL_ZIP"
fi

echo "==> verifying sha256 of $(basename "$BEZEL_ZIP")"
actual="$(sha256sum "$BEZEL_ZIP" | cut -d' ' -f1)"
if [ "$actual" != "$BEZEL_SHA256" ]; then
    echo "tools/userspace/build-bezels.sh: SHA-256 mismatch for $BEZEL_ZIP" >&2
    echo "  expected: $BEZEL_SHA256" >&2
    echo "  actual:   $actual" >&2
    exit 1
fi

echo "==> baking bezels into $OUT_DIR"
rm -rf "$STAGE_DIR"
mkdir -p "$OUT_DIR"
node "$CONVERTER" "$BEZEL_ZIP" "$OUT_DIR"

count="$(find "$OUT_DIR" -name '*.pkbz' | wc -l)"
if [ "$count" -eq 0 ]; then
    echo "tools/userspace/build-bezels.sh: the converter wrote no .pkbz files" >&2
    exit 1
fi

printf '%s' "$PIKO_STATE" > "$PIKO_STAMP"
echo "==> done: $count file(s), $(du -sh "$OUT_DIR" | cut -f1)"
