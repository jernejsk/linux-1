// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Allwinner DE3 real-time write-back (RT-WB) as a DRM writeback connector.
 *
 * The RT-WB engine captures the blender output of a mixer (pre-TCON),
 * optionally converts or scales it, and writes it to memory.
 *
 * Copyright (C) 2026 Jernej Skrabec <jernej.skrabec@gmail.com>
 */

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/component.h>
#include <linux/dma-fence.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_of.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_writeback.h>

#include "sun4i_crtc.h"
#include "sunxi_engine.h"

#define WB_GCTRL		0x00
#define WB_GCTRL_CLK_GATE	BIT(29)
#define WB_GCTRL_AUTO_GATE	BIT(28)
#define WB_GCTRL_PORT(p)	((p) << 16)
#define WB_GCTRL_SOFT_RESET	BIT(4)
#define WB_GCTRL_START		BIT(0)

#define WB_SIZE			0x04
#define WB_CROP_COORD		0x08
#define WB_CROP_SIZE		0x0c
#define WB_A_CH0_ADDR		0x10
#define WB_A_CH1_ADDR		0x14
#define WB_A_CH2_ADDR		0x18
#define WB_A_HIGH_ADDR		0x1c
#define WB_CH0_PITCH		0x30
#define WB_CH12_PITCH		0x34
#define WB_ADDR_SWITCH		0x40
#define WB_FORMAT		0x44
#define WB_FORMAT_RGB888	0x0
#define WB_FORMAT_BGR888	0x1
#define WB_FORMAT_XRGB8888	0x4
#define WB_FORMAT_XBGR8888	0x5
#define WB_FORMAT_BGRX8888	0x6
#define WB_FORMAT_RGBX8888	0x7
#define WB_FORMAT_YUV420P	0x8
#define WB_FORMAT_NV12		0xc
#define WB_FORMAT_NV21		0xd
#define WB_FORMAT_10BIT		BIT(4)
#define WB_INT			0x48
#define WB_STATUS		0x4c
#define WB_STATUS_BUSY		BIT(8)
#define WB_STATUS_TIMEOUT	BIT(6)
#define WB_STATUS_OVERFLOW	BIT(5)
#define WB_STATUS_FINISH	BIT(4)
#define WB_STATUS_IRQ		BIT(0)
#define WB_STATUS_W1C		(WB_STATUS_TIMEOUT | WB_STATUS_OVERFLOW | \
				 WB_STATUS_FINISH | WB_STATUS_IRQ)
#define WB_SFTM			0x50
#define WB_BYPASS		0x54
#define WB_DE3_CSC_EN		BIT(0)
#define WB_COARSE_EN		BIT(1)
#define WB_FINE_EN		BIT(2)
#define WB_CS_HORZ		0x70
#define WB_CS_VERT		0x74
#define WB_FS_INSIZE		0x80
#define WB_FS_OUTSIZE		0x84
#define WB_FS_HSTEP		0x88
#define WB_FS_VSTEP		0x8c
#define WB_CSC_CTL		0x90
#define WB_CSC_D(n)		(0x94 + (n) * 4)
#define WB_CSC_COEFF(n)	(0xa0 + (n) * 4)
#define WB_START		0xe0
#define WB_YHCOEFF(n)		(0x200 + (n) * 4)
#define WB_CHCOEFF(n)		(0x280 + (n) * 4)

#define WB_MAX_INPUTS		2
#define WB_MAX_CRTCS		32
#define WB_MAX_WIDTH		4096
#define WB_MAX_HEIGHT		4096
#define WB_MIN_WIDTH		8
#define WB_MIN_HEIGHT		4
#define WB_YUV_MAX_WIDTH	2048

#define DE33_RTWB_MUX		0x20
#define DE33_RTWB_START		BIT(4)
#define DE33_WB_OFFSET		0x11000

#define WB_RCQ_STATUS		0x04
#define WB_RCQ_STATUS_FINISH	BIT(2)
#define WB_RCQ_STATUS_W1C	GENMASK(3, 2)
#define WB_RCQ_CTL		0x10

struct sun50i_wb_rcq_head {
	u32 low_addr;
	u32 len_hi;
	u32 dirty;
	u32 reg_offset;
};

struct sun50i_wb_cfg {
	const u32 *formats;
	unsigned int format_count;
	unsigned int max_output_width;
	bool has_rcq;
};

struct sun50i_wb {
	struct drm_writeback_connector	wb_conn;
	void __iomem			*regs;
	struct clk			*bus_clk;
	struct clk			*mod_clk;
	struct reset_control		*reset;
	struct device			*dev;
	struct device			*dma_dev;
	const struct sun50i_wb_cfg	*cfg;
	struct regmap			*top;
	void __iomem			*rcq;
	void				*shadow;
	dma_addr_t			shadow_dma;
	struct sun50i_wb_rcq_head	*heads;
	dma_addr_t			heads_dma;
	u8				crtc_port[WB_MAX_CRTCS];
	u32				rtwb_mux;
	struct completion		job_done;
};

static const u32 sun50i_wb_formats[] = {
	DRM_FORMAT_RGB888,
	DRM_FORMAT_BGR888,
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_RGBX8888,
	DRM_FORMAT_BGRX8888,
	DRM_FORMAT_XRGB2101010,
	DRM_FORMAT_YUV420,
	DRM_FORMAT_NV12,
	DRM_FORMAT_NV21,
};

static const u32 sun50i_h616_wb_formats[] = {
	DRM_FORMAT_RGB888,
	DRM_FORMAT_BGR888,
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_ABGR8888,
	DRM_FORMAT_RGBX8888,
	DRM_FORMAT_RGBA8888,
	DRM_FORMAT_BGRX8888,
	DRM_FORMAT_BGRA8888,
	DRM_FORMAT_XRGB2101010,
	DRM_FORMAT_AYUV,
	DRM_FORMAT_YUV420,
	DRM_FORMAT_NV12,
	DRM_FORMAT_NV21,
};

static const struct sun50i_wb_cfg sun50i_h6_wb_cfg = {
	.formats = sun50i_wb_formats,
	.format_count = ARRAY_SIZE(sun50i_wb_formats),
	.max_output_width = WB_MAX_WIDTH,
};

static const struct sun50i_wb_cfg sun50i_h616_wb_cfg = {
	.formats = sun50i_h616_wb_formats,
	.format_count = ARRAY_SIZE(sun50i_h616_wb_formats),
	.max_output_width = WB_YUV_MAX_WIDTH,
	.has_rcq = true,
};

static const u32 sun50i_wb_coeff_up[] = {
	0x00004000, 0x00033ffe, 0x00063efc, 0x000a3bfb,
	0xff0f37fb, 0xfe1433fb, 0xfd192ffb, 0xfd1f29fb,
	0xfc2424fc, 0xfb291ffd, 0xfb2f19fd, 0xfb3314fe,
	0xfb370fff, 0xfb3b0a00, 0xfc3e0600, 0xfe3f0300,
};

static const u32 sun50i_wb_coeff_down[] = {
	0x000e240e, 0x0010240c, 0x0013230a, 0x00142309,
	0x00162208, 0x01182106, 0x011a2005, 0x021b1f04,
	0x031d1d03, 0x041e1c02, 0x05201a01, 0x06211801,
	0x07221601, 0x09231400, 0x0a231300, 0x0c231100,
};

/* Full-range RGB to limited-range YCbCr, using DE33's 17-bit factors. */
static const u32 sun50i_wb_rgb2yuv[2][12] = {
	[DRM_COLOR_YCBCR_BT601] = {
		0x0000837a, 0x0001021d, 0x00003221, 0x00000040,
		0xffffb41c, 0xffff6b03, 0x0000e0e1, 0x00000200,
		0x0000e0e1, 0xffff43b1, 0xffffdb6e, 0x00000200,
	},
	[DRM_COLOR_YCBCR_BT709] = {
		0x00005d7c, 0x00013a7c, 0x00001fbf, 0x00000040,
		0xffffcc78, 0xffff52a7, 0x0000e0e1, 0x00000200,
		0x0000e0e1, 0xffff33be, 0xffffeb61, 0x00000200,
	},
};

static struct sun50i_wb *conn_to_wb(struct drm_connector *conn)
{
	struct drm_writeback_connector *wb_conn =
		container_of(conn, struct drm_writeback_connector, base);

	return container_of(wb_conn, struct sun50i_wb, wb_conn);
}

static struct sun50i_wb *encoder_to_wb(struct drm_encoder *encoder)
{
	struct drm_writeback_connector *wb_conn;

	wb_conn = container_of(encoder, struct drm_writeback_connector, encoder);
	return container_of(wb_conn, struct sun50i_wb, wb_conn);
}

static void sun50i_wb_write(struct sun50i_wb *wb, u32 reg, u32 value)
{
	if (wb->cfg->has_rcq)
		*(u32 *)(wb->shadow + reg) = value;
	else
		writel(value, wb->regs + reg);
}

static void sun50i_wb_submit_rcq(struct sun50i_wb *wb)
{
	unsigned int i;

	writel(0, wb->rcq + WB_RCQ_CTL);
	writel(WB_RCQ_STATUS_W1C, wb->rcq + WB_RCQ_STATUS);
	for (i = 0; i < 4; i++)
		wb->heads[i].dirty = 1;
	dma_wmb();
	writel(1, wb->rcq + WB_RCQ_CTL);
}

static int sun50i_wb_init_rcq(struct sun50i_wb *wb)
{
	static const struct {
		u32 offset;
		u32 size;
		u32 reg_offset;
	} blocks[] = {
		{ 0x000, 0x0d0, DE33_WB_OFFSET + 0x000 },
		{ 0x200, 0x040, DE33_WB_OFFSET + 0x200 },
		{ 0x280, 0x040, DE33_WB_OFFSET + 0x280 },
		/* The start value is stored at 0xe0 but targets GCTRL. */
		{ 0x0e0, 0x004, DE33_WB_OFFSET + 0x000 },
	};
	unsigned int i;

	wb->shadow = dmam_alloc_coherent(wb->dma_dev, 0x300, &wb->shadow_dma,
					 GFP_KERNEL);
	if (!wb->shadow)
		return -ENOMEM;

	wb->heads = dmam_alloc_coherent(wb->dma_dev, sizeof(*wb->heads) * 4,
					&wb->heads_dma, GFP_KERNEL);
	if (!wb->heads)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(blocks); i++) {
		dma_addr_t addr = wb->shadow_dma + blocks[i].offset;

		wb->heads[i].low_addr = lower_32_bits(addr);
		wb->heads[i].len_hi = blocks[i].size |
					 (upper_32_bits(addr) << 24);
		wb->heads[i].reg_offset = blocks[i].reg_offset;
	}

	writel(lower_32_bits(wb->heads_dma), wb->rcq + 0x14);
	writel(upper_32_bits(wb->heads_dma), wb->rcq + 0x18);
	writel(sizeof(*wb->heads) * 4, wb->rcq + 0x1c);

	return 0;
}

static u32 sun50i_wb_format(u32 format)
{
	switch (format) {
	case DRM_FORMAT_RGB888:
		return WB_FORMAT_RGB888;
	case DRM_FORMAT_BGR888:
		return WB_FORMAT_BGR888;
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
		return WB_FORMAT_XRGB8888;
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
		return WB_FORMAT_XBGR8888;
	case DRM_FORMAT_BGRX8888:
	case DRM_FORMAT_BGRA8888:
		return WB_FORMAT_BGRX8888;
	case DRM_FORMAT_RGBX8888:
	case DRM_FORMAT_RGBA8888:
		return WB_FORMAT_RGBX8888;
	case DRM_FORMAT_XRGB2101010:
		return WB_FORMAT_10BIT;
	case DRM_FORMAT_AYUV:
		return WB_FORMAT_XRGB8888;
	case DRM_FORMAT_YUV420:
		return WB_FORMAT_YUV420P;
	case DRM_FORMAT_NV12:
		return WB_FORMAT_NV12;
	case DRM_FORMAT_NV21:
		return WB_FORMAT_NV21;
	default:
		return U32_MAX;
	}
}

static void sun50i_wb_set_scaler(struct sun50i_wb *wb, u32 in_w, u32 in_h,
				 u32 out_w, u32 out_h,
				 const struct drm_format_info *format)
{
	bool subsampled = format->hsub > 1 || format->vsub > 1;
	unsigned int i;
	const u32 *c_coeff;
	const u32 *y_coeff;
	u32 control = format->is_yuv && !wb->cfg->has_rcq ?
		      WB_DE3_CSC_EN : 0;
	u32 step_h;
	u32 step_v;
	u32 mid_h = in_h;
	u32 mid_w = in_w;

	if (in_w > 2 * out_w) {
		sun50i_wb_write(wb, WB_CS_HORZ, in_w | (out_w << 17));
		mid_w = 2 * out_w;
		control |= WB_COARSE_EN;
	} else {
		sun50i_wb_write(wb, WB_CS_HORZ, 0);
	}

	if (in_h > 2 * out_h) {
		sun50i_wb_write(wb, WB_CS_VERT, in_h | (out_h << 17));
		mid_h = 2 * out_h;
		control |= WB_COARSE_EN;
	} else {
		sun50i_wb_write(wb, WB_CS_VERT, 0);
	}

	if (mid_w != out_w || mid_h != out_h || subsampled) {
		control |= WB_FINE_EN;
		step_h = div_u64((u64)mid_w << 18, out_w) << 2;
		step_v = div_u64((u64)mid_h << 18, out_h) << 2;
		y_coeff = (mid_w > out_w || mid_h > out_h) ?
			sun50i_wb_coeff_down : sun50i_wb_coeff_up;
		c_coeff = (mid_w > out_w / format->hsub ||
			   mid_h > out_h / format->vsub) ?
			sun50i_wb_coeff_down : sun50i_wb_coeff_up;
		for (i = 0; i < ARRAY_SIZE(sun50i_wb_coeff_up); i++) {
			sun50i_wb_write(wb, WB_YHCOEFF(i), y_coeff[i]);
			sun50i_wb_write(wb, WB_CHCOEFF(i), c_coeff[i]);
		}
	} else {
		step_h = 1 << 20;
		step_v = 1 << 20;
	}

	sun50i_wb_write(wb, WB_FS_INSIZE,
			(mid_h - 1) << 16 | (mid_w - 1));
	sun50i_wb_write(wb, WB_FS_OUTSIZE,
			(out_h - 1) << 16 | (out_w - 1));
	sun50i_wb_write(wb, WB_FS_HSTEP, step_h);
	sun50i_wb_write(wb, WB_FS_VSTEP, step_v);
	sun50i_wb_write(wb, WB_BYPASS, control);
}

static void sun50i_wb_set_csc(struct sun50i_wb *wb,
			      const struct drm_format_info *format,
			      u32 width, u32 height)
{
	enum drm_color_encoding encoding;
	const u32 *coeff;
	unsigned int i;

	if (!wb->cfg->has_rcq)
		return;

	if (!format->is_yuv) {
		sun50i_wb_write(wb, WB_CSC_CTL, 0);
		return;
	}

	encoding = width <= 736 && height <= 576 ?
		   DRM_COLOR_YCBCR_BT601 : DRM_COLOR_YCBCR_BT709;
	coeff = sun50i_wb_rgb2yuv[encoding];

	sun50i_wb_write(wb, WB_CSC_CTL, 1);
	for (i = 0; i < 3; i++)
		sun50i_wb_write(wb, WB_CSC_D(i), 0);
	for (i = 0; i < 12; i++)
		sun50i_wb_write(wb, WB_CSC_COEFF(i), coeff[i]);
}

static irqreturn_t sun50i_wb_irq(int irq, void *data)
{
	struct sun50i_wb *wb = data;
	u32 status = readl(wb->regs + WB_STATUS);
	int ret;

	if (!(status & WB_STATUS_IRQ))
		return IRQ_NONE;

	if (status & WB_STATUS_FINISH)
		ret = 0;
	else
		ret = -EIO;

	writel(WB_STATUS_W1C, wb->regs + WB_STATUS);
	if (wb->cfg->has_rcq)
		regmap_write(wb->top, DE33_RTWB_MUX, wb->rtwb_mux);
	writel(0, wb->regs + WB_INT);

	/*
	 * Stop and reset the engine, like the vendor driver does after
	 * every capture. Leaving it merely idle keeps it attached to the
	 * blender output and can stall the whole mixer pipeline.
	 */
	writel(WB_GCTRL_CLK_GATE | WB_GCTRL_SOFT_RESET, wb->regs + WB_GCTRL);

	drm_writeback_signal_completion(&wb->wb_conn, ret);
	complete(&wb->job_done);

	return IRQ_HANDLED;
}

static int sun50i_wb_encoder_atomic_check(struct drm_encoder *encoder,
					  struct drm_crtc_state *crtc_state,
					  struct drm_connector_state *conn_state)
{
	struct sun50i_wb *wb = encoder_to_wb(encoder);
	struct drm_framebuffer *fb;

	if (!conn_state->writeback_job)
		return 0;

	fb = conn_state->writeback_job->fb;
	if (sun50i_wb_format(fb->format->format) == U32_MAX)
		return -EINVAL;

	if (crtc_state->mode.hdisplay < WB_MIN_WIDTH ||
	    crtc_state->mode.hdisplay > WB_MAX_WIDTH ||
	    crtc_state->mode.vdisplay < WB_MIN_HEIGHT ||
	    crtc_state->mode.vdisplay > WB_MAX_HEIGHT)
		return -EINVAL;

	if (fb->width < WB_MIN_WIDTH || fb->height < WB_MIN_HEIGHT ||
	    fb->width > crtc_state->mode.hdisplay ||
	    fb->height > crtc_state->mode.vdisplay ||
	    fb->width > wb->cfg->max_output_width)
		return -EINVAL;

	if (fb->format->is_yuv && fb->width > WB_YUV_MAX_WIDTH)
		return -EINVAL;

	if (fb->width % fb->format->hsub || fb->height % fb->format->vsub)
		return -EINVAL;

	if (fb->format->format == DRM_FORMAT_YUV420 &&
	    fb->pitches[1] != fb->pitches[2])
		return -EINVAL;

	if (wb->cfg->has_rcq &&
	    fb->format->format == DRM_FORMAT_XRGB2101010 &&
	    (fb->width != crtc_state->mode.hdisplay ||
	     fb->height != crtc_state->mode.vdisplay))
		return -EINVAL;

	return 0;
}

static const struct drm_encoder_helper_funcs sun50i_wb_encoder_helper_funcs = {
	.atomic_check	= sun50i_wb_encoder_atomic_check,
};

static void sun50i_wb_atomic_commit(struct drm_connector *conn,
				    struct drm_atomic_commit *state)
{
	struct drm_connector_state *conn_state =
		drm_atomic_get_new_connector_state(state, conn);
	struct drm_writeback_job *job = conn_state->writeback_job;
	struct drm_gem_dma_object *gem;
	struct drm_framebuffer *fb;
	struct sun50i_wb *wb = conn_to_wb(conn);
	struct drm_crtc *crtc = conn_state->crtc;
	dma_addr_t addr[3] = {};
	unsigned int i;
	u32 format;
	u32 gctrl;
	u32 out_h;
	u32 out_w;
	u32 high = 0;
	u32 port;
	u32 h;
	u32 w;

	if (!job || !job->fb || !crtc)
		return;

	if (!wait_for_completion_timeout(&wb->job_done, HZ)) {
		drm_err(conn->dev, "previous writeback job never completed\n");

		/* Quiesce the interrupt and consider the old job lost. */
		writel(0, wb->regs + WB_INT);
		writel(WB_STATUS_W1C, wb->regs + WB_STATUS);
		if (wb->cfg->has_rcq)
			regmap_write(wb->top, DE33_RTWB_MUX, wb->rtwb_mux);
		drm_writeback_signal_completion(&wb->wb_conn, -ETIMEDOUT);
	}
	reinit_completion(&wb->job_done);

	fb = job->fb;
	w = crtc->state->mode.hdisplay;
	h = crtc->state->mode.vdisplay;
	out_w = fb->width;
	out_h = fb->height;
	port = wb->crtc_port[drm_crtc_index(crtc)];
	format = sun50i_wb_format(fb->format->format);

	for (i = 0; i < fb->format->num_planes; i++) {
		gem = drm_fb_dma_get_gem_obj(fb, i);
		addr[i] = gem->dma_addr + fb->offsets[i];
		high |= (upper_32_bits(addr[i]) & 0xff) << (8 * i);
	}

	gctrl = WB_GCTRL_CLK_GATE | WB_GCTRL_AUTO_GATE;
	if (wb->cfg->has_rcq) {
		gctrl = WB_GCTRL_AUTO_GATE;
		wb->rtwb_mux = port * 2;
		regmap_write(wb->top, DE33_RTWB_MUX, wb->rtwb_mux);
	} else {
		gctrl |= WB_GCTRL_PORT(port);
	}
	sun50i_wb_write(wb, WB_GCTRL, gctrl);

	sun50i_wb_write(wb, WB_SIZE, (h - 1) << 16 | (w - 1));
	sun50i_wb_write(wb, WB_CROP_COORD, 0);
	sun50i_wb_write(wb, WB_CROP_SIZE, (h - 1) << 16 | (w - 1));

	sun50i_wb_write(wb, WB_A_CH0_ADDR, lower_32_bits(addr[0]));
	sun50i_wb_write(wb, WB_A_CH1_ADDR, lower_32_bits(addr[1]));
	sun50i_wb_write(wb, WB_A_CH2_ADDR, lower_32_bits(addr[2]));
	sun50i_wb_write(wb, WB_A_HIGH_ADDR, high);
	sun50i_wb_write(wb, WB_CH0_PITCH, fb->pitches[0]);
	sun50i_wb_write(wb, WB_CH12_PITCH,
			fb->format->num_planes > 1 ? fb->pitches[1] : 0);

	sun50i_wb_write(wb, WB_ADDR_SWITCH, 0); /* group A, no auto switch */
	sun50i_wb_write(wb, WB_FORMAT, format);
	sun50i_wb_write(wb, WB_INT, 1);
	if (wb->cfg->has_rcq)
		sun50i_wb_write(wb, WB_SFTM, 0x20);

	sun50i_wb_set_scaler(wb, w, h, out_w, out_h, fb->format);
	sun50i_wb_set_csc(wb, fb->format, out_w, out_h);

	writel(WB_STATUS_W1C, wb->regs + WB_STATUS);

	drm_writeback_queue_job(&wb->wb_conn, conn_state);

	/* capture starts with the next frame */
	if (wb->cfg->has_rcq) {
		struct sun4i_crtc *scrtc = drm_crtc_to_sun4i_crtc(crtc);
		u32 status;
		int ret;

		sun50i_wb_write(wb, WB_START,
				WB_GCTRL_AUTO_GATE | WB_GCTRL_START);

		/*
		 * Both the mixer and the writeback have their own register
		 * command queues, but they share one fetch engine.
		 * Triggering one queue while the other fetch is still in
		 * flight can corrupt either update, so serialize them.
		 */
		sunxi_engine_sync(scrtc->engine);
		sun50i_wb_submit_rcq(wb);
		ret = readl_poll_timeout(wb->rcq + WB_RCQ_STATUS, status,
					 status & WB_RCQ_STATUS_FINISH,
					 10, 20000);
		if (ret)
			drm_warn(conn->dev, "writeback RCQ sync timed out\n");
		regmap_write(wb->top, DE33_RTWB_MUX,
			     wb->rtwb_mux | DE33_RTWB_START);
	} else {
		writel(gctrl | WB_GCTRL_START, wb->regs + WB_GCTRL);
	}
}

static int sun50i_wb_get_modes(struct drm_connector *connector)
{
	return drm_add_modes_noedid(connector, WB_MAX_WIDTH, WB_MAX_HEIGHT);
}

static const struct drm_connector_helper_funcs sun50i_wb_conn_helper_funcs = {
	.get_modes	= sun50i_wb_get_modes,
	.atomic_commit	= sun50i_wb_atomic_commit,
};

static const struct drm_connector_funcs sun50i_wb_conn_funcs = {
	.fill_modes		= drm_helper_probe_single_connector_modes,
	.destroy		= drm_connector_cleanup,
	.reset			= drm_atomic_helper_connector_reset,
	.atomic_duplicate_state	= drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_connector_destroy_state,
};

static void sun50i_wb_setup_clones(struct drm_device *drm,
				   struct drm_encoder *wb_encoder)
{
	struct drm_encoder *encoder;
	u32 wb_mask = drm_encoder_mask(wb_encoder);

	wb_encoder->possible_clones = wb_mask;

	drm_for_each_encoder(encoder, drm) {
		if (encoder == wb_encoder ||
		    !(encoder->possible_crtcs & wb_encoder->possible_crtcs))
			continue;

		encoder->possible_clones |= drm_encoder_mask(encoder) | wb_mask;
		wb_encoder->possible_clones |= drm_encoder_mask(encoder);
	}
}

static int sun50i_wb_bind(struct device *dev, struct device *master,
			  void *data)
{
	struct device_node *ep, *mixer, *port;
	struct of_endpoint endpoint;
	struct sun50i_wb *wb = dev_get_drvdata(dev);
	struct drm_crtc *crtc;
	struct drm_device *drm = data;
	u32 possible_crtcs;
	int ret;

	if (wb->cfg->has_rcq && !wb->shadow) {
		wb->dma_dev = drm_dev_dma_dev(drm);
		ret = sun50i_wb_init_rcq(wb);
		if (ret)
			return ret;
	}

	possible_crtcs = 0;
	memset(wb->crtc_port, U8_MAX, sizeof(wb->crtc_port));
	port = of_graph_get_port_by_id(wb->dev->of_node, 0);
	for_each_child_of_node(port, ep) {
		ret = of_graph_parse_endpoint(ep, &endpoint);
		if (ret || endpoint.id >= WB_MAX_INPUTS)
			continue;

		mixer = of_graph_get_remote_port_parent(ep);
		drm_for_each_crtc(crtc, drm) {
			struct sun4i_crtc *scrtc = drm_crtc_to_sun4i_crtc(crtc);

			if (scrtc->engine->node == mixer) {
				wb->crtc_port[drm_crtc_index(crtc)] = endpoint.id;
				possible_crtcs |= drm_crtc_mask(crtc);
			}
		}
		of_node_put(mixer);
	}
	of_node_put(port);

	if (!possible_crtcs) {
		dev_err(wb->dev, "no CRTC connected to writeback\n");
		return -ENODEV;
	}

	drm_connector_helper_add(&wb->wb_conn.base,
				 &sun50i_wb_conn_helper_funcs);

	ret = drm_writeback_connector_init(drm, &wb->wb_conn,
					   &sun50i_wb_conn_funcs,
					   &sun50i_wb_encoder_helper_funcs,
					   wb->cfg->formats,
					   wb->cfg->format_count,
					   possible_crtcs);
	if (ret)
		return ret;

	sun50i_wb_setup_clones(drm, &wb->wb_conn.encoder);

	return 0;
}

static const struct component_ops sun50i_wb_ops = {
	.bind	= sun50i_wb_bind,
};

static int sun50i_wb_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct platform_device *top_pdev;
	struct device_node *np;
	struct sun50i_wb *wb;
	int irq, ret;

	wb = devm_kzalloc(dev, sizeof(*wb), GFP_KERNEL);
	if (!wb)
		return -ENOMEM;

	wb->dev = dev;
	wb->cfg = of_device_get_match_data(dev);
	if (!wb->cfg)
		return -EINVAL;

	init_completion(&wb->job_done);
	complete(&wb->job_done);

	wb->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(wb->regs))
		return PTR_ERR(wb->regs);

	if (wb->cfg->has_rcq) {
		wb->rcq = devm_platform_ioremap_resource(pdev, 1);
		if (IS_ERR(wb->rcq))
			return PTR_ERR(wb->rcq);

		np = of_parse_phandle(dev->of_node, "allwinner,top", 0);
		if (!np)
			return -EINVAL;
		top_pdev = of_find_device_by_node(np);
		of_node_put(np);
		if (!top_pdev)
			return -EPROBE_DEFER;
		wb->top = dev_get_regmap(&top_pdev->dev, NULL);
		if (!wb->top) {
			platform_device_put(top_pdev);
			return -EPROBE_DEFER;
		}
		if (!device_link_add(dev, &top_pdev->dev,
				     DL_FLAG_AUTOREMOVE_CONSUMER)) {
			platform_device_put(top_pdev);
			return -EINVAL;
		}
		platform_device_put(top_pdev);
	}

	wb->bus_clk = devm_clk_get_enabled(dev, "bus");
	if (IS_ERR(wb->bus_clk))
		return PTR_ERR(wb->bus_clk);

	wb->mod_clk = devm_clk_get_enabled(dev, "mod");
	if (IS_ERR(wb->mod_clk))
		return PTR_ERR(wb->mod_clk);

	wb->reset = devm_reset_control_get(dev, NULL);
	if (IS_ERR(wb->reset))
		return PTR_ERR(wb->reset);

	ret = reset_control_deassert(wb->reset);
	if (ret)
		return ret;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		ret = irq;
		goto err_reset;
	}

	ret = devm_request_irq(dev, irq, sun50i_wb_irq, 0,
			       dev_name(dev), wb);
	if (ret)
		goto err_reset;

	dev_set_drvdata(dev, wb);

	ret = component_add(dev, &sun50i_wb_ops);
	if (ret)
		goto err_reset;

	return 0;

err_reset:
	reset_control_assert(wb->reset);
	return ret;
}

static void sun50i_wb_remove(struct platform_device *pdev)
{
	struct sun50i_wb *wb = platform_get_drvdata(pdev);

	component_del(&pdev->dev, &sun50i_wb_ops);
	reset_control_assert(wb->reset);
}

static const struct of_device_id sun50i_wb_of_table[] = {
	{ .compatible = "allwinner,sun50i-h6-de3-wb", .data = &sun50i_h6_wb_cfg },
	{ .compatible = "allwinner,sun50i-h616-de33-wb", .data = &sun50i_h616_wb_cfg },
	{ }
};
MODULE_DEVICE_TABLE(of, sun50i_wb_of_table);

struct platform_driver sun50i_wb_platform_driver = {
	.probe	= sun50i_wb_probe,
	.remove	= sun50i_wb_remove,
	.driver	= {
		.name		= "sun50i-de3-wb",
		.of_match_table	= sun50i_wb_of_table,
	},
};
module_platform_driver(sun50i_wb_platform_driver);

MODULE_AUTHOR("Jernej Skrabec <jernej.skrabec@gmail.com>");
MODULE_DESCRIPTION("Allwinner DE3 write-back connector");
MODULE_LICENSE("GPL");
