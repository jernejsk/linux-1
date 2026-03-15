// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) Jernej Skrabec <jernej.skrabec@gmail.com>
 */

#include <asm/io.h>
#include <linux/list.h>
#include <linux/slab.h>

#include "sun8i_rdma.h"

struct sun8i_rdma_unit {
	struct list_head node;
	void __iomem *base;
	void *mem;
	const struct reg_region *regions;
};

struct sun8i_rdma {
	struct list_head list;
};

struct sun8i_rdma *sun8i_rdma_init(void)
{
	struct sun8i_rdma *priv;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return NULL;

	INIT_LIST_HEAD(&priv->list);

	return priv;
}

void sun8i_rdma_deinit(struct sun8i_rdma *rdma)
{
	struct sun8i_rdma_unit *unit, *tmp;

	list_for_each_entry_safe(unit, tmp, &rdma->list, node) {
		list_del(&unit->node);
		kfree(unit->mem);
		kfree(unit);
	}

	kfree(rdma);
}

void sun8i_rdma_apply(struct sun8i_rdma *rdma)
{
	struct sun8i_rdma_unit *unit;

	list_for_each_entry(unit, &rdma->list, node) {
		const struct reg_region *region = unit->regions;

		while (region->count) {
			writesl(unit->base + region->offset,
				unit->mem + region->offset,
				region->count);
			region++;
		}
	}
}

struct sun8i_rdma_unit *
sun8i_rdma_add_unit(struct sun8i_rdma *rdma, void __iomem *base,
		    unsigned int size, const struct reg_region *regions)
{
	struct sun8i_rdma_unit *unit;

	unit = kzalloc(sizeof(*unit), GFP_KERNEL);
	if (!unit)
		return NULL;

	unit->mem = kzalloc(size, GFP_KERNEL);
	if (!unit->mem) {
		kfree(unit);
		return NULL;
	}

	unit->base = base;
	unit->regions = regions;

	list_add_tail(&rdma->list, &unit->node);

	return unit;
}

void sun8i_rdma_write(struct sun8i_rdma_unit *unit,
		      unsigned int reg, u32 value)
{
	u32 *ptr = unit->mem + reg;

	*ptr = value;
}

void sun8i_rdma_memcpy(struct sun8i_rdma_unit *unit, unsigned int reg,
		       u32 *values, unsigned int count)
{
	memcpy(unit->mem + reg, values, count * 4);
}
