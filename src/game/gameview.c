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
            total++;
            if (v == level_cell(r->tiles, r->level, cx, cy)) match++;
        }
    return total > 0 && match * 10 >= total * 9;
}

/* one row of the level (plane A) of a region */
static void level_row(uint8_t *line, int w, const Region *r, int screen_y)
{
    int wy = r->cam_y + (screen_y - r->top);
    int cy = wy >> 3, row = wy & 7;
    int x = -(r->cam_x & 7);
    int cx = r->cam_x >> 3;
    for (; x < w; x += 8, cx++) {
        uint8_t px[8];
        int i;
        tile_row(px, level_cell(r->tiles, r->level, cx, cy), row);
        for (i = 0; i < 8; i++)
            if (x + i >= 0 && x + i < w) line[x + i] = px[i];
    }
}

/* One row of a plane as the VDP shows it now (scroll as set for this line),
 * screen columns x_from..x_to. plane: 0 = A, 1 = B. */
static void plane_row(uint8_t *line, int x_from, int x_to, int screen_y, int plane)
{
    static const int psz[4] = {32, 64, 32, 128};
    int pw = psz[vdp_reg[16] & 3], ph = psz[(vdp_reg[16] >> 4) & 3];
    uint32_t base = plane ? (uint32_t)(vdp_reg[4] & 7) << 13 : (uint32_t)(vdp_reg[2] & 0x38) << 10;
    uint32_t hsbase = (uint32_t)(vdp_reg[13] & 0x3F) << 10, hsaddr;
    int hs, vs, y, x, col;
    switch (vdp_reg[11] & 3) {
    case 0: hsaddr = hsbase; break;
    case 2: hsaddr = hsbase + (uint32_t)(screen_y & ~7) * 4; break;
    case 3: hsaddr = hsbase + (uint32_t)screen_y * 4; break;
    default: hsaddr = hsbase + (uint32_t)(screen_y & 7) * 4; break;
    }
    hsaddr = (hsaddr + (plane ? 2u : 0u)) & 0xFFFF;
    hs = ((vdp_vram[hsaddr] << 8) | vdp_vram[hsaddr + 1]) & 0x3FF;
    if (hs & 0x200) hs -= 0x400;
    vs = vdp_vsram[plane];        /* (whole-screen vertical scroll: the game does not use columns) */
    y = (screen_y + vs) & (ph * 8 - 1);
    for (col = -1; col <= x_to / 8; col++) {
        int sx = col * 8 + (hs & 7);
        int tx = (col - (hs >> 3)) & (pw - 1);
        uint32_t ea = (base + (uint32_t)((y >> 3) * pw + tx) * 2) & 0xFFFF;
        uint8_t px[8];
        int i;
        if (sx + 8 <= x_from || sx >= x_to) continue;
        tile_row(px, ((uint32_t)vdp_vram[ea] << 8) | vdp_vram[ea + 1], y & 7);
        for (i = 0; i < 8; i++) {
            x = sx + i;
            if (x >= x_from && x < x_to) line[x] = px[i];
        }
    }
}

/* One row of the sprite table, as the VDP draws it (earlier sprites in the link
 * list in front), but without its per-line limits. */
static void sprite_row(uint8_t *line, int w, int y)
{
    uint32_t base = (uint32_t)(vdp_reg[5] & 0x7E) << 9;
    int link = 0, count = 0;
    do {
        const uint8_t *s = &vdp_vram[(base + (uint32_t)link * 8) & 0xFFFF];
        int sy = (((s[0] << 8) | s[1]) & 0x3FF) - 128;
        int hs = ((s[2] >> 2) & 3) + 1, vs = (s[2] & 3) + 1;
        uint32_t attr = ((uint32_t)s[4] << 8) | s[5];
        int sx = (((s[6] << 8) | s[7]) & 0x1FF) - 128;
        link = s[3] & 0x7F;
        if (y >= sy && y < sy + vs * 8) {
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
                    if (x >= 0 && x < w && (px[i] & PIX_OPAQUE) && !(line[x] & PIX_OPAQUE)) line[x] = px[i];
                }
            }
        }
        count++;
    } while (link != 0 && count < 80);
}

#define VIEW_W 320

static uint32_t picture[VIEW_W * VDP_HEIGHT];
static int frame_ok;              /* every line so far drawn here */
int g_gameview_reason;            /* why the last frame was not drawn here (tests) */
static Region region[2];
static int nregions, win_from, win_to;   /* rows covered by the window plane */

/* At line 0: is the screen showing gameplay, and where are the cameras? */
static int frame_setup(void)
{
    int i;
    if (!vdp_live_picture()) { g_gameview_reason = 1; return 0; }
    if (vdp_width() != VIEW_W) { g_gameview_reason = 2; return 0; }
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
    } else {
        region[0].bottom = VDP_HEIGHT;
        nregions = 1;
    }
    (void)i;
    g_gameview_reason = 0;
    return 1;
}

void gameview_line(int line)
{
    uint8_t a[VIEW_W], b[VIEW_W], sp[VIEW_W];
    uint32_t *o, backdrop;
    const Region *r;
    int x;
    if (line == 0) frame_ok = frame_setup();
    if (!frame_ok || line < 0 || line >= VDP_HEIGHT) return;
    if (!vdp_live_picture()) { frame_ok = 0; g_gameview_reason = 7; return; }
    o = picture + (size_t)line * VIEW_W;
    /* rows covered by the window plane now (the game moves it during the frame
     * when the present list opens and closes): as the VDP drew them */
    {
        int wv = (vdp_reg[18] & 0x1F) * 8;
        int in_window = (vdp_reg[18] & 0x80) ? line >= wv : line < wv;
        if (vdp_reg[17] & 0x1F) { frame_ok = 0; g_gameview_reason = 3; return; }
        if (in_window) {
            memcpy(o, vdp_framebuffer() + (size_t)line * VDP_FB_PITCH, VIEW_W * sizeof(uint32_t));
            return;
        }
    }
    r = (nregions == 2 && line >= region[1].top) ? &region[1] : &region[0];
    /* At the last line of each half: does plane A (whichever the game points the
     * VDP at by then; split screen switches planes a line into the lower half)
     * show this floor here? */
    if (line == r->bottom - 1 && !plane_shows_level(r, win_from, win_to)) {
        frame_ok = 0;
        g_gameview_reason = 5 + (r == &region[1]);
        return;
    }
    memset(sp, 0, sizeof sp);
    /* The level: inside the original screen, plane A exactly as the VDP shows it
     * (the game sometimes redraws changed ground there a little after changing
     * the floor layout); outside it (wider views), from the floor layout. */
    (void)level_row;
    plane_row(a, 0, VIEW_W, line, 0);
    plane_row(b, 0, VIEW_W, line, 1);
    sprite_row(sp, VIEW_W, line);
    backdrop = vdp_palette_rgb(vdp_reg[7] & 0x3F);
    for (x = 0; x < VIEW_W; x++) {
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
    *width = VIEW_W;
    *height = VDP_HEIGHT;
    *pitch = VIEW_W;
    return 1;
}
