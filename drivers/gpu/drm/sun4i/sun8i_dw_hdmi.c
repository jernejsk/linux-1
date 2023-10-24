// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2018 Jernej Skrabec <jernej.skrabec@siol.net>
 */

#include <linux/component.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/videodev2.h>

#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_bridge_connector.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_of.h>
#include <drm/drm_print.h>
#include <drm/drm_simple_kms_helper.h>

#include <media/cec-notifier.h>

#include <uapi/linux/media-bus-format.h>

#include "sun4i_crtc.h"
#include "sun4i_tcon.h"
#include "sun8i_dw_hdmi.h"
#include "sun8i_tcon_top.h"
#include "sunxi_engine.h"

#define bridge_to_sun8i_dw_hdmi(x) \
	container_of(x, struct sun8i_dw_hdmi, enc_bridge)

static int sun8i_hdmi_enc_attach(struct drm_bridge *bridge,
				 enum drm_bridge_attach_flags flags)
{
	struct sun8i_dw_hdmi *hdmi = bridge_to_sun8i_dw_hdmi(bridge);

	return drm_bridge_attach(&hdmi->encoder, hdmi->hdmi_bridge,
				 &hdmi->enc_bridge, flags);
}

static void sun8i_hdmi_enc_detach(struct drm_bridge *bridge)
{
	struct sun8i_dw_hdmi *hdmi = bridge_to_sun8i_dw_hdmi(bridge);

	cec_notifier_conn_unregister(hdmi->cec_notifier);
	hdmi->cec_notifier = NULL;
}

static void sun8i_hdmi_enc_hpd_notify(struct drm_bridge *bridge,
				      enum drm_connector_status status)
{
	struct sun8i_dw_hdmi *hdmi = bridge_to_sun8i_dw_hdmi(bridge);
	struct edid *edid;

	if (!hdmi->cec_notifier)
		return;

	if (status == connector_status_connected) {
		edid = drm_bridge_get_edid(hdmi->hdmi_bridge, hdmi->connector);
		if (edid)
			cec_notifier_set_phys_addr_from_edid(hdmi->cec_notifier,
							     edid);
	} else {
		cec_notifier_phys_addr_invalidate(hdmi->cec_notifier);
	}
}

static int sun8i_hdmi_enc_atomic_check(struct drm_bridge *bridge,
				       struct drm_bridge_state *bridge_state,
				       struct drm_crtc_state *crtc_state,
				       struct drm_connector_state *conn_state)
{
	struct sun4i_crtc *crtc = drm_crtc_to_sun4i_crtc(crtc_state->crtc);
	struct sunxi_engine *engine = crtc->engine;
	struct drm_connector_state *old_conn_state;

	old_conn_state =
		drm_atomic_get_old_connector_state(conn_state->state,
						   conn_state->connector);

	switch (conn_state->colorspace) {
	case DRM_MODE_COLORIMETRY_SMPTE_170M_YCC:
	case DRM_MODE_COLORIMETRY_XVYCC_601:
	case DRM_MODE_COLORIMETRY_SYCC_601:
	case DRM_MODE_COLORIMETRY_OPYCC_601:
	case DRM_MODE_COLORIMETRY_BT601_YCC:
		engine->encoding = DRM_COLOR_YCBCR_BT601;
		break;

	default:
	case DRM_MODE_COLORIMETRY_NO_DATA:
	case DRM_MODE_COLORIMETRY_BT709_YCC:
	case DRM_MODE_COLORIMETRY_XVYCC_709:
	case DRM_MODE_COLORIMETRY_RGB_WIDE_FIXED:
	case DRM_MODE_COLORIMETRY_RGB_WIDE_FLOAT:
		engine->encoding = DRM_COLOR_YCBCR_BT709;
		break;

	case DRM_MODE_COLORIMETRY_BT2020_CYCC:
	case DRM_MODE_COLORIMETRY_BT2020_YCC:
	case DRM_MODE_COLORIMETRY_BT2020_RGB:
	case DRM_MODE_COLORIMETRY_DCI_P3_RGB_D65:
	case DRM_MODE_COLORIMETRY_DCI_P3_RGB_THEATER:
		engine->encoding = DRM_COLOR_YCBCR_BT2020;
		break;
	}

	engine->format = bridge_state->output_bus_cfg.format;
	DRM_DEBUG_DRIVER("HDMI output bus format: 0x%04x\n", engine->format);

	if (!drm_connector_atomic_hdr_metadata_equal(old_conn_state, conn_state))
		crtc_state->mode_changed = true;

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
	struct sun4i_crtc *crtc = drm_crtc_to_sun4i_crtc(crtc_state->crtc);
	u32 *input_fmt, *supported, count, i;

	*num_input_fmts = 0;
	input_fmt = NULL;

	supported = sunxi_engine_get_supported_formats(crtc->engine, &count);
	if (count == 0 || !supported)
		return NULL;

	for (i = 0; i < count; i++)
		if (output_fmt == supported[i]) {
			input_fmt = kzalloc(sizeof(*input_fmt), GFP_KERNEL);
			if (!input_fmt)
				break;

			*num_input_fmts = 1;
			*input_fmt = output_fmt;

			break;
		}

	kfree(supported);

	return input_fmt;
}

static const struct drm_bridge_funcs sun8i_hdmi_enc_bridge_funcs = {
	.attach = sun8i_hdmi_enc_attach,
	.detach = sun8i_hdmi_enc_detach,
	.hpd_notify = sun8i_hdmi_enc_hpd_notify,
	.atomic_check = sun8i_hdmi_enc_atomic_check,
	.atomic_duplicate_state = drm_atomic_helper_bridge_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_bridge_destroy_state,
	.atomic_get_input_bus_fmts = sun8i_hdmi_enc_get_input_bus_fmts,
	.atomic_reset = drm_atomic_helper_bridge_reset,
};

static void
sun8i_dw_hdmi_encoder_atomic_mode_set(struct drm_encoder *encoder,
				      struct drm_crtc_state *crtc_state,
				      struct drm_connector_state *conn_state)
{
	struct sun4i_crtc *crtc = drm_crtc_to_sun4i_crtc(crtc_state->crtc);
	struct sun8i_dw_hdmi *hdmi = encoder_to_sun8i_dw_hdmi(encoder);
	struct drm_display_mode *mode = &crtc_state->adjusted_mode;
	int div;

	switch (crtc->engine->format) {
	case MEDIA_BUS_FMT_UYYVYY8_0_5X24:
	case MEDIA_BUS_FMT_UYYVYY10_0_5X30:
		div = 2;
		break;
	default:
		div = 1;
		break;
	}

	clk_set_rate(hdmi->clk_tmds, mode->crtc_clock * 1000 / div);
}

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
	unsigned long clock = mode->crtc_clock * 1000;

	if (drm_mode_is_420(info, mode))
		clock /= 2;

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
	struct cec_connector_info conn_info;
	struct drm_connector *connector;
	struct drm_device *drm = data;
	struct device_node *phy_node;
	struct drm_encoder *encoder;
	struct sun8i_dw_hdmi *hdmi;
	struct sun8i_hdmi_phy *phy;
	int ret;

	if (!pdev->dev.of_node)
		return -ENODEV;

	phy_node = of_parse_phandle(dev->of_node, "phys", 0);
	if (!phy_node) {
		dev_err(dev, "Can't find PHY phandle\n");
		return -EINVAL;
	}

	phy = sun8i_hdmi_phy_get(phy_node);
	of_node_put(phy_node);
	if (IS_ERR(phy))
		return dev_err_probe(dev, PTR_ERR(phy),
				     "Couldn't get the HDMI PHY\n");

	ret = sun8i_hdmi_phy_register_cec(phy, dev);
	if (ret)
		return ret;

	hdmi = drmm_kzalloc(drm, sizeof(*hdmi), GFP_KERNEL);
	if (!hdmi)
		return -ENOMEM;

	plat_data = &hdmi->plat_data;
	hdmi->dev = &pdev->dev;
	encoder = &hdmi->encoder;
	hdmi->phy = phy;

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

	ret = sun8i_hdmi_phy_init(phy);
	if (ret)
		goto err_disable_clk_tmds;

	drm_encoder_helper_add(encoder, &sun8i_dw_hdmi_encoder_helper_funcs);
	ret = drm_simple_encoder_init(drm, encoder, DRM_MODE_ENCODER_TMDS);
	if (ret)
		goto err_deinit_phy;

	plat_data->mode_valid = hdmi->quirks->mode_valid;
	plat_data->use_drm_infoframe = hdmi->quirks->use_drm_infoframe;
	plat_data->ycbcr_420_allowed = hdmi->quirks->use_drm_infoframe;
	plat_data->input_bus_encoding = V4L2_YCBCR_ENC_709;
	plat_data->output_port = 1;
	plat_data->alt_yuv_map = 1;
	sun8i_hdmi_phy_set_ops(phy, plat_data);

	platform_set_drvdata(pdev, hdmi);

	hdmi->hdmi = dw_hdmi_probe(pdev, plat_data);
	if (IS_ERR(hdmi->hdmi)) {
		ret = PTR_ERR(hdmi->hdmi);
		goto err_deinit_phy;
	}

	hdmi->hdmi_bridge = of_drm_find_bridge(dev->of_node);

	hdmi->enc_bridge.funcs = &sun8i_hdmi_enc_bridge_funcs;
	hdmi->enc_bridge.type = DRM_MODE_CONNECTOR_HDMIA;
	hdmi->enc_bridge.interlace_allowed = true;

	drm_bridge_add(&hdmi->enc_bridge);

	ret = drm_bridge_attach(encoder, &hdmi->enc_bridge, NULL,
				DRM_BRIDGE_ATTACH_NO_CONNECTOR);
	if (ret)
		goto err_remove_dw_hdmi;

	connector = drm_bridge_connector_init(drm, encoder);
	if (IS_ERR(connector)) {
		dev_err(dev, "Unable to create HDMI bridge connector\n");
		ret = PTR_ERR(connector);
		goto err_remove_dw_hdmi;
	}

	hdmi->connector = connector;
	drm_connector_attach_encoder(connector, encoder);

	drm_atomic_helper_connector_reset(connector);

	drm_mode_create_hdmi_colorspace_property(connector, 0);

	if (hdmi->quirks->use_drm_infoframe) {
		drm_connector_attach_hdr_output_metadata_property(connector);
		drm_connector_attach_max_bpc_property(connector, 8, 12);
		drm_connector_attach_colorspace_property(connector);
	}

	connector->ycbcr_420_allowed = hdmi->quirks->use_drm_infoframe;

	cec_fill_conn_info_from_drm(&conn_info, connector);

	hdmi->cec_notifier = cec_notifier_conn_register(&pdev->dev, NULL,
							&conn_info);
	if (!hdmi->cec_notifier) {
		ret = -ENOMEM;
		goto err_remove_dw_hdmi;
	}

	return 0;

err_remove_dw_hdmi:
	drm_bridge_remove(&hdmi->enc_bridge);
	dw_hdmi_remove(hdmi->hdmi);
err_deinit_phy:
	sun8i_hdmi_phy_deinit(phy);
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

	drm_bridge_remove(&hdmi->enc_bridge);
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
	.remove_new = sun8i_dw_hdmi_remove,
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
