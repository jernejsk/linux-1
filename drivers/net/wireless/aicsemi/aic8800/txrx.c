// SPDX-License-Identifier: GPL-2.0-only
/*
 * Data path for AICSemi AIC8800 series wireless devices.
 *
 * The firmware works on 802.3 frames with the Ethernet header split out into
 * the transmit descriptor, and hands received frames back the same way, so the
 * driver only ever deals with Ethernet frames plus a small descriptor.
 *
 * Copyright (C) 2026 Jernej Skrabec <jernej.skrabec@gmail.com>
 */

#include <linux/etherdevice.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <net/ieee80211_radiotap.h>

#include "aic8800.h"

/* Map a TID to the access category the firmware expects. */
static const u8 aic_tid_to_ac[IEEE80211_NUM_TIDS] = {
	AIC_AC_BE, AIC_AC_BK, AIC_AC_BK, AIC_AC_BE,
	AIC_AC_VI, AIC_AC_VI, AIC_AC_VO, AIC_AC_VO,
};

/**
 * aic_tx_peer - work out which peer a frame is for
 * @vif: interface the frame is transmitted on
 * @skb: frame, still carrying its Ethernet header
 * @tid: filled in with the TID to use
 *
 * Returns the peer, or %NULL if the frame has to be dropped.
 */
static struct aic_sta *aic_tx_peer(struct aic_vif *vif, struct sk_buff *skb,
				   u8 *tid)
{
	const struct ethhdr *eth = (const struct ethhdr *)skb->data;
	struct aic_hw *hw = vif->hw;
	struct aic_sta *sta = NULL;

	*tid = skb->priority & IEEE80211_QOS_CTL_TAG1D_MASK;
	if (*tid >= IEEE80211_NUM_TIDS)
		*tid = 0;

	switch (vif->wdev.iftype) {
	case NL80211_IFTYPE_STATION:
	case NL80211_IFTYPE_P2P_CLIENT:
		sta = vif->sta.ap;
		break;
	case NL80211_IFTYPE_AP:
	case NL80211_IFTYPE_P2P_GO:
		if (is_multicast_ether_addr(eth->h_dest)) {
			sta = &hw->sta[vif->ap.bcmc_idx];
		} else {
			struct aic_sta *iter;

			list_for_each_entry(iter, &vif->ap.sta_list, list) {
				if (ether_addr_equal(iter->addr, eth->h_dest)) {
					sta = iter;
					break;
				}
			}
		}
		break;
	default:
		break;
	}

	if (sta && !sta->valid)
		sta = NULL;

	/*
	 * A peer that does not do QoS gets everything on the best effort queue
	 * with no TID.
	 */
	if (sta && !sta->qos)
		*tid = 0xff;

	return sta;
}

/**
 * aic_start_xmit - hand an Ethernet frame to the firmware
 * @skb: frame to send
 * @ndev: interface to send it on
 */
netdev_tx_t aic_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct aic_vif *vif = netdev_priv(ndev);
	struct aic_hw *hw = vif->hw;
	struct aic_txdesc *desc;
	struct ethhdr eth;
	struct aic_sta *sta;
	u16 len;
	u8 tid;

	if (skb->len <= sizeof(eth))
		goto drop;

	sta = aic_tx_peer(vif, skb, &tid);
	if (!sta)
		goto drop;

	/*
	 * The descriptor replaces the Ethernet header, but it is larger, so the
	 * skb needs some headroom.  Most frames come with enough of it.
	 */
	if (skb_cow_head(skb, sizeof(*desc) - sizeof(eth)))
		goto drop;

	memcpy(&eth, skb->data, sizeof(eth));
	skb_pull(skb, sizeof(eth));
	len = skb->len;

	desc = skb_push(skb, sizeof(*desc));
	memset(desc, 0, sizeof(*desc));
	desc->packet_len = cpu_to_le16(len);
	memcpy(&desc->eth_dest_addr, eth.h_dest, ETH_ALEN);
	memcpy(&desc->eth_src_addr, eth.h_source, ETH_ALEN);
	desc->ethertype = eth.h_proto;
	desc->staid = sta->sta_idx;
	desc->tid = tid;
	desc->vif_idx = vif->vif_index;
	desc->ac = tid == 0xff ? AIC_AC_BE : aic_tid_to_ac[tid];
	if (vif->use_4addr && sta->sta_idx < AIC_MAX_STA)
		desc->flags = cpu_to_le16(AIC_TXDESC_F_USE_4ADDR);

	skb->priority = desc->ac;

	vif->stats.tx_packets++;
	vif->stats.tx_bytes += len;

	hw->bus_ops->send_data(hw, skb);

	return NETDEV_TX_OK;

drop:
	vif->stats.tx_dropped++;
	dev_kfree_skb_any(skb);

	return NETDEV_TX_OK;
}

/**
 * aic_txq_flush_vif - drop everything queued for one interface
 * @hw: device
 * @vif: interface going away
 */
void aic_txq_flush_vif(struct aic_hw *hw, struct aic_vif *vif)
{
	struct sk_buff_head done;
	struct sk_buff *skb;
	int i;

	__skb_queue_head_init(&done);

	spin_lock_bh(&hw->tx_lock);
	for (i = 0; i < AIC_TXQ_CNT; i++) {
		struct sk_buff *tmp;

		skb_queue_walk_safe(&hw->txq[i], skb, tmp) {
			struct aic_txdesc *desc = (struct aic_txdesc *)skb->data;

			if (desc->vif_idx != vif->vif_index)
				continue;

			__skb_unlink(skb, &hw->txq[i]);
			__skb_queue_tail(&done, skb);
		}
	}
	spin_unlock_bh(&hw->tx_lock);

	while ((skb = __skb_dequeue(&done)))
		dev_kfree_skb_any(skb);
}

/* Receive. -----------------------------------------------------------------*/

static void aic_rx_mgmt(struct aic_hw *hw, const struct aic_rxhdr *rxhdr,
			const u8 *frame, unsigned int len)
{
	struct aic_vif *vif = aic_vif_from_fw_idx(hw, rxhdr->flags_vif_idx);
	struct cfg80211_rx_info info = {
		.freq = rxhdr->phy_info.phy_prim20_freq,
		.sig_dbm = rxhdr->vect.rx_vect1.rssi1,
		.buf = frame,
		.len = len,
	};

	if (!vif)
		return;

	cfg80211_rx_mgmt_ext(&vif->wdev, &info);
}

static void aic_rx_monitor(struct aic_hw *hw, const struct aic_rxhdr *rxhdr,
			   const u8 *frame, unsigned int len)
{
	struct ieee80211_radiotap_header *rtap;
	struct sk_buff *skb;
	unsigned int rtap_len = sizeof(*rtap) + 8;
	__le32 present = cpu_to_le32(BIT(IEEE80211_RADIOTAP_TSFT) |
				     BIT(IEEE80211_RADIOTAP_DBM_ANTSIGNAL) |
				     BIT(IEEE80211_RADIOTAP_CHANNEL));
	u8 *pos;

	if (hw->monitor_vif == AIC_INVALID_VIF)
		return;

	skb = dev_alloc_skb(rtap_len + len);
	if (!skb)
		return;

	rtap = skb_put_zero(skb, rtap_len);
	rtap->it_version = 0;
	rtap->it_len = cpu_to_le16(rtap_len);
	rtap->it_present = present;

	pos = (u8 *)(rtap + 1);
	put_unaligned_le64(((u64)le32_to_cpu(rxhdr->vect.tsf_hi) << 32) |
			   le32_to_cpu(rxhdr->vect.tsf_lo), pos);
	pos += 8;

	skb_put_data(skb, frame, len);

	skb->dev = hw->vif[hw->monitor_vif] ?
		   hw->vif[hw->monitor_vif]->ndev : NULL;
	if (!skb->dev) {
		dev_kfree_skb_any(skb);
		return;
	}

	skb->protocol = htons(ETH_P_802_2);
	skb_reset_mac_header(skb);
	skb->pkt_type = PACKET_OTHERHOST;
	skb->ip_summed = CHECKSUM_UNNECESSARY;

	netif_receive_skb(skb);
}

/**
 * aic_rx_frame - hand one received frame to the network stack
 * @hw: device
 * @rxhdr: receive descriptor, which also holds the bus header
 * @frame: frame contents
 * @len: length of @frame
 *
 * Data frames arrive as 802.3 frames without their Ethernet header, which the
 * firmware put into the descriptor, so the header is rebuilt here.  Management
 * frames and monitor traffic arrive as full 802.11 frames instead.
 */
void aic_rx_frame(struct aic_hw *hw, const struct aic_rxhdr *rxhdr,
		  const u8 *frame, unsigned int len)
{
	struct aic_vif *vif;
	struct sk_buff *skb;

	if (rxhdr->flags_monitor_vif) {
		aic_rx_monitor(hw, rxhdr, frame, len);
		return;
	}

	if (rxhdr->flags_is_80211_mpdu) {
		aic_rx_mgmt(hw, rxhdr, frame, len);
		return;
	}

	if (!rxhdr->flags_upload)
		return;

	vif = aic_vif_from_fw_idx(hw, rxhdr->flags_vif_idx);
	if (!vif || !vif->ndev)
		return;

	/*
	 * The firmware strips the Ethernet header off, so the frame starts with
	 * its payload and the addresses have to be taken from the descriptor.
	 * Leave room for the header plus NET_IP_ALIGN so that the IP header
	 * ends up aligned.
	 */
	skb = napi_alloc_skb(&hw->napi, len + ETH_HLEN + NET_IP_ALIGN);
	if (!skb) {
		vif->stats.rx_dropped++;
		return;
	}

	skb_reserve(skb, NET_IP_ALIGN);
	skb_put_data(skb, frame, len);

	skb->dev = vif->ndev;
	skb->protocol = eth_type_trans(skb, vif->ndev);
	skb->ip_summed = CHECKSUM_UNNECESSARY;

	vif->stats.rx_packets++;
	vif->stats.rx_bytes += len;

	napi_gro_receive(&hw->napi, skb);
}
