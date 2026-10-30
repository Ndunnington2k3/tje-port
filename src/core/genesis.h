/* The Genesis machine model that the translated game code runs against. */
#ifndef TJE_GENESIS_H
#define TJE_GENESIS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Controller buttons (bit set = pressed) */
enum {
    PAD_UP = 1 << 0, PAD_DOWN = 1 << 1, PAD_LEFT = 1 << 2, PAD_RIGHT = 1 << 3,
    PAD_B = 1 << 4, PAD_C = 1 << 5, PAD_A = 1 << 6, PAD_START = 1 << 7,
    PAD_BACK = 1 << 8,         /* port only (Escape / controller Back): not a Genesis button */
    /* menu controls (g_menu_pad): fixed, not rebindable; directions use PAD_UP..PAD_RIGHT */
    MENU_OK = 1 << 9,          /* Enter / Space, controller A or Start */
    MENU_BACK = 1 << 10        /* Escape / Backspace, controller B or Back */
};

#define GEN_LINES_NTSC     262
#define GEN_CYCLES_PER_LINE 488

/* Load the original ROM image; returns 0 on success. */
int genesis_load_rom(const char *path);

/* Load a Gens save state (.gsx/.gs0) taken during gameplay. genesis_run() will then
 * restore it and enter the game's main loop instead of booting. Returns 0 on success. */
int genesis_load_state(const char *path);

/* Reset the machine and run the game. Returns when the platform asks to quit
 * or after a fatal error. Return value: 0 = quit, nonzero = error. */
int genesis_run(void);

/* Port data in ROM space: copies data (strings, menu tables...) into an area just
 * after the game's ROM, so game routines can be handed 68000 pointers to it.
 * Returns its 68000 address (0 if full). genesis_port_rom_reset() frees it all. */
uint32_t genesis_port_rom(const void *data, uint32_t size);
void genesis_port_rom_reset(void);
/* Write access to port ROM data placed earlier (NULL if addr is not in it). */
uint8_t *genesis_port_rom_ptr(uint32_t addr);

/* Port code to run at the start of every frame, just before line 0 is drawn
 * (after the game's VBlank work), e.g. to put the port's own graphics in VRAM.
 * NULL to stop. Cleared when the machine restarts. */
void genesis_set_frame_hook(void (*fn)(void));
/* Called after the VDP has drawn each visible line (0-223), with the machine as it
 * is at that moment: for renderers that draw the screen themselves. */
void genesis_set_line_hook(void (*fn)(int line));
/* Where each frame's picture comes from: fn returns 1 with a picture of its own
 * (e.g. the gameplay view), or 0 to show the VDP's. */
void genesis_set_picture_source(int (*fn)(const uint32_t **pixels, int *w, int *h, int *pitch));

/* Machine snapshots: the complete machine (CPU registers, RAM, VDP, sound) as a
 * portable byte blob. Taken by game code at a known point (the top of the main
 * game loop), restored by restarting from it.
 *   genesis_snapshot(NULL, 0) returns the size; otherwise it writes the blob and
 *   returns its length (0 if cap is too small). */
size_t genesis_snapshot(uint8_t *buf, size_t cap);
/* Where the 64 KB of work RAM ($FF0000-$FFFFFF) sits inside a snapshot blob. */
size_t genesis_snapshot_ram_offset(void);
int genesis_snapshot_valid(const uint8_t *blob, size_t len);
/* Abandon the running game and restart: from power-on, or from a snapshot taken at
 * the top of the main game loop (it is copied; the call does not return). */
void genesis_restart_boot(void);
/* Stop the game: genesis_run() returns 0 (does not return here). */
void genesis_quit(void);
void genesis_restart_snapshot(const uint8_t *blob, size_t len);

/* Called by the runtime when the cycle count reaches the next scanline. */
void genesis_run_events(void);
/* Deliver any pending interrupt that the current mask allows. */
void genesis_check_interrupts(void);

/* Called after the platform reads the controllers each frame, before the game
 * sees them (e.g. to turn PAD_BACK into Start during play). NULL for none. */
void genesis_set_input_hook(void (*fn)(uint16_t pad[2]));

/* Controllers: current button state for pad 0/1 (PAD_* bits). */
extern uint16_t g_pad[2];
/* Menu controls this frame (plat_menu_input): PAD_UP..PAD_RIGHT, MENU_OK, MENU_BACK. */
extern uint16_t g_menu_pad;
extern uint32_t g_frame;

void genesis_log(const char *fmt, ...);
void genesis_fatal(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif
