/*
 * Saved games: arrival snapshots, the save file format, loading.
 * See save.h for the design.
 */
#include "game.h"
#include "save.h"
#include "core/ser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* RAM (names from GameConstants.X68) */
#define PLAYER_INDICATOR      0xFF802Eu   /* 0 ToeJam, 1 Earl, 2 both */
#define TOEJAMS_CURRENT_LEVEL 0xFF912Cu   /* Earl's is the next word */
#define HIGHEST_LEVEL         0xFF9132u
#define FIXED_WORLD_FLAG      0xFF9134u
#define PLAYER_STATS          0xFFA248u   /* lives, cash, score, promotion, health ... */
#define PLAYER_STATS_END      0xFFA25Au
#define ELEVATOR_FLAG         0xFFDA6Au   /* per player: elevator they are in */
#define PRESENT_IDENTITIES    0xFFDA8Au   /* followed by both inventories */
#define INVENTORIES_END       0xFFDAE2u
#define LEVEL_ITEMS           0xFFDAE2u   /* 2 slots x 32 items x 8 bytes, $FF = empty */
#define LEVEL_PLAYERS_FLAG    0xFFDCE2u   /* per slot: players on that floor */
#define LEVEL_LOADED          0xFFDCE4u   /* per slot: floor whose items are loaded */
#define DROPPED_PRESENTS      0xFFDCE6u
#define ITEMS_TAKEN           0xFFDDE8u   /* per floor: 32-bit mask of items picked up */
#define ITEMS_TAKEN_END       (ITEMS_TAKEN + 26u * 4u)
#define EARTHLINGS_IN_LEVEL   0xFFDE72u   /* 2 x 20 entries of 18 bytes */
#define EARTHLING_LOAD_FLAG   0xFFE1AAu
#define EARTHLING_LEVEL       0xFFE1ACu
#define SHIP_PIECE_LEVELS     0xFFE212u   /* 10 bytes: floor of each piece, 0 = collected */
#define FINAL_LEVEL           0x1A

#define EARTHLING_SIZE  18
#define EARTHLINGS_PER_BLOCK 20
#define SHIP_PIECE      0x00              /* earthling type */
#define NO_EARTHLING    0xFF

#define MAX_FLOORS 32

int g_boot_to_menu;

/* ---- arrival snapshots ------------------------------------------------------ */

typedef struct {
    uint8_t *blob;
    size_t len;
    int elevator;          /* taken as a player stepped out of an elevator */
    unsigned stamp;        /* order taken */
} FloorSnap;

static FloorSnap floor_snap[MAX_FLOORS];
static unsigned snap_counter;
static int latest_floor = -1;
static int last_level[2];

/* After loading a save: the slot's snapshot, and work left for the first loop pass. */
static uint8_t *loaded_blob;
static size_t loaded_len;
static int post_load_pending;

static void snap_clear(void)
{
    int i;
    for (i = 0; i < MAX_FLOORS; i++) {
        free(floor_snap[i].blob);
        floor_snap[i].blob = NULL;
        floor_snap[i].len = 0;
    }
    latest_floor = -1;
}

static void snap_store(int floor, uint8_t *blob, size_t len, int elevator)
{
    FloorSnap *f = &floor_snap[floor];
    free(f->blob);
    f->blob = blob;
    f->len = len;
    f->elevator = elevator;
    f->stamp = ++snap_counter;
    latest_floor = floor;
}

static int player_active(int p)
{
    uint32_t who = rd8(PLAYER_INDICATOR);
    return who == 2 || (int)who == p;
}

static int player_floor(int p) { return (int)rd8(TOEJAMS_CURRENT_LEVEL + 2u * (uint32_t)p); }

/* Called at the top of every main-loop pass: a player arriving on a floor
 * gets that floor's start recorded. */
static void watch_arrivals(void)
{
    int p, floor = -1, elevator = 0;
    for (p = 0; p < 2; p++) {
        int lv = player_floor(p);
        if (!player_active(p)) { last_level[p] = lv; continue; }
        if (lv != last_level[p]) {
            last_level[p] = lv;
            floor = lv;
            elevator = rd8(ELEVATOR_FLAG + (uint32_t)p) != 0;
        }
    }
    if (floor < 0 || floor >= MAX_FLOORS || floor == FINAL_LEVEL)
        return;
    /* Keep the elevator arrival if we have one: a floor reached by falling is
     * restarted from where its elevator dropped the player off earlier. */
    if (!elevator && floor_snap[floor].blob && floor_snap[floor].elevator) {
        floor_snap[floor].stamp = ++snap_counter;
        latest_floor = floor;
        return;
    }
    {
        size_t n = genesis_snapshot(NULL, 0);
        uint8_t *blob = (uint8_t *)malloc(n);
        if (!blob) return;
        if (genesis_snapshot(blob, n) != n) { free(blob); return; }
        snap_store(floor, blob, n, elevator);
    }
}

/* A new game started (the loop was entered from StartGame or a save state). */
static void loop_entered(void)
{
    snap_clear();
    last_level[0] = last_level[1] = -1;   /* the first pass records the starting floor */
}

/* ---- after loading -------------------------------------------------------- */

static int ship_piece_on_floor(int floor)
{
    int i;
    for (i = 0; i < 10; i++)
        if ((int)rd8(SHIP_PIECE_LEVELS + (uint32_t)i) == floor) return 1;
    return 0;
}

/* The floor's earthlings were generated when the players arrived, possibly with a
 * ship piece that has since been collected. Take it away the way collecting does. */
static void remove_collected_ship_pieces(void)
{
    int b, e;
    for (b = 0; b < 2; b++) {
        int floor;
        if (!rd8(EARTHLING_LOAD_FLAG + (uint32_t)b)) continue;
        floor = (int)rd8(EARTHLING_LEVEL + (uint32_t)b);
        if (ship_piece_on_floor(floor)) continue;
        for (e = 0; e < EARTHLINGS_PER_BLOCK; e++) {
            uint32_t ent = EARTHLINGS_IN_LEVEL + (uint32_t)(b * EARTHLINGS_PER_BLOCK + e) * EARTHLING_SIZE;
            if (rd8(ent) != SHIP_PIECE || (int)rd8(ent + 2) != floor) continue;
            if (rd8(ent + 1)) {
                /* it has a sprite: release it (LOC_0000D950) */
                push32(rd8(ent + 1));
                CALL(f_00D950);
                POP_ARGS(1);
            }
            wr8(ent, NO_EARTHLING);
            wr8(ent + 1, 0);
        }
    }
}

static void after_load(void)
{
    snap_clear();
    snap_store(player_active(0) ? player_floor(0) : player_floor(1), loaded_blob, loaded_len, 1);
    loaded_blob = NULL;
    last_level[0] = player_floor(0);
    last_level[1] = player_floor(1);
    remove_collected_ship_pieces();
    CALL(f_00B72E_DrawHUD);   /* the HUD was drawn on arrival, with the stats of then */
}

/* ---- the main game loop ------------------------------------------------------
 * InGameLoop (Source/Misc/Code/InGameLoop.X68), same work and timing as the
 * original, plus the arrival watch. D2 counts passes; D3 holds the VBlank count. */

#define JSR(fn, ret) do { push32(ret); fn(); } while (0)

TJE_OVERRIDE(0x0014E6)
void f_0014E6_InGameLoop(void)
{
    if (post_load_pending) {
        post_load_pending = 0;
        after_load();
    } else {
        loop_entered();
    }
    for (;;) {
        CY(44);
        RT_POLL();
        watch_arrivals();
        R.d[3] = (R.d[3] & 0xFFFF0000u) | rd16(VBLANK_COUNTER);
        R.d[2] = (R.d[2] & 0xFFFF0000u) | ((R.d[2] + 1) & 0xFFFFu);
        R.d[0] = (R.d[0] & 0xFFFF0000u) | (R.d[2] & 0xFFFFu);
        if (R.d[2] & 1) {
            /* odd passes */
            CY(40);
            push32(0);
            JSR(f_00105A, 0x0014FCu);
            CY(20);
            JSR(f_0028F6, 0x001502u);
            CY(20);
            JSR(f_01F68E_ProcessLand, 0x001508u);
            CY(20);
            JSR(f_00CC62, 0x00150Eu);
            CY(18);
            R.a[7] += 4;
        } else {
            CY(20);
            JSR(f_00219C, 0x001518u);
            CY(20);
            if (R.d[2] & 2) {
                CY(20);
                JSR(f_0190CE_ProcessEarthlings, 0x001524u);
                CY(20);
                JSR(f_012A26, 0x00152Au);
                CY(20);
                JSR(f_01416C_ProcessElevators, 0x001530u);
                CY(20);
                JSR(f_015A08, 0x001536u);
                CY(20);
                JSR(f_01C4E8_ProcessTomatoRain, 0x00153Cu);
                CY(10);
            } else {
                /* every fourth pass */
                CY(42);
                push32(0);
                JSR(f_00DAB6_UpdateLandAroundPlayer, 0x001546u);
                CY(36);
                push32(1);
                JSR(f_00DAB6_UpdateLandAroundPlayer, 0x001550u);
                CY(18);
                JSR(f_000A6E_ProcessInput, 0x001554u);
                CY(20);
                JSR(f_0111C8_ProcessPlayers, 0x00155Au);
                CY(20);
                JSR(f_00C110_Process2FEntityFlag, 0x001560u);
                CY(8);
                R.a[7] += 8;
            }
            CY(20);
            JSR(f_00E402_ProcessMovement, 0x001568u);
            CY(20);
            JSR(f_00BFA6, 0x00156Eu);
            CY(20);
            JSR(f_00CB34, 0x001574u);
            CY(20);
            JSR(f_01E4A2, 0x00157Au);
        }
        CY(10);
        /* wait for the next frame, stirring the random numbers meanwhile */
        for (;;) {
            CY(26);
            if (rd16(VBLANK_COUNTER) != (R.d[3] & 0xFFFFu))
                break;
            CY(10);
            CY(20);
            RT_POLL();
            JSR(f_004E7E_GenerateRandom, 0x001582u);
        }
    }
}

/* ---- save files ------------------------------------------------------------- */

#define SAVE_MAGIC   "TJESAVE"
#define SAVE_VERSION 1u

static void slot_path(int slot, char *out, size_t size)
{
    const char *dir = getenv("TJE_SAVE_DIR");
    snprintf(out, size, "%s%stje_save%d.sav", dir ? dir : "", dir ? "/" : "", slot + 1);
}

static int count_ship_pieces(const uint8_t *ram)
{
    int i, n = 0;
    for (i = 0; i < 10; i++)
        if (ram[(SHIP_PIECE_LEVELS & 0xFFFFu) + (uint32_t)i] == 0) n++;
    return n;
}

static void info_from_ram(SaveInfo *info, const uint8_t *ram)
{
    info->used = 1;
    info->fixed_world = ram[FIXED_WORLD_FLAG & 0xFFFFu] != 0;
    info->players = ram[PLAYER_INDICATOR & 0xFFFFu];
    info->floor[0] = ram[TOEJAMS_CURRENT_LEVEL & 0xFFFFu];
    info->floor[1] = ram[(TOEJAMS_CURRENT_LEVEL + 2u) & 0xFFFFu];
    info->ship_pieces = count_ship_pieces(ram);
}

/* Read a slot file. On success *blob (malloc'd) holds the snapshot. */
static int read_slot(int slot, SaveInfo *info, uint8_t **blob, size_t *len)
{
    char path[512];
    FILE *f;
    long n;
    uint8_t *data;
    Ser s;
    uint32_t blen, sum, i, check = 0;
    char magic[8];

    memset(info, 0, sizeof *info);
    slot_path(slot, path, sizeof path);
    f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 4 * 1024 * 1024) { fclose(f); return -2; }
    data = (uint8_t *)malloc((size_t)n);
    if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(data); return -3; }
    fclose(f);

    s.buf = data; s.pos = 0; s.cap = (size_t)n; s.err = 0;
    des_bytes(&s, magic, 8);
    if (memcmp(magic, SAVE_MAGIC, 8) != 0 || des_u32(&s) != SAVE_VERSION) { free(data); return -4; }
    info->fixed_world = (int)des_u8(&s);
    info->players = (int)des_u8(&s);
    info->floor[0] = (int)des_u8(&s);
    info->floor[1] = (int)des_u8(&s);
    info->ship_pieces = (int)des_u8(&s);
    blen = des_u32(&s);
    sum = des_u32(&s);
    if (s.err || s.pos + blen > (size_t)n) { free(data); return -5; }
    for (i = 0; i < blen; i++) check = check * 31u + data[s.pos + i];
    if (check != sum || !genesis_snapshot_valid(data + s.pos, blen)) { free(data); return -6; }
    info->used = 1;
    if (blob) {
        *blob = (uint8_t *)malloc(blen);
        if (!*blob) { free(data); return -7; }
        memcpy(*blob, data + s.pos, blen);
        *len = blen;
    }
    free(data);
    return 0;
}

int save_info(int slot, SaveInfo *info)
{
    if (read_slot(slot, info, NULL, NULL) != 0) info->used = 0;
    return info->used;
}

/* The snapshot a save made now would restart from. */
static FloorSnap *save_source(void)
{
    if (rd8(PLAYER_INDICATOR) == 2) {
        /* two players: the most recent arrival, whoever it was */
        return latest_floor >= 0 ? &floor_snap[latest_floor] : NULL;
    } else {
        int floor = player_floor(player_active(0) ? 0 : 1);
        if (floor < 0 || floor >= MAX_FLOORS) return NULL;
        return floor_snap[floor].blob ? &floor_snap[floor] : NULL;
    }
}

int save_possible(void)
{
    int p;
    if (player_floor(0) == FINAL_LEVEL || player_floor(1) == FINAL_LEVEL) return 0;
    for (p = 0; p < 2; p++)
        if (player_active(p) && (int8_t)rd8(PLAYER_STATS + (uint32_t)p) < 0) return 0;  /* out of lives */
    return save_source() != NULL;
}

/* Write the progress made since arriving into the snapshot's RAM. */
static void patch_progress(uint8_t *ram)
{
    static const struct { uint32_t from, to; } keep[] = {
        {PLAYER_STATS, PLAYER_STATS_END},          /* lives, bucks, points, rank, health */
        {HIGHEST_LEVEL, HIGHEST_LEVEL + 1},
        {PRESENT_IDENTITIES, INVENTORIES_END},     /* which presents are known; both inventories */
        {SHIP_PIECE_LEVELS, SHIP_PIECE_LEVELS + 10},
        {DROPPED_PRESENTS, ITEMS_TAKEN_END},       /* presents dropped, items taken on every floor */
    };
    unsigned i;
    int slot, item;
    for (i = 0; i < sizeof keep / sizeof keep[0]; i++)
        memcpy(ram + (keep[i].from & 0xFFFFu), g_ram + (keep[i].from & 0xFFFFu), keep[i].to - keep[i].from);
    /* The floor's items were laid out when the players arrived; empty the ones
     * taken since, as PlaceStuffInLevel does for items taken on an earlier visit. */
    for (slot = 0; slot < 2; slot++) {
        int floor = ram[(LEVEL_LOADED + (uint32_t)slot) & 0xFFFFu];
        const uint8_t *m;
        uint32_t mask;
        if (!ram[(LEVEL_PLAYERS_FLAG + (uint32_t)slot) & 0xFFFFu] || floor >= FINAL_LEVEL) continue;
        m = ram + ((ITEMS_TAKEN + (uint32_t)floor * 4) & 0xFFFFu);
        mask = (uint32_t)m[0] << 24 | (uint32_t)m[1] << 16 | (uint32_t)m[2] << 8 | m[3];
        for (item = 0; item < 32; item++)
            if (mask & (0x80000000u >> item))
                ram[(LEVEL_ITEMS + (uint32_t)slot * 256 + (uint32_t)item * 8) & 0xFFFFu] = 0xFF;
    }
}

int save_write(int slot)
{
    FloorSnap *src = save_source();
    uint8_t *blob, *ram;
    char path[512], tmp[520];
    FILE *f;
    SaveInfo info;
    uint32_t i, sum = 0;
    Ser s;
    uint8_t hdr[64];
    int ok;

    if (!src || !save_possible()) return -1;
    blob = (uint8_t *)malloc(src->len);
    if (!blob) return -2;
    memcpy(blob, src->blob, src->len);
    ram = blob + genesis_snapshot_ram_offset();
    patch_progress(ram);
    info_from_ram(&info, ram);
    for (i = 0; i < src->len; i++) sum = sum * 31u + blob[i];

    s.buf = hdr; s.pos = 0; s.cap = sizeof hdr; s.err = 0;
    ser_bytes(&s, SAVE_MAGIC, 8);
    ser_u32(&s, SAVE_VERSION);
    ser_u8(&s, (uint32_t)info.fixed_world);
    ser_u8(&s, (uint32_t)info.players);
    ser_u8(&s, (uint32_t)info.floor[0]);
    ser_u8(&s, (uint32_t)info.floor[1]);
    ser_u8(&s, (uint32_t)info.ship_pieces);
    ser_u32(&s, (uint32_t)src->len);
    ser_u32(&s, sum);

    /* write to a temporary file first so a failed save never destroys the old one */
    slot_path(slot, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f) { free(blob); return -3; }
    ok = fwrite(hdr, 1, s.pos, f) == s.pos && fwrite(blob, 1, src->len, f) == src->len;
    ok = (fclose(f) == 0) && ok;
    free(blob);
    if (!ok) { remove(tmp); return -4; }
    remove(path);
    if (rename(tmp, path) != 0) { remove(tmp); return -5; }
    return 0;
}

int save_load(int slot)
{
    SaveInfo info;
    uint8_t *blob;
    size_t len;
    if (read_slot(slot, &info, &blob, &len) != 0) return -1;
    free(loaded_blob);
    loaded_blob = blob;
    loaded_len = len;
    post_load_pending = 1;
    genesis_restart_snapshot(blob, len);   /* copies it; continues in f_0014E6_InGameLoop */
    return -2;
}

void save_quit_to_menu(void)
{
    g_boot_to_menu = 1;
    genesis_restart_boot();
}
