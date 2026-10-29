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
