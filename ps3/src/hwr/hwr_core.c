/* Hybrid hardware renderer, backend-independent part.
 * See hwr.h for the overall design. This file parses GP0 drawing commands,
 * tracks the drawing state, turns primitives into triangle lists and decides
 * when a fresh snapshot of native VRAM is needed. */
#include <string.h>
#include "hwr.h"
#include "../../../libpcsxcore/pgxp.h"

#define VCAP      65536   /* vertices per submit */
#define SCAP      32768   /* staged second-pass vertices */
#define OPCAP     4096
/* queued vertices that trigger a submit mid-frame, so the GPU draws while the
   CPU keeps emulating instead of getting the whole frame at the end */
#define CHUNK     2048
#define MAXDIRTY  32
#define MAXSNAPS  12      /* per frame; beyond this the frame falls back to software */

hwr_stats_t hwr_stats;

/* PGXP slice 1: 1 = texture correction active for the current batch, mirrored
 * fresh per batch by the backend (hwr_rsx.c) so the menu toggle works in-game. */
int hwr_pgxp_tex = 0;

static hwr_dev *dev;
static const uint16_t *vram;
static void (*sync_cb)(void);
static int full_mode;                  /* dev->full at attach */

typedef struct { float x, y, u, v, r, g, b, uw, vw, invw; } pv;   /* a primitive's vertex while it is set up
 * (uw/vw/invw: PGXP slice 1 pre-divided texcoords; identity when PGXP is off) */

static hwr_vtx vb[VCAP], sv[SCAP];
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
static int cur_spr;                    /* 1 = the primitive being emitted is a textured sprite (do_rect) */

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
    case 0xe5: st.ox = sx11(w); st.oy = sx11(w >> 11); break;
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
}

static void reserve(int n)
{
    if (nv + nsv + 2 * n > VCAP || nsv + n > SCAP || nops + 3 > OPCAP) submit();
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
    if (p->invw == 0.0f) {   /* producer left PGXP unset (fill/snapshot quads): identity, never divide by zero */
        d->uw = p->u; d->vw = p->v; d->invw = 1.0f;
    } else {
        d->uw = p->uw; d->vw = p->vw; d->invw = p->invw;
    }
    d->pad = 0.0f;
}

/* p: n vertices forming n/3 triangles */
static void emit(const pv *p, int n, int semi, int textured, const int *sc)
{
    int i;
    if (st.mchk && tmpl.mode != 4) hwr_stats.mask_checks++;   /* counted for the log */
    reserve(n);
    set_prog();
    prim_area(p, n, sc);
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

static void ps1_sample_shift(pv *t, int n)
{
    int k, i;
    const float dx = DX, dy0 = DY;
    for (k = 0; k + 2 < n; k += 3) {
        pv *a = &t[k], *b = &t[k + 1], *c = &t[k + 2];
        float e1x = b->x - a->x, e1y = b->y - a->y, e2x = c->x - a->x, e2y = c->y - a->y;
        float det = e1x * e2y - e2x * e1y, ix;
        float *pa[8] = { &a->u, &a->v, &a->r, &a->g, &a->b, &a->uw, &a->vw, &a->invw };
        a->x += dx; b->x += dx; c->x += dx; a->y += dy0; b->y += dy0; c->y += dy0;
        if (det > -0.5f && det < 0.5f) { pgxp_stats.degen_skip++; continue; }      /* degenerate: nothing is drawn anyway */
        ix = 1.f / det;
        for (i = 0; i < 8; i++) {
            size_t off = (size_t)((char *)pa[i] - (char *)a);
            float va = *pa[i], vb = *(float *)((char *)b + off), vc = *(float *)((char *)c + off);
            float gx = ((vb - va) * e2y - (vc - va) * e1y) * ix;
            float gy = ((vc - va) * e1x - (vb - va) * e2x) * ix;
            float s = -0.5f * gy + (i < 2 ? 1.f / 256.f : 0.f) + dx * gx + dy0 * gy;
            *(float *)((char *)a + off) += s; *(float *)((char *)b + off) += s; *(float *)((char *)c + off) += s;
        }
    }
}

static void colour(pv *p, uint32_t c)
{
    p->r = (float)(c & 0xff); p->g = (float)((c >> 8) & 0xff); p->b = (float)((c >> 16) & 0xff);
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

/* PGXP slice 1: fill uw/vw/invw for the triangles actually drawn. All-or-
 * nothing per triangle: every vertex must validate against the GTE cache
 * (same frame, exact integer match, float within ~1px), else the whole
 * triangle keeps affine mapping (identity values). rsx/rsy are the raw GP0
 * coordinates BEFORE the draw offset: the cache is keyed by the integer
 * position the GTE produced, and the draw offset is added by the GPU, so
 * the lookup key must be the pre-offset value. tsrc maps each drawn vertex
 * back to its source polygon vertex (a dropped oversized first half must not
 * shift the second half's keys). Called before ps1_sample_shift, so the
 * shift moves the pre-divided values along their own gradients like u/v. */
static void pgxp_resolve(pv *t, int nt, const int *tsrc, const int *rsx, const int *rsy, int tex, int *tex_ok)
{
    int k, i;
    for (k = 0; k + 2 < nt; k += 3) {
        float iw[3];
        int ok = tex && hwr_pgxp_tex;
        if (ok) {
            for (i = 0; i < 3; i++) {
                float fx, fy;
                int s = tsrc[k + i];
                if (!pgxp_lookup(rsx[s], rsy[s], &fx, &fy, &iw[i])) { ok = 0; break; }
            }
        }
        tex_ok[k / 3] = ok;
        if (tex) { if (ok) pgxp_stats.tex_persp++; else pgxp_stats.tex_affine++; }
        for (i = 0; i < 3; i++) {
            pv *v = &t[k + i];
            if (ok) { v->uw = v->u * iw[i]; v->vw = v->v * iw[i]; v->invw = iw[i]; }
            else { v->uw = v->u; v->vw = v->v; v->invw = 1.0f; }
        }
    }
}

static void do_poly(const uint32_t *l, const uint32_t *end, const pgxp_shadow_t *sh)
{
    uint32_t w0 = le32(l), cmd = w0 >> 24, uvw[4] = { 0 };
    int gour = cmd & 0x10, quad = cmd & 8, tex = cmd & 4, semi = cmd & 2, raw = cmd & 1;
    int n = quad ? 4 : 3, i, idx = 1, sc[4], ta[4], ca[4] = { 0 }, cx = 0, cy = 0, spr = 0;
    int umin = 255, umax = 0, vmin = 255, vmax = 0, nt = 0;
    int rsx[4], rsy[4], tsrc[6], xy_idx[4];   /* PGXP: raw GP0 coords + drawn-vertex source map + command-word index of each vertex */
    int tex_ok[2] = { 0, 0 };   /* PGXP log: per-triangle texture status from pgxp_resolve */
    pv p[4], t[6];
    for (i = 0; i < n; i++) {
        uint32_t c = w0, xy;
        if (gour && i) { if (l + idx >= end) return; c = le32(l + idx++); }
        if (l + idx >= end) return;
        xy = le32(l + idx++);
        xy_idx[i] = idx - 1;
        rsx[i] = sx11(xy); rsy[i] = sx11(xy >> 16);   /* before the draw offset (see pgxp_resolve) */
        p[i].x = (float)(rsx[i] + st.ox); p[i].y = (float)(rsy[i] + st.oy);
        if (tex) {
            if (l + idx >= end) return;
            uvw[i] = le32(l + idx++);
            {
                int u = uvw[i] & 0xff, v = (uvw[i] >> 8) & 0xff;
                p[i].u = (float)u; p[i].v = (float)v;
                if (u < umin) umin = u;
                if (u > umax) umax = u;
                if (v < vmin) vmin = v;
                if (v > vmax) vmax = v;
            }
        } else p[i].u = p[i].v = 0.f;
        if (tex && raw) { p[i].r = p[i].g = p[i].b = 128.f; } else colour(&p[i], c);
        /* PGXP gate: identity unless a correction overwrites it below
           (pgxp_resolve when texture is on, substitution when the master is
           on). This is what makes skipping pgxp_resolve safe: without it,
           uw/vw/invw here would be uninitialized stack values, not identity
           (do_poly never set them — resolve was their only writer). */
        p[i].uw = p[i].u; p[i].vw = p[i].v; p[i].invw = 1.0f;
    }
    if (tex) {
        uint32_t clut = uvw[0] >> 16;
        cx = (clut & 0x3f) * 16; cy = (clut >> 6) & 0x1ff;
        set_texpage(uvw[1] >> 16);
    }
    if (!scissor(sc)) return;
    if (!too_big(&p[0], &p[1], &p[2])) { t[nt] = p[0]; tsrc[nt++] = 0; t[nt] = p[1]; tsrc[nt++] = 1; t[nt] = p[2]; tsrc[nt++] = 2; } else pgxp_stats.toobig_drop++;
    if (quad && !too_big(&p[1], &p[2], &p[3])) { t[nt] = p[1]; tsrc[nt++] = 1; t[nt] = p[2]; tsrc[nt++] = 2; t[nt] = p[3]; tsrc[nt++] = 3; } else if (quad) pgxp_stats.toobig_drop++;
    if (!nt) return;
    /* axis-aligned check on the triangles actually drawn, before the sample
       shift (which preserves all vertex differences, so it cannot change it) */
    spr = tex ? poly_is_sprite(t, nt) : 0;
    /* the PS1 never draws the right and bottom edge, so a polygon mapping texels
       0..64 shows 0..63: clamping to 64 let upscaled edge pixels pick texel 64,
       a thin line of the neighbouring tile along polygon seams (Tekken 3 skies) */
    if (umax > umin) umax--;
    if (vmax > vmin) vmax--;
    /* PGXP gate: when texture correction is off (master off, or the texture
       sub-option off), skip the resolve call entirely — no cache lookups,
       identity already set in vertex setup above. tex_ok stays {0,0} (its
       zero-initialization); nothing reads it. The affine count is kept
       honest: these triangles are drawn affine. */
    if (hwr_pgxp_tex)
        pgxp_resolve(t, nt, tsrc, rsx, rsy, tex, tex_ok);
    else if (tex)
        for (i = 0; i + 2 < nt; i += 3) pgxp_stats.tex_affine++;
    /* PGXP T2+T3+edge: vertex stabilization + shadow texture from
       provenance-validated shadows. Direct-write words carry T2 register
       shadows; DMA words carry T3 RAM shadows (pgxp_cmd_shadow_at resolves
       both). The shift is sub-pixel (the GTE fraction). Edge consistency
       comes from the position map (see below): every vertex at
       a given integer position in a batch gets the same treatment.
       Gated on the master switch only (no new setting). In VRAM-check mode
       this diverges from the software reference exactly like texture
       correction already does. */
    if (sh && pgxp_track) {
        /* Edge-consistent per-vertex substitution (option A), with the
           PGXP-lite hybrid on top: each drawn vertex is decided
           independently — no triangle veto. A vertex with a validated, live
           own shadow (sv->i == word, within PGXP_AGE game frames)
           substitutes from it. A vertex without one borrows the precise
           float recorded in the position map for its integer position
           (rsx, rsy), if any — so every vertex at a given integer position
           in this batch gets the SAME treatment (same float, or all
           integer), regardless of draw order. A vertex with neither stays
           integer with exactly what pgxp_resolve gave it. The pre-pass
           records every validated own shadow before any draw, so a
           no-shadow vertex drawn BEFORE its shadowed neighbour still
           borrows. Substituted positions always pair with shadow/borrowed
           invw, never with affine or cache texture, so tex_mixed stays a
           violation detector (always 0). Use-time validation (sv->i ==
           word) is what makes RAM lists safe: a word rewritten after its
           shadow was recorded fails here and falls back. Known limitation:
           cross-batch order-dependence persists (a position drawn integer
           in batch 1 and substituted in batch 2 of the same frame still
           disagrees) — a frame-wide pre-pass is impossible in a streaming
           renderer; this is rare and far fewer cracks than per-triangle
           veto. ox cancels in the delta.
           Hybrid additions (on top of the above, in order per triangle):
           (1) ambiguity: when a position has TWO map candidates (two
           depths at one pixel), pgxp_pick_candidates chooses per triangle
           by projection-seq proximity, then depth-fit scoring — the first
           writer is kept for unambiguous cases. (2) weld: after the
           choice, a vertex within 0.25px of a pixel edge and 3% depth of a
           neighbour snaps to the neighbour's exact place (same-pixel snap
           for own shadows, neighbour snap for own and borrowed), so a
           joint shared by two body parts lands on one place. (3) the
           use-time guard (> 1.5px) still runs last, on the final resolved
           position, as the failure detector. */
        int tri_sub[2] = { 0, 0 }, tri_shtex[2] = { 0, 0 };
        int k, j;
        /* Pre-pass: record every validated, live own shadow into the
           position map (first-writer wins), before any substitution. A
           stale shadow is skipped silently here (its age is counted once,
           in the draw pass below, when the vertex is actually used). */
        for (k = 0; k + 2 < nt; k += 3) {
            for (j = 0; j < 3; j++) {
                int s = tsrc[k + j];
                const pgxp_shadow_t *sv = &sh[xy_idx[s]];
                if (pgxp_shadow_live(sv) && sv->i == le32(l + xy_idx[s]))
                    pgxp_edge_record(rsx[s], rsy[s], sv);
            }
        }
        /* Draw pass, per triangle: gather candidates (one own shadow, or up
           to two map candidates), resolve ambiguity per triangle, then
           weld + guard + apply per vertex. */
        for (k = 0; k + 2 < nt; k += 3) {
            int tri = k / 3;
            pgxp_edge_cand_t cand[3][2];
            int nc[3], ownf[3], pick[3], csx[3], csy[3];
            pgxp_stats.sub_verts += 3;
            for (j = 0; j < 3; j++) {
                int s = tsrc[k + j];
                const pgxp_shadow_t *sv = &sh[xy_idx[s]];
                csx[j] = rsx[s]; csy[j] = rsy[s];
                nc[j] = 0; ownf[j] = 0; pick[j] = 0;
                if (sv->valid && sv->i == le32(l + xy_idx[s])) {
                    if (pgxp_shadow_live(sv)) {
                        nc[j] = 1; ownf[j] = 1;
                        cand[j][0].x = sv->x; cand[j][0].y = sv->y;
                        cand[j][0].invw = sv->invw;
                        cand[j][0].seq = sv->seq; cand[j][0].frame = sv->frame;
                        pgxp_edge_record(rsx[s], rsy[s], sv); /* belt and braces: the pre-pass already covered this vertex */
                    } else {
                        /* Valid shadow, exact word, but older than PGXP_AGE
                           game frames: expired before use. Count it once
                           here (the pre-pass skipped it silently) and fall
                           through to the map borrow below. */
                        pgxp_stats.aged_out++;
                    }
                }
                if (nc[j] == 0)
                    nc[j] = pgxp_edge_lookup_n(rsx[s], rsy[s], cand[j]);
            }
            pgxp_pick_candidates(3, csx, csy, nc, cand, pick);
            for (j = 0; j < 3; j++) {
                int s = tsrc[k + j];
                pv *v = &t[k + j];
                float fx, fy, fi, dx, dy;
                int borrowed;
                unsigned src_frame;
                if (nc[j] == 0)
                    continue; /* stays integer + pgxp_resolve texture */
                borrowed = !ownf[j];
                fx = cand[j][pick[j]].x; fy = cand[j][pick[j]].y; fi = cand[j][pick[j]].invw;
                src_frame = cand[j][pick[j]].frame;
                /* Weld: snap to a neighbour's exact place (same-pixel for
                   own shadows, neighbour-pixel for own and borrowed). Runs
                   after the ambiguity choice and before the guard, so the
                   guard still sees the final position. */
                {
                    float wx, wy, wi;
                    unsigned wseq;
                    if (pgxp_weld_try(rsx[s], rsy[s], ownf[j], fx, fy, fi, src_frame, &wx, &wy, &wi, &wseq)) {
                        fx = wx; fy = wy; fi = wi;
                        pgxp_stats.welds++;
                    }
                }
                /* Use-time guard (option 1): block teleported vertices.
                   A legit shadow differs from its integer by < 1px (same
                   GTE value, truncated vs exact), but a clamped GTE float
                   can sit thousands of px away while its integer (clamped
                   by limG1/limG2) still validates exactly — the exact-match
                   check alone cannot catch it. Skip substitution when
                   either axis exceeds 1.5px (the big-counter threshold;
                   exactly 1.5 still substitutes): the vertex keeps its
                   integer position + resolve texture (stock behaviour),
                   the triangle is NOT dropped, and too_big already ran on
                   integers before substitution. The check is on the final
                   resolved (fx,fy), so it covers own and borrowed alike.
                   One guard per vertex (the gather above yields exactly one
                   resolved position per vertex, and the integer branch
                   continues before reaching here), so guard_skip cannot
                   double-count. A skipped vertex counts ONLY as guard_skip
                   — not applied, borrowed, noted, or big — so big=0 +
                   guard_skip>0 means the guard worked, and a nonzero big
                   means an unguarded substitution path. ox cancels in the
                   delta (both sides are pre-draw-offset, like sub_big). */
                dx = fx - (float)rsx[s]; if (dx < 0) dx = -dx;
                dy = fy - (float)rsy[s]; if (dy < 0) dy = -dy;
                if (dx > 1.5f || dy > 1.5f) { pgxp_stats.guard_skip++; continue; }
                pgxp_stats.sub_applied++;
                if (borrowed) pgxp_stats.sub_borrowed++;
                pgxp_note_substituted(rsx[s], rsy[s]);
                tri_sub[tri] = 1; tri_shtex[tri] = 1;
                if (dx > pgxp_stats.sub_max_dx) pgxp_stats.sub_max_dx = dx;
                if (dy > pgxp_stats.sub_max_dy) pgxp_stats.sub_max_dy = dy;
                /* Applied deltas are <= 1.5 by the guard above, so this
                   must stay 0; it stays as the failure detector. */
                if (dx > 1.5f || dy > 1.5f) pgxp_stats.sub_big++;
                v->x = fx + (float)st.ox;
                v->y = fy + (float)st.oy;
                v->uw = v->u * fi;
                v->vw = v->v * fi;
                v->invw = fi;
            }
        }
        /* PGXP log: mixed-precision triangles (positions substituted but
           texture NOT from shadow/borrowed invw). Every substituted vertex
           pairs with shadow/borrowed invw by construction, so this is
           always 0; the counter stays as a rule-violation detector. */
        if (tex)
            for (i = 0; i < nt / 3; i++)
                if (tri_sub[i] && !tri_shtex[i]) pgxp_stats.tex_mixed++;
    }
    ps1_sample_shift(t, nt);
    tex_setup(tex, raw, cx, cy, umin, umax, vmin, vmax, ta, ca);
    if (!prepare(tex, ta[0], ta[1], ta[2], ta[3], ca[0], ca[1], ca[2])) return;
    if (!mask_prepare(t, nt, sc)) return;
    cur_spr = spr; emit(t, nt, semi, tex, sc);
    grow_bbox(t, nt, sc);
    hwr_stats.prims++;
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
        q[i].invw = 1.0f;   /* PGXP: untextured, identity (uw/vw stay 0 = u/v) */
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
    prev.invw = 1.0f;   /* PGXP: lines are untextured, identity (uw/vw stay 0 = u/v) */
    colour(&prev, w0);
    { uint32_t xy = le32(l + 1); prev.x = (float)(sx11(xy) + st.ox); prev.y = (float)(sx11(xy >> 16) + st.oy); }
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
        cur.invw = 1.0f;   /* PGXP: see prev above */
        colour(&cur, c);
        cur.x = (float)(sx11(xy) + st.ox); cur.y = (float)(sx11(xy >> 16) + st.oy);
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
    x = (float)(sx11(xy) + st.ox); y = (float)(sx11(xy >> 16) + st.oy);
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
        /* PGXP: sprites are axis-aligned with no perspective to correct: identity */
        p[i].uw = p[i].u; p[i].vw = p[i].v; p[i].invw = 1.0f;
    }
    t[0] = p[0]; t[1] = p[1]; t[2] = p[2]; t[3] = p[1]; t[4] = p[2]; t[5] = p[3];
    {
        int ua = st.flipx ? u0 - w + 1 : u0, va = st.flipy ? v0 - h + 1 : v0, uw = w, vh = h;
        if (ua < 0 || va < 0) { ua = va = 0; uw = vh = 256; }    /* wraps below 0: whole page */
        tex_setup(tex, raw, cx, cy, ua, ua + uw - 1, va, va + vh - 1, ta, ca);
        if (!prepare(tex, ta[0], ta[1], ta[2], ta[3], ca[0], ca[1], ca[2])) return;
    }
    if (!mask_prepare(t, 6, sc)) return;
    cur_spr = tex ? 1 : 0; emit(t, 6, semi, tex, sc);
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
    if (cmd < 0x40) do_poly(list, end, pgxp_cmd_shadow_at(list, gpulib_get_cmd_buffer()));
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
    pgxp_next_frame();   /* PGXP slice 1: entries from older frames stop matching */
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
