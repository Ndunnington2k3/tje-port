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

#include <pspkernel.h>
#include <pspctrl.h>
#include <pspaudio.h>
#include <psppower.h>
#include <psputility.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspgu.h>
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
static int16_t ring[RING * 2];
static volatile unsigned ring_r, ring_w;  /* in stereo frames, wrapping */
static int audio_on = 1;

static int audio_thread(SceSize args, void *argp)
{
    static int16_t block[AUDIO_CHUNK * 2];
    (void)args; (void)argp;
    if (sceAudioSRCChReserve(AUDIO_CHUNK, 48000, 2) < 0) return 0;
    while (!quit) {
        unsigned have = ring_w - ring_r, i;
        if (have >= AUDIO_CHUNK) {
            for (i = 0; i < AUDIO_CHUNK; i++) {
                unsigned k = (ring_r + i) % RING;
                block[i * 2] = ring[k * 2];
                block[i * 2 + 1] = ring[k * 2 + 1];
            }
            ring_r += AUDIO_CHUNK;
        } else {
            memset(block, 0, sizeof block);  /* (not enough yet: a moment of silence) */
        }
        sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, block);
    }
    sceAudioSRCChRelease();
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
static FILE *perf_log;
static int perf_checked;
static uint64_t perf_last, perf_game, perf_draw, perf_wait;
static unsigned perf_frames;

static void perf_end(uint64_t t_start, uint64_t t_drawn, uint64_t t_end)
{
    if (!perf_checked) {
        char path[300];
        FILE *on;
        perf_checked = 1;
        snprintf(path, sizeof path, "%sperf.on", data_dir);
        on = fopen(path, "r");
        if (on) {
            fclose(on);
            snprintf(path, sizeof path, "%sperf.log", data_dir);
            perf_log = fopen(path, "w");
        }
    }
    if (!perf_log) return;
    if (perf_last) perf_game += t_start - perf_last;
    perf_draw += t_drawn - t_start;
    perf_wait += t_end - t_drawn;
    perf_last = t_end;
    if (++perf_frames == 60) {
        fprintf(perf_log, "60 frames: game %.2f ms, draw %.2f, wait %.2f (a frame: 16.68)\n",
                perf_game / 60000.0, perf_draw / 60000.0, perf_wait / 60000.0);
        fflush(perf_log);
        perf_frames = 0;
        perf_game = perf_draw = perf_wait = 0;
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
    uint64_t t_start = sceKernelGetSystemTimeWide(), t_drawn;
    present_set_frame(pixels, width, height, pitch);
    present_draw(SCR_W, SCR_H);
    t_drawn = sceKernelGetSystemTimeWide();
    if (!pixels) list_check();
    gfx_gu_show();
    if (clock_pacing) {
        /* (network play: by the clock, plat_pacing) */
        uint64_t now = sceKernelGetSystemTimeWide();
        next_us += 16683 + pace_adjust_us;
        if (now > next_us + 67000 || next_us > now + 67000) next_us = now;
        else if (next_us > now) sceKernelDelayThread((SceUInt)(next_us - now));
    }
    perf_end(t_start, t_drawn, sceKernelGetSystemTimeWide());
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

/* network play over ad hoc: to come (platform_psp_net.c) */
int plat_net_open(void) { return -1; }
void plat_net_close(void) {}
int plat_net_send(const PlatNetAddr *to, const void *d, int n) { (void)to; (void)d; (void)n; return -1; }
int plat_net_broadcast(const void *d, int n) { (void)d; (void)n; return -1; }
int plat_net_recv(PlatNetAddr *from, void *buf, int cap) { (void)from; (void)buf; (void)cap; return 0; }
int plat_net_addr_equal(const PlatNetAddr *a, const PlatNetAddr *b) { return !memcmp(a->b, b->b, 6); }
void plat_net_addr_text(const PlatNetAddr *a, char *out, int size)
{
    snprintf(out, (size_t)size, "%02X:%02X:%02X:%02X:%02X:%02X", a->b[0], a->b[1], a->b[2], a->b[3], a->b[4], a->b[5]);
}

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
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    plat_default_bindings(0);
    plat_default_bindings(1);

    present_init(gfx_gu_init());
    {
        extern int g_gv_use_list;
        g_gv_use_list = present_has_lists();   /* gameplay drawn by the GE from lists */
    }
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
