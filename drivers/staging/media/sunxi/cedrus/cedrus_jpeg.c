// SPDX-License-Identifier: GPL-2.0
/*
 * Cedrus VPU driver
 *
 * Copyright (C) 2022 Emmanuel Gil Peyrot <linkmauve@linkmauve.fr>
 */

#include <media/videobuf2-dma-contig.h>
#include <media/v4l2-jpeg.h>

#include "cedrus.h"
#include "cedrus_hw.h"
#include "cedrus_regs.h"

static enum cedrus_irq_status cedrus_jpeg_irq_status(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;
	u32 reg;

	reg = cedrus_read(dev, VE_DEC_MPEG_STATUS);
	reg &= VE_DEC_MPEG_STATUS_CHECK_MASK;

	if (!reg)
		return CEDRUS_IRQ_NONE;

	if (reg & VE_DEC_MPEG_STATUS_CHECK_ERROR)
		return CEDRUS_IRQ_ERROR;

	return CEDRUS_IRQ_OK;
}

static void cedrus_jpeg_irq_clear(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;

	cedrus_write(dev, VE_DEC_MPEG_STATUS, VE_DEC_MPEG_STATUS_CHECK_MASK);
}

static void cedrus_jpeg_irq_disable(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;
	u32 reg = cedrus_read(dev, VE_DEC_MPEG_CTRL);

	reg &= ~VE_DEC_MPEG_CTRL_IRQ_MASK;

	cedrus_write(dev, VE_DEC_MPEG_CTRL, reg);
}

static int cedrus_write_table_header(struct cedrus_dev *dev,
				     struct v4l2_jpeg_reference *table)
{
	u16 start_codes[16], code;
	u8 offsets[16], *ptr;
	unsigned int count;
	u32 *ptr32;
	int i;

	ptr = table->start;
	if (!ptr)
		return -EINVAL;

	count = 0;
	code = 0;
	for (i = 0; i < 16; i++) {
		offsets[i] = count;
		start_codes[i] = code;
		count += ptr[i];
		code += ptr[i];
		code *= 2;
	}

	for (i = 15; i >= 0 && !ptr[i]; i--)
		start_codes[i] = 0xffff;

	ptr32 = (u32*)start_codes;
	for (i = 0; i < 8; i++)
		cedrus_write(dev, VE_DEC_MPEG_SRAM_RW_DATA, ptr32[i]);

	ptr32 = (u32*)offsets;
	for (i = 0; i < 4; i++)
		cedrus_write(dev, VE_DEC_MPEG_SRAM_RW_DATA, ptr32[i]);

	for (i = 0; i < 4; i++)
		cedrus_write(dev, VE_DEC_MPEG_SRAM_RW_DATA, 0);

	return 0;
}

static int cedrus_jpeg_write_dh_tables(struct cedrus_dev *dev,
				       struct v4l2_jpeg_header *hdr)
{
	struct v4l2_jpeg_reference *tables[4], *table;
	struct v4l2_jpeg_scan_component_spec *comp;
	unsigned int i, j, ret;
	size_t length;
	u32 *ptr, val;

	cedrus_write(dev, VE_DEC_MPEG_SRAM_RW_OFFSET, 0);

	j = 0;
	for (i = 0; i < 2; i++) {
		comp = &hdr->scan->component[i];

		tables[j++] = &hdr->huffman_tables[comp->dc_entropy_coding_table_selector];
		tables[j++] = &hdr->huffman_tables[comp->ac_entropy_coding_table_selector + 2];
	}

	for (i = 0; i < 4; i++) {
		ret = cedrus_write_table_header(dev, tables[i]);
		if (ret)
			return ret;
	}

	for (i = 0; i < 192; i++)
		cedrus_write(dev, VE_DEC_MPEG_SRAM_RW_DATA, 0);

	for (i = 0; i < 4; i++) {
		table = tables[i];
		ptr = (u32*)&table->start[16];
		length = table->length - 16;

		for (j = 0; j < length / 4; j++)
			cedrus_write(dev, VE_DEC_MPEG_SRAM_RW_DATA, ptr[j]);

		if (length & 3) {
			val = 0;
			for (j = 0; j < (length & 3); j++)
				val = (val << 8) | table->start[15 + length - j];
			cedrus_write(dev, VE_DEC_MPEG_SRAM_RW_DATA, val);
		}

		for (j = 0; j < 64 - DIV_ROUND_UP(length, 4); j++)
			cedrus_write(dev, VE_DEC_MPEG_SRAM_RW_DATA, 0);
	}

	return 0;
}

static int cedrus_write_quantization_matrix(struct cedrus_dev *dev, u32 flags,
					    struct v4l2_jpeg_reference *table)
{
	const u8 *matrix;
	u32 reg, val;
	int i;

	matrix = table->start;
	if (!matrix)
		return -EINVAL;

	for (i = 0; i < 64; i++) {
		/* determine if values are 8 or 16 bits */
		val = *matrix++;
		if (table->length > 64)
			val = (val << 8) | *matrix++;

		reg = VE_DEC_MPEG_IQMINPUT_WEIGHT(i, val);
		reg |= flags;

		cedrus_write(dev, VE_DEC_MPEG_IQMINPUT, reg);
	}

	return 0;
}

static int cedrus_jpeg_setup(struct cedrus_ctx *ctx, struct cedrus_run *run)
{
	struct v4l2_jpeg_reference quantization_tables[4] = { };
	dma_addr_t src_buf_addr, dst_luma_addr, dst_chroma_addr;
	struct v4l2_jpeg_reference huffman_tables[4] = { };
	struct v4l2_jpeg_frame_component_spec *components;
	struct vb2_buffer *src_buf = &run->src->vb2_buf;
	struct v4l2_jpeg_scan_header scan_header;
	struct cedrus_dev *dev = ctx->dev;
	struct v4l2_jpeg_header header = {
		.scan = &scan_header,
		.quantization_tables = quantization_tables,
		.huffman_tables = huffman_tables,
	};
	u32 reg, subsampling;
	unsigned long size;
	int ret, index;
	u8 hmax, vmax;

	size = vb2_get_plane_payload(src_buf, 0);
	components = header.frame.component;

	ret = v4l2_jpeg_parse_header(vb2_plane_vaddr(src_buf, 0), size, &header);
	if (ret < 0) {
		v4l2_err(&dev->v4l2_dev, "failed to parse JPEG header: %d\n", ret);
		return -EINVAL;
	}

	index = components[0].horizontal_sampling_factor << 20 |
		components[0].vertical_sampling_factor << 16 |
		components[1].horizontal_sampling_factor << 12 |
		components[1].vertical_sampling_factor << 8 |
		components[2].horizontal_sampling_factor << 4 |
		components[2].vertical_sampling_factor;

	switch (index) {
	case 0x221111:
		subsampling = VE_DEC_MPEG_TRIGGER_CHROMA_FMT_420;
		break;
	case 0x211111:
		subsampling = VE_DEC_MPEG_TRIGGER_CHROMA_FMT_422;
		break;
	case 0x111111:
		subsampling = VE_DEC_MPEG_TRIGGER_CHROMA_FMT_444;
		break;
	case 0x121111:
		subsampling = VE_DEC_MPEG_TRIGGER_CHROMA_FMT_422T;
		break;
	default:
		v4l2_err(&dev->v4l2_dev, "unsupported subsampling\n");
		return -EINVAL;
	}

	ctx->codec.jpeg.subsampling = subsampling;

	/* Activate MPEG engine and select JPEG subengine. */
	cedrus_engine_enable(ctx);

	reg = VE_DEC_MPEG_TRIGGER_JPEG | subsampling;
	cedrus_write(dev, VE_DEC_MPEG_TRIGGER, reg);

	/* Set restart interval. */
	cedrus_write(dev, VE_DEC_MPEG_JPEG_RES_INT, header.restart_interval);

	/* Set resolution in blocks. */
	hmax = components[0].horizontal_sampling_factor;
	vmax = components[0].vertical_sampling_factor;
	for (index = 1; index < 3; index++) {
		if (hmax < components[index].horizontal_sampling_factor)
			hmax = components[index].horizontal_sampling_factor;
		if (vmax < components[index].vertical_sampling_factor)
			vmax = components[index].vertical_sampling_factor;
	}

	reg = VE_DEC_MPEG_JPEG_SIZE_WIDTH(DIV_ROUND_UP(header.frame.width, 8 * hmax));
	reg |= VE_DEC_MPEG_JPEG_SIZE_HEIGHT(DIV_ROUND_UP(header.frame.height, 8 * vmax));
	cedrus_write(dev, VE_DEC_MPEG_JPEG_SIZE, reg);

	/* Set intra quantisation matrix. */
	index = components[0].quantization_table_selector;
	ret = cedrus_write_quantization_matrix(dev, VE_DEC_MPEG_IQMINPUT_FLAG_INTRA,
					       &quantization_tables[index]);
	if (ret)
		return ret;

	/* Set non-intra quantisation matrix. */
	index = components[1].quantization_table_selector;
	ret = cedrus_write_quantization_matrix(dev, VE_DEC_MPEG_IQMINPUT_FLAG_NON_INTRA,
					       &quantization_tables[index]);
	if (ret)
		return ret;

	/* Set Diffie-Huffman tables. */
	ret = cedrus_jpeg_write_dh_tables(dev, &header);
	if (ret)
		return ret;

	/* Destination luma and chroma buffers. */

	dst_luma_addr = cedrus_dst_buf_addr(ctx, &run->dst->vb2_buf, 0);
	dst_chroma_addr = cedrus_dst_buf_addr(ctx, &run->dst->vb2_buf, 1);

	/* JPEG outputs to rotation/scale down output buffers */
	cedrus_write(dev, VE_DEC_MPEG_ROT_LUMA, dst_luma_addr);
	cedrus_write(dev, VE_DEC_MPEG_ROT_CHROMA, dst_chroma_addr);

	/* Disable rotation and scaling. */
	cedrus_write(dev, VE_DEC_MPEG_SD_ROT_DBLK_CTL, 0);

	/* Source offset and length in bits. */

	cedrus_write(dev, VE_DEC_MPEG_VLD_OFFSET, 8 * header.ecs_offset);

	reg = size * 8;
	cedrus_write(dev, VE_DEC_MPEG_VLD_LEN, reg);

	/* Source beginning and end addresses. */

	src_buf_addr = vb2_dma_contig_plane_dma_addr(src_buf, 0);

	reg = VE_DEC_MPEG_VLD_ADDR_BASE(src_buf_addr);
	reg |= VE_DEC_MPEG_VLD_ADDR_VALID_PIC_DATA;
	reg |= VE_DEC_MPEG_VLD_ADDR_LAST_PIC_DATA;
	reg |= VE_DEC_MPEG_VLD_ADDR_FIRST_PIC_DATA;

	cedrus_write(dev, VE_DEC_MPEG_VLD_ADDR, reg);

	reg = src_buf_addr + size;
	cedrus_write(dev, VE_DEC_MPEG_VLD_END_ADDR, reg);

	/* Enable appropriate interrupts and components. */

	reg = VE_DEC_MPEG_CTRL_IRQ_MASK;
	if (subsampling == VE_DEC_MPEG_TRIGGER_CHROMA_FMT_422 ||
	    subsampling == VE_DEC_MPEG_TRIGGER_CHROMA_FMT_422T ||
	    subsampling == VE_DEC_MPEG_TRIGGER_CHROMA_FMT_444)
		reg |= VE_DEC_MPEG_CTRL_JPEG_FORCE_420;

	cedrus_write(dev, VE_DEC_MPEG_CTRL, reg);

	return 0;
}

static void cedrus_jpeg_trigger(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;
	u32 reg;

	/* Trigger JPEG engine. */
	reg = VE_DEC_MPEG_TRIGGER_HW_JPEG_VLD | VE_DEC_MPEG_TRIGGER_JPEG;
	reg |= ctx->codec.jpeg.subsampling;

	cedrus_write(dev, VE_DEC_MPEG_TRIGGER, reg);
}

struct cedrus_dec_ops cedrus_dec_ops_jpeg = {
	.irq_clear	= cedrus_jpeg_irq_clear,
	.irq_disable	= cedrus_jpeg_irq_disable,
	.irq_status	= cedrus_jpeg_irq_status,
	.setup		= cedrus_jpeg_setup,
	.trigger	= cedrus_jpeg_trigger,
};
