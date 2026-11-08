#!/usr/bin/env bash
# Builds the PS3 package: the PCSX-ReARMed core (the folder above ps3/, or ../core
# next to it) + this front-end.
#
# Needs the PS3 toolchain and PSL1GHT (https://github.com/ps3dev/ps3toolchain) with
# PS3DEV set (default /usr/local/ps3dev), python3, and NVIDIA's Cg library for the
# shader compiler cgcomp (Ubuntu: nvidia-cg-toolkit).
#
# usage: ./build.sh [package-name]      -> build/<package-name>.pkg
#        DYNAREC=0 ./build.sh           -> interpreter-only build
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$PWD
# the core: this repository's top level (ps3/ is inside the pcsx_rearmed tree), or a
# core/ folder next to ps3/ (older source archives)
if [ -f "$ROOT/../Makefile.libretro" ]; then CORE=$(cd "$ROOT/.." && pwd)
else CORE=$(cd "$ROOT/../core" && pwd); fi
NAME=${1:-pcsx-rearmed-ps3}
export PS3DEV=${PS3DEV:-/usr/local/ps3dev}
export PSL1GHT=${PSL1GHT:-$PS3DEV}
export PATH=$PS3DEV/bin:$PS3DEV/ppu/bin:$PS3DEV/spu/bin:$PATH
JOBS=${JOBS:-$(nproc)}

# PSL1GHT's command-buffer callback must declare its clobbers, or GCC 11+ miscompiles
# every RSX command and the GPU hangs when the buffer first wraps (see psl1ght-patches/)
if ! grep -q '"memory"' "$PSL1GHT/ppu/include/rsx/rsx_function_macros.h"; then
    echo "PSL1GHT is missing psl1ght-patches/0001-rsx-context-callback-clobbers.patch:"
    echo "apply it to the PSL1GHT source, then rebuild and install ppu/librsx."
    exit 1
fi
DYNAREC=${DYNAREC:-lightrec}

B=$ROOT/build
GEN=$B/gen
mkdir -p "$B/obj" "$GEN" "$B/pkg/USRDIR"

# build provenance for the home corner (menu.c) and the startup log: branch
# + short commit of the tree being built. CI checks out the branch, so git
# answers; a tree without .git (or a failed git) falls back to "unknown"
# instead of failing the build. A "+" suffix means tracked files differ
# from HEAD (uncommitted changes); the build dir itself is untracked, so it
# never triggers the marker.
BRANCH=${GITHUB_REF_NAME:-$(git -C "$ROOT" rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)}
[ "$BRANCH" = HEAD ] && BRANCH=unknown
COMMIT=$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)
if [ "$COMMIT" != unknown ] && [ -n "$(git -C "$ROOT" status --porcelain --untracked-files=no 2>/dev/null)" ]; then COMMIT="$COMMIT+"; fi
printf '#define BUILD_BRANCH "%s"\n#define BUILD_COMMIT "%s"\n' "$BRANCH" "$COMMIT" >"$GEN/build_info.h"

echo "== core ($DYNAREC)"
make -C "$CORE" -f Makefile.libretro platform=psl1ght DYNAREC=$DYNAREC \
    EXTRA_CFLAGS="${CORE_CFLAGS:-}" -j"$JOBS" >"$B/core.log" 2>&1 || { tail -30 "$B/core.log"; exit 1; }
cp "$CORE/pcsx_rearmed_libretro_psl1ght.a" "$B/libcore.a"

echo "== shaders"
c_array() {
    python3 - "$1" "$2" <<'P'
import sys
d = open(sys.argv[1], 'rb').read()
print('static const unsigned char %s[] __attribute__((aligned(128))) = {%s};' % (sys.argv[2], ','.join(map(str, d))))
P
}
# fragment programs: cgcomp, after checking the Cg compiler's fp40 code for the
# colour-register hazard cgcomp does not handle (tools/fpcheck.py)
fpc() {
  cgc -quiet -profile fp40 -o "$2.fp40" "$1" || exit 1
  python3 tools/fpcheck.py "$2.fp40" || { echo "fragment program $1: colour register hazard" >&2; exit 1; }
  cgcomp -f "$1" "$2" >/dev/null
}
cgcomp -v src/shaders/quad.vcg "$GEN/quad.vpo" >/dev/null
for v in "blit 0 0 blit" "blit 1 0 blitd" "fxaa 0 0 fxaa" "fxaa 1 0 fxaad" "fxaa 0 1 fxaaf" "fxaa 1 1 fxaafd" "color 0 0 color" "color 1 0 colord"; do
  set -- $v
  cpp -P -DDITHER=$2 -DFXAA_FLOAT=$3 -Isrc/shaders src/shaders/$1.fcg >"$GEN/$4.fcg"
  fpc "$GEN/$4.fcg" "$GEN/$4.fpo"
done
{
    c_array "$GEN/quad.vpo" quad_vpo
    c_array "$GEN/blit.fpo" blit_fpo
    c_array "$GEN/fxaa.fpo" fxaa_fpo
    c_array "$GEN/blitd.fpo" blitd_fpo
    c_array "$GEN/fxaad.fpo" fxaad_fpo
    c_array "$GEN/fxaaf.fpo" fxaaf_fpo
    c_array "$GEN/fxaafd.fpo" fxaafd_fpo
    c_array "$GEN/color.fpo" color_fpo
    c_array "$GEN/colord.fpo" colord_fpo
} >"$GEN/shaders.h"
# hardware renderer: one fragment program per texture mode (+ texture window)
# two sets: byte-decoding programs (fallback) and fast ones (FAST=1: the texture unit decodes VRAM)
# vertex programs: byte-decoding and fast sets (the pass is a constant, "native")
for v in "0 hwr" "1 hwr_fast"; do
  set -- $v
  cpp -P -DFAST=$1 src/shaders/hwr.vcg >"$GEN/$2.vcg"; cgcomp -v "$GEN/$2.vcg" "$GEN/$2.vpo" >/dev/null
  c_array "$GEN/$2.vpo" "$2_vpo" >>"$GEN/shaders.h"
done
# + the mask-check variants (MCHK=1, names m...) and the bit-15 self copy (mode 5)
for F in 0 1; do
  for v in "0 0 0 f0" "1 0 0 f1" "2 0 0 f2" "3 0 0 f3" "4 0 0 f4" "1 1 0 f1w" "2 1 0 f2w" "3 1 0 f3w" \
           "0 0 1 mf0" "1 0 1 mf1" "2 0 1 mf2" "3 0 1 mf3" "4 0 1 mf4" "1 1 1 mf1w" "2 1 1 mf2w" "3 1 1 mf3w" "5 0 0 f5"; do
    set -- $v
    n=$4; [ $F = 1 ] && n=f$4
    cpp -P -DMODE=$1 -DTWIN=$2 -DMCHK=$3 -DFAST=$F src/shaders/hwr.fcg >"$GEN/hwr_$n.fcg"
    fpc "$GEN/hwr_$n.fcg" "$GEN/hwr_$n.fpo"
    c_array "$GEN/hwr_$n.fpo" "hwr_${n}_fpo" >>"$GEN/shaders.h"
  done
done
# + bilinear variants (FILT=1, names ...b): textured modes 1..3 only, with and
# without texture window and mask check, both program sets (24 programs).
# Flat/copy/self have no bilinear variant: hwr_rsx.c aliases them to nearest.
for F in 0 1; do
  for v in "1 0 0 f1b" "2 0 0 f2b" "3 0 0 f3b" "1 1 0 f1wb" "2 1 0 f2wb" "3 1 0 f3wb" \
           "1 0 1 mf1b" "2 0 1 mf2b" "3 0 1 mf3b" "1 1 1 mf1wb" "2 1 1 mf2wb" "3 1 1 mf3wb"; do
    set -- $v
    n=$4; [ $F = 1 ] && n=f$4
    cpp -P -DMODE=$1 -DTWIN=$2 -DMCHK=$3 -DFAST=$F -DFILT=1 src/shaders/hwr.fcg >"$GEN/hwr_$n.fcg"
    fpc "$GEN/hwr_$n.fcg" "$GEN/hwr_$n.fpo"
    c_array "$GEN/hwr_$n.fpo" "hwr_${n}_fpo" >>"$GEN/shaders.h"
  done
done
# + Performance variants (PERF=1, names ...p): fast mode 3 only, with and
# without texture window and mask check (4 programs). The backend needs the
# texture unit on LINEAR for these (see hwr_rsx.c use_prog); CLUT modes in
# Performance mode reuse the manual bilinear programs above, and the slow
# program set has no Performance variant at all (it aliases manual too).
for v in "3 0 0 f3p" "3 1 0 f3wp" "3 0 1 mf3p" "3 1 1 mf3wp"; do
  set -- $v
  n=f$4
  cpp -P -DMODE=$1 -DTWIN=$2 -DMCHK=$3 -DFAST=1 -DPERF=1 src/shaders/hwr.fcg >"$GEN/hwr_$n.fcg"
  fpc "$GEN/hwr_$n.fcg" "$GEN/hwr_$n.fpo"
  c_array "$GEN/hwr_$n.fpo" "hwr_${n}_fpo" >>"$GEN/shaders.h"
done

echo "== SPU programs"
# FMV decoding: the core's own MDEC routines (libpcsxcore/mdec_decode.h) on the SPUs
spu-gcc -O3 -Wall -Wno-unused-function -I"$CORE/libpcsxcore" -Ispu -I"$PSL1GHT/spu/include" spu/mdec_spu.c \
    -L"$PSL1GHT/spu/lib" -lsputhread -o "$GEN/mdec_spu.elf"
spu-strip "$GEN/mdec_spu.elf"
c_array "$GEN/mdec_spu.elf" mdec_spu_elf >"$GEN/spu_bins.h"

echo "== front-end"
CF="-O2 -Wall -Wno-unused-function -mcpu=cell -I$PSL1GHT/ppu/include -I$GEN -Isrc -Isrc/hwr \
    -I$CORE/deps/libretro-common/include -I$CORE/plugins/gpulib ${FE_CFLAGS:-}"
OBJS=""
for f in src/*.c src/hwr/*.c; do
    o=$B/obj/$(basename "$f" .c).o
    X=""; [ "$(basename "$f")" = spu_mdec.c ] && X="-Ispu -I$CORE/libpcsxcore"
    ppu-gcc $CF $X -c "$f" -o "$o"
    OBJS="$OBJS $o"
done

echo "== link"
ppu-gcc -mcpu=cell -Wl,--gc-sections -L$PSL1GHT/ppu/lib $OBJS "$B/libcore.a" \
    -lrsx -lgcm_sys -lio -laudio -lsysutil -lsysmodule -lrt -llv2 -lm -o "$B/$NAME.elf"
ppu-strip "$B/$NAME.elf" -o "$B/${NAME}_stripped.elf"
sprxlinker "$B/${NAME}_stripped.elf"
CID=UP0001-PCSR00002_00-0000000000000001
make_self_npdrm "$B/${NAME}_stripped.elf" "$B/pkg/USRDIR/EBOOT.BIN" $CID >/dev/null
ls -la "$B/pkg/USRDIR/"   # staging state in the log, visible even if the next check trips
test -s "$B/pkg/USRDIR/EBOOT.BIN" || { echo "ERROR: EBOOT.BIN missing after make_self_npdrm" >&2; exit 1; }
sfo --title "Another PCSX-ReARMed for PS3" --appid PCSR00002 -f "$PS3DEV/bin/sfo.xml" "$B/pkg/PARAM.SFO" >/dev/null
cp "$ROOT/data/ICON0.PNG" "$B/pkg/ICON0.PNG"   # the XMB icon (320x176, transparent background)
cp "$ROOT/data/PIC1.PNG" "$B/pkg/PIC1.PNG"     # the XMB background (1920x1080)
cp "$ROOT/data/ps1titles.txt" "$B/pkg/USRDIR/"   # disc titles (tools/make_titles.py, libretro-database, CC BY-SA 4.0)
cp -r "$ROOT/data/cheats" "$B/pkg/USRDIR/cheats"   # cheat database (duckstation/chtdb, one .cht file per serial; see README)
python3 "$PS3DEV/bin/pkg.py" -c $CID "$B/pkg/" "$B/$NAME.pkg" >/dev/null
ls -la "$B/$NAME.pkg"
"$PS3DEV/bin/pkg" --list "$B/$NAME.pkg" | grep -q "USRDIR/EBOOT.BIN" || { echo "ERROR: EBOOT.BIN missing from package" >&2; exit 1; }
"$PS3DEV/bin/pkg" --list "$B/$NAME.pkg" | grep -q "USRDIR/ps1titles.txt" || { echo "ERROR: ps1titles.txt missing from package" >&2; exit 1; }
