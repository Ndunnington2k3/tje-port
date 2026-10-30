/*
 * The drawing layer: everything the port's renderer needs from a GPU, and no more.
 *
 * Textures hold 32-bit pixels 0xAARRGGBB (in a uint32_t). Drawing is a batch of
 * textured rectangles, with optional flipping, clipping and alpha test (pixels
 * with alpha < 128 are not drawn). Each platform supplies one implementation:
 * OpenGL on PC (with SDL's 2D renderer as a fallback), sceGu on PSP, citro3d on
 * 3DS, and a software one for tests.
 */
#ifndef TJE_GFX_H
#define TJE_GFX_H

#include <stdint.h>

typedef struct GfxTex GfxTex;

enum {
    GFX_FLIP_X = 1,
    GFX_FLIP_Y = 2,
    GFX_OPAQUE = 4,      /* ignore the alpha channel (draw every pixel) */
    GFX_SMOOTH = 8       /* filter when scaling (only for shrinking below the original size) */
};

typedef struct GfxBackend {
    const char *name;
    GfxTex *(*tex_create)(int w, int h);
    void (*tex_update)(GfxTex *t, int x, int y, int w, int h, const uint32_t *pixels, int pitch_pixels);
    void (*tex_destroy)(GfxTex *t);
    /* Start a frame on a target of w x h pixels, cleared to black. */
    void (*frame_begin)(int w, int h);
    /* Limit drawing to a rectangle of the target (w <= 0: no limit). */
    void (*clip)(int x, int y, int w, int h);
    /* Draw the rectangle (sx, sy, sw, sh) of a texture at (dx, dy, dw, dh). */
    void (*quad)(GfxTex *t, int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh, unsigned flags);
    /* Finish the frame (the platform shows it). */
    void (*frame_end)(void);
} GfxBackend;

#endif
