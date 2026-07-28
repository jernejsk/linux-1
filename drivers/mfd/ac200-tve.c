// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for the TV encoder in the X-Powers AC200
 *
 * Copyright (c) 2026 Jernej Skrabec <jernej.skrabec@gmail.com>
 *
 * The encoder normally takes CCIR656 video from the SoC and turns it into
 * composite video. It also contains a standard colour bar generator, which
 * replaces that input, and that is all this driver drives for now: it brings
 * the encoder up and puts the colour bars on the CVBS output, so that the
 * analog side can be tested before the CCIR656 input is wired up.
 *
 * The register documentation covers the encoder itself, but the clock tree
 * setup and the second register block at 0x4100 are undocumented, so those
 * sequences are taken verbatim from the vendor driver.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>

/* system registers */
#define AC200_SYS_PLL_CTL0		0x000c
#define AC200_SYS_PLL_CTL0_ENABLE	BIT(15)
#define AC200_SYS_PLL_CTL0_BIAS_EN	BIT(14)
#define AC200_SYS_PLL_CTL0_LDO1_EN	BIT(11)
#define AC200_SYS_PLL_CTL0_LDO_EN	BIT(10)
#define AC200_SYS_PLL_CTL0_POST_DIV9	BIT(9)
#define AC200_SYS_PLL_CTL0_PRE_DIV_M	GENMASK(3, 0)
#define AC200_SYS_PLL_CTL0_PRE_DIV_M_1	1
#define AC200_SYS_TVE_CTL0		0x0018
#define AC200_SYS_TVE_CTL0_SYSCLK_GATING	BIT(3)
#define AC200_SYS_TVE_CTL0_SCLK_GATING		BIT(2)
#define AC200_SYS_TVE_CTL0_DCLK_GATING		BIT(1)
#define AC200_SYS_TVE_CTL0_RESET_INVALID	BIT(0)
#define AC200_SYS_TVE_CTL1		0x001a
#define AC200_SYS_UNKNOWN_40		0x0040

/* TV encoder registers */
#define AC200_TVE_CTL0			0x4000
#define AC200_TVE_CTL0_UNKNOWN		GENMASK(9, 8)
#define AC200_TVE_CTL0_EN		BIT(0)
#define AC200_TVE_CTL1			0x4002
#define AC200_TVE_CTL1_CLK_DISABLE	BIT(15)
#define AC200_TVE_MOD0			0x4004
#define AC200_TVE_MOD0_COLOR_BAR_TYPE	BIT(9)
#define AC200_TVE_MOD0_COLOR_BAR_MOD	BIT(8)
#define AC200_TVE_MOD0_TV_MOD_PAL	BIT(0)
#define AC200_TVE_MOD1			0x4006
#define AC200_TVE_DAC_CFG0		0x4008
#define AC200_TVE_DAC_CFG0_EN		BIT(0)
#define AC200_TVE_DAC_CFG1		0x400a
#define AC200_TVE_YC_DELAY		0x400c
#define AC200_TVE_YC_FILTER		0x400e
#define AC200_TVE_BURST_FRQ0		0x4010
#define AC200_TVE_BURST_FRQ1		0x4012
#define AC200_TVE_FRONT_PORCH		0x4014
#define AC200_TVE_BACK_PORCH		0x4016
#define AC200_TVE_TOTAL_LINE		0x401c
#define AC200_TVE_FIRST_ACTIVE		0x401e
#define AC200_TVE_BLACK_LEVEL		0x4020
#define AC200_TVE_BLANK_LEVEL		0x4022
#define AC200_TVE_PLUG_EN		0x4030
#define AC200_TVE_PLUG_EN_AUTO_DET	BIT(0)
#define AC200_TVE_PLUG_STA		0x4038
#define AC200_TVE_PLUG_STA_MASK		GENMASK(1, 0)
#define AC200_TVE_PLUG_DEBOUNCE		0x4040
#define AC200_TVE_PLUG_PULSE_LEVEL	0x40f4
#define AC200_TVE_PLUG_PULSE_START	0x40f8
#define AC200_TVE_PLUG_PULSE_PERIOD	0x40fa

/* undocumented encoder block, values below come from the vendor driver */
#define AC200_TVE_UNKNOWN_4130		0x4130
#define AC200_TVE_UNKNOWN_4132		0x4132
#define AC200_TVE_CALIBRATION		0x4306
#define AC200_TVE_CALIBRATION_MASK	GENMASK(9, 0)
#define AC200_TVE_CALIBRATION_DEFAULT	0x28f
#define AC200_TVE_UNKNOWN_43A0		0x43a0
#define AC200_TVE_UNKNOWN_43A2		0x43a2

/* encoder input interface registers */
#define AC200_TVE_IF_CTL		0x5000
#define AC200_TVE_IF_CTL_SEL_SYUV	BIT(0)
#define AC200_TVE_IF_TIM0		0x5008
#define AC200_TVE_IF_TIM1		0x500a
#define AC200_TVE_IF_TIM2		0x500c
#define AC200_TVE_IF_TIM3		0x500e
#define AC200_TVE_IF_SYNC0		0x5010
#define AC200_TVE_IF_SYNC1		0x5012
#define AC200_TVE_IF_SYNC2		0x5014
#define AC200_TVE_IF_TIM4		0x5016

/* eFuse copy of the DAC calibration */
#define AC200_EFUSE_TVE			0x8002

/*
 * The CCIR656 input carries two bytes per pixel, so the horizontal timings
 * the encoder is programmed with are twice the pixel counts.
 */
#define AC200_TVE_CCIR_BYTES_PER_PIXEL	2

enum ac200_tve_mode {
	AC200_TVE_NTSC,
	AC200_TVE_PAL,
};

struct ac200_tve_timing {
	const char *name;
	u16 hactive;
	u16 vactive;
	u16 hbp;	/* back porch plus sync, in pixels */
	u16 vbp;	/* back porch plus sync, in lines */
	u16 mod0;
	const struct reg_sequence *regs;
	unsigned int num_regs;
};

/*
 * Encoder settings per TV standard. Everything from 0x4100 on is undocumented
 * and is replayed exactly as the vendor driver writes it.
 *
 * TVE_TOTAL_LINE is written for both standards, as the vendor driver does, but
 * it always reads back 0x20d whatever is written to it. The encoder appears to
 * take the line count from TV_MOD instead.
 */
static const struct reg_sequence ac200_tve_ntsc_regs[] = {
	{ AC200_TVE_MOD1,		0x0707 },
	{ AC200_TVE_DAC_CFG1,		0x4300 },
	{ AC200_TVE_YC_DELAY,		0x1400 },
	{ AC200_TVE_YC_FILTER,		0x3000 },
	{ AC200_TVE_BURST_FRQ0,		0x7c1f },
	{ AC200_TVE_BURST_FRQ1,		0x21f0 },
	{ AC200_TVE_FRONT_PORCH,	0x0020 },
	{ AC200_TVE_BACK_PORCH,		0x0076 },
	{ 0x4018,			0x0016 },
	{ 0x401a,			0x0000 },
	{ AC200_TVE_TOTAL_LINE,		0x020d },	/* see note below */
	{ AC200_TVE_FIRST_ACTIVE,	0x0016 },
	{ AC200_TVE_BLACK_LEVEL,	0x011a },
	{ AC200_TVE_BLANK_LEVEL,	0x00f0 },
	{ 0x4100,			0x0001 },
	{ 0x4102,			0x0000 },
	{ 0x4104,			0x0000 },
	{ 0x4106,			0x0000 },
	{ 0x4108,			0x0002 },
	{ 0x410a,			0x0000 },
	{ 0x410c,			0x004f },
	{ 0x410e,			0x0000 },
	{ 0x4110,			0x0000 },
	{ 0x4112,			0x0000 },
	{ 0x4114,			0x447e },
	{ 0x4116,			0x0016 },
	{ 0x4118,			0xa0a0 },
	{ 0x411a,			0x0000 },
	{ 0x411c,			0x00f0 },
	{ 0x411e,			0x0010 },
	{ 0x4120,			0x0320 },
	{ 0x4122,			0x01e8 },
	{ 0x4124,			0x05a0 },
	{ 0x4126,			0x0000 },
	{ 0x4128,			0x0000 },
	{ 0x412a,			0x0001 },
	{ 0x412c,			0x0101 },
	{ 0x412e,			0x0000 },
	{ 0x4134,			0x0000 },
	{ 0x4136,			0x0000 },
	{ 0x4138,			0x0000 },
	{ 0x413a,			0x0000 },
	{ 0x413c,			0x0000 },
	{ 0x413e,			0x0000 },
	{ AC200_TVE_IF_SYNC2,		0x0100 },
	{ AC200_TVE_UNKNOWN_4130,	0x0000 },
	{ AC200_TVE_UNKNOWN_4132,	0x2004 },
};

static const struct reg_sequence ac200_tve_pal_regs[] = {
	{ AC200_TVE_MOD1,		0x0707 },
	{ AC200_TVE_DAC_CFG1,		0x4300 },
	{ AC200_TVE_YC_DELAY,		0x1400 },
	{ AC200_TVE_YC_FILTER,		0x3000 },
	{ AC200_TVE_BURST_FRQ0,		0x8acb },
	{ AC200_TVE_BURST_FRQ1,		0x2a09 },
	{ AC200_TVE_FRONT_PORCH,	0x0018 },
	{ AC200_TVE_BACK_PORCH,		0x008a },
	{ 0x4018,			0x0016 },
	{ 0x401a,			0x0000 },
	{ AC200_TVE_TOTAL_LINE,		0x0271 },	/* see note below */
	{ AC200_TVE_FIRST_ACTIVE,	0x0016 },
	{ AC200_TVE_BLACK_LEVEL,	0x00fc },
	{ AC200_TVE_BLANK_LEVEL,	0x00fc },
	{ 0x4100,			0x0000 },
	{ 0x4102,			0x0000 },
	{ 0x4104,			0x0001 },
	{ 0x4106,			0x0000 },
	{ 0x4108,			0x0005 },
	{ 0x410a,			0x0000 },
	{ 0x410c,			0x2929 },
	{ 0x410e,			0x0000 },
	{ 0x4110,			0x0000 },
	{ 0x4112,			0x0000 },
	{ 0x4114,			0x447e },
	{ 0x4116,			0x0016 },
	{ 0x4118,			0xabab },
	{ 0x411a,			0x0000 },
	{ 0x411c,			0x00fc },
	{ 0x411e,			0x0010 },
	{ 0x4120,			0x0320 },
	{ 0x4122,			0x01e8 },
	{ 0x4124,			0x05a0 },
	{ 0x4126,			0x0000 },
	{ 0x4128,			0x0000 },
	{ 0x412a,			0x0001 },
	{ 0x412c,			0x0101 },
	{ 0x412e,			0x0000 },
	{ 0x4134,			0x0000 },
	{ 0x4136,			0x0000 },
	{ 0x4138,			0x0000 },
	{ 0x413a,			0x0000 },
	{ 0x413c,			0x0000 },
	{ 0x413e,			0x0000 },
	{ AC200_TVE_UNKNOWN_43A0,	0x0001 },
	{ AC200_TVE_UNKNOWN_43A2,	0x0003 },
	{ AC200_TVE_IF_SYNC2,		0x2149 },
	{ AC200_TVE_UNKNOWN_4130,	0x0380 },
	{ AC200_TVE_UNKNOWN_4132,	0x2009 },
};

static const struct ac200_tve_timing ac200_tve_timings[] = {
	[AC200_TVE_NTSC] = {
		.name		= "NTSC",
		.hactive	= 720,
		.vactive	= 480,
		.hbp		= 57 + 62,
		.vbp		= 15 + 3,
		.mod0		= 0,
		.regs		= ac200_tve_ntsc_regs,
		.num_regs	= ARRAY_SIZE(ac200_tve_ntsc_regs),
	},
	[AC200_TVE_PAL] = {
		.name		= "PAL",
		.hactive	= 720,
		.vactive	= 576,
		.hbp		= 69 + 63,
		.vbp		= 19 + 3,
		.mod0		= AC200_TVE_MOD0_TV_MOD_PAL,
		.regs		= ac200_tve_pal_regs,
		.num_regs	= ARRAY_SIZE(ac200_tve_pal_regs),
	},
};

/*
 * Bringing the encoder clocks up. The PLL feeds the encoder with 216MHz and
 * the vendor driver enables it in two steps, first the bias and the on-chip
 * LDOs and then the PLL itself. Register 0x0040 is undocumented.
 */
static const struct reg_sequence ac200_tve_clk_regs[] = {
	{ AC200_SYS_TVE_CTL1,	0x0003 },
	{ AC200_SYS_UNKNOWN_40,	0x0702 },
	{ AC200_SYS_PLL_CTL0,	AC200_SYS_PLL_CTL0_BIAS_EN |
				AC200_SYS_PLL_CTL0_LDO1_EN |
				AC200_SYS_PLL_CTL0_LDO_EN |
				AC200_SYS_PLL_CTL0_POST_DIV9 |
				AC200_SYS_PLL_CTL0_PRE_DIV_M_1 },
	{ AC200_SYS_PLL_CTL0,	AC200_SYS_PLL_CTL0_ENABLE |
				AC200_SYS_PLL_CTL0_BIAS_EN |
				AC200_SYS_PLL_CTL0_LDO1_EN |
				AC200_SYS_PLL_CTL0_LDO_EN |
				AC200_SYS_PLL_CTL0_POST_DIV9 |
				AC200_SYS_PLL_CTL0_PRE_DIV_M_1 },
	{ AC200_SYS_TVE_CTL0,	AC200_SYS_TVE_CTL0_RESET_INVALID },
	{ AC200_SYS_TVE_CTL0,	AC200_SYS_TVE_CTL0_SYSCLK_GATING |
				AC200_SYS_TVE_CTL0_SCLK_GATING |
				AC200_SYS_TVE_CTL0_DCLK_GATING |
				AC200_SYS_TVE_CTL0_RESET_INVALID },
};

/* Cable detection, driven by the encoder itself off the 32kHz clock. */
static const struct reg_sequence ac200_tve_plug_regs[] = {
	{ AC200_TVE_DAC_CFG0,		0x12a0 },
	{ AC200_TVE_DAC_CFG1,		0x4300 },
	{ AC200_TVE_PLUG_PULSE_LEVEL,	0x0230 },
	{ AC200_TVE_PLUG_PULSE_START,	0x0064 },
	{ AC200_TVE_PLUG_PULSE_PERIOD,	0x0c80 },
	{ AC200_TVE_PLUG_DEBOUNCE,	0x0002 },
	{ AC200_TVE_PLUG_EN,		AC200_TVE_PLUG_EN_AUTO_DET },
};

struct ac200_tve {
	struct device *dev;
	struct regmap *regmap;
	struct mutex lock;	/* serialises the state below */
	enum ac200_tve_mode mode;
	bool test_pattern;
	bool enabled;
};

static int ac200_tve_calibrate(struct ac200_tve *tve)
{
	unsigned int val;
	int ret;

	/*
	 * The DAC trim lives in the chip's own eFuse. An unprogrammed part
	 * reads back zero, in which case the vendor driver falls back to a
	 * fixed value.
	 */
	ret = regmap_read(tve->regmap, AC200_EFUSE_TVE, &val);
	if (ret)
		return ret;

	val &= AC200_TVE_CALIBRATION_MASK;
	if (!val) {
		val = AC200_TVE_CALIBRATION_DEFAULT;
		dev_dbg(tve->dev, "No DAC calibration data, using default\n");
	}

	return regmap_write(tve->regmap, AC200_TVE_CALIBRATION, val);
}

static int ac200_tve_set_mode(struct ac200_tve *tve)
{
	const struct ac200_tve_timing *timing = &ac200_tve_timings[tve->mode];
	unsigned int hbp, hactive;
	int ret;

	hbp = timing->hbp * AC200_TVE_CCIR_BYTES_PER_PIXEL;
	hactive = timing->hactive * AC200_TVE_CCIR_BYTES_PER_PIXEL;

	/* CCIR656 input, which is what the SoC side will drive */
	ret = regmap_write(tve->regmap, AC200_TVE_IF_CTL, 0);
	if (ret)
		return ret;

	ret = regmap_write(tve->regmap, AC200_TVE_IF_TIM0, hbp - 1);
	if (ret)
		return ret;

	ret = regmap_write(tve->regmap, AC200_TVE_IF_TIM1, hactive - 1);
	if (ret)
		return ret;

	ret = regmap_write(tve->regmap, AC200_TVE_IF_TIM2, timing->vbp - 1);
	if (ret)
		return ret;

	ret = regmap_write(tve->regmap, AC200_TVE_IF_TIM3, timing->vactive - 1);
	if (ret)
		return ret;

	ret = regmap_write(tve->regmap, AC200_TVE_IF_TIM4, timing->vbp - 1);
	if (ret)
		return ret;

	ret = regmap_write(tve->regmap, AC200_TVE_IF_SYNC0, 0x0000);
	if (ret)
		return ret;

	ret = regmap_write(tve->regmap, AC200_TVE_IF_SYNC1, 0x0004);
	if (ret)
		return ret;

	ret = regmap_write(tve->regmap, AC200_TVE_CTL1,
			   AC200_TVE_CTL1_CLK_DISABLE);
	if (ret)
		return ret;

	/*
	 * Writing TV_MOD reloads a number of the timing registers with the
	 * defaults for the selected standard, so this has to come before the
	 * per-standard table below.
	 */
	ret = regmap_write(tve->regmap, AC200_TVE_MOD0, timing->mod0 |
			   (tve->test_pattern ?
			    AC200_TVE_MOD0_COLOR_BAR_MOD : 0));
	if (ret)
		return ret;

	ret = regmap_multi_reg_write(tve->regmap, timing->regs,
				     timing->num_regs);
	if (ret)
		return ret;

	return regmap_write(tve->regmap, AC200_TVE_CTL0,
			    AC200_TVE_CTL0_UNKNOWN);
}

static int ac200_tve_enable(struct ac200_tve *tve)
{
	int ret;

	ret = ac200_tve_set_mode(tve);
	if (ret)
		return ret;

	ret = regmap_write(tve->regmap, AC200_TVE_DAC_CFG0,
			   0x02a0 | AC200_TVE_DAC_CFG0_EN);
	if (ret)
		return ret;

	ret = regmap_write(tve->regmap, AC200_TVE_CTL0,
			   AC200_TVE_CTL0_UNKNOWN | AC200_TVE_CTL0_EN);
	if (ret)
		return ret;

	tve->enabled = true;

	return 0;
}

static int ac200_tve_disable(struct ac200_tve *tve)
{
	int ret;

	ret = regmap_write(tve->regmap, AC200_TVE_CTL0,
			   AC200_TVE_CTL0_UNKNOWN);
	if (ret)
		return ret;

	ret = regmap_write(tve->regmap, AC200_TVE_DAC_CFG0, 0x02a0);
	if (ret)
		return ret;

	tve->enabled = false;

	return 0;
}

static int ac200_tve_enabled_get(void *data, u64 *val)
{
	struct ac200_tve *tve = data;

	guard(mutex)(&tve->lock);
	*val = tve->enabled;

	return 0;
}

static int ac200_tve_enabled_set(void *data, u64 val)
{
	struct ac200_tve *tve = data;

	guard(mutex)(&tve->lock);
	if (!!val == tve->enabled)
		return 0;

	return val ? ac200_tve_enable(tve) : ac200_tve_disable(tve);
}
DEFINE_DEBUGFS_ATTRIBUTE(ac200_tve_enabled_fops, ac200_tve_enabled_get,
			 ac200_tve_enabled_set, "%llu\n");

static int ac200_tve_pattern_get(void *data, u64 *val)
{
	struct ac200_tve *tve = data;

	guard(mutex)(&tve->lock);
	*val = tve->test_pattern;

	return 0;
}

static int ac200_tve_pattern_set(void *data, u64 val)
{
	struct ac200_tve *tve = data;

	guard(mutex)(&tve->lock);
	tve->test_pattern = !!val;

	if (!tve->enabled)
		return 0;

	return regmap_update_bits(tve->regmap, AC200_TVE_MOD0,
				  AC200_TVE_MOD0_COLOR_BAR_MOD,
				  tve->test_pattern ?
				  AC200_TVE_MOD0_COLOR_BAR_MOD : 0);
}
DEFINE_DEBUGFS_ATTRIBUTE(ac200_tve_pattern_fops, ac200_tve_pattern_get,
			 ac200_tve_pattern_set, "%llu\n");

static int ac200_tve_mode_show(struct seq_file *s, void *data)
{
	struct ac200_tve *tve = s->private;

	guard(mutex)(&tve->lock);
	seq_printf(s, "%s\n", ac200_tve_timings[tve->mode].name);

	return 0;
}

static int ac200_tve_mode_open(struct inode *inode, struct file *file)
{
	return single_open(file, ac200_tve_mode_show, inode->i_private);
}

static ssize_t ac200_tve_mode_write(struct file *file,
				    const char __user *buf, size_t count,
				    loff_t *ppos)
{
	struct ac200_tve *tve = ((struct seq_file *)file->private_data)->private;
	enum ac200_tve_mode mode;
	char name[8];
	int i, ret;

	if (count >= sizeof(name))
		return -EINVAL;

	if (copy_from_user(name, buf, count))
		return -EFAULT;

	name[count] = '\0';
	i = strlen(name);
	while (i-- > 0 && (name[i] == '\n' || name[i] == ' '))
		name[i] = '\0';

	for (mode = 0; mode < ARRAY_SIZE(ac200_tve_timings); mode++)
		if (!strcasecmp(name, ac200_tve_timings[mode].name))
			break;

	if (mode == ARRAY_SIZE(ac200_tve_timings))
		return -EINVAL;

	guard(mutex)(&tve->lock);
	if (mode == tve->mode)
		return count;

	tve->mode = mode;

	if (tve->enabled) {
		ret = ac200_tve_disable(tve);
		if (ret)
			return ret;

		ret = ac200_tve_enable(tve);
		if (ret)
			return ret;
	}

	return count;
}

static const struct file_operations ac200_tve_mode_fops = {
	.owner		= THIS_MODULE,
	.open		= ac200_tve_mode_open,
	.read		= seq_read,
	.write		= ac200_tve_mode_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int ac200_tve_plug_show(struct seq_file *s, void *data)
{
	static const char * const states[] = {
		"unconnected", "connected", "reserved", "short to ground",
	};
	struct ac200_tve *tve = s->private;
	unsigned int val;
	int ret;

	ret = regmap_read(tve->regmap, AC200_TVE_PLUG_STA, &val);
	if (ret)
		return ret;

	seq_printf(s, "%s\n",
		   states[FIELD_GET(AC200_TVE_PLUG_STA_MASK, val)]);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ac200_tve_plug);

static const u16 ac200_tve_dump_regs[] = {
	AC200_SYS_PLL_CTL0, AC200_SYS_TVE_CTL0, AC200_SYS_TVE_CTL1,
	AC200_SYS_UNKNOWN_40,
	AC200_TVE_CTL0, AC200_TVE_CTL1, AC200_TVE_MOD0, AC200_TVE_MOD1,
	AC200_TVE_DAC_CFG0, AC200_TVE_DAC_CFG1, AC200_TVE_YC_DELAY,
	AC200_TVE_YC_FILTER, AC200_TVE_BURST_FRQ0, AC200_TVE_BURST_FRQ1,
	AC200_TVE_FRONT_PORCH, AC200_TVE_BACK_PORCH, AC200_TVE_TOTAL_LINE,
	AC200_TVE_FIRST_ACTIVE, AC200_TVE_BLACK_LEVEL, AC200_TVE_BLANK_LEVEL,
	AC200_TVE_PLUG_EN, AC200_TVE_PLUG_STA, AC200_TVE_CALIBRATION,
	AC200_TVE_IF_CTL, AC200_TVE_IF_TIM0, AC200_TVE_IF_TIM1,
	AC200_TVE_IF_TIM2, AC200_TVE_IF_TIM3, AC200_TVE_IF_SYNC0,
	AC200_TVE_IF_SYNC1, AC200_TVE_IF_SYNC2, AC200_TVE_IF_TIM4,
};

static int ac200_tve_regs_show(struct seq_file *s, void *data)
{
	struct ac200_tve *tve = s->private;
	unsigned int val;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(ac200_tve_dump_regs); i++) {
		ret = regmap_read(tve->regmap, ac200_tve_dump_regs[i], &val);
		if (ret)
			return ret;

		seq_printf(s, "%04x: %04x\n", ac200_tve_dump_regs[i], val);
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ac200_tve_regs);

static void ac200_tve_debugfs_remove(void *data)
{
	debugfs_remove_recursive(data);
}

static int ac200_tve_debugfs_init(struct ac200_tve *tve)
{
	struct dentry *dir;

	dir = debugfs_create_dir(dev_name(tve->dev), NULL);

	debugfs_create_file_unsafe("enabled", 0644, dir, tve,
				   &ac200_tve_enabled_fops);
	debugfs_create_file_unsafe("test_pattern", 0644, dir, tve,
				   &ac200_tve_pattern_fops);
	debugfs_create_file("mode", 0644, dir, tve, &ac200_tve_mode_fops);
	debugfs_create_file("plug", 0444, dir, tve, &ac200_tve_plug_fops);
	debugfs_create_file("regs", 0444, dir, tve, &ac200_tve_regs_fops);

	return devm_add_action_or_reset(tve->dev, ac200_tve_debugfs_remove,
					dir);
}

static void ac200_tve_shutdown(void *data)
{
	struct ac200_tve *tve = data;

	ac200_tve_disable(tve);
	regmap_write(tve->regmap, AC200_SYS_TVE_CTL0, 0);
	regmap_write(tve->regmap, AC200_SYS_PLL_CTL0, 0);
}

static int ac200_tve_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ac200_tve *tve;
	const char *mode;
	int ret;

	tve = devm_kzalloc(dev, sizeof(*tve), GFP_KERNEL);
	if (!tve)
		return -ENOMEM;

	tve->dev = dev;
	tve->regmap = dev_get_regmap(dev->parent, NULL);
	if (!tve->regmap)
		return dev_err_probe(dev, -ENODEV, "Parent has no regmap\n");

	ret = devm_mutex_init(dev, &tve->lock);
	if (ret)
		return ret;

	tve->mode = AC200_TVE_PAL;
	if (!device_property_read_string(dev, "x-powers,tv-mode", &mode)) {
		if (!strcmp(mode, "ntsc"))
			tve->mode = AC200_TVE_NTSC;
		else if (strcmp(mode, "pal"))
			return dev_err_probe(dev, -EINVAL,
					     "Unknown TV mode '%s'\n", mode);
	}

	/*
	 * Until the CCIR656 input is described in the device tree there is
	 * nothing to encode, so come up showing the internal colour bars.
	 */
	tve->test_pattern = true;

	ret = regmap_multi_reg_write(tve->regmap, ac200_tve_clk_regs,
				     ARRAY_SIZE(ac200_tve_clk_regs));
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable clocks\n");

	ret = devm_add_action_or_reset(dev, ac200_tve_shutdown, tve);
	if (ret)
		return ret;

	ret = ac200_tve_calibrate(tve);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to calibrate DAC\n");

	ret = regmap_multi_reg_write(tve->regmap, ac200_tve_plug_regs,
				     ARRAY_SIZE(ac200_tve_plug_regs));
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to set up plug detection\n");

	scoped_guard(mutex, &tve->lock) {
		ret = ac200_tve_enable(tve);
		if (ret)
			return dev_err_probe(dev, ret,
					     "Failed to enable encoder\n");
	}

	ret = ac200_tve_debugfs_init(tve);
	if (ret)
		return ret;

	dev_info(dev, "%s composite output enabled, showing colour bars\n",
		 ac200_tve_timings[tve->mode].name);

	return 0;
}

static const struct of_device_id ac200_tve_match[] = {
	{ .compatible = "x-powers,ac200-tve" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ac200_tve_match);

static struct platform_driver ac200_tve_driver = {
	.probe		= ac200_tve_probe,
	.driver		= {
		.name		= "ac200-tve",
		.of_match_table	= ac200_tve_match,
	},
};
module_platform_driver(ac200_tve_driver);

MODULE_AUTHOR("Jernej Skrabec <jernej.skrabec@gmail.com>");
MODULE_DESCRIPTION("AC200 TV encoder driver");
MODULE_LICENSE("GPL");
