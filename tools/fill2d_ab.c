/* 2D 엔진의 주소 기준(TWOD_REQ_BASE)을 두 가지로 놓고 채우기를 시험한다.
 *
 *   A) GTT 창 시작 (0x80000000) + 오프셋 0   - 리눅스가 쓰는 방식
 *   B) 스톨른 물리 주소 (0x7f800000) + 오프셋 0 - 프레임버퍼를 직접 가리킨다
 *
 * 판정은 스크린샷이 아니라 프레임버퍼 메모리를 직접 읽어서 한다. 어느 쪽이든
 * 픽셀이 바뀌면 그게 답이다.
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
#define PSB_SGX_2D_SLAVE_PORT		0x4000
#define PSB_CR_BIF_TWOD_REQ_BASE	0x0c88
#define PSB_CR_2D_BLIT_STATUS		0x0e04
#define PSB_CR_2D_SOCIF				0x0e18

#define PSB_2D_FENCE_BH				0x70000000
#define PSB_2D_BLIT_BH				0x80000000
#define PSB_2D_DST_SURF_BH			0xa0000000
#define PSB_2D_FLUSH_BH				0xf0000000
#define PSB_2D_DST_8888ARGB			0x00060000
#define PSB_2D_ROP3_PATCOPY			0x0000f0f0

static volatile uint8* sSGX;
static uint32 rd(uint32 o) { return *(volatile uint32*)(sSGX + o); }
static void wr(uint32 o, uint32 v) { *(volatile uint32*)(sSGX + o) = v; }


static void
fill(uint32 base, uint32 offset, uint32 stride, int x, int y, int w, int h,
	uint32 colour)
{
	uint32 cmd[8];
	int i, spin;

	wr(PSB_CR_BIF_TWOD_REQ_BASE, base);
	(void)rd(PSB_CR_BIF_TWOD_REQ_BASE);

	cmd[0] = PSB_2D_FENCE_BH;
	cmd[1] = PSB_2D_DST_SURF_BH | PSB_2D_DST_8888ARGB | stride;
	cmd[2] = offset;
	cmd[3] = PSB_2D_BLIT_BH | PSB_2D_ROP3_PATCOPY;
	cmd[4] = colour;
	cmd[5] = ((uint32)x << 12) | (uint32)y;
	cmd[6] = ((uint32)w << 12) | (uint32)h;
	cmd[7] = PSB_2D_FLUSH_BH;

	for (spin = 0; spin < 10000 && rd(PSB_CR_2D_SOCIF) < 8; spin++)
		snooze(100);
	for (i = 0; i < 8; i++)
		wr(PSB_SGX_2D_SLAVE_PORT + i * 4, cmd[i]);
	(void)rd(PSB_SGX_2D_SLAVE_PORT + 7 * 4);
	snooze(50000);
}


int
main(void)
{
	pci_info info;
	pci_info_args args;
	mem_map_args mmio, fb, gtt;
	volatile uint8* regs;
	uint8* pixels;
	uint32 stride, height, stolen;
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
	mmio.name = "sgx";
	mmio.physical_address = info.u.h0.base_registers[0];
	mmio.size = info.u.h0.base_register_sizes[0];
	mmio.flags = B_ANY_ADDRESS;
	mmio.protection = B_READ_AREA | B_WRITE_AREA;
	if (ioctl(fd, POKE_MAP_MEMORY, &mmio, sizeof(mmio)) < 0)
		return 1;
	regs = (volatile uint8*)mmio.address;
	sSGX = regs + PSB_SGX_OFFSET;
	stride = *(volatile uint32*)(regs + 0x71188) & 0xffff;
	height = (*(volatile uint32*)(regs + 0x6101c) & 0xffff) + 1;

	memset(&gtt, 0, sizeof(gtt));
	gtt.signature = POKE_SIGNATURE;
	gtt.name = "gtt";
	gtt.physical_address = info.u.h0.base_registers[3];
	gtt.size = info.u.h0.base_register_sizes[3];
	gtt.flags = B_ANY_ADDRESS;
	gtt.protection = B_READ_AREA | B_WRITE_AREA;
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

#define PIXEL(px, py) (*(uint32*)(pixels + (py) * stride + (px) * 4))

	printf("스톨른 %08lx, stride %lu, BLIT_STATUS %08lx\n",
		(unsigned long)stolen, (unsigned long)stride,
		(unsigned long)rd(PSB_CR_2D_BLIT_STATUS));

	printf("\n[A] REQ_BASE = GTT 창 %08lx, 오프셋 0\n",
		(unsigned long)info.u.h0.base_registers[2]);
	printf("  전 (205,125) = %08lx\n", (unsigned long)PIXEL(205, 125));
	fill(info.u.h0.base_registers[2], 0, stride, 200, 120, 320, 200,
		0x00ff00ff);
	printf("  후 (205,125) = %08lx   BLIT_STATUS %08lx\n",
		(unsigned long)PIXEL(205, 125),
		(unsigned long)rd(PSB_CR_2D_BLIT_STATUS));

	printf("\n[B] REQ_BASE = 스톨른 %08lx, 오프셋 0\n", (unsigned long)stolen);
	printf("  전 (605,325) = %08lx\n", (unsigned long)PIXEL(605, 325));
	fill(stolen, 0, stride, 600, 320, 320, 200, 0x0000ff00);
	printf("  후 (605,325) = %08lx   BLIT_STATUS %08lx\n",
		(unsigned long)PIXEL(605, 325),
		(unsigned long)rd(PSB_CR_2D_BLIT_STATUS));

	return 0;
}
