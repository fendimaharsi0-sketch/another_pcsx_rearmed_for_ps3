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

/* T1: per-register shadows. i is the exact integer (packed SXY word) the
 * shadow was recorded for; a shadow is only ever used when the register's
 * current integer still equals i, so a stale shadow fails safe. */
static pgxp_shadow_t pgxp_gpr[32];
static pgxp_shadow_t pgxp_gte[32]; /* data regs; only 12/13/14 (SXY) are used */

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
	/* T1: shadows are per-frame, like the cache (see header). */
	memset(pgxp_gpr, 0, sizeof pgxp_gpr);
	memset(pgxp_gte, 0, sizeof pgxp_gte);
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
	if (count == 1)
		pgxp_cmd_shadow[idx] = pgxp_gpr[found];
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

const pgxp_shadow_t *pgxp_cmd_shadow_at(const uint32_t *list, const uint32_t *base)
{
	if (!list || !base || list < base || list >= base + PGXP_CMD_LEN)
		return NULL;
	return &pgxp_cmd_shadow[list - base];
}
