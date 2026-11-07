/* Frame interpolation by geometry: vertex matcher. See interp_match.h and
 * INTERP-DESIGN.md (worktree root) for the design and the Salsa64 mapping.
 *
 * Tables: two ping-pong open-address hash maps (prev / cur), one entry per
 * captured vertex: key (kind + base + occurrence), final (x, y), frame id.
 * At present the tables swap and the new cur is memset to zero, so a table
 * only ever holds one frame's entries (no generational tagging needed).
 * Hash entries are slot-indexed under the same table: the hash key carries
 * NO occurrence as identity — occurrence is only the storage slot (draw
 * order among same-attribute vertices that frame) — and the lookup probes
 * every slot 0..(prev-frame duplicate count - 1). That count comes from a
 * second occurrence map holding the PREVIOUS frame's counts, swapped at
 * present alongside the tables; the current frame's map is memset after the
 * swap. The address key still uses occurrence as identity (its lookup
 * probes the single slot, as before).
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
static interp_occ_t *interp_occ, *interp_occ_prev;
static int interp_mode_v;
static float interp_disp_v = INTERP_DISP;        /* current per-vertex cut, native px */
static float interp_disp_half_v = INTERP_DISP_HALF;   /* half-cut (per-frame rule) */
static uint32_t interp_frame;      /* frames presented with captures */
static uint32_t interp_cur_count;  /* vertices captured in the current frame */
static int interp_fb_flag;         /* vram_read/copy happened this frame */
static int interp_frame_cut_v;     /* cut decision for the last presented frame */
/* frame-local counts (for the per-frame cut, reset at present) */
static unsigned int f_lookups, f_paired, f_moved, f_disp60;

unsigned int interp_lookups, interp_hits_addr, interp_hits_hash,
    interp_moved, interp_sprites, interp_frames_cut, interp_frames_fb,
    interp_full, interp_hash_miss, interp_hash_displaced, interp_hash_ambig,
    interp_hist_121_240, interp_hist_241_480, interp_hist_481_960,
    interp_hist_gt960;

static uint32_t interp_mix(uint64_t key, uint32_t bits)
{
    uint32_t h = (uint32_t)key ^ (uint32_t)(key >> 32);
    h *= 2654435761u;
    return h >> (32 - bits);
}

void interp_log_reset(void)
{
    interp_lookups = interp_hits_addr = interp_hits_hash = interp_moved =
        interp_sprites = interp_frames_cut = interp_frames_fb = interp_full =
        interp_hash_miss = interp_hash_displaced = interp_hash_ambig =
        interp_hist_121_240 = interp_hist_241_480 = interp_hist_481_960 =
        interp_hist_gt960 = 0;
}

void interp_set_disp(int px)
{
    if (px < INTERP_DISP_MIN) px = INTERP_DISP_MIN;
    if (px > INTERP_DISP_MAX) px = INTERP_DISP_MAX;
    interp_disp_v = (float)px;
    interp_disp_half_v = (float)px / 2.0f;
}

int interp_disp(void) { return (int)interp_disp_v; }

void interp_set(int mode)
{
    if (mode < INTERP_OFF) mode = INTERP_OFF;
    if (mode > INTERP_TEST_HALF) mode = INTERP_TEST_HALF;
    if (mode == INTERP_OFF) {
        free(interp_prev);
        free(interp_cur);
        free(interp_occ);
        free(interp_occ_prev);
        interp_prev = interp_cur = NULL;
        interp_occ = interp_occ_prev = NULL;
        interp_mode_v = INTERP_OFF;
        return;
    }
    if (!interp_prev) {
        interp_prev = (interp_entry_t *)calloc(INTERP_TABLE_SIZE, sizeof *interp_prev);
        interp_cur = (interp_entry_t *)calloc(INTERP_TABLE_SIZE, sizeof *interp_cur);
        interp_occ = (interp_occ_t *)calloc(INTERP_OCC_SIZE, sizeof *interp_occ);
        interp_occ_prev = (interp_occ_t *)calloc(INTERP_OCC_SIZE, sizeof *interp_occ_prev);
        if (!interp_prev || !interp_cur || !interp_occ || !interp_occ_prev) {
            free(interp_prev);
            free(interp_cur);
            free(interp_occ);
            free(interp_occ_prev);
            interp_prev = interp_cur = NULL;
            interp_occ = interp_occ_prev = NULL;
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
    if (!interp_prev || !interp_cur || !interp_occ || !interp_occ_prev) return;
    memset(interp_prev, 0, INTERP_TABLE_SIZE * sizeof *interp_prev);
    memset(interp_cur, 0, INTERP_TABLE_SIZE * sizeof *interp_cur);
    memset(interp_occ, 0, INTERP_OCC_SIZE * sizeof *interp_occ);
    memset(interp_occ_prev, 0, INTERP_OCC_SIZE * sizeof *interp_occ_prev);
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

/* Previous frame's occurrence count for this key base (read-only: 0 when
 * the base never appeared last frame). This is what bounds the hash
 * lookup's slot probe: the previous frame stored exactly count entries
 * under this attribute hash, in slots 0..count-1. A store dropped by a
 * full vertex table leaves a hole — its slot simply finds no entry and is
 * skipped, so a gap never hides a later slot. */
static uint32_t interp_occ_count_prev(uint64_t base)
{
    uint32_t i = interp_mix(base, INTERP_OCC_BITS);
    uint32_t n;
    for (n = 0; n < INTERP_OCC_SIZE; n++) {
        interp_occ_t *o = &interp_occ_prev[i];
        if (o->base == 0) return 0;
        if (o->base == base) return o->count;
        i = (i + 1) & INTERP_OCC_MASK;
    }
    return 0;
}

/* One lookup attempt against one table under one key. Returns 0 when no
 * prev-frame entry exists under the key, 1 on a pair (prev-frame entry
 * within the cut, prev position written out), 2 when a prev-frame entry
 * existed but was displaced beyond the cut (its displacement,
 * max(|dx|, |dy|) — the same metric as the cut — written to *out_disp).
 * f_disp60 counts paired vertices displaced more than half the cut. */
static int interp_try(interp_entry_t *tab, uint64_t key, float x, float y,
                      float *out_px, float *out_py, float *out_disp)
{
    interp_entry_t *e = interp_find(tab, key);
    float dx, dy;
    if (!e || e->frame != interp_frame - 1) return 0;
    dx = x - e->x;
    dy = y - e->y;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    if (dx > interp_disp_v || dy > interp_disp_v) {
        if (out_disp) *out_disp = dx > dy ? dx : dy;
        return 2;
    }
    if (out_px) *out_px = e->x;
    if (out_py) *out_py = e->y;
    f_paired++;
    if (dx > interp_disp_half_v || dy > interp_disp_half_v) f_disp60++;
    return 1;
}

/* Occurrence-independent hash lookup: probe EVERY previous-frame entry
 * stored under this attribute hash, in slots 0..(prev-frame duplicate
 * count - 1), and pair with the NEAREST candidate by displacement
 * max(|dx|, |dy|), the same metric as the cut. Ties go to the lowest slot
 * (first stored — deterministic). Returns 0 when no prev-frame candidate
 * exists under the key, 1 on a pair (nearest prev position written out),
 * 2 when candidates existed but all were displaced beyond the cut (the
 * SMALLEST displacement among them written to *out_disp). *out_cand gets
 * the number of prev-frame candidates found (0 when none); the caller
 * counts ambiguity when it exceeds 1. Cost per lookup is O(duplicates of
 * this attribute hash last frame), bounded by INTERP_MAX_VERTS; a flat
 * mesh sharing one attribute hash is the worst case, and each probe is one
 * open-address find. f_paired / f_disp60 count a hash pair exactly as an
 * address pair. */
static int interp_try_hash(interp_entry_t *tab, uint32_t attr_h, float x, float y,
                           float *out_px, float *out_py, float *out_disp,
                           unsigned *out_cand)
{
    uint32_t count = interp_occ_count_prev((uint64_t)attr_h | INTERP_KEY_KIND_HASH);
    uint32_t j;
    unsigned cand = 0;
    float best_disp = -1.0f, best_x = 0.0f, best_y = 0.0f;
    float min_disp = -1.0f;

    /* A stored entry's slot is always < INTERP_MAX_VERTS: the table holds
     * at most that many entries, so slots past it can hold nothing (the
     * occurrence counter itself saturates at 0xffff, higher than the
     * table). The clamp bounds the probe loop per lookup. */
    if (count > INTERP_MAX_VERTS) count = INTERP_MAX_VERTS;
    for (j = 0; j < count; j++) {
        uint64_t key = INTERP_KEY_KIND_HASH | ((uint64_t)attr_h << 16) | j;
        interp_entry_t *e = interp_find(tab, key);
        float dx, dy, d;
        if (!e || e->frame != interp_frame - 1) continue;
        cand++;
        dx = x - e->x;
        dy = y - e->y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        d = dx > dy ? dx : dy;
        if (d <= interp_disp_v) {
            if (best_disp < 0.0f || d < best_disp) {
                best_disp = d;
                best_x = e->x;
                best_y = e->y;
            }
        } else {
            if (min_disp < 0.0f || d < min_disp) min_disp = d;
        }
    }
    if (out_cand) *out_cand = cand;
    if (cand == 0) return 0;
    if (best_disp >= 0.0f) {
        if (out_px) *out_px = best_x;
        if (out_py) *out_py = best_y;
        f_paired++;
        if (best_disp > interp_disp_half_v) f_disp60++;
        return 1;
    }
    if (out_disp) *out_disp = min_disp;
    return 2;
}

int interp_vertex(int addr_off, uint32_t attr_h, float x, float y,
                  float *out_px, float *out_py)
{
    uint64_t addr_key = 0, hash_key;
    uint32_t hash_occ;
    int addr_status = 0, hash_status = 0;   /* 0 no entry, 1 paired, 2 displaced */
    float addr_disp = 0.0f, hash_disp = 0.0f;
    unsigned hash_cand = 0;

    if (!interp_prev || !interp_cur || !interp_occ || !interp_occ_prev || interp_mode_v == INTERP_OFF)
        return 0;

    /* Address occurrence is eager and taken up front, once per vertex:
     * the address lookup in this frame must use the same number the store
     * used in the previous frame, so the number is taken whether or not
     * the key ends up used for the lookup. The HASH key takes no number
     * up front anymore — occurrence is not part of hash identity (CTR
     * re-sorts its display list every frame, so a vertex's occurrence
     * changes frame to frame and an eager hash number would pair it with
     * a different same-attribute vertex). The hash store below takes its
     * slot number at store time, in draw order; the lookup probes every
     * slot the previous frame stored, so lookup and store agree without
     * any number being consumed on the lookup path. */
    if (addr_off >= 0) {
        uint64_t addr_base = (uint64_t)(uint32_t)(addr_off >> 2);
        uint32_t addr_occ = interp_occ_next(addr_base | INTERP_KEY_KIND_ADDR);
        addr_key = INTERP_KEY_KIND_ADDR | (addr_base << 16) | addr_occ;
    }

    f_lookups++;
    interp_lookups++;

    /* 1. Address key first (DMA lists). 2. Attribute-hash fallback on ANY
     * address failure (no entry, wrong frame, or displaced): the CTR case —
     * DMA lists whose RAM addresses do not repeat across frames. The hash
     * lookup probes every previous-frame entry under the attribute hash
     * and pairs with the nearest; a hash rescue is a real pair, not a move. */
    if (addr_off >= 0)
        addr_status = interp_try(interp_prev, addr_key, x, y, out_px, out_py, &addr_disp);
    if (addr_status != 1)
        hash_status = interp_try_hash(interp_prev, attr_h, x, y, out_px, out_py, &hash_disp, &hash_cand);
    if (hash_cand > 1)
        interp_hash_ambig++;   /* more than one prev-frame candidate under the hash key */
    if (addr_status == 1)
        interp_hits_addr++;
    else if (hash_status == 1)
        interp_hits_hash++;
    else {
        /* No pair. The hash was tried (the address did not pair), so its
         * outcome is counted: no entry vs a displaced entry. "moved" and
         * the histogram count only when a previous-frame candidate existed
         * (by either key) but was displaced beyond the cut; the histogram
         * takes the SMALLEST displacement among the displaced candidates
         * found (address and/or every hash candidate), once per rejected vertex. */
        if (hash_status == 0)
            interp_hash_miss++;
        else if (hash_status == 2)
            interp_hash_displaced++;
        if (addr_status == 2 || hash_status == 2) {
            float d = addr_disp;
            if (hash_status == 2 && (addr_status != 2 || hash_disp < addr_disp)) d = hash_disp;
            f_moved++;
            interp_moved++;
            if (d <= 240.0f)
                interp_hist_121_240++;
            else if (d <= 480.0f)
                interp_hist_241_480++;
            else if (d <= 960.0f)
                interp_hist_481_960++;
            else
                interp_hist_gt960++;
        }
    }

    /* 3. Store under BOTH keys, so the next frame can find this vertex
     * either way. The hash slot number is taken here, at store time, in
     * draw order — it is a storage slot only, never an identity input to
     * the lookup. A table-full store is dropped, never truncated. */
    if (addr_off >= 0)
        interp_store(interp_cur, addr_key, x, y, interp_frame);
    hash_occ = interp_occ_next(attr_h | INTERP_KEY_KIND_HASH);
    hash_key = INTERP_KEY_KIND_HASH | ((uint64_t)attr_h << 16) | hash_occ;
    interp_store(interp_cur, hash_key, x, y, interp_frame);
    interp_cur_count++;
    return addr_status == 1 || hash_status == 1;
}

void interp_note_fb(void)
{
    if (interp_mode_v != INTERP_OFF) interp_fb_flag = 1;
}

int interp_frame_cut(void) { return interp_frame_cut_v; }

void interp_present(void)
{
    interp_entry_t *tmp;
    interp_occ_t *tmp_occ;

    if (!interp_prev || !interp_cur || !interp_occ || !interp_occ_prev) {
        interp_fb_flag = 0;
        return;
    }
    if (interp_cur_count == 0) {
        /* duplicate present: nothing was drawn, keep the previous table */
        interp_fb_flag = 0;
        return;
    }
    /* per-frame cut (Salsa64's thresholds, per vertex here): >5% of lookups
     * moved, or >50% of paired vertices displaced more than half the cut,
     * or <50% of vertices found a partner. */
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
    /* the just-finished frame's occurrence counts become the previous
     * frame's counts for the next frame's hash lookups; the new current
     * map (stale counts from two frames ago) is cleared for this frame */
    tmp_occ = interp_occ;
    interp_occ = interp_occ_prev;
    interp_occ_prev = tmp_occ;
    memset(interp_occ, 0, INTERP_OCC_SIZE * sizeof *interp_occ);
    interp_cur_count = 0;
    f_lookups = f_paired = f_moved = f_disp60 = 0;
    interp_fb_flag = 0;
    interp_frame++;
}
