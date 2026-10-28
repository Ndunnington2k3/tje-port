# tje-port

A multiplatform C port of *ToeJam & Earl* (Sega Genesis, 1991), built from the
[ToeJam & Earl disassembly](http://www.mybrillgamesite.com) by RyanFaeScotland.

Windows is the first (prototype) target. PSP and Nintendo 3DS are planned.

> This repository contains **no game code or assets**. You need your own copy of the
> disassembly and the original ROM (`Toejam & Earl (U) (REV 00) [!].bin`) to build and run it.

## How it works

1. **Translate** – `tools/m68k2c` reads the disassembly and turns every 68000 routine into C
   (`src/generated/`, built locally, never committed). This gives a complete, faithful game quickly.
2. **Run** – `src/core` supplies what that code expects from the Genesis: memory, the video chip
   (VDP), controllers and sound. `src/platform` puts it on screen through SDL2.
3. **Rewrite** – routines are replaced one at a time with clean, hand-written C in `src/game`.
   Each rewrite is checked against the translated original by running both and comparing game
   memory frame by frame, so behaviour stays identical until a change is intended.
   Planned rewrites: HUD, widescreen camera and level drawing, sound, and game mechanics.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for details.

## Layout

| Folder | Contents |
|---|---|
| `tools/m68k2c` | 68000 assembly → C translator |
| `src/core` | Genesis memory map, VDP, input and sound interfaces |
| `src/platform` | Platform backends (SDL2 first) |
| `src/game` | Hand-ported game code that replaces translated routines |
| `src/generated` | Translator output (local only) |
| `docs` | Design notes |

## Building (Windows)

Requires CMake 3.20+, SDL2, and either Visual Studio 2022 or MinGW-w64. Full steps will be added
once the first playable build lands.

## Legal

*ToeJam & Earl* is © Sega / ToeJam & Earl Productions. This project is a non-commercial fan
effort; the disassembly it depends on prohibits commercial use.
