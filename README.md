# tje-port

A multiplatform C port of *ToeJam & Earl* (Sega Genesis, 1991), built from the
[ToeJam & Earl disassembly](http://www.mybrillgamesite.com) by RyanFaeScotland.

Windows is the first (prototype) target. PSP and Nintendo 3DS are planned.

> This repository contains **no game code or assets**. You need your own copy of the
> disassembly and the original ROM (`Toejam & Earl (U) (REV 00) [!].bin`) to build and run it.

## Status

- The whole game is translated to C (721 routines, ~54,000 68000 instructions) and runs natively:
  SEGA logo, intro, menus, Jam Out, one- and two-player games, split screen, presents, map.
- Video: software renderer of the Genesis VDP (planes, window, sprites, scrolling, DMA).
- **Sound: not yet** (the sound chip writes are accepted and ignored).
- Tested with hours of automated random play without errors.

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
| `tools/generate.py` | One-step: assemble disassembly → verify against ROM → translate |
| `tools/m68k2c` | 68000 decoder and C emitter |
| `src/core` | Runtime for translated code, Genesis model (memory, VDP, I/O, timing) |
| `src/platform` | Platform backends: `sdl2` (desktop), `null` (headless testing) |
| `src/game` | Hand-ported game code that replaces translated routines (coming) |
| `src/generated` | Translator output (local only) |

## Building on Windows

You need:

- **Python 3** (python.org; tick "Add to PATH")
- **vasm** for 68000, Motorola syntax (`vasmm68k_mot_win32.exe`, the one the disassembly uses)
- **CMake 3.20+** and either **Visual Studio 2022** (Desktop C++ workload) or **MinGW-w64**
- Git

SDL2 is downloaded and built automatically by CMake if it isn't installed.

```bat
cd C:\Users\NJD\Desktop\Stuff\TJEDisassembly\PC\tje-port

REM 1. Translate the game (re-run whenever the disassembly or translator changes)
python tools\generate.py --disasm ..\..\master --rom "..\..\Toejam & Earl (U) (REV 00) [!].bin" --vasm D:\VASM\vasmm68k_mot_win32.exe

REM 2. Build
cmake -S . -B build
cmake --build build --config Release

REM 3. Run (pass the ROM, or copy it next to tje.exe as tje.bin)
build\Release\tje.exe "..\..\Toejam & Earl (U) (REV 00) [!].bin"
```

With MinGW the executable is `build\tje.exe` instead of `build\Release\tje.exe`.

## Controls

| | Player 1 | Player 2 |
|---|---|---|
| D-pad | Arrow keys | W A S D |
| A / B / C | Z / X / C | J / K / L |
| Start | Enter | Right Shift |

Game controllers work too (X = A, A = B, B = C on an Xbox layout). F11 or Alt+Enter toggles
fullscreen; Escape quits.

## Testing

`tje_headless` runs the game without a window, for automated checks:

```
tje_headless <rom> --frames 3000 --press 600:5:S --shot 1200:frame.ppm
tje_headless <rom> --frames 40000 --random 7       # mash random buttons for ~11 minutes
```

Any untranslated call, bad jump or stack imbalance is reported on stderr.

## Legal

*ToeJam & Earl* is © Sega / ToeJam & Earl Productions. This project is a non-commercial fan
effort; the disassembly it depends on prohibits commercial use.
