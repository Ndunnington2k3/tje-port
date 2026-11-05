/*
 * vgm2wav_blastem: as tools/vgm2wav.c, through BlastEm's YM2612 (an accurate
 * one; BlastEm is GPLv3 and not part of this project - this is built against a
 * checkout of it and only makes the WAV files).
 *
 *   vgm2wav_blastem in.vgm out.wav [tail_seconds]
 *
 * Plays the VGM (YM2612 + DAC) to its end and on from its loop point for
 * tail_seconds (default 2), writes 44.1 kHz 16-bit stereo on the VGM's time base,
 * the last 0.1 s before the cut faded into what precedes the place it goes on
 * from, and prints "loop LOOP_FROM LOOP_TO" for music/tracks.txt.
 *
 * Build (BLASTEM = a checkout of BlastEm's source; stub headers in tools/blastem_stub):
 *   cc -O2 -Itools/blastem_stub -I$BLASTEM -o vgm2wav_blastem tools/vgm2wav_blastem.c \
 *      $BLASTEM/ym2612.c $BLASTEM/ym_common.c -lm
 * (copy ym2612.c and ym_common.c next to the stub headers first if the compiler
 * finds BlastEm's own render.h / event_log.h before the stubs)
 */
#include "ym2612.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- what ym2612.c needs from the rest of BlastEm ---- */
struct audio_source { int dummy; };
static audio_source the_source;
static int16_t *native;
static size_t native_len, native_cap;
audio_source *render_audio_source(const char *name, uint64_t master_clock, uint64_t sample_divider, uint8_t channels)
{
    (void)name; (void)master_clock; (void)sample_divider; (void)channels;
    return &the_source;
}
void render_audio_adjust_clock(audio_source *src, uint64_t master_clock, uint64_t sample_divider) { (void)src; (void)master_clock; (void)sample_divider; }
void render_free_source(audio_source *src) { (void)src; }
void render_put_stereo_sample(audio_source *src, int16_t left, int16_t right)
{
    (void)src;
    if (native_len + 1 > native_cap) {
        native_cap = native_cap ? native_cap * 2 : 1 << 20;
        native = (int16_t *)realloc(native, native_cap * 4);
        if (!native) { fprintf(stderr, "out of memory\n"); exit(1); }
    }
    native[native_len * 2] = left;
    native[native_len * 2 + 1] = right;
    native_len++;
}
uint8_t scope_add_channel(oscilloscope *scope, const char *name, uint32_t sample_rate) { (void)scope; (void)name; (void)sample_rate; return 0; }
void scope_add_sample(oscilloscope *scope, uint8_t channel, int16_t value, uint8_t trigger) { (void)scope; (void)channel; (void)value; (void)trigger; }
void vgm_ym2612_init(vgm_writer *writer, uint32_t clock) { (void)writer; (void)clock; }
void vgm_ym2612_part1_write(vgm_writer *writer, uint32_t cycle, uint8_t reg, uint8_t value) { (void)writer; (void)cycle; (void)reg; (void)value; }
void vgm_ym2612_part2_write(vgm_writer *writer, uint32_t cycle, uint8_t reg, uint8_t value) { (void)writer; (void)cycle; (void)reg; (void)value; }
int wave_init(FILE *f, uint32_t sample_rate, uint16_t bits_per_sample, uint16_t num_channels) { (void)f; (void)sample_rate; (void)bits_per_sample; (void)num_channels; return 0; }
int wave_finalize(FILE *f) { (void)f; return 0; }
void save_int32(serialize_buffer *buf, uint32_t val) { (void)buf; (void)val; }
void save_int16(serialize_buffer *buf, uint16_t val) { (void)buf; (void)val; }
void save_int8(serialize_buffer *buf, uint8_t val) { (void)buf; (void)val; }
void save_buffer8(serialize_buffer *buf, void *val, size_t len) { (void)buf; (void)val; (void)len; }
uint32_t load_int32(deserialize_buffer *buf) { (void)buf; return 0; }
uint16_t load_int16(deserialize_buffer *buf) { (void)buf; return 0; }
uint8_t load_int8(deserialize_buffer *buf) { (void)buf; return 0; }
void load_buffer8(deserialize_buffer *buf, void *dst, size_t len) { (void)buf; (void)dst; (void)len; }

/* ---- the VGM ---- */
#define MCLK 53693175u          /* the Genesis' master clock; the YM2612 runs at MCLK / 7 */
#define TAPS 32
#define XFADE 4410
static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static double sinc(double x) { return x == 0 ? 1.0 : sin(M_PI * x) / (M_PI * x); }

static void write_wav(const char *path, const int16_t *pcm, uint32_t frames)
{
    FILE *f = fopen(path, "wb");
    uint32_t data = frames * 4, v;
    if (!f) { perror(path); exit(1); }
    fwrite("RIFF", 1, 4, f); v = 36 + data; fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    v = 16; fwrite(&v, 4, 1, f);
    { uint16_t w[2] = {1, 2}; fwrite(w, 2, 2, f); }
    v = 44100; fwrite(&v, 4, 1, f);
    v = 44100 * 4; fwrite(&v, 4, 1, f);
    { uint16_t w[2] = {4, 16}; fwrite(w, 2, 2, f); }
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fwrite(pcm, 4, frames, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    static ym2612_context ym;
    FILE *f;
    uint8_t *vgm, *pcm_bank = NULL;
    long len, pos, loop_at;
    uint32_t total, loop_ofs, loop_samples, data_ofs, pcm_len = 0, pcm_pos = 0, played = 0, target, i;
    uint64_t cyc_x = 0;            /* master cycles x 44100 */
    double tail = argc > 3 ? atof(argv[3]) : 2.0;
    if (argc < 3) { fprintf(stderr, "usage: vgm2wav_blastem in.vgm out.wav [tail_seconds]\n"); return 2; }
    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
    vgm = (uint8_t *)malloc((size_t)len);
    if (fread(vgm, 1, (size_t)len, f) != (size_t)len) return 1;
    fclose(f);
    if (len < 0x40 || memcmp(vgm, "Vgm ", 4)) { fprintf(stderr, "not a VGM (gzipped? gunzip it)\n"); return 1; }
    total = le32(vgm + 0x18);
    loop_ofs = le32(vgm + 0x1C);
    loop_samples = le32(vgm + 0x20);
    data_ofs = le32(vgm + 8) >= 0x150 && le32(vgm + 0x34) ? 0x34 + le32(vgm + 0x34) : 0x40;
    loop_at = loop_ofs ? (long)(0x1C + loop_ofs) : -1;
    target = total + (loop_at >= 0 ? (uint32_t)(tail * 44100) : 0);

    ym_init(&ym, MCLK, 7, 0);
    pos = (long)data_ofs;
#define NOW ((uint32_t)(cyc_x / 44100))
#define WAIT(n) do { uint32_t n_ = (n); if (played + n_ > target) n_ = target - played; cyc_x += (uint64_t)n_ * MCLK; played += n_; ym_run(&ym, NOW); \
                     if (NOW > 0x40000000u) { uint32_t d = 0x20000000u; ym_adjust_cycles(&ym, d); cyc_x -= (uint64_t)d * 44100; } } while (0)
    while (played < target && pos < len) {
        uint8_t op = vgm[pos];
        if (op == 0x52 || op == 0x53) {
            ym_run(&ym, NOW);
            if (op == 0x52) ym_address_write_part1(&ym, vgm[pos + 1]); else ym_address_write_part2(&ym, vgm[pos + 1]);
            ym_data_write(&ym, vgm[pos + 2]);
            pos += 3;
        } else if (op == 0x61) { WAIT((uint32_t)(vgm[pos + 1] | (vgm[pos + 2] << 8))); pos += 3; }
        else if (op == 0x62) { WAIT(735); pos += 1; }
        else if (op == 0x63) { WAIT(882); pos += 1; }
        else if (op >= 0x70 && op <= 0x7F) { WAIT((op & 15) + 1u); pos += 1; }
        else if (op >= 0x80 && op <= 0x8F) {
            ym_run(&ym, NOW);
            ym_address_write_part1(&ym, 0x2A);
            ym_data_write(&ym, pcm_pos < pcm_len ? pcm_bank[pcm_pos] : 0x80);
            pcm_pos++;
            WAIT(op & 15u);
            pos += 1;
        } else if (op == 0xE0) { pcm_pos = le32(vgm + pos + 1); pos += 5; }
        else if (op == 0x67) {
            uint32_t n = le32(vgm + pos + 3);
            if (vgm[pos + 2] == 0x00 && !pcm_bank) { pcm_bank = vgm + pos + 7; pcm_len = n; }
            pos += 7 + (long)n;
        } else if (op == 0x50 || op == 0x4F) pos += 2;
        else if (op == 0x66) { if (loop_at < 0) break; pos = loop_at; }
        else { fprintf(stderr, "unknown VGM command %02X at %lX\n", op, pos); return 1; }
    }

    {
        uint32_t frames = played;
        int16_t *out = (int16_t *)malloc((size_t)frames * 4);
        double rate = (double)MCLK / (7 * 6 * 24), ratio = rate / 44100.0, cutoff = ratio > 1 ? 1.0 / ratio : 1.0;
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
    }
    return 0;
}
