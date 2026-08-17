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

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/module.h>

#include "aic8800.h"

#define AIC_FW_DIR			"aicsemi/aic8800d80/"

/* Where the boot ROM expects the full MAC firmware. */
#define AIC_FW_RAM_ADDR			0x00120000
#define AIC_FW_RAM_ADDR_D80X2		0x00128000

/* Chip identification register. */
#define AIC_REG_CHIP_ID			0x40500000
#define AIC_CHIP_ID_REV			GENMASK(21, 16)
#define AIC_CHIP_ID_H			GENMASK(23, 22)

/* Revision numbers as they appear in the chip id, not consecutive. */
#define AIC_CHIP_REV_U01		1
#define AIC_CHIP_REV_U02		3
#define AIC_CHIP_REV_U04		7

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

	dev_info(hw->dev, "AIC8800D80%s revision %u (chip id %08x)\n",
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

/*
 * The Bluetooth controller in the same package runs on the same firmware
 * image, but needs a patch table of its own.  The table is a list of sections,
 * each holding pairs of a device address and the value to write there.
 */
#define AIC_BT_PT_TAG			"AICBT_PT_TAG"
#define AIC_BT_PT_TAG_LEN		16
#define AIC_BT_PT_NAME_LEN		16

enum aic_bt_pt_type {
	AIC_BT_PT_INF	= 0,
	AIC_BT_PT_TRAP	= 1,
	AIC_BT_PT_B4	= 2,
	AIC_BT_PT_BTMODE = 3,
	AIC_BT_PT_PWRON	= 4,
	AIC_BT_PT_AF	= 5,
	AIC_BT_PT_VER	= 6,
};

struct aic_bt_pt_hdr {
	char name[AIC_BT_PT_NAME_LEN];
	__le32 type;
	__le32 pairs;
} __packed;

/*
 * Fields of an %AIC_BT_PT_INF section, in pairs.  Only the second word of each
 * pair, the value, is used by the driver.
 */
enum aic_bt_inf_field {
	AIC_BT_INF_ADID_ADDR	= 0,
	AIC_BT_INF_PATCH_ADDR	= 1,
	AIC_BT_INF_RESET	= 2,
	AIC_BT_INF_ADID_FLAG	= 3,
	AIC_BT_INF_EXT_PATCH_NB	= 4,
	AIC_BT_INF_EXT_PATCH	= 5,
};

/* Fields of an %AIC_BT_PT_BTMODE section, again as pair indexes. */
enum aic_bt_mode_field {
	AIC_BT_MODE_HWINFO_AUTO	= 0,
	AIC_BT_MODE_HWINFO	= 1,
	AIC_BT_MODE_CPMODE	= 2,
	AIC_BT_MODE_BTMODE	= 3,
	AIC_BT_MODE_BTPORT	= 4,
	AIC_BT_MODE_UART_BAUD	= 5,
	AIC_BT_MODE_UART_FC	= 6,
	AIC_BT_MODE_LPM		= 7,
	AIC_BT_MODE_TXPWR	= 8,
	AIC_BT_MODE_FIELDS,
};

/* Bluetooth only operation, sharing the antenna with the wireless part. */
#define AIC_BT_MODE_BT_ONLY_COANT	5
/* The controller is reachable over the UART rather than over the mailbox. */
#define AIC_BT_PORT_UART		2
#define AIC_BT_UART_BAUD		1500000
#define AIC_BT_UART_FLOW_CTRL		1
/* Minimum and maximum transmit power level, one byte each. */
#define AIC_BT_TXPWR_LVL		0x00006f2f

/* Fixed addresses of the two patch areas, unless the table says otherwise. */
#define AIC_BT_ADID_ADDR		0x00201940
#define AIC_BT_PATCH_ADDR		0x0020b43c

static bool bluetooth = true;
module_param(bluetooth, bool, 0444);
MODULE_PARM_DESC(bluetooth, "load the firmware for the Bluetooth controller");

static u32 aic_bt_pair_value(const struct aic_bt_pt_hdr *hdr, unsigned int pair)
{
	const __le32 *pairs = (const __le32 *)(hdr + 1);

	return le32_to_cpu(pairs[2 * pair + 1]);
}

/* Write every pair of one section to the device. */
static int aic_bt_pt_apply(struct aic_hw *hw, const struct aic_bt_pt_hdr *hdr,
			   const u32 *btmode)
{
	const __le32 *pairs = (const __le32 *)(hdr + 1);
	unsigned int i, n = le32_to_cpu(hdr->pairs);
	int ret;

	for (i = 0; i < n; i++) {
		u32 addr = le32_to_cpu(pairs[2 * i]);
		u32 val = le32_to_cpu(pairs[2 * i + 1]);

		if (btmode && i < AIC_BT_MODE_FIELDS)
			val = btmode[i];

		ret = aic_send_dbg_mem_write(hw, addr, val);
		if (ret)
			return ret;
	}

	return 0;
}

/**
 * aic_bt_load - upload the Bluetooth patches and configuration
 * @hw: device
 * @rev: firmware file revision suffix
 *
 * Has to run before the wireless firmware is started, and only does anything if
 * the wireless firmware image also carries the Bluetooth stack.
 */
static int aic_bt_load(struct aic_hw *hw, const char *rev)
{
	/*
	 * Configuration written into the %AIC_BT_PT_BTMODE section.  The
	 * hardware information word is left for the firmware to detect.
	 */
	static const u32 btmode[AIC_BT_MODE_FIELDS] = {
		[AIC_BT_MODE_HWINFO_AUTO]	= 1,
		[AIC_BT_MODE_HWINFO]		= 0xffffffff,
		[AIC_BT_MODE_CPMODE]		= 0,
		[AIC_BT_MODE_BTMODE]		= AIC_BT_MODE_BT_ONLY_COANT,
		[AIC_BT_MODE_BTPORT]		= AIC_BT_PORT_UART,
		[AIC_BT_MODE_UART_BAUD]		= AIC_BT_UART_BAUD,
		[AIC_BT_MODE_UART_FC]		= AIC_BT_UART_FLOW_CTRL,
		[AIC_BT_MODE_LPM]		= 0,
		[AIC_BT_MODE_TXPWR]		= AIC_BT_TXPWR_LVL,
	};
	u32 adid_addr = AIC_BT_ADID_ADDR, patch_addr = AIC_BT_PATCH_ADDR;
	u32 ext_patch_nb = 0, ext_patch[8][2];
	const struct firmware *table;
	const struct aic_bt_pt_hdr *hdr;
	char name[64];
	size_t off;
	unsigned int i;
	int ret;

	snprintf(name, sizeof(name), "%sfw_patch_table_8800d80_%s.bin",
		 AIC_FW_DIR, rev);
	ret = request_firmware(&table, name, hw->dev);
	if (ret) {
		dev_err(hw->dev, "failed to load %s: %d\n", name, ret);
		return ret;
	}

	if (table->size < AIC_BT_PT_TAG_LEN ||
	    memcmp(table->data, AIC_BT_PT_TAG, strlen(AIC_BT_PT_TAG))) {
		dev_err(hw->dev, "%s is not a Bluetooth patch table\n", name);
		ret = -EINVAL;
		goto out;
	}

	/*
	 * First pass: pick up the addresses the patch binaries have to be
	 * uploaded to.
	 */
	for (off = AIC_BT_PT_TAG_LEN; off + sizeof(*hdr) <= table->size;) {
		unsigned int pairs;
		size_t len;

		hdr = (const struct aic_bt_pt_hdr *)(table->data + off);
		pairs = le32_to_cpu(hdr->pairs);
		/* section types above this are not patch data */
		if (le32_to_cpu(hdr->type) >= 1000)
			pairs = 0;

		len = sizeof(*hdr) + 8 * pairs;
		if (off + len > table->size) {
			dev_err(hw->dev, "%s is truncated\n", name);
			ret = -EINVAL;
			goto out;
		}

		if (le32_to_cpu(hdr->type) == AIC_BT_PT_INF) {
			if (pairs > AIC_BT_INF_ADID_ADDR)
				adid_addr = aic_bt_pair_value(hdr,
							AIC_BT_INF_ADID_ADDR);
			if (pairs > AIC_BT_INF_PATCH_ADDR)
				patch_addr = aic_bt_pair_value(hdr,
							AIC_BT_INF_PATCH_ADDR);
			if (pairs > AIC_BT_INF_EXT_PATCH_NB)
				ext_patch_nb = aic_bt_pair_value(hdr,
						AIC_BT_INF_EXT_PATCH_NB);

			ext_patch_nb = min_t(u32, ext_patch_nb,
					     ARRAY_SIZE(ext_patch));
			if (pairs < AIC_BT_INF_EXT_PATCH + 2 * ext_patch_nb)
				ext_patch_nb = 0;

			for (i = 0; i < ext_patch_nb; i++) {
				const __le32 *p = (const __le32 *)(hdr + 1);

				p += 2 * AIC_BT_INF_EXT_PATCH + 2 * i;
				ext_patch[i][0] = le32_to_cpu(p[0]);
				ext_patch[i][1] = le32_to_cpu(p[1]);
			}
		}

		off += len;
	}

	snprintf(name, sizeof(name), "%sfw_adid_8800d80_u02.bin", AIC_FW_DIR);
	ret = aic_fw_upload(hw, adid_addr, name);
	if (ret)
		goto out;

	snprintf(name, sizeof(name), "%sfw_patch_8800d80_%s.bin", AIC_FW_DIR,
		 rev);
	ret = aic_fw_upload(hw, patch_addr, name);
	if (ret)
		goto out;

	for (i = 0; i < ext_patch_nb; i++) {
		snprintf(name, sizeof(name),
			 "%sfw_patch_8800d80_%s_ext%u.bin", AIC_FW_DIR, rev,
			 ext_patch[i][0]);
		ret = aic_fw_upload(hw, ext_patch[i][1], name);
		if (ret)
			goto out;
	}

	/* Second pass: apply the patches themselves. */
	for (off = AIC_BT_PT_TAG_LEN; off + sizeof(*hdr) <= table->size;) {
		unsigned int pairs, type;

		hdr = (const struct aic_bt_pt_hdr *)(table->data + off);
		type = le32_to_cpu(hdr->type);
		pairs = le32_to_cpu(hdr->pairs);
		if (type >= 1000)
			pairs = 0;

		switch (type) {
		case AIC_BT_PT_VER:
			dev_info(hw->dev, "Bluetooth patch version %.*s\n",
				 8 * pairs, (const char *)(hdr + 1));
			break;
		case AIC_BT_PT_BTMODE:
			if (pairs < AIC_BT_MODE_FIELDS) {
				dev_err(hw->dev,
					"short Bluetooth mode section\n");
				ret = -EINVAL;
				goto out;
			}
			ret = aic_bt_pt_apply(hw, hdr, btmode);
			break;
		default:
			ret = aic_bt_pt_apply(hw, hdr, NULL);
			/* the firmware needs time to bring its radio up */
			if (!ret && type == AIC_BT_PT_PWRON)
				msleep(100);
			break;
		}
		if (ret)
			goto out;

		off += sizeof(*hdr) + 8 * pairs;
	}

out:
	release_firmware(table);

	return ret;
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
	const char *rev;
	char name[64];
	int ret, chip_h;

	chip_h = aic_fw_read_chip_id(hw);
	if (chip_h < 0)
		return chip_h;

	rev = hw->chip_rev >= AIC_CHIP_REV_U04 ? "u04" : "u02";

	if (bluetooth) {
		ret = aic_bt_load(hw, rev);
		if (ret)
			return ret;
	}

	snprintf(name, sizeof(name), "%sfmacfw%s_8800d80%s_u02.bin",
		 AIC_FW_DIR, bluetooth ? "bt" : "", chip_h ? "_h" : "");

	ret = aic_fw_upload(hw, aic_fw_ram_addr(hw), name);
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
MODULE_FIRMWARE(AIC_FW_DIR "fmacfwbt_8800d80_u02.bin");
MODULE_FIRMWARE(AIC_FW_DIR "fmacfwbt_8800d80_h_u02.bin");
MODULE_FIRMWARE(AIC_FW_DIR "fw_adid_8800d80_u02.bin");
MODULE_FIRMWARE(AIC_FW_DIR "fw_patch_8800d80_u02.bin");
MODULE_FIRMWARE(AIC_FW_DIR "fw_patch_table_8800d80_u02.bin");
MODULE_FIRMWARE(AIC_FW_DIR "fw_patch_8800d80_u04.bin");
MODULE_FIRMWARE(AIC_FW_DIR "fw_patch_table_8800d80_u04.bin");
