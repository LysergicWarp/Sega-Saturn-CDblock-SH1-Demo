ifeq ($(strip $(YAUL_INSTALL_ROOT)),)
  $(error Undefined YAUL_INSTALL_ROOT (install root directory))
endif

include $(YAUL_INSTALL_ROOT)/share/build.pre.mk

# libyaul's PRS decoder, for the SH-2s' jobs
include $(YAUL_INSTALL_ROOT)/share/build.bcl.mk

# Each asset follows the format: <path>;<symbol>. Duplicates are removed. The package comes from
# ../../src/modules-asm, built first. The stream is ../module-check/original.txt compressed by
# bcl_prs.
BUILTIN_ASSETS= \
	../../src/modules-asm/build/decompress-package.bin;asset_decompress \
	build/original.prs;asset_stream \
	../module-check/original.txt;asset_original \
	build/small.prs;asset_small_stream \
	build/small.txt;asset_small_original

SH_PROGRAM:= dispatch-check
SH_SRCS:= \
	main.c \
	../../src/dispatch/dispatch.c \
	../../src/sh1-client/sh1-client.c

SH_CFLAGS+= -O2 -I. -I../../src/dispatch -I../../src/sh1-client $(BCL_CFLAGS)
SH_LDFLAGS+= $(BCL_LDFLAGS)

IP_VERSION:= V1.000
IP_RELEASE_DATE:= 20261026
IP_AREAS:= JTUBKAEL
IP_PERIPHERALS:= JAMKST
IP_TITLE:= Compute selftest
IP_MASTER_STACK_ADDR:= 0x06004000
IP_SLAVE_STACK_ADDR:= 0x06001E00
IP_1ST_READ_ADDR:= 0x06004000
IP_1ST_READ_SIZE:= 0

include $(YAUL_INSTALL_ROOT)/share/build.post.iso-cue.mk

build/original.prs: ../module-check/original.txt
	$(YAUL_INSTALL_ROOT)/bin/bcl_prs $< $@

# The cache case's stream: the original's first 256 bytes, small enough that its decode leaves the
# job record in the slave's cache
build/small.txt: ../module-check/original.txt
	mkdir -p build
	head -c 256 $< > $@

build/small.prs: build/small.txt
	$(YAUL_INSTALL_ROOT)/bin/bcl_prs $< $@
