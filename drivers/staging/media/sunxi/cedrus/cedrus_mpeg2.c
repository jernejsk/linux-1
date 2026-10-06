// SPDX-License-Identifier: GPL-2.0
/*
 * Cedrus VPU driver
 *
 * Copyright (C) 2016 Florent Revest <florent.revest@free-electrons.com>
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 * Copyright (C) 2018 Bootlin
 */

#include <media/videobuf2-dma-contig.h>

#include "cedrus.h"
#include "cedrus_hw.h"
#include "cedrus_regs.h"

static enum cedrus_irq_status cedrus_mpeg2_irq_status(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;
	u32 reg;

	reg = cedrus_read(dev, VE_DEC_MPEG_STATUS);
	reg &= VE_DEC_MPEG_STATUS_CHECK_MASK;

	if (!reg)
		return CEDRUS_IRQ_NONE;

	if (reg & VE_DEC_MPEG_STATUS_CHECK_ERROR ||
	    !(reg & VE_DEC_MPEG_STATUS_SUCCESS))
		return CEDRUS_IRQ_ERROR;

	return CEDRUS_IRQ_OK;
}

static void cedrus_mpeg2_irq_clear(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;

	cedrus_write(dev, VE_DEC_MPEG_STATUS, VE_DEC_MPEG_STATUS_CHECK_MASK);
}

static void cedrus_mpeg2_irq_disable(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;
	u32 reg = cedrus_read(dev, VE_DEC_MPEG_CTRL);

	reg &= ~VE_DEC_MPEG_CTRL_IRQ_MASK;

	cedrus_write(dev, VE_DEC_MPEG_CTRL, reg);
}

static int cedrus_mpeg2_setup(struct cedrus_ctx *ctx, struct cedrus_run *run)
{
	struct vb2_buffer *fwd, *bwd;
	bool field, tff;
	const struct v4l2_ctrl_mpeg2_sequence *seq;
	const struct v4l2_ctrl_mpeg2_picture *pic;
	const struct v4l2_ctrl_mpeg2_quantisation *quantisation;
	dma_addr_t src_buf_addr, dst_luma_addr, dst_chroma_addr;
	struct cedrus_dev *dev = ctx->dev;
	struct vb2_queue *vq;
	const u8 *matrix;
	unsigned int i;
	u32 reg;

	seq = run->mpeg2.sequence;
	pic = run->mpeg2.picture;

	quantisation = run->mpeg2.quantisation;

	/* Activate MPEG engine. */
	cedrus_engine_enable(ctx);

	/* Set intra quantisation matrix. */
	matrix = quantisation->intra_quantiser_matrix;
	for (i = 0; i < 64; i++) {
		reg = VE_DEC_MPEG_IQMINPUT_WEIGHT(i, matrix[i]);
		reg |= VE_DEC_MPEG_IQMINPUT_FLAG_INTRA;

		cedrus_write(dev, VE_DEC_MPEG_IQMINPUT, reg);
	}

	/* Set non-intra quantisation matrix. */
	matrix = quantisation->non_intra_quantiser_matrix;
	for (i = 0; i < 64; i++) {
		reg = VE_DEC_MPEG_IQMINPUT_WEIGHT(i, matrix[i]);
		reg |= VE_DEC_MPEG_IQMINPUT_FLAG_NON_INTRA;

		cedrus_write(dev, VE_DEC_MPEG_IQMINPUT, reg);
	}

	/*
	 * top_field_first is always 0 in field pictures, but the hardware
	 * uses this bit to tell the first field of a frame from the second.
	 * A field is the second one when it has the opposite parity of the
	 * previous field and is decoded into the same capture buffer.
	 */
	if (pic->picture_structure == V4L2_MPEG2_PIC_FRAME) {
		tff = !!(pic->flags & V4L2_MPEG2_PIC_FLAG_TOP_FIELD_FIRST);
		ctx->codec.mpeg2.pair_pending = false;
	} else {
		bool second = ctx->codec.mpeg2.pair_pending &&
			      ctx->codec.mpeg2.pair_structure != pic->picture_structure &&
			      ctx->codec.mpeg2.pair_dst == run->dst->vb2_buf.index;

		tff = (pic->picture_structure == V4L2_MPEG2_PIC_TOP_FIELD) != second;
		ctx->codec.mpeg2.pair_pending = !second;
		ctx->codec.mpeg2.pair_structure = pic->picture_structure;
		ctx->codec.mpeg2.pair_dst = run->dst->vb2_buf.index;
	}

	/* Set MPEG picture header. */

	reg = VE_DEC_MPEG_MP12HDR_SLICE_TYPE(pic->picture_coding_type);
	reg |= VE_DEC_MPEG_MP12HDR_F_CODE(0, 0, pic->f_code[0][0]);
	reg |= VE_DEC_MPEG_MP12HDR_F_CODE(0, 1, pic->f_code[0][1]);
	reg |= VE_DEC_MPEG_MP12HDR_F_CODE(1, 0, pic->f_code[1][0]);
	reg |= VE_DEC_MPEG_MP12HDR_F_CODE(1, 1, pic->f_code[1][1]);
	reg |= VE_DEC_MPEG_MP12HDR_INTRA_DC_PRECISION(pic->intra_dc_precision);
	reg |= VE_DEC_MPEG_MP12HDR_INTRA_PICTURE_STRUCTURE(pic->picture_structure);
	reg |= VE_DEC_MPEG_MP12HDR_TOP_FIELD_FIRST(tff);
	reg |= VE_DEC_MPEG_MP12HDR_FRAME_PRED_FRAME_DCT(pic->flags & V4L2_MPEG2_PIC_FLAG_FRAME_PRED_DCT);
	reg |= VE_DEC_MPEG_MP12HDR_CONCEALMENT_MOTION_VECTORS(pic->flags & V4L2_MPEG2_PIC_FLAG_CONCEALMENT_MV);
	reg |= VE_DEC_MPEG_MP12HDR_Q_SCALE_TYPE(pic->flags & V4L2_MPEG2_PIC_FLAG_Q_SCALE_TYPE);
	reg |= VE_DEC_MPEG_MP12HDR_INTRA_VLC_FORMAT(pic->flags & V4L2_MPEG2_PIC_FLAG_INTRA_VLC);
	reg |= VE_DEC_MPEG_MP12HDR_ALTERNATE_SCAN(pic->flags & V4L2_MPEG2_PIC_FLAG_ALT_SCAN);
	reg |= VE_DEC_MPEG_MP12HDR_FULL_PEL_FORWARD_VECTOR(0);
	reg |= VE_DEC_MPEG_MP12HDR_FULL_PEL_BACKWARD_VECTOR(0);

	cedrus_write(dev, VE_DEC_MPEG_MP12HDR, reg);

	/* Set frame dimensions. */

	/*
	 * Field pictures cover half of the frame's macroblock rows; the
	 * vendor library halves both the coded and the boundary height.
	 */
	field = pic->picture_structure != V4L2_MPEG2_PIC_FRAME;

	reg = VE_DEC_MPEG_PICCODEDSIZE_WIDTH(seq->horizontal_size);
	reg |= VE_DEC_MPEG_PICCODEDSIZE_HEIGHT(field ?
			ALIGN(seq->vertical_size, 32) / 2 : seq->vertical_size);

	cedrus_write(dev, VE_DEC_MPEG_PICCODEDSIZE, reg);

	reg = VE_DEC_MPEG_PICBOUNDSIZE_WIDTH(ctx->src_fmt.width);
	reg |= VE_DEC_MPEG_PICBOUNDSIZE_HEIGHT(field ? ctx->src_fmt.height / 2 :
						   ctx->src_fmt.height);

	cedrus_write(dev, VE_DEC_MPEG_PICBOUNDSIZE, reg);

	/*
	 * Forward and backward prediction reference buffers. As the vendor
	 * library does, I and P pictures use the current frame as backward
	 * reference: the second field of a frame predicts from the first one.
	 * Missing references also fall back to the current frame.
	 */
	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE);

	fwd = vb2_find_buffer(vq, pic->forward_ref_ts) ?: &run->dst->vb2_buf;
	if (pic->picture_coding_type == V4L2_MPEG2_PIC_CODING_TYPE_B)
		bwd = vb2_find_buffer(vq, pic->backward_ref_ts) ?:
		      &run->dst->vb2_buf;
	else
		bwd = &run->dst->vb2_buf;

	cedrus_write(dev, VE_DEC_MPEG_FWD_REF_LUMA_ADDR,
		     cedrus_dst_buf_addr(ctx, fwd, 0));
	cedrus_write(dev, VE_DEC_MPEG_FWD_REF_CHROMA_ADDR,
		     cedrus_dst_buf_addr(ctx, fwd, 1));
	cedrus_write(dev, VE_DEC_MPEG_BWD_REF_LUMA_ADDR,
		     cedrus_dst_buf_addr(ctx, bwd, 0));
	cedrus_write(dev, VE_DEC_MPEG_BWD_REF_CHROMA_ADDR,
		     cedrus_dst_buf_addr(ctx, bwd, 1));

	/* Destination luma and chroma buffers. */

	dst_luma_addr = cedrus_dst_buf_addr(ctx, &run->dst->vb2_buf, 0);
	dst_chroma_addr = cedrus_dst_buf_addr(ctx, &run->dst->vb2_buf, 1);

	cedrus_write(dev, VE_DEC_MPEG_REC_LUMA, dst_luma_addr);
	cedrus_write(dev, VE_DEC_MPEG_REC_CHROMA, dst_chroma_addr);

	/* Source offset and length in bits. */

	cedrus_write(dev, VE_DEC_MPEG_VLD_OFFSET, 0);

	reg = vb2_get_plane_payload(&run->src->vb2_buf, 0) * 8;
	cedrus_write(dev, VE_DEC_MPEG_VLD_LEN, reg);

	/* Source beginning and end addresses. */

	src_buf_addr = vb2_dma_contig_plane_dma_addr(&run->src->vb2_buf, 0);

	reg = VE_DEC_MPEG_VLD_ADDR_BASE(src_buf_addr);
	reg |= VE_DEC_MPEG_VLD_ADDR_VALID_PIC_DATA;
	reg |= VE_DEC_MPEG_VLD_ADDR_LAST_PIC_DATA;
	reg |= VE_DEC_MPEG_VLD_ADDR_FIRST_PIC_DATA;

	cedrus_write(dev, VE_DEC_MPEG_VLD_ADDR, reg);

	reg = src_buf_addr + vb2_get_plane_payload(&run->src->vb2_buf, 0);
	cedrus_write(dev, VE_DEC_MPEG_VLD_END_ADDR, reg);

	/* Macroblock address: start at the beginning. */
	reg = VE_DEC_MPEG_MBADDR_Y(0) | VE_DEC_MPEG_MBADDR_X(0);
	cedrus_write(dev, VE_DEC_MPEG_MBADDR, reg);

	/* Clear previous errors. */
	cedrus_write(dev, VE_DEC_MPEG_ERROR, 0);

	/* Clear correct macroblocks register. */
	cedrus_write(dev, VE_DEC_MPEG_CRTMBADDR, 0);

	/* Enable appropriate interruptions and components. */

	reg = VE_DEC_MPEG_CTRL_IRQ_MASK | VE_DEC_MPEG_CTRL_MC_NO_WRITEBACK |
	      VE_DEC_MPEG_CTRL_MC_CACHE_EN;

	cedrus_write(dev, VE_DEC_MPEG_CTRL, reg);

	return 0;
}

static int cedrus_mpeg2_start(struct cedrus_ctx *ctx)
{
	memset(&ctx->codec.mpeg2, 0, sizeof(ctx->codec.mpeg2));

	return 0;
}

static void cedrus_mpeg2_trigger(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;
	u32 reg;

	/* Trigger MPEG engine. */
	reg = VE_DEC_MPEG_TRIGGER_HW_MPEG_VLD | VE_DEC_MPEG_TRIGGER_MPEG2 |
	      VE_DEC_MPEG_TRIGGER_MB_BOUNDARY;

	cedrus_write(dev, VE_DEC_MPEG_TRIGGER, reg);
}

struct cedrus_dec_ops cedrus_dec_ops_mpeg2 = {
	.irq_clear	= cedrus_mpeg2_irq_clear,
	.irq_disable	= cedrus_mpeg2_irq_disable,
	.irq_status	= cedrus_mpeg2_irq_status,
	.setup		= cedrus_mpeg2_setup,
	.start		= cedrus_mpeg2_start,
	.trigger	= cedrus_mpeg2_trigger,
};
