/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) Jernej Skrabec <jernej.skrabec@gmail.com>
 */

#ifndef _SUN50I_FMT_H_
#define _SUN50I_FMT_H_

#include "sun8i_mixer.h"

#define SUN50I_FMT_DE3 0xa8000
#define SUN50I_FMT_DE33 0x5000

#define SUN50I_FMT_CTRL(base)   ((base) + 0x00)
#define SUN50I_FMT_SIZE(base)   ((base) + 0x04)
#define SUN50I_FMT_SWAP(base)   ((base) + 0x08)
#define SUN50I_FMT_DEPTH(base)  ((base) + 0x0c)
#define SUN50I_FMT_FORMAT(base) ((base) + 0x10)
#define SUN50I_FMT_COEF(base)   ((base) + 0x14)
#define SUN50I_FMT_LMT_Y(base)  ((base) + 0x20)
#define SUN50I_FMT_LMT_C0(base) ((base) + 0x24)
#define SUN50I_FMT_LMT_C1(base) ((base) + 0x28)

#define SUN50I_FMT_LIMIT(low, high) (((high) << 16) | (low))

#define SUN50I_FMT_CS_YUV444RGB 0
#define SUN50I_FMT_CS_YUV422    1
#define SUN50I_FMT_CS_YUV420    2

void sun50i_fmt_setup(struct sun8i_mixer *mixer, u16 width,
		      u16 height, u32 format);

#endif
