// SPDX-License-Identifier: GPL-2.0-only

#include <linux/align.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/kthread.h>
#include <linux/netdevice.h>
#include <linux/interrupt.h>
#include <linux/mmc/card.h>
#include <linux/mmc/core.h>
#include <linux/mmc/host.h>
#include <linux/mmc/sdio.h>
#include <linux/mmc/sdio_func.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm.h>
#include <linux/pm_wakeirq.h>
#include <linux/property.h>
#include <linux/scatterlist.h>
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
#define UWE5622_FW_READY_TIMEOUT_MS	10000
#define UWE5622_CP_RESET_REG		0x40088288
/* Bit 22 forces the Bluetooth memory off. */
#define UWE5622_WIFI_MEM_CFG1		0x4083c130
#define UWE5622_BTRAM_SHUTDOWN		BIT(22)
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
/*
 * The controller has to be told when the host stops being able to take an
 * in-band interrupt, because that is when it starts using the host-wake output
 * instead. Without this it stays quiet and nothing wakes the system.
 */
#define UWE5622_F0_AP_INT_CP0		0x1b0
#define UWE5622_AP_INT_ALLOW_SLEEP	BIT(0)
#define UWE5622_AP_INT_SUSPEND		BIT(5)
#define UWE5622_AP_INT_RESUME		BIT(6)

#define UWE5622_SDIO_CONFIG_ENABLE	BIT(0)
#define UWE5622_SDIO_CONFIG_SDMA_RX	BIT(4)
#define UWE5622_SDIO_CONFIG_BLK_SIZE	GENMASK(7, 5)
#define UWE5622_SDIO_CONFIG_BT_WAKE_EN	BIT(8)
#define UWE5622_SDIO_CONFIG_BT_TRIGGER	GENMASK(10, 9)
#define UWE5622_SDIO_CONFIG_INBAND_IRQ	BIT(11)
#define UWE5622_SDIO_CONFIG_WLAN_WAKE_EN	BIT(15)
#define UWE5622_SDIO_CONFIG_WLAN_TRIGGER	GENMASK(17, 16)
#define UWE5622_SDIO_CONFIG_WAKE_DURATION GENMASK(22, 18)
#define UWE5622_SDIO_CONFIG_WAKE_SPLIT	BIT(23)

/*
 * How the controller drives its wake output. It only tells active-low from
 * active-high: every non-zero encoding gives the same active-high pulse, and
 * neither keeps the edge-versus-level distinction the interrupt was described
 * with.
 */
#define UWE5622_WAKE_PULSE_LOW		0
#define UWE5622_WAKE_PULSE_HIGH		3
#define UWE5622_WAKE_DURATION_STEP_MS	10
#define UWE5622_WAKE_DURATION_DEFAULT_MS	20

#define UWE5622_PUH_PAD		GENMASK(5, 0)
#define UWE5622_PUH_CHECKSUM		BIT(6)
/*
 * Set when the receive engine's checksum accumulator follows the payload, as a
 * little-endian u16 that the payload length does not count.
 */
#define UWE5622_PUH_CHECKSUM		BIT(6)
#define UWE5622_PUH_LENGTH		GENMASK(22, 7)
#define UWE5622_PUH_EOF		BIT(23)
#define UWE5622_PUH_SUBTYPE		GENMASK(27, 24)
#define UWE5622_PUH_TYPE		GENMASK(31, 28)

#define UWE5622_RX_MAX_SIZE		(156 * UWE5622_SDIO_BLOCK_SIZE)
/*
 * A packet buffer has to hold the largest record the firmware can produce, so
 * it is two 840-byte blocks or four 512-byte ones depending on the block size
 * the controller was configured with.
 */
#define UWE5622_RX_PAC_SIZE		2048
#define UWE5622_RX_PAC_MAX		32
#define UWE5622_RX_PASS_LIMIT		1024
/*
 * How many frames may wait for the delivery thread before the receive loop
 * starts passing them up itself. Reads then throttle to the rate the stack
 * consumes, which is what should happen, instead of the backlog growing.
 */
#define UWE5622_RX_QUEUE_LIMIT		512

/* Function 0 register holding the controller's sleep request in bit 0. */
#define UWE5622_F0_SLEEP_CTL		0x1a2
#define UWE5622_RX_CHANNEL_BASE		12
#define UWE5622_SDIO_MAX_PAYLOAD		1676
#define UWE5622_TX_ERROR_LIMIT		4
/*
 * Transmit aggregation budget. Several records go out in one transfer, the same
 * way they arrive in one on the receive side, because a transfer costs far more
 * than the bytes in it.
 */
#define UWE5622_TX_MAX_BLOCKS		48
#define UWE5622_TX_MAX_SIZE		(UWE5622_TX_MAX_BLOCKS * \
					 UWE5622_SDIO_BLOCK_SIZE)









struct uwe5622_sdio {
	struct sdio_func *func;
	struct uwe5622 wcn;
	struct sk_buff_head tx_queue;
	struct work_struct tx_work;
	struct sk_buff_head rx_queue;
	struct task_struct *rx_thread;
	u8 *rx_buf;
	bool enabled;
	bool irq_claimed;
	int wake_irq[2];
	bool wake_irq_armed;
	bool wake_expected;
	/* Set by the host-wake handler, and the answer uwe5622_woke_host() gives. */
	bool wake_asserted;
	u32 wake_config;
	/* Aggregated receive: one buffer per packet plus the transfer trailer. */
	void *rx_pac[UWE5622_RX_PAC_MAX];
	/*
	 * One socket buffer per packet slot, read into directly. A frame that is
	 * passed up is replaced; a slot the controller left empty is reused as it
	 * is, so a quiet link allocates nothing at all.
	 */
	struct sk_buff *rx_skb[UWE5622_RX_PAC_MAX];
	void *rx_trailer;
	void *tx_buf;
	unsigned int tx_writes;
	unsigned int tx_records;
	unsigned int tx_qmax;
	struct scatterlist rx_sg[UWE5622_RX_PAC_MAX + 1];
	unsigned int rx_pac_num;
	/* Consecutive failed transmit transfers, reset by every success. */
	unsigned int tx_errors;
};

/*
 * Bus bookkeeping for a payload waiting to go out, kept clear of the offsets the
 * owning client uses for its own purposes before it hands the payload over.
 */
struct uwe5622_tx_cb {
	u8 channel;
	u8 tag;
};

#define UWE5622_TX_CB(_skb)	((struct uwe5622_tx_cb *)((_skb)->cb + 16))

static_assert(16 + sizeof(struct uwe5622_tx_cb) <=
	      sizeof_field(struct sk_buff, cb));

static size_t uwe5622_tx_budget(void)
{
	return UWE5622_TX_MAX_BLOCKS * UWE5622_SDIO_BLOCK_SIZE;
}

/*
 * A packet buffer holds one record and is two blocks, which is also the smallest
 * transfer that can carry a full record.
 */
static unsigned int uwe5622_pac_size(void)
{
	return 2 * UWE5622_SDIO_BLOCK_SIZE;
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
	unsigned long deadline = jiffies +
		msecs_to_jiffies(UWE5622_FW_READY_TIMEOUT_MS);
	u32 config = UWE5622_SDIO_CONFIG_ENABLE |
		     UWE5622_SDIO_CONFIG_INBAND_IRQ;

	bool config_written = false;
	u32 status;
	int ret;

	config |= sdio->wake_config;

	do {
		ret = uwe5622_sdio_direct_read_locked(sdio, UWE5622_SYNC_ADDR,
						      sync, sizeof(sync));
		if (ret)
			return ret;

		status = get_unaligned_le32(sync);
		switch (status) {
		case UWE5622_SYNC_ALL_FINISHED:
			/*
			 * Without its SDIO configuration the firmware keeps the
			 * receive mode it defaults to, which does not match the
			 * transfers this driver issues: it would answer as ready
			 * and then hand over transfers full of padding while its
			 * interrupt stays asserted. Fail the boot instead.
			 */
			if (!config_written)
				return dev_err_probe(&sdio->func->dev, -EIO,
						     "firmware finished before its SDIO configuration was written\n");
			return 0;
		case UWE5622_SYNC_CAL_WAITING:
			ret = uwe5622_sdio_write_u32_locked(sdio,
					UWE5622_SYNC_SDIO_CONFIG,
					config);
			if (!ret)
				ret = uwe5622_sdio_write_u32_locked(sdio,
						UWE5622_SYNC_ADDR,
						UWE5622_SYNC_CAL_WRITE_DONE);
			if (ret)
				return ret;
			config_written = true;
			break;
		case UWE5622_SYNC_VERIFY_WAITING:
			ret = uwe5622_sdio_answer_bind(sdio, sync + 0x40);
			if (ret)
				return ret;
			break;
		default:
			break;
		}

		/*
		 * The window in which the firmware waits for its calibration
		 * data is only open for a moment, and missing it leaves the
		 * SDIO configuration unwritten, so poll much faster than the
		 * second the whole handshake takes.
		 */
		usleep_range(800, 1200);
	} while (time_before(jiffies, deadline));

	return dev_err_probe(&sdio->func->dev, -ETIMEDOUT,
			     "firmware ready timeout, sync status %#08x\n", status);
}

/*
 * One transfer carries several records, each introduced by a four-byte public
 * header. The aggregate ends at the first EOF header or once the payload
 * accounted for reaches the valid length reported in the transfer trailer,
 * whichever comes first. Type 0xf marks padding the firmware inserts between
 * records; the receive channel comes from the subtype alone. A single damaged
 * record is skipped rather than discarding the rest of the transfer.
 */

/*
 * Aggregated receive. In this mode the firmware hands over a batch of packets
 * in one transfer, writing each into its own buffer and the trailer into a
 * final one, so a single CMD53 replaces one transaction per packet. The
 * alternative single buffer mode only ever carries one packet per transfer,
 * which caps throughput well below the negotiated link rate.
 */
static void uwe5622_sdio_free_rx_skbs(struct uwe5622_sdio *sdio)
{
	int i;

	for (i = 0; i < UWE5622_RX_PAC_MAX; i++) {
		kfree_skb(sdio->rx_skb[i]);
		sdio->rx_skb[i] = NULL;
	}
}

static int uwe5622_sdio_read_aggregated(struct uwe5622_sdio *sdio,
					unsigned int pac_num, u32 *valid_len,
					u32 *pending)
{
	struct sdio_func *func = sdio->func;
	struct mmc_host *host = func->card->host;
	struct mmc_request mrq = {};
	struct mmc_command cmd = {};
	struct mmc_data data = {};
	unsigned int i, blocks;
	const u8 *trailer;

	pac_num = clamp_t(unsigned int, pac_num, 1, UWE5622_RX_PAC_MAX);

	for (i = 0; i < pac_num; i++) {
		if (sdio->rx_skb[i])
			continue;
		sdio->rx_skb[i] = alloc_skb(uwe5622_pac_size(), GFP_KERNEL);
		if (!sdio->rx_skb[i])
			return i ? (int)i : -ENOMEM;
	}

	sg_init_table(sdio->rx_sg, pac_num + 1);
	for (i = 0; i < pac_num; i++)
		sg_set_buf(&sdio->rx_sg[i], sdio->rx_skb[i]->data,
			   uwe5622_pac_size());
	sg_set_buf(&sdio->rx_sg[pac_num], sdio->rx_trailer,
		   UWE5622_SDIO_BLOCK_SIZE);

	blocks = pac_num * (uwe5622_pac_size() / UWE5622_SDIO_BLOCK_SIZE) + 1;

	data.sg = sdio->rx_sg;
	data.sg_len = pac_num + 1;
	data.blksz = UWE5622_SDIO_BLOCK_SIZE;
	data.blocks = blocks;
	data.flags = MMC_DATA_READ;

	/* CMD53, function 1, block mode, fixed address at the packet window. */
	cmd.opcode = SD_IO_RW_EXTENDED;
	cmd.arg = (func->num & 0x7) << 28 | BIT(27) |
		  (UWE5622_SDIO_PACKET_ADDR & 0x1ffff) << 9 |
		  (blocks & 0x1ff);
	cmd.flags = MMC_RSP_SPI_R5 | MMC_RSP_R5 | MMC_CMD_ADTC;

	mrq.cmd = &cmd;
	mrq.data = &data;
	mmc_set_data_timeout(&data, func->card);
	mmc_wait_for_req(host, &mrq);

	if (cmd.error)
		return cmd.error;
	if (data.error)
		return data.error;

	trailer = sdio->rx_trailer;
	*valid_len = get_unaligned_le32(trailer + UWE5622_SDIO_BLOCK_SIZE - 8);
	*pending = get_unaligned_le32(trailer + UWE5622_SDIO_BLOCK_SIZE - 4);

	return pac_num;
}

static void uwe5622_sdio_drain(struct uwe5622_sdio *sdio);

static int uwe5622_sdio_rx_thread(void *data)
{
	struct uwe5622_sdio *sdio = data;
	struct sk_buff *skb;

	/*
	 * Low real time priority, like the vendor driver gives its transport
	 * threads: a late read costs a whole transfer's worth of bus time, and
	 * the firmware's queue drains no faster than this thread runs.
	 */
	sched_set_fifo_low(current);

	while (!kthread_should_stop()) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (skb_queue_empty(&sdio->rx_queue)) {
			schedule();
			continue;
		}
		__set_current_state(TASK_RUNNING);

		while ((skb = skb_dequeue(&sdio->rx_queue)))
			uwe5622_core_rx(&sdio->wcn, skb->cb[0], skb);
	}
	__set_current_state(TASK_RUNNING);

	return 0;
}

/*
 * Hand a batch to the delivery thread instead of passing it up from the read
 * loop. Delivery costs more than the transfer that fetched the frames, and
 * every microsecond spent in it is a microsecond the controller spends holding
 * a queue nobody is draining, so the loop should get back to reading at once.
 */
static void uwe5622_sdio_deliver(struct uwe5622_sdio *sdio,
				 struct sk_buff_head *queue, bool behind)
{
	struct sk_buff *skb;

	if (sdio->rx_thread && sdio->rx_thread != current && behind &&
	    skb_queue_len(&sdio->rx_queue) < UWE5622_RX_QUEUE_LIMIT) {
		spin_lock_bh(&sdio->rx_queue.lock);
		skb_queue_splice_tail_init(queue, &sdio->rx_queue);
		spin_unlock_bh(&sdio->rx_queue.lock);
		wake_up_process(sdio->rx_thread);
		return;
	}

	while ((skb = __skb_dequeue(queue)))
		uwe5622_core_rx(&sdio->wcn, skb->cb[0], skb);
}

static void uwe5622_sdio_drain_rx_aggregated(struct uwe5622_sdio *sdio)
{
	struct sdio_func *func = sdio->func;
	struct sk_buff_head queue;
	struct sk_buff *skb;
	u32 valid_len, pending;
	unsigned int pass = 0;
	int i, got;

	/*
	 * Claimed once for the whole drain rather than around each read. The
	 * controller is the only user of this host, and a claim costs a mutex
	 * and a runtime power reference every time; at these rates that is
	 * thousands of pairs a second for nothing.
	 */
	sdio_claim_host(func);

	do {
		__skb_queue_head_init(&queue);

		got = uwe5622_sdio_read_aggregated(sdio, sdio->rx_pac_num,
						   &valid_len, &pending);
		if (got < 0) {
			dev_err_ratelimited(&func->dev,
					    "aggregated RX failed: %d\n", got);
			goto out;
		}

		for (i = 0; i < got; i++) {
			u32 header = get_unaligned_le32(sdio->rx_skb[i]->data);
			u16 payload_len;
			u8 channel;

			if (header & UWE5622_PUH_EOF)
				break;
			payload_len = FIELD_GET(UWE5622_PUH_LENGTH, header);
			if (!payload_len ||
			    payload_len > UWE5622_SDIO_MAX_PAYLOAD ||
			    FIELD_GET(UWE5622_PUH_TYPE, header) == 0xf)
				continue;

			channel = UWE5622_RX_CHANNEL_BASE +
				  FIELD_GET(UWE5622_PUH_SUBTYPE, header);
			skb = sdio->rx_skb[i];
			sdio->rx_skb[i] = NULL;
			/*
			 * The accumulator sits past the payload, so take it
			 * before the buffer is trimmed to the payload itself,
			 * and hand it to the client beside the frame.
			 */
			if (header & UWE5622_PUH_CHECKSUM) {
				const u8 *at = skb->data + sizeof(__le32) +
					       payload_len;

				put_unaligned(get_unaligned_le16(at),
					      (u16 *)&skb->cb[2]);
				skb->cb[4] = 1;
			} else {
				skb->cb[4] = 0;
			}
			skb_put(skb, sizeof(__le32) + payload_len);
			skb_pull(skb, sizeof(__le32));
			skb->cb[0] = channel;
			__skb_queue_tail(&queue, skb);
		}

		sdio->rx_pac_num = clamp_t(unsigned int, pending, 1,
					   UWE5622_RX_PAC_MAX);

		uwe5622_sdio_deliver(sdio, &queue, pending);

		cond_resched();
		/*
		 * Drain until the controller reports nothing left: stopping
		 * early strands the backlog, and the interrupt that would have
		 * fetched it has already been consumed. The cap only exists so
		 * a misreported count cannot spin here forever.
		 */
		if (++pass == UWE5622_RX_PASS_LIMIT) {
			dev_warn_ratelimited(&func->dev,
					     "aggregated RX still reports %u pending after %u passes\n",
					     pending, pass);
			goto out;
		}
	} while (pending);
out:
	sdio_release_host(func);
}

/*
 * This callback runs on the host's SDIO interrupt thread, which is nice -16.
 * The in-band interrupt is only deasserted once the queued data has been read,
 * so the transfer itself has to be drained here rather than handed to a worker,
 * which would leave the interrupt asserted and spin the thread instead. Only
 * the delivery of what was read is handed off, by uwe5622_sdio_deliver().
 * UWE5622_RX_MAX_BURST bounds one pass so a chatty firmware cannot hold the
 * CPU indefinitely.
 */
/*
 * Registers outside the always-on domain only answer while the controller is
 * awake, so its sleep request has to be cleared for the transfer rather than
 * once at boot: the firmware puts it back.
 */
static int uwe5622_sdio_wake_locked(struct uwe5622_sdio *sdio)
{
	int ret = 0;

	sdio_f0_writeb(sdio->func, 0, UWE5622_F0_SLEEP_CTL, &ret);

	return ret;
}

static void uwe5622_sdio_drain(struct uwe5622_sdio *sdio)
{
	uwe5622_sdio_drain_rx_aggregated(sdio);

}

static void uwe5622_sdio_irq(struct sdio_func *func)
{
	struct uwe5622_sdio *sdio = sdio_get_drvdata(func);

	/*
	 * No interrupt status read here. Which function interrupted is already
	 * known, nothing reads the value, and a command costs bus time on every
	 * interrupt: the queue itself says what there is to do.
	 */
	uwe5622_sdio_drain(sdio);
}

static void uwe5622_sdio_tx_work(struct work_struct *work)
{
	struct uwe5622_sdio *sdio = container_of(work, struct uwe5622_sdio,
						 tx_work);
	struct sk_buff_head batch;
	size_t budget = uwe5622_tx_budget();
	struct sk_buff *skb;
	size_t used;
	int ret;

	__skb_queue_head_init(&batch);

	while (!skb_queue_empty(&sdio->tx_queue)) {
		used = 0;
		while ((skb = skb_dequeue(&sdio->tx_queue))) {
			size_t record = ALIGN(skb->len, 4);

			if (used + record + sizeof(__le32) > budget) {
				skb_queue_head(&sdio->tx_queue, skb);
				break;
			}
			memcpy(sdio->tx_buf + used, skb->data, skb->len);
			/* Records start on a four-byte boundary. */
			memset(sdio->tx_buf + used + skb->len, 0,
			       record - skb->len);
			used += record;
			__skb_queue_tail(&batch, skb);
		}
		if (!used)
			break;

		put_unaligned_le32(UWE5622_PUH_EOF, sdio->tx_buf + used);
		used += sizeof(__le32);
		used = roundup(used, UWE5622_SDIO_BLOCK_SIZE);

		sdio_claim_host(sdio->func);
		ret = sdio_writesb(sdio->func, UWE5622_SDIO_PACKET_ADDR,
				   sdio->tx_buf, used);
		sdio_release_host(sdio->func);

		if (!ret) {
			sdio->tx_errors = 0;
			sdio->tx_writes++;
			sdio->tx_records += skb_queue_len(&batch);
			if (!(sdio->tx_writes % 2048))
				dev_info(&sdio->func->dev,
					 "tx writes=%u records=%u qmax=%u\n",
					 sdio->tx_writes, sdio->tx_records,
					 sdio->tx_qmax);
			while ((skb = __skb_dequeue(&batch)))
				dev_consume_skb_any(skb);
			continue;
		}

		dev_err_ratelimited(&sdio->func->dev,
				    "TX transfer failed: %d\n", ret);
		/*
		 * The firmware never saw this transfer, so hand every tag in it
		 * back and let the client release what it reserved. Without
		 * this a failed write silently consumes firmware transmit
		 * credits for good.
		 */
		while ((skb = __skb_dequeue(&batch))) {
			uwe5622_core_tx_error(&sdio->wcn,
					      UWE5622_TX_CB(skb)->channel,
					      UWE5622_TX_CB(skb)->tag);
			dev_kfree_skb_any(skb);
		}
		if (++sdio->tx_errors < UWE5622_TX_ERROR_LIMIT)
			continue;

		dev_err(&sdio->func->dev,
			"%u consecutive transmit failures, recovering\n",
			sdio->tx_errors);
		sdio->tx_errors = 0;
		skb_queue_purge(&sdio->tx_queue);
		uwe5622_core_request_recovery(&sdio->wcn);
		return;
	}
}

static int uwe5622_sdio_boot(struct uwe5622_sdio *sdio,
			     const struct firmware *fw)
{
	int ret;

	sdio_claim_host(sdio->func);
	ret = sdio_enable_func(sdio->func);
	if (ret)
		goto out_release;
	sdio->enabled = true;

	ret = sdio_set_block_size(sdio->func, UWE5622_SDIO_BLOCK_SIZE);
	if (ret)
		goto out_disable;

	{
		/*
		 * Clear the controller's sleep request before anything else.
		 * Left set, the core powers down between packets and has to be
		 * woken for each one; and a core that went to sleep under a
		 * previous instance of this driver has to be woken here or it
		 * will not run the firmware about to be downloaded at all.
		 */
		sdio_f0_writeb(sdio->func, 0, UWE5622_F0_SLEEP_CTL, &ret);
		if (ret)
			dev_warn(&sdio->func->dev,
				 "failed to clear the sleep request: %d\n", ret);
	}

	ret = uwe5622_sdio_download_firmware(sdio, fw);
	if (ret)
		goto out_disable;

	/*
	 * Clear the handshake word the previous firmware left behind while the
	 * core is still held in reset. Its memory survives the reset, so a
	 * reload would otherwise read the old "all finished" state and skip the
	 * whole handshake; clearing it once the core runs is worse still,
	 * because it races with the firmware posting the state in which it waits
	 * for its SDIO configuration and can drop that request, leaving the
	 * controller answering as ready with a receive mode the host does not
	 * use.
	 */
	ret = uwe5622_sdio_write_u32_locked(sdio, UWE5622_SYNC_ADDR, 0);
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

static int uwe5622_sdio_start(struct uwe5622 *wcn, const struct firmware *fw)
{
	struct uwe5622_sdio *sdio = wcn->bus_priv;

	return uwe5622_sdio_boot(sdio, fw);
}

static void uwe5622_sdio_stop(struct uwe5622 *wcn)
{
	struct uwe5622_sdio *sdio = wcn->bus_priv;
	int ret;

	cancel_work_sync(&sdio->tx_work);
	skb_queue_purge(&sdio->tx_queue);
	skb_queue_purge(&sdio->rx_queue);
	sdio_claim_host(sdio->func);
	if (sdio->irq_claimed) {
		sdio_release_irq(sdio->func);
		sdio->irq_claimed = false;
	}
	if (sdio->enabled) {
		u32 reset;

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

/*
 * The controller's enable line belongs to the card's power sequence, which is
 * where the binding puts it, so the way to reach it is to ask the host to cycle
 * the card: that drops power, drives the line, restores it and reinitialises the
 * function. Everything the controller held is gone afterwards, firmware included,
 * which is the point.
 */
static int uwe5622_sdio_power_cycle(struct uwe5622 *wcn)
{
	struct uwe5622_sdio *sdio = wcn->bus_priv;
	struct sdio_func *func = sdio->func;
	int ret;

	sdio_claim_host(func);
	ret = mmc_hw_reset(func->card);
	if (!ret)
		ret = sdio_enable_func(func);
	sdio_release_host(func);
	if (ret)
		return ret;

	sdio->enabled = false;
	dev_info(&func->dev, "controller power cycled\n");

	return 0;
}

static int uwe5622_sdio_bt_ram(struct uwe5622 *wcn, bool on)
{
	struct uwe5622_sdio *sdio = wcn->bus_priv;
	u32 value;
	int ret;

	sdio_claim_host(sdio->func);
	ret = uwe5622_sdio_wake_locked(sdio);
	if (ret) {
		sdio_release_host(sdio->func);
		return ret;
	}
	ret = uwe5622_sdio_read_u32_locked(sdio, UWE5622_WIFI_MEM_CFG1, &value);
	if (!ret && !value) {
		/*
		 * The whole register reading as zero means the window does not
		 * reach it rather than that every bit in it is clear, and a
		 * write would then put a zero into a live configuration
		 * register, so leave it alone.
		 */
		dev_info(&sdio->func->dev,
			 "memory configuration register is not reachable\n");
		ret = -ENODEV;
	} else if (!ret) {
		if (on)
			value &= ~UWE5622_BTRAM_SHUTDOWN;
		else
			value |= UWE5622_BTRAM_SHUTDOWN;
		ret = uwe5622_sdio_write_u32_locked(sdio, UWE5622_WIFI_MEM_CFG1,
						    value);
		dev_info(&sdio->func->dev, "bt memory %s, cfg1 %#010x (%d)\n",
			 on ? "on" : "off", value, ret);
	}
	sdio_release_host(sdio->func);

	return ret;
}

static int uwe5622_sdio_tx(struct uwe5622 *wcn, u8 channel,
			   struct sk_buff *skb, u8 tag)
{
	struct uwe5622_sdio *sdio = wcn->bus_priv;
	u32 header;

	if (channel >= UWE5622_RX_CHANNEL_BASE ||
	    skb->len > UWE5622_SDIO_MAX_PAYLOAD)
		return -EINVAL;
	if (skb_queue_len(&sdio->tx_queue) >= 256)
		return -ENOBUFS;

	/*
	 * The public header goes in front of the payload where it already lies,
	 * so a frame from the network stack reaches the controller without being
	 * copied on the way. The end marker and the padding to a whole transfer
	 * belong to the transfer, not to one record, and are added when several
	 * records are packed together.
	 */
	if (skb_cow_head(skb, UWE5622_BUS_HEADROOM))
		return -ENOMEM;

	header = FIELD_PREP(UWE5622_PUH_LENGTH, skb->len) |
		 FIELD_PREP(UWE5622_PUH_SUBTYPE, channel);
	put_unaligned_le32(header, skb_push(skb, UWE5622_BUS_HEADROOM));
	UWE5622_TX_CB(skb)->channel = channel;
	UWE5622_TX_CB(skb)->tag = tag;
	skb_queue_tail(&sdio->tx_queue, skb);
	if (skb_queue_len(&sdio->tx_queue) > sdio->tx_qmax)
		sdio->tx_qmax = skb_queue_len(&sdio->tx_queue);
	schedule_work(&sdio->tx_work);

	return 0;
}

/*
 * Let the controller sleep, or hold it awake. It only drives the host-wake line
 * from its own low power state, so the state that costs a wake-up per transfer
 * while the system runs is exactly the state the system has to leave it in when
 * it suspends.
 */
static void uwe5622_sdio_allow_sleep(struct uwe5622_sdio *sdio, bool allow)
{
	int ret = 0;

	sdio_claim_host(sdio->func);
	sdio_f0_writeb(sdio->func, allow ? 1 : 0, UWE5622_F0_SLEEP_CTL, &ret);
	if (allow && !ret) {
		/*
		 * The request is the byte and then this strobe, which is a
		 * different bit of the same register the system suspend state is
		 * reported through. The controller needs two cycles of its
		 * 32 kHz clock to take it.
		 */
		sdio_f0_writeb(sdio->func, UWE5622_AP_INT_ALLOW_SLEEP,
			       UWE5622_F0_AP_INT_CP0, &ret);
		udelay(65);
	}
	sdio_release_host(sdio->func);
	if (ret)
		dev_warn(&sdio->func->dev,
			 "failed to %s the controller sleeping: %d\n",
			 allow ? "allow" : "stop", ret);
}

/*
 * Tell the controller that the host is going down or has come back. The vendor
 * driver does the same two writes, and they are what arms and disarms the
 * host-wake output on its side.
 */
static void uwe5622_sdio_notify_host_pm(struct uwe5622_sdio *sdio, u8 event)
{
	int ret = 0;

	sdio_claim_host(sdio->func);
	sdio_f0_writeb(sdio->func, event, UWE5622_F0_AP_INT_CP0, &ret);
	sdio_release_host(sdio->func);
	if (ret)
		dev_warn(&sdio->func->dev,
			 "failed to report host power state %#x: %d\n", event,
			 ret);
}

static int uwe5622_sdio_suspend_bus(struct uwe5622 *wcn, bool wake)
{
	struct uwe5622_sdio *sdio = wcn->bus_priv;
	mmc_pm_flag_t required = MMC_PM_KEEP_POWER;
	mmc_pm_flag_t caps;
	int ret;

	flush_work(&sdio->tx_work);

	/* An out-of-band host-wake interrupt replaces SDIO IRQ wake support. */
	if (wake && device_may_wakeup(&sdio->func->dev) &&
	    !sdio->wake_irq[0] && !sdio->wake_irq[1])
		required |= MMC_PM_WAKE_SDIO_IRQ;

	caps = sdio_get_host_pm_caps(sdio->func);
	if ((caps & required) != required)
		return -EOPNOTSUPP;

	ret = sdio_set_host_pm_flags(sdio->func, required);
	if (ret)
		return ret;

	/*
	 * Last, once nothing else will be asked of the controller: from here it
	 * answers by driving the host-wake line rather than the bus.
	 */
	if (wake && sdio->wake_config &&
	    (sdio->wake_irq[0] || sdio->wake_irq[1])) {
		unsigned int i;

		/*
		 * Read out whatever is already waiting first, then listen before
		 * saying anything. Being told the host is going down makes the
		 * transport answer the next pending record with a pulse on the
		 * wake line instead of a transfer, and it does that once: a
		 * record from before the sleep would otherwise spend the only
		 * pulse there is, leaving the sleep deaf to everything after it.
		 * If one is pending anyway, the pulse arrives against an armed
		 * interrupt and the sleep is abandoned, which is the right answer
		 * when there is something to read.
		 */
		uwe5622_sdio_drain_rx_aggregated(sdio);

		/*
		 * Drop an edge latched while the interrupt was masked, so the
		 * one pulse that matters is not answered by an older one.
		 */
		for (i = 0; i < ARRAY_SIZE(sdio->wake_irq); i++) {
			if (!sdio->wake_irq[i])
				continue;
			irq_set_irqchip_state(sdio->wake_irq[i],
					      IRQCHIP_STATE_PENDING, false);
			enable_irq(sdio->wake_irq[i]);
			ret = enable_irq_wake(sdio->wake_irq[i]);
			if (ret) {
				disable_irq(sdio->wake_irq[i]);
				return ret;
			}
		}
		sdio->wake_irq_armed = true;
		/*
		 * Let the edges latched while the interrupts were masked be
		 * delivered and discarded first, then start believing them: a
		 * pulse answering the notification below can arrive before the
		 * write returns, and it must not be taken for an old one.
		 */
		for (i = 0; i < ARRAY_SIZE(sdio->wake_irq); i++)
			if (sdio->wake_irq[i])
				synchronize_irq(sdio->wake_irq[i]);
		WRITE_ONCE(sdio->wake_asserted, false);
		WRITE_ONCE(sdio->wake_expected, true);
		uwe5622_sdio_notify_host_pm(sdio, UWE5622_AP_INT_SUSPEND);
		/* From here a pulse means the controller wants the host back. */
		uwe5622_sdio_allow_sleep(sdio, true);
	}

	return 0;
}

static bool uwe5622_sdio_woke_host(struct uwe5622 *wcn)
{
	struct uwe5622_sdio *sdio = wcn->bus_priv;

	return READ_ONCE(sdio->wake_asserted);
}

static int uwe5622_sdio_resume_bus(struct uwe5622 *wcn)
{
	struct uwe5622_sdio *sdio = wcn->bus_priv;

	if (sdio->wake_config) {
		uwe5622_sdio_allow_sleep(sdio, false);
		uwe5622_sdio_notify_host_pm(sdio, UWE5622_AP_INT_RESUME);
	}
	if (sdio->wake_irq_armed) {
		unsigned int i;

		WRITE_ONCE(sdio->wake_expected, false);
		sdio->wake_irq_armed = false;
		for (i = 0; i < ARRAY_SIZE(sdio->wake_irq); i++) {
			if (!sdio->wake_irq[i])
				continue;
			disable_irq_wake(sdio->wake_irq[i]);
			disable_irq(sdio->wake_irq[i]);
		}
	}

	return 0;
}

static const struct uwe5622_bus_ops uwe5622_sdio_bus_ops = {
	.start = uwe5622_sdio_start,
	.stop = uwe5622_sdio_stop,
	.tx = uwe5622_sdio_tx,
	.bt_ram = uwe5622_sdio_bt_ram,
	.power_cycle = uwe5622_sdio_power_cycle,
	.suspend = uwe5622_sdio_suspend_bus,
	.resume = uwe5622_sdio_resume_bus,
	.woke_host = uwe5622_sdio_woke_host,
};

/*
 * The controller pulsed its host-wake output. There is nothing to read here: the
 * record it is holding is delivered once the transport is running again, and all
 * this has to do is tell the power management core that this device is why the
 * system is coming back, which also aborts a suspend still in progress.
 */
static irqreturn_t uwe5622_sdio_wake_irq(int irq, void *data)
{
	struct uwe5622_sdio *sdio = data;

	/*
	 * An edge that arrived while the interrupt was masked is delivered as
	 * soon as it is unmasked, and that is not a wake: the controller has not
	 * been told the host is going down yet, so it is still answering the bus
	 * normally and whatever it wanted will be read on the way out of suspend.
	 */
	if (!READ_ONCE(sdio->wake_expected))
		return IRQ_HANDLED;

	WRITE_ONCE(sdio->wake_asserted, true);
	pm_wakeup_event(&sdio->func->dev, 0);
	dev_dbg(&sdio->func->dev, "controller asked to be read\n");

	return IRQ_HANDLED;
}

static int uwe5622_sdio_wake_trigger(struct device *dev, int irq, u32 *trigger)
{
	switch (irq_get_trigger_type(irq)) {
	case IRQ_TYPE_EDGE_RISING:
	case IRQ_TYPE_LEVEL_HIGH:
		*trigger = UWE5622_WAKE_PULSE_HIGH;
		return 0;
	case IRQ_TYPE_EDGE_FALLING:
	case IRQ_TYPE_LEVEL_LOW:
		*trigger = UWE5622_WAKE_PULSE_LOW;
		return 0;
	default:
		return dev_err_probe(dev, -EINVAL,
				     "host-wake interrupt polarity is invalid\n");
	}
}

static int uwe5622_sdio_set_wake_config(struct uwe5622_sdio *sdio, int bt,
					int wlan)
{
	u32 duration, trigger;
	int ret;

	duration = UWE5622_WAKE_DURATION_DEFAULT_MS;
	if (device_property_present(&sdio->func->dev,
				    "sprd,host-wake-duration-ms")) {
		ret = device_property_read_u32(&sdio->func->dev,
					       "sprd,host-wake-duration-ms",
					       &duration);
		if (ret)
			return dev_err_probe(&sdio->func->dev, ret,
					     "failed to read host-wake duration\n");
	}
	if (!duration || duration % UWE5622_WAKE_DURATION_STEP_MS ||
	    duration / UWE5622_WAKE_DURATION_STEP_MS >
		FIELD_MAX(UWE5622_SDIO_CONFIG_WAKE_DURATION))
		return dev_err_probe(&sdio->func->dev, -EINVAL,
				     "host-wake duration is invalid\n");

	if (bt > 0) {
		ret = uwe5622_sdio_wake_trigger(&sdio->func->dev, bt, &trigger);
		if (ret)
			return ret;
		sdio->wake_config |= UWE5622_SDIO_CONFIG_BT_WAKE_EN |
			FIELD_PREP(UWE5622_SDIO_CONFIG_BT_TRIGGER, trigger);
	}
	if (wlan > 0) {
		ret = uwe5622_sdio_wake_trigger(&sdio->func->dev, wlan,
						&trigger);
		if (ret)
			return ret;
		sdio->wake_config |= UWE5622_SDIO_CONFIG_WLAN_WAKE_EN |
			FIELD_PREP(UWE5622_SDIO_CONFIG_WLAN_TRIGGER, trigger);
	}
	/*
	 * With one output enabled it carries both radios. With two, the firmware
	 * would still send everything to the Bluetooth one, which it gives
	 * priority, unless it is told to keep them apart.
	 */
	if (bt > 0 && wlan > 0)
		sdio->wake_config |= UWE5622_SDIO_CONFIG_WAKE_SPLIT;
	sdio->wake_config |= FIELD_PREP(UWE5622_SDIO_CONFIG_WAKE_DURATION,
					duration /
					UWE5622_WAKE_DURATION_STEP_MS);

	return 0;
}

static int uwe5622_sdio_request_wake_irq(struct uwe5622_sdio *sdio, int irq,
					unsigned int slot)
{
	int ret;

	/*
	 * The controller drives these outputs whenever it hands a transfer over,
	 * not only to wake the system, so they stay masked while the system runs:
	 * the per-transfer pulses would saturate the CPU as soon as traffic
	 * starts. They are unmasked for the duration of a sleep only.
	 *
	 * The power management core's dedicated wake interrupt does that, but it
	 * arms after the last chance this driver has to talk to the controller,
	 * and the controller answers the first record after being told the host
	 * is going down with a single pulse. Arming has to come first or that
	 * pulse is lost, so the interrupts are this driver's to manage.
	 */
	ret = devm_request_threaded_irq(&sdio->func->dev, irq, NULL,
					uwe5622_sdio_wake_irq,
					IRQF_ONESHOT | IRQF_NO_AUTOEN,
					"uwe5622-host-wake", sdio);
	if (ret)
		return dev_err_probe(&sdio->func->dev, ret,
				     "failed to request host-wake IRQ\n");
	sdio->wake_irq[slot] = irq;

	return 0;
}

/*
 * The controller brings out one wake output per radio, and a board wires the
 * ones it has room for. Take whichever the interrupts describe: with both, each
 * radio is given its own output, which is what the separation bit selects; with
 * one, that output carries the wake traffic of both radios.
 */
static int uwe5622_sdio_get_wake_irq(struct uwe5622_sdio *sdio)
{
	struct fwnode_handle *fwnode = dev_fwnode(&sdio->func->dev);
	int bt, wlan, ret;

	bt = fwnode_irq_get_byname(fwnode, "bt-host-wake");
	if (bt == -EPROBE_DEFER)
		return bt;
	if (bt < 0 && bt != -EINVAL && bt != -ENXIO)
		return bt;
	wlan = fwnode_irq_get_byname(fwnode, "wlan-host-wake");
	if (wlan == -EPROBE_DEFER)
		return wlan;
	if (wlan < 0 && wlan != -EINVAL && wlan != -ENXIO)
		return wlan;
	if (bt < 0 && wlan < 0)
		return 0;

	ret = uwe5622_sdio_set_wake_config(sdio, bt, wlan);
	if (ret)
		return ret;

	if (bt > 0) {
		ret = uwe5622_sdio_request_wake_irq(sdio, bt, 0);
		if (ret)
			return ret;
	}
	if (wlan > 0) {
		ret = uwe5622_sdio_request_wake_irq(sdio, wlan, 1);
		if (ret)
			return ret;
	}

	return 0;
}

static int uwe5622_sdio_probe(struct sdio_func *func,
			      const struct sdio_device_id *id)
{
	struct uwe5622_sdio *sdio;
	int pac, ret;

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
	/* Room for the aggregation budget plus its end marker and padding. */
	sdio->tx_buf = devm_kmalloc(&func->dev,
				    UWE5622_TX_MAX_SIZE + UWE5622_SDIO_BLOCK_SIZE,
				    GFP_KERNEL);
	if (!sdio->tx_buf)
		return -ENOMEM;

	/*
	 * The firmware download window is reached through function 0 registers
	 * at 0x15c, outside the vendor CCCR range that sdio_f0_writeb() allows
	 * by default, so every access would fail with -EINVAL without this.
	 */
	func->card->quirks |= MMC_QUIRK_LENIENT_FN0;

	for (pac = 0; pac < UWE5622_RX_PAC_MAX; pac++) {
		sdio->rx_pac[pac] = devm_kmalloc(&func->dev,
						 UWE5622_RX_PAC_SIZE,
						 GFP_KERNEL);
		if (!sdio->rx_pac[pac])
			return -ENOMEM;
	}
	sdio->rx_trailer = devm_kmalloc(&func->dev, UWE5622_SDIO_BLOCK_SIZE,
					GFP_KERNEL);
	if (!sdio->rx_trailer)
		return -ENOMEM;
	sdio->rx_pac_num = 1;

	sdio->func = func;
	skb_queue_head_init(&sdio->tx_queue);
	INIT_WORK(&sdio->tx_work, uwe5622_sdio_tx_work);
	skb_queue_head_init(&sdio->rx_queue);
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
	sdio->wcn.services[UWE5622_SERVICE_AT] =
		(struct uwe5622_channel_pair) { 0, 13 };
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
	ret = device_init_wakeup(&func->dev,
				 device_property_read_bool(&func->dev,
							   "wakeup-source"));
	if (ret)
		return ret;
	ret = uwe5622_sdio_get_wake_irq(sdio);
	if (ret)
		goto err_wakeup;

	/*
	 * Last, because everything above can still fail and only the remove
	 * callback stops this thread, which a failed probe never reaches: the
	 * device memory would be freed underneath a thread still holding it.
	 */
	sdio->rx_thread = kthread_run(uwe5622_sdio_rx_thread, sdio, "%s-rx",
				      dev_name(&func->dev));
	if (IS_ERR(sdio->rx_thread)) {
		ret = dev_err_probe(&func->dev, PTR_ERR(sdio->rx_thread),
				    "failed to start the receive thread\n");
		sdio->rx_thread = NULL;
		goto err_wakeup;
	}

	ret = uwe5622_core_probe(&sdio->wcn);
	if (!ret)
		return 0;

	kthread_stop(sdio->rx_thread);
	skb_queue_purge(&sdio->rx_queue);
	uwe5622_sdio_free_rx_skbs(sdio);
err_wakeup:
	device_init_wakeup(&func->dev, false);
	return ret;
}

static void uwe5622_sdio_remove(struct sdio_func *func)
{
	struct uwe5622_sdio *sdio = sdio_get_drvdata(func);

	uwe5622_core_remove(&sdio->wcn);
	kthread_stop(sdio->rx_thread);
	skb_queue_purge(&sdio->rx_queue);
	uwe5622_sdio_free_rx_skbs(sdio);
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
