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

빌드:

```sh
gcc -o probe probe.c -I/boot/system/develop/headers/private/drivers
```
