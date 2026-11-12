/*
 * Difficulty modes (port feature): Chill Mode, Original, Nightmare!
 *
 * The mode is a byte of the game's RAM in a stretch the game never uses
 * ($FFE48A-$FFFDFF: never written after power-on), so it goes wherever RAM goes:
 * saved games, the snapshot that starts a network game (the host's choice reaches
 * Earl), the RAM checksums both machines compare. 0 is Original, so anything that
 * never set it (a save state, --vanilla) plays the original game.
 *
 * Chill Mode: lives never run out; promotions at two thirds of the score, each
 * one filling the energy.
 * Nightmare!: each floor gets the earthlings of the floor three above it (floor 1
 * too), all 20 places filled; earthlings got rid of come back - when the floor is
 * entered again, and after about a minute on it; bad presents a third more likely;
 * falling from floor 17 or above drops two floors.
 * Original is the game as it was: every routine here then runs the original
 * translation, untouched.
 */
#include "game.h"
#include "menu.h"
#include "difficulty.h"

#include <string.h>

#define DIFF_MODE            0xFFF000u   /* (unused RAM, see above) */
#define DIFF_RESPAWN         0xFFF010u   /* 2 x 20 words: earthling places' time gone */
#define DIFF_TICKS           0xFFF060u   /* word: ProcessEarthlings calls (the respawn clock) */

#define TOEJAMS_LIVES        0xFFA248u
#define TOEJAMS_HEALTH       0xFFA252u
#define HIGHEST_LEVEL        0xFF9132u
#define LEVEL_SEEDS          0xFF9136u
#define SHIP_PIECE_LEVELS    0xFFE212u
#define SHIP_PIECE_HERE      0xFFE220u
#define EARTHLINGS_IN_LEVEL  0xFFDE72u   /* 2 sets x 20 places x 18 bytes */
#define EARTHLINGS_GONE      0xFFE142u   /* per floor: a bit per place, earthling gone */
#define EARTHLING_LOAD_FLAG  0xFFE1AAu
#define EARTHLING_LEVEL      0xFFE1ACu
#define EARTHLING_DATA       0x025F82u   /* ROM: $26 bytes a kind */
#define EARTHLING_ROSTERS    0x0263CEu   /* ROM: 20 kinds a floor, $FF none */
#define PRESENT_WEIGHTS      0x0ABE38u   /* ROM: 27 weights */

#define SET_SIZE    0x168u
#define PLACE_SIZE  18u
#define PLACES      20
#define FLOORS      26                   /* 0..25 */
#define SANTA       0x14
#define DEVIL       0x01
#define MAIL_ORDER  9

#define NIGHTMARE_SHIFT     3            /* each floor gets the earthlings of this many floors up */
#define RESPAWN_TICKS       900          /* ProcessEarthlings calls (15 a second): a minute */
#define FALL_TWO_FROM       17

int difficulty(void)
{
    int m = rd8(DIFF_MODE);
    return m == DIFF_CHILL || m == DIFF_NIGHTMARE ? m : DIFF_ORIGINAL;
}

int difficulty_in(const unsigned char *ram)
{
    int m = ram[DIFF_MODE & 0xFFFFu];
    return m == DIFF_CHILL || m == DIFF_NIGHTMARE ? m : DIFF_ORIGINAL;
}

void difficulty_set(int mode)
{
    wr8(DIFF_MODE, (uint32_t)mode);
    memset(&g_ram[DIFF_RESPAWN & 0xFFFFu], 0, 2 * PLACES * 2);
    wr16(DIFF_TICKS, 0);
}

const char *difficulty_name(int mode)
{
    return mode == DIFF_CHILL ? "Chill Mode" : mode == DIFF_NIGHTMARE ? "Nightmare!" : "Original";
}

/* ---- the screen, after the What screen ---------------------------------------- */

int difficulty_menu(void)
{
    Menu m;
    int v;
    menu_init(&m);
    m.b_is_back = 1;
    menu_add(&m, 12, 10, "Chill Mode", DIFF_CHILL);
    menu_add(&m, 12, 15, "Original", DIFF_ORIGINAL);
    menu_add(&m, 12, 20, "Nightmare!", DIFF_NIGHTMARE);
    menu_add(&m, 12, 25, "Back", -3);
    menu_add(&m, 15, 6, "How Tough?", -1);
    menu_add(&m, 14, 12, "Lives that never run out", -1);
    menu_add(&m, 14, 17, "The game as it was", -1);
    menu_add(&m, 14, 22, "Good luck", -1);
    m.hand_col = 8;
    m.sel = 1;
    v = menu_run(&m, MENU_NO_TITLE);
    if (v < 0) return -1;
    difficulty_set(v);
    return v;
}

/* ---- Chill Mode ------------------------------------------------------------------ */

/* GetScoreForPromo: the score a rank needs - two thirds of it */
TJE_OVERRIDE(0x00B878)
void f_00B878_GetScoreForPromo(void)
{
    orig_f_00B878_GetScoreForPromo();
    if (difficulty() == DIFF_CHILL) R.d[0] = (uint32_t)(int32_t)((int16_t)R.d[0] * 2 / 3);
}

/* CheckPromotionScore: a promotion fills the energy (as a new life does) */
TJE_OVERRIDE(0x00B8AE)
void f_00B8AE_CheckPromotionScore(void)
{
    int p = (int16_t)(ARG_L(0) & 0xFFFF);
    orig_f_00B8AE_CheckPromotionScore();
    if (difficulty() == DIFF_CHILL && R.d[0] == 1 && (p == 0 || p == 1))
        wr8(TOEJAMS_HEALTH + (uint32_t)p, 0x64);
}

/* HealthAndLivesChecks: where a life is lost - never the last, and given back */
TJE_OVERRIDE(0x00BC28)
void f_00BC28_HealthAndLivesChecks(void)
{
    int8_t was[2];
    int p, chill = difficulty() == DIFF_CHILL;
    for (p = 0; p < 2; p++) {
        was[p] = (int8_t)rd8(TOEJAMS_LIVES + (uint32_t)p);
        if (chill && was[p] == 0) wr8(TOEJAMS_LIVES + (uint32_t)p, (uint32_t)(was[p] = 1));
    }
    orig_f_00BC28_HealthAndLivesChecks();
    if (chill)
        for (p = 0; p < 2; p++)
            if (was[p] >= 0 && (int8_t)rd8(TOEJAMS_LIVES + (uint32_t)p) < was[p])
                wr8(TOEJAMS_LIVES + (uint32_t)p, (uint32_t)was[p]);
}

/* ---- Nightmare!: earthlings ------------------------------------------------------- */

/* the kind in a place of a floor: the list of the floor three up, its empty
 * places filled from its own kinds in turn */
static int nightmare_kind(int floor, int place)
{
    uint32_t list = EARTHLING_ROSTERS + 20u * (uint32_t)(floor + NIGHTMARE_SHIFT > FLOORS - 1 ? FLOORS - 1 : floor + NIGHTMARE_SHIFT);
    int kinds[PLACES], n = 0, i;
    if (floor == 0) return (int8_t)rd8(EARTHLING_ROSTERS + (uint32_t)place);   /* (floor 0 as it was) */
    for (i = 0; i < PLACES; i++) {
        int k = (int8_t)rd8(list + (uint32_t)i);
        if (k > 0) kinds[n++] = k;
    }
    if (!n) return -1;
    {
        int k = (int8_t)rd8(list + (uint32_t)place);
        return k > 0 ? k : kinds[place % n];
    }
}

static uint32_t place_at(int set, int place)
{
    return EARTHLINGS_IN_LEVEL + SET_SIZE * (uint32_t)set + PLACE_SIZE * (uint32_t)place;
}

static int stack_random(uint32_t v)
{
    push32(v);
    CALL(f_004EE8_StackBasedRandom);
    POP_ARGS(1);
    return (int)R.d[0];
}

/* FindSpawnLocationForEarthling for a place: 0 found (x, y set), else not */
static int find_spot(int kind, int set, int floor, int16_t *x, int16_t *y)
{
    uint32_t sp, r;
    /* (the two words it writes, on the 68000's stack: below what is in use) */
    R.a[7] -= 4;
    sp = R.a[7];
    push32((uint32_t)floor);
    push32(sp + 2);             /* y: -6(A6) in the original */
    push32(sp);                 /* x: -8(A6) */
    push32((uint32_t)set);
    push32((uint32_t)kind);
    CALL(f_01778A_FindSpawnLocationForEarthling);
    POP_ARGS(5);
    r = R.d[0];
    *x = (int16_t)rd16(sp);
    *y = (int16_t)rd16(sp + 2);
    R.a[7] += 4;
    return r != 0;
}

static void fill_place(int set, int place, int kind, int floor, int16_t x, int16_t y)
{
    uint32_t e = place_at(set, place);
    wr8(e, (uint32_t)kind);
    wr8(e + 2, (uint32_t)floor);
    wr16(e + 6, (uint16_t)x);
    wr16(e + 8, (uint16_t)y);
    wr16(e + 0xC, (uint16_t)y);
    if (kind == 0x13 || kind == MAIL_ORDER) { wr16(e + 0xA, 0); wr16(e + 0xC, 0); }
    else if (kind == 7) wr16(e + 0xA, 1);
    else wr16(e + 0xA, (uint16_t)x);
    wr8(e + 0x10, rd8(EARTHLING_DATA + (uint32_t)kind * 0x26u + 0x24u));
    wr8(e + 0x11, 0);
}

/* LOC_00017A86 (loading a floor's earthlings), Nightmare's way: as the original
 * (same calls, same random numbers drawn in the same order) but with the floor's
 * list from three floors up, every place filled, floor 1 included, and the
 * earthlings got rid of on an earlier visit back again. */
static void nightmare_load(int floor)
{
    int set, place, ship = 0, mail;
    uint32_t a3;
    int16_t x = 0, y = 0;
    if (floor >= FLOORS) return;
    for (set = 0; set < 2; set++)
        if (rd8(EARTHLING_LOAD_FLAG + (uint32_t)set) && (int8_t)rd8(EARTHLING_LEVEL + (uint32_t)set) == floor) {
            wr8(EARTHLING_LOAD_FLAG + (uint32_t)set, rd8(EARTHLING_LOAD_FLAG + (uint32_t)set) + 1);
            return;
        }
    set = rd8(EARTHLING_LOAD_FLAG) ? 1 : 0;
    a3 = (uint32_t)stack_random(rd16(LEVEL_SEEDS + 2u * (uint32_t)floor));
    if (floor) {
        int i;
        for (i = 0; i < 10; i++)
            if ((int8_t)rd8(SHIP_PIECE_LEVELS + (uint32_t)i) == floor) ship = 1;
    }
    push32((uint32_t)floor);
    push32((uint32_t)set);
    CALL(f_020E32);
    POP_ARGS(2);
    mail = (int8_t)R.d[0];
    /* (back again: whatever was got rid of last time) */
    if (floor) wr32(EARTHLINGS_GONE + 4u * (uint32_t)floor, 0);
    for (place = 0; place < PLACES; place++) {
        int kind;
        if (mail) {
            kind = MAIL_ORDER;
            mail = 0;
        } else if (ship) {
            uint32_t sp;
            kind = 0;
            R.a[7] -= 4;
            sp = R.a[7];
            push32(sp + 2);
            push32(sp);
            CALL(f_017A52);
            POP_ARGS(2);
            x = (int16_t)rd16(sp);
            y = (int16_t)rd16(sp + 2);
            R.a[7] += 4;
            ship = 0;
            if ((int8_t)rd8(HIGHEST_LEVEL) == floor) wr8(SHIP_PIECE_HERE, (uint32_t)floor);
        } else {
            if (a3) { stack_random(a3); a3 = 0; }
            kind = nightmare_kind(floor, place);
            if (kind == SANTA && (int8_t)rd8(HIGHEST_LEVEL) > floor) kind = DEVIL;
        }
        if (kind > 0) {
            if (floor == 0) {
                uint32_t sp;
                R.a[7] -= 4;
                sp = R.a[7];
                push32(sp + 2);
                push32(sp);
                push32((uint32_t)kind);
                CALL(f_023C3C);
                POP_ARGS(3);
                x = (int16_t)rd16(sp);
                y = (int16_t)rd16(sp + 2);
                R.a[7] += 4;
            } else if (find_spot(kind, set, floor, &x, &y)) {
                wr32(EARTHLINGS_GONE + 4u * (uint32_t)floor,
                     rd32(EARTHLINGS_GONE + 4u * (uint32_t)floor) | (0x80000000u >> place));
            }
        }
        if (kind < 0 || (kind > 0 && (rd32(EARTHLINGS_GONE + 4u * (uint32_t)floor) & (0x80000000u >> place))))
            wr8(place_at(set, place), 0xFF);
        else
            fill_place(set, place, kind, floor, x, y);
        wr16(DIFF_RESPAWN + 2u * (uint32_t)(set * PLACES + place), 0);
    }
    wr8(EARTHLING_LOAD_FLAG + (uint32_t)set, rd8(EARTHLING_LOAD_FLAG + (uint32_t)set) + 1);
    wr8(EARTHLING_LEVEL + (uint32_t)set, (uint32_t)floor);
}

TJE_OVERRIDE(0x017A86)
void f_017A86(void)
{
    if (difficulty() != DIFF_NIGHTMARE) { orig_f_017A86(); return; }
    {
        /* (entered as the original: LINK and saved registers are its business; this
         * keeps D2-D7/A2-A6 by not touching them) */
        uint32_t keep[6];
        int i;
        for (i = 0; i < 6; i++) keep[i] = R.d[2 + i];
        nightmare_load((int16_t)(ARG_L(0) & 0xFFFF));
        for (i = 0; i < 6; i++) R.d[2 + i] = keep[i];
    }
    RETURN();
}

/* ProcessEarthlings: in Nightmare, an earthling got rid of on the floor comes back
 * (somewhere else on it) after about a minute */
TJE_OVERRIDE(0x0190CE)
void f_0190CE_ProcessEarthlings(void)
{
    int set, place;
    orig_f_0190CE_ProcessEarthlings();
    if (difficulty() != DIFF_NIGHTMARE) return;
    for (set = 0; set < 2; set++) {
        int floor = (int8_t)rd8(EARTHLING_LEVEL + (uint32_t)set);
        if (!rd8(EARTHLING_LOAD_FLAG + (uint32_t)set) || floor <= 0 || floor >= FLOORS) continue;
        for (place = 0; place < PLACES; place++) {
            uint32_t t = DIFF_RESPAWN + 2u * (uint32_t)(set * PLACES + place), bit = 0x80000000u >> place;
            uint32_t gone = EARTHLINGS_GONE + 4u * (uint32_t)floor;
            int kind = nightmare_kind(floor, place), n;
            int16_t x, y;
            if (kind <= 0 || rd8(place_at(set, place)) != 0xFF) { wr16(t, 0); continue; }
            n = rd16(t) + 1;
            if (n < RESPAWN_TICKS) { wr16(t, (uint32_t)n); continue; }
            wr16(t, 0);
            if (kind == SANTA && (int8_t)rd8(HIGHEST_LEVEL) > floor) kind = DEVIL;
            if (find_spot(kind, set, floor, &x, &y)) continue;   /* (no room: try again a minute later) */
            wr32(gone, rd32(gone) & ~bit);
            fill_place(set, place, kind, floor, x, y);
        }
    }
}

/* ---- Nightmare!: presents ----------------------------------------------------------- */

static int bad_present(int p)
{
    /* rain cloud, total bummer, randomizer, earthling, school book */
    return p == 0x0D || p == 0x10 || p == 0x12 || p == 0x17 || p == 0x18;
}

/* SelectARandomPresent: the same draw from the same random numbers, with the bad
 * presents' weights a third more likely (good x5, bad x7: 15.5% -> 20.4%) */
TJE_OVERRIDE(0x014ADA)
void f_014ADA_SelectARandomPresent(void)
{
    int w[27], total = 0, i, r, sum;
    if (difficulty() != DIFF_NIGHTMARE) { orig_f_014ADA_SelectARandomPresent(); return; }
    CY(200);
    for (i = 0; i < 27; i++) {
        w[i] = (int)rd8(PRESENT_WEIGHTS + (uint32_t)i) * (bad_present(i) ? 7 : 5);
        total += w[i];
    }
    do {
        CALL(f_004E7E_GenerateRandom);
        r = (int)((R.d[0] & 0xFFFFu) % (uint32_t)(total + 1));
    } while (r == 0);
    for (i = 0, sum = 0; i < 27; i++) {
        sum += w[i];
        if (r <= sum) break;
    }
    R.d[0] = (uint32_t)i;
    RETURN();
}

/* ---- Nightmare!: falling ------------------------------------------------------------ */

static int falling;   /* (inside MakePlayerFall) */

TJE_OVERRIDE(0x00FD78)
void f_00FD78_MakePlayerFall(void)
{
    falling++;
    orig_f_00FD78_MakePlayerFall();
    falling--;
}

/* LOC_000143B6: MakePlayerFall's last call before taking a floor off the faller
 * (A3: the player). From floor 17 up, Nightmare takes another. */
TJE_OVERRIDE(0x0143B6)
void f_0143B6(void)
{
    int floor = (int16_t)(ARG_L(0) & 0xFFFF);
    uint32_t who = R.a[3];
    orig_f_0143B6();
    if (falling && difficulty() == DIFF_NIGHTMARE && floor >= FALL_TWO_FROM &&
        (who == 0xFFA25Au || who == 0xFFA2DAu) && (int8_t)rd8(who + 0x4C) == floor)
        wr8(who + 0x4C, (uint32_t)(floor - 1));
}
