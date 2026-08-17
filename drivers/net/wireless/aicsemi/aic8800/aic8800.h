/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Driver for AICSemi AIC8800 series wireless devices.
 *
 * Copyright (C) 2026 Jernej Skrabec <jernej.skrabec@gmail.com>
 */

#ifndef AIC8800_H
#define AIC8800_H

#include <linux/completion.h>
#include <linux/if_ether.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <net/cfg80211.h>

#include "fw_desc.h"
#include "fw_msg.h"

/* Limits the firmware was built with, see the AIC_FW_* comment in fw_msg.h. */
#define AIC_MAX_VIF		4
#define AIC_MAX_STA		32
#define AIC_MAX_AP_VIF		2
#define AIC_INVALID_STA		0xff
#define AIC_INVALID_VIF		0xff
#define AIC_CHAN_CTXT_CNT	3

/* Firmware only ever reports these two bands. */
#define AIC_NUM_BANDS		2

/* Access categories, in firmware order. */
enum aic_ac {
	AIC_AC_BK,
	AIC_AC_BE,
	AIC_AC_VI,
	AIC_AC_VO,
	AIC_AC_COUNT,
};

/*
 * Transmit queues.  The firmware has one queue per access category plus a
 * broadcast/multicast queue used while an AP interface has sleeping stations.
 */
#define AIC_TXQ_CNT		(AIC_AC_COUNT + 1)
#define AIC_TXQ_BCMC		AIC_AC_COUNT

struct aic_hw;

/**
 * struct aic_bus_ops - transport specific operations
 * @start: enable interrupts and start the bus threads
 * @stop: undo @start
 * @fw_started: re-arm the transport after the firmware was started
 * @send_msg: hand a control message to the device, may sleep
 * @send_data: queue a data frame for transmission, must not sleep
 * @kick_tx: tell the transport that new data is pending
 */
struct aic_bus_ops {
	int (*start)(struct aic_hw *hw);
	void (*stop)(struct aic_hw *hw);
	int (*fw_started)(struct aic_hw *hw);
	int (*send_msg)(struct aic_hw *hw, const void *buf, unsigned int len);
	int (*send_data)(struct aic_hw *hw, struct sk_buff *skb);
	void (*kick_tx)(struct aic_hw *hw);
};

/**
 * struct aic_cmd - one outstanding request towards the firmware
 * @list: link in &aic_cmd_mgr.cmds
 * @id: message id of the request
 * @cfm_id: message id of the expected confirmation
 * @cfm: where to copy the confirmation payload, may be %NULL
 * @cfm_len: size of the @cfm buffer
 * @done: completed once the confirmation arrived
 * @result: 0 or a negative error code
 */
struct aic_cmd {
	struct list_head list;
	u16 id;
	u16 cfm_id;
	void *cfm;
	u16 cfm_len;
	struct completion done;
	int result;
};

/**
 * struct aic_cmd_mgr - serialises requests towards the firmware
 * @lock: protects @cmds and @crashed
 * @cmds: list of outstanding requests
 * @send_lock: only one request may be in flight at a time
 * @crashed: set once the firmware stopped answering
 */
struct aic_cmd_mgr {
	spinlock_t lock;
	struct list_head cmds;
	struct mutex send_lock;
	bool crashed;
};

/**
 * struct aic_sta - a peer known to the firmware
 * @valid: entry is in use
 * @sta_idx: firmware index, also the index into &aic_hw.sta
 * @vif_idx: interface this peer belongs to
 * @addr: peer MAC address
 * @qos: peer supports QoS
 * @acm: bitmap of access categories requiring admission control
 * @uapsd_tids: bitmap of TIDs configured for U-APSD
 * @ch_idx: channel context the peer is on
 * @listen_interval: as announced by the peer
 */
struct aic_sta {
	struct list_head list;
	bool valid;
	u8 sta_idx;
	u8 vif_idx;
	u8 addr[ETH_ALEN];
	bool qos;
	u8 acm;
	u8 uapsd_tids;
	u8 ch_idx;
	u16 listen_interval;
};

/**
 * struct aic_vif - a virtual interface
 * @list: link in &aic_hw.vifs
 * @hw: owning device
 * @wdev: cfg80211 view of this interface
 * @ndev: network device, %NULL for a P2P device
 * @stats: interface counters
 * @drv_vif_index: index into &aic_hw.vif
 * @vif_index: firmware index, %AIC_INVALID_VIF while the interface is down
 * @ch_index: channel context index
 * @up: interface has been brought up
 * @use_4addr: 4 address mode is enabled
 * @sta: state only used by station interfaces
 * @ap: state only used by AP interfaces
 */
struct aic_vif {
	struct list_head list;
	struct aic_hw *hw;
	struct wireless_dev wdev;
	struct net_device *ndev;
	struct net_device_stats stats;
	u8 drv_vif_index;
	u8 vif_index;
	u8 ch_index;
	bool up;
	bool use_4addr;

	union {
		struct {
			struct aic_sta *ap;
			struct cfg80211_bss *bss;
			bool external_auth;
			u8 tdls_sta_idx;
		} sta;
		struct {
			struct list_head sta_list;
			bool started;
			bool flushing;
		} ap;
	};
};

/**
 * struct aic_fw_info - what the firmware told us about itself
 * @version_lmac: lower MAC firmware version
 * @version_machw: MAC hardware version words
 * @version_phy: PHY version words
 * @features: firmware feature bitmap, see MM_FEAT_*
 * @max_sta: number of peers the firmware can track
 */
struct aic_fw_info {
	u32 version_lmac;
	u32 version_machw[2];
	u32 version_phy[2];
	u32 features;
	u8 max_sta;
};

/**
 * struct aic_hw - one AIC8800 device
 * @dev: backing device
 * @bus_ops: transport operations
 * @bus_priv: transport private data
 * @chip_id: which member of the family this is, see enum aic_chip
 * @chip_rev: silicon revision
 * @wiphy: cfg80211 device
 * @vifs: list of active interfaces
 * @vif: interfaces by driver index
 * @avail_vif_mask: bitmap of free entries in @vif
 * @sta: peers by firmware index
 * @cmd_mgr: request bookkeeping
 * @fw: firmware information
 * @mutex: serialises cfg80211 operations
 * @tx_lock: protects the transmit queues
 * @txq: per access category transmit queues
 * @cfm_ring: frames waiting for a transmit confirmation
 * @cfm_idx: next index into @cfm_ring
 * @napi: receive polling context
 * @napi_dev: dummy device owning @napi
 * @rx_queue: buffers read from the bus, consumed by @napi
 * @scan_req: scan in progress
 * @roc: remain on channel request in progress
 * @roc_started: the firmware told us the remain on channel period started
 * @monitor_vif: firmware index of the monitor interface, or %AIC_INVALID_VIF
 * @chandef_monitor: channel the monitor interface is tuned to
 */
struct aic_hw {
	struct device *dev;
	const struct aic_bus_ops *bus_ops;
	void *bus_priv;
	u32 chip_id;
	u8 chip_rev;

	struct wiphy *wiphy;
	struct list_head vifs;
	struct aic_vif *vif[AIC_MAX_VIF];
	u32 avail_vif_mask;
	struct aic_sta sta[AIC_MAX_STA];

	struct aic_cmd_mgr cmd_mgr;
	struct aic_fw_info fw;

	struct mutex mutex;

	spinlock_t tx_lock;
	struct sk_buff_head txq[AIC_TXQ_CNT];
	struct sk_buff *cfm_ring[AIC_TXCFM_RING_SIZE];
	u32 cfm_idx;

	struct napi_struct napi;
	struct net_device *napi_dev;
	struct sk_buff_head rx_queue;

	struct cfg80211_scan_request *scan_req;
	struct wireless_dev *roc;
	bool roc_started;

	u8 monitor_vif;
	struct cfg80211_chan_def chandef_monitor;
};

enum aic_chip {
	AIC_CHIP_8801,
	AIC_CHIP_8800DC,
	AIC_CHIP_8800DW,
	AIC_CHIP_8800D80,
	AIC_CHIP_8800D80X2,
};

/* cmd.c */
void aic_cmd_mgr_init(struct aic_cmd_mgr *mgr);
void aic_cmd_mgr_deinit(struct aic_cmd_mgr *mgr);
void *aic_msg_alloc(u16 id, u16 dst, u16 src, u16 param_len);
void aic_msg_free(const void *param);
int aic_send_msg(struct aic_hw *hw, const void *param, bool need_cfm,
		 u16 cfm_id, void *cfm, u16 cfm_len);
void aic_rx_handle_msg(struct aic_hw *hw, const void *buf, unsigned int len);
void aic_rx_handle_print(struct aic_hw *hw, const void *buf, unsigned int len);

/* chan.c */
u8 aic_chan_width_to_fw(enum nl80211_chan_width width);
void aic_chandef_to_fw(const struct cfg80211_chan_def *chandef,
		       struct mac_chan_op *op);

/* msg_tx.c */
int aic_send_reset(struct aic_hw *hw);
int aic_send_version_req(struct aic_hw *hw);
int aic_send_dbg_mem_read(struct aic_hw *hw, u32 addr, u32 *val);
int aic_send_dbg_mem_write(struct aic_hw *hw, u32 addr, u32 val);
int aic_send_dbg_mem_mask_write(struct aic_hw *hw, u32 addr, u32 mask, u32 val);
int aic_send_dbg_mem_block_write(struct aic_hw *hw, u32 addr, const void *data,
				 u32 len);
int aic_send_dbg_start_app(struct aic_hw *hw, u32 boot_addr, u32 boot_type,
			   u32 *boot_status);
int aic_send_me_config(struct aic_hw *hw);
int aic_send_me_chan_config(struct aic_hw *hw);
int aic_send_start(struct aic_hw *hw);
int aic_send_get_mac_addr(struct aic_hw *hw, u8 *addr);
int aic_send_add_if(struct aic_hw *hw, const u8 *mac, enum nl80211_iftype type,
		    bool p2p, u8 *vif_idx);
int aic_send_remove_if(struct aic_hw *hw, u8 vif_idx);
int aic_send_set_filter(struct aic_hw *hw, u32 filter);
int aic_send_chan_ctxt_add(struct aic_hw *hw,
			   const struct cfg80211_chan_def *chandef, u8 *idx);
int aic_send_chan_ctxt_del(struct aic_hw *hw, u8 idx);
int aic_send_chan_ctxt_link(struct aic_hw *hw, u8 vif_idx, u8 chan_idx,
			    bool chan_switch);
int aic_send_chan_ctxt_unlink(struct aic_hw *hw, u8 vif_idx);
int aic_send_key_add(struct aic_hw *hw, u8 vif_idx, u8 sta_idx, bool pairwise,
		     const u8 *key, u8 key_len, u8 key_idx, u8 cipher_suite,
		     u8 *hw_key_idx);
int aic_send_key_del(struct aic_hw *hw, u8 hw_key_idx);
int aic_send_scanu_req(struct aic_hw *hw, struct aic_vif *vif,
		       struct cfg80211_scan_request *req);
int aic_send_scanu_cancel(struct aic_hw *hw);
int aic_send_sm_connect(struct aic_hw *hw, struct aic_vif *vif,
			struct cfg80211_connect_params *sme);
int aic_send_sm_disconnect(struct aic_hw *hw, struct aic_vif *vif,
			   u16 reason);
int aic_send_sm_external_auth_rsp(struct aic_hw *hw, u8 vif_idx, u16 status);
int aic_send_me_sta_add(struct aic_hw *hw, struct aic_vif *vif,
			struct station_parameters *params, const u8 *mac,
			u8 *sta_idx);
int aic_send_me_sta_del(struct aic_hw *hw, u8 sta_idx);
int aic_send_me_set_control_port(struct aic_hw *hw, u8 sta_idx, bool open);
int aic_send_me_set_ps_mode(struct aic_hw *hw, bool enable);
int aic_send_me_config_monitor(struct aic_hw *hw,
			       const struct cfg80211_chan_def *chandef,
			       u8 *chan_idx);
int aic_send_apm_start(struct aic_hw *hw, struct aic_vif *vif,
		       struct cfg80211_ap_settings *settings,
		       struct sk_buff *bcn, u16 tim_oft, u8 tim_len,
		       u8 *ch_idx, u8 *bcmc_idx);
int aic_send_apm_stop(struct aic_hw *hw, u8 vif_idx);
int aic_send_bcn_change(struct aic_hw *hw, u8 vif_idx, struct sk_buff *bcn,
			u16 tim_oft, u8 tim_len, const u16 *csa_oft);
int aic_send_roc(struct aic_hw *hw, struct aic_vif *vif,
		 struct ieee80211_channel *chan, unsigned int duration);
int aic_send_cancel_roc(struct aic_hw *hw, struct aic_vif *vif);
int aic_send_set_power(struct aic_hw *hw, u8 vif_idx, s8 pwr);

/* fw.c */
int aic_fw_load(struct aic_hw *hw);

/* event.c */
void aic_rx_handle_event(struct aic_hw *hw, u16 id, const void *param, u16 len);

/* cfg80211.c */
int aic_cfg80211_init(struct aic_hw *hw);
void aic_cfg80211_deinit(struct aic_hw *hw);
struct aic_vif *aic_vif_from_fw_idx(struct aic_hw *hw, u8 vif_idx);
struct aic_sta *aic_sta_from_fw_idx(struct aic_hw *hw, u8 sta_idx);

/* txrx.c */
netdev_tx_t aic_start_xmit(struct sk_buff *skb, struct net_device *ndev);
void aic_rx_frame(struct aic_hw *hw, const struct aic_rxhdr *rxhdr,
		  const u8 *frame, unsigned int len);
void aic_txq_flush_vif(struct aic_hw *hw, struct aic_vif *vif);

/* core.c */
struct aic_hw *aic_hw_alloc(struct device *dev, const struct aic_bus_ops *ops,
			    void *bus_priv);
void aic_hw_free(struct aic_hw *hw);
int aic_hw_start(struct aic_hw *hw);
void aic_hw_stop(struct aic_hw *hw);

#endif /* AIC8800_H */
