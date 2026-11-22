# Assembles the firmware bootstrap into build/firmware-boot.bin with the yaul image's SH binutils.
# It carries the assembly card firmware's image, which its own Makefile builds first.

PREFIX ?= $(if $(YAUL_INSTALL_ROOT),$(YAUL_INSTALL_ROOT)/bin/)sh2eb-elf-
FIRMWARE := ../../card-firmwares/card-firmware-asm/build

build/firmware-boot.bin: build/firmware-boot.elf
	$(PREFIX)objcopy -O binary $< $@

build/firmware-boot.elf: firmware-boot.s firmware-boot.ld $(FIRMWARE)/card-firmware.bin
	mkdir -p build
	$(PREFIX)as --big --isa=sh -I $(FIRMWARE) -o build/firmware-boot.o firmware-boot.s
	$(PREFIX)ld --no-relax -T firmware-boot.ld -o $@ build/firmware-boot.o

clean:
	rm -rf build

.PHONY: clean
