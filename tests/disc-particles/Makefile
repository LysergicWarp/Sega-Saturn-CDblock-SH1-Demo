ifeq ($(strip $(YAUL_INSTALL_ROOT)),)
  $(error Undefined YAUL_INSTALL_ROOT (install root directory))
endif

include $(YAUL_INSTALL_ROOT)/share/build.pre.mk

# Each asset follows the format: <path>;<symbol>. The particle module's package comes from
# ../../src/modules-asm, built first.
BUILTIN_ASSETS= \
	../../src/modules-asm/build/particle-package.bin;asset_particle

SH_PROGRAM:= disc-particles
SH_SRCS:= \
	main.c \
	../../src/sh1-client/sh1-client.c

SH_CFLAGS+= -O2 -I. -I../../src/sh1-client
SH_LDFLAGS+=

IP_VERSION:= V0.001
IP_RELEASE_DATE:= 20261122
IP_AREAS:= JTUBKAEL
IP_PERIPHERALS:= JAMKST
IP_TITLE:= Particles, three CPUs
IP_MASTER_STACK_ADDR:= 0x06004000
IP_SLAVE_STACK_ADDR:= 0x06001E00
IP_1ST_READ_ADDR:= 0x06004000
IP_1ST_READ_SIZE:= 0

include $(YAUL_INSTALL_ROOT)/share/build.post.iso-cue.mk

# craft.py writes the encoded bootstrap into cd/ as SH1EXT.BIN first, and points the header's +0xf8
# and +0xfc at it after the image is made.
