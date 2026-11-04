/*
 * tje_headless --record-music DIR: the game's music and FM sound effects recorded
 * from the Genesis sound for the native sound (core/music.h):
 *   DIR/music/NN.wav - every song of the table (FM, PSG and its drum sounds),
 *                      48 kHz 16-bit stereo;
 *   DIR/sfx/NN.wav   - every FM sound effect of the engine ($E47C, 1-22),
 *                      24 kHz 16-bit mono.
 * The FM chip is the YM2612 as in the Genesis (TJE_REC_YM3438=1: the later chip,
 * without its distortion of quiet sounds - sound.c snd_clean_chip).
 *
 * In the menus (--press 900:3:S: nothing times out there), each song is started as
 * the game starts one (the VBlank's song request, VBLANK_FLAG_7), other songs held
 * off meanwhile (sound_hooks.c, g_music_record_song). A song plays once: at its end
 * the engine itself starts the next (often the same again, or song+7: an intro
 * followed by its main part) - that request ends the recording. A song whose engine
 * stops changing has ended too (a second more is kept). Then, the music stopped,
 * each effect is asked for as the game does ($E47C) and kept until its channels are
 * done and it has died away.
 */
#include "core/m68k_rt.h"
#include "core/genesis.h"
#include "core/music.h"
#include "core/sound.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir_(p) _mkdir(p)
#else
#include <sys/stat.h>
#define mkdir_(p) mkdir(p, 0777)
#endif

extern int g_music_record_song;
extern void (*g_music_record_held)(int);
extern void (*g_music_record_sfx_hook)(int);

#define SONGS        15
#define SFX_FIRST    1
#define SFX_LAST     22           /* (the table at $3C3C0 holds 23, 0 being none) */
#define BOOT_FRAMES  1400         /* (the menus by then: --press 900:3:S) */
#define MAX_FRAMES   (60 * 60 * 6)
#define STATE_A      0xFFFE00u    /* the engine's channels */
#define STATE_A_LEN  0x120u
#define FLAG_7       0xFF802Cu    /* the song asked for */
#define FLAG_8       0xFF802Du    /* the song playing */
#define SONG_ALT     0xFFE476u
#define SFX_REQUEST  0xFFE47Cu
#define MUSIC_STOP   0xFFE47Au    /* nonzero: the engine stops the music */
#define SFX_SLOTS(i) (0xFFFF18u + 0x26u * (uint32_t)(i))   /* its effect channels' data pointers */

enum { P_BOOT, P_WAIT, P_ASKED, P_SONG, P_SFX_WAIT, P_SFX_ASKED, P_SFX };

static char out_dir[512];
static int active, phase, item = -1, ended;
static unsigned frame, item_frames, quiet;
static int16_t *audio;
static size_t audio_len, audio_cap;      /* stereo frames */
static uint8_t last_state[STATE_A_LEN];
static int last_peak;

void record_music_init(const char *dir)
{
    char p[600];
    snprintf(out_dir, sizeof out_dir, "%s", dir);
    snprintf(p, sizeof p, "%s/music", out_dir);
    mkdir_(p);
    snprintf(p, sizeof p, "%s/sfx", out_dir);
    mkdir_(p);
    snd_clean_chip = getenv("TJE_REC_YM3438") != NULL;
    active = 1;
}

int record_music_active(void) { return active; }

void record_music_audio(const int16_t *samples, int n)
{
    int i;
    last_peak = 0;
    for (i = 0; i < n * 2; i++) if (abs(samples[i]) > last_peak) last_peak = abs(samples[i]);
    if (!active || (phase != P_SONG && phase != P_SFX)) return;
    if (audio_len + (size_t)n > audio_cap) {
        audio_cap = (audio_len + (size_t)n) * 2 + 48000;
        audio = (int16_t *)realloc(audio, audio_cap * 4);
        if (!audio) { fprintf(stderr, "record-music: out of memory\n"); exit(1); }
    }
    memcpy(audio + audio_len * 2, samples, (size_t)n * 4);
    audio_len += (size_t)n;
}

static void on_song(int s)
{
    if (phase == P_ASKED && s == item) { phase = P_SONG; audio_len = 0; item_frames = 0; quiet = 0; ended = 0; }
    else if (phase == P_SONG && s == item) ended = 1;   /* (the same again: its end) */
}
static void on_held(int s) { (void)s; if (phase == P_SONG) ended = 1; }
static void on_sfx(int id)
{
    if (phase == P_SFX_ASKED && id == item) { phase = P_SFX; audio_len = 0; item_frames = 0; quiet = 0; }
}

static void put16(FILE *f, unsigned v) { fputc((int)(v & 0xFF), f); fputc((int)((v >> 8) & 0xFF), f); }
static void put32(FILE *f, uint32_t v) { put16(f, v & 0xFFFF); put16(f, v >> 16); }

static void write_wav(const char *path, const int16_t *pcm, uint32_t frames, int channels, uint32_t rate)
{
    FILE *f = fopen(path, "wb");
    uint32_t data = frames * (uint32_t)channels * 2;
    if (!f) { fprintf(stderr, "record-music: can't write %s\n", path); return; }
    fwrite("RIFF", 1, 4, f);
    put32(f, 4 + 8 + 16 + 8 + data);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16); put16(f, 1); put16(f, (unsigned)channels); put32(f, rate);
    put32(f, rate * (uint32_t)channels * 2); put16(f, (unsigned)channels * 2); put16(f, 16);
    fwrite("data", 1, 4, f);
    put32(f, data);
    fwrite(pcm, 2, frames * (uint32_t)channels, f);
    fclose(f);
}

static void write_song(void)
{
    char path[600];
    snprintf(path, sizeof path, "%s/music/%02d.wav", out_dir, item);
    write_wav(path, audio, (uint32_t)audio_len, 2, 48000);
    fprintf(stderr, "record-music: song %02d: %.1f s%s\n", item, audio_len / 48000.0, ended ? "" : " (no next song)");
}

static void write_sfx(void)
{
    char path[600];
    size_t n = audio_len, i;
    int16_t *mono;
    /* the silence after it left off; 24 kHz mono (both halves, two frames a sample) */
    while (n > 0 && abs(audio[(n - 1) * 2]) < 48 && abs(audio[(n - 1) * 2 + 1]) < 48) n--;
    mono = (int16_t *)malloc(n / 2 * 2 + 2);
    for (i = 0; i + 1 < n; i += 2)
        mono[i / 2] = (int16_t)((audio[i * 2] + audio[i * 2 + 1] + audio[i * 2 + 2] + audio[i * 2 + 3]) / 4);
    snprintf(path, sizeof path, "%s/sfx/%02d.wav", out_dir, item);
    write_wav(path, mono, (uint32_t)(n / 2), 1, 24000);
    free(mono);
    fprintf(stderr, "record-music: effect %02d: %.2f s\n", item, n / 48000.0);
}

static void ask_song(void)
{
    g_music_record_song = item;
    /* asked for as the game does: the VBlank starts it (+7 for songs 7.. comes from
     * SONG_ALT, so it is left off and the number asked for is the table's) */
    wr16(SONG_ALT, 0);
    wr8(FLAG_8, 0xFF);
    wr8(FLAG_7, (uint32_t)item);
}

/* once a frame (after the frame's sound) */
void record_music_frame(void)
{
    unsigned i;
    int same;
    if (!active) return;
    frame++;
    if (frame == 1) { music_song_hook = on_song; g_music_record_held = on_held; g_music_record_sfx_hook = on_sfx; }
    switch (phase) {
    case P_BOOT:
        if (frame >= BOOT_FRAMES) { item = 0; g_music_record_song = 999; phase = P_WAIT; item_frames = 0; }
        return;
    case P_WAIT:
        /* (no song meanwhile: the last one dies away first, not to be in this one) */
        if (last_peak < 64 || ++item_frames > 180) { ask_song(); phase = P_ASKED; item_frames = 0; }
        return;
    case P_ASKED:
        if ((++item_frames & 15) == 0) { wr8(FLAG_8, 0xFF); wr8(FLAG_7, (uint32_t)item); }
        return;
    case P_SONG:
        same = 1;
        for (i = 0; i < STATE_A_LEN; i++) {
            uint8_t b = (uint8_t)rd8(STATE_A + i);
            if (b != last_state[i]) same = 0;
            last_state[i] = b;
        }
        quiet = same ? quiet + 1 : 0;
        if (ended || quiet >= 60 || ++item_frames > MAX_FRAMES) {
            write_song();
            item++;
            g_music_record_song = 999;
            item_frames = 0;
            if (item < SONGS) { phase = P_WAIT; return; }
            /* the effects: the music stopped first */
            wr16(MUSIC_STOP, 1);
            item = SFX_FIRST;
            phase = P_SFX_WAIT;
        }
        return;
    case P_SFX_WAIT:
        if (last_peak < 32 || ++item_frames > 240) {
            wr16(SFX_REQUEST, (uint32_t)item);
            phase = P_SFX_ASKED;
            item_frames = 0;
        }
        return;
    case P_SFX_ASKED:
        if (++item_frames > 30) { fprintf(stderr, "record-music: effect %02d didn't start\n", item); goto next_sfx; }
        return;
    case P_SFX:
        same = !rd32(SFX_SLOTS(0)) && !rd32(SFX_SLOTS(1)) && !rd32(SFX_SLOTS(2)) && last_peak < 48;
        quiet = same ? quiet + 1 : 0;
        if (quiet >= 6 || ++item_frames > 600) {
            write_sfx();
            goto next_sfx;
        }
        return;
    }
    return;
next_sfx:
    item++;
    item_frames = 0;
    if (item > SFX_LAST) { fprintf(stderr, "record-music: done\n"); exit(0); }
    phase = P_SFX_WAIT;
}
