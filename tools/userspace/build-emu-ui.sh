#!/bin/sh
set -eu

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
DL_DIR="${DL_DIR:-$REPO/build/dl}"
. "$REPO/tools/userspace/dl-cache.sh"
piko_seed_dl_cache "$REPO" "$DL_DIR"

FONT_VERSION="${FONT_VERSION:-2.1.5-1}"
FONT_DEB="$DL_DIR/fonts-liberation2_${FONT_VERSION}_all.deb"
FONT_URL="https://deb.debian.org/debian/pool/main/f/fonts-liberation2/fonts-liberation2_${FONT_VERSION}_all.deb"
FONT_SHA256="35ffaa54f117e633e89a5da89af235a073b34dbe582726fbf6989568f7fd9bda"
FONT_SRC="$REPO/build/src/liberation2-$FONT_VERSION"
FONT_DIR="$FONT_SRC/usr/share/fonts/truetype/liberation2"

STAGE_DIR="${STAGE_DIR:-$REPO/build/stage-ui}"
OUT_DIR="$STAGE_DIR/usr/local/share/piko/ui"

FONT_BAKER="$REPO/tools/scripts/font-to-pkfn.js"
UI_BAKER="$REPO/tools/scripts/ui-bake.js"
IMAGE_LIB="$REPO/tools/scripts/piko-image.js"
ICON_CALENDAR="${ICON_CALENDAR:-$REPO/userspace/src/pikoemu/assets/calendar.png}"
ICON_ERROR="${ICON_ERROR:-$REPO/userspace/src/pikoemu/assets/error.png}"

SANS_SIZE="${SANS_SIZE:-13}"
SANS_BOLD_SIZE="${SANS_BOLD_SIZE:-18}"

FORCE=0
for arg in "$@"; do
    case "$arg" in
        --force) FORCE=1 ;;
        *)
            echo "tools/userspace/build-emu-ui.sh: unknown argument: $arg" >&2
            echo "Usage: tools/userspace/build-emu-ui.sh [--force]" >&2
            exit 1
            ;;
    esac
done

for f in "$FONT_BAKER" "$UI_BAKER" "$IMAGE_LIB" "$ICON_CALENDAR" "$ICON_ERROR"; do
    if [ ! -f "$f" ]; then
        echo "tools/userspace/build-emu-ui.sh: missing $f" >&2
        exit 1
    fi
done

PIKO_STAMP="$STAGE_DIR/.piko-stamp"
PIKO_STATE="$(sha256sum "$0" "$FONT_BAKER" "$UI_BAKER" "$ICON_CALENDAR" "$ICON_ERROR" "$IMAGE_LIB" \
    "$REPO/package-lock.json" | cut -d' ' -f1 | tr '\n' ' ')$FONT_SHA256"
if [ "$FORCE" -eq 0 ] && [ -f "$PIKO_STAMP" ] \
   && [ "$(cat "$PIKO_STAMP")" = "$PIKO_STATE" ] && [ -d "$OUT_DIR" ]; then
    echo "==> emu ui already staged for these inputs, skipping (--force to rebuild)"
    exit 0
fi

if ! command -v node >/dev/null 2>&1; then
    echo "tools/userspace/build-emu-ui.sh: node not found." >&2
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
    echo "==> reusing cached $(basename "$FONT_DEB")"
fi

echo "==> verifying sha256 of $(basename "$FONT_DEB")"
actual="$(sha256sum "$FONT_DEB" | cut -d' ' -f1)"
if [ "$actual" != "$FONT_SHA256" ]; then
    echo "tools/userspace/build-emu-ui.sh: SHA-256 mismatch for $FONT_DEB" >&2
    echo "  expected: $FONT_SHA256" >&2
    echo "  actual:   $actual" >&2
    exit 1
fi

if [ ! -f "$FONT_DIR/LiberationSans-Regular.ttf" ]; then
    echo "==> extracting Liberation $FONT_VERSION"
    rm -rf "$FONT_SRC"
    mkdir -p "$FONT_SRC"
    ( cd "$FONT_SRC" && ar x "$FONT_DEB" && tar xf data.tar.* && rm -f data.tar.* control.tar.* debian-binary )
fi
for ttf in LiberationSans-Regular.ttf LiberationSans-Bold.ttf; do
    if [ ! -f "$FONT_DIR/$ttf" ]; then
        echo "tools/userspace/build-emu-ui.sh: $ttf missing after extracting $(basename "$FONT_DEB")" >&2
        exit 1
    fi
done

echo "==> baking the emu ui into $OUT_DIR"
rm -rf "$STAGE_DIR"
mkdir -p "$OUT_DIR"

node "$FONT_BAKER" --size "$SANS_SIZE"      "$FONT_DIR/LiberationSans-Regular.ttf" "$OUT_DIR/sans.pkfn"
node "$FONT_BAKER" --size "$SANS_BOLD_SIZE" "$FONT_DIR/LiberationSans-Bold.ttf"    "$OUT_DIR/sans-bold.pkfn"
node "$UI_BAKER" --icon calendar="$ICON_CALENDAR" --icon error="$ICON_ERROR" "$OUT_DIR/notify.pkui"

for f in sans.pkfn sans-bold.pkfn notify.pkui; do
    if [ ! -s "$OUT_DIR/$f" ]; then
        echo "tools/userspace/build-emu-ui.sh: $f was not produced" >&2
        exit 1
    fi
done

printf '%s' "$PIKO_STATE" > "$PIKO_STAMP"
echo "    $OUT_DIR ($(ls "$OUT_DIR" | wc -l) files)"
