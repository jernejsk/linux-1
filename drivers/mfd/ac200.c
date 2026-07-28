// SPDX-License-Identifier: GPL-2.0-only
/*
 * MFD core driver for X-Powers' AC200 IC
 *
 * The AC200 is a chip which is co-packaged with Allwinner H6 SoC and
 * includes analog audio codec, analog TV encoder, ethernet PHY, eFuse
 * and RTC.
 *
 * Copyright (c) 2019 Jernej Skrabec <jernej.skrabec@gmail.com>
 *
 * Based on AC100 driver with following copyrights:
 * Copyright (2016) Chen-Yu Tsai
 */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#include <dt-bindings/mfd/x-powers,ac200.h>

struct ac200_dev {
	struct clk	*clk;
	struct regmap	*regmap;
};

static const char * const ac200_supplies[] = {
	"ac-ldoin",
	"ephy-vcc",
	"rtc-vcc",
	"tv-vcc",
};

#define AC200_SYS_VERSION	0x0000
#define AC200_SYS_VERSION_PACKAGE	GENMASK(15, 14)
#define AC200_SYS_VERSION_CHIP		GENMASK(11, 0)
#define AC200_SYS_CONTROL	0x0002
#define AC200_SYS_IRQ_ENABLE	0x0004
#define AC200_SYS_IRQ_INTB_EN		BIT(15)
#define AC200_SYS_IRQ_INTB_ACTIVE_HIGH	BIT(14)
#define AC200_SYS_IRQ_RTC		BIT(12)
#define AC200_SYS_IRQ_EPHY		BIT(8)
#define AC200_SYS_IRQ_TVE		BIT(4)
#define AC200_SYS_IRQ_STATUS	0x0006
#define AC200_SYS_BG_CTL	0x0050

/* interface register (can be accessed from any page) */
#define AC200_TWI_REG_ADDR_H	0xFE

#define AC200_MAX_REG		0xA1F2

static const struct regmap_range_cfg ac200_range_cfg[] = {
	{
		.range_max = AC200_MAX_REG,
		.selector_reg = AC200_TWI_REG_ADDR_H,
		.selector_mask = 0xff,
		.selector_shift = 0,
		.window_start = 0,
		.window_len = 256,
	}
};

/*
 * Only the paging register is cached. Every sub-block of the AC200 has its
 * own reset bit (SYS_AUDIO_CTL0, SYS_EPHY_CTL0, SYS_TVE_CTL0 and the per
 * module resets in the audio block's SYS_MOD_RST), and asserting one of
 * those returns that block's registers to their defaults behind regmap's
 * back. Caching them would hand out stale values after any DAPM power
 * down or PHY reset, so everything except the paging register is marked
 * volatile.
 *
 * Caching just the paging register is still worth it: without it every
 * single register access costs three I2C transfers, because regmap
 * open codes a read-modify-write of the selector (see _regmap_select_page()
 * and _regmap_update_bits()). With the selector cached, an access to a
 * register on the current page is a single transfer.
 */
static bool ac200_volatile_reg(struct device *dev, unsigned int reg)
{
	return reg != AC200_TWI_REG_ADDR_H;
}

static const struct regmap_config ac200_regmap_config = {
	.name		= "AC200",
	.reg_bits	= 8,
	.reg_stride	= 2,
	.val_bits	= 16,
	.ranges		= ac200_range_cfg,
	.num_ranges	= ARRAY_SIZE(ac200_range_cfg),
	.max_register	= AC200_MAX_REG,
	.volatile_reg	= ac200_volatile_reg,
	.cache_type	= REGCACHE_MAPLE,
};

static const struct regmap_irq ac200_irqs[] = {
	REGMAP_IRQ_REG(AC200_IRQ_TVE, 0, AC200_SYS_IRQ_TVE),
	REGMAP_IRQ_REG(AC200_IRQ_EPHY, 0, AC200_SYS_IRQ_EPHY),
	REGMAP_IRQ_REG(AC200_IRQ_RTC, 0, AC200_SYS_IRQ_RTC),
};

/*
 * SYS_IRQ_ENABLE is an enable register rather than a mask register, hence
 * unmask_base. It also holds the INTB output enable and polarity bits, but
 * regmap-irq only ever touches the bits covered by the irq masks, so those
 * survive.
 *
 * SYS_IRQ_STATUS is read only and follows the state of the source block, so
 * there is nothing to acknowledge here: an interrupt is cleared by servicing
 * it in the sub-block that raised it.
 */
static const struct regmap_irq_chip ac200_irq_chip = {
	.name		= "ac200",
	.status_base	= AC200_SYS_IRQ_STATUS,
	.unmask_base	= AC200_SYS_IRQ_ENABLE,
	.num_regs	= 1,
	.irqs		= ac200_irqs,
	.num_irqs	= ARRAY_SIZE(ac200_irqs),
};

static const struct mfd_cell ac200_cells[] = {
	{
		.name		= "ac200-codec",
		.of_compatible	= "x-powers,ac200-codec",
	}, {
		.name		= "ac200-ephy-ctl",
		.of_compatible	= "x-powers,ac200-ephy-ctl",
	}, {
		.name		= "ac200-tve",
		.of_compatible	= "x-powers,ac200-tve",
	},
};

static void ac200_reset(void *data)
{
	struct ac200_dev *ac200 = data;

	regmap_write(ac200->regmap, AC200_SYS_CONTROL, 0);
}

static int ac200_init_irq(struct ac200_dev *ac200, int irq)
{
	struct device *dev = regmap_get_device(ac200->regmap);
	struct regmap_irq_chip_data *data;
	u16 val = AC200_SYS_IRQ_INTB_EN;
	u32 trigger;
	int ret;

	trigger = irq_get_trigger_type(irq);
	switch (trigger) {
	case IRQ_TYPE_LEVEL_HIGH:
		val |= AC200_SYS_IRQ_INTB_ACTIVE_HIGH;
		break;
	case IRQ_TYPE_NONE:
	case IRQ_TYPE_LEVEL_LOW:
		break;
	default:
		return dev_err_probe(dev, -EINVAL,
				     "INTB is level triggered, not type %u\n",
				     trigger);
	}

	ret = regmap_update_bits(ac200->regmap, AC200_SYS_IRQ_ENABLE,
				 AC200_SYS_IRQ_INTB_EN |
				 AC200_SYS_IRQ_INTB_ACTIVE_HIGH, val);
	if (ret)
		return ret;

	ret = devm_regmap_add_irq_chip(dev, ac200->regmap, irq, IRQF_ONESHOT,
				       0, &ac200_irq_chip, &data);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to add IRQ chip\n");

	return 0;
}

static int ac200_i2c_probe(struct i2c_client *i2c)
{
	struct device *dev = &i2c->dev;
	struct ac200_dev *ac200;
	unsigned int val;
	u16 bgval;
	int ret;

	ac200 = devm_kzalloc(dev, sizeof(*ac200), GFP_KERNEL);
	if (!ac200)
		return -ENOMEM;

	ret = devm_regulator_bulk_get_enable(dev, ARRAY_SIZE(ac200_supplies),
					     ac200_supplies);
	if (ret)
		return dev_err_probe(dev, ret, "Can't enable supplies\n");

	ac200->clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(ac200->clk))
		return dev_err_probe(dev, PTR_ERR(ac200->clk),
				     "Can't obtain the clock\n");

	ac200->regmap = devm_regmap_init_i2c(i2c, &ac200_regmap_config);
	if (IS_ERR(ac200->regmap))
		return dev_err_probe(dev, PTR_ERR(ac200->regmap),
				     "Regmap init failed\n");

	/*
	 * The bandgap trim only matters for the TV encoder, and not every
	 * platform stores it in the SoC eFuse. Carry on without it.
	 */
	ret = nvmem_cell_read_u16(dev, "bandgap", &bgval);
	if (ret == -ENOENT) {
		dev_dbg(dev, "No bandgap calibration data, using defaults\n");
		bgval = 0;
	} else if (ret) {
		return dev_err_probe(dev, ret, "Unable to read bandgap data\n");
	}

	/*
	 * There is no documentation on how long we have to wait before
	 * executing first operation. Vendor driver sleeps for 40 ms.
	 */
	msleep(40);

	/* Doubles as a check that the chip is alive and out of power on. */
	ret = regmap_read(ac200->regmap, AC200_SYS_VERSION, &val);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to read chip version\n");

	dev_info(dev, "AC200 rev %lx in package %lu\n",
		 FIELD_GET(AC200_SYS_VERSION_CHIP, val),
		 FIELD_GET(AC200_SYS_VERSION_PACKAGE, val));

	ret = regmap_write(ac200->regmap, AC200_SYS_CONTROL, 0);
	if (ret)
		return ret;

	ret = regmap_write(ac200->regmap, AC200_SYS_CONTROL, 1);
	if (ret)
		return ret;

	/*
	 * The datasheet does not say how long the chip needs after the reset
	 * is released. The vendor driver sleeps around this point too, so
	 * give it some slack before touching anything else.
	 */
	usleep_range(1000, 2000);

	/*
	 * Register the reset action before the children, so that devres
	 * unwinding tears the children down first and only then puts the
	 * chip back into reset.
	 */
	ret = devm_add_action_or_reset(dev, ac200_reset, ac200);
	if (ret)
		return ret;

	if (bgval) {
		/* bandgap register is not documented */
		ret = regmap_write(ac200->regmap, AC200_SYS_BG_CTL,
				   0x8280 | bgval);
		if (ret)
			return ret;
	}

	if (i2c->irq > 0) {
		ret = ac200_init_irq(ac200, i2c->irq);
		if (ret)
			return ret;
	}

	ret = devm_mfd_add_devices(dev, PLATFORM_DEVID_NONE, ac200_cells,
				   ARRAY_SIZE(ac200_cells), NULL, 0, NULL);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to add MFD devices\n");

	return 0;
}

static const struct i2c_device_id ac200_ids[] = {
	{ "ac200", },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ac200_ids);

static const struct of_device_id ac200_of_match[] = {
	{ .compatible = "x-powers,ac200" },
	{ }
};
MODULE_DEVICE_TABLE(of, ac200_of_match);

static struct i2c_driver ac200_i2c_driver = {
	.driver = {
		.name	= "ac200",
		.of_match_table	= ac200_of_match,
	},
	.probe	= ac200_i2c_probe,
	.id_table = ac200_ids,
};
module_i2c_driver(ac200_i2c_driver);

MODULE_DESCRIPTION("MFD core driver for AC200");
MODULE_AUTHOR("Jernej Skrabec <jernej.skrabec@gmail.com>");
MODULE_LICENSE("GPL");
