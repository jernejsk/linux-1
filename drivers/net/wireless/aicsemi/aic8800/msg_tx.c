// SPDX-License-Identifier: GPL-2.0-only
/*
 * Requests sent to the AIC8800 firmware.
 *
 * Copyright (C) 2026 Jernej Skrabec <jernej.skrabec@gmail.com>
 */

#include "aic8800.h"

int aic_send_reset(struct aic_hw *hw)
{
	void *req;

	req = aic_msg_alloc(MM_RESET_REQ, TASK_MM, DRV_TASK_ID, 0);
	if (!req)
		return -ENOMEM;

	return aic_send_msg(hw, req, true, MM_RESET_CFM, NULL, 0);
}

/**
 * aic_fw_probe_running - find out whether the firmware is already up
 * @hw: device
 *
 * The device keeps running when the driver is unloaded, and a warm boot leaves
 * it running as well, so the firmware may well answer before it was loaded.
 * The boot ROM ignores this message, which is what the short timeout is for.
 */
bool aic_fw_probe_running(struct aic_hw *hw)
{
	void *req;

	req = aic_msg_alloc(MM_VERSION_REQ, TASK_MM, DRV_TASK_ID, 0);
	if (!req)
		return false;

	return !aic_send_msg_timeout(hw, req, MM_VERSION_CFM, NULL, 0,
				     AIC_FW_PROBE_TIMEOUT_MS, false);
}

int aic_send_version_req(struct aic_hw *hw)
{
	struct mm_version_cfm cfm = {};
	void *req;
	int ret;

	req = aic_msg_alloc(MM_VERSION_REQ, TASK_MM, DRV_TASK_ID, 0);
	if (!req)
		return -ENOMEM;

	ret = aic_send_msg(hw, req, true, MM_VERSION_CFM, &cfm, sizeof(cfm));
	if (ret)
		return ret;

	hw->fw.version_lmac = cfm.version_lmac;
	hw->fw.version_machw[0] = cfm.version_machw_1;
	hw->fw.version_machw[1] = cfm.version_machw_2;
	hw->fw.version_phy[0] = cfm.version_phy_1;
	hw->fw.version_phy[1] = cfm.version_phy_2;
	hw->fw.features = cfm.features;
	hw->fw.max_sta = cfm.max_sta_nb;

	dev_info(hw->dev,
		 "firmware %u.%u.%u.%u, features %08x, %u peers, %u interfaces\n",
		 (cfm.version_lmac >> 24) & 0xff, (cfm.version_lmac >> 16) & 0xff,
		 (cfm.version_lmac >> 8) & 0xff, cfm.version_lmac & 0xff,
		 cfm.features, cfm.max_sta_nb, cfm.max_vif_nb);

	return 0;
}

int aic_send_dbg_mem_read(struct aic_hw *hw, u32 addr, u32 *val)
{
	struct dbg_mem_read_req *req;
	struct dbg_mem_read_cfm cfm = {};
	int ret;

	req = aic_msg_alloc(DBG_MEM_READ_REQ, TASK_DBG, DRV_TASK_ID,
			    sizeof(*req));
	if (!req)
		return -ENOMEM;

	req->memaddr = addr;

	ret = aic_send_msg(hw, req, true, DBG_MEM_READ_CFM, &cfm, sizeof(cfm));
	if (ret)
		return ret;

	if (cfm.memaddr != addr) {
		dev_err(hw->dev, "read of %08x answered for %08x (%08x)\n",
			addr, cfm.memaddr, cfm.memdata);
		return -EPROTO;
	}

	*val = cfm.memdata;

	return 0;
}

int aic_send_dbg_mem_write(struct aic_hw *hw, u32 addr, u32 val)
{
	struct dbg_mem_write_req *req;

	req = aic_msg_alloc(DBG_MEM_WRITE_REQ, TASK_DBG, DRV_TASK_ID,
			    sizeof(*req));
	if (!req)
		return -ENOMEM;

	req->memaddr = addr;
	req->memdata = val;

	return aic_send_msg(hw, req, true, DBG_MEM_WRITE_CFM, NULL, 0);
}

int aic_send_dbg_mem_mask_write(struct aic_hw *hw, u32 addr, u32 mask, u32 val)
{
	struct dbg_mem_mask_write_req *req;

	req = aic_msg_alloc(DBG_MEM_MASK_WRITE_REQ, TASK_DBG, DRV_TASK_ID,
			    sizeof(*req));
	if (!req)
		return -ENOMEM;

	req->memaddr = addr;
	req->memmask = mask;
	req->memdata = val;

	return aic_send_msg(hw, req, true, DBG_MEM_MASK_WRITE_CFM, NULL, 0);
}

/**
 * aic_send_dbg_mem_block_write - write a block of device memory
 * @hw: device
 * @addr: destination address
 * @data: source buffer
 * @len: number of bytes, at most %AIC_FW_BLOCK_SIZE
 *
 * The firmware expects the request to always carry the full parameter
 * structure, only @len bytes of which are meaningful.
 */
int aic_send_dbg_mem_block_write(struct aic_hw *hw, u32 addr, const void *data,
				 u32 len)
{
	struct dbg_mem_block_write_req *req;

	if (len > sizeof(req->memdata))
		return -EINVAL;

	req = aic_msg_alloc(DBG_MEM_BLOCK_WRITE_REQ, TASK_DBG, DRV_TASK_ID,
			    sizeof(*req));
	if (!req)
		return -ENOMEM;

	req->memaddr = addr;
	req->memsize = len;
	memcpy(req->memdata, data, len);

	return aic_send_msg(hw, req, true, DBG_MEM_BLOCK_WRITE_CFM, NULL, 0);
}

int aic_send_dbg_start_app(struct aic_hw *hw, u32 boot_addr, u32 boot_type,
			   u32 *boot_status)
{
	struct dbg_start_app_req *req;
	struct dbg_start_app_cfm cfm = {};
	int ret;

	req = aic_msg_alloc(DBG_START_APP_REQ, TASK_DBG, DRV_TASK_ID,
			    sizeof(*req));
	if (!req)
		return -ENOMEM;

	req->bootaddr = boot_addr;
	req->boottype = boot_type;

	ret = aic_send_msg(hw, req, true, DBG_START_APP_CFM, &cfm, sizeof(cfm));
	if (ret)
		return ret;

	if (boot_status)
		*boot_status = cfm.bootstatus;

	return 0;
}

/* Lifetime of a queued frame before the firmware gives up on it. */
#define AIC_TX_LIFETIME_MS	100

/* Local low power clock accuracy, in ppm. */
#define AIC_LP_CLK_PPM		20

/* How long the firmware keeps a U-APSD service period open, in us. */
#define AIC_UAPSD_TIMEOUT	300

static void aic_fill_ht_cap(struct mac_htcapability *out,
			    const struct ieee80211_sta_ht_cap *ht)
{
	int i;

	/*
	 * The firmware always supports LDPC receive even though the band
	 * capabilities do not advertise it.
	 */
	out->ht_capa_info = ht->cap | IEEE80211_HT_CAP_LDPC_CODING;
	out->a_mpdu_param = ht->ampdu_factor |
			    (ht->ampdu_density <<
			     IEEE80211_HT_AMPDU_PARM_DENSITY_SHIFT);
	for (i = 0; i < sizeof(ht->mcs); i++)
		out->mcs_rate[i] = ((const u8 *)&ht->mcs)[i];
}

static void aic_fill_vht_cap(struct mac_vhtcapability *out,
			     const struct ieee80211_sta_vht_cap *vht)
{
	out->vht_capa_info = vht->cap;
	out->rx_mcs_map = le16_to_cpu(vht->vht_mcs.rx_mcs_map);
	out->rx_highest = le16_to_cpu(vht->vht_mcs.rx_highest);
	out->tx_mcs_map = le16_to_cpu(vht->vht_mcs.tx_mcs_map);
	out->tx_highest = le16_to_cpu(vht->vht_mcs.tx_highest);
}

static void aic_fill_he_cap(struct mac_hecapability *out,
			    const struct ieee80211_sta_he_cap *he)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(he->he_cap_elem.mac_cap_info); i++)
		out->mac_cap_info[i] = he->he_cap_elem.mac_cap_info[i];
	for (i = 0; i < ARRAY_SIZE(he->he_cap_elem.phy_cap_info); i++)
		out->phy_cap_info[i] = he->he_cap_elem.phy_cap_info[i];

	out->mcs_supp.rx_mcs_80 = le16_to_cpu(he->he_mcs_nss_supp.rx_mcs_80);
	out->mcs_supp.tx_mcs_80 = le16_to_cpu(he->he_mcs_nss_supp.tx_mcs_80);
	out->mcs_supp.rx_mcs_160 = le16_to_cpu(he->he_mcs_nss_supp.rx_mcs_160);
	out->mcs_supp.tx_mcs_160 = le16_to_cpu(he->he_mcs_nss_supp.tx_mcs_160);
	out->mcs_supp.rx_mcs_80p80 =
		le16_to_cpu(he->he_mcs_nss_supp.rx_mcs_80p80);
	out->mcs_supp.tx_mcs_80p80 =
		le16_to_cpu(he->he_mcs_nss_supp.tx_mcs_80p80);

	for (i = 0; i < ARRAY_SIZE(out->ppe_thres); i++)
		out->ppe_thres[i] = he->ppe_thres[i];
}

/**
 * aic_send_me_config - tell the firmware which capabilities to advertise
 * @hw: device
 *
 * Has to be sent before the wiphy is registered, because the firmware derives
 * the rate control tables from it.
 */
int aic_send_me_config(struct aic_hw *hw)
{
	struct ieee80211_supported_band *band;
	const struct ieee80211_sta_he_cap *he;
	struct me_config_req *req;

	/* the 5 GHz band carries the full set of capabilities */
	band = hw->wiphy->bands[NL80211_BAND_5GHZ];
	if (!band)
		band = hw->wiphy->bands[NL80211_BAND_2GHZ];

	req = aic_msg_alloc(ME_CONFIG_REQ, TASK_ME, DRV_TASK_ID, sizeof(*req));
	if (!req)
		return -ENOMEM;

	req->ht_supp = band->ht_cap.ht_supported;
	if (req->ht_supp)
		aic_fill_ht_cap(&req->ht_cap, &band->ht_cap);

	req->vht_supp = band->vht_cap.vht_supported;
	if (req->vht_supp)
		aic_fill_vht_cap(&req->vht_cap, &band->vht_cap);

	he = ieee80211_get_he_iftype_cap(band, NL80211_IFTYPE_STATION);
	req->he_supp = he && he->has_he;
	if (req->he_supp)
		aic_fill_he_cap(&req->he_cap, he);

	req->he_ul_on = false;
	req->ps_on = true;
	req->dpsm = true;
	req->ant_div_on = false;
	req->tx_lft = AIC_TX_LIFETIME_MS;
	req->phy_bw_max = PHY_CHNL_BW_80;

	dev_info(hw->dev, "capabilities: HT %d, VHT %d, HE %d\n",
		 req->ht_supp, req->vht_supp, req->he_supp);

	return aic_send_msg(hw, req, true, ME_CONFIG_CFM, NULL, 0);
}

/**
 * aic_send_me_chan_config - hand the regulatory channel list to the firmware
 * @hw: device
 *
 * Has to be sent after the wiphy is registered so that the regulatory core has
 * already applied the channel flags.
 */
int aic_send_me_chan_config(struct aic_hw *hw)
{
	struct me_chan_config_req *req;
	struct ieee80211_supported_band *band;
	int i;

	req = aic_msg_alloc(ME_CHAN_CONFIG_REQ, TASK_ME, DRV_TASK_ID,
			    sizeof(*req));
	if (!req)
		return -ENOMEM;

	band = hw->wiphy->bands[NL80211_BAND_2GHZ];
	for (i = 0; band && i < band->n_channels; i++) {
		struct ieee80211_channel *chan = &band->channels[i];

		if (req->chan2G4_cnt == ARRAY_SIZE(req->chan2G4))
			break;
		if (chan->flags & IEEE80211_CHAN_DISABLED)
			continue;

		req->chan2G4[req->chan2G4_cnt].flags = 0;
		if (chan->flags & IEEE80211_CHAN_NO_IR)
			req->chan2G4[req->chan2G4_cnt].flags |= CHAN_NO_IR;
		if (chan->flags & IEEE80211_CHAN_RADAR)
			req->chan2G4[req->chan2G4_cnt].flags |= CHAN_RADAR;
		req->chan2G4[req->chan2G4_cnt].band = PHY_BAND_2G4;
		req->chan2G4[req->chan2G4_cnt].freq = chan->center_freq;
		req->chan2G4[req->chan2G4_cnt].tx_power =
			(s8)(chan->max_power);
		req->chan2G4_cnt++;
	}

	band = hw->wiphy->bands[NL80211_BAND_5GHZ];
	for (i = 0; band && i < band->n_channels; i++) {
		struct ieee80211_channel *chan = &band->channels[i];

		if (req->chan5G_cnt == ARRAY_SIZE(req->chan5G))
			break;
		if (chan->flags & IEEE80211_CHAN_DISABLED)
			continue;

		req->chan5G[req->chan5G_cnt].flags = 0;
		if (chan->flags & IEEE80211_CHAN_NO_IR)
			req->chan5G[req->chan5G_cnt].flags |= CHAN_NO_IR;
		if (chan->flags & IEEE80211_CHAN_RADAR)
			req->chan5G[req->chan5G_cnt].flags |= CHAN_RADAR;
		req->chan5G[req->chan5G_cnt].band = PHY_BAND_5G;
		req->chan5G[req->chan5G_cnt].freq = chan->center_freq;
		req->chan5G[req->chan5G_cnt].tx_power =
			(s8)(chan->max_power);
		req->chan5G_cnt++;
	}

	return aic_send_msg(hw, req, true, ME_CHAN_CONFIG_CFM, NULL, 0);
}

int aic_send_start(struct aic_hw *hw)
{
	struct mm_start_req *req;

	req = aic_msg_alloc(MM_START_REQ, TASK_MM, DRV_TASK_ID, sizeof(*req));
	if (!req)
		return -ENOMEM;

	/* the PHY configuration is empty on this family */
	req->uapsd_timeout = AIC_UAPSD_TIMEOUT;
	req->lp_clk_accuracy = AIC_LP_CLK_PPM;

	return aic_send_msg(hw, req, true, MM_START_CFM, NULL, 0);
}

int aic_send_get_mac_addr(struct aic_hw *hw, u8 *addr)
{
	struct mm_get_mac_addr_cfm cfm = {};
	void *req;
	int ret;

	req = aic_msg_alloc(MM_GET_MAC_ADDR_REQ, TASK_MM, DRV_TASK_ID, 0);
	if (!req)
		return -ENOMEM;

	ret = aic_send_msg(hw, req, true, MM_GET_MAC_ADDR_CFM, &cfm,
			   sizeof(cfm));
	if (ret)
		return ret;

	memcpy(addr, cfm.mac_addr, ETH_ALEN);

	return 0;
}

static u8 aic_iftype_to_fw(enum nl80211_iftype type)
{
	switch (type) {
	case NL80211_IFTYPE_STATION:
	case NL80211_IFTYPE_P2P_CLIENT:
		return VIF_STA;
	case NL80211_IFTYPE_ADHOC:
		return VIF_IBSS;
	case NL80211_IFTYPE_AP:
	case NL80211_IFTYPE_P2P_GO:
		return VIF_AP;
	case NL80211_IFTYPE_MESH_POINT:
		return VIF_MESH_POINT;
	case NL80211_IFTYPE_MONITOR:
		return VIF_MONITOR;
	default:
		return VIF_STA;
	}
}

int aic_send_add_if(struct aic_hw *hw, const u8 *mac, enum nl80211_iftype type,
		    bool p2p, u8 *vif_idx)
{
	struct mm_add_if_cfm cfm = {};
	struct mm_add_if_req *req;
	int ret;

	req = aic_msg_alloc(MM_ADD_IF_REQ, TASK_MM, DRV_TASK_ID, sizeof(*req));
	if (!req)
		return -ENOMEM;

	memcpy(&req->addr, mac, ETH_ALEN);
	req->type = aic_iftype_to_fw(type);
	req->p2p = p2p;

	ret = aic_send_msg(hw, req, true, MM_ADD_IF_CFM, &cfm, sizeof(cfm));
	if (ret)
		return ret;

	if (cfm.status) {
		dev_err(hw->dev, "firmware refused the new interface: %u\n",
			cfm.status);
		return -EIO;
	}

	*vif_idx = cfm.inst_nbr;

	return 0;
}

int aic_send_remove_if(struct aic_hw *hw, u8 vif_idx)
{
	struct mm_remove_if_req *req;

	req = aic_msg_alloc(MM_REMOVE_IF_REQ, TASK_MM, DRV_TASK_ID,
			    sizeof(*req));
	if (!req)
		return -ENOMEM;

	req->inst_nbr = vif_idx;

	return aic_send_msg(hw, req, true, MM_REMOVE_IF_CFM, NULL, 0);
}

int aic_send_set_filter(struct aic_hw *hw, u32 filter)
{
	struct mm_set_filter_req *req;

	req = aic_msg_alloc(MM_SET_FILTER_REQ, TASK_MM, DRV_TASK_ID,
			    sizeof(*req));
	if (!req)
		return -ENOMEM;

	req->filter = filter;

	return aic_send_msg(hw, req, true, MM_SET_FILTER_CFM, NULL, 0);
}

int aic_send_chan_ctxt_add(struct aic_hw *hw,
			   const struct cfg80211_chan_def *chandef, u8 *idx)
{
	struct mm_chan_ctxt_add_cfm cfm = {};
	struct mm_chan_ctxt_add_req *req;
	int ret;

	req = aic_msg_alloc(MM_CHAN_CTXT_ADD_REQ, TASK_MM, DRV_TASK_ID,
			    sizeof(*req));
	if (!req)
		return -ENOMEM;

	aic_chandef_to_fw(chandef, &req->chan);

	ret = aic_send_msg(hw, req, true, MM_CHAN_CTXT_ADD_CFM, &cfm,
			   sizeof(cfm));
	if (ret)
		return ret;

	if (cfm.status)
		return -EIO;

	*idx = cfm.index;

	return 0;
}

int aic_send_chan_ctxt_del(struct aic_hw *hw, u8 idx)
{
	struct mm_chan_ctxt_del_req *req;

	req = aic_msg_alloc(MM_CHAN_CTXT_DEL_REQ, TASK_MM, DRV_TASK_ID,
			    sizeof(*req));
	if (!req)
		return -ENOMEM;

	req->index = idx;

	return aic_send_msg(hw, req, true, MM_CHAN_CTXT_DEL_CFM, NULL, 0);
}

int aic_send_chan_ctxt_link(struct aic_hw *hw, u8 vif_idx, u8 chan_idx,
			    bool chan_switch)
{
	struct mm_chan_ctxt_link_req *req;

	req = aic_msg_alloc(MM_CHAN_CTXT_LINK_REQ, TASK_MM, DRV_TASK_ID,
			    sizeof(*req));
	if (!req)
		return -ENOMEM;

	req->vif_index = vif_idx;
	req->chan_index = chan_idx;
	req->chan_switch = chan_switch;

	return aic_send_msg(hw, req, true, MM_CHAN_CTXT_LINK_CFM, NULL, 0);
}

int aic_send_chan_ctxt_unlink(struct aic_hw *hw, u8 vif_idx)
{
	struct mm_chan_ctxt_unlink_req *req;

	req = aic_msg_alloc(MM_CHAN_CTXT_UNLINK_REQ, TASK_MM, DRV_TASK_ID,
			    sizeof(*req));
	if (!req)
		return -ENOMEM;

	req->vif_index = vif_idx;

	return aic_send_msg(hw, req, true, MM_CHAN_CTXT_UNLINK_CFM, NULL, 0);
}

int aic_send_key_add(struct aic_hw *hw, u8 vif_idx, u8 sta_idx, bool pairwise,
		     const u8 *key, u8 key_len, u8 key_idx, u8 cipher_suite,
		     u8 *hw_key_idx)
{
	struct mm_key_add_cfm cfm = {};
	struct mm_key_add_req *req;
	int ret;

	if (key_len > sizeof(req->key.array))
		return -EINVAL;

	req = aic_msg_alloc(MM_KEY_ADD_REQ, TASK_MM, DRV_TASK_ID, sizeof(*req));
	if (!req)
		return -ENOMEM;

	req->key_idx = key_idx;
	req->sta_idx = sta_idx;
	req->pairwise = pairwise;
	req->inst_nbr = vif_idx;
	req->cipher_suite = cipher_suite;
	req->key.length = key_len;
	memcpy(req->key.array, key, key_len);

	ret = aic_send_msg(hw, req, true, MM_KEY_ADD_CFM, &cfm, sizeof(cfm));
	if (ret)
		return ret;

	if (cfm.status)
		return -EIO;

	*hw_key_idx = cfm.hw_key_idx;

	return 0;
}

int aic_send_key_del(struct aic_hw *hw, u8 hw_key_idx)
{
	struct mm_key_del_req *req;

	req = aic_msg_alloc(MM_KEY_DEL_REQ, TASK_MM, DRV_TASK_ID, sizeof(*req));
	if (!req)
		return -ENOMEM;

	req->hw_key_idx = hw_key_idx;

	return aic_send_msg(hw, req, true, MM_KEY_DEL_CFM, NULL, 0);
}

int aic_send_scanu_req(struct aic_hw *hw, struct aic_vif *vif,
		       struct cfg80211_scan_request *param)
{
	struct scanu_start_req *req;
	int i;

	req = aic_msg_alloc(SCANU_START_REQ, TASK_SCANU, DRV_TASK_ID,
			    sizeof(*req));
	if (!req)
		return -ENOMEM;

	req->vif_idx = vif->vif_index;
	req->no_cck = false;
	eth_broadcast_addr((u8 *)&req->bssid);

	for (i = 0; i < param->n_channels; i++) {
		struct ieee80211_channel *chan = param->channels[i];

		if (req->chan_cnt == ARRAY_SIZE(req->chan))
			break;

		req->chan[req->chan_cnt].band = chan->band;
		req->chan[req->chan_cnt].freq = chan->center_freq;
		req->chan[req->chan_cnt].flags =
			chan->flags & IEEE80211_CHAN_NO_IR ? CHAN_NO_IR : 0;
		req->chan[req->chan_cnt].tx_power =
			(s8)(chan->max_reg_power);
		req->chan_cnt++;
	}

	for (i = 0; i < param->n_ssids; i++) {
		if (req->ssid_cnt == ARRAY_SIZE(req->ssid))
			break;
		req->ssid[req->ssid_cnt].length = param->ssids[i].ssid_len;
		memcpy(req->ssid[req->ssid_cnt].array, param->ssids[i].ssid,
		       param->ssids[i].ssid_len);
		req->ssid_cnt++;
	}

	/*
	 * The additional information elements are passed in a message of their
	 * own on this family, rather than being fetched from host memory.
	 */
	req->add_ies = 0;
	req->add_ie_len = 0;

	/*
	 * The firmware answers with SCANU_START_CFM once the scan is over, not
	 * when it starts, so that has to reach the event handler rather than be
	 * swallowed here as the confirmation of this request.
	 */
	return aic_send_msg(hw, req, false, 0, NULL, 0);
}

/**
 * aic_send_scanu_vendor_ie - hand over the elements to add to probe requests
 * @hw: device
 * @vif: interface that is going to scan
 * @ie: elements, may be %NULL
 * @ie_len: length of @ie
 */
int aic_send_scanu_vendor_ie(struct aic_hw *hw, struct aic_vif *vif,
			     const u8 *ie, size_t ie_len)
{
	struct scanu_vendor_ie_req *req;

	if (ie_len > sizeof(req->ie))
		return -EINVAL;

	req = aic_msg_alloc(SCANU_VENDOR_IE_REQ, TASK_SCANU, DRV_TASK_ID,
			    sizeof(*req));
	if (!req)
		return -ENOMEM;

	req->vif_idx = vif->vif_index;
	req->add_ie_len = ie_len;
	if (ie_len)
		memcpy(req->ie, ie, ie_len);

	return aic_send_msg(hw, req, true, SCANU_VENDOR_IE_CFM, NULL, 0);
}

int aic_send_scanu_cancel(struct aic_hw *hw)
{
	void *req;

	req = aic_msg_alloc(SCANU_CANCEL_REQ, TASK_SCANU, DRV_TASK_ID, 0);
	if (!req)
		return -ENOMEM;

	return aic_send_msg(hw, req, true, SCANU_CANCEL_CFM, NULL, 0);
}

static bool aic_cipher_is_wep(u32 cipher)
{
	return cipher == WLAN_CIPHER_SUITE_WEP40 ||
	       cipher == WLAN_CIPHER_SUITE_WEP104;
}

static u8 aic_auth_type_to_fw(enum nl80211_auth_type type)
{
	switch (type) {
	case NL80211_AUTHTYPE_SHARED_KEY:
		return WLAN_AUTH_SHARED_KEY;
	case NL80211_AUTHTYPE_FT:
		return WLAN_AUTH_FT;
	case NL80211_AUTHTYPE_SAE:
		return WLAN_AUTH_SAE;
	case NL80211_AUTHTYPE_OPEN_SYSTEM:
	default:
		return WLAN_AUTH_OPEN;
	}
}

int aic_send_sm_connect(struct aic_hw *hw, struct aic_vif *vif,
			struct cfg80211_connect_params *sme)
{
	struct sm_connect_req *req;

	req = aic_msg_alloc(SM_CONNECT_REQ, TASK_SM, DRV_TASK_ID, sizeof(*req));
	if (!req)
		return -ENOMEM;

	if (sme->ie_len > sizeof(req->ie_buf)) {
		aic_msg_free(req);
		return -EINVAL;
	}

	req->vif_idx = vif->vif_index;
	req->ssid.length = min_t(size_t, sme->ssid_len,
				 sizeof(req->ssid.array));
	memcpy(req->ssid.array, sme->ssid, req->ssid.length);

	if (sme->bssid)
		memcpy(&req->bssid, sme->bssid, ETH_ALEN);
	else
		eth_broadcast_addr((u8 *)&req->bssid);

	if (sme->channel) {
		req->chan.band = sme->channel->band;
		req->chan.freq = sme->channel->center_freq;
		req->chan.flags = 0;
		req->chan.tx_power = (s8)(sme->channel->max_power);
	} else {
		req->chan.freq = (u16)-1;
	}

	req->flags = 0;
	if (sme->crypto.control_port)
		req->flags |= CONTROL_PORT_HOST;
	if (sme->crypto.control_port_no_encrypt)
		req->flags |= CONTROL_PORT_NO_ENC;
	/* the flag means "not WEP" rather than what its name suggests */
	if (!aic_cipher_is_wep(sme->crypto.cipher_group))
		req->flags |= WPA_WPA2_IN_USE;
	if (sme->mfp == NL80211_MFP_REQUIRED)
		req->flags |= MFP_IN_USE;
	if (vif->sta.ap)
		req->flags |= REASSOCIATION;
	/*
	 * WEP and TKIP are not allowed with HT, and a peer that offers them
	 * anyway has to be talked to without it.
	 */
	if (sme->crypto.n_ciphers_pairwise &&
	    (aic_cipher_is_wep(sme->crypto.ciphers_pairwise[0]) ||
	     sme->crypto.ciphers_pairwise[0] == WLAN_CIPHER_SUITE_TKIP))
		req->flags |= DISABLE_HT;

	req->ctrl_port_ethertype = sme->crypto.control_port_ethertype ?:
				  cpu_to_be16(ETH_P_PAE);
	req->auth_type = aic_auth_type_to_fw(sme->auth_type);
	req->uapsd_queues = 0;
	req->listen_interval = 0;
	req->dont_wait_bcmc = false;
	req->ie_len = sme->ie_len;
	if (sme->ie_len)
		memcpy(req->ie_buf, sme->ie, sme->ie_len);

	return aic_send_msg(hw, req, true, SM_CONNECT_CFM, NULL, 0);
}

int aic_send_sm_disconnect(struct aic_hw *hw, struct aic_vif *vif, u16 reason)
{
	struct sm_disconnect_req *req;

	req = aic_msg_alloc(SM_DISCONNECT_REQ, TASK_SM, DRV_TASK_ID,
			    sizeof(*req));
	if (!req)
		return -ENOMEM;

	req->reason_code = reason;
	req->vif_idx = vif->vif_index;

	return aic_send_msg(hw, req, true, SM_DISCONNECT_CFM, NULL, 0);
}

int aic_send_sm_external_auth_rsp(struct aic_hw *hw, u8 vif_idx, u16 status)
{
	struct sm_external_auth_required_rsp *rsp;

	rsp = aic_msg_alloc(SM_EXTERNAL_AUTH_REQUIRED_RSP, TASK_SM,
			    DRV_TASK_ID, sizeof(*rsp));
	if (!rsp)
		return -ENOMEM;

	rsp->status = status;
	rsp->vif_idx = vif_idx;

	return aic_send_msg(hw, rsp, false, 0, NULL, 0);
}

int aic_send_me_sta_add(struct aic_hw *hw, struct aic_vif *vif,
			struct station_parameters *params, const u8 *mac,
			u8 *sta_idx)
{
	const struct link_station_parameters *link = &params->link_sta_params;
	struct me_sta_add_cfm cfm = {};
	struct me_sta_add_req *req;
	int ret;

	req = aic_msg_alloc(ME_STA_ADD_REQ, TASK_ME, DRV_TASK_ID, sizeof(*req));
	if (!req)
		return -ENOMEM;

	memcpy(&req->mac_addr, mac, ETH_ALEN);

	req->rate_set.length = min_t(u8, link->supported_rates_len,
				     sizeof(req->rate_set.array));
	memcpy(req->rate_set.array, link->supported_rates,
	       req->rate_set.length);

	if (link->ht_capa) {
		const struct ieee80211_ht_cap *ht = link->ht_capa;
		int i;

		req->flags |= STA_HT_CAPA;
		req->ht_cap.ht_capa_info = le16_to_cpu(ht->cap_info);
		req->ht_cap.a_mpdu_param = ht->ampdu_params_info;
		for (i = 0; i < sizeof(ht->mcs); i++)
			req->ht_cap.mcs_rate[i] = ((const u8 *)&ht->mcs)[i];
		req->ht_cap.ht_extended_capa = le16_to_cpu(ht->extended_ht_cap_info);
		req->ht_cap.tx_beamforming_capa = le32_to_cpu(ht->tx_BF_cap_info);
		req->ht_cap.asel_capa = ht->antenna_selection_info;
	}

	if (link->vht_capa) {
		const struct ieee80211_vht_cap *vht = link->vht_capa;

		req->flags |= STA_VHT_CAPA;
		req->vht_cap.vht_capa_info = le32_to_cpu(vht->vht_cap_info);
		req->vht_cap.rx_mcs_map =
			le16_to_cpu(vht->supp_mcs.rx_mcs_map);
		req->vht_cap.rx_highest =
			le16_to_cpu(vht->supp_mcs.rx_highest);
		req->vht_cap.tx_mcs_map =
			le16_to_cpu(vht->supp_mcs.tx_mcs_map);
		req->vht_cap.tx_highest =
			le16_to_cpu(vht->supp_mcs.tx_highest);
	}

	if (link->he_capa) {
		const struct ieee80211_he_cap_elem *he = link->he_capa;
		int i;

		req->flags |= STA_HE_CAPA;
		for (i = 0; i < ARRAY_SIZE(req->he_cap.mac_cap_info); i++)
			req->he_cap.mac_cap_info[i] = he->mac_cap_info[i];
		for (i = 0; i < ARRAY_SIZE(req->he_cap.phy_cap_info); i++)
			req->he_cap.phy_cap_info[i] = he->phy_cap_info[i];
	}

	if (params->sta_flags_set & BIT(NL80211_STA_FLAG_WME))
		req->flags |= STA_QOS_CAPA;
	if (params->sta_flags_set & BIT(NL80211_STA_FLAG_MFP))
		req->flags |= STA_MFP_CAPA;

	req->aid = params->aid;
	req->uapsd_queues = params->uapsd_queues;
	req->max_sp_len = params->max_sp * 2;
	req->vif_idx = vif->vif_index;
	req->opmode = link->opmode_notif;

	ret = aic_send_msg(hw, req, true, ME_STA_ADD_CFM, &cfm, sizeof(cfm));
	if (ret)
		return ret;

	if (cfm.status) {
		dev_err(hw->dev, "firmware refused peer %pM: %u\n", mac,
			cfm.status);
		return -EIO;
	}

	*sta_idx = cfm.sta_idx;

	return 0;
}

int aic_send_me_sta_del(struct aic_hw *hw, u8 sta_idx)
{
	struct me_sta_del_req *req;

	req = aic_msg_alloc(ME_STA_DEL_REQ, TASK_ME, DRV_TASK_ID, sizeof(*req));
	if (!req)
		return -ENOMEM;

	req->sta_idx = sta_idx;
	req->tdls_sta = false;

	return aic_send_msg(hw, req, true, ME_STA_DEL_CFM, NULL, 0);
}

int aic_send_me_traffic_ind(struct aic_hw *hw, u8 sta_idx, bool uapsd,
			    bool tx_avail)
{
	struct me_traffic_ind_req *req;

	req = aic_msg_alloc(ME_TRAFFIC_IND_REQ, TASK_ME, DRV_TASK_ID,
			    sizeof(*req));
	if (!req)
		return -ENOMEM;

	req->sta_idx = sta_idx;
	req->tx_avail = tx_avail;
	req->uapsd = uapsd;

	return aic_send_msg(hw, req, true, ME_TRAFFIC_IND_CFM, NULL, 0);
}

int aic_send_me_set_control_port(struct aic_hw *hw, u8 sta_idx, bool open)
{
	struct me_set_control_port_req *req;

	req = aic_msg_alloc(ME_SET_CONTROL_PORT_REQ, TASK_ME, DRV_TASK_ID,
			    sizeof(*req));
	if (!req)
		return -ENOMEM;

	req->sta_idx = sta_idx;
	req->control_port_open = open;

	return aic_send_msg(hw, req, true, ME_SET_CONTROL_PORT_CFM, NULL, 0);
}

int aic_send_me_set_ps_mode(struct aic_hw *hw, bool enable)
{
	struct me_set_ps_mode_req *req;

	req = aic_msg_alloc(ME_SET_PS_MODE_REQ, TASK_ME, DRV_TASK_ID,
			    sizeof(*req));
	if (!req)
		return -ENOMEM;

	req->ps_state = enable;

	return aic_send_msg(hw, req, true, ME_SET_PS_MODE_CFM, NULL, 0);
}

int aic_send_me_config_monitor(struct aic_hw *hw,
			       const struct cfg80211_chan_def *chandef,
			       u8 *chan_idx)
{
	struct me_config_monitor_cfm cfm = {};
	struct me_config_monitor_req *req;
	int ret;

	req = aic_msg_alloc(ME_CONFIG_MONITOR_REQ, TASK_ME, DRV_TASK_ID,
			    sizeof(*req));
	if (!req)
		return -ENOMEM;

	if (chandef) {
		aic_chandef_to_fw(chandef, &req->chan);
		req->chan_set = true;
	}
	/* logging of unsupported HT frames, which the firmware drops otherwise */
	req->uf = false;
	req->auto_reply = false;

	ret = aic_send_msg(hw, req, true, ME_CONFIG_MONITOR_CFM, &cfm,
			   sizeof(cfm));
	if (ret)
		return ret;

	if (chan_idx)
		*chan_idx = cfm.chan_index;

	return 0;
}

/*
 * The firmware wants the basic rate set of the BSS, which userspace only
 * expresses as the rates marked basic in the beacon's rate elements.
 */
static void aic_beacon_basic_rates(const u8 *ies, size_t len,
				   struct mac_rateset *rates)
{
	static const u8 eids[] = { WLAN_EID_SUPP_RATES, WLAN_EID_EXT_SUPP_RATES };
	unsigned int i, j;

	for (i = 0; i < ARRAY_SIZE(eids); i++) {
		const u8 *ie = cfg80211_find_ie(eids[i], ies, len);

		if (!ie)
			continue;

		for (j = 0; j < ie[1]; j++) {
			if (!(ie[2 + j] & 0x80))	/* not a basic rate */
				continue;
			if (rates->length == ARRAY_SIZE(rates->array))
				return;
			rates->array[rates->length++] = ie[2 + j];
		}
	}
}

int aic_send_apm_start(struct aic_hw *hw, struct aic_vif *vif,
		       struct cfg80211_ap_settings *settings,
		       struct sk_buff *bcn, u16 tim_oft, u8 tim_len,
		       u8 *ch_idx, u8 *bcmc_idx)
{
	struct cfg80211_chan_def *chandef = &settings->chandef;
	struct apm_start_cfm cfm = {};
	struct apm_start_req *req;
	int ret;

	req = aic_msg_alloc(APM_START_REQ, TASK_APM, DRV_TASK_ID, sizeof(*req));
	if (!req)
		return -ENOMEM;

	req->vif_idx = vif->vif_index;
	req->bcn_addr = 0;
	req->bcn_len = bcn->len;
	req->tim_len = tim_len;
	req->tim_oft = tim_oft;
	req->bcn_int = settings->beacon_interval;
	req->flags = 0;
	if (settings->crypto.control_port)
		req->flags |= CONTROL_PORT_HOST;
	if (settings->crypto.control_port_no_encrypt)
		req->flags |= CONTROL_PORT_NO_ENC;
	/*
	 * The firmware only lets frames of this type past a controlled port
	 * that is still closed, which is how the handshake that opens it gets
	 * through in the first place.  Userspace only names the type when it
	 * wants an unusual one, so fill in the usual one here.
	 */
	req->ctrl_port_ethertype = settings->crypto.control_port_ethertype ?:
				   cpu_to_be16(ETH_P_PAE);
	if (settings->crypto.n_ciphers_pairwise &&
	    !aic_cipher_is_wep(settings->crypto.ciphers_pairwise[0]))
		req->flags |= WPA_WPA2_IN_USE;

	req->chan.band = chandef->chan->band;
	req->chan.freq = chandef->chan->center_freq;
	req->chan.flags = 0;
	req->chan.tx_power = (s8)(chandef->chan->max_power);
	req->center_freq1 = chandef->center_freq1;
	req->center_freq2 = chandef->center_freq2;
	req->ch_width = aic_chan_width_to_fw(chandef->width);

	/* the head starts with the frame header and the fixed beacon body */
	req->basic_rates.length = 0;
	if (settings->beacon.head_len > offsetof(struct ieee80211_mgmt,
						 u.beacon.variable))
		aic_beacon_basic_rates(settings->beacon.head +
				       offsetof(struct ieee80211_mgmt,
						u.beacon.variable),
				       settings->beacon.head_len -
				       offsetof(struct ieee80211_mgmt,
						u.beacon.variable),
				       &req->basic_rates);
	aic_beacon_basic_rates(settings->beacon.tail, settings->beacon.tail_len,
			       &req->basic_rates);
	if (!req->basic_rates.length)
		dev_warn(hw->dev, "the beacon carries no basic rates\n");

	ret = aic_send_bcn(hw, vif->vif_index, bcn);
	if (ret) {
		dev_err(hw->dev, "failed to hand over the beacon: %d\n", ret);
		aic_msg_free(req);
		return ret;
	}

	ret = aic_send_msg(hw, req, true, APM_START_CFM, &cfm, sizeof(cfm));
	if (ret) {
		dev_err(hw->dev, "APM_START_REQ failed: %d\n", ret);
		return ret;
	}

	if (cfm.status) {
		dev_err(hw->dev, "firmware refused to start the AP: %u\n",
			cfm.status);
		return -EIO;
	}

	*ch_idx = cfm.ch_idx;
	*bcmc_idx = cfm.bcmc_idx;

	return 0;
}

int aic_send_apm_stop(struct aic_hw *hw, u8 vif_idx)
{
	struct apm_stop_req *req;

	req = aic_msg_alloc(APM_STOP_REQ, TASK_APM, DRV_TASK_ID, sizeof(*req));
	if (!req)
		return -ENOMEM;

	req->vif_idx = vif_idx;

	return aic_send_msg(hw, req, true, APM_STOP_CFM, NULL, 0);
}

/**
 * aic_send_bcn - hand the beacon template to the firmware
 * @hw: device
 * @vif_idx: interface the beacon belongs to
 * @bcn: beacon, from the first byte of the header
 *
 * On this family the template travels in the message itself rather than being
 * fetched from host memory, which also bounds how large it can be.
 */
int aic_send_bcn(struct aic_hw *hw, u8 vif_idx, struct sk_buff *bcn)
{
	struct apm_set_bcn_ie_req *req;

	if (bcn->len > sizeof(req->bcn_ie)) {
		dev_err(hw->dev, "beacon of %u bytes does not fit\n", bcn->len);
		return -E2BIG;
	}

	req = aic_msg_alloc(APM_SET_BEACON_IE_REQ, TASK_APM, DRV_TASK_ID,
			    sizeof(*req));
	if (!req)
		return -ENOMEM;

	req->vif_idx = vif_idx;
	req->bcn_ie_len = bcn->len;
	memcpy(req->bcn_ie, bcn->data, bcn->len);

	return aic_send_msg(hw, req, true, APM_SET_BEACON_IE_CFM, NULL, 0);
}

int aic_send_bcn_change(struct aic_hw *hw, u8 vif_idx, struct sk_buff *bcn,
			u16 tim_oft, u8 tim_len, const u16 *csa_oft)
{
	struct mm_bcn_change_req *req;
	int i;

	req = aic_msg_alloc(MM_BCN_CHANGE_REQ, TASK_MM, DRV_TASK_ID,
			    sizeof(*req));
	if (!req)
		return -ENOMEM;

	req->bcn_ptr = 0;
	req->bcn_len = bcn->len;
	req->tim_oft = tim_oft;
	req->tim_len = tim_len;
	req->inst_nbr = vif_idx;
	if (csa_oft)
		for (i = 0; i < BCN_MAX_CSA_CPT; i++)
			req->csa_oft[i] = csa_oft[i];

	return aic_send_msg(hw, req, true, MM_BCN_CHANGE_CFM, NULL, 0);
}

int aic_send_roc(struct aic_hw *hw, struct aic_vif *vif,
		 struct ieee80211_channel *chan, unsigned int duration)
{
	struct mm_remain_on_channel_cfm cfm = {};
	struct mm_remain_on_channel_req *req;
	int ret;

	req = aic_msg_alloc(MM_REMAIN_ON_CHANNEL_REQ, TASK_MM, DRV_TASK_ID,
			    sizeof(*req));
	if (!req)
		return -ENOMEM;

	req->op_code = MM_ROC_OP_START;
	req->vif_index = vif->vif_index;
	req->duration_ms = duration;
	req->band = chan->band;
	req->type = PHY_CHNL_BW_20;
	req->prim20_freq = chan->center_freq;
	req->center1_freq = chan->center_freq;
	req->center2_freq = 0;
	req->tx_power = (s8)(chan->max_power);

	ret = aic_send_msg(hw, req, true, MM_REMAIN_ON_CHANNEL_CFM, &cfm,
			   sizeof(cfm));
	if (ret)
		return ret;

	return cfm.status ? -EIO : 0;
}

int aic_send_cancel_roc(struct aic_hw *hw, struct aic_vif *vif)
{
	struct mm_remain_on_channel_req *req;

	req = aic_msg_alloc(MM_REMAIN_ON_CHANNEL_REQ, TASK_MM, DRV_TASK_ID,
			    sizeof(*req));
	if (!req)
		return -ENOMEM;

	req->op_code = MM_ROC_OP_CANCEL;
	req->vif_index = vif->vif_index;

	return aic_send_msg(hw, req, true, MM_REMAIN_ON_CHANNEL_CFM, NULL, 0);
}

int aic_send_set_power(struct aic_hw *hw, u8 vif_idx, s8 pwr)
{
	struct mm_set_power_req *req;

	req = aic_msg_alloc(MM_SET_POWER_REQ, TASK_MM, DRV_TASK_ID,
			    sizeof(*req));
	if (!req)
		return -ENOMEM;

	req->inst_nbr = vif_idx;
	req->power = pwr;

	return aic_send_msg(hw, req, true, MM_SET_POWER_CFM, NULL, 0);
}
