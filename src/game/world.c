/*
 * World codes and the What screen (GameSelectMenu) with "Custom World".
 * See world.h.
 */
#include "game.h"
#include "menu.h"
#include "world.h"

#include <stdio.h>
#include <string.h>

#define WHAT_ITEMS_ROM    0x0242B0u   /* the original five items */
#define WHAT_COUNT_ROM    0x024340u
#define WHAT_RANDOM       0           /* item values (MenuRoutine's jump table) */
#define WHAT_CUSTOM       5           /* new: MenuRoutine never sees it */

#define CURRENT_RANDOM    0xFF91E2u
#define FIXED_WORLD_FLAG  0xFF9134u
#define SEED_LIMIT        0x7FFFFFFEu /* seeds are 1..SEED_LIMIT */

int g_world_kind = WORLD_RANDOM;
uint32_t g_world_seed;

static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
static uint32_t pending_seed;          /* Custom World: seed for the next new game */

void world_code(uint32_t seed, char out[WORLD_CODE_LEN + 1])
{
    uint32_t v = seed ? seed - 1 : 0;
    int i;
    for (i = WORLD_CODE_LEN - 1; i >= 0; i--) {
        out[i] = digits[v % 36];
        v /= 36;
    }
    out[WORLD_CODE_LEN] = 0;
}

uint32_t world_seed_from_code(const char *code)
{
    uint64_t v = 0;
    int i;
    for (i = 0; i < WORLD_CODE_LEN; i++) {
        const char *p = code[i] ? strchr(digits, code[i]) : NULL;
        v = v * 36 + (uint64_t)(p ? p - digits : 0);
    }
    return (uint32_t)(v % SEED_LIMIT) + 1;
}

/* GenerateLevelsForGame: StartGame has just seeded the random numbers. A Custom
 * World replaces that seed; either way the seed is remembered (world code). */
TJE_OVERRIDE(0x004658)
void f_004658_GenerateLevelsForGame(void)
{
    if (pending_seed) {
        wr32(CURRENT_RANDOM, pending_seed);
        g_world_kind = WORLD_CUSTOM;
        pending_seed = 0;
    } else {
        g_world_kind = rd8(FIXED_WORLD_FLAG) ? WORLD_FIXED : WORLD_RANDOM;
    }
    g_world_seed = g_world_kind == WORLD_FIXED ? 0 : rd32(CURRENT_RANDOM);
    orig_f_004658_GenerateLevelsForGame();
}

/* ---- the world code screen ----------------------------------------------------- */

typedef struct {
    char code[WORLD_CODE_LEN + 1];
    int pos;                       /* character being changed */
    int code_item, first_char_item, hint_item;
} CodeCtx;

#define CODE_COL   15
#define CODE_ROW   13

static void show_char(Menu *m, int i, int hidden)
{
    CodeCtx *c = (CodeCtx *)m->user;
    char s[2] = {hidden ? ' ' : c->code[i], 0};
    menu_set_text(m, c->first_char_item + i, s);
}

/* While the code is being changed (the code line's .edit is set), Up/Down change
 * the blinking letter; otherwise they move the hand as usual. */
static void set_editing(Menu *m, int on)
{
    CodeCtx *c = (CodeCtx *)m->user;
    m->item[c->code_item].edit = on;
    show_char(m, c->pos, 0);
    menu_set_text(m, c->hint_item, on ? "Up and Down -- Change Letter" : "Start -- Change the Code");
    menu_set_text(m, c->hint_item + 1, on ? "Left and Right -- Move" : "");
    menu_set_text(m, c->hint_item + 2, on ? "Start -- Done" : "");
}

static void code_frame(Menu *m, unsigned t)
{
    CodeCtx *c = (CodeCtx *)m->user;
    if (m->item[c->code_item].edit) show_char(m, c->pos, (t % 32) >= 24);
}

static int code_action(Menu *m, int dir)
{
    CodeCtx *c = (CodeCtx *)m->user;
    const char *p;
    int k;
    if (m->sel != c->code_item) return dir == MENU_SELECT;   /* Play / Back */
    switch (dir) {
    case MENU_SELECT:
    case MENU_BACK:
        set_editing(m, !m->item[c->code_item].edit && dir == MENU_SELECT);
        return 0;
    case MENU_LEFT:
    case MENU_RIGHT:
        show_char(m, c->pos, 0);
        c->pos = (c->pos + (dir == MENU_RIGHT ? 1 : WORLD_CODE_LEN - 1)) % WORLD_CODE_LEN;
        if (!m->item[c->code_item].edit) set_editing(m, 1);
        return 0;
    case MENU_UP:
    case MENU_DOWN:
        p = strchr(digits, c->code[c->pos]);
        k = p ? (int)(p - digits) : 0;
        k = (k + (dir == MENU_DOWN ? 1 : 35)) % 36;
        c->code[c->pos] = digits[k];
        show_char(m, c->pos, 0);
        return 0;
    }
    return 0;
}

/* Returns the seed to play, or 0 to go back. */
static uint32_t code_screen(void)
{
    Menu m;
    CodeCtx c;
    int i, v;
    enum { CODE, PLAY, BACK };

    memset(&c, 0, sizeof c);
    /* start from the last world played, so its code can be read off and shared */
    world_code(g_world_seed ? g_world_seed : 1, c.code);
    menu_init(&m);
    m.user = &c;
    m.action = code_action;
    m.frame = code_frame;
    m.b_is_back = 1;
    m.hand_col = 9;
    c.code_item = menu_add(&m, 13, CODE_ROW, "", CODE);
    menu_add(&m, 13, 17, "Play", PLAY);
    menu_add(&m, 13, 19, "Back", BACK);
    menu_add(&m, 9, 10, "Enter a World Code", -1);
    c.hint_item = menu_add(&m, 9, 21, "Start -- Change the Code", -1);   /* the font has no ':' or '/' */
    menu_add(&m, 9, 23, "", -1);
    menu_add(&m, 9, 25, "", -1);
    c.first_char_item = m.count;
    for (i = 0; i < WORLD_CODE_LEN; i++) {
        char s[2] = {c.code[i], 0};
        menu_add(&m, CODE_COL + i * 2, CODE_ROW, s, -1);
    }
    v = menu_run(&m, MENU_TITLE_WHAT);
    if (v == PLAY) return world_seed_from_code(c.code);
    return 0;
}

/* ---- the What screen ---------------------------------------------------------------- */

TJE_OVERRIDE(0x023654)
void f_023654_GameSelectMenu(void)
{
    /* Returns the choice in D0, as the original: 0 Random World, 1 Jam Out,
     * 2 intro, 3 controller options, 4 Fixed World. */
    int sel = 0, v;
    CY(40);
    for (;;) {
        Menu m;
        int i, n = (int16_t)rd16(WHAT_COUNT_ROM);
        menu_init(&m);
        for (i = 0; i < n && i < MENU_MAX - 1; i++) {
            uint32_t e = WHAT_ITEMS_ROM + (uint32_t)i * 8, s = rd32(e + 2);
            char text[MENU_TEXT];
            int k;
            for (k = 0; k < MENU_TEXT - 1 && rd8(s + (uint32_t)k); k++) text[k] = (char)rd8(s + (uint32_t)k);
            text[k] = 0;
            menu_add(&m, (int)rd8(e), (int)rd8(e + 1), text, (int16_t)rd16(e + 6));
            if (i == 0)   /* right under Random World, in the gap the original leaves */
                menu_add(&m, 6, 12, "Play New Game -- Custom World", WHAT_CUSTOM);
        }
        m.sel = sel;
        v = menu_run(&m, MENU_TITLE_WHAT);
        sel = m.sel;
        if (v != WHAT_CUSTOM) break;
        pending_seed = code_screen();
        if (pending_seed) { v = WHAT_RANDOM; break; }   /* starts like Random World */
    }
    if (v != WHAT_CUSTOM && v != WHAT_RANDOM) pending_seed = 0;
    R.d[0] = (uint32_t)(int32_t)v;
    RETURN();
}
