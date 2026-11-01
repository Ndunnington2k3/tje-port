/*
 * Scene changes without garbage (port option: g_options.clean_transitions).
 *
 * Most scene changes switch the display off while the next screen is built;
 * the renderer hides the torn and stale frames around those (src/core/vdp.c).
 * A few build the next screen with the display on. Those are wrapped here so
 * the picture stays frozen on the last good frame until the new one is ready.
 */
#include "game.h"
#include "core/vdp.h"

/* LOC_00013540: the elevator has closed and the ride screen (the starry shaft)
 * replaces the floor - planes, tiles, palette and scroll all rewritten while
 * the display is on. Frames after it also still show the floor's HUD sprites
 * until the sprite table is next uploaded. */
TJE_OVERRIDE(0x013540)
void f_013540(void)
{
    vdp_freeze(-1);
    orig_f_013540();          /* pops its own return address */
    vdp_freeze(2);
}

/* ---- two-window mode: the split screen without its transition ----------------
 * With a window per player (network play) the screen is never split: each window
 * draws its own player. The game still splits and joins its cameras (that decides
 * which earthlings wake up, who shares a present, and so on), but the screen work
 * that goes with it is not needed: LOC_00000C2E (split), LOC_00000D7E (join) and
 * LOC_00000EE6 (one camera again) switch the display off, wait for VBlanks and
 * redraw both planes while the display is on. Each skips all of that when it finds
 * the display already off, so it is run believing so: the game's state changes as
 * always, at once, with no pause, fade or redraw. */
extern int g_gv_windows;

static void as_if_display_off(void (*orig)(void))
{
    uint32_t shadow = rd8(0xFF8000u);   /* (the game's copy of VDP register 1) */
    wr8(0xFF8000u, shadow & ~0x40u);
    orig();
    wr8(0xFF8000u, (rd8(0xFF8000u) & ~0x40u) | (shadow & 0x40u));
}

TJE_OVERRIDE(0x000C2E)
void f_000C2E(void)
{
    if (g_gv_windows) as_if_display_off(orig_f_000C2E);
    else orig_f_000C2E();
}

TJE_OVERRIDE(0x000D7E)
void f_000D7E(void)
{
    if (g_gv_windows) as_if_display_off(orig_f_000D7E);
    else orig_f_000D7E();
}

TJE_OVERRIDE(0x000EE6)
void f_000EE6(void)
{
    if (g_gv_windows) as_if_display_off(orig_f_000EE6);
    else orig_f_000EE6();
}
