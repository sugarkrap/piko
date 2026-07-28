#!/bin/sh
set -eu

# Reconstructs kernel-src/linux-$KERNEL_VERSION/ from scratch: downloads a
# pristine kernel.org tarball, then applies every hand-patched file this
# project tracks in git. This is the automated version of the manual,
# "no automated apply-script yet -- do this by hand, carefully, file by
# file" procedure in docs/HANDOFF.md -- read that doc for the full
# rationale behind each file. kernel-src/ itself stays gitignored (it's a
# multi-hundred-MB build tree, not source, see .gitignore) -- this script
# is what makes it reproducible without vendoring it.
#
# Usage:
#   flash/setup-kernel-src.sh [--force]
#
# --force re-applies everything even if kernel-src/ already looks patched
# (the default is to skip all of this and exit 0 if a previous run's
# marker file is present, so this script is cheap to call unconditionally
# at the start of a build).
#
# Env overrides:
#   KERNEL_VERSION   default 7.1.4 (matches the tracked .config/zImage)
#   KERNEL_SRC_DIR   default <repo>/kernel-src (gitignored)
#
# Exit codes:
#   0   kernel-src/linux-$KERNEL_VERSION is ready to build
#   1   a hard failure (download, extraction, an expected input file
#       missing from this repo, etc.)
#   2   everything mechanical succeeded, but the Kconfig/Makefile wiring
#       for MACH_CORGI/SHEPHERD/HUSKY and the hostap net/wireless+crypto
#       dependencies could not be applied because patches/*.patch aren't
#       present yet -- see patches/README.md. Callers that only want a
#       best-effort/rootfs-only fallback (e.g. CI, until those patches are
#       committed) should treat this exit code as non-fatal.

REPO="$(cd "$(dirname "$0")/.." && pwd)"
KERNEL_VERSION="${KERNEL_VERSION:-7.1.4}"
KERNEL_SRC_DIR="${KERNEL_SRC_DIR:-$REPO/kernel-src}"
KERNEL_DIR="$KERNEL_SRC_DIR/linux-$KERNEL_VERSION"
TARBALL="$KERNEL_SRC_DIR/linux-$KERNEL_VERSION.tar.xz"
KERNEL_URL="https://cdn.kernel.org/pub/linux/kernel/v${KERNEL_VERSION%%.*}.x/linux-$KERNEL_VERSION.tar.xz"
MARKER="$KERNEL_DIR/.piko-patched"

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

if [ "$FORCE" -eq 0 ] && [ -f "$MARKER" ]; then
    echo "==> $KERNEL_DIR already patched (marker present), nothing to do"
    echo "    (pass --force to redo it)"
    exit 0
fi

mkdir -p "$KERNEL_SRC_DIR"

if [ ! -f "$TARBALL" ]; then
    echo "==> downloading $KERNEL_URL"
    curl -fL -o "$TARBALL.partial" "$KERNEL_URL"
    mv "$TARBALL.partial" "$TARBALL"
else
    echo "==> reusing cached $TARBALL"
fi

if [ ! -d "$KERNEL_DIR" ]; then
    echo "==> extracting to $KERNEL_SRC_DIR"
    tar xf "$TARBALL" -C "$KERNEL_SRC_DIR"
fi

if [ ! -f "$KERNEL_DIR/Makefile" ]; then
    echo "flash/setup-kernel-src.sh: $KERNEL_DIR doesn't look like a kernel tree (no Makefile)" >&2
    exit 1
fi

# copy_in SRC DEST_REL_TO_KERNEL_DIR
copy_in() {
    src="$1"
    dest="$KERNEL_DIR/$2"
    if [ ! -f "$src" ]; then
        echo "flash/setup-kernel-src.sh: missing tracked input: $src" >&2
        exit 1
    fi
    mkdir -p "$(dirname "$dest")"
    cp "$src" "$dest"
}

echo "==> applying Corgi board files (see README.md 'What corgi_patched.c changes')"
copy_in "$REPO/corgi_patched.c"            arch/arm/mach-pxa/corgi.c
copy_in "$REPO/modules/corgi_pm_patched.c" arch/arm/mach-pxa/corgi_pm.c
copy_in "$REPO/corgi.h"                    arch/arm/mach-pxa/corgi.h

echo "==> applying the W100 (Imageon) display driver"
copy_in "$REPO/modules/w100/w100fb_patched.c"  drivers/video/fbdev/w100fb.c
copy_in "$REPO/modules/w100/w100fb_private.h" drivers/video/fbdev/w100fb.h
copy_in "$REPO/modules/w100/w100fb.h"          include/video/w100fb.h

echo "==> applying the sharpsl NAND driver"
copy_in "$REPO/modules/nand/sharpsl_nand_patched.c" drivers/mtd/nand/raw/sharpsl.c
copy_in "$REPO/modules/nand/sharpslpart.c"          drivers/mtd/parsers/sharpslpart.c
copy_in "$REPO/modules/nand/sharpsl.h"              include/linux/mtd/sharpsl.h

echo "==> applying hostap_cs (PCMCIA WiFi) + lib80211 + michael_mic"
HOSTAP_DEST=drivers/net/wireless/intersil/hostap
for f in "$REPO"/modules/hostap/hostap*.c "$REPO"/modules/hostap/hostap*.h \
         "$REPO/modules/hostap/Kconfig" "$REPO/modules/hostap/Makefile"; do
    copy_in "$f" "$HOSTAP_DEST/$(basename "$f")"
done
for f in "$REPO"/modules/hostap/lib80211*.c "$REPO"/modules/hostap/lib80211*.h; do
    copy_in "$f" "net/wireless/$(basename "$f")"
done
copy_in "$REPO/modules/hostap/michael_mic.c" crypto/michael_mic.c

# The pieces above are all full-file replacements/additions -- unambiguous,
# nothing upstream to merge against. What's left is *incremental* edits to
# upstream files that still exist (arch/arm/mach-pxa/{Kconfig,Makefile} for
# the MACH_CORGI/SHEPHERD/HUSKY/PXA_SHARP_C7xx board entry, net/wireless/
# {Kconfig,Makefile} and crypto/{Kconfig,Makefile} for the lib80211/
# michael_mic re-additions) -- see README.md's "hostap_cs ... ported" section
# for exactly what each needs. Those are captured as patches/*.patch, not
# full-file copies, since only a few lines change in files this project
# doesn't otherwise own.
KCONFIG_INCOMPLETE=0
apply_patch_if_present() {
    patch_file="$1"
    if [ ! -f "$patch_file" ]; then
        KCONFIG_INCOMPLETE=1
        return
    fi
    echo "==> applying $(basename "$patch_file")"
    patch -p1 -d "$KERNEL_DIR" < "$patch_file"
}

apply_patch_if_present "$REPO/patches/mach-pxa-corgi-kconfig.patch"
apply_patch_if_present "$REPO/patches/wireless-lib80211-kconfig.patch"

if [ "$KCONFIG_INCOMPLETE" -eq 1 ]; then
    echo "" >&2
    echo "flash/setup-kernel-src.sh: Kconfig/Makefile wiring patches are missing (see patches/README.md)." >&2
    echo "  Full-file sources (board files, w100, nand, hostap) are applied and correct." >&2
    echo "  Still needed by hand, once per new kernel-src reconstruction, until the" >&2
    echo "  patch files above are generated from a known-working local kernel-src tree:" >&2
    echo "    - arch/arm/mach-pxa/{Kconfig,Makefile}: MACH_CORGI/SHEPHERD/HUSKY, PXA_SHARP_C7xx" >&2
    echo "    - net/wireless/{Kconfig,Makefile}: lib80211 + its crypt helpers" >&2
    echo "    - crypto/{Kconfig,Makefile}: CRYPTO_MICHAEL_MIC" >&2
    echo "  (docs/HANDOFF.md steps 2/5, README.md 'hostap_cs ... ported' section)" >&2
    echo "$KERNEL_DIR is otherwise ready; oldconfig/build will fail or silently" >&2
    echo "produce a kernel missing this board's machine descriptor without them." >&2
    exit 2
fi

echo "==> applying kernel.config-corgi-$KERNEL_VERSION"
copy_in "$REPO/kernel.config-corgi-$KERNEL_VERSION" .config

echo "==> oldconfig (non-interactive, accepting defaults for anything new)"
( cd "$KERNEL_DIR" && yes "" | make ARCH=arm oldconfig >/tmp/piko-oldconfig.log 2>&1 ) || {
    echo "flash/setup-kernel-src.sh: oldconfig failed, see /tmp/piko-oldconfig.log" >&2
    exit 1
}

touch "$MARKER"
echo "==> $KERNEL_DIR ready to build"
