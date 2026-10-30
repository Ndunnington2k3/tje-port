/* The gameplay view drawn from game memory (src/game/gameview.c, docs/RENDERER.md). */
#ifndef TJE_GAMEVIEW_H
#define TJE_GAMEVIEW_H

#include <stdint.h>

#define GAMEVIEW_MAX_W 640
#define GAMEVIEW_MAX_H 448

/* Called after the VDP has drawn each visible line (genesis_set_line_hook): draws
 * the same line from game memory, with the machine as it is at that moment. */
void gameview_line(int line);

/* The size of the gameplay picture wanted (game pixels; e.g. from view_layout_expand
 * for the window). Takes effect at the next frame. 320x224 is the original. */
void gameview_set_size(int w, int h);

/* At the end of a frame: the gameplay picture of this frame (0x00RRGGBB, any size), if every
 * line of it was gameplay. Returns 0 when the screen showed something else
 * (menus, map, transitions): show the VDP picture then. */
int gameview_frame(const uint32_t **pixels, int *width, int *height, int *pitch);

/* Keep the menus' starry sky (plane B as it is now) to go around the map. */
void gameview_capture_stars(void);

#endif
