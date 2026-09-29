## Haiku Generic Build Makefile ##
NAME = poulsbo.accelerant
TYPE = SHARED

SRCS = ../src/accelerant.c
LOCAL_INCLUDE_PATHS = ../src
# overlay_buffer, overlay_token 등 스프라이트 평면에 쓰는 타입이 여기 있다.
SYSTEM_INCLUDE_PATHS = /boot/system/develop/headers/private/graphics
LIBS = be

## Include the Makefile-Engine that ships with the Haiku SDK.
include /boot/system/develop/etc/makefile-engine
