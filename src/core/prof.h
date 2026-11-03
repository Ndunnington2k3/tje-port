/*
 * Where the time goes, by part (builds with TJE_PROF, e.g. the PSP's profiling
 * build): each part's time excludes the parts it calls. The platform sets the
 * clock (microseconds) and reads prof_us[] (then clears it).
 */
#ifndef TJE_PROF_H
#define TJE_PROF_H
#include <stdint.h>

enum { PROF_68K, PROF_VDP, PROF_Z80, PROF_FM, PROF_SOUND, PROF_VIEW, PROF_PRESENT, PROF_HOOKS, PROF_N };

#ifdef TJE_PROF
extern uint32_t prof_us[PROF_N];
extern uint32_t (*prof_clock)(void);
void prof_push(int part);
void prof_pop(void);
void prof_level0(void);
#define PROF_PUSH(p) prof_push(p)
#define PROF_POP() prof_pop()
#define PROF_LEVEL0() prof_level0()
#else
#define PROF_PUSH(p) ((void)0)
#define PROF_POP() ((void)0)
#define PROF_LEVEL0() ((void)0)
#endif

#endif
