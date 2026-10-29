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

## Replacing routines with hand-written C (src/game)

A routine is replaced by writing a C function with the translated function's name and a marker:

```c
TJE_OVERRIDE(0x03A476)
void f_03A476_SEGAVideo(void) { ... RETURN(); }
```

`tools/generate.py` scans `src/game/*.c` for `TJE_OVERRIDE` markers and tells the translator, which then
emits the original as `orig_f_03A476_SEGAVideo` (still callable, e.g. for comparisons). All direct
calls and the indirect-call table reach the replacement. It records the list in
`src/generated/overrides.txt`, and CMake stops with a clear message if `src/game` and that list
disagree (generate.py not re-run). The translator warns if other routines *branch* into a replaced
routine's code, since those paths keep running the original.

Replacements follow the 68000 calling conventions (see `src/game/game.h`): the caller has pushed a
return address (`RETURN()` pops it), arguments are on the emulated stack (`ARG_L(n)`), results go in
`R.d[0]`, and D2-D7/A2-A6 must be preserved. To call game routines: `push32()` the arguments,
`CALL(f_...)`, `POP_ARGS(n)`. Loops that wait for the next frame use `wait_vblank()`, which charges the
same 68000 time as the original wait loops.

How a replacement is checked: run the old and new builds with the same input and compare game RAM
(`--dumpram`), ignoring the stack area ($FF7000-$FF7FFF), whose leftover bytes legitimately differ.

Port options live in `src/game/options.c` (`g_options`); `--vanilla` turns all of them off.

Replaced so far:

| Routine | File | Change |
|---|---|---|
| `SEGAAndIntroVideoRoutine` ($027AD6) | `intro.c` | quick start: skip title/intro if the SEGA screen was skipped |
| `SEGAVideo` ($03A476) | `intro.c` | Start ends the SEGA screen cleanly (logo or ship phase); skipped after Quit to Menu |
| `InGameLoop` ($0014E6) | `save.c` | records an arrival snapshot when a player reaches a floor |
| `GamePaused` ($01DBD8) | `pause.c` | pause menu (resume / save / quit) in the status bar |
| `PlayerSelectMenu` ($0235E2) | `menus.c` | Who screen gets "Load Game" and a saved-games list |

`InGameLoop` is a *split point* in the translator (`SPLIT_LABELS`): code flowing into it becomes a call,
so `StartGame` does not inline the loop and the loop can be replaced on its own. Without pausing, RAM
outside the stack matches the previous build exactly (boot with random input, level states, elevator
rides).

Without Start pressed, RAM outside the stack matches the original translation exactly through the SEGA
screen, ship, title and intro (checked at frames 100, 300, 800 and 2900).

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

## Saved games (src/game/save.c, pause.c, menus.c)

Not save states: a save restarts the floor. The machine model can take a complete *snapshot* - CPU
registers, work RAM, VDP memory and state, Z80 and its RAM, FM registers (replayed into the chip on
load, so a snapshot loads the same with sound on or off), PSG - as a portable little-endian blob
(`genesis_snapshot`, `src/core/ser.h`), and restart from one (`genesis_restart_snapshot`, which
`longjmp`s back into `genesis_run`).

The C `InGameLoop` takes a snapshot at the top of the loop whenever a player arrives on a floor (the
first pass of a new game counts). At that moment an elevator arrival still has the player inside
the elevator, so loading replays stepping out. A floor reached by falling keeps the snapshot from
when an elevator last brought a player there. With two players, the most recent arrival is used.

Saving copies that floor's snapshot and writes the current progress into its RAM image: lives, bucks,
points, rank and health ($FFA248-$FFA259), highest floor, present identities and both inventories
($FFDA8A-$FFDAE1) and the ship piece table ($FFE212, 10 floors, 0 = collected). The file holds a
summary for the file list, then the blob with a checksum; it is written to a temporary file and
renamed. Loading restarts from the blob, then on the loop's first pass removes ship-piece earthlings
(type 0 in the 18-byte entries at $FFDE72) whose floor is no longer in the ship table, the way
collecting one does, and redraws the HUD.

The pause menu draws in the window plane (the status bar) with the HUD's font (tiles $5B5 a-z, $5D8
0-9) and restores the bar afterwards. HUD sprites over the bar are moved off screen while it is up;
one of their sprite-table entries shows ToeJam's pointing hand, whose tiles are copied from VRAM when
a menu screen shows it and placed over the rank-text tiles ($59F) for the duration.

The Who screen uses the game's own menu routine (LOC_00023784) with item tables built at run time in
a *port ROM* area after the ROM (`genesis_port_rom`), so the game's code can be handed pointers to
them. Quit to Menu restarts the machine with `g_boot_to_menu` set, which skips the SEGA screen.

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
