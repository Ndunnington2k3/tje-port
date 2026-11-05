/*
 * vgm2wav: a Genesis VGM (YM2612 and its DAC; .vgm, ungzipped) played through the
 * same FM chip as the game (Nuked OPN2, YM2612 mode) into a 44.1 kHz 16-bit stereo
 * WAV - the VGM's own time base, so its sample numbers are the WAV's.
 *
 *   vgm2wav in.vgm out.wav [tail_seconds]
 *
 * The file is played to its end and then on from its loop point for tail_seconds
 * more (default 2): the notes ringing at the end die away in the file, and the
 * loop is played from the same distance past the loop point (music/tracks.txt:
 * cut at end + tail, go on from loop + tail - the two places sound alike, and the
 * last 0.1 s before the cut is faded into what comes before the other). Prints
 * "loop LOOP_FROM LOOP_TO" for the table, then "total loop_start frames" (the
 * VGM's own figures).
 *
 * Build: cc -O2 -o vgm2wav tools/vgm2wav.c third_party/nuked-opn2/ym3438.c -Ithird_party/nuked-opn2 -lm
 */
#include "ym3438.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *vgm;
static long vgm_len;
static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

/* ---- the chip, clocked in master cycles ---- */
static ym3438_t chip;
static double clock_hz;                      /* the VGM's YM2612 clock */
static int64_t master_due;                   /* master cycles owed, x 44100 */
static int32_t acc[2];
static int slot, debt;
static int16_t *native;                      /* chip-rate output (clock / 144) */
static size_t native_len, native_cap;

static void chip_clock(int n)
{
    while (n-- > 0) {
        Bit16s b[2];
        OPN2_Clock(&chip, b);
        acc[0] += b[0];
        acc[1] += b[1];
        if (++slot == 24) {
            int c;
            slot = 0;
            if (native_len + 1 > native_cap) {
                native_cap = native_cap ? native_cap * 2 : 1 << 20;
                native = (int16_t *)realloc(native, native_cap * 4);
                if (!native) { fprintf(stderr, "vgm2wav: out of memory\n"); exit(1); }
            }
            for (c = 0; c < 2; c++) {
                int32_t v = acc[c] * 6;   /* (as the game's sound: sound.c ym_emit_sample) */
                native[native_len * 2 + c] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            }
            native_len++;
            acc[0] = acc[1] = 0;
        }
    }
}

/* wait n VGM samples (1/44100 s); a chip clock is 6 master cycles */
static void wait_samples(uint32_t n)
{
    int64_t clocks;
    master_due += (int64_t)n * (int64_t)clock_hz;
    clocks = master_due / (44100 * 6);
    master_due -= clocks * 44100 * 6;
    if (debt) { int d = debt < clocks ? debt : (int)clocks; debt -= d; clocks -= d; }
    chip_clock((int)clocks);
}

static void chip_write(int port, uint8_t reg, uint8_t val)
{
    OPN2_Write(&chip, (Bit32u)(port * 2), reg);
    chip_clock(2);
    OPN2_Write(&chip, (Bit32u)(port * 2 + 1), val);
    chip_clock(2);
    debt += 4;   /* (the clocks a write takes come out of the next wait) */
}

/* ---- 44.1 kHz from the chip's rate: windowed sinc ---- */
#define TAPS 32
#define XFADE 4410   /* (0.1 s) */
static double sinc(double x) { return x == 0 ? 1.0 : sin(M_PI * x) / (M_PI * x); }

static void write_wav(const char *path, const int16_t *pcm, uint32_t frames)
{
    FILE *f = fopen(path, "wb");
    uint8_t h[44];
    uint32_t data = frames * 4;
    if (!f) { perror(path); exit(1); }
    memcpy(h, "RIFF", 4);
    h[4] = (uint8_t)(36 + data); h[5] = (uint8_t)((36 + data) >> 8); h[6] = (uint8_t)((36 + data) >> 16); h[7] = (uint8_t)((36 + data) >> 24);
    memcpy(h + 8, "WAVEfmt ", 8);
    h[16] = 16; h[17] = h[18] = h[19] = 0;
    h[20] = 1; h[21] = 0; h[22] = 2; h[23] = 0;
    h[24] = 0x44; h[25] = 0xAC; h[26] = 0; h[27] = 0;           /* 44100 */
    h[28] = 0x10; h[29] = 0xB1; h[30] = 0x02; h[31] = 0;        /* 176400 */
    h[32] = 4; h[33] = 0; h[34] = 16; h[35] = 0;
    memcpy(h + 36, "data", 4);
    h[40] = (uint8_t)data; h[41] = (uint8_t)(data >> 8); h[42] = (uint8_t)(data >> 16); h[43] = (uint8_t)(data >> 24);
    fwrite(h, 1, 44, f);
    fwrite(pcm, 4, frames, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    FILE *f;
    uint32_t total, loop_ofs, loop_samples, data_ofs, pcm_pos = 0, played = 0, target;
    uint8_t *pcm_bank = NULL;
    uint32_t pcm_len = 0;
    long pos, loop_at;
    double tail = argc > 3 ? atof(argv[3]) : 2.0;
    int looped = 0;
    if (argc < 3) { fprintf(stderr, "usage: vgm2wav in.vgm out.wav [tail_seconds]\n"); return 2; }
    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    vgm_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    vgm = (uint8_t *)malloc((size_t)vgm_len);
    if (fread(vgm, 1, (size_t)vgm_len, f) != (size_t)vgm_len) { fprintf(stderr, "vgm2wav: can't read\n"); return 1; }
    fclose(f);
    if (vgm_len < 0x40 || memcmp(vgm, "Vgm ", 4)) { fprintf(stderr, "vgm2wav: not a VGM (gzipped? gunzip it)\n"); return 1; }
    total = le32(vgm + 0x18);
    loop_ofs = le32(vgm + 0x1C);
    loop_samples = le32(vgm + 0x20);
    clock_hz = le32(vgm + 0x2C) & 0x3FFFFFFF;
    data_ofs = le32(vgm + 8) >= 0x150 && le32(vgm + 0x34) ? 0x34 + le32(vgm + 0x34) : 0x40;
    if (!clock_hz) { fprintf(stderr, "vgm2wav: no YM2612 in this VGM\n"); return 1; }
    loop_at = loop_ofs ? (long)(0x1C + loop_ofs) : -1;
    target = total + (loop_at >= 0 ? (uint32_t)(tail * 44100) : 0);

    OPN2_SetChipType(ym3438_mode_ym2612);
    OPN2_Reset(&chip);
    pos = (long)data_ofs;
    while (played < target && pos < vgm_len) {
        uint8_t op = vgm[pos];
        if (op == 0x52 || op == 0x53) { chip_write(op - 0x52, vgm[pos + 1], vgm[pos + 2]); pos += 3; }
        else if (op == 0x61) { uint32_t n = vgm[pos + 1] | (vgm[pos + 2] << 8); if (played + n > target) n = target - played; wait_samples(n); played += n; pos += 3; }
        else if (op == 0x62 || op == 0x63) { uint32_t n = op == 0x62 ? 735 : 882; if (played + n > target) n = target - played; wait_samples(n); played += n; pos += 1; }
        else if (op >= 0x70 && op <= 0x7F) { uint32_t n = (op & 15) + 1u; if (played + n > target) n = target - played; wait_samples(n); played += n; pos += 1; }
        else if (op >= 0x80 && op <= 0x8F) {
            uint32_t n = op & 15u;
            chip_write(0, 0x2A, pcm_pos < pcm_len ? pcm_bank[pcm_pos] : 0x80);
            pcm_pos++;
            if (played + n > target) n = target - played;
            wait_samples(n);
            played += n;
            pos += 1;
        } else if (op == 0xE0) { pcm_pos = le32(vgm + pos + 1); pos += 5; }
        else if (op == 0x67) {
            uint32_t n = le32(vgm + pos + 3);
            if (vgm[pos + 2] == 0x00 && !pcm_bank) { pcm_bank = vgm + pos + 7; pcm_len = n; }
            pos += 7 + (long)n;
        } else if (op == 0x50) pos += 2;
        else if (op == 0x4F) pos += 2;
        else if (op == 0x66) {
            if (loop_at < 0) break;
            pos = loop_at;   /* (on from the loop point: the tail) */
            looped = 1;
        } else { fprintf(stderr, "vgm2wav: unknown command %02X at %lX\n", op, pos); return 1; }
    }
    (void)looped;

    /* the chip's rate to 44.1 kHz */
    {
        uint32_t frames = played, i;
        int16_t *out = (int16_t *)malloc((size_t)frames * 4);
        double ratio = (clock_hz / 144.0) / 44100.0, cutoff = ratio > 1 ? 1.0 / ratio : 1.0;
        for (i = 0; i < frames; i++) {
            double t = i * ratio, s[2] = {0, 0};
            long k0 = (long)floor(t) - TAPS / 2 + 1, k;
            int c;
            for (k = k0; k < k0 + TAPS; k++) {
                double x = t - (double)k, w;
                if (k < 0 || (size_t)k >= native_len) continue;
                w = cutoff * sinc(x * cutoff) * (0.42 + 0.5 * cos(M_PI * x / (TAPS / 2)) + 0.08 * cos(2 * M_PI * x / (TAPS / 2)));
                s[0] += w * native[k * 2];
                s[1] += w * native[k * 2 + 1];
            }
            for (c = 0; c < 2; c++) {
                long v = lround(s[c]);
                out[i * 2 + c] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            }
        }
        if (loop_at >= 0 && tail > 0) {
            /* the last moments before the cut faded into what comes before the
             * place it goes on from: the jump lands where the sound already is (the
             * chip's free-running parts differ from pass to pass) */
            uint32_t lf = (total - loop_samples) + (uint32_t)(tail * 44100), lt = total + (uint32_t)(tail * 44100), n = XFADE;
            if (n > lf) n = lf;
            for (i = 0; i < n; i++) {
                double w = (double)i / n, a = cos(w * M_PI / 2), b = sin(w * M_PI / 2);
                int c;
                for (c = 0; c < 2; c++) {
                    long v = lround(out[(lt - n + i) * 2 + c] * a + out[(lf - n + i) * 2 + c] * b);
                    out[(lt - n + i) * 2 + c] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
                }
            }
            frames = lt;
            printf("loop %u %u\n", lf, lt);
        }
        write_wav(argv[2], out, frames);
        free(out);
    }
    printf("%u %u %u\n", total, loop_ofs ? total - loop_samples : 0, played);
    return 0;
}
