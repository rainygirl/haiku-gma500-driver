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
static uint16 sHotX = 0;
static uint16 sHotY = 0;
static bool sCursorVisible = false;


static void
write32(uint32 offset, uint32 value)
{
	*(volatile uint32*)(sRegisters + offset) = value;
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
	}
	return NULL;
}
