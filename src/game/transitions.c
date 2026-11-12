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
    extern int g_gv_ride_screen;
    vdp_freeze(-1);
    orig_f_013540();          /* pops its own return address */
    vdp_freeze(2);
    g_gv_ride_screen = 1;     /* (two windows: the ride screen is up - gameview.c) */
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

/* ---- two-window mode: a present list does not stop the game ----------------
 * CheckBButton (LOC_00009868): with the players sharing the screen, a present
 * list opened stops the whole game (a loop of its own, only the list running,
 * until it closes); with the screen split it is part of the main loop and the
 * other player plays on. With a window per player there is no shared screen:
 * the list always goes the second way (the game then splits, as it does for a
 * list opened when apart, with no transition here). */
TJE_OVERRIDE(0x009868)
void f_009868_CheckBButton(void)
{
    int p;
    if (!g_gv_windows) { orig_f_009868_CheckBButton(); return; }
    CY(30);
    /* A list about to open with the players together: split first (as the game
     * does a moment later anyway), so it opens in its player's half, as when
     * they are apart - opened on the shared screen, the split would leave it
     * there, out of its player's window. (ProcessInventory: B newly pressed by a
     * player in the game, list closed.) */
    if (!rd8(0xFF8010u) && (int8_t)rd8(0xFFA248u) >= 0 && (int8_t)rd8(0xFFA249u) >= 0)
        for (p = 0; p < 2; p++) {
            uint32_t st = rd8(0xFFA25Au + 0x80u * (uint32_t)p + 0x4B);
            if (!rd8(0xFF9364u + (uint32_t)p) && st != 5 && st != 6 &&
                !(rd8(0xFF8020u + (uint32_t)p) & 0x10) && (rd8(0xFF801Eu + (uint32_t)p) & 0x10)) {
                push32(0);
                CALL(f_000C2E);
                POP_ARGS(1);
                break;
            }
        }
    CALL(f_00949C_ProcessInventory);
    CY(30);
    RETURN();
}

/* ---- the ship's take-off (the last piece found) -----------------------------
 * LOC_0001FDF0 boards the players, flies the ship to Funkotron over the credits
 * and loads the last floor, all from inside the game's main loop: a window per
 * player (network play) would draw its player's floor meanwhile. It shows the
 * game's own picture instead (gameview.c). */
extern int g_gv_ship_scene;

TJE_OVERRIDE(0x01FDF0)
void f_01FDF0(void)
{
    g_gv_ship_scene = 1;
    orig_f_01FDF0();
    g_gv_ship_scene = 0;
}
