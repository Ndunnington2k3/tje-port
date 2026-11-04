/*
 * The drawing layer (gfx/gfx.h) on the PSP's graphics engine, through sceGu.
 *
 * Two 32-bit frame buffers in video memory (512 pixels a row, 480 x 272 shown),
 * textures in main memory (the GE reads them there), quads drawn as sprites in
 * strips 64 texels wide (the texture cache's width).
 */
#include "gfx_gu.h"
#include "gfx/viewlist.h"

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <malloc.h>
#include <string.h>

#define BUF_W   512
#define SCR_W   480
#define SCR_H   272

struct GfxTex {
    int w, h;            /* size asked for */
    int pw, ph;          /* power-of-two size in memory */
    uint32_t *data;      /* 0xAABBGGRR (the GE's 8888 order) */
};

static unsigned int __attribute__((aligned(64))) list[192 * 1024];   /* (a view list: ~5000 sprites) */
static int vsync = 1;

static void gpu_idle(void);
static int pow2(int v) { int p = 8; while (p < v) p <<= 1; return p; }

static GfxTex *tex_create(int w, int h)
{
    GfxTex *t = (GfxTex *)malloc(sizeof *t);
    if (!t) return NULL;
    t->w = w;
    t->h = h;
    t->pw = pow2(w);
    t->ph = pow2(h);
    if (t->pw > 512 || t->ph > 512) { free(t); return NULL; }
    t->data = (uint32_t *)memalign(64, (size_t)t->pw * t->ph * 4);
    if (!t->data) { free(t); return NULL; }
    memset(t->data, 0, (size_t)t->pw * t->ph * 4);
    return t;
}

static void tex_update(GfxTex *t, int x, int y, int w, int h, const uint32_t *pixels, int pitch)
{
    int r;
    for (r = 0; r < h; r++) {
        const uint32_t *s = pixels + (size_t)r * pitch;
        uint32_t *d = t->data + (size_t)(y + r) * t->pw + x;
        int c;
        /* 0x00RRGGBB -> 0xFFBBGGRR */
        for (c = 0; c < w; c++) {
            uint32_t p = s[c];
            d[c] = 0xFF000000u | ((p & 0xFF) << 16) | (p & 0xFF00) | ((p >> 16) & 0xFF);
        }
    }
    sceKernelDcacheWritebackRange(t->data + (size_t)y * t->pw, (unsigned)((size_t)h * t->pw * 4));
}

static void tex_destroy(GfxTex *t)
{
    if (!t) return;
    free(t->data);
    free(t);
}

static void frame_begin(int w, int h)
{
    (void)w; (void)h;
    gpu_idle();
    sceGuStart(GU_DIRECT, list);
    sceGuScissor(0, 0, SCR_W, SCR_H);
    sceGuClearColor(0xFF000000);
    sceGuClear(GU_COLOR_BUFFER_BIT);
}

static void clip(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) sceGuScissor(0, 0, SCR_W, SCR_H);
    else sceGuScissor(x, y, x + w, y + h);
}

typedef struct { float u, v; float x, y, z; } Vtx;

static void quad(GfxTex *t, int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh, unsigned flags)
{
    int strip;
    float zx = (float)dw / (float)sw;
    if (!t || sw <= 0 || sh <= 0) return;
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexImage(0, t->pw, t->ph, t->pw, t->data);
    sceGuTexFunc(GU_TFX_REPLACE, (flags & GFX_OPAQUE) ? GU_TCC_RGB : GU_TCC_RGBA);
    sceGuTexFilter((flags & GFX_SMOOTH) ? GU_LINEAR : GU_NEAREST, (flags & GFX_SMOOTH) ? GU_LINEAR : GU_NEAREST);
    if (flags & GFX_OPAQUE) sceGuDisable(GU_ALPHA_TEST);
    else { sceGuEnable(GU_ALPHA_TEST); sceGuAlphaFunc(GU_GEQUAL, 128, 0xFF); }
    /* strips 64 texels wide (whole ones on the screen when zoomed) */
    for (strip = 0; strip < sw; strip += 64) {
        int n = sw - strip < 64 ? sw - strip : 64;
        Vtx *v = (Vtx *)sceGuGetMemory(2 * sizeof(Vtx));
        float u0 = (float)(sx + strip), u1 = (float)(sx + strip + n);
        float x0 = (float)dx + strip * zx, x1 = (float)dx + (strip + n) * zx;
        float v0 = (float)sy, v1 = (float)(sy + sh);
        if (flags & GFX_FLIP_X) {
            /* (mirrored: the strip lands on the other side) */
            x0 = (float)dx + (float)dw - (strip + n) * zx;
            x1 = (float)dx + (float)dw - strip * zx;
            { float k = u0; u0 = u1; u1 = k; }
        }
        if (flags & GFX_FLIP_Y) { float k = v0; v0 = v1; v1 = k; }
        v[0].u = u0; v[0].v = v0; v[0].x = x0; v[0].y = (float)dy; v[0].z = 0;
        v[1].u = u1; v[1].v = v1; v[1].x = x1; v[1].y = (float)(dy + dh); v[1].z = 0;
        sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, 0, v);
    }
}

/* The GE draws a frame while the CPU runs the next one: frame_end only starts
 * it; the next frame waits for it (gpu_idle) before changing anything it reads
 * (the list, textures, pieces) and shows it (gfx_gu_show). */
static int gpu_busy, frame_waiting;

static void gpu_idle(void)
{
    if (!gpu_busy) return;
    sceGuSync(0, 0);
    gpu_busy = 0;
}

static void frame_end(void)
{
    sceGuFinish();
    gpu_busy = 1;
    frame_waiting = 1;
}

static void *draw_buf = (void *)0;   /* (offset in VRAM of the buffer being drawn) */

void gfx_gu_show(void)
{
    gpu_idle();
    if (!frame_waiting) return;
    if (vsync) sceDisplayWaitVblankStart();
    draw_buf = sceGuSwapBuffers();
    frame_waiting = 0;
}

const uint32_t *gfx_gu_drawn(void)
{
    /* (the buffer just drawn, before gfx_gu_show: uncached VRAM) */
    gpu_idle();
    return (const uint32_t *)(0x44000000u + (uint32_t)draw_buf);
}

/* ---- gameplay views from lists (gfx/viewlist.h) --------------------------------
 * The Genesis' 2048 tiles as one 512 x 256 texture of 4-bit colour numbers
 * (64 tiles a row; the GE's 4-bit order is the low nibble first, the Genesis' the
 * high: swapped on the way in), brought up to date tile by tile as the game
 * changes them. A cell is a 2-vertex sprite from it, its palette line the
 * colour table's offset (16 entries each, entry 0 see-through). Objects' pieces
 * are 8-bit textures whose bytes (colour, priority, opaque) go through a table
 * per pass: a priority pass shows only bytes with bits 6 and 7 set, the other
 * only bit 7. */

static uint8_t __attribute__((aligned(64))) atlas[512 * 256 / 2];
static uint32_t __attribute__((aligned(64))) clut_tiles[64];
static uint32_t __attribute__((aligned(64))) clut_pieces[2][256];
static uint32_t __attribute__((aligned(64))) ovl_px[2 * 320 * 232];   /* the overlays, one after another */

typedef struct { unsigned short u, v; short x, y, z; } V16;

static uint32_t abgr(uint32_t c) { return 0xFF000000u | ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF); }

static void atlas_update(const ViewList *vl)
{
    int t, changed = 0;
    if (vl->frozen) return;
    for (t = 0; t < 2048; t++) {
        const uint8_t *src;
        uint8_t *dst;
        int r;
        if (!vl->tiles_changed[t]) continue;
        vl->tiles_changed[t] = 0;
        src = vl->vram + t * 32;
        dst = atlas + (t >> 6) * 8 * 256 + (t & 63) * 4;
        for (r = 0; r < 8; r++) {
            int i;
            for (i = 0; i < 4; i++) {
                uint8_t b = src[r * 4 + i];
                dst[r * 256 + i] = (uint8_t)((b >> 4) | (b << 4));
            }
        }
        changed = 1;
    }
    if (changed) {
        sceKernelDcacheWritebackRange(atlas, sizeof atlas);
        sceGuTexFlush();
    }
}

static void tiles_pass(const ViewList *vl, int layer, int high, int x, int y)
{
    int k;
    sceGuTexMode(GU_PSM_T4, 0, 0, 0);
    sceGuTexImage(0, 512, 256, 512, atlas);
    sceGuClutLoad(64 / 8, clut_tiles);
    for (k = 0; k < vl->nbatches; k++) {
        const VlBatch *b = &vl->batches[k];
        int count[4] = {0, 0, 0, 0}, c, line;
        if (b->layer != layer || !b->count) continue;
        for (c = b->first; c < b->first + b->count; c++) {
            unsigned e = vl->cells[c].entry;
            if (((e >> 15) & 1) == (unsigned)high) count[(e >> 13) & 3]++;
        }
        sceGuScissor(x + b->x0, y + b->y0, x + b->x1, y + b->y1);
        for (line = 0; line < 4; line++) {
            V16 *v, *o;
            if (!count[line]) continue;
            v = o = (V16 *)sceGuGetMemory((unsigned)count[line] * 2 * sizeof(V16));
            for (c = b->first; c < b->first + b->count; c++) {
                const VlCell *cl = &vl->cells[c];
                unsigned e = cl->entry, t = e & 0x7FF;
                unsigned short u0 = (unsigned short)((t & 63) * 8), v0 = (unsigned short)((t >> 6) * 8);
                if (((e >> 15) & 1) != (unsigned)high || ((e >> 13) & 3) != (unsigned)line) continue;
                o[0].x = (short)(x + cl->x); o[0].y = (short)(y + cl->y); o[0].z = 0;
                o[1].x = (short)(x + cl->x + 8); o[1].y = (short)(y + cl->y + 8); o[1].z = 0;
                o[0].u = (e & 0x0800) ? u0 + 8 : u0; o[1].u = (e & 0x0800) ? u0 : u0 + 8;
                o[0].v = (e & 0x1000) ? v0 + 8 : v0; o[1].v = (e & 0x1000) ? v0 : v0 + 8;
                o += 2;
            }
            sceGuClutMode(GU_PSM_8888, 0, 0x0F, (unsigned)line);
            sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, count[line] * 2, 0, v);
        }
    }
}

static void pieces_pass(const ViewList *vl, int high, int x, int y)
{
    int k;
    sceGuClutLoad(256 / 8, clut_pieces[high]);
    sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
    for (k = 0; k < vl->npieces; k++) {
        const VlPiece *p = &vl->pieces[k];
        V16 *v = (V16 *)sceGuGetMemory(2 * sizeof(V16));
        int y0 = p->y0 > 0 ? p->y0 : 0, y1 = p->y1 < vl->h ? p->y1 : vl->h;
        if (y0 >= y1) continue;
        sceGuScissor(x, y + y0, x + vl->w, y + y1);
        sceGuTexMode(GU_PSM_T8, 0, 0, 0);
        sceGuTexImage(0, pow2(p->w), pow2(p->h), p->stride, p->px);
        sceGuTexFlush();
        v[0].u = 0; v[0].v = 0; v[0].x = (short)(x + p->x); v[0].y = (short)(y + p->y); v[0].z = 0;
        v[1].u = (unsigned short)p->w; v[1].v = (unsigned short)p->h;
        v[1].x = (short)(x + p->x + p->w); v[1].y = (short)(y + p->y + p->h); v[1].z = 0;
        sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, v);
    }
}

static void view_list(const ViewList *vl, int x, int y)
{
    int i, high, k;
    atlas_update(vl);
    for (i = 0; i < 64; i++) clut_tiles[i] = (i & 15) ? abgr(vl->pal[i]) : 0;
    for (i = 0; i < 256; i++) {
        clut_pieces[0][i] = (i & 0xC0) == 0x80 ? abgr(vl->pal[i & 0x3F]) : 0;
        clut_pieces[1][i] = (i & 0xC0) == 0xC0 ? abgr(vl->pal[i & 0x3F]) : 0;
    }
    sceKernelDcacheWritebackRange(clut_tiles, sizeof clut_tiles);
    sceKernelDcacheWritebackRange(clut_pieces, sizeof clut_pieces);
    if (vl->npieces) {
        const VlPiece *last = &vl->pieces[vl->npieces - 1];
        sceKernelDcacheWritebackRange((void *)vl->pieces[0].px,
                                      (unsigned)(last->px + last->stride * last->h - vl->pieces[0].px));
    }
    /* the backdrop */
    sceGuScissor(x, y, x + vl->w, y + vl->h);
    sceGuClearColor(abgr(vl->backdrop));
    sceGuClear(GU_COLOR_BUFFER_BIT);
    sceGuEnable(GU_ALPHA_TEST);
    sceGuAlphaFunc(GU_GREATER, 0, 0xFF);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    for (high = 0; high < 2; high++) {
        tiles_pass(vl, 0, high, x, y);
        tiles_pass(vl, 1, high, x, y);
        pieces_pass(vl, high, x, y);
    }
    sceGuDisable(GU_ALPHA_TEST);
    sceGuScissor(0, 0, SCR_W, SCR_H);
    /* the finished pictures over it (status bar, present list, menus, boxes),
     * copied one after another into one area the GE reads */
    {
        size_t used = 0;
        for (k = 0; k < vl->noverlays; k++) {
            const VlOverlay *o = &vl->overlays[k];
            int r, c, w = o->w > 512 ? 512 : o->w, h = o->h > 512 ? 512 : o->h, tbw = (w + 3) & ~3;
            uint32_t *d = ovl_px + used;
            V16 *v;
            if (used + (size_t)tbw * (size_t)h > sizeof ovl_px / 4) break;
            used += ((size_t)tbw * (size_t)h + 15) & ~(size_t)15;
            for (r = 0; r < h; r++) {
                const uint32_t *src = o->px + (size_t)r * o->pitch;
                uint32_t *dst = d + r * tbw;
                for (c = 0; c < w; c++) dst[c] = abgr(src[c]);
            }
            sceKernelDcacheWritebackRange(d, (unsigned)(tbw * h * 4));
            sceGuTexMode(GU_PSM_8888, 0, 0, 0);
            sceGuTexImage(0, pow2(w), pow2(h), tbw, d);
            sceGuTexFlush();
            sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
            v = (V16 *)sceGuGetMemory(2 * sizeof(V16));
            v[0].u = 0; v[0].v = 0; v[0].x = (short)(x + o->x); v[0].y = (short)(y + o->y); v[0].z = 0;
            v[1].u = (unsigned short)w; v[1].v = (unsigned short)h;
            v[1].x = (short)(x + o->x + w); v[1].y = (short)(y + o->y + h); v[1].z = 0;
            sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, v);
        }
    }
}

static const GfxBackend backend = {
    "sceGu", tex_create, tex_update, tex_destroy, frame_begin, clip, quad, frame_end, view_list, gpu_idle
};

const GfxBackend *gfx_gu_init(void)
{
    /* frame buffers: two 512 x 272 x 32-bit ones at the start of video memory */
    void *fb0 = (void *)0;
    void *fb1 = (void *)(BUF_W * SCR_H * 4);
    sceGuInit();
    sceGuStart(GU_DIRECT, list);
    sceGuDrawBuffer(GU_PSM_8888, fb0, BUF_W);
    sceGuDispBuffer(SCR_W, SCR_H, fb1, BUF_W);
    sceGuOffset(2048 - (SCR_W / 2), 2048 - (SCR_H / 2));
    sceGuViewport(2048, 2048, SCR_W, SCR_H);
    sceGuScissor(0, 0, SCR_W, SCR_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_BLEND);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuShadeModel(GU_FLAT);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
    return &backend;
}

void gfx_gu_vsync(int on) { vsync = on; }
