// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for AC200 Ethernet PHY
 *
 * Copyright (c) 2019 Jernej Skrabec <jernej.skrabec@gmail.com>
 */

#include <linux/clk.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/phy.h>

#define AC200_EPHY_ID			0x00441400
#define AC200_EPHY_ID_MASK		0x0ffffff0
#define AC200_EPHY_PAGE_SELECT		0x1f

struct ac200_ephy_priv {
	struct clk *clk;
};

static int ac200_ephy_read_page(struct phy_device *phydev)
{
	return __phy_read(phydev, AC200_EPHY_PAGE_SELECT);
}

static int ac200_ephy_write_page(struct phy_device *phydev, int page)
{
	return __phy_write(phydev, AC200_EPHY_PAGE_SELECT, page);
}

static int ac200_ephy_config_init(struct phy_device *phydev)
{
	int ret;

	ret = phy_write_paged(phydev, 0x0100, 0x12, 0x4824); /* Disable APS */
	if (ret)
		return ret;

	/* PHYAFE TRX optimization */
	ret = phy_write_paged(phydev, 0x0200, 0x18, 0x0000);
	if (ret)
		return ret;

	ret = phy_write_paged(phydev, 0x0600, 0x14, 0x708f); /* TX optimization */
	if (ret)
		return ret;

	ret = phy_write_paged(phydev, 0x0600, 0x13, 0xf000); /* RX optimization */
	if (ret)
		return ret;

	ret = phy_write_paged(phydev, 0x0600, 0x15, 0x1530);
	if (ret)
		return ret;

	ret = phy_write_paged(phydev, 0x0800, 0x18, 0x00bc); /* TRX optimization */
	if (ret)
		return ret;

	/* Disable intelligent EEE. */
	ret = phy_modify_paged(phydev, 0x0100, 0x17, BIT(3), 0);
	if (ret)
		return ret;

	/* Disable 802.3az EEE. */
	ret = phy_clear_bits_mmd(phydev, MDIO_MMD_AN, MDIO_AN_EEE_ADV,
				 MDIO_EEE_100TX);
	if (ret)
		return ret;

	/*
	 * Undocumented, and only done by the vendor driver when the AC200 is
	 * paired with an H6 (CONFIG_ARCH_SUN50IW6). That is the only SoC this
	 * chip is used with, so apply it unconditionally.
	 */
	return phy_set_bits(phydev, 0x13, BIT(12));
}

static int ac200_ephy_probe(struct phy_device *phydev)
{
	struct device *dev = &phydev->mdio.dev;
	struct ac200_ephy_priv *priv;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->clk = devm_clk_get_optional_enabled(dev, NULL);
	if (IS_ERR(priv->clk))
		return dev_err_probe(dev, PTR_ERR(priv->clk),
				     "Failed to request clock\n");

	phydev->priv = priv;

	return 0;
}

static int ac200_ephy_suspend(struct phy_device *phydev)
{
	struct ac200_ephy_priv *priv = phydev->priv;
	int ret;

	ret = genphy_suspend(phydev);
	if (ret)
		return ret;

	clk_disable_unprepare(priv->clk);

	return 0;
}

static int ac200_ephy_resume(struct phy_device *phydev)
{
	struct ac200_ephy_priv *priv = phydev->priv;
	int ret;

	ret = clk_prepare_enable(priv->clk);
	if (ret)
		return ret;

	ret = genphy_resume(phydev);
	if (ret)
		clk_disable_unprepare(priv->clk);

	return ret;
}

static struct phy_driver ac200_ephy_driver[] = {
	{
		.phy_id		= AC200_EPHY_ID,
		.phy_id_mask	= AC200_EPHY_ID_MASK,
		.name		= "X-Powers AC200 EPHY",
		.soft_reset	= genphy_soft_reset,
		.config_init	= ac200_ephy_config_init,
		.probe		= ac200_ephy_probe,
		.read_page	= ac200_ephy_read_page,
		.write_page	= ac200_ephy_write_page,
		.suspend	= ac200_ephy_suspend,
		.resume		= ac200_ephy_resume,
	}
};
module_phy_driver(ac200_ephy_driver);

MODULE_AUTHOR("Jernej Skrabec <jernej.skrabec@gmail.com>");
MODULE_DESCRIPTION("AC200 Ethernet PHY driver");
MODULE_LICENSE("GPL");

static const struct mdio_device_id __maybe_unused ac200_ephy_phy_tbl[] = {
	{ AC200_EPHY_ID, AC200_EPHY_ID_MASK },
	{ }
};
MODULE_DEVICE_TABLE(mdio, ac200_ephy_phy_tbl);
