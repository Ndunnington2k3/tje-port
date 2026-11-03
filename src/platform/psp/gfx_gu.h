/* The drawing layer on the PSP's graphics engine (sceGu). */
#ifndef TJE_GFX_GU_H
#define TJE_GFX_GU_H
#include "gfx/gfx.h"
const GfxBackend *gfx_gu_init(void);
/* Wait for the vertical blank before showing each frame (on by default). */
void gfx_gu_vsync(int on);
/* Show the frame just drawn (after the vertical blank when vsync is on). */
void gfx_gu_show(void);
#endif
