ifeq ($(strip $(YAUL_INSTALL_ROOT)),)
  $(error Undefined YAUL_INSTALL_ROOT (install root directory))
endif

include $(YAUL_INSTALL_ROOT)/share/build.pre.mk

# Each asset follows the format: <path>;<symbol>. art/ holds the picture, made once from the
# artist's PNG (its license in art/Readme.txt): its bottom 192 rows in big-endian 16-bit pixels,
# 15-bit color with the top bit set, in strips. Strip n is the picture's columns 8n to 8n + 7, row
# after row, 3,072 bytes. strips.bin holds each encrypted for the cipher module under stream number
# n and padded to two sectors, so strip n is its sectors 2n and 2n + 1. The table: big-endian
# longwords, the strip count, then each strip's checksum before encryption, with the benchmark's
# checksum.
BUILTIN_ASSETS= \
	art/strips-table.bin;asset_strip_table

SH_PROGRAM:= scroller
SH_SRCS:= \
	main.c \
	../../shell/shell.c \
	../../sh1-client/sh1-client.c

SH_CFLAGS+= -O2 -I. -I../../shell -I../../sh1-client
SH_LDFLAGS+=

IP_VERSION:= V0.001
IP_RELEASE_DATE:= 20261027
IP_AREAS:= JTUBKAEL
IP_PERIPHERALS:= JAMKST
IP_TITLE:= Streamed scroller
IP_MASTER_STACK_ADDR:= 0x06004000
IP_SLAVE_STACK_ADDR:= 0x06001E00
IP_1ST_READ_ADDR:= 0x06004000
IP_1ST_READ_SIZE:= 0

include $(YAUL_INSTALL_ROOT)/share/build.post.iso-cue.mk

# The strips go on the disc as STRIPS.BIN: the image is made from cd/, after this target
pre-build-iso: art/strips.bin
	mkdir -p cd
	cp art/strips.bin cd/STRIPS.BIN
