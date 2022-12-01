// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Cedrus VPU driver
 *
 * Copyright (c) 2020 Jernej Skrabec <jernej.skrabec@siol.net>
 */

#include <linux/delay.h>
#include <linux/types.h>

#include <media/videobuf2-dma-contig.h>

#include "cedrus.h"
#include "cedrus_hw.h"
#include "cedrus_regs.h"

#define MV_BUF_SIZE			(64 * SZ_1K)
#define ACDC_BUF_SIZE			(16 * SZ_1K)
#define BITPLANES_BUF_SIZE		(16 * SZ_1K)

#define VC1_PROFILE_SIMPLE		0
#define VC1_PROFILE_MAIN		1
#define VC1_PROFILE_COMPLEX		2
#define VC1_PROFILE_ADVANCED		3

#define VC1_PICTURE_TYPE_I		0
#define VC1_PICTURE_TYPE_P		1
#define VC1_PICTURE_TYPE_B		2
#define VC1_PICTURE_TYPE_BI		3
#define VC1_PICTURE_TYPE_SKIPPED	4

#define VC1_FCM_PROGRESSIVE		0
#define VC1_FCM_INTERLACED_FRAME	1
#define VC1_FCM_INTERLACED_FIELD	2

#define VC1_MVMODE_1MV_HPEL_BILIN	0
#define VC1_MVMODE_1MV			1
#define VC1_MVMODE_1MV_HPEL		2
#define VC1_MVMODE_MIXED_MV		3
#define VC1_MVMODE_INTENSITY_COMP	4

#define VC1_CONDOVER_NONE		0
#define VC1_CONDOVER_ALL		1
#define VC1_CONDOVER_SELECT		2

#define VC1_QUANT_FRAME_IMPLICIT	0
#define VC1_QUANT_FRAME_EXPLICIT	1
#define VC1_QUANT_NON_UNIFORM		2
#define VC1_QUANT_UNIFORM		3

#define VC1_BITPLANE_OFFSET_ACPRED	0x0000
#define VC1_BITPLANE_OFFSET_OVERFLAGS	0x0400
#define VC1_BITPLANE_OFFSET_MVTYPEMB	0x0800
#define VC1_BITPLANE_OFFSET_SKIPMB	0x0C00
#define VC1_BITPLANE_OFFSET_DIRECTMB	0x1000
#define VC1_BITPLANE_OFFSET_FIELDTX	0x1400
#define VC1_BITPLANE_OFFSET_FORWARDMB	0x1800

#define FRACTION(num, denom) (((num) * 256) / (denom))

static const unsigned int vc1_fractions[] = {
	FRACTION(1, 2),
	FRACTION(1, 3),
	FRACTION(2, 3),
	FRACTION(1, 4),
	FRACTION(3, 4),
	FRACTION(1, 5),
	FRACTION(2, 5),
	FRACTION(3, 5),
	FRACTION(4, 5),
	FRACTION(1, 6),
	FRACTION(5, 6),
	FRACTION(1, 7),
	FRACTION(2, 7),
	FRACTION(3, 7),
	FRACTION(4, 7),
	FRACTION(5, 7),
	FRACTION(6, 7),
	FRACTION(1, 8),
	FRACTION(3, 8),
	FRACTION(5, 8),
	FRACTION(7, 8),
};

const uint8_t ff_vc1_pquant_table[3][32] = {
    /* Implicit quantizer */
    {  0,  1,  2,  3,  4,  5,  6,  7,  8,  6,  7,  8,  9, 10, 11, 12,
      13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 27, 29, 31 },
    /* Explicit quantizer, pquantizer uniform */
    {  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15,
      16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31 },
};

static const unsigned int vc1_mvmode_map[] = {3, 0, 2, 1};

static unsigned int cedrus_vc1_get_fraction(unsigned int index)
{
	if (index >= ARRAY_SIZE(vc1_fractions))
		return FRACTION(7, 8);

	return vc1_fractions[index];
}

static unsigned int cedrus_vc1_get_pq(unsigned int pqindex, unsigned int qmode)
{
	if (qmode == VC1_QUANT_FRAME_IMPLICIT)
		return ff_vc1_pquant_table[0][pqindex];
	else
		return ff_vc1_pquant_table[1][pqindex];
}
static unsigned int seq;
static void cedrus_vc1_bitplanes_setup(struct cedrus_ctx *ctx,
				       struct cedrus_run *run)
{
	const struct v4l2_vc1_entrypoint_header *entrypoint;
	const struct v4l2_ctrl_vc1_bitplanes *bitplanes;
	unsigned int mb_num, plane_size;

	entrypoint = &run->vc1.slice_params->entrypoint_header;
	bitplanes = run->vc1.bitplanes;

	/*
	 * FIXME: Not sure if max coded size or current code
	 * size is correct.
	 */
	mb_num = DIV_ROUND_UP(entrypoint->coded_width, 16) *
		 DIV_ROUND_UP(entrypoint->coded_height, 16);
	plane_size = DIV_ROUND_UP(mb_num, 8);

	/*if (plane_size > 1024) {
		printk("VC-1: Warning, bitplane size too big!\n");
		plane_size = 1024;
	}*/

	plane_size = 1024;

	if (bitplanes->bitplane_flags & V4L2_VC1_BITPLANE_FLAG_MVTYPEMB) {
		memcpy(ctx->codec.vc1.bitplanes_buf + VC1_BITPLANE_OFFSET_MVTYPEMB,
		       bitplanes->mvtypemb, plane_size);
		memcpy(ctx->codec.vc1.bitplanes_buf + VC1_BITPLANE_OFFSET_MVTYPEMB + 7 * SZ_1K,
		       bitplanes->mvtypemb + 1024, plane_size);
	}
	if (bitplanes->bitplane_flags & V4L2_VC1_BITPLANE_FLAG_DIRECTMB) {
		memcpy(ctx->codec.vc1.bitplanes_buf + VC1_BITPLANE_OFFSET_DIRECTMB,
		       bitplanes->directmb, plane_size);
		memcpy(ctx->codec.vc1.bitplanes_buf + VC1_BITPLANE_OFFSET_DIRECTMB + 7 * SZ_1K,
		       bitplanes->directmb + 1024, plane_size);
	}
	if (bitplanes->bitplane_flags & V4L2_VC1_BITPLANE_FLAG_SKIPMB) {
		memcpy(ctx->codec.vc1.bitplanes_buf + VC1_BITPLANE_OFFSET_SKIPMB,
		       bitplanes->skipmb, plane_size);
		memcpy(ctx->codec.vc1.bitplanes_buf + VC1_BITPLANE_OFFSET_SKIPMB + 7 * SZ_1K,
		       bitplanes->skipmb + 1024, plane_size);
	}
	if (bitplanes->bitplane_flags & V4L2_VC1_BITPLANE_FLAG_FIELDTX) {
		memcpy(ctx->codec.vc1.bitplanes_buf + VC1_BITPLANE_OFFSET_FIELDTX,
		       bitplanes->fieldtx, plane_size);
		memcpy(ctx->codec.vc1.bitplanes_buf + VC1_BITPLANE_OFFSET_FIELDTX + 7 * SZ_1K,
		       bitplanes->fieldtx + 1024, plane_size);
	}
	if (bitplanes->bitplane_flags & V4L2_VC1_BITPLANE_FLAG_FORWARDMB) {
		memcpy(ctx->codec.vc1.bitplanes_buf + VC1_BITPLANE_OFFSET_FORWARDMB,
		       bitplanes->forwardmb, plane_size);
		memcpy(ctx->codec.vc1.bitplanes_buf + VC1_BITPLANE_OFFSET_FORWARDMB + 7 * SZ_1K,
		       bitplanes->forwardmb + 1024, plane_size);
	}
	if (bitplanes->bitplane_flags & V4L2_VC1_BITPLANE_FLAG_ACPRED) {
		memcpy(ctx->codec.vc1.bitplanes_buf + VC1_BITPLANE_OFFSET_ACPRED,
		       bitplanes->acpred, plane_size);
		memcpy(ctx->codec.vc1.bitplanes_buf + VC1_BITPLANE_OFFSET_ACPRED + 7 * SZ_1K,
		       bitplanes->acpred + 1024, plane_size);
	}
	if (bitplanes->bitplane_flags & V4L2_VC1_BITPLANE_FLAG_OVERFLAGS) {
		memcpy(ctx->codec.vc1.bitplanes_buf + VC1_BITPLANE_OFFSET_OVERFLAGS,
		       bitplanes->overflags, plane_size);
		memcpy(ctx->codec.vc1.bitplanes_buf + VC1_BITPLANE_OFFSET_OVERFLAGS + 7 * SZ_1K,
		       bitplanes->overflags + 1024, plane_size);
	}
}

static enum cedrus_irq_status
cedrus_vc1_irq_status(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;
	u32 reg = cedrus_read(dev, VE_DEC_VC1_STATUS);

	if (reg & (VE_DEC_VC1_STATUS_ERROR |
		   VE_DEC_VC1_STATUS_VLD_DATA_REQ))
		return CEDRUS_IRQ_ERROR;

	if (reg & VE_DEC_VC1_STATUS_SUCCESS)
		return CEDRUS_IRQ_OK;

	return CEDRUS_IRQ_NONE;
}

static void cedrus_vc1_irq_clear(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;

	cedrus_write(dev, VE_DEC_VC1_STATUS,
		     VE_DEC_VC1_STATUS_INT_MASK);
}

static void cedrus_vc1_irq_disable(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;
	u32 reg = cedrus_read(dev, VE_DEC_VC1_CTRL);

	cedrus_write(dev, VE_DEC_VC1_CTRL,
		     reg & ~VE_DEC_VC1_CTRL_IRQ_MASK);
}

static const unsigned int vc1_dmvrange_map[] = {0, 2, 1, 3};
static u32 oldval;
static int intenen;
static u32 vc1_icb1_regbak, vc1_icb0_reg68, vc1_icf0_reg6c;

static int cedrus_vc1_setup(struct cedrus_ctx *ctx, struct cedrus_run *run)
{
	bool interlaced, top_field_first, second_field, ref_field, rangeredfrm;
	const struct v4l2_ctrl_vc1_slice_params *slice = run->vc1.slice_params;
	const struct v4l2_ctrl_vc1_bitplanes *bitplanes = run->vc1.bitplanes;
	const struct v4l2_vc1_entrypoint_header *entrypoint;
	struct cedrus_buffer *fwd_buf, *bwd_buf, *out_buf;
	struct vb2_buffer *src_buf = &run->src->vb2_buf;
	const struct v4l2_vc1_picture_layer *picture;
	const struct v4l2_vc1_vopdquant *vopdquant;
	dma_addr_t dst_luma_addr, dst_chroma_addr;
	const struct v4l2_vc1_sequence *sequence;
	const struct v4l2_vc1_metadata *metadata;
	unsigned int bfraction, frfd, mvmode;
	struct cedrus_dev *dev = ctx->dev;
	struct vb2_buffer *backward_vb2;
	struct vb2_buffer *forward_vb2;
	dma_addr_t src_buf_addr;
	u32 reg, condover, pq;
	struct vb2_queue *vq;
	size_t slice_bytes;
	int brfd, flag;

	unsigned int raw_coding = slice->raw_coding_flags;
	//printk("%d raw coding: %.2x, flags: %.2x\n", seq + 1, slice->raw_coding_flags, bitplanes->bitplane_flags);
	raw_coding = ~bitplanes->bitplane_flags;

	sequence = &slice->sequence;
	entrypoint = &slice->entrypoint_header;
	picture = &slice->picture_layer;
	vopdquant = &slice->vopdquant;
	metadata = &slice->metadata;

	second_field = !!(picture->flags & V4L2_VC1_PICTURE_LAYER_FLAG_SECOND_FIELD);
	top_field_first = !!(picture->flags & V4L2_VC1_PICTURE_LAYER_FLAG_TFF);
	interlaced = picture->fcm != VC1_FCM_PROGRESSIVE;
	ref_field = !!(picture->flags & V4L2_VC1_PICTURE_LAYER_FLAG_REFFIELD);
	rangeredfrm = !!(picture->flags & V4L2_VC1_PICTURE_LAYER_FLAG_RANGEREDFRM);
	pq = cedrus_vc1_get_pq(picture->pqindex, entrypoint->quantizer);
	flag = picture->intcompfield < 2 && picture->mvmode == VC1_MVMODE_INTENSITY_COMP;

	out_buf = vb2_to_cedrus_buffer(&run->dst->vb2_buf);
	out_buf->codec.vc1.rangeredfrm = rangeredfrm;
	out_buf->codec.vc1.interlaced = interlaced;
	out_buf->codec.vc1.ptype = picture->ptype;

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE);

	forward_vb2 = vb2_find_buffer(vq, slice->forward_ref_ts);
	//forward_vb2 = vb2_find_buffer(vq, slice->backward_ref_ts);
	//fwd_buf = NULL;
	if (forward_vb2)
		fwd_buf = vb2_to_cedrus_buffer(forward_vb2);
	else
		fwd_buf = out_buf;

	backward_vb2 = vb2_find_buffer(vq, slice->backward_ref_ts);
	//backward_vb2 = vb2_find_buffer(vq, slice->forward_ref_ts);
	//bwd_buf = NULL;
	if (backward_vb2)
		bwd_buf = vb2_to_cedrus_buffer(backward_vb2);
	else
		bwd_buf = out_buf;

	cedrus_engine_enable(ctx);

	/* Set bitstream source */

	slice_bytes = vb2_get_plane_payload(src_buf, 0);
	cedrus_write(dev, VE_DEC_VC1_BITS_LEN, slice_bytes * 8);
	cedrus_write(dev, VE_DEC_VC1_BITS_OFFSET, 0);

	src_buf_addr = vb2_dma_contig_plane_dma_addr(src_buf, 0);
	cedrus_write(dev, VE_DEC_VC1_BITS_END_ADDR,
		     src_buf_addr + slice_bytes);
	cedrus_write(dev, VE_DEC_VC1_BITS_ADDR,
		     VE_DEC_VC1_BITS_ADDR_BASE(src_buf_addr) |
		     VE_DEC_VC1_BITS_ADDR_VALID_SLICE_DATA |
		     VE_DEC_VC1_BITS_ADDR_LAST_SLICE_DATA |
		     VE_DEC_VC1_BITS_ADDR_FIRST_SLICE_DATA);

	/* Set auxiliary buffers */

	cedrus_write(dev, VE_DEC_VC1_DCACPRED_ADDR,
		     ctx->codec.vc1.acdc_buf_addr);
	cedrus_write(dev, VE_DEC_VC1_BITPLANE_ADDR,
		     ctx->codec.vc1.bitplanes_buf_addr);
	cedrus_write(dev, VE_DEC_VC1_MVINFO_ADDR,
		     ctx->codec.vc1.mv_buf_addr);

	cedrus_write(dev, VE_DEC_VC1_STATUS,
		     VE_DEC_VC1_STATUS_INT_MASK);

	cedrus_write(dev, VE_DEC_VC1_TRIGGER_TYPE,
		     VE_DEC_VC1_TRIGGER_TYPE_INIT_SWDEC);

	/* TODO: minimize time impact of this block */
	for (reg = 0; reg < slice->data_bit_offset; reg++) {
		cedrus_write(dev, VE_DEC_VC1_TRIGGER_TYPE,
				VE_DEC_VC1_TRIGGER_TYPE_GET_BITS |
				VE_DEC_VC1_TRIGGER_TYPE_N_BITS(1));

		cedrus_wait_for(dev, VE_DEC_VC1_STATUS, VE_DEC_VC1_STATUS_BITS_BUSY);
	}

	cedrus_write(dev, VE_DEC_VC1_ROT_CTRL, 0);

	cedrus_write(dev, VE_DEC_VC1_PICHDRLEN,
		     VE_DEC_VC1_PICHDRLEN_LENGTH(0));

	reg = VE_DEC_VC1_EPHS_PROFILE(sequence->profile);
	if (entrypoint->flags & V4L2_VC1_ENTRYPOINT_HEADER_FLAG_LOOPFILTER)
		reg |= VE_DEC_VC1_EPHS_LOOPFILTER;
	if (metadata->flags & V4L2_VC1_METADATA_FLAG_MULTIRES)
		reg |= VE_DEC_VC1_EPHS_MULTIRES;
	if (entrypoint->flags & V4L2_VC1_ENTRYPOINT_HEADER_FLAG_FASTUVMC)
		reg |= VE_DEC_VC1_EPHS_FASTUVMC;
	if (entrypoint->flags & V4L2_VC1_ENTRYPOINT_HEADER_FLAG_EXTENDED_DMV)
		reg |= VE_DEC_VC1_EPHS_EXTENDEDMV;
	reg |= VE_DEC_VC1_EPHS_DQUANT(entrypoint->dquant);
	if (entrypoint->flags & V4L2_VC1_ENTRYPOINT_HEADER_FLAG_VSTRANSFORM)
		reg |= VE_DEC_VC1_EPHS_VSTRANSFORM;
	if (entrypoint->flags & V4L2_VC1_ENTRYPOINT_HEADER_FLAG_OVERLAP)
		reg |= VE_DEC_VC1_EPHS_OVERLAP;
	reg |= VE_DEC_VC1_EPHS_QUANTIZER(entrypoint->quantizer);
	if (metadata->flags & V4L2_VC1_METADATA_FLAG_RANGERED)
		reg |= VE_DEC_VC1_EPHS_RANGERED;
	if (sequence->flags & V4L2_VC1_SEQUENCE_FLAG_FINTERPFLAG)
		reg |= VE_DEC_VC1_EPHS_FINTERPFLAG;
	if (metadata->flags & V4L2_VC1_METADATA_FLAG_SYNCMARKER)
		reg |= VE_DEC_VC1_EPHS_SYNCMARKER;
	if (sequence->profile != VC1_PROFILE_ADVANCED)
		reg |= VE_DEC_VC1_EPHS_EPTB_DET_BYPASS;
	cedrus_write(dev, VE_DEC_VC1_EPHS, reg);
	
	if (sequence->profile == VC1_PROFILE_ADVANCED)
		condover = picture->condover;
	else if (picture->ptype == VC1_PICTURE_TYPE_B || pq < 9 ||
		 !(entrypoint->flags & V4L2_VC1_ENTRYPOINT_HEADER_FLAG_OVERLAP))
		condover = VC1_CONDOVER_NONE;
	else
		condover = VC1_CONDOVER_ALL;

	reg = VE_DEC_VC1_PICCTRL_PTYPE(picture->ptype);
        reg |= VE_DEC_VC1_PICCTRL_FCM(picture->fcm ? picture->fcm + 1 : 0);
	if (interlaced && !(top_field_first ^ second_field))
		reg |= VE_DEC_VC1_PICCTRL_BOTTOM_FIELD;
	if (second_field)
		reg |= VE_DEC_VC1_PICCTRL_SECOND_FIELD;
	if (rangeredfrm)
		reg |= VE_DEC_VC1_PICCTRL_RANGEREDFRM;
	if (fwd_buf && fwd_buf->codec.vc1.rangeredfrm)
		reg |= VE_DEC_VC1_PICCTRL_FWD_RANGEREDFRM;
	if (bwd_buf && bwd_buf->codec.vc1.rangeredfrm)
		reg |= VE_DEC_VC1_PICCTRL_BWD_RANGEREDFRM;
	reg |= VE_DEC_VC1_PICCTRL_TRANSACFRM(picture->transacfrm);
	reg |= VE_DEC_VC1_PICCTRL_TRANSACFRM2(picture->transacfrm2);
	if (picture->flags & V4L2_VC1_PICTURE_LAYER_FLAG_TRANSDCTAB)
		reg |= VE_DEC_VC1_PICCTRL_TRANSDCTAB;
	if (picture->flags & V4L2_VC1_PICTURE_LAYER_FLAG_RNDCTRL)
		reg |= VE_DEC_VC1_PICCTRL_RNDCTRL;
	reg |= VE_DEC_VC1_PICCTRL_CONDOVER(condover ? condover + 1 : 0);
	if (raw_coding & V4L2_VC1_RAW_CODING_FLAG_ACPRED)
		reg |= VE_DEC_VC1_PICCTRL_ACPRED_RAW;
	if (raw_coding & V4L2_VC1_RAW_CODING_FLAG_OVERFLAGS)
		reg |= VE_DEC_VC1_PICCTRL_OVERFLAGS_RAW;
	reg |= VE_DEC_VC1_PICCTRL_CBPTAB(picture->cbptab);
	if (raw_coding & V4L2_VC1_RAW_CODING_FLAG_SKIPMB)
		reg |= VE_DEC_VC1_PICCTRL_SKIPMB_RAW;
	if (picture->flags & V4L2_VC1_PICTURE_LAYER_FLAG_TTMBF)
		reg |= VE_DEC_VC1_PICCTRL_TTMBF;
	reg |= VE_DEC_VC1_PICCTRL_TTFRM(picture->ttfrm);
	if (raw_coding & V4L2_VC1_RAW_CODING_FLAG_DIRECTMB)
		reg |= VE_DEC_VC1_PICCTRL_DIRECTMB_RAW;
	if (bwd_buf && bwd_buf->codec.vc1.ptype != VC1_PICTURE_TYPE_P)
	//if (picture->ptype != VC1_PICTURE_TYPE_P)
		reg |= VE_DEC_VC1_PICCTRL_DIRECT_REF_INTRA;
	if (bitplanes->bitplane_flags)
		reg |= VE_DEC_VC1_PICCTRL_BITPL_CODING;
	cedrus_write(dev, VE_DEC_VC1_PICCTRL, reg);

	reg = VE_DEC_VC1_PICQP_PQINDEX(picture->pqindex);
	if (picture->flags & V4L2_VC1_PICTURE_LAYER_FLAG_HALFQP)
		reg |= VE_DEC_VC1_PICQP_HALFQP;
	if (picture->flags & V4L2_VC1_PICTURE_LAYER_FLAG_PQUANTIZER)
		reg |= VE_DEC_VC1_PICQP_PQUANTIZER;
	reg |= VE_DEC_VC1_PICQP_DQPPROFILE(vopdquant->dqprofile);
	reg |= VE_DEC_VC1_PICQP_DQSBEDGE(vopdquant->dqsbedge);
	reg |= VE_DEC_VC1_PICQP_DQDBEDGE(vopdquant->dqdbedge);
	reg |= VE_DEC_VC1_PICQP_ALTPQUANT(vopdquant->altpquant);
	if (vopdquant->flags & V4L2_VC1_VOPDQUANT_FLAG_DQUANTFRM)
		reg |= VE_DEC_VC1_PICQP_DQUANTFRM;
	if (vopdquant->flags & V4L2_VC1_VOPDQUANT_FLAG_DQBILEVEL)
		reg |= VE_DEC_VC1_PICQP_DQBILEVEL;
	cedrus_write(dev, VE_DEC_VC1_PICQP, reg);

	bfraction = cedrus_vc1_get_fraction(picture->bfraction);
	reg = VE_DEC_VC1_PICMV_BFRACTION(bfraction);
	if (bfraction < FRACTION(1, 2))
		reg |= VE_DEC_VC1_PICMV_BFRAC_LESS_THAN_HALF;
	reg |= VE_DEC_VC1_PICMV_MVRANGE(picture->mvrange);
	/* FIXME: is this ok? */
	if (picture->ptype == VC1_PICTURE_TYPE_P &&
	    picture->mvmode == VC1_MVMODE_INTENSITY_COMP)
		mvmode = picture->mvmode2;
	else
		mvmode = picture->mvmode;
	reg |= VE_DEC_VC1_PICMV_MVMODE(vc1_mvmode_map[mvmode & 3]);
	//if ((picture->ptype == VC1_PICTURE_TYPE_B && fwd_buf && fwd_buf->codec.vc1.compen) ||
	//    (picture->ptype != VC1_PICTURE_TYPE_B && picture->flags & V4L2_VC1_PICTURE_LAYER_FLAG_INTCOMP))
	//if (picture->flags & V4L2_VC1_PICTURE_LAYER_FLAG_INTCOMP)
	if (picture->ptype == VC1_PICTURE_TYPE_B) {
		if (intenen)
			reg |= VE_DEC_VC1_PICMV_INTENSITY_COMP_EN;
	} else if (picture->flags & V4L2_VC1_PICTURE_LAYER_FLAG_INTCOMP) { // (flag) {
		reg |= VE_DEC_VC1_PICMV_INTENSITY_COMP_EN;
	}
	if (picture->ptype == VC1_PICTURE_TYPE_P)
		intenen = !!(reg & VE_DEC_VC1_PICMV_INTENSITY_COMP_EN);
	else if (picture->ptype == VC1_PICTURE_TYPE_I)
		intenen = 0;
	reg |= VE_DEC_VC1_PICMV_MVTAB(picture->mvtab);
	if (raw_coding & V4L2_VC1_RAW_CODING_FLAG_MVTYPEMB)
		reg |= VE_DEC_VC1_PICMV_MVTYPEMB_RAW;
	cedrus_write(dev, VE_DEC_VC1_PICMV, reg);

	if (picture->fcm == VC1_FCM_INTERLACED_FIELD) {
		if (picture->ptype == VC1_PICTURE_TYPE_I) {
			cedrus_write(dev, VE_DEC_VC1_PICINTENCOMP, 0);
			cedrus_write(dev, VE_ENGINE_DEC_VC1 + 0x68, 0);
			cedrus_write(dev, VE_ENGINE_DEC_VC1 + 0x6c, 0);
			vc1_icb1_regbak = 0;
			vc1_icb0_reg68 = 0;
		} else if (picture->ptype == VC1_PICTURE_TYPE_B) {
			cedrus_write(dev, VE_DEC_VC1_PICINTENCOMP, vc1_icb1_regbak & 0x3f3f3f3f);
			cedrus_write(dev, VE_ENGINE_DEC_VC1 + 0x68, vc1_icb0_reg68);
			if (second_field)
				reg = 0;
			else
				reg = vc1_icf0_reg6c;
			cedrus_write(dev, VE_ENGINE_DEC_VC1 + 0x6c, reg);
		} else if (picture->ptype == VC1_PICTURE_TYPE_B) {
			reg = VE_DEC_VC1_PICINTENCOMP_LUMASCALE1(picture->lumscale);
			reg |= VE_DEC_VC1_PICINTENCOMP_LUMASHIFT1(picture->lumshift);
			reg |= VE_DEC_VC1_PICINTENCOMP_LUMASCALE2(picture->lumscale2);
			reg |= VE_DEC_VC1_PICINTENCOMP_LUMASHIFT2(picture->lumshift2);
			cedrus_write(dev, VE_DEC_VC1_PICINTENCOMP, reg);
			if (!second_field) {
				vc1_icf0_reg6c = vc1_icb1_regbak & 0xff3f3f3f;
				cedrus_write(dev, VE_ENGINE_DEC_VC1 + 0x6c, vc1_icf0_reg6c);
				cedrus_write(dev, VE_ENGINE_DEC_VC1 + 0x68, 0);
				vc1_icb0_reg68 = reg | VE_DEC_VC1_PICINTENCOMP_FIELD(picture->intcompfield);
			} else {
				cedrus_write(dev, VE_ENGINE_DEC_VC1 + 0x6c, 0);
				cedrus_write(dev, VE_ENGINE_DEC_VC1 + 0x68, vc1_icb0_reg68);
				vc1_icf0_reg6c = vc1_icb1_regbak & 0xff3f3f3f;
				vc1_icb1_regbak = reg | VE_DEC_VC1_PICINTENCOMP_FIELD(picture->intcompfield);
			}
		}
	} else {
		if (picture->ptype == VC1_PICTURE_TYPE_I) {
			reg = 0;
		} else if (picture->ptype == VC1_PICTURE_TYPE_P) {
			reg = VE_DEC_VC1_PICINTENCOMP_LUMASCALE1(picture->lumscale);
			reg |= VE_DEC_VC1_PICINTENCOMP_LUMASHIFT1(picture->lumshift);
			reg |= VE_DEC_VC1_PICINTENCOMP_LUMASCALE2(picture->lumscale2);
			reg |= VE_DEC_VC1_PICINTENCOMP_LUMASHIFT2(picture->lumshift2);
			if (!reg && oldval)
				reg = oldval;
			else
				oldval = reg;
		} else {
			reg = oldval;
		}
		cedrus_write(dev, VE_DEC_VC1_PICINTENCOMP, reg);
	}

	if (picture->ptype == VC1_PICTURE_TYPE_B) {
		frfd = (bfraction * picture->refdist) >> 8;
		brfd = picture->refdist - frfd - 1;
	} else {
		frfd = picture->refdist;
		brfd = 0;
	}

	if (frfd > 3)
		frfd = 3;
	if (brfd < 0)
		brfd = 0;
	else if (brfd > 3)
		brfd = 3;

	reg = 0;
	if (raw_coding & V4L2_VC1_RAW_CODING_FLAG_FIELDTX)
		reg |= VE_DEC_VC1_PICINTERLACE_FIELDTX_RAW;
	//reg |= VE_DEC_VC1_PICINTERLACE_DMVRANGE(vc1_dmvrange_map[picture->dmvrange & 3]);
	reg |= VE_DEC_VC1_PICINTERLACE_DMVRANGE(picture->dmvrange);
	//if (picture->flags & V4L2_VC1_PICTURE_LAYER_FLAG_4MVSWITCH)
	if (mvmode == VC1_MVMODE_1MV)
	//if (picture->mvmode == VC1_MVMODE_1MV)
		reg |= VE_DEC_VC1_PICINTERLACE_4MVSWITCH;
	reg |= VE_DEC_VC1_PICINTERLACE_MBMODETAB(picture->mbmodetab);
	reg |= VE_DEC_VC1_PICINTERLACE_IMVTAB(picture->imvtab);
	reg |= VE_DEC_VC1_PICINTERLACE_ICBPTAB(picture->icbptab);
	if (flag)//(picture->flags & V4L2_VC1_PICTURE_LAYER_FLAG_INTCOMP)
		reg |= VE_DEC_VC1_PICINTERLACE_INTENCOMP;
	reg |= VE_DEC_VC1_PICINTERLACE_2MVBPTAB(picture->twomvbptab);
	reg |= VE_DEC_VC1_PICINTERLACE_4MVBPTAB(picture->fourmvbptab);
	reg |= VE_DEC_VC1_PICINTERLACE_FRFD(frfd);
	reg |= VE_DEC_VC1_PICINTERLACE_BRFD(brfd);
	if (!(second_field ^ ref_field))
		reg |= VE_DEC_VC1_PICINTERLACE_REFFIELD;
	reg |= VE_DEC_VC1_PICINTERLACE_INTENCOMPFLD(picture->intcompfield);
	if (raw_coding & V4L2_VC1_RAW_CODING_FLAG_FORWARDMB)
		reg |= VE_DEC_VC1_PICINTERLACE_FORWARD_RAW;
	if (fwd_buf && bwd_buf) {
		if (fwd_buf->codec.vc1.interlaced)
			reg |= VE_DEC_VC1_PICINTERLACE_FWD_INTERLACE;
		if (bwd_buf->codec.vc1.interlaced)
			reg |= VE_DEC_VC1_PICINTERLACE_BWD_INTERLACE;
	} else if (interlaced) {
		reg |= VE_DEC_VC1_PICINTERLACE_FWD_INTERLACE |
		       VE_DEC_VC1_PICINTERLACE_BWD_INTERLACE;
	}
	if (picture->flags & V4L2_VC1_PICTURE_LAYER_FLAG_NUMREF)
		reg |= VE_DEC_VC1_PICINTERLACE_NUMREF;
	cedrus_write(dev, VE_DEC_VC1_PICINTERLACE, reg);

	/* Set frame dimensions. */

	reg = VE_DEC_VC1_FSIZE_WIDTH(ctx->src_fmt.width);
	reg |= VE_DEC_VC1_FSIZE_HEIGHT(ctx->src_fmt.height);
	cedrus_write(dev, VE_DEC_VC1_FSIZE, reg);

	reg = VE_DEC_VC1_PICSIZE_WIDTH(ctx->src_fmt.width);
	reg |= VE_DEC_VC1_PICSIZE_HEIGHT(ctx->src_fmt.height);
	cedrus_write(dev, VE_DEC_VC1_PICSIZE, reg);

	cedrus_write(dev, VE_ENGINE_DEC_VC1 + 0x70, 0);

	/* Destination luma and chroma buffers. */

	dst_luma_addr = cedrus_dst_buf_addr(ctx, &run->dst->vb2_buf, 0);
	dst_chroma_addr = cedrus_dst_buf_addr(ctx, &run->dst->vb2_buf, 1);

	cedrus_write(dev, VE_DEC_VC1_REC_LUMA, dst_luma_addr);
	cedrus_write(dev, VE_DEC_VC1_REC_CHROMA, dst_chroma_addr);

	/* Forward and backward prediction reference buffers. */

	if (forward_vb2) {
		cedrus_write(dev, VE_DEC_VC1_FWD_REF_LUMA_ADDR,
			cedrus_dst_buf_addr(ctx, forward_vb2, 0));
		cedrus_write(dev, VE_DEC_VC1_FWD_REF_CHROMA_ADDR,
			cedrus_dst_buf_addr(ctx, forward_vb2, 1));
	} else {
		cedrus_write(dev, VE_DEC_VC1_FWD_REF_LUMA_ADDR, dst_luma_addr);
		cedrus_write(dev, VE_DEC_VC1_FWD_REF_CHROMA_ADDR, dst_chroma_addr);
	}

	if (backward_vb2) {
		cedrus_write(dev, VE_DEC_VC1_BWD_REF_LUMA_ADDR,
			cedrus_dst_buf_addr(ctx, backward_vb2, 0));
		cedrus_write(dev, VE_DEC_VC1_BWD_REF_CHROMA_ADDR,
			cedrus_dst_buf_addr(ctx, backward_vb2, 1));
	} else {
		cedrus_write(dev, VE_DEC_VC1_BWD_REF_LUMA_ADDR, dst_luma_addr);
		cedrus_write(dev, VE_DEC_VC1_BWD_REF_CHROMA_ADDR, dst_chroma_addr);
	}

	/* Setup bitplanes */

	if (bitplanes && bitplanes->bitplane_flags)
		cedrus_vc1_bitplanes_setup(ctx, run);

	reg = VE_DEC_VC1_CTRL_FINISH_IRQ_EN |
	      VE_DEC_VC1_CTRL_ERROR_IRQ_EN |
	      VE_DEC_VC1_CTRL_VLD_DATA_REQ_IRQ_EN;
	if (picture->ptype == VC1_PICTURE_TYPE_SKIPPED) {
		printk("frame is skipped!\n");
		reg |= VE_DEC_VC1_CTRL_NO_RECONSTRUCT_PIC;
	}
	cedrus_write(dev, VE_DEC_VC1_CTRL, reg);

	return 0;
}

static struct file *f;
static loff_t offset;

static int cedrus_vc1_start(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;
	int ret;
	
	oldval = 0;
	intenen = false;
	vc1_icb1_regbak = vc1_icb0_reg68 = vc1_icf0_reg6c = 0;
	seq = 0;
	
	f = filp_open("/root/out.bin", O_CREAT | O_WRONLY | O_TRUNC, 0666);
	offset = 0;

	ctx->codec.vc1.mv_buf =
		dma_alloc_coherent(dev->dev, MV_BUF_SIZE,
				   &ctx->codec.vc1.mv_buf_addr,
				   GFP_KERNEL);
	if (!ctx->codec.vc1.mv_buf)
		return -ENOMEM;

	ctx->codec.vc1.acdc_buf =
		dma_alloc_coherent(dev->dev, ACDC_BUF_SIZE,
				   &ctx->codec.vc1.acdc_buf_addr,
				   GFP_KERNEL);
	if (!ctx->codec.vc1.acdc_buf) {
		ret = -ENOMEM;
		goto err_mv_buf;
	}

	ctx->codec.vc1.bitplanes_buf =
		dma_alloc_coherent(dev->dev, BITPLANES_BUF_SIZE,
				   &ctx->codec.vc1.bitplanes_buf_addr,
				   GFP_KERNEL);
	if (!ctx->codec.vc1.bitplanes_buf) {
		ret = -ENOMEM;
		goto err_acdc_buf;
	}

	return 0;

err_acdc_buf:
	dma_free_coherent(dev->dev, ACDC_BUF_SIZE,
			  ctx->codec.vc1.acdc_buf,
			  ctx->codec.vc1.acdc_buf_addr);

err_mv_buf:
	dma_free_coherent(dev->dev, MV_BUF_SIZE,
			  ctx->codec.vc1.mv_buf,
			  ctx->codec.vc1.mv_buf_addr);

	return ret;
}

#include <linux/kernel.h>

static void write_section(const char *info, volatile char *regs, unsigned int o, unsigned int size)
{
	char data[128];
	int i;

	for (i = 0; i <= size; i += 4) {
		snprintf(data, sizeof(data), "%.3x: %.8x\n", i + o, readl(regs + i + o));
		kernel_write(f, data, strlen(data), &offset);
	}
	kernel_write(f, info, strlen(info), &offset);
}

static void cedrus_vc1_stop(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;
	
	filp_close(f, NULL);

	dma_free_coherent(dev->dev, MV_BUF_SIZE,
			  ctx->codec.vc1.mv_buf,
			  ctx->codec.vc1.mv_buf_addr);
	dma_free_coherent(dev->dev, ACDC_BUF_SIZE,
			  ctx->codec.vc1.acdc_buf,
			  ctx->codec.vc1.acdc_buf_addr);
	dma_free_coherent(dev->dev, BITPLANES_BUF_SIZE,
			  ctx->codec.vc1.bitplanes_buf,
			  ctx->codec.vc1.bitplanes_buf_addr);
}

static void cedrus_vc1_trigger(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;
	/*int i;

	for (i = 0; i <= 0xec; i+=4)
		printk("%.3x: %.8x\n", i, cedrus_read(dev, i));
	printk("main section\n");
	for (i = 0x300; i <= 0x3cc; i+=4)
		printk("%.3x: %.8x\n", i, cedrus_read(dev, i));
	printk("--eof--\n");*/
	
	//write_section("main section\n", dev->base, 0, 0xec);
	//write_section("--eof--\n", dev->base, 0x300, 0xcc);
	if (seq == 59)
		kernel_write(f, ctx->codec.vc1.bitplanes_buf, BITPLANES_BUF_SIZE, &offset);
	
	seq++;

	cedrus_write(dev, VE_DEC_VC1_TRIGGER_TYPE,
		     VE_DEC_VC1_TRIGGER_TYPE_DECODE);
}

struct cedrus_dec_ops cedrus_dec_ops_vc1 = {
	.irq_clear	= cedrus_vc1_irq_clear,
	.irq_disable	= cedrus_vc1_irq_disable,
	.irq_status	= cedrus_vc1_irq_status,
	.setup		= cedrus_vc1_setup,
	.start		= cedrus_vc1_start,
	.stop		= cedrus_vc1_stop,
	.trigger	= cedrus_vc1_trigger,
};
