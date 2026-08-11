#!/bin/sh
set -eu

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
SRC="$REPO/userspace/src"

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
check_marker "$SRC/matchbox-panel" "system-monitor swap bar" \
    "applets/mb-applet-system-monitor.c" "CARD_SWAP_PREFIX"
check_marker "$SRC/matchbox-panel" "wireless applet fixes" \
    "applets/mb-applet-wireless.c" "iface_address"

echo "==> X11 submodules ready to configure"
