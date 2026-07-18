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
	.atomic_check		= drm_atomic_helper_check,
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
