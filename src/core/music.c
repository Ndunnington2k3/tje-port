/* Recorded music for the native sound: see music.h. */
#include "music.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __PSP__
/* (the PSP's own file calls: this runs on the audio thread, the game's stdio on
 * another) */
#include <pspiofilemgr.h>
typedef SceUID MFile;
#define MF_NONE (-1)
static MFile mf_open(const char *p) { SceUID f = sceIoOpen(p, PSP_O_RDONLY, 0); return f >= 0 ? f : MF_NONE; }
static int mf_read(MFile f, void *b, int n) { int r = sceIoRead(f, b, (SceSize)n); return r < 0 ? 0 : r; }
static void mf_seek(MFile f, long pos) { sceIoLseek32(f, (int)pos, PSP_SEEK_SET); }
static void mf_close(MFile f) { sceIoClose(f); }
#else
typedef FILE *MFile;
#define MF_NONE NULL
static MFile mf_open(const char *p) { return fopen(p, "rb"); }
static int mf_read(MFile f, void *b, int n) { return (int)fread(b, 1, (size_t)n, f); }
static void mf_seek(MFile f, long pos) { fseek(f, pos, SEEK_SET); }
static void mf_close(MFile f) { fclose(f); }
#endif

void (*music_song_hook)(int song);

static char dir[256] = "music/";
static volatile int req_song = -1, req_seq;   /* (written by the game) */
static volatile int paused;
static int ticks, idle_frames;

void music_set_dir(const char *d)
{
    size_t n;
    snprintf(dir, sizeof dir, "%s", d);
    n = strlen(dir);
    if (n && dir[n - 1] != '/' && dir[n - 1] != '\\' && n + 1 < sizeof dir) { dir[n] = '/'; dir[n + 1] = 0; }
}

void music_song(int song)
{
    req_song = song;
    req_seq++;
    paused = 0;
    idle_frames = 0;
    if (music_song_hook) music_song_hook(song);
}

void music_tick(void) { ticks++; }
int music_current(void) { return req_song; }

void music_frame(void)
{
    /* the game pauses its music by not running the engine (the pause screen):
     * the recording waits too */
    if (ticks) { idle_frames = 0; paused = 0; }
    else if (++idle_frames >= 3) paused = 1;
    ticks = 0;
}

/* ---- the player (music_mix's thread) ---------------------------------------- */
static MFile f = MF_NONE;
static int seq = -1;
static long data_at;                       /* file offset of the samples */
static uint32_t frames_total, loop_from, loop_to;   /* sample frames */
static int channels, rate;
static uint32_t pos_frame;                 /* the next frame to read from the file */
static uint32_t step, frac;                /* 16.16: file frames per output frame */
static int16_t cur[2], nxt[2];
#define BUF_FRAMES 2048
static int16_t buf[BUF_FRAMES * 2];
static int buf_len, buf_i;

static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static void close_song(void)
{
    if (f != MF_NONE) mf_close(f);
    f = MF_NONE;
}

static void open_song(int song)
{
    char path[300];
    uint8_t h[12], c[8];
    long at = 12;
    int have_fmt = 0, have_loop = 0, bits = 0;
    close_song();
    if (song < 0) return;
    snprintf(path, sizeof path, "%s%02d.wav", dir, song);
    f = mf_open(path);
    if (f == MF_NONE) return;
    if (mf_read(f, h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) { close_song(); return; }
    data_at = 0;
    for (;;) {
        uint32_t len;
        mf_seek(f, at);
        if (mf_read(f, c, 8) != 8) break;
        len = le32(c + 4);
        if (!memcmp(c, "fmt ", 4)) {
            uint8_t fm[16];
            if (mf_read(f, fm, 16) != 16) break;
            channels = fm[2] | (fm[3] << 8);
            rate = (int)le32(fm + 4);
            bits = fm[14] | (fm[15] << 8);
            have_fmt = (fm[0] | (fm[1] << 8)) == 1;
        } else if (!memcmp(c, "smpl", 4) && len >= 60) {
            uint8_t sm[60];
            if (mf_read(f, sm, 60) == 60 && le32(sm + 28) > 0) {
                loop_from = le32(sm + 36 + 8);
                loop_to = le32(sm + 36 + 12) + 1;      /* (the chunk's end is the last frame) */
                have_loop = 1;
            }
        } else if (!memcmp(c, "data", 4)) {
            data_at = at + 8;
            frames_total = len;
        }
        at += 8 + (long)len + (len & 1);
    }
    if (!have_fmt || !data_at || bits != 16 || (channels != 1 && channels != 2) || rate < 4000) { close_song(); return; }
    frames_total /= (uint32_t)(channels * 2);
    {
        /* (NN.loop overrides the file's own loop) */
        char lp[300];
        FILE *l;
        unsigned a, b;
        snprintf(lp, sizeof lp, "%s%02d.loop", dir, song);
        l = fopen(lp, "r");
        if (l) {
            if (fscanf(l, "%u %u", &a, &b) == 2 && a < b) { loop_from = a; loop_to = b; have_loop = 1; }
            fclose(l);
        }
    }
    if (!have_loop || loop_to > frames_total || loop_from >= loop_to) { loop_from = 0; loop_to = 0; }
    step = (uint32_t)(((uint64_t)rate << 16) / 48000u);
    frac = 0;
    pos_frame = 0;
    buf_len = buf_i = 0;
    cur[0] = cur[1] = nxt[0] = nxt[1] = 0;
    mf_seek(f, data_at);
}

/* the next frame of the file, looping; 0 at the end of a song played once */
static int next_frame(int16_t *lr)
{
    if (f == MF_NONE) return 0;
    if (buf_i >= buf_len) {
        uint32_t end = loop_to ? loop_to : frames_total, want;
        int got;
        if (pos_frame >= end) {
            if (!loop_to) { close_song(); return 0; }
            pos_frame = loop_from;
            mf_seek(f, data_at + (long)pos_frame * channels * 2);
        }
        want = end - pos_frame;
        if (want > BUF_FRAMES) want = BUF_FRAMES;
        got = mf_read(f, buf, (int)want * channels * 2) / (channels * 2);
        if (got <= 0) { close_song(); return 0; }
        pos_frame += (uint32_t)got;
        buf_len = got;
        buf_i = 0;
    }
    if (channels == 2) { lr[0] = buf[buf_i * 2]; lr[1] = buf[buf_i * 2 + 1]; }
    else lr[0] = lr[1] = buf[buf_i];
    buf_i++;
    return 1;
}

void music_mix(int16_t *out, int frames)
{
    int i;
    if (seq != req_seq) {
        seq = req_seq;
        open_song(req_song);
        if (f != MF_NONE) { next_frame(cur); next_frame(nxt); }
    }
    if (f == MF_NONE || paused) return;
    for (i = 0; i < frames; i++) {
        int c;
        for (c = 0; c < 2; c++) {
            int32_t v = cur[c] + (int32_t)(((int32_t)(nxt[c] - cur[c]) * (int32_t)(frac >> 1)) >> 15) + out[i * 2 + c];
            out[i * 2 + c] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        frac += step;
        while (frac >= 0x10000u) {
            frac -= 0x10000u;
            cur[0] = nxt[0];
            cur[1] = nxt[1];
            if (!next_frame(nxt)) { nxt[0] = nxt[1] = 0; if (f == MF_NONE) return; }
        }
    }
}
