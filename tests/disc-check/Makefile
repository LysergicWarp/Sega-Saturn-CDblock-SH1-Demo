ifeq ($(strip $(YAUL_INSTALL_ROOT)),)
  $(error Undefined YAUL_INSTALL_ROOT (install root directory))
endif

include $(YAUL_INSTALL_ROOT)/share/build.pre.mk

# libyaul's PRS decoder, for the master's job
include $(YAUL_INSTALL_ROOT)/share/build.bcl.mk

# Each asset follows the format: <path>;<symbol>. Duplicates are removed. The package comes from
# ../../src/modules-asm, built first.
BUILTIN_ASSETS= \
	../../src/modules-asm/build/decompress-package.bin;asset_decompress \
	../module-check/original.txt;asset_original

SH_PROGRAM:= disc-check
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
IP_TITLE:= Disc decode selftest
IP_MASTER_STACK_ADDR:= 0x06004000
IP_SLAVE_STACK_ADDR:= 0x06001E00
IP_1ST_READ_ADDR:= 0x06004000
IP_1ST_READ_SIZE:= 0

include $(YAUL_INSTALL_ROOT)/share/build.post.iso-cue.mk

# The stream goes on the disc as STREAM.PRS: the image is made from cd/, after this target
pre-build-iso: build/original.prs
	mkdir -p cd
	cp build/original.prs cd/STREAM.PRS

build/original.prs: ../module-check/original.txt
	mkdir -p build
	$(YAUL_INSTALL_ROOT)/bin/bcl_prs $< $@
