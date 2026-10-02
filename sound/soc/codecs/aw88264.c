// SPDX-License-Identifier: GPL-2.0-only
//
// aw88264.c  --  ALSA SoC AW88264 codec driver
//
// The Awinic AW88264 smart speaker amplifier, as found (twice, as a stereo
// pair) on the Fairphone 4.
//
// This is a plain amplifier driver: power, mute, volume and the I2S slave
// configuration, and nothing else. The vendor driver it replaces also carries
// firmware-container loading, DSP calibration and a thermal monitor, none of
// which is needed to make sound and none of which belongs in a first
// submission.
//
// It is NOT derived from mainline's aw88261/aw88395 family, which covers a
// later generation of the part (chip ids 0x2049..0x2183). This one reports
// 0x1852 and its register map diverges above 0x05 -- most importantly volume,
// which lives in HAGCCFG4 here and in SYSCTRL2 there. aw88261.c was the model
// for structure; every register and field below came from the hardware's own
// documentation as carried by the vendor driver.

#include <linux/i2c.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>

#include "aw88264.h"

struct aw88264 {
	struct regmap *regmap;
	struct device *dev;
	struct gpio_desc *reset_gpio;
	/* volume in 0.5 dB steps of attenuation; 0 is loudest */
	unsigned int attenuation;
};

static const struct regmap_config aw88264_regmap_config = {
	.reg_bits = 8,
	.val_bits = 16,
	.max_register = AW88264_REG_MAX,
	.reg_format_endian = REGMAP_ENDIAN_LITTLE,
	.val_format_endian = REGMAP_ENDIAN_BIG,
};

/*
 * Volume. The register field is two nibbles -- [7:4] at -6 dB a step and
 * [3:0] at -0.5 dB -- so the driver keeps a plain count of 0.5 dB steps and
 * packs it here. 191 steps is the most the field can express, -95.5 dB.
 */
static unsigned int aw88264_att_to_reg(unsigned int att)
{
	unsigned int coarse, fine;

	att = min_t(unsigned int, att, AW88264_VOL_MAX_STEPS);
	coarse = att / AW88264_VOL_STEPS_PER_6DB;
	fine = att % AW88264_VOL_STEPS_PER_6DB;

	/*
	 * Belt and braces: a coarse term that does not fit silently truncates
	 * to a *louder* setting than was asked for, so clamp rather than trust
	 * the constant above to stay correct.
	 */
	if (coarse > AW88264_VOL_FINE_MASK)
		coarse = AW88264_VOL_FINE_MASK;

	return (coarse << AW88264_VOL_COARSE_SHIFT) | fine;
}

static unsigned int aw88264_reg_to_att(unsigned int val)
{
	unsigned int att = ((val >> AW88264_VOL_COARSE_SHIFT) *
			    AW88264_VOL_STEPS_PER_6DB) +
			   (val & AW88264_VOL_FINE_MASK);

	return min_t(unsigned int, att, AW88264_VOL_MAX_STEPS);
}

static void aw88264_set_attenuation(struct aw88264 *aw88264, unsigned int att)
{
	regmap_update_bits(aw88264->regmap, AW88264_HAGCCFG4_REG,
			   ~AW88264_VOL_MASK,
			   aw88264_att_to_reg(att) << AW88264_VOL_START_BIT);
}

/*
 * The ALSA control runs the other way round from the register: higher means
 * louder, which is what every mixer expects and what alsamixer draws.
 */
static int aw88264_volume_get(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct aw88264 *aw88264 = snd_soc_component_get_drvdata(component);
	unsigned int val, att;
	int ret;

	/*
	 * Read the part rather than report what we last wrote. They should
	 * agree, and if they ever do not it is worth the control saying so
	 * instead of the driver insisting.
	 */
	ret = regmap_read(aw88264->regmap, AW88264_HAGCCFG4_REG, &val);
	if (ret)
		return ret;

	att = aw88264_reg_to_att((val >> AW88264_VOL_START_BIT) & 0xff);
	att = min_t(unsigned int, att, AW88264_VOL_MAX_STEPS);

	ucontrol->value.integer.value[0] = AW88264_VOL_MAX_STEPS - att;

	return 0;
}

static int aw88264_volume_put(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct aw88264 *aw88264 = snd_soc_component_get_drvdata(component);
	unsigned int value = ucontrol->value.integer.value[0];
	unsigned int att;

	if (value > AW88264_VOL_MAX_STEPS)
		return -EINVAL;

	att = AW88264_VOL_MAX_STEPS - value;
	if (att == aw88264->attenuation)
		return 0;

	aw88264->attenuation = att;
	aw88264_set_attenuation(aw88264, att);

	return 1;
}

static const DECLARE_TLV_DB_SCALE(aw88264_volume_tlv, -9750, 50, 0);

static const struct snd_kcontrol_new aw88264_controls[] = {
	SOC_SINGLE_EXT_TLV("Speaker Volume", AW88264_HAGCCFG4_REG,
			   AW88264_VOL_START_BIT, AW88264_VOL_MAX_STEPS, 0,
			   aw88264_volume_get, aw88264_volume_put,
			   aw88264_volume_tlv),
};

static void aw88264_power(struct aw88264 *aw88264, bool on)
{
	if (on) {
		regmap_update_bits(aw88264->regmap, AW88264_SYSCTRL_REG,
				   ~AW88264_PWDN_MASK,
				   AW88264_PWDN_NORMAL_WORKING << AW88264_PWDN_START_BIT);
		regmap_update_bits(aw88264->regmap, AW88264_SYSCTRL_REG,
				   ~AW88264_AMPPD_MASK,
				   AW88264_AMPPD_NORMAL_WORKING << AW88264_AMPPD_START_BIT);
		/* and let it listen to the I2S link, which is a separate bit */
		regmap_update_bits(aw88264->regmap, AW88264_SYSCTRL_REG,
				   ~AW88264_I2SEN_MASK,
				   AW88264_I2SEN_ENABLE << AW88264_I2SEN_START_BIT);
	} else {
		/* amplifier first, then the rest: the other order pops */
		regmap_update_bits(aw88264->regmap, AW88264_SYSCTRL_REG,
				   ~AW88264_I2SEN_MASK,
				   AW88264_I2SEN_DISABLE << AW88264_I2SEN_START_BIT);
		regmap_update_bits(aw88264->regmap, AW88264_SYSCTRL_REG,
				   ~AW88264_AMPPD_MASK,
				   AW88264_AMPPD_POWER_DOWN << AW88264_AMPPD_START_BIT);
		regmap_update_bits(aw88264->regmap, AW88264_SYSCTRL_REG,
				   ~AW88264_PWDN_MASK,
				   AW88264_PWDN_POWER_DOWN << AW88264_PWDN_START_BIT);
	}
}

static void aw88264_hw_mute(struct aw88264 *aw88264, bool mute)
{
	regmap_update_bits(aw88264->regmap, AW88264_SYSCTRL2_REG,
			   ~AW88264_HMUTE_MASK,
			   (mute ? AW88264_HMUTE_ENABLE : AW88264_HMUTE_DISABLE)
				<< AW88264_HMUTE_START_BIT);
}

static int aw88264_mute_stream(struct snd_soc_dai *dai, int mute, int stream)
{
	struct aw88264 *aw88264 = snd_soc_component_get_drvdata(dai->component);

	if (stream != SNDRV_PCM_STREAM_PLAYBACK)
		return 0;

	if (mute) {
		aw88264_hw_mute(aw88264, true);
		aw88264_power(aw88264, false);
	} else {
		aw88264_power(aw88264, true);
		aw88264_hw_mute(aw88264, false);
	}

	return 0;
}

static int aw88264_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct aw88264 *aw88264 = snd_soc_component_get_drvdata(dai->component);
	unsigned int mode;

	/*
	 * The part is always a clock consumer: it has no PLL of its own to
	 * drive the bus with, and the vendor driver never configures one.
	 */
	if ((fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) != SND_SOC_DAIFMT_CBC_CFC) {
		dev_err(aw88264->dev, "only clock consumer mode is supported\n");
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		mode = AW88264_I2SMD_STANDARD_I2S;
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		mode = AW88264_I2SMD_MSB_JUSTIFIED;
		break;
	case SND_SOC_DAIFMT_RIGHT_J:
		mode = AW88264_I2SMD_LSB_JUSTIFIED;
		break;
	default:
		dev_err(aw88264->dev, "unsupported DAI format %d\n",
			fmt & SND_SOC_DAIFMT_FORMAT_MASK);
		return -EINVAL;
	}

	regmap_update_bits(aw88264->regmap, AW88264_I2SCTRL_REG,
			   ~AW88264_I2SMD_MASK, mode << AW88264_I2SMD_START_BIT);

	return 0;
}

static int aw88264_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params,
			     struct snd_soc_dai *dai)
{
	struct aw88264 *aw88264 = snd_soc_component_get_drvdata(dai->component);
	unsigned int rate_val, cco_mux, width_val;

	if (substream->stream != SNDRV_PCM_STREAM_PLAYBACK)
		return 0;

	switch (params_rate(params)) {
	case 8000:
		rate_val = AW88264_I2SSR_8KHZ;
		break;
	case 11025:
		rate_val = AW88264_I2SSR_11P025KHZ;
		break;
	case 12000:
		rate_val = AW88264_I2SSR_12KHZ;
		break;
	case 16000:
		rate_val = AW88264_I2SSR_16KHZ;
		break;
	case 22050:
		rate_val = AW88264_I2SSR_22P05KHZ;
		break;
	case 24000:
		rate_val = AW88264_I2SSR_24KHZ;
		break;
	case 32000:
		rate_val = AW88264_I2SSR_32KHZ;
		break;
	case 44100:
		rate_val = AW88264_I2SSR_44P1KHZ;
		break;
	case 48000:
		rate_val = AW88264_I2SSR_48KHZ;
		break;
	case 96000:
		rate_val = AW88264_I2SSR_96KHZ;
		break;
	case 192000:
		rate_val = AW88264_I2SSR_192KHZ;
		break;
	default:
		dev_err(aw88264->dev, "unsupported rate %d\n", params_rate(params));
		return -EINVAL;
	}

	switch (params_rate(params)) {
	case 8000:
	case 16000:
	case 32000:
		cco_mux = AW88264_I2S_CCO_MUX_8_16_32KHZ;
		break;
	default:
		cco_mux = AW88264_I2S_CCO_MUX_OTHER;
		break;
	}

	switch (params_width(params)) {
	case 16:
		width_val = AW88264_I2SFS_16_BITS;
		break;
	case 20:
		width_val = AW88264_I2SFS_20_BITS;
		break;
	case 24:
		width_val = AW88264_I2SFS_24_BITS;
		break;
	case 32:
		width_val = AW88264_I2SFS_32_BITS;
		break;
	default:
		dev_err(aw88264->dev, "unsupported sample width %d\n",
			params_width(params));
		return -EINVAL;
	}

	regmap_update_bits(aw88264->regmap, AW88264_I2SCTRL_REG,
			   ~AW88264_I2S_CCO_MUX_MASK,
			   cco_mux << AW88264_I2S_CCO_MUX_START_BIT);
	regmap_update_bits(aw88264->regmap, AW88264_I2SCTRL_REG,
			   ~AW88264_I2SSR_MASK, rate_val << AW88264_I2SSR_START_BIT);
	regmap_update_bits(aw88264->regmap, AW88264_I2SCTRL_REG,
			   ~AW88264_I2SFS_MASK, width_val << AW88264_I2SFS_START_BIT);

	return 0;
}

static const struct snd_soc_dai_ops aw88264_dai_ops = {
	.hw_params = aw88264_hw_params,
	.set_fmt = aw88264_set_fmt,
	.mute_stream = aw88264_mute_stream,
	.no_capture_mute = 1,
};

#define AW88264_RATES	(SNDRV_PCM_RATE_8000_48000 | \
			 SNDRV_PCM_RATE_96000 | SNDRV_PCM_RATE_192000)
#define AW88264_FORMATS	(SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE | \
			 SNDRV_PCM_FMTBIT_S32_LE)

static struct snd_soc_dai_driver aw88264_dai[] = {
	{
		.name = "aw88264-aif",
		.playback = {
			.stream_name = "Speaker Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = AW88264_RATES,
			.formats = AW88264_FORMATS,
		},
		.ops = &aw88264_dai_ops,
	},
};

static const struct snd_soc_dapm_widget aw88264_dapm_widgets[] = {
	SND_SOC_DAPM_AIF_IN("AIF_RX", "Speaker Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_OUTPUT("OUT"),
};

static const struct snd_soc_dapm_route aw88264_dapm_routes[] = {
	{ "OUT", NULL, "AIF_RX" },
};

static int aw88264_component_probe(struct snd_soc_component *component)
{
	struct aw88264 *aw88264 = snd_soc_component_get_drvdata(component);

	/* Start muted and powered down; DAPM brings it up when a stream runs. */
	aw88264_hw_mute(aw88264, true);
	aw88264_power(aw88264, false);
	aw88264_set_attenuation(aw88264, aw88264->attenuation);

	return 0;
}

static const struct snd_soc_component_driver soc_component_aw88264 = {
	.probe			= aw88264_component_probe,
	.controls		= aw88264_controls,
	.num_controls		= ARRAY_SIZE(aw88264_controls),
	.dapm_widgets		= aw88264_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(aw88264_dapm_widgets),
	.dapm_routes		= aw88264_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(aw88264_dapm_routes),
	.idle_bias_on		= 1,
	.endianness		= 1,
};

static int aw88264_check_chipid(struct aw88264 *aw88264)
{
	unsigned int chip_id;
	int ret;

	ret = regmap_read(aw88264->regmap, AW88264_ID_REG, &chip_id);
	if (ret)
		return dev_err_probe(aw88264->dev, ret, "failed to read chip id\n");

	if (chip_id != AW88264_CHIP_ID)
		return dev_err_probe(aw88264->dev, -ENODEV,
				     "unsupported chip id 0x%04x, expected 0x%04x\n",
				     chip_id, AW88264_CHIP_ID);

	return 0;
}

static int aw88264_i2c_probe(struct i2c_client *i2c)
{
	struct aw88264 *aw88264;
	int ret;

	aw88264 = devm_kzalloc(&i2c->dev, sizeof(*aw88264), GFP_KERNEL);
	if (!aw88264)
		return -ENOMEM;

	aw88264->dev = &i2c->dev;
	/* quiet until something asks otherwise */
	aw88264->attenuation = AW88264_VOL_MAX_STEPS;
	i2c_set_clientdata(i2c, aw88264);

	aw88264->regmap = devm_regmap_init_i2c(i2c, &aw88264_regmap_config);
	if (IS_ERR(aw88264->regmap))
		return dev_err_probe(&i2c->dev, PTR_ERR(aw88264->regmap),
				     "failed to init regmap\n");

	aw88264->reset_gpio = devm_gpiod_get_optional(&i2c->dev, "reset",
						      GPIOD_OUT_LOW);
	if (IS_ERR(aw88264->reset_gpio))
		return dev_err_probe(&i2c->dev, PTR_ERR(aw88264->reset_gpio),
				     "failed to get reset gpio\n");

	if (aw88264->reset_gpio) {
		gpiod_set_value_cansleep(aw88264->reset_gpio, 1);
		usleep_range(2000, 2500);
		gpiod_set_value_cansleep(aw88264->reset_gpio, 0);
		usleep_range(2000, 2500);
	}

	ret = aw88264_check_chipid(aw88264);
	if (ret)
		return ret;

	return devm_snd_soc_register_component(&i2c->dev, &soc_component_aw88264,
					       aw88264_dai, ARRAY_SIZE(aw88264_dai));
}

static const struct i2c_device_id aw88264_i2c_id[] = {
	{ "aw88264" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, aw88264_i2c_id);

static const struct of_device_id aw88264_of_match[] = {
	{ .compatible = "awinic,aw88264" },
	{ }
};
MODULE_DEVICE_TABLE(of, aw88264_of_match);

static struct i2c_driver aw88264_i2c_driver = {
	.driver = {
		.name = "aw88264",
		.of_match_table = aw88264_of_match,
	},
	.probe = aw88264_i2c_probe,
	.id_table = aw88264_i2c_id,
};
module_i2c_driver(aw88264_i2c_driver);

MODULE_AUTHOR("Tim auf der Landwehr <tadl-git@taufderl.de>");
MODULE_DESCRIPTION("ASoC AW88264 driver");
MODULE_LICENSE("GPL");
