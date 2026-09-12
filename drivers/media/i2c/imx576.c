// SPDX-License-Identifier: GPL-2.0-only
/*
 * A V4L2 driver for Sony IMX576 cameras.
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
#define IMX576_REG_CHIP_ID		CCI_REG16(0x0016)
#define IMX576_CHIP_ID			0x0576

/* Streaming Mode */
#define IMX576_REG_MODE_SELECT		CCI_REG8(0x0100)
#define IMX576_MODE_STANDBY		0x00
#define IMX576_MODE_STREAMING		0x01

/* Lines per frame */
#define IMX576_REG_LPFR			CCI_REG16(0x0340)

/* Exposure control */
#define IMX576_REG_EXPOSURE		CCI_REG16(0x0202)
#define IMX576_EXPOSURE_MIN		8
#define IMX576_EXPOSURE_OFFSET		22
#define IMX576_EXPOSURE_STEP		1
#define IMX576_EXPOSURE_DEFAULT		0x0648

/* Analog gain control */
#define IMX576_REG_ANALOG_GAIN		CCI_REG16(0x0204)
#define IMX576_ANA_GAIN_MIN		0
#define IMX576_ANA_GAIN_MAX		978
#define IMX576_ANA_GAIN_STEP		1
#define IMX576_ANA_GAIN_DEFAULT		0

/* Group hold register */
#define IMX576_REG_HOLD		CCI_REG8(0x0104)

/* Input clock rate */
#define IMX576_INCLK_RATE	24000000

/* CSI2 HW configuration */
#define IMX576_LINK_FREQ	600000000
#define IMX576_NUM_DATA_LANES	4

#define IMX576_REG_MIN		0x00
#define IMX576_REG_MAX		0xffff

/**
 * struct imx576_reg_list - imx576 sensor register list
 * @num_of_regs: Number of registers in the list
 * @regs: Pointer to register list
 */
struct imx576_reg_list {
	u32 num_of_regs;
	const struct cci_reg_sequence *regs;
};

/**
 * struct imx576_mode - imx576 sensor mode structure
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
struct imx576_mode {
	u32 width;
	u32 height;
	u32 code;
	u32 hblank;
	u32 vblank;
	u32 vblank_min;
	u32 vblank_max;
	u64 pclk;
	u32 link_freq_idx;
	struct imx576_reg_list reg_list;
};

static const char * const imx576_supply_names[] = {
	"vana",		/* 2.8V Analog Power */
	"vif",		/* 1.8V Interface Power */
	"vdig",		/* 1.05V Digital Power */
};

/**
 * struct imx576 - imx576 sensor device structure
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
struct imx576 {
	struct device *dev;
	struct i2c_client *client;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct gpio_desc *reset_gpio;
	struct clk *inclk;
	struct regulator_bulk_data supplies[ARRAY_SIZE(imx576_supply_names)];
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
	const struct imx576_mode *cur_mode;
	struct mutex mutex;
	struct regmap *regmap;
};

static const s64 link_freq[] = {
	IMX576_LINK_FREQ,
};

/* Sensor mode registers */
static const struct cci_reg_sequence mode_2880x2156_regs[] = {
	// common registers
	{ CCI_REG8(0x0136), 0x18 },
	{ CCI_REG8(0x0137), 0x00 },
	{ CCI_REG8(0x3c7e), 0x05 },
	{ CCI_REG8(0x3c7f), 0x07 },
	{ CCI_REG8(0x380d), 0x80 },
	{ CCI_REG8(0x3c00), 0x1a },
	{ CCI_REG8(0x3c01), 0x1a },
	{ CCI_REG8(0x3c02), 0x1a },
	{ CCI_REG8(0x3c03), 0x1a },
	{ CCI_REG8(0x3c04), 0x1a },
	{ CCI_REG8(0x3c05), 0x01 },
	{ CCI_REG8(0x3c08), 0xff },
	{ CCI_REG8(0x3c09), 0xff },
	{ CCI_REG8(0x3c0a), 0x01 },
	{ CCI_REG8(0x3c0d), 0xff },
	{ CCI_REG8(0x3c0e), 0xff },
	{ CCI_REG8(0x3c0f), 0x20 },
	{ CCI_REG8(0x3f89), 0x01 },
	{ CCI_REG8(0x4b8e), 0x18 },
	{ CCI_REG8(0x4b8f), 0x10 },
	{ CCI_REG8(0x4ba8), 0x08 },
	{ CCI_REG8(0x4baa), 0x08 },
	{ CCI_REG8(0x4bab), 0x08 },
	{ CCI_REG8(0x4bc9), 0x10 },
	{ CCI_REG8(0x5511), 0x01 },
	{ CCI_REG8(0x560b), 0x5b },
	{ CCI_REG8(0x56a7), 0x60 },
	{ CCI_REG8(0x5b3b), 0x60 },
	{ CCI_REG8(0x5ba7), 0x60 },
	{ CCI_REG8(0x6002), 0x00 },
	{ CCI_REG8(0x6014), 0x01 },
	{ CCI_REG8(0x6118), 0x0a },
	{ CCI_REG8(0x6122), 0x0a },
	{ CCI_REG8(0x6128), 0x0a },
	{ CCI_REG8(0x6132), 0x0a },
	{ CCI_REG8(0x6138), 0x0a },
	{ CCI_REG8(0x6142), 0x0a },
	{ CCI_REG8(0x6148), 0x0a },
	{ CCI_REG8(0x6152), 0x0a },
	{ CCI_REG8(0x617b), 0x04 },
	{ CCI_REG8(0x617e), 0x04 },
	{ CCI_REG8(0x6187), 0x04 },
	{ CCI_REG8(0x618a), 0x04 },
	{ CCI_REG8(0x6193), 0x04 },
	{ CCI_REG8(0x6196), 0x04 },
	{ CCI_REG8(0x619f), 0x04 },
	{ CCI_REG8(0x61a2), 0x04 },
	{ CCI_REG8(0x61ab), 0x04 },
	{ CCI_REG8(0x61ae), 0x04 },
	{ CCI_REG8(0x61b7), 0x04 },
	{ CCI_REG8(0x61ba), 0x04 },
	{ CCI_REG8(0x61c3), 0x04 },
	{ CCI_REG8(0x61c6), 0x04 },
	{ CCI_REG8(0x61cf), 0x04 },
	{ CCI_REG8(0x61d2), 0x04 },
	{ CCI_REG8(0x61db), 0x04 },
	{ CCI_REG8(0x61de), 0x04 },
	{ CCI_REG8(0x61e7), 0x04 },
	{ CCI_REG8(0x61ea), 0x04 },
	{ CCI_REG8(0x61f3), 0x04 },
	{ CCI_REG8(0x61f6), 0x04 },
	{ CCI_REG8(0x61ff), 0x04 },
	{ CCI_REG8(0x6202), 0x04 },
	{ CCI_REG8(0x620b), 0x04 },
	{ CCI_REG8(0x620e), 0x04 },
	{ CCI_REG8(0x6217), 0x04 },
	{ CCI_REG8(0x621a), 0x04 },
	{ CCI_REG8(0x6223), 0x04 },
	{ CCI_REG8(0x6226), 0x04 },
	{ CCI_REG8(0x6b0b), 0x02 },
	{ CCI_REG8(0x6b0c), 0x01 },
	{ CCI_REG8(0x6b0d), 0x05 },
	{ CCI_REG8(0x6b0f), 0x04 },
	{ CCI_REG8(0x6b10), 0x02 },
	{ CCI_REG8(0x6b11), 0x06 },
	{ CCI_REG8(0x6b12), 0x03 },
	{ CCI_REG8(0x6b13), 0x07 },
	{ CCI_REG8(0x6b14), 0x0d },
	{ CCI_REG8(0x6b15), 0x09 },
	{ CCI_REG8(0x6b16), 0x0c },
	{ CCI_REG8(0x6b17), 0x08 },
	{ CCI_REG8(0x6b18), 0x0e },
	{ CCI_REG8(0x6b19), 0x0a },
	{ CCI_REG8(0x6b1a), 0x0f },
	{ CCI_REG8(0x6b1b), 0x0b },
	{ CCI_REG8(0x6b1c), 0x01 },
	{ CCI_REG8(0x6b1d), 0x05 },
	{ CCI_REG8(0x6b1f), 0x04 },
	{ CCI_REG8(0x6b20), 0x02 },
	{ CCI_REG8(0x6b21), 0x06 },
	{ CCI_REG8(0x6b22), 0x03 },
	{ CCI_REG8(0x6b23), 0x07 },
	{ CCI_REG8(0x6b24), 0x0d },
	{ CCI_REG8(0x6b25), 0x09 },
	{ CCI_REG8(0x6b26), 0x0c },
	{ CCI_REG8(0x6b27), 0x08 },
	{ CCI_REG8(0x6b28), 0x0e },
	{ CCI_REG8(0x6b29), 0x0a },
	{ CCI_REG8(0x6b2a), 0x0f },
	{ CCI_REG8(0x6b2b), 0x0b },
	{ CCI_REG8(0x7948), 0x01 },
	{ CCI_REG8(0x7949), 0x06 },
	{ CCI_REG8(0x794b), 0x04 },
	{ CCI_REG8(0x794c), 0x04 },
	{ CCI_REG8(0x794d), 0x3a },
	{ CCI_REG8(0x7951), 0x00 },
	{ CCI_REG8(0x7952), 0x01 },
	{ CCI_REG8(0x7955), 0x00 },
	{ CCI_REG8(0x9004), 0x10 },
	{ CCI_REG8(0x9200), 0xa0 },
	{ CCI_REG8(0x9201), 0xa7 },
	{ CCI_REG8(0x9202), 0xa0 },
	{ CCI_REG8(0x9203), 0xaa },
	{ CCI_REG8(0x9204), 0xa0 },
	{ CCI_REG8(0x9205), 0xad },
	{ CCI_REG8(0x9206), 0xa0 },
	{ CCI_REG8(0x9207), 0xb0 },
	{ CCI_REG8(0x9208), 0xa0 },
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
	{ CCI_REG8(0x921e), 0x7f },
	{ CCI_REG8(0x921f), 0x77 },
	{ CCI_REG8(0x99af), 0x0f },
	{ CCI_REG8(0x99b0), 0x0f },
	{ CCI_REG8(0x99b1), 0x0f },
	{ CCI_REG8(0x99b2), 0x0f },
	{ CCI_REG8(0x99b3), 0x0f },
	{ CCI_REG8(0x99e1), 0x0f },
	{ CCI_REG8(0x99e2), 0x0f },
	{ CCI_REG8(0x99e3), 0x0f },
	{ CCI_REG8(0x99e4), 0x0f },
	{ CCI_REG8(0x99e5), 0x0f },
	{ CCI_REG8(0x99e6), 0x0f },
	{ CCI_REG8(0x99e7), 0x0f },
	{ CCI_REG8(0x99e8), 0x0f },
	{ CCI_REG8(0x99e9), 0x0f },
	{ CCI_REG8(0x99ea), 0x0f },
	{ CCI_REG8(0xe286), 0x31 },
	{ CCI_REG8(0xe2a6), 0x32 },
	{ CCI_REG8(0xe2c6), 0x33 },
	{ CCI_REG8(0x4038), 0x00 },
	{ CCI_REG8(0x9856), 0xa0 },
	{ CCI_REG8(0x9857), 0x78 },
	{ CCI_REG8(0x9858), 0x64 },
	{ CCI_REG8(0x986e), 0x64 },
	{ CCI_REG8(0x9870), 0x3c },
	{ CCI_REG8(0x993a), 0x0e },
	{ CCI_REG8(0x993b), 0x0e },
	{ CCI_REG8(0x9953), 0x08 },
	{ CCI_REG8(0x9954), 0x08 },
	{ CCI_REG8(0x996b), 0x0f },
	{ CCI_REG8(0x996d), 0x0f },
	{ CCI_REG8(0x996f), 0x0f },
	{ CCI_REG8(0x998e), 0x0f },
	{ CCI_REG8(0xa101), 0x01 },
	{ CCI_REG8(0xa103), 0x01 },
	{ CCI_REG8(0xa105), 0x01 },
	{ CCI_REG8(0xa107), 0x01 },
	{ CCI_REG8(0xa109), 0x01 },
	{ CCI_REG8(0xa10b), 0x01 },
	{ CCI_REG8(0xa10d), 0x01 },
	{ CCI_REG8(0xa10f), 0x01 },
	{ CCI_REG8(0xa111), 0x01 },
	{ CCI_REG8(0xa113), 0x01 },
	{ CCI_REG8(0xa115), 0x01 },
	{ CCI_REG8(0xa117), 0x01 },
	{ CCI_REG8(0xa119), 0x01 },
	{ CCI_REG8(0xa11b), 0x01 },
	{ CCI_REG8(0xa11d), 0x01 },
	{ CCI_REG8(0xaa58), 0x00 },
	{ CCI_REG8(0xaa59), 0x01 },
	{ CCI_REG8(0xab03), 0x10 },
	{ CCI_REG8(0xab04), 0x10 },
	{ CCI_REG8(0xab05), 0x10 },
	{ CCI_REG8(0xad6a), 0x03 },
	{ CCI_REG8(0xad6b), 0xff },
	{ CCI_REG8(0xad77), 0x00 },
	{ CCI_REG8(0xad82), 0x03 },
	{ CCI_REG8(0xad83), 0xff },
	{ CCI_REG8(0xae06), 0x04 },
	{ CCI_REG8(0xae07), 0x16 },
	{ CCI_REG8(0xae08), 0xff },
	{ CCI_REG8(0xae09), 0x04 },
	{ CCI_REG8(0xae0a), 0x16 },
	{ CCI_REG8(0xae0b), 0xff },
	{ CCI_REG8(0xaf01), 0x04 },
	{ CCI_REG8(0xaf03), 0x0a },
	{ CCI_REG8(0xaf05), 0x18 },
	{ CCI_REG8(0xb048), 0x0a },

	// resolution 1 (2880x2156)
	{ CCI_REG8(0x0112), 0x0a },
	{ CCI_REG8(0x0113), 0x0a },
	{ CCI_REG8(0x0114), 0x03 },
	{ CCI_REG8(0x0342), 0x0c },
	{ CCI_REG8(0x0343), 0x58 },
	{ CCI_REG8(0x0340), 0x08 },
	{ CCI_REG8(0x0341), 0xa0 },
	{ CCI_REG8(0x0344), 0x00 },
	{ CCI_REG8(0x0345), 0x00 },
	{ CCI_REG8(0x0346), 0x00 },
	{ CCI_REG8(0x0347), 0x00 },
	{ CCI_REG8(0x0348), 0x16 },
	{ CCI_REG8(0x0349), 0x7f },
	{ CCI_REG8(0x034a), 0x10 },
	{ CCI_REG8(0x034b), 0xd7 },
	{ CCI_REG8(0x0220), 0x62 },
	{ CCI_REG8(0x0900), 0x01 },
	{ CCI_REG8(0x0901), 0x22 },
	{ CCI_REG8(0x0902), 0x08 },
	{ CCI_REG8(0x3140), 0x00 },
	{ CCI_REG8(0x3246), 0x81 },
	{ CCI_REG8(0x3247), 0x81 },
	{ CCI_REG8(0x0401), 0x00 },
	{ CCI_REG8(0x0404), 0x00 },
	{ CCI_REG8(0x0405), 0x10 },
	{ CCI_REG8(0x0408), 0x00 },
	{ CCI_REG8(0x0409), 0x00 },
	{ CCI_REG8(0x040a), 0x00 },
	{ CCI_REG8(0x040b), 0x00 },
	{ CCI_REG8(0x040c), 0x0b },
	{ CCI_REG8(0x040d), 0x40 },
	{ CCI_REG8(0x040e), 0x08 },
	{ CCI_REG8(0x040f), 0x6c },
	{ CCI_REG8(0x034c), 0x0b },
	{ CCI_REG8(0x034d), 0x40 },
	{ CCI_REG8(0x034e), 0x08 },
	{ CCI_REG8(0x034f), 0x6c },
	{ CCI_REG8(0x0301), 0x05 },
	{ CCI_REG8(0x0303), 0x04 },
	{ CCI_REG8(0x0305), 0x04 },
	{ CCI_REG8(0x0306), 0x00 },
	{ CCI_REG8(0x0307), 0xaf },
	{ CCI_REG8(0x030b), 0x02 },
	{ CCI_REG8(0x030d), 0x04 },
	{ CCI_REG8(0x030e), 0x00 },
	{ CCI_REG8(0x030f), 0xd1 },
	{ CCI_REG8(0x0310), 0x01 },
	{ CCI_REG8(0x0b06), 0x01 },
	{ CCI_REG8(0x3620), 0x00 },
	{ CCI_REG8(0x3f0c), 0x00 },
	{ CCI_REG8(0x3f14), 0x01 },
	{ CCI_REG8(0x3f80), 0x03 },
	{ CCI_REG8(0x3f81), 0xe8 },
	{ CCI_REG8(0x3ffc), 0x00 },
	{ CCI_REG8(0x3ffd), 0x26 },
	{ CCI_REG8(0x0202), 0x07 },
	{ CCI_REG8(0x0203), 0xd0 },
	{ CCI_REG8(0x0224), 0x01 },
	{ CCI_REG8(0x0225), 0xf4 },
	{ CCI_REG8(0x3fe0), 0x03 },
	{ CCI_REG8(0x3fe1), 0xe8 },
	{ CCI_REG8(0x0204), 0x00 },
	{ CCI_REG8(0x0205), 0x00 },
	{ CCI_REG8(0x0216), 0x00 },
	{ CCI_REG8(0x0217), 0x00 },
	{ CCI_REG8(0x0218), 0x01 },
	{ CCI_REG8(0x0219), 0x00 },
	{ CCI_REG8(0x020e), 0x01 },
	{ CCI_REG8(0x020f), 0x00 },
	{ CCI_REG8(0x3fe2), 0x00 },
	{ CCI_REG8(0x3fe3), 0x00 },
	{ CCI_REG8(0x3fe4), 0x01 },
	{ CCI_REG8(0x3fe5), 0x00 },
};

/* Supported sensor mode configurations */
static const struct imx576_mode supported_mode = {
	.width = 2880,
	.height = 2156,
	.hblank = 456, // FIXME
	.vblank = 506, // FIXME
	.vblank_min = 506, // FIXME
	.vblank_max = 32420, // FIXME
	.pclk = 619200000, // outputPixelClock?
	.link_freq_idx = 0,
	.code = MEDIA_BUS_FMT_SRGGB10_1X10,
	.reg_list = {
		.num_of_regs = ARRAY_SIZE(mode_2880x2156_regs),
		.regs = mode_2880x2156_regs,
	},
};

/**
 * to_imx576() - imx576 V4L2 sub-device to imx576 device.
 * @subdev: pointer to imx576 V4L2 sub-device
 *
 * Return: pointer to imx576 device
 */
static inline struct imx576 *to_imx576(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct imx576, sd);
}

/**
 * imx576_update_controls() - Update control ranges based on streaming mode
 * @imx576: pointer to imx576 device
 * @mode: pointer to imx576_mode sensor mode
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx576_update_controls(struct imx576 *imx576,
				  const struct imx576_mode *mode)
{
	int ret;

	ret = __v4l2_ctrl_s_ctrl(imx576->link_freq_ctrl, mode->link_freq_idx);
	if (ret)
		return ret;

	ret = __v4l2_ctrl_s_ctrl(imx576->hblank_ctrl, mode->hblank);
	if (ret)
		return ret;

	return __v4l2_ctrl_modify_range(imx576->vblank_ctrl, mode->vblank_min,
					mode->vblank_max, 1, mode->vblank);
}

/**
 * imx576_update_exp_gain() - Set updated exposure and gain
 * @imx576: pointer to imx576 device
 * @exposure: updated exposure value
 * @gain: updated analog gain value
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx576_update_exp_gain(struct imx576 *imx576, u32 exposure, u32 gain)
{
	u32 lpfr;
	int ret = 0;

	lpfr = imx576->vblank + imx576->cur_mode->height;

	dev_dbg(imx576->dev, "Set exp %u, analog gain %u, lpfr %u\n",
		exposure, gain, lpfr);

	cci_write(imx576->regmap, IMX576_REG_HOLD, 1, &ret);
	if (ret)
		return ret;

	cci_write(imx576->regmap, IMX576_REG_LPFR, lpfr, &ret);
	if (ret)
		goto error_release_group_hold;

	cci_write(imx576->regmap, IMX576_REG_EXPOSURE, exposure, &ret);
	if (ret)
		goto error_release_group_hold;

	cci_write(imx576->regmap, IMX576_REG_ANALOG_GAIN, gain, &ret);

error_release_group_hold:
	cci_write(imx576->regmap, IMX576_REG_HOLD, 0, NULL);

	return ret;
}

/**
 * imx576_set_ctrl() - Set subdevice control
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
static int imx576_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx576 *imx576 =
		container_of(ctrl->handler, struct imx576, ctrl_handler);
	u32 analog_gain;
	u32 exposure;
	int ret;

	switch (ctrl->id) {
	case V4L2_CID_VBLANK:
		imx576->vblank = imx576->vblank_ctrl->val;

		dev_dbg(imx576->dev, "Received vblank %u, new lpfr %u\n",
			imx576->vblank,
			imx576->vblank + imx576->cur_mode->height);

		ret = __v4l2_ctrl_modify_range(imx576->exp_ctrl,
					       IMX576_EXPOSURE_MIN,
					       imx576->vblank +
					       imx576->cur_mode->height -
					       IMX576_EXPOSURE_OFFSET,
					       1, IMX576_EXPOSURE_DEFAULT);
		break;
	case V4L2_CID_EXPOSURE:
		/* Set controls only if sensor is in power on state */
		if (!pm_runtime_get_if_in_use(imx576->dev))
			return 0;

		exposure = ctrl->val;
		analog_gain = imx576->again_ctrl->val;

		dev_dbg(imx576->dev, "Received exp %u, analog gain %u\n",
			exposure, analog_gain);

		ret = imx576_update_exp_gain(imx576, exposure, analog_gain);

		pm_runtime_put(imx576->dev);

		break;
	default:
		dev_err(imx576->dev, "Invalid control %d\n", ctrl->id);
		ret = -EINVAL;
	}

	return ret;
}

/* V4l2 subdevice control ops*/
static const struct v4l2_ctrl_ops imx576_ctrl_ops = {
	.s_ctrl = imx576_set_ctrl,
};

/**
 * imx576_enum_mbus_code() - Enumerate V4L2 sub-device mbus codes
 * @sd: pointer to imx576 V4L2 sub-device structure
 * @sd_state: V4L2 sub-device configuration
 * @code: V4L2 sub-device code enumeration need to be filled
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx576_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index > 0)
		return -EINVAL;

	code->code = supported_mode.code;

	return 0;
}

/**
 * imx576_enum_frame_size() - Enumerate V4L2 sub-device frame sizes
 * @sd: pointer to imx576 V4L2 sub-device structure
 * @sd_state: V4L2 sub-device configuration
 * @fsize: V4L2 sub-device size enumeration need to be filled
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx576_enum_frame_size(struct v4l2_subdev *sd,
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
 * imx576_fill_pad_format() - Fill subdevice pad format
 *                            from selected sensor mode
 * @imx576: pointer to imx576 device
 * @mode: pointer to imx576_mode sensor mode
 * @fmt: V4L2 sub-device format need to be filled
 */
static void imx576_fill_pad_format(struct imx576 *imx576,
				   const struct imx576_mode *mode,
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
 * imx576_get_pad_format() - Get subdevice pad format
 * @sd: pointer to imx576 V4L2 sub-device structure
 * @sd_state: V4L2 sub-device configuration
 * @fmt: V4L2 sub-device format need to be set
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx576_get_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_format *fmt)
{
	struct imx576 *imx576 = to_imx576(sd);

	mutex_lock(&imx576->mutex);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		struct v4l2_mbus_framefmt *framefmt;

		framefmt = v4l2_subdev_state_get_format(sd_state, fmt->pad);
		fmt->format = *framefmt;
	} else {
		imx576_fill_pad_format(imx576, imx576->cur_mode, fmt);
	}

	mutex_unlock(&imx576->mutex);

	return 0;
}

/**
 * imx576_set_pad_format() - Set subdevice pad format
 * @sd: pointer to imx576 V4L2 sub-device structure
 * @sd_state: V4L2 sub-device configuration
 * @fmt: V4L2 sub-device format need to be set
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx576_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_format *fmt)
{
	struct imx576 *imx576 = to_imx576(sd);
	const struct imx576_mode *mode;
	int ret = 0;

	mutex_lock(&imx576->mutex);

	mode = &supported_mode;
	imx576_fill_pad_format(imx576, mode, fmt);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		struct v4l2_mbus_framefmt *framefmt;

		framefmt = v4l2_subdev_state_get_format(sd_state, fmt->pad);
		*framefmt = fmt->format;
	} else {
		ret = imx576_update_controls(imx576, mode);
		if (!ret)
			imx576->cur_mode = mode;
	}

	mutex_unlock(&imx576->mutex);

	return ret;
}

/**
 * imx576_init_state() - Initialize sub-device state
 * @sd: pointer to imx576 V4L2 sub-device structure
 * @sd_state: V4L2 sub-device configuration
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx576_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *sd_state)
{
	struct imx576 *imx576 = to_imx576(sd);
	struct v4l2_subdev_format fmt = { 0 };

	fmt.which = sd_state ? V4L2_SUBDEV_FORMAT_TRY : V4L2_SUBDEV_FORMAT_ACTIVE;
	imx576_fill_pad_format(imx576, &supported_mode, &fmt);

	return imx576_set_pad_format(sd, sd_state, &fmt);
}

/**
 * imx576_start_streaming() - Start sensor stream
 * @imx576: pointer to imx576 device
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx576_start_streaming(struct imx576 *imx576)
{
	const struct imx576_reg_list *reg_list;
	int ret;

	/* Write sensor mode registers */
	reg_list = &imx576->cur_mode->reg_list;
	ret = cci_multi_reg_write(imx576->regmap, reg_list->regs,
				  reg_list->num_of_regs, NULL);
	if (ret) {
		dev_err(imx576->dev, "fail to write initial registers\n");
		return ret;
	}

	/* Setup handler will write actual exposure and gain */
	ret =  __v4l2_ctrl_handler_setup(imx576->sd.ctrl_handler);
	if (ret) {
		dev_err(imx576->dev, "fail to setup handler\n");
		return ret;
	}

	/* Delay is required before streaming*/
	usleep_range(7400, 8000);

	/* Start streaming */
	cci_write(imx576->regmap, IMX576_REG_MODE_SELECT, IMX576_MODE_STREAMING, &ret);
	if (ret) {
		dev_err(imx576->dev, "fail to start streaming\n");
		return ret;
	}

	return 0;
}

/**
 * imx576_stop_streaming() - Stop sensor stream
 * @imx576: pointer to imx576 device
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx576_stop_streaming(struct imx576 *imx576)
{
	return cci_write(imx576->regmap, IMX576_REG_MODE_SELECT,
			 IMX576_MODE_STANDBY, NULL);
}

/**
 * imx576_set_stream() - Enable sensor streaming
 * @sd: pointer to imx576 subdevice
 * @enable: set to enable sensor streaming
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx576_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct imx576 *imx576 = to_imx576(sd);
	int ret;

	mutex_lock(&imx576->mutex);

	if (enable) {
		ret = pm_runtime_resume_and_get(imx576->dev);
		if (ret)
			goto error_unlock;

		ret = imx576_start_streaming(imx576);
		if (ret)
			goto error_power_off;
	} else {
		imx576_stop_streaming(imx576);
		pm_runtime_put(imx576->dev);
	}

	mutex_unlock(&imx576->mutex);

	return 0;

error_power_off:
	pm_runtime_put(imx576->dev);
error_unlock:
	mutex_unlock(&imx576->mutex);

	return ret;
}

/**
 * imx576_detect() - Detect imx576 sensor
 * @imx576: pointer to imx576 device
 *
 * Return: 0 if successful, -EIO if sensor id does not match
 */
static int imx576_detect(struct imx576 *imx576)
{
	int ret;
	u64 val;

	ret = cci_read(imx576->regmap, IMX576_REG_CHIP_ID, &val, NULL);
	if (ret)
		return ret;

	if (val != IMX576_CHIP_ID) {
		dev_err(imx576->dev, "chip id mismatch: %x!=%llx\n",
			IMX576_CHIP_ID, val);
		return -ENXIO;
	}

	return 0;
}

/**
 * imx576_parse_hw_config() - Parse HW configuration and check if supported
 * @imx576: pointer to imx576 device
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx576_parse_hw_config(struct imx576 *imx576)
{
	struct fwnode_handle *fwnode = dev_fwnode(imx576->dev);
	struct v4l2_fwnode_endpoint bus_cfg = {};
	struct fwnode_handle *ep;
	unsigned long rate;
	unsigned int i;
	int ret;

	if (!fwnode)
		return -ENXIO;

	/* Request optional reset pin */
	imx576->reset_gpio = devm_gpiod_get_optional(imx576->dev, "reset",
						     GPIOD_OUT_LOW);
	if (IS_ERR(imx576->reset_gpio)) {
		dev_err(imx576->dev, "failed to get reset gpio %ld\n",
			PTR_ERR(imx576->reset_gpio));
		return PTR_ERR(imx576->reset_gpio);
	}

	/* Get sensor input clock */
	imx576->inclk = devm_clk_get(imx576->dev, NULL);
	if (IS_ERR(imx576->inclk)) {
		dev_err(imx576->dev, "could not get inclk\n");
		return PTR_ERR(imx576->inclk);
	}

	rate = clk_get_rate(imx576->inclk);
	if (rate != IMX576_INCLK_RATE) {
		dev_err(imx576->dev, "inclk frequency mismatch\n");
		return -EINVAL;
	}

	/* Get optional DT defined regulators */
	for (i = 0; i < ARRAY_SIZE(imx576_supply_names); i++)
		imx576->supplies[i].supply = imx576_supply_names[i];

	ret = devm_regulator_bulk_get(imx576->dev,
				      ARRAY_SIZE(imx576_supply_names),
				      imx576->supplies);
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
		dev_err(imx576->dev, "selected bus-type is not supported\n");
		ret = -EINVAL;
		goto done_endpoint_free;
	}

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != IMX576_NUM_DATA_LANES) {
		dev_err(imx576->dev,
			"number of CSI2 data lanes %d is not supported\n",
			bus_cfg.bus.mipi_csi2.num_data_lanes);
		ret = -EINVAL;
		goto done_endpoint_free;
	}

	if (!bus_cfg.nr_of_link_frequencies) {
		dev_err(imx576->dev, "no link frequencies defined\n");
		ret = -EINVAL;
		goto done_endpoint_free;
	}

	for (i = 0; i < bus_cfg.nr_of_link_frequencies; i++)
		if (bus_cfg.link_frequencies[i] == IMX576_LINK_FREQ)
			goto done_endpoint_free;

	ret = -EINVAL;

done_endpoint_free:
	v4l2_fwnode_endpoint_free(&bus_cfg);

	return ret;
}

/* V4l2 subdevice ops */
static const struct v4l2_subdev_video_ops imx576_video_ops = {
	.s_stream = imx576_set_stream,
};

static const struct v4l2_subdev_pad_ops imx576_pad_ops = {
	.enum_mbus_code = imx576_enum_mbus_code,
	.enum_frame_size = imx576_enum_frame_size,
	.get_fmt = imx576_get_pad_format,
	.set_fmt = imx576_set_pad_format,
};

static const struct v4l2_subdev_ops imx576_subdev_ops = {
	.video = &imx576_video_ops,
	.pad = &imx576_pad_ops,
};

static const struct v4l2_subdev_internal_ops imx576_internal_ops = {
	.init_state = imx576_init_state,
};

/**
 * imx576_power_on() - Sensor power on sequence
 * @dev: pointer to i2c device
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx576_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx576 *imx576 = to_imx576(sd);
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(imx576_supply_names),
				    imx576->supplies);
	if (ret < 0) {
		dev_err(dev, "failed to enable regulators\n");
		return ret;
	}

	gpiod_set_value_cansleep(imx576->reset_gpio, 0);

	ret = clk_prepare_enable(imx576->inclk);
	if (ret) {
		dev_err(imx576->dev, "fail to enable inclk\n");
		goto error_reset;
	}

	usleep_range(1000, 1200);

	return 0;

error_reset:
	gpiod_set_value_cansleep(imx576->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(imx576_supply_names),
			       imx576->supplies);

	return ret;
}

/**
 * imx576_power_off() - Sensor power off sequence
 * @dev: pointer to i2c device
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx576_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx576 *imx576 = to_imx576(sd);

	clk_disable_unprepare(imx576->inclk);

	gpiod_set_value_cansleep(imx576->reset_gpio, 1);

	regulator_bulk_disable(ARRAY_SIZE(imx576_supply_names),
			       imx576->supplies);

	return 0;
}

/**
 * imx576_init_controls() - Initialize sensor subdevice controls
 * @imx576: pointer to imx576 device
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx576_init_controls(struct imx576 *imx576)
{
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl_handler *ctrl_hdlr = &imx576->ctrl_handler;
	const struct imx576_mode *mode = imx576->cur_mode;
	u32 lpfr;
	int ret;

	/* set properties from fwnode (e.g. rotation, orientation) */
	ret = v4l2_fwnode_device_parse(imx576->dev, &props);
	if (ret)
		return ret;

	ret = v4l2_ctrl_handler_init(ctrl_hdlr, 8);
	if (ret)
		return ret;

	/* Serialize controls with sensor device */
	ctrl_hdlr->lock = &imx576->mutex;

	/* Initialize exposure and gain */
	lpfr = mode->vblank + mode->height;
	imx576->exp_ctrl = v4l2_ctrl_new_std(ctrl_hdlr,
					     &imx576_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     IMX576_EXPOSURE_MIN,
					     lpfr - IMX576_EXPOSURE_OFFSET,
					     IMX576_EXPOSURE_STEP,
					     IMX576_EXPOSURE_DEFAULT);

	imx576->again_ctrl = v4l2_ctrl_new_std(ctrl_hdlr,
					       &imx576_ctrl_ops,
					       V4L2_CID_ANALOGUE_GAIN,
					       IMX576_ANA_GAIN_MIN,
					       IMX576_ANA_GAIN_MAX,
					       IMX576_ANA_GAIN_STEP,
					       IMX576_ANA_GAIN_DEFAULT);

	v4l2_ctrl_cluster(2, &imx576->exp_ctrl);

	imx576->vblank_ctrl = v4l2_ctrl_new_std(ctrl_hdlr,
						&imx576_ctrl_ops,
						V4L2_CID_VBLANK,
						mode->vblank_min,
						mode->vblank_max,
						1, mode->vblank);

	/* Read only controls */
	imx576->pclk_ctrl = v4l2_ctrl_new_std(ctrl_hdlr,
					      &imx576_ctrl_ops,
					      V4L2_CID_PIXEL_RATE,
					      mode->pclk, mode->pclk,
					      1, mode->pclk);

	imx576->link_freq_ctrl = v4l2_ctrl_new_int_menu(ctrl_hdlr,
							&imx576_ctrl_ops,
							V4L2_CID_LINK_FREQ,
							ARRAY_SIZE(link_freq) -
							1,
							mode->link_freq_idx,
							link_freq);
	if (imx576->link_freq_ctrl)
		imx576->link_freq_ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	imx576->hblank_ctrl = v4l2_ctrl_new_std(ctrl_hdlr,
						&imx576_ctrl_ops,
						V4L2_CID_HBLANK,
						IMX576_REG_MIN,
						IMX576_REG_MAX,
						1, mode->hblank);
	if (imx576->hblank_ctrl)
		imx576->hblank_ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &imx576_ctrl_ops, &props);

	if (ctrl_hdlr->error) {
		dev_err(imx576->dev, "control init failed: %d\n",
			ctrl_hdlr->error);
		v4l2_ctrl_handler_free(ctrl_hdlr);
		return ctrl_hdlr->error;
	}

	imx576->sd.ctrl_handler = ctrl_hdlr;

	return 0;
}

/**
 * imx576_probe() - I2C client device binding
 * @client: pointer to i2c client device
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx576_probe(struct i2c_client *client)
{
	struct imx576 *imx576;
	int ret;

	imx576 = devm_kzalloc(&client->dev, sizeof(*imx576), GFP_KERNEL);
	if (!imx576)
		return -ENOMEM;

	imx576->dev = &client->dev;

	/* Initialize subdev */
	v4l2_i2c_subdev_init(&imx576->sd, client, &imx576_subdev_ops);
	imx576->sd.internal_ops = &imx576_internal_ops;

	ret = imx576_parse_hw_config(imx576);
	if (ret) {
		dev_err(imx576->dev, "HW configuration is not supported\n");
		return ret;
	}

	mutex_init(&imx576->mutex);

	imx576->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(imx576->regmap))
		return dev_err_probe(imx576->dev, PTR_ERR(imx576->regmap),
				     "failed to initialize CCI\n");

	ret = imx576_power_on(imx576->dev);
	if (ret) {
		dev_err(imx576->dev, "failed to power-on the sensor\n");
		goto error_mutex_destroy;
	}

	/* Check module identity */
	ret = imx576_detect(imx576);
	if (ret) {
		dev_err(imx576->dev, "failed to find sensor: %d\n", ret);
		goto error_power_off;
	}

	/* Set default mode to max resolution */
	imx576->cur_mode = &supported_mode;
	imx576->vblank = imx576->cur_mode->vblank;

	ret = imx576_init_controls(imx576);
	if (ret) {
		dev_err(imx576->dev, "failed to init controls: %d\n", ret);
		goto error_power_off;
	}

	/* Initialize subdev */
	imx576->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	imx576->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	/* Initialize source pad */
	imx576->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&imx576->sd.entity, 1, &imx576->pad);
	if (ret) {
		dev_err(imx576->dev, "failed to init entity pads: %d\n", ret);
		goto error_handler_free;
	}

	ret = v4l2_async_register_subdev_sensor(&imx576->sd);
	if (ret < 0) {
		dev_err(imx576->dev,
			"failed to register async subdev: %d\n", ret);
		goto error_media_entity;
	}

	pm_runtime_set_active(imx576->dev);
	pm_runtime_enable(imx576->dev);
	pm_runtime_idle(imx576->dev);

	return 0;

error_media_entity:
	media_entity_cleanup(&imx576->sd.entity);
error_handler_free:
	v4l2_ctrl_handler_free(imx576->sd.ctrl_handler);
error_power_off:
	imx576_power_off(imx576->dev);
error_mutex_destroy:
	mutex_destroy(&imx576->mutex);

	return ret;
}

static void imx576_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx576 *imx576 = to_imx576(sd);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(sd->ctrl_handler);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		imx576_power_off(&client->dev);
	pm_runtime_set_suspended(&client->dev);

	mutex_destroy(&imx576->mutex);
}

static const struct dev_pm_ops imx576_pm_ops = {
	SET_RUNTIME_PM_OPS(imx576_power_off, imx576_power_on, NULL)
};

static const struct of_device_id imx576_of_match[] = {
	{ .compatible = "sony,imx576" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, imx576_of_match);

static struct i2c_driver imx576_driver = {
	.probe = imx576_probe,
	.remove = imx576_remove,
	.driver = {
		.name = "imx576",
		.pm = &imx576_pm_ops,
		.of_match_table = imx576_of_match,
	},
};

module_i2c_driver(imx576_driver);

MODULE_DESCRIPTION("Sony IMX576 sensor driver");
MODULE_LICENSE("GPL");
