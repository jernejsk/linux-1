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
#define UWE5622_N_CHANNELS_2GHZ		14
#define UWE5622_N_CHANNELS_5GHZ		25
#define UWE5622_WIFI_CMD_TIMEOUT		msecs_to_jiffies(3000)
/* Consecutive command timeouts that mean the firmware is gone. */
#define UWE5622_WIFI_CMD_TIMEOUT_LIMIT	3
#define UWE5622_WIFI_CMD_TX_MAX		1596
#define UWE5622_WIFI_RSP_MAX		2048

enum uwe5622_wifi_cmd_id {
	UWE5622_CMD_GET_INFO = 1,
	UWE5622_CMD_SET_REGDOM = 2,
	UWE5622_CMD_OPEN = 3,
	UWE5622_CMD_CLOSE = 4,
	UWE5622_CMD_POWER_SAVE = 5,
	UWE5622_CMD_SET_CHANNEL = 7,
	UWE5622_CMD_SYNC_VERSION = 9,
	UWE5622_CMD_CONNECT = 10,
	UWE5622_CMD_SCAN = 11,
	UWE5622_CMD_DISCONNECT = 13,
	UWE5622_CMD_KEY = 14,
	UWE5622_CMD_GET_STATION = 16,
	UWE5622_CMD_START_AP = 17,
	UWE5622_CMD_DEL_STATION = 18,
	UWE5622_CMD_SOFTAP_BLACKLIST = 19,
	UWE5622_CMD_SOFTAP_WHITELIST = 20,
	UWE5622_CMD_TX_MGMT = 21,
	UWE5622_CMD_REGISTER_FRAME = 22,
	UWE5622_CMD_SET_IE = 25,
	UWE5622_CMD_NOTIFY_IP_ACQUIRED = 26,
	UWE5622_CMD_SET_ROAM_OFFLOAD = 28,
	UWE5622_CMD_ADDBA_REQ = 40,
	UWE5622_CMD_BA = 68,
	UWE5622_CMD_TX_DATA = 72,
	UWE5622_CMD_DOWNLOAD_INI = 76,
	UWE5622_CMD_RESET_BEACON = 79,
	UWE5622_CMD_SET_WOWLAN = 83,
};

enum uwe5622_wifi_event_id {
	UWE5622_EVENT_CONNECT = 0x80,
	UWE5622_EVENT_DISCONNECT = 0x81,
	UWE5622_EVENT_SCAN_DONE = 0x82,
	UWE5622_EVENT_MGMT_FRAME = 0x83,
	/*
	 * The firmware can report whether Bluetooth is active so the host can
	 * be gentler with the bus. No code in the W21.05.3 image reaches the
	 * function that builds it, so treat its arrival as evidence rather than
	 * something to depend on.
	 */
	UWE5622_EVENT_COEX_BT_ON_OFF = 0x90,
	UWE5622_EVENT_NEW_STATION = 0xa0,
	UWE5622_EVENT_SDIO_FLOW_CONTROL = 0xb3,
	UWE5622_EVENT_SDIO_SEQ_NUM = 0xe0,
	UWE5622_EVENT_BA = 0xf3,
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

/*
 * Bit 3 of the first byte of every host-bound command, event and receive
 * descriptor. The firmware sets it on whatever it builds while its power
 * management state says asleep and clears it once awake, which makes it the
 * only account of what woke the system that the controller gives.
 */
#define UWE5622_HOST_RESUME_MARK	BIT(3)

/* Enough of the frame that woke the system to identify it. */
#define UWE5622_WOWLAN_PACKET_MAX	256

struct uwe5622_wowlan {
	/* What this sleep asked the controller to wake for. */
	bool armed;
	bool any;
	bool magic;
	bool disconnect;
	/* What the controller marked as responsible, first marked object only. */
	bool seen;
	bool woke_disconnect;
	bool woke_magic;
	u16 packet_len;
	u16 packet_present;
	u8 packet[UWE5622_WOWLAN_PACKET_MAX];
};

struct uwe5622_wifi;

struct uwe5622_wifi_skb_cb {
	u8 ctx_id;
	/* Which block acknowledgment command a queued payload belongs to. */
	u8 ba_cmd;
};

#define UWE5622_SKB_CB(_skb) ((struct uwe5622_wifi_skb_cb *)(_skb)->cb)

struct uwe5622_vif {
	struct wireless_dev wdev;
	struct uwe5622_wifi *wifi;
	u8 ctx_id;
	u8 mode;
	u8 sta_lut;
	/* Exclusive transmit credit pool, UWE5622_CREDIT_NO_POOL when unassigned. */
	u8 credit_pool;
	bool opened;
	bool connected;
	/* Management frame subtypes the controller was told to hand up. */
	u32 mgmt_regs;
	/* Reopens the firmware context after it refuses an association. */
	struct delayed_work connect_watchdog;
	struct work_struct recover_work;
	struct work_struct roam_resync_work;
	struct napi_struct napi;
	struct sk_buff_head rx_queue;
	bool napi_ready;
};

struct uwe5622_peer {
	u8 ctx_id;
	u8 sta_lut;
	u8 address[ETH_ALEN];
	bool valid;
	/* A pairwise key was installed for it, so its port is open. */
	bool authorized;
	/* It asked for the quality-of-service the firmware then uses. */
	bool wme;
	/* Transmit block acknowledgment sessions asked for, one bit per tid. */
	unsigned long ba_tx;
	unsigned int frames;
	unsigned long ba_retry;
};

/* Fields of the receive descriptor the reorder window needs. */
#define UWE5622_RX_STA_OFFSET		10
#define UWE5622_RX_STA_LUT_VALID	BIT(10)
#define UWE5622_RX_STA_LUT		GENMASK(15, 11)
#define UWE5622_RX_INFO_OFFSET		12
#define UWE5622_RX_INFO_QOS		BIT(13)
#define UWE5622_RX_INFO_TID		GENMASK(19, 16)
#define UWE5622_RX_INFO_SEQ		GENMASK(31, 20)

#define UWE5622_REORDER_SESSIONS	8
#define UWE5622_REORDER_WINDOW		64

/*
 * One receive block acknowledgment session. The peer may hand over frames out
 * of order within its window, so they are held here until the gaps ahead of the
 * head are filled, or until the head has waited long enough that releasing with
 * a gap beats stalling the stream.
 */

struct uwe5622_wifi {
	struct device *dev;
	struct wiphy *wiphy;
	struct uwe5622_client *cmd_client;
	struct uwe5622_client *data_client;
	struct ieee80211_supported_band band_2ghz;
	struct ieee80211_supported_band band_5ghz;
	/* Per-device channel state; cfg80211 writes regulatory flags into it. */
	struct ieee80211_channel channels_2ghz[UWE5622_N_CHANNELS_2GHZ];
	struct ieee80211_channel channels_5ghz[UWE5622_N_CHANNELS_5GHZ];

	/* Allows one firmware command transaction at a time. */
	struct mutex cmd_mutex;
	struct completion cmd_done;
	u32 next_token;
	/* Consecutive command timeouts, reset by every answered command. */
	unsigned int cmd_timeouts;
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
	/* Protects the four firmware-owned SDIO transmit credit pools. */
	spinlock_t credit_lock;
	u32 tx_credits[4];
	/* Owning context of each pool plus one, zero when the pool is shared. */
	u8 credit_owner[4];
	bool tx_with_credit;
	/* When transmission ran out of credit, zero once any came back. */
	ktime_t stall_start;

	struct sk_buff_head eapol_queue;
	struct work_struct eapol_work;
	struct sk_buff_head ba_queue;
	struct work_struct ba_work;
		struct notifier_block inetaddr_notifier;
	bool stopping;
	/* Set while the firmware is parked for system sleep. */
	bool parked;
	/* Protects the wake-up state below. */
	spinlock_t wowlan_lock;
	struct uwe5622_wowlan wowlan;
	struct delayed_work wowlan_work;
	u8 perm_addr[ETH_ALEN];
	u32 fw_capa;
	/* What the firmware said it can hold, for the limits cfg80211 enforces. */
	/* Cookies for management frames the stack asked to be sent. */
	u8 max_ap_sta;
	u8 max_acl;
	u32 fw_std;
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
void uwe5622_wowlan_marked_event(struct uwe5622_wifi *wifi, u8 id);

#endif
