#!/bin/sh
# 1단계: 커널 드라이버만 설치한다. accelerant 는 아직 넣지 않는다.
#
# accelerant 가 없으면 app_server 는 /dev/graphics/poulsbo 를 열었다가
# accelerant 로딩에 실패하고 다음 장치(vesa)로 넘어간다. 즉 이 단계에서는
# 화면이 지금과 똑같이 동작해야 한다. 드라이버가 커널에서 살아남는지,
# 장치가 publish 되는지만 본다.
set -e
cd "$(dirname "$0")/.."

DRIVERS="$HOME/config/non-packaged/add-ons/kernel/drivers"
driver="$(find driver/objects.* -name poulsbo -type f | head -1)"
[ -n "$driver" ] || { echo "드라이버를 먼저 빌드하라: make" >&2; exit 1; }

mkdir -p "$DRIVERS/bin" "$DRIVERS/dev/graphics"
cp "$driver" "$DRIVERS/bin/poulsbo"
ln -sf ../../bin/poulsbo "$DRIVERS/dev/graphics/poulsbo"
echo "설치: $DRIVERS/bin/poulsbo"
echo "재부팅한 뒤 확인:"
echo "  ls /dev/graphics/"
echo "  grep poulsbo /var/log/syslog"
