#include "vdp.h"
#include "m68k_rt.h"
#include "genesis.h"
#include "ser.h"

#include <string.h>

uint8_t vdp_vram[0x10000];
uint16_t vdp_cram[64];
uint16_t vdp_vsram[40];
uint8_t vdp_reg[32];

static uint32_t framebuffer[VDP_MAX_WIDTH * VDP_HEIGHT];
static uint32_t palette_rgb[64];

static int write_pending;
static uint32_t code;          /* CD5..CD0 */
static uint32_t addr;          /* 16-bit VRAM address (17 bits for 128K mode, unused) */
static int fill_pending;
static int vblank_flag;
static int vint_flag;
static int cur_line;
static int freeze_frames;          /* clean transitions: -1 frozen until released, n > 0: n more frames */
static int frame_visible = 1;      /* clean transitions: this frame may be shown */
static int frames_on = 2;          /* clean transitions: frames in a row with the display on at line 0 */

static const uint8_t level3[8] = {0, 52, 87, 116, 144, 172, 206, 255};

/* Port overlays: pictures drawn over the finished line, outside the Genesis
 * hardware (no tiles, sprites or palette lines of the game are used). */
#define OVERLAYS 8
static struct {
    int on, x, y, w, h;
    const uint8_t *px;        /* w*h colour numbers, 0 = clear */
    uint32_t rgb[16];
} overlay[OVERLAYS];

static uint32_t genesis_rgb(uint16_t c);

void vdp_overlay(int slot, int x, int y, int w, int h, const uint8_t *pixels, const uint16_t colors[16])
{
    int i;
    if (slot < 0 || slot >= OVERLAYS) return;
    overlay[slot].x = x; overlay[slot].y = y; overlay[slot].w = w; overlay[slot].h = h;
    overlay[slot].px = pixels;
    for (i = 0; i < 16; i++) overlay[slot].rgb[i] = genesis_rgb(colors[i]);
    overlay[slot].on = pixels != NULL;
}

void vdp_overlay_off(int slot)
{
    if (slot >= 0 && slot < OVERLAYS) overlay[slot].on = 0;
}

static void draw_overlays(uint32_t *fb, int line, int width)
{
    int i, x;
    for (i = 0; i < OVERLAYS; i++) {
        const uint8_t *row;
        if (!overlay[i].on || line < overlay[i].y || line >= overlay[i].y + overlay[i].h) continue;
        row = overlay[i].px + (line - overlay[i].y) * overlay[i].w;
        for (x = 0; x < overlay[i].w; x++) {
            int sx = overlay[i].x + x;
            if (sx >= 0 && sx < width && row[x]) fb[sx] = overlay[i].rgb[row[x] & 15];
        }
    }
}

static uint32_t genesis_rgb(uint16_t c)
{
    uint32_t r = level3[(c >> 1) & 7], g = level3[(c >> 5) & 7], b = level3[(c >> 9) & 7];
    return (r << 16) | (g << 8) | b;
}

static void update_palette(int i)
{
    palette_rgb[i] = genesis_rgb(vdp_cram[i]);
}

void vdp_reset(void)
{
    int i;
    memset(vdp_vram, 0, sizeof vdp_vram);
    memset(vdp_cram, 0, sizeof vdp_cram);
    memset(vdp_vsram, 0, sizeof vdp_vsram);
    memset(vdp_reg, 0, sizeof vdp_reg);
    memset(framebuffer, 0, sizeof framebuffer);
    for (i = 0; i < 64; i++) update_palette(i);
    write_pending = 0;
    code = addr = 0;
    fill_pending = 0;
    vblank_flag = vint_flag = 0;
    cur_line = 0;
    freeze_frames = 0;
    memset(overlay, 0, sizeof overlay);
    frame_visible = 1;
    frames_on = 2;
}

void vdp_refresh_palette(void)
{
    int i;
    for (i = 0; i < 64; i++) update_palette(i);
}

int vdp_width(void) { return (vdp_reg[12] & 0x01) ? 320 : 256; }
const uint32_t *vdp_framebuffer(void) { return framebuffer; }
int vdp_vint_enabled(void) { return (vdp_reg[1] & 0x20) != 0; }
int vdp_hint_enabled(void) { return (vdp_reg[0] & 0x10) != 0; }
int vdp_hint_reload(void) { return vdp_reg[10]; }
void vdp_set_line(int line) { cur_line = line; }
void vdp_set_vblank(int on)
{
    vblank_flag = on;
    if (on) {
        vint_flag = 1;
        if (freeze_frames > 0) freeze_frames--;    /* a frozen frame was shown */
    }
}
void vdp_ack_vint(void) { vint_flag = 0; }

static uint32_t increment(void) { return vdp_reg[15]; }

/* per-frame statistics (debugging transitions) */
static uint32_t st_writes[4], st_off_lines;

/* Clean transitions (port option). The original game shows a frame or two of
 * half-built screens when it changes scene:
 *  - the display is switched on part way down a frame (a torn frame), and the
 *    next frame can still show the old scene's sprites, because the sprite table
 *    is uploaded from a buffer the game rebuilds a frame later. So after the
 *    display has been off, a frame is only shown once the display was on at the
 *    top of this frame and the one before; until then it stays blank;
 *  - some scenes are rebuilt with the display on (the elevator ride): game code
 *    freezes the picture around that with vdp_freeze(). */
static int clean_transitions;

void vdp_set_clean_transitions(int on) { clean_transitions = on; }
void vdp_freeze(int frames) { freeze_frames = clean_transitions ? frames : 0; }

int vdp_frozen(void)
{
    return freeze_frames != 0;
}

int vdp_live_picture(void)
{
    return !freeze_frames && (vdp_reg[1] & 0x40) && (!clean_transitions || frame_visible);
}

uint32_t vdp_palette_rgb(int i)
{
    return palette_rgb[i & 0x3F];
}

static void stat_write(uint32_t a)
{
    uint32_t pa = (uint32_t)(vdp_reg[2] & 0x38) << 10, pb = (uint32_t)(vdp_reg[4] & 7) << 13;
    uint32_t sat = (uint32_t)(vdp_reg[5] & 0x7F) << 9;
    a &= 0xFFFF;
    if (a >= sat && a < sat + 0x280) st_writes[3]++;
    else if (a >= pa && a < pa + 0x2000) st_writes[1]++;
    else if (a >= pb && a < pb + 0x2000) st_writes[2]++;
    else st_writes[0]++;
}

void vdp_frame_stats(uint32_t out[5])
{
    int i;
    for (i = 0; i < 4; i++) { out[i] = st_writes[i]; st_writes[i] = 0; }
    out[4] = st_off_lines;
    st_off_lines = 0;
}

static void vram_write16(uint32_t a, uint32_t v)
{
    stat_write(a);
    a &= 0xFFFF;
    if (a & 1) v = ((v & 0xFF) << 8) | (v >> 8);  /* odd address writes byte-swapped */
    a &= 0xFFFE;
    vdp_vram[a] = (uint8_t)(v >> 8);
    vdp_vram[a + 1] = (uint8_t)v;
}

static void port_write(uint32_t v)
{
    switch (code & 0x0F) {
    case 1: vram_write16(addr, v); break;
    case 3: {
        int i = (addr >> 1) & 0x3F;
        vdp_cram[i] = (uint16_t)(v & 0x0EEE);
        update_palette(i);
        break;
    }
    case 5: {
        int i = (addr >> 1) & 0x3F;
        if (i < 40) vdp_vsram[i] = (uint16_t)(v & 0x07FF);
        break;
    }
    default: break;
    }
    addr = (addr + increment()) & 0xFFFF;
}

static uint32_t dma_length(void)
{
    uint32_t n = vdp_reg[19] | ((uint32_t)vdp_reg[20] << 8);
    return n ? n : 0x10000;
}

static void dma_68k(void)
{
    uint32_t len = dma_length();
    /* The 68000 is frozen while the VDP copies from its memory. Bytes per scanline:
     * blanking (or display off) 205 in H40 / 167 in H32; active display 18 / 16. */
    {
        int blank = vblank_flag || !(vdp_reg[1] & 0x40);
        int h40 = (vdp_reg[12] & 0x01) != 0;
        uint32_t per_line = blank ? (h40 ? 205u : 167u) : (h40 ? 18u : 16u);
        R.cycles += len * 2u * GEN_CYCLES_PER_LINE / per_line;
    }
    uint32_t src = ((uint32_t)vdp_reg[21] << 1) | ((uint32_t)vdp_reg[22] << 9) | ((uint32_t)(vdp_reg[23] & 0x7F) << 17);
    while (len--) {
        port_write(rd16(src));
        src = (src & 0xFE0000u) | ((src + 2) & 0x1FFFFu);  /* source wraps within 128K */
    }
    vdp_reg[19] = vdp_reg[20] = 0;
}

static void dma_fill(uint32_t v)
{
    uint32_t len = dma_length();
    uint8_t b = (uint8_t)(v >> 8);
    vram_write16(addr, v);
    addr = (addr + increment()) & 0xFFFF;
    while (--len) {
        vdp_vram[addr ^ 1] = b;
        stat_write(addr);
        addr = (addr + increment()) & 0xFFFF;
    }
    vdp_reg[19] = vdp_reg[20] = 0;
}

static void dma_copy(void)
{
    uint32_t len = dma_length();
    uint32_t src = vdp_reg[21] | ((uint32_t)vdp_reg[22] << 8);
    while (len--) {
        vdp_vram[addr ^ 1] = vdp_vram[src ^ 1];
        stat_write(addr);
        src = (src + 1) & 0xFFFF;
        addr = (addr + increment()) & 0xFFFF;
    }
    vdp_reg[19] = vdp_reg[20] = 0;
}

void vdp_write_control(uint32_t v)
{
    v &= 0xFFFF;
    if (!write_pending) {
        if ((v & 0xC000) == 0x8000) {
            int r = (v >> 8) & 0x1F;
            if (r < 24) vdp_reg[r] = (uint8_t)v;
            code &= ~0x3u;  /* register writes clear the pending code (approximation) */
            return;
        }
        code = (code & 0x3C) | (v >> 14);
        addr = (addr & 0xC000) | (v & 0x3FFF);
        write_pending = 1;
        return;
    }
    write_pending = 0;
    code = (code & 0x03) | ((v >> 2) & 0x3C);
    addr = (addr & 0x3FFF) | ((v & 3) << 14);
    if ((code & 0x20) && (vdp_reg[1] & 0x10)) {
        switch (vdp_reg[23] >> 6) {
        case 0: case 1: dma_68k(); break;
        case 2: fill_pending = 1; break;
        case 3: dma_copy(); break;
        }
    }
}

void vdp_write_data(uint32_t v)
{
    write_pending = 0;
    if (fill_pending) {
        fill_pending = 0;
        dma_fill(v);
        return;
    }
    port_write(v);
}

uint32_t vdp_read_data(void)
{
    uint32_t v = 0;
    write_pending = 0;
    switch (code & 0x0F) {
    case 0: v = ((uint32_t)vdp_vram[addr & 0xFFFE] << 8) | vdp_vram[(addr & 0xFFFE) + 1]; break;
    case 4: v = vdp_vsram[((addr >> 1) & 0x3F) % 40]; break;
    case 8: v = vdp_cram[(addr >> 1) & 0x3F]; break;
    }
    addr = (addr + increment()) & 0xFFFF;
    return v;
}

uint32_t vdp_read_status(void)
{
    uint32_t s = 0x3400;          /* FIFO empty */
    write_pending = 0;
    if (vint_flag) s |= 0x80;
    if (vblank_flag || !(vdp_reg[1] & 0x40)) s |= 0x08;
    /* toggle HBlank bit so polling loops make progress */
    if (R.cycles % GEN_CYCLES_PER_LINE > 400) s |= 0x04;
    return s;
}

uint32_t vdp_read_hv(void)
{
    uint32_t v = (uint32_t)(cur_line > 0xEA ? cur_line - 6 : cur_line) & 0xFF;
    uint32_t h = (R.cycles % GEN_CYCLES_PER_LINE) * 0xB6 / GEN_CYCLES_PER_LINE;
    return (v << 8) | h;
}

/* ---- rendering ---------------------------------------------------------- */

/* Line buffer entries: bits 0-5 colour index (pal*16+idx), bit 6 priority, bit 7 opaque */
#define PIX_OPAQUE 0x80
#define PIX_PRIO   0x40

static void draw_tile_row(uint8_t *out, int x, uint32_t entry, int row, int width)
{
    uint32_t tile = entry & 0x7FF;
    uint32_t pal = (entry >> 9) & 0x30;
    uint32_t prio = (entry & 0x8000) ? PIX_PRIO : 0;
    const uint8_t *p;
    int i;
    if (entry & 0x1000) row = 7 - row;
    p = &vdp_vram[(tile * 32 + row * 4) & 0xFFFF];
    for (i = 0; i < 8; i++) {
        int px = (entry & 0x0800) ? 7 - i : i;
        uint8_t b = p[px >> 1];
        uint32_t c = (px & 1) ? (b & 0x0F) : (b >> 4);
        int sx = x + i;
        if (sx < 0 || sx >= width) continue;
        out[sx] = (uint8_t)(c ? (PIX_OPAQUE | prio | pal | c) : prio);
    }
}

static int plane_dims(int *w, int *h)
{
    static const int sz[4] = {32, 64, 32, 128};
    *w = sz[vdp_reg[16] & 3];
    *h = sz[(vdp_reg[16] >> 4) & 3];
    return 0;
}

static void render_plane(uint8_t *out, int line, int width, uint32_t base, int hscroll, int plane_index,
                         int x_from, int x_to)
{
    int pw, ph, col;
    int vsmode = (vdp_reg[11] >> 2) & 1;
    plane_dims(&pw, &ph);
    for (col = -1; col <= width / 8; col++) {
        int sx = col * 8 + (hscroll & 7);
        int vscroll, y, tx, ty;
        uint32_t ea;
        uint32_t entry;
        if (sx + 8 <= x_from || sx >= x_to) continue;
        {
            int vcol = (sx < 0 ? 0 : sx) / 16;
            if (vcol > 19) vcol = 19;
            vscroll = vsmode ? vdp_vsram[vcol * 2 + plane_index] : vdp_vsram[plane_index];
        }
        y = (line + vscroll) & (ph * 8 - 1);
        tx = (col - (hscroll >> 3)) & (pw - 1);
        ty = y >> 3;
        ea = (base + (uint32_t)(ty * pw + tx) * 2) & 0xFFFF;
        entry = ((uint32_t)vdp_vram[ea] << 8) | vdp_vram[ea + 1];
        {
            uint8_t tmp[8];
            int i;
            draw_tile_row(tmp, 0, entry, y & 7, 8);
            for (i = 0; i < 8; i++) {
                int x = sx + i;
                if (x >= x_from && x < x_to && x >= 0 && x < width) out[x] = tmp[i];
            }
        }
    }
}

static void render_window(uint8_t *out, int line, int width, int x_from, int x_to)
{
    uint32_t base = (width == 320) ? ((uint32_t)(vdp_reg[3] & 0x3C) << 10) : ((uint32_t)(vdp_reg[3] & 0x3E) << 10);
    int pw = (width == 320) ? 64 : 32;
    int col;
    for (col = x_from / 8; col < x_to / 8; col++) {
        uint32_t ea = (base + (uint32_t)((line >> 3) * pw + col) * 2) & 0xFFFF;
        uint32_t entry = ((uint32_t)vdp_vram[ea] << 8) | vdp_vram[ea + 1];
        draw_tile_row(out, col * 8, entry, line & 7, width);
    }
}

static void render_sprites(uint8_t *out, int line, int width)
{
    uint32_t base = (width == 320) ? ((uint32_t)(vdp_reg[5] & 0x7E) << 9) : ((uint32_t)(vdp_reg[5] & 0x7F) << 9);
    int maxspr = (width == 320) ? 80 : 64;
    int perline = (width == 320) ? 20 : 16;
    int link = 0, count = 0, onl = 0, masked = 0;
    int dotbudget = width;
    do {
        const uint8_t *s = &vdp_vram[(base + link * 8) & 0xFFFF];
        int sy = (((s[0] << 8) | s[1]) & 0x3FF) - 128;
        int hs = ((s[2] >> 2) & 3) + 1;
        int vs = (s[2] & 3) + 1;
        uint32_t attr = ((uint32_t)s[4] << 8) | s[5];
        int sx = (((s[6] << 8) | s[7]) & 0x1FF) - 128;
        link = s[3] & 0x7F;
        if (line >= sy && line < sy + vs * 8) {
            if (sx == -128 && onl > 0) masked = 1;
            onl++;
            if (onl > perline) break;
            if (!masked) {
                int row = line - sy;
                int c;
                if (attr & 0x1000) row = vs * 8 - 1 - row;
                for (c = 0; c < hs; c++) {
                    int colx = (attr & 0x0800) ? (hs - 1 - c) : c;
                    uint32_t tile = (attr & 0x7FF) + (uint32_t)(colx * vs + (row >> 3));
                    uint32_t entry = (attr & 0xF800) | (tile & 0x7FF);
                    uint8_t tmp[8];
                    int i;
                    /* row already flipped at sprite level; draw unflipped vertically */
                    draw_tile_row(tmp, 0, entry & ~0x1000u, row & 7, 8);
                    for (i = 0; i < 8; i++) {
                        int x = sx + c * 8 + i;
                        if (x < 0 || x >= width) continue;
                        if ((tmp[i] & PIX_OPAQUE) && !(out[x] & PIX_OPAQUE)) out[x] = tmp[i];
                    }
                }
            }
            dotbudget -= hs * 8;
            if (dotbudget <= 0) break;
        }
        count++;
    } while (link != 0 && count < maxspr);
}

void vdp_render_line(int line)
{
    uint8_t la[VDP_MAX_WIDTH], lb[VDP_MAX_WIDTH], ls[VDP_MAX_WIDTH];
    int width = vdp_width();
    uint32_t *fb = &framebuffer[line * VDP_FB_PITCH];
    uint32_t bg = palette_rgb[vdp_reg[7] & 0x3F];
    int x;
    int hsmode = vdp_reg[11] & 3;
    uint32_t hsbase = (uint32_t)(vdp_reg[13] & 0x3F) << 10;
    uint32_t hsaddr;
    int hsa, hsb;
    int win_from = 0, win_to = 0;

    if (line == 0) {
        if (vdp_reg[1] & 0x40) { if (frames_on < 2) frames_on++; }
        else frames_on = 0;
        frame_visible = frames_on >= 2;
    }
    if (freeze_frames)
        return;                                    /* keep showing the last picture */
    if (!(vdp_reg[1] & 0x40) || (clean_transitions && !frame_visible)) {
        st_off_lines++;
        for (x = 0; x < VDP_MAX_WIDTH; x++) fb[x] = bg;
        return;
    }
    switch (hsmode) {
    case 0: hsaddr = hsbase; break;
    case 2: hsaddr = hsbase + (uint32_t)(line & ~7) * 4; break;
    case 3: hsaddr = hsbase + (uint32_t)line * 4; break;
    default: hsaddr = hsbase + (uint32_t)(line & 7) * 4; break;
    }
    hsaddr &= 0xFFFF;
    hsa = ((vdp_vram[hsaddr] << 8) | vdp_vram[hsaddr + 1]) & 0x3FF;
    hsb = ((vdp_vram[hsaddr + 2] << 8) | vdp_vram[hsaddr + 3]) & 0x3FF;
    if (hsa & 0x200) hsa -= 0x400;
    if (hsb & 0x200) hsb -= 0x400;

    /* window region on this line */
    {
        int wv = vdp_reg[18] & 0x1F, down = vdp_reg[18] & 0x80;
        int wh = (vdp_reg[17] & 0x1F) * 16, right = vdp_reg[17] & 0x80;
        int in_v = down ? (line >= wv * 8) : (line < wv * 8);
        if (in_v) { win_from = 0; win_to = width; }
        else if (wh) {
            if (right) { win_from = wh; win_to = width; }
            else { win_from = 0; win_to = wh; }
            if (win_to > width) win_to = width;
        }
    }

    memset(la, 0, sizeof la);
    memset(lb, 0, sizeof lb);
    memset(ls, 0, sizeof ls);
    render_plane(lb, line, width, (uint32_t)(vdp_reg[4] & 7) << 13, hsb, 1, 0, width);
    if (win_to > win_from) {
        render_plane(la, line, width, (uint32_t)(vdp_reg[2] & 0x38) << 10, hsa, 0, 0, win_from);
        render_plane(la, line, width, (uint32_t)(vdp_reg[2] & 0x38) << 10, hsa, 0, win_to, width);
        render_window(la, line, width, win_from, win_to);
    } else {
        render_plane(la, line, width, (uint32_t)(vdp_reg[2] & 0x38) << 10, hsa, 0, 0, width);
    }
    render_sprites(ls, line, width);

    for (x = 0; x < width; x++) {
        uint8_t a = la[x], b = lb[x], s = ls[x];
        uint8_t c = 0;
        int have = 0;
        /* highest priority first */
        if ((s & (PIX_OPAQUE | PIX_PRIO)) == (PIX_OPAQUE | PIX_PRIO)) { c = s; have = 1; }
        else if ((a & (PIX_OPAQUE | PIX_PRIO)) == (PIX_OPAQUE | PIX_PRIO)) { c = a; have = 1; }
        else if ((b & (PIX_OPAQUE | PIX_PRIO)) == (PIX_OPAQUE | PIX_PRIO)) { c = b; have = 1; }
        else if (s & PIX_OPAQUE) { c = s; have = 1; }
        else if (a & PIX_OPAQUE) { c = a; have = 1; }
        else if (b & PIX_OPAQUE) { c = b; have = 1; }
        fb[x] = have ? palette_rgb[c & 0x3F] : bg;
    }
    for (; x < VDP_MAX_WIDTH; x++) fb[x] = 0;
    draw_overlays(fb, line, width);
}

/* ---- snapshots ---------------------------------------------------------- */
void vdp_save(Ser *s)
{
    int i;
    ser_bytes(s, vdp_vram, sizeof vdp_vram);
    for (i = 0; i < 64; i++) ser_u16(s, vdp_cram[i]);
    for (i = 0; i < 40; i++) ser_u16(s, vdp_vsram[i]);
    ser_bytes(s, vdp_reg, sizeof vdp_reg);
    ser_u8(s, (uint32_t)write_pending);
    ser_u32(s, code);
    ser_u32(s, addr);
    ser_u8(s, (uint32_t)fill_pending);
    ser_u8(s, (uint32_t)vblank_flag);
    ser_u8(s, (uint32_t)vint_flag);
    ser_u16(s, (uint32_t)cur_line);
}

void vdp_load(Ser *s)
{
    int i;
    des_bytes(s, vdp_vram, sizeof vdp_vram);
    for (i = 0; i < 64; i++) vdp_cram[i] = (uint16_t)des_u16(s);
    for (i = 0; i < 40; i++) vdp_vsram[i] = (uint16_t)des_u16(s);
    des_bytes(s, vdp_reg, sizeof vdp_reg);
    write_pending = (int)des_u8(s);
    code = des_u32(s);
    addr = des_u32(s);
    fill_pending = (int)des_u8(s);
    vblank_flag = (int)des_u8(s);
    vint_flag = (int)des_u8(s);
    cur_line = (int)des_u16(s);
    vdp_refresh_palette();
}
