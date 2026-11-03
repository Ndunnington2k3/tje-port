/* A view list drawn in software, exactly as a GPU draws it (gfx/viewlist.h): the
 * reference the GPU backends are checked against. */
#include "gfx/viewlist.h"
#include <stddef.h>

static void cell(const ViewList *vl, const VlCell *c, const VlBatch *b, int high, uint32_t *out, int pitch)
{
    unsigned e = c->entry, tile = e & 0x7FF;
    int r, i, line = (int)((e >> 13) & 3);
    if (((e >> 15) & 1) != (unsigned)high) return;
    for (r = 0; r < 8; r++) {
        int y = c->y + r, tr = (e & 0x1000) ? 7 - r : r;
        const uint8_t *row = vl->vram + tile * 32 + tr * 4;
        if (y < b->y0 || y >= b->y1 || y < 0 || y >= vl->h) continue;
        for (i = 0; i < 8; i++) {
            int x = c->x + i, ti = (e & 0x0800) ? 7 - i : i, v;
            if (x < b->x0 || x >= b->x1 || x < 0 || x >= vl->w) continue;
            v = (ti & 1) ? (row[ti >> 1] & 15) : (row[ti >> 1] >> 4);
            if (v) out[(size_t)y * pitch + x] = vl->pal[line * 16 + v];
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
    int x, y, high, k, layer;
    for (y = 0; y < vl->h; y++)
        for (x = 0; x < vl->w; x++) out[(size_t)y * pitch + x] = vl->backdrop;
    for (high = 0; high < 2; high++) {
        for (layer = 0; layer < 2; layer++)
            for (k = 0; k < vl->nbatches; k++) {
                const VlBatch *b = &vl->batches[k];
                int c;
                if (b->layer != layer) continue;
                for (c = b->first; c < b->first + b->count; c++) cell(vl, &vl->cells[c], b, high, out, pitch);
            }
        for (k = 0; k < vl->npieces; k++) piece(vl, &vl->pieces[k], high, out, pitch);
    }
    for (k = 0; k < vl->noverlays; k++) {
        const VlOverlay *o = &vl->overlays[k];
        for (y = 0; y < o->h; y++)
            for (x = 0; x < o->w; x++) {
                int px = o->x + x, py = o->y + y;
                if (px >= 0 && px < vl->w && py >= 0 && py < vl->h)
                    out[(size_t)py * pitch + px] = o->px[(size_t)y * o->pitch + x];
            }
    }
}
