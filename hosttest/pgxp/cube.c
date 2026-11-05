/* PGXP-lite test program: a slowly turning cube drawn with the GTE (RTPT) and
 * flat triangles sent straight to GP0. Its vertices move by fractions of a pixel
 * per frame, which the PS1 rounds to whole pixels (the wobble PGXP-lite fixes). */
#include "sintab.h"
#define GP0 (*(volatile unsigned *)0x1f801810)
#define GP1 (*(volatile unsigned *)0x1f801814)
#define ISTAT (*(volatile unsigned *)0x1f801070)
#define IMASK (*(volatile unsigned *)0x1f801074)

/* LLVM's MIPS-I assembler lacks the COP2 moves: encoded by hand, through $t0 */
#define CTC2(v, r) do { register unsigned t_ __asm__("$8") = (unsigned)(v); \
    __asm__ volatile(".word 0x48c80000 + (" #r " << 11)\n\tnop" : : "r"(t_)); } while (0)
#define MTC2(v, r) do { register unsigned t_ __asm__("$8") = (unsigned)(v); \
    __asm__ volatile(".word 0x48880000 + (" #r " << 11)\n\tnop" : : "r"(t_)); } while (0)
#define MFC2(v, r) do { register unsigned t_ __asm__("$8"); \
    __asm__ volatile(".word 0x48080000 + (" #r " << 11)\n\tnop\n\tnop" : "=r"(t_)); (v) = t_; } while (0)
/* SWC2 rt, 0(ptr): the GTE's register straight to memory, through $t1 */
#define SWC2(r, ptr) do { register unsigned *p_ __asm__("$9") = (ptr); \
    __asm__ volatile(".word 0xe9200000 + (" #r " << 16)\n\tnop" : : "r"(p_) : "memory"); } while (0)
#define RTPT() __asm__ volatile("nop\n\tnop\n\t.word 0x4a280030\n\tnop\n\tnop" ::: "memory")
#define NCLIP() __asm__ volatile("nop\n\tnop\n\t.word 0x4b400006\n\tnop\n\tnop" ::: "memory")

#ifdef MANY
#define S 40
#else
#define S 300
#endif
static const short V[8][3] = {
    {-S,-S,-S},{S,-S,-S},{S,S,-S},{-S,S,-S},
    {-S,-S, S},{S,-S, S},{S,S, S},{-S,S, S} };
static const unsigned char F[12][3] = {
    {0,1,2},{0,2,3},{5,4,7},{5,7,6},{4,0,3},{4,3,7},{1,5,6},{1,6,2},{4,5,1},{4,1,0},{3,2,6},{3,6,7} };
static const unsigned C[6] = { 0x3040c0, 0x30c040, 0xc04030, 0xc0c030, 0xc030c0, 0x30c0c0 };

#ifdef TEX
/* a 64x64 15-bit checkerboard at VRAM 640,0 (texpage x 10, 15-bit) */
static void upload_texture(void)
{
    int x, y;
    GP0 = 0xa0000000; GP0 = (0 << 16) | 640; GP0 = (64 << 16) | 64;
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x += 2) {
            unsigned a = (((x >> 3) ^ (y >> 3)) & 1) ? 0x7fff : 0x1084;
            unsigned b = ((((x + 1) >> 3) ^ (y >> 3)) & 1) ? 0x7fff : 0x1084;
            GP0 = a | (b << 16);
        }
}
#endif

static void wait_vblank(void)
{
    ISTAT = ~1u;
    while (!(ISTAT & 1)) ;
}

static void matrix(int a, int b)
{
    /* rotation about Y by a, then about X by b (4.12) */
    int ca = sintab[(a + 64) & 255], sa = sintab[a & 255];
    int cb = sintab[(b + 64) & 255], sb = sintab[b & 255];
    int r11 = ca, r12 = 0, r13 = sa;
    int r21 = (sa * sb) >> 12, r22 = cb, r23 = -((ca * sb) >> 12);
    int r31 = -((sa * cb) >> 12), r32 = sb, r33 = (ca * cb) >> 12;
    CTC2((r11 & 0xffff) | (r12 << 16), 0);
    CTC2((r13 & 0xffff) | (r21 << 16), 1);
    CTC2((r22 & 0xffff) | (r23 << 16), 2);
    CTC2((r31 & 0xffff) | (r32 << 16), 3);
    CTC2(r33, 4);
}

void main(void)
{
    unsigned t = 0;
    unsigned sr;
    __asm__ volatile("mfc0 %0, $12\n\tnop" : "=r"(sr));
    sr |= 0x40000000;                       /* COP2 on */
    __asm__ volatile("mtc0 %0, $12\n\tnop" : : "r"(sr));
    IMASK = 0;
    GP1 = 0x00000000;                       /* reset */
    GP1 = 0x03000000;                       /* display on */
    GP1 = 0x08000001;                       /* 320x240, NTSC, 15-bit */
    GP1 = 0x06c60260;                       /* horizontal range */
    GP1 = 0x07040010;                       /* vertical range */
    GP1 = 0x05000000;                       /* display start 0,0 */
    GP0 = 0xe1000400;                       /* draw to the display area allowed */
    GP0 = 0xe3000000;                       /* draw area 0,0 .. */
    GP0 = 0xe4000000 | (239 << 10) | 319;
    GP0 = 0xe5000000;                       /* offset 0 */
#ifdef TEX
    upload_texture();
#endif
#ifdef TEX
    CTC2(0, 5); CTC2(0, 6); CTC2(1100, 7);  /* close: strong perspective */
#else
    CTC2(0, 5); CTC2(0, 6); CTC2(1500, 7);  /* translation: 1500 in front */
#endif
    CTC2(160 << 16, 24); CTC2(120 << 16, 25);   /* screen centre */
    CTC2(250, 26);                          /* projection distance */
#ifdef LAG
    /* like a 15-30 fps game: the packets drawn now were projected during the previous
       frame, spread over its vblanks, and the display flips between two buffers */
    static unsigned pk[2][12 * 7];
    static int np[2];
    int cur = 0;
#endif
#ifdef SWC
#ifdef MANY
    static unsigned ll[100 * 12 * 8];
#else
    static unsigned ll[12 * 8];
#endif
    int nl;
#endif
    for (;;) {
        int f, trx = 0;
#ifdef SWC
        nl = 0;
        {   /* two display buffers, flipped each frame (as games do) */
            static int cb;
            cb ^= 1;
            wait_vblank();
            GP1 = 0x05000000 | ((cb ? 0 : 256) << 10);
            GP0 = 0xe3000000 | ((cb ? 256 : 0) << 10);
            GP0 = 0xe4000000 | (((cb ? 256 : 0) + 239) << 10) | 319;
            GP0 = 0xe5000000 | ((cb ? 256 : 0) << 11);
            GP0 = 0x02101010; GP0 = (cb ? 256 : 0) << 16; GP0 = (240 << 16) | 320;
        }
#endif
#ifdef LAG
        int k;
        wait_vblank();
        GP1 = 0x05000000 | ((cur ? 0 : 256) << 10);   /* show the buffer drawn last time */
        GP0 = 0xe3000000 | ((cur ? 256 : 0) << 10);   /* draw into the other one */
        GP0 = 0xe4000000 | (((cur ? 256 : 0) + 239) << 10) | 319;
        GP0 = 0xe5000000 | ((cur ? 256 : 0) << 11);
        GP0 = 0x02101010; GP0 = (cur ? 256 : 0) << 16; GP0 = (240 << 16) | 320;
        for (k = 0; k < np[cur ^ 1]; k++) GP0 = pk[cur ^ 1][k];   /* last frame's packets, from RAM */
        np[cur] = 0;
#elif !defined(SWC)
        wait_vblank();
        GP0 = 0x02101010; GP0 = 0; GP0 = (240 << 16) | 320;   /* clear */
#endif
#ifdef TEX
        matrix(t + 24, 36);                 /* turning, close: strong perspective */
#else
        matrix(20, 28);
        CTC2(t - 200, 5);                   /* drifts right by 1/6 pixel per frame */
#endif
#ifdef MANY
        {   /* stress: 10x10 small cubes, about 3600 projected vertices per frame */
        int c;
        for (c = 0; c < 100; c++) {
        trx = ((c % 10) - 5) * 140 + (int)(t & 63);
        CTC2(trx, 5); CTC2(((c / 10) - 5) * 110, 6); CTC2(1800 + (c & 3) * 200, 7);
#endif
        for (f = 0; f < 12; f++) {
            const short *a = V[F[f][0]], *b = V[F[f][1]], *c = V[F[f][2]];
            unsigned s0, s1, s2; int n;
#ifdef LAG
            CTC2(1100 + f * 9, 7);   /* each face its own corners (no re-projection refreshes them) */
#endif
#ifdef JOINT
            /* each face projects its own copy of its corners a fifth of a pixel apart
               (body parts that meet at a joint): welded or merged into one place */
            CTC2(trx + (f % 3) - 1, 5);
#endif
            MTC2((a[0] & 0xffff) | (a[1] << 16), 0); MTC2(a[2], 1);
            MTC2((b[0] & 0xffff) | (b[1] << 16), 2); MTC2(b[2], 3);
            MTC2((c[0] & 0xffff) | (c[1] << 16), 4); MTC2(c[2], 5);
            RTPT();
            NCLIP();
            MFC2(n, 24);
            if (n >= 0) continue;           /* draws the far faces: the inside of a box, big walls in perspective */
            MFC2(s0, 12); MFC2(s1, 13); MFC2(s2, 14);
#ifdef SWC
            {   /* a linked list in RAM, the screen coordinates stored by SWC2 (like most
                   games), sent by DMA */
                unsigned *q = &ll[nl * 8];
                q[0] = (7u << 24) | ((unsigned)(q + 8) & 0xffffff);
                q[1] = 0x25808080;
                SWC2(12, q + 2); q[3] = 0;
                SWC2(13, q + 4); q[5] = (0x10a << 16) | (f & 1 ? 0x3f3f : 0x003f);
                SWC2(14, q + 6); q[7] = f & 1 ? 0x3f00 : 0x3f3f;
                nl++; }
            continue;
#endif
#ifdef LAG
            {   unsigned *q = &pk[cur][np[cur]];
                q[0] = 0x25808080; q[1] = s0; q[2] = 0;
                q[3] = s1; q[4] = (0x10a << 16) | (f & 1 ? 0x3f3f : 0x003f);
                q[5] = s2; q[6] = f & 1 ? 0x3f00 : 0x3f3f;
                np[cur] += 7; }
            wait_vblank();   /* the CPU spends several vblanks on a frame: the first faces wait longest */
            continue;
#endif
#ifdef TEX
            /* raw textured triangle; each face is a quad split 0-1-2 / 0-2-3 */
            GP0 = 0x25808080;
            GP0 = s0; GP0 = 0;
            GP0 = s1; GP0 = (0x10a << 16) | (f & 1 ? 0x3f3f : 0x003f);
            GP0 = s2; GP0 = f & 1 ? 0x3f00 : 0x3f3f;
#else
            GP0 = 0x20000000 | C[f >> 1];
            GP0 = s0; GP0 = s1; GP0 = s2;
#endif
        }
#ifdef MANY
        }
        }
#endif
#ifdef LAG
        cur ^= 1;
#endif
#ifdef SWC
        if (nl) {
            ll[(nl - 1) * 8] |= 0xffffff;   /* end of the list */
            *(volatile unsigned *)0x1f8010f0 |= 0x800;   /* DMA2 on */
            GP1 = 0x04000002;
            *(volatile unsigned *)0x1f8010a0 = (unsigned)ll & 0xffffff;
            *(volatile unsigned *)0x1f8010a4 = 0;
            *(volatile unsigned *)0x1f8010a8 = 0x01000401;
            while (*(volatile unsigned *)0x1f8010a8 & 0x01000000) ;
        }
#endif
        t++;
    }
}

void _start(void)
{
    __asm__ volatile("la $sp, 0x801ffff0");
    main();
}
