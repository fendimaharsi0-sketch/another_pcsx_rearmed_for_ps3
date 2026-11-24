# Toolchain image for building the PS3 package on any machine.
#
# It is what ps3/build.sh expects to find on the host: the ps3dev cross
# toolchain (PPU + SPU), PSL1GHT and its host tools, and the Cg compiler.
# Build it once from CI (see .github/workflows/build-image.yml) and pull it
# locally, instead of installing all of this on Windows.
#
# Base is Ubuntu because the Cg compiler comes from the nvidia-cg-toolkit
# apt package, which Debian does not carry; every package below is the same
# set the Build PS3 PKG workflow installs on its ubuntu-latest runner, so the
# image and that workflow cannot drift apart.
#
# The toolchain build is the slow part (PPU and SPU binutils + GCC/newlib,
# about half an hour). GDB is skipped: build.sh never calls it.
#
# PSL1GHT is patched twice, for the same two reasons as the workflow:
#  - 0001: rsx_function_macros.h needs the "memory" clobber, or GCC 11+
#    miscompiles the RSX command-buffer callback and the GPU hangs at the
#    first buffer wrap;
#  - 0002: the pkg tool caps a package at 1024 entries and fills per-
#    directory stack arrays, so our 4502-entry package aborts it. Without
#    this patch the pack step dies with a stack smashing error.

FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      autoconf automake bison build-essential flex gcc libelf-dev make texinfo \
      libncurses5-dev patch python3 python-is-python3 subversion wget zlib1g-dev \
      libtool-bin python3-dev bzip2 libgmp3-dev pkg-config libssl-dev clang git \
      cmake ca-certificates nvidia-cg-toolkit \
 && rm -rf /var/lib/apt/lists/*

ENV PS3DEV=/usr/local/ps3dev
ENV PSL1GHT=${PS3DEV}
ENV PATH=${PATH}:${PS3DEV}/bin:${PS3DEV}/ppu/bin:${PS3DEV}/spu/bin

WORKDIR /build

# Cross toolchain: binutils + GCC/newlib for the PPU, then for the SPU.
RUN git clone https://github.com/ps3dev/ps3toolchain.git /src/ps3toolchain \
 && cd /src/ps3toolchain \
 && ./download.sh \
 && ./toolchain.sh 001 && ./toolchain.sh 002 && ./toolchain.sh 004 \
 && ./toolchain.sh 005 && ./toolchain.sh 006 \
 && cd / && rm -rf /src/ps3toolchain

# PSL1GHT and its host tools (cgcomp, sprxlinker, make_self_npdrm, sfo, pkg).
COPY psl1ght-patches /src/psl1ght-patches
RUN git clone https://github.com/ps3dev/PSL1GHT.git /src/PSL1GHT \
 && cd /src/PSL1GHT \
 && patch -p1 < /src/psl1ght-patches/0001-rsx-context-callback-clobbers.patch \
 && patch -p1 < /src/psl1ght-patches/0002-pkg-max-files.patch \
 && make install-ctrl && make && make install

# The Python packer replaces PSL1GHT's C pkg tool for packing: the C tool
# drops USRDIR-level files on some machines even when it reports success.
RUN gcc -shared -fPIC $(python3-config --includes) \
      /src/PSL1GHT/tools/ps3py/crypt.c -o /src/PSL1GHT/tools/ps3py/pkgcrypt.so \
 && cp /src/PSL1GHT/tools/ps3py/pkg.py /src/PSL1GHT/tools/ps3py/Struct.py \
      /src/PSL1GHT/tools/ps3py/fself.py /src/PSL1GHT/tools/ps3py/pkgcrypt.so \
      "${PS3DEV}/bin/" \
 && test -x "${PS3DEV}/bin/cgcomp" \
 && test -x "${PS3DEV}/bin/sprxlinker" \
 && test -x "${PS3DEV}/bin/make_self_npdrm" \
 && test -x "${PS3DEV}/bin/sfo" \
 && test -f  "${PS3DEV}/bin/pkg.py"
