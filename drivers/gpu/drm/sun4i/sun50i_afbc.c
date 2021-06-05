// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) Jernej Skrabec <jernej.skrabec@gmail.com>
 */

#include <drm/drm_blend.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_plane.h>
#include <uapi/drm/drm_fourcc.h>

#include "sun50i_afbc.h"
#include "sun8i_mixer.h"

static u32 sun50i_afbc_get_base(struct sun8i_mixer *mixer, unsigned int channel)
{
	u32 base = sun8i_channel_base(mixer, channel);

	if (mixer->cfg->de_type == sun8i_mixer_de3)
		return base + SUN50I_AFBC_CH_OFFSET;

	return base + 0x4000;
}

bool sun50i_afbc_format_mod_supported(struct sun8i_mixer *mixer,
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

	if (mixer->cfg->de_type == sun8i_mixer_de2)
		return false;

	mode = AFBC_FORMAT_MOD_BLOCK_SIZE_16x16 |
	       AFBC_FORMAT_MOD_SPARSE |
	       AFBC_FORMAT_MOD_SPLIT;

	switch (format) {
	case DRM_FORMAT_RGBA8888:
	case DRM_FORMAT_RGB888:
	case DRM_FORMAT_RGB565:
	case DRM_FORMAT_RGBA4444:
	case DRM_FORMAT_RGBA5551:
	case DRM_FORMAT_RGBA1010102:
		mode |= AFBC_FORMAT_MOD_YTR;
		break;
	case DRM_FORMAT_YUYV:
	case DRM_FORMAT_Y210:
	case DRM_FORMAT_YUV420_8BIT:
	case DRM_FORMAT_YUV420_10BIT:
		break;
	default:
		return false;
	}

	return modifier == DRM_FORMAT_MOD_ARM_AFBC(mode);
}

void sun50i_afbc_atomic_update(struct sun8i_mixer *mixer, unsigned int channel,
			       struct drm_plane *plane)
{
	struct drm_plane_state *state = plane->state;
	struct drm_framebuffer *fb = state->fb;
	const struct drm_format_info *format = fb->format;
	struct drm_gem_dma_object *gem;
	u32 base, val, src_w, src_h;
	u32 def_color0, def_color1;
	struct regmap *regs;
	dma_addr_t dma_addr;

	base = sun50i_afbc_get_base(mixer, channel);
	regs = mixer->engine.regs;

	src_w = drm_rect_width(&state->src) >> 16;
	src_h = drm_rect_height(&state->src) >> 16;

	val = SUN50I_FBD_SIZE_HEIGHT(src_h);
	val |= SUN50I_FBD_SIZE_WIDTH(src_w);
	regmap_write(regs, SUN50I_FBD_SIZE(base), val);

	val = SUN50I_FBD_BLK_SIZE_HEIGHT(DIV_ROUND_UP(src_h, 16));
	val = SUN50I_FBD_BLK_SIZE_WIDTH(DIV_ROUND_UP(src_w, 16));
	regmap_write(regs, SUN50I_FBD_BLK_SIZE(base), val);

	val = SUN50I_FBD_SRC_CROP_TOP(0);
	val |= SUN50I_FBD_SRC_CROP_LEFT(0);
	regmap_write(regs, SUN50I_FBD_SRC_CROP(base), val);

	val = SUN50I_FBD_LAY_CROP_TOP(state->src.y1 >> 16);
	val |= SUN50I_FBD_LAY_CROP_LEFT(state->src.x1 >> 16);
	regmap_write(regs, SUN50I_FBD_LAY_CROP(base), val);

	/*
	 * Default color is always set to white, in colorspace and bitness
	 * that coresponds to used format. If it is actually used or not
	 * depends on AFBC buffer. At least in Cedrus it can be turned on
	 * or off.
	 * NOTE: G and B channels are off by 1 (up). It's unclear if this
	 * is because HW need such value or it is due to good enough code
	 * in vendor driver and HW clips the value anyway.
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
	switch (format->format) {
	case DRM_FORMAT_RGBA8888:
		val |= SUN50I_FBD_FMT_YUV_TRAN;
		val |= SUN50I_FBD_FMT_IN_FMT(SUN50I_AFBC_RGBA_8888);
		def_color0 = SUN50I_FBD_DEFAULT_COLOR0_ALPHA(255) |
			     SUN50I_FBD_DEFAULT_COLOR0_YR(255);
		def_color1 = SUN50I_FBD_DEFAULT_COLOR1_UG(256) |
			     SUN50I_FBD_DEFAULT_COLOR1_VB(256);
		break;
	case DRM_FORMAT_RGB888:
		val |= SUN50I_FBD_FMT_YUV_TRAN;
		val |= SUN50I_FBD_FMT_IN_FMT(SUN50I_AFBC_RGB_888);
		def_color0 = SUN50I_FBD_DEFAULT_COLOR0_ALPHA(0) |
			     SUN50I_FBD_DEFAULT_COLOR0_YR(255);
		def_color1 = SUN50I_FBD_DEFAULT_COLOR1_UG(256) |
			     SUN50I_FBD_DEFAULT_COLOR1_VB(256);
		break;
	case DRM_FORMAT_RGB565:
		val |= SUN50I_FBD_FMT_YUV_TRAN;
		val |= SUN50I_FBD_FMT_IN_FMT(SUN50I_AFBC_RGB_565);
		def_color0 = SUN50I_FBD_DEFAULT_COLOR0_ALPHA(0) |
			     SUN50I_FBD_DEFAULT_COLOR0_YR(31);
		def_color1 = SUN50I_FBD_DEFAULT_COLOR1_UG(64) |
			     SUN50I_FBD_DEFAULT_COLOR1_VB(32);
		break;
	case DRM_FORMAT_RGBA4444:
		val |= SUN50I_FBD_FMT_YUV_TRAN;
		val |= SUN50I_FBD_FMT_IN_FMT(SUN50I_AFBC_RGBA_4444);
		def_color0 = SUN50I_FBD_DEFAULT_COLOR0_ALPHA(15) |
			     SUN50I_FBD_DEFAULT_COLOR0_YR(15);
		def_color1 = SUN50I_FBD_DEFAULT_COLOR1_UG(16) |
			     SUN50I_FBD_DEFAULT_COLOR1_VB(16);
		break;
	case DRM_FORMAT_RGBA5551:
		val |= SUN50I_FBD_FMT_YUV_TRAN;
		val |= SUN50I_FBD_FMT_IN_FMT(SUN50I_AFBC_RGBA_5551);
		def_color0 = SUN50I_FBD_DEFAULT_COLOR0_ALPHA(1) |
			     SUN50I_FBD_DEFAULT_COLOR0_YR(31);
		def_color1 = SUN50I_FBD_DEFAULT_COLOR1_UG(32) |
			     SUN50I_FBD_DEFAULT_COLOR1_VB(32);
		break;
	case DRM_FORMAT_RGBA1010102:
		val |= SUN50I_FBD_FMT_YUV_TRAN;
		val |= SUN50I_FBD_FMT_IN_FMT(SUN50I_AFBC_RGBA1010102);
		def_color0 = SUN50I_FBD_DEFAULT_COLOR0_ALPHA(3) |
			     SUN50I_FBD_DEFAULT_COLOR0_YR(1023);
		def_color1 = SUN50I_FBD_DEFAULT_COLOR1_UG(1024) |
			     SUN50I_FBD_DEFAULT_COLOR1_VB(1024);
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

	/* Get the physical address of the buffer in memory */
	gem = drm_fb_dma_get_gem_obj(fb, 0);

	DRM_DEBUG_DRIVER("Using GEM @ %pad\n", &gem->dma_addr);

	/* Compute the start of the displayed memory */
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

	val = SUN50I_FBD_CTL_GLB_ALPHA(state->alpha >> 16);
	val |= SUN50I_FBD_CTL_CLK_GATE;
	val |= (state->alpha == DRM_BLEND_ALPHA_OPAQUE) ?
		SUN50I_FBD_CTL_ALPHA_MODE_PIXEL :
		SUN50I_FBD_CTL_ALPHA_MODE_COMBINED;
	val |= SUN50I_FBD_CTL_FBD_EN;
	regmap_write(regs, SUN50I_FBD_CTL(base), val);
}

void sun50i_afbc_disable(struct sun8i_mixer *mixer, unsigned int channel)
{
	u32 base = sun50i_afbc_get_base(mixer, channel);

	regmap_write(mixer->engine.regs, SUN50I_FBD_CTL(base), 0);
}
