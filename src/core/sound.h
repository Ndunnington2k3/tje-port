/*
 * Genesis sound hardware: YM2612 FM chip, SN76489 PSG and the Z80 that runs the
 * game's sound driver. This is the "emulated sound" backend; a native sound
 * routine can replace it later behind the same calls.
 *
 * Time is measured in 68000 cycles (R.cycles). The Z80 and FM chip catch up to
 * the 68000 whenever it touches them, and once per scanline.
 */
#ifndef TJE_SOUND_H
#define TJE_SOUND_H

#include <stdint.h>

#define SND_OUTPUT_RATE 48000

/* output: nonzero to synthesise FM/PSG audio. The Z80 sound driver runs either way,
 * so the game behaves identically with sound on or off. */
void snd_init(int output);
void snd_reset(void);
int snd_enabled(void);

/* 68000-side access to the sound hardware */
uint32_t snd_z80_area_read(uint32_t a);          /* $A00000-$A0FFFF */
void snd_z80_area_write(uint32_t a, uint32_t v);
void snd_busreq_write(uint32_t v);               /* $A11100 */
uint32_t snd_busreq_read(void);                  /* bit 0 set = Z80 still owns the bus */
void snd_reset_write(uint32_t v);                /* $A11200 */
void snd_psg_write(uint32_t v);                  /* $C00011 */

/* Restore from a save state: Z80 RAM (8 KB) and the FM registers (2 x 256).
 * The Z80 is restarted from address 0 with the bus released. */
void snd_load_state(const uint8_t *z80ram, const uint8_t *ym_regs);

/* Called by the machine model */
void snd_line(int line);      /* once per scanline */
void snd_vint(void);          /* VBlank: raises the Z80 interrupt */

/* Collect this frame's audio (interleaved stereo int16 at SND_OUTPUT_RATE).
 * Returns the number of stereo frames written into *out (valid until next call). */
int snd_end_frame(const int16_t **out);

#endif
