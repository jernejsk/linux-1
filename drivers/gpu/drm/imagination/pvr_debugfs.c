// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright (c) 2023 Imagination Technologies Ltd. */

#include "pvr_debugfs.h"

#include "pvr_device.h"
#include "pvr_fw_trace.h"
#include "pvr_power.h"

#include <linux/dcache.h>
#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/kernel.h>
#include <linux/types.h>

#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_print.h>

/*
 * Writing "soft" or "hard" resets the GPU the way the driver does after a
 * firmware stall, so the recovery path can be exercised on demand.
 */
static ssize_t pvr_debugfs_reset_write(struct file *file, const char __user *ubuf,
				       size_t len, loff_t *off)
{
	struct pvr_device *pvr_dev = file->private_data;
	char buf[8] = {};
	int err;

	if (len >= sizeof(buf))
		return -EINVAL;

	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;

	if (sysfs_streq(buf, "hard"))
		err = pvr_power_reset(pvr_dev, true);
	else if (sysfs_streq(buf, "soft"))
		err = pvr_power_reset(pvr_dev, false);
	else
		return -EINVAL;

	return err ? err : len;
}

static const struct file_operations pvr_debugfs_reset_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = pvr_debugfs_reset_write,
	.llseek = default_llseek,
};

static void pvr_debugfs_reset_init(struct pvr_device *pvr_dev, struct dentry *dir)
{
	debugfs_create_file("reset", 0200, dir, pvr_dev, &pvr_debugfs_reset_fops);
}

static const struct pvr_debugfs_entry pvr_debugfs_entries[] = {
	{"pvr_fw", pvr_fw_trace_debugfs_init},
	{"pvr_power", pvr_debugfs_reset_init},
};

void
pvr_debugfs_init(struct drm_minor *minor)
{
	struct drm_device *drm_dev = minor->dev;
	struct pvr_device *pvr_dev = to_pvr_device(drm_dev);
	struct dentry *root = minor->debugfs_root;

	for (size_t i = 0; i < ARRAY_SIZE(pvr_debugfs_entries); ++i) {
		const struct pvr_debugfs_entry *entry = &pvr_debugfs_entries[i];
		struct dentry *dir;

		dir = debugfs_create_dir(entry->name, root);
		if (IS_ERR(dir)) {
			drm_warn(drm_dev,
				 "failed to create debugfs dir '%s' (err=%d)",
				 entry->name, (int)PTR_ERR(dir));
			continue;
		}

		entry->init(pvr_dev, dir);
	}
}

/*
 * Since all entries are created under &drm_minor->debugfs_root, there's no
 * need for a pvr_debugfs_fini() as DRM will clean up everything under its root
 * automatically.
 */
