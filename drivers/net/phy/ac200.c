// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for AC200 Ethernet PHY
 *
 * Copyright (c) 2019 Jernej Skrabec <jernej.skrabec@gmail.com>
 */

#include <linux/clk.h>
#include <linux/etherdevice.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/phy.h>

#define AC200_EPHY_ID			0x00441400
#define AC200_EPHY_ID_MASK		0x0ffffff0

/*
 * The register names come from the vendor driver for the Rockchip RK630,
 * which carries the same PHY, with the same ID and the same tuning values.
 */

/* page 0 registers */
#define AC200_EPHY_INT_STATUS		0x10
#define AC200_EPHY_INT_MASK		0x11
#define AC200_EPHY_INT_LINK_CHANGE	BIT(15)
#define AC200_EPHY_INT_WOL		BIT(14)
#define AC200_EPHY_GLOBAL_CTL		0x13
#define AC200_EPHY_GLOBAL_CTL_UNKNOWN	BIT(12)
#define AC200_EPHY_GLOBAL_CTL_WOL_EN	BIT(10)
#define AC200_EPHY_GLOBAL_CTL_WOL_HOLD	BIT(7)
#define AC200_EPHY_WOL_MAC(x)		(0x16 + (x))
#define AC200_EPHY_PAGE_SELECT		0x1f

/* the page number lives in the upper byte of the page select register */
#define AC200_EPHY_PAGE(x)		((x) << 8)

/* page 1 registers */
#define AC200_EPHY_P1_APS_CTL		0x12
#define AC200_EPHY_P1_EEE_CTL		0x17
#define AC200_EPHY_P1_EEE_INTELLIGENT	BIT(3)

/* page 2 registers */
#define AC200_EPHY_P2_AFE_CTL		0x18

/* page 6 registers */
#define AC200_EPHY_P6_AFE_RX_CTL	0x13
#define AC200_EPHY_P6_AFE_TX_CTL	0x14
#define AC200_EPHY_P6_AFE_DRIVER2	0x15

/* page 8 registers */
#define AC200_EPHY_P8_AFE_CTL		0x18

#define AC200_EPHY_INTS			(AC200_EPHY_INT_LINK_CHANGE | \
					 AC200_EPHY_INT_WOL)

struct ac200_ephy_priv {
	struct clk *clk;
	bool wol_irq_enabled;
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

	/* Disable APS */
	ret = phy_write_paged(phydev, AC200_EPHY_PAGE(1),
			      AC200_EPHY_P1_APS_CTL, 0x4824);
	if (ret)
		return ret;

	/* PHYAFE TRX optimization */
	ret = phy_write_paged(phydev, AC200_EPHY_PAGE(2),
			      AC200_EPHY_P2_AFE_CTL, 0x0000);
	if (ret)
		return ret;

	/* PHYAFE TX optimization */
	ret = phy_write_paged(phydev, AC200_EPHY_PAGE(6),
			      AC200_EPHY_P6_AFE_TX_CTL, 0x708f);
	if (ret)
		return ret;

	/* PHYAFE RX optimization */
	ret = phy_write_paged(phydev, AC200_EPHY_PAGE(6),
			      AC200_EPHY_P6_AFE_RX_CTL, 0xf000);
	if (ret)
		return ret;

	ret = phy_write_paged(phydev, AC200_EPHY_PAGE(6),
			      AC200_EPHY_P6_AFE_DRIVER2, 0x1530);
	if (ret)
		return ret;

	/* PHYAFE TRX optimization */
	ret = phy_write_paged(phydev, AC200_EPHY_PAGE(8),
			      AC200_EPHY_P8_AFE_CTL, 0x00bc);
	if (ret)
		return ret;

	/* Disable intelligent EEE. */
	ret = phy_modify_paged(phydev, AC200_EPHY_PAGE(1),
			       AC200_EPHY_P1_EEE_CTL,
			       AC200_EPHY_P1_EEE_INTELLIGENT, 0);
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
	return phy_set_bits(phydev, AC200_EPHY_GLOBAL_CTL,
			    AC200_EPHY_GLOBAL_CTL_UNKNOWN);
}

static int ac200_ephy_ack_interrupt(struct phy_device *phydev)
{
	int ret;

	/* The status bits are write-one-to-clear. */
	ret = phy_read(phydev, AC200_EPHY_INT_STATUS);
	if (ret < 0)
		return ret;

	return phy_write(phydev, AC200_EPHY_INT_STATUS, ret);
}

static int ac200_ephy_config_intr(struct phy_device *phydev)
{
	int ret;

	if (phydev->interrupts == PHY_INTERRUPT_ENABLED) {
		ret = ac200_ephy_ack_interrupt(phydev);
		if (ret)
			return ret;

		ret = phy_set_bits(phydev, AC200_EPHY_INT_MASK,
				   AC200_EPHY_INT_LINK_CHANGE);
	} else {
		ret = phy_clear_bits(phydev, AC200_EPHY_INT_MASK,
				     AC200_EPHY_INT_LINK_CHANGE);
		if (ret)
			return ret;

		ret = ac200_ephy_ack_interrupt(phydev);
	}

	return ret;
}

static irqreturn_t ac200_ephy_handle_interrupt(struct phy_device *phydev)
{
	int status, enabled;

	enabled = phy_read(phydev, AC200_EPHY_INT_MASK);
	if (enabled < 0) {
		phy_error(phydev);
		return IRQ_NONE;
	}

	status = phy_read(phydev, AC200_EPHY_INT_STATUS);
	if (status < 0) {
		phy_error(phydev);
		return IRQ_NONE;
	}

	if (!(status & enabled & AC200_EPHY_INTS))
		return IRQ_NONE;

	/* Clear every latched event, not just the ones that are unmasked. */
	if (phy_write(phydev, AC200_EPHY_INT_STATUS, status) < 0) {
		phy_error(phydev);
		return IRQ_NONE;
	}

	if (status & enabled & AC200_EPHY_INT_LINK_CHANGE)
		phy_trigger_machine(phydev);

	return IRQ_HANDLED;
}

/*
 * The interrupt is armed as a wakeup source right away instead of leaving that
 * to dev_pm_set_wake_irq(). The AC200 hangs off an I2C bus, and arming the
 * wakeup during the noirq phase would make regmap-irq talk to a controller
 * that has already been suspended.
 */
static int ac200_ephy_set_wake_irq(struct phy_device *phydev, bool enable)
{
	struct ac200_ephy_priv *priv = phydev->priv;
	int ret;

	if (!device_can_wakeup(&phydev->mdio.dev) ||
	    priv->wol_irq_enabled == enable)
		return 0;

	if (enable)
		ret = enable_irq_wake(phydev->irq);
	else
		ret = disable_irq_wake(phydev->irq);
	if (ret)
		return ret;

	priv->wol_irq_enabled = enable;

	return 0;
}

static void ac200_ephy_get_wol(struct phy_device *phydev,
			       struct ethtool_wolinfo *wol)
{
	int ret;

	wol->supported = WAKE_MAGIC;
	wol->wolopts = 0;

	ret = phy_read(phydev, AC200_EPHY_GLOBAL_CTL);
	if (ret >= 0 && (ret & AC200_EPHY_GLOBAL_CTL_WOL_EN))
		wol->wolopts = WAKE_MAGIC;
}

static int ac200_ephy_set_wol(struct phy_device *phydev,
			      struct ethtool_wolinfo *wol)
{
	struct net_device *ndev = phydev->attached_dev;
	const u8 *mac;
	int ret, i;

	if (wol->wolopts & ~WAKE_MAGIC)
		return -EOPNOTSUPP;

	if (!(wol->wolopts & WAKE_MAGIC)) {
		ret = phy_clear_bits(phydev, AC200_EPHY_INT_MASK,
				     AC200_EPHY_INT_WOL);
		if (ret)
			return ret;

		ret = phy_clear_bits(phydev, AC200_EPHY_GLOBAL_CTL,
				     AC200_EPHY_GLOBAL_CTL_WOL_EN);
		if (ret)
			return ret;

		ret = ac200_ephy_set_wake_irq(phydev, false);
		if (ret)
			return ret;

		return device_set_wakeup_enable(&phydev->mdio.dev, false);
	}

	if (!ndev)
		return -ENODEV;

	/*
	 * The magic packet detector has its own copy of the MAC address,
	 * stored big endian across three registers.
	 */
	mac = ndev->dev_addr;
	for (i = 0; i < 3; i++) {
		ret = phy_write(phydev, AC200_EPHY_WOL_MAC(i),
				mac[2 * i] << 8 | mac[2 * i + 1]);
		if (ret)
			return ret;
	}

	ret = ac200_ephy_ack_interrupt(phydev);
	if (ret)
		return ret;

	ret = phy_set_bits(phydev, AC200_EPHY_INT_MASK, AC200_EPHY_INT_WOL);
	if (ret)
		return ret;

	ret = phy_modify(phydev, AC200_EPHY_GLOBAL_CTL,
			 AC200_EPHY_GLOBAL_CTL_WOL_HOLD,
			 AC200_EPHY_GLOBAL_CTL_WOL_EN);
	if (ret)
		return ret;

	ret = ac200_ephy_set_wake_irq(phydev, true);
	if (ret)
		return ret;

	return device_set_wakeup_enable(&phydev->mdio.dev, true);
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

	/*
	 * The magic packet detector can raise the PHY interrupt, which on this
	 * chip is routed through the AC200 interrupt controller. Only offer
	 * Wake-on-LAN if the board actually wired that up and marked the PHY as
	 * a wakeup source.
	 */
	if (of_property_read_bool(dev->of_node, "wakeup-source") &&
	    phy_interrupt_is_valid(phydev))
		device_set_wakeup_capable(dev, true);

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
		.config_intr	= ac200_ephy_config_intr,
		.handle_interrupt = ac200_ephy_handle_interrupt,
		.get_wol	= ac200_ephy_get_wol,
		.set_wol	= ac200_ephy_set_wol,
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
