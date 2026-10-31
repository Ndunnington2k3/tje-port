/*
 * Diagnostics for the wider gameplay view. See viewdiag.h.
 *
 * Kept per frame (a ring of VIEWDIAG_KEEP): the picture shown (15-bit colour,
 * which holds the Genesis' 9-bit colours exactly), work RAM, VRAM, and a few of
 * the view's own facts. A capture file is:
 *   "TJEVCAP1", u32 count, then per frame (oldest first):
 *   u32 frame number, u16 width, u16 height, i16 cam x, i16 cam y, u8 gameview
 *   picture?, u8 snapshot age, u16 0, u64 objects-drawn mask, then width*height
 *   u16 pixels (0RRRRRGGGGGBBBBB), 64K RAM, 64K VRAM.
 * Findings (one-frame drop-outs of objects, items or sprite pixels) go to the
 * log, and the first few also save a capture by themselves.
 */
#include "viewdiag.h"
#include "gameview.h"
#include "core/genesis.h"
#include "core/m68k_rt.h"
#include "core/vdp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VIEWDIAG_KEEP 120
#define AUTO_CAPTURES 3

extern unsigned long long g_gv_drawn_mask;
extern uint8_t g_gv_item_drawn[98];
extern int g_gv_item_x[98], g_gv_item_y[98];
extern int g_gv_cam_x, g_gv_cam_y;
extern uint8_t *g_gv_layer;
extern int g_gv_snap_age, g_gv_gameplay;

typedef struct {
    unsigned frame;
    int w, h, cam_x, cam_y, is_view, snap_age;
    unsigned long long mask;
    uint16_t *px;                 /* GAMEVIEW_MAX_W * GAMEVIEW_MAX_H */
    uint8_t ram[0x10000], vram[0x10000];
} Kept;

static FILE *log_file;
static char prefix[1024];
static Kept *ring;
static int ring_pos, ring_count, captures, auto_captures;
static unsigned frame_no;

/* for the drop-out finder: the last three gameview pictures */
static uint32_t *pic[3];
static uint8_t *lay[3];
static int pw[3], ph[3], cx[3], cy[3], ok[3];
static unsigned long long hist[4];
static uint8_t ih[4][98];

int viewdiag_active(void) { return log_file != NULL; }

void viewdiag_open(const char *log_path, const char *capture_prefix)
{
    int k;
    log_file = fopen(log_path, "w");
    if (!log_file) return;
    snprintf(prefix, sizeof prefix, "%s", capture_prefix);
    ring = (Kept *)calloc(VIEWDIAG_KEEP, sizeof *ring);
    for (k = 0; ring && k < VIEWDIAG_KEEP; k++)
        ring[k].px = (uint16_t *)malloc(sizeof(uint16_t) * GAMEVIEW_MAX_W * GAMEVIEW_MAX_H);
    for (k = 0; k < 3; k++) {
        pic[k] = (uint32_t *)malloc(sizeof(uint32_t) * GAMEVIEW_MAX_W * GAMEVIEW_MAX_H);
        lay[k] = (uint8_t *)calloc((size_t)GAMEVIEW_MAX_W * GAMEVIEW_MAX_H, 1);
    }
    if (!g_gv_layer) g_gv_layer = (uint8_t *)calloc((size_t)GAMEVIEW_MAX_W * GAMEVIEW_MAX_H, 1);
    fprintf(log_file, "ToeJam & Earl view log. F12 saves the last %d frames.\n", VIEWDIAG_KEEP);
    fflush(log_file);
}

static void put16(FILE *f, unsigned v) { fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f); }
static void put32(FILE *f, unsigned v) { put16(f, v & 0xFFFF); put16(f, v >> 16); }

void viewdiag_capture(const char *why)
{
    char path[1100];
    FILE *f;
    int i;
    if (!log_file || !ring || !ring_count) return;
    snprintf(path, sizeof path, "%s%d.bin", prefix, ++captures);
    f = fopen(path, "wb");
    if (!f) return;
    fwrite("TJEVCAP1", 1, 8, f);
    put32(f, (unsigned)ring_count);
    for (i = 0; i < ring_count; i++) {
        const Kept *k = &ring[(ring_pos - ring_count + i + VIEWDIAG_KEEP) % VIEWDIAG_KEEP];
        int y;
        put32(f, k->frame);
        put16(f, (unsigned)k->w);
        put16(f, (unsigned)k->h);
        put16(f, (unsigned)(k->cam_x & 0xFFFF));
        put16(f, (unsigned)(k->cam_y & 0xFFFF));
        fputc(k->is_view, f);
        fputc(k->snap_age > 255 ? 255 : k->snap_age, f);
        put16(f, 0);
        put32(f, (unsigned)(k->mask & 0xFFFFFFFFu));
        put32(f, (unsigned)(k->mask >> 32));
        for (y = 0; y < k->h; y++) fwrite(k->px + (size_t)y * k->w, 2, (size_t)k->w, f);
        fwrite(k->ram, 1, sizeof k->ram, f);
        fwrite(k->vram, 1, sizeof k->vram, f);
    }
    fclose(f);
    fprintf(log_file, "frame %u: saved %s (%s)\n", frame_no, path, why);
    fflush(log_file);
}

static void find_dropouts(void)
{
    const uint32_t *gv;
    int gw, gh, gp, k, e;
    char why[160];
    int found = 0;
    { uint32_t *t = pic[2]; pic[2] = pic[1]; pic[1] = pic[0]; pic[0] = t; }
    { uint8_t *t = lay[2]; lay[2] = lay[1]; lay[1] = lay[0]; lay[0] = t; }
    for (k = 2; k > 0; k--) { pw[k] = pw[k-1]; ph[k] = ph[k-1]; cx[k] = cx[k-1]; cy[k] = cy[k-1]; ok[k] = ok[k-1]; }
    ok[0] = gameview_frame(&gv, &gw, &gh, &gp) && (gw != 320 || gh != VDP_HEIGHT) && g_gv_gameplay ? 1 : 0;
    if (ok[0]) {
        int y;
        pw[0] = gw; ph[0] = gh;
        cx[0] = g_gv_cam_x; cy[0] = g_gv_cam_y;
        for (y = 0; y < gh; y++) {
            memcpy(pic[0] + (size_t)y * gw, gv + (size_t)y * gp, sizeof(uint32_t) * (size_t)gw);
            memcpy(lay[0] + (size_t)y * gw, g_gv_layer + (size_t)y * GAMEVIEW_MAX_W, (size_t)gw);
        }
    }
    /* sprite pixels that change for exactly one frame (A == C != B), compared at
     * the same place in the level */
    if (ok[0] && ok[1] && ok[2] && pw[0] == pw[1] && pw[1] == pw[2] && ph[0] == ph[1] && ph[1] == ph[2]) {
        int w = pw[1], h = ph[1], ox = (w - 320) / 2, oy = (h - VDP_HEIGHT) / 2, x, y;
        int nin = 0, nout = 0, bx0 = 9999, by0 = 9999, bx1 = -1, by1 = -1;
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                int xa = x + cx[1] - cx[2], ya = y + cy[1] - cy[2];
                int xc = x + cx[1] - cx[0], yc = y + cy[1] - cy[0];
                uint32_t a, b, c;
                if (xa < 0 || ya < 0 || xa >= w || ya >= h || xc < 0 || yc < 0 || xc >= w || yc >= h) continue;
                if (y >= oy + 184) continue;                            /* (status bar) */
                a = pic[2][ya * w + xa] & 0xFFFFFF;
                b = pic[1][y * w + x] & 0xFFFFFF;
                c = pic[0][yc * w + xc] & 0xFFFFFF;
                if (a != c || a == b) continue;
                if (!lay[1][y * w + x] && !lay[2][ya * w + xa]) continue;   /* (not a sprite) */
                if (x >= ox && x < ox + 320 && y >= oy && y < oy + VDP_HEIGHT) nin++;
                else {
                    nout++;
                    if (x < bx0) bx0 = x;
                    if (x > bx1) bx1 = x;
                    if (y < by0) by0 = y;
                    if (y > by1) by1 = y;
                }
            }
        if (nout >= 16) {
            fprintf(log_file, "frame %u: %d sprite pixels outside 4:3 changed for one frame, (%d,%d)-(%d,%d) "
                    "from the original screen's corner (and %d inside)\n", frame_no - 1, nout,
                    bx0 - ox, by0 - oy, bx1 - ox, by1 - oy, nin);
            found = 1;
        }
    }
    /* objects and items drawn, then not for one or two frames (one step of the
     * game's 30 frames a second), then drawn again */
    memmove(&hist[1], &hist[0], sizeof hist[0] * 3);
    hist[0] = g_gv_drawn_mask;
    memmove(ih[1], ih[0], sizeof ih[0] * 3);
    memcpy(ih[0], g_gv_item_drawn, 98);
    for (e = 0; e < 98; e++) {
        int gap = !ih[0][e] ? 0 : (!ih[1][e] && ih[2][e]) ? 1 : (!ih[1][e] && !ih[2][e] && ih[3][e]) ? 2 : 0;
        if (gap) {
            fprintf(log_file, "frame %u: floor item %d missing for %d frame(s) (x %d y %d)\n", frame_no - 1, e, gap,
                    g_gv_item_x[e], g_gv_item_y[e]);
            found = 1;
        }
    }
    for (e = 0; e < 40; e++) {
        unsigned long long b = 1ull << e;
        int gap = !(hist[0] & b) ? 0 : (!(hist[1] & b) && (hist[2] & b)) ? 1 :
                  (!(hist[1] & b) && !(hist[2] & b) && (hist[3] & b)) ? 2 : 0;
        if (gap) {
            uint32_t o = 0xFFA25Au + (uint32_t)e * 0x80;
            fprintf(log_file, "frame %u: object %d missing for %d frame(s) (x %d y %d, flags %02X, sprites %d, frame %06X)\n",
                    frame_no - 1, e, gap, (int16_t)rd16(o) - g_gv_cam_x, (int16_t)rd16(o + 2) - g_gv_cam_y,
                    rd8(o + 0x2F), rd8(o + 0x4A), rd32(o + 0x22));
            found = 1;
        }
    }
    if (found) {
        fflush(log_file);
        if (auto_captures < AUTO_CAPTURES) {
            snprintf(why, sizeof why, "found by itself");
            auto_captures++;
            viewdiag_capture(why);
        }
    }
}

void viewdiag_frame(const uint32_t *pixels, int width, int height, int pitch)
{
    Kept *k;
    const uint32_t *gv;
    int gw, gh, gp, x, y;
    if (!log_file || !ring) return;
    frame_no++;
    if (width > GAMEVIEW_MAX_W) width = GAMEVIEW_MAX_W;
    if (height > GAMEVIEW_MAX_H) height = GAMEVIEW_MAX_H;
    k = &ring[ring_pos];
    k->frame = frame_no;
    k->w = width;
    k->h = height;
    k->cam_x = g_gv_cam_x;
    k->cam_y = g_gv_cam_y;
    k->is_view = gameview_frame(&gv, &gw, &gh, &gp) && gv == pixels;
    k->snap_age = g_gv_snap_age;
    k->mask = g_gv_drawn_mask;
    for (y = 0; y < height; y++)
        for (x = 0; x < width; x++) {
            uint32_t c = pixels[(size_t)y * pitch + x];
            k->px[(size_t)y * width + x] = (uint16_t)(((c >> 9) & 0x7C00) | ((c >> 6) & 0x03E0) | ((c >> 3) & 0x001F));
        }
    memcpy(k->ram, g_ram, sizeof k->ram);
    memcpy(k->vram, vdp_vram, sizeof k->vram);
    ring_pos = (ring_pos + 1) % VIEWDIAG_KEEP;
    if (ring_count < VIEWDIAG_KEEP) ring_count++;
    find_dropouts();
    if (getenv("TJE_VIEWDIAG_AT") && (unsigned)atoi(getenv("TJE_VIEWDIAG_AT")) == frame_no) viewdiag_capture("asked");
}
