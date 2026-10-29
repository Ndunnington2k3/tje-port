/* The port's own artwork, converted by tools/art2c.py from the PNGs in art/. */
#ifndef TJE_ART_H
#define TJE_ART_H

#include <stdint.h>

typedef struct {
    int width, height;
    struct { int slot; uint16_t color; } palette[16];  /* Genesis colour for each slot used */
    int colors;
    const unsigned char *pixels;                       /* width x height colour slots, 0 = clear */
} ArtImage;

/* "LOAD?" title for the saved-games list (palette line 1, slots 12-15) */
extern const ArtImage art_load_title;

#endif
