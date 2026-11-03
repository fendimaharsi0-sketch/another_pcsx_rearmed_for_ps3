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
 * array (NULL when the list is not the CPU-write buffer, e.g. DMA lists
 * straight from RAM: those words have no shadow and fall back). */
typedef struct { int valid; unsigned i; float x, y, invw; } pgxp_shadow_t;
void pgxp_cmd_write(int idx, unsigned word);
void pgxp_cmd_shift(int left, int total);
void pgxp_cmd_fill_none(int start, int count);
const pgxp_shadow_t *pgxp_cmd_shadow_at(const uint32_t *list, const uint32_t *base);

#ifdef __cplusplus
}
#endif
#endif
