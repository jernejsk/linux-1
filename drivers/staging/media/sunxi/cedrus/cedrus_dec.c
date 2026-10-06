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

#include <linux/reset.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-event.h>
#include <media/v4l2-mem2mem.h>

#include "cedrus.h"
#include "cedrus_dec.h"
#include "cedrus_hw.h"

/*
 * Let the other contexts run again once @ctx no longer holds the engine in
 * the middle of a picture. Snapshot the contexts under the lock and schedule
 * them outside it: job_ready() is called with the m2m job lock held and takes
 * sched_lock, so the reverse nesting must be avoided.
 */
/* DEBUG (h616-bench): 0 = off, 1 = ctx-switch reset only, 2 = reset + picture gating */
int cedrus_sched_gate;
module_param_named(sched_gate, cedrus_sched_gate, int, 0644);

void cedrus_release_held(struct cedrus_dev *dev, struct cedrus_ctx *ctx)
{
	struct v4l2_m2m_ctx *wake[16];
	struct cedrus_ctx *other;
	unsigned long flags;
	unsigned int n = 0, i;

	spin_lock_irqsave(&dev->sched_lock, flags);
	if (dev->held_ctx != ctx) {
		spin_unlock_irqrestore(&dev->sched_lock, flags);
		return;
	}
	dev->held_ctx = NULL;
	list_for_each_entry(other, &dev->ctxs, list)
		if (other != ctx && n < ARRAY_SIZE(wake))
			wake[n++] = other->fh.m2m_ctx;
	spin_unlock_irqrestore(&dev->sched_lock, flags);

	for (i = 0; i < n; i++)
		v4l2_m2m_try_schedule(wake[i]);
}

void cedrus_device_run(void *priv)
{
	struct cedrus_ctx *ctx = priv;
	struct cedrus_dev *dev = ctx->dev;
	struct cedrus_run run = {};
	struct media_request *src_req;
	int error;

	run.src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	run.dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);

	/*
	 * The engine keeps internal state between jobs (SRAM tables, row
	 * buffers, entropy state) that belongs to the context that ran last.
	 * Reset it whenever another context takes over, as the vendor library
	 * does on every decoder switch.
	 */
	if (cedrus_sched_gate && dev->last_ctx != ctx) {
		reset_control_reset(dev->rstc);
		dev->last_ctx = ctx;
	}

	/*
	 * A source buffer with HOLD_CAPTURE_BUF is not the last slice of its
	 * picture: keep the engine for this context until the picture is done.
	 */
	if (cedrus_sched_gate >= 2 &&
	    (run.src->flags & V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF)) {
		unsigned long flags;

		spin_lock_irqsave(&dev->sched_lock, flags);
		dev->held_ctx = ctx;
		spin_unlock_irqrestore(&dev->sched_lock, flags);
	} else {
		cedrus_release_held(dev, ctx);
	}

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

	case V4L2_PIX_FMT_VC1_SLICE:
		run.vc1.sequence = cedrus_find_control_data(ctx,
			V4L2_CID_STATELESS_VC1_SEQUENCE);
		run.vc1.entrypoint = cedrus_find_control_data(ctx,
			V4L2_CID_STATELESS_VC1_ENTRYPOINT_HEADER);
		run.vc1.picture = cedrus_find_control_data(ctx,
			V4L2_CID_STATELESS_VC1_PICTURE_LAYER);
		run.vc1.bitplanes = cedrus_find_control_data(ctx,
			V4L2_CID_STATELESS_VC1_BITPLANES);
		/*
		 * The slice parameters only describe the picture of the
		 * request which sets them.
		 */
		if (cedrus_ctrl_in_request(ctx, src_req,
					   V4L2_CID_STATELESS_VC1_SLICE_PARAMS)) {
			run.vc1.slices = cedrus_find_control_data(ctx,
				V4L2_CID_STATELESS_VC1_SLICE_PARAMS);
			run.vc1.num_slices = cedrus_get_num_of_controls(ctx,
				V4L2_CID_STATELESS_VC1_SLICE_PARAMS);
		}
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
		v4l2_m2m_buf_done_and_job_finish(ctx->dev->m2m_dev,
						 ctx->fh.m2m_ctx,
						 VB2_BUF_STATE_ERROR);
	}
}
