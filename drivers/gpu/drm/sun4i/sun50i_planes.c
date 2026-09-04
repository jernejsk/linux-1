// SPDX-License-Identifier: GPL-2.0+
/* Copyright (c) 2025 Jernej Skrabec <jernej.skrabec@gmail.com> */

#include <drm/drm_device.h>

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include "sun50i_planes.h"
#include "sun8i_rdma.h"
#include "sun8i_ui_layer.h"
#include "sun8i_vi_layer.h"

static const struct sun50i_planes_quirks sun50i_h616_planes_quirks = {
	.def_map = {
		{
			.map = {0, 6, 7},
			.num_ch = 3,
		},
		{
			.map = {1, 2, 8},
			.num_ch = 3,
		},
	},
	.cfg = {
		.de_type	= SUN8I_MIXER_DE33,
		.afbc_mask	= BIT(0),
		.scaler_type    = {
			[0] = SUN8I_SCALER_VI_ED,
			[1] = SUN8I_SCALER_VI_8,
			[2] = SUN8I_SCALER_VI_8,
			[6] = SUN8I_SCALER_VI_8,
			[7] = SUN8I_SCALER_VI_8,
			[8] = SUN8I_SCALER_VI_8,
		},
		.scanline_yuv	= {4096, 2048, 2048},
		.scanline_rgb	= {4096, 2048, 2048, 0, 0, 0, 2048, 2048, 2048},
		.scanline_ed	= {4096},
	},
};

static const struct sun50i_planes_quirks sun60i_a733_planes_quirks = {
	.def_map = {
		{
			/*
			 * Channel 0 has an advanced scaler this driver cannot
			 * program yet, so leave it out and give the display
			 * the five channels whose scalers are understood.
			 */
			.map = {1, 2, 6, 7, 8},
			.num_ch = 5,
		},
	},
	.cfg = {
		.de_type	= SUN8I_MIXER_DE33,
		.scaler_type    = {
			[1] = SUN8I_SCALER_VI_8,
			[2] = SUN8I_SCALER_VI_8,
			[6] = SUN8I_SCALER_VI_8,
			[7] = SUN8I_SCALER_VI_8,
			[8] = SUN8I_SCALER_VI_8,
		},
		.scanline_yuv	= {4096, 2048, 2048},
		.scanline_rgb	= {4096, 2048, 2048, 0, 0, 0, 2048, 2048, 2048},
	},
};

static const struct of_device_id sun50i_planes_of_table[] = {
	{
		.compatible = "allwinner,sun50i-h616-de33-planes",
		.data = &sun50i_h616_planes_quirks
	},
	{
		.compatible = "allwinner,sun60i-a733-de33-planes",
		.data = &sun60i_a733_planes_quirks
	},
	{ }
};
MODULE_DEVICE_TABLE(of, sun50i_planes_of_table);

struct drm_plane **
sun50i_planes_setup(struct device *dev, struct drm_device *drm,
		    unsigned int mixer, struct sun8i_rdma *rdma)
{
	struct sun50i_planes *planes = dev_get_drvdata(dev);
	const struct sun50i_planes_quirks *quirks;
	struct drm_plane **drm_planes;
	const struct default_map *map;
	unsigned int i;

	if (!of_match_node(sun50i_planes_of_table, dev->of_node)) {
		dev_err(dev, "Device is not planes driver!\n");
		return ERR_PTR(-EINVAL);
	}

	if (!planes) {
		dev_err(dev, "Planes driver is not loaded yet!\n");
		return ERR_PTR(-EINVAL);
	}

	if (mixer > 1) {
		dev_err(dev, "Mixer index is too high!\n");
		return ERR_PTR(-EINVAL);
	}

	quirks = planes->quirks;
	map = &quirks->def_map[mixer];

	drm_planes = devm_kcalloc(drm->dev, map->num_ch + 1,
				  sizeof(*drm_planes), GFP_KERNEL);
	if (!drm_planes)
		return ERR_PTR(-ENOMEM);

	for (i = 0; i < map->num_ch; i++) {
		unsigned int phy_ch = map->map[i];
		struct sun8i_layer *layer;
		enum drm_plane_type type;

		if ((i == 0 && map->num_ch == 1) || i == 1)
			type = DRM_PLANE_TYPE_PRIMARY;
		else
			type = DRM_PLANE_TYPE_OVERLAY;

		if (phy_ch < UI_PLANE_OFFSET)
			layer = sun8i_vi_layer_init_one(drm, type, i, phy_ch,
							map->num_ch,
							&quirks->cfg, rdma,
							planes->base);
		else
			layer = sun8i_ui_layer_init_one(drm, type, i, phy_ch,
							map->num_ch,
							&quirks->cfg, rdma,
							planes->base);

		if (IS_ERR(layer)) {
			dev_err(drm->dev,
				"Couldn't initialize DRM plane\n");
			return ERR_CAST(layer);
		}

		drm_planes[i] = &layer->plane;
	}

	return drm_planes;
}

static void sun50i_planes_init_mapping(struct sun50i_planes *planes)
{
	const struct sun50i_planes_quirks *quirks = planes->quirks;
	unsigned int i, j;
	u32 mapping;

	mapping = 0;
	for (j = 0; j < MAX_DISP; j++)
		for (i = 0; i < quirks->def_map[j].num_ch; i++) {
			unsigned int ch = quirks->def_map[j].map[i];

			if (ch < UI_PLANE_OFFSET)
				mapping |= j << (ch * 2);
			else
				mapping |= j << ((ch - UI_PLANE_OFFSET) * 2 + 16);
		}
	regmap_write(planes->mapping, SUNXI_DE33_DE_CHN2CORE_MUX_REG, mapping);

	for (j = 0; j < MAX_DISP; j++) {
		mapping = 0;
		for (i = 0; i < quirks->def_map[j].num_ch; i++) {
			unsigned int ch = quirks->def_map[j].map[i];

			if (ch >= UI_PLANE_OFFSET)
				ch += 2;

			mapping |= ch << (i * 4);
		}
		regmap_write(planes->mapping, SUNXI_DE33_DE_PORT02CHN_MUX_REG + j * 4, mapping);
	}
}

static int sun50i_planes_probe(struct platform_device *pdev)
{
	struct platform_device *clk_pdev;
	struct device *dev = &pdev->dev;
	struct sun50i_planes *planes;
	struct device_node *np;
	int ret;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(40));
	if (ret)
		return dev_err_probe(dev, ret, "Cannot set DMA mask\n");

	planes = devm_kzalloc(dev, sizeof(*planes), GFP_KERNEL);
	if (!planes)
		return -ENOMEM;

	planes->quirks = of_device_get_match_data(dev);
	if (!planes->quirks)
		return dev_err_probe(dev, -EINVAL, "Unable to get quirks\n");

	np = of_parse_phandle(dev->of_node,
			      "allwinner,plane-mapping", 0);
	clk_pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!clk_pdev)
		return dev_err_probe(dev, -EPROBE_DEFER, "Unable to get mapping\n");

	planes->mapping = dev_get_regmap(&clk_pdev->dev, NULL);
	if (!planes->mapping) {
		platform_device_put(clk_pdev);
		return dev_err_probe(dev, -EINVAL, "Unable to get regmap\n");
	}

	if (!device_link_add(dev, &clk_pdev->dev,
			     DL_FLAG_AUTOREMOVE_CONSUMER)) {
		platform_device_put(clk_pdev);
		return dev_err_probe(dev, -EINVAL, "Failed to create link\n");
	}
	platform_device_put(clk_pdev);

	planes->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(planes->base))
		return PTR_ERR(planes->base);

	sun50i_planes_init_mapping(planes);

	dev_set_drvdata(dev, planes);

	return 0;
}

struct platform_driver sun50i_planes_platform_driver = {
	.probe		= sun50i_planes_probe,
	.driver		= {
		.name		= "sun50i-planes",
		.of_match_table	= sun50i_planes_of_table,
	},
};
