/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) Jernej Skrabec <jernej.skrabec@gmail.com>
 */

#ifndef _SUN50I_CDC_H_
#define _SUN50I_CDC_H_

#define SUN50i_CDC_CTRL(ch)        (0xD0000 + (ch) * 0x8000 + 0x00)
#define SUN50i_CDC_UPDATE(ch)      (0xD0000 + (ch) * 0x8000 + 0x04)
#define SUN50i_CDC_IN_CSC(ch)      (0xD0000 + (ch) * 0x8000 + 0x14)
#define SUN50i_CDC_OUT_CSC(ch)     (0xD0000 + (ch) * 0x8000 + 0x54)
#define SUN50i_CDC_LUT_COEF(ch, i) (0xD0000 + (ch) * 0x8000 + 0x1000 + (i) * 0xC00)

enum conversion_type {
	SDR_TO_WCG_RGB,
	SDR_TO_HDR_RGB,
	WCG_TO_SDR_YUV,
	HDR_TO_SDR_YUV,
};

struct sun8i_mixer;

void sun50i_cdc_setup(struct sun8i_mixer *mixer, unsigned int channel,
		      const u32 *in_csc, const u32 *out_csc,
		      enum conversion_type conv);
void sun50i_cdc_disable(struct sun8i_mixer *mixer, unsigned int channel);

#endif
