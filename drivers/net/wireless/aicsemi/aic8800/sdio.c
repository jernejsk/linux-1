// SPDX-License-Identifier: GPL-2.0-only
/*
 * SDIO transport for AICSemi AIC8800 series wireless devices.
 *
 * Copyright (C) 2026 Jernej Skrabec <jernej.skrabec@gmail.com>
 */

#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/kthread.h>
#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/mmc/sdio.h>
#include <linux/mmc/sdio_func.h>
#include <linux/mmc/sdio_ids.h>
#include <linux/module.h>
#include <linux/of_irq.h>
#include <linux/pm.h>
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
#define AIC_SDIO_TO_DEVICE_SOFT_IRQ	BIT(0)
#define AIC_SDIO_TO_DEVICE_SLEEP	0x02
#define AIC_SDIO_TO_DEVICE_AUTO_PS	0x08
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

/* Buffers read from the device per interrupt before yielding the bus. */
#define AIC_SDIO_RX_BUDGET		32

/*
 * Function 0 registers of the device SDIO block that control how it samples
 * the bus.  The defaults only work up to 25 MHz on some boards.
 */
#define AIC_SDIO_F0_IOPAD_CTRL		0xf0
#define AIC_SDIO_F0_IOPAD_DELAY1		0xf1
#define AIC_SDIO_F0_IOPAD_DELAY2		0xf8
#define AIC_SDIO_F0_DRIVE		0xf2

#define AIC_SDIO_IOPAD_CTRL_SDR		0x01
#define AIC_SDIO_IOPAD_CTRL_DDR		0x21
#define AIC_SDIO_IOPAD_CTRL_FREE_CLK	BIT(6)

static int iopad_delay = 0x40;
module_param(iopad_delay, int, 0644);
MODULE_PARM_DESC(iopad_delay,
		 "device side SDIO input delay, -1 to leave it alone");

static int iopad_delay2;
module_param(iopad_delay2, int, 0644);
MODULE_PARM_DESC(iopad_delay2, "device side SDIO output delay");

static int iopad_drive = -1;
module_param(iopad_drive, int, 0644);
MODULE_PARM_DESC(iopad_drive,
		 "device side SDIO pad drive strength, -1 to leave it alone");

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

	/* optional out of band wake line, only used to wake the system */
	int wake_irq;

	bool up;
	bool suspended;
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

	/*
	 * The device keeps parsing packet headers until it finds a zero length,
	 * so everything that goes out on the wire has to be initialised, not
	 * just the message itself.
	 */
	memset(buf, 0, round_up(len + AIC_SDIO_TAIL_LEN, AIC_SDIO_BLOCK_SIZE));
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
		dev_err(hw->dev, "no room for a message: %d\n", credits);
		ret = credits;
		goto out;
	}
	if (len > credits * AIC_FW_BUF_SIZE) {
		ret = -EBUSY;
		goto out;
	}

	ret = aic_sdio_write_fifo(sdio, buf, len);
	if (ret)
		dev_err(hw->dev, "message write of %u bytes failed: %d\n", len,
			ret);

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
					 (atomic_read(&sdio->tx_pending) &&
					  !sdio->suspended) ||
					 kthread_should_stop());
		if (kthread_should_stop())
			break;

		atomic_set(&sdio->tx_pending, 0);
		aic_sdio_tx_work(sdio);

		/* let a waiting suspend know that the queues are drained */
		wake_up(&sdio->tx_wq);
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
	int ret, i;

	if (!hw || !sdio->up)
		return;

	/*
	 * The host is already claimed for us.  Drain the device in a bounded
	 * loop: a busy device must not starve the transmit thread, and a device
	 * that reports data that never goes away must not wedge the bus.
	 */
	for (i = 0; i < AIC_SDIO_RX_BUDGET; i++) {
		ret = aic_sdio_rx_one(sdio);
		if (ret <= 0)
			break;
		queued = true;
	}

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

	/*
	 * A driver that is loaded a second time finds the firmware already
	 * running, and possibly asleep: it only answers messages once it has
	 * been woken.  The boot ROM does not mind the request.
	 */
	sdio_claim_host(sdio->func);
	aic_sdio_wr(sdio, AIC_SDIO_INTR_TO_DEVICE, AIC_SDIO_TO_DEVICE_WAKEUP);
	sdio_release_host(sdio->func);
	usleep_range(5000, 6000);

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

/* Power management. ---------------------------------------------------------*/

/**
 * aic_sdio_sleep - allow the device to stop its bus clock domain
 * @sdio: transport
 *
 * The device keeps receiving while asleep and raises the host wake line, or
 * the in band SDIO interrupt, when something has to be handled.
 */
static int aic_sdio_sleep(struct aic_sdio *sdio)
{
	int ret;

	sdio_claim_host(sdio->func);
	ret = aic_sdio_wr(sdio, AIC_SDIO_INTR_TO_DEVICE,
			  AIC_SDIO_TO_DEVICE_SLEEP);
	sdio_release_host(sdio->func);

	return ret;
}

/**
 * aic_sdio_wakeup - bring the device back out of sleep
 * @sdio: transport
 *
 * The device clears the request bit once it is running again.
 */
static int aic_sdio_wakeup(struct aic_sdio *sdio)
{
	int retries = 50;
	int ret;
	u8 val;

	sdio_claim_host(sdio->func);

	ret = aic_sdio_wr(sdio, AIC_SDIO_INTR_TO_DEVICE,
			  AIC_SDIO_TO_DEVICE_WAKEUP);
	while (!ret && retries--) {
		ret = aic_sdio_rd(sdio, AIC_SDIO_INTR_TO_DEVICE, &val);
		if (ret || !(val & AIC_SDIO_TO_DEVICE_SOFT_IRQ))
			break;

		usleep_range(200, 400);
	}

	sdio_release_host(sdio->func);

	if (!ret && retries < 0) {
		dev_err(&sdio->func->dev, "device did not wake up\n");
		return -ETIMEDOUT;
	}

	return ret;
}

static int aic_sdio_suspend(struct device *dev)
{
	struct sdio_func *func = dev_to_sdio_func(dev);
	struct aic_sdio *sdio = sdio_get_drvdata(func);
	struct aic_hw *hw = sdio->hw;
	mmc_pm_flag_t flags = MMC_PM_KEEP_POWER;
	int ret;

	if (!(sdio_get_host_pm_caps(func) & MMC_PM_KEEP_POWER)) {
		dev_err(dev, "the host cannot keep this device powered\n");
		return -EOPNOTSUPP;
	}

	/*
	 * Without a host wake line the in band interrupt is the only way the
	 * device can announce a wake up event.
	 */
	if (hw->wakeup_enabled && sdio->wake_irq <= 0)
		flags |= MMC_PM_WAKE_SDIO_IRQ;

	ret = sdio_set_host_pm_flags(func, flags);
	if (ret)
		return ret;

	aic_hw_suspend(hw);

	/*
	 * Let the transmit thread finish what it has already queued and keep it
	 * off the bus from here on, the host is about to suspend the card.
	 */
	wait_event_timeout(sdio->tx_wq, !atomic_read(&sdio->tx_pending),
			   msecs_to_jiffies(100));
	sdio->suspended = true;

	if (sdio->wake_irq > 0 && hw->wakeup_enabled) {
		enable_irq(sdio->wake_irq);
		enable_irq_wake(sdio->wake_irq);
	}

	return aic_sdio_sleep(sdio);
}

static int aic_sdio_resume(struct device *dev)
{
	struct sdio_func *func = dev_to_sdio_func(dev);
	struct aic_sdio *sdio = sdio_get_drvdata(func);
	struct aic_hw *hw = sdio->hw;
	int ret;

	if (sdio->wake_irq > 0 && hw->wakeup_enabled) {
		disable_irq_wake(sdio->wake_irq);
		disable_irq(sdio->wake_irq);
	}

	ret = aic_sdio_wakeup(sdio);
	if (ret)
		return ret;

	sdio->suspended = false;
	aic_hw_resume(hw);

	/* anything that arrived while suspended is waiting to be read */
	aic_sdio_kick_tx(hw);
	napi_schedule(&hw->napi);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(aic_sdio_pm_ops, aic_sdio_suspend,
				aic_sdio_resume);

/**
 * aic_sdio_wake_isr - host wake line went active
 * @irq: interrupt number
 * @data: transport
 *
 * The line is only used to wake the system, the pending work is picked up by
 * the normal SDIO interrupt afterwards.
 */
static irqreturn_t aic_sdio_wake_isr(int irq, void *data)
{
	struct aic_sdio *sdio = data;

	dev_dbg(&sdio->func->dev, "host wake\n");

	return IRQ_HANDLED;
}

/**
 * aic_sdio_wake_irq_init - claim the optional host wake line
 * @sdio: transport
 *
 * Boards that cannot wake from the in band SDIO interrupt route a separate
 * line from the device to a wake capable interrupt instead.  The line is only
 * enabled while the system is suspended.
 */
static int aic_sdio_wake_irq_init(struct aic_sdio *sdio)
{
	struct device *dev = &sdio->func->dev;
	int irq, ret;

	irq = fwnode_irq_get_byname(dev_fwnode(dev), "host-wake");
	if (irq == -EPROBE_DEFER)
		return irq;
	if (irq <= 0)
		return 0;

	ret = devm_request_irq(dev, irq, aic_sdio_wake_isr,
			       IRQF_TRIGGER_RISING | IRQF_NO_AUTOEN,
			       "aic8800-wake", sdio);
	if (ret) {
		dev_err(dev, "failed to claim the host wake line: %d\n", ret);
		return ret;
	}

	sdio->wake_irq = irq;
	device_set_wakeup_capable(dev, true);

	return 0;
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
	int ret;

	sdio_claim_host(func);

	/*
	 * The device only accepts block mode transfers, but the SDIO core picks
	 * byte mode whenever a transfer is not larger than one block.  Capping
	 * byte mode at 511 bytes makes it use block mode from one block on.
	 */
	func->card->quirks |= MMC_QUIRK_LENIENT_FN0 |
			      MMC_QUIRK_BROKEN_BYTE_MODE_512;

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

	if (iopad_drive >= 0) {
		sdio_f0_writeb(func, iopad_drive, AIC_SDIO_F0_DRIVE, &ret);
		if (ret)
			goto out;
	}

	/*
	 * Tell the device how to sample the bus.  Without this it answers
	 * writes with a CRC error as soon as the clock goes past 25 MHz.
	 */
	if (iopad_delay >= 0) {
		u8 ctrl = AIC_SDIO_IOPAD_CTRL_FREE_CLK;

		ctrl |= func->card->host->ios.timing == MMC_TIMING_UHS_DDR50 ?
			AIC_SDIO_IOPAD_CTRL_DDR : AIC_SDIO_IOPAD_CTRL_SDR;

		sdio_f0_writeb(func, ctrl, AIC_SDIO_F0_IOPAD_CTRL, &ret);
		if (!ret)
			sdio_f0_writeb(func, iopad_delay2,
				       AIC_SDIO_F0_IOPAD_DELAY2, &ret);
		if (!ret)
			sdio_f0_writeb(func, iopad_delay,
				       AIC_SDIO_F0_IOPAD_DELAY1, &ret);
		if (ret)
			goto out;

		usleep_range(1000, 2000);
	}

	/* block mode only, the driver never uses byte mode transfers */
	ret = aic_sdio_wr(sdio, AIC_SDIO_BYTEMODE_ENABLE, 1);

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

	ret = aic_sdio_wake_irq_init(sdio);
	if (ret)
		goto err_free_hw;

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
	.drv.pm = pm_sleep_ptr(&aic_sdio_pm_ops),
};
module_sdio_driver(aic_sdio_driver);

MODULE_DESCRIPTION("AICSemi AIC8800 SDIO wireless driver");
MODULE_AUTHOR("Jernej Skrabec <jernej.skrabec@gmail.com>");
MODULE_LICENSE("GPL");
