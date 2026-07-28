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
#   tools/setup-kernel-src.sh [--force]
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
#       missing from this repo, oldconfig failing, etc.)

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
    echo "tools/setup-kernel-src.sh: $KERNEL_DIR doesn't look like a kernel tree (no Makefile)" >&2
    exit 1
fi

# copy_in SRC DEST_REL_TO_KERNEL_DIR
copy_in() {
    src="$1"
    dest="$KERNEL_DIR/$2"
    if [ ! -f "$src" ]; then
        echo "tools/setup-kernel-src.sh: missing tracked input: $src" >&2
        exit 1
    fi
    mkdir -p "$(dirname "$dest")"
    cp "$src" "$dest"
}

echo "==> applying Corgi board files (see README.md 'What corgi_patched.c changes')"
copy_in "$REPO/corgi_patched.c"            arch/arm/mach-pxa/corgi.c
copy_in "$REPO/modules/corgi_pm_patched.c" arch/arm/mach-pxa/corgi_pm.c
copy_in "$REPO/corgi.h"                    arch/arm/mach-pxa/corgi.h

echo "==> applying reference current-driver snapshots"
copy_in "$REPO/drivers/spitz.c"      arch/arm/mach-pxa/spitz.c
copy_in "$REPO/drivers/spitz_pm.c"   arch/arm/mach-pxa/spitz_pm.c
copy_in "$REPO/drivers/sharpsl_pm.c" arch/arm/mach-pxa/sharpsl_pm.c
copy_in "$REPO/drivers/pxa25x_udc.c" drivers/usb/gadget/udc/pxa25x_udc.c
# pxa25x_udc.c was already patched to fetch the D+ pullup line via
# devm_gpiod_get_index_optional(..., "pullup", ...) instead of the old
# platform_data mach->gpio_pullup path, but the matching struct field
# (pullup_gpio) was never added to its header -- caught by a real CI
# build failing with "struct pxa25x_udc has no member named
# pullup_gpio". See the PATCHED comment in drivers/pxa25x_udc.h itself.
copy_in "$REPO/drivers/pxa25x_udc.h" drivers/usb/gadget/udc/pxa25x_udc.h

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
for f in "$REPO"/modules/hostap/hostap*.c "$REPO"/modules/hostap/hostap*.h; do
    copy_in "$f" "$HOSTAP_DEST/$(basename "$f")"
done
# hostap's own Kconfig/Makefile -- the whole hostap/ directory was removed
# from mainline, so unlike mach-pxa/net/wireless/crypto below (which are
# full working copies of files that still exist upstream), there is no
# pristine drivers/net/wireless/intersil/hostap/{Kconfig,Makefile} to
# replace at all. Without these, CONFIG_HOSTAP/CONFIG_HOSTAP_CS in the
# tracked .config have no matching symbol anywhere in the tree, so
# oldconfig silently drops them instead of erroring -- hostap.ko then
# never gets built, and nothing surfaces until packaging looks for a file
# that was never produced (hit exactly this in CI).
copy_in "$REPO/modules/hostap/Kconfig"  "$HOSTAP_DEST/Kconfig"
copy_in "$REPO/modules/hostap/Makefile" "$HOSTAP_DEST/Makefile"
for f in "$REPO"/modules/hostap/lib80211*.c "$REPO"/modules/hostap/lib80211*.h; do
    copy_in "$f" "net/wireless/$(basename "$f")"
done
copy_in "$REPO/modules/hostap/michael_mic.c" crypto/michael_mic.c

# Kconfig/Makefile wiring: MACH_CORGI/SHEPHERD/HUSKY + PXA_SHARP_C7xx
# (arch/arm/mach-pxa), lib80211 + its crypt helpers (net/wireless), and
# CRYPTO_MICHAEL_MIC (crypto). These are full working copies pulled
# directly from an already-built, confirmed-working kernel-src (the one
# that produced zImage-corgi-7.1.4) -- not hand-derived diffs -- same
# trust model as every other full-file copy above and modules/hostap/
# {Kconfig,Makefile}'s existing precedent.
echo "==> applying mach-pxa/wireless/crypto Kconfig+Makefile wiring"
copy_in "$REPO/modules/mach-pxa/Kconfig"  arch/arm/mach-pxa/Kconfig
copy_in "$REPO/modules/mach-pxa/Makefile" arch/arm/mach-pxa/Makefile
copy_in "$REPO/modules/wireless/Kconfig"  net/wireless/Kconfig
copy_in "$REPO/modules/wireless/Makefile" net/wireless/Makefile
copy_in "$REPO/modules/crypto/Kconfig"    crypto/Kconfig
copy_in "$REPO/modules/crypto/Makefile"   crypto/Makefile

# The Corgi/Husky ASoC sound machine driver (SND_PXA2XX_SOC_CORGI, depends
# on PXA_SHARP_C7xx + I2C, same board-removal history as corgi.c/corgi_pm.c/
# w100fb.c above) -- also never existed in this repo until it was pulled
# directly from the same already-working kernel-src.
echo "==> applying the Corgi ASoC sound driver"
copy_in "$REPO/modules/sound-pxa/corgi.c"   sound/soc/pxa/corgi.c
copy_in "$REPO/modules/sound-pxa/Kconfig"   sound/soc/pxa/Kconfig
copy_in "$REPO/modules/sound-pxa/Makefile"  sound/soc/pxa/Makefile

echo "==> applying kernel.config-corgi-$KERNEL_VERSION"
copy_in "$REPO/kernel.config-corgi-$KERNEL_VERSION" .config

echo "==> oldconfig (non-interactive, accepting defaults for anything new)"
( cd "$KERNEL_DIR" && yes "" | make ARCH=arm oldconfig >/tmp/piko-oldconfig.log 2>&1 ) || {
    echo "tools/setup-kernel-src.sh: oldconfig failed, see /tmp/piko-oldconfig.log" >&2
    exit 1
}

touch "$MARKER"
echo "==> $KERNEL_DIR ready to build"
