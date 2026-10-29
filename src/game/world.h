/*
 * Worlds and world codes (port feature).
 *
 * A Random World comes from one number: StartGame seeds the game's random
 * number generator (Park-Miller, 1..$7FFFFFFE) and GenerateLevelsForGame then
 * draws the floors' seeds from it. Everything about the world (floors, items,
 * earthlings, where the ship pieces are) follows from that number.
 *
 * A world code writes the number in 6 characters of A-Z and 0-9: 36^6 codes for
 * 2^31 - 2 seeds, so every world has a code (codes above the last seed wrap
 * around). "Custom World" on the What screen plays the world of a typed code.
 */
#ifndef TJE_WORLD_H
#define TJE_WORLD_H

#include <stdint.h>

enum { WORLD_RANDOM, WORLD_FIXED, WORLD_CUSTOM };

#define WORLD_CODE_LEN 6

/* The world being played: its kind and seed (0 = unknown). */
extern int g_world_kind;
extern uint32_t g_world_seed;

void world_code(uint32_t seed, char out[WORLD_CODE_LEN + 1]);
uint32_t world_seed_from_code(const char *code);

#endif
