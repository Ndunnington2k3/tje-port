/* Checks for the window layout rules (src/gfx/view.c). Exit code 0 = all passed. */
#include "gfx/view.h"

#include <stdio.h>

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static void expect(const char *what, const ViewLayout *v, int scale, int vw, int vh, int dx, int dy, int dw, int dh)
{
    CHECK(v->scale == scale && v->view_w == vw && v->view_h == vh && v->dst_x == dx && v->dst_y == dy &&
          v->dst_w == dw && v->dst_h == dh,
          "%s: got scale %d view %dx%d dst %d,%d %dx%d", what, v->scale, v->view_w, v->view_h,
          v->dst_x, v->dst_y, v->dst_w, v->dst_h);
}

int main(void)
{
    ViewLayout v;
    int w, h;

    /* fixed (menus etc.) */
    view_layout_fixed(960, 672, &v);   expect("fixed exact 3x", &v, 3, 320, 224, 0, 0, 960, 672);
    view_layout_fixed(1920, 1080, &v); expect("fixed 1080p", &v, 4, 320, 224, 320, 92, 1280, 896);
    view_layout_fixed(1000, 700, &v);  expect("fixed odd", &v, 3, 320, 224, 20, 14, 960, 672);
    view_layout_fixed(160, 112, &v);   expect("fixed tiny", &v, 0, 320, 224, 0, 0, 160, 112);
    view_layout_fixed(400, 112, &v);   expect("fixed tiny wide", &v, 0, 320, 224, 120, 0, 160, 112);

    /* expand (gameplay) */
    view_layout_expand(1920, 1080, VIEW_MAX_W, VIEW_MAX_H, &v);
    expect("1080p", &v, 4, 480, 270, 0, 0, 1920, 1080);
    view_layout_expand(400, 240, VIEW_MAX_W, VIEW_MAX_H, &v);
    expect("3DS top", &v, 1, 400, 240, 0, 0, 400, 240);
    view_layout_expand(480, 272, VIEW_MAX_W, VIEW_MAX_H, &v);
    expect("PSP", &v, 1, 480, 272, 0, 0, 480, 272);
    view_layout_expand(960, 672, VIEW_MAX_W, VIEW_MAX_H, &v);
    expect("exact 3x", &v, 3, 320, 224, 0, 0, 960, 672);
    view_layout_expand(3440, 1440, VIEW_MAX_W, VIEW_MAX_H, &v);
    expect("ultrawide", &v, 6, 572, 240, 4, 0, 3432, 1440);
    view_layout_expand(5120, 1440, VIEW_MAX_W, VIEW_MAX_H, &v);
    expect("super ultrawide zooms in", &v, 8, 640, 180, 0, 0, 5120, 1440);
    view_layout_expand(3840, 2160, VIEW_MAX_W, VIEW_MAX_H, &v);
    expect("4K", &v, 9, 426, 240, 3, 0, 3834, 2160);
    view_layout_expand(256, 192, VIEW_MAX_W, VIEW_MAX_H, &v);
    expect("DS screen", &v, 1, 256, 192, 0, 0, 256, 192);
    view_layout_expand(100, 100, VIEW_MAX_W, VIEW_MAX_H, &v);
    expect("tiny", &v, 1, 100, 100, 0, 0, 100, 100);

    /* every size: whole zoom, never more than the limit, inside the window and
     * filling it but for less than two zoom steps (one, plus the even size), even-sized
     * view (camera in the middle), and the original area shown whenever the window
     * holds it within the limit */
    for (w = 2; w <= 6000; w += 7)
        for (h = 2; h <= 3400; h += 11) {
            view_layout_expand(w, h, VIEW_MAX_W, VIEW_MAX_H, &v);
            CHECK(v.scale >= 1 && v.dst_w == v.view_w * v.scale && v.dst_h == v.view_h * v.scale, "%dx%d not whole", w, h);
            CHECK(v.view_w <= VIEW_MAX_W && v.view_h <= VIEW_MAX_H, "%dx%d too much shown", w, h);
            CHECK(v.dst_x >= 0 && v.dst_y >= 0 && v.dst_x + v.dst_w <= w && v.dst_y + v.dst_h <= h,
                  "%dx%d outside the window", w, h);
            CHECK(w - v.dst_w < 2 * v.scale && h - v.dst_h < 2 * v.scale, "%dx%d does not fill the window", w, h);
            CHECK(!(v.view_w & 1) && !(v.view_h & 1), "%dx%d odd view", w, h);
            {
                /* some whole zoom shows the original area without passing the limit */
                int z = w / VIEW_BASE_W < h / VIEW_BASE_H ? w / VIEW_BASE_W : h / VIEW_BASE_H;
                if (z >= 1 && z >= (w + VIEW_MAX_W - 1) / VIEW_MAX_W && z >= (h + VIEW_MAX_H - 1) / VIEW_MAX_H)
                    CHECK(v.view_w >= VIEW_BASE_W && v.view_h >= VIEW_BASE_H, "%dx%d hides the original area", w, h);
            }
            view_layout_fixed(w, h, &v);
            CHECK(v.dst_x >= 0 && v.dst_y >= 0 && v.dst_x + v.dst_w <= w && v.dst_y + v.dst_h <= h,
                  "%dx%d fixed outside the window", w, h);
        }

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("view_test: all passed\n");
    return 0;
}
