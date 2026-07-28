#!/bin/sh
set -eu

# Builds an offline update package for userspace/src/piko-update.c: a plain
# ustar tar (no compression -- piko-update reads it with its own from-
# scratch reader, no tar/gzip/unzip dependency on the device) containing
# a MANIFEST (md5 per shipped file) plus:
#
#   - boot/zImage-full + lib/modules/$KVER/...   (only if kernel-src is
#     available locally -- see "Kernel/modules" below)
#   - everything under nand-root/, mapped straight onto the same paths
#     under "/" (etc/*, usr/sbin/*, init -- whatever's actually committed
#     there is what gets shipped, so this can't drift from a hand-picked
#     file list the way two independent lists would)
#   - a freshly cross-compiled usr/sbin/piko-update itself (self-update)
#
# This is the offline counterpart to flash/chunked-deploy.sh (which pushes
# the same kind of update live over SSH). Use this one when the device
# isn't reachable over WiFi at all -- copy the resulting update.tar to an
# SD card and run `piko-update /mnt/card/update.tar` on the device.
#
# Usage:
#   flash/build-update-package.sh [output.tar]
#
# Env overrides (defaults match flash/build-and-deploy.sh):
#   KERNEL_DIR     kernel-src/linux-7.1.4 checkout (gitignored, local only)
#   TOOLCHAIN      directory holding CROSS_COMPILE-prefixed binaries
#   CROSS_COMPILE  cross toolchain prefix
#
# If KERNEL_DIR doesn't exist (e.g. in CI, which has no local buildroot/
# kernel-src checkout -- see docs/HOWTO-BUILD-DEPLOY-KERNEL.md), this
# script still produces a valid, useful package: piko-update itself plus
# the full rootfs/config overlay, just without a kernel bump. It prints
# which mode it ran in.

REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$REPO/update.tar}"

KERNEL_DIR="${KERNEL_DIR:-$REPO/kernel-src/linux-7.1.4}"
TOOLCHAIN="${TOOLCHAIN:-/home/makaron/Code/dosbox-armv5-zaurus/buildroot/output/host/bin}"
CROSS_COMPILE="${CROSS_COMPILE:-arm-buildroot-linux-uclibcgnueabi-}"

STAGE="$(mktemp -d /tmp/piko-update-package.XXXXXX)"
trap 'rm -rf "$STAGE"' EXIT

# manifest_add SRC_FILE DEST_REL_PATH [MODE]
# Copies SRC_FILE into the staging tree at DEST_REL_PATH (relative to "/"),
# records its md5, and appends a MANIFEST line. MODE defaults to SRC_FILE's
# own permission bits so scripts/binaries keep their exec bit automatically.
MANIFEST="$STAGE/MANIFEST"
FILES_LIST="$STAGE/.files"
: > "$FILES_LIST"

manifest_add() {
    src="$1"
    dest="$2"
    mode="${3:-}"

    if [ ! -f "$src" ]; then
        echo "build-update-package: missing input file: $src" >&2
        exit 1
    fi
    if [ -z "$mode" ]; then
        mode="$(stat -c '%a' "$src" 2>/dev/null || stat -f '%Lp' "$src")"
    fi

    stage_dest="$STAGE/payload/$dest"
    mkdir -p "$(dirname "$stage_dest")"
    cp "$src" "$stage_dest"
    chmod "$mode" "$stage_dest"

    md5="$(md5sum "$src" | cut -d' ' -f1)"
    echo "$md5 $dest" >> "$MANIFEST"
    echo "$dest" >> "$FILES_LIST"
}

mkdir -p "$STAGE/payload"

{
    echo "PIKO-UPDATE-PACKAGE 1"
    echo "# built $(date -u +%Y-%m-%dT%H:%M:%SZ) from $(cd "$REPO" && git rev-parse --short HEAD 2>/dev/null || echo unknown)"
} > "$MANIFEST"

echo "==> cross-compiling userspace/src/piko-update.c"
GCC="${TOOLCHAIN}/${CROSS_COMPILE}gcc"
if [ ! -x "$GCC" ]; then
    # Fall back to whatever CROSS_COMPILE resolves to on PATH (e.g. CI's
    # apt-installed gcc-arm-linux-gnueabi -- a static glibc binary runs
    # fine on this uclibc rootfs since there's no dynamic libc dependency
    # at all once it's linked -static).
    GCC="${CROSS_COMPILE}gcc"
fi
if ! command -v "$GCC" >/dev/null 2>&1; then
    echo "build-update-package: no working cross-compiler found ($GCC)" >&2
    echo "  set TOOLCHAIN/CROSS_COMPILE, or install one (e.g. gcc-arm-linux-gnueabi)" >&2
    exit 1
fi
"$GCC" -march=armv5te -O2 -static -Wall -Wextra \
    -o "$STAGE/piko-update" "$REPO/userspace/src/piko-update.c"
STRIP="${GCC%gcc}strip"
command -v "$STRIP" >/dev/null 2>&1 && "$STRIP" "$STAGE/piko-update" || true
manifest_add "$STAGE/piko-update" "usr/sbin/piko-update" 755

echo "==> packaging nand-root/ overlay (etc/, usr/sbin/, init -- whatever's there)"
( cd "$REPO/nand-root" && find . -type f ) | sed 's#^\./##' | while read -r rel; do
    manifest_add "$REPO/nand-root/$rel" "$rel"
done

if [ -d "$KERNEL_DIR" ]; then
    echo "==> KERNEL_DIR present ($KERNEL_DIR) -- including kernel + modules"

    if [ ! -f "$KERNEL_DIR/arch/arm/boot/zImage" ]; then
        echo "build-update-package: $KERNEL_DIR exists but has no built zImage" >&2
        echo "  build it first: cd $KERNEL_DIR && ARCH=arm CROSS_COMPILE=$CROSS_COMPILE make zImage modules" >&2
        exit 1
    fi

    manifest_add "$KERNEL_DIR/arch/arm/boot/zImage" "boot/zImage-full"

    KVER="$(cat "$KERNEL_DIR/include/config/kernel.release" 2>/dev/null || true)"
    if [ -z "$KVER" ]; then
        echo "build-update-package: cannot determine kernel release (no include/config/kernel.release)" >&2
        exit 1
    fi
    echo "# kernel: $KVER" >> "$MANIFEST"

    # Same module set flash/chunked-deploy.sh deploys live over SSH -- kept
    # duplicated here on purpose, matching that script's own "keep this
    # self-contained" precedent, rather than a shared-include abstraction
    # for four lines that rarely change independently of a kernel rebuild.
    AUDIO_MODULES="
        sound/soundcore.ko
        sound/core/snd.ko
        sound/core/snd-timer.ko
        sound/core/snd-pcm.ko
        sound/core/snd-pcm-dmaengine.ko
        sound/arm/snd-pxa2xx-lib.ko
        sound/ac97_bus.ko
        sound/pci/ac97/snd-ac97-codec.ko
        sound/soc/snd-soc-core.ko
        sound/soc/pxa/snd-soc-pxa2xx.ko
        sound/soc/pxa/snd-soc-pxa2xx-i2s.ko
        sound/soc/codecs/snd-soc-wm8731.ko
        sound/soc/codecs/snd-soc-wm8731-i2c.ko
        sound/soc/pxa/snd-soc-corgi.ko
        sound/core/oss/snd-mixer-oss.ko
        sound/core/oss/snd-pcm-oss.ko
    "
    for relpath in $AUDIO_MODULES; do
        manifest_add "$KERNEL_DIR/$relpath" "lib/modules/$KVER/zaurus-audio/$(basename "$relpath")"
    done

    # These keep the "kernel/" depmod-tree prefix exactly as
    # chunked-deploy.sh's own WIFI_MODULES/SPI_MODULES/SD_MODULES lists do
    # (stripped to find the source file under KERNEL_DIR, kept as-is for
    # the /lib/modules/$KVER/... destination) -- some of these live
    # directly under drivers/, others (net/wireless, lib/crypto, fs/nls,
    # fs/fat) don't, so the prefix has to travel with each entry rather
    # than being reconstructed from a shorter name.
    WIFI_PCMCIA_SPI_SD_MODULES="
        kernel/drivers/pcmcia/pcmcia_core.ko
        kernel/drivers/pcmcia/pcmcia_rsrc.ko
        kernel/drivers/pcmcia/pcmcia.ko
        kernel/drivers/pcmcia/soc_common.ko
        kernel/drivers/pcmcia/pxa2xx_base.ko
        kernel/drivers/pcmcia/pxa2xx_sharpsl.ko
        kernel/drivers/net/wireless/intersil/hostap/hostap.ko
        kernel/drivers/net/wireless/intersil/hostap/hostap_cs.ko
        kernel/net/wireless/lib80211.ko
        kernel/net/wireless/lib80211_crypt_wep.ko
        kernel/net/wireless/lib80211_crypt_ccmp.ko
        kernel/net/wireless/lib80211_crypt_tkip.ko
        kernel/lib/crypto/libarc4.ko
        kernel/drivers/soc/pxa/ssp.ko
        kernel/drivers/spi/spi-pxa2xx-core.ko
        kernel/drivers/spi/spi-pxa2xx-platform.ko
        kernel/drivers/input/touchscreen/ads7846.ko
        kernel/drivers/input/evdev.ko
        kernel/drivers/input/mousedev.ko
        kernel/drivers/mmc/core/mmc_core.ko
        kernel/drivers/mmc/core/mmc_block.ko
        kernel/drivers/mmc/host/pxamci.ko
        kernel/fs/nls/nls_cp437.ko
        kernel/fs/nls/nls_cp850.ko
        kernel/fs/nls/nls_iso8859-15.ko
        kernel/fs/fat/fat.ko
        kernel/fs/fat/vfat.ko
    "
    for relpath in $WIFI_PCMCIA_SPI_SD_MODULES; do
        src_rel="$(echo "$relpath" | sed 's#^kernel/##')"
        manifest_add "$KERNEL_DIR/$src_rel" "lib/modules/$KVER/$relpath"
    done
else
    echo "==> KERNEL_DIR not found ($KERNEL_DIR) -- rootfs-only package (no kernel bump)"
    echo "# kernel: not included (rootfs-only package, no local kernel-src build found)" >> "$MANIFEST"
fi

echo "==> writing $OUT"
(
    cd "$STAGE"
    files="MANIFEST"
    while read -r rel; do
        files="$files payload/$rel"
    done < "$FILES_LIST"
    # shellcheck disable=SC2086
    tar --format=ustar --transform 's#^payload/##' -cf "$OUT" $files
)

md5sum "$OUT"
n="$(wc -l < "$FILES_LIST")"
echo "==> done: $OUT ($n file(s) + MANIFEST)"
echo "    copy to the SD card and run: piko-update /mnt/card/$(basename "$OUT")"
