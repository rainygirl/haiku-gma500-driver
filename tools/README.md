# 조사용 도구

드라이버를 쓰기 전에 커널 코드 없이 하드웨어를 확인하는 데 쓴 프로그램들이다.
모두 `/dev/misc/poke` 를 통해 유저랜드에서 PCI 설정과 MMIO 를 읽고 쓴다.
커널에 아무것도 올리지 않으므로, 레지스터 하나를 잘못 건드려도 되돌리기 쉽다.

| 파일 | 하는 일 |
| --- | --- |
| `probe.c` | PCI BAR, 파이프 A/B 타이밍, 디스플레이 평면, 커서, 패널, GTT 덤프 |
| `fbtest.c` | 스톨른 프레임버퍼에 직접 그린다. 스크린샷으로 주소 계산을 검증 |
| `cursor_test.c` | 하드웨어 커서 점등 (1차: GTT 오프셋 - 실패) |
| `cursor_ab.c` | 커서 버퍼를 시스템 RAM/스톨른에 각각 두고 비교 (둘 다 동작) |
| `dumpcursor.c` | 커서 버퍼를 파일로 덤프. 색 깨짐이 데이터 탓인지 가르는 데 씀 |
| `gammatoggle.c` | `CURBCNTR` 의 감마 비트(26)를 살아 있는 상태에서 껐다 켠다 |
| `checkdriver.c` | 드라이버/accelerant 가 실제로 무엇을 물고 있는지 확인 |
| `sgxprobe.c` | SGX 2D 엔진 레지스터를 읽기만 해서 살아 있는지 본다 |
| `fill2d.c` | 2D 엔진으로 사각형 채우기를 한 번 보낸다 |
| `fill2d_ab.c` | 2D 주소 기준(GTT 창 / 프레임버퍼 물리)을 나란히 시험 |
| `blit2d.c` | 화면 간 복사와 겹치는 복사를 보내고 픽셀로 확인 |
| `bench2d.c` | 프레임버퍼 안 복사를 CPU 와 2D 엔진으로 각각 재서 배수를 낸다 |
| `bench3.c` | app_server 가 실제로 하는 복사(백버퍼 안, 백버퍼->화면)를 잰다 |
| `setbase.c` | `CR_BIF_TWOD_REQ_BASE` 를 프레임버퍼 물리 주소로 되돌린다 |
| `gttprobe.c` | 스톨른 메모리가 어디까지인지, 프레임버퍼 뒤에 얼마가 남는지 |
| `ovprobe.c` | 오버레이 레지스터와 스프라이트 평면을 읽기만 해서 본다 |
| `ovtest.c` | 오버레이를 켜 보는 시험. 주소 방식·slot_time·워터마크·계수를 인자로 바꾼다 |
| `spritetest.c` | 평면 C 를 스프라이트로 띄운다. 평면·Z순서·픽셀 포맷을 인자로 바꾼다 |
| `dispkick.c` | 평면(또는 파이프)을 껐다 켜서 멈춘 화면을 되살려 본다 |
| `panelpower.c` | LVDS 패널 전원을 껐다 켠다. DPMS 구현의 바탕이다 |
| `ovapp.cpp` | BeAPI 쪽에서 오버레이 비트맵을 만들어 훅 경로를 두드린다 |

빌드:

```sh
gcc -o probe probe.c -I/boot/system/develop/headers/private/drivers
```

`fill2d`, `blit2d`, `bench2d` 는 자기 명령 버퍼의 물리 주소를
`CR_BIF_TWOD_REQ_BASE` 에 써 넣는다. 끝난 뒤 값이 남아 있으면 accelerant 가
보내는 명령이 엉뚱한 메모리를 읽으므로, `setbase 7f800000` 으로 되돌린다.

`ovapp.cpp` 는 BeAPI 를 쓰므로 `g++ -o ovapp ovapp.cpp -lbe` 로 빌드한다.

오버레이(`ovtest`)를 켜면 디스플레이 FIFO 가 굶어 화면이 죽고, `dispkick` 도
`panelpower` 도 되살리지 못한다. 재부팅해야 한다. 이 칩에 오버레이 평면이
없다는 것을 확인한 경위는 AGENTS.md 에 있다. 스프라이트(`spritetest`)는
안전하다.
