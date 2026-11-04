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
 * same frame, exact integer match, float within ~1px of the integer.
 * Anything else falls back to the integer vertex unchanged, so a cache miss
 * can never corrupt geometry. Vertices the CPU transforms itself never hit
 * the cache (same documented limitation as the pre-removal design).
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
 * are wiped every frame by pgxp_next_frame, like the cache. RAM values
 * (LWC2/SWC2) carry no shadow until T3: the destination is always unproven.
 * Registering cop2_notify makes the Lightrec emitter fall back to C calls
 * for MFC2/MTC2 even when tracking is off (block-compile-time choice):
 * unmeasured performance cost, revisit if FPS drops. */

/* gte_pgxp bitmask, mirrored from the frontend settings (ps3/src/config.c):
 * bit 0 = texture correction active (master on + texture on)
 * bit 1 = culling correction active (master on + culling on)
 * 0 = tracking off entirely (master off, or both sub-options off) */
#define PGXP_TEX  1
#define PGXP_CULL 2
extern int gte_pgxp;

/* Record a GTE-projected vertex: (sx, sy) is the integer screen position the
 * game got, (x, y) the pre-truncation float position, z the projected depth
 * (GTE SZ, > 0). Vertices at/behind the camera (z <= 0) are not recorded. */
void pgxp_push(int sx, int sy, float x, float y, int z);

/* Look a vertex up by the integer screen position the game put in a GP0
 * command. Returns 1 and fills (x, y, invw) on a validated hit, 0 otherwise;
 * the caller must then use the integer vertex unchanged. */
int pgxp_lookup(int sx, int sy, float *x, float *y, float *invw);

/* Start a new frame: entries from older frames stop matching. Called once per
 * presented frame from the frontend (hwr_present). */
void pgxp_next_frame(void);

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
typedef struct { int valid; unsigned i; float x, y, invw; } pgxp_shadow_t;
void pgxp_cmd_write(int idx, unsigned word);
void pgxp_cmd_shift(int left, int total);
void pgxp_cmd_fill_none(int start, int count);
const pgxp_shadow_t *pgxp_cmd_shadow_at(const uint32_t *list, const uint32_t *base);

/* T3: RAM + scratchpad shadows (DuckStation's default memory mode, NOT CPU
 * mode: no ALU tracking, no depth buffer). One shadow per RAM word (2MB =
 * 512K words, ~10MB BSS) plus one per scratchpad word (1KB = 256 words).
 * The host RAM base is set once by the core at memory init (psxMemInitMap);
 * until then RAM lists resolve to NULL (safe fallback). Shadows are wiped
 * every frame by pgxp_next_frame, like the register shadows. */
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
/* T3: attach RAM shadows to command words copied from a DMA list into the
 * command buffer. word = the game value of the copied word, psx_addr = the
 * PS1 address that word came from. Attaches only when the slot is valid
 * and claims exactly this word, else clears the entry (same fallback as
 * pgxp_cmd_fill_none, which this replaces at DMA fill sites). */
void pgxp_cmd_dma_word(int idx, unsigned word, unsigned psx_addr);

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
 *  sub_verts/sub_applied: how many drawn vertices T2 actually moved.
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
 *  toobig_drop: triangles do_poly dropped as oversized (pre-override). */
typedef struct {
    unsigned long cmd_unique, cmd_ambig, cmd_none;
    unsigned long sub_verts, sub_applied;
    float sub_max_dx, sub_max_dy;
    unsigned long sub_big;
    unsigned long tex_persp, tex_affine, tex_mixed;
    unsigned long lk_hit, lk_miss;
    unsigned long nclip_total, nclip_flip, nclip_restore;
    unsigned long degen_skip, toobig_drop;
} pgxp_stats_t;
extern pgxp_stats_t pgxp_stats;
void pgxp_stats_reset(void);

#ifdef __cplusplus
}
#endif
#endif
