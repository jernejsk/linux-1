/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __UWE5622_WIFI_H
#define __UWE5622_WIFI_H

#include <linux/auxiliary_bus.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/uwe5622.h>
#include <net/cfg80211.h>

#define UWE5622_WIFI_MAX_CTX		8
#define UWE5622_WIFI_CMD_TIMEOUT		msecs_to_jiffies(3000)
#define UWE5622_WIFI_RSP_MAX		2048

enum uwe5622_wifi_cmd_id {
	UWE5622_CMD_GET_INFO = 1,
	UWE5622_CMD_OPEN = 3,
	UWE5622_CMD_CLOSE = 4,
	UWE5622_CMD_POWER_SAVE = 5,
	UWE5622_CMD_SET_CHANNEL = 7,
	UWE5622_CMD_SYNC_VERSION = 9,
	UWE5622_CMD_CONNECT = 10,
	UWE5622_CMD_SCAN = 11,
	UWE5622_CMD_DISCONNECT = 13,
	UWE5622_CMD_KEY = 14,
	UWE5622_CMD_START_AP = 17,
	UWE5622_CMD_DEL_STATION = 18,
	UWE5622_CMD_SET_IE = 25,
	UWE5622_CMD_TX_DATA = 72,
	UWE5622_CMD_DOWNLOAD_INI = 76,
	UWE5622_CMD_SET_WOWLAN = 83,
};

enum uwe5622_wifi_event_id {
	UWE5622_EVENT_CONNECT = 0x80,
	UWE5622_EVENT_DISCONNECT = 0x81,
	UWE5622_EVENT_SCAN_DONE = 0x82,
	UWE5622_EVENT_MGMT_FRAME = 0x83,
	UWE5622_EVENT_NEW_STATION = 0xa0,
	UWE5622_EVENT_STA_LUT = 0xf5,
	UWE5622_EVENT_HANG = 0xf6,
};

enum uwe5622_wifi_mode {
	UWE5622_MODE_NONE = 0,
	UWE5622_MODE_STATION = 1,
	UWE5622_MODE_AP = 2,
};

struct uwe5622_cmd_hdr {
	u8 common;
	u8 id;
	__le16 plen;
	__le32 token;
	s8 status;
	u8 rsp_count;
	u8 reserved[2];
} __packed;

struct uwe5622_wifi;

struct uwe5622_wifi_skb_cb {
	u8 ctx_id;
};

#define UWE5622_SKB_CB(_skb) ((struct uwe5622_wifi_skb_cb *)(_skb)->cb)

struct uwe5622_vif {
	struct wireless_dev wdev;
	struct uwe5622_wifi *wifi;
	u8 ctx_id;
	u8 mode;
	u8 sta_lut;
	bool opened;
	bool connected;
};

struct uwe5622_peer {
	u8 ctx_id;
	u8 sta_lut;
	u8 address[ETH_ALEN];
	bool valid;
};

struct uwe5622_wifi {
	struct device *dev;
	struct wiphy *wiphy;
	struct uwe5622_client *cmd_client;
	struct uwe5622_client *data_client;
	struct ieee80211_supported_band band_2ghz;
	struct ieee80211_supported_band band_5ghz;

	/* Allows one firmware command transaction at a time. */
	struct mutex cmd_mutex;
	struct completion cmd_done;
	u32 next_token;
	u32 pending_token;
	u8 pending_id;
	u8 response_ctx;
	s8 response_status;
	size_t response_len;
	u8 response[UWE5622_WIFI_RSP_MAX];

	/* Protects context-to-netdev and AP peer mappings. */
	spinlock_t vif_lock;
	struct net_device *vifs[UWE5622_WIFI_MAX_CTX];
	struct uwe5622_peer peers[32];
	/* Protects the outstanding cfg80211 scan request. */
	spinlock_t scan_lock;
	struct cfg80211_scan_request *scan_request;

	struct sk_buff_head eapol_queue;
	struct work_struct eapol_work;
	bool stopping;
	u8 perm_addr[ETH_ALEN];
	u32 fw_capa;
};

static inline struct uwe5622_vif *uwe5622_vif_from_wdev(struct wireless_dev *wdev)
{
	return container_of(wdev, struct uwe5622_vif, wdev);
}

int uwe5622_wifi_cmd(struct uwe5622_wifi *wifi, u8 ctx_id, u8 id,
		     const void *data, size_t len, void *response,
		     size_t *response_len, u8 *response_ctx);
void uwe5622_wifi_cmd_rx(void *priv, struct sk_buff *skb);
void uwe5622_wifi_cmd_reset(void *priv);
void uwe5622_wifi_event(struct uwe5622_wifi *wifi,
			const struct uwe5622_cmd_hdr *hdr,
			const u8 *data, size_t len);

#endif
