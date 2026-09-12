// SPDX-License-Identifier: GPL-2.0-only
/*
 * A V4L2 driver for Sony IMX582 cameras.
 * Copyright (C) 2024 Luca Weiss <luca.weiss@fairphone.com>
 *
 * Based on Sony imx412 camera driver
 * Copyright (C) 2021 Intel Corporation
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

/* Chip ID */
#define IMX582_REG_CHIP_ID		CCI_REG16(0x0016)
#define IMX582_CHIP_ID			0x0582

/* Streaming Mode */
#define IMX582_REG_MODE_SELECT		CCI_REG8(0x0100)
#define IMX582_MODE_STANDBY		0x00
#define IMX582_MODE_STREAMING		0x01

/* Lines per frame */
#define IMX582_REG_LPFR			CCI_REG16(0x0340)

/* Exposure control */
#define IMX582_REG_EXPOSURE		CCI_REG16(0x0202)
#define IMX582_EXPOSURE_MIN		8
#define IMX582_EXPOSURE_OFFSET		22
#define IMX582_EXPOSURE_STEP		1
#define IMX582_EXPOSURE_DEFAULT		0x0648

/* Analog gain control */
#define IMX582_REG_ANALOG_GAIN		CCI_REG16(0x0204)
#define IMX582_ANA_GAIN_MIN		0
#define IMX582_ANA_GAIN_MAX		978
#define IMX582_ANA_GAIN_STEP		1
#define IMX582_ANA_GAIN_DEFAULT		0

/* Group hold register */
#define IMX582_REG_HOLD		CCI_REG8(0x0104)

/* Input clock rate */
#define IMX582_INCLK_RATE	24000000

/* CSI2 HW configuration */
#define IMX582_LINK_FREQ	600000000
#define IMX582_NUM_DATA_LANES	4

#define IMX582_REG_MIN		0x00
#define IMX582_REG_MAX		0xffff

/**
 * struct imx582_reg_list - imx582 sensor register list
 * @num_of_regs: Number of registers in the list
 * @regs: Pointer to register list
 */
struct imx582_reg_list {
	u32 num_of_regs;
	const struct cci_reg_sequence *regs;
};

/**
 * struct imx582_mode - imx582 sensor mode structure
 * @width: Frame width
 * @height: Frame height
 * @code: Format code
 * @hblank: Horizontal blanking in lines
 * @vblank: Vertical blanking in lines
 * @vblank_min: Minimum vertical blanking in lines
 * @vblank_max: Maximum vertical blanking in lines
 * @pclk: Sensor pixel clock
 * @link_freq_idx: Link frequency index
 * @reg_list: Register list for sensor mode
 */
struct imx582_mode {
	u32 width;
	u32 height;
	u32 code;
	u32 hblank;
	u32 vblank;
	u32 vblank_min;
	u32 vblank_max;
	u64 pclk;
	u32 link_freq_idx;
	struct imx582_reg_list reg_list;
};

static const char * const imx582_supply_names[] = {
	"vana1",	/* 2.9V Analog Power */
	"vana2",	/* 1.8V Analog Power */
	"vif",		/* 1.8V Interface Power */
	"vdig",		/* 1.1V Digital Power */
};

/**
 * struct imx582 - imx582 sensor device structure
 * @dev: Pointer to generic device
 * @client: Pointer to i2c client
 * @sd: V4L2 sub-device
 * @pad: Media pad. Only one pad supported
 * @reset_gpio: Sensor reset gpio
 * @inclk: Sensor input clock
 * @supplies: Regulator supplies
 * @ctrl_handler: V4L2 control handler
 * @link_freq_ctrl: Pointer to link frequency control
 * @pclk_ctrl: Pointer to pixel clock control
 * @hblank_ctrl: Pointer to horizontal blanking control
 * @vblank_ctrl: Pointer to vertical blanking control
 * @exp_ctrl: Pointer to exposure control
 * @again_ctrl: Pointer to analog gain control
 * @vblank: Vertical blanking in lines
 * @cur_mode: Pointer to current selected sensor mode
 * @mutex: Mutex for serializing sensor controls
 */
struct imx582 {
	struct device *dev;
	struct i2c_client *client;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct gpio_desc *reset_gpio;
	struct clk *inclk;
	struct regulator_bulk_data supplies[ARRAY_SIZE(imx582_supply_names)];
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *link_freq_ctrl;
	struct v4l2_ctrl *pclk_ctrl;
	struct v4l2_ctrl *hblank_ctrl;
	struct v4l2_ctrl *vblank_ctrl;
	struct {
		struct v4l2_ctrl *exp_ctrl;
		struct v4l2_ctrl *again_ctrl;
	};
	u32 vblank;
	const struct imx582_mode *cur_mode;
	struct mutex mutex;
	struct regmap *regmap;
};

static const s64 link_freq[] = {
	IMX582_LINK_FREQ,
};

/* Sensor mode registers */
static const struct cci_reg_sequence mode_4000x2256_regs[] = {
	// common registers
	{ CCI_REG8(0x0136), 0x13 },
	{ CCI_REG8(0x0137), 0x33 },
	{ CCI_REG8(0x3c7e), 0x02 },
	{ CCI_REG8(0x3c7f), 0x06 },
	{ CCI_REG8(0x3c00), 0x10 },
	{ CCI_REG8(0x3c01), 0x10 },
	{ CCI_REG8(0x3c02), 0x10 },
	{ CCI_REG8(0x3c03), 0x10 },
	{ CCI_REG8(0x3c04), 0x10 },
	{ CCI_REG8(0x3c05), 0x01 },
	{ CCI_REG8(0x3c06), 0x00 },
	{ CCI_REG8(0x3c07), 0x00 },
	{ CCI_REG8(0x3c08), 0x03 },
	{ CCI_REG8(0x3c09), 0xff },
	{ CCI_REG8(0x3c0a), 0x01 },
	{ CCI_REG8(0x3c0b), 0x00 },
	{ CCI_REG8(0x3c0c), 0x00 },
	{ CCI_REG8(0x3c0d), 0x03 },
	{ CCI_REG8(0x3c0e), 0xff },
	{ CCI_REG8(0x3c0f), 0x20 },
	{ CCI_REG8(0x6e1d), 0x00 },
	{ CCI_REG8(0x6e25), 0x00 },
	{ CCI_REG8(0x6e38), 0x03 },
	{ CCI_REG8(0x6e3b), 0x01 },
	{ CCI_REG8(0x9004), 0x2c },
	{ CCI_REG8(0x9200), 0xf4 },
	{ CCI_REG8(0x9201), 0xa7 },
	{ CCI_REG8(0x9202), 0xf4 },
	{ CCI_REG8(0x9203), 0xaa },
	{ CCI_REG8(0x9204), 0xf4 },
	{ CCI_REG8(0x9205), 0xad },
	{ CCI_REG8(0x9206), 0xf4 },
	{ CCI_REG8(0x9207), 0xb0 },
	{ CCI_REG8(0x9208), 0xf4 },
	{ CCI_REG8(0x9209), 0xb3 },
	{ CCI_REG8(0x920a), 0xb7 },
	{ CCI_REG8(0x920b), 0x34 },
	{ CCI_REG8(0x920c), 0xb7 },
	{ CCI_REG8(0x920d), 0x36 },
	{ CCI_REG8(0x920e), 0xb7 },
	{ CCI_REG8(0x920f), 0x37 },
	{ CCI_REG8(0x9210), 0xb7 },
	{ CCI_REG8(0x9211), 0x38 },
	{ CCI_REG8(0x9212), 0xb7 },
	{ CCI_REG8(0x9213), 0x39 },
	{ CCI_REG8(0x9214), 0xb7 },
	{ CCI_REG8(0x9215), 0x3a },
	{ CCI_REG8(0x9216), 0xb7 },
	{ CCI_REG8(0x9217), 0x3c },
	{ CCI_REG8(0x9218), 0xb7 },
	{ CCI_REG8(0x9219), 0x3d },
	{ CCI_REG8(0x921a), 0xb7 },
	{ CCI_REG8(0x921b), 0x3e },
	{ CCI_REG8(0x921c), 0xb7 },
	{ CCI_REG8(0x921d), 0x3f },
	{ CCI_REG8(0x921e), 0x85 },
	{ CCI_REG8(0x921f), 0x77 },
	{ CCI_REG8(0x9226), 0x42 },
	{ CCI_REG8(0x9227), 0x52 },
	{ CCI_REG8(0x9228), 0x60 },
	{ CCI_REG8(0x9229), 0xb9 },
	{ CCI_REG8(0x922a), 0x60 },
	{ CCI_REG8(0x922b), 0xbf },
	{ CCI_REG8(0x922c), 0x60 },
	{ CCI_REG8(0x922d), 0xc5 },
	{ CCI_REG8(0x922e), 0x60 },
	{ CCI_REG8(0x922f), 0xcb },
	{ CCI_REG8(0x9230), 0x60 },
	{ CCI_REG8(0x9231), 0xd1 },
	{ CCI_REG8(0x9232), 0x60 },
	{ CCI_REG8(0x9233), 0xd7 },
	{ CCI_REG8(0x9234), 0x60 },
	{ CCI_REG8(0x9235), 0xdd },
	{ CCI_REG8(0x9236), 0x60 },
	{ CCI_REG8(0x9237), 0xe3 },
	{ CCI_REG8(0x9238), 0x60 },
	{ CCI_REG8(0x9239), 0xe9 },
	{ CCI_REG8(0x923a), 0x60 },
	{ CCI_REG8(0x923b), 0xef },
	{ CCI_REG8(0x923c), 0x60 },
	{ CCI_REG8(0x923d), 0xf5 },
	{ CCI_REG8(0x923e), 0x60 },
	{ CCI_REG8(0x923f), 0xf9 },
	{ CCI_REG8(0x9240), 0x60 },
	{ CCI_REG8(0x9241), 0xfd },
	{ CCI_REG8(0x9242), 0x61 },
	{ CCI_REG8(0x9243), 0x01 },
	{ CCI_REG8(0x9244), 0x61 },
	{ CCI_REG8(0x9245), 0x05 },
	{ CCI_REG8(0x924a), 0x61 },
	{ CCI_REG8(0x924b), 0x6b },
	{ CCI_REG8(0x924c), 0x61 },
	{ CCI_REG8(0x924d), 0x7f },
	{ CCI_REG8(0x924e), 0x61 },
	{ CCI_REG8(0x924f), 0x92 },
	{ CCI_REG8(0x9250), 0x61 },
	{ CCI_REG8(0x9251), 0x9c },
	{ CCI_REG8(0x9252), 0x61 },
	{ CCI_REG8(0x9253), 0xab },
	{ CCI_REG8(0x9254), 0x61 },
	{ CCI_REG8(0x9255), 0xc4 },
	{ CCI_REG8(0x9256), 0x61 },
	{ CCI_REG8(0x9257), 0xce },
	{ CCI_REG8(0x9810), 0x14 },
	{ CCI_REG8(0x9814), 0x14 },
	{ CCI_REG8(0xc449), 0x04 },
	{ CCI_REG8(0xc44a), 0x01 },
	{ CCI_REG8(0xe286), 0x31 },
	{ CCI_REG8(0xe2a6), 0x32 },
	{ CCI_REG8(0xe2c6), 0x33 },

	// reg_2 4000x2256@30.01fps_759.916Mbps
	{ CCI_REG8(0x0112), 0x0a },
	{ CCI_REG8(0x0113), 0x0a },
	{ CCI_REG8(0x0114), 0x03 },
	{ CCI_REG8(0x0342), 0x1e },
	{ CCI_REG8(0x0343), 0xc0 },
	{ CCI_REG8(0x0340), 0x0e },
	{ CCI_REG8(0x0341), 0x44 },
	{ CCI_REG8(0x0344), 0x00 },
	{ CCI_REG8(0x0345), 0x00 },
	{ CCI_REG8(0x0346), 0x02 },
	{ CCI_REG8(0x0347), 0xe8 },
	{ CCI_REG8(0x0348), 0x1f },
	{ CCI_REG8(0x0349), 0x3f },
	{ CCI_REG8(0x034a), 0x14 },
	{ CCI_REG8(0x034b), 0x87 },
	{ CCI_REG8(0x0900), 0x01 },
	{ CCI_REG8(0x0901), 0x22 },
	{ CCI_REG8(0x0902), 0x08 },
	{ CCI_REG8(0x3246), 0x81 },
	{ CCI_REG8(0x3247), 0x81 },
	{ CCI_REG8(0x0401), 0x00 },
	{ CCI_REG8(0x0404), 0x00 },
	{ CCI_REG8(0x0405), 0x10 },
	{ CCI_REG8(0x0408), 0x00 },
	{ CCI_REG8(0x0409), 0x00 },
	{ CCI_REG8(0x040a), 0x00 },
	{ CCI_REG8(0x040b), 0x00 },
	{ CCI_REG8(0x040c), 0x0f },
	{ CCI_REG8(0x040d), 0xa0 },
	{ CCI_REG8(0x040e), 0x08 },
	{ CCI_REG8(0x040f), 0xd0 },
	{ CCI_REG8(0x034c), 0x0f },
	{ CCI_REG8(0x034d), 0xa0 },
	{ CCI_REG8(0x034e), 0x08 },
	{ CCI_REG8(0x034f), 0xd0 },
	{ CCI_REG8(0x0301), 0x05 },
	{ CCI_REG8(0x0303), 0x02 },
	{ CCI_REG8(0x0305), 0x03 },
	{ CCI_REG8(0x0306), 0x01 },
	{ CCI_REG8(0x0307), 0x51 },
	{ CCI_REG8(0x030b), 0x01 },
	{ CCI_REG8(0x030d), 0x13 },
	{ CCI_REG8(0x030e), 0x07 },
	{ CCI_REG8(0x030f), 0x58 },
	{ CCI_REG8(0x0310), 0x01 },
	{ CCI_REG8(0x3620), 0x00 },
	{ CCI_REG8(0x3621), 0x00 },
	{ CCI_REG8(0x380c), 0x80 },
	{ CCI_REG8(0x3c13), 0x00 },
	{ CCI_REG8(0x3c14), 0x28 },
	{ CCI_REG8(0x3c15), 0x28 },
	{ CCI_REG8(0x3c16), 0x32 },
	{ CCI_REG8(0x3c17), 0x46 },
	{ CCI_REG8(0x3c18), 0x67 },
	{ CCI_REG8(0x3c19), 0x8f },
	{ CCI_REG8(0x3c1a), 0x8f },
	{ CCI_REG8(0x3c1b), 0x99 },
	{ CCI_REG8(0x3c1c), 0xad },
	{ CCI_REG8(0x3c1d), 0xce },
	{ CCI_REG8(0x3c1e), 0x8f },
	{ CCI_REG8(0x3c1f), 0x8f },
	{ CCI_REG8(0x3c20), 0x99 },
	{ CCI_REG8(0x3c21), 0xad },
	{ CCI_REG8(0x3c22), 0xce },
	{ CCI_REG8(0x3c25), 0x22 },
	{ CCI_REG8(0x3c26), 0x23 },
	{ CCI_REG8(0x3c27), 0xe6 },
	{ CCI_REG8(0x3c28), 0xe6 },
	{ CCI_REG8(0x3c29), 0x08 },
	{ CCI_REG8(0x3c2a), 0x0f },
	{ CCI_REG8(0x3c2b), 0x14 },
	{ CCI_REG8(0x3f0c), 0x01 },
	{ CCI_REG8(0x3f14), 0x00 },
	{ CCI_REG8(0x3f80), 0x00 },
	{ CCI_REG8(0x3f81), 0x00 },
	{ CCI_REG8(0x3f82), 0x00 },
	{ CCI_REG8(0x3f83), 0x00 },
	{ CCI_REG8(0x3f8c), 0x07 },
	{ CCI_REG8(0x3f8d), 0xd0 },
	{ CCI_REG8(0x3ff4), 0x00 },
	{ CCI_REG8(0x3ff5), 0x00 },
	{ CCI_REG8(0x3ffc), 0x04 },
	{ CCI_REG8(0x3ffd), 0xb0 },
	{ CCI_REG8(0x0202), 0x0e },
	{ CCI_REG8(0x0203), 0x14 },
	{ CCI_REG8(0x0224), 0x01 },
	{ CCI_REG8(0x0225), 0xf4 },
	{ CCI_REG8(0x3fe0), 0x01 },
	{ CCI_REG8(0x3fe1), 0xf4 },
	{ CCI_REG8(0x0204), 0x00 },
	{ CCI_REG8(0x0205), 0x70 },
	{ CCI_REG8(0x0216), 0x00 },
	{ CCI_REG8(0x0217), 0x70 },
	{ CCI_REG8(0x0218), 0x01 },
	{ CCI_REG8(0x0219), 0x00 },
	{ CCI_REG8(0x020e), 0x01 },
	{ CCI_REG8(0x020f), 0x00 },
	{ CCI_REG8(0x0210), 0x01 },
	{ CCI_REG8(0x0211), 0x00 },
	{ CCI_REG8(0x0212), 0x01 },
	{ CCI_REG8(0x0213), 0x00 },
	{ CCI_REG8(0x0214), 0x01 },
	{ CCI_REG8(0x0215), 0x00 },
	{ CCI_REG8(0x3fe2), 0x00 },
	{ CCI_REG8(0x3fe3), 0x70 },
	{ CCI_REG8(0x3fe4), 0x01 },
	{ CCI_REG8(0x3fe5), 0x00 },
	{ CCI_REG8(0xe000), 0x00 },
	{ CCI_REG8(0x3e20), 0x02 },
	{ CCI_REG8(0x3e3b), 0x01 },
	{ CCI_REG8(0x4034), 0x01 },
	{ CCI_REG8(0x4035), 0xf0 },
};

/* Supported sensor mode configurations */
static const struct imx582_mode supported_mode = {
	.width = 4000,
	.height = 2256,
	.hblank = 456, // FIXME
	.vblank = 506, // FIXME
	.vblank_min = 506, // FIXME
	.vblank_max = 32420, // FIXME
	.pclk = 619200000, // outputPixelClock?
	.link_freq_idx = 0,
	.code = MEDIA_BUS_FMT_SRGGB10_1X10,
	.reg_list = {
		.num_of_regs = ARRAY_SIZE(mode_4000x2256_regs),
		.regs = mode_4000x2256_regs,
	},
};

/**
 * to_imx582() - imx582 V4L2 sub-device to imx582 device.
 * @subdev: pointer to imx582 V4L2 sub-device
 *
 * Return: pointer to imx582 device
 */
static inline struct imx582 *to_imx582(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct imx582, sd);
}

/**
 * imx582_update_controls() - Update control ranges based on streaming mode
 * @imx582: pointer to imx582 device
 * @mode: pointer to imx582_mode sensor mode
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx582_update_controls(struct imx582 *imx582,
				  const struct imx582_mode *mode)
{
	int ret;

	ret = __v4l2_ctrl_s_ctrl(imx582->link_freq_ctrl, mode->link_freq_idx);
	if (ret)
		return ret;

	ret = __v4l2_ctrl_s_ctrl(imx582->hblank_ctrl, mode->hblank);
	if (ret)
		return ret;

	return __v4l2_ctrl_modify_range(imx582->vblank_ctrl, mode->vblank_min,
					mode->vblank_max, 1, mode->vblank);
}

/**
 * imx582_update_exp_gain() - Set updated exposure and gain
 * @imx582: pointer to imx582 device
 * @exposure: updated exposure value
 * @gain: updated analog gain value
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx582_update_exp_gain(struct imx582 *imx582, u32 exposure, u32 gain)
{
	u32 lpfr;
	int ret = 0;

	lpfr = imx582->vblank + imx582->cur_mode->height;

	dev_dbg(imx582->dev, "Set exp %u, analog gain %u, lpfr %u\n",
		exposure, gain, lpfr);

	cci_write(imx582->regmap, IMX582_REG_HOLD, 1, &ret);
	if (ret)
		return ret;

	cci_write(imx582->regmap, IMX582_REG_LPFR, lpfr, &ret);
	if (ret)
		goto error_release_group_hold;

	cci_write(imx582->regmap, IMX582_REG_EXPOSURE, exposure, &ret);
	if (ret)
		goto error_release_group_hold;

	cci_write(imx582->regmap, IMX582_REG_ANALOG_GAIN, gain, &ret);

error_release_group_hold:
	cci_write(imx582->regmap, IMX582_REG_HOLD, 0, NULL);

	return ret;
}

/**
 * imx582_set_ctrl() - Set subdevice control
 * @ctrl: pointer to v4l2_ctrl structure
 *
 * Supported controls:
 * - V4L2_CID_VBLANK
 * - cluster controls:
 *   - V4L2_CID_ANALOGUE_GAIN
 *   - V4L2_CID_EXPOSURE
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx582_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx582 *imx582 =
		container_of(ctrl->handler, struct imx582, ctrl_handler);
	u32 analog_gain;
	u32 exposure;
	int ret;

	switch (ctrl->id) {
	case V4L2_CID_VBLANK:
		imx582->vblank = imx582->vblank_ctrl->val;

		dev_dbg(imx582->dev, "Received vblank %u, new lpfr %u\n",
			imx582->vblank,
			imx582->vblank + imx582->cur_mode->height);

		ret = __v4l2_ctrl_modify_range(imx582->exp_ctrl,
					       IMX582_EXPOSURE_MIN,
					       imx582->vblank +
					       imx582->cur_mode->height -
					       IMX582_EXPOSURE_OFFSET,
					       1, IMX582_EXPOSURE_DEFAULT);
		break;
	case V4L2_CID_EXPOSURE:
		/* Set controls only if sensor is in power on state */
		if (!pm_runtime_get_if_in_use(imx582->dev))
			return 0;

		exposure = ctrl->val;
		analog_gain = imx582->again_ctrl->val;

		dev_dbg(imx582->dev, "Received exp %u, analog gain %u\n",
			exposure, analog_gain);

		ret = imx582_update_exp_gain(imx582, exposure, analog_gain);

		pm_runtime_put(imx582->dev);

		break;
	default:
		dev_err(imx582->dev, "Invalid control %d\n", ctrl->id);
		ret = -EINVAL;
	}

	return ret;
}

/* V4l2 subdevice control ops*/
static const struct v4l2_ctrl_ops imx582_ctrl_ops = {
	.s_ctrl = imx582_set_ctrl,
};

/**
 * imx582_enum_mbus_code() - Enumerate V4L2 sub-device mbus codes
 * @sd: pointer to imx582 V4L2 sub-device structure
 * @sd_state: V4L2 sub-device configuration
 * @code: V4L2 sub-device code enumeration need to be filled
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx582_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index > 0)
		return -EINVAL;

	code->code = supported_mode.code;

	return 0;
}

/**
 * imx582_enum_frame_size() - Enumerate V4L2 sub-device frame sizes
 * @sd: pointer to imx582 V4L2 sub-device structure
 * @sd_state: V4L2 sub-device configuration
 * @fsize: V4L2 sub-device size enumeration need to be filled
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx582_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_size_enum *fsize)
{
	if (fsize->index > 0)
		return -EINVAL;

	if (fsize->code != supported_mode.code)
		return -EINVAL;

	fsize->min_width = supported_mode.width;
	fsize->max_width = fsize->min_width;
	fsize->min_height = supported_mode.height;
	fsize->max_height = fsize->min_height;

	return 0;
}

/**
 * imx582_fill_pad_format() - Fill subdevice pad format
 *                            from selected sensor mode
 * @imx582: pointer to imx582 device
 * @mode: pointer to imx582_mode sensor mode
 * @fmt: V4L2 sub-device format need to be filled
 */
static void imx582_fill_pad_format(struct imx582 *imx582,
				   const struct imx582_mode *mode,
				   struct v4l2_subdev_format *fmt)
{
	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.code = mode->code;
	fmt->format.field = V4L2_FIELD_NONE;
	fmt->format.colorspace = V4L2_COLORSPACE_RAW;
	fmt->format.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->format.quantization = V4L2_QUANTIZATION_DEFAULT;
	fmt->format.xfer_func = V4L2_XFER_FUNC_NONE;
}

/**
 * imx582_get_pad_format() - Get subdevice pad format
 * @sd: pointer to imx582 V4L2 sub-device structure
 * @sd_state: V4L2 sub-device configuration
 * @fmt: V4L2 sub-device format need to be set
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx582_get_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_format *fmt)
{
	struct imx582 *imx582 = to_imx582(sd);

	mutex_lock(&imx582->mutex);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		struct v4l2_mbus_framefmt *framefmt;

		framefmt = v4l2_subdev_state_get_format(sd_state, fmt->pad);
		fmt->format = *framefmt;
	} else {
		imx582_fill_pad_format(imx582, imx582->cur_mode, fmt);
	}

	mutex_unlock(&imx582->mutex);

	return 0;
}

/**
 * imx582_set_pad_format() - Set subdevice pad format
 * @sd: pointer to imx582 V4L2 sub-device structure
 * @sd_state: V4L2 sub-device configuration
 * @fmt: V4L2 sub-device format need to be set
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx582_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_format *fmt)
{
	struct imx582 *imx582 = to_imx582(sd);
	const struct imx582_mode *mode;
	int ret = 0;

	mutex_lock(&imx582->mutex);

	mode = &supported_mode;
	imx582_fill_pad_format(imx582, mode, fmt);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		struct v4l2_mbus_framefmt *framefmt;

		framefmt = v4l2_subdev_state_get_format(sd_state, fmt->pad);
		*framefmt = fmt->format;
	} else {
		ret = imx582_update_controls(imx582, mode);
		if (!ret)
			imx582->cur_mode = mode;
	}

	mutex_unlock(&imx582->mutex);

	return ret;
}

/**
 * imx582_init_state() - Initialize sub-device state
 * @sd: pointer to imx582 V4L2 sub-device structure
 * @sd_state: V4L2 sub-device configuration
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx582_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *sd_state)
{
	struct imx582 *imx582 = to_imx582(sd);
	struct v4l2_subdev_format fmt = { 0 };

	fmt.which = sd_state ? V4L2_SUBDEV_FORMAT_TRY : V4L2_SUBDEV_FORMAT_ACTIVE;
	imx582_fill_pad_format(imx582, &supported_mode, &fmt);

	return imx582_set_pad_format(sd, sd_state, &fmt);
}

/**
 * imx582_start_streaming() - Start sensor stream
 * @imx582: pointer to imx582 device
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx582_start_streaming(struct imx582 *imx582)
{
	const struct imx582_reg_list *reg_list;
	int ret;

	/* Write sensor mode registers */
	reg_list = &imx582->cur_mode->reg_list;
	ret = cci_multi_reg_write(imx582->regmap, reg_list->regs,
				  reg_list->num_of_regs, NULL);
	if (ret) {
		dev_err(imx582->dev, "fail to write initial registers\n");
		return ret;
	}

	/* Setup handler will write actual exposure and gain */
	ret =  __v4l2_ctrl_handler_setup(imx582->sd.ctrl_handler);
	if (ret) {
		dev_err(imx582->dev, "fail to setup handler\n");
		return ret;
	}

	/* Delay is required before streaming*/
	usleep_range(7400, 8000);

	/* Start streaming */
	cci_write(imx582->regmap, IMX582_REG_MODE_SELECT, IMX582_MODE_STREAMING, &ret);
	if (ret) {
		dev_err(imx582->dev, "fail to start streaming\n");
		return ret;
	}

	return 0;
}

/**
 * imx582_stop_streaming() - Stop sensor stream
 * @imx582: pointer to imx582 device
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx582_stop_streaming(struct imx582 *imx582)
{
	return cci_write(imx582->regmap, IMX582_REG_MODE_SELECT,
			 IMX582_MODE_STANDBY, NULL);
}

/**
 * imx582_set_stream() - Enable sensor streaming
 * @sd: pointer to imx582 subdevice
 * @enable: set to enable sensor streaming
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx582_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct imx582 *imx582 = to_imx582(sd);
	int ret;

	mutex_lock(&imx582->mutex);

	if (enable) {
		ret = pm_runtime_resume_and_get(imx582->dev);
		if (ret)
			goto error_unlock;

		ret = imx582_start_streaming(imx582);
		if (ret)
			goto error_power_off;
	} else {
		imx582_stop_streaming(imx582);
		pm_runtime_put(imx582->dev);
	}

	mutex_unlock(&imx582->mutex);

	return 0;

error_power_off:
	pm_runtime_put(imx582->dev);
error_unlock:
	mutex_unlock(&imx582->mutex);

	return ret;
}

/**
 * imx582_detect() - Detect imx582 sensor
 * @imx582: pointer to imx582 device
 *
 * Return: 0 if successful, -EIO if sensor id does not match
 */
static int imx582_detect(struct imx582 *imx582)
{
	int ret;
	u64 val;

	ret = cci_read(imx582->regmap, IMX582_REG_CHIP_ID, &val, NULL);
	if (ret)
		return ret;

	if (val != IMX582_CHIP_ID) {
		dev_err(imx582->dev, "chip id mismatch: %x!=%llx\n",
			IMX582_CHIP_ID, val);
		return -ENXIO;
	}

	return 0;
}

/**
 * imx582_parse_hw_config() - Parse HW configuration and check if supported
 * @imx582: pointer to imx582 device
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx582_parse_hw_config(struct imx582 *imx582)
{
	struct fwnode_handle *fwnode = dev_fwnode(imx582->dev);
	struct v4l2_fwnode_endpoint bus_cfg = {};
	struct fwnode_handle *ep;
	unsigned long rate;
	unsigned int i;
	int ret;

	if (!fwnode)
		return -ENXIO;

	/* Request optional reset pin */
	imx582->reset_gpio = devm_gpiod_get_optional(imx582->dev, "reset",
						     GPIOD_OUT_LOW);
	if (IS_ERR(imx582->reset_gpio)) {
		dev_err(imx582->dev, "failed to get reset gpio %ld\n",
			PTR_ERR(imx582->reset_gpio));
		return PTR_ERR(imx582->reset_gpio);
	}

	/* Get sensor input clock */
	imx582->inclk = devm_clk_get(imx582->dev, NULL);
	if (IS_ERR(imx582->inclk)) {
		dev_err(imx582->dev, "could not get inclk\n");
		return PTR_ERR(imx582->inclk);
	}

	rate = clk_get_rate(imx582->inclk);
	if (rate != IMX582_INCLK_RATE) {
		dev_err(imx582->dev, "inclk frequency mismatch\n");
		return -EINVAL;
	}

	/* Get optional DT defined regulators */
	for (i = 0; i < ARRAY_SIZE(imx582_supply_names); i++)
		imx582->supplies[i].supply = imx582_supply_names[i];

	ret = devm_regulator_bulk_get(imx582->dev,
				      ARRAY_SIZE(imx582_supply_names),
				      imx582->supplies);
	if (ret)
		return ret;

	ep = fwnode_graph_get_next_endpoint(fwnode, NULL);
	if (!ep)
		return -ENXIO;

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return ret;

	if (bus_cfg.bus_type != V4L2_MBUS_CSI2_DPHY) {
		dev_err(imx582->dev, "selected bus-type is not supported\n");
		ret = -EINVAL;
		goto done_endpoint_free;
	}

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != IMX582_NUM_DATA_LANES) {
		dev_err(imx582->dev,
			"number of CSI2 data lanes %d is not supported\n",
			bus_cfg.bus.mipi_csi2.num_data_lanes);
		ret = -EINVAL;
		goto done_endpoint_free;
	}

	if (!bus_cfg.nr_of_link_frequencies) {
		dev_err(imx582->dev, "no link frequencies defined\n");
		ret = -EINVAL;
		goto done_endpoint_free;
	}

	for (i = 0; i < bus_cfg.nr_of_link_frequencies; i++)
		if (bus_cfg.link_frequencies[i] == IMX582_LINK_FREQ)
			goto done_endpoint_free;

	ret = -EINVAL;

done_endpoint_free:
	v4l2_fwnode_endpoint_free(&bus_cfg);

	return ret;
}

/* V4l2 subdevice ops */
static const struct v4l2_subdev_video_ops imx582_video_ops = {
	.s_stream = imx582_set_stream,
};

static const struct v4l2_subdev_pad_ops imx582_pad_ops = {
	.enum_mbus_code = imx582_enum_mbus_code,
	.enum_frame_size = imx582_enum_frame_size,
	.get_fmt = imx582_get_pad_format,
	.set_fmt = imx582_set_pad_format,
};

static const struct v4l2_subdev_ops imx582_subdev_ops = {
	.video = &imx582_video_ops,
	.pad = &imx582_pad_ops,
};

static const struct v4l2_subdev_internal_ops imx582_internal_ops = {
	.init_state = imx582_init_state,
};

/**
 * imx582_power_on() - Sensor power on sequence
 * @dev: pointer to i2c device
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx582_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx582 *imx582 = to_imx582(sd);
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(imx582_supply_names),
				    imx582->supplies);
	if (ret < 0) {
		dev_err(dev, "failed to enable regulators\n");
		return ret;
	}

	gpiod_set_value_cansleep(imx582->reset_gpio, 0);

	ret = clk_prepare_enable(imx582->inclk);
	if (ret) {
		dev_err(imx582->dev, "fail to enable inclk\n");
		goto error_reset;
	}

	usleep_range(1000, 1200);

	return 0;

error_reset:
	gpiod_set_value_cansleep(imx582->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(imx582_supply_names),
			       imx582->supplies);

	return ret;
}

/**
 * imx582_power_off() - Sensor power off sequence
 * @dev: pointer to i2c device
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx582_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx582 *imx582 = to_imx582(sd);

	clk_disable_unprepare(imx582->inclk);

	gpiod_set_value_cansleep(imx582->reset_gpio, 1);

	regulator_bulk_disable(ARRAY_SIZE(imx582_supply_names),
			       imx582->supplies);

	return 0;
}

/**
 * imx582_init_controls() - Initialize sensor subdevice controls
 * @imx582: pointer to imx582 device
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx582_init_controls(struct imx582 *imx582)
{
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl_handler *ctrl_hdlr = &imx582->ctrl_handler;
	const struct imx582_mode *mode = imx582->cur_mode;
	u32 lpfr;
	int ret;

	/* set properties from fwnode (e.g. rotation, orientation) */
	ret = v4l2_fwnode_device_parse(imx582->dev, &props);
	if (ret)
		return ret;

	ret = v4l2_ctrl_handler_init(ctrl_hdlr, 8);
	if (ret)
		return ret;

	/* Serialize controls with sensor device */
	ctrl_hdlr->lock = &imx582->mutex;

	/* Initialize exposure and gain */
	lpfr = mode->vblank + mode->height;
	imx582->exp_ctrl = v4l2_ctrl_new_std(ctrl_hdlr,
					     &imx582_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     IMX582_EXPOSURE_MIN,
					     lpfr - IMX582_EXPOSURE_OFFSET,
					     IMX582_EXPOSURE_STEP,
					     IMX582_EXPOSURE_DEFAULT);

	imx582->again_ctrl = v4l2_ctrl_new_std(ctrl_hdlr,
					       &imx582_ctrl_ops,
					       V4L2_CID_ANALOGUE_GAIN,
					       IMX582_ANA_GAIN_MIN,
					       IMX582_ANA_GAIN_MAX,
					       IMX582_ANA_GAIN_STEP,
					       IMX582_ANA_GAIN_DEFAULT);

	v4l2_ctrl_cluster(2, &imx582->exp_ctrl);

	imx582->vblank_ctrl = v4l2_ctrl_new_std(ctrl_hdlr,
						&imx582_ctrl_ops,
						V4L2_CID_VBLANK,
						mode->vblank_min,
						mode->vblank_max,
						1, mode->vblank);

	/* Read only controls */
	imx582->pclk_ctrl = v4l2_ctrl_new_std(ctrl_hdlr,
					      &imx582_ctrl_ops,
					      V4L2_CID_PIXEL_RATE,
					      mode->pclk, mode->pclk,
					      1, mode->pclk);

	imx582->link_freq_ctrl = v4l2_ctrl_new_int_menu(ctrl_hdlr,
							&imx582_ctrl_ops,
							V4L2_CID_LINK_FREQ,
							ARRAY_SIZE(link_freq) -
							1,
							mode->link_freq_idx,
							link_freq);
	if (imx582->link_freq_ctrl)
		imx582->link_freq_ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	imx582->hblank_ctrl = v4l2_ctrl_new_std(ctrl_hdlr,
						&imx582_ctrl_ops,
						V4L2_CID_HBLANK,
						IMX582_REG_MIN,
						IMX582_REG_MAX,
						1, mode->hblank);
	if (imx582->hblank_ctrl)
		imx582->hblank_ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &imx582_ctrl_ops, &props);

	if (ctrl_hdlr->error) {
		dev_err(imx582->dev, "control init failed: %d\n",
			ctrl_hdlr->error);
		v4l2_ctrl_handler_free(ctrl_hdlr);
		return ctrl_hdlr->error;
	}

	imx582->sd.ctrl_handler = ctrl_hdlr;

	return 0;
}

/**
 * imx582_probe() - I2C client device binding
 * @client: pointer to i2c client device
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx582_probe(struct i2c_client *client)
{
	struct imx582 *imx582;
	int ret;

	imx582 = devm_kzalloc(&client->dev, sizeof(*imx582), GFP_KERNEL);
	if (!imx582)
		return -ENOMEM;

	imx582->dev = &client->dev;

	/* Initialize subdev */
	v4l2_i2c_subdev_init(&imx582->sd, client, &imx582_subdev_ops);
	imx582->sd.internal_ops = &imx582_internal_ops;

	ret = imx582_parse_hw_config(imx582);
	if (ret) {
		dev_err(imx582->dev, "HW configuration is not supported\n");
		return ret;
	}

	mutex_init(&imx582->mutex);

	imx582->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(imx582->regmap))
		return dev_err_probe(imx582->dev, PTR_ERR(imx582->regmap),
				     "failed to initialize CCI\n");

	ret = imx582_power_on(imx582->dev);
	if (ret) {
		dev_err(imx582->dev, "failed to power-on the sensor\n");
		goto error_mutex_destroy;
	}

	/* Check module identity */
	ret = imx582_detect(imx582);
	if (ret) {
		dev_err(imx582->dev, "failed to find sensor: %d\n", ret);
		goto error_power_off;
	}

	/* Set default mode to max resolution */
	imx582->cur_mode = &supported_mode;
	imx582->vblank = imx582->cur_mode->vblank;

	ret = imx582_init_controls(imx582);
	if (ret) {
		dev_err(imx582->dev, "failed to init controls: %d\n", ret);
		goto error_power_off;
	}

	/* Initialize subdev */
	imx582->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	imx582->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	/* Initialize source pad */
	imx582->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&imx582->sd.entity, 1, &imx582->pad);
	if (ret) {
		dev_err(imx582->dev, "failed to init entity pads: %d\n", ret);
		goto error_handler_free;
	}

	ret = v4l2_async_register_subdev_sensor(&imx582->sd);
	if (ret < 0) {
		dev_err(imx582->dev,
			"failed to register async subdev: %d\n", ret);
		goto error_media_entity;
	}

	pm_runtime_set_active(imx582->dev);
	pm_runtime_enable(imx582->dev);
	pm_runtime_idle(imx582->dev);

	return 0;

error_media_entity:
	media_entity_cleanup(&imx582->sd.entity);
error_handler_free:
	v4l2_ctrl_handler_free(imx582->sd.ctrl_handler);
error_power_off:
	imx582_power_off(imx582->dev);
error_mutex_destroy:
	mutex_destroy(&imx582->mutex);

	return ret;
}

static void imx582_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx582 *imx582 = to_imx582(sd);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(sd->ctrl_handler);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		imx582_power_off(&client->dev);
	pm_runtime_set_suspended(&client->dev);

	mutex_destroy(&imx582->mutex);
}

static const struct dev_pm_ops imx582_pm_ops = {
	SET_RUNTIME_PM_OPS(imx582_power_off, imx582_power_on, NULL)
};

static const struct of_device_id imx582_of_match[] = {
	{ .compatible = "sony,imx582" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, imx582_of_match);

static struct i2c_driver imx582_driver = {
	.probe = imx582_probe,
	.remove = imx582_remove,
	.driver = {
		.name = "imx582",
		.pm = &imx582_pm_ops,
		.of_match_table = imx582_of_match,
	},
};

module_i2c_driver(imx582_driver);

MODULE_DESCRIPTION("Sony IMX582 sensor driver");
MODULE_LICENSE("GPL");
