/* Load the MSVDX (VXD370) firmware and see whether it comes up.
 *
 * Follows psb_setup_fw() in Intel's psb kernel driver (psb_msvdxinit.c):
 * reset the MTX, clear the host/MTX communication area, upload code and data
 * into the MTX's RAM through the RAM access port, read both back, set the PC
 * and start the thread. A running firmware writes 0xA5A5A5A5 into the
 * communication area's signature word.
 *
 * Deliberately not done here: enabling the decoder's MMU and setting up the
 * RENDEC buffers. Neither is needed for the firmware to announce itself, and
 * both would let the core address memory. No decode command is sent. If the
 * signature does not appear, the MTX is stopped and reset again.
 *
 * The firmware itself is Intel's msvdx_fw.bin and is not part of this tree:
 *   msvdxfw /path/to/msvdx_fw.bin
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

#define PSB_MSVDX_OFFSET 0x50000

#define MTX_ENABLE				0x0000
#define MTX_RW_DATA				0x00f8
#define MTX_RW_REQUEST			0x00fc
#define MTX_RAM_DATA			0x0104
#define MTX_RAM_CONTROL			0x0108
#define MTX_RAM_STATUS			0x010c
#define MTX_SOFT_RESET			0x0200
#define MSVDX_INT_STATUS		0x0608
#define MSVDX_MAN_CLK_ENABLE	0x0620
#define MSVDX_MTX_RAM_BANK		0x06f0

#define COMMS					0x2cc0
#define COMMS_FW_STATUS			(COMMS - 0x10)
#define COMMS_MSG_COUNTER		(COMMS - 0x04)
#define COMMS_SIGNATURE			(COMMS + 0x00)
#define COMMS_TO_HOST_RD		(COMMS + 0x08)
#define COMMS_TO_HOST_WRT		(COMMS + 0x0c)
#define COMMS_TO_MTX_RD			(COMMS + 0x14)
#define COMMS_FLAGS				(COMMS + 0x18)
#define COMMS_TO_MTX_WRT		(COMMS + 0x1c)
#define SIGNATURE_VALUE			0xa5a5a5a5

#define MTX_CODE_BASE			0x80900000
#define MTX_DATA_BASE			0x82880000
#define PC_START_ADDRESS		0x80900000
#define MTX_CORE_CODE_MEM		0x10
#define MTX_CORE_DATA_MEM		0x18
#define MTX_PC					((0 << 4) | 5)

#define CLK_CORE				0x01
#define CLK_MTX					0x40

#define FLAGS_MMU_NONOPT_INV	0x002
#define FLAGS_MMU_HW_INVAL		0x020
#define FLAGS_BRN23154			0x200
#define POULSBO_D1				0x6

struct msvdx_fw {
	uint32 ver;
	uint32 text_size;
	uint32 data_size;
	uint32 data_location;
};

static volatile uint8* sVd;

static uint32 rd(uint32 off) { return *(volatile uint32*)(sVd + off); }
static void wr(uint32 off, uint32 v) { *(volatile uint32*)(sVd + off) = v; }

static int
wait_for(uint32 off, uint32 value, uint32 mask)
{
	int i;
	for (i = 0; i < 1000; i++) {
		if ((rd(off) & mask) == value)
			return 0;
		snooze(100);
	}
	printf("  timeout at %04lx: want %08lx mask %08lx, got %08lx\n",
		(unsigned long)off, (unsigned long)value, (unsigned long)mask,
		(unsigned long)rd(off));
	return 1;
}

static void
ram_access(uint32 ramId, uint32 address, int read)
{
	uint32 ctrl = (ramId << 20) | (((address >> 2) << 2) & 0x000ffffc)
		| (1 << 1) | (read ? 1 : 0);
	wr(MTX_RAM_CONTROL, ctrl);
}

static int
upload(uint32 mem, uint32 bankSize, uint32 address, uint32 words,
	const uint32* data, int verify)
{
	uint32 saved = rd(MTX_RAM_CONTROL);
	uint32 bank = ~0u;
	uint32 i;
	int bad = 0;

	wait_for(MTX_RAM_STATUS, 1, 0xffffffff);
	for (i = 0; i < words; i++) {
		uint32 ramId = mem + address / bankSize;
		if (ramId != bank) {
			ram_access(ramId, address, verify);
			bank = ramId;
		}
		address += 4;
		if (verify) {
			wait_for(MTX_RAM_STATUS, 1, 0xffffffff);
			if (rd(MTX_RAM_DATA) != data[i]) {
				printf("  verify mismatch at word %lu\n", (unsigned long)i);
				bad = 1;
				break;
			}
		} else {
			wr(MTX_RAM_DATA, data[i]);
			wait_for(MTX_RAM_STATUS, 1, 0xffffffff);
		}
	}
	wr(MTX_RAM_CONTROL, saved);
	return bad;
}

static void
write_core_reg(uint32 reg, uint32 value)
{
	wr(MTX_RW_DATA, value);
	wr(MTX_RW_REQUEST, reg);	/* RNW = 0 (write), DREADY = 0 */
	wait_for(MTX_RW_REQUEST, 0x80000000, 0x80000000);
}

static void
stop_mtx(void)
{
	wr(MTX_ENABLE, 0);
	wr(MTX_SOFT_RESET, 1);
	wr(MSVDX_MAN_CLK_ENABLE, CLK_CORE);
}

int
main(int argc, char** argv)
{
	pci_info info;
	pci_info_args args;
	mem_map_args mmio;
	struct msvdx_fw* fw;
	uint32* text;
	uint32* data;
	uint32 bankSize;
	size_t size;
	char* blob;
	FILE* f;
	int fd;
	int index;
	int i;

	if (argc != 2) {
		fprintf(stderr, "usage: %s msvdx_fw.bin\n", argv[0]);
		return 2;
	}
	f = fopen(argv[1], "rb");
	if (f == NULL)
		return 1;
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	blob = malloc(size);
	if (fread(blob, 1, size, f) != size)
		return 1;
	fclose(f);
	fw = (struct msvdx_fw*)blob;
	if (fw->ver != 2 || size != sizeof(*fw)
			+ 4 * (fw->text_size + fw->data_size)) {
		fprintf(stderr, "not a version 2 msvdx_fw.bin\n");
		return 1;
	}
	text = (uint32*)(blob + sizeof(*fw));
	data = text + fw->text_size;
	printf("firmware: text %lu words, data %lu words at %08lx\n",
		(unsigned long)fw->text_size, (unsigned long)fw->data_size,
		(unsigned long)fw->data_location);

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
	mmio.name = "msvdx fw";
	mmio.physical_address = info.u.h0.base_registers[0];
	mmio.size = info.u.h0.base_register_sizes[0];
	mmio.flags = B_ANY_ADDRESS;
	mmio.protection = B_READ_AREA | B_WRITE_AREA;
	if (ioctl(fd, POKE_MAP_MEMORY, &mmio, sizeof(mmio)) < 0)
		return 1;
	sVd = (volatile uint8*)mmio.address + PSB_MSVDX_OFFSET;
	printf("revision %02x, MTX_ENABLE %08lx before\n", info.revision,
		(unsigned long)rd(MTX_ENABLE));

	wr(MSVDX_MAN_CLK_ENABLE, CLK_CORE | CLK_MTX);
	wr(MTX_SOFT_RESET, 1);

	wr(COMMS_FLAGS, info.revision >= POULSBO_D1
		? FLAGS_MMU_HW_INVAL | FLAGS_BRN23154
		: FLAGS_MMU_NONOPT_INV | FLAGS_MMU_HW_INVAL | FLAGS_BRN23154);
	wr(COMMS_MSG_COUNTER, 0);
	wr(COMMS_SIGNATURE, 0);
	wr(COMMS_TO_HOST_RD, 0);
	wr(COMMS_TO_HOST_WRT, 0);
	wr(COMMS_TO_MTX_RD, 0);
	wr(COMMS_TO_MTX_WRT, 0);
	wr(COMMS_FW_STATUS, 0);

	bankSize = 1u << (((rd(MSVDX_MTX_RAM_BANK) & 0x000f0000) >> 16) + 2);
	printf("RAM bank size %lu bytes\n", (unsigned long)bankSize);

	upload(MTX_CORE_CODE_MEM, bankSize, PC_START_ADDRESS - MTX_CODE_BASE,
		fw->text_size, text, 0);
	upload(MTX_CORE_DATA_MEM, bankSize, fw->data_location - MTX_DATA_BASE,
		fw->data_size, data, 0);
	if (upload(MTX_CORE_CODE_MEM, bankSize, PC_START_ADDRESS - MTX_CODE_BASE,
			fw->text_size, text, 1)
		|| upload(MTX_CORE_DATA_MEM, bankSize,
			fw->data_location - MTX_DATA_BASE, fw->data_size, data, 1)) {
		printf("upload did not verify; stopping\n");
		stop_mtx();
		return 1;
	}
	printf("code and data uploaded and verified\n");

	write_core_reg(MTX_PC, PC_START_ADDRESS);
	wr(MTX_ENABLE, 1);

	for (i = 0; i < 2000 && rd(COMMS_SIGNATURE) != SIGNATURE_VALUE; i++)
		snooze(100);
	printf("signature %08lx after %d ms; FW_STATUS %08lx MTX_ENABLE %08lx "
		"INT_STATUS %08lx\n", (unsigned long)rd(COMMS_SIGNATURE), i / 10,
		(unsigned long)rd(COMMS_FW_STATUS), (unsigned long)rd(MTX_ENABLE),
		(unsigned long)rd(MSVDX_INT_STATUS));
	if (rd(COMMS_SIGNATURE) != SIGNATURE_VALUE) {
		printf("firmware did not come up; MTX stopped and reset\n");
		stop_mtx();
		return 1;
	}
	printf("MSVDX firmware is running\n");
	return 0;
}
