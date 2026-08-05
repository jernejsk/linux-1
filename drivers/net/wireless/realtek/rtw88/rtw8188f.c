// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/* Copyright(c) 2016 - 2017 Realtek Corporation
 * Copyright(c) 2026  Jernej Skrabec <jernej.skrabec@gmail.com>
 *
 * Support for the RTL8188F chip, used by the RTL8189FS/RTL8189FTV SDIO
 * module. Derived from the vendor driver (rtl8189ES_linux, rtl8189fs
 * branch, hal/rtl8188f and hal/phydm/{,halrf/}rtl8188f) with rtw8703b.c
 * as the structural template: same 1T1R 2.4 GHz 11n generation, same
 * 8051 firmware interface, same SDIO front end. Unlike 8703B/8723D this
 * chip has no Bluetooth, so all coexistence hooks are left out.
 */

#include <linux/of_net.h>
#include "main.h"
#include "coex.h"
#include "debug.h"
#include "efuse.h"
#include "mac.h"
#include "phy.h"
#include "reg.h"
#include "rx.h"
#include "tx.h"
#include "rtw8188f.h"
#include "rtw8188f_table.h"

#define BIT_MASK_TXQ_INIT (BIT(7))
#define WLAN_RL_VAL 0x3030
/* disable BAR */
#define WLAN_BAR_VAL 0x0201ffff
#define WLAN_PIFS_VAL 0
#define WLAN_RX_PKT_LIMIT 0x18
#define WLAN_SLOT_TIME 0x09
#define WLAN_SPEC_SIFS 0x100a
#define WLAN_MAX_AGG_NR 0x1f
#define WLAN_AMPDU_MAX_TIME 0x70
#define WLAN_ACK_TO 0x40
#define WLAN_USTIME 0x28

/* vendor _InitWMACSetting(): RCR_APM | RCR_AM | RCR_AB | RCR_CBSSID_DATA |
 * RCR_CBSSID_BCN | RCR_AMF | RCR_HTC_LOC_CTRL | RCR_APP_PHYST_RXFF |
 * RCR_APP_ICV | RCR_APP_MIC. Same value the 8723x family uses.
 */
#define WLAN_RCR_CFG 0x700060CE
#define WLAN_RX_FILTER0 0xFFFF
#define WLAN_RX_FILTER1 0x400
#define WLAN_RX_FILTER2 0xFFFF
#define WLAN_TXQ_RPT_EN 0x1F

/* unit is 32us */
#define TBTT_PROHIBIT_SETUP_TIME 0x04
#define TBTT_PROHIBIT_HOLD_TIME_STOP_BCN 0x64

#define TRANS_SEQ_END			\
	0xFFFF,				\
	RTW_PWR_CUT_ALL_MSK,		\
	RTW_PWR_INTF_ALL_MSK,		\
	0,				\
	RTW_PWR_CMD_END, 0, 0

/* The chip has no Bluetooth, but the core still walks the coex tables on
 * the "wifi only" path, so provide minimal ones (same approach as
 * rtw8814a.c). rtw8188f_read_efuse() forces efuse->btcoex to false.
 */
static const u8 wl_rssi_step_8188f[] = {60, 50, 44, 30};
static const u8 bt_rssi_step_8188f[] = {30, 30, 30, 30};
static const struct coex_5g_afh_map afh_5g_8188f[] = { {0, 0, 0} };

static const struct coex_rf_para rf_para_tx_8188f[] = {
	{0, 0, false, 7},  /* for normal */
	{0, 10, false, 7}, /* for WL-CPT */
	{1, 0, true, 4},
	{1, 2, true, 4},
	{1, 10, true, 4},
	{1, 15, true, 4}
};

static const struct coex_rf_para rf_para_rx_8188f[] = {
	{0, 0, false, 7},  /* for normal */
	{0, 10, false, 7}, /* for WL-CPT */
	{1, 0, true, 5},
	{1, 2, true, 5},
	{1, 10, true, 5},
	{1, 15, true, 5}
};

static const struct rtw_hw_reg rtw8188f_txagc[] = {
	[DESC_RATE1M]	= { .addr = 0xe08, .mask = 0x0000ff00 },
	[DESC_RATE2M]	= { .addr = 0x86c, .mask = 0x0000ff00 },
	[DESC_RATE5_5M]	= { .addr = 0x86c, .mask = 0x00ff0000 },
	[DESC_RATE11M]	= { .addr = 0x86c, .mask = 0xff000000 },
	[DESC_RATE6M]	= { .addr = 0xe00, .mask = 0x000000ff },
	[DESC_RATE9M]	= { .addr = 0xe00, .mask = 0x0000ff00 },
	[DESC_RATE12M]	= { .addr = 0xe00, .mask = 0x00ff0000 },
	[DESC_RATE18M]	= { .addr = 0xe00, .mask = 0xff000000 },
	[DESC_RATE24M]	= { .addr = 0xe04, .mask = 0x000000ff },
	[DESC_RATE36M]	= { .addr = 0xe04, .mask = 0x0000ff00 },
	[DESC_RATE48M]	= { .addr = 0xe04, .mask = 0x00ff0000 },
	[DESC_RATE54M]	= { .addr = 0xe04, .mask = 0xff000000 },
	[DESC_RATEMCS0]	= { .addr = 0xe10, .mask = 0x000000ff },
	[DESC_RATEMCS1]	= { .addr = 0xe10, .mask = 0x0000ff00 },
	[DESC_RATEMCS2]	= { .addr = 0xe10, .mask = 0x00ff0000 },
	[DESC_RATEMCS3]	= { .addr = 0xe10, .mask = 0xff000000 },
	[DESC_RATEMCS4]	= { .addr = 0xe14, .mask = 0x000000ff },
	[DESC_RATEMCS5]	= { .addr = 0xe14, .mask = 0x0000ff00 },
	[DESC_RATEMCS6]	= { .addr = 0xe14, .mask = 0x00ff0000 },
	[DESC_RATEMCS7]	= { .addr = 0xe14, .mask = 0xff000000 },
};

/* Shared across this chip generation, from the vendor's
 * halrf_powertracking_ce.c ofdm_swing_table_new[].
 */
static const u32 rtw8188f_ofdm_swing_table[] = {
	0x0b40002d, /* 0,  -15.0dB */
	0x0c000030, /* 1,  -14.5dB */
	0x0cc00033, /* 2,  -14.0dB */
	0x0d800036, /* 3,  -13.5dB */
	0x0e400039, /* 4,  -13.0dB */
	0x0f00003c, /* 5,  -12.5dB */
	0x10000040, /* 6,  -12.0dB */
	0x11000044, /* 7,  -11.5dB */
	0x12000048, /* 8,  -11.0dB */
	0x1300004c, /* 9,  -10.5dB */
	0x14400051, /* 10, -10.0dB */
	0x15800056, /* 11, -9.5dB */
	0x16c0005b, /* 12, -9.0dB */
	0x18000060, /* 13, -8.5dB */
	0x19800066, /* 14, -8.0dB */
	0x1b00006c, /* 15, -7.5dB */
	0x1c800072, /* 16, -7.0dB */
	0x1e400079, /* 17, -6.5dB */
	0x20000080, /* 18, -6.0dB */
	0x22000088, /* 19, -5.5dB */
	0x24000090, /* 20, -5.0dB */
	0x26000098, /* 21, -4.5dB */
	0x288000a2, /* 22, -4.0dB */
	0x2ac000ab, /* 23, -3.5dB */
	0x2d4000b5, /* 24, -3.0dB */
	0x300000c0, /* 25, -2.5dB */
	0x32c000cb, /* 26, -2.0dB */
	0x35c000d7, /* 27, -1.5dB */
	0x390000e4, /* 28, -1.0dB */
	0x3c8000f2, /* 29, -0.5dB */
	0x40000100, /* 30, +0dB */
	0x43c0010f, /* 31, +0.5dB */
	0x47c0011f, /* 32, +1.0dB */
	0x4c000130, /* 33, +1.5dB */
	0x50800142, /* 34, +2.0dB */
	0x55400155, /* 35, +2.5dB */
	0x5a400169, /* 36, +3.0dB */
	0x5fc0017f, /* 37, +3.5dB */
	0x65400195, /* 38, +4.0dB */
	0x6b8001ae, /* 39, +4.5dB */
	0x71c001c7, /* 40, +5.0dB */
	0x788001e2, /* 41, +5.5dB */
	0x7f8001fe  /* 42, +6.0dB */
};

static const u32 rtw8188f_cck_pwr_regs[] = {
	0x0a22, 0x0a23, 0x0a24, 0x0a25, 0x0a26, 0x0a27, 0x0a28, 0x0a29,
	0x0a9a, 0x0a9b, 0x0a9c, 0x0a9d, 0x0aa0, 0x0aa1, 0x0aa2, 0x0aa3,
};

/* vendor cck_swing_table_ch1_ch13_88f[][16] */
static const u8 rtw8188f_cck_swing_table[][16] = {
	{0x44, 0x42, 0x3C, 0x33, 0x28, 0x1C, 0x13, 0x0B, 0x05, 0x02,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*-16dB*/
	{0x48, 0x46, 0x3F, 0x36, 0x2A, 0x1E, 0x14, 0x0B, 0x05, 0x02,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*-15.5dB*/
	{0x4D, 0x4A, 0x43, 0x39, 0x2C, 0x20, 0x15, 0x0C, 0x06, 0x02,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*-15dB*/
	{0x51, 0x4F, 0x47, 0x3C, 0x2F, 0x22, 0x16, 0x0D, 0x06, 0x02,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*-14.5dB*/
	{0x56, 0x53, 0x4B, 0x40, 0x32, 0x24, 0x17, 0x0E, 0x06, 0x02,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*-14dB*/
	{0x5B, 0x58, 0x50, 0x43, 0x35, 0x26, 0x19, 0x0E, 0x07, 0x02,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*-13.5dB*/
	{0x60, 0x5D, 0x54, 0x47, 0x38, 0x28, 0x1A, 0x0F, 0x07, 0x02,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*-13dB*/
	{0x66, 0x63, 0x59, 0x4C, 0x3B, 0x2B, 0x1C, 0x10, 0x08, 0x02,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*-12.5dB*/
	{0x6C, 0x69, 0x5F, 0x50, 0x3F, 0x2D, 0x1E, 0x11, 0x08, 0x03,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*-12dB*/
	{0x73, 0x6F, 0x64, 0x55, 0x42, 0x30, 0x1F, 0x12, 0x08, 0x03,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*-11.5dB*/
	{0x79, 0x76, 0x6A, 0x5A, 0x46, 0x33, 0x21, 0x13, 0x09, 0x03,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*-11dB*/
	{0x81, 0x7C, 0x71, 0x5F, 0x4A, 0x36, 0x23, 0x14, 0x0A, 0x03,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*-10.5dB*/
	{0x88, 0x84, 0x77, 0x65, 0x4F, 0x39, 0x25, 0x15, 0x0A, 0x03,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*-10dB*/
	{0x90, 0x8C, 0x7E, 0x6B, 0x54, 0x3C, 0x27, 0x17, 0x0B, 0x03,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*-9.5dB*/
	{0x99, 0x94, 0x86, 0x71, 0x58, 0x40, 0x2A, 0x18, 0x0B, 0x04,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*-9dB*/
	{0xA2, 0x9D, 0x8E, 0x78, 0x5E, 0x43, 0x2C, 0x19, 0x0C, 0x04,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*-8.5dB*/
	{0xAC, 0xA6, 0x96, 0x7F, 0x63, 0x47, 0x2F, 0x1B, 0x0D, 0x04,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*-8dB*/
	{0xB6, 0xB0, 0x9F, 0x87, 0x69, 0x4C, 0x32, 0x1D, 0x0D, 0x04,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*-7.5dB*/
	{0xC1, 0xBA, 0xA8, 0x8F, 0x6F, 0x50, 0x35, 0x1E, 0x0E, 0x04,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*-7dB*/
	{0xCC, 0xC5, 0xB2, 0x97, 0x76, 0x55, 0x38, 0x20, 0x0F, 0x05,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*-6.5dB*/
	{0xD8, 0xD1, 0xBD, 0xA0, 0x7D, 0x5A, 0x3B, 0x22, 0x10, 0x05,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}  /*-6dB*/
};

/* vendor cck_swing_table_ch14_88f[][16] */
static const u8 rtw8188f_cck_swing_table_ch14[][16] = {
	{0x44, 0x42, 0x3C, 0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*-16dB*/
	{0x48, 0x46, 0x3F, 0x2A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*-15.5dB*/
	{0x4D, 0x4A, 0x43, 0x2C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*-15dB*/
	{0x51, 0x4F, 0x47, 0x2F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*-14.5dB*/
	{0x56, 0x53, 0x4B, 0x32, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*-14dB*/
	{0x5B, 0x58, 0x50, 0x35, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*-13.5dB*/
	{0x60, 0x5D, 0x54, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*-13dB*/
	{0x66, 0x63, 0x59, 0x3B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*-12.5dB*/
	{0x6C, 0x69, 0x5F, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*-12dB*/
	{0x73, 0x6F, 0x64, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*-11.5dB*/
	{0x79, 0x76, 0x6A, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*-11dB*/
	{0x81, 0x7C, 0x71, 0x4A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*-10.5dB*/
	{0x88, 0x84, 0x77, 0x4F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*-10dB*/
	{0x90, 0x8C, 0x7E, 0x54, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*-9.5dB*/
	{0x99, 0x94, 0x86, 0x58, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*-9dB*/
	{0xA2, 0x9D, 0x8E, 0x5E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*-8.5dB*/
	{0xAC, 0xA6, 0x96, 0x63, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*-8dB*/
	{0xB6, 0xB0, 0x9F, 0x69, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*-7.5dB*/
	{0xC1, 0xBA, 0xA8, 0x6F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*-7dB*/
	{0xCC, 0xC5, 0xB2, 0x76, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*-6.5dB*/
	{0xD8, 0xD1, 0xBD, 0x7D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}  /*-6dB*/
};

#define RTW_OFDM_SWING_TABLE_SIZE	ARRAY_SIZE(rtw8188f_ofdm_swing_table)
#define RTW_CCK_SWING_TABLE_SIZE	ARRAY_SIZE(rtw8188f_cck_swing_table)

/* Power sequences, from the vendor's Hal8188FPwrSeq.h. Only the
 * transitions the core needs are listed; the SDIO-only rows are kept
 * with their interface masks so the tables stay faithful to the source.
 */
static const struct rtw_pwr_seq_cmd trans_carddis_to_cardemu_8188f[] = {
	{0x0086,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_SDIO,
	 RTW_PWR_CMD_WRITE, BIT(0), 0},
	{0x0086,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_SDIO,
	 RTW_PWR_CMD_POLLING, BIT(1), BIT(1)},
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(3) | BIT(4), 0},
	{TRANS_SEQ_END},
};

static const struct rtw_pwr_seq_cmd trans_cardemu_to_act_8188f[] = {
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(2), 0},
	{0x0006,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_POLLING, BIT(1), BIT(1)},
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(7), 0},
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(3), 0},
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(0), BIT(0)},
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_POLLING, BIT(0), 0},
	/* 0x27 <= 0x35 to reduce RF noise */
	{0x0027,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, 0xff, 0x35},
	{TRANS_SEQ_END},
};

static const struct rtw_pwr_seq_cmd trans_act_to_cardemu_8188f[] = {
	/* turn off RF */
	{0x001F,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, 0xFF, 0},
	/* 0x4C[23] = 0, switch DPDT_SEL_P output from register 0x65[2] */
	{0x004E,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(7), 0},
	/* xtal_qsel = 0 for xtal bring up */
	{0x0027,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, 0xff, 0x34},
	/* turn off MAC by HW state machine */
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(1), BIT(1)},
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_POLLING, BIT(1), 0},
	{TRANS_SEQ_END},
};

static const struct rtw_pwr_seq_cmd trans_cardemu_to_carddis_8188f[] = {
	/* SOP option to disable BG/MB */
	{0x0007,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, 0xFF, 0x00},
	/* enable WL suspend */
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK | RTW_PWR_INTF_USB_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(3) | BIT(4), BIT(3)},
	{0x0086,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_SDIO,
	 RTW_PWR_CMD_WRITE, BIT(0), BIT(0)},
	{0x0086,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_SDIO,
	 RTW_PWR_CMD_POLLING, BIT(1), 0},
	{TRANS_SEQ_END},
};

static const struct rtw_pwr_seq_cmd * const card_enable_flow_8188f[] = {
	trans_carddis_to_cardemu_8188f,
	trans_cardemu_to_act_8188f,
	NULL
};

static const struct rtw_pwr_seq_cmd * const card_disable_flow_8188f[] = {
	trans_act_to_cardemu_8188f,
	trans_cardemu_to_carddis_8188f,
	NULL
};

/* HPQ/NPQ/LPQ page counts from NORMAL_PAGE_NUM_{HPQ,NPQ,LPQ}_8188F.
 * Only entry 0 (SDIO) is reachable for this chip, the rest are copies so
 * the core's per-bus indexing stays in range.
 */
static const struct rtw_page_table page_table_8188f[] = {
	{12, 2, 2, 0, 1},
	{12, 2, 2, 0, 1},
	{12, 2, 2, 0, 1},
	{12, 2, 2, 0, 1},
	{12, 2, 2, 0, 1},
};

/* vendor _InitNormalChipThreeOutEpPriority(), non-WMM case:
 * BE/BK -> LOW, VI -> NORMAL, VO/MGT/HI -> HIGH
 */
static const struct rtw_rqpn rqpn_table_8188f[] = {
	{RTW_DMA_MAPPING_HIGH, RTW_DMA_MAPPING_NORMAL,
	 RTW_DMA_MAPPING_LOW, RTW_DMA_MAPPING_LOW,
	 RTW_DMA_MAPPING_HIGH, RTW_DMA_MAPPING_HIGH},
	{RTW_DMA_MAPPING_HIGH, RTW_DMA_MAPPING_NORMAL,
	 RTW_DMA_MAPPING_LOW, RTW_DMA_MAPPING_LOW,
	 RTW_DMA_MAPPING_HIGH, RTW_DMA_MAPPING_HIGH},
	{RTW_DMA_MAPPING_HIGH, RTW_DMA_MAPPING_NORMAL,
	 RTW_DMA_MAPPING_LOW, RTW_DMA_MAPPING_LOW,
	 RTW_DMA_MAPPING_HIGH, RTW_DMA_MAPPING_HIGH},
	{RTW_DMA_MAPPING_HIGH, RTW_DMA_MAPPING_NORMAL,
	 RTW_DMA_MAPPING_LOW, RTW_DMA_MAPPING_LOW,
	 RTW_DMA_MAPPING_HIGH, RTW_DMA_MAPPING_HIGH},
	{RTW_DMA_MAPPING_HIGH, RTW_DMA_MAPPING_NORMAL,
	 RTW_DMA_MAPPING_LOW, RTW_DMA_MAPPING_LOW,
	 RTW_DMA_MAPPING_HIGH, RTW_DMA_MAPPING_HIGH},
};

static const struct rtw_prioq_addrs prioq_addrs_8188f = {
	.prio[RTW_DMA_MAPPING_EXTRA] = {
		.rsvd = REG_RQPN_NPQ + 2, .avail = REG_RQPN_NPQ + 3,
	},
	.prio[RTW_DMA_MAPPING_LOW] = {
		.rsvd = REG_RQPN + 1, .avail = REG_FIFOPAGE_CTRL_2 + 1,
	},
	.prio[RTW_DMA_MAPPING_NORMAL] = {
		.rsvd = REG_RQPN_NPQ, .avail = REG_RQPN_NPQ + 1,
	},
	.prio[RTW_DMA_MAPPING_HIGH] = {
		.rsvd = REG_RQPN, .avail = REG_FIFOPAGE_CTRL_2,
	},
	.wsize = false,
};

static const struct rtw_hw_reg dig_8188f[] = {
	[0] = { .addr = REG_OFDM0_XAAGC1, .mask = 0x7f },
	[1] = { .addr = REG_OFDM0_XAAGC1, .mask = 0x7f },
};

static const struct rtw_hw_reg dig_cck_8188f[] = {
	[0] = { .addr = 0xa0c, .mask = 0x3f00 },
};

static const struct rtw_rf_sipi_addr rf_sipi_addr_8188f[] = {
	[RF_PATH_A] = { .hssi_1 = 0x820, .lssi_read    = 0x8a0,
			.hssi_2 = 0x824, .lssi_read_pi = 0x8b8},
	[RF_PATH_B] = { .hssi_1 = 0x828, .lssi_read    = 0x8a4,
			.hssi_2 = 0x82c, .lssi_read_pi = 0x8bc},
};

static void try_mac_from_devicetree(struct rtw_dev *rtwdev)
{
	struct device_node *node = rtwdev->dev->of_node;
	struct rtw_efuse *efuse = &rtwdev->efuse;
	int ret;

	if (node) {
		ret = of_get_mac_address(node, efuse->addr);
		if (ret == 0)
			rtw_dbg(rtwdev, RTW_DBG_EFUSE,
				"got wifi mac address from DT: %pM\n",
				efuse->addr);
	}
}

static int rtw8188f_read_efuse(struct rtw_dev *rtwdev, u8 *log_map)
{
	struct rtw_efuse *efuse = &rtwdev->efuse;
	struct rtw8188f_efuse *map;
	int i;

	map = (struct rtw8188f_efuse *)log_map;

	efuse->rfe_option = 0;
	efuse->rf_board_option = map->rf_board_option;
	efuse->crystal_cap = map->xtal_k;
	efuse->pa_type_2g = map->pa_type;
	efuse->lna_type_2g = map->lna_type_2g[0];
	efuse->channel_plan = map->channel_plan;
	efuse->country_code[0] = map->country_code[0];
	efuse->country_code[1] = map->country_code[1];
	efuse->bt_setting = map->rf_bt_setting;
	efuse->regd = map->rf_board_option & 0x7;
	efuse->thermal_meter[RF_PATH_A] = map->thermal_meter;
	efuse->thermal_meter_k = map->thermal_meter;
	efuse->afe = map->afe;

	for (i = 0; i < 4; i++)
		efuse->txpwr_idx_table[i] = map->txpwr_idx_table[i];

	switch (rtw_hci_type(rtwdev)) {
	case RTW_HCI_TYPE_SDIO:
		ether_addr_copy(efuse->addr, map->s.mac_addr);
		break;
	case RTW_HCI_TYPE_USB:
		ether_addr_copy(efuse->addr, map->u.mac_addr);
		break;
	default:
		/* unsupported now */
		return -EOPNOTSUPP;
	}

	if (!is_valid_ether_addr(efuse->addr))
		try_mac_from_devicetree(rtwdev);

	/* RTL8188F is a WiFi-only part, there is no Bluetooth to coexist
	 * with. The core derives btcoex from rf_board_option, which is
	 * meaningless here, so force the "wifi only" path.
	 */
	efuse->btcoex = false;

	/* Regd is derived from rf_board_option and should be 0 if there
	 * is no valid data.
	 */
	if (efuse->rf_board_option == 0xff) {
		efuse->regd = 0;
		efuse->rf_board_option &= GENMASK(5, 0);
	}

	/* Default from the vendor driver. */
	if (efuse->crystal_cap == 0xff)
		efuse->crystal_cap = 0x20;

	return 0;
}

static void rtw8188f_cfg_ldo25(struct rtw_dev *rtwdev, bool enable)
{
	u8 ldo_pwr;

	ldo_pwr = rtw_read8(rtwdev, REG_LDO_EFUSE_CTRL + 3);
	if (enable) {
		ldo_pwr &= ~BIT_MASK_LDO25_VOLTAGE;
		ldo_pwr |= (BIT_LDO25_VOLTAGE_V25 << 4) | BIT_LDO25_EN;
	} else {
		ldo_pwr &= ~BIT_LDO25_EN;
	}
	rtw_write8(rtwdev, REG_LDO_EFUSE_CTRL + 3, ldo_pwr);
}

static void rtw8188f_efuse_grant(struct rtw_dev *rtwdev, bool on)
{
	if (on) {
		rtw_write8(rtwdev, REG_EFUSE_ACCESS, EFUSE_ACCESS_ON);

		rtw_write16_set(rtwdev, REG_SYS_FUNC_EN, BIT_FEN_ELDR);
		rtw_write16_set(rtwdev, REG_SYS_CLKR,
				BIT_LOADER_CLK_EN | BIT_ANA8M);
	} else {
		rtw_write8(rtwdev, REG_EFUSE_ACCESS, EFUSE_ACCESS_OFF);
	}
}

static int rtw8188f_mac_init(struct rtw_dev *rtwdev)
{
	rtw_write32(rtwdev, REG_TCR, BIT_TCR_CFG);

	rtw_write16(rtwdev, REG_RXFLTMAP0, WLAN_RX_FILTER0);
	rtw_write16(rtwdev, REG_RXFLTMAP1, WLAN_RX_FILTER1);
	rtw_write16(rtwdev, REG_RXFLTMAP2, WLAN_RX_FILTER2);
	rtw_write32(rtwdev, REG_RCR, WLAN_RCR_CFG);

	rtw_write32(rtwdev, REG_INT_MIG, 0);
	rtw_write32(rtwdev, REG_MCUTST_1, 0x0);

	/* The vendor driver's "YJ,TODO" CCA block writes 0x3 to 0x577 and 0
	 * to 0x976, which is exactly REG_MISC_CTRL/BIT_DIS_SECOND_CCA and
	 * REG_2ND_CCA_CTRL in rtw88 terms.
	 */
	rtw_write8(rtwdev, REG_MISC_CTRL, BIT_DIS_SECOND_CCA);
	rtw_write8(rtwdev, REG_2ND_CCA_CTRL, 0);

	/* vendor _InitRDGSetting() */
	rtw_write8(rtwdev, REG_RD_CTRL, 0xFF);
	rtw_write16(rtwdev, REG_RD_NAV_NXT, 0x200);

	return 0;
}

static int rtw8188f_mac_postinit(struct rtw_dev *rtwdev)
{
	rtw_write8(rtwdev, REG_FWHW_TXQ_CTRL + 1, WLAN_TXQ_RPT_EN);

	return 0;
}

static void rtw8188f_pwrtrack_init(struct rtw_dev *rtwdev)
{
	struct rtw_dm_info *dm_info = &rtwdev->dm_info;
	u8 path;

	dm_info->default_ofdm_index = 30;
	dm_info->default_cck_index = 20;

	for (path = RF_PATH_A; path < rtwdev->hal.rf_path_num; path++) {
		ewma_thermal_init(&dm_info->avg_thermal[path]);
		dm_info->delta_power_index[path] = 0;
	}
	dm_info->pwr_trk_triggered = false;
	dm_info->pwr_trk_init_trigger = true;
	dm_info->thermal_meter_k = rtwdev->efuse.thermal_meter_k;
	dm_info->txagc_remnant_cck = 0;
	dm_info->txagc_remnant_ofdm[RF_PATH_A] = 0;
}

static void rtw8188f_phy_set_param(struct rtw_dev *rtwdev)
{
	u8 xtal_cap = rtwdev->efuse.crystal_cap & 0x3F;

	/* power on BB/RF domain, vendor PHY_BBConfig8188F() */
	rtw_write16_set(rtwdev, REG_SYS_FUNC_EN,
			BIT_FEN_EN_25_1 | BIT_FEN_BB_GLB_RST | BIT_FEN_BB_RSTB);
	rtw_write8(rtwdev, REG_RF_CTRL,
		   BIT_RF_EN | BIT_RF_RSTB | BIT_RF_SDM_RSTB);
	usleep_range(10, 20);
	rtw_write_rf(rtwdev, RF_PATH_A, RF_WLINT, RFREG_MASK, 0x780);

	rtw_phy_load_tables(rtwdev);

	/* crystal cap: 0x24[22:17] = 0x24[16:11] = xtal_cap */
	rtw_write32_mask(rtwdev, REG_AFE_XTAL_CTRL, BIT_MASK_XTAL_8188F,
			 xtal_cap | (xtal_cap << 6));

	rtw_write32_clr(rtwdev, REG_RCR, BIT_RCR_ADF);
	rtw_write8_set(rtwdev, REG_HIQ_NO_LMT_EN, 0xff);

	/* Init EDCA, vendor _InitEDCA() */
	rtw_write16(rtwdev, REG_SPEC_SIFS, WLAN_SPEC_SIFS);
	rtw_write16(rtwdev, REG_MAC_SPEC_SIFS, WLAN_SPEC_SIFS);
	rtw_write16(rtwdev, REG_SIFS, WLAN_SPEC_SIFS); /* CCK */
	rtw_write16(rtwdev, REG_SIFS + 2, WLAN_SPEC_SIFS); /* OFDM */
	rtw_write32(rtwdev, REG_EDCA_VO_PARAM, 0x002FA226);
	rtw_write32(rtwdev, REG_EDCA_VI_PARAM, 0x005EA324);
	rtw_write32(rtwdev, REG_EDCA_BE_PARAM, 0x005EA42B);
	rtw_write32(rtwdev, REG_EDCA_BK_PARAM, 0x0000A44F);
	rtw_write8(rtwdev, REG_USTIME_EDCA, WLAN_USTIME);
	rtw_write8(rtwdev, REG_USTIME_TSF, WLAN_USTIME);

	/* Init retry, vendor _InitRetryFunction() */
	rtw_write8(rtwdev, REG_ACKTO, WLAN_ACK_TO);

	/* RX aggregation burst parameters; sdio.c sets the DMA mode and
	 * the size/timeout thresholds.
	 */
	rtw_write8(rtwdev, REG_RXDMA_MODE,
		   BIT_DMA_MODE |
		   FIELD_PREP_CONST(BIT_DMA_BURST_CNT, AGG_BURST_NUM) |
		   FIELD_PREP_CONST(BIT_MASK_AGG_BURST_SIZE, AGG_BURST_SIZE));

	/* Init beacon parameters */
	rtw_write8(rtwdev, REG_BCN_CTRL,
		   BIT_DIS_TSF_UDT | BIT_EN_BCN_FUNCTION | BIT_EN_TXBCN_RPT);
	rtw_write8(rtwdev, REG_TBTT_PROHIBIT, TBTT_PROHIBIT_SETUP_TIME);
	rtw_write8(rtwdev, REG_TBTT_PROHIBIT + 1,
		   TBTT_PROHIBIT_HOLD_TIME_STOP_BCN & 0xFF);
	rtw_write8(rtwdev, REG_TBTT_PROHIBIT + 2,
		   (rtw_read8(rtwdev, REG_TBTT_PROHIBIT + 2) & 0xF0)
		   | (TBTT_PROHIBIT_HOLD_TIME_STOP_BCN >> 8));

	/* configure packet burst */
	rtw_write8_set(rtwdev, REG_SINGLE_AMPDU_CTRL, BIT_EN_SINGLE_APMDU);
	rtw_write8(rtwdev, REG_RX_PKT_LIMIT, WLAN_RX_PKT_LIMIT);
	rtw_write8(rtwdev, REG_MAX_AGGR_NUM, WLAN_MAX_AGG_NR);
	rtw_write8(rtwdev, REG_PIFS, WLAN_PIFS_VAL);
	rtw_write8_clr(rtwdev, REG_FWHW_TXQ_CTRL, BIT_MASK_TXQ_INIT);
	rtw_write8(rtwdev, REG_AMPDU_MAX_TIME, WLAN_AMPDU_MAX_TIME);

	rtw_write8(rtwdev, REG_SLOT, WLAN_SLOT_TIME);
	rtw_write16(rtwdev, REG_RETRY_LIMIT, WLAN_RL_VAL);
	rtw_write32(rtwdev, REG_BAR_MODE_CTRL, WLAN_BAR_VAL);
	rtw_write16(rtwdev, REG_ATIMWND, 0x2);

	/* vendor BBTurnOnBlock_8188F() */
	rtw_write32_set(rtwdev, REG_FPGA0_RFMOD, BIT_CCKEN | BIT_OFDMEN);

	rtw_phy_init(rtwdev);

	/* Unlike 8703B/8723D/8710B, the 8188F CCK AGC report always uses a
	 * 3 bit LNA index (vendor phydm_cck_new_agc_chk() has no 8188F
	 * case, and phydm_phy_sts_n_parsing() only extends the index for
	 * those other chips).
	 */
	rtwdev->dm_info.rx_cck_agc_report_type = 0;

	rtw8188f_pwrtrack_init(rtwdev);
}

static void rtw8188f_set_channel_rf(struct rtw_dev *rtwdev, u8 channel, u8 bw)
{
	u32 rf_cfgch;

	rf_cfgch = rtw_read_rf(rtwdev, RF_PATH_A, RF_CFGCH, RFREG_MASK);
	rf_cfgch &= ~GENMASK(7, 0);
	rf_cfgch |= channel;

	/* vendor PHY_RF6052SetBandwidth8188F() */
	rf_cfgch &= ~GENMASK(11, 10);
	switch (bw) {
	case RTW_CHANNEL_WIDTH_20:
		rf_cfgch |= BIT(10) | BIT(11);
		break;
	case RTW_CHANNEL_WIDTH_40:
		rf_cfgch |= BIT(10);
		break;
	default:
		break;
	}
	rtw_write_rf(rtwdev, RF_PATH_A, RF_TRX_BW, RFREG_MASK, rf_cfgch);

	switch (bw) {
	case RTW_CHANNEL_WIDTH_20:
		rtw_write_rf(rtwdev, RF_PATH_A, RF_FILTER_RC, RFREG_MASK,
			     0x00065);
		rtw_write_rf(rtwdev, RF_PATH_A, RF_FILTER_BW, RFREG_MASK,
			     0x00000);
		break;
	case RTW_CHANNEL_WIDTH_40:
		rtw_write_rf(rtwdev, RF_PATH_A, RF_FILTER_RC, RFREG_MASK,
			     0x00025);
		rtw_write_rf(rtwdev, RF_PATH_A, RF_FILTER_BW, RFREG_MASK,
			     0x00800);
		break;
	default:
		break;
	}
	rtw_write_rf(rtwdev, RF_PATH_A, RF_RC_CORNER, RFREG_MASK, 0x00140);
	rtw_write_rf(rtwdev, RF_PATH_A, RF_RC_CORNER_B, RFREG_MASK, 0x00C6C);
}

/* Spur calibration, vendor phy_SpurCalibration_8188F(). Measures the
 * spur level with the PSD engine on the channels where the chip is known
 * to have one, and enables the notch filter and CSI mask if the level is
 * above the threshold.
 */
#define SPUR_THRESHOLD 0x16

static const u16 spur_freq_8188f[] = {
	/* channel 5, 6, 7, 8, 13, 14, 11 */
	0xFCCD, 0xFC4D, 0xFFCD, 0xFF4D, 0xFCCD, 0xFF9A, 0xFDCD,
};

static const u8 spur_chan_8188f[] = { 5, 6, 7, 8, 13, 14, 11 };

struct rtw8188f_notch {
	u8 idx;
	u32 csi_mask[4];
};

static const struct rtw8188f_notch notch_8188f[] = {
	{  5, { 0x06000000, 0, 0, 0 } },
	{  4, { 0x00000600, 0, 0, 0 } },
	{  3, { 0, 0, 0, 0 } },
	{ 10, { 0, 0, 0, 0x00000380 } },
	{ 11, { 0x06000000, 0, 0, 0 } },
	{  5, { 0, 0, 0, 0x00180000 } },
	{ 25, { 0, 0x04000000, 0, 0 } },
};

static void rtw8188f_notch_disable(struct rtw_dev *rtwdev)
{
	rtw_write32_mask(rtwdev, REG_NOTCH_CTRL, BIT_NOTCH_EN, 0);
	rtw_write32_mask(rtwdev, REG_CSI_MASK_EN, BIT_CSI_MASK_EN, 0);
}

static void rtw8188f_spur_calibration(struct rtw_dev *rtwdev, u8 channel)
{
	const struct rtw8188f_notch *notch;
	u8 initial_gain;
	u32 reg948;
	int idx;

	for (idx = 0; idx < ARRAY_SIZE(spur_chan_8188f); idx++)
		if (spur_chan_8188f[idx] == channel)
			break;

	if (idx == ARRAY_SIZE(spur_chan_8188f)) {
		rtw8188f_notch_disable(rtwdev);
		return;
	}

	/* Only do this while WiFi uses path S1: either HW controlled with
	 * 0x864[5:3] == 1, or SW controlled with 0x948[9] == 0.
	 */
	reg948 = rtw_read32(rtwdev, REG_S0S1_PATH_SWITCH);
	if (reg948 & BIT(6)) {
		if (rtw_read32_mask(rtwdev, REG_FPGA0_XB_RF_INT_OE,
				    GENMASK(5, 3)) != 1) {
			rtw8188f_notch_disable(rtwdev);
			return;
		}
	} else if (reg948 & BIT(9)) {
		rtw8188f_notch_disable(rtwdev);
		return;
	}

	initial_gain = rtw_read32_mask(rtwdev, REG_OFDM0_XAAGC1, MASKBYTE0) & 0x7f;

	rtw_write32_mask(rtwdev, REG_FPGA0_RFMOD, BIT(24), 0);
	rtw_phy_dig_write(rtwdev, 0x30);
	rtw_write32(rtwdev, REG_FPGA0_ANAPARAM4, 0xccf000c0);

	rtw_write32(rtwdev, REG_FPGA0_PSD_FUNC, spur_freq_8188f[idx]);
	rtw_write32(rtwdev, REG_FPGA0_PSD_FUNC,
		    0x400000 | spur_freq_8188f[idx]);

	msleep(30);

	if (rtw_read32(rtwdev, REG_FPGA0_PSD_REPORT) < SPUR_THRESHOLD) {
		rtw_write32(rtwdev, REG_FPGA0_PSD_FUNC, spur_freq_8188f[idx]);
		rtw_write32(rtwdev, REG_FPGA0_ANAPARAM4, 0xccc000c0);
		rtw_write32_mask(rtwdev, REG_FPGA0_RFMOD, BIT(24), 1);
		rtw_phy_dig_write(rtwdev, initial_gain);
		rtw8188f_notch_disable(rtwdev);
		return;
	}

	rtw_write32(rtwdev, REG_FPGA0_PSD_FUNC, spur_freq_8188f[idx]);
	rtw_write32(rtwdev, REG_FPGA0_ANAPARAM4, 0xccc000c0);
	rtw_write32_mask(rtwdev, REG_FPGA0_RFMOD, BIT(24), 1);
	rtw_phy_dig_write(rtwdev, initial_gain);

	notch = &notch_8188f[idx];
	rtw_write32_mask(rtwdev, REG_NOTCH_CTRL, BIT_MASK_NOTCH_IDX,
			 notch->idx);
	rtw_write32_mask(rtwdev, REG_NOTCH_CTRL, BIT_NOTCH_EN, 1);
	rtw_write32(rtwdev, REG_CSI_MASK_0, notch->csi_mask[0]);
	rtw_write32(rtwdev, REG_CSI_MASK_1, notch->csi_mask[1]);
	rtw_write32(rtwdev, REG_CSI_MASK_2, notch->csi_mask[2]);
	rtw_write32(rtwdev, REG_CSI_MASK_3, notch->csi_mask[3]);
	rtw_write32_mask(rtwdev, REG_CSI_MASK_EN, BIT_CSI_MASK_EN, 1);
}

/* vendor phy_PostSetBwMode8188F() */
static void rtw8188f_set_channel_bb(struct rtw_dev *rtwdev, u8 bw,
				    u8 primary_ch_idx)
{
	rtw_write32_mask(rtwdev, REG_FPGA0_RFMOD, GENMASK(10, 8), 0x7);
	rtw_write32_mask(rtwdev, REG_FPGA0_RFMOD, GENMASK(14, 12), 0x5);
	rtw_write32_mask(rtwdev, REG_OFDM0_TX_PSD_NOISE, GENMASK(31, 30), 0x0);
	rtw_write32_mask(rtwdev, REG_OFDM0_TX_PSD_NOISE, GENMASK(29, 28), 0x1);
	rtw_write32_mask(rtwdev, REG_OFDM0_XA_RX_AFE, GENMASK(29, 28), 0x1);
	rtw_write32_mask(rtwdev, REG_BB_RX_DFIR, BIT(19), 0x0);

	switch (bw) {
	case RTW_CHANNEL_WIDTH_20:
		rtw_write32_mask(rtwdev, REG_FPGA0_RFMOD, BIT(0), 0x0);
		rtw_write32_mask(rtwdev, REG_FPGA1_RFMOD, BIT(0), 0x0);
		rtw_write32_mask(rtwdev, REG_BB_RX_DFIR, GENMASK(23, 20), 0x3);
		break;
	case RTW_CHANNEL_WIDTH_40:
		rtw_write32_mask(rtwdev, REG_FPGA0_RFMOD, BIT(0), 0x1);
		rtw_write32_mask(rtwdev, REG_FPGA1_RFMOD, BIT(0), 0x1);
		/* 0x6 for ACPR */
		rtw_write32_mask(rtwdev, REG_BB_RX_DFIR, GENMASK(23, 20), 0x6);
		/* primary channel (CCK RXSC) */
		rtw_write32_mask(rtwdev, REG_CCK0_SYSTEM, BIT_CCK_SIDE_BAND,
				 primary_ch_idx == RTW_SC_20_UPPER ? 1 : 0);
		rtw_write32_mask(rtwdev, REG_RRSR, GENMASK(22, 21), 0x0);
		break;
	default:
		break;
	}
}

static void rtw8188f_set_channel(struct rtw_dev *rtwdev, u8 channel, u8 bw,
				 u8 primary_chan_idx)
{
	rtw8188f_set_channel_rf(rtwdev, channel, bw);
	rtw_set_channel_mac(rtwdev, channel, bw, primary_chan_idx);
	rtw8188f_set_channel_bb(rtwdev, bw, primary_chan_idx);
	rtw8188f_spur_calibration(rtwdev, channel);
}

/* vendor phydm_cck_rssi_8188f() */
static s8 get_cck_rx_pwr(struct rtw_dev *rtwdev, u8 lna_idx, u8 vga_idx)
{
	switch (lna_idx) {
	case 7:
		if (vga_idx <= 27)
			return -100 + 2 * (27 - vga_idx);
		return -100;
	case 5:
		return -74 + 2 * (21 - vga_idx);
	case 3:
		return -60 + 2 * (20 - vga_idx);
	case 1:
		return -44 + 2 * (19 - vga_idx);
	default:
		rtw_dbg(rtwdev, RTW_DBG_RFK, "unexpected lna index (%d)\n",
			lna_idx);
		return -120;
	}
}

static void query_phy_status_cck(struct rtw_dev *rtwdev, u8 *phy_raw,
				 struct rtw_rx_pkt_stat *pkt_stat)
{
	struct phy_status_8188f *phy_status = (struct phy_status_8188f *)phy_raw;
	u8 cck_agc_rpt = phy_status->cck_agc_rpt_ofdm_cfosho_a;
	u8 vga_idx = cck_agc_rpt & VGA_BITS;
	u8 lna_idx = FIELD_GET(LNA_BITS, cck_agc_rpt);
	s8 rx_power;

	rx_power = get_cck_rx_pwr(rtwdev, lna_idx, vga_idx);

	pkt_stat->rx_power[RF_PATH_A] = rx_power;
	pkt_stat->rssi = rtw_phy_rf_power_2_rssi(pkt_stat->rx_power, 1);
	rtwdev->dm_info.rssi[RF_PATH_A] = pkt_stat->rssi;
	pkt_stat->signal_power = rx_power;
}

static void query_phy_status_ofdm(struct rtw_dev *rtwdev, u8 *phy_raw,
				  struct rtw_rx_pkt_stat *pkt_stat)
{
	struct phy_status_8188f *phy_status = (struct phy_status_8188f *)phy_raw;
	struct rtw_dm_info *dm_info = &rtwdev->dm_info;
	s8 val_s8;

	val_s8 = phy_status->path_agc[RF_PATH_A].gain & 0x3F;
	pkt_stat->rx_power[RF_PATH_A] = (val_s8 * 2) - 110;
	pkt_stat->rssi = rtw_phy_rf_power_2_rssi(pkt_stat->rx_power, 1);
	pkt_stat->rx_snr[RF_PATH_A] = (s8)(phy_status->path_rxsnr[RF_PATH_A] / 2);

	/* signal power reported by HW */
	val_s8 = phy_status->cck_sig_qual_ofdm_pwdb_all >> 1;
	pkt_stat->signal_power = (val_s8 & 0x7f) - 110;

	pkt_stat->rx_evm[RF_PATH_A] = phy_status->stream_rxevm[RF_PATH_A];
	pkt_stat->cfo_tail[RF_PATH_A] = phy_status->path_cfotail[RF_PATH_A];

	dm_info->curr_rx_rate = pkt_stat->rate;
	dm_info->rssi[RF_PATH_A] = pkt_stat->rssi;
	dm_info->rx_snr[RF_PATH_A] = pkt_stat->rx_snr[RF_PATH_A] >> 1;
	/* convert to KHz (used only for debugfs) */
	dm_info->cfo_tail[RF_PATH_A] = (pkt_stat->cfo_tail[RF_PATH_A] * 5) >> 1;

	/* (EVM value as s8 / 2) is dbm, should usually be in -33 to 0
	 * range. rx_evm_dbm needs the absolute (positive) value.
	 */
	val_s8 = (s8)pkt_stat->rx_evm[RF_PATH_A];
	val_s8 = clamp_t(s8, -val_s8 >> 1, 0, 64);
	val_s8 &= 0x3F; /* 64->0: second path of 1SS rate is 64 */
	dm_info->rx_evm_dbm[RF_PATH_A] = val_s8;
}

static void query_phy_status(struct rtw_dev *rtwdev, u8 *phy_status,
			     struct rtw_rx_pkt_stat *pkt_stat)
{
	if (pkt_stat->rate <= DESC_RATE11M)
		query_phy_status_cck(rtwdev, phy_status, pkt_stat);
	else
		query_phy_status_ofdm(rtwdev, phy_status, pkt_stat);
}

static void rtw8188f_false_alarm_statistics(struct rtw_dev *rtwdev)
{
	struct rtw_dm_info *dm_info = &rtwdev->dm_info;
	u32 cck_fa_cnt;
	u32 ofdm_fa_cnt;
	u32 crc32_cnt;
	u32 val32;

	/* hold counter */
	rtw_write32_mask(rtwdev, REG_OFDM_FA_HOLDC_11N, BIT_MASK_OFDM_FA_KEEP, 1);
	rtw_write32_mask(rtwdev, REG_OFDM_FA_RSTD_11N, BIT_MASK_OFDM_FA_KEEP1, 1);
	rtw_write32_mask(rtwdev, REG_CCK_FA_RST_11N, BIT_MASK_CCK_CNT_KEEP, 1);
	rtw_write32_mask(rtwdev, REG_CCK_FA_RST_11N, BIT_MASK_CCK_FA_KEEP, 1);

	cck_fa_cnt = rtw_read32_mask(rtwdev, REG_CCK_FA_LSB_11N, MASKBYTE0);
	cck_fa_cnt += rtw_read32_mask(rtwdev, REG_CCK_FA_MSB_11N, MASKBYTE3) << 8;

	val32 = rtw_read32(rtwdev, REG_OFDM_FA_TYPE1_11N);
	ofdm_fa_cnt = u32_get_bits(val32, BIT_MASK_OFDM_FF_CNT);
	ofdm_fa_cnt += u32_get_bits(val32, BIT_MASK_OFDM_SF_CNT);
	val32 = rtw_read32(rtwdev, REG_OFDM_FA_TYPE2_11N);
	dm_info->ofdm_cca_cnt = u32_get_bits(val32, BIT_MASK_OFDM_CCA_CNT);
	ofdm_fa_cnt += u32_get_bits(val32, BIT_MASK_OFDM_PF_CNT);
	val32 = rtw_read32(rtwdev, REG_OFDM_FA_TYPE3_11N);
	ofdm_fa_cnt += u32_get_bits(val32, BIT_MASK_OFDM_RI_CNT);
	ofdm_fa_cnt += u32_get_bits(val32, BIT_MASK_OFDM_CRC_CNT);
	val32 = rtw_read32(rtwdev, REG_OFDM_FA_TYPE4_11N);
	ofdm_fa_cnt += u32_get_bits(val32, BIT_MASK_OFDM_MNS_CNT);

	dm_info->cck_fa_cnt = cck_fa_cnt;
	dm_info->ofdm_fa_cnt = ofdm_fa_cnt;
	dm_info->total_fa_cnt = cck_fa_cnt + ofdm_fa_cnt;

	dm_info->cck_err_cnt = rtw_read32(rtwdev, REG_IGI_C_11N);
	dm_info->cck_ok_cnt = rtw_read32(rtwdev, REG_IGI_D_11N);
	crc32_cnt = rtw_read32(rtwdev, REG_OFDM_CRC32_CNT_11N);
	dm_info->ofdm_err_cnt = u32_get_bits(crc32_cnt, BIT_MASK_OFDM_LCRC_ERR);
	dm_info->ofdm_ok_cnt = u32_get_bits(crc32_cnt, BIT_MASK_OFDM_LCRC_OK);
	crc32_cnt = rtw_read32(rtwdev, REG_HT_CRC32_CNT_11N);
	dm_info->ht_err_cnt = u32_get_bits(crc32_cnt, BIT_MASK_HT_CRC_ERR);
	dm_info->ht_ok_cnt = u32_get_bits(crc32_cnt, BIT_MASK_HT_CRC_OK);
	dm_info->vht_err_cnt = 0;
	dm_info->vht_ok_cnt = 0;

	val32 = rtw_read32(rtwdev, REG_CCK_CCA_CNT_11N);
	dm_info->cck_cca_cnt = (u32_get_bits(val32, BIT_MASK_CCK_FA_MSB) << 8) |
			       u32_get_bits(val32, BIT_MASK_CCK_FA_LSB);
	dm_info->total_cca_cnt = dm_info->cck_cca_cnt + dm_info->ofdm_cca_cnt;

	/* reset counter */
	rtw_write32_mask(rtwdev, REG_OFDM_FA_RSTC_11N, BIT_MASK_OFDM_FA_RST, 1);
	rtw_write32_mask(rtwdev, REG_OFDM_FA_RSTC_11N, BIT_MASK_OFDM_FA_RST, 0);
	rtw_write32_mask(rtwdev, REG_OFDM_FA_RSTD_11N, BIT_MASK_OFDM_FA_RST1, 1);
	rtw_write32_mask(rtwdev, REG_OFDM_FA_RSTD_11N, BIT_MASK_OFDM_FA_RST1, 0);
	rtw_write32_mask(rtwdev, REG_OFDM_FA_HOLDC_11N, BIT_MASK_OFDM_FA_KEEP, 0);
	rtw_write32_mask(rtwdev, REG_OFDM_FA_RSTD_11N, BIT_MASK_OFDM_FA_KEEP1, 0);
	rtw_write32_mask(rtwdev, REG_CCK_FA_RST_11N, BIT_MASK_CCK_CNT_KPEN, 0);
	rtw_write32_mask(rtwdev, REG_CCK_FA_RST_11N, BIT_MASK_CCK_CNT_KPEN, 2);
	rtw_write32_mask(rtwdev, REG_CCK_FA_RST_11N, BIT_MASK_CCK_FA_KPEN, 0);
	rtw_write32_mask(rtwdev, REG_CCK_FA_RST_11N, BIT_MASK_CCK_FA_KPEN, 2);
	rtw_write32_mask(rtwdev, REG_PAGE_F_RST_11N, BIT_MASK_F_RST_ALL, 1);
	rtw_write32_mask(rtwdev, REG_PAGE_F_RST_11N, BIT_MASK_F_RST_ALL, 0);
}

static void rtw8188f_lck(struct rtw_dev *rtwdev)
{
	u32 lc_cal;
	u8 val_ctx, rf_val;
	int ret;

	val_ctx = rtw_read8(rtwdev, REG_CTX);
	if ((val_ctx & BIT_MASK_CTX_TYPE) != 0)
		rtw_write8(rtwdev, REG_CTX, val_ctx & ~BIT_MASK_CTX_TYPE);
	else
		rtw_write8(rtwdev, REG_TXPAUSE, 0xFF);
	lc_cal = rtw_read_rf(rtwdev, RF_PATH_A, RF_CFGCH, RFREG_MASK);

	rtw_write_rf(rtwdev, RF_PATH_A, RF_CFGCH, RFREG_MASK, lc_cal | BIT_LCK);

	ret = read_poll_timeout(rtw_read_rf, rf_val, rf_val != 0x1,
				10000, 1000000, false,
				rtwdev, RF_PATH_A, RF_CFGCH, BIT_LCK);
	if (ret)
		rtw_warn(rtwdev, "failed to poll LCK status bit\n");

	rtw_write_rf(rtwdev, RF_PATH_A, RF_CFGCH, RFREG_MASK, lc_cal);
	if ((val_ctx & BIT_MASK_CTX_TYPE) != 0)
		rtw_write8(rtwdev, REG_CTX, val_ctx);
	else
		rtw_write8(rtwdev, REG_TXPAUSE, 0x00);
}

/* IQK, ported from the vendor's phy_iq_calibrate_8188f() and friends in
 * hal/phydm/halrf/rtl8188f/halrf_8188f.c. RTL8188F is 1T1R, so only
 * path A is calibrated; the vendor's path B code is compiled out there.
 */
#define ADDA_ON_VAL_8188F 0x03c00014

static const u32 iqk_adda_regs_8188f[RTW8188F_IQK_ADDA_REG_NUM] = {
	REG_FPGA0_XCD_SWITCH, REG_BLUE_TOOTH, 0xe70, 0xe74,
	0xe78, 0xe7c, 0xe80, 0xe84,
	0xe88, 0xe8c, 0xed0, 0xed4,
	0xed8, 0xedc, 0xee0, REG_PMPD_ANAEN,
};

static const u32 iqk_mac8_regs_8188f[RTW8188F_IQK_MAC8_REG_NUM] = {
	REG_TXPAUSE, REG_BCN_CTRL, REG_BCN_CTRL_CLINT0,
};

static const u32 iqk_mac32_regs_8188f[RTW8188F_IQK_MAC32_REG_NUM] = {
	REG_GPIO_MUXCFG,
};

static const u32 iqk_bb_regs_8188f[RTW8188F_IQK_BB_REG_NUM] = {
	REG_OFDM0_TRX_PATH_EN, REG_OFDM0_TR_MUX_PAR, REG_FPGA0_XCD_RF_INT_SW,
	REG_CONFIG_ANT_A, REG_CONFIG_ANT_B, REG_FPGA0_XAB_RF_INT_SW,
	REG_FPGA0_XA_RF_INT_OE, REG_FPGA0_XB_RF_INT_OE, REG_FPGA0_RFMOD,
};

static void rtw8188f_iqk_backup_regs(struct rtw_dev *rtwdev,
				     struct rtw8188f_iqk_backup_regs *backup)
{
	int i;

	for (i = 0; i < RTW8188F_IQK_ADDA_REG_NUM; i++)
		backup->adda[i] = rtw_read32(rtwdev, iqk_adda_regs_8188f[i]);
	for (i = 0; i < RTW8188F_IQK_MAC8_REG_NUM; i++)
		backup->mac8[i] = rtw_read8(rtwdev, iqk_mac8_regs_8188f[i]);
	for (i = 0; i < RTW8188F_IQK_MAC32_REG_NUM; i++)
		backup->mac32[i] = rtw_read32(rtwdev, iqk_mac32_regs_8188f[i]);
	for (i = 0; i < RTW8188F_IQK_BB_REG_NUM; i++)
		backup->bb[i] = rtw_read32(rtwdev, iqk_bb_regs_8188f[i]);

	backup->igia = rtw_read32_mask(rtwdev, REG_OFDM0_XAAGC1, MASKBYTE0);
	backup->rf_pi_enable = rtw_read32_mask(rtwdev, REG_FPGA0_XA_HSSI_PARA1,
					       BIT(8));
}

static void rtw8188f_iqk_restore_regs(struct rtw_dev *rtwdev,
				      const struct rtw8188f_iqk_backup_regs *backup)
{
	int i;

	/* Switch BB back to SI mode if it was not in PI mode before. */
	if (!backup->rf_pi_enable) {
		rtw_write32(rtwdev, REG_FPGA0_XA_HSSI_PARA1, 0x01000000);
		rtw_write32(rtwdev, REG_FPGA0_XB_HSSI_PARA1, 0x01000000);
	}

	for (i = 0; i < RTW8188F_IQK_ADDA_REG_NUM; i++)
		rtw_write32(rtwdev, iqk_adda_regs_8188f[i], backup->adda[i]);
	for (i = 0; i < RTW8188F_IQK_MAC8_REG_NUM; i++)
		rtw_write8(rtwdev, iqk_mac8_regs_8188f[i], backup->mac8[i]);
	for (i = 0; i < RTW8188F_IQK_MAC32_REG_NUM; i++)
		rtw_write32(rtwdev, iqk_mac32_regs_8188f[i], backup->mac32[i]);
	for (i = 0; i < RTW8188F_IQK_BB_REG_NUM; i++)
		rtw_write32(rtwdev, iqk_bb_regs_8188f[i], backup->bb[i]);

	/* Restore the RX initial gain. The vendor driver writes 0x50 first
	 * to make sure the new value is latched.
	 */
	rtw_write32_mask(rtwdev, REG_OFDM0_XAAGC1, MASKBYTE0, 0x50);
	rtw_write32_mask(rtwdev, REG_OFDM0_XAAGC1, MASKBYTE0, backup->igia);

	/* 0xe30/0xe34 IQC default value */
	rtw_write32(rtwdev, REG_TX_IQK_TONE_A, 0x01008c00);
	rtw_write32(rtwdev, REG_RX_IQK_TONE_A, 0x01008c00);
}

static void rtw8188f_iqk_path_adda_on(struct rtw_dev *rtwdev)
{
	int i;

	for (i = 0; i < RTW8188F_IQK_ADDA_REG_NUM; i++)
		rtw_write32(rtwdev, iqk_adda_regs_8188f[i], ADDA_ON_VAL_8188F);
}

static void rtw8188f_iqk_config_mac(struct rtw_dev *rtwdev)
{
	rtw_write8(rtwdev, REG_TXPAUSE, 0xff);
}

static void rtw8188f_iqk_one_shot(struct rtw_dev *rtwdev)
{
	rtw_write32(rtwdev, REG_IQK_AGC_PTS, 0xf9000000);
	rtw_write32(rtwdev, REG_IQK_AGC_PTS, 0xf8000000);

	msleep(RTW8188F_IQK_DELAY_MS);

	/* reload RF 0xdf */
	rtw_write32_mask(rtwdev, REG_FPGA0_IQK, MASKH3BYTES, 0x000000);
	rtw_write_rf(rtwdev, RF_PATH_A, RF_RC_CORNER, RFREG_MASK, 0x180);
}

static bool rtw8188f_iqk_tx_ok(struct rtw_dev *rtwdev)
{
	u32 reg_eac, reg_e94, reg_e9c;

	reg_eac = rtw_read32(rtwdev, REG_RX_PWR_AFTER_IQK_A);
	reg_e94 = rtw_read32(rtwdev, REG_TX_PWR_BEFORE_IQK_A);
	reg_e9c = rtw_read32(rtwdev, REG_TX_PWR_AFTER_IQK_A);

	rtw_dbg(rtwdev, RTW_DBG_RFK,
		"[IQK] 0xeac = 0x%x 0xe94 = 0x%x 0xe9c = 0x%x\n",
		reg_eac, reg_e94, reg_e9c);

	return !(reg_eac & BIT(28)) &&
	       FIELD_GET(GENMASK(25, 16), reg_e94) != 0x142 &&
	       FIELD_GET(GENMASK(25, 16), reg_e9c) != 0x42;
}

static bool rtw8188f_iqk_rx_ok(struct rtw_dev *rtwdev)
{
	u32 reg_eac, reg_ea4;

	reg_eac = rtw_read32(rtwdev, REG_RX_PWR_AFTER_IQK_A);
	reg_ea4 = rtw_read32(rtwdev, REG_RX_PWR_BEFORE_IQK_A);

	rtw_dbg(rtwdev, RTW_DBG_RFK, "[IQK] 0xea4 = 0x%x 0xeac = 0x%x\n",
		reg_ea4, reg_eac);

	return !(reg_eac & BIT(27)) &&
	       FIELD_GET(GENMASK(25, 16), reg_ea4) != 0x132 &&
	       FIELD_GET(GENMASK(25, 16), reg_eac) != 0x36;
}

/* Set up the RF path for one IQK step. @rck_os, @txpa_g2 and @pad are the
 * per-step values from the vendor driver.
 */
static void rtw8188f_iqk_rf_setting(struct rtw_dev *rtwdev, u32 rck_os,
				    u32 txpa_g2, u32 pad)
{
	rtw_write32_mask(rtwdev, REG_FPGA0_IQK, MASKH3BYTES, 0x000000);
	rtw_write_rf(rtwdev, RF_PATH_A, RF_WE_LUT, 0x80000, 0x1);
	rtw_write_rf(rtwdev, RF_PATH_A, RF_RCK_OS, RFREG_MASK, rck_os);
	rtw_write_rf(rtwdev, RF_PATH_A, RF_TXPA_G1, RFREG_MASK, 0x0000f);
	rtw_write_rf(rtwdev, RF_PATH_A, RF_TXPA_G2, RFREG_MASK, txpa_g2);
	/* PA/PAD gain adjust */
	rtw_write_rf(rtwdev, RF_PATH_A, RF_RC_CORNER, RFREG_MASK, 0x980);
	rtw_write_rf(rtwdev, RF_PATH_A, RF_PAD_TXG, RFREG_MASK, pad);
	rtw_write32_mask(rtwdev, REG_FPGA0_IQK, MASKH3BYTES, 0x808000);
}

/* vendor phy_path_a_iqk_8188f(): TX IQK only */
static bool rtw8188f_iqk_tx_path(struct rtw_dev *rtwdev)
{
	rtw_dbg(rtwdev, RTW_DBG_RFK, "[IQK] path A TX IQK\n");

	rtw8188f_iqk_rf_setting(rtwdev, 0x20000, 0x07ff7, 0x5102a);

	rtw_write32(rtwdev, REG_TX_IQK_TONE_A, 0x18008c1c);
	rtw_write32(rtwdev, REG_RX_IQK_TONE_A, 0x38008c1c);
	rtw_write32(rtwdev, REG_TX_IQK_PI_A, 0x821403ff);
	rtw_write32(rtwdev, REG_RX_IQK_PI_A, 0x28160000);

	/* LO calibration setting */
	rtw_write32(rtwdev, REG_IQK_AGC_RSP, 0x00462911);

	rtw8188f_iqk_one_shot(rtwdev);

	return rtw8188f_iqk_tx_ok(rtwdev);
}

/* vendor phy_path_a_rx_iqk_8188f(): re-runs TX IQK to get TXIMR, then RX */
static bool rtw8188f_iqk_rx_path(struct rtw_dev *rtwdev, u32 lok)
{
	u32 reg_e94, reg_e9c, u4tmp;

	rtw_dbg(rtwdev, RTW_DBG_RFK, "[IQK] path A RX IQK\n");

	/* 1 Get TXIMR setting */
	rtw8188f_iqk_rf_setting(rtwdev, 0x30000, 0xf1173, 0x5102a);

	rtw_write32(rtwdev, REG_TX_IQK, 0x01007c00);
	rtw_write32(rtwdev, REG_RX_IQK, 0x01004800);

	rtw_write32(rtwdev, REG_TX_IQK_TONE_A, 0x10008c1c);
	rtw_write32(rtwdev, REG_RX_IQK_TONE_A, 0x30008c1c);
	rtw_write32(rtwdev, REG_TX_IQK_PI_A, 0x82160fff);
	rtw_write32(rtwdev, REG_RX_IQK_PI_A, 0x28160000);

	rtw_write32(rtwdev, REG_IQK_AGC_RSP, 0x00462911);

	rtw8188f_iqk_one_shot(rtwdev);

	if (!rtw8188f_iqk_tx_ok(rtwdev))
		return false;

	reg_e94 = rtw_read32(rtwdev, REG_TX_PWR_BEFORE_IQK_A);
	reg_e9c = rtw_read32(rtwdev, REG_TX_PWR_AFTER_IQK_A);
	u4tmp = 0x80007c00 | (reg_e94 & 0x3ff0000) |
		((reg_e9c & 0x3ff0000) >> 16);
	rtw_write32(rtwdev, REG_TX_IQK, u4tmp);

	/* 1 RX IQK */
	rtw8188f_iqk_rf_setting(rtwdev, 0x30000, 0xf7ff2, 0x51000);

	rtw_write32(rtwdev, REG_RX_IQK, 0x01004800);

	rtw_write32(rtwdev, REG_TX_IQK_TONE_A, 0x30008c1c);
	rtw_write32(rtwdev, REG_RX_IQK_TONE_A, 0x10008c1c);
	rtw_write32(rtwdev, REG_TX_IQK_PI_A, 0x82160000);
	rtw_write32(rtwdev, REG_RX_IQK_PI_A, 0x281613ff);

	rtw_write32(rtwdev, REG_IQK_AGC_RSP, 0x0046a911);

	rtw8188f_iqk_one_shot(rtwdev);

	/* reload the LOK value saved by the TX step */
	rtw_write_rf(rtwdev, RF_PATH_A, RF_LOK, RFREG_MASK, lok);

	return rtw8188f_iqk_rx_ok(rtwdev);
}

static void rtw8188f_iqk_one_round(struct rtw_dev *rtwdev,
				   s32 result[][IQK_NR], u8 t)
{
	u32 lok = 0;
	int i;

	rtw_dbg(rtwdev, RTW_DBG_RFK, "[IQK] round %d\n", t);

	rtw8188f_iqk_path_adda_on(rtwdev);

	/* BB setting */
	rtw_write32(rtwdev, REG_OFDM0_TRX_PATH_EN, 0x03a05600);
	rtw_write32(rtwdev, REG_OFDM0_TR_MUX_PAR, 0x000800e4);
	rtw_write32(rtwdev, REG_FPGA0_XCD_RF_INT_SW, 0x25204000);

	rtw8188f_iqk_config_mac(rtwdev);

	rtw_write32_mask(rtwdev, REG_FPGA0_IQK, MASKH3BYTES, 0x808000);
	rtw_write32(rtwdev, REG_TX_IQK, 0x01007c00);
	rtw_write32(rtwdev, REG_RX_IQK, 0x01004800);

	for (i = 0; i < RTW8188F_IQK_RETRY; i++) {
		if (!rtw8188f_iqk_tx_path(rtwdev))
			continue;

		rtw_write32_mask(rtwdev, REG_FPGA0_IQK, MASKH3BYTES, 0x000000);
		/* Save the LOK result, reloaded after the RX step. */
		lok = rtw_read_rf(rtwdev, RF_PATH_A, RF_LOK, RFREG_MASK);

		result[t][IQK_TX_X] =
			FIELD_GET(GENMASK(25, 16),
				  rtw_read32(rtwdev, REG_TX_PWR_BEFORE_IQK_A));
		result[t][IQK_TX_Y] =
			FIELD_GET(GENMASK(25, 16),
				  rtw_read32(rtwdev, REG_TX_PWR_AFTER_IQK_A));
		break;
	}

	for (i = 0; i < RTW8188F_IQK_RETRY; i++) {
		if (!rtw8188f_iqk_rx_path(rtwdev, lok))
			continue;

		result[t][IQK_RX_X] =
			FIELD_GET(GENMASK(25, 16),
				  rtw_read32(rtwdev, REG_RX_PWR_BEFORE_IQK_A));
		result[t][IQK_RX_Y] =
			FIELD_GET(GENMASK(25, 16),
				  rtw_read32(rtwdev, REG_RX_PWR_AFTER_IQK_A));
		break;
	}

	/* back to BB mode */
	rtw_write32_mask(rtwdev, REG_FPGA0_IQK, MASKH3BYTES, 0);
}

static s32 iqk_to_s32(s32 val)
{
	/* val is a 10 bit two's complement number */
	if (val & BIT(9))
		return val | ~GENMASK(9, 0);
	return val;
}

/* vendor phy_simularity_compare_8188f(), reduced to the 1T1R case */
static bool rtw8188f_iqk_similarity_cmp(struct rtw_dev *rtwdev,
					s32 result[][IQK_NR], u8 c1, u8 c2)
{
	u32 bitmap = 0;
	bool valid = true;
	int i, j;

	for (i = 0; i < IQK_NR; i++) {
		s32 tmp1, tmp2, diff;

		if (i == IQK_TX_Y || i == IQK_RX_Y) {
			tmp1 = iqk_to_s32(result[c1][i]);
			tmp2 = iqk_to_s32(result[c2][i]);
		} else {
			tmp1 = result[c1][i];
			tmp2 = result[c2][i];
		}

		diff = abs(tmp1 - tmp2);
		if (diff <= RTW8188F_IQK_MAX_TOLERANCE)
			continue;

		if (i == IQK_RX_X && !bitmap) {
			if (result[c1][i] + result[c1][i + 1] == 0)
				valid = false;
			else if (result[c2][i] + result[c2][i + 1] == 0)
				valid = false;
			else
				bitmap |= BIT(i);

			if (!valid) {
				u8 cand = result[c1][i] + result[c1][i + 1] == 0
					  ? c2 : c1;

				for (j = IQK_TX_X; j < IQK_RX_X; j++)
					result[IQK_ROUND_HYBRID][j] =
						result[cand][j];
			}
		} else {
			bitmap |= BIT(i);
		}
	}

	if (bitmap == 0)
		return valid;

	if (!(bitmap & GENMASK(IQK_TX_Y, IQK_TX_X))) {
		result[IQK_ROUND_HYBRID][IQK_TX_X] = result[c1][IQK_TX_X];
		result[IQK_ROUND_HYBRID][IQK_TX_Y] = result[c1][IQK_TX_Y];
	}

	if (!(bitmap & GENMASK(IQK_RX_Y, IQK_RX_X))) {
		result[IQK_ROUND_HYBRID][IQK_RX_X] = result[c1][IQK_RX_X];
		result[IQK_ROUND_HYBRID][IQK_RX_Y] = result[c1][IQK_RX_Y];
	}

	return false;
}

/* vendor _phy_path_a_fill_iqk_matrix8188f() */
static void rtw8188f_iqk_fill_matrix(struct rtw_dev *rtwdev, const s32 *result,
				     bool tx_only)
{
	s32 oldval_0, x, y, tx0_a, tx0_c;
	u32 reg;

	oldval_0 = (rtw_read32(rtwdev, REG_OFDM0_XA_TX_IQ_IMBALANCE) >> 22)
		   & 0x3ff;

	x = iqk_to_s32(result[IQK_TX_X]);
	tx0_a = (x * oldval_0) >> 8;
	rtw_write32_mask(rtwdev, REG_OFDM0_XA_TX_IQ_IMBALANCE, 0x3ff, tx0_a);
	rtw_write32_mask(rtwdev, REG_OFDM0_ECCA_THRES, BIT(31),
			 ((x * oldval_0) >> 7) & 0x1);

	y = iqk_to_s32(result[IQK_TX_Y]);
	tx0_c = (y * oldval_0) >> 8;
	rtw_write32_mask(rtwdev, REG_OFDM0_XC_TX_AFE, MASKH4BITS,
			 (tx0_c & 0x3c0) >> 6);
	rtw_write32_mask(rtwdev, REG_OFDM0_XA_TX_IQ_IMBALANCE, 0x003f0000,
			 tx0_c & 0x3f);
	rtw_write32_mask(rtwdev, REG_OFDM0_ECCA_THRES, BIT(29),
			 ((y * oldval_0) >> 7) & 0x1);

	if (tx_only) {
		rtw_dbg(rtwdev, RTW_DBG_RFK, "[IQK] only TX filled\n");
		return;
	}

	reg = result[IQK_RX_X];
	rtw_write32_mask(rtwdev, REG_OFDM0_XA_RX_IQ_IMB, 0x3ff, reg);
	reg = result[IQK_RX_Y] & 0x3f;
	rtw_write32_mask(rtwdev, REG_OFDM0_XA_RX_IQ_IMB, 0xfc00, reg);
	reg = (result[IQK_RX_Y] >> 6) & 0xf;
	rtw_write32_mask(rtwdev, REG_OFDM0_RX_IQ_EXT_A, MASKH4BITS, reg);
}

static void rtw8188f_iqk(struct rtw_dev *rtwdev)
{
	struct rtw_dm_info *dm_info = &rtwdev->dm_info;
	struct rtw8188f_iqk_backup_regs backup;
	u8 final_candidate = IQK_ROUND_INVALID;
	s32 result[IQK_ROUND_SIZE][IQK_NR];
	u8 i, j;

	rtw_dbg(rtwdev, RTW_DBG_RFK, "[IQK] Start!\n");

	memset(result, 0, sizeof(result));
	rtw8188f_iqk_backup_regs(rtwdev, &backup);

	for (i = IQK_ROUND_0; i <= IQK_ROUND_2; i++) {
		rtw8188f_iqk_one_round(rtwdev, result, i);

		if (i > IQK_ROUND_0)
			rtw8188f_iqk_restore_regs(rtwdev, &backup);

		for (j = IQK_ROUND_0; j < i; j++) {
			if (rtw8188f_iqk_similarity_cmp(rtwdev, result, j, i)) {
				final_candidate = j;
				goto iqk_done;
			}
		}
	}

	if (final_candidate == IQK_ROUND_INVALID) {
		s32 reg_tmp = 0;

		for (i = 0; i < IQK_NR; i++)
			reg_tmp += result[IQK_ROUND_HYBRID][i];

		if (reg_tmp != 0) {
			final_candidate = IQK_ROUND_HYBRID;
		} else {
			rtw_warn(rtwdev, "[IQK] failed, using defaults\n");
			goto out;
		}
	}

iqk_done:
	if (result[final_candidate][IQK_TX_X] != 0)
		rtw8188f_iqk_fill_matrix(rtwdev, result[final_candidate],
					 result[final_candidate][IQK_RX_X] == 0);

	dm_info->iqk.result.s1_x = result[final_candidate][IQK_TX_X];
	dm_info->iqk.result.s1_y = result[final_candidate][IQK_TX_Y];
	dm_info->iqk.done = true;

out:
	for (i = IQK_ROUND_0; i < IQK_ROUND_SIZE; i++)
		rtw_dbg(rtwdev, RTW_DBG_RFK,
			"[IQK] Result %u: rege94=%x rege9c=%x regea4=%x regeac=%x %s\n",
			i, result[i][IQK_TX_X], result[i][IQK_TX_Y],
			result[i][IQK_RX_X], result[i][IQK_RX_Y],
			final_candidate == i ? "(final candidate)" : "");

	rtw_dbg(rtwdev, RTW_DBG_RFK, "[IQK] Finished.\n");
}

static void rtw8188f_phy_calibration(struct rtw_dev *rtwdev)
{
	rtw8188f_iqk(rtwdev);
	rtw8188f_lck(rtwdev);
}

static void rtw8188f_set_tx_power_index_by_rate(struct rtw_dev *rtwdev,
						u8 path, u8 rs)
{
	struct rtw_hal *hal = &rtwdev->hal;
	const struct rtw_hw_reg *txagc;
	u8 rate, pwr_index;
	int j;

	for (j = 0; j < rtw_rate_size[rs]; j++) {
		rate = rtw_rate_section[rs][j];
		pwr_index = hal->tx_pwr_tbl[path][rate];

		if (rate >= ARRAY_SIZE(rtw8188f_txagc)) {
			rtw_warn(rtwdev, "rate 0x%x isn't supported\n", rate);
			continue;
		}
		txagc = &rtw8188f_txagc[rate];
		if (!txagc->addr) {
			rtw_warn(rtwdev, "rate 0x%x isn't defined\n", rate);
			continue;
		}

		rtw_write32_mask(rtwdev, txagc->addr, txagc->mask, pwr_index);
	}
}

static void rtw8188f_set_tx_power_index(struct rtw_dev *rtwdev)
{
	struct rtw_hal *hal = &rtwdev->hal;
	int rs, path;

	for (path = 0; path < hal->rf_path_num; path++)
		for (rs = 0; rs <= RTW_RATE_SECTION_HT_1S; rs++)
			rtw8188f_set_tx_power_index_by_rate(rtwdev, path, rs);
}

static void rtw8188f_pwrtrack_set_ofdm_pwr(struct rtw_dev *rtwdev, s8 swing_idx,
					   s8 txagc_idx)
{
	struct rtw_dm_info *dm_info = &rtwdev->dm_info;

	dm_info->txagc_remnant_ofdm[RF_PATH_A] = txagc_idx;

	swing_idx = clamp_t(s8, swing_idx, 0, RTW_OFDM_SWING_TABLE_SIZE - 1);
	rtw_write32(rtwdev, REG_OFDM0_XA_TX_IQ_IMBALANCE,
		    rtw8188f_ofdm_swing_table[swing_idx]);
	rtw_write32_mask(rtwdev, REG_OFDM0_XC_TX_AFE, MASKH4BITS, 0);
	rtw_write32_mask(rtwdev, REG_OFDM0_ECCA_THRES, BIT(24), 0);
}

static void rtw8188f_pwrtrack_set_cck_pwr(struct rtw_dev *rtwdev, s8 swing_idx,
					  s8 txagc_idx)
{
	struct rtw_dm_info *dm_info = &rtwdev->dm_info;
	const u8 (*swing_table)[16];
	int i;

	dm_info->txagc_remnant_cck = txagc_idx;

	swing_idx = clamp_t(s8, swing_idx, 0, RTW_CCK_SWING_TABLE_SIZE - 1);

	if (rtwdev->hal.current_channel == 14)
		swing_table = rtw8188f_cck_swing_table_ch14;
	else
		swing_table = rtw8188f_cck_swing_table;

	BUILD_BUG_ON(ARRAY_SIZE(rtw8188f_cck_pwr_regs) != 16);

	for (i = 0; i < ARRAY_SIZE(rtw8188f_cck_pwr_regs); i++)
		rtw_write8(rtwdev, rtw8188f_cck_pwr_regs[i],
			   swing_table[swing_idx][i]);
}

static void rtw8188f_pwrtrack_set(struct rtw_dev *rtwdev)
{
	struct rtw_dm_info *dm_info = &rtwdev->dm_info;
	s8 final_ofdm_swing_index;
	s8 final_cck_swing_index;
	u8 pwr_tracking_limit_ofdm = 32; /* +1dB */
	u8 pwr_tracking_limit_cck = RTW_CCK_SWING_TABLE_SIZE - 1; /* -6dB */

	final_ofdm_swing_index = dm_info->default_ofdm_index +
				 dm_info->delta_power_index[RF_PATH_A];
	final_cck_swing_index = dm_info->default_cck_index +
				dm_info->delta_power_index[RF_PATH_A];

	if (final_ofdm_swing_index > pwr_tracking_limit_ofdm)
		rtw8188f_pwrtrack_set_ofdm_pwr(rtwdev, pwr_tracking_limit_ofdm,
					       final_ofdm_swing_index -
					       pwr_tracking_limit_ofdm);
	else if (final_ofdm_swing_index < 0)
		rtw8188f_pwrtrack_set_ofdm_pwr(rtwdev, 0,
					       final_ofdm_swing_index);
	else
		rtw8188f_pwrtrack_set_ofdm_pwr(rtwdev, final_ofdm_swing_index, 0);

	if (final_cck_swing_index > pwr_tracking_limit_cck)
		rtw8188f_pwrtrack_set_cck_pwr(rtwdev, pwr_tracking_limit_cck,
					      final_cck_swing_index -
					      pwr_tracking_limit_cck);
	else if (final_cck_swing_index < 0)
		rtw8188f_pwrtrack_set_cck_pwr(rtwdev, 0, final_cck_swing_index);
	else
		rtw8188f_pwrtrack_set_cck_pwr(rtwdev, final_cck_swing_index, 0);

	rtw_phy_set_tx_power_level(rtwdev, rtwdev->hal.current_channel);
}

static void rtw8188f_phy_pwrtrack(struct rtw_dev *rtwdev)
{
	struct rtw_dm_info *dm_info = &rtwdev->dm_info;
	struct rtw_swing_table swing_table;
	u8 thermal_value, delta;

	rtw_phy_config_swing_table(rtwdev, &swing_table);

	if (rtwdev->efuse.thermal_meter[RF_PATH_A] == 0xff) {
		rtw_warn(rtwdev, "thermal meter is not calibrated\n");
		return;
	}

	thermal_value = rtw_read_rf(rtwdev, RF_PATH_A, RF_T_METER,
				    BIT_MASK_THERMAL);

	rtw_phy_pwrtrack_avg(rtwdev, thermal_value, RF_PATH_A);

	if (dm_info->pwr_trk_init_trigger)
		dm_info->pwr_trk_init_trigger = false;
	else if (!rtw_phy_pwrtrack_thermal_changed(rtwdev, thermal_value,
						   RF_PATH_A))
		goto out;

	delta = rtw_phy_pwrtrack_get_delta(rtwdev, RF_PATH_A);

	dm_info->delta_power_index[RF_PATH_A] =
		rtw_phy_pwrtrack_get_pwridx(rtwdev, &swing_table, RF_PATH_A,
					    RF_PATH_A, delta);
	if (dm_info->delta_power_index[RF_PATH_A] ==
	    dm_info->delta_power_index_last[RF_PATH_A])
		goto out;

	dm_info->delta_power_index_last[RF_PATH_A] =
		dm_info->delta_power_index[RF_PATH_A];
	rtw8188f_pwrtrack_set(rtwdev);

out:
	dm_info->pwr_trk_triggered = false;
}

static void rtw8188f_pwr_track(struct rtw_dev *rtwdev)
{
	struct rtw_efuse *efuse = &rtwdev->efuse;
	struct rtw_dm_info *dm_info = &rtwdev->dm_info;

	if (efuse->power_track_type != 0) {
		rtw_warn(rtwdev, "unsupported power track type\n");
		return;
	}

	if (!dm_info->pwr_trk_triggered) {
		rtw_write_rf(rtwdev, RF_PATH_A, RF_T_METER,
			     BIT_MASK_THERMAL_TRIG, 0x03);
		dm_info->pwr_trk_triggered = true;
		return;
	}

	rtw8188f_phy_pwrtrack(rtwdev);
}

static void rtw8188f_fill_txdesc_checksum(struct rtw_dev *rtwdev,
					  struct rtw_tx_pkt_info *pkt_info,
					  u8 *txdesc)
{
	size_t words = 32 / 2; /* calculate the first 32 bytes (16 words) */
	struct rtw_tx_desc *tx_desc = (struct rtw_tx_desc *)txdesc;
	__le16 *data = (__le16 *)(txdesc);
	__le16 chksum = 0;

	le32p_replace_bits(&tx_desc->w7, 0, RTW_TX_DESC_W7_TXDESC_CHECKSUM);

	while (words--)
		chksum ^= *data++;

	chksum = ~chksum;

	le32p_replace_bits(&tx_desc->w7, __le16_to_cpu(chksum),
			   RTW_TX_DESC_W7_TXDESC_CHECKSUM);
}

static const struct rtw_chip_ops rtw8188f_ops = {
	.power_on		= rtw_power_on,
	.power_off		= rtw_power_off,
	.mac_init		= rtw8188f_mac_init,
	.mac_postinit		= rtw8188f_mac_postinit,
	.dump_fw_crash		= NULL,
	.shutdown		= NULL,
	.read_efuse		= rtw8188f_read_efuse,
	.phy_set_param		= rtw8188f_phy_set_param,
	.set_channel		= rtw8188f_set_channel,
	.query_phy_status	= query_phy_status,
	.read_rf		= rtw_phy_read_rf_sipi,
	.write_rf		= rtw_phy_write_rf_reg_sipi,
	.set_tx_power_index	= rtw8188f_set_tx_power_index,
	.set_antenna		= NULL,
	.cfg_ldo25		= rtw8188f_cfg_ldo25,
	.efuse_grant		= rtw8188f_efuse_grant,
	.set_ampdu_factor	= NULL,
	.false_alarm_statistics	= rtw8188f_false_alarm_statistics,
	.phy_calibration	= rtw8188f_phy_calibration,
	.dpk_track		= NULL,
	/* Like 8703B, this chip generation has no REG_CSRATIO, so there
	 * is no CCK PD default to read and nothing to set.
	 */
	.cck_pd_set		= NULL,
	.pwr_track		= rtw8188f_pwr_track,
	.config_bfee		= NULL,
	.set_gid_table		= NULL,
	.cfg_csi_rate		= NULL,
	.adaptivity_init	= NULL,
	.adaptivity		= NULL,
	.cfo_init		= NULL,
	.cfo_track		= NULL,
	.config_tx_path		= NULL,
	.config_txrx_mode	= NULL,
	.fill_txdesc_checksum	= rtw8188f_fill_txdesc_checksum,

	/* WiFi-only chip, no coex */
	.coex_set_init		= NULL,
	.coex_set_ant_switch	= NULL,
	.coex_set_gnt_fix	= NULL,
	.coex_set_gnt_debug	= NULL,
	.coex_set_rfe_type	= NULL,
	.coex_set_wl_tx_power	= NULL,
	.coex_set_wl_rx_gain	= NULL,
};

static const struct rtw_rfe_def rtw8188f_rfe_defs[] = {
	[0] = { .phy_pg_tbl = &rtw8188f_bb_pg_tbl,
		.txpwr_lmt_tbl = &rtw8188f_txpwr_lmt_tbl, },
};

const struct rtw_chip_info rtw8188f_hw_spec = {
	.ops = &rtw8188f_ops,
	.id = RTW_CHIP_TYPE_8188F,

	.fw_name = "rtw88/rtw8188f_fw.bin",
	.wlan_cpu = RTW_WCPU_8051,
	.tx_pkt_desc_sz = 40,
	.tx_buf_desc_sz = 16,
	.rx_pkt_desc_sz = 24,
	.rx_buf_desc_sz = 8,
	.phy_efuse_size = 256,
	.log_efuse_size = 512,
	.ptct_efuse_size = 15,
	/* TX 32K, RX 16K, see the comment above PAGE_SIZE_TX_8188F in the
	 * vendor driver's include/rtl8188f_hal.h.
	 */
	.txff_size = 32768,
	.rxff_size = 16384,
	.rsvd_drv_pg_num = 8,
	.band = RTW_BAND_2G,
	.page_size = TX_PAGE_SIZE,
	.csi_buf_pg_num = 0,
	.dig_min = 0x20,
	.txgi_factor = 1,
	.is_pwr_by_rate_dec = true,
	.rx_ldpc = false,
	.tx_stbc = false,
	.max_power_index = 0x3f,
	.ampdu_density = IEEE80211_HT_MPDU_DENSITY_16,
	.usb_tx_agg_desc_num = 1,
	.hw_feature_report = true,
	.c2h_ra_report_size = 7,
	.old_datarate_fb_limit = true,

	.path_div_supported = false,
	.ht_supported = true,
	.vht_supported = false,
	.lps_deep_mode_supported = 0,

	.sys_func_en = 0xFD,
	.pwr_on_seq = card_enable_flow_8188f,
	.pwr_off_seq = card_disable_flow_8188f,
	.rqpn_table = rqpn_table_8188f,
	.prioq_addrs = &prioq_addrs_8188f,
	.page_table = page_table_8188f,
	/* used only in pci.c, not needed for SDIO devices */
	.intf_table = NULL,

	.dig = dig_8188f,
	.dig_cck = dig_cck_8188f,

	.rf_sipi_addr = {0x840, 0x844},
	.rf_sipi_read_addr = rf_sipi_addr_8188f,
	.fix_rf_phy_num = 2,
	.ltecoex_addr = NULL,

	.mac_tbl = &rtw8188f_mac_tbl,
	.agc_tbl = &rtw8188f_agc_tbl,
	.bb_tbl = &rtw8188f_bb_tbl,
	.rf_tbl = {&rtw8188f_rf_a_tbl},

	.rfe_defs = rtw8188f_rfe_defs,
	.rfe_defs_size = ARRAY_SIZE(rtw8188f_rfe_defs),

	.iqk_threshold = 8,

	/* WoWLAN firmware exists, but is not implemented yet */
	.wow_fw_name = "rtw88/rtw8188f_wow_fw.bin",
	.wowlan_stub = NULL,
	.max_scan_ie_len = IEEE80211_MAX_DATA_LEN,

	/* No Bluetooth, but the core still walks these tables on the
	 * wifi-only path.
	 */
	.coex_para_ver = 0,
	.bt_desired_ver = 0,
	.scbd_support = false,
	.new_scbd10_def = false,
	.ble_hid_profile_support = false,
	.wl_mimo_ps_support = false,
	.pstdma_type = 0,
	.bt_rssi_type = COEX_BTRSSI_RATIO,
	.ant_isolation = 15,
	.rssi_tolerance = 2,
	.bt_rssi_step = bt_rssi_step_8188f,
	.wl_rssi_step = wl_rssi_step_8188f,
	.wl_rf_para_tx = rf_para_tx_8188f,
	.wl_rf_para_rx = rf_para_rx_8188f,
	.wl_rf_para_num = ARRAY_SIZE(rf_para_tx_8188f),
	.afh_5g = afh_5g_8188f,
	.afh_5g_num = ARRAY_SIZE(afh_5g_8188f),
};
EXPORT_SYMBOL(rtw8188f_hw_spec);

MODULE_FIRMWARE("rtw88/rtw8188f_fw.bin");

MODULE_AUTHOR("Realtek Corporation");
MODULE_AUTHOR("Jernej Skrabec <jernej.skrabec@gmail.com>");
MODULE_DESCRIPTION("Realtek 802.11n wireless 8188f driver");
MODULE_LICENSE("Dual BSD/GPL");
