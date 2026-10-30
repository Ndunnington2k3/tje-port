/*
 * Putting each frame on the screen, through the drawing layer (gfx.h).
 *
 * For now every screen is the picture the Genesis video model made, drawn at the
 * largest whole-number zoom that fits the window (view_layout_fixed). The new
 * gameplay view will be drawn here too.
 */
#ifndef TJE_PRESENT_H
#define TJE_PRESENT_H

#include <stdint.h>
#include "gfx/gfx.h"

void present_init(const GfxBackend *backend);
void present_shutdown(void);

/* Hand over the frame the video model made (0x00RRGGBB pixels). */
void present_set_frame(const uint32_t *pixels, int width, int height, int pitch);

/* Draw the latest frame into a target of win_w x win_h pixels (between the
 * backend's frame_begin and frame_end). Can be called again without a new
 * frame, e.g. while the window is being resized. */
void present_draw(int win_w, int win_h);

/* The size (game pixels) a gameplay view should have to fill the window that
 * present_draw last drew into (view_layout_expand). 320x224 until then. */
void present_wanted_view(int *w, int *h);

#endif
