# Haiku용 GMA500 드라이버

[English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md)

Sony VAIO P처럼 Intel SCH US15W(Poulsbo, 이른바 "GMA500")를 쓰는 기기를 위한
그래픽 드라이버입니다. 이 칩에서 Haiku가 **하드웨어 커서**를 쓸 수 있게 합니다.
VESA 드라이버로는 되지 않는 일입니다.

이 드라이버가 없으면 app_server가 마우스 포인터를 소프트웨어로 그립니다.
포인터가 움직일 때마다 그 아래 화면을 저장하고, 포인터를 그리고, 원래 픽셀을
되돌립니다. 2D 가속이 없는 1.33GHz Atom에서는 그 일이 눈에 보입니다. 이
드라이버를 쓰면 디스플레이 엔진이 스캔아웃 단계에서 포인터를 합성하므로 그
과정이 통째로 사라집니다.

## pkgman으로 설치

| Haiku | 명령 |
| --- | --- |
| 32비트 x86 (x86_gcc2) | `pkgman add-repo https://pkgman.rainygirl.com/x86_gcc2`<br>`pkgman install gma500` |

**설치한 뒤 재부팅하십시오.** 커널은 그래픽 드라이버를 부팅할 때 읽으므로,
재부팅 전까지는 아무것도 달라지지 않습니다.

`pkgman add-repo`가 `Operation not supported`로 실패하면 그 이미지의 네트워크
킷에 TLS가 없는 것입니다. 주소의 `https://`를 `http://`로 바꿔 쓰십시오.

## 소스에서 설치

Haiku 기기에서:

```sh
make                      # 드라이버와 accelerant를 빌드
make install              # 둘 다 ~/config/non-packaged에 설치
```

그다음 재부팅합니다.

```sh
make uninstall            # 둘 다 제거. 재부팅하면 VESA로 돌아갑니다
```

## 잘 됐는지 확인

재부팅한 뒤:

```sh
ls /dev/graphics/
```

`vesa` 옆에 `poulsbo`가 보여야 합니다. app_server가 실제로 어느 쪽을 골랐는지는
이렇게 확인합니다.

```sh
listimage $(ps | grep [a]pp_server | head -1 | awk '{print $2}') | grep accelerant
```

`poulsbo.accelerant`가 나오면 성공입니다. `vesa.accelerant`가 나오면 드라이버가
선택되지 않은 것입니다 - [AGENTS.md](AGENTS.md)를 참고하십시오.

## VESA로 되돌리기

pkgman으로 설치했다면:

```sh
pkgman uninstall gma500
```

소스에서 설치했다면:

```sh
make uninstall
```

어느 쪽이든 그다음 재부팅하십시오. `/dev/graphics/poulsbo`가 없으면 app_server가
스스로 VESA 드라이버로 돌아가므로, 드라이버가 잘못되어도 화면이 아주 사라지는
일은 생기지 않습니다. 만약 그런 상황이 오면 ssh로 접속해 파일을 지우면 됩니다.

## 2D 가속에 대하여

이 드라이버는 칩에 들어 있는 PowerVR SGX535 2D 엔진을 구동하며, 화면 간 복사와
사각형 채우기·반전 훅을 제공합니다. VAIO P에서 잰 결과 복사는 CPU가 같은 일을
할 때보다 약 13배 빠릅니다.

다만 Haiku의 app_server는 이 훅들을 호출하지 않으며, 이는 드라이버가 고칠 수 있는
문제가 아닙니다. `AccelerantHWInterface`는 언제나 주 메모리에 백버퍼를 잡습니다.
app_server는 각 프레임을 거기서 합성한 뒤 완성된 사각형만 화면으로 밀어 넣으므로,
블릿 훅이 대신할 수 있는 유일한 동작인 프레임버퍼 안에서의 복사를 아예 하지
않습니다. VAIO P에서 재 보면 800x500 창 끌기 한 걸음은 백버퍼 안 복사 2.4 ms에
화면으로 밀어넣기 1.6 ms이고, 같은 복사를 2D 엔진으로 하면 4.1 ms입니다. 캐시가
도는 쪽이 이미 더 빠릅니다.

그래도 훅을 남겨 둔 것은 구현이 올바르고 비용이 들지 않으며, 이 accelerant를 직접
부르는 쪽에서는 쓸 수 있기 때문입니다. 측정값 전체는 [AGENTS.md](AGENTS.md)에
있습니다.

## 이 드라이버가 하지 않는 일

- **3D 가속은 없습니다.** SGX535의 3D 쪽은 공개된 문서가 없습니다.
- **모드 설정을 하지 않습니다.** BIOS가 세운 모드를 그대로 둡니다. 대상 기기는
  어차피 패널 해상도가 고정입니다.
- **DPMS는 "켜짐"만 보고합니다.**

## AI 사용 고지

이 드라이버의 일부는 AI 코딩 도구(Anthropic Claude)의 도움을 받아 개발했습니다.
모든 코드는 작성자가 실기기에서 검토하고 테스트했습니다.

## 라이선스

MIT. 레지스터가 하는 일, 측정한 값, 시간을 가장 많이 쓴 두 가지 실수 같은 개발
기록은 [AGENTS.md](AGENTS.md)에 있습니다.
