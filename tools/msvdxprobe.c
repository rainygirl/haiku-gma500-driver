/* Is the GMA500's video decoder (Imagination VXD370, "MSVDX") alive?
 * Read-only: nothing is written to the device.
 *
 * Register positions come from Intel's psb kernel driver (psb_drv.h,
 * psb_msvdx.h): the MSVDX block sits at BAR0 + 0x50000, 32 KB.
 */
#include <Drivers.h>
#include <PCI.h>
#include <OS.h>
#include <poke.h>

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define PSB_MSVDX_OFFSET 0x50000

int
main(void)
{
	pci_info info;
	pci_info_args args;
	mem_map_args mmio;
	volatile uint8* vd;
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
	mmio.name = "msvdx probe";
	mmio.physical_address = info.u.h0.base_registers[0];
	mmio.size = info.u.h0.base_register_sizes[0];
	mmio.flags = B_ANY_ADDRESS;
	mmio.protection = B_READ_AREA;
	if (ioctl(fd, POKE_MAP_MEMORY, &mmio, sizeof(mmio)) < 0)
		return 1;
	vd = (volatile uint8*)mmio.address + PSB_MSVDX_OFFSET;

#define SHOW(name, off) printf("  %-28s %04x  %08lx\n", name, off, \
	(unsigned long)*(volatile uint32*)(vd + (off)))

	printf("BAR0 %08lx size %lx; MSVDX at +50000\n",
		(unsigned long)info.u.h0.base_registers[0],
		(unsigned long)info.u.h0.base_register_sizes[0]);
	SHOW("MTX_ENABLE", 0x0000);
	SHOW("MTX_KICKI", 0x0088);
	SHOW("MTX_RAM_ACCESS_STATUS", 0x010c);
	SHOW("MTX_SOFT_RESET", 0x0200);
	SHOW("MSVDX_CONTROL", 0x0600);
	SHOW("MSVDX_INTERRUPT_STATUS", 0x0608);
	SHOW("MSVDX_HOST_INTERRUPT_ENABLE", 0x0610);
	SHOW("MSVDX_MAN_CLK_ENABLE", 0x0620);
	SHOW("MSVDX_MMU_CONTROL0", 0x0680);
	SHOW("MSVDX_MTX_RAM_BANK", 0x06f0);
	SHOW("RENDEC_CONTROL0", 0x0868);
	SHOW("RENDEC_BUFFER_SIZE", 0x0870);
	/* A sweep of the core register page, to see which words answer. */
	printf("core page 0x0600-0x06ff:\n");
	for (index = 0x600; index < 0x700; index += 0x10)
		printf("  %04x: %08lx %08lx %08lx %08lx\n", index,
			(unsigned long)*(volatile uint32*)(vd + index),
			(unsigned long)*(volatile uint32*)(vd + index + 4),
			(unsigned long)*(volatile uint32*)(vd + index + 8),
			(unsigned long)*(volatile uint32*)(vd + index + 12));
	return 0;
}
