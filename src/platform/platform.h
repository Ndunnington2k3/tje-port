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

/* Show an error/info message to the user. */
void plat_message(const char *title, const char *text);

/* Path of the ROM to load (from command line, config, or a default). */
const char *plat_rom_path(void);

#ifdef __cplusplus
}
#endif

#endif
