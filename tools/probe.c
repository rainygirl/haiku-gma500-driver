/* Poulsbo (GMA500) register probe.
 *
 * Runs in userland through /dev/misc/poke, so nothing has to be loaded into
 * the kernel before we know whether the display engine answers at all.
 * The display half of this chip is Intel's, with i915-shaped registers; the
 * drawing half is a PowerVR SGX and is not touched here.
 */
#include <Drivers.h>
#include <PCI.h>
#include <OS.h>
#include <poke.h>

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#define VENDOR_INTEL	0x8086
#define DEVICE_POULSBO	0x8108

static int sFD = -1;
static volatile uint8* sRegs = NULL;

static uint32
rd(uint32 offset)
{
	return *(volatile uint32*)(sRegs + offset);
}

static void
show(const char* name, uint32 offset)
{
	printf("  %-12s %06lx  %08lx\n", name, (unsigned long)offset,
		(unsigned long)rd(offset));
}

int
main()
{
	pci_info info;
	pci_info_args args;
	mem_map_args map;
	int i;
	int index;
	int found = 0;

	sFD = open(POKE_DEVICE_FULLNAME, O_RDWR);
	if (sFD < 0) {
		printf("poke 드라이버를 열 수 없다\n");
		return 1;
	}

	args.signature = POKE_SIGNATURE;
	args.info = &info;
	for (index = 0; index < 255; index++) {
		args.index = index;
		if (ioctl(sFD, POKE_GET_NTH_PCI_INFO, &args, sizeof(args)) != B_OK)
			break;
		if (args.status != B_OK)
			break;
		if (info.vendor_id == VENDOR_INTEL && info.device_id == DEVICE_POULSBO) {
			found = 1;
			break;
		}
	}
	if (!found) {
		printf("Poulsbo 그래픽 장치를 찾지 못했다\n");
		return 1;
	}

	printf("PCI %02x:%02x.%x  8086:8108  rev %02x\n", info.bus, info.device,
		info.function, info.revision);
	for (i = 0; i < 6; i++) {
		if (info.u.h0.base_register_sizes[i] == 0)
			continue;
		printf("  BAR%d  %08lx  size %8lx  %s\n", i,
			(unsigned long)info.u.h0.base_registers[i],
			(unsigned long)info.u.h0.base_register_sizes[i],
			(info.u.h0.base_register_flags[i] & PCI_address_space) ? "io"
				: "mem");
	}

	memset(&map, 0, sizeof(map));
	map.signature = POKE_SIGNATURE;
	map.name = "gma500 mmio";
	map.physical_address = info.u.h0.base_registers[0];
	map.size = info.u.h0.base_register_sizes[0];
	// 이 ioctl 은 B_OK 가 아니라 area_id 를 돌려준다. 0 이상이면 성공이다.
	map.flags = B_ANY_ADDRESS;
	map.protection = B_READ_AREA | B_WRITE_AREA;
	if (ioctl(sFD, POKE_MAP_MEMORY, &map, sizeof(map)) < 0) {
		printf("MMIO 매핑 실패: %s\n", strerror(errno));
		return 1;
	}
	sRegs = (volatile uint8*)map.address;
	printf("MMIO %08lx -> %p (%lu KB)\n",
		(unsigned long)info.u.h0.base_registers[0], map.address,
		(unsigned long)(map.size / 1024));

	printf("\n파이프 A 타이밍\n");
	show("HTOTAL", 0x60000);
	show("HBLANK", 0x60004);
	show("HSYNC", 0x60008);
	show("VTOTAL", 0x6000c);
	show("VBLANK", 0x60010);
	show("VSYNC", 0x60014);
	show("PIPEASRC", 0x6001c);
	show("PIPEACONF", 0x70008);
	show("PIPEASTAT", 0x70024);

	printf("\n디스플레이 평면 A\n");
	show("DSPACNTR", 0x70180);
	show("DSPALINOFF", 0x70184);
	show("DSPASTRIDE", 0x70188);
	show("DSPASURF", 0x7019c);
	show("DSPATILEOFF", 0x701a4);

	printf("\n하드웨어 커서 A\n");
	show("CURACNTR", 0x70080);
	show("CURABASE", 0x70084);
	show("CURAPOS", 0x70088);

	printf("\n파이프 B (LVDS 패널이 붙는 쪽)\n");
	show("HTOTAL_B", 0x61000);
	show("HBLANK_B", 0x61004);
	show("HSYNC_B", 0x61008);
	show("VTOTAL_B", 0x6100c);
	show("VBLANK_B", 0x61010);
	show("VSYNC_B", 0x61014);
	show("PIPEBSRC", 0x6101c);
	show("PIPEBCONF", 0x71008);
	show("PIPEBSTAT", 0x71024);
	show("DSPBCNTR", 0x71180);
	show("DSPBLINOFF", 0x71184);
	show("DSPBSTRIDE", 0x71188);
	show("DSPBSURF", 0x7119c);
	show("CURBCNTR", 0x700c0);
	show("CURBBASE", 0x700c4);
	show("CURBPOS", 0x700c8);

	printf("\n패널\n");
	show("PP_CONTROL", 0x61204);
	show("PP_STATUS", 0x61200);
	show("BLC_PWM_CTL", 0x61254);
	show("LVDS", 0x61180);
	show("VGACNTRL", 0x71400);

	{
		/* GTT: BAR3 는 페이지 테이블 자체다. 프레임버퍼가 어느 칸에
		   들어 있는지, 그 뒤로 빈 칸이 있는지 본다. */
		mem_map_args gtt;
		volatile uint32* pte;
		uint32 fbOffset = rd(0x7119c);
		uint32 first = fbOffset / 4096;
		uint32 count = info.u.h0.base_register_sizes[3] / 4;
		uint32 i2;
		uint32 used = 0;
		uint32 firstFree = 0;

		memset(&gtt, 0, sizeof(gtt));
		gtt.signature = POKE_SIGNATURE;
		gtt.name = "gma500 gtt";
		gtt.physical_address = info.u.h0.base_registers[3];
		gtt.size = info.u.h0.base_register_sizes[3];
		gtt.flags = B_ANY_ADDRESS;
		gtt.protection = B_READ_AREA | B_WRITE_AREA;
		if (ioctl(sFD, POKE_MAP_MEMORY, &gtt, sizeof(gtt)) >= 0) {
			pte = (volatile uint32*)gtt.address;
			for (i2 = 0; i2 < count; i2++) {
				if (pte[i2] & 1) {
					used++;
					if (firstFree != 0 && used > 0 && i2 == firstFree)
						firstFree = 0;
				} else if (firstFree == 0 && i2 > first)
					firstFree = i2;
			}
			printf("\nGTT (%lu 칸 = %lu MB 창)\n", (unsigned long)count,
				(unsigned long)(count * 4096 / 1024 / 1024));
			printf("  프레임버퍼 GTT offset %08lx -> 칸 %lu\n",
				(unsigned long)fbOffset, (unsigned long)first);
			printf("  칸 %lu..%lu PTE: %08lx %08lx %08lx %08lx\n",
				(unsigned long)first, (unsigned long)first + 3,
				(unsigned long)pte[first], (unsigned long)pte[first + 1],
				(unsigned long)pte[first + 2], (unsigned long)pte[first + 3]);
			printf("  유효한 칸 %lu개, 프레임버퍼 뒤 첫 빈 칸 %lu (GTT offset %08lx)\n",
				(unsigned long)used, (unsigned long)firstFree,
				(unsigned long)(firstFree * 4096));
			ioctl(sFD, POKE_UNMAP_MEMORY, &gtt, sizeof(gtt));
		} else
			printf("\nGTT 매핑 실패\n");
	}

	ioctl(sFD, POKE_UNMAP_MEMORY, &map, sizeof(map));
	close(sFD);
	return 0;
}
