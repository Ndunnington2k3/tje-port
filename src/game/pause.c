/*
 * The pause screen: GamePaused (Source/.../GamePaused in EntireSource.X68),
 * rewritten in C with a menu (port feature):
 *
 *     resume      save game      quit to menu
 *
 * The PAUSE sign drops in exactly as in the original. The menu is drawn in the
 * status bar (the window plane at the bottom of the screen) with the HUD's own
 * font, and the bar is put back as it was when the game resumes.
 * Left/Right choose, Start selects, B goes back. Start on "resume" (where the
 * cursor starts) unpauses, so Start, Start still works as it always did, and
 * the original pause-screen button code still works too.
 */
#include "game.h"
#include "save.h"
#include "menu.h"
#include "core/vdp.h"

#include <stdio.h>
#include <string.h>

/* RAM (names from GameConstants.X68) */
#define VHBLANK_FLAG_1        0xFF8010u   /* split screen */
#define VBLANK_FLAG_8         0xFF802Au
#define TOEJAM_CAMERA_NEWX    0xFF8036u
#define TOEJAM_CAMERA_NEWY    0xFF8038u
#define EARL_CAMERA_NEWX      0xFF803Au
#define EARL_CAMERA_NEWY      0xFF803Cu
#define TOEJAMS_CURRENT_LEVEL 0xFF912Cu
#define EARLS_CURRENT_LEVEL   0xFF912Eu
#define VBLANK_FLAG_1         0xFF9364u   /* per player: a present/shop screen is up */
#define TOEJAMS_XPOS          0xFFA25Au   /* entity table, $80 bytes per entity */
#define SHIP_PIECE_LEVELS     0xFFE212u
#define FINAL_LEVEL           0x1A

/* ROM */
#define PAUSE_SIGN_ANIM       0x0AF040u

/* sound effects (PlaySound arguments) */
#define SND_PRIORITY          0x63
#define SND_PAUSE_1           0x06
#define SND_PAUSE_2           0x14
#define SND_MOVE              0x07        /* the menu screens' cursor sound */

/* ---- calling the game ------------------------------------------------------- */

static void play_sound(uint32_t id)
{
    push32(SND_PRIORITY);
    push32(id);
    CALL(f_03F700_PlaySound);
    POP_ARGS(2);
}

/* MovePadInputIntoD0: the pad of player p (0/1) as PAD_* bits */
static uint32_t pad(int p)
{
    push32((uint32_t)p);
    CALL(f_00040A_MovePadInputIntoD0);
    POP_ARGS(1);
    return R.d[0] & 0xFFu;
}

static void wait_start_released(void)
{
    while ((pad(1) | pad(0)) & PAD_START)
        wait_vblank();
}

/* ---- the status bar ----------------------------------------------------------- */

#define BAR_COLS   40
#define BAR_ROWS   4              /* top border, two text rows, bottom border */
#define TILE_BLANK 0x8580u        /* priority, palette 0, HUD font space */

static uint16_t bar_saved[BAR_ROWS][BAR_COLS];
static uint32_t bar_base;         /* VRAM address of the bar's top border row */
static uint32_t bar_stride;
static uint32_t bar_top_row;      /* screen row (8-pixel units) of the top border */

static uint16_t vram16(uint32_t a) { return (uint16_t)((vdp_vram[a & 0xFFFF] << 8) | vdp_vram[(a + 1) & 0xFFFF]); }
static void vram16_set(uint32_t a, uint16_t v) { vdp_vram[a & 0xFFFF] = (uint8_t)(v >> 8); vdp_vram[(a + 1) & 0xFFFF] = (uint8_t)v; }

static uint16_t *bar_cell(int row, int col) { return &bar_saved[row][col]; }

/* Where the status bar is: the window plane, from its first row down. */
static void bar_locate(void)
{
    uint32_t window = (uint32_t)(vdp_reg[3] & 0x3E) << 10;
    uint32_t top = (vdp_reg[0x12] & 0x80) ? (vdp_reg[0x12] & 0x1Fu) : 23u;
    bar_stride = (vdp_reg[0x0C] & 0x81) ? 0x80u : 0x40u;   /* 64 or 32 cells per row */
    if (top > 28 - BAR_ROWS) top = 28 - BAR_ROWS;
    bar_base = window + top * bar_stride;
    bar_top_row = top;
}

static void bar_save(void)
{
    int r, c;
    bar_locate();
    for (r = 0; r < BAR_ROWS; r++)
        for (c = 0; c < BAR_COLS; c++)
            *bar_cell(r, c) = vram16(bar_base + (uint32_t)r * bar_stride + (uint32_t)c * 2);
}

static void bar_restore(void)
{
    int r, c;
    for (r = 0; r < BAR_ROWS; r++)
        for (c = 0; c < BAR_COLS; c++)
            vram16_set(bar_base + (uint32_t)r * bar_stride + (uint32_t)c * 2, *bar_cell(r, c));
}

/* A single box across the bar: the borders continue over the joins between the
 * HUD's boxes (their pattern repeats every few tiles), the inside is blank. */
static void bar_box(void)
{
    int r, c;
    for (r = 0; r < BAR_ROWS; r++)
        for (c = 1; c < BAR_COLS - 1; c++) {
            uint16_t t = TILE_BLANK;
            if (r == 0 || r == BAR_ROWS - 1) t = *bar_cell(r, 1 + (c - 1) % 16);
            vram16_set(bar_base + (uint32_t)r * bar_stride + (uint32_t)c * 2, t);
        }
}

/* HUD font: lowercase letters, digits and a little punctuation. */
static uint16_t glyph(char ch)
{
    if (ch >= 'a' && ch <= 'z') return (uint16_t)(0x8000u | (0x5B5u + (uint32_t)(ch - 'a')));
    if (ch >= 'A' && ch <= 'Z') return (uint16_t)(0x8000u | (0x5B5u + (uint32_t)(ch - 'A')));
    if (ch >= '0' && ch <= '9') return (uint16_t)(0x8000u | (0x5D8u + (uint32_t)(ch - '0')));
    switch (ch) {
    case '?': return 0x85CF;
    case '.': return 0x85D3;
    case ',': return 0x85D4;
    case '\'': return 0x85D5;
    case '-': return 0x85D6;
    case '!': return 0x85D7;
    default: return TILE_BLANK;
    }
}

/* Text on text row 0 or 1 of the box, from column col (1..38). */
static void bar_text(int row, int col, const char *s)
{
    uint32_t a = bar_base + (uint32_t)(row + 1) * bar_stride;
    for (; *s && col < BAR_COLS - 1; s++, col++)
        vram16_set(a + (uint32_t)col * 2, glyph(*s));
}

static void bar_clear_row(int row)
{
    int c;
    uint32_t a = bar_base + (uint32_t)(row + 1) * bar_stride;
    for (c = 1; c < BAR_COLS - 1; c++) vram16_set(a + (uint32_t)c * 2, TILE_BLANK);
}

static void bar_centre(int row, const char *s)
{
    int n = (int)strlen(s);
    bar_clear_row(row);
    bar_text(row, 1 + (BAR_COLS - 2 - n) / 2, s);
}

/* Parts of the HUD are sprites (floor number, lives, health bar, icons). While the
 * menu is up, sprites over the bar are moved off screen in the sprite table each
 * frame (the VBlank handler may rewrite it), and put back afterwards. One of
 * their entries is used for ToeJam's pointing hand from the menus. */
#define SAT_BYTES (80 * 8)
static uint8_t sat_saved[SAT_BYTES];
static uint32_t sat_base;
static int hand_entry;            /* sprite table entry used for the hand, -1: none */
static int hand_on, hand_x, hand_y;

/* The hand's 4x2 tiles go where the HUD's rank text is drawn (tiles $59F-$5A6),
 * which the menu covers anyway. */
#define HAND_TILE   0x59Fu
#define HAND_BYTES  (8 * 32)
static uint8_t hand_vram_saved[HAND_BYTES];
static int have_hand;

static void sprites_save(void)
{
    int i;
    uint32_t bar_top_y = 128u + bar_top_row * 8u;  /* sprite y of the bar */
    sat_base = (uint32_t)(vdp_reg[5] & 0x7F) << 9;
    if (vdp_reg[0x0C] & 0x81) sat_base &= 0xFC00u;     /* 40-cell mode ignores bit 9 */
    memcpy(sat_saved, &vdp_vram[sat_base & 0xFFFF], SAT_BYTES);
    hand_entry = -1;
    for (i = 0; i < 80 && hand_entry < 0; i++) {
        const uint8_t *e = sat_saved + i * 8;
        uint32_t y = (uint32_t)((e[0] << 8) | e[1]) & 0x3FFu;
        if (y >= bar_top_y && y < bar_top_y + BAR_ROWS * 8u) hand_entry = i;
    }
    have_hand = g_menu_hand_ok && hand_entry >= 0;
    hand_on = 0;
    if (have_hand) {
        memcpy(hand_vram_saved, &vdp_vram[HAND_TILE * 32], HAND_BYTES);
        /* the pause menu's version of the hand: no cuff, and the knuckle line in
         * dark grey instead of black (colours 1-7 dropped, 8 -> 9) */
        for (i = 0; i < HAND_BYTES; i++) {
            uint8_t b = g_menu_hand[i], hi = b >> 4, lo = b & 15;
            if (hi < 8) hi = 0; else if (hi == 8) hi = 9;
            if (lo < 8) lo = 0; else if (lo == 8) lo = 9;
            vdp_vram[HAND_TILE * 32 + (uint32_t)i] = (uint8_t)(hi << 4 | lo);
        }
    }
}

static void sprites_update(void)
{
    int i;
    uint32_t bar_top_y = 128u + bar_top_row * 8u;
    for (i = 0; i < 80; i++) {
        const uint8_t *e = sat_saved + i * 8;
        uint32_t a = sat_base + (uint32_t)i * 8;
        uint32_t y = (uint32_t)((e[0] << 8) | e[1]) & 0x3FFu;
        uint32_t h = ((e[2] & 3u) + 1u) * 8u;
        if (y + h > bar_top_y && y < bar_top_y + BAR_ROWS * 8u)
            vram16_set(a, (uint16_t)(vram16(a) & 0xFC00u));     /* y = 0: off the top */
    }
    if (have_hand && hand_on) {
        uint32_t a = sat_base + (uint32_t)hand_entry * 8;
        vram16_set(a, (uint16_t)(128 + hand_y));
        vdp_vram[(a + 2) & 0xFFFF] = 0x0D;                       /* 4 x 2 tiles; keep the link */
        vram16_set(a + 4, (uint16_t)(0x8000u | 0x6000u | HAND_TILE));   /* priority, palette 3 */
        vram16_set(a + 6, (uint16_t)(128 + hand_x));
    }
}

static void sprites_restore(void)
{
    memcpy(&vdp_vram[sat_base & 0xFFFF], sat_saved, SAT_BYTES);
    if (have_hand) memcpy(&vdp_vram[HAND_TILE * 32], hand_vram_saved, HAND_BYTES);
}

/* ---- the menu ---------------------------------------------------------------- */

/* Each choice leaves room on its left for the hand (4 tiles and a space). */
#define HAND_COLS 5

typedef struct {
    const char *label[4];
    int count;
    int row[4];
    int col[4];
} Choice;

/* Centre the choices on one row. */
static void choice_centre(Choice *ch, int row)
{
    int i, total = 0, col;
    for (i = 0; i < ch->count; i++) total += HAND_COLS + (int)strlen(ch->label[i]);
    total += 2 * (ch->count - 1);
    col = 1 + (BAR_COLS - 2 - total) / 2 + HAND_COLS;
    for (i = 0; i < ch->count; i++) {
        ch->col[i] = col;
        ch->row[i] = row;
        col += (int)strlen(ch->label[i]) + 2 + HAND_COLS;
    }
}

static void choice_draw(const Choice *ch, int sel, int blink_off)
{
    int i;
    for (i = 0; i < ch->count; i++) {
        if (i == sel && blink_off && !have_hand) {
            char blank[40];
            size_t n = strlen(ch->label[i]);
            memset(blank, ' ', n);
            blank[n] = 0;
            bar_text(ch->row[i], ch->col[i], blank);
        } else {
            bar_text(ch->row[i], ch->col[i], ch->label[i]);
        }
    }
    /* the hand points at the choice, as on the menu screens */
    hand_on = sel >= 0;
    if (hand_on) {
        hand_x = ch->col[sel] * 8 - HAND_COLS * 8;
        hand_y = (int)(bar_top_row + 1 + (uint32_t)ch->row[sel]) * 8 - 2;
    }
}

/* Pause-screen button code from the original: four button combinations in a row
 * on either pad leave a single ship piece, on the floor above. */
static int cheat_step;

static void cheat(uint32_t p0, uint32_t p1)
{
    static const uint8_t code[4] = {0x71, 0x48, 0x12, 0x24};
    int i, first, best;
    if (cheat_step > 3) return;
    if (p0 != code[cheat_step] && p1 != code[cheat_step]) return;
    if (cheat_step == 3) {
        for (first = 0; first < 10 && !rd8(SHIP_PIECE_LEVELS + (uint32_t)first); first++) {}
        best = first;
        for (i = first + 1; i < 10; i++)
            if (rd8(SHIP_PIECE_LEVELS + (uint32_t)i) &&
                (int8_t)rd8(SHIP_PIECE_LEVELS + (uint32_t)best) > (int8_t)rd8(SHIP_PIECE_LEVELS + (uint32_t)i))
                best = i;
        for (i = 0; i < 10; i++)
            if (i != best) wr8(SHIP_PIECE_LEVELS + (uint32_t)i, 0);
        if (rd8(TOEJAMS_CURRENT_LEVEL) != rd8(SHIP_PIECE_LEVELS + (uint32_t)best))
            wr8(SHIP_PIECE_LEVELS + (uint32_t)best, rd8(TOEJAMS_CURRENT_LEVEL) + 1);
        play_sound(SND_PAUSE_2);
    }
    cheat_step++;
}

/* Input for the menu: buttons newly pressed on the pausing player's pad (Start on
 * either), with the original code checked along the way. */
static uint32_t prev_buttons;
static int menu_player;

static uint32_t menu_input(void)
{
    uint32_t p0, p1, now, pressed;
    wait_vblank();
    sprites_update();
    p0 = pad(0);
    p1 = pad(1);
    cheat(p0, p1);
    now = (menu_player ? p1 : p0) | ((p0 | p1) & PAD_START);
    pressed = now & ~prev_buttons;
    prev_buttons = now;
    return pressed;
}

/* Run the choices until one is picked; returns its index, or -1 for B.
 * Left/Up and Right/Down move through them in order. */
static int choose(Choice *ch, int sel)
{
    unsigned t = 0;
    for (;;) {
        uint32_t in;
        choice_draw(ch, sel, (t++ % 30) >= 20);
        in = menu_input();
        if (in & (PAD_LEFT | PAD_UP)) { sel = (sel + ch->count - 1) % ch->count; t = 0; play_sound(SND_MOVE); }
        if (in & (PAD_RIGHT | PAD_DOWN)) { sel = (sel + 1) % ch->count; t = 0; play_sound(SND_MOVE); }
        if (in & PAD_START) { choice_draw(ch, sel, 0); hand_on = 0; return sel; }
        if (in & PAD_B) { choice_draw(ch, sel, 0); hand_on = 0; return -1; }
    }
}

/* Show a message in the middle of the bar for a moment. */
static void flash_message(const char *msg)
{
    int i;
    bar_box();
    bar_centre(0, msg);
    hand_on = 0;
    for (i = 0; i < 90; i++) {
        if (menu_input() & (PAD_START | PAD_B)) break;
    }
}

/* A question centred on the first row, its answers centred on the second. */
static int ask(const char *question, Choice *answers, int sel)
{
    bar_box();
    bar_centre(0, question);
    choice_centre(answers, 1);
    return choose(answers, sel);
}

static int yes_no(const char *question)
{
    Choice ch = {{"yes", "no"}, 2, {0}, {0}};
    return ask(question, &ch, 1) == 0;
}

/* Pick a file to save to; B backs out. */
static void save_menu(void)
{
    int sel = 0;
    for (;;) {
        Choice ch = {{"file 1", "file 2", "file 3"}, SAVE_SLOTS, {0}, {0}};
        SaveInfo info;
        sel = ask("which file?", &ch, sel);
        if (sel < 0) return;
        if (save_info(sel, &info) && !yes_no("would you like to overwrite?"))
            continue;
        if (save_write(sel) == 0) {
            play_sound(SND_PAUSE_2);
            flash_message("game saved!");
        } else {
            flash_message("couldn't save the game!");
        }
        return;
    }
}

/* The menu; returns when the game should resume (quitting does not return).
 *      resume        save game
 *      quit to menu                  */
static void pause_menu(void)
{
    int sel = 0;
    bar_save();
    sprites_save();
    sprites_update();
    for (;;) {
        Choice ch = {{"resume", "save game", "quit to menu"}, 3, {0, 0, 1}, {11, 25, 11}};
        bar_box();
        sel = choose(&ch, sel);
        if (sel <= 0) break;                       /* resume (or B) */
        if (sel == 1) {
            if (save_possible()) save_menu();
            else flash_message("you can't save here!");
        } else if (sel == 2) {
            if (yes_no("quit to the menu?")) {
                save_quit_to_menu();
            }
        }
    }
    bar_restore();
    sprites_restore();
}

/* ---- GamePaused --------------------------------------------------------------- */

TJE_OVERRIDE(0x01DBD8)
void f_01DBD8_GamePaused(void)
{
    int player = (int16_t)(ARG_L(0) & 0xFFFF);   /* who pressed Start */
    uint32_t sign, id;
    int view = 0;
    uint32_t landed = 2;

    CY(80);
    if (rd8(TOEJAMS_CURRENT_LEVEL) == FINAL_LEVEL) {
        /* the last floor has its own pause (LOC_0001DAEE) */
        push32((uint32_t)player);
        CALL(f_01DAEE);
        POP_ARGS(1);
        RETURN();
    }
    if (rd8(VBLANK_FLAG_1 + (uint32_t)player))
        RETURN();

    /* the PAUSE sign: a free entity slot, placed over the player's view */
    push32(0);
    CALL(f_01E6F4);
    POP_ARGS(1);
    id = R.d[0] & 0xFFFFu;
    if (!id)
        RETURN();
    sign = TOEJAMS_XPOS + ((id << 7) & 0xFFFFu);
    wr16(sign, rd16(TOEJAM_CAMERA_NEWX) + 0xA0);
    wr16(sign + 2, rd16(TOEJAM_CAMERA_NEWY) + 0xE1);
    wr8(sign + 0x4C, rd8(TOEJAMS_CURRENT_LEVEL));
    if (rd8(VHBLANK_FLAG_1)) {
        wr16(sign + 4, 0xA8);
        if (player) {
            /* split screen, Earl paused: drop it on Earl's half */
            wr8(sign + 0x4C, rd8(EARLS_CURRENT_LEVEL));
            wr16(sign, rd16(EARL_CAMERA_NEWX) + 0xA0);
            wr16(sign + 2, rd16(EARL_CAMERA_NEWY) + 0xE1);
            view = 1;
            landed = 4;
        }
    } else {
        wr16(sign + 4, 0x70);
    }
    wr8(sign + 0x2E, 4);
    wr8(sign + 0x2B, 0x30);
    wr16(sign + 0x2C, rd16(sign + 0x2C) | 0x8000u);
    wr32(sign + 0x1A, rd32(PAUSE_SIGN_ANIM));
    wr32(sign + 0x1E, rd32(PAUSE_SIGN_ANIM + 4));
    wr8(sign + 0x28, 1);
    wr8(sign + 0x26, 0xFF);

    /* let it fall until it lands */
    for (;;) {
        push32((uint32_t)view);
        push32(sign);
        CALL(f_00D756);
        POP_ARGS(2);
        if (rd8(sign + 0x2F) & landed) break;
        CALL(f_01E90C);
    }
    do {
        CALL(f_00CC62);
    } while (!rd8(sign + 0x4A));

    play_sound(SND_PAUSE_1);
    push32(1);
    CALL(f_000578_WaitForNVBlanks);
    POP_ARGS(1);
    play_sound(SND_PAUSE_2);
    push32(0);
    CALL(f_03E488);                /* hold the music */
    CALL(f_03E3CC);
    CALL(f_03E39E);
    POP_ARGS(1);

    wait_start_released();

    cheat_step = 0;
    menu_player = player;
    prev_buttons = PAD_START;
    pause_menu();

    /* take the sign away and carry on */
    wr8(VBLANK_FLAG_8, 0xFF);
    push32(id);
    CALL(f_00D950);
    POP_ARGS(1);
    /* Start, or the button used to back out, must not reach the game */
    while ((pad(1) | pad(0)) & (PAD_START | PAD_A | PAD_B | PAD_C))
        wait_vblank();
    RETURN();
}
