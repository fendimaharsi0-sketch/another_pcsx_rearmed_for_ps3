# PCSX-ReARMed for PS3 — Personal Fork (Snapshot)

## About this fork

This is a personal fork of PCSX-ReARMed for PlayStation 3, maintained for my own daily use. It is not an official release, and it is not affiliated with, or endorsed by, the original project.

This snapshot was built from the `ps3` branch. It is provided as-is, as a point-in-time snapshot, without any guarantee of further updates or long-term maintenance.

## A note on authorship

I wish to be clear about the origin of this work. This fork was developed with the assistance of an AI assistant, and I do not claim that the code herein was written solely by me.

The greatest credit belongs to **Calyps0man**, whose work made this fork possible. He built an extraordinarily solid core, and everything that makes this emulator function as it does rests on that foundation — together with the upstream PCSX-ReARMed project and its contributors. My own contribution is modest by comparison.

## What this snapshot adds

At this time, the changes in this fork are limited to cosmetic improvements and quality-of-life features. No core emulation work has been undertaken. The additions are:

- **Cheat support** — a fullscreen cheat menu with per-game cheat settings. Cheats must first be enabled via the global “Enable cheats” setting in System & Saves (off by default); only then will the cheat menu appear in the quick menu and any cheat apply.
- **Game cover support** — display of game cover artwork in the game list.
- **CD speed-up** — an option to accelerate CD-ROM reading (TurboCD).
- **Real-time CPU switching** — switching between the dynamic recompiler (Dynarec) and the interpreter from the quick menu, as a workaround for certain titles that hang under one CPU mode. Live switching can be unstable in some cases; it is provided as a workaround, not as a fix.
- **Disc swapping** — changing discs from the quick menu, for multi-disc titles and boot-disc scenarios.
- **Per-slot memory card picker** — choosing a memory card per slot (Auto first, then imported cards, then None, plus the legacy Default where present), with protection against two slots pointing to the same file. Slot 1 defaults to Auto, slot 2 to None.
- **Boot BIOS entry** — booting into a real BIOS (a `scph5501.bin` file in the system folder) from the game list, for those who need the BIOS menu or memory card manager. A real BIOS remains optional; the HLE BIOS is still the default when no BIOS file is present.
- **Digital pad / analog toggle** — switching between digital and analog pad modes from the quick menu.
- **Widescreen rendering** — a GTE-based 16:9 widescreen hack.
- **Texture filtering** — bilinear texture filtering, including a performance variant, and sprite texture filtering.

## Limitations

I must be forthright about my limitations. Due to my limited knowledge of emulation and programming, I am unable to provide:

- Fixes for core emulation issues.
- Game compatibility updates.
- Fixes for core rendering.
- Any other work relating to core system performance.

For such matters, users are kindly directed to the upstream project and to the wider community, whose expertise far exceeds my own. I apologize in advance for any shortcomings in this fork, and I am grateful for your understanding.

## License

This fork follows the license of the upstream project. The emulator core is distributed under the GNU General Public License, version 2 (see `COPYING` in the repository).

The bundled cheat database, where included, remains the work of its original authors and is included with attribution. The cheat handling code added in this fork is provided under the MIT license.

This software is provided without warranty of any kind, as permitted by the applicable licenses.

## Acknowledgments

My sincere thanks go to Calyps0man, to the PCSX-ReARMed developers and contributors, and to everyone whose work this fork quietly stands upon. Any errors or shortcomings in this snapshot are entirely my responsibility, and not the responsibility of the previous developers. However, I acknowledge that I am unable to provide support, owing to my own personal limitations.
