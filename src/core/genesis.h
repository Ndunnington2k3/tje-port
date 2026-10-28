/* The Genesis machine model that the translated game code runs against. */
#ifndef TJE_GENESIS_H
#define TJE_GENESIS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Controller buttons (bit set = pressed) */
enum {
    PAD_UP = 1 << 0, PAD_DOWN = 1 << 1, PAD_LEFT = 1 << 2, PAD_RIGHT = 1 << 3,
    PAD_B = 1 << 4, PAD_C = 1 << 5, PAD_A = 1 << 6, PAD_START = 1 << 7
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

/* Called by the runtime when the cycle count reaches the next scanline. */
void genesis_run_events(void);
/* Deliver any pending interrupt that the current mask allows. */
void genesis_check_interrupts(void);

/* Controllers: current button state for pad 0/1 (PAD_* bits). */
extern uint16_t g_pad[2];
extern uint32_t g_frame;

void genesis_log(const char *fmt, ...);
void genesis_fatal(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif
