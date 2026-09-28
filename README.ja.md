# Haiku 用 GMA500 ドライバー

[English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md)

Sony VAIO P のような Intel SCH US15W (Poulsbo、いわゆる「GMA500」) を搭載した
マシン向けのグラフィックドライバーです。このチップで Haiku が
**ハードウェアカーソル**を使えるようにします。VESA ドライバーではできないことです。

これがないと、app_server はマウスポインターをソフトウェアで描きます。動くたびに
ポインターの下を保存し、ポインターを描き、元のピクセルを戻します。2D
アクセラレーションのない 1.33 GHz Atom では、その処理が目に見えます。このドライバー
では表示エンジンがスキャンアウト時にポインターを合成するため、その処理自体が
なくなります。

## pkgman でインストール

| Haiku | コマンド |
| --- | --- |
| 32 ビット x86 (x86_gcc2) | `pkgman add-repo https://pkgman.rainygirl.com/x86_gcc2`<br>`pkgman install gma500` |

**インストール後は再起動してください。** カーネルはグラフィックドライバーを起動時に
読み込むため、再起動するまで何も変わりません。

`pkgman add-repo` が `Operation not supported` で失敗する場合、そのイメージの
ネットワークキットに TLS がありません。`http://pkgman.rainygirl.com/x86_gcc2` を
使ってください。

## ソースからインストール

Haiku マシン上で:

```sh
make                      # ドライバーと accelerant をビルド
make install              # 両方を ~/config/non-packaged にインストール
```

そのあと再起動します。

```sh
make uninstall            # 両方を削除。再起動すると VESA に戻ります
```

## 動いているかの確認

再起動後:

```sh
ls /dev/graphics/
```

`vesa` の隣に `poulsbo` があるはずです。app_server がどちらを選んだかは:

```sh
listimage $(ps | grep [a]pp_server | head -1 | awk '{print $2}') | grep accelerant
```

`poulsbo.accelerant` と出れば成功です。`vesa.accelerant` と出た場合はドライバーが
選ばれていません - [AGENTS.md](AGENTS.md) を参照してください。

## VESA に戻す

pkgman でインストールした場合:

```sh
pkgman uninstall gma500
```

ソースからインストールした場合:

```sh
make uninstall
```

いずれの場合も再起動してください。`/dev/graphics/poulsbo` が無くなると app_server
は自動的に VESA ドライバーへ戻るため、ドライバーが壊れても画面が出なくなることは
ありません。万一そうなったら ssh で入ってファイルを削除してください。

## このドライバーがしないこと

- **2D・3D アクセラレーションはありません。** このチップの描画エンジンは PowerVR
  SGX535 で、公開された資料がありません。ここでは Intel 製で i915 系の表示側だけを
  使っています。
- **モード設定を行いません。** BIOS が設定したモードをそのまま使います。対象の
  マシンはいずれもパネル解像度が固定です。
- **DPMS は「オン」のみ報告します。**

## AI 利用の告知

このドライバーの一部は AI コーディングツール (Anthropic の Claude) の支援を受けて
開発しました。すべてのコードは作者が実機で確認・テストしています。

## ライセンス

MIT。レジスタの役割、測定結果、最も時間を取られた 2 つの間違いなどの開発記録は
[AGENTS.md](AGENTS.md) にあります。
