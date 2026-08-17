// SPDX-License-Identifier: GPL-2.0-only
/*
 * SDIO transport for AICSemi AIC8800 series wireless devices.
 *
 * Copyright (C) 2026 Jernej Skrabec <jernej.skrabec@gmail.com>
 */

#include <linux/kthread.h>
#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/mmc/sdio.h>
#include <linux/mmc/sdio_func.h>
#include <linux/mmc/sdio_ids.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#include "aic8800.h"

/* SDIO function 1 register map of the AIC8800D80 and later. */
#define AIC_SDIO_INTR_ENABLE		0x00
#define AIC_SDIO_INTR_PENDING		0x01
#define AIC_SDIO_INTR_TO_DEVICE		0x02
#define AIC_SDIO_FLOW_CTRL_DATA		0x03
#define AIC_SDIO_INT_STATUS		0x04
#define AIC_SDIO_BYTEMODE_LEN		0x05
#define AIC_SDIO_BYTEMODE_LEN_MSB	0x06
#define AIC_SDIO_BYTEMODE_ENABLE	0x07
#define AIC_SDIO_MISC_CTRL		0x08
#define AIC_SDIO_FLOW_CTRL_MSG		0x09
#define AIC_SDIO_CLK_TEST_RESULT		0x0a
#define AIC_SDIO_RD_FIFO		0x0f
#define AIC_SDIO_WR_FIFO		0x10

/* AIC_SDIO_INTR_ENABLE / AIC_SDIO_INT_STATUS */
#define AIC_SDIO_INTR_EN_ALL		0x07
#define AIC_SDIO_INT_MISC		BIT(7)
#define AIC_SDIO_INT_COUNT		GENMASK(6, 0)

/* AIC_SDIO_INTR_PENDING */
#define AIC_SDIO_PENDING_SOFT_IRQ	BIT(0)
#define AIC_SDIO_PENDING_AWAKE		BIT(4)

/* AIC_SDIO_INTR_TO_DEVICE */
#define AIC_SDIO_TO_DEVICE_WAKEUP	0x11

/*
 * Values at or above this in AIC_SDIO_INT_STATUS refer to the message queue
 * and carry the block count in the low three bits, lower values are a plain
 * block count for the data queue.
 */
#define AIC_SDIO_INT_MSG_BASE		113
#define AIC_SDIO_INT_MSG_COUNT		GENMASK(2, 0)

#define AIC_SDIO_BLOCK_SIZE		512

/* Size of one firmware receive/transmit buffer, used for flow control. */
#define AIC_FW_BUF_SIZE			1536

/* The firmware needs four zero bytes after a transfer that is not block sized. */
#define AIC_SDIO_TAIL_LEN		4

/* Aggregate at most this much data into a single bus write. */
#define AIC_TX_AGGR_SIZE		(64 * 1024)

/* Largest control message we ever send, including all framing. */
#define AIC_MSG_BUF_SIZE		2048

#define AIC_FLOW_CTRL_RETRIES		50

struct aic_sdio {
	struct sdio_func *func;
	struct aic_hw *hw;

	struct task_struct *tx_thread;
	wait_queue_head_t tx_wq;
	atomic_t tx_pending;

	/* aggregation buffer, only touched by the transmit thread */
	u8 *tx_buf;
	unsigned int tx_len;
	unsigned int tx_count;

	/* control message staging buffer, serialised by aic_cmd_mgr.send_lock */
	u8 *msg_buf;

	bool up;
};

/*
 * CRC-8 with polynomial 0x107 as implemented by the firmware.  This is not the
 * usual bitwise algorithm: the message bits are folded in over the polynomial
 * rather than over the high bit of the register, so the kernel's crc8() cannot
 * be used.
 */
static u8 aic_crc8(const u8 *buf, unsigned int len)
{
	u8 crc = 0;

	while (len--) {
		u8 bit;

		for (bit = 0x80; bit; bit >>= 1) {
			bool high = crc & 0x80;

			crc <<= 1;
			if (high)
				crc ^= 0x07;
			if (*buf & bit)
				crc ^= 0x07;
		}
		buf++;
	}

	return crc;
}

/*
 * All register accessors below require the SDIO host to be claimed.  The SDIO
 * interrupt handler is called with the host already claimed, and everything
 * else claims it around a whole operation rather than around each access,
 * which keeps the number of host claims per transfer down.
 */
static int aic_sdio_wr(struct aic_sdio *sdio, u8 reg, u8 val)
{
	int ret = 0;

	sdio_writeb(sdio->func, val, reg, &ret);

	return ret;
}

static int aic_sdio_rd(struct aic_sdio *sdio, u8 reg, u8 *val)
{
	int ret = 0;

	*val = sdio_readb(sdio->func, reg, &ret);

	return ret;
}

/**
 * aic_sdio_credits - ask the firmware how much room it has
 * @sdio: transport
 * @msg: query the message queue instead of the data queue
 *
 * Returns the number of free %AIC_FW_BUF_SIZE buffers, or a negative error
 * code.  The firmware can take a while to free buffers up, so retry for a
 * bounded amount of time before giving up.
 */
static int aic_sdio_credits(struct aic_sdio *sdio, bool msg)
{
	u8 reg = msg ? AIC_SDIO_FLOW_CTRL_MSG : AIC_SDIO_FLOW_CTRL_DATA;
	unsigned int i;

	for (i = 0; i < AIC_FLOW_CTRL_RETRIES; i++) {
		u8 val;
		int ret;

		ret = aic_sdio_rd(sdio, reg, &val);
		if (ret)
			return ret;
		if (val)
			return val;

		if (i < 30)
			udelay(200);
		else if (i < 40)
			usleep_range(1000, 1500);
		else
			usleep_range(10000, 11000);
	}

	return -EBUSY;
}

static int aic_sdio_write_fifo(struct aic_sdio *sdio, const void *buf,
			       unsigned int len)
{
	return sdio_writesb(sdio->func, AIC_SDIO_WR_FIFO, (void *)buf,
			    round_up(len, AIC_SDIO_BLOCK_SIZE));
}

static int aic_sdio_read_fifo(struct aic_sdio *sdio, void *buf,
			      unsigned int len)
{
	return sdio_readsb(sdio->func, buf, AIC_SDIO_RD_FIFO, len);
}

/* Control message transmission. --------------------------------------------*/

static int aic_sdio_send_msg(struct aic_hw *hw, const void *msg,
			     unsigned int msg_len)
{
	struct aic_sdio *sdio = hw->bus_priv;
	u8 *buf = sdio->msg_buf;
	unsigned int len;
	int credits, ret;

	/*
	 * Wire format: four byte bus header, one reserved word, then the
	 * message itself.
	 */
	len = AIC_BUS_HDR_LEN + 4 + msg_len;
	if (len + AIC_SDIO_TAIL_LEN > AIC_MSG_BUF_SIZE)
		return -EMSGSIZE;

	memset(buf, 0, len + AIC_SDIO_TAIL_LEN);
	put_unaligned_le16(msg_len + 4, buf);
	buf[2] = AIC_PKT_CFG_CMD_RSP;
	buf[3] = aic_crc8(buf, 3);
	memcpy(buf + AIC_BUS_HDR_LEN + 4, msg, msg_len);

	len = round_up(len, AIC_BUS_ALIGN);
	if (len % AIC_SDIO_BLOCK_SIZE)
		len += AIC_SDIO_TAIL_LEN;

	sdio_claim_host(sdio->func);

	credits = aic_sdio_credits(sdio, false);
	if (credits < 0) {
		ret = credits;
		goto out;
	}
	if (len > credits * AIC_FW_BUF_SIZE) {
		ret = -EBUSY;
		goto out;
	}

	ret = aic_sdio_write_fifo(sdio, buf, len);

out:
	sdio_release_host(sdio->func);

	return ret;
}

/* Data transmission. -------------------------------------------------------*/

/*
 * Append one frame to the aggregation buffer.  The frame is expected to start
 * with its struct aic_txdesc, which start_xmit() put into the skb headroom.
 */
static void aic_sdio_aggr_add(struct aic_sdio *sdio, struct sk_buff *skb)
{
	u8 *hdr = sdio->tx_buf + sdio->tx_len;

	put_unaligned_le16(skb->len, hdr);
	hdr[2] = AIC_PKT_DATA_TX;
	hdr[3] = aic_crc8(hdr, 3);

	memcpy(hdr + AIC_BUS_HDR_LEN, skb->data, skb->len);
	sdio->tx_len += AIC_BUS_HDR_LEN + skb->len;

	/* every frame starts on a four byte boundary */
	while (sdio->tx_len & (AIC_BUS_ALIGN - 1))
		sdio->tx_buf[sdio->tx_len++] = 0;

	sdio->tx_count++;
}

static int aic_sdio_aggr_flush(struct aic_sdio *sdio)
{
	unsigned int len = sdio->tx_len;
	int ret;

	if (!len)
		return 0;

	if (len % AIC_SDIO_BLOCK_SIZE) {
		memset(sdio->tx_buf + len, 0, AIC_SDIO_TAIL_LEN);
		len += AIC_SDIO_TAIL_LEN;
	}

	ret = aic_sdio_write_fifo(sdio, sdio->tx_buf, len);
	if (ret)
		dev_err_ratelimited(sdio->hw->dev, "transmit failed: %d\n",
				    ret);

	sdio->tx_len = 0;
	sdio->tx_count = 0;

	return ret;
}

static struct sk_buff *aic_sdio_dequeue(struct aic_hw *hw)
{
	struct sk_buff *skb = NULL;
	int i;

	spin_lock_bh(&hw->tx_lock);
	/* strict priority, highest access category first */
	for (i = AIC_TXQ_CNT - 1; i >= 0; i--) {
		skb = __skb_dequeue(&hw->txq[i]);
		if (skb)
			break;
	}
	spin_unlock_bh(&hw->tx_lock);

	return skb;
}

static void aic_sdio_tx_work(struct aic_sdio *sdio)
{
	struct aic_hw *hw = sdio->hw;
	int credits;

	sdio_claim_host(sdio->func);

	credits = aic_sdio_credits(sdio, false);
	if (credits < 0)
		goto out;

	while (credits > 0) {
		struct sk_buff *skb = aic_sdio_dequeue(hw);

		if (!skb)
			break;

		/*
		 * Flush before the frame would not fit, so that a single frame
		 * larger than the remaining room still gets its own transfer.
		 */
		if (sdio->tx_len + AIC_BUS_HDR_LEN + skb->len +
		    AIC_SDIO_TAIL_LEN > AIC_TX_AGGR_SIZE) {
			if (aic_sdio_aggr_flush(sdio))
				goto drop;
			credits = aic_sdio_credits(sdio, false);
			if (credits <= 0)
				goto requeue;
		}

		aic_sdio_aggr_add(sdio, skb);
		credits--;

		if (!(le16_to_cpu(((struct aic_txdesc *)skb->data)->flags) &
		      AIC_TXDESC_F_MGMT) &&
		    !(get_unaligned_le32(&((struct aic_txdesc *)skb->data)->hostid) &
		      AIC_TXDESC_HOSTID_CFM))
			dev_consume_skb_any(skb);
		continue;

requeue:
		spin_lock_bh(&hw->tx_lock);
		__skb_queue_head(&hw->txq[skb->priority & 7], skb);
		spin_unlock_bh(&hw->tx_lock);
		break;
drop:
		dev_kfree_skb_any(skb);
		break;
	}

	aic_sdio_aggr_flush(sdio);

out:
	sdio_release_host(sdio->func);
}

static int aic_sdio_tx_thread(void *data)
{
	struct aic_sdio *sdio = data;

	while (!kthread_should_stop()) {
		wait_event_interruptible(sdio->tx_wq,
					 atomic_read(&sdio->tx_pending) ||
					 kthread_should_stop());
		if (kthread_should_stop())
			break;

		atomic_set(&sdio->tx_pending, 0);
		aic_sdio_tx_work(sdio);
	}

	return 0;
}

static void aic_sdio_kick_tx(struct aic_hw *hw)
{
	struct aic_sdio *sdio = hw->bus_priv;

	atomic_set(&sdio->tx_pending, 1);
	wake_up(&sdio->tx_wq);
}

static int aic_sdio_send_data(struct aic_hw *hw, struct sk_buff *skb)
{
	unsigned int prio = skb->priority & 7;

	if (prio >= AIC_TXQ_CNT)
		prio = AIC_TXQ_BCMC;

	spin_lock_bh(&hw->tx_lock);
	__skb_queue_tail(&hw->txq[prio], skb);
	spin_unlock_bh(&hw->tx_lock);

	aic_sdio_kick_tx(hw);

	return 0;
}

/* Receive. -----------------------------------------------------------------*/

/*
 * Read one burst of data from the device.  Returns the number of bytes read,
 * zero when the device has nothing pending, or a negative error code.
 */
static int aic_sdio_rx_one(struct aic_sdio *sdio)
{
	struct aic_hw *hw = sdio->hw;
	struct sk_buff *skb;
	unsigned int len;
	u8 status;
	int ret;

	ret = aic_sdio_rd(sdio, AIC_SDIO_INT_STATUS, &status);
	if (ret)
		return ret;

	if (status & AIC_SDIO_INT_MISC) {
		u8 pending;

		/* acknowledge the device to host soft interrupt */
		ret = aic_sdio_rd(sdio, AIC_SDIO_INTR_PENDING, &pending);
		if (!ret)
			aic_sdio_wr(sdio, AIC_SDIO_INTR_PENDING,
				    pending & ~AIC_SDIO_PENDING_SOFT_IRQ);
		status &= ~AIC_SDIO_INT_MISC;
	}

	if (!status)
		return 0;

	if (status >= AIC_SDIO_INT_MSG_BASE)
		len = (status & AIC_SDIO_INT_MSG_COUNT) * AIC_SDIO_BLOCK_SIZE;
	else
		len = status * AIC_SDIO_BLOCK_SIZE;

	if (!len)
		return 0;

	skb = __dev_alloc_skb(len, GFP_ATOMIC);
	if (!skb)
		return -ENOMEM;

	ret = aic_sdio_read_fifo(sdio, skb->data, len);
	if (ret) {
		dev_kfree_skb_any(skb);
		return ret;
	}

	skb_put(skb, len);
	skb_queue_tail(&hw->rx_queue, skb);

	return len;
}

static void aic_sdio_irq(struct sdio_func *func)
{
	struct aic_sdio *sdio = sdio_get_drvdata(func);
	struct aic_hw *hw = sdio->hw;
	bool queued = false;
	int ret;

	if (!hw || !sdio->up)
		return;

	/*
	 * The host is already claimed for us.  Drain the device in a bounded
	 * loop so that a busy device cannot starve the transmit thread.
	 */
	do {
		ret = aic_sdio_rx_one(sdio);
		if (ret > 0)
			queued = true;
	} while (ret > 0);

	if (queued)
		napi_schedule(&hw->napi);
}

/* Bus operations. ----------------------------------------------------------*/

static int aic_sdio_start(struct aic_hw *hw)
{
	struct aic_sdio *sdio = hw->bus_priv;
	int ret;

	sdio_claim_host(sdio->func);

	ret = sdio_claim_irq(sdio->func, aic_sdio_irq);
	if (ret) {
		sdio_release_host(sdio->func);
		return ret;
	}

	/* enable the function interrupts in the CCCR */
	sdio_f0_writeb(sdio->func, 0x07, SDIO_CCCR_IENx, &ret);
	if (!ret)
		ret = aic_sdio_wr(sdio, AIC_SDIO_INTR_ENABLE,
				  AIC_SDIO_INTR_EN_ALL);

	sdio_release_host(sdio->func);

	if (ret) {
		dev_err(hw->dev, "failed to enable interrupts: %d\n", ret);
		return ret;
	}

	sdio->up = true;

	return 0;
}

/*
 * Bring the device side of the bus back up after the firmware took over from
 * the boot ROM.  This mirrors what the tail of aic_sdio_func_init() does, but
 * has to be redone because the firmware reinitialises the SDIO block.
 */
static int aic_sdio_fw_started(struct aic_hw *hw)
{
	struct aic_sdio *sdio = hw->bus_priv;
	u8 val;
	int ret;

	sdio_claim_host(sdio->func);
	ret = aic_sdio_wr(sdio, AIC_SDIO_BYTEMODE_ENABLE, 1);
	if (!ret)
		ret = aic_sdio_wr(sdio, AIC_SDIO_INTR_TO_DEVICE,
				  AIC_SDIO_TO_DEVICE_WAKEUP);
	sdio_release_host(sdio->func);
	if (ret)
		return ret;

	usleep_range(5000, 6000);

	sdio_claim_host(sdio->func);
	ret = aic_sdio_rd(sdio, AIC_SDIO_INTR_PENDING, &val);
	if (!ret && !(val & AIC_SDIO_PENDING_AWAKE)) {
		dev_err(hw->dev, "firmware did not come up (pending %02x)\n",
			val);
		ret = -EIO;
	}
	if (!ret)
		ret = aic_sdio_wr(sdio, AIC_SDIO_INTR_ENABLE,
				  AIC_SDIO_INTR_EN_ALL);
	sdio_release_host(sdio->func);

	return ret;
}

static void aic_sdio_stop(struct aic_hw *hw)
{
	struct aic_sdio *sdio = hw->bus_priv;

	sdio->up = false;

	sdio_claim_host(sdio->func);
	aic_sdio_wr(sdio, AIC_SDIO_INTR_ENABLE, 0);
	sdio_release_irq(sdio->func);
	sdio_release_host(sdio->func);
}

static const struct aic_bus_ops aic_sdio_bus_ops = {
	.start = aic_sdio_start,
	.stop = aic_sdio_stop,
	.fw_started = aic_sdio_fw_started,
	.send_msg = aic_sdio_send_msg,
	.send_data = aic_sdio_send_data,
	.kick_tx = aic_sdio_kick_tx,
};

/* Probe. -------------------------------------------------------------------*/

static int aic_sdio_func_init(struct aic_sdio *sdio)
{
	struct sdio_func *func = sdio->func;
	u8 val;
	int ret;

	sdio_claim_host(func);

	func->card->quirks |= MMC_QUIRK_LENIENT_FN0;

	ret = sdio_set_block_size(func, AIC_SDIO_BLOCK_SIZE);
	if (ret) {
		dev_err(&func->dev, "failed to set block size: %d\n", ret);
		goto out;
	}

	ret = sdio_enable_func(func);
	if (ret) {
		dev_err(&func->dev, "failed to enable function: %d\n", ret);
		goto out;
	}

	sdio_f0_writeb(func, 0x7f, 0xf2, &ret);
	if (ret)
		goto out;

	/* block mode only, the driver never uses byte mode transfers */
	ret = aic_sdio_wr(sdio, AIC_SDIO_BYTEMODE_ENABLE, 1);
	if (ret)
		goto out;

	ret = aic_sdio_wr(sdio, AIC_SDIO_INTR_TO_DEVICE,
			  AIC_SDIO_TO_DEVICE_WAKEUP);
	if (ret)
		goto out;

	sdio_release_host(func);

	usleep_range(5000, 6000);

	sdio_claim_host(func);
	ret = aic_sdio_rd(sdio, AIC_SDIO_INTR_PENDING, &val);
	if (ret)
		goto out;

	if (!(val & AIC_SDIO_PENDING_AWAKE)) {
		dev_err(&func->dev, "device did not wake up (pending %02x)\n",
			val);
		ret = -EIO;
	}

out:
	sdio_release_host(func);

	return ret;
}

static int aic_sdio_probe(struct sdio_func *func,
			  const struct sdio_device_id *id)
{
	struct aic_sdio *sdio;
	struct aic_hw *hw;
	int ret;

	/*
	 * The device exposes a second function that is only used by the
	 * 8800DC family.  Bind to function one and ignore the rest.
	 */
	if (func->num != 1)
		return -ENODEV;

	sdio = devm_kzalloc(&func->dev, sizeof(*sdio), GFP_KERNEL);
	if (!sdio)
		return -ENOMEM;

	sdio->func = func;
	init_waitqueue_head(&sdio->tx_wq);
	atomic_set(&sdio->tx_pending, 0);

	sdio->tx_buf = kmalloc(AIC_TX_AGGR_SIZE + AIC_SDIO_BLOCK_SIZE,
			       GFP_KERNEL);
	if (!sdio->tx_buf)
		return -ENOMEM;

	sdio->msg_buf = kmalloc(AIC_MSG_BUF_SIZE, GFP_KERNEL);
	if (!sdio->msg_buf) {
		ret = -ENOMEM;
		goto err_free_tx;
	}

	sdio_set_drvdata(func, sdio);

	ret = aic_sdio_func_init(sdio);
	if (ret)
		goto err_free_msg;

	hw = aic_hw_alloc(&func->dev, &aic_sdio_bus_ops, sdio);
	if (IS_ERR(hw)) {
		ret = PTR_ERR(hw);
		goto err_disable;
	}

	sdio->hw = hw;

	switch (func->device) {
	case 0x0082:
		hw->chip_id = AIC_CHIP_8800D80;
		break;
	case 0x2082:
		hw->chip_id = AIC_CHIP_8800D80X2;
		break;
	default:
		dev_err(&func->dev, "unsupported device id %04x\n",
			func->device);
		ret = -ENODEV;
		goto err_free_hw;
	}

	sdio->tx_thread = kthread_run(aic_sdio_tx_thread, sdio, "aic8800-tx");
	if (IS_ERR(sdio->tx_thread)) {
		ret = PTR_ERR(sdio->tx_thread);
		goto err_free_hw;
	}

	ret = aic_hw_start(hw);
	if (ret)
		goto err_stop_thread;

	return 0;

err_stop_thread:
	kthread_stop(sdio->tx_thread);
err_free_hw:
	aic_hw_free(hw);
err_disable:
	sdio_claim_host(func);
	sdio_disable_func(func);
	sdio_release_host(func);
err_free_msg:
	kfree(sdio->msg_buf);
err_free_tx:
	kfree(sdio->tx_buf);

	return ret;
}

static void aic_sdio_remove(struct sdio_func *func)
{
	struct aic_sdio *sdio = sdio_get_drvdata(func);

	if (!sdio)
		return;

	if (sdio->hw) {
		aic_hw_stop(sdio->hw);
		aic_hw_free(sdio->hw);
	}

	if (sdio->tx_thread)
		kthread_stop(sdio->tx_thread);

	sdio_claim_host(func);
	sdio_disable_func(func);
	sdio_release_host(func);

	kfree(sdio->msg_buf);
	kfree(sdio->tx_buf);
}

static const struct sdio_device_id aic_sdio_ids[] = {
	{ SDIO_DEVICE(0xc8a1, 0x0082) },	/* AIC8800D80 */
	{ SDIO_DEVICE(0xc8a1, 0x2082) },	/* AIC8800D80X2 */
	{ }
};
MODULE_DEVICE_TABLE(sdio, aic_sdio_ids);

static struct sdio_driver aic_sdio_driver = {
	.name = "aic8800_sdio",
	.id_table = aic_sdio_ids,
	.probe = aic_sdio_probe,
	.remove = aic_sdio_remove,
};
module_sdio_driver(aic_sdio_driver);

MODULE_DESCRIPTION("AICSemi AIC8800 SDIO wireless driver");
MODULE_AUTHOR("Jernej Skrabec <jernej.skrabec@gmail.com>");
MODULE_LICENSE("GPL");
