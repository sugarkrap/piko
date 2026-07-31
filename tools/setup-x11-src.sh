#!/bin/sh
set -eu

# Verifies that the X11/Matchbox submodules under userspace/src/ carry this
# project's local changes.
#
# This script used to APPLY those changes: the submodules were pinned at
# pristine upstream commits and every local edit lived here as a patch under
# modules/x11/, because committing inside a submodule would have made the
# parent repo record a SHA that does not exist on gitlab.freedesktop.org or
# git.yoctoproject.org, so a fresh clone could not fetch it.
#
# That constraint is gone. Each of those submodules now points at a fork
# under github.com/sugarkrap, and the local changes are real commits on top
# of the same upstream commit they were pinned at:
#
#   matchbox-panel           3 commits  battery /proc/apm backend, the
#                                       system-monitor meminfo parse, and
#                                       the wireless applet fixes
#   matchbox-window-manager  2 commits  GConf m4 fallback, missing includes
#   xserver                  5 commits  font-util compat m4, kdrive evdev
#                                       absolute-pointer (touchscreen),
#                                       config-file calibration + release-
#                                       noise filter, a control FIFO for
#                                       live recalibration (Pikalibrate),
#                                       and a median-of-3 sample filter
#   libX11                   2 commits  cherry-picked upstream XKBgeom.h
#                                       (1f1ca086), nls srcdir fix
#   libfontenc               1 commit   font-util compat m4
#
# So `git submodule update --init --recursive` is now sufficient, and there
# is nothing left to patch. This script stays because it is called from CI
# and from the build docs, and because a submodule silently sitting on the
# wrong commit is exactly the kind of thing that costs an afternoon: it now
# fails loudly instead of applying anything.
#
# Usage:
#   tools/setup-x11-src.sh [--force]
#
# --force is accepted and ignored, so existing callers keep working.
#
# Build order, for reference: xtrans -> libfontenc -> libXfont -> xcb chain
# -> libX11 -> libXext -> pixman/libxkbfile -> xserver -> xkbcomp/xev ->
# libmatchbox -> matchbox-window-manager / -desktop-classic / -panel /
# -common.

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
SRC="$REPO/userspace/src"

# check_marker SUBMODULE_DIR DESCRIPTION FILE PATTERN
# Confirms a local change is present by grepping for something only it adds.
check_marker() {
    dir="$1"
    what="$2"
    file="$3"
    pattern="$4"

    if [ ! -d "$dir" ]; then
        echo "FAILED: submodule not checked out: ${dir#$SRC/}" >&2
        echo "Run: git submodule update --init --recursive" >&2
        exit 1
    fi
    if [ ! -f "$dir/$file" ]; then
        echo "FAILED: ${dir#$SRC/} is missing $file" >&2
        echo "This submodule is not on its piko fork commit." >&2
        echo "Run: git submodule update --init --recursive" >&2
        exit 1
    fi
    if ! grep -q "$pattern" "$dir/$file"; then
        echo "FAILED: ${dir#$SRC/} does not carry: $what" >&2
        echo "Expected '$pattern' in $file." >&2
        echo "This submodule is probably on pristine upstream rather than" >&2
        echo "the fork. Run: git submodule update --init --recursive" >&2
        exit 1
    fi
    echo "    ok: ${dir#$SRC/} -- $what"
}

echo "==> verifying the X11/Matchbox submodules carry their local changes"

check_marker "$SRC/xserver" "font-util compat macros" \
    "m4/fontutil-compat.m4" "XORG_FONT_MACROS_VERSION"
check_marker "$SRC/xserver" "kdrive evdev absolute pointer (touchscreen)" \
    "hw/kdrive/linux/evdev.c" "EV_ABS"
check_marker "$SRC/xserver" "config-file touchscreen calibration + release-noise filter" \
    "hw/kdrive/linux/evdev.c" "EvdevLoadCalibration"
check_marker "$SRC/xserver" "Pikalibrate control FIFO" \
    "hw/kdrive/linux/linux.c" "PikalibrateFd"
check_marker "$SRC/xserver" "median-of-3 touchscreen sample filter" \
    "hw/kdrive/linux/evdev.c" "EvdevMedian3"
check_marker "$SRC/libfontenc" "font-util compat macros" \
    "m4/fontutil-compat.m4" "XORG_FONT_MACROS_VERSION"
check_marker "$SRC/libX11" "XKBgeom.h (upstream 1f1ca086)" \
    "include/X11/extensions/XKBgeom.h" "XkbGeometry"
check_marker "$SRC/matchbox-window-manager" "GConf m4 fallback" \
    "configure.ac" "AM_GCONF_SOURCE_2"
check_marker "$SRC/matchbox-panel" "battery /proc/apm backend" \
    "configure.ac" "enable_proc_apm"
check_marker "$SRC/matchbox-panel" "system-monitor meminfo parse by key" \
    "applets/mb-applet-system-monitor.c" "MemAvailable"
check_marker "$SRC/matchbox-panel" "wireless applet fixes" \
    "applets/mb-applet-wireless.c" "iface_address"

echo "==> X11 submodules ready to configure"
