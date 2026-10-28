# Architecture

## Goals

- Playable on Windows first, then PSP and Nintendo 3DS at full speed.
- Planned changes: HUD, aspect ratio (widescreen), sound routine, game mechanics.
- Emulated FM sound stays available as an option alongside a rewritten sound routine.

## Layers

```
 src/game       hand-ported C (replaces generated routines one by one)
 src/generated  translated 68000 routines (local only)
      |  R (registers), rd/wr (memory), rt_call, CY/RT_POLL (timing)
 src/core       runtime + Genesis model: RAM/ROM, VDP, I/O, interrupts, scanline timing
      |  platform.h: plat_present, plat_poll, plat_message, plat_rom_path
 src/platform   sdl2 (Windows/Linux/macOS), null (headless tests); later PSP and 3DS
```

Game code never calls SDL or any platform API directly.

## Producing the translation

`tools/generate.py`:

1. Takes `Source/EntireSource.X68` from the disassembly, makes `INCBIN` paths absolute and rewrites two
   `JSR (-$xxx,PC)` lines that newer vasm releases reject (to the identical bytes as `DC.w`).
2. Assembles it with vasm to get a listing: the address of every line, its label and source text.
3. Refuses to continue unless the assembled binary is byte-identical to the user's ROM.
4. Runs `tools/m68k2c/m68k2c.py`.

## Translator (tools/m68k2c)

- `decode68k.py` decodes 68000 instructions from the ROM bytes (full instruction set). It is checked
  against the listing: all 50,714 instruction lines decode to the right length.
- **Functions**: every call target becomes a C function: JSR/BSR targets, interrupt vectors, addresses
  stored in data or loaded with LEA/PEA/MOVE #imm (function pointers), and listing code that nothing
  reaches statically. Names come from the disassembly labels (`f_001360_StartGame`).
- **Bodies**: everything reachable from the entry through branches is emitted into that function, with
  `goto` for branches. Shared tails are duplicated (63.6k emitted vs 53.9k unique instructions).
- **Switch tables** (`MOVE.w (tbl,PC,Dn.w),Dn; JMP (base,PC,Dn.w)`) become C `switch` statements. The
  index range comes from the nearest `CMPI`/`Bcc` bounds checks on the index register.
- **Code hidden in data**: about 3,000 instructions the disassembly lists as `dc.b` (e.g. Jam Out) are
  found by following the code; addresses taken from unlabelled data are tried as code and dropped if
  they don't decode.
- Every translated statement carries the original source line (and comment) as a C comment.

## Runtime model (src/core/m68k_rt.h)

- Registers are the global `R` (`R.d[8]`, `R.a[8]`, `R.sr`). Condition flags are a local `Flags F` in
  each function, so the C compiler deletes flag computations nothing reads.
- JSR pushes the real return address on the emulated stack before calling the C function, and RTS pops
  it, so stack-passed arguments are exactly where the original code expects them. After every call the
  stack pointer is checked (`RT_SPCHECK`); a mismatch would mean the code plays tricks with return
  addresses, and is logged.
- Indirect calls go through `rt_call(addr)`, a binary search of the generated function table.
- Memory is a big-endian byte array for RAM plus the ROM image; `rd16`/`wr32` etc. assemble values with
  shifts. Constant addresses (the common case) let the compiler fold the address decoding away.

## Timing and interrupts

- Each basic block adds an estimate of its 68000 cycle cost (`CY(n)`). Loop heads and function entries
  check whether a scanline's worth of cycles (488) has passed (`RT_POLL`).
- Each scanline the VDP renders one line; the horizontal interrupt counter runs; at line 224 the frame
  is presented, input is read, and the vertical interrupt is raised. Interrupts are delivered by calling
  the handler's C function directly when the SR mask allows it.
- Busy-wait loops that poll a VBlank flag simply run until the cycle count reaches the next frame.

## Byte order

The 68000 is big-endian; PC, PSP and 3DS are little-endian. Translated code goes through the
byte-swapping `rd`/`wr` helpers. Hand-ported code uses native structs and is free of this cost.

## Video

`src/core/vdp.c`: VRAM/CRAM/VSRAM, control/data ports, 68k→VDP DMA, fill and copy, and a per-line
renderer (planes A/B with full/cell/line horizontal scroll and 2-cell vertical scroll, window plane,
sprites with link list, per-line limits and masking, priorities). Not yet: shadow/highlight, interlace.

Later, level and sprite drawing move to a native renderer that can show a wider view and use the GPU on
PSP and 3DS; the software VDP stays as the reference.

## Audio

Not implemented yet. The game writes the YM2612 directly from the 68000 and also uploads a small Z80
program (likely for speech samples). Planned: an audio interface with an emulated YM2612 + PSG + Z80
backend, and a native sound routine.

## Verifying rewrites (planned)

A harness runs a generated routine and its hand-written replacement from the same state and compares
RAM and VDP state afterwards, and replays recorded input for whole-game comparisons.
