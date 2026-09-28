# Haiku용 GMA500 드라이버

[English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md)

Sony VAIO P 처럼 Intel SCH US15W(Poulsbo, 이른바 "GMA500")를 쓰는 기기를 위한
그래픽 드라이버예요. 이 칩에서 Haiku가 **하드웨어 커서**를 쓸 수 있게 해줘요.
VESA 드라이버로는 안 되는 일이에요.

이 드라이버가 없으면 app_server가 마우스 포인터를 소프트웨어로 그려요. 포인터가
움직일 때마다 그 아래 화면을 저장하고, 포인터를 그리고, 원래 픽셀을 되돌려요.
2D 가속이 없는 1.33GHz Atom에서는 그 일이 눈에 보여요. 이 드라이버를 쓰면
디스플레이 엔진이 스캔아웃 단계에서 포인터를 합성하기 때문에 그 과정이 통째로
사라져요.

## pkgman으로 설치

| Haiku | 명령 |
| --- | --- |
| 32비트 x86 (x86_gcc2) | `pkgman add-repo https://pkgman.rainygirl.com/x86_gcc2`<br>`pkgman install gma500` |

**설치 후 재부팅하세요.** 커널은 그래픽 드라이버를 부팅할 때 읽기 때문에, 재부팅
전까지는 아무 일도 일어나지 않아요.

`pkgman add-repo`가 `Operation not supported`로 실패하면 그 이미지의 네트워크
킷에 TLS가 없는 거예요. 주소의 `https://`를 `http://`로 바꿔 쓰세요.

## 소스에서 설치

Haiku 기기에서:

```sh
make                      # 드라이버와 accelerant를 빌드
make install              # 둘 다 ~/config/non-packaged에 설치
```

그리고 재부팅하세요.

```sh
make uninstall            # 둘 다 제거. 재부팅하면 VESA로 돌아가요
```

## 잘 됐는지 확인하기

재부팅한 뒤:

```sh
ls /dev/graphics/
```

`vesa` 옆에 `poulsbo`가 보여야 해요. app_server가 실제로 어느 쪽을 골랐는지는
이렇게 봐요:

```sh
listimage $(ps | grep [a]pp_server | head -1 | awk '{print $2}') | grep accelerant
```

`poulsbo.accelerant`가 나오면 성공이에요. `vesa.accelerant`가 나오면 드라이버가
선택되지 않은 거예요 - [AGENTS.md](AGENTS.md)를 보세요.

## VESA로 되돌리기

pkgman으로 설치했다면:

```sh
pkgman uninstall gma500
```

소스에서 설치했다면:

```sh
make uninstall
```

어느 쪽이든 그다음 재부팅하세요. `/dev/graphics/poulsbo`가 없으면 app_server가
알아서 VESA 드라이버로 돌아가기 때문에, 드라이버가 잘못돼도 화면이 아주 없어지는
일은 생기지 않아요. 혹시 그런 일이 생기면 ssh로 접속해서 파일을 지우면 돼요.

## 이 드라이버가 하지 않는 일

- **2D·3D 가속은 없어요.** 이 칩의 그리기 엔진은 PowerVR SGX535라서 공개된
  문서가 없어요. 여기서는 Intel이 만든 i915 계열 디스플레이 쪽만 써요.
- **모드 설정을 하지 않아요.** BIOS가 세운 모드를 그대로 둬요. 대상 기기들은
  어차피 패널 해상도가 고정이에요.
- **DPMS는 "켜짐"만 보고해요.**

## AI 사용 고지

이 드라이버의 일부는 AI 코딩 도구(Anthropic Claude)의 도움을 받아 개발했어요.
모든 코드는 작성자가 실기기에서 검토하고 테스트했어요.

## 라이선스

MIT. 레지스터가 무슨 일을 하는지, 무엇을 측정했는지, 어떤 두 가지 실수에 시간을
가장 많이 썼는지 같은 개발 기록은 [AGENTS.md](AGENTS.md)에 있어요.
