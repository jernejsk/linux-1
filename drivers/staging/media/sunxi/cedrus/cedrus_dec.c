// SPDX-License-Identifier: GPL-2.0
/*
 * Cedrus VPU driver
 *
 * Copyright (C) 2016 Florent Revest <florent.revest@free-electrons.com>
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 * Copyright (C) 2018 Bootlin
 *
 * Based on the vim2m driver, that is:
 *
 * Copyright (c) 2009-2010 Samsung Electronics Co., Ltd.
 * Pawel Osciak, <pawel@osciak.com>
 * Marek Szyprowski, <m.szyprowski@samsung.com>
 */

#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-event.h>
#include <media/v4l2-mem2mem.h>

#include "cedrus.h"
#include "cedrus_dec.h"
#include "cedrus_hw.h"

/*
 * How long the engine is kept for a picture while its next slice is awaited.
 * After that the other contexts get their turn.
 */
#define CEDRUS_PIC_GUARD_MS	100

static void cedrus_run_slice(struct cedrus_ctx *ctx, bool new_frame)
{
	struct cedrus_dev *dev = ctx->dev;
	struct cedrus_run run = {};
	struct media_request *src_req;
	int error;

	run.new_frame = new_frame;
	run.src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	run.dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);

	/* Apply request(s) controls if needed. */
	src_req = run.src->vb2_buf.req_obj.req;

	if (src_req)
		v4l2_ctrl_request_setup(src_req, &ctx->hdl);

	switch (ctx->src_fmt.pixelformat) {
	case V4L2_PIX_FMT_MPEG2_SLICE:
		run.mpeg2.sequence = cedrus_find_control_data(ctx,
			V4L2_CID_STATELESS_MPEG2_SEQUENCE);
		run.mpeg2.picture = cedrus_find_control_data(ctx,
			V4L2_CID_STATELESS_MPEG2_PICTURE);
		run.mpeg2.quantisation = cedrus_find_control_data(ctx,
			V4L2_CID_STATELESS_MPEG2_QUANTISATION);
		break;

	case V4L2_PIX_FMT_H264_SLICE:
		run.h264.decode_params = cedrus_find_control_data(ctx,
			V4L2_CID_STATELESS_H264_DECODE_PARAMS);
		run.h264.pps = cedrus_find_control_data(ctx,
			V4L2_CID_STATELESS_H264_PPS);
		run.h264.scaling_matrix = cedrus_find_control_data(ctx,
			V4L2_CID_STATELESS_H264_SCALING_MATRIX);
		run.h264.slice_params = cedrus_find_control_data(ctx,
			V4L2_CID_STATELESS_H264_SLICE_PARAMS);
		run.h264.sps = cedrus_find_control_data(ctx,
			V4L2_CID_STATELESS_H264_SPS);
		run.h264.pred_weights = cedrus_find_control_data(ctx,
			V4L2_CID_STATELESS_H264_PRED_WEIGHTS);
		break;

	case V4L2_PIX_FMT_HEVC_SLICE:
		run.h265.sps = cedrus_find_control_data(ctx,
			V4L2_CID_STATELESS_HEVC_SPS);
		run.h265.pps = cedrus_find_control_data(ctx,
			V4L2_CID_STATELESS_HEVC_PPS);
		run.h265.slice_params = cedrus_find_control_data(ctx,
			V4L2_CID_STATELESS_HEVC_SLICE_PARAMS);
		run.h265.decode_params = cedrus_find_control_data(ctx,
			V4L2_CID_STATELESS_HEVC_DECODE_PARAMS);
		run.h265.scaling_matrix = cedrus_find_control_data(ctx,
			V4L2_CID_STATELESS_HEVC_SCALING_MATRIX);
		run.h265.entry_points = cedrus_find_control_data(ctx,
			V4L2_CID_STATELESS_HEVC_ENTRY_POINT_OFFSETS);
		run.h265.entry_points_count = cedrus_get_num_of_controls(ctx,
			V4L2_CID_STATELESS_HEVC_ENTRY_POINT_OFFSETS);
		break;

	case V4L2_PIX_FMT_VP8_FRAME:
		run.vp8.frame_params = cedrus_find_control_data(ctx,
			V4L2_CID_STATELESS_VP8_FRAME);
		break;

	case V4L2_PIX_FMT_VP9_FRAME:
		run.vp9.frame_params = cedrus_find_control_data(ctx,
			V4L2_CID_STATELESS_VP9_FRAME);
		run.vp9.prob_updates = cedrus_find_control_data(ctx,
			V4L2_CID_STATELESS_VP9_COMPRESSED_HDR);
		break;

	default:
		break;
	}

	v4l2_m2m_buf_copy_metadata(run.src, run.dst);

	cedrus_dst_format_set(dev, &ctx->dst_fmt);

	error = ctx->current_codec->setup(ctx, &run);
	if (error)
		v4l2_err(&ctx->dev->v4l2_dev,
			 "Failed to setup decoding job: %d\n", error);

	/* Complete request(s) controls if needed. */

	if (src_req)
		v4l2_ctrl_request_complete(src_req, &ctx->hdl);

	/* Trigger decoding if setup went well, bail out otherwise. */
	if (!error) {
		/* Start the watchdog timer. */
		schedule_delayed_work(&dev->watchdog_work,
				      msecs_to_jiffies(2000));

		ctx->current_codec->trigger(ctx);
	} else {
		cedrus_job_done(ctx, VB2_BUF_STATE_ERROR);
	}
}

void cedrus_device_run(void *priv)
{
	struct cedrus_ctx *ctx = priv;

	ctx->job_aborted = false;
	cedrus_run_slice(ctx, ctx->fh.m2m_ctx->new_frame);
}

/* The OUTPUT buffer which is queued behind the one at the head. */
static struct vb2_v4l2_buffer *cedrus_pic_next_src(struct cedrus_ctx *ctx)
{
	struct v4l2_m2m_ctx *m2m_ctx = ctx->fh.m2m_ctx;
	struct vb2_v4l2_buffer *vbuf = NULL;
	struct v4l2_m2m_buffer *b;
	unsigned int count = 0;
	unsigned long flags;

	spin_lock_irqsave(&m2m_ctx->out_q_ctx.rdy_spinlock, flags);
	v4l2_m2m_for_each_src_buf(m2m_ctx, b) {
		if (count++ == 1) {
			vbuf = &b->vb;
			break;
		}
	}
	spin_unlock_irqrestore(&m2m_ctx->out_q_ctx.rdy_spinlock, flags);

	return vbuf;
}

/* Stop waiting for the next slice, if @ctx (or any context) does. */
static bool cedrus_pic_stop_waiting(struct cedrus_dev *dev,
				    struct cedrus_ctx **ctx,
				    enum vb2_buffer_state *state)
{
	unsigned long flags;
	bool waiting;

	spin_lock_irqsave(&dev->pic_lock, flags);
	waiting = dev->pic_wait && (!*ctx || dev->pic_ctx == *ctx);
	if (waiting) {
		*ctx = dev->pic_ctx;
		*state = dev->pic_src_state;
		dev->pic_wait = false;
		dev->pic_ctx = NULL;
	}
	spin_unlock_irqrestore(&dev->pic_lock, flags);

	return waiting;
}

/*
 * A slice is done. The slices of a picture can't be interleaved with the
 * jobs of other contexts, as the engine carries state from one slice to the
 * next. So when the slice holds the capture buffer, more slices of the
 * picture follow and the job is kept until the last one is done. The OUTPUT
 * buffer of the slice is kept as well, the job is finished with it when the
 * next slice doesn't arrive.
 */
void cedrus_job_done(struct cedrus_ctx *ctx, enum vb2_buffer_state state)
{
	struct cedrus_dev *dev = ctx->dev;
	struct vb2_v4l2_buffer *src;
	unsigned long flags;

	src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);

	if (!src || !(src->flags & V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF) ||
	    ctx->job_aborted) {
		spin_lock_irqsave(&dev->pic_lock, flags);
		dev->pic_ctx = NULL;
		spin_unlock_irqrestore(&dev->pic_lock, flags);

		v4l2_m2m_buf_done_and_job_finish(dev->m2m_dev,
						 ctx->fh.m2m_ctx, state);
		return;
	}

	spin_lock_irqsave(&dev->pic_lock, flags);
	dev->pic_ctx = ctx;
	dev->pic_src_state = state;
	dev->pic_wait = true;
	spin_unlock_irqrestore(&dev->pic_lock, flags);

	mod_delayed_work(system_wq, &dev->pic_guard_work,
			 msecs_to_jiffies(CEDRUS_PIC_GUARD_MS));
	schedule_work(&dev->pic_work);
}

/* Run the next slice of the picture, if it is queued. */
void cedrus_pic_work(struct work_struct *work)
{
	struct cedrus_dev *dev = container_of(work, struct cedrus_dev, pic_work);
	struct vb2_v4l2_buffer *src, *next, *dst;
	enum vb2_buffer_state state;
	struct cedrus_ctx *ctx;
	unsigned long flags;

	spin_lock_irqsave(&dev->pic_lock, flags);
	ctx = dev->pic_ctx;
	next = dev->pic_wait ? cedrus_pic_next_src(ctx) : NULL;
	if (!next) {
		spin_unlock_irqrestore(&dev->pic_lock, flags);
		return;
	}
	state = dev->pic_src_state;
	dev->pic_wait = false;
	spin_unlock_irqrestore(&dev->pic_lock, flags);

	cancel_delayed_work(&dev->pic_guard_work);

	dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
	if (next->vb2_buf.timestamp != dst->vb2_buf.timestamp) {
		/*
		 * This is another picture already. Finish the job, the
		 * capture buffer is then returned by v4l2-mem2mem.
		 */
		spin_lock_irqsave(&dev->pic_lock, flags);
		dev->pic_ctx = NULL;
		spin_unlock_irqrestore(&dev->pic_lock, flags);

		v4l2_m2m_buf_done_and_job_finish(dev->m2m_dev,
						 ctx->fh.m2m_ctx, state);
		return;
	}

	src = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
	v4l2_m2m_buf_done(src, state);

	cedrus_run_slice(ctx, false);
}

/* The next slice did not arrive in time. */
void cedrus_pic_guard(struct work_struct *work)
{
	struct cedrus_dev *dev = container_of(to_delayed_work(work),
					      struct cedrus_dev, pic_guard_work);
	enum vb2_buffer_state state;
	struct cedrus_ctx *ctx = NULL;

	if (!cedrus_pic_stop_waiting(dev, &ctx, &state))
		return;

	dev_dbg(dev->dev, "next slice of the picture is late\n");
	v4l2_m2m_buf_done_and_job_finish(dev->m2m_dev, ctx->fh.m2m_ctx, state);
}

/* The capture buffer is not held anymore (decoder flush command). */
void cedrus_pic_flush(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;
	enum vb2_buffer_state state;
	struct vb2_v4l2_buffer *src;

	src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	if (!src || (src->flags & V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF))
		return;

	if (cedrus_pic_stop_waiting(dev, &ctx, &state))
		v4l2_m2m_buf_done_and_job_finish(dev->m2m_dev,
						 ctx->fh.m2m_ctx, state);
}

/* An OUTPUT buffer was queued, it may be the slice which is awaited. */
void cedrus_pic_slice_queued(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;

	if (READ_ONCE(dev->pic_ctx) == ctx)
		schedule_work(&dev->pic_work);
}

void cedrus_job_abort(void *priv)
{
	struct cedrus_ctx *ctx = priv;
	struct cedrus_dev *dev = ctx->dev;
	enum vb2_buffer_state state;

	/* A slice which is being decoded ends the job when it is done. */
	ctx->job_aborted = true;

	if (cedrus_pic_stop_waiting(dev, &ctx, &state))
		v4l2_m2m_buf_done_and_job_finish(dev->m2m_dev,
						 ctx->fh.m2m_ctx, state);
}
