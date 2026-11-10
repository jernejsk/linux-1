/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Cedrus VPU driver
 *
 * Copyright (C) 2016 Florent Revest <florent.revest@free-electrons.com>
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 * Copyright (C) 2018 Bootlin
 *
 * Based on the vim2m driver, that is:
 *
 * Copyright (c) 2009-2010 Samsung Electronics Co., Ltd.
 * Pawel Osciak, <pawel@osciak.com>
 * Marek Szyprowski, <m.szyprowski@samsung.com>
 */

#ifndef _CEDRUS_DEC_H_
#define _CEDRUS_DEC_H_

void cedrus_device_run(void *priv);
void cedrus_job_abort(void *priv);
void cedrus_job_done(struct cedrus_ctx *ctx, enum vb2_buffer_state state);
void cedrus_pic_work(struct work_struct *work);
void cedrus_pic_guard(struct work_struct *work);
void cedrus_pic_flush(struct cedrus_ctx *ctx);
void cedrus_pic_slice_queued(struct cedrus_ctx *ctx);

#endif
