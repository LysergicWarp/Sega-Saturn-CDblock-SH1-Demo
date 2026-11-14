# Assembles each module into build/NAME-module.bin, linked into the card firmware's module slot,
# then build/NAME-package.bin, its package, with ../../tools/package.py. Uses SH binutils,
# sh2eb-elf- or the toolchain under YAUL_INSTALL_ROOT, PREFIX naming another, and Python 3, PYTHON
# naming another. The yaul Docker image (ijacquez/yaul) has the binutils but no Python: make
# build/NAME-module.bin there, then run the package.py line on a host with Python.

PREFIX ?= $(if $(YAUL_INSTALL_ROOT),$(YAUL_INSTALL_ROOT)/bin/)sh2eb-elf-
PYTHON ?= python3
MODULES = decompress transform cipher

all: $(MODULES:%=build/%-package.bin)

build/%-package.bin: build/%-module.bin ../../tools/package.py
	$(PYTHON) ../../tools/package.py $< $@

build/%-module.bin: build/%-module.elf
	$(PREFIX)objcopy -O binary $< $@

build/%-module.elf: %-module.s %-module.ld
	mkdir -p build
	$(PREFIX)as --big --isa=sh -o build/$*-module.o $<
	$(PREFIX)ld --no-relax -T $*-module.ld -o $@ build/$*-module.o

clean:
	rm -rf build

# The images stay beside their packages.
.SECONDARY:

.PHONY: all clean
