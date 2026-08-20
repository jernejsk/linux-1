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
static int aic_txcfm_claim(struct aic_hw *hw, struct sk_buff *skb,
			   struct wireless_dev *wdev, u64 cookie);

netdev_tx_t aic_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct aic_vif *vif = netdev_priv(ndev);
	struct aic_hw *hw = vif->hw;
	struct aic_txdesc *desc;
	struct ethhdr eth;
	struct aic_sta *sta;
	bool eapol;
	u16 len;
	u8 tid;

	if (skb->len <= sizeof(eth))
		goto drop;

	/*
	 * The handshake that opens the controlled port travels as data, and the
	 * firmware confirms those frames, which turns a lost one into something
	 * visible instead of an association that quietly stalls.
	 */
	eapol = skb->protocol == htons(ETH_P_PAE);

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

	if (eapol) {
		int idx = aic_txcfm_claim(hw, skb, NULL, 0);

		if (idx >= 0)
			desc->hostid = cpu_to_le32(AIC_TXDESC_HOSTID_CFM | idx);
	}

	vif->stats.tx_packets++;
	vif->stats.tx_bytes += len;

	spin_lock_bh(&hw->tx_lock);
	if (sta->ps_active) {
		__skb_queue_tail(&sta->ps_queue, skb);
		spin_unlock_bh(&hw->tx_lock);

		schedule_work(&hw->ps_work);

		return NETDEV_TX_OK;
	}
	spin_unlock_bh(&hw->tx_lock);

	hw->bus_ops->send_data(hw, skb);

	return NETDEV_TX_OK;

drop:
	vif->stats.tx_dropped++;
	dev_kfree_skb_any(skb);

	return NETDEV_TX_OK;
}

/**
 * aic_sta_find - look a peer of one interface up by address
 * @hw: device
 * @vif: interface the peer belongs to
 * @addr: peer address, %NULL for the peer of a station interface
 */
struct aic_sta *aic_sta_find(struct aic_hw *hw, struct aic_vif *vif,
			     const u8 *addr)
{
	struct aic_sta *sta;

	switch (vif->wdev.iftype) {
	case NL80211_IFTYPE_STATION:
	case NL80211_IFTYPE_P2P_CLIENT:
		sta = vif->sta.ap;
		if (sta && sta->valid &&
		    (!addr || ether_addr_equal(sta->addr, addr)))
			return sta;
		break;
	case NL80211_IFTYPE_AP:
	case NL80211_IFTYPE_P2P_GO:
		if (!addr)
			break;
		list_for_each_entry(sta, &vif->ap.sta_list, list)
			if (sta->valid && ether_addr_equal(sta->addr, addr))
				return sta;
		break;
	default:
		break;
	}

	return NULL;
}

/**
 * aic_sta_init - prepare a peer entry for use
 * @sta: peer entry, previously unused or released
 * @sta_idx: firmware index of the peer
 */
void aic_sta_init(struct aic_sta *sta, u8 sta_idx)
{
	/* the queue was drained by aic_sta_release() before we got here */
	memset(sta, 0, sizeof(*sta));
	skb_queue_head_init(&sta->ps_queue);
	sta->sta_idx = sta_idx;
	sta->valid = true;
}

/**
 * aic_sta_release - drop a peer entry and everything held for it
 * @hw: device
 * @sta: peer entry
 */
void aic_sta_release(struct aic_hw *hw, struct aic_sta *sta)
{
	unsigned int t, i;

	sta->valid = false;
	sta->ps_active = false;
	skb_queue_purge(&sta->ps_queue);

	/* whatever the reorder windows still hold goes nowhere now */
	spin_lock_bh(&hw->rx_lock);
	for (t = 0; t < IEEE80211_NUM_TIDS; t++) {
		struct aic_reord_tid *tid = &sta->reord[t];

		for (i = 0; i < AIC_REORD_WIN; i++) {
			dev_kfree_skb_any(tid->buf[i]);
			tid->buf[i] = NULL;
		}
		tid->stored = 0;
		tid->started = false;
	}
	spin_unlock_bh(&hw->rx_lock);
}

/**
 * aic_txq_ps_work - tell the firmware which peers have traffic waiting
 * @work: work item in &aic_hw
 *
 * The firmware sets the traffic indication bit in the beacon from this, and it
 * cannot be told from the contexts frames are queued in, so it happens here.
 */
void aic_txq_ps_work(struct work_struct *work)
{
	struct aic_hw *hw = container_of(work, struct aic_hw, ps_work);
	int i;

	mutex_lock(&hw->mutex);

	for (i = 0; i < AIC_MAX_STA; i++) {
		struct aic_sta *sta = &hw->sta[i];
		bool waiting;

		if (!sta->valid)
			continue;

		spin_lock_bh(&hw->tx_lock);
		waiting = !skb_queue_empty(&sta->ps_queue);
		spin_unlock_bh(&hw->tx_lock);

		if (waiting == sta->ps_announced)
			continue;

		if (!aic_send_me_traffic_ind(hw, sta->sta_idx, false, waiting))
			sta->ps_announced = waiting;
	}

	mutex_unlock(&hw->mutex);
}

/**
 * aic_txq_ps_change - a peer went to sleep or woke up
 * @hw: device
 * @sta: peer
 * @asleep: the peer is now asleep
 *
 * Frames for a sleeping peer are held back until the firmware asks for them,
 * which it does once the peer polls for its traffic.
 */
void aic_txq_ps_change(struct aic_hw *hw, struct aic_sta *sta, bool asleep)
{
	struct sk_buff_head release;
	struct sk_buff *skb;

	__skb_queue_head_init(&release);

	spin_lock_bh(&hw->tx_lock);
	sta->ps_active = asleep;
	if (!asleep)
		skb_queue_splice_tail_init(&sta->ps_queue, &release);
	spin_unlock_bh(&hw->tx_lock);

	if (asleep)
		return;

	while ((skb = __skb_dequeue(&release)))
		hw->bus_ops->send_data(hw, skb);

	schedule_work(&hw->ps_work);
}

/**
 * aic_txq_ps_release - let a sleeping peer have some of its traffic
 * @hw: device
 * @sta: peer
 * @pkt_cnt: number of frames to release, zero for all of them
 */
void aic_txq_ps_release(struct aic_hw *hw, struct aic_sta *sta, u8 pkt_cnt)
{
	struct sk_buff_head release;
	struct sk_buff *skb;

	__skb_queue_head_init(&release);

	spin_lock_bh(&hw->tx_lock);
	while (!skb_queue_empty(&sta->ps_queue)) {
		if (pkt_cnt && release.qlen == pkt_cnt)
			break;

		__skb_queue_tail(&release, __skb_dequeue(&sta->ps_queue));
	}
	spin_unlock_bh(&hw->tx_lock);

	while ((skb = __skb_dequeue(&release)))
		hw->bus_ops->send_data(hw, skb);

	schedule_work(&hw->ps_work);
}

/**
 * aic_txcfm_claim - take a slot in the confirmation ring
 * @hw: device
 * @skb: frame the firmware is going to confirm
 * @wdev: interface to report a management frame status on, %NULL for data
 * @cookie: cookie to report the status with
 *
 * Returns the slot index, or a negative error if the ring is full.  The caller
 * owns @skb until the confirmation arrives.
 */
static int aic_txcfm_claim(struct aic_hw *hw, struct sk_buff *skb,
			   struct wireless_dev *wdev, u64 cookie)
{
	int i, idx;

	spin_lock_bh(&hw->tx_lock);
	for (i = 0; i < AIC_TXCFM_RING_SIZE; i++) {
		idx = (hw->cfm_idx + i) % AIC_TXCFM_RING_SIZE;
		if (!hw->cfm_ring[idx].skb) {
			hw->cfm_ring[idx].skb = skb;
			hw->cfm_ring[idx].wdev = wdev;
			hw->cfm_ring[idx].cookie = cookie;
			hw->cfm_idx = (idx + 1) % AIC_TXCFM_RING_SIZE;
			spin_unlock_bh(&hw->tx_lock);
			return idx;
		}
	}
	spin_unlock_bh(&hw->tx_lock);

	return -ENOSPC;
}

/**
 * aic_mgmt_tx - send one management frame
 * @vif: interface to send it on
 * @sta: peer the frame is addressed to, may be %NULL
 * @params: frame and transmission parameters from cfg80211
 * @cookie: filled in with the cookie the status will be reported with
 *
 * Management frames are handed to the firmware as complete 802.11 frames, with
 * the Ethernet addresses in the descriptor left empty.
 */
int aic_mgmt_tx(struct aic_vif *vif, struct aic_sta *sta,
		struct cfg80211_mgmt_tx_params *params, u64 *cookie)
{
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)params->buf;
	struct aic_hw *hw = vif->hw;
	struct aic_txdesc *desc;
	struct sk_buff *skb;
	u16 flags = AIC_TXDESC_F_MGMT;
	int idx;

	skb = alloc_skb(sizeof(*desc) + params->len, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	if (params->no_cck)
		flags |= AIC_TXDESC_F_MGMT_NO_CCK;
	/*
	 * Robust management frames are protected by the firmware, which needs to
	 * be told so.  Deciding that needs the action category byte.
	 */
	if (params->len > offsetofend(struct ieee80211_hdr_3addr, seq_ctrl) &&
	    _ieee80211_is_robust_mgmt_frame(hdr))
		flags |= AIC_TXDESC_F_MGMT_ROBUST;

	desc = skb_put_zero(skb, sizeof(*desc));
	desc->packet_len = cpu_to_le16(params->len);
	desc->staid = sta ? sta->sta_idx : AIC_INVALID_STA;
	desc->vif_idx = vif->vif_index;
	desc->tid = 0xff;
	desc->ac = AIC_AC_VO;
	desc->flags = cpu_to_le16(flags);
	skb_put_data(skb, params->buf, params->len);

	skb->priority = AIC_AC_VO;

	*cookie = ++hw->mgmt_cookie;

	idx = aic_txcfm_claim(hw, skb, &vif->wdev, *cookie);
	if (idx < 0) {
		dev_kfree_skb(skb);
		return idx;
	}

	desc->hostid = cpu_to_le32(AIC_TXDESC_HOSTID_CFM | idx);

	return hw->bus_ops->send_data(hw, skb);
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

	spin_lock_bh(&hw->tx_lock);
	for (i = 0; i < AIC_MAX_STA; i++)
		if (hw->sta[i].valid && hw->sta[i].vif_idx == vif->vif_index)
			skb_queue_splice_tail_init(&hw->sta[i].ps_queue, &done);
	spin_unlock_bh(&hw->tx_lock);

	while ((skb = __skb_dequeue(&done)))
		dev_kfree_skb_any(skb);

	aic_txcfm_flush(hw, vif);
}

/**
 * aic_txcfm_flush - give up on outstanding confirmations
 * @hw: device
 * @vif: interface going away, %NULL for all of them
 *
 * Management frames are reported as unacknowledged so that cfg80211 does not
 * wait for a status that is never going to arrive.
 */
void aic_txcfm_flush(struct aic_hw *hw, struct aic_vif *vif)
{
	int i;

	for (i = 0; i < AIC_TXCFM_RING_SIZE; i++) {
		struct aic_txcfm_slot slot;

		spin_lock_bh(&hw->tx_lock);
		slot = hw->cfm_ring[i];
		if (slot.skb && (!vif || slot.wdev == &vif->wdev))
			memset(&hw->cfm_ring[i], 0, sizeof(hw->cfm_ring[i]));
		else
			slot.skb = NULL;
		spin_unlock_bh(&hw->tx_lock);

		if (!slot.skb)
			continue;

		if (slot.wdev) {
			skb_pull(slot.skb, sizeof(struct aic_txdesc));
			cfg80211_mgmt_tx_status(slot.wdev, slot.cookie,
						slot.skb->data, slot.skb->len,
						false, GFP_KERNEL);
		}

		dev_kfree_skb_any(slot.skb);
	}
}

/* Receive. -----------------------------------------------------------------*/

static void aic_rx_mgmt(struct aic_hw *hw, const struct aic_rxhdr *rxhdr,
			const u8 *frame, unsigned int len)
{
	const struct ieee80211_mgmt *mgmt = (const struct ieee80211_mgmt *)frame;
	struct aic_vif *vif = aic_vif_from_fw_idx(hw, rxhdr->flags_vif_idx);
	struct cfg80211_rx_info info = {
		.freq = rxhdr->phy_info.phy_prim20_freq,
		.sig_dbm = rxhdr->vect.rx_vect1.rssi1,
		.buf = frame,
		.len = len,
	};

	if (!vif || len < offsetof(struct ieee80211_mgmt, u))
		return;

	/*
	 * Beacons and probe responses of other networks are only of interest
	 * for the overlapping BSS scan an AP interface runs.
	 */
	if (ieee80211_is_beacon(mgmt->frame_control) ||
	    ieee80211_is_probe_resp(mgmt->frame_control)) {
		cfg80211_report_obss_beacon(hw->wiphy, frame, len,
					    rxhdr->phy_info.phy_prim20_freq,
					    rxhdr->vect.rx_vect1.rssi1);
		return;
	}

	/*
	 * An unprotected disconnect from a peer that thinks it is not
	 * associated has to reach userspace as such, so that it can run an SA
	 * query rather than tear the connection down on a forged frame.
	 */
	if ((ieee80211_is_deauth(mgmt->frame_control) ||
	     ieee80211_is_disassoc(mgmt->frame_control)) && vif->ndev &&
	    len >= offsetofend(struct ieee80211_mgmt, u.deauth.reason_code) &&
	    (le16_to_cpu(mgmt->u.deauth.reason_code) ==
	     WLAN_REASON_CLASS2_FRAME_FROM_NONAUTH_STA ||
	     le16_to_cpu(mgmt->u.deauth.reason_code) ==
	     WLAN_REASON_CLASS3_FRAME_FROM_NONASSOC_STA)) {
		cfg80211_rx_unprot_mlme_mgmt(vif->ndev, frame, len);
		return;
	}

	cfg80211_rx_mgmt_ext(&vif->wdev, &info);
}

static void aic_rx_monitor(struct aic_hw *hw, const struct aic_rxhdr *rxhdr,
			   const u8 *frame, unsigned int len)
{
	/* TSFT, channel and signal strength, in the order radiotap wants */
	struct aic_rtap {
		struct ieee80211_radiotap_header hdr;
		__le64 tsft;
		__le16 chan_freq;
		__le16 chan_flags;
		s8 signal;
	} __packed *rtap;
	struct aic_vif *vif = aic_vif_from_fw_idx(hw, hw->monitor_vif);
	u16 freq = rxhdr->phy_info.phy_prim20_freq;
	struct sk_buff *skb;

	if (!vif || !vif->ndev)
		return;

	skb = dev_alloc_skb(sizeof(*rtap) + len);
	if (!skb)
		return;

	rtap = skb_put_zero(skb, sizeof(*rtap));
	rtap->hdr.it_len = cpu_to_le16(sizeof(*rtap));
	rtap->hdr.it_present = cpu_to_le32(BIT(IEEE80211_RADIOTAP_TSFT) |
					   BIT(IEEE80211_RADIOTAP_CHANNEL) |
					   BIT(IEEE80211_RADIOTAP_DBM_ANTSIGNAL));
	rtap->tsft = cpu_to_le64(((u64)le32_to_cpu(rxhdr->vect.tsf_hi) << 32) |
				 le32_to_cpu(rxhdr->vect.tsf_lo));
	rtap->chan_freq = cpu_to_le16(freq);
	rtap->chan_flags = cpu_to_le16(freq >= 4900 ?
				       IEEE80211_CHAN_5GHZ | IEEE80211_CHAN_OFDM :
				       IEEE80211_CHAN_2GHZ | IEEE80211_CHAN_DYN);
	rtap->signal = rxhdr->vect.rx_vect1.rssi1;

	skb_put_data(skb, frame, len);

	skb->dev = vif->ndev;
	skb->protocol = htons(ETH_P_802_2);
	skb_reset_mac_header(skb);
	skb->pkt_type = PACKET_OTHERHOST;
	skb->ip_summed = CHECKSUM_UNNECESSARY;

	netif_receive_skb(skb);
}

/* Length of the cipher header the firmware leaves in front of the payload. */
static unsigned int aic_rx_cipher_hdr_len(u8 decr_status)
{
	switch (decr_status) {
	case AIC_RX_DECR_WEP:
		return 4;
	case AIC_RX_DECR_TKIP:
	case AIC_RX_DECR_CCMP128:
	case AIC_RX_DECR_CCMP256:
	case AIC_RX_DECR_GCMP128:
	case AIC_RX_DECR_GCMP256:
		return 8;
	case AIC_RX_DECR_WAPI:
		return 18;
	default:
		return 0;
	}
}

/**
 * aic_rx_to_8023 - turn a received 802.11 frame into an Ethernet one
 * @skb: frame as it came from the firmware
 * @decr_status: what the firmware decrypted the frame with
 * @iftype: type of the interface the frame arrived on
 * @addr: address of that interface
 *
 * The firmware decrypts in place but hands over the whole 802.11 frame,
 * cipher header included, so that header is taken out before the rest is
 * converted the usual way.
 */
static int aic_rx_to_8023(struct sk_buff *skb, u8 decr_status,
			  enum nl80211_iftype iftype, const u8 *addr)
{
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;
	unsigned int hdrlen, cipher_len;

	if (skb->len < ieee80211_hdrlen(hdr->frame_control))
		return -EINVAL;

	hdrlen = ieee80211_hdrlen(hdr->frame_control);
	cipher_len = ieee80211_has_protected(hdr->frame_control) ?
		     aic_rx_cipher_hdr_len(decr_status) : 0;

	if (cipher_len) {
		if (skb->len < hdrlen + cipher_len)
			return -EINVAL;

		memmove(skb->data + cipher_len, skb->data, hdrlen);
		skb_pull(skb, cipher_len);
		hdr = (struct ieee80211_hdr *)skb->data;
		hdr->frame_control &= ~cpu_to_le16(IEEE80211_FCTL_PROTECTED);
	}

	return ieee80211_data_to_8023(skb, addr, iftype);
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
/* What has to survive a stay in the reorder window, kept in the buffer. */
struct aic_rx_cb {
	u8 decr_status;
	bool amsdu;
};

/*
 * Hand one converted frame to the network stack.  The 802.11 header is still in
 * front of the payload here, the conversion happens on the way out.
 */
static void aic_rx_up(struct aic_hw *hw, struct aic_vif *vif,
		      struct sk_buff *skb)
{
	skb->dev = vif->ndev;
	skb->protocol = eth_type_trans(skb, vif->ndev);
	skb->ip_summed = CHECKSUM_UNNECESSARY;

	vif->stats.rx_packets++;
	vif->stats.rx_bytes += skb->len;

	napi_gro_receive(&hw->napi, skb);
}

static void aic_rx_deliver(struct aic_hw *hw, struct aic_vif *vif,
			   struct sk_buff *skb)
{
	struct aic_rx_cb *cb = (struct aic_rx_cb *)skb->cb;
	struct sk_buff_head list;
	bool amsdu = cb->amsdu;

	if (aic_rx_to_8023(skb, cb->decr_status, vif->wdev.iftype,
			   vif->ndev->dev_addr)) {
		vif->stats.rx_dropped++;
		dev_kfree_skb_any(skb);
		return;
	}

	if (!amsdu) {
		aic_rx_up(hw, vif, skb);
		return;
	}

	/*
	 * An aggregate carries several frames behind one header, and the
	 * helper takes the buffer over whether it can split it or not.
	 */
	__skb_queue_head_init(&list);
	ieee80211_amsdu_to_8023s(skb, &list, vif->ndev->dev_addr,
				 vif->wdev.iftype, 0, NULL, NULL, false);
	if (skb_queue_empty(&list))
		vif->stats.rx_dropped++;

	while ((skb = __skb_dequeue(&list)))
		aic_rx_up(hw, vif, skb);
}

/* Everything the window holds up to the first hole, in order. */
static void aic_reord_release(struct aic_hw *hw, struct aic_vif *vif,
			      struct aic_reord_tid *tid)
{
	while (tid->stored) {
		unsigned int i = tid->head_sn % AIC_REORD_WIN;

		if (!tid->buf[i])
			break;

		aic_rx_deliver(hw, vif, tid->buf[i]);
		tid->buf[i] = NULL;
		tid->stored--;
		tid->head_sn = (tid->head_sn + 1) & IEEE80211_SN_MASK;
	}
}

/**
 * aic_reord_work - release frames a hole has been holding up for too long
 * @work: work item in &aic_hw
 *
 * A frame that never arrives must not hold the window forever, so a window
 * that has not moved within %AIC_REORD_TIMEOUT_MS is released past the hole.
 */
void aic_reord_work(struct work_struct *work)
{
	struct aic_hw *hw = container_of(to_delayed_work(work), struct aic_hw,
					 reord_work);
	bool pending = false;
	unsigned int i, t;

	local_bh_disable();
	spin_lock(&hw->rx_lock);

	for (i = 0; i < AIC_MAX_STA; i++) {
		struct aic_sta *sta = &hw->sta[i];
		struct aic_vif *vif;

		if (!sta->valid)
			continue;

		vif = aic_vif_from_fw_idx(hw, sta->vif_idx);

		for (t = 0; t < IEEE80211_NUM_TIDS; t++) {
			struct aic_reord_tid *tid = &sta->reord[t];

			if (!tid->stored)
				continue;

			if (!vif || !vif->ndev) {
				unsigned int n;

				for (n = 0; n < AIC_REORD_WIN; n++) {
					dev_kfree_skb_any(tid->buf[n]);
					tid->buf[n] = NULL;
				}
				tid->stored = 0;
				continue;
			}

			if (time_after_eq(jiffies, tid->deadline)) {
				unsigned int n;

				/* step past the hole, then take what follows */
				for (n = 0; n < AIC_REORD_WIN; n++) {
					if (tid->buf[tid->head_sn % AIC_REORD_WIN])
						break;
					tid->head_sn = (tid->head_sn + 1) &
						       IEEE80211_SN_MASK;
				}
				aic_reord_release(hw, vif, tid);
			}

			if (tid->stored)
				pending = true;
		}
	}

	spin_unlock(&hw->rx_lock);
	local_bh_enable();

	if (pending)
		schedule_delayed_work(&hw->reord_work,
				      msecs_to_jiffies(AIC_REORD_TIMEOUT_MS));
}

/**
 * aic_reord_frame - put one frame back in sequence
 * @hw: device
 * @vif: interface the frame arrived on
 * @sta: peer that sent it
 * @skb: frame, with its 802.11 header
 * @tid_nr: traffic identifier taken from the QoS control field
 *
 * Returns %true when the frame was taken over, %false when the caller should
 * deliver it as it is.
 */
static bool aic_reord_frame(struct aic_hw *hw, struct aic_vif *vif,
			    struct aic_sta *sta, struct sk_buff *skb,
			    u8 tid_nr)
{
	const struct ieee80211_hdr *hdr = (const void *)skb->data;
	struct aic_reord_tid *tid = &sta->reord[tid_nr];
	u16 sn = IEEE80211_SEQ_TO_SN(le16_to_cpu(hdr->seq_ctrl));
	unsigned int i;
	u16 ahead;

	spin_lock(&hw->rx_lock);

	if (!tid->started) {
		tid->head_sn = sn;
		tid->started = true;
	}

	ahead = (sn - tid->head_sn) & IEEE80211_SN_MASK;

	/* already released, or so far behind that it is a duplicate */
	if (ahead >= IEEE80211_SN_MODULO / 2) {
		spin_unlock(&hw->rx_lock);
		dev_kfree_skb_any(skb);

		return true;
	}

	/*
	 * A frame past the window means the sender moved on, so the window
	 * moves with it and whatever is left behind goes up as it is.
	 */
	if (ahead >= AIC_REORD_WIN) {
		u16 move = ahead - AIC_REORD_WIN + 1;

		while (move--) {
			i = tid->head_sn % AIC_REORD_WIN;
			if (tid->buf[i]) {
				aic_rx_deliver(hw, vif, tid->buf[i]);
				tid->buf[i] = NULL;
				tid->stored--;
			}
			tid->head_sn = (tid->head_sn + 1) & IEEE80211_SN_MASK;
		}
	}

	i = sn % AIC_REORD_WIN;
	if (tid->buf[i]) {
		spin_unlock(&hw->rx_lock);
		dev_kfree_skb_any(skb);

		return true;
	}

	tid->buf[i] = skb;
	tid->stored++;
	tid->deadline = jiffies + msecs_to_jiffies(AIC_REORD_TIMEOUT_MS);

	aic_reord_release(hw, vif, tid);

	spin_unlock(&hw->rx_lock);

	if (tid->stored)
		schedule_delayed_work(&hw->reord_work,
				      msecs_to_jiffies(AIC_REORD_TIMEOUT_MS));

	return true;
}

/*
 * The receive vector says how the frame arrived, in the terms of whichever
 * generation sent it.
 */
void aic_rx_rate(struct rate_info *rate, const struct aic_rx_vector_1 *vect)
{
	static const u8 widths[] = {
		RATE_INFO_BW_20, RATE_INFO_BW_40, RATE_INFO_BW_80,
		RATE_INFO_BW_160,
	};
	static const u16 leg_rates[] = {
		/* the four bit legacy code, in half megabits */
		[0] = 960, [1] = 480, [2] = 240, [3] = 120,
		[4] = 1080, [5] = 720, [6] = 360, [7] = 180,
		[8] = 22, [9] = 44, [10] = 110, [11] = 220,
	};

	memset(rate, 0, sizeof(*rate));
	rate->bw = vect->ch_bw < ARRAY_SIZE(widths) ? widths[vect->ch_bw] :
						      RATE_INFO_BW_20;

	switch (vect->format_mod) {
	case AIC_FORMATMOD_HT_MF:
	case AIC_FORMATMOD_HT_GF:
		rate->flags = RATE_INFO_FLAGS_MCS;
		rate->mcs = vect->ht.mcs;
		if (vect->ht.short_gi)
			rate->flags |= RATE_INFO_FLAGS_SHORT_GI;
		break;
	case AIC_FORMATMOD_VHT:
		rate->flags = RATE_INFO_FLAGS_VHT_MCS;
		rate->mcs = vect->vht.mcs;
		rate->nss = vect->vht.nss + 1;
		if (vect->vht.short_gi)
			rate->flags |= RATE_INFO_FLAGS_SHORT_GI;
		break;
	case AIC_FORMATMOD_HE_SU_ER:
	case AIC_FORMATMOD_HE_SU:
	case AIC_FORMATMOD_HE_MU:
	case AIC_FORMATMOD_HE_TB:
		rate->flags = RATE_INFO_FLAGS_HE_MCS;
		rate->mcs = vect->he.mcs;
		rate->nss = vect->he.nss + 1;
		rate->he_gi = vect->he.gi_type;
		break;
	default:
		rate->legacy = vect->leg_rate < ARRAY_SIZE(leg_rates) ?
			       leg_rates[vect->leg_rate] : 0;
		break;
	}
}

void aic_rx_frame(struct aic_hw *hw, const struct aic_rxhdr *rxhdr,
		  const u8 *frame, unsigned int len)
{
	const struct ieee80211_hdr *hdr;
	struct aic_sta *sta = NULL;
	struct aic_vif *vif;
	struct sk_buff *skb;
	u8 tid_nr;

	if (rxhdr->flags_monitor_vif) {
		aic_rx_monitor(hw, rxhdr, frame, len);
		return;
	}

	if (rxhdr->flags_is_80211_mpdu) {
		aic_rx_mgmt(hw, rxhdr, frame, len);
		return;
	}

	if (!rxhdr->flags_upload || hw->pm_polling)
		return;

	if (rxhdr->flags_sta_idx < AIC_MAX_STA &&
	    hw->sta[rxhdr->flags_sta_idx].valid) {
		sta = &hw->sta[rxhdr->flags_sta_idx];
		sta->last_rssi = rxhdr->vect.rx_vect1.rssi1;
		aic_rx_rate(&sta->last_rate, &rxhdr->vect.rx_vect1);
	}

	vif = aic_vif_from_fw_idx(hw, rxhdr->flags_vif_idx);
	if (!vif || !vif->ndev)
		return;

	/*
	 * Data frames arrive as 802.11 frames that the firmware has decrypted
	 * in place.  The conversion only ever makes the frame shorter, and
	 * NET_IP_ALIGN plus the room the 802.11 header leaves behind keeps the
	 * IP header aligned.
	 */
	skb = napi_alloc_skb(&hw->napi, len + NET_IP_ALIGN);
	if (!skb) {
		vif->stats.rx_dropped++;
		return;
	}

	skb_reserve(skb, NET_IP_ALIGN);
	skb_put_data(skb, frame, len);

	/* what the conversion needs, for after the reorder window */
	((struct aic_rx_cb *)skb->cb)->decr_status = rxhdr->vect.decr_status;
	((struct aic_rx_cb *)skb->cb)->amsdu = rxhdr->flags_is_amsdu;

	hdr = (const struct ieee80211_hdr *)skb->data;
	if (sta && rxhdr->flags_need_reord && skb->len >= 26 &&
	    ieee80211_is_data_qos(hdr->frame_control)) {
		tid_nr = *ieee80211_get_qos_ctl((struct ieee80211_hdr *)hdr) &
			 IEEE80211_QOS_CTL_TID_MASK;
		if (tid_nr < IEEE80211_NUM_TIDS &&
		    aic_reord_frame(hw, vif, sta, skb, tid_nr))
			return;
	}

	aic_rx_deliver(hw, vif, skb);
}
