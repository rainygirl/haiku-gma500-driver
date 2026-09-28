/* 프레임버퍼에 직접 그린다 - 눈이 아니라 스크린샷으로 판정하기 위한 검증.
 *
 * app_server 의 스크린샷은 같은 물리 메모리를 읽으므로, 여기서 그린 사각형이
 * 스크린샷에 찍히면 (1) 스톨른 물리 주소 계산, (2) poke 매핑, (3) 픽셀 형식
 * 해석이 모두 맞다는 뜻이다. 그러면 커서가 안 보이는 원인은 커서 평면 자체로
 * 좁혀진다.
 *
 * 그린 자리는 원래 픽셀을 저장했다가 되돌린다.
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

#define DSPBSTRIDE	0x71188
#define PIPEBSRC	0x6101c

#define RECT_X		100
#define RECT_Y		100
#define RECT_W		300
#define RECT_H		200

int
main(int argc, char** argv)
{
	pci_info info;
	pci_info_args args;
	mem_map_args mmio, fb;
	volatile uint8* regs;
	uint8* pixels;
	uint32 stride, height, fbBytes;
	phys_addr_t stolenBase;
	uint32* backup;
	int index, x, y;
	int seconds = argc > 1 ? atoi(argv[1]) : 6;

	int fd = open(POKE_DEVICE_FULLNAME, O_RDWR);
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
	mmio.name = "gma mmio";
	mmio.physical_address = info.u.h0.base_registers[0];
	mmio.size = info.u.h0.base_register_sizes[0];
	mmio.flags = B_ANY_ADDRESS;
	mmio.protection = B_READ_AREA | B_WRITE_AREA;
	if (ioctl(fd, POKE_MAP_MEMORY, &mmio, sizeof(mmio)) < 0)
		return 1;
	regs = (volatile uint8*)mmio.address;
	stride = *(volatile uint32*)(regs + DSPBSTRIDE) & 0xffff;
	height = (*(volatile uint32*)(regs + PIPEBSRC) & 0xffff) + 1;
	fbBytes = stride * height;

	{	/* 프레임버퍼의 물리 주소는 GTT 0번 칸에 적혀 있다 */
		mem_map_args gtt;
		memset(&gtt, 0, sizeof(gtt));
		gtt.signature = POKE_SIGNATURE;
		gtt.name = "gma gtt";
		gtt.physical_address = info.u.h0.base_registers[3];
		gtt.size = info.u.h0.base_register_sizes[3];
		gtt.flags = B_ANY_ADDRESS;
		gtt.protection = B_READ_AREA | B_WRITE_AREA;
		if (ioctl(fd, POKE_MAP_MEMORY, &gtt, sizeof(gtt)) < 0)
			return 1;
		stolenBase = *(volatile uint32*)gtt.address & ~0xfff;
		ioctl(fd, POKE_UNMAP_MEMORY, &gtt, sizeof(gtt));
	}
	printf("스톨른 %08lx, stride %lu, height %lu, 프레임버퍼 %lu 바이트\n",
		(unsigned long)stolenBase, (unsigned long)stride,
		(unsigned long)height, (unsigned long)fbBytes);

	memset(&fb, 0, sizeof(fb));
	fb.signature = POKE_SIGNATURE;
	fb.name = "gma framebuffer";
	fb.physical_address = stolenBase;
	fb.size = fbBytes;
	fb.flags = B_ANY_ADDRESS;
	fb.protection = B_READ_AREA | B_WRITE_AREA;
	if (ioctl(fd, POKE_MAP_MEMORY, &fb, sizeof(fb)) < 0) {
		printf("프레임버퍼 매핑 실패\n");
		return 1;
	}
	pixels = (uint8*)fb.address;

	backup = (uint32*)malloc(RECT_W * RECT_H * 4);
	for (y = 0; y < RECT_H; y++) {
		for (x = 0; x < RECT_W; x++) {
			uint32* p = (uint32*)(pixels + (RECT_Y + y) * stride
				+ (RECT_X + x) * 4);
			backup[y * RECT_W + x] = *p;
			*p = ((x / 20 + y / 20) % 2) ? 0x00ff2020 : 0x0020ff20;
		}
	}
	printf("(%d,%d) 에 %dx%d 체크무늬를 그렸다. %d초 뒤 되돌린다.\n",
		RECT_X, RECT_Y, RECT_W, RECT_H, seconds);
	fflush(stdout);

	snooze((bigtime_t)seconds * 1000000);

	for (y = 0; y < RECT_H; y++) {
		for (x = 0; x < RECT_W; x++) {
			*(uint32*)(pixels + (RECT_Y + y) * stride + (RECT_X + x) * 4)
				= backup[y * RECT_W + x];
		}
	}
	printf("되돌렸다.\n");
	return 0;
}
