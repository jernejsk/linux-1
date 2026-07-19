// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) Jernej Skrabec <jernej.skrabec@gmail.com>
 */

#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/list.h>
#include <linux/slab.h>

#include "sun8i_rdma.h"

#define SUN8I_RCQ_STATUS_OFFSET	0x0c
#define SUN8I_RCQ_STATUS_FINISH	BIT(2)
#define SUN8I_RCQ_STATUS_ACCEPT	BIT(3)
#define SUN8I_RCQ_STATUS_W1C	(BIT(0) | GENMASK(3, 2))

/* Two frame periods at the lowest supported refresh rate. */
#define SUN8I_RCQ_FINISH_TIMEOUT_US	40000

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
	void *rcq_mem;
	dma_addr_t rcq_dma;
	size_t rcq_size;
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
	bool rcq_armed;
};

struct sun8i_rdma *sun8i_rdma_init(struct device *dev,
				   void __iomem *rcq, bool rcq_enabled)
{
	struct sun8i_rdma *rdma;

	rdma = kzalloc_obj(*rdma, GFP_KERNEL);
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
			dma_free_coherent(rdma->dev, unit->rcq_size,
					  unit->rcq_mem, unit->rcq_dma);
		kfree(unit->mem);
		kfree(unit);
	}

	kfree(rdma);
}

int sun8i_rdma_prepare(struct sun8i_rdma *rdma)
{
	struct sun8i_rdma_unit *unit;
	unsigned int count = 0;

	if (!rdma->rcq_enabled || rdma->rcq_ready)
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
	memset(rdma->heads, 0, rdma->head_alloc * sizeof(*rdma->heads));

	count = 0;
	list_for_each_entry(unit, &rdma->list, node) {
		const struct reg_region *region = unit->regions;
		size_t offset = 0;

		unit->first_head = count;
		while (region->count) {
			struct sun8i_rcq_head *head = &rdma->heads[count++];
			dma_addr_t addr = unit->rcq_dma + offset;

			head->low_addr = lower_32_bits(addr);
			head->len_hi = region->count * sizeof(u32) |
				       (upper_32_bits(addr) << 24);
			head->reg_offset = unit->reg_offset + region->offset;
			offset += ALIGN(region->count * sizeof(u32), 32);
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

/* Wait until a previously triggered queue fetch was consumed. */
void sun8i_rdma_sync(struct sun8i_rdma *rdma)
{
	u32 status;
	int ret;

	if (!rdma->rcq_enabled || !rdma->rcq_armed)
		return;

	ret = readl_poll_timeout(rdma->rcq - SUN8I_RCQ_STATUS_OFFSET, status,
				 status & SUN8I_RCQ_STATUS_FINISH,
				 10, SUN8I_RCQ_FINISH_TIMEOUT_US);
	if (ret)
		dev_warn(rdma->dev, "RCQ sync timed out\n");
	rdma->rcq_armed = false;
}

int sun8i_rdma_apply(struct sun8i_rdma *rdma)
{
	struct sun8i_rdma_unit *unit;
	bool dirty = false;
	unsigned int i;

	if (rdma->rcq_enabled) {
		if (WARN_ON(!rdma->rcq_ready))
			return -EINVAL;

		/*
		 * The queue may be fetched only once per update. Triggering
		 * a new update before the previous one was consumed can
		 * lose it entirely, leaving the hardware scanning out stale
		 * buffer addresses. Wait for the previous fetch and fall
		 * back to CPU writes if it never happened, like the vendor
		 * driver does.
		 */
		if (rdma->rcq_armed) {
			u32 status;
			int ret;

			ret = readl_poll_timeout(rdma->rcq - SUN8I_RCQ_STATUS_OFFSET,
						 status,
						 status & SUN8I_RCQ_STATUS_FINISH,
						 10, SUN8I_RCQ_FINISH_TIMEOUT_US);
			rdma->rcq_armed = false;
			if (ret) {
				dev_warn(rdma->dev,
					 "RCQ not consumed, syncing registers with CPU\n");
				writel(0, rdma->rcq);
				writel(SUN8I_RCQ_STATUS_W1C,
				       rdma->rcq - SUN8I_RCQ_STATUS_OFFSET);
				list_for_each_entry(unit, &rdma->list, node) {
					const struct reg_region *region;

					for (region = unit->regions; region->count; region++)
						writesl(unit->base + region->offset,
							unit->mem + region->offset,
							region->count);
					unit->dirty = false;
				}
				return 0;
			}
		}

		/*
		 * Gate queue fetches while the head list and staging
		 * memory are inconsistent, and retire the dirty flags of
		 * the consumed update so a spurious refetch applies
		 * nothing.
		 */
		writel(0, rdma->rcq);
		writel(SUN8I_RCQ_STATUS_W1C,
		       rdma->rcq - SUN8I_RCQ_STATUS_OFFSET);
		for (i = 0; i < rdma->head_count; i++)
			rdma->heads[i].dirty = 0;

		list_for_each_entry(unit, &rdma->list, node) {
			const struct reg_region *region;
			unsigned int head;
			size_t offset = 0;

			if (!unit->dirty)
				continue;

			head = unit->first_head;
			for (region = unit->regions; region->count; region++) {
				memcpy(unit->rcq_mem + offset,
				       unit->mem + region->offset,
				       region->count * sizeof(u32));
				rdma->heads[head++].dirty = 1;
				offset += ALIGN(region->count * sizeof(u32), 32);
			}
			unit->dirty = false;
			dirty = true;
		}

		if (dirty) {
			dma_wmb();
			writel(1, rdma->rcq);
			rdma->rcq_armed = true;
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
	const struct reg_region *region;

	unit = kzalloc_obj(*unit, GFP_KERNEL);
	if (!unit)
		return NULL;

	unit->mem = kzalloc(size, GFP_KERNEL);
	if (!unit->mem) {
		kfree(unit);
		return NULL;
	}

	if (rdma->rcq_enabled) {
		/* Every RCQ source block must start at a 32-byte boundary. */
		for (region = regions; region->count; region++)
			unit->rcq_size += ALIGN(region->count * sizeof(u32), 32);

		unit->rcq_mem = dma_alloc_coherent(rdma->dev, unit->rcq_size,
						   &unit->rcq_dma, GFP_KERNEL);
		if (!unit->rcq_mem) {
			kfree(unit->mem);
			kfree(unit);
			return NULL;
		}
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
