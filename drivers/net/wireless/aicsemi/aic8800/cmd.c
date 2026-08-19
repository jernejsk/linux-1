// SPDX-License-Identifier: GPL-2.0-only
/*
 * Control message channel towards the AIC8800 firmware.
 *
 * Copyright (C) 2026 Jernej Skrabec <jernej.skrabec@gmail.com>
 */

#include <linux/slab.h>

#include "aic8800.h"

#define AIC_CMD_TIMEOUT_MS	15000

/* Largest confirmation payload the firmware can produce. */
#define AIC_MSG_PARAM_MAX	1024

/*
 * Wire format of a request.  The parameters follow the header and the whole
 * thing is handed to the transport as one blob.
 */
struct aic_msg_hdr {
	__le16 id;
	__le16 dst_id;
	__le16 src_id;
	__le16 param_len;
	u8 param[] __counted_by_le(param_len);
};

/* Wire format of a confirmation or indication. */
struct aic_msg_ind {
	__le16 id;
	__le16 dst_id;
	__le16 src_id;
	__le16 param_len;
	__le32 pattern;
	u8 param[] __counted_by_le(param_len);
};

void aic_cmd_mgr_init(struct aic_cmd_mgr *mgr)
{
	spin_lock_init(&mgr->lock);
	INIT_LIST_HEAD(&mgr->cmds);
	mutex_init(&mgr->send_lock);
	mgr->timeouts = 0;
	mgr->crashed = false;
}

void aic_cmd_mgr_deinit(struct aic_cmd_mgr *mgr)
{
	struct aic_cmd *cmd, *tmp;

	spin_lock_bh(&mgr->lock);
	mgr->crashed = true;
	list_for_each_entry_safe(cmd, tmp, &mgr->cmds, list) {
		list_del(&cmd->list);
		cmd->result = -EPIPE;
		complete(&cmd->done);
	}
	spin_unlock_bh(&mgr->lock);

	mutex_destroy(&mgr->send_lock);
}

/**
 * aic_msg_alloc - allocate a request buffer
 * @id: message id
 * @dst: destination task
 * @src: source task
 * @param_len: size of the parameter structure
 *
 * Returns a pointer to the parameter area, to be passed to aic_send_msg().
 */
void *aic_msg_alloc(u16 id, u16 dst, u16 src, u16 param_len)
{
	struct aic_msg_hdr *msg;

	msg = kzalloc(struct_size(msg, param, param_len), GFP_KERNEL);
	if (!msg)
		return NULL;

	msg->id = cpu_to_le16(id);
	msg->dst_id = cpu_to_le16(dst);
	msg->src_id = cpu_to_le16(src);
	msg->param_len = cpu_to_le16(param_len);

	return msg->param;
}

static struct aic_msg_hdr *aic_msg_of(const void *param)
{
	return container_of(param, struct aic_msg_hdr, param);
}

void aic_msg_free(const void *param)
{
	kfree(aic_msg_of(param));
}

/**
 * aic_send_msg_timeout - send a request to the firmware
 * @hw: device
 * @param: parameter area returned by aic_msg_alloc(), freed on return
 * @cfm_id: id of the expected confirmation, zero to not wait for one
 * @cfm: buffer for the confirmation payload, may be %NULL
 * @cfm_len: size of @cfm
 * @timeout_ms: how long to wait for the confirmation
 * @fatal: treat a timeout as the firmware having died
 */
int aic_send_msg_timeout(struct aic_hw *hw, const void *param, u16 cfm_id,
			 void *cfm, u16 cfm_len, unsigned int timeout_ms,
			 bool fatal)
{
	struct aic_msg_hdr *msg = aic_msg_of(param);
	struct aic_cmd_mgr *mgr = &hw->cmd_mgr;
	unsigned int len = struct_size(msg, param, le16_to_cpu(msg->param_len));
	bool need_cfm = cfm_id != 0;
	struct aic_cmd cmd = {};
	int ret;

	/*
	 * A single timeout is not proof that the firmware died, and refusing
	 * everything afterwards turns one lost answer into a dead device.
	 */
	if (mgr->timeouts >= AIC_CMD_MAX_TIMEOUTS) {
		aic_msg_free(param);
		return -EPIPE;
	}

	cmd.id = le16_to_cpu(msg->id);
	cmd.cfm_id = cfm_id;
	cmd.cfm = cfm;
	cmd.cfm_len = cfm_len;
	cmd.result = -EINTR;
	init_completion(&cmd.done);

	/*
	 * The firmware processes one request at a time and answers in order, so
	 * serialise here rather than tracking tokens.
	 */
	mutex_lock(&mgr->send_lock);

	if (need_cfm) {
		spin_lock_bh(&mgr->lock);
		list_add_tail(&cmd.list, &mgr->cmds);
		spin_unlock_bh(&mgr->lock);
	}

	ret = hw->bus_ops->send_msg(hw, msg, len);
	if (ret) {
		dev_err(hw->dev, "failed to send message %04x: %d\n",
			cmd.id, ret);
		goto out;
	}

	if (!need_cfm)
		goto out;

	if (!wait_for_completion_timeout(&cmd.done,
					 msecs_to_jiffies(timeout_ms))) {
		if (fatal) {
			dev_err(hw->dev,
				"message %04x timed out waiting for %04x\n",
				cmd.id, cfm_id);
			mgr->timeouts++;
		}
		ret = -ETIMEDOUT;
		goto out;
	}

	ret = cmd.result;
	if (!ret)
		mgr->timeouts = 0;

out:
	if (need_cfm) {
		spin_lock_bh(&mgr->lock);
		if (!list_empty(&cmd.list))
			list_del(&cmd.list);
		spin_unlock_bh(&mgr->lock);
	}
	mutex_unlock(&mgr->send_lock);
	aic_msg_free(param);

	return ret;
}

/**
 * aic_send_msg - send a request and wait for its confirmation
 * @hw: device
 * @param: parameter area returned by aic_msg_alloc(), freed on return
 * @need_cfm: wait for the confirmation before returning
 * @cfm_id: id of the expected confirmation
 * @cfm: buffer for the confirmation payload, may be %NULL
 * @cfm_len: size of @cfm
 */
int aic_send_msg(struct aic_hw *hw, const void *param, bool need_cfm,
		 u16 cfm_id, void *cfm, u16 cfm_len)
{
	return aic_send_msg_timeout(hw, param, need_cfm ? cfm_id : 0, cfm,
				    cfm_len, AIC_CMD_TIMEOUT_MS, true);
}

/**
 * aic_rx_handle_msg - process one message coming from the firmware
 * @hw: device
 * @buf: message, starting at the message header
 * @len: number of valid bytes in @buf
 */
void aic_rx_handle_msg(struct aic_hw *hw, const void *buf, unsigned int len)
{
	const struct aic_msg_ind *ind = buf;
	struct aic_cmd_mgr *mgr = &hw->cmd_mgr;
	struct aic_cmd *cmd;
	u16 param_len, id;
	bool found = false;

	if (len < sizeof(*ind)) {
		dev_warn(hw->dev, "short firmware message (%u bytes)\n", len);
		return;
	}

	id = le16_to_cpu(ind->id);
	param_len = le16_to_cpu(ind->param_len);
	if (param_len > len - sizeof(*ind) || param_len > AIC_MSG_PARAM_MAX) {
		dev_warn(hw->dev,
			 "firmware message %04x claims %u bytes of %u\n",
			 id, param_len, len);
		return;
	}

	spin_lock_bh(&mgr->lock);
	list_for_each_entry(cmd, &mgr->cmds, list) {
		if (cmd->cfm_id != id)
			continue;

		if (cmd->cfm)
			memcpy(cmd->cfm, ind->param,
			       min_t(u16, param_len, cmd->cfm_len));
		cmd->result = 0;
		list_del_init(&cmd->list);
		complete(&cmd->done);
		found = true;
		break;
	}
	spin_unlock_bh(&mgr->lock);

	if (!found)
		aic_rx_handle_event(hw, id, ind->param, param_len);
}

/**
 * aic_rx_handle_print - print a firmware log message
 * @hw: device
 * @buf: NUL terminated or length delimited string
 * @len: number of bytes in @buf
 */
void aic_rx_handle_print(struct aic_hw *hw, const void *buf, unsigned int len)
{
	dev_info(hw->dev, "fw: %*pEp\n", len, buf);
}
