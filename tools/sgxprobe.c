/* SGX 2D 엔진이 살아 있는지 읽기만으로 확인한다.
 *
 * 이 칩의 그리기 엔진은 PowerVR SGX535 다. 지금까지 아무도 건드린 적이 없으므로
 * 전원이나 클럭이 꺼져 있을 수 있다. 쓰기는 버스를 멈출 위험이 있으니, 먼저
 * 레지스터가 그럴듯한 값을 돌려주는지만 본다.
 *
 * 레지스터 위치는 리눅스 staging 시절의 gma500 (psb_reg.h, psb_drv.h) 에서
 * 가져왔다. 이 코드는 커널에 올라간 적이 없는 psb_2d.c 가 쓰던 것과 같다.
 */
#include <Drivers.h>
#include <PCI.h>
#include <OS.h>
#include <poke.h>

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define PSB_SGX_OFFSET			0x40000
#define PSB_SGX_2D_SLAVE_PORT	0x4000
#define PSB_CR_BIF_CTRL			0x0c00
#define PSB_CR_BIF_TWOD_REQ_BASE 0x0c88
#define PSB_CR_2D_BLIT_STATUS	0x0e04
#define PSB_CR_2D_SOCIF			0x0e18

int
main(void)
{
	pci_info info;
	pci_info_args args;
	mem_map_args mmio;
	volatile uint8* sgx;
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
	mmio.name = "sgx probe";
	mmio.physical_address = info.u.h0.base_registers[0];
	mmio.size = info.u.h0.base_register_sizes[0];
	mmio.flags = B_ANY_ADDRESS;
	mmio.protection = B_READ_AREA | B_WRITE_AREA;
	if (ioctl(fd, POKE_MAP_MEMORY, &mmio, sizeof(mmio)) < 0)
		return 1;
	sgx = (volatile uint8*)mmio.address + PSB_SGX_OFFSET;

#define SHOW(name, off) printf("  %-22s %04x  %08lx\n", name, off, \
	(unsigned long)*(volatile uint32*)(sgx + (off)))

	printf("GTT 창 (BAR2)            %08lx\n",
		(unsigned long)info.u.h0.base_registers[2]);
	printf("SGX 레지스터 (BAR0+40000)\n");
	SHOW("CR_2D_SOCIF", PSB_CR_2D_SOCIF);
	SHOW("CR_2D_BLIT_STATUS", PSB_CR_2D_BLIT_STATUS);
	SHOW("CR_BIF_CTRL", PSB_CR_BIF_CTRL);
	SHOW("CR_BIF_TWOD_REQ_BASE", PSB_CR_BIF_TWOD_REQ_BASE);
	/* SGX 코어가 응답하는지 보는 용도로 몇 개 더 */
	SHOW("CR_CORE_ID (0x010)", 0x010);
	SHOW("CR_CORE_REVISION", 0x014);
	SHOW("CR_SOFT_RESET", 0x080);
	SHOW("CR_CLKGATECTL", 0x000);
	return 0;
}
