/* Decode the first IDR picture of an H.264 Annex B file on the GMA500's
 * video decoder and write it out as tightly packed NV12.
 *
 *   msvdxdec msvdx_fw.bin in.264 out.nv12 [-v]
 *
 * The header parsing here is the minimum for one IDR slice of a progressive
 * stream (SPS, PPS, slice header up to slice_data()); it exists to test the
 * decoder, not to be an H.264 parser. Compare the output against a software
 * decode of the same file, e.g.
 *   ffmpeg -i in.264 -frames:v 1 -f rawvideo -pix_fmt nv12 ref.nv12
 */
#include "msvdx.h"
#include "psb/psb_shim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- bit reader over RBSP ------------------------------------------------ */

typedef struct {
	const uint8_t* p;
	uint32_t size;
	uint32_t bit;
} bits;

static uint32_t
u(bits* b, int n)
{
	uint32_t v = 0;
	while (n--) {
		uint32_t byte = b->bit >> 3;
		v = (v << 1) | (byte < b->size
			? (b->p[byte] >> (7 - (b->bit & 7))) & 1 : 0);
		b->bit++;
	}
	return v;
}

static uint32_t
ue(bits* b)
{
	int zeros = 0;
	while (u(b, 1) == 0 && zeros < 32)
		zeros++;
	return ((1u << zeros) - 1) + (zeros ? u(b, zeros) : 0);
}

static int32_t
se(bits* b)
{
	uint32_t k = ue(b);
	return (k & 1) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2);
}

/* Strip emulation-prevention bytes; epPos[] records, for every removed
 * byte, its position in the RBSP so bit offsets can be mapped back. */
static uint32_t
to_rbsp(const uint8_t* in, uint32_t n, uint8_t* out, uint32_t* ep,
	int* epCount)
{
	uint32_t o = 0;
	int zeros = 0;
	*epCount = 0;
	for (uint32_t i = 0; i < n; i++) {
		if (zeros >= 2 && in[i] == 3) {
			ep[(*epCount)++] = o;
			zeros = 0;
			continue;
		}
		zeros = in[i] == 0 ? zeros + 1 : 0;
		out[o++] = in[i];
	}
	return o;
}

/* ---- the three headers ----------------------------------------------------- */

typedef struct {
	int profile_idc, level_idc, chroma_format_idc;
	int log2_max_frame_num, poc_type, log2_max_poc_lsb;
	int delta_pic_order_always_zero, num_ref_frames, gaps;
	int width_mbs, height_map_units, frame_mbs_only, mbaff, direct_8x8;
} sps_t;

typedef struct {
	int entropy, bottom_field_pic_order, num_slice_groups_minus1;
	int num_ref_l0, num_ref_l1, weighted_pred, weighted_bipred;
	int pic_init_qp_minus26, chroma_qp_index_offset, deblock_present;
	int constrained_intra, redundant_pic_cnt_present;
} pps_t;

static void
parse_sps(bits* b, sps_t* s)
{
	memset(s, 0, sizeof(*s));
	s->chroma_format_idc = 1;
	s->profile_idc = u(b, 8);
	u(b, 8);
	s->level_idc = u(b, 8);
	ue(b);
	if (s->profile_idc == 100 || s->profile_idc == 110
		|| s->profile_idc == 122 || s->profile_idc == 244) {
		s->chroma_format_idc = ue(b);
		ue(b);
		ue(b);
		u(b, 1);
		if (u(b, 1)) {
			fprintf(stderr, "scaling matrices in the SPS are not handled\n");
			exit(1);
		}
	}
	s->log2_max_frame_num = ue(b) + 4;
	s->poc_type = ue(b);
	if (s->poc_type == 0)
		s->log2_max_poc_lsb = ue(b) + 4;
	else if (s->poc_type == 1) {
		s->delta_pic_order_always_zero = u(b, 1);
		se(b);
		se(b);
		for (uint32_t i = ue(b); i > 0; i--)
			se(b);
	}
	s->num_ref_frames = ue(b);
	s->gaps = u(b, 1);
	s->width_mbs = ue(b) + 1;
	s->height_map_units = ue(b) + 1;
	s->frame_mbs_only = u(b, 1);
	if (!s->frame_mbs_only)
		s->mbaff = u(b, 1);
	s->direct_8x8 = u(b, 1);
}

static void
parse_pps(bits* b, pps_t* p)
{
	memset(p, 0, sizeof(*p));
	ue(b);
	ue(b);
	p->entropy = u(b, 1);
	p->bottom_field_pic_order = u(b, 1);
	p->num_slice_groups_minus1 = ue(b);
	if (p->num_slice_groups_minus1) {
		fprintf(stderr, "slice groups are not handled\n");
		exit(1);
	}
	p->num_ref_l0 = ue(b);
	p->num_ref_l1 = ue(b);
	p->weighted_pred = u(b, 1);
	p->weighted_bipred = u(b, 2);
	p->pic_init_qp_minus26 = se(b);
	se(b);
	p->chroma_qp_index_offset = se(b);
	p->deblock_present = u(b, 1);
	p->constrained_intra = u(b, 1);
	p->redundant_pic_cnt_present = u(b, 1);
}

/* ---- main ------------------------------------------------------------------- */

int
main(int argc, char** argv)
{
	uint8_t* file;
	long fileSize;
	FILE* f;
	const uint8_t* nal[64];
	uint32_t nalSize[64];
	int nals = 0;
	sps_t sps;
	pps_t pps;
	int haveSps = 0, havePps = 0;
	int slice = -1;

	if (argc < 4) {
		fprintf(stderr, "usage: %s msvdx_fw.bin in.264 out.nv12 [-v|-vv]\n",
			argv[0]);
		return 2;
	}
	if (argc > 4)
		psb_verbose = strlen(argv[4]) - 1;

	f = fopen(argv[2], "rb");
	if (f == NULL)
		return 1;
	fseek(f, 0, SEEK_END);
	fileSize = ftell(f);
	fseek(f, 0, SEEK_SET);
	file = malloc(fileSize);
	fread(file, 1, fileSize, f);
	fclose(f);

	/* Split Annex B */
	for (long i = 0; i + 3 < fileSize && nals < 64; i++) {
		if (file[i] == 0 && file[i + 1] == 0 && file[i + 2] == 1) {
			if (nals > 0)
				nalSize[nals - 1] = (file + i) - nal[nals - 1]
					- (i > 0 && file[i - 1] == 0 ? 1 : 0);
			nal[nals++] = file + i + 3;
			i += 2;
		}
	}
	if (nals > 0)
		nalSize[nals - 1] = (file + fileSize) - nal[nals - 1];

	static uint8_t rbsp[1 << 20];
	static uint32_t ep[1 << 16];
	int epCount;
	for (int n = 0; n < nals; n++) {
		int type = nal[n][0] & 0x1f;
		bits b;
		uint32_t len = to_rbsp(nal[n] + 1, nalSize[n] - 1, rbsp, ep, &epCount);
		b.p = rbsp;
		b.size = len;
		b.bit = 0;
		if (type == 7) {
			parse_sps(&b, &sps);
			haveSps = 1;
		} else if (type == 8) {
			parse_pps(&b, &pps);
			havePps = 1;
		} else if (type == 5 && slice < 0)
			slice = n;
	}
	if (!haveSps || !havePps || slice < 0) {
		fprintf(stderr, "need an SPS, a PPS and an IDR slice\n");
		return 1;
	}
	printf("SPS: profile %d level %d %dx%d MBs, frame_mbs_only %d, poc type "
		"%d, refs %d\n", sps.profile_idc, sps.level_idc, sps.width_mbs,
		sps.height_map_units * (2 - sps.frame_mbs_only), sps.frame_mbs_only,
		sps.poc_type, sps.num_ref_frames);
	printf("PPS: %s, qp %d, deblock ctl %d\n", pps.entropy ? "CABAC" : "CAVLC",
		26 + pps.pic_init_qp_minus26, pps.deblock_present);

	/* Slice header */
	VAPictureParameterBufferH264 pic;
	VASliceParameterBufferH264 sl;
	VAIQMatrixBufferH264 iq;
	memset(&pic, 0, sizeof(pic));
	memset(&sl, 0, sizeof(sl));
	memset(&iq, 16, sizeof(iq));

	int nalRefIdc = (nal[slice][0] >> 5) & 3;
	uint32_t len = to_rbsp(nal[slice] + 1, nalSize[slice] - 1, rbsp, ep,
		&epCount);
	bits b = { rbsp, len, 0 };
	sl.first_mb_in_slice = ue(&b);
	uint32_t sliceType = ue(&b) % 5;
	ue(&b);	/* pps id */
	int frameNum = u(&b, sps.log2_max_frame_num);
	if (!sps.frame_mbs_only && u(&b, 1)) {
		fprintf(stderr, "field pictures are not handled\n");
		return 1;
	}
	ue(&b);	/* idr_pic_id */
	if (sps.poc_type == 0) {
		u(&b, sps.log2_max_poc_lsb);
		if (pps.bottom_field_pic_order)
			se(&b);
	}
	if (pps.redundant_pic_cnt_present)
		ue(&b);
	/* I slice: no ref list modification, no weights. IDR marking: */
	u(&b, 1);	/* no_output_of_prior_pics_flag */
	u(&b, 1);	/* long_term_reference_flag */
	sl.slice_qp_delta = se(&b);
	if (pps.deblock_present) {
		sl.disable_deblocking_filter_idc = ue(&b);
		if (sl.disable_deblocking_filter_idc != 1) {
			sl.slice_alpha_c0_offset_div2 = se(&b);
			sl.slice_beta_offset_div2 = se(&b);
		}
	}
	if (pps.entropy) {
		/* cabac_alignment_one_bit */
		while (b.bit & 7)
			b.bit++;
	}
	/* The header is parsed in RBSP bits; slice_data_bit_offset counts from
	 * the start of the NAL unit, header byte included, in the bytes as
	 * stored. Add back every emulation-prevention byte that precedes it. */
	uint32_t rbspBits = b.bit;
	uint32_t rawBits = rbspBits + 8;
	for (int i = 0; i < epCount; i++)
		if (ep[i] * 8 < rbspBits)
			rawBits += 8;
	printf("slice: type %u, first MB %u, qp delta %d, deblock idc %d, "
		"data at bit %u of %u bytes\n", sliceType, sl.first_mb_in_slice,
		sl.slice_qp_delta, sl.disable_deblocking_filter_idc, rawBits,
		nalSize[slice]);
	if (sliceType != 2) {
		fprintf(stderr, "only I slices are handled\n");
		return 1;
	}

	/* Hardware */
	if (msvdx_open(argv[1]) != 0) {
		fprintf(stderr, "msvdx_open failed\n");
		return 1;
	}

	struct psb_driver_data_s driver;
	memset(&driver, 0, sizeof(driver));
	struct psb_surface_s surface;
	struct object_surface_s objSurface;
	int width = sps.width_mbs * 16;
	int height = sps.height_map_units * (2 - sps.frame_mbs_only) * 16;
	if (psb_surface_create(&driver, width, height, &surface)
			!= VA_STATUS_SUCCESS) {
		fprintf(stderr, "surface allocation failed\n");
		msvdx_close();
		return 1;
	}
	memset(surface.buf.mb.cpu, 0x5a, surface.size);	/* to see what was written */
	msvdx_flush(&surface.buf.mb, 0, surface.buf.mb.size);
	objSurface.surface_id = 0;
	objSurface.width = width;
	objSurface.height = height;
	objSurface.psb_surface = &surface;
	driver.surface_heap.surfaces[0] = &objSurface;

	struct object_config_s config;
	memset(&config, 0, sizeof(config));
	config.profile = sps.profile_idc == 66 ? VAProfileH264Baseline
		: sps.profile_idc == 77 ? VAProfileH264Main : VAProfileH264High;

	struct object_context_s context;
	memset(&context, 0, sizeof(context));
	context.driver_data = &driver;
	context.picture_width = width;
	context.picture_height = height;
	context.num_render_targets = 1;
	context.current_render_target = &objSurface;
	context.msvdx_context = 1;

	if (psb_H264_vtable.createContext(&context, &config)
			!= VA_STATUS_SUCCESS) {
		fprintf(stderr, "createContext failed\n");
		msvdx_close();
		return 1;
	}

	/* VA parameters */
	pic.CurrPic.picture_id = 0;
	pic.CurrPic.frame_idx = frameNum;
	pic.CurrPic.flags = nalRefIdc ? VA_PICTURE_H264_SHORT_TERM_REFERENCE : 0;
	for (int i = 0; i < 16; i++) {
		pic.ReferenceFrames[i].picture_id = 0xffffffff;
		pic.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
	}
	pic.picture_width_in_mbs_minus1 = sps.width_mbs - 1;
	pic.picture_height_in_mbs_minus1
		= sps.height_map_units * (2 - sps.frame_mbs_only) - 1;
	pic.num_ref_frames = 0;	/* an IDR picture references nothing */
	pic.seq_fields.bits.chroma_format_idc = sps.chroma_format_idc;
	pic.seq_fields.bits.gaps_in_frame_num_value_allowed_flag = sps.gaps;
	pic.seq_fields.bits.frame_mbs_only_flag = sps.frame_mbs_only;
	pic.seq_fields.bits.mb_adaptive_frame_field_flag = sps.mbaff;
	pic.seq_fields.bits.direct_8x8_inference_flag = sps.direct_8x8;
	pic.seq_fields.bits.MinLumaBiPredSize8x8 = sps.level_idc >= 31;
	pic.seq_fields.bits.log2_max_frame_num_minus4 = sps.log2_max_frame_num - 4;
	pic.seq_fields.bits.pic_order_cnt_type = sps.poc_type;
	pic.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4
		= sps.poc_type == 0 ? sps.log2_max_poc_lsb - 4 : 0;
	pic.seq_fields.bits.delta_pic_order_always_zero_flag
		= sps.delta_pic_order_always_zero;
	pic.num_slice_groups_minus1 = 0;
	pic.pic_init_qp_minus26 = pps.pic_init_qp_minus26;
	pic.chroma_qp_index_offset = pps.chroma_qp_index_offset;
	pic.second_chroma_qp_index_offset = pps.chroma_qp_index_offset;
	pic.pic_fields.bits.entropy_coding_mode_flag = pps.entropy;
	pic.pic_fields.bits.weighted_pred_flag = pps.weighted_pred;
	pic.pic_fields.bits.weighted_bipred_idc = pps.weighted_bipred;
	pic.pic_fields.bits.constrained_intra_pred_flag = pps.constrained_intra;
	pic.pic_fields.bits.pic_order_present_flag = pps.bottom_field_pic_order;
	pic.pic_fields.bits.deblocking_filter_control_present_flag
		= pps.deblock_present;
	pic.pic_fields.bits.redundant_pic_cnt_present_flag
		= pps.redundant_pic_cnt_present;
	pic.pic_fields.bits.reference_pic_flag = nalRefIdc != 0;
	pic.frame_num = frameNum;

	sl.slice_data_size = nalSize[slice];
	sl.slice_data_offset = 0;
	sl.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
	sl.slice_data_bit_offset = rawBits;
	sl.slice_type = sliceType;
	for (int i = 0; i < 32; i++) {
		sl.RefPicList0[i].picture_id = 0xffffffff;
		sl.RefPicList0[i].flags = VA_PICTURE_H264_INVALID;
		sl.RefPicList1[i].picture_id = 0xffffffff;
		sl.RefPicList1[i].flags = VA_PICTURE_H264_INVALID;
	}

	/* psb_H264 takes ownership of the parameter buffers and frees them. */
	VAPictureParameterBufferH264* picCopy = malloc(sizeof(pic));
	VAIQMatrixBufferH264* iqCopy = malloc(sizeof(iq));
	memcpy(picCopy, &pic, sizeof(pic));
	memcpy(iqCopy, &iq, sizeof(iq));

	struct psb_buffer_s bitstream;
	if (psb_buffer_create(&driver, nalSize[slice], psb_bt_cpu_vpu, &bitstream)
			!= VA_STATUS_SUCCESS) {
		fprintf(stderr, "bitstream allocation failed\n");
		msvdx_close();
		return 1;
	}
	memcpy(bitstream.mb.cpu, nal[slice], nalSize[slice]);
	msvdx_flush(&bitstream.mb, 0, bitstream.mb.size);

	struct object_buffer_s bufPic = { VAPictureParameterBufferType, picCopy,
		sizeof(pic), 1, NULL };
	struct object_buffer_s bufIq = { VAIQMatrixBufferType, iqCopy,
		sizeof(iq), 1, NULL };
	struct object_buffer_s bufSlice = { VASliceParameterBufferType, &sl,
		sizeof(sl), 1, NULL };
	struct object_buffer_s bufData = { VASliceDataBufferType, NULL,
		nalSize[slice], 1, &bitstream };
	object_buffer_p buffers[] = { &bufPic, &bufIq, &bufSlice, &bufData };

	bigtime_t start = system_time();
	VAStatus st = psb_H264_vtable.beginPicture(&context);
	if (st == VA_STATUS_SUCCESS)
		st = psb_H264_vtable.renderPicture(&context, buffers, 4);
	if (st == VA_STATUS_SUCCESS)
		st = psb_H264_vtable.endPicture(&context);
	bigtime_t elapsed = system_time() - start;
	printf("decode: status %d, %lld us\n", st, (long long)elapsed);
	msvdx_dump_state("after decode");

	/* Write NV12 without the stride */
	msvdx_flush(&surface.buf.mb, 0, surface.buf.mb.size);
	f = fopen(argv[3], "wb");
	for (int y = 0; y < height; y++)
		fwrite(surface.buf.mb.cpu + y * surface.stride, 1, width, f);
	for (int y = 0; y < height / 2; y++)
		fwrite(surface.buf.mb.cpu + surface.chroma_offset
			+ y * surface.stride, 1, width, f);
	fclose(f);
	printf("wrote %dx%d NV12 to %s; first luma bytes: %02x %02x %02x %02x\n",
		width, height, argv[3], surface.buf.mb.cpu[0], surface.buf.mb.cpu[1],
		surface.buf.mb.cpu[2], surface.buf.mb.cpu[3]);

	psb_H264_vtable.destroyContext(&context);
	msvdx_close();
	return st == VA_STATUS_SUCCESS ? 0 : 1;
}
