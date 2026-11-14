# Build environment for the demo's Beetle Saturn fork. Debian 13 with GCC 14 builds the Linux core
# and the headless runner, MinGW-w64 GCC 14 with posix threads the Windows core. The MinGW packages
# are version-pinned, the rest of apt is not.

ARG BASE_IMAGE=debian:trixie-slim@sha256:020c0d20b9880058cbe785a9db107156c3c75c2ac944a6aa7ab59f2add76a7bd
FROM ${BASE_IMAGE}

ARG MINGW_GCC_PACKAGE_VERSION=14.2.0-19+27+b1
ARG MINGW_BINUTILS_PACKAGE_VERSION=2.44-3+12+b1

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        binutils-mingw-w64-x86-64=${MINGW_BINUTILS_PACKAGE_VERSION} \
        gcc-mingw-w64-x86-64-posix=${MINGW_GCC_PACKAGE_VERSION} \
        g++-mingw-w64-x86-64-posix=${MINGW_GCC_PACKAGE_VERSION} \
    && rm -rf /var/lib/apt/lists/*
