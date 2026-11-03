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

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif
#endif
