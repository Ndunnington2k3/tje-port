/*
 * Saved games (port feature).
 *
 * A save is not a save state. Loading one puts the players back at the start of
 * the floor they were on (stepping out of the elevator, as when they arrived),
 * with the presents, identified presents, stats and rocketship pieces they had
 * when they saved. A ship piece already collected is not on the floor again.
 *
 * How it works: the main game loop (InGameLoop, rewritten in C in save.c) takes a
 * machine snapshot whenever a player arrives on a floor. Saving stores that
 * floor's arrival snapshot with the current progress written into its RAM.
 * Loading restarts the machine from it and then tidies up (ship piece, HUD).
 */
#ifndef TJE_SAVE_H
#define TJE_SAVE_H

#include <stdint.h>

#define SAVE_SLOTS 3

typedef struct {
    int used;              /* 0: the slot is empty */
    int fixed_world;       /* 1: Fixed World, 0: Random World */
    int players;           /* 0 = ToeJam, 1 = Earl, 2 = both */
    int floor[2];          /* floor of ToeJam / Earl */
    int ship_pieces;       /* collected so far (of 10) */
} SaveInfo;

/* Summary of a slot (0..SAVE_SLOTS-1) for the file list. Returns info->used. */
int save_info(int slot, SaveInfo *info);

/* Can the game be saved right now? (a player is on a floor with a known start) */
int save_possible(void);

/* Save the game in progress to a slot. Returns 0 on success. */
int save_write(int slot);

/* Load a slot: abandons whatever is running and does not return on success.
 * Returns nonzero if the slot is empty or unreadable. */
int save_load(int slot);

/* Abandon the game and go back to the Who menu (does not return). */
void save_quit_to_menu(void);

/* Set by save_quit_to_menu() for the boot sequence: skip the SEGA screen, title
 * and intro once and go straight to the menus. */
extern int g_boot_to_menu;

#endif
