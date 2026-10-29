/*
 * Conventions for hand-written C that replaces translated game routines.
 *
 * Mark a replacement with TJE_OVERRIDE(0xADDRESS) on its own line next to the
 * function, using the routine's ROM address. tools/generate.py finds the marker,
 * and the translator then emits the original as orig_<name>. Every call from
 * translated code (and every indirect call) reaches your function instead.
 * Re-run generate.py after adding or removing a marker; CMake checks this.
 *
 * A replacement is entered exactly like the 68000 routine:
 *   - the caller has pushed a return address; finish with RETURN() to pop it;
 *   - stack arguments are at ARG_L(0), ARG_L(1)... (longs) above that address;
 *   - return values go in R.d[0];
 *   - D2-D7 and A2-A6 must be left as you found them (the original compiler's rules).
 *     Using C locals instead of R.d[]/R.a[] makes this automatic.
 *
 * To call translated (or replaced) routines, push arguments last-to-first with
 * push32(), call with CALL(f_xxxxxx_Name), then drop them with POP_ARGS(n).
 */
#ifndef TJE_GAME_H
#define TJE_GAME_H

#include "core/m68k_rt.h"
#include "core/genesis.h"
#include "gen_funcs.h"
#include "options.h"

/* Marker for tools/generate.py; expands to nothing. */
#define TJE_OVERRIDE(addr)

/* Call a routine the way JSR does: push a return address, call; the callee pops it. */
#define CALL(fn) do { push32(0); fn(); } while (0)
/* Remove n long arguments pushed for a CALL. */
#define POP_ARGS(n) (R.a[7] += 4u * (uint32_t)(n))
/* Return from a replacement the way RTS does. */
#define RETURN() do { R.a[7] += 4; return; } while (0)
/* n-th long argument of the replacement itself (0 = first). */
#define ARG_L(n) rd32(R.a[7] + 4u + 4u * (uint32_t)(n))

/* Frequently used RAM locations (names from the disassembly's GameConstants.X68). */
#define VBLANK_COUNTER   0xFF800Eu   /* word, incremented by the VBlank handler */
#define VBLANK_OVERRIDE  0xFF800Au   /* long, routine the VBlank handler calls if set */

/* Wait for the next VBlank like the game's own `move.w (A2),D0 / cmp.w (A2),D0 / beq`
 * loops, charging the same 68000 time per iteration. */
static inline void wait_vblank(void)
{
    uint32_t v = rd16(VBLANK_COUNTER);
    do { CY(18); RT_POLL(); } while (rd16(VBLANK_COUNTER) == v);
}

/* Is Start held on either controller? (current hardware state, no side effects) */
static inline int start_held(void)
{
    return ((g_pad[0] | g_pad[1]) & PAD_START) != 0;
}

#endif
