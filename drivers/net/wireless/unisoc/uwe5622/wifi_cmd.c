// SPDX-License-Identifier: GPL-2.0-only

#include <linux/errno.h>
#include <linux/bitfield.h>
#include <linux/jiffies.h>
#include <linux/skbuff.h>
#include <linux/unaligned.h>

#include "wifi.h"

#define UWE5622_HEAD_TYPE_MASK	GENMASK(2, 0)
#define UWE5622_HEAD_RSP		BIT(4)
#define UWE5622_HEAD_CTX_MASK	GENMASK(7, 5)
#define UWE5622_HEAD_TYPE_CMD	0
#define UWE5622_HEAD_TYPE_EVENT	1

int uwe5622_wifi_cmd(struct uwe5622_wifi *wifi, u8 ctx_id, u8 id,
		     const void *data, size_t len, void *response,
		     size_t *response_len, u8 *response_ctx)
{
	struct uwe5622_cmd_hdr *hdr;
	struct sk_buff *skb;
	size_t copy_len;
	u32 token;
	int ret;

	if (ctx_id >= UWE5622_WIFI_MAX_CTX ||
	    len > UWE5622_WIFI_CMD_TX_MAX - sizeof(*hdr))
		return -EINVAL;

	mutex_lock(&wifi->cmd_mutex);
	if (wifi->stopping) {
		ret = -ESHUTDOWN;
		goto out_unlock;
	}

	skb = alloc_skb(sizeof(*hdr) + len, GFP_KERNEL);
	if (!skb) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	token = ++wifi->next_token;
	if (!token)
		token = ++wifi->next_token;
	hdr = skb_put_zero(skb, sizeof(*hdr));
	hdr->common = FIELD_PREP(UWE5622_HEAD_CTX_MASK, ctx_id) |
		      UWE5622_HEAD_RSP | UWE5622_HEAD_TYPE_CMD;
	hdr->id = id;
	hdr->plen = cpu_to_le16(sizeof(*hdr) + len);
	hdr->token = cpu_to_le32(token);
	if (len)
		skb_put_data(skb, data, len);

	reinit_completion(&wifi->cmd_done);
	WRITE_ONCE(wifi->pending_id, id);
	WRITE_ONCE(wifi->pending_token, token);
	wifi->response_len = 0;
	wifi->response_status = -ETIMEDOUT;

	ret = uwe5622_client_send(wifi->cmd_client, skb);
	kfree_skb(skb);
	if (ret)
		goto out_clear;

	if (!wait_for_completion_timeout(&wifi->cmd_done,
					 UWE5622_WIFI_CMD_TIMEOUT)) {
		dev_err(wifi->dev, "command %#x timed out\n", id);
		ret = -ETIMEDOUT;
		goto out_clear;
	}

	ret = wifi->response_status ? -EIO : 0;
	if (response_ctx)
		*response_ctx = wifi->response_ctx;
	if (response_len) {
		copy_len = min(*response_len, wifi->response_len);
		if (response && copy_len)
			memcpy(response, wifi->response, copy_len);
		*response_len = copy_len;
	}

out_clear:
	WRITE_ONCE(wifi->pending_token, 0);
	WRITE_ONCE(wifi->pending_id, 0);
out_unlock:
	mutex_unlock(&wifi->cmd_mutex);
	return ret;
}

void uwe5622_wifi_cmd_rx(void *priv, struct sk_buff *skb)
{
	struct uwe5622_wifi *wifi = priv;
	const struct uwe5622_cmd_hdr *hdr;
	size_t len, payload_len;
	u8 type;

	if (skb->len < sizeof(*hdr))
		goto out;

	hdr = (const void *)skb->data;
	len = le16_to_cpu(hdr->plen);
	if (len < sizeof(*hdr) || len > skb->len)
		goto out;
	payload_len = len - sizeof(*hdr);
	type = FIELD_GET(UWE5622_HEAD_TYPE_MASK, hdr->common);

	if (type == UWE5622_HEAD_TYPE_EVENT) {
		uwe5622_wifi_event(wifi, hdr, skb->data + sizeof(*hdr),
				   payload_len);
		goto out;
	}

	if (type != UWE5622_HEAD_TYPE_CMD ||
	    hdr->id != READ_ONCE(wifi->pending_id) ||
	    le32_to_cpu(hdr->token) != READ_ONCE(wifi->pending_token))
		goto out;

	wifi->response_ctx = FIELD_GET(UWE5622_HEAD_CTX_MASK, hdr->common);
	wifi->response_status = hdr->status;
	wifi->response_len = min(payload_len, sizeof(wifi->response));
	memcpy(wifi->response, skb->data + sizeof(*hdr), wifi->response_len);
	complete(&wifi->cmd_done);
out:
	kfree_skb(skb);
}

void uwe5622_wifi_cmd_reset(void *priv)
{
	struct uwe5622_wifi *wifi = priv;

	wifi->response_status = -ESHUTDOWN;
	complete(&wifi->cmd_done);
}
