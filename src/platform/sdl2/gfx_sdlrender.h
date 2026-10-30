#ifndef TJE_GFX_SDLRENDER_H
#define TJE_GFX_SDLRENDER_H

#include <SDL.h>
#include "gfx/gfx.h"

const GfxBackend *gfx_sdlrender_init(SDL_Renderer *r);

#endif
