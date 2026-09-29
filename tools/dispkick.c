/* 언더런으로 멈춘 화면을 되살린다. 두 단계가 있다.
 *
 *   plane  평면을 껐다가 다시 켠다. 모드나 파이프는 건드리지 않는다.
 *   pipe   그것으로 안 되면 파이프까지 껐다 켠다. 타이밍 값은 그대로 둔다.
 *
 * 정상과 고장 상태의 레지스터 차이는 PIPEBSTAT 비트 31 하나뿐이었다.
 * i915 에서 그 비트는 FIFO 언더런 상태이고 1 을 써서 지운다.
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

#define PIPEBCONF	0x71008
#define PIPEBSTAT	0x71024
#define DSPBCNTR	0x71180
#define DSPBLINOFF	0x71184
#define DSPBSURF	0x7119c
#define DSPARB		0x70030
#define PIPE_ENABLE	(1u << 31)
#define PLANE_ENABLE (1u << 31)

static volatile uint8* sRegs;
static uint32 rd(uint32 o) { return *(volatile uint32*)(sRegs + o); }
static void wr(uint32 o, uint32 v) { *(volatile uint32*)(sRegs + o) = v; }


static void
show(const char* when)
{
	printf("%s PIPEBCONF %08lx  PIPEBSTAT %08lx  DSPBCNTR %08lx  "
		"SURF %08lx  DSPARB %08lx\n", when,
		(unsigned long)rd(PIPEBCONF), (unsigned long)rd(PIPEBSTAT),
		(unsigned long)rd(DSPBCNTR), (unsigned long)rd(DSPBSURF),
		(unsigned long)rd(DSPARB));
}


int
main(int argc, char** argv)
{
	pci_info info;
	pci_info_args args;
	mem_map_args mmio;
	uint32 cntr, surf, linoff;
	int hard = (argc > 1 && strcmp(argv[1], "pipe") == 0);
	int holdMs = 60;
	int index;
	int fd;

	if (argc > 2)
		holdMs = atoi(argv[2]);

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
	mmio.name = "dispkick";
	mmio.physical_address = info.u.h0.base_registers[0];
	mmio.size = info.u.h0.base_register_sizes[0];
	mmio.flags = B_ANY_ADDRESS;
	mmio.protection = B_READ_AREA | B_WRITE_AREA;
	if (ioctl(fd, POKE_MAP_MEMORY, &mmio, sizeof(mmio)) < 0)
		return 1;
	sRegs = (volatile uint8*)mmio.address;

	printf("오버레이 OVADD %08lx  DOVSTA %08lx\n",
		(unsigned long)rd(0x30000), (unsigned long)rd(0x30008));
	show("전 ");

	cntr = rd(DSPBCNTR);
	linoff = rd(DSPBLINOFF);
	surf = rd(DSPBSURF);

	/* 평면 끄기.
	   3 세대는 DSPxADDR(=LINOFF, 0x71184) 이 래치 레지스터이고 4 세대부터
	   DSPxSURF(0x7119c) 다. 어느 쪽인지 모르니 둘 다 쓴다. */
	wr(DSPBCNTR, cntr & ~PLANE_ENABLE);
	(void)rd(DSPBCNTR);
	printf("  CNTR 을 %08lx 로 씀 -> 되읽으니 %08lx\n",
		(unsigned long)(cntr & ~PLANE_ENABLE), (unsigned long)rd(DSPBCNTR));
	wr(DSPBLINOFF, linoff);
	wr(DSPBSURF, surf);
	(void)rd(DSPBSURF);
	printf("  래치 뒤 CNTR %08lx\n", (unsigned long)rd(DSPBCNTR));
	printf("  평면을 %d ms 동안 꺼 둔다 - 화면이 꺼져야 한다\n", holdMs);
	fflush(stdout);
	snooze((bigtime_t)holdMs * 1000);

	/* 언더런 상태 비트 지우기 */
	wr(PIPEBSTAT, rd(PIPEBSTAT));
	(void)rd(PIPEBSTAT);
	snooze(20000);

	if (hard) {
		printf("  파이프까지 껐다 켠다\n");
		wr(PIPEBCONF, rd(PIPEBCONF) & ~PIPE_ENABLE);
		(void)rd(PIPEBCONF);
		snooze(100000);
		wr(PIPEBSTAT, rd(PIPEBSTAT));
		wr(PIPEBCONF, rd(PIPEBCONF) | PIPE_ENABLE);
		(void)rd(PIPEBCONF);
		snooze(100000);
	}

	/* 평면 켜기 */
	wr(DSPBCNTR, cntr | PLANE_ENABLE);
	wr(DSPBLINOFF, linoff);
	wr(DSPBSURF, surf);
	(void)rd(DSPBSURF);
	snooze(100000);

	/* 다시 한 번 지운다 - 껐다 켜는 동안 또 섰을 수 있다 */
	wr(PIPEBSTAT, rd(PIPEBSTAT));
	snooze(50000);

	show("후 ");
	return 0;
}
