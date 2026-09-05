/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Copyright (c) 2026 Jernej Skrabec <jernej.skrabec@gmail.com> */

#ifndef PVR_DEVFREQ_H
#define PVR_DEVFREQ_H

#include <linux/devfreq.h>
#include <linux/ktime.h>
#include <linux/spinlock.h>

struct devfreq;
struct pvr_device;
struct thermal_cooling_device;

/**
 * struct pvr_devfreq - Frequency scaling state for a PowerVR device.
 */
struct pvr_devfreq {
	/** @devfreq: The devfreq device, or %NULL when scaling is not in use. */
	struct devfreq *devfreq;

	/** @cooling: Cooling device throttling us, or %NULL. */
	struct thermal_cooling_device *cooling;

	/** @gov_data: Thresholds for the simple_ondemand governor. */
	struct devfreq_simple_ondemand_data gov_data;

	/** @current_frequency: Rate last requested through the OPP table. */
	unsigned long current_frequency;

	/** @busy_time: Time spent with at least one job in flight. */
	ktime_t busy_time;

	/** @idle_time: Time spent with nothing in flight. */
	ktime_t idle_time;

	/** @time_last_update: When @busy_time and @idle_time were last advanced. */
	ktime_t time_last_update;

	/** @busy_count: Number of jobs currently in flight. */
	int busy_count;

	/**
	 * @lock: Protects @busy_time, @idle_time, @time_last_update and
	 * @busy_count, which jobs on every queue update.
	 */
	spinlock_t lock;
};

int pvr_devfreq_init(struct pvr_device *pvr_dev);
void pvr_devfreq_fini(struct pvr_device *pvr_dev);

void pvr_devfreq_resume(struct pvr_device *pvr_dev);
void pvr_devfreq_suspend(struct pvr_device *pvr_dev);

void pvr_devfreq_record_busy(struct pvr_device *pvr_dev);
void pvr_devfreq_record_idle(struct pvr_device *pvr_dev);

#endif /* PVR_DEVFREQ_H */
