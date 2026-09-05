// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Jernej Skrabec <jernej.skrabec@siol.net>
 */

#include <linux/bitfield.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/regmap.h>

#include "sun8i_dw_hdmi.h"

struct sun8i_phy_clk {
	struct clk_hw		hw;
	struct sun8i_hdmi_phy	*phy;
};

static inline struct sun8i_phy_clk *hw_to_phy_clk(struct clk_hw *hw)
{
	return container_of(hw, struct sun8i_phy_clk, hw);
}

/*
 * The wrapper's own PLL drives the transmit clock. Its settings are keyed on
 * the pixel clock and assume the 26 MHz oscillator; the factors are packed
 * the way the vendor tables carry them, so unpack them on the way out.
 */
struct sun60i_a733_hdmi_phy_pll {
	unsigned long	mpixelclock;
	u32		factors;
	u32		ldo;
	u32		pattern0;
	u32		pattern1;
};

static const struct sun60i_a733_hdmi_phy_pll sun60i_a733_hdmi_phy_pll[] = {
	{  13500000, 0xe8673500, 0x00035000, 0x00000000, 0x30000000 },
	{  27000000, 0xe8595c00, 0x00035000, 0x80000000, 0x30000000 },
	{  54000000, 0xe80c1a00, 0x00035000, 0x80000000, 0x30000000 },
	{  65000000, 0xe8235a00, 0x00035000, 0x80000000, 0x30000000 },
	{  74250000, 0xe81f5a00, 0x00035000, 0x80000000, 0x30000000 },
	{ 108000000, 0xe80c3500, 0x00035000, 0x00000000, 0x30000000 },
	{ 148500000, 0xe80f5a00, 0x00035000, 0x80000000, 0x30000000 },
	{ 185625000, 0xe80f5a00, 0x00035000, 0x80000000, 0x30000000 },
	{ 297000000, 0xe807b602, 0x00035000, 0x00000000, 0x30000000 },
	{ 371250000, 0xe807b602, 0x00035000, 0x00000000, 0x30000000 },
	{ 594000000, 0xe803b602, 0x00035000, 0x00000000, 0x30000000 },
};

/* The factors sit in the low half of the packed value. */
#define SUN60I_A733_PLL_FACTOR_INPUT_DIV2	BIT(1)
#define SUN60I_A733_PLL_FACTOR_LOCK_MODE	BIT(5)
#define SUN60I_A733_PLL_FACTOR_UNLOCK_MODE	GENMASK(7, 6)
#define SUN60I_A733_PLL_FACTOR_N		GENMASK(15, 8)
#define SUN60I_A733_PLL_FACTOR_P0		GENMASK(22, 16)
#define SUN60I_A733_PLL_FACTOR_MASK		GENMASK(22, 0)

static const struct sun60i_a733_hdmi_phy_pll *
sun60i_a733_hdmi_phy_find_pll(unsigned long rate)
{
	const struct sun60i_a733_hdmi_phy_pll *table = sun60i_a733_hdmi_phy_pll;
	unsigned int last = ARRAY_SIZE(sun60i_a733_hdmi_phy_pll) - 1;
	unsigned int i;

	if (rate <= table[0].mpixelclock)
		return &table[0];
	if (rate >= table[last].mpixelclock)
		return &table[last];

	for (i = 0; i < last; i++) {
		if (rate == table[i].mpixelclock)
			return &table[i];
		if (rate > table[i].mpixelclock &&
		    rate < table[i + 1].mpixelclock)
			return (table[i + 1].mpixelclock - rate) >
			       (rate - table[i].mpixelclock) ?
			       &table[i] : &table[i + 1];
	}

	return &table[last];
}

/*
 * Several entries drive the PLL in fractional mode, so the integer factors
 * alone do not describe the rate they produce. Read the factors back and
 * report what the entry carrying them was aiming for.
 */
static unsigned long
sun60i_a733_hdmi_phy_pll_rate(u32 factors, unsigned long parent_rate)
{
	const struct sun60i_a733_hdmi_phy_pll *pll = sun60i_a733_hdmi_phy_pll;
	unsigned long n, p0, div2;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(sun60i_a733_hdmi_phy_pll); i++)
		if ((pll[i].factors & SUN60I_A733_PLL_FACTOR_MASK) == factors)
			return pll[i].mpixelclock;

	n = FIELD_GET(SUN60I_A733_PLL_FACTOR_N, factors) + 1;
	p0 = FIELD_GET(SUN60I_A733_PLL_FACTOR_P0, factors) + 1;
	div2 = (factors & SUN60I_A733_PLL_FACTOR_INPUT_DIV2) ? 2 : 1;

	return parent_rate / div2 * n / p0;
}

static int sun60i_a733_phy_clk_set_rate(struct clk_hw *hw, unsigned long rate,
					unsigned long parent_rate)
{
	struct sun8i_hdmi_phy *phy = hw_to_phy_clk(hw)->phy;
	const struct sun60i_a733_hdmi_phy_pll *pll;
	unsigned int val;
	int ret;

	pll = sun60i_a733_hdmi_phy_find_pll(rate);

	regmap_update_bits(phy->regs, SUN60I_A733_HDMI_PHY_PLL,
			   SUN60I_A733_HDMI_PHY_PLL_OUT_GATE, 0);

	val = 0;
	if (pll->factors & SUN60I_A733_PLL_FACTOR_INPUT_DIV2)
		val |= SUN60I_A733_HDMI_PHY_PLL_INPUT_DIV2;
	if (pll->factors & SUN60I_A733_PLL_FACTOR_LOCK_MODE)
		val |= SUN60I_A733_HDMI_PHY_PLL_LOCK_MODE;
	val |= FIELD_PREP(SUN60I_A733_HDMI_PHY_PLL_UNLOCK_MODE,
			  FIELD_GET(SUN60I_A733_PLL_FACTOR_UNLOCK_MODE,
				    pll->factors));
	val |= FIELD_PREP(SUN60I_A733_HDMI_PHY_PLL_N,
			  FIELD_GET(SUN60I_A733_PLL_FACTOR_N, pll->factors));
	val |= FIELD_PREP(SUN60I_A733_HDMI_PHY_PLL_P0,
			  FIELD_GET(SUN60I_A733_PLL_FACTOR_P0, pll->factors));

	regmap_update_bits(phy->regs, SUN60I_A733_HDMI_PHY_PLL,
			   SUN60I_A733_HDMI_PHY_PLL_INPUT_DIV2 |
			   SUN60I_A733_HDMI_PHY_PLL_LOCK_MODE |
			   SUN60I_A733_HDMI_PHY_PLL_UNLOCK_MODE |
			   SUN60I_A733_HDMI_PHY_PLL_N |
			   SUN60I_A733_HDMI_PHY_PLL_P0, val);

	regmap_write(phy->regs, SUN60I_A733_HDMI_PHY_PLL_LDO, pll->ldo);
	regmap_write(phy->regs, SUN60I_A733_HDMI_PHY_PLL_PATTERN0,
		     pll->pattern0);
	regmap_write(phy->regs, SUN60I_A733_HDMI_PHY_PLL_PATTERN1,
		     pll->pattern1);

	regmap_update_bits(phy->regs, SUN60I_A733_HDMI_PHY_PLL,
			   SUN60I_A733_HDMI_PHY_PLL_EN |
			   SUN60I_A733_HDMI_PHY_PLL_LDO_EN,
			   SUN60I_A733_HDMI_PHY_PLL_EN |
			   SUN60I_A733_HDMI_PHY_PLL_LDO_EN);

	regmap_update_bits(phy->regs, SUN60I_A733_HDMI_PHY_CLK,
			   SUN60I_A733_HDMI_PHY_CLK_SHIFTER_GATE,
			   SUN60I_A733_HDMI_PHY_CLK_SHIFTER_GATE);

	regmap_update_bits(phy->regs, SUN60I_A733_HDMI_PHY_PLL,
			   SUN60I_A733_HDMI_PHY_PLL_LOCK_EN,
			   SUN60I_A733_HDMI_PHY_PLL_LOCK_EN);

	ret = regmap_read_poll_timeout(phy->regs,
				       SUN60I_A733_HDMI_PHY_PLL_STATUS, val,
				       val & SUN60I_A733_HDMI_PHY_PLL_STATUS_LOCK,
				       100, 20000);
	if (ret) {
		dev_err(phy->dev, "PLL failed to lock for %lu Hz\n", rate);
		return ret;
	}

	udelay(20);

	regmap_update_bits(phy->regs, SUN60I_A733_HDMI_PHY_PLL,
			   SUN60I_A733_HDMI_PHY_PLL_OUT_GATE,
			   SUN60I_A733_HDMI_PHY_PLL_OUT_GATE);

	return 0;
}


static unsigned long sun60i_a733_phy_clk_recalc_rate(struct clk_hw *hw,
						     unsigned long parent_rate)
{
	struct sun8i_hdmi_phy *phy = hw_to_phy_clk(hw)->phy;
	unsigned int val;
	u32 factors;

	regmap_read(phy->regs, SUN60I_A733_HDMI_PHY_PLL, &val);

	factors = FIELD_PREP(SUN60I_A733_PLL_FACTOR_N,
			     FIELD_GET(SUN60I_A733_HDMI_PHY_PLL_N, val)) |
		  FIELD_PREP(SUN60I_A733_PLL_FACTOR_P0,
			     FIELD_GET(SUN60I_A733_HDMI_PHY_PLL_P0, val));
	if (val & SUN60I_A733_HDMI_PHY_PLL_INPUT_DIV2)
		factors |= SUN60I_A733_PLL_FACTOR_INPUT_DIV2;

	return sun60i_a733_hdmi_phy_pll_rate(factors, parent_rate);
}

static int sun60i_a733_phy_clk_determine_rate(struct clk_hw *hw,
					      struct clk_rate_request *req)
{
	const struct sun60i_a733_hdmi_phy_pll *pll;

	pll = sun60i_a733_hdmi_phy_find_pll(req->rate);
	req->rate = pll->mpixelclock;

	return 0;
}

static int sun60i_a733_phy_clk_enable(struct clk_hw *hw)
{
	struct sun8i_hdmi_phy *phy = hw_to_phy_clk(hw)->phy;

	return regmap_update_bits(phy->regs, SUN60I_A733_HDMI_PHY_PLL,
				  SUN60I_A733_HDMI_PHY_PLL_OUT_GATE,
				  SUN60I_A733_HDMI_PHY_PLL_OUT_GATE);
}

static void sun60i_a733_phy_clk_disable(struct clk_hw *hw)
{
	struct sun8i_hdmi_phy *phy = hw_to_phy_clk(hw)->phy;

	regmap_update_bits(phy->regs, SUN60I_A733_HDMI_PHY_PLL,
			   SUN60I_A733_HDMI_PHY_PLL_OUT_GATE, 0);
}

static int sun60i_a733_phy_clk_is_enabled(struct clk_hw *hw)
{
	struct sun8i_hdmi_phy *phy = hw_to_phy_clk(hw)->phy;
	unsigned int val;

	regmap_read(phy->regs, SUN60I_A733_HDMI_PHY_PLL, &val);

	return !!(val & SUN60I_A733_HDMI_PHY_PLL_OUT_GATE);
}

static const struct clk_ops sun60i_a733_phy_clk_ops = {
	.determine_rate	= sun60i_a733_phy_clk_determine_rate,
	.recalc_rate	= sun60i_a733_phy_clk_recalc_rate,
	.set_rate	= sun60i_a733_phy_clk_set_rate,
	.enable		= sun60i_a733_phy_clk_enable,
	.disable	= sun60i_a733_phy_clk_disable,
	.is_enabled	= sun60i_a733_phy_clk_is_enabled,
};

static int sun8i_phy_clk_determine_rate(struct clk_hw *hw,
					struct clk_rate_request *req)
{
	unsigned long rate = req->rate;
	unsigned long best_rate = 0;
	struct clk_hw *best_parent = NULL;
	struct clk_hw *parent;
	int best_div = 1;
	int i, p;

	for (p = 0; p < clk_hw_get_num_parents(hw); p++) {
		parent = clk_hw_get_parent_by_index(hw, p);
		if (!parent)
			continue;

		for (i = 1; i <= 16; i++) {
			unsigned long ideal = rate * i;
			unsigned long rounded;

			rounded = clk_hw_round_rate(parent, ideal);

			if (rounded == ideal) {
				best_rate = rounded;
				best_div = i;
				best_parent = parent;
				break;
			}

			if (!best_rate ||
			    abs(rate - rounded / i) <
			    abs(rate - best_rate / best_div)) {
				best_rate = rounded;
				best_div = i;
				best_parent = parent;
			}
		}

		if (best_rate / best_div == rate)
			break;
	}

	req->rate = best_rate / best_div;
	req->best_parent_rate = best_rate;
	req->best_parent_hw = best_parent;

	return 0;
}

static unsigned long sun8i_phy_clk_recalc_rate(struct clk_hw *hw,
					       unsigned long parent_rate)
{
	struct sun8i_phy_clk *priv = hw_to_phy_clk(hw);
	u32 reg;

	regmap_read(priv->phy->regs, SUN8I_HDMI_PHY_PLL_CFG2_REG, &reg);
	reg = ((reg >> SUN8I_HDMI_PHY_PLL_CFG2_PREDIV_SHIFT) &
		SUN8I_HDMI_PHY_PLL_CFG2_PREDIV_MSK) + 1;

	return parent_rate / reg;
}

static int sun8i_phy_clk_set_rate(struct clk_hw *hw, unsigned long rate,
				  unsigned long parent_rate)
{
	struct sun8i_phy_clk *priv = hw_to_phy_clk(hw);
	unsigned long best_rate = 0;
	u8 best_m = 0, m;

	for (m = 1; m <= 16; m++) {
		unsigned long tmp_rate = parent_rate / m;

		if (tmp_rate > rate)
			continue;

		if (!best_rate ||
		    (rate - tmp_rate) < (rate - best_rate)) {
			best_rate = tmp_rate;
			best_m = m;
		}
	}

	regmap_update_bits(priv->phy->regs, SUN8I_HDMI_PHY_PLL_CFG2_REG,
			   SUN8I_HDMI_PHY_PLL_CFG2_PREDIV_MSK,
			   SUN8I_HDMI_PHY_PLL_CFG2_PREDIV(best_m));

	return 0;
}

static u8 sun8i_phy_clk_get_parent(struct clk_hw *hw)
{
	struct sun8i_phy_clk *priv = hw_to_phy_clk(hw);
	u32 reg;

	regmap_read(priv->phy->regs, SUN8I_HDMI_PHY_PLL_CFG1_REG, &reg);
	reg = (reg & SUN8I_HDMI_PHY_PLL_CFG1_CKIN_SEL_MSK) >>
	      SUN8I_HDMI_PHY_PLL_CFG1_CKIN_SEL_SHIFT;

	return reg;
}

static int sun8i_phy_clk_set_parent(struct clk_hw *hw, u8 index)
{
	struct sun8i_phy_clk *priv = hw_to_phy_clk(hw);

	if (index > 1)
		return -EINVAL;

	regmap_update_bits(priv->phy->regs, SUN8I_HDMI_PHY_PLL_CFG1_REG,
			   SUN8I_HDMI_PHY_PLL_CFG1_CKIN_SEL_MSK,
			   index << SUN8I_HDMI_PHY_PLL_CFG1_CKIN_SEL_SHIFT);

	return 0;
}

static const struct clk_ops sun8i_phy_clk_ops = {
	.determine_rate	= sun8i_phy_clk_determine_rate,
	.recalc_rate	= sun8i_phy_clk_recalc_rate,
	.set_rate	= sun8i_phy_clk_set_rate,

	.get_parent	= sun8i_phy_clk_get_parent,
	.set_parent	= sun8i_phy_clk_set_parent,
};

int sun8i_phy_clk_create(struct sun8i_hdmi_phy *phy, struct device *dev,
			 bool second_parent)
{
	struct clk_init_data init;
	struct sun8i_phy_clk *priv;
	const char *parents[2];

	parents[0] = __clk_get_name(phy->clk_pll0);
	if (!parents[0])
		return -ENODEV;

	if (second_parent) {
		parents[1] = __clk_get_name(phy->clk_pll1);
		if (!parents[1])
			return -ENODEV;
	}

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	init.name = "hdmi-phy-clk";
	init.parent_names = parents;
	init.num_parents = second_parent ? 2 : 1;

	if (phy->variant->has_clk_provider) {
		/*
		 * This PLL runs from the oscillator and drives both the
		 * transmitter and, through the video output top, the timing
		 * controller, so it hands out a clock of its own and never
		 * asks its parent for a rate.
		 */
		init.ops = &sun60i_a733_phy_clk_ops;
		init.flags = 0;
	} else {
		init.ops = &sun8i_phy_clk_ops;
		init.flags = CLK_SET_RATE_PARENT;
	}

	priv->phy = phy;
	priv->hw.init = &init;

	phy->clk_phy = devm_clk_register(dev, &priv->hw);
	if (IS_ERR(phy->clk_phy))
		return PTR_ERR(phy->clk_phy);

	if (phy->variant->has_clk_provider)
		return devm_of_clk_add_hw_provider(dev, of_clk_hw_simple_get,
						   &priv->hw);

	return 0;
}
