// SPDX-License-Identifier: GPL-2.0-only

#include <linux/auxiliary_bus.h>
#include <linux/crc16.h>
#include <linux/etherdevice.h>
#include <linux/firmware.h>
#include <linux/ieee80211.h>
#include <linux/inetdevice.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/unaligned.h>
#include <net/cfg80211.h>

#include "wifi.h"

#define UWE5622_VALID_CONFIG	BIT(7)
#define UWE5622_TX_DESC_LEN	11
#define UWE5622_DATA_TX_MAX	1672
#define UWE5622_RX_DESC_LEN	28
#define UWE5622_RX_CREDIT_OFFSET	24
#define UWE5622_CREDIT_COLORS	4
#define UWE5622_CREDIT_MAX	U16_MAX
#define UWE5622_CREDIT_NO_POOL	0xff
#define UWE5622_RX_MH_DESC_LEN	28
#define UWE5622_EAPOL_QUEUE_MAX	64
#define UWE5622_WIFI_CONFIG_NAME	"unisoc/uwe5622/wifi_config.bin"
#define UWE5622_WIFI_CONFIG_SEC1_LEN 328
#define UWE5622_WIFI_CONFIG_SEC2_LEN 1464
#define UWE5622_WIFI_CONFIG_SEC3_MAX 1500
/*
 * The coexistence engine's configuration is the last 84 bytes of section two:
 * twenty-one words, of which the antenna and isolation settings are the two the
 * firmware is known to act on.
 */
#define UWE5622_WIFI_COEX_OFFSET 1380
#define UWE5622_WIFI_COEX_ANT_CFG0 36
#define UWE5622_WIFI_COEX_ISOLATION_CFG0 44
#define UWE5622_WIFI_CONFIG_MAGIC_OFFSET 251

#define UWE5622_GET_INFO_CAP_5G	BIT(0)
#define UWE5622_GET_INFO_CAP_AP_SME BIT(3)

enum uwe5622_cipher {
	UWE5622_CIPHER_NONE,
	UWE5622_CIPHER_WEP40,
	UWE5622_CIPHER_WEP104,
	UWE5622_CIPHER_TKIP,
	UWE5622_CIPHER_CCMP,
	UWE5622_CIPHER_AES_CMAC = 8,
};

struct uwe5622_cmd_open {
	u8 mode;
	u8 reserved;
	u8 mac[ETH_ALEN];
} __packed;

struct uwe5622_cmd_connect {
	__le32 wpa_versions;
	u8 bssid[ETH_ALEN];
	u8 channel;
	u8 auth_type;
	u8 pairwise_cipher;
	u8 group_cipher;
	u8 key_mgmt;
	u8 mfp;
	u8 psk_len;
	u8 ssid_len;
	u8 psk[WLAN_MAX_KEY_LEN];
	u8 ssid[IEEE80211_MAX_SSID_LEN];
} __packed;

struct uwe5622_event_mgmt_frame {
	u8 type;
	u8 channel;
	s8 signal;
	u8 reserved;
	u8 bssid[ETH_ALEN];
	__le16 len;
	u8 data[];
} __packed;

struct uwe5622_event_sta_lut {
	u8 ctx_id;
	u8 action;
	u8 sta_lut;
	u8 address[ETH_ALEN];
	u8 ht;
	u8 vht;
} __packed;

struct uwe5622_key_add {
	u8 index;
	u8 pairwise;
	u8 mac[ETH_ALEN];
	u8 sequence[16];
	u8 cipher;
	u8 len;
	u8 data[];
} __packed;

struct uwe5622_key_del {
	u8 index;
	u8 pairwise;
	u8 mac[ETH_ALEN];
} __packed;

#define UWE5622_RATE(_rate, _hw) { .bitrate = (_rate), .hw_value = (_hw) }
static struct ieee80211_rate uwe5622_rates[] = {
	UWE5622_RATE(10, 0), UWE5622_RATE(20, 1),
	UWE5622_RATE(55, 2), UWE5622_RATE(110, 3),
	UWE5622_RATE(60, 4), UWE5622_RATE(90, 5),
	UWE5622_RATE(120, 6), UWE5622_RATE(180, 7),
	UWE5622_RATE(240, 8), UWE5622_RATE(360, 9),
	UWE5622_RATE(480, 10), UWE5622_RATE(540, 11),
};

#define UWE5622_CHAN2(_ch, _freq) { \
	.band = NL80211_BAND_2GHZ, .hw_value = (_ch), .center_freq = (_freq) }
static struct ieee80211_channel uwe5622_channels_2ghz[] = {
	UWE5622_CHAN2(1, 2412), UWE5622_CHAN2(2, 2417),
	UWE5622_CHAN2(3, 2422), UWE5622_CHAN2(4, 2427),
	UWE5622_CHAN2(5, 2432), UWE5622_CHAN2(6, 2437),
	UWE5622_CHAN2(7, 2442), UWE5622_CHAN2(8, 2447),
	UWE5622_CHAN2(9, 2452), UWE5622_CHAN2(10, 2457),
	UWE5622_CHAN2(11, 2462), UWE5622_CHAN2(12, 2467),
	UWE5622_CHAN2(13, 2472), UWE5622_CHAN2(14, 2484),
};

#define UWE5622_CHAN5(_ch, _freq) { \
	.band = NL80211_BAND_5GHZ, .hw_value = (_ch), .center_freq = (_freq) }
static struct ieee80211_channel uwe5622_channels_5ghz[] = {
	UWE5622_CHAN5(36, 5180), UWE5622_CHAN5(40, 5200),
	UWE5622_CHAN5(44, 5220), UWE5622_CHAN5(48, 5240),
	UWE5622_CHAN5(52, 5260), UWE5622_CHAN5(56, 5280),
	UWE5622_CHAN5(60, 5300), UWE5622_CHAN5(64, 5320),
	UWE5622_CHAN5(100, 5500), UWE5622_CHAN5(104, 5520),
	UWE5622_CHAN5(108, 5540), UWE5622_CHAN5(112, 5560),
	UWE5622_CHAN5(116, 5580), UWE5622_CHAN5(120, 5600),
	UWE5622_CHAN5(124, 5620), UWE5622_CHAN5(128, 5640),
	UWE5622_CHAN5(132, 5660), UWE5622_CHAN5(136, 5680),
	UWE5622_CHAN5(140, 5700), UWE5622_CHAN5(144, 5720),
	UWE5622_CHAN5(149, 5745), UWE5622_CHAN5(153, 5765),
	UWE5622_CHAN5(157, 5785), UWE5622_CHAN5(161, 5805),
	UWE5622_CHAN5(165, 5825),
};

static const struct ieee80211_supported_band uwe5622_band_2ghz = {
	.channels = uwe5622_channels_2ghz,
	.n_channels = ARRAY_SIZE(uwe5622_channels_2ghz),
	.bitrates = uwe5622_rates,
	.n_bitrates = ARRAY_SIZE(uwe5622_rates),
	.ht_cap = {
		.ht_supported = true,
		.cap = IEEE80211_HT_CAP_SGI_20 | IEEE80211_HT_CAP_SGI_40,
		.ampdu_factor = IEEE80211_HT_MAX_AMPDU_64K,
		.ampdu_density = IEEE80211_HT_MPDU_DENSITY_8,
		.mcs = { .rx_mask = { 0xff }, .tx_params = IEEE80211_HT_MCS_TX_DEFINED },
	},
};

static const struct ieee80211_supported_band uwe5622_band_5ghz = {
	.channels = uwe5622_channels_5ghz,
	.n_channels = ARRAY_SIZE(uwe5622_channels_5ghz),
	.bitrates = &uwe5622_rates[4],
	.n_bitrates = ARRAY_SIZE(uwe5622_rates) - 4,
	.ht_cap = {
		.ht_supported = true,
		.cap = IEEE80211_HT_CAP_SGI_20 | IEEE80211_HT_CAP_SGI_40,
		.ampdu_factor = IEEE80211_HT_MAX_AMPDU_64K,
		.ampdu_density = IEEE80211_HT_MPDU_DENSITY_8,
		.mcs = { .rx_mask = { 0xff }, .tx_params = IEEE80211_HT_MCS_TX_DEFINED },
	},
	.vht_cap = {
		.vht_supported = true,
		.cap = IEEE80211_VHT_CAP_MAX_MPDU_LENGTH_7991 |
		       IEEE80211_VHT_CAP_SHORT_GI_80,
		.vht_mcs = {
			.rx_mcs_map = cpu_to_le16(0xfffe),
			.tx_mcs_map = cpu_to_le16(0xfffe),
		},
	},
};

/*
 * BIP is deliberately absent. Management frame protection needs an integrity
 * group key at key index 4 or 5, which the firmware key command has no room
 * for, so advertising the cipher only makes userspace negotiate protected
 * management frames and then fail to install the key.
 */
static const u32 uwe5622_akm_suites[] = {
	WLAN_AKM_SUITE_8021X,
	WLAN_AKM_SUITE_PSK,
	WLAN_AKM_SUITE_FT_8021X,
	WLAN_AKM_SUITE_FT_PSK,
	WLAN_AKM_SUITE_8021X_SHA256,
	WLAN_AKM_SUITE_PSK_SHA256,
};

static const u32 uwe5622_cipher_suites[] = {
	WLAN_CIPHER_SUITE_WEP40,
	WLAN_CIPHER_SUITE_WEP104,
	WLAN_CIPHER_SUITE_TKIP,
	WLAN_CIPHER_SUITE_CCMP,
};

static struct net_device *uwe5622_get_ndev(struct uwe5622_wifi *wifi, u8 ctx)
{
	struct net_device *ndev = NULL;

	if (ctx >= UWE5622_WIFI_MAX_CTX)
		return NULL;
	spin_lock_bh(&wifi->vif_lock);
	ndev = wifi->vifs[ctx];
	if (ndev)
		dev_hold(ndev);
	spin_unlock_bh(&wifi->vif_lock);
	return ndev;
}

static int uwe5622_set_ie(struct uwe5622_vif *vif, u8 type,
			  const u8 *ie, size_t len)
{
	u8 *data;
	int ret;

	if (!len)
		return 0;
	if (len > U16_MAX - 3)
		return -EINVAL;
	data = kmalloc(3 + len, GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data[0] = type;
	put_unaligned_le16(len, data + 1);
	memcpy(data + 3, ie, len);
	ret = uwe5622_wifi_cmd(vif->wifi, vif->ctx_id, UWE5622_CMD_SET_IE,
			       data, 3 + len, NULL, NULL, NULL);
	kfree(data);
	return ret;
}

static u8 uwe5622_cipher(u32 cipher)
{
	switch (cipher) {
	case WLAN_CIPHER_SUITE_WEP40:
		return UWE5622_CIPHER_WEP40;
	case WLAN_CIPHER_SUITE_WEP104:
		return UWE5622_CIPHER_WEP104;
	case WLAN_CIPHER_SUITE_TKIP:
		return UWE5622_CIPHER_TKIP;
	case WLAN_CIPHER_SUITE_CCMP:
		return UWE5622_CIPHER_CCMP;
	case WLAN_CIPHER_SUITE_AES_CMAC:
		return UWE5622_CIPHER_AES_CMAC;
	default:
		return UWE5622_CIPHER_NONE;
	}
}

static u8 uwe5622_akm(u32 akm)
{
	switch (akm) {
	case WLAN_AKM_SUITE_8021X:
		return 1;
	case WLAN_AKM_SUITE_PSK:
		return 2;
	case WLAN_AKM_SUITE_FT_8021X:
		return 3;
	case WLAN_AKM_SUITE_FT_PSK:
		return 4;
	case WLAN_AKM_SUITE_8021X_SHA256:
		return 5;
	case WLAN_AKM_SUITE_PSK_SHA256:
		return 6;
	default:
		return 0;
	}
}

static int uwe5622_ndev_open(struct net_device *ndev)
{
	netif_start_queue(ndev);
	return 0;
}

static int uwe5622_ndev_stop(struct net_device *ndev)
{
	netif_stop_queue(ndev);
	return 0;
}

static void uwe5622_request_tx_ba(struct uwe5622_wifi *wifi, u8 ctx_id,
				  u8 sta_lut, u8 tid);

static struct sk_buff *uwe5622_build_tx(struct uwe5622_vif *vif,
					const struct sk_buff *skb, u8 type,
					u8 color)
{
	struct uwe5622_wifi *wifi = vif->wifi;
	struct sk_buff *tx;
	const struct ethhdr *eth;
	u8 sta_lut;
	u8 *desc;
	int i;

	sta_lut = READ_ONCE(vif->sta_lut);
	if (vif->mode == UWE5622_MODE_AP && skb->len >= ETH_HLEN) {
		eth = (const void *)skb->data;
		if (is_multicast_ether_addr(eth->h_dest)) {
			sta_lut = 4;
		} else {
			sta_lut = 0;
			spin_lock_bh(&wifi->vif_lock);
			for (i = 0; i < ARRAY_SIZE(wifi->peers); i++) {
				if (wifi->peers[i].valid &&
				    wifi->peers[i].ctx_id == vif->ctx_id &&
				    ether_addr_equal(wifi->peers[i].address,
						     eth->h_dest)) {
					sta_lut = wifi->peers[i].sta_lut;
					break;
				}
			}
			spin_unlock_bh(&wifi->vif_lock);
			if (sta_lut < 6)
				return NULL;
		}
	}

	tx = alloc_skb(5 + UWE5622_TX_DESC_LEN + skb->len, GFP_ATOMIC);
	if (!tx)
		return NULL;
	skb_reserve(tx, 5);
	desc = skb_put_zero(tx, UWE5622_TX_DESC_LEN);
	desc[0] = type | (vif->ctx_id << 5);
	desc[1] = UWE5622_TX_DESC_LEN;
	put_unaligned_le16(skb->len, desc + 3);
	desc[6] = sta_lut;
	desc[7] = color;
	skb_put_data(tx, skb->data, skb->len);
	return tx;
}

static void uwe5622_wake_queues(struct uwe5622_wifi *wifi)
{
	struct net_device *ndev;
	int i;

	for (i = 0; i < UWE5622_WIFI_MAX_CTX; i++) {
		ndev = uwe5622_get_ndev(wifi, i);
		if (!ndev)
			continue;
		if (netif_running(ndev) && netif_queue_stopped(ndev))
			netif_wake_queue(ndev);
		dev_put(ndev);
	}
}

static void uwe5622_add_tx_credits(struct uwe5622_wifi *wifi,
				   const u8 credits[UWE5622_CREDIT_COLORS],
				   bool reset)
{
	bool added = false;
	int i;

	spin_lock_bh(&wifi->credit_lock);
	if (reset) {
		memset(wifi->tx_credits, 0, sizeof(wifi->tx_credits));
	} else {
		for (i = 0; i < UWE5622_CREDIT_COLORS; i++) {
			if (!credits[i])
				continue;
			wifi->tx_credits[i] =
				min_t(u32, wifi->tx_credits[i] + credits[i],
				      UWE5622_CREDIT_MAX);
			added = true;
		}
	}
	spin_unlock_bh(&wifi->credit_lock);

	if (added)
		uwe5622_wake_queues(wifi);
}

/*
 * The firmware hands out credits in four pools and expects one pool to belong
 * to each open context: a context may spend its own pool and any pool nobody
 * has claimed, but never a pool that belongs to another context. Spending
 * another context's credits desynchronises the firmware's own accounting and it
 * stops granting altogether. Pools are claimed lazily on first transmit so a
 * context that never sends does not hold one.
 */
static void uwe5622_claim_credit_pool(struct uwe5622_wifi *wifi,
				      struct uwe5622_vif *vif)
{
	int i;

	lockdep_assert_held(&wifi->credit_lock);

	if (vif->credit_pool != UWE5622_CREDIT_NO_POOL)
		return;

	for (i = 0; i < UWE5622_CREDIT_COLORS; i++) {
		if (wifi->credit_owner[i] == vif->ctx_id + 1) {
			vif->credit_pool = i;
			return;
		}
	}
	for (i = 0; i < UWE5622_CREDIT_COLORS; i++) {
		if (!wifi->credit_owner[i]) {
			wifi->credit_owner[i] = vif->ctx_id + 1;
			vif->credit_pool = i;
			return;
		}
	}
}

static void uwe5622_release_credit_pool(struct uwe5622_wifi *wifi,
					struct uwe5622_vif *vif)
{
	int i;

	spin_lock_bh(&wifi->credit_lock);
	for (i = 0; i < UWE5622_CREDIT_COLORS; i++) {
		if (wifi->credit_owner[i] == vif->ctx_id + 1)
			wifi->credit_owner[i] = 0;
	}
	vif->credit_pool = UWE5622_CREDIT_NO_POOL;
	spin_unlock_bh(&wifi->credit_lock);
}

static bool uwe5622_take_tx_credit(struct uwe5622_wifi *wifi,
				   struct net_device *ndev, u8 *color)
{
	struct uwe5622_vif *vif = netdev_priv(ndev);
	int i;

	*color = 0;
	if (!READ_ONCE(wifi->tx_with_credit))
		return true;

	spin_lock_bh(&wifi->credit_lock);
	uwe5622_claim_credit_pool(wifi, vif);
	if (vif->credit_pool != UWE5622_CREDIT_NO_POOL &&
	    wifi->tx_credits[vif->credit_pool]) {
		wifi->tx_credits[vif->credit_pool]--;
		*color = vif->credit_pool;
		spin_unlock_bh(&wifi->credit_lock);
		return true;
	}
	for (i = 0; i < UWE5622_CREDIT_COLORS; i++) {
		if (wifi->credit_owner[i] || !wifi->tx_credits[i])
			continue;
		wifi->tx_credits[i]--;
		*color = i;
		spin_unlock_bh(&wifi->credit_lock);
		return true;
	}
	netif_stop_queue(ndev);
	spin_unlock_bh(&wifi->credit_lock);
	return false;
}

static void uwe5622_return_tx_credit(struct uwe5622_wifi *wifi,
				     struct net_device *ndev, u8 color)
{
	if (!READ_ONCE(wifi->tx_with_credit))
		return;

	spin_lock_bh(&wifi->credit_lock);
	if (wifi->tx_credits[color] < UWE5622_CREDIT_MAX)
		wifi->tx_credits[color]++;
	spin_unlock_bh(&wifi->credit_lock);
	if (netif_queue_stopped(ndev))
		netif_wake_queue(ndev);
}

static netdev_tx_t uwe5622_ndev_xmit(struct sk_buff *skb,
				     struct net_device *ndev)
{
	struct uwe5622_vif *vif = netdev_priv(ndev);
	struct uwe5622_wifi *wifi = vif->wifi;
	struct sk_buff *tx;
	u8 color;
	int ret;

	if (unlikely(skb->protocol == htons(ETH_P_PAE))) {
		if (skb_queue_len(&wifi->eapol_queue) >= UWE5622_EAPOL_QUEUE_MAX)
			return NETDEV_TX_BUSY;
		UWE5622_SKB_CB(skb)->ctx_id = vif->ctx_id;
		skb_queue_tail(&wifi->eapol_queue, skb);
		schedule_work(&wifi->eapol_work);
		return NETDEV_TX_OK;
	}

	if (!uwe5622_take_tx_credit(wifi, ndev, &color))
		return NETDEV_TX_BUSY;
	tx = uwe5622_build_tx(vif, skb, 2, color);
	if (!tx)
		goto return_credit;
	uwe5622_request_tx_ba(wifi, vif->ctx_id, tx->data[6], 0);
	if (tx->len > UWE5622_DATA_TX_MAX) {
		kfree_skb(tx);
		goto return_credit;
	}
	ret = uwe5622_client_send_tagged(wifi->data_client, tx, color);
	kfree_skb(tx);
	if (ret) {
		uwe5622_return_tx_credit(wifi, ndev, color);
		if (ret == -ENOBUFS)
			return NETDEV_TX_BUSY;
		goto drop;
	}

	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += skb->len;
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
return_credit:
	uwe5622_return_tx_credit(wifi, ndev, color);
drop:
	ndev->stats.tx_dropped++;
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static const struct net_device_ops uwe5622_netdev_ops = {
	.ndo_open = uwe5622_ndev_open,
	.ndo_stop = uwe5622_ndev_stop,
	.ndo_start_xmit = uwe5622_ndev_xmit,
};

/*
 * Sequence numbers are 12 bits wide and wrap, so distances are always taken
 * modulo 4096: anything less than half the space ahead of the head counts as a
 * future frame, everything else as one already released.
 */
#define UWE5622_SEQ_MASK	0xfff

static struct {
	unsigned int frames;
	unsigned int stored;
	unsigned int expired;
	unsigned int ahead;
	unsigned int dup;
	unsigned int nosession;
	unsigned int jumps;
	u64 ns;
} uwe5622_reorder_stats;

static bool uwe5622_reorder_debug;
module_param_named(reorder_debug, uwe5622_reorder_debug, bool, 0644);
MODULE_PARM_DESC(reorder_debug, "report reorder window statistics");

static bool uwe5622_tx_block_ack = true;
module_param_named(tx_block_ack, uwe5622_tx_block_ack, bool, 0644);
MODULE_PARM_DESC(tx_block_ack, "ask peers for a transmit block ack session");

/*
 * Off by default: the window as it stands mislabels a fifth of the stream as
 * arriving from behind the head, and passing those on late costs a third of the
 * throughput against delivering everything in arrival order.
 */
static bool uwe5622_reorder_enable;
module_param_named(reorder, uwe5622_reorder_enable, bool, 0644);
MODULE_PARM_DESC(reorder, "reorder received frames inside the block ack window");

static unsigned int uwe5622_reorder_timeout_ms = 50;
module_param_named(reorder_timeout_ms, uwe5622_reorder_timeout_ms, uint, 0644);
MODULE_PARM_DESC(reorder_timeout_ms,
		 "how long a reorder window waits for a missing frame");

#define UWE5622_REORDER_TIMEOUT \
	msecs_to_jiffies(uwe5622_reorder_timeout_ms)

static u16 uwe5622_seq_delta(u16 seq, u16 head)
{
	return (seq - head) & UWE5622_SEQ_MASK;
}

static struct uwe5622_reorder *uwe5622_reorder_find(struct uwe5622_wifi *wifi,
						    u8 sta_lut, u8 tid)
{
	struct uwe5622_reorder *session;
	int i;

	for (i = 0; i < ARRAY_SIZE(wifi->reorder); i++) {
		session = &wifi->reorder[i];
		if (session->active && session->sta_lut == sta_lut &&
		    session->tid == tid)
			return session;
	}

	return NULL;
}

/* Move the head past every slot that is filled, queueing what it passes. */
static void uwe5622_reorder_release(struct uwe5622_reorder *session,
				    struct sk_buff_head *done)
{
	struct sk_buff *skb;

	while (session->stored) {
		skb = session->frame[session->head % session->size];
		if (!skb)
			break;
		session->frame[session->head % session->size] = NULL;
		session->stored--;
		session->head = (session->head + 1) & UWE5622_SEQ_MASK;
		__skb_queue_tail(done, skb);
	}
}

/* Release everything held, gaps included, and leave the head after it. */
static void uwe5622_reorder_flush(struct uwe5622_reorder *session,
				  struct sk_buff_head *done)
{
	struct sk_buff *skb;
	u16 i, next;

	for (i = 0, next = 0; i < session->size; i++) {
		skb = session->frame[(session->head + i) % session->size];
		if (!skb)
			continue;
		session->frame[(session->head + i) % session->size] = NULL;
		session->stored--;
		__skb_queue_tail(done, skb);
		next = i + 1;
	}
	session->head = (session->head + next) & UWE5622_SEQ_MASK;
}

static void uwe5622_reorder_open(struct uwe5622_wifi *wifi, u8 sta_lut, u8 tid,
				 u16 win_start, u16 win_size)
{
	struct sk_buff_head done;
	struct uwe5622_reorder *session;
	struct sk_buff *skb;
	int i;

	__skb_queue_head_init(&done);
	if (!win_size || win_size > UWE5622_REORDER_WINDOW)
		win_size = UWE5622_REORDER_WINDOW;

	spin_lock_bh(&wifi->reorder_lock);
	session = uwe5622_reorder_find(wifi, sta_lut, tid);
	if (session) {
		uwe5622_reorder_flush(session, &done);
	} else {
		for (i = 0; i < ARRAY_SIZE(wifi->reorder); i++) {
			if (!wifi->reorder[i].active) {
				session = &wifi->reorder[i];
				break;
			}
		}
	}
	if (session) {
		/*
		 * A session that was closed with frames still held left them in
		 * its slots, and the window is about to describe a different
		 * range, so empty it rather than only resetting the count: a
		 * frame left behind is leaked and makes a later one at the same
		 * index look like a duplicate.
		 */
		uwe5622_reorder_flush(session, &done);
		session->active = true;
		session->sta_lut = sta_lut;
		session->tid = tid;
		session->head = win_start & UWE5622_SEQ_MASK;
		session->size = win_size;
		session->stored = 0;
	}
	spin_unlock_bh(&wifi->reorder_lock);

	while ((skb = __skb_dequeue(&done)))
		netif_rx(skb);

	if (!session)
		dev_warn(wifi->dev, "no room for another reorder session\n");
}

static void uwe5622_reorder_close(struct uwe5622_wifi *wifi, u8 sta_lut,
				  int tid)
{
	struct sk_buff_head done;
	struct sk_buff *skb;
	int i;

	__skb_queue_head_init(&done);
	spin_lock_bh(&wifi->reorder_lock);
	for (i = 0; i < ARRAY_SIZE(wifi->reorder); i++) {
		if (!wifi->reorder[i].active ||
		    wifi->reorder[i].sta_lut != sta_lut ||
		    (tid >= 0 && wifi->reorder[i].tid != tid))
			continue;
		uwe5622_reorder_flush(&wifi->reorder[i], &done);
		wifi->reorder[i].active = false;
	}
	spin_unlock_bh(&wifi->reorder_lock);

	while ((skb = __skb_dequeue(&done)))
		netif_rx(skb);
}

static void uwe5622_reorder_close_all(struct uwe5622_wifi *wifi)
{
	struct sk_buff_head done;
	struct sk_buff *skb;
	int i;

	__skb_queue_head_init(&done);
	spin_lock_bh(&wifi->reorder_lock);
	for (i = 0; i < ARRAY_SIZE(wifi->reorder); i++) {
		if (!wifi->reorder[i].active)
			continue;
		uwe5622_reorder_flush(&wifi->reorder[i], &done);
		wifi->reorder[i].active = false;
	}
	spin_unlock_bh(&wifi->reorder_lock);

	while ((skb = __skb_dequeue(&done)))
		netif_rx(skb);
}

/*
 * Returns true once the frame belongs to the session, whether it was stored or
 * queued for delivery; the caller then has nothing left to do with it.
 */
static bool uwe5622_reorder_rx(struct uwe5622_wifi *wifi, u8 sta_lut, u8 tid,
			       u16 seq, struct sk_buff *skb,
			       struct sk_buff_head *done)
{
	struct uwe5622_reorder *session;
	bool pending;
	u16 delta;
	u64 start = ktime_get_ns();
	unsigned int before;

	if (!uwe5622_reorder_enable)
		return false;

	uwe5622_reorder_stats.frames++;
	spin_lock_bh(&wifi->reorder_lock);
	session = uwe5622_reorder_find(wifi, sta_lut, tid);
	if (!session) {
		spin_unlock_bh(&wifi->reorder_lock);
		uwe5622_reorder_stats.nosession++;
		uwe5622_reorder_stats.ns += ktime_get_ns() - start;
		if (uwe5622_reorder_debug &&
		    !(uwe5622_reorder_stats.frames % 16384))
			dev_info(wifi->dev,
				 "reorder frames=%u none=%u held=%u expired=%u ahead=%u dup=%u cost=%lluns\n",
				 uwe5622_reorder_stats.frames,
				 uwe5622_reorder_stats.nosession,
				 uwe5622_reorder_stats.stored,
				 uwe5622_reorder_stats.expired,
				 uwe5622_reorder_stats.ahead,
				 uwe5622_reorder_stats.dup,
				 uwe5622_reorder_stats.ns /
				 uwe5622_reorder_stats.frames);
		return false;
	}

	delta = uwe5622_seq_delta(seq, session->head);
	if (delta >= session->size) {
		/*
		 * A frame from behind the head has to be passed straight on
		 * rather than dropped. It looks like a retransmission of
		 * something already delivered, but dropping these costs
		 * everything: about a fifth of the stream arrives this way once
		 * a session is running, the stack then has real holes to fill,
		 * and the link falls to a quarter of a megabyte a second. So
		 * the head is tracking the stream wrongly rather than the peer
		 * repeating itself, and until that is understood the frame is
		 * worth more delivered late than discarded.
		 */
		if (delta > UWE5622_SEQ_MASK / 2) {
			spin_unlock_bh(&wifi->reorder_lock);
			__skb_queue_tail(done, skb);
			uwe5622_reorder_stats.ahead++;
			return true;
		}

		/* The peer has moved well ahead: follow it. */
		if (uwe5622_reorder_debug && uwe5622_reorder_stats.jumps++ < 30)
			dev_info(wifi->dev,
				 "jump: sta %u tid %u seq %u head %u delta %u stored %u\n",
				 sta_lut, tid, seq, session->head, delta,
				 session->stored);
		uwe5622_reorder_flush(session, done);
		session->head = (seq - session->size + 1) & UWE5622_SEQ_MASK;
		delta = uwe5622_seq_delta(seq, session->head);
	}

	if (session->frame[seq % session->size]) {
		/* A retransmission of something already held. */
		spin_unlock_bh(&wifi->reorder_lock);
		dev_kfree_skb_any(skb);
		uwe5622_reorder_stats.dup++;
		return true;
	}

	session->frame[seq % session->size] = skb;
	if (!session->stored++)
		session->deadline = jiffies + UWE5622_REORDER_TIMEOUT;
	before = session->stored;
	uwe5622_reorder_release(session, done);
	pending = session->stored;
	spin_unlock_bh(&wifi->reorder_lock);


	/*
	 * The timer only has to be armed while something is held, and rearming
	 * it for every frame costs more than the wait it guards.
	 */
	if (pending)
		uwe5622_reorder_stats.stored++;
	uwe5622_reorder_stats.ns += ktime_get_ns() - start;
	if (uwe5622_reorder_debug && !(uwe5622_reorder_stats.frames % 16384))
		dev_info(wifi->dev,
			 "reorder frames=%u none=%u held=%u expired=%u ahead=%u dup=%u depth=%u cost=%lluns\n",
			 uwe5622_reorder_stats.frames,
			 uwe5622_reorder_stats.nosession,
			 uwe5622_reorder_stats.stored,
			 uwe5622_reorder_stats.expired, uwe5622_reorder_stats.ahead,
			 uwe5622_reorder_stats.dup, before,
			 uwe5622_reorder_stats.ns / uwe5622_reorder_stats.frames);

	if (pending && !delayed_work_pending(&wifi->reorder_work))
		schedule_delayed_work(&wifi->reorder_work,
				      UWE5622_REORDER_TIMEOUT);
	return true;
}

/*
 * A hole the peer never fills would otherwise hold the whole stream, so give it
 * a deadline and release past it once that expires.
 */
static void uwe5622_reorder_expire(struct work_struct *work)
{
	struct uwe5622_wifi *wifi = container_of(work, struct uwe5622_wifi,
						 reorder_work.work);
	struct sk_buff_head done;
	struct sk_buff *skb;
	bool pending = false;
	int i;

	__skb_queue_head_init(&done);
	spin_lock_bh(&wifi->reorder_lock);
	for (i = 0; i < ARRAY_SIZE(wifi->reorder); i++) {
		struct uwe5622_reorder *session = &wifi->reorder[i];

		if (!session->active || !session->stored)
			continue;
		if (time_after(jiffies, session->deadline)) {
			uwe5622_reorder_stats.expired++;
			uwe5622_reorder_flush(session, &done);
		} else {
			pending = true;
		}
	}
	spin_unlock_bh(&wifi->reorder_lock);

	while ((skb = __skb_dequeue(&done)))
		netif_rx(skb);

	if (pending)
		schedule_delayed_work(&wifi->reorder_work,
				      UWE5622_REORDER_TIMEOUT);
}

/*
 * Receive side block acknowledgement. The firmware asks the host whether to
 * accept a session the peer proposes and only aggregates once the host answers,
 * so without this the peer sends single frames and the link runs at a fraction
 * of its rate. The answer cannot be sent from the receive path, which is where
 * the request arrives and where the reply would have to be waited for, so it
 * goes through a worker.
 */
#define UWE5622_BA_ADDBA_REQ_EVENT	0
#define UWE5622_BA_ADDBA_RSP_CMD	1
#define UWE5622_BA_DELBA_EVENT		2
#define UWE5622_BA_DELBA_ALL_EVENT	5

struct uwe5622_event_ba {
	u8 type;
	u8 tid;
	u8 sta_lut;
	u8 reserved;
	__le16 win_start;
	__le16 win_size;
} __packed;

struct uwe5622_cmd_ba {
	u8 type;
	u8 tid;
	u8 address[ETH_ALEN];
	u8 success;
} __packed;

/*
 * Transmit side. The peer will not aggregate what this station sends until a
 * session is asked for, so once traffic to a peer is clearly under way the
 * firmware is told to negotiate one. A refusal is retried, but not before
 * UWE5622_BA_RETRY, so a peer that always says no costs one command every few
 * seconds rather than one per frame.
 */
#define UWE5622_BA_POLICY_IMMEDIATE	BIT(1)
#define UWE5622_BA_PARAM_TID		GENMASK(5, 2)
#define UWE5622_BA_PARAM_BUFFER_SIZE	GENMASK(15, 6)
#define UWE5622_BA_WINDOW		64
#define UWE5622_BA_FRAMES		16
#define UWE5622_BA_RETRY		msecs_to_jiffies(3000)

struct uwe5622_cmd_addba {
	u8 sta_lut;
	u8 address[ETH_ALEN];
	u8 dialog_token;
	__le16 param;
	__le16 timeout;
} __packed;

static void uwe5622_event_ba(struct uwe5622_wifi *wifi, u8 ctx,
			     const u8 *data, size_t len)
{
	const struct uwe5622_event_ba *event = (const void *)data;
	struct uwe5622_cmd_ba *rsp;
	struct sk_buff *skb;
	bool valid;

	if (len < sizeof(*event) || event->sta_lut >= ARRAY_SIZE(wifi->peers))
		return;

	switch (event->type) {
	case UWE5622_BA_ADDBA_REQ_EVENT:
		break;
	case UWE5622_BA_DELBA_EVENT:
		uwe5622_reorder_close(wifi, event->sta_lut, event->tid);
		return;
	case UWE5622_BA_DELBA_ALL_EVENT:
		uwe5622_reorder_close(wifi, event->sta_lut, -1);
		return;
	default:
		return;
	}

	uwe5622_reorder_open(wifi, event->sta_lut, event->tid,
			     le16_to_cpu(event->win_start),
			     le16_to_cpu(event->win_size));

	skb = alloc_skb(sizeof(*rsp), GFP_ATOMIC);
	if (!skb)
		return;
	rsp = skb_put_zero(skb, sizeof(*rsp));
	rsp->type = UWE5622_BA_ADDBA_RSP_CMD;
	rsp->tid = event->tid;

	spin_lock_bh(&wifi->vif_lock);
	valid = wifi->peers[event->sta_lut].valid;
	if (valid)
		ether_addr_copy(rsp->address,
				wifi->peers[event->sta_lut].address);
	spin_unlock_bh(&wifi->vif_lock);
	if (!valid) {
		kfree_skb(skb);
		return;
	}
	rsp->success = 1;

	UWE5622_SKB_CB(skb)->ctx_id = ctx;
	UWE5622_SKB_CB(skb)->ba_cmd = UWE5622_CMD_BA;
	skb_queue_tail(&wifi->ba_queue, skb);
	schedule_work(&wifi->ba_work);
}

static void uwe5622_request_tx_ba(struct uwe5622_wifi *wifi, u8 ctx_id,
				  u8 sta_lut, u8 tid)
{
	struct uwe5622_cmd_addba *req;
	struct sk_buff *skb;
	struct uwe5622_peer *peer;
	bool ask = false;

	if (sta_lut >= ARRAY_SIZE(wifi->peers) || !uwe5622_tx_block_ack)
		return;

	spin_lock_bh(&wifi->vif_lock);
	peer = &wifi->peers[sta_lut];
	if (peer->valid && ++peer->frames > UWE5622_BA_FRAMES &&
	    (!peer->ba_retry || time_after(jiffies, peer->ba_retry)) &&
	    !test_and_set_bit(tid, &peer->ba_tx))
		ask = true;
	spin_unlock_bh(&wifi->vif_lock);
	if (!ask)
		return;

	skb = alloc_skb(sizeof(*req), GFP_ATOMIC);
	if (!skb) {
		clear_bit(tid, &peer->ba_tx);
		return;
	}
	req = skb_put_zero(skb, sizeof(*req));
	req->sta_lut = sta_lut;
	spin_lock_bh(&wifi->vif_lock);
	ether_addr_copy(req->address, peer->address);
	spin_unlock_bh(&wifi->vif_lock);
	req->dialog_token = 1;
	req->param = cpu_to_le16(UWE5622_BA_POLICY_IMMEDIATE |
				 FIELD_PREP(UWE5622_BA_PARAM_TID, tid) |
				 FIELD_PREP(UWE5622_BA_PARAM_BUFFER_SIZE,
					    UWE5622_BA_WINDOW));

	UWE5622_SKB_CB(skb)->ctx_id = ctx_id;
	UWE5622_SKB_CB(skb)->ba_cmd = UWE5622_CMD_ADDBA_REQ;
	skb_queue_tail(&wifi->ba_queue, skb);
	schedule_work(&wifi->ba_work);
}

static void uwe5622_ba_work(struct work_struct *work)
{
	struct uwe5622_wifi *wifi = container_of(work, struct uwe5622_wifi,
						ba_work);
	struct sk_buff *skb;

	while ((skb = skb_dequeue(&wifi->ba_queue))) {
		u8 id = UWE5622_SKB_CB(skb)->ba_cmd;
		int ret;

		ret = uwe5622_wifi_cmd(wifi, UWE5622_SKB_CB(skb)->ctx_id, id,
				       skb->data, skb->len, NULL, NULL, NULL);
		if (ret && id == UWE5622_CMD_ADDBA_REQ) {
			const struct uwe5622_cmd_addba *req =
				(const void *)skb->data;
			u8 tid = FIELD_GET(UWE5622_BA_PARAM_TID,
					   le16_to_cpu(req->param));

			spin_lock_bh(&wifi->vif_lock);
			if (req->sta_lut < ARRAY_SIZE(wifi->peers)) {
				clear_bit(tid, &wifi->peers[req->sta_lut].ba_tx);
				wifi->peers[req->sta_lut].ba_retry = jiffies +
					UWE5622_BA_RETRY;
			}
			spin_unlock_bh(&wifi->vif_lock);
		}
		kfree_skb(skb);
	}
}

static void uwe5622_eapol_work(struct work_struct *work)
{
	struct uwe5622_wifi *wifi = container_of(work, struct uwe5622_wifi,
						 eapol_work);
	struct uwe5622_vif *vif;
	struct net_device *ndev;
	struct sk_buff *skb, *tx;
	u8 *data;
	int ret;

	while ((skb = skb_dequeue(&wifi->eapol_queue))) {
		ndev = uwe5622_get_ndev(wifi, UWE5622_SKB_CB(skb)->ctx_id);
		if (!ndev) {
			kfree_skb(skb);
			continue;
		}
		vif = netdev_priv(ndev);
		tx = uwe5622_build_tx(vif, skb, 0, 0);
		if (!tx) {
			ndev->stats.tx_dropped++;
			goto free;
		}
		data = skb_push(tx, 5);
		memcpy(data, "01234", 5);
		ret = uwe5622_wifi_cmd(wifi, vif->ctx_id, UWE5622_CMD_TX_DATA,
				       tx->data, tx->len, NULL, NULL, NULL);
		kfree_skb(tx);
		if (ret) {
			ndev->stats.tx_dropped++;
		} else {
			ndev->stats.tx_packets++;
			ndev->stats.tx_bytes += skb->len;
		}
free:
		kfree_skb(skb);
		dev_put(ndev);
	}
}

/*
 * The firmware wants to know the station's address once DHCP has run: it uses it
 * to answer for the host while it sleeps, and its own transmit aggregation
 * decisions wait on it as well.
 */
static int uwe5622_inetaddr_event(struct notifier_block *nb, unsigned long event,
				  void *data)
{
	struct uwe5622_wifi *wifi = container_of(nb, struct uwe5622_wifi,
						 inetaddr_notifier);
	struct in_ifaddr *ifa = data;
	struct net_device *ndev = ifa->ifa_dev->dev;
	struct uwe5622_vif *vif;

	if (event != NETDEV_UP || ndev->netdev_ops != &uwe5622_netdev_ops)
		return NOTIFY_DONE;

	vif = netdev_priv(ndev);
	if (vif->wifi != wifi || vif->mode != UWE5622_MODE_STATION)
		return NOTIFY_DONE;

	if (uwe5622_wifi_cmd(wifi, vif->ctx_id, UWE5622_CMD_NOTIFY_IP_ACQUIRED,
			     &ifa->ifa_address, sizeof(ifa->ifa_address),
			     NULL, NULL, NULL))
		dev_warn(wifi->dev, "firmware rejected the acquired address\n");

	return NOTIFY_DONE;
}

/*
 * The firmware keeps its own regulatory table and has to be told which rules the
 * host settled on, or it stays on whatever it booted with. One rule per distinct
 * frequency range covering the channels that are actually usable is what the
 * firmware expects, so ranges are emitted once even though several channels
 * share them.
 */
#define UWE5622_STD_11D		BIT(0)
#define UWE5622_REGDOM_RULES	16

struct uwe5622_reg_rule {
	__le32 start_freq_khz;
	__le32 end_freq_khz;
	__le32 max_bandwidth_khz;
	__le32 max_antenna_gain;
	__le32 max_eirp;
	__le32 flags;
	__le32 dfs_cac_ms;
} __packed;

struct uwe5622_cmd_regdom {
	__le32 n_rules;
	char alpha2[2];
	u8 reserved[2];
	struct uwe5622_reg_rule rules[];
} __packed;

static void uwe5622_reg_notify(struct wiphy *wiphy,
			       struct regulatory_request *request)
{
	struct uwe5622_wifi *wifi = wiphy_priv(wiphy);
	const struct ieee80211_reg_rule *rule;
	const struct ieee80211_freq_range *range;
	struct uwe5622_cmd_regdom *regdom;
	unsigned int i, band, rules = 0;
	u32 last_start = 0;
	size_t len;

	len = struct_size(regdom, rules, UWE5622_REGDOM_RULES);
	regdom = kzalloc(len, GFP_KERNEL);
	if (!regdom)
		return;

	for (band = 0; band < NUM_NL80211_BANDS; band++) {
		struct ieee80211_supported_band *sband = wiphy->bands[band];

		if (!sband)
			continue;
		for (i = 0; i < sband->n_channels; i++) {
			struct ieee80211_channel *chan = &sband->channels[i];

			if (chan->flags & IEEE80211_CHAN_DISABLED)
				continue;
			rule = freq_reg_info(wiphy,
					     MHZ_TO_KHZ(chan->center_freq));
			if (IS_ERR(rule))
				continue;
			range = &rule->freq_range;
			if (range->start_freq_khz == last_start ||
			    rules == UWE5622_REGDOM_RULES)
				continue;
			last_start = range->start_freq_khz;
			regdom->rules[rules].start_freq_khz =
				cpu_to_le32(range->start_freq_khz);
			regdom->rules[rules].end_freq_khz =
				cpu_to_le32(range->end_freq_khz);
			regdom->rules[rules].max_bandwidth_khz =
				cpu_to_le32(range->max_bandwidth_khz);
			regdom->rules[rules].max_antenna_gain =
				cpu_to_le32(rule->power_rule.max_antenna_gain);
			regdom->rules[rules].max_eirp =
				cpu_to_le32(rule->power_rule.max_eirp);
			regdom->rules[rules].flags = cpu_to_le32(rule->flags);
			regdom->rules[rules].dfs_cac_ms =
				cpu_to_le32(rule->dfs_cac_ms);
			rules++;
		}
	}

	regdom->n_rules = cpu_to_le32(rules);
	regdom->alpha2[0] = request->alpha2[0];
	regdom->alpha2[1] = request->alpha2[1];

	if (uwe5622_wifi_cmd(wifi, 0, UWE5622_CMD_SET_REGDOM, regdom,
			     struct_size(regdom, rules, rules),
			     NULL, NULL, NULL))
		dev_warn(wifi->dev, "firmware rejected regulatory domain %c%c\n",
			 request->alpha2[0], request->alpha2[1]);
	kfree(regdom);
}

static int uwe5622_open_firmware(struct uwe5622_vif *vif)
{
	struct uwe5622_cmd_open open = { .mode = vif->mode };
	size_t response_len = 0;
	u8 ctx;
	int ret;

	ether_addr_copy(open.mac, vif->wdev.netdev->dev_addr);
	ret = uwe5622_wifi_cmd(vif->wifi, 0, UWE5622_CMD_OPEN, &open,
			       sizeof(open), NULL, &response_len, &ctx);
	if (ret)
		return ret;
	if (ctx >= UWE5622_WIFI_MAX_CTX)
		return -ERANGE;
	vif->ctx_id = ctx;
	vif->opened = true;
	return 0;
}

static void uwe5622_close_firmware(struct uwe5622_vif *vif)
{
	u8 mode = vif->mode;

	if (!vif->opened)
		return;
	uwe5622_release_credit_pool(vif->wifi, vif);
	uwe5622_wifi_cmd(vif->wifi, vif->ctx_id, UWE5622_CMD_CLOSE,
			 &mode, sizeof(mode), NULL, NULL, NULL);
	vif->opened = false;
}

/*
 * An access point must not answer on the same address as the station interface,
 * so it runs on a locally administered variant of the permanent address.
 */
static void uwe5622_vif_address(struct uwe5622_wifi *wifi,
				enum nl80211_iftype type, u8 address[ETH_ALEN])
{
	ether_addr_copy(address, wifi->perm_addr);
	if (type == NL80211_IFTYPE_AP) {
		address[0] |= 0x02;
		address[5] ^= 0x80;
	}
}

static struct net_device *uwe5622_first_ndev(struct uwe5622_wifi *wifi)
{
	struct net_device *ndev = NULL;
	int i;

	spin_lock_bh(&wifi->vif_lock);
	for (i = 0; i < UWE5622_WIFI_MAX_CTX; i++) {
		if (!wifi->vifs[i])
			continue;
		ndev = wifi->vifs[i];
		dev_hold(ndev);
		break;
	}
	spin_unlock_bh(&wifi->vif_lock);
	return ndev;
}

static void uwe5622_set_vif_type(struct uwe5622_vif *vif,
				 enum nl80211_iftype type, u8 address[ETH_ALEN])
{
	vif->mode = type == NL80211_IFTYPE_AP ? UWE5622_MODE_AP :
						UWE5622_MODE_STATION;
	vif->sta_lut = type == NL80211_IFTYPE_AP ? 4 : 0;
	uwe5622_vif_address(vif->wifi, type, address);
}

static void uwe5622_forget_vif(struct uwe5622_vif *vif)
{
	struct uwe5622_wifi *wifi = vif->wifi;

	spin_lock_bh(&wifi->vif_lock);
	if (wifi->vifs[vif->ctx_id] == vif->wdev.netdev)
		wifi->vifs[vif->ctx_id] = NULL;
	spin_unlock_bh(&wifi->vif_lock);
}

static int uwe5622_remember_vif(struct uwe5622_vif *vif)
{
	struct uwe5622_wifi *wifi = vif->wifi;

	spin_lock_bh(&wifi->vif_lock);
	if (wifi->vifs[vif->ctx_id]) {
		spin_unlock_bh(&wifi->vif_lock);
		uwe5622_close_firmware(vif);
		return -EBUSY;
	}
	wifi->vifs[vif->ctx_id] = vif->wdev.netdev;
	spin_unlock_bh(&wifi->vif_lock);

	return 0;
}

/*
 * Replace a context with a fresh one describing the same interface. The
 * firmware picks the context number, so the mapping has to be dropped before
 * the old context is closed and taken again once the new one is open.
 */
static int uwe5622_reopen_firmware(struct uwe5622_vif *vif)
{
	int ret;

	uwe5622_forget_vif(vif);
	uwe5622_close_firmware(vif);
	ret = uwe5622_open_firmware(vif);
	if (ret)
		return ret;

	return uwe5622_remember_vif(vif);
}

static struct wireless_dev *
uwe5622_add_virtual_intf(struct wiphy *wiphy, const char *name,
			unsigned char name_assign_type,
			enum nl80211_iftype type, struct vif_params *params)
{
	struct uwe5622_wifi *wifi = wiphy_priv(wiphy);
	struct uwe5622_vif *vif;
	struct net_device *ndev;
	u8 address[ETH_ALEN];
	int ret;

	if (type != NL80211_IFTYPE_STATION && type != NL80211_IFTYPE_AP)
		return ERR_PTR(-EOPNOTSUPP);

	/*
	 * Only one interface at a time. The wiphy describes no interface
	 * combinations, both types are derived from the one permanent address,
	 * and the system sleep callbacks only ever reach the first interface, so
	 * a second one would open a context sharing an address with the first
	 * and then be left behind on suspend.
	 */
	ndev = uwe5622_first_ndev(wifi);
	if (ndev) {
		dev_put(ndev);
		return ERR_PTR(-EOPNOTSUPP);
	}

	ndev = alloc_etherdev(sizeof(*vif));
	if (!ndev)
		return ERR_PTR(-ENOMEM);
	vif = netdev_priv(ndev);
	vif->wifi = wifi;
	vif->credit_pool = UWE5622_CREDIT_NO_POOL;
	vif->wdev.wiphy = wiphy;
	vif->wdev.iftype = type;
	vif->wdev.netdev = ndev;
	ndev->ieee80211_ptr = &vif->wdev;
	ndev->netdev_ops = &uwe5622_netdev_ops;
	ndev->needs_free_netdev = true;
	SET_NETDEV_DEV(ndev, wiphy_dev(wiphy));
	uwe5622_set_vif_type(vif, type, address);
	eth_hw_addr_set(ndev, address);
	strscpy(ndev->name, name, IFNAMSIZ);
	ndev->name_assign_type = name_assign_type;

	ret = uwe5622_open_firmware(vif);
	if (ret)
		goto err_free;
	ret = cfg80211_register_netdevice(ndev);
	if (ret)
		goto err_close;

	ret = uwe5622_remember_vif(vif);
	if (ret)
		goto err_unregister;

	return &vif->wdev;

err_unregister:
	cfg80211_unregister_netdevice(ndev);
	return ERR_PTR(ret);
err_close:
	uwe5622_close_firmware(vif);
err_free:
	free_netdev(ndev);
	return ERR_PTR(ret);
}

/*
 * Completing a scan that cfg80211 is no longer tracking makes it warn, so a scan
 * is only ever completed once and only for the interface that asked for it. That
 * matters on interface teardown, which is reached with a scan outstanding when
 * the firmware stops answering.
 */
static void uwe5622_finish_scan_wdev(struct uwe5622_wifi *wifi,
				     struct wireless_dev *wdev, bool aborted)
{
	struct cfg80211_scan_request *request;
	struct cfg80211_scan_info info = { .aborted = aborted };

	spin_lock_bh(&wifi->scan_lock);
	request = wifi->scan_request;
	if (request && wdev && request->wdev != wdev)
		request = NULL;
	else
		wifi->scan_request = NULL;
	spin_unlock_bh(&wifi->scan_lock);
	if (request)
		cfg80211_scan_done(request, &info);
}

static void uwe5622_finish_scan(struct uwe5622_wifi *wifi, bool aborted)
{
	uwe5622_finish_scan_wdev(wifi, NULL, aborted);
}

static void uwe5622_abort_scan(struct wiphy *wiphy, struct wireless_dev *wdev)
{
	uwe5622_finish_scan_wdev(wiphy_priv(wiphy), wdev, true);
}

static int uwe5622_del_virtual_intf(struct wiphy *wiphy,
				    struct wireless_dev *wdev)
{
	struct uwe5622_wifi *wifi = wiphy_priv(wiphy);
	struct uwe5622_vif *vif = uwe5622_vif_from_wdev(wdev);
	struct net_device *ndev = wdev->netdev;

	uwe5622_forget_vif(vif);
	uwe5622_finish_scan_wdev(wifi, wdev, true);
	uwe5622_close_firmware(vif);
	cfg80211_unregister_netdevice(ndev);
	return 0;
}

/*
 * The firmware ties a context to the mode it was opened with, so switching
 * between station and access point means closing the context and opening a new
 * one. Userspace brings the interface down for this, which is also what lets the
 * hardware address change.
 */
static int uwe5622_change_virtual_intf(struct wiphy *wiphy,
				       struct net_device *ndev,
				       enum nl80211_iftype type,
				       struct vif_params *params)
{
	struct uwe5622_wifi *wifi = wiphy_priv(wiphy);
	struct uwe5622_vif *vif = netdev_priv(ndev);
	enum nl80211_iftype old_type;
	u8 address[ETH_ALEN];
	int ret;

	if (type != NL80211_IFTYPE_STATION && type != NL80211_IFTYPE_AP)
		return -EOPNOTSUPP;
	if (ndev->ieee80211_ptr->iftype == type)
		return 0;
	if (netif_running(ndev))
		return -EBUSY;

	old_type = ndev->ieee80211_ptr->iftype;
	uwe5622_set_vif_type(vif, type, address);
	eth_hw_addr_set(ndev, address);

	ret = uwe5622_reopen_firmware(vif);
	if (ret) {
		/*
		 * cfg80211 still describes the interface the way it was, so put
		 * everything back rather than leaving a netdev whose mode and
		 * address describe a type it has no context for. A retry would
		 * otherwise return success at the same-type check above with the
		 * firmware holding nothing at all.
		 */
		uwe5622_set_vif_type(vif, old_type, address);
		eth_hw_addr_set(ndev, address);
		if (uwe5622_reopen_firmware(vif))
			dev_err(wifi->dev,
				"failed to restore the %s context: %d\n",
				old_type == NL80211_IFTYPE_AP ? "access point" :
								"station",
				ret);
		return ret;
	}
	ndev->ieee80211_ptr->iftype = type;

	return 0;
}

static int uwe5622_scan(struct wiphy *wiphy,
			struct cfg80211_scan_request *request)
{
	struct uwe5622_wifi *wifi = wiphy_priv(wiphy);
	struct uwe5622_vif *vif = uwe5622_vif_from_wdev(request->wdev);
	size_t ssid_len = 0, len;
	u16 n5 = 0;
	u32 channels = 0;
	u8 *data, *p;
	int i, ret;

	for (i = 0; i < request->n_ssids; i++)
		ssid_len += 1 + request->ssids[i].ssid_len;
	for (i = 0; i < request->n_channels; i++) {
		if (request->channels[i]->band == NL80211_BAND_2GHZ)
			channels |= BIT(request->channels[i]->hw_value - 1);
		else
			n5++;
	}
	len = 8 + 2 + ssid_len + 2 + n5 * sizeof(__le16);
	data = kzalloc(len, GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	put_unaligned_le32(channels, data);
	put_unaligned_le16(ssid_len, data + 8);
	p = data + 10;
	for (i = 0; i < request->n_ssids; i++) {
		*p++ = request->ssids[i].ssid_len;
		memcpy(p, request->ssids[i].ssid, request->ssids[i].ssid_len);
		p += request->ssids[i].ssid_len;
	}
	put_unaligned_le16(n5, p);
	p += 2;
	for (i = 0; i < request->n_channels; i++) {
		if (request->channels[i]->band == NL80211_BAND_5GHZ) {
			put_unaligned_le16(request->channels[i]->hw_value, p);
			p += 2;
		}
	}

	ret = uwe5622_set_ie(vif, 1, request->ie, request->ie_len);
	if (ret)
		goto out;
	spin_lock_bh(&wifi->scan_lock);
	if (wifi->scan_request) {
		spin_unlock_bh(&wifi->scan_lock);
		ret = -EBUSY;
		goto out;
	}
	wifi->scan_request = request;
	spin_unlock_bh(&wifi->scan_lock);
	ret = uwe5622_wifi_cmd(wifi, vif->ctx_id, UWE5622_CMD_SCAN,
			       data, len, NULL, NULL, NULL);
	if (ret)
		uwe5622_finish_scan(wifi, true);
out:
	kfree(data);
	return ret;
}

static int uwe5622_connect(struct wiphy *wiphy, struct net_device *ndev,
			   struct cfg80211_connect_params *sme)
{
	struct uwe5622_vif *vif = netdev_priv(ndev);
	struct uwe5622_cmd_connect connect = {};
	u8 cipher;
	int ret;

	if (!sme->ssid || sme->ssid_len > sizeof(connect.ssid))
		return -EINVAL;
	ret = uwe5622_set_ie(vif, 3, sme->ie, sme->ie_len);
	if (ret)
		return ret;

	connect.wpa_versions = cpu_to_le32(sme->crypto.wpa_versions);
	if (sme->bssid)
		ether_addr_copy(connect.bssid, sme->bssid);
	else if (sme->bssid_hint)
		ether_addr_copy(connect.bssid, sme->bssid_hint);
	if (sme->channel)
		connect.channel = ieee80211_frequency_to_channel(
						 sme->channel->center_freq);
	else if (sme->channel_hint)
		connect.channel = ieee80211_frequency_to_channel(
						 sme->channel_hint->center_freq);
	connect.auth_type = sme->auth_type == NL80211_AUTHTYPE_SHARED_KEY;
	if (sme->crypto.n_ciphers_pairwise) {
		cipher = uwe5622_cipher(sme->crypto.ciphers_pairwise[0]);
		if (!cipher)
			return -EOPNOTSUPP;
		connect.pairwise_cipher = cipher | UWE5622_VALID_CONFIG;
	}
	if (sme->crypto.cipher_group) {
		cipher = uwe5622_cipher(sme->crypto.cipher_group);
		if (!cipher)
			return -EOPNOTSUPP;
		connect.group_cipher = cipher | UWE5622_VALID_CONFIG;
	}
	if (sme->crypto.n_akm_suites) {
		u8 akm = uwe5622_akm(sme->crypto.akm_suites[0]);

		/*
		 * The firmware takes the valid bit as permission to use the
		 * number beside it, so an unsupported suite must be refused
		 * here rather than sent as a valid zero.
		 */
		if (!akm)
			return -EOPNOTSUPP;
		connect.key_mgmt = akm | UWE5622_VALID_CONFIG;
	}
	connect.mfp = sme->mfp;
	connect.ssid_len = sme->ssid_len;
	memcpy(connect.ssid, sme->ssid, sme->ssid_len);

	/* PMK offload is deliberately disabled; iwd supplies EAPOL and keys. */
	return uwe5622_wifi_cmd(vif->wifi, vif->ctx_id, UWE5622_CMD_CONNECT,
				&connect, sizeof(connect), NULL, NULL, NULL);
}

static int uwe5622_disconnect(struct wiphy *wiphy, struct net_device *ndev,
			      u16 reason)
{
	struct uwe5622_vif *vif = netdev_priv(ndev);
	__le16 value = cpu_to_le16(reason);

	return uwe5622_wifi_cmd(vif->wifi, vif->ctx_id, UWE5622_CMD_DISCONNECT,
				&value, sizeof(value), NULL, NULL, NULL);
}

static int uwe5622_add_key(struct wiphy *wiphy, struct wireless_dev *wdev,
			   int link_id, u8 index, bool pairwise,
			   const u8 *mac, struct key_params *params)
{
	struct uwe5622_vif *vif = uwe5622_vif_from_wdev(wdev);
	struct uwe5622_key_add *key;
	u8 *data, cipher;
	size_t len;
	int ret;

	if (link_id >= 0 || index > 3 || params->key_len > WLAN_MAX_KEY_LEN)
		return -EINVAL;
	cipher = uwe5622_cipher(params->cipher);
	if (!cipher)
		return -EOPNOTSUPP;
	len = 1 + sizeof(*key) + params->key_len;
	data = kzalloc(len, GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data[0] = 3;
	key = (void *)(data + 1);
	key->index = index;
	key->pairwise = pairwise;
	if (mac)
		ether_addr_copy(key->mac, mac);
	if (params->seq)
		memcpy(key->sequence, params->seq, min_t(int, params->seq_len, 8));
	key->cipher = cipher;
	key->len = params->key_len;
	memcpy(key->data, params->key, params->key_len);
	ret = uwe5622_wifi_cmd(vif->wifi, vif->ctx_id, UWE5622_CMD_KEY,
			       data, len, NULL, NULL, NULL);
	kfree(data);
	return ret;
}

static int uwe5622_del_key(struct wiphy *wiphy, struct wireless_dev *wdev,
			   int link_id, u8 index, bool pairwise, const u8 *mac)
{
	struct uwe5622_vif *vif = uwe5622_vif_from_wdev(wdev);
	struct {
		u8 subcmd;
		struct uwe5622_key_del key;
	} __packed data = { .subcmd = 4 };

	if (link_id >= 0 || index > 3)
		return -EINVAL;
	data.key.index = index;
	data.key.pairwise = pairwise;
	if (mac)
		ether_addr_copy(data.key.mac, mac);
	return uwe5622_wifi_cmd(vif->wifi, vif->ctx_id, UWE5622_CMD_KEY,
				&data, sizeof(data), NULL, NULL, NULL);
}

static int uwe5622_set_default_key(struct wiphy *wiphy,
				   struct net_device *ndev, int link_id,
				   u8 index, bool unicast, bool multicast)
{
	struct uwe5622_vif *vif = netdev_priv(ndev);
	u8 data[] = { 2, index };

	if (link_id >= 0 || index > 3)
		return -EINVAL;
	return uwe5622_wifi_cmd(vif->wifi, vif->ctx_id, UWE5622_CMD_KEY,
				data, sizeof(data), NULL, NULL, NULL);
}

static int uwe5622_start_ap(struct wiphy *wiphy, struct net_device *ndev,
			    struct cfg80211_ap_settings *settings)
{
	struct uwe5622_vif *vif = netdev_priv(ndev);
	const struct cfg80211_beacon_data *beacon = &settings->beacon;
	size_t len, index, hidden_index;
	u8 *data, *frame, channel;
	int ret;

	if (!beacon->head || beacon->head_len < 38 || !settings->ssid ||
	    settings->ssid_len > IEEE80211_MAX_SSID_LEN)
		return -EINVAL;
	channel = ieee80211_frequency_to_channel(
				settings->chandef.chan->center_freq);
	ret = uwe5622_wifi_cmd(vif->wifi, vif->ctx_id,
			       UWE5622_CMD_SET_CHANNEL, &channel, 1,
			       NULL, NULL, NULL);
	if (ret)
		return ret;
	ret = uwe5622_set_ie(vif, 0, beacon->beacon_ies,
			     beacon->beacon_ies_len);
	if (ret)
		return ret;
	ret = uwe5622_set_ie(vif, 2, beacon->proberesp_ies,
			     beacon->proberesp_ies_len);
	if (ret)
		return ret;
	ret = uwe5622_set_ie(vif, 4, beacon->assocresp_ies,
			     beacon->assocresp_ies_len);
	if (ret)
		return ret;

	len = beacon->head_len + beacon->tail_len + 1;
	if (settings->hidden_ssid)
		len += settings->ssid_len;
	data = kzalloc(2 + len, GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	put_unaligned_le16(len, data);
	frame = data + 2;
	memcpy(frame, beacon->head, 37);
	index = 37;
	frame[index++] = settings->ssid_len + 1;
	memcpy(frame + index, settings->ssid, settings->ssid_len);
	index += settings->ssid_len;
	frame[index++] = settings->hidden_ssid;
	hidden_index = settings->hidden_ssid ? index - settings->ssid_len :
						 index;
	memcpy(frame + index, beacon->head + hidden_index - 1,
	       beacon->head_len + 1 - hidden_index);
	if (beacon->tail)
		memcpy(frame + beacon->head_len + 1 +
		       (settings->hidden_ssid ? settings->ssid_len : 0),
		       beacon->tail, beacon->tail_len);
	ret = uwe5622_wifi_cmd(vif->wifi, vif->ctx_id, UWE5622_CMD_START_AP,
			       data, 2 + len, NULL, NULL, NULL);
	kfree(data);
	if (!ret)
		netif_carrier_on(ndev);
	return ret;
}

static int uwe5622_change_beacon(struct wiphy *wiphy, struct net_device *ndev,
				 struct cfg80211_ap_update *info)
{
	struct uwe5622_vif *vif = netdev_priv(ndev);
	const struct cfg80211_beacon_data *beacon = &info->beacon;
	int ret;

	ret = uwe5622_set_ie(vif, 0, beacon->beacon_ies,
			     beacon->beacon_ies_len);
	if (ret)
		return ret;
	ret = uwe5622_set_ie(vif, 2, beacon->proberesp_ies,
			     beacon->proberesp_ies_len);
	if (ret)
		return ret;
	return uwe5622_set_ie(vif, 4, beacon->assocresp_ies,
			      beacon->assocresp_ies_len);
}

static int uwe5622_stop_ap(struct wiphy *wiphy, struct net_device *ndev,
			   unsigned int link_id)
{
	struct uwe5622_vif *vif = netdev_priv(ndev);
	int ret;

	if (link_id)
		return -EINVAL;
	netif_carrier_off(ndev);

	/*
	 * There is no command that stops an access point: starting one brings
	 * its context up and only closing the context takes it down again.
	 * Returning here would leave the firmware beaconing after cfg80211 has
	 * told everyone the access point stopped, and the next start would find
	 * the context already running, so trade it for a fresh one.
	 */
	ret = uwe5622_reopen_firmware(vif);
	if (ret)
		dev_err(vif->wifi->dev,
			"failed to close the access point context: %d\n", ret);

	return ret;
}

static int uwe5622_del_station(struct wiphy *wiphy, struct wireless_dev *wdev,
			       struct station_del_parameters *params)
{
	struct uwe5622_vif *vif = uwe5622_vif_from_wdev(wdev);
	struct {
		u8 mac[ETH_ALEN];
		__le16 reason;
	} __packed data = { .reason = cpu_to_le16(params->reason_code) };

	if (params->mac)
		ether_addr_copy(data.mac, params->mac);
	return uwe5622_wifi_cmd(vif->wifi, vif->ctx_id,
				UWE5622_CMD_DEL_STATION, &data, sizeof(data),
				NULL, NULL, NULL);
}

/*
 * Sub-type 4 of the power-save command carries the 802.11 power-save state, as
 * opposed to sub-type 5 which the suspend path uses to park the firmware.
 */

/*
 * Firmware station report: a five byte rate description followed by the signal,
 * the noise floor and the transmit failure count.
 */
struct uwe5622_station_report {
	u8 rate_flags;
	u8 mcs;
	__le16 legacy;
	u8 nss;
	s8 signal;
	u8 noise;
	u8 reserved;
	__le32 tx_failed;
} __packed;

#define UWE5622_RATE_BW_40		BIT(2)
#define UWE5622_RATE_BW_80		BIT(3)
#define UWE5622_RATE_BW_160		(BIT(4) | BIT(5))
#define UWE5622_RATE_SHORT_GI		BIT(6)
#define UWE5622_RATE_MODE_MASK		GENMASK(1, 0)

static int uwe5622_get_station(struct wiphy *wiphy, struct wireless_dev *wdev,
			       const u8 *mac, struct station_info *sinfo)
{
	struct uwe5622_vif *vif = uwe5622_vif_from_wdev(wdev);
	struct net_device *ndev = wdev->netdev;
	struct uwe5622_station_report report = {};
	size_t len = sizeof(report);
	u8 mode;
	int ret;

	sinfo->filled = BIT_ULL(NL80211_STA_INFO_TX_BYTES) |
			BIT_ULL(NL80211_STA_INFO_TX_PACKETS) |
			BIT_ULL(NL80211_STA_INFO_RX_BYTES) |
			BIT_ULL(NL80211_STA_INFO_RX_PACKETS);
	sinfo->tx_bytes = ndev->stats.tx_bytes;
	sinfo->tx_packets = ndev->stats.tx_packets;
	sinfo->rx_bytes = ndev->stats.rx_bytes;
	sinfo->rx_packets = ndev->stats.rx_packets;

	ret = uwe5622_wifi_cmd(vif->wifi, vif->ctx_id, UWE5622_CMD_GET_STATION,
			       NULL, 0, &report, &len, NULL);
	if (ret || len < sizeof(report))
		return ret ? ret : 0;

	sinfo->signal = report.signal;
	sinfo->tx_failed = le32_to_cpu(report.tx_failed);
	sinfo->filled |= BIT_ULL(NL80211_STA_INFO_SIGNAL) |
			 BIT_ULL(NL80211_STA_INFO_TX_FAILED) |
			 BIT_ULL(NL80211_STA_INFO_TX_BITRATE);

	if (report.rate_flags & UWE5622_RATE_BW_160)
		sinfo->txrate.bw = RATE_INFO_BW_160;
	else if (report.rate_flags & UWE5622_RATE_BW_80)
		sinfo->txrate.bw = RATE_INFO_BW_80;
	else if (report.rate_flags & UWE5622_RATE_BW_40)
		sinfo->txrate.bw = RATE_INFO_BW_40;
	else
		sinfo->txrate.bw = RATE_INFO_BW_20;

	mode = report.rate_flags & UWE5622_RATE_MODE_MASK;
	if (mode & (RATE_INFO_FLAGS_MCS | RATE_INFO_FLAGS_VHT_MCS)) {
		sinfo->txrate.flags = mode;
		sinfo->txrate.mcs = report.mcs;
		if ((mode & RATE_INFO_FLAGS_VHT_MCS) && report.nss)
			sinfo->txrate.nss = report.nss;
	} else {
		sinfo->txrate.legacy = le16_to_cpu(report.legacy);
	}
	if (report.rate_flags & UWE5622_RATE_SHORT_GI)
		sinfo->txrate.flags |= RATE_INFO_FLAGS_SHORT_GI;

	return 0;
}

static int uwe5622_set_power_mgmt(struct wiphy *wiphy, struct net_device *ndev,
				  bool enabled, int timeout)
{
	struct uwe5622_vif *vif = netdev_priv(ndev);
	u8 data[] = { 4, enabled };

	return uwe5622_wifi_cmd(vif->wifi, vif->ctx_id, UWE5622_CMD_POWER_SAVE,
				data, sizeof(data), NULL, NULL, NULL);
}

static int uwe5622_wifi_suspend(struct wiphy *wiphy,
				struct cfg80211_wowlan *wowlan)
{
	struct uwe5622_wifi *wifi = wiphy_priv(wiphy);
	struct net_device *ndev = uwe5622_first_ndev(wifi);
	struct uwe5622_vif *vif;
	u8 data[] = { 5, 0 };
	int ret;

	flush_work(&wifi->eapol_work);
	flush_work(&wifi->ba_work);
	if (!ndev)
		return 0;
	vif = netdev_priv(ndev);
	ret = uwe5622_wifi_cmd(wifi, vif->ctx_id, UWE5622_CMD_POWER_SAVE,
			       data, sizeof(data), NULL, NULL, NULL);
	if (!ret)
		netif_device_detach(ndev);
	dev_put(ndev);
	return ret;
}

static int uwe5622_wifi_resume(struct wiphy *wiphy)
{
	struct uwe5622_wifi *wifi = wiphy_priv(wiphy);
	struct net_device *ndev = uwe5622_first_ndev(wifi);
	struct uwe5622_vif *vif;
	u8 data[] = { 5, 1 };
	int ret;

	if (!ndev)
		return 0;
	vif = netdev_priv(ndev);
	ret = uwe5622_wifi_cmd(wifi, vif->ctx_id, UWE5622_CMD_POWER_SAVE,
			       data, sizeof(data), NULL, NULL, NULL);
	if (!ret)
		netif_device_attach(ndev);
	dev_put(ndev);
	return ret;
}

#ifdef CONFIG_PM
static void uwe5622_set_wakeup(struct wiphy *wiphy, bool enabled)
{
	struct uwe5622_wifi *wifi = wiphy_priv(wiphy);
	struct cfg80211_wowlan *wowlan = wiphy->wowlan_config;
	u8 data[2] = {};
	int ret = 0;

	uwe5622_set_wake(wifi->cmd_client, enabled);
	if (!enabled || !wowlan || wowlan->any) {
		data[0] = 0;
		ret = uwe5622_wifi_cmd(wifi, 0, UWE5622_CMD_SET_WOWLAN,
				       data, sizeof(data), NULL, NULL, NULL);
	} else {
		if (wowlan->magic_pkt) {
			data[0] = 1;
			ret = uwe5622_wifi_cmd(wifi, 0, UWE5622_CMD_SET_WOWLAN,
					       data, sizeof(data), NULL, NULL, NULL);
		}
		if (!ret && wowlan->disconnect) {
			data[0] = 2;
			ret = uwe5622_wifi_cmd(wifi, 0, UWE5622_CMD_SET_WOWLAN,
					       data, sizeof(data), NULL, NULL, NULL);
		}
	}
	if (ret)
		dev_warn(wifi->dev, "failed to configure WoWLAN: %d\n", ret);
}

static const struct wiphy_wowlan_support uwe5622_wowlan_support = {
	.flags = WIPHY_WOWLAN_ANY | WIPHY_WOWLAN_MAGIC_PKT |
		 WIPHY_WOWLAN_DISCONNECT,
};
#endif

static const struct cfg80211_ops uwe5622_cfg80211_ops = {
	.suspend = uwe5622_wifi_suspend,
	.resume = uwe5622_wifi_resume,
#ifdef CONFIG_PM
	.set_wakeup = uwe5622_set_wakeup,
#endif
	.get_station = uwe5622_get_station,
	.set_power_mgmt = uwe5622_set_power_mgmt,
	.add_virtual_intf = uwe5622_add_virtual_intf,
	.change_virtual_intf = uwe5622_change_virtual_intf,
	.del_virtual_intf = uwe5622_del_virtual_intf,
	.scan = uwe5622_scan,
	.abort_scan = uwe5622_abort_scan,
	.connect = uwe5622_connect,
	.disconnect = uwe5622_disconnect,
	.add_key = uwe5622_add_key,
	.del_key = uwe5622_del_key,
	.set_default_key = uwe5622_set_default_key,
	.start_ap = uwe5622_start_ap,
	.change_beacon = uwe5622_change_beacon,
	.stop_ap = uwe5622_stop_ap,
	.del_station = uwe5622_del_station,
};

static void uwe5622_event_scan_frame(struct uwe5622_wifi *wifi,
				      const u8 *data, size_t len)
{
	const struct uwe5622_event_mgmt_frame *frame = (const void *)data;
	struct cfg80211_inform_bss bss = {};
	struct cfg80211_bss *result;
	u16 frame_len;

	if (len < sizeof(*frame) || frame->type != 4)
		return;
	frame_len = le16_to_cpu(frame->len);
	if (frame_len > len - sizeof(*frame))
		return;
	bss.chan = ieee80211_get_channel(wifi->wiphy,
		ieee80211_channel_to_frequency(frame->channel,
			frame->channel <= 14 ? NL80211_BAND_2GHZ :
						 NL80211_BAND_5GHZ));
	if (!bss.chan)
		return;
	bss.signal = frame->signal * 100;
	bss.boottime_ns = ktime_get_boottime_ns();
	result = cfg80211_inform_bss_frame_data(wifi->wiphy, &bss,
				(struct ieee80211_mgmt *)frame->data,
				frame_len, GFP_ATOMIC);
	if (result)
		cfg80211_put_bss(wifi->wiphy, result);
}

static void uwe5622_event_connect(struct uwe5622_wifi *wifi, u8 ctx,
				  const u8 *data, size_t len)
{
	struct net_device *ndev = uwe5622_get_ndev(wifi, ctx);
	struct uwe5622_vif *vif;
	const u8 *pos, *bssid;
	u16 req_len, resp_len, status = WLAN_STATUS_UNSPECIFIED_FAILURE;

	if (!ndev)
		return;
	vif = netdev_priv(ndev);
	if (len < 1)
		goto out;
	if (data[0] != 0 && data[0] != 2) {
		if (len >= 3)
			status = data[2];
		cfg80211_connect_result(ndev, NULL, NULL, 0, NULL, 0,
					status, GFP_ATOMIC);
		goto out;
	}
	if (len < 1 + ETH_ALEN + 2 + 2)
		goto out;
	bssid = data + 1;
	pos = data + 1 + ETH_ALEN + 2;
	req_len = get_unaligned_le16(pos);
	pos += 2;
	if (req_len > data + len - pos)
		goto out;
	if (data[0] == 2) {
		cfg80211_roamed(ndev, &(struct cfg80211_roam_info) {
			.links[0].bssid = bssid,
			.req_ie = pos,
			.req_ie_len = req_len,
		}, GFP_ATOMIC);
		goto connected;
	}
	pos += req_len;
	if (data + len - pos < 2)
		goto out;
	resp_len = get_unaligned_le16(pos);
	pos += 2;
	if (resp_len > data + len - pos)
		goto out;
	cfg80211_connect_result(ndev, bssid, pos - req_len - 2, req_len,
				pos, resp_len, WLAN_STATUS_SUCCESS, GFP_ATOMIC);
connected:
	vif->connected = true;
	netif_carrier_on(ndev);
out:
	dev_put(ndev);
}

static void uwe5622_event_disconnect(struct uwe5622_wifi *wifi, u8 ctx,
				     const u8 *data, size_t len)
{
	struct net_device *ndev = uwe5622_get_ndev(wifi, ctx);
	struct uwe5622_vif *vif;
	u16 reason = WLAN_REASON_UNSPECIFIED;

	if (!ndev)
		return;
	if (len >= 2)
		reason = get_unaligned_le16(data);
	vif = netdev_priv(ndev);
	vif->connected = false;
	netif_carrier_off(ndev);
	cfg80211_disconnected(ndev, reason, NULL, 0, false, GFP_ATOMIC);
	dev_put(ndev);
}

static void uwe5622_event_new_station(struct uwe5622_wifi *wifi, u8 ctx,
				      const u8 *data, size_t len)
{
	struct station_info sinfo = {};
	struct net_device *ndev = uwe5622_get_ndev(wifi, ctx);
	u16 ie_len;

	if (!ndev)
		return;
	if (len < 9)
		goto out;
	ie_len = get_unaligned_le16(data + 7);
	if (ie_len > len - 9)
		goto out;
	if (data[0]) {
		sinfo.assoc_req_ies = data + 9;
		sinfo.assoc_req_ies_len = ie_len;
		cfg80211_new_sta(ndev->ieee80211_ptr, data + 1, &sinfo,
				 GFP_ATOMIC);
	} else {
		cfg80211_del_sta(ndev->ieee80211_ptr, data + 1, GFP_ATOMIC);
	}
out:
	dev_put(ndev);
}

void uwe5622_wifi_event(struct uwe5622_wifi *wifi,
			const struct uwe5622_cmd_hdr *hdr,
			const u8 *data, size_t len)
{
	u8 ctx = hdr->common >> 5;
	struct net_device *ndev;
	struct uwe5622_vif *vif;
	const struct uwe5622_event_sta_lut *lut;

	switch (hdr->id) {
	case UWE5622_EVENT_CONNECT:
		uwe5622_event_connect(wifi, ctx, data, len);
		break;
	case UWE5622_EVENT_DISCONNECT:
		uwe5622_event_disconnect(wifi, ctx, data, len);
		break;
	case UWE5622_EVENT_SCAN_DONE:
		uwe5622_finish_scan(wifi, !len || data[0] != 1);
		break;
	case UWE5622_EVENT_MGMT_FRAME:
		uwe5622_event_scan_frame(wifi, data, len);
		break;
	case UWE5622_EVENT_COEX_BT_ON_OFF:
		dev_info(wifi->dev, "firmware reports Bluetooth %s\n",
			 len && data[0] ? "active" : "idle");
		break;
	case UWE5622_EVENT_NEW_STATION:
		uwe5622_event_new_station(wifi, ctx, data, len);
		break;
	case UWE5622_EVENT_SDIO_FLOW_CONTROL:
		if (len >= UWE5622_CREDIT_COLORS)
			uwe5622_add_tx_credits(wifi, data,
					       !(data[0] | data[1] |
						 data[2] | data[3]));
		break;
	case UWE5622_EVENT_SDIO_SEQ_NUM:
		break;
	case UWE5622_EVENT_BA:
		uwe5622_event_ba(wifi, ctx, data, len);
		break;
	case UWE5622_EVENT_STA_LUT:
		if (len < sizeof(*lut))
			break;
		lut = (const void *)data;
		if (lut->sta_lut < ARRAY_SIZE(wifi->peers)) {
			spin_lock_bh(&wifi->vif_lock);
			if (lut->action == 0) {
				memset(&wifi->peers[lut->sta_lut], 0,
				       sizeof(wifi->peers[lut->sta_lut]));
			} else if (lut->action == 1 || lut->action == 2) {
				wifi->peers[lut->sta_lut].ctx_id = lut->ctx_id;
				wifi->peers[lut->sta_lut].sta_lut = lut->sta_lut;
				ether_addr_copy(wifi->peers[lut->sta_lut].address,
						lut->address);
				wifi->peers[lut->sta_lut].valid = true;
			}
			spin_unlock_bh(&wifi->vif_lock);
		}
		ndev = uwe5622_get_ndev(wifi, lut->ctx_id);
		if (!ndev)
			break;
		vif = netdev_priv(ndev);
		if (lut->action == 0 && vif->sta_lut == lut->sta_lut)
			WRITE_ONCE(vif->sta_lut, vif->mode == UWE5622_MODE_AP ? 4 : 0);
		else if (lut->action == 1 || lut->action == 2)
			WRITE_ONCE(vif->sta_lut, lut->sta_lut);
		dev_put(ndev);
		break;
	case UWE5622_EVENT_HANG:
		dev_err(wifi->dev, "firmware reported a hang\n");
		uwe5622_recover(wifi->cmd_client);
		break;
	default:
		dev_dbg(wifi->dev, "unhandled event %#x\n", hdr->id);
		break;
	}
}

static void uwe5622_rx_one_frame(struct uwe5622_wifi *wifi,
				 const u8 *data, size_t len)
{
	struct sk_buff_head done;
	struct net_device *ndev;
	struct sk_buff *skb;
	u32 word, info;
	u16 frame_len, flags, seq;
	u8 ctx, offset, sta_lut, tid;

	if (len < UWE5622_RX_DESC_LEN)
		return;
	word = get_unaligned_le32(data);
	ctx = FIELD_GET(GENMASK(7, 4), word);
	offset = FIELD_GET(GENMASK(15, 8), word);
	frame_len = FIELD_GET(GENMASK(31, 16), word);
	if (offset < UWE5622_RX_DESC_LEN || offset > len ||
	    frame_len > len - offset)
		return;
	ndev = uwe5622_get_ndev(wifi, ctx);
	if (!ndev)
		return;
	skb = netdev_alloc_skb_ip_align(ndev, frame_len);
	if (!skb) {
		ndev->stats.rx_dropped++;
		goto out;
	}
	skb_put_data(skb, data + offset, frame_len);
	skb->protocol = eth_type_trans(skb, ndev);
	ndev->stats.rx_packets++;
	ndev->stats.rx_bytes += frame_len;

	__skb_queue_head_init(&done);
	flags = get_unaligned_le16(data + UWE5622_RX_STA_OFFSET);
	info = get_unaligned_le32(data + UWE5622_RX_INFO_OFFSET);
	sta_lut = FIELD_GET(UWE5622_RX_STA_LUT, flags);
	tid = FIELD_GET(UWE5622_RX_INFO_TID, info);
	seq = FIELD_GET(UWE5622_RX_INFO_SEQ, info);

	if (!(flags & UWE5622_RX_STA_LUT_VALID) ||
	    !(info & UWE5622_RX_INFO_QOS) ||
	    !uwe5622_reorder_rx(wifi, sta_lut, tid, seq, skb, &done))
		__skb_queue_tail(&done, skb);

	while ((skb = __skb_dequeue(&done)))
		netif_rx(skb);
out:
	dev_put(ndev);
}

static void uwe5622_wifi_data_rx(void *priv, struct sk_buff *skb)
{
	struct uwe5622_wifi *wifi = priv;
	const u8 *pos = skb->data;
	size_t left = skb->len, advance;
	u8 count;
	u32 word;
	u16 frame_len;
	u8 offset;

	if (left < UWE5622_RX_DESC_LEN)
		goto out;
	uwe5622_add_tx_credits(wifi, pos + UWE5622_RX_CREDIT_OFFSET, false);
	count = pos[8];
	if (count <= 1) {
		uwe5622_rx_one_frame(wifi, pos, left);
		goto out;
	}

	while (count-- && left >= UWE5622_RX_DESC_LEN) {
		word = get_unaligned_le32(pos);
		offset = FIELD_GET(GENMASK(15, 8), word);
		frame_len = FIELD_GET(GENMASK(31, 16), word);
		if (offset < UWE5622_RX_DESC_LEN || frame_len > left - offset)
			break;
		uwe5622_rx_one_frame(wifi, pos, left);
		advance = ALIGN(offset + frame_len + UWE5622_RX_MH_DESC_LEN, 8);
		if (advance > left)
			break;
		pos += advance;
		left -= advance;
	}
out:
	kfree_skb(skb);
}

static void uwe5622_wifi_data_reset(void *priv)
{
	struct uwe5622_wifi *wifi = priv;
	int i;

	for (i = 0; i < UWE5622_WIFI_MAX_CTX; i++) {
		struct net_device *ndev = uwe5622_get_ndev(wifi, i);

		if (!ndev)
			continue;
		netif_device_detach(ndev);
		netif_carrier_off(ndev);
		dev_put(ndev);
	}
	uwe5622_finish_scan(wifi, true);
}

static const struct uwe5622_client_ops uwe5622_cmd_client_ops = {
	.rx = uwe5622_wifi_cmd_rx,
	.reset = uwe5622_wifi_cmd_reset,
};

/*
 * A transfer that failed on the bus never reached the firmware, so the credit
 * taken for it is still ours to spend. The tag carries the pool it came from.
 */
static void uwe5622_wifi_data_tx_error(void *priv, u8 tag)
{
	struct uwe5622_wifi *wifi = priv;
	struct net_device *ndev;
	int i;

	if (tag >= UWE5622_CREDIT_COLORS)
		return;

	spin_lock_bh(&wifi->credit_lock);
	if (wifi->tx_credits[tag] < UWE5622_CREDIT_MAX)
		wifi->tx_credits[tag]++;
	spin_unlock_bh(&wifi->credit_lock);

	for (i = 0; i < UWE5622_WIFI_MAX_CTX; i++) {
		ndev = uwe5622_get_ndev(wifi, i);
		if (!ndev)
			continue;
		ndev->stats.tx_errors++;
		if (netif_running(ndev) && netif_queue_stopped(ndev))
			netif_wake_queue(ndev);
		dev_put(ndev);
	}
}

static const struct uwe5622_client_ops uwe5622_data_client_ops = {
	.rx = uwe5622_wifi_data_rx,
	.reset = uwe5622_wifi_data_reset,
	.tx_error = uwe5622_wifi_data_tx_error,
};

static bool uwe5622_wifi_config_valid(const struct firmware *config,
				      u16 section_len[3])
{
	const u8 *section;
	u16 major, minor;
	size_t config_len;
	int i;

	if (config->size < 12 || memcmp(config->data, "UWEI", 4) ||
	    get_unaligned_le16(config->data + 4) != 1)
		return false;

	for (i = 0; i < 3; i++)
		section_len[i] = get_unaligned_le16(config->data + 6 + 2 * i);
	config_len = 12 + section_len[0] + section_len[1] + section_len[2];
	if (section_len[0] != UWE5622_WIFI_CONFIG_SEC1_LEN ||
	    section_len[1] != UWE5622_WIFI_CONFIG_SEC2_LEN ||
	    section_len[2] > UWE5622_WIFI_CONFIG_SEC3_MAX ||
	    config_len != config->size)
		return false;

	section = config->data + 12;
	major = get_unaligned_le16(section);
	minor = get_unaligned_le16(section + 2);
	if (major < 2 || major > 128 || minor > 128)
		return false;
	if (major > 2 && section[UWE5622_WIFI_CONFIG_MAGIC_OFFSET] != 0xaa)
		return false;

	return true;
}

static int uwe5622_wifi_init_firmware(struct uwe5622_wifi *wifi)
{
	struct {
		__le32 main_version;
		u8 api[256];
	} version = { .main_version = cpu_to_le32(1) };
	u8 request[] = { 2, 0, 1, 0, 0 };
	u8 info[128] = {};
	const u8 *sec2;
	u16 ampdu;
	size_t len = sizeof(info);
	static const u8 api_ids[] = {
		1, 3, 4, 5, 7, 9, 10, 11, 13, 14, 17, 18, 25, 72,
		76, 83, 0x80, 0x81, 0x82, 0x83, 0xa0, 0xb3, 0xe0,
		0xf5, 0xf6,
	};
	const struct firmware *config;
	const u8 *section, *coex;
	u16 section_len[3];
	u8 *download;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(api_ids); i++)
		version.api[api_ids[i]] = 1;
	ret = uwe5622_wifi_cmd(wifi, 0, UWE5622_CMD_SYNC_VERSION,
			       &version, sizeof(version), NULL, NULL, NULL);
	if (ret)
		return ret;

	ret = request_firmware(&config, UWE5622_WIFI_CONFIG_NAME, wifi->dev);
	if (ret)
		return dev_err_probe(wifi->dev, ret, "failed to load %s\n",
				     UWE5622_WIFI_CONFIG_NAME);
	if (!uwe5622_wifi_config_valid(config, section_len)) {
		ret = -EINVAL;
		goto out_config;
	}
	section = config->data + 12;
	for (i = 0; i < ARRAY_SIZE(section_len); i++) {
		if (!section_len[i])
			continue;
		download = kzalloc(4 + section_len[i] + sizeof(__le16),
				   GFP_KERNEL);
		if (!download) {
			ret = -ENOMEM;
			goto out_config;
		}
		download[0] = i + 1;
		memcpy(download + 4, section, section_len[i]);
		put_unaligned_le16(crc16(0xffff, section, section_len[i]),
				     download + 4 + section_len[i]);
		ret = uwe5622_wifi_cmd(wifi, 0, UWE5622_CMD_DOWNLOAD_INI,
				       download, 4 + section_len[i] + 2,
				       NULL, NULL, NULL);
		kfree(download);
		if (ret)
			goto out_config;
		section += section_len[i];
	}

	/*
	 * Report what the controller accepted. A stale image or a rejected
	 * upload is otherwise indistinguishable from an arbiter that ran and
	 * granted no airtime, and that distinction is the whole question when
	 * Bluetooth and 2.4 GHz Wi-Fi will not share the antenna.
	 */
	coex = config->data + 12 + section_len[0] + UWE5622_WIFI_COEX_OFFSET;
	dev_info(wifi->dev,
		 "configuration v%u.%u accepted, antenna %#x isolation %#x\n",
		 get_unaligned_le16(config->data + 12),
		 get_unaligned_le16(config->data + 14),
		 get_unaligned_le32(coex + UWE5622_WIFI_COEX_ANT_CFG0),
		 get_unaligned_le32(coex + UWE5622_WIFI_COEX_ISOLATION_CFG0));
	ret = 0;
out_config:
	release_firmware(config);
	if (ret)
		return dev_err_probe(wifi->dev, ret,
				     "invalid or rejected Wi-Fi configuration\n");

	ret = uwe5622_wifi_cmd(wifi, 0, UWE5622_CMD_GET_INFO,
			       request, sizeof(request), info, &len, NULL);
	if (ret)
		return ret;
	if (len < 83)
		return -EPROTO;
	wifi->fw_std = get_unaligned_le32(info + 12);
	wifi->fw_capa = get_unaligned_le32(info + 16);
	wifi->tx_with_credit = info[82] == 0;
	sec2 = info + 24;
	ampdu = get_unaligned_le16(sec2 + 2);
	wifi->band_2ghz.ht_cap.cap = get_unaligned_le16(sec2);
	wifi->band_2ghz.ht_cap.ampdu_factor = ampdu & 0x3;
	wifi->band_2ghz.ht_cap.ampdu_density = (ampdu >> 2) & 0x7;
	memcpy(&wifi->band_2ghz.ht_cap.mcs, sec2 + 4,
	       sizeof(wifi->band_2ghz.ht_cap.mcs));
	wifi->band_5ghz.ht_cap = wifi->band_2ghz.ht_cap;
	wifi->band_5ghz.vht_cap.cap = get_unaligned_le32(sec2 + 20);
	memcpy(&wifi->band_5ghz.vht_cap.vht_mcs, sec2 + 24,
	       sizeof(wifi->band_5ghz.vht_cap.vht_mcs));
	wifi->wiphy->available_antennas_tx = get_unaligned_le32(sec2 + 32);
	wifi->wiphy->available_antennas_rx = get_unaligned_le32(sec2 + 36);
	wifi->wiphy->retry_short = sec2[40];
	wifi->wiphy->retry_long = sec2[41];
	ether_addr_copy(wifi->perm_addr, info + 76);
	if (!is_valid_ether_addr(wifi->perm_addr))
		eth_random_addr(wifi->perm_addr);
	return 0;
}

static int uwe5622_wifi_probe(struct auxiliary_device *adev,
			      const struct auxiliary_device_id *id)
{
	struct uwe5622_wifi *wifi;
	struct wiphy *wiphy;
	int ret;

	wiphy = wiphy_new(&uwe5622_cfg80211_ops, sizeof(*wifi));
	if (!wiphy)
		return -ENOMEM;
	wifi = wiphy_priv(wiphy);
	wifi->dev = &adev->dev;
	wifi->wiphy = wiphy;
	wifi->band_2ghz = uwe5622_band_2ghz;
	wifi->band_5ghz = uwe5622_band_5ghz;
	mutex_init(&wifi->cmd_mutex);
	init_completion(&wifi->cmd_done);
	spin_lock_init(&wifi->vif_lock);
	spin_lock_init(&wifi->scan_lock);
	spin_lock_init(&wifi->credit_lock);
	skb_queue_head_init(&wifi->eapol_queue);
	INIT_WORK(&wifi->eapol_work, uwe5622_eapol_work);
	skb_queue_head_init(&wifi->ba_queue);
	INIT_WORK(&wifi->ba_work, uwe5622_ba_work);
	spin_lock_init(&wifi->reorder_lock);
	INIT_DELAYED_WORK(&wifi->reorder_work, uwe5622_reorder_expire);
	wifi->inetaddr_notifier.notifier_call = uwe5622_inetaddr_event;
	set_wiphy_dev(wiphy, &adev->dev);

	wifi->cmd_client = uwe5622_client_register(&adev->dev,
			UWE5622_SERVICE_WIFI_COMMAND,
			&uwe5622_cmd_client_ops, wifi);
	if (IS_ERR(wifi->cmd_client)) {
		ret = PTR_ERR(wifi->cmd_client);
		goto err_wiphy;
	}
	wifi->data_client = uwe5622_client_register(&adev->dev,
			UWE5622_SERVICE_WIFI_DATA,
			&uwe5622_data_client_ops, wifi);
	if (IS_ERR(wifi->data_client)) {
		ret = PTR_ERR(wifi->data_client);
		goto err_cmd;
	}
	ret = uwe5622_power_get(wifi->cmd_client);
	if (ret)
		goto err_data;
	ret = uwe5622_wifi_init_firmware(wifi);
	if (ret)
		goto err_power;

	wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION) |
				 BIT(NL80211_IFTYPE_AP);
	wiphy->bands[NL80211_BAND_2GHZ] = &wifi->band_2ghz;
	if (wifi->fw_capa & UWE5622_GET_INFO_CAP_5G)
		wiphy->bands[NL80211_BAND_5GHZ] = &wifi->band_5ghz;
	wiphy->signal_type = CFG80211_SIGNAL_TYPE_MBM;
	wiphy->max_scan_ssids = 9;
	wiphy->max_scan_ie_len = 255;
	wiphy->akm_suites = uwe5622_akm_suites;
	wiphy->n_akm_suites = ARRAY_SIZE(uwe5622_akm_suites);
	wiphy->cipher_suites = uwe5622_cipher_suites;
	wiphy->n_cipher_suites = ARRAY_SIZE(uwe5622_cipher_suites);
	wiphy->max_num_pmkids = 4;
#ifdef CONFIG_PM
	wiphy->wowlan = &uwe5622_wowlan_support;
#endif
	if (wifi->fw_std & UWE5622_STD_11D)
		wiphy->reg_notifier = uwe5622_reg_notify;

	if (wifi->fw_capa & UWE5622_GET_INFO_CAP_AP_SME)
		wiphy->flags |= WIPHY_FLAG_HAVE_AP_SME;
	/* Do not set NL80211_EXT_FEATURE_4WAY_HANDSHAKE_STA_PSK. */
	ret = wiphy_register(wiphy);
	if (ret)
		goto err_power;
	ret = register_inetaddr_notifier(&wifi->inetaddr_notifier);
	if (ret)
		goto err_wiphy_registered;
	auxiliary_set_drvdata(adev, wifi);
	dev_info(&adev->dev, "registered UWE5622 fullmac Wi-Fi\n");
	return 0;

err_wiphy_registered:
	wiphy_unregister(wiphy);
err_power:
	uwe5622_power_put(wifi->cmd_client);
err_data:
	uwe5622_client_unregister(wifi->data_client);
err_cmd:
	uwe5622_client_unregister(wifi->cmd_client);
err_wiphy:
	wiphy_free(wiphy);
	return ret;
}

static void uwe5622_wifi_remove(struct auxiliary_device *adev)
{
	struct uwe5622_wifi *wifi = auxiliary_get_drvdata(adev);
	struct uwe5622_vif *vif;
	struct net_device *ndev;
	int i;

	unregister_inetaddr_notifier(&wifi->inetaddr_notifier);
	cancel_work_sync(&wifi->eapol_work);
	skb_queue_purge(&wifi->eapol_queue);
	cancel_work_sync(&wifi->ba_work);
	skb_queue_purge(&wifi->ba_queue);
	cancel_delayed_work_sync(&wifi->reorder_work);
	uwe5622_reorder_close_all(wifi);
	uwe5622_finish_scan(wifi, true);
	rtnl_lock();
	for (i = 0; i < UWE5622_WIFI_MAX_CTX; i++) {
		spin_lock_bh(&wifi->vif_lock);
		ndev = wifi->vifs[i];
		wifi->vifs[i] = NULL;
		spin_unlock_bh(&wifi->vif_lock);
		if (!ndev)
			continue;
		vif = netdev_priv(ndev);
		uwe5622_close_firmware(vif);
		/* Only the RTNL is held here, not the wiphy mutex. */
		unregister_netdevice(ndev);
	}
	rtnl_unlock();
	mutex_lock(&wifi->cmd_mutex);
	wifi->stopping = true;
	mutex_unlock(&wifi->cmd_mutex);
	wiphy_unregister(wifi->wiphy);
	uwe5622_power_put(wifi->cmd_client);
	uwe5622_client_unregister(wifi->data_client);
	uwe5622_client_unregister(wifi->cmd_client);
	wiphy_free(wifi->wiphy);
}

static const struct auxiliary_device_id uwe5622_wifi_ids[] = {
	{ .name = "uwe5622_core.wifi" },
	{}
};
MODULE_DEVICE_TABLE(auxiliary, uwe5622_wifi_ids);

static struct auxiliary_driver uwe5622_wifi_driver = {
	.name = "uwe5622_wifi",
	.probe = uwe5622_wifi_probe,
	.remove = uwe5622_wifi_remove,
	.id_table = uwe5622_wifi_ids,
};
module_auxiliary_driver(uwe5622_wifi_driver);

MODULE_DESCRIPTION("Unisoc UWE5622 fullmac Wi-Fi driver");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE(UWE5622_WIFI_CONFIG_NAME);
