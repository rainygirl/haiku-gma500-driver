# GMA500 driver - development notes

## 이 칩이 왜 곤란한가

`8086:8108` 은 이름만 "Intel GMA500" 이다. 디스플레이 파이프라인은 Intel 이
만들었고 i915 계열 레지스터가 그대로 통하지만, **그리기 엔진은 Imagination
의 PowerVR SGX535** 다. 문서가 공개돼 있지 않고, 리눅스의 `gma500` 드라이버도
2D/3D 를 쓰지 않는다(예전 staging 시절 `psb_2d.c` 가 있었으나 제거됐다).

그래서 이 드라이버는 **디스플레이 쪽 절반만** 쓴다. 모드는 BIOS 가 세운 것을
그대로 두고, 하드웨어 커서 평면을 붙인다. 그것만으로도 이 기기에서는 의미가
있다 - VESA 드라이버에는 커서 훅이 없어서 app_server 가 포인터가 움직일 때마다
그 아래를 저장·복원·합성한다. 1.33 GHz Atom 에서는 그 비용이 눈에 보인다.

## 커널 코드를 쓰기 전에 알아낸 것

`/dev/misc/poke` 로 유저랜드에서 PCI 설정과 MMIO 를 직접 읽었다. 커널에
아무것도 올리지 않으므로 레지스터를 잘못 건드려도 되돌리기 쉽다.
(`tools/probe.c`)

    PCI 00:02.0  8086:8108 rev 06
    BAR0 94200000   512 KB  MMIO (디스플레이 + SGX)
    BAR1 00006070     8 B   I/O
    BAR2 80000000   256 MB  GTT 창
    BAR3 94280000   256 KB  GTT 페이지 테이블 (65536 칸)

파이프 A 는 꺼져 있고 640x480 VGA 값이 남아 있다. 패널은 **파이프 B** 에
붙어 있다:

    PIPEBCONF  80000000   활성
    PIPEBSRC   063f02ff   1600x768
    DSPBCNTR   99000000   평면 활성, 32bpp xRGB
    DSPBSTRIDE 00001900   6400 바이트
    DSPBSURF   00000000   GTT 0번 칸
    CURBCNTR/BASE/POS     전부 0 - 커서가 한 번도 쓰인 적 없다

GTT 0번 칸은 `7f800001` 이다. 프레임버퍼는 스톨른 메모리 `0x7f800000` 에
있고, 유효 비트가 1이다. 쓰이지 않는 칸은 전부 `7ffbe001` - 스크래치 페이지
하나를 가리킨다.

### 주소 계산을 눈이 아니라 스크린샷으로 검증했다

하드웨어 커서는 스캔아웃 단계에서 합성되므로 `screenshot` 에 찍히지 않는다.
그래서 먼저 **프레임버퍼에 직접 그려서** 계산이 맞는지 확인했다
(`tools/fbtest.c`): 스톨른 `0x7f800000` 에 체크무늬를 그리고 스크린샷을 찍으니
그대로 나왔다. 물리 주소·stride·픽셀 형식·poke 매핑이 모두 맞다는 뜻이고,
이후 커서가 안 보이면 원인은 커서 평면 쪽으로 좁혀진다.

## 커서: 두 번 틀리고 세 번째에 켜졌다

1차 시도는 아무것도 그리지 못했다. 리눅스 `gma500` 원본을 읽고 두 군데가
틀렸음을 확인했다.

| 틀린 것 | 실제 |
| --- | --- |
| 커서 베이스에 GTT 오프셋을 씀 | `psb_device.c` 의 `.cursor_needs_phys = 1` - **물리 주소**를 받는다 |
| 파이프 선택 비트 누락 | `gma_display.c`: `temp \|= (pipe << 28)` - 파이프 B 는 비트 28 |

고친 뒤 실기기 화면에서 확인된 조합:

    CURBCNTR = (1 << 28) | (1 << 26) | 0x27
               파이프 B  | 감마      | 64x64 ARGB
    CURBBASE = 물리 주소 (16 KB 정렬, 물리적으로 연속)
    CURBPOS  = (y << 16) | x, 음수는 각 필드의 비트 15 로 부호

### 커서 버퍼는 스톨른 메모리가 아니어도 된다

리눅스는 `cursor_needs_phys` 인 칩에서 커서를 **스톨른 메모리**에 잡는다
(`psb_intel_display.c`: "Allocate 4 pages of stolen mem for a hardware
cursor"). 그래서 시스템 RAM 은 안 될 것이라고 의심했지만, A/B 로 나란히
켜 보니(`tools/cursor_ab.c`) **둘 다 보였다** - 빨강(시스템 RAM)과
파랑(스톨른)이 모두 화면을 가로질렀다.

필요한 조건은 "스톨른"이 아니라 **물리적으로 연속인 16 KB** 다. 리눅스가
스톨른을 쓰는 이유는 그쪽이 연속을 보장하기 때문으로 보인다. 드라이버는
`create_area(..., B_CONTIGUOUS, ...)` 로 잡는다 - 커널은 연속 영역을 요청할
수 있다. (유저랜드 실험에서는 큰 영역을 잠그고 연속 4페이지를 찾아 썼다.)

## 드라이버 구조

    src/poulsbo.h      공유 구조체와 레지스터 정의
    src/driver.c       커널 드라이버 - 매핑과 정보 제공만
    src/accelerant.c   app_server 가 쓰는 훅

드라이버는 모드를 세우지 않는다. 파이프 B 의 타이밍 레지스터를 읽어
`display_mode` 로 옮기고, 스톨른 프레임버퍼와 MMIO 를 영역으로 매핑하고,
커서용 연속 16 KB 를 잡아 공유 구조체에 담는다. accelerant 는 그 영역들을
`clone_area` 로 가져가 쓴다.

`propose_display_mode`/`set_display_mode` 는 지금 모드만 받는다. 이 기기의
패널은 1600x768 고정이고, 모드 설정을 잘못하면 화면이 죽는다. 모드 설정은
이 드라이버의 범위가 아니다.

### app_server 가 vesa 대신 이 드라이버를 고르는 이유

`AccelerantHWInterface::_OpenGraphicsDevice()` 는 `/dev/graphics/` 를 먼저
훑고, **아무것도 못 찾았을 때만** `vesa` 와 `framebuffer` 를 연다. 그래서
`/dev/graphics/poulsbo` 가 publish 되면 그쪽이 선택된다. accelerant 로딩이
실패하면 다음 장치로 넘어가므로, 드라이버만 설치하고 accelerant 를 빼면
자동으로 vesa 로 되돌아간다 - 단계별로 시험하기 좋은 성질이다.

### 커널 모듈에서 걸린 것

- `B_MTR_WC` 는 이 버전의 공개 헤더에 없다. 스톨른 메모리는 보통 RAM 이라
  기본(write-back) 매핑이 오히려 빠르므로 그냥 뺐다. write-combining 은
  PCIe 너머의 진짜 VRAM 에서 의미가 있다.
- 커널 애드온에는 libgcc 가 없다. 64비트 나눗셈(`__udivdi3`)을 쓰면 링크가
  깨진다. 픽셀 클럭 계산을 32비트로 바꿨다 (1794 × 778 × 60 = 8.4e7 로
  넘치지 않는다).
- 직접 링크하지 말고 `/boot/system/develop/etc/makefile-engine` 에
  `TYPE = DRIVER` 로 맡기는 편이 낫다. 커널 라이브러리와 링크 옵션을 알아서
  붙인다.

## 드라이버를 붙이면서 걸린 두 가지

### 커널이 만든 영역은 그냥 clone 되지 않는다

accelerant 가 `clone_area` 로 공유 구조체를 가져가려 하면 `B_NOT_ALLOWED` 로
막힌다. 커널이 만든 영역을 다른 팀이 복제하려면 **`B_CLONEABLE_AREA`** 가
보호 플래그에 들어 있어야 한다. 공유 구조체·레지스터·프레임버퍼·커서 네
영역 모두에 필요하다.

### 프레임버퍼는 write-combining 이어야 한다

처음에는 스톨른 메모리가 보통 RAM 이니 기본(write-back) 매핑이 빠를 것이라고
보고 캐시 설정을 넣지 않았다. **틀렸다.** 실기기에서 창을 끌면 잔상이 남았다가
잠시 뒤 사라졌다 - 스캔아웃 엔진은 CPU 캐시를 들여다보지 않으므로, app_server
가 그린 픽셀이 캐시에 머무는 동안 화면에 나타나지 않는다. 캐시가 밀려날 때
뒤늦게 반영되는 것이 그 잔상이다.

Haiku 의 vesa 드라이버가 같은 함수로 이미 해결해 두고 있었다:

    vm_set_area_memory_type(area, base, B_WRITE_COMBINING_MEMORY);

`B_WRITE_COMBINING_MEMORY` 는 공개 헤더(KernelExport.h)에 있고,
`vm_set_area_memory_type` 은 비공개지만 커널이 심볼을 내보낸다. 이 한 줄을
넣자 잔상이 사라졌다.

### 감마 비트를 켜면 커서 색이 깨진다

리눅스 `gma_display.c` 를 그대로 따라 `MCURSOR_GAMMA_ENABLE` 을 켰더니,
실기기에서 손 모양 포인터의 **검은 테두리가 사라지고 흰색과 섞여** 보였다.

원인을 데이터에서 먼저 배제했다. 하드웨어가 읽고 있는 커서 버퍼를 그대로 꺼내
보니(`tools/dumpcursor.c`) 그림은 완벽했다 - 흰 속(rgb 252~254)과 검은 테두리가
정확히 들어 있고, 반투명 픽셀 134개는 전부 검정(rgb=0)인 안티앨리어싱이었다.
즉 app_server 가 써 넣은 것에는 문제가 없고, 하드웨어가 그것을 해석하는 방식이
문제였다.

감마를 켜면 커서 픽셀이 파이프의 팔레트 LUT 를 통과한다. 그런데 이 드라이버는
BIOS 가 세운 모드를 그대로 쓰므로 **LUT 를 프로그래밍한 적이 없다**. 디스플레이
평면은 감마를 끄고 있어서(`DSPBCNTR` 비트 30 = 0) 영향이 없지만, 커서만 켜 두면
초기화되지 않은 LUT 를 지나며 색이 뒤집힌다. 리눅스는 감마를 켜는 대신 LUT 도
함께 채운다 - 앞부분만 따라 한 것이 잘못이었다.

`tools/gammatoggle.c` 로 살아 있는 레지스터의 비트 26 만 끄자 그 자리에서
테두리가 돌아왔다. app_server 도 재시작도 필요 없는 실험이었다.

    CURBCNTR 14000027 -> 10000027

## 2D 블리터: 엔진은 살아 있었다

커서가 붙은 뒤 SGX 쪽을 읽기만으로 찔러 봤다(`tools/sgxprobe.c`). 죽어 있을
거라고 봤는데 `CR_CORE_ID` 가 `0x0113` 을 돌려줬다. 클럭도 전원도 이미 켜져
있다. 부팅 펌웨어가 켜 놓은 것을 아무도 끄지 않았을 뿐이다.

명령은 BAR0+0x40000 의 SGX 레지스터 영역에서 +0x4000 슬레이브 포트로 32 비트씩
밀어 넣는다. `CR_2D_SOCIF` 가 FIFO 의 빈 자리를 알려주고(0x80 이면 비어 있음),
`CR_2D_BLIT_STATUS` 의 비트 24 가 BUSY, 하위 24 비트가 완료한 블릿 수다.

### 리눅스와 갈리는 지점은 주소 기준 하나였다

명령 형식은 리눅스 staging 시절의 `psb_2d.c` 를 그대로 따랐는데 처음에는
아무것도 그려지지 않았다. 엔진은 명령을 소비했고 완료 카운터도 올라갔다.
메모리만 그대로였다.

원인은 `CR_BIF_TWOD_REQ_BASE` 다. `psb_2d.c` 는 여기에 GTT 창의 주소를 넣는다.
이 기기에서는 프레임버퍼의 **물리 주소**(`0x7f800000`)를 넣어야 그린다.

그리고 참고한 그 코드는 상류에서 한 번도 켜진 적이 없다. `psbfb_fillrect` 는

    if (1 || !dev_priv->ops->accel_2d || ...)
        return cfb_fillrect(info, region);

이렇게 `if (1 ||` 로 막힌 채 커밋됐다. 명령 형식의 출처로는 쓸 만하지만,
누가 검증해 둔 코드는 아니라고 보는 편이 맞다.

### 정작 app_server 가 이 훅을 부르지 않는다

fill/blit/invert 를 다 붙이고 accelerant 를 올린 뒤, 창을 여덟 번 옮기고
`CR_2D_BLIT_STATUS` 를 읽었다. 0 이었다. 한 번도 불리지 않았다는 뜻이다.

app_server 소스를 보면 이유가 분명하다. `AccelerantHWInterface` 에는
`B_SCREEN_TO_SCREEN_BLIT` 도 `B_FILL_RECTANGLE` 도 `B_ACQUIRE_ENGINE` 도
나오지 않는다. 이 훅들이 남아 있는 곳은 `DWindowHWInterface`, 즉 app_server 를
창 안에서 돌리는 시험용 하니스뿐이다.

여기서 한 번 틀렸다. 남은 경로가 `DrawingEngine::CopyRegion` -> `CopyRect()` ->
CPU `memcpy` 이길래, 프레임버퍼 안에서의 복사를 CPU 와 2D 엔진으로 나란히 재고
(`tools/bench2d.c`, 800x500 구역 20 회) 12.9 배 차이를 근거로 app_server 를 고쳤다.

    프레임버퍼 안 CPU memcpy   한 번 52.8 ms
    SGX 2D 블릿                한 번  4.1 ms

`HWInterface::AcceleratedCopyRect()` 를 기본값 false 인 가상 함수로 두고,
`AccelerantHWInterface` 가 블릿 훅으로 구현하고, `DrawingEngine::CopyRect` 가
CPU 복사 전에 먼저 시도하게 했다. 빌드해서 실기기에 올렸다. 완료 카운터는
그대로 0 이었다.

### 이유: app_server 는 프레임버퍼 안에서 복사하지 않는다

`AccelerantHWInterface::SetMode()` 는 **언제나** `MallocBuffer` 백버퍼를 만든다.
조건이

    if (!fBackBuffer.IsSet() || ... )

라 첫 호출에 참이고, 이를 해제하는 곳이 어디에도 없다. 업스트림 Haiku 도 같다.
그러니 app_server 는 모든 그리기를 주 메모리에서 끝내고 더러워진 사각형만
화면으로 밀어 넣는다. 프레임버퍼 안에서의 복사 - 블릿 훅이 대신할 수 있는 유일한
동작 - 를 아예 하지 않는다. 내가 잰 52.8 ms 는 app_server 가 하지 않는 일이었다.

실제로 하는 일을 다시 쟀다(`tools/bench3.c`, 같은 구역):

    백버퍼 안 복사 (CopyRect)         한 번 2.4 ms
    백버퍼 -> 화면 (_CopyBackToFront)  한 번 1.6 ms
    합계                              한 번 4.0 ms

2D 엔진의 4.1 ms 와 같거나 그보다 빠르다. 캐시가 도는 메모리에서 하기 때문이다.
`_CopyBackToFront` 를 엔진에 맡길 수도 없다. 출발지가 malloc 메모리라 엔진이
읽을 주소가 아니다. 백버퍼를 스톨른 메모리로 옮기면 주소는 풀리지만 Painter 의
블렌딩 읽기와 캐시 일관성이 새 문제가 된다.

그래서 app_server 수정은 패치에서 다시 뺐다. 훅 자체는 남긴다. 구현이 올바르고,
비용이 없고, accelerant 를 직접 부르는 쪽에서는 쓸 수 있다.

## 오버레이 평면은 이 칩에 없다

커서와 2D 다음으로 오버레이를 노렸다. app_server 가 2D 훅과 달리 오버레이
훅은 실제로 호출하기 때문이다(`BitmapManager.cpp` 의 `AcquireOverlayChannel`).
YUV 변환과 확대를 디스플레이 엔진이 맡으면 영상 재생에서 CPU 가 크게 빈다.

읽기만 하는 프로브로 MMIO 0x30000 을 보니 살아 있어 보였다. `DOVSTA` 가
`0x80084000`, `OGAMC0~5` 에 하드웨어 기본 감마 램프(0x08/0x10/0x20/0x40/
0x80/0xc0). 물리 주소를 `OVADD` 에 넣자 레지스터 버퍼가 실제로 적재됐다 -
섀도 레지스터 0x30100 부터가 우리가 쓴 창 위치·크기·버퍼 주소·OCOMD·OCONFIG
를 그대로 되비쳤다. i915 는 `MI_OVERLAY_FLIP` 링 명령으로 갱신을 거는데
Poulsbo 에는 그 커맨드 스트리머가 없다(그리기 엔진이 PowerVR 이다). 레지스터
쓰기만으로 걸린다는 것까지 확인했다.

그런데 켜면 매번 디스플레이 FIFO 가 굶었다. 화면이 옆으로 흐르며 떨리다
꺼지고, `PIPEBSTAT` 의 비트 31(언더런)이 서서 내려오지 않는다. 평면 재시작,
파이프 재시작, 패널 전원 순환을 다 만들어 봤지만 어느 것도 되살리지 못했다 -
리셋 말고는 방법이 없었다. 다섯 가지를 바꿔 가며 시험했고 결과는 모두 같았다:

    OVADD 를 GTT 오프셋 / 물리 주소       물리 주소여야 적재된다
    버퍼를 GTT / 물리 / 프레임버퍼 자체    전부 같은 언더런
    slot_time 0x80 / 0                    같음
    DSPARB, DSPFW1 워터마크                같음
    폴리페이즈 필터 계수 비움 / 채움       같음

답은 자료에 있었다. Intel SCH US15W 데이터시트(문서 319537) 9.3.1 은 평면을
**Display / Cursor / VGA 셋만** 열거하고, 105 쪽 전체에 overlay 라는 말이 한
번도 나오지 않는다. 이 칩에는 오버레이 평면이 없다. 0x30000 의 레지스터
블록은 i915 에서 물려받은 껍데기이고, 켜면 어디에도 연결되지 않은 요청이
디스플레이 메모리 경로를 막는다.

추측으로 좁히기 전에 데이터시트를 먼저 읽었어야 했다. 재부팅 다섯 번을 썼다.

## 대신 스프라이트 평면이 있다

같은 데이터시트가 보장하는 것이 있다. "The secondary display plane can be
used ... as a sprite plane on either the primary or secondary display."

평면 C(0x72180~)는 1 차 평면과 같은 레지스터 배치에 위치·크기·컬러키를
더 가지며 `DISPPLANE_SEL_PIPE_B` 로 파이프 B 에 붙는다. 시험 첫 판에 언더런
없이 깨끗하게 켜지고 꺼졌다. 다만 화면에는 아무것도 없었는데, 원인은 Z 순서
비트였다.

    비트 0~2 = 1 (오버레이 위)    1 차 평면 아래에 깔려 보이지 않는다
    비트 0~2 = 0 (디스플레이 A 위) 1 차 평면 위로 올라온다

0 으로 바꾸자 색 띠가 그대로 떴다. 주소 레지스터는 `DSPCSURF` 가 아니라
`DSPCLINOFF` 다 - SURF 에 쓴 값은 되읽히지 않는다(4 세대부터의 레지스터다).

제약이 둘 있다. 색 공간은 RGB 뿐이다. 디스플레이 평면의 픽셀 포맷 필드에
YUV 값이 정의되어 있지 않다 - `psb_intel_reg.h` 도 i915 도 8BPP / 15·16 /
16 / 32-no-alpha / 32 뿐이다. 그리고 확대·축소가 없다. YUV 변환과 스케일링은
오버레이 유닛의 일이었고 그 유닛이 없다.

accelerant 는 이 평면으로 오버레이 훅을 구현한다. 드라이버가 프레임버퍼 뒤에
남는 스톨른 메모리(이 기기에서 3132 KB)를 잡아 두고 `sprite_offset` /
`sprite_size` 로 알려준다.

### 그런데 Haiku 의 오버레이 경로는 앱 경계에서 끊겨 있다

`SetViewOverlay` 로 시험하면 훅은 정확히 호출된다 - 우리가 정한
`bytes_per_row` 가 앱까지 도달한다. 그러나 `BBitmap::Bits()` 에 쓰면 앱이
죽는다.

`Overlay::SetClientData` 가 `fClientData->buffer = fOverlayBuffer->buffer` 로
**app_server 주소공간의 포인터**를 그대로 공유 메모리에 넣고, 클라이언트는
`ServerMemoryAllocator::AddArea` 로 영역을 `B_ANY_ADDRESS` 에 매핑한다. 두
주소가 같을 리 없다. 실기기에서 `Bits()` 가 `0x45768000` 을 돌려줬고
`area_for()` 는 -1 이었다.

드라이버로 고칠 수 있는 문제가 아니다. 버퍼의 `area_id` 와 오프셋을 앱에
넘기고 앱이 그 영역을 clone 하도록 `overlay_client_data` 와 libbe 를 함께
고쳐야 한다. 훅은 올바르고 비용이 없으므로 그대로 둔다.

## 하드웨어 커서를 켜면 드래그 비트맵이 사라진다

바탕화면 아이콘을 끌면 포인터만 보이고 아이콘이 보이지 않았다. 드라이버가
아니라 app_server 의 빈틈이다.

`HWInterface::SetDragBitmap()` 은 드래그 비트맵과 커서를
`CursorAndDragBitmap()` 으로 합성한다. 그런데 그 비트맵은 소프트웨어 커서
(`_DrawCursor()`)로만 화면에 나가고, `AccelerantHWInterface::_DrawCursor()` 는
하드웨어 커서가 켜져 있으면 아무것도 하지 않는다. `AccelerantHWInterface` 는
`SetDragBitmap()` 을 재정의하지 않으므로 하드웨어 커서에는 맨 포인터만 남는다.
accelerant 는 드래그가 시작됐다는 사실조차 받지 못한다 - 커서 훅이 있는 모든
accelerant 에서 업스트림도 같다.

고친 곳은 VAIO P 패치(`haiku-sony-vaio-p-patch` 의 `vaio-p-patches.diff`)다.
드래그 비트맵이 걸리면 소프트웨어 커서로 바꾸고, 풀리면 하드웨어 커서로
되돌린다. 드래그 중 `SetCursor()` 도 소프트웨어에 머문다. 패치하지 않은
시스템에 이 드라이버만 설치하면 증상은 그대로다.

합성 비트맵을 커서 평면에 올리는 방법은 쓰지 않았다. 아이콘과 이름을 담은
드래그 비트맵은 대개 64x64 보다 넓다.

이미지를 다시 굽지 않고 실기기에서 확인했다. 기기에서 app_server 만 네이티브로
빌드하고(`jam -q app_server`), launch_daemon 설정으로 패키지판 대신 띄웠다:

    ~/config/settings/launch/dragtest-app_server
    service x-vnd.Haiku-app_server {
        launch /boot/home/dragtest/app_server
    }

같은 이름의 잡을 덮어쓰므로 `launch` 경로만 바뀐다. 파일을 지우고 재부팅하면
원래대로 돌아간다. 아이콘을 끌면 아이콘이 보이고, 소프트웨어 커서로 넘어갔다가
놓으면 하드웨어 커서로 돌아온다.

시험하며 걸린 것 둘:

- ssh 세션에서는 `LIBRARY_PATH` 가 비어 있다. 빌드가 `:<경로>` 를 붙이면 기본
  검색 경로가 통째로 사라져 `package extract` 가 메시지 없이 3 으로 끝난다.
  빌드 전에 `. /boot/system/boot/SetupEnvironment` 를 읽어야 한다.
- app_server 를 손으로 재시작하면 Tracker 는 죽은 인스턴스에 붙은 채 남아
  바탕화면이 까매진다. Tracker 도 재시작해야 한다.

## 실기기에서 확인된 것

Sony VAIO P (VGN-P70H), Haiku R1~beta6+development hrev99002, x86_gcc2:

    /dev/graphics/poulsbo          드라이버가 publish
    app_server 가 로드한 accelerant  poulsbo.accelerant (vesa 아님)
    CURBCNTR  10000027             app_server 가 켠 하드웨어 커서 (감마 없음)
    CURBBASE  00bf3000             드라이버가 잡은 물리 연속 16 KB
    CURBPOS   017e031e             마우스 위치가 그대로 반영

바탕화면·Deskbar·Tracker 모두 정상, 창 드래그 시 잔상 없음.

2D 엔진:

    CR_CORE_ID             00000113   SGX535 이 응답
    CR_2D_SOCIF            00000080   FIFO 비어 있음
    CR_BIF_TWOD_REQ_BASE   7f800000   프레임버퍼 물리 주소
    CR_2D_BLIT_STATUS      완료 카운터가 명령마다 증가

`tools/fill2d.c`, `tools/blit2d.c` 로 채우기·복사·겹치는 복사를 보내고,
프레임버퍼 메모리를 직접 읽어 픽셀 값으로 확인했다.

## 앞으로

- **Haiku 의 오버레이 경로 수정.** 스프라이트 평면은 동작하는데 app_server 가
  픽셀 버퍼를 앱에 전달하지 못한다. `overlay_client_data` 에 `area_id` 와
  오프셋을 더하고 `BBitmap::Bits()` 가 그 영역을 clone 하게 하면 된다.
  app_server 와 libbe 를 함께 고쳐야 하므로 이미지 전체를 다시 빌드해야 한다.
- **채우기·반전 가속.** 훅은 있으나 app_server 가 부르는 곳이 없다. 패치 쪽에
  `FillRegion`/`InvertRegion` 경로까지 되살릴지는 회귀 위험을 보고 판단한다.
  복사에 비하면 이득도 작다.
- **모드 설정.** 패널이 고정이라 실익이 적고, 위험은 크다.
- **DPMS.** 지금은 `B_DPMS_ON` 만 보고한다. 파이프/패널 전원 순서를 지켜야
  하므로 별도 작업이다.
