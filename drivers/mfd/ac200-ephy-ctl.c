// SPDX-License-Identifier: GPL-2.0-only
/*
 * System control driver for the AC200 Ethernet PHY
 * Copyright (c) 2022 Arm Ltd.
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset-controller.h>

/* macros for system ephy control 0 register */
#define AC200_SYS_EPHY_CTL0		0x0014
#define AC200_EPHY_RESET_INVALID	BIT(0)
#define AC200_EPHY_SYSCLK_GATING	BIT(1)

/* macros for system ephy control 1 register */
#define AC200_SYS_EPHY_CTL1		0x0016
#define AC200_EPHY_E_EPHY_MII_IO_EN	BIT(0)
#define AC200_EPHY_E_LNK_LED_IO_EN	BIT(1)
#define AC200_EPHY_E_SPD_LED_IO_EN	BIT(2)
#define AC200_EPHY_E_DPX_LED_IO_EN	BIT(3)

/* on-chip eFuse copy of the PHY calibration data */
#define AC200_EFUSE_EPHY		0x8004

/* macros for ephy control register */
#define AC200_EPHY_CTL			0x6000
#define AC200_EPHY_SHUTDOWN		BIT(0)
#define AC200_EPHY_LED_POL		BIT(1)
#define AC200_EPHY_CLK_SEL		BIT(2)
#define AC200_EPHY_ADDR(x)		(((x) & 0x1F) << 4)
#define AC200_EPHY_XMII_SEL		BIT(11)
#define AC200_EPHY_CALIB(x)		(((x) & 0xF) << 12)

struct ac200_ephy_ctl_dev {
	struct reset_controller_dev	rcdev;
	struct clk_hw			gate_clk;
	struct regmap			*regmap;
	u16				ephy_ctl;
};

static struct ac200_ephy_ctl_dev *to_phy_dev(struct reset_controller_dev *rcdev)
{
	return container_of(rcdev, struct ac200_ephy_ctl_dev, rcdev);
}

static struct ac200_ephy_ctl_dev *to_clk_dev(struct clk_hw *hw)
{
	return container_of(hw, struct ac200_ephy_ctl_dev, gate_clk);
}

static int ac200_ephy_clk_prepare(struct clk_hw *hw)
{
	struct ac200_ephy_ctl_dev *ac200 = to_clk_dev(hw);

	return regmap_set_bits(ac200->regmap, AC200_SYS_EPHY_CTL0,
			       AC200_EPHY_SYSCLK_GATING);
}

static void ac200_ephy_clk_unprepare(struct clk_hw *hw)
{
	struct ac200_ephy_ctl_dev *ac200 = to_clk_dev(hw);

	regmap_clear_bits(ac200->regmap, AC200_SYS_EPHY_CTL0,
			  AC200_EPHY_SYSCLK_GATING);
}

static int ac200_ephy_clk_is_prepared(struct clk_hw *hw)
{
	struct ac200_ephy_ctl_dev *ac200 = to_clk_dev(hw);

	return regmap_test_bits(ac200->regmap, AC200_SYS_EPHY_CTL0,
				AC200_EPHY_SYSCLK_GATING);
}

static const struct clk_ops ac200_ephy_clk_ops = {
	.prepare = ac200_ephy_clk_prepare,
	.unprepare = ac200_ephy_clk_unprepare,
	.is_prepared = ac200_ephy_clk_is_prepared,
};

static int ephy_ctl_assert(struct reset_controller_dev *rcdev, unsigned long id)
{
	struct ac200_ephy_ctl_dev *ac200 = to_phy_dev(rcdev);

	return regmap_clear_bits(ac200->regmap, AC200_SYS_EPHY_CTL0,
				 AC200_EPHY_RESET_INVALID);
}

static int ephy_ctl_deassert(struct reset_controller_dev *rcdev,
			     unsigned long id)
{
	struct ac200_ephy_ctl_dev *ac200 = to_phy_dev(rcdev);
	int ret;

	ret = regmap_set_bits(ac200->regmap, AC200_SYS_EPHY_CTL0,
			      AC200_EPHY_RESET_INVALID);
	if (ret)
		return ret;

	/*
	 * EPHY_CTL lives inside the block this reset covers, so re-apply the
	 * calibration, PHY address and interface selection every time the
	 * reset is released. The vendor code programs it after releasing the
	 * reset for the same reason.
	 */
	return regmap_write(ac200->regmap, AC200_EPHY_CTL, ac200->ephy_ctl);
}

static int ephy_ctl_reset(struct reset_controller_dev *rcdev, unsigned long id)
{
	int ret;

	ret = ephy_ctl_assert(rcdev, id);
	if (ret)
		return ret;

	/*
	 * Going over I2C already takes a while, but do not rely on the bus
	 * speed for the reset pulse width.
	 */
	usleep_range(100, 200);

	return ephy_ctl_deassert(rcdev, id);
}

static int ephy_ctl_status(struct reset_controller_dev *rcdev, unsigned long id)
{
	struct ac200_ephy_ctl_dev *ac200 = to_phy_dev(rcdev);
	int ret;

	ret = regmap_test_bits(ac200->regmap, AC200_SYS_EPHY_CTL0,
			       AC200_EPHY_RESET_INVALID);

	return ret < 0 ? ret : !ret;
}

static int ephy_ctl_reset_of_xlate(struct reset_controller_dev *rcdev,
				   const struct of_phandle_args *reset_spec)
{
	if (WARN_ON(reset_spec->args_count != 0))
		return -EINVAL;

	return 0;
}

static const struct reset_control_ops ephy_ctl_reset_ops = {
	.assert		= ephy_ctl_assert,
	.deassert	= ephy_ctl_deassert,
	.reset		= ephy_ctl_reset,
	.status		= ephy_ctl_status,
};

static void ac200_ephy_ctl_disable(void *data)
{
	struct ac200_ephy_ctl_dev *priv = data;

	regmap_write(priv->regmap, AC200_EPHY_CTL, AC200_EPHY_SHUTDOWN);
	regmap_write(priv->regmap, AC200_SYS_EPHY_CTL1, 0);
	regmap_write(priv->regmap, AC200_SYS_EPHY_CTL0, 0);
}

static int ac200_ephy_ctl_probe(struct platform_device *pdev)
{
	struct reset_controller_dev *rcdev;
	struct device *dev = &pdev->dev;
	struct ac200_ephy_ctl_dev *priv;
	struct clk_init_data clk_init = {
		.ops = &ac200_ephy_clk_ops,
	};
	phy_interface_t phy_if;
	u16 caldata, ephy_ctl, ephy_ctl1;
	unsigned long rate;
	struct clk *clk;
	u32 value;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->regmap = dev_get_regmap(dev->parent, NULL);
	if (!priv->regmap)
		return dev_err_probe(dev, -ENODEV,
				     "Parent has no regmap\n");

	clk_init.name = devm_kasprintf(dev, GFP_KERNEL, "%s-gate",
				       dev_name(dev));
	if (!clk_init.name)
		return -ENOMEM;

	ret = nvmem_cell_read_u16(dev, "calibration", &caldata);
	if (ret == -ENOENT) {
		unsigned int val;

		/*
		 * Not every platform keeps the PHY trim in the SoC eFuse. The
		 * AC200 has its own copy, which is what the vendor driver
		 * falls back to. A chip with an unprogrammed eFuse reads 0,
		 * which just means "no trim" and still links.
		 */
		ret = regmap_read(priv->regmap, AC200_EFUSE_EPHY, &val);
		if (ret)
			return dev_err_probe(dev, ret,
					     "Unable to read eFuse\n");

		caldata = val;
		dev_dbg(dev, "Using on-chip PHY calibration data %u\n", caldata);
	} else if (ret) {
		return dev_err_probe(dev, ret,
				     "Unable to read calibration data\n");
	}

	ephy_ctl = AC200_EPHY_CALIB(caldata + 3);

	ret = of_get_phy_mode(dev->of_node, &phy_if);
	if (ret) {
		dev_err(dev, "Unable to read PHY connection mode\n");
		return ret;
	}

	switch (phy_if) {
	case PHY_INTERFACE_MODE_MII:
		break;
	case PHY_INTERFACE_MODE_RMII:
		ephy_ctl |= AC200_EPHY_XMII_SEL;
		break;
	default:
		dev_err(dev, "Illegal PHY connection mode (%d), only RMII or MII supported\n",
			phy_if);
		return -EINVAL;
	}

	if (of_property_read_bool(dev->of_node, "x-powers,led-active-low"))
		ephy_ctl |= AC200_EPHY_LED_POL;

	ret = of_property_read_u32(dev->of_node, "x-powers,phy-address", &value);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Unable to read PHY address value\n");
	if (value > 0x1f)
		return dev_err_probe(dev, -EINVAL,
				     "Invalid PHY address %u\n", value);

	ephy_ctl |= AC200_EPHY_ADDR(value);

	clk = clk_get(dev->parent, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk),
				     "Unable to obtain the clock\n");

	rate = clk_get_rate(clk);
	clk_put(clk);

	/* CLK_SEL only distinguishes between a 24MHz and a 27MHz reference. */
	switch (rate) {
	case 24000000:
		ephy_ctl |= AC200_EPHY_CLK_SEL;
		break;
	case 27000000:
		break;
	default:
		return dev_err_probe(dev, -EINVAL,
				     "Unsupported clock rate %lu\n", rate);
	}

	/* Assert reset and gate clock, to disable PHY for now */
	ret = regmap_write(priv->regmap, AC200_SYS_EPHY_CTL0, 0);
	if (ret)
		return ret;

	/*
	 * The LED pins are shared with other functions on some boards - the
	 * H616 vendor code has to leave the link LED off because it collides
	 * with TWI2 - so each one is enabled only when the board asks for it.
	 */
	ephy_ctl1 = AC200_EPHY_E_EPHY_MII_IO_EN;
	if (of_property_read_bool(dev->of_node, "x-powers,link-led"))
		ephy_ctl1 |= AC200_EPHY_E_LNK_LED_IO_EN;
	if (of_property_read_bool(dev->of_node, "x-powers,speed-led"))
		ephy_ctl1 |= AC200_EPHY_E_SPD_LED_IO_EN;
	if (of_property_read_bool(dev->of_node, "x-powers,duplex-led"))
		ephy_ctl1 |= AC200_EPHY_E_DPX_LED_IO_EN;

	ret = regmap_write(priv->regmap, AC200_SYS_EPHY_CTL1, ephy_ctl1);
	if (ret)
		return ret;

	priv->ephy_ctl = ephy_ctl;

	ret = regmap_write(priv->regmap, AC200_EPHY_CTL, ephy_ctl);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(dev, ac200_ephy_ctl_disable, priv);
	if (ret)
		return ret;

	rcdev = &priv->rcdev;
	rcdev->owner = dev->driver->owner;
	rcdev->nr_resets = 1;
	rcdev->ops = &ephy_ctl_reset_ops;
	rcdev->of_node = dev->of_node;
	rcdev->of_reset_n_cells = 0;
	rcdev->of_xlate = ephy_ctl_reset_of_xlate;

	ret = devm_reset_controller_register(dev, rcdev);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Unable to register reset controller\n");

	priv->gate_clk.init = &clk_init;
	ret = devm_clk_hw_register(dev, &priv->gate_clk);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Unable to register gate clock\n");

	ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_simple_get,
					  &priv->gate_clk);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Unable to register clock provider\n");

	return 0;
}

static const struct of_device_id ac200_ephy_ctl_match[] = {
	{ .compatible = "x-powers,ac200-ephy-ctl" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ac200_ephy_ctl_match);

static struct platform_driver ac200_ephy_ctl_driver = {
	.probe		= ac200_ephy_ctl_probe,
	.driver		= {
		.name		= "ac200-ephy-ctl",
		.of_match_table	= ac200_ephy_ctl_match,
	},
};
module_platform_driver(ac200_ephy_ctl_driver);

MODULE_AUTHOR("Andre Przywara <andre.przywara@arm.com>");
MODULE_DESCRIPTION("AC200 Ethernet PHY control driver");
MODULE_LICENSE("GPL");
