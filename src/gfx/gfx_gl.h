#ifndef TJE_GFX_GL_H
#define TJE_GFX_GL_H

#include "gfx/gfx.h"

/* Set up the OpenGL backend in the current context. getproc returns GL function
 * addresses (e.g. SDL_GL_GetProcAddress). NULL if OpenGL 2.1 is not available. */
const GfxBackend *gfx_gl_init(void *(*getproc)(const char *name));

#endif
