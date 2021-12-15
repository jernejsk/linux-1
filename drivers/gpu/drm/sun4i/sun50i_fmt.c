// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) Jernej Skrabec <jernej.skrabec@gmail.com>
 */

#include "sun50i_fmt.h"

#include <uapi/linux/media-bus-format.h>

static bool sun50i_fmt_is_rgb(u32 format)
{
	switch (format) {
	case MEDIA_BUS_FMT_RGB101010_1X30:
	case MEDIA_BUS_FMT_RGB888_1X24:
		return true;
	default:
		return false;
	}
}

static bool sun50i_fmt_is_10bit(u32 format)
{
	switch (format) {
	case MEDIA_BUS_FMT_RGB101010_1X30:
	case MEDIA_BUS_FMT_YUV10_1X30:
	case MEDIA_BUS_FMT_UYYVYY10_0_5X30:
	case MEDIA_BUS_FMT_UYVY10_1X20:
		return true;
	default:
		return false;
	}
}

static u32 sun50i_fmt_get_colorspace(u32 format)
{
	switch (format) {
	case MEDIA_BUS_FMT_UYYVYY8_0_5X24:
	case MEDIA_BUS_FMT_UYYVYY10_0_5X30:
		return SUN50I_FMT_CS_YUV420;
	case MEDIA_BUS_FMT_UYVY8_1X16:
	case MEDIA_BUS_FMT_UYVY10_1X20:
		return SUN50I_FMT_CS_YUV422;
	default:
		return SUN50I_FMT_CS_YUV444RGB;
	}
}

void sun50i_fmt_setup(struct sun8i_mixer *mixer, u16 width, u16 height)
{
	u32 colorspace, limit[3];
	bool bit10;

	colorspace = sun50i_fmt_get_colorspace(mixer->out_format);
	bit10 = sun50i_fmt_is_10bit(mixer->out_format);

	regmap_write(mixer->engine.regs, SUN50I_FMT_CTRL, 0);

	regmap_write(mixer->engine.regs, SUN50I_FMT_SIZE,
		     SUN8I_MIXER_SIZE(width, height));
	regmap_write(mixer->engine.regs, SUN50I_FMT_SWAP,
		     !sun50i_fmt_is_rgb(mixer->out_format));
	regmap_write(mixer->engine.regs, SUN50I_FMT_DEPTH, bit10);
	regmap_write(mixer->engine.regs, SUN50I_FMT_FORMAT, colorspace);
	regmap_write(mixer->engine.regs, SUN50I_FMT_COEF, 0);

	if (colorspace != SUN50I_FMT_CS_YUV444RGB) {
		limit[0] = SUN50I_FMT_LIMIT(64, 940);
		limit[1] = SUN50I_FMT_LIMIT(64, 960);
		limit[2] = SUN50I_FMT_LIMIT(64, 960);
	} else if (bit10) {
		limit[0] = SUN50I_FMT_LIMIT(0, 1023);
		limit[1] = SUN50I_FMT_LIMIT(0, 1023);
		limit[2] = SUN50I_FMT_LIMIT(0, 1023);
	} else {
		limit[0] = SUN50I_FMT_LIMIT(0, 1021);
		limit[1] = SUN50I_FMT_LIMIT(0, 1021);
		limit[2] = SUN50I_FMT_LIMIT(0, 1021);
	}

	regmap_write(mixer->engine.regs, SUN50I_FMT_LMT_Y, limit[0]);
	regmap_write(mixer->engine.regs, SUN50I_FMT_LMT_C0, limit[1]);
	regmap_write(mixer->engine.regs, SUN50I_FMT_LMT_C1, limit[2]);

	regmap_write(mixer->engine.regs, SUN50I_FMT_CTRL, 1);
}
