/* Decode an H.264 Annex B file on the GMA500's video decoder and write the
 * pictures out as tightly packed NV12.
 *
 *   msvdxdec msvdx_fw.bin in.264 out.nv12 [-v|-vv]
 *
 * The parsing here is the minimum for a progressive stream of I and P
 * pictures, one slice each, with one reference frame (the previous one); it
 * exists to test the decoder, not to be an H.264 parser. Compare the output against a software
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

/* ---- slice header ---------------------------------------------------------- */

typedef struct {
	int nal_ref_idc, idr, slice_type, frame_num, poc_lsb;
	int num_ref_l0_minus1;
	uint32_t data_bit;	/* in the stored bytes, NAL header included */
} slice_t;

static int
parse_slice(const uint8_t* nal, uint32_t size, const sps_t* sps,
	const pps_t* pps, slice_t* sh, VASliceParameterBufferH264* sl)
{
	static uint8_t rbsp[1 << 21];
	static uint32_t ep[1 << 16];
	int epCount;
	uint32_t len = to_rbsp(nal + 1, size - 1, rbsp, ep, &epCount);
	bits b = { rbsp, len, 0 };

	memset(sh, 0, sizeof(*sh));
	sh->nal_ref_idc = (nal[0] >> 5) & 3;
	sh->idr = (nal[0] & 0x1f) == 5;
	sl->first_mb_in_slice = ue(&b);
	sh->slice_type = ue(&b) % 5;
	ue(&b);
	sh->frame_num = u(&b, sps->log2_max_frame_num);
	if (!sps->frame_mbs_only && u(&b, 1)) {
		fprintf(stderr, "field pictures are not handled\n");
		return 1;
	}
	if (sh->idr)
		ue(&b);
	if (sps->poc_type == 0) {
		sh->poc_lsb = u(&b, sps->log2_max_poc_lsb);
		if (pps->bottom_field_pic_order)
			se(&b);
	} else if (sps->poc_type == 1 && !sps->delta_pic_order_always_zero) {
		se(&b);
		if (pps->bottom_field_pic_order)
			se(&b);
	}
	if (pps->redundant_pic_cnt_present)
		ue(&b);
	if (sh->slice_type == 1) {
		fprintf(stderr, "B slices are not handled\n");
		return 1;
	}
	sh->num_ref_l0_minus1 = pps->num_ref_l0;
	if (sh->slice_type == 0 || sh->slice_type == 3) {
		if (u(&b, 1))
			sh->num_ref_l0_minus1 = ue(&b);
		if (u(&b, 1)) {	/* ref_pic_list_modification_flag_l0 */
			uint32_t op;
			while ((op = ue(&b)) != 3) {
				ue(&b);
				(void)op;
			}
		}
		if (pps->weighted_pred) {
			fprintf(stderr, "weighted prediction is not handled\n");
			return 1;
		}
	}
	if (sh->nal_ref_idc) {
		if (sh->idr) {
			u(&b, 1);
			u(&b, 1);
		} else if (u(&b, 1)) {	/* adaptive_ref_pic_marking_mode_flag */
			uint32_t mmco;
			while ((mmco = ue(&b)) != 0) {
				if (mmco == 1 || mmco == 3)
					ue(&b);
				if (mmco == 2)
					ue(&b);
				if (mmco == 3 || mmco == 6)
					ue(&b);
				if (mmco == 4)
					ue(&b);
			}
		}
	}
	sl->cabac_init_idc = 0;
	if (pps->entropy && sh->slice_type != 2 && sh->slice_type != 4)
		sl->cabac_init_idc = ue(&b);
	sl->slice_qp_delta = se(&b);
	sl->disable_deblocking_filter_idc = 0;
	sl->slice_alpha_c0_offset_div2 = 0;
	sl->slice_beta_offset_div2 = 0;
	if (pps->deblock_present) {
		sl->disable_deblocking_filter_idc = ue(&b);
		if (sl->disable_deblocking_filter_idc != 1) {
			sl->slice_alpha_c0_offset_div2 = se(&b);
			sl->slice_beta_offset_div2 = se(&b);
		}
	}
	if (pps->entropy) {
		while (b.bit & 7)
			b.bit++;
	}
	sh->data_bit = b.bit + 8;
	for (int i = 0; i < epCount; i++)
		if (ep[i] * 8 < b.bit)
			sh->data_bit += 8;
	return 0;
}

/* ---- main ------------------------------------------------------------------- */

#define NUM_SURFACES 2

int
main(int argc, char** argv)
{
	uint8_t* file;
	long fileSize;
	FILE* f;
	static const uint8_t* nal[1 << 14];
	static uint32_t nalSize[1 << 14];
	int nals = 0;
	sps_t sps;
	pps_t pps;
	int haveSps = 0, havePps = 0;

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

	for (long i = 0; i + 3 < fileSize && nals < (1 << 14); i++) {
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

	uint32_t maxSlice = 0;
	for (int n = 0; n < nals; n++) {
		static uint8_t rbsp[1 << 16];
		static uint32_t ep[1 << 12];
		int epCount;
		int type = nal[n][0] & 0x1f;
		if (type == 7 && !haveSps) {
			bits b = { rbsp, to_rbsp(nal[n] + 1, nalSize[n] - 1, rbsp, ep,
				&epCount), 0 };
			parse_sps(&b, &sps);
			haveSps = 1;
		} else if (type == 8 && !havePps) {
			bits b = { rbsp, to_rbsp(nal[n] + 1, nalSize[n] - 1, rbsp, ep,
				&epCount), 0 };
			parse_pps(&b, &pps);
			havePps = 1;
		} else if ((type == 1 || type == 5) && nalSize[n] > maxSlice)
			maxSlice = nalSize[n];
	}
	if (!haveSps || !havePps) {
		fprintf(stderr, "need an SPS and a PPS\n");
		return 1;
	}
	int width = sps.width_mbs * 16;
	int height = sps.height_map_units * (2 - sps.frame_mbs_only) * 16;
	printf("SPS: profile %d level %d, %dx%d, poc type %d, refs %d\n",
		sps.profile_idc, sps.level_idc, width, height, sps.poc_type,
		sps.num_ref_frames);
	printf("PPS: %s, qp %d\n", pps.entropy ? "CABAC" : "CAVLC",
		26 + pps.pic_init_qp_minus26);

	if (msvdx_open(argv[1]) != 0) {
		fprintf(stderr, "msvdx_open failed\n");
		return 1;
	}

	struct psb_driver_data_s driver;
	memset(&driver, 0, sizeof(driver));
	struct psb_surface_s surface[NUM_SURFACES];
	struct object_surface_s objSurface[NUM_SURFACES];
	for (int i = 0; i < NUM_SURFACES; i++) {
		if (psb_surface_create(&driver, width, height, &surface[i])
				!= VA_STATUS_SUCCESS) {
			fprintf(stderr, "surface allocation failed\n");
			msvdx_close();
			return 1;
		}
		objSurface[i].surface_id = i;
		objSurface[i].width = width;
		objSurface[i].height = height;
		objSurface[i].psb_surface = &surface[i];
		driver.surface_heap.surfaces[i] = &objSurface[i];
	}

	struct object_config_s config;
	memset(&config, 0, sizeof(config));
	config.profile = sps.profile_idc == 66 ? VAProfileH264Baseline
		: sps.profile_idc == 77 ? VAProfileH264Main : VAProfileH264High;

	struct object_context_s context;
	memset(&context, 0, sizeof(context));
	context.driver_data = &driver;
	context.picture_width = width;
	context.picture_height = height;
	context.num_render_targets = NUM_SURFACES;
	context.msvdx_context = 1;
	if (psb_H264_vtable.createContext(&context, &config)
			!= VA_STATUS_SUCCESS) {
		fprintf(stderr, "createContext failed\n");
		msvdx_close();
		return 1;
	}

	struct psb_buffer_s bitstream;
	if (psb_buffer_create(&driver, maxSlice + 64, psb_bt_cpu_vpu, &bitstream)
			!= VA_STATUS_SUCCESS) {
		fprintf(stderr, "bitstream allocation failed\n");
		msvdx_close();
		return 1;
	}

	FILE* out = fopen(argv[3], "wb");
	int frames = 0;
	int failed = 0;
	int prev = -1;
	int prevFrameNum = 0;
	int poc = 0;
	bigtime_t total = 0;
	bigtime_t worst = 0;

	for (int n = 0; n < nals && !failed; n++) {
		int type = nal[n][0] & 0x1f;
		if (type != 1 && type != 5)
			continue;

		VAPictureParameterBufferH264* pic = calloc(1, sizeof(*pic));
		VAIQMatrixBufferH264* iq = malloc(sizeof(*iq));
		VASliceParameterBufferH264 sl;
		slice_t sh;
		memset(&sl, 0, sizeof(sl));
		memset(iq, 16, sizeof(*iq));
		if (parse_slice(nal[n], nalSize[n], &sps, &pps, &sh, &sl) != 0)
			break;
		if (sl.first_mb_in_slice != 0) {
			fprintf(stderr, "multiple slices per picture are not handled\n");
			break;
		}
		if (sh.idr)
			poc = 0;
		int cur = frames % NUM_SURFACES;

		pic->CurrPic.picture_id = cur;
		pic->CurrPic.frame_idx = sh.frame_num;
		pic->CurrPic.flags = sh.nal_ref_idc
			? VA_PICTURE_H264_SHORT_TERM_REFERENCE : 0;
		pic->CurrPic.TopFieldOrderCnt = poc;
		pic->CurrPic.BottomFieldOrderCnt = poc;
		for (int i = 0; i < 16; i++) {
			pic->ReferenceFrames[i].picture_id = 0xffffffff;
			pic->ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
		}
		for (int i = 0; i < 32; i++) {
			sl.RefPicList0[i].picture_id = 0xffffffff;
			sl.RefPicList0[i].flags = VA_PICTURE_H264_INVALID;
			sl.RefPicList1[i].picture_id = 0xffffffff;
			sl.RefPicList1[i].flags = VA_PICTURE_H264_INVALID;
		}
		if (!sh.idr && prev >= 0) {
			VAPictureH264 ref;
			memset(&ref, 0, sizeof(ref));
			ref.picture_id = prev;
			ref.frame_idx = prevFrameNum;
			ref.flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
			ref.TopFieldOrderCnt = poc - 2;
			ref.BottomFieldOrderCnt = poc - 2;
			pic->ReferenceFrames[0] = ref;
			pic->num_ref_frames = 1;
			sl.RefPicList0[0] = ref;
		}
		pic->picture_width_in_mbs_minus1 = sps.width_mbs - 1;
		pic->picture_height_in_mbs_minus1
			= sps.height_map_units * (2 - sps.frame_mbs_only) - 1;
		pic->seq_fields.bits.chroma_format_idc = sps.chroma_format_idc;
		pic->seq_fields.bits.gaps_in_frame_num_value_allowed_flag = sps.gaps;
		pic->seq_fields.bits.frame_mbs_only_flag = sps.frame_mbs_only;
		pic->seq_fields.bits.mb_adaptive_frame_field_flag = sps.mbaff;
		pic->seq_fields.bits.direct_8x8_inference_flag = sps.direct_8x8;
		pic->seq_fields.bits.MinLumaBiPredSize8x8 = sps.level_idc >= 31;
		pic->seq_fields.bits.log2_max_frame_num_minus4
			= sps.log2_max_frame_num - 4;
		pic->seq_fields.bits.pic_order_cnt_type = sps.poc_type;
		pic->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4
			= sps.poc_type == 0 ? sps.log2_max_poc_lsb - 4 : 0;
		pic->seq_fields.bits.delta_pic_order_always_zero_flag
			= sps.delta_pic_order_always_zero;
		pic->pic_init_qp_minus26 = pps.pic_init_qp_minus26;
		pic->chroma_qp_index_offset = pps.chroma_qp_index_offset;
		pic->second_chroma_qp_index_offset = pps.chroma_qp_index_offset;
		pic->pic_fields.bits.entropy_coding_mode_flag = pps.entropy;
		pic->pic_fields.bits.weighted_pred_flag = pps.weighted_pred;
		pic->pic_fields.bits.weighted_bipred_idc = pps.weighted_bipred;
		pic->pic_fields.bits.constrained_intra_pred_flag = pps.constrained_intra;
		pic->pic_fields.bits.pic_order_present_flag = pps.bottom_field_pic_order;
		pic->pic_fields.bits.deblocking_filter_control_present_flag
			= pps.deblock_present;
		pic->pic_fields.bits.redundant_pic_cnt_present_flag
			= pps.redundant_pic_cnt_present;
		pic->pic_fields.bits.reference_pic_flag = sh.nal_ref_idc != 0;
		pic->frame_num = sh.frame_num;

		sl.slice_data_size = nalSize[n];
		sl.slice_data_offset = 0;
		sl.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
		sl.slice_data_bit_offset = sh.data_bit;
		sl.slice_type = sh.slice_type;
		sl.num_ref_idx_l0_active_minus1 = sh.num_ref_l0_minus1;

		memcpy(bitstream.mb.cpu, nal[n], nalSize[n]);
		msvdx_flush(&bitstream.mb, 0, nalSize[n]);

		struct object_buffer_s bufPic = { VAPictureParameterBufferType, pic,
			sizeof(*pic), 1, NULL };
		struct object_buffer_s bufIq = { VAIQMatrixBufferType, iq,
			sizeof(*iq), 1, NULL };
		struct object_buffer_s bufSlice = { VASliceParameterBufferType, &sl,
			sizeof(sl), 1, NULL };
		struct object_buffer_s bufData = { VASliceDataBufferType, NULL,
			nalSize[n], 1, &bitstream };
		object_buffer_p buffers[] = { &bufPic, &bufIq, &bufSlice, &bufData };

		context.current_render_target = &objSurface[cur];
		bigtime_t start = system_time();
		VAStatus st = psb_H264_vtable.beginPicture(&context);
		if (st == VA_STATUS_SUCCESS)
			st = psb_H264_vtable.renderPicture(&context, buffers, 4);
		if (st == VA_STATUS_SUCCESS)
			st = psb_H264_vtable.endPicture(&context);
		bigtime_t elapsed = system_time() - start;
		total += elapsed;
		if (elapsed > worst)
			worst = elapsed;
		if (st != VA_STATUS_SUCCESS) {
			fprintf(stderr, "frame %d (%c, %u bytes): status %d\n", frames,
				"PBIpi"[sh.slice_type], nalSize[n], st);
			failed = 1;
			break;
		}
		if (psb_verbose)
			printf("frame %d: %c %u bytes, %lld us\n", frames,
				"PBIpi"[sh.slice_type], nalSize[n], (long long)elapsed);

		msvdx_flush(&surface[cur].buf.mb, 0, surface[cur].buf.mb.size);
		for (int y = 0; y < height; y++)
			fwrite(surface[cur].buf.mb.cpu + y * surface[cur].stride, 1,
				width, out);
		for (int y = 0; y < height / 2; y++)
			fwrite(surface[cur].buf.mb.cpu + surface[cur].chroma_offset
				+ y * surface[cur].stride, 1, width, out);

		if (sh.nal_ref_idc) {
			prev = cur;
			prevFrameNum = sh.frame_num;
		}
		poc += 2;
		frames++;
	}
	fclose(out);
	printf("decoded %d frames of %dx%d, %lld us total, %lld us per frame "
		"(%.1f fps), worst %lld us\n", frames, width, height,
		(long long)total, frames ? (long long)(total / frames) : 0,
		total ? frames * 1e6 / total : 0.0, (long long)worst);

	psb_H264_vtable.destroyContext(&context);
	msvdx_close();
	return failed;
}
