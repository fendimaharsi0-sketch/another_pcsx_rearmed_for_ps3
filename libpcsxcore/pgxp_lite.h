/*
 * "PGXP-lite": sub-pixel screen positions for the polygons a hardware renderer
 * draws, without following values through the CPU (full PGXP does that at
 * every load and store, too slow for a dynarec on a slow host).
 *
 * The GTE's perspective transforms (RTPS, RTPT) round each projected vertex to
 * whole pixels, which makes 3D wobble. Here each result is also computed in
 * floating point and kept, keyed by the 32-bit word the game will copy into the
 * GPU packet (SX in the low half, SY in the high half). When a renderer meets
 * that exact word as a polygon vertex, it can draw the vertex at the precise
 * position instead. A word that was not produced by the GTE recently (2D, or
 * changed by the game) is simply not found.
 */
#ifndef PGXP_LITE_H
#define PGXP_LITE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern int pgxp_lite_on;            /* set by the front end: 1 positions, 2 also depth (textures) */
extern unsigned int pgxp_lite_frame; /* game frames (see pgxp_lite_vsync / pgxp_lite_flip) */
extern unsigned int pgxp_lite_stores, pgxp_lite_hits, pgxp_lite_misses, pgxp_lite_welds;   /* statistics */
extern unsigned long long pgxp_lite_ticks;   /* time spent working out the projections (timebase ticks, PowerPC) */

/* the front end, once per screen refresh; gpulib, at each display address write
   (GP1(05h)): together they count the game's own frames, which decide when a
   vertex is too old to belong to what is being drawn */
void pgxp_lite_vsync(void);
void pgxp_lite_flip(uint32_t disp);

/* gte.c: a projection (only with pgxp_lite_on), the GTE's own integers: x, y, z
   = TR * 4096 + R * V, the divider's h, OFX, OFY, the result's SX, SY, near: the
   divider's result was used (SZ3 * 2 > H), | 2: widescreen (gte_widescreen).
   Worked out later (see pgxp_lite.c). */
void pgxp_lite_project(int64_t x, int64_t y, int64_t z, uint32_t h, int32_t ofx, int32_t ofy,
                       int16_t sx, int16_t sy, int near);

/* Forget every stored vertex (word table, address table, GTE FIFO, queued
   events): after PGXP was off, the frame counter stood still, so entries
   from before the switch still count as fresh. Called on the OFF -> ON
   transition; the statistics and the frame/projection counters stay. */
void pgxp_lite_invalidate(void);

/* Set the mode with transition handling: OFF -> ON (0 -> 1 or 2) invalidates
   the tables first; 1 <-> 2 and ON -> OFF do not. */
void pgxp_lite_set(int on);
/* the same with the vertex already worked out: offset dx, dy, depth z */
void pgxp_lite_store(uint32_t sxy, float dx, float dy, float z);

/* SWC2 of a GTE screen coordinate (SXY0-2, SXYP) to PS1 address addr: when the
   value is one of the last projected vertices, that RAM word is remembered as
   that vertex (the CPU interpreter and Lightrec call it; see lightrec_swc2_hook) */
void pgxp_lite_swc2(uint32_t addr, uint32_t val);

/* a vertex the renderer may take for a word: its sub-pixel offset (dx, dy in
   [-2, 2]), its depth (camera units; 0 unknown: behind the camera) and its
   projection number (vertices projected together have close numbers) */
struct pgxp_cand { float dx, dy, z; uint32_t seq; };

/* the renderer: the vertex word at ptr (in PS1 RAM, a GPU packet) when the game
   stored it there straight from the GTE: 1, -1 not known by its address */
int pgxp_lite_cand_at(const void *ptr, uint32_t word, struct pgxp_cand *c);
extern unsigned int pgxp_lite_addr_hits;

/* the renderer: the vertices projected to this word recently, c[0..1]; returns
   how many (0: not known; 2: two different vertices rounded to that pixel) */
int pgxp_lite_cands(uint32_t sxy, struct pgxp_cand *c);

/* the renderer: fetch the entries of n corners (packet word addresses, values) */
void pgxp_lite_prefetch(const uint32_t *const *ptr, const uint32_t *words, int n);

/* the first of them, with z[1] the second one's depth (0 none) */
int pgxp_lite_find(uint32_t sxy, float *dx, float *dy, float *z);
/* ---- Culling correction (NCLIP): reads the lite word table ----
 * History: the RAM-shadow corrector (and Design A, its NCLIP consumer)
 * were parked 2026-11-09 and removed from the tree — the shadow's
 * coordinates were never verified against the picture the way lite's
 * are, and the hardware verdict on the shadow itself was no visible
 * help for real perf cost. The code and the mode2w design doc are kept
 * in the local archive (ps3-corrector-archive/), not in the repo.
 * NCLIP now reads the SAME table the renderer draws from, so culling
 * and drawing cannot disagree per triangle; see pgxp_lite_nclip below.
 * Gate: pgxp_cull_track is the setting AND lite mode (derived in
 * pgxp_cull_set, which the front end calls wherever it calls
 * pgxp_lite_set), so culling correction is effective only while lite
 * is on. The statistics below feed the culling log lines. */
extern int pgxp_cull_track;
extern unsigned int pgxp_nclip_total, pgxp_nclip_flip, pgxp_nclip_restore, pgxp_nclip_nogte;
extern unsigned int pgxp_nclip_guess, pgxp_nclip_dzero;

/* Set the culling correction: the effective track is the setting AND
 * lite mode. The front end calls this wherever it calls
 * pgxp_lite_set. */
void pgxp_cull_set(int on);

/* NCLIP decision (gte.c, gteNCLIP_): for each of the three SXY vertices,
 * look up the packed word in the lite table (pgxp_lite_cands). A word
 * with no candidate keeps the integer result (counted in
 * pgxp_nclip_nogte); a word with two candidates is settled by do_poly's
 * first seq-proximity pass (see pgxp_lite.c). The precise determinant
 * (cross-product form) decides the winding; a sliver (0,1) is nudged to
 * +-1, the same rule the draw's precise winding would give (a zero-area
 * triangle rasterizes to zero pixels on real hardware, so a sliver must
 * not flip a face away). Returns 1 with *det set when the precise
 * winding is used, 0 when the integer result is kept. The counters
 * feed the culling log lines; they are reset by the log print.
 * Two more, for the residual-crack diagnosis: pgxp_nclip_guess counts a
 * triangle whose precise winding rests on an ambiguous corner settled by
 * default (the seq test could not tell the two candidates apart, so the
 * first one was taken - a guess, not a decision); pgxp_nclip_dzero
 * counts a triangle whose precise determinant was exactly zero (integer
 * kept, the other fallback). Both are per triangle, counted only when
 * the precise winding is actually used (guess) or fell back on zero
 * (dzero); a triangle that fell back on a missing word (nogte) counts
 * in neither. */
int pgxp_lite_nclip(int sx0, int sy0, int sx1, int sy1, int sx2, int sy2, int64_t *det);

#ifdef __cplusplus
}
#endif

#endif
