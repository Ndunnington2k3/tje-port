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
 * The log records the captures saved.
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

extern int g_gv_cam_x, g_gv_cam_y;
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
static int ring_pos, ring_count, captures;
static unsigned frame_no;


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
    k->mask = 0;
    for (y = 0; y < height; y++)
        for (x = 0; x < width; x++) {
            uint32_t c = pixels[(size_t)y * pitch + x];
            k->px[(size_t)y * width + x] = (uint16_t)(((c >> 9) & 0x7C00) | ((c >> 6) & 0x03E0) | ((c >> 3) & 0x001F));
        }
    memcpy(k->ram, g_ram, sizeof k->ram);
    memcpy(k->vram, vdp_vram, sizeof k->vram);
    ring_pos = (ring_pos + 1) % VIEWDIAG_KEEP;
    if (ring_count < VIEWDIAG_KEEP) ring_count++;
    if (getenv("TJE_VIEWDIAG_AT") && (unsigned)atoi(getenv("TJE_VIEWDIAG_AT")) == frame_no) viewdiag_capture("asked");
}
