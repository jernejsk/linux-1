/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) Jernej Skrabec <jernej.skrabec@gmail.com>
 */

#ifndef _SUN8I_RDMA_H_
#define _SUN8I_RDMA_H_

struct sun8i_rdma;
struct sun8i_rdma_unit;

struct reg_region {
	unsigned int offset;
	unsigned int count;
};

struct sun8i_rdma *sun8i_rdma_init(void);
void sun8i_rdma_deinit(struct sun8i_rdma *rdma);
void sun8i_rdma_apply(struct sun8i_rdma *rdma);

struct sun8i_rdma_unit *
sun8i_rdma_add_unit(struct sun8i_rdma *rdma, void __iomem *base,
		    unsigned int size, const struct reg_region *regions);
void sun8i_rdma_write(struct sun8i_rdma_unit *unit,
		      unsigned int reg, u32 value);
void sun8i_rdma_memcpy(struct sun8i_rdma_unit *unit, unsigned int reg,
		       u32 *values, unsigned int count);

#endif /* _SUN8I_RDMA_H_ */
