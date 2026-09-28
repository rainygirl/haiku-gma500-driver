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

- **채우기·반전 가속.** 훅은 있으나 app_server 가 부르는 곳이 없다. 패치 쪽에
  `FillRegion`/`InvertRegion` 경로까지 되살릴지는 회귀 위험을 보고 판단한다.
  복사에 비하면 이득도 작다.
- **모드 설정.** 패널이 고정이라 실익이 적고, 위험은 크다.
- **DPMS.** 지금은 `B_DPMS_ON` 만 보고한다. 파이프/패널 전원 순서를 지켜야
  하므로 별도 작업이다.
