/*
 * Menu screens: LOC_00023784 (Source/Menus in the disassembly) in C.
 * See menu.h. The order of calls each frame is the original's, so menus run
 * the same way (including one GenerateRandom per frame, which the Random World
 * seed depends on).
 */
#include "game.h"
#include "menu.h"
#include "core/vdp.h"

#include <stdio.h>
#include <string.h>

#define VDP_CTRL            0xC00004u
#define VDP_REG1_SHADOW     0xFF8000u   /* INITIAL_STACK_POINTER: copy of VDP register 1 */
#define VBLANK_FLAG_2       0xFFD930u
#define WATER_PAL1          0x0980C8u
#define TOEJAM_EARL_PAL     0x098128u
#define WINDOW_D000_CMD     0x50000003u /* VDP write to $D000 (the window plane) */

#define SND_CURSOR          0x07
#define SND_PRIORITY        0x32

static void wait_frames(uint32_t n)
{
    push32(n);
    CALL(f_000578_WaitForNVBlanks);
    POP_ARGS(1);
}

static void display(int on)
{
    uint32_t r1 = rd8(VDP_REG1_SHADOW);
    r1 = on ? (r1 | 0x40u) : (r1 & 0x3Cu);
    wr8(VDP_REG1_SHADOW, r1);
    wr16(VDP_CTRL, (r1 | 0x8104u) & 0xFFFFu);
}

static uint32_t pads_now(void)
{
    uint32_t p;
    CALL(f_02706C_ReadPad2Input);
    p = R.d[0];
    CALL(f_027006_ReadPad1Input);
    return (p | R.d[0]) & 0xFFu;
}

void menu_init(Menu *m)
{
    memset(m, 0, sizeof *m);
    m->hand_col = 2;
}

int menu_add(Menu *m, int col, int row, const char *text, int value)
{
    MenuItem *it;
    if (m->count >= MENU_MAX) return -1;
    it = &m->item[m->count];
    it->col = col;
    it->row = row;
    it->value = value;
    snprintf(it->text, sizeof it->text, "%s", text);
    return m->count++;
}

/* Write item i's text to the window plane with the game's text routine
 * (LOC_00009F6A), padded over whatever was there before. */
static void draw_item(Menu *m, int i)
{
    MenuItem *it = &m->item[i];
    uint8_t *dst = genesis_port_rom_ptr(m->text_addr[i]);
    int n = (int)strlen(it->text), k;
    if (!dst) return;
    for (k = 0; k < MENU_TEXT - 1; k++)
        dst[k] = (uint8_t)(k < n ? it->text[k] : (k < m->shown_len[i] ? ' ' : 0));
    dst[MENU_TEXT - 1] = 0;
    if (n > m->shown_len[i]) m->shown_len[i] = n;
    wr32(VDP_CTRL, WINDOW_D000_CMD + ((uint32_t)it->col << 17) + ((uint32_t)it->row << 23));
    push32(m->text_addr[i]);
    CALL(f_009F6A);
    POP_ARGS(1);
}

void menu_set_text(Menu *m, int i, const char *text)
{
    if (i < 0 || i >= m->count) return;
    snprintf(m->item[i].text, sizeof m->item[i].text, "%s", text);
    if (m->running) draw_item(m, i);
}

static void place_hand(Menu *m)
{
    push32((uint32_t)m->item[m->sel].row);
    push32((uint32_t)m->hand_col);
    CALL(f_023B14);
    POP_ARGS(2);
}

/* One frame of the menu screen, as the original's loop does it. */
void menu_frame(void)
{
    wr8(VBLANK_FLAG_2, 0);
    CALL(f_00CC62);
    wait_frames(1);
    push32(0);
    push32(0);
    CALL(f_00CED0_SpriteLoadingStuff);
    push32(8);
    CALL(f_008BD4);
    push32(0);
    CALL(f_00059E);
    CALL(f_004E7E_GenerateRandom);
    POP_ARGS(4);
}

uint8_t g_menu_hand[8 * 32];
int g_menu_hand_ok;

/* Keep a copy of the hand's tiles: the 4x2-tile sprite on palette line 3. */
static void grab_hand(void)
{
    uint32_t sat = (uint32_t)(vdp_reg[5] & 0x7F) << 9;
    int i, link = 0;
    if (g_menu_hand_ok) return;
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

static int selectable(const Menu *m, int i)
{
    return i >= 0 && i < m->count && m->item[i].value >= 0;
}

int menu_run(Menu *m, int title)
{
    int i, result = -1;
    uint32_t prev = 0;
    unsigned t;

    CY(60);
    if (m->sel < 0 || !selectable(m, m->sel)) m->sel = 0;
    push32((uint32_t)title);             /* stays on the stack for f_023BDA, as in the game */
    CALL(f_023B4E_LoadMenu);

    /* strings in ROM space for the game's text routine */
    genesis_port_rom_reset();
    for (i = 0; i < m->count; i++) {
        static const uint8_t blank[MENU_TEXT];
        m->text_addr[i] = genesis_port_rom(blank, MENU_TEXT);
        m->shown_len[i] = 0;
    }

    /* set the screen up with the display off */
    wait_frames(1);
    display(0);
    CALL(f_00046A);
    push32(0);
    CALL(f_01D7C0);
    POP_ARGS(1);
    push32(0);
    push32(WATER_PAL1);
    CALL(f_026FDA_LoadPalette);
    POP_ARGS(2);
    push32(3);
    push32(TOEJAM_EARL_PAL);
    CALL(f_026FDA_LoadPalette);
    POP_ARGS(2);
    push32(1);
    CALL(f_008B62);
    POP_ARGS(1);
    for (i = 0; i < m->count; i++) draw_item(m, i);
    m->running = 1;
    CALL(f_00CC62);
    wait_frames(1);
    push32(0);
    push32(0);
    CALL(f_00CED0_SpriteLoadingStuff);
    POP_ARGS(2);
    display(1);
    CALL(f_008BB2_SetVHBlankFlag2);

    /* wait until A, B, C and Start are let go */
    while (pads_now() & 0xF0u) {
        wait_frames(1);
        push32(8);
        CALL(f_008BD4);
        POP_ARGS(1);
    }
    CALL(f_023A90);                      /* ToeJam's hand */

    for (t = 0;; t++) {
        uint32_t p;
        menu_frame();
        if (t == 2) grab_hand();
        if (m->frame) m->frame(m, t);
        p = pads_now();
        if (p != prev) {
            uint32_t pressed = p & ~prev;
            prev = p;
            if ((pressed & PAD_B) && m->b_is_back && m->item[m->sel].edit && m->action) {
                if (m->action(m, MENU_BACK)) {
                    result = -1;
                    break;
                }
            } else if ((pressed & PAD_B) && m->b_is_back) {
                result = -1;
                break;
            }
            if (pressed & (PAD_A | PAD_B | PAD_C | PAD_START)) {
                if (!m->action || m->action(m, 0)) {
                    result = m->item[m->sel].value;
                    break;
                }
                prev = pads_now();       /* a binding may have been changed: start afresh */
            } else if ((pressed & (PAD_UP | PAD_DOWN)) && m->item[m->sel].edit && m->action) {
                if (m->action(m, (pressed & PAD_UP) ? MENU_UP : MENU_DOWN)) {
                    result = m->item[m->sel].value;
                    break;
                }
            } else if (pressed & PAD_UP) {
                push32(SND_PRIORITY);
                push32(SND_CURSOR);
                CALL(f_03F700_PlaySound);
                POP_ARGS(2);
                if (selectable(m, m->sel - 1)) m->sel--;
            } else if (pressed & PAD_DOWN) {
                push32(SND_PRIORITY);
                push32(SND_CURSOR);
                CALL(f_03F700_PlaySound);
                POP_ARGS(2);
                if (selectable(m, m->sel + 1)) m->sel++;
            } else if ((pressed & (PAD_LEFT | PAD_RIGHT)) && m->action) {
                if (m->action(m, (pressed & PAD_LEFT) ? -1 : 1)) {
                    result = m->item[m->sel].value;
                    break;
                }
            }
        }
        place_hand(m);
    }

    /* take the screen down */
    m->running = 0;
    CALL(f_008BBC_ClearVHBlankFlag2);
    display(0);
    CALL(f_023AF8);                      /* remove the hand */
    push32(0);
    CALL(f_008B62);
    POP_ARGS(1);
    wr16(VDP_CTRL, 0x9100);
    wr16(VDP_CTRL, 0x9200);
    CALL(f_023BDA);                      /* removes the title (LoadMenu's argument) */
    POP_ARGS(1);
    return result;
}
