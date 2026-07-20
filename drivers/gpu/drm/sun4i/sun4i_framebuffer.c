// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2015 Free Electrons
 * Copyright (C) 2015 NextThing Co
 *
 * Maxime Ripard <maxime.ripard@free-electrons.com>
 */

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_blend.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_framebuffer_helper.h>

#include "sun4i_drv.h"
#include "sun4i_framebuffer.h"

static int sun4i_de_check_writebacks(struct drm_device *dev,
				     struct drm_atomic_commit *state)
{
	struct drm_connector_state *new_conn_state;
	struct drm_connector *connector;
	int ret, i;

	/*
	 * A writeback connector's atomic_check() runs during mode_fixup(),
	 * alongside the display connector sharing its CRTC. If the display
	 * connector negotiates a new output format in the same commit, the
	 * writeback connector may be checked before or after that happens,
	 * depending on iteration order. Re-run it now that mode_fixup() has
	 * finished and the CRTC state's format is guaranteed final.
	 */
	for_each_new_connector_in_state(state, connector, new_conn_state, i) {
		const struct drm_encoder_helper_funcs *funcs;
		struct drm_crtc_state *new_crtc_state;
		struct drm_encoder *encoder;

		if (connector->connector_type != DRM_MODE_CONNECTOR_WRITEBACK ||
		    !new_conn_state->crtc)
			continue;

		encoder = new_conn_state->best_encoder;
		if (!encoder)
			continue;

		funcs = encoder->helper_private;
		if (!funcs || !funcs->atomic_check)
			continue;

		new_crtc_state = drm_atomic_get_new_crtc_state(state,
							       new_conn_state->crtc);

		ret = funcs->atomic_check(encoder, new_crtc_state,
					  new_conn_state);
		if (ret)
			return ret;
	}

	return 0;
}

static int sun4i_de_atomic_check(struct drm_device *dev,
				 struct drm_atomic_commit *state)
{
	int ret;

	ret = drm_atomic_helper_check_modeset(dev, state);
	if (ret)
		return ret;

	ret = sun4i_de_check_writebacks(dev, state);
	if (ret)
		return ret;

	return drm_atomic_helper_check_planes(dev, state);
}

static const struct drm_framebuffer_funcs sun4i_fb_funcs = {
	.destroy	= drm_gem_fb_destroy,
	.create_handle	= drm_gem_fb_create_handle,
};

static struct drm_framebuffer *
sun4i_fb_create(struct drm_device *dev, struct drm_file *file,
		const struct drm_format_info *info,
		const struct drm_mode_fb_cmd2 *mode_cmd)
{
	struct drm_afbc_framebuffer *afbc_fb;
	int ret;

	if (!drm_is_afbc(mode_cmd->modifier[0]))
		return drm_gem_fb_create(dev, file, info, mode_cmd);

	afbc_fb = kzalloc_obj(*afbc_fb);
	if (!afbc_fb)
		return ERR_PTR(-ENOMEM);

	ret = drm_gem_fb_init_with_funcs(dev, &afbc_fb->base, file, info,
					 mode_cmd, &sun4i_fb_funcs);
	if (ret) {
		kfree(afbc_fb);
		return ERR_PTR(ret);
	}

	ret = drm_gem_fb_afbc_init(dev, info, mode_cmd, afbc_fb);
	if (ret) {
		drm_framebuffer_put(&afbc_fb->base);
		return ERR_PTR(ret);
	}

	return &afbc_fb->base;
}

static const struct drm_mode_config_funcs sun4i_de_mode_config_funcs = {
	.atomic_check		= sun4i_de_atomic_check,
	.atomic_commit		= drm_atomic_helper_commit,
	.fb_create		= sun4i_fb_create,
};

static void sun4i_de_atomic_commit_tail(struct drm_atomic_commit *state)
{
	struct drm_device *dev = state->dev;

	drm_atomic_helper_commit_modeset_disables(dev, state);
	drm_atomic_helper_commit_crtc_enable(dev, state);
	drm_atomic_helper_commit_encoder_bridge_pre_enable(dev, state);
	drm_atomic_helper_commit_planes(dev, state,
					DRM_PLANE_COMMIT_ACTIVE_ONLY);
	drm_atomic_helper_commit_encoder_bridge_enable(dev, state);
	drm_atomic_helper_commit_writebacks(dev, state);
	drm_atomic_helper_fake_vblank(state);
	drm_atomic_helper_commit_hw_done(state);
	/*
	 * Wait for a vblank after the engine commit, not for the flip
	 * event: the event can be signaled by a vblank interrupt that
	 * fires between atomic_begin and the register queue trigger, in
	 * which case buffers would be freed while the hardware still
	 * scans out from them for one more frame.
	 */
	drm_atomic_helper_wait_for_vblanks(dev, state);
	drm_atomic_helper_cleanup_planes(dev, state);
}

static const struct drm_mode_config_helper_funcs sun4i_de_mode_config_helpers = {
	.atomic_commit_tail	= sun4i_de_atomic_commit_tail,
};

void sun4i_framebuffer_init(struct drm_device *drm)
{
	drm_mode_config_reset(drm);

	drm->mode_config.max_width = 8192;
	drm->mode_config.max_height = 8192;
	drm->mode_config.normalize_zpos = true;

	drm->mode_config.funcs = &sun4i_de_mode_config_funcs;
	drm->mode_config.helper_private = &sun4i_de_mode_config_helpers;
}
