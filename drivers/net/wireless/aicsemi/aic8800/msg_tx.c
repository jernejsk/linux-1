// SPDX-License-Identifier: GPL-2.0-only
/*
 * Requests sent to the AIC8800 firmware.
 *
 * Copyright (C) 2026 Jernej Skrabec <jernej.skrabec@gmail.com>
 */

#include "aic8800.h"

int aic_send_reset(struct aic_hw *hw)
{
	void *req;

	req = aic_msg_alloc(MM_RESET_REQ, TASK_MM, DRV_TASK_ID, 0);
	if (!req)
		return -ENOMEM;

	return aic_send_msg(hw, req, true, MM_RESET_CFM, NULL, 0);
}

int aic_send_version_req(struct aic_hw *hw)
{
	struct mm_version_cfm cfm = {};
	void *req;
	int ret;

	req = aic_msg_alloc(MM_VERSION_REQ, TASK_MM, DRV_TASK_ID, 0);
	if (!req)
		return -ENOMEM;

	ret = aic_send_msg(hw, req, true, MM_VERSION_CFM, &cfm, sizeof(cfm));
	if (ret)
		return ret;

	hw->fw.version_lmac = cfm.version_lmac;
	hw->fw.version_machw[0] = cfm.version_machw_1;
	hw->fw.version_machw[1] = cfm.version_machw_2;
	hw->fw.version_phy[0] = cfm.version_phy_1;
	hw->fw.version_phy[1] = cfm.version_phy_2;
	hw->fw.features = cfm.features;
	hw->fw.max_sta = cfm.max_sta_nb;

	dev_info(hw->dev,
		 "firmware %u.%u.%u.%u, features %08x, %u peers, %u interfaces\n",
		 (cfm.version_lmac >> 24) & 0xff, (cfm.version_lmac >> 16) & 0xff,
		 (cfm.version_lmac >> 8) & 0xff, cfm.version_lmac & 0xff,
		 cfm.features, cfm.max_sta_nb, cfm.max_vif_nb);

	return 0;
}

int aic_send_dbg_mem_read(struct aic_hw *hw, u32 addr, u32 *val)
{
	struct dbg_mem_read_req *req;
	struct dbg_mem_read_cfm cfm = {};
	int ret;

	req = aic_msg_alloc(DBG_MEM_READ_REQ, TASK_DBG, DRV_TASK_ID,
			    sizeof(*req));
	if (!req)
		return -ENOMEM;

	req->memaddr = addr;

	ret = aic_send_msg(hw, req, true, DBG_MEM_READ_CFM, &cfm, sizeof(cfm));
	if (ret)
		return ret;

	*val = cfm.memdata;

	return 0;
}

int aic_send_dbg_mem_write(struct aic_hw *hw, u32 addr, u32 val)
{
	struct dbg_mem_write_req *req;

	req = aic_msg_alloc(DBG_MEM_WRITE_REQ, TASK_DBG, DRV_TASK_ID,
			    sizeof(*req));
	if (!req)
		return -ENOMEM;

	req->memaddr = addr;
	req->memdata = val;

	return aic_send_msg(hw, req, true, DBG_MEM_WRITE_CFM, NULL, 0);
}

int aic_send_dbg_mem_mask_write(struct aic_hw *hw, u32 addr, u32 mask, u32 val)
{
	struct dbg_mem_mask_write_req *req;

	req = aic_msg_alloc(DBG_MEM_MASK_WRITE_REQ, TASK_DBG, DRV_TASK_ID,
			    sizeof(*req));
	if (!req)
		return -ENOMEM;

	req->memaddr = addr;
	req->memmask = mask;
	req->memdata = val;

	return aic_send_msg(hw, req, true, DBG_MEM_MASK_WRITE_CFM, NULL, 0);
}

/**
 * aic_send_dbg_mem_block_write - write a block of device memory
 * @hw: device
 * @addr: destination address
 * @data: source buffer
 * @len: number of bytes, at most %AIC_FW_BLOCK_SIZE
 *
 * The firmware expects the request to always carry the full parameter
 * structure, only @len bytes of which are meaningful.
 */
int aic_send_dbg_mem_block_write(struct aic_hw *hw, u32 addr, const void *data,
				 u32 len)
{
	struct dbg_mem_block_write_req *req;

	if (len > sizeof(req->memdata))
		return -EINVAL;

	req = aic_msg_alloc(DBG_MEM_BLOCK_WRITE_REQ, TASK_DBG, DRV_TASK_ID,
			    sizeof(*req));
	if (!req)
		return -ENOMEM;

	req->memaddr = addr;
	req->memsize = len;
	memcpy(req->memdata, data, len);

	return aic_send_msg(hw, req, true, DBG_MEM_BLOCK_WRITE_CFM, NULL, 0);
}

int aic_send_dbg_start_app(struct aic_hw *hw, u32 boot_addr, u32 boot_type,
			   u32 *boot_status)
{
	struct dbg_start_app_req *req;
	struct dbg_start_app_cfm cfm = {};
	int ret;

	req = aic_msg_alloc(DBG_START_APP_REQ, TASK_DBG, DRV_TASK_ID,
			    sizeof(*req));
	if (!req)
		return -ENOMEM;

	req->bootaddr = boot_addr;
	req->boottype = boot_type;

	ret = aic_send_msg(hw, req, true, DBG_START_APP_CFM, &cfm, sizeof(cfm));
	if (ret)
		return ret;

	if (boot_status)
		*boot_status = cfm.bootstatus;

	return 0;
}
