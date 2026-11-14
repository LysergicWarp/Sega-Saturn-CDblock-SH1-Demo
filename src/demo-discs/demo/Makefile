ifeq ($(strip $(YAUL_INSTALL_ROOT)),)
  $(error Undefined YAUL_INSTALL_ROOT (install root directory))
endif

include $(YAUL_INSTALL_ROOT)/share/build.pre.mk

# libyaul's PRS decoder, for the SH-2s' jobs
include $(YAUL_INSTALL_ROOT)/share/build.bcl.mk

# Each asset follows the format: <path>;<symbol>. The package comes from ../../modules-asm, the
# benchmark's table from ../../../tools/bench.py, both made by ../../../build.py first.
BUILTIN_ASSETS= \
	../../modules-asm/build/decompress-package.bin;asset_decompress \
	build/bench-table.bin;asset_bench_table

SH_PROGRAM:= cd-block-sh1-demo
SH_SRCS:= \
	main.c \
	benchmark.c \
	../../dispatch/dispatch.c \
	../../sh1-client/sh1-client.c

SH_CFLAGS+= -O2 -I. -I../../dispatch -I../../sh1-client $(BCL_CFLAGS)
SH_LDFLAGS+= $(BCL_LDFLAGS)

IP_VERSION:= V0.001
IP_RELEASE_DATE:= 20261026
IP_AREAS:= JTUBKAEL
IP_PERIPHERALS:= JAMKST
IP_TITLE:= CD-block SH-1 demo
IP_MASTER_STACK_ADDR:= 0x06004000
IP_SLAVE_STACK_ADDR:= 0x06001E00
IP_1ST_READ_ADDR:= 0x06004000
IP_1ST_READ_SIZE:= 0

include $(YAUL_INSTALL_ROOT)/share/build.post.iso-cue.mk

# The benchmark's file goes on the disc as BENCH.PRS: the image is made from cd/, after this target
pre-build-iso: build/bench.prs
	mkdir -p cd
	cp build/bench.prs cd/BENCH.PRS
