# Assembles the disc extension probe into build/probe.bin with the yaul image's SH binutils.

PREFIX ?= $(if $(YAUL_INSTALL_ROOT),$(YAUL_INSTALL_ROOT)/bin/)sh2eb-elf-

build/probe.bin: build/probe.elf
	$(PREFIX)objcopy -O binary $< $@

build/probe.elf: probe.s probe.ld
	mkdir -p build
	$(PREFIX)as --big --isa=sh -o build/probe.o probe.s
	$(PREFIX)ld --no-relax -T probe.ld -o $@ build/probe.o

clean:
	rm -rf build

.PHONY: clean
