// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright (c) 2026 Jernej Skrabec <jernej.skrabec@gmail.com> */

#include "pvr_ccb.h"
#include "pvr_devfreq.h"
#include "pvr_device.h"
#include "pvr_fw.h"
#include "pvr_power.h"
#include "pvr_rogue_fwif.h"

#include <drm/drm_device.h>
#include <drm/drm_print.h>

#include <linux/clk.h>
#include <linux/devfreq.h>
#include <linux/devfreq_cooling.h>
#include <linux/pm_opp.h>
#include <linux/pm_runtime.h>
#include <linux/thermal.h>

static void pvr_devfreq_update_utilization(struct pvr_devfreq *df)
{
	ktime_t now = ktime_get();
	ktime_t last = df->time_last_update;

	if (df->busy_count > 0)
		df->busy_time += ktime_sub(now, last);
	else
		df->idle_time += ktime_sub(now, last);

	df->time_last_update = now;
}

static void pvr_devfreq_reset(struct pvr_devfreq *df)
{
	df->busy_time = 0;
	df->idle_time = 0;
	df->time_last_update = ktime_get();
}

/*
 * The firmware keeps its own idea of the core clock for its timers, so tell
 * it whenever the rate moves. The runtime configuration is what it reads on
 * every boot; the command is only for a firmware that is up right now.
 */
static void pvr_devfreq_notify_fw(struct pvr_device *pvr_dev)
{
	struct drm_device *drm_dev = from_pvr_device(pvr_dev);
	struct pvr_fw_device *fw_dev = &pvr_dev->fw_dev;
	struct rogue_fwif_runtime_cfg *runtime_cfg;
	struct rogue_fwif_kccb_cmd cmd;
	u32 clock_speed_hz;

	if (!READ_ONCE(fw_dev->initialised) || pvr_dev->lost)
		return;

	clock_speed_hz = clk_get_rate(pvr_dev->core_clk);

	runtime_cfg = pvr_fw_object_vmap(fw_dev->mem.runtime_cfg_obj);
	if (IS_ERR(runtime_cfg))
		return;

	WRITE_ONCE(runtime_cfg->core_clock_speed, clock_speed_hz);
	pvr_fw_object_vunmap(fw_dev->mem.runtime_cfg_obj);

	if (pm_runtime_get_if_active(drm_dev->dev) <= 0)
		return;

	cmd.cmd_type = ROGUE_FWIF_KCCB_CMD_CORECLKSPEEDCHANGE;
	cmd.kccb_flags = 0;
	cmd.cmd_data.core_clk_speed_change_data.new_clock_speed = clock_speed_hz;

	if (pvr_kccb_send_cmd_powered(pvr_dev, &cmd, NULL))
		drm_warn(drm_dev, "Firmware missed the clock change to %u Hz\n",
			 clock_speed_hz);

	pm_runtime_put(drm_dev->dev);
}

static int pvr_devfreq_target(struct device *dev, unsigned long *freq, u32 flags)
{
	struct drm_device *drm_dev = dev_get_drvdata(dev);
	struct pvr_device *pvr_dev = to_pvr_device(drm_dev);
	struct dev_pm_opp *opp;
	int err;

	opp = devfreq_recommended_opp(dev, freq, flags);
	if (IS_ERR(opp))
		return PTR_ERR(opp);
	dev_pm_opp_put(opp);

	err = dev_pm_opp_set_rate(dev, *freq);
	if (err)
		return err;

	pvr_dev->devfreq.current_frequency = *freq;
	pvr_devfreq_notify_fw(pvr_dev);

	return 0;
}

static int pvr_devfreq_get_dev_status(struct device *dev,
				      struct devfreq_dev_status *status)
{
	struct drm_device *drm_dev = dev_get_drvdata(dev);
	struct pvr_device *pvr_dev = to_pvr_device(drm_dev);
	struct pvr_devfreq *df = &pvr_dev->devfreq;
	unsigned long irqflags;

	status->current_frequency = clk_get_rate(pvr_dev->core_clk);

	spin_lock_irqsave(&df->lock, irqflags);

	pvr_devfreq_update_utilization(df);

	status->total_time = ktime_to_ns(ktime_add(df->busy_time, df->idle_time));
	status->busy_time = ktime_to_ns(df->busy_time);

	pvr_devfreq_reset(df);

	spin_unlock_irqrestore(&df->lock, irqflags);

	return 0;
}

static struct devfreq_dev_profile pvr_devfreq_profile = {
	.timer = DEVFREQ_TIMER_DELAYED,
	.polling_ms = 50, /* about three frames */
	.target = pvr_devfreq_target,
	.get_dev_status = pvr_devfreq_get_dev_status,
};

static const char * const pvr_devfreq_supplies[] = { "power", NULL };

/**
 * pvr_devfreq_init() - Set up frequency scaling for a device
 * @pvr_dev: Target PowerVR device.
 *
 * Scaling is only used when the device tree carries an operating points
 * table. Without one this is a no-op and the clocks stay where firmware left
 * them.
 *
 * Returns:
 *  * 0 on success, or
 *  * Any error returned by the OPP or devfreq cores.
 */
int pvr_devfreq_init(struct pvr_device *pvr_dev)
{
	struct drm_device *drm_dev = from_pvr_device(pvr_dev);
	struct device *dev = drm_dev->dev;
	struct pvr_devfreq *df = &pvr_dev->devfreq;
	struct thermal_cooling_device *cooling;
	struct devfreq *devfreq;
	struct dev_pm_opp *opp;
	unsigned long cur_freq;
	int err;

	err = devm_pm_opp_set_regulators(dev, pvr_devfreq_supplies);
	if (err && err != -ENODEV)
		return dev_err_probe(dev, err, "Couldn't set OPP regulators\n");

	err = devm_pm_opp_of_add_table(dev);
	if (err == -ENODEV)
		return 0;
	if (err)
		return dev_err_probe(dev, err, "Couldn't add OPP table\n");

	spin_lock_init(&df->lock);
	pvr_devfreq_reset(df);

	cur_freq = clk_get_rate(pvr_dev->core_clk);

	opp = devfreq_recommended_opp(dev, &cur_freq, 0);
	if (IS_ERR(opp))
		return PTR_ERR(opp);

	pvr_devfreq_profile.initial_freq = cur_freq;
	df->current_frequency = cur_freq;

	/*
	 * Land on a real operating point right away, so the supply matches
	 * the clock before the first job runs.
	 */
	err = dev_pm_opp_set_opp(dev, opp);
	dev_pm_opp_put(opp);
	if (err)
		return dev_err_probe(dev, err, "Couldn't set initial OPP\n");

	pvr_devfreq_notify_fw(pvr_dev);

	/* The same thresholds panfrost settled on for rendering loads. */
	df->gov_data.upthreshold = 45;
	df->gov_data.downdifferential = 5;

	devfreq = devm_devfreq_add_device(dev, &pvr_devfreq_profile,
					  DEVFREQ_GOV_SIMPLE_ONDEMAND,
					  &df->gov_data);
	if (IS_ERR(devfreq))
		return dev_err_probe(dev, PTR_ERR(devfreq),
				     "Couldn't add devfreq device\n");

	df->devfreq = devfreq;

	cooling = of_devfreq_cooling_register(dev->of_node, devfreq);
	if (IS_ERR(cooling))
		drm_info(drm_dev, "Running without a GPU cooling device\n");
	else
		df->cooling = cooling;

	return 0;
}

void pvr_devfreq_fini(struct pvr_device *pvr_dev)
{
	struct pvr_devfreq *df = &pvr_dev->devfreq;

	if (df->cooling) {
		devfreq_cooling_unregister(df->cooling);
		df->cooling = NULL;
	}
}

void pvr_devfreq_resume(struct pvr_device *pvr_dev)
{
	struct pvr_devfreq *df = &pvr_dev->devfreq;

	if (!df->devfreq)
		return;

	pvr_devfreq_reset(df);
	devfreq_resume_device(df->devfreq);
}

void pvr_devfreq_suspend(struct pvr_device *pvr_dev)
{
	struct pvr_devfreq *df = &pvr_dev->devfreq;

	if (!df->devfreq)
		return;

	devfreq_suspend_device(df->devfreq);
}

void pvr_devfreq_record_busy(struct pvr_device *pvr_dev)
{
	struct pvr_devfreq *df = &pvr_dev->devfreq;
	unsigned long irqflags;

	if (!df->devfreq)
		return;

	spin_lock_irqsave(&df->lock, irqflags);
	pvr_devfreq_update_utilization(df);
	df->busy_count++;
	spin_unlock_irqrestore(&df->lock, irqflags);
}

void pvr_devfreq_record_idle(struct pvr_device *pvr_dev)
{
	struct pvr_devfreq *df = &pvr_dev->devfreq;
	unsigned long irqflags;

	if (!df->devfreq)
		return;

	spin_lock_irqsave(&df->lock, irqflags);
	pvr_devfreq_update_utilization(df);
	WARN_ON(--df->busy_count < 0);
	spin_unlock_irqrestore(&df->lock, irqflags);
}
