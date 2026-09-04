// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2018 Jernej Skrabec <jernej.skrabec@siol.net>
 */

#include <linux/component.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_encoder.h>
#include <drm/drm_of.h>

#include <uapi/linux/media-bus-format.h>

#include "sun4i_crtc.h"
#include "sun8i_dw_hdmi.h"
#include "sun8i_tcon_top.h"
#include "sunxi_engine.h"

#define bridge_to_sun8i_dw_hdmi(x) \
	container_of(x, struct sun8i_dw_hdmi, bridge)

static int sun8i_hdmi_enc_attach(struct drm_bridge *bridge,
				 struct drm_encoder *encoder,
				 enum drm_bridge_attach_flags flags)
{
	struct sun8i_dw_hdmi *hdmi = bridge_to_sun8i_dw_hdmi(bridge);

	return drm_bridge_attach(encoder, hdmi->next_bridge,
				 &hdmi->bridge, flags);
}

static int sun8i_hdmi_enc_atomic_check(struct drm_bridge *bridge,
				       struct drm_bridge_state *bridge_state,
				       struct drm_crtc_state *crtc_state,
				       struct drm_connector_state *conn_state)
{
	struct sun4i_crtc_state *scrtc_state =
		drm_crtc_state_to_sun4i_crtc_state(crtc_state);

	switch (conn_state->colorspace) {
	case DRM_MODE_COLORIMETRY_SMPTE_170M_YCC:
	case DRM_MODE_COLORIMETRY_XVYCC_601:
	case DRM_MODE_COLORIMETRY_SYCC_601:
	case DRM_MODE_COLORIMETRY_OPYCC_601:
	case DRM_MODE_COLORIMETRY_BT601_YCC:
		scrtc_state->encoding = DRM_COLOR_YCBCR_BT601;
		break;

	default:
	case DRM_MODE_COLORIMETRY_NO_DATA:
	case DRM_MODE_COLORIMETRY_BT709_YCC:
	case DRM_MODE_COLORIMETRY_XVYCC_709:
	case DRM_MODE_COLORIMETRY_RGB_WIDE_FIXED:
	case DRM_MODE_COLORIMETRY_RGB_WIDE_FLOAT:
	case DRM_MODE_COLORIMETRY_OPRGB:
		scrtc_state->encoding = DRM_COLOR_YCBCR_BT709;
		break;

	case DRM_MODE_COLORIMETRY_BT2020_CYCC:
	case DRM_MODE_COLORIMETRY_BT2020_YCC:
	case DRM_MODE_COLORIMETRY_BT2020_RGB:
	case DRM_MODE_COLORIMETRY_DCI_P3_RGB_D65:
	case DRM_MODE_COLORIMETRY_DCI_P3_RGB_THEATER:
		scrtc_state->encoding = DRM_COLOR_YCBCR_BT2020;
		break;
	}

	scrtc_state->format = bridge_state->output_bus_cfg.format;

	return 0;
}

static u32 *
sun8i_hdmi_enc_get_input_bus_fmts(struct drm_bridge *bridge,
				  struct drm_bridge_state *bridge_state,
				  struct drm_crtc_state *crtc_state,
				  struct drm_connector_state *conn_state,
				  u32 output_fmt,
				  unsigned int *num_input_fmts)
{
	struct sun4i_crtc *scrtc = drm_crtc_to_sun4i_crtc(crtc_state->crtc);
	u32 *input_fmt;

	*num_input_fmts = 0;

	if (!sunxi_engine_format_valid(scrtc->engine, output_fmt))
		return NULL;

	input_fmt = kmalloc_obj(*input_fmt);
	if (!input_fmt)
		return NULL;

	*num_input_fmts = 1;
	*input_fmt = output_fmt;

	return input_fmt;
}

static const struct drm_bridge_funcs sun8i_hdmi_enc_bridge_funcs = {
	.attach = sun8i_hdmi_enc_attach,
	.atomic_check = sun8i_hdmi_enc_atomic_check,
	.atomic_get_input_bus_fmts = sun8i_hdmi_enc_get_input_bus_fmts,
	.atomic_duplicate_state = drm_atomic_helper_bridge_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_bridge_destroy_state,
	.atomic_create_state = drm_atomic_helper_bridge_create_state,
};

static void
sun8i_dw_hdmi_encoder_atomic_mode_set(struct drm_encoder *encoder,
				      struct drm_crtc_state *crtc_state,
				      struct drm_connector_state *conn_state)
{
	struct sun4i_crtc_state *scrtc_state =
		drm_crtc_state_to_sun4i_crtc_state(crtc_state);
	struct sun8i_dw_hdmi *hdmi = encoder_to_sun8i_dw_hdmi(encoder);
	struct drm_display_mode *mode = &crtc_state->adjusted_mode;
	unsigned long rate = mode->crtc_clock * 1000UL;

	/*
	 * The TMDS character rate is derived from the pixel clock: deep
	 * color multiplies it (10-bit: 5/4), and YUV 4:2:0 halves it
	 * again since two horizontal samples share one TMDS period.
	 */
	switch (scrtc_state->format) {
	case MEDIA_BUS_FMT_UYYVYY10_0_5X30:
		rate = rate * 5 / 4 / 2;
		break;
	case MEDIA_BUS_FMT_UYYVYY8_0_5X24:
		rate = rate / 2;
		break;
	default:
		break;
	}

	clk_set_rate(hdmi->clk_tmds, rate);
}
static const struct drm_encoder_funcs sun8i_dw_hdmi_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static const struct drm_encoder_helper_funcs
sun8i_dw_hdmi_encoder_helper_funcs = {
	.atomic_mode_set = sun8i_dw_hdmi_encoder_atomic_mode_set,
};

static enum drm_mode_status
sun8i_dw_hdmi_mode_valid_a83t(struct dw_hdmi *hdmi, void *data,
			      const struct drm_display_info *info,
			      const struct drm_display_mode *mode)
{
	if (mode->clock > 297000)
		return MODE_CLOCK_HIGH;

	return MODE_OK;
}

static enum drm_mode_status
sun8i_dw_hdmi_mode_valid_h6(struct dw_hdmi *hdmi, void *data,
			    const struct drm_display_info *info,
			    const struct drm_display_mode *mode)
{
	/*
	 * Controller support maximum of 594 MHz, which correlates to
	 * 4K@60Hz 4:4:4 or RGB.
	 */
	if (mode->clock > 594000)
		return MODE_CLOCK_HIGH;

	return MODE_OK;
}

static bool sun8i_dw_hdmi_node_is_tcon_top(struct device_node *node)
{
	return IS_ENABLED(CONFIG_DRM_SUN8I_TCON_TOP) &&
		!!of_match_node(sun8i_tcon_top_of_table, node);
}

static u32 sun8i_dw_hdmi_find_possible_crtcs(struct drm_device *drm,
					     struct device_node *node)
{
	struct device_node *port, *ep, *remote, *remote_port;
	u32 crtcs = 0;

	remote = of_graph_get_remote_node(node, 0, -1);
	if (!remote)
		return 0;

	if (sun8i_dw_hdmi_node_is_tcon_top(remote)) {
		port = of_graph_get_port_by_id(remote, 4);
		if (!port)
			goto crtcs_exit;

		for_each_child_of_node(port, ep) {
			remote_port = of_graph_get_remote_port(ep);
			if (remote_port) {
				crtcs |= drm_of_crtc_port_mask(drm, remote_port);
				of_node_put(remote_port);
			}
		}

		of_node_put(port);
	} else {
		crtcs = drm_of_find_possible_crtcs(drm, node);
	}

crtcs_exit:
	of_node_put(remote);

	return crtcs;
}

static int sun8i_dw_hdmi_bind(struct device *dev, struct device *master,
			      void *data)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct dw_hdmi_plat_data *plat_data;
	struct drm_device *drm = data;
	struct device_node *phy_node;
	struct drm_encoder *encoder;
	struct sun8i_dw_hdmi *hdmi;
	int ret;

	if (!pdev->dev.of_node)
		return -ENODEV;

	hdmi = devm_drm_bridge_alloc(&pdev->dev, struct sun8i_dw_hdmi,
				     bridge, &sun8i_hdmi_enc_bridge_funcs);
	if (IS_ERR(hdmi))
		return PTR_ERR(hdmi);

	ret = devm_drm_bridge_add(&pdev->dev, &hdmi->bridge);
	if (ret)
		return ret;

	plat_data = &hdmi->plat_data;
	hdmi->dev = &pdev->dev;
	encoder = &hdmi->encoder;

	hdmi->quirks = of_device_get_match_data(dev);

	encoder->possible_crtcs =
		sun8i_dw_hdmi_find_possible_crtcs(drm, dev->of_node);
	/*
	 * If we failed to find the CRTC(s) which this encoder is
	 * supposed to be connected to, it's because the CRTC has
	 * not been registered yet.  Defer probing, and hope that
	 * the required CRTC is added later.
	 */
	if (encoder->possible_crtcs == 0)
		return -EPROBE_DEFER;

	hdmi->rst_ctrl = devm_reset_control_get(dev, "ctrl");
	if (IS_ERR(hdmi->rst_ctrl))
		return dev_err_probe(dev, PTR_ERR(hdmi->rst_ctrl),
				     "Could not get ctrl reset control\n");

	hdmi->clk_tmds = devm_clk_get(dev, "tmds");
	if (IS_ERR(hdmi->clk_tmds))
		return dev_err_probe(dev, PTR_ERR(hdmi->clk_tmds),
				     "Couldn't get the tmds clock\n");

	hdmi->regulator = devm_regulator_get(dev, "hvcc");
	if (IS_ERR(hdmi->regulator))
		return dev_err_probe(dev, PTR_ERR(hdmi->regulator),
				     "Couldn't get regulator\n");

	ret = regulator_enable(hdmi->regulator);
	if (ret) {
		dev_err(dev, "Failed to enable regulator\n");
		return ret;
	}

	ret = reset_control_deassert(hdmi->rst_ctrl);
	if (ret) {
		dev_err(dev, "Could not deassert ctrl reset control\n");
		goto err_disable_regulator;
	}

	ret = clk_prepare_enable(hdmi->clk_tmds);
	if (ret) {
		dev_err(dev, "Could not enable tmds clock\n");
		goto err_assert_ctrl_reset;
	}

	phy_node = of_parse_phandle(dev->of_node, "phys", 0);
	if (!phy_node) {
		dev_err(dev, "Can't found PHY phandle\n");
		ret = -EINVAL;
		goto err_disable_clk_tmds;
	}

	ret = sun8i_hdmi_phy_get(hdmi, phy_node);
	of_node_put(phy_node);
	if (ret) {
		dev_err(dev, "Couldn't get the HDMI PHY\n");
		goto err_disable_clk_tmds;
	}

	ret = sun8i_hdmi_phy_init(hdmi->phy);
	if (ret)
		goto err_disable_clk_tmds;

	plat_data->mode_valid = hdmi->quirks->mode_valid;
	plat_data->use_drm_infoframe = hdmi->quirks->use_drm_infoframe;
	plat_data->ycbcr_420_allowed = hdmi->quirks->ycbcr_420_allowed;
	/*
	 * The controller shares the same distinct video mapping code for
	 * packed YUV 4:4:4 across all sun8i variants, even where YUV
	 * output is never negotiated in practice.
	 */
	plat_data->ycbcr444_alt_vmap = true;
	plat_data->phy_max_tmds_msm_ctrl_quirk = true;
	sun8i_hdmi_phy_set_ops(hdmi->phy, plat_data);

	platform_set_drvdata(pdev, hdmi);

	hdmi->hdmi = dw_hdmi_probe(pdev, plat_data);
	if (IS_ERR(hdmi->hdmi)) {
		ret = PTR_ERR(hdmi->hdmi);
		goto err_deinit_phy;
	}

	/* dw_hdmi_probe() registered a bridge for our own OF node. */
	hdmi->next_bridge = of_drm_find_bridge(dev->of_node);
	if (!hdmi->next_bridge) {
		ret = -ENODEV;
		goto err_remove_dw_hdmi;
	}

	drm_encoder_helper_add(encoder, &sun8i_dw_hdmi_encoder_helper_funcs);
	ret = drmm_encoder_init(drm, encoder, NULL, DRM_MODE_ENCODER_TMDS, NULL);
	if (ret)
		goto err_remove_dw_hdmi;

	/*
	 * Attach our bridge in front of the dw-hdmi one, so display
	 * engine constraints participate in bus format negotiation.
	 * dw-hdmi still creates the connector.
	 */
	ret = drm_bridge_attach(encoder, &hdmi->bridge, NULL, 0);
	if (ret)
		goto cleanup_encoder;

	return 0;

cleanup_encoder:
	drm_encoder_cleanup(encoder);
err_remove_dw_hdmi:
	dw_hdmi_remove(hdmi->hdmi);
err_deinit_phy:
	sun8i_hdmi_phy_deinit(hdmi->phy);
err_disable_clk_tmds:
	clk_disable_unprepare(hdmi->clk_tmds);
err_assert_ctrl_reset:
	reset_control_assert(hdmi->rst_ctrl);
err_disable_regulator:
	regulator_disable(hdmi->regulator);

	return ret;
}

static void sun8i_dw_hdmi_unbind(struct device *dev, struct device *master,
				 void *data)
{
	struct sun8i_dw_hdmi *hdmi = dev_get_drvdata(dev);

	dw_hdmi_remove(hdmi->hdmi);
	sun8i_hdmi_phy_deinit(hdmi->phy);
	clk_disable_unprepare(hdmi->clk_tmds);
	reset_control_assert(hdmi->rst_ctrl);
	regulator_disable(hdmi->regulator);
}

static const struct component_ops sun8i_dw_hdmi_ops = {
	.bind	= sun8i_dw_hdmi_bind,
	.unbind	= sun8i_dw_hdmi_unbind,
};

static int sun8i_dw_hdmi_probe(struct platform_device *pdev)
{
	return component_add(&pdev->dev, &sun8i_dw_hdmi_ops);
}

static void sun8i_dw_hdmi_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &sun8i_dw_hdmi_ops);
}

static const struct sun8i_dw_hdmi_quirks sun8i_a83t_quirks = {
	.mode_valid = sun8i_dw_hdmi_mode_valid_a83t,
};

static const struct sun8i_dw_hdmi_quirks sun50i_h6_quirks = {
	.mode_valid = sun8i_dw_hdmi_mode_valid_h6,
	.use_drm_infoframe = true,
	.ycbcr_420_allowed = true,
};

static const struct of_device_id sun8i_dw_hdmi_dt_ids[] = {
	{
		.compatible = "allwinner,sun8i-a83t-dw-hdmi",
		.data = &sun8i_a83t_quirks,
	},
	{
		.compatible = "allwinner,sun50i-h6-dw-hdmi",
		.data = &sun50i_h6_quirks,
	},
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, sun8i_dw_hdmi_dt_ids);

static struct platform_driver sun8i_dw_hdmi_pltfm_driver = {
	.probe  = sun8i_dw_hdmi_probe,
	.remove = sun8i_dw_hdmi_remove,
	.driver = {
		.name = "sun8i-dw-hdmi",
		.of_match_table = sun8i_dw_hdmi_dt_ids,
	},
};

static int __init sun8i_dw_hdmi_init(void)
{
	int ret;

	ret = platform_driver_register(&sun8i_dw_hdmi_pltfm_driver);
	if (ret)
		return ret;

	ret = platform_driver_register(&sun8i_hdmi_phy_driver);
	if (ret) {
		platform_driver_unregister(&sun8i_dw_hdmi_pltfm_driver);
		return ret;
	}

	return ret;
}

static void __exit sun8i_dw_hdmi_exit(void)
{
	platform_driver_unregister(&sun8i_dw_hdmi_pltfm_driver);
	platform_driver_unregister(&sun8i_hdmi_phy_driver);
}

module_init(sun8i_dw_hdmi_init);
module_exit(sun8i_dw_hdmi_exit);

MODULE_AUTHOR("Jernej Skrabec <jernej.skrabec@siol.net>");
MODULE_DESCRIPTION("Allwinner DW HDMI bridge");
MODULE_LICENSE("GPL");
