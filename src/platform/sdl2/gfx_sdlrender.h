#ifndef TJE_GFX_SDLRENDER_H
#define TJE_GFX_SDLRENDER_H

#include <SDL.h>
#include "gfx/gfx.h"

const GfxBackend *gfx_sdlrender_init(SDL_Renderer *r);
void gfx_sdlrender_use(SDL_Renderer *r);

#endif
