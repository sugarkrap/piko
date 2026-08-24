#!/bin/sh
set -eu

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
SRC_DIR="${SRC_DIR:-$REPO/build/src}"
DL_DIR="${DL_DIR:-$REPO/build/dl}"
. "$REPO/tools/userspace/dl-cache.sh"
piko_seed_dl_cache "$REPO" "$DL_DIR"
CACHE_DIR="${CACHE_DIR:-$DL_DIR}"
STAGE_DIR="${TIMIDITY_STAGE:-$REPO/build/stage-timidity}"

FREEPATS_VERSION="20060219"
FREEPATS_TARBALL="$CACHE_DIR/freepats_$FREEPATS_VERSION.orig.tar.gz"
FREEPATS_URL="http://deb.debian.org/debian/pool/main/f/freepats/freepats_$FREEPATS_VERSION.orig.tar.gz"
FREEPATS_SHA256="70bf8ca084df3903d6c9de43fe20539fc0a553d95cfba4d525da3fe66fda5f10"
FREEPATS_INNER_SHA256="0261ea1057b232183fa472432d5cedb0dca33698a5319328cdf193d4b2193c8a"

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

mkdir -p "$CACHE_DIR"

if [ ! -f "$FREEPATS_TARBALL" ]; then
    echo "==> downloading $FREEPATS_URL"
    curl -fL --http1.1 -o "$FREEPATS_TARBALL.partial" "$FREEPATS_URL"
    mv "$FREEPATS_TARBALL.partial" "$FREEPATS_TARBALL"
fi

got="$(sha256sum "$FREEPATS_TARBALL" | cut -d' ' -f1)"
if [ "$got" != "$FREEPATS_SHA256" ]; then
    echo "tools/userspace/build-timidity-patches.sh: SHA-256 mismatch for $FREEPATS_TARBALL" >&2
    echo "  expected: $FREEPATS_SHA256" >&2
    echo "  actual:   $got" >&2
    exit 1
fi

if [ "$FORCE" -eq 0 ] && [ -f "$STAGE_DIR/timidity.cfg" ] && [ -f "$STAGE_DIR/freepats.cfg" ]; then
    echo "==> $STAGE_DIR already staged"
    echo "    patches: $(find "$STAGE_DIR" -name '*.pat' | wc -l)"
    exit 0
fi

WORK="$(mktemp -d /tmp/piko-freepats.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

echo "==> unpacking the Debian source tarball"
tar xzf "$FREEPATS_TARBALL" -C "$WORK"

INNER="$WORK/freepats-$FREEPATS_VERSION/upstream/freepats-$FREEPATS_VERSION.tar.bz2"
if [ ! -f "$INNER" ]; then
    echo "tools/userspace/build-timidity-patches.sh: expected $INNER inside the tarball" >&2
    exit 1
fi

got="$(sha256sum "$INNER" | cut -d' ' -f1)"
if [ "$got" != "$FREEPATS_INNER_SHA256" ]; then
    echo "tools/userspace/build-timidity-patches.sh: SHA-256 mismatch for the inner tarball" >&2
    echo "  expected: $FREEPATS_INNER_SHA256" >&2
    echo "  actual:   $got" >&2
    exit 1
fi

echo "==> unpacking the patch set"
tar xjf "$INNER" -C "$WORK"

if [ ! -f "$WORK/freepats/freepats.cfg" ]; then
    echo "tools/userspace/build-timidity-patches.sh: no freepats.cfg in the patch set" >&2
    exit 1
fi

rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR"
cp -a "$WORK/freepats/." "$STAGE_DIR/"

printf 'source freepats.cfg\n' > "$STAGE_DIR/timidity.cfg"

find "$STAGE_DIR" -type f -exec chmod 0644 {} +
find "$STAGE_DIR" -type d -exec chmod 0755 {} +

echo ""
echo "==> done: $STAGE_DIR"
echo "    patches: $(find "$STAGE_DIR" -name '*.pat' | wc -l)"
echo "    size:    $(du -sh "$STAGE_DIR" | cut -f1)"
echo "    config:  timidity.cfg -> freepats.cfg (relative, so the install path is free)"
