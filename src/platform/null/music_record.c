/*
 * tje_headless --record-music DIR: every song of the game's table recorded from the
 * Genesis sound (FM chip, PSG and its drum sounds) into DIR/NN.wav, 48 kHz 16-bit
 * stereo - for the native sound (core/music.h).
 *
 * In the menus (--press 900:3:S: nothing times out there), each song is started as
 * the game starts one (the VBlank's song request, VBLANK_FLAG_7), other songs held
 * off meanwhile (sound_hooks.c, g_music_record_song). A song plays once: at its end
 * the engine itself starts the next (often the same again, or song+7: an intro
 * followed by its main part) - that request ends the recording. The native sound
 * then plays the recordings as the engine starts them. A song whose engine stops
 * changing has ended too (a second more is kept).
 */
#include "core/m68k_rt.h"
#include "core/genesis.h"
#include "core/music.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int g_music_record_song;

#define SONGS        (getenv("TJE_REC_SONGS") ? atoi(getenv("TJE_REC_SONGS")) : 15)
#define BOOT_FRAMES  1400          /* (the menus by then: --press 900:3:S, nothing timing out) */
#define MAX_FRAMES   (getenv("TJE_REC_MAXF") ? atoi(getenv("TJE_REC_MAXF")) : 60 * 60 * 8)
#define STATE_A      0xFFFE00u     /* the engine's channels ($114 bytes from RAMWritesRoutine1) */
#define STATE_A_LEN  0x120u
#define STATE_B      0xFFE47Cu     /* its requests and the song's end count ($E47C-$E487) */
#define STATE_B_LEN  0x0Cu
#define STATE_LEN    (STATE_A_LEN + STATE_B_LEN)
#define FLAG_7       0xFF802Cu     /* the song asked for */
#define FLAG_8       0xFF802Du     /* the song playing */
#define SONG_ALT     0xFFE476u

static char out_dir[512];
static int active, song = -1, started;
static unsigned frame, song_frames;
static int16_t *audio;              /* this song's samples */
static size_t audio_len, audio_cap; /* stereo frames */
static size_t *frame_at;            /* audio_len at the start of each frame of the song */
static uint8_t *states;             /* the engine's state at each frame */
static uint32_t *hashes;
static int quiet_frames;

void record_music_init(const char *dir)
{
    snprintf(out_dir, sizeof out_dir, "%s", dir);
    active = 1;
    frame_at = (size_t *)malloc(sizeof(size_t) * (MAX_FRAMES + 2));
    states = (uint8_t *)malloc((size_t)STATE_LEN * (MAX_FRAMES + 2));
    hashes = (uint32_t *)malloc(sizeof(uint32_t) * (MAX_FRAMES + 2));
    if (!frame_at || !states || !hashes) { fprintf(stderr, "record-music: out of memory\n"); exit(1); }
}

int record_music_active(void) { return active; }

static int ended;
static void on_held(int s)
{
    (void)s;
    if (started) ended = 1;
}

static void on_song(int s)
{
    if (active && started && s == song) { ended = 1; return; }   /* (the same again: its end) */
    if (!active || song < 0 || s != song || started) return;
    started = 1;
    song_frames = 0;
    audio_len = 0;
    quiet_frames = 0;
}

static int last_peak;
void record_music_audio(const int16_t *samples, int n)
{
    int i;
    last_peak = 0;
    for (i = 0; i < n * 2; i++) if (abs(samples[i]) > last_peak) last_peak = abs(samples[i]);
    if (!active || !started) return;
    if (audio_len + (size_t)n > audio_cap) {
        audio_cap = (audio_len + (size_t)n) * 2 + 48000;
        audio = (int16_t *)realloc(audio, audio_cap * 4);
        if (!audio) { fprintf(stderr, "record-music: out of memory\n"); exit(1); }
    }
    memcpy(audio + audio_len * 2, samples, (size_t)n * 4);
    audio_len += (size_t)n;
}

static void put16(FILE *f, unsigned v) { fputc((int)(v & 0xFF), f); fputc((int)((v >> 8) & 0xFF), f); }
static void put32(FILE *f, uint32_t v) { put16(f, v & 0xFFFF); put16(f, v >> 16); }

static void write_song(size_t from, size_t to, long loop_from)
{
    char path[600];
    FILE *f;
    uint32_t frames = (uint32_t)(to - from), data = frames * 4, smpl = loop_from >= 0 ? 60 : 0;
    snprintf(path, sizeof path, "%s/%02d.wav", out_dir, song);
    f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "record-music: can't write %s\n", path); return; }
    fwrite("RIFF", 1, 4, f);
    put32(f, 4 + 8 + 16 + 8 + data + (smpl ? 8 + smpl : 0));
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16); put16(f, 1); put16(f, 2); put32(f, 48000); put32(f, 48000 * 4); put16(f, 4); put16(f, 16);
    if (smpl) {
        fwrite("smpl", 1, 4, f);
        put32(f, smpl);
        put32(f, 0); put32(f, 0); put32(f, 1000000000u / 48000u); put32(f, 60); put32(f, 0);
        put32(f, 0); put32(f, 0); put32(f, 1); put32(f, 0);
        put32(f, 0); put32(f, 0); put32(f, (uint32_t)loop_from); put32(f, frames - 1); put32(f, 0); put32(f, 0);
    }
    fwrite("data", 1, 4, f);
    put32(f, data);
    fwrite(audio + from * 2, 4, frames, f);
    fclose(f);
    if (loop_from >= 0) {
        snprintf(path, sizeof path, "%s/%02d.loop", out_dir, song);
        f = fopen(path, "w");
        if (f) { fprintf(f, "%ld %u\n", loop_from, frames); fclose(f); }
    }
    fprintf(stderr, "record-music: song %02d: %.1f s%s\n", song, frames / 48000.0,
            ended ? "" : " (ended without the next song)");
    if (loop_from >= 0) fprintf(stderr, "record-music:   loops from %.1f s\n", loop_from / 48000.0);
}

static int waiting;   /* frames waited for the last song to die away */
static void ask(void)
{
    g_music_record_song = song;
    /* ask for it as the game does: the VBlank starts it (+7 for songs 7.. comes
     * from SONG_ALT, so it is left off and the number asked for is the table's) */
    wr16(SONG_ALT, 0);
    wr8(FLAG_8, 0xFF);
    wr8(FLAG_7, (uint32_t)song);
}

static void next_song(void)
{
    if (song >= 0 && getenv("TJE_REC_DUMP")) {
        char p[600];
        FILE *d;
        snprintf(p, sizeof p, "%s/%02d.states", out_dir, song);
        d = fopen(p, "wb");
        if (d) { fwrite(states, STATE_LEN, song_frames, d); fclose(d); }
    }
    song++;
    started = 0;
    ended = 0;
    if (song >= SONGS) {
        fprintf(stderr, "record-music: done\n");
        exit(0);
    }
    /* (no song meanwhile: the last one dies away first, not to be in this one) */
    g_music_record_song = 999;
    waiting = 1;
}

/* once a frame (after the frame's sound) */
void record_music_frame(void)
{
    uint8_t *st;
    uint32_t h = 2166136261u;
    unsigned i, k;
    if (!active) return;
    frame++;
    if (frame == 1) { extern void (*g_music_record_held)(int); music_song_hook = on_song; g_music_record_held = on_held; }
    if (frame < BOOT_FRAMES) return;
    if (song < 0) { next_song(); return; }
    if (waiting) {
        if (last_peak < 64 || ++waiting > 180) { waiting = 0; ask(); }
        return;
    }
    if (!started) {
        /* (asked again while it hasn't started: the game may have taken the request) */
        if ((frame & 15) == 0) { wr8(FLAG_8, 0xFF); wr8(FLAG_7, (uint32_t)song); }
        return;
    }
    if (song_frames > MAX_FRAMES) {
        fprintf(stderr, "record-music: song %02d didn't loop\n", song);
        write_song(0, audio_len, -1);
        next_song();
        return;
    }
    frame_at[song_frames] = audio_len;
    st = states + (size_t)song_frames * STATE_LEN;
    for (i = 0; i < STATE_A_LEN; i++) st[i] = (uint8_t)rd8(STATE_A + i);
    for (i = 0; i < STATE_B_LEN; i++) st[STATE_A_LEN + i] = (uint8_t)rd8(STATE_B + i);
    for (i = 0; i < STATE_LEN; i++) h = (h ^ st[i]) * 16777619u;
    hashes[song_frames] = h;
    if (ended) {
        write_song(0, audio_len, -1);
        next_song();
        return;
    }
    /* (the same state frame after frame: the song has ended, nothing after it) */
    if (song_frames > 2 && hashes[song_frames - 1] == h && !memcmp(states + (size_t)(song_frames - 1) * STATE_LEN, st, STATE_LEN)) {
        if (++quiet_frames >= 60) {
            write_song(0, audio_len, -1);
            next_song();
            return;
        }
    } else quiet_frames = 0;
    song_frames++;
}
