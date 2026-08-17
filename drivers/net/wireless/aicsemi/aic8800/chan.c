// SPDX-License-Identifier: GPL-2.0-only
/*
 * Channel description conversion for AICSemi AIC8800 series devices.
 *
 * Copyright (C) 2026 Jernej Skrabec <jernej.skrabec@gmail.com>
 */

#include "aic8800.h"

/**
 * aic_chan_width_to_fw - translate a channel width
 * @width: cfg80211 channel width
 *
 * Returns the matching &enum mac_chan_bandwidth value.  Widths the firmware
 * does not know about are reported as 20 MHz, which is always safe.
 */
u8 aic_chan_width_to_fw(enum nl80211_chan_width width)
{
	switch (width) {
	case NL80211_CHAN_WIDTH_20_NOHT:
	case NL80211_CHAN_WIDTH_20:
		return PHY_CHNL_BW_20;
	case NL80211_CHAN_WIDTH_40:
		return PHY_CHNL_BW_40;
	case NL80211_CHAN_WIDTH_80:
		return PHY_CHNL_BW_80;
	case NL80211_CHAN_WIDTH_160:
		return PHY_CHNL_BW_160;
	case NL80211_CHAN_WIDTH_80P80:
		return PHY_CHNL_BW_80P80;
	default:
		return PHY_CHNL_BW_20;
	}
}

/**
 * aic_chandef_to_fw - describe a channel to the firmware
 * @chandef: channel definition
 * @op: destination
 */
void aic_chandef_to_fw(const struct cfg80211_chan_def *chandef,
		       struct mac_chan_op *op)
{
	op->band = chandef->chan->band;
	op->type = aic_chan_width_to_fw(chandef->width);
	op->prim20_freq = chandef->chan->center_freq;
	op->center1_freq = chandef->center_freq1;
	op->center2_freq = chandef->center_freq2;
	op->tx_power = (s8)(chandef->chan->max_power);
	op->flags = 0;
	if (chandef->chan->flags & IEEE80211_CHAN_NO_IR)
		op->flags |= CHAN_NO_IR;
	if (chandef->chan->flags & IEEE80211_CHAN_RADAR)
		op->flags |= CHAN_RADAR;
}
