// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) Jernej Skrabec <jernej.skrabec@siol.net>
 */

#include <drm/drm_print.h>

#include <uapi/linux/media-bus-format.h>

#include "sun8i_csc.h"
#include "sun8i_mixer.h"

static const u32 ccsc_base[][2] = {
	[CCSC_MIXER0_LAYOUT]	= {CCSC00_OFFSET, CCSC01_OFFSET},
	[CCSC_MIXER1_LAYOUT]	= {CCSC10_OFFSET, CCSC11_OFFSET},
	[CCSC_D1_MIXER0_LAYOUT]	= {CCSC00_OFFSET, CCSC01_D1_OFFSET},
};

/*
 * Factors are in two's complement format, 10 bits for fractinal part.
 * First tree values in each line are multiplication factor and last
 * value is constant, which is added at the end.
 */

static const u32 yuv2rgb[2][2][12] = {
	[DRM_COLOR_YCBCR_LIMITED_RANGE] = {
		[DRM_COLOR_YCBCR_BT601] = {
			0x000004A8, 0x00000000, 0x00000662, 0xFFFC8451,
			0x000004A8, 0xFFFFFE6F, 0xFFFFFCC0, 0x00021E4D,
			0x000004A8, 0x00000811, 0x00000000, 0xFFFBACA9,
		},
		[DRM_COLOR_YCBCR_BT709] = {
			0x000004A8, 0x00000000, 0x0000072B, 0xFFFC1F99,
			0x000004A8, 0xFFFFFF26, 0xFFFFFDDF, 0x00013383,
			0x000004A8, 0x00000873, 0x00000000, 0xFFFB7BEF,
		}
	},
	[DRM_COLOR_YCBCR_FULL_RANGE] = {
		[DRM_COLOR_YCBCR_BT601] = {
			0x00000400, 0x00000000, 0x0000059B, 0xFFFD322E,
			0x00000400, 0xFFFFFEA0, 0xFFFFFD25, 0x00021DD5,
			0x00000400, 0x00000716, 0x00000000, 0xFFFC74BD,
		},
		[DRM_COLOR_YCBCR_BT709] = {
			0x00000400, 0x00000000, 0x0000064C, 0xFFFCD9B4,
			0x00000400, 0xFFFFFF41, 0xFFFFFE21, 0x00014F96,
			0x00000400, 0x0000076C, 0x00000000, 0xFFFC49EF,
		}
	},
};

/*
 * DE3 has a bit different CSC units. Factors are in two's complement format.
 * First three factors in a row are multiplication factors which have 17 bits
 * for fractional part. Fourth value in a row is comprised of two factors.
 * Upper 16 bits represents difference, which is subtracted from the input
 * value before multiplication and lower 16 bits represents constant, which
 * is addes at the end.
 *
 * x' = c00 * (x + d0) + c01 * (y + d1) + c02 * (z + d2) + const0
 * y' = c10 * (x + d0) + c11 * (y + d1) + c12 * (z + d2) + const1
 * z' = c20 * (x + d0) + c21 * (y + d1) + c22 * (z + d2) + const2
 *
 * Please note that above formula is true only for Blender CSC. Other DE3 CSC
 * units takes only positive value for difference. From what can be deducted
 * from BSP driver code, those units probably automatically assume that
 * difference has to be subtracted.
 *
 * Layout of factors in table:
 * c00 c01 c02 [d0 const0]
 * c10 c11 c12 [d1 const1]
 * c20 c21 c22 [d2 const2]
 */

static const u32 yuv2rgb_de3[2][3][12] = {
	[DRM_COLOR_YCBCR_LIMITED_RANGE] = {
		[DRM_COLOR_YCBCR_BT601] = {
			0x0002542A, 0x00000000, 0x0003312A, 0xFFC00000,
			0x0002542A, 0xFFFF376B, 0xFFFE5FC3, 0xFE000000,
			0x0002542A, 0x000408D2, 0x00000000, 0xFE000000,
		},
		[DRM_COLOR_YCBCR_BT709] = {
			0x0002542A, 0x00000000, 0x000395E2, 0xFFC00000,
			0x0002542A, 0xFFFF92D2, 0xFFFEEF27, 0xFE000000,
			0x0002542A, 0x0004398C, 0x00000000, 0xFE000000,
		},
		[DRM_COLOR_YCBCR_BT2020] = {
			0x0002542A, 0x00000000, 0x00035B7B, 0xFFC00000,
			0x0002542A, 0xFFFFA017, 0xFFFEB2FC, 0xFE000000,
			0x0002542A, 0x00044896, 0x00000000, 0xFE000000,
		}
	},
	[DRM_COLOR_YCBCR_FULL_RANGE] = {
		[DRM_COLOR_YCBCR_BT601] = {
			0x00020000, 0x00000000, 0x0002CDD2, 0x00000000,
			0x00020000, 0xFFFF4FCE, 0xFFFE925D, 0xFE000000,
			0x00020000, 0x00038B43, 0x00000000, 0xFE000000,
		},
		[DRM_COLOR_YCBCR_BT709] = {
			0x00020000, 0x00000000, 0x0003264C, 0x00000000,
			0x00020000, 0xFFFFA018, 0xFFFF1053, 0xFE000000,
			0x00020000, 0x0003B611, 0x00000000, 0xFE000000,
		},
		[DRM_COLOR_YCBCR_BT2020] = {
			0x00020000, 0x00000000, 0x0002F2FE, 0x00000000,
			0x00020000, 0xFFFFABC0, 0xFFFEDB78, 0xFE000000,
			0x00020000, 0x0003C346, 0x00000000, 0xFE000000,
		}
	},
};

/* always convert to limited mode */
static const u32 rgb2yuv_de3[3][12] = {
	[DRM_COLOR_YCBCR_BT601] = {
		0x0000837A, 0x0001021D, 0x00003221, 0x00000040,
		0xFFFFB41C, 0xFFFF6B03, 0x0000E0E1, 0x00000200,
		0x0000E0E1, 0xFFFF43B1, 0xFFFFDB6E, 0x00000200,
	},
	[DRM_COLOR_YCBCR_BT709] = {
		0x00005D7C, 0x00013A7C, 0x00001FBF, 0x00000040,
		0xFFFFCC78, 0xFFFF52A7, 0x0000E0E1, 0x00000200,
		0x0000E0E1, 0xFFFF33BE, 0xFFFFEB61, 0x00000200,
	},
	[DRM_COLOR_YCBCR_BT2020] = {
		0x00007384, 0x00012A21, 0x00001A13, 0x00000040,
		0xFFFFC133, 0xFFFF5DEC, 0x0000E0E1, 0x00000200,
		0x0000E0E1, 0xFFFF3135, 0xFFFFEDEA, 0x00000200,
	},
};

/* always convert to limited mode */
static const u32 yuv2yuv_de3[2][3][3][12] = {
	[DRM_COLOR_YCBCR_LIMITED_RANGE] = {
		[DRM_COLOR_YCBCR_BT601] = {
			[DRM_COLOR_YCBCR_BT601] = {
				0x00020000, 0x00000000, 0x00000000, 0x00000000,
				0x00000000, 0x00020000, 0x00000000, 0x00000000,
				0x00000000, 0x00000000, 0x00020000, 0x00000000,
			},
			[DRM_COLOR_YCBCR_BT709] = {
				0x00020000, 0xFFFFC4D7, 0xFFFF9589, 0xFFC00040,
				0x00000000, 0x0002098B, 0x00003AAF, 0xFE000200,
				0x00000000, 0x0000266D, 0x00020CF8, 0xFE000200,
			},
			[DRM_COLOR_YCBCR_BT2020] = {
				0x00020000, 0xFFFFBFCE, 0xFFFFC5FF, 0xFFC00040,
				0x00000000, 0x00020521, 0x00001F89, 0xFE000200,
				0x00000000, 0x00002C87, 0x00020F07, 0xFE000200,
			},
		},
		[DRM_COLOR_YCBCR_BT709] = {
			[DRM_COLOR_YCBCR_BT601] = {
				0x00020000, 0x000032D9, 0x00006226, 0xFFC00040,
				0x00000000, 0x0001FACE, 0xFFFFC759, 0xFE000200,
				0x00000000, 0xFFFFDAE7, 0x0001F780, 0xFE000200,
			},
			[DRM_COLOR_YCBCR_BT709] = {
				0x00020000, 0x00000000, 0x00000000, 0x00000000,
				0x00000000, 0x00020000, 0x00000000, 0x00000000,
				0x00000000, 0x00000000, 0x00020000, 0x00000000,
			},
			[DRM_COLOR_YCBCR_BT2020] = {
				0x00020000, 0xFFFFF782, 0x00003036, 0xFFC00040,
				0x00000000, 0x0001FD99, 0xFFFFE5CA, 0xFE000200,
				0x00000000, 0x000005E4, 0x0002015A, 0xFE000200,
			},
		},
		[DRM_COLOR_YCBCR_BT2020] = {
			[DRM_COLOR_YCBCR_BT601] = {
				0x00020000, 0x00003B03, 0x000034D2, 0xFFC00040,
				0x00000000, 0x0001FD8C, 0xFFFFE183, 0xFE000200,
				0x00000000, 0xFFFFD4F3, 0x0001F3FA, 0xFE000200,
			},
			[DRM_COLOR_YCBCR_BT709] = {
				0x00020000, 0x00000916, 0xFFFFD061, 0xFFC00040,
				0x00000000, 0x0002021C, 0x00001A40, 0xFE000200,
				0x00000000, 0xFFFFFA19, 0x0001FE5A, 0xFE000200,
			},
			[DRM_COLOR_YCBCR_BT2020] = {
				0x00020000, 0x00000000, 0x00000000, 0x00000000,
				0x00000000, 0x00020000, 0x00000000, 0x00000000,
				0x00000000, 0x00000000, 0x00020000, 0x00000000,
			},
		},
	},
	[DRM_COLOR_YCBCR_FULL_RANGE] = {
		[DRM_COLOR_YCBCR_BT601] = {
			[DRM_COLOR_YCBCR_BT601] = {
				0x0001B7B8, 0x00000000, 0x00000000, 0x00000040,
				0x00000000, 0x0001C1C2, 0x00000000, 0xFE000200,
				0x00000000, 0x00000000, 0x0001C1C2, 0xFE000200,
			},
			[DRM_COLOR_YCBCR_BT709] = {
				0x0001B7B8, 0xFFFFCC08, 0xFFFFA27B, 0x00000040,
				0x00000000, 0x0001CA24, 0x0000338D, 0xFE000200,
				0x00000000, 0x000021C1, 0x0001CD26, 0xFE000200,
			},
			[DRM_COLOR_YCBCR_BT2020] = {
				0x0001B7B8, 0xFFFFC79C, 0xFFFFCD0C, 0x00000040,
				0x00000000, 0x0001C643, 0x00001BB4, 0xFE000200,
				0x00000000, 0x0000271D, 0x0001CEF5, 0xFE000200,
			},
		},
		[DRM_COLOR_YCBCR_BT709] = {
			[DRM_COLOR_YCBCR_BT601] = {
				0x0001B7B8, 0x00002CAB, 0x00005638, 0x00000040,
				0x00000000, 0x0001BD32, 0xFFFFCE3C, 0xFE000200,
				0x00000000, 0xFFFFDF6A, 0x0001BA4A, 0xFE000200,
			},
			[DRM_COLOR_YCBCR_BT709] = {
				0x0001B7B8, 0x00000000, 0x00000000, 0x00000040,
				0x00000000, 0x0001C1C2, 0x00000000, 0xFE000200,
				0x00000000, 0x00000000, 0x0001C1C2, 0xFE000200,
			},
			[DRM_COLOR_YCBCR_BT2020] = {
				0x0001B7B8, 0xFFFFF88A, 0x00002A5A, 0x00000040,
				0x00000000, 0x0001BFA5, 0xFFFFE8FA, 0xFE000200,
				0x00000000, 0x0000052D, 0x0001C2F1, 0xFE000200,
			},
		},
		[DRM_COLOR_YCBCR_BT2020] = {
			[DRM_COLOR_YCBCR_BT601] = {
				0x0001B7B8, 0x000033D6, 0x00002E66, 0x00000040,
				0x00000000, 0x0001BF9A, 0xFFFFE538, 0xFE000200,
				0x00000000, 0xFFFFDA2F, 0x0001B732, 0xFE000200,
			},
			[DRM_COLOR_YCBCR_BT709] = {
				0x0001B7B8, 0x000007FB, 0xFFFFD62B, 0x00000040,
				0x00000000, 0x0001C39D, 0x0000170F, 0xFE000200,
				0x00000000, 0xFFFFFAD1, 0x0001C04F, 0xFE000200,
			},
			[DRM_COLOR_YCBCR_BT2020] = {
				0x0001B7B8, 0x00000000, 0x00000000, 0x00000040,
				0x00000000, 0x0001C1C2, 0x00000000, 0xFE000200,
				0x00000000, 0x00000000, 0x0001C1C2, 0xFE000200,
			},
		},
	},
};

static u32 sun8i_csc_base(struct sun8i_mixer *mixer, int layer)
{
	if (mixer->cfg->de_type == sun8i_mixer_de33)
		return sun8i_channel_base(mixer, layer) - 0x800;
	else
		return ccsc_base[mixer->cfg->ccsc][layer];
}

static void sun8i_csc_setup(struct regmap *map, u32 base,
			    enum format_type fmt_type,
			    enum drm_color_encoding encoding,
			    enum drm_color_range range)
{
	u32 base_reg, val = 0;
	const u32 *table;
	int i;

	table = yuv2rgb[range][encoding];

	switch (fmt_type) {
	case FORMAT_TYPE_RGB:
		val = 0;
		break;
	case FORMAT_TYPE_YUV:
		val = SUN8I_CSC_CTRL_EN;
		base_reg = SUN8I_CSC_COEFF(base, 0);
		regmap_bulk_write(map, base_reg, table, 12);
		break;
	case FORMAT_TYPE_YVU:
		val = SUN8I_CSC_CTRL_EN;
		for (i = 0; i < 12; i++) {
			if ((i & 3) == 1)
				base_reg = SUN8I_CSC_COEFF(base, i + 1);
			else if ((i & 3) == 2)
				base_reg = SUN8I_CSC_COEFF(base, i - 1);
			else
				base_reg = SUN8I_CSC_COEFF(base, i);
			regmap_write(map, base_reg, table[i]);
		}
		break;
	default:
		val = 0;
		DRM_WARN("Wrong CSC mode specified.\n");
		return;
	}

	regmap_write(map, SUN8I_CSC_CTRL(base), val);
}

static const u32 *sun8i_csc_get_de3_yuv_table(enum drm_color_encoding in_enc,
					      enum drm_color_range in_range,
					      u32 out_format,
					      enum drm_color_encoding out_enc)
{
	if (out_format == MEDIA_BUS_FMT_RGB888_1X24)
		return yuv2rgb_de3[in_range][in_enc];

	/* check for identity transformation */
	if (in_range == DRM_COLOR_YCBCR_LIMITED_RANGE && out_enc == in_enc)
		return NULL;

	return yuv2yuv_de3[in_range][in_enc][out_enc];
}

static void sun8i_de3_ccsc_setup(struct sunxi_engine *engine, int layer,
				 enum format_type fmt_type,
				 enum drm_color_encoding encoding,
				 enum drm_color_range range)
{
	u32 addr, val = 0, mask;
	struct regmap *map;
	const u32 *table;
	int i;

	mask = SUN50I_MIXER_BLEND_CSC_CTL_EN(layer);
	table = yuv2rgb_de3[range][encoding];
	map = engine->regs;

	switch (fmt_type) {
	case FORMAT_TYPE_RGB:
		if (engine->format == MEDIA_BUS_FMT_RGB888_1X24)
			break;
		val = mask;
		addr = SUN50I_MIXER_BLEND_CSC_COEFF(DE3_BLD_BASE, layer, 0);
		regmap_bulk_write(map, addr, rgb2yuv_de3[engine->encoding], 12);
		break;
	case FORMAT_TYPE_YUV:
		table = sun8i_csc_get_de3_yuv_table(encoding, range,
						    engine->format,
						    engine->encoding);
		if (!table)
			break;
		val = mask;
		addr = SUN50I_MIXER_BLEND_CSC_COEFF(DE3_BLD_BASE, layer, 0);
		regmap_bulk_write(map, addr, table, 12);
		break;
	case FORMAT_TYPE_YVU:
		table = sun8i_csc_get_de3_yuv_table(encoding, range,
						    engine->format,
						    engine->encoding);
		if (!table)
			table = yuv2yuv_de3[range][encoding][encoding];
		val = mask;
		for (i = 0; i < 12; i++) {
			if ((i & 3) == 1)
				addr = SUN50I_MIXER_BLEND_CSC_COEFF(DE3_BLD_BASE,
								    layer,
								    i + 1);
			else if ((i & 3) == 2)
				addr = SUN50I_MIXER_BLEND_CSC_COEFF(DE3_BLD_BASE,
								    layer,
								    i - 1);
			else
				addr = SUN50I_MIXER_BLEND_CSC_COEFF(DE3_BLD_BASE,
								    layer, i);
			regmap_write(map, addr, table[i]);
		}
		break;
	default:
		val = 0;
		DRM_WARN("Wrong CSC mode specified.\n");
		return;
	}

	regmap_update_bits(map, SUN50I_MIXER_BLEND_CSC_CTL(DE3_BLD_BASE),
			   mask, val);
}

/* extract constant from high word and invert sign if necessary */
static u32 sun8i_de33_ccsc_get_constant(u32 value)
{
	value >>= 16;

	if (value & BIT(15))
		return 0x400 - (value & 0x3ff);

	return value;
}

static void sun8i_de33_convert_table(const u32 *src, u32 *dst)
{
	dst[0] = sun8i_de33_ccsc_get_constant(src[3]);
	dst[1] = sun8i_de33_ccsc_get_constant(src[7]);
	dst[2] = sun8i_de33_ccsc_get_constant(src[11]);
	memcpy(&dst[3], src, sizeof(u32) * 12);
	dst[6] &= 0xffff;
	dst[10] &= 0xffff;
	dst[14] &= 0xffff;
}

static void sun8i_de33_ccsc_setup(struct sun8i_mixer *mixer, int layer,
				  enum format_type fmt_type,
				  enum drm_color_encoding encoding,
				  enum drm_color_range range)
{
	u32 addr, val = 0, base, csc[15];
	struct sunxi_engine *engine;
	struct regmap *map;
	const u32 *table;
	int i;

	table = yuv2rgb_de3[range][encoding];
	base = sun8i_csc_base(mixer, layer);
	engine = &mixer->engine;
	map = engine->regs;

	switch (fmt_type) {
	case FORMAT_TYPE_RGB:
		if (engine->format == MEDIA_BUS_FMT_RGB888_1X24)
			break;
		val = SUN8I_CSC_CTRL_EN;
		sun8i_de33_convert_table(rgb2yuv_de3[engine->encoding], csc);
		regmap_bulk_write(map, SUN50I_CSC_COEFF(base, 0), csc, 15);
		break;
	case FORMAT_TYPE_YUV:
		table = sun8i_csc_get_de3_yuv_table(encoding, range,
						    engine->format,
						    engine->encoding);
		if (!table)
			break;
		val = SUN8I_CSC_CTRL_EN;
		sun8i_de33_convert_table(table, csc);
		regmap_bulk_write(map, SUN50I_CSC_COEFF(base, 0), csc, 15);
		break;
	case FORMAT_TYPE_YVU:
		table = sun8i_csc_get_de3_yuv_table(encoding, range,
						    engine->format,
						    engine->encoding);
		if (!table)
			table = yuv2yuv_de3[range][encoding][encoding];
		val = SUN8I_CSC_CTRL_EN;
		sun8i_de33_convert_table(table, csc);
		for (i = 0; i < 15; i++) {
			addr = SUN50I_CSC_COEFF(base, i);
			if (i > 3) {
				if (((i - 3) & 3) == 1)
					addr = SUN50I_CSC_COEFF(base, i + 1);
				else if (((i - 3) & 3) == 2)
					addr = SUN50I_CSC_COEFF(base, i - 1);
			}
			regmap_write(map, addr, csc[i]);
		}
		break;
	default:
		val = 0;
		DRM_WARN("Wrong CSC mode specified.\n");
		return;
	}

	regmap_write(map, SUN8I_CSC_CTRL(base), val);
}

void sun8i_csc_set_ccsc(struct sun8i_mixer *mixer, int layer,
			enum format_type fmt_type,
			enum drm_color_encoding encoding,
			enum drm_color_range range)
{
	u32 base;

	if (mixer->cfg->de_type == sun8i_mixer_de3) {
		sun8i_de3_ccsc_setup(&mixer->engine, layer,
				     fmt_type, encoding, range);
		return;
	} else if (mixer->cfg->de_type == sun8i_mixer_de33) {
		sun8i_de33_ccsc_setup(mixer, layer, fmt_type,
				      encoding, range);
		return;
	}

	if (layer < mixer->cfg->vi_num) {
		base = ccsc_base[mixer->cfg->ccsc][layer];

		sun8i_csc_setup(mixer->engine.regs, base,
				fmt_type, encoding, range);
	}
}
