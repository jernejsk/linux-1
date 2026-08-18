// SPDX-License-Identifier: GPL-2.0-only
/*
 * cfg80211 glue for AICSemi AIC8800 series wireless devices.
 *
 * The firmware is a full MAC, so most operations are a straight translation of
 * a cfg80211 call into one firmware message.
 *
 * Copyright (C) 2026 Jernej Skrabec <jernej.skrabec@gmail.com>
 */

#include <linux/etherdevice.h>
#include <linux/if_arp.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <linux/module.h>
#include <linux/rtnetlink.h>
#include <net/cfg80211.h>

#include "aic8800.h"

#define AIC_RATE(_bitrate, _hw_value, _flags) {			\
	.bitrate = (_bitrate),					\
	.hw_value = (_hw_value),				\
	.flags = (_flags),					\
}

#define AIC_CHAN2G(_channel) {					\
	.band = NL80211_BAND_2GHZ,				\
	.center_freq = 2407 + 5 * (_channel),			\
	.hw_value = (_channel),					\
	.max_power = 20,					\
}

#define AIC_CHAN5G(_channel) {					\
	.band = NL80211_BAND_5GHZ,				\
	.center_freq = 5000 + 5 * (_channel),			\
	.hw_value = (_channel),					\
	.max_power = 20,					\
}

static struct ieee80211_rate aic_rates[] = {
	AIC_RATE(10, 0x00, 0),
	AIC_RATE(20, 0x01, IEEE80211_RATE_SHORT_PREAMBLE),
	AIC_RATE(55, 0x02, IEEE80211_RATE_SHORT_PREAMBLE),
	AIC_RATE(110, 0x03, IEEE80211_RATE_SHORT_PREAMBLE),
	AIC_RATE(60, 0x04, 0),
	AIC_RATE(90, 0x05, 0),
	AIC_RATE(120, 0x06, 0),
	AIC_RATE(180, 0x07, 0),
	AIC_RATE(240, 0x08, 0),
	AIC_RATE(360, 0x09, 0),
	AIC_RATE(480, 0x0a, 0),
	AIC_RATE(540, 0x0b, 0),
};

/* The OFDM rates start here, the first four entries are CCK only. */
#define AIC_RATES_OFDM_OFFSET	4

static struct ieee80211_channel aic_2ghz_channels[] = {
	AIC_CHAN2G(1), AIC_CHAN2G(2), AIC_CHAN2G(3), AIC_CHAN2G(4),
	AIC_CHAN2G(5), AIC_CHAN2G(6), AIC_CHAN2G(7), AIC_CHAN2G(8),
	AIC_CHAN2G(9), AIC_CHAN2G(10), AIC_CHAN2G(11), AIC_CHAN2G(12),
	AIC_CHAN2G(13), AIC_CHAN2G(14),
};

static struct ieee80211_channel aic_5ghz_channels[] = {
	AIC_CHAN5G(36), AIC_CHAN5G(40), AIC_CHAN5G(44), AIC_CHAN5G(48),
	AIC_CHAN5G(52), AIC_CHAN5G(56), AIC_CHAN5G(60), AIC_CHAN5G(64),
	AIC_CHAN5G(100), AIC_CHAN5G(104), AIC_CHAN5G(108), AIC_CHAN5G(112),
	AIC_CHAN5G(116), AIC_CHAN5G(120), AIC_CHAN5G(124), AIC_CHAN5G(128),
	AIC_CHAN5G(132), AIC_CHAN5G(136), AIC_CHAN5G(140), AIC_CHAN5G(144),
	AIC_CHAN5G(149), AIC_CHAN5G(153), AIC_CHAN5G(157), AIC_CHAN5G(161),
	AIC_CHAN5G(165),
};

/*
 * The device is a single spatial stream part supporting up to 80 MHz, with
 * LDPC, short GI and 40 MHz operation in the 2.4 GHz band.
 */
#define AIC_HT_CAP							\
	(IEEE80211_HT_CAP_LDPC_CODING | IEEE80211_HT_CAP_SGI_20 |	\
	 IEEE80211_HT_CAP_SGI_40 | IEEE80211_HT_CAP_SUP_WIDTH_20_40 |	\
	 IEEE80211_HT_CAP_RX_STBC |					\
	 (1 << IEEE80211_HT_CAP_RX_STBC_SHIFT))

#define AIC_VHT_CAP							\
	(IEEE80211_VHT_CAP_RXLDPC | IEEE80211_VHT_CAP_SHORT_GI_80 |	\
	 IEEE80211_VHT_CAP_RXSTBC_1 |					\
	 (IEEE80211_VHT_MAX_AMPDU_1024K <<				\
	  IEEE80211_VHT_CAP_MAX_A_MPDU_LENGTH_EXPONENT_SHIFT))

static const struct ieee80211_sta_he_cap aic_he_cap = {
	.has_he = true,
	.he_cap_elem = {
		.mac_cap_info[0] = IEEE80211_HE_MAC_CAP0_HTC_HE,
		.mac_cap_info[1] = IEEE80211_HE_MAC_CAP1_TF_MAC_PAD_DUR_16US,
		.mac_cap_info[2] = IEEE80211_HE_MAC_CAP2_BSR |
				   IEEE80211_HE_MAC_CAP2_ACK_EN,
		.mac_cap_info[3] = IEEE80211_HE_MAC_CAP3_OMI_CONTROL |
				   IEEE80211_HE_MAC_CAP3_MAX_AMPDU_LEN_EXP_EXT_1,
		.mac_cap_info[4] = IEEE80211_HE_MAC_CAP4_AMSDU_IN_AMPDU,
		.phy_cap_info[0] =
			IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_40MHZ_IN_2G |
			IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_40MHZ_80MHZ_IN_5G,
		.phy_cap_info[1] =
			IEEE80211_HE_PHY_CAP1_LDPC_CODING_IN_PAYLOAD,
		.phy_cap_info[2] =
			IEEE80211_HE_PHY_CAP2_NDP_4x_LTF_AND_3_2US,
		.phy_cap_info[3] =
			IEEE80211_HE_PHY_CAP3_DCM_MAX_CONST_RX_16_QAM,
		.phy_cap_info[6] =
			IEEE80211_HE_PHY_CAP6_PARTIAL_BW_EXT_RANGE,
		.phy_cap_info[8] =
			IEEE80211_HE_PHY_CAP8_20MHZ_IN_40MHZ_HE_PPDU_IN_2G,
		.phy_cap_info[9] =
			IEEE80211_HE_PHY_CAP9_NOMINAL_PKT_PADDING_16US,
	},
	.he_mcs_nss_supp = {
		/* one stream, MCS 0-11 */
		.rx_mcs_80 = cpu_to_le16(0xfffc),
		.tx_mcs_80 = cpu_to_le16(0xfffc),
		.rx_mcs_160 = cpu_to_le16(0xffff),
		.tx_mcs_160 = cpu_to_le16(0xffff),
		.rx_mcs_80p80 = cpu_to_le16(0xffff),
		.tx_mcs_80p80 = cpu_to_le16(0xffff),
	},
};

static struct ieee80211_sband_iftype_data aic_he_iftype_data[] = {
	{
		.types_mask = BIT(NL80211_IFTYPE_STATION) |
			      BIT(NL80211_IFTYPE_AP),
	},
};

static struct ieee80211_supported_band aic_band_2ghz = {
	.channels = aic_2ghz_channels,
	.n_channels = ARRAY_SIZE(aic_2ghz_channels),
	.bitrates = aic_rates,
	.n_bitrates = ARRAY_SIZE(aic_rates),
	.ht_cap = {
		.ht_supported = true,
		.cap = AIC_HT_CAP,
		.ampdu_factor = IEEE80211_HT_MAX_AMPDU_64K,
		.ampdu_density = IEEE80211_HT_MPDU_DENSITY_16,
		.mcs = {
			.rx_mask = { 0xff },
			.rx_highest = cpu_to_le16(150),
			.tx_params = IEEE80211_HT_MCS_TX_DEFINED,
		},
	},
};

static struct ieee80211_supported_band aic_band_5ghz = {
	.channels = aic_5ghz_channels,
	.n_channels = ARRAY_SIZE(aic_5ghz_channels),
	.bitrates = aic_rates + AIC_RATES_OFDM_OFFSET,
	.n_bitrates = ARRAY_SIZE(aic_rates) - AIC_RATES_OFDM_OFFSET,
	.ht_cap = {
		.ht_supported = true,
		.cap = AIC_HT_CAP,
		.ampdu_factor = IEEE80211_HT_MAX_AMPDU_64K,
		.ampdu_density = IEEE80211_HT_MPDU_DENSITY_16,
		.mcs = {
			.rx_mask = { 0xff },
			.rx_highest = cpu_to_le16(150),
			.tx_params = IEEE80211_HT_MCS_TX_DEFINED,
		},
	},
	.vht_cap = {
		.vht_supported = true,
		.cap = AIC_VHT_CAP,
		.vht_mcs = {
			/* MCS 0-9 on one stream */
			.rx_mcs_map = cpu_to_le16(0xfffe),
			.tx_mcs_map = cpu_to_le16(0xfffe),
			.rx_highest = cpu_to_le16(433),
			.tx_highest = cpu_to_le16(433),
		},
	},
};

/*
 * Every type named here has to be advertised in wiphy::interface_modes as
 * well, or cfg80211 refuses the whole combination.
 */
static const struct ieee80211_iface_limit aic_iface_limits[] = {
	{
		.max = 1,
		.types = BIT(NL80211_IFTYPE_STATION),
	},
	{
		.max = 1,
		.types = BIT(NL80211_IFTYPE_AP),
	},
	{
		.max = 1,
		.types = BIT(NL80211_IFTYPE_MONITOR),
	},
};

static const struct ieee80211_iface_combination aic_iface_combinations[] = {
	{
		.limits = aic_iface_limits,
		.n_limits = ARRAY_SIZE(aic_iface_limits),
		.max_interfaces = 2,
		.num_different_channels = 1,
	},
};

/*
 * The firmware hands every management frame it does not consume itself to the
 * host, and takes any management frame for transmission.
 */
static const struct ieee80211_txrx_stypes
aic_mgmt_stypes[NUM_NL80211_IFTYPES] = {
	[NL80211_IFTYPE_STATION] = {
		.tx = 0xffff,
		.rx = BIT(IEEE80211_STYPE_ACTION >> 4) |
		      BIT(IEEE80211_STYPE_PROBE_REQ >> 4) |
		      BIT(IEEE80211_STYPE_AUTH >> 4),
	},
	[NL80211_IFTYPE_AP] = {
		.tx = 0xffff,
		.rx = BIT(IEEE80211_STYPE_ASSOC_REQ >> 4) |
		      BIT(IEEE80211_STYPE_REASSOC_REQ >> 4) |
		      BIT(IEEE80211_STYPE_PROBE_REQ >> 4) |
		      BIT(IEEE80211_STYPE_DISASSOC >> 4) |
		      BIT(IEEE80211_STYPE_AUTH >> 4) |
		      BIT(IEEE80211_STYPE_DEAUTH >> 4) |
		      BIT(IEEE80211_STYPE_ACTION >> 4),
	},
};

/*
 * Wake up patterns are matched by the firmware against the whole received
 * frame, with one mask byte per pattern byte.
 */
#define AIC_WOW_PATTERN_MAX_LEN		64
#define AIC_WOW_PATTERN_MAX_OFFSET	255

static const struct wiphy_wowlan_support aic_wowlan_support = {
	.flags = WIPHY_WOWLAN_MAGIC_PKT | WIPHY_WOWLAN_ANY,
	.n_patterns = 1,
	.pattern_min_len = 1,
	.pattern_max_len = AIC_WOW_PATTERN_MAX_LEN,
	.max_pkt_offset = AIC_WOW_PATTERN_MAX_OFFSET,
};

static const u32 aic_cipher_suites[] = {
	WLAN_CIPHER_SUITE_WEP40,
	WLAN_CIPHER_SUITE_WEP104,
	WLAN_CIPHER_SUITE_TKIP,
	WLAN_CIPHER_SUITE_CCMP,
	WLAN_CIPHER_SUITE_CCMP_256,
	WLAN_CIPHER_SUITE_GCMP,
	WLAN_CIPHER_SUITE_GCMP_256,
	WLAN_CIPHER_SUITE_AES_CMAC,
	WLAN_CIPHER_SUITE_BIP_CMAC_256,
	WLAN_CIPHER_SUITE_BIP_GMAC_128,
	WLAN_CIPHER_SUITE_BIP_GMAC_256,
};

/* Helpers. -----------------------------------------------------------------*/

struct aic_vif *aic_vif_from_fw_idx(struct aic_hw *hw, u8 vif_idx)
{
	struct aic_vif *vif;

	if (vif_idx == AIC_INVALID_VIF)
		return NULL;

	list_for_each_entry(vif, &hw->vifs, list)
		if (vif->vif_index == vif_idx)
			return vif;

	return NULL;
}

struct aic_sta *aic_sta_from_fw_idx(struct aic_hw *hw, u8 sta_idx)
{
	if (sta_idx >= AIC_MAX_STA || !hw->sta[sta_idx].valid)
		return NULL;

	return &hw->sta[sta_idx];
}

static u8 aic_cipher_to_fw(u32 cipher)
{
	switch (cipher) {
	case WLAN_CIPHER_SUITE_WEP40:
		return MAC_CIPHER_WEP40;
	case WLAN_CIPHER_SUITE_WEP104:
		return MAC_CIPHER_WEP104;
	case WLAN_CIPHER_SUITE_TKIP:
		return MAC_CIPHER_TKIP;
	case WLAN_CIPHER_SUITE_CCMP:
		return MAC_CIPHER_CCMP;
	case WLAN_CIPHER_SUITE_CCMP_256:
		return MAC_CIPHER_CCMP_256;
	case WLAN_CIPHER_SUITE_GCMP:
		return MAC_CIPHER_GCMP_128;
	case WLAN_CIPHER_SUITE_GCMP_256:
		return MAC_CIPHER_GCMP_256;
	case WLAN_CIPHER_SUITE_AES_CMAC:
		return MAC_CIPHER_BIP_CMAC_128;
	default:
		return MAC_CIPHER_INVALID;
	}
}

static void aic_scan_abort(struct aic_hw *hw, struct wireless_dev *wdev);

/* Network device. ----------------------------------------------------------*/

/**
 * aic_iface_start - let the firmware know about an interface
 * @hw: device
 * @vif: interface
 *
 * Must be called with the device mutex held.
 */
static int aic_iface_start(struct aic_hw *hw, struct aic_vif *vif)
{
	int ret;

	if (list_empty(&hw->vifs)) {
		ret = aic_send_start(hw);
		if (ret)
			return ret;
	}

	ret = aic_send_add_if(hw, vif->ndev->dev_addr, vif->wdev.iftype, false,
			      &vif->vif_index);
	if (ret)
		return ret;

	list_add_tail(&vif->list, &hw->vifs);
	vif->up = true;

	if (vif->wdev.iftype != NL80211_IFTYPE_MONITOR) {
		/* a power save setting from before the interface was up */
		aic_send_me_set_ps_mode(hw, hw->ps_enabled);
		return 0;
	}

	hw->monitor_vif = vif->vif_index;

	/* the channel may only be set after the interface is up */
	if (hw->chandef_monitor.chan) {
		ret = aic_send_me_config_monitor(hw, &hw->chandef_monitor, NULL);
		if (ret)
			return ret;
	}

	/* the firmware only forwards what its receive filter lets in */
	return aic_send_set_filter(hw, AIC_RX_FILTER_MONITOR);
}

/**
 * aic_iface_stop - take an interface away from the firmware
 * @hw: device
 * @vif: interface
 *
 * Must be called with the device mutex held.
 */
static void aic_iface_stop(struct aic_hw *hw, struct aic_vif *vif)
{
	if (!vif->up)
		return;

	aic_scan_abort(hw, &vif->wdev);
	aic_txq_flush_vif(hw, vif);
	aic_send_remove_if(hw, vif->vif_index);

	if (hw->monitor_vif == vif->vif_index) {
		hw->monitor_vif = AIC_INVALID_VIF;
		aic_send_set_filter(hw, AIC_RX_FILTER_DEFAULT);
	}

	list_del(&vif->list);
	vif->vif_index = AIC_INVALID_VIF;
	vif->up = false;
}

static int aic_open(struct net_device *ndev)
{
	struct aic_vif *vif = netdev_priv(ndev);
	struct aic_hw *hw = vif->hw;
	int ret;

	mutex_lock(&hw->mutex);

	ret = aic_iface_start(hw, vif);
	if (!ret) {
		netif_carrier_off(ndev);
		netif_tx_start_all_queues(ndev);
	}

	mutex_unlock(&hw->mutex);

	return ret;
}

static int aic_stop(struct net_device *ndev)
{
	struct aic_vif *vif = netdev_priv(ndev);
	struct aic_hw *hw = vif->hw;

	mutex_lock(&hw->mutex);

	netif_tx_stop_all_queues(ndev);
	netif_carrier_off(ndev);

	aic_iface_stop(hw, vif);

	mutex_unlock(&hw->mutex);

	return 0;
}

static void aic_get_stats64(struct net_device *ndev,
			    struct rtnl_link_stats64 *stats)
{
	struct aic_vif *vif = netdev_priv(ndev);

	netdev_stats_to_stats64(stats, &vif->stats);
}

static const struct net_device_ops aic_netdev_ops = {
	.ndo_open = aic_open,
	.ndo_stop = aic_stop,
	.ndo_start_xmit = aic_start_xmit,
	.ndo_get_stats64 = aic_get_stats64,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
};

/* Interface management. ----------------------------------------------------*/

static struct aic_vif *aic_interface_add(struct aic_hw *hw, const char *name,
					 unsigned char name_assign_type,
					 enum nl80211_iftype type)
{
	struct net_device *ndev;
	struct aic_vif *vif;
	int idx, ret;

	idx = ffs(hw->avail_vif_mask);
	if (!idx)
		return ERR_PTR(-EBUSY);
	idx--;

	ndev = alloc_netdev_mqs(sizeof(*vif), name, name_assign_type,
				ether_setup, AIC_AC_COUNT, 1);
	if (!ndev)
		return ERR_PTR(-ENOMEM);

	vif = netdev_priv(ndev);
	vif->hw = hw;
	vif->ndev = ndev;
	vif->drv_vif_index = idx;
	vif->vif_index = AIC_INVALID_VIF;
	vif->wdev.wiphy = hw->wiphy;
	vif->wdev.netdev = ndev;
	vif->wdev.iftype = type;

	if (type == NL80211_IFTYPE_AP || type == NL80211_IFTYPE_P2P_GO)
		INIT_LIST_HEAD(&vif->ap.sta_list);

	ndev->netdev_ops = &aic_netdev_ops;
	ndev->ieee80211_ptr = &vif->wdev;
	if (type == NL80211_IFTYPE_MONITOR)
		ndev->type = ARPHRD_IEEE80211_RADIOTAP;
	ndev->needed_headroom = sizeof(struct aic_txdesc);
	ndev->features |= NETIF_F_SG;
	SET_NETDEV_DEV(ndev, wiphy_dev(hw->wiphy));

	eth_hw_addr_set(ndev, hw->wiphy->perm_addr);
	if (idx) {
		u8 addr[ETH_ALEN];

		ether_addr_copy(addr, hw->wiphy->perm_addr);
		addr[ETH_ALEN - 1] ^= idx;
		addr[0] |= 0x02;
		eth_hw_addr_set(ndev, addr);
	}

	ret = cfg80211_register_netdevice(ndev);
	if (ret) {
		free_netdev(ndev);
		return ERR_PTR(ret);
	}

	hw->vif[idx] = vif;
	hw->avail_vif_mask &= ~BIT(idx);

	return vif;
}

static void aic_interface_remove(struct aic_hw *hw, struct aic_vif *vif)
{
	hw->vif[vif->drv_vif_index] = NULL;
	hw->avail_vif_mask |= BIT(vif->drv_vif_index);
	cfg80211_unregister_netdevice(vif->ndev);
	free_netdev(vif->ndev);
}

static struct wireless_dev *aic_cfg_add_iface(struct wiphy *wiphy,
					      const char *name,
					      unsigned char name_assign_type,
					      enum nl80211_iftype type,
					      struct vif_params *params)
{
	struct aic_hw *hw = wiphy_priv(wiphy);
	struct aic_vif *vif;

	vif = aic_interface_add(hw, name, name_assign_type, type);
	if (IS_ERR(vif))
		return ERR_CAST(vif);

	return &vif->wdev;
}

static int aic_cfg_del_iface(struct wiphy *wiphy, struct wireless_dev *wdev)
{
	struct aic_hw *hw = wiphy_priv(wiphy);
	struct aic_vif *vif = container_of(wdev, struct aic_vif, wdev);

	aic_interface_remove(hw, vif);

	return 0;
}

static int aic_cfg_change_iface(struct wiphy *wiphy, struct net_device *ndev,
				enum nl80211_iftype type,
				struct vif_params *params)
{
	struct aic_vif *vif = netdev_priv(ndev);
	struct aic_hw *hw = wiphy_priv(wiphy);
	int ret = 0;

	mutex_lock(&hw->mutex);

	/* the firmware cannot change the type of an interface it knows */
	aic_iface_stop(hw, vif);

	vif->wdev.iftype = type;
	if (params->use_4addr != -1)
		vif->use_4addr = params->use_4addr;

	if (type == NL80211_IFTYPE_AP || type == NL80211_IFTYPE_P2P_GO)
		INIT_LIST_HEAD(&vif->ap.sta_list);
	else
		memset(&vif->sta, 0, sizeof(vif->sta));

	ndev->type = type == NL80211_IFTYPE_MONITOR ?
		     ARPHRD_IEEE80211_RADIOTAP : ARPHRD_ETHER;

	if (netif_running(ndev))
		ret = aic_iface_start(hw, vif);

	mutex_unlock(&hw->mutex);

	return ret;
}

/* Scanning. ----------------------------------------------------------------*/

static int aic_cfg_scan(struct wiphy *wiphy,
			struct cfg80211_scan_request *request)
{
	struct aic_hw *hw = wiphy_priv(wiphy);
	struct aic_vif *vif;
	int ret;

	vif = container_of(request->wdev, struct aic_vif, wdev);
	if (!vif->up)
		return -EBUSY;

	mutex_lock(&hw->mutex);

	if (hw->scan_req) {
		ret = -EBUSY;
		goto out;
	}

	/* the elements are kept by the firmware until the next scan */
	ret = aic_send_scanu_vendor_ie(hw, vif, request->ie, request->ie_len);
	if (ret)
		goto out;

	hw->scan_req = request;
	ret = aic_send_scanu_req(hw, vif, request);
	if (ret)
		hw->scan_req = NULL;

out:
	mutex_unlock(&hw->mutex);

	return ret;
}

/**
 * aic_scan_abort - stop a scan and tell cfg80211 it is over
 * @hw: device
 * @wdev: interface the scan has to belong to, %NULL for any
 *
 * The firmware answers a cancelled scan with the same confirmation as a
 * completed one, but not reliably, so the scan is completed here and the
 * confirmation is then ignored.  Must be called with the device mutex held.
 */
static void aic_scan_abort(struct aic_hw *hw, struct wireless_dev *wdev)
{
	struct cfg80211_scan_info info = { .aborted = true };
	struct cfg80211_scan_request *req = hw->scan_req;

	if (!req || (wdev && req->wdev != wdev))
		return;

	hw->scan_req = NULL;
	aic_send_scanu_cancel(hw);
	cfg80211_scan_done(req, &info);
}

static void aic_cfg_abort_scan(struct wiphy *wiphy, struct wireless_dev *wdev)
{
	struct aic_hw *hw = wiphy_priv(wiphy);

	mutex_lock(&hw->mutex);
	aic_scan_abort(hw, wdev);
	mutex_unlock(&hw->mutex);
}

/* Connection management. ---------------------------------------------------*/

static int aic_cfg_connect(struct wiphy *wiphy, struct net_device *ndev,
			   struct cfg80211_connect_params *sme)
{
	struct aic_hw *hw = wiphy_priv(wiphy);
	struct aic_vif *vif = netdev_priv(ndev);
	int ret;

	if (!vif->up)
		return -EBUSY;

	mutex_lock(&hw->mutex);
	ret = aic_send_sm_connect(hw, vif, sme);
	mutex_unlock(&hw->mutex);

	return ret;
}

static int aic_cfg_disconnect(struct wiphy *wiphy, struct net_device *ndev,
			      u16 reason_code)
{
	struct aic_hw *hw = wiphy_priv(wiphy);
	struct aic_vif *vif = netdev_priv(ndev);
	int ret;

	mutex_lock(&hw->mutex);

	/*
	 * Both supplicants disconnect interfaces they never connected, and the
	 * firmware answers that with an error.
	 */
	if (!vif->up || !vif->sta.ap)
		ret = 0;
	else
		ret = aic_send_sm_disconnect(hw, vif, reason_code);

	mutex_unlock(&hw->mutex);

	return ret;
}

static int aic_cfg_external_auth(struct wiphy *wiphy, struct net_device *ndev,
				 struct cfg80211_external_auth_params *params)
{
	struct aic_hw *hw = wiphy_priv(wiphy);
	struct aic_vif *vif = netdev_priv(ndev);

	if (!vif->up)
		return -EBUSY;

	return aic_send_sm_external_auth_rsp(hw, vif->vif_index,
					     params->status);
}

/* Keys. --------------------------------------------------------------------*/

static int aic_cfg_add_key(struct wiphy *wiphy, struct wireless_dev *wdev,
			   int link_id, u8 key_index, bool pairwise,
			   const u8 *mac_addr, struct key_params *params)
{
	struct aic_vif *vif = container_of(wdev, struct aic_vif, wdev);
	struct aic_hw *hw = wiphy_priv(wiphy);
	struct aic_sta *sta = NULL;
	u8 cipher, hw_key_idx;
	u8 sta_idx = AIC_INVALID_STA;
	int ret;

	if (!vif->up)
		return -EBUSY;

	cipher = aic_cipher_to_fw(params->cipher);
	if (cipher == MAC_CIPHER_INVALID)
		return -EOPNOTSUPP;

	if (mac_addr) {
		struct aic_sta *iter;

		if (vif->wdev.iftype == NL80211_IFTYPE_STATION) {
			if (vif->sta.ap &&
			    ether_addr_equal(vif->sta.ap->addr, mac_addr))
				sta = vif->sta.ap;
		} else {
			list_for_each_entry(iter, &vif->ap.sta_list, list) {
				if (ether_addr_equal(iter->addr, mac_addr)) {
					sta = iter;
					break;
				}
			}
		}
		if (!sta)
			return -ENOENT;
		sta_idx = sta->sta_idx;
	}

	mutex_lock(&hw->mutex);
	ret = aic_send_key_add(hw, vif->vif_index, sta_idx, pairwise,
			       params->key, params->key_len, key_index, cipher,
			       &hw_key_idx);
	if (!ret) {
		if (sta)
			sta->hw_key_idx = hw_key_idx;
		else
			vif->key_hw_idx[key_index] = hw_key_idx;
	}
	mutex_unlock(&hw->mutex);

	return ret;
}

static int aic_cfg_get_key(struct wiphy *wiphy, struct wireless_dev *wdev,
			   int link_id, u8 key_index, bool pairwise,
			   const u8 *mac_addr, void *cookie,
			   void (*callback)(void *cookie,
					    struct key_params *))
{
	return -EOPNOTSUPP;
}

static int aic_cfg_del_key(struct wiphy *wiphy, struct wireless_dev *wdev,
			   int link_id, u8 key_index, bool pairwise,
			   const u8 *mac_addr)
{
	struct aic_vif *vif = container_of(wdev, struct aic_vif, wdev);
	struct aic_hw *hw = wiphy_priv(wiphy);
	u8 hw_key_idx;
	int ret;

	if (!vif->up)
		return 0;

	if (mac_addr) {
		struct aic_sta *sta = NULL, *iter;

		if (vif->wdev.iftype == NL80211_IFTYPE_STATION)
			sta = vif->sta.ap;
		else
			list_for_each_entry(iter, &vif->ap.sta_list, list)
				if (ether_addr_equal(iter->addr, mac_addr)) {
					sta = iter;
					break;
				}
		if (!sta)
			return 0;
		hw_key_idx = sta->hw_key_idx;
	} else {
		hw_key_idx = vif->key_hw_idx[key_index];
	}

	mutex_lock(&hw->mutex);
	ret = aic_send_key_del(hw, hw_key_idx);
	mutex_unlock(&hw->mutex);

	return ret;
}

static int aic_cfg_set_default_key(struct wiphy *wiphy, struct net_device *ndev,
				   int link_id, u8 key_index, bool unicast,
				   bool multicast)
{
	return 0;
}

static int aic_cfg_set_default_mgmt_key(struct wiphy *wiphy,
					struct wireless_dev *wdev, int link_id,
					u8 key_index)
{
	return 0;
}

/* Peers. -------------------------------------------------------------------*/

static int aic_cfg_add_station(struct wiphy *wiphy, struct wireless_dev *wdev,
			       const u8 *mac, struct station_parameters *params)
{
	struct aic_vif *vif = container_of(wdev, struct aic_vif, wdev);
	struct aic_hw *hw = wiphy_priv(wiphy);
	struct aic_sta *sta;
	u8 sta_idx;
	int ret;

	if (!vif->up)
		return -EBUSY;

	mutex_lock(&hw->mutex);

	ret = aic_send_me_sta_add(hw, vif, params, mac, &sta_idx);
	if (ret)
		goto out;

	if (sta_idx >= AIC_MAX_STA) {
		ret = -EIO;
		goto out;
	}

	sta = &hw->sta[sta_idx];
	aic_sta_init(sta, sta_idx);
	sta->vif_idx = vif->vif_index;
	sta->ch_idx = vif->ch_index;
	ether_addr_copy(sta->addr, mac);
	sta->qos = params->sta_flags_set & BIT(NL80211_STA_FLAG_WME);
	sta->uapsd_tids = params->uapsd_queues;

	if (vif->wdev.iftype == NL80211_IFTYPE_AP ||
	    vif->wdev.iftype == NL80211_IFTYPE_P2P_GO)
		list_add_tail(&sta->list, &vif->ap.sta_list);

out:
	mutex_unlock(&hw->mutex);

	return ret;
}

static void aic_sta_forget(struct aic_hw *hw, struct aic_vif *vif,
			   struct aic_sta *sta)
{
	if (vif->wdev.iftype == NL80211_IFTYPE_AP ||
	    vif->wdev.iftype == NL80211_IFTYPE_P2P_GO)
		list_del(&sta->list);
	aic_sta_release(sta);
}

static int aic_cfg_del_station(struct wiphy *wiphy, struct wireless_dev *wdev,
			       struct station_del_parameters *params)
{
	struct aic_vif *vif = container_of(wdev, struct aic_vif, wdev);
	struct aic_hw *hw = wiphy_priv(wiphy);
	struct aic_sta *sta, *tmp;
	int ret = 0;

	if (!vif->up)
		return 0;

	mutex_lock(&hw->mutex);

	if (vif->wdev.iftype == NL80211_IFTYPE_AP ||
	    vif->wdev.iftype == NL80211_IFTYPE_P2P_GO) {
		list_for_each_entry_safe(sta, tmp, &vif->ap.sta_list, list) {
			if (params->mac &&
			    !ether_addr_equal(sta->addr, params->mac))
				continue;

			ret = aic_send_me_sta_del(hw, sta->sta_idx);
			aic_sta_forget(hw, vif, sta);
			if (params->mac)
				break;
		}
	} else if (vif->sta.ap) {
		ret = aic_send_me_sta_del(hw, vif->sta.ap->sta_idx);
		aic_sta_release(vif->sta.ap);
		vif->sta.ap = NULL;
	}

	mutex_unlock(&hw->mutex);

	return ret;
}

static int aic_cfg_change_station(struct wiphy *wiphy,
				  struct wireless_dev *wdev, const u8 *mac,
				  struct station_parameters *params)
{
	struct aic_vif *vif = container_of(wdev, struct aic_vif, wdev);
	struct aic_hw *hw = wiphy_priv(wiphy);
	struct aic_sta *sta;
	int ret = 0;

	if (!vif->up)
		return -EBUSY;

	mutex_lock(&hw->mutex);

	sta = aic_sta_find(hw, vif, mac);
	if (!sta) {
		ret = -ENOENT;
		goto out;
	}

	/*
	 * Both supplicants open the control port this way once the handshake
	 * they ran themselves is done.
	 */
	if (params->sta_flags_mask & BIT(NL80211_STA_FLAG_AUTHORIZED)) {
		bool authorized = params->sta_flags_set &
				  BIT(NL80211_STA_FLAG_AUTHORIZED);

		ret = aic_send_me_set_control_port(hw, sta->sta_idx, authorized);
	}

out:
	mutex_unlock(&hw->mutex);

	return ret;
}

static int aic_cfg_get_station(struct wiphy *wiphy, struct wireless_dev *wdev,
			       const u8 *mac, struct station_info *sinfo)
{
	struct aic_vif *vif = container_of(wdev, struct aic_vif, wdev);
	struct aic_hw *hw = wiphy_priv(wiphy);
	struct aic_sta *sta;

	sinfo->filled = BIT_ULL(NL80211_STA_INFO_RX_PACKETS) |
			BIT_ULL(NL80211_STA_INFO_TX_PACKETS) |
			BIT_ULL(NL80211_STA_INFO_RX_BYTES) |
			BIT_ULL(NL80211_STA_INFO_TX_BYTES);
	sinfo->rx_packets = vif->stats.rx_packets;
	sinfo->tx_packets = vif->stats.tx_packets;
	sinfo->rx_bytes = vif->stats.rx_bytes;
	sinfo->tx_bytes = vif->stats.tx_bytes;

	/*
	 * Both supplicants poll the signal of the peer they are connected to,
	 * and roam on it, so report the last one the receive path saw.
	 */
	sta = aic_sta_find(hw, vif, mac);
	if (sta && sta->last_rssi) {
		sinfo->filled |= BIT_ULL(NL80211_STA_INFO_SIGNAL);
		sinfo->signal = sta->last_rssi;
	}

	return 0;
}

static int aic_cfg_dump_station(struct wiphy *wiphy, struct wireless_dev *wdev,
				int idx, u8 *mac, struct station_info *sinfo)
{
	struct aic_vif *vif = container_of(wdev, struct aic_vif, wdev);
	struct aic_hw *hw = wiphy_priv(wiphy);
	struct aic_sta *sta = NULL;
	int i = 0;

	mutex_lock(&hw->mutex);

	switch (vif->wdev.iftype) {
	case NL80211_IFTYPE_STATION:
	case NL80211_IFTYPE_P2P_CLIENT:
		if (idx == 0)
			sta = vif->sta.ap;
		break;
	case NL80211_IFTYPE_AP:
	case NL80211_IFTYPE_P2P_GO:
		list_for_each_entry(sta, &vif->ap.sta_list, list)
			if (i++ == idx)
				break;
		if (i <= idx)
			sta = NULL;
		break;
	default:
		break;
	}

	if (sta && sta->valid)
		ether_addr_copy(mac, sta->addr);

	mutex_unlock(&hw->mutex);

	if (!sta || !sta->valid)
		return -ENOENT;

	return aic_cfg_get_station(wiphy, wdev, mac, sinfo);
}

/* AP mode. -----------------------------------------------------------------*/

/*
 * Build the beacon the firmware transmits from the head and tail cfg80211
 * hands over, and locate the TIM element so the firmware can maintain it.
 */
static struct sk_buff *aic_build_beacon(struct cfg80211_beacon_data *bcn,
					u16 *tim_oft, u8 *tim_len)
{
	static const u8 tim[] = { WLAN_EID_TIM, 4, 0, 0, 0, 0 };
	struct sk_buff *skb;
	unsigned int len;

	len = bcn->head_len + sizeof(tim) + bcn->tail_len;
	skb = dev_alloc_skb(len);
	if (!skb)
		return NULL;

	skb_put_data(skb, bcn->head, bcn->head_len);
	*tim_oft = skb->len;
	*tim_len = sizeof(tim);
	skb_put_data(skb, tim, sizeof(tim));
	if (bcn->tail_len)
		skb_put_data(skb, bcn->tail, bcn->tail_len);

	return skb;
}

static int aic_cfg_start_ap(struct wiphy *wiphy, struct net_device *ndev,
			    struct cfg80211_ap_settings *settings)
{
	struct aic_vif *vif = netdev_priv(ndev);
	struct aic_hw *hw = wiphy_priv(wiphy);
	struct aic_sta *bcmc;
	struct sk_buff *bcn;
	u8 ch_idx, bcmc_idx;
	u16 tim_oft;
	u8 tim_len;
	int ret;

	if (!vif->up)
		return -EBUSY;

	bcn = aic_build_beacon(&settings->beacon, &tim_oft, &tim_len);
	if (!bcn)
		return -ENOMEM;

	mutex_lock(&hw->mutex);

	ret = aic_send_apm_start(hw, vif, settings, bcn, tim_oft, tim_len,
				 &ch_idx, &bcmc_idx);
	if (ret)
		goto out;

	vif->ch_index = ch_idx;
	vif->ap.bcmc_idx = bcmc_idx;
	vif->ap.started = true;

	if (bcmc_idx < AIC_MAX_STA) {
		bcmc = &hw->sta[bcmc_idx];
		aic_sta_init(bcmc, bcmc_idx);
		bcmc->vif_idx = vif->vif_index;
		bcmc->ch_idx = ch_idx;
		eth_broadcast_addr(bcmc->addr);
	}

	netif_carrier_on(ndev);

out:
	mutex_unlock(&hw->mutex);
	dev_kfree_skb(bcn);

	return ret;
}

static int aic_cfg_change_beacon(struct wiphy *wiphy, struct net_device *ndev,
				 struct cfg80211_ap_update *info)
{
	struct aic_vif *vif = netdev_priv(ndev);
	struct aic_hw *hw = wiphy_priv(wiphy);
	struct sk_buff *bcn;
	u16 tim_oft;
	u8 tim_len;
	int ret;

	if (!vif->up || !vif->ap.started)
		return -EBUSY;

	bcn = aic_build_beacon(&info->beacon, &tim_oft, &tim_len);
	if (!bcn)
		return -ENOMEM;

	mutex_lock(&hw->mutex);
	ret = aic_send_bcn(hw, vif->vif_index, bcn);
	if (!ret)
		ret = aic_send_bcn_change(hw, vif->vif_index, bcn, tim_oft,
					  tim_len, NULL);
	mutex_unlock(&hw->mutex);

	dev_kfree_skb(bcn);

	return ret;
}

static int aic_cfg_stop_ap(struct wiphy *wiphy, struct net_device *ndev,
			   unsigned int link_id)
{
	struct aic_vif *vif = netdev_priv(ndev);
	struct aic_hw *hw = wiphy_priv(wiphy);
	struct aic_sta *sta, *tmp;

	if (!vif->up)
		return 0;

	mutex_lock(&hw->mutex);

	list_for_each_entry_safe(sta, tmp, &vif->ap.sta_list, list) {
		aic_send_me_sta_del(hw, sta->sta_idx);
		aic_sta_forget(hw, vif, sta);
	}

	aic_send_apm_stop(hw, vif->vif_index);
	if (vif->ap.bcmc_idx < AIC_MAX_STA)
		aic_sta_release(&hw->sta[vif->ap.bcmc_idx]);
	vif->ap.started = false;

	netif_carrier_off(ndev);

	mutex_unlock(&hw->mutex);

	return 0;
}

static int aic_cfg_change_bss(struct wiphy *wiphy, struct net_device *ndev,
			      struct bss_parameters *params)
{
	return 0;
}

/* Monitor mode. ------------------------------------------------------------*/

static int aic_cfg_set_monitor_channel(struct wiphy *wiphy,
				       struct net_device *ndev,
				       struct cfg80211_chan_def *chandef)
{
	struct aic_hw *hw = wiphy_priv(wiphy);
	int ret;

	mutex_lock(&hw->mutex);

	hw->chandef_monitor = *chandef;
	ret = aic_send_me_config_monitor(hw, chandef, NULL);

	mutex_unlock(&hw->mutex);

	return ret;
}

/* Miscellaneous. -----------------------------------------------------------*/

static int aic_cfg_set_wiphy_params(struct wiphy *wiphy, int radio_idx,
				    u32 changed)
{
	return 0;
}

static int aic_cfg_set_txq_params(struct wiphy *wiphy, struct net_device *ndev,
				  struct ieee80211_txq_params *params)
{
	return 0;
}

static int aic_cfg_set_tx_power(struct wiphy *wiphy, struct wireless_dev *wdev,
				int radio_idx,
				enum nl80211_tx_power_setting type, int mbm)
{
	struct aic_hw *hw = wiphy_priv(wiphy);
	struct aic_vif *vif;
	s8 pwr;
	int ret = 0;

	if (type == NL80211_TX_POWER_AUTOMATIC)
		pwr = 0x7f;
	else
		pwr = MBM_TO_DBM(mbm);

	mutex_lock(&hw->mutex);
	if (wdev) {
		vif = container_of(wdev, struct aic_vif, wdev);
		if (vif->up)
			ret = aic_send_set_power(hw, vif->vif_index, pwr);
	} else {
		list_for_each_entry(vif, &hw->vifs, list) {
			ret = aic_send_set_power(hw, vif->vif_index, pwr);
			if (ret)
				break;
		}
	}
	mutex_unlock(&hw->mutex);

	return ret;
}

static int aic_cfg_get_tx_power(struct wiphy *wiphy, struct wireless_dev *wdev,
				int radio_idx, unsigned int link_id, int *dbm)
{
	*dbm = 20;

	return 0;
}

static int aic_cfg_set_power_mgmt(struct wiphy *wiphy, struct net_device *ndev,
				  bool enabled, int timeout)
{
	struct aic_hw *hw = wiphy_priv(wiphy);
	int ret = 0;

	mutex_lock(&hw->mutex);

	/*
	 * Userspace sets this on interfaces that are still down, which the
	 * firmware has no state for yet.
	 */
	if (!list_empty(&hw->vifs))
		ret = aic_send_me_set_ps_mode(hw, enabled);
	hw->ps_enabled = enabled;

	mutex_unlock(&hw->mutex);

	return ret;
}

static int aic_cfg_get_channel(struct wiphy *wiphy, struct wireless_dev *wdev,
			       unsigned int link_id,
			       struct cfg80211_chan_def *chandef)
{
	struct aic_vif *vif = container_of(wdev, struct aic_vif, wdev);
	struct aic_hw *hw = wiphy_priv(wiphy);

	if (!vif->up)
		return -ENODATA;

	if (vif->wdev.iftype == NL80211_IFTYPE_MONITOR) {
		*chandef = hw->chandef_monitor;
		return 0;
	}

	if (!vif->chandef.chan)
		return -ENODATA;

	*chandef = vif->chandef;

	return 0;
}

static int aic_cfg_remain_on_channel(struct wiphy *wiphy,
				     struct wireless_dev *wdev,
				     struct ieee80211_channel *chan,
				     unsigned int duration, u64 *cookie,
				     const u8 *rx_addr)
{
	struct aic_vif *vif = container_of(wdev, struct aic_vif, wdev);
	struct aic_hw *hw = wiphy_priv(wiphy);
	int ret;

	if (!vif->up)
		return -EBUSY;

	mutex_lock(&hw->mutex);

	if (hw->roc) {
		ret = -EBUSY;
		goto out;
	}

	ret = aic_send_roc(hw, vif, chan, duration);
	if (!ret) {
		hw->roc = wdev;
		hw->roc_chan = chan;
		hw->roc_started = false;
		*cookie = (unsigned long)wdev;
	}

out:
	mutex_unlock(&hw->mutex);

	return ret;
}

static int aic_cfg_cancel_remain_on_channel(struct wiphy *wiphy,
					    struct wireless_dev *wdev,
					    u64 cookie)
{
	struct aic_vif *vif = container_of(wdev, struct aic_vif, wdev);
	struct aic_hw *hw = wiphy_priv(wiphy);
	int ret;

	mutex_lock(&hw->mutex);
	ret = aic_send_cancel_roc(hw, vif);
	mutex_unlock(&hw->mutex);

	return ret;
}

static int aic_cfg_mgmt_tx(struct wiphy *wiphy, struct wireless_dev *wdev,
			   struct cfg80211_mgmt_tx_params *params, u64 *cookie)
{
	struct aic_vif *vif = container_of(wdev, struct aic_vif, wdev);
	const struct ieee80211_mgmt *mgmt = (const void *)params->buf;
	struct aic_hw *hw = wiphy_priv(wiphy);
	struct aic_sta *sta;
	int ret;

	if (params->len < offsetof(struct ieee80211_mgmt, u))
		return -EINVAL;

	mutex_lock(&hw->mutex);

	if (!vif->up) {
		ret = -ENETDOWN;
		goto out;
	}

	sta = aic_sta_find(hw, vif, mgmt->da);
	ret = aic_mgmt_tx(vif, sta, params, cookie);

out:
	mutex_unlock(&hw->mutex);

	return ret;
}

static int aic_cfg_mgmt_tx_cancel_wait(struct wiphy *wiphy,
				       struct wireless_dev *wdev, u64 cookie)
{
	struct aic_vif *vif = container_of(wdev, struct aic_vif, wdev);
	struct aic_hw *hw = wiphy_priv(wiphy);
	int ret = 0;

	mutex_lock(&hw->mutex);
	if (hw->roc == wdev)
		ret = aic_send_cancel_roc(hw, vif);
	mutex_unlock(&hw->mutex);

	return ret;
}

/*
 * A wake on LAN magic packet carries six 0xff bytes at the start of the UDP
 * payload, which for an untagged IPv4 frame sits at a fixed offset.
 */
#define AIC_WOW_MAGIC_OFFSET	(ETH_HLEN + sizeof(struct iphdr) + \
				 sizeof(struct udphdr))
#define AIC_WOW_MAGIC_LEN	6

static int aic_cfg_suspend(struct wiphy *wiphy, struct cfg80211_wowlan *wow)
{
	struct aic_hw *hw = wiphy_priv(wiphy);
	u8 mask[AIC_WOW_PATTERN_MAX_LEN];
	u8 pattern[AIC_WOW_PATTERN_MAX_LEN];
	u16 offset, len;
	int ret;

	if (!wow)
		return 0;

	if (wow->n_patterns) {
		const struct cfg80211_pkt_pattern *pat = &wow->patterns[0];
		int i;

		len = min_t(int, pat->pattern_len, sizeof(pattern));
		offset = pat->pkt_offset;

		/*
		 * cfg80211 masks are a bitmap with one bit per pattern byte,
		 * the firmware wants a byte mask instead.
		 */
		for (i = 0; i < len; i++) {
			mask[i] = pat->mask[i / 8] & BIT(i % 8) ? 0xff : 0x00;
			pattern[i] = pat->pattern[i];
		}
	} else if (wow->magic_pkt) {
		offset = AIC_WOW_MAGIC_OFFSET;
		len = AIC_WOW_MAGIC_LEN;
		memset(mask, 0xff, len);
		memset(pattern, 0xff, len);
	} else {
		/* WIPHY_WOWLAN_ANY, any received frame wakes the host */
		return 0;
	}

	mutex_lock(&hw->mutex);
	ret = aic_send_wakeup_info(hw, offset, mask, pattern, len);
	mutex_unlock(&hw->mutex);

	if (ret)
		dev_err(hw->dev, "failed to arm wake on wireless: %d\n", ret);

	return ret;
}

static int aic_cfg_resume(struct wiphy *wiphy)
{
	struct aic_hw *hw = wiphy_priv(wiphy);
	int ret;

	mutex_lock(&hw->mutex);
	ret = aic_send_wakeup_info(hw, 0, NULL, NULL, 0);
	mutex_unlock(&hw->mutex);

	return ret;
}

static void aic_cfg_set_wakeup(struct wiphy *wiphy, bool enabled)
{
	struct aic_hw *hw = wiphy_priv(wiphy);

	hw->wakeup_enabled = enabled;
}

static int aic_cfg_dump_survey(struct wiphy *wiphy, struct net_device *ndev,
			       int idx, struct survey_info *info)
{
	struct aic_hw *hw = wiphy_priv(wiphy);
	struct ieee80211_supported_band *band;
	int n2 = 0;

	band = wiphy->bands[NL80211_BAND_2GHZ];
	if (band)
		n2 = band->n_channels;

	if (idx < n2) {
		info->channel = &band->channels[idx];
	} else {
		band = wiphy->bands[NL80211_BAND_5GHZ];
		if (!band || idx - n2 >= band->n_channels)
			return -ENOENT;
		info->channel = &band->channels[idx - n2];
	}

	if (idx >= ARRAY_SIZE(hw->survey))
		return -ENOENT;

	/* the survey is indexed the same way, 2.4 GHz channels first */
	info->filled = 0;
	if (hw->survey[idx].filled) {
		info->filled = SURVEY_INFO_TIME |
			       SURVEY_INFO_TIME_BUSY |
			       SURVEY_INFO_NOISE_DBM;
		info->time = hw->survey[idx].chan_time_ms;
		info->time_busy = hw->survey[idx].chan_time_busy_ms;
		info->noise = hw->survey[idx].noise_dbm;
	}

	return 0;
}

const struct cfg80211_ops aic_cfg80211_ops = {
	.add_virtual_intf = aic_cfg_add_iface,
	.del_virtual_intf = aic_cfg_del_iface,
	.change_virtual_intf = aic_cfg_change_iface,
	.scan = aic_cfg_scan,
	.abort_scan = aic_cfg_abort_scan,
	.connect = aic_cfg_connect,
	.disconnect = aic_cfg_disconnect,
	.external_auth = aic_cfg_external_auth,
	.add_key = aic_cfg_add_key,
	.get_key = aic_cfg_get_key,
	.del_key = aic_cfg_del_key,
	.set_default_key = aic_cfg_set_default_key,
	.set_default_mgmt_key = aic_cfg_set_default_mgmt_key,
	.add_station = aic_cfg_add_station,
	.del_station = aic_cfg_del_station,
	.change_station = aic_cfg_change_station,
	.get_station = aic_cfg_get_station,
	.dump_station = aic_cfg_dump_station,
	.start_ap = aic_cfg_start_ap,
	.change_beacon = aic_cfg_change_beacon,
	.stop_ap = aic_cfg_stop_ap,
	.change_bss = aic_cfg_change_bss,
	.set_monitor_channel = aic_cfg_set_monitor_channel,
	.set_wiphy_params = aic_cfg_set_wiphy_params,
	.set_txq_params = aic_cfg_set_txq_params,
	.set_tx_power = aic_cfg_set_tx_power,
	.get_tx_power = aic_cfg_get_tx_power,
	.set_power_mgmt = aic_cfg_set_power_mgmt,
	.get_channel = aic_cfg_get_channel,
	.remain_on_channel = aic_cfg_remain_on_channel,
	.cancel_remain_on_channel = aic_cfg_cancel_remain_on_channel,
	.suspend = aic_cfg_suspend,
	.resume = aic_cfg_resume,
	.set_wakeup = aic_cfg_set_wakeup,
	.mgmt_tx = aic_cfg_mgmt_tx,
	.mgmt_tx_cancel_wait = aic_cfg_mgmt_tx_cancel_wait,
	.dump_survey = aic_cfg_dump_survey,
};

/**
 * aic_reg_notifier - the regulatory domain changed
 * @wiphy: cfg80211 device
 * @request: what changed and who asked for it
 *
 * cfg80211 has already applied the new domain to the channel list, so the
 * firmware only needs to be handed that list again.
 */
static void aic_reg_notifier(struct wiphy *wiphy,
			     struct regulatory_request *request)
{
	struct aic_hw *hw = wiphy_priv(wiphy);

	/* the initial domain is applied before the firmware is configured */
	if (!hw->chan_config_done)
		return;

	mutex_lock(&hw->mutex);
	if (aic_send_me_chan_config(hw))
		dev_warn(hw->dev, "failed to update the channel list for %c%c\n",
			 request->alpha2[0], request->alpha2[1]);
	mutex_unlock(&hw->mutex);
}

/* Setup. -------------------------------------------------------------------*/

static void aic_setup_bands(struct aic_hw *hw)
{
	struct wiphy *wiphy = hw->wiphy;

	aic_he_iftype_data[0].he_cap = aic_he_cap;

	aic_band_2ghz.iftype_data = aic_he_iftype_data;
	aic_band_2ghz.n_iftype_data = ARRAY_SIZE(aic_he_iftype_data);
	aic_band_5ghz.iftype_data = aic_he_iftype_data;
	aic_band_5ghz.n_iftype_data = ARRAY_SIZE(aic_he_iftype_data);

	wiphy->bands[NL80211_BAND_2GHZ] = &aic_band_2ghz;
	wiphy->bands[NL80211_BAND_5GHZ] = &aic_band_5ghz;
}

int aic_cfg80211_init(struct aic_hw *hw)
{
	struct wiphy *wiphy = hw->wiphy;
	struct aic_vif *vif;
	u8 mac[ETH_ALEN];
	int ret;

	ret = aic_send_get_mac_addr(hw, mac);
	if (ret || !is_valid_ether_addr(mac)) {
		dev_warn(hw->dev,
			 "no usable MAC address from the device, using a random one\n");
		eth_random_addr(mac);
	}
	ether_addr_copy(wiphy->perm_addr, mac);
	dev_info(hw->dev, "MAC address %pM\n", mac);

	aic_setup_bands(hw);

	wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION) |
				 BIT(NL80211_IFTYPE_AP) |
				 BIT(NL80211_IFTYPE_MONITOR);
	wiphy->iface_combinations = aic_iface_combinations;
	wiphy->n_iface_combinations = ARRAY_SIZE(aic_iface_combinations);
	/*
	 * The firmware runs the MLME but leaves the SME to userspace, on an AP
	 * interface as much as on a station one: it hands the authentication
	 * and association frames over rather than answering them, and it does
	 * not take a PSK to do the four way handshake with.
	 */
	wiphy->flags |= WIPHY_FLAG_HAS_REMAIN_ON_CHANNEL |
			WIPHY_FLAG_4ADDR_STATION |
			WIPHY_FLAG_4ADDR_AP;
	wiphy->features |= NL80211_FEATURE_SAE |
			   NL80211_FEATURE_NEED_OBSS_SCAN;

	wiphy->max_scan_ssids = SCAN_SSID_MAX;
	wiphy->max_scan_ie_len = AIC_SCAN_IE_MAX;
	/* the firmware does no PMKSA caching of its own */
	wiphy->max_num_pmkids = 0;
	wiphy->max_remain_on_channel_duration = 5000;
	wiphy->cipher_suites = aic_cipher_suites;
	wiphy->n_cipher_suites = ARRAY_SIZE(aic_cipher_suites);
	wiphy->reg_notifier = aic_reg_notifier;
	wiphy->mgmt_stypes = aic_mgmt_stypes;
	wiphy->wowlan = &aic_wowlan_support;
	wiphy->signal_type = CFG80211_SIGNAL_TYPE_MBM;

	ret = aic_send_me_config(hw);
	if (ret)
		return ret;

	ret = wiphy_register(wiphy);
	if (ret) {
		dev_err(hw->dev, "failed to register the wiphy: %d\n", ret);
		return ret;
	}

	ret = aic_send_me_chan_config(hw);
	if (ret)
		goto err_unregister;

	hw->chan_config_done = true;

	rtnl_lock();
	vif = aic_interface_add(hw, "wlan%d", NET_NAME_ENUM,
				NL80211_IFTYPE_STATION);
	rtnl_unlock();
	if (IS_ERR(vif)) {
		ret = PTR_ERR(vif);
		goto err_unregister;
	}

	return 0;

err_unregister:
	wiphy_unregister(wiphy);

	return ret;
}

void aic_cfg80211_deinit(struct aic_hw *hw)
{
	int i;

	rtnl_lock();
	for (i = 0; i < AIC_MAX_VIF; i++)
		if (hw->vif[i])
			aic_interface_remove(hw, hw->vif[i]);
	rtnl_unlock();

	wiphy_unregister(hw->wiphy);
}
