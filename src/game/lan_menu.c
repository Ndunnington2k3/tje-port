/*
 * Network Play on the Who screen (the "LAN menu"): Host Game / Join Game.
 *
 * Host: name the room, wait in it for a player, then start a new game or load a
 * saved two-player game; the game itself starts both machines (net.c).
 * Join: rooms found on the local network; choose one, wait for the host to start.
 */
#include "game.h"
#include "menu.h"
#include "net.h"
#include "lan_menu.h"
#include "platform/platform.h"

#include <stdio.h>
#include <string.h>

#define SND_PRIORITY    0x32
#define SND_BUZZ        0x1A

/* a heading, centred */
static int heading(Menu *m, int row, const char *text)
{
    int n = (int)strlen(text);
    return menu_add(m, 20 - n / 2, row, text, -1);
}

static void buzz(void)
{
    push32(SND_PRIORITY);
    push32(SND_BUZZ);
    CALL(f_03F700_PlaySound);
    POP_ARGS(2);
}

/* A message and OK (also used by the Who screen for how a session ended). */
void lan_message(const char *text)
{
    Menu m;
    char line[MENU_TEXT];
    const char *p = text;
    int row = 10;
    menu_init(&m);
    m.b_is_back = 1;
    m.hand_col = 14;
    menu_add(&m, 18, 18, "OK", 0);
    /* broken into lines of up to 30 characters */
    while (*p && row < 16) {
        int n = (int)strlen(p), take = n > 30 ? 30 : n;
        if (take < n) while (take > 0 && p[take] != ' ') take--;
        if (take <= 0) take = n > 30 ? 30 : n;
        memcpy(line, p, (size_t)take);
        line[take] = 0;
        menu_add(&m, 20 - take / 2, row, line, -1);
        row += 2;
        p += take;
        while (*p == ' ') p++;
    }
    menu_run(&m, MENU_NO_TITLE);
}

/* ---- naming the room ------------------------------------------------------------ */

static const char name_chars[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-!.,'";

typedef struct {
    char name[NET_NAME_MAX + 1];
    int cursor;
    int name_item, cursor_item;
    int typed_space;
} NameCtx;

#define NAME_COL 10
#define NAME_ROW 12

static void show_name(Menu *m)
{
    NameCtx *c = (NameCtx *)m->user;
    char marks[NET_NAME_MAX + 2];
    int i;
    menu_set_text(m, c->name_item, c->name[0] ? c->name : " ");
    for (i = 0; i < c->cursor && i < NET_NAME_MAX; i++) marks[i] = ' ';
    marks[i++] = '-';
    marks[i] = 0;
    menu_set_text(m, c->cursor_item, marks);
}

static int valid_char(int ch) { return ch && strchr(name_chars, ch) != NULL; }

static void insert_char(NameCtx *c, int ch)
{
    int n = (int)strlen(c->name);
    if (n >= NET_NAME_MAX || !valid_char(ch)) return;
    memmove(c->name + c->cursor + 1, c->name + c->cursor, (size_t)(n - c->cursor + 1));
    c->name[c->cursor++] = (char)ch;
}

static void delete_char(NameCtx *c)
{
    int n = (int)strlen(c->name);
    if (c->cursor == 0) return;
    memmove(c->name + c->cursor - 1, c->name + c->cursor, (size_t)(n - c->cursor + 1));
    c->cursor--;
}

static void name_frame(Menu *m, unsigned t)
{
    NameCtx *c = (NameCtx *)m->user;
    char typed[32];
    int n = plat_text_input(typed, sizeof typed), i;
    (void)t;
    c->typed_space = 0;
    if (m->sel != 0 || n <= 0) return;
    for (i = 0; i < n; i++) {
        if (typed[i] == ' ') c->typed_space = 1;
        insert_char(c, typed[i]);
    }
    show_name(m);
}

static int name_action(Menu *m, int dir)
{
    NameCtx *c = (NameCtx *)m->user;
    int n = (int)strlen(c->name);
    if (m->sel != 0) return dir == MENU_SELECT;
    switch (dir) {
    case MENU_LEFT:
        if (c->cursor > 0) c->cursor--;
        break;
    case MENU_RIGHT:
        if (c->cursor < n && c->cursor < NET_NAME_MAX - 1) c->cursor++;
        else if (c->cursor == n && n < NET_NAME_MAX) { insert_char(c, ' '); }
        break;
    case MENU_UP:
    case MENU_DOWN: {
        /* change the letter under the cursor (a controller has no keyboard) */
        const char *p;
        int k, len = (int)sizeof name_chars - 1;
        if (c->cursor == n) { if (n >= NET_NAME_MAX) break; insert_char(c, 'A'); c->cursor--; break; }
        p = strchr(name_chars, c->name[c->cursor]);
        k = p ? (int)(p - name_chars) : 0;
        k = (k + (dir == MENU_UP ? len - 1 : 1)) % len;
        c->name[c->cursor] = name_chars[k];
        break;
    }
    case MENU_BACK_DIR:
        if (n == 0) return 1;           /* nothing left: back out */
        delete_char(c);
        break;
    case MENU_SELECT:
        if (c->typed_space) break;      /* (Space typed: a space, not "choose") */
        menu_point(m, 1);               /* on to "Open Room" */
        return 0;
    }
    show_name(m);
    return 0;
}

/* Returns 1 with the name chosen, 0 to go back. */
static int name_room(char *name)
{
    Menu m;
    NameCtx c;
    int v;
    memset(&c, 0, sizeof c);
    plat_net_name(c.name, sizeof c.name);
    {
        /* only the characters the game's font has */
        int i, k = 0;
        for (i = 0; c.name[i]; i++) if (valid_char(c.name[i])) c.name[k++] = c.name[i];
        c.name[k] = 0;
        if (!k) snprintf(c.name, sizeof c.name, "ToeJam");
    }
    c.cursor = (int)strlen(c.name);
    menu_init(&m);
    m.b_is_back = 1;
    m.user = &c;
    m.action = name_action;
    m.frame = name_frame;
    c.name_item = menu_add(&m, NAME_COL, NAME_ROW, c.name, 0);
    m.item[c.name_item].edit = 1;
    menu_add(&m, 6, 17, "Open Room", 1);
    menu_add(&m, 6, 19, "Back", 2);
    heading(&m, 7, "Host Game");
    menu_add(&m, 6, 10, "Room Name", -1);
    c.cursor_item = menu_add(&m, NAME_COL, NAME_ROW + 1, "-", -1);
    show_name(&m);
    m.sel = 1;
    for (;;) {
        v = menu_run(&m, MENU_NO_TITLE);
        if (v != 1) return 0;
        {
            /* (no spaces at the ends) */
            int n = (int)strlen(c.name), s = 0;
            while (n > 0 && c.name[n - 1] == ' ') c.name[--n] = 0;
            while (c.name[s] == ' ') s++;
            memmove(c.name, c.name + s, strlen(c.name + s) + 1);
        }
        if (c.name[0]) break;
        buzz();
        c.cursor = 0;
        show_name(&m);
    }
    snprintf(name, NET_NAME_MAX + 1, "%s", c.name);
    return 1;
}

/* ---- the host's room ------------------------------------------------------------ */

enum { ROOM_NEW = 1, ROOM_LOAD, ROOM_CLOSE };

static void room_frame(Menu *m, unsigned t)
{
    static int shown = -1;
    int joined = net_role() == NET_HOST && net_phase() == NET_H_JOINED;
    (void)t;
    if (net_role() != NET_HOST) { m->close = 1; m->close_value = ROOM_CLOSE; return; }
    if (joined != shown || t == 0) {
        shown = joined;
        menu_set_text(m, *(int *)m->user, joined ? "Earl is here!" : "Waiting for a player to join...");
    }
}

/* Returns 0 to start (a new game, or a saved game was loaded: does not return),
 * -1 when the room is closed. */
static int host_room(void)
{
    for (;;) {
        Menu m;
        int status, v;
        char title[MENU_TEXT];
        menu_init(&m);
        m.b_is_back = 1;
        m.frame = room_frame;
        m.user = &status;
        menu_add(&m, 6, 14, "Start New Game", ROOM_NEW);
        menu_add(&m, 6, 16, "Load Game", ROOM_LOAD);
        menu_add(&m, 6, 19, "Close Room", ROOM_CLOSE);
        snprintf(title, sizeof title, "Room: %s", net_room_name());
        heading(&m, 7, title);
        status = menu_add(&m, 6, 10, " ", -1);
        v = menu_run(&m, MENU_NO_TITLE);
        if (v == ROOM_NEW || v == ROOM_LOAD) {
            if (net_phase() != NET_H_JOINED) { buzz(); continue; }
            if (v == ROOM_NEW) return 0;
            load_game_menu(1);             /* (a game loaded does not come back here) */
            continue;
        }
        net_leave(NULL);
        return -1;
    }
}

/* ---- joining -------------------------------------------------------------------- */

#define LIST_ROWS 5
typedef struct {
    NetRoom rooms[LIST_ROWS];
    int n, items[LIST_ROWS], note;
} ListCtx;

static void list_frame(Menu *m, unsigned t)
{
    ListCtx *c = (ListCtx *)m->user;
    int i;
    c->n = net_scan(c->rooms, LIST_ROWS);
    if (t % 15) return;
    menu_set_text(m, c->note, c->n ? " " : "Looking for rooms...");   /* (first: same row as the first room) */
    for (i = 0; i < LIST_ROWS; i++) {
        char line[MENU_TEXT];
        if (i < c->n) {
            const NetRoom *r = &c->rooms[i];
            snprintf(line, sizeof line, "%s%s", r->name,
                     !r->compatible ? " - other version" : r->state == NET_ROOM_REJOIN ? " - join the game" : "");
        } else {
            snprintf(line, sizeof line, " ");
        }
        menu_set_text(m, c->items[i], line);
    }
}

typedef struct { int status; } WaitCtx;

static void wait_frame(Menu *m, unsigned t)
{
    WaitCtx *c = (WaitCtx *)m->user;
    char line[MENU_TEXT];
    (void)t;
    if (net_role() != NET_JOIN) { m->close = 1; m->close_value = -1; return; }
    switch (net_phase()) {
    case NET_J_CONNECTING: snprintf(line, sizeof line, "Joining..."); break;
    case NET_J_WAITING: snprintf(line, sizeof line, "Waiting for ToeJam to start..."); break;
    default: snprintf(line, sizeof line, "Getting the game... %d%%", net_progress()); break;
    }
    menu_set_text(m, c->status, line);
}

/* Wait in the room; the game starts by restarting the machine (does not return). */
static void join_wait(void)
{
    Menu m;
    WaitCtx c;
    char title[MENU_TEXT];
    menu_init(&m);
    m.b_is_back = 1;
    m.user = &c;
    m.frame = wait_frame;
    menu_add(&m, 6, 19, "Leave", 0);
    snprintf(title, sizeof title, "Room: %s", net_room_name());
    heading(&m, 7, title);
    c.status = menu_add(&m, 6, 12, "Joining...", -1);
    menu_run(&m, MENU_NO_TITLE);
    if (net_role() == NET_JOIN) net_leave(NULL);
}

static void join_list(void)
{
    char msg[96];
    for (;;) {
        Menu m;
        ListCtx c;
        int i, v;
        memset(&c, 0, sizeof c);
        menu_init(&m);
        m.b_is_back = 1;
        m.user = &c;
        m.frame = list_frame;
        for (i = 0; i < LIST_ROWS; i++) c.items[i] = menu_add(&m, 6, 10 + i * 2, " ", i);
        menu_add(&m, 6, 21, "Back", LIST_ROWS);
        heading(&m, 7, "Join Game");
        c.note = menu_add(&m, 6, 10, "Looking for rooms...", -1);
        v = menu_run(&m, MENU_NO_TITLE);
        if (v < 0 || v >= LIST_ROWS) return;
        if (v >= c.n) { buzz(); continue; }
        if (net_join(&c.rooms[v]) == 0) join_wait();
        if (net_take_message(msg, sizeof msg)) lan_message(msg);
    }
}

/* ---- the LAN menu --------------------------------------------------------------- */

int lan_menu(void)
{
    int sel = 0;
    char msg[96];
    /* back from the What screen while hosting: still in the room */
    if (net_role() == NET_HOST && (net_phase() == NET_H_WAITING || net_phase() == NET_H_JOINED)) {
        if (host_room() == 0) return 0;
    }
    for (;;) {
        Menu m;
        int v;
        menu_init(&m);
        m.b_is_back = 1;
        menu_add(&m, 6, 13, "Host Game", 1);
        menu_add(&m, 6, 15, "Join Game", 2);
        menu_add(&m, 6, 19, "Back", 3);
        heading(&m, 7, "Network Play");
        m.sel = sel;
        v = menu_run(&m, MENU_NO_TITLE);
        sel = m.sel;
        if (v == 1) {
            char name[NET_NAME_MAX + 1];
            if (!name_room(name)) continue;
            if (net_host(name) != 0) {
                if (net_take_message(msg, sizeof msg)) lan_message(msg);
                continue;
            }
            if (host_room() == 0) return 0;
        } else if (v == 2) {
            join_list();
        } else {
            return -1;
        }
    }
}
