#include "gfx/view.h"

static int whole_zoom(int win_w, int win_h)
{
    int sx = win_w / VIEW_BASE_W, sy = win_h / VIEW_BASE_H;
    return sx < sy ? sx : sy;
}

static void centre(int win_w, int win_h, ViewLayout *o)
{
    o->dst_x = (win_w - o->dst_w) / 2;
    o->dst_y = (win_h - o->dst_h) / 2;
}

/* Shrink the original picture into a window smaller than it, keeping its shape. */
static void shrink(int win_w, int win_h, ViewLayout *o)
{
    o->scale = 0;
    o->view_w = VIEW_BASE_W;
    o->view_h = VIEW_BASE_H;
    if (win_w < 1) win_w = 1;
    if (win_h < 1) win_h = 1;
    if ((long)win_w * VIEW_BASE_H <= (long)win_h * VIEW_BASE_W) {
        o->dst_w = win_w;
        o->dst_h = (int)((long)win_w * VIEW_BASE_H / VIEW_BASE_W);
    } else {
        o->dst_h = win_h;
        o->dst_w = (int)((long)win_h * VIEW_BASE_W / VIEW_BASE_H);
    }
    if (o->dst_w < 1) o->dst_w = 1;
    if (o->dst_h < 1) o->dst_h = 1;
    centre(win_w, win_h, o);
}

void view_layout_fixed(int win_w, int win_h, ViewLayout *o)
{
    int s = whole_zoom(win_w, win_h);
    if (s < 1) { shrink(win_w, win_h, o); return; }
    o->scale = s;
    o->view_w = VIEW_BASE_W;
    o->view_h = VIEW_BASE_H;
    o->dst_w = VIEW_BASE_W * s;
    o->dst_h = VIEW_BASE_H * s;
    centre(win_w, win_h, o);
}

void view_layout_expand(int win_w, int win_h, int max_w, int max_h, ViewLayout *o)
{
    int s = whole_zoom(win_w, win_h), w, h;
    if (max_w < 2) max_w = 2;
    if (max_h < 2) max_h = 2;
    if (win_w < 2) win_w = 2;
    if (win_h < 2) win_h = 2;
    /* never below 1x: a small screen (a DS, a tiny window) shows less of the level */
    if (s < 1) s = 1;
    /* zoom in rather than show more than the limit */
    if (s < (win_w + max_w - 1) / max_w) s = (win_w + max_w - 1) / max_w;
    if (s < (win_h + max_h - 1) / max_h) s = (win_h + max_h - 1) / max_h;
    w = (win_w / s) & ~1;
    h = (win_h / s) & ~1;
    o->scale = s;
    o->view_w = w;
    o->view_h = h;
    o->dst_w = w * s;
    o->dst_h = h * s;
    centre(win_w, win_h, o);
}
