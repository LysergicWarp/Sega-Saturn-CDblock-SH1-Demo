# Builds the card firmware's C version into build/card-firmware.bin, then build/card-rom.bin, the
# card ROM in the known cards' layout, with ../../../tools/payload.py. Uses an SH-1 GCC, sh-elf-
# from the demo's Dockerfile or PREFIX naming another, and Python 3, PYTHON naming another. That
# image has no Python: make build/card-firmware.bin there, then run the payload.py line on a host
# with Python.

PREFIX ?= sh-elf-
PYTHON ?= python3

# SH-1 code, freestanding, r14 left alone, and no calls to memcpy or memset made up by the compiler
CFLAGS = -m1 -Os -std=gnu11 -Wall -Wextra -ffreestanding -fno-builtin -nostdlib -ffixed-r14 \
	-fomit-frame-pointer -fno-tree-loop-distribute-patterns -fno-asynchronous-unwind-tables

build/card-rom.bin: build/card-firmware.bin ../../../tools/payload.py
	$(PYTHON) ../../../tools/payload.py card $< $@

build/card-firmware.bin: build/card-firmware.elf
	$(PREFIX)objcopy -O binary -j .image $< $@

build/card-firmware.elf: card-firmware.c boundary.h boundary.s card-firmware.ld
	mkdir -p build
	$(PREFIX)gcc $(CFLAGS) -c -o build/card-firmware.o card-firmware.c
	$(PREFIX)as --big --isa=sh -o build/boundary.o boundary.s
	$(PREFIX)gcc -m1 -nostdlib -T card-firmware.ld -Wl,--no-relax,--no-warn-rwx-segments \
		-o $@ build/card-firmware.o build/boundary.o -lgcc

clean:
	rm -rf build

.PHONY: clean
