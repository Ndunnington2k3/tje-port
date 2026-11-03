# Network play

Two copies of the game on a local network: ToeJam on the machine that hosts,
Earl on the one that joins. `src/game/net.c` (protocol, lockstep), `src/game/lan_menu.c`
(menus), `src/platform/common/net_udp.c` (PC: UDP). Tests: `tools/net_test.py`.

## Menus

Who screen: the three original lines, Load Game, **Network Play**, Options (six lines,
evenly spaced where the original three were centred).

Network Play ("the LAN menu"):
- **Host Game**: name the room (the system's name to start with: the computer's name
  on PC, the console's on PSP/3DS), Open Room. In the room: "Waiting for a player to
  join..." until someone does, then **Start New Game** (the What screen, as usual) or
  **Load Game** (saved games of both players). Close Room ends it.
- **Join Game**: the rooms found on the local network (refreshed every second; a room
  with another version of the game says so). Choosing one waits in it until the host
  starts; then the game arrives ("Getting the game... n%") and play begins.

Naming: type on a keyboard, or with a controller: Left/Right move along the name,
Up/Down change the letter, B deletes. The game's font has letters (one case), digits
and `- ! . , '`.

## How the two machines stay the same

**Lockstep on inputs.** Both machines run the whole game, frame for frame, from the
same starting point, on the same inputs. Each frame a machine samples its own player's
controls and sends them; they are used `delay` frames later (3 frames, 50 ms) on both
machines. A frame is finished only with both players' inputs for it: a machine without
them waits, polling every millisecond for up to a frame's time before it shows the frame
again (after 0.4 s it says why).

**Keeping pace.** Both machines show 60 frames a second, but a frame that takes too long
(a busy PC) puts that machine a frame behind, for good: lag like that used to pile up
until the other machine had to wait (repeated frames, then a burst of skipped ones: a
stutter). A machine 2 or more frames behind the other now runs one frame without showing
it, and is back in step; the other machine never notices. The same catches a joiner up
after a snapshot (many frames at once). Only inputs travel: 15 bits per frame (the game's
buttons and the menu controls), each packet carrying every input the other side has
not yet confirmed (up to 40 frames), so a lost packet costs nothing and nothing is ever
sent again on request.

**The same starting point.** The game starts on the host. At the top of the main game
loop (InGameLoop, `save.c`) the host takes a machine snapshot (CPU, RAM, VDP, sound) and
sends it with the inputs since: a *package*, compressed (zero runs), in 1000-byte
pieces, each sent again until the joiner reports it (GOT). The joiner restarts its
machine from it (`genesis_restart_snapshot`); `g_net_resume` makes the main loop carry
on from the snapshot's point rather than start afresh. About 50 KB.

What both machines must agree on, and why they do:
- Game code, ROM and machine model: the version check (protocol, `NET_BUILD`, snapshot
  size, a checksum of the ROM) is in every room's answer; other versions cannot join.
  Change `NET_BUILD` whenever anything changes what the game does.
- Two-window mode (`g_gv_windows`), which changes how the game splits the screen
  (`transitions.c`): on for both during network play.
- Window size, sound on or off, port drawing options: they never change game memory
  (`tools/view_parity.py`; sound checked separately).
- The port's own input handling (the soft pause, the map that does not pause, Back as
  Start) acts on this machine's player before the inputs are sent: what is sent is what
  the game sees.

**Checking.** Every 32 frames each machine records a checksum of the game's 64 KB of
RAM; the joiner sends its latest with its inputs, the host compares. If they differ
(a bug: it should never happen), the host sends a new package from its latest snapshot
(taken every 120 frames) with all the inputs since; the joiner restarts from it and
catches up without showing the frames it replays. `TJE_NET_LOG=1` logs it.

## Leaving and coming back

- The joiner leaves (soft pause, quit) or is lost (nothing heard for 5 s): the host's
  game holds, with "earl left - waiting for earl to come back". The room is listed
  again ("join the game"). Whoever joins it gets a package: the latest snapshot and
  every input since (Earl's missing ones are empty), replays to the present and plays
  on as Earl. Back on the host quits to the menu instead.
- The host leaves or is lost: the joiner goes back to the Who screen with a message.
- The game ends (game over, the ending): both reach the Who screen together, and the
  session ends there.

## Packets

All little-endian, at most 1200 bytes (PSP ad hoc PDP and 3DS local wireless take
about 1400). Header: `'T' 'J' protocol type session(4)`.

| type | from | content |
|---|---|---|
| DISCOVER | joiner, broadcast | |
| ROOM | host | name(16) state version-check(4) |
| JOIN | joiner | version-check(4); repeated until WELCOME |
| WELCOME | host | result (ok / full / other version), delay |
| KEEP | both, in the room | every 250 ms |
| CHUNK | host | package id(4) length(4) piece(2) data(<=1000) |
| GOT | joiner | package id(4) pieces(2) bitmap |
| INPUT | both, in the game | player first-frame(4) count inputs(2 each) have-up-to(4) checksum-frame(4) checksum(4) |
| BYE | both | |

## Platforms

`plat_net_*` (platform.h) is datagrams only: open, send, broadcast, receive (never
blocking), a system name, a millisecond clock. PC: UDP on the first free port of
41290-41297 (several copies on one machine), broadcasts to 255.255.255.255 and
127.0.0.1. PSP (ad hoc, PDP with the matching library for rooms) and 3DS (local
wireless, UDS) implement the same calls; the two cannot talk to each other.

## Testing

`tools/net_test.py --exe build/tje_headless --rom tje.bin` runs two headless copies
(`--realtime`) through the menus and plays with random inputs; scenarios: plain,
loss (25% of packets dropped, `TJE_NET_LOSS`), jitter (frames take up to `JIT` ms longer
at random, `TJE_JITTER`; `--realtime` shows frames on a shared 60 Hz clock like vsync on one
monitor), rejoin (the joiner vanishes, a third
copy joins the game in progress), mend (the joiner's memory is changed: the host must
notice and mend it). Every scenario compares the checksum logs (`TJE_NET_HASHLOG`).
`TJE_NET_DELAY` sets the input delay. `TJE_NET_LOG=1` also reports, every 300 frames,
how often a machine waited, frames shown twice and frames not shown.
