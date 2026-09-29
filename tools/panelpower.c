/* LVDS 패널 전원을 껐다 켠다.
 *
 * 리눅스 gma500 의 psb_intel_lvds_set_power 순서를 따른다.
 *   끄기: PP_CONTROL 의 POWER_TARGET_ON 을 내리고 PP_STATUS 의 PP_ON 이
 *         내려가기를 기다린 뒤 LVDS 포트를 끈다.
 *   켜기: LVDS 포트를 켜고 POWER_TARGET_ON 을 올린 뒤 PP_ON 을 기다린다.
 *
 * DPMS 구현의 바탕이자, 언더런으로 죽은 출력을 되살리는 제일 강한 수단이다.
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

#define PP_STATUS		0x61200
#define PP_CONTROL		0x61204
#define POWER_TARGET_ON	(1u << 0)
#define PP_ON			(1u << 31)
#define PP_READY		(1u << 30)
#define LVDS			0x61180
#define LVDS_PORT_EN	(1u << 31)
#define BLC_PWM_CTL		0x61254
#define PIPEBSTAT		0x71024

static volatile uint8* sRegs;
static uint32 rd(uint32 o) { return *(volatile uint32*)(sRegs + o); }
static void wr(uint32 o, uint32 v) { *(volatile uint32*)(sRegs + o) = v; }


static void
wait_pp(int wantOn)
{
	int spin;
	for (spin = 0; spin < 500; spin++) {
		int on = (rd(PP_STATUS) & PP_ON) != 0;
		if (on == wantOn)
			return;
		snooze(10000);
	}
	printf("  (PP_ON 이 %d 이 되기를 기다렸으나 시간 초과, PP_STATUS %08lx)\n",
		wantOn, (unsigned long)rd(PP_STATUS));
}


int
main(int argc, char** argv)
{
	pci_info info;
	pci_info_args args;
	mem_map_args mmio;
	uint32 backlight;
	int holdMs = 2000;
	int index;
	int fd;

	if (argc > 1)
		holdMs = atoi(argv[1]);

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
	mmio.name = "panelpower";
	mmio.physical_address = info.u.h0.base_registers[0];
	mmio.size = info.u.h0.base_register_sizes[0];
	mmio.flags = B_ANY_ADDRESS;
	mmio.protection = B_READ_AREA | B_WRITE_AREA;
	if (ioctl(fd, POKE_MAP_MEMORY, &mmio, sizeof(mmio)) < 0)
		return 1;
	sRegs = (volatile uint8*)mmio.address;

	backlight = rd(BLC_PWM_CTL);
	printf("전  PP_CONTROL %08lx  PP_STATUS %08lx  LVDS %08lx  BLC %08lx\n",
		(unsigned long)rd(PP_CONTROL), (unsigned long)rd(PP_STATUS),
		(unsigned long)rd(LVDS), (unsigned long)backlight);
	printf("    PIPEBSTAT %08lx\n", (unsigned long)rd(PIPEBSTAT));

	/* 끄기 */
	wr(PP_CONTROL, rd(PP_CONTROL) & ~POWER_TARGET_ON);
	(void)rd(PP_CONTROL);
	wait_pp(0);
	wr(LVDS, rd(LVDS) & ~LVDS_PORT_EN);
	(void)rd(LVDS);
	printf("껐다. PP_STATUS %08lx  LVDS %08lx\n",
		(unsigned long)rd(PP_STATUS), (unsigned long)rd(LVDS));

	/* 꺼져 있는 동안 언더런 상태를 지워 본다 */
	wr(PIPEBSTAT, rd(PIPEBSTAT));
	printf("    PIPEBSTAT 지운 뒤 %08lx\n", (unsigned long)rd(PIPEBSTAT));

	snooze((bigtime_t)holdMs * 1000);

	/* 켜기 */
	wr(LVDS, rd(LVDS) | LVDS_PORT_EN);
	(void)rd(LVDS);
	snooze(50000);
	wr(PP_CONTROL, rd(PP_CONTROL) | POWER_TARGET_ON);
	(void)rd(PP_CONTROL);
	wait_pp(1);
	wr(BLC_PWM_CTL, backlight);

	printf("후  PP_CONTROL %08lx  PP_STATUS %08lx  LVDS %08lx\n",
		(unsigned long)rd(PP_CONTROL), (unsigned long)rd(PP_STATUS),
		(unsigned long)rd(LVDS));
	printf("    PIPEBSTAT %08lx\n", (unsigned long)rd(PIPEBSTAT));
	return 0;
}
