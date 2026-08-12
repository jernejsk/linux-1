// SPDX-License-Identifier: GPL-2.0-only

#include <linux/auxiliary_bus.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/skbuff.h>
#include <linux/uwe5622.h>
#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#define UWE5622_BT_TX_MAX	63
#define UWE5622_BT_CMD_TIMEOUT	msecs_to_jiffies(5000)

#define UWE5622_BT_SET_SLEEP_MODE 0xfd09
#define UWE5622_BT_START_SLEEP	 0xfd0d

struct uwe5622_bt {
	struct device *dev;
	struct hci_dev *hdev;
	struct uwe5622_client *client;
	struct sk_buff_head tx_queue;
	struct work_struct tx_work;
	/* Protects the partial H4 receive frame and parser lengths. */
	spinlock_t rx_lock;
	struct sk_buff *rx_skb;
	size_t rx_need;
	size_t rx_header_len;
	bool opened;
	bool suspended;
};

static size_t uwe5622_bt_header_len(u8 type)
{
	switch (type) {
	case HCI_EVENT_PKT:
		return HCI_EVENT_HDR_SIZE;
	case HCI_ACLDATA_PKT:
		return HCI_ACL_HDR_SIZE;
	case HCI_SCODATA_PKT:
		return HCI_SCO_HDR_SIZE;
	default:
		return 0;
	}
}

static size_t uwe5622_bt_frame_len(struct sk_buff *skb)
{
	switch (hci_skb_pkt_type(skb)) {
	case HCI_EVENT_PKT:
		return HCI_EVENT_HDR_SIZE + ((struct hci_event_hdr *)skb->data)->plen;
	case HCI_ACLDATA_PKT:
		return HCI_ACL_HDR_SIZE +
			le16_to_cpu(((struct hci_acl_hdr *)skb->data)->dlen);
	case HCI_SCODATA_PKT:
		return HCI_SCO_HDR_SIZE + ((struct hci_sco_hdr *)skb->data)->dlen;
	default:
		return 0;
	}
}

static void uwe5622_bt_rx(void *priv, struct sk_buff *wire)
{
	struct uwe5622_bt *bt = priv;
	struct sk_buff *complete;
	const u8 *data = wire->data;
	size_t copy, frame_len, len = wire->len;
	u8 type;

	while (len) {
		complete = NULL;
		spin_lock(&bt->rx_lock);
		if (!bt->opened) {
			spin_unlock(&bt->rx_lock);
			break;
		}
		if (!bt->rx_skb) {
			type = *data++;
			len--;
			/*
			 * The controller pads what it sends, and the padding
			 * arrives after a complete frame. Zero is not a packet
			 * type, so between frames it is padding rather than a
			 * receive error, and counting it as one only makes the
			 * controller look unhealthy.
			 */
			if (!type) {
				spin_unlock(&bt->rx_lock);
				continue;
			}
			bt->rx_header_len = uwe5622_bt_header_len(type);
			if (!bt->rx_header_len) {
				bt->hdev->stat.err_rx++;
				spin_unlock(&bt->rx_lock);
				continue;
			}
			bt->rx_skb = bt_skb_alloc(HCI_MAX_FRAME_SIZE, GFP_ATOMIC);
			if (!bt->rx_skb) {
				bt->hdev->stat.err_rx++;
				spin_unlock(&bt->rx_lock);
				break;
			}
			hci_skb_pkt_type(bt->rx_skb) = type;
			bt->rx_need = bt->rx_header_len;
		}

		copy = min(len, bt->rx_need - bt->rx_skb->len);
		skb_put_data(bt->rx_skb, data, copy);
		data += copy;
		len -= copy;
		if (bt->rx_skb->len < bt->rx_need) {
			spin_unlock(&bt->rx_lock);
			continue;
		}
		if (bt->rx_need == bt->rx_header_len) {
			frame_len = uwe5622_bt_frame_len(bt->rx_skb);
			if (frame_len < bt->rx_header_len ||
			    frame_len > HCI_MAX_FRAME_SIZE) {
				kfree_skb(bt->rx_skb);
				bt->rx_skb = NULL;
				bt->hdev->stat.err_rx++;
				spin_unlock(&bt->rx_lock);
				continue;
			}
			bt->rx_need = frame_len;
			if (bt->rx_skb->len < bt->rx_need) {
				spin_unlock(&bt->rx_lock);
				continue;
			}
		}
		complete = bt->rx_skb;
		bt->rx_skb = NULL;
		spin_unlock(&bt->rx_lock);
		if (hci_recv_frame(bt->hdev, complete) < 0)
			bt->hdev->stat.err_rx++;
	}
	kfree_skb(wire);
}

static void uwe5622_bt_reset(void *priv)
{
	struct uwe5622_bt *bt = priv;

	if (bt->opened)
		hci_reset_dev(bt->hdev);
}

static const struct uwe5622_client_ops uwe5622_bt_client_ops = {
	.rx = uwe5622_bt_rx,
	.reset = uwe5622_bt_reset,
};

static void uwe5622_bt_tx_work(struct work_struct *work)
{
	struct uwe5622_bt *bt = container_of(work, struct uwe5622_bt, tx_work);
	struct sk_buff *skb, *wire;
	size_t h4_len, pad;
	u8 type;
	int ret;

	while ((skb = skb_dequeue(&bt->tx_queue))) {
		type = hci_skb_pkt_type(skb);
		h4_len = 1 + skb->len;
		pad = (type == HCI_COMMAND_PKT || type == HCI_ACLDATA_PKT) ?
			4 - h4_len % 4 : 0;
		wire = alloc_skb(h4_len + pad, GFP_KERNEL);
		if (!wire) {
			bt->hdev->stat.err_tx++;
			goto free;
		}
		skb_put_u8(wire, type);
		skb_put_data(wire, skb->data, skb->len);
		if (pad)
			skb_put_zero(wire, pad);
		ret = uwe5622_client_send(bt->client, wire);
		kfree_skb(wire);
		if (ret)
			bt->hdev->stat.err_tx++;
free:
		kfree_skb(skb);
	}
}

static int uwe5622_bt_open(struct hci_dev *hdev)
{
	struct uwe5622_bt *bt = hci_get_drvdata(hdev);
	int ret;

	ret = uwe5622_power_get(bt->client);
	if (ret)
		return ret;
	uwe5622_bluetooth_enable(bt->client, true);
	uwe5622_bluetooth_wake(bt->client, true);
	ret = uwe5622_bluetooth_ram(bt->client, true);
	if (ret && ret != -ENODEV)
		bt_dev_warn(hdev, "failed to power the Bluetooth memory: %d",
			    ret);
	/* The firmware's Bluetooth stack needs time before it answers HCI. */
	msleep(100);
	spin_lock_bh(&bt->rx_lock);
	bt->opened = true;
	spin_unlock_bh(&bt->rx_lock);
	return 0;
}

static int uwe5622_bt_flush(struct hci_dev *hdev)
{
	struct uwe5622_bt *bt = hci_get_drvdata(hdev);

	cancel_work_sync(&bt->tx_work);
	skb_queue_purge(&bt->tx_queue);
	return 0;
}

/*
 * The controller does not join the firmware's coexistence arbitration until it is
 * told that Bluetooth is in use. Until then its Wi-Fi side registers as the only
 * active client, the shared scheduler that divides one antenna between the two is
 * never entered, and Bluetooth reception dies whenever Wi-Fi holds a 2.4 GHz
 * channel: on this board discovery finds nothing at all at -28 dBm, and finds its
 * target as soon as this command has been sent. It is the vendor's dual-mode
 * enable, sent by its own initialization, and it belongs to bringing the
 * controller up rather than to any board.
 */
#define UWE5622_BT_OP_ENABLE	0xfca1

static int uwe5622_bt_enable(struct hci_dev *hdev, bool on)
{
	static const u8 disable[] = { 0x00, 0x00, 0x00 };
	static const u8 enable[] = { 0x00, 0x00, 0x01 };
	struct sk_buff *skb;

	skb = __hci_cmd_sync(hdev, UWE5622_BT_OP_ENABLE, sizeof(enable),
			     on ? enable : disable, HCI_INIT_TIMEOUT);
	if (IS_ERR(skb))
		return PTR_ERR(skb);

	/*
	 * The controller answers with the payload it accepted rather than a bare
	 * status, so check that what came back is what was asked for instead of
	 * assuming the usual layout.
	 */
	if (skb->len < sizeof(enable) ||
	    skb->data[skb->len - 1] != (on ? 0x01 : 0x00)) {
		bt_dev_warn(hdev, "coexistence %s not confirmed: %*ph",
			    on ? "enable" : "disable", (int)skb->len, skb->data);
		kfree_skb(skb);
		return -EIO;
	}
	kfree_skb(skb);

	return 0;
}

static int uwe5622_bt_setup(struct hci_dev *hdev)
{
	return uwe5622_bt_enable(hdev, true);
}

static int uwe5622_bt_close(struct hci_dev *hdev)
{
	struct uwe5622_bt *bt = hci_get_drvdata(hdev);
	struct sk_buff *rx;

	/*
	 * While the transport is still up, so the firmware stops counting
	 * Bluetooth as an active coexistence client and gives Wi-Fi the antenna
	 * back.
	 */
	if (bt->opened && !bt->suspended)
		uwe5622_bt_enable(hdev, false);

	uwe5622_bt_flush(hdev);
	spin_lock_bh(&bt->rx_lock);
	bt->opened = false;
	rx = bt->rx_skb;
	bt->rx_skb = NULL;
	spin_unlock_bh(&bt->rx_lock);
	kfree_skb(rx);
	uwe5622_bluetooth_wake(bt->client, false);
	uwe5622_bluetooth_enable(bt->client, false);
	uwe5622_power_put(bt->client);
	return 0;
}

static int uwe5622_bt_send(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct uwe5622_bt *bt = hci_get_drvdata(hdev);

	if (bt->suspended)
		return -EHOSTDOWN;
	if (skb_queue_len(&bt->tx_queue) >= UWE5622_BT_TX_MAX)
		return -ENOBUFS;
	switch (hci_skb_pkt_type(skb)) {
	case HCI_COMMAND_PKT:
		hdev->stat.cmd_tx++;
		break;
	case HCI_ACLDATA_PKT:
		hdev->stat.acl_tx++;
		break;
	case HCI_SCODATA_PKT:
		hdev->stat.sco_tx++;
		break;
	default:
		return -EILSEQ;
	}
	skb_queue_tail(&bt->tx_queue, skb);
	schedule_work(&bt->tx_work);
	return 0;
}

static int uwe5622_bt_sleep_mode(struct uwe5622_bt *bt, bool suspend,
				 bool shutdown)
{
	u8 param[] = { suspend, shutdown ? 0 : 1, 0, 0, 0, 0 };
	int ret;

	ret = hci_cmd_sync_status(bt->hdev, UWE5622_BT_SET_SLEEP_MODE,
				  sizeof(param), param, UWE5622_BT_CMD_TIMEOUT);
	if (ret || !suspend)
		return ret;
	return hci_cmd_sync_status(bt->hdev, UWE5622_BT_START_SLEEP, 0, NULL,
				   UWE5622_BT_CMD_TIMEOUT);
}

static int uwe5622_bt_suspend(struct device *dev)
{
	struct uwe5622_bt *bt = dev_get_drvdata(dev);
	int ret;

	if (!bt->opened)
		return 0;
	ret = hci_suspend_dev(bt->hdev);
	if (ret)
		return ret;
	flush_work(&bt->tx_work);
	ret = uwe5622_bt_sleep_mode(bt, true, false);
	if (ret) {
		hci_resume_dev(bt->hdev);
		return ret;
	}
	bt->suspended = true;
	uwe5622_bluetooth_wake(bt->client, false);
	uwe5622_set_wake(bt->client, device_may_wakeup(dev));
	return 0;
}

static int uwe5622_bt_resume(struct device *dev)
{
	struct uwe5622_bt *bt = dev_get_drvdata(dev);
	int ret;

	if (!bt->opened)
		return 0;
	uwe5622_bluetooth_wake(bt->client, true);
	msleep(20);
	bt->suspended = false;
	ret = uwe5622_bt_sleep_mode(bt, false, false);
	if (ret) {
		/*
		 * The controller never acknowledged being woken, so put the
		 * transport back where it was instead of accepting packets it
		 * cannot answer: stay suspended, drop the wake line, and leave
		 * HCI stopped for the core to resume later.
		 */
		bt->suspended = true;
		uwe5622_bluetooth_wake(bt->client, false);
		uwe5622_set_wake(bt->client, false);
		return ret;
	}
	ret = hci_resume_dev(bt->hdev);
	uwe5622_set_wake(bt->client, false);
	return ret;
}

static DEFINE_SIMPLE_DEV_PM_OPS(uwe5622_bt_pm_ops, uwe5622_bt_suspend,
				 uwe5622_bt_resume);

static int uwe5622_bt_probe(struct auxiliary_device *adev,
			    const struct auxiliary_device_id *id)
{
	struct uwe5622_bt *bt;
	struct hci_dev *hdev;
	int ret;

	bt = devm_kzalloc(&adev->dev, sizeof(*bt), GFP_KERNEL);
	if (!bt)
		return -ENOMEM;
	bt->dev = &adev->dev;
	spin_lock_init(&bt->rx_lock);
	skb_queue_head_init(&bt->tx_queue);
	INIT_WORK(&bt->tx_work, uwe5622_bt_tx_work);
	bt->client = uwe5622_client_register(&adev->dev,
					     UWE5622_SERVICE_BLUETOOTH,
					     &uwe5622_bt_client_ops, bt);
	if (IS_ERR(bt->client))
		return PTR_ERR(bt->client);

	hdev = hci_alloc_dev();
	if (!hdev) {
		ret = -ENOMEM;
		goto err_client;
	}
	bt->hdev = hdev;
	switch (uwe5622_client_bus(bt->client)) {
	case UWE5622_BUS_USB:
		hdev->bus = HCI_USB;
		break;
	case UWE5622_BUS_PCIE:
		hdev->bus = HCI_PCI;
		break;
	case UWE5622_BUS_SDIO:
	default:
		hdev->bus = HCI_SDIO;
		break;
	}
	hdev->open = uwe5622_bt_open;
	hdev->close = uwe5622_bt_close;
	hdev->setup = uwe5622_bt_setup;
	hdev->flush = uwe5622_bt_flush;
	hdev->send = uwe5622_bt_send;
	/*
	 * The firmware claims the hold, sniff and park link policy modes in its
	 * features and then rejects being configured with all of them at once,
	 * which fails controller setup outright. Nothing else needs the command,
	 * so skip it and keep the supported command bitmap, which the controller
	 * does report correctly and which the core needs to see that extended
	 * scanning is available here: reading how many advertising sets the
	 * controller has puts it in extended mode, after which it answers the
	 * legacy scan commands with Command Disallowed, as it should.
	 */
	hci_set_quirk(hdev, HCI_QUIRK_BROKEN_WRITE_DEF_LINK_POLICY);
	hci_set_drvdata(hdev, bt);
	SET_HCIDEV_DEV(hdev, &adev->dev);
	ret = hci_register_dev(hdev);
	if (ret)
		goto err_hdev;
	auxiliary_set_drvdata(adev, bt);
	ret = device_init_wakeup(&adev->dev,
				 device_property_read_bool(&adev->dev,
							   "wakeup-source"));
	if (ret) {
		hci_unregister_dev(hdev);
		goto err_hdev;
	}
	return 0;

err_hdev:
	hci_free_dev(hdev);
err_client:
	uwe5622_client_unregister(bt->client);
	return ret;
}

static void uwe5622_bt_remove(struct auxiliary_device *adev)
{
	struct uwe5622_bt *bt = auxiliary_get_drvdata(adev);

	hci_unregister_dev(bt->hdev);
	device_init_wakeup(&adev->dev, false);
	cancel_work_sync(&bt->tx_work);
	skb_queue_purge(&bt->tx_queue);
	uwe5622_client_unregister(bt->client);
	hci_free_dev(bt->hdev);
}

static void uwe5622_bt_shutdown(struct auxiliary_device *adev)
{
	struct uwe5622_bt *bt = auxiliary_get_drvdata(adev);

	if (!bt->opened)
		return;
	uwe5622_bt_sleep_mode(bt, true, true);
	bt->suspended = true;
	uwe5622_bluetooth_wake(bt->client, false);
	uwe5622_bluetooth_enable(bt->client, false);
}

static const struct auxiliary_device_id uwe5622_bt_ids[] = {
	{ .name = "uwe5622_core.bluetooth" },
	{}
};
MODULE_DEVICE_TABLE(auxiliary, uwe5622_bt_ids);

static struct auxiliary_driver uwe5622_bt_driver = {
	.name = "uwe5622_bluetooth",
	.probe = uwe5622_bt_probe,
	.remove = uwe5622_bt_remove,
	.shutdown = uwe5622_bt_shutdown,
	.id_table = uwe5622_bt_ids,
	.driver.pm = pm_ptr(&uwe5622_bt_pm_ops),
};
module_auxiliary_driver(uwe5622_bt_driver);

MODULE_DESCRIPTION("Unisoc UWE5622 Bluetooth HCI driver");
MODULE_LICENSE("GPL");
