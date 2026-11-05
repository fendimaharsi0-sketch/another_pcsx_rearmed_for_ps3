/* PGXP slice 1: float vertex tracking for texture + culling correction.
 *
 * Original code written for this fork following the documented PGXP mechanism
 * (a parallel float shadow of the GTE dataflow, validated against the game's
 * exact integer results). It is NOT copied from DuckStation (GPLv3) or any
 * other PGXP implementation, and it deliberately does less than full PGXP:
 * no CPU tracking, no shadow RAM, no depth buffer, no vertex cache.
 *
 * Design: GTE RTPS/RTPT record the pre-truncation float screen position and
 * the projected depth for each projected vertex, keyed by the integer screen
 * position the game actually got. The RSX layer looks a vertex up by that
 * integer position and uses the float depth only when the entry validates:
 * recorded within PGXP_AGE game frames, exact integer match, float within
 * ~1px of the integer. Anything else falls back to the integer vertex
 * unchanged, so a cache miss can never corrupt geometry. Vertices the CPU
 * transforms itself never hit the cache (same documented limitation as the
 * pre-removal design).
 *
 * Scope of slice 1: texture correction (perspective-correct UV interpolation
 * from the validated depth) and culling correction (NCLIP recomputed from the
 * validated positions). Screen positions are NOT substituted: slice 1 moves no
 * vertex, so shared edges cannot crack the way XY substitution did before.
 */

#ifndef __PGXP_H__
#define __PGXP_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* T1: base-tracking shadows (no visual effect yet). GTE pushes also record
 * their precise value into a per-register shadow (SXY regs 12/13/14); the
 * Lightrec cop2_notify hook and the interpreter's MFC2/MTC2 cases propagate
 * shadows GTE<->CPU registers, validating at every step against the exact
 * integer the game produced (a stale shadow fails validation and falls back
 * to invalid, never to a wrong value). r0 never carries a shadow. Shadows
 * live at most PGXP_AGE game frames (see above), stamped at projection time
 * and carried along with every copy, so a shadow recorded before the master
 * was switched off (or before a title change) can never be used again once
 * it is older than the window. Registering cop2_notify makes the Lightrec emitter fall back to C calls
 * for MFC2/MTC2 even when tracking is off (block-compile-time choice):
 * unmeasured performance cost, revisit if FPS drops. */

/* gte_pgxp bitmask, mirrored from the frontend settings (ps3/src/config.c):
 * bit 0 = texture correction active (master on + texture on)
 * bit 1 = culling correction active (master on + culling on)
 * 0 = tracking off entirely (master off, or both sub-options off) */
#define PGXP_TEX  1
#define PGXP_CULL 2
extern int gte_pgxp;

/* Game-frame aging (PGXP-lite hybrid): a shadow lives at most PGXP_AGE game
 * frames. The game-frame clock (pgxp_frame) advances on GP1(05h) display-area
 * flips (pgxp_on_flip, from the GPU) and, for games that never flip, on every
 * 8 frontend vsyncs (counted in pgxp_next_frame, one presented frame = one
 * vsync). Shadows carry the frame they were projected in plus a projection
 * sequence number (pgxp_seq, one per GTE projection, in projection order);
 * the sequence travels with the shadow through registers and RAM, so the
 * renderer can tell which of two candidates for one pixel is newer. Age is
 * enforced at use time (pgxp_shadow_live, edge/cache lookups); the per-frame
 * wipes are gone, so a static display list keeps its shadows for up to 3
 * game frames instead of dying at the next present. */
#define PGXP_AGE 3
extern unsigned pgxp_frame;
/* GP1(05h) display-area flip: advance the game-frame clock when the display
 * address changed (a game that always sets the same address does not advance
 * here; the vsync fallback in pgxp_next_frame covers it). */
void pgxp_on_flip(unsigned disp);

/* Record a GTE-projected vertex: (sx, sy) is the integer screen position the
 * game got, (x, y) the pre-truncation float position, z the projected depth
 * (GTE SZ, > 0). Vertices at/behind the camera (z <= 0) are not recorded. */
void pgxp_push(int sx, int sy, float x, float y, int z);

/* Look a vertex up by the integer screen position the game put in a GP0
 * command. Returns 1 and fills (x, y, invw) on a validated hit, 0 otherwise;
 * the caller must then use the integer vertex unchanged. */
int pgxp_lookup(int sx, int sy, float *x, float *y, float *invw);

/* Advance one presented frame (one frontend vsync): the game-frame clock
 * ages (8 vsyncs without a GP1(05h) flip = one game frame), and the
 * substitution-state flip counters are updated. Shadows are NOT wiped here
 * any more (they age out at use time, see PGXP_AGE above). Called once per
 * presented frame from the frontend (hwr_present) — never from non-present
 * paths; a master toggle calls pgxp_invalidate_all instead. */
void pgxp_next_frame(void);
/* Drop every shadow, cache entry, edge-map entry and flip-table entry at
 * once (master toggle, or a new title starting in config_game_begin, so no
 * shadow from the previous title can be borrowed). The game-frame clock is
 * NOT reset: it is a clock, and old stamps age out by the arithmetic. */
void pgxp_invalidate_all(void);

/* T1: 1 when the PGXP master switch is on (mirrored from cfg by
 * config_mirror_gte_pgxp). Shadows are maintained only while this is set. */
extern int pgxp_track;

/* T1: record a GTE push into the SXY register shadow (RTPS path: the FIFO
 * shift is mirrored here; the caller passes the NEW packed SXY2). */
void pgxp_on_rtps(unsigned packed, float x, float y, float invw, int valid);
/* T1: record one RTPT vertex into its SXY register shadow (no FIFO shift). */
void pgxp_on_rtpt(int v, unsigned packed, float x, float y, float invw, int valid);
/* T1: propagate a shadow across a register move. from_ram=1 (LWC2) means the
 * value came from RAM and is always unproven. */
void pgxp_on_mfc2(int rd, int rt, unsigned val);
void pgxp_on_mtc2(int rt, int rd, unsigned data, int from_ram);
/* T1: Lightrec cop2_notify entry point: decodes the raw opcode and dispatches
 * to pgxp_on_mfc2 / pgxp_on_mtc2. */
void pgxp_on_notify(unsigned opcode, unsigned data);
/* T2: shadows for GP0 command words written directly by the CPU.
 * pgxp_cmd_shadow_at() maps a command-list pointer to its parallel shadow
 * array: the CPU-write buffer maps to pgxp_cmd_shadow[], a RAM list maps
 * into the RAM shadow array (T3, when the RAM base is known), anything else
 * maps to NULL (safe fallback to the integer vertex). */
/* A shadow: the precise float position (x, y) + depth (invw = 1/z) for the
 * integer word i. seq = projection sequence number (one per GTE projection,
 * in projection order; assigned in pgxp_on_rtps/pgxp_on_rtpt, carried along
 * with every copy). frame = the game frame (pgxp_frame) the vertex was
 * projected in; a shadow older than PGXP_AGE game frames is dead. */
typedef struct { int valid; unsigned i; float x, y, invw; unsigned seq; unsigned frame; } pgxp_shadow_t;
/* 1 when a shadow is live: valid and projected within PGXP_AGE game frames.
 * Pure predicate (no counting); the caller counts a valid-but-stale
 * rejection as aged_out where it matters (do_poly draw pass, edge lookup). */
int pgxp_shadow_live(const pgxp_shadow_t *sh);
void pgxp_cmd_write(int idx, unsigned word);
void pgxp_cmd_shift(int left, int total);
void pgxp_cmd_fill_none(int start, int count);
const pgxp_shadow_t *pgxp_cmd_shadow_at(const uint32_t *list, const uint32_t *base);

/* T3: RAM + scratchpad shadows (DuckStation's default memory mode, NOT CPU
 * mode: no ALU tracking, no depth buffer). One shadow per RAM word (2MB =
 * 512K words, ~10MB BSS) plus one per scratchpad word (1KB = 256 words).
 * The host RAM base is set once by the core at memory init (psxMemInitMap);
 * until then RAM lists resolve to NULL (safe fallback). Shadows live at
 * most PGXP_AGE game frames (see above), like the register shadows. */
void pgxp_set_ram_base(const uint32_t *base);
/* Map a PS1 address to its RAM/scratchpad shadow slot, or NULL when the
 * address is not RAM or scratchpad. Segments are stripped like lightrec's
 * kunseg(), so all aliases of one physical word share one slot (RAM mirrors
 * fold onto the same 2MB window). A bare "& 0x1fffff" mask would falsely
 * map I/O (e.g. 0x1f801810 -> 0x1810) into RAM, so the range check comes
 * first. */
const pgxp_shadow_t *pgxp_ram_shadow_at(unsigned psx_addr);
/* T3: RAM shadow flow for full-word transfers. addr = PS1 effective address,
 * word = the game value transferred, rt = GPR (is_cop2=0) or GTE register
 * (is_cop2=1, SXY regs 12/13/14 only; 15 maps to 14 like MFC2). Stores copy
 * the source shadow as-is when it claims exactly this word, else clear the
 * slot; loads carry the slot into the destination register only when the
 * slot is valid and claims exactly this word, else invalidate it. Every
 * hook returns immediately when pgxp_track is off. */
void pgxp_on_sw_ram(unsigned addr, unsigned word, int rt, int is_cop2);
void pgxp_on_lw_ram(unsigned addr, unsigned word, int rt, int is_cop2);
/* Sub-word + unaligned (DuckStation memory-mode scope, T3 extension).
 * Stores (SB/SH/SWL/SWR): merged = the post-write RAM/scratchpad word
 * containing addr (read back by the caller after the write, RAM/scratchpad
 * only — the caller passes 0 for any other address and the slot lookup
 * below then no-ops). The exact-match rule: if the source GPR's shadow
 * claims exactly merged, the slot takes that shadow; else the slot is
 * invalidated. A partial write that completes a known shadowed value
 * (e.g. SH low + SH high from one shadowed SXY word) restores coverage;
 * any other partial write safely invalidates — never a wrong shadow.
 * Each SWL/SWR op writes exactly one aligned word in both the interpreter
 * (doSWL/doSWR) and Lightrec (lightrec_swl/swr), so the per-word rule has
 * exactly one word to judge per op; a two-word span only arises across an
 * SWL+SWR pair, each handled by its own call. Loads: LB/LH invalidate the
 * destination GPR shadow (a byte/halfword cannot carry a full shadow);
 * LWL/LWR at a word-aligned address behave exactly as LW, unaligned they
 * invalidate (full-word-via-pair propagation is a follow-up, not here).
 * Every hook returns immediately when pgxp_track is off (no counter). */
void pgxp_on_sb_ram(unsigned addr, unsigned merged, int rt);
void pgxp_on_sh_ram(unsigned addr, unsigned merged, int rt);
void pgxp_on_swl_ram(unsigned addr, unsigned merged, int rt);
void pgxp_on_swr_ram(unsigned addr, unsigned merged, int rt);
void pgxp_on_lb_ram(int rt);
void pgxp_on_lh_ram(int rt);
void pgxp_on_lwl_ram(unsigned addr, unsigned word, int rt);
void pgxp_on_lwr_ram(unsigned addr, unsigned word, int rt);
/* T3: attach RAM shadows to command words copied from a DMA list into the
 * command buffer. word = the game value of the copied word, psx_addr = the
 * PS1 address that word came from. Attaches only when the slot is valid
 * and claims exactly this word, else clears the entry (same fallback as
 * pgxp_cmd_fill_none, which this replaces at DMA fill sites). */
void pgxp_cmd_dma_word(int idx, unsigned word, unsigned psx_addr);

/* Edge-consistent substitution map (option A). Keyed by integer screen
 * position (pre-draw-offset, same keys as pgxp_resolve), value = up to TWO
 * precise candidates (float position + invw + projection seq + frame) from
 * validated shadows. One candidate is the first-writer rule: a re-record of
 * the same point (depth within 3% of an existing candidate) keeps the first
 * writer, so shared edges resolve to one value and do not flicker. A second
 * candidate (a different depth at the same pixel — e.g. a fighter's hand in
 * front of a wall) is stored alongside, and the per-triangle choice between
 * them is made by pgxp_pick_candidates (projection-seq proximity, then
 * depth-fit scoring over the still-ambiguous corners). A third distinct
 * candidate replaces the older-seq one (PGXP-lite's rule). A hash collision
 * with a different position is NOT recorded (the later position simply gets
 * no borrow — safe fallback, never a wrong float). Candidates age out after
 * PGXP_AGE game frames (pruned lazily at lookup/record); a lookup that
 * prunes a stale candidate counts it as aged_out. Record/lookup return
 * immediately (miss) when pgxp_track is off. */
typedef struct { float x, y, invw; unsigned seq; unsigned frame; } pgxp_edge_cand_t;
void pgxp_edge_record(int sx, int sy, const pgxp_shadow_t *sh);
/* Return the live candidates (0-2) for one integer position, in stored
 * order; stale candidates are pruned (and counted as aged_out) first. */
int pgxp_edge_lookup_n(int sx, int sy, pgxp_edge_cand_t *out);
/* Weld (PGXP-lite idea, adapted to the fork's draw-time resolver): snap the
 * resolved position (fx, fy, fi) at integer (sx, sy) to a neighbour's exact
 * place when the two are the same joint. own = 1 when the position came
 * from an own shadow (the same-pixel snap applies: the map entry for this
 * pixel holds the first copy's place); own = 0 for a borrowed position
 * (neighbour snap only, so an ambiguity choice is never overridden).
 * frame = the source shadow's projection frame; only same-frame candidates
 * weld. A neighbour snap also writes the welded place back into this
 * vertex's own map entry (when the entry still holds the pre-weld values),
 * so the joint's other copy — drawn later in the batch — welds to the same
 * place instead of the two copies swapping places. Returns 1 and fills
 * (ox, oy, oinvw, oseq) when welded, else 0. */
int pgxp_weld_try(int sx, int sy, int own, float fx, float fy, float fi, unsigned frame, float *ox, float *oy, float *oinvw, unsigned *oseq);
/* Per-triangle ambiguity resolution (PGXP-lite stage A + B, implemented
 * live): corners i = 0..n-1 (n = 3, one triangle) with nc[i] candidates
 * (0-2) in cand[i][2]; pick[i] = the chosen candidate index (0 when nc <= 1).
 * Stage A (seq proximity): corners with two candidates pick the one whose
 * projection seq is clearly closer to the single-candidate corners' seqs
 * (his rule: hi/4 > lo + spread + 16, where hi/lo are the seq distances).
 * Stage B (depth-fit scoring): the still-ambiguous corners are tried in
 * every combination (at most 8 for a triangle); a combination whose depths
 * fail pgxp_depths_fit scores +1e7, plus the projection-seq spread (capped
 * at 1e6), plus (hi/lo - 1) * 0.5 for the depth ratio; the best combination
 * wins, ties keep the first. A corner with no candidate stays integer. */
void pgxp_pick_candidates(int n, const int *sx, const int *sy, const int *nc, const pgxp_edge_cand_t cand[][2], int *pick);
/* Texture-precision logging: record one substituted vertex's integer
 * screen position into this frame's substituted-position set (for the
 * flip_new/flip_lost counters). Called from do_poly for every substituted
 * vertex (own shadow or borrowed). Returns immediately when off. */
void pgxp_note_substituted(int sx, int sy);

/* PGXP logging: per-window counters for hardware diagnosis (no more guessing).
 * All counters live in pgxp.c and are incremented from the core (lookup,
 * command shadows, NCLIP) and the RSX layer (substitution, texture resolve,
 * degenerate skips). The frontend prints one summary line per 60 presented
 * frames while the debug log is on, then resets them, so each line covers
 * one window. Every line is tagged with the master state it was collected
 * under ([ON]/[OFF]) and its frame count, and a marker line is printed
 * whenever the master toggles (flushing the pending partial window first),
 * so ON/OFF can be compared inside one run -- the same scene never recurs
 * across sessions. While the debug log is off, the log state resets, so
 * re-enabling starts clean. Single-threaded (hooks run on the CPU thread):
 * no locking.
 * Every field answers one diagnostic question:
 *  cmd_unique/ambig/none: reverse-lookup health (how often a GP0 word finds
 *    exactly one live register shadow, several, or none).
 *  sub_verts/sub_applied: how many drawn vertices were moved (own shadow
 *    + borrowed); sub_borrowed: how many of those came from the edge map
 *    (a vertex without its own shadow borrowing a position recorded by
 *    another vertex at the same integer position in the same batch).
 *  sub_max_dx/dy, sub_big: substitution size; legit shadows differ from the
 *    integer by < 1px (same GTE value, truncated vs exact), so sub_big
 *    (> 1.5px) is the smoking gun for a misaligned/wrong shadow.
 *  tex_persp/affine/mixed: texture precision per triangle; mixed = a triangle
 *    whose positions were substituted but whose texture stayed affine (cache
 *    miss) -- steady vertices + sliding texture = the "weird wobble".
 *  lk_hit/miss: slice-1 cache health (texture + culling lookups).
 *  nclip_total/flip/restore: culling corrections applied, sign flips vs the
 *    integer winding (flip = a triangle the precise winding culls but the
 *    integer would draw, or vice versa), restores (integer det 0, precise != 0).
 *  degen_skip: triangles sample_shift dropped as degenerate (post-override
 *    positions); compare master ON vs OFF to see if substitution kills any.
 *  toobig_drop: triangles do_poly dropped as oversized (pre-override).
 *  flip_new/flip_lost: substitution-state flicker per screen position
 *    across frames (shaking signal): new = positions substituted this
 *    frame but not last, lost = last frame but not this. Counts conflate
 *    scene motion with flicker when the camera moves (different objects
 *    occupy the same screen position in different frames), so the clean
 *    flicker baseline is a STILL scene (parked/paused view) or a same-
 *    scene build comparison — the counter just counts flips.
 *  invw_conflict/invw_conflict_max: same screen position claimed with
 *    different invw within one frame (edge-warp signal): two triangles
 *    sharing a screen position but using different w will warp/tear the
 *    texture at that edge. Counted at most once per position per frame
 *    (threshold |invw diff| > 0.05, above rounding). Motion-robust (one
 *    frozen frame cannot explain two different invw for one position).
 *    Note: a legitimate overlap (e.g. a car over ground at the same
 *    screen position, different depths) also counts — that IS the warp
 *    signal (two depths sharing a position with different w), not a
 *    false alarm.
 *  set_repl: sub-word store SETs (SB/SH/SWL/SWR only) that overwrote a
 *    different existing shadow in the destination slot (proxy for false-
 *    positive churn / mixed sources). A replacement is not proof the new
 *    shadow is wrong, only that sources churned; SW/SWC2 SETs are never
 *    counted (always sound: destination word equals source value).
 *  guard_skip: vertices whose substitution was skipped by the use-time
 *    guard in do_poly (resolved precise position > 1.5px from the integer
 *    on either axis — teleported vertices, e.g. from a clamped GTE float
 *    whose integer still validates exactly). A skipped vertex keeps its
 *    integer position + resolve texture (stock behaviour); it does NOT
 *    count as applied, borrowed, noted, or big. Log reading: big=0 +
 *    guard_skip>0 = the guard worked (blocked big vertices); big=0 +
 *    guard_skip=0 = no big vertices occurred in the scene; big>0 = the
 *    guard missed some (bug or an unguarded substitution site).
 *  welds: vertices snapped to a neighbour's exact place by pgxp_weld_try
 *    (same-pixel snap for own shadows, neighbour-pixel snap for own and
 *    borrowed). A weld moves a vertex at most 0.25px toward a same-depth
 *    neighbour, so a high count with no visual change is still fine.
 *  amb_resolved: ambiguous corners (two candidates at one integer position)
 *    that pgxp_pick_candidates decided by seq proximity / depth-fit scoring.
 *  aged_out: live-looking shadows/entries rejected for age at a use site
 *    (a stale own shadow in do_poly, a stale edge-map candidate pruned by a
 *    lookup). How often the 3-game-frame window expires data before it is
 *    used; a high count means the window is too short for that scene. */
typedef struct {
    unsigned long cmd_unique, cmd_ambig, cmd_none;
    unsigned long sub_verts, sub_applied, sub_borrowed;
    float sub_max_dx, sub_max_dy;
    unsigned long sub_big;
    unsigned long tex_persp, tex_affine, tex_mixed;
    unsigned long lk_hit, lk_miss;
    unsigned long nclip_total, nclip_flip, nclip_restore;
    unsigned long degen_skip, toobig_drop;
    unsigned long st_sb, st_sh, st_swl, st_swr;
    unsigned long ld_lb, ld_lh, ld_lwl, ld_lwr;
    unsigned long flip_new, flip_lost;
    unsigned long invw_conflict;
    float invw_conflict_max;
    unsigned long set_repl;
    unsigned long guard_skip;
    unsigned long welds, amb_resolved, aged_out;
} pgxp_stats_t;
extern pgxp_stats_t pgxp_stats;
void pgxp_stats_reset(void);

#ifdef __cplusplus
}
#endif
#endif
