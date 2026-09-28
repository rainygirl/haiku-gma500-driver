/* 2D 엔진의 화면 간 복사를 시험한다.
 *
 * 먼저 색이 다른 사각형 두 개를 채우고, 하나를 다른 자리로 복사한 뒤
 * 프레임버퍼 메모리를 직접 읽어 확인한다. 겹치는 복사(아래로 조금 밀기)도
 * 시험한다 - 창을 끌 때 실제로 일어나는 일이 그것이다.
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
#define PSB_2D_SRC_SURF_BH			0x90000000
#define PSB_2D_DST_SURF_BH			0xa0000000
#define PSB_2D_SRC_OFF_BH			0x30000000
#define PSB_2D_FLUSH_BH				0xf0000000
#define PSB_2D_DST_8888ARGB			0x00060000
#define PSB_2D_SRC_8888ARGB			0x00060000
#define PSB_2D_USE_PAT				0x00010000
#define PSB_2D_ROP3_PATCOPY			0x0000f0f0
#define PSB_2D_ROP3_SRCCOPY			0x0000cccc
#define PSB_2D_COPYORDER_TL2BR		(0 << 23)
#define PSB_2D_COPYORDER_BR2TL		(1 << 23)
#define PSB_2D_COPYORDER_TR2BL		(2 << 23)
#define PSB_2D_COPYORDER_BL2TR		(3 << 23)

static volatile uint8* sSGX;
static uint32 rd(uint32 o) { return *(volatile uint32*)(sSGX + o); }
static void wr(uint32 o, uint32 v) { *(volatile uint32*)(sSGX + o) = v; }


static void
submit(uint32* cmd, int count)
{
	int i, spin;
	for (spin = 0; spin < 10000 && rd(PSB_CR_2D_SOCIF) < (uint32)count; spin++)
		snooze(100);
	for (i = 0; i < count; i++)
		wr(PSB_SGX_2D_SLAVE_PORT + i * 4, cmd[i]);
	(void)rd(PSB_SGX_2D_SLAVE_PORT + (count - 1) * 4);
	snooze(30000);
}


static void
fill(uint32 stride, int x, int y, int w, int h, uint32 colour)
{
	uint32 cmd[8];
	cmd[0] = PSB_2D_FENCE_BH;
	cmd[1] = PSB_2D_DST_SURF_BH | PSB_2D_DST_8888ARGB | stride;
	cmd[2] = 0;
	cmd[3] = PSB_2D_BLIT_BH | PSB_2D_ROP3_PATCOPY;
	cmd[4] = colour;
	cmd[5] = ((uint32)x << 12) | (uint32)y;
	cmd[6] = ((uint32)w << 12) | (uint32)h;
	cmd[7] = PSB_2D_FLUSH_BH;
	submit(cmd, 8);
}


static void
blit(uint32 stride, int sx, int sy, int dx, int dy, int w, int h)
{
	uint32 cmd[10];
	uint32 direction;
	int xdir = sx - dx;
	int ydir = sy - dy;

	if (xdir < 0)
		direction = (ydir < 0) ? PSB_2D_COPYORDER_BR2TL : PSB_2D_COPYORDER_TR2BL;
	else
		direction = (ydir < 0) ? PSB_2D_COPYORDER_BL2TR : PSB_2D_COPYORDER_TL2BR;

	if (direction == PSB_2D_COPYORDER_BR2TL
		|| direction == PSB_2D_COPYORDER_TR2BL) {
		sx += w - 1;
		dx += w - 1;
	}
	if (direction == PSB_2D_COPYORDER_BR2TL
		|| direction == PSB_2D_COPYORDER_BL2TR) {
		sy += h - 1;
		dy += h - 1;
	}

	cmd[0] = PSB_2D_FENCE_BH;
	cmd[1] = PSB_2D_DST_SURF_BH | PSB_2D_DST_8888ARGB | stride;
	cmd[2] = 0;
	cmd[3] = PSB_2D_SRC_SURF_BH | PSB_2D_SRC_8888ARGB | stride;
	cmd[4] = 0;
	cmd[5] = PSB_2D_SRC_OFF_BH | ((uint32)sx << 12) | (uint32)sy;
	cmd[6] = PSB_2D_BLIT_BH | PSB_2D_USE_PAT | PSB_2D_ROP3_SRCCOPY | direction;
	cmd[7] = ((uint32)dx << 12) | (uint32)dy;
	cmd[8] = ((uint32)w << 12) | (uint32)h;
	cmd[9] = PSB_2D_FLUSH_BH;
	submit(cmd, 10);
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

#define PIXEL(px, py) (*(uint32*)(pixels + (py) * stride + (px) * 4))

	/* 2D 엔진의 주소 기준을 프레임버퍼 물리 주소로 */
	wr(PSB_CR_BIF_TWOD_REQ_BASE, stolen);
	(void)rd(PSB_CR_BIF_TWOD_REQ_BASE);

	printf("stride %lu, 스톨른 %08lx\n", (unsigned long)stride,
		(unsigned long)stolen);

	printf("\n[1] 채우기 두 개\n");
	fill(stride, 100, 400, 200, 150, 0x00ff0000);	/* 빨강 */
	fill(stride, 400, 400, 200, 150, 0x000000ff);	/* 파랑 */
	printf("  (150,450) = %08lx (빨강 기대)\n", (unsigned long)PIXEL(150, 450));
	printf("  (450,450) = %08lx (파랑 기대)\n", (unsigned long)PIXEL(450, 450));

	printf("\n[2] 겹치지 않는 복사: 빨강 -> (700,400)\n");
	blit(stride, 100, 400, 700, 400, 200, 150);
	printf("  (750,450) = %08lx (빨강 기대)\n", (unsigned long)PIXEL(750, 450));

	printf("\n[3] 겹치는 복사: (400,400) 을 (430,430) 으로 (창 끌기와 같은 모양)\n");
	blit(stride, 400, 400, 430, 430, 200, 150);
	printf("  (480,480) = %08lx (파랑 기대)\n", (unsigned long)PIXEL(480, 480));
	printf("  (610,570) = %08lx (파랑 기대 - 겹침 영역 끝)\n",
		(unsigned long)PIXEL(610, 570));

	printf("\nBLIT_STATUS %08lx\n", (unsigned long)rd(PSB_CR_2D_BLIT_STATUS));
	return 0;
}
