/* SPDX-License-Identifier: GPL-2.0-only */
//
// aw88264.h  --  ALSA SoC AW88264 codec support
//
// Register map for the Awinic AW88264 smart PA (chip id 0x1852).
//
// Derived mechanically from the vendor driver's aw882xx_reg.h rather than
// transcribed, because 61 addresses copied by hand is 61 chances to be wrong.
// Mainline's aw88xxx drivers cover a later generation (0x2049..0x2183) whose
// map diverges from this one above 0x05; aw88261.h is the template for style,
// not a source of addresses.
//
#ifndef __AW88264_H__
#define __AW88264_H__

/* chip id, read from AW88264_ID_REG */
#define AW88264_CHIP_ID			(0x1852)

/* registers */
#define AW88264_ID_REG		(0x00)  /* read-only */
#define AW88264_SYSST_REG		(0x01)  /* read-only */
#define AW88264_SYSINT_REG		(0x02)  /* read-only */
#define AW88264_SYSINTM_REG		(0x03)
#define AW88264_SYSCTRL_REG		(0x04)
#define AW88264_SYSCTRL2_REG		(0x05)
#define AW88264_I2SCTRL_REG		(0x06)
#define AW88264_I2SCFG1_REG		(0x07)
#define AW88264_I2SCFG2_REG		(0x08)
#define AW88264_HAGCCFG1_REG		(0x09)
#define AW88264_HAGCCFG2_REG		(0x0a)
#define AW88264_HAGCCFG3_REG		(0x0b)
#define AW88264_HAGCCFG4_REG		(0x0c)
#define AW88264_HAGCCFG5_REG		(0x0d)
#define AW88264_HAGCCFG6_REG		(0x0e)
#define AW88264_HAGCCFG7_REG		(0x0f)
#define AW88264_HAGCST_REG		(0x10)  /* read-only */
#define AW88264_PRODID_REG		(0x11)  /* read-only */
#define AW88264_VBAT_REG		(0x12)  /* read-only */
#define AW88264_TEMP_REG		(0x13)  /* read-only */
#define AW88264_PVDD_REG		(0x14)  /* read-only */
#define AW88264_DBGCTRL_REG		(0x20)
#define AW88264_I2SINT_REG		(0x21)  /* read-only */
#define AW88264_I2SCAPCNT_REG		(0x22)  /* read-only */
#define AW88264_CRCIN_REG		(0x38)
#define AW88264_CRCOUT_REG		(0x39)  /* read-only */
#define AW88264_VSNCTRL1_REG		(0x50)
#define AW88264_ISNCTRL1_REG		(0x52)
#define AW88264_ISNCTRL2_REG		(0x53)
#define AW88264_VTMCTRL1_REG		(0x54)
#define AW88264_VTMCTRL2_REG		(0x55)
#define AW88264_VTMCTRL3_REG		(0x56)
#define AW88264_ISNDAT_REG		(0x57)  /* read-only */
#define AW88264_VSNDAT_REG		(0x58)  /* read-only */
#define AW88264_PWMCTRL_REG		(0x59)
#define AW88264_PWMCTRL2_REG		(0x5a)
#define AW88264_BSTCTRL1_REG		(0x60)
#define AW88264_BSTCTRL2_REG		(0x61)
#define AW88264_BSTCTRL3_REG		(0x62)
#define AW88264_BSTDBG1_REG		(0x63)
#define AW88264_BSTDBG2_REG		(0x64)
#define AW88264_BSTDBG3_REG		(0x65)
#define AW88264_PLLCTRL1_REG		(0x66)
#define AW88264_PLLCTRL2_REG		(0x67)
#define AW88264_PLLCTRL3_REG		(0x68)
#define AW88264_CDACTRL1_REG		(0x69)
#define AW88264_CDACTRL2_REG		(0x6a)
#define AW88264_SADCCTRL_REG		(0x6b)
#define AW88264_TESTCTRL1_REG		(0x70)
#define AW88264_TESTCTRL2_REG		(0x71)
#define AW88264_EFCTRL1_REG		(0x72)
#define AW88264_EFCTRL2_REG		(0x73)
#define AW88264_EFWH_REG		(0x74)
#define AW88264_EFWM2_REG		(0x75)
#define AW88264_EFWM1_REG		(0x76)
#define AW88264_EFWL_REG		(0x77)
#define AW88264_EFRH_REG		(0x78)  /* read-only */
#define AW88264_EFRM2_REG		(0x79)  /* read-only */
#define AW88264_EFRM1_REG		(0x7a)  /* read-only */
#define AW88264_EFRL_REG		(0x7b)  /* read-only */
#define AW88264_TESTDET_REG		(0x7c)  /* read-only */

#define AW88264_REG_MAX			(0x7d)

/*
 * Bit fields, for the registers a codec driver actually touches.
 *
 * Attribution here is not guesswork: the vendor header groups fields under a
 * "default value of <REG>" marker and labels the interesting ones inline, e.g.
 * "VOL bit 15:8 (HAGCCFG4 0x0C)". Each field below was placed by reading those
 * boundaries, not by assuming it sits where the same-named field sits in
 * aw88261 -- where, for the two that matter most, it does not:
 *
 *   field   AW88264 (0x1852)          aw88261 (0x2113)
 *   VOL     HAGCCFG4 0x0C, bits 15:8  SYSCTRL2 0x05, bits 8:0
 *   step    6 dB per 12 units         6 dB per 48 units
 *
 * Reusing aw88261's volume path on this part would write the wrong bits of
 * the wrong register with the wrong scaling. That difference is most of the
 * reason this is a separate driver rather than a compatible string.
 */

/* SYSCTRL (0x04) */
#define AW88264_PWDN_START_BIT		(0)
#define AW88264_PWDN_BITS_LEN		(1)
#define AW88264_PWDN_MASK		\
	(~(((1 << AW88264_PWDN_BITS_LEN) - 1) << AW88264_PWDN_START_BIT))
#define AW88264_PWDN_POWER_DOWN		(1)
#define AW88264_PWDN_NORMAL_WORKING	(0)

#define AW88264_AMPPD_START_BIT		(1)
#define AW88264_AMPPD_BITS_LEN		(1)
#define AW88264_AMPPD_MASK		\
	(~(((1 << AW88264_AMPPD_BITS_LEN) - 1) << AW88264_AMPPD_START_BIT))
#define AW88264_AMPPD_POWER_DOWN	(1)
#define AW88264_AMPPD_NORMAL_WORKING	(0)

/*
 * The I2S receiver's own enable. Distinct from the two power bits above, and
 * easy to miss: with PWDN and AMPPD clear and HMUTE clear, the part is powered
 * and unmuted and still completely silent, because nothing is being clocked
 * into it. Measured on hardware -- SYSCTRL read 0x4000 during playback, bit 6
 * clear, no sound.
 */
#define AW88264_I2SEN_START_BIT		(6)
#define AW88264_I2SEN_BITS_LEN		(1)
#define AW88264_I2SEN_MASK		\
	(~(((1 << AW88264_I2SEN_BITS_LEN) - 1) << AW88264_I2SEN_START_BIT))
#define AW88264_I2SEN_ENABLE		(1)
#define AW88264_I2SEN_DISABLE		(0)

/* SYSCTRL2 (0x05) */
#define AW88264_HMUTE_START_BIT		(4)
#define AW88264_HMUTE_BITS_LEN		(1)
#define AW88264_HMUTE_MASK		\
	(~(((1 << AW88264_HMUTE_BITS_LEN) - 1) << AW88264_HMUTE_START_BIT))
#define AW88264_HMUTE_ENABLE		(1)
#define AW88264_HMUTE_DISABLE		(0)

/* HAGCCFG4 (0x0C) -- volume lives here, NOT in SYSCTRL2 */
#define AW88264_VOL_START_BIT		(8)
#define AW88264_VOL_BITS_LEN		(8)
#define AW88264_VOL_MASK		\
	(~(((1 << AW88264_VOL_BITS_LEN) - 1) << AW88264_VOL_START_BIT))
#define AW88264_VOL_DEFAULT_VALUE	(0)

/* 0 is loudest, -255 the quietest step the field can express */
#define AW88264_VOLUME_MAX		(0)
#define AW88264_VOLUME_MIN		(-255)
#define AW88264_VOLUME_STEP_6DB		(6 * 2)

/* I2SCTRL (0x06) -- mode, frame size, sample rate, and the CCO mux */
#define AW88264_I2SMD_START_BIT		(8)
#define AW88264_I2SMD_BITS_LEN		(2)
#define AW88264_I2SMD_MASK		\
	(~(((1 << AW88264_I2SMD_BITS_LEN) - 1) << AW88264_I2SMD_START_BIT))
#define AW88264_I2SMD_STANDARD_I2S	(0)
#define AW88264_I2SMD_MSB_JUSTIFIED	(1)
#define AW88264_I2SMD_LSB_JUSTIFIED	(2)

#define AW88264_I2SFS_START_BIT		(6)
#define AW88264_I2SFS_BITS_LEN		(2)
#define AW88264_I2SFS_MASK		\
	(~(((1 << AW88264_I2SFS_BITS_LEN) - 1) << AW88264_I2SFS_START_BIT))
#define AW88264_I2SFS_16_BITS		(0)
#define AW88264_I2SFS_20_BITS		(1)
#define AW88264_I2SFS_24_BITS		(2)
#define AW88264_I2SFS_32_BITS		(3)

#define AW88264_I2SSR_START_BIT		(0)
#define AW88264_I2SSR_BITS_LEN		(4)
#define AW88264_I2SSR_MASK		\
	(~(((1 << AW88264_I2SSR_BITS_LEN) - 1) << AW88264_I2SSR_START_BIT))
#define AW88264_I2SSR_8KHZ		(0)
#define AW88264_I2SSR_11P025KHZ		(1)
#define AW88264_I2SSR_12KHZ		(2)
#define AW88264_I2SSR_16KHZ		(3)
#define AW88264_I2SSR_22P05KHZ		(4)
#define AW88264_I2SSR_24KHZ		(5)
#define AW88264_I2SSR_32KHZ		(6)
#define AW88264_I2SSR_44P1KHZ		(7)
#define AW88264_I2SSR_48KHZ		(8)
#define AW88264_I2SSR_96KHZ		(9)
#define AW88264_I2SSR_192KHZ		(10)

/*
 * The internal clock mux has to be told whether the rate is one of the
 * 8/16/32 kHz family or anything else. The vendor driver sets it alongside
 * every rate change and never explains why; it is reproduced because the
 * hardware is not here to be asked.
 */
#define AW88264_I2S_CCO_MUX_START_BIT	(14)
#define AW88264_I2S_CCO_MUX_BITS_LEN	(1)
#define AW88264_I2S_CCO_MUX_MASK	\
	(~(((1 << AW88264_I2S_CCO_MUX_BITS_LEN) - 1) << AW88264_I2S_CCO_MUX_START_BIT))
#define AW88264_I2S_CCO_MUX_8_16_32KHZ	(0)
#define AW88264_I2S_CCO_MUX_OTHER	(1)

/*
 * Volume encoding. The 8-bit field is two nibbles: [7:4] steps of -6 dB,
 * [3:0] steps of -0.5 dB. So the driver works in 0.5 dB units internally and
 * packs them on the way out.
 */
#define AW88264_VOL_COARSE_SHIFT	(4)
#define AW88264_VOL_FINE_MASK		(0x0f)
#define AW88264_VOL_STEPS_PER_6DB	(12)
#define AW88264_VOL_MAX_STEPS		(191)	/* 15 * 12 + 11 = -95.5 dB */
/*
 * 191, not 195. The coarse nibble holds 0-15 and the fine nibble counts
 * 0.5 dB steps within one 6 dB stride, so it only ever holds 0-11: the
 * largest attenuation the field can express is 15 * 12 + 11. Asking for
 * 195 makes the coarse term 195 / 12 = 16, which does not fit in four
 * bits and truncates to 0 -- so the quietest setting on the control
 * wrote a near-maximum one to the part. Found on hardware; the speaker
 * on this board is loud enough that this is a safety bug, not a
 * cosmetic one.
 */

#endif /* __AW88264_H__ */
