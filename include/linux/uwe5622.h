/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_UWE5622_H
#define __LINUX_UWE5622_H

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
	/*
	 * The controller's debug trace ring, shared by its Wi-Fi, Bluetooth and
	 * coexistence code. It carries no HCI traffic: every HCI event,
	 * advertising reports among them, arrives on the Bluetooth channel.
	 */
	UWE5622_SERVICE_WCN_TRACE,
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
	void (*reset)(void *priv);
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
void uwe5622_recover(struct uwe5622_client *client);
void uwe5622_bluetooth_enable(struct uwe5622_client *client, bool enabled);
int uwe5622_bluetooth_ram(struct uwe5622_client *client, bool on);
void uwe5622_bluetooth_wake(struct uwe5622_client *client, bool enabled);
enum uwe5622_bus_type uwe5622_client_bus(struct uwe5622_client *client);

#endif
