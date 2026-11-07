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

/* the RAM byte offset of a packet pointer, or -1 if it is not a RAM address
 * (a copied command) or RAM is mapped at address 0 (host tests). Shared by
 * the interpolation matcher for its address key; independent of pgxp_lite_on. */
int pgxp_ram_off(const void *ptr);

/* the renderer: the vertices projected to this word recently, c[0..1]; returns
   how many (0: not known; 2: two different vertices rounded to that pixel) */
int pgxp_lite_cands(uint32_t sxy, struct pgxp_cand *c);

/* the renderer: fetch the entries of n corners (packet word addresses, values) */
void pgxp_lite_prefetch(const uint32_t *const *ptr, const uint32_t *words, int n);

/* the first of them, with z[1] the second one's depth (0 none) */
int pgxp_lite_find(uint32_t sxy, float *dx, float *dy, float *z);

/* ---- PGXP corrector: a RAM shadow that supplies what lite misses ----
 *
 * Lite only sees vertices the game stores with SWC2 straight from the GTE.
 * World geometry that reaches RAM through the CPU instead (MFC2 to a GPR,
 * then SW to RAM — Driver 2's world) is invisible to it. The corrector
 * follows those values through the CPU: GTE register shadows (SXY 12/13/14),
 * GPR shadows, and one shadow per RAM/scratchpad word, validated at every
 * step against the exact integer the game produced (a stale shadow fails
 * validation and falls back to invalid, never to a wrong value). At draw
 * time, when lite's word AND address tables both miss a corner, the renderer
 * asks the RAM shadow by the packet's address (pgxp_corr_cand_at); a lite
 * hit always wins, the corrector never overrides one. The candidate carries
 * the position (dx, dy) and the depth (z = SZ3, lite's camera-units scale:
 * the GTE's SZ3 is the raw projection depth >> 12, and lite's z is the raw
 * projection depth / 4096 — the same number; see pgxp_lite.c), so mode 2
 * gets perspective for those corners too, through lite's own shader path.
 *
 * What is NOT here (deliberately): the research branch's edge map, borrow,
 * hybrid welds/ambiguity, texture-precision counters, do_poly substitution
 * and per-frame wipe are orphan code — none of it is ported, and the
 * corrector never writes lite's tables (its answers are transient, at
 * draw-lookup time only), so switching it off leaves lite byte-identical.
 * The game-frame clock is lite's (pgxp_lite_frame); the projection sequence
 * is shared with lite (pgxp_seq), so one corner's lite and corrector
 * candidates order on the same clock.
 *
 * Shared gate (both correctors): each tracking hook is gated by the flag
 * of the corrector(s) that consume it, so the hooks run exactly when a
 * consumer needs them and never otherwise:
 *   GTE RTPS/RTPT -> cache fill: pgxp_cull_track only.
 *   GTE RTPS/RTPT -> register shadow: pgxp_corr_track only.
 *   MFC2/MTC2 (cop2_notify), loads/stores (interpreter + Lightrec emitter),
 *   draw-time join: pgxp_corr_track only.
 *   NCLIP correction: pgxp_cull_track only.
 * Each track flag is its setting AND lite mode (derived in pgxp_corr_set /
 * pgxp_cull_set, which the front end calls wherever it calls pgxp_lite_set),
 * so a corrector is effective only while lite is on. Union property: the
 * tracking hooks are active when EITHER corrector is on, and totally dead
 * when BOTH are off — all correctors off is exactly lite-only behaviour. */
extern int pgxp_corr_track;         /* 1 while the RAM-shadow corrector is effective (setting AND lite mode) */
extern int pgxp_cull_track;         /* 1 while the culling corrector is effective (setting AND lite mode) */
extern unsigned int pgxp_corr_lookups, pgxp_corr_hits, pgxp_corr_guards;   /* statistics (log) */
extern unsigned int pgxp_corr_nclip_total, pgxp_corr_nclip_flip, pgxp_corr_nclip_restore;   /* culling statistics (log) */

/* a shadow: the precise float position (x, y) + depth (invw = 1/SZ3) for the
 * integer word i. seq = projection number (one per GTE projection, in
 * projection order, shared with lite's pgxp_seq); frame = the game frame
 * (pgxp_lite_frame) the vertex was projected in; a shadow older than
 * PGXP_AGE game frames is dead. */
typedef struct { int valid; unsigned i; float x, y, invw; unsigned seq; unsigned frame; } pgxp_corr_shadow_t;

/* map a PS1 address to its RAM/scratchpad shadow slot, or NULL when the
 * address is not RAM or scratchpad. Segments are stripped like lightrec's
 * kunseg(), so all aliases of one physical word share one slot. */
const pgxp_corr_shadow_t *pgxp_corr_ram_shadow_at(unsigned psx_addr);

/* GTE hooks (gte.c): record a projection into the SXY register shadow
 * (RTPS mirrors the FIFO shift; RTPT writes one vertex straight). */
void pgxp_corr_on_rtps(unsigned packed, float x, float y, float invw, int valid);
void pgxp_corr_on_rtpt(int v, unsigned packed, float x, float y, float invw, int valid);
/* register moves: MFC2/MTC2 between GTE and GPR (from_ram=1 means the value
 * came from RAM and is always unproven); the Lightrec cop2_notify entry
 * point decodes the raw opcode and dispatches to these. */
void pgxp_corr_on_mfc2(int rd, int rt, unsigned val);
void pgxp_corr_on_mtc2(int rt, int rd, unsigned data, int from_ram);
void pgxp_corr_on_notify(unsigned opcode, unsigned data);
/* RAM shadow flow for full-word transfers (psxinterpreter.c, Lightrec):
 * addr = PS1 effective address, word = the game value transferred,
 * rt = GPR (is_cop2=0) or GTE register (is_cop2=1, SXY regs 12/13/14 only;
 * 15 maps to 14 like MFC2). Stores copy the source shadow as-is when it
 * claims exactly this word, else clear the slot; loads carry the slot into
 * the destination register only when the slot is valid and claims exactly
 * this word, else invalidate it. */
void pgxp_corr_on_sw_ram(unsigned addr, unsigned word, int rt, int is_cop2);
void pgxp_corr_on_lw_ram(unsigned addr, unsigned word, int rt, int is_cop2);
/* sub-word + unaligned: stores apply the exact-match-or-invalidate rule on
 * the merged post-write word (read back by the caller after the write,
 * RAM/scratchpad only — the caller passes 0 for any other address);
 * LB/LH invalidate the destination shadow; LWL/LWR at a word-aligned
 * address behave exactly as LW, unaligned they invalidate. */
void pgxp_corr_on_sb_ram(unsigned addr, unsigned merged, int rt);
void pgxp_corr_on_sh_ram(unsigned addr, unsigned merged, int rt);
void pgxp_corr_on_swl_ram(unsigned addr, unsigned merged, int rt);
void pgxp_corr_on_swr_ram(unsigned addr, unsigned merged, int rt);
void pgxp_corr_on_lb_ram(int rt);
void pgxp_corr_on_lh_ram(int rt);
void pgxp_corr_on_lwl_ram(unsigned addr, unsigned word, int rt);
void pgxp_corr_on_lwr_ram(unsigned addr, unsigned word, int rt);

/* drop every corrector shadow at once (corrector toggle, or a new title, so
 * no shadow from the previous title can be borrowed). The game-frame clock
 * is NOT reset: it is a clock, and old stamps age out by the arithmetic. */
void pgxp_corr_invalidate(void);
/* set the corrector with transition handling: a value change invalidates
 * the shadows first. The front end calls this wherever it calls
 * pgxp_lite_set (setting change, launch). */
void pgxp_corr_set(int on);
/* the Lightrec gate (lightrec/plugin.c): re-points cop2_notify and flushes
 * every compiled block when the corrector value changes, so all live code
 * matches the new registration. */
void lightrec_pgxp_corr_gate(int on);

/* the renderer: the vertex word at ptr (in PS1 RAM, a GPU packet) when the
 * game stored it there through the CPU (MFC2 -> SW): 1 and c filled
 * (dx, dy = sub-pixel offset, z = depth in lite's scale, 0 unknown),
 * -1 not known by its address. Only ever called on a lite miss (see
 * hwr_core.c); returns -1 at once while the corrector is off. A shadow
 * more than 1.5px from its integer is skipped (Build A guard) and counted
 * in pgxp_corr_guards. */
int pgxp_corr_cand_at(const void *ptr, uint32_t word, struct pgxp_cand *c);

/* ---- culling corrector (NCLIP): a second, separate corrector ----
 * The slice-1 cache (below) is filled by the GTE hooks and read only by
 * the NCLIP decision (pgxp_corr_nclip). The slice-1 texture correction
 * that also read this cache on the research branch is NOT ported (a
 * different consumer, out of scope). */

/* Record a GTE-projected vertex into the slice-1 cache (gte.c, RTPS/RTPT):
 * (sx, sy) is the integer screen position the game got, (x, y) the
 * pre-truncation float position, z the projected depth (GTE SZ, > 0).
 * Vertices at/behind the camera (z <= 0) are not recorded. */
void pgxp_corr_push(int sx, int sy, float x, float y, int z);

/* Drop every cache entry at once (culling toggle, or a new title). The
 * game-frame clock is NOT reset. */
void pgxp_corr_cache_invalidate(void);
/* Set the culling corrector with transition handling: the effective track
 * is the setting AND lite mode (a value change invalidates the cache
 * first). The front end calls this wherever it calls pgxp_lite_set. */
void pgxp_cull_set(int on);

/* NCLIP decision (gte.c, gteNCLIP_): recompute the winding from the precise
 * GTE positions when all three FIFO vertices validate in the cache.
 * Returns 1 and fills *det with the corrected determinant (already
 * sliver-bumped, as s64) when the precise winding is nonzero; 0 (the
 * integer result is kept) when any vertex misses, or the precise winding
 * is exactly zero. Counts nclip_total on a correction, nclip_flip when
 * the integer and precise windings differ in sign, nclip_restore when the
 * integer winding was zero. A sliver with a nonzero precise winding is
 * bumped to +/-1 so the game's sign test sees it unambiguously
 * (DuckStation's sliver rule). */
int pgxp_corr_nclip(int sx0, int sy0, int sx1, int sy1, int sx2, int sy2, int64_t *det);

#ifdef __cplusplus
}
#endif

#endif
