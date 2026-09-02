// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Jernej Skrabec <jernej.skrabec@gmail.com>
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/reset.h>

#include <dt-bindings/clock/sun60i-a733-serdes-ccu.h>
#include <dt-bindings/reset/sun60i-a733-serdes-ccu.h>

#include "ccu_common.h"
#include "ccu_reset.h"

#include "ccu_gate.h"
#include "ccu_mux.h"

#define SERDES_USB3_BGR_REG	0x08

static const struct clk_parent_data ahb_parent[] = {
	{ .fw_name = "ahb" },
};

static const struct clk_parent_data mbus_parent[] = {
	{ .fw_name = "mbus" },
};

static SUNXI_CCU_GATE_DATA(usb3_hclk_clk, "usb3-hclk", ahb_parent,
			   SERDES_USB3_BGR_REG, BIT(16), 0);
static SUNXI_CCU_GATE_DATA(usb3_aclk_clk, "usb3-aclk", mbus_parent,
			   SERDES_USB3_BGR_REG, BIT(17), 0);

/*
 * The USB3 controller takes its PIPE clock either from the combo PHY or,
 * when only the USB2 PHY is used, from the CCU's USB2_U2_PIPE clock.
 */
static const struct clk_parent_data usb3_pipe_parents[] = {
	{ .fw_name = "pipe" },
	{ .fw_name = "u2-pipe" },
};

static SUNXI_CCU_MUX_DATA(usb3_pipe_clk, "usb3-pipe", usb3_pipe_parents,
			  SERDES_USB3_BGR_REG, 20, 1, CLK_SET_RATE_NO_REPARENT);

static struct ccu_common *sun60i_a733_serdes_ccu_clks[] = {
	&usb3_hclk_clk.common,
	&usb3_aclk_clk.common,
	&usb3_pipe_clk.common,
};

static struct clk_hw_onecell_data sun60i_a733_serdes_hw_clks = {
	.hws	= {
		[CLK_SERDES_USB3_HCLK]	= &usb3_hclk_clk.common.hw,
		[CLK_SERDES_USB3_ACLK]	= &usb3_aclk_clk.common.hw,
		[CLK_SERDES_USB3_PIPE]	= &usb3_pipe_clk.common.hw,
	},
	.num = CLK_SERDES_USB3_PIPE + 1,
};

static struct ccu_reset_map sun60i_a733_serdes_ccu_resets[] = {
	[RST_SERDES_USB3_USB2_PHY]	= { SERDES_USB3_BGR_REG, BIT(4) },
};

static const struct sunxi_ccu_desc sun60i_a733_serdes_ccu_desc = {
	.ccu_clks	= sun60i_a733_serdes_ccu_clks,
	.num_ccu_clks	= ARRAY_SIZE(sun60i_a733_serdes_ccu_clks),

	.hw_clks	= &sun60i_a733_serdes_hw_clks,

	.resets		= sun60i_a733_serdes_ccu_resets,
	.num_resets	= ARRAY_SIZE(sun60i_a733_serdes_ccu_resets),
};

static int sun60i_a733_serdes_ccu_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct reset_control *rst;
	struct clk_bulk_data *clks;
	void __iomem *reg;
	int ret, num;

	reg = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(reg))
		return PTR_ERR(reg);

	num = devm_clk_bulk_get_all_enabled(dev, &clks);
	if (num < 0)
		return dev_err_probe(dev, num, "failed to enable the bus clocks\n");

	rst = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(rst))
		return dev_err_probe(dev, PTR_ERR(rst), "failed to get the reset\n");

	ret = reset_control_deassert(rst);
	if (ret)
		return dev_err_probe(dev, ret, "failed to deassert the reset\n");

	return devm_sunxi_ccu_probe(dev, reg, &sun60i_a733_serdes_ccu_desc);
}

static const struct of_device_id sun60i_a733_serdes_ccu_ids[] = {
	{ .compatible = "allwinner,sun60i-a733-serdes-ccu" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, sun60i_a733_serdes_ccu_ids);

static struct platform_driver sun60i_a733_serdes_ccu_driver = {
	.probe	= sun60i_a733_serdes_ccu_probe,
	.driver	= {
		.name			= "sun60i-a733-serdes-ccu",
		.suppress_bind_attrs	= true,
		.of_match_table		= sun60i_a733_serdes_ccu_ids,
	},
};
module_platform_driver(sun60i_a733_serdes_ccu_driver);

MODULE_IMPORT_NS("SUNXI_CCU");
MODULE_DESCRIPTION("Support for the Allwinner A733 SerDes subsystem CCU");
MODULE_LICENSE("GPL");
