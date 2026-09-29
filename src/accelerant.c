/*
 * Poulsbo accelerant.
 *
 * app_server 가 이 모듈을 통해 화면을 다룬다. 모드 설정은 하지 않는다 -
 * BIOS 가 세운 파이프 B 의 상태를 그대로 보고하고, 대신 하드웨어 커서를
 * 제공한다. 그것만으로도 소프트웨어 커서의 저장·복원·합성이 사라진다.
 *
 * 커서 레지스터 규칙은 실기기에서 확인한 것이다:
 *   CURBCNTR = (1<<28) 파이프B | (1<<26) 감마 | 0x27 (64x64 ARGB)
 *   CURBBASE = 물리 주소 (GTT 오프셋이 아니다 - Poulsbo 는 cursor_needs_phys)
 *   CURBPOS  = (y<<16) | x, 음수는 각 필드의 비트 15 로 부호를 준다
 */
#include <Accelerant.h>
#include <Drivers.h>
#include <GraphicsDefs.h>
#include <OS.h>
#include <video_overlay.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "poulsbo.h"

static int sDeviceFD = -1;
static area_id sSharedClone = -1;
static area_id sRegistersClone = -1;
static area_id sFramebufferClone = -1;
static area_id sCursorClone = -1;
static poulsbo_shared_info* sShared = NULL;
static uint8* sRegisters = NULL;
static uint8* sFramebuffer = NULL;
static uint8* sCursor = NULL;
static engine_token sEngineToken = { 1, B_2D_ACCELERATION, NULL };
static uint16 sHotX = 0;
static uint16 sHotY = 0;
static bool sCursorVisible = false;

/* 스프라이트 평면 상태. 버퍼는 하나만 내준다 - 스톨른에 남는 자리가
   3 MB 안팎이고, 이 기기에서 오버레이를 두 개 이상 쓸 일은 없다. */
static overlay_buffer sOverlayBuffer;
static bool sOverlayBufferUsed = false;
static bool sOverlayTokenUsed = false;
static bool sSpriteVisible = false;


static void
write32(uint32 offset, uint32 value)
{
	*(volatile uint32*)(sRegisters + offset) = value;
}


static uint32
read32(uint32 offset)
{
	return *(volatile uint32*)(sRegisters + offset);
}


static uint32
sgx_read(uint32 offset)
{
	return *(volatile uint32*)(sRegisters + PSB_SGX_OFFSET + offset);
}


static void
sgx_write(uint32 offset, uint32 value)
{
	*(volatile uint32*)(sRegisters + PSB_SGX_OFFSET + offset) = value;
}


/* 명령을 2D 슬레이브 포트에 밀어 넣는다. FIFO 가 빌 때까지 기다리되 무한히
   돌지는 않는다 - 엔진이 응답하지 않으면 그냥 포기한다. app_server 를 붙잡고
   있는 것보다 그림 한 번 빠지는 편이 낫다. */
static void
submit_2d(const uint32* commands, int count)
{
	int spin;
	int i;

	for (spin = 0; spin < 100000; spin++) {
		if ((int)(sgx_read(PSB_CR_2D_SOCIF) & PSB_C2_SOCIF_FREESPACE_MASK)
				>= count)
			break;
	}
	for (i = 0; i < count; i++)
		sgx_write(PSB_SGX_2D_SLAVE_PORT + i * 4, commands[i]);
	(void)sgx_read(PSB_SGX_2D_SLAVE_PORT + (count - 1) * 4);
}


static void
fill_rect_2d(uint16 left, uint16 top, uint16 right, uint16 bottom,
	uint32 colour, uint32 rop)
{
	uint32 commands[8];

	if (right < left || bottom < top)
		return;

	commands[0] = PSB_2D_FENCE_BH;
	commands[1] = PSB_2D_DST_SURF_BH | PSB_2D_DST_8888ARGB
		| sShared->bytes_per_row;
	commands[2] = 0;
	commands[3] = PSB_2D_BLIT_BH | rop;
	commands[4] = colour;
	commands[5] = ((uint32)left << PSB_2D_XSTART_SHIFT) | top;
	commands[6] = ((uint32)(right - left + 1) << PSB_2D_XSIZE_SHIFT)
		| (uint32)(bottom - top + 1);
	commands[7] = PSB_2D_FLUSH_BH;
	submit_2d(commands, 8);
}


static void
arm_cursor(void)
{
	/* base 를 다시 쓰는 것이 더블 버퍼 갱신 신호다.
	   감마는 켜지 않는다 - 이 파이프의 팔레트 LUT 는 초기화돼 있지 않고
	   (DSPBCNTR 의 감마 비트도 꺼져 있다) 커서만 그 LUT 를 통과시키면
	   검정 테두리가 흰색과 섞여 깨져 보인다. 실기기에서 확인했다. */
	write32(PSB_CURBCNTR, sCursorVisible
		? (PSB_MCURSOR_PIPE_B | PSB_CURSOR_MODE_64_ARGB)
		: PSB_CURSOR_MODE_DISABLE);
	write32(PSB_CURBBASE, sCursorVisible ? sShared->cursor_physical : 0);
}


static status_t
init_common(int fd, bool isClone)
{
	poulsbo_private_data data;

	sDeviceFD = fd;
	data.magic = POULSBO_PRIVATE_MAGIC;
	if (ioctl(sDeviceFD, POULSBO_GET_PRIVATE_DATA, &data, sizeof(data)) != 0)
		return errno != 0 ? (status_t)errno : B_ERROR;

	sSharedClone = clone_area("poulsbo shared clone", (void**)&sShared,
		B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, data.shared_info_area);
	if (sSharedClone < B_OK)
		return sSharedClone;

	sRegistersClone = clone_area("poulsbo regs clone", (void**)&sRegisters,
		B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, sShared->registers_area);
	if (sRegistersClone < B_OK)
		return sRegistersClone;

	sFramebufferClone = clone_area("poulsbo fb clone", (void**)&sFramebuffer,
		B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, sShared->framebuffer_area);
	if (sFramebufferClone < B_OK)
		return sFramebufferClone;

	sCursorClone = clone_area("poulsbo cursor clone", (void**)&sCursor,
		B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, sShared->cursor_area);
	if (sCursorClone < B_OK)
		return sCursorClone;

	return B_OK;
}


static void
uninit_common(void)
{
	if (sCursorClone >= B_OK) delete_area(sCursorClone);
	if (sFramebufferClone >= B_OK) delete_area(sFramebufferClone);
	if (sRegistersClone >= B_OK) delete_area(sRegistersClone);
	if (sSharedClone >= B_OK) delete_area(sSharedClone);
	sCursorClone = sFramebufferClone = sRegistersClone = sSharedClone = -1;
}


static status_t
poulsbo_init_accelerant(int fd)
{
	status_t status = init_common(fd, false);
	if (status != B_OK)
		return status;
	sCursorVisible = false;
	arm_cursor();

	/* 2D 엔진의 주소 기준. 리눅스는 여기에 GTT 창 주소를 넣지만 이 기기에서는
	   프레임버퍼의 물리 주소여야 실제로 그려진다 - poulsbo.h 의 설명 참고. */
	sgx_write(PSB_CR_BIF_TWOD_REQ_BASE, sShared->framebuffer_physical);
	(void)sgx_read(PSB_CR_BIF_TWOD_REQ_BASE);
	return B_OK;
}


static void
poulsbo_uninit_accelerant(void)
{
	sCursorVisible = false;
	if (sRegisters != NULL)
		arm_cursor();
	uninit_common();
}


static ssize_t
poulsbo_accelerant_clone_info_size(void)
{
	return B_PATH_NAME_LENGTH;
}


static void
poulsbo_get_accelerant_clone_info(void* data)
{
	strcpy((char*)data, "/dev/graphics/poulsbo");
}


static status_t
poulsbo_clone_accelerant(void* data)
{
	int fd = open((const char*)data, B_READ_WRITE);
	if (fd < 0)
		return errno;
	return init_common(fd, true);
}


static status_t
poulsbo_get_accelerant_device_info(accelerant_device_info* info)
{
	info->version = B_ACCELERANT_VERSION;
	strcpy(info->name, "Intel SCH US15W (Poulsbo) / GMA500");
	strcpy(info->chipset, "GMA500");
	strcpy(info->serial_no, "");
	info->memory = sShared->framebuffer_size;
	info->dac_speed = sShared->current_mode.timing.pixel_clock / 1000;
	return B_OK;
}


static uint32
poulsbo_accelerant_mode_count(void)
{
	return 1;
}


static status_t
poulsbo_get_mode_list(display_mode* modes)
{
	modes[0] = sShared->current_mode;
	return B_OK;
}


static status_t
poulsbo_propose_display_mode(display_mode* target, const display_mode* low,
	const display_mode* high)
{
	/* 패널은 고정이다. 들어온 요청이 무엇이든 지금 모드로 되돌려 준다. */
	*target = sShared->current_mode;
	return B_OK;
}


static status_t
poulsbo_set_display_mode(display_mode* mode)
{
	/* 모드를 바꾸지 않는다. 같은 모드 요청만 성공으로 친다. */
	if (mode->virtual_width != sShared->current_mode.virtual_width
		|| mode->virtual_height != sShared->current_mode.virtual_height)
		return B_BAD_VALUE;
	return B_OK;
}


static status_t
poulsbo_get_display_mode(display_mode* mode)
{
	*mode = sShared->current_mode;
	return B_OK;
}


static status_t
poulsbo_get_frame_buffer_config(frame_buffer_config* config)
{
	config->frame_buffer = sFramebuffer;
	config->frame_buffer_dma = (void*)sShared->framebuffer_physical;
	config->bytes_per_row = sShared->bytes_per_row;
	return B_OK;
}


static status_t
poulsbo_get_pixel_clock_limits(display_mode* mode, uint32* low, uint32* high)
{
	*low = mode->timing.pixel_clock;
	*high = mode->timing.pixel_clock;
	return B_OK;
}


static uint32
poulsbo_dpms_capabilities(void)
{
	return B_DPMS_ON;
}


static uint32
poulsbo_dpms_mode(void)
{
	return B_DPMS_ON;
}


static status_t
poulsbo_set_dpms_mode(uint32 mode)
{
	return mode == B_DPMS_ON ? B_OK : B_UNSUPPORTED;
}


static void
poulsbo_move_cursor(uint16 x, uint16 y)
{
	int32 px = (int32)x - (int32)sHotX;
	int32 py = (int32)y - (int32)sHotY;
	uint32 position = 0;

	if (px < 0) {
		position |= PSB_CURSOR_POS_SIGN << PSB_CURSOR_X_SHIFT;
		px = -px;
	}
	if (py < 0) {
		position |= PSB_CURSOR_POS_SIGN << PSB_CURSOR_Y_SHIFT;
		py = -py;
	}
	position |= ((uint32)px & 0x7fff) << PSB_CURSOR_X_SHIFT;
	position |= ((uint32)py & 0x7fff) << PSB_CURSOR_Y_SHIFT;

	write32(PSB_CURBPOS, position);
	write32(PSB_CURBBASE, sCursorVisible ? sShared->cursor_physical : 0);
}


static void
poulsbo_show_cursor(bool visible)
{
	sCursorVisible = visible;
	arm_cursor();
}


static status_t
poulsbo_set_cursor_bitmap(uint16 width, uint16 height, uint16 hotX,
	uint16 hotY, color_space space, uint16 bytesPerRow, const uint8* data)
{
	uint16 row;

	if (width > POULSBO_CURSOR_WIDTH || height > POULSBO_CURSOR_HEIGHT)
		return B_UNSUPPORTED;
	if (space != B_RGBA32 && space != B_RGB32)
		return B_UNSUPPORTED;

	memset(sCursor, 0, POULSBO_CURSOR_BYTES);
	for (row = 0; row < height; row++) {
		memcpy(sCursor + row * POULSBO_CURSOR_WIDTH * 4,
			data + row * bytesPerRow, width * 4);
	}
	sHotX = hotX;
	sHotY = hotY;
	arm_cursor();
	return B_OK;
}


static status_t
poulsbo_set_cursor_shape(uint16 width, uint16 height, uint16 hotX,
	uint16 hotY, const uint8* andMask, const uint8* xorMask)
{
	/* 옛 2색 커서. 하드웨어는 ARGB 만 아니까 흑백으로 펼쳐 준다. */
	uint16 x, y;
	uint16 stride = (width + 7) / 8;

	if (width > POULSBO_CURSOR_WIDTH || height > POULSBO_CURSOR_HEIGHT)
		return B_UNSUPPORTED;

	memset(sCursor, 0, POULSBO_CURSOR_BYTES);
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			uint8 bit = 0x80 >> (x & 7);
			bool opaque = (andMask[y * stride + x / 8] & bit) == 0;
			bool black = (xorMask[y * stride + x / 8] & bit) == 0;
			uint32* pixel = (uint32*)(sCursor
				+ (y * POULSBO_CURSOR_WIDTH + x) * 4);
			if (!opaque)
				*pixel = 0;
			else
				*pixel = black ? 0xff000000 : 0xffffffff;
		}
	}
	sHotX = hotX;
	sHotY = hotY;
	arm_cursor();
	return B_OK;
}


static uint32
poulsbo_accelerant_engine_count(void)
{
	return 1;
}


static void
poulsbo_wait_engine_idle(void)
{
	int spin;
	for (spin = 0; spin < 1000000; spin++) {
		if ((sgx_read(PSB_CR_2D_BLIT_STATUS) & PSB_C2B_STATUS_BUSY) == 0)
			return;
	}
}


static status_t
poulsbo_sync_to_token(sync_token* token)
{
	/* 하드웨어가 주는 것은 "완료한 블릿 수"와 busy 비트뿐이라, 토큰별로
	   기다리는 대신 엔진이 놀 때까지 기다린다. 보수적이지만 틀리지 않는다. */
	poulsbo_wait_engine_idle();
	return B_OK;
}


static status_t
poulsbo_get_sync_token(engine_token* engineToken, sync_token* token)
{
	token->engine_id = engineToken->engine_id;
	token->counter = sgx_read(PSB_CR_2D_BLIT_STATUS) & 0x00ffffff;
	return B_OK;
}


static status_t
poulsbo_acquire_engine(uint32 capabilities, uint32 maxWait, sync_token* token,
	engine_token** _engineToken)
{
	if (token != NULL)
		poulsbo_sync_to_token(token);
	*_engineToken = &sEngineToken;
	return B_OK;
}


static status_t
poulsbo_release_engine(engine_token* engineToken, sync_token* token)
{
	if (token != NULL)
		poulsbo_get_sync_token(engineToken, token);
	return B_OK;
}


static void
poulsbo_screen_to_screen_blit(engine_token* engineToken, blit_params* list,
	uint32 count)
{
	uint32 commands[10];
	uint32 index;

	for (index = 0; index < count; index++) {
		blit_params* p = &list[index];
		uint16 width = p->width + 1;
		uint16 height = p->height + 1;
		uint16 sourceX = p->src_left;
		uint16 sourceY = p->src_top;
		uint16 destX = p->dest_left;
		uint16 destY = p->dest_top;
		uint32 direction;

		/* 겹치는 복사는 방향을 맞춰야 자기 꼬리를 밟지 않는다. */
		if ((int)sourceX - (int)destX < 0) {
			direction = ((int)sourceY - (int)destY < 0)
				? PSB_2D_COPYORDER_BR2TL : PSB_2D_COPYORDER_TR2BL;
		} else {
			direction = ((int)sourceY - (int)destY < 0)
				? PSB_2D_COPYORDER_BL2TR : PSB_2D_COPYORDER_TL2BR;
		}
		if (direction == PSB_2D_COPYORDER_BR2TL
			|| direction == PSB_2D_COPYORDER_TR2BL) {
			sourceX += width - 1;
			destX += width - 1;
		}
		if (direction == PSB_2D_COPYORDER_BR2TL
			|| direction == PSB_2D_COPYORDER_BL2TR) {
			sourceY += height - 1;
			destY += height - 1;
		}

		commands[0] = PSB_2D_FENCE_BH;
		commands[1] = PSB_2D_DST_SURF_BH | PSB_2D_DST_8888ARGB
			| sShared->bytes_per_row;
		commands[2] = 0;
		commands[3] = PSB_2D_SRC_SURF_BH | PSB_2D_SRC_8888ARGB
			| sShared->bytes_per_row;
		commands[4] = 0;
		commands[5] = PSB_2D_SRC_OFF_BH
			| ((uint32)sourceX << PSB_2D_XSTART_SHIFT) | sourceY;
		commands[6] = PSB_2D_BLIT_BH | PSB_2D_USE_PAT | PSB_2D_ROP3_SRCCOPY
			| direction;
		commands[7] = ((uint32)destX << PSB_2D_XSTART_SHIFT) | destY;
		commands[8] = ((uint32)width << PSB_2D_XSIZE_SHIFT) | height;
		commands[9] = PSB_2D_FLUSH_BH;
		submit_2d(commands, 10);
	}
}


static void
poulsbo_fill_rectangle(engine_token* engineToken, uint32 colour,
	fill_rect_params* list, uint32 count)
{
	uint32 index;
	for (index = 0; index < count; index++) {
		fill_rect_2d(list[index].left, list[index].top, list[index].right,
			list[index].bottom, colour, PSB_2D_ROP3_PATCOPY);
	}
}


static void
poulsbo_invert_rectangle(engine_token* engineToken, fill_rect_params* list,
	uint32 count)
{
	uint32 index;
	for (index = 0; index < count; index++) {
		fill_rect_2d(list[index].left, list[index].top, list[index].right,
			list[index].bottom, 0, PSB_2D_ROP3_DSTINVERT);
	}
}




//	#pragma mark - 스프라이트 평면 (오버레이 훅)


/* 이 칩에는 오버레이 평면이 없다. 데이터시트(문서 319537) 9.3.1 이 평면을
   Display / Cursor / VGA 셋만 열거하고 overlay 라는 말은 105 쪽 어디에도
   없다. 대신 평면 C 를 스프라이트로 파이프 B 에 붙일 수 있다. RGB 전용이고
   확대·축소가 없으므로 1:1 로만 내준다. app_server 는 이 훅들을 실제로
   호출하므로(BitmapManager.cpp 의 AcquireOverlayChannel), 영상 창이 이
   경로를 타면 프레임마다 일어나던 CPU 합성이 사라진다. */

static uint32
poulsbo_overlay_count(const display_mode* mode)
{
	return sShared->sprite_size > 0 ? 1 : 0;
}


static const uint32*
poulsbo_overlay_supported_spaces(const display_mode* mode)
{
	/* 디스플레이 평면의 픽셀 포맷 필드에는 YUV 값이 없다. RGB 뿐이다. */
	static const uint32 kSpaces[] = { B_RGB16, B_RGB32, 0 };
	return kSpaces;
}


static uint32
poulsbo_overlay_supported_features(uint32 colorSpace)
{
	return B_OVERLAY_COLOR_KEY;
}


static const overlay_buffer*
poulsbo_allocate_overlay_buffer(color_space space, uint16 width, uint16 height)
{
	uint32 bytesPerPixel;
	uint32 bytesPerRow;

	if (sOverlayBufferUsed)
		return NULL;

	switch (space) {
		case B_RGB16:
			bytesPerPixel = 2;
			break;
		case B_RGB32:
			bytesPerPixel = 4;
			break;
		default:
			return NULL;
	}

	/* 스트라이드는 64 바이트에 맞춘다 - 디스플레이 평면이 캐시라인
	   단위로 읽는다. */
	bytesPerRow = ((uint32)width * bytesPerPixel + 63) & ~63;
	if (bytesPerRow * height > sShared->sprite_size)
		return NULL;

	sOverlayBuffer.space = space;
	sOverlayBuffer.width = width;
	sOverlayBuffer.height = height;
	sOverlayBuffer.bytes_per_row = bytesPerRow;
	sOverlayBuffer.buffer = sFramebuffer + sShared->sprite_offset;
	sOverlayBuffer.buffer_dma = (void*)(sShared->framebuffer_physical
		+ sShared->sprite_offset);
	sOverlayBufferUsed = true;
	return &sOverlayBuffer;
}


static status_t
poulsbo_release_overlay_buffer(const overlay_buffer* buffer)
{
	if (buffer != &sOverlayBuffer)
		return B_BAD_VALUE;
	if (sSpriteVisible) {
		write32(PSB_DSPCCNTR, 0);
		write32(PSB_DSPCLINOFF, 0);
		(void)read32(PSB_DSPCLINOFF);
		sSpriteVisible = false;
	}
	sOverlayBufferUsed = false;
	return B_OK;
}


static status_t
poulsbo_get_overlay_constraints(const display_mode* mode,
	const overlay_buffer* buffer, overlay_constraints* constraints)
{
	if (constraints == NULL)
		return B_BAD_VALUE;
	memset(constraints, 0, sizeof(overlay_constraints));

	constraints->view.h_alignment = 0;
	constraints->view.v_alignment = 0;
	constraints->view.width_alignment = 7;
	constraints->view.height_alignment = 0;
	constraints->view.width.min = 4;
	constraints->view.height.min = 4;
	constraints->view.width.max = mode->virtual_width;
	constraints->view.height.max = mode->virtual_height;

	constraints->window.h_alignment = 0;
	constraints->window.v_alignment = 0;
	constraints->window.width_alignment = 0;
	constraints->window.height_alignment = 0;
	constraints->window.width.min = 4;
	constraints->window.height.min = 4;
	constraints->window.width.max = mode->virtual_width;
	constraints->window.height.max = mode->virtual_height;

	/* 확대·축소가 없다. 스프라이트 평면은 원본을 1:1 로만 내보낸다. */
	constraints->h_scale.min = 1.0f;
	constraints->h_scale.max = 1.0f;
	constraints->v_scale.min = 1.0f;
	constraints->v_scale.max = 1.0f;
	return B_OK;
}


static overlay_token
poulsbo_allocate_overlay(void)
{
	if (sOverlayTokenUsed)
		return NULL;
	sOverlayTokenUsed = true;
	return (overlay_token)&sOverlayTokenUsed;
}


static status_t
poulsbo_release_overlay(overlay_token token)
{
	if (token != (overlay_token)&sOverlayTokenUsed)
		return B_BAD_VALUE;
	if (sSpriteVisible) {
		write32(PSB_DSPCCNTR, 0);
		write32(PSB_DSPCLINOFF, 0);
		(void)read32(PSB_DSPCLINOFF);
		sSpriteVisible = false;
	}
	sOverlayTokenUsed = false;
	return B_OK;
}


static status_t
poulsbo_configure_overlay(overlay_token token, const overlay_buffer* buffer,
	const overlay_window* window, const overlay_view* view)
{
	uint32 control;
	uint32 offset;
	int32 left, top, right, bottom;

	if (token != (overlay_token)&sOverlayTokenUsed
		|| buffer != &sOverlayBuffer)
		return B_BAD_VALUE;

	if (window == NULL || view == NULL) {
		/* 끄라는 뜻이다. */
		if (sSpriteVisible) {
			write32(PSB_DSPCCNTR, 0);
			write32(PSB_DSPCLINOFF, 0);
			(void)read32(PSB_DSPCLINOFF);
			sSpriteVisible = false;
		}
		return B_OK;
	}

	/* 화면 밖으로 나가는 부분은 잘라 낸다. 스프라이트에는 확대·축소가
	   없으므로 원본 시작점도 같은 만큼 민다. */
	left = window->h_start;
	top = window->v_start;
	right = left + window->width;
	bottom = top + window->height;
	if (right > (int32)sShared->current_mode.virtual_width)
		right = sShared->current_mode.virtual_width;
	if (bottom > (int32)sShared->current_mode.virtual_height)
		bottom = sShared->current_mode.virtual_height;

	offset = (uint32)view->v_start * buffer->bytes_per_row;
	if (left < 0) {
		offset += (uint32)(-left) * (buffer->bytes_per_row / buffer->width);
		left = 0;
	}
	if (top < 0) {
		offset += (uint32)(-top) * buffer->bytes_per_row;
		top = 0;
	}
	offset += (uint32)view->h_start
		* (buffer->space == B_RGB16 ? 2 : 4);

	if (left >= right || top >= bottom) {
		if (sSpriteVisible) {
			write32(PSB_DSPCCNTR, 0);
			write32(PSB_DSPCLINOFF, 0);
			(void)read32(PSB_DSPCLINOFF);
			sSpriteVisible = false;
		}
		return B_OK;
	}

	control = PSB_PLANE_ENABLE | PSB_PLANE_SEL_PIPE_B
		| PSB_SPRITE_ABOVE_DISPLAY;
	control |= buffer->space == B_RGB16
		? PSB_PLANE_FORMAT_RGB16 : PSB_PLANE_FORMAT_RGB32;

	write32(PSB_DSPCSTRIDE, buffer->bytes_per_row);
	write32(PSB_DSPCPOS, ((uint32)top << 16) | (uint32)left);
	write32(PSB_DSPCSIZE, ((uint32)(bottom - top - 1) << 16)
		| (uint32)(right - left - 1));
	write32(PSB_DSPCTILEOFF, 0);
	write32(PSB_DSPCCNTR, control);
	/* 평면 레지스터는 주소 레지스터를 써야 반영된다. 이 세대는 SURF 가
	   아니라 LINOFF 가 주소다 - SURF 에 쓴 값은 되읽히지 않는다. */
	write32(PSB_DSPCLINOFF, sShared->sprite_offset + offset);
	(void)read32(PSB_DSPCLINOFF);
	sSpriteVisible = true;
	return B_OK;
}


void*
get_accelerant_hook(uint32 feature, void* data)
{
	switch (feature) {
		case B_INIT_ACCELERANT:
			return (void*)poulsbo_init_accelerant;
		case B_UNINIT_ACCELERANT:
			return (void*)poulsbo_uninit_accelerant;
		case B_CLONE_ACCELERANT:
			return (void*)poulsbo_clone_accelerant;
		case B_ACCELERANT_CLONE_INFO_SIZE:
			return (void*)poulsbo_accelerant_clone_info_size;
		case B_GET_ACCELERANT_CLONE_INFO:
			return (void*)poulsbo_get_accelerant_clone_info;
		case B_GET_ACCELERANT_DEVICE_INFO:
			return (void*)poulsbo_get_accelerant_device_info;

		case B_ACCELERANT_MODE_COUNT:
			return (void*)poulsbo_accelerant_mode_count;
		case B_GET_MODE_LIST:
			return (void*)poulsbo_get_mode_list;
		case B_PROPOSE_DISPLAY_MODE:
			return (void*)poulsbo_propose_display_mode;
		case B_SET_DISPLAY_MODE:
			return (void*)poulsbo_set_display_mode;
		case B_GET_DISPLAY_MODE:
			return (void*)poulsbo_get_display_mode;
		case B_GET_FRAME_BUFFER_CONFIG:
			return (void*)poulsbo_get_frame_buffer_config;
		case B_GET_PIXEL_CLOCK_LIMITS:
			return (void*)poulsbo_get_pixel_clock_limits;

		case B_DPMS_CAPABILITIES:
			return (void*)poulsbo_dpms_capabilities;
		case B_DPMS_MODE:
			return (void*)poulsbo_dpms_mode;
		case B_SET_DPMS_MODE:
			return (void*)poulsbo_set_dpms_mode;

		case B_MOVE_CURSOR:
			return (void*)poulsbo_move_cursor;
		case B_SHOW_CURSOR:
			return (void*)poulsbo_show_cursor;
		case B_SET_CURSOR_BITMAP:
			return (void*)poulsbo_set_cursor_bitmap;
		case B_SET_CURSOR_SHAPE:
			return (void*)poulsbo_set_cursor_shape;

		case B_ACCELERANT_ENGINE_COUNT:
			return (void*)poulsbo_accelerant_engine_count;
		case B_ACQUIRE_ENGINE:
			return (void*)poulsbo_acquire_engine;
		case B_RELEASE_ENGINE:
			return (void*)poulsbo_release_engine;
		case B_WAIT_ENGINE_IDLE:
			return (void*)poulsbo_wait_engine_idle;
		case B_GET_SYNC_TOKEN:
			return (void*)poulsbo_get_sync_token;
		case B_SYNC_TO_TOKEN:
			return (void*)poulsbo_sync_to_token;

		case B_SCREEN_TO_SCREEN_BLIT:
			return (void*)poulsbo_screen_to_screen_blit;
		case B_FILL_RECTANGLE:
			return (void*)poulsbo_fill_rectangle;
		case B_INVERT_RECTANGLE:
			return (void*)poulsbo_invert_rectangle;

		case B_OVERLAY_COUNT:
			return (void*)poulsbo_overlay_count;
		case B_OVERLAY_SUPPORTED_SPACES:
			return (void*)poulsbo_overlay_supported_spaces;
		case B_OVERLAY_SUPPORTED_FEATURES:
			return (void*)poulsbo_overlay_supported_features;
		case B_ALLOCATE_OVERLAY_BUFFER:
			return (void*)poulsbo_allocate_overlay_buffer;
		case B_RELEASE_OVERLAY_BUFFER:
			return (void*)poulsbo_release_overlay_buffer;
		case B_GET_OVERLAY_CONSTRAINTS:
			return (void*)poulsbo_get_overlay_constraints;
		case B_ALLOCATE_OVERLAY:
			return (void*)poulsbo_allocate_overlay;
		case B_RELEASE_OVERLAY:
			return (void*)poulsbo_release_overlay;
		case B_CONFIGURE_OVERLAY:
			return (void*)poulsbo_configure_overlay;
	}
	return NULL;
}
