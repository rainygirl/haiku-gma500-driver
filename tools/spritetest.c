/* 스프라이트 평면(평면 C)을 파이프 B 위에 띄운다.
 *
 * 오버레이는 이 칩에 없다. Intel SCH US15W 데이터시트(문서 319537) 9.3.1 은
 * 평면을 Display / Cursor / VGA 셋만 열거하고, 문서 전체에 overlay 라는 말이
 * 한 번도 나오지 않는다. 0x30000 의 레지스터 블록은 i915 에서 물려받은
 * 잔재다 - 레지스터 버퍼는 적재되지만 평면이 연결돼 있지 않아 켜면 디스플레이
 * 메모리 경로가 엉킨다.
 *
 * 대신 데이터시트가 보장하는 것이 있다. "The secondary display plane can be
 * used ... as a sprite plane on either the primary or secondary display."
 * 평면 C 는 1차 평면과 같은 레지스터 계열이고 위치·크기·컬러키를 갖는다.
 */
#include <Drivers.h>
#include <PCI.h>
#include <OS.h>
#include <poke.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 평면 A 와 C 의 레지스터는 0x1000 간격으로 같은 배치다 */
#define PLANE_A_BASE	0x70180
#define PLANE_C_BASE	0x72180
#define REG_CNTR(b)		((b) + 0x00)
#define REG_LINOFF(b)	((b) + 0x04)
#define REG_STRIDE(b)	((b) + 0x08)
#define REG_POS(b)		((b) + 0x0c)
#define REG_SIZE(b)		((b) + 0x10)
#define REG_SURF(b)		((b) + 0x1c)
#define REG_TILEOFF(b)	((b) + 0x24)
#define PIPEBSTAT	0x71024
#define DSPARB		0x70030
#define DSPFW2		0x70038

#define PLANE_ENABLE		(1u << 31)
#define PLANE_32BPP_NO_ALPHA	(0x6u << 26)
#define PLANE_SEL_PIPE_B	(1u << 24)
#define SPRITE_ABOVE_OVERLAY	1u

#define BUF_OFFSET	0x610000
#define VIEW_W		320
#define VIEW_H		240
#define WIN_X		600
#define WIN_Y		250

static volatile uint8* sRegs;
static uint32 rd(uint32 o) { return *(volatile uint32*)(sRegs + o); }
static void wr(uint32 o, uint32 v) { *(volatile uint32*)(sRegs + o) = v; }


static uint32 sBase;

static void
show(const char* when)
{
	printf("%s CNTR %08lx  POS %08lx  SIZE %08lx  LINOFF %08lx  "
		"PIPEBSTAT %08lx\n", when,
		(unsigned long)rd(REG_CNTR(sBase)), (unsigned long)rd(REG_POS(sBase)),
		(unsigned long)rd(REG_SIZE(sBase)), (unsigned long)rd(REG_LINOFF(sBase)),
		(unsigned long)rd(PIPEBSTAT));
}


int
main(int argc, char** argv)
{
	pci_info info;
	pci_info_args args;
	mem_map_args mmio, stolen, gtt;
	uint8* base;
	uint32* video;
	uint32 stolenBase, stride, height, cntr;
	uint32 bytesPerRow;
	int seconds = 5;
	int zorder = 1;
	uint32 planeBase = PLANE_C_BASE;
	const char* planeName = "C";
	uint32 format = 0x6;			/* 32비트 XRGB */
	int index, x, y;
	int fd;

	if (argc > 1)
		seconds = atoi(argv[1]);
	if (argc > 2)
		zorder = atoi(argv[2]);
	if (argc > 3 && argv[3][0] == 'a') {
		planeBase = PLANE_A_BASE;
		planeName = "A";
	}
	if (argc > 4)
		format = (uint32)strtoul(argv[4], NULL, 16);

	fd = open(POKE_DEVICE_FULLNAME, O_RDWR);
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
	mmio.name = "sprite";
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
	stolen.size = 0x700000;
	stolen.flags = B_ANY_ADDRESS;
	stolen.protection = B_READ_AREA | B_WRITE_AREA;
	if (ioctl(fd, POKE_MAP_MEMORY, &stolen, sizeof(stolen)) < 0)
		return 1;
	base = (uint8*)stolen.address;

	sBase = planeBase;
	printf("화면 %lux%lu, 스톨른 %08lx, 평면 %s, Z순서 %d\n",
		(unsigned long)(stride / 4), (unsigned long)height,
		(unsigned long)stolenBase, planeName, zorder);
	printf("픽셀 포맷 필드 %lx\n", (unsigned long)format);
	printf("1차 평면 DSPBCNTR %08lx (비교용)\n", (unsigned long)rd(0x71180));
	printf("DSPARB %08lx  DSPFW2 %08lx\n", (unsigned long)rd(DSPARB),
		(unsigned long)rd(DSPFW2));

	/* 색 띠를 그린다. 포맷이 0x6/0x7 이면 RGB32 로, 그 밖이면 YCbCr422
	   로 같은 네 가지 색을 만든다. YUV 로 제대로 해석되면 RGB 때와 똑같은
	   띠가 보이고, 지원하지 않으면 깨져 보인다. */
	video = (uint32*)(base + BUF_OFFSET);
	if (format == 0x6 || format == 0x7) {
		for (y = 0; y < VIEW_H; y++) {
			for (x = 0; x < VIEW_W; x++) {
				uint32 c;
				if (x < VIEW_W / 4) c = 0x00ff0000;
				else if (x < VIEW_W / 2) c = 0x0000ff00;
				else if (x < VIEW_W * 3 / 4) c = 0x000000ff;
				else c = 0x00ffffff;
				video[y * VIEW_W + x] = c;
			}
		}
		bytesPerRow = VIEW_W * 4;
	} else {
		/* YCbCr422, 두 픽셀이 4 바이트: Y0 Cb Y1 Cr */
		uint8* row = (uint8*)video;
		for (y = 0; y < VIEW_H; y++) {
			for (x = 0; x < VIEW_W; x += 2) {
				uint8 yy, cb, cr;
				if (x < VIEW_W / 4) { yy = 81; cb = 90; cr = 240; }
				else if (x < VIEW_W / 2) { yy = 145; cb = 54; cr = 34; }
				else if (x < VIEW_W * 3 / 4) { yy = 41; cb = 240; cr = 110; }
				else { yy = 235; cb = 128; cr = 128; }
				row[y * (VIEW_W * 2) + x * 2 + 0] = yy;
				row[y * (VIEW_W * 2) + x * 2 + 1] = cb;
				row[y * (VIEW_W * 2) + x * 2 + 2] = yy;
				row[y * (VIEW_W * 2) + x * 2 + 3] = cr;
			}
		}
		bytesPerRow = VIEW_W * 2;
	}

	show("전 ");

	/* 평면 C 설정. SURF 는 GTT 오프셋이다 - 1차 평면이 오프셋 0 으로
	   프레임버퍼를 가리키는 것과 같은 방식이다. */
	wr(REG_STRIDE(planeBase), bytesPerRow);
	wr(REG_POS(planeBase), ((uint32)WIN_Y << 16) | (uint32)WIN_X);
	wr(REG_SIZE(planeBase), ((uint32)(VIEW_H - 1) << 16) | (uint32)(VIEW_W - 1));
	/* 3 세대는 LINOFF(=BASE) 가 주소 레지스터다. SURF 는 4 세대부터. */
	wr(REG_TILEOFF(planeBase), 0);
	cntr = PLANE_ENABLE | (format << 26) | PLANE_SEL_PIPE_B
		| (uint32)zorder;
	wr(REG_CNTR(planeBase), cntr);
	wr(REG_LINOFF(planeBase), BUF_OFFSET);	/* 이 쓰기가 반영시킨다 */
	(void)rd(REG_LINOFF(planeBase));
	snooze(200000);
	printf("  CNTR 을 %08lx 로 씀 -> 되읽기 %08lx\n",
		(unsigned long)cntr, (unsigned long)rd(REG_CNTR(planeBase)));

	show("켬 ");
	printf("\n화면 (%d,%d) 에 320x240 색 띠(빨강/초록/파랑/흰색)가 보이는가?\n",
		WIN_X, WIN_Y);
	printf("%d 초 뒤에 끈다.\n", seconds);
	fflush(stdout);
	snooze((bigtime_t)seconds * 1000000);

	/* 끄기 */
	wr(REG_CNTR(planeBase), 0);
	wr(REG_LINOFF(planeBase), 0);
	(void)rd(REG_LINOFF(planeBase));
	snooze(100000);
	wr(PIPEBSTAT, rd(PIPEBSTAT));
	show("끔 ");
	return 0;
}
