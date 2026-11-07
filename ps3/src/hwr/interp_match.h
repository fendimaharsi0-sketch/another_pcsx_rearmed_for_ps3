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
 * Rules (Salsa64's, streaming form — the fork's lists stream through do_poly,
 * so matching is a per-vertex lookup, not a post-frame batch):
 * - per-vertex cut: a vertex that moved more than INTERP_DISP px in one game
 *   frame is not interpolated (camera cut, teleport);
 * - per-polygon all-or-nothing: enforced by the caller — every drawn vertex
 *   of the polygon must have a valid prev, else none of them moves;
 * - per-frame cut (at present): >5% of lookups moved, or >50% of paired
 *   vertices displaced >60 px, or <50% of vertices found a partner.
 * The atomic rules (collapse / object / component) are NOT here yet — Salsa64
 * added them in interp 6-8, after the basic mode worked; see INTERP-DESIGN.md.
 */
#ifndef INTERP_MATCH_H
#define INTERP_MATCH_H

#include <stdint.h>
#include <stddef.h>

#define INTERP_MAX_VERTS 32768   /* vertices per frame table (Salsa64: VM_CLIP 32768) */
#define INTERP_DISP 120.0f       /* per-vertex cut, screen px (Salsa64: INTERP_DISP 120) */
#define INTERP_DISP_HALF 60.0f   /* per-frame cut: paired displaced more than this */

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
 * interp_log_reset, same pattern as the PGXP corrector counters). */
extern unsigned int interp_lookups, interp_hits_addr, interp_hits_hash,
    interp_moved, interp_sprites, interp_frames_cut, interp_frames_fb,
    interp_full;
void interp_log_reset(void);

#endif /* INTERP_MATCH_H */
