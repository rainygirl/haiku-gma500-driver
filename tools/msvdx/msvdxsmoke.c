/* Send the decoder one render message whose command buffer does nothing,
 * and see whether the firmware answers.
 *
 * This proves the path before any codec is involved: the MMU page tables,
 * the LLDMA record that copies the command buffer into the MTX, the
 * host-to-MTX ring and the MTX-to-host ring. The command buffer is a
 * RENDER_BUFFER_HEADER followed by an empty register-write block.
 *
 *   msvdxsmoke /path/to/msvdx_fw.bin
 */
#include "msvdx.h"

#include <stdio.h>
#include <string.h>

#include "hw.h"

#define CMD_SIZE		0x3000
#define LLDMA_SIZE		0x2000

/* FW_VA_RENDER, 32 bytes, as psb-kmp's psb_msvdx.h lays it out. */
#define VA_MSGID_RENDER			0x81
#define VA_MSGID_CMD_COMPLETED	0xc0
#define VA_MSGID_CMD_FAILED		0xc5

int
main(int argc, char** argv)
{
	msvdx_buf cmd;
	uint32* c;
	uint32 cmdBytes;
	DMA_sLinkedList* ll;
	uint32 msg[8];
	uint32 reply[32];
	int n;
	int i;

	if (argc != 2) {
		fprintf(stderr, "usage: %s msvdx_fw.bin\n", argv[0]);
		return 2;
	}
	if (msvdx_open(argv[1]) != 0) {
		fprintf(stderr, "msvdx_open failed\n");
		return 1;
	}
	msvdx_dump_state("after open");
	for (i = 0; i < 4; i++) {
		printf("MTX PC %08lx\n", (unsigned long)msvdx_read_core_reg(0x05));
		snooze(10000);
	}
	printf("comms:");
	for (i = 0x2cb0; i < 0x2ce0; i += 4)
		printf(" %08lx", (unsigned long)msvdx_read(i));
	printf("\n");

	if (msvdx_alloc(&cmd, CMD_SIZE + LLDMA_SIZE) != 0) {
		fprintf(stderr, "alloc failed\n");
		msvdx_close();
		return 1;
	}
	printf("cmd buffer: phys %08lx dev %08lx\n", (unsigned long)cmd.phys,
		(unsigned long)cmd.dev);

	/* Command buffer */
	c = (uint32*)cmd.cpu;
	c[0] = CMD_HEADER;	/* RENDER_BUFFER_HEADER: cmd, reserved, 2 LLDMA ptrs */
	c[1] = 0;
	c[2] = 0;
	c[3] = 0;
	c[4] = CMD_REGVALPAIR_WRITE | 0;
	cmdBytes = 5 * 4;

	/* One LLDMA record, at CMD_SIZE, that DMAs the command buffer into the
	 * MTX (LLDMA_TYPE_RENDER_BUFF_VLD in psb_cmdbuf.c). */
	ll = (DMA_sLinkedList*)(cmd.cpu + CMD_SIZE);
	memset(ll, 0, sizeof(*ll));
	DMA_LL_SET_WD2(ll, REG_MSVDX_MTX_OFFSET + MTX_CORE_CR_MTX_SYSC_CDMAT_OFFSET);
	ll->ui32Word_6 = cmd.dev;	/* source address */
	DMA_LL_SET_WD7(ll, IMG_NULL);
	DMA_LL_SET_WD1(ll, DMA_PERIPH_INCR_OFF, DMA_PERIPH_INCR_1, cmdBytes / 4);
	DMA_LL_SET_WD0(ll, DMA_BSWAP_NO_SWAP, DMA_DIR_MEM_TO_PERIPH,
		DMA_PWIDTH_32_BIT);
	DMA_LL_SET_WD3(ll, DMA_ACC_DEL_0, DMA_BURST_1, 0 /* MMU group 0 */);
	DMA_LL_SET_WD4(ll, DMA_MODE_2D_OFF, 0);
	DMA_LL_SET_WD5(ll, 0, 0);
	msvdx_flush(&cmd, 0, cmd.size);

	for (i = 0; i < 8; i++)
		printf("lldma word %d: %08lx\n", i,
			(unsigned long)((uint32*)ll)[i]);

	/* Render message */
	memset(msg, 0, sizeof(msg));
	msg[0] = 32 | (VA_MSGID_RENDER << 8) | (cmdBytes << 16);
	msg[1] = msvdx_mmu_ptd();
	msg[2] = cmd.dev + CMD_SIZE;	/* LLDMA record */
	msg[3] = 1;					/* context */
	msg[4] = 0x1234;			/* fence */
	msg[5] = 0;					/* operating mode */
	msg[6] = 0;					/* first MB / last MB */
	msg[7] = FW_VA_RENDER_HOST_INT | FW_VA_RENDER_IS_VLD_NOT_MC;
	for (i = 0; i < 8; i++)
		printf("msg word %d: %08lx\n", i, (unsigned long)msg[i]);

	if (msvdx_send(msg) != 0) {
		fprintf(stderr, "send failed\n");
		msvdx_close();
		return 1;
	}
	msvdx_dump_state("after send");

	for (i = 0; i < 4; i++) {
		n = msvdx_receive(reply, 32, 1000);
		if (n == 0)
			break;
		printf("reply (%d words): id %02lx:", n,
			(unsigned long)((reply[0] >> 8) & 0xff));
		for (int j = 0; j < n && j < 32; j++)
			printf(" %08lx", (unsigned long)reply[j]);
		printf("\n");
	}
	if (i == 0)
		printf("no reply within 1 s\n");
	msvdx_dump_state("end");
	printf("DMAC 0x500:");
	for (i = 0x500; i < 0x580; i += 4) {
		if ((i & 0x1f) == 0) printf("\n  %04x:", i);
		printf(" %08lx", (unsigned long)msvdx_read(i));
	}
	printf("\nMMU 0x680:");
	for (i = 0x680; i < 0x6e0; i += 4) {
		if ((i & 0x1f) == 0) printf("\n  %04x:", i);
		printf(" %08lx", (unsigned long)msvdx_read(i));
	}
	printf("\n");
	for (i = 0; i < 4; i++) {
		printf("MTX PC %08lx\n", (unsigned long)msvdx_read_core_reg(0x05));
		snooze(10000);
	}
	printf("comms:");
	for (i = 0x2cb0; i < 0x2ce0; i += 4)
		printf(" %08lx", (unsigned long)msvdx_read(i));
	printf("\n");

	msvdx_close();
	return 0;
}
