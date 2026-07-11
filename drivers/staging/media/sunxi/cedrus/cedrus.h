/* SPDX-License-Identifier: GPL-2.0 */
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

#ifndef _CEDRUS_H_
#define _CEDRUS_H_

#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-dma-contig.h>

#include <linux/iopoll.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>

#define CEDRUS_NAME			"cedrus"

#define CEDRUS_CAPABILITY_UNTILED	BIT(0)
#define CEDRUS_CAPABILITY_H265_DEC	BIT(1)
#define CEDRUS_CAPABILITY_H264_DEC	BIT(2)
#define CEDRUS_CAPABILITY_MPEG2_DEC	BIT(3)
#define CEDRUS_CAPABILITY_VP8_DEC	BIT(4)
#define CEDRUS_CAPABILITY_H265_10_DEC	BIT(5)
#define CEDRUS_CAPABILITY_VP9_DEC	BIT(6)

enum cedrus_irq_status {
	CEDRUS_IRQ_NONE,
	CEDRUS_IRQ_ERROR,
	CEDRUS_IRQ_OK,
};

enum cedrus_h264_pic_type {
	CEDRUS_H264_PIC_TYPE_FRAME	= 0,
	CEDRUS_H264_PIC_TYPE_FIELD,
	CEDRUS_H264_PIC_TYPE_MBAFF,
};

struct cedrus_control {
	struct v4l2_ctrl_config cfg;
	unsigned int		capabilities;
};

struct cedrus_h264_run {
	const struct v4l2_ctrl_h264_decode_params	*decode_params;
	const struct v4l2_ctrl_h264_pps			*pps;
	const struct v4l2_ctrl_h264_scaling_matrix	*scaling_matrix;
	const struct v4l2_ctrl_h264_slice_params	*slice_params;
	const struct v4l2_ctrl_h264_sps			*sps;
	const struct v4l2_ctrl_h264_pred_weights	*pred_weights;
};

struct cedrus_mpeg2_run {
	const struct v4l2_ctrl_mpeg2_sequence		*sequence;
	const struct v4l2_ctrl_mpeg2_picture		*picture;
	const struct v4l2_ctrl_mpeg2_quantisation	*quantisation;
};

struct cedrus_h265_run {
	const struct v4l2_ctrl_hevc_sps			*sps;
	const struct v4l2_ctrl_hevc_pps			*pps;
	const struct v4l2_ctrl_hevc_slice_params	*slice_params;
	const struct v4l2_ctrl_hevc_decode_params	*decode_params;
	const struct v4l2_ctrl_hevc_scaling_matrix	*scaling_matrix;
	const u32					*entry_points;
	u32						entry_points_count;
};

struct cedrus_vp8_run {
	const struct v4l2_ctrl_vp8_frame		*frame_params;
};

struct cedrus_vp9_run {
	const struct v4l2_ctrl_vp9_frame		*frame_params;
	const struct v4l2_ctrl_vp9_compressed_hdr	*prob_updates;
};

struct cedrus_run {
	struct vb2_v4l2_buffer	*src;
	struct vb2_v4l2_buffer	*dst;

	union {
		struct cedrus_h264_run	h264;
		struct cedrus_mpeg2_run	mpeg2;
		struct cedrus_h265_run	h265;
		struct cedrus_vp8_run	vp8;
		struct cedrus_vp9_run	vp9;
	};
};

struct cedrus_vp9_ctx;

struct cedrus_buffer {
	struct v4l2_m2m_buffer          m2m_buf;

	union {
		struct {
			unsigned int			position;
			enum cedrus_h264_pic_type	pic_type;
			void				*mv_col_buf;
			dma_addr_t			mv_col_buf_dma;
			ssize_t				mv_col_buf_size;
		} h264;
		struct {
			void		*mv_col_buf;
			dma_addr_t	mv_col_buf_dma;
			ssize_t		mv_col_buf_size;
		} h265;
		struct {
			u32		width;
			u32		height;
			u8		bit_depth;
		} vp9;
	} codec;
};

struct cedrus_ctx {
	struct v4l2_fh			fh;
	struct cedrus_dev		*dev;

	struct v4l2_pix_format		src_fmt;
	struct v4l2_pix_format		dst_fmt;
	struct cedrus_dec_ops		*current_codec;
	unsigned int			bit_depth;

	struct v4l2_ctrl_handler	hdl;
	struct v4l2_ctrl		**ctrls;

	union {
		struct {
			void		*pic_info_buf;
			dma_addr_t	pic_info_buf_dma;
			ssize_t		pic_info_buf_size;
			void		*neighbor_info_buf;
			dma_addr_t	neighbor_info_buf_dma;
			void		*deblk_buf;
			dma_addr_t	deblk_buf_dma;
			ssize_t		deblk_buf_size;
			void		*intra_pred_buf;
			dma_addr_t	intra_pred_buf_dma;
			ssize_t		intra_pred_buf_size;
		} h264;
		struct {
			void		*neighbor_info_buf;
			dma_addr_t	neighbor_info_buf_addr;
			void		*entry_points_buf;
			dma_addr_t	entry_points_buf_addr;
		} h265;
		struct {
			unsigned int	last_frame_p_type;
			unsigned int	last_filter_type;
			unsigned int	last_sharpness_level;

			u8		*entropy_probs_buf;
			dma_addr_t	entropy_probs_buf_dma;
		} vp8;
		struct cedrus_vp9_ctx	*vp9;
	} codec;
};

static inline struct cedrus_ctx *cedrus_file2ctx(struct file *file)
{
	return container_of(file_to_v4l2_fh(file), struct cedrus_ctx, fh);
}

struct cedrus_dec_ops {
	void (*irq_clear)(struct cedrus_ctx *ctx);
	void (*irq_disable)(struct cedrus_ctx *ctx);
	enum cedrus_irq_status (*irq_status)(struct cedrus_ctx *ctx);
	int (*setup)(struct cedrus_ctx *ctx, struct cedrus_run *run);
	int (*start)(struct cedrus_ctx *ctx);
	void (*stop)(struct cedrus_ctx *ctx);
	void (*trigger)(struct cedrus_ctx *ctx);
	unsigned int (*extra_cap_size)(struct cedrus_ctx *ctx,
				       struct v4l2_pix_format *pix_fmt);
};

struct cedrus_variant {
	unsigned int	capabilities;
	unsigned int	mod_rate;
};

struct cedrus_dev {
	struct v4l2_device	v4l2_dev;
	struct video_device	vfd;
	struct media_device	mdev;
	struct media_pad	pad[2];
	struct platform_device	*pdev;
	struct device		*dev;
	struct v4l2_m2m_dev	*m2m_dev;

	/* Device file mutex */
	struct mutex		dev_mutex;

	void __iomem		*base;

	struct clk		*mod_clk;
	struct clk		*ahb_clk;
	struct clk		*ram_clk;

	struct reset_control	*rstc;

	unsigned int		capabilities;

	struct delayed_work	watchdog_work;
};

extern struct cedrus_dec_ops cedrus_dec_ops_mpeg2;
extern struct cedrus_dec_ops cedrus_dec_ops_h264;
extern struct cedrus_dec_ops cedrus_dec_ops_h265;
extern struct cedrus_dec_ops cedrus_dec_ops_vp8;
extern struct cedrus_dec_ops cedrus_dec_ops_vp9;

static inline void cedrus_write(struct cedrus_dev *dev, u32 reg, u32 val)
{
	writel(val, dev->base + reg);
}

static inline u32 cedrus_read(struct cedrus_dev *dev, u32 reg)
{
	return readl(dev->base + reg);
}

static inline u32 cedrus_wait_for(struct cedrus_dev *dev, u32 reg, u32 flag)
{
	u32 value;

	return readl_poll_timeout_atomic(dev->base + reg, value,
			(value & flag) == 0, 10, 1000);
}

static inline dma_addr_t cedrus_buf_addr(struct vb2_buffer *buf,
					 struct v4l2_pix_format *pix_fmt,
					 unsigned int plane)
{
	dma_addr_t addr = vb2_dma_contig_plane_dma_addr(buf, 0);

	return addr + (pix_fmt ? (dma_addr_t)pix_fmt->bytesperline *
	       pix_fmt->height * plane : 0);
}

static inline dma_addr_t cedrus_dst_buf_addr(struct cedrus_ctx *ctx,
					     struct vb2_buffer *buf,
					     unsigned int plane)
{
	return buf ? cedrus_buf_addr(buf, &ctx->dst_fmt, plane) : 0;
}

/*
 * Size of the low-2-bit plane the hardware appends after the 8-bit NV12
 * planes for 10-bit reconstruction.  Shared by all codecs.
 */
static inline u32 cedrus_dst_2bit_size(unsigned int width, unsigned int height)
{
	return ALIGN(width / 4, 32) * height * 3 / 2;
}

static inline bool cedrus_dst_is_p010(struct cedrus_ctx *ctx)
{
	return ctx->dst_fmt.pixelformat == V4L2_PIX_FMT_P010;
}

static inline bool cedrus_is_afbc_format(u32 format)
{
	switch (format) {
	case V4L2_PIX_FMT_YUV420_8_AFBC_16X16_SPLIT:
	case V4L2_PIX_FMT_YUV420_10_AFBC_16X16_SPLIT:
		return true;
	default:
		return false;
	}
}

static inline bool cedrus_dst_is_afbc(struct cedrus_ctx *ctx)
{
	return cedrus_is_afbc_format(ctx->dst_fmt.pixelformat);
}

/* Size of the userspace-visible P010 surface at the buffer start. */
static inline u32 cedrus_dst_p010_size(struct cedrus_ctx *ctx)
{
	return ctx->dst_fmt.bytesperline * ctx->dst_fmt.height * 3 / 2;
}

/*
 * Format describing the 8-bit NV12 reconstruction/reference surface.
 * For P010 output this is a hidden surface (NV12 planes plus an appended
 * 2-bit plane) that follows the visible P010 data; otherwise it is the
 * capture format itself.
 */
static inline struct v4l2_pix_format cedrus_dst_frame_fmt(struct cedrus_ctx *ctx)
{
	struct v4l2_pix_format fmt = ctx->dst_fmt;

	if (cedrus_dst_is_p010(ctx)) {
		fmt.pixelformat = V4L2_PIX_FMT_NV12;
		fmt.bytesperline = ALIGN(fmt.width, 16);
	}

	return fmt;
}

/*
 * Address of a plane of the reconstruction/reference surface.  Identical
 * to cedrus_dst_buf_addr() for non-P010 output; for P010 it points past
 * the visible P010 data to the hidden NV12 surface.
 */
static inline dma_addr_t cedrus_dst_frame_addr(struct cedrus_ctx *ctx,
					       struct vb2_buffer *buf,
					       unsigned int plane)
{
	struct v4l2_pix_format fmt = cedrus_dst_frame_fmt(ctx);
	dma_addr_t addr;

	if (!buf)
		return 0;

	addr = vb2_dma_contig_plane_dma_addr(buf, 0);
	if (cedrus_dst_is_p010(ctx))
		addr += cedrus_dst_p010_size(ctx);

	return addr + (dma_addr_t)fmt.bytesperline * fmt.height * plane;
}

/*
 * Extra capture-buffer space needed for 10-bit decoding beyond the
 * visible plane: the appended low-2-bit reconstruction plane, plus (for
 * P010) the hidden 8-bit NV12 reconstruction surface.  Shared by the
 * VP9 and H.265 decoders as the extra_cap_size op.
 */
static inline unsigned int
cedrus_dst_10bit_extra_size(struct cedrus_ctx *ctx,
			    struct v4l2_pix_format *pix_fmt)
{
	unsigned int extra;

	if (ctx->bit_depth <= 8)
		return 0;

	/*
	 * AFBC buffers are sized in full by cedrus_prepare_format(); the
	 * compressed body already carries the 10-bit samples, so no extra
	 * low-2-bit plane is appended.
	 */
	if (cedrus_is_afbc_format(pix_fmt->pixelformat))
		return 0;

	extra = cedrus_dst_2bit_size(pix_fmt->width, pix_fmt->height);

	if (pix_fmt->pixelformat == V4L2_PIX_FMT_P010)
		extra += ALIGN(pix_fmt->width, 16) * pix_fmt->height * 3 / 2;

	return extra;
}

static inline void cedrus_write_ref_buf_addr(struct cedrus_ctx *ctx,
					     struct vb2_queue *q,
					     u64 timestamp,
					     u32 luma_reg,
					     u32 chroma_reg)
{
	struct cedrus_dev *dev = ctx->dev;
	struct vb2_buffer *buf = vb2_find_buffer(q, timestamp);

	cedrus_write(dev, luma_reg, cedrus_dst_buf_addr(ctx, buf, 0));
	cedrus_write(dev, chroma_reg, cedrus_dst_buf_addr(ctx, buf, 1));
}

static inline struct cedrus_buffer *
vb2_v4l2_to_cedrus_buffer(const struct vb2_v4l2_buffer *p)
{
	return container_of(p, struct cedrus_buffer, m2m_buf.vb);
}

static inline struct cedrus_buffer *
vb2_to_cedrus_buffer(const struct vb2_buffer *p)
{
	return vb2_v4l2_to_cedrus_buffer(to_vb2_v4l2_buffer(p));
}

static inline bool
cedrus_is_capable(struct cedrus_ctx *ctx, unsigned int capabilities)
{
	return (ctx->dev->capabilities & capabilities) == capabilities;
}

void *cedrus_find_control_data(struct cedrus_ctx *ctx, u32 id);
u32 cedrus_get_num_of_controls(struct cedrus_ctx *ctx, u32 id);

#endif
