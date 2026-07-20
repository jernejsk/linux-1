// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Jernej Skrabec <jernej.skrabec@gmail.com>
 */

#include <uapi/linux/media-bus-format.h>

#include "sun50i_fmt.h"
#include "sun8i_rdma.h"

static const struct reg_region sun50i_fmt_regions[] = {
	{ SUN50I_FMT_CTRL, 6 },
	{ SUN50I_FMT_LMT_Y, 3 },
	{ }
};

static u32 sun50i_fmt_get_colorspace(u32 format)
{
	switch (format) {
	case MEDIA_BUS_FMT_UYYVYY8_0_5X24:
		return SUN50I_FMT_CS_YUV420;
	default:
		return SUN50I_FMT_CS_YUV444RGB;
	}
}

void sun50i_fmt_setup(struct sun8i_mixer *mixer, u16 width,
		      u16 height, u32 format)
{
	u32 colorspace, limit[3];
	bool bypass;

	colorspace = sun50i_fmt_get_colorspace(format);
	bypass = colorspace == SUN50I_FMT_CS_YUV444RGB;

	sun8i_rdma_write(mixer->fmt_rdma, SUN50I_FMT_SIZE,
			 SUN8I_MIXER_SIZE(width, height));
	sun8i_rdma_write(mixer->fmt_rdma, SUN50I_FMT_SWAP, 0);
	/* bit depth compensation is needed for 10-bit output only */
	sun8i_rdma_write(mixer->fmt_rdma, SUN50I_FMT_DEPTH, 0);
	sun8i_rdma_write(mixer->fmt_rdma, SUN50I_FMT_FORMAT, colorspace);
	sun8i_rdma_write(mixer->fmt_rdma, SUN50I_FMT_COEF, 0);

	if (bypass) {
		limit[0] = SUN50I_FMT_LIMIT(0, 1021);
		limit[1] = SUN50I_FMT_LIMIT(0, 1021);
		limit[2] = SUN50I_FMT_LIMIT(0, 1021);
	} else {
		limit[0] = SUN50I_FMT_LIMIT(64, 940);
		limit[1] = SUN50I_FMT_LIMIT(64, 960);
		limit[2] = SUN50I_FMT_LIMIT(64, 960);
	}

	sun8i_rdma_write(mixer->fmt_rdma, SUN50I_FMT_LMT_Y, limit[0]);
	sun8i_rdma_write(mixer->fmt_rdma, SUN50I_FMT_LMT_C0, limit[1]);
	sun8i_rdma_write(mixer->fmt_rdma, SUN50I_FMT_LMT_C1, limit[2]);

	sun8i_rdma_write(mixer->fmt_rdma, SUN50I_FMT_CTRL, !bypass);
}

int sun50i_fmt_init(struct sun8i_mixer *mixer)
{
	u32 base, reg_offset = 0;

	if (mixer->cfg->de_type == SUN8I_MIXER_DE33) {
		base = SUN50I_FMT_DE33;
		reg_offset = 0x280000 + mixer->engine.id * 0x20000;
	} else {
		base = SUN50I_FMT_DE3;
	}

	mixer->fmt_rdma = sun8i_rdma_add_unit(mixer->rdma,
					      mixer->base + base,
					      reg_offset + base, 0x2c,
					      sun50i_fmt_regions);

	return mixer->fmt_rdma ? 0 : -ENOMEM;
}
