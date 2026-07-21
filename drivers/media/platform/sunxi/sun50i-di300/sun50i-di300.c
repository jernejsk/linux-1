// SPDX-License-Identifier: GPL-2.0
/*
 * Allwinner DI300 deinterlace driver
 *
 * Copyright (C) 2026 Jernej Skrabec <jernej.skrabec@siol.net>
 *
 * Based on vim2m and sun8i-di drivers.
 *
 * The DI300 block found on H616 (and later SoCs) takes up to three
 * temporally adjacent input frames per pass: a future reference frame,
 * the frame being deinterlaced and a past reference frame. It produces
 * both deinterlaced fields of the middle frame in a single hardware
 * run, unlike the older sun8i-di block which needs one run per output
 * field. Only the plain motion-adaptive DIT path is implemented here;
 * temporal noise reduction is not used. Film mode detection (FMD), on
 * SoCs that have it, is enabled with its raw per-field statistics
 * exposed read-only via a control; see the comment above the FMD
 * register block in sun50i-di300.h for what is and isn't implemented.
 */

#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>

#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>

#include "sun50i-di300.h"

static u32 deinterlace_formats[] = {
	V4L2_PIX_FMT_NV12,
	V4L2_PIX_FMT_NV21,
	V4L2_PIX_FMT_YUV420,
	V4L2_PIX_FMT_NV16,
	V4L2_PIX_FMT_NV61,
	V4L2_PIX_FMT_YUV422P,
};

static inline u32 deinterlace_read(struct deinterlace_dev *dev, u32 reg)
{
	return readl(dev->base + reg);
}

static inline void deinterlace_write(struct deinterlace_dev *dev,
				     u32 reg, u32 value)
{
	writel(value, dev->base + reg);
}

static inline void deinterlace_set_bits(struct deinterlace_dev *dev,
					u32 reg, u32 bits)
{
	writel(readl(dev->base + reg) | bits, dev->base + reg);
}

static inline void deinterlace_clr_bits(struct deinterlace_dev *dev,
					u32 reg, u32 bits)
{
	writel(readl(dev->base + reg) & ~bits, dev->base + reg);
}

static u32 deinterlace_hw_format(u32 pixelformat)
{
	switch (pixelformat) {
	case V4L2_PIX_FMT_YUV420:
		return 0;
	default:
	case V4L2_PIX_FMT_NV12:
	case V4L2_PIX_FMT_NV21:
		return 1;
	case V4L2_PIX_FMT_YUV422P:
		return 2;
	case V4L2_PIX_FMT_NV16:
	case V4L2_PIX_FMT_NV61:
		return 3;
	}
}

static bool deinterlace_fmt_is_uv_combined(u32 pixelformat)
{
	switch (pixelformat) {
	case V4L2_PIX_FMT_NV12:
	case V4L2_PIX_FMT_NV21:
	case V4L2_PIX_FMT_NV16:
	case V4L2_PIX_FMT_NV61:
		return true;
	default:
		return false;
	}
}

static bool deinterlace_fmt_is_uv_first(u32 pixelformat)
{
	switch (pixelformat) {
	case V4L2_PIX_FMT_NV12:
	case V4L2_PIX_FMT_NV16:
		return true;
	default:
		return false;
	}
}

/* offsets of the luma, and first and second chroma plane, in bytes */
static void deinterlace_plane_offsets(struct v4l2_pix_format *fmt,
				      unsigned int offset[3], bool present[3])
{
	unsigned int luma_size = fmt->bytesperline * fmt->height;

	offset[0] = 0;
	present[0] = true;
	present[1] = true;
	present[2] = false;

	switch (fmt->pixelformat) {
	case V4L2_PIX_FMT_YUV420:
		offset[1] = luma_size;
		offset[2] = luma_size + luma_size / 4;
		present[2] = true;
		break;
	case V4L2_PIX_FMT_YUV422P:
		offset[1] = luma_size;
		offset[2] = luma_size + luma_size / 2;
		present[2] = true;
		break;
	default:
		/* UV-combined formats: no separate third plane */
		offset[1] = luma_size;
		offset[2] = 0;
		break;
	}
}

static void deinterlace_write_in_addr(struct deinterlace_dev *dev,
				      u32 top_reg, u32 top_hreg,
				      u32 bot_reg, u32 bot_hreg,
				      dma_addr_t addr,
				      struct v4l2_pix_format *fmt)
{
	unsigned int offset[3];
	bool present[3];
	unsigned int i;

	deinterlace_plane_offsets(fmt, offset, present);

	for (i = 0; i < 3; i++)
		deinterlace_write(dev, top_reg + i * 4,
				  present[i] ? addr + offset[i] : 0);
	deinterlace_write(dev, top_hreg, 0);

	for (i = 0; i < 3; i++)
		deinterlace_write(dev, bot_reg + i * 4,
				  present[i] ?
				  addr + offset[i] + fmt->bytesperline : 0);
	deinterlace_write(dev, bot_hreg, 0);
}

static void deinterlace_write_out_addr(struct deinterlace_dev *dev,
				       u32 reg, u32 hreg, dma_addr_t addr,
				       struct v4l2_pix_format *fmt)
{
	unsigned int offset[3];
	bool present[3];
	unsigned int i;

	deinterlace_plane_offsets(fmt, offset, present);

	for (i = 0; i < 3; i++)
		deinterlace_write(dev, reg + i * 4,
				  present[i] ? addr + offset[i] : 0);
	deinterlace_write(dev, hreg, 0);
}

static void deinterlace_device_run(void *priv)
{
	struct deinterlace_ctx *ctx = priv;
	struct deinterlace_dev *dev = ctx->dev;
	struct vb2_v4l2_buffer *src, *dst0, *dst1;
	struct v4l2_m2m_buffer *b;
	dma_addr_t next, curr, prev, out0, out1;
	unsigned int hwfmt = deinterlace_hw_format(ctx->src_fmt.pixelformat);
	unsigned int i, reg;
	bool motion;

	src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);

	ctx->fmd_active = dev->has_fmd && ctx->fmd_enabled;

	/*
	 * Apply this job's field-weave decision, if userspace attached a
	 * request to the OUTPUT buffer carrying one. Complete it right
	 * away: unlike the stats control, nothing here needs to wait for
	 * the buffer's eventual release, since this is an input applied
	 * before the job runs, not output read back after.
	 */
	ctx->req = src->vb2_buf.req_obj.req;
	if (ctx->req) {
		v4l2_ctrl_request_setup(ctx->req, &ctx->hdl);
		v4l2_ctrl_request_complete(ctx->req, &ctx->hdl);
		ctx->req = NULL;
	}

	/*
	 * Pick the two buffers actually at the front of the ready
	 * queue. v4l2_m2m_last_dst_buf() returns the tail of the whole
	 * ready list, which is only the "second" buffer when exactly
	 * two are queued; with more buffers queued (the normal case for
	 * any real client) it names a buffer this job never touches,
	 * while the IRQ handler's two dst_buf_remove() calls always pop
	 * from the front and would complete a stale, never-written one.
	 */
	dst0 = NULL;
	dst1 = NULL;
	v4l2_m2m_for_each_dst_buf(ctx->fh.m2m_ctx, b) {
		if (!dst0) {
			dst0 = &b->vb;
		} else if (!dst1) {
			dst1 = &b->vb;
			break;
		}
	}
	ctx->dst0 = dst0;
	ctx->dst1 = dst1;

	v4l2_m2m_buf_copy_metadata(src, dst0);

	next = vb2_dma_contig_plane_dma_addr(&src->vb2_buf, 0);
	curr = ctx->prev[0] ?
		vb2_dma_contig_plane_dma_addr(&ctx->prev[0]->vb2_buf, 0) : next;
	prev = ctx->prev[1] ?
		vb2_dma_contig_plane_dma_addr(&ctx->prev[1]->vb2_buf, 0) : curr;

	/*
	 * A single real temporal neighbour (prev[0]) is enough to turn
	 * motion mode on: the missing prev[1] slot then just duplicates
	 * curr, which reads as "no motion" between those two and leaves
	 * the genuine next-vs-curr comparison to drive the spatial/
	 * temporal blend. Requiring both slots real only delays real
	 * motion-adaptive output by an extra frame at stream start.
	 */
	motion = ctx->prev[0] != NULL;

	deinterlace_write(dev, DEINTERLACE_SIZE,
			  DEINTERLACE_SIZE_WIDTH(ctx->src_fmt.width) |
			  DEINTERLACE_SIZE_HEIGHT(ctx->src_fmt.height));

	reg = DEINTERLACE_FMT_IN0(hwfmt) | DEINTERLACE_FMT_IN1(hwfmt) |
	      DEINTERLACE_FMT_IN2(hwfmt) | DEINTERLACE_FMT_DIT(hwfmt);
	if (deinterlace_fmt_is_uv_combined(ctx->src_fmt.pixelformat) &&
	    deinterlace_fmt_is_uv_first(ctx->src_fmt.pixelformat))
		reg |= DEINTERLACE_FMT_UVSEQ;
	deinterlace_write(dev, DEINTERLACE_FMT, reg);

	if (ctx->first_field)
		deinterlace_write(dev, DEINTERLACE_FIELD_ORDER,
				  DEINTERLACE_FIELD_ORDER_BFF);
	else
		deinterlace_write(dev, DEINTERLACE_FIELD_ORDER, 0);

	for (i = 0; i < 3; i++) {
		unsigned int in_pitch = ctx->src_fmt.bytesperline * 2;
		unsigned int out_pitch = ctx->dst_fmt.bytesperline;

		deinterlace_write(dev, DEINTERLACE_IN_F01_PITCH(i),
				  DEINTERLACE_IN_F01_PITCH_F0(in_pitch) |
				  DEINTERLACE_IN_F01_PITCH_F1(in_pitch));
		deinterlace_write(dev, DEINTERLACE_IN_F2_PITCH(i), in_pitch);
		deinterlace_write(dev, DEINTERLACE_OUT_DIT_PITCH(i), out_pitch);
	}

	deinterlace_write_in_addr(dev, DEINTERLACE_IN_F0_TOP_ADDR(0),
				  DEINTERLACE_IN_F0_TOP_HADDR,
				  DEINTERLACE_IN_F0_BOT_ADDR(0),
				  DEINTERLACE_IN_F0_BOT_HADDR,
				  next, &ctx->src_fmt);
	deinterlace_write_in_addr(dev, DEINTERLACE_IN_F1_TOP_ADDR(0),
				  DEINTERLACE_IN_F1_TOP_HADDR,
				  DEINTERLACE_IN_F1_BOT_ADDR(0),
				  DEINTERLACE_IN_F1_BOT_HADDR,
				  curr, &ctx->src_fmt);
	deinterlace_write_in_addr(dev, DEINTERLACE_IN_F2_TOP_ADDR(0),
				  DEINTERLACE_IN_F2_TOP_HADDR,
				  DEINTERLACE_IN_F2_BOT_ADDR(0),
				  DEINTERLACE_IN_F2_BOT_HADDR,
				  prev, &ctx->src_fmt);

	out0 = vb2_dma_contig_plane_dma_addr(&dst0->vb2_buf, 0);
	out1 = vb2_dma_contig_plane_dma_addr(&dst1->vb2_buf, 0);
	deinterlace_write_out_addr(dev, DEINTERLACE_OUT_DIT0_ADDR(0),
				   DEINTERLACE_OUT_DIT0_HADDR, out0,
				   &ctx->dst_fmt);
	deinterlace_write_out_addr(dev, DEINTERLACE_OUT_DIT1_ADDR(0),
				   DEINTERLACE_OUT_DIT1_HADDR, out1,
				   &ctx->dst_fmt);

	deinterlace_write(dev, DEINTERLACE_FLAG_PITCH, 0x200);
	deinterlace_write(dev, DEINTERLACE_IN_FLAG_ADDR, ctx->flag1_buf_dma);
	deinterlace_write(dev, DEINTERLACE_OUT_FLAG_ADDR, ctx->flag2_buf_dma);
	deinterlace_write(dev, DEINTERLACE_FLAG_HADDR, 0);
	swap(ctx->flag1_buf_dma, ctx->flag2_buf_dma);

	/*
	 * Restrict the deinterlace and motion-detect blocks to the full
	 * frame. The crop registers power up as an empty (zero) window;
	 * left at zero the engine asserts busy but processes nothing and
	 * never signals completion.
	 */
	reg = (ctx->src_fmt.width - 1) << 16;
	deinterlace_write(dev, DEINTERLACE_DIT_CROP_H, reg);
	deinterlace_write(dev, DEINTERLACE_MD_CROP_H, reg);
	reg = (ctx->src_fmt.height - 1) << 16;
	deinterlace_write(dev, DEINTERLACE_DIT_CROP_V, reg);
	deinterlace_write(dev, DEINTERLACE_MD_CROP_V, reg);

	if (ctx->fmd_active) {
		reg = (ctx->src_fmt.width - 1) << 16;
		deinterlace_write(dev, DEINTERLACE_FMD_CROP_H, reg);
		reg = (ctx->src_fmt.height - 1) << 16;
		deinterlace_write(dev, DEINTERLACE_FMD_CROP_V, reg);

		/*
		 * Row thresholds scale with frame area relative to the
		 * vendor's 720x480 reference, verbatim from
		 * di_dev_apply_fixed_para(). Fixed to the 8x8 block size
		 * selected by leaving FMD_GLB at its DEINTERLACE_FMD_GLB
		 * default of 0.
		 */
		reg = 3 * ctx->src_fmt.width * ctx->src_fmt.height / (720 * 480);
		reg |= (2 * ctx->src_fmt.width * ctx->src_fmt.height /
			(720 * 480)) << 8;
		reg |= DEINTERLACE_FMD_ROW_TH_EXIT_VIDEO(60);
		deinterlace_write(dev, DEINTERLACE_FMD_ROW_TH, reg);
	}

	reg = DEINTERLACE_FUNC_EN_DIT;
	if (motion)
		reg |= DEINTERLACE_FUNC_EN_MD;
	if (ctx->fmd_active)
		reg |= DEINTERLACE_FUNC_EN_FMD;
	deinterlace_write(dev, DEINTERLACE_FUNC_EN, reg);

	reg = DEINTERLACE_DIT_SETTING_DIAG_INTP_EN;
	if (motion)
		reg |= DEINTERLACE_DIT_SETTING_MODE_LUMA |
		       DEINTERLACE_DIT_SETTING_MOTION_BLEND_LUMA |
		       DEINTERLACE_DIT_SETTING_MODE_CHROMA |
		       DEINTERLACE_DIT_SETTING_MOTION_BLEND_CHROMA;
	deinterlace_write(dev, DEINTERLACE_DIT_SETTING, reg);

	reg = DEINTERLACE_DIT_INTER_PARA_DEFAULT;
	if (ctx->fmd_active && ctx->weave_ctrl) {
		unsigned int phase0, phase1;

		v4l2_ctrl_lock(ctx->weave_ctrl);
		phase0 = ctx->weave_ctrl->p_cur.p_u32[0];
		phase1 = ctx->weave_ctrl->p_cur.p_u32[1];
		v4l2_ctrl_unlock(ctx->weave_ctrl);

		if (phase0 < 3)
			reg |= DEINTERLACE_DIT_INTER_PARA_FIELD_WEAVE_F1 |
			       DEINTERLACE_DIT_INTER_PARA_FIELD_WEAVE_PHASE_F1(phase0);
		if (phase1 < 3)
			reg |= DEINTERLACE_DIT_INTER_PARA_FIELD_WEAVE_F2 |
			       DEINTERLACE_DIT_INTER_PARA_FIELD_WEAVE_PHASE_F2(phase1);
	}
	deinterlace_write(dev, DEINTERLACE_DIT_INTER_PARA, reg);

	reg = DEINTERLACE_DMA_CTL_C | DEINTERLACE_DMA_CTL_P |
	      DEINTERLACE_DMA_CTL_DI | DEINTERLACE_DMA_CTL_W0 |
	      DEINTERLACE_DMA_CTL_W1 | DEINTERLACE_DMA_CTL_MCLK_GATE;
	if (motion)
		reg |= DEINTERLACE_DMA_CTL_FR | DEINTERLACE_DMA_CTL_FW;
	deinterlace_write(dev, DEINTERLACE_DMA_CTL, reg);

	deinterlace_set_bits(dev, DEINTERLACE_INT_CTL,
			     DEINTERLACE_INT_CTL_FINISH_EN);
	deinterlace_set_bits(dev, DEINTERLACE_START, DEINTERLACE_START_EN);
}

static int deinterlace_job_ready(void *priv)
{
	struct deinterlace_ctx *ctx = priv;

	return v4l2_m2m_num_src_bufs_ready(ctx->fh.m2m_ctx) >= 1 &&
	       v4l2_m2m_num_dst_bufs_ready(ctx->fh.m2m_ctx) >= 2;
}

static void deinterlace_job_abort(void *priv)
{
	struct deinterlace_ctx *ctx = priv;

	ctx->aborting = 1;
}

static irqreturn_t deinterlace_irq(int irq, void *data)
{
	struct deinterlace_dev *dev = data;
	struct vb2_v4l2_buffer *src, *dst0, *dst1;
	struct deinterlace_ctx *ctx;
	enum vb2_buffer_state state;
	unsigned int val;

	ctx = v4l2_m2m_get_curr_priv(dev->m2m_dev);
	if (!ctx) {
		v4l2_err(&dev->v4l2_dev,
			 "Instance released before the end of transaction\n");
		return IRQ_NONE;
	}

	val = deinterlace_read(dev, DEINTERLACE_STATUS);
	if (!(val & DEINTERLACE_STATUS_FINISH))
		return IRQ_NONE;

	deinterlace_write(dev, DEINTERLACE_INT_CTL, 0);
	deinterlace_set_bits(dev, DEINTERLACE_STATUS, DEINTERLACE_STATUS_FINISH);
	deinterlace_clr_bits(dev, DEINTERLACE_START, DEINTERLACE_START_EN);

	state = ctx->aborting ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE;

	if (ctx->fmd_active) {
		dev->fmd_stats[0] = deinterlace_read(dev, DEINTERLACE_FMD_FID12) &
				    DEINTERLACE_FMD_HIST_CNT_MASK;
		dev->fmd_stats[1] = deinterlace_read(dev, DEINTERLACE_FMD_FID23) &
				    DEINTERLACE_FMD_HIST_CNT_MASK;
		dev->fmd_stats[2] = deinterlace_read(dev, DEINTERLACE_FMD_FOD_FID30) &
				    DEINTERLACE_FMD_HIST_CNT_MASK;
		dev->fmd_stats[3] = deinterlace_read(dev, DEINTERLACE_FMD_FOD_FID32) &
				    DEINTERLACE_FMD_HIST_CNT_MASK;
		dev->fmd_stats[4] = deinterlace_read(dev, DEINTERLACE_FMD_FOD_FID10) &
				    DEINTERLACE_FMD_HIST_CNT_MASK;
		dev->fmd_stats[5] = deinterlace_read(dev, DEINTERLACE_FMD_FOD_FID12) &
				    DEINTERLACE_FMD_HIST_CNT_MASK;
		dev->fmd_stats[6] = deinterlace_read(dev, DEINTERLACE_FMD_FRD02) &
				    DEINTERLACE_FMD_HIST_CNT_MASK;
		dev->fmd_stats[7] = deinterlace_read(dev, DEINTERLACE_FMD_FRD13) &
				    DEINTERLACE_FMD_HIST_CNT_MASK;
		dev->fmd_stats[8] = deinterlace_read(dev, DEINTERLACE_FMD_FIELD_HIST0);
		dev->fmd_stats[9] = deinterlace_read(dev, DEINTERLACE_FMD_FIELD_HIST1);
	}

	dst0 = ctx->dst0;
	dst1 = ctx->dst1;
	v4l2_m2m_dst_buf_remove_by_buf(ctx->fh.m2m_ctx, dst0);
	v4l2_m2m_buf_done(dst0, state);
	v4l2_m2m_dst_buf_remove_by_buf(ctx->fh.m2m_ctx, dst1);
	v4l2_m2m_buf_done(dst1, state);

	src = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
	if (ctx->prev[1])
		v4l2_m2m_buf_done(ctx->prev[1], state);
	ctx->prev[1] = ctx->prev[0];
	ctx->prev[0] = src;

	v4l2_m2m_job_finish(ctx->dev->m2m_dev, ctx->fh.m2m_ctx);

	return IRQ_HANDLED;
}

static void deinterlace_init(struct deinterlace_dev *dev)
{
	u32 vsn = deinterlace_read(dev, DEINTERLACE_FUNC_VSN);

	dev->has_fmd = DEINTERLACE_FUNC_VSN_FMD_EXIST(vsn) != 0;

	deinterlace_write(dev, DEINTERLACE_MD_PARA, DEINTERLACE_MD_PARA_DEFAULT);

	deinterlace_write(dev, DEINTERLACE_DIT_CHR_PARA0,
			  DEINTERLACE_DIT_CHR_PARA0_DEFAULT);
	deinterlace_write(dev, DEINTERLACE_DIT_CHR_PARA1,
			  DEINTERLACE_DIT_CHR_PARA1_DEFAULT);
	deinterlace_write(dev, DEINTERLACE_DIT_INTRA_PARA,
			  DEINTERLACE_DIT_INTRA_PARA_DEFAULT);
	/* DEINTERLACE_DIT_INTER_PARA is written every job in device_run() */
	deinterlace_write(dev, DEINTERLACE_DIT_DEMO_H, 0);
	deinterlace_write(dev, DEINTERLACE_DIT_DEMO_V, 0);

	if (dev->has_fmd) {
		deinterlace_write(dev, DEINTERLACE_FMD_DIFF_TH0,
				  DEINTERLACE_FMD_DIFF_TH0_DEFAULT);
		deinterlace_write(dev, DEINTERLACE_FMD_DIFF_TH1,
				  DEINTERLACE_FMD_DIFF_TH1_DEFAULT);
		deinterlace_write(dev, DEINTERLACE_FMD_DIFF_TH2,
				  DEINTERLACE_FMD_DIFF_TH2_DEFAULT);
		deinterlace_write(dev, DEINTERLACE_FMD_FEAT_TH0,
				  DEINTERLACE_FMD_FEAT_TH0_DEFAULT);
		deinterlace_write(dev, DEINTERLACE_FMD_FEAT_TH1,
				  DEINTERLACE_FMD_FEAT_TH1_DEFAULT);
		deinterlace_write(dev, DEINTERLACE_FMD_FEAT_TH2,
				  DEINTERLACE_FMD_FEAT_TH2_DEFAULT);
		deinterlace_write(dev, DEINTERLACE_FMD_MOT_TH,
				  DEINTERLACE_FMD_MOT_TH_DEFAULT);
		deinterlace_write(dev, DEINTERLACE_FMD_TEXT_TH,
				  DEINTERLACE_FMD_TEXT_TH_DEFAULT);
		deinterlace_write(dev, DEINTERLACE_FMD_BLK_TH,
				  DEINTERLACE_FMD_BLK_TH_DEFAULT);
		deinterlace_write(dev, DEINTERLACE_FMD_GLB, 0);
	}
}

static inline struct deinterlace_ctx *deinterlace_file2ctx(struct file *file)
{
	return container_of(file_to_v4l2_fh(file), struct deinterlace_ctx, fh);
}

static bool deinterlace_check_format(u32 pixelformat)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(deinterlace_formats); i++)
		if (deinterlace_formats[i] == pixelformat)
			return true;

	return false;
}

static void deinterlace_prepare_format(struct v4l2_pix_format *pix_fmt)
{
	unsigned int height = pix_fmt->height;
	unsigned int width = pix_fmt->width;
	unsigned int bytesperline;
	unsigned int sizeimage;

	width = clamp(width, DEINTERLACE_MIN_WIDTH, DEINTERLACE_MAX_WIDTH);
	height = clamp(height, DEINTERLACE_MIN_HEIGHT, DEINTERLACE_MAX_HEIGHT);

	bytesperline = ALIGN(width, 2);
	sizeimage = bytesperline * height;

	switch (pix_fmt->pixelformat) {
	case V4L2_PIX_FMT_NV12:
	case V4L2_PIX_FMT_NV21:
	case V4L2_PIX_FMT_YUV420:
		sizeimage += bytesperline * height / 2;
		break;
	case V4L2_PIX_FMT_NV16:
	case V4L2_PIX_FMT_NV61:
	case V4L2_PIX_FMT_YUV422P:
		sizeimage += bytesperline * height;
		break;
	}

	pix_fmt->width = width;
	pix_fmt->height = height;
	pix_fmt->bytesperline = bytesperline;
	pix_fmt->sizeimage = sizeimage;
}

static int deinterlace_querycap(struct file *file, void *priv,
				struct v4l2_capability *cap)
{
	strscpy(cap->driver, DEINTERLACE_NAME, sizeof(cap->driver));
	strscpy(cap->card, DEINTERLACE_NAME, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info),
		 "platform:%s", DEINTERLACE_NAME);

	return 0;
}

static int deinterlace_enum_fmt(struct file *file, void *priv,
				struct v4l2_fmtdesc *f)
{
	if (f->index < ARRAY_SIZE(deinterlace_formats)) {
		f->pixelformat = deinterlace_formats[f->index];

		return 0;
	}

	return -EINVAL;
}

static int deinterlace_enum_framesizes(struct file *file, void *priv,
				       struct v4l2_frmsizeenum *fsize)
{
	if (fsize->index != 0)
		return -EINVAL;

	if (!deinterlace_check_format(fsize->pixel_format))
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fsize->stepwise.min_width = DEINTERLACE_MIN_WIDTH;
	fsize->stepwise.min_height = DEINTERLACE_MIN_HEIGHT;
	fsize->stepwise.max_width = DEINTERLACE_MAX_WIDTH;
	fsize->stepwise.max_height = DEINTERLACE_MAX_HEIGHT;
	fsize->stepwise.step_width = 2;
	fsize->stepwise.step_height = 2;

	return 0;
}

static int deinterlace_g_fmt_vid_cap(struct file *file, void *priv,
				     struct v4l2_format *f)
{
	struct deinterlace_ctx *ctx = deinterlace_file2ctx(file);

	f->fmt.pix = ctx->dst_fmt;

	return 0;
}

static int deinterlace_g_fmt_vid_out(struct file *file, void *priv,
				     struct v4l2_format *f)
{
	struct deinterlace_ctx *ctx = deinterlace_file2ctx(file);

	f->fmt.pix = ctx->src_fmt;

	return 0;
}

static int deinterlace_try_fmt_vid_cap(struct file *file, void *priv,
				       struct v4l2_format *f)
{
	struct deinterlace_ctx *ctx = deinterlace_file2ctx(file);

	if (!deinterlace_check_format(ctx->src_fmt.pixelformat))
		return -EINVAL;

	f->fmt.pix.pixelformat = ctx->src_fmt.pixelformat;
	f->fmt.pix.field = V4L2_FIELD_NONE;
	f->fmt.pix.width = ctx->src_fmt.width;
	f->fmt.pix.height = ctx->src_fmt.height;

	deinterlace_prepare_format(&f->fmt.pix);

	return 0;
}

static int deinterlace_try_fmt_vid_out(struct file *file, void *priv,
				       struct v4l2_format *f)
{
	if (!deinterlace_check_format(f->fmt.pix.pixelformat))
		f->fmt.pix.pixelformat = deinterlace_formats[0];

	if (f->fmt.pix.field != V4L2_FIELD_INTERLACED_TB &&
	    f->fmt.pix.field != V4L2_FIELD_INTERLACED_BT &&
	    f->fmt.pix.field != V4L2_FIELD_INTERLACED)
		f->fmt.pix.field = V4L2_FIELD_INTERLACED;

	deinterlace_prepare_format(&f->fmt.pix);

	return 0;
}

static int deinterlace_s_fmt_vid_cap(struct file *file, void *priv,
				     struct v4l2_format *f)
{
	struct deinterlace_ctx *ctx = deinterlace_file2ctx(file);
	struct vb2_queue *vq;
	int ret;

	ret = deinterlace_try_fmt_vid_cap(file, priv, f);
	if (ret)
		return ret;

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (vb2_is_busy(vq))
		return -EBUSY;

	ctx->dst_fmt = f->fmt.pix;

	return 0;
}

static int deinterlace_s_fmt_vid_out(struct file *file, void *priv,
				     struct v4l2_format *f)
{
	struct deinterlace_ctx *ctx = deinterlace_file2ctx(file);
	struct vb2_queue *vq;
	int ret;

	ret = deinterlace_try_fmt_vid_out(file, priv, f);
	if (ret)
		return ret;

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (vb2_is_busy(vq))
		return -EBUSY;

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE);
	if (vb2_is_busy(vq))
		return -EBUSY;

	ctx->src_fmt = f->fmt.pix;

	ctx->dst_fmt.pixelformat = f->fmt.pix.pixelformat;
	ctx->dst_fmt.width = f->fmt.pix.width;
	ctx->dst_fmt.height = f->fmt.pix.height;
	ctx->dst_fmt.colorspace = f->fmt.pix.colorspace;
	ctx->dst_fmt.xfer_func = f->fmt.pix.xfer_func;
	ctx->dst_fmt.ycbcr_enc = f->fmt.pix.ycbcr_enc;
	ctx->dst_fmt.quantization = f->fmt.pix.quantization;
	ctx->dst_fmt.field = V4L2_FIELD_NONE;
	deinterlace_prepare_format(&ctx->dst_fmt);

	return 0;
}

static const struct v4l2_ioctl_ops deinterlace_ioctl_ops = {
	.vidioc_querycap		= deinterlace_querycap,

	.vidioc_enum_framesizes		= deinterlace_enum_framesizes,

	.vidioc_enum_fmt_vid_cap	= deinterlace_enum_fmt,
	.vidioc_g_fmt_vid_cap		= deinterlace_g_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap		= deinterlace_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap		= deinterlace_s_fmt_vid_cap,

	.vidioc_enum_fmt_vid_out	= deinterlace_enum_fmt,
	.vidioc_g_fmt_vid_out		= deinterlace_g_fmt_vid_out,
	.vidioc_try_fmt_vid_out		= deinterlace_try_fmt_vid_out,
	.vidioc_s_fmt_vid_out		= deinterlace_s_fmt_vid_out,

	.vidioc_reqbufs			= v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf		= v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf			= v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf			= v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf		= v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs		= v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf			= v4l2_m2m_ioctl_expbuf,

	.vidioc_streamon		= v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff		= v4l2_m2m_ioctl_streamoff,

	.vidioc_subscribe_event		= v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event	= v4l2_event_unsubscribe,
};

static int deinterlace_queue_setup(struct vb2_queue *vq, unsigned int *nbuffers,
				   unsigned int *nplanes, unsigned int sizes[],
				   struct device *alloc_devs[])
{
	struct deinterlace_ctx *ctx = vb2_get_drv_priv(vq);
	struct v4l2_pix_format *pix_fmt;

	if (V4L2_TYPE_IS_OUTPUT(vq->type))
		pix_fmt = &ctx->src_fmt;
	else
		pix_fmt = &ctx->dst_fmt;

	if (*nplanes) {
		if (sizes[0] < pix_fmt->sizeimage)
			return -EINVAL;
	} else {
		sizes[0] = pix_fmt->sizeimage;
		*nplanes = 1;
	}

	return 0;
}

static int deinterlace_buf_prepare(struct vb2_buffer *vb)
{
	struct vb2_queue *vq = vb->vb2_queue;
	struct deinterlace_ctx *ctx = vb2_get_drv_priv(vq);
	struct v4l2_pix_format *pix_fmt;

	if (V4L2_TYPE_IS_OUTPUT(vq->type))
		pix_fmt = &ctx->src_fmt;
	else
		pix_fmt = &ctx->dst_fmt;

	if (vb2_plane_size(vb, 0) < pix_fmt->sizeimage)
		return -EINVAL;

	vb2_set_plane_payload(vb, 0, pix_fmt->sizeimage);

	return 0;
}

static void deinterlace_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct deinterlace_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
}

static int deinterlace_buf_out_validate(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct deinterlace_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	vbuf->field = ctx->src_fmt.field;

	return 0;
}

static void deinterlace_buf_request_complete(struct vb2_buffer *vb)
{
	struct deinterlace_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_ctrl_request_complete(vb->req_obj.req, &ctx->hdl);
}

static void deinterlace_queue_cleanup(struct vb2_queue *vq, u32 state)
{
	struct deinterlace_ctx *ctx = vb2_get_drv_priv(vq);
	struct vb2_v4l2_buffer *vbuf;

	do {
		if (V4L2_TYPE_IS_OUTPUT(vq->type))
			vbuf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		else
			vbuf = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);

		if (vbuf)
			v4l2_m2m_buf_done(vbuf, state);
	} while (vbuf);

	if (V4L2_TYPE_IS_OUTPUT(vq->type)) {
		if (ctx->prev[0])
			v4l2_m2m_buf_done(ctx->prev[0], state);
		if (ctx->prev[1])
			v4l2_m2m_buf_done(ctx->prev[1], state);
		ctx->prev[0] = NULL;
		ctx->prev[1] = NULL;
	}
}

static int deinterlace_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct deinterlace_ctx *ctx = vb2_get_drv_priv(vq);
	struct device *dev = ctx->dev->dev;
	int ret;

	if (V4L2_TYPE_IS_OUTPUT(vq->type)) {
		ret = pm_runtime_resume_and_get(dev);
		if (ret < 0) {
			dev_err(dev, "Failed to enable module\n");

			goto err_runtime_get;
		}

		ctx->first_field =
			ctx->src_fmt.field == V4L2_FIELD_INTERLACED_BT;

		ctx->prev[0] = NULL;
		ctx->prev[1] = NULL;
		ctx->aborting = 0;

		ctx->flag1_buf = dma_alloc_coherent(dev, DEINTERLACE_FLAG_SIZE,
						    &ctx->flag1_buf_dma,
						    GFP_KERNEL);
		if (!ctx->flag1_buf) {
			ret = -ENOMEM;

			goto err_no_mem1;
		}

		ctx->flag2_buf = dma_alloc_coherent(dev, DEINTERLACE_FLAG_SIZE,
						    &ctx->flag2_buf_dma,
						    GFP_KERNEL);
		if (!ctx->flag2_buf) {
			ret = -ENOMEM;

			goto err_no_mem2;
		}
	}

	return 0;

err_no_mem2:
	dma_free_coherent(dev, DEINTERLACE_FLAG_SIZE, ctx->flag1_buf,
			  ctx->flag1_buf_dma);
err_no_mem1:
	pm_runtime_put(dev);
err_runtime_get:
	deinterlace_queue_cleanup(vq, VB2_BUF_STATE_QUEUED);

	return ret;
}

static void deinterlace_stop_streaming(struct vb2_queue *vq)
{
	struct deinterlace_ctx *ctx = vb2_get_drv_priv(vq);

	if (V4L2_TYPE_IS_OUTPUT(vq->type)) {
		struct device *dev = ctx->dev->dev;

		dma_free_coherent(dev, DEINTERLACE_FLAG_SIZE, ctx->flag1_buf,
				  ctx->flag1_buf_dma);
		dma_free_coherent(dev, DEINTERLACE_FLAG_SIZE, ctx->flag2_buf,
				  ctx->flag2_buf_dma);

		pm_runtime_put(dev);
	}

	deinterlace_queue_cleanup(vq, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops deinterlace_qops = {
	.queue_setup		= deinterlace_queue_setup,
	.buf_prepare		= deinterlace_buf_prepare,
	.buf_queue		= deinterlace_buf_queue,
	.buf_out_validate	= deinterlace_buf_out_validate,
	.buf_request_complete	= deinterlace_buf_request_complete,
	.start_streaming	= deinterlace_start_streaming,
	.stop_streaming		= deinterlace_stop_streaming,
};

static int deinterlace_queue_init(void *priv, struct vb2_queue *src_vq,
				  struct vb2_queue *dst_vq)
{
	struct deinterlace_ctx *ctx = priv;
	int ret;

	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	src_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	src_vq->drv_priv = ctx;
	src_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src_vq->ops = &deinterlace_qops;
	src_vq->mem_ops = &vb2_dma_contig_memops;
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src_vq->lock = &ctx->dev->dev_mutex;
	src_vq->dev = ctx->dev->dev;
	src_vq->supports_requests = true;

	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	dst_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	dst_vq->drv_priv = ctx;
	dst_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	dst_vq->min_queued_buffers = 2;
	dst_vq->ops = &deinterlace_qops;
	dst_vq->mem_ops = &vb2_dma_contig_memops;
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->lock = &ctx->dev->dev_mutex;
	dst_vq->dev = ctx->dev->dev;

	ret = vb2_queue_init(dst_vq);
	if (ret)
		return ret;

	return 0;
}

/* driver-private, order: FID12, FID23, FOD_FID30/32/10/12, FRD02/13, FIELD_HIST0/1 */
#define V4L2_CID_SUNXI_DI300_FMD_STATS	(V4L2_CID_USER_BASE + 0x1100)

/*
 * Plain enable switch rather than something tied to the Request API:
 * FMD's per-job stats aren't associated with any per-job *input*, so
 * there is nothing to bind atomically to a specific job the way
 * stateless codec controls are. A request bound to the job that
 * produced a value would also be the wrong tool here regardless: this
 * driver retains OUTPUT buffers as temporal reference for up to two
 * further jobs, so a request on one wouldn't reach
 * MEDIA_REQUEST_STATE_COMPLETE until released that much later. Set
 * once at stream start; the stats control is read with a plain
 * VIDIOC_G_EXT_CTRLS, no request needed for that either.
 */
#define V4L2_CID_SUNXI_DI300_FMD_ENABLE	(V4L2_CID_USER_BASE + 0x1101)

static int deinterlace_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	struct deinterlace_ctx *ctx =
		container_of(ctrl->handler, struct deinterlace_ctx, hdl);

	switch (ctrl->id) {
	case V4L2_CID_SUNXI_DI300_FMD_STATS:
		memcpy(ctrl->p_new.p_u32, ctx->dev->fmd_stats,
		       sizeof(ctx->dev->fmd_stats));
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int deinterlace_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct deinterlace_ctx *ctx =
		container_of(ctrl->handler, struct deinterlace_ctx, hdl);

	switch (ctrl->id) {
	case V4L2_CID_SUNXI_DI300_FMD_ENABLE:
		ctx->fmd_enabled = ctrl->val;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct v4l2_ctrl_ops deinterlace_ctrl_ops = {
	.g_volatile_ctrl = deinterlace_g_volatile_ctrl,
	.s_ctrl		 = deinterlace_s_ctrl,
};

static const struct v4l2_ctrl_config deinterlace_fmd_stats_ctrl = {
	.ops	= &deinterlace_ctrl_ops,
	.id	= V4L2_CID_SUNXI_DI300_FMD_STATS,
	.name	= "FMD Raw Statistics",
	.type	= V4L2_CTRL_TYPE_U32,
	.flags	= V4L2_CTRL_FLAG_READ_ONLY | V4L2_CTRL_FLAG_VOLATILE,
	.min	= 0,
	.max	= DEINTERLACE_FMD_HIST_CNT_MASK,
	.step	= 1,
	.def	= 0,
	.dims	= { DEINTERLACE_FMD_STATS_COUNT },
};

static const struct v4l2_ctrl_config deinterlace_fmd_enable_ctrl = {
	.ops	= &deinterlace_ctrl_ops,
	.id	= V4L2_CID_SUNXI_DI300_FMD_ENABLE,
	.name	= "FMD Enable",
	.type	= V4L2_CTRL_TYPE_BOOLEAN,
	.min	= 0,
	.max	= 1,
	.step	= 1,
	.def	= 0,
};

/*
 * Per-job field-weave decision: [0] for the job's first output
 * (dst0), [1] for its second (dst1). 0-2 select a weave phase for
 * that output (direct field combine instead of motion-adaptive
 * blend); 3 disables weave for that output, falling back to whatever
 * DIT_SETTING already configures. Bound to a request the way
 * stateless codec controls are -- see the comment on ctx->weave_ctrl.
 */
#define V4L2_CID_SUNXI_DI300_FMD_WEAVE	(V4L2_CID_USER_BASE + 0x1102)
#define DEINTERLACE_FMD_WEAVE_COUNT	2
#define DEINTERLACE_FMD_WEAVE_DISABLED	3

static const struct v4l2_ctrl_config deinterlace_fmd_weave_ctrl = {
	.id	= V4L2_CID_SUNXI_DI300_FMD_WEAVE,
	.name	= "FMD Field Weave",
	.type	= V4L2_CTRL_TYPE_U32,
	.min	= 0,
	.max	= DEINTERLACE_FMD_WEAVE_DISABLED,
	.step	= 1,
	.def	= DEINTERLACE_FMD_WEAVE_DISABLED,
	.dims	= { DEINTERLACE_FMD_WEAVE_COUNT },
};

static int deinterlace_open(struct file *file)
{
	struct deinterlace_dev *dev = video_drvdata(file);
	struct deinterlace_ctx *ctx = NULL;
	int ret;

	if (mutex_lock_interruptible(&dev->dev_mutex))
		return -ERESTARTSYS;

	ctx = kzalloc_obj(*ctx);
	if (!ctx) {
		mutex_unlock(&dev->dev_mutex);
		return -ENOMEM;
	}

	ctx->src_fmt.pixelformat = deinterlace_formats[0];
	ctx->src_fmt.field = V4L2_FIELD_INTERLACED;
	ctx->src_fmt.width = 640;
	ctx->src_fmt.height = 480;
	deinterlace_prepare_format(&ctx->src_fmt);

	ctx->dst_fmt.pixelformat = deinterlace_formats[0];
	ctx->dst_fmt.field = V4L2_FIELD_NONE;
	ctx->dst_fmt.width = 640;
	ctx->dst_fmt.height = 480;
	deinterlace_prepare_format(&ctx->dst_fmt);

	v4l2_fh_init(&ctx->fh, video_devdata(file));
	ctx->dev = dev;

	if (dev->has_fmd) {
		v4l2_ctrl_handler_init(&ctx->hdl, 3);
		v4l2_ctrl_new_custom(&ctx->hdl, &deinterlace_fmd_stats_ctrl, NULL);
		v4l2_ctrl_new_custom(&ctx->hdl, &deinterlace_fmd_enable_ctrl, NULL);
		ctx->weave_ctrl = v4l2_ctrl_new_custom(&ctx->hdl,
						      &deinterlace_fmd_weave_ctrl,
						      NULL);
		if (ctx->hdl.error) {
			ret = ctx->hdl.error;
			v4l2_ctrl_handler_free(&ctx->hdl);
			v4l2_fh_exit(&ctx->fh);
			goto err_free;
		}
		ctx->fh.ctrl_handler = &ctx->hdl;
	}

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(dev->m2m_dev, ctx,
					    &deinterlace_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		v4l2_ctrl_handler_free(&ctx->hdl);
		v4l2_fh_exit(&ctx->fh);
		goto err_free;
	}

	v4l2_fh_add(&ctx->fh, file);

	mutex_unlock(&dev->dev_mutex);

	return 0;

err_free:
	kfree(ctx);
	mutex_unlock(&dev->dev_mutex);

	return ret;
}

static int deinterlace_release(struct file *file)
{
	struct deinterlace_dev *dev = video_drvdata(file);
	struct deinterlace_ctx *ctx = deinterlace_file2ctx(file);

	mutex_lock(&dev->dev_mutex);

	v4l2_fh_del(&ctx->fh, file);
	v4l2_fh_exit(&ctx->fh);
	v4l2_ctrl_handler_free(&ctx->hdl);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);

	kfree(ctx);

	mutex_unlock(&dev->dev_mutex);

	return 0;
}

static const struct v4l2_file_operations deinterlace_fops = {
	.owner		= THIS_MODULE,
	.open		= deinterlace_open,
	.release	= deinterlace_release,
	.poll		= v4l2_m2m_fop_poll,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= v4l2_m2m_fop_mmap,
};

static const struct video_device deinterlace_video_device = {
	.name		= DEINTERLACE_NAME,
	.vfl_dir	= VFL_DIR_M2M,
	.fops		= &deinterlace_fops,
	.ioctl_ops	= &deinterlace_ioctl_ops,
	.minor		= -1,
	.release	= video_device_release_empty,
	.device_caps	= V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING,
};

static const struct v4l2_m2m_ops deinterlace_m2m_ops = {
	.device_run	= deinterlace_device_run,
	.job_ready	= deinterlace_job_ready,
	.job_abort	= deinterlace_job_abort,
};

static int deinterlace_request_validate(struct media_request *req)
{
	struct media_request_object *obj;
	struct deinterlace_ctx *ctx = NULL;
	unsigned int count;

	list_for_each_entry(obj, &req->objects, list) {
		struct vb2_buffer *vb;

		if (vb2_request_object_is_buffer(obj)) {
			vb = container_of(obj, struct vb2_buffer, req_obj);
			ctx = vb2_get_drv_priv(vb->vb2_queue);

			break;
		}
	}

	if (!ctx)
		return -ENOENT;

	count = vb2_request_buffer_cnt(req);
	if (!count) {
		v4l2_info(&ctx->dev->v4l2_dev,
			  "No buffer was provided with the request\n");
		return -ENOENT;
	} else if (count > 1) {
		v4l2_info(&ctx->dev->v4l2_dev,
			  "More than one buffer was provided with the request\n");
		return -EINVAL;
	}

	return vb2_request_validate(req);
}

static const struct media_device_ops deinterlace_media_ops = {
	.req_validate	= deinterlace_request_validate,
	.req_queue	= v4l2_m2m_request_queue,
};

static int deinterlace_probe(struct platform_device *pdev)
{
	struct deinterlace_dev *dev;
	struct video_device *vfd;
	int irq, ret;

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->vfd = deinterlace_video_device;
	dev->dev = &pdev->dev;

	irq = platform_get_irq(pdev, 0);
	if (irq <= 0)
		return irq;

	ret = devm_request_irq(dev->dev, irq, deinterlace_irq,
			       0, dev_name(dev->dev), dev);
	if (ret) {
		dev_err(dev->dev, "Failed to request IRQ\n");

		return ret;
	}

	dev->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(dev->base))
		return PTR_ERR(dev->base);

	dev->bus_clk = devm_clk_get(dev->dev, "bus");
	if (IS_ERR(dev->bus_clk)) {
		dev_err(dev->dev, "Failed to get bus clock\n");

		return PTR_ERR(dev->bus_clk);
	}

	dev->mod_clk = devm_clk_get(dev->dev, "mod");
	if (IS_ERR(dev->mod_clk)) {
		dev_err(dev->dev, "Failed to get mod clock\n");

		return PTR_ERR(dev->mod_clk);
	}

	dev->rstc = devm_reset_control_get(dev->dev, NULL);
	if (IS_ERR(dev->rstc)) {
		dev_err(dev->dev, "Failed to get reset control\n");

		return PTR_ERR(dev->rstc);
	}

	mutex_init(&dev->dev_mutex);

	ret = v4l2_device_register(&pdev->dev, &dev->v4l2_dev);
	if (ret) {
		dev_err(dev->dev, "Failed to register V4L2 device\n");

		return ret;
	}

	vfd = &dev->vfd;
	vfd->lock = &dev->dev_mutex;
	vfd->v4l2_dev = &dev->v4l2_dev;

	snprintf(vfd->name, sizeof(vfd->name), "%s",
		 deinterlace_video_device.name);
	video_set_drvdata(vfd, dev);

	dev->m2m_dev = v4l2_m2m_init(&deinterlace_m2m_ops);
	if (IS_ERR(dev->m2m_dev)) {
		v4l2_err(&dev->v4l2_dev,
			 "Failed to initialize V4L2 M2M device\n");
		ret = PTR_ERR(dev->m2m_dev);

		goto err_v4l2;
	}

	dev->mdev.dev = &pdev->dev;
	strscpy(dev->mdev.model, DEINTERLACE_NAME, sizeof(dev->mdev.model));
	strscpy(dev->mdev.bus_info, "platform:" DEINTERLACE_NAME,
		sizeof(dev->mdev.bus_info));

	media_device_init(&dev->mdev);
	dev->mdev.ops = &deinterlace_media_ops;
	dev->v4l2_dev.mdev = &dev->mdev;

	ret = video_register_device(vfd, VFL_TYPE_VIDEO, 0);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "Failed to register video device\n");

		goto err_media;
	}

	v4l2_info(&dev->v4l2_dev,
		  "Device registered as /dev/video%d\n", vfd->num);

	ret = v4l2_m2m_register_media_controller(dev->m2m_dev, vfd,
						 MEDIA_ENT_F_PROC_VIDEO_COMPOSER);
	if (ret) {
		v4l2_err(&dev->v4l2_dev,
			 "Failed to initialize V4L2 M2M media controller\n");
		goto err_video;
	}

	ret = media_device_register(&dev->mdev);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "Failed to register media device\n");
		goto err_m2m_mc;
	}

	platform_set_drvdata(pdev, dev);

	pm_runtime_enable(dev->dev);

	/*
	 * Probe dev->has_fmd once up front, so it is stable by the time
	 * the first open() decides whether to expose the FMD stats
	 * control: has_fmd is otherwise only set on runtime resume, which
	 * doesn't happen until the first client streams.
	 */
	ret = pm_runtime_resume_and_get(dev->dev);
	if (ret < 0) {
		dev_err(dev->dev, "Failed to enable module\n");

		goto err_mdev;
	}
	pm_runtime_put(dev->dev);

	return 0;

err_mdev:
	pm_runtime_disable(dev->dev);
	media_device_unregister(&dev->mdev);
err_m2m_mc:
	v4l2_m2m_unregister_media_controller(dev->m2m_dev);
err_video:
	video_unregister_device(&dev->vfd);
err_media:
	media_device_cleanup(&dev->mdev);
	v4l2_m2m_release(dev->m2m_dev);
err_v4l2:
	v4l2_device_unregister(&dev->v4l2_dev);

	return ret;
}

static void deinterlace_remove(struct platform_device *pdev)
{
	struct deinterlace_dev *dev = platform_get_drvdata(pdev);

	if (media_devnode_is_registered(dev->mdev.devnode)) {
		media_device_unregister(&dev->mdev);
		v4l2_m2m_unregister_media_controller(dev->m2m_dev);
		media_device_cleanup(&dev->mdev);
	}

	v4l2_m2m_release(dev->m2m_dev);
	video_unregister_device(&dev->vfd);
	v4l2_device_unregister(&dev->v4l2_dev);

	pm_runtime_force_suspend(&pdev->dev);
}

static int deinterlace_runtime_resume(struct device *device)
{
	struct deinterlace_dev *dev = dev_get_drvdata(device);
	int ret;

	ret = clk_set_rate_exclusive(dev->mod_clk, 300000000);
	if (ret) {
		dev_err(dev->dev, "Failed to set exclusive mod clock rate\n");

		return ret;
	}

	ret = reset_control_deassert(dev->rstc);
	if (ret) {
		dev_err(dev->dev, "Failed to deassert reset\n");

		goto err_exclusive_rate;
	}

	ret = clk_prepare_enable(dev->bus_clk);
	if (ret) {
		dev_err(dev->dev, "Failed to enable bus clock\n");

		goto err_rst;
	}

	ret = clk_prepare_enable(dev->mod_clk);
	if (ret) {
		dev_err(dev->dev, "Failed to enable mod clock\n");

		goto err_bus_clk;
	}

	deinterlace_init(dev);

	return 0;

err_bus_clk:
	clk_disable_unprepare(dev->bus_clk);
err_rst:
	reset_control_assert(dev->rstc);
err_exclusive_rate:
	clk_rate_exclusive_put(dev->mod_clk);

	return ret;
}

static int deinterlace_runtime_suspend(struct device *device)
{
	struct deinterlace_dev *dev = dev_get_drvdata(device);

	clk_disable_unprepare(dev->mod_clk);
	clk_disable_unprepare(dev->bus_clk);

	reset_control_assert(dev->rstc);

	clk_rate_exclusive_put(dev->mod_clk);

	return 0;
}

static const struct of_device_id deinterlace_dt_match[] = {
	{ .compatible = "allwinner,sun50i-h616-deinterlace" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, deinterlace_dt_match);

static const struct dev_pm_ops deinterlace_pm_ops = {
	.runtime_resume		= deinterlace_runtime_resume,
	.runtime_suspend	= deinterlace_runtime_suspend,
};

static struct platform_driver deinterlace_driver = {
	.probe		= deinterlace_probe,
	.remove		= deinterlace_remove,
	.driver		= {
		.name		= DEINTERLACE_NAME,
		.of_match_table	= deinterlace_dt_match,
		.pm		= &deinterlace_pm_ops,
	},
};
module_platform_driver(deinterlace_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Jernej Skrabec <jernej.skrabec@siol.net>");
MODULE_DESCRIPTION("Allwinner DI300 deinterlace driver");
