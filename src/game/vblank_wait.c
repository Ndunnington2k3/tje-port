/*
 * WaitForNVBlanks (LOC_000578): the game's wait for the next VBlank(s) - most of
 * each frame spent turning a 26-cycle loop (read VBLANK_COUNTER, compare, branch)
 * until the interrupt changes the counter. The same, cycle for cycle, with the
 * turns before the next event (the next line: nothing can change the counter
 * before then) added at once instead of run one by one.
 */
#include "game.h"

#define VBLANK_COUNTER 0xFF800Eu

TJE_OVERRIDE(0x000578)
void f_000578_WaitForNVBlanks(void)
{
    uint32_t d1;
    CY(42);
    RT_POLL();
    R.d[0] = (R.d[0] & 0xFFFF0000u) | (rd16(R.a[7] + 6) & 0xFFFFu);   /* (the count, off the stack) */
    if (!((rd8(0xFF8000u) >> 6) & 1)) {                                 /* (the display off: no wait) */
        CY(16);
        RETURN();
    }
    CY(10);
    for (;;) {
        CY(14);
        if ((int16_t)R.d[0] <= 0) break;                                /* TST.w D0 / BGT */
        CY(16);
        RT_POLL();
        d1 = rd16(VBLANK_COUNTER);
        R.d[1] = (R.d[1] & 0xFFFF0000u) | d1;
        for (;;) {
            int32_t gap;
            CY(26);
            RT_POLL();
            if (rd16(VBLANK_COUNTER) != d1) break;
            /* (the turns that end before the next event, all at once) */
            gap = (int32_t)(R.next_event - R.cycles);
            if (gap > 26) R.cycles += (uint32_t)((gap - 1) / 26) * 26u;
        }
        CY(4);
        R.d[0] = (R.d[0] & 0xFFFF0000u) | ((R.d[0] - 1) & 0xFFFFu);
    }
    CY(16);
    RETURN();
}
