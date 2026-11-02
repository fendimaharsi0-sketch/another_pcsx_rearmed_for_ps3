# UNOFFICIAL PCSX-ReARMed Port for PS3

A PlayStation 1 emulator for the PlayStation 3. Version 0.1 (build 72).

It is the [PCSX-ReARMed](https://github.com/libretro/pcsx_rearmed) libretro core with
its own PSL1GHT front end, built around the PS3's hardware:

- **Lightrec dynarec** tuned for the Cell PPU (no microcoded instructions in the
  generated code).
- **RSX renderer**: 3D at 2x, 3x, 4x or 4.5x resolution, with the PS1's VRAM kept
  on the GPU.
- **SPU jobs**: FMV decoding and edge smoothing run on the Cell's SPUs.
- **PS1 discs** play straight from the console's drive (ONLY ON CFW!!!).

<a href="https://youtu.be/rMDbHit3Br4">
  <img src="https://img.youtube.com/vi/rMDbHit3Br4/maxresdefault.jpg" alt="Watch gameplay" width="900">
</a>

## Notes from the author:

**FULL DISCLOSURE: THIS WAS MADE BY AI 99%. I SPENT TIME DEBUGGING AND TESTING, WHICH I AM GOOD AT. VIBE CODED YOU MIGHT CALL IT, BUT FELT MORE LIKE A FRUSTRATING GRIND TO MAKE IT WORK THE WAY IT SHOULD...**

**EITHER WAYS. MOST FEATURES SHOULD WORK , BUT THERE MAY STILL BE BUGS , FREEZES, SLOWDOWNS AND OTHER ISSUES.  I DO NOT PLAN ON MAINTAINING THIS PROJECT. IT WAS MADE FOR FUN TO TEST AI CAPABILITIES.**

**IT MAY RUN A LOT OF GAMES OR NOT. I DON'T KNOW, YOU WILL HAVE TO FIND OUT FOR YOURSELF. I DO NOT TAKE REQUESTS AND I WILL NOT BE DOING MORE IMPROVEMENTS. SOURCE CODE IS HERE SO YOU ARE FREE TO DO IT YOURSELF.**

## More technical notes:

- **I DID NOT TEST 60 HZ ON PAL GAMES.**
- **BE CAREFUL WITH VIDEO SKIPPING FEATURE, IT MAY SKIP FMVS IN CUTSCENS TOO**
- **2 THREADS DOES NOT ALWAYS MEAN GAME WILL RUN BETTER. SOME GAMES MAY RUN SLOWER OR BREAK COMPLETELY**
- **SOME PS1 GAMES HAVE FPS DIPS ORIGINALLY, IT IS NOT ALWAYS BECAUSE OF THE EMULATION**
- **SOME FREEZES AND SLOWDOWNS MAY HAPPEN DUE TO AGING HDD OR BAD LASER/DISC (IF YOU ARE PLAYING FROM A DISC)**

## Visual summary

![Features](https://raw.githubusercontent.com/Calyps0man/pcsx_rearmed_for_ps3/ps3/pcsx-rearmed-ps3-features.gif)



## Contents

- [Features](#features)
- [Requirements](#requirements)
- [Installing](#installing)
- [Games and BIOS](#games-and-bios)
- [Playing a PS1 disc](#playing-a-ps1-disc)
- [Controls](#controls)
- [Settings](#settings)
- [Per-game settings](#per-game-settings)
- [Performance tips](#performance-tips)
- [Files](#files)
- [Reporting a problem](#reporting-a-problem)
- [How it works](#how-it-works)
- [Building](#building)
- [Tests](#tests)
- [Credits and licence](#credits-and-licence)

## Features

- Disc images (.cue/.bin, .chd, .pbp, .iso, .m3u) from the HDD or USB, and PS1 discs
  in the console's drive, listed with the game's name and a disc icon.
- Copy disc to HDD: copies a PS1 disc to a .bin/.cue named after the game, so it
  loads without drive pauses.
- RSX hardware renderer at up to 4.5x (4096x2304). It draws everything the PS1 GPU
  does, including semi-transparency, texture windows, the mask bit and dithering.
- High-resolution menus (480-line pictures) are drawn only as large as the TV can
  show, so they stay fast at 4x.
- Picture filters: sharp, bilinear, FXAA on the RSX, or edge smoothing on the SPUs.
- 10 save-state slots per game, with the time each was saved.
- Per-game settings, kept in their own file.
- Skip intro videos: presses Start/X during a game's opening videos, and
  fast-forwards the ones that can't be skipped.
- Sound that slows down with the game instead of crackling (or keeps its pitch).
- PS1 CPU overclock (+50% / +75%) for games that slowed down on a real PS1.
- Debug tools: VRAM check against the software renderer, numbered VRAM dumps with a
  GP0 trace, and hang reports.

## Requirements

- A PS3 on **custom firmware** (tested on a CECHA00) or HEN.
- Optional: a PS1 BIOS (see below). Without one, the built-in HLE BIOS is used.

> [!WARNING]
> **HEN CANNOT PLAY FROM DISCS AT THE MOMENT**

## Installing

Install `pcsx-rearmed-ps3-full64.pkg` with **Package Manager → Install Package
Files**. The app appears as **PCSX-ReARMed for PS3** (title ID `PCSR00001`); its folder
is `/dev_hdd0/game/PCSR00001/USRDIR`.

Installing a new version over an old one keeps your saves, states and settings.

## Games and BIOS

Put games in any of these folders:

| Where | Folders |
|---|---|
| App folder | `/dev_hdd0/game/PCSR00001/USRDIR/roms` |
| HDD | `/dev_hdd0/PSXISO`, `/dev_hdd0/ROMS/PSX`, `/dev_hdd0/ROMS/PS1`, `/dev_hdd0/roms/psx` |
| USB | `/dev_usb000/` or `/dev_usb001/` + `PSXISO`, `ROMS/PSX` or `roms/psx` |

Formats: `.cue`/`.bin`, `.chd`, `.pbp`, `.iso`, `.m3u` (multi-disc games).

A BIOS improves compatibility. Put `scph5501.bin` (or `scph5500`, `scph5502`,
`scph101`, `scph7001`, `scph1001`, `psxonpsp660`) in `USRDIR/system`. Any other
512 KB BIOS file there is used too. With a BIOS, **PS1 logo at start** in the
settings shows the PlayStation logo when a game starts.

## Playing a PS1 disc 
> [!WARNING]
> **CFW ONLY!!! DOES NOT WORK ON HEN**

Insert the disc. The first entry of the game list is the drive. It is checked when
the list opens (Square checks again), and it shows the game's name with a black
disc icon and "from disc" at the right. Press X to play.

Reading PS1 discs needs raw CD access, which retail firmware blocks. On CFW the
emulator unlocks it for the session with temporary LV1/LV2 patches (the same
method as IRISMAN), then restores the original values. On HEN this is not
possible (HEN gives no LV1 access): playing from the disc and copying it need CFW,
and the app says so. On HEN, copy the disc to an image on a PC instead.

- Every patch site is checked before anything is written. Unknown kernels are
  left alone.
- The patches are applied only for CD-sized discs.
- Supported kernel layouts: retail 4.75 and later, and DEX 4.81 and later.

Game names come from a list of 10,306 PS1 serials (`ps3/data/ps1titles.txt`).

### Cover art in the game list

The list is wide on the left, with a square cover panel on the right for the
selected game (title without the file extension, format, "own settings" when
the game has its own settings). No cover art is bundled in this package: the
panel reads the cover from the user's own webMAN MOD cover pack, which most
modded PS3s already have, so the package does not grow. When no cover is
found the panel shows "No cover found" and the list works exactly as before.

Lookup, in order: (1) a cover named after the game file itself, next to
it (`Game.cue` → `Game.JPG`/`Game.PNG`) — a user's own custom cover, so
it wins over the generic pack. (2) The pack by serial —
`SLUS_123.45_COV.JPG` or `.PNG` in
`/dev_hdd0/game/BLES80608/USRDIR/covers_retro/psx/` (USB000/USB001 in the
same layout are tried too); the disc's serial comes from its `SYSTEM.CNF`,
a file's serial from `[SLUS-12345]` or `SLUS_123.45` in its file name.
(3) webMAN MOD's own game list, `/dev_hdd0/xmlhost/game_plugin/mygames.xml`:
the entry whose mount path matches the game (extension stripped, so a
list entry `Game.cue` matches webMAN's `Game.bin`, `+`/`%XX` decoded,
case-insensitive) contributes its icon path directly — that is how files
whose names carry no serial still get their pack cover, because webMAN's
scanner already found the serial. Generic webMAN icons and non-local
paths are skipped, and a missing or stale list (a game added after
webMAN's last Refresh XML) simply skips this layer. A playlist (`.m3u`)
entry looks up its playlist name, not the discs inside it. Reading the
serial out of the disc image itself is still not in this version: for
webMAN users the XML already covers it, and it would only help users
without webMAN. Covers are small by design (the pack's are 268x268 or
smaller): only the selected game's cover is kept, files over 512 KB and
images over 512x512 are refused, and the cover is freed when the list is
left.

### Copy disc to HDD

The console's drive pauses the game whenever it has to seek, for example when a
game loads from another part of the disc. To avoid that, select the disc entry
and press **Circle (copy to HDD)**:

- The whole disc is copied to `/dev_hdd0/PSXISO/<game name>.bin` and `.cue`,
  named after the game, and appears in the game list as a disc image.
- A progress bar shows the time left. Press Circle twice to stop; an unfinished copy
  is removed.
- The copy needs up to 700 MB of free space, which is checked first. A disc that
  is already on the HDD isn't copied again.
- Subchannel data isn't copied, as when playing the disc itself, so
  LibCrypt-protected PAL games need an `.sbi` file next to the copy.

## Controls

| Where | Buttons |
|---|---|
| Game list | X play · Triangle settings · Square check the drive again · L1/R1 page · Circle copy the disc to the HDD (disc entry) |
| In game | **Select + Start** opens the menu |
| In game | **L3 + R3** toggles the DualShock's analog mode |

Footer prompts (game list, settings, import, pause) show X / Circle / Triangle / Square as monochrome glyphs drawn procedurally at run time — no button image is bundled. D-pad, shoulder and combo names (Up/Down, L1/R1, SELECT+START) stay text.

The in-game menu has these entries:

- Resume
- Save state / Load state (Left/Right picks one of 10 slots)
- Reset
- Settings
- Own settings for this game
- Quit to game list
- Dump VRAM (debug)

## Settings

| Setting | Values (default first) | Notes |
|---|---|---|
| CPU | Dynarec, 2 threads, **Dynarec, 1 thread**, Interpreter | 2 threads (experimental, can freeze some games): new game code is translated on the PPU's second thread while the game goes on more slowly. Interpreter only for games that misbehave. |
| Renderer | **RSX 2x**, 3x, 4x, 4.5x, Software (native), RSX 1x (native, drawn by the RSX) | In game, it shows the resolution actually drawn. |
| Picture filter | **Smoothing on SPUs**, Sharp (nearest), Smooth (bilinear), FXAA (RSX) | |
| Aspect ratio | **4:3**, 14:9, Zoom 14:9, Zoom 16:9, Stretch to screen | Zoom crops the top and bottom. |
| Widescreen (GTE 16:9) | **Off**, On (16:9) | GTE widescreen, like DuckStation: in RTPS/RTPT the projected X offset is scaled 3/4 before adding OFX (the screen centre is not scaled), so more of the 3D world fits inside the 4:3 VRAM picture. Pair with Aspect ratio = Stretch to screen on a 16:9 TV, which stretches it back to correct proportions with a wider field of view; at 4:3 it only looks squashed, and Zoom 16:9 is not a substitute (it crops the top and bottom and stretches vertically). Only GTE-projected 3D vertices are affected: sprites, HUD, 2D backgrounds, FMV and CPU-projected polygons keep their coordinates and stretch with the display aspect, and games that cull in world space at 4:3 gain no extra field of view. Default off, per game. Takes effect immediately, no game restart. |
| Screen size | **100% x 100%** | Shrink or stretch the picture to fit your TV. |
| Frame skip | **Auto**, Off | Software renderer only. |
| Dithering | **Off**, On | |
| Show FPS | **On**, Off | Game frame rate and emulation speed. |
| GPU thread | **On**, Off | Software renderer on the PPU's second thread. |
| Left stick as D-pad | **On**, Off | |
| Right stick as buttons | **Off**, On | Triangle/X/Square/Circle. |
| Vibration | **On**, Off | DualShock 3 vibration in games that use it. |
| FMV decoding | **SPUs**, PPU | |
| PS1 CPU speed | **Normal**, +50%, +75% | Some games may glitch. |
| Sound when slow | **Slow down**, Keep pitch | |
| Skip intro videos | **Off**, On | Videos in a game's first 3 minutes. |
| PAL games at 60 Hz | **Off**, On | Applies from the next game start. |
| PS1 logo at start | **Off**, On | Needs a BIOS in `USRDIR/system`. |
| Speed limit | **Normal**, Strict, Off | Normal catches up after a hitch; Strict never goes above 100%. |
| VRAM check (debug) | **Off**, On | The software renderer draws too, for comparison. |
| Debug log | **Off**, On | Writes the log files and hang reports. |
| Texture filter | **Nearest**, Bilinear, Bilinear (Performance) | Prototype, RSX only, in the upscaled pass only, directly below Renderer in the menu. Bilinear = manual 4-tap for all textured modes. Performance = hardware LINEAR for 15-bit direct textures only (1 fetch), manual bilinear for CLUT modes, so the speedup depends on how much 15-bit texture a game uses; its transparent/STP edges are softer than manual (plain average, not majority vote) and texture-window edges can blend across the window. Native VRAM stays nearest, so it never double-blurs. Takes effect immediately, no game restart. |
| Sprite filter | **Nearest**, Bilinear, Bilinear (Performance) | Same modes as Texture filter, but for sprites, like DuckStation's Sprite Texture Filtering: textured GP0 sprites (textured rectangles, `do_rect`) plus textured polygons whose UV mapping is axis-aligned (U varies only with X, V only with Y — a 2D, non-rotated, non-skewed quad/triangle; both triangles of a quad must pass). Nearest keeps small sprites, particles and HUD icons crisp. Rotated, skewed or perspective polygons keep Texture filter. RSX only, upscaled pass only. Takes effect immediately, no game restart. |
| Memory card 1 | **Auto (per-game)**, Shared (imported), None | Auto: the game's own card, `saves/<serial>_1.mcd`. Shared: the imported PS3 VMC card, `saves/pcsx-card1.mcd`, shared by every game set to Shared. Per game. Applies on the next launch from the game list; Reset alone does not apply it. |
| Memory card 2 | **None**, Auto (per-game), Shared (card2) | Auto: `saves/<serial>_2.mcd`. Shared: `saves/pcsx-card2.mcd` — not the imported VMC; import only creates slot 1's card. Per game. Next launch only. |
| Import PS3 VMC | Press X | Copies a PS3 virtual card from `savedata/vmc/*.VM1` (read-only there, never written) into the shared card `saves/pcsx-card1.mcd`, backs up the old shared card as `pcsx-card1.mcd.bak`, and switches Memory card 1 to Shared. One-way only: saves made here never appear in XMB. Game list only, never in a game. A PS1 card holds only 15 blocks, so expect many `.VM1` files: the picker sorts them and scrolls. |

Settings for all games are saved in `USRDIR/settings.txt`.

## Per-game settings

In game, open the menu and choose **Own settings for this game**. From then on,
setting changes made in that game apply to it only. They are saved in
`USRDIR/configs/<game>.ini`, named after the game file (or the disc's label). The
game list marks such games with "own settings".

Choosing the entry again removes them, and the settings for all games apply again.
Screen size is always shared by all games.

## Performance tips

- **RSX 3x** is the sweet spot: most games run at full speed with any filter.
- **4x and 4.5x** depend on the game. Heavy scenes, such as Silent Hill's fog, can
  be more than the RSX can draw at 60 fps.
- The filter only costs RSX time. FXAA costs the most. Smoothing on SPUs leaves the
  RSX free; Sharp and Smooth cost almost nothing.
- Give a demanding game a lower resolution in its own settings, and keep 4x for the
  rest.
- If a game itself runs slowly (low game FPS at 100% speed), try **PS1 CPU speed
  +50%** in its own settings.
- Games on the disc can pause briefly while the console's drive seeks. Disc images
  on the HDD read ahead and don't: use **Copy disc to HDD** for those games.

## Files

All paths are inside `/dev_hdd0/game/PCSR00001/USRDIR`.

| What | Where |
|---|---|
| Settings | `settings.txt`, `configs/<game>.ini` |
| Save states | `states/` |
| Memory cards (one per game) | `saves/<serial>_1.mcd` |
| Shared card (imported PS3 VMC) | `saves/pcsx-card1.mcd`, backup `saves/pcsx-card1.mcd.bak` |
| BIOS | `system/` |
| Logs (only with Debug log on) | `log.txt` (app and menus) and `logs/<game>.txt`, replaced on each run |
| VRAM dumps, hang dumps | `dumps/` |

## Reporting a problem

1. Turn on **Debug log** (Settings, or the game's own settings).
2. Reproduce the problem.
3. Send `logs/<game>.txt`.

For a drawing problem, also turn on **VRAM check**, go to the wrong-looking scene,
and choose **Dump VRAM** in the in-game menu. Each dump is numbered (no dump
overwrites another) and writes these files to `dumps/`:

- the RSX picture
- the software renderer's picture
- a difference picture
- a GP0 trace of the next frame

## How it works

### RSX renderer

With RSX 1x–4.5x, the RSX draws every polygon, line, sprite and fill twice:

- once at the chosen scale for the screen;
- once at native resolution into a copy of the PS1's VRAM, which textures are read
  from.

The core's own VRAM is synced only for the rectangles it touches (uploads, copies,
reads, save states). Further details:

- Texture coordinates are clamped to each primitive's range, so upscaled edges
  don't pick up neighbouring texels.
- Polygons follow the PS1's pixel coverage and colour rounding in the native pass.
- The mask bit (bit 15: set mask, check mask) is kept in the native VRAM. Once a
  game uses the mask check, both passes also keep it in the stencil buffer, so
  there's no stall per primitive.
- A self-test at start-up checks the RSX's surface and texture layout and which
  bit-15 method works on the console. If it fails, the log says why and the hybrid
  renderer is used instead.
- 24-bit (FMV) frames are shown from the core's VRAM.

### SPUs

Five SPU threads take two kinds of jobs:

- **FMV decoding**: the SPUs decode videos ahead with the core's own routines. The
  output is identical, checked at start-up.
- **Smoothing on SPUs**: the RSX scales each frame to the screen into main memory,
  the SPUs smooth its edges, and the RSX only copies the result.

### Changes to the core

This repository is PCSX-ReARMed (notaz/pcsx_rearmed, branch `libretro`) with the PS3
changes on branch `ps3`; the PS3 front end is in `ps3/`. The core changes are also
in `core-patches/` (0001–0036) for applying to upstream:

- Lightrec on the PS3 (64-bit ELFv1 PowerPC): function descriptors, real cache
  flushing, the code buffer in `.text`.
- No Cell-microcoded instructions in Lightrec's code (each costs about 11 cycles on
  the Cell PPU); the core is built with `-mcpu=cell`.
- gpulib hooks that let the front end draw with the RSX instead of the software
  renderer.
- MDEC decoding routines shared with the SPU program.
- Lightrec: a full code buffer frees the other blocks instead of stopping, and
  accesses to unmapped memory no longer stop emulation.
- A GTE fix (matrix-vector helpers read the wrong registers) and a 15-bit colour
  conversion fix for big-endian.
- Reading PS1 discs in the console's drive, with asynchronous CD read-ahead.
- Lightrec's threaded compiler on the PPU's second hardware thread (lv2 threads),
  switched on or off at run time by the CPU setting, with fixes for its queue
  and code table.
- A faster memory allocator for GNU Lightning.
- Hang reports and dumps for the debug log.

Translating PS1 code is slow on the Cell: about 0.3 to 2 ms per block. A game that
loads a lot of new code at once (a race start in GT2) stutters while it is
translated, with 1 or 2 threads alike. With **Dynarec, 2 threads**, Lightrec
translates on the PPU's second hardware thread while the code runs in its
interpreter, but the two threads share one core.

Translated code stays as long as the game runs: when the same code is loaded again
(GT2's race code after the menus), Lightrec finds it unchanged and does not
translate it again, so only the first race after loading the game stutters.

The picture uses three display buffers. The next frame is drawn into the free
one, so the CPU no longer waits for the TV's refresh before drawing.

The front end runs the emulator on a 2 MiB thread: the PS3's main thread has only
64 KiB of stack, which Lightrec's interpreter overflowed in some games.

## Building

You need:

- the PS3 toolchain and PSL1GHT (https://github.com/ps3dev/ps3toolchain);
- `python3`;
- NVIDIA's Cg toolkit for `cgcomp` (Ubuntu: `nvidia-cg-toolkit`).

PSL1GHT needs `psl1ght-patches/0001-rsx-context-callback-clobbers.patch`. Its
command-buffer callback is inline assembly that doesn't declare what it changes;
with GCC 11 or later, the GPU hangs after about 1,000 frames. Apply the patch, then
rebuild and install `ppu/librsx`. `build.sh` refuses to build without it.

```sh
PS3DEV=/usr/local/ps3dev ps3/build.sh [name]   # -> ps3/build/<name>.pkg
```

`ps3/build.sh` builds the core from this repository (the folder above `ps3/`), which
already has the changes. `DYNAREC=0 ps3/build.sh` builds an interpreter-only package.

## Tests

```sh
hosttest/fetch_testdata.sh   # PS1 test programs by krom (PeterLemon/PSX)
hosttest/build.sh            # needs gcc-powerpc64-linux-gnu qemu-user nvidia-cg-toolkit libosmesa6-dev
cd hosttest && ./runall.sh runner_lr_ppc64 out_lr 180 pcsx_rearmed_drc=enabled
cd hosttest && ./hwr_full_all.sh     # RSX renderer, native build with a Mesa backend
```

- **CPU and GTE:** the core runs big-endian under `qemu-ppc64`, in the same byte
  order as the PS3. All CPU/GTE instruction tests pass with the interpreter and
  with the dynarec. The 72 test programs give the same frames with both (one
  differs only in timing).
- **RSX renderer:** an OpenGL model of the backend, using the same shaders,
  replays test programs and GP0 traces from real games (`hosttest/gp0replay`).
  Its output matches the software renderer apart from edge pixels.
- **Disc reader:** `hosttest/disc` checks the reader, its patch transaction and
  the disc copy against simulated drives and kernels.

QEMU can't check instruction-cache behaviour, RSX timing or performance on a real
console; those were tested on the PS3.

## Credits and licence

- PCSX-ReARMed (notaz and contributors), PCSX/PCSX-Reloaded, P.E.Op.S. GPU
  (dfxvideo), dfsound, Lightrec and GNU Lightning, libchdr, libretro-common.
- Menu font: DejaVu Sans Mono (Bitstream Vera licence).
- Disc reading (playing and copying PS1 discs from the drive): added by another AI
  agent, following IRISMAN's method.
- PS1 game titles (`ps3/data/ps1titles.txt`, made by `ps3/tools/make_titles.py`):
  [libretro-database](https://github.com/libretro/libretro-database), CC BY-SA 4.0.

Licensed under the GPL v2 or later; see `COPYING`. The original PCSX-ReARMed README
is in [README.upstream.md](README.upstream.md).
