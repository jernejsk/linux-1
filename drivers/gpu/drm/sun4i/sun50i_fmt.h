/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2026 Jernej Skrabec <jernej.skrabec@gmail.com>
 */

#ifndef _SUN50I_FMT_H_
#define _SUN50I_FMT_H_

#include "sun8i_mixer.h"

#define SUN50I_FMT_DE3		0xa8000
#define SUN50I_FMT_DE33		0x5000

#define SUN50I_FMT_CTRL		0x00
#define SUN50I_FMT_SIZE		0x04
#define SUN50I_FMT_SWAP		0x08
#define SUN50I_FMT_DEPTH	0x0c
#define SUN50I_FMT_FORMAT	0x10
#define SUN50I_FMT_COEF		0x14
#define SUN50I_FMT_LMT_Y	0x20
#define SUN50I_FMT_LMT_C0	0x24
#define SUN50I_FMT_LMT_C1	0x28

#define SUN50I_FMT_CS_YUV444RGB	0
#define SUN50I_FMT_CS_YUV422	1
#define SUN50I_FMT_CS_YUV420	2

/*
 * Clamp values for the RGB/YUV444 bypass case and the YUV422/YUV420
 * subsampling case, taken verbatim from the vendor driver. They are
 * the same for 8-bit and 10-bit output, so the formatter clearly
 * clamps in some fixed internal precision rather than the output
 * width.
 */
#define SUN50I_FMT_LIMIT_BYPASS	0x0fff0000
#define SUN50I_FMT_LIMIT_Y	0x0eb00100
#define SUN50I_FMT_LIMIT_C	0x0f000100

int sun50i_fmt_init(struct sun8i_mixer *mixer);
void sun50i_fmt_setup(struct sun8i_mixer *mixer, u16 width,
		      u16 height, u32 format);

#endif /* _SUN50I_FMT_H_ */
