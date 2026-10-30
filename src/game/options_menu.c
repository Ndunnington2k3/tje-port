/*
 * The Options screens (from the Who screen): Controls, port settings and
 * Exit Game. The controls screens replace the game's controller options,
 * which were on the What screen.
 */
#include "game.h"
#include "menu.h"
#include "settings.h"
#include "options_menu.h"
#include "platform/platform.h"
#include "core/vdp.h"

#include <stdio.h>
#include <string.h>


/* All Options screens use the game's own "OPTIONS" title, the one from its
 * controller options screen (LoadMenu 3). */
static int options_screen(Menu *m)
{
    return menu_run(m, MENU_TITLE_CONTROLLER);
}

/* ---- Controls ---------------------------------------------------------------- */

typedef struct {
    int player;
    int key_item, pad_item;        /* menu index of the first keyboard / controller label */
} ControlsCtx;

/* The game's actions; the numbers are the binding numbers (platform.h). */
static const struct { const char *name; int bit; } pad_buttons[8] = {
    {"Up", 0}, {"Down", 1}, {"Left", 2}, {"Right", 3},
    {"Action", 6}, {"Present List", 4}, {"Map", 5}, {"Pause", 7},    /* the game's own names */
};

#define CTRL_RESET   8
#define CTRL_BACK    9
#define CTRL_ROW     11             /* first button line */
#define COL_NAME     6
#define COL_KEY      19
#define COL_PAD      29

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

/* Wait until nothing is held, so the input just bound cannot act on the menu
 * (bound to A, it would choose the line under the hand straight away). */
static void wait_all_released(void)
{
    int frames;
    for (frames = 0; frames < 60 * 5 && (((g_pad[0] | g_pad[1]) & 0x1FFu) || g_menu_pad); frames++)
        menu_frame();
}

/* Ask for one button: returns 1 when bound, 0 when skipped (8 seconds pass),
 * -1 when cancelled (Escape). */
static int capture_one(Menu *m, int i)
{
    ControlsCtx *c = (ControlsCtx *)m->user;
    int bit = pad_buttons[i].bit, slot = 0, frames, r = 0, b;
    char token[32], prompt[40];

    menu_point(m, i);
    snprintf(prompt, sizeof prompt, "Press %s...", pad_buttons[i].name);
    menu_set_text(m, c->key_item + i, prompt);
    menu_set_text(m, c->pad_item + i, "");
    g_binding_active = 1;          /* the menu ignores its controls until this is done */
    plat_capture_start(c->player);
    for (frames = 0; frames < 60 * 8 && r == 0; frames++) {
        menu_point(m, i);
        menu_frame();
        r = plat_capture_poll(&slot, token, sizeof token);
    }
    if (r == 0) plat_capture_cancel();
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
    wait_all_released();          /* after binding, so the new binding is what is checked */
    g_binding_active = 0;
    return r;
}

/* Go down the list from button i to Start, one press each (Escape stops). */
static void capture_from(Menu *m, int i)
{
    for (; i < 8; i++)
        if (capture_one(m, i) < 0) break;
    menu_point(m, i < 8 ? i : 7);
}

static int controls_action(Menu *m, int dir)
{
    int v = m->item[m->sel].value;
    if (dir != 0) return 0;
    if (v < 8) { capture_from(m, v); return 0; }
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
    menu_run(&m, MENU_TITLE_CONTROLLER);
}

/* "Controls" in Options (replacing the game's two fixed layouts): pick a
 * player, then set that player's controls. */
void controls_select_menu(void)
{
    int sel = 0;
    for (;;) {
        Menu m;
        int v;
        menu_init(&m);
        m.b_is_back = 1;
        menu_add(&m, 6, 12, "Player 1 Controls", 0);
        menu_add(&m, 6, 14, "Player 2 Controls", 1);
        menu_add(&m, 6, 18, "Back", 2);
        m.sel = sel;
        v = menu_run(&m, MENU_TITLE_CONTROLLER);
        sel = m.sel;
        if (v != 0 && v != 1) break;
        controls_menu(v);
    }
    settings_save();
}

/* ---- Options ------------------------------------------------------------------ */

enum { OPT_QUICK, OPT_CLEAN, OPT_BACK, OPT_EXIT, OPT_CONTROLS };

/* On/Off settings: their names are choosable lines; the values are separate
 * lines in a column further right (the font is proportional). */
typedef struct { int value; const char *name; int *setting; int row; } Toggle;
static const Toggle toggles[] = {
    {OPT_QUICK, "Quick Start", &g_options.quick_start, 14},
    {OPT_CLEAN, "Clean Transitions", &g_options.clean_transitions, 16},
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
        menu_add(&m, 6, 12, "Controls", OPT_CONTROLS);
        for (i = 0; i < NTOGGLES; i++) menu_add(&m, 6, toggles[i].row, toggles[i].name, toggles[i].value);
        menu_add(&m, 6, 19, "Back", OPT_BACK);
        menu_add(&m, 6, 23, "Exit Game", OPT_EXIT);   /* out of the way, and it asks first */
        for (i = 0; i < NTOGGLES; i++)
            value_items[i] = menu_add(&m, COL_VALUE, toggles[i].row, *toggles[i].setting ? "On" : "Off", -1);
        m.sel = sel;
        v = options_screen(&m);
        sel = m.sel;
        if (v == OPT_CONTROLS) {
            controls_select_menu();
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

