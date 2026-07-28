// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for the TV encoder in the X-Powers AC200
 *
 * Copyright (c) 2026 Jernej Skrabec <jernej.skrabec@gmail.com>
 *
 * The encoder takes CCIR656 video from the SoC and turns it into composite
 * video, so it shows up as a DRM bridge behind the TCON driving that
 * interface.
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
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>

#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_modes.h>
#include <drm/drm_of.h>
#include <drm/drm_probe_helper.h>

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
#define AC200_TVE_PLUG_IRQ_EN		0x4032
#define AC200_TVE_PLUG_IRQ_EN_AUTO_DET	BIT(0)
#define AC200_TVE_PLUG_IRQ_STA		0x4034
#define AC200_TVE_PLUG_IRQ_STA_AUTO_DET	BIT(0)
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
	struct drm_bridge bridge;
	struct drm_bridge *next_bridge;
	enum ac200_tve_mode mode;
	bool enabled;
	bool detect_valid;
	int irq;
};

static inline struct ac200_tve *bridge_to_ac200_tve(struct drm_bridge *bridge)
{
	return container_of(bridge, struct ac200_tve, bridge);
}

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
	ret = regmap_write(tve->regmap, AC200_TVE_MOD0, timing->mod0);
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

	tve->detect_valid |= tve->enabled;
	tve->enabled = false;

	/* DAC off and switched to the detection regime, then start detecting. */
	ret = regmap_write(tve->regmap, AC200_TVE_DAC_CFG0,
			   AC200_TVE_DAC_CFG0_BIAS |
			   AC200_TVE_DAC_CFG0_DETECT_BIAS);
	if (ret)
		return ret;

	/*
	 * The status is a latch rather than a live sample, so it keeps
	 * reporting whatever the detector last saw until it is cleared.
	 */
	ret = regmap_write(tve->regmap, AC200_TVE_PLUG_STA, 0);
	if (ret)
		return ret;

	return regmap_write(tve->regmap, AC200_TVE_PLUG_EN,
			    AC200_TVE_PLUG_EN_AUTO_DET);
}

static int ac200_tve_bridge_get_modes(struct drm_bridge *bridge,
				      struct drm_connector *connector)
{
	return drm_connector_helper_tv_get_modes(connector);
}

static int ac200_tve_bridge_attach(struct drm_bridge *bridge,
				   struct drm_encoder *encoder,
				   enum drm_bridge_attach_flags flags)
{
	struct ac200_tve *tve = bridge_to_ac200_tve(bridge);

	if (!(flags & DRM_BRIDGE_ATTACH_NO_CONNECTOR))
		return -EINVAL;

	return drm_bridge_attach(encoder, tve->next_bridge, bridge, flags);
}

static enum drm_mode_status
ac200_tve_bridge_mode_valid(struct drm_bridge *bridge,
			    const struct drm_display_info *info,
			    const struct drm_display_mode *mode)
{
	/* Only the two interlaced standards the encoder knows about. */
	if (!(mode->flags & DRM_MODE_FLAG_INTERLACE))
		return MODE_NO_INTERLACE;

	if (mode->hdisplay != 720)
		return MODE_BAD_HVALUE;

	if (mode->vdisplay != 480 && mode->vdisplay != 576)
		return MODE_BAD_VVALUE;

	return MODE_OK;
}

static int
ac200_tve_bridge_atomic_check(struct drm_bridge *bridge,
			      struct drm_bridge_state *bridge_state,
			      struct drm_crtc_state *crtc_state,
			      struct drm_connector_state *conn_state)
{
	struct drm_display_mode *adj = &crtc_state->adjusted_mode;

	/*
	 * The encoder wants one whole progressive frame per output field over
	 * CCIR656 and picks the lines it needs for each field itself, so the
	 * pipeline runs at the field rate with the full frame height: the
	 * pixel clock doubles and the interlace flag goes away.
	 */
	drm_mode_copy(adj, &crtc_state->mode);
	adj->flags &= ~DRM_MODE_FLAG_INTERLACE;
	adj->clock *= 2;
	drm_mode_set_crtcinfo(adj, 0);

	return 0;
}

static enum drm_connector_status ac200_tve_plug_status(struct ac200_tve *tve)
{
	unsigned int val;

	if (regmap_read(tve->regmap, AC200_TVE_PLUG_STA, &val))
		return connector_status_unknown;

	/*
	 * Only the connected state counts as such, the way the vendor driver
	 * reports it: a reading of short to ground means the DAC is not
	 * looking at a terminated cable either.
	 */
	if (FIELD_GET(AC200_TVE_PLUG_STA_MASK, val) ==
	    AC200_TVE_PLUG_STA_CONNECTED)
		return connector_status_connected;

	return connector_status_disconnected;
}

static enum drm_connector_status
ac200_tve_bridge_detect(struct drm_bridge *bridge,
			struct drm_connector *connector)
{
	struct ac200_tve *tve = bridge_to_ac200_tve(bridge);

	/*
	 * The detector shares the DAC with the video output and is only set up
	 * for an idle DAC, so while the encoder drives a picture the answer
	 * from before it was turned on is the best there is.
	 */
	if (tve->enabled)
		return connector->status;

	/*
	 * Before the DAC has driven a picture once the detector reports a
	 * short no matter what is attached, so there is nothing to report
	 * yet. Saying so rather than guessing keeps the output usable.
	 */
	if (!tve->detect_valid)
		return connector_status_unknown;

	return ac200_tve_plug_status(tve);
}

static void ac200_tve_bridge_atomic_enable(struct drm_bridge *bridge,
					   struct drm_atomic_commit *state)
{
	struct ac200_tve *tve = bridge_to_ac200_tve(bridge);
	struct drm_connector_state *conn_state;
	struct drm_connector *connector;

	connector = drm_atomic_get_new_connector_for_encoder(state,
							     bridge->encoder);
	if (WARN_ON(!connector))
		return;

	conn_state = drm_atomic_get_new_connector_state(state, connector);
	tve->mode = conn_state->tv.mode == DRM_MODE_TV_MODE_NTSC ?
		    AC200_TVE_NTSC : AC200_TVE_PAL;

	if (ac200_tve_enable(tve))
		dev_err(tve->dev, "Failed to enable encoder\n");
}

static void ac200_tve_bridge_atomic_disable(struct drm_bridge *bridge,
					    struct drm_atomic_commit *state)
{
	struct ac200_tve *tve = bridge_to_ac200_tve(bridge);

	if (ac200_tve_disable(tve))
		dev_err(tve->dev, "Failed to disable encoder\n");
}

static irqreturn_t ac200_tve_irq(int irq, void *data)
{
	struct ac200_tve *tve = data;

	if (regmap_write(tve->regmap, AC200_TVE_PLUG_IRQ_STA,
			 AC200_TVE_PLUG_IRQ_STA_AUTO_DET))
		return IRQ_NONE;

	drm_bridge_hpd_notify(&tve->bridge, ac200_tve_plug_status(tve));

	return IRQ_HANDLED;
}

static void ac200_tve_bridge_hpd_enable(struct drm_bridge *bridge)
{
	struct ac200_tve *tve = bridge_to_ac200_tve(bridge);

	regmap_write(tve->regmap, AC200_TVE_PLUG_IRQ_EN,
		     AC200_TVE_PLUG_IRQ_EN_AUTO_DET);
}

static void ac200_tve_bridge_hpd_disable(struct drm_bridge *bridge)
{
	struct ac200_tve *tve = bridge_to_ac200_tve(bridge);

	regmap_write(tve->regmap, AC200_TVE_PLUG_IRQ_EN, 0);
}

static const struct drm_bridge_funcs ac200_tve_bridge_funcs = {
	.attach			= ac200_tve_bridge_attach,
	.detect			= ac200_tve_bridge_detect,
	.hpd_enable		= ac200_tve_bridge_hpd_enable,
	.hpd_disable		= ac200_tve_bridge_hpd_disable,
	.get_modes		= ac200_tve_bridge_get_modes,
	.mode_valid		= ac200_tve_bridge_mode_valid,
	.atomic_check		= ac200_tve_bridge_atomic_check,
	.atomic_enable		= ac200_tve_bridge_atomic_enable,
	.atomic_disable		= ac200_tve_bridge_atomic_disable,
	.atomic_duplicate_state	= drm_atomic_helper_bridge_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_bridge_destroy_state,
	.atomic_reset		= drm_atomic_helper_bridge_reset,
};

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

	tve = devm_drm_bridge_alloc(dev, struct ac200_tve, bridge,
				    &ac200_tve_bridge_funcs);
	if (IS_ERR(tve))
		return PTR_ERR(tve);

	tve->dev = dev;
	tve->regmap = dev_get_regmap(dev->parent, NULL);
	if (!tve->regmap)
		return dev_err_probe(dev, -ENODEV, "Parent has no regmap\n");

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

	/*
	 * The cable detector only reads sensibly once the encoder as a whole
	 * has been programmed, so set a mode up before arming it. This does
	 * not drive the DAC, that only happens once the display pipeline hands
	 * over a mode of its own.
	 */
	ret = ac200_tve_set_mode(tve);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to set up the encoder\n");

	/*
	 * Put the DAC into the detection regime, which is also where it waits
	 * for the display pipeline to hand over a mode.
	 */
	ac200_tve_disable(tve);

	tve->next_bridge = devm_drm_of_get_bridge(dev, dev->of_node, 1, 0);
	if (IS_ERR(tve->next_bridge))
		return dev_err_probe(dev, PTR_ERR(tve->next_bridge),
				     "Failed to find the output connector\n");

	tve->bridge.of_node = dev->of_node;
	tve->irq = platform_get_irq(pdev, 0);
	if (tve->irq < 0)
		return tve->irq;

	ret = devm_request_threaded_irq(dev, tve->irq, NULL, ac200_tve_irq,
					IRQF_ONESHOT, dev_name(dev), tve);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to request interrupt\n");

	tve->bridge.ops = DRM_BRIDGE_OP_DETECT | DRM_BRIDGE_OP_HPD |
			  DRM_BRIDGE_OP_MODES;
	tve->bridge.supported_tv_modes = BIT(DRM_MODE_TV_MODE_NTSC) |
					 BIT(DRM_MODE_TV_MODE_PAL);
	tve->bridge.type = DRM_MODE_CONNECTOR_Composite;
	tve->bridge.interlace_allowed = true;

	return devm_drm_bridge_add(dev, &tve->bridge);
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
