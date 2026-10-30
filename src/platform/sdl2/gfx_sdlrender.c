/*
 * Fallback backend of the drawing layer: SDL's own 2D renderer (Direct3D,
 * OpenGL, or software, whatever SDL finds). Used when OpenGL 2.1 is missing.
 * Alpha test is done with blending: the port's textures only use alpha 0 or 255.
 */
#include "gfx/gfx.h"
#include "gfx_sdlrender.h"

#include <stdlib.h>

struct GfxTex {
    SDL_Texture *tex;
    int w, h;
};

static SDL_Renderer *ren;

static GfxTex *tex_create(int w, int h)
{
    GfxTex *t = (GfxTex *)calloc(1, sizeof *t);
    if (!t) return NULL;
    t->tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    t->w = w;
    t->h = h;
    if (!t->tex) { free(t); return NULL; }
    return t;
}

static void tex_update(GfxTex *t, int x, int y, int w, int h, const uint32_t *pixels, int pitch)
{
    SDL_Rect r = {x, y, w, h};
    SDL_UpdateTexture(t->tex, &r, pixels, pitch * 4);
}

static void tex_destroy(GfxTex *t)
{
    if (!t) return;
    SDL_DestroyTexture(t->tex);
    free(t);
}

static void frame_begin(int w, int h)
{
    (void)w; (void)h;
    SDL_RenderSetClipRect(ren, NULL);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
}

static void clip(int x, int y, int w, int h)
{
    SDL_Rect r = {x, y, w, h};
    SDL_RenderSetClipRect(ren, (w > 0 && h > 0) ? &r : NULL);
}

static void quad(GfxTex *t, int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh, unsigned flags)
{
    SDL_Rect s = {sx, sy, sw, sh};
    SDL_Rect d = {dx, dy, dw, dh};
    int flip = ((flags & GFX_FLIP_X) ? SDL_FLIP_HORIZONTAL : 0) | ((flags & GFX_FLIP_Y) ? SDL_FLIP_VERTICAL : 0);
    if (!t) return;
    SDL_SetTextureBlendMode(t->tex, (flags & GFX_OPAQUE) ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t->tex, (flags & GFX_SMOOTH) ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
    SDL_RenderCopyEx(ren, t->tex, &s, &d, 0.0, NULL, (SDL_RendererFlip)flip);
}

static void frame_end(void)
{
}

static const GfxBackend backend = {
    "SDL renderer", tex_create, tex_update, tex_destroy, frame_begin, clip, quad, frame_end
};

const GfxBackend *gfx_sdlrender_init(SDL_Renderer *r)
{
    ren = r;
    return &backend;
}
