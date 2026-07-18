/*
 * Copyright (C) 2017 Jernej Skrabec <jernej.skrabec@siol.net>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.  This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#ifndef _SUN8I_VI_SCALER_H_
#define _SUN8I_VI_SCALER_H_

#include <drm/drm_fourcc.h>
#include "sun8i_mixer.h"

#define DE2_VI_SCALER_UNIT_BASE 0x20000
#define DE2_VI_SCALER_UNIT_SIZE 0x20000

#define DE3_VI_SCALER_UNIT_BASE 0x20000
#define DE3_VI_SCALER_UNIT_SIZE 0x08000

#define DE33_VI_SCALER_UNIT_BASE 0x4000

/* this two macros assumes 16 fractional bits which is standard in DRM */
#define SUN8I_VI_SCALER_SCALE_MIN		1
#define SUN8I_VI_SCALER_SCALE_MAX		((1UL << 20) - 1)

#define SUN8I_VI_SCALER_SCALE_FRAC		20
#define SUN8I_VI_SCALER_SCALE_UNIT		BIT(SUN8I_VI_SCALER_SCALE_FRAC)
#define SUN8I_VI_SCALER_PHASE_FRAC		20
#define SUN8I_VI_SCALER_COEFF_COUNT		32
#define SUN8I_VI_SCALER_SIZE(w, h)		(((h) - 1) << 16 | ((w) - 1))

#define SUN8I_SCALER_VSU_CTRL			0x0
#define SUN50I_SCALER_VSU_SCALE_MODE		0x10
#define SUN50I_SCALER_VSU_DIR_THR		0x20
#define SUN50I_SCALER_VSU_EDGE_THR		0x24
#define SUN50I_SCALER_VSU_EDSCL_CTRL		0x28
#define SUN50I_SCALER_VSU_ANGLE_THR		0x2c
#define SUN8I_SCALER_VSU_OUTSIZE		0x40
#define SUN50I_SCALER_VSU_GLB_ALPHA		0x44
#define SUN8I_SCALER_VSU_YINSIZE		0x80
#define SUN8I_SCALER_VSU_YHSTEP			0x88
#define SUN8I_SCALER_VSU_YVSTEP			0x8c
#define SUN8I_SCALER_VSU_YHPHASE		0x90
#define SUN8I_SCALER_VSU_YVPHASE		0x98
#define SUN8I_SCALER_VSU_CINSIZE		0xc0
#define SUN8I_SCALER_VSU_CHSTEP			0xc8
#define SUN8I_SCALER_VSU_CVSTEP			0xcc
#define SUN8I_SCALER_VSU_CHPHASE		0xd0
#define SUN8I_SCALER_VSU_CVPHASE		0xd8
#define SUN8I_SCALER_VSU_YHCOEFF0(i)		(0x200 + 0x4 * (i))
#define SUN8I_SCALER_VSU_YHCOEFF1(i)		(0x300 + 0x4 * (i))
#define SUN8I_SCALER_VSU_YVCOEFF(i)		(0x400 + 0x4 * (i))
#define SUN8I_SCALER_VSU_CHCOEFF0(i)		(0x600 + 0x4 * (i))
#define SUN8I_SCALER_VSU_CHCOEFF1(i)		(0x700 + 0x4 * (i))
#define SUN8I_SCALER_VSU_CVCOEFF(i)		(0x800 + 0x4 * (i))

#define SUN8I_SCALER_VSU_CTRL_EN		BIT(0)
#define SUN8I_SCALER_VSU_CTRL_COEFF_RDY		BIT(4)

#define SUN50I_SCALER_VSU_SUB_ZERO_DIR_THR(x)	(((x) << 24) & 0xFF)
#define SUN50I_SCALER_VSU_ZERO_DIR_THR(x)		(((x) << 16) & 0xFF)
#define SUN50I_SCALER_VSU_HORZ_DIR_THR(x)		(((x) << 8) & 0xFF)
#define SUN50I_SCALER_VSU_VERT_DIR_THR(x)		((x) & 0xFF)

#define SUN50I_SCALER_VSU_SCALE_MODE_UI		0
#define SUN50I_SCALER_VSU_SCALE_MODE_NORMAL	1
#define SUN50I_SCALER_VSU_SCALE_MODE_ED_SCALE	2

#define SUN50I_SCALER_VSU_EDGE_SHIFT(x)		(((x) << 16) & 0xF)
#define SUN50I_SCALER_VSU_EDGE_OFFSET(x)		((x) & 0xFF)

#define SUN50I_SCALER_VSU_ANGLE_SHIFT(x)		(((x) << 16) & 0xF)
#define SUN50I_SCALER_VSU_ANGLE_OFFSET(x)		((x) & 0xFF)

void sun8i_vi_scaler_enable(struct sun8i_layer *layer, bool enable);
int sun8i_vi_scaler_init(struct sun8i_layer *layer, struct sun8i_rdma *rdma,
			 void __iomem *reg_base, u32 reg_offset_base);
void sun8i_vi_scaler_setup(struct sun8i_layer *layer,
			   u32 src_w, u32 src_h, u32 dst_w, u32 dst_h,
			   u32 hscale, u32 vscale, u32 hphase, u32 vphase,
			   const struct drm_format_info *format, bool afbc);

#endif
