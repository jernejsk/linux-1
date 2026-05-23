// SPDX-License-Identifier: GPL-2.0+
/* Copyright (c) 2026 Jernej Skrabec <jernej.skrabec@gmail.com> */

/*
 * Allwinner A523/A527/T527 eDP/DP TX driver.
 *
 * The controller is an Innosilicon eDP 1.3 TX core fed by TCON-TV1.
 * This driver currently provides probe / clock + reset management /
 * AUX channel / hot-plug detect. Link training and PHY programming
 * are stubbed out and will be added in follow-up patches.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>

#include <drm/display/drm_dp_helper.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_drv.h>
#include <drm/drm_of.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>

/* Subset of Innosilicon eDP 1.3 registers needed for boot-time setup. */
#define SUN55I_EDP_HPD_SCALE		0x0018
#define  SUN55I_EDP_HPD_SCALE_EN	BIT(3)
#define SUN55I_EDP_RESET		0x001c
#define SUN55I_EDP_HPD_EVENT		0x0080
#define  SUN55I_EDP_HPD_EVENT_PLUG	BIT(0)
#define  SUN55I_EDP_HPD_EVENT_AUX_REPLY	BIT(1)
#define SUN55I_EDP_HPD_INT		0x0084
#define  SUN55I_EDP_HPD_INT_EN		BIT(0)
#define SUN55I_EDP_HPD_PLUG		0x0088
#define  SUN55I_EDP_HPD_PLUG_IN		BIT(1)
#define  SUN55I_EDP_HPD_PLUG_OUT	BIT(2)
#define SUN55I_EDP_HPD_EN		0x008c
#define  SUN55I_EDP_HPD_EN_PLUG_IN	BIT(1)
#define  SUN55I_EDP_HPD_EN_PLUG_OUT	BIT(2)

#define SUN55I_EDP_PHY_AUX		0x0400
#define SUN55I_EDP_AUX_TIMEOUT		0x0404
#define  SUN55I_EDP_AUX_TIMEOUT_STATUS	GENMASK(17, 16)
#define  SUN55I_EDP_AUX_REPLY_TYPE	GENMASK(27, 24)
#define  SUN55I_EDP_AUX_REPLY_CODE	GENMASK(7, 4)
#define  SUN55I_EDP_AUX_REPLY_NO_STOP	0xe
#define SUN55I_EDP_AUX_DATA0		0x0408
#define SUN55I_EDP_AUX_START		0x0418

#define SUN55I_EDP_AUX_BLOCK_MAX	16
#define SUN55I_EDP_AUX_TIMEOUT_US	50000

struct sun55i_edp_variant {
	int connector_type;
};

struct sun55i_edp {
	struct device		*dev;
	void __iomem		*regs;

	struct clk		*clk_bus;
	struct clk		*clk_mod;
	struct clk		*clk_24m;
	struct reset_control	*rst_bus;

	struct regulator	*vdd_supply;
	struct regulator	*vcc_supply;

	int			irq;

	const struct sun55i_edp_variant *variant;

	struct drm_dp_aux	aux;
	struct drm_bridge	bridge;
	struct drm_encoder	encoder;
	struct drm_connector	connector;

	bool			plugged;
};

static inline struct sun55i_edp *bridge_to_sun55i_edp(struct drm_bridge *b)
{
	return container_of(b, struct sun55i_edp, bridge);
}

static inline struct sun55i_edp *connector_to_sun55i_edp(struct drm_connector *c)
{
	return container_of(c, struct sun55i_edp, connector);
}

static void sun55i_edp_aux_clear_reply(struct sun55i_edp *edp)
{
	u32 val = readl(edp->regs + SUN55I_EDP_HPD_EVENT);

	writel(val | SUN55I_EDP_HPD_EVENT_AUX_REPLY,
	       edp->regs + SUN55I_EDP_HPD_EVENT);
}

static ssize_t sun55i_edp_aux_transfer(struct drm_dp_aux *aux,
				       struct drm_dp_aux_msg *msg)
{
	struct sun55i_edp *edp = container_of(aux, struct sun55i_edp, aux);
	bool is_write;
	u32 request, val;
	int ret, i;

	if (msg->size > SUN55I_EDP_AUX_BLOCK_MAX)
		return -E2BIG;
	if (msg->size && !msg->buffer)
		return -EINVAL;

	is_write = (msg->request & ~DP_AUX_I2C_MOT) == DP_AUX_NATIVE_WRITE ||
		   (msg->request & ~DP_AUX_I2C_MOT) == DP_AUX_I2C_WRITE;

	/* Pre-clear request, data and any pending AUX reply event. */
	writel(0, edp->regs + SUN55I_EDP_PHY_AUX);
	for (i = 0; i < 4; i++)
		writel(0, edp->regs + SUN55I_EDP_AUX_DATA0 + i * 4);
	sun55i_edp_aux_clear_reply(edp);

	if (is_write) {
		u32 buf[4] = { 0 };
		const u8 *src = msg->buffer;

		for (i = 0; i < msg->size; i++)
			buf[i / 4] |= src[i] << ((i % 4) * 8);
		for (i = 0; i < DIV_ROUND_UP(msg->size, 4); i++) {
			writel(buf[i],
			       edp->regs + SUN55I_EDP_AUX_DATA0 + i * 4);
			usleep_range(10, 20);
		}
	}

	request = (msg->size ? (msg->size - 1) : 0) & 0x1f;
	request |= FIELD_PREP(GENMASK(27, 8), msg->address);
	request |= FIELD_PREP(GENMASK(31, 28), msg->request);
	writel(request, edp->regs + SUN55I_EDP_PHY_AUX);
	udelay(1);
	writel(1, edp->regs + SUN55I_EDP_AUX_START);

	ret = readl_poll_timeout(edp->regs + SUN55I_EDP_HPD_EVENT, val,
				 val & SUN55I_EDP_HPD_EVENT_AUX_REPLY,
				 5, SUN55I_EDP_AUX_TIMEOUT_US);
	if (ret) {
		dev_dbg(edp->dev, "AUX request 0x%08x timed out\n", request);
		ret = -ETIMEDOUT;
		goto out;
	}

	ret = readl_poll_timeout(edp->regs + SUN55I_EDP_AUX_TIMEOUT, val,
				 FIELD_GET(SUN55I_EDP_AUX_TIMEOUT_STATUS, val) == 0,
				 5, SUN55I_EDP_AUX_TIMEOUT_US);
	if (ret) {
		dev_dbg(edp->dev, "AUX reply for 0x%08x timed out\n", request);
		ret = -ETIMEDOUT;
		goto out;
	}

	val = readl(edp->regs + SUN55I_EDP_AUX_TIMEOUT);
	if (FIELD_GET(SUN55I_EDP_AUX_REPLY_TYPE, val) ==
	    SUN55I_EDP_AUX_REPLY_NO_STOP) {
		dev_dbg(edp->dev, "AUX reply for 0x%08x lacked STOP\n",
			request);
		ret = -EIO;
		goto out;
	}

	msg->reply = FIELD_GET(SUN55I_EDP_AUX_REPLY_CODE, val);

	if (!is_write && msg->reply == DP_AUX_NATIVE_REPLY_ACK) {
		u32 buf[4];
		u8 *dst = msg->buffer;

		for (i = 0; i < 4; i++) {
			buf[i] = readl(edp->regs +
				       SUN55I_EDP_AUX_DATA0 + i * 4);
			usleep_range(10, 20);
		}
		for (i = 0; i < msg->size; i++)
			dst[i] = (buf[i / 4] >> ((i % 4) * 8)) & 0xff;
	}

	ret = msg->size;

out:
	sun55i_edp_aux_clear_reply(edp);
	return ret;
}

static int sun55i_edp_bridge_attach(struct drm_bridge *bridge,
				    struct drm_encoder *encoder,
				    enum drm_bridge_attach_flags flags)
{
	struct sun55i_edp *edp = bridge_to_sun55i_edp(bridge);

	if (flags & DRM_BRIDGE_ATTACH_NO_CONNECTOR) {
		dev_err(edp->dev, "Fix bridge driver to make connector optional!\n");
		return -EINVAL;
	}

	return 0;
}

static void sun55i_edp_bridge_atomic_enable(struct drm_bridge *bridge,
					    struct drm_atomic_state *state)
{
	/* TODO: link training + video stream enable. */
}

static void sun55i_edp_bridge_atomic_disable(struct drm_bridge *bridge,
					     struct drm_atomic_state *state)
{
	/* TODO: video stream disable + link teardown. */
}

static const struct drm_bridge_funcs sun55i_edp_bridge_funcs = {
	.attach			= sun55i_edp_bridge_attach,
	.atomic_enable		= sun55i_edp_bridge_atomic_enable,
	.atomic_disable		= sun55i_edp_bridge_atomic_disable,
	.atomic_duplicate_state	= drm_atomic_helper_bridge_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_bridge_destroy_state,
	.atomic_reset		= drm_atomic_helper_bridge_reset,
};

static int sun55i_edp_connector_get_modes(struct drm_connector *connector)
{
	/* TODO: read EDID via AUX once AUX transfer is implemented. */
	return 0;
}

static const struct drm_connector_helper_funcs sun55i_edp_connector_helper_funcs = {
	.get_modes = sun55i_edp_connector_get_modes,
};

static enum drm_connector_status
sun55i_edp_connector_detect(struct drm_connector *connector, bool force)
{
	struct sun55i_edp *edp = connector_to_sun55i_edp(connector);

	return edp->plugged ? connector_status_connected
			    : connector_status_disconnected;
}

static const struct drm_connector_funcs sun55i_edp_connector_funcs = {
	.fill_modes		= drm_helper_probe_single_connector_modes,
	.detect			= sun55i_edp_connector_detect,
	.destroy		= drm_connector_cleanup,
	.reset			= drm_atomic_helper_connector_reset,
	.atomic_duplicate_state	= drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_connector_destroy_state,
};

static int sun55i_edp_hw_enable(struct sun55i_edp *edp)
{
	int ret;

	ret = reset_control_deassert(edp->rst_bus);
	if (ret)
		return ret;

	ret = clk_prepare_enable(edp->clk_bus);
	if (ret)
		goto err_assert_reset;

	ret = clk_prepare_enable(edp->clk_mod);
	if (ret)
		goto err_disable_bus;

	if (edp->clk_24m) {
		ret = clk_prepare_enable(edp->clk_24m);
		if (ret)
			goto err_disable_mod;
	}

	return 0;

err_disable_mod:
	clk_disable_unprepare(edp->clk_mod);
err_disable_bus:
	clk_disable_unprepare(edp->clk_bus);
err_assert_reset:
	reset_control_assert(edp->rst_bus);
	return ret;
}

static void sun55i_edp_hw_disable(struct sun55i_edp *edp)
{
	if (edp->clk_24m)
		clk_disable_unprepare(edp->clk_24m);
	clk_disable_unprepare(edp->clk_mod);
	clk_disable_unprepare(edp->clk_bus);
	reset_control_assert(edp->rst_bus);
}

static void sun55i_edp_hpd_enable(struct sun55i_edp *edp)
{
	u32 val;

	val = readl(edp->regs + SUN55I_EDP_HPD_SCALE);
	writel(val | SUN55I_EDP_HPD_SCALE_EN,
	       edp->regs + SUN55I_EDP_HPD_SCALE);

	writel(SUN55I_EDP_HPD_INT_EN, edp->regs + SUN55I_EDP_HPD_INT);
	writel(SUN55I_EDP_HPD_EN_PLUG_IN | SUN55I_EDP_HPD_EN_PLUG_OUT,
	       edp->regs + SUN55I_EDP_HPD_EN);
}

static void sun55i_edp_hpd_disable(struct sun55i_edp *edp)
{
	writel(0, edp->regs + SUN55I_EDP_HPD_INT);
	writel(0, edp->regs + SUN55I_EDP_HPD_EN);
}

static irqreturn_t sun55i_edp_irq(int irq, void *data)
{
	struct sun55i_edp *edp = data;
	bool changed = false;
	u32 plug;

	plug = readl(edp->regs + SUN55I_EDP_HPD_PLUG);

	if (plug & SUN55I_EDP_HPD_PLUG_IN) {
		writel(SUN55I_EDP_HPD_PLUG_IN,
		       edp->regs + SUN55I_EDP_HPD_PLUG);
		edp->plugged = true;
		changed = true;
	}

	if (plug & SUN55I_EDP_HPD_PLUG_OUT) {
		writel(SUN55I_EDP_HPD_PLUG_OUT,
		       edp->regs + SUN55I_EDP_HPD_PLUG);
		edp->plugged = false;
		changed = true;
	}

	if (changed && edp->connector.dev)
		drm_helper_hpd_irq_event(edp->connector.dev);

	return changed ? IRQ_HANDLED : IRQ_NONE;
}

static int sun55i_edp_bind(struct device *dev, struct device *master,
			   void *data)
{
	struct sun55i_edp *edp = dev_get_drvdata(dev);
	struct drm_device *drm = data;
	int ret;

	ret = sun55i_edp_hw_enable(edp);
	if (ret)
		return ret;

	sun55i_edp_hpd_enable(edp);

	ret = devm_request_irq(dev, edp->irq, sun55i_edp_irq, 0,
			       dev_name(dev), edp);
	if (ret) {
		dev_err(dev, "Couldn't request IRQ\n");
		goto err_disable_hpd;
	}

	edp->plugged = !!(readl(edp->regs + SUN55I_EDP_HPD_PLUG) &
			  SUN55I_EDP_HPD_PLUG_IN);

	edp->aux.dev = dev;
	edp->aux.name = "sun55i-edp-aux";
	edp->aux.transfer = sun55i_edp_aux_transfer;
	ret = drm_dp_aux_register(&edp->aux);
	if (ret)
		goto err_disable_hpd;

	drm_simple_encoder_init(drm, &edp->encoder, DRM_MODE_ENCODER_TMDS);
	edp->encoder.possible_crtcs =
		drm_of_find_possible_crtcs(drm, dev->of_node);
	if (!edp->encoder.possible_crtcs) {
		ret = -EPROBE_DEFER;
		goto err_unregister_aux;
	}

	edp->bridge.funcs = &sun55i_edp_bridge_funcs;
	edp->bridge.of_node = dev->of_node;
	edp->bridge.type = edp->variant->connector_type;
	edp->bridge.ops = DRM_BRIDGE_OP_DETECT | DRM_BRIDGE_OP_HPD;

	ret = drm_bridge_attach(&edp->encoder, &edp->bridge, NULL, 0);
	if (ret)
		goto err_cleanup_encoder;

	drm_connector_helper_add(&edp->connector,
				 &sun55i_edp_connector_helper_funcs);
	ret = drm_connector_init(drm, &edp->connector,
				 &sun55i_edp_connector_funcs,
				 edp->variant->connector_type);
	if (ret)
		goto err_cleanup_encoder;

	drm_connector_attach_encoder(&edp->connector, &edp->encoder);

	return 0;

err_cleanup_encoder:
	drm_encoder_cleanup(&edp->encoder);
err_unregister_aux:
	drm_dp_aux_unregister(&edp->aux);
err_disable_hpd:
	sun55i_edp_hpd_disable(edp);
	sun55i_edp_hw_disable(edp);
	return ret;
}

static void sun55i_edp_unbind(struct device *dev, struct device *master,
			      void *data)
{
	struct sun55i_edp *edp = dev_get_drvdata(dev);

	drm_connector_cleanup(&edp->connector);
	drm_encoder_cleanup(&edp->encoder);
	drm_dp_aux_unregister(&edp->aux);
	sun55i_edp_hpd_disable(edp);
	sun55i_edp_hw_disable(edp);
}

static const struct component_ops sun55i_edp_ops = {
	.bind	= sun55i_edp_bind,
	.unbind	= sun55i_edp_unbind,
};

static int sun55i_edp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sun55i_edp *edp;

	edp = devm_kzalloc(dev, sizeof(*edp), GFP_KERNEL);
	if (!edp)
		return -ENOMEM;

	edp->dev = dev;
	edp->variant = of_device_get_match_data(dev);
	if (!edp->variant)
		return -EINVAL;

	edp->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(edp->regs))
		return PTR_ERR(edp->regs);

	edp->irq = platform_get_irq(pdev, 0);
	if (edp->irq < 0)
		return edp->irq;

	edp->clk_bus = devm_clk_get(dev, "bus");
	if (IS_ERR(edp->clk_bus))
		return dev_err_probe(dev, PTR_ERR(edp->clk_bus),
				     "missing bus clock\n");

	edp->clk_mod = devm_clk_get(dev, "mod");
	if (IS_ERR(edp->clk_mod))
		return dev_err_probe(dev, PTR_ERR(edp->clk_mod),
				     "missing mod clock\n");

	edp->clk_24m = devm_clk_get_optional(dev, "24m");
	if (IS_ERR(edp->clk_24m))
		return dev_err_probe(dev, PTR_ERR(edp->clk_24m),
				     "bad 24m clock\n");

	edp->rst_bus = devm_reset_control_get(dev, "bus");
	if (IS_ERR(edp->rst_bus))
		return dev_err_probe(dev, PTR_ERR(edp->rst_bus),
				     "missing bus reset\n");

	edp->vdd_supply = devm_regulator_get_optional(dev, "vdd");
	if (IS_ERR(edp->vdd_supply)) {
		if (PTR_ERR(edp->vdd_supply) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		edp->vdd_supply = NULL;
	}

	edp->vcc_supply = devm_regulator_get_optional(dev, "vcc");
	if (IS_ERR(edp->vcc_supply)) {
		if (PTR_ERR(edp->vcc_supply) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		edp->vcc_supply = NULL;
	}

	platform_set_drvdata(pdev, edp);

	return component_add(dev, &sun55i_edp_ops);
}

static void sun55i_edp_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &sun55i_edp_ops);
}

static const struct sun55i_edp_variant sun55i_a523_edp_variant = {
	.connector_type = DRM_MODE_CONNECTOR_eDP,
};

static const struct sun55i_edp_variant sun55i_a523_dp_variant = {
	.connector_type = DRM_MODE_CONNECTOR_DisplayPort,
};

static const struct of_device_id sun55i_edp_of_table[] = {
	{
		.compatible = "allwinner,sun55i-a523-edp",
		.data = &sun55i_a523_edp_variant,
	},
	{
		.compatible = "allwinner,sun55i-a523-dp",
		.data = &sun55i_a523_dp_variant,
	},
	{ }
};
MODULE_DEVICE_TABLE(of, sun55i_edp_of_table);

static struct platform_driver sun55i_edp_driver = {
	.probe	= sun55i_edp_probe,
	.remove	= sun55i_edp_remove,
	.driver	= {
		.name		= "sun55i-edp",
		.of_match_table	= sun55i_edp_of_table,
	},
};
module_platform_driver(sun55i_edp_driver);

MODULE_AUTHOR("Jernej Skrabec <jernej.skrabec@gmail.com>");
MODULE_DESCRIPTION("Allwinner A523 eDP/DP TX driver");
MODULE_LICENSE("GPL");
