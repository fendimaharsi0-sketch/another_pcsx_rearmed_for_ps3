/* Hybrid hardware renderer, backend-independent part.
 * See hwr.h for the overall design. This file parses GP0 drawing commands,
 * tracks the drawing state, turns primitives into triangle lists and decides
 * when a fresh snapshot of native VRAM is needed. */
#include <string.h>
#include <math.h>
#include "hwr.h"

#define VCAP      65536   /* vertices per submit */
#define SCAP      32768   /* staged second-pass vertices */
#define OPCAP     4096
/* queued vertices that trigger a submit mid-frame, so the GPU draws while the
   CPU keeps emulating instead of getting the whole frame at the end */
#define CHUNK     2048
#define MAXDIRTY  32
#define MAXSNAPS  12      /* per frame; beyond this the frame falls back to software */

hwr_stats_t hwr_stats;

static hwr_dev *dev;
static const uint16_t *vram;
static void (*sync_cb)(void);
static int full_mode;                  /* dev->full at attach */

/* where a polygon's time goes (all of do_poly, then its parts; for the log) */
unsigned long long hwr_poly_t_all, hwr_poly_t_parse, hwr_poly_t_shift, hwr_poly_t_setup, hwr_poly_t_emit;
unsigned int hwr_poly_n;
/* only one polygon in PG_SAMPLE is timed (the log multiplies): reading the
   timebase costs about as much as a short step, ten times per polygon.
   Timed only while the debug log runs (psx_debug_trace, set at game start):
   with it off, no timebase is ever read here. */
#define PG_SAMPLE 8
static int pg_timed;
extern int psx_debug_trace;   /* libpcsxcore/misc.c: front end debug log */
#if defined(__powerpc__) || defined(__PPC__)
#define PG_TB() (pg_timed ? __builtin_ppc_get_timebase() : 0ull)
#else
#define PG_TB() 0ull
#endif

typedef struct { float x, y, u, v, r, g, b, ox, oy, ow; int16_t qx, qy, qz, qpad; } pv;   /* a primitive's vertex while it is set up;
                                                     ox, oy, ow: PGXP-lite offset and depth (do_poly only),
                                                     qx, qy, qz: the same as the vertex's pgx, pgy, pgz */

/* PGXP-lite (core: libpcsxcore/pgxp_lite.c), weak so the host tools link without the core */
extern int pgxp_lite_on __attribute__((weak));
int pgxp_lite_find(uint32_t sxy, float *dx, float *dy, float *z) __attribute__((weak));
struct pgxp_cand { float dx, dy, z; uint32_t seq; };   /* as in libpcsxcore/pgxp_lite.h */
int pgxp_lite_cand_at(const void *ptr, uint32_t word, struct pgxp_cand *c) __attribute__((weak));
int pgxp_lite_cands(uint32_t sxy, struct pgxp_cand *c) __attribute__((weak));
void pgxp_lite_prefetch(const uint32_t *const *ptr, const uint32_t *words, int n) __attribute__((weak));
static int pg_poly;                    /* the primitive being emitted has PGXP-lite offsets */
unsigned int hwr_pgxp_polys, hwr_pgxp_misses, hwr_pgxp_persp, hwr_pgxp_flat, hwr_pgxp_misfit, hwr_pgxp_alone, hwr_pgxp_amb;   /* polygons drawn with / without them (log) */
int hwr_pgxp_first;                    /* host tests: the first vertex of a word, as before build 86 */
float hwr_pgxp_grow = 0.5f;            /* PGXP seam fill: solid polygons drawn this much (pixels) larger */
unsigned int hwr_pgxp_grown;           /* PGXP seam fill: polygons drawn larger (log) */
/* where PGXP's time goes in the renderer (timebase ticks, PowerPC; for the log) */
unsigned long long hwr_pgxp_t_find, hwr_pgxp_t_pick, hwr_pgxp_t_depth, hwr_pgxp_t_seam;

/* Small integers to float from tables. On the PS3's PPU an integer reaches a
   floating-point register only through memory, and loading it straight after
   storing it waits for the store (a "load-hit-store", about 40 cycles): a
   polygon's corners, texture coordinates and colours were 7 of them per corner.
   The tables give the same, exact values. */
static float f_byte[256];               /* 0..255 */
static float f_s11hi[64], f_s11lo[32];  /* an 11-bit signed number: hi[v >> 5 & 63] + lo[v & 31] */
static void cvt_init(void)
{
    int i;
    for (i = 0; i < 256; i++) f_byte[i] = (float)i;
    for (i = 0; i < 64; i++) f_s11hi[i] = (float)(((i << 5) ^ 1024) - 1024);
    for (i = 0; i < 32; i++) f_s11lo[i] = (float)i;
}
static inline float s11f(uint32_t v) { return f_s11hi[(v >> 5) & 63] + f_s11lo[v & 31]; }

static hwr_vtx vb_own[VCAP], sv[SCAP];
/* the vertices of the batch being built: in the backend's memory when it offers
   that (vtx_area: the RSX reads them from there; they were copied, a millisecond
   per frame in 3D games on the PS3), else in vb_own. NULL between batches. */
static hwr_vtx *vb;
static int vcap, vzero;                 /* vertices vb holds; bytes of it already claimed (dcbz) */
static int nv, nsv;
static hwr_op ops[OPCAP];
static int nops;

/* drawing state */
static struct {
    int tpx, tpy, abr, depth;          /* texpage: halfword x base, y base, blend, colour depth */
    int flipx, flipy;                  /* textured-rectangle mirroring (E1 bits 12, 13 only) */
    int twmx, twmy, twox, twoy;        /* texture window */
    int dx0, dy0, dx1, dy1;            /* draw area, inclusive */
    int ox, oy;                        /* draw offset */
    float oxf, oyf;                    /* the same as floats */
    int mset, mchk;                    /* E6: set bit 15 on drawn pixels, skip pixels with bit 15 */
} st;

/* batch = run of draws that sample the same VRAM snapshot */
static int batch_open, snapped;
static int bx0, by0, bx1, by1;         /* area drawn in this batch (native, exclusive end) */
static struct { int x0, y0, x1, y1; } dirty[MAXDIRTY];
static int ndirty;
static int frame_snaps, frame_overflow;

/* second-pass group for semi-transparent textured primitives */
static int grp_open, grp_abr, grp_prog, grp_mset, grp_spr, grp_sc[4];
static int gb[4];                       /* the open group's covered area (ints, see hwr_op) */
static int pb[4];                       /* the last emitted primitive's area */
static float gx0, gy0, gx1, gy1;        /* area covered by the open group */
static int cur_prog;                   /* program variant of the primitive being emitted */
static int cur_spr;                    /* it is a textured sprite (the sprite filter), set before emit() */
/* the backend: sprites are drawn differently from other textured primitives (its
   sprite and texture filters differ); else they are not told apart at all */
int hwr_sprites_apart;

static inline uint32_t le32(const uint32_t *p)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return __builtin_bswap32(*p);
#else
    return *p;
#endif
}
static inline int sx11(uint32_t v) { return (int)(v << 21) >> 21; }

static void set_texpage(uint32_t v)
{
    st.tpx = (v & 15) * 64;
    st.tpy = ((v >> 4) & 1) * 256;
    st.abr = (v >> 5) & 3;
    st.depth = (v >> 7) & 3;
}

static void ecmd(uint32_t w)
{
    switch (w >> 24) {
    case 0xe1: set_texpage(w); st.flipx = (w >> 12) & 1; st.flipy = (w >> 13) & 1; break;
    case 0xe2: st.twmx = w & 31; st.twmy = (w >> 5) & 31; st.twox = (w >> 10) & 31; st.twoy = (w >> 15) & 31; break;
    case 0xe3: st.dx0 = w & 1023; st.dy0 = (w >> 10) & 511; break;
    case 0xe4: st.dx1 = w & 1023; st.dy1 = (w >> 10) & 511; break;
    case 0xe5: st.ox = sx11(w); st.oy = sx11(w >> 11); st.oxf = s11f(w); st.oyf = s11f(w >> 11); break;
    case 0xe6: st.mset = w & 1; st.mchk = (w >> 1) & 1; break;
    }
}

void hwr_sync_ecmds(const uint32_t *ex)
{
    int i;
    for (i = 1; i <= 6; i++) ecmd((ex[i] & 0x00ffffff) | ((0xe0u + i) << 24));
}

/* ---------------- ops and vertices ---------------- */

static void close_group(void)
{
    hwr_op *o;
    if (!grp_open) return;
    grp_open = 0;
    if (!nsv) return;
    o = &ops[nops++];
    o->blend = (uint8_t)(grp_abr + 1);
    o->prog = (uint8_t)grp_prog;
    o->pass = 2; o->mset = (uint8_t)grp_mset; o->sprite = (uint8_t)grp_spr;
    o->sx0 = grp_sc[0]; o->sy0 = grp_sc[1]; o->sx1 = grp_sc[2]; o->sy1 = grp_sc[3];
    o->first = nv; o->count = nsv;
    o->bx0 = (int16_t)gb[0]; o->by0 = (int16_t)gb[1]; o->bx1 = (int16_t)gb[2]; o->by1 = (int16_t)gb[3];
    memcpy(vb + nv, sv, nsv * sizeof *sv);
    nv += nsv; nsv = 0;
}

static void submit(void)
{
    close_group();
    if (nops && dev) dev->submit(dev, ops, nops, vb, nv);
    nops = 0; nv = 0;
    vb = NULL;                          /* the backend's area is used up */
}

static void reserve(int n)
{
    if (vb && (nv + nsv + 2 * n > vcap || nsv + n > SCAP || nops + 3 > OPCAP)) submit();
    if (!vb) {
        if (dev && dev->vtx_area) vb = dev->vtx_area(dev, &vcap);
        if (!vb || vcap < 64) { vb = vb_own; vcap = VCAP; }
        vzero = 0;
    }
#if defined(__powerpc__) || defined(__PPC__)
    if (vb != vb_own) {
        /* the lines about to be written are claimed with dcbz: a plain store into
           a line the cache does not hold reads it from memory first */
        int need = (nv + nsv + 2 * n) * (int)sizeof *vb;
        for (; vzero < need; vzero += 128) __asm__ volatile("dcbz 0,%0" : : "r"((char *)vb + vzero) : "memory");
    }
#endif
}

static hwr_op *op_for(int blend, const int *sc)
{
    hwr_op *o = nops ? &ops[nops - 1] : 0;
    close_group();
    o = nops ? &ops[nops - 1] : 0;
    if (o && o->blend == blend && o->prog == cur_prog && o->sx0 == sc[0] && o->sy0 == sc[1] && o->sx1 == sc[2] && o->sy1 == sc[3]
        && o->pass == 0 && o->mset == st.mset && o->sprite == cur_spr && o->first + o->count == (uint32_t)nv)
        return o;
    o = &ops[nops++];
    o->blend = (uint8_t)blend;
    o->prog = (uint8_t)cur_prog;
    o->pass = 0; o->mset = (uint8_t)st.mset; o->sprite = (uint8_t)cur_spr;
    o->sx0 = sc[0]; o->sy0 = sc[1]; o->sx1 = sc[2]; o->sy1 = sc[3];
    o->first = nv; o->count = 0;
    o->bx0 = o->by0 = 32767; o->bx1 = o->by1 = -32768;
    return o;
}

static void op_area(hwr_op *o, const int *b)
{
    if (b[2] <= b[0] || b[3] <= b[1]) return;
    if (b[0] < o->bx0) o->bx0 = (int16_t)b[0];
    if (b[1] < o->by0) o->by0 = (int16_t)b[1];
    if (b[2] > o->bx1) o->bx1 = (int16_t)b[2];
    if (b[3] > o->by1) o->by1 = (int16_t)b[3];
}

/* area of n vertices clipped to the scissor (exclusive end) into pb: min/max in
   float, then 4 conversions (each costs a load-hit-store on the PPU); truncation
   is monotonic, so this equals converting every vertex */
static void prim_area(const pv *p, int n, const int *sc)
{
    int i;
    float fx0 = p[0].x, fy0 = p[0].y, fx1 = p[0].x, fy1 = p[0].y;
    for (i = 1; i < n; i++) {
        if (p[i].x < fx0) fx0 = p[i].x;
        if (p[i].y < fy0) fy0 = p[i].y;
        if (p[i].x > fx1) fx1 = p[i].x;
        if (p[i].y > fy1) fy1 = p[i].y;
    }
    pb[0] = (int)fx0; pb[1] = (int)fy0; pb[2] = (int)fx1 + 1; pb[3] = (int)fy1 + 1;
    if (pb[0] < sc[0]) pb[0] = sc[0];
    if (pb[1] < sc[1]) pb[1] = sc[1];
    if (pb[2] > sc[2]) pb[2] = sc[2];
    if (pb[3] > sc[3]) pb[3] = sc[3];
}

static hwr_vtx tmpl;

static void set_prog(void)
{
    int m = tmpl.mode;
    cur_prog = (m >= 1 && m <= 3 && (tmpl.twmx > 0 || tmpl.twmy > 0)) ? m + 4 : m;
    /* mask check (E6 bit 1): the programs read bit 15 of the pixel they draw over
       from the native VRAM; full mode only (hybrid has no GPU-side VRAM to read) */
    if (full_mode && st.mchk && m != 4) cur_prog += HWR_PROG_MCHK;
}

/* tmpl.pass and tmpl.a are set by emit() for the vertices it writes */
static void put(hwr_vtx *d, const pv *p)
{
    *d = tmpl;
    d->x = p->x; d->y = p->y; d->u = p->u; d->v = p->v;
    d->r = p->r; d->g = p->g; d->b = p->b;
    if (pg_poly) {
        d->pgx = p->qx; d->pgy = p->qy; d->pgz = p->qz;   /* (see do_poly) */
    }
}

/* p: n vertices forming n/3 triangles */
static void emit(const pv *p, int n, int semi, int textured, const int *sc)
{
    int i;
    if (st.mchk && tmpl.mode != 4) hwr_stats.mask_checks++;   /* counted for the log */
    reserve(n);
    set_prog();
    prim_area(p, n, sc);
    if (pg_poly) {   /* the upscaled pass may draw up to 2 pixels from the native one */
        pb[0] -= 2; pb[1] -= 2; pb[2] += 2; pb[3] += 2;
        if (pb[0] < sc[0]) pb[0] = sc[0];
        if (pb[1] < sc[1]) pb[1] = sc[1];
        if (pb[2] > sc[2]) pb[2] = sc[2];
        if (pb[3] > sc[3]) pb[3] = sc[3];
    }
    tmpl.a = st.mset ? 1 : 0;
    tmpl.z = 0.f;
    if (semi && textured) {
        hwr_op *o;
        float px0 = p[0].x, py0 = p[0].y, px1 = p[0].x, py1 = p[0].y;
        for (i = 1; i < n; i++) {
            if (p[i].x < px0) px0 = p[i].x;
            if (p[i].x > px1) px1 = p[i].x;
            if (p[i].y < py0) py0 = p[i].y;
            if (p[i].y > py1) py1 = p[i].y;
        }
        /* all pass-1 halves of a group are drawn before all pass-2 halves; that is only
           the right order while the group's primitives do not overlap each other */
        if (grp_open && px0 < gx1 && px1 > gx0 && py0 < gy1 && py1 > gy0) close_group();
        if (!grp_open || grp_abr != st.abr || grp_prog != cur_prog || grp_mset != st.mset || grp_spr != cur_spr || memcmp(grp_sc, sc, sizeof grp_sc)) {
            close_group();
            o = &ops[nops++];
            o->blend = HWR_BLEND_NONE;
            o->prog = (uint8_t)cur_prog;
            o->pass = 1; o->mset = (uint8_t)st.mset; o->sprite = (uint8_t)cur_spr;
            o->sx0 = sc[0]; o->sy0 = sc[1]; o->sx1 = sc[2]; o->sy1 = sc[3];
            o->first = nv; o->count = 0;
            o->bx0 = o->by0 = 32767; o->bx1 = o->by1 = -32768;
            grp_open = 1; grp_abr = st.abr; grp_prog = cur_prog; grp_mset = st.mset; grp_spr = cur_spr; memcpy(grp_sc, sc, sizeof grp_sc);
            gx0 = gy0 = 1e9f; gx1 = gy1 = -1e9f;
            gb[0] = gb[1] = 32767; gb[2] = gb[3] = -32768;
        }
        if (pb[2] > pb[0] && pb[3] > pb[1]) {
            if (pb[0] < gb[0]) gb[0] = pb[0];
            if (pb[1] < gb[1]) gb[1] = pb[1];
            if (pb[2] > gb[2]) gb[2] = pb[2];
            if (pb[3] > gb[3]) gb[3] = pb[3];
        }
        if (px0 < gx0) gx0 = px0;
        if (py0 < gy0) gy0 = py0;
        if (px1 > gx1) gx1 = px1;
        if (py1 > gy1) gy1 = py1;
        o = &ops[nops - 1];
        op_area(o, pb);
        tmpl.pass = 1;
        for (i = 0; i < n; i++) put(&vb[nv++], &p[i]);
        tmpl.pass = 2;
        for (i = 0; i < n; i++) put(&sv[nsv++], &p[i]);
        o->count += n;
    } else {
        hwr_op *o = op_for(semi ? st.abr + 1 : HWR_BLEND_NONE, sc);
        op_area(o, pb);
        tmpl.pass = 0;
        for (i = 0; i < n; i++) put(&vb[nv++], &p[i]);
        o->count += n;
    }
}

/* ---------------- batches and snapshots ---------------- */

static void close_batch(void)
{
    if (!batch_open) return;
    submit();
    batch_open = 0;
}

static void open_batch(void)
{
    batch_open = 1; snapped = 0;
    bx0 = by0 = 1 << 20; bx1 = by1 = -(1 << 20);
}

static int take_snapshot(void)
{
    int i;
    static const int full[4] = { 0, 0, 1024, 512 };
    hwr_vtx keep;
    if (frame_overflow) return 0;
    if (!full_mode && frame_snaps >= MAXSNAPS) { frame_overflow = 1; hwr_stats.overflow_frames++; return 0; }
    submit();                      /* draws queued so far use the previous snapshot */
    if (sync_cb) sync_cb();        /* SPU renderer must have finished writing VRAM */
    if (!dev->snapshot(dev, vram)) { frame_overflow = 1; return 0; }
    frame_snaps++; hwr_stats.snapshots++;
    snapped = 1;
    /* copy areas written outside the GPU path (uploads, copies, fills) into the upscaled target */
    keep = tmpl;                   /* the caller's primitive keeps its texture setup */
    memset(&tmpl, 0, sizeof tmpl);
    tmpl.mode = 4;
    for (i = 0; i < ndirty; i++) {
        float x0 = dirty[i].x0, y0 = dirty[i].y0, x1 = dirty[i].x1, y1 = dirty[i].y1;
        pv q[6] = {
            { x0, y0, x0, y0, 0, 0, 0 }, { x1, y0, x1, y0, 0, 0, 0 }, { x0, y1, x0, y1, 0, 0, 0 },
            { x1, y0, x1, y0, 0, 0, 0 }, { x0, y1, x0, y1, 0, 0, 0 }, { x1, y1, x1, y1, 0, 0, 0 } };
        cur_spr = 0; emit(q, 6, 0, 0, full);
    }
    ndirty = 0;
    tmpl = keep;
    return 1;
}

static void add_dirty1(int x0, int y0, int x1, int y1)
{
    int i;
    if (x1 <= x0 || y1 <= y0) return;
    for (i = 0; i < ndirty; i++)       /* contained in an existing rect? */
        if (x0 >= dirty[i].x0 && y0 >= dirty[i].y0 && x1 <= dirty[i].x1 && y1 <= dirty[i].y1) return;
    if (ndirty == MAXDIRTY) {           /* merge everything into one */
        for (i = 1; i < ndirty; i++) {
            if (dirty[i].x0 < dirty[0].x0) dirty[0].x0 = dirty[i].x0;
            if (dirty[i].y0 < dirty[0].y0) dirty[0].y0 = dirty[i].y0;
            if (dirty[i].x1 > dirty[0].x1) dirty[0].x1 = dirty[i].x1;
            if (dirty[i].y1 > dirty[0].y1) dirty[0].y1 = dirty[i].y1;
        }
        ndirty = 1;
        if (x0 < dirty[0].x0) dirty[0].x0 = x0;
        if (y0 < dirty[0].y0) dirty[0].y0 = y0;
        if (x1 > dirty[0].x1) dirty[0].x1 = x1;
        if (y1 > dirty[0].y1) dirty[0].y1 = y1;
        return;
    }
    dirty[ndirty].x0 = x0; dirty[ndirty].y0 = y0; dirty[ndirty].x1 = x1; dirty[ndirty].y1 = y1;
    ndirty++;
}

/* rect in VRAM coordinates that may wrap around the 1024x512 edges */
static void add_dirty(int x, int y, int w, int h)
{
    int xs[2][2], ys[2][2], nx = 1, ny = 1, i, j;
    if (w <= 0 || h <= 0) return;
    if (w > 1024) w = 1024;
    if (h > 512) h = 512;
    x &= 1023; y &= 511;
    xs[0][0] = x; xs[0][1] = x + w;
    if (x + w > 1024) { xs[0][1] = 1024; xs[1][0] = 0; xs[1][1] = x + w - 1024; nx = 2; }
    ys[0][0] = y; ys[0][1] = y + h;
    if (y + h > 512) { ys[0][1] = 512; ys[1][0] = 0; ys[1][1] = y + h - 512; ny = 2; }
    for (i = 0; i < nx; i++)
        for (j = 0; j < ny; j++) add_dirty1(xs[i][0], ys[j][0], xs[i][1], ys[j][1]);
}

void hwr_vram_written(int x, int y, int w, int h)
{
    if (!dev) return;
    close_batch();
    add_dirty(x, y, w, h);
}

static int overlap1(int x0, int y0, int x1, int y1)
{
    return x0 < bx1 && x1 > bx0 && y0 < by1 && y1 > by0;
}
/* rect with wrap-around against the area drawn in the current batch */
static int overlaps(int x, int y, int w, int h)
{
    int r = 0;
    x &= 1023; y &= 511;
    r |= overlap1(x, y, x + w, y + h);
    if (x + w > 1024) r |= overlap1(0, y, x + w - 1024, y + h);
    if (y + h > 512) r |= overlap1(x, 0, x + w, y + h - 512);
    if (x + w > 1024 && y + h > 512) r |= overlap1(0, 0, x + w - 1024, y + h - 512);
    return r;
}

/* ---------------- primitives ---------------- */

static int scissor(int *sc)
{
    sc[0] = st.dx0; sc[1] = st.dy0; sc[2] = st.dx1 + 1; sc[3] = st.dy1 + 1;
    return sc[2] > sc[0] && sc[3] > sc[1];
}

/* make sure the batch can sample VRAM for a primitive textured from the given areas */
static int prepare(int textured, int tx, int ty, int tw, int th, int cx, int cy, int cw)
{
    if (!batch_open) open_batch();
    if (ndirty && !snapped && !take_snapshot()) return 0;
    if (!textured) return 1;
    if (snapped && (overlaps(tx, ty, tw, th) || overlaps(cx, cy, cw, 1))) {
        close_batch(); open_batch();   /* texture was drawn to in this batch: render-to-texture */
    }
    if (!snapped && !take_snapshot()) return 0;
    return 1;
}

/* full mode, mask check on: the primitive reads bit 15 of the pixels it covers, so
   drawing earlier in this batch over the same area (bit 15 set by an earlier
   primitive's mask-set flag or STP texels) must reach the native VRAM first: a
   barrier, as for a texture that was drawn to */
static int mask_prepare(const pv *p, int n, const int *sc)
{
    if (!full_mode || !st.mchk || dev->mask_stencil) return 1;
    prim_area(p, n, sc);
    if (pb[2] <= pb[0] || pb[3] <= pb[1]) return 1;
    if (!batch_open) open_batch();
    if (snapped && overlap1(pb[0], pb[1], pb[2], pb[3])) { close_batch(); open_batch(); }
    if (!snapped && !take_snapshot()) return 0;
    return 1;
}

/* the batch's drawn area grows by the last emitted primitive's (see prim_area) */
static void grow_bbox(const pv *p, int n, const int *sc)
{
    int x0 = pb[0], y0 = pb[1], x1 = pb[2], y1 = pb[3];
    (void)p; (void)n; (void)sc;
    if (x1 <= x0 || y1 <= y0) return;
    if (x0 < bx0) bx0 = x0;
    if (y0 < by0) by0 = y0;
    if (x1 > bx1) bx1 = x1;
    if (y1 > by1) by1 = y1;
}

/* texture parameters into tmpl; returns texture-page area actually sampled */
static void tex_setup(int textured, int raw, int cx, int cy, int umin, int umax, int vmin, int vmax,
                      int *ta, int *ca)
{
    int mode = st.depth == 0 ? 1 : st.depth == 1 ? 2 : 3;
    memset(&tmpl, 0, sizeof tmpl);
    if (!textured) return;
    tmpl.mode = (int16_t)mode;
    tmpl.raw = raw ? 1 : 0;
    tmpl.tpx = (int16_t)st.tpx; tmpl.tpy = (int16_t)st.tpy;
    tmpl.cx = (int16_t)cx; tmpl.cy = (int16_t)cy;
    tmpl.twmx = (int16_t)st.twmx; tmpl.twmy = (int16_t)st.twmy;
    tmpl.twox = (int16_t)(st.twox & st.twmx); tmpl.twoy = (int16_t)(st.twoy & st.twmy);
    if (st.twmx || st.twmy || umax > 255 || vmax > 255) { umin = 0; umax = 255; vmin = 0; vmax = 255; }  /* window or wrap-around */
    umin &= 255; vmin &= 255;
    if (umax > 255) umax = 255;
    if (vmax > 255) vmax = 255;
    if (umax < umin) { umin = 0; umax = 255; }
    if (vmax < vmin) { vmin = 0; vmax = 255; }
    tmpl.umin = (int16_t)umin; tmpl.vmin = (int16_t)vmin; tmpl.umax = (int16_t)umax; tmpl.vmax = (int16_t)vmax;
    if (mode == 1) { ta[0] = st.tpx + umin / 4; ta[2] = umax / 4 - umin / 4 + 1; ca[2] = 16; }
    else if (mode == 2) { ta[0] = st.tpx + umin / 2; ta[2] = umax / 2 - umin / 2 + 1; ca[2] = 256; }
    else { ta[0] = st.tpx + umin; ta[2] = umax - umin + 1; ca[2] = 0; }
    ta[1] = st.tpy + vmin; ta[3] = vmax - vmin + 1;
    ca[0] = cx; ca[1] = cy;
}

static float mn3(float a, float b, float c) { return a < b ? (a < c ? a : c) : (b < c ? b : c); }
static float mx3(float a, float b, float c) { return a > b ? (a > c ? a : c) : (b > c ? b : c); }

/* the PS1 skips polygons wider than 1023 or taller than 511 pixels */
static int too_big(const pv *a, const pv *b, const pv *c)
{
    return mx3(a->x, b->x, c->x) - mn3(a->x, b->x, c->x) > 1023.f ||
           mx3(a->y, b->y, c->y) - mn3(a->y, b->y, c->y) > 511.f;
}

/* The software rasteriser steps texture coordinates and colours from the top edge of
 * each scanline, while a GPU interpolates at pixel centres. Moving the attributes back
 * by half a pixel along their vertical gradient makes the GPU pick the same texels
 * (measured with a coordinate-encoding probe texture in host_test/hwr; it matters for
 * minified textures). A tiny bias keeps exact integer coordinates (1:1 mapped
 * polygons) from flooring down by one. */

/* Coverage: the PS1 fills pixel x of a span when floor(left) <= x < floor(right)
 * (the software renderer's rule), a GPU when the pixel centre x + 0.5 is inside.
 * So a thin polygon (the slivers along Silent Hill's tree trunks and posts) got
 * pixels on the PS1 that the GPU skipped, and the background showed through as
 * light dots. Positions move by (HWR_DX, HWR_DY), just under half a pixel left
 * and down (measured against the software renderer on Silent Hill traces), which
 * gives the GPU the PS1's pixels; integer-aligned edges keep their pixels. The
 * attributes move with them (A + d.grad A), so every pixel samples the same
 * texel and colour as before. */
#ifndef HWR_DX
#define HWR_DX (-0.5f + 1.f / 64.f)
#endif
#ifndef HWR_DY
#define HWR_DY (0.5f - 1.f / 64.f)
#endif
#ifdef HWR_TUNE                         /* host tuning: HWR_DX / HWR_DY from the environment */
#include <stdlib.h>
static float hwr_dx(void) { static float v = 9; if (v == 9) { const char *e = getenv("HWR_DXV"); v = e ? (float)atof(e) : HWR_DX; } return v; }
static float hwr_dy(void) { static float v = 9; if (v == 9) { const char *e = getenv("HWR_DYV"); v = e ? (float)atof(e) : HWR_DY; } return v; }
#define DX hwr_dx()
#define DY hwr_dy()
#else
#define DX HWR_DX
#define DY HWR_DY
#endif

static int tri_is_sprite(const pv *a, const pv *b, const pv *c);

/* 2D drawn with polygons, one texel per pixel (sprite_dir below): the PS1 shows
 * texel u0 + k on pixel k going right and u0 - k going left (a mirrored sprite),
 * both as whole native pixels. Sampled at pixel centres, an increasing coordinate
 * needs only the bias; a decreasing one falls a whole texel short (u0 - k - 0.5
 * floors to u0 - k - 1: every column one texel over, and the last one from
 * outside the sprite, a thin line along its edge or along every seam of a sprite
 * built from tiles), so it gets just under one texel more. This holds for every
 * sample inside a native pixel, so at every resolution scale, unlike the half
 * pixel moves below, which are right only at 1x: upscaled, they put sprite
 * polygons and their textures half a native pixel off rectangles and each other
 * (hairline gaps and overlaps between the pieces of 2D scenes). A sprite polygon
 * has whole-pixel corners, which keep their pixels without the coverage move,
 * so it gets none (DuckStation's flipped-sprite handling, from Beetle PSX HW). */
static void sprite_sample_shift(pv *t, int n, int sx, int sy)
{
    int k;
    const float su = sx > 0 ? 1.f / 256.f : 1.f - 1.f / 256.f, sv = sy > 0 ? 1.f / 256.f : 1.f - 1.f / 256.f;
    for (k = 0; k + 2 < n; k += 3) {
        pv *a = &t[k], *b = &t[k + 1], *c = &t[k + 2];
        float e1x = b->x - a->x, e1y = b->y - a->y, e2x = c->x - a->x, e2y = c->y - a->y;
        float ix = 1.f / (e1x * e2y - e2x * e1y);
        float *pa[3] = { &a->r, &a->g, &a->b };
        int i;
        a->u += su; b->u += su; c->u += su; a->v += sv; b->v += sv; c->v += sv;
        for (i = 0; i < 3; i++) {                     /* colours as for any polygon */
            size_t off = (size_t)((char *)pa[i] - (char *)a);
            float va = *pa[i], vb = *(float *)((char *)b + off), vc = *(float *)((char *)c + off);
            float s = -0.5f * ((vc - va) * e1x - (vb - va) * e2x) * ix;
            *(float *)((char *)a + off) += s; *(float *)((char *)b + off) += s; *(float *)((char *)c + off) += s;
        }
    }
}

/* Every triangle sprite-like (tri_is_sprite) with exactly one texel per pixel
 * across and down: returns 1 and the directions of u along x and v along y. */
static int sprite_dir(const pv *t, int n, int *sx, int *sy)
{
    int k, dx = 0, dy = 0;
    for (k = 0; k + 2 < n; k += 3) {
        const pv *a = &t[k], *b = &t[k + 1], *c = &t[k + 2];
        float e1x = b->x - a->x, e1y = b->y - a->y, e2x = c->x - a->x, e2y = c->y - a->y;
        float det = e1x * e2y - e2x * e1y;
        float gu = (b->u - a->u) * e2y - (c->u - a->u) * e1y;     /* du/dx * det */
        float gv = (c->v - a->v) * e1x - (b->v - a->v) * e2x;     /* dv/dy * det */
        int ku, kv;
        if (!tri_is_sprite(a, b, c)) return 0;
        ku = gu == det ? 1 : gu == -det ? -1 : 0;
        kv = gv == det ? 1 : gv == -det ? -1 : 0;
        if (!ku || !kv || (dx && ku != dx) || (dy && kv != dy)) return 0;
        dx = ku; dy = kv;
    }
    *sx = dx; *sy = dy;
    return dx != 0;
}

static void ps1_sample_shift(pv *t, int n)
{
    int k, i;
    const float dx = DX, dy0 = DY;
    for (k = 0; k + 2 < n; k += 3) {
        pv *a = &t[k], *b = &t[k + 1], *c = &t[k + 2];
        float e1x = b->x - a->x, e1y = b->y - a->y, e2x = c->x - a->x, e2y = c->y - a->y;
        float det = e1x * e2y - e2x * e1y, ix;
        float *pa[5] = { &a->u, &a->v, &a->r, &a->g, &a->b };
        a->x += dx; b->x += dx; c->x += dx; a->y += dy0; b->y += dy0; c->y += dy0;
        if (det > -0.5f && det < 0.5f) continue;      /* degenerate: nothing is drawn anyway */
        ix = 1.f / det;
        for (i = 0; i < 5; i++) {
            size_t off = (size_t)((char *)pa[i] - (char *)a);
            float va = *pa[i], vb = *(float *)((char *)b + off), vc = *(float *)((char *)c + off);
            float gx = ((vb - va) * e2y - (vc - va) * e1y) * ix;
            float gy = ((vc - va) * e1x - (vb - va) * e2x) * ix;
            float s = -0.5f * gy + (i < 2 ? 1.f / 256.f : 0.f) + dx * gx + dy0 * gy;
            /* across too, when a pixel spans more than one texel: the PS1 takes the
               texel at the pixel's left edge, a GPU the one at its centre, gx / 2
               further (X-Files picks single bytes of its 24-bit picture with quads
               of 4 texels per pixel for its cursor: one at the centre took the
               wrong bytes, a box of noise; 1:1 and magnified textures land on the
               same texel either way and keep the measured mapping). The middle of
               that texel, not its edge: the RSX puts the corners on its sub-pixel
               grid, which moves the coordinates by up to gx / 64 (at the edge
               the cursor's colours came from the byte before) */
            if (i < 2 && (gx > 1.f || gx < -1.f)) s += -0.5f * gx + 0.5f - 1.f / 256.f;
            *(float *)((char *)a + off) += s; *(float *)((char *)b + off) += s; *(float *)((char *)c + off) += s;
        }
    }
}

static void colour(pv *p, uint32_t c)
{
    p->r = f_byte[c & 0xff]; p->g = f_byte[(c >> 8) & 0xff]; p->b = f_byte[(c >> 16) & 0xff];
}

#include "pgxp_depth.h"   /* corner depths made to fit one surface */

/* Seam fill: the edges PGXP polygons drew this frame (sub-pixel positions,
   1/256 pixel), so an edge a later polygon shares with an earlier one is left
   where it is. Grown, it covered a strip of the earlier polygon, and when the
   game drew that one again semi-transparently (fog, light, reflections: Silent
   Hill's ground, GT2, Soul Reaver), the strip lost the blend: a grid of light or
   dark lines along the polygon edges. Separate meshes (the cracks seam fill is
   for) share no edges, so theirs still grow. A semi-transparent polygon laid
   over an earlier one moves out where that one did, so it covers its strip too,
   but only on edges no other solid polygon has: there the neighbour's own layer
   covers it, and two additive layers over one strip were a line of their own
   (Silent Hill's ground: its fog is added over every tile). A solid polygon does
   not grow over an edge a semi-transparent one drew before it either (GT2's
   wheels: a light outline). */
#define SEEN_N 16384
/* 8 bytes an edge, 128 KiB: twice the edges of the old table (20 bytes an
   edge, 160 KiB), where busy frames (Silent Hill) already lost a few, in less
   memory, as it is read for every polygon (the PS3's PPU waits ~400 cycles for
   main memory). k: the edge (both corners) hashed; st: the frame it is from
   (seen_st); v: n << 2 | grown << 1 | ns (n solid, ns semi-transparent
   polygons had it, grown: the first solid one moved out there) */
static struct { uint32_t k; uint16_t st, v; } seen[SEEN_N];
static uint16_t seen_st;          /* this frame's stamp: 1..65535, the table cleared when it wraps */
static uint32_t seen_frame = ~0u;
static void seen_new_frame(void)
{
    if (hwr_stats.frames == seen_frame) return;
    seen_frame = hwr_stats.frames;
    if (++seen_st == 0) { memset(seen, 0, sizeof seen); seen_st = 1; }
}
/* a corner, 1/256 pixel, from x * 256 + 1.5 * 2^23 (rounded to a whole number
   by the add itself, as lrintf did): its bits are read as an integer below,
   all eight of a polygon's stored first, so only the first load waits */
#define SEEN_MAGIC 12582912.f
/* the corner exactly: x and y in the float's low 23 bits (2^22 + the value) */
static uint64_t seen_corner(uint32_t bx, uint32_t by) { return (uint64_t)(bx & 0x7fffff) << 23 | (by & 0x7fffff); }
static uint64_t seen_hash(uint64_t c1, uint64_t c2)   /* either direction */
{
    uint64_t a = c1 < c2 ? c1 : c2, b = c1 < c2 ? c2 : c1, h = a * 0x9E3779B97F4A7C15ull ^ b * 0xC2B2AE3D27D4EB4Full;
    h ^= h >> 29;
    return h * 0xBF58476D1CE4E5B9ull;
}
#define SEEN_SLOT(h) ((unsigned)((h) >> 50) & (SEEN_N - 1))   /* 14 bits: SEEN_N */
#define SEEN_KEY(h) ((uint32_t)((h) >> 16))
/* the edge's slot this frame, made when it is new (-1: no room near it);
   *r: 0 if no polygon had it yet, else n * 2 + grown + 4096 if a
   semi-transparent one had it */
static inline int seen_find(uint64_t hh, int *r)
{
    unsigned h = SEEN_SLOT(hh), k;
    uint32_t key = SEEN_KEY(hh);
    for (k = 0; k < 8; k++, h = (h + 1) & (SEEN_N - 1)) {
        if (seen[h].st != seen_st) {
            seen[h].k = key; seen[h].st = seen_st; seen[h].v = 0;
            *r = 0; return h;
        }
        if (seen[h].k == key) {
            unsigned v = seen[h].v;
            *r = (v >> 2) * 2 + ((v >> 1) & 1) + (v & 1 ? 4096 : 0);
            return h;
        }
    }
    *r = 0; return -1;
}
/* add: 1 + grown for a solid polygon, 4 for a semi-transparent one */
static inline void seen_add(int h, int add)
{
    unsigned v, n;
    if (h < 0) return;
    v = seen[h].v; n = v >> 2;
    if (add == 4) v |= 1;
    else {
        if (!n) v = (v & ~2u) | (add == 2 ? 2u : 0u);
        if (n < 1000) v += 4;
    }
    seen[h].v = (uint16_t)v;
}
/* 1 / sqrt(x), x > 0: the PPU's estimate and two Newton steps (relative error
   about 1e-6; offsets of a fraction of a pixel need far less), without the
   divide and square root that each stall the PPU for dozens of cycles */
static inline float seam_rsqrt(float x)
{
#if defined(__powerpc__) || defined(__PPU__)
    double d = x, y;
    __asm__("frsqrte %0,%1" : "=d" (y) : "d" (d));
    y = y * (1.5 - 0.5 * d * y * y);
    y = y * (1.5 - 0.5 * d * y * y);
    return (float)y;
#else
    return 1.f / sqrtf(x);
#endif
}

/* DuckStation IsPossibleSpritePolygon, integer-exact: a textured triangle is
   sprite-like when its UV mapping is separable — U varies only with X and V
   only with Y (dU/dY == 0 and dV/dX == 0). The numerators below are the same
   gradients ps1_sample_shift computes, but with exact integer arithmetic:
   p[].x/y/u/v are integer-valued here (before ps1_sample_shift, which only
   adds the same shift to every vertex of a triangle, so it cannot change a
   difference). A quad is sprite-like only when every triangle actually drawn
   passes, so a twisted quad whose halves disagree stays a polygon. Degenerate
   triangles (det == 0, nothing drawn) are not sprites. Shape orientation alone
   does not matter: a diamond whose UVs are still u=x, v=y passes, because only
   the UV mapping is tested, like DuckStation. The test is per triangle (quads
   are split into triangles before drawing), so a non-affine quad whose two
   triangles are each separable passes even if a per-quad test would reject
   it — per-triangle is the correct unit here. */
static int tri_is_sprite(const pv *a, const pv *b, const pv *c)
{
    int e1x = (int)(b->x - a->x), e1y = (int)(b->y - a->y);
    int e2x = (int)(c->x - a->x), e2y = (int)(c->y - a->y);
    int det = e1x * e2y - e2x * e1y;
    int du1 = (int)(b->u - a->u), du2 = (int)(c->u - a->u);
    int dv1 = (int)(b->v - a->v), dv2 = (int)(c->v - a->v);
    if (det == 0) return 0;
    /* dU/dY numerator: (uc-ua)*e1x - (ub-ua)*e2x ; dV/dX numerator: (vb-va)*e2y - (vc-va)*e1y */
    return (du2 * e1x - du1 * e2x) == 0 && (dv1 * e2y - dv2 * e1y) == 0;
}
static int poly_is_sprite(const pv *t, int n)
{
    int k;
    if (n < 3) return 0;
    for (k = 0; k + 2 < n; k += 3)
        if (!tri_is_sprite(&t[k], &t[k + 1], &t[k + 2])) return 0;
    return 1;
}

static void do_poly(const uint32_t *l, const uint32_t *end)
{
    uint32_t w0 = le32(l), cmd = w0 >> 24, uvw[4] = { 0 };
    int gour = cmd & 0x10, quad = cmd & 8, tex = cmd & 4, semi = cmd & 2, raw = cmd & 1;
    int n = quad ? 4 : 3, i, idx = 1, sc[4], ta[4], ca[4] = { 0 }, cx = 0, cy = 0;
    int umin = 255, umax = 0, vmin = 255, vmax = 0, nt = 0, pg = 0, spr = 0, s1 = 0, sdx = 0, sdy = 0;
    uint32_t xys[4];
    const uint32_t *xyp[4];
    pv p[4], t[6];
    unsigned long long pt0, pt1;
    pg_timed = psx_debug_trace && !(++hwr_poly_n & (PG_SAMPLE - 1));
    pt0 = PG_TB();
    for (i = 0; i < n; i++) {
        uint32_t c = w0, xy;
        if (gour && i) { if (l + idx >= end) return; c = le32(l + idx++); }
        if (l + idx >= end) return;
        xyp[i] = l + idx;
        xy = le32(l + idx++);
        xys[i] = xy;
        p[i].x = (s11f(xy) + st.oxf); p[i].y = (s11f(xy >> 16) + st.oyf);
        p[i].ox = p[i].oy = p[i].ow = 0.f;
        if (tex) {
            if (l + idx >= end) return;
            uvw[i] = le32(l + idx++);
            {
                int u = uvw[i] & 0xff, v = (uvw[i] >> 8) & 0xff;
                p[i].u = f_byte[u]; p[i].v = f_byte[v];
                if (u < umin) umin = u;
                if (u > umax) umax = u;
                if (v < vmin) vmin = v;
                if (v > vmax) vmax = v;
            }
        } else p[i].u = p[i].v = 0.f;
        if (tex && raw) { p[i].r = p[i].g = p[i].b = 128.f; } else colour(&p[i], c);
    }
    if (tex) {
        uint32_t clut = uvw[0] >> 16;
        cx = (clut & 0x3f) * 16; cy = (clut >> 6) & 0x1ff;
        set_texpage(uvw[1] >> 16);
    }
    if (!scissor(sc)) { hwr_poly_t_all += PG_TB() - pt0; return; }
    pt1 = PG_TB(); hwr_poly_t_parse += pt1 - pt0;
    /* PGXP-lite: every vertex found among recent GTE results is drawn at its exact
       position, the others at whole pixels, decided per vertex only: polygons that
       share a corner then always agree on it, so no gap opens along their edge
       (a rule on the whole polygon put a shared, found corner at its exact place
       in one polygon and at whole pixels in the next). */
    if (&pgxp_lite_on && pgxp_lite_on && pgxp_lite_cands) {
        float z[4][2];
        struct pgxp_cand cd[4][2];
        int found = 0, nz = 0, exact = 0, nc[4], amb = 0, pick[4] = { 0, 0, 0, 0 };
        float zalt[4] = { 0.f, 0.f, 0.f, 0.f };
        unsigned long long t0, t1;
        /* (also works out the projections still queued: timed on its own) */
        if (pgxp_lite_prefetch) pgxp_lite_prefetch((const uint32_t *const *)xyp, xys, n);
        t0 = PG_TB();
        for (i = 0; i < n; i++) {
            /* by its value; the packet word's address (the game stored the GTE's
               result there itself) only tells which of a word's two vertices it is.
               The place drawn is always the one the table by value holds (merged
               with a vertex on the same point, welded to a neighbour): a corner
               found by address in one polygon and by value in the next was drawn
               at two places, thin slits all over RE3's Jill */
            struct pgxp_cand a;
            nc[i] = pgxp_lite_cands(xys[i], cd[i]);
            if (pgxp_lite_cand_at && pgxp_lite_cand_at(xyp[i], xys[i], &a) > 0) {
                exact++;
                if (!nc[i]) { cd[i][0] = a; nc[i] = 1; }
                else if (nc[i] > 1) {
                    float d0 = cd[i][0].z - a.z, d1 = cd[i][1].z - a.z;
                    if (d0 < 0.f) d0 = -d0;
                    if (d1 < 0.f) d1 = -d1;
                    if (d1 < d0) cd[i][0] = cd[i][1];
                    nc[i] = 1;
                }
            }
            if (nc[i]) found++;
            if (nc[i] > 1) amb |= 1 << i;
        }
        /* A word that stands for two vertices: the polygon takes the one projected
           much closer in time to its other corners (a wall's corner, not the
           fighter's in front). When it is not clearly closer (two vertices of one
           object, the front and back of Jill's body, projected among each other),
           every polygon draws the corner at the first one's place: polygons around
           it that chose differently opened a Y-shaped slit. Only the depth is then
           chosen per polygon (z[1]). */
        if (amb && !hwr_pgxp_first) {
            uint32_t smin = 0, smax = 0, ref = 0; int k = 0;
            for (i = 0; i < n; i++) if (nc[i] == 1) {
                uint32_t q = cd[i][0].seq;
                if (!k || (int32_t)(q - smin) < 0) smin = q;
                if (!k || (int32_t)(q - smax) > 0) smax = q;
                ref = q; k++;
            }
            for (i = 0; i < n; i++)
                if (nc[i] > 1) {
                    uint32_t d0 = cd[i][0].seq - ref, d1 = cd[i][1].seq - ref, lo, hi;
                    if ((int32_t)d0 < 0) d0 = -d0;
                    if ((int32_t)d1 < 0) d1 = -d1;
                    lo = d0 < d1 ? d0 : d1; hi = d0 < d1 ? d1 : d0;
                    if (k && hi / 4 > lo + (smax - smin) + 16) pick[i] = d1 < d0;
                    else { pick[i] = 0; zalt[i] = cd[i][1].z; }
                }
            amb = 0;
            hwr_pgxp_amb++;
        }
        t1 = PG_TB(); hwr_pgxp_t_find += t1 - t0; t0 = t1;
        if (amb && !hwr_pgxp_first) {
            float best = 0.f;
            int m, first = 1;
            for (m = 0; m < 16; m++) {
                float zz[4], px[4], py[4], sc = 0.f, lo = 0.f, hi = 0.f;
                uint32_t smin = 0, smax = 0;
                int k = 0, allz = 1;
                if (m & ~amb) continue;
                for (i = 0; i < n; i++) {
                    const struct pgxp_cand *c = nc[i] ? &cd[i][(m >> i) & 1] : NULL;
                    px[i] = p[i].x + (c ? c->dx : 0.f); py[i] = p[i].y + (c ? c->dy : 0.f);
                    zz[i] = c ? c->z : 0.f;
                    if (zz[i] <= 0.f) allz = 0;
                    else { if (!lo || zz[i] < lo) lo = zz[i]; if (zz[i] > hi) hi = zz[i]; }
                    if (c) {
                        if (!k || (int32_t)(c->seq - smin) < 0) smin = c->seq;
                        if (!k || (int32_t)(c->seq - smax) > 0) smax = c->seq;
                        k++;
                    }
                }
                if (allz && !pgxp_depths_fit(px, py, zz, n)) sc = 1e7f;
                sc += (float)(smax - smin < 1000000u ? smax - smin : 1000000u);
                if (lo > 0.f) sc += (hi / lo - 1.f) * 0.5f;
                if (first || sc < best) { best = sc; first = 0; for (i = 0; i < n; i++) pick[i] = (m >> i) & 1; }
            }
            hwr_pgxp_amb++;
        }
        t1 = PG_TB(); hwr_pgxp_t_pick += t1 - t0; t0 = t1;
        for (i = 0; i < n; i++) {
            if (nc[i]) {
                const struct pgxp_cand *c = &cd[i][pick[i]];
                p[i].ox = c->dx; p[i].oy = c->dy;
                /* depth is lite's word-keyed depth (the corrector's
                   address-keyed depth went with it). */
                z[i][0] = c->z;
            } else { p[i].ox = p[i].oy = 0.f; z[i][0] = 0.f; }
            z[i][1] = zalt[i];
            if (z[i][0] > 0.f) nz++;
        }
        /* A corner found alone is still drawn where it was found. Builds 82-85 put
           it back at whole pixels (meant for a CPU-made wall whose corner shared a
           pixel with a fighter's vertex), but a vertex must be at the same place in
           every polygon that has it: the neighbouring polygon kept it exact, and a
           thin gap opened along their edge (GT2's cars). Only counted now. */
        if (found == 1 && !exact) hwr_pgxp_alone++;
        pg = found > 0;
        if (pg) hwr_pgxp_polys++;
        else hwr_pgxp_misses++;
        /* perspective-correct textures (setting 2): only when every corner of the
           polygon has its depth from the GTE; otherwise the whole polygon stays flat
           (affine, like the PS1). A guessed depth for the missing corner bent the
           texture, and the next frame, with the corner found again, it snapped back:
           textures jumping, or squeezed into a corner so they looked like another */
        if (pg && pgxp_lite_on >= 2 && tex) {
            float zz[4], px[4], py[4], zmax = 0.f;
            int alt = 0, m, ok = 0;
            for (i = 0; i < n; i++) { px[i] = p[i].x + p[i].ox; py[i] = p[i].y + p[i].oy; }
            /* a vertex that shares its pixel with another vertex this frame has two
               depths: when every corner has one, try each choice and keep the one
               whose corners agree best (the first that fitted let a palm tree behind
               Nina in Tekken 3 take her depth whenever hers was stored first) */
            if (nz == n) {
                float bhi = 0.f, blo = 1.f, zb[4];
                for (i = 0; i < n; i++) if (z[i][1] != 0.f) alt |= 1 << i;
                for (m = 0; m < 16; m++) {
                    float lo, hi;
                    if (m & ~alt) continue;
                    for (i = 0; i < n; i++) zz[i] = (m >> i) & 1 ? z[i][1] : z[i][0];
                    if (!pgxp_depths_fit(px, py, zz, n)) continue;
                    lo = hi = zz[0];
                    for (i = 1; i < n; i++) { if (zz[i] < lo) lo = zz[i]; if (zz[i] > hi) hi = zz[i]; }
                    if (!ok || hi * blo < bhi * lo) { bhi = hi; blo = lo; for (i = 0; i < n; i++) zb[i] = zz[i]; ok = 1; }   /* hi / lo smallest */
                }
                if (ok) for (i = 0; i < n; i++) zz[i] = zb[i];
            }
            /* otherwise repair the odd corner (pgxp_depth.h) rather than drawing the
               polygon flat for a frame */
            if (!ok && nz >= n - 1) {
                for (i = 0; i < n; i++) zz[i] = z[i][0];
                ok = pgxp_depths(px, py, zz, n);
                if (ok) hwr_pgxp_misfit++;
            }
            if (ok) {
                float iz;
                for (i = 0; i < n; i++) if (zz[i] > zmax) zmax = zz[i];
                iz = 1.f / zmax;
                for (i = 0; i < n; i++) p[i].ow = zz[i] * iz;
            }
            if (p[0].ow > 0.f) hwr_pgxp_persp++;
            else hwr_pgxp_flat++;
        }
        /* Seam fill: a model's parts (Jill's face and forehead, the top and her
           skin) are separate meshes whose edges meet only after the PS1 rounds
           their corners to whole pixels. Drawn exactly, they stand a fraction of a
           pixel apart and thin cracks open between them, showing what is behind
           (eyes, neck, shoulders in RE3). A solid textured polygon is drawn larger,
           an edge moved outward by hwr_pgxp_grow, so neighbouring parts overlap
           instead. Edges shared with a polygon drawn earlier this frame stay
           (see seen[]); semi-transparent polygons move out only where the solid
           one under them did (elsewhere an overlap would blend twice: a visible
           line). Only the upscaled pass moves (the offsets).
           A supplement to culling, not a replacement: it only grows polygons the
           culling kept, so a wrongly culled triangle stays missing. */
        t1 = PG_TB(); hwr_pgxp_t_depth += t1 - t0; t0 = t1;
        if (pg && hwr_pgxp_grow > 0.f) {
            static const int o4[4] = { 0, 1, 3, 2 }, o3[3] = { 0, 1, 2 };   /* corners in outline order */
            const int *o = n == 4 ? o4 : o3;
            float qx[4], qy[4], ax[4], ay[4], area = 0.f, g[4], ex[4], ey[4], el[4];
            union { float f[8]; uint32_t u[8]; } cb;
            uint64_t eh[4];
            int slot[4], grew;
            int ng = 0;
            seen_new_frame();
            for (i = 0; i < n; i++) {
                qx[i] = p[o[i]].x + p[o[i]].ox; qy[i] = p[o[i]].y + p[o[i]].oy;
                cb.f[2 * i] = qx[i] * 256.f + SEEN_MAGIC; cb.f[2 * i + 1] = qy[i] * 256.f + SEEN_MAGIC;
            }
            for (i = 0; i < n; i++) {
                int j = i + 1 == n ? 0 : i + 1;
                eh[i] = seen_hash(seen_corner(cb.u[2 * i], cb.u[2 * i + 1]), seen_corner(cb.u[2 * j], cb.u[2 * j + 1]));
                __builtin_prefetch(&seen[SEEN_SLOT(eh[i])]);   /* loaded while the sums below run */
            }
            /* the edges (corner i to the next) and their squared lengths */
            for (i = 0; i < n; i++) {
                int j = i + 1 == n ? 0 : i + 1;
                ex[i] = qx[j] - qx[i]; ey[i] = qy[j] - qy[i]; el[i] = ex[i] * ex[i] + ey[i] * ey[i];
                area += qx[i] * qy[j] - qx[j] * qy[i];
            }
            /* a sliver under a pixel thick (GT2 draws such grey ones along its
               cars' wheels) hardly shows on the PS1; moved out it would be a
               line around the wheel. Those keep their size. */
            {
                float ml = el[0];
                for (i = 1; i < n; i++) if (el[i] > ml) ml = el[i];
                /* thickness: twice the area over the longest side (a quad: half that) */
                if (area * area < (n == 4 ? 4.f : 1.f) * ml) area = 0.f;
            }
            /* g[i]: how far edge i moves out */
            for (i = 0; i < n; i++) {
                int e;
                slot[i] = seen_find(eh[i], &e);
                /* solid: no polygon had it yet; semi: one solid had it and moved out there, no semi one.
                   Plain (untextured) ones stay: GT2 lays grey ones under its cars'
                   tyres, and moved out they showed as a light line around them. */
                g[i] = (semi ? e == 3 : e == 0 && tex) ? hwr_pgxp_grow : 0.f;
                ng += g[i] > 0.f;
            }
            grew = ng && (area > 0.05f || area < -0.05f);
            if (grew) {
                float sd = area > 0.f ? 1.f : -1.f, lim = hwr_pgxp_grow * 2.8f;
                /* unit edges, each edge once (it is in two corners) */
                for (i = 0; i < n; i++)
                    if (el[i] >= 1e-6f) { float r = seam_rsqrt(el[i]); ex[i] *= r; ey[i] *= r; }
                for (i = 0; i < n; i++) {
                    int pr = i ? i - 1 : n - 1;
                    float e1x = ex[pr], e1y = ey[pr], e2x = ex[i], e2y = ey[i], k, g1 = g[pr], g2 = g[i];
                    float n1x, n1y, n2x, n2y, det;
                    ax[i] = ay[i] = 0.f;
                    if (el[pr] < 1e-6f || el[i] < 1e-6f || (g1 == 0.f && g2 == 0.f)) continue;
                    if (g1 == g2) {
                        /* the corner moves along its bisector so both of its edges
                           move out by the same amount; a sharp corner at most ~2.8 times */
                        k = 1.f + e1x * e2x + e1y * e2y;
                        if (k < 0.25f) k = 0.25f;
                        k = sd * g1 / k;
                        ax[i] = (e1y + e2y) * k; ay[i] = -(e1x + e2x) * k;
                        continue;
                    }
                    /* one edge moves, the other stays: the corner goes where the
                       moved edge's line meets the other edge's line */
                    n1x = sd * e1y; n1y = -sd * e1x; n2x = sd * e2y; n2y = -sd * e2x;   /* outward normals */
                    det = n1x * n2y - n1y * n2x;
                    if (det > -0.05f && det < 0.05f) continue;   /* (nearly) straight on: leave the corner */
                    det = 1.f / det;
                    ax[i] = (g1 * n2y - g2 * n1y) * det; ay[i] = (n1x * g2 - n2x * g1) * det;
                    k = ax[i] * ax[i] + ay[i] * ay[i];
                    if (k > lim * lim) { k = lim * seam_rsqrt(k); ax[i] *= k; ay[i] *= k; }
                }
                for (i = 0; i < n; i++) { p[o[i]].ox += ax[i]; p[o[i]].oy += ay[i]; }
                hwr_pgxp_grown++;
            }
            for (i = 0; i < n; i++)   /* (where they are exactly, before the offsets) */
                seen_add(slot[i], semi ? 4 : 1 + (g[i] > 0.f && grew));
        }
        hwr_pgxp_t_seam += PG_TB() - t0;
    }
    if (pg) {   /* the vertices' pgx, pgy, pgz (1/1024 pixel; depth 0: affine, as the PS1):
                   all stored, then all loaded, so only the first load waits (see f_byte) */
        volatile int32_t q[12];
        for (i = 0; i < n; i++) {
            q[3 * i] = (int32_t)(p[i].ox * 1024.f); q[3 * i + 1] = (int32_t)(p[i].oy * 1024.f);
            q[3 * i + 2] = (int32_t)(p[i].ow * 32767.f);
        }
        for (i = 0; i < n; i++) { p[i].qx = (int16_t)q[3 * i]; p[i].qy = (int16_t)q[3 * i + 1]; p[i].qz = (int16_t)q[3 * i + 2]; }
    }
    if (!too_big(&p[0], &p[1], &p[2])) { t[nt++] = p[0]; t[nt++] = p[1]; t[nt++] = p[2]; }
    if (quad && !too_big(&p[1], &p[2], &p[3])) { t[nt++] = p[1]; t[nt++] = p[2]; t[nt++] = p[3]; }
    if (!nt) { hwr_poly_t_all += PG_TB() - pt0; return; }
    /* axis-aligned check on the triangles actually drawn, before the sample
       shift (which preserves all vertex differences, so it cannot change it) */
    if (tex && hwr_sprites_apart) spr = poly_is_sprite(t, nt);
    if (tex && !pg) s1 = sprite_dir(t, nt, &sdx, &sdy);   /* whole-pixel corners only (no PGXP) */
    /* the PS1 never draws the right and bottom edge, so a polygon mapping texels
       0..64 shows 0..63: clamping to 64 let upscaled edge pixels pick texel 64,
       a thin line of the neighbouring tile along polygon seams (Tekken 3 skies).
       Mirrored, the edge left out holds the lowest texel: 64..0 shows 64..1. */
    if (umax > umin) { if (s1 && sdx < 0) umin++; else umax--; }
    if (vmax > vmin) { if (s1 && sdy < 0) vmin++; else vmax--; }
    pt1 = PG_TB();
    if (s1) sprite_sample_shift(t, nt, sdx, sdy); else ps1_sample_shift(t, nt);
    { unsigned long long q = PG_TB(); hwr_poly_t_shift += q - pt1; pt1 = q; }
    tex_setup(tex, raw, cx, cy, umin, umax, vmin, vmax, ta, ca);
    if (!prepare(tex, ta[0], ta[1], ta[2], ta[3], ca[0], ca[1], ca[2])) { hwr_poly_t_all += PG_TB() - pt0; return; }
    if (!mask_prepare(t, nt, sc)) { hwr_poly_t_all += PG_TB() - pt0; return; }
    { unsigned long long q = PG_TB(); hwr_poly_t_setup += q - pt1; pt1 = q; }
    pg_poly = pg;
    cur_spr = spr;
    emit(t, nt, semi, tex, sc);
    pg_poly = 0;
    grow_bbox(t, nt, sc);
    hwr_stats.prims++;
    { unsigned long long q = PG_TB(); hwr_poly_t_emit += q - pt1; hwr_poly_t_all += q - pt0; }
}

static void line(const pv *a, const pv *b, int semi, const int *sc)
{
    pv q[6], c0 = *a, c1 = *b;
    float dx = b->x - a->x, dy = b->y - a->y;
    if (dx > 1023.f || dx < -1023.f || dy > 511.f || dy < -511.f) return;
    if ((dx < 0 ? -dx : dx) >= (dy < 0 ? -dy : dy)) {
        if (dx < 0) { c0 = *b; c1 = *a; }
        q[0] = c0; q[1] = c1; q[1].x += 1.f; q[2] = c0; q[2].y += 1.f;
        q[3] = q[1]; q[4] = q[2]; q[5] = c1; q[5].x += 1.f; q[5].y += 1.f;
    } else {
        if (dy < 0) { c0 = *b; c1 = *a; }
        q[0] = c0; q[1] = c0; q[1].x += 1.f; q[2] = c1; q[2].y += 1.f;
        q[3] = q[1]; q[4] = q[2]; q[5] = c1; q[5].x += 1.f; q[5].y += 1.f;
    }
    if (!mask_prepare(q, 6, sc)) return;
    cur_spr = 0; emit(q, 6, semi, 0, sc);
    grow_bbox(q, 6, sc);
}

/* full mode: fill rectangle (0x02) as a flat quad; ignores the draw area and the
   mask like the PS1, x and width in steps of 16 */
static void do_fill(const uint32_t *l, const uint32_t *end)
{
    uint32_t c, xy, wh;
    int x, y, w, h, sc[4], ta[4], ca[4], i;
    pv q[6];
    if (l + 2 >= end) return;
    c = le32(l); xy = le32(l + 1); wh = le32(l + 2);
    x = xy & 0x3f0; y = (xy >> 16) & 0x1ff;
    w = ((wh & 0x3ff) + 15) & ~15; h = (wh >> 16) & 0x1ff;
    if (!w || !h) return;
    sc[0] = x; sc[1] = y; sc[2] = x + w > 1024 ? 1024 : x + w; sc[3] = y + h > 512 ? 512 : y + h;
    tex_setup(0, 0, 0, 0, 0, 0, 0, 0, ta, ca);
    if (!prepare(0, 0, 0, 0, 0, 0, 0, 0)) return;
    memset(q, 0, sizeof q);
    for (i = 0; i < 6; i++) {
        static const int qx[6] = { 0, 1, 0, 1, 0, 1 }, qy[6] = { 0, 0, 1, 0, 1, 1 };
        q[i].x = (float)(qx[i] ? sc[2] : sc[0]); q[i].y = (float)(qy[i] ? sc[3] : sc[1]);
        /* the PS1 stores the fill colour as 15 bits */
        q[i].r = (float)((c & 0xf8) | ((c & 0xf8) >> 5));
        q[i].g = (float)(((c >> 8) & 0xf8) | (((c >> 8) & 0xf8) >> 5));
        q[i].b = (float)(((c >> 16) & 0xf8) | (((c >> 16) & 0xf8) >> 5));
    }
    {   /* fills store bit 15 = 0 and ignore the mask settings */
        int ms = st.mset, mc = st.mchk;
        st.mset = st.mchk = 0;
        cur_spr = 0; emit(q, 6, 0, 0, sc);
        st.mset = ms; st.mchk = mc;
    }
    grow_bbox(q, 6, sc);
}

static void do_line(const uint32_t *l, const uint32_t *end)
{
    uint32_t w0 = le32(l), cmd = w0 >> 24;
    int gour = cmd & 0x10, poly = cmd & 8, semi = cmd & 2, idx = 2, sc[4], ta[4], ca[4];
    pv prev, cur;
    if (l + 1 >= end || !scissor(sc)) return;
    tex_setup(0, 0, 0, 0, 0, 0, 0, 0, ta, ca);
    if (!prepare(0, 0, 0, 0, 0, 0, 0, 0)) return;
    memset(&prev, 0, sizeof prev);
    colour(&prev, w0);
    { uint32_t xy = le32(l + 1); prev.x = (s11f(xy) + st.oxf); prev.y = (s11f(xy >> 16) + st.oyf); }
    for (;;) {
        uint32_t c = w0, xy;
        if (l + idx >= end) break;
        if (gour) {
            c = le32(l + idx);
            if (poly && (c & 0xf000f000) == 0x50005000) break;
            idx++;
            if (l + idx >= end) break;
        }
        xy = le32(l + idx);
        if (!gour && poly && (xy & 0xf000f000) == 0x50005000) break;
        idx++;
        memset(&cur, 0, sizeof cur);
        colour(&cur, c);
        cur.x = (s11f(xy) + st.oxf); cur.y = (s11f(xy >> 16) + st.oyf);
        line(&prev, &cur, semi, sc);
        prev = cur;
        if (!poly) break;
    }
    hwr_stats.prims++;
}

static void do_rect(const uint32_t *l, const uint32_t *end)
{
    uint32_t w0 = le32(l), cmd = w0 >> 24, xy, uvw = 0;
    int size = (cmd >> 3) & 3, tex = cmd & 4, semi = cmd & 2, raw = cmd & 1;
    int idx = 2, w, h, sc[4], ta[4], ca[4] = { 0 }, cx = 0, cy = 0, u0 = 0, v0 = 0;
    float x, y;
    pv p[4], t[6];
    if (l + 1 >= end) return;
    xy = le32(l + 1);
    x = (s11f(xy) + st.oxf); y = (s11f(xy >> 16) + st.oyf);
    if (tex) { if (l + idx >= end) return; uvw = le32(l + idx++); }
    if (size == 0) { uint32_t wh; if (l + idx >= end) return; wh = le32(l + idx++); w = wh & 0x3ff; h = (wh >> 16) & 0x1ff; }
    else w = h = size == 1 ? 1 : size == 2 ? 8 : 16;
    if (!w || !h || !scissor(sc)) return;
    if (tex) { u0 = uvw & 0xff; v0 = (uvw >> 8) & 0xff; cx = ((uvw >> 16) & 0x3f) * 16; cy = (uvw >> 22) & 0x1ff; }
    for (int i = 0; i < 4; i++) {
        int rx = i & 1, ry = i >> 1;
        p[i].x = x + (rx ? w : 0); p[i].y = y + (ry ? h : 0);
        /* mirrored rectangles step texels downwards from u0/v0: pixel k samples u0-k,
           which with centre sampling means the left edge sits at u0+1 */
        if (tex && st.flipx) p[i].u = (float)(u0 + 1 - (rx ? w : 0));
        else p[i].u = (float)(u0 + (rx ? w : 0));
        if (tex && st.flipy) p[i].v = (float)(v0 + 1 - (ry ? h : 0));
        else p[i].v = (float)(v0 + (ry ? h : 0));
        if (tex && raw) p[i].r = p[i].g = p[i].b = 128.f; else colour(&p[i], w0);
    }
    t[0] = p[0]; t[1] = p[1]; t[2] = p[2]; t[3] = p[1]; t[4] = p[2]; t[5] = p[3];
    {
        int ua = st.flipx ? u0 - w + 1 : u0, va = st.flipy ? v0 - h + 1 : v0, uw = w, vh = h;
        if (ua < 0 || va < 0) { ua = va = 0; uw = vh = 256; }    /* wraps below 0: whole page */
        tex_setup(tex, raw, cx, cy, ua, ua + uw - 1, va, va + vh - 1, ta, ca);
        if (!prepare(tex, ta[0], ta[1], ta[2], ta[3], ca[0], ca[1], ca[2])) return;
    }
    if (!mask_prepare(t, 6, sc)) return;
    cur_spr = tex && hwr_sprites_apart; /* a textured sprite */
    emit(t, 6, semi, tex, sc);
    grow_bbox(t, 6, sc);
    hwr_stats.prims++;
}

int hwr_cmd(const uint32_t *list, const uint32_t *end)
{
    uint32_t w0, cmd;
    if (!dev || list >= end) return 0;
    w0 = le32(list); cmd = w0 >> 24;
    if ((cmd & 0xf8) == 0xe0) { ecmd(w0); return 0; }
    if (cmd == 0x02) {
        if (full_mode) do_fill(list, end);
        else if (list + 2 < end) {
            uint32_t xy = le32(list + 1), wh = le32(list + 2);
            close_batch();
            add_dirty(xy & 0x3f0, (xy >> 16) & 0x1ff, ((wh & 0x3ff) + 15) & ~15, (wh >> 16) & 0x1ff);
        }
        return full_mode;
    }
    if (cmd < 0x20 || cmd >= 0x80 || frame_overflow) return 0;
    if (cmd < 0x40) do_poly(list, end);
    else if (cmd < 0x60) do_line(list, end);
    else do_rect(list, end);
    if (nv + nsv >= CHUNK) submit();
    return full_mode;
}

void hwr_flush(void) { if (dev) close_batch(); }

static int shown_w, shown_h;            /* native size of the last picture shown */

void hwr_shown_size(int *w, int *h) { *w = dev ? shown_w : 0; *h = dev ? shown_h : 0; }

int hwr_present(int x, int y, int w, int h, int bgr24)
{
    int ok;
    if (!dev) return 0;
    shown_w = w; shown_h = h;
    close_batch();
    /* areas written only by uploads/fills (FMV, still screens) reach the target here */
    if (ndirty && !frame_overflow) { open_batch(); take_snapshot(); close_batch(); }
    hwr_stats.frames++;
    if (frame_snaps > (int)hwr_stats.max_snaps) hwr_stats.max_snaps = frame_snaps;
    ok = !frame_overflow && !bgr24 && w > 0 && h > 0 && x >= 0 && y >= 0 && x + w <= 1024 && y + h <= 512;
    if (frame_overflow) add_dirty(0, 0, 1024, 512);  /* resync everything next frame */
    frame_overflow = 0; frame_snaps = 0;
    if (ok) ok = dev->present(dev, x, y, w, h);
    if (!ok) hwr_stats.fallbacks++;
    return ok;
}

void hwr_attach(hwr_dev *d, const uint16_t *v, void (*cb)(void))
{
    dev = d; vram = v; sync_cb = cb; full_mode = d->full;
    vb = NULL;
    cvt_init();
    nv = nsv = nops = 0; grp_open = 0;
    batch_open = 0; ndirty = 0; frame_snaps = frame_overflow = 0;
    memset(&hwr_stats, 0, sizeof hwr_stats);
    add_dirty(0, 0, 1024, 512);
}

void hwr_attach_vram(const uint16_t *v)
{
    close_batch();
    vram = v;
    add_dirty(0, 0, 1024, 512);
}

void hwr_detach(void)
{
    close_batch();
    dev = 0;
}

int hwr_active(void) { return dev != 0; }
