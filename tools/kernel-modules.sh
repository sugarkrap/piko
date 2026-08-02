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

# Sound/ALSA stack used to ship here as insmod'd .ko's, loaded in dependency
# order by /usr/sbin/audioon. Now built into the kernel (CONFIG_SOUND/
# CONFIG_SND*/CONFIG_AC97_BUS=y etc.) for the same reason SPI/touchscreen
# were: the WM8731 codec is board-soldered and always present at boot, so
# there was nothing gained by shipping it as a separately-insmod'd module,
# and building it in closes off the same kernel/module ABI-drift risk called
# out below for WIFI_MODULES. Kept as an empty var (not deleted) because
# every consumer of this file runs under `set -eu`.
AUDIO_MODULES="
"

# WiFi/PCMCIA stack -- MUST be redeployed in lockstep with every kernel
# rebuild (learned the hard way 2026-07-26: shipping a new zImage without
# these leaves stale .ko's whose struct-module ABI no longer matches the
# new kernel, so insmod fails with "section size must match", PCMCIA never
# comes up, and the device becomes unreachable over WiFi/SSH).
#
# libarc4.ko (CONFIG_CRYPTO_LIB_ARC4) used to ship here too, but the kernel
# config already carries it as CONFIG_CRYPTO_LIB_ARC4=y -- no .ko is built
# for it anymore, so the entry was stale (found while cross-checking this
# list against kernel.config-corgi-7.1.4 for the audio/NLS built-in
# change): the next WiFi-affecting redeploy would have hard-failed here
# with "missing module", the exact failure mode the paragraph above exists
# to prevent, just via a different mechanism (stale list vs. mismatched
# ABI).
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
"

# NOTE: the SPI stack (ssp, spi-pxa2xx-core/platform), ads7846
# (touchscreen), and evdev/mousedev used to ship here as SPI_MODULES --
# they're all built into the kernel now (CONFIG_PXA_SSP/CONFIG_SPI_PXA2XX/
# CONFIG_TOUCHSCREEN_ADS7846/CONFIG_INPUT_EVDEV/CONFIG_INPUT_MOUSEDEV=y),
# for the same reason MMC was: this is board-soldered hardware that's
# always present at boot and never unloaded, so there's nothing gained by
# making it a separately-shipped, separately-insmod'd module, and every
# device now probes during kernel init instead of racing rcS/mdev.

# VFAT/NLS/FAT for SD cards used to ship here too. CONFIG_MMC/
# CONFIG_MMC_BLOCK/CONFIG_MMC_PXA are built into the kernel (not modules)
# specifically so the pxa2xx-mci platform device probes and the block
# device appears during kernel init, before rcS's mdev daemon is even
# running -- see rcS's mdev-coldplug comment for the race this avoids.
# CONFIG_FAT_FS/CONFIG_VFAT_FS/the NLS codepages (Cacko-formatted SD cards
# use cp437/cp850/iso8859-15) are all built in now too, for the same
# reason; ext4 support is built into the kernel as well. Kept as an empty
# var (not deleted) because every consumer of this file runs under
# `set -eu`.
SD_MODULES="
"
