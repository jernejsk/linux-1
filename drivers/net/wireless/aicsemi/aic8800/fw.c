// SPDX-License-Identifier: GPL-2.0-only
/*
 * Firmware download for AICSemi AIC8800 series wireless devices.
 *
 * The boot ROM already speaks the firmware message protocol, so the firmware
 * image is written into device memory with DBG_MEM_BLOCK_WRITE_REQ messages and
 * then started with DBG_START_APP_REQ.
 *
 * Copyright (C) 2026 Jernej Skrabec <jernej.skrabec@gmail.com>
 */

#include <linux/firmware.h>

#include "aic8800.h"

#define AIC_FW_DIR			"aicsemi/aic8800d80/"

/* Where the boot ROM expects the full MAC firmware. */
#define AIC_FW_RAM_ADDR			0x00120000
#define AIC_FW_RAM_ADDR_D80X2		0x00128000

/* Chip identification register. */
#define AIC_REG_CHIP_ID			0x40500000
#define AIC_CHIP_ID_REV			GENMASK(21, 16)
#define AIC_CHIP_ID_H			GENMASK(23, 22)

#define AIC_CHIP_REV_U01		1
#define AIC_CHIP_REV_U02		2
#define AIC_CHIP_REV_U03		3

/* Bytes per DBG_MEM_BLOCK_WRITE_REQ. */
#define AIC_FW_BLOCK_SIZE		1024

/*
 * Firmware configuration patch area.  The firmware image carries a pointer to
 * its configuration structure and to a descriptor the driver fills in with a
 * list of (offset, value) pairs to apply before the firmware runs.
 */
#define AIC_FW_CONFIG_PTR_OFFSET	0x0198
#define AIC_FW_VERSION_OFFSET		0x001c

#define AIC_PATCH_MAGIC			0x48435450	/* "PTCH" */
#define AIC_PATCH_MAGIC_2		0x50544348	/* "HCTP" */

#define AIC_PATCH_OFF_MAGIC		0x00
#define AIC_PATCH_OFF_PAIR_START	0x04
#define AIC_PATCH_OFF_MAGIC_2		0x08
#define AIC_PATCH_OFF_PAIR_COUNT	0x0c
#define AIC_PATCH_OFF_BLOCK_SIZE	0x30
#define AIC_PATCH_BLOCK_MAX		4

/* Firmware versions from this one on place the pair list themselves. */
#define AIC_FW_VERSION_PATCH_BUF	0x06090100

/* Default location of the pair list on older firmware. */
#define AIC_PATCH_PAIRS_ADDR		0x0016f800

/* Offsets into the firmware configuration structure. */
#define AIC_CFG_BAND_SUPPORT		0x00b4
#define AIC_CFG_USER_EXT_FLAGS		0x0188

#define AIC_BAND_SUPPORT_5G		0xf3010001
#define AIC_BAND_SUPPORT_2G_ONLY	0xf3010000

/* AIC_CFG_USER_EXT_FLAGS */
#define AIC_USER_PWROFST_COVER_CALIB	BIT(0)

struct aic_fw_pair {
	u32 offset;
	u32 value;
};

static u32 aic_fw_ram_addr(struct aic_hw *hw)
{
	if (hw->chip_id == AIC_CHIP_8800D80X2)
		return AIC_FW_RAM_ADDR_D80X2;

	return AIC_FW_RAM_ADDR;
}

static int aic_fw_read_chip_id(struct aic_hw *hw)
{
	u32 val;
	int ret;

	ret = aic_send_dbg_mem_read(hw, AIC_REG_CHIP_ID, &val);
	if (ret) {
		dev_err(hw->dev, "failed to read the chip id: %d\n", ret);
		return ret;
	}

	hw->chip_rev = FIELD_GET(AIC_CHIP_ID_REV, val);

	dev_info(hw->dev, "AIC8800D80%s revision u%02u (id %08x)\n",
		 FIELD_GET(AIC_CHIP_ID_H, val) == 3 ? "H" : "",
		 hw->chip_rev, val);

	return FIELD_GET(AIC_CHIP_ID_H, val) == 3;
}

static int aic_fw_upload(struct aic_hw *hw, u32 addr, const char *name)
{
	const struct firmware *fw;
	size_t off;
	int ret;

	ret = request_firmware(&fw, name, hw->dev);
	if (ret) {
		dev_err(hw->dev, "failed to load %s: %d\n", name, ret);
		return ret;
	}

	if (!fw->size || (fw->size & 3)) {
		dev_err(hw->dev, "%s has a bad size of %zu bytes\n", name,
			fw->size);
		ret = -EINVAL;
		goto out;
	}

	dev_info(hw->dev, "uploading %s (%zu bytes) to %08x\n", name, fw->size,
		 addr);

	for (off = 0; off < fw->size; off += AIC_FW_BLOCK_SIZE) {
		size_t len = min_t(size_t, AIC_FW_BLOCK_SIZE, fw->size - off);

		ret = aic_send_dbg_mem_block_write(hw, addr + off,
						   fw->data + off, len);
		if (ret) {
			dev_err(hw->dev, "upload of %s failed at %zu: %d\n",
				name, off, ret);
			goto out;
		}
	}

out:
	release_firmware(fw);

	return ret;
}

/*
 * Tell the firmware about the driver's configuration choices before starting
 * it.  The pairs are offsets into the firmware configuration structure, so the
 * base address has to be added to each of them.
 */
static int aic_fw_apply_config(struct aic_hw *hw)
{
	static const struct aic_fw_pair pairs[] = {
		{ AIC_CFG_BAND_SUPPORT, AIC_BAND_SUPPORT_5G },
		{ AIC_CFG_USER_EXT_FLAGS, AIC_USER_PWROFST_COVER_CALIB },
	};
	u32 base = aic_fw_ram_addr(hw);
	u32 config_base, patch_base, pairs_addr, version;
	int i, ret;

	ret = aic_send_dbg_mem_read(hw, base + AIC_FW_CONFIG_PTR_OFFSET,
				    &config_base);
	if (ret)
		return ret;

	ret = aic_send_dbg_mem_read(hw, base + AIC_FW_CONFIG_PTR_OFFSET + 8,
				    &patch_base);
	if (ret)
		return ret;

	ret = aic_send_dbg_mem_read(hw, base + AIC_FW_VERSION_OFFSET, &version);
	if (ret)
		return ret;

	dev_dbg(hw->dev, "firmware image version %08x\n", version);

	if (version > AIC_FW_VERSION_PATCH_BUF) {
		ret = aic_send_dbg_mem_read(hw,
					    base + AIC_FW_CONFIG_PTR_OFFSET + 12,
					    &pairs_addr);
		if (ret)
			return ret;
	} else {
		pairs_addr = AIC_PATCH_PAIRS_ADDR;
	}

	ret = aic_send_dbg_mem_write(hw, patch_base + AIC_PATCH_OFF_MAGIC,
				     AIC_PATCH_MAGIC);
	if (!ret)
		ret = aic_send_dbg_mem_write(hw,
					     patch_base + AIC_PATCH_OFF_MAGIC_2,
					     AIC_PATCH_MAGIC_2);
	if (!ret)
		ret = aic_send_dbg_mem_write(hw,
					patch_base + AIC_PATCH_OFF_PAIR_START,
					pairs_addr);
	if (!ret)
		ret = aic_send_dbg_mem_write(hw,
					patch_base + AIC_PATCH_OFF_PAIR_COUNT,
					ARRAY_SIZE(pairs));
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(pairs); i++) {
		ret = aic_send_dbg_mem_write(hw, pairs_addr + 8 * i,
					     pairs[i].offset + config_base);
		if (!ret)
			ret = aic_send_dbg_mem_write(hw, pairs_addr + 8 * i + 4,
						     pairs[i].value);
		if (ret)
			return ret;
	}

	/* the driver never uses the block copy feature of the patch area */
	for (i = 0; i < AIC_PATCH_BLOCK_MAX; i++) {
		ret = aic_send_dbg_mem_write(hw,
			patch_base + AIC_PATCH_OFF_BLOCK_SIZE + 4 * i, 0);
		if (ret)
			return ret;
	}

	return 0;
}

/**
 * aic_fw_load - bring the device firmware up
 * @hw: device
 *
 * Identifies the chip, uploads the matching firmware image, applies the
 * configuration patches and starts the firmware.
 */
int aic_fw_load(struct aic_hw *hw)
{
	u32 boot_status;
	int ret, chip_h;

	chip_h = aic_fw_read_chip_id(hw);
	if (chip_h < 0)
		return chip_h;

	ret = aic_fw_upload(hw, aic_fw_ram_addr(hw),
			    chip_h ? AIC_FW_DIR "fmacfw_8800d80_h_u02.bin"
				   : AIC_FW_DIR "fmacfw_8800d80_u02.bin");
	if (ret)
		return ret;

	ret = aic_fw_apply_config(hw);
	if (ret) {
		dev_err(hw->dev, "failed to configure the firmware: %d\n", ret);
		return ret;
	}

	ret = aic_send_dbg_start_app(hw, aic_fw_ram_addr(hw),
				     HOST_START_APP_AUTO, &boot_status);
	if (ret) {
		dev_err(hw->dev, "failed to start the firmware: %d\n", ret);
		return ret;
	}

	dev_dbg(hw->dev, "firmware started, boot status %08x\n", boot_status);

	return 0;
}

MODULE_FIRMWARE(AIC_FW_DIR "fmacfw_8800d80_u02.bin");
MODULE_FIRMWARE(AIC_FW_DIR "fmacfw_8800d80_h_u02.bin");
