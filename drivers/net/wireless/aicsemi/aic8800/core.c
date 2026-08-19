// SPDX-License-Identifier: GPL-2.0-only
/*
 * Core device handling for AICSemi AIC8800 series wireless devices.
 *
 * Copyright (C) 2026 Jernej Skrabec <jernej.skrabec@gmail.com>
 */

#include <linux/etherdevice.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#include "aic8800.h"

#define AIC_NAPI_WEIGHT		64

static void aic_rx_data_cfm(struct aic_hw *hw, const void *param,
			    unsigned int len)
{
	const struct aic_txcfm *cfm = param;
	struct aic_txcfm_slot slot;
	struct sk_buff *skb;
	u32 idx;
	bool acked;

	if (len < sizeof(*cfm))
		return;

	idx = le32_to_cpu(cfm->idx) % AIC_TXCFM_RING_SIZE;

	spin_lock_bh(&hw->tx_lock);
	slot = hw->cfm_ring[idx];
	memset(&hw->cfm_ring[idx], 0, sizeof(hw->cfm_ring[idx]));
	spin_unlock_bh(&hw->tx_lock);

	skb = slot.skb;
	if (!skb) {
		dev_warn_ratelimited(hw->dev,
				     "transmit confirmation for unknown frame %u\n",
				     idx);
		return;
	}

	if (slot.wdev) {
		acked = !!(le32_to_cpu(cfm->status) & AIC_TXCFM_S_ACKNOWLEDGED);

		skb_pull(skb, sizeof(struct aic_txdesc));
		cfg80211_mgmt_tx_status(slot.wdev, slot.cookie, skb->data,
					skb->len, acked, GFP_ATOMIC);
	}

	dev_consume_skb_any(skb);
}

/**
 * aic_rx_process - split one bus read into packets
 * @hw: device
 * @skb: buffer as read from the bus, consumed here
 * @rx_list: list receive frames are appended to
 *
 * Returns the number of data frames handed to the network stack.
 */
static int aic_rx_process(struct aic_hw *hw, struct sk_buff *skb)
{
	unsigned int off = 0;
	int frames = 0;

	while (off + AIC_BUS_HDR_LEN <= skb->len) {
		const u8 *hdr = skb->data + off;
		unsigned int len = get_unaligned_le16(hdr);
		u8 type = hdr[2];
		unsigned int stride;

		if (!len)
			break;

		if ((type & AIC_PKT_CFG) != AIC_PKT_CFG) {
			/*
			 * A data frame.  The length in the header covers the
			 * frame only, the receive header sits in front of it
			 * and overlaps the bus header.
			 */
			stride = round_up(len + AIC_RX_HDR_PAD, AIC_BUS_ALIGN);
			if (off + AIC_RX_HDR_PAD + len > skb->len)
				break;

			aic_rx_frame(hw, (const struct aic_rxhdr *)hdr,
				     hdr + AIC_RX_HDR_PAD, len);
			frames++;
		} else {
			stride = round_up(len, AIC_BUS_ALIGN) + AIC_BUS_HDR_LEN;
			if (off + AIC_BUS_HDR_LEN + len > skb->len)
				break;

			switch (type & AIC_PKT_TYPE_MASK) {
			case AIC_PKT_CFG_CMD_RSP:
				aic_rx_handle_msg(hw, hdr + AIC_BUS_HDR_LEN,
						  len);
				break;
			case AIC_PKT_CFG_DATA_CFM:
				aic_rx_data_cfm(hw, hdr + AIC_BUS_HDR_LEN, len);
				break;
			case AIC_PKT_CFG_PRINT:
				aic_rx_handle_print(hw, hdr + AIC_BUS_HDR_LEN,
						    len);
				break;
			default:
				dev_dbg_ratelimited(hw->dev,
						    "unknown packet type %02x\n",
						    type);
				break;
			}
		}

		off += stride;
	}

	dev_kfree_skb_any(skb);

	return frames;
}

static int aic_napi_poll(struct napi_struct *napi, int budget)
{
	struct aic_hw *hw = container_of(napi, struct aic_hw, napi);
	int done = 0;

	while (done < budget) {
		struct sk_buff *skb = skb_dequeue(&hw->rx_queue);

		if (!skb)
			break;

		done += aic_rx_process(hw, skb);
	}

	if (done < budget && skb_queue_empty(&hw->rx_queue))
		napi_complete_done(napi, done);

	return done;
}

struct aic_hw *aic_hw_alloc(struct device *dev, const struct aic_bus_ops *ops,
			    void *bus_priv)
{
	struct aic_hw *hw;
	struct wiphy *wiphy;
	int i;

	wiphy = wiphy_new(&aic_cfg80211_ops, sizeof(*hw));
	if (!wiphy)
		return ERR_PTR(-ENOMEM);

	hw = wiphy_priv(wiphy);
	hw->wiphy = wiphy;
	set_wiphy_dev(wiphy, dev);

	hw->dev = dev;
	hw->bus_ops = ops;
	hw->bus_priv = bus_priv;
	hw->monitor_vif = AIC_INVALID_VIF;
	hw->avail_vif_mask = GENMASK(AIC_MAX_VIF - 1, 0);

	INIT_LIST_HEAD(&hw->vifs);
	mutex_init(&hw->mutex);
	spin_lock_init(&hw->tx_lock);
	skb_queue_head_init(&hw->rx_queue);
	for (i = 0; i < AIC_TXQ_CNT; i++)
		skb_queue_head_init(&hw->txq[i]);

	/*
	 * Every peer entry owns a queue, and the teardown path walks all of
	 * them whether they were ever used or not.
	 */
	for (i = 0; i < AIC_MAX_STA; i++)
		skb_queue_head_init(&hw->sta[i].ps_queue);

	INIT_WORK(&hw->ps_work, aic_txq_ps_work);

	aic_cmd_mgr_init(&hw->cmd_mgr);

	hw->napi_dev = alloc_netdev_dummy(0);
	if (!hw->napi_dev) {
		mutex_destroy(&hw->mutex);
		wiphy_free(wiphy);
		return ERR_PTR(-ENOMEM);
	}

	netif_napi_add(hw->napi_dev, &hw->napi, aic_napi_poll);

	return hw;
}

void aic_hw_free(struct aic_hw *hw)
{
	int i;

	netif_napi_del(&hw->napi);
	free_netdev(hw->napi_dev);

	aic_cmd_mgr_deinit(&hw->cmd_mgr);

	cancel_work_sync(&hw->ps_work);

	for (i = 0; i < AIC_MAX_STA; i++)
		aic_sta_release(&hw->sta[i]);

	aic_txcfm_flush(hw, NULL);

	skb_queue_purge(&hw->rx_queue);
	for (i = 0; i < AIC_TXQ_CNT; i++)
		skb_queue_purge(&hw->txq[i]);

	mutex_destroy(&hw->mutex);
	wiphy_free(hw->wiphy);
}

int aic_hw_start(struct aic_hw *hw)
{
	int ret;

	napi_enable(&hw->napi);

	ret = hw->bus_ops->start(hw);
	if (ret)
		goto err_napi;

	ret = aic_fw_load(hw);
	if (ret)
		goto err_bus;

	/*
	 * Starting the firmware resets the device side of the bus, so the
	 * transport has to be brought back up before the first message.
	 */
	if (hw->bus_ops->fw_started) {
		ret = hw->bus_ops->fw_started(hw);
		if (ret)
			goto err_bus;
	}

	ret = aic_send_reset(hw);
	if (ret) {
		dev_err(hw->dev, "firmware reset failed: %d\n", ret);
		goto err_bus;
	}

	ret = aic_send_version_req(hw);
	if (ret) {
		dev_err(hw->dev, "failed to read the firmware version: %d\n",
			ret);
		goto err_bus;
	}

	ret = aic_cfg80211_init(hw);
	if (ret)
		goto err_bus;

	return 0;

err_bus:
	hw->bus_ops->stop(hw);
err_napi:
	napi_disable(&hw->napi);

	return ret;
}

/**
 * aic_hw_suspend - stop using the device without tearing it down
 * @hw: device
 *
 * The firmware keeps running and keeps the connection up while the host is
 * suspended, so only the host side of the data path is stopped.
 */
void aic_hw_suspend(struct aic_hw *hw)
{
	struct aic_vif *vif;

	mutex_lock(&hw->mutex);
	list_for_each_entry(vif, &hw->vifs, list)
		if (vif->ndev)
			netif_device_detach(vif->ndev);
	mutex_unlock(&hw->mutex);

	napi_disable(&hw->napi);
}

void aic_hw_resume(struct aic_hw *hw)
{
	struct aic_vif *vif;

	napi_enable(&hw->napi);

	mutex_lock(&hw->mutex);
	list_for_each_entry(vif, &hw->vifs, list)
		if (vif->ndev)
			netif_device_attach(vif->ndev);
	mutex_unlock(&hw->mutex);
}

void aic_hw_stop(struct aic_hw *hw)
{
	aic_cfg80211_deinit(hw);
	hw->bus_ops->stop(hw);
	napi_disable(&hw->napi);
}
