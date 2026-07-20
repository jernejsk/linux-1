/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Allwinner DI300 deinterlace driver
 *
 * Copyright (C) 2026 Jernej Skrabec <jernej.skrabec@siol.net>
 */

#ifndef _SUN50I_DI300_H_
#define _SUN50I_DI300_H_

#include <media/v4l2-device.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-dma-contig.h>

#include <linux/platform_device.h>

#define DEINTERLACE_NAME		"sun50i-di300"

#define DEINTERLACE_RESET			0x000
#define DEINTERLACE_RESET_EN				BIT(31)

#define DEINTERLACE_START			0x010
#define DEINTERLACE_START_EN				BIT(0)

#define DEINTERLACE_INT_CTL			0x014
#define DEINTERLACE_INT_CTL_FINISH_EN			BIT(0)

#define DEINTERLACE_STATUS			0x018
#define DEINTERLACE_STATUS_FINISH			BIT(0)
#define DEINTERLACE_STATUS_BUSY				BIT(8)

#define DEINTERLACE_FUNC_EN			0x020
#define DEINTERLACE_FUNC_EN_DIT			BIT(0)
#define DEINTERLACE_FUNC_EN_MD				BIT(1)

#define DEINTERLACE_DMA_CTL			0x024
#define DEINTERLACE_DMA_CTL_C				BIT(0)
#define DEINTERLACE_DMA_CTL_P				BIT(4)
#define DEINTERLACE_DMA_CTL_DI				BIT(8)
#define DEINTERLACE_DMA_CTL_FR				BIT(12)
#define DEINTERLACE_DMA_CTL_W0				BIT(16)
#define DEINTERLACE_DMA_CTL_W1				BIT(20)
#define DEINTERLACE_DMA_CTL_TNR				BIT(24)
#define DEINTERLACE_DMA_CTL_FW				BIT(28)
#define DEINTERLACE_DMA_CTL_MCLK_GATE			BIT(31)

#define DEINTERLACE_SIZE			0x030
#define DEINTERLACE_SIZE_WIDTH(w)			(((w) - 1) & 0x7ff)
#define DEINTERLACE_SIZE_HEIGHT(h)			((((h) - 1) & 0x7ff) << 16)

/*
 * format: 0 planar YUV420, 1 UV-combined YUV420,
 *         2 planar YUV422, 3 UV-combined YUV422
 */
#define DEINTERLACE_FMT				0x034
#define DEINTERLACE_FMT_IN0(v)				((v) & 3)
#define DEINTERLACE_FMT_IN1(v)				(((v) & 3) << 4)
#define DEINTERLACE_FMT_IN2(v)				(((v) & 3) << 8)
#define DEINTERLACE_FMT_DIT(v)				(((v) & 3) << 16)
#define DEINTERLACE_FMT_UVSEQ				BIT(28)

#define DEINTERLACE_FIELD_ORDER			0x038
#define DEINTERLACE_FIELD_ORDER_BFF			BIT(0)

/* pitch of in_f0/in_f1, packed as two 16-bit halves per plane */
#define DEINTERLACE_IN_F01_PITCH(p)		(0x040 + (p) * 4)
#define DEINTERLACE_IN_F01_PITCH_F0(v)			((v) & 0xffff)
#define DEINTERLACE_IN_F01_PITCH_F1(v)			(((v) & 0xffff) << 16)

/* pitch of in_f2 */
#define DEINTERLACE_IN_F2_PITCH(p)		(0x050 + (p) * 4)

/* pitch of dit output */
#define DEINTERLACE_OUT_DIT_PITCH(p)		(0x070 + (p) * 4)

#define DEINTERLACE_FLAG_PITCH			0x080

#define DEINTERLACE_IN_F0_TOP_ADDR(p)		(0x090 + (p) * 4)
#define DEINTERLACE_IN_F0_TOP_HADDR		0x09c
#define DEINTERLACE_IN_F0_BOT_ADDR(p)		(0x0a0 + (p) * 4)
#define DEINTERLACE_IN_F0_BOT_HADDR		0x0ac

#define DEINTERLACE_IN_F1_TOP_ADDR(p)		(0x0b0 + (p) * 4)
#define DEINTERLACE_IN_F1_TOP_HADDR		0x0bc
#define DEINTERLACE_IN_F1_BOT_ADDR(p)		(0x0c0 + (p) * 4)
#define DEINTERLACE_IN_F1_BOT_HADDR		0x0cc

#define DEINTERLACE_IN_F2_TOP_ADDR(p)		(0x0d0 + (p) * 4)
#define DEINTERLACE_IN_F2_TOP_HADDR		0x0dc
#define DEINTERLACE_IN_F2_BOT_ADDR(p)		(0x0e0 + (p) * 4)
#define DEINTERLACE_IN_F2_BOT_HADDR		0x0ec

#define DEINTERLACE_OUT_DIT0_ADDR(p)		(0x100 + (p) * 4)
#define DEINTERLACE_OUT_DIT0_HADDR		0x10c

#define DEINTERLACE_OUT_DIT1_ADDR(p)		(0x110 + (p) * 4)
#define DEINTERLACE_OUT_DIT1_HADDR		0x11c

#define DEINTERLACE_IN_FLAG_ADDR		0x120
#define DEINTERLACE_OUT_FLAG_ADDR		0x124
#define DEINTERLACE_FLAG_HADDR			0x128

/* fixed tuning defaults, verbatim from vendor's di_dev_apply_fixed_para() */
#define DEINTERLACE_MD_PARA			0x180
#define DEINTERLACE_MD_PARA_DEFAULT			0x21360c04

#define DEINTERLACE_MD_CROP_H			0x190
#define DEINTERLACE_MD_CROP_V			0x194

#define DEINTERLACE_DIT_SETTING			0x1a0
#define DEINTERLACE_DIT_SETTING_MODE_LUMA		BIT(0)
#define DEINTERLACE_DIT_SETTING_MOTION_BLEND_LUMA	BIT(4)
#define DEINTERLACE_DIT_SETTING_DIAG_INTP_EN		BIT(5)
#define DEINTERLACE_DIT_SETTING_MODE_CHROMA		BIT(16)
#define DEINTERLACE_DIT_SETTING_MOTION_BLEND_CHROMA	BIT(20)
#define DEINTERLACE_DIT_SETTING_OUTPUT_1FRAME		BIT(24)

#define DEINTERLACE_DIT_CHR_PARA0		0x1a4
#define DEINTERLACE_DIT_CHR_PARA0_DEFAULT		0x30058000

#define DEINTERLACE_DIT_CHR_PARA1		0x1a8
#define DEINTERLACE_DIT_CHR_PARA1_DEFAULT		0x04300000

#define DEINTERLACE_DIT_INTRA_PARA		0x1b0
#define DEINTERLACE_DIT_INTRA_PARA_DEFAULT		0x514240ac

#define DEINTERLACE_DIT_INTER_PARA		0x1b4
#define DEINTERLACE_DIT_INTER_PARA_DEFAULT		0x22000000

#define DEINTERLACE_DIT_CROP_H			0x1c0
#define DEINTERLACE_DIT_CROP_V			0x1c4
#define DEINTERLACE_DIT_DEMO_H			0x1c8
#define DEINTERLACE_DIT_DEMO_V			0x1cc

#define DEINTERLACE_MIN_WIDTH	2U
#define DEINTERLACE_MIN_HEIGHT	2U
#define DEINTERLACE_MAX_WIDTH	2048U
#define DEINTERLACE_MAX_HEIGHT	1200U

#define DEINTERLACE_FLAG_SIZE	(DEINTERLACE_MAX_WIDTH * DEINTERLACE_MAX_HEIGHT / 4)

struct deinterlace_ctx {
	struct v4l2_fh		fh;
	struct deinterlace_dev	*dev;

	struct v4l2_pix_format	src_fmt;
	struct v4l2_pix_format	dst_fmt;

	void			*flag1_buf;
	dma_addr_t		flag1_buf_dma;

	void			*flag2_buf;
	dma_addr_t		flag2_buf_dma;

	/* prev[0]: one frame back, prev[1]: two frames back */
	struct vb2_v4l2_buffer	*prev[2];

	unsigned int		first_field;

	int			aborting;
};

struct deinterlace_dev {
	struct v4l2_device	v4l2_dev;
	struct video_device	vfd;
	struct device		*dev;
	struct v4l2_m2m_dev	*m2m_dev;

	/* Device file mutex */
	struct mutex		dev_mutex;

	void __iomem		*base;

	struct clk		*bus_clk;
	struct clk		*mod_clk;

	struct reset_control	*rstc;
};

#endif
