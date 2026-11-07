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

/* the RAM byte offset of a packet pointer, or -1 if it is not a RAM address
 * (a copied command) or RAM is mapped at address 0 (host tests). Shared by
 * the interpolation matcher for its address key; independent of pgxp_lite_on
 * (the offset is a fact about the pointer, not about the lite tables). */
int pgxp_ram_off(const void *ptr)
{
	uintptr_t off;
	if (pgxp_lite_no_addr) return -1;   /* (RAM may be mapped at address 0: Lightrec on a PC) */
	off = (uintptr_t)((const uint8_t *)ptr - (const uint8_t *)psxRegs.ptrs.psxM);
	if (off >= 0x200000) return -1;     /* not read from RAM (a copied command) */
	return (int)off;
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

/* ---- PGXP corrector (see pgxp_lite.h): tracking only. ----
 * Ported from the research branch's pgxp.c tracking (T1 register shadows,
 * T3 RAM/scratchpad shadows, sub-word hooks), renamed pgxp_* -> pgxp_corr_*.
 * NOT ported (orphan, stays on the research branch): the slice-1 cache,
 * the command shadow, the edge map, hybrid welds/ambiguity, the
 * texture-precision counters and flip tables, the per-frame wipe, and the
 * do_poly substitution. The sub-word hooks below keep only the tracking
 * (the research sub-word and load counters are gone with the texprec
 * logging they fed). The game-frame clock is lite's
 * (pgxp_lite_frame); the projection sequence is lite's pgxp_seq, shared,
 * so a lite and a corrector candidate for one corner order on one clock. */

int pgxp_corr_track;
unsigned int pgxp_corr_lookups, pgxp_corr_hits, pgxp_corr_guards;

/* T1: per-register shadows. i is the exact integer (packed SXY word) the
 * shadow was recorded for; a shadow is only ever used when the register's
 * current integer still equals i, so a stale shadow fails safe. */
static pgxp_corr_shadow_t pgxp_corr_gpr[32];
static pgxp_corr_shadow_t pgxp_corr_gte[32]; /* data regs; only 12/13/14 (SXY) are used */

/* T3: shadows for RAM + scratchpad words. 2MB RAM = 512K words; 1KB
 * scratchpad = 256 words. PGXP_CORR_RAM_GUARD: polygon tails (a command is
 * at most ~13 words) may read a few words past a RAM list that starts at
 * the very end of RAM; the guard words stay zero (never written), so such
 * reads fall back safely instead of reading past the array. */
#define PGXP_CORR_RAM_WORDS (0x200000 / 4)
#define PGXP_CORR_SCRATCH_WORDS (0x400 / 4)
#define PGXP_CORR_RAM_GUARD 64
static pgxp_corr_shadow_t pgxp_corr_ram_shadow[PGXP_CORR_RAM_WORDS + PGXP_CORR_RAM_GUARD];
static pgxp_corr_shadow_t pgxp_corr_scratch_shadow[PGXP_CORR_SCRATCH_WORDS];

/* Map a PS1 address to its RAM/scratchpad shadow slot, or NULL. Segment
 * stripped like lightrec's kunseg(): KSEG0 (0x8...) and KSEG1 (0xa...) fold
 * onto KUSEG, so all aliases of one physical word share one slot. */
static pgxp_corr_shadow_t *pgxp_corr_ram_slot(unsigned addr)
{
	unsigned phys;
	if (addr >= 0xa0000000u) phys = addr - 0xa0000000u;
	else phys = addr & ~0x80000000u;
	if (phys < 0x00800000u) return &pgxp_corr_ram_shadow[(phys & 0x1fffffu) / 4];
	if (phys >= 0x1f800000u && phys < 0x1f800400u)
		return &pgxp_corr_scratch_shadow[(phys - 0x1f800000u) / 4];
	return NULL;
}

const pgxp_corr_shadow_t *pgxp_corr_ram_shadow_at(unsigned psx_addr)
{
	return pgxp_corr_ram_slot(psx_addr);
}

/* 1 when a shadow is live: valid and projected within PGXP_AGE game frames
 * of lite's clock. Pure predicate (no counting). */
static int pgxp_corr_live(const pgxp_corr_shadow_t *sh)
{
	if (!sh || !sh->valid) return 0;
	return (pgxp_lite_frame - sh->frame) <= PGXP_AGE;
}

void pgxp_corr_on_rtps(unsigned packed, float x, float y, float invw, int valid)
{
	if (!pgxp_corr_track) return;
	/* RTPS shifts the SXY FIFO first (SXY0<-SXY1<-SXY2), then writes the new
	 * SXY2: mirror the shift here. No CPU instruction can observe the GTE
	 * state between the reg shift and this call (a GTE op is atomic), so
	 * shifting + setting together is exactly equivalent. */
	pgxp_corr_gte[12] = pgxp_corr_gte[13];
	pgxp_corr_gte[13] = pgxp_corr_gte[14];
	if (valid) {
		pgxp_corr_gte[14].valid = 1; pgxp_corr_gte[14].i = packed;
		pgxp_corr_gte[14].x = x; pgxp_corr_gte[14].y = y; pgxp_corr_gte[14].invw = invw;
		pgxp_corr_gte[14].seq = ++pgxp_seq; pgxp_corr_gte[14].frame = pgxp_lite_frame;
	} else {
		pgxp_corr_gte[14].valid = 0;
	}
}

void pgxp_corr_on_rtpt(int v, unsigned packed, float x, float y, float invw, int valid)
{
	int reg;
	if (!pgxp_corr_track || v < 0 || v > 2) return;
	/* RTPT writes all three vertices straight into SXY0/1/2: no FIFO shift. */
	reg = 12 + v;
	if (valid) {
		pgxp_corr_gte[reg].valid = 1; pgxp_corr_gte[reg].i = packed;
		pgxp_corr_gte[reg].x = x; pgxp_corr_gte[reg].y = y; pgxp_corr_gte[reg].invw = invw;
		pgxp_corr_gte[reg].seq = ++pgxp_seq; pgxp_corr_gte[reg].frame = pgxp_lite_frame;
	} else {
		pgxp_corr_gte[reg].valid = 0;
	}
}

void pgxp_corr_on_mfc2(int rd, int rt, unsigned val)
{
	int src;
	if (!pgxp_corr_track || rt == 0) return; /* r0 never carries a shadow */
	src = (rd == 15) ? 14 : rd; /* MFC2 from SXYP reads SXY2 */
	if (src < 12 || src > 14 || !pgxp_corr_gte[src].valid || pgxp_corr_gte[src].i != val) {
		pgxp_corr_gpr[rt].valid = 0;
		return;
	}
	pgxp_corr_gpr[rt] = pgxp_corr_gte[src];
}

void pgxp_corr_on_mtc2(int rt, int rd, unsigned data, int from_ram)
{
	pgxp_corr_shadow_t in;
	if (!pgxp_corr_track) return;
	in.valid = 0;
	if (!from_ram && rt != 0 && pgxp_corr_gpr[rt].valid && pgxp_corr_gpr[rt].i == data)
		in = pgxp_corr_gpr[rt];
	/* from_ram (LWC2): the value came from RAM, whose shadow is unknown
	 * until the load hook (pgxp_corr_on_lw_ram, is_cop2=1) sets the real
	 * one, so the destination is always unproven here. */
	if (rd == 15) {
		/* MTC2 to SXYP shifts the FIFO, exactly like the register side. */
		pgxp_corr_gte[12] = pgxp_corr_gte[13];
		pgxp_corr_gte[13] = pgxp_corr_gte[14];
		pgxp_corr_gte[14] = in;
	} else if (rd >= 12 && rd <= 14) {
		pgxp_corr_gte[rd] = in;
	}
	/* Other regs (RGB, control, ...) carry no screen-coordinate shadow. */
}

void pgxp_corr_on_notify(unsigned opcode, unsigned data)
{
	unsigned op, rs, rt, rd;
	if (!pgxp_corr_track) return;
	op = opcode >> 26;
	if (op == 0x11) {
		/* COP2 moves: rs 0x00=MFC2, 0x02=CFC2, 0x04=MTC2, 0x06=CTC2. */
		rs = (opcode >> 21) & 0x1f;
		rt = (opcode >> 16) & 0x1f;
		rd = (opcode >> 11) & 0x1f;
		if (rs == 0x00) pgxp_corr_on_mfc2(rd, rt, data);
		else if (rs == 0x04) pgxp_corr_on_mtc2(rt, rd, data, 0);
		/* CFC2/CTC2: control regs, no shadow. */
	} else if (op == 0x32) {
		/* LWC2: the GTE reg is the rt field; value came from RAM (unproven). */
		rd = (opcode >> 16) & 0x1f;
		pgxp_corr_on_mtc2(0, rd, data, 1);
	}
	/* SWC2 (0x3a): GTE -> RAM store. Handled by pgxp_corr_on_sw_ram on the
	 * store path (interpreter SWC2, Lightrec rec_pgxp_mem), not here. */
}

void pgxp_corr_on_sw_ram(unsigned addr, unsigned word, int rt, int is_cop2)
{
	pgxp_corr_shadow_t *slot;
	const pgxp_corr_shadow_t *src = NULL;
	if (!pgxp_corr_track) return;
	slot = pgxp_corr_ram_slot(addr);
	if (!slot) return;
	if (is_cop2) {
		/* GTE source: SXY regs only; SXYP (15) reads SXY2, like MFC2. */
		int s = (rt == 15) ? 14 : rt;
		if (s >= 12 && s <= 14) src = &pgxp_corr_gte[s];
	} else if (rt != 0) {
		src = &pgxp_corr_gpr[rt]; /* r0 never carries a shadow */
	}
	/* Copy as-is only when the source shadow claims exactly the word
	 * stored; anything else (stale shadow, unshadowed value, non-SXY
	 * GTE reg) clears the slot so no stale shadow survives. Read-time
	 * validation (slot.i == word at lookup time) is the second net. */
	if (src && src->valid && src->i == word) *slot = *src;
	else slot->valid = 0;
}

void pgxp_corr_on_lw_ram(unsigned addr, unsigned word, int rt, int is_cop2)
{
	const pgxp_corr_shadow_t *slot;
	if (!pgxp_corr_track) return;
	slot = pgxp_corr_ram_slot(addr);
	if (is_cop2) {
		if (rt == 15) {
			/* LWC2 to SXYP shifts the FIFO, exactly like MTC2. */
			pgxp_corr_gte[12] = pgxp_corr_gte[13];
			pgxp_corr_gte[13] = pgxp_corr_gte[14];
			if (slot && slot->valid && slot->i == word) pgxp_corr_gte[14] = *slot;
			else pgxp_corr_gte[14].valid = 0;
		} else if (rt >= 12 && rt <= 14) {
			if (slot && slot->valid && slot->i == word) pgxp_corr_gte[rt] = *slot;
			else pgxp_corr_gte[rt].valid = 0;
		}
		/* Other GTE regs carry no screen-coordinate shadow. */
	} else if (rt != 0) {
		if (slot && slot->valid && slot->i == word) pgxp_corr_gpr[rt] = *slot;
		else pgxp_corr_gpr[rt].valid = 0;
	}
}

/* Sub-word + unaligned stores: exact-match-or-invalidate on the merged
 * post-write word. The slot lookup floors an unaligned addr to its
 * containing word, which is exactly the word the caller read back.
 * Range check first: a non-RAM/scratchpad addr yields NULL, never an I/O
 * alias, and the op then only no-ops on the NULL slot lookup. */
static void pgxp_corr_store_merged_word(unsigned addr, unsigned merged, int rt)
{
	pgxp_corr_shadow_t *slot;
	const pgxp_corr_shadow_t *src = NULL;
	slot = pgxp_corr_ram_slot(addr);
	if (!slot) return;
	if (rt != 0) src = &pgxp_corr_gpr[rt]; /* r0 never carries a shadow */
	if (src && src->valid && src->i == merged) *slot = *src;
	else slot->valid = 0;
}

void pgxp_corr_on_sb_ram(unsigned addr, unsigned merged, int rt)
{
	if (!pgxp_corr_track) return;
	pgxp_corr_store_merged_word(addr, merged, rt);
}

void pgxp_corr_on_sh_ram(unsigned addr, unsigned merged, int rt)
{
	if (!pgxp_corr_track) return;
	pgxp_corr_store_merged_word(addr, merged, rt);
}

void pgxp_corr_on_swl_ram(unsigned addr, unsigned merged, int rt)
{
	if (!pgxp_corr_track) return;
	pgxp_corr_store_merged_word(addr, merged, rt);
}

void pgxp_corr_on_swr_ram(unsigned addr, unsigned merged, int rt)
{
	if (!pgxp_corr_track) return;
	pgxp_corr_store_merged_word(addr, merged, rt);
}

void pgxp_corr_on_lb_ram(int rt)
{
	if (!pgxp_corr_track) return;
	if (rt != 0) pgxp_corr_gpr[rt].valid = 0;
}

void pgxp_corr_on_lh_ram(int rt)
{
	if (!pgxp_corr_track) return;
	if (rt != 0) pgxp_corr_gpr[rt].valid = 0;
}

void pgxp_corr_on_lwl_ram(unsigned addr, unsigned word, int rt)
{
	if (!pgxp_corr_track) return;
	if ((addr & 3) == 0) pgxp_corr_on_lw_ram(addr, word, rt, 0);
	else if (rt != 0) pgxp_corr_gpr[rt].valid = 0;
}

void pgxp_corr_on_lwr_ram(unsigned addr, unsigned word, int rt)
{
	if (!pgxp_corr_track) return;
	if ((addr & 3) == 0) pgxp_corr_on_lw_ram(addr, word, rt, 0);
	else if (rt != 0) pgxp_corr_gpr[rt].valid = 0;
}

void pgxp_corr_invalidate(void)
{
	memset(pgxp_corr_gpr, 0, sizeof pgxp_corr_gpr);
	memset(pgxp_corr_gte, 0, sizeof pgxp_corr_gte);
	memset(pgxp_corr_ram_shadow, 0, sizeof pgxp_corr_ram_shadow);
	memset(pgxp_corr_scratch_shadow, 0, sizeof pgxp_corr_scratch_shadow);
}

void pgxp_corr_set(int on)
{
	/* Effective only while lite is on: the front end calls this after
	 * pgxp_lite_set, so pgxp_lite_on is current here. A value change of
	 * the EFFECTIVE track invalidates the shadows (toggling lite off
	 * with the setting on, and back on, both count). */
	int track = on && pgxp_lite_on;
	if (pgxp_corr_track != track) {
		pgxp_corr_track = track;
		pgxp_corr_invalidate();
	}
}

/* The draw-time join (see hwr_core.c): lite missed this corner's word in
 * both its tables, so ask the RAM shadow by the packet word's address.
 * Only the RAM shadow is read here (a GPU packet lives in RAM; a copied
 * command has no RAM address and returns -1, like lite's cand_at).
 *
 * Depth: the shadow's invw is 1/SZ3, and lite's candidate z is the raw
 * projection depth / 4096. The two are the same number: the GTE's SZ3 is
 * MAC3 >> (12-shift) and MAC3 is the raw depth >> shift, so SZ3 is the raw
 * depth >> 12 whatever the shift, and lite's z is the raw depth / 4096.
 * Hence z = 1/invw = SZ3 (up to SZ3's truncation of the raw depth's low 12
 * bits, far inside lite's own depth tolerances). One caveat: SZ3 is clamped
 * to 0xffff by the GTE (limD), so a vertex deeper than 65535 camera units
 * reports z = 65535 here while lite's own z keeps growing; at that depth
 * the perspective ratio barely moves, and lite's depth-fit test is the
 * safety net for a mixed polygon. */
int pgxp_corr_cand_at(const void *ptr, uint32_t word, struct pgxp_cand *c)
{
	const pgxp_corr_shadow_t *sh;
	uintptr_t off;
	int sx, sy;
	float dx, dy;
	if (!pgxp_corr_track) return -1;
	pgxp_corr_lookups++;
	off = (uintptr_t)((const uint8_t *)ptr - (const uint8_t *)psxRegs.ptrs.psxM);
	if (off >= 0x200000) return -1;     /* not read from RAM (a copied command) */
	sh = &pgxp_corr_ram_shadow[off >> 2];
	if (!pgxp_corr_live(sh) || sh->i != word) return -1;
	sx = (int16_t)word; sy = (int16_t)(word >> 16);
	dx = sh->x - (float)sx; dy = sh->y - (float)sy;
	/* Build A guard, at the corrector boundary: a shadow more than 1.5px
	 * from its integer is a teleported vertex (a clamped GTE float whose
	 * integer still validates exactly). Skip it, keep the integer vertex. */
	if (dx > 1.5f || dx < -1.5f || dy > 1.5f || dy < -1.5f) {
		pgxp_corr_guards++;
		return -1;
	}
	c->dx = dx; c->dy = dy;
	c->z = sh->invw > 0.f ? 1.0f / sh->invw : 0.f;
	c->seq = sh->seq;
	pgxp_corr_hits++;
	return 1;
}

/* ---- Culling corrector (NCLIP, see pgxp_lite.h) ----
 * The slice-1 cache: a 4096-entry hash of projected vertices, keyed by
 * integer screen position. Ported from the research branch's slice-1
 * cache (push/lookup), renamed pgxp_corr_*. The slice-1 texture correction
 * that also read this cache there is NOT ported; the only consumer here is
 * the NCLIP decision below. No statistics in the push/lookup (the research
 * hit/miss counters fed the orphan log); the NCLIP counters are the
 * corrector's own (below). */

int pgxp_cull_track;
unsigned int pgxp_corr_nclip_total, pgxp_corr_nclip_flip, pgxp_corr_nclip_restore;

#define PGXP_CORR_CACHE_BITS 12
#define PGXP_CORR_CACHE_SIZE (1 << PGXP_CORR_CACHE_BITS)
#define PGXP_CORR_CACHE_MASK (PGXP_CORR_CACHE_SIZE - 1)

typedef struct { short sx, sy; int valid; unsigned frame; float x, y, invw; } pgxp_corr_entry_t;

static pgxp_corr_entry_t pgxp_corr_cache[PGXP_CORR_CACHE_SIZE];

static inline unsigned pgxp_corr_hash(int sx, int sy)
{
	/* unsigned arithmetic throughout: the signed product would overflow 32
	 * bits for large |sx| (undefined behaviour); the wrapped bit pattern is
	 * identical, so this keeps the old hash's distribution with defined behaviour. */
	return ((unsigned)sx * 73856093u ^ (unsigned)sy * 19349663u) & PGXP_CORR_CACHE_MASK;
}

void pgxp_corr_push(int sx, int sy, float x, float y, int z)
{
	pgxp_corr_entry_t *e;

	if (z <= 0) return;
	e = &pgxp_corr_cache[pgxp_corr_hash(sx, sy)];
	/* First writer wins while the entry is live (same as before, but the
	 * window is PGXP_AGE game frames instead of one presented frame); a
	 * stale or never-written slot takes the new value. */
	if (!e->valid || (pgxp_lite_frame - e->frame) > PGXP_AGE) {
		e->sx = (short)sx; e->sy = (short)sy;
		e->x = x; e->y = y; e->invw = 1.0f / (float)z;
		e->valid = 1; e->frame = pgxp_lite_frame;
	}
	/* else: first writer in this window wins. A later push for the same pixel
	 * (or a hash collision with another pixel) is ignored, so shared edges
	 * resolve to one value and do not flicker frame to frame. */
}

static int pgxp_corr_lookup(int sx, int sy, float *x, float *y, float *invw)
{
	pgxp_corr_entry_t *e = &pgxp_corr_cache[pgxp_corr_hash(sx, sy)];

	if (!e->valid) return 0;
	/* Stale entries fail here (and are cleared, so the slot is reusable). */
	if ((pgxp_lite_frame - e->frame) > PGXP_AGE) { e->valid = 0; return 0; }
	if (e->sx != sx || e->sy != sy) return 0;
	/* Validation (DuckStation's Validate() rule, in this architecture's terms):
	 * the float must claim to describe exactly this integer vertex, in this
	 * window, and lie within ~1px of it. A saturated or stale entry fails here
	 * and the caller falls back to the integer vertex. */
	if (e->x < (float)sx - 0.02f || e->x > (float)sx + 1.02f) return 0;
	if (e->y < (float)sy - 0.02f || e->y > (float)sy + 1.02f) return 0;
	*x = e->x; *y = e->y; *invw = e->invw;
	return 1;
}

void pgxp_corr_cache_invalidate(void)
{
	memset(pgxp_corr_cache, 0, sizeof pgxp_corr_cache);
}

void pgxp_cull_set(int on)
{
	/* Effective only while lite is on (same derivation as pgxp_corr_set). */
	int track = on && pgxp_lite_on;
	if (pgxp_cull_track != track) {
		pgxp_cull_track = track;
		pgxp_corr_cache_invalidate();
	}
}

int pgxp_corr_nclip(int sx0, int sy0, int sx1, int sy1, int sx2, int sy2, int64_t *det)
{
	float x0, y0, x1, y1, x2, y2, iw, d;
	int64_t idet;
	if (!pgxp_cull_track) return 0;
	if (!pgxp_corr_lookup(sx0, sy0, &x0, &y0, &iw) ||
	    !pgxp_corr_lookup(sx1, sy1, &x1, &y1, &iw) ||
	    !pgxp_corr_lookup(sx2, sy2, &x2, &y2, &iw)) return 0;   /* any miss: integer result kept */
	d = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
	if (d == 0.0f) return 0;   /* precise winding zero: integer result kept */
	/* The integer winding, same formula as gteNCLIP_'s (per-term s64 casts;
	 * the two agree for every reachable coordinate: the GTE clamps SX/SY
	 * to [-1024, 1023], so no product can overflow a 32-bit int). */
	idet = (int64_t)sx0 * (sy1 - sy2) + (int64_t)sx1 * (sy2 - sy0) + (int64_t)sx2 * (sy0 - sy1);
	pgxp_corr_nclip_total++;
	if ((idet > 0 && d < 0.0f) || (idet < 0 && d > 0.0f))
		pgxp_corr_nclip_flip++;
	else if (idet == 0)
		pgxp_corr_nclip_restore++;
	if (d > 0.0f && d < 1.0f) d = 1.0f;
	else if (d < 0.0f && d > -1.0f) d = -1.0f;
	*det = (int64_t)d;
	return 1;
}
