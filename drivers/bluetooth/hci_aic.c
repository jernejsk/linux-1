// SPDX-License-Identifier: GPL-2.0-only
/*
 * Bluetooth support for AICSemi AIC8800 series controllers attached over UART.
 *
 * The controller shares one firmware image with the wireless part of the same
 * package.  That image is loaded over SDIO by the wireless driver, together
 * with the Bluetooth patches, so there is nothing left to download here and the
 * controller comes up speaking H:4 at a fixed rate.
 *
 * Copyright (C) 2026 Jernej Skrabec <jernej.skrabec@gmail.com>
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm.h>
#include <linux/regulator/consumer.h>
#include <linux/serdev.h>
#include <linux/skbuff.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#include "hci_uart.h"

/* Rate the firmware configures its UART for. */
#define AIC_BT_BAUD		1500000

/* Time the controller needs after its reset is released. */
#define AIC_BT_RESET_DELAY_MS	100

struct aic_bt {
	struct hci_uart hu;
	struct gpio_desc *reset;
	struct gpio_desc *device_wake;
	int host_wake_irq;
	struct sk_buff *rx_skb;
	struct sk_buff_head txq;
};

static int aic_bt_open(struct hci_uart *hu)
{
	struct aic_bt *aic = hu->priv;

	skb_queue_head_init(&aic->txq);

	if (aic->device_wake)
		gpiod_set_value_cansleep(aic->device_wake, 1);

	return 0;
}

static int aic_bt_close(struct hci_uart *hu)
{
	struct aic_bt *aic = hu->priv;

	if (aic->device_wake)
		gpiod_set_value_cansleep(aic->device_wake, 0);

	skb_queue_purge(&aic->txq);
	kfree_skb(aic->rx_skb);
	aic->rx_skb = NULL;

	return 0;
}

static int aic_bt_flush(struct hci_uart *hu)
{
	struct aic_bt *aic = hu->priv;

	skb_queue_purge(&aic->txq);

	return 0;
}

static const struct h4_recv_pkt aic_bt_recv_pkts[] = {
	{ H4_RECV_ACL,		.recv = hci_recv_frame },
	{ H4_RECV_SCO,		.recv = hci_recv_frame },
	{ H4_RECV_EVENT,	.recv = hci_recv_frame },
	{ H4_RECV_ISO,		.recv = hci_recv_frame },
};

static int aic_bt_recv(struct hci_uart *hu, const void *data, int count)
{
	struct aic_bt *aic = hu->priv;

	aic->rx_skb = h4_recv_buf(hu, aic->rx_skb, data, count,
				  aic_bt_recv_pkts,
				  ARRAY_SIZE(aic_bt_recv_pkts));
	if (IS_ERR(aic->rx_skb)) {
		int err = PTR_ERR(aic->rx_skb);

		bt_dev_err(hu->hdev, "corrupted receive stream (%d)", err);
		aic->rx_skb = NULL;

		return err;
	}

	return count;
}

static int aic_bt_enqueue(struct hci_uart *hu, struct sk_buff *skb)
{
	struct aic_bt *aic = hu->priv;

	skb_queue_tail(&aic->txq, skb);

	return 0;
}

static struct sk_buff *aic_bt_dequeue(struct hci_uart *hu)
{
	struct aic_bt *aic = hu->priv;
	struct sk_buff *skb = skb_dequeue(&aic->txq);

	if (skb)
		memcpy(skb_push(skb, 1), &hci_skb_pkt_type(skb), 1);

	return skb;
}

static const struct hci_uart_proto aic_bt_proto = {
	.id		= HCI_UART_AIC,
	.name		= "AIC",
	.init_speed	= AIC_BT_BAUD,
	.oper_speed	= AIC_BT_BAUD,
	.open		= aic_bt_open,
	.close		= aic_bt_close,
	.flush		= aic_bt_flush,
	.recv		= aic_bt_recv,
	.enqueue	= aic_bt_enqueue,
	.dequeue	= aic_bt_dequeue,
};

/* The line is only used to wake the system, the controller repeats itself. */
static irqreturn_t aic_bt_host_wake_isr(int irq, void *data)
{
	return IRQ_HANDLED;
}

static int aic_bt_host_wake_init(struct aic_bt *aic, struct device *dev)
{
	struct gpio_desc *gpio;
	int irq, ret;

	gpio = devm_gpiod_get_optional(dev, "host-wakeup", GPIOD_IN);
	if (IS_ERR(gpio))
		return PTR_ERR(gpio);
	if (!gpio)
		return 0;

	irq = gpiod_to_irq(gpio);
	if (irq < 0)
		return 0;

	ret = devm_request_irq(dev, irq, aic_bt_host_wake_isr,
			       IRQF_TRIGGER_RISING | IRQF_NO_AUTOEN,
			       "aic8800-bt-wake", aic);
	if (ret)
		return ret;

	aic->host_wake_irq = irq;
	device_set_wakeup_capable(dev, true);

	return 0;
}

static int aic_bt_probe(struct serdev_device *serdev)
{
	struct device *dev = &serdev->dev;
	struct aic_bt *aic;
	u32 speed = AIC_BT_BAUD;
	int ret;

	aic = devm_kzalloc(dev, sizeof(*aic), GFP_KERNEL);
	if (!aic)
		return -ENOMEM;

	aic->hu.priv = aic;
	aic->hu.serdev = serdev;
	serdev_device_set_drvdata(serdev, aic);

	ret = devm_regulator_get_enable_optional(dev, "vddio");
	if (ret && ret != -ENODEV)
		return dev_err_probe(dev, ret,
				     "failed to enable the io supply\n");

	aic->device_wake = devm_gpiod_get_optional(dev, "device-wakeup",
						   GPIOD_OUT_LOW);
	if (IS_ERR(aic->device_wake))
		return dev_err_probe(dev, PTR_ERR(aic->device_wake),
				     "failed to get the device wake line\n");

	ret = aic_bt_host_wake_init(aic, dev);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to set up the host wake line\n");

	/*
	 * The controller has no state to keep across a reset, so it is simply
	 * released here and held in reset again when the driver goes away.
	 */
	aic->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(aic->reset))
		return dev_err_probe(dev, PTR_ERR(aic->reset),
				     "failed to get the reset line\n");
	if (aic->reset)
		msleep(AIC_BT_RESET_DELAY_MS);

	of_property_read_u32(dev->of_node, "max-speed", &speed);
	hci_uart_set_speeds(&aic->hu, speed, speed);

	return hci_uart_register_device(&aic->hu, &aic_bt_proto);
}

static void aic_bt_remove(struct serdev_device *serdev)
{
	struct aic_bt *aic = serdev_device_get_drvdata(serdev);

	hci_uart_unregister_device(&aic->hu);

	if (aic->reset)
		gpiod_set_value_cansleep(aic->reset, 1);
}

static int aic_bt_suspend(struct device *dev)
{
	struct aic_bt *aic = dev_get_drvdata(dev);

	if (aic->host_wake_irq > 0 && device_may_wakeup(dev)) {
		enable_irq(aic->host_wake_irq);
		enable_irq_wake(aic->host_wake_irq);
	}

	return 0;
}

static int aic_bt_resume(struct device *dev)
{
	struct aic_bt *aic = dev_get_drvdata(dev);

	if (aic->host_wake_irq > 0 && device_may_wakeup(dev)) {
		disable_irq_wake(aic->host_wake_irq);
		disable_irq(aic->host_wake_irq);
	}

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(aic_bt_pm_ops, aic_bt_suspend, aic_bt_resume);

static const struct of_device_id aic_bt_of_match[] = {
	{ .compatible = "aicsemi,aic8800d80-bt" },
	{ }
};
MODULE_DEVICE_TABLE(of, aic_bt_of_match);

static struct serdev_device_driver aic_bt_driver = {
	.driver = {
		.name = "hci-aic",
		.of_match_table = aic_bt_of_match,
		.pm = pm_sleep_ptr(&aic_bt_pm_ops),
	},
	.probe = aic_bt_probe,
	.remove = aic_bt_remove,
};

int __init aic_init(void)
{
	serdev_device_driver_register(&aic_bt_driver);

	return hci_uart_register_proto(&aic_bt_proto);
}

int __exit aic_deinit(void)
{
	serdev_device_driver_unregister(&aic_bt_driver);

	return hci_uart_unregister_proto(&aic_bt_proto);
}
