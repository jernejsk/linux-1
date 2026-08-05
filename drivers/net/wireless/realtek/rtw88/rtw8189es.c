// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/* Copyright(c) 2007-2017  Realtek Corporation
 * Copyright(c) 2026  Jernej Skrabec <jernej.skrabec@gmail.com>
 */

#include <linux/mmc/sdio_func.h>
#include <linux/mmc/sdio_ids.h>
#include <linux/module.h>
#include "main.h"
#include "rtw8188e.h"
#include "sdio.h"

static const struct sdio_device_id rtw_8189es_id_table[] = {
	{
		SDIO_DEVICE(SDIO_VENDOR_ID_REALTEK,
			    SDIO_DEVICE_ID_REALTEK_RTW8189ES),
		.driver_data = (kernel_ulong_t)&rtw8188e_hw_spec,
	},
	{}
};
MODULE_DEVICE_TABLE(sdio, rtw_8189es_id_table);

static struct sdio_driver rtw_8189es_driver = {
	.name = KBUILD_MODNAME,
	.id_table = rtw_8189es_id_table,
	.probe = rtw_sdio_probe,
	.remove = rtw_sdio_remove,
	.shutdown = rtw_sdio_shutdown,
	.drv = {
		.pm = &rtw_sdio_pm_ops,
	}};
module_sdio_driver(rtw_8189es_driver);

MODULE_AUTHOR("Jernej Skrabec <jernej.skrabec@gmail.com>");
MODULE_DESCRIPTION("Realtek 802.11n wireless 8189es driver");
MODULE_LICENSE("Dual BSD/GPL");
