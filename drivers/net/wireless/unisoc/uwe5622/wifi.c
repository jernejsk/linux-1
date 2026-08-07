// SPDX-License-Identifier: GPL-2.0-only

#include <linux/auxiliary_bus.h>
#include <linux/crc16.h>
#include <linux/etherdevice.h>
#include <linux/firmware.h>
#include <linux/ieee80211.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/unaligned.h>
#include <net/cfg80211.h>

#include "wifi.h"

#define UWE5622_VALID_CONFIG	BIT(7)
#define UWE5622_TX_DESC_LEN	11
#define UWE5622_RX_DESC_LEN	28
#define UWE5622_RX_MH_DESC_LEN	28
#define UWE5622_EAPOL_QUEUE_MAX	64
#define UWE5622_WIFI_CONFIG_NAME	"unisoc/uwe5622/wifi_config.bin"

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

static struct ieee80211_supported_band uwe5622_band_2ghz = {
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

static struct ieee80211_supported_band uwe5622_band_5ghz = {
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
};

static const u32 uwe5622_cipher_suites[] = {
	WLAN_CIPHER_SUITE_WEP40,
	WLAN_CIPHER_SUITE_WEP104,
	WLAN_CIPHER_SUITE_TKIP,
	WLAN_CIPHER_SUITE_CCMP,
	WLAN_CIPHER_SUITE_AES_CMAC,
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

static struct sk_buff *uwe5622_build_tx(struct uwe5622_vif *vif,
					const struct sk_buff *skb, u8 type)
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
	skb_put_data(tx, skb->data, skb->len);
	return tx;
}

static netdev_tx_t uwe5622_ndev_xmit(struct sk_buff *skb,
				     struct net_device *ndev)
{
	struct uwe5622_vif *vif = netdev_priv(ndev);
	struct uwe5622_wifi *wifi = vif->wifi;
	struct sk_buff *tx;
	int ret;

	if (unlikely(skb->protocol == htons(ETH_P_PAE))) {
		if (skb_queue_len(&wifi->eapol_queue) >= UWE5622_EAPOL_QUEUE_MAX)
			return NETDEV_TX_BUSY;
		UWE5622_SKB_CB(skb)->ctx_id = vif->ctx_id;
		skb_queue_tail(&wifi->eapol_queue, skb);
		schedule_work(&wifi->eapol_work);
		return NETDEV_TX_OK;
	}

	tx = uwe5622_build_tx(vif, skb, 2);
	if (!tx)
		goto drop;
	ret = uwe5622_client_send(wifi->data_client, tx);
	kfree_skb(tx);
	if (ret == -ENOBUFS)
		return NETDEV_TX_BUSY;
	if (ret)
		goto drop;

	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += skb->len;
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
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
		tx = uwe5622_build_tx(vif, skb, 0);
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
	uwe5622_wifi_cmd(vif->wifi, vif->ctx_id, UWE5622_CMD_CLOSE,
			 &mode, sizeof(mode), NULL, NULL, NULL);
	vif->opened = false;
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

	ndev = alloc_etherdev(sizeof(*vif));
	if (!ndev)
		return ERR_PTR(-ENOMEM);
	vif = netdev_priv(ndev);
	vif->wifi = wifi;
	vif->mode = type == NL80211_IFTYPE_AP ? UWE5622_MODE_AP :
						 UWE5622_MODE_STATION;
	vif->sta_lut = type == NL80211_IFTYPE_AP ? 4 : 0;
	vif->wdev.wiphy = wiphy;
	vif->wdev.iftype = type;
	vif->wdev.netdev = ndev;
	ndev->ieee80211_ptr = &vif->wdev;
	ndev->netdev_ops = &uwe5622_netdev_ops;
	ndev->needs_free_netdev = true;
	SET_NETDEV_DEV(ndev, wiphy_dev(wiphy));
	ether_addr_copy(address, wifi->perm_addr);
	if (type == NL80211_IFTYPE_AP) {
		address[0] |= 0x02;
		address[5] ^= 0x80;
	}
	eth_hw_addr_set(ndev, address);
	strscpy(ndev->name, name, IFNAMSIZ);
	ndev->name_assign_type = name_assign_type;

	ret = uwe5622_open_firmware(vif);
	if (ret)
		goto err_free;
	ret = register_netdevice(ndev);
	if (ret)
		goto err_close;

	spin_lock_bh(&wifi->vif_lock);
	if (wifi->vifs[vif->ctx_id]) {
		spin_unlock_bh(&wifi->vif_lock);
		ret = -EBUSY;
		goto err_unregister;
	}
	wifi->vifs[vif->ctx_id] = ndev;
	spin_unlock_bh(&wifi->vif_lock);
	return &vif->wdev;

err_unregister:
	uwe5622_close_firmware(vif);
	unregister_netdevice(ndev);
	return ERR_PTR(ret);
err_close:
	uwe5622_close_firmware(vif);
err_free:
	free_netdev(ndev);
	return ERR_PTR(ret);
}

static void uwe5622_finish_scan(struct uwe5622_wifi *wifi, bool aborted)
{
	struct cfg80211_scan_request *request;
	struct cfg80211_scan_info info = { .aborted = aborted };

	spin_lock_bh(&wifi->scan_lock);
	request = wifi->scan_request;
	wifi->scan_request = NULL;
	spin_unlock_bh(&wifi->scan_lock);
	if (request)
		cfg80211_scan_done(request, &info);
}

static int uwe5622_del_virtual_intf(struct wiphy *wiphy,
				    struct wireless_dev *wdev)
{
	struct uwe5622_wifi *wifi = wiphy_priv(wiphy);
	struct uwe5622_vif *vif = uwe5622_vif_from_wdev(wdev);
	struct net_device *ndev = wdev->netdev;

	spin_lock_bh(&wifi->vif_lock);
	if (wifi->vifs[vif->ctx_id] == ndev)
		wifi->vifs[vif->ctx_id] = NULL;
	spin_unlock_bh(&wifi->vif_lock);
	uwe5622_finish_scan(wifi, true);
	uwe5622_close_firmware(vif);
	unregister_netdevice(ndev);
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
	if (sme->crypto.n_akm_suites)
		connect.key_mgmt = uwe5622_akm(sme->crypto.akm_suites[0]) |
					 UWE5622_VALID_CONFIG;
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
	if (link_id)
		return -EINVAL;
	netif_carrier_off(ndev);
	return 0;
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

static int uwe5622_wifi_suspend(struct wiphy *wiphy,
				struct cfg80211_wowlan *wowlan)
{
	struct uwe5622_wifi *wifi = wiphy_priv(wiphy);
	struct net_device *ndev = uwe5622_first_ndev(wifi);
	struct uwe5622_vif *vif;
	u8 data[] = { 5, 0 };
	int ret;

	flush_work(&wifi->eapol_work);
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

static const struct cfg80211_ops uwe5622_cfg80211_ops = {
	.suspend = uwe5622_wifi_suspend,
	.resume = uwe5622_wifi_resume,
	.set_wakeup = uwe5622_set_wakeup,
	.add_virtual_intf = uwe5622_add_virtual_intf,
	.del_virtual_intf = uwe5622_del_virtual_intf,
	.scan = uwe5622_scan,
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
	case UWE5622_EVENT_NEW_STATION:
		uwe5622_event_new_station(wifi, ctx, data, len);
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
		break;
	default:
		dev_dbg(wifi->dev, "unhandled event %#x\n", hdr->id);
		break;
	}
}

static void uwe5622_rx_one_frame(struct uwe5622_wifi *wifi,
				 const u8 *data, size_t len)
{
	struct net_device *ndev;
	struct sk_buff *skb;
	u32 word;
	u16 frame_len;
	u8 ctx, offset;

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

static const struct uwe5622_client_ops uwe5622_data_client_ops = {
	.rx = uwe5622_wifi_data_rx,
	.reset = uwe5622_wifi_data_reset,
};

static int uwe5622_wifi_init_firmware(struct uwe5622_wifi *wifi)
{
	struct {
		__le32 main_version;
		u8 api[256];
	} version = { .main_version = cpu_to_le32(1) };
	u8 request[] = { 2, 0, 1, 0, 0 };
	u8 info[128] = {};
	size_t len = sizeof(info);
	static const u8 api_ids[] = {
		1, 3, 4, 5, 7, 9, 10, 11, 13, 14, 17, 18, 25, 72,
		76, 83, 0x80, 0x81, 0x82, 0x83, 0xa0, 0xf5, 0xf6,
	};
	const struct firmware *config;
	const u8 *section;
	u16 section_len[3];
	size_t config_len;
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
	if (config->size < 12 || memcmp(config->data, "UWEI", 4) ||
	    get_unaligned_le16(config->data + 4) != 1) {
		ret = -EINVAL;
		goto out_config;
	}
	for (i = 0; i < ARRAY_SIZE(section_len); i++)
		section_len[i] = get_unaligned_le16(config->data + 6 + 2 * i);
	config_len = 12 + section_len[0] + section_len[1] + section_len[2];
	if (!section_len[0] || !section_len[1] || config_len != config->size) {
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
	wifi->fw_capa = get_unaligned_le32(info + 16);
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
	mutex_init(&wifi->cmd_mutex);
	init_completion(&wifi->cmd_done);
	spin_lock_init(&wifi->vif_lock);
	spin_lock_init(&wifi->scan_lock);
	skb_queue_head_init(&wifi->eapol_queue);
	INIT_WORK(&wifi->eapol_work, uwe5622_eapol_work);
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
	wiphy->bands[NL80211_BAND_2GHZ] = &uwe5622_band_2ghz;
	if (wifi->fw_capa & UWE5622_GET_INFO_CAP_5G)
		wiphy->bands[NL80211_BAND_5GHZ] = &uwe5622_band_5ghz;
	wiphy->signal_type = CFG80211_SIGNAL_TYPE_MBM;
	wiphy->max_scan_ssids = 9;
	wiphy->max_scan_ie_len = 255;
	wiphy->cipher_suites = uwe5622_cipher_suites;
	wiphy->n_cipher_suites = ARRAY_SIZE(uwe5622_cipher_suites);
	wiphy->max_num_pmkids = 4;
	wiphy->wowlan = &uwe5622_wowlan_support;
	if (wifi->fw_capa & UWE5622_GET_INFO_CAP_AP_SME)
		wiphy->flags |= WIPHY_FLAG_HAVE_AP_SME;
	/* Do not set NL80211_EXT_FEATURE_4WAY_HANDSHAKE_STA_PSK. */
	ret = wiphy_register(wiphy);
	if (ret)
		goto err_power;
	auxiliary_set_drvdata(adev, wifi);
	dev_info(&adev->dev, "registered UWE5622 fullmac Wi-Fi\n");
	return 0;

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

	cancel_work_sync(&wifi->eapol_work);
	skb_queue_purge(&wifi->eapol_queue);
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
