# Shared kernel-module manifest for the stage-2 kernel (kernel-src/linux-7.1.4).
# Sourced (". tools/kernel-modules.sh") by every script that ships modules
# alongside a stage-2 kernel build -- tools/chunked-deploy.sh (live SSH
# redeploy), flash/build-update-package.sh (offline update.tar), and
# flash/build-mtd3-jffs2.sh (SD-card recovery mtd3 image) -- so the module
# list only has one place to go stale relative to the kernel .config.
#
# Not a standalone script: no shebang, no set -eu (would clobber the
# sourcing script's own options). Just variable assignments.
#
# Every path is relative to $KERNEL_DIR. AUDIO_MODULES paths have no
# "kernel/" prefix (they're deployed flat, into a side directory
# /lib/modules/$KVER/zaurus-audio/<basename>, not the real depmod tree);
# WIFI_MODULES/SD_MODULES paths keep a "kernel/" prefix even
# though it must be stripped to find the file under $KERNEL_DIR, because
# that prefix is also the real /lib/modules/$KVER/... destination path, and
# some of these live directly under drivers/ while others (net/wireless,
# lib/crypto, fs/nls, fs/fat) don't -- the prefix has to travel with each
# entry rather than being reconstructed from a shorter name.

# Sound/ALSA stack. Order doesn't matter for deployment; flash/*/audioon
# (generated in tools/chunked-deploy.sh) loads them in dependency order at
# insmod time separately from this list.
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

# WiFi/PCMCIA stack -- MUST be redeployed in lockstep with every kernel
# rebuild (learned the hard way 2026-07-26: shipping a new zImage without
# these leaves stale .ko's whose struct-module ABI no longer matches the
# new kernel, so insmod fails with "section size must match", PCMCIA never
# comes up, and the device becomes unreachable over WiFi/SSH).
WIFI_MODULES="
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
"

# NOTE: the SPI stack (ssp, spi-pxa2xx-core/platform), ads7846
# (touchscreen), and evdev/mousedev used to ship here as SPI_MODULES --
# they're all built into the kernel now (CONFIG_PXA_SSP/CONFIG_SPI_PXA2XX/
# CONFIG_TOUCHSCREEN_ADS7846/CONFIG_INPUT_EVDEV/CONFIG_INPUT_MOUSEDEV=y),
# for the same reason MMC was: this is board-soldered hardware that's
# always present at boot and never unloaded, so there's nothing gained by
# making it a separately-shipped, separately-insmod'd module, and every
# device now probes during kernel init instead of racing rcS/mdev.

# NLS stack for SD cards. CONFIG_MMC/CONFIG_MMC_BLOCK/CONFIG_MMC_PXA are
# built into the kernel (not modules) specifically so the pxa2xx-mci
# platform device probes and the block device appears during kernel init,
# before rcS's mdev daemon is even running -- see rcS's mdev-coldplug
# comment for the race this avoids. FAT/VFAT (the usual Cacko-formatted SD
# card filesystem) are built in for the exact same reason -- CONFIG_FAT_FS
# and CONFIG_VFAT_FS are =y, not =m, so fs/fat/fat.ko and fs/fat/vfat.ko
# never exist as separate files to begin with (their objects link straight
# into the kernel image; see fs/fat/Makefile's fat-y/vfat-y). Listing them
# here used to make piko-sync-deploy/chunked-deploy.sh fail outright trying to
# read a .ko that was never built (found 2026-08-03, deploy died on this
# exact entry). Only CONFIG_MSDOS_FS stayed =m, and nothing on this device
# mounts plain msdosfs (real cards are vfat), so msdos.ko is intentionally
# not shipped either -- ext4 support is built into the kernel too.
SD_MODULES="
kernel/fs/nls/nls_cp437.ko
kernel/fs/nls/nls_cp850.ko
kernel/fs/nls/nls_iso8859-15.ko
"
