/*
 * Poulsbo 커널 드라이버.
 *
 * 하는 일이 적다. BIOS(또는 VESA)가 이미 세워 놓은 파이프 B 의 모드를 읽어
 * 그대로 보고하고, 프레임버퍼와 MMIO 를 영역으로 매핑하고, 하드웨어 커서용
 * 물리 연속 16 KB 를 잡아 둔다. 모드 설정은 하지 않는다 - 이 기기의 패널은
 * 1600x768 고정이고, 잘못 건드리면 화면이 죽는다.
 */
#include <KernelExport.h>
#include <Drivers.h>
#include <PCI.h>
#include <graphic_driver.h>

#include <stdlib.h>
#include <string.h>

#include "poulsbo.h"

#define TRACE(x...)	dprintf("poulsbo: " x)

/* 커널이 내보내는 비공개 함수다. private/kernel/vm/vm.h 를 그대로 넣으면
   C++ 정의가 딸려 오므로 선언만 옮겨 적는다. vesa 드라이버도 이 함수로
   프레임버퍼에 write-combining 을 건다. */
extern status_t vm_set_area_memory_type(area_id id, phys_addr_t physicalBase,
	uint32 type);

#define VENDOR_INTEL	0x8086
#define DEVICE_POULSBO	0x8108

int32 api_version = B_CUR_DRIVER_API_VERSION;

static pci_module_info* sPCI = NULL;
static pci_info sPCIInfo;
static bool sHasDevice = false;

static area_id sSharedArea = -1;
static area_id sRegistersArea = -1;
static area_id sFramebufferArea = -1;
static area_id sCursorArea = -1;
static poulsbo_shared_info* sShared = NULL;
static uint8* sRegisters = NULL;

static const char* sDeviceNames[] = { "graphics/poulsbo", NULL };


static uint32
read32(uint32 offset)
{
	return *(volatile uint32*)(sRegisters + offset);
}


static bool
find_poulsbo(void)
{
	long index;
	for (index = 0; sPCI->get_nth_pci_info(index, &sPCIInfo) == B_OK; index++) {
		if (sPCIInfo.vendor_id == VENDOR_INTEL
			&& sPCIInfo.device_id == DEVICE_POULSBO)
			return true;
	}
	return false;
}


status_t
init_hardware(void)
{
	bool found;
	if (get_module(B_PCI_MODULE_NAME, (module_info**)&sPCI) != B_OK)
		return B_ERROR;
	found = find_poulsbo();
	put_module(B_PCI_MODULE_NAME);
	sPCI = NULL;
	return found ? B_OK : B_ERROR;
}


static void
read_current_mode(display_mode* mode)
{
	uint32 htotal = read32(PSB_HTOTAL_B);
	uint32 hsync = read32(PSB_HSYNC_B);
	uint32 vtotal = read32(PSB_VTOTAL_B);
	uint32 vsync = read32(PSB_VSYNC_B);
	uint32 source = read32(PSB_PIPEBSRC);

	memset(mode, 0, sizeof(*mode));
	mode->timing.h_display = (htotal & 0xffff) + 1;
	mode->timing.h_total = (htotal >> 16) + 1;
	mode->timing.h_sync_start = (hsync & 0xffff) + 1;
	mode->timing.h_sync_end = (hsync >> 16) + 1;
	mode->timing.v_display = (vtotal & 0xffff) + 1;
	mode->timing.v_total = (vtotal >> 16) + 1;
	mode->timing.v_sync_start = (vsync & 0xffff) + 1;
	mode->timing.v_sync_end = (vsync >> 16) + 1;
	/* 패널은 60 Hz 고정이다. 픽셀 클럭은 타이밍에서 되짚어 kHz 로 적는다.
	   커널 모듈에는 libgcc 가 없어 64비트 나눗셈을 쓸 수 없다 - 이 값들은
	   32비트 안에서 곱해도 넘치지 않는다 (1794 * 778 * 60 = 8.4e7). */
	mode->timing.pixel_clock = ((uint32)mode->timing.h_total
		* (uint32)mode->timing.v_total * 60) / 1000;
	mode->space = B_RGB32_LITTLE;
	mode->virtual_width = ((source >> 16) & 0xffff) + 1;
	mode->virtual_height = (source & 0xffff) + 1;
	mode->h_display_start = 0;
	mode->v_display_start = 0;
	mode->flags = B_SCROLL;
}


status_t
init_driver(void)
{
	physical_entry entry;
	void* address;
	uint32 stolenBase;
	uint32 stolenSize;
	uint32 stride;
	uint32 height;
	uint32 index;
	uint32 entries;
	area_id gttArea;
	uint32* gtt;

	if (get_module(B_PCI_MODULE_NAME, (module_info**)&sPCI) != B_OK)
		return B_ERROR;
	if (!find_poulsbo()) {
		put_module(B_PCI_MODULE_NAME);
		return B_ERROR;
	}
	sHasDevice = true;

	/* MMIO (BAR0). 레지스터라 캐시를 끈다. */
	sRegistersArea = map_physical_memory("poulsbo registers",
		sPCIInfo.u.h0.base_registers[0], sPCIInfo.u.h0.base_register_sizes[0],
		B_ANY_KERNEL_ADDRESS, POULSBO_AREA_PROTECTION, (void**)&sRegisters);
	if (sRegistersArea < B_OK) {
		TRACE("MMIO 매핑 실패\n");
		goto error;
	}

	/* 프레임버퍼의 물리 주소는 GTT 0번 칸에 적혀 있다 (BIOS 가 채운다). */
	gttArea = map_physical_memory("poulsbo gtt",
		sPCIInfo.u.h0.base_registers[3], sPCIInfo.u.h0.base_register_sizes[3],
		B_ANY_KERNEL_ADDRESS, B_READ_AREA, (void**)&gtt);
	if (gttArea < B_OK) {
		TRACE("GTT 매핑 실패\n");
		goto error;
	}
	stolenBase = gtt[0] & ~0xfff;
	/* 스톨른이 어디까지인지는 GTT 가 알려준다. BIOS 는 앞쪽 칸들을 스톨른
	   페이지로 연속 매핑하고, 그 뒤부터는 스크래치 페이지를 가리킨다.
	   연속이 끊기는 지점이 스톨른의 끝이다. */
	entries = sPCIInfo.u.h0.base_register_sizes[3] / 4;
	stolenSize = 0;
	for (index = 0; index < entries; index++) {
		if ((gtt[index] & 1) == 0)
			break;
		if ((gtt[index] & ~0xfff) != stolenBase + index * B_PAGE_SIZE)
			break;
		stolenSize += B_PAGE_SIZE;
	}
	delete_area(gttArea);

	stride = read32(PSB_DSPBSTRIDE) & 0xffff;
	height = (read32(PSB_PIPEBSRC) & 0xffff) + 1;
	if (stride == 0 || height == 0 || (read32(PSB_PIPEBCONF) & 0x80000000) == 0) {
		TRACE("파이프 B 가 꺼져 있다 - 이 드라이버는 모드를 세우지 않는다\n");
		goto error;
	}

	if (stolenSize < stride * height)
		stolenSize = stride * height;
	/* 프레임버퍼만이 아니라 스톨른 전체를 덮어 둔다. 뒤에 남는 자리를
	   스프라이트 평면의 원본 버퍼로 쓰기 때문이다. */
	sFramebufferArea = map_physical_memory("poulsbo framebuffer", stolenBase,
		stolenSize, B_ANY_KERNEL_ADDRESS, POULSBO_AREA_PROTECTION,
		&address);
	if (sFramebufferArea < B_OK) {
		TRACE("프레임버퍼 매핑 실패\n");
		goto error;
	}
	/* 스캔아웃은 CPU 캐시를 들여다보지 않는다. write-back 으로 두면 그린
	   픽셀이 캐시에 머물다가 늦게 반영돼 창을 끌 때 잔상이 남는다.
	   vesa 드라이버와 같이 write-combining 으로 바꾼다. */
	vm_set_area_memory_type(sFramebufferArea, stolenBase,
		B_WRITE_COMBINING_MEMORY);

	/* 커서는 물리적으로 연속인 16 KB 를 요구한다 (cursor_needs_phys). */
	sCursorArea = create_area("poulsbo cursor", &address, B_ANY_KERNEL_ADDRESS,
		B_PAGE_SIZE * 4, B_CONTIGUOUS, POULSBO_AREA_PROTECTION);
	if (sCursorArea < B_OK) {
		TRACE("커서 영역 생성 실패\n");
		goto error;
	}
	memset(address, 0, B_PAGE_SIZE * 4);
	if (get_memory_map(address, B_PAGE_SIZE * 4, &entry, 1) != B_OK) {
		TRACE("커서 물리 주소 조회 실패\n");
		goto error;
	}

	sSharedArea = create_area("poulsbo shared", (void**)&sShared,
		B_ANY_KERNEL_ADDRESS,
		(sizeof(poulsbo_shared_info) + B_PAGE_SIZE - 1) & ~(B_PAGE_SIZE - 1),
		B_FULL_LOCK, POULSBO_AREA_PROTECTION);
	if (sSharedArea < B_OK) {
		TRACE("공유 영역 생성 실패\n");
		goto error;
	}

	memset(sShared, 0, sizeof(poulsbo_shared_info));
	sShared->magic = POULSBO_PRIVATE_MAGIC;
	sShared->shared_area = sSharedArea;
	sShared->registers_area = sRegistersArea;
	sShared->framebuffer_area = sFramebufferArea;
	sShared->cursor_area = sCursorArea;
	sShared->cursor_physical = (uint32)entry.address;
	sShared->framebuffer_physical = stolenBase;
	sShared->framebuffer_size = stride * height;
	sShared->bytes_per_row = stride;
	sShared->stolen_size = stolenSize;
	sShared->sprite_offset = (stride * height + B_PAGE_SIZE - 1)
		& ~(B_PAGE_SIZE - 1);
	sShared->sprite_size = stolenSize > sShared->sprite_offset
		? stolenSize - sShared->sprite_offset : 0;
	read_current_mode(&sShared->current_mode);

	TRACE("%ux%u, stride %u, 스톨른 %08x (%u KB), 커서 %08x, "
		"스프라이트 %u KB\n",
		(unsigned)sShared->current_mode.virtual_width,
		(unsigned)sShared->current_mode.virtual_height,
		(unsigned)stride, (unsigned)stolenBase,
		(unsigned)(stolenSize / 1024),
		(unsigned)sShared->cursor_physical,
		(unsigned)(sShared->sprite_size / 1024));
	return B_OK;

error:
	if (sSharedArea >= B_OK) delete_area(sSharedArea);
	if (sCursorArea >= B_OK) delete_area(sCursorArea);
	if (sFramebufferArea >= B_OK) delete_area(sFramebufferArea);
	if (sRegistersArea >= B_OK) delete_area(sRegistersArea);
	sSharedArea = sCursorArea = sFramebufferArea = sRegistersArea = -1;
	put_module(B_PCI_MODULE_NAME);
	sPCI = NULL;
	sHasDevice = false;
	return B_ERROR;
}


void
uninit_driver(void)
{
	if (sSharedArea >= B_OK) delete_area(sSharedArea);
	if (sCursorArea >= B_OK) delete_area(sCursorArea);
	if (sFramebufferArea >= B_OK) delete_area(sFramebufferArea);
	if (sRegistersArea >= B_OK) delete_area(sRegistersArea);
	if (sPCI != NULL)
		put_module(B_PCI_MODULE_NAME);
}


const char**
publish_devices(void)
{
	return sHasDevice ? sDeviceNames : NULL;
}


static status_t
device_open(const char* name, uint32 flags, void** cookie)
{
	*cookie = NULL;
	return B_OK;
}


static status_t
device_close(void* cookie)
{
	return B_OK;
}


static status_t
device_free(void* cookie)
{
	return B_OK;
}


static status_t
device_read(void* cookie, off_t position, void* buffer, size_t* length)
{
	*length = 0;
	return B_NOT_ALLOWED;
}


static status_t
device_write(void* cookie, off_t position, const void* buffer, size_t* length)
{
	*length = 0;
	return B_NOT_ALLOWED;
}


static status_t
device_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	switch (op) {
		case B_GET_ACCELERANT_SIGNATURE:
			strcpy((char*)buffer, POULSBO_ACCELERANT_NAME);
			return B_OK;

		case B_GET_PATH_FOR_DEVICE:
			strcpy((char*)buffer, "/dev/graphics/poulsbo");
			return B_OK;

		case POULSBO_GET_PRIVATE_DATA:
		{
			poulsbo_private_data* data = (poulsbo_private_data*)buffer;
			if (data->magic != POULSBO_PRIVATE_MAGIC)
				return B_BAD_VALUE;
			data->shared_info_area = sSharedArea;
			return B_OK;
		}
	}
	return B_DEV_INVALID_IOCTL;
}


static device_hooks sDeviceHooks = {
	device_open,
	device_close,
	device_free,
	device_control,
	device_read,
	device_write,
	NULL, NULL, NULL, NULL
};


device_hooks*
find_device(const char* name)
{
	return &sDeviceHooks;
}
