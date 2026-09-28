/* Poulsbo 하드웨어 커서 점등 실험 (2차).
 *
 * 1차에서 틀린 두 가지를 리눅스 gma500 원본으로 확인해 고쳤다.
 *
 *  - Poulsbo 는 psb_device.c 에서 `.cursor_needs_phys = 1` 이다. 커서 베이스
 *    레지스터는 GTT 오프셋이 아니라 **물리 주소**를 받는다. 그래서 GTT 는
 *    건드리지 않고, 물리적으로 연속인 16 KB 를 찾아 그 주소를 그대로 쓴다.
 *  - 커서를 파이프 B 로 보내려면 제어 레지스터에 (pipe << 28) 이 필요하다
 *    (gma_display.c: temp |= (pipe << 28)).
 *
 * 연속 메모리는 잠근 유저 영역에서 직접 찾는다. 유저 팀은 B_CONTIGUOUS 를
 * 요청할 수 없지만, 큰 영역을 잠그고 물리 주소를 훑으면 연속 구간이 나온다.
 * 스톨른 메모리를 쓰지 않으므로 BIOS 가 남긴 것을 건드릴 일이 없다.
 *
 * 스캔아웃 단계에서 합성되는 그림이라 screenshot 에는 찍히지 않는다.
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

#define CURSOR_MODE_64_ARGB_AX	((1 << 5) | 0x07)
#define MCURSOR_GAMMA_ENABLE	(1 << 26)
#define MCURSOR_PIPE_B			(1 << 28)

#define CURSOR_BYTES	(64 * 64 * 4)		/* 16 KB = 4 페이지 */
#define SEARCH_PAGES	2048				/* 8 MB 안에서 연속 구간을 찾는다 */

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


int
main(int argc, char** argv)
{
	pci_info info;
	pci_info_args args;
	mem_map_args mmio;
	area_id area;
	uint8* buffer;
	uint8* cursor = NULL;
	phys_addr_t cursorPhys = 0;
	uint32 savedCntr, savedBase, savedPos;
	int index, i, x, y;
	int seconds = argc > 1 ? atoi(argv[1]) : 30;

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
	if (ioctl(sFD, POKE_MAP_MEMORY, &mmio, sizeof(mmio)) < 0) {
		printf("MMIO 매핑 실패\n");
		return 1;
	}
	sRegs = (volatile uint8*)mmio.address;

	area = create_area("gma cursor search", (void**)&buffer, B_ANY_ADDRESS,
		SEARCH_PAGES * 4096, B_FULL_LOCK, B_READ_AREA | B_WRITE_AREA);
	if (area < 0) {
		printf("영역 생성 실패: %s\n", strerror(area));
		return 1;
	}

	/* 16 KB 정렬된, 물리적으로 연속인 4 페이지를 찾는다. */
	for (i = 0; i + 3 < SEARCH_PAGES; i++) {
		phys_addr_t p0 = physical_of(buffer + (size_t)i * 4096);
		if (p0 == 0 || (p0 & 0x3fff) != 0)
			continue;
		if (physical_of(buffer + (size_t)(i + 1) * 4096) == p0 + 4096
			&& physical_of(buffer + (size_t)(i + 2) * 4096) == p0 + 8192
			&& physical_of(buffer + (size_t)(i + 3) * 4096) == p0 + 12288) {
			cursor = buffer + (size_t)i * 4096;
			cursorPhys = p0;
			break;
		}
	}
	if (cursor == NULL) {
		printf("연속 16 KB 를 찾지 못했다\n");
		return 1;
	}
	printf("커서 버퍼: 물리 %08lx (검색 %d 페이지 중 %d번째)\n",
		(unsigned long)cursorPhys, SEARCH_PAGES, i);

	memset(cursor, 0, CURSOR_BYTES);
	for (y = 0; y < 64; y++) {
		for (x = 0; x < 64; x++) {
			uint32* p = (uint32*)(cursor + (y * 64 + x) * 4);
			if (x < 4 || x > 59 || y < 4 || y > 59)
				*p = 0xffff0000;			/* 불투명 빨강 테두리 */
			else if (x == y || x == 63 - y)
				*p = 0xff00ff00;			/* 초록 대각선 */
			else
				*p = 0x40000000;			/* 옅은 검정 */
		}
	}

	savedCntr = rd(CURBCNTR);
	savedBase = rd(CURBBASE);
	savedPos = rd(CURBPOS);

	wr(CURBPOS, (300 << 16) | 400);
	wr(CURBCNTR, MCURSOR_PIPE_B | CURSOR_MODE_64_ARGB_AX | MCURSOR_GAMMA_ENABLE);
	wr(CURBBASE, (uint32)cursorPhys);		/* base 쓰기가 더블버퍼를 갱신한다 */

	printf("CURBCNTR %08lx  CURBBASE %08lx  CURBPOS %08lx\n",
		(unsigned long)rd(CURBCNTR), (unsigned long)rd(CURBBASE),
		(unsigned long)rd(CURBPOS));
	printf("화면에 빨간 테두리 사각형이 대각선으로 움직이면 성공. %d초.\n", seconds);
	fflush(stdout);

	for (i = 0; i < seconds * 10; i++) {
		int px = 200 + (i * 13) % 1200;
		int py = 150 + (i * 7) % 500;
		wr(CURBPOS, (py << 16) | px);
		snooze(100000);
	}

	wr(CURBCNTR, savedCntr);
	wr(CURBBASE, savedBase);
	wr(CURBPOS, savedPos);
	delete_area(area);
	printf("되돌렸다.\n");
	return 0;
}
