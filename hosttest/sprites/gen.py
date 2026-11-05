import struct, sys
vram = [0]*(1024*512)
def rgb(r,g,b): return (r&31)|((g&31)<<5)|((b&31)<<10)
# texture page at x=512: sprite texels u 8..39, v 0..15; everything else bright white (junk)
for v in range(64):
    for u in range(64):
        c = rgb(31,31,31)
        if 8 <= u < 40 and v < 16: c = rgb((u-8)%32, 4+ (v%16), 31-((u-8)%32)) | 0  # gradient, never 0
        vram[v*1024+512+u] = c
TP = 8 | (2<<7)
def quad(x,y,w,h,uL,uR,vT,vB):
    return [0x2D808080, (y<<16)|x, (vT<<8)|uL, (y<<16)|(x+w), (TP<<16)|(vT<<8)|uR,
            ((y+h)<<16)|x, (vB<<8)|uL, ((y+h)<<16)|(x+w), (vB<<8)|uR]
def rect(x,y,w,h,u,v): return [0x65808080, (y<<16)|x, (v<<8)|u, (h<<16)|w]
cmds = []
cmds.append([0xE1000108]+quad(16,16,32,16, 8,40, 0,16))      # normal
cmds.append(quad(16,48,32,16, 40,8, 0,16))                   # mirrored, left u=40 right u=8
cmds.append(quad(16,80,32,16, 39,7, 0,16))                   # mirrored, left u=39 right u=7
cmds.append(rect(16,112,32,16, 8,0))                         # rect
cmds.append([0xE1001108]+rect(16,144,32,16, 39,0)+[0xE1000108])  # x-flipped rect
cmds.append(quad(64,16,32,16, 8,40, 16,0))                   # v mirrored
cmds.append(quad(64,48,16,16, 8,24, 0,16)+quad(80,48,16,16, 24,40, 0,16))   # two tiles side by side
cmds.append(quad(64,80,16,16, 24,8, 0,16)+quad(80,80,16,16, 40,24, 0,16))   # two mirrored tiles (sprite turned left)
out = [0x54305047, 1]
for i in range(0,1024*512,2): out.append(vram[i] | (vram[i+1]<<16))
out += [0, 0xE1000108, 0xE2000000, 0xE3000000, 0xE4000000|(511<<10)|1023, 0xE5000000, 0xE6000000, 0]
for c in cmds:
    # one packet per command
    i = 0
    while i < len(c):
        w = c[i]; op = w>>24
        n = 1 if op in (0xE1,) else (9 if op==0x2D else 4)
        out += [1, n] + c[i:i+n]; i += n
out += [3, 0, 0, 320, 240]
open(sys.argv[1],'wb').write(struct.pack('<%dI'%len(out), *out))
