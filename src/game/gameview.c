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
#include "core/vdp.h"
#include "gfx/present.h"

#include <string.h>

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
    word = rd16(tiles + (uint32_t)((by - 1) * 19 + (bx - 1)) * 2);
    h = (word & 0x1F00u) >> 8;
    if (h) t = edge_cell(h, sy, sx);
    if (!t) t = block_cell(word & 0xFF, sy, sx);
    return attr | t;
}

/* 8 pixels of a tile row into line-buffer form (bits 0-5 colour, 6 priority, 7 opaque) */
static void tile_row(uint8_t out[8], uint32_t entry, int row)
{
    uint32_t tile = entry & 0x7FF, pal = (entry >> 9) & 0x30;
    uint8_t prio = (entry & 0x8000) ? PIX_PRIO : 0;
    const uint8_t *p;
    int i;
    if (entry & 0x1000) row = 7 - row;
    p = &vdp_vram[(tile * 32 + (uint32_t)row * 4) & 0xFFFF];
    for (i = 0; i < 8; i++) {
        int px = (entry & 0x0800) ? 7 - i : i;
        uint8_t b = p[px >> 1];
        uint32_t c = (px & 1) ? (b & 0x0F) : (b >> 4);
        out[i] = (uint8_t)(c ? (PIX_OPAQUE | prio | pal | c) : prio);
    }
}

typedef struct {
    int top, bottom;          /* screen rows */
    int cam_x, cam_y;         /* world position of the region's top-left pixel */
    uint32_t tiles;
    int level;
} Region;

/* Does plane A in VRAM hold this floor where the camera is? (If not, the screen is
 * showing something else: the map, a cutscene, a transition...) */
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
        tile_row(px, level_cell(r->tiles, r->level, cx, cy), row);
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

/* ---- sprites ----------------------------------------------------------------- */

#define OBJECTS             0xFFA25Au   /* TOEJAMS_XPOS: 40 objects of $80 bytes */
#define OBJECT_SIZE         0x80u
#define NUM_OBJECTS         40

/* Which hardware sprites belong to objects of the level (players, Earthlings...)
 * as opposed to the status bar and other screen furniture. */
static uint8_t object_sprite[80];

static void find_object_sprites(void)
{
    int e, i;
    memset(object_sprite, 0, sizeof object_sprite);
    for (e = 0; e < NUM_OBJECTS; e++) {
        uint32_t o = OBJECTS + (uint32_t)e * OBJECT_SIZE;
        uint32_t fr;
        int n;
        if (!(rd8(o + 0x2F) & 1) || !(rd8(o + 0x2F) & 2)) continue;
        fr = rd32(o + 0x22);
        n = fr ? (int)rd8(fr) : 0;
        for (i = 0; i < n && i < 12; i++) object_sprite[rd8(o + 0x30 + (uint32_t)i) % 80] = 1;
        if (rd8(o + 0x48)) object_sprite[rd8(o + 0x48) % 80] = 1;   /* its shadow */
    }
    object_sprite[0] = 0;
}

/* One row of the sprite table, as the VDP draws it (earlier sprites in the link
 * list in front), without its per-line limits. which: 1 = objects' sprites,
 * 2 = the others, 3 = all. */
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
        if (y >= sy && y < sy + vs * 8 && (which & (object_sprite[idx % 80] ? 1 : 2))) {
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
                    if (x >= x_from && x < x_to && (px[i] & PIX_OPAQUE) && !(buf[x + off] & PIX_OPAQUE))
                        buf[x + off] = px[i];
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
    uint8_t px[PIECE_MAX_PX];
} Piece;

static Piece pieces[MAX_PIECES];
static int npieces;
int g_gameview_test_pieces;   /* tests: draw every object from its frame data, none from the sprite table */

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
                int b = (int8_t)rd8(src++), n, k;
                if (b < 0) {
                    uint8_t v = 0;
                    if (b & 0x40) v = rd8(src++);
                    n = 64 - (b & 0x3F);
                    for (k = 0; k < n; k++, o += 4) if (o < total) out[o] = v;
                } else if (b & 0x40) {
                    n = b & 0x1F;
                    if (b & 0x20) {
                        uint8_t v = rd8(src++), v2 = (uint8_t)((v << 4) | (v >> 4));
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
                    for (k = 0; k < n; k++, o += 4) { uint8_t v = rd8(src++); if (o < total) out[o] = v; }
                }
                left -= n;
                if (n <= 0) break;
            }
        }
    } else if (flags & 0x40) {
        int k;
        for (k = 0; k < total; k++) out[k] = rd8(src + (uint32_t)k);
    } else {
        int left = total, o = 0;
        while (left > 0) {
            int b = (int8_t)rd8(src++), n, k;
            if (b < 0) {
                uint8_t v = 0;
                if (b & 0x40) v = rd8(src++);
                n = 64 - (b & 0x3F);
                for (k = 0; k < n; k++, o++) if (o < total) out[o] = v;
            } else if (b & 0x40) {
                n = b & 0x3F;
                for (k = 0; k < n; k++, o++) if (o < total) out[o] = 0xFF;
            } else {
                n = b;
                for (k = 0; k < n; k++, o++) { uint8_t v = rd8(src++); if (o < total) out[o] = v; }
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
    int dx = (int16_t)rd16(o) - r->cam_x, dy = (int16_t)rd16(o + 2) - r->cam_y;
    int hw = rd8(o + 0x29), hh = rd8(o + 0x2A), z = (int16_t)rd16(o + 4);
    int height = (r->bottom - r->top) >= VDP_HEIGHT ? 0xE0 : 0x70;
    return hw + dx >= 0 && dx - hw <= 0x140 && dy + 10 > 0 && dy - hh - z <= height;
}

/* Decode the pieces of every object of the region's floor that has no hardware
 * sprites but would show in the picture (original-screen area x0..x1, y0..y1). */
static void find_offscreen_objects(const Region *r, int x0, int y0, int x1, int y1)
{
    static uint8_t data[16 * 32];
    uint32_t order[NUM_OBJECTS];
    uint8_t listed[NUM_OBJECTS];
    int e, k, norder = 0;
    /* in the game's drawing order (LOC_0000C4DA): the object lists at $FFD938 and
     * $FFD93C, linked through $5A; front first */
    memset(listed, 0, sizeof listed);
    for (k = 0; k < 2; k++) {
        uint32_t o = rd32(k ? 0xFFD93Cu : 0xFFD938u);
        int guard = 0;
        while (o >= OBJECTS && o < OBJECTS + NUM_OBJECTS * OBJECT_SIZE && guard++ < NUM_OBJECTS) {
            int idx = (int)((o - OBJECTS) / OBJECT_SIZE);
            if (!listed[idx]) { listed[idx] = 1; order[norder++] = o; }
            o = rd32(o + 0x5A);
        }
    }
    for (e = 0; e < NUM_OBJECTS; e++)
        if (!listed[e]) order[norder++] = OBJECTS + (uint32_t)e * OBJECT_SIZE;
    npieces = 0;
    for (k = 0; k < norder && npieces < MAX_PIECES; k++) {
        uint32_t o = order[k], fr;
        int n, i, ox_, oy_, z, attr;
        if (!(rd8(o + 0x2F) & 1)) continue;                /* exists */
        if (rd8(o + 0x2F) & 2) {                           /* has sprites on camera 0 */
            if (!g_gameview_test_pieces || !rd8(o + 0x4A)) continue;
        }
        if ((int)rd8(o + 0x4C) != r->level) continue;
        if (rd8(o + 0x4D) & 0x40) continue;                /* hidden */
        if (!(rd8(o + 0x2F) & 2) && on_game_screen(o, r)) continue;   /* the game hides it */
        fr = rd32(o + 0x22);
        if (fr < 0x200 || fr >= 0x400000) continue;
        n = rd8(fr);
        attr = (int)rd16(o + 0x2C);
        z = (int16_t)rd16(o + 4);
        if (z > 0xE0 || z < -0xE0) continue;
        ox_ = (int16_t)rd16(o) - r->cam_x;
        oy_ = (int16_t)rd16(o + 2) - r->cam_y + r->top;
        if (ox_ < x0 - 96 || ox_ > x1 + 96 || oy_ < y0 - 96 || oy_ > y1 + 160) continue;
        for (i = 0; i < n && i < 12 && npieces < MAX_PIECES; i++) {
            uint32_t pc = fr + 4 + (uint32_t)i * 10;
            int cw = rd8(pc), ch = rd8(pc + 1), flags = rd8(pc + 5);
            int dx = (int8_t)rd8(pc + ((attr & 0x0800) ? 4 : 2)), dy = (int8_t)rd8(pc + 3);
            int pal = (flags & 3) << 4, prio = (attr & 0x8000) ? PIX_PRIO : 0, flip = attr & 0x0800;
            Piece *p;
            int px, py;
            if (cw < 1 || cw > 4 || ch < 1 || ch > 4) continue;
            p = &pieces[npieces];
            p->x = ox_ + dx;
            p->y = oy_ + dy - z;
            p->w = cw * 8;
            p->h = ch * 8;
            if (p->x + p->w <= x0 || p->x >= x1 || p->y + p->h <= y0 || p->y >= y1) continue;
            unpack(rd32(pc + 6), flags, cw * ch, data);
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
}

static void pieces_row(uint8_t *buf, int off, int x_from, int x_to, int y)
{
    int i, x;
    for (i = 0; i < npieces; i++) {
        const Piece *p = &pieces[i];
        const uint8_t *src;
        if (y < p->y || y >= p->y + p->h) continue;
        src = p->px + (y - p->y) * p->w;
        for (x = 0; x < p->w; x++) {
            int sx = p->x + x;
            if (sx >= x_from && sx < x_to && (src[x] & PIX_OPAQUE) && !(buf[sx + off] & PIX_OPAQUE))
                buf[sx + off] = src[x];
        }
    }
}

/* ---- the picture ------------------------------------------------------------- */

static uint32_t picture[GAMEVIEW_MAX_W * GAMEVIEW_MAX_H];
static uint8_t band_row[GAMEVIEW_MAX_H];  /* picture rows holding the status bar / present list */
static int pic_w = 320, pic_h = VDP_HEIGHT;
static int want_w = 320, want_h = VDP_HEIGHT;
static int off_x, off_y;                 /* where the original screen's (0, 0) is in the picture */
static int frame_ok;                     /* every line so far drawn here */
int g_gameview_reason;                   /* why the last frame was not drawn here (tests) */
static Region region[2];
static int nregions, win_from, win_to;   /* rows covered by the window plane at line 0 */

void gameview_set_size(int w, int h)
{
    want_w = w < 2 ? 2 : w > GAMEVIEW_MAX_W ? GAMEVIEW_MAX_W : w & ~1;
    want_h = h < 2 ? 2 : h > GAMEVIEW_MAX_H ? GAMEVIEW_MAX_H : h & ~1;
}

/* At line 0: is the screen showing gameplay, and where are the cameras? */
static int frame_setup(void)
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
    if (win_from == 0 && win_to >= VDP_HEIGHT) { g_gameview_reason = 4; return 0; }

    region[0].cam_x = (int16_t)rd16(TOEJAM_CAMERA_XPOS);
    region[0].cam_y = (int16_t)rd16(TOEJAM_CAMERA_YPOS);
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
        /* split screen keeps the original size for now */
        pic_w = 320;
        pic_h = VDP_HEIGHT;
    } else {
        region[0].bottom = VDP_HEIGHT;
        nregions = 1;
        pic_w = want_w;
        pic_h = want_h;
    }
    if (nregions == 1 && !plane_shows_level(&region[0], win_from, win_to)) { g_gameview_reason = 5; return 0; }
    off_x = (pic_w - 320) / 2;
    off_y = (pic_h - VDP_HEIGHT) / 2;
    memset(band_row, 0, sizeof band_row);
    g_gameview_reason = 0;
    return 1;
}

/* Draw picture row r, which shows original-screen row y (any y: rows above and
 * below the original screen too). game_line: y is a line of the original screen
 * that the VDP showed as gameplay (not window), so plane A in VRAM is right there. */
static void draw_row(int r, int y, int game_line)
{
    static uint8_t a[GAMEVIEW_MAX_W], b[GAMEVIEW_MAX_W], sp[GAMEVIEW_MAX_W];
    const Region *reg = (nregions == 2 && y >= region[1].top) ? &region[1] : &region[0];
    int x0 = -off_x, x1 = pic_w - off_x;          /* original-screen columns in the picture */
    uint32_t *o = picture + (size_t)r * GAMEVIEW_MAX_W, backdrop;
    int x;
    if (r < 0 || r >= pic_h || band_row[r]) return;
    memset(sp, 0, (size_t)pic_w);
    /* the level: plane A as the VDP shows it inside the original screen (the game
     * sometimes redraws changed ground there a little after changing the floor
     * layout); from the floor layout everywhere else */
    if (game_line) {
        int i0 = x0 < 0 ? 0 : x0, i1 = x1 > 320 ? 320 : x1;
        plane_row(a, off_x, i0, i1, y, 0);
        if (x0 < 0) level_row(a, off_x, x0, 0, reg, y);
        if (x1 > 320) level_row(a, off_x, 320, x1, reg, y);
    } else {
        level_row(a, off_x, x0, x1, reg, y);
    }
    plane_row(b, off_x, x0, x1, y, 1);
    /* sprites: the objects' anywhere; the others (status bar icons, pause sign)
     * only where the original screen showed gameplay */
    sprite_row(sp, off_x, x0, x1, y, g_gameview_test_pieces ? (game_line ? 2 : 0) : game_line ? 3 : 1);
    pieces_row(sp, off_x, x0, x1, y);
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

/* A window line (status bar, present list) as the VDP drew it, kept at the top
 * or bottom edge of the picture, centred. */
static void draw_band(int line)
{
    int r = line < VDP_HEIGHT / 2 ? line : pic_h - VDP_HEIGHT + line;
    const uint32_t *src = vdp_framebuffer() + (size_t)line * VDP_FB_PITCH;
    uint32_t *o;
    int x;
    if (r < 0 || r >= pic_h) return;
    o = picture + (size_t)r * GAMEVIEW_MAX_W;
    if (pic_w != 320) {
        /* the picture outside the bar: the level, as on the rows around it */
        draw_row(r, r - off_y, 0);
    }
    band_row[r] = 1;
    for (x = 0; x < 320; x++) {
        int px = x + off_x;
        if (px >= 0 && px < pic_w) o[px] = src[x];
    }
}

void gameview_line(int line)
{
    const Region *r;
    int y, in_window, wv;
    if (line == 0) {
        int w, h;
        present_wanted_view(&w, &h);
        gameview_set_size(w, h);
        frame_ok = frame_setup();
        if (frame_ok) {
            find_object_sprites();
            /* (at the original size, exactly the original picture: objects off its
             * screen are not drawn, even the few pixels of them that would reach in) */
            if (pic_w != 320 || pic_h != VDP_HEIGHT || g_gameview_test_pieces)
                find_offscreen_objects(&region[0], -off_x, -off_y, pic_w - off_x, pic_h - off_y);
            else
                npieces = 0;
            /* rows above the original screen */
            for (y = -off_y; y < 0; y++) draw_row(y + off_y, y, 0);
        }
    }
    if (!frame_ok || line < 0 || line >= VDP_HEIGHT) return;
    if (!vdp_live_picture()) { frame_ok = 0; g_gameview_reason = 7; return; }
    if (vdp_reg[17] & 0x1F) { frame_ok = 0; g_gameview_reason = 3; return; }
    r = (nregions == 2 && line >= region[1].top) ? &region[1] : &region[0];
    /* At the last line of each half: does plane A (whichever the game points the
     * VDP at by then; split screen switches planes a line into the lower half)
     * show this floor here? */
    if (nregions == 2 && line == r->bottom - 1 && !plane_shows_level(r, win_from, win_to)) {
        frame_ok = 0;
        g_gameview_reason = 5 + (r == &region[1]);
        return;
    }
    if ((line & 31) == 0) find_object_sprites();    /* (the game rewrites the sprite table during the frame) */
    /* window rows now (the game moves the window during the frame when the
     * present list opens and closes) */
    wv = (vdp_reg[18] & 0x1F) * 8;
    in_window = (vdp_reg[18] & 0x80) ? line >= wv : line < wv;
    if (in_window) draw_band(line);
    if (pic_w == 320 && pic_h == VDP_HEIGHT) {
        if (!in_window) draw_row(line, line, 1);
    } else {
        draw_row(line + off_y, line, !in_window);
    }
    if (line == VDP_HEIGHT - 1)                       /* rows below the original screen */
        for (y = VDP_HEIGHT; y < pic_h - off_y; y++) draw_row(y + off_y, y, 0);
}

int gameview_frame(const uint32_t **pixels, int *width, int *height, int *pitch)
{
    if (!frame_ok) return 0;
    /* the cameras only change between frames (at VBlank); if not, something
     * restarted the machine during the frame (loading a game): don't trust it */
    if (region[0].cam_x != (int16_t)rd16(TOEJAM_CAMERA_XPOS) || region[0].cam_y != (int16_t)rd16(TOEJAM_CAMERA_YPOS) ||
        (nregions == 2) != (rd8(SPLIT_SCREEN) != 0)) {
        g_gameview_reason = 8;
        return 0;
    }
    *pixels = picture;
    *width = pic_w;
    *height = pic_h;
    *pitch = GAMEVIEW_MAX_W;
    return 1;
}
