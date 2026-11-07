# Mode-2 texture fix: w from the RAM shadow (mode2w)

Target (Fendi 2026-11-07): fix lite mode 2's broken textures. Mode 1 is
accepted as good (close to DuckStation minimal PGXP). Base tree:
pgxp-research at 536835fb content (frame interpolation PARKED — its 11
files reverted out of this tree; interp code stays in git history at
8e7fa21b). Backup of the pre-park tree: ps3-interp-work-bak-20261107-mode2w
(== 8e7fa21b).

## Ground truth (verified in code, 2026-11-07)

1. Shader mechanism is SOUND — no shader change needed for the mechanism.
   `ps3/src/shaders/hwr.vcg`: `oPos = float4(...) * w` with
   `w = pg.z * (1/32767)` (upscaled pass only). The clip-w trick makes the
   RSX interpolate UVs with true perspective. Given a correct, consistent
   per-vertex w, the texture is straight. The warp is a DATA problem (wrong
   w per vertex), not a shader problem.
2. The (x,y,w) unit ALREADY exists in the corrector.
   - `libpcsxcore/gte.c` gteRTPS/gteRTPT call `pgxp_corr_on_rtps` /
     `pgxp_corr_on_rtpt` with (x, y, invw = 1/SZ3) from the SAME GTE op.
     RTPS: new vertex = SXY2 + SZ3. RTPT: per-vertex v, SXY reg 12+v + its
     own sz3. Mapping SXY<->SZ is already resolved in code (see the comment
     above `pgxp_corr_cand_at` in pgxp_lite.c: shadow invw = 1/SZ3, lite z =
     raw depth / 4096 — the same number, up to SZ3 truncation).
   - RAM shadow entry `pgxp_corr_entry_t { short sx, sy; int valid;
     unsigned frame; float x, y, invw; }` — x, y, invw ride together,
     keyed by RAM address + exact-word validation + Build A guard (1.5px).
   - `pgxp_corr_cand_at(ptr, word, c)` returns a full candidate
     (dx, dy, z = 1/invw = SZ3).
3. Current priority in `ps3/src/hwr/hwr_core.c` do_poly (~lines 744-762):
   lite candidate first (`pgxp_lite_cand_at`); corrector candidate ONLY
   where lite missed (`!nc[i] && pgxp_corr_track`). Lite HIT always wins —
   including for z. The corrector's z is never used on a lite HIT.
4. Depth safety nets already exist and stay: `pgxp_depths_fit` (fit test,
   hwr_core.c:816,861) + `pgxp_depths` odd-corner repair (hwr_core.c:872,
   `ps3/src/hwr/pgxp_depth.h`). Note the fit test is loose for triangles
   (zmax <= 2*zmin), so a wrong-but-plausible depth passes it — it cannot
   be the fix by itself.
5. Quantization is NOT the suspect: `p[i].ow = zz[i] * iz` (hwr_core.c:879)
   then `q[3*i+2] = ow * 32767` — relative depth, ~15 bits. Plenty.

## Root-cause hypothesis (hardware-testable, not proven)

Lite's per-vertex z can belong to a DIFFERENT vertex: the word table is
keyed by the 32-bit SXY word (screen position only), and two different 3D
points can project to the same screen word. The two-depths pick can choose
the wrong depth for a vertex; the fit test (2x tolerance) lets it through.
Result: inconsistent depths inside one triangle -> warped texture.
The shadow's z is address-keyed (the packet's RAM address = that vertex)
and comes from the same GTE op as its position, so it cannot be another
vertex's depth.

## Change (targeted, mode 1 untouched)

In do_poly's candidate selection (hwr_core.c ~744-762), AFTER the candidate
is picked:
- Keep lite's dx/dy (positions unchanged; mode 1 does not use z at all).
- For z: if `pgxp_corr_track` and the shadow has a validated entry for this
  vertex, use the shadow's z (from `pgxp_corr_cand_at`) INSTEAD of lite's z.
  If the shadow misses, keep lite's z (no behavior change).
- Keep the fit test + odd-corner repair as safety nets (they already run
  on zz[] after selection).
- Constraint: do NOT double-count the corrector log counters (lookups/hits)
  — restructure so the shadow is consulted once per vertex, or add a
  z-only accessor that does not bump counters. Keep the Build A guard
  (inside pgxp_corr_cand_at) as the validity gate.
- Constraint: gate the whole thing on the corrector being ON (pgxp_corr),
  same as the existing supplement. When the corrector is OFF, mode 2 is
  byte-identical to today. When lite is OFF (mode 0), nothing changes.
- No new settings. No change to the Interpolation page (parked, gone).

## Priority — OURS WINS (Fendi 2026-11-07, correcting the cross-check idea)

Fendi's correction: our mode-2 texture decision must WIN over lite mode 2,
which is PROVEN wrong (hardware: mode 2 warps). The earlier idea (cross-check
both sources; on disagreement fall back to affine) is WRONG — it would let
lite's proven-wrong value veto our good one.
- For z in mode 2: the shadow's z WINS wherever the shadow has a validated
  entry (exact-word + Build A guard). Lite's z is used ONLY where the shadow
  misses. Positions (dx,dy) stay lite's — unchanged.
- The fit test + odd-corner repair stay as gates on the final zz[] (they
  already run after selection). They are the fallback for "no trustworthy
  depth", not a veto between two sources.
- Optional diagnostic (log-only, no veto): count shadow-vs-lite z
  disagreements, so the hardware log shows how often ours overrode lite's.
  It must not change behavior.
- Why ours is the trustworthy source: lite's z is word-keyed (screen
  position only) and proven wrong on hardware; the shadow's z is
  address-keyed (that packet's vertex) and from the same GTE op as its
  position. On disagreement the trustworthy source wins — there is no
  reason to justify a decision that is clearly visibly wrong.

## Verification (coordinator's job, I verify after)

- Host test: lite HIT + shadow HIT with different z -> z from shadow;
  shadow MISS -> lite z; mode 1 (pgxp=1) output identical to baseline;
  corrector OFF -> mode 2 identical to baseline. Extend the existing
  pgxp_corr_test.c in ~/workspace/pgxplite-verify/ (57 checks) — do not
  invent a new harness.
- PPU compile all touched files + the 13 common.h includers, exit 0,
  warnings identical to baseline modulo line numbers.
- Orphan audit: no new unused symbols.
- Diff vs backup (ps3-interp-work-bak-20261107-mode2w, == 8e7fa21b):
  the ONLY differences must be the mode-2w change + the interp park
  (the 11 reverted files). Nothing else.

## Decisive test (Fendi, hardware)

Mode 2 on the same scene that warps today. Verdict is visual: textures
straight (fix works) vs still warped (hypothesis wrong — then the warp
source is elsewhere, e.g. lite's address-table staleness, and we stop
instead of guessing further).

## Does NOT fix

- DMA / CPU-ALU vertices: shadow misses there -> lite z stays, warp
  possible in those polygons. Silent, same as today.
- Mode 2's other risk (the fit test is loose) stays as is.

## Mode 2b — FUTURE, only if needed (Fendi 2026-11-07)

If mode-2w (shadow z wins) proves insufficient on hardware, a mode 2b:
ALL texture decisions come from us (shadow/corrector), not lite — not
just the depth, the whole texture path. Contingent on the mode-2w hardware
verdict. NOT built now; do not start it until Fendi says so after testing
mode-2w.
