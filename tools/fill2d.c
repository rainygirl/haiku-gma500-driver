/* SGX 2D 엔진으로 사각형 하나를 채워 본다.
 *
 * 명령 형식은 리눅스 staging 시절의 psb_2d.c (psb_accel_2d_fillrect) 와 같다.
 * 그 코드는 커널에서 제거됐지만 git 역사에 남아 있다.
 *
 * 결과는 프레임버퍼 메모리에 남으므로 screenshot 으로 확인할 수 있다 -
 * 하드웨어 커서와 달리 눈으로 볼 필요가 없다.
 *
 * 모든 대기는 횟수 제한을 둔다. 엔진이 응답하지 않으면 그냥 포기한다.
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
#define PSB_CR_BIF_CTRL				0x0c00
#define PSB_CR_BIF_TWOD_REQ_BASE	0x0c88
#define PSB_CR_BIF_INT_STAT			0x0c04
#define PSB_CR_BIF_FAULT			0x0c08
#define PSB_CR_BIF_DIR_LIST_BASE0	0x0c84
#define PSB_CR_BIF_DIR_LIST_BASE1	0x0c38
#define PSB_CR_2D_BLIT_STATUS		0x0e04
#define PSB_CR_2D_SOCIF				0x0e18

#define PSB_2D_FENCE_BH				0x70000000
#define PSB_2D_BLIT_BH				0x80000000
#define PSB_2D_DST_SURF_BH			0xa0000000
#define PSB_2D_FLUSH_BH				0xf0000000
#define PSB_2D_DST_8888ARGB			0x00060000
#define PSB_2D_ROP3_PATCOPY			0x0000f0f0
#define PSB_2D_DST_XSTART_SHIFT		12
#define PSB_2D_DST_XSIZE_SHIFT		12

#define DSPBSTRIDE	0x71188

static volatile uint8* sSGX;
static volatile uint8* sRegs;

static uint32 rd(uint32 o) { return *(volatile uint32*)(sSGX + o); }
static void wr(uint32 o, uint32 v) { *(volatile uint32*)(sSGX + o) = v; }


int
main(int argc, char** argv)
{
	pci_info info;
	pci_info_args args;
	mem_map_args mmio;
	uint32 cmd[8];
	uint32 stride, gatt;
	int index, i, spin;
	int x = argc > 1 ? atoi(argv[1]) : 200;
	int y = argc > 2 ? atoi(argv[2]) : 120;
	int w = argc > 3 ? atoi(argv[3]) : 320;
	int h = argc > 4 ? atoi(argv[4]) : 200;
	uint32 colour = argc > 5 ? strtoul(argv[5], NULL, 16) : 0x00ff00ff;
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
	mmio.name = "sgx 2d";
	mmio.physical_address = info.u.h0.base_registers[0];
	mmio.size = info.u.h0.base_register_sizes[0];
	mmio.flags = B_ANY_ADDRESS;
	mmio.protection = B_READ_AREA | B_WRITE_AREA;
	if (ioctl(fd, POKE_MAP_MEMORY, &mmio, sizeof(mmio)) < 0)
		return 1;
	sRegs = (volatile uint8*)mmio.address;
	sSGX = sRegs + PSB_SGX_OFFSET;

	gatt = info.u.h0.base_registers[2];
	stride = *(volatile uint32*)(sRegs + DSPBSTRIDE) & 0xffff;

	printf("GTT 창 %08lx, stride %lu\n", (unsigned long)gatt,
		(unsigned long)stride);
	printf("전 BIF: INT_STAT %08lx  FAULT %08lx  DIR0 %08lx  DIR1 %08lx\n",
		(unsigned long)rd(PSB_CR_BIF_INT_STAT),
		(unsigned long)rd(PSB_CR_BIF_FAULT),
		(unsigned long)rd(PSB_CR_BIF_DIR_LIST_BASE0),
		(unsigned long)rd(PSB_CR_BIF_DIR_LIST_BASE1));
	printf("전: SOCIF %08lx  BLIT_STATUS %08lx  TWOD_REQ_BASE %08lx\n",
		(unsigned long)rd(PSB_CR_2D_SOCIF),
		(unsigned long)rd(PSB_CR_2D_BLIT_STATUS),
		(unsigned long)rd(PSB_CR_BIF_TWOD_REQ_BASE));

	/* 2D 엔진의 주소 기준을 GTT 창 시작으로 잡는다. 그러면 명령의
	   dst_offset 이 GTT 오프셋이 되고, 프레임버퍼는 0번 칸이라 0 이다. */
	wr(PSB_CR_BIF_TWOD_REQ_BASE, gatt);
	(void)rd(PSB_CR_BIF_TWOD_REQ_BASE);
	printf("TWOD_REQ_BASE 설정 -> %08lx\n",
		(unsigned long)rd(PSB_CR_BIF_TWOD_REQ_BASE));

	cmd[0] = PSB_2D_FENCE_BH;
	cmd[1] = PSB_2D_DST_SURF_BH | PSB_2D_DST_8888ARGB | stride;
	cmd[2] = 0;								/* 프레임버퍼 = GTT 오프셋 0 */
	cmd[3] = PSB_2D_BLIT_BH | PSB_2D_ROP3_PATCOPY;
	cmd[4] = colour;
	cmd[5] = ((uint32)x << PSB_2D_DST_XSTART_SHIFT) | (uint32)y;
	cmd[6] = ((uint32)w << PSB_2D_DST_XSIZE_SHIFT) | (uint32)h;
	cmd[7] = PSB_2D_FLUSH_BH;

	for (spin = 0; spin < 100000; spin++) {
		if (rd(PSB_CR_2D_SOCIF) >= 8)
			break;
		snooze(100);
	}
	if (rd(PSB_CR_2D_SOCIF) < 8) {
		printf("FIFO 가 비지 않는다 - 포기\n");
		return 1;
	}

	for (i = 0; i < 8; i++)
		wr(PSB_SGX_2D_SLAVE_PORT + i * 4, cmd[i]);
	(void)rd(PSB_SGX_2D_SLAVE_PORT + 7 * 4);

	for (spin = 0; spin < 100000; spin++) {
		if (rd(PSB_CR_2D_BLIT_STATUS) == 0)
			break;
		snooze(100);
	}
	printf("BIF: INT_STAT %08lx  FAULT %08lx  DIR0 %08lx  DIR1 %08lx\n",
		(unsigned long)rd(PSB_CR_BIF_INT_STAT),
		(unsigned long)rd(PSB_CR_BIF_FAULT),
		(unsigned long)rd(PSB_CR_BIF_DIR_LIST_BASE0),
		(unsigned long)rd(PSB_CR_BIF_DIR_LIST_BASE1));
	printf("후: SOCIF %08lx  BLIT_STATUS %08lx  BIF_CTRL %08lx\n",
		(unsigned long)rd(PSB_CR_2D_SOCIF),
		(unsigned long)rd(PSB_CR_2D_BLIT_STATUS),
		(unsigned long)rd(PSB_CR_BIF_CTRL));
	printf("(%d,%d) %dx%d 를 %08lx 로 채우라고 보냈다\n", x, y, w, h,
		(unsigned long)colour);
	return 0;
}
