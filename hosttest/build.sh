#!/bin/sh
# Builds the big-endian (ppc64) test runners and the OpenGL replay tool.
# Needs gcc-powerpc64-linux-gnu, qemu-user, nvidia-cg-toolkit, libosmesa6-dev.
set -e
cd "$(dirname "$0")"
T=$PWD; R=$(cd .. && pwd)
# the core: the repository's top level, or core/ next to ps3/ (older layout)
if [ -f $R/Makefile.libretro ]; then C=$R; else C=$R/core; fi
for d in 0 lightrec; do
  w=$R/.core-be-$d
  [ -d $w ] || git -C $C worktree add -q --detach $w ps3
  git -C $w checkout -q --detach ps3
  make -C $w -f Makefile.libretro platform=unix CC=powerpc64-linux-gnu-gcc CXX=powerpc64-linux-gnu-g++ \
    AR=powerpc64-linux-gnu-ar ARCH=ppc64 DYNAREC=$d STATIC_LINKING=1 HAVE_PHYSICAL_CDROM=0 USE_ASYNC_CDROM=0 \
    USE_ASYNC_GPU=0 USE_ASYNC_SPU=0 USE_LIBRETRO_VFS=0 -j"$(nproc)" >/dev/null
done
F=$R/ps3/src/hwr
CF="-O1 -g -I$C/deps/libretro-common/include -I$F -I$C/plugins/gpulib -I$C/include"
powerpc64-linux-gnu-gcc $CF runner.c hwr_trace.c $F/hwr_core.c $R/.core-be-0/pcsx_rearmed_libretro.so -lm -lpthread -static -o runner_ppc64 2>/dev/null
powerpc64-linux-gnu-gcc $CF runner.c hwr_trace.c $F/hwr_core.c $R/.core-be-lightrec/pcsx_rearmed_libretro.so -lm -lpthread -static -o runner_lr_ppc64 2>/dev/null
# GLSL versions of the renderer programs: glsl/ byte-decoding set, glsl_fast/ fast set (GLSL=../glsl_fast for the scripts)
FX=$R/ps3/src/shaders; mkdir -p glsl glsl_fast
cpp -P $FX/hwr.vcg >glsl/hwr.vcg && cgc -quiet -profile glslv -o glsl/hwr.vert glsl/hwr.vcg
cpp -P -DFAST=1 $FX/hwr.vcg >glsl_fast/hwr.vcg && cgc -quiet -profile glslv -o glsl_fast/hwr.vert glsl_fast/hwr.vcg

for m in 0 1 2 3 4; do cgc -quiet -profile glslf -DMODE=$m -o glsl/f$m.frag $FX/hwr.fcg
  cgc -quiet -profile glslf -DMODE=$m -DFAST=1 -o glsl_fast/f$m.frag $FX/hwr.fcg; done
for m in 1 2 3; do cgc -quiet -profile glslf -DMODE=$m -DTWIN=1 -o glsl/f${m}w.frag $FX/hwr.fcg
  cgc -quiet -profile glslf -DMODE=$m -DTWIN=1 -DFAST=1 -o glsl_fast/f${m}w.frag $FX/hwr.fcg; done
# mask-check variants (m...) and the bit-15 self copy (f5)
for v in "0 0 f0" "1 0 f1" "2 0 f2" "3 0 f3" "4 0 f4" "1 1 f1w" "2 1 f2w" "3 1 f3w"; do set -- $v
  cgc -quiet -profile glslf -DMODE=$1 -DTWIN=$2 -DMCHK=1 -o glsl/m$3.frag $FX/hwr.fcg
  cgc -quiet -profile glslf -DMODE=$1 -DTWIN=$2 -DMCHK=1 -DFAST=1 -o glsl_fast/m$3.frag $FX/hwr.fcg; done
cgc -quiet -profile glslf -DMODE=5 -o glsl/f5.frag $FX/hwr.fcg
cgc -quiet -profile glslf -DMODE=5 -DFAST=1 -o glsl_fast/f5.frag $FX/hwr.fcg
gcc -O1 -g -I$F -o hwr_replay hwr_replay.c -lOSMesa -lm

# full-mode GL model, native: core built little-endian with the peops GPU (.core-le, a copy of core)
if [ -d $R/.core-le ]; then
  make -C $R/.core-le -f Makefile.libretro platform=unix BUILTIN_GPU=peops DYNAREC=0 STATIC_LINKING=1 HAVE_PHYSICAL_CDROM=0 \
    USE_ASYNC_CDROM=0 USE_ASYNC_GPU=0 USE_ASYNC_SPU=0 USE_LIBRETRO_VFS=0 -j"$(nproc)" >/dev/null
  gcc $CF -DHWR_GL -DSPU_MDEC -DHOST_TEST -I$C/libpcsxcore -I$R/ps3/spu runner.c hwr_gl.c hwr_trace.c $F/hwr_core.c \
    $R/ps3/src/spu_mdec.c $R/ps3/spu/mdec_spu.c $R/.core-le/pcsx_rearmed_libretro.so -lOSMesa -lm -lpthread -o runner_gl
fi
# SPU FMV decoder (dispatcher + SPU program run directly, DMA rules checked): spumdec/spumdec_test [iterations]
gcc -O2 -g -DHOST_TEST -I$C/libpcsxcore -I$R/ps3/spu spumdec/test.c $R/ps3/src/spu_mdec.c $R/ps3/spu/mdec_spu.c -o spumdec/spumdec_test
# SPU audio mixing (SPU mixer vs PPU mixer, bit-exact): spuaudio/spuaudio_test
gcc -O2 -g -DHOST_TEST -DUSE_ASYNC_SPU -I$R/plugins/dfsound -I$R/ps3/spu -I$R/frontend spuaudio/test.c -o spuaudio/spuaudio_test
echo built
# SPU edge smoothing kernel: vector code vs scalar reference (writes aa_in/aa_out.ppm)
gcc -O2 -ffp-contract=off -I$R/ps3/spu spuaa/test.c -lm -o spuaa/aa_test
