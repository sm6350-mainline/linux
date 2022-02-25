// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2022 Luca Weiss <luca.weiss@fairphone.com>
 *
 * Partially based on vendor driver:
 *   Copyright (c) 2018 AWINIC Technology CO., LTD
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/regmap.h>

#define AW8695_CHIPID			0x95
#define AW8695_RESET			0xaa
/* Default of BASE_ADDR* registers */
#define AW8695_RAM_BASE_ADDR		0x800

#define AW8695_HIGH_MASK		GENMASK(15, 8)
#define AW8695_LOW_MASK			GENMASK(7, 0)

/* Chip ID */
#define AW8695_ID				0x00

/* System Status */
#define AW8695_SYSST				0x01

/* System Interrupt */
#define AW8695_SYSINT				0x02
#define AW8695_SYSINT_BSTERRI			BIT(7)
#define AW8695_SYSINT_OVI			BIT(6)
#define AW8695_SYSINT_UVLI			BIT(5)
#define AW8695_SYSINT_FF_AEI			BIT(4)
#define AW8695_SYSINT_FF_AFI			BIT(3)
#define AW8695_SYSINT_OCDI			BIT(2)
#define AW8695_SYSINT_OTI			BIT(1)
#define AW8695_SYSINT_DONEI			BIT(0)

/* System Interrupt Mask */
#define AW8695_SYSINTM				0x03
#define AW8695_SYSINTM_BSTERR_OFF		BIT(7)
#define AW8695_SYSINTM_OV_OFF			BIT(6)
#define AW8695_SYSINTM_UVLO_OFF			BIT(5)
#define AW8695_SYSINTM_OCD_OFF			BIT(2)
#define AW8695_SYSINTM_OT_OFF			BIT(1)

/* System Control */
#define AW8695_SYSCTRL				0x04
#define AW8695_SYSCTRL_RAMINIT_EN		BIT(5)

#define AW8695_SYSCTRL_PLAY_MODE_MASK		GENMASK(3, 2)
#define AW8695_SYSCTRL_PLAY_MODE_CONT		(2)
#define AW8695_SYSCTRL_PLAY_MODE_RAM		(0)

#define AW8695_SYSCTRL_BST_MODE_MASK		GENMASK(1, 1)
#define AW8695_SYSCTRL_BST_MODE_BOOST		(1)
#define AW8695_SYSCTRL_BST_MODE_BYPASS		(0)

#define AW8695_SYSCTRL_WORK_MODE_MASK		GENMASK(0, 0)
#define AW8695_SYSCTRL_STANDBY			(1)
#define AW8695_SYSCTRL_ACTIVE			(0)

/* Process Control */
#define AW8695_GO				0x05
#define AW8695_GO_ENABLE			BIT(0)

/* Waveform #1 */
#define AW8695_WAVSEQ1				0x07

/* Waveform #2 */
#define AW8695_WAVSEQ2				0x08

/* Waveform Loop #1 */
#define AW8695_WAVLOOP1				0x0f
#define AW8695_WAVLOOP1_SEQ1_MASK		GENMASK(7, 4)
#define AW8695_WAVLOOP1_SEQ2_MASK		GENMASK(3, 0)

#define AW8695_WAVLOOP_INFINITE			0xf

/* Debug Control */
#define AW8695_DBGCTRL				0x20
#define AW8695_DBGCTRL_INT_MODE_MASK		GENMASK(2, 2)
#define AW8695_DBGCTRL_INT_MODE_EDGE		(1)

/* High Five Bits of Wave SRAM */
#define AW8695_BASE_ADDRH			0x21

/* Low Eight Bits of Wave SRAM */
#define AW8695_BASE_ADDRL			0x22

/* Global Control Data */
#define AW8695_DATCTRL				0x2b
#define AW8695_DATCTRL_FC_MASK			GENMASK(6, 6)
#define AW8695_DATCTRL_FC_1000HZ		(1)

#define AW8695_DATCTRL_LPF_ENABLE_MASK		GENMASK(5, 5)
#define AW8695_DATCTRL_LPF_ENABLE		(1)

/* PWM Output Protect Configuration */
#define AW8695_PWMPRC				0x2d
#define AW8695_PWMPRC_PRC_ENABLE		BIT(7)

/* PWM Debug */
#define AW8695_PWMDBG				0x2e
#define AW8695_PWMDBG_PWM_MODE_MASK		GENMASK(6, 5)
#define AW8695_PWMDBG_PWM_24K			(2)

/* Debug Status */
#define AW8695_DBGSTAT				0x30

/* Boost Debug #1 */
#define AW8695_BSTDBG1				0x31

/* Boost Debug #2 */
#define AW8695_BSTDBG2				0x32

/* Boost Debug #3 */
#define AW8695_BSTDBG3				0x33

/* Boost Config */
#define AW8695_BSTCFG				0x34
#define AW8695_BSTCFG_PEAKCUR_MASK		GENMASK(2, 0)
#define AW8695_BSTCFG_PEAKCUR_2A		(1)

#define AW8695_ANADBG				0x35
#define AW8695_ANADBG_IOC_MASK			GENMASK(3, 2)
#define AW8695_ANADBG_IOC_4P65A			(3)

#define AW8695_ANACTRL				0x36
#define AW8695_ANACTRL_LRA_SRC_MASK		GENMASK(5, 5)
#define AW8695_ANACTRL_LRA_SRC_REG		(1)

/* Waveform Protect Level */
#define AW8695_PRLVL				0x3e
#define AW8695_PRLVL_PR_ENABLE			BIT(7)

/* SRAM Address 0xhigh */
#define AW8695_RAMADDRH				0x40

/* SRAM Address 0xlow */
#define AW8695_RAMADDRL				0x41

/* SRAM Data */
#define AW8695_RAMDATA				0x42

#define AW8695_GLB_STATE			0x46

#define AW8695_BST_AUTO				0x47
#define AW8695_BST_AUTO_BST_AUTOSW_MASK		GENMASK(2, 2)
#define AW8695_BST_AUTO_BST_MANUAL_BOOST	(0)

/* CONT Mode Control */
#define AW8695_CONT_CTRL			0x48
#define AW8695_CONT_CTRL_EN_CLOSE_MASK		GENMASK(3, 3)
#define AW8695_CONT_CTRL_CLOSE_PLAYBACK		(1)
#define AW8695_CONT_CTRL_OPEN_PLAYBACK		(0)
#define AW8695_CONT_CTRL_F0_DETECT_ENABLE	BIT(2)

/* High 8 Bits Pre Setting f0 Value */
#define AW8695_F_PRE_H				0x49

/* Low 8 Bits Pre Setting f0 Value */
#define AW8695_F_PRE_L				0x4a

#define AW8695_TSET				0x4d

#define AW8695_TRIM_LRA				0x5b

#define AW8695_R_SPARE				0x5d

/* Detection Control */
#define AW8695_DETCTRL				0x5f
#define AW8695_DETCTRL_PROTECT_MASK		GENMASK(5, 5)
#define AW8695_DETCTRL_PROTECT_NO_ACTION	(1)
#define AW8695_DETCTRL_DIAG_GO_ENABLE		BIT(0)

/* ADC Test */
#define AW8695_ADCTEST				0x66
#define AW8695_ADCTEST_VBAT_MODE_MASK		GENMASK(6, 6)
#define AW8695_ADCTEST_VBAT_HW_COMP		(1)

/* High 8 Bits Detected f0 Value */
#define AW8695_F_LRA_F0_H			0x68

/* Low 8 Bits Detected f0 Value */
#define AW8695_F_LRA_F0_L			0x69

#define AW8695_BEMF_VTHH_H			0x74

#define AW8695_BEMF_VTHH_L			0x75

#define AW8695_BEMF_VTHL_H			0x76

#define AW8695_BEMF_VTHL_L			0x77

/* Drive Level Setting */
#define AW8695_DRV_LVL				0x7b

/* Number Configuration for F0 Trace #1 */
#define AW8695_NUM_F0_1				0x7d
#define AW8695_NUM_F0_1_PRE_MASK		GENMASK(7, 4)
#define AW8695_NUM_F0_1_WAIT_MASK		GENMASK(3, 0)

/* Number Configuration for F0 Trace #2 */
#define AW8695_NUM_F0_2				0x7e

/* Number Configuration for F0 Trace #3 */
#define AW8695_NUM_F0_3				0x7f

#define AW8695_MAX_REG				0x7f

enum aw8695_work_mode {
	AW8695_STANDBY_MODE,
	AW8695_RAM_MODE,
	AW8695_CONT_MODE,
};

struct aw8695_data {
	struct input_dev *input_dev;
	struct i2c_client *client;
	struct regmap *regmap;
	struct gpio_desc *reset_gpio;
	bool running;
	struct work_struct play_work;
	/* Parameters from devicetree */
	u32 f0_preset;
	u32 f0_coefficient;
	u32 f0_cali_percent;
	u32 drive_level;
	u32 f0_det_play;
	u32 f0_det_wait;
	u32 f0_det_repeat;
	u32 f0_det_trace;
	u8 boost_debug[3];
	u8 tset;
	u8 r_spare;
	u32 bemf_vthh;
	u32 bemf_vthl;
};

/*
 * Sine wave representing the magnitude of the drive to be used.
 * Data is encoded in two's complement.
 *   round(84 * sin(x / 16.25))
 */
static const u8 aw8695_sine_waveform[] = {
	0x00, 0x05, 0x0a, 0x0f, 0x14, 0x19, 0x1e, 0x23, 0x28, 0x2c, 0x30, 0x35,
	0x39, 0x3c, 0x40, 0x43, 0x46, 0x49, 0x4b, 0x4d, 0x4f, 0x51, 0x52, 0x53,
	0x54, 0x54, 0x54, 0x54, 0x53, 0x52, 0x51, 0x4f, 0x4d, 0x4b, 0x49, 0x46,
	0x43, 0x40, 0x3c, 0x39, 0x35, 0x31, 0x2c, 0x28, 0x23, 0x1f, 0x1a, 0x15,
	0x10, 0x0b, 0x05, 0x00, 0xfb, 0xf6, 0xf1, 0xec, 0xe7, 0xe2, 0xdd, 0xd9,
	0xd4, 0xd0, 0xcc, 0xc8, 0xc4, 0xc0, 0xbd, 0xba, 0xb7, 0xb5, 0xb3, 0xb1,
	0xaf, 0xae, 0xad, 0xac, 0xac, 0xac, 0xac, 0xad, 0xae, 0xaf, 0xb1, 0xb2,
	0xb5, 0xb7, 0xba, 0xbd, 0xc0, 0xc3, 0xc7, 0xcb, 0xcf, 0xd3, 0xd8, 0xdc,
	0xe1, 0xe6, 0xeb, 0xf0, 0xf5, 0xfa
};

/*
 * Header that gets written to AW8695 SRAM that describes the available
 * waveforms being transferred afterwards.
 *
 * @version: waveform library version
 * @start_address: start address of waveform in SRAM
 * @end_address: end address of waveform in SRAM
 */
struct aw8695_sram_waveform_header {
	u8 version;
	struct {
		__be16 start_address;
		__be16 end_address;
	} __packed waveform_address[1];
} __packed;

static const struct aw8695_sram_waveform_header sram_waveform_header = {
	.version = 0x01,
	.waveform_address = {
		/* Simple sine wave defined above */
		{
			.start_address = cpu_to_be16(AW8695_RAM_BASE_ADDR +
				sizeof(struct aw8695_sram_waveform_header)),
			.end_address = cpu_to_be16(AW8695_RAM_BASE_ADDR +
				sizeof(struct aw8695_sram_waveform_header) +
				ARRAY_SIZE(aw8695_sine_waveform) - 1),
		}
	}
};

static int aw8695_interrupt_clear(struct aw8695_data *haptics)
{
	unsigned int read_buf;

	/* Clear UVLI bit by reading register */
	return regmap_read(haptics->regmap, AW8695_SYSINT, &read_buf);
}

static int aw8695_haptic_set_active(struct aw8695_data *haptics)
{
	int err;

	err = regmap_update_bits(haptics->regmap, AW8695_SYSCTRL,
				 AW8695_SYSCTRL_WORK_MODE_MASK,
				 FIELD_PREP(AW8695_SYSCTRL_WORK_MODE_MASK,
					    AW8695_SYSCTRL_ACTIVE));
	if (err)
		return err;

	err = aw8695_interrupt_clear(haptics);
	if (err)
		return err;

	return regmap_update_bits(haptics->regmap, AW8695_SYSINTM,
				  AW8695_SYSINTM_UVLO_OFF, 0);
}

static int aw8695_set_work_mode(struct aw8695_data *haptics,
				 enum aw8695_work_mode mode)
{
	struct device *dev = &haptics->client->dev;
	int err;

	switch (mode) {
	case AW8695_STANDBY_MODE:
		err = regmap_update_bits(haptics->regmap, AW8695_SYSINTM,
					 AW8695_SYSINTM_UVLO_OFF, AW8695_SYSINTM_UVLO_OFF);
		if (err)
			return err;

		return regmap_update_bits(haptics->regmap, AW8695_SYSCTRL,
					  AW8695_SYSCTRL_WORK_MODE_MASK,
					  FIELD_PREP(AW8695_SYSCTRL_WORK_MODE_MASK,
						     AW8695_SYSCTRL_STANDBY));
	case AW8695_RAM_MODE:
		err = regmap_update_bits(haptics->regmap, AW8695_SYSCTRL,
					 AW8695_SYSCTRL_PLAY_MODE_MASK,
					 FIELD_PREP(AW8695_SYSCTRL_PLAY_MODE_MASK,
						    AW8695_SYSCTRL_PLAY_MODE_RAM));
		if (err)
			return err;

		err = regmap_update_bits(haptics->regmap, AW8695_SYSCTRL,
					 AW8695_SYSCTRL_BST_MODE_MASK,
					 FIELD_PREP(AW8695_SYSCTRL_BST_MODE_MASK,
						    AW8695_SYSCTRL_BST_MODE_BYPASS));
		if (err)
			return err;

		return aw8695_haptic_set_active(haptics);
	case AW8695_CONT_MODE:
		err = regmap_update_bits(haptics->regmap, AW8695_SYSCTRL,
					 AW8695_SYSCTRL_PLAY_MODE_MASK,
					 FIELD_PREP(AW8695_SYSCTRL_PLAY_MODE_MASK,
						    AW8695_SYSCTRL_PLAY_MODE_CONT));
		if (err)
			return err;

		err = regmap_update_bits(haptics->regmap, AW8695_SYSCTRL,
					 AW8695_SYSCTRL_BST_MODE_MASK,
					 FIELD_PREP(AW8695_SYSCTRL_BST_MODE_MASK,
						    AW8695_SYSCTRL_BST_MODE_BYPASS));
		if (err)
			return err;

		return aw8695_haptic_set_active(haptics);
	default:
		dev_err(dev, "Unhandled mode: %d\n", mode);
		return -EINVAL;
	}
}

static int aw8695_haptics_play(struct input_dev *dev, void *data,
			       struct ff_effect *effect)
{
	struct aw8695_data *haptics = input_get_drvdata(dev);
	int level;

	level = effect->u.rumble.strong_magnitude;
	if (!level)
		level = effect->u.rumble.weak_magnitude;

	if (haptics->running && level)
		return 0;

	haptics->running = level;
	schedule_work(&haptics->play_work);

	return 0;
}

static int aw8695_haptics_stop(struct aw8695_data *haptics)
{
	int err;
	unsigned int read_buf;
	struct device *dev = &haptics->client->dev;

	err = regmap_update_bits(haptics->regmap, AW8695_GO,
				 AW8695_GO_ENABLE, 0);
	if (err)
		return err;

	err = regmap_read_poll_timeout(haptics->regmap, AW8695_GLB_STATE, read_buf,
			(read_buf & 0x0f) == 0, 2000, 2000 * 100);
	if (err) {
		dev_err(dev, "Did not enter standby: %d\n", err);
		return err;
	}

	return aw8695_set_work_mode(haptics, AW8695_STANDBY_MODE);
}

static int aw8695_haptics_start(struct aw8695_data *haptics)
{
	int err;

	err = aw8695_haptics_stop(haptics);
	if (err)
		return err;

	/*
	 * Configure for waveform #1 to be played infinitely,
	 * and waveform #2 to not be played.
	 */
	err = regmap_write(haptics->regmap, AW8695_WAVSEQ1, 0x1);
	if (err)
		return err;

	err = regmap_write(haptics->regmap, AW8695_WAVSEQ2, 0x0);
	if (err)
		return err;

	err = regmap_write(haptics->regmap, AW8695_WAVLOOP1,
			   FIELD_PREP(AW8695_WAVLOOP1_SEQ1_MASK,
				      AW8695_WAVLOOP_INFINITE) |
			   FIELD_PREP(AW8695_WAVLOOP1_SEQ2_MASK, 0));
	if (err)
		return err;

	/* Configure for RAM mode */
	err = aw8695_set_work_mode(haptics, AW8695_RAM_MODE);
	if (err)
		return err;

	/* Start vibration */
	return regmap_update_bits(haptics->regmap, AW8695_GO,
				  AW8695_GO_ENABLE, AW8695_GO_ENABLE);
}

static void aw8695_close(struct input_dev *input)
{
	struct aw8695_data *haptics = input_get_drvdata(input);
	struct device *dev = &haptics->client->dev;
	int err;

	cancel_work_sync(&haptics->play_work);
	err = aw8695_haptics_stop(haptics);
	if (err)
		dev_err(dev, "Failed to stop haptics: %d\n", err);
}

static void aw8695_haptics_play_work(struct work_struct *work)
{
	struct aw8695_data *haptics =
		container_of(work, struct aw8695_data, play_work);
	struct device *dev = &haptics->client->dev;
	int err;

	if (haptics->running)
		err = aw8695_haptics_start(haptics);
	else
		err = aw8695_haptics_stop(haptics);

	if (err)
		dev_err(dev, "Failed to execute work command: %d\n", err);
}

static void aw8695_hw_reset(struct aw8695_data *haptics)
{
	gpiod_set_value_cansleep(haptics->reset_gpio, 1);

	usleep_range(1000, 2000);

	gpiod_set_value_cansleep(haptics->reset_gpio, 0);

	usleep_range(3500, 4000);
}

static int aw8695_haptic_offset_calibration(struct aw8695_data *haptics)
{
	unsigned int read_buf;
	int err;

	err = regmap_update_bits(haptics->regmap, AW8695_SYSCTRL,
				 AW8695_SYSCTRL_RAMINIT_EN,
				 AW8695_SYSCTRL_RAMINIT_EN);
	if (err)
		return err;

	err = regmap_update_bits(haptics->regmap, AW8695_DETCTRL,
				 AW8695_DETCTRL_DIAG_GO_ENABLE,
				 AW8695_DETCTRL_DIAG_GO_ENABLE);
	if (err)
		return err;

	err = regmap_read_poll_timeout(haptics->regmap, AW8695_DETCTRL, read_buf,
			(read_buf & AW8695_DETCTRL_DIAG_GO_ENABLE) == 0, 10000, 10000 * 50);
	if (err)
		return err;

	return regmap_update_bits(haptics->regmap, AW8695_SYSCTRL,
				  AW8695_SYSCTRL_RAMINIT_EN, 0);
}

static int aw8695_haptic_read_f0(struct aw8695_data *haptics)
{
	struct device *dev = &haptics->client->dev;
	unsigned long f0;
	unsigned int f0_reg;
	unsigned int read_buf;
	int err;

	err = regmap_read(haptics->regmap, AW8695_F_LRA_F0_H, &read_buf);
	if (err)
		return err;
	f0_reg = FIELD_PREP(AW8695_HIGH_MASK, read_buf);

	err = regmap_read(haptics->regmap, AW8695_F_LRA_F0_L, &read_buf);
	if (err)
		return err;
	f0_reg |= FIELD_PREP(AW8695_LOW_MASK, read_buf);

	if (!f0_reg) {
		dev_err(dev, "Failed to read f0 value!\n");
		return -EINVAL;
	}

	f0 = 1000000000 / (f0_reg * haptics->f0_coefficient);
	dev_dbg(dev, "Read new f0: %d\n", (int)f0);

	return (int)f0;
}

static int aw8695_haptic_get_f0(struct aw8695_data *haptics)
{
	struct device *dev = &haptics->client->dev;
	unsigned int read_buf;
	unsigned int f0_trace_ms;
	unsigned int f0_reg;
	int f0;
	int err;

	err = aw8695_haptics_stop(haptics);
	if (err)
		return err;

	err = regmap_write(haptics->regmap, AW8695_TRIM_LRA, 0x00);
	if (err)
		return err;

	err = aw8695_set_work_mode(haptics, AW8695_CONT_MODE);
	if (err)
		return err;

	err = regmap_update_bits(haptics->regmap, AW8695_CONT_CTRL,
				 AW8695_CONT_CTRL_EN_CLOSE_MASK,
				 FIELD_PREP(AW8695_CONT_CTRL_EN_CLOSE_MASK,
					    AW8695_CONT_CTRL_OPEN_PLAYBACK));
	if (err)
		return err;

	err = regmap_update_bits(haptics->regmap, AW8695_CONT_CTRL,
				 AW8695_CONT_CTRL_F0_DETECT_ENABLE,
				 AW8695_CONT_CTRL_F0_DETECT_ENABLE);
	if (err)
		return err;

	/* LPF */
	err = regmap_update_bits(haptics->regmap, AW8695_DATCTRL,
				 AW8695_DATCTRL_FC_MASK | AW8695_DATCTRL_LPF_ENABLE_MASK,
				 FIELD_PREP(AW8695_DATCTRL_FC_MASK,
					    AW8695_DATCTRL_FC_1000HZ) |
				 FIELD_PREP(AW8695_DATCTRL_LPF_ENABLE_MASK,
					    AW8695_DATCTRL_LPF_ENABLE));
	if (err)
		return err;

	/* LRA OSC Source */
	err = regmap_update_bits(haptics->regmap, AW8695_ANACTRL,
				 AW8695_ANACTRL_LRA_SRC_MASK,
				 FIELD_PREP(AW8695_ANACTRL_LRA_SRC_MASK,
					    AW8695_ANACTRL_LRA_SRC_REG));
	if (err)
		return err;

	/* preset f0 */
	f0_reg = 1000000000 / (haptics->f0_preset * haptics->f0_coefficient);
	err = regmap_write(haptics->regmap, AW8695_F_PRE_H,
			   FIELD_GET(AW8695_HIGH_MASK, f0_reg));
	if (err)
		return err;
	err = regmap_write(haptics->regmap, AW8695_F_PRE_L,
			   FIELD_GET(AW8695_LOW_MASK, f0_reg));
	if (err)
		return err;

	/* f0 driver level */
	err = regmap_write(haptics->regmap, AW8695_DRV_LVL, haptics->drive_level);
	if (err)
		return err;

	/* f0 trace parameter */
	err = regmap_write(haptics->regmap, AW8695_NUM_F0_1,
			FIELD_PREP(AW8695_NUM_F0_1_PRE_MASK, haptics->f0_det_play) |
			FIELD_PREP(AW8695_NUM_F0_1_WAIT_MASK, haptics->f0_det_wait));
	if (err)
		return err;
	err = regmap_write(haptics->regmap, AW8695_NUM_F0_2, haptics->f0_det_repeat);
	if (err)
		return err;
	err = regmap_write(haptics->regmap, AW8695_NUM_F0_3, haptics->f0_det_trace);
	if (err)
		return err;

	err = aw8695_interrupt_clear(haptics);
	if (err)
		return err;

	/* play go and start f0 calibration */
	err = regmap_update_bits(haptics->regmap, AW8695_GO,
				 AW8695_GO_ENABLE, AW8695_GO_ENABLE);
	if (err)
		return err;

	/* f0 trace time */
	f0_trace_ms =
		(1000 * 10 / haptics->f0_preset) * (haptics->f0_det_play + haptics->f0_det_wait +
		(haptics->f0_det_trace + haptics->f0_det_wait) * (haptics->f0_det_repeat - 1));
	usleep_range(f0_trace_ms * 1000, f0_trace_ms * 1000 + 500);

	err = regmap_read_poll_timeout(haptics->regmap, AW8695_GLB_STATE, read_buf,
			(read_buf & 0x0f) == 0, 10000, 10000 * 50);
	if (err) {
		dev_err(dev, "Did not enter standby: %d\n", err);
		return err;
	}

	f0 = aw8695_haptic_read_f0(haptics);
	if (f0 < 0)
		return f0;

	/* restore default config */
	err = regmap_update_bits(haptics->regmap, AW8695_CONT_CTRL,
				 AW8695_CONT_CTRL_EN_CLOSE_MASK | AW8695_CONT_CTRL_F0_DETECT_ENABLE,
				 FIELD_PREP(AW8695_CONT_CTRL_EN_CLOSE_MASK,
					    AW8695_CONT_CTRL_CLOSE_PLAYBACK));
	if (err)
		return err;

	return f0;
}

static int aw8695_haptic_f0_calibration(struct aw8695_data *haptics)
{
	struct device *dev = &haptics->client->dev;
	unsigned int read_buf;
	int f0_cali_step, f0_limit, f0;
	char f0_cali_lra;
	int err;

	f0 = aw8695_haptic_get_f0(haptics);
	if (f0 < 0) {
		dev_err(dev, "Failed to read f0: %d\n", f0);
		return f0;
	}

	/* make sure the f0_limit is not more than f0_cali_percent % away from read f0 */
	if (f0 * 100 < haptics->f0_preset * (100 - haptics->f0_cali_percent) ||
	    f0 * 100 > haptics->f0_preset * (100 + haptics->f0_cali_percent)) {
		f0_limit = (int)haptics->f0_preset;
	} else {
		f0_limit = (int)f0;
	}

	/* calculate cali step */
	f0_cali_step = 100000 * (f0_limit - (int)haptics->f0_preset) /
		(f0_limit * 25);

	if (f0_cali_step >= 0) {
		if (f0_cali_step % 10 >= 5)
			f0_cali_step = f0_cali_step / 10 + 1 + 32;
		else
			f0_cali_step = f0_cali_step / 10 + 32;
	} else {
		if (f0_cali_step % 10 <= -5)
			f0_cali_step = 32 + (f0_cali_step / 10 - 1);
		else
			f0_cali_step = 32 + f0_cali_step / 10;
	}

	if (f0_cali_step > 31)
		f0_cali_lra = (char)f0_cali_step - 32;
	else
		f0_cali_lra = (char)f0_cali_step + 32;

	err = regmap_write(haptics->regmap, AW8695_TRIM_LRA, f0_cali_lra);
	if (err)
		return err;

	err = regmap_read(haptics->regmap, AW8695_TRIM_LRA, &read_buf);
	if (err)
		return err;

	dev_dbg(dev, "Calibrated TRIM_LRA: %x\n", read_buf);

	/* restore default work mode */
	err = aw8695_set_work_mode(haptics, AW8695_STANDBY_MODE);
	if (err)
		return err;
	err = regmap_update_bits(haptics->regmap, AW8695_SYSCTRL,
				 AW8695_SYSCTRL_PLAY_MODE_MASK,
				 FIELD_PREP(AW8695_SYSCTRL_PLAY_MODE_MASK,
					    AW8695_SYSCTRL_PLAY_MODE_RAM));
	if (err)
		return err;

	return aw8695_haptics_stop(haptics);
}

static int aw8695_init(struct aw8695_data *haptics)
{
	int err;
	unsigned int read_buf;
	struct device *dev = &haptics->client->dev;

	aw8695_hw_reset(haptics);

	err = regmap_read(haptics->regmap, AW8695_ID, &read_buf);
	if (err) {
		dev_err(dev, "Failed to read ID register: %d\n", err);
		return err;
	}

	if (read_buf != AW8695_CHIPID) {
		dev_err(dev, "Chip ID mismatch: expected %x, got %x\n",
			AW8695_CHIPID, read_buf);
		return -ENODEV;
	}

	err = regmap_write(haptics->regmap, AW8695_ID, AW8695_RESET);
	if (err) {
		dev_err(dev, "Failed to reset: %d\n", err);
		return err;
	}

	/* Wait ~1ms after reset */
	usleep_range(1000, 1500);

	/* Clear UVLI bit by reading register */
	err = aw8695_interrupt_clear(haptics);
	if (err) {
		dev_err(dev, "Failed to clear interrupt: %d\n", err);
		return err;
	}

	/* Set interrupt mode to edge */
	err = regmap_update_bits(haptics->regmap, AW8695_DBGCTRL,
				 AW8695_DBGCTRL_INT_MODE_MASK,
				 FIELD_PREP(AW8695_DBGCTRL_INT_MODE_MASK,
					    AW8695_DBGCTRL_INT_MODE_EDGE));
	if (err) {
		dev_err(dev, "Failed to set interrupt mode: %d\n", err);
		return err;
	}

	/* Configure interrupts */
	err = regmap_update_bits(haptics->regmap, AW8695_SYSINTM,
				 AW8695_SYSINTM_BSTERR_OFF | AW8695_SYSINTM_OV_OFF |
				 AW8695_SYSINTM_UVLO_OFF | AW8695_SYSINTM_OCD_OFF |
				 AW8695_SYSINTM_OT_OFF,
				 AW8695_SYSINTM_BSTERR_OFF);
	if (err)
		return err;

	err = aw8695_set_work_mode(haptics, AW8695_STANDBY_MODE);
	if (err)
		return err;

	err = regmap_update_bits(haptics->regmap, AW8695_PWMDBG,
				 AW8695_PWMDBG_PWM_MODE_MASK,
				 FIELD_PREP(AW8695_PWMDBG_PWM_MODE_MASK,
					    AW8695_PWMDBG_PWM_24K));
	if (err)
		return err;

	err = regmap_write(haptics->regmap, AW8695_BSTDBG1, haptics->boost_debug[0]);
	if (err)
		return err;
	err = regmap_write(haptics->regmap, AW8695_BSTDBG2, haptics->boost_debug[1]);
	if (err)
		return err;
	err = regmap_write(haptics->regmap, AW8695_BSTDBG3, haptics->boost_debug[2]);
	if (err)
		return err;
	err = regmap_write(haptics->regmap, AW8695_TSET, haptics->tset);
	if (err)
		return err;
	err = regmap_write(haptics->regmap, AW8695_R_SPARE, haptics->r_spare);
	if (err)
		return err;

	err = regmap_update_bits(haptics->regmap, AW8695_ANADBG,
				 AW8695_ANADBG_IOC_MASK,
				 FIELD_PREP(AW8695_ANADBG_IOC_MASK,
					    AW8695_ANADBG_IOC_4P65A));
	if (err)
		return err;

	/* Set boost peak current */
	err = regmap_update_bits(haptics->regmap, AW8695_BSTCFG,
				 AW8695_BSTCFG_PEAKCUR_MASK,
				 FIELD_PREP(AW8695_BSTCFG_PEAKCUR_MASK,
					    AW8695_BSTCFG_PEAKCUR_2A));
	if (err)
		return err;

	/* Adjust motorprotect config */
	err = regmap_update_bits(haptics->regmap, AW8695_DETCTRL,
				 AW8695_DETCTRL_PROTECT_MASK,
				 FIELD_PREP(AW8695_DETCTRL_PROTECT_MASK,
					    AW8695_DETCTRL_PROTECT_NO_ACTION));
	if (err)
		return err;
	err = regmap_update_bits(haptics->regmap, AW8695_PWMPRC,
				 AW8695_PWMPRC_PRC_ENABLE, 0);
	if (err)
		return err;
	err = regmap_update_bits(haptics->regmap, AW8695_PRLVL,
				 AW8695_PRLVL_PR_ENABLE, 0);
	if (err)
		return err;

	/* Adjust auto boost config */
	err = regmap_update_bits(haptics->regmap, AW8695_BST_AUTO,
				 AW8695_BST_AUTO_BST_AUTOSW_MASK,
				 FIELD_PREP(AW8695_BST_AUTO_BST_AUTOSW_MASK,
					    AW8695_BST_AUTO_BST_MANUAL_BOOST));
	if (err)
		return err;

	err = aw8695_haptic_offset_calibration(haptics);
	if (err)
		return err;

	/* Set vbat compensation mode */
	err = regmap_update_bits(haptics->regmap, AW8695_ADCTEST,
				 AW8695_ADCTEST_VBAT_MODE_MASK,
				 FIELD_PREP(AW8695_ADCTEST_VBAT_MODE_MASK,
					    AW8695_ADCTEST_VBAT_HW_COMP));
	if (err)
		return err;

	err = aw8695_haptic_f0_calibration(haptics);
	if (err)
		return err;

	/* beme config */
	err = regmap_write(haptics->regmap, AW8695_BEMF_VTHH_H,
			   FIELD_GET(AW8695_HIGH_MASK, haptics->bemf_vthh));
	if (err)
		return err;
	err = regmap_write(haptics->regmap, AW8695_BEMF_VTHH_L,
			   FIELD_GET(AW8695_LOW_MASK, haptics->bemf_vthh));
	if (err)
		return err;
	err = regmap_write(haptics->regmap, AW8695_BEMF_VTHL_H,
			   FIELD_GET(AW8695_HIGH_MASK, haptics->bemf_vthl));
	if (err)
		return err;
	return regmap_write(haptics->regmap, AW8695_BEMF_VTHL_L,
			    FIELD_GET(AW8695_LOW_MASK, haptics->bemf_vthl));
}

static int aw8695_ram_init(struct aw8695_data *haptics)
{
	unsigned char *ptr;
	int err;
	int i;

	/* Enable SRAM init */
	err = regmap_update_bits(haptics->regmap, AW8695_SYSCTRL,
				 AW8695_SYSCTRL_RAMINIT_EN, AW8695_SYSCTRL_RAMINIT_EN);
	if (err)
		return err;

	/* Set RAMDATA write address */
	err = regmap_write(haptics->regmap, AW8695_RAMADDRH,
			   FIELD_GET(AW8695_HIGH_MASK, AW8695_RAM_BASE_ADDR));
	if (err)
		return err;
	err = regmap_write(haptics->regmap, AW8695_RAMADDRL,
			   FIELD_GET(AW8695_LOW_MASK, AW8695_RAM_BASE_ADDR));
	if (err)
		return err;

	/* Write waveform header */
	ptr = (unsigned char *) &sram_waveform_header;
	for (i = 0; i < sizeof(sram_waveform_header); i++) {
		err = regmap_write(haptics->regmap, AW8695_RAMDATA,
				   ptr[i]);
		if (err)
			return err;
	}

	/* Write waveform data */
	for (i = 0; i < ARRAY_SIZE(aw8695_sine_waveform); i++) {
		err = regmap_write(haptics->regmap, AW8695_RAMDATA,
				   aw8695_sine_waveform[i]);
		if (err)
			return err;
	}

	/* Disable SRAM init */
	return regmap_update_bits(haptics->regmap, AW8695_SYSCTRL,
				  AW8695_SYSCTRL_RAMINIT_EN, 0);
}

static irqreturn_t aw8695_irq(int irq, void *data)
{
	struct aw8695_data *haptics = data;
	struct device *dev = &haptics->client->dev;
	unsigned int read_buf;
	int err;

	err = regmap_read(haptics->regmap, AW8695_SYSINT, &read_buf);
	if (err) {
		dev_err(dev, "Failed to read SYSINT register: %d\n", err);
		return IRQ_NONE;
	}
	dev_dbg(dev, "Interrupt: SYSINT=0x%x\n", read_buf);

	if (read_buf & AW8695_SYSINT_BSTERRI)
		dev_err(dev, "Received boost short circuit protection or over-voltage protection interrupt!\n");
	if (read_buf & AW8695_SYSINT_OVI)
		dev_err(dev, "Received wave data overflow or DPWM DC error interrupt!\n");
	if (read_buf & AW8695_SYSINT_UVLI)
		dev_err(dev, "Received under voltage lock out interrupt!\n");
	if (read_buf & AW8695_SYSINT_OCDI)
		dev_err(dev, "Received over current interrupt!\n");
	if (read_buf & AW8695_SYSINT_OTI)
		dev_err(dev, "Received over temperature interrupt!\n");

	if (read_buf & AW8695_SYSINT_DONEI)
		dev_dbg(dev, "Received playback done interrupt\n");
	/* FIFO mode is not (yet) implemented in this driver */
	if (read_buf & AW8695_SYSINT_FF_AEI)
		dev_dbg(dev, "Received FIFO almost empty interrupt\n");
	if (read_buf & AW8695_SYSINT_FF_AFI)
		dev_dbg(dev, "Received FIFO almost full interrupt\n");

	err = regmap_read(haptics->regmap, AW8695_DBGSTAT, &read_buf);
	if (err) {
		dev_err(dev, "Failed to read DBGSTAT register: %d\n", err);
		return IRQ_NONE;
	}
	dev_dbg(dev, "Interrupt: DBGSTAT=0x%x\n", read_buf);

	err = regmap_read(haptics->regmap, AW8695_SYSST, &read_buf);
	if (err) {
		dev_err(dev, "Failed to read SYSST register: %d\n", err);
		return IRQ_NONE;
	}
	dev_dbg(dev, "Interrupt: SYSST=0x%x\n", read_buf);

	return IRQ_HANDLED;
}

static const struct regmap_config aw8695_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,

	.max_register = AW8695_MAX_REG,
	.cache_type = REGCACHE_NONE,
};

static int aw8695_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct aw8695_data *haptics;
	int err;

	haptics = devm_kzalloc(dev, sizeof(*haptics), GFP_KERNEL);
	if (!haptics)
		return -ENOMEM;

	err = of_property_read_u32(dev->of_node, "awinic,f0-preset", &haptics->f0_preset);
	if (err)
		return dev_err_probe(dev, err, "Failed to read awinic,f0-preset\n");

	err = of_property_read_u32(dev->of_node, "awinic,f0-coefficient",
				   &haptics->f0_coefficient);
	if (err)
		return dev_err_probe(dev, err, "Failed to read awinic,f0-coefficient\n");

	err = of_property_read_u32(dev->of_node, "awinic,f0-calibration-percent",
				   &haptics->f0_cali_percent);
	if (err)
		return dev_err_probe(dev, err, "Failed to read awinic,f0-coefficient\n");

	err = of_property_read_u32(dev->of_node, "awinic,drive-level", &haptics->drive_level);
	if (err)
		return dev_err_probe(dev, err, "Failed to read awinic,drive-level\n");

	err = of_property_read_u32(dev->of_node, "awinic,f0-detection-play-time",
				   &haptics->f0_det_play);
	if (err)
		return dev_err_probe(dev, err, "Failed to read awinic,f0-detection-play-time\n");

	err = of_property_read_u32(dev->of_node, "awinic,f0-detection-wait-time",
				   &haptics->f0_det_wait);
	if (err)
		return dev_err_probe(dev, err, "Failed to read awinic,f0-detection-wait-time\n");

	err = of_property_read_u32(dev->of_node, "awinic,f0-detection-repeat",
				   &haptics->f0_det_repeat);
	if (err)
		return dev_err_probe(dev, err, "Failed to read awinic,f0-detection-repeat\n");

	err = of_property_read_u32(dev->of_node, "awinic,f0-detection-trace",
				   &haptics->f0_det_trace);
	if (err)
		dev_err_probe(dev, err, "Failed to read awinic,f0-detection-trace\n");

	err = of_property_read_u8_array(dev->of_node, "awinic,boost-debug",
					haptics->boost_debug, ARRAY_SIZE(haptics->boost_debug));
	if (err)
		return dev_err_probe(dev, err, "Failed to read awinic,boost-debug\n");

	err = of_property_read_u8(dev->of_node, "awinic,tset", &haptics->tset);
	if (err)
		return dev_err_probe(dev, err, "Failed to read awinic,tset\n");

	err = of_property_read_u8(dev->of_node, "awinic,r-spare", &haptics->r_spare);
	if (err)
		return dev_err_probe(dev, err, "Failed to read awinic,r-spare\n");

	err = of_property_read_u32(dev->of_node, "awinic,bemf-upper-threshold",
				   &haptics->bemf_vthh);
	if (err)
		return dev_err_probe(dev, err, "Failed to read awinic,bemf-upper-threshold\n");

	err = of_property_read_u32(dev->of_node, "awinic,bemf-lower-threshold",
				   &haptics->bemf_vthl);
	if (err)
		return dev_err_probe(dev, err, "Failed to read awinic,bemf-lower-threshold\n");

	haptics->input_dev = devm_input_allocate_device(dev);
	if (!haptics->input_dev)
		return -ENOMEM;

	haptics->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(haptics->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(haptics->reset_gpio),
				     "Failed to get reset gpio\n");

	err = devm_request_threaded_irq(dev, client->irq, NULL, aw8695_irq,
		IRQF_ONESHOT, NULL, haptics);
	if (err)
		return dev_err_probe(dev, err, "Failed to request interrupt\n");

	INIT_WORK(&haptics->play_work, aw8695_haptics_play_work);

	haptics->input_dev->name = "aw8695-haptics";
	haptics->input_dev->close = aw8695_close;

	input_set_drvdata(haptics->input_dev, haptics);
	input_set_capability(haptics->input_dev, EV_FF, FF_RUMBLE);

	err = input_ff_create_memless(haptics->input_dev, NULL,
				      aw8695_haptics_play);
	if (err)
		return dev_err_probe(dev, err, "Failed to create FF dev\n");

	haptics->client = client;
	i2c_set_clientdata(client, haptics);

	haptics->regmap = devm_regmap_init_i2c(client, &aw8695_regmap_config);
	if (IS_ERR(haptics->regmap))
		return dev_err_probe(dev, PTR_ERR(haptics->regmap),
				     "Failed to allocate register map\n");

	err = aw8695_init(haptics);
	if (err)
		return dev_err_probe(dev, err, "Failed to init aw8695\n");

	err = aw8695_ram_init(haptics);
	if (err)
		return dev_err_probe(dev, err, "Failed to init aw8695 sram\n");

	err = input_register_device(haptics->input_dev);
	if (err)
		return dev_err_probe(dev, err, "Failed to register input device\n");

	return 0;
}

static const struct of_device_id aw8695_of_id[] = {
	{ .compatible = "awinic,aw8695", },
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(of, aw8695_of_id);

static struct i2c_driver aw8695_driver = {
	.driver = {
		.name = "aw8695-haptics",
		.of_match_table = aw8695_of_id,
	},
	.probe = aw8695_probe,
};

module_i2c_driver(aw8695_driver);

MODULE_AUTHOR("Luca Weiss <luca.weiss@fairphone.com>");
MODULE_DESCRIPTION("AW8695 LRA Haptic Driver");
MODULE_LICENSE("GPL");
