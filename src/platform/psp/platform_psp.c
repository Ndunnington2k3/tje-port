/*
 * The PSP platform (platform.h): sceGu drawing (gfx_gu.c), the controls, sound
 * through sceAudio on a thread of its own, the Memory Stick for the ROM, settings
 * and saved games (next to EBOOT.PBP), and the Home button.
 *
 * Controls: the PSP's buttons are player 1's (the controls screen binds them; a
 * PSP has no keyboard, so the keyboard column stays empty). The analogue stick
 * moves as the d-pad does. Menus: Cross or Start chooses, Circle goes back;
 * in play Select is the port's Back (the soft pause in network play).
 */
#include "platform/platform.h"
#include "core/genesis.h"
#include "gfx/present.h"
#include "gfx_gu.h"
#include "gfx/viewlist.h"
#include "game/gameview.h"
#include "game/options.h"
#include "core/prof.h"
#include "core/sound.h"
#include "core/music.h"

#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <pspctrl.h>
#include <pspaudio.h>
#include <psppower.h>
#include <psputility.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

PSP_MODULE_INFO("ToeJam & Earl", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-2048);

#define SCR_W 480
#define SCR_H 272

static volatile int quit;
static char data_dir[256];
static char rom_path[300];

/* ---- the Home button ---------------------------------------------------------- */

static int exit_cb(int a, int b, void *c)
{
    (void)a; (void)b; (void)c;
    quit = 1;
    return 0;
}

static int callback_thread(SceSize args, void *argp)
{
    int cb;
    (void)args; (void)argp;
    cb = sceKernelCreateCallback("exit", exit_cb, NULL);
    sceKernelRegisterExitCallback(cb);
    sceKernelSleepThreadCB();
    return 0;
}

/* ---- sound -------------------------------------------------------------------- */

#define AUDIO_CHUNK 1024                 /* stereo frames per output block (21 ms) */
#define RING        (AUDIO_CHUNK * 8)
#define AUDIO_KEEP  2048                 /* the game's sound kept ahead (43 ms) */
static int16_t ring[RING * 2];
static volatile unsigned ring_r, ring_w;  /* in stereo frames, wrapping */
static int audio_on = 1;

/* The output: the PSP's own rate, 44.1 kHz, on a normal channel - the sound comes
 * at 48 kHz (the game's and the music's) and is converted here, between each two
 * samples (the SRC channel's own conversion was rough: a distorted sound). */
#define OUT_N 1024
static int audio_thread(SceSize args, void *argp)
{
    static int16_t in[(OUT_N * 2 + 16) * 2], out[OUT_N * 2];
    static int have;                       /* 48 kHz frames in `in` */
    static uint32_t frac;                  /* 16.16 position in `in` */
    const uint32_t step = (uint32_t)((48000u << 16) / 44100u);
    int ch;
    (void)args; (void)argp;
    ch = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, OUT_N, PSP_AUDIO_FORMAT_STEREO);
    if (ch < 0) return 0;
    while (!quit) {
        int need = (int)((frac + (OUT_N - 1) * step) >> 16) + 2, get = need - have, i, from = have;
        uint32_t adv;
        if (get > 0) {
            /* the game's sound and the music over it. The game's is kept about
             * AUDIO_KEEP frames ahead (it comes a frame at a time, this takes it
             * in blocks): when it runs out (a slow frame), this waits for that much
             * again rather than playing it in pieces; the frames a second of the
             * game and of the output differ a hair - a frame now and then is left
             * out or said twice to stay there */
            static int priming = 1;
            int avail = (int)(ring_w - ring_r), take, skip = 0;
            if (priming && avail >= AUDIO_KEEP) priming = 0;
            if (priming) take = 0;
            else if (avail < get) { take = avail; priming = 1; }
            else {
                take = get;
                if (avail > AUDIO_KEEP + 512 && avail > get + 1) skip = 1;
                else if (avail < AUDIO_KEEP - 512) take = get - 1;
            }
            for (i = 0; i < take; i++) {
                unsigned k = (ring_r + (unsigned)i) % RING;
                in[(from + i) * 2] = ring[k * 2];
                in[(from + i) * 2 + 1] = ring[k * 2 + 1];
            }
            ring_r += (unsigned)(take + skip);
            if (take == get - 1 && take > 0) {
                in[(from + take) * 2] = in[(from + take - 1) * 2];
                in[(from + take) * 2 + 1] = in[(from + take - 1) * 2 + 1];
                take++;
            }
            memset(in + (from + take) * 2, 0, (size_t)(get - take) * 4);
            if (snd_native()) music_mix(in + from * 2, get);
            have = need;
        }
        for (i = 0; i < OUT_N; i++) {
            uint32_t p = frac + (uint32_t)i * step, k = p >> 16;
            int32_t f = (int32_t)((p & 0xFFFF) >> 1), c;
            for (c = 0; c < 2; c++) {
                int32_t a0 = in[k * 2 + c], a1 = in[(k + 1) * 2 + c];
                out[i * 2 + c] = (int16_t)(a0 + (((a1 - a0) * f) >> 15));
            }
        }
        adv = (frac + OUT_N * step) >> 16;
        frac = (frac + OUT_N * step) & 0xFFFF;
        memmove(in, in + adv * 2, (size_t)(have - (int)adv) * 4);
        have -= (int)adv;
        sceAudioOutputBlocking(ch, PSP_AUDIO_VOLUME_MAX, out);
    }
    sceAudioChRelease(ch);
    return 0;
}

/* the recorded music read ahead from the Memory Stick (music.c) */
static int music_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    while (!quit)
        if (!music_fill()) sceKernelDelayThread(8000);
    return 0;
}

int plat_audio_enabled(void) { return audio_on; }

void plat_audio(const int16_t *samples, int frames)
{
    int i;
    /* (keep the delay short: the game runs at 59.92 frames a second, the
     * output a hair faster or slower) */
    if (ring_w - ring_r > RING - (unsigned)frames - AUDIO_CHUNK) return;
    for (i = 0; i < frames; i++) {
        unsigned k = (ring_w + (unsigned)i) % RING;
        ring[k * 2] = samples[i * 2];
        ring[k * 2 + 1] = samples[i * 2 + 1];
    }
    ring_w += (unsigned)frames;
}

/* ---- controls ------------------------------------------------------------------ */

#define NBUTTONS 8
static const struct { const char *tok, *label; unsigned mask; } psp_buttons[] = {
    {"up", "Up", PSP_CTRL_UP}, {"down", "Down", PSP_CTRL_DOWN}, {"left", "Left", PSP_CTRL_LEFT},
    {"right", "Right", PSP_CTRL_RIGHT}, {"cross", "Cross", PSP_CTRL_CROSS}, {"circle", "Circle", PSP_CTRL_CIRCLE},
    {"square", "Square", PSP_CTRL_SQUARE}, {"triangle", "Triangle", PSP_CTRL_TRIANGLE},
    {"l", "L", PSP_CTRL_LTRIGGER}, {"r", "R", PSP_CTRL_RTRIGGER}, {"start", "Start", PSP_CTRL_START},
    {"select", "Select", PSP_CTRL_SELECT},
};
#define NPSP ((int)(sizeof psp_buttons / sizeof psp_buttons[0]))
/* the game's actions: Up, Down, Left, Right, Present List, Map, Action, Pause */
static const char *const default_pad[NBUTTONS] = {"up", "down", "left", "right", "square", "triangle", "cross", "start"};
static char bind_pad[2][NBUTTONS][12];
static unsigned held;                   /* PSP buttons, plus the stick as the d-pad */
static int capturing = -1, capture_done;
static char capture_token[12];
static unsigned capture_before;

static unsigned token_mask(const char *t)
{
    int i;
    for (i = 0; i < NPSP; i++) if (!strcmp(t, psp_buttons[i].tok)) return psp_buttons[i].mask;
    return 0;
}

void plat_default_bindings(int player)
{
    int b;
    for (b = 0; b < NBUTTONS; b++)
        snprintf(bind_pad[player & 1][b], sizeof bind_pad[0][0], "%s", player ? "" : default_pad[b]);
}

const char *plat_binding(int player, int button, int slot)
{
    if (player < 0 || player > 1 || button < 0 || button >= NBUTTONS || slot == PLAT_BIND_KEY) return "";
    return bind_pad[player][button];
}

int plat_set_binding(int player, int button, int slot, const char *token)
{
    if (player < 0 || player > 1 || button < 0 || button >= NBUTTONS) return -1;
    if (slot == PLAT_BIND_KEY) return *token ? -1 : 0;
    if (*token && !token_mask(token)) return -1;
    snprintf(bind_pad[player][button], sizeof bind_pad[0][0], "%s", token);
    return 0;
}

void plat_binding_label(int slot, const char *token, char *out, int size)
{
    int i;
    (void)slot;
    for (i = 0; i < NPSP; i++)
        if (!strcmp(token, psp_buttons[i].tok)) { snprintf(out, (size_t)size, "%s", psp_buttons[i].label); return; }
    snprintf(out, (size_t)size, "--");
}

void plat_capture_start(int player)
{
    capturing = player;
    capture_done = 0;
    capture_before = held;
}

void plat_capture_cancel(void) { capturing = -1; capture_done = 0; }

int plat_capture_poll(int *slot, char *token, int size)
{
    if (!capture_done) return 0;
    capture_done = 0;
    capturing = -1;
    if (!capture_token[0]) return -1;
    *slot = PLAT_BIND_PAD;
    snprintf(token, (size_t)size, "%s", capture_token);
    return 1;
}

static void read_controls(void)
{
    SceCtrlData c;
    unsigned h;
    sceCtrlPeekBufferPositive(&c, 1);
    h = c.Buttons;
    /* the stick as the d-pad */
    if (c.Lx < 64) h |= PSP_CTRL_LEFT;
    if (c.Lx > 192) h |= PSP_CTRL_RIGHT;
    if (c.Ly < 64) h |= PSP_CTRL_UP;
    if (c.Ly > 192) h |= PSP_CTRL_DOWN;
    if (capturing >= 0) {
        unsigned now = h & ~capture_before;
        int i;
        capture_before = h;
        if (now & PSP_CTRL_SELECT) { capture_token[0] = 0; capture_done = 1; }   /* (Select: cancel) */
        else
            for (i = 0; i < NPSP; i++)
                if (now & psp_buttons[i].mask) {
                    snprintf(capture_token, sizeof capture_token, "%s", psp_buttons[i].tok);
                    capture_done = 1;
                    break;
                }
        h = 0;                  /* (the game sees nothing while capturing) */
    }
    held = h;
}

int plat_poll(uint16_t pad[2])
{
    int p, b;
    read_controls();
    pad[0] = pad[1] = 0;
    for (p = 0; p < 2; p++)
        for (b = 0; b < NBUTTONS; b++)
            if (bind_pad[p][b][0] && (held & token_mask(bind_pad[p][b]))) pad[p] |= (uint16_t)(1u << b);
    if (held & PSP_CTRL_SELECT) pad[0] |= PAD_BACK;
    return !quit;
}

uint16_t plat_menu_input(void)
{
    uint16_t m = 0;
    if (capturing >= 0) return 0;
    if (held & PSP_CTRL_UP) m |= PAD_UP;
    if (held & PSP_CTRL_DOWN) m |= PAD_DOWN;
    if (held & PSP_CTRL_LEFT) m |= PAD_LEFT;
    if (held & PSP_CTRL_RIGHT) m |= PAD_RIGHT;
    if (held & (PSP_CTRL_CROSS | PSP_CTRL_START)) m |= MENU_OK;
    if (held & (PSP_CTRL_CIRCLE | PSP_CTRL_SELECT)) m |= MENU_BACK;
    return m;
}

int plat_text_input(char *buf, int cap) { (void)buf; (void)cap; return 0; }

/* ---- the screen ------------------------------------------------------------------ */

static int clock_pacing, pace_adjust_us;
static uint64_t next_us;

void plat_pacing(int by_clock, int adjust_us)
{
    if (by_clock != clock_pacing) {
        clock_pacing = by_clock;
        gfx_gu_vsync(!by_clock);
        next_us = sceKernelGetSystemTimeWide();
    }
    pace_adjust_us = by_clock ? adjust_us : 0;
}

/* (perf.log next to EBOOT.PBP, while a file perf.on is there: where each second's
 * time went - the game, drawing, waiting - for tuning on the real thing) */
/* (The log is written in short appends, each file opened and closed again: the
 * Memory Stick only records a file's length when it is closed, and quitting with
 * Home closes nothing - an open log comes back empty. The time a write takes is
 * left out of the figures.) */
static int perf_on = -1;
static double prof_read_us;   /* (TJE_PROF: one clock read) */
static char perf_path[300];
static char perf_text[2048];
static int perf_len;
static uint64_t perf_last, perf_game, perf_draw, perf_wait;
static unsigned perf_frames;
static uint32_t perf_write_us;   /* (the last write, charged to "present" by the profiler) */
static int perf_lists;

static void perf_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void perf_printf(const char *fmt, ...)
{
    va_list ap;
    int n;
    if (perf_len >= (int)sizeof perf_text - 1) return;
    va_start(ap, fmt);
    n = vsnprintf(perf_text + perf_len, sizeof perf_text - (size_t)perf_len, fmt, ap);
    va_end(ap);
    if (n > 0) perf_len += n;
    if (perf_len > (int)sizeof perf_text - 1) perf_len = (int)sizeof perf_text - 1;
}

static void perf_write(void)
{
    uint32_t t0 = sceKernelGetSystemTimeLow();
    SceUID fd = sceIoOpen(perf_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, perf_text, (SceSize)perf_len);
        sceIoClose(fd);
    }
    perf_len = 0;
    perf_write_us = sceKernelGetSystemTimeLow() - t0;
}

/* (at the top of perf.log: the clocks, the sound setting, and a few fixed loops
 * timed - how fast this PSP runs plain code and memory, against PPSSPP's numbers) */
static volatile uint32_t bench_sink;
static uint8_t bench_mem[1 << 20] __attribute__((aligned(64)));

static void perf_header(void)
{
    uint32_t t0, x = 1, i, j;
    {
        /* (the largest block malloc still gives) */
        size_t lo = 0, hi = 64u << 20;
        while (hi - lo > 16384) {
            size_t mid = (lo + hi) / 2;
            void *p = malloc(mid);
            if (p) { free(p); lo = mid; } else hi = mid;
        }
        perf_printf("cpu %d MHz, bus %d MHz; music chip %d; heap free %u KB, system free %u KB\n",
                    scePowerGetCpuClockFrequencyInt(), scePowerGetBusClockFrequencyInt(), g_options.fm_sound,
                    (unsigned)(lo / 1024), (unsigned)(sceKernelTotalFreeMemSize() / 1024));
    }
    perf_write();
    t0 = sceKernelGetSystemTimeLow();
    for (i = 0; i < 20000000u; i++) x = x * 1103515245u + 12345u;
    bench_sink = x;
    perf_printf("bench: 20M multiply-adds %u ms\n", (sceKernelGetSystemTimeLow() - t0) / 1000);
    t0 = sceKernelGetSystemTimeLow();
    for (j = 0; j < 200; j++)
        for (i = 0; i < 8192; i += 4) x += *(volatile uint32_t *)(bench_mem + i);
    bench_sink = x;
    perf_printf("bench: 8 KB read 200 times (cached) %u ms\n", (sceKernelGetSystemTimeLow() - t0) / 1000);
    t0 = sceKernelGetSystemTimeLow();
    for (j = 0; j < 16; j++)
        for (i = 0; i < sizeof bench_mem; i += 64) x += *(volatile uint32_t *)(bench_mem + ((i * 97) & (sizeof bench_mem - 64)));
    bench_sink = x;
    perf_printf("bench: 256K cache misses %u ms\n", (sceKernelGetSystemTimeLow() - t0) / 1000);
    t0 = sceKernelGetSystemTimeLow();
    for (j = 0; j < 16; j++) memcpy(bench_mem + (512 << 10), bench_mem, 512 << 10);
    perf_printf("bench: 8 MB memcpy %u ms\n", (sceKernelGetSystemTimeLow() - t0) / 1000);
#ifdef TJE_PROF
    perf_printf("profile: one frame in %d measured; a clock read %.2f us (taken out)\n", PROF_EVERY, prof_read_us);
#endif
    perf_write();
}

static void perf_end(uint64_t t_start, uint64_t t_drawn, uint64_t t_end)
{
    if (perf_on < 0) {
        char path[300];
        FILE *on;
        snprintf(path, sizeof path, "%sperf.on", data_dir);
        on = fopen(path, "r");
        perf_on = on != NULL;
        if (on) {
            fclose(on);
            snprintf(perf_path, sizeof perf_path, "%sperf.log", data_dir);
            sceIoRemove(perf_path);
        }
    }
    if (!perf_on) return;
    if (!perf_last) {
        perf_header();
        perf_last = sceKernelGetSystemTimeWide();
        return;
    }
    perf_game += t_start - perf_last;
    perf_draw += t_drawn - t_start;
    perf_wait += t_end - t_drawn;
    perf_last = t_end;
    if (++perf_frames == 60) {
        static unsigned seconds;
        perf_printf("%4u s: game %.2f ms, draw %.2f, wait %.2f (a frame: 16.68)%s\n", ++seconds,
                    perf_game / 60000.0, perf_draw / 60000.0, perf_wait / 60000.0, perf_lists ? " [GE gameplay]" : "");
#ifdef TJE_PROF
        {
            static const char *names[PROF_N] = {"68k", "vdp lines", "z80", "fm", "sound other", "view", "present", "hooks", "(list", "overlays", "ge)"};
            int k;
            { uint32_t w = perf_write_us / PROF_EVERY; prof_us[PROF_PRESENT] -= prof_us[PROF_PRESENT] > w ? w : prof_us[PROF_PRESENT]; }
            perf_printf("   ");
            for (k = 0; k < PROF_N; k++) {
                {
                    /* (less the clock reads' own time, measured at the start) */
                    double us = (double)prof_us[k] - prof_reads[k] * prof_read_us;
                    perf_printf(" %s %.2f", names[k], (us > 0 ? us : 0) * PROF_EVERY / 60000.0);
                }
                prof_us[k] = 0;
                prof_reads[k] = 0;
            }
            perf_printf("\n");
        }
#endif
        perf_lists = 0;
        perf_frames = 0;
        perf_game = perf_draw = perf_wait = 0;
        perf_write();
        perf_last = sceKernelGetSystemTimeWide();   /* (the write left out) */
    }
}

/* (tests: vlcheck.on next to EBOOT.PBP - each view drawn by the GE compared with
 * the same list drawn in software, gfx/viewlist_soft.c) */
static void list_check(void)
{
    static int on = -1, frames, bad_frames;
    static uint32_t ref[480 * 272];
    const ViewList *vl = gameview_last_list();
    const uint32_t *fb;
    int x, y, ox, oy;
    long bad = 0;
    if (on < 0) {
        char path[300];
        FILE *f;
        snprintf(path, sizeof path, "%svlcheck.on", data_dir);
        f = fopen(path, "r");
        on = f != NULL;
        if (f) fclose(f);
    }
    if (!on || vl->w > 480 || vl->h > 272) return;
    viewlist_render_soft(vl, ref, 480);
    fb = gfx_gu_drawn();
    ox = (SCR_W - vl->w) / 2;
    oy = (SCR_H - vl->h) / 2;
    for (y = 0; y < vl->h; y++)
        for (x = 0; x < vl->w; x++) {
            uint32_t g = fb[(y + oy) * 512 + x + ox], r = ref[y * 480 + x];
            uint32_t g_rgb = ((g & 0xFF) << 16) | (g & 0xFF00) | ((g >> 16) & 0xFF);
            if (g_rgb != (r & 0xFFFFFF)) {
                if (!bad && bad_frames < 3) printf("vlcheck: first difference at %d,%d: GE %06X, list %06X\n", x, y, g_rgb, r & 0xFFFFFF);
                bad++;
            }
        }
    frames++;
    if (bad) { bad_frames++; printf("vlcheck: %ld pixels differ\n", bad); }
    if (frames % 120 == 0) printf("vlcheck: %d views, %d differ\n", frames, bad_frames);
}

void plat_present(const uint32_t *pixels, int width, int height, int pitch)
{
    /* the frame before (the GE drew it while this one ran) is shown first; this
     * one is then given to the GE, to be shown next time */
    uint64_t t_start = sceKernelGetSystemTimeWide(), t_shown, t_given;
    gfx_gu_show();
    t_shown = sceKernelGetSystemTimeWide();
    present_set_frame(pixels, width, height, pitch);
    present_draw(SCR_W, SCR_H);
    t_given = sceKernelGetSystemTimeWide();
    if (!pixels) { list_check(); perf_lists = 1; }
    if (clock_pacing) {
        /* (network play: by the clock, plat_pacing) */
        uint64_t now = sceKernelGetSystemTimeWide();
        next_us += 16683 + pace_adjust_us;
        if (now > next_us + 67000 || next_us > now + 67000) next_us = now;
        else if (next_us > now) sceKernelDelayThread((SceUInt)(next_us - now));
    }
    /* ("draw": giving the GE this frame; "wait": for the GE and the vertical blank) */
    t_shown = t_start + (t_given - t_shown);
    perf_end(t_start, t_shown, sceKernelGetSystemTimeWide());
    {
        static unsigned n;
        if (++n % 300 == 0) printf("tje: %u frames, %u ms\n", n, plat_ms());
    }
}

/* ---- the rest ---------------------------------------------------------------------- */

const char *plat_data_dir(void) { return data_dir; }

/* (testing: start.gsx next to EBOOT.PBP starts the game from that save state) */
const char *plat_start_state(void)
{
    static char path[300];
    FILE *f;
    snprintf(path, sizeof path, "%sstart.gsx", data_dir);
    f = fopen(path, "rb");
    if (!f) return NULL;
    fclose(f);
    return path;
}
const char *plat_rom_path(void) { return rom_path; }

void plat_message(const char *title, const char *text)
{
    SceCtrlData c;
    pspDebugScreenInit();
    pspDebugScreenPrintf("%s\n\n%s\n\nPress X to quit.\n", title, text);
    do { sceCtrlReadBufferPositive(&c, 1); } while (!(c.Buttons & PSP_CTRL_CROSS) && !quit);
}

uint32_t plat_ms(void) { return (uint32_t)(sceKernelGetSystemTimeWide() / 1000); }
void plat_sleep_ms(int ms) { sceKernelDelayThread((SceUInt)ms * 1000u); }

void plat_net_name(char *out, int size)
{
    char nick[128] = "";
    if (sceUtilityGetSystemParamString(PSP_SYSTEMPARAM_ID_STRING_NICKNAME, nick, sizeof nick) < 0 || !nick[0])
        snprintf(nick, sizeof nick, "PSP");
    snprintf(out, (size_t)size, "%s", nick);
}

/* network play: net_adhoc.c */

int plat_init(int argc, char **argv)
{
    int i, th;
    /* the folder EBOOT.PBP is in */
    snprintf(data_dir, sizeof data_dir, "%s", argc > 0 && argv[0] ? argv[0] : "ms0:/PSP/GAME/TJE/EBOOT.PBP");
    for (i = (int)strlen(data_dir) - 1; i >= 0 && data_dir[i] != '/'; i--) {}
    data_dir[i + 1] = 0;
    snprintf(rom_path, sizeof rom_path, "%stje.bin", data_dir);
    for (i = 1; i < argc; i++) if (argv[i][0] != '-') snprintf(rom_path, sizeof rom_path, "%s", argv[i]);

    th = sceKernelCreateThread("callbacks", callback_thread, 0x11, 0xFA0, 0, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
    scePowerSetClockFrequency(333, 333, 166);
#ifdef TJE_PROF
    prof_clock = (uint32_t (*)(void))sceKernelGetSystemTimeLow;
    {
        /* (what a clock read costs, to take out of the figures) */
        uint32_t t0 = sceKernelGetSystemTimeLow(), t1;
        int i;
        for (i = 0; i < 2000; i++) (void)sceKernelGetSystemTimeLow();
        t1 = sceKernelGetSystemTimeLow();
        prof_read_us = (double)(t1 - t0) / 2000.0;
    }
#endif
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    plat_default_bindings(0);
    plat_default_bindings(1);

    /* (tests: nettest.on next to EBOOT.PBP - network log and memory checksums there) */
    {
        static char path[300], hash[300];
        FILE *f;
        snprintf(path, sizeof path, "%snettest.on", data_dir);
        if ((f = fopen(path, "rb")) != NULL) {
            fclose(f);
            snprintf(hash, sizeof hash, "%snet.hash", data_dir);
            setenv("TJE_NET_LOG", "1", 1);
            setenv("TJE_NET_HASHLOG", hash, 1);
        }
    }
    present_init(gfx_gu_init());
    {
        extern int g_gv_use_list;
        g_gv_use_list = present_has_lists();   /* gameplay drawn by the GE from lists */
    }
    snd_music_external = 1;   /* (the audio thread mixes the music) */
    music_threaded = 1;       /* (and this one reads it ahead) */
    th = sceKernelCreateThread("music", music_thread, 0x30, 0x8000, PSP_THREAD_ATTR_USER, NULL);   /* (below the game's: it runs while the game waits) */
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
    th = sceKernelCreateThread("audio", audio_thread, 0x12, 0x10000, PSP_THREAD_ATTR_USER, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
    return 0;
}

void plat_shutdown(void)
{
    quit = 1;
    sceGuTerm();
    sceKernelExitGame();
}
