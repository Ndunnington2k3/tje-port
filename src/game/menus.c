/*
 * Front-end menus.
 *
 * PlayerSelectMenu (the "Who..." screen), rewritten in C with one more item
 * (port feature): "Load Game", which opens a list of the saved games. Both use
 * the game's own menu routine (LOC_00023784) with item tables the port puts in
 * ROM space (genesis_port_rom), so they look and behave like the other menus.
 */
#include "game.h"
#include "save.h"
#include "core/vdp.h"

#include <stdio.h>
#include <string.h>

#define WHO_ITEMS_ROM   0x024246u   /* the original three items */
#define WHO_COUNT_ROM   0x0242AEu
#define MENU_WHO        0           /* LoadMenu title: "Who..." */

#define SND_PRIORITY    0x32
#define SND_BUZZ        0x1A

/* Menu item tables: 8 bytes per item (column, row, string pointer, value).
 * A negative value is a line that cannot be selected; such lines must come after
 * all selectable ones, as the cursor stops at the first of them. */
typedef struct {
    uint8_t bytes[16 * 8];
    int count;
} ItemTable;

static void add_item(ItemTable *t, int col, int row, uint32_t text, int value)
{
    uint8_t *e = t->bytes + t->count * 8;
    e[0] = (uint8_t)col;
    e[1] = (uint8_t)row;
    e[2] = (uint8_t)(text >> 24); e[3] = (uint8_t)(text >> 16); e[4] = (uint8_t)(text >> 8); e[5] = (uint8_t)text;
    e[6] = (uint8_t)((unsigned)value >> 8); e[7] = (uint8_t)value;
    t->count++;
}

uint8_t g_menu_hand[8 * 32];
int g_menu_hand_ok;

/* Keep a copy of the hand's tiles: the 4x2-tile sprite on palette line 3. */
static void grab_hand(void)
{
    uint32_t sat = (uint32_t)(vdp_reg[5] & 0x7F) << 9;
    int i, link = 0;
    if (vdp_reg[0x0C] & 0x81) sat &= 0xFC00u;
    for (i = 0; i < 80; i++) {
        const uint8_t *e = &vdp_vram[(sat + (uint32_t)link * 8) & 0xFFFF];
        uint32_t attr = (uint32_t)(e[4] << 8) | e[5];
        if (e[2] == 0x0D && ((attr >> 13) & 3) == 3) {
            memcpy(g_menu_hand, &vdp_vram[((attr & 0x7FFu) * 32) & 0xFFFF], sizeof g_menu_hand);
            g_menu_hand_ok = 1;
            return;
        }
        link = e[3] & 0x7F;
        if (!link) return;
    }
}

static uint32_t rom_string(const char *s)
{
    return genesis_port_rom(s, (uint32_t)strlen(s) + 1);
}

/* Show a menu with LOC_00023784 under one of the game's titles; returns the
 * value of the chosen item. */
static int run_menu(int title, const ItemTable *t)
{
    uint32_t table = genesis_port_rom(t->bytes, (uint32_t)t->count * 8);
    int choice;
    push32((uint32_t)title);
    CALL(f_023B4E_LoadMenu);
    push32((uint32_t)t->count);
    push32(table);
    push32(2);
    CALL(f_023784);
    POP_ARGS(3);
    grab_hand();
    choice = (int16_t)(R.d[0] & 0xFFFF);
    CALL(f_023BDA);                /* takes the title away (same argument) */
    POP_ARGS(1);
    return choice;
}

/* The saved-games list. Loading a file does not return. */
static void load_game_menu(void)
{
    static const char *const names[SAVE_SLOTS] = {"File 1", "File 2", "File 3"};
    for (;;) {
        ItemTable t;
        SaveInfo info[SAVE_SLOTS];
        char line[64];
        int i, choice;
        const int back = SAVE_SLOTS;

        genesis_port_rom_reset();
        t.count = 0;
        for (i = 0; i < SAVE_SLOTS; i++) save_info(i, &info[i]);
        /* selectable: the saved files and Back */
        for (i = 0; i < SAVE_SLOTS; i++) {
            if (!info[i].used) continue;
            snprintf(line, sizeof line, "%s -- %s World", names[i], info[i].fixed_world ? "Fixed" : "Random");
            add_item(&t, 6, 10 + i * 4, rom_string(line), i);
        }
        add_item(&t, 6, 22, rom_string("Back"), back);
        /* not selectable: empty files and the details under each file */
        for (i = 0; i < SAVE_SLOTS; i++) {
            if (!info[i].used) {
                snprintf(line, sizeof line, "%s -- Empty!", names[i]);
                add_item(&t, 6, 10 + i * 4, rom_string(line), -1);
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
                add_item(&t, 10, 11 + i * 4, rom_string(line), -1);
            }
        }
        choice = run_menu(MENU_WHO, &t);
        if (choice == back) return;
        if (choice >= 0 && choice < SAVE_SLOTS && info[choice].used) {
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
    int choice;
    CY(40);
    for (;;) {
        ItemTable t;
        int i, n = (int16_t)rd16(WHO_COUNT_ROM);
        genesis_port_rom_reset();
        t.count = 0;
        for (i = 0; i < n && i < 15; i++) {
            memcpy(t.bytes + t.count * 8, g_rom + WHO_ITEMS_ROM + (uint32_t)i * 8, 8);
            t.count++;
        }
        add_item(&t, 6, 21, rom_string("Load Game"), 3);
        choice = run_menu(MENU_WHO, &t);
        if (choice != 3) break;
        load_game_menu();
    }
    R.d[0] = (uint32_t)(int32_t)choice;
    RETURN();
}
