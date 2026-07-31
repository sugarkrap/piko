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
#   TOOLCHAIN_BIN_DIR optional compiler bin dir prepended to PATH
#   CROSS_COMPILE    optional explicit compiler prefix (e.g. arm-linux-gnueabi-)
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
TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-/home/makaron/Code/dosbox-armv5-zaurus/buildroot/output/host/bin}"

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
copy_in "$REPO/modules/mach-pxa/corgi_patched.c"    arch/arm/mach-pxa/corgi.c
copy_in "$REPO/modules/mach-pxa/corgi_pm_patched.c" arch/arm/mach-pxa/corgi_pm.c
copy_in "$REPO/modules/mach-pxa/corgi.h"            arch/arm/mach-pxa/corgi.h

# LED blink-code boot checkpoints (2026-07-29 mtd1 boot investigation,
# see docs/DEADLETTER-LED-MARKERS.md). head.S carries the piko_blink
# routine plus checkpoints 1-2 (pre-MMU, raw physical writes) and the
# identity-mapped GPIO/SCOOP sections the later, post-MMU checkpoints
# depend on. Each checkpoint blinks BOTH LEDs a distinct COUNT; the
# highest complete group observed says how far boot got. Diagnostic only.
copy_in "$REPO/modules/arch-arm/head_patched.S"      arch/arm/kernel/head.S

# Same investigation: blink-code checkpoints 5-6 (paging_init() and
# devicemaps_init() entry), plus the fix that makes every post-MMU
# checkpoint viable at all -- prepare_page_table() runs at the very start
# of paging_init() and otherwise wipes every low virtual-address mapping,
# including the diagnostic GPIO/SCOOP sections head.S adds above.
copy_in "$REPO/modules/arch-arm/mmu_patched.c"        arch/arm/mm/mmu.c

# Same investigation: blink-code checkpoints 3 and 4 (setup_arch() entry,
# and just after setup_machine_tags()/mdesc->fixup returns).
copy_in "$REPO/modules/arch-arm/setup_patched.c"       arch/arm/kernel/setup.c

# Same investigation: blink-code checkpoints 6-8 live here (machine-number
# lookup, mdesc->fixup i.e. fixup_corgi, and ATAG parsing), plus the
# fast-forever blink that makes dump_machine_table()'s silent `while(true)`
# hang visible when the bootloader's machine number matches nothing.
copy_in "$REPO/modules/arch-arm/atags_parse_patched.c" arch/arm/kernel/atags_parse.c

# Same investigation: blink-code checkpoints through start_kernel() --
# this bootstrap kernel has no console at all, so LED counts are the only
# visibility into how far it gets.
copy_in "$REPO/modules/arch-arm/main_patched.c"        init/main.c

# Same investigation: the boot reaches timekeeping_init() but never returns
# from time_init(), so these two carry a fine-grained bisect of the PXA
# timer bring-up (pxa_timer_init -> pxa_timer_nodt_init -> common_init).
copy_in "$REPO/modules/mach-pxa/generic_patched.c"    arch/arm/mach-pxa/generic.c
copy_in "$REPO/modules/clk-pxa/timer_pxa_patched.c"   drivers/clocksource/timer-pxa.c
copy_in "$REPO/modules/clk-pxa/clk_pxa25x_patched.c"  drivers/clk/pxa/clk-pxa25x.c
copy_in "$REPO/modules/clk-pxa/clk_pxa_patched.c"     drivers/clk/pxa/clk-pxa.c

# pxa25x.c adds the "pxa2xx-i2s" rx/tx DMA slave-map entries that mainline
# only ever defined for pxa27x -- without them dma_request_slave_channel()
# returns NULL and ASoC fails PCM open with -ENXIO on this board. See the
# PATCHED comment in the file itself.
copy_in "$REPO/modules/mach-pxa/pxa25x_patched.c"     arch/arm/mach-pxa/pxa25x.c

# spi-pxa2xx-platform.c requested the SSP port twice (once in
# pxa2xx_spi_init_pdata(), again in probe()); pxa_ssp_request() only
# matches a port with use_count == 0, so the second one always failed and
# probe fell back to a zeroed ssp_device with irq == 0 ("cannot get IRQ 0",
# probe fails -EINVAL). That took down the whole Corgi SPI bus, and with it
# ads7846 (touchscreen), corgi-lcd (backlight) and max1111 (battery ADC).
# See the PATCHED comments in the file itself.
echo "==> applying the pxa2xx-spi double-SSP-request fix"
copy_in "$REPO/modules/spi/spi_pxa2xx_platform_patched.c" drivers/spi/spi-pxa2xx-platform.c

echo "==> applying reference current-driver snapshots"
copy_in "$REPO/drivers/spitz.c"      arch/arm/mach-pxa/spitz.c
copy_in "$REPO/drivers/spitz_pm.c"   arch/arm/mach-pxa/spitz_pm.c
copy_in "$REPO/drivers/sharpsl_pm.c" arch/arm/mach-pxa/sharpsl_pm.c
copy_in "$REPO/drivers/pxa25x_udc.c" drivers/usb/gadget/udc/pxa25x_udc.c
# pxa25x_udc.c fetches the D+ pullup line via
# devm_gpiod_get_index_optional(..., "pullup", ...) instead of the old
# platform_data mach->gpio_pullup path, but the matching struct field
# (pullup_gpio) doesn't exist in any pristine header -- caught by a real
# CI build failing with "struct pxa25x_udc has no member named
# pullup_gpio". See the PATCHED comment in drivers/pxa25x_udc.h itself.
copy_in "$REPO/drivers/pxa25x_udc.h" drivers/usb/gadget/udc/pxa25x_udc.h

echo "==> applying the W100 (Imageon) display driver"
copy_in "$REPO/modules/w100/w100fb_patched.c"  drivers/video/fbdev/w100fb.c
copy_in "$REPO/modules/w100/w100fb_private.h" drivers/video/fbdev/w100fb.h
copy_in "$REPO/modules/w100/w100fb.h"          include/video/w100fb.h

# Sharp panel VCOM/phase: writable comadj + phadadj params, so the
# per-unit calibration lost across our kexec can be swept live instead of
# rebuilding per guess. See the block comment in the file.
echo "==> applying the Corgi LCD (VCOM/phase override) driver"
copy_in "$REPO/modules/lcd/corgi_lcd_patched.c" drivers/video/backlight/corgi_lcd.c

# Wire the W100 driver into the fbdev Kconfig/Makefile.
#
# Without this CONFIG_FB_W100 does not exist as a symbol at all, so
# w100fb.c is copied in but NEVER BUILT -- which is why the bootstrap
# kernel has had no console for this entire project (found 2026-07-29).
#
# The repo also carries full-file snapshots (modules/w100/Kconfig_fbdev,
# Makefile_fbdev) but those are from an older tree; appending the two
# stanzas to the pristine files is version-proof and far smaller. Both
# files end at top level, so a plain append lands outside any menu/if.
if ! grep -q "FB_W100" "$KERNEL_DIR/drivers/video/fbdev/Kconfig"; then
    cat >> "$KERNEL_DIR/drivers/video/fbdev/Kconfig" <<'W100_KCONFIG'

config FB_W100
	tristate "W100 frame buffer support"
	depends on FB && HAS_IOMEM && (ARCH_PXA || COMPILE_TEST)
	select FB_CFB_FILLRECT
	select FB_CFB_COPYAREA
	select FB_CFB_IMAGEBLIT
	help
	  Frame buffer driver for the w100 as found on the Sharp SL-Cxx series.
	  It can also drive the w3220 chip found on iPAQ hx4700.
W100_KCONFIG
fi
if ! grep -q "FB_W100" "$KERNEL_DIR/drivers/video/fbdev/Makefile"; then
    printf 'obj-$(CONFIG_FB_W100)\t\t  += w100fb.o\n' \
        >> "$KERNEL_DIR/drivers/video/fbdev/Makefile"
fi

echo "==> applying the sharpsl NAND driver"
copy_in "$REPO/modules/nand/sharpsl_nand_patched.c" drivers/mtd/nand/raw/sharpsl.c
copy_in "$REPO/modules/nand/sharpslpart.c"          drivers/mtd/parsers/sharpslpart.c
copy_in "$REPO/modules/nand/sharpsl.h"              include/linux/mtd/sharpsl.h

echo "==> rate-limiting JFFS2's per-block ECC warning (see modules/jffs2/wbuf_patched.c)"
copy_in "$REPO/modules/jffs2/wbuf_patched.c"        fs/jffs2/wbuf.c

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
for f in "$REPO"/modules/hostap/lib80211*.c; do
    copy_in "$f" "net/wireless/$(basename "$f")"
done
# lib80211.h is a public kernel header (every consumer includes it as
# <net/lib80211.h>, confirmed via grep across modules/hostap/*.c) -- it
# belongs in include/net/, not alongside the .c files in net/wireless/.
# Got this wrong initially: putting it in net/wireless/ left the header
# undiscoverable, so lib80211.c itself failed with "fatal error:
# net/lib80211.h: No such file or directory" despite the file being
# present on disk, just at the wrong path (hit exactly this in CI).
copy_in "$REPO/modules/hostap/lib80211.h" include/net/lib80211.h
copy_in "$REPO/modules/hostap/michael_mic.c" crypto/michael_mic.c

# hostap/{Kconfig,Makefile} above restore the SYMBOL definitions, but that
# alone isn't enough: the commit that removed hostap_cs from mainline also
# deleted the lines in the *enclosing* drivers/net/wireless/intersil/
# {Kconfig,Makefile} that source/build the hostap/ subdirectory at all
# (other intersil drivers like orinoco/p54 are still upstream, so that
# directory itself still exists -- it just no longer mentions hostap).
# There's no pristine snapshot of these two files tracked anywhere in
# modules/ (unlike the full-file copies elsewhere in this script), so
# patch the missing wiring back in directly, idempotently -- this is the
# actual root cause of "missing input file: .../hostap/hostap.ko" recurring
# even with hostap's own Kconfig/Makefile correctly restored: without this,
# CONFIG_HOSTAP has no Kconfig symbol anywhere in the tree, so oldconfig
# silently drops it (same failure mode as every other Kconfig gap in this
# script, just one directory level higher than hostap/ itself).
ensure_kconfig_source() {
    file="$1"; source_line="$2"
    if [ ! -f "$file" ]; then
        echo "tools/setup-kernel-src.sh: expected upstream file missing: $file" >&2
        exit 1
    fi
    grep -qF "$source_line" "$file" && return 0
    if grep -q '^endmenu' "$file"; then
        marker='^endmenu'
    elif grep -q '^endif' "$file"; then
        marker='^endif'
    else
        marker=''
    fi
    if [ -n "$marker" ]; then
        awk -v line="$source_line" -v marker="$marker" \
            '!done && $0 ~ marker { print line; done=1 } { print }' \
            "$file" > "$file.piko-tmp"
        mv "$file.piko-tmp" "$file"
    else
        printf '%s\n' "$source_line" >> "$file"
    fi
}
ensure_line_in_file() {
    file="$1"; line="$2"
    if [ ! -f "$file" ]; then
        echo "tools/setup-kernel-src.sh: expected upstream file missing: $file" >&2
        exit 1
    fi
    grep -qF "$line" "$file" || printf '%s\n' "$line" >> "$file"
}
INTERSIL_DIR=drivers/net/wireless/intersil
ensure_kconfig_source "$KERNEL_DIR/$INTERSIL_DIR/Kconfig" \
    'source "drivers/net/wireless/intersil/hostap/Kconfig"'
ensure_line_in_file "$KERNEL_DIR/$INTERSIL_DIR/Makefile" \
    'obj-$(CONFIG_HOSTAP) += hostap/'

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
# pxa2xx-i2s.c fixes the clock-provider switch in set_dai_fmt: the
# SND_SOC_DAIFMT_{BP,BC}_{FP,FC} aliases are codec-centric, so the old
# BP_FP/BC_FP cases never matched corgi's CBC_CFC request and the CPU
# never drove BITCLK (silent playback, DMA armed but never requested).
# See the PATCHED comment in the file itself.
copy_in "$REPO/modules/sound-pxa/pxa2xx-i2s_patched.c" sound/soc/pxa/pxa2xx-i2s.c

echo "==> applying kernel.config-corgi-$KERNEL_VERSION"
copy_in "$REPO/kernel.config-corgi-$KERNEL_VERSION" .config

if [ -n "${TOOLCHAIN_BIN_DIR}" ] && [ -d "$TOOLCHAIN_BIN_DIR" ]; then
    PATH="$TOOLCHAIN_BIN_DIR:$PATH"
fi

if [ -z "${CROSS_COMPILE:-}" ]; then
    for prefix in arm-buildroot-linux-uclibcgnueabi- arm-unknown-linux-uclibcgnueabi- arm-linux-gnueabi- arm-unknown-linux-gnueabi-; do
        if command -v "${prefix}gcc" >/dev/null 2>&1; then
            CROSS_COMPILE="$prefix"
            break
        fi
    done
fi

if [ -z "${CROSS_COMPILE:-}" ]; then
    echo "tools/setup-kernel-src.sh: no ARM cross compiler found in PATH." >&2
    echo "Expected one of: arm-buildroot-linux-uclibcgnueabi-gcc, arm-unknown-linux-uclibcgnueabi-gcc, arm-linux-gnueabi-gcc, arm-unknown-linux-gnueabi-gcc" >&2
    echo "Set TOOLCHAIN_BIN_DIR to your toolchain bin path, or export CROSS_COMPILE explicitly." >&2
    exit 1
fi

echo "==> using cross-compiler prefix for oldconfig: $CROSS_COMPILE"

echo "==> oldconfig (non-interactive, accepting defaults for anything new)"
( cd "$KERNEL_DIR" && yes "" | make ARCH=arm CROSS_COMPILE="$CROSS_COMPILE" oldconfig >/tmp/piko-oldconfig.log 2>&1 ) || {
    echo "tools/setup-kernel-src.sh: oldconfig failed, see /tmp/piko-oldconfig.log" >&2
    exit 1
}

touch "$MARKER"
echo "==> $KERNEL_DIR ready to build"
