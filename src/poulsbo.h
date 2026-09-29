/*
 * Poulsbo (Intel SCH US15W, GMA500) graphics driver for Haiku - 공유 정의.
 *
 * 이 칩의 그리기 엔진은 PowerVR SGX 라 문서가 없지만, 디스플레이 파이프라인은
 * i915 계열이라 레지스터가 그대로 통한다. 이 드라이버는 그 절반만 쓴다:
 * BIOS 가 세운 모드를 그대로 두고, 하드웨어 커서를 붙인다.
 *
 * 하드웨어 커서만으로도 이 기기에서는 의미가 있다. app_server 가 소프트웨어
 * 커서를 쓰면 포인터가 움직일 때마다 그 아래를 저장·복원·합성해야 하는데,
 * 1.33 GHz Atom 과 가속 없는 프레임버퍼에서는 그 비용이 그대로 보인다.
 */
#ifndef POULSBO_H
#define POULSBO_H

#include <Accelerant.h>
#include <Drivers.h>
#include <GraphicsDefs.h>
#include <OS.h>

#define POULSBO_ACCELERANT_NAME	"poulsbo.accelerant"
#define POULSBO_PRIVATE_MAGIC	'PSBO'

#define POULSBO_CURSOR_WIDTH	64
#define POULSBO_CURSOR_HEIGHT	64
/* 커널이 만든 영역을 accelerant(유저랜드)가 clone 하려면 B_CLONEABLE_AREA 가
   있어야 한다. 없으면 clone_area 가 B_NOT_ALLOWED 로 막힌다. */
#define POULSBO_AREA_PROTECTION	(B_READ_AREA | B_WRITE_AREA \
	| B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA | B_CLONEABLE_AREA)

#define POULSBO_CURSOR_BYTES	(POULSBO_CURSOR_WIDTH * POULSBO_CURSOR_HEIGHT * 4)

typedef struct {
	uint32			magic;
	area_id			shared_area;
	area_id			registers_area;		/* BAR0, 매핑된 MMIO */
	area_id			framebuffer_area;	/* 스톨른 프레임버퍼 */
	area_id			cursor_area;		/* 물리 연속 16 KB */
	uint32			cursor_physical;	/* CURBBASE 에 그대로 쓴다 */
	uint32			framebuffer_size;
	uint32			bytes_per_row;
	uint32			framebuffer_physical;
	display_mode	current_mode;
	/* 스톨른 메모리 전체와, 프레임버퍼 뒤에 남는 자리. 남는 자리는
	   스프라이트 평면(평면 C)의 원본 버퍼로 쓴다. framebuffer_area 는
	   스톨른 전체를 덮으므로 accelerant 가 그대로 접근한다.
	   새 필드는 구조체 끝에 둔다 - 드라이버와 accelerant 의 판이 어긋나도
	   앞쪽 필드의 자리가 밀리지 않는다. 짝이 맞지 않으면 sprite_size 가
	   0 이라 오버레이 훅이 스스로 비활성이 된다. */
	uint32			stolen_size;
	uint32			sprite_offset;
	uint32			sprite_size;
} poulsbo_shared_info;

typedef struct {
	uint32	magic;
	area_id	shared_info_area;
} poulsbo_private_data;

enum {
	POULSBO_GET_PRIVATE_DATA = B_DEVICE_OP_CODES_END + 1
};

/* 디스플레이 레지스터 (파이프 B = 내장 LVDS 패널) */
#define PSB_HTOTAL_B		0x61000
#define PSB_HSYNC_B			0x61008
#define PSB_VTOTAL_B		0x6100c
#define PSB_VSYNC_B			0x61014
#define PSB_PIPEBSRC		0x6101c
#define PSB_PIPEBCONF		0x71008
#define PSB_DSPBSTRIDE		0x71188
#define PSB_DSPBSURF		0x7119c
#define PSB_CURBCNTR		0x700c0
#define PSB_CURBBASE		0x700c4
#define PSB_CURBPOS			0x700c8

/* --- 스프라이트 평면 (평면 C) ------------------------------------------
 *
 * 이 칩에는 오버레이 평면이 없다. Intel SCH US15W 데이터시트(문서 319537)
 * 9.3.1 은 평면을 Display / Cursor / VGA 셋만 열거하고, 105 쪽 어디에도
 * overlay 라는 말이 나오지 않는다. MMIO 0x30000 의 오버레이 레지스터 블록은
 * i915 에서 물려받은 잔재이고, 켜면 디스플레이 FIFO 가 굶어 화면이 죽는다.
 *
 * 대신 데이터시트가 보장하는 것이 스프라이트 평면이다. 평면 C 는 1 차 평면과
 * 같은 레지스터 배치에 위치·크기·컬러키를 더 가지며, 파이프 B 에 붙일 수
 * 있다. 색 공간은 RGB 뿐이다 - 디스플레이 평면의 픽셀 포맷 필드에 YUV 값이
 * 없다. 확대·축소도 없다. 그래서 1:1 RGB 합성 전용이다.
 *
 * Z 순서 비트가 핵심이다. 0(= 디스플레이 A 위) 이어야 1 차 평면 위로 올라온다.
 * 1(= 오버레이 위) 로 두면 1 차 평면 아래에 깔려 보이지 않는다.
 */
#define PSB_DSPCCNTR		0x72180
#define PSB_DSPCLINOFF		0x72184
#define PSB_DSPCSTRIDE		0x72188
#define PSB_DSPCPOS			0x7218c
#define PSB_DSPCSIZE		0x72190
#define PSB_DSPCSURF		0x7219c
#define PSB_DSPCTILEOFF		0x721a4

#define PSB_PLANE_ENABLE			(1u << 31)
#define PSB_PLANE_FORMAT_RGB15		(0x4u << 26)
#define PSB_PLANE_FORMAT_RGB16		(0x5u << 26)
#define PSB_PLANE_FORMAT_RGB32		(0x6u << 26)
#define PSB_PLANE_SEL_PIPE_B		(1u << 24)
#define PSB_SPRITE_ABOVE_DISPLAY	0u

/* 커서 제어 비트 - 리눅스 gma500 의 gma_display.c 와 같은 조합 */
#define PSB_CURSOR_MODE_DISABLE		0x00
#define PSB_CURSOR_MODE_64_ARGB		((1 << 5) | 0x07)
/* 감마는 쓰지 않는다. 리눅스 gma500 은 이 비트를 켜지만 LUT 도 함께
   프로그래밍한다. BIOS 가 세운 모드를 그대로 쓰는 이 드라이버에서는 LUT 가
   초기화돼 있지 않아, 켜면 커서 색이 깨진다. */
#define PSB_MCURSOR_GAMMA_ENABLE	(1 << 26)
#define PSB_MCURSOR_PIPE_B			(1 << 28)
#define PSB_CURSOR_POS_SIGN			0x8000
#define PSB_CURSOR_X_SHIFT			0
#define PSB_CURSOR_Y_SHIFT			16

/* --- SGX 2D 엔진 -------------------------------------------------------
 *
 * 이 칩의 그리기 엔진은 PowerVR SGX535 다. 3D 쪽은 문서가 없지만 2D 블리터는
 * 리눅스 staging 시절의 gma500 (psb_2d.c, psb_reg.h) 에 명령 형식이 남아 있다.
 * 그 코드는 정작 `if (1 || ...) return cfb_fillrect(...)` 로 막힌 채 커밋돼서
 * 상류에서 실제로 쓰인 적은 없다.
 *
 * 리눅스와 다른 점이 하나 있고 그것이 핵심이다. psb_2d.c 는 2D 엔진의 주소
 * 기준(CR_BIF_TWOD_REQ_BASE)에 GTT 창의 주소를 넣지만, 그렇게 하면 이 기기에서
 * 아무것도 그려지지 않는다(엔진은 명령을 소비하고 완료 카운터도 올라가지만
 * 메모리는 그대로다). 프레임버퍼의 **물리 주소**를 기준으로 주면 그린다.
 */
#define PSB_SGX_OFFSET				0x40000
#define PSB_SGX_2D_SLAVE_PORT		0x4000
#define PSB_CR_BIF_TWOD_REQ_BASE	0x0c88
#define PSB_CR_2D_BLIT_STATUS		0x0e04
#define PSB_C2B_STATUS_BUSY			(1 << 24)
#define PSB_CR_2D_SOCIF				0x0e18
#define PSB_C2_SOCIF_FREESPACE_MASK	0xff

/* 명령 블록 머리 */
#define PSB_2D_SRC_OFF_BH			0x30000000
#define PSB_2D_FENCE_BH				0x70000000
#define PSB_2D_BLIT_BH				0x80000000
#define PSB_2D_SRC_SURF_BH			0x90000000
#define PSB_2D_DST_SURF_BH			0xa0000000
#define PSB_2D_FLUSH_BH				0xf0000000

#define PSB_2D_DST_8888ARGB			0x00060000
#define PSB_2D_SRC_8888ARGB			0x00060000
#define PSB_2D_USE_PAT				0x00010000
#define PSB_2D_ROP3_PATCOPY			0x0000f0f0
#define PSB_2D_ROP3_SRCCOPY			0x0000cccc
#define PSB_2D_ROP3_DSTINVERT		0x00005555
#define PSB_2D_COPYORDER_TL2BR		(0 << 23)
#define PSB_2D_COPYORDER_BR2TL		(1 << 23)
#define PSB_2D_COPYORDER_TR2BL		(2 << 23)
#define PSB_2D_COPYORDER_BL2TR		(3 << 23)
#define PSB_2D_XSTART_SHIFT			12
#define PSB_2D_XSIZE_SHIFT			12

#endif	/* POULSBO_H */
