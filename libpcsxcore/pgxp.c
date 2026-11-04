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
 * guard words stay zero (never written, wiped with the array), so such
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
 * position (pre-draw-offset, same keys as pgxp_resolve), value = precise
 * float position + invw from a validated shadow. First-writer wins per
 * frame; a hash collision with a different position is NOT recorded (the
 * later position simply gets no borrow — safe fallback, never a wrong
 * float). Single-slot hash like the slice-1 cache, but a much bigger table
 * (64K slots) so collisions are rare; a 1024x512 direct array would be
 * 10MB and is deliberately NOT used. Wiped every frame by pgxp_next_frame
 * (memset, valid = 0). Storage is declared here (before pgxp_next_frame);
 * the record/lookup functions are defined below, after pgxp_lookup. */
#define PGXP_EDGE_BITS 16
#define PGXP_EDGE_SIZE (1 << PGXP_EDGE_BITS)
#define PGXP_EDGE_MASK (PGXP_EDGE_SIZE - 1)
typedef struct { int valid; int sx, sy; float x, y, invw; int conflict_counted; } pgxp_edge_t;
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
 * NOT wiped by the edge-map memset in pgxp_next_frame (that memset only
 * covers pgxp_edge_map); cur/prev are managed by the flip code itself. */
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
	/* Gate: when the master is off, nothing reads any PGXP array (every hook
	 * is gated on pgxp_track, and hwr_pgxp_tex is 0 so do_poly skips the
	 * cache lookup), so skip the gen bump and all wipes below. The first
	 * next_frame after re-enabling runs the normal path, so no transition
	 * wipe beyond it is needed. Flip tables have their own lifecycle (see
	 * below): OFF frames clear them and reset the transition flag, so the
	 * first ON frame after OFF has no stale baseline to compare against. */
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
	 * compares against the first. Computed BEFORE the wipes below (the
	 * flip tables are separate arrays, not covered by those memsets, but
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
	if (++pgxp_gen == 0) {
		memset(pgxp_cache, 0, sizeof pgxp_cache);
		pgxp_gen = 1;
	}
	/* T1: shadows are per-frame, like the cache (see header). */
	memset(pgxp_gpr, 0, sizeof pgxp_gpr);
	memset(pgxp_gte, 0, sizeof pgxp_gte);
	/* T3: RAM + scratchpad shadows are per-frame, like the registers
	 * (sizeof covers the guard words too, so they stay zero). */
	memset(pgxp_ram_shadow, 0, sizeof pgxp_ram_shadow);
	memset(pgxp_scratch_shadow, 0, sizeof pgxp_scratch_shadow);
	/* Edge map: per-frame, like everything above. */
	memset(pgxp_edge_map, 0, sizeof pgxp_edge_map);
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

	if (e->gen != pgxp_gen) { pgxp_stats.lk_miss++; return 0; }
	if (e->sx != sx || e->sy != sy) { pgxp_stats.lk_miss++; return 0; }
	/* Validation (DuckStation's Validate() rule, in this architecture's terms):
	 * the float must claim to describe exactly this integer vertex, in this
	 * frame, and lie within ~1px of it. A saturated or stale entry fails here
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

void pgxp_edge_record(int sx, int sy, float x, float y, float invw)
{
	pgxp_edge_t *e;
	if (!pgxp_track) return;
	e = &pgxp_edge_map[pgxp_edge_hash(sx, sy)];
	if (e->valid) {
		/* First-writer wins: an occupied slot is left alone (never
		 * overwrite, never borrow a wrong float). But a SAME-position
		 * re-record with a DIFFERENT invw is the edge-warp signal: two
		 * own shadows claiming one screen position with different w
		 * (each triangle uses its OWN invw at draw, bypassing the map,
		 * so the shared edge gets two different w and warps). Count it
		 * once per position per frame (flag resets with the per-frame
		 * map wipe); a hash collision (different position) is not a
		 * conflict and is ignored. */
		if (e->sx == sx && e->sy == sy && !e->conflict_counted) {
			float d = invw - e->invw;
			if (d < 0) d = -d;
			if (d > 0.05f) {
				pgxp_stats.invw_conflict++;
				if (d > pgxp_stats.invw_conflict_max)
					pgxp_stats.invw_conflict_max = d;
				e->conflict_counted = 1;
			}
		}
		return;
	}
	e->valid = 1; e->sx = sx; e->sy = sy; e->x = x; e->y = y; e->invw = invw;
	e->conflict_counted = 0;
}

int pgxp_edge_lookup(int sx, int sy, float *x, float *y, float *invw)
{
	pgxp_edge_t *e;
	if (!pgxp_track) return 0;
	e = &pgxp_edge_map[pgxp_edge_hash(sx, sy)];
	if (!e->valid || e->sx != sx || e->sy != sy) return 0;
	*x = e->x; *y = e->y; *invw = e->invw;
	return 1;
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
