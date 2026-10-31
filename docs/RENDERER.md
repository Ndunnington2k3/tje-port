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

### Wider than the original (step 3, 1 player)

- The picture is the size `view_layout_expand` gives for the window (`present_wanted_view`). The
  original 320x224 screen sits in the middle of it; output rows above and below it are drawn at line 0
  and line 223 with the state then.
- Level outside the original screen: from the floor layout. Plane B (starfield) from VRAM, repeating.
- The status bar and the present list (window rows) stay at the bottom / top edge, centred; the level
  continues beside them. Sprites that are not objects' (status bar icons, hint boxes, the pause sign)
  are only drawn on the original gameplay rows; objects' sprites anywhere.
- Objects with no hardware sprites (off the original screen) are drawn from their current frame in ROM
  (frame/piece format and the three unpackers re-implemented read-only), in the game's drawing order
  (object lists at `$FFD938` / `$FFD93C`, linked via `$5A`). An object that passes the game's own
  on-screen test (LOC_0000BDB8) but has no sprites is one the game is hiding, and is skipped.
  `TJE_GV_PIECES=1 tje_headless --gvcheck` draws every object this way and compares with the VDP:
  identical but for a few pixels (elevator base, speech bubble offsets).
- At exactly 320x224 nothing outside is drawn, so the picture is the original's (0 differing pixels
  over ~50,000 frames of random play, all floors, 1P and 2P).
- Split screen stays 320x224 for now (shown with borders).
- Floor items (presents, food, trees and the like, dropped presents, telephones) are not objects until
  they come within about 20 px of the original screen (LOC_00015118, CheckForTelephone); outside that
  they are drawn from their records (8 bytes: type, floor, object, x, y) with the frame the game would
  give them (`item_frame`, from LOC_00014FFE / LOC_00022B7E). Items the game would show but has no
  object for are skipped, as it does.
- An object that has just come onto the game's screen gets its hardware sprites a frame after the
  game's on-screen test passes ($2F bit 1 set, $4A still 0). Wholly inside, the original shows
  nothing for that frame too; partly outside, it is drawn from its frame data, or it would blink
  at the old screen edge for a frame each time it crosses it.
- In step with the hardware sprites: the game runs at 30 frames a second. Its main loop spends two
  video frames on one pass and sends the VDP a new sprite table every other frame (DMA at VBlank);
  in between, the objects in RAM are half updated (next animation frame already set, sprites being
  reallocated, $4A briefly 0). Objects and items are therefore read from a copy of RAM taken when
  the sprite table goes to VRAM (`vdp_set_sat_dma_hook` -> `gameview_sat_dma`), so frame data and
  hardware sprites always show the same moment (live RAM if no table was sent for 4 frames).
  Measured: frame-data positions vs the VDP's sprite table matched in 97% of object-frames before,
  99.9% after; one-frame drop-outs while walking across six floors went from 59 to 0.
  `TJE_GV_SYNC=file` logs, per frame, each object's frame-data position against the VDP's.
- A floor item that the game turns into an object (within $14 px of its screen) gets no animation
  frame until the game's next pass, a whole 30 fps step. On the original that happens just out of
  sight; here the item vanished for two frames each time it crossed that band (the flicker the
  user caught with F12 on the first island). Meanwhile the item is drawn from its record.
- Falling to the floor below: the game switches the floor number and layout as the fall starts,
  while the screen still shows the floor being left for about a second. The layout no longer
  matches plane A, so this used to fall back to 4:3 (or worse, to the extended-screen mode with a
  made-up border). Now, while the camera stays put, the picture around the original screen is held
  as it was and the original screen is the VDP's (`frame_ok` 4, `hold_line`). A plane A that
  differs from the layout in a few blocks only (ground caving in) still counts as gameplay for up to
  60 frames.
- Straight after gameplay, until the display is next switched off, a screen that is not gameplay
  (the screen splitting, a floor being set up) stays 4:3 instead of being extended outwards.
- Drawing order outside the original screen is the game's (LOC_0000C690): objects sorted by Y,
  lower on the screen in front, those with $2F bit 7 (food, flat on the ground: LOC_00014FFE) behind
  the rest, the second list ($FFD93C) after. Floor items drawn from their records are put in the
  same order as the objects they would be, and every piece and hardware sprite of an object gets a
  rank, so a piece and an object's hardware sprite overlap the right way round too.
- An object whose shadow reaches under the status bar or past the screen edges (Cupid flying high)
  is drawn from its frame data with its shadow, as the game cuts the shadow sprite there.
- The present list / shop box (window from the top) stays where the original has it, so sprites it
  half covers (the hint sign) stay next to it; the status bar keeps to the bottom edge.
- Clean transitions: a frame where the display is switched off part way down (the old scene on top,
  black below: falling to the next floor) shows the last picture instead.
- `--view-log` (PC and headless) logs objects and items missing for one or two frames and keeps the
  last 120 frames (picture, RAM, VRAM); F12 on PC saves them (`tje_captureN.bin`), read with
  `tools/viewcap.py`.
- Test tools (tje_headless, wider `--window`): `TJE_GV_FLICKER=file` lists objects and items drawn,
  then missing for one frame, then drawn again; `TJE_GV_BLINK=file` lists sprite pixels that change
  for exactly one frame at the same world position (`TJE_GV_BLINKSAVE=prefix` saves the three
  pictures). Remaining reports are the game's own: a new particle object has no frame for its first
  frame, objects walking out of the picture, animations.
- Objects with hardware sprites that reach outside the original gameplay area are drawn from their
  frame data instead (and their sprites skipped): a hardware sprite's X wraps past 383, and the game
  cuts and culls sprites at its screen edges and under the status bar.
- Shadows of objects drawn that way: the game's shadow sprite (VRAM graphics), learned per shadow
  class ($2E) from objects that have one on screen.
- The status bar is kept flush with the bottom: the filler row under it (one cell repeated) is not
  copied; the level shows there.
- The elevator you arrive in closes and sinks away about 30 frames after you step out (a timer: states
  4 -> 0 -> 9 -> 11 in LOC_00014052 and on); that is the original's behaviour, now visible further out.
- Objects drawn from frame data get the same effects as their sprites would: wading (rows under the
  waterline taken away, the first with a wavy edge; LOC_0000C83A) and flashing (colour 15 dropped;
  LOC_0000C8CC).

### Other screens drawn wider

Option `extend_screens` (on). Every screen that is not gameplay keeps its original 320x224 picture
in the middle, untouched; only what goes around it is made up (cutscenes are designed for 4:3):
- a screen with a border ring (edge blocks whose tiles do not occur further in: the purple frames of
  the cutscenes and menus) has the border carried on outwards, its tiles picked from its top and
  bottom stretch, staggered so it does not look repeated;
- a screen without one whose picture repeats (or one plane of it: the title screen) has the pattern
  carried on, with the period measured in the outer band of the screen;
- the map, during play, gets the menus' starry sky around it (captured from the menus' plane B when
  they are shown), so its purple squares do not look like more floor to uncover;
- anything else stays 4:3.
Gameplay is told apart from cutscenes by the main game loop having started (`g_in_game`) and the
status bar being up (or split screen, or the last floor).

While `g_gameview_extend_screen` is set (by the screen's code) and the window wants a wider picture,
the screen is drawn with its repeating background plane everywhere and everything else only where the
original screen is: 1 = plane B is the background (SEGA screen, stars), 2 = plane A (title screen).
Kept on screen while the VDP is frozen (clean transitions). The SEGA screen also freezes its last
picture across its end, which changes the palette a frame before the screen goes.

## Steps

1. Done: drawing layer (`src/gfx`), OpenGL + fallback, whole-number zoom, live resize.
2. Gameplay view at exactly 320x224, compared pixel for pixel with the VDP picture (done, above).
   Next for step 3: objects outside the original screen, drawn from the object table (they have no
   hardware sprites).
3. Wider view (`view_layout_expand`), both halves when split.
4. Status bar, minimap and LAN markers drawn natively.
5. PSP (sceGu) and 3DS (citro3d) backends.

## Split screen and per-player views (plan, Oct 31)

Requirements (user): split screen at any aspect ratio; robust enough for one window per player;
local play follows the game's cameras (one shared view while the game has both players on one
screen, split when it splits); network play shows each player a view centred on themselves
(drawing only, the game's logic unchanged); the game's own shared status bar; for network play a
device draws only its own player's view. Split layout in one window: the original's (stacked).

### Audit: how the game draws a frame (Oct 31)

Frame pipeline (all in the main loop, `LOC_0000CC62` "DrewLevelOnScreenRoutineB" and callees):
- The main loop runs at 30 passes a second (InGameLoop alternates two kinds of pass). Objects move
  on one pass; the sprite table copy at $FFB65A (80 x 8 bytes) is DMA'd to VRAM $F800 at VBlank
  every other frame. In between, RAM is half updated, which is why the view reads a RAM snapshot
  taken at that DMA (`gameview_sat_dma`).
- Animation (`LOC_0000CCAC`..): per object, $28 counts down from $27; the next entry of the
  animation list $1A (bit 7 = flip, $FF loop, $FE stop) picks a frame from the table at $1E; $22
  is set. **VRAM budget:** objects that have sprites add their pattern size $2B to a per-pass total
  and stop animating for this pass once it would pass $70; objects with no sprites ($4A = 0) always
  animate. (Logic; unchanged by any view.)
- Graphics: `LOC_0000C900` unpacks the frame into the object's VRAM pattern slot, once per camera
  that shows it ($2F bit 1 = camera 0, bit 2 = camera 1). Patterns are per camera.
- Order: `LOC_0000C690` builds the list at $FFD938 linked by $5A: objects 0-25 (0-39 on the last
  floor), sorted by Y (lower in front), those with $2F bit 7 after all others.
- Sprite table: `LOC_0000C222` walks that list and writes each object's hardware sprites, first for
  camera 0 (slots $30.., shadow $48, position minus TOEJAM_CAMERA_NEW) then camera 1 (slots $3C..,
  shadow $49, minus EARL_CAMERA_NEW). Attached objects ($4D bit 3) take their position and priority
  from the object in $56 plus offsets $57-$59 here (`LOC_0000C150`). Hidden ($4D bit 6) objects
  are written off screen (`LOC_0000D5F2`).
- While a player is paused (VBLANK_FLAG_1 + player), only objects 26-39 are processed (the second
  list, $FFD93C, built in SetupPlayer: the pause sign and the present-list icons) and written by
  `LOC_0000CE3C`.

Sprite table layout (SetupPlayer): entry 0 is the head; entries 1-10 are a fixed band of ten 32x8
sprites at Y 108 across the screen (tile $5A9, every other one flipped): the split-screen divider,
and the hint/message box the game shows in the middle of the screen when not split (seen at Y 108
in one-player play); entries 11-23 are the status bar icons (written by the status-bar routines,
`LOC_0000B868` etc., also during the frame); objects' sprites are allocated from the rest.

Split screen (`VHBLANK_FLAG_1` $FF8010): ToeJam's half shows plane A at $A000 with his camera;
HBlankJSR at line 112 switches plane A to $C000 (Earl's, streamed separately by
UpdateLandAroundPlayer) and the scroll to Earl's camera (V minus $70), plane B at half speed in each
half. The status bar (window) is shared. The on-screen test (`LOC_0000BDB8`) uses 112 lines;
LoadInEarthlings counts Earl's camera only while split.

Sprites that are not objects' (measured over random play on six floors, with the writer traced):
- status bar icons (entries 11-23; `LOC_0000B608`/`LOC_0000B93C` and the HUD routines
  `LOC_0000AA3A`..`LOC_0000B408`), at the status bar, or under the present list when it is open;
- the present-list icons (objects 26-39, pause/present screens);
- the band at Y 108 (divider / message text);
- the present-list and present-opening icons (tiles $2A8-$358): objects 26-39 of the second list
  ($FFD93C), written by the object code (`LOC_0000CC62` -> `LOC_0000CE3C`) while a player is paused
  or the list is open; they have no $2F camera bits, so they are not counted as objects' sprites.
None of these is tied to a world position: per view they belong to that view's screen (the band,
messages) or to the status bar.

More of the game's drawing, found while closing the differences (Oct 31):
- At the status bar's line (H-interrupt, register 10 = $B6) `LOC_0000CFEE` relinks sprite-table
  entry 23 straight to entry 79, cutting every object's sprite out below it; in split screen the
  H-interrupt at line 112 (`LOC_0000D002`/`LOC_0000D02E`) switches the chain to camera 1's sprites.
  The VBlank restores the chain (`LOC_0000CF9A`).
- Blinking players ($4D bit 4, after a hit): at each VBlank `LOC_0000D132` writes Y = 1 into one
  player's sprites in VRAM (alternating players) until the next table upload.
- Some graphics exist only in VRAM: speech-bubble text is written there by the game, effects
  (wading, flashing) are applied there, and a new animation frame's graphics can arrive a few frames
  after $22 changes (upload budget). So an object the game shows on a camera is drawn from its
  graphics in VRAM (tile, palette, flip from its own sprite-table entries); only objects the game
  has not given sprites use the frame data in ROM.
- An object the game has just created can have $4A = 0 while its entries are already linked in the
  table it sent: shown.
- Attached objects (speech bubbles, $4D bit 3) are placed as `LOC_0000C150` does: the object in $56
  plus offsets $57-$59.
- Shadows: only for objects with $2F bit 6; size and place from the object's own shadow entry ($48 /
  $49) when it has one (the game varies it, e.g. with height); objects too high or low (|Z| > $E0)
  keep their shadow without their sprites. Floor items get no shadow.

Parity (`TJE_GV_PIECES=1 --gvcheck`: every object drawn by the view instead of the hardware sprites,
compared with the VDP; at 320x224 only what the game shows, and its status-bar cut, so the drawing
itself is compared). Random play on eight floors plus a 2-player run with split screen:

| | frames | split | differing frames | pixels |
|---|---|---|---|---|
| before (one-frame snapshot only) | ~27,000 | 0 | 3,500+ | 1,000,000+ |
| now | 29,817 | 2,554 | 64 | 30,000 |

The 64 left are single moments: an animation frame whose graphics the game uploads late on a frame
our snapshot already has, a present thrown with a bounce, a yes/no prompt's first frame, the first
frame after a state load (no snapshot yet). Split screen: 918 split frames, 2 differing (64 px).

What a per-view renderer needs from this:
1. Everything from frame data (no hardware sprites): close the three differences above.
2. Per view: its camera (game camera 0/1, or a private one for network play), its floor, plane B
   parallax at half speed from its camera, and the screen-space sprites of its half (band/messages
   at its centre line, pause sign).
3. The status bar drawn from the window plane's tiles in VRAM, plus its icon sprites (entries
   11-23), as one unit, once per window.
4. Split screen: each half is a view; the divider band is drawn between them by us.

### Plan

1. Audit (renderer parity): every writer of the sprite table and what it draws, in which view;
   every split-screen difference (above); what the game draws per player that is not an object.
2. A view renderer independent of the VDP's lines: `view_render(player, camera, size)` draws plane B
   (with that view's parallax), the level from the floor layout, objects and items from their frame
   data in the game's order (snapshot at the sprite-table DMA), shadows, effects, and the non-object
   sprites from step 1 placed per view; the status bar from the window plane's tiles in VRAM.
   Checked against the VDP: 1 player at 320x224, and each 320x112 half in split screen, pixel for
   pixel (`--gvcheck`).
3. Layouts: one view (1P, or 2P sharing a camera), two views stacked (split), two windows (one per
   player; SDL second window), network mode (one view, own player). Per-player cameras for network
   mode: the game's camera-follow rule run on a private camera per player (drawing only).
4. Performance: draw only the views shown; skip the VDP's line rendering while the native views are
   up (still checked in the tests).
