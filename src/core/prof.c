#include "core/prof.h"

#ifdef TJE_PROF
uint32_t prof_us[PROF_N];
uint32_t (*prof_clock)(void);
static int stack[16], depth;
static uint32_t last;

static void charge(void)
{
    uint32_t now = prof_clock ? prof_clock() : 0;
    prof_us[depth ? stack[depth - 1] : PROF_68K] += now - last;
    last = now;
}

void prof_push(int part)
{
    charge();
    if (depth < 16) stack[depth] = part;
    depth++;
}

void prof_pop(void)
{
    charge();
    if (depth > 0) depth--;
}
void prof_level0(void)
{
    charge();
    depth = 0;
}
#endif
