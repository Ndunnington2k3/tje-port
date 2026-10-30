/*
 * Platform interface. Each target (SDL2 desktop, PSP, 3DS, headless test)
 * implements these functions; nothing else in the game talks to the OS.
 */
#ifndef TJE_PLATFORM_H
#define TJE_PLATFORM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Create the window / screen. Returns 0 on success. */
int plat_init(int argc, char **argv);
void plat_shutdown(void);

/* Show one finished frame (0x00RRGGBB pixels) and pace to ~60 fps. */
void plat_present(const uint32_t *pixels, int width, int height, int pitch);

/* Read input into pad[0..1] (PAD_* bits from core/genesis.h). Returns 0 when the user quits. */
int plat_poll(uint16_t pad[2]);

/* Audio: return nonzero if this platform wants sound (emulating it costs CPU). */
int plat_audio_enabled(void);
/* Queue one frame's worth of audio: interleaved stereo int16 at 48 kHz. */
void plat_audio(const int16_t *samples, int frames);

/* Menu controls, fixed (not the game's bindings): any keyboard or controller.
 * Arrows / d-pad / left stick move (PAD_UP..PAD_RIGHT); Enter, Space,
 * controller A or Start = MENU_OK; Escape, Backspace, controller B or Back =
 * MENU_BACK. Nothing while a binding is being captured. */
uint16_t plat_menu_input(void);

/* ---- input bindings ------------------------------------------------------
 * Each game action of each player (button = bit number:
 * 0 Up, 1 Down, 2 Left, 3 Right, 4 Present List, 5 Map, 6 Action, 7 Pause;
 * with the game's standard controller layout these are the Genesis B, C, A and
 * Start bits) can have one keyboard binding and one controller binding, stored as text tokens ("" = none) so the
 * game can keep them in its settings file. */
#define PLAT_BIND_KEY 0
#define PLAT_BIND_PAD 1
const char *plat_binding(int player, int button, int slot);
int plat_set_binding(int player, int button, int slot, const char *token);   /* 0 = ok */
void plat_default_bindings(int player);
/* A short name to show for a token, e.g. "Z", "Pad A" ("--" for none). */
void plat_binding_label(int slot, const char *token, char *out, int size);
/* Capture the next key or controller input for player `player`'s bindings.
 * While capturing, the game sees no input. plat_capture_poll() returns 1 once
 * something was pressed (*slot = PLAT_BIND_KEY/PAD, token filled in), or -1 if
 * the capture was cancelled (Escape), else 0. */
void plat_capture_start(int player);
int plat_capture_poll(int *slot, char *token, int size);
void plat_capture_cancel(void);

/* Folder for settings and saved games ("" = current folder), ending in a separator. */
const char *plat_data_dir(void);

/* Show an error/info message to the user. */
void plat_message(const char *title, const char *text);

/* Path of the ROM to load (from command line, config, or a default). */
const char *plat_rom_path(void);

#ifdef __cplusplus
}
#endif

#endif
