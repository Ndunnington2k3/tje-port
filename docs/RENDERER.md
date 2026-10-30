# The new renderer: how the game draws, and what the new gameplay view reads

Audit of the original drawing code (Source/EntireSource.X68 in the disassembly), done before writing
the gameplay view. Addresses are the game's; labels are the disassembly's.

## The rule

The game never knows the screen size. It keeps its 320x224 cameras and all its own visibility tests,
so the gameplay, saves, world codes and LAN are identical at any window size. The new view only reads
game memory and the ROM. Checked by `tools/view_parity.py` (same inputs at several view sizes must give
byte-identical RAM).

## Cameras

| RAM | What |
|---|---|
| `$FF8036/38` TOEJAM_CAMERA_NEWX/Y | ToeJam's camera (top-left of his 320x224 view), set by the game logic |
| `$FF803A/3C` EARL_CAMERA_NEWX/Y | Earl's (used when split; otherwise copied from ToeJam's) |
| `$FF8002-08` *_CAMERA_XPOS/YPOS | copies taken at VBlank (what the planes are scrolled to) |
| `VHBLANK_FLAG_1` (`$FF9364/65` group) | split screen on (each half 320x112) |

## Every place the game uses a camera, and what a wider view means for it

| Routine | Uses the camera for | Effect of a wider view |
|---|---|---|
| `LoadInEarthlings` ($18300-ish) | wakes Earthlings within x -$A0..$1E0, y -$70..$150 of a camera | none (logic); anything the wider view shows (up to 640x448) is already awake |
| `LOC_00012A26` | same window for elevators | none |
| `LOC_00015118` | dropped/level items within x -$14..$154 (and y) of a camera | none (logic) |
| `LOC_00022C94` area (`LOC_00022CA8`) | same kind of window ($14/$154) for another object list | none (logic) |
| `LOC_0000BDB8` | **is an object on screen?** x from -halfwidth($29) to 320, y from -10 to 224 (112 split), minus height ($2A) and altitude (Z) | decides which objects get hardware sprites; objects outside the original screen have none. **The new view must draw objects itself** (below) |
| `LOC_0000C222` | object position on screen = world + $80 - camera (hardware sprite coordinates) | the new view does the same sum with its own, wider origin |
| `LOC_0001DC24`, `LOC_00020D10`, `LOC_00021BE8` | create an object at camera + ($A0 or $50, $E0/$E1) with altitude $70/$A8, i.e. coming up from the bottom middle of the screen | positioned in the world, so the wider view shows them correctly, still centred; check visually that nothing appears in the newly visible strip below |
| `LOC_00023160` | screen positions of a list at `$FFE264` | to check in step 2 |
| `LOC_00013540` (elevator step 1), `LOC_00016068` (arriving on a level) | sets/reads cameras when moving between floors | none |
| Plane streamers `LOC_000016D2` (rows), `LOC_00001BA8` (columns), `UpdateCameraY` | copy the level into the 64x32-cell planes just around the 320x224 view | the planes do not hold the wider area: **the new view draws the level from the floor data** |

Earthling placement (`FindSpawnLocationForEarthling`, `LOC_00018064`) uses players' positions and the
floor layout, never the camera.

## Objects (sprites)

- Object table: 40 entries of $80 bytes from `TOEJAMS_XPOS` ($FFA25A) to $FFB659 (players are 0 and 1).
  Known fields: `$00/$02/$04` world X/Y/Z, `$1A/$1E` animation pointers, `$22` current frame pointer,
  `$26-$28` animation timers, `$29/$2A` half width / height, `$2F` flags (bit 0 = exists, bits 1/2 =
  has hardware sprites on camera 0/1), `$30..` its hardware sprite numbers, `$48/$49` first sprite per
  camera, `$4A` sprite count, `$4B` sprite id, `$4C` floor, `$4D` flags, `$5E` player.
- `LOC_0000BE92` (per object, each frame): `LOC_0000BDB8` on-screen test per camera; on screen ->
  allocate hardware sprites and pattern space; off screen -> release them (`$D7FA`, `LOC_0000D756`).
- `LOC_0000C222`: writes each object's hardware sprites into the sprite table copy at `$FFB65A`
  (80 x 8 bytes, DMA'd to VRAM $F800). `LOC_0000D05A` links them (draw order).
- `LOC_0000C4DA` / `LOC_0000C900`: unpack the current frames' graphics (`SpriteUnpackRLE`,
  `SpriteUnpackInterleavedRLE`, `SpriteUnpackRaw`) into `$FFB8DA` and queue them for VRAM
  (`$FFD8DC` list, count `$FFD92E`), sent at VBlank by `SpriteLoadingStuff`.

So for the new view: walk the object table every frame; for each object that exists and is on the
floor being drawn, take its current frame (from `$22` and the animation data in ROM), unpack the
graphics from ROM with the game's own unpack routines into the port's texture cache, and draw it at
world - view origin. This covers objects outside the original screen, which the game never gives
hardware sprites. Draw order: the game's (to be confirmed from `LOC_0000D05A`: sorted by Y).

## Level background

- Floor layout per player: `TOEJAM_LEVEL_TILES` ($FF81AA) and `EARL_LEVEL_TILES` ($FF8586), 26 rows of
  19 words ($26 bytes a row), one word per 128x64-pixel block (block = X/128, Y/64).
- Graphics: `LevelTiles` in ROM (loaded to VRAM at floor load); the streamers turn each block into
  8x8 cells in the planes. The new view builds the whole floor (2432x1664 px) once per floor into its
  own tile map, by running the same block -> cells logic, and draws the visible part.
- Water animation: `DrawLevelGraphics` cycles four colours in CRAM every 4 frames. The new view reads
  colours from the live CRAM every frame, so this, fades and flashes work unchanged.

## What stays on the Genesis path

Menus, cutscenes, the intro, the map screen, Jam Out, the pause and present screens' text (window
plane), and the status bar until it is redrawn natively (step 4). Those keep 4:3 with borders.

## The gameplay view (src/game/gameview.c), as built

- Drawn line by line, right after the VDP draws the same line (`genesis_set_line_hook`), so it sees the
  machine exactly as the VDP does at that moment. This matters: the game rewrites the sprite table
  during the frame, and in split screen it switches plane A (`$A000` / `$C000`) and the scroll one line
  into the lower half.
- At line 0 it decides whether this is gameplay: display on, window plane covering whole rows (status
  bar at the bottom, or the present list from the top), and plane A holding the floor where the camera
  is (sampled against the floor layout, at the last line of each half). Otherwise the VDP picture is
  shown (menus, map, transitions, SEGA screen).
- Per line: window rows copied from the VDP line; plane A and B as the VDP shows them inside the original
  screen; sprites from the sprite table without the per-line limits (no drop-outs); VDP priority rules.
- The floor layout -> cell rules (`level_cell`, from LOC_00001612 / LOC_00001670) are verified to match
  plane A exactly; they draw the level outside the original screen. Inside it, plane A from VRAM is
  used, because the game sometimes changes the layout first and redraws that ground a little later.
- `tje_headless --gvcheck PREFIX` compares it with the VDP picture every frame. Result over random play
  on every save state and a 2-player boot with split screen (~50,000 gameplay frames): identical except
  one frame at a split-screen switch where the game briefly draws its divider bar into plane A.
- Option `native_view` (on; `--vdp-view` or `--vanilla` turns it off).

## Steps

1. Done: drawing layer (`src/gfx`), OpenGL + fallback, whole-number zoom, live resize.
2. Gameplay view at exactly 320x224, compared pixel for pixel with the VDP picture (done, above).
   Next for step 3: objects outside the original screen, drawn from the object table (they have no
   hardware sprites).
3. Wider view (`view_layout_expand`), both halves when split.
4. Status bar, minimap and LAN markers drawn natively.
5. PSP (sceGu) and 3DS (citro3d) backends.
