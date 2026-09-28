#!/bin/sh
# 2단계: accelerant 를 설치하고 app_server 를 다시 띄운다.
#
# 여기서부터 화면이 걸릴 수 있다. ssh 는 app_server 와 무관하게 살아 있으므로,
# 화면이 돌아오지 않으면 다음으로 되돌린다:
#
#   ~/Workspace/haiku-apps/gma500-driver/tools/uninstall.sh && shutdown -r
set -e
cd "$(dirname "$0")/.."

ACCELERANTS="$HOME/config/non-packaged/add-ons/accelerants"
accelerant="$(find accelerant/objects.* -name 'poulsbo.accelerant' -type f | head -1)"
[ -n "$accelerant" ] || { echo "accelerant 를 먼저 빌드하라: make" >&2; exit 1; }

mkdir -p "$ACCELERANTS"
cp "$accelerant" "$ACCELERANTS/poulsbo.accelerant"
echo "설치: $ACCELERANTS/poulsbo.accelerant"
echo "app_server 를 다시 띄운다 (열린 창은 사라진다)"
sleep 1
kill -9 $(ps | awk '/app_server/ && !/awk/ {print $2; exit}') 2>/dev/null || true
