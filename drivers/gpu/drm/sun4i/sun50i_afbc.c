// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) Jernej Skrabec <jernej.skrabec@gmail.com>
 */

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

bool sun50i_afbc_format_mod_supported(struct sun8i_layer *layer,
				      u32 format, u64 modifier)
{
	u64 mode;

	if (modifier == DRM_FORMAT_MOD_INVALID)
		return false;

	if (modifier == DRM_FORMAT_MOD_LINEAR) {
		if (format == DRM_FORMAT_YUV420_8BIT ||
		    format == DRM_FORMAT_YUV420_10BIT ||
		    format == DRM_FORMAT_Y210)
			return false;
		return true;
	}

	if (!layer->cfg->has_afbc)
		return false;

	/* SPLIT is invalid for RGB formats at or below 16 bpp. */
	if ((modifier & AFBC_FORMAT_MOD_SPLIT) &&
	    (format == DRM_FORMAT_BGR565 ||
	     format == DRM_FORMAT_ABGR4444 ||
	     format == DRM_FORMAT_ABGR1555))
		return false;

	/* The FBD accepts both layouts without a SPLIT control bit. */
	mode = modifier & ~(u64)AFBC_FORMAT_MOD_SPLIT;

	switch (format) {
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_BGR888:
	case DRM_FORMAT_BGR565:
	case DRM_FORMAT_ABGR4444:
	case DRM_FORMAT_ABGR1555:
	case DRM_FORMAT_ABGR2101010:
		if (mode == DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_16x16 |
						    AFBC_FORMAT_MOD_SPARSE |
						    AFBC_FORMAT_MOD_YTR))
			return true;
		break;
	case DRM_FORMAT_YUYV:
	case DRM_FORMAT_Y210:
	case DRM_FORMAT_YUV420_8BIT:
	case DRM_FORMAT_YUV420_10BIT:
		break;
	default:
		return false;
	}

	return mode == DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_16x16 |
					       AFBC_FORMAT_MOD_SPARSE);
}

void sun50i_afbc_atomic_update(struct sun8i_layer *layer,
			       struct drm_plane *plane)
{
	struct drm_plane_state *state = plane->state;
	struct drm_framebuffer *fb = state->fb;
	const struct drm_format_info *format = fb->format;
	struct drm_afbc_framebuffer *afbc_fb;
	struct drm_gem_dma_object *gem;
	u32 base, val, src_w, src_h;
	u32 def_color0, def_color1;
	bool ytr;
	struct regmap *regs;
	dma_addr_t dma_addr;

	base = sun8i_channel_base(layer) + SUN50I_AFBC_CH_OFFSET;
	regs = layer->regs;
	ytr = fb->modifier & AFBC_FORMAT_MOD_YTR;
	regmap_write(regs, SUN50I_FBD_FMT_SEQ(base),
		     SUN50I_FBD_FMT_SEQ_CANONICAL);

	src_w = drm_rect_width(&state->src) >> 16;
	src_h = drm_rect_height(&state->src) >> 16;

	val = SUN50I_FBD_SIZE_HEIGHT(src_h);
	val |= SUN50I_FBD_SIZE_WIDTH(src_w);
	regmap_write(regs, SUN50I_FBD_SIZE(base), val);

	/* The block grid sets the framebuffer header stride. */
	afbc_fb = container_of(fb, struct drm_afbc_framebuffer, base);
	val = SUN50I_FBD_BLK_SIZE_HEIGHT(afbc_fb->aligned_height / 16);
	val |= SUN50I_FBD_BLK_SIZE_WIDTH(afbc_fb->aligned_width / 16);
	regmap_write(regs, SUN50I_FBD_BLK_SIZE(base), val);

	val = SUN50I_FBD_SRC_CROP_TOP(0);
	val |= SUN50I_FBD_SRC_CROP_LEFT(0);
	regmap_write(regs, SUN50I_FBD_SRC_CROP(base), val);

	val = SUN50I_FBD_LAY_CROP_TOP(state->src.y1 >> 16);
	val |= SUN50I_FBD_LAY_CROP_LEFT(state->src.x1 >> 16);
	regmap_write(regs, SUN50I_FBD_LAY_CROP(base), val);

	/*
	 * Default Y/R is the component maximum; Cb/Cr is the midpoint.
	 * YTR differences use the midpoint of their extra sign bit.
	 */
	def_color0 = 0;
	def_color1 = 0;

	val = 0;
	switch (format->format) {
	case DRM_FORMAT_YUYV:
	case DRM_FORMAT_YUV420_10BIT:
		val |= SUN50I_FBD_FMT_SBS1(2);
		val |= SUN50I_FBD_FMT_SBS0(1);
		break;
	case DRM_FORMAT_Y210:
		val |= SUN50I_FBD_FMT_SBS1(3);
		val |= SUN50I_FBD_FMT_SBS0(2);
		break;
	default:
		val |= SUN50I_FBD_FMT_SBS1(1);
		val |= SUN50I_FBD_FMT_SBS0(1);
		break;
	}
	if (ytr)
		val |= SUN50I_FBD_FMT_YUV_TRAN;

	switch (format->format) {
	case DRM_FORMAT_ABGR8888:
		val |= SUN50I_FBD_FMT_IN_FMT(SUN50I_AFBC_RGBA_8888);
		def_color0 = SUN50I_FBD_DEFAULT_COLOR0_ALPHA(255) |
			     SUN50I_FBD_DEFAULT_COLOR0_YR(255);
		def_color1 = SUN50I_FBD_DEFAULT_COLOR1_UG(256) |
			     SUN50I_FBD_DEFAULT_COLOR1_VB(256);
		if (!ytr)
			def_color1 = SUN50I_FBD_DEFAULT_COLOR1_UG(255) |
				     SUN50I_FBD_DEFAULT_COLOR1_VB(255);
		break;
	case DRM_FORMAT_BGR888:
		val |= SUN50I_FBD_FMT_IN_FMT(SUN50I_AFBC_RGB_888);
		def_color0 = SUN50I_FBD_DEFAULT_COLOR0_ALPHA(0) |
			     SUN50I_FBD_DEFAULT_COLOR0_YR(255);
		def_color1 = SUN50I_FBD_DEFAULT_COLOR1_UG(256) |
			     SUN50I_FBD_DEFAULT_COLOR1_VB(256);
		if (!ytr)
			def_color1 = SUN50I_FBD_DEFAULT_COLOR1_UG(255) |
				     SUN50I_FBD_DEFAULT_COLOR1_VB(255);
		break;
	case DRM_FORMAT_BGR565:
		val |= SUN50I_FBD_FMT_IN_FMT(SUN50I_AFBC_RGB_565);
		def_color0 = SUN50I_FBD_DEFAULT_COLOR0_ALPHA(0) |
			     SUN50I_FBD_DEFAULT_COLOR0_YR(63);
		def_color1 = SUN50I_FBD_DEFAULT_COLOR1_UG(64) |
			     SUN50I_FBD_DEFAULT_COLOR1_VB(64);
		if (!ytr) {
			def_color0 = SUN50I_FBD_DEFAULT_COLOR0_YR(31);
			def_color1 = SUN50I_FBD_DEFAULT_COLOR1_UG(63) |
				     SUN50I_FBD_DEFAULT_COLOR1_VB(31);
		}
		break;
	case DRM_FORMAT_ABGR4444:
		val |= SUN50I_FBD_FMT_IN_FMT(SUN50I_AFBC_RGBA_4444);
		def_color0 = SUN50I_FBD_DEFAULT_COLOR0_ALPHA(15) |
			     SUN50I_FBD_DEFAULT_COLOR0_YR(15);
		def_color1 = SUN50I_FBD_DEFAULT_COLOR1_UG(16) |
			     SUN50I_FBD_DEFAULT_COLOR1_VB(16);
		if (!ytr)
			def_color1 = SUN50I_FBD_DEFAULT_COLOR1_UG(15) |
				     SUN50I_FBD_DEFAULT_COLOR1_VB(15);
		break;
	case DRM_FORMAT_ABGR1555:
		val |= SUN50I_FBD_FMT_IN_FMT(SUN50I_AFBC_RGBA_5551);
		def_color0 = SUN50I_FBD_DEFAULT_COLOR0_ALPHA(1) |
			     SUN50I_FBD_DEFAULT_COLOR0_YR(31);
		def_color1 = SUN50I_FBD_DEFAULT_COLOR1_UG(32) |
			     SUN50I_FBD_DEFAULT_COLOR1_VB(32);
		if (!ytr)
			def_color1 = SUN50I_FBD_DEFAULT_COLOR1_UG(31) |
				     SUN50I_FBD_DEFAULT_COLOR1_VB(31);
		break;
	case DRM_FORMAT_ABGR2101010:
		val |= SUN50I_FBD_FMT_IN_FMT(SUN50I_AFBC_RGBA1010102);
		def_color0 = SUN50I_FBD_DEFAULT_COLOR0_ALPHA(3) |
			     SUN50I_FBD_DEFAULT_COLOR0_YR(1023);
		def_color1 = SUN50I_FBD_DEFAULT_COLOR1_UG(1024) |
			     SUN50I_FBD_DEFAULT_COLOR1_VB(1024);
		if (!ytr)
			def_color1 = SUN50I_FBD_DEFAULT_COLOR1_UG(1023) |
				     SUN50I_FBD_DEFAULT_COLOR1_VB(1023);
		break;
	case DRM_FORMAT_YUV420_8BIT:
		val |= SUN50I_FBD_FMT_IN_FMT(SUN50I_AFBC_YUV420);
		def_color0 = SUN50I_FBD_DEFAULT_COLOR0_ALPHA(0) |
			     SUN50I_FBD_DEFAULT_COLOR0_YR(255);
		def_color1 = SUN50I_FBD_DEFAULT_COLOR1_UG(128) |
			     SUN50I_FBD_DEFAULT_COLOR1_VB(128);
		break;
	case DRM_FORMAT_YUYV:
		val |= SUN50I_FBD_FMT_IN_FMT(SUN50I_AFBC_YUV422);
		def_color0 = SUN50I_FBD_DEFAULT_COLOR0_ALPHA(0) |
			     SUN50I_FBD_DEFAULT_COLOR0_YR(255);
		def_color1 = SUN50I_FBD_DEFAULT_COLOR1_UG(128) |
			     SUN50I_FBD_DEFAULT_COLOR1_VB(128);
		break;
	case DRM_FORMAT_YUV420_10BIT:
		val |= SUN50I_FBD_FMT_IN_FMT(SUN50I_AFBC_P010);
		def_color0 = SUN50I_FBD_DEFAULT_COLOR0_ALPHA(0) |
			     SUN50I_FBD_DEFAULT_COLOR0_YR(1023);
		def_color1 = SUN50I_FBD_DEFAULT_COLOR1_UG(512) |
			     SUN50I_FBD_DEFAULT_COLOR1_VB(512);
		break;
	case DRM_FORMAT_Y210:
		val |= SUN50I_FBD_FMT_IN_FMT(SUN50I_AFBC_P210);
		def_color0 = SUN50I_FBD_DEFAULT_COLOR0_ALPHA(0) |
			     SUN50I_FBD_DEFAULT_COLOR0_YR(1023);
		def_color1 = SUN50I_FBD_DEFAULT_COLOR1_UG(512) |
			     SUN50I_FBD_DEFAULT_COLOR1_VB(512);
		break;
	}
	regmap_write(regs, SUN50I_FBD_FMT(base), val);

	gem = drm_fb_dma_get_gem_obj(fb, 0);

	DRM_DEBUG_DRIVER("Using GEM @ %pad\n", &gem->dma_addr);

	dma_addr = gem->dma_addr + fb->offsets[0];

	regmap_write(regs, SUN50I_FBD_LADDR(base), lower_32_bits(dma_addr));
	regmap_write(regs, SUN50I_FBD_HADDR(base), upper_32_bits(dma_addr));

	val = SUN50I_FBD_OVL_SIZE_HEIGHT(src_h);
	val |= SUN50I_FBD_OVL_SIZE_WIDTH(src_w);
	regmap_write(regs, SUN50I_FBD_OVL_SIZE(base), val);

	val = SUN50I_FBD_OVL_COOR_Y(0);
	val |= SUN50I_FBD_OVL_COOR_X(0);
	regmap_write(regs, SUN50I_FBD_OVL_COOR(base), val);

	regmap_write(regs, SUN50I_FBD_OVL_BG_COLOR(base),
		     SUN8I_MIXER_BLEND_COLOR_BLACK);
	regmap_write(regs, SUN50I_FBD_DEFAULT_COLOR0(base), def_color0);
	regmap_write(regs, SUN50I_FBD_DEFAULT_COLOR1(base), def_color1);

	val = SUN50I_FBD_CTL_GLB_ALPHA(state->alpha >> 8);
	val |= SUN50I_FBD_CTL_CLK_GATE;
	val |= (state->alpha == DRM_BLEND_ALPHA_OPAQUE) ?
		SUN50I_FBD_CTL_ALPHA_MODE_PIXEL :
		SUN50I_FBD_CTL_ALPHA_MODE_COMBINED;
	val |= SUN50I_FBD_CTL_FBD_EN;
	regmap_write(regs, SUN50I_FBD_CTL(base), val);
}

void sun50i_afbc_disable(struct sun8i_layer *layer)
{
	u32 base = sun8i_channel_base(layer) + SUN50I_AFBC_CH_OFFSET;

	regmap_write(layer->regs, SUN50I_FBD_CTL(base), 0);
}
