/*
 * The drawing layer (gfx/gfx.h) on the PSP's graphics engine, through sceGu.
 *
 * Two 32-bit frame buffers in video memory (512 pixels a row, 480 x 272 shown),
 * textures in main memory (the GE reads them there), quads drawn as sprites in
 * strips 64 texels wide (the texture cache's width).
 */
#include "gfx_gu.h"
#include "gfx/viewlist.h"
#include "core/prof.h"

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspge.h>
#include <malloc.h>
#include <stdio.h>
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
    if (!t->data) { printf("gfx: no memory for a %dx%d texture\n", t->pw, t->ph); free(t); return NULL; }
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

/* Three frame buffers in video memory: one shown, one waiting to be (paced by the
 * clock, network play: shown at the next vertical blank - never part way down
 * the screen), one drawn. */
#define NBUF 3
static const unsigned buf_at[NBUF] = {0, BUF_W * SCR_H * 4, 2 * BUF_W * SCR_H * 4};
static int draw_i;                          /* the buffer being drawn */
static int disp_i = 1;                      /* the one on the screen */
static int pend_i = -1, pend_vc;            /* the one given to be shown at the next blank, and when */

static void displayed_now(void)
{
    /* (a buffer given at Vcount v is on the screen from the blank making it
     * v + 1; another given before then took its place) */
    if (pend_i >= 0 && (int)sceDisplayGetVcount() != pend_vc) { disp_i = pend_i; pend_i = -1; }
}

static void pick_draw(void)
{
    int i;
    displayed_now();
    for (i = 0; i < NBUF; i++)
        if (i != disp_i && i != pend_i) { draw_i = i; return; }
}

static void frame_begin(int w, int h)
{
    (void)w; (void)h;
    gpu_idle();
    pick_draw();
    sceGuStart(GU_DIRECT, list);
    sceGuDrawBufferList(GU_PSM_8888, (void *)buf_at[draw_i], BUF_W);
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
    sceGuTexFlush();   /* (the same address every frame, new pixels: not the GE's cached ones) */
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

void gfx_gu_show(void)
{
    gpu_idle();
    if (!frame_waiting) return;
    displayed_now();
    {
        /* (always given to be taken at a blank, never part way down the screen: a
         * swap made "immediately" after waiting for the blank tears the top lines
         * whenever the thread is held up past the blank's end - the audio thread) */
        int vc = (int)sceDisplayGetVcount();
        sceDisplaySetFrameBuf((uint8_t *)sceGeEdramGetAddr() + buf_at[draw_i], BUF_W, PSP_DISPLAY_PIXEL_FORMAT_8888,
                              PSP_DISPLAY_SETBUF_NEXTFRAME);
        if (vsync && (int)sceDisplayGetVcount() == vc) {
            /* (paced by the screen: on it from the blank this waits for) */
            sceDisplayWaitVblankStart();
            disp_i = draw_i;
            pend_i = -1;
        } else {
            /* (counted from after the call: a blank during it may or may not have
             * taken it - taken as not, the one on the screen kept safe) */
            pend_vc = (int)sceDisplayGetVcount();
            pend_i = draw_i;
        }
    }
    frame_waiting = 0;
}

const uint32_t *gfx_gu_drawn(void)
{
    /* (the buffer just drawn, before gfx_gu_show: uncached VRAM) */
    gpu_idle();
    return (const uint32_t *)(0x44000000u + buf_at[draw_i]);
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

static uint8_t __attribute__((aligned(64))) atlas[512 * 512 / 2];   /* rows 256-511: the cache's tiles */
static uint32_t __attribute__((aligned(64))) clut_tiles[128];        /* lines 4-7: flashing (colour 15 see-through) */
static uint32_t __attribute__((aligned(64))) clut_pieces[2][256];
static uint32_t __attribute__((aligned(64))) ovl_px[2 * 320 * 232];   /* the overlays, one after another */

typedef struct { unsigned short u, v; short x, y, z; } V16;

/* a sprite's two vertices, its part left of or above the screen's edge cut off
 * (texels 1:1, flipped when u1 < u0 or v1 < v0): the GE in 2D mode takes no
 * negative coordinates */
static inline void sprite2(V16 *o, int x0, int y0, int x1, int y1, int u0, int v0, int u1, int v1)
{
    if (x0 < 0) { int d = x1 > 0 ? -x0 : x1 - x0; u0 += u1 >= u0 ? d : -d; x0 = 0; if (x1 < 0) x1 = 0; }
    if (y0 < 0) { int d = y1 > 0 ? -y0 : y1 - y0; v0 += v1 >= v0 ? d : -d; y0 = 0; if (y1 < 0) y1 = 0; }
    o[0].u = (unsigned short)u0; o[0].v = (unsigned short)v0; o[0].x = (short)x0; o[0].y = (short)y0; o[0].z = 0;
    o[1].u = (unsigned short)u1; o[1].v = (unsigned short)v1; o[1].x = (short)x1; o[1].y = (short)y1; o[1].z = 0;
}

static uint32_t abgr(uint32_t c) { return 0xFF000000u | ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF); }

static int atlas_put(const uint8_t *tiles, uint8_t *changed, int base)
{
    int t, any = 0;
    for (t = 0; t < 2048; t++) {
        const uint8_t *src;
        uint8_t *dst;
        int r;
        if (!changed[t]) continue;
        changed[t] = 0;
        src = tiles + t * 32;
        dst = atlas + (base + (t >> 6) * 8) * 256 + (t & 63) * 4;
        for (r = 0; r < 8; r++) {
            int i;
            for (i = 0; i < 4; i++) {
                uint8_t b = src[r * 4 + i];
                dst[r * 256 + i] = (uint8_t)((b >> 4) | (b << 4));
            }
        }
        any = 1;
    }
    return any;
}

static void atlas_update(const ViewList *vl)
{
    int changed;
    if (vl->frozen) return;
    changed = atlas_put(vl->vram, vl->tiles_changed, 0);
    if (vl->cache && vl->cache_changed) changed |= atlas_put(vl->cache, vl->cache_changed, 256);
    if (changed) {
        sceKernelDcacheWritebackRange(atlas, sizeof atlas);
        sceGuTexFlush();
    }
}

static void tiles_tex(void)
{
    sceGuTexMode(GU_PSM_T4, 0, 0, 0);
    sceGuTexImage(0, 512, 512, 512, atlas);
    sceGuClutLoad(128 / 8, clut_tiles);
}

/* one piece (an 8-bit texture) */
static void piece_draw(const ViewList *vl, const VlPiece *p, int high, int x, int y)
{
    V16 *v;
    int y0 = p->y0 > 0 ? p->y0 : 0, y1 = p->y1 < vl->h ? p->y1 : vl->h;
    if (y0 >= y1) return;
    v = (V16 *)sceGuGetMemory(2 * sizeof(V16));
    sceGuScissor(x, y + y0, x + vl->w, y + y1);
    sceGuTexMode(GU_PSM_T8, 0, 0, 0);
    sceGuTexImage(0, pow2(p->w), pow2(p->h), p->stride, p->px);
    sceGuTexFlush();
    sprite2(v, x + p->x, y + p->y, x + p->x + p->w, y + p->y + p->h, 0, 0, p->w, p->h);
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, v);
}

static void pieces_clut(int high)
{
    sceGuClutLoad(256 / 8, clut_pieces[high]);
    sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
}

/* a cell's sprite: the atlas texel and its palette group (line, + 4 flashing) */
static inline int cell_put(V16 *o, const VlCell *cl, int x, int y)
{
    unsigned e = cl->entry, t = (e & 0x7FF) + ((cl->ext & 1) ? 2048 : 0);
    unsigned short u0 = (unsigned short)((t & 63) * 8), v0 = (unsigned short)((t >> 6) * 8);
    sprite2(o, x + cl->x, y + cl->y, x + cl->x + 8, y + cl->y + 8,
            (e & 0x0800) ? u0 + 8 : u0, (e & 0x1000) ? v0 + 8 : v0, (e & 0x0800) ? u0 : u0 + 8, (e & 0x1000) ? v0 : v0 + 8);
    return (int)((e >> 13) & 3) + ((cl->ext & 2) ? 4 : 0);
}

/* The cells' vertices: written to cached memory (the display list's, from
 * sceGuGetMemory, is uncached - every store a trip to the bus), then written
 * back for the GE. One frame's; the GE is done with the last before the next
 * frame's list is begun. */
static V16 __attribute__((aligned(64))) vbuf[(VL_MAX_CELLS + 256) * 2];
static int vb_used;
static V16 *vget(int n)
{
    if (vb_used + n > (int)(sizeof vbuf / sizeof vbuf[0])) return (V16 *)sceGuGetMemory((unsigned)n * sizeof(V16));
    vb_used += n;
    return vbuf + vb_used - n;
}
static void vflush(V16 *v, int n)
{
    if (v >= vbuf && v < vbuf + sizeof vbuf / sizeof vbuf[0]) sceKernelDcacheWritebackRange(v, (unsigned)n * sizeof(V16));
}

/* Each batch's vertices for both priorities, made once a frame in two runs over
 * its cells (count, then put); the passes then only issue the draws. Planes: a
 * block per priority, its palette groups one after another (a draw each);
 * sprites: in their order, a draw for each run of one palette group. */
typedef struct { V16 *v[2]; short n[2][8]; int run0[1], runs[1]; } Prep;
static Prep prep[VL_MAX_BATCHES];
#define MAX_RUNS 2048
static struct { short grp, n; int from; } runs[MAX_RUNS];
static int nruns;

static void prepare(const ViewList *vl, int x, int y)
{
    int k;
    nruns = 0;
    for (k = 0; k < vl->nbatches; k++) {
        const VlBatch *b = &vl->batches[k];
        Prep *P = &prep[k];
        int c, h, g, tot[2] = {0, 0};
        memset(P, 0, sizeof *P);
        if (b->piece >= 0 || !b->count || b->layer > 2) continue;
        for (c = b->first; c < b->first + b->count; c++) {
            const VlCell *cl = &vl->cells[c];
            P->n[(cl->entry >> 15) & 1][((cl->entry >> 13) & 3) + ((cl->ext & 2) ? 4 : 0)]++;
        }
        for (h = 0; h < 2; h++) {
            for (g = 0; g < 8; g++) tot[h] += P->n[h][g];
            P->v[h] = tot[h] ? vget(tot[h] * 2) : NULL;
        }
        if (b->layer < 2) {
            V16 *at[2][8];
            for (h = 0; h < 2; h++) {
                int t = 0;
                for (g = 0; g < 8; g++) { at[h][g] = P->v[h] + t * 2; t += P->n[h][g]; }
            }
            for (c = b->first; c < b->first + b->count; c++) {
                const VlCell *cl = &vl->cells[c];
                h = (cl->entry >> 15) & 1;
                g = (int)(((cl->entry >> 13) & 3) + ((cl->ext & 2) ? 4 : 0));
                cell_put(at[h][g], cl, x, y);
                at[h][g] += 2;
            }
        } else {
            V16 *o[2] = {P->v[0], P->v[1]};
            int cur[2] = {-1, -1}, last[2] = {-1, -1};
            P->run0[0] = nruns;
            for (c = b->first; c < b->first + b->count; c++) {
                const VlCell *cl = &vl->cells[c];
                int grp;
                h = (cl->entry >> 15) & 1;
                grp = cell_put(o[h], cl, x, y);
                if (grp != cur[h] && nruns < MAX_RUNS) {
                    runs[nruns].grp = (short)(grp | (h << 8));
                    runs[nruns].from = (int)(o[h] - P->v[h]) / 2;
                    runs[nruns].n = 0;
                    last[h] = nruns++;
                    cur[h] = grp;
                }
                if (last[h] >= 0) runs[last[h]].n++;
                o[h] += 2;
            }
            P->runs[0] = nruns - P->run0[0];
        }
    }
}

static void tiles_pass(const ViewList *vl, int stage, int layer, int high, int x, int y)
{
    int k;
    tiles_tex();
    for (k = 0; k < vl->nbatches; k++) {
        const VlBatch *b = &vl->batches[k];
        const Prep *P = &prep[k];
        int g, t;
        if (b->layer != layer || b->stage != stage) continue;
        if (b->piece >= 0) {
            /* (an object's piece in its place among the sprites) */
            pieces_clut(high);
            piece_draw(vl, &vl->pieces[b->piece], high, x, y);
            tiles_tex();
            continue;
        }
        if (!P->v[high]) continue;
        sceGuScissor(x + b->x0, y + b->y0, x + b->x1, y + b->y1);
        if (layer == 2) {
            int r;
            for (r = P->run0[0]; r < P->run0[0] + P->runs[0]; r++) {
                if ((runs[r].grp >> 8) != high) continue;
                sceGuClutMode(GU_PSM_8888, 0, 0x0F, (unsigned)(runs[r].grp & 0xFF));
                sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, runs[r].n * 2, 0, P->v[high] + runs[r].from * 2);
            }
            continue;
        }
        for (g = 0, t = 0; g < 8; g++) {
            if (P->n[high][g]) {
                sceGuClutMode(GU_PSM_8888, 0, 0x0F, (unsigned)g);
                sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, P->n[high][g] * 2, 0, P->v[high] + t * 2);
            }
            t += P->n[high][g];
        }
    }
}

static void pieces_pass(const ViewList *vl, int high, int x, int y)
{
    int k;
    pieces_clut(high);
    for (k = 0; k < vl->npieces; k++) piece_draw(vl, &vl->pieces[k], high, x, y);
}

static void view_list_(const ViewList *vl, int x, int y);
static void view_list(const ViewList *vl, int x, int y)
{
    PROF_PUSH(PROF_GE);
    view_list_(vl, x, y);
    PROF_POP();
}

static void view_list_(const ViewList *vl, int x, int y)
{
    int i, high, k;
    vb_used = 0;
    atlas_update(vl);
    for (i = 0; i < 64; i++) {
        clut_tiles[i] = (i & 15) ? abgr(vl->pal[i]) : 0;
        clut_tiles[64 + i] = (i & 15) && (i & 15) != 15 ? abgr(vl->pal[i]) : 0;
    }
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
    prepare(vl, x, y);
    vflush(vbuf, vb_used);
    /* the backdrop */
    sceGuScissor(x, y, x + vl->w, y + vl->h);
    sceGuClearColor(abgr(vl->backdrop));
    sceGuClear(GU_COLOR_BUFFER_BIT);
    sceGuEnable(GU_ALPHA_TEST);
    sceGuAlphaFunc(GU_GREATER, 0, 0xFF);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    {
        int stage, has1 = 0;
        for (k = 0; k < vl->nbatches; k++) has1 |= vl->batches[k].stage;
        for (stage = 0; stage <= has1; stage++) {
            for (k = 0; k < vl->nbatches; k++) {
                /* (the stage's rectangles to fill with the backdrop first) */
                const VlBatch *b = &vl->batches[k];
                if (b->stage != stage || b->layer != 3) continue;
                typedef struct { short x, y, z; } VP;
                VP *q = (VP *)sceGuGetMemory(2 * sizeof(VP));
                q[0].x = (short)(x + b->x0); q[0].y = (short)(y + b->y0); q[0].z = 0;
                q[1].x = (short)(x + b->x1); q[1].y = (short)(y + b->y1); q[1].z = 0;
                sceGuScissor(x, y, x + vl->w, y + vl->h);
                sceGuDisable(GU_TEXTURE_2D);
                sceGuColor(abgr(vl->backdrop));
                sceGuDrawArray(GU_SPRITES, GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, q);
                sceGuEnable(GU_TEXTURE_2D);
            }
            for (high = 0; high < 2; high++) {
                tiles_pass(vl, stage, 0, high, x, y);
                tiles_pass(vl, stage, 1, high, x, y);
                tiles_pass(vl, stage, 2, high, x, y);
                if (!vl->pieces_in_batches && stage == 0) pieces_pass(vl, high, x, y);
            }
        }
    }
    sceGuDisable(GU_ALPHA_TEST);
    sceGuScissor(0, 0, SCR_W, SCR_H);
    /* the finished pictures over it (status bar, present list, menus, boxes),
     * copied one after another into one area the GE reads */
    {
        size_t used = 0;
        static VlOverlay ovl_was[VL_MAX_OVERLAYS];
        static uint32_t *ovl_was_at[VL_MAX_OVERLAYS];
        for (k = vl->noverlays; k < VL_MAX_OVERLAYS; k++) ovl_was[k].stamp = 0;
        for (k = 0; k < vl->noverlays; k++) {
            const VlOverlay *o = &vl->overlays[k];
            int r, c, w = o->w > 512 ? 512 : o->w, h = o->h > 512 ? 512 : o->h, tbw;
            if (o->tile_w) { w = o->tile_w; h = o->tile_h; }   /* (a tile: uploaded once, drawn over the rectangle) */
            tbw = (w + 3) & ~3;
            uint32_t *d = ovl_px + used;
            V16 *v;
            if (used + (size_t)tbw * (size_t)h > sizeof ovl_px / 4) break;
            used += ((size_t)tbw * (size_t)h + 15) & ~(size_t)15;
            /* (the same pixels in the same place as last frame: there already) */
            if (o->stamp && k < VL_MAX_OVERLAYS && ovl_was[k].stamp == o->stamp && ovl_was[k].px == o->px &&
                ovl_was[k].pitch == o->pitch && ovl_was[k].keyed == o->keyed && ovl_was[k].w == w && ovl_was[k].h == h &&
                ovl_was_at[k] == d)
                goto copied;
            for (r = 0; r < h; r++) {
                const uint32_t *src = o->px + (size_t)r * o->pitch;
                uint32_t *dst = d + r * tbw;
                if (o->keyed) for (c = 0; c < w; c++) dst[c] = (src[c] >> 24) ? abgr(src[c]) : 0;
                else for (c = 0; c < w; c++) dst[c] = abgr(src[c]);
            }
            sceKernelDcacheWritebackRange(d, (unsigned)(tbw * h * 4));
        copied:
            ovl_was[k] = *o;
            ovl_was[k].w = w;
            ovl_was[k].h = h;
            ovl_was_at[k] = d;
            sceGuTexMode(GU_PSM_8888, 0, 0, 0);
            sceGuTexImage(0, pow2(w), pow2(h), tbw, d);
            sceGuTexFlush();
            sceGuTexFunc(GU_TFX_REPLACE, o->keyed ? GU_TCC_RGBA : GU_TCC_RGB);
            if (o->keyed) { sceGuEnable(GU_ALPHA_TEST); sceGuAlphaFunc(GU_GREATER, 0, 0xFF); }
            if (o->tile_w) {
                /* the tile in a grid from (tx0, ty0), cut to the rectangle */
                int gx0 = o->x - ((((o->x - o->tx0) % w) + w) % w), gy0 = o->y - ((((o->y - o->ty0) % h) + h) % h), gx, gy, nq = 0;
                V16 *q;
                for (gy = gy0; gy < o->y + o->h; gy += h)
                    for (gx = gx0; gx < o->x + o->w; gx += w) nq++;
                q = v = (V16 *)sceGuGetMemory((unsigned)nq * 2 * sizeof(V16));
                for (gy = gy0; gy < o->y + o->h; gy += h)
                    for (gx = gx0; gx < o->x + o->w; gx += w) {
                        sprite2(q, x + gx, y + gy, x + gx + w, y + gy + h, 0, 0, w, h);
                        q += 2;
                    }
                sceGuScissor(x + o->x, y + o->y, x + o->x + o->w, y + o->y + o->h);
                sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, nq * 2, 0, v);
                sceGuScissor(0, 0, SCR_W, SCR_H);
            } else {
                v = (V16 *)sceGuGetMemory(2 * sizeof(V16));
                sprite2(v, x + o->x, y + o->y, x + o->x + w, y + o->y + h, 0, 0, w, h);
                sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, v);
            }
            if (o->keyed) sceGuDisable(GU_ALPHA_TEST);
        }
    }
}

static const GfxBackend backend = {
    "sceGu", tex_create, tex_update, tex_destroy, frame_begin, clip, quad, frame_end, view_list, gpu_idle
};

const GfxBackend *gfx_gu_init(void)
{
    /* frame buffers: three 512 x 272 x 32-bit ones at the start of video memory */
    sceGuInit();
    sceGuStart(GU_DIRECT, list);
    sceGuDrawBuffer(GU_PSM_8888, (void *)buf_at[0], BUF_W);
    sceGuDispBuffer(SCR_W, SCR_H, (void *)buf_at[1], BUF_W);
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
    sceDisplaySetFrameBuf((uint8_t *)sceGeEdramGetAddr() + buf_at[1], BUF_W, PSP_DISPLAY_PIXEL_FORMAT_8888,
                          PSP_DISPLAY_SETBUF_IMMEDIATE);
    disp_i = 1;
    pend_i = -1;
    return &backend;
}

void gfx_gu_vsync(int on) { vsync = on; }

void gfx_gu_wait_blank(void)
{
    if (vsync) sceDisplayWaitVblankStart();
}
