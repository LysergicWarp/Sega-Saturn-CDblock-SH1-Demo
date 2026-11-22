ifeq ($(strip $(YAUL_INSTALL_ROOT)),)
  $(error Undefined YAUL_INSTALL_ROOT (install root directory))
endif

include $(YAUL_INSTALL_ROOT)/share/build.pre.mk

# libyaul's PRS decoder, for the SH-2s' jobs
include $(YAUL_INSTALL_ROOT)/share/build.bcl.mk

# Each asset follows the format: <path>;<symbol>. The package comes from ../../src/modules-asm, the
# benchmark's table from ../../tools/bench.py, both made by ../../build.py first.
BUILTIN_ASSETS= \
	../../src/modules-asm/build/decompress-package.bin;asset_decompress \
	../../src/demo-discs/demo/build/bench-table.bin;asset_bench_table

SH_PROGRAM:= disc-bench
SH_SRCS:= \
	main.c \
	../../src/demo-discs/demo/benchmark.c \
	../../src/dispatch/dispatch.c \
	../../src/sh1-client/sh1-client.c

SH_CFLAGS+= -O2 -I. -I../../src/demo-discs/demo -I../../src/dispatch -I../../src/sh1-client \
	-DDISPATCH_FIRMWARE_FROM_DISC $(BCL_CFLAGS)
SH_LDFLAGS+= $(BCL_LDFLAGS)

IP_VERSION:= V0.001
IP_RELEASE_DATE:= 20261122
IP_AREAS:= JTUBKAEL
IP_PERIPHERALS:= JAMKST
IP_TITLE:= SH-1 bench, no card
IP_MASTER_STACK_ADDR:= 0x06004000
IP_SLAVE_STACK_ADDR:= 0x06001E00
IP_1ST_READ_ADDR:= 0x06004000
IP_1ST_READ_SIZE:= 0

include $(YAUL_INSTALL_ROOT)/share/build.post.iso-cue.mk

# The benchmark's file goes on the disc as BENCH.PRS. craft.py writes the encoded bootstrap into
# cd/ as SH1EXT.BIN first, and points the header's +0xf8 and +0xfc at it after the image is made.
pre-build-iso: ../../src/demo-discs/demo/build/bench.prs
	mkdir -p cd
	cp ../../src/demo-discs/demo/build/bench.prs cd/BENCH.PRS
