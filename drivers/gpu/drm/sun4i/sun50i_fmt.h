/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) Jernej Skrabec <jernej.skrabec@gmail.com>
 */

#ifndef _SUN50I_FMT_H_
#define _SUN50I_FMT_H_

#include "sun8i_mixer.h"

#define SUN50I_FMT_CTRL   0xa8000
#define SUN50I_FMT_SIZE   0xa8004
#define SUN50I_FMT_SWAP   0xa8008
#define SUN50I_FMT_DEPTH  0xa800c
#define SUN50I_FMT_FORMAT 0xa8010
#define SUN50I_FMT_COEF   0xa8014
#define SUN50I_FMT_LMT_Y  0xa8020
#define SUN50I_FMT_LMT_C0 0xa8024
#define SUN50I_FMT_LMT_C1 0xa8028

#define SUN50I_FMT_LIMIT(low, high) (((high) << 16) | (low))

#define SUN50I_FMT_CS_YUV444RGB 0
#define SUN50I_FMT_CS_YUV422    1
#define SUN50I_FMT_CS_YUV420    2

void sun50i_fmt_setup(struct sun8i_mixer *mixer, u16 width,
		      u16 height, u32 format);

#endif
