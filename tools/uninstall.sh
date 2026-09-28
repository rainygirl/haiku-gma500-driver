#!/bin/sh
# 되돌리기. 드라이버와 accelerant 를 지운다. 재부팅하면 vesa 로 돌아간다.
DRIVERS="$HOME/config/non-packaged/add-ons/kernel/drivers"
ACCELERANTS="$HOME/config/non-packaged/add-ons/accelerants"
rm -f "$DRIVERS/bin/poulsbo" "$DRIVERS/dev/graphics/poulsbo"
rm -f "$ACCELERANTS/poulsbo.accelerant"
echo "지웠다. 재부팅하면 vesa 로 돌아간다."
