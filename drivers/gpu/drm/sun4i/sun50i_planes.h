/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (c) 2025 Jernej Skrabec <jernej.skrabec@gmail.com> */

#ifndef _SUN50I_PLANES_H_
#define _SUN50I_PLANES_H_

#include "sun8i_mixer.h"

/*
 * Mapping registers, located in the clock register space. Later parts split
 * the single channel mux into one register per channel kind and moved the
 * per-display port muxes up, so the offsets are described per SoC.
 */
struct sun50i_planes_mux_regs {
	unsigned int	vi_chn;		/* video channel to display mux */
	unsigned int	ui_chn;		/* interface channel to display mux */
	unsigned int	ui_chn_shift;	/* where the interface channels start */
	unsigned int	port_chn;	/* first per-display port mux */
};

#define MAX_DISP	2
#define UI_PLANE_OFFSET	6

struct regmap;
struct drm_device;
struct sun8i_rdma;

struct default_map {
	unsigned int map[MAX_CHANNELS];
	unsigned int num_ch;
};

struct sun50i_planes_quirks {
	struct default_map		def_map[MAX_DISP];
	struct sun50i_planes_mux_regs	mux;
	struct sun8i_layer_cfg		cfg;
};

struct sun50i_planes {
	struct regmap				*mapping;
	const struct sun50i_planes_quirks	*quirks;
	void __iomem				*base;
};

struct drm_plane **
sun50i_planes_setup(struct device *dev, struct drm_device *drm,
		    unsigned int mixer, struct sun8i_rdma *rdma);

extern struct platform_driver sun50i_planes_platform_driver;

#endif /* _SUN50I_PLANES_H_ */
