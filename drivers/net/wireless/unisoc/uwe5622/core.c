// SPDX-License-Identifier: GPL-2.0-only

#include <linux/auxiliary_bus.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>

#include "core.h"

static void uwe5622_auxdev_release(struct device *dev)
{
	struct auxiliary_device *adev = to_auxiliary_dev(dev);
	struct uwe5622_auxdev *auxdev;

	auxdev = container_of(adev, struct uwe5622_auxdev, adev);
	kfree(auxdev);
}

static void uwe5622_auxdev_del(struct uwe5622_auxdev *auxdev)
{
	if (!auxdev)
		return;

	auxiliary_device_delete(&auxdev->adev);
	auxiliary_device_uninit(&auxdev->adev);
}

static struct uwe5622_auxdev *
uwe5622_auxdev_add(struct uwe5622 *wcn, const char *name)
{
	struct uwe5622_auxdev *auxdev;
	int ret;

	auxdev = kzalloc_obj(*auxdev);
	if (!auxdev)
		return ERR_PTR(-ENOMEM);

	auxdev->adev.name = name;
	auxdev->adev.id = 0;
	auxdev->adev.dev.parent = wcn->dev;
	auxdev->adev.dev.platform_data = wcn;
	auxdev->adev.dev.release = uwe5622_auxdev_release;
	device_set_of_node_from_dev(&auxdev->adev.dev, wcn->dev);

	ret = auxiliary_device_init(&auxdev->adev);
	if (ret) {
		of_node_put(auxdev->adev.dev.of_node);
		kfree(auxdev);
		return ERR_PTR(ret);
	}

	ret = auxiliary_device_add(&auxdev->adev);
	if (ret) {
		auxiliary_device_uninit(&auxdev->adev);
		return ERR_PTR(ret);
	}

	return auxdev;
}

struct uwe5622_client *
uwe5622_client_register(struct device *dev, enum uwe5622_service service,
			const struct uwe5622_client_ops *ops, void *priv)
{
	struct uwe5622 *wcn = dev_get_platdata(dev);
	struct uwe5622_client *client;
	u8 tx_channel, rx_channel;

	if (!wcn || !ops || !ops->rx || service >= UWE5622_SERVICE_COUNT)
		return ERR_PTR(-EINVAL);
	tx_channel = wcn->services[service].tx;
	rx_channel = wcn->services[service].rx;
	if (tx_channel >= UWE5622_MAX_CHANNELS ||
	    rx_channel >= UWE5622_MAX_CHANNELS)
		return ERR_PTR(-EINVAL);

	client = kzalloc_obj(*client);
	if (!client)
		return ERR_PTR(-ENOMEM);

	client->wcn = wcn;
	client->ops = ops;
	client->priv = priv;
	client->tx_channel = tx_channel;
	client->rx_channel = rx_channel;

	mutex_lock(&wcn->channel_mutex);
	if (rcu_access_pointer(wcn->channels[rx_channel])) {
		mutex_unlock(&wcn->channel_mutex);
		kfree(client);
		return ERR_PTR(-EBUSY);
	}
	rcu_assign_pointer(wcn->channels[rx_channel], client);
	mutex_unlock(&wcn->channel_mutex);

	return client;
}
EXPORT_SYMBOL_GPL(uwe5622_client_register);

void uwe5622_client_unregister(struct uwe5622_client *client)
{
	struct uwe5622 *wcn;

	if (!client)
		return;

	wcn = client->wcn;
	if (client->wake_enabled)
		uwe5622_set_wake(client, false);
	mutex_lock(&wcn->channel_mutex);
	if (rcu_access_pointer(wcn->channels[client->rx_channel]) == client)
		RCU_INIT_POINTER(wcn->channels[client->rx_channel], NULL);
	mutex_unlock(&wcn->channel_mutex);
	synchronize_srcu(&wcn->channel_srcu);
	kfree(client);
}
EXPORT_SYMBOL_GPL(uwe5622_client_unregister);

int uwe5622_client_send(struct uwe5622_client *client, struct sk_buff *skb)
{
	struct uwe5622 *wcn;

	if (!client || !skb)
		return -EINVAL;

	wcn = client->wcn;
	if (READ_ONCE(wcn->state) != UWE5622_READY)
		return -ESHUTDOWN;

	return wcn->bus_ops->tx(wcn, client->tx_channel, skb);
}
EXPORT_SYMBOL_GPL(uwe5622_client_send);

int uwe5622_power_get(struct uwe5622_client *client)
{
	struct uwe5622 *wcn;
	int ret = 0;

	if (!client)
		return -EINVAL;

	wcn = client->wcn;
	mutex_lock(&wcn->state_mutex);
	if (wcn->state != UWE5622_READY)
		ret = -ESHUTDOWN;
	else
		wcn->users++;
	mutex_unlock(&wcn->state_mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(uwe5622_power_get);

void uwe5622_power_put(struct uwe5622_client *client)
{
	struct uwe5622 *wcn;

	if (!client)
		return;

	wcn = client->wcn;
	mutex_lock(&wcn->state_mutex);
	if (WARN_ON(!wcn->users)) {
		mutex_unlock(&wcn->state_mutex);
		return;
	}
	wcn->users--;
	mutex_unlock(&wcn->state_mutex);
}
EXPORT_SYMBOL_GPL(uwe5622_power_put);

void uwe5622_set_wake(struct uwe5622_client *client, bool enabled)
{
	struct uwe5622 *wcn;
	bool any = false;
	int i;

	if (!client)
		return;
	wcn = client->wcn;
	mutex_lock(&wcn->channel_mutex);
	client->wake_enabled = enabled;
	for (i = 0; i < UWE5622_MAX_CHANNELS; i++) {
		struct uwe5622_client *other;

		other = rcu_dereference_protected(wcn->channels[i],
						 lockdep_is_held(&wcn->channel_mutex));
		if (other && other->wake_enabled) {
			any = true;
			break;
		}
	}
	WRITE_ONCE(wcn->wake_enabled, any);
	mutex_unlock(&wcn->channel_mutex);
}
EXPORT_SYMBOL_GPL(uwe5622_set_wake);

void uwe5622_bluetooth_enable(struct uwe5622_client *client, bool enabled)
{
	if (client && client->wcn->bluetooth_enable)
		gpiod_set_value_cansleep(client->wcn->bluetooth_enable, enabled);
}
EXPORT_SYMBOL_GPL(uwe5622_bluetooth_enable);

void uwe5622_bluetooth_wake(struct uwe5622_client *client, bool enabled)
{
	if (client && client->wcn->device_wake)
		gpiod_set_value_cansleep(client->wcn->device_wake, enabled);
}
EXPORT_SYMBOL_GPL(uwe5622_bluetooth_wake);

enum uwe5622_bus_type uwe5622_client_bus(struct uwe5622_client *client)
{
	return client ? client->wcn->bus_type : UWE5622_BUS_SDIO;
}
EXPORT_SYMBOL_GPL(uwe5622_client_bus);

void uwe5622_core_rx(struct uwe5622 *wcn, u8 channel, struct sk_buff *skb)
{
	struct uwe5622_client *client;
	int idx;

	if (channel >= UWE5622_MAX_CHANNELS) {
		dev_warn_ratelimited(wcn->dev, "invalid RX channel %u\n", channel);
		kfree_skb(skb);
		return;
	}

	idx = srcu_read_lock(&wcn->channel_srcu);
	client = srcu_dereference(wcn->channels[channel], &wcn->channel_srcu);
	if (client)
		client->ops->rx(client->priv, skb);
	else
		kfree_skb(skb);
	srcu_read_unlock(&wcn->channel_srcu, idx);
}

int uwe5622_core_probe(struct uwe5622 *wcn)
{
	const struct firmware *fw;
	int ret;

	mutex_init(&wcn->state_mutex);
	mutex_init(&wcn->channel_mutex);
	ret = init_srcu_struct(&wcn->channel_srcu);
	if (ret)
		return ret;
	wcn->state = UWE5622_BOOTING;

	ret = request_firmware(&fw, UWE5622_FIRMWARE_NAME, wcn->dev);
	if (ret)
		goto err_firmware;

	ret = wcn->bus_ops->start(wcn, fw);
	release_firmware(fw);
	if (ret)
		goto err_start;

	wcn->state = UWE5622_READY;
	wcn->wifi_auxdev = uwe5622_auxdev_add(wcn, "wifi");
	if (IS_ERR(wcn->wifi_auxdev)) {
		ret = PTR_ERR(wcn->wifi_auxdev);
		wcn->wifi_auxdev = NULL;
		goto err_stop;
	}

	wcn->bt_auxdev = uwe5622_auxdev_add(wcn, "bluetooth");
	if (IS_ERR(wcn->bt_auxdev)) {
		ret = PTR_ERR(wcn->bt_auxdev);
		wcn->bt_auxdev = NULL;
		goto err_del_wifi;
	}

	return 0;

err_del_wifi:
	uwe5622_auxdev_del(wcn->wifi_auxdev);
	wcn->wifi_auxdev = NULL;
err_stop:
	wcn->state = UWE5622_STOPPING;
	wcn->bus_ops->stop(wcn);
	wcn->state = UWE5622_OFF;
	cleanup_srcu_struct(&wcn->channel_srcu);
	return ret;

err_firmware:
	wcn->state = UWE5622_OFF;
	cleanup_srcu_struct(&wcn->channel_srcu);
	return dev_err_probe(wcn->dev, ret, "failed to load %s\n",
			     UWE5622_FIRMWARE_NAME);

err_start:
	wcn->state = UWE5622_OFF;
	cleanup_srcu_struct(&wcn->channel_srcu);
	return dev_err_probe(wcn->dev, ret, "failed to start WCN firmware\n");
}

void uwe5622_core_remove(struct uwe5622 *wcn)
{
	mutex_lock(&wcn->state_mutex);
	if (wcn->state == UWE5622_OFF) {
		mutex_unlock(&wcn->state_mutex);
		return;
	}
	mutex_unlock(&wcn->state_mutex);

	/* Children must still be able to send their firmware-close commands. */
	uwe5622_auxdev_del(wcn->bt_auxdev);
	uwe5622_auxdev_del(wcn->wifi_auxdev);
	wcn->bt_auxdev = NULL;
	wcn->wifi_auxdev = NULL;
	mutex_lock(&wcn->state_mutex);
	wcn->state = UWE5622_STOPPING;
	mutex_unlock(&wcn->state_mutex);
	if (wcn->device_wake)
		gpiod_set_value_cansleep(wcn->device_wake, 0);
	if (wcn->bluetooth_enable)
		gpiod_set_value_cansleep(wcn->bluetooth_enable, 0);
	wcn->bus_ops->stop(wcn);

	mutex_lock(&wcn->state_mutex);
	wcn->state = UWE5622_OFF;
	mutex_unlock(&wcn->state_mutex);
	cleanup_srcu_struct(&wcn->channel_srcu);
}

void uwe5622_core_shutdown(struct uwe5622 *wcn)
{
	uwe5622_core_remove(wcn);
}

int uwe5622_core_suspend(struct uwe5622 *wcn)
{
	int ret;

	mutex_lock(&wcn->state_mutex);
	if (wcn->state != UWE5622_READY) {
		ret = -EBUSY;
		goto out;
	}

	wcn->state = UWE5622_SUSPENDING;
	ret = wcn->bus_ops->suspend(wcn, wcn->wake_enabled);
	if (ret)
		wcn->state = UWE5622_READY;
	else
		wcn->state = UWE5622_SUSPENDED;
out:
	mutex_unlock(&wcn->state_mutex);
	return ret;
}

int uwe5622_core_resume(struct uwe5622 *wcn)
{
	int ret;

	mutex_lock(&wcn->state_mutex);
	if (wcn->state != UWE5622_SUSPENDED) {
		ret = -EINVAL;
		goto out;
	}

	ret = wcn->bus_ops->resume(wcn);
	if (!ret)
		wcn->state = UWE5622_READY;
	else
		wcn->state = UWE5622_RECOVERING;
out:
	mutex_unlock(&wcn->state_mutex);
	return ret;
}

MODULE_FIRMWARE(UWE5622_FIRMWARE_NAME);
MODULE_DESCRIPTION("Unisoc UWE5622 connectivity core");
MODULE_LICENSE("GPL");
