
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

WIFI_MODULES="
kernel/drivers/net/wireless/intersil/hostap/hostap.ko
kernel/drivers/net/wireless/intersil/hostap/hostap_cs.ko
kernel/net/wireless/lib80211.ko
kernel/net/wireless/lib80211_crypt_wep.ko
kernel/net/wireless/lib80211_crypt_ccmp.ko
kernel/net/wireless/lib80211_crypt_tkip.ko
kernel/lib/crypto/libarc4.ko
"

SD_MODULES=""

NAND_MODULES="
kernel/drivers/mtd/parsers/sharpslpart.ko
kernel/drivers/mtd/nand/raw/sharpsl.ko
"

CPUFREQ_MODULES="
kernel/drivers/cpufreq/pxa2xx-cpufreq.ko
"
