CC_ARM  ?= arm-linux-gnueabihf-gcc
CC_HOST ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -fPIC
LIB      = libmpc-addin-srate.so
VERSION ?= dev

all: build/arm/$(LIB)

build/arm/$(LIB): src/srate.c
	mkdir -p build/arm
	$(CC_ARM) $(CFLAGS) -march=armv7-a -mfpu=neon -mfloat-abi=hard -shared -o $@ $< -ldl -lpthread

build/host/$(LIB): src/srate.c
	mkdir -p build/host
	$(CC_HOST) $(CFLAGS) -shared -o $@ $< -ldl -lpthread

test: build/host/$(LIB)
	$(CC_HOST) -O2 -o build/host/test_host tools/test_host.c -ldl
	$(CC_HOST) -O2 -o build/host/test_alsa tools/test_alsa.c -l:libasound.so.2
	printf ' 1 [Force          ]: USB-Audio - Force\n                      Akai Professional Force\n 2 [SH4d           ]: USB-Audio - SH-4d\n                      Roland SH-4d at usb-1\n' > build/host/cards
	SRATE_PROC_CARDS=build/host/cards build/host/test_host build/host/$(LIB)
	printf 'mode=plug\nverbose=1\n' > build/host/srate.conf
	rm -f build/host/srate.log
	SRATE_PROC_CARDS=build/host/cards LD_PRELOAD=$(CURDIR)/build/host/$(LIB) build/host/test_alsa
	grep -q "set_rate null 44100 -> 0" build/host/srate.log
	SRATE_TEST_FAILFIRST=1 SRATE_TEST_DMX=1 TEST_CH=32 SRATE_PROC_CARDS=build/host/cards LD_PRELOAD=$(CURDIR)/build/host/$(LIB) build/host/test_alsa | grep -q "writei 256 -> 256"
	grep -q "Kanal-Reduktion aktiv: 32 -> 2" build/host/srate.log
	@echo "Host-Tests OK"

dist: build/arm/$(LIB)
	rm -rf build/dist && mkdir -p build/dist/mpc-addin-srate
	cp build/arm/$(LIB) srate.conf install.sh uninstall.sh diag.sh README.md build/dist/mpc-addin-srate/
	chmod 755 build/dist/mpc-addin-srate/*.sh
	cd build/dist && zip -qr mpc-addin-srate-$(VERSION).zip mpc-addin-srate
	@echo "build/dist/mpc-addin-srate-$(VERSION).zip"

clean:
	rm -rf build

.PHONY: all test dist clean
