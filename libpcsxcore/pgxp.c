/* PGXP slice 1: float vertex tracking for texture + culling correction.
 * See pgxp.h for the design. Original code for this fork; not copied from
 * DuckStation or any other PGXP implementation. */

#include <string.h>
#include "pgxp.h"

#define PGXP_CACHE_BITS 12
#define PGXP_CACHE_SIZE (1 << PGXP_CACHE_BITS)
#define PGXP_CACHE_MASK (PGXP_CACHE_SIZE - 1)

typedef struct { short sx, sy; unsigned gen; float x, y, invw; } pgxp_entry_t;

static pgxp_entry_t pgxp_cache[PGXP_CACHE_SIZE];
static unsigned pgxp_gen = 1;

int gte_pgxp = 0;

static inline unsigned pgxp_hash(int sx, int sy)
{
	/* unsigned arithmetic throughout: the signed product would overflow 32
	 * bits for large |sx| (undefined behaviour); the wrapped bit pattern is
	 * identical, so this keeps the old hash's distribution with defined behaviour. */
	return ((unsigned)sx * 73856093u ^ (unsigned)sy * 19349663u) & PGXP_CACHE_MASK;
}

void pgxp_next_frame(void)
{
	if (++pgxp_gen == 0) {
		memset(pgxp_cache, 0, sizeof pgxp_cache);
		pgxp_gen = 1;
	}
}

void pgxp_push(int sx, int sy, float x, float y, int z)
{
	pgxp_entry_t *e;

	if (z <= 0) return;
	e = &pgxp_cache[pgxp_hash(sx, sy)];
	if (e->gen != pgxp_gen) {
		e->sx = (short)sx; e->sy = (short)sy;
		e->x = x; e->y = y; e->invw = 1.0f / (float)z;
		e->gen = pgxp_gen;
	}
	/* else: first writer in this frame wins. A later push for the same pixel
	 * (or a hash collision with another pixel) is ignored, so shared edges
	 * resolve to one value and do not flicker frame to frame. */
}

int pgxp_lookup(int sx, int sy, float *x, float *y, float *invw)
{
	pgxp_entry_t *e = &pgxp_cache[pgxp_hash(sx, sy)];

	if (e->gen != pgxp_gen) return 0;
	if (e->sx != sx || e->sy != sy) return 0;
	/* Validation (DuckStation's Validate() rule, in this architecture's terms):
	 * the float must claim to describe exactly this integer vertex, in this
	 * frame, and lie within ~1px of it. A saturated or stale entry fails here
	 * and the caller falls back to the integer vertex. */
	if (e->x < (float)sx - 0.02f || e->x > (float)sx + 1.02f) return 0;
	if (e->y < (float)sy - 0.02f || e->y > (float)sy + 1.02f) return 0;
	*x = e->x; *y = e->y; *invw = e->invw;
	return 1;
}
