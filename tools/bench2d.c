/* CPU 복사와 2D 엔진 복사의 속도를 나란히 잰다.
 *
 * app_server 의 DrawingEngine::CopyRect 는 프레임버퍼를 CPU 로 한 줄씩
 * memcpy 한다. 프레임버퍼는 write-combining 이라 읽기가 캐시를 타지 못하므로
 * 창을 끄는 동안 이 복사가 얼마나 비싼지 실제로 재 본다.
 *
 * 화면을 건드리므로 시험 구역을 미리 저장해 두었다가 끝나면 되돌린다.
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
#define PSB_C2B_STATUS_BUSY			(1 << 24)

#define PSB_2D_FENCE_BH				0x70000000
#define PSB_2D_BLIT_BH				0x80000000
#define PSB_2D_SRC_SURF_BH			0x90000000
#define PSB_2D_DST_SURF_BH			0xa0000000
#define PSB_2D_SRC_OFF_BH			0x30000000
#define PSB_2D_FLUSH_BH				0xf0000000
#define PSB_2D_DST_8888ARGB			0x00060000
#define PSB_2D_SRC_8888ARGB			0x00060000
#define PSB_2D_USE_PAT				0x00010000
#define PSB_2D_ROP3_SRCCOPY			0x0000cccc

#define WIDTH	800
#define HEIGHT	500
#define ROUNDS	20

static volatile uint8* sSGX;
static uint32 rd(uint32 o) { return *(volatile uint32*)(sSGX + o); }
static void wr(uint32 o, uint32 v) { *(volatile uint32*)(sSGX + o) = v; }


static void
wait_idle(void)
{
	int spin;
	for (spin = 0; spin < 5000000; spin++) {
		if ((rd(PSB_CR_2D_BLIT_STATUS) & PSB_C2B_STATUS_BUSY) == 0)
			return;
	}
}


static void
hw_blit(uint32 stride, int sx, int sy, int dx, int dy, int w, int h)
{
	uint32 cmd[10];
	int i, spin;

	cmd[0] = PSB_2D_FENCE_BH;
	cmd[1] = PSB_2D_DST_SURF_BH | PSB_2D_DST_8888ARGB | stride;
	cmd[2] = 0;
	cmd[3] = PSB_2D_SRC_SURF_BH | PSB_2D_SRC_8888ARGB | stride;
	cmd[4] = 0;
	cmd[5] = PSB_2D_SRC_OFF_BH | ((uint32)sx << 12) | (uint32)sy;
	cmd[6] = PSB_2D_BLIT_BH | PSB_2D_USE_PAT | PSB_2D_ROP3_SRCCOPY;
	cmd[7] = ((uint32)dx << 12) | (uint32)dy;
	cmd[8] = ((uint32)w << 12) | (uint32)h;
	cmd[9] = PSB_2D_FLUSH_BH;

	for (spin = 0; spin < 100000 && rd(PSB_CR_2D_SOCIF) < 10; spin++)
		;
	for (i = 0; i < 10; i++)
		wr(PSB_SGX_2D_SLAVE_PORT + i * 4, cmd[i]);
	(void)rd(PSB_SGX_2D_SLAVE_PORT + 36);
}


int
main(void)
{
	pci_info info;
	pci_info_args args;
	mem_map_args mmio, fb, gtt;
	volatile uint8* regs;
	uint8* pixels;
	uint8* saved;
	uint32 stride, height, stolen;
	bigtime_t start, cpuTime, hwTime;
	int index, round, row;
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

	wr(PSB_CR_BIF_TWOD_REQ_BASE, stolen);
	(void)rd(PSB_CR_BIF_TWOD_REQ_BASE);

	printf("%lux%lu, stride %lu, %dx%d 구역을 %d 번 복사한다\n",
		(unsigned long)(stride / 4), (unsigned long)height,
		(unsigned long)stride, WIDTH, HEIGHT, ROUNDS);

	/* 시험 구역 저장 (원래 화면으로 되돌리기 위해) */
	saved = (uint8*)malloc(stride * (HEIGHT + 2));
	if (saved == NULL)
		return 1;
	memcpy(saved, pixels, stride * (HEIGHT + 2));

	/* CPU 복사: app_server 의 _CopyRect 와 같은 모양, 한 줄씩 아래로 1 픽셀 */
	start = system_time();
	for (round = 0; round < ROUNDS; round++) {
		for (row = HEIGHT - 1; row >= 0; row--) {
			memcpy(pixels + (row + 1) * stride, pixels + row * stride,
				WIDTH * 4);
		}
	}
	cpuTime = system_time() - start;

	/* 되돌린 뒤 하드웨어 복사 */
	memcpy(pixels, saved, stride * (HEIGHT + 2));

	start = system_time();
	for (round = 0; round < ROUNDS; round++) {
		hw_blit(stride, 0, 0, 0, 1, WIDTH, HEIGHT);
		wait_idle();
	}
	hwTime = system_time() - start;

	memcpy(pixels, saved, stride * (HEIGHT + 2));
	free(saved);

	printf("CPU  복사 %8lld us  (한 번 %lld us, 초당 %lld 회)\n",
		cpuTime, cpuTime / ROUNDS, 1000000LL / (cpuTime / ROUNDS));
	printf("2D   복사 %8lld us  (한 번 %lld us, 초당 %lld 회)\n",
		hwTime, hwTime / ROUNDS, 1000000LL / (hwTime / ROUNDS));
	printf("배수 %.1f\n", (double)cpuTime / (double)hwTime);
	return 0;
}
