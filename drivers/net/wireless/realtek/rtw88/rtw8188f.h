/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/* Copyright(c) 2016 - 2017 Realtek Corporation
 * Copyright(c) 2026  Jernej Skrabec <jernej.skrabec@gmail.com>
 */

#ifndef __RTW8188F_H__
#define __RTW8188F_H__

#include "main.h"

extern const struct rtw_chip_info rtw8188f_hw_spec;

struct rtw8188fu_efuse {
	u8 res4[48];			/* 0xd0 */
	u8 vendor_id[2];		/* 0x100 */
	u8 product_id[2];		/* 0x102 */
	u8 usb_option;			/* 0x104 */
	u8 res5[2];			/* 0x105 */
	u8 mac_addr[ETH_ALEN];		/* 0x107 */
} __packed;

struct rtw8188fs_efuse {
	u8 res4[0x4a];			/* 0xd0 */
	u8 mac_addr[ETH_ALEN];		/* 0x11a */
} __packed;

/* The layout matches the vendor driver's PG offsets for 8188F in
 * include/hal_pg.h, and happens to be identical to the 8723x one.
 */
struct rtw8188f_efuse {
	__le16 rtl_id;
	u8 rsvd[2];
	u8 afe;
	u8 rsvd1[11];

	/* power index for four RF paths */
	struct rtw_txpwr_idx txpwr_idx_table[4];

	u8 channel_plan;		/* 0xb8 */
	u8 xtal_k;			/* 0xb9 */
	u8 thermal_meter;		/* 0xba */
	u8 iqk_lck;			/* 0xbb */
	u8 pa_type;			/* 0xbc */
	u8 lna_type_2g[2];		/* 0xbd */
	u8 lna_type_5g[2];		/* 0xbf */
	u8 rf_board_option;		/* 0xc1 */
	u8 rf_feature_option;		/* 0xc2 */
	u8 rf_bt_setting;		/* 0xc3 */
	u8 eeprom_version;		/* 0xc4 */
	u8 eeprom_customer_id;		/* 0xc5 */
	u8 tx_bb_swing_setting_2g;	/* 0xc6 */
	u8 res_c7;
	u8 tx_pwr_calibrate_rate;	/* 0xc8 */
	u8 rf_antenna_option;		/* 0xc9 */
	u8 rfe_option;			/* 0xca */
	u8 country_code[2];		/* 0xcb */
	u8 res[3];
	union {
		struct rtw8188fu_efuse u;
		struct rtw8188fs_efuse s;
	};
} __packed;

/* phy status parsing */
#define VGA_BITS GENMASK(4, 0)
#define LNA_BITS GENMASK(7, 5)

struct phy_rx_agc_info {
#ifdef __LITTLE_ENDIAN
	u8 gain: 7;
	u8 trsw: 1;
#else
	u8 trsw: 1;
	u8 gain: 7;
#endif
} __packed;

/* Called phy_status_rpt_8192cd in the vendor driver, shared by all
 * chips of this generation (see rtw8703b.h for the 8703B copy).
 */
struct phy_status_8188f {
	struct phy_rx_agc_info path_agc[2];
	u8 ch_corr[2];
	u8 cck_sig_qual_ofdm_pwdb_all;
	/* for CCK: bits 0:4: VGA index, bits 5:7: LNA index */
	u8 cck_agc_rpt_ofdm_cfosho_a;
	u8 cck_rpt_b_ofdm_cfosho_b;
	u8 reserved_1;
	u8 noise_power_db_msb;
	s8 path_cfotail[2];
	u8 pcts_mask[2];
	s8 stream_rxevm[2];
	u8 path_rxsnr[2];
	u8 noise_power_db_lsb;
	u8 reserved_2[3];
	u8 stream_csi[2];
	u8 stream_target_csi[2];
	s8 sig_evm;
	u8 reserved_3;

#ifdef __LITTLE_ENDIAN
	u8 antsel_rx_keep_2: 1;
	u8 sgi_en: 1;
	u8 rxsc: 2;
	u8 idle_long: 1;
	u8 r_ant_train_en: 1;
	u8 ant_sel_b: 1;
	u8 ant_sel: 1;
#else
	u8 ant_sel: 1;
	u8 ant_sel_b: 1;
	u8 r_ant_train_en: 1;
	u8 idle_long: 1;
	u8 rxsc: 2;
	u8 sgi_en: 1;
	u8 antsel_rx_keep_2: 1;
#endif
} __packed;

/* MAC registers */
/* The crystal cap for 8188F lives in 0x24[22:11], not in 0x2c like the
 * 8723x family: vendor phydm_set_crystal_cap_reg() writes
 * 0x24[22:17] = 0x24[16:11] = crystal_cap.
 */
#define BIT_MASK_XTAL_8188F	0x007FF800
#define REG_SECONDARY_CCA_CTRL	0x0577
#define REG_2ND_CCA_RELATED	0x0976
#define REG_CTX			0x0d03
#define BIT_MASK_CTX_TYPE	GENMASK(6, 4)

#define BIT_CFENDFORM		BIT(9)
#define BIT_WMAC_TCR_ERR0	BIT(12)
#define BIT_WMAC_TCR_ERR1	BIT(13)
#define BIT_TCR_CFG		(BIT_CFENDFORM | BIT_WMAC_TCR_ERR0 | \
				 BIT_WMAC_TCR_ERR1)

/* False alarm counters. These are generic 11N baseband registers; the
 * 8723x driver keeps an identical copy in rtw8723x.h.
 */
#define REG_CCK_FA_RST_11N	0x0a2c
#define BIT_MASK_CCK_CNT_KEEP	BIT(12)
#define BIT_MASK_CCK_CNT_EN	BIT(13)
#define BIT_MASK_CCK_CNT_KPEN	(BIT_MASK_CCK_CNT_KEEP | BIT_MASK_CCK_CNT_EN)
#define BIT_MASK_CCK_FA_KEEP	BIT(14)
#define BIT_MASK_CCK_FA_EN	BIT(15)
#define BIT_MASK_CCK_FA_KPEN	(BIT_MASK_CCK_FA_KEEP | BIT_MASK_CCK_FA_EN)
#define REG_CCK_FA_MSB_11N	0x0a58
#define REG_CCK_FA_LSB_11N	0x0a5c
#define REG_CCK_CCA_CNT_11N	0x0a60
#define BIT_MASK_CCK_FA_MSB	GENMASK(7, 0)
#define BIT_MASK_CCK_FA_LSB	GENMASK(15, 8)
#define REG_OFDM_FA_HOLDC_11N	0x0c00
#define BIT_MASK_OFDM_FA_KEEP	BIT(31)
#define REG_OFDM_FA_RSTC_11N	0x0c0c
#define BIT_MASK_OFDM_FA_RST	BIT(31)
#define REG_OFDM_FA_TYPE1_11N	0x0cf0
#define BIT_MASK_OFDM_FF_CNT	GENMASK(15, 0)
#define BIT_MASK_OFDM_SF_CNT	GENMASK(31, 16)
#define REG_OFDM_FA_RSTD_11N	0x0d00
#define BIT_MASK_OFDM_FA_RST1	BIT(27)
#define BIT_MASK_OFDM_FA_KEEP1	BIT(31)
#define REG_OFDM_FA_TYPE2_11N	0x0da0
#define BIT_MASK_OFDM_CCA_CNT	GENMASK(15, 0)
#define BIT_MASK_OFDM_PF_CNT	GENMASK(31, 16)
#define REG_OFDM_FA_TYPE3_11N	0x0da4
#define BIT_MASK_OFDM_RI_CNT	GENMASK(15, 0)
#define BIT_MASK_OFDM_CRC_CNT	GENMASK(31, 16)
#define REG_OFDM_FA_TYPE4_11N	0x0da8
#define BIT_MASK_OFDM_MNS_CNT	GENMASK(15, 0)
#define REG_PAGE_F_RST_11N	0x0f14
#define BIT_MASK_F_RST_ALL	BIT(16)
#define REG_IGI_C_11N		0x0f84
#define REG_IGI_D_11N		0x0f88
#define REG_HT_CRC32_CNT_11N	0x0f90
#define BIT_MASK_HT_CRC_OK	GENMASK(15, 0)
#define BIT_MASK_HT_CRC_ERR	GENMASK(31, 16)
#define REG_OFDM_CRC32_CNT_11N	0x0f94
#define BIT_MASK_OFDM_LCRC_OK	GENMASK(15, 0)
#define BIT_MASK_OFDM_LCRC_ERR	GENMASK(31, 16)

/* Baseband registers */
#define REG_FPGA1_RFMOD		0x0900
#define REG_CCK0_SYSTEM		0x0a00
#define BIT_CCK_SIDE_BAND	BIT(4)
#define REG_BB_RX_DFIR		0x0954
#define REG_S0S1_PATH_SWITCH	0x0948
#define REG_FPGA0_ANAPARAM4	0x0088
#define REG_FPGA0_PSD_FUNC	0x0808
#define REG_FPGA0_PSD_REPORT	0x0f58
#define REG_OFDM0_XAAGC1	0x0c50
#define REG_OFDM0_XA_RX_AFE	0x0c10
#define REG_OFDM0_TX_PSD_NOISE	0x0ce4
#define REG_OFDM0_XA_TX_AFE	0x0c84
#define REG_OFDM0_ECCA_THRES	0x0c4c
#define REG_NOTCH_CTRL		0x0c40
#define BIT_MASK_NOTCH_IDX	GENMASK(28, 24)
#define BIT_NOTCH_EN		BIT(9)
#define REG_CSI_MASK_0		0x0d40
#define REG_CSI_MASK_1		0x0d44
#define REG_CSI_MASK_2		0x0d48
#define REG_CSI_MASK_3		0x0d4c
#define REG_CSI_MASK_EN		0x0d2c
#define BIT_CSI_MASK_EN		BIT(28)

/* 0xaXX: CCK TX power registers used by power tracking */
#define REG_CCK_TXSF2		0x0a24
#define REG_CCK_DBG		0x0a28

/* RF registers */
#define RF_WLINT		0x01
#define RF_TRX_BW		0x18
#define BIT_LCK			BIT(15)
#define RF_RC_CORNER_B		0x1b
#define RF_FILTER_BW		0x1c
#define RF_FILTER_RC		0x87
#define RF_RC_CORNER		0xdf

#define AGG_BURST_NUM		3
#define AGG_BURST_SIZE		0 /* 1K */
#define BIT_MASK_AGG_BURST_SIZE	GENMASK(5, 4)

#endif /* __RTW8188F_H__ */
