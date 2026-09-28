/* CURBCNTR 의 감마 비트(26)를 살아 있는 상태에서 끄고 켠다.
 * app_server 는 커서 모양이 바뀔 때만 이 레지스터를 쓰므로, 여기서 바꾼 값이
 * 한동안 유지된다. 재부팅도 app_server 재시작도 필요 없는 실험이다. */
#include <Drivers.h>
#include <PCI.h>
#include <OS.h>
#include <poke.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CURBCNTR	0x700c0
#define GAMMA		(1 << 26)

int
main(int argc, char** argv)
{
	pci_info info;
	pci_info_args args;
	mem_map_args mmio;
	volatile uint32* reg;
	uint32 value;
	int index;
	int on = argc > 1 && strcmp(argv[1], "on") == 0;
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
	mmio.name = "gamma toggle";
	mmio.physical_address = info.u.h0.base_registers[0];
	mmio.size = info.u.h0.base_register_sizes[0];
	mmio.flags = B_ANY_ADDRESS;
	mmio.protection = B_READ_AREA | B_WRITE_AREA;
	if (ioctl(fd, POKE_MAP_MEMORY, &mmio, sizeof(mmio)) < 0)
		return 1;

	reg = (volatile uint32*)((uint8*)mmio.address + CURBCNTR);
	value = *reg;
	printf("이전 CURBCNTR %08lx (감마 %s)\n", (unsigned long)value,
		(value & GAMMA) ? "켜짐" : "꺼짐");
	value = on ? (value | GAMMA) : (value & ~GAMMA);
	*reg = value;
	printf("이후 CURBCNTR %08lx (감마 %s)\n", (unsigned long)*reg,
		(*reg & GAMMA) ? "켜짐" : "꺼짐");
	return 0;
}
