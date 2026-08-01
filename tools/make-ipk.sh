#!/bin/sh
set -eu

# Builds an .ipk package this device will accept.
#
# Usage:
#   tools/make-ipk.sh --name NAME --version VER --root DIR [options]
#
#   --name NAME        package name (required)
#   --version VER      package version, e.g. 1.0 (required)
#   --root DIR         directory whose contents become the package's
#                      filesystem: DIR/usr/bin/foo lands at /usr/bin/foo
#   --arch ARCH        piko (default) or all
#   --desc TEXT        one-line description
#   --depends LIST     comma-separated package names
#   --out DIR          where to write the .ipk (default: current directory)
#
# Example -- package a binary plus its desktop entry:
#
#   mkdir -p stage/usr/bin stage/usr/share/applications
#   cp myapp stage/usr/bin/
#   cp myapp.desktop stage/usr/share/applications/
#   tools/make-ipk.sh --name myapp --version 1.0 --root stage
#
#
# ARCHITECTURE -- why "piko" and not "arm"
# ========================================
# The device's /etc/opkg/opkg.conf lists exactly two acceptable
# architectures, "all" and "piko", and deliberately does NOT list "arm".
# That is the retro-compatibility gate: Sharp-era Zaurus packages declare
# "arm", meaning ARM OABI against glibc 2.2.2 with Qtopia paths, and
# nothing in this rootfs can load them. Refusing the whole architecture
# is how they are kept out until translation is actually implemented.
#
# So a package built here must say "piko" or it will be refused right
# alongside them -- which is the intended behaviour, not a bug to work
# around. Do not "fix" a refused package by relabelling it "arm"; that
# would mean adding "arm" back to the device's accepted list, which
# re-admits every genuinely incompatible Sharp package at the same time.
#
# Use "all" only for packages that contain no compiled code at all --
# scripts, icons, themes, data.
#
#
# ON THE PACKAGE FORMAT
# =====================
# Written in the modern layout: an `ar` archive of debian-binary,
# control.tar.gz and data.tar.gz. opkg also accepts the pre-2005 layout
# (the same three members inside a plain tar.gz), and reads both without
# being told which is which -- so the format is NOT what distinguishes an
# acceptable package from a Sharp one. Only the Architecture field is.

NAME=""
VERSION=""
ROOT=""
ARCH="piko"
DESC=""
DEPENDS=""
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
        --out)      OUTDIR="${2:?--out needs a value}"; shift 2 ;;
        -h|--help)  sed -n '3,30p' "$0"; exit 0 ;;
        *) echo "FAILED: unknown option: $1" >&2; exit 1 ;;
    esac
done

[ -n "$NAME" ]    || { echo "FAILED: --name is required" >&2; exit 1; }
[ -n "$VERSION" ] || { echo "FAILED: --version is required" >&2; exit 1; }
[ -n "$ROOT" ]    || { echo "FAILED: --root is required" >&2; exit 1; }
[ -d "$ROOT" ]    || { echo "FAILED: --root $ROOT is not a directory" >&2; exit 1; }

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

# --- sanity checks on the payload ------------------------------------
# Catching these here is worth it: on the device the same mistakes show
# up as an application that installs cleanly and then does nothing, with
# no message anywhere.
if [ -d "$ROOT/home/QtPalmtop" ]; then
    echo "FAILED: $ROOT contains /home/QtPalmtop." >&2
    echo "That is the Sharp/Qtopia layout. This system has no Qtopia." >&2
    exit 1
fi

# A .desktop file is what puts an application on the Matchbox desktop.
# Not an error -- plenty of packages are libraries or command-line tools
# -- but silently getting no icon is a surprise worth pre-empting.
if [ ! -d "$ROOT/usr/share/applications" ]; then
    echo "note: no usr/share/applications/ in the payload, so this package"
    echo "      will not appear on the desktop (fine for libraries and"
    echo "      command-line tools)."
fi

# --- control file -----------------------------------------------------
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

# --- assemble ---------------------------------------------------------
# --owner/--group 0 (rather than whatever the build user happens to be):
# the device installs as root and has a deliberately minimal /etc/group,
# so a package carrying an unknown numeric owner is asking for trouble.
# --format=gnu keeps the members readable by the old tar layout too.
( cd "$WORK/control" && tar --owner=0 --group=0 --format=gnu -czf "$WORK/control.tar.gz" ./control )
( cd "$ROOT"         && tar --owner=0 --group=0 --format=gnu -czf "$WORK/data.tar.gz" . )

echo "2.0" > "$WORK/debian-binary"

OUT="$OUTDIR/${NAME}_${VERSION}_${ARCH}.ipk"
rm -f "$OUT"
# Member ORDER matters: debian-binary must come first, as the format
# requires, and opkg reads the members in sequence out of the ar stream.
( cd "$WORK" && ar rc "$OUT" debian-binary control.tar.gz data.tar.gz )

echo "==> $OUT"
echo "    arch=$ARCH  version=$VERSION  size=$(wc -c < "$OUT") bytes"
echo
echo "Install it on the device with:"
echo "    pkgadd /tmp/$(basename "$OUT")           # into the NAND root"
echo "    pkgadd /tmp/$(basename "$OUT") card      # onto the SD card"
