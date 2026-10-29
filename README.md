# tje-port

A multiplatform C port of *ToeJam & Earl* (Sega Genesis, 1991), built from the
[ToeJam & Earl disassembly](http://www.mybrillgamesite.com) by RyanFaeScotland.

Windows is the first (prototype) target. PSP and Nintendo 3DS are planned.

> This repository contains **no game code or assets**. You need your own copy of the
> disassembly and the original ROM (`Toejam & Earl (U) (REV 00) [!].bin`) to build and run it.

## Status

- The whole game is translated to C (~770 routines, ~57,000 68000 instructions) and runs natively:
  SEGA logo, intro, menus, Jam Out, one- and two-player games, split screen, presents, map.
- Video: software renderer of the Genesis VDP (planes, window, sprites, scrolling, DMA).
- Sound: emulated - the game's Z80 sound driver runs on a Z80 core and drives an emulated
  YM2612 FM chip (Nuked-OPN2) and PSG. A native sound routine can replace this later.
- Tested from boot and from save states on all 26 levels with automated random play
  (`tools/stress_test.py`), with no errors.

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
| `src/game` | Hand-written C that replaces translated routines, and port options |
| `src/generated` | Translator output (local only) |
| `third_party` | Nuked-OPN2 (YM2612, LGPL-2.1) and superzazu/z80 (MIT) |

## Building on Windows

You need:

- **Python 3** (python.org; tick "Add to PATH")
- **CMake 3.20+** and either **Visual Studio 2022** (Desktop C++ workload) or **MinGW-w64**
- Git

SDL2 is downloaded and built automatically by CMake if it isn't installed.

```bat
cd C:\Users\NJD\Desktop\Stuff\TJEDisassembly\PC\tje-port

REM 1. Translate the game (re-run whenever the disassembly or translator changes)
python tools\generate.py --disasm ..\..\master --rom "..\..\Toejam & Earl (U) (REV 00) [!].bin"

REM 2. Build
cmake -S . -B build
cmake --build build --config Release

REM 3. Run (pass the ROM, or copy it next to tje.exe as tje.bin)
build\Release\tje.exe "..\..\Toejam & Earl (U) (REV 00) [!].bin"
```

With MinGW the executable is `build\tje.exe` instead of `build\Release\tje.exe`.

No assembler is needed: `generate.py` lines the disassembly source up against the ROM itself and checks
every instruction and data byte as it goes. If you also want the classic check that the source
assembles to an identical ROM, add `--vasm D:\VASM\vasmm68k_mot_win32.exe`.

Options:

- `--state "<path>\Level 12.gsx"` - start from a Gens save state (the disassembly ships one for every
  level in `Resources\Save States`), handy as a level select while developing.
- `--mute` - no sound (saves CPU).
- `--vanilla` - turn off the port's changes and behave exactly like the original.

Port changes so far (on by default):

- **Quick start**: Start on the SEGA screen goes straight to the Who menu.
- **Pause menu**: Resume, Save Game, Quit to Menu, in the status bar with ToeJam's pointing hand.
  Left/Right (or Up/Down) choose, Start selects, B backs out. Start, Start still just pauses and
  unpauses, and the original pause-screen button code still works.
- **Saved games** (3 files, both players): saving remembers your presents, identified presents, stats
  and rocketship pieces; presents you picked up stay gone; loading puts you back at the start of that floor, stepping out of the
  elevator. A collected ship piece stays collected. Random World keeps its world. Load from
  **Load Game** on the Who screen. Files are `tje_save1.sav`-`tje_save3.sav` next to the program
  (or in the folder named by the `TJE_SAVE_DIR` environment variable).

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

Any untranslated call, bad jump or stack imbalance is reported on stderr. More options:
`--state file.gsx`, `--wav out.wav` (record the sound), `--audio`, `--dumpram F:file`,
`--dumpvram F:file`, `--poke F:addr:val`, `--save F:slot` / `--load F:slot` (saved games).

`tools/stress_test.py` runs many of these in parallel - from boot and from every save state - and
reports failures. Build with `-DCMAKE_C_FLAGS=-DTJE_COVERAGE` to also get a report of which
translated routines ran:

```
python tools/stress_test.py --exe build/tje_headless --rom tje.bin --states "<disassembly>\Resources\Save States" --frames 20000 --seeds 2
```

To check the translator itself, instruction by instruction, against a reference 68000 (Musashi,
downloaded by CMake):

```
cmake -S . -B build_isa -DTJE_BUILD_ISA_TEST=ON
cmake --build build_isa --target isa_test --config Release
build_isa\Release\isa_test.exe tje.bin 256
```

## Third-party code

- `third_party/nuked-opn2` - Nuked-OPN2 by nukeykt, YM2612 emulator, LGPL-2.1
- `third_party/z80` - z80 by Nicolas Allemand (superzazu), MIT
- Musashi (68000 emulator, MIT) - downloaded only for the optional `isa_test` checker

## Legal

*ToeJam & Earl* is © Sega / ToeJam & Earl Productions. This project is a non-commercial fan
effort; the disassembly it depends on prohibits commercial use.
