/*
 * Headless platform for automated testing.
 *
 *   tje_headless <rom> [options]
 *     --frames N           stop after N frames (default 600)
 *     --shot F:path.ppm    save frame F as a PPM image (repeatable)
 *     --press F:N:buttons  hold buttons from frame F for N frames, pad 1
 *                          (buttons: any of U D L R A B C S, E = Escape; on menus
 *                          A/C/S = choose, B/E = back)
 *     --random SEED        after frame 500, mash random buttons (Start only rarely)
 *     --realtime           run at 60 frames a second (network play tests)
 *     --random-from F      start the random buttons at frame F (500)
 *     --random-until F     stop the random buttons at frame F
 *     --type F:text        type text at frame F (keyboard text entry)
 *     --dumpram F:path     save the 64 KB of work RAM at frame F (repeatable)
 *     --dumpvram F:path    save VRAM, CRAM and the VDP registers at frame F
 *     --wav path.wav       emulate sound and record it to a WAV file
 *     --record-music DIR   record every song for the native sound (music_record.c)
 *     --audio              emulate sound (Z80, FM, PSG) without recording it
 *     --save F:slot        at frame F, save the game to slot 0-2 (port save feature)
 *     --load F:slot        at frame F, load slot 0-2
 *     --poke F:addr:val    at frame F, write byte val to address addr (hex), e.g. 100:FFE212:0
 *     --capture key:NAME|pad:NAME   answer the next input-binding capture with this (repeatable)
 *     --elevator N         every N frames, put ToeJam in the elevator (ELEVATOR_FLAG = 1)
 *                          (test hook: exercises level transitions)
 *     --window WxH         draw every frame into a window of this size with the software
 *                          backend of the drawing layer (src/gfx), as the real program
 *                          would; --shot then saves the window's picture
 *     --gvcheck PREFIX     compare the gameplay view drawn from game memory (gameview.c)
 *                          with the VDP picture every frame; report differences and save
 *                          the first few as PREFIX_<frame>_vdp.ppm / _gv.ppm
 */
#include "platform/platform.h"
#include "core/genesis.h"
#include "core/m68k_rt.h"
#include "core/vdp.h"
#include "game/save.h"
#include "gfx/gfx_soft.h"
#include "gfx/present.h"
#include "game/gameview.h"
#include "gfx/viewlist.h"
#include "game/viewdiag.h"
extern int g_gv_gameplay;
extern int g_gv_win_top_seen;
static int g_gv_win_top_seen_ext(void) { return g_gv_win_top_seen; }

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#define MAX_EVENTS 1024

static const char *rom_path = "tje.bin";
static unsigned max_frames = 600;
static unsigned frame;
static unsigned gv_split_frames;
static struct { unsigned frame; char path[256]; } shots[MAX_EVENTS];
static int nshots;
static int tw_w, tw_h;   /* --two-windows */
static struct { unsigned frame; char path[256]; } dumps[MAX_EVENTS];
static int ndumps;
static struct { unsigned frame; char path[256]; } vdumps[MAX_EVENTS];
static int nvdumps;
static struct { unsigned frame, len; uint16_t buttons; int pad; } presses[MAX_EVENTS];
static int npresses;
static unsigned random_seed;
static uint16_t random_buttons;
static unsigned random_left;
static unsigned elevator_period;
static unsigned random_until, random_from = 500;
static int realtime;
static struct { unsigned frame; char text[64]; int done; } types[8];
static int ntypes;
static struct { unsigned frame; int slot, load; } saves[MAX_EVENTS];
static int nsaves;
static int window_w, window_h;   /* --window */
static const char *gvcheck;      /* --gvcheck */
static unsigned gv_frames, gv_bad_frames, gv_saved;
static unsigned long gv_bad_pixels;
static unsigned gv_reasons[16];
extern int g_gameview_reason;
static char captures[16][32];
static int ncaptures, next_capture, capturing, capture_ready;
static char bindings[2][8][2][32];
void record_music_init(const char *dir);
int record_music_active(void);
void record_music_audio(const int16_t *samples, int n);
void record_music_frame(void);

static struct { unsigned frame, addr, val; } pokes[MAX_EVENTS];
static int npokes;
static FILE *wav;
static int audio_flag;
static uint32_t wav_frames;


static uint16_t parse_buttons(const char *s)
{
    uint16_t b = 0;
    for (; *s; s++) {
        switch (*s) {
        case 'U': b |= PAD_UP; break;
        case 'D': b |= PAD_DOWN; break;
        case 'L': b |= PAD_LEFT; break;
        case 'R': b |= PAD_RIGHT; break;
        case 'A': b |= PAD_A; break;
        case 'B': b |= PAD_B; break;
        case 'C': b |= PAD_C; break;
        case 'S': b |= PAD_START; break;
        case 'E': b |= PAD_BACK; break;     /* Escape */
        }
    }
    return b;
}

int plat_init(int argc, char **argv)
{
    int i;
    if (getenv("TJE_USE_LIST")) {   /* (profiling: views drawn as lists, as on the PSP) */
        extern int g_gv_use_list;
        g_gv_use_list = 1;
    }
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc && nshots < MAX_EVENTS) {
            const char *a = argv[++i];
            const char *c = strchr(a, ':');
            if (!c) continue;
            shots[nshots].frame = (unsigned)atoi(a);
            snprintf(shots[nshots].path, sizeof shots[nshots].path, "%s", c + 1);
            nshots++;
        } else if ((!strcmp(argv[i], "--press") || !strcmp(argv[i], "--press2")) && i + 1 < argc && npresses < MAX_EVENTS) {
            unsigned f = 0, n = 0;
            char btn[32] = {0};
            presses[npresses].pad = argv[i][7] == '2';   /* (--press2: player 2's pad) */
            if (sscanf(argv[++i], "%u:%u:%31s", &f, &n, btn) == 3) {
                presses[npresses].frame = f;
                presses[npresses].len = n;
                presses[npresses].buttons = parse_buttons(btn);
                npresses++;
            }
        } else if (!strcmp(argv[i], "--dumpram") && i + 1 < argc && ndumps < MAX_EVENTS) {
            const char *a = argv[++i];
            const char *c = strchr(a, ':');
            if (!c) continue;
            dumps[ndumps].frame = (unsigned)atoi(a);
            snprintf(dumps[ndumps].path, sizeof dumps[ndumps].path, "%s", c + 1);
            ndumps++;
        } else if ((!strcmp(argv[i], "--save") || !strcmp(argv[i], "--load")) && i + 1 < argc && nsaves < MAX_EVENTS) {
            unsigned f = 0;
            int slot = 0;
            saves[nsaves].load = argv[i][2] == 'l';
            if (sscanf(argv[++i], "%u:%d", &f, &slot) == 2) {
                saves[nsaves].frame = f;
                saves[nsaves].slot = slot;
                nsaves++;
            }
        } else if (!strcmp(argv[i], "--poke") && i + 1 < argc && npokes < MAX_EVENTS) {
            if (sscanf(argv[++i], "%u:%x:%x", &pokes[npokes].frame, &pokes[npokes].addr, &pokes[npokes].val) == 3)
                npokes++;
        } else if (!strcmp(argv[i], "--dumpvram") && i + 1 < argc && nvdumps < MAX_EVENTS) {
            const char *a = argv[++i];
            const char *c = strchr(a, ':');
            if (!c) continue;
            vdumps[nvdumps].frame = (unsigned)atoi(a);
            snprintf(vdumps[nvdumps].path, sizeof vdumps[nvdumps].path, "%s", c + 1);
            nvdumps++;
        } else if (!strcmp(argv[i], "--capture") && i + 1 < argc && ncaptures < 16) {
            snprintf(captures[ncaptures++], sizeof captures[0], "%s", argv[++i]);
        } else if (!strcmp(argv[i], "--audio")) {
            audio_flag = 1;
        } else if (!strcmp(argv[i], "--record-music") && i + 1 < argc) {
            record_music_init(argv[++i]);
        } else if (!strcmp(argv[i], "--wav") && i + 1 < argc) {
            wav = fopen(argv[++i], "wb");
            if (wav) { static const uint8_t hdr[44] = {0}; fwrite(hdr, 1, 44, wav); }
        } else if (!strcmp(argv[i], "--type") && i + 1 < argc && ntypes < 8) {
            const char *a = argv[++i], *c = strchr(a, ':');
            if (c) {
                types[ntypes].frame = (unsigned)atoi(a);
                snprintf(types[ntypes].text, sizeof types[0].text, "%s", c + 1);
                types[ntypes++].done = 0;
            }
        } else if (!strcmp(argv[i], "--realtime")) {
            realtime = 1;
        } else if (!strcmp(argv[i], "--random-from") && i + 1 < argc) {
            random_from = (unsigned)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--random-until") && i + 1 < argc) {
            random_until = (unsigned)atoi(argv[++i]);   /* (random input stops at this frame) */
        } else if (!strcmp(argv[i], "--elevator") && i + 1 < argc) {
            elevator_period = (unsigned)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--gvcheck") && i + 1 < argc) {
            gvcheck = argv[++i];   /* (the gameplay view is on by default: options.c) */
            
        } else if (!strcmp(argv[i], "--window") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &window_w, &window_h) == 2 && window_w > 0 && window_h > 0)
                present_init(gfx_soft_init());
            else
                window_w = window_h = 0;
        } else if (!strcmp(argv[i], "--view-log") && i + 1 < argc) {
            char cap[600];
            snprintf(cap, sizeof cap, "%s_capture", argv[i + 1]);
            viewdiag_open(argv[++i], cap);
        } else if (!strcmp(argv[i], "--two-windows") && i + 1 < argc) {
            /* each player's own picture as well, views of WxH game pixels; --shot
             * also saves them (path.p1.ppm, path.p2.ppm) */
            if (sscanf(argv[++i], "%dx%d", &tw_w, &tw_h) == 2) g_gv_windows = 1;
            if (getenv("TJE_AS_NET_PLAYER")) {   /* (profiling: player 1's window drawn as lists, as on the PSP) */
                extern int g_gv_net_player, g_gv_use_list;
                g_gv_net_player = 0;
                g_gv_use_list = 1;
            }
        } else if (!strcmp(argv[i], "--random") && i + 1 < argc) {
            random_seed = (unsigned)atoi(argv[++i]) * 2654435761u + 1;
        } else if (argv[i][0] != '-') rom_path = argv[i];
        else if (i + 1 < argc && argv[i + 1][0] != '-') i++;  /* skip value of unknown/overflowing option */
    }
    {
        /* (checks that compare with the VDP's picture need all of its lines) */
        extern int g_gv_need_vdp;
        if (gvcheck || getenv("TJE_VIEWCHECK") || getenv("TJE_BARCHECK") || getenv("TJE_WIDEBAR") ||
            getenv("TJE_GV_SYNC") || getenv("TJE_ALL_VDP_LINES"))
            g_gv_need_vdp = 1;
    }
    return 0;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

void plat_shutdown(void)
{
    if (gvcheck) {
        int r;
        fprintf(stderr, "gvcheck: %u gameplay frames drawn (%u split), %u differ (%lu pixels)\n",
                gv_frames, gv_split_frames, gv_bad_frames, gv_bad_pixels);
        fprintf(stderr, "gvcheck: not drawn:");
        for (r = 0; r < 16; r++) if (gv_reasons[r]) fprintf(stderr, " [%d] %u", r, gv_reasons[r]);
        fprintf(stderr, "\n");
    }
    if (wav) {
        uint8_t h[44];
        memcpy(h, "RIFF\0\0\0\0WAVEfmt ", 16);
        put32(h + 4, 36 + wav_frames * 4);
        put32(h + 16, 16);
        h[20] = 1; h[21] = 0; h[22] = 2; h[23] = 0;      /* PCM, stereo */
        put32(h + 24, 48000);
        put32(h + 28, 48000 * 4);
        h[32] = 4; h[33] = 0; h[34] = 16; h[35] = 0;
        memcpy(h + 36, "data", 4);
        put32(h + 40, wav_frames * 4);
        fseek(wav, 0, SEEK_SET);
        fwrite(h, 1, 44, wav);
        fclose(wav);
        wav = NULL;
    }
}

int plat_audio_enabled(void) { return wav != NULL || audio_flag || record_music_active(); }

void plat_audio(const int16_t *samples, int frames)
{
    record_music_audio(samples, frames);
    int i;
    if (!wav) return;
    for (i = 0; i < frames * 2; i++) {  /* little-endian int16 */
        fputc(samples[i] & 0xFF, wav);
        fputc((samples[i] >> 8) & 0xFF, wav);
    }
    wav_frames += (uint32_t)frames;
}

const char *plat_rom_path(void) { return rom_path; }

/* Bindings are only stored here (input comes from the command line). */
static const char *const def_bind[2][8][2] = {
    {{"Up", "dpup"}, {"Down", "dpdown"}, {"Left", "dpleft"}, {"Right", "dpright"},
     {"X", "b"}, {"C", "x"}, {"Z", "a"}, {"Return", "start"}},
    {{"W", "dpup"}, {"S", "dpdown"}, {"A", "dpleft"}, {"D", "dpright"},
     {"K", "b"}, {"L", "x"}, {"J", "a"}, {"Right Shift", "start"}},
};
void plat_default_bindings(int player)
{
    int b, k;
    for (b = 0; b < 8; b++)
        for (k = 0; k < 2; k++) snprintf(bindings[player][b][k], sizeof bindings[0][0][0], "%s", def_bind[player][b][k]);
}
const char *plat_binding(int player, int button, int slot)
{
    static int init;
    if (!init) { init = 1; plat_default_bindings(0); plat_default_bindings(1); }
    if (player < 0 || player > 1 || button < 0 || button > 7 || slot < 0 || slot > 1) return "";
    return bindings[player][button][slot];
}
int plat_set_binding(int player, int button, int slot, const char *token)
{
    plat_binding(0, 0, 0);
    if (player < 0 || player > 1 || button < 0 || button > 7 || slot < 0 || slot > 1) return -1;
    snprintf(bindings[player][button][slot], sizeof bindings[0][0][0], "%s", token);
    return 0;
}
void plat_binding_label(int slot, const char *token, char *out, int size)
{
    if (!*token) snprintf(out, (size_t)size, "--");
    else snprintf(out, (size_t)size, "%s%s", slot == 1 ? "Pad " : "", token);
}
void plat_capture_start(int player) { (void)player; capturing = 1; capture_ready = 0; }
void plat_capture_cancel(void) { capturing = 0; }
/* Like a real key press: the capture completes once a --press button is down
 * while capturing (that button stands for the key being bound). */
int plat_capture_poll(int *slot, char *token, int size)
{
    const char *c;
    if (!capturing || !capture_ready) return 0;
    capturing = 0;
    if (next_capture >= ncaptures) return -1;
    c = captures[next_capture++];
    *slot = !strncmp(c, "pad:", 4);
    snprintf(token, (size_t)size, "%s", strchr(c, ':') ? strchr(c, ':') + 1 : c);
    return 1;
}
const char *plat_data_dir(void) { return ""; }

/* Menu controls from the same --press letters: directions; A, C, S = OK; B, E = back. */
static uint16_t held_now;
uint16_t plat_menu_input(void)
{
    uint16_t m = held_now & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT);
    if (capturing) return 0;
    if (held_now & (PAD_A | PAD_C | PAD_START)) m |= MENU_OK;
    if (held_now & (PAD_B | PAD_BACK)) m |= MENU_BACK;
    return m;
}

void plat_message(const char *title, const char *text)
{
    fprintf(stderr, "%s: %s\n", title, text);
}

#ifdef TJE_SAT_TRACE
/* (audit builds) for each sprite-table entry: the return addresses on the 68000
 * stack when it was last written (who wrote it) */
uint32_t g_sat_writer[80][4];
void sat_trace_write(uint32_t a)
{
    int e = (int)(((a & 0xFFFFu) - 0xB65Au) / 8), n = 0;
    uint32_t sp = R.a[7];
    int k;
    if (e < 0 || e >= 80) return;
    for (k = 0; k < 64 && n < 4; k++, sp += 2) {
        uint32_t v = rd32(sp);
        if (v >= 0x200 && v < 0x80000 && !(v & 1)) g_sat_writer[e][n++] = v;
    }
    while (n < 4) g_sat_writer[e][n++] = 0;
}
#endif

#ifdef TJE_SAT_TRACE
void sat_vram_write(uint32_t a, uint32_t v, int line, int vblank)
{
    static FILE *f;
    uint32_t sp = R.a[7], ret[3] = {0, 0, 0};
    int k, n = 0;
    if (!getenv("TJE_SAT_VRAM")) return;
    if (!f) f = fopen(getenv("TJE_SAT_VRAM"), "w");
    for (k = 0; k < 64 && n < 3; k++, sp += 2) {
        uint32_t w = rd32(sp);
        if (w >= 0x200 && w < 0x80000 && !(w & 1)) ret[n++] = w;
    }
    fprintf(f, "frame %u line %d vbl %d entry %d +%d = %04X by %05X %05X %05X\n", frame, line, vblank, (int)((a & 0x3FF) / 8),
            (int)(a & 7), v, ret[0], ret[1], ret[2]);
}
#endif

static void save_ppm(const char *path, const uint32_t *px, int w, int h, int pitch)
{
    FILE *f = fopen(path, "wb");
    int x, y;
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            uint32_t c = px[y * pitch + x];
            fputc((c >> 16) & 0xFF, f);
            fputc((c >> 8) & 0xFF, f);
            fputc(c & 0xFF, f);
        }
    fclose(f);
}

static int clock_pacing, pace_adjust_us;

void plat_pacing(int by_clock, int adjust_us)
{
    clock_pacing = by_clock;
    pace_adjust_us = by_clock ? adjust_us : 0;
}

static uint64_t now_us(void)
{
#ifdef _WIN32
    return (uint64_t)plat_ms() * 1000u;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
#endif
}

void plat_present(const uint32_t *pixels, int width, int height, int pitch)
{
    record_music_frame();
    int i;

    if (realtime) {
        /* (--realtime: shown at the next 60 Hz "vertical blank" of a clock all
         * copies share, like windows on one monitor with vsync; TJE_JITTER=ms: the
         * frame's work takes up to that much longer at random, like a busy PC) */
        static int jitter = -1;
        uint32_t now, vb;
        if (jitter < 0) jitter = getenv("TJE_JITTER") ? atoi(getenv("TJE_JITTER")) : 0;
        if (jitter > 0) usleep((useconds_t)(rand() % (jitter + 1)) * 1000u);
        if (clock_pacing) {
            /* (network play: by the clock, plat_pacing) */
            static uint64_t next;
            uint64_t t = now_us();
            next += 16683 + pace_adjust_us;
            if (!next || t > next + 67000 || next > t + 67000) next = t;
            while (now_us() < next) usleep(200);
            goto paced;
        }
        now = plat_ms();
        vb = (uint32_t)(((uint64_t)now * 3 / 50 + 1) * 50 / 3);
        while ((int32_t)(plat_ms() - vb) < 0) usleep(500);
    }
paced:

    if (getenv("TJE_PICHASH")) {
        /* (tests) a hash of every picture shown (and each two-window picture), to
         * compare builds that must draw the same */
        static FILE *hf;
        uint32_t hash = 2166136261u;
        int x, y;
        if (!hf) hf = fopen(getenv("TJE_PICHASH"), "w");
        for (y = 0; y < height; y++)
            for (x = 0; x < width; x++) hash = (hash ^ (pixels[(size_t)y * pitch + x] & 0xFFFFFF)) * 16777619u;
        if (hf) fprintf(hf, "%u %dx%d %08X\n", frame, width, height, hash);
    }
    viewdiag_frame(pixels, width, height, pitch);
    if (getenv("TJE_GV_REASONS")) {
        static int last_split = -1;
        if ((int)rd8(0xFF8010u) != last_split) {
            fprintf(stderr, "frame %u: split %d\n", frame, rd8(0xFF8010u));
            last_split = (int)rd8(0xFF8010u);
        }
        const uint32_t *g_; int a_, b_, c_;
        extern int g_gv_split_menu;
        g_gv_split_menu = 0;
        if (gameview_frame(&g_, &a_, &b_, &c_) && g_gv_split_menu)
            fprintf(stderr, "frame %u: split, menu over half %d\n", frame, g_gv_split_menu);
        else if (!gameview_frame(&g_, &a_, &b_, &c_)) fprintf(stderr, "frame %u: not drawn, reason %d%s (win %02X)\n", frame, g_gameview_reason, rd8(0xFF8010u) ? " split" : "", vdp_reg[18]);
    }
    if (gvcheck) {
        const uint32_t *gv;
        int gw, gh, gp;
        if (!gameview_frame(&gv, &gw, &gh, &gp)) {
            if (gv_reasons[g_gameview_reason & 15]++ == 20) {
                char path[512];
                snprintf(path, sizeof path, "%s_reason%d_%u.ppm", gvcheck, g_gameview_reason, frame);
                save_ppm(path, pixels, width, height, pitch);
            }
        }
        else {
            unsigned long bad = 0;
            int x, y;
            extern int g_gv_kept;
            gv_frames++;
            if (rd8(0xFF8010u)) gv_split_frames++;
            if (gw != 320 || gh != VDP_HEIGHT) goto gv_done;   /* a wider view: nothing to compare */
            if (g_gv_kept) goto gv_done;                       /* the last picture kept (clean transitions) */
            const uint32_t *v = vdp_framebuffer();     /* the VDP's own picture */
            for (y = 0; y < gh; y++)
                for (x = 0; x < gw; x++)
                    if ((gv[y * gp + x] & 0xFFFFFF) != (v[y * VDP_FB_PITCH + x] & 0xFFFFFF)) bad++;
            if (bad) {
                gv_bad_frames++;
                gv_bad_pixels += bad;
                if (gv_saved < 6 || (gv_saved < 40 && (frame % 97 == 0 || getenv("TJE_GV_SAVEALL")))) {
                    char path[512];
                    snprintf(path, sizeof path, "%s_%u_vdp.ppm", gvcheck, frame);
                    save_ppm(path, v, gw, gh, VDP_FB_PITCH);
                    snprintf(path, sizeof path, "%s_%u_gv.ppm", gvcheck, frame);
                    save_ppm(path, gv, gw, gh, gp);
                    gv_saved++;
                    fprintf(stderr, "gvcheck: frame %u: %lu pixels differ\n", frame, bad);
                }
            }
        gv_done:;
        }
    }
    if (getenv("TJE_VIEWCHECK")) {
        /* (tests) the per-player views drawn at the original size against the VDP's
         * picture, gameplay rows only (no status bar, no split divider) */
        extern int g_gv_gameplay, g_gv_view_strict;
        static uint32_t vbuf[320 * 224];
        static unsigned frames_1p, frames_split, bad_frames, saved;
        static unsigned long bad_px;
        const uint32_t *gv;
        int gw, gh, gp;
        extern int g_gv_win_top_seen;
        if (gameview_frame(&gv, &gw, &gh, &gp) && g_gv_gameplay && ((vdp_reg[18] & 0x80) || rd8(0xFF8010u)) && !(vdp_reg[17] & 0x1F) && !g_gv_win_top_seen) {
            int wv = rd8(0xFF8010u) ? 184 : (vdp_reg[18] & 0x1F) * 8;
            const uint32_t *v = vdp_framebuffer();
            unsigned long bad = 0;
            int split = rd8(0xFF8010u) != 0, y, x, half;
            g_gv_view_strict = 1;
            for (half = 0; half < (split ? 2 : 1); half++) {
                if (split && gameview_split_half_shown_as_vdp(half)) continue;
                int top = split ? half * 112 : 0, h = split ? 112 : 224;
                int y0 = split ? (half ? 120 : 0) : 0, y1 = split ? (half ? wv - 1 : 104) : wv - 1;   /* (row 183: the status bar H-interrupt cuts sprites there) */
                gameview_render_view(half, (int16_t)rd16(half ? 0xFF8006u : 0xFF8002u), (int16_t)rd16(half ? 0xFF8008u : 0xFF8004u),
                                     half, h, vbuf + top * 320, 320, h, 320, !split);
                for (y = y0; y < y1; y++)
                    for (x = 0; x < 320; x++)
                        if ((vbuf[y * 320 + x] & 0xFFFFFF) != (v[y * VDP_FB_PITCH + x] & 0xFFFFFF)) bad++;
            }
            {
                if (bad && saved < 30) {
                    char path[512];
                    snprintf(path, sizeof path, "%s_%u_view.ppm", getenv("TJE_VIEWCHECK"), frame);
                    save_ppm(path, vbuf, 320, 224, 320);
                    snprintf(path, sizeof path, "%s_%u_vdp.ppm", getenv("TJE_VIEWCHECK"), frame);
                    save_ppm(path, v, 320, 224, VDP_FB_PITCH);
                    saved++;
                }
            }
            g_gv_view_strict = 0;
            if (split) frames_split++; else frames_1p++;
            if (bad) { bad_frames++; bad_px += bad; fprintf(stderr, "viewcheck: frame %u: %lu pixels differ%s\n", frame, bad, split ? " (split)" : ""); }
        }
        if (frame + 1 == max_frames)
            fprintf(stderr, "viewcheck: %u frames (%u split), %u differ (%lu pixels)\n", frames_1p + frames_split, frames_split, bad_frames, bad_px);
    }
    if (getenv("TJE_GV_SYNC")) {
        extern FILE *g_gv_sync_log;
        if (!g_gv_sync_log) g_gv_sync_log = fopen(getenv("TJE_GV_SYNC"), "w");
        fprintf(g_gv_sync_log, "-- frame %u cam %d,%d new %d,%d\n", frame, (int16_t)rd16(0xFF8002u), (int16_t)rd16(0xFF8004u), (int16_t)rd16(0xFF8036u), (int16_t)rd16(0xFF8038u));
    }
    if (getenv("TJE_BARCHECK") && g_gv_gameplay && !rd8(0xFF8010u) && (vdp_reg[18] & 0x80) &&
        (vdp_reg[18] & 0x1F) * 8 <= 184 && !rd8(0xFF9364u) && !rd8(0xFF9365u)) {
        /* (tests) the status bar built from game memory against the VDP's */
        extern int gameview_test_bar(uint32_t (*out)[320], int own);
        static uint32_t bar[40][320];
        static unsigned checked, bad_frames;
        const uint32_t *v = vdp_framebuffer();
        int n = gameview_test_bar(bar, 0), y, x;
        unsigned long bad = 0;
        for (y = 0; y < n; y++)
            for (x = 0; x < 320; x++)
                if ((bar[y][x] & 0xFFFFFF) != (v[(184 + y) * VDP_FB_PITCH + x] & 0xFFFFFF)) bad++;
        checked++;
        if (bad) {
            if (bad_frames++ < 5) {
                char path[300];
                snprintf(path, sizeof path, "%s_%u_bar.ppm", getenv("TJE_BARCHECK"), frame);
                save_ppm(path, &bar[0][0], 320, n, 320);
                snprintf(path, sizeof path, "%s_%u_vdp.ppm", getenv("TJE_BARCHECK"), frame);
                save_ppm(path, v + 184 * VDP_FB_PITCH, 320, n, VDP_FB_PITCH);
            }
            fprintf(stderr, "barcheck: frame %u: %lu pixels differ\n", frame, bad);
        }
        if (frame + 1 == max_frames) fprintf(stderr, "barcheck: %u frames, %u differ\n", checked, bad_frames);
    }
    if (getenv("TJE_WIDEBAR")) {
        /* (tests) the wide 1P picture's status bar against the VDP's bar lines */
        extern int gameview_test_bar_rows(int *from, int *to, int *row);
        const uint32_t *gv;
        int gw, gh, gp, from, to, row;
        static unsigned checked, bad_frames;
        if (gameview_frame(&gv, &gw, &gh, &gp) && gameview_test_bar_rows(&from, &to, &row) && !g_gv_win_top_seen_ext()) {
            const uint32_t *v = vdp_framebuffer();
            int y, x, ox = (gw - 320) / 2;
            unsigned long bad = 0;
            for (y = from; y < to && y < 183 + 41; y++)
                for (x = 0; x < 320; x++)
                    if (x + ox >= 0 && x + ox < gw &&
                        (gv[(row + y - from) * gp + x + ox] & 0xFFFFFF) != (v[y * VDP_FB_PITCH + x] & 0xFFFFFF)) bad++;
            checked++;
            if (bad) {
                if (bad_frames++ < 6) {
                    char path[300];
                    snprintf(path, sizeof path, "%s_%u_wide.ppm", getenv("TJE_WIDEBAR"), frame);
                    save_ppm(path, gv, gw, gh, gp);
                    snprintf(path, sizeof path, "%s_%u_vdp.ppm", getenv("TJE_WIDEBAR"), frame);
                    save_ppm(path, v, 320, 224, VDP_FB_PITCH);
                }
                fprintf(stderr, "widebar: frame %u: %lu pixels differ\n", frame, bad);
            }
        }
    }
    if (g_gv_windows) {
        int k;
        for (k = 0; k < 2; k++) {
            const uint32_t *px;
            int pp;
            int ww = tw_w, wh = tw_h;
            if (!gameview_window_frame(k, &ww, &wh, &px, &pp)) continue;
            if (!px) {
                /* (a network player's window: a view list - drawn here as the GPU would) */
                static uint32_t *lp;
                const ViewList *vl = gameview_last_list();
                if (!vl) continue;
                if (!lp) lp = (uint32_t *)malloc(sizeof(uint32_t) * GAMEVIEW_MAX_W * GAMEVIEW_MAX_H);
                if (!lp) continue;
                viewlist_render_soft(vl, lp, GAMEVIEW_MAX_W);
                px = lp;
                pp = GAMEVIEW_MAX_W;
                ww = vl->w;
                wh = vl->h;
            }
            if (getenv("TJE_PICHASH")) {
                static FILE *wf;
                uint32_t hash = 2166136261u;
                int x, y;
                if (!wf) {
                    char path[600];
                    snprintf(path, sizeof path, "%s.windows", getenv("TJE_PICHASH"));
                    wf = fopen(path, "w");
                }
                for (y = 0; y < wh; y++)
                    for (x = 0; x < ww; x++) hash = (hash ^ (px[(size_t)y * pp + x] & 0xFFFFFF)) * 16777619u;
                if (wf) fprintf(wf, "%u p%d %dx%d %08X\n", frame, k + 1, ww, wh, hash);
            }
            for (i = 0; i < nshots; i++)
                if (shots[i].frame == frame) {
                    char path[300];
                    snprintf(path, sizeof path, "%s.p%d.ppm", shots[i].path, k + 1);
                    save_ppm(path, px, ww, wh, pp);
                }
        }
    }
    if (window_w > 0) {
        int w, h;
        const uint32_t *win;
        present_set_frame(pixels, width, height, pitch);
        present_draw(window_w, window_h);
        win = gfx_soft_target(&w, &h);
        for (i = 0; i < nshots; i++)
            if (shots[i].frame == frame && win) save_ppm(shots[i].path, win, w, h, w);
    } else if (pixels) {
        for (i = 0; i < nshots; i++)
            if (shots[i].frame == frame) save_ppm(shots[i].path, pixels, width, height, pitch);
    }
    for (i = 0; i < nshots; i++)
        if (shots[i].frame == frame && getenv("TJE_SHOT_VDP")) {
            /* (tests) the VDP's own picture too */
            char path[300];
            snprintf(path, sizeof path, "%s.vdp.ppm", shots[i].path);
            save_ppm(path, vdp_framebuffer(), 320, 224, VDP_FB_PITCH);
        }
    for (i = 0; i < ndumps; i++)
        if (dumps[i].frame == frame) {
            FILE *f = fopen(dumps[i].path, "wb");
            if (f) { fwrite(g_ram, 1, sizeof g_ram, f); fclose(f); }
        }
    if (getenv("TJE_TRACE_VDP")) {
        uint32_t st[5];
        vdp_stats_on = 1;
        vdp_frame_stats(st);
        fprintf(stderr, "v%5u tiles %5u A %5u B %5u sat %4u off %3u reg1 %02X\n", frame, st[0], st[1], st[2], st[3], st[4], vdp_reg[1]);
    }
    for (i = 0; i < nvdumps; i++)
        if (vdumps[i].frame == frame) {
            /* VRAM (64 KB), then CRAM (64 words) and the VDP registers */
            FILE *f = fopen(vdumps[i].path, "wb");
            if (f) {
                fwrite(vdp_vram, 1, sizeof vdp_vram, f);
                fwrite(vdp_cram, 2, 64, f);
                fwrite(vdp_reg, 1, 32, f);
                fclose(f);
            }
        }
    frame++;
}

static unsigned rnd(void);
static uint16_t choices_p2(void);
static unsigned rnd(void)
{
    random_seed = random_seed * 1103515245u + 12345u;
    return (random_seed >> 16) & 0x7FFF;
}

int plat_poll(uint16_t pad[2])
{
    int i;
    pad[0] = pad[1] = 0;
    if (random_seed && frame >= (elevator_period ? 1000u : random_from) && (!random_until || frame < random_until)) {
        if (random_left == 0) {
            static const uint16_t choices[] = {PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT, PAD_A, PAD_B, PAD_C,
                                               PAD_UP | PAD_RIGHT, PAD_DOWN | PAD_LEFT, 0};
            random_buttons = choices[rnd() % 10];
            if (rnd() % 40 == 0) random_buttons = PAD_START;
            /* in elevator mode stick to walking so no menus are left open */
            if (elevator_period && frame > 1000) random_buttons &= PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT;
            random_left = 5 + rnd() % 120;
        }
        random_left--;
        pad[0] = random_buttons;
        pad[1] = (uint16_t)(rnd() % 8 == 0 ? choices_p2() : 0);
    }
    {
        /* report level changes ($FF912C = ToeJam's current level) */
        static uint32_t last_level;
        uint32_t lv = rd8(0xFF912C);
        if (lv != last_level) {
            fprintf(stderr, "frame %u: ToeJam on level %u\n", frame, lv);
            last_level = lv;
        }
    }
    if (getenv("TJE_TRACE_ELEV")) {
        /* floor/elevator/fall state each frame, for working out arrival timing */
        fprintf(stderr, "f%5u TJlv %2u Elv %2u cur %2u/%2u ELEVFLAG %u/%u fall %u/%u half0 st %02X lv %u pl %u  half1 st %02X\n",
                frame, rd8(0xFFA2A6), rd8(0xFFA326), rd8(0xFF912C), rd8(0xFF912E), rd8(0xFFDA6A), rd8(0xFFDA6B),
                rd8(0xFFDA22), rd8(0xFFDA23), rd8(0xFFDA4A + 5), rd8(0xFFDA4A + 4), rd8(0xFFDA4A + 6), rd8(0xFFDA4A + 8 + 5));
    }
    if (elevator_period && frame > 1000 && frame % elevator_period == 0) {
        /* ELEVATOR_FLAG ($FFDA6A, from the disassembly): 1 = ToeJam is in the elevator going up */
        if (rd8(0xFF912C) && rd8(0xFFDA6A) == 0) {
            wr8(0xFFDA6A, 1);
            if (getenv("TJE_DEBUG_ELEV")) fprintf(stderr, "frame %u: elevator flag set\n", frame);
        }
    }
    if (elevator_period && frame > 1000) {
        unsigned t = frame % elevator_period;
        pad[1] = 0;
        if (t < 150) pad[0] = 0;  /* hands off while the elevator rides */
    }
    {
        /* a capture completes on a newly pressed button, like a key-down event */
        static uint16_t held_before;
        uint16_t held = 0;
        for (i = 0; i < npresses; i++)
            if (frame >= presses[i].frame && frame < presses[i].frame + presses[i].len) held |= presses[i].buttons;
        if (capturing && (held & ~held_before)) capture_ready = 1;
        held_before = held;
        held_now = held;
    }
    for (i = 0; i < npokes; i++)
        if (pokes[i].frame == frame) wr8(pokes[i].addr, pokes[i].val);
    for (i = 0; i < nsaves; i++)
        if (saves[i].frame == frame) {
            if (saves[i].load) {
                fprintf(stderr, "frame %u: loading slot %d\n", frame, saves[i].slot);
                if (save_load(saves[i].slot) != 0) fprintf(stderr, "frame %u: load failed\n", frame);
            } else {
                int rc = save_write(saves[i].slot);
                fprintf(stderr, "frame %u: save to slot %d -> %d\n", frame, saves[i].slot, rc);
            }
        }
    for (i = 0; i < npresses; i++)
        if (frame >= presses[i].frame && frame < presses[i].frame + presses[i].len) pad[presses[i].pad] |= presses[i].buttons;
    if (capturing) pad[0] = pad[1] = 0;   /* as SDL: nothing reaches the game while capturing */
    return frame < max_frames;
}

static uint16_t choices_p2(void)
{
    return (uint16_t)(1u << (rnd() % 7));
}

/* (headless: --type F:text types text at frame F) */
int plat_text_input(char *buf, int cap)
{
    int i, n = 0;
    for (i = 0; i < ntypes; i++)
        if (types[i].frame == frame && !types[i].done) {
            int k;
            for (k = 0; types[i].text[k] && n < cap; k++) buf[n++] = types[i].text[k];
            types[i].done = 1;
        }
    return n;
}

const char *plat_start_state(void) { return NULL; }
