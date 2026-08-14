/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_UWE5622_H
#define __LINUX_UWE5622_H

#include <linux/bits.h>
#include <linux/types.h>

struct device;
struct sk_buff;
struct uwe5622;
struct uwe5622_client;

enum uwe5622_service {
	UWE5622_SERVICE_WIFI_COMMAND,
	UWE5622_SERVICE_WIFI_DATA,
	UWE5622_SERVICE_WIFI_LOG,
	UWE5622_SERVICE_BLUETOOTH,
	/* The controller's AT command interpreter, which controls its logging. */
	UWE5622_SERVICE_AT,
	UWE5622_SERVICE_COUNT,
};

enum uwe5622_bus_type {
	UWE5622_BUS_SDIO,
	UWE5622_BUS_USB,
	UWE5622_BUS_PCIE,
};

/**
 * struct uwe5622_client_ops - callbacks for one WCN logical channel pair
 * @rx: consume one complete payload; ownership of the skb is transferred
 * @reset: notify the client that firmware state was lost
 * @tx_error: a queued transfer was never handed to the firmware; @tag is the
 *	value passed to uwe5622_client_send_tagged()
 */
struct uwe5622_client_ops {
	void (*rx)(void *priv, struct sk_buff *skb);
	/* The firmware is about to go away; drop whatever describes it. */
	void (*reset)(void *priv);
	/*
	 * The firmware has been reloaded underneath, and the client's device is
	 * the same device it was: re-establish what the controller forgot. Runs
	 * in process context with the core ready to carry commands again.
	 */
	void (*restart)(void *priv);
	void (*tx_error)(void *priv, u8 tag);
};

struct uwe5622_client *
uwe5622_client_register(struct device *dev, enum uwe5622_service service,
			const struct uwe5622_client_ops *ops, void *priv);
void uwe5622_client_unregister(struct uwe5622_client *client);
/*
 * Hand @skb to the controller. The bus takes ownership of it when this returns
 * success and frees it once the transfer is done, which is what lets a payload
 * reach the controller without being copied; on failure the caller still owns
 * it. The bus puts its own header in front of the payload, so @skb needs
 * UWE5622_BUS_HEADROOM bytes of headroom or it is reallocated to get them.
 */
int uwe5622_client_send(struct uwe5622_client *client, struct sk_buff *skb);
/* Room for the header the bus puts in front of every payload. */
#define UWE5622_BUS_HEADROOM	4

/*
 * As uwe5622_client_send(), but @tag is handed back through
 * uwe5622_client_ops::tx_error if the transfer fails on the bus after this
 * call has already returned success.
 */
int uwe5622_client_send_tagged(struct uwe5622_client *client,
			       struct sk_buff *skb, u8 tag);

int uwe5622_power_get(struct uwe5622_client *client);
void uwe5622_power_put(struct uwe5622_client *client);
void uwe5622_set_wake(struct uwe5622_client *client, bool enabled);
/*
 * Whether the controller pulled the host out of its sleep itself. Answers for
 * the sleep that has just ended, and only until the next one is armed.
 */
/*
 * The header every command, response and event on the Wi-Fi command channel
 * carries. It is shared because the controller's own configuration is uploaded
 * over that channel before either of the devices it offers exists.
 */
struct uwe5622_cmd_hdr {
	u8 common;
	u8 id;
	__le16 plen;
	__le32 token;
	s8 status;
	u8 rsp_count;
	u8 reserved[2];
} __packed;

#define UWE5622_HEAD_TYPE_MASK	GENMASK(2, 0)
#define UWE5622_HEAD_RSP	BIT(4)
#define UWE5622_HEAD_CTX_MASK	GENMASK(7, 5)
#define UWE5622_HEAD_TYPE_CMD	0
#define UWE5622_HEAD_TYPE_EVENT	1

bool uwe5622_woke_host(struct uwe5622_client *client);
void uwe5622_recover(struct uwe5622_client *client);
void uwe5622_bluetooth_enable(struct uwe5622_client *client, bool enabled);
int uwe5622_bluetooth_ram(struct uwe5622_client *client, bool on);
void uwe5622_bluetooth_wake(struct uwe5622_client *client, bool enabled);
enum uwe5622_bus_type uwe5622_client_bus(struct uwe5622_client *client);

#endif
