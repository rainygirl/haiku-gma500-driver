/* 오버레이 평면을 유저랜드에서 켜 본다.
 *
 * 커서 때와 같은 순서다. 커널에 아무것도 올리지 않고 /dev/misc/poke 로만
 * 확인한다. 답을 얻어야 하는 것이 둘이다.
 *
 *   1. 링 버퍼 없이 OVADD 쓰기만으로 오버레이 갱신이 걸리는가.
 *      i915 는 MI_OVERLAY_FLIP 명령으로 거는데 Poulsbo 에는 그 커맨드
 *      스트리머가 없다(그리기 엔진이 PowerVR SGX 다).
 *   2. OVADD 와 버퍼 주소가 물리 주소인가 GTT 오프셋인가.
 *      커서(CURBBASE)는 물리 주소였다.
 *
 * 레지스터 버퍼와 영상 버퍼를 둘 다 스톨른 메모리의 남는 자리에 둔다.
 * 그러면 물리 주소와 GTT 오프셋을 둘 다 알 수 있어 A/B 가 쉽다.
 *
 * overlay_registers 구조는 Haiku 의 intel_extreme accelerant 에서 가져왔다
 * (MIT).
 */
#include <Drivers.h>
#include <PCI.h>
#include <OS.h>
#include <poke.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#define OV_UPDATE		0x30000
#define OV_TEST			0x30004
#define OV_STATUS		0x30008
#define OV_EXT_STATUS	0x3000c

#define OVERLAY_FORMAT_RGB32	0x1
#define OVERLAY_FORMAT_YCbCr422	0x8
#define OVERLAY_MIRROR_NORMAL	0x0

#define REG_OFFSET	0x600000		/* 스톨른 안 6 MB 지점 - 레지스터 버퍼 */
#define BUF_OFFSET	0x610000		/* 그 위 - 영상 버퍼 */
#define VIEW_W		320
#define VIEW_H		240
#define WIN_X		600
#define WIN_Y		250

struct overlay_scale {
	uint32 _reserved0 : 3;
	uint32 horizontal_scale_fraction : 12;
	uint32 _reserved1 : 1;
	uint32 horizontal_downscale_factor : 3;
	uint32 _reserved2 : 1;
	uint32 vertical_scale_fraction : 12;
};

struct overlay_registers {
	uint32 buffer_rgb0;
	uint32 buffer_rgb1;
	uint32 buffer_u0;
	uint32 buffer_v0;
	uint32 buffer_u1;
	uint32 buffer_v1;
	uint16 stride_rgb;
	uint16 stride_uv;
	uint16 vertical_phase0_rgb;
	uint16 vertical_phase1_rgb;
	uint16 vertical_phase0_uv;
	uint16 vertical_phase1_uv;
	uint16 horizontal_phase_rgb;
	uint16 horizontal_phase_uv;
	uint32 initial_vertical_phase0_shift_rgb0 : 4;
	uint32 initial_vertical_phase1_shift_rgb0 : 4;
	uint32 initial_horizontal_phase_shift_rgb0 : 4;
	uint32 initial_vertical_phase0_shift_uv : 4;
	uint32 initial_vertical_phase1_shift_uv : 4;
	uint32 initial_horizontal_phase_shift_uv : 4;
	uint32 _reserved0 : 8;
	uint16 window_left;
	uint16 window_top;
	uint16 window_width;
	uint16 window_height;
	uint16 source_width_rgb;
	uint16 source_width_uv;
	uint16 source_bytes_per_row_rgb;
	uint16 source_bytes_per_row_uv;
	uint16 source_height_rgb;
	uint16 source_height_uv;
	struct overlay_scale scale_rgb;
	struct overlay_scale scale_uv;
	uint32 brightness_correction : 8;
	uint32 _reserved1 : 10;
	uint32 contrast_correction : 9;
	uint32 _reserved2 : 5;
	uint32 saturation_cos_correction : 10;
	uint32 _reserved3 : 6;
	uint32 saturation_sin_correction : 11;
	uint32 _reserved4 : 5;
	uint32 color_key_blue : 8;
	uint32 color_key_green : 8;
	uint32 color_key_red : 8;
	uint32 _reserved5 : 8;
	uint32 color_key_mask_blue : 8;
	uint32 color_key_mask_green : 8;
	uint32 color_key_mask_red : 8;
	uint32 _reserved6 : 7;
	uint32 color_key_enabled : 1;
	uint32 source_chroma_key_high_red : 8;
	uint32 source_chroma_key_high_blue : 8;
	uint32 source_chroma_key_high_green : 8;
	uint32 _reserved7 : 8;
	uint32 source_chroma_key_low_red : 8;
	uint32 source_chroma_key_low_blue : 8;
	uint32 source_chroma_key_low_green : 8;
	uint32 _reserved8 : 8;
	uint32 _reserved9 : 24;
	uint32 source_chroma_key_red_enabled : 1;
	uint32 source_chroma_key_blue_enabled : 1;
	uint32 source_chroma_key_green_enabled : 1;
	uint32 _reserved10 : 5;
	uint32 _reserved11 : 3;
	uint32 color_control_output_mode : 1;
	uint32 yuv_to_rgb_bypass : 1;
	uint32 _reserved12 : 11;
	uint32 gamma2_enabled : 1;
	uint32 _reserved13 : 1;
	uint32 select_pipe : 1;
	uint32 slot_time : 8;
	uint32 _reserved14 : 5;
	uint32 overlay_enabled : 1;
	uint32 active_field : 1;
	uint32 active_buffer : 2;
	uint32 test_mode : 1;
	uint32 buffer_field_mode : 1;
	uint32 _reserved15 : 1;
	uint32 tv_flip_field_enabled : 1;
	uint32 _reserved16 : 1;
	uint32 tv_flip_field_parity : 1;
	uint32 source_format : 4;
	uint32 ycbcr422_order : 2;
	uint32 _reserved18 : 1;
	uint32 mirroring_mode : 2;
	uint32 _reserved19 : 13;
	uint32 _reserved20;
	uint32 start_0y;
	uint32 start_1y;
	uint32 start_0u;
	uint32 start_0v;
	uint32 start_1u;
	uint32 start_1v;
	uint32 _reserved21[6];
	uint16 horizontal_scale_uv;
	uint16 horizontal_scale_rgb;
	uint16 vertical_scale_uv;
	uint16 vertical_scale_rgb;
	uint32 _reserved22[86];
	uint16 vertical_coefficients_rgb[128];
	uint16 horizontal_coefficients_rgb[128];
	uint32 _reserved23[64];
	uint16 vertical_coefficients_uv[128];
	uint16 horizontal_coefficients_uv[128];
};


/* --- 폴리페이즈 필터 계수 -------------------------------------------------
 *
 * Haiku 의 intel_extreme accelerant (overlay.cpp, MIT) 에서 가져왔다. 원본은
 * X 드라이버에서 왔다. 계수를 채우지 않고 오버레이를 켜면 필터 엔진이
 * 쓰레기 값으로 돌아 필요한 것보다 훨씬 많은 라인을 읽는 것으로 보인다.
 */
#define NUM_HORIZONTAL_TAPS		5
#define NUM_HORIZONTAL_UV_TAPS	3
#define NUM_PHASES				17
#define MAX_TAPS				5

struct phase_coefficient {
	unsigned char sign;
	unsigned char exponent;
	unsigned short mantissa;
};


static int
split_coefficient(double* coefficient, int mantissaSize,
	struct phase_coefficient* splitCoefficient)
{
	double absCoefficient = fabs(*coefficient);
	int sign = (*coefficient < 0.0) ? 1 : 0;
	int intCoefficient, res;
	int maxValue = 1 << mantissaSize;
	res = 12 - mantissaSize;

	if ((intCoefficient = (int)(absCoefficient * 4 * maxValue + 0.5))
			< maxValue) {
		splitCoefficient->exponent = 3;
		splitCoefficient->mantissa = intCoefficient << res;
		*coefficient = (double)intCoefficient / (double)(4 * maxValue);
	} else if ((intCoefficient = (int)(absCoefficient * 2 * maxValue + 0.5))
			< maxValue) {
		splitCoefficient->exponent = 2;
		splitCoefficient->mantissa = intCoefficient << res;
		*coefficient = (double)intCoefficient / (double)(2 * maxValue);
	} else if ((intCoefficient = (int)(absCoefficient * maxValue + 0.5))
			< maxValue) {
		splitCoefficient->exponent = 1;
		splitCoefficient->mantissa = intCoefficient << res;
		*coefficient = (double)intCoefficient / (double)maxValue;
	} else if ((intCoefficient = (int)(absCoefficient * maxValue * 0.5 + 0.5))
			< maxValue) {
		splitCoefficient->exponent = 0;
		splitCoefficient->mantissa = intCoefficient << res;
		*coefficient = (double)intCoefficient / (double)(maxValue / 2);
	} else
		return 0;

	splitCoefficient->sign = sign;
	if (sign)
		*coefficient = -*coefficient;
	return 1;
}


static void
update_coefficients(int taps, double filterCutOff, int horizontal, int isY,
	struct phase_coefficient* splitCoefficients)
{
	int isVerticalUV = !horizontal && !isY;
	int mantissaSize = horizontal ? 7 : 6;
	double rawCoefficients[MAX_TAPS * 32];
	double coefficients[NUM_PHASES][MAX_TAPS];
	int num, i, j, k, pos;
	double sum;

	if (filterCutOff < 1)
		filterCutOff = 1;
	if (filterCutOff > 3)
		filterCutOff = 3;

	num = taps * 16;
	for (i = 0; i < num * 2; i++) {
		double sinc;
		double value = (1.0 / filterCutOff) * taps * M_PI * (i - num)
			/ (2 * num);
		double window;
		if (value == 0.0)
			sinc = 1.0;
		else
			sinc = sin(value) / value;
		window = (0.5 - 0.5 * cos(i * M_PI / num));
		rawCoefficients[i] = sinc * window;
	}

	for (i = 0; i < NUM_PHASES; i++) {
		int tapAdjust[MAX_TAPS];
		sum = 0.0;
		for (j = 0; j < taps; j++) {
			pos = i + j * 32;
			sum += rawCoefficients[pos];
		}
		for (j = 0; j < taps; j++) {
			pos = i + j * 32;
			coefficients[i][j] = rawCoefficients[pos] / sum;
		}
		for (j = 0; j < taps; j++) {
			pos = j + i * taps;
			split_coefficient(&coefficients[i][j], mantissaSize
				+ (((j == (taps - 1) / 2) && !isVerticalUV) ? 2 : 0),
				&splitCoefficients[pos]);
		}

		tapAdjust[0] = (taps - 1) / 2;
		for (j = 1, k = 1; j <= tapAdjust[0]; j++, k++) {
			tapAdjust[k] = tapAdjust[0] - j;
			tapAdjust[++k] = tapAdjust[0] + j;
		}

		sum = 0.0;
		for (j = 0; j < taps; j++)
			sum += coefficients[i][j];

		if (sum != 1.0) {
			for (k = 0; k < taps; k++) {
				int tap2Fix = tapAdjust[k];
				double diff = 1.0 - sum;
				coefficients[i][tap2Fix] += diff;
				pos = tap2Fix + i * taps;
				split_coefficient(&coefficients[i][tap2Fix], mantissaSize
					+ (((tap2Fix == (taps - 1) / 2) && !isVerticalUV) ? 2 : 0),
					&splitCoefficients[pos]);
				sum = 0.0;
				for (j = 0; j < taps; j++)
					sum += coefficients[i][j];
				if (sum == 1.0)
					break;
			}
		}
	}
}


static volatile uint8* sRegs;
static uint32 rd(uint32 o) { return *(volatile uint32*)(sRegs + o); }
static void wr(uint32 o, uint32 v) { *(volatile uint32*)(sRegs + o) = v; }


int
main(int argc, char** argv)
{
	pci_info info;
	pci_info_args args;
	mem_map_args mmio, stolen, gtt;
	struct overlay_registers* ov;
	uint8* base;
	uint32* video;
	uint32 stolenBase, stride, height, address;
	int ovaddPhys = 1;
	int bufPhys = 1;
	int seconds = 5;
	int slotTime = 0;
	int useFramebuffer = 0;
	int withCoefficients = 1;
	uint32 newArb = 0;
	uint32 newFw1 = 0;
	uint32 oldArb, oldFw1;
	int index, x, y;
	int fd = open(POKE_DEVICE_FULLNAME, O_RDWR);

	/* argv[1] 두 글자: 첫째는 OVADD, 둘째는 영상 버퍼 주소.
	   g = GTT 오프셋, p = 물리 주소. 예: gg, pg, gp, pp */
	if (argc > 1 && strlen(argv[1]) == 2) {
		ovaddPhys = argv[1][0] == 'p';
		bufPhys = argv[1][1] == 'p';
		useFramebuffer = argv[1][1] == 'f';
	}
	if (argc > 2)
		seconds = atoi(argv[2]);
	if (argc > 3)
		slotTime = atoi(argv[3]);
	if (argc > 4)
		newArb = (uint32)strtoul(argv[4], NULL, 16);
	if (argc > 5)
		newFw1 = (uint32)strtoul(argv[5], NULL, 16);
	if (argc > 6)
		withCoefficients = atoi(argv[6]);

	if (fd < 0)
		return 1;
	args.signature = POKE_SIGNATURE;
	args.info = &info;
	for (index = 0; index < 255; index++) {
		args.index = index;
		if (ioctl(fd, POKE_GET_NTH_PCI_INFO, &args, sizeof(args)) != B_OK
			|| args.status != B_OK)
			return 1;
		if (info.vendor_id == 0x8086 && info.device_id == 0x8108)
			break;
	}
	memset(&mmio, 0, sizeof(mmio));
	mmio.signature = POKE_SIGNATURE;
	mmio.name = "ov regs";
	mmio.physical_address = info.u.h0.base_registers[0];
	mmio.size = info.u.h0.base_register_sizes[0];
	mmio.flags = B_ANY_ADDRESS;
	mmio.protection = B_READ_AREA | B_WRITE_AREA;
	if (ioctl(fd, POKE_MAP_MEMORY, &mmio, sizeof(mmio)) < 0)
		return 1;
	sRegs = (volatile uint8*)mmio.address;
	stride = rd(0x71188) & 0xffff;
	height = (rd(0x6101c) & 0xffff) + 1;

	memset(&gtt, 0, sizeof(gtt));
	gtt.signature = POKE_SIGNATURE;
	gtt.name = "gtt";
	gtt.physical_address = info.u.h0.base_registers[3];
	gtt.size = info.u.h0.base_register_sizes[3];
	gtt.flags = B_ANY_ADDRESS;
	gtt.protection = B_READ_AREA;
	if (ioctl(fd, POKE_MAP_MEMORY, &gtt, sizeof(gtt)) < 0)
		return 1;
	stolenBase = *(volatile uint32*)gtt.address & ~0xfff;
	ioctl(fd, POKE_UNMAP_MEMORY, &gtt, sizeof(gtt));

	memset(&stolen, 0, sizeof(stolen));
	stolen.signature = POKE_SIGNATURE;
	stolen.name = "stolen";
	stolen.physical_address = stolenBase;
	stolen.size = 0x700000;			/* 7 MB - 프레임버퍼 4.7 MB 너머까지 */
	stolen.flags = B_ANY_ADDRESS;
	stolen.protection = B_READ_AREA | B_WRITE_AREA;
	if (ioctl(fd, POKE_MAP_MEMORY, &stolen, sizeof(stolen)) < 0)
		return 1;
	base = (uint8*)stolen.address;

	printf("스톨른 %08lx, 화면 %lux%lu\n", (unsigned long)stolenBase,
		(unsigned long)(stride / 4), (unsigned long)height);
	printf("레지스터 버퍼 오프셋 %06x (물리 %08lx)\n", REG_OFFSET,
		(unsigned long)(stolenBase + REG_OFFSET));
	printf("영상 버퍼   오프셋 %06x (물리 %08lx)\n", BUF_OFFSET,
		(unsigned long)(stolenBase + BUF_OFFSET));
	printf("OVADD: %s, 영상 버퍼: %s\n",
		ovaddPhys ? "물리 주소" : "GTT 오프셋",
		useFramebuffer ? "프레임버퍼 자체 (GTT 오프셋 0)"
			: (bufPhys ? "물리 주소" : "GTT 오프셋"));

	/* 영상 버퍼: 가로 4 색 띠 */
	video = (uint32*)(base + BUF_OFFSET);
	for (y = 0; y < VIEW_H; y++) {
		for (x = 0; x < VIEW_W; x++) {
			uint32 c;
			if (x < VIEW_W / 4) c = 0x00ff0000;			/* 빨강 */
			else if (x < VIEW_W / 2) c = 0x0000ff00;	/* 초록 */
			else if (x < VIEW_W * 3 / 4) c = 0x000000ff;/* 파랑 */
			else c = 0x00ffffff;						/* 흰색 */
			video[y * VIEW_W + x] = c;
		}
	}

	/* 레지스터 버퍼 */
	ov = (struct overlay_registers*)(base + REG_OFFSET);
	memset(ov, 0, sizeof(struct overlay_registers));

	if (useFramebuffer) {
		/* 원본을 화면 자체로 삼는다. 이 영역은 디스플레이가 이미 읽고
		   있으므로 주소가 유효한지 의심할 여지가 없다. 오버레이가 뜨면
		   바탕화면 좌측 상단이 그 자리에 복제되어 보인다. */
		ov->buffer_rgb0 = 0;
		ov->stride_rgb = (uint16)stride;
	} else {
		ov->buffer_rgb0 = bufPhys ? (stolenBase + BUF_OFFSET) : BUF_OFFSET;
		ov->stride_rgb = VIEW_W * 4;
	}
	ov->window_left = WIN_X;
	ov->window_top = WIN_Y;
	ov->window_width = VIEW_W;
	ov->window_height = VIEW_H;
	ov->source_width_rgb = VIEW_W;
	ov->source_height_rgb = VIEW_H;
	/* 8xx 가 아닌 쪽 계산식 - yaddress 는 버퍼 오프셋, yswidth 는 폭*2 */
	{
		int yaddress = useFramebuffer ? 0 : BUF_OFFSET;
		int yswidth = VIEW_W << 1;
		ov->source_bytes_per_row_rgb = (((((yaddress + yswidth + 0x3f) >> 6)
			- (yaddress >> 6)) << 1) - 1) << 2;
	}
	/* 1:1 - 배율 4096 */
	ov->scale_rgb.horizontal_downscale_factor = 1;
	ov->scale_rgb.horizontal_scale_fraction = 0;
	ov->scale_rgb.vertical_scale_fraction = 0;
	ov->scale_uv.horizontal_downscale_factor = 0;
	ov->scale_uv.horizontal_scale_fraction = 0x800;
	ov->scale_uv.vertical_scale_fraction = 0x800;
	ov->vertical_scale_rgb = 1;
	ov->vertical_scale_uv = 0;

	ov->color_control_output_mode = 1;
	ov->select_pipe = 1;			/* 이 기기의 패널은 파이프 B 다 */
	ov->slot_time = slotTime;
	ov->source_format = OVERLAY_FORMAT_RGB32;
	ov->mirroring_mode = OVERLAY_MIRROR_NORMAL;
	ov->color_key_enabled = 0;
	ov->overlay_enabled = 1;

	address = ovaddPhys ? (stolenBase + REG_OFFSET) : REG_OFFSET;

	printf("slot_time %d\n", slotTime);
	printf("\n전  OVADD %08lx  DOVSTA %08lx  OVTEST %08lx\n",
		(unsigned long)rd(OV_UPDATE), (unsigned long)rd(OV_STATUS),
		(unsigned long)rd(OV_TEST));
	printf("    DSPARB %08lx  FW1 %08lx  FW2 %08lx  FW3 %08lx\n",
		(unsigned long)rd(0x70030), (unsigned long)rd(0x70034),
		(unsigned long)rd(0x70038), (unsigned long)rd(0x7003c));
	printf("    FW4 %08lx  FW5 %08lx  FW6 %08lx\n",
		(unsigned long)rd(0x70050), (unsigned long)rd(0x70054),
		(unsigned long)rd(0x70058));

	/* 필터 계수 - 1:1 이므로 차단 주파수 1.0 */
	{
		struct phase_coefficient coeff[NUM_HORIZONTAL_TAPS * NUM_PHASES];
		struct phase_coefficient coeffUV[NUM_HORIZONTAL_UV_TAPS * NUM_PHASES];
		int i, j, pos;

		if (withCoefficients) {
			update_coefficients(NUM_HORIZONTAL_TAPS, 1.0, 1, 1, coeff);
			update_coefficients(NUM_HORIZONTAL_UV_TAPS, 1.0, 1, 0, coeffUV);
			pos = 0;
			for (i = 0; i < NUM_PHASES; i++) {
				for (j = 0; j < NUM_HORIZONTAL_TAPS; j++) {
					ov->horizontal_coefficients_rgb[pos]
						= (unsigned short)((coeff[pos].sign << 15)
							| (coeff[pos].exponent << 12)
							| coeff[pos].mantissa);
					pos++;
				}
			}
			pos = 0;
			for (i = 0; i < NUM_PHASES; i++) {
				for (j = 0; j < NUM_HORIZONTAL_UV_TAPS; j++) {
					ov->horizontal_coefficients_uv[pos]
						= (unsigned short)((coeffUV[pos].sign << 15)
							| (coeffUV[pos].exponent << 12)
							| coeffUV[pos].mantissa);
					pos++;
				}
			}
			printf("필터 계수 채움 (첫 5개 RGB: %04x %04x %04x %04x %04x)\n",
				ov->horizontal_coefficients_rgb[0],
				ov->horizontal_coefficients_rgb[1],
				ov->horizontal_coefficients_rgb[2],
				ov->horizontal_coefficients_rgb[3],
				ov->horizontal_coefficients_rgb[4]);
		} else
			printf("필터 계수 비움\n");
	}

	oldArb = rd(0x70030);
	oldFw1 = rd(0x70034);
	if (newArb != 0) {
		printf("DSPARB %08lx -> %08lx\n", (unsigned long)oldArb,
			(unsigned long)newArb);
		wr(0x70030, newArb);
		(void)rd(0x70030);
	}
	if (newFw1 != 0) {
		printf("DSPFW1 %08lx -> %08lx\n", (unsigned long)oldFw1,
			(unsigned long)newFw1);
		wr(0x70034, newFw1);
		(void)rd(0x70034);
	}
	printf("PIPEBSTAT 전 %08lx\n", (unsigned long)rd(0x71024));
	wr(0x71024, rd(0x71024));	/* 언더런 상태 비트 지우고 시작 */

	wr(OV_UPDATE, address | 1);		/* 비트 0: 계수까지 적재 */
	(void)rd(OV_UPDATE);
	snooze(100000);

	printf("후  OVADD %08lx  DOVSTA %08lx  OVTEST %08lx  EXT %08lx\n",
		(unsigned long)rd(OV_UPDATE), (unsigned long)rd(OV_STATUS),
		(unsigned long)rd(OV_TEST), (unsigned long)rd(OV_EXT_STATUS));

	/* 적재된 섀도 레지스터. 하드웨어가 레지스터 버퍼를 실제로 읽었는지
	   여기서 갈린다 - 우리가 써 넣은 값이 되비치면 적재는 된 것이다. */
	printf("\n섀도 레지스터 (0x30100 부터, 레지스터 버퍼와 같은 배치)\n");
	{
		uint32 off;
		for (off = 0x30100; off < 0x30170; off += 16) {
			printf("  %05lx  %08lx %08lx %08lx %08lx\n", (unsigned long)off,
				(unsigned long)rd(off), (unsigned long)rd(off + 4),
				(unsigned long)rd(off + 8), (unsigned long)rd(off + 12));
		}
		printf("  우리가 쓴 OCOMD %08lx, OCONFIG %08lx\n",
			(unsigned long)*(((uint32*)ov) + 0x68 / 4),
			(unsigned long)*(((uint32*)ov) + 0x64 / 4));
	}
	snooze(200000);
	printf("\n켠 지 200 ms: PIPEBSTAT %08lx  DOVSTA %08lx\n",
		(unsigned long)rd(0x71024), (unsigned long)rd(OV_STATUS));
	printf("화면 (%d,%d) 에 320x240 이 보이는가?\n", WIN_X, WIN_Y);
	printf("%d 초 뒤에 끈다.\n", seconds);
	fflush(stdout);
	snooze((bigtime_t)seconds * 1000000);

	printf("켜 있는 동안 PIPEBSTAT %08lx (비트 31 이 서면 언더런)\n",
		(unsigned long)rd(0x71024));

	ov->overlay_enabled = 0;
	wr(OV_UPDATE, address);
	(void)rd(OV_UPDATE);
	snooze(100000);
	printf("끔. DOVSTA %08lx  PIPEBSTAT %08lx\n",
		(unsigned long)rd(OV_STATUS), (unsigned long)rd(0x71024));

	if (newArb != 0)
		wr(0x70030, oldArb);
	if (newFw1 != 0)
		wr(0x70034, oldFw1);
	wr(0x71024, rd(0x71024));
	printf("워터마크 복원. DSPARB %08lx  DSPFW1 %08lx\n",
		(unsigned long)rd(0x70030), (unsigned long)rd(0x70034));
	return 0;
}
