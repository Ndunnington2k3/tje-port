/*
 * The Options screens (from the Who screen): port settings, control bindings
 * and Exit Game.
 */
#include "game.h"
#include "menu.h"
#include "settings.h"
#include "options_menu.h"
#include "platform/platform.h"
#include "core/vdp.h"
#include "art.h"

#include <stdio.h>
#include <string.h>


/* All Options screens run under the "OPTIONS..." title (art/options_title.png). */
static int options_screen(Menu *m)
{
    int v;
    menu_art_title(&art_options_title);
    v = menu_run(m, MENU_NO_TITLE);
    menu_art_title(NULL);
    return v;
}

/* ---- Controls ---------------------------------------------------------------- */

typedef struct {
    int player;
    int key_item, pad_item;        /* menu index of the first keyboard / controller label */
} ControlsCtx;

/* Shown in the Genesis pad's order; the numbers are the PAD_* bit numbers. */
static const struct { const char *name; int bit; } pad_buttons[8] = {
    {"Up", 0}, {"Down", 1}, {"Left", 2}, {"Right", 3}, {"A", 6}, {"B", 4}, {"C", 5}, {"Start", 7},
};

#define CTRL_RESET   8
#define CTRL_BACK    9
#define CTRL_ROW     11             /* first button line */
#define COL_NAME     6
#define COL_KEY      15
#define COL_PAD      28

/* The game's font is proportional, so each column is its own line of text. */
static void refresh_controls(Menu *m)
{
    ControlsCtx *c = (ControlsCtx *)m->user;
    char label[32];
    int i;
    for (i = 0; i < 8; i++) {
        plat_binding_label(PLAT_BIND_KEY, plat_binding(c->player, pad_buttons[i].bit, PLAT_BIND_KEY), label, sizeof label);
        menu_set_text(m, c->key_item + i, label);
        plat_binding_label(PLAT_BIND_PAD, plat_binding(c->player, pad_buttons[i].bit, PLAT_BIND_PAD), label, sizeof label);
        menu_set_text(m, c->pad_item + i, label);
    }
}

/* Wait for a key or controller input for the button under the hand. */
static void capture_binding(Menu *m, int i)
{
    ControlsCtx *c = (ControlsCtx *)m->user;
    int bit = pad_buttons[i].bit, slot = 0, frames, r = 0, b;
    char token[32];

    menu_set_text(m, c->key_item + i, "Press a key or button...");
    menu_set_text(m, c->pad_item + i, "");
    plat_capture_start(c->player);
    for (frames = 0; frames < 60 * 8 && r == 0; frames++) {   /* gives up after 8 seconds */
        menu_frame();
        r = plat_capture_poll(&slot, token, sizeof token);
    }
    if (r == 0) plat_capture_cancel();
    /* Wait until everything is let go: the key just pressed may now be bound to
     * a button the menu reacts to (A would start another capture at once). */
    for (frames = 0; frames < 60 * 5 && ((g_pad[0] | g_pad[1]) & 0x1FFu); frames++)
        menu_frame();
    if (r == 1) {
        /* an input already used by another button of this player swaps over */
        char old[32];
        snprintf(old, sizeof old, "%s", plat_binding(c->player, bit, slot));
        for (b = 0; b < 8; b++)
            if (b != bit && !strcmp(plat_binding(c->player, b, slot), token))
                plat_set_binding(c->player, b, slot, old);
        plat_set_binding(c->player, bit, slot, token);
    }
    refresh_controls(m);
}

static int controls_action(Menu *m, int dir)
{
    int v = m->item[m->sel].value;
    if (dir != 0) return 0;
    if (v < 8) { capture_binding(m, v); return 0; }
    if (v == CTRL_RESET) { plat_default_bindings(((ControlsCtx *)m->user)->player); refresh_controls(m); return 0; }
    return 1;   /* Back */
}

static void controls_menu(int player)
{
    Menu m;
    ControlsCtx c;
    char line[32];
    int i;
    menu_init(&m);
    c.player = player;
    m.user = &c;
    m.action = controls_action;
    m.b_is_back = 1;
    /* choosable lines first (the hand stops at the first line that is not) */
    for (i = 0; i < 8; i++) menu_add(&m, COL_NAME, CTRL_ROW + i, pad_buttons[i].name, i);
    menu_add(&m, COL_NAME, 21, "Reset to Defaults", CTRL_RESET);
    menu_add(&m, COL_NAME, 23, "Back", CTRL_BACK);
    snprintf(line, sizeof line, "Player %d", player + 1);
    menu_add(&m, COL_NAME, 9, line, -1);
    menu_add(&m, COL_KEY, 9, "Keyboard", -1);
    menu_add(&m, COL_PAD, 9, "Controller", -1);
    c.key_item = m.count;
    for (i = 0; i < 8; i++) menu_add(&m, COL_KEY, CTRL_ROW + i, "", -1);
    c.pad_item = m.count;
    for (i = 0; i < 8; i++) menu_add(&m, COL_PAD, CTRL_ROW + i, "", -1);
    refresh_controls(&m);
    options_screen(&m);
}

/* ---- Options ------------------------------------------------------------------ */

enum { OPT_CONTROLS1, OPT_CONTROLS2, OPT_QUICK, OPT_CLEAN, OPT_BACK, OPT_EXIT };

/* On/Off settings: their names are choosable lines; the values are separate
 * lines in a column further right (the font is proportional). */
typedef struct { int value; const char *name; int *setting; int row; } Toggle;
static const Toggle toggles[] = {
    {OPT_QUICK, "Quick Start", &g_options.quick_start, 15},
    {OPT_CLEAN, "Clean Transitions", &g_options.clean_transitions, 17},
};
#define NTOGGLES ((int)(sizeof toggles / sizeof toggles[0]))
#define COL_VALUE 28

static void apply_options(void)
{
    vdp_set_clean_transitions(g_options.clean_transitions);
}

static int options_action(Menu *m, int dir)
{
    int *value_items = (int *)m->user, i;
    for (i = 0; i < NTOGGLES; i++)
        if (m->item[m->sel].value == toggles[i].value) {
            *toggles[i].setting = !*toggles[i].setting;
            apply_options();
            menu_set_text(m, value_items[i], *toggles[i].setting ? "On" : "Off");
            return 0;
        }
    return dir == 0;            /* the others open or close something */
}

static int confirm_exit(void)
{
    Menu m;
    menu_init(&m);
    m.b_is_back = 1;
    m.hand_col = 12;
    menu_add(&m, 16, 14, "No", 0);
    menu_add(&m, 16, 16, "Yes", 1);
    menu_add(&m, 11, 11, "Exit the game?", -1);
    return options_screen(&m) == 1;
}

void options_menu(void)
{
    int sel = 0;
    for (;;) {
        Menu m;
        int value_items[NTOGGLES], i, v;
        menu_init(&m);
        m.action = options_action;
        m.user = value_items;
        m.b_is_back = 1;
        menu_add(&m, 6, 10, "Controls -- Player 1", OPT_CONTROLS1);
        menu_add(&m, 6, 12, "Controls -- Player 2", OPT_CONTROLS2);
        for (i = 0; i < NTOGGLES; i++) menu_add(&m, 6, toggles[i].row, toggles[i].name, toggles[i].value);
        menu_add(&m, 6, 20, "Back", OPT_BACK);
        menu_add(&m, 6, 24, "Exit Game", OPT_EXIT);   /* out of the way, and it asks first */
        for (i = 0; i < NTOGGLES; i++)
            value_items[i] = menu_add(&m, COL_VALUE, toggles[i].row, *toggles[i].setting ? "On" : "Off", -1);
        m.sel = sel;
        v = options_screen(&m);
        sel = m.sel;
        if (v == OPT_CONTROLS1 || v == OPT_CONTROLS2) {
            controls_menu(v == OPT_CONTROLS1 ? 0 : 1);
            continue;
        }
        if (v == OPT_EXIT) {
            if (confirm_exit()) {
                settings_save();
                genesis_quit();
            }
            continue;
        }
        break;                                        /* Back, or B */
    }
    settings_save();
}

