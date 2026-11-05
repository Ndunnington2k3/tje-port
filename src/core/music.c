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

static char dir[256] = "music/", sfx_dir[256] = "sfx/";
static volatile int req_song = -1, req_seq;   /* (written by the game) */
static volatile int paused;
static int ticks, idle_frames;
/* (the engine's frames since the song was asked for: the title's song is kept
 * with the game - its sounds go with the intro's pictures) */
static volatile uint32_t song_ticks;

static volatile int dir_gen;
void music_set_dir(const char *d)
{
    size_t n;
    char was[256];
    snprintf(was, sizeof was, "%s", dir);
    snprintf(dir, sizeof dir, "%s", d);
    n = strlen(dir);
    if (n && dir[n - 1] != '/' && dir[n - 1] != '\\' && n + 1 < sizeof dir) { dir[n] = '/'; dir[n + 1] = 0; }
    /* the effects: sfx/ next to music/ */
    snprintf(sfx_dir, sizeof sfx_dir, "%s", dir);
    n = strlen(sfx_dir);
    if (n >= 6 && !strcmp(sfx_dir + n - 6, "music/")) snprintf(sfx_dir + n - 6, sizeof sfx_dir - (n - 6), "sfx/");
    if (strcmp(was, dir)) dir_gen++;
}

void music_song(int song)
{
    if (getenv("TJE_MUSIC_LOG")) { extern uint32_t g_frame; fprintf(stderr, "music: song %d at frame %u\n", song, (unsigned)g_frame); }
    req_song = song;
    song_ticks = 0;
    req_seq++;
    paused = 0;
    idle_frames = 0;
    if (music_song_hook) music_song_hook(song);
}

void music_tick(void) { ticks++; song_ticks++; }

/* ---- the engine's FM sound effects: sfx/NN.wav, loaded when first played ---- */
#define SFX_MAX 32
static volatile int sfx_req = -1, sfx_seq;
void music_sfx(int id)
{
    if (id < 0 || id >= SFX_MAX) return;   /* (0: the effect stopped) */
    if (getenv("TJE_MUSIC_LOG")) { extern uint32_t g_frame; fprintf(stderr, "music: effect %d at frame %u\n", id, (unsigned)g_frame); }
    sfx_req = id;
    sfx_seq++;
}
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
static volatile int seq = -1;   /* (the request the ring has; the mixer reads it) */
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

static volatile uint32_t ring_w, ring_r;
static volatile int gate;   /* (the title's song kept with the game: music_mix) */

/* ---- music/tracks.txt: which file plays each song, and how files loop ----------
 *   file NAME LOOP_FROM LOOP_TO   at frame LOOP_TO, NAME goes on from LOOP_FROM
 *   song N NAME START             the game's song N: NAME from frame START
 * (frames of the file: 44100 a second for 44.1 kHz). A song the game asks for
 * while its file plays at about that place already (the engine starting the
 * next part of a theme, or the theme again, as the file goes on by itself)
 * changes nothing: no gap, no jump. Without a table: NN.wav for song NN, looping
 * as its smpl chunk or NN.loop says. */
#define T_FILES 16
#define T_SONGS 32
static struct { char name[64]; uint32_t loop_from, loop_to; } t_file[T_FILES];
static struct { int file; uint32_t start; } t_song[T_SONGS];
static int t_files;

static void table_load(void)
{
    char path[300], text[4096], *line, *next;
    MFile tf;
    int n, i;
    t_files = 0;
    for (i = 0; i < T_SONGS; i++) t_song[i].file = -1;
    snprintf(path, sizeof path, "%stracks.txt", dir);
    tf = mf_open(path);
    if (tf == MF_NONE) return;
    n = mf_read(tf, text, (int)sizeof text - 1);
    mf_close(tf);
    text[n > 0 ? n : 0] = 0;
    for (line = text; line && *line; line = next) {
        char name[64];
        unsigned a, b;
        int song;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (sscanf(line, " file %63s %u %u", name, &a, &b) == 3 && t_files < T_FILES) {
            snprintf(t_file[t_files].name, sizeof t_file[0].name, "%s", name);
            t_file[t_files].loop_from = a;
            t_file[t_files].loop_to = b > a ? b : 0;
            t_files++;
        } else if (sscanf(line, " song %d %63s %u", &song, name, &a) == 3 && song >= 0 && song < T_SONGS) {
            for (i = 0; i < t_files; i++)
                if (!strcmp(t_file[i].name, name)) { t_song[song].file = i; t_song[song].start = a; }
        }
    }
}

static char cur_name[64];   /* (the file open) */

static void open_file(const char *name, int song, uint32_t start, uint32_t t_from, uint32_t t_to)
{
    char path[300];
    uint8_t h[12], c[8];
    long at = 12;
    int have_fmt = 0, have_loop = 0, bits = 0;
    close_song();
    cur_name[0] = 0;
    if (!name) return;
    snprintf(path, sizeof path, "%s%s", dir, name);
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
        l = song >= 0 ? fopen(lp, "r") : NULL;
        if (l) {
            if (fscanf(l, "%u %u", &a, &b) == 2 && a < b) { loop_from = a; loop_to = b; have_loop = 1; }
            fclose(l);
        }
    }
    if (t_to) { loop_from = t_from; loop_to = t_to; have_loop = 1; }   /* (the table's) */
    if (!have_loop || loop_to > frames_total || loop_from >= loop_to) { loop_from = 0; loop_to = 0; }
    step = (uint32_t)(((uint64_t)rate << 16) / 48000u);
    frac = 0;
    pos_frame = start < frames_total ? start : 0;
    buf_len = buf_i = 0;
    cur[0] = cur[1] = nxt[0] = nxt[1] = 0;
    mf_seek(f, data_at + (long)pos_frame * channels * 2);
    snprintf(cur_name, sizeof cur_name, "%s", name);
}

static void open_song(int song)
{
    char name[16];
    if (song < 0) { open_file(NULL, -1, 0, 0, 0); return; }
    if (song < T_SONGS && t_song[song].file >= 0) {
        int k = t_song[song].file;
        open_file(t_file[k].name, -1, t_song[song].start, t_file[k].loop_from, t_file[k].loop_to);
        return;
    }
    snprintf(name, sizeof name, "%02d.wav", song);
    open_file(name, song, 0, 0, 0);
}

/* the song asked for is where its file plays now (within -1.5 / +4 s: the file
 * a little behind or ahead of the game) */
static int song_here(int song)
{
    int k;
    int64_t d, len, playing;
    if (f == MF_NONE || song < 0 || song >= T_SONGS || t_song[song].file < 0) return 0;
    k = t_song[song].file;
    if (strcmp(t_file[k].name, cur_name)) return 0;
    /* (the frame being heard: the reader is ahead by what it holds and the ring) */
    playing = (int64_t)pos_frame - (buf_len - buf_i) - (int64_t)(ring_w - ring_r) * rate / 48000;
    d = playing - (int64_t)t_song[song].start;
    len = loop_to ? (int64_t)loop_to - loop_from : 0;
    if (len > 0) { d %= len; if (d > len / 2) d -= len; if (d < -len / 2) d += len; }
    if (getenv("TJE_MUSIC_LOG")) fprintf(stderr, "music: reader: song %d: the file is %+.3f s from its start\n", song, (double)d / rate);
    return d > -(int64_t)rate * 3 / 2 && d < (int64_t)rate * 4;
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

static struct { int16_t *pcm; uint32_t frames; int rate, tried; } sfx[SFX_MAX];
static int sfx_cur = -1, sfx_seen = -1;
static uint32_t sfx_pos, sfx_step;   /* 16.16 */

static void sfx_load(int id)
{
    char path[300];
    MFile g;
    uint8_t h[12], c[8], fm[16];
    long at = 12;
    int ch = 0, bits = 0, rate = 0, pcm_ok = 0;
    sfx[id].tried = 1;
    snprintf(path, sizeof path, "%s%02d.wav", sfx_dir, id);
    g = mf_open(path);
    if (g == MF_NONE) return;
    if (mf_read(g, h, 12) == 12 && !memcmp(h, "RIFF", 4))
        for (;;) {
            uint32_t len;
            mf_seek(g, at);
            if (mf_read(g, c, 8) != 8) break;
            len = le32(c + 4);
            if (!memcmp(c, "fmt ", 4) && mf_read(g, fm, 16) == 16) {
                ch = fm[2] | (fm[3] << 8);
                rate = (int)le32(fm + 4);
                bits = fm[14] | (fm[15] << 8);
                pcm_ok = (fm[0] | (fm[1] << 8)) == 1;
            } else if (!memcmp(c, "data", 4) && pcm_ok && bits == 16 && (ch == 1 || ch == 2) && rate >= 4000) {
                uint32_t n = len / (uint32_t)(ch * 2), i;
                int16_t *p = (int16_t *)malloc((size_t)n * (size_t)ch * 2);
                if (p && mf_read(g, p, (int)(n * (uint32_t)ch * 2)) == (int)(n * (uint32_t)ch * 2)) {
                    if (ch == 2) for (i = 0; i < n; i++) p[i] = (int16_t)((p[i * 2] + p[i * 2 + 1]) / 2);
                    sfx[id].pcm = p;
                    sfx[id].frames = n;
                    sfx[id].rate = rate;
                } else free(p);
                break;
            }
            at += 8 + (long)len + (len & 1);
        }
    mf_close(g);
}

static void sfx_mix(int16_t *out, int frames)
{
    int i;
    if (sfx_seen != sfx_seq) {
        int id = sfx_req;
        sfx_seen = sfx_seq;
        sfx_cur = id > 0 && sfx[id].pcm ? id : -1;   /* (loaded by the reader, music_fill) */
        sfx_pos = 0;
        if (sfx_cur >= 0) sfx_step = (uint32_t)(((uint64_t)sfx[id].rate << 16) / 48000u);
    }
    if (sfx_cur < 0) return;
    for (i = 0; i < frames; i++) {
        uint32_t k = sfx_pos >> 16;
        int32_t v, c;
        if (k + 1 >= sfx[sfx_cur].frames) { sfx_cur = -1; return; }
        v = sfx[sfx_cur].pcm[k] + (((int32_t)(sfx[sfx_cur].pcm[k + 1] - sfx[sfx_cur].pcm[k]) * (int32_t)((sfx_pos & 0xFFFF) >> 1)) >> 15);
        for (c = 0; c < 2; c++) {
            int32_t o = out[i * 2 + c] + v;
            out[i * 2 + c] = (int16_t)(o > 32767 ? 32767 : o < -32768 ? -32768 : o);
        }
        sfx_pos += sfx_step;
    }
}

/* ---- the ring between the reader (music_fill) and the mixer (music_mix) ----
 * The files are read ahead into the ring - on the PSP by a thread of their own, a
 * Memory Stick read taking longer than the sound can wait; elsewhere by music_mix
 * itself. The reader owns the files, the mixer only the ring's read side. */
#define RING_FRAMES 32768                     /* (0.68 s at 48 kHz) */
static int16_t ring[RING_FRAMES * 2];
static volatile uint32_t flush_to;
static volatile int ring_gen;
int music_threaded;

/* the reader: next part of the song into the ring; 0 = nothing to do just now */
int music_fill(void)
{
    int i, n, did = 0;
    static int sfx_gen = -1, sfx_next = SFX_MAX, song_gen = -1;
    /* (the folder changed: the song opened again from there) */
    if (song_gen != dir_gen) { song_gen = dir_gen; table_load(); close_song(); cur_name[0] = 0; seq = req_seq - 1; }
    if (seq != req_seq) {
        int want = req_seq, song = req_song;
        int here = song_here(song);
        if (getenv("TJE_MUSIC_LOG")) fprintf(stderr, "music: reader: song %d %s\n", song, here ? "plays on" : "opened");
        if (here) gate = 0;   /* (playing on: it is there already) */
        else {
            open_song(song);
            if (f != MF_NONE) { next_frame(cur); next_frame(nxt); }
            gate = song == 0;
            flush_to = ring_w;
            ring_gen++;
        }
        seq = want;   /* (last: the mixer takes the ring again from here) */
        did = 1;
    }
    /* the effects, loaded here a few at a time once the song is going (the mixer
     * can't wait for the Memory Stick) */
    if (sfx_gen != dir_gen) { sfx_gen = dir_gen; sfx_next = 1; }
    if (sfx_next < SFX_MAX && (f == MF_NONE || ring_w - ring_r > RING_FRAMES / 2)) {
        int k;
        for (k = 0; k < 4 && sfx_next < SFX_MAX; k++, sfx_next++) if (!sfx[sfx_next].pcm) sfx_load(sfx_next);
        did = 1;
    }
    if (f == MF_NONE) return did;
    n = (int)(RING_FRAMES - 1 - (ring_w - ring_r));
    if (n < 1024) return did;
    for (i = 0; i < n; i++) {
        uint32_t k = (ring_w + (uint32_t)i) % RING_FRAMES;
        int c;
        for (c = 0; c < 2; c++)
            ring[k * 2 + c] = (int16_t)(cur[c] + (int32_t)(((int32_t)(nxt[c] - cur[c]) * (int32_t)(frac >> 1)) >> 15));
        frac += step;
        while (frac >= 0x10000u) {
            frac -= 0x10000u;
            cur[0] = nxt[0];
            cur[1] = nxt[1];
            if (!next_frame(nxt)) { nxt[0] = nxt[1] = 0; if (f == MF_NONE) { i++; goto done; } }
        }
    }
done:
    ring_w += (uint32_t)i;
    return 1;
}

void music_mix(int16_t *out, int frames)
{
    static int gen;
    static uint32_t song_out;   /* (frames of the song played) */
    int i;
    if (!music_threaded) music_fill();
    sfx_mix(out, frames);
    if (gen != ring_gen) { gen = ring_gen; ring_r = flush_to; song_out = 0; }
    if (paused || seq != req_seq) return;
    /* the title's song ahead of the game (its first frames are slow: starting
     * up) by more than 0.15 s: it waits for the game */
    if (gate && song_out > song_ticks * 801u + 7200u) return;
    song_out += (uint32_t)frames;   /* (another song asked for: not the old one's rest while it opens) */
    for (i = 0; i < frames && ring_r != ring_w; i++) {
        uint32_t k = ring_r % RING_FRAMES;
        int c;
        for (c = 0; c < 2; c++) {
            int32_t v = ring[k * 2 + c] + out[i * 2 + c];
            out[i * 2 + c] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        ring_r++;
    }
}
