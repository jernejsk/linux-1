/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2015 Free Electrons
 * Copyright (C) 2015 NextThing Co
 *
 * Maxime Ripard <maxime.ripard@free-electrons.com>
 */

#ifndef _SUN4I_CRTC_H_
#define _SUN4I_CRTC_H_

struct sun4i_crtc {
	struct drm_crtc			crtc;
	struct drm_pending_vblank_event	*event;

	struct sunxi_engine		*engine;
	struct sun4i_tcon		*tcon;
};

/**
 * struct sun4i_crtc_state - CRTC state subclass
 * @base: base CRTC state
 * @format: negotiated output media bus format of the display engine
 * @encoding: negotiated output YCbCr encoding, meaningless when @format
 *	is RGB
 */
struct sun4i_crtc_state {
	struct drm_crtc_state		base;
	u32				format;
	enum drm_color_encoding		encoding;
};

static inline struct sun4i_crtc *drm_crtc_to_sun4i_crtc(struct drm_crtc *crtc)
{
	return container_of(crtc, struct sun4i_crtc, crtc);
}

static inline struct sun4i_crtc_state *
drm_crtc_state_to_sun4i_crtc_state(struct drm_crtc_state *state)
{
	return container_of(state, struct sun4i_crtc_state, base);
}

struct sun4i_crtc *sun4i_crtc_init(struct drm_device *drm,
				   struct sunxi_engine *engine,
				   struct sun4i_tcon *tcon);
void sun4i_crtc_finish_page_flip(struct drm_crtc *crtc);

#endif /* _SUN4I_CRTC_H_ */
