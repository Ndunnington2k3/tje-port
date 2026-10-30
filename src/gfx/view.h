/*
 * Where the picture goes in a window of any size (pure arithmetic, every platform).
 *
 * The game never sees any of this: it always runs with its original 320x224
 * cameras. These only decide how the picture is scaled and, for the gameplay
 * view, how much of the level around the camera is shown.
 */
#ifndef TJE_VIEW_H
#define TJE_VIEW_H

#define VIEW_BASE_W 320     /* the original screen, in game pixels */
#define VIEW_BASE_H 224

/* The most of the level the gameplay view may show: the area around the camera
 * where the game keeps Earthlings awake (the screen plus 160 px each side and
 * 112 px above and below), so a frozen one is never seen. */
#define VIEW_MAX_W 640
#define VIEW_MAX_H 448

typedef struct {
    int view_w, view_h;   /* game pixels shown (the original 320x224, or more for gameplay) */
    int dst_x, dst_y;     /* where they go in the window (pixels) */
    int dst_w, dst_h;
    int scale;            /* whole-number zoom; 0 when the window is too small and the picture is shrunk */
} ViewLayout;

/* Menus, cutscenes and anything else drawn the original way: the 320x224 picture
 * at the largest whole-number zoom that fits, centred (borders around it).
 * Smaller than 320x224: shrunk to fit, keeping its shape. */
void view_layout_fixed(int win_w, int win_h, ViewLayout *out);

/* The gameplay view, always at a whole-number zoom and filling the window (but
 * for less than one zoom step at the edges):
 *  - the zoom is the largest that still shows the original 320x224 area, and
 *    the rest of the window shows more of the level;
 *  - if that would show more than max_w x max_h game pixels, it zooms in further
 *    instead (the original area may then be partly cut off in one direction);
 *  - never below 1x: a window or screen smaller than 320x224 (e.g. a DS) shows
 *    less of the level around the camera.
 * view_w and view_h are even, so the camera stays in the middle. */
void view_layout_expand(int win_w, int win_h, int max_w, int max_h, ViewLayout *out);

#endif
