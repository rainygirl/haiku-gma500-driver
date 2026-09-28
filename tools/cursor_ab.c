/* 커서 버퍼를 어디에 두어야 하는지 가리는 A/B 실험.
 *
 *   A (빨강) = 잠근 일반 시스템 RAM 의 연속 16 KB
 *   B (파랑) = 스톨른 메모리(프레임버퍼 뒤쪽)의 16 KB
 *
 * 리눅스 gma500 은 psb_intel_display.c 에서 B 를 쓴다 - "Allocate 4 pages of
 * stolen mem for a hardware cursor". 커서 페치 경로가 스톨른 창만 다룰 수
 * 있다면 A 는 아무것도 못 그린다. 눈으로 가리는 것이 가장 빠르다.
 *
 * 스톨른 영역은 쓰기 전에 원래 내용을 저장했다가 되돌린다.
 */
#include <Drivers.h>
#include <PCI.h>
#include <OS.h>
#include <poke.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CURBCNTR	0x700c0
#define CURBBASE	0x700c4
#define CURBPOS		0x700c8
#define DSPBSTRIDE	0x71188
#define PIPEBSRC	0x6101c
#define DSPCLK_GATE_D	0x6200

#define CURSOR_MODE_64_ARGB_AX	((1 << 5) | 0x07)
#define MCURSOR_GAMMA_ENABLE	(1 << 26)
#define MCURSOR_PIPE_B			(1 << 28)

#define CURSOR_BYTES	(64 * 64 * 4)
#define SEARCH_PAGES	2048

static int sFD;
static volatile uint8* sRegs;

static void wr(uint32 o, uint32 v) { *(volatile uint32*)(sRegs + o) = v; }
static uint32 rd(uint32 o) { return *(volatile uint32*)(sRegs + o); }


static phys_addr_t
physical_of(void* address)
{
	mem_map_args phys;
	memset(&phys, 0, sizeof(phys));
	phys.signature = POKE_SIGNATURE;
	phys.address = address;
	phys.size = 4096;
	if (ioctl(sFD, POKE_GET_PHYSICAL_ADDRESS, &phys, sizeof(phys)) != B_OK)
		return 0;
	return phys.physical_address;
}


static void
paint(uint8* image, uint32 border, uint32 diagonal)
{
	int x, y;
	for (y = 0; y < 64; y++) {
		for (x = 0; x < 64; x++) {
			uint32* p = (uint32*)(image + (y * 64 + x) * 4);
			if (x < 5 || x > 58 || y < 5 || y > 58)
				*p = border;
			else if (x == y || x == 63 - y)
				*p = diagonal;
			else
				*p = 0x30000000;
		}
	}
}


static void
run_phase(const char* label, uint32 physical, int seconds)
{
	int i;
	printf("\n[%s] CURBBASE = %08lx 로 %d초\n", label,
		(unsigned long)physical, seconds);
	fflush(stdout);

	wr(CURBPOS, (300 << 16) | 400);
	wr(CURBCNTR, MCURSOR_PIPE_B | CURSOR_MODE_64_ARGB_AX | MCURSOR_GAMMA_ENABLE);
	wr(CURBBASE, physical);
	printf("  읽기: CNTR %08lx BASE %08lx\n", (unsigned long)rd(CURBCNTR),
		(unsigned long)rd(CURBBASE));
	fflush(stdout);

	/* 화면 한가운데를 천천히 가로지른다. 놓치기 어렵게. */
	for (i = 0; i < seconds * 20; i++) {
		int px = 60 + (i * 12) % 1480;
		wr(CURBPOS, (350 << 16) | px);
		wr(CURBBASE, physical);		/* 매번 다시 무장 (더블버퍼 갱신) */
		snooze(50000);
	}
	wr(CURBCNTR, 0);
	wr(CURBBASE, 0);
}


int
main(int argc, char** argv)
{
	pci_info info;
	pci_info_args args;
	mem_map_args mmio, stolen;
	area_id area;
	uint8* buffer;
	uint8* ramCursor = NULL;
	uint8* stolenCursor;
	uint8 backup[CURSOR_BYTES];
	phys_addr_t ramPhys = 0;
	phys_addr_t stolenBase, stolenCursorPhys;
	uint32 fbBytes;
	uint32 savedCntr, savedBase, savedPos;
	int index, i;
	int seconds = argc > 1 ? atoi(argv[1]) : 20;

	sFD = open(POKE_DEVICE_FULLNAME, O_RDWR);
	if (sFD < 0)
		return 1;

	args.signature = POKE_SIGNATURE;
	args.info = &info;
	for (index = 0; index < 255; index++) {
		args.index = index;
		if (ioctl(sFD, POKE_GET_NTH_PCI_INFO, &args, sizeof(args)) != B_OK
			|| args.status != B_OK)
			return 1;
		if (info.vendor_id == 0x8086 && info.device_id == 0x8108)
			break;
	}

	memset(&mmio, 0, sizeof(mmio));
	mmio.signature = POKE_SIGNATURE;
	mmio.name = "gma mmio";
	mmio.physical_address = info.u.h0.base_registers[0];
	mmio.size = info.u.h0.base_register_sizes[0];
	mmio.flags = B_ANY_ADDRESS;
	mmio.protection = B_READ_AREA | B_WRITE_AREA;
	if (ioctl(sFD, POKE_MAP_MEMORY, &mmio, sizeof(mmio)) < 0)
		return 1;
	sRegs = (volatile uint8*)mmio.address;

	printf("DSPCLK_GATE_D %08lx   (커서 클럭이 막혀 있으면 여기 보인다)\n",
		(unsigned long)rd(DSPCLK_GATE_D));

	savedCntr = rd(CURBCNTR);
	savedBase = rd(CURBBASE);
	savedPos = rd(CURBPOS);

	/* --- A: 시스템 RAM --- */
	area = create_area("gma cursor search", (void**)&buffer, B_ANY_ADDRESS,
		SEARCH_PAGES * 4096, B_FULL_LOCK, B_READ_AREA | B_WRITE_AREA);
	if (area >= 0) {
		for (i = 0; i + 3 < SEARCH_PAGES; i++) {
			phys_addr_t p0 = physical_of(buffer + (size_t)i * 4096);
			if (p0 == 0 || (p0 & 0x3fff) != 0)
				continue;
			if (physical_of(buffer + (size_t)(i + 1) * 4096) == p0 + 4096
				&& physical_of(buffer + (size_t)(i + 2) * 4096) == p0 + 8192
				&& physical_of(buffer + (size_t)(i + 3) * 4096) == p0 + 12288) {
				ramCursor = buffer + (size_t)i * 4096;
				ramPhys = p0;
				break;
			}
		}
	}
	if (ramCursor != NULL) {
		paint(ramCursor, 0xffff0000, 0xff00ff00);	/* 빨강 테두리 */
		run_phase("A 시스템 RAM = 빨강", (uint32)ramPhys, seconds);
	} else
		printf("A: 연속 16 KB 를 못 찾아 건너뛴다\n");

	/* --- B: 스톨른 메모리 --- */
	{
		mem_map_args gtt;
		volatile uint32* pte;
		memset(&gtt, 0, sizeof(gtt));
		gtt.signature = POKE_SIGNATURE;
		gtt.name = "gma gtt";
		gtt.physical_address = info.u.h0.base_registers[3];
		gtt.size = info.u.h0.base_register_sizes[3];
		gtt.flags = B_ANY_ADDRESS;
		gtt.protection = B_READ_AREA | B_WRITE_AREA;
		if (ioctl(sFD, POKE_MAP_MEMORY, &gtt, sizeof(gtt)) < 0)
			return 1;
		pte = (volatile uint32*)gtt.address;
		stolenBase = pte[0] & ~0xfff;			/* 프레임버퍼 0번 칸 = 스톨른 시작 */
		ioctl(sFD, POKE_UNMAP_MEMORY, &gtt, sizeof(gtt));
	}
	fbBytes = ((rd(PIPEBSRC) >> 16) + 1) * 0 + (rd(DSPBSTRIDE) & 0xffff)
		* ((rd(PIPEBSRC) & 0xffff) + 1);
	/* 프레임버퍼 뒤, 1 MB 여유를 두고 16 KB 정렬 */
	stolenCursorPhys = (stolenBase + fbBytes + 0x100000 + 0x3fff) & ~0x3fffULL;

	printf("\n스톨른 시작 %08lx, 프레임버퍼 %lu 바이트 -> 커서 %08lx\n",
		(unsigned long)stolenBase, (unsigned long)fbBytes,
		(unsigned long)stolenCursorPhys);

	memset(&stolen, 0, sizeof(stolen));
	stolen.signature = POKE_SIGNATURE;
	stolen.name = "gma stolen cursor";
	stolen.physical_address = stolenCursorPhys;
	stolen.size = CURSOR_BYTES;
	stolen.flags = B_ANY_ADDRESS;
	stolen.protection = B_READ_AREA | B_WRITE_AREA;
	if (ioctl(sFD, POKE_MAP_MEMORY, &stolen, sizeof(stolen)) < 0) {
		printf("스톨른 매핑 실패\n");
	} else {
		stolenCursor = (uint8*)stolen.address;
		memcpy(backup, stolenCursor, CURSOR_BYTES);
		paint(stolenCursor, 0xff0000ff, 0xffffff00);	/* 파랑 테두리 */
		run_phase("B 스톨른 = 파랑", (uint32)stolenCursorPhys, seconds);
		memcpy(stolenCursor, backup, CURSOR_BYTES);
		ioctl(sFD, POKE_UNMAP_MEMORY, &stolen, sizeof(stolen));
	}

	wr(CURBCNTR, savedCntr);
	wr(CURBBASE, savedBase);
	wr(CURBPOS, savedPos);
	if (area >= 0)
		delete_area(area);
	printf("\n끝. 되돌렸다.\n");
	return 0;
}
