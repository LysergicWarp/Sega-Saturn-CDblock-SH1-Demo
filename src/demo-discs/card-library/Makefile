ifeq ($(strip $(YAUL_INSTALL_ROOT)),)
  $(error Undefined YAUL_INSTALL_ROOT (install root directory))
endif

include $(YAUL_INSTALL_ROOT)/share/build.pre.mk

# Each asset follows the format: <path>;<symbol>. The packages come from ../../modules-asm, made by
# ../../../build.py first. The stream is main.c compressed by bcl_prs, and main.c the original.
# The encrypted file is main.c encrypted by ../../../tools/cipher.py, which build.py runs first.
BUILTIN_ASSETS= \
	../../modules-asm/build/decompress-package.bin;asset_decompress \
	../../modules-asm/build/transform-package.bin;asset_transform \
	../../modules-asm/build/cipher-package.bin;asset_cipher \
	build/main.prs;asset_stream \
	build/main.xtea;asset_encrypted \
	main.c;asset_original

SH_PROGRAM:= card-library
SH_SRCS:= \
	main.c \
	../../sh1-client/sh1-client.c

SH_CFLAGS+= -O2 -I. -I../../sh1-client
SH_LDFLAGS+=

IP_VERSION:= V0.001
IP_RELEASE_DATE:= 20261027
IP_AREAS:= JTUBKAEL
IP_PERIPHERALS:= JAMKST
IP_TITLE:= Card ROM module library
IP_MASTER_STACK_ADDR:= 0x06004000
IP_SLAVE_STACK_ADDR:= 0x06001E00
IP_1ST_READ_ADDR:= 0x06004000
IP_1ST_READ_SIZE:= 0

include $(YAUL_INSTALL_ROOT)/share/build.post.iso-cue.mk

build/main.prs: main.c
	mkdir -p build
	$(YAUL_INSTALL_ROOT)/bin/bcl_prs $< $@
