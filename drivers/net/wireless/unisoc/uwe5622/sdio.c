// SPDX-License-Identifier: GPL-2.0-only

#include <linux/align.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/mmc/host.h>
#include <linux/mmc/sdio.h>
#include <linux/mmc/sdio_func.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/pm.h>
#include <linux/pm_wakeirq.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/unaligned.h>

#include "core.h"

#define UWE5622_SDIO_BLOCK_SIZE		840
#define UWE5622_SDIO_PACKET_ADDR	0x20
#define UWE5622_SDIO_DIRECT_ADDR	0x0f
#define UWE5622_SDIO_TARGET_ADDR0	0x15c

#define UWE5622_FW_LOAD_ADDR		0x40500000
#define UWE5622_FW_MAX_SIZE		0x000e7400
#define UWE5622_FW_CHUNK_SIZE		SZ_32K
#define UWE5622_CP_RESET_REG		0x40088288
#define UWE5622_CP_RESET_BIT		BIT(0)

#define UWE5622_SYNC_ADDR		0x405e73b0
#define UWE5622_SYNC_SDIO_CONFIG	(UWE5622_SYNC_ADDR + 0x1c)
#define UWE5622_SYNC_BIND_DATA		(UWE5622_SYNC_ADDR + 0x40)
#define UWE5622_SYNC_INFO_SIZE		0x50
#define UWE5622_SYNC_CAL_WAITING	0xf0f0f0f1
#define UWE5622_SYNC_CAL_WRITE_DONE	0xf0f0f0f2
#define UWE5622_SYNC_VERIFY_WAITING	0xf0f0f0f6
#define UWE5622_SYNC_VERIFY_WRITE_DONE	0xf0f0f0f7
#define UWE5622_SYNC_ALL_FINISHED	0xf0f0f0ff

/* SDMA RX, 840-byte blocks, and an in-band DATA1 interrupt. */
#define UWE5622_SDIO_CONFIG		(BIT(0) | BIT(4) | BIT(11))

#define UWE5622_PUH_PAD		GENMASK(5, 0)
#define UWE5622_PUH_CHECKSUM		BIT(6)
#define UWE5622_PUH_LENGTH		GENMASK(22, 7)
#define UWE5622_PUH_EOF		BIT(23)
#define UWE5622_PUH_SUBTYPE		GENMASK(27, 24)
#define UWE5622_PUH_TYPE		GENMASK(31, 28)

#define UWE5622_RX_FIRST_SIZE		(2 * UWE5622_SDIO_BLOCK_SIZE)
#define UWE5622_RX_MAX_SIZE		(156 * UWE5622_SDIO_BLOCK_SIZE)
#define UWE5622_RX_CHANNEL_BASE		12

struct uwe5622_sdio {
	struct sdio_func *func;
	struct uwe5622 wcn;
	struct sk_buff_head tx_queue;
	struct work_struct tx_work;
	u8 *rx_buf;
	bool enabled;
	bool irq_claimed;
	bool wake_irq_set;
};

static irqreturn_t uwe5622_host_wake_irq(int irq, void *data)
{
	struct uwe5622_sdio *sdio = data;

	pm_wakeup_event(&sdio->func->dev, 0);
	return IRQ_HANDLED;
}

static int uwe5622_sdio_set_target(struct uwe5622_sdio *sdio, u32 address)
{
	int i;
	int ret = 0;

	for (i = 0; i < sizeof(address); i++) {
		sdio_f0_writeb(sdio->func, address >> (i * 8),
			       UWE5622_SDIO_TARGET_ADDR0 + i, &ret);
		if (ret)
			break;
	}

	return ret;
}

/* The target-side direct window advances after each transfer at address 0x0f. */
static int uwe5622_sdio_direct_write_locked(struct uwe5622_sdio *sdio,
					     u32 address, const void *data,
					     size_t length)
{
	const u8 *source = data;
	size_t chunk;
	int ret;

	ret = uwe5622_sdio_set_target(sdio, address);
	if (ret)
		return ret;

	while (length) {
		chunk = min_t(size_t, length, sdio->func->cur_blksize);
		ret = sdio_memcpy_toio(sdio->func, UWE5622_SDIO_DIRECT_ADDR,
				       (void *)source, chunk);
		if (ret)
			return ret;
		source += chunk;
		length -= chunk;
	}

	return 0;
}

static int uwe5622_sdio_direct_read_locked(struct uwe5622_sdio *sdio,
					    u32 address, void *data,
					    size_t length)
{
	u8 *destination = data;
	size_t chunk;
	int ret;

	ret = uwe5622_sdio_set_target(sdio, address);
	if (ret)
		return ret;

	while (length) {
		chunk = min_t(size_t, length, sdio->func->cur_blksize);
		ret = sdio_memcpy_fromio(sdio->func, destination,
					 UWE5622_SDIO_DIRECT_ADDR, chunk);
		if (ret)
			return ret;
		destination += chunk;
		length -= chunk;
	}

	return 0;
}

static int uwe5622_sdio_write_u32_locked(struct uwe5622_sdio *sdio,
					  u32 address, u32 value)
{
	__le32 wire_value = cpu_to_le32(value);

	return uwe5622_sdio_direct_write_locked(sdio, address, &wire_value,
						 sizeof(wire_value));
}

static int uwe5622_sdio_read_u32_locked(struct uwe5622_sdio *sdio,
					 u32 address, u32 *value)
{
	__le32 wire_value;
	int ret;

	ret = uwe5622_sdio_direct_read_locked(sdio, address, &wire_value,
					      sizeof(wire_value));
	if (!ret)
		*value = le32_to_cpu(wire_value);

	return ret;
}

static int uwe5622_sdio_download_firmware(struct uwe5622_sdio *sdio,
					   const struct firmware *fw)
{
	size_t offset = 0;
	size_t chunk;
	int ret;

	if (!fw->size || fw->size > UWE5622_FW_MAX_SIZE)
		return dev_err_probe(&sdio->func->dev, -EINVAL,
				     "invalid raw firmware size %zu\n", fw->size);
	if (fw->size >= 4 && (!memcmp(fw->data, "WCNM", 4) ||
			     !memcmp(fw->data, "WCNE", 4)))
		return dev_err_probe(&sdio->func->dev, -EINVAL,
				     "packed firmware images are not supported yet\n");

	while (offset < fw->size) {
		chunk = min_t(size_t, fw->size - offset, UWE5622_FW_CHUNK_SIZE);
		ret = uwe5622_sdio_direct_write_locked(sdio,
						 UWE5622_FW_LOAD_ADDR + offset,
						 fw->data + offset, chunk);
		if (ret)
			return ret;
		offset += chunk;
	}

	return 0;
}

static int uwe5622_sdio_release_cpu(struct uwe5622_sdio *sdio)
{
	u32 reset;
	int ret;

	ret = uwe5622_sdio_read_u32_locked(sdio, UWE5622_CP_RESET_REG,
					   &reset);
	if (ret)
		return ret;

	return uwe5622_sdio_write_u32_locked(sdio, UWE5622_CP_RESET_REG,
					      reset & ~UWE5622_CP_RESET_BIT);
}

static int uwe5622_sdio_answer_bind(struct uwe5622_sdio *sdio,
					     const u8 challenge[16])
{
	u8 response[16];
	int ret;

	ret = uwe5622_bind_verify(challenge, response);
	if (ret)
		return dev_err_probe(&sdio->func->dev, ret,
				     "firmware bind challenge is malformed\n");

	ret = uwe5622_sdio_direct_write_locked(sdio, UWE5622_SYNC_BIND_DATA,
					       response, sizeof(response));
	if (ret)
		return ret;

	return uwe5622_sdio_write_u32_locked(sdio, UWE5622_SYNC_ADDR,
					      UWE5622_SYNC_VERIFY_WRITE_DONE);
}

static int uwe5622_sdio_wait_ready(struct uwe5622_sdio *sdio)
{
	u8 sync[UWE5622_SYNC_INFO_SIZE];
	unsigned long deadline = jiffies + msecs_to_jiffies(5000);
	u32 status;
	int ret;

	do {
		ret = uwe5622_sdio_direct_read_locked(sdio, UWE5622_SYNC_ADDR,
						      sync, sizeof(sync));
		if (ret)
			return ret;

		status = get_unaligned_le32(sync);
		switch (status) {
		case UWE5622_SYNC_ALL_FINISHED:
			return 0;
		case UWE5622_SYNC_CAL_WAITING:
			ret = uwe5622_sdio_write_u32_locked(sdio,
					UWE5622_SYNC_SDIO_CONFIG,
					UWE5622_SDIO_CONFIG);
			if (!ret)
				ret = uwe5622_sdio_write_u32_locked(sdio,
						UWE5622_SYNC_ADDR,
						UWE5622_SYNC_CAL_WRITE_DONE);
			if (ret)
				return ret;
			break;
		case UWE5622_SYNC_VERIFY_WAITING:
			ret = uwe5622_sdio_answer_bind(sdio, sync + 0x40);
			if (ret)
				return ret;
			break;
		default:
			break;
		}

		usleep_range(18000, 22000);
	} while (time_before(jiffies, deadline));

	return dev_err_probe(&sdio->func->dev, -ETIMEDOUT,
			     "firmware ready timeout, sync status %#08x\n", status);
}

static size_t uwe5622_sdio_rx_size(u32 pending)
{
	size_t overhead;
	size_t length;

	if (!pending)
		return UWE5622_RX_FIRST_SIZE;

	overhead = 8 * ((pending >> 10) + 1) + 64;
	length = roundup(pending + overhead, UWE5622_SDIO_BLOCK_SIZE);
	return min_t(size_t, length, UWE5622_RX_MAX_SIZE);
}

static int uwe5622_sdio_queue_rx(struct uwe5622_sdio *sdio, size_t read_len,
				  u32 valid_len, struct sk_buff_head *queue)
{
	size_t payload_total = 0;
	size_t limit = read_len - 8;
	size_t offset = 0;
	bool eof = false;

	while (offset + sizeof(__le32) <= limit) {
		struct sk_buff *skb;
		u32 header = get_unaligned_le32(sdio->rx_buf + offset);
		u32 record_len;
		u16 payload_len;
		u8 channel;

		if (header & UWE5622_PUH_EOF) {
			eof = true;
			break;
		}
		if (FIELD_GET(UWE5622_PUH_TYPE, header) != 0)
			return -EPROTO;
		if (header & UWE5622_PUH_CHECKSUM)
			return -EOPNOTSUPP;

		payload_len = FIELD_GET(UWE5622_PUH_LENGTH, header);
		record_len = sizeof(__le32) + ALIGN(payload_len, 4);
		if (!payload_len || record_len > limit - offset)
			return -EPROTO;

		channel = UWE5622_RX_CHANNEL_BASE +
			  FIELD_GET(UWE5622_PUH_SUBTYPE, header);
		skb = alloc_skb(payload_len, GFP_KERNEL);
		if (!skb)
			return -ENOMEM;
		skb_put_data(skb, sdio->rx_buf + offset + sizeof(__le32),
			     payload_len);
		skb->cb[0] = channel;
		__skb_queue_tail(queue, skb);

		payload_total += payload_len;
		offset += record_len;
	}

	if (!eof || payload_total < valid_len)
		return -EPROTO;

	return 0;
}

static void uwe5622_sdio_irq(struct sdio_func *func)
{
	struct uwe5622_sdio *sdio = sdio_get_drvdata(func);
	struct sk_buff_head queue;
	struct sk_buff *skb;
	size_t read_len = UWE5622_RX_FIRST_SIZE;
	u32 pending;
	u32 valid_len;
	int ret = 0;
	int err = 0;

	__skb_queue_head_init(&queue);
	sdio_claim_host(func);
	sdio_f0_readb(func, SDIO_CCCR_INTx, &err);
	if (err) {
		ret = err;
		goto out_release;
	}

	do {
		ret = sdio_readsb(func, sdio->rx_buf,
				  UWE5622_SDIO_PACKET_ADDR, read_len);
		if (ret)
			break;

		valid_len = get_unaligned_le32(sdio->rx_buf + read_len - 8);
		pending = get_unaligned_le32(sdio->rx_buf + read_len - 4);
		ret = uwe5622_sdio_queue_rx(sdio, read_len, valid_len, &queue);
		if (ret)
			break;
		read_len = uwe5622_sdio_rx_size(pending);
	} while (pending);

out_release:
	sdio_release_host(func);
	if (ret)
		dev_err_ratelimited(&func->dev, "RX transfer failed: %d\n", ret);

	while ((skb = __skb_dequeue(&queue)))
		uwe5622_core_rx(&sdio->wcn, skb->cb[0], skb);
}

static void uwe5622_sdio_tx_work(struct work_struct *work)
{
	struct uwe5622_sdio *sdio = container_of(work, struct uwe5622_sdio,
						 tx_work);
	struct sk_buff *skb;
	int ret;

	while ((skb = skb_dequeue(&sdio->tx_queue))) {
		sdio_claim_host(sdio->func);
		ret = sdio_writesb(sdio->func, UWE5622_SDIO_PACKET_ADDR,
				   skb->data, skb->len);
		sdio_release_host(sdio->func);
		if (ret)
			dev_err_ratelimited(&sdio->func->dev,
					    "TX transfer failed: %d\n", ret);
		kfree_skb(skb);
	}
}

static int uwe5622_sdio_start(struct uwe5622 *wcn, const struct firmware *fw)
{
	struct uwe5622_sdio *sdio = wcn->bus_priv;
	int ret;

	sdio_claim_host(sdio->func);
	ret = sdio_enable_func(sdio->func);
	if (ret)
		goto out_release;
	sdio->enabled = true;

	ret = sdio_set_block_size(sdio->func, UWE5622_SDIO_BLOCK_SIZE);
	if (ret)
		goto out_disable;

	ret = uwe5622_sdio_download_firmware(sdio, fw);
	if (ret)
		goto out_disable;
	ret = uwe5622_sdio_release_cpu(sdio);
	if (ret)
		goto out_disable;
	ret = uwe5622_sdio_wait_ready(sdio);
	if (ret)
		goto out_disable;

	ret = sdio_claim_irq(sdio->func, uwe5622_sdio_irq);
	if (ret)
		goto out_disable;
	sdio->irq_claimed = true;
	sdio_release_host(sdio->func);

	dev_info(&sdio->func->dev, "firmware is ready\n");
	return 0;

out_disable:
	sdio_disable_func(sdio->func);
	sdio->enabled = false;
out_release:
	sdio_release_host(sdio->func);
	return ret;
}

static void uwe5622_sdio_stop(struct uwe5622 *wcn)
{
	struct uwe5622_sdio *sdio = wcn->bus_priv;
	u32 reset;
	int ret;

	cancel_work_sync(&sdio->tx_work);
	skb_queue_purge(&sdio->tx_queue);
	sdio_claim_host(sdio->func);
	if (sdio->irq_claimed) {
		sdio_release_irq(sdio->func);
		sdio->irq_claimed = false;
	}
	if (sdio->enabled) {
		ret = uwe5622_sdio_read_u32_locked(sdio, UWE5622_CP_RESET_REG,
						   &reset);
		if (!ret)
			ret = uwe5622_sdio_write_u32_locked(sdio,
					UWE5622_CP_RESET_REG,
					reset | UWE5622_CP_RESET_BIT);
		if (ret)
			dev_warn(&sdio->func->dev,
				 "failed to hold firmware CPU in reset: %d\n", ret);
		sdio_disable_func(sdio->func);
		sdio->enabled = false;
	}
	sdio_release_host(sdio->func);
}

static int uwe5622_sdio_tx(struct uwe5622 *wcn, u8 channel,
			   struct sk_buff *skb)
{
	struct uwe5622_sdio *sdio = wcn->bus_priv;
	size_t record_len = sizeof(__le32) + ALIGN(skb->len, 4);
	size_t transfer_len = roundup(record_len + sizeof(__le32),
				       UWE5622_SDIO_BLOCK_SIZE);
	struct sk_buff *transfer;
	u32 header;

	if (channel >= UWE5622_RX_CHANNEL_BASE || skb->len > 0xffff)
		return -EINVAL;
	if (skb_queue_len(&sdio->tx_queue) >= 256)
		return -ENOBUFS;

	transfer = alloc_skb(transfer_len, GFP_ATOMIC);
	if (!transfer)
		return -ENOMEM;
	skb_put_zero(transfer, transfer_len);

	header = FIELD_PREP(UWE5622_PUH_LENGTH, skb->len) |
		 FIELD_PREP(UWE5622_PUH_SUBTYPE, channel);
	put_unaligned_le32(header, transfer->data);
	skb_copy_bits(skb, 0, transfer->data + sizeof(__le32), skb->len);
	put_unaligned_le32(UWE5622_PUH_EOF, transfer->data + record_len);
	skb_queue_tail(&sdio->tx_queue, transfer);
	schedule_work(&sdio->tx_work);

	return 0;
}

static int uwe5622_sdio_suspend_bus(struct uwe5622 *wcn, bool wake)
{
	struct uwe5622_sdio *sdio = wcn->bus_priv;
	mmc_pm_flag_t required = MMC_PM_KEEP_POWER;
	mmc_pm_flag_t caps;

	flush_work(&sdio->tx_work);

	if (wake)
		required |= MMC_PM_WAKE_SDIO_IRQ;

	caps = sdio_get_host_pm_caps(sdio->func);
	if ((caps & required) != required)
		return -EOPNOTSUPP;

	return sdio_set_host_pm_flags(sdio->func, required);
}

static int uwe5622_sdio_resume_bus(struct uwe5622 *wcn)
{
	return 0;
}

static const struct uwe5622_bus_ops uwe5622_sdio_bus_ops = {
	.start = uwe5622_sdio_start,
	.stop = uwe5622_sdio_stop,
	.tx = uwe5622_sdio_tx,
	.suspend = uwe5622_sdio_suspend_bus,
	.resume = uwe5622_sdio_resume_bus,
};

static int uwe5622_sdio_probe(struct sdio_func *func,
			      const struct sdio_device_id *id)
{
	struct uwe5622_sdio *sdio;
	int irq, ret;

	if (func->num != 1 ||
	    !of_device_is_compatible(func->dev.of_node, "sprd,uwe5622"))
		return -ENODEV;

	sdio = devm_kzalloc(&func->dev, sizeof(*sdio), GFP_KERNEL);
	if (!sdio)
		return -ENOMEM;

	sdio->rx_buf = devm_kmalloc(&func->dev, UWE5622_RX_MAX_SIZE,
				      GFP_KERNEL);
	if (!sdio->rx_buf)
		return -ENOMEM;

	sdio->func = func;
	skb_queue_head_init(&sdio->tx_queue);
	INIT_WORK(&sdio->tx_work, uwe5622_sdio_tx_work);
	sdio->wcn.dev = &func->dev;
	sdio->wcn.bus_ops = &uwe5622_sdio_bus_ops;
	sdio->wcn.bus_priv = sdio;
	sdio->wcn.bus_type = UWE5622_BUS_SDIO;
	sdio->wcn.services[UWE5622_SERVICE_WIFI_COMMAND] =
		(struct uwe5622_channel_pair) { 8, 22 };
	sdio->wcn.services[UWE5622_SERVICE_WIFI_DATA] =
		(struct uwe5622_channel_pair) { 10, 24 };
	sdio->wcn.services[UWE5622_SERVICE_WIFI_LOG] =
		(struct uwe5622_channel_pair) { 8, 23 };
	sdio->wcn.services[UWE5622_SERVICE_BLUETOOTH] =
		(struct uwe5622_channel_pair) { 3, 17 };
	sdio->wcn.bluetooth_enable = devm_gpiod_get_optional(&func->dev,
					"bluetooth-enable", GPIOD_OUT_LOW);
	if (IS_ERR(sdio->wcn.bluetooth_enable))
		return dev_err_probe(&func->dev,
			PTR_ERR(sdio->wcn.bluetooth_enable),
			"failed to acquire Bluetooth enable GPIO\n");
	sdio->wcn.device_wake = devm_gpiod_get_optional(&func->dev,
							"device-wake",
							GPIOD_OUT_LOW);
	if (IS_ERR(sdio->wcn.device_wake))
		return dev_err_probe(&func->dev, PTR_ERR(sdio->wcn.device_wake),
				     "failed to acquire device-wake GPIO\n");
	sdio_set_drvdata(func, sdio);
	device_init_wakeup(&func->dev, true);
	irq = of_irq_get(func->dev.of_node, 0);
	if (irq == -EPROBE_DEFER)
		return irq;
	if (irq > 0) {
		ret = devm_request_threaded_irq(&func->dev, irq, NULL,
				uwe5622_host_wake_irq,
				IRQF_ONESHOT | IRQF_NO_SUSPEND,
				dev_name(&func->dev), sdio);
		if (ret)
			return dev_err_probe(&func->dev, ret,
					     "failed to request host-wake IRQ\n");
		ret = dev_pm_set_wake_irq(&func->dev, irq);
		if (ret)
			return dev_err_probe(&func->dev, ret,
					     "failed to set host-wake IRQ\n");
		sdio->wake_irq_set = true;
	} else if (irq != -ENXIO && irq != -EINVAL) {
		return dev_err_probe(&func->dev, irq,
				     "failed to resolve host-wake IRQ\n");
	}

	ret = uwe5622_core_probe(&sdio->wcn);
	if (ret && sdio->wake_irq_set) {
		dev_pm_clear_wake_irq(&func->dev);
		sdio->wake_irq_set = false;
	}
	return ret;
}

static void uwe5622_sdio_remove(struct sdio_func *func)
{
	struct uwe5622_sdio *sdio = sdio_get_drvdata(func);

	uwe5622_core_remove(&sdio->wcn);
	if (sdio->wake_irq_set)
		dev_pm_clear_wake_irq(&func->dev);
	device_init_wakeup(&func->dev, false);
}

static void uwe5622_sdio_shutdown(struct sdio_func *func)
{
	struct uwe5622_sdio *sdio = sdio_get_drvdata(func);

	if (sdio)
		uwe5622_core_shutdown(&sdio->wcn);
}

static int uwe5622_sdio_suspend(struct device *dev)
{
	struct sdio_func *func = dev_to_sdio_func(dev);
	struct uwe5622_sdio *sdio = sdio_get_drvdata(func);

	return uwe5622_core_suspend(&sdio->wcn);
}

static int uwe5622_sdio_resume(struct device *dev)
{
	struct sdio_func *func = dev_to_sdio_func(dev);
	struct uwe5622_sdio *sdio = sdio_get_drvdata(func);

	return uwe5622_core_resume(&sdio->wcn);
}

static DEFINE_SIMPLE_DEV_PM_OPS(uwe5622_sdio_pm_ops, uwe5622_sdio_suspend,
				 uwe5622_sdio_resume);

static const struct sdio_device_id uwe5622_sdio_ids[] = {
	{ SDIO_DEVICE(0x0000, 0x0000) },
	{}
};
MODULE_DEVICE_TABLE(sdio, uwe5622_sdio_ids);

static struct sdio_driver uwe5622_sdio_driver = {
	.name = "uwe5622_sdio",
	.id_table = uwe5622_sdio_ids,
	.probe = uwe5622_sdio_probe,
	.remove = uwe5622_sdio_remove,
	.shutdown = uwe5622_sdio_shutdown,
	.drv.pm = pm_sleep_ptr(&uwe5622_sdio_pm_ops),
};
module_sdio_driver(uwe5622_sdio_driver);

MODULE_DESCRIPTION("Unisoc UWE5622 SDIO transport");
MODULE_LICENSE("GPL");
