#include "sound.h"
#include "m68k_rt.h"
#include "genesis.h"

#include "z80.h"
#include "ym3438.h"

#include <string.h>

/* Clocks (master M = 53.69 MHz; 68000 = M/7):
 *   FM chip: input M/7, internal cycle M/42 -> 1/6 per 68k cycle; 24 per sample (53,267 Hz)
 *   Z80:     M/15                         -> 7/15 per 68k cycle
 *   PSG:     M/15/16 = 223,722 Hz         -> ~4.2 ticks per FM sample */
#define YM_SAMPLE_CYCLES 24

/* The Z80 always runs: the game reads its status bytes (speech playing etc.), so
 * gameplay must not depend on whether sound is on. Only FM/PSG synthesis and
 * output are optional (they are the expensive part). The driver never reads the
 * FM chip's status, so skipping FM synthesis does not change what the Z80 does. */
static int enabled;

static z80 cpu;
static uint8_t z80_ram[0x2000];
static uint32_t z80_bank;          /* 68k address of the $8000 window (bits 15-23) */
static int z80_busreq, z80_reset_line;
static uint32_t z80_pos;           /* 68k cycle the Z80 has run up to */
static int32_t z80_budget;         /* Z80 cycles still owed (x15, may go negative) */

static ym3438_t ym;
static uint32_t ym_pos;            /* 68k cycle the FM chip has been clocked up to */
static uint32_t ym_frac;           /* remainder in 68k cycles (0..5) */
static uint32_t ym_debt;           /* extra cycles already clocked between back-to-back writes */
static int ym_slot;
static int32_t ym_acc[2];

/* PSG */
static uint16_t psg_tone[3], psg_count[3];
static uint8_t psg_vol[4], psg_out[3], psg_latch, psg_noise_ctrl;
static uint16_t psg_noise_count, psg_lfsr = 0x8000;
static uint32_t psg_frac;

/* 53 kHz native buffer for the current frame, then resampled to 48 kHz */
#define NATIVE_MAX 4096
static int16_t native_buf[NATIVE_MAX * 2];
static int native_len;
static int16_t out_buf[NATIVE_MAX * 2];
static uint32_t resample_pos;     /* 16.16 position in the native stream */
static int16_t last_sample[2];
static int32_t dc_x[2], dc_y[2];   /* DC-blocking high-pass state (x256) */

static const int16_t psg_volume[16] = {
    2048, 1627, 1292, 1026, 815, 647, 514, 408, 324, 257, 204, 162, 129, 102, 81, 0
};

int snd_enabled(void) { return enabled; }

/* ---- PSG ---------------------------------------------------------------- */
static void psg_step(int ticks)
{
    int i;
    while (ticks-- > 0) {
        for (i = 0; i < 3; i++) {
            if (psg_count[i]) psg_count[i]--;
            if (psg_count[i] == 0) {
                psg_count[i] = psg_tone[i] ? psg_tone[i] : 1;
                psg_out[i] ^= 1;
            }
        }
        if (psg_noise_count) psg_noise_count--;
        if (psg_noise_count == 0) {
            int rate = psg_noise_ctrl & 3;
            uint16_t fb;
            psg_noise_count = rate == 3 ? (psg_tone[2] ? psg_tone[2] : 1) * 2 : (uint16_t)(0x20 << rate);
            fb = (psg_noise_ctrl & 4) ? ((psg_lfsr ^ (psg_lfsr >> 3)) & 1) : (psg_lfsr & 1);
            psg_lfsr = (uint16_t)((psg_lfsr >> 1) | (fb << 15));
        }
    }
}

static int psg_sample(void)
{
    int s = 0, i;
    for (i = 0; i < 3; i++) s += psg_out[i] ? psg_volume[psg_vol[i]] : -psg_volume[psg_vol[i]];
    s += (psg_lfsr & 1) ? psg_volume[psg_vol[3]] : -psg_volume[psg_vol[3]];
    return s / 2;
}

void snd_psg_write(uint32_t v)
{
    v &= 0xFF;
    if (v & 0x80) {
        psg_latch = (uint8_t)((v >> 4) & 7);
        if (psg_latch & 1) psg_vol[psg_latch >> 1] = v & 0x0F;
        else if ((psg_latch >> 1) == 3) { psg_noise_ctrl = v & 7; psg_lfsr = 0x8000; }
        else { int c = psg_latch >> 1; psg_tone[c] = (uint16_t)((psg_tone[c] & 0x3F0) | (v & 0x0F)); }
    } else {
        if (psg_latch & 1) psg_vol[psg_latch >> 1] = v & 0x0F;
        else if ((psg_latch >> 1) < 3) { int c = psg_latch >> 1; psg_tone[c] = (uint16_t)((psg_tone[c] & 0x0F) | ((v & 0x3F) << 4)); }
        else { psg_noise_ctrl = v & 7; psg_lfsr = 0x8000; }
    }
}

/* ---- FM chip ------------------------------------------------------------ */
static void ym_emit_sample(void)
{
    int s0, s1, p;
    if (native_len >= NATIVE_MAX) return;
    psg_frac += 4194;            /* PSG ticks per FM sample = 144/240 * 16/... ~ 4.2 (x1000) */
    psg_step((int)(psg_frac / 1000));
    psg_frac %= 1000;
    p = psg_sample();
    s0 = ym_acc[0] * 6 + p;
    s1 = ym_acc[1] * 6 + p;
    if (s0 > 32767) s0 = 32767; else if (s0 < -32768) s0 = -32768;
    if (s1 > 32767) s1 = 32767; else if (s1 < -32768) s1 = -32768;
    native_buf[native_len * 2] = (int16_t)s0;
    native_buf[native_len * 2 + 1] = (int16_t)s1;
    native_len++;
}

static void ym_sync(uint32_t to)
{
    int32_t delta = (int32_t)(to - ym_pos);
    uint32_t n;
    if (delta <= 0) return;
    ym_pos = to;
    ym_frac += (uint32_t)delta;
    n = ym_frac / 6;
    ym_frac %= 6;
    if (ym_debt) {
        uint32_t d = ym_debt < n ? ym_debt : n;
        ym_debt -= d;
        n -= d;
    }
    while (n--) {
        Bit16s buf[2];
        OPN2_Clock(&ym, buf);
        ym_acc[0] += buf[0];
        ym_acc[1] += buf[1];
        if (++ym_slot == YM_SAMPLE_CYCLES) {
            ym_slot = 0;
            ym_emit_sample();
            ym_acc[0] = ym_acc[1] = 0;
        }
    }
}

static uint8_t ym_read(uint32_t port)
{
    /* Busy flag is cleared: polling loops would otherwise wait on time that only
     * advances between scanlines. Timer flags are real. */
    return (uint8_t)(OPN2_Read(&ym, port & 3) & 0x7F);
}

static void ym_write(uint32_t port, uint32_t v, uint32_t when)
{
    ym_sync(when);
    OPN2_Write(&ym, port & 3, (Bit8u)v);
    /* give the chip a few cycles to latch it before any following write */
    {
        Bit16s buf[2];
        int i;
        ym_debt += 2;
        for (i = 0; i < 2; i++) {
            OPN2_Clock(&ym, buf);
            ym_acc[0] += buf[0];
            ym_acc[1] += buf[1];
            if (++ym_slot == YM_SAMPLE_CYCLES) { ym_slot = 0; ym_emit_sample(); ym_acc[0] = ym_acc[1] = 0; }
        }
    }
}

/* ---- Z80 ---------------------------------------------------------------- */
static uint32_t z80_now68(void)
{
    /* Z80 time converted back to 68k cycles: z80 cycles * 15/7 */
    return z80_pos;
}

static uint8_t z80_read(void *ud, uint16_t a)
{
    (void)ud;
    if (a < 0x4000) return z80_ram[a & 0x1FFF];
    if (a < 0x6000) { if (!enabled) return 0; ym_sync(z80_now68()); return ym_read(a); }
    if (a >= 0x8000) return (uint8_t)rd8(z80_bank | (a & 0x7FFF));
    return 0xFF;
}

static void z80_write(void *ud, uint16_t a, uint8_t v)
{
    (void)ud;
    if (a < 0x4000) { z80_ram[a & 0x1FFF] = v; return; }
    if (a < 0x6000) { if (enabled) ym_write(a, v, z80_now68()); return; }
    if (a < 0x6100) { z80_bank = ((z80_bank >> 1) | ((uint32_t)(v & 1) << 23)) & 0xFF8000u; return; }
    if (a == 0x7F11 || a == 0x7F13 || a == 0x7F15 || a == 0x7F17) { snd_psg_write(v); return; }
    if (a >= 0x8000) wr8(z80_bank | (a & 0x7FFF), v);
}

static uint8_t z80_in(z80 *z, uint8_t p) { (void)z; (void)p; return 0xFF; }
static void z80_out(z80 *z, uint8_t p, uint8_t v) { (void)z; (void)p; (void)v; }

static int z80_running(void) { return !z80_busreq && !z80_reset_line; }

/* Run the Z80 up to 68k cycle `to`. */
static void z80_sync(uint32_t to)
{
    uint32_t from = z80_pos;
    int32_t delta = (int32_t)(to - from);
    int32_t total;
    if (delta <= 0) return;
    if (!z80_running()) { z80_pos = to; z80_budget = 0; return; }
    /* budget in Z80 cycles x15: delta 68k cycles * 7 */
    z80_budget += delta * 7;
    total = z80_budget;
    while (z80_budget > 0) {
        unsigned long c0 = cpu.cyc;
        int32_t used;
        z80_step(&cpu);
        used = (int32_t)(cpu.cyc - c0);
        if (used <= 0) used = 4;
        z80_budget -= used * 15;
        /* current Z80 time in 68k cycles, so FM writes land at the right moment */
        z80_pos = from + (uint32_t)((int64_t)(total - z80_budget) * delta / (total > 0 ? total : 1));
        if ((int32_t)(z80_pos - to) > 0) z80_pos = to;
    }
    z80_pos = to;
}

/* ---- 68000-side interface ------------------------------------------------ */
uint32_t snd_z80_area_read(uint32_t a)
{
    a &= 0xFFFF;
    if (a < 0x4000) { z80_sync(R.cycles); return z80_ram[a & 0x1FFF]; }
    if (a < 0x6000) {
        z80_sync(R.cycles);
        if (!enabled) return 0;
        ym_sync(R.cycles);
        return ym_read(a);
    }
    return 0xFF;
}

void snd_z80_area_write(uint32_t a, uint32_t v)
{
    a &= 0xFFFF;
    if (a < 0x4000) { z80_sync(R.cycles); z80_ram[a & 0x1FFF] = (uint8_t)v; return; }
    if (a < 0x6000) {
        z80_sync(R.cycles);
        if (enabled) ym_write(a, v, R.cycles);
        return;
    }
    if (a < 0x6100) { z80_bank = ((z80_bank >> 1) | ((uint32_t)(v & 1) << 23)) & 0xFF8000u; return; }
    if (a == 0x7F11) snd_psg_write(v);
}

void snd_busreq_write(uint32_t v)
{
    z80_sync(R.cycles);
    z80_busreq = (v & 0x100) != 0;
}

uint32_t snd_busreq_read(void)
{
    /* bit 0 (of the high byte) is 0 once the 68000 owns the bus */
    if (z80_busreq || z80_reset_line) return 0;
    return 1;
}

void snd_reset_write(uint32_t v)
{
    int assert_reset = (v & 0x100) == 0;
    z80_sync(R.cycles);
    if (assert_reset && !z80_reset_line) {
        z80_init(&cpu);
        cpu.read_byte = z80_read;
        cpu.write_byte = z80_write;
        cpu.port_in = z80_in;
        cpu.port_out = z80_out;
        if (enabled) { ym_sync(R.cycles); OPN2_Reset(&ym); }
    }
    z80_reset_line = assert_reset;
}

void snd_line(int line)
{
    z80_sync(R.cycles);
    if (enabled) ym_sync(R.cycles);
    if (line == 225) cpu.int_pending = 0;   /* INT line is only held for about one line */
}

void snd_vint(void)
{
    z80_sync(R.cycles);
    if (z80_running()) z80_gen_int(&cpu, 0xFF);
}

int snd_end_frame(const int16_t **out)
{
    /* resample native (53,267 Hz) -> 48 kHz with linear interpolation */
    const uint32_t step = (uint32_t)((53267.0 / SND_OUTPUT_RATE) * 65536.0);
    int n = 0;
    if (!enabled) { *out = out_buf; return 0; }
    ym_sync(R.cycles);
    while (n < NATIVE_MAX) {
        uint32_t i = resample_pos >> 16;
        uint32_t f = resample_pos & 0xFFFF;
        int16_t a0, a1, b0, b1;
        if (i + 1 > (uint32_t)native_len) break;
        a0 = i == 0 ? last_sample[0] : native_buf[(i - 1) * 2];
        a1 = i == 0 ? last_sample[1] : native_buf[(i - 1) * 2 + 1];
        b0 = native_buf[i * 2];
        b1 = native_buf[i * 2 + 1];
        {
            int c;
            int32_t v[2];
            v[0] = a0 + (((int32_t)(b0 - a0) * (int32_t)f) >> 16);
            v[1] = a1 + (((int32_t)(b1 - a1) * (int32_t)f) >> 16);
            for (c = 0; c < 2; c++) {
                /* y = x - x_prev + 0.995 * y_prev  (~40 Hz corner at 48 kHz) */
                int32_t x = v[c] * 256;
                int32_t y = x - dc_x[c] + (int32_t)(((int64_t)dc_y[c] * 1019) >> 10);
                dc_x[c] = x;
                dc_y[c] = y;
                y >>= 8;
                if (y > 32767) y = 32767; else if (y < -32768) y = -32768;
                out_buf[n * 2 + c] = (int16_t)y;
            }
        }
        n++;
        resample_pos += step;
    }
    /* carry the fractional position and last sample into the next frame */
    if (native_len > 0) {
        uint32_t consumed = resample_pos >> 16;
        if (consumed > (uint32_t)native_len) consumed = (uint32_t)native_len;
        last_sample[0] = native_buf[(native_len - 1) * 2];
        last_sample[1] = native_buf[(native_len - 1) * 2 + 1];
        resample_pos -= consumed << 16;
        native_len = 0;
    }
    *out = out_buf;
    return n;
}

void snd_init(int on)
{
    enabled = on;
    OPN2_SetChipType(ym3438_mode_ym2612);
    snd_reset();
}

void snd_reset(void)
{
    memset(z80_ram, 0, sizeof z80_ram);
    z80_init(&cpu);
    cpu.read_byte = z80_read;
    cpu.write_byte = z80_write;
    cpu.port_in = z80_in;
    cpu.port_out = z80_out;
    z80_bank = 0;
    z80_busreq = 0;
    z80_reset_line = 1;
    z80_pos = ym_pos = R.cycles;
    z80_budget = 0;
    ym_frac = 0;
    ym_debt = 0;
    OPN2_Reset(&ym);
    ym_slot = 0;
    ym_acc[0] = ym_acc[1] = 0;
    memset(psg_tone, 0, sizeof psg_tone);
    memset(psg_count, 0, sizeof psg_count);
    memset(psg_out, 0, sizeof psg_out);
    memset(psg_vol, 0x0F, sizeof psg_vol);
    psg_latch = 0; psg_noise_ctrl = 0; psg_noise_count = 0; psg_lfsr = 0x8000; psg_frac = 0;
    native_len = 0;
    resample_pos = 0;
    last_sample[0] = last_sample[1] = 0;
    dc_x[0] = dc_x[1] = dc_y[0] = dc_y[1] = 0;
}

void snd_load_state(const uint8_t *zram, const uint8_t *regs)
{
    int port, r;
    memcpy(z80_ram, zram, sizeof z80_ram);
    z80_init(&cpu);
    cpu.read_byte = z80_read;
    cpu.write_byte = z80_write;
    cpu.port_in = z80_in;
    cpu.port_out = z80_out;
    z80_busreq = 0;
    z80_reset_line = 0;
    z80_pos = ym_pos = R.cycles;
    z80_budget = 0;
    if (!enabled) return;
    OPN2_Reset(&ym);
    for (port = 0; port < 2; port++)
        for (r = 0x21; r < 0xB8; r++) {
            if (r == 0x28 || (r >= 0x29 && r < 0x30)) continue;  /* no key-on / DAC / timers */
            if ((r & 3) == 3 && r >= 0x30) continue;
            ym_write((uint32_t)port * 2, (uint32_t)r, R.cycles);
            ym_write((uint32_t)port * 2 + 1, regs[port * 256 + r], R.cycles);
        }
}
