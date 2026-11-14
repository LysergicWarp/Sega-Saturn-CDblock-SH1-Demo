# The SH-1 compiler for the card firmware's C version: binutils 2.42 and GCC 13.2, the GCC the yaul
# image's SH-2 compiler is, built for sh-elf with SH-1 code (-m1) the default and libgcc built for
# it alone. The yaul image's sh2eb-elf compiler has no -m1, and its code uses instructions the
# SH-1 lacks (mul.l, dt, the delayed conditional branches). build.py builds this image when asked
# for the C firmware. Installed in /opt/sh1, its tools named sh-elf-.

ARG BASE_IMAGE=ubuntu:24.04@sha256:68ee22004a9109b44973313b02ff7b1de250f1de0f5ecb628d73f579aaff33ac
FROM ${BASE_IMAGE}

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential make ca-certificates curl xz-utils libgmp-dev libmpfr-dev libmpc-dev \
    && rm -rf /var/lib/apt/lists/*

ARG BINUTILS=binutils-2.42
ARG BINUTILS_SHA256=f6e4d41fd5fc778b06b7891457b3620da5ecea1006c6a4a41ae998109f85a800
ARG GCC=gcc-13.2.0
ARG GCC_SHA256=e275e76442a6067341a27f04c5c6b83d8613144004c0413528863dc6b5c743da

RUN mkdir /work && cd /work \
    && curl -fsSL -o binutils.tar.xz https://ftp.gnu.org/gnu/binutils/${BINUTILS}.tar.xz \
    && echo "${BINUTILS_SHA256}  binutils.tar.xz" | sha256sum -c - \
    && tar xf binutils.tar.xz && mkdir build-binutils && cd build-binutils \
    && ../${BINUTILS}/configure --target=sh-elf --prefix=/opt/sh1 --disable-nls --disable-werror \
        --disable-gdb --disable-sim \
    && make -j"$(nproc)" MAKEINFO=true && make install MAKEINFO=true \
    && cd / && rm -rf /work

ENV PATH=/opt/sh1/bin:$PATH

RUN mkdir /work && cd /work \
    && curl -fsSL -o gcc.tar.xz https://ftp.gnu.org/gnu/gcc/${GCC}/${GCC}.tar.xz \
    && echo "${GCC_SHA256}  gcc.tar.xz" | sha256sum -c - \
    && tar xf gcc.tar.xz && mkdir build-gcc && cd build-gcc \
    && ../${GCC}/configure --target=sh-elf --prefix=/opt/sh1 --enable-languages=c \
        --with-cpu=m1 --with-multilib-list=m1 --with-endian=big --without-headers \
        --disable-nls --disable-shared --disable-threads --disable-libssp --disable-libquadmath \
        --disable-libgomp --disable-libatomic \
    && make -j"$(nproc)" all-gcc all-target-libgcc MAKEINFO=true \
    && make install-gcc install-target-libgcc MAKEINFO=true \
    && cd / && rm -rf /work
