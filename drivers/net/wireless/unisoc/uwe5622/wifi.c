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
#include <linux/if_vlan.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/ipv6.h>
#include <net/cfg80211.h>
#include <net/ip6_checksum.h>

#include "wifi.h"

#define UWE5622_VALID_CONFIG	BIT(7)
#define UWE5622_TX_DESC_LEN	11
/* The descriptor, plus room for the header the bus adds in front of it. */
#define UWE5622_TX_HEADROOM	(UWE5622_TX_DESC_LEN + UWE5622_BUS_HEADROOM)
#define UWE5622_DATA_TX_MAX	1672
#define UWE5622_RX_DESC_LEN	28
#define UWE5622_RX_CREDIT_OFFSET	24
#define UWE5622_CREDIT_COLORS	4
#define UWE5622_CREDIT_MAX	U16_MAX
#define UWE5622_WIFI_CONFIG_MAGIC_OFFSET 251
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

/* What the controller says a management frame it hands up is for. */
#define UWE5622_FRAME_REGISTERED	1
#define UWE5622_FRAME_DEAUTH		2
#define UWE5622_FRAME_DISASSOC		3
#define UWE5622_FRAME_SCAN		4

struct uwe5622_event_mgmt_frame {
	u8 type;
	u8 channel;
	s8 signal;
	u8 reserved;
	u8 bssid[ETH_ALEN];
	__le16 len;
	u8 data[];
} __packed;

struct uwe5622_cmd_mgmt_tx {
	u8 channel;
	u8 dont_wait_for_ack;
	__le32 wait;
	__le64 cookie;
	__le16 len;
	u8 frame[];
} __packed;

struct uwe5622_cmd_register_frame {
	__le16 type;
	u8 reg;
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

/*
 * The transmit descriptor can ask the MAC hardware to complete one checksum for
 * the frame. It carries only which of TCP and UDP to complete and where the
 * transport header starts, so hardware has to place the result at the standard
 * offset for that protocol: anything else, and anything not linear, is completed
 * in software instead.
 */
#define UWE5622_TX_CSUM_ENABLE		BIT(0)
#define UWE5622_TX_CSUM_TCP		BIT(1)

static bool uwe5622_tx_csum_is_tcp(const struct sk_buff *skb)
{
	return skb->csum_offset == offsetof(struct tcphdr, check);
}

static bool uwe5622_tx_csum_offload(struct sk_buff *skb)
{
	unsigned int offset = skb_checksum_start_offset(skb);

	if (skb_is_nonlinear(skb) || skb_csum_is_sctp(skb))
		return false;
	if (offset > U16_MAX || offset + skb->csum_offset > skb->len)
		return false;

	/*
	 * Hardware knows where the checksum of a TCP or UDP header lives and
	 * nothing else. Identify the protocol by the field the stack asked to
	 * be filled rather than by the network header, which says nothing about
	 * what an IPv6 extension chain ends in.
	 */
	if (skb->csum_offset != offsetof(struct tcphdr, check) &&
	    skb->csum_offset != offsetof(struct udphdr, check))
		return false;

	return true;
}

/*
 * Which station the firmware should send a frame to. A client has one peer, a
 * group address goes to the entry the firmware reserves for it, and an access
 * point has to find the destination among its own: a frame for an address that
 * is not associated has nowhere to go.
 */
static bool uwe5622_tx_sta_lut(struct uwe5622_vif *vif,
			       const struct sk_buff *skb, u8 *sta_lut)
{
	struct uwe5622_wifi *wifi = vif->wifi;
	const struct ethhdr *eth;
	int i;

	if (vif->mode != UWE5622_MODE_AP || skb->len < ETH_HLEN) {
		*sta_lut = READ_ONCE(vif->sta_lut);
		return true;
	}

	eth = (const void *)skb->data;
	if (is_multicast_ether_addr(eth->h_dest)) {
		*sta_lut = 4;
		return true;
	}

	*sta_lut = 0;
	spin_lock_bh(&wifi->vif_lock);
	for (i = 0; i < ARRAY_SIZE(wifi->peers); i++) {
		if (wifi->peers[i].valid &&
		    wifi->peers[i].ctx_id == vif->ctx_id &&
		    ether_addr_equal(wifi->peers[i].address, eth->h_dest)) {
			*sta_lut = wifi->peers[i].sta_lut;
			break;
		}
	}
	spin_unlock_bh(&wifi->vif_lock);

	return *sta_lut >= 6;
}

/*
 * The descriptor describes the frame that follows it, so it is given the frame's
 * own length and offsets, never those of the buffer it ends up sharing.
 */
static void uwe5622_fill_tx_desc(struct uwe5622_vif *vif, u8 *desc,
				 unsigned int frame_len, u8 type, u8 color,
				 u8 sta_lut, unsigned int csum_offset,
				 bool csum_tcp)
{
	memset(desc, 0, UWE5622_TX_DESC_LEN);
	desc[0] = type | (vif->ctx_id << 5);
	desc[1] = UWE5622_TX_DESC_LEN;
	put_unaligned_le16(frame_len, desc + 3);
	desc[6] = sta_lut;
	desc[7] = color;
	if (csum_offset) {
		desc[2] = UWE5622_TX_CSUM_ENABLE;
		if (csum_tcp)
			desc[2] |= UWE5622_TX_CSUM_TCP;
		put_unaligned_le16(csum_offset, desc + 9);
	}
}

/*
 * Put the descriptor in front of the frame the stack handed over, in the
 * headroom the netdev asks every transmit skb to keep for it. The frame then
 * travels to the bus as it is, and the only copy left on the transmit path is
 * the one that packs several frames into a single transfer.
 */
static int uwe5622_push_tx_desc(struct uwe5622_vif *vif, struct sk_buff *skb,
				u8 type, u8 color, u8 *sta_lut)
{
	unsigned int csum_offset = 0;
	unsigned int frame_len;
	bool csum_tcp = false;
	u8 *desc;

	if (!uwe5622_tx_sta_lut(vif, skb, sta_lut))
		return -ENOENT;

	/* Both of these describe the frame, so they are taken before it moves. */
	if (skb->ip_summed == CHECKSUM_PARTIAL) {
		csum_offset = skb_checksum_start_offset(skb);
		csum_tcp = uwe5622_tx_csum_is_tcp(skb);
	}
	frame_len = skb->len;

	if (skb_cow_head(skb, UWE5622_TX_HEADROOM))
		return -ENOMEM;

	desc = skb_push(skb, UWE5622_TX_DESC_LEN);
	uwe5622_fill_tx_desc(vif, desc, frame_len, type, color, *sta_lut,
			     csum_offset, csum_tcp);

	return 0;
}

/*
 * The command path needs one buffer it owns, so that frame is still copied. Its
 * caller puts a marker in front of the descriptor before handing the buffer to
 * the command channel, so the headroom for that comes with the buffer.
 */
#define UWE5622_CMD_TX_MARKER	5

static struct sk_buff *uwe5622_build_tx(struct uwe5622_vif *vif,
					const struct sk_buff *skb, u8 type,
					u8 color)
{
	struct sk_buff *tx;
	u8 sta_lut;
	u8 *desc;

	if (!uwe5622_tx_sta_lut(vif, skb, &sta_lut))
		return NULL;
	tx = alloc_skb(UWE5622_CMD_TX_MARKER + UWE5622_TX_DESC_LEN + skb->len,
		       GFP_ATOMIC);
	if (!tx)
		return NULL;
	skb_reserve(tx, UWE5622_CMD_TX_MARKER);
	desc = skb_put(tx, UWE5622_TX_DESC_LEN);
	uwe5622_fill_tx_desc(vif, desc, skb->len, type, color, sta_lut, 0,
			     false);
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
	unsigned int len;
	u8 color, sta_lut;
	int ret;

	if (unlikely(skb->protocol == htons(ETH_P_PAE))) {
		if (skb_queue_len(&wifi->eapol_queue) >= UWE5622_EAPOL_QUEUE_MAX)
			return NETDEV_TX_BUSY;
		UWE5622_SKB_CB(skb)->ctx_id = vif->ctx_id;
		skb_queue_tail(&wifi->eapol_queue, skb);
		schedule_work(&wifi->eapol_work);
		return NETDEV_TX_OK;
	}

	/*
	 * Complete in software what the descriptor cannot express, before a
	 * credit is spent on the frame.
	 */
	if (skb->ip_summed == CHECKSUM_PARTIAL &&
	    !uwe5622_tx_csum_offload(skb) && skb_checksum_help(skb))
		goto drop;

	if (skb->len + UWE5622_TX_DESC_LEN > UWE5622_DATA_TX_MAX)
		goto drop;

	if (!uwe5622_take_tx_credit(wifi, ndev, &color))
		return NETDEV_TX_BUSY;
	len = skb->len;
	if (uwe5622_push_tx_desc(vif, skb, 2, color, &sta_lut))
		goto return_credit;
	uwe5622_request_tx_ba(wifi, vif->ctx_id, sta_lut, 0);
	/* The bus owns the frame from here, and frees it once it has gone out. */
	ret = uwe5622_client_send_tagged(wifi->data_client, skb, color);
	if (ret) {
		uwe5622_return_tx_credit(wifi, ndev, color);
		if (ret == -ENOBUFS) {
			skb_pull(skb, UWE5622_TX_DESC_LEN);
			return NETDEV_TX_BUSY;
		}
		goto drop;
	}

	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += len;
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

static void uwe5622_deliver(struct sk_buff_head *done)
{
	struct sk_buff *skb;
	struct list_head list;

	if (skb_queue_empty(done))
		return;

	INIT_LIST_HEAD(&list);
	while ((skb = __skb_dequeue(done)))
		list_add_tail(&skb->list, &list);

	local_bh_disable();
	netif_receive_skb_list(&list);
	local_bh_enable();
}

#define UWE5622_NAPI_QUEUE_LIMIT	256



/*
 * Receive offload needs a poll it can hold open while it coalesces, so frames go
 * through one rather than being pushed up as they arrive. Coalescing is only
 * worth having because the controller's checksum arrives with them: segments the
 * hardware has already summed can be joined without touching their payload.
 */
static int uwe5622_napi_poll(struct napi_struct *napi, int budget)
{
	struct uwe5622_vif *vif = container_of(napi, struct uwe5622_vif, napi);
	struct sk_buff *skb;
	int done = 0;

	while (done < budget && (skb = skb_dequeue(&vif->rx_queue))) {
		napi_gro_receive(napi, skb);
		done++;
	}

	if (done < budget)
		napi_complete_done(napi, done);

	return done;
}

/*
 * Queue for the poll, unless it is not running yet or has fallen behind, in which
 * case deliver here and now. An unbounded queue would hold socket buffers, and
 * every one of those holds the device, so an interface could never be taken away.
 */
static void uwe5622_deliver_vif(struct uwe5622_vif *vif,
				struct sk_buff_head *done)
{
	struct sk_buff *skb;

	if (!vif->napi_ready ||
	    skb_queue_len(&vif->rx_queue) >= UWE5622_NAPI_QUEUE_LIMIT) {
		uwe5622_deliver(done);
		return;
	}

	while ((skb = __skb_dequeue(done)))
		skb_queue_tail(&vif->rx_queue, skb);
	napi_schedule(&vif->napi);
}

static int uwe5622_reopen_firmware(struct uwe5622_vif *vif);

/*
 * A connect the firmware answers in neither direction leaves the interface
 * scanning for ever: no refusal arrives and no association happens, and nothing
 * in the firmware recovers from it. Give the attempt a deadline and replace the
 * context when it passes, which is the only thing that clears the state.
 */
#define UWE5622_CONNECT_TIMEOUT	msecs_to_jiffies(8000)

static void uwe5622_connect_watchdog(struct work_struct *work)
{
	struct uwe5622_vif *vif = container_of(to_delayed_work(work),
					       struct uwe5622_vif,
					       connect_watchdog);

	dev_warn(vif->wifi->dev,
		 "no answer to the association attempt, replacing the context\n");
	schedule_work(&vif->recover_work);
}

static void uwe5622_recover_work(struct work_struct *work)
{
	struct uwe5622_vif *vif = container_of(work, struct uwe5622_vif,
					       recover_work);
	int ret;

	ret = uwe5622_reopen_firmware(vif);
	if (ret)
		dev_err(vif->wifi->dev,
			"failed to replace the context after a refused association: %d\n",
			ret);
}

/*
 * The receive engine hands up the raw Internet accumulator over the transport
 * segment, not a verdict. Whether it can be believed is decided here, by adding
 * the pseudo header the hardware does not cover and seeing whether the result
 * comes out zero, which is what a correct checksum means. Until that has been
 * shown over live traffic there is nothing to trust: a wrong sum accepted as
 * CHECKSUM_COMPLETE makes the stack take corrupted packets in silence.
 *
 * Returns the ip_summed to use, and reports what the sum looked like either way.
 */
static u8 uwe5622_rx_csum(const u8 *frame, u16 len, u16 raw)
{
	const u8 *net = frame + ETH_HLEN;
	u16 proto = get_unaligned_be16(frame + 2 * ETH_ALEN);
	u16 netlen = len - ETH_HLEN;
	__wsum sum = (__force __wsum)raw;
	u16 l4len, verdict = 0;
	u8 l4proto;

	if (len < ETH_HLEN)
		goto skip;
	if (proto == ETH_P_8021Q || proto == ETH_P_8021AD) {
		if (netlen < VLAN_HLEN)
			goto skip;
		proto = get_unaligned_be16(net + 2);
		net += VLAN_HLEN;
		netlen -= VLAN_HLEN;
	}

	if (proto == ETH_P_IP) {
		const struct iphdr *ip = (const struct iphdr *)net;
		u16 ihl, total;

		if (netlen < sizeof(*ip) || ip->version != 4)
			goto skip;
		ihl = ip->ihl * 4;
		total = ntohs(ip->tot_len);
		if (ihl < sizeof(*ip) || total < ihl || total > netlen)
			goto skip;
		if (ip_is_fragment(ip))
			goto skip;
		l4proto = ip->protocol;
		if (l4proto != IPPROTO_TCP && l4proto != IPPROTO_UDP)
			goto skip;
		l4len = total - ihl;
		verdict = csum_tcpudp_magic(ip->saddr, ip->daddr, l4len,
					    l4proto, sum);
	} else if (proto == ETH_P_IPV6) {
		const struct ipv6hdr *ip6 = (const struct ipv6hdr *)net;

		if (netlen < sizeof(*ip6) || ip6->version != 6)
			goto skip;
		l4proto = ip6->nexthdr;
		if (l4proto != IPPROTO_TCP && l4proto != IPPROTO_UDP)
			goto skip;
		l4len = ntohs(ip6->payload_len);
		if (l4len + sizeof(*ip6) > netlen)
			goto skip;
		verdict = csum_ipv6_magic(&ip6->saddr, &ip6->daddr, l4len,
					  l4proto, sum);
	} else {
		goto skip;
	}

	if (!verdict) {
		/*
		 * The sum plus the pseudo header comes out zero, so the segment
		 * is intact. Say so rather than passing the sum on: the stack
		 * then has nothing left to check, which is what lets receive
		 * offload coalesce without touching the payload. Verifying costs
		 * only the pseudo header, never a pass over the data.
		 */
		return CHECKSUM_UNNECESSARY;
	}

	/* Anything unverified reaches the stack unclaimed, as it must. */
	return CHECKSUM_NONE;

skip:
	return CHECKSUM_NONE;
}

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

/*
 * Block acknowledgment sessions the peer proposes. The firmware does not accept
 * one by itself and only aggregates once the host answers, so without this the
 * peer sends single frames and the link runs at a fraction of its rate. The
 * answer cannot be sent from the receive path where the request arrives, so it
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
 * What the firmware answers an ADDBA request with: how the negotiation ended,
 * followed by the request it was given back unchanged. Only the result is worth
 * reading; the echo cannot report what the peer agreed to, because the firmware
 * saves the request before adding parameters of its own to the frame it sends.
 */
enum uwe5622_addba_result {
	UWE5622_ADDBA_SUCCESS,
	UWE5622_ADDBA_FAIL,
	UWE5622_ADDBA_TIMEOUT,
	UWE5622_ADDBA_DECLINE,
};

struct uwe5622_rsp_addba {
	u8 result;
	struct uwe5622_cmd_addba req;
} __packed;

static const char *uwe5622_addba_result_name(u8 result)
{
	switch (result) {
	case UWE5622_ADDBA_FAIL:
		return "failed";
	case UWE5622_ADDBA_TIMEOUT:
		return "timed out";
	case UWE5622_ADDBA_DECLINE:
		return "declined";
	default:
		return "rejected";
	}
}

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
	case UWE5622_BA_DELBA_ALL_EVENT:
	default:
		return;
	}

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

	if (sta_lut >= ARRAY_SIZE(wifi->peers))
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
		struct uwe5622_rsp_addba rsp = {};
		size_t rsp_len = sizeof(rsp);
		bool refused;
		int ret;

		ret = uwe5622_wifi_cmd(wifi, UWE5622_SKB_CB(skb)->ctx_id, id,
				       skb->data, skb->len,
				       id == UWE5622_CMD_ADDBA_REQ ? &rsp : NULL,
				       id == UWE5622_CMD_ADDBA_REQ ? &rsp_len :
								     NULL,
				       NULL);
		/*
		 * The command succeeding only means the firmware answered. What
		 * the peer decided is in the answer, and a session that was
		 * declined or timed out has to be forgotten here or this TID is
		 * never asked for again.
		 */
		refused = !ret && rsp_len >= sizeof(rsp.result) &&
			  rsp.result != UWE5622_ADDBA_SUCCESS;
		if ((ret || refused) && id == UWE5622_CMD_ADDBA_REQ) {
			const struct uwe5622_cmd_addba *req =
				(const void *)skb->data;
			u8 tid = FIELD_GET(UWE5622_BA_PARAM_TID,
					   le16_to_cpu(req->param));

			if (refused)
				dev_dbg(wifi->dev,
					"peer %pM %s a block ack session for TID %u\n",
					req->address,
					uwe5622_addba_result_name(rsp.result),
					tid);

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
	struct net_device *ndev;
	struct sk_buff *skb, *tx;
	u8 *data;
	struct uwe5622_vif *vif;
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
		data = skb_push(tx, UWE5622_CMD_TX_MARKER);
		memcpy(data, "01234", UWE5622_CMD_TX_MARKER);
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
	int ret;

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

	ret = uwe5622_wifi_cmd(wifi, 0, UWE5622_CMD_SET_REGDOM, regdom,
			       struct_size(regdom, rules, rules),
			       NULL, NULL, NULL);
	/*
	 * A domain the firmware will not take is worth reporting, except for the
	 * world domain, which it refuses by design and which is what cfg80211
	 * asks for whenever it restores its default. Nothing is reported when
	 * the firmware was never asked, which is the case while it is parked for
	 * system sleep.
	 */
	if (ret && ret != -ESHUTDOWN) {
		if (request->alpha2[0] == '0' && request->alpha2[1] == '0')
			dev_dbg(wifi->dev,
				"firmware keeps its own regulatory domain\n");
		else
			dev_warn(wifi->dev,
				 "firmware rejected regulatory domain %c%c\n",
				 request->alpha2[0], request->alpha2[1]);
	}
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

/*
 * End an association that no firmware event will end, because the firmware that
 * held it is gone: either it was reloaded underneath, or the interface itself is
 * going away. cfg80211 keeps a reference to the BSS until it is told.
 */
static void uwe5622_report_disconnect(struct uwe5622_vif *vif)
{
	struct net_device *ndev = vif->wdev.netdev;

	if (!vif->connected)
		return;
	vif->connected = false;
	netif_carrier_off(ndev);
	cfg80211_disconnected(ndev, WLAN_REASON_DEAUTH_LEAVING, NULL, 0, true,
			      GFP_KERNEL);
}

/*
 * Everything a context owns that outlives its own code: two works that can be
 * scheduled from a firmware event, a poll instance that the netdev is about to
 * be freed with, and whatever the poll never got around to taking. Unregistering
 * the netdev frees it, and freeing a netdev whose poll is still attached warns,
 * so this has to run first on every teardown path.
 */
static void uwe5622_quiesce_vif(struct uwe5622_vif *vif)
{
	uwe5622_report_disconnect(vif);
	cancel_delayed_work_sync(&vif->connect_watchdog);
	cancel_work_sync(&vif->recover_work);
	vif->napi_ready = false;
	napi_disable(&vif->napi);
	netif_napi_del(&vif->napi);
	skb_queue_purge(&vif->rx_queue);
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
	skb_queue_head_init(&vif->rx_queue);
	INIT_WORK(&vif->recover_work, uwe5622_recover_work);
	INIT_DELAYED_WORK(&vif->connect_watchdog, uwe5622_connect_watchdog);
	vif->wdev.wiphy = wiphy;
	vif->wdev.iftype = type;
	vif->wdev.netdev = ndev;
	ndev->ieee80211_ptr = &vif->wdev;
	ndev->netdev_ops = &uwe5622_netdev_ops;
	ndev->features |= NETIF_F_RXCSUM | NETIF_F_IP_CSUM | NETIF_F_IPV6_CSUM;
	ndev->hw_features |= NETIF_F_RXCSUM | NETIF_F_IP_CSUM | NETIF_F_IPV6_CSUM;
	ndev->needed_headroom = UWE5622_TX_HEADROOM;
	ndev->needs_free_netdev = true;
	SET_NETDEV_DEV(ndev, wiphy_dev(wiphy));
	uwe5622_set_vif_type(vif, type, address);
	eth_hw_addr_set(ndev, address);
	strscpy(ndev->name, name, IFNAMSIZ);
	ndev->name_assign_type = name_assign_type;

	ret = uwe5622_open_firmware(vif);
	if (ret)
		goto err_free;
	netif_napi_add(ndev, &vif->napi, uwe5622_napi_poll);
	ret = cfg80211_register_netdevice(ndev);
	if (ret)
		goto err_napi;
	napi_enable(&vif->napi);
	/*
	 * Run the poll in its own thread. Frames are handed over from the
	 * transport's read loop, a kernel thread that holds the bus claimed for
	 * the whole drain, and a receive softirq raised there is never
	 * serviced: the poll simply never runs and reception stops. A threaded
	 * poll does not depend on that.
	 */
	dev_set_threaded(ndev, NETDEV_NAPI_THREADED_ENABLED);
	vif->napi_ready = true;

	ret = uwe5622_remember_vif(vif);
	if (ret)
		goto err_unregister;

	return &vif->wdev;

err_unregister:
	/* Unregistering frees the netdev, so it must not be freed again here. */
	uwe5622_quiesce_vif(vif);
	uwe5622_close_firmware(vif);
	cfg80211_unregister_netdevice(ndev);
	return ERR_PTR(ret);
err_napi:
	netif_napi_del(&vif->napi);
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
	uwe5622_quiesce_vif(vif);
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
	ret = uwe5622_wifi_cmd(vif->wifi, vif->ctx_id, UWE5622_CMD_CONNECT,
			       &connect, sizeof(connect), NULL, NULL, NULL);
	if (!ret)
		mod_delayed_work(system_wq, &vif->connect_watchdog,
				 UWE5622_CONNECT_TIMEOUT);

	return ret;
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

/*
 * Which management frames may be sent and asked for. The controller answers
 * authentication and association itself, so those are left out: the stack is
 * offered the frames it can act on without taking that over.
 */
static const struct ieee80211_txrx_stypes
uwe5622_mgmt_stypes[NUM_NL80211_IFTYPES] = {
	[NL80211_IFTYPE_STATION] = {
		.tx = BIT(IEEE80211_STYPE_ACTION >> 4),
		.rx = BIT(IEEE80211_STYPE_ACTION >> 4),
	},
	[NL80211_IFTYPE_AP] = {
		.tx = BIT(IEEE80211_STYPE_ACTION >> 4) |
		      BIT(IEEE80211_STYPE_PROBE_RESP >> 4),
		.rx = BIT(IEEE80211_STYPE_ACTION >> 4) |
		      BIT(IEEE80211_STYPE_PROBE_REQ >> 4),
	},
};

/*
 * Whether a beacon body already carries an element, walking it from the first
 * one after the fixed fields. A malformed tail simply ends the walk.
 */
static bool uwe5622_beacon_has_element(const u8 *frame, size_t len, u8 eid)
{
	size_t offset = offsetof(struct ieee80211_mgmt, u.beacon.variable);

	while (offset + 2 <= len) {
		u8 elen = frame[offset + 1];

		if (frame[offset] == eid)
			return true;
		if (offset + 2 + elen > len)
			break;
		offset += 2 + elen;
	}

	return false;
}

static int uwe5622_start_ap(struct wiphy *wiphy, struct net_device *ndev,
			    struct cfg80211_ap_settings *settings)
{
	struct uwe5622_vif *vif = netdev_priv(ndev);
	const struct cfg80211_beacon_data *beacon = &settings->beacon;
	u8 ds_params[] = { WLAN_EID_DS_PARAMS, 1, 0 };
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
	data = kzalloc(2 + len + sizeof(ds_params), GFP_KERNEL);
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
	/*
	 * The firmware takes the channel it beacons on from the beacon itself,
	 * and refuses one it cannot find a channel in. The element that carries
	 * it is defined for the 2.4 GHz band only, so nothing above channel 14
	 * arrives with one and every such access point is refused. Supply it
	 * when it is absent, which costs three bytes in a band that ignores it.
	 */
	if (!uwe5622_beacon_has_element(frame, len, WLAN_EID_DS_PARAMS)) {
		ds_params[2] = channel;
		memcpy(frame + len, ds_params, sizeof(ds_params));
		len += sizeof(ds_params);
		put_unaligned_le16(len, data);
	}
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
	ret = uwe5622_set_ie(vif, 4, beacon->assocresp_ies,
			     beacon->assocresp_ies_len);
	if (ret)
		return ret;
	/*
	 * The elements above are what the firmware adds to frames it builds
	 * itself. A beacon body that has changed has to be handed over as a
	 * body, or the access point keeps beaconing the one it started with.
	 */
	if (!beacon->tail)
		return 0;

	return uwe5622_wifi_cmd(vif->wifi, vif->ctx_id,
				UWE5622_CMD_RESET_BEACON,
				beacon->tail, beacon->tail_len,
				NULL, NULL, NULL);
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

/* Firmware list subcommands, shared by both access-control lists. */
#define UWE5622_ACL_ADD		3
#define UWE5622_ACL_FLUSH	5
#define UWE5622_ACL_ENABLE	7
#define UWE5622_ACL_DISABLE	8

struct uwe5622_cmd_acl {
	u8 subtype;
	u8 count;
	u8 mac[][ETH_ALEN];
} __packed;

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

static int uwe5622_fill_station(struct uwe5622_vif *vif,
			       struct net_device *ndev,
			       struct station_info *sinfo)
{
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

static int uwe5622_get_station(struct wiphy *wiphy, struct wireless_dev *wdev,
			       const u8 *mac, struct station_info *sinfo)
{
	return uwe5622_fill_station(uwe5622_vif_from_wdev(wdev), wdev->netdev,
				    sinfo);
}

/*
 * The stations an access point is holding, in the order the firmware handed
 * their lookup entries over. There is one report for the interface rather than
 * one per station, so every station carries the same link description; the
 * addresses are what this is for.
 */
static int uwe5622_dump_station(struct wiphy *wiphy, struct wireless_dev *wdev,
				int idx, u8 *mac, struct station_info *sinfo)
{
	struct uwe5622_vif *vif = uwe5622_vif_from_wdev(wdev);
	struct net_device *ndev = wdev->netdev;
	struct uwe5622_wifi *wifi = vif->wifi;
	int found = -1;
	unsigned int i;

	spin_lock_bh(&wifi->vif_lock);
	for (i = 0; i < ARRAY_SIZE(wifi->peers); i++) {
		if (!wifi->peers[i].valid ||
		    wifi->peers[i].ctx_id != vif->ctx_id)
			continue;
		if (++found != idx)
			continue;
		ether_addr_copy(mac, wifi->peers[i].address);
		break;
	}
	spin_unlock_bh(&wifi->vif_lock);
	if (i == ARRAY_SIZE(wifi->peers))
		return -ENOENT;

	return uwe5622_fill_station(vif, ndev, sinfo);
}

/*
 * Access control is two firmware lists. The deny list is added to and flushed;
 * the accept list is switched on with its members and switched off again. An
 * empty request means cfg80211 is turning access control off.
 */
static int uwe5622_acl_cmd(struct uwe5622_vif *vif, u8 id, u8 subtype,
			   const struct cfg80211_acl_data *acl)
{
	unsigned int n = acl ? acl->n_acl_entries : 0;
	struct uwe5622_cmd_acl *cmd;
	unsigned int i;
	size_t len;
	int ret;

	len = struct_size(cmd, mac, n);
	cmd = kzalloc(len, GFP_KERNEL);
	if (!cmd)
		return -ENOMEM;
	cmd->subtype = subtype;
	cmd->count = n;
	for (i = 0; i < n; i++)
		ether_addr_copy(cmd->mac[i], acl->mac_addrs[i].addr);
	ret = uwe5622_wifi_cmd(vif->wifi, vif->ctx_id, id, cmd, len,
			       NULL, NULL, NULL);
	kfree(cmd);

	return ret;
}

static int uwe5622_set_mac_acl(struct wiphy *wiphy, struct net_device *ndev,
			       const struct cfg80211_acl_data *acl)
{
	struct uwe5622_vif *vif = netdev_priv(ndev);
	struct uwe5622_wifi *wifi = vif->wifi;

	if (acl && acl->n_acl_entries > wifi->max_acl)
		return -ENOSPC;

	if (!acl || !acl->n_acl_entries ||
	    acl->acl_policy == NL80211_ACL_POLICY_ACCEPT_UNLESS_LISTED) {
		int ret;

		/* Leaving the accept list on would deny everything else. */
		ret = uwe5622_acl_cmd(vif, UWE5622_CMD_SOFTAP_WHITELIST,
				      UWE5622_ACL_DISABLE, NULL);
		if (ret)
			return ret;
		if (!acl || !acl->n_acl_entries)
			return uwe5622_acl_cmd(vif,
					       UWE5622_CMD_SOFTAP_BLACKLIST,
					       UWE5622_ACL_FLUSH, NULL);

		return uwe5622_acl_cmd(vif, UWE5622_CMD_SOFTAP_BLACKLIST,
				       UWE5622_ACL_ADD, acl);
	}

	return uwe5622_acl_cmd(vif, UWE5622_CMD_SOFTAP_WHITELIST,
			       UWE5622_ACL_ENABLE, acl);
}

/*
 * Send a management frame the stack built itself. The controller answers for the
 * transmission, so a failure is reported as a frame that was not acknowledged
 * rather than left for the caller to guess at.
 */
static int uwe5622_mgmt_tx(struct wiphy *wiphy, struct wireless_dev *wdev,
			   struct cfg80211_mgmt_tx_params *params, u64 *cookie)
{
	struct uwe5622_vif *vif = uwe5622_vif_from_wdev(wdev);
	struct uwe5622_wifi *wifi = vif->wifi;
	struct uwe5622_cmd_mgmt_tx *cmd;
	size_t len;
	int ret;

	if (!params->len)
		return -EINVAL;

	*cookie = atomic64_inc_return(&wifi->mgmt_cookie);
	len = struct_size(cmd, frame, params->len);
	cmd = kzalloc(len, GFP_KERNEL);
	if (!cmd)
		return -ENOMEM;
	if (params->chan) {
		int freq = params->chan->center_freq;

		cmd->channel = ieee80211_frequency_to_channel(freq);
	}
	cmd->dont_wait_for_ack = params->dont_wait_for_ack;
	cmd->wait = cpu_to_le32(params->wait);
	cmd->cookie = cpu_to_le64(*cookie);
	cmd->len = cpu_to_le16(params->len);
	memcpy(cmd->frame, params->buf, params->len);
	ret = uwe5622_wifi_cmd(wifi, vif->ctx_id, UWE5622_CMD_TX_MGMT, cmd,
			       len, NULL, NULL, NULL);
	kfree(cmd);
	if (!params->dont_wait_for_ack)
		cfg80211_mgmt_tx_status(wdev, *cookie, params->buf, params->len,
					!ret, GFP_KERNEL);

	return ret;
}

/*
 * Which management frames the stack wants to see. The controller takes one
 * subtype at a time, so the difference against what it was already told is what
 * gets sent.
 */
static void uwe5622_update_mgmt_frame_registrations(struct wiphy *wiphy,
						    struct wireless_dev *wdev,
						    struct mgmt_frame_regs *upd)
{
	struct uwe5622_vif *vif = uwe5622_vif_from_wdev(wdev);
	u32 changed = upd->interface_stypes ^ vif->mgmt_regs;
	unsigned int subtype;

	for (subtype = 0; subtype < 16; subtype++) {
		struct uwe5622_cmd_register_frame cmd = {
			.type = cpu_to_le16(subtype),
			.reg = !!(upd->interface_stypes & BIT(subtype)),
		};

		if (!(changed & BIT(subtype)))
			continue;
		if (uwe5622_wifi_cmd(vif->wifi, vif->ctx_id,
				     UWE5622_CMD_REGISTER_FRAME, &cmd,
				     sizeof(cmd), NULL, NULL, NULL))
			continue;
		vif->mgmt_regs ^= BIT(subtype);
	}
}

static int uwe5622_set_power_mgmt(struct wiphy *wiphy, struct net_device *ndev,
				  bool enabled, int timeout)
{
	struct uwe5622_vif *vif = netdev_priv(ndev);
	u8 data[] = { 4, enabled };

	return uwe5622_wifi_cmd(vif->wifi, vif->ctx_id, UWE5622_CMD_POWER_SAVE,
				data, sizeof(data), NULL, NULL, NULL);
}

/*
 * Whether the firmware is parked for system sleep, kept under the command mutex
 * so it cannot change while a command is being decided on.
 */
static void uwe5622_set_parked(struct uwe5622_wifi *wifi, bool parked)
{
	mutex_lock(&wifi->cmd_mutex);
	wifi->parked = parked;
	mutex_unlock(&wifi->cmd_mutex);
}

/*
 * Which frames the controller should wake the system for. The subtypes come from
 * the firmware's own numbering: zero is its default, which wakes for anything it
 * would normally hand up, and is also how a controller is told to forget an
 * earlier request.
 */
#define UWE5622_WOWLAN_ANY		0
#define UWE5622_WOWLAN_MAGIC_PKT	1
#define UWE5622_WOWLAN_DISCONNECT	2

/*
 * How long resume waits for the object the controller marked. It cannot deliver
 * that object until the transport is reading again, so reporting from resume
 * itself would race the record and call every wake unknown.
 */
#define UWE5622_WOWLAN_REPORT_DELAY	msecs_to_jiffies(500)

static int uwe5622_set_wowlan(struct uwe5622_wifi *wifi, u8 subtype)
{
	u8 cmd[2] = { subtype, 0 };

	return uwe5622_wifi_cmd(wifi, 0, UWE5622_CMD_SET_WOWLAN, cmd,
				sizeof(cmd), NULL, NULL, NULL);
}

/*
 * Program what this sleep asked for, not what the last change to the
 * configuration asked for: cfg80211 hands a wake configuration to suspend every
 * time, while it announces one through set_wakeup() only when wake-up as a whole
 * is switched on or off. A trigger set edited while it was already enabled never
 * reaches that callback at all.
 */
static int uwe5622_arm_wowlan(struct uwe5622_wifi *wifi,
			      struct cfg80211_wowlan *wowlan)
{
	struct uwe5622_wowlan armed = {};
	unsigned long flags;
	int ret;

	cancel_delayed_work_sync(&wifi->wowlan_work);

	/*
	 * Start from the controller's default so a trigger dropped since the last
	 * sleep is dropped here too, then add what was asked for.
	 */
	ret = uwe5622_set_wowlan(wifi, UWE5622_WOWLAN_ANY);
	if (ret || !wowlan)
		goto out;

	if (wowlan->any) {
		/*
		 * The default already wakes for anything the controller would
		 * hand up, so there is nothing more to program; what it wakes
		 * for still has to be remembered to be able to report it.
		 */
		armed.any = true;
	} else {
		if (wowlan->magic_pkt) {
			ret = uwe5622_set_wowlan(wifi,
						 UWE5622_WOWLAN_MAGIC_PKT);
			if (ret)
				goto out;
			armed.magic = true;
		}
		if (wowlan->disconnect) {
			ret = uwe5622_set_wowlan(wifi,
						 UWE5622_WOWLAN_DISCONNECT);
			if (ret)
				goto out;
			armed.disconnect = true;
		}
		if (!armed.magic && !armed.disconnect)
			goto out;
	}
	armed.armed = true;
out:
	spin_lock_irqsave(&wifi->wowlan_lock, flags);
	wifi->wowlan = armed;
	spin_unlock_irqrestore(&wifi->wowlan_lock, flags);

	return ret;
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

	/* Both of these have to reach a controller that is still answering. */
	ret = uwe5622_arm_wowlan(wifi, wowlan);
	if (ret) {
		dev_err(wifi->dev, "failed to arm wake-up triggers: %d\n", ret);
		goto out;
	}

	ret = uwe5622_wifi_cmd(wifi, vif->ctx_id, UWE5622_CMD_POWER_SAVE,
			       data, sizeof(data), NULL, NULL, NULL);
	if (!ret) {
		uwe5622_set_parked(wifi, true);
		netif_device_detach(ndev);
	}
out:
	dev_put(ndev);

	return ret;
}

/*
 * Say what woke the system, once the receive path has had its chance to deliver
 * the object the controller marked. A marked disconnect event and a marked frame
 * carrying the magic pattern are exact reasons; a marked frame that matched no
 * trigger of its own is still the packet that did it and is reported as such.
 * Nothing marked means the system woke for something other than this controller,
 * which is reported as a wake whose cause is not known.
 */
static void uwe5622_wowlan_report(struct work_struct *work)
{
	struct uwe5622_wifi *wifi = container_of(to_delayed_work(work),
						 struct uwe5622_wifi,
						 wowlan_work);
	struct cfg80211_wowlan_wakeup wakeup = {
		/* Anything but negative claims a matched packet pattern. */
		.pattern_idx = -1,
	};
	struct net_device *ndev = uwe5622_first_ndev(wifi);
	struct uwe5622_wowlan woke;
	unsigned long flags;

	if (!ndev)
		return;

	spin_lock_irqsave(&wifi->wowlan_lock, flags);
	woke = wifi->wowlan;
	wifi->wowlan.armed = false;
	spin_unlock_irqrestore(&wifi->wowlan_lock, flags);

	if (!woke.armed)
		goto out;

	if (!woke.seen) {
		/*
		 * Nothing was marked. If the controller pulled the host out of
		 * its sleep itself then this was still a wake-up it caused, and
		 * when a magic packet was the only thing it was watching for
		 * then a magic packet is what it found. The matching frame is
		 * not always handed up afterwards, and nothing else can pull the
		 * host out while that is the only trigger armed.
		 */
		if (!uwe5622_woke_host(wifi->cmd_client)) {
			cfg80211_report_wowlan_wakeup(ndev->ieee80211_ptr, NULL,
						      GFP_KERNEL);
			goto out;
		}
		wakeup.magic_pkt = woke.magic && !woke.any && !woke.disconnect;
		cfg80211_report_wowlan_wakeup(ndev->ieee80211_ptr, &wakeup,
					      GFP_KERNEL);
		goto out;
	}

	wakeup.disconnect = woke.woke_disconnect;
	wakeup.magic_pkt = woke.woke_magic;
	if (woke.packet_len) {
		wakeup.packet = woke.packet;
		wakeup.packet_len = woke.packet_present;
		wakeup.packet_present_len = woke.packet_len;
	}
	cfg80211_report_wowlan_wakeup(ndev->ieee80211_ptr, &wakeup, GFP_KERNEL);
out:
	dev_put(ndev);
}

static int uwe5622_wifi_resume(struct wiphy *wiphy)
{
	struct uwe5622_wifi *wifi = wiphy_priv(wiphy);
	struct net_device *ndev = uwe5622_first_ndev(wifi);
	struct uwe5622_vif *vif;
	u8 data[] = { 5, 1 };
	int ret;

	/*
	 * Before anything else, and whether or not there is an interface left to
	 * wake: a flag still set here refuses every later command, which
	 * userspace sees as a device that has stopped answering entirely.
	 */
	uwe5622_set_parked(wifi, false);
	if (!ndev)
		return 0;
	vif = netdev_priv(ndev);
	ret = uwe5622_wifi_cmd(wifi, vif->ctx_id, UWE5622_CMD_POWER_SAVE,
			       data, sizeof(data), NULL, NULL, NULL);
	/*
	 * Attach either way. A controller that did not answer is better handed to
	 * a stack that can time out, disconnect and try again than left with an
	 * interface nothing can be sent through.
	 */
	if (ret)
		dev_warn(wifi->dev, "controller did not wake cleanly: %d\n",
			 ret);
	netif_device_attach(ndev);
	dev_put(ndev);

	schedule_delayed_work(&wifi->wowlan_work, UWE5622_WOWLAN_REPORT_DELAY);

	return 0;
}

#ifdef CONFIG_PM
/*
 * Only the physical wake source belongs here. What the controller should wake
 * for is programmed at suspend, from the configuration that suspend is handed.
 */
static void uwe5622_set_wakeup(struct wiphy *wiphy, bool enabled)
{
	struct uwe5622_wifi *wifi = wiphy_priv(wiphy);

	uwe5622_set_wake(wifi->cmd_client, enabled);
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
	.dump_station = uwe5622_dump_station,
	.set_mac_acl = uwe5622_set_mac_acl,
	.mgmt_tx = uwe5622_mgmt_tx,
	.update_mgmt_frame_registrations =
		uwe5622_update_mgmt_frame_registrations,
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

	if (len < sizeof(*frame) || frame->type != UWE5622_FRAME_SCAN)
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
	cancel_delayed_work(&vif->connect_watchdog);
	if (len < 1)
		goto out;
	if (data[0] != 0 && data[0] != 2) {
		if (len >= 3)
			status = data[2];
		cfg80211_connect_result(ndev, NULL, NULL, 0, NULL, 0,
					status, GFP_ATOMIC);
		/* Not from here: this arrives in atomic context. */
		schedule_work(&vif->recover_work);
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

/*
 * A management frame the stack asked to see, or one the controller reports
 * because it ended an association. Both are handed over as received frames; the
 * stack decides what they mean.
 */
static void uwe5622_event_mgmt_frame(struct uwe5622_wifi *wifi, u8 ctx,
				     const u8 *data, size_t len)
{
	const struct uwe5622_event_mgmt_frame *frame = (const void *)data;
	struct net_device *ndev;
	u16 frame_len;
	int freq;

	if (len < sizeof(*frame))
		return;
	if (frame->type == UWE5622_FRAME_SCAN) {
		uwe5622_event_scan_frame(wifi, data, len);
		return;
	}
	if (frame->type != UWE5622_FRAME_REGISTERED &&
	    frame->type != UWE5622_FRAME_DEAUTH &&
	    frame->type != UWE5622_FRAME_DISASSOC)
		return;
	frame_len = le16_to_cpu(frame->len);
	if (frame_len > len - sizeof(*frame))
		return;
	ndev = uwe5622_get_ndev(wifi, ctx);
	if (!ndev)
		return;
	freq = ieee80211_channel_to_frequency(frame->channel,
					      frame->channel <= 14 ?
					      NL80211_BAND_2GHZ :
					      NL80211_BAND_5GHZ);
	cfg80211_rx_mgmt(ndev->ieee80211_ptr, freq, frame->signal, frame->data,
			 frame_len, 0);
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
		uwe5622_event_mgmt_frame(wifi, ctx, data, len);
		break;
	case UWE5622_EVENT_COEX_BT_ON_OFF:
		dev_dbg(wifi->dev, "firmware reports Bluetooth %s\n",
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

/*
 * The controller marks the command, event or received frame it built while
 * asleep, and that object is the account of what woke the system. The first
 * marked one is kept for the sleep it belongs to; there can be several, and
 * only the first is the cause.
 */
static bool uwe5622_wowlan_take(struct uwe5622_wifi *wifi)
	__must_hold(&wifi->wowlan_lock)
{
	return wifi->wowlan.armed && !wifi->wowlan.seen;
}

void uwe5622_wowlan_marked_event(struct uwe5622_wifi *wifi, u8 id)
{
	unsigned long flags;

	spin_lock_irqsave(&wifi->wowlan_lock, flags);
	if (uwe5622_wowlan_take(wifi) && id == UWE5622_EVENT_DISCONNECT &&
	    wifi->wowlan.disconnect) {
		wifi->wowlan.seen = true;
		wifi->wowlan.woke_disconnect = true;
	}
	spin_unlock_irqrestore(&wifi->wowlan_lock, flags);
}

/*
 * Six 0xff bytes and then one address sixteen times, anywhere in the frame.
 * Which address is a firmware matter: its matcher compares the associated
 * access point's own address rather than the interface's, so a frame that woke
 * the controller carries whichever of the two that firmware asks senders for.
 */
static bool uwe5622_is_magic_packet(const u8 *frame, u16 len, const u8 *addr)
{
	static const u8 sync[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	u16 need = sizeof(sync) + 16 * ETH_ALEN;
	u16 i, rep;

	if (len < ETH_HLEN + need)
		return false;

	/* Skip the destination address, which is six 0xff bytes on a broadcast. */
	for (i = ETH_ALEN; i + need <= len; i++) {
		if (memcmp(frame + i, sync, sizeof(sync)))
			continue;
		for (rep = 0; rep < 16; rep++)
			if (memcmp(frame + i + sizeof(sync) + rep * ETH_ALEN,
				   addr, ETH_ALEN))
				break;
		if (rep == 16)
			return true;
	}

	return false;
}

static void uwe5622_wowlan_marked_frame(struct uwe5622_wifi *wifi,
					struct net_device *ndev,
					const u8 *frame, u16 len)
{
	struct wireless_dev *wdev = ndev->ieee80211_ptr;
	unsigned long flags;
	bool magic;

	magic = uwe5622_is_magic_packet(frame, len, ndev->dev_addr);
	if (!magic && wdev->iftype == NL80211_IFTYPE_STATION)
		magic = uwe5622_is_magic_packet(frame, len,
						wdev->u.client.connected_addr);

	spin_lock_irqsave(&wifi->wowlan_lock, flags);
	if (uwe5622_wowlan_take(wifi)) {
		wifi->wowlan.seen = true;
		wifi->wowlan.woke_magic = wifi->wowlan.magic && magic;
		/*
		 * Keep the frame itself. It is the whole of the answer for the
		 * trigger that wakes for anything, and it is worth having
		 * alongside a magic packet as well.
		 */
		wifi->wowlan.packet_present = len;
		wifi->wowlan.packet_len = min_t(u16, len,
						UWE5622_WOWLAN_PACKET_MAX);
		memcpy(wifi->wowlan.packet, frame, wifi->wowlan.packet_len);
	}
	spin_unlock_irqrestore(&wifi->wowlan_lock, flags);
}

static void uwe5622_rx_one_frame(struct uwe5622_wifi *wifi,
				 const u8 *data, size_t len,
				 bool csum_present, u16 csum_raw)
{
	struct sk_buff_head done;
	struct net_device *ndev;
	struct sk_buff *skb;
	u16 frame_len;
	u8 ctx, offset;
	u32 word;

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
	if (unlikely(word & UWE5622_HOST_RESUME_MARK))
		uwe5622_wowlan_marked_frame(wifi, ndev, data + offset,
					    frame_len);
	if (csum_present)
		skb->ip_summed = uwe5622_rx_csum(data + offset, frame_len,
						 csum_raw);
	skb->protocol = eth_type_trans(skb, ndev);
	ndev->stats.rx_packets++;
	ndev->stats.rx_bytes += frame_len;

	__skb_queue_head_init(&done);
	__skb_queue_tail(&done, skb);

	uwe5622_deliver_vif(netdev_priv(ndev), &done);
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
		uwe5622_rx_one_frame(wifi, pos, left, skb->cb[4],
				     get_unaligned((u16 *)&skb->cb[2]));
		goto out;
	}

	while (count-- && left >= UWE5622_RX_DESC_LEN) {
		word = get_unaligned_le32(pos);
		offset = FIELD_GET(GENMASK(15, 8), word);
		frame_len = FIELD_GET(GENMASK(31, 16), word);
		if (offset < UWE5622_RX_DESC_LEN || frame_len > left - offset)
			break;
		uwe5622_rx_one_frame(wifi, pos, left, false, 0);
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

static int uwe5622_wifi_init_firmware(struct uwe5622_wifi *wifi);

/*
 * The controller was reloaded and its devices stayed where they were. Every
 * interface still exists and keeps its name, address and mode; what the new
 * firmware has never heard of is its contexts, its peers, its block ack
 * sessions and its transmit credits. Rebuild exactly that, and tell userspace
 * the link is gone so it connects again on the interface it already has.
 */
static void uwe5622_wifi_restart(void *priv)
{
	struct uwe5622_wifi *wifi = priv;
	struct net_device *ndev[UWE5622_WIFI_MAX_CTX] = {};
	int i, ret;

	uwe5622_set_parked(wifi, false);
	for (i = 0; i < UWE5622_WIFI_MAX_CTX; i++)
		ndev[i] = uwe5622_get_ndev(wifi, i);

	spin_lock_bh(&wifi->vif_lock);
	memset(wifi->peers, 0, sizeof(wifi->peers));
	memset(wifi->vifs, 0, sizeof(wifi->vifs));
	spin_unlock_bh(&wifi->vif_lock);

	spin_lock_bh(&wifi->credit_lock);
	memset(wifi->tx_credits, 0, sizeof(wifi->tx_credits));
	memset(wifi->credit_owner, 0, sizeof(wifi->credit_owner));
	spin_unlock_bh(&wifi->credit_lock);

	ret = uwe5622_wifi_init_firmware(wifi);
	if (ret) {
		dev_err(wifi->dev,
			"failed to configure the reloaded firmware: %d\n", ret);
		goto out;
	}

	for (i = 0; i < UWE5622_WIFI_MAX_CTX; i++) {
		struct uwe5622_vif *vif;

		if (!ndev[i])
			continue;
		vif = netdev_priv(ndev[i]);
		/* Its context went with the firmware; do not try to close it. */
		vif->opened = false;
		vif->credit_pool = UWE5622_CREDIT_NO_POOL;
		if (vif->mode == UWE5622_MODE_AP)
			cfg80211_stop_iface(wifi->wiphy, &vif->wdev,
					    GFP_KERNEL);
		else
			uwe5622_report_disconnect(vif);

		ret = uwe5622_open_firmware(vif);
		if (ret) {
			dev_err(wifi->dev,
				"failed to reopen %s after recovery: %d\n",
				ndev[i]->name, ret);
			continue;
		}
		if (uwe5622_remember_vif(vif))
			continue;
		netif_device_attach(ndev[i]);
	}
out:
	for (i = 0; i < UWE5622_WIFI_MAX_CTX; i++)
		dev_put(ndev[i]);
}

static const struct uwe5622_client_ops uwe5622_cmd_client_ops = {
	.rx = uwe5622_wifi_cmd_rx,
	.reset = uwe5622_wifi_cmd_reset,
	.restart = uwe5622_wifi_restart,
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
	wifi->max_ap_sta = info[20];
	wifi->max_acl = info[21];
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
	spin_lock_init(&wifi->wowlan_lock);
	INIT_DELAYED_WORK(&wifi->wowlan_work, uwe5622_wowlan_report);
	skb_queue_head_init(&wifi->eapol_queue);
	INIT_WORK(&wifi->eapol_work, uwe5622_eapol_work);
	skb_queue_head_init(&wifi->ba_queue);
	INIT_WORK(&wifi->ba_work, uwe5622_ba_work);
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
	/* What an access point may hold, as the firmware reported it. */
	wiphy->mgmt_stypes = uwe5622_mgmt_stypes;
	wiphy->max_ap_assoc_sta = wifi->max_ap_sta;
	wiphy->max_acl_mac_addrs = wifi->max_acl;
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
	cancel_delayed_work_sync(&wifi->wowlan_work);
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
		uwe5622_quiesce_vif(vif);
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
