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
 */
struct uwe5622_client_ops {
	void (*rx)(void *priv, struct sk_buff *skb);
	void (*reset)(void *priv);
};

struct uwe5622_client *
uwe5622_client_register(struct device *dev, enum uwe5622_service service,
			const struct uwe5622_client_ops *ops, void *priv);
void uwe5622_client_unregister(struct uwe5622_client *client);
/* The caller retains ownership of @skb, including after a successful send. */
int uwe5622_client_send(struct uwe5622_client *client, struct sk_buff *skb);

int uwe5622_power_get(struct uwe5622_client *client);
void uwe5622_power_put(struct uwe5622_client *client);
void uwe5622_set_wake(struct uwe5622_client *client, bool enabled);
void uwe5622_bluetooth_enable(struct uwe5622_client *client, bool enabled);
void uwe5622_bluetooth_wake(struct uwe5622_client *client, bool enabled);
enum uwe5622_bus_type uwe5622_client_bus(struct uwe5622_client *client);

#endif
