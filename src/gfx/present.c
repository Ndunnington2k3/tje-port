#include "gfx/present.h"
#include "gfx/view.h"

#include "gfx/viewlist.h"
#include <stddef.h>

static const GfxBackend *gfx;

/* One per window (two-window mode: one per player); present_select picks which
 * the other calls work on. */
typedef struct {
    GfxTex *frame_tex;
    int tex_w, tex_h;
    int frame_w, frame_h, have_frame;
    int wanted_w, wanted_h;
} Target;
static Target targets[PRESENT_TARGETS] = {
    {NULL, 0, 0, VIEW_BASE_W, VIEW_BASE_H, 0, VIEW_BASE_W, VIEW_BASE_H},
    {NULL, 0, 0, VIEW_BASE_W, VIEW_BASE_H, 0, VIEW_BASE_W, VIEW_BASE_H},
};
static Target *t = &targets[0];
static const ViewList *list;   /* this frame's view as a list, or NULL */

void present_set_list(const ViewList *vl) { list = vl; }
int present_has_lists(void) { return gfx && gfx->view_list; }
void present_gpu_idle(void) { if (gfx && gfx->idle) gfx->idle(); }

void present_select(int target)
{
    if (target >= 0 && target < PRESENT_TARGETS) t = &targets[target];
}

void present_init(const GfxBackend *backend)
{
    gfx = backend;
}

void present_shutdown(void)
{
    int k;
    for (k = 0; k < PRESENT_TARGETS; k++) {
        present_select(k);
        if (gfx && t->frame_tex) gfx->tex_destroy(t->frame_tex);
        t->frame_tex = NULL;
        t->tex_w = t->tex_h = 0;
        t->have_frame = 0;
    }
    present_select(0);
}

void present_set_frame(const uint32_t *pixels, int width, int height, int pitch)
{
    if (!gfx || !pixels) return;   /* (NULL: the frame is a list, present_set_list) */
    present_gpu_idle();            /* (the texture may still be being read) */
    if (!t->frame_tex || width > t->tex_w || height > t->tex_h) {
        if (t->frame_tex) gfx->tex_destroy(t->frame_tex);
        t->tex_w = width > VIEW_BASE_W ? width : VIEW_BASE_W;
        t->tex_h = height > VIEW_BASE_H ? height : VIEW_BASE_H;
        t->frame_tex = gfx->tex_create(t->tex_w, t->tex_h);
        if (!t->frame_tex) return;
    }
    gfx->tex_update(t->frame_tex, 0, 0, width, height, pixels, pitch);
    t->frame_w = width;
    t->frame_h = height;
    t->have_frame = 1;
}

void present_draw(int win_w, int win_h)
{
    ViewLayout v, e;
    if (!gfx) return;
    view_layout_expand(win_w, win_h, VIEW_MAX_W, VIEW_MAX_H, &e);
    t->wanted_w = e.view_w;
    t->wanted_h = e.view_h;
    gfx->frame_begin(win_w, win_h);
    if (list && gfx->view_list) {
        /* (the view at its own size, centred: drawn 1:1) */
        gfx->view_list(list, (win_w - list->w) / 2, (win_h - list->h) / 2);
        list = NULL;
    } else if (t->have_frame && t->frame_tex) {
        if (t->frame_w == VIEW_BASE_W && t->frame_h == VIEW_BASE_H) {
            /* drawn the original way (menus etc.), or a 256-wide picture (the
             * Genesis' narrow mode), which fills the same area, as on a television */
            view_layout_fixed(win_w, win_h, &v);
        } else if (t->frame_w == VIEW_BASE_W * 256 / 320 && t->frame_h == VIEW_BASE_H) {
            view_layout_fixed(win_w, win_h, &v);
        } else {
            /* the gameplay view at the size asked for (or, right after the window
             * changed size, the previous size at the new zoom, centred) */
            v = e;
            v.dst_w = t->frame_w * e.scale;
            v.dst_h = t->frame_h * e.scale;
            v.dst_x = (win_w - v.dst_w) / 2;
            v.dst_y = (win_h - v.dst_h) / 2;
        }
        gfx->quad(t->frame_tex, 0, 0, t->frame_w, t->frame_h, v.dst_x, v.dst_y, v.dst_w, v.dst_h,
                  GFX_OPAQUE | (v.scale ? 0 : GFX_SMOOTH));
    }
    gfx->frame_end();
}

void present_wanted_view(int *w, int *h)
{
    *w = targets[0].wanted_w;
    *h = targets[0].wanted_h;
}

void present_wanted_view_of(int target, int *w, int *h)
{
    if (target < 0 || target >= PRESENT_TARGETS) target = 0;
    *w = targets[target].wanted_w;
    *h = targets[target].wanted_h;
}
