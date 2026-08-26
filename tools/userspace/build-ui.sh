#!/bin/sh
set -eu

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
DL_DIR="${DL_DIR:-$REPO/build/dl}"
. "$REPO/tools/userspace/dl-cache.sh"
piko_seed_dl_cache "$REPO" "$DL_DIR"

FONT_VERSION="${FONT_VERSION:-2.1.5-1}"
FONT_DEB="$DL_DIR/fonts-liberation2_${FONT_VERSION}_all.deb"
FONT_URL="http://archive.ubuntu.com/ubuntu/pool/main/f/fonts-liberation2/fonts-liberation2_${FONT_VERSION}_all.deb"
FONT_SHA256="787ae3c986eb6d61daa383aa7c3be2e55e4f9bf0f903047caeb4c066038ccaec"
FONT_SUBDIR="usr/share/fonts/truetype/liberation2"

STAGE_DIR="${STAGE_DIR:-$REPO/build/stage-ui}"
OUT_DIR="$STAGE_DIR/usr/local/share/piko/ui"
SRC_DIR="${SRC_DIR:-$REPO/build/src}/liberation2-$FONT_VERSION"
ICON="$REPO/userspace/src/pikoemu/ui/office-calendar.png"
FONT_TOOL="$REPO/tools/scripts/font-to-pkfn.js"
UI_TOOL="$REPO/tools/scripts/ui-bake.js"

FORCE=0
for arg in "$@"; do
    case "$arg" in
        --force) FORCE=1 ;;
        *)
            echo "tools/userspace/build-ui.sh: unknown argument: $arg" >&2
            echo "Usage: tools/userspace/build-ui.sh [--force]" >&2
            exit 1
            ;;
    esac
done

PIKO_STAMP="$STAGE_DIR/.piko-stamp"
PIKO_STATE="$(sha256sum "$0" "$FONT_TOOL" "$UI_TOOL" "$ICON" \
    "$REPO/tools/scripts/piko-image.js" "$REPO/package-lock.json" \
    | cut -d' ' -f1 | tr '\n' ' ')$FONT_SHA256"
if [ "$FORCE" -eq 0 ] && [ -f "$PIKO_STAMP" ] \
   && [ "$(cat "$PIKO_STAMP")" = "$PIKO_STATE" ] && [ -d "$OUT_DIR" ]; then
    echo "==> ui assets already staged for these inputs, skipping (--force to rebuild)"
    exit 0
fi

if ! command -v node >/dev/null 2>&1; then
    echo "tools/userspace/build-ui.sh: node not found." >&2
    exit 1
fi
if [ ! -d "$REPO/node_modules" ]; then
    echo "==> installing host tooling deps (npm ci)"
    ( cd "$REPO" && npm ci --no-audit --no-fund )
fi

mkdir -p "$DL_DIR"
if [ ! -f "$FONT_DEB" ]; then
    echo "==> downloading $FONT_URL"
    curl -fL --http1.1 -o "$FONT_DEB.partial" "$FONT_URL"
    mv "$FONT_DEB.partial" "$FONT_DEB"
else
    echo "==> reusing cached $FONT_DEB"
fi

echo "==> verifying sha256 of $(basename "$FONT_DEB")"
actual="$(sha256sum "$FONT_DEB" | cut -d' ' -f1)"
if [ "$actual" != "$FONT_SHA256" ]; then
    echo "tools/userspace/build-ui.sh: SHA-256 mismatch for $FONT_DEB" >&2
    echo "  expected: $FONT_SHA256" >&2
    echo "  actual:   $actual" >&2
    exit 1
fi

echo "==> extracting Liberation Sans"
rm -rf "$SRC_DIR"
mkdir -p "$SRC_DIR"
( cd "$SRC_DIR" && ar x "$FONT_DEB" && tar xf data.tar.* )

REGULAR="$SRC_DIR/$FONT_SUBDIR/LiberationSans-Regular.ttf"
BOLD="$SRC_DIR/$FONT_SUBDIR/LiberationSans-Bold.ttf"
for f in "$REGULAR" "$BOLD"; do
    if [ ! -f "$f" ]; then
        echo "tools/userspace/build-ui.sh: $f missing from the package" >&2
        exit 1
    fi
done

echo "==> baking ui assets into $OUT_DIR"
rm -rf "$STAGE_DIR"
mkdir -p "$OUT_DIR"
node "$FONT_TOOL" --size 18 "$BOLD" "$OUT_DIR/sans-bold.pkfn"
node "$FONT_TOOL" --size 13 "$REGULAR" "$OUT_DIR/sans.pkfn"
node "$UI_TOOL" --icon "calendar=$ICON" "$OUT_DIR/notify.pkui"

for f in sans-bold.pkfn sans.pkfn notify.pkui; do
    if [ ! -s "$OUT_DIR/$f" ]; then
        echo "tools/userspace/build-ui.sh: $f was not produced" >&2
        exit 1
    fi
done

printf '%s' "$PIKO_STATE" > "$PIKO_STAMP"
echo "==> done: $(du -sh "$OUT_DIR" | cut -f1) in $OUT_DIR"
