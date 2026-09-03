// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Jernej Skrabec <jernej.skrabec@gmail.com>
 *
 * The A733 keeps its CPU clocks apart from the CCU: a linear PLL for each
 * cluster and for the DSU, a generic backup PLL, and a clock selector per
 * consumer that picks between the oscillator, the PLL and the backups.
 */

#include <linux/bitfield.h>
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include <dt-bindings/clock/sun60i-a733-cpu-ccu.h>

#include "ccu_common.h"
#include "ccu_reset.h"

#include "ccu_mux.h"
#include "ccu_nkmp.h"

/*
 * The linear PLLs run straight off the oscillator: rate = parent * N / P.
 * P is not in the PLL register but in the consumer's clock register, and a
 * factor change has to be latched with the update bit before it takes.
 */
#define LPLL_EN				(BIT(31) | BIT(30) | BIT(29) | BIT(27))
#define LPLL_LOCK			BIT(28)
#define LPLL_UPDATE			BIT(26)
#define LPLL_FACTORS			GENMASK(21, 8)
#define LPLL_N				GENMASK(15, 8)
#define LPLL_VCO_MIN			480000000UL
#define LPLL_VCO_MAX			2260000000UL

#define CPU_CLK_P			GENMASK(17, 16)

struct sun60i_a733_lpll {
	struct ccu_common	common;
	u16			clk_reg;
};

static struct sun60i_a733_lpll *hw_to_lpll(struct clk_hw *hw)
{
	return container_of(hw_to_ccu_common(hw), struct sun60i_a733_lpll, common);
}

static int lpll_enable(struct clk_hw *hw)
{
	struct ccu_common *common = hw_to_ccu_common(hw);
	unsigned long flags;
	u32 reg;

	spin_lock_irqsave(common->lock, flags);
	reg = readl(common->base + common->reg);
	writel(reg | LPLL_EN, common->base + common->reg);
	spin_unlock_irqrestore(common->lock, flags);

	ccu_helper_wait_for_lock(common, LPLL_LOCK);

	return 0;
}

static void lpll_disable(struct clk_hw *hw)
{
	struct ccu_common *common = hw_to_ccu_common(hw);
	unsigned long flags;
	u32 reg;

	spin_lock_irqsave(common->lock, flags);
	reg = readl(common->base + common->reg);
	writel(reg & ~LPLL_EN, common->base + common->reg);
	spin_unlock_irqrestore(common->lock, flags);
}

static int lpll_is_enabled(struct clk_hw *hw)
{
	struct ccu_common *common = hw_to_ccu_common(hw);

	return (readl(common->base + common->reg) & LPLL_EN) == LPLL_EN;
}

static unsigned long lpll_recalc_rate(struct clk_hw *hw,
				      unsigned long parent_rate)
{
	struct sun60i_a733_lpll *lpll = hw_to_lpll(hw);
	u32 n = FIELD_GET(LPLL_N, readl(lpll->common.base + lpll->common.reg));
	u32 p = FIELD_GET(CPU_CLK_P, readl(lpll->common.base + lpll->clk_reg));

	return parent_rate * n >> p;
}

/*
 * Pick the highest rate not above the request. P only exists to reach rates
 * below the VCO range, so try it in increasing order.
 */
static unsigned long lpll_find_best(unsigned long parent, unsigned long rate,
				    u32 *n_out, u32 *p_out)
{
	unsigned long best = 0;
	u32 p;

	for (p = 0; p < 3; p++) {
		unsigned long vco = rate << p, n, out;

		if (vco < LPLL_VCO_MIN)
			continue;
		if (vco > LPLL_VCO_MAX)
			vco = LPLL_VCO_MAX;

		n = vco / parent;
		out = parent * n >> p;
		if (out > best) {
			best = out;
			*n_out = n;
			*p_out = p;
		}
		break;
	}

	if (!best) {
		*n_out = DIV_ROUND_UP(LPLL_VCO_MIN, parent);
		*p_out = 2;
		best = parent * *n_out >> 2;
	}

	return best;
}

static int lpll_determine_rate(struct clk_hw *hw, struct clk_rate_request *req)
{
	u32 n, p;

	req->rate = lpll_find_best(req->best_parent_rate, req->rate, &n, &p);

	return 0;
}

static int lpll_set_rate(struct clk_hw *hw, unsigned long rate,
			 unsigned long parent_rate)
{
	struct sun60i_a733_lpll *lpll = hw_to_lpll(hw);
	struct ccu_common *common = &lpll->common;
	unsigned long flags;
	u32 reg, n, p;

	lpll_find_best(parent_rate, rate, &n, &p);

	spin_lock_irqsave(common->lock, flags);

	reg = readl(common->base + lpll->clk_reg);
	reg &= ~CPU_CLK_P;
	writel(reg | FIELD_PREP(CPU_CLK_P, p), common->base + lpll->clk_reg);

	reg = readl(common->base + common->reg);
	reg &= ~LPLL_FACTORS;
	reg |= FIELD_PREP(LPLL_N, n) | LPLL_UPDATE;
	writel(reg, common->base + common->reg);

	spin_unlock_irqrestore(common->lock, flags);

	/* The update bit clears once the new factors are in */
	WARN_ON(readl_poll_timeout(common->base + common->reg, reg,
				   !(reg & LPLL_UPDATE), 1, 1000));
	ccu_helper_wait_for_lock(common, LPLL_LOCK);

	return 0;
}

static const struct clk_ops sun60i_a733_lpll_ops = {
	.disable	= lpll_disable,
	.enable		= lpll_enable,
	.is_enabled	= lpll_is_enabled,
	.recalc_rate	= lpll_recalc_rate,
	.determine_rate	= lpll_determine_rate,
	.set_rate	= lpll_set_rate,
};

static const struct clk_parent_data hosc[] = {
	{ .fw_name = "hosc" },
};

#define SUN60I_A733_LPLL(_struct, _name, _reg, _clk_reg)			\
	struct sun60i_a733_lpll _struct = {				\
		.clk_reg	= _clk_reg,				\
		.common		= {					\
			.reg		= _reg,				\
			.hw.init	= CLK_HW_INIT_PARENTS_DATA(_name, hosc, \
							&sun60i_a733_lpll_ops, \
							CLK_SET_RATE_UNGATE), \
		},							\
	}

static SUN60I_A733_LPLL(pll_cpu_l_clk, "pll-cpu-l", 0x1000, 0x101c);
static SUN60I_A733_LPLL(pll_cpu_b_clk, "pll-cpu-b", 0x2000, 0x201c);
static SUN60I_A733_LPLL(pll_cpu_dsu_clk, "pll-cpu-dsu", 0x3000, 0x301c);

/* The backup PLL is a regular one: rate = parent * N / M1 / P0 */
static struct ccu_nkmp pll_cpu_back_clk = {
	.enable		= BIT(31) | BIT(30) | BIT(29) | BIT(27),
	.lock		= BIT(28),
	.n		= _SUNXI_CCU_MULT_MIN(8, 8, 12),
	.k		= _SUNXI_CCU_MULT(0, 0),
	.m		= _SUNXI_CCU_DIV(1, 1),
	.p		= _SUNXI_CCU_DIV(20, 3),
	.common		= {
		.reg		= 0x0000,
		.hw.init	= CLK_HW_INIT_PARENTS_DATA("pll-cpu-back", hosc,
							   &ccu_nkmp_ops,
							   CLK_SET_RATE_GATE),
	},
};

static const struct clk_parent_data cpu_l_parents[] = {
	{ .fw_name = "hosc" },
	{ .fw_name = "hosc" },
	{ .fw_name = "hosc" },
	{ .hw = &pll_cpu_l_clk.common.hw },
	{ .fw_name = "pll-periph0-2x" },
	{ .hw = &pll_cpu_back_clk.common.hw },
};

static const struct clk_parent_data cpu_b_parents[] = {
	{ .fw_name = "hosc" },
	{ .fw_name = "hosc" },
	{ .fw_name = "hosc" },
	{ .hw = &pll_cpu_b_clk.common.hw },
	{ .fw_name = "pll-periph0-2x" },
	{ .hw = &pll_cpu_back_clk.common.hw },
};

static const struct clk_parent_data cpu_dsu_parents[] = {
	{ .fw_name = "hosc" },
	{ .fw_name = "hosc" },
	{ .fw_name = "hosc" },
	{ .hw = &pll_cpu_dsu_clk.common.hw },
	{ .hw = &pll_cpu_l_clk.common.hw },
	{ .fw_name = "pll-periph0-600m" },
};

#define CPU_CLK_FLAGS	(CLK_SET_RATE_PARENT | CLK_SET_RATE_NO_REPARENT | \
			 CLK_IS_CRITICAL)

static SUNXI_CCU_MUX_DATA(cpu_l_clk, "cpu-l", cpu_l_parents,
			  0x101c, 24, 3, CPU_CLK_FLAGS);
static SUNXI_CCU_MUX_DATA(cpu_b_clk, "cpu-b", cpu_b_parents,
			  0x201c, 24, 3, CPU_CLK_FLAGS);
static SUNXI_CCU_MUX_DATA(cpu_dsu_clk, "cpu-dsu", cpu_dsu_parents,
			  0x301c, 24, 3, CPU_CLK_FLAGS);

static struct ccu_common *sun60i_a733_cpu_ccu_clks[] = {
	&pll_cpu_back_clk.common,
	&pll_cpu_l_clk.common,
	&pll_cpu_b_clk.common,
	&pll_cpu_dsu_clk.common,
	&cpu_l_clk.common,
	&cpu_b_clk.common,
	&cpu_dsu_clk.common,
};

static struct clk_hw_onecell_data sun60i_a733_cpu_ccu_hw_clks = {
	.num	= CLK_CPU_DSU + 1,
	.hws	= {
		[CLK_PLL_CPU_BACK]	= &pll_cpu_back_clk.common.hw,
		[CLK_PLL_CPU_L]		= &pll_cpu_l_clk.common.hw,
		[CLK_PLL_CPU_B]		= &pll_cpu_b_clk.common.hw,
		[CLK_PLL_CPU_DSU]	= &pll_cpu_dsu_clk.common.hw,
		[CLK_CPU_L]		= &cpu_l_clk.common.hw,
		[CLK_CPU_B]		= &cpu_b_clk.common.hw,
		[CLK_CPU_DSU]		= &cpu_dsu_clk.common.hw,
	},
};

static const struct sunxi_ccu_desc sun60i_a733_cpu_ccu_desc = {
	.ccu_clks	= sun60i_a733_cpu_ccu_clks,
	.num_ccu_clks	= ARRAY_SIZE(sun60i_a733_cpu_ccu_clks),

	.hw_clks	= &sun60i_a733_cpu_ccu_hw_clks,
};

/*
 * Run each consumer from one of its backup clocks while its PLL changes rate.
 * The manual lists the periphery PLL outputs among those backup clocks, and
 * they keep the cores fast enough to make progress during the switch.
 */
static struct ccu_mux_nb sun60i_a733_cpu_l_nb = {
	.common		= &cpu_l_clk.common,
	.cm		= &cpu_l_clk.mux,
	.delay_us	= 1,
	.bypass_index	= 4, /* PLL_PERI0 at 1.2 GHz */
};

static struct ccu_mux_nb sun60i_a733_cpu_b_nb = {
	.common		= &cpu_b_clk.common,
	.cm		= &cpu_b_clk.mux,
	.delay_us	= 1,
	.bypass_index	= 4, /* PLL_PERI0 at 1.2 GHz */
};

static struct ccu_mux_nb sun60i_a733_cpu_dsu_nb = {
	.common		= &cpu_dsu_clk.common,
	.cm		= &cpu_dsu_clk.mux,
	.delay_us	= 1,
	.bypass_index	= 5, /* PLL_PERI0 at 600 MHz */
};

static int sun60i_a733_cpu_ccu_probe(struct platform_device *pdev)
{
	void __iomem *reg;
	int ret;

	reg = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(reg))
		return PTR_ERR(reg);

	ret = devm_sunxi_ccu_probe(&pdev->dev, reg, &sun60i_a733_cpu_ccu_desc);
	if (ret)
		return ret;

	ccu_mux_notifier_register(pll_cpu_l_clk.common.hw.clk,
				  &sun60i_a733_cpu_l_nb);
	ccu_mux_notifier_register(pll_cpu_b_clk.common.hw.clk,
				  &sun60i_a733_cpu_b_nb);
	ccu_mux_notifier_register(pll_cpu_dsu_clk.common.hw.clk,
				  &sun60i_a733_cpu_dsu_nb);

	return 0;
}

static const struct of_device_id sun60i_a733_cpu_ccu_ids[] = {
	{ .compatible = "allwinner,sun60i-a733-cpu-ccu" },
	{ }
};
MODULE_DEVICE_TABLE(of, sun60i_a733_cpu_ccu_ids);

static struct platform_driver sun60i_a733_cpu_ccu_driver = {
	.probe	= sun60i_a733_cpu_ccu_probe,
	.driver	= {
		.name			= "sun60i-a733-cpu-ccu",
		.suppress_bind_attrs	= true,
		.of_match_table		= sun60i_a733_cpu_ccu_ids,
	},
};
module_platform_driver(sun60i_a733_cpu_ccu_driver);

MODULE_IMPORT_NS("SUNXI_CCU");
MODULE_DESCRIPTION("Support for the Allwinner A733 CPU CCU");
MODULE_LICENSE("GPL");
