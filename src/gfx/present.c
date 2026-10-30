#include "gfx/present.h"
#include "gfx/view.h"

#include <stddef.h>

static const GfxBackend *gfx;
static GfxTex *frame_tex;
static int tex_w, tex_h;
static int frame_w = VIEW_BASE_W, frame_h = VIEW_BASE_H, have_frame;

void present_init(const GfxBackend *backend)
{
    gfx = backend;
}

void present_shutdown(void)
{
    if (gfx && frame_tex) gfx->tex_destroy(frame_tex);
    frame_tex = NULL;
    tex_w = tex_h = 0;
    have_frame = 0;
}

void present_set_frame(const uint32_t *pixels, int width, int height, int pitch)
{
    if (!gfx) return;
    if (!frame_tex || width > tex_w || height > tex_h) {
        if (frame_tex) gfx->tex_destroy(frame_tex);
        tex_w = width > VIEW_BASE_W ? width : VIEW_BASE_W;
        tex_h = height > VIEW_BASE_H ? height : VIEW_BASE_H;
        frame_tex = gfx->tex_create(tex_w, tex_h);
        if (!frame_tex) return;
    }
    gfx->tex_update(frame_tex, 0, 0, width, height, pixels, pitch);
    frame_w = width;
    frame_h = height;
    have_frame = 1;
}

void present_draw(int win_w, int win_h)
{
    ViewLayout v;
    if (!gfx) return;
    gfx->frame_begin(win_w, win_h);
    if (have_frame && frame_tex) {
        /* A 256-wide picture (the Genesis' narrow mode) fills the same area as a
         * 320-wide one, as on a television. */
        view_layout_fixed(win_w, win_h, &v);
        gfx->quad(frame_tex, 0, 0, frame_w, frame_h, v.dst_x, v.dst_y, v.dst_w, v.dst_h,
                  GFX_OPAQUE | (v.scale ? 0 : GFX_SMOOTH));
    }
    gfx->frame_end();
}
