# Assembles the card firmware into build/card-firmware.bin, then build/card-rom.bin, the card ROM
# in the known cards' layout, with ../../../tools/payload.py. Uses SH binutils, sh2eb-elf- or the
# toolchain under YAUL_INSTALL_ROOT, PREFIX naming another, and Python 3, PYTHON naming another.
# The yaul Docker image (ijacquez/yaul) has the binutils but no Python: make build/card-firmware.bin
# there, then run the payload.py line on a host with Python.

PREFIX ?= $(if $(YAUL_INSTALL_ROOT),$(YAUL_INSTALL_ROOT)/bin/)sh2eb-elf-
PYTHON ?= python3

build/card-rom.bin: build/card-firmware.bin ../../../tools/payload.py
	$(PYTHON) ../../../tools/payload.py card $< $@

build/card-firmware.bin: build/card-firmware.elf
	$(PREFIX)objcopy -O binary $< $@

build/card-firmware.elf: card-firmware.s card-firmware.ld
	mkdir -p build
	$(PREFIX)as --big --isa=sh -o build/card-firmware.o card-firmware.s
	$(PREFIX)ld --no-relax -T card-firmware.ld -o $@ build/card-firmware.o

clean:
	rm -rf build

.PHONY: clean
