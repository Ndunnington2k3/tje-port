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

/* The game is handing its sprite table to the VDP (vdp_set_sat_dma_hook). */
void gameview_sat_dma(void);

/* One player's view (0 ToeJam, 1 Earl) drawn from game memory at the end of a
 * frame: w x h pixels around the original screen whose top-left is world
 * (cam_x, cam_y) and which is screen_h high (224, or 112 for a split half);
 * game_cam: 0/1 when that is the game's camera 0/1, -1 for a camera of our own.
 * band: draw the middle band of sprites (hint box). Status bar not included. */
int gameview_render_view(int player, int cam_x, int cam_y, int game_cam, int screen_h,
                         uint32_t *out, int w, int h, int pitch, int band);

/* Two-window mode (network play): the platform sets g_gv_windows, and after each
 * gameview_frame asks for each player's own picture at its window's size (game
 * pixels). 0: not a gameplay frame (show the usual picture in both windows). */
extern int g_gv_windows;
/* Let the VDP leave out the lines nobody will see (called once at start-up). */
void gameview_install_line_filter(void);
extern int g_gv_need_vdp;   /* (tests) every VDP line drawn */
int gameview_window_frame(int player, int *w, int *h, const uint32_t **pixels, int *pitch);   /* (w, h: in, the size wanted; out, the picture's) */
/* Two-window mode: a player's Map button opens a map in their window and the game
 * goes on (called with each frame's input, before the game reads it). */
void gameview_map_input(uint16_t pad[2]);
int gameview_map_open(int player);

/* Network play: the player this machine shows (-1: none); the frame's picture is
 * their window's. */
extern int g_gv_net_player;
/* The platform's GPU draws gameplay views from lists (gfx/viewlist.h): set at
 * start-up when present_has_lists(). The frame's picture is then NULL. */
extern int g_gv_use_list;
/* (tests) the list of the last view drawn from a list */
struct ViewList;
const struct ViewList *gameview_last_list(void);
/* A box with a message over the middle of the picture (NULL: none). Lower-case
 * letters, digits, space and - . , ' ! only. */
void gameview_set_message(const char *text);

/* (tests) split screen: is half k (0 top, 1 bottom) shown as the VDP draws it this
 * frame (under its player's menu, held while falling, or the split being set up)? */
int gameview_split_half_shown_as_vdp(int k);

#endif
