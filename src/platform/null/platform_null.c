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
 *     --dumpram F:path     save the 64 KB of work RAM at frame F (repeatable)
 *     --dumpvram F:path    save VRAM, CRAM and the VDP registers at frame F
 *     --wav path.wav       emulate sound and record it to a WAV file
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
#include "game/viewdiag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_EVENTS 1024

static const char *rom_path = "tje.bin";
static unsigned max_frames = 600;
static unsigned frame;
static struct { unsigned frame; char path[256]; } shots[MAX_EVENTS];
static int nshots;
static struct { unsigned frame; char path[256]; } dumps[MAX_EVENTS];
static int ndumps;
static struct { unsigned frame; char path[256]; } vdumps[MAX_EVENTS];
static int nvdumps;
static struct { unsigned frame, len; uint16_t buttons; } presses[MAX_EVENTS];
static int npresses;
static unsigned random_seed;
static uint16_t random_buttons;
static unsigned random_left;
static unsigned elevator_period;
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
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc && nshots < MAX_EVENTS) {
            const char *a = argv[++i];
            const char *c = strchr(a, ':');
            if (!c) continue;
            shots[nshots].frame = (unsigned)atoi(a);
            snprintf(shots[nshots].path, sizeof shots[nshots].path, "%s", c + 1);
            nshots++;
        } else if (!strcmp(argv[i], "--press") && i + 1 < argc && npresses < MAX_EVENTS) {
            unsigned f = 0, n = 0;
            char btn[32] = {0};
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
        } else if (!strcmp(argv[i], "--wav") && i + 1 < argc) {
            wav = fopen(argv[++i], "wb");
            if (wav) { static const uint8_t hdr[44] = {0}; fwrite(hdr, 1, 44, wav); }
        } else if (!strcmp(argv[i], "--elevator") && i + 1 < argc) {
            elevator_period = (unsigned)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--gvcheck") && i + 1 < argc) {
            gvcheck = argv[++i];   /* (the gameplay view is on by default: options.c) */
            {
                extern int g_gameview_test_pieces;
                g_gameview_test_pieces = getenv("TJE_GV_PIECES") != NULL;
            }
        } else if (!strcmp(argv[i], "--window") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &window_w, &window_h) == 2 && window_w > 0 && window_h > 0)
                present_init(gfx_soft_init());
            else
                window_w = window_h = 0;
        } else if (!strcmp(argv[i], "--view-log") && i + 1 < argc) {
            char cap[600];
            snprintf(cap, sizeof cap, "%s_capture", argv[i + 1]);
            viewdiag_open(argv[++i], cap);
        } else if (!strcmp(argv[i], "--random") && i + 1 < argc) {
            random_seed = (unsigned)atoi(argv[++i]) * 2654435761u + 1;
        } else if (argv[i][0] != '-') rom_path = argv[i];
        else if (i + 1 < argc && argv[i + 1][0] != '-') i++;  /* skip value of unknown/overflowing option */
    }
    return 0;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

void plat_shutdown(void)
{
    if (getenv("TJE_GV_FLICKER")) {
        extern int g_gv_max_pieces;
        fprintf(stderr, "max pieces %d\n", g_gv_max_pieces);
    }
    if (gvcheck) {
        int r;
        fprintf(stderr, "gvcheck: %u gameplay frames drawn, %u differ (%lu pixels)\n",
                gv_frames, gv_bad_frames, gv_bad_pixels);
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

int plat_audio_enabled(void) { return wav != NULL || audio_flag; }

void plat_audio(const int16_t *samples, int frames)
{
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

void plat_present(const uint32_t *pixels, int width, int height, int pitch)
{
    int i;
    viewdiag_frame(pixels, width, height, pitch);
    if (getenv("TJE_GV_REASONS")) {
        const uint32_t *g_; int a_, b_, c_;
        if (!gameview_frame(&g_, &a_, &b_, &c_)) fprintf(stderr, "frame %u: not drawn, reason %d\n", frame, g_gameview_reason);
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
    if (getenv("TJE_GV_SYNC")) {
        extern FILE *g_gv_sync_log;
        if (!g_gv_sync_log) g_gv_sync_log = fopen(getenv("TJE_GV_SYNC"), "w");
        fprintf(g_gv_sync_log, "-- frame %u cam %d,%d new %d,%d\n", frame, (int16_t)rd16(0xFF8002u), (int16_t)rd16(0xFF8004u), (int16_t)rd16(0xFF8036u), (int16_t)rd16(0xFF8038u));
    }
    if (getenv("TJE_GV_BLINK")) {
        /* pixels of the wider picture that change for exactly one frame (A == C != B),
         * compared at the same world position (the level does not move with the camera) */
        static uint32_t *pic[3];
        static uint8_t *lay[3];
        extern uint8_t *g_gv_layer;
        static int pw[3], ph[3], cx[3], cy[3], ok[3];
        static FILE *bl;
        const uint32_t *gv;
        int gw, gh, gp, k;
        if (!bl) bl = fopen(getenv("TJE_GV_BLINK"), "w");
        for (k = 0; k < 3 && !pic[k]; k++) { pic[k] = malloc(sizeof(uint32_t) * 1024 * 1024); lay[k] = calloc(1024 * 1024, 1); }
        if (!g_gv_layer) g_gv_layer = calloc((size_t)GAMEVIEW_MAX_W * GAMEVIEW_MAX_H, 1);
        { uint32_t *t = pic[2]; pic[2] = pic[1]; pic[1] = pic[0]; pic[0] = t; }
        { uint8_t *t = lay[2]; lay[2] = lay[1]; lay[1] = lay[0]; lay[0] = t; }
        for (k = 2; k > 0; k--) { pw[k] = pw[k-1]; ph[k] = ph[k-1]; cx[k] = cx[k-1]; cy[k] = cy[k-1]; ok[k] = ok[k-1]; }
        ok[0] = gameview_frame(&gv, &gw, &gh, &gp) && (gw != 320 || gh != VDP_HEIGHT) && !rd8(0xFF9364u) ? 1 : 0;
        if (ok[0]) {
            int y;
            pw[0] = gw; ph[0] = gh;
            extern int g_gv_cam_x, g_gv_cam_y;
            cx[0] = g_gv_cam_x; cy[0] = g_gv_cam_y;
            for (y = 0; y < gh; y++) {
                memcpy(pic[0] + y * gw, gv + y * gp, sizeof(uint32_t) * (size_t)gw);
                memcpy(lay[0] + y * gw, g_gv_layer + (size_t)y * GAMEVIEW_MAX_W, (size_t)gw);
            }
        }
        if (ok[0] && ok[1] && ok[2] && pw[0] == pw[1] && pw[1] == pw[2] && ph[0] == ph[1] && ph[1] == ph[2]) {
            int w = pw[1], h = ph[1], ox = (w - 320) / 2, oy = (h - VDP_HEIGHT) / 2, x, y;
            int nin = 0, nout = 0, bx0 = 9999, by0 = 9999, bx1 = -1, by1 = -1;
            for (y = 0; y < h; y++)
                for (x = 0; x < w; x++) {
                    int xa = x + cx[1] - cx[2], ya = y + cy[1] - cy[2];
                    int xc = x + cx[1] - cx[0], yc = y + cy[1] - cy[0];
                    uint32_t a, b, c;
                    if (xa < 0 || ya < 0 || xa >= w || ya >= h || xc < 0 || yc < 0 || xc >= w || yc >= h) continue;
                    if (y >= oy + 184) continue;     /* (status bar) */
                    a = pic[2][ya * w + xa] & 0xFFFFFF; b = pic[1][y * w + x] & 0xFFFFFF; c = pic[0][yc * w + xc] & 0xFFFFFF;
                    if (a != c || a == b) continue;
                    if (!lay[1][y * w + x] && !lay[2][ya * w + xa]) continue;   /* (not a sprite: stars) */
                    if (x >= ox && x < ox + 320 && y >= oy && y < oy + VDP_HEIGHT) nin++;
                    else {
                        nout++;
                        if (x < bx0) bx0 = x; if (x > bx1) bx1 = x;
                        if (y < by0) by0 = y; if (y > by1) by1 = y;
                    }
                }
            if ((nout >= 16 || nin >= 16) && getenv("TJE_GV_BLINKSAVE") && nout >= 16) {
                static int saved;
                if (saved++ < 30) {
                    char path[512];
                    snprintf(path, sizeof path, "%s_%u_a.ppm", getenv("TJE_GV_BLINKSAVE"), frame - 1);
                    save_ppm(path, pic[2], w, h, w);
                    snprintf(path, sizeof path, "%s_%u_b.ppm", getenv("TJE_GV_BLINKSAVE"), frame - 1);
                    save_ppm(path, pic[1], w, h, w);
                    snprintf(path, sizeof path, "%s_%u_c.ppm", getenv("TJE_GV_BLINKSAVE"), frame - 1);
                    save_ppm(path, pic[0], w, h, w);
                }
            }
            if (nout >= 16 || nin >= 16)
                fprintf(bl, "frame %u: blink outside %d (%d,%d)-(%d,%d) inside %d cam %d,%d\n", frame - 1, nout,
                        bx0 - ox, by0 - oy, bx1 - ox, by1 - oy, nin, cx[1], cy[1]);
        }
    }
    if (getenv("TJE_GV_FLICKER")) {
        /* objects drawn from frame data, and objects drawn at all, frame by frame:
         * report any that vanish for 1-2 frames and come back */
        extern unsigned long long g_gv_drawn_mask;
        extern uint8_t g_gv_item_drawn[98];
        extern int g_gv_item_x[98], g_gv_item_y[98];
        static uint8_t ih[3][98];
        static unsigned long long hist[4];
        static FILE *fl;
        int e;
        if (!fl) fl = fopen(getenv("TJE_GV_FLICKER"), "w");
        hist[3] = hist[2]; hist[2] = hist[1]; hist[1] = hist[0]; hist[0] = g_gv_drawn_mask;
        memcpy(ih[2], ih[1], 98); memcpy(ih[1], ih[0], 98); memcpy(ih[0], g_gv_item_drawn, 98);
        for (e = 0; e < 98; e++)
            if (ih[0][e] && !ih[1][e] && ih[2][e])
                fprintf(fl, "frame %u: item %d missing for 1 frame (x %d y %d, before %d after %d)\n", frame - 1, e,
                        g_gv_item_x[e], g_gv_item_y[e], ih[2][e], ih[0][e]);
        for (e = 0; e < 40; e++) {
            unsigned long long b = 1ull << e;
            if ((hist[0] & b) && !(hist[1] & b) && (hist[2] & b))
                fprintf(fl, "frame %u: object %d missing for 1 frame (x %d y %d 2F %02X 4A %d 22 %06X)\n", frame - 1, e,
                        (int16_t)rd16(0xFFA25Au + (uint32_t)e * 0x80) - (int16_t)rd16(0xFF8002u),
                        (int16_t)rd16(0xFFA25Cu + (uint32_t)e * 0x80) - (int16_t)rd16(0xFF8004u),
                        rd8(0xFFA289u + (uint32_t)e * 0x80), rd8(0xFFA2A4u + (uint32_t)e * 0x80),
                        rd32(0xFFA27Cu + (uint32_t)e * 0x80));
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
    } else {
        for (i = 0; i < nshots; i++)
            if (shots[i].frame == frame) save_ppm(shots[i].path, pixels, width, height, pitch);
    }
    for (i = 0; i < ndumps; i++)
        if (dumps[i].frame == frame) {
            FILE *f = fopen(dumps[i].path, "wb");
            if (f) { fwrite(g_ram, 1, sizeof g_ram, f); fclose(f); }
        }
    if (getenv("TJE_TRACE_VDP")) {
        uint32_t st[5];
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
    if (random_seed && frame >= (elevator_period ? 1000u : 500u)) {
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
        if (frame >= presses[i].frame && frame < presses[i].frame + presses[i].len) pad[0] |= presses[i].buttons;
    if (capturing) pad[0] = pad[1] = 0;   /* as SDL: nothing reaches the game while capturing */
    return frame < max_frames;
}

static uint16_t choices_p2(void)
{
    return (uint16_t)(1u << (rnd() % 7));
}
