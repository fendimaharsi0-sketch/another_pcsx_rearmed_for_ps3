# Frame interpolation by geometry — design (pgxp-research)

Status: design doc, 2026-11-07. Slice 1 implemented per §10.1; slice 2 (§10.2)
designed, not built.

Source: Salsa64 `docs/interpolacion.md` (design bible, read fully),
`src/rdp/vmatch.{c,h}`, `src/rdp/rdp.{c,h}`, `src/rsp/gfx/vgroup.c`,
`lib/rsxkit` lincomb vertex program. Fork ground truth is this worktree
(pgxp-research @ 536835fb: PGXP-lite + RAM-shadow/NCLIP correctors).

## 0. What Salsa64 does

Games that run at 20/30 fps are shown at 60 fps by rebuilding in-between
frames **from the geometry**, not by blending images. Each output vertex is
paired with the same vertex in the previous game frame; the new frame's
command list is re-drawn with vertex positions interpolated
(`pos = lerp(prev, pos, t)`). Measured on the host (GoldenEye vs ground
truth): **30.0 dB @ 20 fps, 33.6 dB @ 30 fps**, vs 19.9/20.7 dB for repeating
the frame (what we do today) and 21.9/22.7 dB for blending. Pairing rate
97–99% of vertices with no per-game patches. Per game, default OFF, zero
cost when OFF (byte-identical dumps vs main).

Rules that make it work (all in `vmatch.c`):
- **All-or-nothing per group**: one vertex of a `G_VTX` group without a
  partner ⇒ no vertex of the group moves, so a mesh never tears.
- **Per-vertex cut**: a vertex that moved >120 N64 px in one game frame is
  not interpolated (camera cut, teleport). Only near-screen vertices count.
- **Per-frame cut**: >5% of visible groups moved >120 px, or >50% of paired
  groups displaced >60 px, or <50% of vertices found a partner ⇒ the whole
  frame is not interpolated (scene change resolves itself: no partners).
- **Atomic rules** (`vmatch_atomic`, added in interp 6–8): collapse (a group
  whose mid-interpolation radius is <0.7× the smaller end radius — a >90°
  turn — is not interpolated), object (draw-order-adjacent groups that touch
  on screen form an object; if ≥50% of its vertices were rejected, the whole
  object appears in the current pose), component/mesh (union-find over
  triangles; a mesh with any front-facing group un-paired is not interpolated
  at all, so shared triangles never stretch).
- **Exclusions**: 2D rects/HUD (no identity, drawn at the new frame's
  position), flat/orthographic groups (all `w == 1` — 3D HUD), tasks that read
  framebuffer textures (a redraw would sample the target's later contents),
  scrolling textures (left at game rate), near-plane crossers (a vertex whose
  `w` changes sign between frames stays at its current position — it would
  pass through `w = 0` and project to infinity).
- **Scheduling**: `k` = fields between presented frames (2 ⇒ 30 fps, 3 ⇒ 20
  fps, 4 ⇒ 15 fps); on field `j` since the frame appeared, if `j+1 < k`,
  emit an intermediate with `t = (j+1)/k`; the true frame shows in the last
  field. Latency: the true frame completes `k−1` fields late (33 ms @ 20
  fps, 17 ms @ 30 fps). A missed intermediate **repeats the previous field**,
  never jumps to the final frame (forward-then-back reads as flicker).
- **Identity**: `(RDRAM address of the vertex-list entry + occurrence index)`
  primary; content hash (FNV-1a over the 16 bytes of each loaded vertex,
  computed **in the SPE at load time** — the PPE used to re-read RDRAM at
  translate time, by when the game may have rewritten a dynamic buffer)
  fallback, ~1–2% of groups. Perfect Dark double-buffers its vertices, so
  address matching gives 0% there; the hash restores 89–99%.
- **Measured costs (PS3)**: matching 0.8–3.5 ms PPE/task (later 1.8–2.4 ms:
  build ~1.0, pair ~0.8, prev ~0.4); RSX scene ×2 (30→60) or ×3 (20→60) —
  presentation went ~2 → 7–8 ms RSX/field at 5×, and at 5× the RSX does not
  keep up (fields skipped); memory ~2.4 MB tables + one extra buffer set +
  2.7 MB presentation surface @ 3×.

## 1. What maps onto the fork

| Salsa64 | Fork |
|---|---|
| Task (one HLE graphics task / display list) | One presented frame's GP0 lists (between presents) |
| Group (the `n` vertices of one `G_VTX`) | One GP0 polygon (3–4 vertices, `do_poly`) |
| Vertex identity from `G_VTX` loads | Packet-word RAM address + occurrence (§2); attr-hash fallback (§2) |
| Re-issue the task's translated list | Re-submit the frame's retained op/vertex stream (§5) |
| Presentation surface (own target) | Second RSX target, slice 2 (§5, §10.2) |
| VI origin change ⇒ new game frame | `frame.fresh` at the display loop (§4) |
| `vmatch.c` (portable C) | `interp_match.c` — same rules, streaming form (§3) |
| `pos = lerp(prev, pos, t)` in the vertex program | CPU lerp at re-submit in slice 1/2; shader lerp is a later optimisation (§3.4) |
| Per-game option, default OFF, zero cost OFF | Setting `interp`, item 38, per-game, default OFF, full kill-gate (§7) |

## 2. Vertex identity — the honest coverage analysis

This is the open research question for the fork. Salsa64 pairs by the
**stable guest address** of a loaded vertex. The fork's GP0 lists are flat
streams of pre-transformed screen-space vertices — there is no object, no
matrix, no model-space vertex in the list. What identity can actually be
used:

### 2.1 Address key (primary) — DMA lists only

In `do_poly` (`ps3/src/hwr/hwr_core.c:679`), each vertex keeps its
packet-word pointer and word: `xyp[i] = l + idx; xys[i] = xy` (:696–701).
The key is `(RAM offset of xyp[i] , occurrence)`, where occurrence = how
many times that offset already appeared as a vertex in this frame — the
exact analog of Salsa64's `(addr, occ)`, which disambiguates the same model
drawn several times. The offset is derived like lite's `pgxp_lite_cand_at`
(`libpcsxcore/pgxp_lite.c:407-425`): `off = ptr - psxRegs.ptrs.psxM`,
reject if `off >= 0x200000`.

Coverage limits, stated plainly:
- **Direct-write lists have no address.** CPU-fed lists go through
  `gpu.cmd_buffer` (`plugins/gpulib/gpu.c:1025-1032`, 1024 words); the
  pointer into the copy is not a RAM address. The DMA research already
  found this hole (packets split across chain links / a chain started while
  `cmd_len > 0` lose their RAM address). Those vertices fall to the
  attr-hash key (§2.2).
- **Address stability across frames is assumed, not proven — and hardware
  DISPROVED it for CTR (2026-11-07, log SCUS-94426, build `ffa2e34+`).**
  Pairing in gameplay: 0.12% → 0% → 0% → 0.37% → 0.88% per window; the
  static menu scene paired 89%, proving the matcher, not the identity. The
  game rebuilds its display list each frame at different RAM addresses
  (rotating/scratch buffer): the address never repeats. Window 6 is the
  fingerprint — 4012 "moved": an address DID repeat, but the geometry at
  that address was different (scratch reuse), correctly rejected by the
  120 px cut. The slice-1 counters measured this, as designed — the bet
  the whole slice rested on lost for this game.
- **A copied/rebuilt packet at a new address falls to the attr-hash key**
  (§2.2) — since the CTR test, the hash is tried on ANY address miss (no
  entry, wrong frame, displaced beyond the cut), not only when no address
  exists, and each vertex is stored under BOTH keys. The address key stays
  primary (exact); the hash rescues vertices whose list moved.

### 2.2 Attribute-hash key (fallback) — no PS1 analog of Salsa64's hash

Salsa64's content hash works because it hashes the **model-space** vertex
entry in RDRAM, which is stable while only the transform changes. A PS1
packet word holds **screen-space** coordinates, which change every frame —
hashing the packet word as identity is useless.

The stable part of a PS1 vertex is its non-positional attributes: `(u, v,
r, g, b)` + the polygon's texpage/clut. The fallback key is therefore
`FNV-1a(u, v, r, g, b, texpage, clut) + occurrence`. It is strictly weaker
than Salsa64's hash: many vertices share attributes (a flat-shaded wall,
a tiled floor, repeated sprites). Mitigations, in order: occurrence
ordering within the frame, the 120 px displacement window (§3.2), and the
per-polygon all-or-nothing rule (§3.2). A wrong attr-hash match moves a
vertex at most 120 px for one frame and is counted (`interp_hash_bad`
cannot be known directly; the proxy is the hash-key hit rate vs the
address-key hit rate in the log). Since the CTR hardware test, the fallback
runs on ANY address miss and every vertex is stored under BOTH keys (§3.1),
so the next frame can find it either way; occurrence numbering is eager
and per key kind, taken up front once per vertex, so lookup and store
agree across frames given stable draw order. Coverage of the fallback is
UNPROVEN on hardware — the CTR re-test is the evidence that decides it;
the counters report the address-vs-hash split per frame.

### 2.3 What the PGXP tables can and cannot do

- The lite **word table** is keyed by rounded pixel — not an identity (a
  vertex moving >1 px changes key; two vertices can share a key). Useless
  for pairing.
- The lite **address table** is the same key as §2.1 but stores lite
  projections (`dx, dy, z, seq`), not previous-frame positions, and expires
  after `PGXP_AGE` 3 game frames. It cannot serve as the interp store.
- The **corrector RAM shadow** proves "the float at this RAM word is the
  GTE projection of the integer now sitting there, projected ≤3 frames
  ago" — same address+exact-word provenance as §2.1, but it tracks
  projections, not drawn positions. It does not replace the interp store.
- Conclusion: interpolation needs **its own retained per-frame vertex
  store** (§3.1). It reuses only the *keying idea* (and the kill-gate
  pattern) from lite/corrector, not their tables.

## 3. Matcher design (`interp_match.c`, portable, host-testable)

### 3.1 Tables

Two ping-pong tables (`prev`, `cur`), allocated lazily on first enable
(Salsa64 allocates on first enable; the corrector's atab is lazily
`calloc`'d the same way). Each table is an open-address hash map:

- Capacity `INTERP_MAX_VERTS` 32768 entries (Salsa64: `VM_CLIP` 32768).
  Each entry: key `u64` (address-key or hash-key + occurrence, tag bit
  distinguishes them), `x, y` float (final drawn position, §3.3),
  `frame` u16. ~24 B/entry ⇒ ~768 KB per table, ~1.5 MB for both.
- A per-frame occurrence map `key → count`, reset at each present — 32768
  slots, sized to the vertex table (a frame can hold as many distinct bases
  as vertices; a smaller map would fill on heavy scenes and collapse all
  further occurrences to 0). ~512 KB. Total matcher memory ~2.0 MB, all
  freed when the setting goes back to Off.
- Full table ⇒ that frame is not interpolated (`interp_full`, logged),
  never truncated (Salsa64: a task exceeding `VTX_INTERP` is drawn whole
  and not interpolated).

### 3.2 Streaming lookup (why not batch)

Salsa64 matches a whole task at translate time because its tasks are
discrete. The fork's GP0 lists **stream** through `do_poly` during the
frame — the frame is only complete at present, but the vertices must be
lerped (test mode) or stored with prev positions (slice 2) **while they
are emitted**. So the matcher is a streaming per-vertex lookup:

1. During the `do_poly` vertex loop, compute per-vertex key + occurrence
   (occurrence counter incremented as vertices stream in).
2. After the vertex loop (positions final, §3.3), look each vertex up in
   the `prev` table:
   - Hit + `|dx| ≤ cut` and `|dy| ≤ cut` (native PS1 px; the cut is the
     `interp_disp` setting, default 120 — Salsa64's `INTERP_DISP` 120,
     same value — tunable 60/120/240/480, see §7) ⇒
     candidate prev position.
   - Miss, or displacement > cut ⇒ no prev for this vertex (`moved`).
3. **Per-polygon all-or-nothing**: all vertices of the polygon must have
   a valid prev, else none of them lerps / gets a prev stored. (Salsa64's
   group rule, applied to the polygon — the fork's group.)
4. Append the vertex (key, occurrence, final x/y) to the `cur` table.
5. At `hwr_present`, swap `cur` → `prev` and clear `cur` for the next
   frame. Per-frame stats (§3.5) are finalised here, including the
   per-frame cut decision (§3.2, below) which gates slice 2's emission.

What slice 1 deliberately omits (Salsa64 added these in interp 6–8, after
the basic mode worked): the atomic rules (collapse / object / component).
A turning character may therefore show a squashed limb for one frame in
test mode — logged via the `moved`/deformed counters, fixed by the atomic
rules in a later slice. The per-vertex 120 px cut + per-polygon
all-or-nothing already prevent tears and streaks, which are the
unacceptable failure modes.

Per-frame cut (computed at present, logged, gates slice 2): the frame is
not interpolatable if >5% of looked-up vertices were `moved`, or >50% of
paired vertices displaced more than half the cut, or <50% of vertices
found a partner (Salsa64's thresholds, §0; the half-cut follows
`interp_disp`, so at cut 240 the displacement bar is 120 px). It is **not** enforced live during the frame
(the cut is only known at frame end) — in slice 1 test mode the per-vertex
and per-polygon rules are the live guards; in slice 2 the cut gates the
intermediate emission.

### 3.3 Capture point — final drawn positions

The capture/lookup hook sits in `do_poly` **after** the PGXP-lite /
corrector lookup block (`hwr_core.c:726-890`) and **after** the sprite
test (`:900-901`), before `emit()`. Rationale:
- Captured positions must be the **final** screen positions (integer base
  + PGXP sub-pixel offset), not the integer ones — otherwise the lerp
  fights the PGXP correction. The lerp replaces the final position and
  zeroes the offset, so the shader does not add it again (exact
  integration against `put()` verified during implementation).
- Sprite exclusion (§6) needs `spr`, known only after the sprite test.

### 3.4 Lerp: CPU now, shader later

Salsa64 lerps in the vertex program (second attribute `prev`, uniform `t`).
The fork's `hwr_vtx` is a fixed 72-byte layout with all six integer
attributes already bound (TEX1–TEX5, `rsxr_submit_`); a second position
stream would widen the struct (the same non-gateable layout change the
lite merge already had to accept) or need a parallel array + attribute
binding — real RSX plumbing. Slice 1/2 therefore lerp **on the PPU** at
re-submit / emit time: `x = prev_x + (cur_x - prev_x) * t`. Same pixels
up to float precision, zero shader changes, and the OFF path is untouched.
A shader-lerp variant (second attribute + `lerp` uniform, Salsa64's
`rsxkit_lincomb_lerp.vcg`) is a later optimisation, not a correctness
requirement.

### 3.5 Counters (log line, every 10 s while the RSX renderer runs —
`log_stats` is called from `hooks_log_gpu_stats`, itself every 10 s in the
game loop, gated only on `frame.gpu`, not on Debug log; the earlier "every
60 presented frames" was wrong)

`hwr: interp: %u lookups, %u hits (addr %u, hash %u), %u moved, %u sprites,
%u frames cut, %u fb-excluded, %u dropped (table full); hash miss %u, hash
displaced %u; hist 121-240 %u, 241-480 %u, 481-960 %u, >960 %u` — same shape
as the corrector's log line, existing fields first and in order. The
address-vs-hash split is the coverage evidence for §2; the dropped count
is the table-capacity evidence. New fields (2026-11-07, tunable-cut step):
- `hash miss`: a hash attempt found no previous-frame entry (no entry, or
  an entry from an older frame). Counted once per vertex whose hash attempt
  failed that way, and only when the hash was actually tried (the address
  key did not already pair the vertex).
- `hash displaced`: a hash attempt found a previous-frame entry but it was
  displaced beyond the cut. Counted once per vertex, only when the hash
  was tried and did not pair.
- `hist …`: counted once per REJECTED vertex — no pair by either key, and
  at least one displaced candidate existed by either key (the same vertices
  `moved` counts). The bucket is the SMALLEST displacement among the
  displaced candidates found (address and/or hash), where displacement =
  max(|dx|, |dy|), the same metric as the cut. Buckets are absolute native
  px, independent of the current cut: 121-240, 241-480, 481-960, >960.
  (At cut 240 a 150 px rejection cannot occur — it pairs — so the 121-240
  bucket stays empty at that cut; the buckets answer "how far did the
  rejected candidates move", not "how far past the cut".)
These answer the open CTR question: rejections clustering at 121-240 mean
loosening the cut to 240 is right; spreading to >960 means the candidates
are wrong vertices (recycled addresses / wrong occurrences), and loosening
would only pair wrong vertices. `interp_log_reset()` resets the new
counters too; the existing counters' semantics did not change.

## 4. Scheduling (fresh vs repeated fields)

The main loop presents **every** display pass (`ps3/src/main.c:966-988`):
`video_begin(); draw_current_frame(); if (frame.fresh) { frame.fresh = 0;
fps_frames++; } video_present();`. `frame.fresh` is set by
`hooks_frame_from_gpu` (RSX path) or `video_cb` (software path;
`data == NULL` = duplicate, returns early without setting fresh,
`main.c:316-324`).

- **Repeated field** ⟺ a display pass where `frame.fresh == 0` at draw
  time. That is the RSX-path duplicate signal (the software path already
  has `video_cb(NULL)`).
- **k measurement**: count display passes between fresh frames
  (`k = 2` ⇒ 30 fps game at 60 Hz, `k = 3` ⇒ 20 fps). A 60 fps game
  (`k = 1`) never enters — same as Salsa64's `last_gap == 1` guard.
- **t**: on the `j`-th field since the frame appeared (`j+1 < k`),
  `t = (j+1)/k` (k=2: t=1/2; k=3: t=1/3, 2/3). The true frame shows in
  the last field — i.e. the fresh frame is held back one field (latency
  `k−1` fields, 17 ms @ 30 fps, 33 ms @ 20 fps), exactly Salsa64's cost.
- **Missed intermediate** ⇒ repeat the previous field (do nothing extra:
  the loop already re-presents the last frame when `fresh == 0`), never
  jump to the final frame.
- **Scheduling state** lives in the display loop (`main.c`), fed by the
  matcher's per-frame interpolatable flag (§3.2). The matcher itself is
  frame-driven; the scheduler is field-driven.

## 5. Presentation — why the intermediate needs a second surface

### 5.1 The target is a working surface, not a display buffer

The HWR renderer draws into the **upscaled target** (native = 0) and the
**native VRAM surface** (native = 1), switching with `set_surface()`
(`ps3/src/hwr/hwr_rsx.c:362`, two switches per submit). The upscaled
target **mirrors VRAM**: "native VRAM and its depth buffer in local
memory: the RSX textures from it" (`hwr_rsx.c:275`), and `hwr_vram_written`
redraws rects into it (`:1195`). Game polygons blend against its current
pixels and sample textures from it.

Consequence: rendering the intermediate **into the same target destroys
the true frame**, and restoring it by re-rendering the true ops is **not
correct in general** — the re-render's semi-transparent polygons would
blend against intermediate pixels instead of the true background (frames
whose background persists from earlier frames are the common case), and
the error would accumulate frame to frame. A backup copy + restore is two
full-screen copies per game frame for no benefit over a second surface.

**The only correct approach is Salsa64's: a dedicated presentation
surface.** The intermediate is rendered there (starting from a copy of the
target, so parts the frame's geometry does not redraw stay identical),
and the final pass blits from it on intermediate fields. The main target
is never touched by invented frames — same rule as Salsa64 ("no copies to
RDRAM", here: no writes to the VRAM mirror).

### 5.2 Slice-2 design (not built)

- Allocate a second upscaled colour target + depth surface in RSX local
  memory at init (sized like the current target: 1024×2304 max, ~9.4 MB
  colour + depth; RSX local is 256 MB, affordable). A `set_surface()`
  variant binds it.
- Retain the frame's submitted op/vertex stream (§5.3) with per-vertex
  prev positions (computed at emit time by the §3.2 streaming lookup).
- On a repeated field with an interpolatable frame: copy the current
  target into the presentation surface, re-submit the retained stream with
  CPU-lerped positions (upscaled pass only — **never** the native pass:
  invented frames must not touch the VRAM mirror), then present from the
  presentation surface (`video_draw_rsx_texture` takes the surface offset
  for that field only).
- Depth: the intermediate render needs the depth surface cleared first
  (its per-triangle depths differ from the true frame's); the main depth
  is untouched. Texture state on re-submit must be verified against
  `draw_ops` (textures are bound per op at submit time — the re-submit
  replays the same ops, so the same bindings apply; if any texture state
  is emit-time-only, the re-submit must re-establish it — flagged as the
  top slice-2 implementation risk).
- If the intermediate cannot be submitted in time (ring full / RSX
  behind), the field is skipped (repeat previous field) and counted.

### 5.3 Retained op stream (slice 2 prerequisite)

`submit()` (`hwr_core.c:171-177`) consumes `ops` + `vb` per batch and
forgets them (`vb = NULL, nv = nops = 0`). Slice 2 retains a copy of each
batch's ops + vertices (+ per-vertex prev positions) into a frame record
(ring of 2), capped (e.g. 32768 vertices/frame; over cap ⇒ frame not
interpolated, logged — Salsa64's `full_task` rule). Slice 1 does **not**
build the frame record; it only captures vertex positions (§3.1).

## 6. Exclusions

- **Sprites**: `poly_is_sprite` / `do_rect` (`hwr_core.c:659-676, 900-901,
  1043`) — a sprite polygon is never lerped and never gets a prev stored.
  (Salsa64's #17 trail issue would be far worse on PS1, which is far more
  sprite-heavy; exclusion is the same answer.) Note the detection runs on
  integer positions before the sample shift and is per-triangle — a twisted
  quad whose halves disagree stays a polygon and is treated as geometry.
- **Lines / fills**: never lerped (1-px geometry; interpolation is
  pointless and risky). `cur_spr = 0` paths (`hwr_core.c:939,970`).
- **Framebuffer-texture tasks**: a frame in which any `vram_read` /
  `vram_copy` happened (`gpu_frontend_hooks.vram_read/vram_copy`,
  `hwr_rsx.c:1637-1639`; `full_vram_read` :1199, `full_vram_copy` :1255)
  is marked non-interpolatable (flag set in the hook, read at present).
  Redrawing it later would sample the target's later contents — the same
  reason Salsa64 excludes FB-texture tasks. This covers the Rollcage-class
  motion-blur feedback (framebuffer feedback ⇒ no interpolation for that
  frame, logged).
- **HUD quads made of triangles**: cannot be distinguished from world
  geometry by primitive type. Mitigation: a HUD quad that does not move
  has prev == cur, so the lerp is identity (harmless); a moving HUD quad
  lerps (minor artifact, same class as Salsa64's moving-HUD-numbers case).
  The flat/orthographic (`w == 1`) rule has no PS1 analog (no `w` in GP0
  vertices) — noted as a gap, not a blocker.
- **Scrolling textures** (water, waterfalls): not interpolated (the UVs
  advance at game rate, like Salsa64's prototype). Tile-offset matching is
  future work.

## 7. Setting + kill-gate

- New setting `interp`, **item 38** on the PGXP page, per-game, default
  OFF. Values: `0 Off / 1 On / 2 Test t=0 / 3 Test t=0.5`.
  - `On` = the real feature (slice 1: capture + pairing + logging only;
    slice 2 adds the intermediate emission; no visual change in slice 1).
  - `Test t=0 / t=0.5` = slice-1 validation mode: the frame is drawn with
    CPU-lerped positions at that fixed `t` (Salsa64's `interp-prueba`
    precedent). `t=0` shows the previous frame's geometry (pairing proof:
    coherent ⇒ pairing is right, garbage ⇒ pairing is wrong); `t=0.5`
    shows the mid-frame (coherence proof). These stay as debug modes after
    slice 2.
- Touch points (PGXP pattern, `config.c`/`menu.c`/`common.h`/`main.c`):
  `settings_t.interp` (`common.h`, after `pgxp_culling`); default
  `.interp = 0` (`config.c:17`); key `{ "interp", F(interp), 0, 3, 1 }`
  (`config.c:59-61`); menu item + help text (`menu.c:358-397`); PGXP page
  `{35,36,37,38}`, count 3→4 (`menu.c:410-421` — the
  `set_pages_cover_all` typedef fails the build if counts ≠ NSET);
  value display + change (`menu.c:512-514, 609-611`); `interp_set()` called
  at launch (`main.c:856-862`) and in `game_settings_changed()`
  (`main.c:695`), next to the `pgxp_*_set` calls.
- **Tunable cut + overlay (2026-11-07, items 39/40, Interpolation page).**
  `interp_disp` (item 39): per-vertex cut in native PS1 px, 60/120/240/480,
  default 120. `settings_t.interp_disp`; default `.interp_disp = 120`;
  key `{ "interp_disp", F(interp_disp), 60, 480, 1 }` (a hand-edited
  in-range value loads as-is; out-of-range file values are ignored, the
  same rule as every other setting); menu cycles the four doubling values;
  the matcher reads it via `interp_set_disp()` (clamped 60..480, half-cut
  derived), called from `game_settings_changed()` and at launch next to
  `interp_set()`, so it takes effect live from the in-game panel. Changing
  it never allocates or frees the matcher tables.
  `interp_overlay` (item 40): Off/On, default Off. `settings_t.interp_overlay`;
  default `.interp_overlay = 0`; key `{ "interp_overlay", F(interp_overlay), 0, 1, 1 }`;
  menu toggle `^= 1`. When On AND `interp` != Off, `interp_overlay()` in
  `main.c` draws the live counters in rows 48–146 (below the FPS band
  8–42, above the notice 660–700; three 30 px lines at y 50/82/114):
  `interp cut <px> lk <lookups> hit <hits> (addr <a> hash <h>) <%>` /
  `moved <n> hash miss <n> hash disp <n> cut <frames cut> drop <n>` /
  `hist 121-240 <n> 241-480 <n> 481-960 <n> >960 <n>` — the same cumulative
  counters as the log line (§3.5), so screen and log agree; both reset
  when `log_stats` prints (every 10 s), so the values are per log window,
  not since launch. Throttled to ~1 Hz plus a change check (same pattern
  as `fps_overlay`); it flushes its own rows, because it must also flush
  the one-time clear when the overlay or interp is turned off mid-game.
  Display only: it reads the matcher's extern counters, never writes
  matcher state (no overlay parameter exists in `interp_match.{h,c}`), so
  it cannot affect pairing. On the software renderer the matcher never
  runs and the overlay reads zeros. NSET is 41 (items 0–40); the
  Interpolation page holds `{ 38, 39, 40 }`, count 3.
- **Kill-gate** (copy the `pgxp_lite_on` pattern — call-site gating, not a
  single choke point): every producer/consumer checks `interp_on` first.
  OFF ⇒ no capture in `do_poly`, no table allocation, no lookup, no
  present-path logic, no extra submit. OFF behavior must be
  byte-identical — verified, not asserted (§11). The matcher tables are
  allocated lazily on first enable, like the corrector's atab.

## 8. Costs (estimates — hardware measurement is Fendi's side)

| | Estimate | Basis |
|---|---|---|
| PPE matching | ~1–3 ms / frame | Salsa64 0.8–3.5 ms/task on PS3; the fork's per-vertex hash work is the same shape, but the vertex count per PS1 frame is usually lower than Zelda's 7195 |
| RSX (slice 2) | scene ×2 @ 30 fps games | One extra upscaled-only render per game frame; the native pass is never repeated. At 1x likely fits; at 2x–3x the known RSX fill-rate bottleneck means skipped fields (the scheduler's repeat rule absorbs them as judder, not corruption) |
| Memory | ~2.0 MB matcher tables + slice-2 surface ~9.4 MB colour + depth | Tables §3.1; surface §5.2 (only allocated when the feature is first enabled) |
| Latency | +1 field (17 ms @ 30 fps) when On | The fresh frame is held back one field (§4); Salsa64 accepts the same cost, with an extrapolation (no-delay) mode as a later option |

## 9. Risks + open questions

1. **Address stability (§2.1)** — the bet everything rests on. Slice-1
   counters answer it; if the address hit rate is ~0 (like Driver 2's
   sub=0.1% under the old hybrid), the attr-hash fallback is the only
   identity, and the feature is likely not viable without a different
   identity source.
2. **DMA vs direct-write split** — what fraction of a game's geometry
   arrives by DMA (address known) vs direct-write (address lost)? Unknown
   per game; the counters report it (address-keyed vs hash-keyed lookups).
3. **RSX headroom at 2x–3x** — slice 2 doubles upscaled work. If the RSX
   cannot keep up, fields are skipped (judder). Slice 1 cannot answer this;
   slice 2's hardware test must measure it at 1x first.
4. **Texture state on re-submit (slice 2)** — `draw_ops` binds textures
   per op at submit time (assumed; verify against the code when building
   slice 2). If any texture state is emit-time-only, the re-submit must
   re-establish it.
5. **Depth lifecycle (slice 2)** — the intermediate render needs its own
   cleared depth; the `R.zc` counter / `set_surface` depth binding must be
   understood before slice 2 (read `draw_ops` :592-720 first).
6. **Atomic rules missing in slice 1** (§3.2) — turning characters may
   squash a limb for a frame in test mode. Acceptable for a test slice;
   Salsa64 needed three iterations (interp 6–8) to get these right.
7. **No camera estimation** — new geometry entering at screen edges has no
   prev and stays at its current position (Salsa64's planned-but-unbuilt
   extension). Same limitation, accepted.

## 10. Slice plan

### 10.1 Slice 1 (this session) — pairing validation

Capture + streaming matcher + test-mode lerp + counters + setting +
kill-gate. No frame record, no second surface, no intermediate emission.
Files: NEW `ps3/src/hwr/interp_match.{c,h}` (portable matcher);
`hwr_core.c` (capture/lookup/lerp hook in `do_poly`, frame swap at
`hwr_present`, FB flag, stats log); `common.h` / `config.c` / `menu.c` /
`main.c` (setting item 38 + `interp_set()` calls). Host test:
`~/workspace/ps3-interp-verify/interp_match_test.c`.

What slice 1 proves on hardware (Fendi's side, §11): the address-key hit
rate (is pairing real?), the visual coherence at t=0 / t=0.5 (is the
pairing right?), and that OFF is byte-identical. If pairing coverage is
~0, slice 2 is not built — the design says so now, so no sunk cost.

### 10.2 Slice 2 (next) — intermediate emission

Frame record (§5.3) + second presentation surface + depth (§5.2) +
scheduler (§4) + `On` gains the intermediate emission. Prerequisites:
slice-1 hardware evidence (address hit rate + t=0/t=0.5 coherence) and
the `draw_ops` texture/depth reads (risks 4–5).

### 10.3 Later (not planned)

Atomic rules (collapse/object/component), shader lerp (second attribute +
`lerp` uniform), extrapolation mode (no-delay, Salsa64's
"Por geometría, sin retraso": 27.0/29.7 dB vs 30.0/33.6 dB interpolated —
worse quality, better latency), camera estimation for entering geometry.

## 11. Verification plan

- **PPU compile**: every touched file exit 0 (`ppu-gcc` 7.2.0, env.sh
  sourced inline). Warning sets compared against the backup baseline.
- **Host tests** (`~/workspace/ps3-interp-verify/`): matcher unit tests —
  address keying + occurrence, attr-hash fallback, 120 px cut,
  per-polygon all-or-nothing, per-frame cut stats, table-full behavior,
  OFF behavior (matcher never consulted), lerp math (t=0 ⇒ prev, t=1 ⇒
  cur, t=0.5 ⇒ midpoint). The matcher file is portable (no PS3 includes)
  so the host test compiles it directly.
- **OFF check**: with the setting OFF, the `do_poly` hook is not taken;
  host test asserts no matcher calls; PPU compile warnings identical to
  baseline.
- **NOT verifiable here**: any hardware result. This VM cannot run the
  PS3. Hardware items for Fendi (slice 1):
  1. A 30 fps 3D game, `interp` On: log line — address/hash hit rate,
     moved %, frames cut %. (Is pairing real?)
  2. Same game, `Test t=0`: the picture should be the previous frame's
     geometry, coherent, one frame behind. (Is the pairing right?)
  3. Same game, `Test t=0.5`: the picture should be a coherent
     mid-frame, no tears, no streaks, sprites/HUD unmoved.
  4. `interp` Off after On: picture identical to a fresh boot with the
     feature never enabled (kill-gate proof).
  5. FPS with the feature On vs Off on the same scene (PPE cost of
     matching — slice 1 has no extra RSX work, so any drop is the matcher).

## 12. Tunable cut + overlay step (2026-11-07)

Why: the CTR hardware re-test showed the hash fallback works mechanically
but rescues ~0% in gameplay, with moved ~79–84% of lookups. The open
question is whether the rejected candidates are the same vertex moving
fast (loosening the cut helps) or a wrong vertex at a recycled address /
wrong occurrence (loosening hurts). This step gives Fendi the knob and the
readout to answer it on hardware: `interp_disp` (120 → 240 → 480 on the
same CTR session), the live overlay, and the hash-split + histogram log
fields (§3.5, §7).

What it proves on hardware (Fendi's side):
1. Same CTR session, `interp` On, overlay On: read the overlay live while
   tuning the cut 120 → 240 → 480 — hit/moved per value, same scene.
   If hits climb at 240/480, the rejections were fast-moving same
   vertices; if moved stays ~80% and the histogram spreads to >960, the
   candidates are wrong vertices.
2. Read the histogram from the log line (`hwr: interp: …; hist …`): the
   verdict field — clustering at 121–240 means loosen to 240; spreading
   to >960 means loosening pairs wrong vertices.
3. Overlay on/off check: on shows the three lines in rows 48–146 (below
   the FPS readout, no overlap); off clears them with no stale text; the
   FPS overlay and the renderer notice are undisturbed.
4. `interp` Off after On: picture identical to a fresh boot with the
   feature never enabled (kill-gate proof, as before).

What it does NOT prove: host tests prove the logic (cut boundaries,
histogram bucketing, split counters, overlay isolation by construction —
the matcher has no overlay input). Only Fendi's hardware session can show
whether cut 240 pairs CTR, whether the overlay reads correctly on his
screen, and whether the ~1 Hz refresh is smooth enough. The overlay's
numbers are per 10 s log window (§3.5), so a "hit %" read mid-window is
a partial window, not a session total.

Verification (this step, `~/workspace/ps3-interp-verify/`):
- Matcher host test: 170 checks, 0 failures (92 existing + 78 new:
  cut boundaries 121/240/480/60, half-cut follows the cut, cut survives
  off/on, hash_miss vs hash_displaced split, histogram buckets incl.
  smallest-displacement rule, determinism of the counter deltas).
  gcc -O2 -Wall -Wextra, zero warnings.
- Config host test (real `config.c`): 23 checks, 0 failures — defaults
  120/Off, all four cut values + both overlay values round-trip,
  hand-edited in-range value loads, out-of-range file values ignored,
  per-game override + restore, per-game and global save write both keys.
- PPU compile (`compile.sh`, BASE re-pointed to the overlay backup):
  every touched file + all 13 `common.h` includers exit 0 with warnings
  identical to the backup; `spu_mdec.c` fails identically in both trees
  (pre-existing missing generated header, untouched). No strays
  (.o/.d/revision.h: none).
