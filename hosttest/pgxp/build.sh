#!/bin/sh
# builds cube.exe and cube_tex.exe (PS-X EXEs) with clang/lld for the R3000
set -e
cd "$(dirname "$0")"
build() {   # build <name> [defines]
N=$1; shift
clang "$@" --target=mipsel-none-elf -march=mips1 -mabi=32 -msoft-float -mno-abicalls -fno-pic -G0 -O1 \
  -ffreestanding -fno-builtin -nostdlib -ffunction-sections -c cube.c -o $N.o 2>&1 | grep -v "MIPS-I support" || true
ld.lld -T link.ld $N.o -o $N.elf
llvm-objcopy -O binary $N.elf $N.bin
N=$N python3 - <<'P'
import struct, os
N = os.environ['N']
b = open(N + '.bin','rb').read()
b += b'\0' * (-len(b) % 2048)
import subprocess
entry = int(subprocess.check_output(['llvm-nm', N + '.elf']).decode().split('_start')[0].split()[-2], 16)
h = bytearray(2048)
h[0:8] = b'PS-X EXE'
struct.pack_into('<IIIIIIIIIIII', h, 0x10, entry, 0, 0x80010000, len(b), 0, 0, 0, 0, 0x801ffff0, 0, 0, 0)
h[0x4c:0x4c+20] = b'Sony Computer Entertainment Inc. for North America area'[:20]
open(N + '.exe','wb').write(bytes(h) + b)
print(N + '.exe', len(b), 'bytes, entry %08x' % entry)
P
}
build cube
build cube_tex -DTEX
build cube_many -DTEX -DMANY
build cube_lag -DTEX -DLAG
build cube_swc -DTEX -DSWC
build cube_many_swc -DTEX -DMANY -DSWC
build cube_joint -DTEX -DSWC -DJOINT
build cube_many_joint -DTEX -DMANY -DSWC -DJOINT
