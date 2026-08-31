#!/bin/sh
set -eu

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
KERNEL_VERSION="${KERNEL_VERSION:-7.1.4}"
KERNEL_CONFIG="${KERNEL_CONFIG:-kernel.config-corgi-$KERNEL_VERSION}"
KERNEL_SRC_DIR="${KERNEL_SRC_DIR:-$REPO/build/kernel/src}"
KERNEL_DIR="$KERNEL_SRC_DIR/linux-$KERNEL_VERSION"
KERNEL_TARBALL_DIR="${KERNEL_TARBALL_DIR:-$REPO/build/kernel/dl}"
TARBALL="$KERNEL_TARBALL_DIR/linux-$KERNEL_VERSION.tar.xz"
KERNEL_URL="https://cdn.kernel.org/pub/linux/kernel/v${KERNEL_VERSION%%.*}.x/linux-$KERNEL_VERSION.tar.xz"
MARKER="$KERNEL_DIR/.piko-patched"
TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin}"

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

mkdir -p "$KERNEL_SRC_DIR" "$KERNEL_TARBALL_DIR"

if [ -f "$TARBALL" ] && ! xz -t "$TARBALL" 2>/dev/null; then
    echo "==> $TARBALL is truncated or corrupt, discarding it"
    rm -f "$TARBALL"
fi

if [ ! -f "$TARBALL" ]; then
    echo "==> downloading $KERNEL_URL"
    attempt=0
    until curl -fL --http1.1 --retry 6 --retry-delay 5 --retry-all-errors \
               --speed-limit 1024 --speed-time 30 -C - \
               -o "$TARBALL.partial" "$KERNEL_URL"; do
        attempt=$((attempt + 1))
        if [ "$attempt" -ge 5 ]; then
            echo "tools/kernel/setup-kernel-src.sh: $KERNEL_URL kept breaking off after $attempt attempts" >&2
            echo "    kept $(du -h "$TARBALL.partial" 2>/dev/null | cut -f1) at $TARBALL.partial" >&2
            exit 1
        fi
        echo "    broke off at $(du -h "$TARBALL.partial" 2>/dev/null | cut -f1), resuming (attempt $attempt)"
        sleep $((attempt * 10))
    done
    xz -t "$TARBALL.partial"
    mv "$TARBALL.partial" "$TARBALL"
else
    echo "==> reusing cached $TARBALL"
fi

if [ ! -d "$KERNEL_DIR" ]; then
    echo "==> extracting to $KERNEL_SRC_DIR"
    tar xf "$TARBALL" -C "$KERNEL_SRC_DIR"
fi

if [ ! -f "$KERNEL_DIR/Makefile" ]; then
    echo "tools/kernel/setup-kernel-src.sh: $KERNEL_DIR doesn't look like a kernel tree (no Makefile)" >&2
    exit 1
fi

copy_in() {
    src="$1"
    dest="$KERNEL_DIR/$2"
    if [ ! -f "$src" ]; then
        echo "tools/kernel/setup-kernel-src.sh: missing tracked input: $src" >&2
        exit 1
    fi
    mkdir -p "$(dirname "$dest")"
    cp "$src" "$dest"
}

echo "==> applying Corgi board files"
copy_in "$REPO/modules/mach-pxa/corgi.c"    arch/arm/mach-pxa/corgi.c
copy_in "$REPO/modules/mach-pxa/corgi_pm.c" arch/arm/mach-pxa/corgi_pm.c
copy_in "$REPO/modules/mach-pxa/corgi.h"            arch/arm/mach-pxa/corgi.h

copy_in "$REPO/modules/arch-arm/head.S"      arch/arm/kernel/head.S

copy_in "$REPO/modules/arch-arm/mmu.c"        arch/arm/mm/mmu.c

copy_in "$REPO/modules/arch-arm/setup.c"       arch/arm/kernel/setup.c

copy_in "$REPO/modules/arch-arm/atags_parse.c" arch/arm/kernel/atags_parse.c

copy_in "$REPO/modules/arch-arm/main.c"        init/main.c

copy_in "$REPO/modules/mach-pxa/generic.c"    arch/arm/mach-pxa/generic.c
copy_in "$REPO/modules/clk-pxa/timer_pxa.c"   drivers/clocksource/timer-pxa.c
copy_in "$REPO/modules/clk-pxa/clk_pxa25x.c"  drivers/clk/pxa/clk-pxa25x.c
copy_in "$REPO/modules/clk-pxa/clk_pxa.c"     drivers/clk/pxa/clk-pxa.c

copy_in "$REPO/modules/cpufreq/pxa2xx_cpufreq.c" drivers/cpufreq/pxa2xx-cpufreq.c

copy_in "$REPO/modules/mach-pxa/pxa25x.c"     arch/arm/mach-pxa/pxa25x.c

echo "==> applying the pxa2xx-spi double-SSP-request fix"
copy_in "$REPO/modules/spi/spi_pxa2xx_platform.c" drivers/spi/spi-pxa2xx-platform.c

echo "==> applying reference current-driver snapshots"
copy_in "$REPO/modules/mach-pxa/spitz.c"          arch/arm/mach-pxa/spitz.c
copy_in "$REPO/modules/mach-pxa/spitz_pm.c"       arch/arm/mach-pxa/spitz_pm.c
copy_in "$REPO/modules/mach-pxa/sharpsl_pm.c"     arch/arm/mach-pxa/sharpsl_pm.c
copy_in "$REPO/modules/usb-gadget/pxa25x_udc.c"   drivers/usb/gadget/udc/pxa25x_udc.c
copy_in "$REPO/modules/usb-gadget/pxa25x_udc.h"   drivers/usb/gadget/udc/pxa25x_udc.h

echo "==> applying the W100 (Imageon) display driver"
copy_in "$REPO/modules/w100/w100fb.c"  drivers/video/fbdev/w100fb.c
copy_in "$REPO/modules/w100/w100fb_private.h" drivers/video/fbdev/w100fb.h
copy_in "$REPO/modules/w100/w100fb.h"          include/video/w100fb.h
copy_in "$REPO/modules/w100/w100fb_accel.h"    include/video/w100fb_accel.h

echo "==> applying the Corgi LCD (VCOM/phase override) driver"
copy_in "$REPO/modules/lcd/corgi_lcd.c" drivers/video/backlight/corgi_lcd.c

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
copy_in "$REPO/modules/nand/sharpsl_nand.c" drivers/mtd/nand/raw/sharpsl.c
copy_in "$REPO/modules/nand/sharpslpart.c"          drivers/mtd/parsers/sharpslpart.c
copy_in "$REPO/modules/nand/sharpsl.h"              include/linux/mtd/sharpsl.h

echo "==> rate-limiting JFFS2's per-block ECC warning (see modules/jffs2/wbuf.c)"
copy_in "$REPO/modules/jffs2/wbuf.c"        fs/jffs2/wbuf.c

echo "==> applying hostap_cs (PCMCIA WiFi)"
HOSTAP_DEST=drivers/net/wireless/intersil/hostap
for f in "$REPO"/modules/hostap/hostap*.c "$REPO"/modules/hostap/hostap*.h; do
    copy_in "$f" "$HOSTAP_DEST/$(basename "$f")"
done
copy_in "$REPO/modules/hostap/Kconfig"  "$HOSTAP_DEST/Kconfig"
copy_in "$REPO/modules/hostap/Makefile" "$HOSTAP_DEST/Makefile"

ensure_kconfig_source() {
    file="$1"; source_line="$2"
    if [ ! -f "$file" ]; then
        echo "tools/kernel/setup-kernel-src.sh: expected upstream file missing: $file" >&2
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
        echo "tools/kernel/setup-kernel-src.sh: expected upstream file missing: $file" >&2
        exit 1
    fi
    grep -qF "$line" "$file" || printf '%s\n' "$line" >> "$file"
}
INTERSIL_DIR=drivers/net/wireless/intersil
ensure_kconfig_source "$KERNEL_DIR/$INTERSIL_DIR/Kconfig" \
    'source "drivers/net/wireless/intersil/hostap/Kconfig"'
ensure_line_in_file "$KERNEL_DIR/$INTERSIL_DIR/Makefile" \
    'obj-$(CONFIG_HOSTAP) += hostap/'

echo "==> applying mach-pxa/wireless/crypto Kconfig+Makefile wiring"
copy_in "$REPO/modules/mach-pxa/Kconfig"  arch/arm/mach-pxa/Kconfig
copy_in "$REPO/modules/mach-pxa/Makefile" arch/arm/mach-pxa/Makefile
copy_in "$REPO/modules/wireless/Kconfig"  net/wireless/Kconfig
copy_in "$REPO/modules/wireless/Makefile" net/wireless/Makefile
copy_in "$REPO/modules/crypto/Kconfig"    crypto/Kconfig
copy_in "$REPO/modules/crypto/Makefile"   crypto/Makefile

echo "==> applying the Corgi ASoC sound driver"
copy_in "$REPO/modules/sound-pxa/corgi.c"   sound/soc/pxa/corgi.c
copy_in "$REPO/modules/sound-pxa/Kconfig"   sound/soc/pxa/Kconfig
copy_in "$REPO/modules/sound-pxa/Makefile"  sound/soc/pxa/Makefile
copy_in "$REPO/modules/sound-pxa/pxa2xx-i2s.c" sound/soc/pxa/pxa2xx-i2s.c

if [ "$FORCE" -eq 1 ] || [ ! -f "$MARKER" ]; then
    echo "==> applying $KERNEL_CONFIG"
    copy_in "$REPO/$KERNEL_CONFIG" .config

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
        echo "tools/kernel/setup-kernel-src.sh: no ARM cross compiler found in PATH." >&2
        echo "Expected one of: arm-buildroot-linux-uclibcgnueabi-gcc, arm-unknown-linux-uclibcgnueabi-gcc, arm-linux-gnueabi-gcc, arm-unknown-linux-gnueabi-gcc" >&2
        echo "Set TOOLCHAIN_BIN_DIR to your toolchain bin path, or export CROSS_COMPILE explicitly." >&2
        exit 1
    fi

    echo "==> using cross-compiler prefix for oldconfig: $CROSS_COMPILE"

    echo "==> oldconfig (non-interactive, accepting defaults for anything new)"
    ( cd "$KERNEL_DIR" && yes "" | make ARCH=arm CROSS_COMPILE="$CROSS_COMPILE" oldconfig >/tmp/piko-oldconfig.log 2>&1 ) || {
        echo "tools/kernel/setup-kernel-src.sh: oldconfig failed, see /tmp/piko-oldconfig.log" >&2
        exit 1
    }

    touch "$MARKER"
else
    echo "==> keeping the existing .config ($MARKER present; pass --force to re-apply $KERNEL_CONFIG)"
fi

echo "==> $KERNEL_DIR ready to build"
