/* app_server 가 실제로 하는 두 가지 복사의 비용을 잰다.
 *   1) 백버퍼(시스템 RAM) 안에서의 복사  - DrawingEngine::CopyRect
 *   2) 백버퍼 -> 프레임버퍼 밀어넣기      - _CopyBackToFront
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

#define WIDTH	800
#define HEIGHT	500
#define ROUNDS	20

int
main(void)
{
	pci_info info;
	pci_info_args args;
	mem_map_args mmio, fb, gtt;
	volatile uint8* regs;
	uint8 *pixels, *back, *saved;
	uint32 stride, height, stolen;
	bigtime_t start, ramTime, pushTime;
	int index, round, row;
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
	mmio.name = "regs";
	mmio.physical_address = info.u.h0.base_registers[0];
	mmio.size = info.u.h0.base_register_sizes[0];
	mmio.flags = B_ANY_ADDRESS;
	mmio.protection = B_READ_AREA | B_WRITE_AREA;
	if (ioctl(fd, POKE_MAP_MEMORY, &mmio, sizeof(mmio)) < 0)
		return 1;
	regs = (volatile uint8*)mmio.address;
	stride = *(volatile uint32*)(regs + 0x71188) & 0xffff;
	height = (*(volatile uint32*)(regs + 0x6101c) & 0xffff) + 1;

	memset(&gtt, 0, sizeof(gtt));
	gtt.signature = POKE_SIGNATURE;
	gtt.name = "gtt";
	gtt.physical_address = info.u.h0.base_registers[3];
	gtt.size = info.u.h0.base_register_sizes[3];
	gtt.flags = B_ANY_ADDRESS;
	gtt.protection = B_READ_AREA;
	if (ioctl(fd, POKE_MAP_MEMORY, &gtt, sizeof(gtt)) < 0)
		return 1;
	stolen = *(volatile uint32*)gtt.address & ~0xfff;
	ioctl(fd, POKE_UNMAP_MEMORY, &gtt, sizeof(gtt));

	memset(&fb, 0, sizeof(fb));
	fb.signature = POKE_SIGNATURE;
	fb.name = "fb";
	fb.physical_address = stolen;
	fb.size = stride * height;
	fb.flags = B_ANY_ADDRESS;
	fb.protection = B_READ_AREA | B_WRITE_AREA;
	if (ioctl(fd, POKE_MAP_MEMORY, &fb, sizeof(fb)) < 0)
		return 1;
	pixels = (uint8*)fb.address;

	back = (uint8*)malloc(stride * (HEIGHT + 2));
	saved = (uint8*)malloc(stride * (HEIGHT + 2));
	if (back == NULL || saved == NULL)
		return 1;
	memcpy(saved, pixels, stride * (HEIGHT + 2));
	memcpy(back, pixels, stride * (HEIGHT + 2));

	/* 1) 백버퍼 안에서의 복사 - 캐시가 도는 보통 메모리 */
	start = system_time();
	for (round = 0; round < ROUNDS; round++) {
		for (row = HEIGHT - 1; row >= 0; row--)
			memcpy(back + (row + 1) * stride, back + row * stride, WIDTH * 4);
	}
	ramTime = system_time() - start;

	/* 2) 백버퍼 -> 프레임버퍼 */
	start = system_time();
	for (round = 0; round < ROUNDS; round++) {
		for (row = 0; row < HEIGHT; row++)
			memcpy(pixels + row * stride, back + row * stride, WIDTH * 4);
	}
	pushTime = system_time() - start;

	memcpy(pixels, saved, stride * (HEIGHT + 2));

	printf("%dx%d 구역, %d 회\n", WIDTH, HEIGHT, ROUNDS);
	printf("백버퍼 안 복사     한 번 %5lld us\n", ramTime / ROUNDS);
	printf("백버퍼 -> 화면     한 번 %5lld us\n", pushTime / ROUNDS);
	return 0;
}
