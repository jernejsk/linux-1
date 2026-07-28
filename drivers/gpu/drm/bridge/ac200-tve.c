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
 *
 * This is the same encoder Allwinner integrated into their earlier SoCs, with
 * every 32 bit register split into two 16 bit ones at the same offset. That
 * makes drivers/gpu/drm/sun4i/sun4i_tv.c a useful reference: the burst
 * frequencies, porches, levels, first active line, colour gains and burst
 * levels below all agree with the values it programs. The names for the
 * otherwise undocumented 0x4100 block come from there, from the H3/H5 TV
 * encoder register layout in U-Boot, and from the R40 user manual, which
 * documents the same encoder register by register.
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
/* the single CVBS DAC is fed from the second output of the encoder */
#define AC200_TVE_CTL0_DAC_MAP		GENMASK(11, 8)
#define AC200_TVE_CTL0_DAC_MAP_CVBS	(3 << 8)
#define AC200_TVE_CTL0_EN		BIT(0)
#define AC200_TVE_CTL1			0x4002
/* clears the encoder clock gate, so the clock runs */
#define AC200_TVE_CTL1_CLK_GATE_DIS	BIT(15)
#define AC200_TVE_MOD0			0x4004
#define AC200_TVE_MOD0_COLOR_BAR_TYPE	BIT(9)
#define AC200_TVE_MOD0_COLOR_BAR_MOD	BIT(8)
#define AC200_TVE_MOD0_TV_MOD_PAL	BIT(0)
#define AC200_TVE_MOD1			0x4006
#define AC200_TVE_MOD1_DAC_TEST_MOD	BIT(12)
#define AC200_TVE_MOD1_CORE_CONTROL_54M	BIT(10)
#define AC200_TVE_MOD1_CORE_DATA_54M	BIT(9)
#define AC200_TVE_MOD1_DAC_CONTROL_54M	BIT(8)
#define AC200_TVE_MOD1_C_SEQ_CR_FIRST	BIT(4)
#define AC200_TVE_MOD1_C_MODE_422	BIT(3)
#define AC200_TVE_MOD1_YUV_RGB_EN	BIT(2)
#define AC200_TVE_MOD1_YC_EN		BIT(1)
#define AC200_TVE_MOD1_COMP_EN		BIT(0)
#define AC200_TVE_DAC_CFG0		0x4008
/*
 * The vendor code sets bit 12 while the plug detector runs and clears it to
 * drive video. On the R40, whose DAC configuration register has a comparable
 * layout, the bits around there select the DAC bias current, which fits the
 * two detection regimes the datasheet describes. The remaining set bits are
 * undocumented bias settings.
 */
#define AC200_TVE_DAC_CFG0_BIAS		0x02a0
#define AC200_TVE_DAC_CFG0_DETECT_BIAS	BIT(12)
#define AC200_TVE_DAC_CFG0_EN		BIT(0)
#define AC200_TVE_DAC_CFG1		0x400a
/*
 * sun4i_tv.c additionally selects the 37.5 ohm internal DAC termination here
 * (bits 1:0), which the vendor code for this chip leaves alone.
 */
#define AC200_TVE_DAC_CFG1_UNKNOWN	BIT(14)
#define AC200_TVE_DAC_CFG1_BIT25	BIT(9)
#define AC200_TVE_DAC_CFG1_CLK_INVERT	BIT(8)
#define AC200_TVE_YC_DELAY		0x400c
#define AC200_TVE_YC_DELAY_Y(x)		((x) << 12)
#define AC200_TVE_YC_DELAY_C(x)		((x) << 8)
#define AC200_TVE_YC_FILTER		0x400e
/* 0: 27MHz, 1: 54MHz, 2: 108MHz, 3: 216MHz */
#define AC200_TVE_YC_FILTER_UPSAMPLE(x)	((x) << 12)
#define AC200_TVE_BURST_FRQ0		0x4010
#define AC200_TVE_BURST_FRQ1		0x4012
#define AC200_TVE_FRONT_PORCH		0x4014
#define AC200_TVE_BACK_PORCH		0x4016
#define AC200_TVE_HD_VSYNC		0x4018
#define AC200_TVE_HD_VSYNC_FRONT_PORCH(x)	(x)
/* the broadcast pulse count sits in the upper half */
#define AC200_TVE_HD_VSYNC_BROAD_PLUS(x)	(x)
/* these two are the halves of one line count register, see the note below */
#define AC200_TVE_TOTAL_LINE		0x401c
#define AC200_TVE_FIRST_ACTIVE		0x401e
#define AC200_TVE_BLACK_LEVEL		0x4020
#define AC200_TVE_BLANK_LEVEL		0x4022
#define AC200_TVE_PLUG_EN		0x4030
#define AC200_TVE_PLUG_EN_AUTO_DET	BIT(0)
#define AC200_TVE_PLUG_STA		0x4038
#define AC200_TVE_PLUG_STA_MASK		GENMASK(1, 0)
#define AC200_TVE_PLUG_STA_UNCONNECTED	0
#define AC200_TVE_PLUG_STA_CONNECTED	1
#define AC200_TVE_PLUG_DEBOUNCE		0x4040
#define AC200_TVE_PLUG_DEBOUNCE_TIMES	GENMASK(3, 0)
#define AC200_TVE_PLUG_PULSE_LEVEL	0x40f4
#define AC200_TVE_PLUG_PULSE_START	0x40f8
#define AC200_TVE_PLUG_PULSE_PERIOD	0x40fa

/*
 * Second encoder block. The datasheet does not describe it, the names come
 * from sun4i_tv.c and from the H3/H5 register layout in U-Boot. Registers at
 * odd multiples of two are the upper halves of the 32 bit originals.
 */
#define AC200_TVE_COLOR_BURST		0x4100
/* how often the colour burst phase is reset */
#define AC200_TVE_COLOR_BURST_8_FIELD	0
#define AC200_TVE_COLOR_BURST_4_FIELD	1
#define AC200_TVE_VSYNC_NUM		0x4104
/* number of equalisation pulses, 0: five, 1: six */
#define AC200_TVE_VSYNC_NUM_SIX		BIT(0)
#define AC200_TVE_NOTCH_FREQ		0x4108
#define AC200_TVE_CBR_LEVEL		0x410c
#define AC200_TVE_CBR_LEVEL_CR_BURST(x)	((x) << 8)
#define AC200_TVE_CBR_LEVEL_CB_BURST(x)	(x)
#define AC200_TVE_BURST_PHASE		0x4110
#define AC200_TVE_BURST_PHASE_CHROMA(x)	(x)
/* the tint adjustment sits in the upper half */
#define AC200_TVE_BURST_PHASE_TINT(x)	(x)
#define AC200_TVE_BURST_WIDTH		0x4114
#define AC200_TVE_BURST_WIDTH_WIDTH(x)	((x) << 8)
#define AC200_TVE_BURST_WIDTH_HSYNC(x)	(x)
/* the breezeway sits in the upper half of the same 32 bit register */
#define AC200_TVE_BURST_BREEZEWAY(x)	(x)
#define AC200_TVE_CB_CR_GAIN		0x4118
#define AC200_TVE_CB_CR_GAIN_CR(x)	((x) << 8)
#define AC200_TVE_CB_CR_GAIN_CB(x)	(x)
#define AC200_TVE_SYNC_VBI_LEVEL	0x411c
#define AC200_TVE_SYNC_VBI_VBLANK(x)	(x)
/* the sync level sits in the upper half of the same 32 bit register */
#define AC200_TVE_SYNC_VBI_SYNC(x)	(x)
#define AC200_TVE_WHITE_LEVEL		0x4120
#define AC200_TVE_WHITE_LEVEL_WHITE(x)	(x)
/* the HD sync breezeway level sits in the upper half */
#define AC200_TVE_WHITE_LEVEL_BREEZE(x)	(x)
#define AC200_TVE_ACTIVE_NUM		0x4124
#define AC200_TVE_CHROMA_BW_GAIN	0x4128
/* 0: 100%, 1: 25%, 2: 50%, 3: 75% */
#define AC200_TVE_CHROMA_COMP_GAIN(x)	(x)
/* chroma filter bandwidth, in the upper half: 0: 0.6MHz, 1: 1.2MHz, ... */
#define AC200_TVE_CHROMA_BW(x)		(x)
#define AC200_TVE_NOTCH_WIDTH		0x412c
#define AC200_TVE_NOTCH_WIDTH_WIDE	BIT(8)
#define AC200_TVE_NOTCH_COMP_YUV_EN	BIT(0)
#define AC200_TVE_RESYNC		0x4130
#define AC200_TVE_RESYNC_PIXEL_NUM(x)	(x)
/* the line count sits in the upper half, together with an unknown bit */
#define AC200_TVE_RESYNC_LINE_NUM(x)	(x)
#define AC200_TVE_RESYNC_UNKNOWN	BIT(13)
#define AC200_TVE_SLAVE_PARA		0x4134
#define AC200_TVE_SLAVE_PARA_THRESH	BIT(8)
#define AC200_TVE_SLAVE_PARA_MODE	BIT(0)
#define AC200_TVE_CFG0			0x4138
#define AC200_TVE_CFG0_INVERT_TOP	BIT(8)
#define AC200_TVE_CFG0_UV_ORDER_CR	BIT(0)
#define AC200_TVE_CFG1			0x413c
/*
 * This one resets to 1, so writing zero clamps the luma input to 64..940
 * instead of taking it as full range.
 */
#define AC200_TVE_CFG1_BYPASS_YCLAMP	BIT(0)
/* RGB sync embedding and set-up sit in the upper half */
#define AC200_TVE_CFG1_RGB_SYNC(x)	((x) << 8)
#define AC200_TVE_CFG1_RGB_SETUP	BIT(0)
/*
 * DAC trim. This is the upper half of the register the H3 and H5 program with
 * 0x02000c00 and 0x02850000 respectively, so 0x28f is in the expected range.
 */
#define AC200_TVE_CALIBRATION		0x4306
#define AC200_TVE_CALIBRATION_MASK	GENMASK(9, 0)
#define AC200_TVE_CALIBRATION_DEFAULT	0x28f
#define AC200_TVE_NOISE_REDUCTION	0x43a0
#define AC200_TVE_NOISE_REDUCTION_EN	BIT(0)
/* the threshold sits in the upper half */
#define AC200_TVE_NOISE_REDUCTION_T(x)	(x)

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
 * Encoder settings per TV standard, replayed as the vendor driver writes them.
 *
 * The core runs at 27MHz, which the datasheet's own burst frequency constants
 * confirm: 0x21f07c1f / 2^32 * 27MHz is the 3.579545MHz NTSC subcarrier and
 * 0x2a098acb / 2^32 * 27MHz the 4.433619MHz PAL one. Everything counted in
 * clocks below is therefore in units of 37ns, and the horizontal timings add up
 * to exactly one line of each standard:
 *
 *	NTSC	32 + 126 + 118 + 1440 = 1716 = 858 * 2	63.556us
 *	PAL	24 + 126 + 138 + 1440 = 1728 = 864 * 2	64.000us
 *
 * with the 126 clock sync landing on the specified 4.7us. The front porches
 * come out shorter and the back porches longer than nominal, but the burst is
 * placed relative to sync by the breezeway and burst width, so what matters is
 * that the totals are exact.
 *
 * The levels are 10 bit with the sync tip at zero. For NTSC blank 240, black
 * 282 and white 800 give a 300mV sync, a 700mV luma range and the specified
 * 7.5 IRE setup; PAL puts black at the blank level, so no setup at all, at the
 * cost of a sync that is a few percent high.
 *
 * The burst width is 68 clocks for both standards, which is exactly the nine
 * subcarrier cycles NTSC wants but a little over eleven for PAL, where ten are
 * specified. sun4i_tv.c programs the same value.
 *
 * The burst levels are the two colour difference components: NTSC keys its
 * burst off -U alone, so Cr is zero, while PAL swings it +-45 degrees and needs
 * both. For the colour gains sun4i_tv.c agrees on 160 for NTSC but uses 224
 * rather than 171 for PAL, so if PAL saturation looks wrong that is the first
 * value to try.
 *
 * TVE_TOTAL_LINE is written for both standards, as the vendor driver does, but
 * it always reads back 0x20d whatever is written to it, even when written last.
 * The encoder appears to take the line count from TV_MOD instead.
 */
static const struct reg_sequence ac200_tve_ntsc_regs[] = {
	{ AC200_TVE_MOD1,                 AC200_TVE_MOD1_COMP_EN |
					  AC200_TVE_MOD1_YC_EN |
					  AC200_TVE_MOD1_YUV_RGB_EN |
					  AC200_TVE_MOD1_DAC_CONTROL_54M |
					  AC200_TVE_MOD1_CORE_DATA_54M |
					  AC200_TVE_MOD1_CORE_CONTROL_54M },
	{ AC200_TVE_DAC_CFG1,             AC200_TVE_DAC_CFG1_UNKNOWN |
					  AC200_TVE_DAC_CFG1_BIT25 |
					  AC200_TVE_DAC_CFG1_CLK_INVERT },
		/* luma and chroma path delays, in clocks */
	{ AC200_TVE_YC_DELAY,             AC200_TVE_YC_DELAY_Y(1) |
					  AC200_TVE_YC_DELAY_C(4) },
	{ AC200_TVE_YC_FILTER,            AC200_TVE_YC_FILTER_UPSAMPLE(3) },
	{ AC200_TVE_BURST_FRQ0,           0x7c1f },
	{ AC200_TVE_BURST_FRQ1,           0x21f0 },
	{ AC200_TVE_FRONT_PORCH,          32 },
	{ AC200_TVE_BACK_PORCH,           118 },
		/* the HD mode registers are left at their reset values */
	{ AC200_TVE_HD_VSYNC,             AC200_TVE_HD_VSYNC_FRONT_PORCH(0x16) },
	{ AC200_TVE_HD_VSYNC + 2,         AC200_TVE_HD_VSYNC_BROAD_PLUS(0) },
	{ AC200_TVE_TOTAL_LINE,           525 },	/* see note below */
	{ AC200_TVE_FIRST_ACTIVE,         22 },	/* both standards, see above */
	{ AC200_TVE_BLACK_LEVEL,          282 },
	{ AC200_TVE_BLANK_LEVEL,          240 },
	{ AC200_TVE_COLOR_BURST,          AC200_TVE_COLOR_BURST_4_FIELD },
	{ AC200_TVE_COLOR_BURST + 2,      0x0000 },
	{ AC200_TVE_VSYNC_NUM,            0 },	/* five equalisation pulses */
	{ AC200_TVE_VSYNC_NUM + 2,        0x0000 },
	{ AC200_TVE_NOTCH_FREQ,           0x0002 },
	{ AC200_TVE_NOTCH_FREQ + 2,       0x0000 },
	{ AC200_TVE_CBR_LEVEL,            AC200_TVE_CBR_LEVEL_CR_BURST(0) |
					  AC200_TVE_CBR_LEVEL_CB_BURST(79) },
	{ AC200_TVE_CBR_LEVEL + 2,        0x0000 },
	{ AC200_TVE_BURST_PHASE,          AC200_TVE_BURST_PHASE_CHROMA(0) },
	{ AC200_TVE_BURST_PHASE + 2,      AC200_TVE_BURST_PHASE_TINT(0) },
	{ AC200_TVE_BURST_WIDTH,          AC200_TVE_BURST_WIDTH_WIDTH(68) |
					  AC200_TVE_BURST_WIDTH_HSYNC(126) },
	{ AC200_TVE_BURST_WIDTH + 2,      AC200_TVE_BURST_BREEZEWAY(22) },
	{ AC200_TVE_CB_CR_GAIN,           AC200_TVE_CB_CR_GAIN_CR(160) |
					  AC200_TVE_CB_CR_GAIN_CB(160) },
	{ AC200_TVE_CB_CR_GAIN + 2,       0x0000 },
	{ AC200_TVE_SYNC_VBI_LEVEL,       AC200_TVE_SYNC_VBI_VBLANK(240) },
	{ AC200_TVE_SYNC_VBI_LEVEL + 2,   AC200_TVE_SYNC_VBI_SYNC(0x10) },
	{ AC200_TVE_WHITE_LEVEL,          AC200_TVE_WHITE_LEVEL_WHITE(800) },
	{ AC200_TVE_WHITE_LEVEL + 2,      AC200_TVE_WHITE_LEVEL_BREEZE(488) },
	{ AC200_TVE_ACTIVE_NUM,           1440 },
	{ AC200_TVE_ACTIVE_NUM + 2,       0x0000 },
		/* full chroma gain, 1.2MHz chroma filter */
	{ AC200_TVE_CHROMA_BW_GAIN,       AC200_TVE_CHROMA_COMP_GAIN(0) },
	{ AC200_TVE_CHROMA_BW_GAIN + 2,   AC200_TVE_CHROMA_BW(1) },
	{ AC200_TVE_NOTCH_WIDTH,          AC200_TVE_NOTCH_WIDTH_WIDE |
					  AC200_TVE_NOTCH_COMP_YUV_EN },
	{ AC200_TVE_NOTCH_WIDTH + 2,      0x0000 },
		/* sync master, chroma Cb first, and the luma input clamped */
	{ AC200_TVE_SLAVE_PARA,           0 },
	{ AC200_TVE_SLAVE_PARA + 2,       0 },
	{ AC200_TVE_CFG0,                 0 },
	{ AC200_TVE_CFG0 + 2,             0 },
	{ AC200_TVE_CFG1,                 0 },
	{ AC200_TVE_CFG1 + 2,             0 },
	{ AC200_TVE_IF_SYNC2,             0x0100 },
	{ AC200_TVE_RESYNC,               AC200_TVE_RESYNC_PIXEL_NUM(0) },
	{ AC200_TVE_RESYNC + 2,           AC200_TVE_RESYNC_LINE_NUM(4) |
					  AC200_TVE_RESYNC_UNKNOWN },
};

static const struct reg_sequence ac200_tve_pal_regs[] = {
	{ AC200_TVE_MOD1,                 AC200_TVE_MOD1_COMP_EN |
					  AC200_TVE_MOD1_YC_EN |
					  AC200_TVE_MOD1_YUV_RGB_EN |
					  AC200_TVE_MOD1_DAC_CONTROL_54M |
					  AC200_TVE_MOD1_CORE_DATA_54M |
					  AC200_TVE_MOD1_CORE_CONTROL_54M },
	{ AC200_TVE_DAC_CFG1,             AC200_TVE_DAC_CFG1_UNKNOWN |
					  AC200_TVE_DAC_CFG1_BIT25 |
					  AC200_TVE_DAC_CFG1_CLK_INVERT },
		/* luma and chroma path delays, in clocks */
	{ AC200_TVE_YC_DELAY,             AC200_TVE_YC_DELAY_Y(1) |
					  AC200_TVE_YC_DELAY_C(4) },
	{ AC200_TVE_YC_FILTER,            AC200_TVE_YC_FILTER_UPSAMPLE(3) },
	{ AC200_TVE_BURST_FRQ0,           0x8acb },
	{ AC200_TVE_BURST_FRQ1,           0x2a09 },
	{ AC200_TVE_FRONT_PORCH,          24 },
	{ AC200_TVE_BACK_PORCH,           138 },
		/* the HD mode registers are left at their reset values */
	{ AC200_TVE_HD_VSYNC,             AC200_TVE_HD_VSYNC_FRONT_PORCH(0x16) },
	{ AC200_TVE_HD_VSYNC + 2,         AC200_TVE_HD_VSYNC_BROAD_PLUS(0) },
	{ AC200_TVE_TOTAL_LINE,           625 },	/* see note below */
	{ AC200_TVE_FIRST_ACTIVE,         22 },	/* both standards, see above */
	{ AC200_TVE_BLACK_LEVEL,          252 },
	{ AC200_TVE_BLANK_LEVEL,          252 },
	{ AC200_TVE_COLOR_BURST,          AC200_TVE_COLOR_BURST_8_FIELD },
	{ AC200_TVE_COLOR_BURST + 2,      0x0000 },
	{ AC200_TVE_VSYNC_NUM,            AC200_TVE_VSYNC_NUM_SIX },
	{ AC200_TVE_VSYNC_NUM + 2,        0x0000 },
	{ AC200_TVE_NOTCH_FREQ,           0x0005 },
	{ AC200_TVE_NOTCH_FREQ + 2,       0x0000 },
	{ AC200_TVE_CBR_LEVEL,            AC200_TVE_CBR_LEVEL_CR_BURST(41) |
					  AC200_TVE_CBR_LEVEL_CB_BURST(41) },
	{ AC200_TVE_CBR_LEVEL + 2,        0x0000 },
	{ AC200_TVE_BURST_PHASE,          AC200_TVE_BURST_PHASE_CHROMA(0) },
	{ AC200_TVE_BURST_PHASE + 2,      AC200_TVE_BURST_PHASE_TINT(0) },
	{ AC200_TVE_BURST_WIDTH,          AC200_TVE_BURST_WIDTH_WIDTH(68) |
					  AC200_TVE_BURST_WIDTH_HSYNC(126) },
	{ AC200_TVE_BURST_WIDTH + 2,      AC200_TVE_BURST_BREEZEWAY(22) },
	{ AC200_TVE_CB_CR_GAIN,           AC200_TVE_CB_CR_GAIN_CR(171) |
					  AC200_TVE_CB_CR_GAIN_CB(171) },
	{ AC200_TVE_CB_CR_GAIN + 2,       0x0000 },
	{ AC200_TVE_SYNC_VBI_LEVEL,       AC200_TVE_SYNC_VBI_VBLANK(252) },
	{ AC200_TVE_SYNC_VBI_LEVEL + 2,   AC200_TVE_SYNC_VBI_SYNC(0x10) },
	{ AC200_TVE_WHITE_LEVEL,          AC200_TVE_WHITE_LEVEL_WHITE(800) },
	{ AC200_TVE_WHITE_LEVEL + 2,      AC200_TVE_WHITE_LEVEL_BREEZE(488) },
	{ AC200_TVE_ACTIVE_NUM,           1440 },
	{ AC200_TVE_ACTIVE_NUM + 2,       0x0000 },
		/* full chroma gain, 1.2MHz chroma filter */
	{ AC200_TVE_CHROMA_BW_GAIN,       AC200_TVE_CHROMA_COMP_GAIN(0) },
	{ AC200_TVE_CHROMA_BW_GAIN + 2,   AC200_TVE_CHROMA_BW(1) },
	{ AC200_TVE_NOTCH_WIDTH,          AC200_TVE_NOTCH_WIDTH_WIDE |
					  AC200_TVE_NOTCH_COMP_YUV_EN },
	{ AC200_TVE_NOTCH_WIDTH + 2,      0x0000 },
		/* sync master, chroma Cb first, and the luma input clamped */
	{ AC200_TVE_SLAVE_PARA,           0 },
	{ AC200_TVE_SLAVE_PARA + 2,       0 },
	{ AC200_TVE_CFG0,                 0 },
	{ AC200_TVE_CFG0 + 2,             0 },
	{ AC200_TVE_CFG1,                 0 },
	{ AC200_TVE_CFG1 + 2,             0 },
	{ AC200_TVE_NOISE_REDUCTION,      AC200_TVE_NOISE_REDUCTION_EN },
	{ AC200_TVE_NOISE_REDUCTION + 2,  AC200_TVE_NOISE_REDUCTION_T(3) },
	{ AC200_TVE_IF_SYNC2,             0x2149 },
	{ AC200_TVE_RESYNC,               AC200_TVE_RESYNC_PIXEL_NUM(896) },
	{ AC200_TVE_RESYNC + 2,           AC200_TVE_RESYNC_LINE_NUM(9) |
					  AC200_TVE_RESYNC_UNKNOWN },
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
	{ AC200_SYS_TVE_CTL1,             0x0003 },
	{ AC200_SYS_UNKNOWN_40,           0x0702 },
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

/*
 * Cable detection parameters. The detector pulses the DAC off the 32kHz clock
 * and compares the voltage it reads back against a threshold, which tells a
 * 75 ohm open line apart from a 37.5 ohm terminated one.
 *
 * Per the datasheet the threshold differs depending on whether the DAC is
 * driving video at the time (roughly 1V when idle against 0.4V when active),
 * and only the idle case has known good parameters, so detection is armed
 * while the encoder is off and disarmed before it starts driving. The debounce
 * count is the one U-Boot uses for the same detector in the H3 and H5, which
 * is far more tolerant than the 2 the vendor code for this chip picks.
 *
 * The pulse level is a DAC code, and the start and period are in 32kHz ticks,
 * so the detector samples 100 ticks into a pulse and repeats every 3200, that
 * is roughly every 98ms.
 */
static const struct reg_sequence ac200_tve_plug_regs[] = {
	{ AC200_TVE_PLUG_PULSE_LEVEL,     0x0230 },
	{ AC200_TVE_PLUG_PULSE_START,     0x0064 },
	{ AC200_TVE_PLUG_PULSE_PERIOD,    0x0c80 },
	{ AC200_TVE_PLUG_DEBOUNCE,        9 },
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

	/*
	 * These describe the CCIR656 input, which nothing drives yet, so none
	 * of it has been exercised. Note that the vendor driver programs the
	 * full frame height and the same vertical back porch for both fields
	 * while leaving the encoder interlaced, which is unlikely to be right
	 * once real video arrives.
	 */
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
			   AC200_TVE_CTL1_CLK_GATE_DIS);
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
			    AC200_TVE_CTL0_DAC_MAP_CVBS);
}

static int ac200_tve_enable(struct ac200_tve *tve)
{
	int ret;

	/* The detector is only calibrated for an idle DAC, so stop it first. */
	ret = regmap_write(tve->regmap, AC200_TVE_PLUG_EN, 0);
	if (ret)
		return ret;

	ret = ac200_tve_set_mode(tve);
	if (ret)
		return ret;

	ret = regmap_write(tve->regmap, AC200_TVE_DAC_CFG0,
			   AC200_TVE_DAC_CFG0_BIAS | AC200_TVE_DAC_CFG0_EN);
	if (ret)
		return ret;

	ret = regmap_write(tve->regmap, AC200_TVE_CTL0,
			   AC200_TVE_CTL0_DAC_MAP_CVBS | AC200_TVE_CTL0_EN);
	if (ret)
		return ret;

	tve->enabled = true;

	return 0;
}

static int ac200_tve_disable(struct ac200_tve *tve)
{
	int ret;

	ret = regmap_write(tve->regmap, AC200_TVE_CTL0,
			   AC200_TVE_CTL0_DAC_MAP_CVBS);
	if (ret)
		return ret;

	tve->enabled = false;

	/* DAC off and switched to the detection regime, then start detecting. */
	ret = regmap_write(tve->regmap, AC200_TVE_DAC_CFG0,
			   AC200_TVE_DAC_CFG0_BIAS |
			   AC200_TVE_DAC_CFG0_DETECT_BIAS);
	if (ret)
		return ret;

	return regmap_write(tve->regmap, AC200_TVE_PLUG_EN,
			    AC200_TVE_PLUG_EN_AUTO_DET);
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

	guard(mutex)(&tve->lock);

	/*
	 * The detector shares the DAC with the video output and is only set up
	 * for the idle case, so anything it reports while the encoder is
	 * running is meaningless. Disable the encoder to get an answer.
	 */
	if (tve->enabled) {
		seq_puts(s, "unknown (encoder enabled)\n");
		return 0;
	}

	ret = regmap_read(tve->regmap, AC200_TVE_PLUG_STA, &val);
	if (ret)
		return ret;

	seq_printf(s, "%s\n",
		   states[FIELD_GET(AC200_TVE_PLUG_STA_MASK, val)]);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ac200_tve_plug);

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

	/*
	 * Only the default of the connector's TV mode property, which is what
	 * userspace overrides to pick a standard.
	 */
	tve->mode = AC200_TVE_PAL;

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
				     "Failed to set up cable detection\n");

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
