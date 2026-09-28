/* 드라이버가 공유 구조체에 채워 넣은 값을 유저랜드에서 확인한다.
 * app_server 를 건드리지 않고 accelerant 가 볼 값을 그대로 본다. */
#include <OS.h>
#include <graphic_driver.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "poulsbo.h"

int
main(void)
{
	poulsbo_private_data data;
	poulsbo_shared_info* shared;
	area_id clone;
	int fd = open("/dev/graphics/poulsbo", O_RDWR);
	char signature[B_FILE_NAME_LENGTH];

	if (fd < 0) {
		printf("장치를 열 수 없다\n");
		return 1;
	}
	if (ioctl(fd, B_GET_ACCELERANT_SIGNATURE, signature, sizeof(signature)) == 0)
		printf("accelerant 이름: %s\n", signature);
	else
		printf("accelerant 이름 조회 실패\n");

	data.magic = POULSBO_PRIVATE_MAGIC;
	if (ioctl(fd, POULSBO_GET_PRIVATE_DATA, &data, sizeof(data)) != 0) {
		printf("공유 영역 조회 실패\n");
		return 1;
	}
	clone = clone_area("poulsbo check", (void**)&shared, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, data.shared_info_area);
	if (clone < 0) {
		printf("공유 영역 clone 실패: %s\n", strerror(clone));
		return 1;
	}

	printf("magic            %.4s\n", (char*)&shared->magic);
	printf("해상도           %u x %u\n",
		(unsigned)shared->current_mode.virtual_width,
		(unsigned)shared->current_mode.virtual_height);
	printf("타이밍           h %u/%u/%u/%u  v %u/%u/%u/%u  %u kHz\n",
		shared->current_mode.timing.h_display,
		shared->current_mode.timing.h_sync_start,
		shared->current_mode.timing.h_sync_end,
		shared->current_mode.timing.h_total,
		shared->current_mode.timing.v_display,
		shared->current_mode.timing.v_sync_start,
		shared->current_mode.timing.v_sync_end,
		shared->current_mode.timing.v_total,
		(unsigned)shared->current_mode.timing.pixel_clock);
	printf("bytes_per_row    %u\n", (unsigned)shared->bytes_per_row);
	printf("프레임버퍼       물리 %08x, %u 바이트, area %ld\n",
		(unsigned)shared->framebuffer_physical,
		(unsigned)shared->framebuffer_size, (long)shared->framebuffer_area);
	printf("커서             물리 %08x, area %ld  (4KB 정렬: %s)\n",
		(unsigned)shared->cursor_physical, (long)shared->cursor_area,
		(shared->cursor_physical & 0xfff) == 0 ? "예 (4KB)" : "아니오");
	printf("레지스터 area    %ld\n", (long)shared->registers_area);

	delete_area(clone);
	close(fd);
	return 0;
}
