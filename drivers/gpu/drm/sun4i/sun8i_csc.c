// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) Jernej Skrabec <jernej.skrabec@siol.net>
 */

#include <drm/drm_print.h>

#include <uapi/linux/media-bus-format.h>

#include "sun50i_cdc.h"
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

static const u32 identity_de3[12] = {
	0x00020000, 0x00000000, 0x00000000, 0x00000000,
	0x00000000, 0x00020000, 0x00000000, 0x00000000,
	0x00000000, 0x00000000, 0x00020000, 0x00000000,
};

static const u32 yuv_601_lim_to_709_lim_de3[12] = {
	0x00020000, 0xFFFFC4D7, 0xFFFF9589, 0xFFC00040,
	0x00000000, 0x0002098B, 0x00003AAF, 0xFE000200,
	0x00000000, 0x0000266D, 0x00020CF8, 0xFE000200,
};

static const u32 yuv_601_lim_to_2020_lim_de3[12] = {
	0x00020000, 0xFFFFBFCE, 0xFFFFC5FF, 0xFFC00040,
	0x00000000, 0x00020521, 0x00001F89, 0xFE000200,
	0x00000000, 0x00002C87, 0x00020F07, 0xFE000200,
};

static const u32 yuv_709_lim_to_601_lim_de3[12] = {
	0x00020000, 0x000032D9, 0x00006226, 0xFFC00040,
	0x00000000, 0x0001FACE, 0xFFFFC759, 0xFE000200,
	0x00000000, 0xFFFFDAE7, 0x0001F780, 0xFE000200,
};

static const u32 yuv_709_lim_to_2020_lim_de3[12] = {
	0x00020000, 0xFFFFF782, 0x00003036, 0xFFC00040,
	0x00000000, 0x0001FD99, 0xFFFFE5CA, 0xFE000200,
	0x00000000, 0x000005E4, 0x0002015A, 0xFE000200,
};

static const u32 yuv_2020_lim_to_601_lim_de3[12] = {
	0x00020000, 0x00003B03, 0x000034D2, 0xFFC00040,
	0x00000000, 0x0001FD8C, 0xFFFFE183, 0xFE000200,
	0x00000000, 0xFFFFD4F3, 0x0001F3FA, 0xFE000200,
};

static const u32 yuv_2020_lim_to_709_lim_de3[12] = {
	0x00020000, 0x00000916, 0xFFFFD061, 0xFFC00040,
	0x00000000, 0x0002021C, 0x00001A40, 0xFE000200,
	0x00000000, 0xFFFFFA19, 0x0001FE5A, 0xFE000200,
};

static const u32 yuv_full_to_lim_de3[12] = {
	0x0001B7B8, 0x00000000, 0x00000000, 0x00000040,
	0x00000000, 0x0001C1C2, 0x00000000, 0xFE000200,
	0x00000000, 0x00000000, 0x0001C1C2, 0xFE000200,
};

static const u32 yuv_601_full_to_709_lim_de3[12] = {
	0x0001B7B8, 0xFFFFCC08, 0xFFFFA27B, 0x00000040,
	0x00000000, 0x0001CA24, 0x0000338D, 0xFE000200,
	0x00000000, 0x000021C1, 0x0001CD26, 0xFE000200,
};

static const u32 yuv_601_full_to_2020_lim_de3[12] = {
	0x0001B7B8, 0xFFFFC79C, 0xFFFFCD0C, 0x00000040,
	0x00000000, 0x0001C643, 0x00001BB4, 0xFE000200,
	0x00000000, 0x0000271D, 0x0001CEF5, 0xFE000200,
};

static const u32 yuv_709_full_to_601_lim_de3[12] = {
	0x0001B7B8, 0x00002CAB, 0x00005638, 0x00000040,
	0x00000000, 0x0001BD32, 0xFFFFCE3C, 0xFE000200,
	0x00000000, 0xFFFFDF6A, 0x0001BA4A, 0xFE000200,
};

static const u32 yuv_709_full_to_2020_lim_de3[12] = {
	0x0001B7B8, 0xFFFFF88A, 0x00002A5A, 0x00000040,
	0x00000000, 0x0001BFA5, 0xFFFFE8FA, 0xFE000200,
	0x00000000, 0x0000052D, 0x0001C2F1, 0xFE000200,
};

static const u32 yuv_2020_full_to_601_lim_de3[12] = {
	0x0001B7B8, 0x000033D6, 0x00002E66, 0x00000040,
	0x00000000, 0x0001BF9A, 0xFFFFE538, 0xFE000200,
	0x00000000, 0xFFFFDA2F, 0x0001B732, 0xFE000200,
};

static const u32 yuv_2020_full_to_709_lim_de3[12] = {
	0x0001B7B8, 0x000007FB, 0xFFFFD62B, 0x00000040,
	0x00000000, 0x0001C39D, 0x0000170F, 0xFE000200,
	0x00000000, 0xFFFFFAD1, 0x0001C04F, 0xFE000200,
};

/* always convert to limited mode */
static const u32 *yuv2yuv_de3[2][3][3] = {
	[DRM_COLOR_YCBCR_LIMITED_RANGE] = {
		[DRM_COLOR_YCBCR_BT601] = {
			[DRM_COLOR_YCBCR_BT601] = identity_de3,
			[DRM_COLOR_YCBCR_BT709] = yuv_601_lim_to_709_lim_de3,
			[DRM_COLOR_YCBCR_BT2020] = yuv_601_lim_to_2020_lim_de3,
		},
		[DRM_COLOR_YCBCR_BT709] = {
			[DRM_COLOR_YCBCR_BT601] = yuv_709_lim_to_601_lim_de3,
			[DRM_COLOR_YCBCR_BT709] = identity_de3,
			[DRM_COLOR_YCBCR_BT2020] = yuv_709_lim_to_2020_lim_de3,
		},
		[DRM_COLOR_YCBCR_BT2020] = {
			[DRM_COLOR_YCBCR_BT601] = yuv_2020_lim_to_601_lim_de3,
			[DRM_COLOR_YCBCR_BT709] = yuv_2020_lim_to_709_lim_de3,
			[DRM_COLOR_YCBCR_BT2020] = identity_de3,
		},
	},
	[DRM_COLOR_YCBCR_FULL_RANGE] = {
		[DRM_COLOR_YCBCR_BT601] = {
			[DRM_COLOR_YCBCR_BT601] = yuv_full_to_lim_de3,
			[DRM_COLOR_YCBCR_BT709] = yuv_601_full_to_709_lim_de3,
			[DRM_COLOR_YCBCR_BT2020] = yuv_601_full_to_2020_lim_de3,
		},
		[DRM_COLOR_YCBCR_BT709] = {
			[DRM_COLOR_YCBCR_BT601] = yuv_709_full_to_601_lim_de3,
			[DRM_COLOR_YCBCR_BT709] = yuv_full_to_lim_de3,
			[DRM_COLOR_YCBCR_BT2020] = yuv_709_full_to_2020_lim_de3,
		},
		[DRM_COLOR_YCBCR_BT2020] = {
			[DRM_COLOR_YCBCR_BT601] = yuv_2020_full_to_601_lim_de3,
			[DRM_COLOR_YCBCR_BT709] = yuv_2020_full_to_709_lim_de3,
			[DRM_COLOR_YCBCR_BT2020] = yuv_full_to_lim_de3,
		},
	},
};

static u32 *sun8i_csc_yvu_remap(const u32 *in_table, u32 *out_table)
{
	int i;

	for (i = 0; i < 12; i++)
		if ((i & 3) == 1)
			out_table[i] = in_table[i + 1];
		else if ((i & 3) == 2)
			out_table[i] = in_table[i - 1];
		else
			out_table[i] = in_table[i];

	return out_table;
}

static void sun8i_csc_setup(struct regmap *map, u32 base,
			    enum format_type fmt_type,
			    enum drm_color_encoding encoding,
			    enum drm_color_range range)
{
	u32 base_reg, yvu_table[12];
	const u32 *table = NULL;

	base_reg = SUN8I_CSC_COEFF(base, 0);

	switch (fmt_type) {
	case FORMAT_TYPE_RGB:
		/* nothing to convert */
		break;
	case FORMAT_TYPE_YUV:
	case FORMAT_TYPE_YVU:
		table = yuv2rgb[range][encoding];
		if (fmt_type == FORMAT_TYPE_YVU)
			table = sun8i_csc_yvu_remap(table, yvu_table);
		break;
	default:
		DRM_WARN("Wrong CSC mode specified.\n");
		return;
	}

	regmap_write(map, SUN8I_CSC_CTRL(base),
		     table ? SUN8I_CSC_CTRL_EN : 0);
	if (table)
		regmap_bulk_write(map, base_reg, table, 12);
}

static bool is_rgb(u32 format)
{
	switch (format) {
	case MEDIA_BUS_FMT_RGB888_1X24:
	case MEDIA_BUS_FMT_RGB101010_1X30:
		return true;
	default:
		return false;
	}
}

static void sun8i_de3_ccsc_setup(struct sun8i_mixer *mixer, int layer,
				 enum format_type fmt_type,
				 enum drm_color_encoding encoding,
				 enum drm_color_range range)
{
	struct sunxi_engine *engine = &mixer->engine;
	const u32 *table = NULL, *in_csc;
	enum drm_color_encoding out_enc;
	u32 addr, mask, yvu_table[12];
	bool enable, is_hdr10, is_sdr;
	struct csc_state *state;

	state = &mixer->csc_states[layer];
	if (state->fmt_type == fmt_type &&
	    state->in_enc == encoding &&
	    state->in_range == range &&
	    state->out_fmt == engine->format &&
	    state->out_enc == engine->encoding &&
	    state->eotf == engine->eotf &&
	    state->is_eotf_supported == engine->is_eotf_supported)
		return;

	state->fmt_type = fmt_type;
	state->in_enc = encoding;
	state->in_range = range;
	state->out_fmt = engine->format;
	state->out_enc = engine->encoding;
	state->eotf = engine->eotf;
	state->is_eotf_supported = engine->is_eotf_supported;

	addr = SUN50I_MIXER_BLEND_CSC_COEFF(DE3_BLD_BASE, layer, 0);
	mask = SUN50I_MIXER_BLEND_CSC_CTL_EN(layer);
	is_hdr10 = engine->eotf == HDMI_EOTF_SMPTE_ST2084;
	is_sdr = engine->eotf == HDMI_EOTF_TRADITIONAL_GAMMA_SDR;
	out_enc = engine->encoding;

	switch (fmt_type) {
	case FORMAT_TYPE_RGB:
		if (!is_rgb(engine->format))
			table = rgb2yuv_de3[out_enc];
		if (!is_sdr && engine->is_eotf_supported) {
			sun50i_cdc_setup(mixer, layer,
					 identity_de3,
					 table ?: identity_de3,
					 is_hdr10 ? SDR_TO_HDR_RGB :
						    SDR_TO_WCG_RGB);
			table = NULL;
		} else {
			sun50i_cdc_disable(mixer, layer);
		}
		break;
	case FORMAT_TYPE_YUV:
	case FORMAT_TYPE_YVU:
		if (!is_sdr && !engine->is_eotf_supported) {
			in_csc = range == DRM_COLOR_YCBCR_FULL_RANGE ?
				 yuv_full_to_lim_de3 : identity_de3;
			if (fmt_type == FORMAT_TYPE_YVU)
				in_csc = sun8i_csc_yvu_remap(in_csc, yvu_table);
			range = DRM_COLOR_YCBCR_LIMITED_RANGE;
			fmt_type = FORMAT_TYPE_YUV;
			out_enc = DRM_COLOR_YCBCR_BT709;
		} else {
			sun50i_cdc_disable(mixer, layer);
		}
		table = is_rgb(engine->format) ?
			yuv2rgb_de3[range][encoding] :
			yuv2yuv_de3[range][encoding][out_enc];
		if (fmt_type == FORMAT_TYPE_YVU)
			table = sun8i_csc_yvu_remap(table, yvu_table);
		if (!is_sdr && !engine->is_eotf_supported) {
			sun50i_cdc_setup(mixer, layer,
					 in_csc, table,
					 is_hdr10 ? HDR_TO_SDR_YUV :
						    WCG_TO_SDR_YUV);
			table = NULL;
		}
		break;
	default:
		DRM_WARN("Wrong CSC mode specified.\n");
		return;
	}

	enable = table != NULL && table != identity_de3;
	regmap_update_bits(engine->regs,
			   SUN50I_MIXER_BLEND_CSC_CTL(DE3_BLD_BASE),
			   mask, enable ? mask : 0);
	if (enable)
		regmap_bulk_write(engine->regs, addr, table, 12);
}

void sun8i_csc_set_ccsc(struct sun8i_mixer *mixer, int layer,
			enum format_type fmt_type,
			enum drm_color_encoding encoding,
			enum drm_color_range range)
{
	u32 base;

	if (mixer->cfg->is_de3) {
		sun8i_de3_ccsc_setup(mixer, layer, fmt_type, encoding, range);
		return;
	}

	if (layer < mixer->cfg->vi_num) {
		base = ccsc_base[mixer->cfg->ccsc][layer];

		sun8i_csc_setup(mixer->engine.regs, base,
				fmt_type, encoding, range);
	}
}
