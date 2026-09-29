/* 오버레이 유닛이 이 칩에 있는지 읽기만으로 본다.
 *
 * i915 계열에서 오버레이는 MMIO 0x30000 부터다. OVADD(0x30000) 에 오버레이
 * 레지스터 버퍼의 물리 주소를 넣어 갱신을 걸고, DOVSTA(0x30008) 로 상태를
 * 본다. Poulsbo 에 이 유닛이 살아 있는지 먼저 확인한다 - 쓰기는 하지 않는다.
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
	mem_map_args mmio;
	volatile uint8* regs;
	uint32 offset;
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
	mmio.name = "overlay probe";
	mmio.physical_address = info.u.h0.base_registers[0];
	mmio.size = info.u.h0.base_register_sizes[0];
	mmio.flags = B_ANY_ADDRESS;
	mmio.protection = B_READ_AREA | B_WRITE_AREA;
	if (ioctl(fd, POKE_MAP_MEMORY, &mmio, sizeof(mmio)) < 0)
		return 1;
	regs = (volatile uint8*)mmio.address;

#define SHOW(name, off) printf("  %-24s %05x  %08lx\n", name, off, \
	(unsigned long)*(volatile uint32*)(regs + (off)))

	printf("오버레이 레지스터 (BAR0 + 0x30000)\n");
	SHOW("OVADD (UPDATE)", 0x30000);
	SHOW("OVTEST", 0x30004);
	SHOW("DOVSTA (STATUS)", 0x30008);
	SHOW("OV_EXT_STATUS", 0x3000c);
	SHOW("OGAMC5", 0x30010);
	SHOW("OGAMC4", 0x30014);
	SHOW("OGAMC3", 0x30018);
	SHOW("OGAMC2", 0x3001c);
	SHOW("OGAMC1", 0x30020);
	SHOW("OGAMC0", 0x30024);

	printf("\n주변 (있으면 유닛이 붙어 있다는 뜻)\n");
	for (offset = 0x30028; offset <= 0x30040; offset += 4)
		SHOW("", offset);

	printf("\n두 번째 오버레이 (OVC, 0x38000) - 감마 기본값이 있으면 실재한다\n");
	SHOW("OVC_OVADD", 0x38000);
	SHOW("OVC_DOVCSTA", 0x38008);
	SHOW("OVC_OGAMC5", 0x38010);
	SHOW("OVC_OGAMC4", 0x38014);
	SHOW("OVC_OGAMC3", 0x38018);
	SHOW("OVC_OGAMC2", 0x3801c);
	SHOW("OVC_OGAMC1", 0x38020);
	SHOW("OVC_OGAMC0", 0x38024);

	printf("\n비교용 - 아무것도 없을 주소\n");
	SHOW("0x3c000", 0x3c000);
	SHOW("0x3c010", 0x3c010);

	printf("\n스프라이트 평면 (평면 C)\n");
	SHOW("DSPCCNTR", 0x72180);
	SHOW("DSPCLINOFF", 0x72184);
	SHOW("DSPCSTRIDE", 0x72188);
	SHOW("DSPCPOS", 0x7218c);
	SHOW("DSPCSIZE", 0x72190);

	printf("\n디스플레이 쪽 (살아 있는 것으로 확인된 레지스터)\n");
	SHOW("PIPEBCONF", 0x71008);
	SHOW("DSPBSTRIDE", 0x71188);
	SHOW("CURBCNTR", 0x700c0);
	return 0;
}
