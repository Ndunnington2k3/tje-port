# Architecture

## Goals

- Playable on Windows first, then PSP and Nintendo 3DS at full speed.
- Planned changes: HUD, aspect ratio (widescreen), sound routine, game mechanics.
- Emulated FM sound stays available as an option alongside a rewritten sound routine.

## Layers

```
 src/game       hand-ported C (replaces generated routines one by one)
 src/generated  translated 68000 routines (local only)
      |  memory access, VDP / input / sound calls
 src/core       Genesis model: 68k RAM, ROM data, VDP, input, sound interface
      |  platform interface (video, audio, input, files, timing)
 src/platform   SDL2 backend (Windows, Linux, macOS); later PSP and 3DS backends
```

Game code never calls SDL or any platform API directly.

## Translator (tools/m68k2c)

- Input: the disassembly's `.X68` sources (vasm Motorola syntax), following `INCLUDE`s from `Master.X68`.
- Output: one C function per routine, operating on an emulated register file (`D0–D7`, `A0–A7`, flags)
  and the Genesis memory map through `core` accessors.
- Condition flags are computed lazily (only when a later branch reads them) to keep the code fast.
- Data tables and graphics are not emitted as C; they are read from the user's ROM at startup.
- Hardware writes (VDP ports, Z80/YM2612, controller ports) become calls into `core`.

## Byte order

The 68000 is big-endian; PC, PSP and 3DS are little-endian. Translated code accesses game memory through
byte-swapping helpers. Hand-ported code uses native structs and is free of this cost.

## Verifying rewrites

A test harness runs a generated routine and its hand-written replacement from the same state and compares
RAM and VDP state afterwards, and can replay recorded input for whole-game comparisons.

## Video

Milestone 1 uses a software VDP renderer (exact 320×224 output). Later, level and sprite drawing move to a
native renderer that can show a wider view and use GPU hardware on PSP and 3DS.

## Audio

Audio is behind its own interface. Backends: emulated YM2612 + PSG (a lightweight core suitable for PSP),
and a native sound routine written later.
