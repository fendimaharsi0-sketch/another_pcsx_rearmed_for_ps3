/* Hardware renderer: interface between the emulator core and a GPU backend.
 *
 * Two modes, chosen by the backend (hwr_dev.full):
 *  - full: the GPU draws every polygon, line, sprite and fill twice, into an
 *    upscaled copy of VRAM (displayed) and into a native-resolution copy that
 *    is the real VRAM: textures are sampled from it. The software renderer
 *    draws nothing; the backend syncs the core's VRAM for the few CPU-side
 *    accesses (uploads, copies, reads, savestates).
 *  - hybrid: the software renderer stays authoritative for PS1 VRAM; every
 *    primitive is also replayed on the GPU into the upscaled copy, texturing
 *    from snapshots of the core's VRAM.
 * Programs output alpha = the pixel's bit 15 (texel STP bit or the mask-set flag);
 * the full-mode backend stores it in the native VRAM.
 * In both, texture data is read as big-endian halfwords (the RSX surface
 * layout), and programs write PS1 red to the blue channel and blue to red (the
 * RSX 15-bit surface has red at the top; PS1 VRAM has blue there).
 *
 * hwr_core.c is backend-independent (host-testable); the RSX backend is
 * hwr_rsx.c and the OpenGL replay used by the tests is in hosttest/.
 */
#ifndef WS_HWR_H
#define WS_HWR_H
#include <stdint.h>

/* One vertex, 80 bytes. Values that are whole numbers are 16-bit integers the
 * GPU converts itself (RSX "S32K", GL_SHORT): the PPU pays for every int->float
 * conversion (load-hit-store) and for every byte written. The program inputs
 * are still float4s:
 *   POSITION  x, y, u, v        TEXCOORD0 r, g, b, z
 *   TEXCOORD1 tpx, tpy, cx, cy  TEXCOORD2 mode, raw, twmx, twmy
 *   TEXCOORD3 twox, twoy, pass, a   TEXCOORD4 umin, vmin, umax, vmax
 *   TEXCOORD5 px, py, pw, pad (PGXP visual-only, see below) */
typedef struct {
    float x, y, u, v;          /* native VRAM coords (offset applied), texel coords */
    float r, g, b, z;          /* vertex colour 0..255; z: depth for the native pass (set by the backend) */
    int16_t tpx, tpy, cx, cy;  /* texpage base (halfwords, lines), CLUT x/y */
    int16_t mode, raw, twmx, twmy; /* mode: 0 flat colour, 1 4bpp, 2 8bpp, 3 15bpp, 4 VRAM copy */
    int16_t twox, twoy, pass, a;   /* pass: 0 all texels, 1 only STP=0, 2 only STP=1;
                                      a: 1 = the mask-set flag (E6 bit 0) */
    int16_t umin, vmin, umax, vmax; /* texel range the primitive uses: sampling is clamped to it, so
                                       upscaled edges do not pick neighbouring texels (seams) */
    float px, py, pw, pad;     /* PGXP: precise VRAM coords + w (z/32768) for the
                                  upscaled pass only; px=x, py=y, pw=1 when no precise
                                  vertex was found (fallback = integer path). The
                                  shader uses x/y + w=1 in the native pass, so the
                                  native VRAM stays byte-exact whatever pgxp does. */
} hwr_vtx;

/* PGXP visual-only switch, set by the backend from the pgxp setting (see
 * common.h): 0 = off (vertices are integer, the original path), 1 = on
 * (upscaled pass uses px/py/pw where the GTE cache hit). Default off. */
extern int hwr_pgxp;

enum { HWR_BLEND_NONE = 0, HWR_BLEND_AVG, HWR_BLEND_ADD, HWR_BLEND_SUB, HWR_BLEND_ADDQ };

/* fragment program variants: 0 flat, 1 4bpp, 2 8bpp, 3 15bpp, 4 VRAM copy,
   5..7 = 4bpp/8bpp/15bpp with texture window; + HWR_PROG_MCHK: the same with the
   mask check (full mode); HWR_PROG_SELF: the backend's bit-15 self copy */
#define HWR_PROG_MCHK 8
#define HWR_PROG_SELF 16
#define HWR_NPROG 17
#define HWR_PROG_BASE(p) ((p) & 7)     /* the variant without the mask check */

typedef struct {
    uint8_t blend;               /* HWR_BLEND_* */
    uint8_t prog;                /* fragment program variant */
    int16_t sx0, sy0, sx1, sy1;  /* scissor, native coords, exclusive end */
    uint8_t pass;                /* vertex pass value of the whole op (0, 1, 2) */
    uint8_t mset;                /* mask-set flag: every pixel gets bit 15 */
    uint32_t first, count;       /* triangle-list vertices */
    int16_t bx0, by0, bx1, by1;  /* area the op's triangles cover (native, clipped to the scissor,
                                    exclusive end; empty if bx1 <= bx0) */
} hwr_op;

typedef struct hwr_dev {
    /* hybrid: copy native VRAM (1024x512 little-endian halfwords) for the
       following draws. full: make everything drawn so far visible to texturing */
    int  (*snapshot)(struct hwr_dev *d, const uint16_t *vram);
    void (*submit)(struct hwr_dev *d, const hwr_op *ops, int nops, const hwr_vtx *v, int nv);
    /* show VRAM rect (native coords) from the upscaled target; 0 = could not */
    int  (*present)(struct hwr_dev *d, int x, int y, int w, int h);
    int full;                    /* see above */
    int mask_stencil;            /* full mode: the backend does the mask check with the stencil
                                    in both passes (no barrier before mask-checked primitives) */
} hwr_dev;

/* 1 if the CLUT of a 4bpp (mode 1) or 8bpp (mode 2) texture has an entry with bit 15
 * (STP) set: only then can one of its texels give a pixel bit 15. vram: the core's
 * VRAM (host-endian halfwords), which the caller knows to be current there. The
 * backends skip the extra draws that set only those pixels (stencil mask plane,
 * bit-15 fix-up) for opaque textured ops whose CLUTs have none (Silent Hill: all). */
static inline int hwr_clut_stp(const uint16_t *vram, int mode, int cx, int cy)
{
    int n = mode == 1 ? 16 : 256, k;
    const uint16_t *row = vram + (cy & 511) * 1024;
    uint16_t m = 0;
    if ((cx & 1023) + n <= 1024) {
        const uint16_t *e = row + (cx & 1023);
        for (k = 0; k < n; k++) m |= e[k];
    } else
        for (k = 0; k < n; k++) m |= row[(cx + k) & 1023];
    return (m & 0x8000) != 0;
}

/* --- backend side --- */
void hwr_attach(hwr_dev *dev, const uint16_t *vram, void (*sync_cb)(void));
void hwr_attach_vram(const uint16_t *vram);
void hwr_detach(void);
int  hwr_active(void);

/* --- core side (called through the core's gpu_frontend_hooks) --- */
/* before the software renderer runs it; 1 = drawn here (full mode), skip it */
int  hwr_cmd(const uint32_t *list, const uint32_t *end);
void hwr_flush(void);                                     /* submit everything queued */
void hwr_vram_written(int x, int y, int w, int h);        /* upload, copy or savestate load */
void hwr_sync_ecmds(const uint32_t *ex_regs);             /* host-endian E1..E6 */
int  hwr_present(int x, int y, int w, int h, int bgr24);  /* 1 = frame shown by the GPU */
void hwr_shown_size(int *w, int *h);                      /* native size of the picture shown (0: none) */

/* statistics for the log */
typedef struct { unsigned frames, prims, snapshots, fallbacks, overflow_frames, max_snaps, mask_checks; } hwr_stats_t;
extern hwr_stats_t hwr_stats;

#endif
