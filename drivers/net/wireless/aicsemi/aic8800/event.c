// SPDX-License-Identifier: GPL-2.0-only
/*
 * Unsolicited messages from the AIC8800 firmware.
 *
 * Copyright (C) 2026 Jernej Skrabec <jernej.skrabec@gmail.com>
 */

#include <linux/etherdevice.h>

#include "aic8800.h"

static void aic_event_scanu_result(struct aic_hw *hw, const void *param,
				   u16 len)
{
	const struct scanu_result_ind *ind = param;
	struct ieee80211_channel *chan;
	struct cfg80211_bss *bss;

	if (len < sizeof(*ind) || ind->length > len - sizeof(*ind))
		return;

	chan = ieee80211_get_channel(hw->wiphy, ind->center_freq);
	if (!chan)
		return;

	bss = cfg80211_inform_bss_frame(hw->wiphy, chan, (void *)ind->payload,
					ind->length, ind->rssi * 100,
					GFP_ATOMIC);
	if (bss)
		cfg80211_put_bss(hw->wiphy, bss);
}

static void aic_event_scanu_done(struct aic_hw *hw, const void *param, u16 len)
{
	struct cfg80211_scan_info info = {};

	if (!hw->scan_req)
		return;

	info.aborted = false;
	cfg80211_scan_done(hw->scan_req, &info);
	hw->scan_req = NULL;
}

static void aic_event_connect(struct aic_hw *hw, const void *param, u16 len)
{
	const struct sm_connect_ind *ind = param;
	struct cfg80211_connect_resp_params resp = {};
	struct aic_vif *vif;
	struct aic_sta *sta;
	const u8 *req_ie, *resp_ie;

	if (len < sizeof(*ind))
		return;

	vif = aic_vif_from_fw_idx(hw, ind->vif_idx);
	if (!vif || !vif->ndev)
		return;

	req_ie = (const u8 *)ind->assoc_ie_buf;
	resp_ie = req_ie + ind->assoc_req_ie_len;

	if (ind->status_code == WLAN_STATUS_SUCCESS &&
	    ind->ap_idx < AIC_MAX_STA) {
		sta = &hw->sta[ind->ap_idx];
		memset(sta, 0, sizeof(*sta));
		sta->valid = true;
		sta->sta_idx = ind->ap_idx;
		sta->vif_idx = ind->vif_idx;
		sta->ch_idx = ind->ch_idx;
		sta->qos = ind->qos;
		sta->acm = ind->acm;
		ether_addr_copy(sta->addr, (const u8 *)&ind->bssid);

		vif->sta.ap = sta;
		vif->ch_index = ind->ch_idx;

		vif->chandef.chan = ieee80211_get_channel(hw->wiphy,
							  ind->center_freq);
		vif->chandef.center_freq1 = ind->center_freq1;
		vif->chandef.center_freq2 = ind->center_freq2;
		switch (ind->width) {
		case PHY_CHNL_BW_40:
			vif->chandef.width = NL80211_CHAN_WIDTH_40;
			break;
		case PHY_CHNL_BW_80:
			vif->chandef.width = NL80211_CHAN_WIDTH_80;
			break;
		case PHY_CHNL_BW_160:
			vif->chandef.width = NL80211_CHAN_WIDTH_160;
			break;
		default:
			vif->chandef.width = NL80211_CHAN_WIDTH_20;
			break;
		}

		netif_carrier_on(vif->ndev);
	}

	resp.links[0].bssid = (const u8 *)&ind->bssid;
	resp.req_ie = req_ie;
	resp.req_ie_len = ind->assoc_req_ie_len;
	resp.resp_ie = resp_ie;
	resp.resp_ie_len = ind->assoc_rsp_ie_len;
	resp.status = ind->status_code;
	resp.timeout_reason = NL80211_TIMEOUT_UNSPECIFIED;

	if (ind->roamed) {
		struct cfg80211_roam_info roam = {};

		roam.links[0].bssid = resp.links[0].bssid;
		roam.links[0].channel = vif->chandef.chan;
		roam.req_ie = resp.req_ie;
		roam.req_ie_len = resp.req_ie_len;
		roam.resp_ie = resp.resp_ie;
		roam.resp_ie_len = resp.resp_ie_len;
		cfg80211_roamed(vif->ndev, &roam, GFP_ATOMIC);
	} else {
		cfg80211_connect_done(vif->ndev, &resp, GFP_ATOMIC);
	}
}

static void aic_event_disconnect(struct aic_hw *hw, const void *param, u16 len)
{
	const struct sm_disconnect_ind *ind = param;
	struct aic_vif *vif;

	if (len < sizeof(*ind))
		return;

	vif = aic_vif_from_fw_idx(hw, ind->vif_idx);
	if (!vif || !vif->ndev)
		return;

	if (vif->sta.ap) {
		vif->sta.ap->valid = false;
		vif->sta.ap = NULL;
	}
	memset(&vif->chandef, 0, sizeof(vif->chandef));

	netif_carrier_off(vif->ndev);
	cfg80211_disconnected(vif->ndev, ind->reason_code, NULL, 0,
			      ind->reason_code == 0, GFP_ATOMIC);
}

static void aic_event_external_auth(struct aic_hw *hw, const void *param,
				    u16 len)
{
	const struct sm_external_auth_required_ind *ind = param;
	struct cfg80211_external_auth_params params = {};
	struct aic_vif *vif;

	if (len < sizeof(*ind))
		return;

	vif = aic_vif_from_fw_idx(hw, ind->vif_idx);
	if (!vif || !vif->ndev)
		return;

	params.action = NL80211_EXTERNAL_AUTH_START;
	ether_addr_copy(params.bssid, (const u8 *)&ind->bssid);
	params.ssid.ssid_len = min_t(u8, ind->ssid.length,
				     sizeof(params.ssid.ssid));
	memcpy(params.ssid.ssid, ind->ssid.array, params.ssid.ssid_len);
	params.key_mgmt_suite = ind->akm;

	vif->sta.external_auth = true;
	cfg80211_external_auth_request(vif->ndev, &params, GFP_ATOMIC);
}

static void aic_event_channel_survey(struct aic_hw *hw, const void *param,
				     u16 len)
{
	const struct mm_channel_survey_ind *ind = param;
	struct ieee80211_supported_band *band;
	int i, idx = 0;

	if (len < sizeof(*ind))
		return;

	band = hw->wiphy->bands[NL80211_BAND_2GHZ];
	for (i = 0; band && i < band->n_channels; i++, idx++)
		if (band->channels[i].center_freq == ind->freq)
			goto found;

	band = hw->wiphy->bands[NL80211_BAND_5GHZ];
	for (i = 0; band && i < band->n_channels; i++, idx++)
		if (band->channels[i].center_freq == ind->freq)
			goto found;

	return;

found:
	if (idx >= ARRAY_SIZE(hw->survey))
		return;

	hw->survey[idx].filled = true;
	hw->survey[idx].chan_time_ms = ind->chan_time_ms;
	hw->survey[idx].chan_time_busy_ms = ind->chan_time_busy_ms;
	hw->survey[idx].noise_dbm = ind->noise_dbm;
}

static void aic_event_roc_start(struct aic_hw *hw, const void *param, u16 len)
{
	const struct mm_channel_switch_ind *ind = param;
	struct aic_vif *vif;

	if (len < sizeof(*ind) || !ind->roc || !hw->roc || hw->roc_started)
		return;

	vif = aic_vif_from_fw_idx(hw, ind->vif_index);
	if (!vif)
		return;

	hw->roc_started = true;
	cfg80211_ready_on_channel(hw->roc, (unsigned long)hw->roc,
				  hw->roc_chan, 0, GFP_ATOMIC);
}

static void aic_event_roc_done(struct aic_hw *hw, const void *param, u16 len)
{
	const struct mm_remain_on_channel_exp_ind *ind = param;

	if (len < sizeof(*ind) || !hw->roc)
		return;

	cfg80211_remain_on_channel_expired(hw->roc, (unsigned long)hw->roc,
					   hw->roc_chan, GFP_ATOMIC);
	hw->roc = NULL;
	hw->roc_started = false;
}

static void aic_event_tkip_mic_failure(struct aic_hw *hw, const void *param,
				       u16 len)
{
	const struct me_tkip_mic_failure_ind *ind = param;
	struct aic_vif *vif;

	if (len < sizeof(*ind))
		return;

	vif = aic_vif_from_fw_idx(hw, ind->vif_idx);
	if (!vif || !vif->ndev)
		return;

	cfg80211_michael_mic_failure(vif->ndev, (const u8 *)&ind->addr,
				     ind->ga ? NL80211_KEYTYPE_GROUP :
					       NL80211_KEYTYPE_PAIRWISE,
				     ind->keyid, (u8 *)&ind->tsc, GFP_ATOMIC);
}

static const struct {
	u16 id;
	void (*fn)(struct aic_hw *hw, const void *param, u16 len);
} aic_events[] = {
	{ SCANU_RESULT_IND,		aic_event_scanu_result },
	{ SCANU_START_CFM,		aic_event_scanu_done },
	{ SM_CONNECT_IND,		aic_event_connect },
	{ SM_DISCONNECT_IND,		aic_event_disconnect },
	{ SM_EXTERNAL_AUTH_REQUIRED_IND, aic_event_external_auth },
	{ MM_CHANNEL_SURVEY_IND,	aic_event_channel_survey },
	{ MM_CHANNEL_SWITCH_IND,	aic_event_roc_start },
	{ MM_REMAIN_ON_CHANNEL_EXP_IND,	aic_event_roc_done },
	{ ME_TKIP_MIC_FAILURE_IND,	aic_event_tkip_mic_failure },
};

/**
 * aic_rx_handle_event - dispatch one unsolicited firmware message
 * @hw: device
 * @id: message id
 * @param: message payload
 * @len: payload length
 */
void aic_rx_handle_event(struct aic_hw *hw, u16 id, const void *param, u16 len)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(aic_events); i++) {
		if (aic_events[i].id == id) {
			aic_events[i].fn(hw, param, len);
			return;
		}
	}

	dev_dbg(hw->dev, "unhandled firmware message %04x\n", id);
}
