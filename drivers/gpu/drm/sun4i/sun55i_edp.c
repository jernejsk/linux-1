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
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_of.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>

/* Subset of Innosilicon eDP 1.3 registers needed for boot-time setup. */
#define SUN55I_EDP_HPD_SCALE		0x0018
#define  SUN55I_EDP_HPD_SCALE_EN	BIT(3)
#define  SUN55I_EDP_HPD_SCALE_EDP_MODE	BIT(27)
#define SUN55I_EDP_RESET		0x001c
#define  SUN55I_EDP_RESET_CONTROLLER	BIT(0)
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

#define SUN55I_EDP_ANA_PLL_FBDIV	0x0180
#define  SUN55I_EDP_ANA_PLL_PD		BIT(0)
#define  SUN55I_EDP_ANA_PLL_FRAC_PD	GENMASK(5, 4)
#define  SUN55I_EDP_ANA_PLL_PREDIV	GENMASK(13, 8)
#define  SUN55I_EDP_ANA_PLL_FBDIV_H4	GENMASK(19, 16)
#define  SUN55I_EDP_ANA_PLL_FBDIV_L8	GENMASK(31, 24)
#define SUN55I_EDP_ANA_PLL_FRAC		0x0184
#define  SUN55I_EDP_ANA_PLL_FRAC_H8	GENMASK(7, 0)
#define  SUN55I_EDP_ANA_PLL_FRAC_M8	GENMASK(15, 8)
#define  SUN55I_EDP_ANA_PLL_FRAC_L8	GENMASK(23, 16)
#define SUN55I_EDP_ANA_PLL_POSDIV	0x0188
#define  SUN55I_EDP_ANA_PLL_POSDIV_V	GENMASK(3, 2)
#define SUN55I_EDP_ANA_AUX_CLOCK	0x018c
#define  SUN55I_EDP_ANA_AUX_CLOCK_16M_DIV	GENMASK(4, 0)
#define SUN55I_EDP_AUX_ISEL_MAINSET	0x01a0
#define  SUN55I_EDP_AUX_ISEL_MASK	GENMASK(7, 4)
#define  SUN55I_EDP_AUX_MAINSET_MASK	GENMASK(11, 8)
#define SUN55I_EDP_TX_MAINSEL		0x01a8
#define  SUN55I_EDP_TX_MAINSEL_LO	GENMASK(20, 16)
#define  SUN55I_EDP_TX_MAINSEL_HI	GENMASK(28, 24)
#define SUN55I_EDP_TX_POSTSEL		0x01ac
#define  SUN55I_EDP_TX_POSTSEL_LO	GENMASK(4, 0)
#define  SUN55I_EDP_TX_POSTSEL_HI	GENMASK(12, 8)
#define SUN55I_EDP_TX_PRESEL		0x01b0
#define  SUN55I_EDP_TX_PRESEL_LO	GENMASK(14, 0)
#define  SUN55I_EDP_TX_PRESEL_MODE	GENMASK(27, 24)
#define SUN55I_EDP_RES1000_CFG		0x2014
#define  SUN55I_EDP_RES1000_CFG_VAL	GENMASK(5, 0)
#define  SUN55I_EDP_RES1000_CFG_EN	BIT(8)

#define SUN55I_EDP_VIDEO_STREAM_EN	0x0200
#define  SUN55I_EDP_VIDEO_STREAM_EN_BIT	BIT(5)
#define SUN55I_EDP_SYNC_POLARITY	0x020c
#define  SUN55I_EDP_VSYNC_POL		BIT(0)
#define  SUN55I_EDP_HSYNC_POL		BIT(1)
#define SUN55I_EDP_HACTIVE_BLANK	0x0210
#define  SUN55I_EDP_HACTIVE		GENMASK(31, 16)
#define  SUN55I_EDP_HBLANK		GENMASK(15, 2)
#define SUN55I_EDP_VACTIVE_BLANK	0x0214
#define  SUN55I_EDP_VACTIVE		GENMASK(15, 0)
#define  SUN55I_EDP_VBLANK		GENMASK(31, 16)
#define SUN55I_EDP_HSW_FRONT_PORCH	0x0218
#define  SUN55I_EDP_HSW			GENMASK(31, 16)
#define  SUN55I_EDP_HFP			GENMASK(15, 0)
#define SUN55I_EDP_VSW_FRONT_PORCH	0x021c
#define  SUN55I_EDP_VSW			GENMASK(31, 16)
#define  SUN55I_EDP_VFP			GENMASK(15, 0)
#define SUN55I_EDP_SYNC_START		0x0224
#define  SUN55I_EDP_HSTART		GENMASK(15, 0)
#define  SUN55I_EDP_VSTART		GENMASK(31, 16)
#define SUN55I_EDP_CAPACITY		0x0100
#define  SUN55I_EDP_CAP_PATTERN		GENMASK(3, 0)
#define  SUN55I_EDP_CAP_RATE		GENMASK(5, 4)
#define  SUN55I_EDP_CAP_LANE_IDX	GENMASK(7, 6)
#define  SUN55I_EDP_CAP_LANE_EN		GENMASK(11, 8)
#define  SUN55I_EDP_CAP_RATE_HI		GENMASK(28, 26)
#define  SUN55I_EDP_CAPACITY_LINK_RESET	(GENMASK(11, 0) | GENMASK(28, 26))

#define SUN55I_EDP_TX32_ISEL_DRV	0x01a4
#define  SUN55I_EDP_TX32_LANE2_SW	GENMASK(27, 24)
#define  SUN55I_EDP_TX32_LANE3_SW	GENMASK(31, 28)
#define SUN55I_EDP_TX_MAINSEL_LANE0_SW	GENMASK(3, 0)
#define SUN55I_EDP_TX_MAINSEL_LANE1_SW	GENMASK(7, 4)
#define SUN55I_EDP_TX_POSTSEL_LANE0_PE	GENMASK(27, 24)
#define SUN55I_EDP_TX_POSTSEL_LANE1_PE	GENMASK(31, 28)
#define SUN55I_EDP_TX_POSTSEL_LANE2_PE	GENMASK(19, 16)
#define SUN55I_EDP_TX_POSTSEL_LANE3_PE	GENMASK(23, 20)

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

	/* Cached sink capabilities. Populated on plug-in. */
	u8			dpcd[DP_RECEIVER_CAP_SIZE];
	u8			max_lane_count;
	u8			max_link_rate;
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

struct sun55i_edp_pll_cfg;
static const struct sun55i_edp_pll_cfg sun55i_edp_pll_rbr;
static const struct sun55i_edp_pll_cfg sun55i_edp_pll_hbr;
static void sun55i_edp_aux_clock_setup(struct sun55i_edp *edp, u32 bit_mhz);
static void sun55i_edp_corepll_set(struct sun55i_edp *edp,
				   const struct sun55i_edp_pll_cfg *cfg);

/* Voltage-swing / pre-emphasis table for low-voltage eDP panels (param 0). */
struct sun55i_edp_train_lvl {
	u8 sw;
	u8 pe;
};

static const struct sun55i_edp_train_lvl
sun55i_edp_train_table[DP_TRAIN_VOLTAGE_SWING_LEVEL_3 + 1]
		      [DP_TRAIN_PRE_EMPH_LEVEL_3 + 1] = {
	{ { 0x1, 0x0 }, { 0x3, 0x4 }, { 0x5, 0x7 }, { 0x5, 0x7 } },
	{ { 0x2, 0x0 }, { 0x5, 0x4 }, { 0x7, 0x7 }, { 0x7, 0x7 } },
	{ { 0x3, 0x0 }, { 0x6, 0x4 }, { 0xa, 0x7 }, { 0xa, 0x7 } },
	{ { 0x5, 0x0 }, { 0x8, 0x4 }, { 0xf, 0x7 }, { 0xf, 0x7 } },
};

static void sun55i_edp_train_set_rate(struct sun55i_edp *edp, u8 link_rate)
{
	u32 val;

	val = readl(edp->regs + SUN55I_EDP_CAPACITY);
	val &= ~(SUN55I_EDP_CAP_RATE | SUN55I_EDP_CAP_RATE_HI);
	if (link_rate == DP_LINK_BW_2_7)
		val |= FIELD_PREP(SUN55I_EDP_CAP_RATE, 0x1);
	writel(val, edp->regs + SUN55I_EDP_CAPACITY);
}

static void sun55i_edp_train_set_lanes(struct sun55i_edp *edp, u8 lanes)
{
	u32 val;

	val = readl(edp->regs + SUN55I_EDP_CAPACITY);
	val &= ~(SUN55I_EDP_CAP_LANE_IDX | SUN55I_EDP_CAP_LANE_EN);
	switch (lanes) {
	case 1:
		val |= FIELD_PREP(SUN55I_EDP_CAP_LANE_IDX, 0x0);
		val |= FIELD_PREP(SUN55I_EDP_CAP_LANE_EN, 0x1);
		break;
	case 2:
		val |= FIELD_PREP(SUN55I_EDP_CAP_LANE_IDX, 0x1);
		val |= FIELD_PREP(SUN55I_EDP_CAP_LANE_EN, 0x3);
		break;
	case 4:
		val |= FIELD_PREP(SUN55I_EDP_CAP_LANE_IDX, 0x2);
		val |= FIELD_PREP(SUN55I_EDP_CAP_LANE_EN, 0xf);
		break;
	default:
		break;
	}
	writel(val, edp->regs + SUN55I_EDP_CAPACITY);
}

static void sun55i_edp_train_set_pattern(struct sun55i_edp *edp, u8 pattern)
{
	u32 val;

	val = readl(edp->regs + SUN55I_EDP_CAPACITY);
	val &= ~SUN55I_EDP_CAP_PATTERN;
	val |= FIELD_PREP(SUN55I_EDP_CAP_PATTERN, pattern);
	writel(val, edp->regs + SUN55I_EDP_CAPACITY);
}

static void sun55i_edp_train_set_lane_drive(struct sun55i_edp *edp,
					    u8 lane, u8 vs, u8 pe)
{
	u8 sw_lv = sun55i_edp_train_table[vs][pe].sw;
	u8 pe_lv = sun55i_edp_train_table[vs][pe].pe;
	u32 val;

	switch (lane) {
	case 0:
		val = readl(edp->regs + SUN55I_EDP_TX_MAINSEL);
		val &= ~SUN55I_EDP_TX_MAINSEL_LANE0_SW;
		val |= FIELD_PREP(SUN55I_EDP_TX_MAINSEL_LANE0_SW, sw_lv);
		writel(val, edp->regs + SUN55I_EDP_TX_MAINSEL);

		val = readl(edp->regs + SUN55I_EDP_TX_POSTSEL);
		val &= ~SUN55I_EDP_TX_POSTSEL_LANE0_PE;
		val |= FIELD_PREP(SUN55I_EDP_TX_POSTSEL_LANE0_PE, pe_lv);
		writel(val, edp->regs + SUN55I_EDP_TX_POSTSEL);
		break;
	case 1:
		val = readl(edp->regs + SUN55I_EDP_TX_MAINSEL);
		val &= ~SUN55I_EDP_TX_MAINSEL_LANE1_SW;
		val |= FIELD_PREP(SUN55I_EDP_TX_MAINSEL_LANE1_SW, sw_lv);
		writel(val, edp->regs + SUN55I_EDP_TX_MAINSEL);

		val = readl(edp->regs + SUN55I_EDP_TX_POSTSEL);
		val &= ~SUN55I_EDP_TX_POSTSEL_LANE1_PE;
		val |= FIELD_PREP(SUN55I_EDP_TX_POSTSEL_LANE1_PE, pe_lv);
		writel(val, edp->regs + SUN55I_EDP_TX_POSTSEL);
		break;
	case 2:
		val = readl(edp->regs + SUN55I_EDP_TX32_ISEL_DRV);
		val &= ~SUN55I_EDP_TX32_LANE2_SW;
		val |= FIELD_PREP(SUN55I_EDP_TX32_LANE2_SW, sw_lv);
		writel(val, edp->regs + SUN55I_EDP_TX32_ISEL_DRV);

		val = readl(edp->regs + SUN55I_EDP_TX_POSTSEL);
		val &= ~SUN55I_EDP_TX_POSTSEL_LANE2_PE;
		val |= FIELD_PREP(SUN55I_EDP_TX_POSTSEL_LANE2_PE, pe_lv);
		writel(val, edp->regs + SUN55I_EDP_TX_POSTSEL);
		break;
	case 3:
		val = readl(edp->regs + SUN55I_EDP_TX32_ISEL_DRV);
		val &= ~SUN55I_EDP_TX32_LANE3_SW;
		val |= FIELD_PREP(SUN55I_EDP_TX32_LANE3_SW, sw_lv);
		writel(val, edp->regs + SUN55I_EDP_TX32_ISEL_DRV);

		val = readl(edp->regs + SUN55I_EDP_TX_POSTSEL);
		val &= ~SUN55I_EDP_TX_POSTSEL_LANE3_PE;
		val |= FIELD_PREP(SUN55I_EDP_TX_POSTSEL_LANE3_PE, pe_lv);
		writel(val, edp->regs + SUN55I_EDP_TX_POSTSEL);
		break;
	}
}

static int sun55i_edp_link_train(struct sun55i_edp *edp)
{
	u8 link_status[DP_LINK_STATUS_SIZE];
	const struct sun55i_edp_pll_cfg *pll_cfg;
	u8 link_rate, lanes, dpcd, value;
	int ret, lane;

	if (!edp->plugged || !edp->max_link_rate || !edp->max_lane_count)
		return -ENODEV;

	link_rate = min_t(u8, edp->max_link_rate, DP_LINK_BW_2_7);
	lanes = min_t(u8, edp->max_lane_count, 4);

	pll_cfg = (link_rate == DP_LINK_BW_2_7) ? &sun55i_edp_pll_hbr
						: &sun55i_edp_pll_rbr;
	sun55i_edp_aux_clock_setup(edp,
				   link_rate == DP_LINK_BW_2_7 ? 1350 : 810);
	sun55i_edp_corepll_set(edp, pll_cfg);
	usleep_range(500, 1000);

	sun55i_edp_train_set_rate(edp, link_rate);
	sun55i_edp_train_set_lanes(edp, lanes);
	for (lane = 0; lane < lanes; lane++)
		sun55i_edp_train_set_lane_drive(edp, lane, 0, 0);

	ret = drm_dp_dpcd_writeb(&edp->aux, DP_LINK_BW_SET, link_rate);
	if (ret < 0)
		return ret;
	value = lanes;
	if (drm_dp_enhanced_frame_cap(edp->dpcd))
		value |= DP_LANE_COUNT_ENHANCED_FRAME_EN;
	ret = drm_dp_dpcd_writeb(&edp->aux, DP_LANE_COUNT_SET, value);
	if (ret < 0)
		return ret;

	/* Clock recovery: pattern 1, single-shot. TODO: VS/PE adjust loop. */
	sun55i_edp_train_set_pattern(edp, 1);
	dpcd = DP_TRAINING_PATTERN_1 | DP_LINK_SCRAMBLING_DISABLE;
	ret = drm_dp_dpcd_writeb(&edp->aux, DP_TRAINING_PATTERN_SET, dpcd);
	if (ret < 0)
		return ret;
	drm_dp_link_train_clock_recovery_delay(&edp->aux, edp->dpcd);
	ret = drm_dp_dpcd_read_link_status(&edp->aux, link_status);
	if (ret < 0)
		return ret;
	if (!drm_dp_clock_recovery_ok(link_status, lanes)) {
		dev_warn(edp->dev,
			 "Clock recovery failed (status %*ph), continuing.\n",
			 DP_LINK_STATUS_SIZE, link_status);
	}

	/* Channel equalization: pattern 2. */
	sun55i_edp_train_set_pattern(edp, 2);
	dpcd = DP_TRAINING_PATTERN_2 | DP_LINK_SCRAMBLING_DISABLE;
	ret = drm_dp_dpcd_writeb(&edp->aux, DP_TRAINING_PATTERN_SET, dpcd);
	if (ret < 0)
		return ret;
	drm_dp_link_train_channel_eq_delay(&edp->aux, edp->dpcd);
	ret = drm_dp_dpcd_read_link_status(&edp->aux, link_status);
	if (ret < 0)
		return ret;
	if (!drm_dp_channel_eq_ok(link_status, lanes)) {
		dev_warn(edp->dev,
			 "Channel EQ failed (status %*ph), continuing.\n",
			 DP_LINK_STATUS_SIZE, link_status);
	}

	/* End training. */
	sun55i_edp_train_set_pattern(edp, 0);
	drm_dp_dpcd_writeb(&edp->aux, DP_TRAINING_PATTERN_SET,
			   DP_TRAINING_PATTERN_DISABLE);
	return 0;
}

static void sun55i_edp_set_video_timings(struct sun55i_edp *edp,
					 const struct drm_display_mode *mode)
{
	u32 val;
	u16 hactive = mode->hdisplay;
	u16 hblank = mode->htotal - mode->hdisplay;
	u16 hfp = mode->hsync_start - mode->hdisplay;
	u16 hsw = mode->hsync_end - mode->hsync_start;
	u16 hbp = mode->htotal - mode->hsync_end;
	u16 vactive = mode->vdisplay;
	u16 vblank = mode->vtotal - mode->vdisplay;
	u16 vfp = mode->vsync_start - mode->vdisplay;
	u16 vsw = mode->vsync_end - mode->vsync_start;
	u16 vbp = mode->vtotal - mode->vsync_end;

	val = readl(edp->regs + SUN55I_EDP_SYNC_POLARITY);
	val &= ~(SUN55I_EDP_HSYNC_POL | SUN55I_EDP_VSYNC_POL);
	if (mode->flags & DRM_MODE_FLAG_PHSYNC)
		val |= SUN55I_EDP_HSYNC_POL;
	if (mode->flags & DRM_MODE_FLAG_PVSYNC)
		val |= SUN55I_EDP_VSYNC_POL;
	writel(val, edp->regs + SUN55I_EDP_SYNC_POLARITY);

	val = readl(edp->regs + SUN55I_EDP_HACTIVE_BLANK);
	val &= ~(SUN55I_EDP_HACTIVE | SUN55I_EDP_HBLANK);
	val |= FIELD_PREP(SUN55I_EDP_HACTIVE, hactive);
	val |= FIELD_PREP(SUN55I_EDP_HBLANK, hblank);
	writel(val, edp->regs + SUN55I_EDP_HACTIVE_BLANK);

	val = readl(edp->regs + SUN55I_EDP_VACTIVE_BLANK);
	val &= ~(SUN55I_EDP_VACTIVE | SUN55I_EDP_VBLANK);
	val |= FIELD_PREP(SUN55I_EDP_VACTIVE, vactive);
	val |= FIELD_PREP(SUN55I_EDP_VBLANK, vblank);
	writel(val, edp->regs + SUN55I_EDP_VACTIVE_BLANK);

	val = readl(edp->regs + SUN55I_EDP_SYNC_START);
	val &= ~(SUN55I_EDP_HSTART | SUN55I_EDP_VSTART);
	val |= FIELD_PREP(SUN55I_EDP_HSTART, hsw + hbp);
	val |= FIELD_PREP(SUN55I_EDP_VSTART, vsw + vbp);
	writel(val, edp->regs + SUN55I_EDP_SYNC_START);

	val = FIELD_PREP(SUN55I_EDP_HSW, hsw) |
	      FIELD_PREP(SUN55I_EDP_HFP, hfp);
	writel(val, edp->regs + SUN55I_EDP_HSW_FRONT_PORCH);

	val = FIELD_PREP(SUN55I_EDP_VSW, vsw) |
	      FIELD_PREP(SUN55I_EDP_VFP, vfp);
	writel(val, edp->regs + SUN55I_EDP_VSW_FRONT_PORCH);
}

static const struct drm_display_mode *
sun55i_edp_get_adjusted_mode(struct drm_bridge *bridge,
			     struct drm_atomic_state *state)
{
	struct drm_connector *connector;
	struct drm_connector_state *conn_state;
	struct drm_crtc_state *crtc_state;

	connector = drm_atomic_get_new_connector_for_encoder(state,
							     bridge->encoder);
	if (!connector)
		return NULL;
	conn_state = drm_atomic_get_new_connector_state(state, connector);
	if (!conn_state || !conn_state->crtc)
		return NULL;
	crtc_state = drm_atomic_get_new_crtc_state(state, conn_state->crtc);
	if (!crtc_state)
		return NULL;

	return &crtc_state->adjusted_mode;
}

static void sun55i_edp_bridge_atomic_enable(struct drm_bridge *bridge,
					    struct drm_atomic_state *state)
{
	struct sun55i_edp *edp = bridge_to_sun55i_edp(bridge);
	const struct drm_display_mode *mode;
	u32 val;

	mode = sun55i_edp_get_adjusted_mode(bridge, state);
	if (mode)
		sun55i_edp_set_video_timings(edp, mode);

	if (sun55i_edp_link_train(edp))
		dev_warn(edp->dev,
			 "Link training failed, output may be unstable.\n");

	/* TODO: transfer-unit programming. */

	val = readl(edp->regs + SUN55I_EDP_VIDEO_STREAM_EN);
	val |= SUN55I_EDP_VIDEO_STREAM_EN_BIT;
	writel(val, edp->regs + SUN55I_EDP_VIDEO_STREAM_EN);
}

static void sun55i_edp_bridge_atomic_disable(struct drm_bridge *bridge,
					     struct drm_atomic_state *state)
{
	struct sun55i_edp *edp = bridge_to_sun55i_edp(bridge);
	u32 val;

	val = readl(edp->regs + SUN55I_EDP_VIDEO_STREAM_EN);
	val &= ~SUN55I_EDP_VIDEO_STREAM_EN_BIT;
	writel(val, edp->regs + SUN55I_EDP_VIDEO_STREAM_EN);

	val = readl(edp->regs + SUN55I_EDP_CAPACITY);
	val &= ~SUN55I_EDP_CAPACITY_LINK_RESET;
	writel(val, edp->regs + SUN55I_EDP_CAPACITY);
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
	struct sun55i_edp *edp = connector_to_sun55i_edp(connector);
	const struct drm_edid *drm_edid;
	int count;

	if (!edp->plugged)
		return 0;

	drm_edid = drm_edid_read_ddc(connector, &edp->aux.ddc);
	drm_edid_connector_update(connector, drm_edid);
	count = drm_edid_connector_add_modes(connector);
	drm_edid_free(drm_edid);

	return count;
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

	if (edp->vdd_supply) {
		ret = regulator_enable(edp->vdd_supply);
		if (ret)
			return ret;
	}

	if (edp->vcc_supply) {
		ret = regulator_enable(edp->vcc_supply);
		if (ret)
			goto err_disable_vdd;
	}

	ret = reset_control_deassert(edp->rst_bus);
	if (ret)
		goto err_disable_vcc;

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
err_disable_vcc:
	if (edp->vcc_supply)
		regulator_disable(edp->vcc_supply);
err_disable_vdd:
	if (edp->vdd_supply)
		regulator_disable(edp->vdd_supply);
	return ret;
}

static void sun55i_edp_hw_disable(struct sun55i_edp *edp)
{
	if (edp->clk_24m)
		clk_disable_unprepare(edp->clk_24m);
	clk_disable_unprepare(edp->clk_mod);
	clk_disable_unprepare(edp->clk_bus);
	reset_control_assert(edp->rst_bus);
	if (edp->vcc_supply)
		regulator_disable(edp->vcc_supply);
	if (edp->vdd_supply)
		regulator_disable(edp->vdd_supply);
}

/*
 * Innosilicon eDP 1.3 PLL programming table for the AUX/main link clock.
 * Values originate from the A523/A527/T527 vendor BSP. Two link rates are
 * supported: 1.62 Gbps (RBR) and 2.7 Gbps (HBR).
 */
struct sun55i_edp_pll_cfg {
	u8	prediv;
	u8	fbdiv_h4;
	u8	fbdiv_l8;
	u8	postdiv;
	u8	frac_pd;
	u8	frac_h8;
	u8	frac_m8;
	u8	frac_l8;
};

static const struct sun55i_edp_pll_cfg __maybe_unused sun55i_edp_pll_rbr = {
	.prediv = 0x2, .fbdiv_l8 = 0x87, .postdiv = 0x1, .frac_pd = 0x3,
};

static const struct sun55i_edp_pll_cfg sun55i_edp_pll_hbr = {
	.prediv = 0x2, .fbdiv_l8 = 0xe1, .postdiv = 0x1, .frac_pd = 0x3,
};

static void sun55i_edp_corepll_set(struct sun55i_edp *edp,
				   const struct sun55i_edp_pll_cfg *cfg)
{
	u32 val;

	/* power down */
	val = readl(edp->regs + SUN55I_EDP_ANA_PLL_FBDIV);
	val |= SUN55I_EDP_ANA_PLL_PD;
	writel(val, edp->regs + SUN55I_EDP_ANA_PLL_FBDIV);

	val &= ~(SUN55I_EDP_ANA_PLL_PREDIV |
		 SUN55I_EDP_ANA_PLL_FBDIV_H4 |
		 SUN55I_EDP_ANA_PLL_FBDIV_L8 |
		 SUN55I_EDP_ANA_PLL_FRAC_PD);
	val |= FIELD_PREP(SUN55I_EDP_ANA_PLL_PREDIV, cfg->prediv);
	val |= FIELD_PREP(SUN55I_EDP_ANA_PLL_FBDIV_H4, cfg->fbdiv_h4);
	val |= FIELD_PREP(SUN55I_EDP_ANA_PLL_FBDIV_L8, cfg->fbdiv_l8);
	val |= FIELD_PREP(SUN55I_EDP_ANA_PLL_FRAC_PD, cfg->frac_pd);
	writel(val, edp->regs + SUN55I_EDP_ANA_PLL_FBDIV);

	val = readl(edp->regs + SUN55I_EDP_ANA_PLL_POSDIV);
	val &= ~SUN55I_EDP_ANA_PLL_POSDIV_V;
	val |= FIELD_PREP(SUN55I_EDP_ANA_PLL_POSDIV_V, cfg->postdiv);
	writel(val, edp->regs + SUN55I_EDP_ANA_PLL_POSDIV);

	val = FIELD_PREP(SUN55I_EDP_ANA_PLL_FRAC_H8, cfg->frac_h8) |
	      FIELD_PREP(SUN55I_EDP_ANA_PLL_FRAC_M8, cfg->frac_m8) |
	      FIELD_PREP(SUN55I_EDP_ANA_PLL_FRAC_L8, cfg->frac_l8);
	writel(val, edp->regs + SUN55I_EDP_ANA_PLL_FRAC);

	/* power up */
	val = readl(edp->regs + SUN55I_EDP_ANA_PLL_FBDIV);
	val &= ~SUN55I_EDP_ANA_PLL_PD;
	writel(val, edp->regs + SUN55I_EDP_ANA_PLL_FBDIV);
}

static void sun55i_edp_aux_clock_setup(struct sun55i_edp *edp, u32 bit_mhz)
{
	u32 val;

	/* AUX clock = bit_clock / 64. bit_clock = bit_rate / 2. */
	val = readl(edp->regs + SUN55I_EDP_ANA_AUX_CLOCK);
	val &= ~SUN55I_EDP_ANA_AUX_CLOCK_16M_DIV;
	val |= FIELD_PREP(SUN55I_EDP_ANA_AUX_CLOCK_16M_DIV,
			  (bit_mhz / 8) / 8);
	writel(val, edp->regs + SUN55I_EDP_ANA_AUX_CLOCK);

	val = readl(edp->regs + SUN55I_EDP_TX_MAINSEL);
	val &= ~(SUN55I_EDP_TX_MAINSEL_LO | SUN55I_EDP_TX_MAINSEL_HI);
	val |= FIELD_PREP(SUN55I_EDP_TX_MAINSEL_LO, 0x14);
	val |= FIELD_PREP(SUN55I_EDP_TX_MAINSEL_HI, 0x14);
	writel(val, edp->regs + SUN55I_EDP_TX_MAINSEL);

	val = readl(edp->regs + SUN55I_EDP_TX_POSTSEL);
	val &= ~(SUN55I_EDP_TX_POSTSEL_LO | SUN55I_EDP_TX_POSTSEL_HI);
	val |= FIELD_PREP(SUN55I_EDP_TX_POSTSEL_LO, 0x14);
	val |= FIELD_PREP(SUN55I_EDP_TX_POSTSEL_HI, 0x14);
	writel(val, edp->regs + SUN55I_EDP_TX_POSTSEL);

	val = readl(edp->regs + SUN55I_EDP_TX_PRESEL);
	val &= ~SUN55I_EDP_TX_PRESEL_LO;
	writel(val, edp->regs + SUN55I_EDP_TX_PRESEL);

	/* Maximise AUX channel swing for sink compatibility. */
	val = readl(edp->regs + SUN55I_EDP_AUX_ISEL_MAINSET);
	val &= ~(SUN55I_EDP_AUX_ISEL_MASK | SUN55I_EDP_AUX_MAINSET_MASK);
	val |= FIELD_PREP(SUN55I_EDP_AUX_ISEL_MASK, 0xf);
	val |= FIELD_PREP(SUN55I_EDP_AUX_MAINSET_MASK, 0xf);
	writel(val, edp->regs + SUN55I_EDP_AUX_ISEL_MAINSET);
}

static void sun55i_edp_controller_init(struct sun55i_edp *edp)
{
	bool is_edp = edp->variant->connector_type == DRM_MODE_CONNECTOR_eDP;
	u32 val;

	/* Controller soft-reset. */
	writel(SUN55I_EDP_RESET_CONTROLLER, edp->regs + SUN55I_EDP_RESET);
	usleep_range(10, 20);
	writel(0, edp->regs + SUN55I_EDP_RESET);

	/* Pick eDP vs DP behaviour. */
	val = readl(edp->regs + SUN55I_EDP_HPD_SCALE);
	if (is_edp)
		val |= SUN55I_EDP_HPD_SCALE_EDP_MODE;
	else
		val &= ~SUN55I_EDP_HPD_SCALE_EDP_MODE;
	writel(val, edp->regs + SUN55I_EDP_HPD_SCALE);

	/* Current-mode drive: set TX_PRESEL[27:24] = 0xf. */
	val = readl(edp->regs + SUN55I_EDP_TX_PRESEL);
	val &= ~SUN55I_EDP_TX_PRESEL_MODE;
	val |= FIELD_PREP(SUN55I_EDP_TX_PRESEL_MODE, 0xf);
	writel(val, edp->regs + SUN55I_EDP_TX_PRESEL);

	/* Termination resistor calibration enable. */
	val = readl(edp->regs + SUN55I_EDP_RES1000_CFG);
	val &= ~SUN55I_EDP_RES1000_CFG_VAL;
	val |= SUN55I_EDP_RES1000_CFG_EN;
	writel(val, edp->regs + SUN55I_EDP_RES1000_CFG);

	/*
	 * Bring up the AUX/main link clock at HBR (2.7 Gbps / 1350 MHz)
	 * before talking to the sink. The link rate can be re-negotiated
	 * during link training later.
	 */
	sun55i_edp_aux_clock_setup(edp, 1350);
	sun55i_edp_corepll_set(edp, &sun55i_edp_pll_hbr);
	usleep_range(500, 1000);
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

static void sun55i_edp_read_sink_caps(struct sun55i_edp *edp)
{
	int ret;

	ret = drm_dp_read_dpcd_caps(&edp->aux, edp->dpcd);
	if (ret < 0) {
		dev_dbg(edp->dev, "Failed to read DPCD: %d\n", ret);
		memset(edp->dpcd, 0, sizeof(edp->dpcd));
		edp->max_lane_count = 0;
		edp->max_link_rate = 0;
		return;
	}

	edp->max_lane_count = drm_dp_max_lane_count(edp->dpcd);
	edp->max_link_rate = drm_dp_max_link_rate(edp->dpcd) / 1000;
	dev_dbg(edp->dev, "Sink: DPCD rev 0x%02x, %u lanes, max %u kHz\n",
		edp->dpcd[DP_DPCD_REV], edp->max_lane_count,
		drm_dp_max_link_rate(edp->dpcd));
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
		sun55i_edp_read_sink_caps(edp);
		changed = true;
	}

	if (plug & SUN55I_EDP_HPD_PLUG_OUT) {
		writel(SUN55I_EDP_HPD_PLUG_OUT,
		       edp->regs + SUN55I_EDP_HPD_PLUG);
		edp->plugged = false;
		memset(edp->dpcd, 0, sizeof(edp->dpcd));
		edp->max_lane_count = 0;
		edp->max_link_rate = 0;
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

	sun55i_edp_controller_init(edp);
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

	if (edp->plugged)
		sun55i_edp_read_sink_caps(edp);

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
