// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Cedrus VPU driver
 *
 * Copyright (c) 2020 Jernej Skrabec <jernej.skrabec@siol.net>
 */

#include <linux/delay.h>
#include <linux/types.h>

#include <media/videobuf2-dma-contig.h>

#include "cedrus.h"
#include "cedrus_hw.h"
#include "cedrus_regs.h"

/*
 * Auxiliary buffers, allocated as one block as the vendor library does:
 * the bitplanes, the DC/AC prediction row and the co-located motion
 * vectors which B pictures read for direct mode.
 */
#define VC1_AUX_ALLOC_SIZE		(144 * SZ_1K)
#define VC1_AUX_BPLANE_OFF		0x0000
#define VC1_AUX_DCAC_OFF		0x4000
#define VC1_AUX_MV_OFF			0x8000
#define VC1_IC_LUMSCALE_DEFAULT		32

/* Size in bits of the start code which begins an Advanced profile buffer */
#define VC1_START_CODE_BITS		32
/* Size in bits of SLICE_ADDR and PIC_HEADER_FLAG */
#define VC1_SLICE_HEADER_BITS		10

#define VC1_BITPLANE_OFFSET_ACPRED	0x0000
#define VC1_BITPLANE_OFFSET_OVERFLAGS	0x0400
#define VC1_BITPLANE_OFFSET_MVTYPEMB	0x0800
#define VC1_BITPLANE_OFFSET_SKIPMB	0x0C00
#define VC1_BITPLANE_OFFSET_DIRECTMB	0x1000
#define VC1_BITPLANE_OFFSET_FIELDTX	0x1400
#define VC1_BITPLANE_OFFSET_FORWARDMB	0x1800

#define BFRAC_HALF			128

/*
 * Pre-computed scales (base=256) of the fractions of Table 40 "BFRACTION VLC
 * Table" of SMPTE 421M, indexed by v4l2_ctrl_vc1_picture_layer.bfraction.
 */
static const u8 vc1_fractions[V4L2_VC1_BFRACTION_NUM] = {
	128 /* 1/2 */,  85 /* 1/3 */, 170 /* 2/3 */,  64 /* 1/4 */,
	192 /* 3/4 */,  51 /* 1/5 */, 102 /* 2/5 */, 153 /* 3/5 */,
	204 /* 4/5 */,  43 /* 1/6 */, 215 /* 5/6 */,  37 /* 1/7 */,
	 74 /* 2/7 */, 111 /* 3/7 */, 148 /* 4/7 */, 185 /* 5/7 */,
	222 /* 6/7 */,  32 /* 1/8 */,  96 /* 3/8 */, 160 /* 5/8 */,
	224 /* 7/8 */,
};

/*
 * Map the VC-1 MVMODE to the hardware PICMV MVMODE code, indexed by the mv mode
 * enum value (1MV_HPEL_BILIN=0, 1MV=1, 1MV_HPEL=2, MIXED_MV=3). Per the vendor
 * library (mMVModeTable): 1MV_HPEL_BILIN -> 3, 1MV -> 0, 1MV_HPEL -> 2,
 * MIXED_MV -> 1.
 */
static const unsigned int vc1_mvmode_map[] = {3, 0, 2, 1};

static unsigned int cedrus_vc1_get_fraction(unsigned int index)
{
	if (index >= ARRAY_SIZE(vc1_fractions))
		return vc1_fractions[ARRAY_SIZE(vc1_fractions) - 1];

	return vc1_fractions[index];
}

/*
 * Pictures whose intensity compensation can apply to a field of a reference,
 * in the order in which the compensations are applied (section 8.3.8 and
 * 10.3.8 of SMPTE 421M), and the hardware register which takes each of them.
 */
enum cedrus_vc1_ic_source {
	/* second field of the reference frame, on its first field: PICICBAK1 */
	CEDRUS_VC1_IC_SELF,
	/* first field (or frame) of the next P picture: PICICBAK0 */
	CEDRUS_VC1_IC_FIRST,
	/* second field of the next P picture: PICINTENCOMP of B fields */
	CEDRUS_VC1_IC_SECOND,
	CEDRUS_VC1_IC_SOURCES,
};

/* Intensity compensation of the two fields (0 top, 1 bottom) of a frame. */
struct cedrus_vc1_ic {
	unsigned int			mask;
	struct v4l2_vc1_intcomp		param[2];
};

struct cedrus_vc1_ic_regs {
	u32		intencomp;
	u32		icbak0;
	u32		icbak1;
	unsigned int	intencompfld;
	bool		enable;
};

static void cedrus_vc1_ic_add(struct cedrus_vc1_ic *ic, unsigned int field,
			      u8 lumscale, u8 lumshift)
{
	ic->mask |= BIT(field);
	ic->param[field].lumscale = lumscale;
	ic->param[field].lumshift = lumshift;
}

/*
 * Register layout shared by PICINTENCOMP and the PICICBAK registers: one
 * LUMSCALE/LUMSHIFT slot per field, the default scale for a field which is
 * not compensated, and the mask of the compensated fields.
 */
static u32 cedrus_vc1_ic_reg(const struct cedrus_vc1_ic *ic)
{
	u32 reg;

	if (!ic->mask)
		return 0;

	if (ic->mask & BIT(0))
		reg = VE_DEC_VC1_PICINTENCOMP_LUMASCALE1(ic->param[0].lumscale) |
		      VE_DEC_VC1_PICINTENCOMP_LUMASHIFT1(ic->param[0].lumshift);
	else
		reg = VE_DEC_VC1_PICINTENCOMP_LUMASCALE1(VC1_IC_LUMSCALE_DEFAULT);

	if (ic->mask & BIT(1))
		reg |= VE_DEC_VC1_PICINTENCOMP_LUMASCALE2(ic->param[1].lumscale) |
		       VE_DEC_VC1_PICINTENCOMP_LUMASHIFT2(ic->param[1].lumshift);
	else
		reg |= VE_DEC_VC1_PICINTENCOMP_LUMASCALE2(VC1_IC_LUMSCALE_DEFAULT);

	return reg | VE_DEC_VC1_PICINTENCOMP_FIELD(ic->mask);
}

/*
 * The reference description lists the compensations of a field in the order
 * in which they apply, without saying which picture signalled each of them.
 * That follows from which pictures could have compensated the field
 * (@possible, a mask of sources). If fewer are listed than could have been,
 * the most recent pictures are taken to be the ones which did.
 */
static void cedrus_vc1_ic_split(const struct v4l2_vc1_reference *ref,
				unsigned int field, unsigned int possible,
				struct cedrus_vc1_ic ic[CEDRUS_VC1_IC_SOURCES])
{
	unsigned int num = ref->num_intcomp[field];
	unsigned int skip, src, i = 0;

	if (num > hweight32(possible))
		possible = GENMASK(CEDRUS_VC1_IC_SOURCES - 1, 0);

	skip = hweight32(possible) - num;

	for (src = 0; src < CEDRUS_VC1_IC_SOURCES && i < num; src++) {
		if (!(possible & BIT(src)))
			continue;

		if (skip) {
			skip--;
			continue;
		}

		cedrus_vc1_ic_add(&ic[src], field, ref->intcomp[field][i].lumscale,
				  ref->intcomp[field][i].lumshift);
		i++;
	}
}

/*
 * Intensity compensation registers of the picture, from its own syntax
 * elements and from the descriptions of its references. The hardware
 * takes, per reference field, up to three compensations in separate
 * registers (see enum cedrus_vc1_ic_source), which the vendor library
 * fills from what it saved of the previous pictures.
 */
static void cedrus_vc1_ic_setup(const struct v4l2_ctrl_vc1_picture_layer *picture,
				unsigned int parity, bool second_field,
				struct cedrus_vc1_ic_regs *regs)
{
	const struct v4l2_vc1_reference *fwd = &picture->forward_ref;
	const struct v4l2_vc1_reference *bwd = &picture->backward_ref;
	bool field_pic = picture->fcm == V4L2_VC1_FCM_FIELD_INTERLACE;
	unsigned int fwd_first = fwd->flags & V4L2_VC1_REFERENCE_FLAG_TFF ? 0 : 1;
	unsigned int bwd_first = bwd->flags & V4L2_VC1_REFERENCE_FLAG_TFF ? 0 : 1;
	struct cedrus_vc1_ic ic[CEDRUS_VC1_IC_SOURCES] = {};
	struct cedrus_vc1_ic cur = {};
	unsigned int field, fields, possible;
	int src;

	memset(regs, 0, sizeof(*regs));

	if (picture->ptype == V4L2_VC1_PICTURE_TYPE_P) {
		if (field_pic) {
			if (picture->mvmode == V4L2_VC1_MVMODE_INTENSITY_COMP) {
				/*
				 * LUMSCALE1/LUMSHIFT1 are for the only field
				 * compensated, or for the top one if both are.
				 */
				switch (picture->intcompfield) {
				case V4L2_VC1_INTCOMPFIELD_TOP:
					cedrus_vc1_ic_add(&cur, 0, picture->lumscale,
							  picture->lumshift);
					break;
				case V4L2_VC1_INTCOMPFIELD_BOTTOM:
					cedrus_vc1_ic_add(&cur, 1, picture->lumscale,
							  picture->lumshift);
					break;
				default:
					cedrus_vc1_ic_add(&cur, 0, picture->lumscale,
							  picture->lumshift);
					cedrus_vc1_ic_add(&cur, 1, picture->lumscale2,
							  picture->lumshift2);
					break;
				}
			}
		} else if (picture->fcm == V4L2_VC1_FCM_FRAME_INTERLACE ?
			   picture->flags & V4L2_VC1_PICTURE_LAYER_FLAG_INTCOMP :
			   picture->mvmode == V4L2_VC1_MVMODE_INTENSITY_COMP) {
			/* One LUMSCALE/LUMSHIFT pair, for both fields. */
			cedrus_vc1_ic_add(&cur, 0, picture->lumscale, picture->lumshift);
			cedrus_vc1_ic_add(&cur, 1, picture->lumscale, picture->lumshift);
			regs->enable = true;
		}

		regs->intencomp = cedrus_vc1_ic_reg(&cur) & 0x3f3f3f3f;
		regs->intencompfld = cur.mask;

		/* A P frame cannot express the history of its reference. */
		if (!field_pic)
			return;
	} else if (picture->ptype != V4L2_VC1_PICTURE_TYPE_B) {
		return;
	}

	/*
	 * Fields of the forward reference which the picture reads: a second
	 * field reads the first field of its own frame instead of the forward
	 * reference field of the other parity.
	 */
	fields = second_field ? BIT(parity) : GENMASK(1, 0);

	for (field = 0; field < 2; field++) {
		if (!(fields & BIT(field)))
			continue;

		possible = 0;

		if (fwd->fcm == V4L2_VC1_FCM_FIELD_INTERLACE &&
		    field == fwd_first &&
		    fwd->ptype[!field] == V4L2_VC1_PICTURE_TYPE_P)
			possible |= BIT(CEDRUS_VC1_IC_SELF);

		if (picture->ptype == V4L2_VC1_PICTURE_TYPE_P) {
			/* The next P picture is the frame being decoded. */
			if (second_field &&
			    (picture->fptype == V4L2_VC1_FPTYPE_P_P ||
			     picture->fptype == V4L2_VC1_FPTYPE_P_I))
				possible |= BIT(CEDRUS_VC1_IC_FIRST);
		} else if (bwd->fcm == V4L2_VC1_FCM_FIELD_INTERLACE) {
			/* The next P picture is the backward reference. */
			if (bwd->ptype[bwd_first] == V4L2_VC1_PICTURE_TYPE_P)
				possible |= BIT(CEDRUS_VC1_IC_FIRST);
			if (field != bwd_first &&
			    bwd->ptype[!bwd_first] == V4L2_VC1_PICTURE_TYPE_P)
				possible |= BIT(CEDRUS_VC1_IC_SECOND);
		} else if (bwd->ptype[0] == V4L2_VC1_PICTURE_TYPE_P) {
			possible |= BIT(CEDRUS_VC1_IC_FIRST);
		}

		cedrus_vc1_ic_split(fwd, field, possible, ic);
	}

	if (picture->ptype == V4L2_VC1_PICTURE_TYPE_P) {
		regs->icbak1 = cedrus_vc1_ic_reg(&ic[CEDRUS_VC1_IC_SELF]);
		regs->icbak0 = cedrus_vc1_ic_reg(&ic[CEDRUS_VC1_IC_FIRST]);
		return;
	}

	if (field_pic) {
		/*
		 * The second field of the backward reference also signalled
		 * the compensation of the first field of that frame, which a
		 * B field reads as a backward reference field. The vendor
		 * library programs both in PICINTENCOMP.
		 */
		if (bwd->fcm == V4L2_VC1_FCM_FIELD_INTERLACE &&
		    bwd->num_intcomp[bwd_first])
			cedrus_vc1_ic_add(&ic[CEDRUS_VC1_IC_SECOND], bwd_first,
					  bwd->intcomp[bwd_first][0].lumscale,
					  bwd->intcomp[bwd_first][0].lumshift);

		regs->icbak1 = cedrus_vc1_ic_reg(&ic[CEDRUS_VC1_IC_SELF]);
		regs->icbak0 = cedrus_vc1_ic_reg(&ic[CEDRUS_VC1_IC_FIRST]);
		regs->intencomp = cedrus_vc1_ic_reg(&ic[CEDRUS_VC1_IC_SECOND]) &
				  0x3f3f3f3f;
		regs->intencompfld = ic[CEDRUS_VC1_IC_SECOND].mask;
		return;
	}

	/*
	 * B frame: a single compensation per field, the one which the next P
	 * picture signalled (the vendor library keeps the register of the
	 * last P frame).
	 */
	for (field = 0; field < 2; field++) {
		for (src = CEDRUS_VC1_IC_SECOND; src >= CEDRUS_VC1_IC_FIRST; src--) {
			if (!(ic[src].mask & BIT(field)))
				continue;

			cedrus_vc1_ic_add(&cur, field, ic[src].param[field].lumscale,
					  ic[src].param[field].lumshift);
			break;
		}
	}

	regs->intencomp = cedrus_vc1_ic_reg(&cur) & 0x3f3f3f3f;
	regs->enable = !!cur.mask;
}

/*
 * The hardware takes each bitplane in two 1 KiB halves: the first one at
 * the offset of the plane, the second one 7 KiB further, after the first
 * halves of all seven planes.
 */
#define VC1_BITPLANE_HALF_SIZE		SZ_1K
#define VC1_BITPLANE_SECOND_HALF	(7 * VC1_BITPLANE_HALF_SIZE)

static const struct {
	u8	flag;
	u16	offset;
	u16	member;
} cedrus_vc1_bitplanes[] = {
#define VC1_BITPLANE(f, m) { V4L2_VC1_BITPLANE_FLAG_##f, VC1_BITPLANE_OFFSET_##f, \
			     offsetof(struct v4l2_ctrl_vc1_bitplanes, m) }
	VC1_BITPLANE(MVTYPEMB, mvtypemb),
	VC1_BITPLANE(DIRECTMB, directmb),
	VC1_BITPLANE(SKIPMB, skipmb),
	VC1_BITPLANE(FIELDTX, fieldtx),
	VC1_BITPLANE(FORWARDMB, forwardmb),
	VC1_BITPLANE(ACPRED, acpred),
	VC1_BITPLANE(OVERFLAGS, overflags),
#undef VC1_BITPLANE
};

/*
 * Coded size of the picture: a Simple or Main profile picture of a MULTIRES
 * sequence may be coded at half width and/or half height (RESPIC).
 */
static void cedrus_vc1_coded_size(struct cedrus_ctx *ctx,
				  struct cedrus_run *run,
				  unsigned int *width, unsigned int *height)
{
	const struct v4l2_ctrl_vc1_sequence *sequence = run->vc1.sequence;
	const struct v4l2_ctrl_vc1_picture_layer *picture = run->vc1.picture;

	*width = ctx->src_fmt.width;
	*height = ctx->src_fmt.height;
	if (sequence->profile == V4L2_VC1_PROFILE_ADVANCED ||
	    !(sequence->flags & V4L2_VC1_SEQUENCE_FLAG_MULTIRES))
		return;
	if (picture->respic & 1)
		*width /= 2;
	if (picture->respic & 2)
		*height /= 2;
}

static void cedrus_vc1_bitplanes_setup(struct cedrus_ctx *ctx,
				       struct cedrus_run *run)
{
	const struct v4l2_ctrl_vc1_picture_layer *picture = run->vc1.picture;
	const u8 *planes = (const u8 *)run->vc1.bitplanes;
	unsigned int mb_width, mb_height, size, first, i, width, height;
	u8 *buf = ctx->codec.vc1.bitplanes_buf;

	/* One bit per macroblock of the picture, or of the field. */
	cedrus_vc1_coded_size(ctx, run, &width, &height);
	mb_width = DIV_ROUND_UP(width, 16);
	mb_height = DIV_ROUND_UP(height, 16);
	if (picture->fcm == V4L2_VC1_FCM_FIELD_INTERLACE)
		mb_height = DIV_ROUND_UP(mb_height, 2);
	size = min_t(unsigned int, DIV_ROUND_UP(mb_width * mb_height, 8),
		     V4L2_VC1_BITPLANE_SIZE);
	first = min_t(unsigned int, size, VC1_BITPLANE_HALF_SIZE);

	for (i = 0; i < ARRAY_SIZE(cedrus_vc1_bitplanes); i++) {
		const u8 *plane = planes + cedrus_vc1_bitplanes[i].member;
		u8 *dst = buf + cedrus_vc1_bitplanes[i].offset;

		if (!(picture->bitplane_flags & cedrus_vc1_bitplanes[i].flag))
			continue;

		memcpy(dst, plane, first);
		if (size > first)
			memcpy(dst + VC1_BITPLANE_SECOND_HALF,
			       plane + VC1_BITPLANE_HALF_SIZE, size - first);
	}
}

static enum cedrus_irq_status
cedrus_vc1_irq_status(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;
	u32 reg = cedrus_read(dev, VE_DEC_VC1_STATUS);

	if (reg & (VE_DEC_VC1_STATUS_ERROR |
		   VE_DEC_VC1_STATUS_VLD_DATA_REQ))
		return CEDRUS_IRQ_ERROR;

	if (reg & VE_DEC_VC1_STATUS_SUCCESS)
		return CEDRUS_IRQ_OK;

	return CEDRUS_IRQ_NONE;
}

static void cedrus_vc1_irq_clear(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;

	cedrus_write(dev, VE_DEC_VC1_STATUS,
		     VE_DEC_VC1_STATUS_INT_MASK);
}

static void cedrus_vc1_irq_disable(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;
	u32 reg = cedrus_read(dev, VE_DEC_VC1_CTRL);

	cedrus_write(dev, VE_DEC_VC1_CTRL,
		     reg & ~VE_DEC_VC1_CTRL_IRQ_MASK);
}

static int cedrus_vc1_setup(struct cedrus_ctx *ctx, struct cedrus_run *run)
{
	bool interlaced, top_field_first, second_field, ref_field, rangeredfrm;
	const struct v4l2_ctrl_vc1_picture_layer *picture = run->vc1.picture;
	const struct v4l2_ctrl_vc1_entrypoint_header *entrypoint;
	const struct v4l2_ctrl_vc1_sequence *sequence;
	const struct v4l2_vc1_vopdquant *vopdquant;
	struct vb2_buffer *src_buf = &run->src->vb2_buf;
	unsigned int bfraction, frfd, mvmode, parity;
	dma_addr_t dst_luma_addr, dst_chroma_addr;
	bool fwd_interlaced, bwd_interlaced;
	struct cedrus_vc1_ic_regs ic;
	struct cedrus_dev *dev = ctx->dev;
	struct vb2_buffer *backward_vb2;
	struct vb2_buffer *forward_vb2;
	unsigned int raw_coding, skip, bits, sc_offset, pichdrlen, i;
	unsigned int width, height;
	dma_addr_t src_buf_addr;
	u32 reg, condover, pq;
	struct vb2_queue *vq;
	size_t buf_size;
	bool advanced;
	int brfd, flag;

	sequence = run->vc1.sequence;
	entrypoint = run->vc1.entrypoint;
	vopdquant = &picture->vopdquant;

	/*
	 * The hardware wants the "raw" bit of every bitplane which is not
	 * passed decoded, including the ones the picture does not have
	 * (e.g. ACPRED of Simple/Main I pictures, which is coded per
	 * macroblock).
	 */
	raw_coding = ~picture->bitplane_flags;

	advanced = sequence->profile == V4L2_VC1_PROFILE_ADVANCED;
	second_field = !!(picture->flags & V4L2_VC1_PICTURE_LAYER_FLAG_SECOND_FIELD);
	top_field_first = !!(picture->flags & V4L2_VC1_PICTURE_LAYER_FLAG_TFF);
	interlaced = picture->fcm != V4L2_VC1_FCM_PROGRESSIVE;
	ref_field = !!(picture->flags & V4L2_VC1_PICTURE_LAYER_FLAG_REFFIELD);
	/* Parity of the field being decoded (0 top, 1 bottom), 0 for frames. */
	parity = picture->fcm == V4L2_VC1_FCM_FIELD_INTERLACE &&
		 !(top_field_first ^ second_field);
	/*
	 * Only the Main profile range reduction. The Advanced profile range
	 * mapping (RANGE_MAPY/RANGE_MAPUV) is an output process which the
	 * uAPI leaves to the application.
	 */
	rangeredfrm = !!(picture->flags & V4L2_VC1_PICTURE_LAYER_FLAG_RANGEREDFRM);
	pq = picture->pquant;

	if (picture->ptype != V4L2_VC1_PICTURE_TYPE_P)
		flag = false;
	else if (picture->fcm == V4L2_VC1_FCM_FRAME_INTERLACE)
		flag = !!(picture->flags & V4L2_VC1_PICTURE_LAYER_FLAG_INTCOMP);
	else if (picture->mvmode == V4L2_VC1_MVMODE_INTENSITY_COMP)
		flag = true;
	else
		flag = false;

	cedrus_vc1_ic_setup(picture, parity, second_field, &ic);

	/*
	 * The state of the references comes from their descriptions in the
	 * picture layer control. Without a reference the hardware is pointed
	 * at the picture being decoded, so its own state is used.
	 */
	if (picture->ptype == V4L2_VC1_PICTURE_TYPE_P ||
	    picture->ptype == V4L2_VC1_PICTURE_TYPE_B)
		fwd_interlaced = picture->forward_ref.fcm != V4L2_VC1_FCM_PROGRESSIVE;
	else
		fwd_interlaced = interlaced;

	if (picture->ptype == V4L2_VC1_PICTURE_TYPE_B)
		bwd_interlaced = picture->backward_ref.fcm != V4L2_VC1_FCM_PROGRESSIVE;
	else
		bwd_interlaced = interlaced;

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE);

	/*
	 * For interlaced-field frames whose first field is intra-coded, the
	 * second field references the first field of the same frame. When
	 * the bitstream cannot refer to anything else (one reference field,
	 * the most recent one), point the hardware at the frame being decoded
	 * rather than at the previous reference frame. A timestamp of 0 means
	 * no reference.
	 */
	if (second_field &&
	    (picture->fptype == V4L2_VC1_FPTYPE_I_I ||
	     picture->fptype == V4L2_VC1_FPTYPE_I_P) &&
	    !(picture->flags & (V4L2_VC1_PICTURE_LAYER_FLAG_NUMREF |
				V4L2_VC1_PICTURE_LAYER_FLAG_REFFIELD))) {
		forward_vb2 = NULL;
		backward_vb2 = NULL;
		fwd_interlaced = interlaced;
	} else {
		forward_vb2 = picture->forward_ref_ts ?
			vb2_find_buffer(vq, picture->forward_ref_ts) : NULL;
		backward_vb2 = picture->backward_ref_ts ?
			vb2_find_buffer(vq, picture->backward_ref_ts) : NULL;
	}

	cedrus_engine_enable(ctx);

	/*
	 * Program EPHS first so that start-code / emulation-prevention-byte
	 * (EPTB) detection is enabled before any bitstream is consumed. The
	 * data_bit_offset skip below counts EPTB-removed (RBSP) bits, matching
	 * the value ffmpeg reports, so the hardware bit reader must strip the
	 * 0x000003 emulation bytes while skipping. If EPHS is programmed only
	 * after the skip, headers containing emulation-prevention bytes desync
	 * the VLD and the picture decodes to garbage.
	 */
	reg = VE_DEC_VC1_EPHS_PROFILE(sequence->profile);
	if (entrypoint->flags & V4L2_VC1_ENTRYPOINT_HEADER_FLAG_LOOPFILTER)
		reg |= VE_DEC_VC1_EPHS_LOOPFILTER;
	if (sequence->flags & V4L2_VC1_SEQUENCE_FLAG_MULTIRES)
		reg |= VE_DEC_VC1_EPHS_MULTIRES;
	if (entrypoint->flags & V4L2_VC1_ENTRYPOINT_HEADER_FLAG_FASTUVMC)
		reg |= VE_DEC_VC1_EPHS_FASTUVMC;
	if (entrypoint->flags & V4L2_VC1_ENTRYPOINT_HEADER_FLAG_EXTENDED_DMV)
		reg |= VE_DEC_VC1_EPHS_EXTENDEDMV;
	reg |= VE_DEC_VC1_EPHS_DQUANT(entrypoint->dquant);
	if (entrypoint->flags & V4L2_VC1_ENTRYPOINT_HEADER_FLAG_VSTRANSFORM)
		reg |= VE_DEC_VC1_EPHS_VSTRANSFORM;
	/* See the CONDOVER handling below for interlaced frame pictures. */
	if ((entrypoint->flags & V4L2_VC1_ENTRYPOINT_HEADER_FLAG_OVERLAP) &&
	    picture->fcm != V4L2_VC1_FCM_FRAME_INTERLACE)
		reg |= VE_DEC_VC1_EPHS_OVERLAP;
	reg |= VE_DEC_VC1_EPHS_QUANTIZER(entrypoint->quantizer);
	if (sequence->flags & V4L2_VC1_SEQUENCE_FLAG_RANGERED)
		reg |= VE_DEC_VC1_EPHS_RANGERED;
	if (sequence->flags & V4L2_VC1_SEQUENCE_FLAG_FINTERPFLAG)
		reg |= VE_DEC_VC1_EPHS_FINTERPFLAG;
	if (sequence->flags & V4L2_VC1_SEQUENCE_FLAG_SYNCMARKER)
		reg |= VE_DEC_VC1_EPHS_SYNCMARKER;
	if (advanced)
		reg |= VE_DEC_VC1_EPHS_STARTCODE_DET_EN;
	else
		reg |= VE_DEC_VC1_EPHS_EPTB_DET_BYPASS;
	cedrus_write(dev, VE_DEC_VC1_EPHS, reg);

	/* Set bitstream source.
	 *
	 * The vendor keeps a multi-frame VBV buffer permanently mapped;
	 * any over-read beyond the current frame lands on valid data of
	 * the next frame.  The stateless API provides one frame per
	 * buffer - limit BITS_LEN to the actual frame size so the
	 * VLD never fetches uninitialized memory past the frame end.
	 * Keep BITS_END_ADDR at the page-rounded allocation boundary as
	 * a safety net against runaway DMA (watchdog timeout).
	 */
	buf_size = vb2_plane_size(src_buf, 0);
	/*
	 * An Advanced profile buffer begins with the frame or field start code
	 * (00 00 01 0D/0C), which data_bit_offset does not count. Start the
	 * bit reader after it: with start-code detection enabled, reading over
	 * a start code with GET_BITS desyncs the VLD (every picture failed on
	 * H616).
	 */
	sc_offset = advanced ? VC1_START_CODE_BITS : 0;
	cedrus_write(dev, VE_DEC_VC1_BITS_LEN,
		     vb2_get_plane_payload(src_buf, 0) * 8 - sc_offset);
	cedrus_write(dev, VE_DEC_VC1_BITS_OFFSET, sc_offset);

	src_buf_addr = vb2_dma_contig_plane_dma_addr(src_buf, 0);
	cedrus_write(dev, VE_DEC_VC1_BITS_END_ADDR,
		     src_buf_addr + buf_size);
	cedrus_write(dev, VE_DEC_VC1_BITS_ADDR,
		     VE_DEC_VC1_BITS_ADDR_BASE(src_buf_addr) |
		     VE_DEC_VC1_BITS_ADDR_VALID_SLICE_DATA |
		     VE_DEC_VC1_BITS_ADDR_LAST_SLICE_DATA |
		     VE_DEC_VC1_BITS_ADDR_FIRST_SLICE_DATA);

	/* Set auxiliary buffers */

	cedrus_write(dev, VE_DEC_VC1_DCACPRED_ADDR,
		     ctx->codec.vc1.acdc_buf_addr);
	cedrus_write(dev, VE_DEC_VC1_BITPLANE_ADDR,
		     ctx->codec.vc1.bitplanes_buf_addr);
	cedrus_write(dev, VE_DEC_VC1_MVINFO_ADDR,
		     ctx->codec.vc1.mv_buf_addr);

	cedrus_write(dev, VE_DEC_VC1_STATUS,
		     VE_DEC_VC1_STATUS_INT_MASK);

	cedrus_write(dev, VE_DEC_VC1_TRIGGER_TYPE,
		     VE_DEC_VC1_TRIGGER_TYPE_INIT_SWDEC);

	/* Skip the picture header to the first macroblock. */
	skip = picture->data_bit_offset;

	/*
	 * Flush up to 24 bits per GET_BITS trigger: the engine's bit reader
	 * handles <= 24-bit reads (the vendor reads 24-bit start codes the same
	 * way) and strips emulation-prevention bytes, so this matches a 1-bit
	 * loop but is far fewer register writes. Wider (e.g. 32-bit) reads
	 * overrun the bit FIFO and desync the VLD.
	 */
	for (reg = 0; reg < skip; reg += bits) {
		bits = min_t(u32, skip - reg, 24);
		cedrus_write(dev, VE_DEC_VC1_TRIGGER_TYPE,
			     VE_DEC_VC1_TRIGGER_TYPE_GET_BITS |
			     VE_DEC_VC1_TRIGGER_TYPE_N_BITS(bits));

		cedrus_wait_for(dev, VE_DEC_VC1_STATUS, VE_DEC_VC1_STATUS_BITS_BUSY);
	}

	cedrus_write(dev, VE_DEC_VC1_ROT_CTRL, 0);

	/*
	 * The bit reader handles the slices of an Advanced profile picture by
	 * itself: it detects the slice start code, reads SLICE_ADDR and
	 * PIC_HEADER_FLAG, and skips PICHDRLEN bits of repeated picture header
	 * when the flag is set. The repeated header has the same length in all
	 * the slices of a picture. It follows the 10 bits of SLICE_ADDR and
	 * PIC_HEADER_FLAG, which data_bit_offset of the slice also counts.
	 */
	pichdrlen = 0;
	for (i = 1; i < run->vc1.num_slices; i++) {
		const struct v4l2_ctrl_vc1_slice_params *slice = &run->vc1.slices[i];

		if ((slice->flags & V4L2_VC1_SLICE_PARAMS_FLAG_PIC_HEADER) &&
		    slice->data_bit_offset >= VC1_SLICE_HEADER_BITS) {
			pichdrlen = slice->data_bit_offset - VC1_SLICE_HEADER_BITS;
			break;
		}
	}
	cedrus_write(dev, VE_DEC_VC1_PICHDRLEN,
		     VE_DEC_VC1_PICHDRLEN_LENGTH(pichdrlen));

	/*
	 * CONDOVER is only coded in Advanced profile I and BI pictures; the
	 * hardware also wants the value which applies to the other pictures.
	 */
	if (!(entrypoint->flags & V4L2_VC1_ENTRYPOINT_HEADER_FLAG_OVERLAP) ||
	    picture->ptype == V4L2_VC1_PICTURE_TYPE_B)
		condover = V4L2_VC1_CONDOVER_NONE;
	else if (pq >= 9)
		/* No CONDOVER is coded, overlap applies to all macroblocks. */
		condover = V4L2_VC1_CONDOVER_ALL;
	else if (advanced &&
		 (picture->ptype == V4L2_VC1_PICTURE_TYPE_I ||
		  picture->ptype == V4L2_VC1_PICTURE_TYPE_BI))
		condover = picture->condover;
	else
		condover = V4L2_VC1_CONDOVER_NONE;

	/*
	 * With overlap smoothing active, the hardware writes garbage for
	 * interlaced frame pictures (every block, not just the smoothed
	 * edges), whatever the CONDOVER and buffer setup. Decode them without
	 * overlap smoothing, which only leaves the vertical block edges
	 * unsmoothed. Interlaced encoders don't seem to use overlap anyway.
	 */
	if (picture->fcm == V4L2_VC1_FCM_FRAME_INTERLACE &&
	    condover != V4L2_VC1_CONDOVER_NONE) {
		dev_warn_once(dev->dev,
			      "VC-1 overlap smoothing in interlaced frame pictures is not supported\n");
		condover = V4L2_VC1_CONDOVER_NONE;
	}

	reg = VE_DEC_VC1_PICCTRL_PTYPE(picture->ptype);
	reg |= VE_DEC_VC1_PICCTRL_FCM(picture->fcm ? picture->fcm + 1 : 0);
	if (interlaced && !(top_field_first ^ second_field))
		reg |= VE_DEC_VC1_PICCTRL_BOTTOM_FIELD;
	if (second_field)
		reg |= VE_DEC_VC1_PICCTRL_SECOND_FIELD;
	if (rangeredfrm)
		reg |= VE_DEC_VC1_PICCTRL_RANGEREDFRM;
	if (picture->forward_ref.flags & V4L2_VC1_REFERENCE_FLAG_RANGEREDFRM)
		reg |= VE_DEC_VC1_PICCTRL_FWD_RANGEREDFRM;
	if (picture->backward_ref.flags & V4L2_VC1_REFERENCE_FLAG_RANGEREDFRM)
		reg |= VE_DEC_VC1_PICCTRL_BWD_RANGEREDFRM;
	reg |= VE_DEC_VC1_PICCTRL_TRANSACFRM(picture->transacfrm);
	reg |= VE_DEC_VC1_PICCTRL_TRANSACFRM2(picture->transacfrm2);
	if (picture->flags & V4L2_VC1_PICTURE_LAYER_FLAG_TRANSDCTAB)
		reg |= VE_DEC_VC1_PICCTRL_TRANSDCTAB;
	if (picture->flags & V4L2_VC1_PICTURE_LAYER_FLAG_RNDCTRL)
		reg |= VE_DEC_VC1_PICCTRL_RNDCTRL;
	reg |= VE_DEC_VC1_PICCTRL_CONDOVER(condover ? condover + 1 : 0);
	if (raw_coding & V4L2_VC1_RAW_CODING_FLAG_ACPRED)
		reg |= VE_DEC_VC1_PICCTRL_ACPRED_RAW;
	if (raw_coding & V4L2_VC1_RAW_CODING_FLAG_OVERFLAGS)
		reg |= VE_DEC_VC1_PICCTRL_OVERFLAGS_RAW;
	reg |= VE_DEC_VC1_PICCTRL_CBPTAB(picture->cbptab);
	if (raw_coding & V4L2_VC1_RAW_CODING_FLAG_SKIPMB)
		reg |= VE_DEC_VC1_PICCTRL_SKIPMB_RAW;
	if (picture->flags & V4L2_VC1_PICTURE_LAYER_FLAG_TTMBF)
		reg |= VE_DEC_VC1_PICCTRL_TTMBF;
	reg |= VE_DEC_VC1_PICCTRL_TTFRM(picture->ttfrm);
	if (raw_coding & V4L2_VC1_RAW_CODING_FLAG_DIRECTMB)
		reg |= VE_DEC_VC1_PICCTRL_DIRECTMB_RAW;
	/*
	 * Direct mode of B pictures: the co-located motion vectors are zero
	 * when the backward reference is intra coded or skipped. A skipped
	 * reference was never decoded, so the motion vector buffer still
	 * holds the vectors of an older picture and must not be used.
	 */
	if (picture->ptype == V4L2_VC1_PICTURE_TYPE_B) {
		if (picture->backward_ref.ptype[parity] != V4L2_VC1_PICTURE_TYPE_P)
			reg |= VE_DEC_VC1_PICCTRL_DIRECT_REF_INTRA;
	} else if (picture->ptype != V4L2_VC1_PICTURE_TYPE_P) {
		reg |= VE_DEC_VC1_PICCTRL_DIRECT_REF_INTRA;
	}
	if (picture->bitplane_flags)
		reg |= VE_DEC_VC1_PICCTRL_BITPL_CODING;
	cedrus_write(dev, VE_DEC_VC1_PICCTRL, reg);

	reg = VE_DEC_VC1_PICQP_PQINDEX(picture->pqindex);
	if (picture->flags & V4L2_VC1_PICTURE_LAYER_FLAG_HALFQP)
		reg |= VE_DEC_VC1_PICQP_HALFQP;
	if (picture->flags & V4L2_VC1_PICTURE_LAYER_FLAG_PQUANTIZER)
		reg |= VE_DEC_VC1_PICQP_PQUANTIZER;
	reg |= VE_DEC_VC1_PICQP_DQPPROFILE(vopdquant->dqprofile);
	reg |= VE_DEC_VC1_PICQP_DQSBEDGE(vopdquant->dqsbedge);
	reg |= VE_DEC_VC1_PICQP_DQDBEDGE(vopdquant->dqdbedge);
	reg |= VE_DEC_VC1_PICQP_ALTPQUANT(vopdquant->altpquant);
	if (vopdquant->flags & V4L2_VC1_VOPDQUANT_FLAG_DQUANTFRM)
		reg |= VE_DEC_VC1_PICQP_DQUANTFRM;
	if (vopdquant->flags & V4L2_VC1_VOPDQUANT_FLAG_DQBILEVEL)
		reg |= VE_DEC_VC1_PICQP_DQBILEVEL;
	cedrus_write(dev, VE_DEC_VC1_PICQP, reg);

	bfraction = cedrus_vc1_get_fraction(picture->bfraction);
	reg = VE_DEC_VC1_PICMV_BFRACTION(bfraction);
	if (bfraction < BFRAC_HALF)
		reg |= VE_DEC_VC1_PICMV_BFRAC_LESS_THAN_HALF;
	reg |= VE_DEC_VC1_PICMV_MVRANGE(picture->mvrange);
	/*
	 * Interlaced-frame pictures have no MVMODE syntax element, so derive
	 * it from the 4MVSWITCH flag as the vendor driver does: mixed-MV for a
	 * P picture with 4MVSWITCH set, 1MV otherwise. For P pictures using
	 * intensity compensation the actual motion mode is carried in mvmode2.
	 */
	if (picture->fcm == V4L2_VC1_FCM_FRAME_INTERLACE)
		mvmode = (picture->ptype == V4L2_VC1_PICTURE_TYPE_P &&
			  (picture->flags & V4L2_VC1_PICTURE_LAYER_FLAG_4MVSWITCH)) ?
			 V4L2_VC1_MVMODE_MIXED_MV : V4L2_VC1_MVMODE_1MV;
	else if (picture->ptype == V4L2_VC1_PICTURE_TYPE_P &&
		 picture->mvmode == V4L2_VC1_MVMODE_INTENSITY_COMP)
		mvmode = picture->mvmode2;
	else
		mvmode = picture->mvmode;
	reg |= VE_DEC_VC1_PICMV_MVMODE(vc1_mvmode_map[mvmode & 3]);
	if (ic.enable)
		reg |= VE_DEC_VC1_PICMV_INTENSITY_COMP_EN;
	reg |= VE_DEC_VC1_PICMV_MVTAB(picture->mvtab);
	if (raw_coding & V4L2_VC1_RAW_CODING_FLAG_MVTYPEMB)
		reg |= VE_DEC_VC1_PICMV_MVTYPEMB_RAW;
	cedrus_write(dev, VE_DEC_VC1_PICMV, reg);

	/*
	 * The order of the writes follows the vendor library: the backup
	 * registers form a stateful transaction. BI fields leave them alone.
	 */
	if (picture->fcm != V4L2_VC1_FCM_FIELD_INTERLACE) {
		cedrus_write(dev, VE_DEC_VC1_PICINTENCOMP, ic.intencomp);
	} else if (picture->ptype == V4L2_VC1_PICTURE_TYPE_B && !second_field) {
		cedrus_write(dev, VE_DEC_VC1_PICINTENCOMP, ic.intencomp);
		cedrus_write(dev, VE_DEC_VC1_PICICBAK0, ic.icbak0);
		cedrus_write(dev, VE_DEC_VC1_PICICBAK1, ic.icbak1);
	} else if (picture->ptype != V4L2_VC1_PICTURE_TYPE_BI) {
		cedrus_write(dev, VE_DEC_VC1_PICINTENCOMP, ic.intencomp);
		cedrus_write(dev, VE_DEC_VC1_PICICBAK1, ic.icbak1);
		cedrus_write(dev, VE_DEC_VC1_PICICBAK0, ic.icbak0);
	}

	/*
	 * B pictures do not code REFDIST: the distance which scales their
	 * motion vectors is the one of the backward reference.
	 */
	if (picture->ptype == V4L2_VC1_PICTURE_TYPE_B) {
		frfd = (bfraction * picture->backward_ref.refdist) >> 8;
		brfd = picture->backward_ref.refdist - frfd - 1;
	} else {
		frfd = picture->refdist;
		brfd = 0;
	}

	if (frfd > 3)
		frfd = 3;
	if (brfd < 0)
		brfd = 0;
	else if (brfd > 3)
		brfd = 3;

	reg = 0;
	if (raw_coding & V4L2_VC1_RAW_CODING_FLAG_FIELDTX)
		reg |= VE_DEC_VC1_PICINTERLACE_FIELDTX_RAW;
	reg |= VE_DEC_VC1_PICINTERLACE_DMVRANGE(picture->dmvrange);
	if (picture->fcm == V4L2_VC1_FCM_FRAME_INTERLACE) {
		if (picture->flags & V4L2_VC1_PICTURE_LAYER_FLAG_4MVSWITCH)
			reg |= VE_DEC_VC1_PICINTERLACE_4MVSWITCH;
	} else if (mvmode == V4L2_VC1_MVMODE_MIXED_MV) {
		reg |= VE_DEC_VC1_PICINTERLACE_4MVSWITCH;
	}
	reg |= VE_DEC_VC1_PICINTERLACE_MBMODETAB(picture->mbmodetab);
	reg |= VE_DEC_VC1_PICINTERLACE_IMVTAB(picture->imvtab);
	reg |= VE_DEC_VC1_PICINTERLACE_ICBPTAB(picture->icbptab);
	if (flag)
		reg |= VE_DEC_VC1_PICINTERLACE_INTENCOMP;
	reg |= VE_DEC_VC1_PICINTERLACE_2MVBPTAB(picture->twomvbptab);
	reg |= VE_DEC_VC1_PICINTERLACE_4MVBPTAB(picture->fourmvbptab);
	reg |= VE_DEC_VC1_PICINTERLACE_FRFD(frfd);
	reg |= VE_DEC_VC1_PICINTERLACE_BRFD(brfd);
	if (!(second_field ^ ref_field))
		reg |= VE_DEC_VC1_PICINTERLACE_REFFIELD;
	if (picture->fcm == V4L2_VC1_FCM_FIELD_INTERLACE)
		reg |= VE_DEC_VC1_PICINTERLACE_INTENCOMPFLD(ic.intencompfld);
	if (raw_coding & V4L2_VC1_RAW_CODING_FLAG_FORWARDMB)
		reg |= VE_DEC_VC1_PICINTERLACE_FORWARD_RAW;
	if (fwd_interlaced)
		reg |= VE_DEC_VC1_PICINTERLACE_FWD_INTERLACE;
	if (bwd_interlaced)
		reg |= VE_DEC_VC1_PICINTERLACE_BWD_INTERLACE;
	if (picture->flags & V4L2_VC1_PICTURE_LAYER_FLAG_NUMREF)
		reg |= VE_DEC_VC1_PICINTERLACE_NUMREF;
	cedrus_write(dev, VE_DEC_VC1_PICINTERLACE, reg);

	/* Set frame dimensions. */

	cedrus_vc1_coded_size(ctx, run, &width, &height);
	reg = VE_DEC_VC1_FSIZE_WIDTH(width);
	reg |= VE_DEC_VC1_FSIZE_HEIGHT(height);
	cedrus_write(dev, VE_DEC_VC1_FSIZE, reg);

	reg = VE_DEC_VC1_PICSIZE_WIDTH(width);
	reg |= VE_DEC_VC1_PICSIZE_HEIGHT(height);
	cedrus_write(dev, VE_DEC_VC1_PICSIZE, reg);

	/* Destination luma and chroma buffers. */

	dst_luma_addr = cedrus_dst_buf_addr(ctx, &run->dst->vb2_buf, 0);
	dst_chroma_addr = cedrus_dst_buf_addr(ctx, &run->dst->vb2_buf, 1);

	cedrus_write(dev, VE_DEC_VC1_REC_LUMA, dst_luma_addr);
	cedrus_write(dev, VE_DEC_VC1_REC_CHROMA, dst_chroma_addr);

	/* Forward and backward prediction reference buffers. */

	if (forward_vb2) {
		cedrus_write(dev, VE_DEC_VC1_FWD_REF_LUMA_ADDR,
			     cedrus_dst_buf_addr(ctx, forward_vb2, 0));
		cedrus_write(dev, VE_DEC_VC1_FWD_REF_CHROMA_ADDR,
			     cedrus_dst_buf_addr(ctx, forward_vb2, 1));
	} else {
		cedrus_write(dev, VE_DEC_VC1_FWD_REF_LUMA_ADDR, dst_luma_addr);
		cedrus_write(dev, VE_DEC_VC1_FWD_REF_CHROMA_ADDR, dst_chroma_addr);
	}

	if (backward_vb2) {
		cedrus_write(dev, VE_DEC_VC1_BWD_REF_LUMA_ADDR,
			     cedrus_dst_buf_addr(ctx, backward_vb2, 0));
		cedrus_write(dev, VE_DEC_VC1_BWD_REF_CHROMA_ADDR,
			     cedrus_dst_buf_addr(ctx, backward_vb2, 1));
	} else {
		cedrus_write(dev, VE_DEC_VC1_BWD_REF_LUMA_ADDR, dst_luma_addr);
		cedrus_write(dev, VE_DEC_VC1_BWD_REF_CHROMA_ADDR, dst_chroma_addr);
	}

	/*
	 * Setup bitplanes. The bitplanes control is only set for the pictures
	 * which have decoded bitplanes; its content is stale otherwise.
	 */

	if (picture->bitplane_flags)
		cedrus_vc1_bitplanes_setup(ctx, run);

	reg = VE_DEC_VC1_CTRL_FINISH_IRQ_EN |
	      VE_DEC_VC1_CTRL_ERROR_IRQ_EN |
	      VE_DEC_VC1_CTRL_VLD_DATA_REQ_IRQ_EN |
	      VE_DEC_VC1_CTRL_MCRI_CACHE_EN;
	cedrus_write(dev, VE_DEC_VC1_CTRL, reg);

	return 0;
}

static int cedrus_vc1_start(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;

	ctx->codec.vc1.aux_buf =
		dma_alloc_coherent(dev->dev, VC1_AUX_ALLOC_SIZE,
				   &ctx->codec.vc1.aux_buf_addr,
				   GFP_KERNEL);
	if (!ctx->codec.vc1.aux_buf)
		return -ENOMEM;

	ctx->codec.vc1.bitplanes_buf = ctx->codec.vc1.aux_buf + VC1_AUX_BPLANE_OFF;
	ctx->codec.vc1.bitplanes_buf_addr = ctx->codec.vc1.aux_buf_addr + VC1_AUX_BPLANE_OFF;
	ctx->codec.vc1.acdc_buf = ctx->codec.vc1.aux_buf + VC1_AUX_DCAC_OFF;
	ctx->codec.vc1.acdc_buf_addr = ctx->codec.vc1.aux_buf_addr + VC1_AUX_DCAC_OFF;
	ctx->codec.vc1.mv_buf = ctx->codec.vc1.aux_buf + VC1_AUX_MV_OFF;
	ctx->codec.vc1.mv_buf_addr = ctx->codec.vc1.aux_buf_addr + VC1_AUX_MV_OFF;

	return 0;
}

static void cedrus_vc1_stop(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;

	dma_free_coherent(dev->dev, VC1_AUX_ALLOC_SIZE,
			  ctx->codec.vc1.aux_buf,
			  ctx->codec.vc1.aux_buf_addr);
}

static void cedrus_vc1_trigger(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;

	cedrus_write(dev, VE_DEC_VC1_TRIGGER_TYPE,
		     VE_DEC_VC1_TRIGGER_TYPE_DECODE);
}

struct cedrus_dec_ops cedrus_dec_ops_vc1 = {
	.irq_clear	= cedrus_vc1_irq_clear,
	.irq_disable	= cedrus_vc1_irq_disable,
	.irq_status	= cedrus_vc1_irq_status,
	.setup		= cedrus_vc1_setup,
	.start		= cedrus_vc1_start,
	.stop		= cedrus_vc1_stop,
	.trigger	= cedrus_vc1_trigger,
};
