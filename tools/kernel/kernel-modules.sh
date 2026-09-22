
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

CIR_MODULES="
kernel/drivers/media/rc/rc-core.ko
kernel/drivers/media/rc/ir-nec-decoder.ko
kernel/drivers/media/rc/ir-rc5-decoder.ko
kernel/drivers/media/rc/ir-rc6-decoder.ko
kernel/drivers/media/rc/ir-jvc-decoder.ko
kernel/drivers/media/rc/ir-sony-decoder.ko
kernel/drivers/media/rc/ir-sharp-decoder.ko
kernel/drivers/media/rc/ir-sanyo-decoder.ko
kernel/drivers/media/rc/piko-cir/piko-cir.ko
"

IRDA_MODULES="
kernel/lib/crc/crc-ccitt.ko
kernel/net/irda/irda.ko
kernel/net/irda/ircomm/ircomm.ko
kernel/net/irda/ircomm/ircomm-tty.ko
kernel/drivers/net/irda/pxaficp_ir.ko
"

USB_MODULES=""
