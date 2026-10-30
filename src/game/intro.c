/*
 * Boot sequence: SEGA logo, title screen, intro.
 *
 * Hand-written replacements for SEGAAndIntroVideoRoutine and SEGAVideo
 * (Source/Cutscenes/Code/SEGALogo.X68 and TitleScreen.X68 in the disassembly).
 * They do exactly what the originals do, plus one port feature:
 *
 *   After "Quit to menu" (g_boot_to_menu) the SEGA screen is skipped altogether.
 *
 *   options.quick_start: pressing Start during the SEGA screen ends it cleanly and
 *   skips the title screen and intro, going straight to the Who menu. The original
 *   only reacts to Start once the title screen is up.
 */
#include "game.h"
#include "save.h"
#include "core/vdp.h"

extern int g_gameview_extend_screen;   /* gameview.c: the star background may be drawn wider */

/* RAM used by the SEGA screen (names from GameConstants.X68 where it has them) */
#define VBLANK_FLAG_6       0xFF801Au   /* byte, set while the boot videos run */
#define VDP_REG1_SHADOW     0xFF8000u   /* byte copy of VDP register 1 ("INITIAL_STACK_POINTER") */
#define SEGA_PAL_TO_LOAD    0xFFE406u   /* long, palette the VBlank handler loads */
#define SEGA_PAL_LOAD_FLAG  0xFFE40Au   /* word */
#define SEGA_ANIM_ACTIVE    0xFFE402u   /* word, LOC_00FFE402 */
#define SEGA_SHIP_SHOWN     0xFFE404u   /* word, LOC_00FFE404: ship fly-by already played once */

/* ROM data */
#define SEGA_VBLANK_HOOK    0x03A3D0u   /* LOC_0003A3D0 */
#define PAL_BLACK           0x0982A8u   /* LOC_000982A8 */
#define SEGA_LOGO_TILES     0x09287Cu   /* SegaLogo */
#define SEGA_LOGO_MAP       0x09281Cu   /* SEGALogoTileMap */
#define SEGA_LOGO_PAL       0x092E9Cu   /* SegaLogo_Pal: 20 palettes of 32 bytes, cycled */
#define SHIP_PAL5           0x098348u
#define SHIP_SPEECH         0x055A2Au
#define WATER_PAL1          0x0980C8u
#define WATER_PAL2          0x0980E8u
#define WATER_PAL3          0x098108u
#define TOEJAM_EARL_PAL     0x098128u

static int sega_skipped;   /* set by the SEGA screen when Start ended it early */

static void load_palette(uint32_t pal, uint32_t line)
{
    push32(line);
    push32(pal);
    CALL(f_026FDA_LoadPalette);
    POP_ARGS(2);
}

static void display_on(void)
{
    /* ORI.b #$40, shadow ; VDP reg 1 = $81xx | shadow | 4 */
    uint32_t r1 = (rd8(VDP_REG1_SHADOW) | 0x40u) & 0xFFu;
    wr8(VDP_REG1_SHADOW, r1);
    wr16(0xC00004u, (r1 | 0x8104u) & 0xFFFFu);
}

/* Advance the SEGA logo's colour cycle: every 3 frames, next of 20 palettes. */
static void cycle_logo_palette(uint32_t *last, uint32_t *step)
{
    uint32_t now = rd16(VBLANK_COUNTER);
    if (now < *last + 3)
        return;
    *last = now;
    wr32(SEGA_PAL_TO_LOAD, rd32(SEGA_PAL_TO_LOAD) + 0x20);
    if (++*step >= 0x14) {
        wr32(SEGA_PAL_TO_LOAD, SEGA_LOGO_PAL);
        *step = 0;
    }
    wr16(SEGA_PAL_LOAD_FLAG, 1);
}

TJE_OVERRIDE(0x03A476)
void f_03A476_SEGAVideo(void)
{
    uint32_t start, last, step;
    int16_t x, y;
    int skip = 0;

    CY(60);
    sega_skipped = 0;
    wr16(SEGA_PAL_LOAD_FLAG, 0);
    wr16(SEGA_ANIM_ACTIVE, 0);
    wr32(VBLANK_OVERRIDE, SEGA_VBLANK_HOOK);
    CALL(f_03A41C);
    push32(0);
    push32(PAL_BLACK);
    CALL(f_026FDA_LoadPalette);
    CALL(f_00046A);
    POP_ARGS(2);

    if (g_boot_to_menu) {
        /* "Quit to menu" restarted the machine: no logo, straight to the menus */
        g_boot_to_menu = 0;
        wait_vblank();
        wr32(VBLANK_OVERRIDE, 0);
        sega_skipped = 1;
        RETURN();
    }

    /* logo graphics and tile map */
    wr32(0xC00004u, 0x40000000u);
    push32(0x310);
    push32(SEGA_LOGO_TILES);
    CALL(f_026F76_MoveDataToVRAM);
    push32(0x40);
    push32(0x04);
    push32(0x0C);
    push32(SEGA_LOGO_MAP);
    push32(0xA51C);
    CALL(f_026F8C);
    POP_ARGS(7);

    wr32(SEGA_PAL_TO_LOAD, SEGA_LOGO_PAL);
    wr16(SEGA_PAL_LOAD_FLAG, 1);
    wr16(SEGA_ANIM_ACTIVE, 1);
    wait_vblank();
    display_on();
    g_gameview_extend_screen = 1;     /* (the stars repeat: wider windows show more of them) */

    /* Phase 1: the logo cycles colours for 120 frames (a busy loop in the original). */
    start = last = rd16(VBLANK_COUNTER);
    step = 0;
    while (rd16(VBLANK_COUNTER) < start + 0x78) {
        CY(48);
        RT_POLL();
        cycle_logo_palette(&last, &step);
        if (g_options.quick_start && start_held()) { skip = 1; break; }
    }

    /* Clean transitions: the end of the screen changes the palette a frame before
     * the screen goes; keep the last picture until the next screen is set up. */
    if (skip || rd16(SEGA_SHIP_SHOWN)) vdp_freeze(-1);

    if (skip) {
        /* stop the colour cycling, as the end of phase 1 does */
        wait_vblank();
        wr16(SEGA_ANIM_ACTIVE, 0);
        wr16(SEGA_PAL_LOAD_FLAG, 0);
    } else if (!rd16(SEGA_SHIP_SHOWN)) {
        /* Phase 2 (first boot only): the ship flies past the logo. */
        wait_vblank();
        wr16(SEGA_ANIM_ACTIVE, 0);
        wr16(SEGA_PAL_LOAD_FLAG, 0);
        CALL(f_03A69E);            /* create the ship sprite */
        CALL(f_028B12);
        load_palette(SHIP_PAL5, 2);
        x = 0x118;
        y = 0x96;
        push32((uint32_t)(int32_t)y);
        push32((uint32_t)(int32_t)x);
        push32(0);
        CALL(f_028C1C);
        POP_ARGS(3);
        wr16(SEGA_ANIM_ACTIVE, 1);
        last = rd16(VBLANK_COUNTER);
        while (x > -0x32) {
            CALL(f_00CC62);
            wait_vblank();
            if (x == 0x101) {
                /* the ship's voice clip */
                push32(0);
                push32(0x32);
                push32(SHIP_SPEECH);
                CALL(f_03F56C);
                POP_ARGS(3);
            }
            if (x < 0x39 && x > 0x2C) {
                y += 2;
            } else if (x > 0xB2 && x < 0xBF) {
                y -= 2;
                if (x == 0xBE) {
                    push32(0x63);
                    push32(0x15);
                    CALL(f_03F700_PlaySound);
                    POP_ARGS(2);
                }
            }
            x--;
            push32((uint32_t)(int32_t)y);
            push32((uint32_t)(int32_t)x);
            CALL(f_028DC8);        /* move the ship */
            POP_ARGS(2);
            cycle_logo_palette(&last, &step);
            CY(120);
            if (g_options.quick_start && start_held()) { skip = 1; break; }
        }
        vdp_freeze(-1);            /* (clean transitions, as above) */
        CALL(f_028BDE);            /* remove the ship */
        CALL(f_03A714);
        wr16(SEGA_SHIP_SHOWN, 1);
    }

    wait_vblank();
    wr32(VBLANK_OVERRIDE, 0);
    sega_skipped = skip;
    g_gameview_extend_screen = 0;
    vdp_freeze(2);                 /* the next screen turns the display off within these */
    RETURN();
}

TJE_OVERRIDE(0x027AD6)
void f_027AD6_SEGAAndIntroVideoRoutine(void)
{
    int show_intro = 1;   /* D2 in the original */

    CY(30);
    wr8(VBLANK_FLAG_6, 1);
    for (;;) {
        CALL(f_03A476_SEGAVideo);
        if (sega_skipped)
            goto load_palettes;            /* port feature: straight to the menus */
        CALL(f_027998_TitleScreenVideo);
        if ((R.d[0] & 0xFFFF) != 0)
            break;                         /* Start pressed on the title screen */
        CALL(f_029A16_IntroVideo);         /* attract mode: play the intro */
        if (R.d[0] != 0) {
            show_intro = 0;                /* Start pressed during it */
            break;
        }
    }
    if (show_intro)
        CALL(f_029A16_IntroVideo);

load_palettes:
    load_palette(WATER_PAL1, 0);
    load_palette(WATER_PAL2, 1);
    load_palette(WATER_PAL3, 2);
    load_palette(TOEJAM_EARL_PAL, 3);
    wr8(VBLANK_FLAG_6, 0);
    wr16(0xC00004u, 0x8C89);
    RETURN();
}
