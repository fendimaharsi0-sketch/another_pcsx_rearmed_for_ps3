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
typedef struct { int valid; int sx, sy; float x, y, invw; } pgxp_edge_t;
static pgxp_edge_t pgxp_edge_map[PGXP_EDGE_SIZE];

void pgxp_next_frame(void)
{
	/* Gate: when the master is off, nothing reads any PGXP array (every hook
	 * is gated on pgxp_track, and hwr_pgxp_tex is 0 so do_poly skips the
	 * cache lookup), so skip the gen bump and all wipes below. The first
	 * next_frame after re-enabling runs the normal path, so no transition
	 * wipe beyond it is needed. */
	if (!pgxp_track) return;
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
	if (e->valid) return; /* first-writer wins: an occupied slot — same
		position re-recorded, or a different position colliding — is left
		alone. Never overwrite, never borrow a wrong float. */
	e->valid = 1; e->sx = sx; e->sy = sy; e->x = x; e->y = y; e->invw = invw;
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
