# 커널 드라이버와 accelerant 는 커널과 같은 기본 아키텍처(x86_gcc2)로
# 빌드해야 한다. 각각 Haiku 의 makefile-engine 을 쓴다.
DRIVER_INSTALL ?= $(HOME)/config/non-packaged/add-ons/kernel/drivers
ACCEL_INSTALL ?= $(HOME)/config/non-packaged/add-ons/accelerants

.PHONY: all driver accelerant install uninstall clean

all: driver accelerant

driver:
	$(MAKE) -C driver

accelerant:
	$(MAKE) -C accelerant

install: all
	mkdir -p $(DRIVER_INSTALL)/bin $(DRIVER_INSTALL)/dev/graphics $(ACCEL_INSTALL)
	cp $$(find driver/objects.* -name poulsbo -type f | head -1) $(DRIVER_INSTALL)/bin/poulsbo
	ln -sf ../../bin/poulsbo $(DRIVER_INSTALL)/dev/graphics/poulsbo
	cp $$(find accelerant/objects.* -name "poulsbo.accelerant" -type f | head -1) $(ACCEL_INSTALL)/

uninstall:
	rm -f $(DRIVER_INSTALL)/bin/poulsbo $(DRIVER_INSTALL)/dev/graphics/poulsbo
	rm -f $(ACCEL_INSTALL)/poulsbo.accelerant

clean:
	$(MAKE) -C driver clean; $(MAKE) -C accelerant clean
