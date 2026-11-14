ifeq ($(strip $(YAUL_INSTALL_ROOT)),)
  $(error Undefined YAUL_INSTALL_ROOT (install root directory))
endif

include $(YAUL_INSTALL_ROOT)/share/build.pre.mk

# Each asset follows the format: <path>;<symbol>. Duplicates are removed. The package comes from
# ../../src/modules-asm, built first. The stream is ../module-check/original.txt compressed by
# bcl_prs.
BUILTIN_ASSETS= \
	../../src/modules-asm/build/decompress-package.bin;asset_decompress \
	build/original.prs;asset_stream \
	../module-check/original.txt;asset_original

SH_PROGRAM:= lifecycle-check
SH_SRCS:= \
	main.c \
	../../src/sh1-client/sh1-client.c

SH_CFLAGS+= -O2 -I. -I../../src/sh1-client
SH_LDFLAGS+=

IP_VERSION:= V1.000
IP_RELEASE_DATE:= 20261026
IP_AREAS:= JTUBKAEL
IP_PERIPHERALS:= JAMKST
IP_TITLE:= Lifecycle check
IP_MASTER_STACK_ADDR:= 0x06004000
IP_SLAVE_STACK_ADDR:= 0x06001E00
IP_1ST_READ_ADDR:= 0x06004000
IP_1ST_READ_SIZE:= 0

include $(YAUL_INSTALL_ROOT)/share/build.post.iso-cue.mk

build/original.prs: ../module-check/original.txt
	$(YAUL_INSTALL_ROOT)/bin/bcl_prs $< $@
