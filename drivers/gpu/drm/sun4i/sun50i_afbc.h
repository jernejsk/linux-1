/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) Jernej Skrabec <jernej.skrabec@gmail.com> */

#ifndef _SUN50I_AFBC_H_
#define _SUN50I_AFBC_H_

#include <linux/bits.h>
#include <linux/types.h>

#define SUN50I_AFBC_RGBA_8888		0x02
#define SUN50I_AFBC_RGB_888		0x08
#define SUN50I_AFBC_RGB_565		0x0a
#define SUN50I_AFBC_RGBA_4444		0x0e
#define SUN50I_AFBC_RGBA_5551		0x12
#define SUN50I_AFBC_RGBA1010102		0x16
#define SUN50I_AFBC_YUV422		0x26
#define SUN50I_AFBC_YUV420		0x2a
#define SUN50I_AFBC_P010		0x30
#define SUN50I_AFBC_P210		0x32

#define SUN50I_FBD_CTL			0x00
#define SUN50I_FBD_CTL_GLB_ALPHA(v)	((v) << 24)
#define SUN50I_FBD_CTL_CLK_GATE		BIT(4)
#define SUN50I_FBD_CTL_ALPHA_MODE_PIXEL	(0 << 2)
#define SUN50I_FBD_CTL_ALPHA_MODE_LAYER	BIT(2)
#define SUN50I_FBD_CTL_ALPHA_MODE_COMBINED	(2 << 2)
#define SUN50I_FBD_CTL_FBD_EN		BIT(0)

#define SUN50I_FBD_FMT_SEQ		0x04
#define SUN50I_FBD_FMT_SEQ_CANONICAL	0x3210

#define SUN50I_FBD_SIZE			0x08
#define SUN50I_FBD_SIZE_HEIGHT(v)	(((v) - 1) << 16)
#define SUN50I_FBD_SIZE_WIDTH(v)	(((v) - 1) << 0)

#define SUN50I_FBD_BLK_SIZE		0x0c
#define SUN50I_FBD_BLK_SIZE_HEIGHT(v)	((v) << 16)
#define SUN50I_FBD_BLK_SIZE_WIDTH(v)	((v) << 0)

#define SUN50I_FBD_SRC_CROP		0x10
#define SUN50I_FBD_SRC_CROP_TOP(v)	((v) << 16)
#define SUN50I_FBD_SRC_CROP_LEFT(v)	((v) << 0)

#define SUN50I_FBD_LAY_CROP		0x14
#define SUN50I_FBD_LAY_CROP_TOP(v)	((v) << 16)
#define SUN50I_FBD_LAY_CROP_LEFT(v)	((v) << 0)

#define SUN50I_FBD_FMT			0x18
#define SUN50I_FBD_FMT_SBS1(v)		((v) << 18)
#define SUN50I_FBD_FMT_SBS0(v)		((v) << 16)
#define SUN50I_FBD_FMT_YUV_TRAN		BIT(7)
#define SUN50I_FBD_FMT_IN_FMT(v)	((v) << 0)

#define SUN50I_FBD_LADDR		0x20
#define SUN50I_FBD_HADDR		0x24

#define SUN50I_FBD_OVL_SIZE		0x30
#define SUN50I_FBD_OVL_SIZE_HEIGHT(v)	(((v) - 1) << 16)
#define SUN50I_FBD_OVL_SIZE_WIDTH(v)	(((v) - 1) << 0)

#define SUN50I_FBD_OVL_COOR		0x34
#define SUN50I_FBD_OVL_COOR_Y(v)	((v) << 16)
#define SUN50I_FBD_OVL_COOR_X(v)	((v) << 0)

#define SUN50I_FBD_OVL_BG_COLOR		0x38

#define SUN50I_FBD_DEFAULT_COLOR0	0x50
#define SUN50I_FBD_DEFAULT_COLOR0_ALPHA(v) ((v) << 16)
#define SUN50I_FBD_DEFAULT_COLOR0_YR(v)	((v) << 0)

#define SUN50I_FBD_DEFAULT_COLOR1	0x54
#define SUN50I_FBD_DEFAULT_COLOR1_UG(v)	((v) << 16)
#define SUN50I_FBD_DEFAULT_COLOR1_VB(v)	((v) << 0)

struct drm_plane;
struct sun8i_layer;
struct sun8i_rdma;

extern const u64 sun50i_afbc_modifiers[];

bool sun50i_afbc_format_mod_supported(struct sun8i_layer *layer,
				      u32 format, u64 modifier);
int sun50i_afbc_init(struct sun8i_layer *layer, struct sun8i_rdma *rdma,
		     void __iomem *reg_base);
void sun50i_afbc_atomic_update(struct sun8i_layer *layer,
			       struct drm_plane *plane);
void sun50i_afbc_disable(struct sun8i_layer *layer);

#endif
