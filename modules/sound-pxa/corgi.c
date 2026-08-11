
#include <linux/bitops.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/soc.h>

#include "../codecs/wm8731.h"

static struct snd_soc_card *corgi_hifi_switch_card;
static void corgi_hifi_switch_fn(struct work_struct *w);
static DECLARE_DELAYED_WORK(corgi_hifi_switch_work, corgi_hifi_switch_fn);

static void corgi_hifi_switch_fn(struct work_struct *w)
{
	struct snd_soc_card *card = corgi_hifi_switch_card;
	struct snd_kcontrol *kctl;

	if (!card)
		return;

	kctl = snd_soc_card_get_kcontrol(card,
					 "Output Mixer HiFi Playback Switch");
	if (!kctl) {
		dev_warn(card->dev,
			 "couldn't find \"Output Mixer HiFi Playback Switch\" kcontrol to force on -- playback will likely be silent\n");
		return;
	}

	snd_soc_dapm_mixer_update_power(snd_soc_card_to_dapm(card), kctl, 1,
					NULL);
}

#include "pxa2xx-i2s.h"

#define CORGI_HP        0
#define CORGI_MIC       1
#define CORGI_LINE      2
#define CORGI_HEADSET   3
#define CORGI_HP_OFF    4

#define CORGI_SPK_ON    0
#define CORGI_SPK_OFF   1

struct corgi_audio_priv {
	int jack_func;
	int spk_func;
	struct gpio_desc *gpiod_mute_l;
	struct gpio_desc *gpiod_mute_r;
	struct gpio_desc *gpiod_apm_on;
	struct gpio_desc *gpiod_mic_bias;
};

static void corgi_ext_control(struct snd_soc_card *card)
{
	struct corgi_audio_priv *priv = snd_soc_card_get_drvdata(card);
	struct snd_soc_dapm_context *dapm = snd_soc_card_to_dapm(card);

	snd_soc_dapm_mutex_lock(dapm);

	switch (priv->jack_func) {
	case CORGI_HP:
		gpiod_set_value_cansleep(priv->gpiod_mute_l, 1);
		gpiod_set_value_cansleep(priv->gpiod_mute_r, 1);
		snd_soc_dapm_disable_pin_unlocked(dapm, "Mic Jack");
		snd_soc_dapm_disable_pin_unlocked(dapm, "Line Jack");
		snd_soc_dapm_enable_pin_unlocked(dapm, "Headphone Jack");
		snd_soc_dapm_disable_pin_unlocked(dapm, "Headset Jack");
		break;
	case CORGI_MIC:
		gpiod_set_value_cansleep(priv->gpiod_mute_l, 0);
		gpiod_set_value_cansleep(priv->gpiod_mute_r, 0);
		snd_soc_dapm_enable_pin_unlocked(dapm, "Mic Jack");
		snd_soc_dapm_disable_pin_unlocked(dapm, "Line Jack");
		snd_soc_dapm_disable_pin_unlocked(dapm, "Headphone Jack");
		snd_soc_dapm_disable_pin_unlocked(dapm, "Headset Jack");
		break;
	case CORGI_LINE:
		gpiod_set_value_cansleep(priv->gpiod_mute_l, 0);
		gpiod_set_value_cansleep(priv->gpiod_mute_r, 0);
		snd_soc_dapm_disable_pin_unlocked(dapm, "Mic Jack");
		snd_soc_dapm_enable_pin_unlocked(dapm, "Line Jack");
		snd_soc_dapm_disable_pin_unlocked(dapm, "Headphone Jack");
		snd_soc_dapm_disable_pin_unlocked(dapm, "Headset Jack");
		break;
	case CORGI_HEADSET:
		gpiod_set_value_cansleep(priv->gpiod_mute_l, 0);
		gpiod_set_value_cansleep(priv->gpiod_mute_r, 1);
		snd_soc_dapm_enable_pin_unlocked(dapm, "Mic Jack");
		snd_soc_dapm_disable_pin_unlocked(dapm, "Line Jack");
		snd_soc_dapm_disable_pin_unlocked(dapm, "Headphone Jack");
		snd_soc_dapm_enable_pin_unlocked(dapm, "Headset Jack");
		break;
	case CORGI_HP_OFF:
	default:
		gpiod_set_value_cansleep(priv->gpiod_mute_l, 0);
		gpiod_set_value_cansleep(priv->gpiod_mute_r, 0);
		snd_soc_dapm_disable_pin_unlocked(dapm, "Mic Jack");
		snd_soc_dapm_disable_pin_unlocked(dapm, "Line Jack");
		snd_soc_dapm_disable_pin_unlocked(dapm, "Headphone Jack");
		snd_soc_dapm_disable_pin_unlocked(dapm, "Headset Jack");
		break;
	}

	if (priv->spk_func == CORGI_SPK_ON)
		snd_soc_dapm_enable_pin_unlocked(dapm, "Ext Spk");
	else
		snd_soc_dapm_disable_pin_unlocked(dapm, "Ext Spk");

	snd_soc_dapm_sync_unlocked(dapm);
	snd_soc_dapm_mutex_unlock(dapm);
}

static int corgi_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);

	corgi_ext_control(rtd->card);
	return 0;
}

static void corgi_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct corgi_audio_priv *priv = snd_soc_card_get_drvdata(rtd->card);

	gpiod_set_value_cansleep(priv->gpiod_mute_l, 1);
	gpiod_set_value_cansleep(priv->gpiod_mute_r, 1);
}

static int corgi_hw_params(struct snd_pcm_substream *substream,
				   struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	unsigned int clk;
	int ret;

	switch (params_rate(params)) {
	case 8000:
	case 16000:
	case 48000:
	case 96000:
		clk = 12288000;
		break;
	case 11025:
	case 22050:
	case 44100:
		clk = 11289600;
		break;
	default:
		return -EINVAL;
	}

	ret = snd_soc_dai_set_fmt(codec_dai, SND_SOC_DAIFMT_I2S |
					   SND_SOC_DAIFMT_NB_NF |
					   SND_SOC_DAIFMT_CBC_CFC);
	if (ret < 0)
		return ret;

	ret = snd_soc_dai_set_fmt(cpu_dai, SND_SOC_DAIFMT_I2S |
					 SND_SOC_DAIFMT_NB_NF |
					 SND_SOC_DAIFMT_CBC_CFC);
	if (ret < 0)
		return ret;

	ret = snd_soc_dai_set_sysclk(codec_dai, WM8731_SYSCLK_XTAL, clk,
					    SND_SOC_CLOCK_IN);
	if (ret < 0)
		return ret;

	ret = snd_soc_dai_set_sysclk(cpu_dai, PXA2XX_I2S_SYSCLK, 0,
					    SND_SOC_CLOCK_IN);
	if (ret < 0)
		return ret;

	return 0;
}

static const struct snd_soc_ops corgi_ops = {
	.startup = corgi_startup,
	.hw_params = corgi_hw_params,
	.shutdown = corgi_shutdown,
};

static int corgi_get_jack(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct corgi_audio_priv *priv = snd_soc_card_get_drvdata(card);

	ucontrol->value.enumerated.item[0] = priv->jack_func;
	return 0;
}

static int corgi_set_jack(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct corgi_audio_priv *priv = snd_soc_card_get_drvdata(card);
	unsigned int value = ucontrol->value.enumerated.item[0];

	if (priv->jack_func == value)
		return 0;

	priv->jack_func = value;
	corgi_ext_control(card);
	return 1;
}

static int corgi_get_spk(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct corgi_audio_priv *priv = snd_soc_card_get_drvdata(card);

	ucontrol->value.enumerated.item[0] = priv->spk_func;
	return 0;
}

static int corgi_set_spk(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct corgi_audio_priv *priv = snd_soc_card_get_drvdata(card);
	unsigned int value = ucontrol->value.enumerated.item[0];

	if (priv->spk_func == value)
		return 0;

	priv->spk_func = value;
	corgi_ext_control(card);
	return 1;
}

static int corgi_amp_event(struct snd_soc_dapm_widget *w,
				   struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_card *card = snd_soc_dapm_to_card(w->dapm);
	struct corgi_audio_priv *priv = snd_soc_card_get_drvdata(card);

	gpiod_set_value_cansleep(priv->gpiod_apm_on, SND_SOC_DAPM_EVENT_ON(event));
	return 0;
}

static int corgi_mic_event(struct snd_soc_dapm_widget *w,
				   struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_card *card = snd_soc_dapm_to_card(w->dapm);
	struct corgi_audio_priv *priv = snd_soc_card_get_drvdata(card);

	gpiod_set_value_cansleep(priv->gpiod_mic_bias,
				SND_SOC_DAPM_EVENT_ON(event));
	return 0;
}

static const struct snd_soc_dapm_widget corgi_dapm_widgets[] = {
	SND_SOC_DAPM_HP("Headphone Jack", NULL),
	SND_SOC_DAPM_MIC("Mic Jack", corgi_mic_event),
	SND_SOC_DAPM_SPK("Ext Spk", corgi_amp_event),
	SND_SOC_DAPM_LINE("Line Jack", NULL),
	SND_SOC_DAPM_HP("Headset Jack", NULL),
};

static const struct snd_soc_dapm_route corgi_audio_map[] = {
	{"Headset Jack", NULL, "LHPOUT"},
	{"Headphone Jack", NULL, "LHPOUT"},
	{"Headphone Jack", NULL, "RHPOUT"},
	{"Ext Spk", NULL, "LOUT"},
	{"Ext Spk", NULL, "ROUT"},
	{"MICIN", NULL, "Mic Jack"},
	{"MICIN", NULL, "Line Jack"},
};

static const char * const corgi_jack_function[] = {
	"Headphone", "Mic", "Line", "Headset", "Off"
};
static const char * const corgi_spk_function[] = {
	"On", "Off"
};

static const struct soc_enum corgi_enum[] = {
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(corgi_jack_function), corgi_jack_function),
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(corgi_spk_function), corgi_spk_function),
};

static const struct snd_kcontrol_new corgi_controls[] = {
	SOC_ENUM_EXT("Jack Function", corgi_enum[0], corgi_get_jack,
		     corgi_set_jack),
	SOC_ENUM_EXT("Speaker Function", corgi_enum[1], corgi_get_spk,
		     corgi_set_spk),
};

static int corgi_wm8731_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_card *card = rtd->card;
	struct snd_soc_dapm_context *dapm = snd_soc_card_to_dapm(card);
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);

	snd_soc_dapm_disable_pin(dapm, "LLINEIN");
	snd_soc_dapm_disable_pin(dapm, "RLINEIN");

	snd_soc_component_update_bits(codec_dai->component, WM8731_APANA,
				      BIT(4), BIT(4));

	corgi_hifi_switch_card = card;
	schedule_delayed_work(&corgi_hifi_switch_work, msecs_to_jiffies(500));

	corgi_ext_control(card);
	return 0;
}

SND_SOC_DAILINK_DEFS(wm8731,
	DAILINK_COMP_ARRAY(COMP_CPU("pxa2xx-i2s")),
	DAILINK_COMP_ARRAY(COMP_CODEC("wm8731.0-001b", "wm8731-hifi")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()));

static struct snd_soc_dai_link corgi_dai = {
	.name = "WM8731",
	.stream_name = "WM8731",
	.init = corgi_wm8731_init,
	.ops = &corgi_ops,
	SND_SOC_DAILINK_REG(wm8731),
};

static struct snd_soc_card snd_soc_corgi = {
	.name = "Corgi",
	.owner = THIS_MODULE,
	.dai_link = &corgi_dai,
	.num_links = 1,
	.controls = corgi_controls,
	.num_controls = ARRAY_SIZE(corgi_controls),
	.dapm_widgets = corgi_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(corgi_dapm_widgets),
	.dapm_routes = corgi_audio_map,
	.num_dapm_routes = ARRAY_SIZE(corgi_audio_map),
	.fully_routed = true,
};

static int corgi_probe(struct platform_device *pdev)
{
	struct corgi_audio_priv *priv;
	struct snd_soc_card *card = &snd_soc_corgi;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->gpiod_mute_l = devm_gpiod_get(&pdev->dev, "mute-l", GPIOD_OUT_LOW);
	if (IS_ERR(priv->gpiod_mute_l))
		return PTR_ERR(priv->gpiod_mute_l);

	priv->gpiod_mute_r = devm_gpiod_get(&pdev->dev, "mute-r", GPIOD_OUT_LOW);
	if (IS_ERR(priv->gpiod_mute_r))
		return PTR_ERR(priv->gpiod_mute_r);

	priv->gpiod_apm_on = devm_gpiod_get(&pdev->dev, "apm-on", GPIOD_OUT_LOW);
	if (IS_ERR(priv->gpiod_apm_on))
		return PTR_ERR(priv->gpiod_apm_on);

	priv->gpiod_mic_bias = devm_gpiod_get(&pdev->dev, "mic-bias",
					      GPIOD_OUT_LOW);
	if (IS_ERR(priv->gpiod_mic_bias))
		return PTR_ERR(priv->gpiod_mic_bias);

	priv->jack_func = CORGI_HP_OFF;
	priv->spk_func = CORGI_SPK_ON;

	card->dev = &pdev->dev;
	snd_soc_card_set_drvdata(card, priv);

	ret = devm_snd_soc_register_card(&pdev->dev, card);
	if (ret)
		dev_err(&pdev->dev, "snd_soc_register_card() failed: %d\n", ret);

	return ret;
}

static struct platform_driver corgi_driver = {
	.driver = {
		.name = "corgi-audio",
		.pm = &snd_soc_pm_ops,
	},
	.probe = corgi_probe,
};

module_platform_driver(corgi_driver);

MODULE_AUTHOR("Richard Purdie");
MODULE_DESCRIPTION("ALSA SoC Corgi/Husky");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:corgi-audio");