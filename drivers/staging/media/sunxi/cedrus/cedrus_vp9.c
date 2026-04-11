// SPDX-License-Identifier: GPL-2.0
/*
 * Cedrus VPU driver
 *
 * Copyright (c) 2024 Jernej Skrabec <jernej.skrabec@gmail.com>
 *
 * VP9 decoder for the Allwinner cedrus VE engine.
 *
 * The VP9 engine sits at the same register bank offset as H.265
 * (VE_ENGINE_DEC_H265 = 0x500).  The register layout was reverse-
 * engineered from the vendor libawvp9HwAL.so blob and cross-
 * referenced against the hantro G2 VP9 implementation.
 */

#include <linux/types.h>
#include <media/v4l2-vp9.h>
#include <media/videobuf2-dma-contig.h>

#include "cedrus.h"
#include "cedrus_hw.h"
#include "cedrus_regs.h"
#include "cedrus_video.h"

/* ------------------------------------------------------------------ */
/* Register offsets inside the VP9 engine (VE_ENGINE_DEC_H265 base)   */
/* All derived from Vp9AsicSetTileInfo / VP9SetBasicReg / …           */
/* ------------------------------------------------------------------ */

/* Header syntax register: frame_type, loop-filter, tiles, segments … */
#define VE_VP9_HDR_SYN			(VE_ENGINE_DEC_H265 + 0x04)
/* tiles_enabled_flag */
#define VE_VP9_HDR_SYN_TILES_EN		BIT(0)
/* inter frame */
#define VE_VP9_HDR_SYN_FRAME_TYPE	BIT(1)
/* bit_depth − 8 (3 bits) */
#define VE_VP9_HDR_SYN_DEPTH(d)		SHIFT_AND_MASK_BITS(d, 4, 2)
/* ref-frame sign-bias last/golden/altref */
#define VE_VP9_HDR_SYN_BIAS_LAST	BIT(5)
#define VE_VP9_HDR_SYN_BIAS_GOLDEN	BIT(6)
#define VE_VP9_HDR_SYN_BIAS_ALT		BIT(7)
#define VE_VP9_HDR_SYN_HP_MV		BIT(8)
/* interp_filter (3 bits) */
#define VE_VP9_HDR_SYN_INTERP(v)	SHIFT_AND_MASK_BITS(v, 11, 9)
#define VE_VP9_HDR_SYN_LF_EN		BIT(12)
/* loop-filter sharpness (3 bits) */
#define VE_VP9_HDR_SYN_LF_SHARP(v)	SHIFT_AND_MASK_BITS(v, 15, 13)
#define VE_VP9_HDR_SYN_LOSSLESS	BIT(16)
#define VE_VP9_HDR_SYN_SEG_EN		BIT(17)
#define VE_VP9_HDR_SYN_SEG_UPD_MAP	BIT(18)
#define VE_VP9_HDR_SYN_SEG_TEMP_UPD	BIT(19)
/* tx_mode (3 bits) */
#define VE_VP9_HDR_SYN_TX_MODE(v)	SHIFT_AND_MASK_BITS(v, 22, 20)
/* comp_pred_mode / reference_mode (2 bits) */
#define VE_VP9_HDR_SYN_REF_MODE(v)	SHIFT_AND_MASK_BITS(v, 24, 23)
/* use_prev_frame_mvs → temporal MV enabled */
#define VE_VP9_HDR_SYN_TEMP_MV		BIT(25)
/* always set (vendor always ORs in 0x4000000) */
#define VE_VP9_HDR_SYN_HW_EN2		BIT(26)
/* entropy_decoding_counts_enabled (non-parallel) */
#define VE_VP9_HDR_SYN_CNT_EN		BIT(27)
/* vp9_decoder_enabled (always 1) */
#define VE_VP9_HDR_SYN_DEC_EN		BIT(31)

/* Current picture size (width[13:0] | height[29:16]) */
#define VE_VP9_PIC_SIZE			(VE_ENGINE_DEC_H265 + 0x08)
#define VE_VP9_PIC_WIDTH(w)		SHIFT_AND_MASK_BITS(w, 13, 0)
#define VE_VP9_PIC_HEIGHT(h)		SHIFT_AND_MASK_BITS(h, 29, 16)

/* Reference frame picture sizes – last / golden / altref */
#define VE_VP9_LAST_PIC_SIZE		(VE_ENGINE_DEC_H265 + 0x0c)
#define VE_VP9_GOLDEN_PIC_SIZE		(VE_ENGINE_DEC_H265 + 0x10)
#define VE_VP9_ALTREF_PIC_SIZE		(VE_ENGINE_DEC_H265 + 0x14)

/* Scale parameters: step-x-1 [4:0], scale-x [20:5] */
#define VE_VP9_LAST_SCALE0		(VE_ENGINE_DEC_H265 + 0x18)
#define VE_VP9_LAST_SCALE1		(VE_ENGINE_DEC_H265 + 0x1c)
#define VE_VP9_GOLDEN_SCALE0		(VE_ENGINE_DEC_H265 + 0x20)
#define VE_VP9_GOLDEN_SCALE1		(VE_ENGINE_DEC_H265 + 0x24)
#define VE_VP9_ALTREF_SCALE0		(VE_ENGINE_DEC_H265 + 0x28)
#define VE_VP9_ALTREF_SCALE1		(VE_ENGINE_DEC_H265 + 0x2c)

/*
 * Stride alignment log2 stored in bits [30:28] of LAST_SCALE1.
 * The vendor stores log2(align/8) there for the primary luma stride.
 * We always use 16-byte alignment → log2(16/8)=1.
 */
#define VE_VP9_SCALE1_STRIDE_ALIGN(v)	SHIFT_AND_MASK_BITS(v, 30, 28)

/* Functional control: interrupt enables, cache, bypass, … */
#define VE_VP9_FUNC_CTRL		(VE_ENGINE_DEC_H265 + 0x30)
#define VE_VP9_FUNC_CTRL_FINISH_INT	BIT(0)
#define VE_VP9_FUNC_CTRL_ERR_INT	BIT(1)
#define VE_VP9_FUNC_CTRL_DATA_REQ_INT	BIT(2)
#define VE_VP9_FUNC_CTRL_MCRI_CACHE	BIT(10)
/* ddr_consistency_en: set for widths in [0x81,0xc0] and height>64 */
#define VE_VP9_FUNC_CTRL_DDR_CONS	BIT(31)

/* Trigger (same semantics as H.265 trigger) */
#define VE_VP9_TRIGGER			(VE_ENGINE_DEC_H265 + 0x34)
#define VE_VP9_TRIGGER_DEC_SLICE	0x8
#define VE_VP9_TRIGGER_INIT_SWDEC	0x7

/* Status / IRQ register */
#define VE_VP9_STATUS			(VE_ENGINE_DEC_H265 + 0x38)
#define VE_VP9_STATUS_DEC_FINISH	BIT(0)
#define VE_VP9_STATUS_DEC_ERROR		BIT(1)
#define VE_VP9_STATUS_VLD_DATA_REQ	BIT(2)
#define VE_VP9_STATUS_BS_DMA_BUSY	BIT(19)

/* CTB counter (write 0 before trigger) */
#define VE_VP9_DEC_CTB_NUM		(VE_ENGINE_DEC_H265 + 0x3c)

/* Bitstream registers */
#define VE_VP9_BITS_ADDR		(VE_ENGINE_DEC_H265 + 0x40)
#define VE_VP9_BITS_ADDR_FIRST		BIT(30)
#define VE_VP9_BITS_ADDR_LAST		BIT(29)
#define VE_VP9_BITS_ADDR_VALID		BIT(28)
#define VE_VP9_BITS_ADDR_BASE(a)	((((a) & 0x0fffff00) | (((a) >> 28) & 0xf)))
#define VE_VP9_BITS_OFFSET		(VE_ENGINE_DEC_H265 + 0x44)
#define VE_VP9_BITS_LEN			(VE_ENGINE_DEC_H265 + 0x48)
#define VE_VP9_BITS_END			(VE_ENGINE_DEC_H265 + 0x4c)
#define VE_VP9_BITS_END_ADDR(a)		(((a) >> 10) & GENMASK(29, 0))

/* Scale-down (secondary output) control */
#define VE_VP9_SD_CTRL			(VE_ENGINE_DEC_H265 + 0x50)
#define VE_VP9_SD_LUMA_ADDR		(VE_ENGINE_DEC_H265 + 0x54)
#define VE_VP9_SD_CHROMA_ADDR		(VE_ENGINE_DEC_H265 + 0x58)

/* Segment feature register */
#define VE_VP9_SEG_FEAT			(VE_ENGINE_DEC_H265 + 0x5c)

/* Neighbor info buffer address */
#define VE_VP9_NEIGHBOR_ADDR		(VE_ENGINE_DEC_H265 + 0x60)
#define VE_VP9_NEIGHBOR_ADDR_BASE(a)	(((a) >> 10) & GENMASK(29, 0))

/* Entry-point / probability table buffer address */
#define VE_VP9_ENTRY_POINT_ADDR		(VE_ENGINE_DEC_H265 + 0x64)
#define VE_VP9_ENTRY_POINT_BASE(a)	(((a) >> 10) & GENMASK(29, 0))

/* First tile start/end CTB coordinates */
#define VE_VP9_TILE_START		(VE_ENGINE_DEC_H265 + 0x68)
#define VE_VP9_TILE_START_INVALID	BIT(31)
#define VE_VP9_TILE_START_X(x)		SHIFT_AND_MASK_BITS(x, 8, 0)
#define VE_VP9_TILE_START_Y(y)		SHIFT_AND_MASK_BITS(y, 24, 16)
#define VE_VP9_TILE_END			(VE_ENGINE_DEC_H265 + 0x6c)
#define VE_VP9_TILE_END_X(x)		SHIFT_AND_MASK_BITS(x, 8, 0)
#define VE_VP9_TILE_END_Y(y)		SHIFT_AND_MASK_BITS(y, 24, 16)

/* Altref chroma buffer (note: unusual placement at 0x7c) */
#define VE_VP9_ALTREF_CHROMA_ADDR	(VE_ENGINE_DEC_H265 + 0x7c)

/* 10-bit lower-2-bit plane offset */
#define VE_VP9_10BIT_OFFSET_ADDR	(VE_ENGINE_DEC_H265 + 0x84)

/* 10-bit stride configuration */
#define VE_VP9_10BIT_CFG		(VE_ENGINE_DEC_H265 + 0x8c)
#define VE_VP9_10BIT_CFG_STRIDE(s)	SHIFT_AND_MASK_BITS(s, 11, 0)

/* Current frame reconstruction buffers */
#define VE_VP9_CUR_LUMA_ADDR		(VE_ENGINE_DEC_H265 + 0x90)
#define VE_VP9_CUR_CHROMA_ADDR		(VE_ENGINE_DEC_H265 + 0x94)

/* Reference frame reconstruction buffers */
#define VE_VP9_LAST_LUMA_ADDR		(VE_ENGINE_DEC_H265 + 0x98)
#define VE_VP9_LAST_CHROMA_ADDR		(VE_ENGINE_DEC_H265 + 0x9c)
#define VE_VP9_GOLDEN_LUMA_ADDR		(VE_ENGINE_DEC_H265 + 0xa0)
#define VE_VP9_GOLDEN_CHROMA_ADDR	(VE_ENGINE_DEC_H265 + 0xa4)
#define VE_VP9_ALTREF_LUMA_ADDR		(VE_ENGINE_DEC_H265 + 0xa8)

/* Frame-buffer address encoding: addr >> 10, shifted left by 2 */
#define VE_VP9_FB_ADDR(a)		((((a) >> 10) & GENMASK(29, 0)) << 2)

/* Collocated MV buffer */
#define VE_VP9_COL_MV_ADDR		(VE_ENGINE_DEC_H265 + 0x78)

/* SRAM port (dequant tables + loop-filter levels) */
#define VE_VP9_SRAM_OFFSET		(VE_ENGINE_DEC_H265 + 0xe0)
#define VE_VP9_SRAM_OFFSET_DEQUANT	0x000
#define VE_VP9_SRAM_OFFSET_LF		0x100
#define VE_VP9_SRAM_DATA		(VE_ENGINE_DEC_H265 + 0xe4)

/* ------------------------------------------------------------------ */
/* Buffer sizes (vendor constants)                                     */
/* ------------------------------------------------------------------ */

/*
 * Probability table: 0x88000 bytes total.
 *   [0x0000 … 0x4aff]: Vp9EntropyProbs  (0x4b00 = sizeof)
 *   [0x4b00 … 0x7eff]: ctx_counters     (0x3398 = sizeof(Vp9EntropyCounts))
 *   [0x8000 …       ]: segment-id data  (variable, reserved in same alloc)
 */
#define CEDRUS_VP9_PROB_TBL_SIZE	0x88000

/* Offset of the ctx_counters region inside prob_tbl */
#define CEDRUS_VP9_CTX_COUNTERS_OFFSET	0x4b00

/*
 * Neighbour info buffer.  The vendor allocates 0x1f4000 bytes.
 * This is mi_cols × mi_rows × (some neighbour info structure).
 * 4096×2304 / 8 / 8 = 18432 64×64 SBs → plenty of headroom at 2 MB.
 */
#define CEDRUS_VP9_NEIGHBOUR_SIZE	0x1f4000

/*
 * Collocated MV buffer.
 * The vendor code stores one entry per 8×8 block:
 *   mi_cols_aligned_to_sb × mi_rows × sizeof(MV) (≈ 8 bytes each).
 * We round up to 4096×2304 / 8 / 8 × 8 = 1,179,648 bytes.
 */
#define CEDRUS_VP9_COL_MV_SIZE		0x120000

/*
 * Segment map: one byte per 64×64 SB, two buffers (ping-pong).
 * (ALIGN(w,64)/64) × (ALIGN(h,64)/64) bytes each.
 * We pre-allocate for the maximum resolution supported.
 */
/*
 * Segment map size constant — kept for the software-only segment
 * tracking that mirrors the vendor vp9_update_segment_id() logic.
 * Not used for any hardware register programming.
 */
#define CEDRUS_VP9_SEG_MAP_SIZE_PER_BUF \
	(DIV_ROUND_UP(CEDRUS_MAX_WIDTH, 64) * DIV_ROUND_UP(CEDRUS_MAX_HEIGHT, 64))

/* ------------------------------------------------------------------ */
/* Hardware-written entropy counts (at prob_tbl + 0x4b00)             */
/* ------------------------------------------------------------------ */

/*
 * Motion-vector component counts written by the Cedrus VP9 hardware.
 * Matches the NmvContextCounts structure in the vendor blob.
 */
struct cedrus_vp9_nmv_counts {
	u32 joints[4];
	u32 sign[2][2];
	u32 classes[2][11];
	u32 class0[2][2];
	u32 bits[2][10][2];
	u32 class0_fp[2][2][4];
	u32 fp[2][4];
	u32 class0_hp[2][2];
	u32 hp[2][2];
};

/*
 * Full entropy counts written by the Cedrus VP9 hardware after each
 * non-parallel frame decode.  The hardware stores the region starting
 * at prob_tbl + CEDRUS_VP9_CTX_COUNTERS_OFFSET.
 *
 * Note: the vendor-blob decompilation shows inter_mode_counts[7][3],
 * but a size cross-check against the documented 0x3398-byte region
 * confirms the actual layout is [7][4] (four per-context mode counts:
 * NEARESTMV, NEARMV, ZEROMV, NEWMV), matching the v4l2 mv_mode[7][4]
 * layout used by v4l2_vp9_adapt_noncoef_probs().
 */
struct cedrus_vp9_entropy_counts {
	u32 inter_mode_counts[7][4];
	u32 sb_ymode_counts[4][10];
	u32 uv_mode_counts[10][10];
	u32 partition_counts[16][4];
	u32 switchable_interp_counts[4][3];
	u32 intra_inter_count[4][2];
	u32 comp_inter_count[5][2];
	u32 single_ref_count[5][2][2];
	u32 comp_ref_count[5][2];
	u32 tx32x32_count[2][4];
	u32 tx16x16_count[2][3];
	u32 tx8x8_count[2][2];
	u32 mbskip_count[3][2];
	struct cedrus_vp9_nmv_counts nmvcount;
	/* per-transform-size coefficient counts: [plane_type][coef_ctx][band][coef] */
	u32 count_coeffs[2][2][6][6][4];
	u32 count_coeffs8x8[2][2][6][6][4];
	u32 count_coeffs16x16[2][2][6][6][4];
	u32 count_coeffs32x32[2][2][6][6][4];
	/* eob branch [0] counts (not-eob): [tx_size][plane_type][coef_ctx][band][coef] */
	u32 count_eobs[4][2][2][6][6];
};

static_assert(sizeof(struct cedrus_vp9_entropy_counts) == 0x3398,
	      "cedrus_vp9_entropy_counts size mismatch");

/* ------------------------------------------------------------------ */
/* VP9 quantiser look-up tables (identical to the vendor blob)        */
/* ------------------------------------------------------------------ */

static const u16 vp9_dc_qlookup[256] = {
	4,    8,    8,    9,    10,   11,   12,   12,
	13,   14,   15,   16,   17,   18,   19,   19,
	20,   21,   22,   23,   24,   25,   26,   26,
	27,   28,   29,   30,   31,   32,   32,   33,
	34,   35,   36,   37,   38,   38,   39,   40,
	41,   42,   43,   43,   44,   45,   46,   47,
	48,   48,   49,   50,   51,   52,   53,   53,
	54,   55,   56,   57,   57,   58,   59,   60,
	61,   62,   62,   63,   64,   65,   66,   66,
	67,   68,   69,   70,   70,   71,   72,   73,
	74,   74,   75,   76,   77,   78,   78,   79,
	80,   81,   81,   82,   83,   84,   85,   85,
	87,   88,   90,   92,   93,   95,   96,   98,
	99,   101,  102,  104,  105,  107,  108,  110,
	111,  113,  114,  116,  117,  118,  120,  121,
	123,  125,  127,  129,  131,  134,  136,  138,
	140,  142,  144,  146,  148,  150,  152,  154,
	156,  158,  161,  164,  166,  169,  172,  174,
	177,  180,  182,  185,  187,  190,  192,  195,
	199,  202,  205,  208,  211,  214,  217,  220,
	223,  226,  230,  233,  237,  240,  243,  247,
	250,  253,  257,  261,  265,  269,  272,  276,
	280,  284,  288,  292,  296,  300,  304,  309,
	313,  317,  322,  326,  330,  335,  340,  344,
	349,  354,  359,  364,  369,  374,  379,  384,
	389,  395,  400,  406,  411,  417,  423,  429,
	435,  441,  447,  454,  461,  467,  475,  482,
	489,  497,  505,  513,  522,  530,  539,  549,
	559,  569,  579,  590,  602,  614,  626,  640,
	654,  668,  684,  700,  717,  736,  755,  775,
	796,  819,  843,  869,  896,  925,  955,  988,
	1022, 1058, 1098, 1139, 1184, 1232, 1282, 1336,
};

static const u16 vp9_ac_qlookup[256] = {
	4,    8,    9,    10,   11,   12,   13,   14,
	15,   16,   17,   18,   19,   20,   21,   22,
	23,   24,   25,   26,   27,   28,   29,   30,
	31,   32,   33,   34,   35,   36,   37,   38,
	39,   40,   41,   42,   43,   44,   45,   46,
	47,   48,   49,   50,   51,   52,   53,   54,
	55,   56,   57,   58,   59,   60,   61,   62,
	63,   64,   65,   66,   67,   68,   69,   70,
	71,   72,   73,   74,   75,   76,   77,   78,
	79,   80,   81,   82,   83,   84,   85,   86,
	87,   88,   89,   90,   91,   92,   93,   94,
	95,   96,   97,   98,   99,   100,  101,  102,
	104,  106,  108,  110,  112,  114,  116,  118,
	120,  122,  124,  126,  128,  130,  132,  134,
	136,  138,  140,  142,  144,  146,  148,  150,
	152,  155,  158,  161,  164,  167,  170,  173,
	176,  179,  182,  185,  188,  191,  194,  197,
	200,  203,  207,  211,  215,  219,  223,  227,
	231,  235,  239,  243,  247,  251,  255,  260,
	265,  270,  275,  280,  285,  290,  295,  300,
	305,  311,  317,  323,  329,  335,  341,  347,
	353,  359,  366,  373,  380,  387,  394,  401,
	408,  416,  424,  432,  440,  448,  456,  465,
	474,  483,  492,  502,  512,  522,  532,  542,
	552,  563,  574,  585,  596,  608,  620,  632,
	645,  658,  671,  684,  698,  712,  726,  741,
	756,  771,  786,  801,  817,  833,  849,  865,
	882,  899,  916,  933,  951,  969,  987,  1005,
	1024, 1044, 1065, 1086, 1108, 1130, 1153, 1176,
	1200, 1225, 1250, 1275, 1301, 1327, 1353, 1380,
	1408, 1436, 1465, 1494, 1524, 1554, 1585, 1616,
	1648, 1681, 1714, 1748, 1783, 1819, 1855, 1892,
};

/* ------------------------------------------------------------------ */
/* Context codec private data (embedded in cedrus_ctx)                */
/* ------------------------------------------------------------------ */

struct cedrus_vp9_ctx {
	/*
	 * prob_tbl: 0x88000 bytes
	 *   [0x0000]: Vp9EntropyProbs (passed to HW via entry_point_addr reg)
	 *   [0x4b00]: Vp9EntropyCounts (ctx counters, after each frame)
	 */
	void		*prob_tbl;
	dma_addr_t	prob_tbl_dma;

	/* Neighbour / context info buffer (0x1f4000 bytes) */
	void		*neighbour_buf;
	dma_addr_t	neighbour_buf_dma;

		/*
	 * Collocated MV buffer.
	 * The HW writes per-CTU motion vectors here after each inter frame
	 * and reads them back for temporal MV prediction on the next frame.
	 */
	void		*col_mv_buf;
	dma_addr_t	col_mv_buf_dma;

	/*
	 * Software segment map (two buffers, alternated per frame).
	 * Mirrors the vendor vp9_update_segment_id() post-processing:
	 * the HW writes packed segment IDs into prob_tbl+0x8000; we
	 * unpack them here in software after each frame.
	 * Not programmed into any hardware register.
	 */
	u8		seg_map[2][CEDRUS_VP9_SEG_MAP_SIZE_PER_BUF];
	unsigned int	cur_seg_map_idx;

	/* Dequant tables, one entry per segment [0..7] */
	s16		y_dequant[8][2];  /* [seg][DC, AC] */
	s16		uv_dequant[8][2]; /* [seg][DC, AC] */

	/* Loop-filter level table [seg][ref][mode] */
	u8		lf_lvl[8][4][2];

	/* VP9 frame context for probability adaptation */
	struct v4l2_vp9_frame_context frame_ctx[4];
	struct v4l2_vp9_frame_context probability_tables;
	unsigned int	frame_ctx_idx;

	/*
	 * Symbol counts read back from the hardware after each non-parallel
	 * frame.  Pointers are initialised once in cedrus_vp9_start() to
	 * point into the prob_tbl DMA buffer; they remain valid for the
	 * lifetime of the context.
	 */
	struct v4l2_vp9_frame_symbol_counts symbol_counts;

	/*
	 * tx16p_buf: the hardware writes tx16x16_count[2][3] but the v4l2
	 * API expects tx16p[2][4].  We copy and zero-pad here on every frame
	 * before calling v4l2_vp9_adapt_coef_probs().
	 */
	u32		tx16p_buf[2][4];

	/* Last decoded frame info, needed for temporal MV decision */
	u32		last_flags;
	bool		last_valid;
};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static inline struct cedrus_vp9_ctx *
to_vp9_ctx(struct cedrus_ctx *ctx)
{
	return ctx->codec.vp9;
}

/* VP9 dequantisation: base_q_idx + delta, clamped to [0..255] */
static int vp9_dc_quant(int qindex, int delta)
{
	return vp9_dc_qlookup[clamp(qindex + delta, 0, 255)];
}

static int vp9_ac_quant(int qindex, int delta)
{
	return vp9_ac_qlookup[clamp(qindex + delta, 0, 255)];
}

/*
 * Populate the dequant tables for all 8 segments.
 * Mirrors the vendor Vp9SetDequant() / vp9_get_qindex() logic.
 */
static void cedrus_vp9_build_dequant(struct cedrus_ctx *ctx,
				     const struct v4l2_ctrl_vp9_frame *dec)
{
	struct cedrus_vp9_ctx *vp9 = to_vp9_ctx(ctx);
	int i, qindex;

	for (i = 0; i < 8; i++) {
		qindex = v4l2_vp9_get_qindex(&dec->quant, &dec->seg, i);

		if (qindex == 0) {
			/* lossless */
			vp9->y_dequant[i][0]  = 1;
			vp9->y_dequant[i][1]  = 1;
			vp9->uv_dequant[i][0] = 1;
			vp9->uv_dequant[i][1] = 1;
		} else {
			vp9->y_dequant[i][0]  = vp9_dc_quant(qindex,
							      dec->quant.delta_q_y_dc);
			vp9->y_dequant[i][1]  = vp9_ac_quant(qindex, 0);
			vp9->uv_dequant[i][0] = vp9_dc_quant(qindex,
							      dec->quant.delta_q_uv_dc);
			vp9->uv_dequant[i][1] = vp9_ac_quant(qindex,
							      dec->quant.delta_q_uv_ac);
		}
	}
}

/*
 * Populate loop-filter level table for all 8 segments and all
 * ref-frame / mode combinations.  Mirrors Vp9LoopFilterFrameInit().
 */
static void cedrus_vp9_build_lf_lvl(struct cedrus_ctx *ctx,
				    const struct v4l2_ctrl_vp9_frame *dec)
{
	struct cedrus_vp9_ctx *vp9 = to_vp9_ctx(ctx);
	const struct v4l2_vp9_loop_filter *lf = &dec->lf;
	const struct v4l2_vp9_segmentation *seg = &dec->seg;
	bool seg_enabled = !!(seg->flags & V4L2_VP9_SEGMENTATION_FLAG_ENABLED);
	bool absolute = !!(seg->flags & V4L2_VP9_SEGMENTATION_FLAG_ABS_OR_DELTA_UPDATE);
	bool delta_en = !!(lf->flags & V4L2_VP9_LOOP_FILTER_FLAG_DELTA_ENABLED);
	int i, ref, mode;

	for (i = 0; i < 8; i++) {
		int seg_lf = lf->level;

		if (seg_enabled &&
		    v4l2_vp9_seg_feat_enabled(seg->feature_enabled,
					      V4L2_VP9_SEG_LVL_ALT_L, i)) {
			s16 delta = seg->feature_data[i][V4L2_VP9_SEG_LVL_ALT_L];

			seg_lf = absolute ? delta :
				 clamp((int)lf->level + delta, 0, 63);
		}

		for (ref = 0; ref < 4; ref++) {
			for (mode = 0; mode < 2; mode++) {
				int lvl = seg_lf;

				if (delta_en) {
					if (ref > 0)
						lvl = clamp(lvl + lf->ref_deltas[ref], 0, 63);
					if (mode > 0)
						lvl = clamp(lvl + lf->mode_deltas[mode - 1], 0, 63);
				}
				vp9->lf_lvl[i][ref][mode] = (u8)lvl;
			}
		}
	}
}

/*
 * Build the segment-feature register value.
 * The HW register packs skip and ref-frame enable bits and the
 * ref-frame value for all 8 segments into a single 32-bit word.
 * Layout per the vendor struct regVP9_SEGMENT_FEATURE:
 *   [7:0]   segment[0..7] ref-frame enable
 *   [15:8]  segment[0..7] skip enable
 *   [31:16] segment[0..7] ref-frame value (2 bits each)
 */
static u32 cedrus_vp9_seg_feat_reg(const struct v4l2_ctrl_vp9_frame *dec)
{
	const struct v4l2_vp9_segmentation *seg = &dec->seg;
	u32 reg = 0;
	int i;

	if (!(seg->flags & V4L2_VP9_SEGMENTATION_FLAG_ENABLED))
		return 0;

	for (i = 0; i < 8; i++) {
		if (seg->feature_enabled[i] &
		    V4L2_VP9_SEGMENT_FEATURE_ENABLED(V4L2_VP9_SEG_LVL_REF_FRAME)) {
			reg |= BIT(i);  /* ref_enable */
			reg |= (u32)(seg->feature_data[i][V4L2_VP9_SEG_LVL_REF_FRAME] & 3)
				<< (16 + i * 2);
		}
		if (seg->feature_enabled[i] &
		    V4L2_VP9_SEGMENT_FEATURE_ENABLED(V4L2_VP9_SEG_LVL_SKIP))
			reg |= BIT(8 + i);  /* skip_enable */
	}
	return reg;
}

/*
 * Write dequant tables and loop-filter levels into the SRAM port.
 * Mirrors VP9SetSramReg().
 */
static void cedrus_vp9_set_sram(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;
	struct cedrus_vp9_ctx *vp9 = to_vp9_ctx(ctx);
	int i;

	/* Select SRAM offset 0x000: dequant tables */
	cedrus_write(dev, VE_VP9_SRAM_OFFSET, VE_VP9_SRAM_OFFSET_DEQUANT);

	/* Write y_dequant for all segments (8 × 2 × 2 bytes = 32 bytes = 8 u32s) */
	for (i = 0; i < 8; i++) {
		u32 val = ((u32)(u16)vp9->y_dequant[i][0]) |
			  ((u32)(u16)vp9->y_dequant[i][1] << 16);
		cedrus_write(dev, VE_VP9_SRAM_DATA, val);
	}

	/* Write uv_dequant for all segments */
	for (i = 0; i < 8; i++) {
		u32 val = ((u32)(u16)vp9->uv_dequant[i][0]) |
			  ((u32)(u16)vp9->uv_dequant[i][1] << 16);
		cedrus_write(dev, VE_VP9_SRAM_DATA, val);
	}

	/* Select SRAM offset 0x100: loop-filter levels */
	cedrus_write(dev, VE_VP9_SRAM_OFFSET, VE_VP9_SRAM_OFFSET_LF);

	/*
	 * The HW expects lf_lvl[seg][ref][mode] packed as pairs of u32:
	 * word0: lvl[0][0][0]  | lvl[0][0][1]<<8  | [reserved]<<16 |
	 *        lvl[0][1][0]<<16 (vendor packs 6 values per SB row)
	 * Actually the vendor writes 8 bytes per segment row using two
	 * consecutive 32-bit writes to 0xe4.
	 * Format (from Vp9AsicSetSramLF in vendor code):
	 *   u32[0]: lvl[0] | lvl[2]<<16 | lvl[3]<<24   (lvl[1] skipped?)
	 *   u32[1]: lvl[4]>>0 | lvl[5]<<8 | lvl[6]<<16 | lvl[7]<<24
	 * where lvl[] = lf_lvl[seg][0..3][0..1] flattened.
	 */
	for (i = 0; i < 8; i++) {
		const u8 *l = vp9->lf_lvl[i][0];
		/* l[0..7] = lf_lvl[i][0][0], [i][0][1], [i][1][0]…[i][3][1] */
		u32 w0 = (l[0] & 0x3f) | ((l[2] & 0x3f) << 16) |
			 ((l[3] & 0x3f) << 24);
		u32 w1 = (l[4] & 0x3f) | ((l[5] & 0x3f) << 8) |
			 ((l[6] & 0x3f) << 16) | ((l[7] & 0x3f) << 24);

		cedrus_write(dev, VE_VP9_SRAM_DATA, w0);
		cedrus_write(dev, VE_VP9_SRAM_DATA, w1);
	}
}

/*
 * Copy the Vp9EntropyProbs structure into the probability table buffer,
 * using the v4l2-vp9 library representation.
 * The buffer layout mirrors the vendor Vp9EntropyProbs struct:
 *   sizeof(Vp9EntropyProbs) = 0xc4d bytes (vendor constant).
 * We write the probability tables managed by the v4l2-vp9 library.
 */
static void cedrus_vp9_write_probs(struct cedrus_ctx *ctx,
				   const struct v4l2_ctrl_vp9_frame *dec)
{
	struct cedrus_vp9_ctx *vp9 = to_vp9_ctx(ctx);

	/* Copy the current probability tables into the hardware buffer. */
	memcpy(vp9->prob_tbl, &vp9->probability_tables,
	       sizeof(vp9->probability_tables));
}

/* ------------------------------------------------------------------ */
/* Scale factor helpers (mirrors Vp9RefFramSize)                       */
/* ------------------------------------------------------------------ */

/*
 * Compute the scale registers for one reference frame.
 * The HW expects:
 *   scale_reg0[4:0]  = x_step_q4 − 1
 *   scale_reg0[20:5] = x_scale_fp (fixed-point scale factor)
 *   scale_reg1[4:0]  = y_step_q4 − 1
 *   scale_reg1[20:5] = y_scale_fp
 * where x_scale_fp = (ref_width  << 14) / cur_width  (0x4000 = no scale)
 *       x_step_q4  = (ref_width  * 16 + cur_width/2) / cur_width
 * (matches vp9_setup_scale_factors_for_frame in libvpx).
 */
static void cedrus_vp9_ref_scale_regs(u32 ref_w, u32 ref_h,
				      u32 cur_w, u32 cur_h,
				      u32 *reg0, u32 *reg1)
{
	u32 xfp, yfp, xstep, ystep;

	if (ref_w == 0 || ref_h == 0 || cur_w == 0 || cur_h == 0) {
		*reg0 = 0;
		*reg1 = 0;
		return;
	}

	xfp   = (ref_w << 14) / cur_w;
	yfp   = (ref_h << 14) / cur_h;
	xstep = (ref_w * 16 + cur_w / 2) / cur_w;
	ystep = (ref_h * 16 + cur_h / 2) / cur_h;

	*reg0 = ((xstep - 1) & 0x1f) | ((xfp & 0xffff) << 5);
	*reg1 = ((ystep - 1) & 0x1f) | ((yfp & 0xffff) << 5);
}

/* ------------------------------------------------------------------ */
/* IRQ handling                                                        */
/* ------------------------------------------------------------------ */

/* forward declaration – defined later in this file */
static void cedrus_vp9_done(struct cedrus_ctx *ctx);

static enum cedrus_irq_status
cedrus_vp9_irq_status(struct cedrus_ctx *ctx)
{
	u32 reg = cedrus_read(ctx->dev, VE_VP9_STATUS);

	if (reg & (VE_VP9_STATUS_DEC_ERROR | VE_VP9_STATUS_VLD_DATA_REQ))
		return CEDRUS_IRQ_ERROR;

	if (reg & VE_VP9_STATUS_DEC_FINISH)
		return CEDRUS_IRQ_OK;

	return CEDRUS_IRQ_NONE;
}

static void cedrus_vp9_irq_clear(struct cedrus_ctx *ctx)
{
	u32 reg = cedrus_read(ctx->dev, VE_VP9_STATUS);

	cedrus_write(ctx->dev, VE_VP9_STATUS, reg);

	/* Perform probability adaptation now that the frame is done. */
	if (reg & VE_VP9_STATUS_DEC_FINISH)
		cedrus_vp9_done(ctx);
}

static void cedrus_vp9_irq_disable(struct cedrus_ctx *ctx)
{
	u32 reg = cedrus_read(ctx->dev, VE_VP9_FUNC_CTRL);

	cedrus_write(ctx->dev, VE_VP9_FUNC_CTRL,
		     reg & ~(VE_VP9_FUNC_CTRL_FINISH_INT |
			     VE_VP9_FUNC_CTRL_ERR_INT |
			     VE_VP9_FUNC_CTRL_DATA_REQ_INT));
}

/* ------------------------------------------------------------------ */
/* Main setup                                                          */
/* ------------------------------------------------------------------ */

static int cedrus_vp9_setup(struct cedrus_ctx *ctx, struct cedrus_run *run)
{
	const struct v4l2_ctrl_vp9_frame *dec;
	const struct v4l2_ctrl_vp9_compressed_hdr *prob_updates;
	struct cedrus_vp9_ctx *vp9 = to_vp9_ctx(ctx);
	struct cedrus_dev *dev = ctx->dev;
	struct vb2_buffer *src_buf = &run->src->vb2_buf;
	struct vb2_queue *cap_q = &ctx->fh.m2m_ctx->cap_q_ctx.q;
	dma_addr_t src_dma, src_end;
	dma_addr_t luma_addr, chroma_addr;
	u32 src_len, src_off;
	u32 hdr_syn, pic_size;
	unsigned int fctx_idx;
	bool intra_only, use_temporal_mvs;
	int ret;

	dec = cedrus_find_control_data(ctx, V4L2_CID_STATELESS_VP9_FRAME);
	if (!dec)
		return -EINVAL;

	prob_updates = cedrus_find_control_data(ctx,
					V4L2_CID_STATELESS_VP9_COMPRESSED_HDR);
	if (!prob_updates)
		return -EINVAL;

	/* ---- probability table management (mirrors start_prepare_run) ---- */
	fctx_idx = v4l2_vp9_reset_frame_ctx(dec, vp9->frame_ctx);
	vp9->frame_ctx_idx = fctx_idx;
	vp9->probability_tables = vp9->frame_ctx[fctx_idx];
	v4l2_vp9_fw_update_probs(&vp9->probability_tables, prob_updates, dec);

	intra_only = !!(dec->flags &
			(V4L2_VP9_FRAME_FLAG_KEY_FRAME |
			 V4L2_VP9_FRAME_FLAG_INTRA_ONLY));

	use_temporal_mvs = vp9->last_valid &&
			   !intra_only &&
			   !(dec->flags & V4L2_VP9_FRAME_FLAG_ERROR_RESILIENT) &&
			   !(vp9->last_flags & V4L2_VP9_FRAME_FLAG_KEY_FRAME) &&
			   (vp9->last_flags & V4L2_VP9_FRAME_FLAG_SHOW_FRAME);

	/* ---- dequant / LF tables ---- */
	cedrus_vp9_build_dequant(ctx, dec);
	cedrus_vp9_build_lf_lvl(ctx, dec);

	/* ---- enable engine ---- */
	ret = cedrus_engine_enable(ctx);
	if (ret)
		return ret;

	/* ---- VP9 mode (uses H265 engine slot, mode=4) ---- */
	/* VE_MODE was already written by cedrus_engine_enable(); we need
	 * to re-confirm for VP9 which shares the H.265 decode slot. */

	/* ---- bitstream setup ---- */
	src_dma = vb2_dma_contig_plane_dma_addr(src_buf, 0);
	src_len = vb2_get_plane_payload(src_buf, 0);
	src_end = src_dma + src_len;

	/*
	 * Bitstream starts right after the uncompressed + compressed headers.
	 * The tile data offset mirrors Vp9ConfigBitStreamRegister().
	 */
	src_off = dec->uncompressed_header_size + dec->compressed_header_size;

	cedrus_write(dev, VE_VP9_BITS_ADDR,
		     VE_VP9_BITS_ADDR_BASE(src_dma) |
		     VE_VP9_BITS_ADDR_FIRST |
		     VE_VP9_BITS_ADDR_VALID |
		     VE_VP9_BITS_ADDR_LAST);

	/* Bit offset into the base address */
	cedrus_write(dev, VE_VP9_BITS_OFFSET,
		     ((src_dma + src_off) & 0x7ffffff) << 3);

	/* Bitstream length in bits */
	cedrus_write(dev, VE_VP9_BITS_LEN,
		     ((src_len - src_off) & 0x7ffffff) << 3);

	cedrus_write(dev, VE_VP9_BITS_END,
		     VE_VP9_BITS_END_ADDR(src_end) << 2);

	/* Trigger SWDEC init (required before DEC_SLICE) */
	cedrus_write(dev, VE_VP9_TRIGGER, VE_VP9_TRIGGER_INIT_SWDEC);

	/* ---- write probability tables ---- */
	cedrus_vp9_write_probs(ctx, dec);

	/* ---- header syntax register ---- */
	hdr_syn = VE_VP9_HDR_SYN_DEC_EN | VE_VP9_HDR_SYN_HW_EN2;

	if (dec->tile_cols_log2 > 0 || dec->tile_rows_log2 > 0)
		hdr_syn |= VE_VP9_HDR_SYN_TILES_EN;

	if (!intra_only)
		hdr_syn |= VE_VP9_HDR_SYN_FRAME_TYPE;

	hdr_syn |= VE_VP9_HDR_SYN_DEPTH(dec->bit_depth - 8);

	if (dec->ref_frame_sign_bias & V4L2_VP9_SIGN_BIAS_LAST)
		hdr_syn |= VE_VP9_HDR_SYN_BIAS_LAST;
	if (dec->ref_frame_sign_bias & V4L2_VP9_SIGN_BIAS_GOLDEN)
		hdr_syn |= VE_VP9_HDR_SYN_BIAS_GOLDEN;
	if (dec->ref_frame_sign_bias & V4L2_VP9_SIGN_BIAS_ALT)
		hdr_syn |= VE_VP9_HDR_SYN_BIAS_ALT;

	if (dec->flags & V4L2_VP9_FRAME_FLAG_ALLOW_HIGH_PREC_MV)
		hdr_syn |= VE_VP9_HDR_SYN_HP_MV;

	hdr_syn |= VE_VP9_HDR_SYN_INTERP(dec->interpolation_filter);

	if (dec->lf.level) {
		hdr_syn |= VE_VP9_HDR_SYN_LF_EN;
		hdr_syn |= VE_VP9_HDR_SYN_LF_SHARP(dec->lf.sharpness);
	}

	if (dec->quant.base_q_idx == 0 &&
	    dec->quant.delta_q_y_dc == 0 &&
	    dec->quant.delta_q_uv_dc == 0 &&
	    dec->quant.delta_q_uv_ac == 0)
		hdr_syn |= VE_VP9_HDR_SYN_LOSSLESS;

	if (dec->seg.flags & V4L2_VP9_SEGMENTATION_FLAG_ENABLED) {
		hdr_syn |= VE_VP9_HDR_SYN_SEG_EN;
		if (dec->seg.flags & V4L2_VP9_SEGMENTATION_FLAG_UPDATE_MAP)
			hdr_syn |= VE_VP9_HDR_SYN_SEG_UPD_MAP;
		if (dec->seg.flags & V4L2_VP9_SEGMENTATION_FLAG_TEMPORAL_UPDATE)
			hdr_syn |= VE_VP9_HDR_SYN_SEG_TEMP_UPD;
	}

	hdr_syn |= VE_VP9_HDR_SYN_TX_MODE(prob_updates->tx_mode);
	hdr_syn |= VE_VP9_HDR_SYN_REF_MODE(dec->reference_mode);

	if (use_temporal_mvs)
		hdr_syn |= VE_VP9_HDR_SYN_TEMP_MV;

	/* entropy counts enabled when not frame-parallel */
	if (!(dec->flags & V4L2_VP9_FRAME_FLAG_PARALLEL_DEC_MODE))
		hdr_syn |= VE_VP9_HDR_SYN_CNT_EN;

	cedrus_write(dev, VE_VP9_HDR_SYN, hdr_syn);

	/* ---- picture size ---- */
	pic_size = VE_VP9_PIC_WIDTH(dec->frame_width_minus_1 + 1) |
		   VE_VP9_PIC_HEIGHT(dec->frame_height_minus_1 + 1);
	cedrus_write(dev, VE_VP9_PIC_SIZE, pic_size);

	/* ---- current frame stride alignment (no-scale path: key/intra) ---- */
	if (intra_only) {
		/*
		 * Vendor code computes log2(align/8) where align=16 → value=1.
		 * This is stored in bits [30:28] of LAST_SCALE1.
		 */
		cedrus_write(dev, VE_VP9_LAST_SCALE1,
			     VE_VP9_SCALE1_STRIDE_ALIGN(1));
	}

	/* ---- reference frame sizes and scale factors (inter frames) ---- */
	if (!intra_only) {
		u32 cur_w = dec->frame_width_minus_1 + 1;
		u32 cur_h = dec->frame_height_minus_1 + 1;
		struct vb2_buffer *last_buf, *golden_buf, *alt_buf;
		u32 last_w, last_h, golden_w, golden_h, alt_w, alt_h;
		u32 s0, s1;

		last_buf = vb2_find_buffer(cap_q, dec->last_frame_ts);
		golden_buf = vb2_find_buffer(cap_q, dec->golden_frame_ts);
		alt_buf = vb2_find_buffer(cap_q, dec->alt_frame_ts);

		/* Fall back to current size if reference unavailable */
		last_w = last_buf ? ctx->dst_fmt.width : cur_w;
		last_h = last_buf ? ctx->dst_fmt.height : cur_h;
		golden_w = golden_buf ? ctx->dst_fmt.width : cur_w;
		golden_h = golden_buf ? ctx->dst_fmt.height : cur_h;
		alt_w = alt_buf ? ctx->dst_fmt.width : cur_w;
		alt_h = alt_buf ? ctx->dst_fmt.height : cur_h;

		cedrus_write(dev, VE_VP9_LAST_PIC_SIZE,
			     VE_VP9_PIC_WIDTH(last_w) |
			     VE_VP9_PIC_HEIGHT(last_h));
		cedrus_write(dev, VE_VP9_GOLDEN_PIC_SIZE,
			     VE_VP9_PIC_WIDTH(golden_w) |
			     VE_VP9_PIC_HEIGHT(golden_h));
		cedrus_write(dev, VE_VP9_ALTREF_PIC_SIZE,
			     VE_VP9_PIC_WIDTH(alt_w) |
			     VE_VP9_PIC_HEIGHT(alt_h));

		cedrus_vp9_ref_scale_regs(last_w, last_h, cur_w, cur_h,
					  &s0, &s1);
		cedrus_write(dev, VE_VP9_LAST_SCALE0, s0);
		/* stride alignment also in LAST_SCALE1 [30:28] */
		cedrus_write(dev, VE_VP9_LAST_SCALE1,
			     s1 | VE_VP9_SCALE1_STRIDE_ALIGN(1));

		cedrus_vp9_ref_scale_regs(golden_w, golden_h, cur_w, cur_h,
					  &s0, &s1);
		cedrus_write(dev, VE_VP9_GOLDEN_SCALE0, s0);
		cedrus_write(dev, VE_VP9_GOLDEN_SCALE1, s1);

		cedrus_vp9_ref_scale_regs(alt_w, alt_h, cur_w, cur_h,
					  &s0, &s1);
		cedrus_write(dev, VE_VP9_ALTREF_SCALE0, s0);
		cedrus_write(dev, VE_VP9_ALTREF_SCALE1, s1);

		/* ---- reference frame buffers ---- */
		cedrus_write_ref_buf_addr(ctx, cap_q, dec->last_frame_ts,
					  VE_VP9_LAST_LUMA_ADDR,
					  VE_VP9_LAST_CHROMA_ADDR);
		cedrus_write_ref_buf_addr(ctx, cap_q, dec->golden_frame_ts,
					  VE_VP9_GOLDEN_LUMA_ADDR,
					  VE_VP9_GOLDEN_CHROMA_ADDR);

		/* Altref: luma at 0xa8, chroma at 0x7c (unusual) */
		{
			struct vb2_buffer *buf = vb2_find_buffer(cap_q,
							dec->alt_frame_ts);
			dma_addr_t y = cedrus_dst_buf_addr(ctx, buf, 0);
			dma_addr_t c = cedrus_dst_buf_addr(ctx, buf, 1);

			cedrus_write(dev, VE_VP9_ALTREF_LUMA_ADDR,
				     VE_VP9_FB_ADDR(y));
			cedrus_write(dev, VE_VP9_ALTREF_CHROMA_ADDR,
				     VE_VP9_FB_ADDR(c));
		}
	}

	/* ---- current frame output buffers ---- */
	luma_addr = cedrus_dst_buf_addr(ctx, &run->dst->vb2_buf, 0);
	chroma_addr = cedrus_dst_buf_addr(ctx, &run->dst->vb2_buf, 1);
	cedrus_write(dev, VE_VP9_CUR_LUMA_ADDR, VE_VP9_FB_ADDR(luma_addr));
	cedrus_write(dev, VE_VP9_CUR_CHROMA_ADDR, VE_VP9_FB_ADDR(chroma_addr));

	/* ---- 10-bit support ---- */
	if (dec->bit_depth > 8) {
		u32 w_aligned = ALIGN(dec->frame_width_minus_1 + 1, 16);
		u32 h_aligned = ALIGN(dec->frame_height_minus_1 + 1, 16);
		u32 offset_addr;
		u32 stride_2bit;

		/*
		 * The lower-2-bit plane is appended right after the 8-bit
		 * luma/chroma planes.  The offset from the start of the
		 * luma buffer is chroma_offset + chroma_size/2.
		 */
		offset_addr = ((u32)(chroma_addr - luma_addr) +
			       (w_aligned >> 1) * (h_aligned >> 1)) & 0xfffffff;
		cedrus_write(dev, VE_VP9_10BIT_OFFSET_ADDR, offset_addr);

		stride_2bit = ALIGN(ALIGN(dec->frame_width_minus_1 + 1, 4) >> 2, 32);
		cedrus_write(dev, VE_VP9_10BIT_CFG,
			     VE_VP9_10BIT_CFG_STRIDE(stride_2bit));
	}

	/* ---- neighbour / entry-point (probability) buffer ---- */
	cedrus_write(dev, VE_VP9_NEIGHBOR_ADDR,
		     VE_VP9_NEIGHBOR_ADDR_BASE(vp9->neighbour_buf_dma) << 2);
	cedrus_write(dev, VE_VP9_ENTRY_POINT_ADDR,
		     VE_VP9_ENTRY_POINT_BASE(vp9->prob_tbl_dma) << 2);

	/* ---- segment feature register ---- */
	cedrus_write(dev, VE_VP9_SEG_FEAT,
		     cedrus_vp9_seg_feat_reg(dec));

	/* ---- tile start/end CTB coordinates ---- */
	{
		u32 mi_cols = DIV_ROUND_UP(dec->frame_width_minus_1 + 1, 8);
		u32 mi_rows = DIV_ROUND_UP(dec->frame_height_minus_1 + 1, 8);
		u32 tile_col_end = get_tile_offset(1, mi_cols,
						   dec->tile_cols_log2);
		u32 tile_row_end = get_tile_offset(1, mi_rows,
						   dec->tile_rows_log2);
		u32 start_x = get_tile_offset(0, mi_cols, dec->tile_cols_log2);
		u32 start_y = get_tile_offset(0, mi_rows, dec->tile_rows_log2);
		u32 end_x   = tile_col_end - 1;
		u32 end_y   = tile_row_end - 1;
		u32 tile_invalid = 0;

		/* A tile is "invalid" when it is a single CTB wide/tall */
		if (start_x == tile_col_end || start_y == tile_row_end)
			tile_invalid = VE_VP9_TILE_START_INVALID;

		cedrus_write(dev, VE_VP9_TILE_START,
			     tile_invalid |
			     VE_VP9_TILE_START_X(start_x >> 3) |
			     VE_VP9_TILE_START_Y(start_y >> 3));
		cedrus_write(dev, VE_VP9_TILE_END,
			     VE_VP9_TILE_END_X(end_x >> 3) |
			     VE_VP9_TILE_END_Y(end_y >> 3));
	}

	/* ---- collocated MV buffer ---- */
	cedrus_write(dev, VE_VP9_COL_MV_ADDR,
		     VE_VP9_FB_ADDR(vp9->col_mv_buf_dma));

	/* ---- segment map (ping-pong) ---- */
	{
		unsigned int cur_idx  = vp9->cur_seg_map_idx;
		unsigned int prev_idx = 1 - cur_idx;

		/*
		 * For key/intra frames: clear the previous segment map so
		 * the HW starts from a clean state.
		 */
		if (intra_only ||
		    (dec->flags & V4L2_VP9_FRAME_FLAG_ERROR_RESILIENT))
			memset(vp9->seg_map[prev_idx], 0,
			       CEDRUS_VP9_SEG_MAP_SIZE_PER_BUF);

		/*
		 * The vendor driver does not expose segment-map buffer
		 * addresses through any hardware register.  The hardware
		 * writes segment-ID output into prob_tbl+0x8000, and the
		 * vendor reads it back in software (vp9_update_segment_id)
		 * after each frame.  The seg_map[] arrays track per-frame
		 * segment assignments in software only.
		 */
	}

	/* ---- write dequant + loop-filter SRAM ---- */
	cedrus_vp9_set_sram(ctx);

	/* ---- functional control (interrupt enables + MCRI cache) ---- */
	{
		u32 w_align = ALIGN(dec->frame_width_minus_1 + 1, 16);
		u32 h_align = ALIGN(dec->frame_height_minus_1 + 1, 16);
		u32 fc = VE_VP9_FUNC_CTRL_FINISH_INT |
			 VE_VP9_FUNC_CTRL_ERR_INT |
			 VE_VP9_FUNC_CTRL_MCRI_CACHE;

		/*
		 * ddr_consistency_en: set when width is in (0x80, 0xc0] and
		 * height > 64 (vendor heuristic in Vp9AsicRun).
		 */
		if ((w_align - 0x81u) < 0x40 && h_align > 64)
			fc |= VE_VP9_FUNC_CTRL_DDR_CONS;

		cedrus_write(dev, VE_VP9_FUNC_CTRL, fc);
	}

	/* ---- reset CTB counter and top-level decode counter ---- */
	cedrus_write(dev, VE_VP9_DEC_CTB_NUM, 0);
	/* VE_VERSION+8 is the VETOP decode counter */
	cedrus_write(dev, VE_VERSION + 8, 0);

	return 0;
}

static void cedrus_vp9_trigger(struct cedrus_ctx *ctx)
{
	cedrus_write(ctx->dev, VE_VP9_TRIGGER, VE_VP9_TRIGGER_DEC_SLICE);
}

/* ------------------------------------------------------------------ */
/* Post-decode: probability adaptation                                 */
/* ------------------------------------------------------------------ */

/*
 * Called from the interrupt handler / done path in cedrus_dec.c (via
 * the stop() hook after the frame is signalled).
 */
static void cedrus_vp9_done(struct cedrus_ctx *ctx)
{
	struct cedrus_vp9_ctx *vp9 = to_vp9_ctx(ctx);
	const struct v4l2_ctrl_vp9_frame *dec;
	unsigned int fctx_idx = vp9->frame_ctx_idx;
	bool intra_only;

	dec = cedrus_find_control_data(ctx, V4L2_CID_STATELESS_VP9_FRAME);
	if (!dec)
		return;

	intra_only = !!(dec->flags &
			(V4L2_VP9_FRAME_FLAG_KEY_FRAME |
			 V4L2_VP9_FRAME_FLAG_INTRA_ONLY));

	if (dec->flags & V4L2_VP9_FRAME_FLAG_REFRESH_FRAME_CTX) {
		if (!(dec->flags & V4L2_VP9_FRAME_FLAG_PARALLEL_DEC_MODE)) {
			struct cedrus_vp9_entropy_counts *cnts =
				vp9->prob_tbl + CEDRUS_VP9_CTX_COUNTERS_OFFSET;
			struct v4l2_vp9_frame_symbol_counts *sc = &vp9->symbol_counts;
			const struct v4l2_ctrl_vp9_compressed_hdr *chdr;
			u8 tx_mode;
			int i;

			/*
			 * tx16x16_count[2][3]: the hardware writes 3 values per
			 * row but the v4l2 API expects [2][4].  Copy and pad.
			 */
			for (i = 0; i < 2; i++) {
				memcpy(vp9->tx16p_buf[i], cnts->tx16x16_count[i],
				       sizeof(cnts->tx16x16_count[0]));
				vp9->tx16p_buf[i][3] = 0;
			}
			sc->tx16p = &vp9->tx16p_buf;

			v4l2_vp9_adapt_coef_probs(&vp9->probability_tables,
						  sc,
						  !vp9->last_valid ||
						  (vp9->last_flags & V4L2_VP9_FRAME_FLAG_KEY_FRAME),
						  intra_only);

			if (!intra_only) {
				chdr = cedrus_find_control_data(ctx,
					V4L2_CID_STATELESS_VP9_COMPRESSED_HDR);
				tx_mode = chdr ? chdr->tx_mode : 0;

				v4l2_vp9_adapt_noncoef_probs(
					&vp9->probability_tables, sc,
					dec->reference_mode,
					dec->interpolation_filter,
					tx_mode, dec->flags);
			}
		}
		vp9->frame_ctx[fctx_idx] = vp9->probability_tables;
	}

	/* Advance segment map ping-pong */
	if (dec->seg.flags & V4L2_VP9_SEGMENTATION_FLAG_UPDATE_MAP)
		vp9->cur_seg_map_idx = 1 - vp9->cur_seg_map_idx;

	vp9->last_flags = dec->flags;
	vp9->last_valid = true;
}

/* ------------------------------------------------------------------ */
/* Entropy-count pointer initialisation                                */
/* ------------------------------------------------------------------ */

/*
 * Return a pointer to the coefficient counts array for the given
 * transform size (i) and band coordinates.  The array has 4 elements:
 * [0..2] are the three coefficient probability counts used by
 * v4l2_vp9_adapt_coef_probs(), and [3] is the eob branch-1 count.
 */
static void *cedrus_vp9_get_coeff_arr(struct cedrus_vp9_entropy_counts *cnts,
				      int i, int j, int k, int l, int m)
{
	switch (i) {
	case 0: return cnts->count_coeffs[j][k][l][m];
	case 1: return cnts->count_coeffs8x8[j][k][l][m];
	case 2: return cnts->count_coeffs16x16[j][k][l][m];
	case 3: return cnts->count_coeffs32x32[j][k][l][m];
	}
	return NULL;
}

/*
 * Return a pointer to the eob branch-1 count for transform size i.
 * The hardware packs this as element [3] of the coefficient array.
 */
static u32 *cedrus_vp9_get_eob1(struct cedrus_vp9_entropy_counts *cnts,
				 int i, int j, int k, int l, int m)
{
	switch (i) {
	case 0: return &cnts->count_coeffs[j][k][l][m][3];
	case 1: return &cnts->count_coeffs8x8[j][k][l][m][3];
	case 2: return &cnts->count_coeffs16x16[j][k][l][m][3];
	case 3: return &cnts->count_coeffs32x32[j][k][l][m][3];
	}
	return NULL;
}

/*
 * Set up all static pointers in vp9->symbol_counts to point into the
 * hardware-written counts region at prob_tbl + CEDRUS_VP9_CTX_COUNTERS_OFFSET.
 * Must be called once after prob_tbl is allocated.  The tx16p pointer is
 * updated per-frame in cedrus_vp9_done() because it requires zero-padding.
 */
static void cedrus_vp9_init_symbol_counts(struct cedrus_vp9_ctx *vp9)
{
	struct cedrus_vp9_entropy_counts *cnts =
		vp9->prob_tbl + CEDRUS_VP9_CTX_COUNTERS_OFFSET;
	struct v4l2_vp9_frame_symbol_counts *sc = &vp9->symbol_counts;
	int i, j, k, l, m;

	sc->partition   = &cnts->partition_counts;
	sc->skip        = &cnts->mbskip_count;
	sc->intra_inter = &cnts->intra_inter_count;
	sc->tx32p       = &cnts->tx32x32_count;
	sc->tx8p        = &cnts->tx8x8_count;
	sc->y_mode      = &cnts->sb_ymode_counts;
	sc->uv_mode     = &cnts->uv_mode_counts;
	sc->comp        = &cnts->comp_inter_count;
	sc->comp_ref    = &cnts->comp_ref_count;
	sc->single_ref  = &cnts->single_ref_count;
	/*
	 * inter_mode_counts[7][4] stores per-context counts for each of the
	 * four inter modes (NEARESTMV, NEARMV, ZEROMV, NEWMV), matching the
	 * mv_mode[7][4] layout expected by merge_probs_variant_c().
	 */
	sc->mv_mode     = &cnts->inter_mode_counts;
	sc->filter      = &cnts->switchable_interp_counts;
	sc->mv_joint    = &cnts->nmvcount.joints;
	sc->sign        = &cnts->nmvcount.sign;
	sc->classes     = &cnts->nmvcount.classes;
	sc->class0      = &cnts->nmvcount.class0;
	sc->bits        = &cnts->nmvcount.bits;
	sc->class0_fp   = &cnts->nmvcount.class0_fp;
	sc->fp          = &cnts->nmvcount.fp;
	sc->class0_hp   = &cnts->nmvcount.class0_hp;
	sc->hp          = &cnts->nmvcount.hp;

	for (i = 0; i < 4; i++)
		for (j = 0; j < 2; j++)
			for (k = 0; k < 2; k++)
				for (l = 0; l < 6; l++)
					for (m = 0; m < 6; m++) {
						sc->coeff[i][j][k][l][m] =
							cedrus_vp9_get_coeff_arr(cnts, i, j, k, l, m);
						sc->eob[i][j][k][l][m][0] =
							&cnts->count_eobs[i][j][k][l][m];
						sc->eob[i][j][k][l][m][1] =
							cedrus_vp9_get_eob1(cnts, i, j, k, l, m);
					}
}

/* ------------------------------------------------------------------ */
/* Start / stop (called once per open/close)                           */
/* ------------------------------------------------------------------ */

static int cedrus_vp9_start(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;
	struct cedrus_vp9_ctx *vp9;

	vp9 = kzalloc(sizeof(*vp9), GFP_KERNEL);
	if (!vp9)
		return -ENOMEM;

	vp9->prob_tbl = dma_alloc_coherent(dev->dev, CEDRUS_VP9_PROB_TBL_SIZE,
					   &vp9->prob_tbl_dma, GFP_KERNEL);
	if (!vp9->prob_tbl)
		goto err_free_ctx;

	vp9->neighbour_buf = dma_alloc_coherent(dev->dev,
						CEDRUS_VP9_NEIGHBOUR_SIZE,
						&vp9->neighbour_buf_dma,
						GFP_KERNEL);
	if (!vp9->neighbour_buf)
		goto err_free_prob;

	vp9->col_mv_buf = dma_alloc_coherent(dev->dev, CEDRUS_VP9_COL_MV_SIZE,
					     &vp9->col_mv_buf_dma, GFP_KERNEL);
	if (!vp9->col_mv_buf)
		goto err_free_neighbour;

	/*
	 * Set up the static pointers in symbol_counts to point into the
	 * hardware-written counts region.  Must be done after prob_tbl is
	 * allocated so the pointers are valid for the whole session.
	 */
	cedrus_vp9_init_symbol_counts(vp9);

	ctx->codec.vp9 = vp9;

	return 0;

err_free_neighbour:
	dma_free_coherent(dev->dev, CEDRUS_VP9_NEIGHBOUR_SIZE,
			  vp9->neighbour_buf, vp9->neighbour_buf_dma);
err_free_prob:
	dma_free_coherent(dev->dev, CEDRUS_VP9_PROB_TBL_SIZE,
			  vp9->prob_tbl, vp9->prob_tbl_dma);
err_free_ctx:
	kfree(vp9);
	return -ENOMEM;
}

static void cedrus_vp9_stop(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;
	struct cedrus_vp9_ctx *vp9 = to_vp9_ctx(ctx);

	cedrus_engine_disable(dev);

	dma_free_coherent(dev->dev, CEDRUS_VP9_COL_MV_SIZE,
			  vp9->col_mv_buf, vp9->col_mv_buf_dma);
	dma_free_coherent(dev->dev, CEDRUS_VP9_NEIGHBOUR_SIZE,
			  vp9->neighbour_buf, vp9->neighbour_buf_dma);
	dma_free_coherent(dev->dev, CEDRUS_VP9_PROB_TBL_SIZE,
			  vp9->prob_tbl, vp9->prob_tbl_dma);
	kfree(vp9);
	ctx->codec.vp9 = NULL;
}

/* ------------------------------------------------------------------ */
/* ops table                                                           */
/* ------------------------------------------------------------------ */

struct cedrus_dec_ops cedrus_dec_ops_vp9 = {
	.irq_clear	= cedrus_vp9_irq_clear,
	.irq_disable	= cedrus_vp9_irq_disable,
	.irq_status	= cedrus_vp9_irq_status,
	.setup		= cedrus_vp9_setup,
	.start		= cedrus_vp9_start,
	.stop		= cedrus_vp9_stop,
	.trigger	= cedrus_vp9_trigger,
};
