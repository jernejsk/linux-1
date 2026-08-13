/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __UWE5622_CORE_H
#define __UWE5622_CORE_H

#include <linux/auxiliary_bus.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/mutex.h>
#include <linux/refcount.h>
#include <linux/skbuff.h>
#include <linux/srcu.h>
#include <linux/kfifo.h>
#include <linux/uwe5622.h>
#include <linux/workqueue.h>

#define UWE5622_FIRMWARE_NAME	"unisoc/uwe5622/wcnmodem.bin"
#define UWE5622_MAX_CHANNELS	32

enum uwe5622_state {
	UWE5622_OFF,
	UWE5622_BOOTING,
	UWE5622_READY,
	UWE5622_SUSPENDING,
	UWE5622_SUSPENDED,
	UWE5622_RECOVERING,
	UWE5622_STOPPING,
};

struct uwe5622_bus_ops {
	int (*start)(struct uwe5622 *wcn, const struct firmware *fw);
	void (*stop)(struct uwe5622 *wcn);
	int (*tx)(struct uwe5622 *wcn, u8 channel, struct sk_buff *skb,
		  u8 tag);
	int (*bt_ram)(struct uwe5622 *wcn, bool on);
	/* Cycles the controller's power, losing everything it held. */
	int (*power_cycle)(struct uwe5622 *wcn);
	int (*suspend)(struct uwe5622 *wcn, bool wake);
	int (*resume)(struct uwe5622 *wcn);
};

struct uwe5622_client {
	struct uwe5622 *wcn;
	const struct uwe5622_client_ops *ops;
	void *priv;
	u8 tx_channel;
	u8 rx_channel;
	bool wake_enabled;
};

struct uwe5622_auxdev {
	struct auxiliary_device adev;
};

struct uwe5622_channel_pair {
	u8 tx;
	u8 rx;
};

struct uwe5622 {
	struct device *dev;
	const struct uwe5622_bus_ops *bus_ops;
	void *bus_priv;
	enum uwe5622_bus_type bus_type;
	struct uwe5622_channel_pair services[UWE5622_SERVICE_COUNT];

	/* Serializes firmware state, users, and system PM transitions. */
	struct mutex state_mutex;
	enum uwe5622_state state;
	unsigned int users;
	bool wake_enabled;
	bool removing;
	struct work_struct recovery_work;
	struct gpio_desc *bluetooth_enable;
	struct gpio_desc *device_wake;

	/* Serializes logical-channel registration and wake aggregation. */
	struct mutex channel_mutex;
	struct srcu_struct channel_srcu;
	struct uwe5622_client __rcu *channels[UWE5622_MAX_CHANNELS];
	/*
	 * The controller's trace ring, drained through debugfs. printk loses
	 * most of it to rate limiting, and a lost record cannot be told apart
	 * from a record the firmware never wrote.
	 */
	struct kfifo trace_fifo;
	spinlock_t trace_lock;
	struct dentry *trace_dir;
	unsigned int trace_records;
	unsigned int trace_dropped;

	struct uwe5622_auxdev *wifi_auxdev;
	struct uwe5622_auxdev *bt_auxdev;
};

int uwe5622_core_probe(struct uwe5622 *wcn);
void uwe5622_core_remove(struct uwe5622 *wcn);
void uwe5622_core_shutdown(struct uwe5622 *wcn);
int uwe5622_core_suspend(struct uwe5622 *wcn);
int uwe5622_core_resume(struct uwe5622 *wcn);
void uwe5622_core_rx(struct uwe5622 *wcn, u8 channel, struct sk_buff *skb);
void uwe5622_core_tx_error(struct uwe5622 *wcn, u8 tx_channel, u8 tag);
void uwe5622_core_request_recovery(struct uwe5622 *wcn);
int uwe5622_bind_verify(const u8 challenge[16], u8 response[16]);

#endif
