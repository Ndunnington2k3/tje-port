/*
 * The game's menu screen (LOC_00023784) rewritten in C, so the port's own
 * menus look and behave exactly like the game's (title, ToeJam's hand, font,
 * sounds) with a few additions:
 *   - the cursor can start on any item;
 *   - Left/Right reach the menu's action callback (for settings with values);
 *   - item text can change while the menu is up;
 *   - B can mean "back" instead of "select".
 */
#ifndef TJE_MENU_H
#define TJE_MENU_H

#include <stdint.h>

#define MENU_MAX 40
#define MENU_TEXT 40

/* LoadMenu titles */
#define MENU_TITLE_WHO   0
#define MENU_TITLE_WHAT  1
#define MENU_NO_TITLE   -1     /* the port draws its own (an overlay) */
#define MENU_TITLE_JAM_OUT    2
#define MENU_TITLE_CONTROLLER 3
#define MENU_ESCAPE    -2      /* a usual escape_value */

typedef struct Menu Menu;

typedef struct {
    int col, row;              /* window cell of the text */
    char text[MENU_TEXT];
    int value;                 /* < 0: a line that cannot be chosen (put these last) */
    int edit;                  /* Up/Down go to the action callback while the hand is here */
} MenuItem;

/* `dir` values for the action callback */
#define MENU_SELECT 0          /* A, C or Start (and B unless b_is_back) */
#define MENU_LEFT  -1
#define MENU_RIGHT  1
#define MENU_UP    -2          /* only on items with .edit set */
#define MENU_DOWN   2
#define MENU_BACK   3          /* B on an item with .edit set (when b_is_back) */

struct Menu {
    MenuItem item[MENU_MAX];
    int count;
    int sel;                   /* item the hand starts on; the one chosen on return */
    int hand_col;              /* hand position (window column), 2 by default */
    int b_is_back;             /* B leaves the menu (returns -1) instead of choosing */
    int escape_value;          /* if not 0: Escape leaves the menu and menu_run returns this
                                  (with b_is_back, Escape does what B does) */
    /* Optional. Called for Left/Right (dir -1/+1) and for A, C and Start (dir 0) on
     * item `sel`. Return 1 to close the menu (it then returns the item's value), 0
     * to stay. It may change texts with menu_set_text(). */
    int (*action)(Menu *m, int dir);
    /* Optional. Called once per frame while the menu is up (t counts frames). */
    void (*frame)(Menu *m, unsigned t);
    void *user;
    /* internal */
    uint32_t text_addr[MENU_MAX];
    int shown_len[MENU_MAX];
    int running;
};

void menu_init(Menu *m);
int menu_add(Menu *m, int col, int row, const char *text, int value);   /* returns the index */
/* Change an item's text (redrawn at once if the menu is on screen). */
void menu_set_text(Menu *m, int i, const char *text);

/* Show the menu under one of the game's titles (or MENU_NO_TITLE) until something is chosen.
 * Returns the chosen item's value, or -1 when B backs out (b_is_back). */
int menu_run(Menu *m, int title);

/* Wait for a frame on a menu screen (for use inside an action callback). */
void menu_frame(void);

/* Nonzero while a port menu (or the pause menu) is reading input itself:
 * Escape is "back" there, and Start (pause) during play. */
extern int g_port_menu_active;
void game_input_hook(uint16_t pad[2]);

/* Copy a menu item table from ROM (8 bytes per item: column, row, string, value). */
void menu_add_rom_items(Menu *m, uint32_t table, int count, int row_shift);

/* ToeJam's pointing hand: its 4x2 tiles (palette line 3), copied from VRAM the
 * first time a menu screen shows it; reused by the pause menu. */
extern uint8_t g_menu_hand[8 * 32];
extern int g_menu_hand_ok;

#endif
