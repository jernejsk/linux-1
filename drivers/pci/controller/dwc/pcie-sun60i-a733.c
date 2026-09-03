// SPDX-License-Identifier: GPL-2.0-only
/*
 * PCIe host controller driver for the Allwinner A733
 *
 * Copyright (C) 2026 Jernej Skrabec <jernej.skrabec@gmail.com>
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/string.h>

#include "pcie-designware.h"

/* Application registers */
#define PCIE_LTSSM_ENABLE		0xc00
#define   LTSSM_ENABLE			BIT(0)
#define PCIE_SII_INT_MASK0		0xe00
#define PCIE_SII_INT0			0xe08
#define   SII_INTX_ASSERTED(x)		BIT(5 + (x))
#define   SII_INTX_ASSERTED_MASK	GENMASK(8, 5)
#define   SII_INTX_DEASSERTED_MASK	GENMASK(12, 9)
#define PCIE_SII_INT1			0xe0c
#define   SII_RDLH_LINK_UP		BIT(1)
#define   SII_SMLH_LINK_UP		BIT(0)

struct sun60i_pcie {
	struct dw_pcie pci;
	void __iomem *app;
	struct clk_bulk_data *clks;
	int num_clks;
	struct reset_control *rsts;
	struct phy *phy;
	struct gpio_desc *reset_gpio;
	struct regulator *vpcie3v3;
	struct irq_domain *intx_domain;
	raw_spinlock_t intx_lock;
};

#define to_sun60i_pcie(x) dev_get_drvdata((x)->dev)

static bool sun60i_pcie_link_up(struct dw_pcie *pci)
{
	struct sun60i_pcie *pcie = to_sun60i_pcie(pci);
	u32 val = readl(pcie->app + PCIE_SII_INT1);

	return (val & (SII_RDLH_LINK_UP | SII_SMLH_LINK_UP)) ==
	       (SII_RDLH_LINK_UP | SII_SMLH_LINK_UP);
}

static int sun60i_pcie_start_link(struct dw_pcie *pci)
{
	struct sun60i_pcie *pcie = to_sun60i_pcie(pci);

	writel(readl(pcie->app + PCIE_LTSSM_ENABLE) | LTSSM_ENABLE,
	       pcie->app + PCIE_LTSSM_ENABLE);

	return 0;
}

static void sun60i_pcie_stop_link(struct dw_pcie *pci)
{
	struct sun60i_pcie *pcie = to_sun60i_pcie(pci);

	writel(readl(pcie->app + PCIE_LTSSM_ENABLE) & ~LTSSM_ENABLE,
	       pcie->app + PCIE_LTSSM_ENABLE);
}

static const struct dw_pcie_ops sun60i_pcie_ops = {
	.link_up	= sun60i_pcie_link_up,
	.start_link	= sun60i_pcie_start_link,
	.stop_link	= sun60i_pcie_stop_link,
};

static void sun60i_pcie_intx_handler(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct sun60i_pcie *pcie = irq_desc_get_handler_data(desc);
	unsigned long status, hwirq;

	chained_irq_enter(chip, desc);

	status = readl(pcie->app + PCIE_SII_INT0);
	/* The status bits are write one to clear */
	writel(status, pcie->app + PCIE_SII_INT0);

	status = (status & SII_INTX_ASSERTED_MASK) >> 5;
	for_each_set_bit(hwirq, &status, PCI_NUM_INTX)
		generic_handle_domain_irq(pcie->intx_domain, hwirq);

	chained_irq_exit(chip, desc);
}

static void sun60i_pcie_intx_set_mask(struct irq_data *data, bool mask)
{
	struct sun60i_pcie *pcie = irq_data_get_irq_chip_data(data);
	unsigned long flags;
	u32 val;

	/* A set bit lets the interrupt through */
	raw_spin_lock_irqsave(&pcie->intx_lock, flags);
	val = readl(pcie->app + PCIE_SII_INT_MASK0);
	if (mask)
		val &= ~SII_INTX_ASSERTED(data->hwirq);
	else
		val |= SII_INTX_ASSERTED(data->hwirq);
	writel(val, pcie->app + PCIE_SII_INT_MASK0);
	raw_spin_unlock_irqrestore(&pcie->intx_lock, flags);
}

static void sun60i_pcie_intx_mask(struct irq_data *data)
{
	sun60i_pcie_intx_set_mask(data, true);
}

static void sun60i_pcie_intx_unmask(struct irq_data *data)
{
	sun60i_pcie_intx_set_mask(data, false);
}

static struct irq_chip sun60i_pcie_intx_chip = {
	.name		= "INTx",
	.irq_mask	= sun60i_pcie_intx_mask,
	.irq_unmask	= sun60i_pcie_intx_unmask,
	.flags		= IRQCHIP_SKIP_SET_WAKE | IRQCHIP_MASK_ON_SUSPEND,
};

static int sun60i_pcie_intx_map(struct irq_domain *domain, unsigned int irq,
				irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &sun60i_pcie_intx_chip, handle_level_irq);
	irq_set_chip_data(irq, domain->host_data);

	return 0;
}

static const struct irq_domain_ops sun60i_pcie_intx_domain_ops = {
	.map = sun60i_pcie_intx_map,
};

static int sun60i_pcie_init_intx(struct sun60i_pcie *pcie)
{
	struct device *dev = pcie->pci.dev;
	struct device_node *intc;
	int irq;

	raw_spin_lock_init(&pcie->intx_lock);

	intc = of_get_child_by_name(dev->of_node, "legacy-interrupt-controller");
	if (!intc)
		return dev_err_probe(dev, -EINVAL, "missing the INTx controller node\n");

	irq = of_irq_get(intc, 0);
	if (irq <= 0) {
		of_node_put(intc);
		return dev_err_probe(dev, irq ?: -EINVAL, "failed to get the INTx interrupt\n");
	}

	pcie->intx_domain = irq_domain_create_linear(of_fwnode_handle(intc),
						     PCI_NUM_INTX,
						     &sun60i_pcie_intx_domain_ops,
						     pcie);
	of_node_put(intc);
	if (!pcie->intx_domain)
		return dev_err_probe(dev, -ENOMEM, "failed to create the INTx domain\n");

	irq_set_chained_handler_and_data(irq, sun60i_pcie_intx_handler, pcie);

	return 0;
}

static int sun60i_pcie_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct sun60i_pcie *pcie = to_sun60i_pcie(pci);

	int ret;

	/*
	 * The slot is powered with PERST# asserted, and the endpoint gets the
	 * 100 ms the specification grants it before the reset is released.
	 */
	gpiod_set_value_cansleep(pcie->reset_gpio, 1);

	if (pcie->vpcie3v3) {
		ret = regulator_enable(pcie->vpcie3v3);
		if (ret)
			return dev_err_probe(pci->dev, ret,
					     "failed to enable the slot supply\n");
	}

	msleep(100);
	gpiod_set_value_cansleep(pcie->reset_gpio, 0);

	return 0;
}

static void sun60i_pcie_host_deinit(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct sun60i_pcie *pcie = to_sun60i_pcie(pci);

	gpiod_set_value_cansleep(pcie->reset_gpio, 1);

	if (pcie->vpcie3v3)
		regulator_disable(pcie->vpcie3v3);
}

static const struct dw_pcie_host_ops sun60i_pcie_host_ops = {
	.init = sun60i_pcie_host_init,
	.deinit = sun60i_pcie_host_deinit,
};

static int sun60i_pcie_power_on(struct sun60i_pcie *pcie)
{
	struct device *dev = pcie->pci.dev;
	int ret;

	/* The AXI slave port is specified for 400 MHz; the CCU boots it faster */
	for (int i = 0; i < pcie->num_clks; i++) {
		if (strcmp(pcie->clks[i].id, "slv"))
			continue;

		ret = clk_set_rate(pcie->clks[i].clk, 400000000);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to set the slave clock\n");
	}

	ret = clk_bulk_prepare_enable(pcie->num_clks, pcie->clks);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable the clocks\n");

	ret = reset_control_deassert(pcie->rsts);
	if (ret) {
		dev_err_probe(dev, ret, "failed to deassert the resets\n");
		goto err_disable_clks;
	}

	ret = phy_init(pcie->phy);
	if (ret) {
		dev_err_probe(dev, ret, "failed to initialize the PHY\n");
		goto err_assert_rsts;
	}

	ret = phy_power_on(pcie->phy);
	if (ret) {
		dev_err_probe(dev, ret, "failed to power on the PHY\n");
		goto err_exit_phy;
	}

	return 0;

err_exit_phy:
	phy_exit(pcie->phy);
err_assert_rsts:
	reset_control_assert(pcie->rsts);
err_disable_clks:
	clk_bulk_disable_unprepare(pcie->num_clks, pcie->clks);
	return ret;
}

static void sun60i_pcie_power_off(struct sun60i_pcie *pcie)
{
	phy_power_off(pcie->phy);
	phy_exit(pcie->phy);
	reset_control_assert(pcie->rsts);
	clk_bulk_disable_unprepare(pcie->num_clks, pcie->clks);
}

static int sun60i_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sun60i_pcie *pcie;
	struct dw_pcie_rp *pp;
	int ret;

	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	if (!pcie)
		return -ENOMEM;

	pcie->pci.dev = dev;
	pcie->pci.ops = &sun60i_pcie_ops;
	pp = &pcie->pci.pp;
	pp->ops = &sun60i_pcie_host_ops;
	platform_set_drvdata(pdev, pcie);

	pcie->app = devm_platform_ioremap_resource_byname(pdev, "app");
	if (IS_ERR(pcie->app))
		return PTR_ERR(pcie->app);

	pcie->num_clks = devm_clk_bulk_get_all(dev, &pcie->clks);
	if (pcie->num_clks < 0)
		return dev_err_probe(dev, pcie->num_clks, "failed to get the clocks\n");

	pcie->rsts = devm_reset_control_array_get_exclusive(dev);
	if (IS_ERR(pcie->rsts))
		return dev_err_probe(dev, PTR_ERR(pcie->rsts), "failed to get the resets\n");

	pcie->phy = devm_phy_get(dev, "pcie-phy");
	if (IS_ERR(pcie->phy))
		return dev_err_probe(dev, PTR_ERR(pcie->phy), "failed to get the PHY\n");

	pcie->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(pcie->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(pcie->reset_gpio),
				     "failed to get the PERST# GPIO\n");

	pcie->vpcie3v3 = devm_regulator_get_optional(dev, "vpcie3v3");
	if (IS_ERR(pcie->vpcie3v3)) {
		if (PTR_ERR(pcie->vpcie3v3) != -ENODEV)
			return dev_err_probe(dev, PTR_ERR(pcie->vpcie3v3),
					     "failed to get the slot supply\n");
		pcie->vpcie3v3 = NULL;
	}

	pm_runtime_enable(dev);
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		goto err_disable_pm;

	ret = sun60i_pcie_power_on(pcie);
	if (ret)
		goto err_put_pm;

	/* Nothing but the INTx assertions may raise the SII interrupt */
	writel(0, pcie->app + PCIE_SII_INT_MASK0);
	writel(~0, pcie->app + PCIE_SII_INT0);

	ret = sun60i_pcie_init_intx(pcie);
	if (ret)
		goto err_power_off;

	ret = dw_pcie_host_init(pp);
	if (ret) {
		dev_err_probe(dev, ret, "failed to initialize the host\n");
		goto err_power_off;
	}

	return 0;

err_power_off:
	sun60i_pcie_power_off(pcie);
err_put_pm:
	pm_runtime_put(dev);
err_disable_pm:
	pm_runtime_disable(dev);
	return ret;
}

static void sun60i_pcie_remove(struct platform_device *pdev)
{
	struct sun60i_pcie *pcie = platform_get_drvdata(pdev);

	dw_pcie_host_deinit(&pcie->pci.pp);
	irq_domain_remove(pcie->intx_domain);
	sun60i_pcie_power_off(pcie);
	pm_runtime_put(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
}

static const struct of_device_id sun60i_pcie_of_match[] = {
	{ .compatible = "allwinner,sun60i-a733-pcie" },
	{ }
};
MODULE_DEVICE_TABLE(of, sun60i_pcie_of_match);

static struct platform_driver sun60i_pcie_driver = {
	.probe	= sun60i_pcie_probe,
	.remove	= sun60i_pcie_remove,
	.driver	= {
		.name			= "sun60i-a733-pcie",
		.of_match_table		= sun60i_pcie_of_match,
		.suppress_bind_attrs	= true,
	},
};
module_platform_driver(sun60i_pcie_driver);

MODULE_DESCRIPTION("Allwinner A733 PCIe host controller driver");
MODULE_AUTHOR("Jernej Skrabec <jernej.skrabec@gmail.com>");
MODULE_LICENSE("GPL");
