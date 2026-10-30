#ifndef TJE_GFX_SOFT_H
#define TJE_GFX_SOFT_H

#include "gfx/gfx.h"

const GfxBackend *gfx_soft_init(void);

/* The picture drawn by the last frame (0xFFRRGGBB), and its size. */
const uint32_t *gfx_soft_target(int *w, int *h);

#endif
