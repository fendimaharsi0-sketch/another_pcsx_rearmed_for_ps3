/* "PGXP-lite": see pgxp_lite.h */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "pgxp_lite.h"
#include "psxmem.h"
#include "r3000a.h"

int pgxp_lite_on;
unsigned int pgxp_lite_frame;
unsigned int pgxp_lite_stores, pgxp_lite_hits, pgxp_lite_misses;
unsigned long long pgxp_lite_ticks;

/* 4-way set-associative, 64K vertices: a 3D game projects a few thousand per
   frame (Tekken 3 about 3000); a vertex lost before its polygon is drawn makes
   that polygon jump between exact and rounded positions from frame to frame.
   A slot is taken over from the oldest frame first. */
#define PGXP_SETS_BITS 14
#define PGXP_WAYS 4
#define PGXP_AGE  3                 /* game frames a vertex stays usable */

/* A "frame" here is a game frame: the game flips its display buffers once per frame
   it draws (GP1(05h) to a new address), and the packets the GPU draws during one
   were mostly built, and their vertices projected, during the one before (later
   still with the GPU thread). Counting screen refreshes instead let vertices
   expire before a 30 or 20 fps game drew them: some corners of a polygon were
   found and others not, so its texture changed shape from one frame to the next.
   A game that never flips (one buffer) ages by 8 refreshes per frame instead. */
static unsigned int pgxp_vsyncs, pgxp_disp = ~0u;

static uint32_t pgxp_seq;           /* vertices projected so far */
static uint32_t pgxp_frame_seq[8];  /* pgxp_seq when each of the last game frames began */

static void next_frame(void)
{
	pgxp_lite_frame++;
	pgxp_frame_seq[pgxp_lite_frame & 7] = pgxp_seq;
}

static uint32_t pgxp_vsync_seq[2];  /* pgxp_seq at the last two screen refreshes */

static void pq_flush(void);         /* the queue (see pgxp_lite_store) */
static unsigned int pq_n;

void pgxp_lite_vsync(void)
{
	pq_flush();
	pgxp_vsync_seq[0] = pgxp_vsync_seq[1]; pgxp_vsync_seq[1] = pgxp_seq;
	if (++pgxp_vsyncs >= 8) { pgxp_vsyncs = 0; next_frame(); }
}

void pgxp_lite_flip(uint32_t disp)
{
	if (disp == pgxp_disp) return;
	pq_flush();
	pgxp_disp = disp;
	pgxp_vsyncs = 0;
	next_frame();
}

/* A pixel's word can stand for two different vertices in one frame (a wall's
   corner next to a fighter's): both are kept, each with its own offset, depth and
   projection number, and the renderer takes for each polygon the combination
   whose corners belong together. */
struct pgxp_ce { int16_t dx, dy; float z; uint32_t seq; };   /* dx, dy in 1/8192 pixel */
struct pgxp_ent {                   /* 32 bytes: a set of 4 is one 128-byte cache line */
	uint32_t key;
	uint16_t frame;                 /* pgxp_lite_frame + 1 when stored (16 bits) */
	uint8_t n, pad;                 /* candidates, 0: empty */
	struct pgxp_ce c[2];
};
static struct pgxp_ent pgxp_tab[1u << PGXP_SETS_BITS][PGXP_WAYS] __attribute__((aligned(128)));

/* Offsets in 1/8192 pixel to float from two small tables: on the PS3's PPU an
   integer reaches a floating-point register only through memory, and loading it
   right after storing it waits for the store (a "load-hit-store", about 40
   cycles). hi[v >> 8] + lo[v & 255] is exactly v / 8192. */
static float i16_hi[256], i16_lo[256];
static int i16_ok;
static void i16_init(void)
{
	int i;
	for (i = 0; i < 256; i++) {
		i16_hi[i] = (float)(((i ^ 128) - 128) * 256) * (1.f / 8192.f);
		i16_lo[i] = (float)i * (1.f / 8192.f);
	}
	i16_ok = 1;
}
static inline float i16f(int16_t v)
{
	uint16_t u = (uint16_t)v;
	return i16_hi[u >> 8] + i16_lo[u & 255];
}

/* idx, idy: the offset already in 1/8192 pixel (converted in pq_prepare) */
static inline void ce_put(struct pgxp_ce *e, int32_t idx, int32_t idy, float z, uint32_t seq)
{
	e->dx = (int16_t)idx; e->dy = (int16_t)idy; e->z = z; e->seq = seq;
}
static inline void ce_get(struct pgxp_cand *c, const struct pgxp_ce *e)
{
	c->dx = i16f(e->dx); c->dy = i16f(e->dy); c->z = e->z; c->seq = e->seq;
}
static inline struct pgxp_ent *pgxp_set(uint32_t k)
{
	return pgxp_tab[(k * 2654435761u) >> (32 - PGXP_SETS_BITS)];
}

/* the GTE's screen FIFO (SXY0, SXY1, SXY2) as precise vertices: each projection
   pushes one, like the GTE */
static struct { uint32_t sxy; struct pgxp_cand c; int32_t idx, idy; } pgxp_fifo[3];

/* Welding: two body parts that each project their own copy of a joint (RE3's
   Jill: shoulders, waist) put it a fraction of a pixel apart. The PS1 rounds both
   to whole pixels and they usually meet; drawn exactly, a thin slit opens between
   the parts. A vertex within a quarter pixel (and 3% in depth) of one projected
   this frame into a neighbouring pixel's word takes that one's exact place (the
   same pixel's word: see the dedup in pgxp_lite_store). Only vertices near a
   pixel's edge can have such a neighbour, so few are checked. */
unsigned int pgxp_lite_welds;
static int weld(uint32_t sxy, float fsx, float fsy, float *dx, float *dy, float *z, uint16_t now)
{
	int sx = (int16_t)sxy, sy = (int16_t)(sxy >> 16), ox, oy;
	int xs = *dx > 0.25f ? 1 : *dx < -0.25f ? -1 : 0, ys = *dy > 0.25f ? 1 : *dy < -0.25f ? -1 : 0;
	for (oy = 0; oy <= 1; oy++)
		for (ox = 0; ox <= 1; ox++) {
			int nx = sx + ox * xs, ny = sy + oy * ys, i, k;
			uint32_t key;
			const struct pgxp_ent *set;
			float nxf, nyf;             /* nx, ny as floats, without a conversion */
			if ((!ox && !oy) || (ox && !xs) || (oy && !ys)) continue;
			nxf = fsx + (ox ? (xs > 0 ? 1.f : -1.f) : 0.f);
			nyf = fsy + (oy ? (ys > 0 ? 1.f : -1.f) : 0.f);
			key = (uint32_t)(uint16_t)nx | ((uint32_t)(uint16_t)ny << 16);
			set = pgxp_set(key);
			for (i = 0; i < PGXP_WAYS; i++) {
				const struct pgxp_ent *e = &set[i];
				if (!e->n || e->key != key || e->frame != now) continue;
				for (k = 0; k < e->n; k++) {
					/* projected since the last screen refresh: an older projection
					   of the same vertex (a game that never flips: its "frames"
					   are 8 refreshes) is where it was, not where it is */
					if ((int32_t)(e->c[k].seq - pgxp_vsync_seq[1]) <= 0) continue;
					{
					float ex = nxf + i16f(e->c[k].dx) - fsx, ey = nyf + i16f(e->c[k].dy) - fsy;
					float d = e->c[k].z - *z;
					if (ex - *dx < 0.25f && *dx - ex < 0.25f && ey - *dy < 0.25f && *dy - ey < 0.25f
					    && d < *z * 0.03f && d > -*z * 0.03f) {
						*dx = ex; *dy = ey; *z = e->c[k].z;
						pgxp_lite_welds++;
						return 1;
					}
					}
				}
				break;
			}
		}
	return 0;
}

static void store_do_(uint32_t sxy, float dx, float dy, float z, float fsx, float fsy, int32_t idx, int32_t idy)
{
	struct pgxp_ent *set = pgxp_set(sxy), *e = NULL;
	uint16_t now = (uint16_t)(pgxp_lite_frame + 1);
	struct pgxp_cand c;
	int i;
	pgxp_lite_stores++;
	if (z > 0.f && (dx > 0.25f || dx < -0.25f || dy > 0.25f || dy < -0.25f)
	    && weld(sxy, fsx, fsy, &dx, &dy, &z, now)) {
		idx = (int32_t)(dx * 8192.f); idy = (int32_t)(dy * 8192.f);   /* (rare) */
	}
	c.dx = dx; c.dy = dy; c.z = z; c.seq = ++pgxp_seq;
	pgxp_fifo[0] = pgxp_fifo[1]; pgxp_fifo[1] = pgxp_fifo[2];
	pgxp_fifo[2].sxy = sxy; pgxp_fifo[2].c = c; pgxp_fifo[2].idx = idx; pgxp_fifo[2].idy = idy;
	for (i = 0; i < PGXP_WAYS; i++)
		if (set[i].n && set[i].key == sxy) { e = &set[i]; break; }
	if (e && e->frame == now) {
		/* the same word twice in a frame at about the same depth: the same point
		   (a vertex projected again for another polygon, or a joint that two body
		   parts each project, RE3's Jill): one place for all its polygons, the first
		   one's, or a gap opens between the parts. At another depth: a different
		   vertex rounding to the same pixel (a second candidate). */
		for (i = 0; i < (int)e->n; i++) {
			float d = e->c[i].z - z;
			if (d < z * 0.03f && d > -z * 0.03f) {
				/* projected before the last screen refresh: the vertex has moved
				   since (a game that never flips keeps one "frame" for 8
				   refreshes), the new place counts */
				if ((int32_t)(e->c[i].seq - pgxp_vsync_seq[1]) <= 0) ce_put(&e->c[i], idx, idy, z, c.seq);
				else e->c[i].seq = c.seq;
				return;
			}
		}
		if (e->n < 2) ce_put(&e->c[e->n++], idx, idy, z, c.seq);
		else ce_put(&e->c[e->c[0].seq < e->c[1].seq ? 0 : 1], idx, idy, z, c.seq);   /* a third one: the older goes */
		return;
	}
	if (!e) {   /* an empty way, else the one stored longest ago */
		e = &set[0];
		for (i = 1; i < PGXP_WAYS && e->n; i++)
			if (!set[i].n || (uint16_t)(now - set[i].frame) > (uint16_t)(now - e->frame))
				e = &set[i];
	}
	e->key = sxy;
	e->frame = now;
	ce_put(&e->c[0], idx, idy, z, c.seq);
	e->n = 1;
}

int pgxp_lite_cands(uint32_t sxy, struct pgxp_cand *c)
{
	const struct pgxp_ent *set;
	uint16_t now = (uint16_t)(pgxp_lite_frame + 1);
	int i;
	if (pq_n) pq_flush();
	set = pgxp_set(sxy);
	for (i = 0; i < PGXP_WAYS; i++) {
		const struct pgxp_ent *e = &set[i];
		if (e->n && e->key == sxy && (uint16_t)(now - e->frame) <= PGXP_AGE) {
			ce_get(&c[0], &e->c[0]);
			if (e->n > 1) ce_get(&c[1], &e->c[1]);
			pgxp_lite_hits++;
			return (int)e->n;
		}
	}
	pgxp_lite_misses++;
	return 0;
}

int pgxp_lite_find(uint32_t sxy, float *dx, float *dy, float *z)
{
	struct pgxp_cand c[2];
	int n = pgxp_lite_cands(sxy, c);
	if (!n) return 0;
	*dx = c[0].dx; *dy = c[0].dy;
	if (z) { z[0] = c[0].z; z[1] = n > 1 ? c[1].z : 0.f; }
	return 1;
}

/* Vertices by the RAM word the game stored them to with SWC2 (most games write
   the GTE's results straight into their GPU packets). Found by address there is
   no mixing up of two vertices that rounded to the same pixel, the weakness of
   the table above: a wall's corner next to a fighter's took the fighter's offset
   and depth, and part of the wall jumped. One entry per word of the 2 MB of RAM
   (8 MB), made at the first SWC2 with PGXP on. */
struct pgxp_aent {                  /* 16 bytes */
	uint32_t val, seq;              /* seq: the vertex's projection number, 0 none */
	float z;
	int16_t dx, dy;                 /* 1/8192 pixel */
};
static struct pgxp_aent *pgxp_atab;
static int pgxp_atab_failed;
unsigned int pgxp_lite_addr_hits;
int pgxp_lite_no_addr;              /* host tests: by value only */

static void swc2_do(uint32_t p, uint32_t val)
{
	struct pgxp_aent *e = &pgxp_atab[(p & 0x1ffffc) >> 2];
	int k;
	for (k = 2; k >= 0; k--)            /* the newest first */
		if (pgxp_fifo[k].sxy == val) break;
	if (k < 0) { e->seq = 0; return; }   /* not a projected vertex (set by the game) */
	e->val = val;
	e->dx = (int16_t)pgxp_fifo[k].idx;
	e->dy = (int16_t)pgxp_fifo[k].idy;
	e->z = pgxp_fifo[k].c.z;
	e->seq = pgxp_fifo[k].c.seq;
}

/* The queue. The GTE hook only records the GTE's integers (pgxp_lite_project);
   the work is done later, for many projections together, in two passes over
   each group of PQ_CHUNK:
   - pq_prepare: the projection in floating point, the integers loaded straight
     into floating-point registers (they were stored long before, so no load
     waits for its store) and the offsets converted to 1/8192 pixel; the table
     lines the second pass needs (the vertex's set, the sets of the neighbours a
     weld looks at, the SWC2's entry) are fetched from memory meanwhile;
   - the tables updated, event after event, in order, as before.
   Done one projection at a time, each conversion waited for its store and each
   table line for memory: 1.3 ms per frame in Tekken 3, 4 ms in Gran Turismo 2
   on the PS3. Everything queued is done before anything reads the tables and
   before a frame or refresh begins, so the results are the same. */
#define PQ_LEN   64
#define PQ_CHUNK 16
struct pq_ev {                       /* one cache line */
	union { int64_t i; double d; } v[8];   /* a projection: x, y, z, h, ofx, ofy, sx, sy */
	uint32_t kind, a, b;             /* PQ_PROJ (a: word, b: 1 the divider's result is used, 2 widescreen), PQ_SWC2
	                                    (a: address, b: value), PQ_STORE (a: word; dx, dy, z given) */
	float dx, dy, z, fsx, fsy;       /* by pq_prepare: offset, depth, the word's x, y */
	int32_t idx, idy;                /* the offset in 1/8192 pixel */
} __attribute__((aligned(128)));
enum { PQ_PROJ, PQ_SWC2, PQ_STORE };
static struct pq_ev pq[PQ_LEN];

#if defined(__powerpc64__) && defined(__GNUC__)
static inline double pq_cvt(const volatile double *bits)   /* the 8-byte integer at bits, to double */
{
	double r, b = *bits;
	__asm__("fcfid %0,%1" : "=d"(r) : "d"(b));
	return r;
}
#define PQ_CVT(ev, k) pq_cvt(&(ev)->v[k].d)
#else
#define PQ_CVT(ev, k) ((double)(ev)->v[k].i)
#endif

static void pq_prepare(struct pq_ev *ev)
{
	float dx, dy;
	if (ev->kind == PQ_SWC2) {
		__builtin_prefetch(&pgxp_atab[(ev->a & 0x1ffffc) >> 2], 1);
		return;
	}
	if (ev->kind == PQ_PROJ) {
		double x = PQ_CVT(ev, 0), y = PQ_CVT(ev, 1), z = PQ_CVT(ev, 2), hd = PQ_CVT(ev, 3);
		double ofx = PQ_CVT(ev, 4), ofy = PQ_CVT(ev, 5), fsx = PQ_CVT(ev, 6), fsy = PQ_CVT(ev, 7);
		double px = 0.0, py = 0.0, k;
		ev->fsx = (float)fsx; ev->fsy = (float)fsy;
		if (z < 4096.0) {           /* behind the camera: x, y, z are 4096 times camera units */
			ev->dx = ev->dy = ev->z = 0.f;
		} else {
			if (ev->b & 1) {
				k = hd / z;
				if (ev->b & 2)      /* widescreen: X at 3/4 from the centre, as the GTE (gte_widescreen) */
					px = ofx * (1.0 / 65536.0) + x * k * 0.75 - fsx;
				else
					px = ofx * (1.0 / 65536.0) + x * k - fsx;
				py = ofy * (1.0 / 65536.0) + y * k - fsy;
				if (!(px > -2.0 && px < 2.0 && py > -2.0 && py < 2.0))
					px = py = 0.0;      /* clamped: where the PS1 draws it */
			}
			ev->dx = (float)px; ev->dy = (float)py; ev->z = (float)(z * (1.0 / 4096.0));
		}
	} else {                        /* (not used by the GTE hook) */
		ev->fsx = (float)(int16_t)ev->a; ev->fsy = (float)(int16_t)(ev->a >> 16);
	}
	dx = ev->dx; dy = ev->dy;
	ev->idx = (int32_t)(dx * 8192.f); ev->idy = (int32_t)(dy * 8192.f);
	__builtin_prefetch(pgxp_set(ev->a), 1);
	if (ev->z > 0.f && (dx > 0.25f || dx < -0.25f || dy > 0.25f || dy < -0.25f)) {
		/* the neighbouring pixels' entries weld() will look at */
		int sx = (int16_t)ev->a, sy = (int16_t)(ev->a >> 16);
		int xs = dx > 0.25f ? 1 : dx < -0.25f ? -1 : 0, ys = dy > 0.25f ? 1 : dy < -0.25f ? -1 : 0;
		if (xs) __builtin_prefetch(pgxp_set((uint32_t)(uint16_t)(sx + xs) | ((uint32_t)(uint16_t)sy << 16)));
		if (ys) __builtin_prefetch(pgxp_set((uint32_t)(uint16_t)sx | ((uint32_t)(uint16_t)(sy + ys) << 16)));
		if (xs && ys) __builtin_prefetch(pgxp_set((uint32_t)(uint16_t)(sx + xs) | ((uint32_t)(uint16_t)(sy + ys) << 16)));
	}
}

static void pq_flush(void)
{
	unsigned int i, j, n = pq_n;
#if defined(__powerpc__) || defined(__PPC__)
	unsigned long long t0 = __builtin_ppc_get_timebase();
#endif
	pq_n = 0;
	if (!i16_ok) i16_init();
	for (i = 0; i < n; i += PQ_CHUNK) {
		unsigned int e = i + PQ_CHUNK < n ? i + PQ_CHUNK : n;
		for (j = i; j < e; j++) pq_prepare(&pq[j]);
		for (j = i; j < e; j++) {
			const struct pq_ev *ev = &pq[j];
			if (ev->kind == PQ_SWC2) swc2_do(ev->a, ev->b);
			else store_do_(ev->a, ev->dx, ev->dy, ev->z, ev->fsx, ev->fsy, ev->idx, ev->idy);
		}
	}
#if defined(__powerpc__) || defined(__PPC__)
	pgxp_lite_ticks += __builtin_ppc_get_timebase() - t0;
#endif
}

static inline struct pq_ev *pq_new(unsigned int kind, uint32_t a, uint32_t b)
{
	struct pq_ev *ev;
	if (pq_n == PQ_LEN) pq_flush();
	ev = &pq[pq_n++];
	ev->kind = kind; ev->a = a; ev->b = b;
	return ev;
}

void pgxp_lite_project(int64_t x, int64_t y, int64_t z, uint32_t h, int32_t ofx, int32_t ofy,
                       int16_t sx, int16_t sy, int near)
{
	struct pq_ev *ev = pq_new(PQ_PROJ, (uint32_t)(uint16_t)sx | ((uint32_t)(uint16_t)sy << 16), near);
	ev->v[0].i = x; ev->v[1].i = y; ev->v[2].i = z; ev->v[3].i = h;
	ev->v[4].i = ofx; ev->v[5].i = ofy; ev->v[6].i = sx; ev->v[7].i = sy;
}

void pgxp_lite_store(uint32_t sxy, float dx, float dy, float z)
{
	struct pq_ev *ev = pq_new(PQ_STORE, sxy, 0);
	ev->dx = dx; ev->dy = dy; ev->z = z;
}

void pgxp_lite_swc2(uint32_t addr, uint32_t val)
{
	uint32_t p = addr & 0x1fffffff;
	if (p >= 0x800000) return;          /* not RAM (the scratchpad: copied later, not followed) */
	if (!pgxp_atab) {
		if (pgxp_atab_failed) return;
		pgxp_atab = calloc(0x200000 / 4, sizeof(*pgxp_atab));
		if (!pgxp_atab) { pgxp_atab_failed = 1; return; }
	}
	pq_new(PQ_SWC2, p, val);
}

int pgxp_lite_cand_at(const void *ptr, uint32_t word, struct pgxp_cand *c)
{
	const struct pgxp_aent *e;
	uintptr_t off;
	if (pq_n) pq_flush();
	if (!pgxp_atab || pgxp_lite_no_addr) return -1;   /* (RAM may be mapped at address 0: Lightrec on a PC) */
	off = (uintptr_t)((const uint8_t *)ptr - (const uint8_t *)psxRegs.ptrs.psxM);
	if (off >= 0x200000) return -1;     /* not read from RAM (a copied command) */
	e = &pgxp_atab[off >> 2];
	/* projected within the last PGXP_AGE game frames */
	if (!e->seq || e->val != word || (int32_t)(e->seq - pgxp_frame_seq[(pgxp_lite_frame - PGXP_AGE) & 7]) <= 0) return -1;
	c->dx = i16f(e->dx);
	c->dy = i16f(e->dy);
	c->z = e->z;
	c->seq = e->seq;
	pgxp_lite_addr_hits++;
	return 1;
}

/* the renderer, before looking up a polygon's corners: their table entries are
   fetched from memory together rather than one cache miss after another (the
   lookups cost several milliseconds per frame on the PS3 in 3D games) */
void pgxp_lite_prefetch(const uint32_t *const *ptr, const uint32_t *words, int n)
{
	int i;
	if (pq_n) pq_flush();
	for (i = 0; i < n; i++) {
		__builtin_prefetch(pgxp_set(words[i]));   /* one cache line */
		if (pgxp_atab && !pgxp_lite_no_addr) {
			uintptr_t off = (uintptr_t)((const uint8_t *)ptr[i] - (const uint8_t *)psxRegs.ptrs.psxM);
			if (off < 0x200000) __builtin_prefetch(&pgxp_atab[off >> 2]);
		}
	}
}

/* Forget every stored vertex. The word table and the address table hold
   entries stamped with the frame counter as it stood when PGXP was switched
   off, so they would still count as fresh for PGXP_AGE frames after switching
   back on; the GTE FIFO would seed new address-table entries with the same
   stale offsets, and the queue still holds unflushed projections from before
   the switch. All four go. What stays: the statistics (diagnostics, not
   lookups) and the frame/projection counters (they only order entries, and
   every entry after this is fresh; the projection number stays monotonic, so
   a fresh store always outranks anything the tables could still name). */
void pgxp_lite_invalidate(void)
{
	memset(pgxp_tab, 0, sizeof(pgxp_tab));
	if (pgxp_atab) memset(pgxp_atab, 0, (0x200000 / 4) * sizeof(*pgxp_atab));
	memset(pgxp_fifo, 0, sizeof(pgxp_fifo));
	pq_n = 0;
}

void pgxp_lite_set(int on)
{
	if (!pgxp_lite_on && on) pgxp_lite_invalidate();
	pgxp_lite_on = on;
}

/* ---- Culling correction (NCLIP, see pgxp_lite.h) ----
 * NCLIP reads the same lite word table the renderer draws from (see
 * pgxp_lite_nclip below), so culling and drawing cannot disagree per
 * triangle: a vertex that the draw placed precisely is culled from
 * that same place, and a vertex the table never saw keeps the integer
 * on both sides. The gate is pgxp_cull_track (setting AND lite mode). */

int pgxp_cull_track;
unsigned int pgxp_nclip_total, pgxp_nclip_flip, pgxp_nclip_restore, pgxp_nclip_nogte;
unsigned int pgxp_nclip_guess, pgxp_nclip_dzero;

void pgxp_cull_set(int on)
{
	/* Effective only while lite is on. NCLIP reads the lite table
	 * directly, so there is nothing to wipe on a toggle: the table is
	 * the draw's own data, and pgxp_lite_set invalidates it on OFF ->
	 * ON. Toggling culling while lite stays on just starts or stops
	 * consulting it. */
	pgxp_cull_track = on && pgxp_lite_on;
}

/* The packed SXY word for an integer vertex: SX in the low half, SY in
 * the high half — the same packing the GTE register holds (gteSX0 is
 * CP2D.p[12].sw.l, gteSY0 is .sw.h). Reconstructed here from the halves
 * NCLIP already passes, so the signature is unchanged. */
static inline unsigned pgxp_pack_sxy(int sx, int sy)
{
	return (unsigned)(uint16_t)sx | ((unsigned)(uint16_t)sy << 16);
}

/* ---- NCLIP from the lite word table (Option 1) ----
 * The earlier attempt (Design A) read a separate GTE register shadow
 * for NCLIP while the renderer drew from the lite word table: two
 * independent sources that could disagree per triangle — and the
 * shadow's coordinates were never verified against the picture the way
 * lite's are (the draw proves them every frame), so a 97-99.8% hook
 * rate meant nothing if the values were wrong. Design A is deleted;
 * NCLIP reads the SAME table the draw reads, with the SAME lookup
 * (pgxp_lite_cands) and the SAME ambiguity rule (do_poly's first pass:
 * a word standing for two vertices takes the one projected much closer
 * in time to its sibling corners, else the first one).
 * The two things the draw has that NCLIP does not are NOT replicated:
 * the address-table disambiguation (NCLIP has no packet address) and
 * the combination search over the polygon's depths (a draw-layer rule,
 * libpcsxcore must not call into hwr). Where the draw settled a word by
 * address, NCLIP's seq rule can still pick the other candidate — the
 * residual mismatch, bounded to ambiguous words.
 * A word with no candidate keeps the integer result (counted in
 * pgxp_nclip_nogte): the draw uses the integer for that corner too, so
 * the two agree. Determinant, sliver bump and flip/restore counting
 * are the same as the draw's precise winding would give. */
int pgxp_lite_nclip(int sx0, int sy0, int sx1, int sy1, int sx2, int sy2, int64_t *det)
{
	const int sx[3] = { sx0, sx1, sx2 };
	const int sy[3] = { sy0, sy1, sy2 };
	struct pgxp_cand cd[3][2];
	int nc[3], pick[3] = { 0, 0, 0 };
	float x[3], y[3], d;
	int64_t idet;
	int i, guessed = 0;
	if (!pgxp_cull_track) return 0;
	for (i = 0; i < 3; i++)
		nc[i] = pgxp_lite_cands(pgxp_pack_sxy(sx[i], sy[i]), cd[i]);
	/* Ambiguity, do_poly's first pass (same formula): the unambiguous
	   corners set the seq window (smin/smax) and the reference (ref =
	   the last one's seq); a two-candidate corner takes the candidate
	   projected clearly closer to it, else the first one. A corner with
	   no candidate at all keeps the integer result for the triangle. */
	{
		uint32_t smin = 0, smax = 0, ref = 0;
		int k = 0;
		for (i = 0; i < 3; i++) if (nc[i] == 1) {
			uint32_t q = cd[i][0].seq;
			if (!k || (int32_t)(q - smin) < 0) smin = q;
			if (!k || (int32_t)(q - smax) > 0) smax = q;
			ref = q; k++;
		}
		for (i = 0; i < 3; i++) {
			if (nc[i] == 2) {
				uint32_t d0 = cd[i][0].seq - ref, d1 = cd[i][1].seq - ref, lo, hi;
				if ((int32_t)d0 < 0) d0 = -d0;
				if ((int32_t)d1 < 0) d1 = -d1;
				lo = d0 < d1 ? d0 : d1; hi = d0 < d1 ? d1 : d0;
				if (k && hi / 4 > lo + (smax - smin) + 16) pick[i] = d1 < d0;
				else guessed = 1;   /* no clear winner: the first candidate, a guess */
			} else if (nc[i] == 0) {
				pgxp_nclip_nogte++;
				return 0;
			}
		}
	}
	for (i = 0; i < 3; i++) {
		x[i] = (float)sx[i] + cd[i][pick[i]].dx;
		y[i] = (float)sy[i] + cd[i][pick[i]].dy;
	}
	d = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
	if (d == 0.0f) { pgxp_nclip_dzero++; return 0; }   /* precise winding zero: integer result kept */
	/* The integer winding, same formula as gteNCLIP_'s (per-term s64 casts;
	 * the two agree for every reachable coordinate: the GTE clamps SX/SY
	 * to [-1024, 1023], so no product can overflow a 32-bit int). */
	idet = (int64_t)sx0 * (sy1 - sy2) + (int64_t)sx1 * (sy2 - sy0) + (int64_t)sx2 * (sy0 - sy1);
	pgxp_nclip_total++;
	if (guessed) pgxp_nclip_guess++;
	if ((idet > 0 && d < 0.0f) || (idet < 0 && d > 0.0f))
		pgxp_nclip_flip++;
	else if (idet == 0)
		pgxp_nclip_restore++;
	if (d > 0.0f && d < 1.0f) d = 1.0f;
	else if (d < 0.0f && d > -1.0f) d = -1.0f;
	*det = (int64_t)d;
	return 1;
}
