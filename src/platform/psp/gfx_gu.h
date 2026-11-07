/* The drawing layer on the PSP's graphics engine (sceGu). */
#ifndef TJE_GFX_GU_H
#define TJE_GFX_GU_H
#include "gfx/gfx.h"
const GfxBackend *gfx_gu_init(void);
/* Wait for the vertical blank before showing each frame (on by default). */
void gfx_gu_vsync(int on);
/* (a frame not drawn: paced as one) */
void gfx_gu_wait_blank(void);
/* Show the frame just drawn (after the vertical blank when vsync is on). */
void gfx_gu_show(void);
/* (tests) the frame just drawn, 512 pixels a row, 0xAABBGGRR, before gfx_gu_show */
const uint32_t *gfx_gu_drawn(void);
#endif
