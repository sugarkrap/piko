#!/bin/sh
set -eu

# Assembles the Matchbox desktop into a single deployable tar and (with
# --deploy) ships it to the device.
#
# One archive rather than ~100 individual transfers: the link to this
# device is genuinely flaky, and the device's busybox has no tar, so the
# archive is unpacked with our own userspace/src/untar.c (installed at
# /usr/local/bin/untar). That is what untar exists for.
#
# Prerequisites -- this script only *collects*, it does not build:
#   tools/build-thirdparty-deps.sh      zlib expat libpng freetype
#                                       fontconfig + the DejaVu faces
#   tools/setup-x11-src.sh              local patches into the X submodules
#   then configure+make, per component, into the DESTDIRs listed below.
# See docs/HOWTO-MATCHBOX-DESKTOP.md for the per-component configure
# lines, which are NOT all obvious (matchbox-desktop in particular needs
# --sysconfdir=/etc and a forced -DUSE_XSETTINGS).
#
# Usage:
#   tools/build-matchbox-payload.sh [--deploy [user@host]] [--adapter IFACE]
#
# Without --deploy it just writes the tar and stops, so you can inspect it.

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
STAGE="$REPO/userspace/stage-target"
HOST_TRIPLET="${CROSS_HOST:-arm-unknown-linux-uclibcgnueabi}"
TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/$HOST_TRIPLET/bin}"
SYSROOT="$REPO/toolchain/x-tools/$HOST_TRIPLET/$HOST_TRIPLET/sysroot"

PAYLOAD="${PAYLOAD_DIR:-/tmp/mb-payload}"
TARBALL="${PAYLOAD_TAR:-/tmp/matchbox-payload.tar}"

# Per-component DESTDIRs. Each component is built separately (they were
# built in parallel by separate agents) and installed to its own prefix so
# concurrent builds cannot race on a shared tree.
D_WM="${D_WM:-/tmp/mbwm-stage}"
D_DESKTOP="${D_DESKTOP:-/tmp/mb-stage-desktop}"
D_PANEL="${D_PANEL:-/tmp/mb-stage-panel}"
D_COMMON="${D_COMMON:-/tmp/mb-stage-common}"

DEPLOY=0
TARGET=""
ADAPTER=""
while [ $# -gt 0 ]; do
    case "$1" in
        --deploy)  DEPLOY=1; shift
                   case "${1:-}" in -*|"") ;; *) TARGET="$1"; shift ;; esac ;;
        --adapter) ADAPTER="${2:?--adapter needs an interface}"; shift 2 ;;
        -h|--help) sed -n '3,30p' "$0"; exit 0 ;;
        *) echo "FAILED: unknown option: $1" >&2; exit 1 ;;
    esac
done
TARGET="${TARGET:-root@10.208.47.72}"

STRIP="$TOOLCHAIN_BIN_DIR/$HOST_TRIPLET-strip"
[ -x "$STRIP" ] || { echo "FAILED: no strip at $STRIP" >&2; exit 1; }

# Runtime libraries. These are the SONAMEs every Matchbox binary lists in
# DT_NEEDED, minus the four the device already has (libc, libX11, libXext,
# libz). Bare filenames here; the version suffix is discovered below so a
# rebuilt library does not silently keep shipping the old one.
LIBS="libmb libXft libXrender libfreetype libfontconfig libpng16 libexpat"

echo "==> assembling into $PAYLOAD"
rm -rf "$PAYLOAD"
mkdir -p "$PAYLOAD/lib" "$PAYLOAD/usr" "$PAYLOAD/etc"

for base in $LIBS; do
    real="$(ls "$STAGE/usr/lib/" 2>/dev/null | grep -E "^${base}\.so\.[0-9.]+$" | sort -V | tail -1)"
    if [ -z "$real" ]; then
        echo "FAILED: $base not in $STAGE/usr/lib -- build it first" >&2
        exit 1
    fi
    cp "$STAGE/usr/lib/$real" "$PAYLOAD/lib/"
    # DT_NEEDED names the SONAME (libfoo.so.N), so that symlink must exist.
    soname="$(echo "$real" | sed -E 's/^(.*\.so\.[0-9]+)\..*$/\1/')"
    [ "$soname" = "$real" ] || ln -sf "$real" "$PAYLOAD/lib/$soname"
    echo "    lib: $real"
done

# libgcc_s comes from the toolchain, not the staging tree: libexpat needs
# it and nothing else drags it in.
cp "$SYSROOT/lib/libgcc_s.so.1" "$PAYLOAD/lib/"

for d in "$D_WM" "$D_DESKTOP" "$D_PANEL" "$D_COMMON"; do
    if [ ! -d "$d" ]; then
        echo "FAILED: missing component DESTDIR: $d" >&2
        echo "Build that component first (see docs/HOWTO-MATCHBOX-DESKTOP.md)." >&2
        exit 1
    fi
    [ -d "$d/usr" ] && cp -a "$d/usr/." "$PAYLOAD/usr/"
    [ -d "$d/etc" ] && cp -a "$d/etc/." "$PAYLOAD/etc/"
    echo "    merged: $d"
done

# Fonts + fontconfig config. The device ships with NO fonts and no
# /etc/fonts at all, and Matchbox themes ask for "Sans bold 16px" -- a
# generic fontconfig family -- so without these every themed widget
# renders blank.
mkdir -p "$PAYLOAD/usr/share/fonts/truetype/dejavu"
cp "$STAGE"/usr/share/fonts/truetype/dejavu/*.ttf \
   "$PAYLOAD/usr/share/fonts/truetype/dejavu/"
cp -a "$STAGE/etc/fonts" "$PAYLOAD/etc/"

echo "==> pruning"
# .la files are dead weight on flash AND leak absolute host build paths
# into the image; dlopen() loads the .so directly and never reads them.
find "$PAYLOAD" -name "*.la" -delete
# Headers and .pc files are for cross-building against this stack, which
# happens on the host, never on the device.
rm -rf "$PAYLOAD/usr/include" "$PAYLOAD/usr/lib/pkgconfig"

echo "==> stripping"
find "$PAYLOAD" -type f | while read -r f; do
    case "$(file -b "$f")" in
        ELF*) chmod u+w "$f"; "$STRIP" --strip-unneeded "$f" 2>/dev/null || true ;;
    esac
done

echo "==> verifying"
# Catch a host binary that wandered in via a mis-set CC.
if find "$PAYLOAD" -type f -exec file {} \; | grep "ELF" | grep -qv "ARM"; then
    echo "FAILED: non-ARM ELF in payload:" >&2
    find "$PAYLOAD" -type f -exec file {} \; | grep "ELF" | grep -v "ARM" >&2
    exit 1
fi
# Every DT_NEEDED must be satisfied by the payload or already present on
# the device. Getting this wrong means a binary that dies at exec time
# with a bare "not found", which is painful to diagnose over this link.
ON_DEVICE="libc.so.0 libX11.so.6 libXext.so.6 libz.so.1"
NEEDED="$(find "$PAYLOAD" -type f | while read -r f; do
    case "$(file -b "$f")" in
        ELF*) "$TOOLCHAIN_BIN_DIR/$HOST_TRIPLET-readelf" -d "$f" 2>/dev/null \
                | grep -oE '\[lib[^]]+\]' | tr -d '[]' ;;
    esac
done | sort -u)"
missing=0
for n in $NEEDED; do
    [ -e "$PAYLOAD/lib/$n" ] && continue
    case " $ON_DEVICE " in *" $n "*) continue ;; esac
    echo "FAILED: nothing provides $n" >&2
    missing=1
done
[ "$missing" -eq 0 ] || exit 1
echo "    all DT_NEEDED satisfied; all ELF are ARM"

tar --format=ustar -cf "$TARBALL" -C "$PAYLOAD" .
echo "==> $TARBALL ($(wc -c < "$TARBALL") bytes, $(find "$PAYLOAD" -type f | wc -l) files)"

if [ "$DEPLOY" -eq 0 ]; then
    echo "Not deploying (pass --deploy to ship it)."
    exit 0
fi

SSH_OPTS="-o BatchMode=yes -o ConnectTimeout=30 -o ServerAliveInterval=15"
SSH_OPTS="$SSH_OPTS -o ServerAliveCountMax=8 -o StrictHostKeyChecking=accept-new"
[ -n "$ADAPTER" ] && SSH_OPTS="$SSH_OPTS -B $ADAPTER"
KEY="${SSH_KEY:-$HOME/.ssh/zaurus_ed25519}"

echo "==> deploying to $TARGET"
ssh $SSH_OPTS -i "$KEY" "$TARGET" "cat > /tmp/mb.tar" < "$TARBALL"
want="$(wc -c < "$TARBALL")"
got="$(ssh $SSH_OPTS -i "$KEY" "$TARGET" "wc -c < /tmp/mb.tar" | tr -d ' \r\n')"
if [ "$want" != "$got" ]; then
    echo "FAILED: short transfer (sent $want, device has $got)" >&2
    exit 1
fi
ssh $SSH_OPTS -i "$KEY" "$TARGET" "/usr/local/bin/untar /tmp/mb.tar / && rm -f /tmp/mb.tar"
echo "==> deployed. Start it with:  DISPLAY=:0 matchbox-session &"
