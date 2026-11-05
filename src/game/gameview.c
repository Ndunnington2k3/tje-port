/*
 * The gameplay view, drawn from game memory instead of by the Genesis video chip
 * (docs/RENDERER.md).
 *
 * Step 2: the same 320x224 picture as the VDP, built differently:
 *  - the level from the floor layout (TOEJAM_LEVEL_TILES / EARL_LEVEL_TILES), cell
 *    by cell, with the game's own block -> cell tables ($9849C, $9FE9C), so any
 *    part of the floor can be drawn, not only what the game streamed into plane A;
 *  - sprites from the sprite table the VDP shows, without its per-line limits;
 *  - the status bar (window plane) taken from the VDP picture.
 * The game is not touched: this only reads memory.
 */
#include "game.h"
#include "gameview.h"
#include "core/prof.h"
#include "options.h"
#include "core/vdp.h"
#include "gfx/present.h"
#include "gfx/viewlist.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define TOEJAM_CAMERA_XPOS  0xFF8002u   /* camera copies the planes were scrolled to */
#define TOEJAM_CAMERA_YPOS  0xFF8004u
#define EARL_CAMERA_XPOS    0xFF8006u
#define EARL_CAMERA_YPOS    0xFF8008u
#define SPLIT_SCREEN        0xFF8010u   /* VHBLANK_FLAG_1: the screen is split */
#define CELL_ATTR           0xFF8196u   /* priority bit of every level cell ($8000 or 0) */
#define TOEJAM_LEVEL_TILES  0xFF81AAu   /* 26 rows x 19 blocks, one word per 128x64 block */
#define EARL_LEVEL_TILES    0xFF8586u
#define TOEJAMS_LEVEL       0xFF912Cu
#define EARLS_LEVEL         0xFF912Eu
#define BLOCK_CELLS         0x09849Cu   /* ROM: cells of each block type (8 rows x 16) */
#define EDGE_CELLS          0x09FE9Cu   /* ROM: cells of the shore/edge overlays */
#define FINAL_LEVEL         26
#define SPACE_BLOCK         0x52        /* what lies beyond the edges of floors 1 and 26 */

#define PIX_OPAQUE 0x80
#define PIX_PRIO   0x40

/* LOC_00001612: cell of a block type at cell row sy (0-7), column sx (0-15) */
static uint32_t block_cell(uint32_t t, int sy, int sx)
{
    if (t & 0x80)
        return rd16(BLOCK_CELLS + ((t & 0x7F) << 8) + (uint32_t)sy * 32 + (uint32_t)(15 - sx) * 2) ^ 0x0800u;
    return rd16(BLOCK_CELLS + (t << 8) + (uint32_t)sy * 32 + (uint32_t)sx * 2);
}

/* LOC_00001670: cell of an edge overlay (0 = none) */
static uint32_t edge_cell(uint32_t h, int sy, int sx)
{
    uint32_t c;
    if (h & 0x10) {
        c = rd16(EDGE_CELLS + (((h & 0x0F) - 1) << 8) + (uint32_t)sy * 32 + (uint32_t)(15 - sx) * 2);
        return c ? c ^ 0x0800u : 0;
    }
    return rd16(EDGE_CELLS + ((h - 1) << 8) + (uint32_t)sy * 32 + (uint32_t)sx * 2);
}

/* The floor layouts as they were at the top of the frame (copied then): the
 * per-player views are drawn at the end of the frame, by when the game may have
 * changed them (falling to another floor). */
static uint8_t layout_copy[2][19 * 26 * 2];
static int layout_copy_on;

static uint32_t layout_word(uint32_t tiles, uint32_t off)
{
    if (layout_copy_on && (tiles == TOEJAM_LEVEL_TILES || tiles == EARL_LEVEL_TILES) && off + 1 < sizeof layout_copy[0]) {
        const uint8_t *c = layout_copy[tiles == EARL_LEVEL_TILES];
        return ((uint32_t)c[off] << 8) | c[off + 1];
    }
    return rd16(tiles + off);
}

/* The plane cell at cell (cx, cy) of a floor, as the game's plane streamers
 * (LOC_000016D2, LOC_00001BA8) would write it, for any position. */
static uint32_t level_cell(uint32_t tiles, int level, int cx, int cy)
{
    uint32_t attr = rd16(CELL_ATTR);
    int bx = cx >> 4, by = cy >> 3, sx = cx & 15, sy = cy & 7;
    uint32_t word, t = 0, h;
    if (cx < 0 || cy < 0 || bx == 0 || bx > 0x13 || by == 0 || by > 0x1A) {
        if (level == 1 || level == FINAL_LEVEL) return attr | block_cell(SPACE_BLOCK, sy, sx);
        return attr;
    }
    word = layout_word(tiles, (uint32_t)((by - 1) * 19 + (bx - 1)) * 2);
    h = (word & 0x1F00u) >> 8;
    if (h) t = edge_cell(h, sy, sx);
    if (!t) t = block_cell(word & 0xFF, sy, sx);
    return attr | t;
}

/* The floor's plane cells, kept per player and brought up to date block by block
 * when the layout (or the floor, or the cell attribute) changes, instead of being
 * worked out from the layout for every 8 pixels of every row: the same cells
 * level_cell gives, for cells (0..335, 0..223) - the floor and the space around
 * it - and the space pattern beyond. Used while a view is being drawn. */
#define FLOOR_CW 336
#define FLOOR_CH 224
static uint16_t floor_grid[2][FLOOR_CH][FLOOR_CW];
static uint16_t floor_space[2][8][16];
static uint8_t floor_words[2][19 * 26 * 2];
static int floor_level[2] = {-1, -1}, floor_attr[2] = {-1, -1};
static int floor_on[2];   /* the grid is right for the view being drawn */

static void floor_sync(uint32_t tiles, int level)
{
    int p = tiles == EARL_LEVEL_TILES, full, bx, by, sx, sy;
    int attr = (int)rd16(CELL_ATTR);
    full = level != floor_level[p] || attr != floor_attr[p];
    if (full) {
        for (sy = 0; sy < FLOOR_CH; sy++)
            for (sx = 0; sx < FLOOR_CW; sx++) {
                int bxx = sx >> 4, byy = sy >> 3;
                if (bxx == 0 || bxx > 0x13 || byy == 0 || byy > 0x1A)
                    floor_grid[p][sy][sx] = (uint16_t)level_cell(tiles, level, sx, sy);
            }
        for (sy = 0; sy < 8; sy++)
            for (sx = 0; sx < 16; sx++) floor_space[p][sy][sx] = (uint16_t)level_cell(tiles, level, -16 + sx, -8 + sy);
        floor_level[p] = level;
        floor_attr[p] = attr;
    }
    for (by = 0; by < 26; by++)
        for (bx = 0; bx < 19; bx++) {
            uint32_t off = (uint32_t)(by * 19 + bx) * 2, w = layout_word(tiles, off);
            if (!full && floor_words[p][off] == (uint8_t)(w >> 8) && floor_words[p][off + 1] == (uint8_t)w) continue;
            floor_words[p][off] = (uint8_t)(w >> 8);
            floor_words[p][off + 1] = (uint8_t)w;
            for (sy = 0; sy < 8; sy++)
                for (sx = 0; sx < 16; sx++) {
                    int cx = (bx + 1) * 16 + sx, cy = (by + 1) * 8 + sy;
                    floor_grid[p][cy][cx] = (uint16_t)level_cell(tiles, level, cx, cy);
                }
        }
    floor_on[p] = 1;
}

static inline uint32_t floor_cell(uint32_t tiles, int level, int cx, int cy)
{
    int p = tiles == EARL_LEVEL_TILES;
    if (!floor_on[p]) return level_cell(tiles, level, cx, cy);
    if (cx >= 0 && cy >= 0 && cx < FLOOR_CW && cy < FLOOR_CH) return floor_grid[p][cy][cx];
    return floor_space[p][cy & 7][cx & 15];
}

/* Decoded tiles: each tile's 8 rows of 8 colour numbers (0-15), one byte a pixel,
 * redone when the VDP says the tile was written (vdp_tile_dirty). */
static uint64_t tile_cache[2048][8];

static inline uint64_t tile_pixels(uint32_t tile, int row)
{
    if (vdp_tile_dirty[tile]) {
        const uint8_t *p = &vdp_vram[tile * 32];
        int r, i;
        for (r = 0; r < 8; r++) {
            uint8_t px[8];
            for (i = 0; i < 4; i++) {
                px[i * 2] = (uint8_t)(p[r * 4 + i] >> 4);
                px[i * 2 + 1] = (uint8_t)(p[r * 4 + i] & 15);
            }
            memcpy(&tile_cache[tile][r], px, 8);
        }
        vdp_tile_dirty[tile] = 0;
    }
    return tile_cache[tile][row];
}

/* 8 pixels of a tile row into line-buffer form (bits 0-5 colour, 6 priority, 7
 * opaque): all 8 at once, a byte each in a 64-bit word (the byte at the lowest
 * address is the leftmost pixel, the same order on every little- or big-endian
 * machine, as the word only ever goes through memcpy) */
static void tile_row(uint8_t out[8], uint32_t entry, int row)
{
    uint64_t x, opaque, o;
    uint32_t tile = entry & 0x7FF;
    if (entry & 0x1000) row = 7 - row;
    x = tile_pixels(tile, row);
    if (entry & 0x0800) {
        /* (mirrored: the bytes in the opposite order - on either kind of machine,
         * reversing a word's bytes reverses their order in memory) */
#if defined(__GNUC__) || defined(__clang__)
        x = __builtin_bswap64(x);
#else
        x = ((x & 0x00FF00FF00FF00FFull) << 8) | ((x >> 8) & 0x00FF00FF00FF00FFull);
        x = ((x & 0x0000FFFF0000FFFFull) << 16) | ((x >> 16) & 0x0000FFFF0000FFFFull);
        x = (x << 32) | (x >> 32);
#endif
    }
    opaque = (x + 0x7F7F7F7F7F7F7F7Full) & 0x8080808080808080ull;   /* (bit 7 of each non-zero byte) */
    o = x | opaque | ((opaque >> 7) * (uint64_t)((entry >> 9) & 0x30));
    if (entry & 0x8000) o |= 0x4040404040404040ull;
    memcpy(out, &o, 8);
}

typedef struct {
    int top, bottom;          /* screen rows */
    int cam_x, cam_y;         /* world position of the region's top-left pixel */
    uint32_t tiles;
    int level;
} Region;

/* Does plane A in VRAM hold this floor where the camera is? (If not, the screen is
 * showing something else: the map, a cutscene, a transition...) */
static int plane_match;   /* the last plane_shows_level: percent of the samples matching */

static int plane_shows_level(const Region *r, int skip_from, int skip_to)
{
    static const int psz[4] = {32, 64, 32, 128};
    int pw = psz[vdp_reg[16] & 3], ph = psz[(vdp_reg[16] >> 4) & 3];
    uint32_t base = (uint32_t)(vdp_reg[2] & 0x38) << 10;
    int i, j, match = 0, total = 0;
    int rows = (r->bottom - r->top) / 8;
    for (j = 1; j < rows - 1; j += 3)
        for (i = 2; i < 38; i += 5) {
            int screen_y = r->top + j * 8, cx = (r->cam_x >> 3) + i, cy = (r->cam_y >> 3) + j;
            uint32_t ea = (base + (uint32_t)((cy & (ph - 1)) * pw + (cx & (pw - 1))) * 2) & 0xFFFF;
            uint32_t v = ((uint32_t)vdp_vram[ea] << 8) | vdp_vram[ea + 1];
            if (screen_y + 8 > skip_from && screen_y < skip_to) continue;   /* under the window */
            if (screen_y + 8 > 184) continue;      /* where the status bar may be */
            total++;
            if (v == level_cell(r->tiles, r->level, cx, cy)) match++;
        }
    plane_match = total > 0 ? match * 100 / total : 0;
    return total > 0 && match * 10 >= total * 9;
}

/* ---- rows ----------------------------------------------------------------------
 * Every row function writes into a line buffer that covers the whole picture:
 * buffer index = original-screen x + off (off = where x 0 of the original screen
 * is in the picture), for original x from x_from to x_to. */

/* the level of a region from the floor layout */
static void level_row(uint8_t *buf, int off, int x_from, int x_to, const Region *r, int screen_y)
{
    int wy = r->cam_y + (screen_y - r->top);
    int cy = wy >> 3, row = wy & 7;
    int wx0 = r->cam_x + x_from;
    int cx = wx0 >> 3;            /* (arithmetic shift: floor for negatives too) */
    int x = x_from - (wx0 & 7);
    for (; x < x_to; x += 8, cx++) {
        uint8_t px[8];
        int i;
        tile_row(px, floor_cell(r->tiles, r->level, cx, cy), row);
        if (x >= x_from && x + 8 <= x_to) memcpy(buf + x + off, px, 8);
        else
            for (i = 0; i < 8; i++)
                if (x + i >= x_from && x + i < x_to) buf[x + i + off] = px[i];
    }
}

/* A plane as the VDP shows it now (scroll as set at this moment). plane: 0 = A, 1 = B. */
static void plane_row(uint8_t *buf, int off, int x_from, int x_to, int screen_y, int plane)
{
    static const int psz[4] = {32, 64, 32, 128};
    int pw = psz[vdp_reg[16] & 3], ph = psz[(vdp_reg[16] >> 4) & 3];
    uint32_t base = plane ? (uint32_t)(vdp_reg[4] & 7) << 13 : (uint32_t)(vdp_reg[2] & 0x38) << 10;
    uint32_t hsbase = (uint32_t)(vdp_reg[13] & 0x3F) << 10, hsaddr;
    int hl = screen_y < 0 ? 0 : screen_y > VDP_HEIGHT - 1 ? VDP_HEIGHT - 1 : screen_y;
    int hs, vs, y, col, col0, col1;
    switch (vdp_reg[11] & 3) {
    case 0: hsaddr = hsbase; break;
    case 2: hsaddr = hsbase + (uint32_t)(hl & ~7) * 4; break;
    case 3: hsaddr = hsbase + (uint32_t)hl * 4; break;
    default: hsaddr = hsbase + (uint32_t)(hl & 7) * 4; break;
    }
    hsaddr = (hsaddr + (plane ? 2u : 0u)) & 0xFFFF;
    hs = ((vdp_vram[hsaddr] << 8) | vdp_vram[hsaddr + 1]) & 0x3FF;
    if (hs & 0x200) hs -= 0x400;
    vs = vdp_vsram[plane];        /* (whole-screen vertical scroll: the game does not use columns) */
    y = (screen_y + vs) & (ph * 8 - 1);
    col0 = (x_from - (hs & 7)) >> 3;
    col1 = (x_to - (hs & 7) + 7) >> 3;
    for (col = col0 - 1; col <= col1; col++) {
        int sx = col * 8 + (hs & 7);
        int tx = (col - (hs >> 3)) & (pw - 1);
        uint32_t ea = (base + (uint32_t)((y >> 3) * pw + tx) * 2) & 0xFFFF;
        uint8_t px[8];
        int i;
        if (sx + 8 <= x_from || sx >= x_to) continue;
        tile_row(px, ((uint32_t)vdp_vram[ea] << 8) | vdp_vram[ea + 1], y & 7);
        for (i = 0; i < 8; i++)
            if (sx + i >= x_from && sx + i < x_to) buf[sx + i + off] = px[i];
    }
}

/* ---- the game's state as the VDP shows it -------------------------------------
 * The game runs at 30 frames a second: its main loop spends two video frames on
 * one pass, and hands the VDP a new sprite table (DMA to VRAM, at VBlank) every
 * other frame. In between, the objects in RAM are half updated (a new animation
 * frame already set, sprites being reallocated, $4A briefly 0). So objects are
 * read from a copy of RAM taken when the sprite table goes to the VDP: what is
 * drawn from frame data then shows the same moment as the hardware sprites, and
 * an object handed over between the two at the old screen edge does not jump or
 * blink. (Live RAM when the game has not sent a sprite table for a while.) */

static uint8_t ram_snap[0x10000];
static int snap_age = 1000;          /* frames since the copy was taken */
int g_gv_snap_age;                   /* (tests) */
int g_gv_gameplay;                   /* (tests) this frame is the gameplay view */

static inline uint32_t ob8(uint32_t a)
{
    if (a >= 0xE00000u && snap_age < 4) return ram_snap[a & 0xFFFFu];
    return rd8(a);
}
static inline uint32_t ob16(uint32_t a)
{
    if (a >= 0xE00000u && snap_age < 4)
        return ((uint32_t)ram_snap[a & 0xFFFFu] << 8) | ram_snap[(a + 1) & 0xFFFFu];
    return rd16(a);
}
static inline uint32_t ob32(uint32_t a)
{
    return (ob16(a) << 16) | ob16(a + 2);
}

/* ---- sprites ----------------------------------------------------------------- */

#define OBJECTS             0xFFA25Au   /* TOEJAMS_XPOS: 40 objects of $80 bytes */
#define OBJECT_SIZE         0x80u
#define NUM_OBJECTS         40

/* Which hardware sprites belong to objects of the level (players, Earthlings...)
 * as opposed to the status bar and other screen furniture. */
FILE *g_gv_sync_log;           /* (tests) */
static uint8_t object_sprite[80];
static uint8_t sat_linked[80];   /* entries linked in the sprite table the game last sent */
/* Hardware sprites not to draw: those of objects drawn from their frame data
 * instead (because part of them is where a hardware sprite cannot go). */
static uint8_t sat_suppressed[80];
#define RANK_FRONT   (-1)        /* sprites that are not objects' (status bar icons...) */
#define RANK_SHADOW  10000       /* shadows: behind every object */
static int cur_rank;                  /* rank of the pieces being added */
static int cur_cam;                   /* which of the game's cameras the view is drawn for (0, 1) */
static int cur_ui;                    /* the pieces being added are the present list's own icons */
static int view_top_window;           /* rows of the window plane from the top (present list) in the view being drawn */
#define CAM_BIT    (cur_cam ? 4 : 2)          /* $2F: has sprites on this camera */
#define CAM_SLOTS  (cur_cam ? 0x3Cu : 0x30u)  /* its hardware sprites on this camera */
#define CAM_SHADOW (cur_cam ? 0x49u : 0x48u)  /* its shadow sprite on this camera */
static int clip_top = -(1 << 20), clip_bottom = 1 << 20;   /* rows the pieces being added may cover (split halves) */
static int sprite_rank[80];           /* rank of each hardware sprite (by its object) */

static void find_object_sprites(void)
{
    int e, i;
    memset(object_sprite, 0, sizeof object_sprite);
    for (e = 0; e < NUM_OBJECTS; e++) {
        uint32_t o = OBJECTS + (uint32_t)e * OBJECT_SIZE;
        uint32_t fr;
        int n;
        if (!(ob8(o + 0x2F) & 1) || !(ob8(o + 0x2F) & 2)) continue;
        fr = ob32(o + 0x22);
        n = fr ? (int)ob8(fr) : 0;
        for (i = 0; i < n && i < 12; i++) object_sprite[ob8(o + 0x30 + (uint32_t)i) % 80] = 1;
        if (ob8(o + 0x48)) object_sprite[ob8(o + 0x48) % 80] = 1;   /* its shadow */
    }
    object_sprite[0] = 0;
    if (g_gv_sync_log) {
        /* (tests) hardware sprites in the gameplay area that are no object's */
        uint32_t base = (uint32_t)(vdp_reg[5] & 0x7E) << 9;
        int link = 0, count = 0;
        do {
            const uint8_t *sp = &vdp_vram[(base + (uint32_t)link * 8) & 0xFFFF];
            int sy = (((sp[0] << 8) | sp[1]) & 0x3FF) - 128, sx = (((sp[6] << 8) | sp[7]) & 0x1FF) - 128;
            int idx = link;
            link = sp[3] & 0x7F;
            if (!object_sprite[idx % 80] && idx && sy > -32 && sy < 224 && sx > -32 && sx < 320)
            {
#ifdef TJE_SAT_TRACE
                extern uint32_t g_sat_writer[80][4];
                fprintf(g_gv_sync_log, "nonobj %d at %d,%d size %X tile %04X by %05X %05X %05X %05X\n", idx, sx, sy, sp[2],
                        (sp[4] << 8) | sp[5], g_sat_writer[idx % 80][0], g_sat_writer[idx % 80][1],
                        g_sat_writer[idx % 80][2], g_sat_writer[idx % 80][3]);
#else
                fprintf(g_gv_sync_log, "nonobj %d at %d,%d size %X tile %04X\n", idx, sx, sy, sp[2], (sp[4] << 8) | sp[5]);
#endif
            }
        } while (link && ++count < 80);
    }
}

/* One row of the sprite table, as the VDP draws it (earlier sprites in the link
 * list in front), without its per-line limits. which: 1 = objects' sprites,
 * 2 = the others, 3 = all. */
static int16_t row_rank[GAMEVIEW_MAX_W];   /* rank of the sprite pixel in each column (pieces_row) */

static void sprite_row(uint8_t *buf, int off, int x_from, int x_to, int y, int which)
{
    uint32_t base = (uint32_t)(vdp_reg[5] & 0x7E) << 9;
    int link = 0, count = 0, on_line = 0;
    do {
        int idx = link;
        const uint8_t *s = &vdp_vram[(base + (uint32_t)link * 8) & 0xFFFF];
        int sy = (((s[0] << 8) | s[1]) & 0x3FF) - 128;
        int hs = ((s[2] >> 2) & 3) + 1, vs = (s[2] & 3) + 1;
        uint32_t attr = ((uint32_t)s[4] << 8) | s[5];
        int sx = (((s[6] << 8) | s[7]) & 0x1FF) - 128;
        link = s[3] & 0x7F;
        if (y >= sy && y < sy + vs * 8) {
            /* a sprite at x 0 after another on the line hides the rest of the line
             * (the VDP's sprite masking, which the game uses) */
            if (sx == -128 && on_line > 0) break;
            on_line++;
        }
        if (y >= sy && y < sy + vs * 8 && !sat_suppressed[idx % 80] && (which & (object_sprite[idx % 80] ? 1 : 2))) {
            int row = y - sy, c;
            if (attr & 0x1000) row = vs * 8 - 1 - row;
            for (c = 0; c < hs; c++) {
                int colx = (attr & 0x0800) ? (hs - 1 - c) : c;
                uint32_t tile = (attr & 0x7FF) + (uint32_t)(colx * vs + (row >> 3));
                uint8_t px[8];
                int i;
                tile_row(px, ((attr & 0xF800) & ~0x1000u) | (tile & 0x7FF), row & 7);
                for (i = 0; i < 8; i++) {
                    int x = sx + c * 8 + i;
                    if (x >= x_from && x < x_to && (px[i] & PIX_OPAQUE) && !(buf[x + off] & PIX_OPAQUE)) {
                        buf[x + off] = px[i];
                        row_rank[x + off] = (int16_t)sprite_rank[idx % 80];
                    }
                }
            }
        }
        count++;
    } while (link != 0 && count < 80);
}

/* ---- objects outside the original screen --------------------------------------
 * The game gives hardware sprites only to objects on its 320x224 screen, but it
 * keeps animating the others; their current frame ($22) points into the ROM:
 *   frame: byte piece count, then from +4 one 10-byte piece each:
 *   +0 width, +1 height (cells), +2 x, +3 y, +4 x when flipped, +5 flags
 *   (bit 7 interleaved RLE, bit 6 raw, else RLE; bits 0-1 palette line),
 *   +6 graphics (long). (LOC_0000C222, LOC_0000C900, SpriteUnpack*.) */

#define MAX_PIECES 96
#define PIECE_MAX_PX (32 * 32)

typedef struct {
    int x, y, w, h;          /* original-screen position and size in pixels */
    int rank;                /* drawing order: lower in front (the game's order) */
    int cy0, cy1;            /* rows it may cover (its half of a split screen) */
    int ui;                  /* drawn over the present list too (the list's own icons) */
    /* (pieces_as_cells: the piece as cells of tiles - VRAM's, or the ROM graphics'
     * cache's - for a GPU; px then not filled, piece_px makes it) */
    int ncells;
    struct { int8_t dx, dy; uint16_t entry, ext; } cells[16];
    uint8_t px[PIECE_MAX_PX];
} Piece;

/* ---- the objects' ROM graphics as tiles, for a GPU -----------------------------
 * The game decodes an object's frame into VRAM only for objects on its screen;
 * the others are drawn from the ROM (add_frame). For a GPU those are decoded
 * once into a cache of tiles (the ROM never changes): an object's pieces are then
 * cells like the VRAM's. Filled up: emptied and started again. */
static int pieces_as_cells;
static uint8_t sc_tiles[VL_CACHE_TILES * 32] __attribute__((aligned(64)));
static uint8_t sc_changed[VL_CACHE_TILES];
static int sc_used;
#define SC_SLOTS 4096
static struct { uint32_t src; uint8_t flags, tiles; uint16_t base; } sc_tab[SC_SLOTS];
static void unpack(uint32_t src, int flags, int tiles, uint8_t *out);

static int sc_tiles_for(uint32_t src, int flags, int tiles)
{
    uint32_t h = (src * 2654435761u ^ (uint32_t)flags * 40503u ^ (uint32_t)tiles) % SC_SLOTS;
    int n;
    flags &= 0xC0;
    for (n = 0; n < SC_SLOTS; n++, h = (h + 1) % SC_SLOTS) {
        if (!sc_tab[h].tiles) break;
        if (sc_tab[h].src == src && sc_tab[h].flags == flags && sc_tab[h].tiles == tiles) return sc_tab[h].base;
    }
    if (sc_used + tiles > VL_CACHE_TILES || n == SC_SLOTS) {
        memset(sc_tab, 0, sizeof sc_tab);
        sc_used = 0;
        h = (src * 2654435761u ^ (uint32_t)flags * 40503u ^ (uint32_t)tiles) % SC_SLOTS;
    }
    sc_tab[h].src = src;
    sc_tab[h].flags = (uint8_t)flags;
    sc_tab[h].tiles = (uint8_t)tiles;
    sc_tab[h].base = (uint16_t)sc_used;
    unpack(src, flags, tiles, sc_tiles + (size_t)sc_used * 32);
    memset(sc_changed + sc_used, 1, (size_t)tiles);
    sc_used += tiles;
    return sc_tab[h].base;
}

/* a piece's cells as the VDP lays out a sprite of hs x vs cells: tile (attr &
 * 0x7FF) + column * vs + row, the whole flipped by attr's flips */
static void piece_sprite_cells(Piece *p, uint32_t attr, int hs, int vs, uint16_t ext)
{
    int c, r;
    p->ncells = 0;
    for (c = 0; c < hs; c++)
        for (r = 0; r < vs; r++) {
            int colx = (attr & 0x0800) ? hs - 1 - c : c, rowy = (attr & 0x1000) ? vs - 1 - r : r;
            uint32_t tile = ((attr & 0x7FF) + (uint32_t)(colx * vs + rowy)) & 0x7FF;
            p->cells[p->ncells].dx = (int8_t)(c * 8);
            p->cells[p->ncells].dy = (int8_t)(r * 8);
            p->cells[p->ncells].entry = (uint16_t)((attr & 0xF800) | tile);
            p->cells[p->ncells].ext = ext;
            p->ncells++;
        }
}

/* the pixels of a piece kept as cells (when a pixel of it is needed) */
static void piece_px(Piece *p)
{
    int k, r, i;
    if (!p->ncells) return;
    memset(p->px, 0, (size_t)(p->w * p->h));
    for (k = 0; k < p->ncells; k++) {
        unsigned e = p->cells[k].entry, ext = p->cells[k].ext;
        const uint8_t *tiles = (ext & 1) ? sc_tiles : vdp_vram;
        uint8_t hi = (uint8_t)(PIX_OPAQUE | ((e & 0x8000) ? PIX_PRIO : 0) | ((e >> 9) & 0x30));
        for (r = 0; r < 8; r++) {
            int y = p->cells[k].dy + r, tr = (e & 0x1000) ? 7 - r : r;
            const uint8_t *row = tiles + (e & 0x7FF) * 32 + tr * 4;
            if (y < 0 || y >= p->h) continue;
            for (i = 0; i < 8; i++) {
                int x = p->cells[k].dx + i, ti = (e & 0x0800) ? 7 - i : i, v;
                if (x < 0 || x >= p->w) continue;
                v = (ti & 1) ? (row[ti >> 1] & 15) : (row[ti >> 1] >> 4);
                if (v && !(v == 15 && (ext & 2))) p->px[y * p->w + x] = (uint8_t)(hi | v);
            }
        }
    }
    p->ncells = 0;
}

static Piece pieces[MAX_PIECES];
int g_gv_cam_x, g_gv_cam_y;   /* (tests) */
static int npieces;

/* SpriteUnpackRLE / SpriteUnpackInterleavedRLE / SpriteUnpackRaw into out
 * (tiles * 32 bytes of 4-bit pixels). */
static void unpack(uint32_t src, int flags, int tiles, uint8_t *out)
{
    int total = tiles * 32;
    memset(out, 0, (size_t)total);
    if (flags & 0x80) {
        int pass;
        for (pass = 0; pass < 4; pass++) {
            int left = tiles * 8, o = pass;
            while (left > 0) {
                int b = (int8_t)ob8(src++), n, k;
                if (b < 0) {
                    uint8_t v = 0;
                    if (b & 0x40) v = ob8(src++);
                    n = 64 - (b & 0x3F);
                    for (k = 0; k < n; k++, o += 4) if (o < total) out[o] = v;
                } else if (b & 0x40) {
                    n = b & 0x1F;
                    if (b & 0x20) {
                        uint8_t v = ob8(src++), v2 = (uint8_t)((v << 4) | (v >> 4));
                        for (k = 0; k < n / 2; k++) {
                            if (o < total) out[o] = v;
                            o += 4;
                            if (o < total) out[o] = v2;
                            o += 4;
                        }
                        if (n & 1) { if (o < total) out[o] = v; o += 4; }
                    } else {
                        for (k = 0; k < n; k++, o += 4) if (o < total) out[o] = 0xFF;
                    }
                } else {
                    n = b;
                    for (k = 0; k < n; k++, o += 4) { uint8_t v = ob8(src++); if (o < total) out[o] = v; }
                }
                left -= n;
                if (n <= 0) break;
            }
        }
    } else if (flags & 0x40) {
        int k;
        for (k = 0; k < total; k++) out[k] = ob8(src + (uint32_t)k);
    } else {
        int left = total, o = 0;
        while (left > 0) {
            int b = (int8_t)ob8(src++), n, k;
            if (b < 0) {
                uint8_t v = 0;
                if (b & 0x40) v = ob8(src++);
                n = 64 - (b & 0x3F);
                for (k = 0; k < n; k++, o++) if (o < total) out[o] = v;
            } else if (b & 0x40) {
                n = b & 0x3F;
                for (k = 0; k < n; k++, o++) if (o < total) out[o] = 0xFF;
            } else {
                n = b;
                for (k = 0; k < n; k++, o++) { uint8_t v = ob8(src++); if (o < total) out[o] = v; }
            }
            left -= n;
            if (n <= 0) break;
        }
    }
}

/* LOC_0000BDB8: the game's own "is this object on my screen?" test. An object
 * that passes it but has no sprites is one the game is deliberately not showing. */
static int on_game_screen(uint32_t o, const Region *r)
{
    int dx = (int16_t)ob16(o) - r->cam_x, dy = (int16_t)ob16(o + 2) - r->cam_y;
    int hw = ob8(o + 0x29), hh = ob8(o + 0x2A), z = (int16_t)ob16(o + 4);
    int height = (r->bottom - r->top) >= VDP_HEIGHT ? 0xE0 : 0x70;
    return hw + dx >= 0 && dx - hw <= 0x140 && dy + 10 > 0 && dy - hh - z <= height;
}

/* Decode the pieces of one frame drawn at original-screen position (x, y) with
 * altitude z and attribute word attr ($2C: priority, flip), into the piece list,
 * keeping those that show in x0..x1, y0..y1. */
/* o: the object (0 for items), for its graphic effects as LOC_0000C900 applies
 * them: wading in water (LOC_0000C83A) and flashing (LOC_0000C8CC). */
static void add_frame(uint32_t o, uint32_t fr, int x, int y, int z, int attr, int x0, int y0, int x1, int y1)
{
    static uint8_t data[16 * 32];
    int n, i;
    if (fr < 0x200 || fr >= 0x400000) return;
    n = ob8(fr);
    for (i = 0; i < n && i < 12 && npieces < MAX_PIECES; i++) {
        uint32_t pc = fr + 4 + (uint32_t)i * 10;
        int cw = ob8(pc), ch = ob8(pc + 1), flags = ob8(pc + 5);
        int dx = (int8_t)ob8(pc + ((attr & 0x0800) ? 4 : 2)), dy = (int8_t)ob8(pc + 3);
        int pal = (flags & 3) << 4, prio = (attr & 0x8000) ? PIX_PRIO : 0, flip = attr & 0x0800;
        Piece *p;
        int px, py;
        if (cw < 1 || cw > 4 || ch < 1 || ch > 4) continue;
        p = &pieces[npieces];
        p->rank = cur_rank;
        p->cy0 = clip_top;
        p->cy1 = clip_bottom;
        p->ui = cur_ui;
        p->x = x + dx;
        p->y = y + dy - z;
        p->w = cw * 8;
        p->h = ch * 8;
        if (p->x + p->w <= x0 || p->x >= x1 || p->y + p->h <= y0 || p->y >= y1) continue;
        /* An object the game shows on this camera: its graphics as the game has
         * them in VRAM (the game writes some itself, as speech-bubble text, puts in
         * its effects, and uploads a new animation frame's graphics up to a few
         * frames late); otherwise the frame data from ROM. */
        if (o && (ob8(o + 0x2F) & CAM_BIT) && (ob8(o + 0x4A) || (!cur_cam && sat_linked[ob8(o + CAM_SLOTS) % 80]))) {
            int slot = ob8(o + CAM_SLOTS + (uint32_t)i) % 80;
            uint32_t ent = 0xFFB65Au + (uint32_t)slot * 8;
            int ssz = ob8(ent + 2), sattr = (int)ob16(ent + 4);
            if (slot && ((ssz >> 2) & 3) + 1 == cw && (ssz & 3) + 1 == ch) {
                if (pieces_as_cells) {
                    piece_sprite_cells(p, (uint32_t)sattr, cw, ch, 0);
                    npieces++;
                    continue;
                }
                p->ncells = 0;
                for (py = 0; py < p->h; py++) {
                    int row = (sattr & 0x1000) ? p->h - 1 - py : py, c;
                    for (c = 0; c < cw; c++) {
                        int colx = (sattr & 0x0800) ? (cw - 1 - c) : c;
                        uint32_t tile = ((uint32_t)sattr & 0x7FF) + (uint32_t)(colx * ch + (row >> 3));
                        uint8_t t8[8];
                        tile_row(t8, (((uint32_t)sattr & 0xF800) & ~0x1000u) | (tile & 0x7FF), row & 7);
                        for (px = 0; px < 8; px++) p->px[py * p->w + c * 8 + px] = (t8[px] & PIX_OPAQUE) ? t8[px] : 0;
                    }
                }
                npieces++;
                continue;
            }
        }
        p->ncells = 0;
        if (pieces_as_cells && !(o && z < 0 && (int8_t)ob8(o + 0x5E) <= 1 && !(ob8(o + 0x55) & 0x20))) {
            /* (from the cache: the tiles as the frame lays them out, the piece
             * flipped across by its attribute) */
            int base = sc_tiles_for(ob32(pc + 6), flags, cw * ch), c, r;
            uint16_t ext = (uint16_t)(1 | ((o && (ob8(o + 0x4D) & 0x20)) ? 2 : 0));
            for (c = 0; c < cw; c++)
                for (r = 0; r < ch; r++) {
                    int col = flip ? cw - 1 - c : c;
                    p->cells[p->ncells].dx = (int8_t)(c * 8);
                    p->cells[p->ncells].dy = (int8_t)(r * 8);
                    p->cells[p->ncells].entry = (uint16_t)((flip ? 0x0800 : 0) | ((attr & 0x8000) ? 0x8000 : 0) | ((flags & 3) << 13) |
                                                           ((base + col * ch + r) & 0x7FF));
                    p->cells[p->ncells].ext = ext;
                    p->ncells++;
                }
            npieces++;
            continue;
        }
        unpack(ob32(pc + 6), flags, cw * ch, data);
        /* in water (below ground level, players only): the rows under the
         * waterline are taken away, the first with a wavy edge; a piece wholly
         * under it is not drawn */
        if (o && z < 0 && (int8_t)ob8(o + 0x5E) <= 1 && !(ob8(o + 0x55) & 0x20)) {
            int rows = ch * 8, cut = dy - 2 - z, c, r;
            if (cut >= 0) continue;
            if (cut + rows > 0) {
                for (c = 0; c < cw; c++)
                    for (r = -cut; r < rows; r++) {
                        uint8_t *b = data + (size_t)(c * rows + r) * 4;
                        if (r == -cut) { b[0] &= 0x0F; b[1] &= 0x00; b[2] &= 0x0F; b[3] &= 0xF0; }
                        else b[0] = b[1] = b[2] = b[3] = 0;
                    }
            }
        }
        /* flashing: colour 15 dropped */
        if (o && (ob8(o + 0x4D) & 0x20)) {
            int k;
            for (k = 0; k < cw * ch * 32; k++)
                if (data[k] == 0xFF || data[k] == 0xF0 || data[k] == 0x0F) data[k] = 0;
        }
        for (py = 0; py < p->h; py++)
            for (px = 0; px < p->w; px++) {
                int sxp = flip ? p->w - 1 - px : px;            /* pixel within the unflipped sprite */
                int col = sxp >> 3, row = py >> 3;
                const uint8_t *t = data + (size_t)(col * ch + row) * 32 + (size_t)(py & 7) * 4;
                int bx = sxp & 7;
                uint8_t b = t[bx >> 1];
                int c = (bx & 1) ? (b & 0x0F) : (b >> 4);
                p->px[py * p->w + px] = (uint8_t)(c ? (PIX_OPAQUE | prio | pal | c) : 0);
            }
        npieces++;
    }
}

/* Does any piece of this frame reach outside the area the original screen shows
 * gameplay in (0..320 across, 0..bottom down)? There the game's hardware sprites
 * cannot be relied on: their X wraps past 383, and the game culls and cuts
 * sprites at its screen edges and under its status bar. */
static int frame_leaves_screen(uint32_t fr, int x, int y, int z, int attr, int bottom)
{
    int n, i;
    if (fr < 0x200 || fr >= 0x400000) return 0;
    n = ob8(fr);
    for (i = 0; i < n && i < 12; i++) {
        uint32_t pc = fr + 4 + (uint32_t)i * 10;
        int dx = (int8_t)ob8(pc + ((attr & 0x0800) ? 4 : 2)), dy = (int8_t)ob8(pc + 3);
        int w = ob8(pc) * 8, h = ob8(pc + 1) * 8;
        if (x + dx < 0 || x + dx + w > 320 || y + dy - z < 0 || y + dy - z + h > bottom) return 1;
    }
    return 0;
}

/* Shadows. The game draws an object's shadow as one more hardware sprite ($48),
 * from graphics it keeps in VRAM, centred under the object 4 pixels up
 * (LOC_0000C222). For objects drawn from their frame data the shadow is drawn
 * here, from the shadow sprite last seen for objects of the same shadow class
 * ($2E). */
#define SHADOW_CLASSES 8
static struct { int known, size; uint32_t attr; } shadow_tpl[SHADOW_CLASSES];

static void learn_shadows(void)
{
    uint32_t base = (uint32_t)(vdp_reg[5] & 0x7E) << 9;
    int e;
    for (e = 0; e < NUM_OBJECTS; e++) {
        uint32_t o = OBJECTS + (uint32_t)e * OBJECT_SIZE;
        int cls = ob8(o + 0x2E), slot = ob8(o + 0x48);
        const uint8_t *sp;
        if (!(ob8(o + 0x2F) & 2) || !slot || cls >= SHADOW_CLASSES) continue;
        sp = &vdp_vram[(base + (uint32_t)(slot % 80) * 8) & 0xFFFF];
        if (!(((sp[4] << 8) | sp[5]) & 0x7FF)) continue;
        shadow_tpl[cls].known = 1;
        shadow_tpl[cls].size = sp[2] & 0x0F;
        shadow_tpl[cls].attr = ((uint32_t)sp[4] << 8) | sp[5];
    }
}

static void add_shadow(int cls, int x, int y, int x0, int y0, int x1, int y1, int own_size, uint32_t own_attr)
{
    Piece *p;
    int hs, vs, px, py, size;
    uint32_t sattr;
    if (own_size >= 0) {            /* the object's own shadow sprite (the game picks its size) */
        size = own_size;
        sattr = own_attr;
    } else {
        if (cls <= 0 || cls >= SHADOW_CLASSES || !shadow_tpl[cls].known) return;
        size = shadow_tpl[cls].size;
        sattr = shadow_tpl[cls].attr;
    }
    if (npieces >= MAX_PIECES) return;
    hs = ((size >> 2) & 3) + 1;
    vs = (size & 3) + 1;
    p = &pieces[npieces];
    p->rank = RANK_SHADOW;
    p->cy0 = clip_top;
    p->cy1 = clip_bottom;
    p->ui = 0;
    p->w = hs * 8;
    p->h = vs * 8;
    p->x = x - (hs * 4);
    p->y = y - 4;
    if (p->x + p->w <= x0 || p->x >= x1 || p->y + p->h <= y0 || p->y >= y1) return;
    p->ncells = 0;
    if (pieces_as_cells) { piece_sprite_cells(p, sattr, hs, vs, 0); npieces++; return; }
    for (py = 0; py < p->h; py++) {
        int c;
        for (c = 0; c < hs; c++) {
            uint32_t attr = sattr;
            int row = (attr & 0x1000) ? p->h - 1 - py : py;
            int colx = (attr & 0x0800) ? (hs - 1 - c) : c;
            uint32_t tile = (attr & 0x7FF) + (uint32_t)(colx * vs + (row >> 3));
            uint8_t t8[8];
            tile_row(t8, ((attr & 0xF800) & ~0x1000u) | (tile & 0x7FF), row & 7);
            for (px = 0; px < 8; px++) p->px[py * p->w + c * 8 + px] = (t8[px] & PIX_OPAQUE) ? t8[px] : 0;
        }
    }
    npieces++;
}

/* Things on the floor that the game turns into objects only near its screen
 * (LOC_00015118, CheckForTelephone): presents, food and other items, trees and
 * the like, dropped presents, telephones. 8-byte records: +0 type, +1 floor,
 * +2 object (0 = none), +4 x, +6 y. */
#define LEVEL_ITEMS         0xFFDAE2u   /* 2 x 32 records (one floor per player) */
#define DROPPED_PRESENTS    0xFFDCE6u   /* 32 records */
#define TELEPHONES          0xFFE24Eu   /* 2 records */
#define PRESENT_IDENTITIES  0xFFDA8Au

static uint32_t item_frame(int type)
{
    uint32_t list;
    if (type < 0x40) list = 0x0AAF34u + (uint32_t)ob8(PRESENT_IDENTITIES + (uint32_t)type * 2) * 4;
    else if (type < 0x51) list = 0x0AB616u + (uint32_t)(type - 0x40) * 4;
    else list = 0x0AF02Cu + (uint32_t)(type - 0x51) * 4;
    return ob32(list);
}

static int pic_w, pic_h;   /* (defined with the picture, below) */
static struct { int cls, x, y, size; uint32_t attr; } sh[NUM_OBJECTS + 100];
static int nsh;

/* Everything to be drawn from frame data or by its hardware sprites, put in the
 * game's drawing order before the pieces are made (LOC_0000C690: objects sorted
 * by Y, lower on the screen in front, those with $2F bit 7 behind all others;
 * the objects of the second list, $FFD93C, after those). */
typedef struct {
    uint32_t o, fr;          /* object (0: a floor item from its record), frame */
    int x, y, z, attr;       /* original-screen position, altitude, attribute */
    int group, world_y, seq; /* sort key */
    int item;                /* item record (tests), or -1 */
    int shadow;              /* shadow class, 0 none */
    int sat;                 /* drawn by its own hardware sprites (rank only) */
} Draw;
static Draw draws[NUM_OBJECTS + 100];
static int ndraws;


/* Where the game itself puts an object on its screen when it writes the sprite
 * table (LOC_0000C222: world minus its NEW camera; attached objects already
 * moved by LOC_0000C150): to turn the table's positions into offsets. */
static int game_screen_x(uint32_t o)
{
    return (int16_t)ob16(o) - (int16_t)ob16(cur_cam ? 0xFF803Au : 0xFF8036u);
}
static int game_screen_y(uint32_t o)
{
    return (int16_t)ob16(o + 2) - (int16_t)ob16(cur_cam ? 0xFF803Cu : 0xFF8038u);
}

static void queue_draw(uint32_t o, uint32_t fr, int x, int y, int z, int attr, int group, int world_y, int item,
                       int shadow)
{
    Draw *d;
    if (ndraws >= (int)(sizeof draws / sizeof draws[0])) return;
    d = &draws[ndraws];
    d->o = o; d->fr = fr; d->x = x; d->y = y; d->z = z; d->attr = attr;
    d->group = group; d->world_y = world_y; d->seq = ndraws; d->item = item; d->shadow = shadow; d->sat = 0;
    ndraws++;
}

static int draw_cmp(const void *a, const void *b)
{
    const Draw *p = (const Draw *)a, *q = (const Draw *)b;
    if (p->group != q->group) return p->group - q->group;
    if (p->world_y != q->world_y) return q->world_y - p->world_y;     /* lower on screen: in front */
    return p->seq - q->seq;
}

/* Decode the pieces of everything that would show in the picture (original-screen
 * area x0..x1, y0..y1) without hardware sprites of its own: objects off the
 * original screen, objects partly where hardware sprites cannot go, items. */
static uint8_t in_main_list[NUM_OBJECTS];

static int nregions;   /* 2: split screen (the regions are set up below) */

/* A player blinking (after being hit: $4D bit 4): every VBlank the game hides one
 * player's sprites in the VDP's sprite table (LOC_0000D132, alternating players),
 * until the next upload of the table brings them back. Is this player hidden
 * now? (Read from the sprite table the VDP has, where the game writes it.) */
static int player_blinked_out(uint32_t o)
{
    uint32_t base = (uint32_t)(vdp_reg[5] & 0x7E) << 9;
    int idx = (int)((o - OBJECTS) / OBJECT_SIZE), slot;
    if (idx > 1 || !(rd8(o + 0x4D) & 0x10)) return 0;   /* (as LOC_0000D132 sees it, at VBlank) */
    slot = ob8(o + ((ob8(o + 0x2F) & 2) ? 0x30u : 0x3Cu)) % 80;
    if (!slot) return 0;
    return ((((uint32_t)vdp_vram[(base + (uint32_t)slot * 8) & 0xFFFF] << 8) | vdp_vram[(base + (uint32_t)slot * 8 + 1) & 0xFFFF]) & 0x3FF) == 1;
}

void gameview_sat_dma(void)
{
    if (g_gv_sync_log) fprintf(g_gv_sync_log, "satdma at line %d\n", vdp_current_line());
    memcpy(ram_snap, g_ram, sizeof ram_snap);
    snap_age = 0;
}

static void pieces_row(uint8_t *buf, int off, int x_from, int x_to, int y)
{
    int i, x;
    /* (tests at the original size: from the status bar's line the game unlinks
     * the objects' hardware sprites, LOC_0000CFEE at its H-interrupt; do the same
     * to compare) */

    for (i = 0; i < npieces; i++) {
        const Piece *p = &pieces[i];
        const uint8_t *src;
        if (y < p->y || y >= p->y + p->h || y < p->cy0 || y >= p->cy1) continue;
        src = p->px + (y - p->y) * p->w;
        for (x = 0; x < p->w; x++) {
            int sx = p->x + x;
            /* (rows of the present list: the game cuts the other sprites there) */
            if (y >= 0 && y < view_top_window && sx >= 0 && sx < 320 && !p->ui) continue;
            /* (in front of a hardware sprite of an object further back) */
            if (sx >= x_from && sx < x_to && (src[x] & PIX_OPAQUE) &&
                (!(buf[sx + off] & PIX_OPAQUE) || p->rank < row_rank[sx + off])) {
                buf[sx + off] = src[x];
                row_rank[sx + off] = (int16_t)p->rank;
            }
        }
    }
}

/* ---- the picture ------------------------------------------------------------- */

static uint32_t picture[GAMEVIEW_MAX_W * GAMEVIEW_MAX_H];
static int hud_end;   /* the line after the status bar's last line (bottom window) */
/* Set while a screen with a repeating background is up: 1 = plane B is the
 * background (SEGA screen), 2 = plane A is (title screen). */
int g_gameview_extend_screen;
static int last_ok, last_w, last_h;
static int display_on_at_top;   /* the display was on at the top of this frame */
int g_gv_win_top_seen;          /* (tests) the window was at the top on some line of this frame */
int g_gv_kept;                  /* (tests) this frame repeats the last picture (clean transitions) */
static void render_views_picture(void);
/* the gameplay view as a list for the GPU (when the platform draws lists) */
static ViewList vlist;
static ViewList wlist;            /* (network play: this machine's window, apart from the screens' lists) */
static ViewList *shown_list = &vlist;   /* the list last given to present */
static ViewList sl_other;          /* (screens: built in whichever of vlist and this isn't shown, so a
                                    * frame that can't be one keeps the shown one whole) */
static ViewList *sl_into = &vlist;
static void set_list(ViewList *vl) { shown_list = vl; present_set_list(vl); }
static int list_ready;        /* this frame's picture is the list */
static int last_was_list;
extern int g_in_game;   /* the last picture made here, kept while the VDP is frozen */
static int window_row_filler(int row);
static uint8_t band_row[GAMEVIEW_MAX_H];  /* picture rows holding the status bar / present list */
static int pic_w = 320, pic_h = VDP_HEIGHT;
static int want_w = 320, want_h = VDP_HEIGHT;
static int off_x, off_y;                 /* where the original screen's (0, 0) is in the picture */
static int frame_ok;                     /* every line so far drawn here */
int g_gameview_reason;                   /* why the last frame was not drawn here (tests) */
static Region region[2];
static int win_from, win_to;   /* rows covered by the window plane at line 0 */

void gameview_set_size(int w, int h)
{
    want_w = w < 2 ? 2 : w > GAMEVIEW_MAX_W ? GAMEVIEW_MAX_W : w & ~1;
    want_h = h < 2 ? 2 : h > GAMEVIEW_MAX_H ? GAMEVIEW_MAX_H : h & ~1;
}

/* At line 0: is the screen showing gameplay, and where are the cameras? */
static int frame_was_gameplay;   /* the last frame was gameplay */
static int layout_switched;      /* the floor layout became another floor's while plane A still shows this one */
static int frame_setup_checks(void);
static int half_held[2];     /* split: this half's plane A no longer shows its floor (falling) */
static int split_blank;      /* split: the whole screen is the window (the split being set up) */
static int views_wide(void);

static int frame_setup(void)
{
    int ok = frame_setup_checks();
    frame_was_gameplay = ok;
    return ok;
}

static int frame_setup_checks(void)
{
    if (!vdp_live_picture()) { g_gameview_reason = 1; return 0; }
    if (vdp_width() != 320) { g_gameview_reason = 2; return 0; }
    /* The window plane covers whole rows: the status bar from a row down to the
     * bottom, or the present list from the top down to a row (with the status bar
     * then at the top of it). Anything else is not gameplay (the map). */
    if (vdp_reg[17] & 0x1F) { g_gameview_reason = 3; return 0; }
    {
        int wv = (vdp_reg[18] & 0x1F) * 8;
        if (wv > VDP_HEIGHT) wv = VDP_HEIGHT;
        if (vdp_reg[18] & 0x80) { win_from = wv; win_to = VDP_HEIGHT; }
        else { win_from = 0; win_to = wv; }
    }
    split_blank = 0;
    if (win_from == 0 && win_to >= VDP_HEIGHT) {
        /* (in split play with the per-player views: the split being set up, the
         * halves blank but for the players; each half shown as the VDP draws it) */
        extern int g_in_game;
        if (!(g_in_game && rd8(SPLIT_SCREEN) && views_wide())) { g_gameview_reason = 4; return 0; }
        split_blank = 1;
    }
    /* only while the game is being played: the main game loop has started, and
     * the status bar is up (window rows), or the screen is split, or this is the
     * last floor (no status bar there). Cutscenes in space can otherwise look like
     * the space around floor 1. */
    {
        extern int g_in_game;
        if (!g_in_game ||
            (win_from == win_to && !rd8(SPLIT_SCREEN) && rd8(TOEJAMS_LEVEL) != FINAL_LEVEL)) {
            g_gameview_reason = 9;
            return 0;
        }
    }

    {
        int i;
        for (i = 0; i < (int)sizeof layout_copy[0]; i++) {
            layout_copy[0][i] = (uint8_t)rd8(TOEJAM_LEVEL_TILES + (uint32_t)i);
            layout_copy[1][i] = (uint8_t)rd8(EARL_LEVEL_TILES + (uint32_t)i);
        }
    }
    region[0].cam_x = (int16_t)rd16(TOEJAM_CAMERA_XPOS);
    region[0].cam_y = (int16_t)rd16(TOEJAM_CAMERA_YPOS);
    g_gv_cam_x = region[0].cam_x;
    g_gv_cam_y = region[0].cam_y;
    region[0].tiles = TOEJAM_LEVEL_TILES;
    region[0].level = rd8(TOEJAMS_LEVEL);
    region[0].top = 0;
    if (rd8(SPLIT_SCREEN)) {
        region[0].bottom = VDP_HEIGHT / 2;
        region[1].top = VDP_HEIGHT / 2;
        region[1].bottom = VDP_HEIGHT;
        region[1].cam_x = (int16_t)rd16(EARL_CAMERA_XPOS);
        region[1].cam_y = (int16_t)rd16(EARL_CAMERA_YPOS);
        region[1].tiles = EARL_LEVEL_TILES;
        region[1].level = rd8(EARLS_LEVEL);
        nregions = 2;
        /* (wider: drawn as two per-player views at the end of the frame) */
        pic_w = want_w;
        pic_h = want_h;
    } else {
        region[0].bottom = VDP_HEIGHT;
        nregions = 1;
        pic_w = want_w;
        pic_h = want_h;
    }
    /* Does plane A show this floor? (Tells gameplay from screens that only look
     * like it.) While the game is changing the floor (ground falling away, a hole
     * opening) the layout runs ahead of plane A for a few frames: if gameplay was
     * on screen just before, from about here, it still is. */
    {
        static int grace, last_x, last_y, last_level;
        int same_place = frame_was_gameplay && region[0].level == last_level &&
                         abs(region[0].cam_x - last_x) < 32 && abs(region[0].cam_y - last_y) < 32;
        last_x = region[0].cam_x;
        last_y = region[0].cam_y;
        last_level = region[0].level;
        /* How much of the floor layout changed since the last frame? A few blocks:
         * ground caving in. Many: the layout of another floor already (falling),
         * while the screen still shows this one. */
        static uint8_t prev_layout[19 * 26 * 2];
        static int have_prev;
        int changed = 0, i;
        for (i = 0; i < (int)sizeof prev_layout; i += 2)
            if (rd16(region[0].tiles + (uint32_t)i) != (uint32_t)((prev_layout[i] << 8) | prev_layout[i + 1])) changed++;
        for (i = 0; i < (int)sizeof prev_layout; i++) prev_layout[i] = (uint8_t)rd8(region[0].tiles + (uint32_t)i);
        if (have_prev && changed > 24) layout_switched = 1;
        have_prev = 1;
        if (nregions == 1 && !plane_shows_level(&region[0], win_from, win_to)) {
            /* (a few blocks changed, not a different floor altogether) */
            if (!same_place || plane_match < 60 || layout_switched || ++grace > 60) { g_gameview_reason = 5; return 0; }
        } else {
            grace = 0;
            layout_switched = 0;
        }
    }
    /* the status bar's last row: filler rows under it show the level instead */
    hud_end = VDP_HEIGHT;
    if (win_to == VDP_HEIGHT && win_from > 0 && (pic_w != 320 || pic_h != VDP_HEIGHT))
        while (hud_end > win_from && window_row_filler((hud_end - 1) / 8)) hud_end -= 8;
    off_x = (pic_w - 320) / 2;
    off_y = (pic_h - VDP_HEIGHT) / 2;
    memset(band_row, 0, sizeof band_row);
    g_gameview_reason = 0;
    return 1;
}

/* Is window row (cells) only a filler: one cell repeated across (the black row
 * under the status bar)? */
static int window_row_filler(int row)
{
    uint32_t base = (uint32_t)(vdp_reg[3] & 0x3C) << 10;
    uint32_t first = 0;
    int c;
    for (c = 0; c < 40; c++) {
        uint32_t ea = (base + (uint32_t)(row * 64 + c) * 2) & 0xFFFF;
        uint32_t v = ((uint32_t)vdp_vram[ea] << 8) | vdp_vram[ea + 1];
        if (c == 0) first = v;
        else if (v != first) return 0;
    }
    /* (and that cell one solid colour: the purple frame of the present list is
     * one cell repeated as well, but it is part of the box) */
    {
        uint32_t t = (first & 0x7FF) * 32;
        uint8_t px0 = vdp_vram[t & 0xFFFF] >> 4;
        int k;
        for (k = 0; k < 32; k++) {
            uint8_t b = vdp_vram[(t + (uint32_t)k) & 0xFFFF];
            if ((b >> 4) != px0 || (b & 15) != px0) return 0;
        }
    }
    return 1;
}


/* A window line (status bar, present list) as the VDP drew it, kept at the top
 * or bottom edge of the picture, centred. */
static int holding;   /* frame_ok 4: the outside kept from the last gameplay frame */
/* The present list's lines as the VDP drew them this frame: the game reuses
 * sprite-table entries down the list (its icons), so they are only right line by
 * line. */
static uint32_t top_window_lines[VDP_HEIGHT][320];
/* In split screen a player's menu (present list, shops...) is the window plane over
 * that player's half: how many lines of each half were window this frame. */
static int half_window_lines[2];
static uint8_t line_window[VDP_HEIGHT];   /* each line this frame: 0 not window, 1 window from the top, 2 from the bottom */
int g_gv_windows;                          /* each player in a window of their own (gameview_window_frame) */
static int views_done;                     /* the views' picture is drawn for this frame */
static int view_skip_half_hud;   /* the view being drawn is under its player's menu */
int g_gv_split_menu;              /* (tests) halves under a menu in the last split picture: bit 0 top, bit 1 bottom */
static int split_half_covered(int k) { return half_window_lines[k] >= VDP_HEIGHT / 2 - 16; }
int gameview_split_half_shown_as_vdp(int k) { return split_half_covered(k) || half_held[k] || split_blank; }
static int after_gameplay;   /* gameplay was on screen, and the display has not been off since */

static void draw_band(int line)
{
    /* the status bar at the bottom edge; the present list (window from the top)
     * where the original has it: sprites the game half covers with it (the hint
     * sign) stay next to it */
    int r = line < VDP_HEIGHT / 2 ? line + off_y : pic_h - hud_end + line;
    const uint32_t *src = vdp_framebuffer() + (size_t)line * VDP_FB_PITCH;
    uint32_t *o;
    int x;
    if (r < 0 || r >= pic_h) return;
    o = picture + (size_t)r * GAMEVIEW_MAX_W;
    band_row[r] = 1;
    for (x = 0; x < 320; x++) {
        int px = x + off_x;
        if (px >= 0 && px < pic_w) o[px] = src[x];
    }
}

/* A screen other than gameplay whose background (plane B) repeats, drawn wider:
 * plane B everywhere, the rest (plane A, window) only where the original screen
 * is, sprites wherever they are. */
static void draw_screen_row(int r, int y)
{
    static uint8_t a[GAMEVIEW_MAX_W], b[GAMEVIEW_MAX_W], sp[GAMEVIEW_MAX_W];
    int x0 = -off_x, x1 = pic_w - off_x;
    uint32_t *o = picture + (size_t)r * GAMEVIEW_MAX_W, backdrop;
    int x;
    if (r < 0 || r >= pic_h) return;
    memset(a, 0, (size_t)pic_w);
    memset(sp, 0, (size_t)pic_w);
    /* the background plane everywhere; the other only where the original screen is */
    if (g_gameview_extend_screen == 2) {
        memset(b, 0, (size_t)pic_w);
        plane_row(a, off_x, x0, x1, y, 0);
        if (y >= 0 && y < VDP_HEIGHT) plane_row(b, off_x, x0 < 0 ? 0 : x0, x1 > 320 ? 320 : x1, y, 1);
    } else {
        plane_row(b, off_x, x0, x1, y, 1);
        if (y >= 0 && y < VDP_HEIGHT) plane_row(a, off_x, x0 < 0 ? 0 : x0, x1 > 320 ? 320 : x1, y, 0);
    }
    sprite_row(sp, off_x, x0, x1, y, 3);
    backdrop = vdp_palette_rgb(vdp_reg[7] & 0x3F);
    for (x = 0; x < pic_w; x++) {
        uint8_t s = sp[x], pa = a[x], pb = b[x], c;
        if ((s & (PIX_OPAQUE | PIX_PRIO)) == (PIX_OPAQUE | PIX_PRIO)) c = s;
        else if ((pa & (PIX_OPAQUE | PIX_PRIO)) == (PIX_OPAQUE | PIX_PRIO)) c = pa;
        else if ((pb & (PIX_OPAQUE | PIX_PRIO)) == (PIX_OPAQUE | PIX_PRIO)) c = pb;
        else if (s & PIX_OPAQUE) c = s;
        else if (pa & PIX_OPAQUE) c = pa;
        else if (pb & PIX_OPAQUE) c = pb;
        else { o[x] = backdrop; continue; }
        o[x] = vdp_palette_rgb(c & 0x3F);
    }
}

/* ---- other screens, wider -----------------------------------------------------
 * Any screen that is not gameplay (cutscenes, menus, map, ride between floors)
 * drawn wider. The background layers (planes and window, no sprites) of the
 * frame are collected line by line; at the end of the frame:
 *  - a border ring (rows/columns at the screen edge whose tiles do not occur
 *    further in) moves out to the edges of the picture; along the new stretches
 *    its tiles are picked from elsewhere on the border, staggered, so it does
 *    not look repeated;
 *  - the space between is filled with the background next to the border,
 *    repeated with its own period when it has one;
 *  - the inside of the original screen (with its sprites) stays as it was.
 * Things the original kept just off its screen are never shown. */

static uint32_t bg_pic[VDP_HEIGHT][320];

/* The menus' starry sky (their plane B, 512x256, repeating), kept to go around
 * the map during play. */
static uint32_t star_pic[256][512];
static int have_stars, map_mode;

void gameview_capture_stars(void)
{
    uint32_t base = (uint32_t)(vdp_reg[4] & 7) << 13, backdrop = vdp_palette_rgb(vdp_reg[7] & 0x3F);
    int ty, tx, y, x;
    if ((vdp_reg[16] & 0x33) != 0x01) return;          /* (expects 64 x 32 planes) */
    for (ty = 0; ty < 32; ty++)
        for (tx = 0; tx < 64; tx++) {
            uint32_t ea = (base + (uint32_t)(ty * 64 + tx) * 2) & 0xFFFF;
            uint32_t e = ((uint32_t)vdp_vram[ea] << 8) | vdp_vram[ea + 1];
            for (y = 0; y < 8; y++) {
                uint8_t px[8];
                tile_row(px, e, y);
                for (x = 0; x < 8; x++)
                    star_pic[ty * 8 + y][tx * 8 + x] = (px[x] & PIX_OPAQUE) ? vdp_palette_rgb(px[x] & 0x3F) : backdrop;
            }
        }
    have_stars = 1;
}
static uint32_t plane_pic[2][VDP_HEIGHT][320];   /* each plane alone (window in A), backdrop where clear */

static void bg_line(int line)
{
    static uint8_t a[GAMEVIEW_MAX_W], b[GAMEVIEW_MAX_W];
    uint32_t backdrop = vdp_palette_rgb(vdp_reg[7] & 0x3F);
    int x, wfrom = 0, wto = 0;
    {
        int wv = (vdp_reg[18] & 0x1F) * 8, down = vdp_reg[18] & 0x80;
        int wh = (vdp_reg[17] & 0x1F) * 16, right = vdp_reg[17] & 0x80;
        int in_v = down ? (line >= wv) : (line < wv);
        if (in_v) { wfrom = 0; wto = 320; }
        else if (wh) {
            if (right) { wfrom = wh; wto = 320; } else { wfrom = 0; wto = wh; }
            if (wto > 320) wto = 320;
        }
    }
    plane_row(b, 0, 0, 320, line, 1);
    plane_row(a, 0, 0, 320, line, 0);
    if (wto > wfrom) {
        uint32_t base = (uint32_t)(vdp_reg[3] & 0x3C) << 10;
        int col;
        for (col = wfrom / 8; col < wto / 8; col++) {
            uint32_t ea = (base + (uint32_t)((line >> 3) * 64 + col) * 2) & 0xFFFF;
            tile_row(a + col * 8, ((uint32_t)vdp_vram[ea] << 8) | vdp_vram[ea + 1], line & 7);
        }
    }
    for (x = 0; x < 320; x++) {
        uint8_t pa = a[x], pb = b[x], c;
        plane_pic[0][line][x] = (pa & PIX_OPAQUE) ? vdp_palette_rgb(pa & 0x3F) : backdrop;
        plane_pic[1][line][x] = (pb & PIX_OPAQUE) ? vdp_palette_rgb(pb & 0x3F) : backdrop;
        if ((pa & (PIX_OPAQUE | PIX_PRIO)) == (PIX_OPAQUE | PIX_PRIO)) c = pa;
        else if ((pb & (PIX_OPAQUE | PIX_PRIO)) == (PIX_OPAQUE | PIX_PRIO)) c = pb;
        else if (pa & PIX_OPAQUE) c = pa;
        else if (pb & PIX_OPAQUE) c = pb;
        else { bg_pic[line][x] = backdrop; continue; }
        bg_pic[line][x] = vdp_palette_rgb(c & 0x3F);
    }
}

static uint32_t (*hash_src)[320] = bg_pic;

static uint32_t block_hash(int bx, int by)
{
    uint32_t h = 2166136261u;
    int x, y;
    for (y = 0; y < 8; y++)
        for (x = 0; x < 8; x++) h = (h ^ hash_src[by * 8 + y][bx * 8 + x]) * 16777619u;
    return h;
}

static void hash_blocks(uint32_t (*src)[320]);

#define BW 40
#define BH 28
static uint32_t blocks[BH][BW];

static int ring_of(int bx, int by)
{
    int d = bx < by ? bx : by;
    if (BW - 1 - bx < d) d = BW - 1 - bx;
    if (BH - 1 - by < d) d = BH - 1 - by;
    return d;
}

/* Border thickness in blocks (0-3): the outer rings whose blocks do not occur in
 * the two rings inside them. */
static int find_border(void)
{
    int b;
    for (b = 3; b >= 1; b--) {
        int inner = 0, shared = 0, bx, by, ox, oy;
        for (by = 0; by < BH; by++)
            for (bx = 0; bx < BW; bx++) {
                int d = ring_of(bx, by);
                if (d < b || d >= b + 2) continue;
                inner++;
                for (oy = 0; oy < BH; oy++) {
                    for (ox = 0; ox < BW; ox++)
                        if (ring_of(ox, oy) < b && blocks[oy][ox] == blocks[by][bx]) { shared++; goto next; }
                }
            next:;
            }
        /* the ring itself must not be one plain colour like the inside */
        if (inner && shared * 10 <= inner) {
            int bxx = 0, diff = 0;
            for (bxx = 1; bxx < BW; bxx++) if (blocks[0][bxx] != blocks[0][0]) diff = 1;
            if (diff || blocks[0][0] != blocks[b][b]) return b;
        }
    }
    return 0;
}

/* Period (in blocks) of the picture along x (axis 0) or y (axis 1), measured in
 * the outer band near the edges (where cutscene panels and text do not reach):
 * the smallest p for which the band repeats every p blocks; 0 if none. */
static int find_period(int axis)
{
    int p;
    int len = axis ? BH : BW, other = axis ? BW : BH;
    for (p = 1; p <= 16 && 2 * p <= len; p++) {
        int same = 0, total = 0, i, j;
        for (i = 0; i + p < len; i++)
            for (j = 0; j < other; j++) {
                uint32_t h1, h2;
                if (j >= 4 && j < other - 4) continue;          /* only the outer band */
                h1 = axis ? blocks[i][j] : blocks[j][i];
                h2 = axis ? blocks[i + p][j] : blocks[j][i + p];
                total++;
                if (h1 == h2) same++;
            }
        if (total && same * 100 >= total * 98) return p;
    }
    return 0;
}

static uint32_t mix(uint32_t v)
{
    v ^= v >> 16; v *= 0x7FEB352Du; v ^= v >> 15; v *= 0x846CA68Bu; v ^= v >> 16;
    return v;
}

static int floor_div(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

static void hash_blocks(uint32_t (*src)[320])
{
    int bx, by;
    hash_src = src;
    for (by = 0; by < BH; by++)
        for (bx = 0; bx < BW; bx++) blocks[by][bx] = block_hash(bx, by);
}

/* Returns 0 if the screen has neither a border nor a repeating pattern: it is
 * then shown at its original size. */
/* (the picture around the original screen is kept from the frame before while
 * the screen's background is the same - the inside is copied each frame; on a
 * PSP working it all out again is several frames' worth of time) */
static int ext_valid;            /* the picture's outside is that of ext_sig */
static uint32_t ext_sig;

static void copy_inside(void)
{
    const uint32_t *vfb = vdp_framebuffer();
    int y, x0 = off_x < 0 ? -off_x : 0, x1 = 320, n;
    if (x1 + off_x > pic_w) x1 = pic_w - off_x;
    n = x1 - x0;
    if (n <= 0) return;
    for (y = 0; y < VDP_HEIGHT; y++) {
        int r = y + off_y;
        if (r < 0 || r >= pic_h) continue;
        memcpy(picture + (size_t)r * GAMEVIEW_MAX_W + x0 + off_x, vfb + (size_t)y * VDP_FB_PITCH + x0, sizeof(uint32_t) * (size_t)n);
    }
}

static int build_extended_screen(void)
{
    const uint32_t *vfb = vdp_framebuffer();
    uint32_t (*fill)[320] = bg_pic;
    int b, px = 0, py = 0, x, y, pl;
    uint32_t sig;
    if (map_mode) {
        sig = 0x4D415055u ^ (uint32_t)pic_w * 31u ^ (uint32_t)pic_h * 7919u;
        if (ext_valid && ext_sig == sig) { copy_inside(); return 1; }
        /* the map during play: the menus' starry sky around it */
        for (y = 0; y < pic_h; y++) {
            int ry = y - off_y;
            uint32_t *o = picture + (size_t)y * GAMEVIEW_MAX_W;
            for (x = 0; x < pic_w; x++) {
                int rx = x - off_x;
                if (rx >= 0 && rx < 320 && ry >= 0 && ry < VDP_HEIGHT) o[x] = vfb[(size_t)ry * VDP_FB_PITCH + (size_t)rx];
                else o[x] = star_pic[ry & 255][rx & 511];
            }
        }
        ext_valid = 1;
        ext_sig = sig;
        return 1;
    }
    hash_blocks(bg_pic);
    {
        int bx, by;
        sig = 2166136261u ^ (uint32_t)pic_w * 31u ^ (uint32_t)pic_h * 7919u;
        for (by = 0; by < BH; by++)
            for (bx = 0; bx < BW; bx++) sig = (sig ^ blocks[by][bx]) * 16777619u;
        if (ext_valid && ext_sig == sig) { copy_inside(); return 1; }
    }
    b = find_border();
    if (!b) {
        /* a repeating pattern: the whole picture, or one plane behind things on the
         * other (the title screen) */
        px = find_period(0);
        py = find_period(1);
        for (pl = 0; pl < 2 && !(px && py); pl++) {
            hash_blocks(plane_pic[pl]);
            px = find_period(0);
            py = find_period(1);
            if (px && py) fill = plane_pic[pl];
        }
        if (!(px && py)) { ext_valid = 0; return 0; }
    }
    ext_valid = 1;
    ext_sig = sig;
    for (y = 0; y < pic_h; y++) {
        int ry = y - off_y;
        uint32_t *o = picture + (size_t)y * GAMEVIEW_MAX_W;
        for (x = 0; x < pic_w; x++) {
            int rx = x - off_x;
            if (rx >= 0 && rx < 320 && ry >= 0 && ry < VDP_HEIGHT) {
                o[x] = vfb[(size_t)ry * VDP_FB_PITCH + (size_t)rx];   /* the original, as it is */
                continue;
            }
            if (b) {
                /* the border carried on outwards: its tiles, picked from along its
                 * top and bottom stretch, staggered so they do not look repeated */
                int bx = floor_div(rx, 8), by = floor_div(ry, 8), span = BW - 2 * b;
                uint32_t h = mix((uint32_t)bx * 2654435761u ^ (uint32_t)by * 2246822519u);
                int sbx = b + (int)(h % (uint32_t)span), sby = (int)((h >> 16) % (uint32_t)b);
                if ((h >> 8) & 1) sby = BH - 1 - sby;
                o[x] = bg_pic[sby * 8 + (ry - by * 8)][sbx * 8 + (rx - bx * 8)];
            } else {
                int sx = rx % (px * 8), sy = ry % (py * 8);
                if (sx < 0) sx += px * 8;
                if (sy < 0) sy += py * 8;
                o[x] = fill[sy][sx];
            }
        }
    }
    return 1;
}

static void hold_line(int line)
{
    int r = line + off_y, x, wv, in_window;
    if (line < 0 || line >= VDP_HEIGHT) return;
    if (!vdp_live_picture() || vdp_width() != 320 || (vdp_reg[17] & 0x1F)) { frame_ok = 0; holding = 0; return; }
    wv = (vdp_reg[18] & 0x1F) * 8;
    in_window = (vdp_reg[18] & 0x80) ? line >= wv : line < wv;
    if (in_window && window_row_filler(line / 8)) return;
    if (in_window) { draw_band(line); return; }
    if (r >= 0 && r < pic_h && !band_row[r]) {
        const uint32_t *src = vdp_framebuffer() + (size_t)line * VDP_FB_PITCH;
        uint32_t *o = picture + (size_t)r * GAMEVIEW_MAX_W;
        for (x = 0; x < 320; x++)
            if (x + off_x >= 0 && x + off_x < pic_w) o[x + off_x] = src[x];
    }
}

/* (screens other than gameplay as lists: this frame is one; frames before trying
 * again after one that couldn't be) */
static int screen_list_frame, screen_list_off;
static int sl_stale;   /* (the last build failed only for that: the picture before is kept) */
static const char *sl_why = "";   /* (why the last couldn't be a list) */
extern int g_gv_use_list;
static int vl_check_on(void);

static void gameview_line_frame(int line);
static int wants_vdp_line_(int line);
void gameview_line(int line)
{
    gameview_line_frame(line);
    if (line == 0) {
        extern int g_gv_windows;
        if (screen_list_off) screen_list_off--;
        screen_list_frame = (g_gv_use_list || vl_check_on()) && !screen_list_off &&
                            (frame_ok == 0 || frame_ok == 2 || frame_ok == 3) && vdp_live_picture() && vdp_width() == 320;
    }
}

static void gameview_line_frame(int line)
{
    const Region *r;
    int y, in_window, wv;
    if (frame_ok == 4 && line != 0) {                 /* gameplay, the outside held */
        hold_line(line);
        return;
    }
    if (frame_ok == 3 && line != 0) {                 /* another screen, wider */
        if (line < 0 || line >= VDP_HEIGHT) return;
        if (screen_list_frame && !vl_check_on()) return;   /* (a list: drawn at the end of the frame) */
        if (!vdp_live_picture() || vdp_width() != 320) { frame_ok = 0; return; }
        bg_line(line);
        if (line == VDP_HEIGHT - 1 && !build_extended_screen()) frame_ok = 0;
        return;
    }
    if (frame_ok == 2 && line != 0) {                 /* a wider screen with a repeating background */
        if (line < 0 || line >= VDP_HEIGHT) return;
        if (screen_list_frame && !vl_check_on()) return;
        if (!vdp_live_picture() || (vdp_reg[17] & 0x1F) || (vdp_reg[18] & 0x1F)) {
            frame_ok = 0;
            return;
        }
        memset(sat_suppressed, 0, sizeof sat_suppressed);
        draw_screen_row(line + off_y, line);
        if (line == VDP_HEIGHT - 1)
            for (y = VDP_HEIGHT; y < pic_h - off_y; y++) draw_screen_row(y + off_y, y);
        return;
    }
    if (line == 0) {
        int w, h, prev_ext = frame_ok == 3;
        display_on_at_top = (vdp_reg[1] & 0x40) != 0;
        g_gv_win_top_seen = 0;
        half_window_lines[0] = half_window_lines[1] = 0;
        memset(line_window, 0, sizeof line_window);
        views_done = 0;
        list_ready = 0;
        half_held[0] = half_held[1] = 0;
        if (snap_age < 1000) snap_age++;
        g_gv_snap_age = snap_age;
        present_wanted_view(&w, &h);
        gameview_set_size(w, h);
        {
            static int held, held_x, held_y, prev_ok;
            int was = prev_ok;
            frame_ok = frame_setup();
            holding = 0;
            /* Gameplay goes on, but plane A shows something the floor layout no
             * longer does (falling to the floor below: the floor number and layout
             * change before the screen does). The camera stays put meanwhile: keep
             * the picture around the original screen as it was, and the original
             * screen as the VDP draws it. */
            if (frame_ok) { held = 0; held_x = region[0].cam_x; held_y = region[0].cam_y; }
            else if (g_gameview_reason == 5 && (was == 1 || was == 4) && held < 600 &&
                     region[0].cam_x == held_x && region[0].cam_y == held_y &&
                     pic_w == last_w && pic_h == last_h && (pic_w != 320 || pic_h != VDP_HEIGHT)) {
                frame_ok = 4;
                holding = 1;
                held++;
            }
            prev_ok = frame_ok;
            /* straight after gameplay, until the display is next switched off: the
             * screen is gameplay changing (splitting, a floor being set up), not a
             * screen of its own to be carried outwards */
            if (frame_ok == 1 || frame_ok == 4) after_gameplay = 1;
            else if (!vdp_live_picture()) after_gameplay = 0;
            if (frame_ok == 4) { hold_line(0); return; }
        }
        if (!frame_ok && g_gameview_extend_screen && vdp_live_picture() && vdp_width() == 320 &&
            !(vdp_reg[17] & 0x1F) && !(vdp_reg[18] & 0x1F) && (want_w != 320 || want_h != VDP_HEIGHT)) {
            pic_w = want_w;
            pic_h = want_h;
            off_x = (pic_w - 320) / 2;
            off_y = (pic_h - VDP_HEIGHT) / 2;
            npieces = 0;
            memset(sat_suppressed, 0, sizeof sat_suppressed);
            memset(object_sprite, 0, sizeof object_sprite);
            frame_ok = 2;
            for (y = -off_y; y < 0; y++) draw_screen_row(y + off_y, y);
            draw_screen_row(off_y, 0);
            return;
        }
        /* (the map, during play: its purple squares carried on outwards would look
         * like more of the floor still to uncover; the menus' starry sky goes
         * around it instead, or nothing if it has not been seen yet) */
        map_mode = g_in_game && (vdp_reg[17] & 0x1F);
        /* (a gameplay screen that is not quite gameplay yet, with the status bar
         * up: a floor being set up, the screen splitting. Its edges are the level,
         * not a border: it stays 4:3 for those frames) */
        if (!frame_ok && g_options.extend_screens && vdp_live_picture() && vdp_width() == 320 &&
            !(g_in_game && !map_mode && ((vdp_reg[18] & 0x1F) || after_gameplay)) &&
            (!map_mode || have_stars) && (want_w != 320 || want_h != VDP_HEIGHT)) {
            pic_w = want_w;
            pic_h = want_h;
            off_x = (pic_w - 320) / 2;
            off_y = (pic_h - VDP_HEIGHT) / 2;
            if (!prev_ext) ext_valid = 0;   /* (only kept between such frames) */
            frame_ok = 3;
            bg_line(0);
            return;
        }
        /* (a gameplay frame: drawn wider by the per-player views at the end of the
         * frame; at the original size the VDP's picture is the original) */
    }
    if (!frame_ok || line < 0 || line >= VDP_HEIGHT) return;
    if (!vdp_live_picture()) { frame_ok = 0; g_gameview_reason = 7; return; }
    if (vdp_reg[17] & 0x1F) { frame_ok = 0; g_gameview_reason = 3; return; }
    r = (nregions == 2 && line >= region[1].top) ? &region[1] : &region[0];
    /* At the last line of each half: does plane A (whichever the game points the
     * VDP at by then; split screen switches planes a line into the lower half)
     * show this floor here? */
    if (nregions == 2 && line == r->bottom - 1 && !split_blank && !split_half_covered(r == &region[1]) &&
        !plane_shows_level(r, win_from, win_to)) {
        /* (wider, with the views: that half is held, the VDP's half in the middle) */
        if (views_wide()) half_held[r == &region[1]] = 1;
        else {
        frame_ok = 0;
        g_gameview_reason = 5 + (r == &region[1]);
        return;
        }
    }
    /* window rows now (the game moves the window during the frame when the
     * present list opens and closes) */
    wv = (vdp_reg[18] & 0x1F) * 8;
    in_window = (vdp_reg[18] & 0x80) ? line >= wv : line < wv;
    if (!(vdp_reg[18] & 0x80) && wv) g_gv_win_top_seen = 1;
    if (in_window) half_window_lines[line >= VDP_HEIGHT / 2]++;
    if (line < VDP_HEIGHT) line_window[line] = (uint8_t)(in_window ? 1 + ((vdp_reg[18] & 0x80) != 0) : 0);
    if ((in_window || nregions == 2 || g_gv_windows) && line < VDP_HEIGHT) {
        /* (split and two windows: every line, for held halves, menus and the bar) */
        /* (two windows: only the lines the VDP draws - the others are not its
         * picture and nothing here reads them) */
        if ((nregions == 2 || g_gv_windows || !(vdp_reg[18] & 0x80)) && !(g_gv_windows && !wants_vdp_line_(line)))
            memcpy(top_window_lines[line], vdp_framebuffer() + (size_t)line * VDP_FB_PITCH, sizeof top_window_lines[0]);
    }
}

static void screen_list_check(void);
static int build_screen_frame(void);
static int frame_main(const uint32_t **pixels, int *width, int *height, int *pitch)
{
    g_gv_gameplay = 0;
    g_gv_kept = 0;
    if (screen_list_frame) {
        /* a screen other than gameplay, drawn from a list (once a frame) */
        static int ok;
        if (!views_done) {
            static int stale_run;
            sl_stale = 0;
            sl_into = shown_list == &vlist ? &sl_other : &vlist;
            PROF_PUSH(PROF_SLIST);
            ok = build_screen_frame();
            PROF_POP();
            if (!ok && sl_stale && ++stale_run <= 3 && last_ok && last_was_list) ok = 2;   /* (keep the last) */
            if (ok != 2) stale_run = 0;
            views_done = 1;
            if (vl_check_on() && ok == 1) screen_list_check();
            if (vl_check_on() && !ok) fprintf(stderr, "slcheck: frame %u: no list (%s, kind %d)\n", g_frame, sl_stale ? "stale" : sl_why, frame_ok);
        }
        if (g_gv_use_list) {
            if (ok == 2) {
                shown_list->frozen = 1;
                set_list(shown_list);
                *pixels = NULL;
                *width = last_w;
                *height = last_h;
                *pitch = GAMEVIEW_MAX_W;
                return 1;
            }
            if (ok) {
                last_ok = 1;
                last_was_list = 1;
                last_w = sl_into->w;
                last_h = sl_into->h;
                set_list(sl_into);
                *pixels = NULL;
                *width = sl_into->w;
                *height = sl_into->h;
                *pitch = GAMEVIEW_MAX_W;
                return 1;
            }
            /* (can't: the usual way for a while; this frame keeps the last picture) */
            screen_list_off = 60;
            printf("screen list: no (%s, kind %d)\n", sl_stale ? "stale" : sl_why, frame_ok);
            if (last_ok && last_was_list) {
                shown_list->frozen = 1;
                set_list(shown_list);
                *pixels = NULL;
                *width = last_w;
                *height = last_h;
                *pitch = GAMEVIEW_MAX_W;
                return 1;
            }
        }
    }
    if (!frame_ok) {
        /* while the VDP keeps its last picture (clean transitions), keep ours; and
         * keep it too for a frame the display was switched off part way down (the
         * top of it would be the old scene, the rest black: falling to the next
         * floor), when clean transitions are on */
        if (last_ok && (vdp_frozen() || vdp_lines_skipped ||
                        (g_options.clean_transitions && display_on_at_top && !(vdp_reg[1] & 0x40)))) {
            g_gv_kept = 1;
            *pixels = picture;
            if (last_was_list) {
                /* (the list again, with the GPU's copy of the tiles kept as it was:
                 * the game is rebuilding them behind the held picture) */
                shown_list->frozen = 1;
                set_list(shown_list);
                *pixels = NULL;
            }
            *width = last_w;
            *height = last_h;
            *pitch = GAMEVIEW_MAX_W;
            return 1;
        }
        last_ok = 0;
        return 0;
    }
    if (frame_ok == 2 || frame_ok == 3 || frame_ok == 4) {
        last_ok = 1;
        last_was_list = 0;
        last_w = pic_w;
        last_h = pic_h;
        *pixels = picture;
        *width = pic_w;
        *height = pic_h;
        *pitch = GAMEVIEW_MAX_W;
        return 1;
    }
    /* the cameras only change between frames (at VBlank); if not, something
     * restarted the machine during the frame (loading a game): don't trust it */
    if (region[0].cam_x != (int16_t)rd16(TOEJAM_CAMERA_XPOS) || region[0].cam_y != (int16_t)rd16(TOEJAM_CAMERA_YPOS) ||
        (nregions == 2) != (rd8(SPLIT_SCREEN) != 0)) {
        g_gameview_reason = 8;
        return 0;
    }
    g_gv_gameplay = 1;
    /* wider than the original: the per-player views (the lines drawn during the
     * frame are only kept for the original size, where they are checked against
     * the VDP) */
    /* (two windows: each window draws its own picture; this one is not shown) */
    if (pic_w == 320 && pic_h == VDP_HEIGHT && !views_done) {
        /* (the original size: the VDP's picture is the original one; kept here
         * too for the frames the picture is held) */
        int y;
        for (y = 0; y < VDP_HEIGHT; y++)
            memcpy(picture + (size_t)y * GAMEVIEW_MAX_W, vdp_framebuffer() + (size_t)y * VDP_FB_PITCH, sizeof(uint32_t) * 320);
        views_done = 1;
    }
    if ((pic_w != 320 || pic_h != VDP_HEIGHT) && !views_done && !g_gv_windows) {
        render_views_picture();
        views_done = 1;   /* (gameview_frame may be asked more than once a frame) */
    }
    last_ok = 1;
    last_w = pic_w;
    last_h = pic_h;
    last_was_list = list_ready;
    *pixels = picture;
    if (list_ready) {
        /* (the GPU draws the view from the list) */
        set_list(&vlist);
        *pixels = NULL;
    }
    *width = pic_w;
    *height = pic_h;
    *pitch = GAMEVIEW_MAX_W;
    return 1;
}

int g_gv_net_player = -1;
static char net_message[96];
static void draw_message(uint32_t *out, int w, int h, const char *text);
static void vl_overlay_clip(ViewList *vl, int x, int y, int w, int h, const uint32_t *px, int pitch);

void gameview_set_message(const char *text)
{
    snprintf(net_message, sizeof net_message, "%s", text ? text : "");
}

int gameview_frame(const uint32_t **pixels, int *width, int *height, int *pitch)
{
    int r = frame_main(pixels, width, height, pitch);
    if (g_gv_net_player >= 0) {
        /* network play: this machine's player's window */
        const uint32_t *px;
        int w, h, pp;
        present_wanted_view(&w, &h);
        if (gameview_window_frame(g_gv_net_player, &w, &h, &px, &pp)) {
            *pixels = px;
            *width = w;
            *height = h;
            *pitch = pp;
            r = 1;
        }
    }
    if (net_message[0] && r && !*pixels) {
        /* (the frame is a list: the box as one more overlay) */
        static uint32_t box[GAMEVIEW_MAX_W * GAMEVIEW_MAX_H];
        int w = *width, h = *height;
        draw_message(box, w, h, net_message);
        vl_overlay_clip(shown_list, (w - 240) / 2, (h - 56) / 2, 240, 56,
                        box + (size_t)((h - 56) / 2) * GAMEVIEW_MAX_W + (w - 240) / 2, GAMEVIEW_MAX_W);
    } else if (net_message[0]) {
        /* (over a copy: the pictures above are kept for the next frame) */
        static uint32_t copy[GAMEVIEW_MAX_W * GAMEVIEW_MAX_H];
        int y, w = r ? *width : vdp_width(), h = r ? *height : VDP_HEIGHT;
        const uint32_t *src = r ? *pixels : vdp_framebuffer();
        int sp = r ? *pitch : VDP_FB_PITCH;
        if (w > GAMEVIEW_MAX_W) w = GAMEVIEW_MAX_W;
        if (h > GAMEVIEW_MAX_H) h = GAMEVIEW_MAX_H;
        for (y = 0; y < h; y++) memcpy(copy + (size_t)y * GAMEVIEW_MAX_W, src + (size_t)y * sp, sizeof(uint32_t) * (size_t)w);
        draw_message(copy, w, h, net_message);
        *pixels = copy;
        *width = w;
        *height = h;
        *pitch = GAMEVIEW_MAX_W;
        r = 1;
    }
    return r;
}

/* ==== Per-player views ==========================================================
 * One player's view drawn at the end of a frame, at any size, around any camera,
 * from game memory only (the snapshot taken when the sprite table went to the VDP,
 * VRAM graphics and planes, the floor layout and the ROM): no use of the VDP's
 * lines. Split screen, two windows and network play draw one of these per
 * player. (docs/RENDERER.md, "Per-player views".) */

/* The game's own cameras this frame (as its planes were scrolled to). */
static struct { int on, cam_x, cam_y, h, level; } gcam[2];
int g_gv_view_strict;   /* (tests) draw only what the game shows on its cameras */

static void views_setup_cameras(void)
{
    int split = rd8(SPLIT_SCREEN) != 0;
    gcam[0].on = 1;
    gcam[0].cam_x = (int16_t)rd16(TOEJAM_CAMERA_XPOS);
    gcam[0].cam_y = (int16_t)rd16(TOEJAM_CAMERA_YPOS);
    gcam[0].h = split ? VDP_HEIGHT / 2 : VDP_HEIGHT;
    gcam[0].level = rd8(TOEJAMS_LEVEL);
    gcam[1].on = split;
    gcam[1].cam_x = (int16_t)rd16(EARL_CAMERA_XPOS);
    gcam[1].cam_y = (int16_t)rd16(EARL_CAMERA_YPOS);
    gcam[1].h = VDP_HEIGHT / 2;
    gcam[1].level = rd8(EARLS_LEVEL);
}

/* Does the game show this object on camera c (it has linked hardware sprites there)? */
static int shown_on(uint32_t o, int c)
{
    if (!gcam[c].on || !(ob8(o + 0x2F) & (c ? 4 : 2))) return 0;
    if (ob8(o + 0x4A)) return 1;
    return !c && sat_linked[ob8(o + 0x30) % 80];
}

/* Is the object wholly where camera c's original screen shows gameplay, yet without
 * sprites there (the game is not showing it)? */
static int hidden_by_game(uint32_t o, uint32_t fr, int c, int attr, int z)
{
    Region r;
    int x, y, bottom;
    if (!gcam[c].on || (int)ob8(o + 0x4C) != gcam[c].level) return 0;
    r.top = 0;
    r.bottom = gcam[c].h;
    r.cam_x = gcam[c].cam_x;
    r.cam_y = gcam[c].cam_y;
    if (!on_game_screen(o, &r)) return 0;
    x = (int16_t)ob16(o) - r.cam_x;
    y = (int16_t)ob16(o + 2) - r.cam_y;
    bottom = gcam[c].h == VDP_HEIGHT ? (win_from > 0 ? win_from : VDP_HEIGHT) : gcam[c].h;
    return !frame_leaves_screen(fr, x, y, z, attr, bottom);
}

/* Fill the draw list for a view: everything on its floor in the picture area
 * x0..x1, y0..y1 (coordinates of its original screen, whose top-left is at world
 * (cam_x, cam_y)). */
static void view_collect(int level, int cam_x, int cam_y, int x0, int y0, int x1, int y1)
{
    uint32_t order[NUM_OBJECTS];
    uint8_t listed[NUM_OBJECTS];
    int e, k, norder = 0, list;
    memset(listed, 0, sizeof listed);
    memset(in_main_list, 0, sizeof in_main_list);
    for (k = 0; k < 2; k++) {
        uint32_t o = ob32(k ? 0xFFD93Cu : 0xFFD938u);
        int guard = 0;
        while (o >= OBJECTS && o < OBJECTS + NUM_OBJECTS * OBJECT_SIZE && guard++ < NUM_OBJECTS) {
            int idx = (int)((o - OBJECTS) / OBJECT_SIZE);
            if (!listed[idx]) { listed[idx] = 1; order[norder++] = o; in_main_list[idx] = (uint8_t)(k == 0); }
            o = ob32(o + 0x5A);
        }
    }
    for (e = 0; e < NUM_OBJECTS; e++)
        if (!listed[e]) order[norder++] = OBJECTS + (uint32_t)e * OBJECT_SIZE;
    learn_shadows();
    {
        int link = 0, count = 0;
        memset(sat_linked, 0, sizeof sat_linked);
        do {
            link = ob8(0xFFB65Au + (uint32_t)link * 8 + 3) & 0x7F;
            if (link && link < 80) sat_linked[link] = 1;
        } while (link && link < 80 && ++count < 80);
    }
    ndraws = 0;
    nsh = 0;
    npieces = 0;
    clip_top = -(1 << 20);
    clip_bottom = 1 << 20;
    for (k = 0; k < norder; k++) {
        uint32_t o = order[k], fr;
        int ox_, oy_, z, attr, c, cam = -1;
        int group = !in_main_list[(o - OBJECTS) / OBJECT_SIZE] ? 2 : (ob8(o + 0x2F) & 0x80) ? 1 : 0;
        Draw *d;
        if (!(ob8(o + 0x2F) & 1)) continue;
        if ((int)ob8(o + 0x4C) != level) continue;
        if (ob8(o + 0x4D) & 0x40) continue;
        if (player_blinked_out(o)) continue;
        fr = ob32(o + 0x22);
        attr = (int)ob16(o + 0x2C);
        z = (int16_t)ob16(o + 4);
        ox_ = (int16_t)ob16(o) - cam_x;
        oy_ = (int16_t)ob16(o + 2) - cam_y;
        if ((ob8(o + 0x4D) & 0x08) && (int8_t)ob8(o + 0x56) >= 0) {
            uint32_t par = OBJECTS + (uint32_t)(ob8(o + 0x56) % NUM_OBJECTS) * OBJECT_SIZE;
            ox_ = (int16_t)ob16(par) + (int8_t)ob8(o + 0x57) - cam_x;
            oy_ = (int16_t)ob16(par + 2) + (int8_t)ob8(o + 0x58) - cam_y;
            z = (int16_t)ob16(par + 4) + (int8_t)ob8(o + 0x59);
        }
        for (c = 0; c < 2; c++) if (shown_on(o, c)) { cam = c; break; }
        if (cam < 0 && g_gv_view_strict) continue;    /* (tests: only what the game shows) */
        if (cam < 0) {
            int hidden = 0;
            for (c = 0; c < 2; c++) if (hidden_by_game(o, fr, c, attr, z)) hidden = 1;
            if (hidden) continue;
        }
        if (z > 0xE0 || z < -0xE0) fr = 0;
        if (ox_ < x0 - 96 || ox_ > x1 + 96 || oy_ < y0 - 96 || oy_ > y1 + 160) continue;
        queue_draw(o, fr, ox_, oy_, z, attr, group, (int16_t)ob16(o + 2), -1,
                   !(ob8(o + 0x2F) & 0x40) ? 0 : (cam >= 0 && !ob8(o + (cam ? 0x49u : 0x48u))) ? 0 : ob8(o + 0x2E));
        d = &draws[ndraws - 1];
        d->sat = cam;                 /* (here: which camera's graphics, -1 = ROM) */
    }
    /* floor items not (yet) objects */
    for (list = 0; list < 3; list++) {
        uint32_t base = list == 0 ? LEVEL_ITEMS : list == 1 ? DROPPED_PRESENTS : TELEPHONES;
        int count = list == 0 ? 64 : list == 1 ? 32 : 2, i;
        for (i = 0; i < count; i++) {
            uint32_t a = base + (uint32_t)i * 8, fr;
            int type = ob8(a), x, y, skip = 0, tel = list == 2, c;
            if (tel ? !ob8(a + 1) : type == 0xFF) continue;
            if ((int)ob8(a + 1) != level) continue;
            if (ob8(a + 2)) {
                uint32_t fo = ob32(OBJECTS + (uint32_t)(ob8(a + 2) % 40) * OBJECT_SIZE + 0x22);
                if (fo >= 0x200 && fo < 0x400000) continue;     /* (drawn as its object) */
            }
            fr = tel ? ob32(ob32(0x0AF03Cu)) : item_frame(type);
            for (c = 0; c < 2; c++) {
                int gx, gy, bottom;
                if (!gcam[c].on || gcam[c].level != level) continue;
                gx = (int16_t)ob16(a + 4) - gcam[c].cam_x;
                gy = (int16_t)ob16(a + 6) - gcam[c].cam_y;
                bottom = gcam[c].h == VDP_HEIGHT ? (win_from > 0 ? win_from : VDP_HEIGHT) : gcam[c].h;
                if (gx > -0x14 && gx < 0x154 && gy > -0x14 && gy < ((!tel && type >= 0x51) ? 0x120 : 0xF4) &&
                    !frame_leaves_screen(fr, gx, gy, 0, 0x8000, bottom))
                    skip = 1;                                   /* (near the game's screen: its object or nothing) */
            }
            if (skip || g_gv_view_strict) continue;
            x = (int16_t)ob16(a + 4) - cam_x;
            y = (int16_t)ob16(a + 6) - cam_y;
            if (x < x0 - 96 || x > x1 + 96 || y < y0 - 96 || y > y1 + 160) continue;
            queue_draw(0, fr, x, y, 0, 0x8000, (!tel && type >= 0x40 && type < 0x51) ? 1 : 0, (int16_t)ob16(a + 6), -1, 0);
            draws[ndraws - 1].sat = -1;
        }
    }
    qsort(draws, (size_t)ndraws, sizeof draws[0], draw_cmp);
    for (k = 0; k < ndraws && npieces < MAX_PIECES; k++) {
        const Draw *d = &draws[k];
        cur_rank = k;
        cur_cam = d->sat >= 0 ? d->sat : 0;
        cur_ui = d->group == 2;
        add_frame(d->o, d->fr, d->x, d->y, d->z, d->attr, x0, y0, x1, y1);
        if (d->shadow && nsh < (int)(sizeof sh / sizeof sh[0])) {
            sh[nsh].cls = d->shadow;
            sh[nsh].x = d->x;
            sh[nsh].y = d->y;
            sh[nsh].size = -1;
            if (d->sat >= 0 && ob8(d->o + (d->sat ? 0x49u : 0x48u))) {
                uint32_t ent = 0xFFB65Au + (uint32_t)(ob8(d->o + (d->sat ? 0x49u : 0x48u)) % 80) * 8;
                int ex = (int)(ob16(ent + 6) & 0x1FF) - 128, ey = (int)(ob16(ent) & 0x3FF) - 128;
                int hs = ((ob8(ent + 2) >> 2) & 3) + 1;
                sh[nsh].size = ob8(ent + 2);
                sh[nsh].attr = ob16(ent + 4);
                if (ex > -128 && ey > -128) {
                    sh[nsh].x = ex + hs * 4 + (d->x - game_screen_x(d->o));
                    sh[nsh].y = ey + 4 + (d->y - game_screen_y(d->o)) - (d->sat ? 0x70 : 0);
                }
            }
            nsh++;
        }
    }
    cur_ui = 0;
    for (k = 0; k < nsh; k++) add_shadow(sh[k].cls, sh[k].x, sh[k].y, x0, y0, x1, y1, sh[k].size, sh[k].attr);
    cur_cam = 0;
}

/* The fixed band of sprites across the middle of the screen (entries 1-10: the
 * hint/message box; the divider in split screen), as the game last sent them,
 * added to the pieces in front, for a view whose original screen is 224 high. */
static void view_screen_sprites(int e_from, int e_to, int top, int h, int x0, int y0, int x1, int y1)
{
    /* (from the table in VRAM: the game writes these entries there directly) */
    uint32_t base = (uint32_t)(vdp_reg[5] & 0x7E) << 9;
    uint8_t linked[80];
    int e, link = 0, count = 0;
    memset(linked, 0, sizeof linked);
    do {
        link = vdp_vram[(base + (uint32_t)link * 8 + 3) & 0xFFFF] & 0x7F;
        if (link && link < 80) linked[link] = 1;
    } while (link && link < 80 && ++count < 80);
    for (e = e_from; e <= e_to && npieces < MAX_PIECES; e++) {
        const uint8_t *en = &vdp_vram[(base + (uint32_t)e * 8) & 0xFFFF];
        int sy = (((en[0] << 8) | en[1]) & 0x3FF) - 128, sx = (((en[6] << 8) | en[7]) & 0x1FF) - 128;
        int size = en[2], attr = (en[4] << 8) | en[5];
        int hs = ((size >> 2) & 3) + 1, vs = (size & 3) + 1, py, c, px;
        Piece *p;
        if (e_from == 1 && !linked[e]) continue;
        if (sy < top || sy >= top + h || sy <= -120) continue;
        sy -= top;
        p = &pieces[npieces];
        p->rank = RANK_FRONT;
        p->cy0 = -(1 << 20);
        p->cy1 = 1 << 20;
        p->ui = e >= 11;
        p->x = sx;
        p->y = sy;
        p->w = hs * 8;
        p->h = vs * 8;
        if (p->x + p->w <= x0 || p->x >= x1 || p->y + p->h <= y0 || p->y >= y1) continue;
        p->ncells = 0;
        if (pieces_as_cells) { piece_sprite_cells(p, (uint32_t)attr, hs, vs, 0); npieces++; continue; }
        for (py = 0; py < p->h; py++) {
            int row = (attr & 0x1000) ? p->h - 1 - py : py;
            for (c = 0; c < hs; c++) {
                int colx = (attr & 0x0800) ? (hs - 1 - c) : c;
                uint32_t tile = ((uint32_t)attr & 0x7FF) + (uint32_t)(colx * vs + (row >> 3));
                uint8_t t8[8];
                tile_row(t8, (((uint32_t)attr & 0xF800) & ~0x1000u) | (tile & 0x7FF), row & 7);
                for (px = 0; px < 8; px++) p->px[py * p->w + c * 8 + px] = (t8[px] & PIX_OPAQUE) ? t8[px] : 0;
            }
        }
        npieces++;
    }
}

/* The fixed band of sprites across the middle (entries 1-10: the hint/message box)
 * for a one-player view. */
static void view_band(int x0, int y0, int x1, int y1)
{
    view_screen_sprites(1, 10, -32, VDP_HEIGHT + 32, x0, y0, x1, y1);
}

/* In split screen the game puts each player's level number and life bar at the
 * top of that player's half (status-bar entries 11-22): part of the view. */
static void view_half_hud(int top, int x0, int y0, int x1, int y1)
{
    int first = npieces, i;
    view_screen_sprites(11, 22, top, VDP_HEIGHT / 2, x0, y0, x1, y1);
    /* (kept in the view's top corners: left half of the original screen to the
     * left edge, right half to the right edge; x0 = -(the view's margin)) */
    for (i = first; i < npieces; i++) {
        pieces[i].x += pieces[i].x < 160 ? x0 : -x0;
        pieces[i].y += y0;
        pieces[i].ui = 1;
    }
}

/* Plane B (the backdrop) for a view whose original screen's top-left is world
 * (cam_x, cam_y): it scrolls at half speed (as VBlank / HBlankJSR set it). */
static void view_plane_b_row(uint8_t *buf, int off, int x_from, int x_to, int screen_y, int cam_x, int cam_y)
{
    static const int psz[4] = {32, 64, 32, 128};
    int pw = psz[vdp_reg[16] & 3], ph = psz[(vdp_reg[16] >> 4) & 3];
    uint32_t base = (uint32_t)(vdp_reg[4] & 7) << 13;
    int hs = -(cam_x >= 0 ? cam_x >> 1 : -((-cam_x) >> 1));
    int vs = cam_y >= 0 ? cam_y >> 1 : -((-cam_y) >> 1);
    int y = (screen_y + vs) & (ph * 8 - 1), x = x_from - ((x_from - hs) & 7), cx = ((x - hs) & (pw * 8 - 1)) >> 3, i;
    uint32_t row_base = base + (uint32_t)((y >> 3) * pw) * 2;
    /* (8 pixels at a time, a tile column each) */
    for (; x < x_to; x += 8, cx = (cx + 1) & (pw - 1)) {
        uint32_t ea = (row_base + (uint32_t)cx * 2) & 0xFFFF;
        uint8_t t8[8];
        tile_row(t8, ((uint32_t)vdp_vram[ea] << 8) | vdp_vram[ea + 1], y & 7);
        if (x >= x_from && x + 8 <= x_to) memcpy(buf + x + off, t8, 8);
        else
            for (i = 0; i < 8; i++)
                if (x + i >= x_from && x + i < x_to) buf[x + i + off] = t8[i];
    }
}

/* Plane A (the level) for a view: the game's own plane in VRAM where it holds this
 * camera's screen (it draws changed ground there a few frames after changing the
 * layout), the floor layout everywhere else. */
static void view_plane_a_row(uint8_t *buf, int off, int x_from, int x_to, int screen_y, const Region *r,
                             int vram_plane, int vram_h)
{
    level_row(buf, off, x_from, x_to, r, screen_y);
    if (vram_plane >= 0 && screen_y >= 0 && screen_y < vram_h) {
        uint32_t base = vram_plane ? 0xC000u : 0xA000u;
        int wy = r->cam_y + screen_y, cy = wy >> 3, row = wy & 7;
        int xa = x_from < 0 ? 0 : x_from, xb = x_to > 320 ? 320 : x_to, x;
        for (x = xa; x < xb; x++) {
            int wx = r->cam_x + x, cx = wx >> 3;
            uint32_t ea = (base + (uint32_t)(((cy & 31) * 64) + (cx & 63)) * 2) & 0xFFFF;
            uint8_t t8[8];
            if (x == xa || (wx & 7) == 0) tile_row(t8, ((uint32_t)vdp_vram[ea] << 8) | vdp_vram[ea + 1], row);
            buf[x + off] = t8[wx & 7];
        }
    }
}

/* One line (0-223) of the window plane (status bar, present list), 320 wide,
 * into buf[off..off+320), clipped to buf[0..w). */
static void window_line_clip(uint8_t *buf, int off, int line, int w)
{
    uint32_t base = (uint32_t)(vdp_reg[3] & 0x3C) << 10;
    int col, i;
    for (col = 0; col < 40; col++) {
        uint32_t ea = (base + (uint32_t)((line >> 3) * 64 + col) * 2) & 0xFFFF;
        uint8_t t8[8];
        tile_row(t8, ((uint32_t)vdp_vram[ea] << 8) | vdp_vram[ea + 1], line & 7);
        for (i = 0; i < 8; i++) {
            int x = off + col * 8 + i;
            if (x >= 0 && x < w) buf[x] = t8[i];
        }
    }
}

static void window_line(uint8_t *buf, int off, int line) { window_line_clip(buf, off, line, off + 320); }



/* The layer a pixel shows (0 sprite, 1 plane A, 2 plane B, 3 the backdrop) for each
 * combination of their opaque / priority bits: priority sprite, plane A, plane B,
 * then the same without priority, as the VDP decides */
static uint8_t layer_pick[64];

static void layer_pick_init(void)
{
    int k, l;
    for (k = 0; k < 64; k++) {
        int bits[3] = {k & 3, (k >> 2) & 3, (k >> 4) & 3}, pick = 3;   /* (bit 1 opaque, bit 0 priority) */
        for (l = 0; l < 3 && pick == 3; l++) if (bits[l] == 3) pick = l;
        for (l = 0; l < 3 && pick == 3; l++) if (bits[l] & 2) pick = l;
        layer_pick[k] = (uint8_t)pick;
    }
}

int gameview_render_view(int player, int cam_x, int cam_y, int game_cam, int screen_h,
                         uint32_t *out, int w, int h, int pitch, int band)
{
    static uint8_t a[GAMEVIEW_MAX_W], b[GAMEVIEW_MAX_W], sp[GAMEVIEW_MAX_W];
    Region r;
    int ox = (w - 320) / 2, oy = (h - screen_h) / 2, y, x;
    uint32_t backdrop = vdp_palette_rgb(vdp_reg[7] & 0x3F), pal[64];
    if (w > GAMEVIEW_MAX_W || h > GAMEVIEW_MAX_H) return -1;
    for (x = 0; x < 64; x++) pal[x] = vdp_palette_rgb(x);
    if (!layer_pick[0] && !layer_pick[63]) layer_pick_init();   /* (once: entry 0 is 3 when set up) */
    views_setup_cameras();
    r.top = 0;
    r.bottom = screen_h;
    r.cam_x = cam_x;
    r.cam_y = cam_y;
    r.tiles = player ? EARL_LEVEL_TILES : TOEJAM_LEVEL_TILES;
    r.level = rd8(player ? EARLS_LEVEL : TOEJAMS_LEVEL);
    floor_sync(r.tiles, r.level);
    view_collect(r.level, cam_x, cam_y, -ox, -oy, w - ox, h - oy);
    if (band) view_band(-ox, -oy, w - ox, h - oy);
    else if (screen_h == VDP_HEIGHT / 2 && game_cam >= 0 && !view_skip_half_hud) view_half_hud(game_cam ? VDP_HEIGHT / 2 : 0, -ox, -oy, w - ox, h - oy);
    for (y = 0; y < h; y++) {
        int sy = y - oy;
        uint32_t *o = out + (size_t)y * pitch;
        memset(sp, 0, (size_t)w);
        view_plane_b_row(b, ox, -ox, w - ox, sy, cam_x, cam_y);
        view_plane_a_row(a, ox, -ox, w - ox, sy, &r, game_cam, screen_h);
        if (sy >= 0 && sy < view_top_window) window_line_clip(a, ox, sy, w);   /* (present list: the window replaces plane A) */
        pieces_row(sp, ox, -ox, w - ox, sy);
        for (x = 0; x < w; x++) {
            /* (which layer shows: from the opaque and priority bits of the three) */
            uint8_t px[4];
            int k;
            px[0] = sp[x];
            px[1] = a[x];
            px[2] = b[x];
            k = layer_pick[((px[0] >> 6) & 3) | (((px[1] >> 6) & 3) << 2) | (((px[2] >> 6) & 3) << 4)];
            o[x] = k < 3 ? pal[px[k] & 0x3F] : backdrop;
        }
    }
    floor_on[0] = floor_on[1] = 0;
    return 0;
}

/* ==== The same view as a list for a GPU (gfx/viewlist.h) =========================
 * gameview_render_view's picture, as cells and pieces: what is drawn where is
 * worked out exactly as there, the pixels are left to the GPU. */

int g_gv_use_list;            /* the platform draws views as lists (gameview_list) */
const ViewList *gameview_last_list(void) { return shown_list; }
static int vl_check = -1;     /* (tests, TJE_VLCHECK) */
static int vl_check_on(void) { if (vl_check < 0) vl_check = getenv("TJE_VLCHECK") != NULL; return vl_check; }
int vl_checked, vl_bad_frames;
static uint8_t vl_piece_pool[VL_MAX_PIECES * 2048] __attribute__((aligned(64)));
static size_t vl_pool_used;

static int vl_batch_open;   /* (cells go to the last batch begun - none if it was empty) */
static int vl_stage;        /* (the stage of the batches begun now: VlBatch) */
static void vl_batch(ViewList *vl, int layer, int x0, int y0, int x1, int y1)
{
    VlBatch *b;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > vl->w) x1 = vl->w;
    if (y1 > vl->h) y1 = vl->h;
    if (vl->nbatches >= VL_MAX_BATCHES || x0 >= x1 || y0 >= y1) { vl_batch_open = 0; return; }
    vl_batch_open = 1;
    b = &vl->batches[vl->nbatches++];
    b->layer = layer;
    b->x0 = x0; b->y0 = y0; b->x1 = x1; b->y1 = y1;
    b->first = vl->ncells;
    b->count = 0;
    b->piece = -1;
    b->stage = vl_stage;
}

static inline void vl_cell_ext(ViewList *vl, int x, int y, uint32_t entry, uint16_t ext)
{
    VlCell *c;
    if (!(entry & 0x7FF) && !(ext & 1)) return;
    if (vl->ncells >= VL_MAX_CELLS || !vl_batch_open) return;
    c = &vl->cells[vl->ncells++];
    c->x = (int16_t)x;
    c->y = (int16_t)y;
    c->entry = (uint16_t)entry;
    c->ext = ext;
    vl->batches[vl->nbatches - 1].count++;
}

static inline void vl_cell(ViewList *vl, int x, int y, uint32_t entry)
{
    VlCell *c;
    if (!(entry & 0x7FF) || vl->ncells >= VL_MAX_CELLS || !vl_batch_open) return;   /* (tile 0: see-through, nothing to draw) */
    c = &vl->cells[vl->ncells++];
    c->x = (int16_t)x;
    c->y = (int16_t)y;
    c->entry = (uint16_t)entry;
    c->ext = 0;
    vl->batches[vl->nbatches - 1].count++;
}

/* the cells of the batch just begun: of the floor (world grid) */
static void vl_floor(ViewList *vl, const Region *r, int ox, int oy)
{
    const VlBatch *b = &vl->batches[vl->nbatches - 1];
    int cx0 = (r->cam_x + b->x0 - ox) >> 3, cx1 = (r->cam_x + b->x1 - 1 - ox) >> 3;
    int cy0 = (r->cam_y + b->y0 - oy) >> 3, cy1 = (r->cam_y + b->y1 - 1 - oy) >> 3, cx, cy;
    for (cy = cy0; cy <= cy1; cy++)
        for (cx = cx0; cx <= cx1; cx++)
            vl_cell(vl, cx * 8 - r->cam_x + ox, cy * 8 - r->cam_y + oy, floor_cell(r->tiles, r->level, cx, cy));
}

/* ... of a VRAM plane holding the same world grid (the game's plane A) */
static void vl_vram_plane(ViewList *vl, const Region *r, uint32_t base, int ox, int oy)
{
    const VlBatch *b = &vl->batches[vl->nbatches - 1];
    int cx0 = (r->cam_x + b->x0 - ox) >> 3, cx1 = (r->cam_x + b->x1 - 1 - ox) >> 3;
    int cy0 = (r->cam_y + b->y0 - oy) >> 3, cy1 = (r->cam_y + b->y1 - 1 - oy) >> 3, cx, cy;
    for (cy = cy0; cy <= cy1; cy++)
        for (cx = cx0; cx <= cx1; cx++) {
            uint32_t ea = (base + (uint32_t)(((cy & 31) * 64) + (cx & 63)) * 2) & 0xFFFF;
            vl_cell(vl, cx * 8 - r->cam_x + ox, cy * 8 - r->cam_y + oy, ((uint32_t)vdp_vram[ea] << 8) | vdp_vram[ea + 1]);
        }
}

/* ... of plane B, scrolled at half the camera's speed (view_plane_b_row) */
static void vl_plane_b(ViewList *vl, int cam_x, int cam_y, int ox, int oy)
{
    static const int psz[4] = {32, 64, 32, 128};
    const VlBatch *b = &vl->batches[vl->nbatches - 1];
    int pw = psz[vdp_reg[16] & 3], ph = psz[(vdp_reg[16] >> 4) & 3];
    uint32_t base = (uint32_t)(vdp_reg[4] & 7) << 13;
    int hs = -(cam_x >= 0 ? cam_x >> 1 : -((-cam_x) >> 1));
    int vs = cam_y >= 0 ? cam_y >> 1 : -((-cam_y) >> 1);
    /* original-screen pixel (x, y) shows plane pixel (x - hs, y + vs) */
    int kx0 = (b->x0 - ox - hs) >> 3, kx1 = (b->x1 - 1 - ox - hs) >> 3;
    int ky0 = (b->y0 - oy + vs) >> 3, ky1 = (b->y1 - 1 - oy + vs) >> 3, kx, ky;
    for (ky = ky0; ky <= ky1; ky++)
        for (kx = kx0; kx <= kx1; kx++) {
            uint32_t ea = (base + (uint32_t)((ky & (ph - 1)) * pw + (kx & (pw - 1))) * 2) & 0xFFFF;
            vl_cell(vl, kx * 8 + hs + ox, ky * 8 - vs + oy, ((uint32_t)vdp_vram[ea] << 8) | vdp_vram[ea + 1]);
        }
}

/* The view of gameview_render_view as a list, at (vx, vy) of a w x h picture
 * (the list's size: pw x ph). */
static void build_view_list(ViewList *vl, int player, int cam_x, int cam_y, int game_cam, int screen_h,
                            int vx, int vy, int w, int h, int band)
{
    Region r;
    int ox = vx + (w - 320) / 2, oy = vy + (h - screen_h) / 2, i, n;
    static int order[VL_MAX_PIECES];
    views_setup_cameras();
    r.top = 0;
    r.bottom = screen_h;
    r.cam_x = cam_x;
    r.cam_y = cam_y;
    r.tiles = player ? EARL_LEVEL_TILES : TOEJAM_LEVEL_TILES;
    r.level = rd8(player ? EARLS_LEVEL : TOEJAMS_LEVEL);
    floor_sync(r.tiles, r.level);
    pieces_as_cells = 1;   /* (the objects as cells: no pixels made on the CPU) */
    view_collect(r.level, cam_x, cam_y, vx - ox, vy - oy, vx + w - ox, vy + h - oy);
    if (band) view_band(vx - ox, vy - oy, vx + w - ox, vy + h - oy);
    else if (screen_h == VDP_HEIGHT / 2 && game_cam >= 0 && !view_skip_half_hud)
        view_half_hud(game_cam ? VDP_HEIGHT / 2 : 0, vx - ox, vy - oy, vx + w - ox, vy + h - oy);
    pieces_as_cells = 0;
    /* plane B */
    vl_batch(vl, 0, vx, vy, vx + w, vy + h);
    vl_plane_b(vl, cam_x, cam_y, ox, oy);
    /* plane A: the game's own plane where it holds this camera's screen, the floor
     * layout around it (view_plane_a_row) */
    if (game_cam >= 0) {
        int ax0 = ox, ax1 = ox + 320, ay0 = oy, ay1 = oy + screen_h;
        uint32_t base = game_cam ? 0xC000u : 0xA000u;
        if (ax0 < vx) ax0 = vx;
        if (ax1 > vx + w) ax1 = vx + w;
        if (ay0 < vy) ay0 = vy;
        if (ay1 > vy + h) ay1 = vy + h;
        vl_batch(vl, 1, vx, vy, vx + w, ay0); vl_floor(vl, &r, ox, oy);              /* above */
        vl_batch(vl, 1, vx, ay1, vx + w, vy + h); vl_floor(vl, &r, ox, oy);          /* below */
        vl_batch(vl, 1, vx, ay0, ax0, ay1); vl_floor(vl, &r, ox, oy);                /* left */
        vl_batch(vl, 1, ax1, ay0, vx + w, ay1); vl_floor(vl, &r, ox, oy);            /* right */
        vl_batch(vl, 1, ax0, ay0, ax1, ay1); vl_vram_plane(vl, &r, base, ox, oy);    /* the game's */
    } else {
        vl_batch(vl, 1, vx, vy, vx + w, vy + h);
        vl_floor(vl, &r, ox, oy);
    }
    /* the pieces, back first: pieces_row keeps the lowest rank (the first of equal
     * ones) in front */
    n = 0;
    for (i = 0; i < npieces && n < VL_MAX_PIECES; i++)
        if (pieces[i].w > 0 && pieces[i].h > 0) order[n++] = i;
    for (i = 1; i < n; i++) {
        int k = order[i], j = i - 1;
        while (j >= 0 && (pieces[order[j]].rank < pieces[k].rank ||
                          (pieces[order[j]].rank == pieces[k].rank && order[j] < k))) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = k;
    }
    /* Sprite against sprite is decided before sprite against plane (the one in
     * front wins whatever the priorities; then its priority counts): drawn in
     * passes by priority, a sprite behind with priority would show through one in
     * front without. Where pieces of different priority overlap (rare) both are
     * made pixels and the pixels behind cleared, below. */
    {
        int j;
        for (i = 0; i < n; i++) {
            Piece *bk = &pieces[order[i]];
            for (j = i + 1; j < n; j++) {
                Piece *fr = &pieces[order[j]];
                int bp, fp;
                if (bk->x >= fr->x + fr->w || fr->x >= bk->x + bk->w || bk->y >= fr->y + fr->h || fr->y >= bk->y + bk->h) continue;
                bp = bk->ncells ? (bk->cells[0].entry & 0x8000) != 0 : 2;
                fp = fr->ncells ? (fr->cells[0].entry & 0x8000) != 0 : 2;
                if (bp == 0 || fp == 1) continue;                 /* (the one behind without, or in front with: fine) */
                piece_px(bk);
                piece_px(fr);
            }
        }
    }
    vl->pieces_in_batches = 1;
    {
        int cy0 = 1 << 30, cy1 = 0, open = 0;
        for (i = 0; i < n; i++) {
            const Piece *p = &pieces[order[i]];
            int y0 = (p->cy0 < -(1 << 19) ? -(1 << 19) : p->cy0) + oy, y1 = (p->cy1 > (1 << 19) ? (1 << 19) : p->cy1) + oy, k;
            if (y0 < vy) y0 = vy;
            if (y1 > vy + h) y1 = vy + h;
            if (y0 >= y1) continue;
            if (p->ncells) {
                /* cells: in a batch with the pieces before it clipped alike */
                if (!open || y0 != cy0 || y1 != cy1) {
                    vl_batch(vl, 2, vx, y0, vx + w, y1);
                    if (vl_batch_open) vl->batches[vl->nbatches - 1].piece = -1;
                    cy0 = y0;
                    cy1 = y1;
                    open = 1;
                }
                for (k = 0; k < p->ncells; k++) vl_cell_ext(vl, p->x + ox + p->cells[k].dx, p->y + oy + p->cells[k].dy, p->cells[k].entry, p->cells[k].ext);
                continue;
            }
            {
                /* pixels (a player wading; pieces overlapping with different
                 * priorities): a piece of its own */
                VlPiece *q = &vl->pieces[vl->npieces];
                int stride = (p->w + 15) & ~15, row;
                uint8_t *dst = vl_piece_pool + vl_pool_used;
                if (vl->npieces >= VL_MAX_PIECES || vl_pool_used + (size_t)stride * (size_t)p->h > sizeof vl_piece_pool) continue;
                vl_pool_used += ((size_t)stride * (size_t)p->h + 63) & ~(size_t)63;
                for (row = 0; row < p->h; row++) memcpy(dst + row * stride, p->px + row * p->w, (size_t)p->w);
                q->stride = stride;
                q->x = p->x + ox;
                q->y = p->y + oy;
                q->w = p->w;
                q->h = p->h;
                q->y0 = y0;
                q->y1 = y1;
                q->px = dst;
                vl_batch(vl, 2, vx, y0, vx + w, y1);
                if (vl_batch_open) vl->batches[vl->nbatches - 1].piece = vl->npieces;
                vl->npieces++;
                open = 0;
            }
        }
    }
    /* (the pixels behind cleared, between pieces made pixels) */
    for (i = 0; i < vl->npieces; i++) {
        VlPiece *bk = &vl->pieces[i];
        int j;
        for (j = i + 1; j < vl->npieces; j++) {
            const VlPiece *fr = &vl->pieces[j];
            int x0 = bk->x > fr->x ? bk->x : fr->x, x1 = bk->x + bk->w < fr->x + fr->w ? bk->x + bk->w : fr->x + fr->w;
            int y0 = bk->y > fr->y ? bk->y : fr->y, y1 = bk->y + bk->h < fr->y + fr->h ? bk->y + bk->h : fr->y + fr->h, x, y;
            if (y0 < fr->y0) y0 = fr->y0;
            if (y1 > fr->y1) y1 = fr->y1;
            if (x0 >= x1 || y0 >= y1) continue;
            for (y = y0; y < y1; y++) {
                uint8_t *b = (uint8_t *)bk->px + (size_t)(y - bk->y) * (size_t)bk->stride - bk->x;
                const uint8_t *f = fr->px + (size_t)(y - fr->y) * (size_t)fr->stride - fr->x;
                for (x = x0; x < x1; x++)
                    if ((f[x] & (PIX_OPAQUE | PIX_PRIO)) == PIX_OPAQUE && (b[x] & (PIX_OPAQUE | PIX_PRIO)) == (PIX_OPAQUE | PIX_PRIO))
                        b[x] = 0;
            }
        }
    }
    floor_on[0] = floor_on[1] = 0;
}

static void vl_begin(ViewList *vl, int w, int h)
{
    int x;
    present_gpu_idle();   /* (the GPU may still be drawing the last list from the pieces) */
    vl_pool_used = 0;
    vl->w = w;
    vl->h = h;
    vl->backdrop = vdp_palette_rgb(vdp_reg[7] & 0x3F);
    for (x = 0; x < 64; x++) vl->pal[x] = vdp_palette_rgb(x);
    vl->vram = vdp_vram;
    vl->tiles_changed = vdp_tile_dirty_gpu;
    vl->cache = sc_tiles;
    vl->cache_changed = sc_changed;
    vl->pieces_in_batches = 0;
    vl->frozen = 0;
    vl->ncells = vl->nbatches = vl->npieces = vl->noverlays = 0;
}

static uint32_t ovl_stamp;   /* (the stamp for overlays added now) */
static void vl_overlay(ViewList *vl, int x, int y, int w, int h, const uint32_t *px, int pitch)
{
    VlOverlay *o;
    if (vl->noverlays >= VL_MAX_OVERLAYS || w <= 0 || h <= 0) return;
    o = &vl->overlays[vl->noverlays++];
    o->x = x; o->y = y; o->w = w; o->h = h; o->px = px; o->pitch = pitch;
    o->keyed = 0;
    o->stamp = ovl_stamp;
    o->tile_w = o->tile_h = o->tx0 = o->ty0 = 0;
}

/* (an overlay clipped to the list's picture) */
static void vl_overlay_clip(ViewList *vl, int x, int y, int w, int h, const uint32_t *px, int pitch)
{
    if (x < 0) { px -= x; w += x; x = 0; }
    if (y < 0) { px -= (size_t)y * (size_t)pitch; h += y; y = 0; }
    if (x + w > vl->w) w = vl->w - x;
    if (y + h > vl->h) h = vl->h - y;
    vl_overlay(vl, x, y, w, h, px, pitch);
}

/* ==== Screens other than gameplay as lists ======================================
 * The VDP's picture of the frame (menus, cutscenes, the ride between floors, the
 * map) for a GPU: its planes, window and sprites as cells of VRAM tiles, drawn from
 * what each line was drawn with (vdp_lines): the lines alike form bands, each band
 * its own batches. Nothing of the picture is worked out pixel by pixel. Screens the
 * lists can't show as the VDP does (a palette changed part way down, scroll modes
 * the game doesn't use, sprite masking) are drawn the usual way. */

#define SL_MAX_BANDS 12

/* the cells of a plane over the batch just begun: plane at `base` (pw x ph cells),
 * scrolled hs across and vs down; the original screen's (0, 0) at (ox, oy) */
static void vl_vdp_plane(ViewList *vl, uint32_t base, int pw, int ph, int hs, int vs, int ox, int oy)
{
    const VlBatch *b = &vl->batches[vl->nbatches - 1];
    /* screen pixel (x, y) shows plane pixel (x - hs, y + vs) */
    int kx0 = (b->x0 - ox - hs) >> 3, kx1 = (b->x1 - 1 - ox - hs) >> 3;
    int ky0 = (b->y0 - oy + vs) >> 3, ky1 = (b->y1 - 1 - oy + vs) >> 3, kx, ky;
    for (ky = ky0; ky <= ky1; ky++)
        for (kx = kx0; kx <= kx1; kx++) {
            uint32_t ea = (base + (uint32_t)((ky & (ph - 1)) * pw + (kx & (pw - 1))) * 2) & 0xFFFF;
            vl_cell(vl, kx * 8 + hs + ox, ky * 8 - vs + oy, ((uint32_t)vdp_vram[ea] << 8) | vdp_vram[ea + 1]);
        }
}

/* the window's cells over the batch just begun (no scrolling) */
static void vl_vdp_window(ViewList *vl, uint32_t base, int ox, int oy)
{
    const VlBatch *b = &vl->batches[vl->nbatches - 1];
    int cx0 = (b->x0 - ox) >> 3, cx1 = (b->x1 - 1 - ox) >> 3, cy0 = (b->y0 - oy) >> 3, cy1 = (b->y1 - 1 - oy) >> 3, cx, cy;
    for (cy = cy0; cy <= cy1; cy++)
        for (cx = cx0; cx <= cx1; cx++) {
            uint32_t ea = (base + (uint32_t)(cy * 64 + cx) * 2) & 0xFFFF;
            if (cx < 0 || cx >= 40 || cy < 0 || cy >= 28) continue;
            vl_cell(vl, cx * 8 + ox, cy * 8 + oy, ((uint32_t)vdp_vram[ea] << 8) | vdp_vram[ea + 1]);
        }
}

/* the sprites' cells over the batch just begun, the ones behind first (the
 * sprite table at `base`, followed from entry 0 as the VDP does) */
static void vl_vdp_sprites_of(ViewList *vl, uint32_t base, int ox, int oy, int lo, int hi);
static void vl_vdp_sprites(ViewList *vl, uint32_t base, int ox, int oy) { vl_vdp_sprites_of(vl, base, ox, oy, 0, 79); }

/* (only the entries lo..hi of the table, in the table's order) */
static void vl_vdp_sprites_of(ViewList *vl, uint32_t base, int ox, int oy, int lo, int hi)
{
    int order[80], n = 0, link = 0, k;
    do {
        order[n++] = link;
        link = vdp_vram[(base + (uint32_t)link * 8 + 3) & 0xFFFF] & 0x7F;
    } while (link != 0 && n < 80);
    for (k = n - 1; k >= 0; k--) {
        if (order[k] < lo || order[k] > hi) continue;
        {
        const uint8_t *e = &vdp_vram[(base + (uint32_t)order[k] * 8) & 0xFFFF];
        int sy = (((e[0] << 8) | e[1]) & 0x3FF) - 128, hs = ((e[2] >> 2) & 3) + 1, vs = (e[2] & 3) + 1;
        uint32_t attr = ((uint32_t)e[4] << 8) | e[5];
        int sx = (((e[6] << 8) | e[7]) & 0x1FF) - 128, c, r;
        for (c = 0; c < hs; c++)
            for (r = 0; r < vs; r++) {
                int colx = (attr & 0x0800) ? hs - 1 - c : c, rowy = (attr & 0x1000) ? vs - 1 - r : r;
                uint32_t tile = ((attr & 0x7FF) + (uint32_t)(colx * vs + rowy)) & 0x7FF;
                vl_cell(vl, sx + c * 8 + ox, sy + r * 8 + oy, (attr & 0xF800) | tile);
            }
        }
    }
}

/* The status bar (window rows from..to-1 of the original screen, with its sprites,
 * entries 11-22, as render_status_bar draws it) as cells, at picture row y0 and
 * column ox, over the view (stage 1: its rectangle backdrop first, where the bar
 * is clear) */
static void vl_status_bar(ViewList *vl, int from, int to, int ox, int y0)
{
    int oy = y0 - from;
    vl_stage = 1;
    vl_batch(vl, 3, ox, y0, ox + 320, y0 + (to - from));
    vl_batch(vl, 1, ox, y0, ox + 320, y0 + (to - from));
    if (vl_batch_open) vl_vdp_window(vl, (uint32_t)(vdp_reg[3] & 0x3C) << 10, ox, oy);
    vl_batch(vl, 2, ox, y0, ox + 320, y0 + (to - from));
    if (vl_batch_open) vl_vdp_sprites_of(vl, (uint32_t)(vdp_reg[5] & 0x7E) << 9, ox, oy, 11, 22);
    vl_stage = 0;
}

/* Sprite masking (a sprite at x 0 after another on its lines hides the rest of
 * the line): the lists don't show it. Does this frame use it? */
static int sprites_masking(uint32_t base)
{
    int n = 0, link = 0, i, j;
    struct { int y0, y1, x; } sp[80];
    do {
        const uint8_t *e = &vdp_vram[(base + (uint32_t)link * 8) & 0xFFFF];
        sp[n].y0 = (((e[0] << 8) | e[1]) & 0x3FF) - 128;
        sp[n].y1 = sp[n].y0 + ((e[2] & 3) + 1) * 8;
        sp[n].x = (((e[6] << 8) | e[7]) & 0x1FF) - 128;
        n++;
        link = e[3] & 0x7F;
    } while (link != 0 && n < 80);
    for (i = 1; i < n; i++) {
        if (sp[i].x != -128 || sp[i].y1 <= 0 || sp[i].y0 >= VDP_HEIGHT) continue;
        for (j = 0; j < i; j++)
            if (sp[j].y0 < sp[i].y1 && sp[i].y0 < sp[j].y1) return 1;
    }
    return 0;
}

/* The VDP's picture of this frame into vl: the original screen at (ox, oy).
 * wide: -1 nothing outside it; 0 or 1 plane A or B, 2 both, drawn over the whole
 * list (a repeating background); sprites_wide: sprites outside it too.
 * Returns 0 if it can't be drawn so. */
static int build_screen_list(ViewList *vl, int ox, int oy, int wide, int sprites_wide)
{
    static const int psz[4] = {32, 64, 32, 128};
    int from[SL_MAX_BANDS], to[SL_MAX_BANDS], nb = 0, k, line;
    int wa = wide == 0 || wide == 2, wb = wide == 1 || wide == 2;   /* (planes drawn wide) */
    uint16_t cram = 0xFFFF;
    /* bands of lines drawn alike */
    for (line = 0; line < VDP_HEIGHT; line++) {
        if (nb && vdp_lines_alike(&vdp_lines[line], &vdp_lines[from[nb - 1]])) { to[nb - 1] = line + 1; continue; }
        if (nb == SL_MAX_BANDS) { sl_why = "bands"; return 0; }
        from[nb] = line;
        to[nb] = line + 1;
        nb++;
    }
    for (k = 0; k < nb; k++) {
        const VdpLineState *st = &vdp_lines[from[k]];
        if (!st->on) continue;
        if (!(st->reg[12] & 1) || (st->reg[11] & 7)) { sl_why = "mode"; return 0; }            /* (H32, scroll by cell/line/column) */
        cram = st->cram_ver;   /* (a palette changed part way down: shown as at the end) */
        if (st->reg[7] != vdp_lines[from[0]].reg[7]) { sl_why = "backdrop"; return 0; }
    }
    (void)cram;
    if (sprites_masking((uint32_t)(vdp_reg[5] & 0x7E) << 9)) { sl_why = "masking"; return 0; }
    /* (the tiles or planes changed after lines were shown: the screen being
     * rebuilt - those lines can't be drawn from what is there now) */
    for (k = 0; k < nb; k++)
        if (vdp_lines[from[k]].on && (vdp_lines[to[k] - 1].vram_chg != vdp_vram_changes ||
                                      vdp_lines[from[k]].sat_chg != vdp_sat_changes)) { sl_stale = 1; return 0; }
    vl->backdrop = vdp_palette_rgb(vdp_lines[from[0]].reg[7] & 0x3F);
    for (k = 0; k < nb; k++) {
        const VdpLineState *st = &vdp_lines[from[k]];
        const uint8_t *rg = st->reg;
        int pw = psz[rg[16] & 3], ph = psz[(rg[16] >> 4) & 3];
        uint32_t base_a = (uint32_t)(rg[2] & 0x38) << 10, base_b = (uint32_t)(rg[4] & 7) << 13;
        uint32_t base_w = (uint32_t)(rg[3] & 0x3C) << 10;
        int y0 = oy + from[k], y1 = oy + to[k], wv = (rg[18] & 0x1F) * 8, down = rg[18] & 0x80;
        int wh = (rg[17] & 0x1F) * 16, right = rg[17] & 0x80, wx0 = 0, wx1 = 0, wy0, wy1, part;
        int x_in0 = ox, x_in1 = ox + 320;
        /* (the band's rows reach out to the list's edge above the first line and
         * below the last, where a plane is drawn wide) */
        int ey0 = (k == 0 && wide >= 0) ? 0 : y0, ey1 = (k == nb - 1 && wide >= 0) ? vl->h : y1;
        if (!st->on) continue;
        /* plane B */
        vl_batch(vl, 0, wb ? 0 : x_in0, wb ? ey0 : y0, wb ? vl->w : x_in1, wb ? ey1 : y1);
        vl_vdp_plane(vl, base_b, pw, ph, st->hsb, (int)(st->vsb & 0x3FF), ox, oy);
        /* plane A, but where the window is: its rows (wy0..wy1, the window's
         * vertical part) and, beside them, its columns (wx0..wx1) */
        if (down) { wy0 = wv; wy1 = VDP_HEIGHT; } else { wy0 = 0; wy1 = wv; }
        if (wh) { if (right) { wx0 = wh; wx1 = 320; } else { wx0 = 0; wx1 = wh > 320 ? 320 : wh; } }
        for (part = 0; part < 3; part++) {
            /* the band split at the window's rows: above, in, below */
            int r0 = part == 0 ? from[k] : part == 1 ? (wy0 > from[k] ? wy0 : from[k]) : (wy1 > from[k] ? wy1 : from[k]);
            int r1 = part == 0 ? (wy0 < to[k] ? wy0 : to[k]) : part == 1 ? (wy1 < to[k] ? wy1 : to[k]) : to[k];
            int ar0 = oy + r0, ar1 = oy + r1;
            if (r1 <= r0) continue;
            if (part == 1) {
                vl_batch(vl, 1, x_in0, ar0, x_in1, ar1);
                vl_vdp_window(vl, base_w, ox, oy);
                continue;
            }
            if (wa) {
                /* (plane A wide: its rows reach the list's edges too) */
                if (r0 == 0 && k == 0) ar0 = 0;
                if (r1 == VDP_HEIGHT || (k == nb - 1 && r1 == to[k])) ar1 = vl->h;
            }
            if (wx1 > wx0) {
                vl_batch(vl, 1, ox + wx0, ar0, ox + wx1, ar1);
                vl_vdp_window(vl, base_w, ox, oy);
                if (wx0 > 0) { vl_batch(vl, 1, wa ? 0 : x_in0, ar0, ox + wx0, ar1); vl_vdp_plane(vl, base_a, pw, ph, st->hsa, (int)(st->vsa & 0x3FF), ox, oy); }
                if (wx1 < 320) { vl_batch(vl, 1, ox + wx1, ar0, wa ? vl->w : x_in1, ar1); vl_vdp_plane(vl, base_a, pw, ph, st->hsa, (int)(st->vsa & 0x3FF), ox, oy); }
            } else {
                vl_batch(vl, 1, wa ? 0 : x_in0, ar0, wa ? vl->w : x_in1, ar1);
                vl_vdp_plane(vl, base_a, pw, ph, st->hsa, (int)(st->vsa & 0x3FF), ox, oy);
            }
        }
        /* the sprites */
        vl_batch(vl, 2, sprites_wide ? 0 : x_in0, sprites_wide ? ey0 : y0, sprites_wide ? vl->w : x_in1, sprites_wide ? ey1 : y1);
        vl_vdp_sprites(vl, (uint32_t)(rg[5] & 0x7E) << 9, ox, oy);
    }
    if (vl->nbatches >= VL_MAX_BATCHES || vl->ncells >= VL_MAX_CELLS) { sl_why = "full"; return 0; }
    /* the port's own pictures over the VDP's (vdp_overlay: the title art) */
    {
        static uint32_t keyed[8][320 * VDP_HEIGHT];
        static const uint8_t *was_px[8];
        static uint32_t was_sum[8], gen[8];
        int i;
        for (i = 0; i < 8; i++) {
            int x, y, w, h, n;
            const uint8_t *px;
            const uint32_t *rgb;
            uint32_t sum = 0;
            if (!vdp_overlay_get(i, &x, &y, &w, &h, &px, &rgb)) continue;
            if (w > 320 || h > VDP_HEIGHT || w <= 0 || h <= 0) { sl_why = "overlay"; return 0; }
            for (n = 0; n < 16; n++) sum = sum * 31u + rgb[n];
            if (was_px[i] != px || was_sum[i] != sum) {
                for (n = 0; n < w * h; n++) keyed[i][n] = px[n] ? 0xFF000000u | rgb[px[n] & 15] : 0;
                was_px[i] = px;
                was_sum[i] = sum;
                gen[i]++;
            }
            n = vl->noverlays;
            ovl_stamp = gen[i] * 16u + (uint32_t)i + 1;
            vl_overlay_clip(vl, ox + x, oy + y, w, h, keyed[i], w);
            ovl_stamp = 0;
            if (vl->noverlays > n) vl->overlays[vl->noverlays - 1].keyed = 1;
        }
    }
    return 1;
}

/* ---- the widened screens as lists ----
 * How the outside is filled (gameview's border or pattern, build_extended_screen)
 * is worked out from the picture as before, but only when the screen's tiles,
 * planes or palette change (never just for a scroll): the background is then drawn
 * on the CPU once. From that:
 *  - a pattern: the planes it comes from are drawn wide, when each repeats in its
 *    own map with the pattern's period (then that is the same picture);
 *  - a border: the border's cells copied out, block by block (the planes scrolled
 *    by whole cells: the blocks are then cells);
 *  - the map: the stars, kept as a picture, around it.
 * Anything else: drawn the usual way. */
static struct { int valid, b, px, py, fill; uint32_t key; } ext_dec;

static uint32_t ext_key(void)
{
    /* (the planes' maps and how they're set: not the tiles' pixels, which change
     * with a twinkling star, nor the scroll) */
    static const int psz[4] = {32, 64, 32, 128};
    static const int regs[] = {2, 3, 4, 12, 16, 17, 18};
    uint32_t h = 2166136261u, bases[3];
    int i, k, n = psz[vdp_reg[16] & 3] * psz[(vdp_reg[16] >> 4) & 3] * 2;
    h = (h ^ (uint32_t)(pic_w * 1000 + pic_h)) * 16777619u;
    for (i = 0; i < (int)(sizeof regs / sizeof regs[0]); i++) h = (h ^ vdp_reg[regs[i]]) * 16777619u;
    bases[0] = (uint32_t)(vdp_reg[2] & 0x38) << 10;
    bases[1] = (uint32_t)(vdp_reg[4] & 7) << 13;
    bases[2] = (uint32_t)(vdp_reg[3] & 0x3C) << 10;
    for (k = 0; k < 3; k++) {
        const uint32_t *w = (const uint32_t *)&vdp_vram[bases[k] & 0xFFFC];
        int words = (k == 2 ? 64 * 28 * 2 : n) / 4;
        if ((bases[k] & 0xFFFC) + (uint32_t)words * 4 > 0x10000) words = (int)((0x10000 - (bases[k] & 0xFFFC)) / 4);
        for (i = 0; i < words; i++) h = (h ^ w[i]) * 16777619u;
    }
    return h;
}

static void ext_decide(void)
{
    int line, pl;
    uint32_t key = ext_key();
    if (ext_dec.valid && ext_dec.key == key) return;
    ext_dec.valid = 1;
    ext_dec.key = key;
    for (line = 0; line < VDP_HEIGHT; line++) bg_line(line);
    hash_blocks(bg_pic);
    ext_dec.b = find_border();
    ext_dec.px = ext_dec.py = 0;
    ext_dec.fill = -1;
    if (!ext_dec.b) {
        ext_dec.px = find_period(0);
        ext_dec.py = find_period(1);
        for (pl = 0; pl < 2 && !(ext_dec.px && ext_dec.py); pl++) {
            hash_blocks(plane_pic[pl]);
            ext_dec.px = find_period(0);
            ext_dec.py = find_period(1);
            if (ext_dec.px && ext_dec.py) ext_dec.fill = pl;
        }
    }
}

/* Does the plane at base (pw x ph cells) repeat every px x py cells in its map? */
static int plane_repeats(uint32_t base, int pw, int ph, int px, int py)
{
    int x, y;
    if (pw % px || ph % py) return 0;
    for (y = 0; y < ph; y++)
        for (x = 0; x < pw; x++) {
            uint32_t a = (base + (uint32_t)(y * pw + x) * 2) & 0xFFFF;
            uint32_t r = (base + (uint32_t)(y * pw + (x + px) % pw) * 2) & 0xFFFF;
            uint32_t d = (base + (uint32_t)(((y + py) % ph) * pw + x) * 2) & 0xFFFF;
            if (vdp_vram[a] != vdp_vram[r] || vdp_vram[a + 1] != vdp_vram[r + 1] ||
                vdp_vram[a] != vdp_vram[d] || vdp_vram[a + 1] != vdp_vram[d + 1]) return 0;
        }
    return 1;
}

/* the border's cells (or the pattern's: pattern set) copied out over the list
 * (outside the original screen); only_plane: -1 both planes, else just that one
 * (0 A, 1 B) */
static int ext_copy_cells(ViewList *vl, int ox, int oy, int pattern, int only_plane)
{
    static const int psz[4] = {32, 64, 32, 128};
    const VdpLineState *st = &vdp_lines[0];
    int pw = psz[vdp_reg[16] & 3], ph = psz[(vdp_reg[16] >> 4) & 3], b = ext_dec.b, span = BW - 2 * b, layer;
    int px = ext_dec.px, py = ext_dec.py;
    uint32_t base_a = (uint32_t)(vdp_reg[2] & 0x38) << 10, base_b = (uint32_t)(vdp_reg[4] & 7) << 13;
    int hs[2], vs[2];
    hs[0] = st->hsa; hs[1] = st->hsb; vs[0] = st->vsa & 0x3FF; vs[1] = st->vsb & 0x3FF;
    if ((hs[0] | hs[1] | vs[0] | vs[1]) & 7) { sl_why = "border-scroll"; return 0; }
    if (vdp_reg[17] & 0x1F) { sl_why = "border-window"; return 0; }                           /* (a window beside: not here) */
    for (layer = 0; layer < 2; layer++) {
        /* layer 0: plane B, 1: plane A or the window */
        int bx, by, bx0 = floor_div(-ox, 8), bx1 = floor_div(vl->w - ox - 1, 8), by0 = floor_div(-oy, 8), by1 = floor_div(vl->h - oy - 1, 8);
        int pi = layer ? 0 : 1;
        if (only_plane >= 0 && only_plane != pi) continue;
        vl_batch(vl, layer, 0, 0, vl->w, vl->h);
        for (by = by0; by <= by1; by++)
            for (bx = bx0; bx <= bx1; bx++) {
                uint32_t h, ea, e;
                int sbx, sby, wv, in_win;
                if (bx >= 0 && bx < BW && by >= 0 && by < BH) continue;   /* (the original screen) */
                if (pattern) {
                    sbx = ((bx % px) + px) % px;
                    sby = ((by % py) + py) % py;
                } else {
                    h = mix((uint32_t)bx * 2654435761u ^ (uint32_t)by * 2246822519u);
                    sbx = b + (int)(h % (uint32_t)span);
                    sby = (int)((h >> 16) % (uint32_t)b);
                    if ((h >> 8) & 1) sby = BH - 1 - sby;
                }
                wv = (vdp_reg[18] & 0x1F) * 8;
                in_win = (vdp_reg[18] & 0x80) ? sby * 8 >= wv : sby * 8 < wv;
                if (layer && in_win && only_plane < 0) {
                    ea = ((((uint32_t)(vdp_reg[3] & 0x3C) << 10)) + (uint32_t)(sby * 64 + sbx) * 2) & 0xFFFF;
                } else {
                    int tx = ((sbx * 8 - hs[pi]) >> 3) & (pw - 1), ty = ((sby * 8 + vs[pi]) >> 3) & (ph - 1);
                    ea = ((pi ? base_b : base_a) + (uint32_t)(ty * pw + tx) * 2) & 0xFFFF;
                }
                e = ((uint32_t)vdp_vram[ea] << 8) | vdp_vram[ea + 1];
                vl_cell(vl, bx * 8 + ox, by * 8 + oy, e);
            }
    }
    return 1;
}

/* the pattern's tile (the top left px x py blocks of the screen, the fill's
 * planes as now), repeated in four strips around the original screen */
static uint32_t pattern_tile[128 * 128];
/* the scroll plane_row draws with now (the scroll table's first entries, VSRAM) */
static uint32_t scroll_now_sig(void)
{
    uint32_t hs = (uint32_t)(vdp_reg[13] & 0x3F) << 10, h = 2166136261u;
    int i, n = (vdp_reg[11] & 3) ? 224 * 4 : 4;
    for (i = 0; i < n; i++) h = (h ^ vdp_vram[(hs + (uint32_t)i) & 0xFFFF]) * 16777619u;
    for (i = 0; i < 4; i++) h = (h ^ (uint32_t)vdp_vsram[i]) * 16777619u;
    return h;
}

/* what planes A and B show (their maps and the tiles there, the scroll of the top
 * line, the palette): the same, the same picture */
static uint32_t planes_sig(void)
{
    static const int sizes[4] = {32, 64, 32, 128};
    int k, n = sizes[vdp_reg[16] & 3] * sizes[(vdp_reg[16] >> 4) & 3], i;
    uint32_t h = 2166136261u;
    const VdpLineState *st = &vdp_lines[0];
    uint32_t bases[2] = {(uint32_t)(vdp_reg[2] & 0x38) << 10, (uint32_t)(vdp_reg[4] & 7) << 13};
    if (n > 4096) n = 4096;
    for (k = 0; k < 2; k++)
        for (i = 0; i < n; i++) {
            uint32_t ea = (bases[k] + (uint32_t)i * 2) & 0xFFFF;
            uint32_t e = ((uint32_t)vdp_vram[ea] << 8) | vdp_vram[ea + 1];
            h = (h ^ e) * 16777619u;
            h = (h ^ vdp_tile_ver[e & 0x7FF]) * 16777619u;
        }
    (void)st;
    h = (h ^ scroll_now_sig()) * 16777619u;
    h = (h ^ vdp_cram_version ^ ((uint32_t)vdp_reg[7] << 16) ^ ((uint32_t)vdp_reg[16] << 24) ^ ((uint32_t)vdp_reg[11] << 8)) * 16777619u;
    return h;
}

static int ext_pattern_overlays(ViewList *vl, int ox, int oy)
{
    int tw = ext_dec.px * 8, th = ext_dec.py * 8, x, y, i, n;
    uint32_t backdrop = vdp_palette_rgb(vdp_reg[7] & 0x3F);
    static uint8_t a[128], b[128];
    static uint32_t was_sig, gen;
    static int was_tw, was_th, was_fill = -1;
    uint32_t sig;
    if (tw > 128 || th > 128) { sl_why = "pattern-size"; return 0; }
    /* (the pattern's tile drawn again only when what it is drawn from changed) */
    sig = planes_sig();
    if (sig != was_sig || tw != was_tw || th != was_th || ext_dec.fill != was_fill) {
        was_sig = sig; was_tw = tw; was_th = th; was_fill = ext_dec.fill;
        gen++;
    } else goto drawn;
    for (y = 0; y < th; y++) {
        memset(a, 0, sizeof a);
        memset(b, 0, sizeof b);
        if (ext_dec.fill != 1) plane_row(a, 0, 0, tw, y, 0);
        if (ext_dec.fill != 0) plane_row(b, 0, 0, tw, y, 1);
        for (x = 0; x < tw; x++) {
            uint8_t pa = a[x], pb = b[x], c;
            if ((pa & (PIX_OPAQUE | PIX_PRIO)) == (PIX_OPAQUE | PIX_PRIO)) c = pa;
            else if ((pb & (PIX_OPAQUE | PIX_PRIO)) == (PIX_OPAQUE | PIX_PRIO)) c = pb;
            else if (pa & PIX_OPAQUE) c = pa;
            else if (pb & PIX_OPAQUE) c = pb;
            else { pattern_tile[y * 128 + x] = backdrop; continue; }
            pattern_tile[y * 128 + x] = vdp_palette_rgb(c & 0x3F);
        }
    }
drawn:
    ovl_stamp = 0x20000000u | (gen & 0x0FFFFFFFu);
    for (i = 0; i < 4; i++) {
        int rx = i == 3 ? ox + 320 : 0, ry = i == 0 ? 0 : i == 1 ? oy + VDP_HEIGHT : oy;
        int rw = i < 2 ? vl->w : i == 2 ? ox : vl->w - ox - 320, rh = i == 0 ? oy : i == 1 ? vl->h - oy - VDP_HEIGHT : VDP_HEIGHT;
        n = vl->noverlays;
        vl_overlay_clip(vl, rx, ry, rw, rh, pattern_tile, 128);
        if (vl->noverlays > n) {
            VlOverlay *o = &vl->overlays[vl->noverlays - 1];
            o->tile_w = tw;
            o->tile_h = th;
            o->tx0 = ox;
            o->ty0 = oy;
        }
    }
    ovl_stamp = 0;
    return 1;
}

/* the border carried outwards when the planes are scrolled part of a cell (the
 * screen's border rows drawn as the software way does, bg_line, and the outside
 * filled from them - again only when they change), in four strips */
static uint32_t border_out[GAMEVIEW_MAX_W * GAMEVIEW_MAX_H];
static void outside_strips(ViewList *vl, int ox, int oy, const uint32_t *pic)
{
    vl_overlay_clip(vl, 0, 0, vl->w, oy, pic, GAMEVIEW_MAX_W);
    vl_overlay_clip(vl, 0, oy + VDP_HEIGHT, vl->w, vl->h - oy - VDP_HEIGHT, pic + (size_t)(oy + VDP_HEIGHT) * GAMEVIEW_MAX_W, GAMEVIEW_MAX_W);
    vl_overlay_clip(vl, 0, oy, ox, VDP_HEIGHT, pic + (size_t)oy * GAMEVIEW_MAX_W, GAMEVIEW_MAX_W);
    vl_overlay_clip(vl, ox + 320, oy, vl->w - ox - 320, VDP_HEIGHT, pic + (size_t)oy * GAMEVIEW_MAX_W + ox + 320, GAMEVIEW_MAX_W);
}

/* what the border rows (b cells at the top and bottom) are drawn from: the map
 * rows they show of planes A and B and the window, and the tiles there */
static uint32_t border_rows_sig(int b)
{
    static const int sizes[4] = {32, 64, 0, 128};
    int pw = sizes[vdp_reg[16] & 3], ph = sizes[(vdp_reg[16] >> 4) & 3], k, y, c;
    uint32_t h = 2166136261u;
    uint32_t bases[3] = {(uint32_t)(vdp_reg[2] & 0x38) << 10, (uint32_t)(vdp_reg[4] & 7) << 13, (uint32_t)(vdp_reg[3] & 0x3E) << 10};
    if (!pw) pw = 32;
    if (!ph) ph = 32;
    for (k = 0; k < 3; k++) {
        int last_row = -1, cols = k == 2 ? 64 : pw, rows = k == 2 ? 32 : ph;
        for (y = 0; y < VDP_HEIGHT; y++) {
            int row;
            if (y == b * 8) y = VDP_HEIGHT - b * 8;
            row = k == 2 ? y >> 3 : (((y + vdp_vsram[k == 1]) >> 3) & (rows - 1));   /* (as plane_row: VSRAM now) */
            if (row == last_row) continue;
            last_row = row;
            for (c = 0; c < cols; c++) {
                uint32_t ea = (bases[k] + (uint32_t)(row * cols + c) * 2) & 0xFFFF;
                uint32_t e = ((uint32_t)vdp_vram[ea] << 8) | vdp_vram[ea + 1];
                h = (h ^ e) * 16777619u;
                h = (h ^ vdp_tile_ver[e & 0x7FF]) * 16777619u;
            }
        }
    }
    return h;
}

static int ext_border_overlays(ViewList *vl, int ox, int oy)
{
    static uint32_t last_sum, last_key, last_vram, gen;
    static uint16_t last_cram;
    static uint8_t last_reg[32];
    static VdpLineState last_top, last_bottom;
    static int have;
    int b = ext_dec.b, span = BW - 2 * b, y, x;
    uint32_t sum = 2166136261u, key = (uint32_t)(vl->w * 1000 + vl->h) ^ ((uint32_t)b << 24);
    uint32_t sig = border_rows_sig(b) ^ scroll_now_sig();
    /* (nothing the border rows are drawn from changed: as last time) */
    if (have && key == last_key && sig == last_vram && vdp_cram_version == last_cram &&
        vdp_lines_alike(&vdp_lines[0], &last_top) && vdp_lines_alike(&vdp_lines[VDP_HEIGHT - 1], &last_bottom) &&
        !memcmp(vdp_reg, last_reg, 15) && !memcmp(vdp_reg + 16, last_reg + 16, 3)) {   /* (not 15, the step; 19-23, DMA) */
        ovl_stamp = 0x80000000u | gen;
        outside_strips(vl, ox, oy, border_out);
        ovl_stamp = 0;
        return 1;
    }
    have = 1;
    last_vram = sig;
    last_cram = vdp_cram_version;
    last_top = vdp_lines[0];
    memcpy(last_reg, vdp_reg, sizeof last_reg);
    last_bottom = vdp_lines[VDP_HEIGHT - 1];
    for (y = 0; y < b * 8; y++) { bg_line(y); bg_line(VDP_HEIGHT - 1 - y); }
    for (y = 0; y < b * 8; y++)
        for (x = 0; x < 320; x++) {
            sum = (sum ^ bg_pic[y][x]) * 16777619u;
            sum = (sum ^ bg_pic[VDP_HEIGHT - 1 - y][x]) * 16777619u;
        }
    if (sum != last_sum || key != last_key) {
        gen++;
        for (y = 0; y < vl->h; y++) {
            int ry = y - oy;
            uint32_t *o = border_out + (size_t)y * GAMEVIEW_MAX_W;
            for (x = 0; x < vl->w; x++) {
                int rx = x - ox, bx, by, sbx, sby;
                uint32_t h;
                if (rx >= 0 && rx < 320 && ry >= 0 && ry < VDP_HEIGHT) { x = ox + 319; continue; }
                bx = floor_div(rx, 8);
                by = floor_div(ry, 8);
                h = mix((uint32_t)bx * 2654435761u ^ (uint32_t)by * 2246822519u);
                sbx = b + (int)(h % (uint32_t)span);
                sby = (int)((h >> 16) % (uint32_t)b);
                if ((h >> 8) & 1) sby = BH - 1 - sby;
                o[x] = bg_pic[sby * 8 + (ry - by * 8)][sbx * 8 + (rx - bx * 8)];
            }
        }
        last_sum = sum;
        last_key = key;
    }
    ovl_stamp = 0x80000000u | gen;
    outside_strips(vl, ox, oy, border_out);
    ovl_stamp = 0;
    return 1;
}

/* the map's stars around it: kept as a picture, shown in four strips */
static int stars_for_w, stars_for_h;
static uint32_t star_out[GAMEVIEW_MAX_W * GAMEVIEW_MAX_H];
static void ext_star_overlays(ViewList *vl, int ox, int oy)
{
    int x, y;
    if (stars_for_w != vl->w || stars_for_h != vl->h) {
        for (y = 0; y < vl->h; y++)
            for (x = 0; x < vl->w; x++)
                star_out[(size_t)y * GAMEVIEW_MAX_W + x] = star_pic[(y - oy) & 255][(x - ox) & 511];
        stars_for_w = vl->w;
        stars_for_h = vl->h;
    }
    ovl_stamp = 0x40000000u | (uint32_t)(vl->w * 1000 + vl->h);
    vl_overlay_clip(vl, 0, 0, vl->w, oy, star_out, GAMEVIEW_MAX_W);
    vl_overlay_clip(vl, 0, oy + VDP_HEIGHT, vl->w, vl->h - oy - VDP_HEIGHT,
                    star_out + (size_t)(oy + VDP_HEIGHT) * GAMEVIEW_MAX_W, GAMEVIEW_MAX_W);
    vl_overlay_clip(vl, 0, oy, ox, VDP_HEIGHT, star_out + (size_t)oy * GAMEVIEW_MAX_W, GAMEVIEW_MAX_W);
    vl_overlay_clip(vl, ox + 320, oy, vl->w - ox - 320, VDP_HEIGHT,
                    star_out + (size_t)oy * GAMEVIEW_MAX_W + ox + 320, GAMEVIEW_MAX_W);
    ovl_stamp = 0;
}

/* (tests, TJE_VLCHECK) the list drawn in software against the picture drawn the
 * usual way */
int g_sl_checked, g_sl_bad;
static void screen_list_check(void)
{
    static uint32_t ref[GAMEVIEW_MAX_W * GAMEVIEW_MAX_H];
    const uint32_t *pic;
    int pitch, x, y;
    long bad = 0;
    if (sl_into->w == 320 && sl_into->h == VDP_HEIGHT && (frame_ok == 0 || pic_w != 320)) { pic = vdp_framebuffer(); pitch = VDP_FB_PITCH; }
    else { pic = picture; pitch = GAMEVIEW_MAX_W; }
    if (frame_ok != 0 && (sl_into->w != pic_w || sl_into->h != pic_h) && pic == picture) return;
    viewlist_render_soft(sl_into, ref, GAMEVIEW_MAX_W);
    for (y = 0; y < sl_into->h; y++)
        for (x = 0; x < sl_into->w; x++)
            bad += (ref[(size_t)y * GAMEVIEW_MAX_W + x] & 0xFFFFFF) != (pic[(size_t)y * pitch + x] & 0xFFFFFF);
    g_sl_checked++;
    if (bad) {
        g_sl_bad++;
        if (getenv("TJE_SLDUMP") && (g_sl_bad <= 12 || (getenv("TJE_SLDUMPFROM") && g_frame >= (uint32_t)atoi(getenv("TJE_SLDUMPFROM")) && g_frame < (uint32_t)atoi(getenv("TJE_SLDUMPFROM")) + 3))) {
            char path[512];
            FILE *f;
            int i;
            for (i = 0; i < 2; i++) {
                snprintf(path, sizeof path, "%s_%u_%s.ppm", getenv("TJE_SLDUMP"), g_frame, i ? "list" : "pic");
                f = fopen(path, "wb");
                if (!f) continue;
                fprintf(f, "P6 %d %d 255\n", sl_into->w, sl_into->h);
                for (y = 0; y < sl_into->h; y++)
                    for (x = 0; x < sl_into->w; x++) {
                        uint32_t c = i ? ref[(size_t)y * GAMEVIEW_MAX_W + x] : pic[(size_t)y * pitch + x];
                        fputc((int)((c >> 16) & 255), f); fputc((int)((c >> 8) & 255), f); fputc((int)(c & 255), f);
                    }
                fclose(f);
            }
        }
        fprintf(stderr, "slcheck: frame %u (kind %d, %dx%d): %ld pixels differ\n", g_frame, frame_ok, sl_into->w, sl_into->h, bad);
    }
    if (g_sl_checked % 500 == 0) fprintf(stderr, "slcheck: %d screens, %d differ\n", g_sl_checked, g_sl_bad);
}

/* This frame's screen (frame_ok 0, 2 or 3) as a list in vlist. 0: it can't be. */
static int build_screen_frame(void)
{
    static const int psz[4] = {32, 64, 32, 128};
    if (frame_ok == 0) {
        vl_begin(sl_into, 320, VDP_HEIGHT);
        return build_screen_list(sl_into, 0, 0, -1, 0);
    }
    vl_begin(sl_into, pic_w, pic_h);
    if (frame_ok == 2) return build_screen_list(sl_into, off_x, off_y, g_gameview_extend_screen == 2 ? 0 : 1, 1);
    /* frame_ok 3 */
    if (map_mode) {
        if (!build_screen_list(sl_into, off_x, off_y, -1, 0)) return 0;
        PROF_PUSH(PROF_OVL); ext_star_overlays(sl_into, off_x, off_y); PROF_POP();
        return 1;
    }
    ext_decide();
    if (ext_dec.b) {
        sl_why = "";
        if (!ext_copy_cells(sl_into, off_x, off_y, 0, -1)) {
            /* (scrolled part of a cell: the border as pictures) */
            vl_begin(sl_into, pic_w, pic_h);
            if (!build_screen_list(sl_into, off_x, off_y, -1, 0)) return 0;
            { int r; PROF_PUSH(PROF_OVL); r = ext_border_overlays(sl_into, off_x, off_y); PROF_POP(); return r; }
        }
        return build_screen_list(sl_into, off_x, off_y, -1, 0);
    }
    if (ext_dec.px && ext_dec.py) {
        int pw = psz[vdp_reg[16] & 3], ph = psz[(vdp_reg[16] >> 4) & 3];
        int ra = plane_repeats((uint32_t)(vdp_reg[2] & 0x38) << 10, pw, ph, ext_dec.px, ext_dec.py);
        int rb = plane_repeats((uint32_t)(vdp_reg[4] & 7) << 13, pw, ph, ext_dec.px, ext_dec.py);
        int wide = ext_dec.fill == 0 ? (ra ? 0 : -9) : ext_dec.fill == 1 ? (rb ? 1 : -9) : (ra && rb ? 2 : -9);
        if (wide < -1) {
            /* (the planes don't repeat so: the pattern's tile, drawn here as the
             * screen shows it now, repeated around the original screen) */
            if (!build_screen_list(sl_into, off_x, off_y, -1, 0)) return 0;
            { int r; PROF_PUSH(PROF_OVL); r = ext_pattern_overlays(sl_into, off_x, off_y); PROF_POP(); return r; }
        }
        if (vdp_reg[17] & 0x1F) { sl_why = "pattern-window"; return 0; }
        return build_screen_list(sl_into, off_x, off_y, wide, 0);
    }
    /* (no outside: shown at its own size) */
    vl_begin(sl_into, 320, VDP_HEIGHT);
    return build_screen_list(sl_into, 0, 0, -1, 0);
}

/* The status bar (window rows from..to-1 of the original screen) with its icon
 * sprites (entries 11-22 where they lie in those rows), into out (320 wide). */
static void render_status_bar(uint32_t *out, int pitch, int from, int to)
{
    static uint8_t a[320], sp[320];
    uint32_t backdrop = vdp_palette_rgb(vdp_reg[7] & 0x3F);
    int line, x, saved = npieces;
    /* (only the bar's own sprites: the view's objects, still in the list, are in
     * the original screen's coordinates and would land in the bar from its top rows) */
    npieces = 0;
    view_screen_sprites(11, 22, from, to - from, 0, 0, 320, to - from);
    for (line = from; line < to; line++) {
        uint32_t *o = out + (size_t)(line - from) * pitch;
        window_line(a, 0, line);
        memset(sp, 0, sizeof sp);
        pieces_row(sp, 0, 0, 320, line - from);
        for (x = 0; x < 320; x++) {
            uint8_t s = sp[x], pa = a[x], c;
            if ((s & (PIX_OPAQUE | PIX_PRIO)) == (PIX_OPAQUE | PIX_PRIO)) c = s;
            else if ((pa & (PIX_OPAQUE | PIX_PRIO)) == (PIX_OPAQUE | PIX_PRIO)) c = pa;
            else if (s & PIX_OPAQUE) c = s;
            else if (pa & PIX_OPAQUE) c = pa;
            else { o[x] = backdrop; continue; }
            o[x] = vdp_palette_rgb(c & 0x3F);
        }
    }
    npieces = saved;
}

/* One 320-pixel row of the original screen into picture row r, centred and
 * clipped to the picture (which may be narrower or shorter than 320x224). */
static void paste_centred_row_in(int r, const uint32_t *src, int r_from, int r_to)
{
    int ox = (pic_w - 320) / 2, x0 = ox < 0 ? -ox : 0, n = 320 - x0;
    if (r < r_from || r >= r_to || r < 0 || r >= pic_h) return;
    if (n > pic_w) n = pic_w;
    memcpy(picture + (size_t)r * GAMEVIEW_MAX_W + (ox < 0 ? 0 : ox), src + x0, sizeof(uint32_t) * (size_t)n);
}

static void paste_centred_row(int r, const uint32_t *src) { paste_centred_row_in(r, src, 0, pic_h); }

/* Split screen: a half under its player's menu shows the menu (the window lines as
 * the VDP drew them, with its sprites) where the original half is in the view, and
 * that player's level around it, as the present list does in one-player play. */
static void paste_split_menu(int k, int area_top, int area_h)
{
    int line, first = k * (VDP_HEIGHT / 2), oy = (area_h - VDP_HEIGHT / 2) / 2;
    for (line = first; line < first + VDP_HEIGHT / 2; line++)
        if (!window_row_filler(line / 8))   /* (plain rows of the window: the level shows there) */
            paste_centred_row_in(area_top + oy + line - first, top_window_lines[line], area_top, area_top + area_h);
}

static int views_wide(void)
{
    return (want_w != 320 || want_h != VDP_HEIGHT);
}

/* One half of the split picture (k: 0 ToeJam's, 1 Earl's) into picture rows
 * area_top.. area_top+area_h-1:
 *  - the player's view, with the player's menu over it if open;
 *  - held (the half's plane A shows something the floor layout no longer does:
 *    falling): the last view drawn of this half around the VDP's half, moved with
 *    the camera if it moves;
 *  - blank (the split being set up): the VDP's half on its own colour. */
static uint32_t held_pic[GAMEVIEW_MAX_W * GAMEVIEW_MAX_H];
static struct { int ok, cam_x, cam_y, top, h, w, frames; } held_half[2];

static void split_half(int k, int area_top, int area_h)
{
    uint32_t *dst = picture + (size_t)area_top * GAMEVIEW_MAX_W;
    int first = k * (VDP_HEIGHT / 2), oy = (area_h - VDP_HEIGHT / 2) / 2, x, y, line;
    if (!split_blank && !half_held[k]) {
        view_skip_half_hud = split_half_covered(k);
        gameview_render_view(k, region[k].cam_x, region[k].cam_y, k, VDP_HEIGHT / 2, dst, pic_w, area_h,
                             GAMEVIEW_MAX_W, 0);
        if (view_skip_half_hud) paste_split_menu(k, area_top, area_h);
        view_skip_half_hud = 0;
        for (y = 0; y < area_h; y++)
            memcpy(held_pic + (size_t)(area_top + y) * GAMEVIEW_MAX_W, dst + (size_t)y * GAMEVIEW_MAX_W,
                   sizeof(uint32_t) * (size_t)pic_w);
        held_half[k].ok = 1;
        held_half[k].cam_x = region[k].cam_x;
        held_half[k].cam_y = region[k].cam_y;
        held_half[k].top = area_top;
        held_half[k].h = area_h;
        held_half[k].w = pic_w;
        held_half[k].frames = 0;
        return;
    }
    if (!split_blank && held_half[k].ok && held_half[k].top == area_top && held_half[k].h == area_h &&
        held_half[k].w == pic_w && held_half[k].frames++ < 600) {
        int dx = region[k].cam_x - held_half[k].cam_x, dy = region[k].cam_y - held_half[k].cam_y;
        uint32_t fill = vdp_palette_rgb(vdp_reg[7] & 0x3F);
        for (y = 0; y < area_h; y++) {
            uint32_t *o = dst + (size_t)y * GAMEVIEW_MAX_W;
            int sy = y + dy;
            for (x = 0; x < pic_w; x++) {
                int sx = x + dx;
                o[x] = (sy >= 0 && sy < area_h && sx >= 0 && sx < pic_w)
                     ? held_pic[(size_t)(area_top + sy) * GAMEVIEW_MAX_W + (size_t)sx] : fill;
            }
        }
    } else {
        /* (the colour the VDP's half shows at its left edge, half way down) */
        uint32_t fill = top_window_lines[first + VDP_HEIGHT / 4][0];
        for (y = 0; y < area_h; y++)
            for (x = 0; x < pic_w; x++) dst[(size_t)y * GAMEVIEW_MAX_W + x] = fill;
    }
    /* the VDP's half, without the band across the middle (the divider is ours), and
     * when held, without the rows of its level number / life bar (the held picture
     * has them in its corners already) */
    {
        int skip_to = first;
        if (!split_blank) {
            uint32_t base = (uint32_t)(vdp_reg[5] & 0x7E) << 9;
            int e;
            for (e = 11; e <= 22; e++) {
                const uint8_t *en = &vdp_vram[(base + (uint32_t)e * 8) & 0xFFFF];
                int sy = (((en[0] << 8) | en[1]) & 0x3FF) - 128, vs = (en[2] & 3) + 1;
                if (sy >= first && sy < first + VDP_HEIGHT / 4 && sy + vs * 8 > skip_to) skip_to = sy + vs * 8;
            }
        }
        for (line = skip_to; line < first + VDP_HEIGHT / 2; line++)
            if (line < VDP_HEIGHT / 2 - 8 || line >= VDP_HEIGHT / 2 + 8)
                paste_centred_row_in(area_top + oy + line - first, top_window_lines[line], area_top, area_top + area_h);
    }
}

/* The gameplay picture of this frame drawn with the per-player views (one view, or
 * two stacked for split screen) at pic_w x pic_h. */
static void render_views_picture(void)
{
    int split = nregions == 2;
    if (vl_check < 0) vl_check = getenv("TJE_VLCHECK") != NULL;
    views_setup_cameras();
    layout_copy_on = 1;
    if (!split && ((g_gv_use_list && !g_gv_windows) || vl_check)) {
        /* the view as a list for the GPU (with the same present list and status
         * bar over it) */
        int top = (win_from == 0 && win_to > 0) ? win_to : 0, bar_h;
        static uint32_t bar[VDP_HEIGHT][320];
        vl_begin(&vlist, pic_w, pic_h);
        build_view_list(&vlist, 0, region[0].cam_x, region[0].cam_y, 0, VDP_HEIGHT, 0, 0, pic_w, pic_h, 1);
        if (top) vl_overlay(&vlist, (pic_w - 320) / 2, (pic_h - VDP_HEIGHT) / 2, 320, top, &top_window_lines[0][0], 320);
        if (win_to == VDP_HEIGHT && win_from > 0) {
            bar_h = hud_end - win_from;
            if (bar_h > 0 && bar_h <= VDP_HEIGHT) vl_status_bar(&vlist, win_from, hud_end, (pic_w - 320) / 2, pic_h - bar_h);
            (void)bar;
        }
        if (g_gv_use_list && !g_gv_windows) { list_ready = 1; layout_copy_on = 0; return; }
    }
    if (!split) {
        int bar_h = 0;
        int top = (win_from == 0 && win_to > 0) ? win_to : 0, line;
        view_top_window = top;
        gameview_render_view(0, region[0].cam_x, region[0].cam_y, 0, VDP_HEIGHT, picture, pic_w, pic_h,
                             GAMEVIEW_MAX_W, 1);
        view_top_window = 0;
        /* the present list where the original has it, as the VDP drew its lines */
        for (line = 0; line < top; line++)
            paste_centred_row(line + (pic_h - VDP_HEIGHT) / 2, top_window_lines[line]);
        /* the status bar, flush with the bottom edge, centred */
        if (win_to == VDP_HEIGHT && win_from > 0) {
            bar_h = hud_end - win_from;
            if (bar_h > 0 && bar_h <= VDP_HEIGHT) {
                static uint32_t bar[VDP_HEIGHT][320];
                render_status_bar(&bar[0][0], 320, win_from, hud_end);
                for (line = 0; line < bar_h; line++) paste_centred_row(pic_h - bar_h + line, bar[line]);
            }
        }
        if (vl_check) {
            /* (tests: the list, drawn as a GPU must, against the picture) */
            static uint32_t chk[GAMEVIEW_MAX_W * GAMEVIEW_MAX_H];
            long bad = 0;
            int x, y;
            viewlist_render_soft(&vlist, chk, GAMEVIEW_MAX_W);
            for (y = 0; y < pic_h; y++)
                for (x = 0; x < pic_w; x++)
                    if ((chk[(size_t)y * GAMEVIEW_MAX_W + x] & 0xFFFFFF) != (picture[(size_t)y * GAMEVIEW_MAX_W + x] & 0xFFFFFF)) bad++;
            vl_checked++;
            if (vl_checked % 500 == 0) fprintf(stderr, "vlcheck: %d views, %d differ\n", vl_checked, vl_bad_frames);
            if (bad) {
                vl_bad_frames++;
                if (vl_bad_frames <= 3 && getenv("TJE_VLCHECK_DUMP")) {
                    char path[300];
                    FILE *f;
                    snprintf(path, sizeof path, "%s_%d.ppm", getenv("TJE_VLCHECK_DUMP"), vl_bad_frames);
                    f = fopen(path, "wb");
                    if (f) {
                        fprintf(f, "P6\n%d %d\n255\n", pic_w, pic_h * 2);
                        for (y = 0; y < pic_h * 2; y++)
                            for (x = 0; x < pic_w; x++) {
                                uint32_t c = y < pic_h ? picture[(size_t)y * GAMEVIEW_MAX_W + x] : chk[(size_t)(y - pic_h) * GAMEVIEW_MAX_W + x];
                                fputc((int)(c >> 16) & 255, f); fputc((int)(c >> 8) & 255, f); fputc((int)c & 255, f);
                            }
                        fclose(f);
                    }
                }
                fprintf(stderr, "vlcheck: %ld pixels differ\n", bad);
            }
        }
    } else {
        /* split screen: ToeJam's half above Earl's, the divider band between */
        int half = (pic_h - 8) / 2, k, x;
        uint32_t *div;
        g_gv_split_menu = split_half_covered(0) | split_half_covered(1) << 1;
        split_half(0, 0, half);
        split_half(1, half + 8, pic_h - half - 8);
        /* the divider: the game's band tile, repeated across */
        div = picture + (size_t)half * GAMEVIEW_MAX_W;
        {
            uint32_t base = (uint32_t)(vdp_reg[5] & 0x7E) << 9;
            const uint8_t *en = &vdp_vram[(base + 8) & 0xFFFF];
            uint32_t attr = ((uint32_t)en[4] << 8) | en[5];
            for (k = 0; k < 8; k++) {
                uint32_t *o = div + (size_t)k * GAMEVIEW_MAX_W;
                for (x = 0; x < pic_w; x++) {
                    int c32 = x & 31, colx = c32 >> 3, odd = (x >> 5) & 1;
                    uint32_t at = odd ? (attr ^ 0x1800) : attr;    /* (every other sprite of the band flipped) */
                    uint8_t t8[8];
                    if (at & 0x0800) colx = 3 - colx;
                    tile_row(t8, (at & 0xF800) | ((at & 0x7FF) + (uint32_t)colx), k);
                    o[x] = (t8[x & 7] & PIX_OPAQUE) ? vdp_palette_rgb(t8[x & 7] & 0x3F) : vdp_palette_rgb(vdp_reg[7] & 0x3F);
                }
            }
        }
    }
    layout_copy_on = 0;
}

/* ==== One window per player (two-window mode; network play) ========================
 * Each player's own view, at any size, around a camera of its own that follows that
 * player as the one-player game's camera does (the player 160 px from the left of
 * the original screen and 128 down), whether the game has the players together or
 * split. Drawing only: the game keeps its own cameras. On top, like one-player play:
 * the player's menu (present list, shops) where the present list goes, and the
 * status bar at the bottom (the game's shared one) while the game shows it; in
 * split play, that player's level number and life bar in the top corners. */
static uint32_t win_pic[2][GAMEVIEW_MAX_W * GAMEVIEW_MAX_H];
static struct { int ok, w, h, cam_x, cam_y, level; } win_last[2];
static uint8_t win_layout[2][988];   /* the player's floor layout when their window was last drawn */

#define PLAYER_OBJ(p)   (0xFFA25Au + (uint32_t)(p) * 0x80u)
#define PLAYER_LIVES(p) (0xFFA248u + (uint32_t)(p))
#define MENU_FLAG(p)    (0xFF9364u + (uint32_t)(p))   /* VBLANK_FLAG_1 / HBLANK_FLAG_1: that player's menu is open */

static int player_in_game(int p) { return (int8_t)rd8(PLAYER_LIVES(p)) >= 0; }

/* Window row r (of out, w x h) gets original-screen row `line` as the VDP drew it,
 * with its x 0 at column x0; rows outside [r_from, r_to) are left alone. */
static void paste_line(uint32_t *out, int w, int h, int r, int x0, int line, int r_from, int r_to)
{
    int xs = x0 < 0 ? -x0 : 0, xd = x0 < 0 ? 0 : x0, n = 320 - xs;
    if (r < r_from || r >= r_to || r < 0 || r >= h || line < 0 || line >= VDP_HEIGHT) return;
    if (xd + n > w) n = w - xd;
    if (n > 0) memcpy(out + (size_t)r * GAMEVIEW_MAX_W + xd, top_window_lines[line] + xs, sizeof(uint32_t) * (size_t)n);
}

/* ---- the two-player status bar, built from game memory --------------------------
 * The bar the game draws while the players share the screen (DrawHUD, $B62A,
 * $AB9E, $AD6A, $B0AC, $B570), built here for any moment, split screen included
 * (where the game shows small per-half displays instead): the window rows 23-27
 * (frame from ROM $A82FC, rank names), and the status sprites (entries 11-22 of the
 * game's sprite table, their tiles as the game keeps them) where the game puts them
 * on the bar. The level number is the window's own player's. */
#define BAR_TOP     184                 /* original-screen line of window row 23 */
#define BAR_H       40
#define SPRITE_TABLE    0xFFB65Au       /* the game's copy of the sprite table */
#define PROMOTION(p)    (0xFFA250u + (uint32_t)(p))
#define ACTIVE_PRESENT(p) (0xFFDE50u + (uint32_t)(p))
#define LEVEL_OF(p)     ((p) ? EARLS_LEVEL : TOEJAMS_LEVEL)

static uint16_t bar_map[5][40];

/* $9E0E: a character's font tile (+$85B4); codes 1-6 are runs of tiles */
static void bar_text(int row, int col, uint32_t str)
{
    static const uint8_t runs[7][4] = {{0}, {1, 0x2E}, {3, 0x2F, 0x30, 0x31}, {2, 0x32, 0x33},
                                       {2, 0x34, 0x35}, {3, 0x36, 0x37, 0x38}, {2, 0x39, 0x3A}};
    int n = 0;
    for (;; str++) {
        int c = rd8(str), t, k;
        if (!c || n > 40) break;
        if (c >= 1 && c <= 6) {
            for (k = 0; k < runs[c][0]; k++)
                if (col < 40) bar_map[row][col++] = (uint16_t)(0x85B4 + runs[c][1 + k]);
            continue;
        }
        if (c >= 'a' && c <= 'z') t = c - 'a' + 1;
        else if (c >= 'A' && c <= 'Z') t = c - 'A' + 1;
        else if (c >= '0' && c <= '9') t = c - '0' + 0x24;
        else if (c == '.') t = 0x1F;
        else if (c == ',') t = 0x20;
        else if (c == '\'' || c == ':') t = 0x21;
        else if (c == '-') t = 0x22;
        else if (c == '!') t = 0x23;
        else if (c == '?') t = 0x1B;
        else t = 0;
        if (col < 40) bar_map[row][col++] = (uint16_t)(0x85B4 + t);
        n++;
    }
}

static void bar_text_c(int row, int col, const char *str)
{
    for (; *str && col < 40; str++, col++) {
        int c = *str, t = c == ' ' ? 0 : c - 'a' + 1;
        bar_map[row][col] = (uint16_t)(0x85B4 + t);
    }
}

/* One status sprite onto the bar's lines: game sprite-table entry e, placed at
 * sprite coordinates (x, y) (128 = the screen's left / top edge). */
static struct { int x, y, on; } bar_spr[23];
static void bar_put(int e, int x, int y) { bar_spr[e].x = x; bar_spr[e].y = y; bar_spr[e].on = 1; }

static void bar_sprite(uint8_t (*buf)[320], int e, int x, int y)
{
    uint32_t en = SPRITE_TABLE + (uint32_t)e * 8;
    int size = (int)ob8(en + 2), attr = (int)ob16(en + 4);
    int hs = ((size >> 2) & 3) + 1, vs = (size & 3) + 1, py, px, c;
    int sx = x - 128, sy = y - 128 - BAR_TOP;
    for (py = 0; py < vs * 8; py++) {
        int line = sy + py, row = (attr & 0x1000) ? vs * 8 - 1 - py : py;
        if (line < 0 || line >= BAR_H) continue;
        for (c = 0; c < hs; c++) {
            int colx = (attr & 0x0800) ? hs - 1 - c : c;
            uint32_t tile = ((uint32_t)attr & 0x7FF) + (uint32_t)(colx * vs + (row >> 3));
            uint8_t t8[8];
            tile_row(t8, (((uint32_t)attr & 0xF800) & ~0x1000u) | (tile & 0x7FF), row & 7);
            for (px = 0; px < 8; px++) {
                int bx = sx + c * 8 + px;
                if (bx >= 0 && bx < 320 && (t8[px] & PIX_OPAQUE)) buf[line][bx] = t8[px];
            }
        }
    }
}

/* The bar into out (320 x BAR_H, 0x00RRGGBB) for the window of player `own`.
 * Returns the number of lines to show (the last row of the window is plain). */
static uint32_t bar_gen;   /* (counts the bars drawn: build_bar) */
static int build_bar(uint32_t (*out)[320], int own)
{
    static uint8_t px[BAR_H][320];
    int row, col, p, line, x;
    uint32_t frame = 0xA82FCu;
    for (row = 0; row < 4; row++)
        for (col = 0; col < 40; col++) {
            int b = (int)rd8(frame + (uint32_t)(row * 40 + col)), t = b & 0x3F;
            if (b & 0x40) t |= 0x0800;
            if (b & 0x80) t |= 0x1000;
            bar_map[row][col] = (uint16_t)(t + 0x8580);
        }
    for (col = 0; col < 40; col++) bar_map[4][col] = 0xC580;
    /* $B62A: the rank names, or "on vacation" for a player not in the game */
    for (p = 0; p < 2; p++) {
        int in = player_in_game(p), other = player_in_game(!p);
        if (in) bar_text(1, p ? 32 : 11, rd32(0xA9F82u + 4u * (uint32_t)rd8(PROMOTION(p))));
        else if (other) {
            bar_text_c(1, p ? 30 : 9, " on");
            bar_text_c(2, p ? 29 : 8, "vacation");
        }
    }
    /* the status sprites where the game puts them on the bar */
    memset(bar_spr, 0, sizeof bar_spr);
    for (p = 0; p < 2; p++) {
        int lives, lv, icon, dx, dy;
        uint32_t fr;
        if (!player_in_game(p)) continue;
        /* $B0AC: life bar (two sprites) */
        bar_put(p ? 17 : 15, p ? 0x14A : 0xA0, 0x14D);
        bar_put(p ? 18 : 16, (p ? 0x14A : 0xA0) + 0x20, 0x14D);
        /* $B570: lives (stars) */
        lives = (int8_t)rd8(PLAYER_LIVES(p));
        bar_put(p ? 20 : 19, p ? 0x18E : 0xE4, 0x14A - (lives < 5 ? 3 : 0));
        /* $AD6A: the head, or the present in use */
        icon = (int)rd8(ACTIVE_PRESENT(p));
        icon = icon == 0xFF ? p : icon + 2;
        fr = rd32(0xABA14u + 4u * (uint32_t)icon);
        dx = (int8_t)rd8(fr + 2);
        dy = (int8_t)rd8(fr + 3);
        /* (hidden by the game: the present in use blinking as it runs out; in split
         * screen the head is hidden by design, shown here) */
        if (!(ob16(SPRITE_TABLE + (uint32_t)(p ? 22 : 21) * 8) == 1 && (rd8(ACTIVE_PRESENT(p)) != 0xFF || !rd8(SPLIT_SCREEN))))
            bar_put(p ? 22 : 21, (p ? 0x132 : 0x88) + dx, 0x141 + dy);
        /* $AB9E: the level number (this window's player's) */
        if (p == own) {
            lv = (int)rd8(LEVEL_OF(p));
            if (lv / 10) {
                bar_put(p ? 13 : 11, 0x114 + 8, 0x141);
                bar_put(p ? 14 : 12, 0x114, 0x141);
            } else {
                bar_put(p ? 13 : 11, 0x114 + 4, 0x141);
            }
        }
    }
    /* (what the bar is drawn from - its map, the sprites put on it and theirs in
     * the sprite table, the tiles and colours they use - unchanged: the bar drawn
     * last time is still right) */
    {
        static uint32_t last_sig;
        static uint32_t (*last_out)[320];
        uint32_t h = 2166136261u ^ (uint32_t)own;
        int e;
        for (row = 0; row < 5; row++)
            for (col = 0; col < 40; col++) {
                h = (h ^ bar_map[row][col]) * 16777619u;
                h = (h ^ vdp_tile_ver[bar_map[row][col] & 0x7FF]) * 16777619u;
            }
        for (e = 11; e <= 22; e++) {
            uint32_t en = SPRITE_TABLE + (uint32_t)e * 8;
            int k, n = ((((int)ob8(en + 2) >> 2) & 3) + 1) * (((int)ob8(en + 2) & 3) + 1);
            if (!bar_spr[e].on) continue;
            h = (h ^ (uint32_t)(e | bar_spr[e].x << 8 | bar_spr[e].y << 20)) * 16777619u;
            h = (h ^ (ob8(en + 2) | ob16(en + 4) << 8)) * 16777619u;
            for (k = 0; k < n; k++) h = (h ^ vdp_tile_ver[(ob16(en + 4) + (uint32_t)k) & 0x7FF]) * 16777619u;
        }
        for (e = 0; e < 64; e++) h = (h ^ vdp_palette_rgb(e)) * 16777619u;
        h = (h ^ ((uint32_t)vdp_reg[7] << 16)) * 16777619u;
        if (h == last_sig && out == last_out) return BAR_H - 8;
        last_sig = h;
        last_out = out;
        bar_gen++;
    }
    for (line = 0; line < BAR_H; line++)
        for (x = 0; x < 320; x++) {
            uint8_t t8[8];
            if ((x & 7) == 0) tile_row(t8, bar_map[line >> 3][x >> 3], line & 7);
            px[line][x] = t8[x & 7];
        }
    for (p = 22; p >= 11; p--)   /* (lower entries in front, as on the VDP) */
        if (bar_spr[p].on) bar_sprite(px, p, bar_spr[p].x, bar_spr[p].y);
    for (line = 0; line < BAR_H; line++)
        for (x = 0; x < 320; x++)
            out[line][x] = (px[line][x] & PIX_OPAQUE) ? vdp_palette_rgb(px[line][x] & 0x3F)
                                                    : vdp_palette_rgb(vdp_reg[7] & 0x3F);
    return BAR_H - 8;
}

/* ---- the map, without pausing (two-window mode) ------------------------------------
 * The game's map screen (DrawGameMap) stops the game while it is up. With a window
 * per player, a player's Map button opens this map in their window instead and the
 * game goes on (that player's character waits). Drawn from game memory as
 * LoadMapIntoVRAM / DisplayMap / LoadMapCharacters / LoadMapElevator /
 * LoadMapShipPiece draw it: the floor at 1/8 size from the map tiles in ROM, the
 * squares nobody has uncovered yet in purple, the players, the elevator and a ship
 * piece; the markers move every few frames. (The original's random "?" hints are not
 * made here: they change the game's state.) */
#define MAP_TILES       0xA4780u        /* ROM: the map's tiles (loaded at tile 1) */
#define MAP_PALETTES    0x98168u        /* ROM: MapWater_Pal, MapSpace_Pal, StarryBackground_Pal */
#define MAP_UNCOVERED   0xFF91ECu       /* 7 bytes a floor: one per row of 8 squares (320 x 256) */
#define MAP_HINTED      0xFF92A2u
#define MAP_SQUARE      0xA6BA0u        /* ROM: the 5x4 tiles of an unvisited square (+$C112) */
#define ICON_TOEJAM     0xA9DE8u
#define ICON_EARL       0xA9E08u
#define ICON_ELEVATOR   0xA9E28u
#define ICON_SHIP       0xA9E48u
#define VBLANK_COUNTER  0xFF800Eu
#define ELEVATOR_LOCATION 0xFFDA4Au
#define ELEVATOR_FLAG   0xFFDA6Au
#define EARTHLINGS_IN_LEVEL 0xFFDE72u
#define EARTHLING_LOAD_FLAG 0xFFE1AAu
#define EARTHLING_LEVEL 0xFFE1ACu

static int map_open[2];
static unsigned map_tick;
static struct { int on, x, y; } map_marks[2][4];   /* per window: ToeJam, Earl, elevator, ship piece */

int gameview_map_open(int player) { return map_open[player & 1]; }

/* (called with each frame's input) A player's Map button in two-window play:
 * opens or closes their map here instead of the game's. */
/* The soft pause (two-window mode): Start opens a menu in that player's window; the
 * game goes on (the other player plays on, this one's character waits). */
static struct { int open, sel, ask; } soft_pause[2];

int gameview_soft_pause_open(int player) { return soft_pause[player & 1].open; }

void gameview_map_input(uint16_t pad[2])
{
    static uint16_t prev[2];
    extern int g_in_game;
    /* (from game memory and the input only: both machines of a network game decide
     * the same) */
    int p, playing = g_gv_windows && g_in_game && rd8(TOEJAMS_LEVEL) != FINAL_LEVEL;
    for (p = 0; p < 2; p++) {
        uint16_t now = pad[p], down = (uint16_t)(now & ~prev[p]);
        prev[p] = now;
        if (!playing || !player_in_game(p)) { map_open[p] = 0; soft_pause[p].open = 0; continue; }
        if (soft_pause[p].open) {
            int n = 2;
            if (down & PAD_UP) soft_pause[p].sel = (soft_pause[p].sel + n - 1) % n;
            if (down & PAD_DOWN) soft_pause[p].sel = (soft_pause[p].sel + 1) % n;
            if (down & PAD_B) {
                if (soft_pause[p].ask) soft_pause[p].ask = 0, soft_pause[p].sel = 1;
                else soft_pause[p].open = 0;
            } else if (down & (PAD_A | PAD_C | PAD_START)) {
                if (!soft_pause[p].ask) {
                    if (soft_pause[p].sel == 0) soft_pause[p].open = 0;          /* resume */
                    else { soft_pause[p].ask = 1; soft_pause[p].sel = 0; }       /* quit? no / yes */
                } else if (soft_pause[p].sel == 1) {
                    extern void save_quit_to_menu(void);
                    soft_pause[0].open = soft_pause[1].open = 0;
                    map_open[0] = map_open[1] = 0;
                    save_quit_to_menu();   /* (network play: leaving the game) */
                } else { soft_pause[p].ask = 0; soft_pause[p].sel = 1; }
            }
            pad[p] = 0;   /* (the character waits) */
            continue;
        }
        if (down & PAD_START) {
            /* (never the game's pause: it would stop both players) */
            soft_pause[p].open = 1;
            soft_pause[p].sel = 0;
            soft_pause[p].ask = 0;
            map_open[p] = 0;
            pad[p] = 0;
            continue;
        }
        pad[p] &= (uint16_t)~PAD_START;
        if (map_open[p]) {
            if (down & (PAD_C | PAD_B)) map_open[p] = 0;
            pad[p] = 0;   /* (the character waits) */
            continue;
        }
        if (!rd8(MENU_FLAG(p))) {
            if (down & PAD_C) {
                map_open[p] = 1;
                map_tick = 0;
            }
            pad[p] &= (uint16_t)~PAD_C;   /* (never the game's map: it would stop the game) */
        }
    }
}

static int div8(int v) { return v >= 0 ? v >> 3 : -((-v) >> 3); }

static void map_marks_update(int win, int p, int level)
{
    int q, e;
    memset(map_marks[win], 0, sizeof map_marks[win]);
    for (q = 0; q < 2; q++) {
        int ef = (int)rd8(ELEVATOR_FLAG + (uint32_t)q);
        if (!player_in_game(q) || (int)rd8(PLAYER_OBJ(q) + 0x4C) != level) continue;
        if (ef && !((ef - 1) & 1)) continue;   /* ($8356: riding the elevator) */
        map_marks[win][q].on = 1;
        map_marks[win][q].x = div8((int16_t)rd16(PLAYER_OBJ(q)) - 0x80) + 2;
        map_marks[win][q].y = div8((int16_t)rd16(PLAYER_OBJ(q) + 2) - 0x40);
    }
    for (q = 0; q < 2; q++) {
        uint32_t el = ELEVATOR_LOCATION + (uint32_t)q * 16;
        if ((int)rd8(el + 4) != level || rd8(el + 5) == 0x0D) continue;
        map_marks[win][2].on = 1;
        map_marks[win][2].x = div8((int16_t)rd16(el) - 0x80) + 2;
        map_marks[win][2].y = div8((int16_t)rd16(el + 2) - 0x40);
        break;
    }
    for (q = 0; q < 2; q++) {
        uint32_t base = EARTHLINGS_IN_LEVEL + (uint32_t)q * 0x168;
        if (!rd8(EARTHLING_LOAD_FLAG + (uint32_t)q) || (int)rd8(EARTHLING_LEVEL + (uint32_t)q) != level) continue;
        if (!rd8(base)) e = 0;
        else if (rd8(base + 0x12)) continue;
        else e = 1;
        map_marks[win][3].on = 1;
        map_marks[win][3].x = div8((int16_t)rd16(base + (uint32_t)e * 18 + 6) - 0x80) - 2;
        map_marks[win][3].y = div8((int16_t)rd16(base + (uint32_t)e * 18 + 8) - 0x40);
        break;
    }
    (void)p;
}

/* a pixel row of a tile kept in ROM (map tiles and icons): colour indices 0-15 */
static void rom_tile_row(uint8_t out[8], uint32_t tile_addr, int row, int hflip, int vflip)
{
    int i;
    if (vflip) row = 7 - row;
    for (i = 0; i < 8; i++) {
        int px = hflip ? 7 - i : i;
        uint32_t b = rd8(tile_addr + (uint32_t)row * 4 + (uint32_t)(px >> 1));
        out[i] = (uint8_t)((px & 1) ? (b & 15) : (b >> 4));
    }
}

/* a pixel row of a tile on the map screen: tiles 1-$121 are the map's (from ROM),
 * the rest are what VRAM keeps during play (the frame, stars) */
static void map_tile_row(uint8_t out[8], uint16_t e, int row)
{
    int tile = e & 0x7FF, i;
    if (tile >= 1 && tile <= 0x121) {
        rom_tile_row(out, MAP_TILES + (uint32_t)(tile - 1) * 32, row, (e & 0x800) != 0, (e & 0x1000) != 0);
        return;
    }
    tile_row(out, e, row);
    for (i = 0; i < 8; i++) out[i] = (out[i] & PIX_OPAQUE) ? (out[i] & 15) : 0;
}

static uint32_t map_hash(uint32_t v) { v ^= v >> 16; v *= 0x7FEB352Du; v ^= v >> 15; v *= 0x846CA68Bu; return v ^ (v >> 16); }

static void render_map(int win, int p, uint32_t *out, int w, int h)
{
    static uint16_t map[28][40], pb[30][42];
    uint32_t pal[4][16], tiles = p ? EARL_LEVEL_TILES : TOEJAM_LEVEL_TILES;
    int level = (int)rd8(LEVEL_OF(p)), ox = (w - 320) / 2, oy = (h - VDP_HEIGHT) / 2;
    int row, col, x, y, k, sq_x, sq_y;
    for (k = 0; k < 48; k++) pal[k >> 4][k & 15] = vdp_color_rgb((uint16_t)rd16(MAP_PALETTES + (uint32_t)k * 2));
    for (k = 0; k < 16; k++) pal[3][k] = vdp_palette_rgb(48 + k);
    /* plane B ($78A8): the starry frame from ROM $A7E9C, the floor one 2-tile cell per
     * 128 x 64 block on it, and a few fixed tiles ($7804) */
    memset(pb, 0, sizeof pb);
    for (row = 0; row < 28; row++)
        for (col = 0; col < 40; col++) {
            int b = (int)rd8(0xA7E9Cu + (uint32_t)((row <= 0x1A ? row : row - 1) * 40 + col)), e = b & 0x3F;
            if (b & 0x40) e |= 0x0800;
            if (b & 0x80) e |= 0x1000;
            pb[row + 1][col + 1] = (uint16_t)(e + 0x4580);
        }
    for (row = 0; row < 26; row++)
        for (col = 0; col < 19; col++) {
            int v = (int)rd16(tiles + (uint32_t)(row * 19 + col) * 2), id = v & 0xFF, b7 = id & 0x80, pl = 0, a, bb;
            uint16_t *cell = &pb[row + 2][2 + col * 2];
            if (id == 0x22) {
                /* (open space: a scattering of stars, at random in the game; fixed here) */
                for (k = 0; k < 2; k++) {
                    uint32_t r = map_hash((uint32_t)(level * 4096 + row * 64 + col * 2 + k));
                    int n = 0, lim = 0x100, rr = (int)(r & 0x3FF);
                    while (rr <= lim && n < 8) { lim -= 0x20; n++; }
                    cell[k] = (uint16_t)(n ? 0x422B + (int)((r >> 12) & 0x1800) + n - 1 : 0x2045);
                }
                continue;
            }
            id &= 0x7F;
            a = id * 2 + 1;
            bb = id * 2 + 2;
            if (id < 0x40) pl = 1;
            else if (id > 0x65) pl = (int)rd8(0x98408u + (uint32_t)(id - 0x66));
            if (rd8(0xFF91EAu) && (rd8(0xFF8D3Eu + (uint32_t)(row * 19 + col)) & 15)) pl = 3;
            a |= (pl << 13) & 0xE000;
            bb |= (pl << 13) & 0xE000;
            if (b7) { cell[0] = (uint16_t)(bb | 0x800); cell[1] = (uint16_t)(a | 0x800); }
            else { cell[0] = (uint16_t)a; cell[1] = (uint16_t)bb; }
        }
    {
        static const struct { uint8_t r, c; uint16_t e; } fixed[] = {
            {11, 1, 0x458F}, {24, 1, 0x458F}, {27, 1, 0x458F}, {11, 40, 0x4D8F}, {24, 40, 0x4D8F},
            {27, 40, 0x4D8F}, {28, 10, 0x459B}, {28, 28, 0x459B}, {28, 31, 0x459B}};
        for (k = 0; k < (int)(sizeof fixed / sizeof fixed[0]); k++) pb[fixed[k].r][fixed[k].c] = fixed[k].e;
    }
    /* the window ($7F52 / DisplayMap): overlays on the floor, and the squares nobody
     * has been to yet */
    for (row = 0; row < 28; row++)
        for (col = 0; col < 40; col++) map[row][col] = 0x8000;
    for (row = 0; row < 26; row++)
        for (col = 0; col < 19; col++) {
            int v = (int)rd16(tiles + (uint32_t)(row * 19 + col) * 2), type = (v & 0x1F00) >> 8, t, a, bb;
            if (!type) continue;
            t = type & 15;
            a = (0x79 + t) * 2 + 1;
            bb = (0x79 + t) * 2 + 2;
            if (type & 0x10) { map[row + 1][1 + col * 2] = (uint16_t)(bb | 0x8800); map[row + 1][2 + col * 2] = (uint16_t)(a | 0x8800); }
            else { map[row + 1][1 + col * 2] = (uint16_t)(a | 0x8000); map[row + 1][2 + col * 2] = (uint16_t)(bb | 0x8000); }
        }
    for (sq_y = 0; sq_y < 7; sq_y++)
        for (sq_x = 0; sq_x < 8; sq_x++) {
            int bit = 0x80 >> sq_x;
            if ((rd8(MAP_UNCOVERED + (uint32_t)(level * 7 + sq_y)) & bit) ||
                (rd8(MAP_HINTED + (uint32_t)(level * 7 + sq_y)) & bit)) continue;
            for (row = 0; row < 4; row++)
                for (col = 0; col < 5; col++)
                    map[sq_y * 4 + row][sq_x * 5 + col] = (uint16_t)(0xC112 + rd8(MAP_SQUARE + (uint32_t)(row * 5 + col)));
        }
    if (map_tick++ % 8 == 0) map_marks_update(win, p, level);
    for (y = 0; y < h; y++) {
        uint32_t *o = out + (size_t)y * GAMEVIEW_MAX_W;
        int sy = y - oy;
        for (x = 0; x < w; x++) {
            /* plane B, scrolled 8 left and 8 up; around the original screen the
             * starry frame carries on */
            int sx = x - ox, px = sx + 8, py = sy + 8, c = px >> 3, r = py >> 3;
            uint16_t e, we;
            uint8_t t8[8];
            if (sx < 0 || sy < 0 || sx >= 320 || sy >= VDP_HEIGHT) {
                /* (around the original screen: more of the unexplored purple) */
                e = (uint16_t)(0xC112 + rd8(MAP_SQUARE + 6));
                map_tile_row(t8, e, ((sy % 8) + 8) % 8);
                o[x] = pal[2][t8[((sx % 8) + 8) % 8]];
                continue;
            }
            e = pb[r][c];
            if (sy >= 0 && sy < VDP_HEIGHT && sx >= 0 && sx < 320 && ((we = map[sy >> 3][sx >> 3]) & 0x7FF)) {
                map_tile_row(t8, we, sy & 7);
                if (t8[sx & 7]) { o[x] = pal[(we >> 13) & 3][t8[sx & 7]]; continue; }
            }
            if (e & 0x7FF) {
                map_tile_row(t8, e, ((py % 8) + 8) % 8);
                if (t8[((px % 8) + 8) % 8]) { o[x] = pal[(e >> 13) & 3][t8[((px % 8) + 8) % 8]]; continue; }
            }
            o[x] = pal[0][0];
        }
    }
    /* the markers (the players and the ship piece blink, as DrawMapCharacters does) */
    {
        static const uint32_t icon[4] = {ICON_TOEJAM, ICON_EARL, ICON_ELEVATOR, ICON_SHIP};
        static const int line[4] = {3, 3, 3, 2}, wide[4] = {1, 1, 1, 2};
        int blink_off = (rd16(VBLANK_COUNTER) & 0x18) == 0;
        for (k = 3; k >= 0; k--) {
            int r, c;
            if (!map_marks[win][k].on || (blink_off && k != 2)) continue;
            for (r = 0; r < 8; r++) {
                int yy = oy + map_marks[win][k].y + r;
                if (yy < 0 || yy >= h) continue;
                for (c = 0; c < wide[k]; c++) {
                    uint8_t t8[8];
                    int i;
                    rom_tile_row(t8, icon[k] + (uint32_t)c * 32, r, 0, 0);
                    for (i = 0; i < 8; i++) {
                        int xx = ox + map_marks[win][k].x + c * 8 + i;
                        if (t8[i] && xx >= 0 && xx < w) out[(size_t)yy * GAMEVIEW_MAX_W + xx] = pal[line[k]][t8[i]];
                    }
                }
            }
        }
    }
}

/* The soft pause's box: the status bar's frame and font (tiles the game keeps in
 * VRAM during play), in the middle of the original screen. */
static void draw_soft_pause(uint32_t *out, int w, int h, int p)
{
    enum { SPW = 20, SPH = 7 };
    static uint16_t box[SPH][SPW];
    static const char *items[2][2] = {{"resume", "quit to menu"}, {"no", "yes"}};
    int r, c, x0 = (w - SPW * 8) / 2, y0 = (h - SPH * 8) / 2, k;
    uint32_t frame = 0xA82FCu;
    for (r = 0; r < SPH; r++)
        for (c = 0; c < SPW; c++) {
            int src_row = r == 0 ? 0 : r == SPH - 1 ? 3 : (r & 1) ? 1 : 2;
            int src_col = c == 0 ? 0 : c == SPW - 1 ? 39 : (r == 0 || r == SPH - 1) ? 1 + (c - 1) % 38 : -1;
            int b = src_col < 0 ? 0 : (int)rd8(frame + (uint32_t)(src_row * 40 + src_col)), t = b & 0x3F;
            if (b & 0x40) t |= 0x0800;
            if (b & 0x80) t |= 0x1000;
            box[r][c] = (uint16_t)(t + 0x8580);
        }
    {
        const char *title = soft_pause[p].ask ? "quit to menu?" : (p ? "earl - paused" : "toejam - paused");
        for (k = 0; title[k] && k < SPW - 2; k++) {
            int ch = title[k], t = ch == ' ' ? 0 : ch == '-' ? 0x22 : ch == '?' ? 0x1B : ch - 'a' + 1;
            box[1][(SPW - (int)strlen(title)) / 2 + k] = (uint16_t)(0x85B4 + t);
        }
        for (r = 0; r < 2; r++) {
            const char *it = items[soft_pause[p].ask][r];
            int col = 5;
            if (soft_pause[p].sel == r) {
                box[3 + r][3] = (uint16_t)(0x85B4 + 0x22);   /* (the choice: "-") */
            }
            for (k = 0; it[k] && col + k < SPW - 1; k++) {
                int ch = it[k], t = ch == ' ' ? 0 : ch - 'a' + 1;
                box[3 + r][col + k] = (uint16_t)(0x85B4 + t);
            }
        }
    }
    for (r = 0; r < SPH * 8; r++) {
        int y = y0 + r;
        if (y < 0 || y >= h) continue;
        for (c = 0; c < SPW; c++) {
            uint8_t t8[8];
            tile_row(t8, box[r >> 3][c], r & 7);
            for (k = 0; k < 8; k++) {
                int x = x0 + c * 8 + k;
                if (x < 0 || x >= w) continue;
                out[(size_t)y * GAMEVIEW_MAX_W + x] = (t8[k] & PIX_OPAQUE) ? vdp_palette_rgb(t8[k] & 0x3F)
                                                                         : vdp_palette_rgb(vdp_reg[7] & 0x3F);
            }
        }
    }
}

/* the game's font (LOC_00009E0E): its tile number for a character */
static int font_tile(int ch)
{
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 1;
    if (ch >= 'A' && ch <= 'Z') return ch - 'A' + 1;
    if (ch >= '0' && ch <= '9') return ch - '0' + 0x24;
    switch (ch) {
    case ',': return 0x20;
    case '\'': case ':': return 0x21;
    case '-': return 0x22;
    case '!': return 0x23;
    case '.': return 0x1F;
    case '?': return 0x1B;
    }
    return 0;
}

/* A message in a box like the soft pause's, its lines broken at spaces. */
static void draw_message(uint32_t *out, int w, int h, const char *text)
{
    enum { MW = 30, MH = 7 };
    static uint16_t box[MH][MW];
    char lines[3][MW];
    int n = 0, r, c, k, x0, y0, len = (int)strlen(text), pos = 0;
    uint32_t frame = 0xA82FCu;
    memset(lines, 0, sizeof lines);
    while (pos < len && n < 3) {
        int take = len - pos > MW - 4 ? MW - 4 : len - pos;
        if (pos + take < len) {
            int b = take;
            while (b > 0 && text[pos + b] != ' ') b--;
            if (b > 0) take = b;
        }
        memcpy(lines[n++], text + pos, (size_t)take);
        pos += take;
        while (text[pos] == ' ') pos++;
    }
    for (r = 0; r < MH; r++)
        for (c = 0; c < MW; c++) {
            int src_row = r == 0 ? 0 : r == MH - 1 ? 3 : (r & 1) ? 1 : 2;
            int src_col = c == 0 ? 0 : c == MW - 1 ? 39 : (r == 0 || r == MH - 1) ? 1 + (c - 1) % 38 : -1;
            int b = src_col < 0 ? 0 : (int)rd8(frame + (uint32_t)(src_row * 40 + src_col)), t = b & 0x3F;
            if (b & 0x40) t |= 0x0800;
            if (b & 0x80) t |= 0x1000;
            box[r][c] = (uint16_t)(t + 0x8580);
        }
    for (k = 0; k < n; k++) {
        int l = (int)strlen(lines[k]), row = (MH - n) / 2 + k;
        for (c = 0; c < l; c++) box[row][(MW - l) / 2 + c] = (uint16_t)(0x85B4 + font_tile(lines[k][c]));
    }
    x0 = (w - MW * 8) / 2;
    y0 = (h - MH * 8) / 2;
    for (r = 0; r < MH * 8; r++) {
        int y = y0 + r;
        if (y < 0 || y >= h) continue;
        for (c = 0; c < MW; c++) {
            uint8_t t8[8];
            tile_row(t8, box[r >> 3][c], r & 7);
            for (k = 0; k < 8; k++) {
                int x = x0 + c * 8 + k;
                if (x < 0 || x >= w) continue;
                out[(size_t)y * GAMEVIEW_MAX_W + x] = (t8[k] & PIX_OPAQUE) ? vdp_palette_rgb(t8[k] & 0x3F)
                                                                         : vdp_palette_rgb(vdp_reg[7] & 0x3F);
            }
        }
    }
}

#define ELEVATOR_FLAG_OF(p) (0xFFDA6Au + (uint32_t)(p))

/* the game's own ride screen is up (LOC_00013540 has built it - transitions.c -
 * and everyone in the game is still riding) */
int g_gv_ride_screen;

static int alone_riding(void)
{
    int p, any = 0;
    for (p = 0; p < 2; p++)
        if (player_in_game(p)) {
            if (!(rd8(ELEVATOR_FLAG_OF(p)) & 1)) any = -1;
            else if (!any) any = 1;
        }
    if (any != 1) g_gv_ride_screen = 0;
    return any == 1 && g_gv_ride_screen;
}

/* (a window showing the usual picture during play: the views' picture of the
 * whole screen, drawn now if nobody has asked for it yet this frame) */
static int window_usual_picture(int *w, int *h, const uint32_t **pixels, int *pitch)
{
    if (!(frame_ok == 1 && g_gv_gameplay)) return 0;
    if (!views_done && (pic_w != 320 || pic_h != VDP_HEIGHT)) {
        render_views_picture();
        views_done = 1;
    }
    *pixels = picture;
    *w = pic_w;
    *h = pic_h;
    *pitch = GAMEVIEW_MAX_W;
    return 1;
}

/* (network play on a GPU) the window drawn as a list last; its overlays (the
 * message box goes after them) */
static int win_list[2], win_list_overlays;

static void window_kept_list(int player, const uint32_t **pixels)
{
    if (!win_list[player]) return;
    wlist.frozen = 1;
    wlist.noverlays = win_list_overlays;
    set_list(&wlist);
    *pixels = NULL;
}

/* (tests, TJE_VLCHECK: the window drawn both ways, the list in software compared) */
static int window_check(int player)
{
    (void)player;
    if (vl_check < 0) vl_check = getenv("TJE_VLCHECK") != NULL;
    return vl_check;
}

static void window_compare(const uint32_t *out, int w, int h)
{
    static uint32_t ref[GAMEVIEW_MAX_W * GAMEVIEW_MAX_H];
    long bad = 0;
    int x, y;
    viewlist_render_soft(&wlist, ref, GAMEVIEW_MAX_W);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            bad += ref[(size_t)y * GAMEVIEW_MAX_W + x] != out[(size_t)y * GAMEVIEW_MAX_W + x];
    if (++vl_checked % 500 == 0) fprintf(stderr, "vlcheck: %d views, %d differ\n", vl_checked, vl_bad_frames);
    if (bad) {
        vl_bad_frames++;
        fprintf(stderr, "vlcheck: window frame differs in %ld pixels\n", bad);
    }
}

int gameview_window_frame(int player, int *wp, int *hp, const uint32_t **pixels, int *pitch)
{
    int w = *wp, h = *hp;
    uint32_t *out;
    static int frozen_for[2];               /* frames this window has kept its picture */
    static int cam_ok[2], cam_level[2], wait_live[2];
    /* (network play on a GPU: this machine's window drawn as a list, gfx/viewlist.h;
     * its kept picture is the list again, the GPU's tiles kept as they were) */
    int as_list = (g_gv_use_list || window_check(player)) && player == g_gv_net_player;
    static int32_t cam_fx[2], cam_fy[2];    /* the window's camera, 1/256 pixel */
    extern int g_in_game;
    int p = player, cam_x, cam_y, ox, oy, line, split, menu, menu_from, menu_to, live, i, changed = 0, tx, ty;
    uint32_t lt;
    if (!g_gv_windows || w < 2 || h < 2) return 0;
    if (w > GAMEVIEW_MAX_W) w = GAMEVIEW_MAX_W;
    if (h > GAMEVIEW_MAX_H) h = GAMEVIEW_MAX_H;
    *wp = w;
    *hp = h;
    if (!player_in_game(p)) p = !p;   /* (one player: both windows show them) */
    out = win_pic[player];
    /* What to show, from game memory (two-window mode never splits the screen, and
     * the game's own split and join no longer redraw its planes, so the VDP's
     * planes say nothing here):
     *  - the player riding an elevator: the ride, as the game shows it;
     *  - the player changing floors (falling), or the display off: the window's
     *    last picture, until the player is on the floor the game shows again;
     *  - else the player's view. */
    /* (an elevator leaves only with every player in the game inside it - the
     * ride screen, LOC_00013540, is then shown here as the game draws it; a
     * player already in it, waiting for the other (ELEVATOR_FLAG 1), keeps their
     * own view) */
    if (!g_in_game || !player_in_game(p) || alone_riding()) {
        frozen_for[player] = 0;
        cam_ok[player] = 0;
        if (window_usual_picture(wp, hp, pixels, pitch)) return 1;
        /* (the ride screen being built behind a kept picture - not live when the
         * frame began: the VDP's kept picture is one drawn for the views, partly;
         * this window keeps its own) */
        if (g_in_game && (!vdp_live_picture() || g_gameview_reason == 1) && win_last[player].ok && win_last[player].w == w && win_last[player].h == h) {
            *pixels = win_pic[player];
            *pitch = GAMEVIEW_MAX_W;
            window_kept_list(player, pixels);
            return 1;
        }
        win_list[player] = 0;
        /* (the game's own picture is shown: afterwards, the window waits for the
         * game to show its player's floor, showing the game's picture till then -
         * not its own from before) */
        if (g_in_game && player_in_game(p)) {
            win_last[player].ok = 0;
            wait_live[player] = 1;
        }
        return 0;
    }
    live = g_gv_gameplay && frame_ok == 1;
    split = rd8(SPLIT_SCREEN) != 0;
    lt = p ? EARL_LEVEL_TILES : TOEJAM_LEVEL_TILES;
    if (win_last[player].ok)
        for (i = 0; i < (int)sizeof win_layout[0]; i++) changed += rd8(lt + (uint32_t)i) != win_layout[player][i];
    {
        /* (switching: the player's floor is not the one last drawn - a fall, the
         * floor layout changing before the screen does; the window waits for the
         * game to show the new floor) */
        int switching = win_last[player].ok && (win_last[player].level != (int)rd8(LEVEL_OF(p)) || changed > 48), draw;
        static uint8_t prev_layout[2][988];
        static int stable[2];
        /* the layout steady for a few frames with the display on: the new floor is up */
        {
            int same = 1;
            for (i = 0; i < 988; i++) {
                uint8_t b = (uint8_t)rd8(lt + (uint32_t)i);
                if (b != prev_layout[player][i]) { same = 0; prev_layout[player][i] = b; }
            }
            stable[player] = same ? stable[player] + 1 : 0;
        }
        if (switching && !wait_live[player]) wait_live[player] = 1;
        else if (wait_live[player] && (live || (vdp_live_picture() && stable[player] >= 8))) wait_live[player] = 0;
        draw = !wait_live[player];
        if (!draw && getenv("TJE_WIN_DBG")) fprintf(stderr, "window %d: holding (switching %d, live %d, stable %d)\n", player, switching, live, stable[player]);
        if (!draw) {
            if (!win_last[player].ok || win_last[player].w != w || win_last[player].h != h || ++frozen_for[player] > 600 ||
                g_gameview_reason == 3) {
                win_list[player] = 0;
                return 0;
            }
            *pixels = win_pic[player];
            *pitch = GAMEVIEW_MAX_W;
            window_kept_list(player, pixels);
            return 1;
        }
    }
    if (!live)
        for (i = 0; i < (int)sizeof layout_copy[0]; i++) {
            layout_copy[0][i] = (uint8_t)rd8(TOEJAM_LEVEL_TILES + (uint32_t)i);
            layout_copy[1][i] = (uint8_t)rd8(EARL_LEVEL_TILES + (uint32_t)i);
        }
    frozen_for[player] = 0;
    /* the camera: the player 160 px across and 128 down the original screen, kept
     * inside the floor as the game keeps its own (8..$938 across, 8..$618 down),
     * following smoothly: the players' positions change in steps (4 pixels every
     * 8 frames walking), the game's camera glides (1-2 pixels a step); this one
     * closes an eighth of the distance each frame */
    tx = (int16_t)ob16(PLAYER_OBJ(p)) - 160;
    ty = (int16_t)ob16(PLAYER_OBJ(p) + 2) - 128;
    if (tx < 8) tx = 8;
    if (tx > 0x938) tx = 0x938;
    if (ty < 8) ty = 8;
    if (ty > 0x618) ty = 0x618;
    if (!cam_ok[player] || cam_level[player] != (int)rd8(LEVEL_OF(p)) ||
        abs(tx - (cam_fx[player] >> 8)) > 256 || abs(ty - (cam_fy[player] >> 8)) > 256) {
        cam_fx[player] = tx * 256;
        cam_fy[player] = ty * 256;
        cam_ok[player] = 1;
        cam_level[player] = (int)rd8(LEVEL_OF(p));
    } else {
        cam_fx[player] += (tx * 256 - cam_fx[player]) / 8;
        cam_fy[player] += (ty * 256 - cam_fy[player]) / 8;
    }
    cam_x = (cam_fx[player] + 128) >> 8;
    cam_y = (cam_fy[player] + 128) >> 8;
    ox = (w - 320) / 2;
    oy = (h - VDP_HEIGHT) / 2;
    if (map_open[p] && p == player) {
        win_list[player] = 0;
        render_map(player, p, out, w, h);
        *pixels = out;
        *pitch = GAMEVIEW_MAX_W;
        return 1;
    }
    views_setup_cameras();
    layout_copy_on = 1;
    /* (the band of sprites across the middle: the hint box; with a menu open it is
     * part of the menu's frame, which comes with the menu below) */
    if (as_list) {
        vl_begin(&wlist, w, h);
        build_view_list(&wlist, p, cam_x, cam_y, -1, VDP_HEIGHT, 0, 0, w, h,
                        !split && !rd8(MENU_FLAG(0)) && !rd8(MENU_FLAG(1)));
    }
    if (!as_list || window_check(player)) {
        views_setup_cameras();
        gameview_render_view(p, cam_x, cam_y, -1, VDP_HEIGHT, out, w, h, GAMEVIEW_MAX_W,
                             !split && !rd8(MENU_FLAG(0)) && !rd8(MENU_FLAG(1)));
    }
    layout_copy_on = 0;
    win_list[player] = as_list && g_gv_use_list;
    win_last[player].ok = 1;
    win_last[player].w = w;
    win_last[player].h = h;
    win_last[player].cam_x = cam_x;
    win_last[player].cam_y = cam_y;
    win_last[player].level = (int)rd8(LEVEL_OF(p));
    {
        int i;
        uint32_t lt = p ? EARL_LEVEL_TILES : TOEJAM_LEVEL_TILES;
        for (i = 0; i < (int)sizeof win_layout[0]; i++) win_layout[player][i] = (uint8_t)rd8(lt + (uint32_t)i);
    }
    /* the player's menu: their half of the screen when the window covers it (in
     * one-player play, the present list from the top), at the top of the original
     * screen; plain filler rows leave the level showing */
    menu_from = p * (VDP_HEIGHT / 2);
    menu_to = menu_from + VDP_HEIGHT / 2;
    menu = live && rd8(MENU_FLAG(p)) && half_window_lines[p] >= VDP_HEIGHT / 2 - 16;
    if (live && !menu && p == 0 && line_window[0] == 1) {
        menu = 1;
        for (menu_to = 0; menu_to < VDP_HEIGHT && line_window[menu_to] == 1; menu_to++) {}
    }
    if (menu)
        for (line = menu_from; line < menu_to; line++)
            if (line_window[line] && !window_row_filler(line / 8)) {
                if (as_list) {
                    /* (a run of such lines: one overlay) */
                    int end = line + 1;
                    while (end < menu_to && line_window[end] && !window_row_filler(end / 8)) end++;
                    vl_overlay_clip(&wlist, ox, oy + line - menu_from, 320, end - line, top_window_lines[line], 320);
                    if (window_check(player))
                        for (; line < end; line++) paste_line(out, w, h, oy + line - menu_from, ox, line, 0, h);
                    line = end - 1;
                    continue;
                }
                paste_line(out, w, h, oy + line - menu_from, ox, line, 0, h);
            }
    /* the two-player status bar, flush with the bottom edge (always: split too) */
    if (rd8(TOEJAMS_LEVEL) != FINAL_LEVEL) {
        static uint32_t bar[BAR_H][320];
        int n = build_bar(bar, p), xs = ox < 0 ? -ox : 0, xd = ox < 0 ? 0 : ox, cnt = 320 - xs;
        if (as_list) {
            ovl_stamp = 0x10000000u | (bar_gen & 0x0FFFFFFFu);   /* (the same bar: the GPU has it) */
            vl_overlay_clip(&wlist, ox, h - n, 320, n, &bar[0][0], 320);
            ovl_stamp = 0;
        }
        if (as_list && !window_check(player)) n = 0;
        if (xd + cnt > w) cnt = w - xd;
        for (line = 0; line < n && cnt > 0; line++)
            if (h - n + line >= 0)
                memcpy(out + (size_t)(h - n + line) * GAMEVIEW_MAX_W + xd, bar[line] + xs, sizeof(uint32_t) * (size_t)cnt);
    }
    if (soft_pause[p].open && p == player) {
        draw_soft_pause(out, w, h, p);
        if (as_list)   /* (the box: SPW x SPH tiles, centred) */
            vl_overlay_clip(&wlist, (w - 160) / 2, (h - 56) / 2, 160, 56,
                            out + (size_t)((h - 56) / 2) * GAMEVIEW_MAX_W + (w - 160) / 2, GAMEVIEW_MAX_W);
    }
    *pixels = out;
    *pitch = GAMEVIEW_MAX_W;
    if (as_list) {
        win_list_overlays = wlist.noverlays;
        if (window_check(player)) window_compare(out, w, h);
        if (g_gv_use_list) {
            set_list(&wlist);
            *pixels = NULL;
        }
    }
    return 1;
}

/* (tests) the bar built for player own's window, lines 184.. of the original screen */
int gameview_test_bar(uint32_t (*out)[320], int own) { return build_bar(out, own); }

/* (tests) where the 1P picture's status bar is: original lines from..to-1 shown
 * at picture row `row`; 0 if not shown this frame */
int gameview_test_bar_rows(int *from, int *to, int *row)
{
    if (frame_ok != 1 || nregions != 1 || !(win_to == VDP_HEIGHT && win_from > 0) || (pic_w == 320 && pic_h == VDP_HEIGHT)) return 0;
    *from = win_from;
    *to = hud_end;
    *row = pic_h - (hud_end - win_from);
    return 1;
}

/* The VDP's lines this frame that something here will show or copy: with the
 * per-player views drawing a gameplay frame, only the window's rows (menus, the
 * present list) and, in a split screen shown in one window, every line (its held
 * and menu halves). Everything else, and the first line (gameplay is decided
 * after it), the VDP draws as always. */
int g_gv_need_vdp;   /* (tests comparing with the VDP's picture) */

static int wants_vdp_line(int line)
{
    return wants_vdp_line_(line);
}
static int wants_vdp_line_(int line)
{
    int wv, in_window;
    if (screen_list_frame && !vl_check_on() && line != 0 && !g_gv_need_vdp) return 0;   /* (drawn from a list) */
    if (g_gv_need_vdp || line == 0 || frame_ok != 1) return 1;
    if (!g_gv_windows) {
        if ((pic_w == 320 && pic_h == VDP_HEIGHT) || nregions == 2) return 1;
        /* (one view: the status bar - the window at the bottom - is built from
         * game memory, render_status_bar; only a window at the top, the present
         * list, is shown as the VDP draws it) */
        if (vdp_reg[18] & 0x80) return 0;
    } else {
        /* (a window showing the game's own picture: its elevator ride screen -
         * gameview_window_frame) */
        extern int g_in_game;
        if (!g_in_game) return 1;
        if (alone_riding()) return 1;
        /* (each window draws its own view and status bar - from game memory: of
         * the VDP's picture only a menu over a player's half and the present list
         * from the top are shown) */
        wv = (vdp_reg[18] & 0x1F) * 8;
        in_window = (vdp_reg[18] & 0x80) ? line >= wv : line < wv;
        return in_window && (!(vdp_reg[18] & 0x80) || rd8(MENU_FLAG(0)) || rd8(MENU_FLAG(1)));
    }
    wv = (vdp_reg[18] & 0x1F) * 8;
    in_window = (vdp_reg[18] & 0x80) ? line >= wv : line < wv;
    return in_window;
}

void gameview_install_line_filter(void) { vdp_line_wanted = wants_vdp_line; }
