// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) Jernej Skrabec <jernej.skrabec@gmail.com>
 */

#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/slab.h>

#include "sun8i_rdma.h"

struct sun8i_rcq_head {
	u32 low_addr;
	u32 len_hi;
	u32 dirty;
	u32 reg_offset;
};

struct sun8i_rdma_unit {
	struct list_head node;
	void __iomem *base;
	void *mem;
	dma_addr_t dma_addr;
	u32 reg_offset;
	u32 size;
	const struct reg_region *regions;
	unsigned int first_head;
	bool dirty;
};

struct sun8i_rdma {
	struct device *dev;
	struct list_head list;
	void __iomem *rcq;
	struct sun8i_rcq_head *heads;
	dma_addr_t heads_dma;
	unsigned int head_count;
	unsigned int head_alloc;
	bool rcq_enabled;
	bool rcq_ready;
};

struct sun8i_rdma *sun8i_rdma_init(struct device *dev,
				   void __iomem *rcq, bool rcq_enabled)
{
	struct sun8i_rdma *rdma;

	rdma = kzalloc(sizeof(*rdma), GFP_KERNEL);
	if (!rdma)
		return NULL;

	rdma->dev = dev;
	rdma->rcq = rcq;
	rdma->rcq_enabled = rcq_enabled;
	INIT_LIST_HEAD(&rdma->list);

	return rdma;
}

void sun8i_rdma_deinit(struct sun8i_rdma *rdma)
{
	struct sun8i_rdma_unit *unit, *tmp;

	if (!rdma)
		return;

	if (rdma->heads)
		dma_free_coherent(rdma->dev,
				  rdma->head_alloc * sizeof(*rdma->heads),
				  rdma->heads, rdma->heads_dma);

	list_for_each_entry_safe(unit, tmp, &rdma->list, node) {
		list_del(&unit->node);
		if (rdma->rcq_enabled)
			dma_free_coherent(rdma->dev, unit->size, unit->mem,
					  unit->dma_addr);
		else
			kfree(unit->mem);
		kfree(unit);
	}

	kfree(rdma);
}

static int sun8i_rdma_prepare_rcq(struct sun8i_rdma *rdma)
{
	struct sun8i_rdma_unit *unit;
	unsigned int count = 0;

	if (rdma->rcq_ready)
		return 0;

	list_for_each_entry(unit, &rdma->list, node) {
		const struct reg_region *region = unit->regions;

		while (region->count) {
			count++;
			region++;
		}
	}

	/* The hardware fetches RCQ heads in pairs. */
	rdma->head_alloc = ALIGN(count, 2);
	rdma->heads = dma_alloc_coherent(rdma->dev,
					 rdma->head_alloc * sizeof(*rdma->heads),
					 &rdma->heads_dma, GFP_KERNEL);
	if (!rdma->heads)
		return -ENOMEM;

	count = 0;
	list_for_each_entry(unit, &rdma->list, node) {
		const struct reg_region *region = unit->regions;

		unit->first_head = count;
		while (region->count) {
			struct sun8i_rcq_head *head = &rdma->heads[count++];
			dma_addr_t addr = unit->dma_addr + region->offset;

			head->low_addr = lower_32_bits(addr);
			head->len_hi = region->count * sizeof(u32) |
				       (upper_32_bits(addr) << 24);
			head->reg_offset = unit->reg_offset + region->offset;
			region++;
		}
	}
	rdma->head_count = count;

	writel(lower_32_bits(rdma->heads_dma), rdma->rcq + 0x4);
	writel(upper_32_bits(rdma->heads_dma), rdma->rcq + 0x8);
	writel(rdma->head_alloc * sizeof(*rdma->heads), rdma->rcq + 0xc);
	rdma->rcq_ready = true;

	return 0;
}

int sun8i_rdma_apply(struct sun8i_rdma *rdma)
{
	struct sun8i_rdma_unit *unit;
	bool dirty = false;
	int ret;

	if (rdma->rcq_enabled) {
		ret = sun8i_rdma_prepare_rcq(rdma);
		if (ret)
			return ret;

		list_for_each_entry(unit, &rdma->list, node) {
			const struct reg_region *region;
			unsigned int head;

			if (!unit->dirty)
				continue;

			head = unit->first_head;
			for (region = unit->regions; region->count; region++)
				rdma->heads[head++].dirty = 1;
			unit->dirty = false;
			dirty = true;
		}

		if (dirty) {
			dma_wmb();
			writel(1, rdma->rcq);
		}

		return 0;
	}

	list_for_each_entry(unit, &rdma->list, node) {
		const struct reg_region *region;

		if (!unit->dirty)
			continue;

		for (region = unit->regions; region->count; region++)
			writesl(unit->base + region->offset,
			       unit->mem + region->offset, region->count);
		unit->dirty = false;
	}

	return 0;
}

struct sun8i_rdma_unit *
sun8i_rdma_add_unit(struct sun8i_rdma *rdma, void __iomem *base,
		    u32 reg_offset, unsigned int size,
		    const struct reg_region *regions)
{
	struct sun8i_rdma_unit *unit;

	unit = kzalloc(sizeof(*unit), GFP_KERNEL);
	if (!unit)
		return NULL;

	if (rdma->rcq_enabled)
		unit->mem = dma_alloc_coherent(rdma->dev, size, &unit->dma_addr,
					       GFP_KERNEL);
	else
		unit->mem = kzalloc(size, GFP_KERNEL);
	if (!unit->mem) {
		kfree(unit);
		return NULL;
	}

	unit->base = base;
	unit->reg_offset = reg_offset;
	unit->size = size;
	unit->regions = regions;
	list_add_tail(&unit->node, &rdma->list);

	return unit;
}

void sun8i_rdma_write(struct sun8i_rdma_unit *unit,
		      unsigned int reg, u32 value)
{
	u32 *ptr = unit->mem + reg;

	WARN_ON(reg + sizeof(*ptr) > unit->size);
	*ptr = value;
	unit->dirty = true;
}

void sun8i_rdma_memcpy(struct sun8i_rdma_unit *unit, unsigned int reg,
		       const u32 *values, unsigned int count)
{
	WARN_ON(reg + count * sizeof(*values) > unit->size);
	memcpy(unit->mem + reg, values, count * sizeof(*values));
	unit->dirty = true;
}
