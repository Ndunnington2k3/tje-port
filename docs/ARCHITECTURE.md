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

`tools/generate.py` runs `tools/m68k2c/m68k2c.py --source Source/EntireSource.X68 --rom <ROM>`.

`srcmap.py` finds the address of every source line without an assembler, by walking the source and the
ROM together (the disassembly is byte-identical to the ROM): instructions take the length the decoder
finds, `DC.B/W/L` their item sizes, `INCBIN` the file size. It stops with an error if any instruction's
mnemonic differs from the ROM, any literal data differs, or the total isn't the ROM size. This gives the
same layout as a vasm listing (checked), with untruncated comments.

Optionally (`--vasm`) the source is also assembled and compared byte for byte with the ROM; two
`JSR (-$xxx,PC)` lines that newer vasm releases reject are rewritten to identical bytes for that.

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

- Each basic block adds its 68000 cycle cost (`CY(n)`), from the Motorola timing tables
  (`tools/m68k2c/timing68k.py`; branches costed as taken, data-dependent ops at typical values).
- 68000→VDP DMA stalls the CPU for as long as the real transfer takes (205 bytes per line in blanking,
  18 during active display, in H40 mode), so frames with big transfers take as long as on hardware.
- Loop heads and function entries check whether a scanline's worth of cycles (488) has passed
  (`RT_POLL`).
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

## Audio (src/core/sound.c)

The game uploads a Z80 sound driver (`Z80SetupData`, ~1.3 KB) that plays the music; the 68000 also
writes FM registers directly and talks to the driver through Z80 RAM ($A00040...).

- Z80: superzazu/z80, with the Genesis Z80 memory map (RAM, YM2612 at $4000, bank register at $6000,
  PSG at $7F11, 68000 window at $8000). Bus request / reset lines are modelled; VBlank raises its INT.
- YM2612: Nuked-OPN2 in YM2612 mode, clocked at its internal rate (68k clock / 6, 24 cycles per sample
  = 53,267 Hz). The YM busy flag reads as clear so polling loops don't stall between scanlines.
- PSG: small SN76489 model (only used at boot to silence it).
- Timing: the Z80 and FM chip catch up to the 68000's cycle count whenever either side touches them and
  once per scanline, so register writes keep their order and timing.
- Output: resampled to 48 kHz with a DC-blocking filter, passed to `plat_audio()` once per frame.

The music deliberately sounds rough in places: several instruments use operator feedback 6-7, which is
noisy on the real chip too.

Planned: a native sound routine behind the same `snd_*` calls, and a lighter FM core for PSP/3DS.

## Save states

`--state` loads a Gens save state (RAM, VRAM, CRAM, VSRAM, VDP registers, Z80 RAM, FM registers) and
calls `InGameLoop` with the stack pointer the main loop uses ($FF7FE4). The translator exports
`InGameLoop` as an entry point for this.

## Testing

**Per-instruction check against a reference 68000** (`tools/isa_test`, CMake option
`TJE_BUILD_ISA_TEST`): `generate.py` also writes `src/generated/isa_cases.c`, one C function per distinct
instruction in the game (~13,300) containing exactly the code the translator emits for it. `isa_test`
runs each from many random register/flag/RAM states, both as that C and on Musashi (fetched by CMake),
and compares registers, condition codes, memory and branch decisions. Result so far: 13.3 million
trials, 0 mismatches. Not compared: flags the 68000 leaves undefined, and C after a DIVU/DIVS overflow
(the manual says cleared, which we do; Musashi leaves it). Odd-address word/long accesses (address
errors on hardware) are skipped.

The decoder also enforces the 68000's addressing-mode rules, so data that only *looks* like code
(e.g. offset tables decoding as `ORI.b #x,D3`, or `ORI` to an address register) is rejected.

`tools/stress_test.py` runs the headless build from boot and from every level's save state with random
input (and optionally forced elevator rides and sound emulation), in parallel, and fails on any fatal
error, bad jump table target or stack imbalance. Coverage builds (`-DTJE_COVERAGE`) record which
translated routines ran.

## Verifying rewrites (planned)

A harness runs a generated routine and its hand-written replacement from the same state and compares
RAM and VDP state afterwards, and replays recorded input for whole-game comparisons.
