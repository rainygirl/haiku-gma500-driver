/* 스톨른 메모리가 얼마나 있고 프레임버퍼가 얼마를 쓰는지 본다.
 * 오버레이 버퍼를 놓을 자리가 남는지 판단하기 위한 것이다.
 */
#include <Drivers.h>
#include <PCI.h>
#include <OS.h>
#include <poke.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int
main(void)
{
	pci_info info;
	pci_info_args args;
	mem_map_args mmio, gtt;
	volatile uint8* regs;
	volatile uint32* entries;
	uint32 stride, height, first, last, count, i, entryCount;
	uint32 fbBytes;
	int index;
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
	fbBytes = stride * height;

	printf("BAR0 %08lx (%lu KB)\n", (unsigned long)info.u.h0.base_registers[0],
		(unsigned long)info.u.h0.base_register_sizes[0] / 1024);
	printf("BAR2 (GTT 창) %08lx (%lu MB)\n",
		(unsigned long)info.u.h0.base_registers[2],
		(unsigned long)info.u.h0.base_register_sizes[2] / (1024 * 1024));
	printf("BAR3 (GTT 표) %08lx (%lu KB)\n",
		(unsigned long)info.u.h0.base_registers[3],
		(unsigned long)info.u.h0.base_register_sizes[3] / 1024);

	memset(&gtt, 0, sizeof(gtt));
	gtt.signature = POKE_SIGNATURE;
	gtt.name = "gtt";
	gtt.physical_address = info.u.h0.base_registers[3];
	gtt.size = info.u.h0.base_register_sizes[3];
	gtt.flags = B_ANY_ADDRESS;
	gtt.protection = B_READ_AREA;
	if (ioctl(fd, POKE_MAP_MEMORY, &gtt, sizeof(gtt)) < 0)
		return 1;
	entries = (volatile uint32*)gtt.address;
	entryCount = info.u.h0.base_register_sizes[3] / 4;

	first = entries[0] & ~0xfff;
	last = first;
	count = 0;
	for (i = 0; i < entryCount; i++) {
		uint32 e = entries[i];
		if ((e & 1) == 0)		/* valid 비트 */
			break;
		last = e & ~0xfff;
		count++;
	}
	printf("\nGTT 항목 %lu 개 중 유효 %lu 개 (%lu KB 매핑)\n",
		(unsigned long)entryCount, (unsigned long)count,
		(unsigned long)count * 4);
	printf("첫 페이지 %08lx, 마지막 %08lx\n", (unsigned long)first,
		(unsigned long)last);
	printf("연속이라면 크기 %lu KB\n",
		(unsigned long)(last - first + 4096) / 1024);

	printf("\n화면 %lux%lu, stride %lu -> 프레임버퍼 %lu KB\n",
		(unsigned long)(stride / 4), (unsigned long)height,
		(unsigned long)stride, (unsigned long)fbBytes / 1024);
	if ((uint32)count * 4096 > fbBytes) {
		printf("남는 스톨른 %lu KB\n",
			(unsigned long)((uint32)count * 4096 - fbBytes) / 1024);
	} else
		printf("남는 스톨른 없음\n");
	return 0;
}
