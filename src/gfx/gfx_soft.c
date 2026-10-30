/*
 * Software backend of the drawing layer: draws into a block of memory. Used by the
 * headless build, so tests can check exactly what a window of any size shows.
 * Nearest-pixel scaling only (GFX_SMOOTH is ignored).
 */
#include "gfx/gfx_soft.h"

#include <stdlib.h>
#include <string.h>

struct GfxTex {
    int w, h;
    uint32_t *px;
};

static uint32_t *target;
static int tw, th;
static int cx0, cy0, cx1, cy1;   /* clip rectangle, end exclusive */

static GfxTex *tex_create(int w, int h)
{
    GfxTex *t = (GfxTex *)calloc(1, sizeof *t);
    if (!t) return NULL;
    t->px = (uint32_t *)calloc((size_t)w * (size_t)h, sizeof(uint32_t));
    if (!t->px) { free(t); return NULL; }
    t->w = w;
    t->h = h;
    return t;
}

static void tex_update(GfxTex *t, int x, int y, int w, int h, const uint32_t *pixels, int pitch)
{
    int row;
    for (row = 0; row < h; row++)
        memcpy(t->px + (size_t)(y + row) * (size_t)t->w + (size_t)x, pixels + (size_t)row * (size_t)pitch,
               (size_t)w * sizeof(uint32_t));
}

static void tex_destroy(GfxTex *t)
{
    if (!t) return;
    free(t->px);
    free(t);
}

static void frame_begin(int w, int h)
{
    if (w != tw || h != th || !target) {
        free(target);
        target = (uint32_t *)malloc((size_t)w * (size_t)h * sizeof(uint32_t));
        tw = w;
        th = h;
    }
    if (target) memset(target, 0, (size_t)w * (size_t)h * sizeof(uint32_t));
    cx0 = cy0 = 0;
    cx1 = w;
    cy1 = h;
}

static void clip(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) { cx0 = cy0 = 0; cx1 = tw; cy1 = th; return; }
    cx0 = x < 0 ? 0 : x;
    cy0 = y < 0 ? 0 : y;
    cx1 = x + w > tw ? tw : x + w;
    cy1 = y + h > th ? th : y + h;
}

static void quad(GfxTex *t, int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh, unsigned flags)
{
    int x, y;
    if (!t || !target || dw <= 0 || dh <= 0) return;
    for (y = dy < cy0 ? cy0 : dy; y < dy + dh && y < cy1; y++) {
        int v = (int)((long)(y - dy) * sh / dh);
        const uint32_t *src;
        if (flags & GFX_FLIP_Y) v = sh - 1 - v;
        src = t->px + (size_t)(sy + v) * (size_t)t->w + (size_t)sx;
        for (x = dx < cx0 ? cx0 : dx; x < dx + dw && x < cx1; x++) {
            int u = (int)((long)(x - dx) * sw / dw);
            uint32_t c;
            if (flags & GFX_FLIP_X) u = sw - 1 - u;
            c = src[u];
            if (!(flags & GFX_OPAQUE) && (c >> 24) < 0x80) continue;
            target[(size_t)y * (size_t)tw + (size_t)x] = c | 0xFF000000u;
        }
    }
}

static void frame_end(void)
{
}

static const GfxBackend backend = {
    "software", tex_create, tex_update, tex_destroy, frame_begin, clip, quad, frame_end
};

const GfxBackend *gfx_soft_init(void)
{
    return &backend;
}

const uint32_t *gfx_soft_target(int *w, int *h)
{
    *w = tw;
    *h = th;
    return target;
}
