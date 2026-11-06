import struct,re,sys
def rv(p): d=open(p,'rb').read(); return struct.unpack('<%dH'%(len(d)//2), d)
def rppm(p):
    d=open(p,'rb').read(); m=re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", d); return int(m.group(1)),int(m.group(2)),d[m.end():]
soft=rv('soft.vram')
def t(r,g,b):
    if (r,g,b)==(31,31,31): return 'J'
    if (r,g,b)==(0,0,0): return '.'
    return f"{r+8},{g-4}"
tests=[("normal",16,16,32,16),("mir40-8",16,48,32,16),("mir39-7",16,80,32,16),("rect",16,112,32,16),("fliprect",16,144,32,16),("vmir",64,16,32,16),("2tiles",64,48,32,16),("2mirtiles",64,80,32,16)]
for s in map(int,sys.argv[1:]):
    w,h,px=rppm('gl%d.ppm'%s); bad=0
    for name,x0,y0,W,H in tests:
        nb=0; ex=None
        for y in range(y0-1,y0+H+1):
            for x in range(x0-1,x0+W+1):
                c=soft[y*1024+x]; st=t(c&31,(c>>5)&31,(c>>10)&31)
                for j in range(s*s):
                    o=((y*s+j//s)*w + x*s+j%s)*3; g=t(px[o]>>3,px[o+1]>>3,px[o+2]>>3)
                    if g!=st:
                        nb+=1
                        if ex is None: ex=(x-x0,y-y0,j,st,g)
        print(f"scale {s} {name:10s} subpixels differing from software: {nb}  first {ex}")
