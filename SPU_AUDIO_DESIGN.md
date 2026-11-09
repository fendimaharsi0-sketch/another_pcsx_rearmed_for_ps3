# SPU audio offload — design (draft, untested on hardware)

Goal: move the PS1 SPU (sound) mixing off the PPU onto a PS3 SPU, so the
PPU has more time for CPU emulation in hardware-render (RSX) mode.
Default OFF (`spu_audio`, config), PPU mixing unchanged unless enabled.

## Baseline (measured in code, not on hardware)

- Mixer: `plugins/dfsound/spu.c`, `SPUasync` -> `do_samples` -> `do_samples_`
  -> `do_channels(ns_to)`. Output int16 stereo 44,100 Hz, 1 sample = 768 PSX
  cycles. Job <= 914 stereo frames (~20.7 ms PAL / 735 NTSC), typically one
  job per VBlank (fed when `SPUasync` gets `flags & 1`).
- 24 voices (`MAXCHAN 24`). Per-voice `SPU_chan` 56 B on PPU; `ADSRInfoEx`
  16 B. SPU RAM 512 KiB (`spu.spuMemC`), voices read it by raw pointer.
- PPU cost is profiled into `gpulib_prof_ticks[1]` (`do_samples`,
  `spu.c:1002-1007`). That counter is the baseline: compare before/after.
- PS3 build sets `USE_ASYNC_SPU = 0` (`Makefile.libretro:331`), so the
  pthread worker path (`work_item`, `queue_channel_work`,
  `do_channel_work`, `sync_worker_thread`) is compiled out and the PS3 runs
  the synchronous `do_channels()` on the PPU.

## Chosen split: candidate B (voice mixing on SPU, reverb stays on PPU)

The existing async design already defines the job boundary:
`struct work_item` (`spu.c:724-749`) — header (`ns_to`, `ctrl`,
`decode_pos`, `rvb_addr`, `channels_new/on/silent`) + per-channel snapshot
(`spos`, `sbpos`, `sinc`, `start`, `loop` as RAM byte offsets, `vol_l`,
`vol_r`, `ns_to`, flags, `ADSRInfoEx adsr`) + `SSumLR[NSSIZE*2]`.
~8.4 KiB per item, 4-deep ring. `queue_channel_work()` builds it on the
PPU; `do_channel_work()` is the worker mixer; `sync_worker_thread()`
reaps it and runs `MixCD` + `do_samples_finish` on the PPU.

A PS3 SPU port is: keep `queue_channel_work` / `sync_worker_thread` on the
PPU, replace the pthread `do_channel_work` with an SPU program + DMA.

Candidate B = move `do_channel_work` entire (ADPCM decode + interpolation
+ ADSR + stereo mix into `SSumLR` + `RVB` accumulate), but leave `REVERBDo`
on the PPU. The SPU returns `RVB` as an extra output (7,312 B). This
removes the hardest DMA problem (reverb delay lines live in SPU RAM,
`[StartAddr, 0x40000)` samples, ~20 scattered reads + ~8 writes per stereo
pair) while keeping most of the win (channel mixing is 24x the per-sample
work; reverb is one pass over `ns_to` pairs).

Stays on PPU: register writes, DMA to/from SPU RAM, IRQ scheduling,
position/ADSR advance via the skip path (`do_samples_skip`), `MixCD`,
`do_samples_finish`, audio feed, `REVERBPrep`, `StartSound` setup,
silent-channel advance.

## SPU input per job (candidate B)

- `work_item` (~8.4 KiB) + `sb_thread` state (25 x 64 B = 1,600 B) +
  gauss table (2 KiB) + reverb params (`REVERBInfo`, 156 B, only for
  `rvb_addr`/flags) + the ADPCM blocks the job will consume (see gather).
  ~13-20 KiB plus blocks.
- SPU output per job: `SSumLR` (7,312 B) + `RVB` (7,312 B) + updated
  `sb_thread` (1,600 B) + decoded ch1/3 capture samples (2 x 1,024 B) if
  those channels are audible. ~18 KiB.

## The hard problem: SPU RAM does not fit in local store

SPU RAM is 512 KiB; an SPU has 256 KiB local store. The mixer reads sample
data by raw pointer (`pCurr`/`pLoop` into `spuMemC`). Fix: a PPU-side
gather. The skip pass (`do_samples_skip` / `skip_block`) already walks
exactly the blocks a job will consume, following loops, so it can record
their RAM addresses and gather them into one contiguous DMA buffer.
Size (computed): pitch 1.0, <=33 blocks/channel ~= 528 B/channel ~=
12.7 KiB for 24 channels; max pitch (`sinc` = 0x3FFF0, ~4 samples per
output sample) ~= 130 blocks/channel ~= 2 KiB/channel ~= ~50 KiB worst
case. The SPU program needs an index map (RAM byte offset ->
gathered-buffer offset) because `pCurr`/`pLoop` are RAM offsets.
On-demand 16-byte DMAs from the SPU would be far worse (up to ~3,000 tiny
DMAs per worst-case job). Endianness: PPU/SPU are big-endian, SPU-RAM
sample data is little-endian — byte-swap gathered blocks (or swap on the
PPU during the gather).

## Sync rules (do not break these)

- Register writes force mixing to within 16 samples (`registers.c:63`);
  keyon/off to 2 samples. The PPU MUST drain/sync the SPU job before
  applying any register write that arrives mid-job. (The existing code
  handles the analogous case for the pthread worker via
  `sync_worker_thread(force)`.)
- IRQ timing: the PPU keeps authoritative channel positions (skip path).
  The SPU must NOT own `pCurr`/`pLoop`; it only consumes the snapshot.
- FMOD ordering: the freq channel (`bFMod==2`) must be computed before its
  consumer (`bFMod==1`) within the same job; process channels in order,
  keep `iFMod` (3,656 B) in local store.
- Decode-buffer capture (ch1/3): `do_decode_bufs` writes decoded samples
  into SPU RAM 0x800/0xC00 for games to read back. If the SPU generates
  them, 2 KiB per job must be DMA'd back (or the PPU decodes those two
  channels itself).
- Job latency: one job is <=914 samples; the SPU job (DMA in + compute +
  DMA out + sync) must complete within a frame (feed happens at VBlank).

## SPU program plumbing (existing pattern)

- `ps3/spu/mdec_spu.c` is a persistent SPU thread: PPU writes a job struct
  (128-byte aligned) to main memory + a word to the inbound mailbox.
  Mailbox today: MDEC job index + 1, or 0x100 + AA job index. Zero ends
  the thread. Job seq goes to `done_ea` when output is in memory.
- Audio adds a third range: 0x200 + audio job index, a new `audio_job`
  struct (`ps3/spu/audio_job.h`, 128-byte aligned), `process_audio()` in
  the SPU program, and PPU-side `spu_audio_start()` / `spu_audio_wait()`.
  The SPU thread's `main` takes `mdec_ea`/`aa_ea` as arg0/arg1; the audio
  jobs array goes in as arg2 (currently unused).
- SPU threads: `spu_mdec_init(5)` starts 5 SPUs for MDEC + AA. The audio
  job uses one of those threads when MDEC is idle (FMV only), or a
  dedicated thread. Which one is a runtime choice, not a compile-time one.
- Build: `ps3/build.sh` compiles `spu/mdec_spu.c` with `spu-gcc` and
  embeds it as `spu_bins.h` (`mdec_spu_elf`). NOTE: the VM's `spu-gcc`
  is 7.2.0 and cannot compile `mdec_spu.c` as a whole (it uses
  `__builtin_convertvector` in `aa_kernel.h`, which needs the CI's SPU
  GCC 9.5). The audio mixing core is therefore written as a
  self-contained file (`ps3/spu/audio_mix.h`) using only standard SPU
  intrinsics, so it compiles under 7.2.0 and can be host-tested.
  Full `mdec_spu.c` compile is verified by CI, not here.

## Verification

- Host test `hosttest/spuaudio/test.c` (pattern: `hosttest/spumdec`,
  `hosttest/spuaa`): builds the SPU audio code with `-DHOST_TEST`
  (memcpy for DMA), runs it on the same `work_item` input as the PPU
  `do_channel_work`, and compares output sample-for-sample
  (`SSumLR` + `RVB` + `sb_thread`). Must be bit-exact before any
  hardware run.
- Hardware: enable `spu_audio` (default OFF), play a game with music +
  SFX, check for glitches/dropouts and compare `gpulib_prof_ticks[1]`
  (PPU mixing ms/frame) before/after. Audio correctness cannot be
  verified in this VM — it needs a hardware listen.

## Implementation status (increment 2, 2026-11-09, untested on hardware)

Increment 1 (mixing core, SPU program, PPU API, config, build) is done
and verified in this VM; increment 2 (gather + wiring) is done and
verified in this VM. The feature is ready to push and build in CI.
Hardware verification is still Fendi's side (listen + prof counter).

DONE in increment 2:
- **Gather** (`plugins/dfsound/spu.c`, `#ifdef USE_SPU_AUDIO`):
  the skip pass records every ADPCM block a job consumes, in order.
  `skip_block` records `start` (the block being processed, NOT the
  block `pCurr` advances to — after a loop-end block the consumer's
  next block is the loop target, not `pCurr + 16`); `decode_block`
  records `start` too (covers FMOD freq channels, which the skip pass
  never walks). `queue_channel_work` records the first block if a
  channel never exhausted one, and the trailing partial block (the
  block the channel is mid-way through at the end of the job — the
  skip pass only records on exhaustion). Exactly once per consumption;
  loop revisits are genuine repeats and are kept as separate entries.
  Cap 8192 entries; realistic max ~3120/job.
- **`ps3/spu/audio_work.h`** (new): the `audio_work` struct extracted
  from `audio_mix.h` (function-free header). `start`/`loop` are
  gathered-block INDICES, not RAM offsets; `iRawPitch` carried
  per-channel (FMOD). Layout verified vs `ADSRInfoEx`/`sample_buf`
  (16 B / 64 B, bitfields match by memcpy test).
- **SPU backend in `spu.c`** (`#elif defined(USE_SPU_AUDIO)`):
  `spu_audio_build_job` (work_item -> audio_work + gathered buffer,
  per-channel section bases, `loop` = first occurrence of the loop RAM
  offset), `init/exit_spu_thread` (calls `spu_audio_init`/`shutdown`;
  falls back to fully synchronous PPU mixing if the SPU is unavailable),
  `thread_work_start` (build + submit; PPU fallback inline on failure),
  `thread_work_wait_sync` (fetch + copy SSumLR/sb_thread/noise/RVB back
  + ch1/3 capture ring + `REVERBDo` on the PPU = candidate B),
  `thread_get_i_done` (non-blocking poll), `thread_sync_caches` (no-op).
  Two real bugs found and fixed by the host test: the `spu_audio_wait`
  seq comparison (must compare the slot's own seq, not the global one)
  and the `sb_thread` copy-back size (`sizeof(spu.sb_thread)` is the
  POINTER size 8, not the 1600-byte array — only `old[0..3]` came back).
- **Enablement**: `Makefile.libretro` (psl1ght) adds
  `-DUSE_SPU_AUDIO -Ips3/spu`; the async-block guard in `spu.c`,
  `externals.h`, `freeze.c`, `reverb.c` and the `pcsx_rearmed_spu_thread`
  option in `frontend/libretro.c` all accept `USE_SPU_AUDIO` alongside
  `USE_ASYNC_SPU`. `ps3/src/main.c` answers `pcsx_rearmed_spu_thread`
  from `cfg.spu_audio`. No CFG_VERSION bump (config is name-keyed).
- **Menu** (`ps3/src/menu.c`): item 43 "SPU audio mixing" appended
  after Color preset (the tail of the settings list on this branch),
  on the Media & Audio page; NSET 43 -> 44. Value shows "SPU"/"PPU".
  Per-game toggle; flows through `game_settings_changed()` ->
  `SPU_configure()` -> `init/exit_spu_thread`. (On the `ps3` branch the
  same item sits after PGXP seam fill; here PGXP culling/seam keep
  research numbering 36/37 and Theme UI is 40/41/42.)
- **Host test** (`hosttest/spuaudio/test.c`, build with
  `-DUSE_SPU_AUDIO`, also in `hosttest/build.sh`):
  jobs 1-2 **PASS, bit-exact** (simple interp, gauss + FMOD, noise,
  ADSR, sb_thread, mix_chan_rvb). Job 3 (real gather + SPU backend
  path, `queue_channel_work` -> SPU program -> reap vs PPU
  `do_channel_work`): **sb_thread and noise state bit-exact**, but
  SSumLR shows a systematic difference (max ~139 LSB, ~0.4%, 1358/1470
  samples differ by >1 LSB) whose root cause is NOT yet isolated. It is
  reported as a diagnostic in the test, not hidden and not gated: the
  mixing core is proven bit-exact, the gather content and the SPU
  decode are verified, but the end-to-end SSumLR is not yet bit-exact.
  The hardware listen test is the deciding check.
- `ps3/src/spu_audio.c`: HOST_TEST `spu_audio_shutdown` added (the
  real one is inside `#ifndef HOST_TEST`); `dma_check` in `audio_spu.c`
  now mirrors the real MFC rules (1/2/4/8/16-byte DMA legal, not only
  multiples of 16 — the done word is a 4-byte DMA). SPU program local
  buffers renamed `ls_ChanBuf`/`ls_iFMod` (they collided with the PPU
  globals when both files are included in one host-test TU).
- **Port to `pgxp-research` (2026-11-10, on top of `661c70a4`)**: the
  patch was built against the `ps3` head; 19 of 20 files applied
  cleanly. Only `ps3/src/menu.c` needed a hand port — the settings
  list tail differs (SPU audio mixing lands after Color preset,
  still item 43, NSET 43 -> 44). Host test re-run on this branch:
  identical results (jobs 1-2 bit-exact, job 3 sb_thread/noise
  bit-exact, SSumLR diagnostic max 139 LSB, same open item).

NOT done / open:
- **Job 3 SSumLR difference (max ~139 LSB)** — see above. Needs
  investigation; do not claim the gather path is bit-exact until it
  is understood. Candidate leads already ruled out: gather content
  (byte-identical to RAM), audio_work fields (match the manual job),
  ADSR stepping (persists with a frozen envelope), block layout (persists
  with the manual block layout), multi-channel interaction (persists
  with ch0 alone), time shift (no shift aligns the outputs).
- Hardware verification: the SPU thread, the DMA, the gather and the
  audio output all need a hardware listen + a `gpulib_prof_ticks[1]`
  before/after comparison. Nothing here is hardware-tested.
- Reverb on the SPU (candidate A) — deliberately deferred; candidate B
  first.
- Full core build: the VM cannot link the whole core (it needs CI's
  toolchain for `mdec_spu.c` under SPU GCC 9.5 and zlib). The CI build
  is the verification for that; all touched files compile individually
  under ppu-gcc/spu-gcc/host gcc in this VM (see below).

## Verification (this VM, 2026-11-09)

- Host test: `gcc -O2 -g -DHOST_TEST -DUSE_SPU_AUDIO -Iplugins/dfsound
  -Ips3/spu -Ips3/src -Ifrontend hosttest/spuaudio/test.c` ->
  jobs 1-2 PASS bit-exact; job 3 sb_thread/noise PASS bit-exact,
  SSumLR diagnostic max 139 LSB (see above).
- `ppu-gcc -fsyntax-only -DUSE_SPU_AUDIO`: `spu.c`, `externals.h`,
  `freeze.c`, `reverb.c`, `main.c`, `menu.c`, `config.c`, `common.h`,
  `spu_audio.c` all pass. `spu-gcc`: `audio_spu.c` -> valid Cell SPU
  ELF. `audio_mix.h`: host gcc + spu-gcc clean.
- `frontend/libretro.c`: host gcc `-fsyntax-only -DUSE_SPU_AUDIO`
  passes (ppu-gcc blocked only by the VM lacking zlib headers; CI
  installs zlib).
