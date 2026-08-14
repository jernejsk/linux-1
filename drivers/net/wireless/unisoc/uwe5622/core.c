// SPDX-License-Identifier: GPL-2.0-only

#include <linux/auxiliary_bus.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/firmware.h>
#include <linux/module.h>
#include <crypto/sha2.h>
#include <linux/crc16.h>
#include <linux/unaligned.h>
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

int uwe5622_client_send_tagged(struct uwe5622_client *client,
			       struct sk_buff *skb, u8 tag)
{
	struct uwe5622 *wcn;

	if (!client || !skb)
		return -EINVAL;

	wcn = client->wcn;
	if (READ_ONCE(wcn->state) != UWE5622_READY)
		return -ESHUTDOWN;

	return wcn->bus_ops->tx(wcn, client->tx_channel, skb, tag);
}
EXPORT_SYMBOL_GPL(uwe5622_client_send_tagged);

int uwe5622_client_send(struct uwe5622_client *client, struct sk_buff *skb)
{
	return uwe5622_client_send_tagged(client, skb, 0);
}
EXPORT_SYMBOL_GPL(uwe5622_client_send);

/*
 * Report a transfer that the bus accepted but never delivered. The owning
 * client is found by transmit channel so it can undo whatever it committed when
 * the transfer was queued.
 */
void uwe5622_core_tx_error(struct uwe5622 *wcn, u8 tx_channel, u8 tag)
{
	struct uwe5622_client *client;
	int idx, i;

	idx = srcu_read_lock(&wcn->channel_srcu);
	for (i = 0; i < UWE5622_MAX_CHANNELS; i++) {
		client = srcu_dereference(wcn->channels[i], &wcn->channel_srcu);
		if (!client || client->tx_channel != tx_channel)
			continue;
		if (client->ops->tx_error)
			client->ops->tx_error(client->priv, tag);
		break;
	}
	srcu_read_unlock(&wcn->channel_srcu, idx);
}

void uwe5622_core_request_recovery(struct uwe5622 *wcn)
{
	mutex_lock(&wcn->state_mutex);
	if (wcn->state == UWE5622_READY && !wcn->removing) {
		wcn->state = UWE5622_RECOVERING;
		schedule_work(&wcn->recovery_work);
	}
	mutex_unlock(&wcn->state_mutex);
}

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

bool uwe5622_woke_host(struct uwe5622_client *client)
{
	struct uwe5622 *wcn;

	if (!client)
		return false;
	wcn = client->wcn;

	return wcn->bus_ops->woke_host && wcn->bus_ops->woke_host(wcn);
}
EXPORT_SYMBOL_GPL(uwe5622_woke_host);

static void uwe5622_notify_reset(struct uwe5622 *wcn)
{
	struct uwe5622_client *client;
	int idx, channel;

	idx = srcu_read_lock(&wcn->channel_srcu);
	for (channel = 0; channel < UWE5622_MAX_CHANNELS; channel++) {
		client = srcu_dereference(wcn->channels[channel],
					  &wcn->channel_srcu);
		if (client && client->ops->reset)
			client->ops->reset(client->priv);
	}
	srcu_read_unlock(&wcn->channel_srcu, idx);
}

/*
 * Turning the controller's logging off has to be asked of the controller, not
 * worked around on the host. With it on, the firmware formats every message,
 * builds trace records and requests a transport page for each one, and all of
 * that arrives on the shared receive FIFO whether or not anything here wants
 * it. Discarding the result on this side pays the firmware, bus, interrupt and
 * allocation cost anyway.
 *
 * Nothing claims the channel the log arrives on, so what the firmware sends
 * before this takes effect, or on an exceptional path afterwards, is dropped
 * where every unclaimed channel is dropped. It still has to be read: all the
 * logical channels share one receive FIFO.
 *
 * The single byte selects the mode: zero clears the enable that is tested
 * before the expensive work in the emitters, so nothing is formatted and no
 * page is requested. It does not stop the receive channel completely, because
 * a page already queued, a flush, or an exceptional path can still deliver
 * one, and fatal assertions go out on their own channel regardless. The
 * channel is therefore still drained; only the ring that captured it is gone.
 */
#define UWE5622_AT_ARMLOG_OFF		"at+armlog=0\r\n"
#define UWE5622_AT_TIMEOUT		msecs_to_jiffies(3000)

static int uwe5622_at_command(struct uwe5622 *wcn, const char *cmd)
{
	size_t len = strlen(cmd);
	struct sk_buff *skb;
	int ret;

	skb = alloc_skb(UWE5622_BUS_HEADROOM + len, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;
	skb_reserve(skb, UWE5622_BUS_HEADROOM);
	skb_put_data(skb, cmd, len);

	reinit_completion(&wcn->at_done);
	ret = wcn->bus_ops->tx(wcn, wcn->services[UWE5622_SERVICE_AT].tx, skb,
			       0);
	if (ret) {
		kfree_skb(skb);
		return ret;
	}

	if (!wait_for_completion_timeout(&wcn->at_done, UWE5622_AT_TIMEOUT))
		return -ETIMEDOUT;

	return 0;
}

static void uwe5622_report_blob(struct uwe5622 *wcn, const char *what,
				const char *name, const struct firmware *fw)
{
	u8 digest[SHA256_DIGEST_SIZE];

	sha256(fw->data, fw->size, digest);
	dev_info(wcn->dev, "%s %s, %zu bytes, sha256 %*phN\n", what, name,
		 fw->size, 8, digest);
}

static void uwe5622_report_firmware(struct uwe5622 *wcn,
				    const struct firmware *fw)
{
	uwe5622_report_blob(wcn, "firmware", UWE5622_FIRMWARE_NAME, fw);
}

/*
 * The controller's configuration blob. It holds the radio calibration and the
 * coexistence and antenna settings, which describe the whole chip rather than
 * the Wi-Fi side of it: the Bluetooth core shares that antenna. The firmware
 * accepts it once, before anything else configures it, so the core uploads it
 * before either of the devices it offers is created. Leaving it to the Wi-Fi
 * driver made it race with Bluetooth being opened, and whichever lost meant the
 * firmware refused the upload.
 */
#define UWE5622_CONFIG_NAME		"unisoc/uwe5622/wifi_config.bin"
#define UWE5622_CONFIG_SEC1_LEN		328
#define UWE5622_CONFIG_SEC2_LEN		1464
#define UWE5622_CONFIG_SEC3_MAX		1500
#define UWE5622_CONFIG_MAGIC_OFFSET	251
#define UWE5622_COEX_OFFSET		1380
#define UWE5622_COEX_ANT_CFG0		36
#define UWE5622_COEX_ISOLATION_CFG0	44
#define UWE5622_CMD_SYNC_VERSION	9
#define UWE5622_CMD_DOWNLOAD_INI	76
#define UWE5622_CONFIG_TIMEOUT		msecs_to_jiffies(3000)

static int uwe5622_config_command(struct uwe5622 *wcn, u8 id, const void *data,
				  size_t len)
{
	struct uwe5622_cmd_hdr *hdr;
	struct sk_buff *skb;
	int ret;

	skb = alloc_skb(UWE5622_BUS_HEADROOM + sizeof(*hdr) + len, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;
	skb_reserve(skb, UWE5622_BUS_HEADROOM);
	hdr = skb_put_zero(skb, sizeof(*hdr));
	hdr->common = UWE5622_HEAD_RSP | UWE5622_HEAD_TYPE_CMD;
	hdr->id = id;
	hdr->plen = cpu_to_le16(sizeof(*hdr) + len);
	hdr->token = cpu_to_le32(++wcn->config_token);
	if (len)
		skb_put_data(skb, data, len);

	reinit_completion(&wcn->config_done);
	wcn->config_status = -ETIMEDOUT;
	WRITE_ONCE(wcn->config_id, id);

	ret = wcn->bus_ops->tx(wcn, wcn->services[UWE5622_SERVICE_WIFI_COMMAND].tx,
			       skb, 0);
	if (ret) {
		kfree_skb(skb);
		goto out;
	}
	if (!wait_for_completion_timeout(&wcn->config_done,
					 UWE5622_CONFIG_TIMEOUT)) {
		ret = -ETIMEDOUT;
		goto out;
	}
	ret = wcn->config_status ? -EIO : 0;
out:
	WRITE_ONCE(wcn->config_id, 0);

	return ret;
}

static bool uwe5622_config_valid(struct uwe5622 *wcn,
				 const struct firmware *config,
				 u16 section_len[3])
{
	const u8 *section;
	u16 major, minor;
	size_t config_len;
	int i;

	if (config->size < 12) {
		dev_err(wcn->dev, "configuration is %zu bytes, too short for a header\n",
			config->size);
		return false;
	}
	if (memcmp(config->data, "UWEI", 4)) {
		dev_err(wcn->dev, "configuration does not begin with UWEI but %*phN\n",
			4, config->data);
		return false;
	}
	if (get_unaligned_le16(config->data + 4) != 1) {
		dev_err(wcn->dev, "configuration container version %u is not 1\n",
			get_unaligned_le16(config->data + 4));
		return false;
	}

	for (i = 0; i < 3; i++)
		section_len[i] = get_unaligned_le16(config->data + 6 + 2 * i);
	config_len = 12 + section_len[0] + section_len[1] + section_len[2];
	dev_dbg(wcn->dev, "configuration sections %u/%u/%u bytes\n",
		section_len[0], section_len[1], section_len[2]);
	if (section_len[0] != UWE5622_CONFIG_SEC1_LEN ||
	    section_len[1] != UWE5622_CONFIG_SEC2_LEN ||
	    section_len[2] > UWE5622_CONFIG_SEC3_MAX) {
		dev_err(wcn->dev,
			"configuration sections are %u/%u/%u bytes, expected %u/%u/up to %u\n",
			section_len[0], section_len[1], section_len[2],
			UWE5622_CONFIG_SEC1_LEN, UWE5622_CONFIG_SEC2_LEN,
			UWE5622_CONFIG_SEC3_MAX);
		return false;
	}
	if (config_len != config->size) {
		dev_err(wcn->dev,
			"configuration sections describe %zu bytes but the file is %zu\n",
			config_len, config->size);
		return false;
	}

	section = config->data + 12;
	major = get_unaligned_le16(section);
	minor = get_unaligned_le16(section + 2);
	if (major < 2 || major > 128 || minor > 128) {
		dev_err(wcn->dev, "configuration version %u.%u is out of range\n",
			major, minor);
		return false;
	}
	if (major > 2 && section[UWE5622_CONFIG_MAGIC_OFFSET] != 0xaa) {
		dev_err(wcn->dev,
			"configuration v%u.%u is missing its 0xaa marker, found %#02x\n",
			major, minor, section[UWE5622_CONFIG_MAGIC_OFFSET]);
		return false;
	}

	return true;
}

static int uwe5622_upload_config(struct uwe5622 *wcn)
{
	struct {
		__le32 main_version;
		u8 api[256];
	} version = { .main_version = cpu_to_le32(1) };
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
	ret = uwe5622_config_command(wcn, UWE5622_CMD_SYNC_VERSION, &version,
				     sizeof(version));
	if (ret)
		return dev_err_probe(wcn->dev, ret,
				     "controller would not agree on a command set: status %d\n",
				     wcn->config_status);

	ret = request_firmware(&config, UWE5622_CONFIG_NAME, wcn->dev);
	if (ret)
		return dev_err_probe(wcn->dev, ret, "failed to load %s\n",
				     UWE5622_CONFIG_NAME);
	uwe5622_report_blob(wcn, "configuration", UWE5622_CONFIG_NAME, config);
	if (!uwe5622_config_valid(wcn, config, section_len)) {
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
		ret = uwe5622_config_command(wcn, UWE5622_CMD_DOWNLOAD_INI,
					     download,
					     4 + section_len[i] + 2);
		kfree(download);
		dev_dbg(wcn->dev, "section %d of %u bytes answered %d\n",
			i + 1, section_len[i], wcn->config_status);
		/*
		 * The third section carries the radio calibration as a run of
		 * tagged values, and answers with the number of them it applied
		 * rather than with nothing: zero means it recognised none, which
		 * is what an empty or misencoded section looks like. The first
		 * two describe the board and answer zero for success.
		 */
		if (i == 2) {
			if (ret == -ETIMEDOUT)
				goto out_config;
			if (wcn->config_status <= 0)
				dev_warn(wcn->dev,
					 "controller applied none of the %u-byte radio calibration (status %d)\n",
					 section_len[i], wcn->config_status);
			ret = 0;
		} else if (ret) {
			dev_err(wcn->dev,
				"controller refused configuration section %d of %u bytes: %d (status %d)\n",
				i + 1, section_len[i], ret, wcn->config_status);
			goto out_config;
		}
		section += section_len[i];
	}

	/*
	 * Report what the controller accepted. A stale image or a rejected
	 * upload is otherwise indistinguishable from an arbiter that ran and
	 * granted no airtime, and that distinction is the whole question when
	 * Bluetooth and 2.4 GHz Wi-Fi will not share the antenna.
	 */
	coex = config->data + 12 + section_len[0] + UWE5622_COEX_OFFSET;
	dev_info(wcn->dev,
		 "configuration v%u.%u accepted, antenna %#x isolation %#x\n",
		 get_unaligned_le16(config->data + 12),
		 get_unaligned_le16(config->data + 14),
		 get_unaligned_le32(coex + UWE5622_COEX_ANT_CFG0),
		 get_unaligned_le32(coex + UWE5622_COEX_ISOLATION_CFG0));
	ret = 0;
out_config:
	release_firmware(config);
	if (ret)
		return dev_err_probe(wcn->dev, ret,
				     "invalid or rejected controller configuration\n");

	return 0;
}

static void uwe5622_quiet_firmware_log(struct uwe5622 *wcn)
{
	int ret = uwe5622_at_command(wcn, UWE5622_AT_ARMLOG_OFF);

	/*
	 * Worth reporting but not worth failing over: a controller that keeps
	 * logging still works, it just spends the bus on saying so.
	 */
	if (ret)
		dev_warn(wcn->dev, "controller kept its logging on: %d\n", ret);
}

static void uwe5622_notify_restart(struct uwe5622 *wcn)
{
	struct uwe5622_client *client;
	int idx, channel;

	idx = srcu_read_lock(&wcn->channel_srcu);
	for (channel = 0; channel < UWE5622_MAX_CHANNELS; channel++) {
		client = srcu_dereference(wcn->channels[channel],
					  &wcn->channel_srcu);
		if (client && client->ops->restart)
			client->ops->restart(client->priv);
	}
	srcu_read_unlock(&wcn->channel_srcu, idx);
}

static void uwe5622_recovery_work(struct work_struct *work)
{
	struct uwe5622 *wcn = container_of(work, struct uwe5622,
					   recovery_work);
	const struct firmware *fw;
	int ret;

	/*
	 * The devices this controller offers stay where they are. Taking them
	 * away and building them again gives userspace a different wiphy, and
	 * therefore a differently named interface, every time the firmware is
	 * reloaded; it also throws away the addresses, modes and interfaces that
	 * the firmware is the only thing to have forgotten. Clients are told the
	 * firmware is going, and told again once it is back.
	 */
	uwe5622_notify_reset(wcn);
	wcn->bus_ops->stop(wcn);

	/*
	 * Take the controller's power away and give it back before loading the
	 * firmware again. Stopping and restarting on its own leaves whatever
	 * wedged the firmware exactly where it was: its core keeps running from
	 * the state it is in, and nothing the host can send reaches it. Only the
	 * enable line clears the chip, which is why recovery has to go through
	 * it.
	 */
	if (wcn->bus_ops->power_cycle) {
		ret = wcn->bus_ops->power_cycle(wcn);
		if (ret)
			dev_warn(wcn->dev, "failed to cycle the controller: %d\n",
				 ret);
	}

	ret = request_firmware(&fw, UWE5622_FIRMWARE_NAME, wcn->dev);
	if (ret)
		goto out_failed;
	ret = wcn->bus_ops->start(wcn, fw);
	release_firmware(fw);
	if (ret)
		goto out_failed;
	uwe5622_quiet_firmware_log(wcn);
	ret = uwe5622_upload_config(wcn);
	if (ret)
		goto out_failed;

	mutex_lock(&wcn->state_mutex);
	if (wcn->removing) {
		mutex_unlock(&wcn->state_mutex);
		wcn->bus_ops->stop(wcn);
		return;
	}
	wcn->state = UWE5622_READY;
	mutex_unlock(&wcn->state_mutex);

	/* Ready first: putting a client back together takes commands. */
	uwe5622_notify_restart(wcn);

	dev_info(wcn->dev, "firmware recovery completed\n");
	return;

out_failed:
	/*
	 * The controller is dead and holds nothing worth parking, so record
	 * that rather than staying in a transitional state for ever: a state
	 * that refuses system sleep would keep the whole machine awake for a
	 * radio that no longer works.
	 */
	mutex_lock(&wcn->state_mutex);
	if (wcn->state == UWE5622_RECOVERING)
		wcn->state = UWE5622_OFF;
	mutex_unlock(&wcn->state_mutex);
	dev_err(wcn->dev, "firmware recovery failed: %d\n", ret);
}

void uwe5622_recover(struct uwe5622_client *client)
{
	struct uwe5622 *wcn;

	if (!client)
		return;
	wcn = client->wcn;
	mutex_lock(&wcn->state_mutex);
	if (wcn->state == UWE5622_READY && !wcn->removing) {
		wcn->state = UWE5622_RECOVERING;
		schedule_work(&wcn->recovery_work);
	}
	mutex_unlock(&wcn->state_mutex);
}
EXPORT_SYMBOL_GPL(uwe5622_recover);

void uwe5622_bluetooth_enable(struct uwe5622_client *client, bool enabled)
{
	if (client && client->wcn->bluetooth_enable)
		gpiod_set_value_cansleep(client->wcn->bluetooth_enable, enabled);
}
EXPORT_SYMBOL_GPL(uwe5622_bluetooth_enable);

/*
 * The controller keeps the Bluetooth memory powered down until asked, and a
 * Bluetooth core without its memory answers commands from the host while its
 * radio receives nothing at all.
 */
int uwe5622_bluetooth_ram(struct uwe5622_client *client, bool on)
{
	if (!client || !client->wcn->bus_ops->bt_ram)
		return 0;

	return client->wcn->bus_ops->bt_ram(client->wcn, on);
}
EXPORT_SYMBOL_GPL(uwe5622_bluetooth_ram);

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

/*
 * Which image the controller is actually running, recorded next to whatever is
 * being diagnosed: a stale firmware invalidates every conclusion drawn from a
 * capture, and the file name alone does not say what is in it.
 */

/*
 * Whether this is the answer to the configuration the core is uploading. Only
 * that exact answer is claimed: the channel belongs to the Wi-Fi device, which
 * stays bound while the firmware is replaced, and everything else on it -- its
 * own answers and the events it is waiting for -- has to reach it untouched.
 */
static bool uwe5622_config_answer(struct uwe5622 *wcn, u8 channel,
				  struct sk_buff *skb)
{
	const struct uwe5622_cmd_hdr *hdr = (const void *)skb->data;

	if (channel != wcn->services[UWE5622_SERVICE_WIFI_COMMAND].rx ||
	    !READ_ONCE(wcn->config_id) || skb->len < sizeof(*hdr))
		return false;
	if (hdr->id != READ_ONCE(wcn->config_id) ||
	    le32_to_cpu(hdr->token) != wcn->config_token)
		return false;

	wcn->config_status = hdr->status;
	complete(&wcn->config_done);

	return true;
}

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
	/*
	 * A configuration the core is uploading is answered on the channel the
	 * Wi-Fi device otherwise owns, and it is uploaded again after the
	 * firmware is replaced, when that device is still bound. So the core
	 * takes the answer whenever it has one outstanding: the device it
	 * belongs to has been told the firmware went and is not waiting for
	 * anything of its own.
	 */
	if (uwe5622_config_answer(wcn, channel, skb)) {
		kfree_skb(skb);
	} else if (client) {
		client->ops->rx(client->priv, skb);
	} else if (channel == wcn->services[UWE5622_SERVICE_AT].rx) {
		dev_dbg(wcn->dev, "AT answer: %*phN\n", (int)skb->len,
			skb->data);
		kfree_skb(skb);
		complete(&wcn->at_done);
	} else {
		dev_dbg_ratelimited(wcn->dev,
				    "dropping %u bytes on unclaimed channel %u\n",
				    skb->len, channel);
		kfree_skb(skb);
	}
	srcu_read_unlock(&wcn->channel_srcu, idx);
}

int uwe5622_core_probe(struct uwe5622 *wcn)
{
	const struct firmware *fw;
	int ret;

	mutex_init(&wcn->state_mutex);
	mutex_init(&wcn->channel_mutex);
	init_completion(&wcn->at_done);
	init_completion(&wcn->config_done);
	INIT_WORK(&wcn->recovery_work, uwe5622_recovery_work);
	ret = init_srcu_struct(&wcn->channel_srcu);
	if (ret)
		return ret;
	wcn->state = UWE5622_BOOTING;

	ret = request_firmware(&fw, UWE5622_FIRMWARE_NAME, wcn->dev);
	if (ret)
		goto err_firmware;

	uwe5622_report_firmware(wcn, fw);
	ret = wcn->bus_ops->start(wcn, fw);
	/*
	 * A controller that is already running its firmware ignores a second
	 * load and never answers the ready handshake, which is what a driver
	 * reload without a board reset looks like. Take its power away and try
	 * once more rather than leaving the device unusable until the next boot.
	 */
	if (ret == -ETIMEDOUT && wcn->bus_ops->power_cycle) {
		dev_info(wcn->dev,
			 "controller did not answer, cycling its power\n");
		if (!wcn->bus_ops->power_cycle(wcn))
			ret = wcn->bus_ops->start(wcn, fw);
	}
	release_firmware(fw);
	if (ret)
		goto err_start;
	uwe5622_quiet_firmware_log(wcn);
	/*
	 * Before either device exists, so that nothing else has configured the
	 * controller by the time it is asked to accept this.
	 */
	ret = uwe5622_upload_config(wcn);
	if (ret)
		goto err_stop;

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
	wcn->removing = true;
	mutex_unlock(&wcn->state_mutex);
	cancel_work_sync(&wcn->recovery_work);

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
	/* Nothing to park; do not keep the system awake for a dead radio. */
	if (wcn->state == UWE5622_OFF) {
		ret = 0;
		goto out;
	}
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
	if (wcn->state == UWE5622_OFF) {
		ret = 0;
		goto out;
	}
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
MODULE_FIRMWARE(UWE5622_CONFIG_NAME);
MODULE_DESCRIPTION("Unisoc UWE5622 connectivity core");
MODULE_LICENSE("GPL");
