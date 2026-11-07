/* Frame interpolation by geometry: vertex matcher. See interp_match.h and
 * INTERP-DESIGN.md (worktree root) for the design and the Salsa64 mapping.
 *
 * Tables: two ping-pong open-address hash maps (prev / cur), one entry per
 * captured vertex: key (kind + base + occurrence), final (x, y), frame id.
 * At present the tables swap and the new cur is memset to zero, so a table
 * only ever holds one frame's entries (no generational tagging needed). The
 * occurrence map (key base -> count this frame) is cleared at the same time.
 * A present with no captures is a no-op (duplicate present keeps prev).
 */
#include "interp_match.h"

#include <stdlib.h>
#include <string.h>

#define INTERP_TABLE_BITS 15
#define INTERP_TABLE_SIZE (1u << INTERP_TABLE_BITS)
#define INTERP_TABLE_MASK (INTERP_TABLE_SIZE - 1)
#define INTERP_OCC_BITS 15
#define INTERP_OCC_SIZE (1u << INTERP_OCC_BITS)
#define INTERP_OCC_MASK (INTERP_OCC_SIZE - 1)   /* one slot per distinct base per frame, like the vertex table */

/* key packing: kind (2 bits, top) | base (32 bits) | occ (16 bits, low).
 * kind is never 0, so a key is never 0 and 0 marks an empty slot. */
#define INTERP_KEY_KIND_ADDR ((uint64_t)1 << 62)
#define INTERP_KEY_KIND_HASH ((uint64_t)2 << 62)
#define INTERP_KEY_OCC_MASK 0xffffu

typedef struct { uint64_t key; float x, y; uint32_t frame; } interp_entry_t;
typedef struct { uint64_t base; uint32_t count; } interp_occ_t;

static interp_entry_t *interp_prev, *interp_cur;
static interp_occ_t *interp_occ;
static int interp_mode_v;
static uint32_t interp_frame;      /* frames presented with captures */
static uint32_t interp_cur_count;  /* vertices captured in the current frame */
static int interp_fb_flag;         /* vram_read/copy happened this frame */
static int interp_frame_cut_v;     /* cut decision for the last presented frame */
/* frame-local counts (for the per-frame cut, reset at present) */
static unsigned int f_lookups, f_paired, f_moved, f_disp60;

unsigned int interp_lookups, interp_hits_addr, interp_hits_hash,
    interp_moved, interp_sprites, interp_frames_cut, interp_frames_fb,
    interp_full;

static uint32_t interp_mix(uint64_t key, uint32_t bits)
{
    uint32_t h = (uint32_t)key ^ (uint32_t)(key >> 32);
    h *= 2654435761u;
    return h >> (32 - bits);
}

void interp_log_reset(void)
{
    interp_lookups = interp_hits_addr = interp_hits_hash = interp_moved =
        interp_sprites = interp_frames_cut = interp_frames_fb = interp_full = 0;
}

void interp_set(int mode)
{
    if (mode < INTERP_OFF) mode = INTERP_OFF;
    if (mode > INTERP_TEST_HALF) mode = INTERP_TEST_HALF;
    if (mode == INTERP_OFF) {
        free(interp_prev);
        free(interp_cur);
        free(interp_occ);
        interp_prev = interp_cur = NULL;
        interp_occ = NULL;
        interp_mode_v = INTERP_OFF;
        return;
    }
    if (!interp_prev) {
        interp_prev = (interp_entry_t *)calloc(INTERP_TABLE_SIZE, sizeof *interp_prev);
        interp_cur = (interp_entry_t *)calloc(INTERP_TABLE_SIZE, sizeof *interp_cur);
        interp_occ = (interp_occ_t *)calloc(INTERP_OCC_SIZE, sizeof *interp_occ);
        if (!interp_prev || !interp_cur || !interp_occ) {
            free(interp_prev);
            free(interp_cur);
            free(interp_occ);
            interp_prev = interp_cur = NULL;
            interp_occ = NULL;
            interp_mode_v = INTERP_OFF;
            return;
        }
        interp_frame = 0;
        interp_cur_count = 0;
        interp_fb_flag = 0;
        interp_frame_cut_v = 0;
        f_lookups = f_paired = f_moved = f_disp60 = 0;
    }
    interp_mode_v = mode;
}

int interp_mode(void) { return interp_mode_v; }
int interp_active(void) { return interp_mode_v != INTERP_OFF; }

float interp_test_t(void)
{
    if (interp_mode_v == INTERP_TEST0) return 0.0f;
    if (interp_mode_v == INTERP_TEST_HALF) return 0.5f;
    return -1.0f;
}

void interp_invalidate(void)
{
    if (!interp_prev || !interp_cur || !interp_occ) return;
    memset(interp_prev, 0, INTERP_TABLE_SIZE * sizeof *interp_prev);
    memset(interp_cur, 0, INTERP_TABLE_SIZE * sizeof *interp_cur);
    memset(interp_occ, 0, INTERP_OCC_SIZE * sizeof *interp_occ);
    interp_frame = 0;
    interp_cur_count = 0;
    interp_fb_flag = 0;
    interp_frame_cut_v = 0;
    f_lookups = f_paired = f_moved = f_disp60 = 0;
}

uint32_t interp_attr_hash(uint32_t u, uint32_t v, uint32_t r, uint32_t g,
                          uint32_t b, uint32_t texpage, uint32_t clut)
{
    /* FNV-1a over the words (Salsa64 hashes the 16 bytes of each vertex;
     * here the stable attributes are already words). The hash only has to
     * be stable across frames, not to match Salsa64's. */
    uint32_t h = 0x811c9dc5u;
    uint32_t w[7] = { u, v, r, g, b, texpage, clut };
    int i;
    for (i = 0; i < 7; i++) {
        h ^= w[i];
        h *= 0x01000193u;
    }
    return h;
}

float interp_lerp(float prev, float cur, float t)
{
    return prev + (cur - prev) * t;
}

static interp_entry_t *interp_find(interp_entry_t *tab, uint64_t key)
{
    uint32_t i = interp_mix(key, INTERP_TABLE_BITS);
    uint32_t n;
    for (n = 0; n < INTERP_TABLE_SIZE; n++) {
        interp_entry_t *e = &tab[i];
        if (e->key == 0) return NULL;
        if (e->key == key) return e;
        i = (i + 1) & INTERP_TABLE_MASK;
    }
    return NULL;
}

static void interp_store(interp_entry_t *tab, uint64_t key, float x, float y, uint32_t frame)
{
    uint32_t i = interp_mix(key, INTERP_TABLE_BITS);
    uint32_t n;
    for (n = 0; n < INTERP_TABLE_SIZE; n++) {
        interp_entry_t *e = &tab[i];
        if (e->key == 0 || e->key == key) {
            e->key = key;
            e->x = x;
            e->y = y;
            e->frame = frame;
            return;
        }
        i = (i + 1) & INTERP_TABLE_MASK;
    }
    interp_full++;   /* table full: the vertex is dropped, never truncated */
}

/* Occurrence count for this key base this frame (then incremented).
 * Saturates at 0xffff: beyond that, occurrences collide (same key) — the
 * 120 px cut still bounds the damage to one frame. */
static uint32_t interp_occ_next(uint64_t base)
{
    uint32_t i = interp_mix(base, INTERP_OCC_BITS);
    uint32_t n;
    for (n = 0; n < INTERP_OCC_SIZE; n++) {
        interp_occ_t *o = &interp_occ[i];
        if (o->base == 0) {
            o->base = base;
            o->count = 1;
            return 0;
        }
        if (o->base == base) {
            uint32_t c = o->count;
            if (o->count < INTERP_KEY_OCC_MASK) o->count++;
            return c;
        }
        i = (i + 1) & INTERP_OCC_MASK;
    }
    return 0;   /* occurrence map full: all further occurrences share occ 0 */
}

/* One lookup attempt against one table under one key. Returns 1 on a pair
 * (prev-frame entry within INTERP_DISP, prev position written out), 0 on a
 * miss, and sets *displaced when a prev-frame entry existed but was beyond
 * the cut. f_disp60 counts paired vertices displaced more than half the cut. */
static int interp_try(interp_entry_t *tab, uint64_t key, float x, float y,
                      float *out_px, float *out_py, int *displaced)
{
    interp_entry_t *e = interp_find(tab, key);
    float dx, dy;
    if (!e || e->frame != interp_frame - 1) return 0;
    dx = x - e->x;
    dy = y - e->y;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    if (dx > INTERP_DISP || dy > INTERP_DISP) {
        *displaced = 1;
        return 0;
    }
    if (out_px) *out_px = e->x;
    if (out_py) *out_py = e->y;
    f_paired++;
    if (dx > INTERP_DISP_HALF || dy > INTERP_DISP_HALF) f_disp60++;
    return 1;
}

int interp_vertex(int addr_off, uint32_t attr_h, float x, float y,
                  float *out_px, float *out_py)
{
    uint64_t addr_key = 0, hash_key;
    uint32_t hash_occ;
    int hit_addr = 0, hit_hash = 0, displaced = 0;

    if (!interp_prev || !interp_cur || !interp_occ || interp_mode_v == INTERP_OFF)
        return 0;

    /* Occurrence numbering is eager and per key kind: the count of prior
     * same-base vertices this frame, in draw order. The lookup in this
     * frame must use the same number the store used in the previous frame,
     * so both numbers are taken up front, once per vertex, whether or not
     * the key ends up used for the lookup. A lazy hash number would shift
     * whenever an earlier same-attribute vertex paired by address, and the
     * next frame's hash lookup would miss. */
    hash_occ = interp_occ_next(attr_h | INTERP_KEY_KIND_HASH);
    hash_key = INTERP_KEY_KIND_HASH | ((uint64_t)attr_h << 16) | hash_occ;
    if (addr_off >= 0) {
        uint64_t addr_base = (uint64_t)(uint32_t)(addr_off >> 2);
        uint32_t addr_occ = interp_occ_next(addr_base | INTERP_KEY_KIND_ADDR);
        addr_key = INTERP_KEY_KIND_ADDR | (addr_base << 16) | addr_occ;
    }

    f_lookups++;
    interp_lookups++;

    /* 1. Address key first (DMA lists). 2. Attribute-hash fallback on ANY
     * address failure (no entry, wrong frame, or displaced): the CTR case —
     * DMA lists whose RAM addresses do not repeat across frames. A hash
     * rescue is a real pair, not a move. */
    if (addr_off >= 0)
        hit_addr = interp_try(interp_prev, addr_key, x, y, out_px, out_py, &displaced);
    if (!hit_addr)
        hit_hash = interp_try(interp_prev, hash_key, x, y, out_px, out_py, &displaced);
    if (hit_addr)
        interp_hits_addr++;
    else if (hit_hash)
        interp_hits_hash++;
    else if (displaced)
        /* "moved" counts only when a previous-frame candidate existed (by
         * either key) but was displaced beyond the cut AND no pair was
         * found. */
        f_moved++, interp_moved++;

    /* 3. Store under BOTH keys, so the next frame can find this vertex
     * either way. A table-full store is dropped, never truncated. */
    if (addr_off >= 0)
        interp_store(interp_cur, addr_key, x, y, interp_frame);
    interp_store(interp_cur, hash_key, x, y, interp_frame);
    interp_cur_count++;
    return hit_addr || hit_hash;
}

void interp_note_fb(void)
{
    if (interp_mode_v != INTERP_OFF) interp_fb_flag = 1;
}

int interp_frame_cut(void) { return interp_frame_cut_v; }

void interp_present(void)
{
    interp_entry_t *tmp;

    if (!interp_prev || !interp_cur || !interp_occ) {
        interp_fb_flag = 0;
        return;
    }
    if (interp_cur_count == 0) {
        /* duplicate present: nothing was drawn, keep the previous table */
        interp_fb_flag = 0;
        return;
    }
    /* per-frame cut (Salsa64's thresholds, per vertex here): >5% of lookups
     * moved, or >50% of paired vertices displaced >60 px, or <50% of vertices
     * found a partner. */
    interp_frame_cut_v = 0;
    if (f_lookups > 0) {
        if (f_moved * 20u > f_lookups) interp_frame_cut_v = 1;
        else if (f_paired > 0 && f_disp60 * 2u > f_paired) interp_frame_cut_v = 1;
        else if (f_paired * 2u < f_lookups) interp_frame_cut_v = 1;
    }
    if (interp_frame_cut_v) interp_frames_cut++;
    if (interp_fb_flag) interp_frames_fb++;

    tmp = interp_prev;
    interp_prev = interp_cur;
    interp_cur = tmp;
    memset(interp_cur, 0, INTERP_TABLE_SIZE * sizeof *interp_cur);
    memset(interp_occ, 0, INTERP_OCC_SIZE * sizeof *interp_occ);
    interp_cur_count = 0;
    f_lookups = f_paired = f_moved = f_disp60 = 0;
    interp_fb_flag = 0;
    interp_frame++;
}
