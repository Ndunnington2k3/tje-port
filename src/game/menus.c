/*
 * Front-end menus.
 *
 * PlayerSelectMenu (the "Who..." screen), rewritten in C with two more items
 * (port features): "Load Game", which opens a list of the saved games, and
 * "Options". They use the game's menu screen (menu.c).
 */
#include "game.h"
#include "save.h"
#include "core/vdp.h"
#include "art.h"
#include "menu.h"
#include "world.h"
#include "options_menu.h"

#include <stdio.h>
#include <string.h>

#define WHO_ITEMS_ROM   0x024246u   /* the original three items */
#define WHO_COUNT_ROM   0x0242AEu
#define WHO_LOAD        3           /* values of the new items */
#define WHO_OPTIONS     4

#define SND_PRIORITY    0x32
#define SND_BUZZ        0x1A

/* The saved-games list has its own title, "LOAD?" (art/load_title.png), drawn
 * into the tiles of the "WHO..." title sprites (palette line 1) with our own
 * colours. Called by the frame hook just before each frame is drawn, because
 * the game re-uploads the title's tiles every few frames. Only tiles and
 * colours are written: the sprites stay where the game puts them, and each
 * block shows the part of the picture under it, so a block placed a pixel
 * lower (the third one is) simply shows the picture a row further on.
 * Palette line 1 is only used by the title; LoadMenu reloads it for the
 * other menus. */
static void load_title_frame(void)
{
    const ArtImage *art = &art_load_title;
    uint32_t sat = (uint32_t)(vdp_reg[5] & 0x7F) << 9;
    int i, n = 0, link = 0, base_x = 0, base_y = 0;
    if (vdp_reg[0x0C] & 0x81) sat &= 0xFC00u;
    for (i = 0; i < 80; i++) {
        const uint8_t *e = &vdp_vram[(sat + (uint32_t)link * 8) & 0xFFFF];
        uint32_t attr = (uint32_t)(e[4] << 8) | e[5];
        if (((attr >> 13) & 3) == 1) {                  /* a title sprite */
            uint32_t tile = attr & 0x7FFu;
            int x0 = ((e[6] << 8) | e[7]) & 0x1FF, y0 = ((e[0] << 8) | e[1]) & 0x3FF;
            int w = ((e[2] >> 2) & 3) + 1, h = (e[2] & 3) + 1;   /* in tiles */
            int tx, ty, x, y;
            if (n++ == 0) { base_x = x0; base_y = y0 - 1; }   /* picture row 0 is empty */
            for (tx = 0; tx < w; tx++)
                for (ty = 0; ty < h; ty++) {
                    /* sprite tiles go down each column first */
                    uint8_t *dst = &vdp_vram[((tile + (uint32_t)(tx * h + ty)) * 32) & 0xFFFF];
                    for (y = 0; y < 8; y++)
                        for (x = 0; x < 8; x += 2) {
                            int ix = x0 - base_x + tx * 8 + x, iy = y0 - base_y + ty * 8 + y;
                            int p0 = 0, p1 = 0;
                            if (iy >= 0 && iy < art->height && ix >= 0 && ix < art->width) {
                                p0 = art->pixels[iy * art->width + ix];
                                if (ix + 1 < art->width) p1 = art->pixels[iy * art->width + ix + 1];
                            }
                            dst[y * 4 + x / 2] = (uint8_t)(p0 << 4 | p1);
                        }
                }
        }
        link = e[3] & 0x7F;
        if (!link) break;
    }
    for (i = 0; i < art->colors; i++)
        vdp_cram[16 + art->palette[i].slot] = art->palette[i].color;
    vdp_refresh_palette();
}

/* The saved-games list. Loading a file does not return. */
static void load_game_menu(void)
{
    static const char *const names[SAVE_SLOTS] = {"File 1", "File 2", "File 3"};
    for (;;) {
        Menu m;
        SaveInfo info[SAVE_SLOTS];
        char line[64];
        int i, choice;
        const int back = SAVE_SLOTS;

        menu_init(&m);
        m.b_is_back = 1;
        for (i = 0; i < SAVE_SLOTS; i++) save_info(i, &info[i]);
        /* choosable: the saved files and Back */
        for (i = 0; i < SAVE_SLOTS; i++) {
            if (!info[i].used) continue;
            if (info[i].world == WORLD_FIXED) snprintf(line, sizeof line, "%s -- Fixed World", names[i]);
            else {
                char code[8];
                world_code(info[i].seed, code);
                snprintf(line, sizeof line, "%s -- %s World %s", names[i],
                         info[i].world == WORLD_CUSTOM ? "Custom" : "Random", info[i].seed ? code : "");
            }
            menu_add(&m, 6, 10 + i * 4, line, i);
        }
        menu_add(&m, 6, 22, "Back", back);
        /* not choosable: empty files and the details under each file */
        for (i = 0; i < SAVE_SLOTS; i++) {
            if (!info[i].used) {
                snprintf(line, sizeof line, "%s -- Empty!", names[i]);
                menu_add(&m, 6, 10 + i * 4, line, -1);
                continue;
            }
            {
                char floors[32];
                if (info[i].players == 2 && info[i].floor[0] != info[i].floor[1])
                    snprintf(floors, sizeof floors, "Floors %d and %d", info[i].floor[0], info[i].floor[1]);
                else
                    snprintf(floors, sizeof floors, "Floor %d", info[i].floor[info[i].players == 1 ? 1 : 0]);
                snprintf(line, sizeof line, "%d Ship Piece%s -- %s", info[i].ship_pieces,
                         info[i].ship_pieces == 1 ? "" : "s", floors);
                menu_add(&m, 10, 11 + i * 4, line, -1);
            }
        }
        genesis_set_frame_hook(load_title_frame);
        choice = menu_run(&m, MENU_TITLE_WHO);
        genesis_set_frame_hook(NULL);
        if (choice < 0 || choice == back) return;
        if (choice < SAVE_SLOTS && info[choice].used) {
            if (save_load(choice) != 0) {
                /* unreadable: say so with the game's "no" sound and show the list again */
                push32(SND_PRIORITY);
                push32(SND_BUZZ);
                CALL(f_03F700_PlaySound);
                POP_ARGS(2);
            }
        }
    }
}

TJE_OVERRIDE(0x0235E2)
void f_0235E2_PlayerSelectMenu(void)
{
    /* Returns 0 for both players, 1 for ToeJam, 2 for Earl (as the original). */
    int choice, sel = 0;
    CY(40);
    for (;;) {
        Menu m;
        int i, n = (int16_t)rd16(WHO_COUNT_ROM);
        menu_init(&m);
        for (i = 0; i < n && i < 8; i++) {
            uint32_t e = WHO_ITEMS_ROM + (uint32_t)i * 8, s = rd32(e + 2);
            char text[MENU_TEXT];
            int k;
            for (k = 0; k < MENU_TEXT - 1 && rd8(s + (uint32_t)k); k++) text[k] = (char)rd8(s + (uint32_t)k);
            text[k] = 0;
            /* two rows higher than the original, so the longer list sits in the middle */
            menu_add(&m, (int)rd8(e), (int)rd8(e + 1) - 2, text, (int16_t)rd16(e + 6));
        }
        menu_add(&m, 6, 19, "Load Game", WHO_LOAD);
        menu_add(&m, 6, 21, "Options", WHO_OPTIONS);
        m.sel = sel;
        choice = menu_run(&m, MENU_TITLE_WHO);
        sel = m.sel;
        if (choice == WHO_LOAD) load_game_menu();
        else if (choice == WHO_OPTIONS) options_menu();
        else break;
    }
    R.d[0] = (uint32_t)(int32_t)choice;
    RETURN();
}
