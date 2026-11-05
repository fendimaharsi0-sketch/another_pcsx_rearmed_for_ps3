/* PGXP slice 1: float vertex tracking for texture + culling correction.
 * See pgxp.h for the design. Original code for this fork; not copied from
 * DuckStation or any other PGXP implementation. */

#include <string.h>
#include "pgxp.h"

#define PGXP_CACHE_BITS 12
#define PGXP_CACHE_SIZE (1 << PGXP_CACHE_BITS)
#define PGXP_CACHE_MASK (PGXP_CACHE_SIZE - 1)

typedef struct { short sx, sy; int valid; unsigned frame; float x, y, invw; } pgxp_entry_t;

static pgxp_entry_t pgxp_cache[PGXP_CACHE_SIZE];

/* Game-frame clock (PGXP-lite hybrid aging): pgxp_frame advances on GP1(05h)
 * display-area flips (pgxp_on_flip) and, for games that never flip, on every
 * 8 frontend vsyncs (pgxp_next_frame). pgxp_disp is the last display address
 * seen; pgxp_vsyncs counts presents since the last flip. The clock is never
 * reset (not by a toggle, not by a title change): age is pgxp_frame minus
 * the stamp, in unsigned arithmetic, so it stays correct across the 2^32
 * wrap. */
unsigned pgxp_frame = 0;
static unsigned pgxp_disp = ~0u;
static unsigned pgxp_vsyncs = 0;
/* Projection sequence number: one per GTE projection (RTPS/RTPT), in
 * projection order, assigned in pgxp_on_rtps/pgxp_on_rtpt. It only orders
 * projections within a few frames (stage-A proximity, second-candidate
 * replacement), so the 2^32 wrap is harmless — all comparisons use
 * distances, never absolute order. */
static unsigned pgxp_seq = 0;

int gte_pgxp = 0;
int pgxp_track = 0;

pgxp_stats_t pgxp_stats;
void pgxp_stats_reset(void) { memset(&pgxp_stats, 0, sizeof pgxp_stats); }

/* T1: per-register shadows. i is the exact integer (packed SXY word) the
 * shadow was recorded for; a shadow is only ever used when the register's
 * current integer still equals i, so a stale shadow fails safe. */
static pgxp_shadow_t pgxp_gpr[32];
static pgxp_shadow_t pgxp_gte[32]; /* data regs; only 12/13/14 (SXY) are used */

/* T3: shadows for RAM + scratchpad words. 2MB RAM = 512K words (~10MB BSS);
 * 1KB scratchpad = 256 words. The host RAM base is set by the core at
 * memory init; pgxp_cmd_shadow_at uses it to resolve RAM command lists.
 * PGXP_RAM_GUARD: polygon tails (a command is at most ~13 words) may read
 * a few words past a RAM list that starts at the very end of RAM; the
 * guard words stay zero (never written), so such
 * reads fall back safely instead of reading past the array. */
#define PGXP_RAM_WORDS (0x200000 / 4)
#define PGXP_SCRATCH_WORDS (0x400 / 4)
#define PGXP_RAM_GUARD 64
static pgxp_shadow_t pgxp_ram_shadow[PGXP_RAM_WORDS + PGXP_RAM_GUARD];
static pgxp_shadow_t pgxp_scratch_shadow[PGXP_SCRATCH_WORDS];
static const uint32_t *pgxp_ram_base; /* host RAM base, NULL until set */

void pgxp_set_ram_base(const uint32_t *base) { pgxp_ram_base = base; }

/* Map a PS1 address to its RAM/scratchpad shadow slot, or NULL. Segment
 * stripped like lightrec's kunseg(): KSEG0 (0x8...) and KSEG1 (0xa...) fold
 * onto KUSEG, so all aliases of one physical word share one slot. */
static pgxp_shadow_t *pgxp_ram_slot(unsigned addr)
{
	unsigned phys;
	if (addr >= 0xa0000000u) phys = addr - 0xa0000000u;
	else phys = addr & ~0x80000000u;
	if (phys < 0x00800000u) return &pgxp_ram_shadow[(phys & 0x1fffffu) / 4];
	if (phys >= 0x1f800000u && phys < 0x1f800400u)
		return &pgxp_scratch_shadow[(phys - 0x1f800000u) / 4];
	return NULL;
}

const pgxp_shadow_t *pgxp_ram_shadow_at(unsigned psx_addr)
{
	return pgxp_ram_slot(psx_addr);
}

static inline unsigned pgxp_hash(int sx, int sy)
{
	/* unsigned arithmetic throughout: the signed product would overflow 32
	 * bits for large |sx| (undefined behaviour); the wrapped bit pattern is
	 * identical, so this keeps the old hash's distribution with defined behaviour. */
	return ((unsigned)sx * 73856093u ^ (unsigned)sy * 19349663u) & PGXP_CACHE_MASK;
}

/* Edge-consistent substitution map (option A). Keyed by integer screen
 * position (pre-draw-offset, same keys as pgxp_resolve), value = up to two
 * precise candidates (see pgxp_edge_cand_t). Single-slot hash like the
 * slice-1 cache, but a much bigger table (64K slots) so collisions are
 * rare; a 1024x512 direct array would be 10MB and is deliberately NOT used.
 * Candidates age out after PGXP_AGE game frames (pruned lazily at
 * lookup/record). Storage is declared here (before pgxp_next_frame); the
 * record/lookup functions are defined below, after pgxp_lookup. */
#define PGXP_EDGE_BITS 16
#define PGXP_EDGE_SIZE (1 << PGXP_EDGE_BITS)
#define PGXP_EDGE_MASK (PGXP_EDGE_SIZE - 1)
typedef struct { int valid; int sx, sy; int n; pgxp_edge_cand_t c[2]; int conflict_counted; } pgxp_edge_t;
static pgxp_edge_t pgxp_edge_map[PGXP_EDGE_SIZE];

/* Texture-precision logging: per-frame substituted-position sets (flip
 * detection). cur accumulates this frame's substituted integer positions
 * (via pgxp_note_substituted, called from do_poly for every substituted
 * vertex, own or borrowed); prev holds last frame's set. At each frame
 * boundary pgxp_next_frame counts new (in cur, not in prev) and lost (in
 * prev, not in cur), then swaps the tables and clears the new cur.
 * Single-slot hash like the edge map (64K slots); a collision between two
 * different positions in adjacent frames counts as one false new + one
 * false lost (rare at 64K slots). Separate arrays with their own lifecycle:
 * NOT touched by pgxp_next_frame's shadow handling (there is no wipe any
 * more); cur/prev are managed by the flip code itself. */
#define PGXP_FLIP_BITS 16
#define PGXP_FLIP_SIZE (1 << PGXP_FLIP_BITS)
#define PGXP_FLIP_MASK (PGXP_FLIP_SIZE - 1)
typedef struct { int valid; int sx, sy; } pgxp_flip_t;
static pgxp_flip_t pgxp_flip_buf[2][PGXP_FLIP_SIZE];
static pgxp_flip_t *pgxp_flip_cur = pgxp_flip_buf[0];
static pgxp_flip_t *pgxp_flip_prev = pgxp_flip_buf[1];
static int pgxp_prev_track = 0;
/* Sparse flip iteration (perf fix): the flip computation used to scan all
 * 64K slots per ON frame (128K random-access hash lookups into a 768KB
 * table — the PGXP-ON perf hit on the PPE). Each buffer now also keeps the
 * list of slot indices that became valid this frame, appended exactly once
 * per slot (on the invalid->valid transition in pgxp_note_substituted).
 * The flip loop iterates only those lists, so its cost is O(used) instead
 * of O(64K). Counts are identical to the full scan: invalid slots
 * contribute nothing in the full loop (it checks valid first), and the
 * used list contains exactly the valid slots, once each (a colliding
 * second position never appends — the slot already holds the first one,
 * exactly as the full loop would see it). The lists swap with the tables
 * (below), and the new cur's count resets to 0 when its table is memset,
 * so stale indices can never be iterated. Bound: a slot appends at most
 * once between memsets, so a count never exceeds PGXP_FLIP_SIZE. */
static int pgxp_flip_used[2][PGXP_FLIP_SIZE];
static int *pgxp_flip_used_cur = pgxp_flip_used[0];
static int *pgxp_flip_used_prev = pgxp_flip_used[1];
static int pgxp_flip_used_cur_n = 0, pgxp_flip_used_prev_n = 0;

static inline unsigned pgxp_flip_hash(int sx, int sy)
{
	return ((unsigned)sx * 73856093u ^ (unsigned)sy * 19349663u) & PGXP_FLIP_MASK;
}

static int pgxp_flip_in(const pgxp_flip_t *table, int sx, int sy)
{
	const pgxp_flip_t *e = &table[pgxp_flip_hash(sx, sy)];
	return e->valid && e->sx == sx && e->sy == sy;
}

void pgxp_note_substituted(int sx, int sy)
{
	pgxp_flip_t *e;
	unsigned h;
	if (!pgxp_track) return;
	h = pgxp_flip_hash(sx, sy);
	e = &pgxp_flip_cur[h];
	if (!e->valid) {
		e->valid = 1; e->sx = sx; e->sy = sy;
		pgxp_flip_used_cur[pgxp_flip_used_cur_n++] = (int)h;
	}
	/* else: same position already noted (idempotent set), or a different
	 * position colliding — left alone, like the edge map. Neither case
	 * appends (the slot is already in the used list exactly once). */
}

void pgxp_next_frame(void)
{
	/* Vsync fallback for the game-frame clock (lite's pgxp_lite_vsync): one
	 * presented frame is one refresh; 8 refreshes without a display flip
	 * count as one game frame. Runs BEFORE the track gate, so the clock
	 * advances (and stale shadows age out) while the master is off too.
	 * This function is one presented frame: it must only be called from the
	 * present path (hwr_present), never from a toggle or menu path. */
	if (++pgxp_vsyncs >= 8) {
		pgxp_vsyncs = 0;
		pgxp_frame++;
	}
	/* Gate: when the master is off, nothing reads any PGXP array (every hook
	 * is gated on pgxp_track, and hwr_pgxp_tex is 0 so do_poly skips the
	 * cache lookup), so there is nothing to do here. Shadows are NOT wiped
	 * (they age out at use time, see PGXP_AGE). Flip tables have their own
	 * lifecycle (see below): OFF frames clear them and reset the transition
	 * flag, so the first ON frame after OFF has no stale baseline to compare
	 * against. */
	if (!pgxp_track) {
		memset(pgxp_flip_cur, 0, sizeof pgxp_flip_buf[0]);
		memset(pgxp_flip_prev, 0, sizeof pgxp_flip_buf[0]);
		pgxp_flip_used_cur_n = 0;
		pgxp_flip_used_prev_n = 0;
		pgxp_prev_track = 0;
		return;
	}
	/* Texture-precision flips: compare this frame's substituted-position
	 * set (cur, accumulated via pgxp_note_substituted during the frame)
	 * against last frame's set (prev). On the first ON frame after OFF
	 * (prev_track == 0) there is no baseline: skip counting this frame
	 * (a toggle must not pollute the flicker count with a one-frame blip
	 * of "new" substitutions), but still swap so the second ON frame
	 * compares against the first. Computed BEFORE the table swap below (the
	 * flip tables are separate arrays, managed only by this flip code, but
	 * the order keeps the frame-boundary logic in one place). */
	if (!pgxp_prev_track) {
		memset(pgxp_flip_prev, 0, sizeof pgxp_flip_buf[0]);
		pgxp_flip_used_prev_n = 0;
	} else {
		/* Sparse: same predicate as the old full scan, iterated over
		 * used slots only. Used entries are valid by construction
		 * (appended only on invalid->valid); the valid check stays as
		 * belt and braces (free on a sparse loop, and it keeps this
		 * loop obviously equivalent to the full scan). */
		int u;
		for (u = 0; u < pgxp_flip_used_cur_n; u++) {
			const pgxp_flip_t *e = &pgxp_flip_cur[pgxp_flip_used_cur[u]];
			if (e->valid && !pgxp_flip_in(pgxp_flip_prev, e->sx, e->sy))
				pgxp_stats.flip_new++;
		}
		for (u = 0; u < pgxp_flip_used_prev_n; u++) {
			const pgxp_flip_t *e = &pgxp_flip_prev[pgxp_flip_used_prev[u]];
			if (e->valid && !pgxp_flip_in(pgxp_flip_cur, e->sx, e->sy))
				pgxp_stats.flip_lost++;
		}
	}
	{
		pgxp_flip_t *tmp = pgxp_flip_prev;
		pgxp_flip_prev = pgxp_flip_cur;
		pgxp_flip_cur = tmp;
		int *utmp = pgxp_flip_used_prev;
		pgxp_flip_used_prev = pgxp_flip_used_cur;
		pgxp_flip_used_cur = utmp;
		int ntmp = pgxp_flip_used_prev_n;
		pgxp_flip_used_prev_n = pgxp_flip_used_cur_n;
		pgxp_flip_used_cur_n = ntmp;
	}
	memset(pgxp_flip_cur, 0, sizeof pgxp_flip_buf[0]);
	pgxp_flip_used_cur_n = 0;
	pgxp_prev_track = 1;
	/* No wipes any more: the register/RAM shadows, the slice-1 cache and
	 * the edge map age out at use time (PGXP_AGE game frames). The 10MB
	 * RAM-shadow memset per presented frame is gone with them. */
}

/* Game-frame clock, flip half (lite's pgxp_lite_flip): the display address
 * is the game's buffer; a change means the game finished a frame. A game
 * that always sets the same address does not advance here (the vsync
 * fallback in pgxp_next_frame covers it). */
void pgxp_on_flip(unsigned disp)
{
	if (disp != pgxp_disp) {
		pgxp_disp = disp;
		pgxp_frame++;
		pgxp_vsyncs = 0;
	}
}

int pgxp_shadow_live(const pgxp_shadow_t *sh)
{
	if (!sh || !sh->valid) return 0;
	return (pgxp_frame - sh->frame) <= PGXP_AGE;
}

void pgxp_push(int sx, int sy, float x, float y, int z)
{
	pgxp_entry_t *e;

	if (z <= 0) return;
	e = &pgxp_cache[pgxp_hash(sx, sy)];
	/* First writer wins while the entry is live (same as before, but the
	 * window is PGXP_AGE game frames instead of one presented frame); a
	 * stale or never-written slot takes the new value. */
	if (!e->valid || (pgxp_frame - e->frame) > PGXP_AGE) {
		e->sx = (short)sx; e->sy = (short)sy;
		e->x = x; e->y = y; e->invw = 1.0f / (float)z;
		e->valid = 1; e->frame = pgxp_frame;
	}
	/* else: first writer in this window wins. A later push for the same pixel
	 * (or a hash collision with another pixel) is ignored, so shared edges
	 * resolve to one value and do not flicker frame to frame. */
}

int pgxp_lookup(int sx, int sy, float *x, float *y, float *invw)
{
	pgxp_entry_t *e = &pgxp_cache[pgxp_hash(sx, sy)];

	if (!e->valid) { pgxp_stats.lk_miss++; return 0; }
	/* Stale entries fail here (and are cleared, so the slot is reusable);
	 * the miss is counted as an ordinary cache miss, not aged_out — the
	 * cache is re-pushed every frame by the game, so a stale entry just
	 * means the vertex is gone, not that the window is too short. */
	if ((pgxp_frame - e->frame) > PGXP_AGE) { e->valid = 0; pgxp_stats.lk_miss++; return 0; }
	if (e->sx != sx || e->sy != sy) { pgxp_stats.lk_miss++; return 0; }
	/* Validation (DuckStation's Validate() rule, in this architecture's terms):
	 * the float must claim to describe exactly this integer vertex, in this
	 * window, and lie within ~1px of it. A saturated or stale entry fails here
	 * and the caller falls back to the integer vertex. */
	if (e->x < (float)sx - 0.02f || e->x > (float)sx + 1.02f) { pgxp_stats.lk_miss++; return 0; }
	if (e->y < (float)sy - 0.02f || e->y > (float)sy + 1.02f) { pgxp_stats.lk_miss++; return 0; }
	*x = e->x; *y = e->y; *invw = e->invw;
	pgxp_stats.lk_hit++;
	return 1;
}

/* Edge map record/lookup (storage declared above, before pgxp_next_frame). */
static inline unsigned pgxp_edge_hash(int sx, int sy)
{
	/* Same multiplicative constants as the slice-1 cache hash; unsigned
	 * arithmetic throughout (signed overflow would be UB). */
	return ((unsigned)sx * 73856093u ^ (unsigned)sy * 19349663u) & PGXP_EDGE_MASK;
}

/* Depth closeness for the "same point" test (lite's weld/dedup rule): two
 * depths are the same point when they are within 3% of each other. invw <= 0
 * means unknown depth (never the same point). */
static int pgxp_same_depth(float invw_a, float invw_b)
{
	float za, zb, d;
	if (invw_a <= 0.f || invw_b <= 0.f) return 0;
	za = 1.f / invw_a; zb = 1.f / invw_b;
	d = za - zb; if (d < 0.f) d = -d;
	return d < 0.03f * zb;
}

/* Drop the entry's stale candidates (older than PGXP_AGE game frames),
 * compacting the live ones to the front; each pruned candidate counts as
 * aged_out. Returns the live count (and clears valid when none are left). */
static int pgxp_edge_prune(pgxp_edge_t *e)
{
	int i, n = 0;
	for (i = 0; i < e->n; i++) {
		if ((pgxp_frame - e->c[i].frame) <= PGXP_AGE) {
			if (n != i) e->c[n] = e->c[i];
			n++;
		} else {
			pgxp_stats.aged_out++;
		}
	}
	e->n = n;
	if (n == 0) e->valid = 0;
	return n;
}

void pgxp_edge_record(int sx, int sy, const pgxp_shadow_t *sh)
{
	pgxp_edge_t *e;
	int i;
	if (!pgxp_track || !sh || !sh->valid) return;
	e = &pgxp_edge_map[pgxp_edge_hash(sx, sy)];
	if (e->valid && (e->sx != sx || e->sy != sy)) {
		/* Hash collision with a different position: not recorded (the later
		 * position simply gets no borrow), UNLESS the occupying entry is
		 * entirely stale — then the slot is dead and reusable. */
		if (pgxp_edge_prune(e) > 0) return;
		/* pruned to empty: store fresh below. */
	}
	if (e->valid) {
		/* Same position: prune first, then decide. */
		if (pgxp_edge_prune(e) > 0) {
			/* Same point (depth within 3% of an existing candidate) keeps
			 * the first writer: the joint's second copy keeps the first
			 * copy's place (lite's store-time dedup). */
			for (i = 0; i < e->n; i++) {
				if (pgxp_same_depth(sh->invw, e->c[i].invw)) return;
			}
			/* Different depth: the edge-warp signal, counted once per
			 * position per frame (threshold |invw diff| > 0.05, as before,
			 * against the first writer). */
			if (!e->conflict_counted) {
				float d = sh->invw - e->c[0].invw;
				if (d < 0) d = -d;
				if (d > 0.05f) {
					pgxp_stats.invw_conflict++;
					if (d > pgxp_stats.invw_conflict_max)
						pgxp_stats.invw_conflict_max = d;
					e->conflict_counted = 1;
				}
			}
			if (e->n < 2) {
				e->c[e->n].x = sh->x; e->c[e->n].y = sh->y; e->c[e->n].invw = sh->invw;
				e->c[e->n].seq = sh->seq; e->c[e->n].frame = sh->frame;
				e->n++;
			} else {
				/* Both slots full: replace the older-seq candidate (lite's
				 * rule). "Older" is the larger distance from the incoming
				 * seq, so the comparison survives the 2^32 seq wrap. */
				unsigned d0 = sh->seq - e->c[0].seq, d1 = sh->seq - e->c[1].seq;
				int old = (d0 >= d1) ? 0 : 1;
				e->c[old].x = sh->x; e->c[old].y = sh->y; e->c[old].invw = sh->invw;
				e->c[old].seq = sh->seq; e->c[old].frame = sh->frame;
			}
			return;
		}
		/* pruned to empty: store fresh below. */
	}
	e->valid = 1; e->sx = sx; e->sy = sy; e->n = 1;
	e->c[0].x = sh->x; e->c[0].y = sh->y; e->c[0].invw = sh->invw;
	e->c[0].seq = sh->seq; e->c[0].frame = sh->frame;
	e->conflict_counted = 0;
}

int pgxp_edge_lookup_n(int sx, int sy, pgxp_edge_cand_t *out)
{
	pgxp_edge_t *e;
	int i;
	if (!pgxp_track) return 0;
	e = &pgxp_edge_map[pgxp_edge_hash(sx, sy)];
	if (!e->valid || e->sx != sx || e->sy != sy) return 0;
	if (pgxp_edge_prune(e) == 0) return 0;
	for (i = 0; i < e->n; i++) out[i] = e->c[i];
	return e->n;
}

/* Weld closeness (lite's rule): the neighbour's exact place is within 0.25px
 * on both axes and its depth within 3% of this vertex's depth. */
static int pgxp_weld_close(float nx, float ny, float ninvw, float fx, float fy, float fi)
{
	float dx, dy;
	if (ninvw <= 0.f || fi <= 0.f) return 0;
	dx = nx - fx; if (dx < 0.f) dx = -dx;
	dy = ny - fy; if (dy < 0.f) dy = -dy;
	if (dx >= 0.25f || dy >= 0.25f) return 0;
	return pgxp_same_depth(ninvw, fi);
}

/* After a neighbour snap, write the welded place back into this vertex's own
 * map entry — but only when the entry still holds the pre-weld values
 * (identity check): the entry may belong to another vertex (first-writer)
 * or a hash collision, and those must never be rewritten. The frame stays:
 * the welded place was projected in the same game frame as the source (the
 * weld only considers same-frame candidates), so the stamp is unchanged. */
static void pgxp_edge_weld_back(int sx, int sy, float ox, float oy, float oinvw, float nx, float ny, float ninvw, unsigned nseq)
{
	pgxp_edge_t *e = &pgxp_edge_map[pgxp_edge_hash(sx, sy)];
	int i;
	if (!e->valid || e->sx != sx || e->sy != sy) return;
	for (i = 0; i < e->n; i++) {
		if (e->c[i].x == ox && e->c[i].y == oy && e->c[i].invw == oinvw) {
			e->c[i].x = nx; e->c[i].y = ny; e->c[i].invw = ninvw;
			e->c[i].seq = nseq;
			return;
		}
	}
}

int pgxp_weld_try(int sx, int sy, int own, float fx, float fy, float fi, unsigned frame, float *ox, float *oy, float *oinvw, unsigned *oseq)
{
	pgxp_edge_cand_t nb[2];
	int i, n;
	if (!pgxp_track || fi <= 0.f) return 0;
	/* (a) Same-pixel snap, own shadows only: the map entry for this pixel
	 * holds the first copy's place (recorded in the pre-pass). Snap to it
	 * when it is a different, near-coincident, same-depth candidate — the
	 * joint's second copy takes the first copy's place (lite's store-time
	 * dedup, applied here at draw time because own shadows bypass the map).
	 * No write-back: the entry already holds the target. */
	if (own) {
		n = pgxp_edge_lookup_n(sx, sy, nb);
		for (i = 0; i < n; i++) {
			if (nb[i].frame != frame) continue;
			if (nb[i].x == fx && nb[i].y == fy && nb[i].invw == fi) continue;
			if (pgxp_weld_close(nb[i].x, nb[i].y, nb[i].invw, fx, fy, fi)) {
				*ox = nb[i].x; *oy = nb[i].y; *oinvw = nb[i].invw; *oseq = nb[i].seq;
				return 1;
			}
		}
	}
	/* (b) Neighbour-pixel snap, own and borrowed: only when the vertex sits
	 * within 0.25px of a pixel edge on some axis (else no neighbour can be
	 * within 0.25px of it). Neighbours are checked in lite's order — +x, +y,
	 * diagonal — and the first qualifying candidate wins. */
	{
		float dx = fx - (float)sx, dy = fy - (float)sy;
		int xs = dx > 0.25f ? 1 : dx < -0.25f ? -1 : 0;
		int ys = dy > 0.25f ? 1 : dy < -0.25f ? -1 : 0;
		static const int off[3][2] = { { 1, 0 }, { 0, 1 }, { 1, 1 } };
		int o;
		if (xs || ys) {
			for (o = 0; o < 3; o++) {
				int nx, ny;
				if (off[o][0] && !xs) continue;
				if (off[o][1] && !ys) continue;
				nx = sx + off[o][0] * xs; ny = sy + off[o][1] * ys;
				n = pgxp_edge_lookup_n(nx, ny, nb);
				for (i = 0; i < n; i++) {
					if (nb[i].frame != frame) continue;
					if (pgxp_weld_close(nb[i].x, nb[i].y, nb[i].invw, fx, fy, fi)) {
						*ox = nb[i].x; *oy = nb[i].y; *oinvw = nb[i].invw; *oseq = nb[i].seq;
						/* Write-back: the joint's other copy reads this
						 * vertex's entry as its neighbour, so it welds to
						 * the same place instead of the two copies swapping
						 * places. Identity-guarded (see above). */
						pgxp_edge_weld_back(sx, sy, fx, fy, fi, *ox, *oy, *oinvw, *oseq);
						return 1;
					}
				}
			}
		}
	}
	return 0;
}

/* PGXP-lite's depth-fit test (pgxp_depth.h, ported verbatim): do the corners'
 * depths fit one surface? zmin > 0, zmax <= 64*zmin; for a quad the fourth
 * corner must lie on the plane through the other three (ratio 0.85-1.15);
 * otherwise (triangle, or a quad with three corners in a line) zmax <= 2*zmin.
 * x, y are the corners' precise screen positions, z their depths (0 = none).
 * Only the fit test is ported — the odd-corner repair (pgxp_depths) belongs
 * to lite's mode-2 texture path, which the fork does not have. */
static int pgxp_depths_fit(const float *x, const float *y, const float *z, int n)
{
	float zmin = z[0], zmax = z[0];
	int i;
	for (i = 1; i < n; i++) { if (z[i] < zmin) zmin = z[i]; if (z[i] > zmax) zmax = z[i]; }
	if (zmin <= 0.f || zmax > 64.f * zmin) return 0;
	if (n == 4) {   /* the fourth corner against the plane through the other three */
		float x1 = x[1] - x[0], y1 = y[1] - y[0], x2 = x[2] - x[0], y2 = y[2] - y[0];
		float det = x1 * y2 - x2 * y1;
		if (det >= 4.f || det <= -4.f) {
			float ia = 1.f / z[0], q1 = 1.f / z[1] - ia, q2 = 1.f / z[2] - ia, id = 1.f / det;
			float qa = (q1 * y2 - q2 * y1) * id, qb = (x1 * q2 - x2 * q1) * id;
			float q = ia + qa * (x[3] - x[0]) + qb * (y[3] - y[0]);
			if (q != 0.f) { float r = q * z[3]; return r > 0.85f && r < 1.15f; }
		}
	}
	return zmax <= 2.f * zmin;   /* a triangle (or a quad with three corners in a line) */
}

/* Projection-seq distance, wrap-safe: seqs compared are always from the same
 * few frames, so their true distance is far below 2^31, and the unsigned
 * difference taken the short way round is that distance. */
static unsigned pgxp_seq_dist(unsigned a, unsigned b)
{
	unsigned d = a - b;
	unsigned r = b - a;
	return d < r ? d : r;
}

void pgxp_pick_candidates(int n, const int *sx, const int *sy, const int *nc, const pgxp_edge_cand_t cand[][2], int *pick)
{
	int i, amb = 0, k = 0;
	unsigned ref = 0, spread = 0;
	int decided[4];
	if (n <= 0 || n > 4) return;
	for (i = 0; i < n; i++) {
		pick[i] = 0;
		decided[i] = 1;
		if (nc[i] > 1) { decided[i] = 0; amb |= 1 << i; }
		else if (nc[i] == 1) k++;
	}
	if (!amb) return;
	/* Stage A: seq proximity against the single-candidate corners (his
	 * rule). ref = the last single-candidate corner's seq; spread = the
	 * largest seq distance among the single-candidate corners. A corner
	 * decides when one candidate is clearly closer: hi/4 > lo + spread + 16. */
	if (k > 0) {
		for (i = n - 1; i >= 0; i--) {
			if (nc[i] == 1) { ref = cand[i][0].seq; break; }
		}
		for (i = 0; i < n; i++) {
			int j;
			if (nc[i] != 1) continue;
			for (j = i + 1; j < n; j++) {
				unsigned d;
				if (nc[j] != 1) continue;
				d = pgxp_seq_dist(cand[i][0].seq, cand[j][0].seq);
				if (d > spread) spread = d;
			}
		}
		for (i = 0; i < n; i++) {
			unsigned d0, d1, lo, hi;
			if (decided[i]) continue;
			d0 = pgxp_seq_dist(cand[i][0].seq, ref);
			d1 = pgxp_seq_dist(cand[i][1].seq, ref);
			lo = d0 < d1 ? d0 : d1; hi = d0 < d1 ? d1 : d0;
			if (hi / 4 > lo + spread + 16) {
				pick[i] = (d0 <= d1) ? 0 : 1;
				decided[i] = 1;
				amb &= ~(1 << i);
			}
		}
	}
	/* Stage B: depth-fit scoring over the still-ambiguous corners. NOTE: in
	 * his v0.2 this block is DEAD CODE — the stage-A block clears amb
	 * unconditionally, so the combination search never runs. Here stage A
	 * leaves undecided corners ambiguous, so stage B runs live, as designed.
	 * The seq spread is computed over corners that have candidates only (a
	 * corner with no candidate has no seq to spread); the depth ratio uses
	 * the corners that have a depth. */
	if (amb) {
		int u[4], m = 0, combo, best = -1;
		float best_sc = 0.f;
		for (i = 0; i < n; i++) if (!decided[i]) u[m++] = i;
		/* At most 2^m combinations (m <= 3 for a triangle: <= 8, within his
		 * <= 16 bound). Decided corners keep their stage-A pick. */
		for (combo = 0; combo < (1 << m); combo++) {
			int cp[4];
			float px[4] = { 0.f }, py[4] = { 0.f }, zz[4] = { 0.f }, sc = 0.f;
			int allz = 1, nz = 0, a, b;
			float hi = 0.f, lo = 0.f;
			unsigned mx = 0;
			for (i = 0; i < n; i++) cp[i] = pick[i];
			for (i = 0; i < m; i++) cp[u[i]] = (combo >> i) & 1;
			for (i = 0; i < n; i++) {
				if (nc[i] > 0) {
					px[i] = cand[i][cp[i]].x; py[i] = cand[i][cp[i]].y;
					zz[i] = cand[i][cp[i]].invw > 0.f ? 1.f / cand[i][cp[i]].invw : 0.f;
				} else {
					px[i] = (float)sx[i]; py[i] = (float)sy[i]; zz[i] = 0.f;
				}
				if (zz[i] <= 0.f) allz = 0;
				else {
					if (nz == 0) lo = hi = zz[i];
					else { if (zz[i] < lo) lo = zz[i]; if (zz[i] > hi) hi = zz[i]; }
					nz++;
				}
			}
			if (allz && !pgxp_depths_fit(px, py, zz, n)) sc += 1e7f;
			for (a = 0; a < n; a++) {
				if (nc[a] == 0) continue;
				for (b = a + 1; b < n; b++) {
					unsigned d;
					if (nc[b] == 0) continue;
					d = pgxp_seq_dist(cand[a][cp[a]].seq, cand[b][cp[b]].seq);
					if (d > mx) mx = d;
				}
			}
			sc += (mx > 1000000u) ? 1000000.f : (float)mx;
			if (nz > 0 && lo > 0.f) sc += (hi / lo - 1.f) * 0.5f;
			if (best < 0 || sc < best_sc) { best = combo; best_sc = sc; }
		}
		for (i = 0; i < m; i++) { pick[u[i]] = (best >> i) & 1; decided[u[i]] = 1; }
	}
	/* Every ambiguous corner got a choice (stage A, stage B, or the best
	 * combination above); count them as resolved. */
	for (i = 0; i < n; i++) {
		if (nc[i] > 1) pgxp_stats.amb_resolved++;
	}
}

/* T1: base-tracking shadows. */

void pgxp_on_rtps(unsigned packed, float x, float y, float invw, int valid)
{
	if (!pgxp_track) return;
	/* RTPS shifts the SXY FIFO first (SXY0<-SXY1<-SXY2), then writes the new
	 * SXY2: mirror the shift here. No CPU instruction can observe the GTE
	 * state between the reg shift and this call (a GTE op is atomic), so
	 * shifting + setting together is exactly equivalent. */
	pgxp_gte[12] = pgxp_gte[13];
	pgxp_gte[13] = pgxp_gte[14];
	if (valid) {
		pgxp_gte[14].valid = 1; pgxp_gte[14].i = packed;
		pgxp_gte[14].x = x; pgxp_gte[14].y = y; pgxp_gte[14].invw = invw;
		pgxp_gte[14].seq = ++pgxp_seq; pgxp_gte[14].frame = pgxp_frame;
	} else {
		pgxp_gte[14].valid = 0;
	}
}

void pgxp_on_rtpt(int v, unsigned packed, float x, float y, float invw, int valid)
{
	int reg;
	if (!pgxp_track || v < 0 || v > 2) return;
	/* RTPT writes all three vertices straight into SXY0/1/2: no FIFO shift. */
	reg = 12 + v;
	if (valid) {
		pgxp_gte[reg].valid = 1; pgxp_gte[reg].i = packed;
		pgxp_gte[reg].x = x; pgxp_gte[reg].y = y; pgxp_gte[reg].invw = invw;
		pgxp_gte[reg].seq = ++pgxp_seq; pgxp_gte[reg].frame = pgxp_frame;
	} else {
		pgxp_gte[reg].valid = 0;
	}
}

void pgxp_on_mfc2(int rd, int rt, unsigned val)
{
	int src;
	if (!pgxp_track || rt == 0) return; /* r0 never carries a shadow */
	src = (rd == 15) ? 14 : rd; /* MFC2 from SXYP reads SXY2 */
	if (src < 12 || src > 14 || !pgxp_gte[src].valid || pgxp_gte[src].i != val) {
		pgxp_gpr[rt].valid = 0;
		return;
	}
	pgxp_gpr[rt] = pgxp_gte[src];
}

void pgxp_on_mtc2(int rt, int rd, unsigned data, int from_ram)
{
	pgxp_shadow_t in;
	if (!pgxp_track) return;
	in.valid = 0;
	if (!from_ram && rt != 0 && pgxp_gpr[rt].valid && pgxp_gpr[rt].i == data)
		in = pgxp_gpr[rt];
	/* from_ram (LWC2): the value came from RAM, whose shadow is unknown
	 * until T3, so the destination is always unproven. */
	if (rd == 15) {
		/* MTC2 to SXYP shifts the FIFO, exactly like the register side. */
		pgxp_gte[12] = pgxp_gte[13];
		pgxp_gte[13] = pgxp_gte[14];
		pgxp_gte[14] = in;
	} else if (rd >= 12 && rd <= 14) {
		pgxp_gte[rd] = in;
	}
	/* Other regs (RGB, control, ...) carry no screen-coordinate shadow. */
}

void pgxp_on_notify(unsigned opcode, unsigned data)
{
	unsigned op, rs, rt, rd;
	if (!pgxp_track) return;
	op = opcode >> 26;
	if (op == 0x11) {
		/* COP2 moves: rs 0x00=MFC2, 0x02=CFC2, 0x04=MTC2, 0x06=CTC2. */
		rs = (opcode >> 21) & 0x1f;
		rt = (opcode >> 16) & 0x1f;
		rd = (opcode >> 11) & 0x1f;
		if (rs == 0x00) pgxp_on_mfc2(rd, rt, data);
		else if (rs == 0x04) pgxp_on_mtc2(rt, rd, data, 0);
		/* CFC2/CTC2: control regs, no shadow. */
	} else if (op == 0x32) {
		/* LWC2: the GTE reg is the rt field; value came from RAM (unproven). */
		rd = (opcode >> 16) & 0x1f;
		pgxp_on_mtc2(0, rd, data, 1);
	}
	/* SWC2 (0x3a): GTE -> RAM store. No RAM shadow until T3; ignore. */
}

/* T2: shadows for GP0 command words (CPU direct writes only). */

#define PGXP_CMD_LEN 1024 /* must match gpulib CMD_BUFFER_LEN (plugins/gpulib/gpu.h) */

static pgxp_shadow_t pgxp_cmd_shadow[PGXP_CMD_LEN];

void pgxp_cmd_write(int idx, unsigned word)
{
	int r, found = -1, count = 0;
	if (idx < 0 || idx >= PGXP_CMD_LEN) return;
	pgxp_cmd_shadow[idx].valid = 0;
	if (!pgxp_track) return;
	/* Reverse lookup: the GP0 handler sees the value, not the source
	 * register, so attach the shadow only when exactly one live register
	 * shadow claims this integer. Zero or ambiguous -> no shadow (safe
	 * fallback to the integer vertex). Non-vertex words may also match;
	 * harmless, since only xy-word positions are ever read. */
	for (r = 1; r < 32; r++) {
		if (pgxp_gpr[r].valid && pgxp_gpr[r].i == word) {
			found = r;
			count++;
		}
	}
	if (count == 1) {
		pgxp_cmd_shadow[idx] = pgxp_gpr[found];
		pgxp_stats.cmd_unique++;
	} else if (count > 1) {
		pgxp_stats.cmd_ambig++;
	} else {
		pgxp_stats.cmd_none++;
	}
}

void pgxp_cmd_shift(int left, int total)
{
	if (left <= 0 || total <= left || total > PGXP_CMD_LEN) return;
	memmove(&pgxp_cmd_shadow[0], &pgxp_cmd_shadow[total - left],
		left * sizeof pgxp_cmd_shadow[0]);
}

void pgxp_cmd_fill_none(int start, int count)
{
	int i;
	if (start < 0) { count += start; start = 0; }
	if (start + count > PGXP_CMD_LEN) count = PGXP_CMD_LEN - start;
	for (i = 0; i < count; i++)
		pgxp_cmd_shadow[start + i].valid = 0;
}

/* T3: RAM shadow flow. */

void pgxp_on_sw_ram(unsigned addr, unsigned word, int rt, int is_cop2)
{
	pgxp_shadow_t *slot;
	const pgxp_shadow_t *src = NULL;
	if (!pgxp_track) return;
	slot = pgxp_ram_slot(addr);
	if (!slot) return;
	if (is_cop2) {
		/* GTE source: SXY regs only; SXYP (15) reads SXY2, like MFC2. */
		int s = (rt == 15) ? 14 : rt;
		if (s >= 12 && s <= 14) src = &pgxp_gte[s];
	} else if (rt != 0) {
		src = &pgxp_gpr[rt]; /* r0 never carries a shadow */
	}
	/* Copy as-is only when the source shadow claims exactly the word
	 * stored; anything else (stale shadow, unshadowed value, non-SXY
	 * GTE reg) clears the slot so no stale shadow survives. Read-time
	 * validation (slot.i == word at DMA/use time) is the second net. */
	if (src && src->valid && src->i == word) *slot = *src;
	else slot->valid = 0;
}

void pgxp_on_lw_ram(unsigned addr, unsigned word, int rt, int is_cop2)
{
	const pgxp_shadow_t *slot;
	if (!pgxp_track) return;
	slot = pgxp_ram_slot(addr);
	if (is_cop2) {
		if (rt == 15) {
			/* LWC2 to SXYP shifts the FIFO, exactly like MTC2. */
			pgxp_gte[12] = pgxp_gte[13];
			pgxp_gte[13] = pgxp_gte[14];
			if (slot && slot->valid && slot->i == word) pgxp_gte[14] = *slot;
			else pgxp_gte[14].valid = 0;
		} else if (rt >= 12 && rt <= 14) {
			if (slot && slot->valid && slot->i == word) pgxp_gte[rt] = *slot;
			else pgxp_gte[rt].valid = 0;
		}
		/* Other GTE regs carry no screen-coordinate shadow. */
	} else if (rt != 0) {
		if (slot && slot->valid && slot->i == word) pgxp_gpr[rt] = *slot;
		else pgxp_gpr[rt].valid = 0;
	}
}

/* Sub-word + unaligned stores: exact-match-or-invalidate on the merged
 * post-write word (see pgxp.h). The slot lookup floors an unaligned addr
 * to its containing word, which is exactly the word the caller read back.
 * Range check first: a non-RAM/scratchpad addr yields NULL, never an I/O
 * alias, and the op then only counts (the caller passed merged=0). */
static void pgxp_store_merged_word(unsigned addr, unsigned merged, int rt)
{
	pgxp_shadow_t *slot;
	const pgxp_shadow_t *src = NULL;
	slot = pgxp_ram_slot(addr);
	if (!slot) return;
	if (rt != 0) src = &pgxp_gpr[rt]; /* r0 never carries a shadow */
	if (src && src->valid && src->i == merged) {
		/* Sub-word SET replacing a different existing shadow: the slot
		 * held a valid shadow for another word (stale) or the same word
		 * with a different invw (different depth) — sources churned.
		 * Same shadow (same i, invw within epsilon) is a no-op re-SET
		 * and is not counted. SW/SWC2 never reach this helper (they
		 * have their own path), so this counts sub-word SETs only. */
		if (slot->valid) {
			float d = slot->invw - src->invw;
			if (d < 0) d = -d;
			if (slot->i != src->i || d > 0.001f)
				pgxp_stats.set_repl++;
		}
		*slot = *src;
	} else slot->valid = 0;
}

void pgxp_on_sb_ram(unsigned addr, unsigned merged, int rt)
{
	if (!pgxp_track) return;
	pgxp_stats.st_sb++;
	pgxp_store_merged_word(addr, merged, rt);
}

void pgxp_on_sh_ram(unsigned addr, unsigned merged, int rt)
{
	if (!pgxp_track) return;
	pgxp_stats.st_sh++;
	pgxp_store_merged_word(addr, merged, rt);
}

void pgxp_on_swl_ram(unsigned addr, unsigned merged, int rt)
{
	if (!pgxp_track) return;
	pgxp_stats.st_swl++;
	pgxp_store_merged_word(addr, merged, rt);
}

void pgxp_on_swr_ram(unsigned addr, unsigned merged, int rt)
{
	if (!pgxp_track) return;
	pgxp_stats.st_swr++;
	pgxp_store_merged_word(addr, merged, rt);
}

void pgxp_on_lb_ram(int rt)
{
	if (!pgxp_track) return;
	pgxp_stats.ld_lb++;
	if (rt != 0) pgxp_gpr[rt].valid = 0;
}

void pgxp_on_lh_ram(int rt)
{
	if (!pgxp_track) return;
	pgxp_stats.ld_lh++;
	if (rt != 0) pgxp_gpr[rt].valid = 0;
}

void pgxp_on_lwl_ram(unsigned addr, unsigned word, int rt)
{
	if (!pgxp_track) return;
	pgxp_stats.ld_lwl++;
	if ((addr & 3) == 0) pgxp_on_lw_ram(addr, word, rt, 0);
	else if (rt != 0) pgxp_gpr[rt].valid = 0;
}

void pgxp_on_lwr_ram(unsigned addr, unsigned word, int rt)
{
	if (!pgxp_track) return;
	pgxp_stats.ld_lwr++;
	if ((addr & 3) == 0) pgxp_on_lw_ram(addr, word, rt, 0);
	else if (rt != 0) pgxp_gpr[rt].valid = 0;
}

void pgxp_cmd_dma_word(int idx, unsigned word, unsigned psx_addr)
{
	const pgxp_shadow_t *slot;
	if (idx < 0 || idx >= PGXP_CMD_LEN) return;
	pgxp_cmd_shadow[idx].valid = 0;
	if (!pgxp_track) return;
	slot = pgxp_ram_slot(psx_addr);
	if (slot && slot->valid && slot->i == word)
		pgxp_cmd_shadow[idx] = *slot;
	/* else: no shadow (same fallback as pgxp_cmd_fill_none). */
}

const pgxp_shadow_t *pgxp_cmd_shadow_at(const uint32_t *list, const uint32_t *base)
{
	if (!list) return NULL;
	if (base && list >= base && list < base + PGXP_CMD_LEN)
		return &pgxp_cmd_shadow[list - base];
	/* T3: a RAM command list (DMA direct path, GPUwriteDataMem) maps into
	 * the RAM shadow array: the list is contiguous RAM words, and the
	 * array is indexed by RAM word, so the list's shadows start at the
	 * list's own word offset. do_poly validates each shadow against its
	 * word at use time (sv->i == word), so a stale slot (RAM rewritten
	 * after the shadow was recorded) falls back safely. */
	if (pgxp_ram_base && list >= pgxp_ram_base &&
	    list < pgxp_ram_base + PGXP_RAM_WORDS)
		return &pgxp_ram_shadow[list - pgxp_ram_base];
	return NULL;
}

/* Drop every PGXP shadow/cache/map entry at once (master toggle in menu.c,
 * or a new title starting in config_game_begin, so no shadow from the
 * previous title can be borrowed). The game-frame clock is NOT reset: it is
 * a clock, and old stamps age out by the arithmetic. Defined last because
 * pgxp_cmd_shadow (T2 section) is only declared above. */
void pgxp_invalidate_all(void)
{
	memset(pgxp_gpr, 0, sizeof pgxp_gpr);
	memset(pgxp_gte, 0, sizeof pgxp_gte);
	memset(pgxp_ram_shadow, 0, sizeof pgxp_ram_shadow);
	memset(pgxp_scratch_shadow, 0, sizeof pgxp_scratch_shadow);
	memset(pgxp_cmd_shadow, 0, sizeof pgxp_cmd_shadow);
	memset(pgxp_edge_map, 0, sizeof pgxp_edge_map);
	memset(pgxp_cache, 0, sizeof pgxp_cache);
	memset(pgxp_flip_cur, 0, sizeof pgxp_flip_buf[0]);
	memset(pgxp_flip_prev, 0, sizeof pgxp_flip_buf[0]);
	pgxp_flip_used_cur_n = 0;
	pgxp_flip_used_prev_n = 0;
	pgxp_prev_track = 0;
}
