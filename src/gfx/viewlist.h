/*
 * A gameplay view as a list of things for a GPU to draw (the PSP's GE; any
 * backend with gfx's view_list), instead of a finished picture: the Genesis'
 * own graphics are drawn by the GPU straight from its tiles.
 *
 *   - cells: 8x8 tiles of the Genesis' video memory (VRAM), each a plane entry
 *     (tile number, flips, palette line, priority), for plane B (layer 0) and
 *     plane A or the window (layer 1), in batches each with a rectangle it is
 *     limited to;
 *   - pieces: the objects' sprites as the view renderer decodes them (one byte a
 *     pixel: bits 0-5 colour, 6 priority, 7 opaque), in drawing order, back first;
 *   - overlays: finished 32-bit pictures put over it all (status bar, present
 *     list), drawn last.
 *
 * Colour 0 of a tile and pixels without bit 7 are see-through. What shows where
 * they overlap is the Genesis' rule, drawn in six passes from the back:
 * plane B, plane A, sprites without priority, then the same with priority; the
 * backdrop colour first. viewlist_render_soft() draws a list exactly so (tests).
 */
#ifndef TJE_VIEWLIST_H
#define TJE_VIEWLIST_H

#include <stdint.h>

#define VL_MAX_CELLS    8192
#define VL_MAX_BATCHES  24
#define VL_MAX_PIECES   96
#define VL_MAX_OVERLAYS 16

typedef struct { int16_t x, y; uint16_t entry; } VlCell;   /* top-left in the picture */

typedef struct {
    int layer;                     /* 0 plane B, 1 plane A / window */
    int x0, y0, x1, y1;            /* drawn only inside (picture pixels, x1/y1 excluded) */
    int first, count;              /* its cells */
} VlBatch;

typedef struct {
    int x, y, w, h;                /* in the picture */
    int y0, y1;                    /* rows it may cover */
    int stride;                    /* bytes a row (a multiple of 16) */
    const uint8_t *px;             /* w x h */
} VlPiece;

typedef struct {
    int x, y, w, h, pitch;         /* in the picture; pitch in pixels */
    const uint32_t *px;            /* 0x00RRGGBB, all drawn */
} VlOverlay;

typedef struct ViewList {
    int w, h;                      /* the picture */
    uint32_t backdrop;             /* 0x00RRGGBB */
    uint32_t pal[64];              /* the four palette lines, 0x00RRGGBB */
    const uint8_t *vram;           /* the tiles: 32 bytes each, high nibble first */
    uint8_t *tiles_changed;        /* tiles written since a GPU last read them (it clears them) */
    int frozen;                    /* keep the GPU's copy of the tiles as it is (picture held) */
    VlCell cells[VL_MAX_CELLS];
    int ncells;
    VlBatch batches[VL_MAX_BATCHES];
    int nbatches;
    VlPiece pieces[VL_MAX_PIECES];
    int npieces;
    VlOverlay overlays[VL_MAX_OVERLAYS];
    int noverlays;
} ViewList;

/* Draw a list into 32-bit pixels (0x00RRGGBB), exactly as a GPU must (tests). */
void viewlist_render_soft(const ViewList *vl, uint32_t *out, int pitch);

#endif
