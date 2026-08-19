#!/bin/sh
set -eu

NAME=""
VERSION=""
ROOT=""
ARCH="piko"
DESC=""
DEPENDS=""
POSTINST=""
OUTDIR="."
MAINTAINER="${IPK_MAINTAINER:-piko <root@zaurus>}"

while [ $# -gt 0 ]; do
    case "$1" in
        --name)     NAME="${2:?--name needs a value}"; shift 2 ;;
        --version)  VERSION="${2:?--version needs a value}"; shift 2 ;;
        --root)     ROOT="${2:?--root needs a value}"; shift 2 ;;
        --arch)     ARCH="${2:?--arch needs a value}"; shift 2 ;;
        --desc)     DESC="${2:?--desc needs a value}"; shift 2 ;;
        --depends)  DEPENDS="${2:?--depends needs a value}"; shift 2 ;;
        --postinst) POSTINST="${2:?--postinst needs a value}"; shift 2 ;;
        --out)      OUTDIR="${2:?--out needs a value}"; shift 2 ;;
        -h|--help)  sed -n '3,30p' "$0"; exit 0 ;;
        *) echo "FAILED: unknown option: $1" >&2; exit 1 ;;
    esac
done

[ -n "$NAME" ]    || { echo "FAILED: --name is required" >&2; exit 1; }
[ -n "$VERSION" ] || { echo "FAILED: --version is required" >&2; exit 1; }
[ -n "$ROOT" ]    || { echo "FAILED: --root is required" >&2; exit 1; }
[ -d "$ROOT" ]    || { echo "FAILED: --root $ROOT is not a directory" >&2; exit 1; }
[ -z "$POSTINST" ] || [ -f "$POSTINST" ] || { echo "FAILED: --postinst $POSTINST is not a file" >&2; exit 1; }

case "$ARCH" in
    piko|all) ;;
    arm)
        echo "FAILED: --arch arm is refused." >&2
        echo "\"arm\" means Sharp-era OABI/glibc packages, which this device" >&2
        echo "rejects on purpose. Build for 'piko' (compiled code) or 'all'" >&2
        echo "(scripts and data). See the header of this script." >&2
        exit 1
        ;;
    *)
        echo "FAILED: --arch $ARCH is not one this device accepts." >&2
        echo "/etc/opkg/opkg.conf lists only 'all' and 'piko'." >&2
        exit 1
        ;;
esac

command -v ar >/dev/null 2>&1 || { echo "FAILED: 'ar' not found (install binutils)" >&2; exit 1; }

mkdir -p "$OUTDIR"
OUTDIR="$(CDPATH= cd -- "$OUTDIR" && pwd)"
ROOT="$(CDPATH= cd -- "$ROOT" && pwd)"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT INT TERM

if [ -d "$ROOT/home/QtPalmtop" ]; then
    echo "FAILED: $ROOT contains /home/QtPalmtop." >&2
    echo "That is the Sharp/Qtopia layout. This system has no Qtopia." >&2
    exit 1
fi

if [ ! -d "$ROOT/usr/share/applications" ]; then
    echo "note: no usr/share/applications/ in the payload, so this package"
    echo "      will not appear on the desktop (fine for libraries and"
    echo "      command-line tools)."
fi

mkdir -p "$WORK/control"
{
    echo "Package: $NAME"
    echo "Version: $VERSION"
    echo "Architecture: $ARCH"
    echo "Maintainer: $MAINTAINER"
    [ -n "$DEPENDS" ] && echo "Depends: $DEPENDS"
    echo "Section: misc"
    echo "Priority: optional"
    echo "Description: ${DESC:-$NAME}"
} > "$WORK/control/control"

if [ -n "$POSTINST" ]; then
    cp "$POSTINST" "$WORK/control/postinst"
    chmod 755 "$WORK/control/postinst"
fi

( cd "$WORK/control" && tar --owner=0 --group=0 --format=gnu -czf "$WORK/control.tar.gz" . )
( cd "$ROOT"         && tar --owner=0 --group=0 --format=gnu -czf "$WORK/data.tar.gz" . )

echo "2.0" > "$WORK/debian-binary"

OUT="$OUTDIR/${NAME}_${VERSION}_${ARCH}.ipk"
rm -f "$OUT"
( cd "$WORK" && ar rc "$OUT" debian-binary control.tar.gz data.tar.gz )

echo "==> $OUT"
echo "    arch=$ARCH  version=$VERSION  size=$(wc -c < "$OUT") bytes"
echo
echo "Install it on the device with:"
echo "    pkgadd /tmp/$(basename "$OUT")           # into the root image"
echo "    pkgadd /tmp/$(basename "$OUT") card      # onto the SD card"
