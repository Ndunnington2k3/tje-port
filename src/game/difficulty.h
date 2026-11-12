/*
 * Difficulty modes (port feature): Chill Mode, Original, Nightmare! See difficulty.c.
 */
#ifndef TJE_DIFFICULTY_H
#define TJE_DIFFICULTY_H

enum { DIFF_ORIGINAL = 0, DIFF_CHILL = 1, DIFF_NIGHTMARE = 2 };

/* The mode of the game in the machine's RAM. */
int difficulty(void);
/* The mode in a copy of the RAM (a saved game's). */
int difficulty_in(const unsigned char *ram);
void difficulty_set(int mode);
const char *difficulty_name(int mode);

/* The "How Tough?" screen: sets the mode and returns it, or -1 for back. */
int difficulty_menu(void);

#endif
