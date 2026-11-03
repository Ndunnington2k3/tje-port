/*
 * The drawing layer (gfx/gfx.h) on the PSP's graphics engine, through sceGu.
 *
 * Two 32-bit frame buffers in video memory (512 pixels a row, 480 x 272 shown),
 * textures in main memory (the GE reads them there), quads drawn as sprites in
 * strips 64 texels wide (the texture cache's width).
 */
#include "gfx_gu.h"

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

static unsigned int __attribute__((aligned(64))) list[64 * 1024];
static int vsync = 1;

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

static void frame_end(void)
{
    sceGuFinish();
    sceGuSync(0, 0);
}

void gfx_gu_show(void)
{
    if (vsync) sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
}

static const GfxBackend backend = {
    "sceGu", tex_create, tex_update, tex_destroy, frame_begin, clip, quad, frame_end
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
