/* 오버레이 경로를 실제 BeAPI 로 두드려 본다.
 *
 * B_BITMAP_WILL_OVERLAY 로 비트맵을 만들고 SetViewOverlay 로 붙이면
 * app_server 가 accelerant 의 오버레이 훅을 호출한다. 이 드라이버에서는
 * 그 훅이 스프라이트 평면(평면 C)으로 내려간다.
 */
#include <Application.h>
#include <Bitmap.h>
#include <View.h>
#include <Window.h>

#include <stdio.h>
#include <string.h>

class OverlayView : public BView {
public:
	OverlayView(BRect frame)
		:
		BView(frame, "overlay", B_FOLLOW_ALL, B_WILL_DRAW),
		fBitmap(NULL)
	{
	}

	virtual void AttachedToWindow()
	{
		BRect bounds(0, 0, 319, 239);
		fBitmap = new BBitmap(bounds,
			B_BITMAP_WILL_OVERLAY | B_BITMAP_RESERVE_OVERLAY_CHANNEL,
			B_RGB32);
		if (fBitmap->InitCheck() != B_OK) {
			printf("오버레이 비트맵을 못 만들었다: %s\n",
				strerror(fBitmap->InitCheck()));
			delete fBitmap;
			fBitmap = NULL;
			SetViewColor(255, 0, 0);
			return;
		}
		printf("오버레이 비트맵 확보. bytes_per_row %ld, area %ld\n",
			fBitmap->BytesPerRow(), (long)fBitmap->Area());
		printf("Bits() = %p, BitsLength = %ld\n", fBitmap->Bits(),
			fBitmap->BitsLength());
		fflush(stdout);

		// 그 포인터가 이 주소공간에서 유효한지 먼저 본다
		{
			area_info info;
			area_id id = area_for(fBitmap->Bits());
			printf("area_for(Bits()) = %ld\n", (long)id);
			if (id >= B_OK && get_area_info(id, &info) == B_OK) {
				printf("  영역 '%s' 주소 %p 크기 %ld 보호 %lx\n",
					info.name, info.address, (long)info.size,
					(unsigned long)info.protection);
			} else {
				printf("  이 주소공간에 없는 포인터다 - 쓰면 죽는다\n");
				fflush(stdout);
				SetViewColor(255, 128, 0);
				return;
			}
		}
		fflush(stdout);

		// 색 띠를 채운다
		uint8* bits = (uint8*)fBitmap->Bits();
		int32 stride = fBitmap->BytesPerRow();
		for (int y = 0; y < 240; y++) {
			uint32* row = (uint32*)(bits + y * stride);
			for (int x = 0; x < 320; x++) {
				if (x < 80) row[x] = 0x00ff0000;
				else if (x < 160) row[x] = 0x0000ff00;
				else if (x < 240) row[x] = 0x000000ff;
				else row[x] = 0x00ffffff;
			}
		}

		rgb_color key;
		SetViewOverlay(fBitmap, fBitmap->Bounds(), Bounds(), &key,
			B_FOLLOW_ALL);
		SetViewColor(key);
		SetLowColor(key);
		printf("SetViewOverlay 했다. 키 색 %d,%d,%d\n", key.red, key.green,
			key.blue);
	}

	virtual void Draw(BRect)
	{
		if (fBitmap == NULL) {
			SetHighColor(255, 255, 0);
			FillRect(Bounds());
		}
	}

private:
	BBitmap* fBitmap;
};


int
main()
{
	BApplication app("application/x-vnd.rainygirl-ovapp");
	BWindow* window = new BWindow(BRect(100, 100, 419, 339),
		"오버레이 시험", B_TITLED_WINDOW, B_QUIT_ON_WINDOW_CLOSE);
	window->AddChild(new OverlayView(window->Bounds()));
	window->Show();
	app.Run();
	return 0;
}
