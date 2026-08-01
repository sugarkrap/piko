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
# WIFI_MODULES/SPI_MODULES/SD_MODULES paths keep a "kernel/" prefix even
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

# SPI stack -- needed for the MAX1111 ADC (main battery voltage, see
# sharpsl-pm's "Cannot read main battery!" warning) which hangs off corgi's
# SPI1 bus (spi_board_info registered unconditionally in corgi.c, see
# corgi_init_spi()). CONFIG_SPI_PXA2XX is a module (=m), and until it loads
# and registers the SPI master, the max1111 device never probes, so
# sharpsl_pm_pxa_read_max1111()/max1111_read_channel() always fails. These
# modules were NEVER part of the original mtd3 rootfs build, so modprobe
# can't find them via modules.dep (no entry exists) -- rcS loads them with
# insmod + explicit path instead (see rootfs/etc/init.d/rcS).
#
# ORDER MATTERS: ssp.ko (drivers/soc/pxa/ssp.c) MUST be loaded before
# spi-pxa2xx-platform.ko: it exports pxa_ssp_request()/pxa_ssp_free(),
# which spi-pxa2xx-platform.ko needs at insmod time ("Unknown symbol
# pxa_ssp_request/pxa_ssp_free" if missing) -- discovered 2026-07-26 after
# the platform module loaded but the SPI bus/max1111 never registered.
#
# ads7846.ko (touchscreen) also hangs off this same SPI1 bus. It was
# PREVIOUSLY built-in (CONFIG_TOUCHSCREEN_ADS7846=y) in whatever kernel is
# currently running on-device, so it "just worked" with no explicit load
# step -- but the current kernel.config-corgi-7.1.4 has it as =m, so a
# rebuilt kernel produces a standalone ads7846.ko that NOTHING loaded
# (found 2026-07-27, before it ever got deployed and silently broke the
# touchscreen). Ship + load it explicitly here like the other SPI modules
# rather than relying on it being built-in.
#
# evdev.ko/mousedev.ko are also =m (CONFIG_INPUT_EVDEV=m,
# CONFIG_INPUT_MOUSEDEV=m) and NOT SPI devices themselves, but they're the
# input-core handler modules that actually create /dev/input/eventN
# (evdev) and /dev/input/mice (mousedev) once ads7846 registers its input
# device -- without evdev.ko, ads7846 probes fine but no eventN node ever
# appears, so anything reading raw evdev (e.g. handheldquake's vid_fb.c)
# would silently see no touchscreen at all. Shipped in this same list for
# convenience since they're needed by the same touchscreen bring-up.
SPI_MODULES="
kernel/drivers/soc/pxa/ssp.ko
kernel/drivers/spi/spi-pxa2xx-core.ko
kernel/drivers/spi/spi-pxa2xx-platform.ko
kernel/drivers/input/touchscreen/ads7846.ko
kernel/drivers/input/evdev.ko
kernel/drivers/input/mousedev.ko
"

# VFAT/NLS stack for SD cards. CONFIG_MMC/CONFIG_MMC_BLOCK/CONFIG_MMC_PXA
# are built into the kernel (not modules) specifically so the pxa2xx-mci
# platform device probes and the block device appears during kernel init,
# before rcS's mdev daemon is even running -- see rcS's mdev-coldplug
# comment for the race this avoids. VFAT/NLS covers the usual
# Cacko-formatted SD cards; ext4 support is built into the kernel too.
SD_MODULES="
kernel/fs/nls/nls_cp437.ko
kernel/fs/nls/nls_cp850.ko
kernel/fs/nls/nls_iso8859-15.ko
kernel/fs/fat/fat.ko
kernel/fs/fat/vfat.ko
"
