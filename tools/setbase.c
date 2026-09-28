/* CR_BIF_TWOD_REQ_BASE 를 지정한 값으로 되돌린다.
 *
 * fill2d/blit2d 같은 시험 도구는 자기 명령 버퍼의 물리 주소를 이 레지스터에
 * 써넣는다. 도구가 끝난 뒤에도 값이 남아 있으면 accelerant 가 보내는 2D
 * 명령이 엉뚱한 메모리를 읽으므로, 프레임버퍼 물리 주소로 돌려놓는다.
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

#define PSB_SGX_OFFSET				0x40000
#define PSB_CR_BIF_TWOD_REQ_BASE	0x0c88

int
main(int argc, char** argv)
{
	pci_info info;
	pci_info_args args;
	mem_map_args mmio;
	volatile uint32* reg;
	uint32 value;
	int index;
	int fd;

	if (argc != 2) {
		fprintf(stderr, "사용법: setbase <물리주소 16진수, 예 7f800000>\n");
		return 1;
	}
	value = (uint32)strtoul(argv[1], NULL, 16);

	fd = open(POKE_DEVICE_FULLNAME, O_RDWR);
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
	mmio.name = "sgx setbase";
	mmio.physical_address = info.u.h0.base_registers[0];
	mmio.size = info.u.h0.base_register_sizes[0];
	mmio.flags = B_ANY_ADDRESS;
	mmio.protection = B_READ_AREA | B_WRITE_AREA;
	if (ioctl(fd, POKE_MAP_MEMORY, &mmio, sizeof(mmio)) < 0)
		return 1;
	reg = (volatile uint32*)((volatile uint8*)mmio.address + PSB_SGX_OFFSET
		+ PSB_CR_BIF_TWOD_REQ_BASE);

	printf("이전 %08lx -> ", (unsigned long)*reg);
	*reg = value;
	printf("%08lx\n", (unsigned long)*reg);
	return 0;
}
