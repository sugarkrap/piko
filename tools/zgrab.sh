#!/bin/sh
set -eu

# Grab the Zaurus framebuffer and put the PNG on the Wayland clipboard.
#
# Glue between the two halves that already existed: userspace/src/fbgrab.c
# (mmaps /dev/fb0 on the device and writes the raw visible framebuffer to
# stdout) and tools/decode-fb.py (raw -> PNG on the host). This streams the
# dump over SSH so nothing is ever staged on the device -- /tmp there is
# JFFS2 on NAND, and a full 640x480x16 grab is ~600 KB of flash writes per
# screenshot.
#
# Geometry is not hardcoded: fbgrab reports "WxH BPPbpp ..." on stderr and
# this script parses that, so a panel-mode change can't silently produce a
# skewed image. (--width/--height/--bpp override it if you need to.)
#
# Usage:
#   tools/zgrab.sh [--target user@host] [-o out.png] [--crop X,Y,W,H]
#                  [--scale N] [--raw out.raw] [--no-copy] [--dev /dev/fb0]
#                  [--adapter IFACE] [--width W] [--height H] [--bpp N]
#                  [user@host]
# Examples:
#   tools/zgrab.sh                                  # screen -> clipboard
#   tools/zgrab.sh -o /tmp/shot.png                 # clipboard + keep the PNG
#   tools/zgrab.sh --crop 0,440,640,40 --scale 3    # read the panel, blown up
#
# The default target is $ZAURUS_HOST, else root@10.208.47.2. fbgrab is not
# deployed by default; cross-build userspace/src/fbgrab.c and ship it to
# /usr/local/bin/fbgrab on the device first.

TARGET=""
ADAPTER=""
OUTPUT=""
RAW_OUT=""
CROP=""
SCALE=""
FBDEV=""
NO_COPY=0
WIDTH=""
HEIGHT=""
BPP=""

usage() {
    cat <<'EOF'
Grab the Zaurus framebuffer and put the PNG on the Wayland clipboard.

Usage:
  tools/zgrab.sh [--target user@host] [-o out.png] [--crop X,Y,W,H]
                 [--scale N] [--raw out.raw] [--no-copy] [--dev /dev/fb0]
                 [--adapter IFACE] [--width W] [--height H] [--bpp N]
                 [user@host]

  --target user@host  SSH destination (default $ZAURUS_HOST, else
                      root@10.208.47.2). A positional user@host also works.
  --adapter IFACE     Bind the SSH connection to a local interface (ssh -B).
  -o, --output FILE   Also write the PNG here (otherwise clipboard only).
  --raw FILE          Keep the raw framebuffer dump too.
  -c, --crop X,Y,W,H  Extract a region (see tools/decode-fb.py).
  -s, --scale N       Integer upscale, for reading small UI.
  --dev PATH          Framebuffer device on the target (default /dev/fb0).
  --width/--height/--bpp
                      Override the geometry fbgrab reports on stderr.
  --no-copy           Skip the clipboard; requires -o.

Examples:
  tools/zgrab.sh                                  # screen -> clipboard
  tools/zgrab.sh -o /tmp/shot.png                 # clipboard + keep the PNG
  tools/zgrab.sh --crop 0,440,640,40 --scale 3    # read the panel, blown up

fbgrab is not deployed by default; cross-build userspace/src/fbgrab.c and
ship it to /usr/local/bin/fbgrab on the device first.
EOF
    exit "${1:-0}"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --target)
            [ $# -ge 2 ] || { echo "FAILED: --target needs a value" >&2; exit 2; }
            TARGET="$2"; shift 2 ;;
        --adapter)
            [ $# -ge 2 ] || { echo "FAILED: --adapter needs a value" >&2; exit 2; }
            ADAPTER="$2"; shift 2 ;;
        -o|--output)
            [ $# -ge 2 ] || { echo "FAILED: -o needs a value" >&2; exit 2; }
            OUTPUT="$2"; shift 2 ;;
        --raw)
            [ $# -ge 2 ] || { echo "FAILED: --raw needs a value" >&2; exit 2; }
            RAW_OUT="$2"; shift 2 ;;
        -c|--crop)
            [ $# -ge 2 ] || { echo "FAILED: --crop needs a value" >&2; exit 2; }
            CROP="$2"; shift 2 ;;
        -s|--scale)
            [ $# -ge 2 ] || { echo "FAILED: --scale needs a value" >&2; exit 2; }
            SCALE="$2"; shift 2 ;;
        --dev)
            [ $# -ge 2 ] || { echo "FAILED: --dev needs a value" >&2; exit 2; }
            FBDEV="$2"; shift 2 ;;
        --width)
            [ $# -ge 2 ] || { echo "FAILED: --width needs a value" >&2; exit 2; }
            WIDTH="$2"; shift 2 ;;
        --height)
            [ $# -ge 2 ] || { echo "FAILED: --height needs a value" >&2; exit 2; }
            HEIGHT="$2"; shift 2 ;;
        --bpp)
            [ $# -ge 2 ] || { echo "FAILED: --bpp needs a value" >&2; exit 2; }
            BPP="$2"; shift 2 ;;
        --no-copy)
            NO_COPY=1; shift ;;
        -h|--help)
            usage 0 ;;
        -*)
            echo "FAILED: unknown option $1" >&2; usage 2 ;;
        *)
            if [ -n "$TARGET" ]; then
                echo "FAILED: multiple targets specified ($TARGET and $1)" >&2
                exit 2
            fi
            TARGET="$1"; shift ;;
    esac
done

if [ "$NO_COPY" -ne 0 ] && [ -z "$OUTPUT" ] && [ -z "$RAW_OUT" ]; then
    echo "FAILED: --no-copy without -o/--raw would discard the grab" >&2
    exit 2
fi

TARGET="${TARGET:-${ZAURUS_HOST:-root@10.208.47.2}}"
KEY="${HOME}/.ssh/zaurus_ed25519"
SSH_OPTS="-o BatchMode=yes -o ConnectTimeout=30 -o ServerAliveInterval=15 -o ServerAliveCountMax=8 -o StrictHostKeyChecking=accept-new"
[ -n "$ADAPTER" ] && SSH_OPTS="$SSH_OPTS -B $ADAPTER"
[ -f "$KEY" ] && SSH_OPTS="$SSH_OPTS -i $KEY"

HERE=$(cd "$(dirname "$0")" && pwd)
DECODE="$HERE/decode-fb.py"
[ -f "$DECODE" ] || { echo "FAILED: $DECODE not found" >&2; exit 1; }

WORK=$(mktemp -d "${TMPDIR:-/tmp}/zgrab.XXXXXX")
trap 'rm -rf "$WORK"' EXIT INT TERM

RAW="$WORK/screen.raw"
ERRLOG="$WORK/fbgrab.err"
PNG="${OUTPUT:-$WORK/screen.png}"

# fbgrab writes the pixels to stdout and the geometry to stderr; keep the two
# apart so the binary stream stays clean.
echo "Grabbing framebuffer from $TARGET ..." >&2
set +e
# shellcheck disable=SC2086
ssh $SSH_OPTS "$TARGET" "fbgrab${FBDEV:+ $FBDEV}" >"$RAW" 2>"$ERRLOG"
rc=$?
set -e

if [ "$rc" -ne 0 ] || [ ! -s "$RAW" ]; then
    echo "FAILED: fbgrab on $TARGET exited $rc and produced $(wc -c <"$RAW") bytes" >&2
    [ -s "$ERRLOG" ] && sed 's/^/  remote: /' "$ERRLOG" >&2
    if grep -qi 'not found' "$ERRLOG" 2>/dev/null; then
        echo "  fbgrab is not deployed by default. Cross-build" >&2
        echo "  userspace/src/fbgrab.c and ship it to /usr/local/bin/fbgrab." >&2
    fi
    exit 1
fi

# "fbgrab: 640x480 16bpp line_length=1024 offset=+0+0"
GEOM=$(sed -n 's/^fbgrab: \([0-9]\{1,\}\)x\([0-9]\{1,\}\) \([0-9]\{1,\}\)bpp.*/\1 \2 \3/p' "$ERRLOG" | head -n 1)
if [ -n "$GEOM" ]; then
    DET_W=$(echo "$GEOM" | cut -d' ' -f1)
    DET_H=$(echo "$GEOM" | cut -d' ' -f2)
    DET_B=$(echo "$GEOM" | cut -d' ' -f3)
else
    # Panel defaults for the SL-C760. Note the visible area is 640x480, not
    # 320x240 -- virtual_size reads 640,960 because of the pan buffer.
    DET_W=640; DET_H=480; DET_B=16
    echo "warning: could not parse geometry from fbgrab; assuming ${DET_W}x${DET_H} ${DET_B}bpp" >&2
fi

WIDTH="${WIDTH:-$DET_W}"
HEIGHT="${HEIGHT:-$DET_H}"
BPP="${BPP:-$DET_B}"

[ -n "$RAW_OUT" ] && cp "$RAW" "$RAW_OUT"

set -- "$DECODE" "$RAW" "$PNG" --width "$WIDTH" --height "$HEIGHT" --bpp "$BPP"
[ -n "$CROP" ] && set -- "$@" --crop "$CROP"
[ -n "$SCALE" ] && set -- "$@" --scale "$SCALE"
python3 "$@" >&2

if [ "$NO_COPY" -eq 0 ]; then
    if ! command -v wl-copy >/dev/null 2>&1; then
        echo "warning: wl-copy not installed; skipping clipboard" >&2
    elif [ -z "${WAYLAND_DISPLAY:-}" ]; then
        echo "warning: WAYLAND_DISPLAY unset; skipping clipboard" >&2
    else
        wl-copy --type image/png <"$PNG"
        echo "Copied to the Wayland clipboard as image/png." >&2
    fi
fi

[ -n "$OUTPUT" ] && echo "PNG: $OUTPUT" >&2
[ -n "$RAW_OUT" ] && echo "raw: $RAW_OUT" >&2

exit 0
