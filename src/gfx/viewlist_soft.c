/* A view list drawn in software, exactly as a GPU draws it (gfx/viewlist.h): the
 * reference the GPU backends are checked against. */
#include "gfx/viewlist.h"
#include <stddef.h>

static void piece(const ViewList *vl, const VlPiece *p, int high, uint32_t *out, int pitch);
static void cell(const ViewList *vl, const VlCell *c, const VlBatch *b, int high, uint32_t *out, int pitch)
{
    unsigned e = c->entry, tile = e & 0x7FF;
    int r, i, line = (int)((e >> 13) & 3);
    const uint8_t *tiles = (c->ext & 1) ? vl->cache : vl->vram;
    if (((e >> 15) & 1) != (unsigned)high) return;
    for (r = 0; r < 8; r++) {
        int y = c->y + r, tr = (e & 0x1000) ? 7 - r : r;
        const uint8_t *row = tiles + tile * 32 + tr * 4;
        if (y < b->y0 || y >= b->y1 || y < 0 || y >= vl->h) continue;
        for (i = 0; i < 8; i++) {
            int x = c->x + i, ti = (e & 0x0800) ? 7 - i : i, v;
            if (x < b->x0 || x >= b->x1 || x < 0 || x >= vl->w) continue;
            v = (ti & 1) ? (row[ti >> 1] & 15) : (row[ti >> 1] >> 4);
            if (v && !(v == 15 && (c->ext & 2))) out[(size_t)y * pitch + x] = vl->pal[line * 16 + v];
        }
    }
}

static void piece(const ViewList *vl, const VlPiece *p, int high, uint32_t *out, int pitch)
{
    int r, i;
    for (r = 0; r < p->h; r++) {
        int y = p->y + r;
        if (y < p->y0 || y >= p->y1 || y < 0 || y >= vl->h) continue;
        for (i = 0; i < p->w; i++) {
            int x = p->x + i;
            uint8_t v = p->px[r * p->stride + i];
            if (x < 0 || x >= vl->w || !(v & 0x80) || ((v >> 6) & 1) != high) continue;
            out[(size_t)y * pitch + x] = vl->pal[v & 0x3F];
        }
    }
}

void viewlist_render_soft(const ViewList *vl, uint32_t *out, int pitch)
{
    int x, y, high, k, layer, stage;
    for (y = 0; y < vl->h; y++)
        for (x = 0; x < vl->w; x++) out[(size_t)y * pitch + x] = vl->backdrop;
    for (stage = 0; stage < 2; stage++) {
        for (k = 0; k < vl->nbatches; k++) {
            const VlBatch *b = &vl->batches[k];
            if (b->stage != stage || b->layer != 3) continue;
            for (y = b->y0; y < b->y1; y++)
                for (x = b->x0; x < b->x1; x++) out[(size_t)y * pitch + x] = vl->backdrop;
        }
        for (high = 0; high < 2; high++) {
            for (layer = 0; layer < 3; layer++)
                for (k = 0; k < vl->nbatches; k++) {
                    const VlBatch *b = &vl->batches[k];
                    int c;
                    if (b->layer != layer || b->stage != stage) continue;
                    if (b->piece >= 0) { piece(vl, &vl->pieces[b->piece], high, out, pitch); continue; }
                    for (c = b->first; c < b->first + b->count; c++) cell(vl, &vl->cells[c], b, high, out, pitch);
                }
            if (!vl->pieces_in_batches && stage == 0)
                for (k = 0; k < vl->npieces; k++) piece(vl, &vl->pieces[k], high, out, pitch);
        }
    }
    for (k = 0; k < vl->noverlays; k++) {
        const VlOverlay *o = &vl->overlays[k];
        for (y = 0; y < o->h; y++)
            for (x = 0; x < o->w; x++) {
                int px = o->x + x, py = o->y + y;
                uint32_t c;
                if (o->tile_w) {
                    int u = (px - o->tx0) % o->tile_w, v = (py - o->ty0) % o->tile_h;
                    if (u < 0) u += o->tile_w;
                    if (v < 0) v += o->tile_h;
                    c = o->px[(size_t)v * o->pitch + u];
                } else c = o->px[(size_t)y * o->pitch + x];
                if (o->keyed && !(c >> 24)) continue;
                if (px >= 0 && px < vl->w && py >= 0 && py < vl->h)
                    out[(size_t)py * pitch + px] = c & 0xFFFFFF;
            }
    }
}
