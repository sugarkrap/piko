#!/bin/sh
set -eu

# Applies this project's hand-written edits to the X11 submodules under
# userspace/src/. Same philosophy as tools/setup-kernel-src.sh: the
# submodules stay pinned at pristine upstream tags, and every local
# change lives here as tracked source under modules/x11/.
#
# Why this exists: these edits used to live ONLY as uncommitted changes
# inside the submodule working trees. That works on one machine and is
# silently lost on a fresh clone -- and committing them inside a
# submodule isn't a fix either, because the parent repo would then record
# a SHA that doesn't exist on gitlab.freedesktop.org, so the clone
# couldn't fetch it.
#
# Usage:
#   tools/setup-x11-src.sh [--force]
#
# Idempotent: each hunk is checked with `git apply --check` first and
# skipped if it's already applied, so this is safe to re-run. --force
# re-copies the whole-file drop-ins even if they're already present.
#
# Run this after `git submodule update --init` and before configuring
# any of the X packages (see docs/archive/HANDOFF-2026-07-28-X11-XFBDEV.md
# for the build order: xtrans -> libfontenc -> libXfont -> xcb chain ->
# libX11 -> libXext -> pixman/libxkbfile -> xserver -> xkbcomp/xev ->
# matchbox-window-manager).

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
SRC="$REPO/userspace/src"
PATCHES="$REPO/modules/x11"

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

# copy_in SRC_FILE DEST_FILE -- whole-file drop-ins (new files upstream
# doesn't have at these tags, so there's nothing to patch against).
copy_in() {
    src="$1"
    dest="$2"
    if [ ! -f "$src" ]; then
        echo "tools/setup-x11-src.sh: missing tracked input: $src" >&2
        exit 1
    fi
    if [ -f "$dest" ] && [ "$FORCE" -eq 0 ] && cmp -s "$src" "$dest"; then
        echo "    already present: ${dest#$SRC/}"
        return 0
    fi
    mkdir -p "$(dirname "$dest")"
    cp "$src" "$dest"
    echo "    installed: ${dest#$SRC/}"
}

# apply_patch SUBMODULE_DIR PATCH_FILE
apply_patch() {
    dir="$1"
    patch_file="$2"
    name="$(basename "$patch_file")"

    if [ ! -d "$dir" ]; then
        echo "tools/setup-x11-src.sh: submodule not checked out: $dir" >&2
        echo "Run: git submodule update --init --recursive" >&2
        exit 1
    fi
    if [ ! -f "$patch_file" ]; then
        echo "tools/setup-x11-src.sh: missing tracked patch: $patch_file" >&2
        exit 1
    fi

    # Already applied? (reverse-applies cleanly => it's in there.)
    if git -C "$dir" apply --reverse --check "$patch_file" 2>/dev/null; then
        echo "    already applied: $name"
        return 0
    fi
    if ! git -C "$dir" apply --check "$patch_file" 2>/dev/null; then
        echo "tools/setup-x11-src.sh: $name applies cleanly neither" >&2
        echo "forwards nor in reverse against $dir." >&2
        echo "The submodule is probably not at its pinned tag, or the tree" >&2
        echo "has been hand-edited. Check 'git -C $dir status'." >&2
        exit 1
    fi
    git -C "$dir" apply "$patch_file"
    echo "    applied: $name"
}

echo "==> xserver: font-util compat macros"
# Real font-util isn't installed here, and xserver's configure.ac needs
# XORG_FONT_MACROS_VERSION/XORG_FONTROOTDIR/XORG_FONTSUBDIR. Also fixes
# the ${datarootdir} that otherwise ends up baked verbatim into the
# compiled-in default font path (AC_DEFINE_DIR only eval's twice).
copy_in "$PATCHES/fontutil-compat.m4" "$SRC/xserver/m4/fontutil-compat.m4"

echo "==> xserver: kdrive evdev absolute-pointer (touchscreen) support"
# Stock kdrive evdev drives relative pointers only -- EV_ABS was debug
# output with no event enqueued, and BTN_TOUCH (0x14a) fell outside the
# BTN_MOUSE..BTN_JOYSTICK range its button handler accepts. Net effect:
# the ads7846 touchscreen fed events to X and the cursor never moved.
# See docs/HOWTO-X11-TOUCHSCREEN.md (incl. how to re-run calibration).
apply_patch "$SRC/xserver" "$PATCHES/xserver-kdrive-evdev-absolute.patch"

echo "==> libfontenc: same font-util compat macros"
copy_in "$PATCHES/fontutil-compat.m4" "$SRC/libfontenc/m4/fontutil-compat.m4"

echo "==> libX11: XKBgeom.h + nls test fix"
# XKBgeom.h was dropped from xorgproto in 2019 ("doesn't belong here,
# refers to libX11 types") and libX11 only started shipping its own copy
# after libX11-1.4.4, which is what this submodule is pinned to. Taken
# verbatim from libX11's own later history (commit 1f1ca086).
copy_in "$PATCHES/XKBgeom.h" "$SRC/libX11/include/X11/extensions/XKBgeom.h"
apply_patch "$SRC/libX11" "$PATCHES/libX11-nls-tests-srcdir.patch"

echo "==> matchbox-window-manager: build fixes"
apply_patch "$SRC/matchbox-window-manager" "$PATCHES/matchbox-gconf-m4-fallback.patch"
apply_patch "$SRC/matchbox-window-manager" "$PATCHES/matchbox-missing-includes.patch"

echo "==> matchbox-panel: battery applet via /proc/apm"
# mb-applet-battery has two upstream backends and we can use neither:
# HAVE_APM_H wants apm.h + -lapm from Debian's apmd, which we do not
# cross-build, and USE_ACPI_LINUX reads /proc/acpi, which this board does
# not have. So configure silently dropped the applet from bin_PROGRAMS
# and the panel had no battery indicator to load.
#
# The kernel is built with CONFIG_APM_EMULATION=y + CONFIG_SHARPSL_PM=y,
# so /proc/apm carries real charge state from the Sharp PM driver. This
# adds --enable-proc-apm, which parses that file directly and needs no
# new library. It also fixes the .desktop install, which upstream gates
# on WANT_APM alone -- so the ACPI backend never installed a menu entry
# either.
apply_patch "$SRC/matchbox-panel" "$PATCHES/matchbox-panel-battery-proc-apm.patch"

echo "==> matchbox-panel: system-monitor /proc/meminfo parse"
# mb-applet-system-monitor read /proc/meminfo by FIELD POSITION, matching
# the field order of 2.6.0. The kernel has inserted fields since --
# MemAvailable landed third in 3.14 -- so every value after MemFree came
# out of the wrong line and the unsigned "used" arithmetic underflowed
# into a huge number. Symptom: a correct CPU bar next to a memory bar
# showing a nonsense percentage. Now keyed off the labels.
apply_patch "$SRC/matchbox-panel" "$PATCHES/matchbox-panel-system-monitor-meminfo.patch"

echo "==> X11 submodules ready to configure"
