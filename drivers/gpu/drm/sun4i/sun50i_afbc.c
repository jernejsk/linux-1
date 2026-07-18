// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (C) Jernej Skrabec <jernej.skrabec@gmail.com> */

#include <drm/drm_blend.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_plane.h>
#include <drm/drm_print.h>
#include <uapi/drm/drm_fourcc.h>

#include "sun50i_afbc.h"
#include "sun8i_mixer.h"
#include "sun8i_rdma.h"

#define SUN50I_AFBC_DE3_OFFSET		0x300
#define SUN50I_AFBC_DE33_OFFSET		0x5000

struct sun50i_afbc_format {
	u32 format;
	u8 hw_format;
	u8 sbs0;
	u8 sbs1;
	u16 alpha;
	u16 yr;
	u16 ug;
	u16 vb;
	u16 ytr_yr;
	u16 ytr_ug;
	u16 ytr_vb;
	bool rgb;
	bool split;
};

static const struct sun50i_afbc_format sun50i_afbc_formats[] = {
	{
		.format = DRM_FORMAT_ABGR8888,
		.hw_format = SUN50I_AFBC_RGBA_8888,
		.sbs0 = 1, .sbs1 = 1,
		.alpha = 255, .yr = 255, .ug = 255, .vb = 255,
		.ytr_yr = 255, .ytr_ug = 256, .ytr_vb = 256,
		.rgb = true, .split = true,
	}, {
		.format = DRM_FORMAT_BGR888,
		.hw_format = SUN50I_AFBC_RGB_888,
		.sbs0 = 1, .sbs1 = 1,
		.yr = 255, .ug = 255, .vb = 255,
		.ytr_yr = 255, .ytr_ug = 256, .ytr_vb = 256,
		.rgb = true, .split = true,
	}, {
		.format = DRM_FORMAT_BGR565,
		.hw_format = SUN50I_AFBC_RGB_565,
		.sbs0 = 1, .sbs1 = 1,
		.yr = 31, .ug = 63, .vb = 31,
		.ytr_yr = 63, .ytr_ug = 64, .ytr_vb = 64,
		.rgb = true,
	}, {
		.format = DRM_FORMAT_ABGR4444,
		.hw_format = SUN50I_AFBC_RGBA_4444,
		.sbs0 = 1, .sbs1 = 1,
		.alpha = 15, .yr = 15, .ug = 15, .vb = 15,
		.ytr_yr = 15, .ytr_ug = 16, .ytr_vb = 16,
		.rgb = true,
	}, {
		.format = DRM_FORMAT_ABGR1555,
		.hw_format = SUN50I_AFBC_RGBA_5551,
		.sbs0 = 1, .sbs1 = 1,
		.alpha = 1, .yr = 31, .ug = 31, .vb = 31,
		.ytr_yr = 31, .ytr_ug = 32, .ytr_vb = 32,
		.rgb = true,
	}, {
		.format = DRM_FORMAT_ABGR2101010,
		.hw_format = SUN50I_AFBC_RGBA1010102,
		.sbs0 = 1, .sbs1 = 1,
		.alpha = 3, .yr = 1023, .ug = 1023, .vb = 1023,
		.ytr_yr = 1023, .ytr_ug = 1024, .ytr_vb = 1024,
		.rgb = true, .split = true,
	}, {
		.format = DRM_FORMAT_YUV420_8BIT,
		.hw_format = SUN50I_AFBC_YUV420,
		.sbs0 = 1, .sbs1 = 1,
		.yr = 255, .ug = 128, .vb = 128,
		.split = true,
	}, {
		.format = DRM_FORMAT_YUYV,
		.hw_format = SUN50I_AFBC_YUV422,
		.sbs0 = 1, .sbs1 = 2,
		.yr = 255, .ug = 128, .vb = 128,
		.split = true,
	}, {
		.format = DRM_FORMAT_YUV420_10BIT,
		.hw_format = SUN50I_AFBC_P010,
		.sbs0 = 1, .sbs1 = 2,
		.yr = 1023, .ug = 512, .vb = 512,
		.split = true,
	}, {
		.format = DRM_FORMAT_Y210,
		.hw_format = SUN50I_AFBC_P210,
		.sbs0 = 2, .sbs1 = 3,
		.yr = 1023, .ug = 512, .vb = 512,
		.split = true,
	},
};

const u64 sun50i_afbc_modifiers[] = {
	DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_16x16 |
				AFBC_FORMAT_MOD_SPARSE),
	DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_16x16 |
				AFBC_FORMAT_MOD_YTR |
				AFBC_FORMAT_MOD_SPARSE),
	DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_16x16 |
				AFBC_FORMAT_MOD_SPARSE |
				AFBC_FORMAT_MOD_SPLIT),
	DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_16x16 |
				AFBC_FORMAT_MOD_YTR |
				AFBC_FORMAT_MOD_SPARSE |
				AFBC_FORMAT_MOD_SPLIT),
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID,
};

static const struct reg_region sun50i_afbc_regions[] = {
	{ 0x00, 7 },
	{ 0x20, 2 },
	{ 0x30, 4 },
	{ 0x50, 2 },
	{ }
};

static const struct reg_region sun50i_de33_afbc_regions[] = {
	{ 0x00, 7 },
	{ 0x20, 2 },
	{ 0x30, 4 },
	{ 0x50, 3 },
	{ }
};

static const struct sun50i_afbc_format *sun50i_afbc_get_format(u32 format)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(sun50i_afbc_formats); i++)
		if (sun50i_afbc_formats[i].format == format)
			return &sun50i_afbc_formats[i];

	return NULL;
}

static bool sun50i_afbc_supported(struct sun8i_layer *layer)
{
	return layer->cfg->afbc_mask & BIT(layer->channel);
}

bool sun50i_afbc_format_mod_supported(struct sun8i_layer *layer,
				      u32 format, u64 modifier)
{
	const struct sun50i_afbc_format *fmt;
	u64 mode;

	if (modifier == DRM_FORMAT_MOD_INVALID)
		return false;

	if (modifier == DRM_FORMAT_MOD_LINEAR)
		return format != DRM_FORMAT_YUV420_8BIT &&
		       format != DRM_FORMAT_YUV420_10BIT &&
		       format != DRM_FORMAT_Y210;

	if (!sun50i_afbc_supported(layer))
		return false;

	fmt = sun50i_afbc_get_format(format);
	if (!fmt)
		return false;

	if ((modifier & AFBC_FORMAT_MOD_SPLIT) && !fmt->split)
		return false;

	mode = modifier & ~(u64)AFBC_FORMAT_MOD_SPLIT;
	if (mode == DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_16x16 |
					    AFBC_FORMAT_MOD_SPARSE))
		return true;

	return fmt->rgb &&
	       mode == DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_16x16 |
					       AFBC_FORMAT_MOD_SPARSE |
					       AFBC_FORMAT_MOD_YTR);
}

int sun50i_afbc_init(struct sun8i_layer *layer, struct sun8i_rdma *rdma,
		     void __iomem *reg_base)
{
	const struct reg_region *regions = sun50i_afbc_regions;
	u32 base, reg_offset, size = 0x58;

	if (!sun50i_afbc_supported(layer))
		return 0;

	if (layer->cfg->de_type == SUN8I_MIXER_DE33) {
		base = layer->channel * DE33_CH_SIZE + SUN50I_AFBC_DE33_OFFSET;
		reg_offset = 0x100000 + base;
		regions = sun50i_de33_afbc_regions;
		size = 0x5c;
	} else {
		base = sun8i_channel_base(layer) + SUN50I_AFBC_DE3_OFFSET;
		reg_offset = base;
	}

	layer->afbc_rdma = sun8i_rdma_add_unit(rdma, reg_base + base,
					       reg_offset, size, regions);

	return layer->afbc_rdma ? 0 : -ENOMEM;
}

void sun50i_afbc_atomic_update(struct sun8i_layer *layer,
			       struct drm_plane *plane)
{
	struct drm_plane_state *state = plane->state;
	struct drm_framebuffer *fb = state->fb;
	const struct sun50i_afbc_format *fmt;
	struct drm_afbc_framebuffer *afbc_fb;
	struct drm_gem_dma_object *gem;
	u32 val, src_w, src_h;
	u32 def_color0, def_color1;
	dma_addr_t dma_addr;
	bool ytr;

	fmt = sun50i_afbc_get_format(fb->format->format);
	if (WARN_ON(!fmt))
		return;

	ytr = fb->modifier & AFBC_FORMAT_MOD_YTR;
	sun8i_rdma_write(layer->afbc_rdma, SUN50I_FBD_FMT_SEQ,
			 SUN50I_FBD_FMT_SEQ_CANONICAL);

	src_w = drm_rect_width(&state->src) >> 16;
	src_h = drm_rect_height(&state->src) >> 16;
	val = SUN50I_FBD_SIZE_HEIGHT(src_h) | SUN50I_FBD_SIZE_WIDTH(src_w);
	sun8i_rdma_write(layer->afbc_rdma, SUN50I_FBD_SIZE, val);

	afbc_fb = container_of(fb, struct drm_afbc_framebuffer, base);
	val = SUN50I_FBD_BLK_SIZE_HEIGHT(afbc_fb->aligned_height / 16) |
	      SUN50I_FBD_BLK_SIZE_WIDTH(afbc_fb->aligned_width / 16);
	sun8i_rdma_write(layer->afbc_rdma, SUN50I_FBD_BLK_SIZE, val);

	sun8i_rdma_write(layer->afbc_rdma, SUN50I_FBD_SRC_CROP, 0);
	val = SUN50I_FBD_LAY_CROP_TOP(state->src.y1 >> 16) |
	      SUN50I_FBD_LAY_CROP_LEFT(state->src.x1 >> 16);
	sun8i_rdma_write(layer->afbc_rdma, SUN50I_FBD_LAY_CROP, val);

	val = SUN50I_FBD_FMT_SBS1(fmt->sbs1) |
	      SUN50I_FBD_FMT_SBS0(fmt->sbs0) |
	      SUN50I_FBD_FMT_IN_FMT(fmt->hw_format);
	if (ytr)
		val |= SUN50I_FBD_FMT_YUV_TRAN;
	sun8i_rdma_write(layer->afbc_rdma, SUN50I_FBD_FMT, val);

	gem = drm_fb_dma_get_gem_obj(fb, 0);
	dma_addr = gem->dma_addr + fb->offsets[0];
	DRM_DEBUG_DRIVER("Using AFBC GEM @ %pad\n", &dma_addr);
	sun8i_rdma_write(layer->afbc_rdma, SUN50I_FBD_LADDR,
			 lower_32_bits(dma_addr));
	sun8i_rdma_write(layer->afbc_rdma, SUN50I_FBD_HADDR,
			 upper_32_bits(dma_addr));

	val = SUN50I_FBD_OVL_SIZE_HEIGHT(src_h) |
	      SUN50I_FBD_OVL_SIZE_WIDTH(src_w);
	sun8i_rdma_write(layer->afbc_rdma, SUN50I_FBD_OVL_SIZE, val);
	sun8i_rdma_write(layer->afbc_rdma, SUN50I_FBD_OVL_COOR, 0);
	sun8i_rdma_write(layer->afbc_rdma, SUN50I_FBD_OVL_BG_COLOR,
			 SUN8I_MIXER_BLEND_COLOR_BLACK);

	if (ytr) {
		def_color0 = SUN50I_FBD_DEFAULT_COLOR0_ALPHA(fmt->alpha) |
			     SUN50I_FBD_DEFAULT_COLOR0_YR(fmt->ytr_yr);
		def_color1 = SUN50I_FBD_DEFAULT_COLOR1_UG(fmt->ytr_ug) |
			     SUN50I_FBD_DEFAULT_COLOR1_VB(fmt->ytr_vb);
	} else {
		def_color0 = SUN50I_FBD_DEFAULT_COLOR0_ALPHA(fmt->alpha) |
			     SUN50I_FBD_DEFAULT_COLOR0_YR(fmt->yr);
		def_color1 = SUN50I_FBD_DEFAULT_COLOR1_UG(fmt->ug) |
			     SUN50I_FBD_DEFAULT_COLOR1_VB(fmt->vb);
	}
	sun8i_rdma_write(layer->afbc_rdma, SUN50I_FBD_DEFAULT_COLOR0,
			 def_color0);
	sun8i_rdma_write(layer->afbc_rdma, SUN50I_FBD_DEFAULT_COLOR1,
			 def_color1);

	val = SUN50I_FBD_CTL_GLB_ALPHA(state->alpha >> 8) |
	      SUN50I_FBD_CTL_CLK_GATE | SUN50I_FBD_CTL_FBD_EN;
	if (!fb->format->has_alpha)
		val |= SUN50I_FBD_CTL_ALPHA_MODE_LAYER;
	else if (state->alpha == DRM_BLEND_ALPHA_OPAQUE)
		val |= SUN50I_FBD_CTL_ALPHA_MODE_PIXEL;
	else
		val |= SUN50I_FBD_CTL_ALPHA_MODE_COMBINED;
	sun8i_rdma_write(layer->afbc_rdma, SUN50I_FBD_CTL, val);
}

void sun50i_afbc_disable(struct sun8i_layer *layer)
{
	if (layer->afbc_rdma)
		sun8i_rdma_write(layer->afbc_rdma, SUN50I_FBD_CTL, 0);
}
