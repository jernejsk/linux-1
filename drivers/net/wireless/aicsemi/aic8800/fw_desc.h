/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Hardware descriptors exchanged with the AIC8800 firmware.
 *
 * Like fw_msg.h these definitions have to match the firmware binary exactly.
 *
 * Copyright (C) RivieraWaves 2012-2019
 * Copyright (C) AICSemi 2018-2024
 */

#ifndef AIC_FW_DESC_H
#define AIC_FW_DESC_H

#include <linux/types.h>

#include "fw_mac.h"

/*
 * Every buffer exchanged over the bus starts with a four byte header:
 *
 *	[15:0]	payload length in bytes, excluding the header itself
 *	[23:16]	channel type, see enum aic_pkt_type
 *	[31:24]	CRC-8 over the first three bytes
 *
 * For receive descriptors the firmware overlays this header with the first
 * word of struct aic_rx_vect, so the length and type can be read either way.
 */
#define AIC_BUS_HDR_LEN			4

/* Every packet in an aggregate starts on a four byte boundary. */
#define AIC_BUS_ALIGN			4

enum aic_pkt_type {
	AIC_PKT_DATA		= 0x00,
	AIC_PKT_DATA_TX		= 0x01,
	AIC_PKT_CFG		= 0x10,
	AIC_PKT_CFG_CMD_RSP	= 0x11,
	AIC_PKT_CFG_DATA_CFM	= 0x12,
	AIC_PKT_CFG_PRINT	= 0x13,
};

/* the type field also carries flags, mask them off before comparing */
#define AIC_PKT_TYPE_MASK		0x7f

/* Descriptor the host prepends to every transmitted frame. */
struct aic_txdesc {
	__le16 packet_len;
	__le16 flags_ext;
	__le32 hostid;
	struct mac_addr eth_dest_addr;
	struct mac_addr eth_src_addr;
	__le16 ethertype;
	u8 ac;
	u8 tid;
	u8 vif_idx;
	u8 staid;
	__le16 flags;
} __packed;

/* struct aic_txdesc::flags */
#define AIC_TXDESC_F_RETRY		BIT(0)
#define AIC_TXDESC_F_MORE_DATA		BIT(2)
#define AIC_TXDESC_F_MGMT		BIT(3)
#define AIC_TXDESC_F_MGMT_NO_CCK	BIT(4)
#define AIC_TXDESC_F_AMSDU		BIT(6)
#define AIC_TXDESC_F_MGMT_ROBUST	BIT(7)
#define AIC_TXDESC_F_USE_4ADDR		BIT(8)
#define AIC_TXDESC_F_EOSP		BIT(9)
#define AIC_TXDESC_F_MESH_FWD		BIT(10)
#define AIC_TXDESC_F_TDLS		BIT(11)

/*
 * struct aic_txdesc::hostid selects what the firmware does with the frame in
 * addition to sending it.  Either it confirms the frame, in which case the low
 * bits carry an index the confirmation refers back to, or it transmits at a
 * fixed rate given by the low bits, or neither.
 */
#define AIC_TXDESC_HOSTID_CFM		BIT(31)
#define AIC_TXDESC_HOSTID_FIXED_RATE	BIT(30)

/* Receive vectors, filled in by the PHY. */
struct aic_rx_leg_vect {
	u8 dyn_bw_in_non_ht:1;
	u8 chn_bw_in_non_ht:2;
	u8 rsvd_nht:4;
	u8 lsig_valid:1;
} __packed;

struct aic_rx_ht_vect {
	u16 sounding:1;
	u16 smoothing:1;
	u16 short_gi:1;
	u16 aggregation:1;
	u16 stbc:1;
	u16 num_extn_ss:2;
	u16 lsig_valid:1;
	u16 mcs:7;
	u16 fec:1;
	u16 length:16;
} __packed;

struct aic_rx_vht_vect {
	u8 sounding:1;
	u8 beamformed:1;
	u8 short_gi:1;
	u8 rsvd_vht1:1;
	u8 stbc:1;
	u8 doze_not_allowed:1;
	u8 first_user:1;
	u8 rsvd_vht2:1;
	u16 partial_aid:9;
	u16 group_id:6;
	u16 rsvd_vht3:1;
	u32 mcs:4;
	u32 nss:3;
	u32 fec:1;
	u32 length:20;
	u32 rsvd_vht4:4;
} __packed;

struct aic_rx_he_vect {
	u8 sounding:1;
	u8 beamformed:1;
	u8 gi_type:2;
	u8 stbc:1;
	u8 rsvd_he1:3;

	u8 uplink_flag:1;
	u8 beam_change:1;
	u8 dcm:1;
	u8 he_ltf_type:2;
	u8 doppler:1;
	u8 rsvd_he2:2;

	u8 bss_color:6;
	u8 rsvd_he3:2;

	u8 txop_duration:7;
	u8 rsvd_he4:1;

	u8 pe_duration:4;
	u8 spatial_reuse:4;

	u8 sig_b_comp_mode:1;
	u8 dcm_sig_b:1;
	u8 mcs_sig_b:3;
	u8 ru_size:3;

	u32 mcs:4;
	u32 nss:3;
	u32 fec:1;
	u32 length:20;
	u32 rsvd_he6:4;
} __packed;

struct aic_rx_vector_1 {
	u8 format_mod:4;
	u8 ch_bw:3;
	u8 pre_type:1;
	u8 antenna_set:8;
	s32 rssi_leg:8;
	u32 leg_length:12;
	u32 leg_rate:4;
	s32 rssi1:8;

	union {
		struct aic_rx_leg_vect leg;
		struct aic_rx_ht_vect ht;
		struct aic_rx_vht_vect vht;
		struct aic_rx_he_vect he;
	};
} __packed;

struct aic_rx_vector_2 {
	u32 rcpi1:8;
	u32 rcpi2:8;
	u32 rcpi3:8;
	u32 rcpi4:8;

	u32 evm1:8;
	u32 evm2:8;
	u32 evm3:8;
	u32 evm4:8;
};

/* enum aic_rx_vector_1::format_mod */
enum aic_rx_format_mod {
	AIC_FORMATMOD_NON_HT,
	AIC_FORMATMOD_NON_HT_DUP_OFDM,
	AIC_FORMATMOD_HT_MF,
	AIC_FORMATMOD_HT_GF,
	AIC_FORMATMOD_VHT,
	AIC_FORMATMOD_HE_SU_ER,
	AIC_FORMATMOD_HE_SU,
	AIC_FORMATMOD_HE_MU,
	AIC_FORMATMOD_HE_TB,
};

struct aic_phy_channel_info {
	u32 phy_band:8;
	u32 phy_channel_type:8;
	u32 phy_prim20_freq:16;
	u32 phy_center1_freq:16;
	u32 phy_center2_freq:16;
};

struct aic_rx_vect {
	/* this word doubles as the bus header, see AIC_BUS_HDR_LEN */
	u32 len:16;
	u32 type:8;
	u32 mpdu_cnt:6;
	u32 ampdu_cnt:2;

	__le32 tsf_lo;
	__le32 tsf_hi;

	struct aic_rx_vector_1 rx_vect1;
	struct aic_rx_vector_2 rx_vect2;

	u32 rx_vect2_valid:1;
	u32 resp_frame:1;
	u32 decr_status:3;
	u32 rx_fifo_oflow:1;

	u32 undef_err:1;
	u32 phy_err:1;
	u32 fcs_err:1;
	u32 addr_mismatch:1;
	u32 ga_frame:1;
	u32 current_ac:2;

	u32 frm_successful_rx:1;
	u32 desc_done_rx:1;
	u32 key_sram_index:10;
	u32 key_sram_v:1;
	u32 type_80211:2;
	u32 subtype_80211:4;
};

/* struct aic_rx_vect::decr_status */
enum aic_rx_decr_status {
	AIC_RX_DECR_UNENC,
	AIC_RX_DECR_WEP,
	AIC_RX_DECR_TKIP,
	AIC_RX_DECR_CCMP128,
	AIC_RX_DECR_CCMP256,
	AIC_RX_DECR_GCMP128,
	AIC_RX_DECR_GCMP256,
	AIC_RX_DECR_WAPI,
};

struct aic_rxhdr {
	struct aic_rx_vect vect;
	struct aic_phy_channel_info phy_info;

	u32 flags_is_amsdu:1;
	u32 flags_is_80211_mpdu:1;
	u32 flags_is_4addr:1;
	u32 flags_new_peer:1;
	u32 flags_user_prio:1;
	u32 flags_need_reord:1;
	u32 flags_upload:1;
	u32 flags_monitor_vif:1;
	u32 flags_vif_idx:8;
	u32 flags_sta_idx:8;
	u32 flags_dst_idx:8;

	u32 pattern;
};

/*
 * The firmware places the frame two bytes after the receive header so that the
 * IP header of an Ethernet frame ends up 32 bit aligned, and pads the whole
 * thing to AIC_RX_HDR_PAD.
 */
#define AIC_RX_HDR_LEN			58
#define AIC_RX_HDR_PAD			60

/*
 * Firmware to host transmit confirmation for a frame that was queued with
 * AIC_TXDESC_HOSTID_CFM set.  @idx repeats the index from the descriptor.
 */
struct aic_txcfm {
	__le32 status;
	__le32 idx;
} __packed;

/* struct aic_txcfm::status */
#define AIC_TXCFM_S_TX_DONE		BIT(0)
#define AIC_TXCFM_S_RETRY_REQUIRED	BIT(1)
#define AIC_TXCFM_S_SW_RETRY_REQUIRED	BIT(2)
#define AIC_TXCFM_S_ACKNOWLEDGED	BIT(3)

/*
 * Receive filter of the MAC hardware, as taken by MM_SET_FILTER_REQ.  The
 * message replaces the whole register, so the bits the firmware sets up itself
 * have to be repeated in every filter the driver asks for.
 */
#define AIC_RX_FILTER_ACCEPT_UNKNOWN		BIT(30)
#define AIC_RX_FILTER_ACCEPT_OTHER_DATA		BIT(29)
#define AIC_RX_FILTER_ACCEPT_QOS_NULL		BIT(28)
#define AIC_RX_FILTER_ACCEPT_Q_DATA		BIT(26)
#define AIC_RX_FILTER_ACCEPT_DATA		BIT(24)
#define AIC_RX_FILTER_ACCEPT_OTHER_CNTRL	BIT(23)
#define AIC_RX_FILTER_ACCEPT_CF_END		BIT(22)
#define AIC_RX_FILTER_ACCEPT_ACK		BIT(21)
#define AIC_RX_FILTER_ACCEPT_CTS		BIT(20)
#define AIC_RX_FILTER_ACCEPT_RTS		BIT(19)
#define AIC_RX_FILTER_ACCEPT_PS_POLL		BIT(18)
#define AIC_RX_FILTER_ACCEPT_BA			BIT(17)
#define AIC_RX_FILTER_ACCEPT_BAR		BIT(16)
#define AIC_RX_FILTER_ACCEPT_OTHER_MGMT		BIT(15)
#define AIC_RX_FILTER_ACCEPT_ALL_BEACON		BIT(13)
#define AIC_RX_FILTER_ACCEPT_BEACON		BIT(10)
#define AIC_RX_FILTER_ACCEPT_PROBE_RESP		BIT(9)
#define AIC_RX_FILTER_ACCEPT_PROBE_REQ		BIT(8)
#define AIC_RX_FILTER_ACCEPT_MY_UNICAST		BIT(7)
#define AIC_RX_FILTER_ACCEPT_UNICAST		BIT(6)
#define AIC_RX_FILTER_ACCEPT_OTHER_BSSID	BIT(4)
#define AIC_RX_FILTER_ACCEPT_BROADCAST		BIT(3)
#define AIC_RX_FILTER_ACCEPT_MULTICAST		BIT(2)

/* What the firmware runs with unless the driver says otherwise. */
#define AIC_RX_FILTER_DEFAULT						\
	(AIC_RX_FILTER_ACCEPT_QOS_NULL | AIC_RX_FILTER_ACCEPT_Q_DATA |	\
	 AIC_RX_FILTER_ACCEPT_DATA | AIC_RX_FILTER_ACCEPT_OTHER_MGMT |	\
	 AIC_RX_FILTER_ACCEPT_MY_UNICAST |				\
	 AIC_RX_FILTER_ACCEPT_BROADCAST | AIC_RX_FILTER_ACCEPT_BEACON |	\
	 AIC_RX_FILTER_ACCEPT_PROBE_RESP | AIC_RX_FILTER_ACCEPT_BA |	\
	 AIC_RX_FILTER_ACCEPT_BAR | AIC_RX_FILTER_ACCEPT_OTHER_DATA |	\
	 AIC_RX_FILTER_ACCEPT_PROBE_REQ | AIC_RX_FILTER_ACCEPT_PS_POLL)

/* Everything on the channel, for a monitor interface. */
#define AIC_RX_FILTER_MONITOR						\
	(AIC_RX_FILTER_DEFAULT | AIC_RX_FILTER_ACCEPT_OTHER_BSSID |	\
	 AIC_RX_FILTER_ACCEPT_ALL_BEACON | AIC_RX_FILTER_ACCEPT_UNICAST |\
	 AIC_RX_FILTER_ACCEPT_MULTICAST | AIC_RX_FILTER_ACCEPT_UNKNOWN |	\
	 AIC_RX_FILTER_ACCEPT_OTHER_CNTRL | AIC_RX_FILTER_ACCEPT_CF_END |\
	 AIC_RX_FILTER_ACCEPT_ACK | AIC_RX_FILTER_ACCEPT_CTS |		\
	 AIC_RX_FILTER_ACCEPT_RTS)

/* number of outstanding confirmations the firmware can refer back to */
#define AIC_TXCFM_RING_SIZE		64

#endif /* AIC_FW_DESC_H */
