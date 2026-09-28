/* 하드웨어가 읽고 있는 커서 버퍼 64x64 ARGB 를 그대로 파일로 뽑는다.
 * app_server 가 무엇을 써 넣었는지, 그것이 온전한지 눈이 아니라 데이터로 본다. */
#include <OS.h>
#include <graphic_driver.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "poulsbo.h"

int
main(int argc, char** argv)
{
	poulsbo_private_data data;
	poulsbo_shared_info* shared;
	area_id sharedClone, cursorClone;
	uint8* cursor;
	FILE* out;
	int fd = open("/dev/graphics/poulsbo", O_RDWR);

	if (fd < 0)
		return 1;
	data.magic = POULSBO_PRIVATE_MAGIC;
	if (ioctl(fd, POULSBO_GET_PRIVATE_DATA, &data, sizeof(data)) != 0)
		return 1;
	sharedClone = clone_area("dump shared", (void**)&shared, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, data.shared_info_area);
	if (sharedClone < 0)
		return 1;
	cursorClone = clone_area("dump cursor", (void**)&cursor, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, shared->cursor_area);
	if (cursorClone < 0)
		return 1;

	out = fopen(argc > 1 ? argv[1] : "/tmp/cursor.raw", "wb");
	fwrite(cursor, 1, POULSBO_CURSOR_BYTES, out);
	fclose(out);
	printf("커서 버퍼 %d 바이트 기록. 물리 %08x\n", POULSBO_CURSOR_BYTES,
		(unsigned)shared->cursor_physical);
	return 0;
}
