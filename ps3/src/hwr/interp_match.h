/* Frame interpolation by geometry: vertex matcher (portable, no PS3 includes).
 *
 * Salsa64's design (docs/interpolacion.md, vmatch.c) mapped onto a flat GP0
 * command-list renderer. The renderer (hwr_core.c do_poly) feeds it one
 * vertex per emitted polygon vertex; it pairs each against the previous
 * frame's table and captures the vertex for the next frame.
 *
 * Identity: (RAM word offset of the packet word + occurrence index) primary
 * — the analog of Salsa64's (RDRAM address + load order); a content hash over
 * the stable (non-positional) attributes (u, v, r, g, b, texpage, clut)
 * fallback, tried on ANY address miss (no entry, wrong frame, or displaced
 * beyond the cut) — and for direct-write lists, whose packet words go
 * through gpu.cmd_buffer and carry no RAM address, it is the only key.
 * Hardware evidence (CTR, 2026-11-07): a game's display lists may be rebuilt
 * at different RAM addresses every frame, so the address key alone pairs
 * ~0% there; the hash is what rescues those vertices (Salsa64's Perfect
 * Dark lesson). Each vertex is stored under BOTH keys. A PS1 packet word
 * holds screen-space coordinates, which change every frame, so hashing the
 * word itself would be useless as identity (Salsa64 hashes the model-space
 * vertex entry, which is stable).
 *
 * The hash key carries NO occurrence (since 2026-11-07, the
 * occurrence-independent step): CTR re-sorts its display list every frame
 * (OT depth sort), so a vertex's occurrence index changes frame to frame
 * and the old (attributes + occurrence) key matched a DIFFERENT vertex that
 * happened to share attributes and that occurrence — at cut 480 that paired
 * 37-49% of vertices, and Fendi's eyes killed it ("pecah poly berantakan":
 * torn polygons). Occurrence is therefore only a STORAGE slot index: each
 * vertex is stored under KIND_HASH | (attr_hash << 16) | occ, where occ is
 * its draw-order index among same-attribute vertices that frame, and the
 * lookup probes EVERY slot 0..(prev-frame duplicate count - 1) for that
 * attribute hash (the count comes from a second, prev-frame occurrence map
 * swapped at present) and pairs with the NEAREST candidate by displacement
 * max(|dx|, |dy|), the same metric as the cut — nearest-position resolution.
 * Ties go to the lowest slot (first stored). A lookup that finds more than
 * one previous-frame candidate under the hash key counts interp_hash_ambig
 * (see below): high ambiguity means nearest-position is guessing among
 * duplicates. Known limitation, asserted by host test: two same-attribute
 * vertices that SWAP positions between frames cross-pair (each pairs with
 * the other's previous position) — nearest position cannot tell them apart,
 * and the per-polygon all-or-nothing rule is the only guard. The ADDRESS
 * key is unchanged (address + occurrence): address hits are ~0 in CTR
 * gameplay anyway, and its occurrence still disambiguates same-address
 * vertices in stable lists.
 *
 * Rules (Salsa64's, streaming form — the fork's lists stream through do_poly,
 * so matching is a per-vertex lookup, not a post-frame batch):
 * - per-vertex cut: a vertex that moved more than the cut (setting
 *   "interp_disp", default 120 px, native PS1 pixels — Salsa64's
 *   INTERP_DISP 120, same value) in one game frame is not interpolated
 *   (camera cut, teleport);
 * - per-polygon all-or-nothing: enforced by the caller — every drawn vertex
 *   of the polygon must have a valid prev, else none of them moves;
 * - per-frame cut (at present): >5% of lookups moved, or >50% of paired
 *   vertices displaced more than half the cut, or <50% of vertices found
 *   a partner.
 * The atomic rules (collapse / object / component) are NOT here yet — Salsa64
 * added them in interp 6-8, after the basic mode worked; see INTERP-DESIGN.md.
 *
 * Diagnostic split for hash failures + rejection histogram (cumulative
 * counters, reset by interp_log_reset — same window as the log line):
 * - hash_miss: a hash attempt found no previous-frame entry (no entry, or
 *   an entry from an older frame). Counted once per vertex whose hash
 *   attempt failed that way, and only when the hash was actually tried
 *   (the address key did not already pair the vertex).
 * - hash_displaced: a hash attempt found a previous-frame entry but it was
 *   displaced beyond the cut. Counted once per vertex, only when the hash
 *   was tried and did not pair.
 * - histogram: counted once per REJECTED vertex — no pair by either key,
 *   and at least one displaced candidate existed by either key (the same
 *   vertices interp_moved counts). The bucket is the SMALLEST displacement
 *   among the displaced candidates found (address and/or hash — now every
 *   hash candidate under the key, not just one), where
 *   displacement = max(|dx|, |dy|), the same metric as the cut. Buckets
 *   are absolute native px, independent of the current cut: 121-240,
 *   241-480, 481-960, >960. (At cut 240 a 150 px rejection cannot occur —
 *   it pairs — so the 121-240 bucket stays empty at that cut; the buckets
 *   answer "how far did the rejected candidates move", not "how far past
 *   the cut".)
 * - hash_ambig: a hash lookup that found MORE THAN ONE previous-frame
 *   candidate under the hash key, regardless of whether the nearest paired.
 *   Counted once per such lookup, only when the hash was actually tried
 *   (the address key did not already pair the vertex). The log line's last
 *   field; the overlay does NOT show it (its third line is already at the
 *   verified 1230 px width limit — see INTERP-DESIGN.md §7).
 */
#ifndef INTERP_MATCH_H
#define INTERP_MATCH_H

#include <stdint.h>
#include <stddef.h>

#define INTERP_MAX_VERTS 32768   /* vertices per frame table (Salsa64: VM_CLIP 32768) */
#define INTERP_DISP 120.0f       /* default per-vertex cut, native PS1 px (Salsa64: INTERP_DISP 120) */
#define INTERP_DISP_HALF 60.0f   /* default half-cut (per-frame cut: paired displaced more than this) */
#define INTERP_DISP_MIN 60       /* tunable cut range, setting "interp_disp" (menu item 39) */
#define INTERP_DISP_MAX 480

/* setting values (config key "interp", menu item 38) */
#define INTERP_OFF 0
#define INTERP_ON 1
#define INTERP_TEST0 2           /* draw the frame at fixed t=0 (previous frame's geometry) */
#define INTERP_TEST_HALF 3       /* draw the frame at fixed t=0.5 (mid-frame) */

/* Set the mode (0 off, 1 on, 2/3 test). Allocates the tables lazily on first
 * enable; frees them on disable, so OFF costs no memory. Safe to call on
 * every settings change. */
void interp_set(int mode);
int interp_mode(void);
int interp_active(void);         /* mode != 0 */
/* Set the per-vertex cut in native PS1 px (setting "interp_disp": the menu
 * cycles 60/120/240/480, default 120). Clamped to 60..480; the half-cut
 * used by the per-frame rule stays half the cut. Independent of the mode:
 * changing it never allocates or frees the tables. Safe to call on every
 * settings change, next to interp_set. */
void interp_set_disp(int px);
int interp_disp(void);           /* current cut in px */
/* Fixed t for the test modes, or -1.0f when no test lerp applies. */
float interp_test_t(void);
/* Clear both tables (new title): no vertex of the old title may pair. */
void interp_invalidate(void);

/* Attribute hash over the stable (non-positional) vertex attributes. */
uint32_t interp_attr_hash(uint32_t u, uint32_t v, uint32_t r, uint32_t g,
                          uint32_t b, uint32_t texpage, uint32_t clut);
/* lerp helper (t=0 -> prev, t=1 -> cur); the renderer uses it for the test modes. */
float interp_lerp(float prev, float cur, float t);

/* Streaming per-vertex step. addr_off: RAM byte offset of the packet word,
 * or -1 when the pointer is not a RAM address (direct-write list / cmd_buffer
 * copy). attr_h: interp_attr_hash(...) of the vertex's stable attributes.
 * x, y: final drawn position (integer + PGXP offset combined). The address
 * key is tried first; on any address failure the attribute-hash key is
 * tried, and the vertex is stored under BOTH keys for the next frame. On a
 * valid match, *out_px and *out_py get the previous frame's position and the
 * return is 1; else 0. The vertex is always captured into the current
 * frame's table (when active). Returns 0 when inactive. */
int interp_vertex(int addr_off, uint32_t attr_h, float x, float y,
                  float *out_px, float *out_py);

/* Frame boundary: swap cur -> prev, finalise the per-frame cut, clear the
 * occurrence map. Call once per presented GPU frame (hwr_present). A present
 * with no captures (duplicate present) is a no-op: the previous table stays. */
void interp_present(void);

/* Set when any game-initiated vram_read/vram_copy happened during the frame
 * (frontend hook wrappers in hwr_rsx.c). Read at present: marks the frame
 * non-interpolatable for slice 2 and is counted in the log. */
void interp_note_fb(void);

/* Cut decision for the frame just presented (slice-2 gate + log). */
int interp_frame_cut(void);

/* Cumulative counters for the log line (log_stats resets them via
 * interp_log_reset, same pattern as the PGXP corrector counters). The
 * histogram buckets count rejected vertices by smallest displacement
 * (see the counting rule at the top of this header). */
extern unsigned int interp_lookups, interp_hits_addr, interp_hits_hash,
    interp_moved, interp_sprites, interp_frames_cut, interp_frames_fb,
    interp_full, interp_hash_miss, interp_hash_displaced, interp_hash_ambig,
    interp_hist_121_240, interp_hist_241_480, interp_hist_481_960,
    interp_hist_gt960;
void interp_log_reset(void);

#endif /* INTERP_MATCH_H */
