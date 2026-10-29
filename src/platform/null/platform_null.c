/*
 * Headless platform for automated testing.
 *
 *   tje_headless <rom> [options]
 *     --frames N           stop after N frames (default 600)
 *     --shot F:path.ppm    save frame F as a PPM image (repeatable)
 *     --press F:N:buttons  hold buttons from frame F for N frames, pad 1
 *                          (buttons: any of U D L R A B C S)
 *     --random SEED        after frame 500, mash random buttons (Start only rarely)
 *     --dumpram F:path     save the 64 KB of work RAM at frame F (repeatable)
 *     --dumpvram F:path    save VRAM, CRAM and the VDP registers at frame F
 *     --wav path.wav       emulate sound and record it to a WAV file
 *     --audio              emulate sound (Z80, FM, PSG) without recording it
 *     --save F:slot        at frame F, save the game to slot 0-2 (port save feature)
 *     --load F:slot        at frame F, load slot 0-2
 *     --poke F:addr:val    at frame F, write byte val to address addr (hex), e.g. 100:FFE212:0
 *     --elevator N         every N frames, put ToeJam in the elevator (ELEVATOR_FLAG = 1)
 *                          (test hook: exercises level transitions)
 */
#include "platform/platform.h"
#include "core/genesis.h"
#include "core/m68k_rt.h"
#include "core/vdp.h"
#include "game/save.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_EVENTS 1024

static const char *rom_path = "tje.bin";
static unsigned max_frames = 600;
static unsigned frame;
static struct { unsigned frame; char path[256]; } shots[MAX_EVENTS];
static int nshots;
static struct { unsigned frame; char path[256]; } dumps[MAX_EVENTS];
static int ndumps;
static struct { unsigned frame; char path[256]; } vdumps[MAX_EVENTS];
static int nvdumps;
static struct { unsigned frame, len; uint16_t buttons; } presses[MAX_EVENTS];
static int npresses;
static unsigned random_seed;
static uint16_t random_buttons;
static unsigned random_left;
static unsigned elevator_period;
static struct { unsigned frame; int slot, load; } saves[MAX_EVENTS];
static int nsaves;
static struct { unsigned frame, addr, val; } pokes[MAX_EVENTS];
static int npokes;
static FILE *wav;
static int audio_flag;
static uint32_t wav_frames;


static uint16_t parse_buttons(const char *s)
{
    uint16_t b = 0;
    for (; *s; s++) {
        switch (*s) {
        case 'U': b |= PAD_UP; break;
        case 'D': b |= PAD_DOWN; break;
        case 'L': b |= PAD_LEFT; break;
        case 'R': b |= PAD_RIGHT; break;
        case 'A': b |= PAD_A; break;
        case 'B': b |= PAD_B; break;
        case 'C': b |= PAD_C; break;
        case 'S': b |= PAD_START; break;
        }
    }
    return b;
}

int plat_init(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc && nshots < MAX_EVENTS) {
            const char *a = argv[++i];
            const char *c = strchr(a, ':');
            if (!c) continue;
            shots[nshots].frame = (unsigned)atoi(a);
            snprintf(shots[nshots].path, sizeof shots[nshots].path, "%s", c + 1);
            nshots++;
        } else if (!strcmp(argv[i], "--press") && i + 1 < argc && npresses < MAX_EVENTS) {
            unsigned f = 0, n = 0;
            char btn[32] = {0};
            if (sscanf(argv[++i], "%u:%u:%31s", &f, &n, btn) == 3) {
                presses[npresses].frame = f;
                presses[npresses].len = n;
                presses[npresses].buttons = parse_buttons(btn);
                npresses++;
            }
        } else if (!strcmp(argv[i], "--dumpram") && i + 1 < argc && ndumps < MAX_EVENTS) {
            const char *a = argv[++i];
            const char *c = strchr(a, ':');
            if (!c) continue;
            dumps[ndumps].frame = (unsigned)atoi(a);
            snprintf(dumps[ndumps].path, sizeof dumps[ndumps].path, "%s", c + 1);
            ndumps++;
        } else if ((!strcmp(argv[i], "--save") || !strcmp(argv[i], "--load")) && i + 1 < argc && nsaves < MAX_EVENTS) {
            unsigned f = 0;
            int slot = 0;
            saves[nsaves].load = argv[i][2] == 'l';
            if (sscanf(argv[++i], "%u:%d", &f, &slot) == 2) {
                saves[nsaves].frame = f;
                saves[nsaves].slot = slot;
                nsaves++;
            }
        } else if (!strcmp(argv[i], "--poke") && i + 1 < argc && npokes < MAX_EVENTS) {
            if (sscanf(argv[++i], "%u:%x:%x", &pokes[npokes].frame, &pokes[npokes].addr, &pokes[npokes].val) == 3)
                npokes++;
        } else if (!strcmp(argv[i], "--dumpvram") && i + 1 < argc && nvdumps < MAX_EVENTS) {
            const char *a = argv[++i];
            const char *c = strchr(a, ':');
            if (!c) continue;
            vdumps[nvdumps].frame = (unsigned)atoi(a);
            snprintf(vdumps[nvdumps].path, sizeof vdumps[nvdumps].path, "%s", c + 1);
            nvdumps++;
        } else if (!strcmp(argv[i], "--audio")) {
            audio_flag = 1;
        } else if (!strcmp(argv[i], "--wav") && i + 1 < argc) {
            wav = fopen(argv[++i], "wb");
            if (wav) { static const uint8_t hdr[44] = {0}; fwrite(hdr, 1, 44, wav); }
        } else if (!strcmp(argv[i], "--elevator") && i + 1 < argc) {
            elevator_period = (unsigned)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--random") && i + 1 < argc) {
            random_seed = (unsigned)atoi(argv[++i]) * 2654435761u + 1;
        } else if (argv[i][0] != '-') rom_path = argv[i];
        else if (i + 1 < argc && argv[i + 1][0] != '-') i++;  /* skip value of unknown/overflowing option */
    }
    return 0;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

void plat_shutdown(void)
{
    if (wav) {
        uint8_t h[44];
        memcpy(h, "RIFF\0\0\0\0WAVEfmt ", 16);
        put32(h + 4, 36 + wav_frames * 4);
        put32(h + 16, 16);
        h[20] = 1; h[21] = 0; h[22] = 2; h[23] = 0;      /* PCM, stereo */
        put32(h + 24, 48000);
        put32(h + 28, 48000 * 4);
        h[32] = 4; h[33] = 0; h[34] = 16; h[35] = 0;
        memcpy(h + 36, "data", 4);
        put32(h + 40, wav_frames * 4);
        fseek(wav, 0, SEEK_SET);
        fwrite(h, 1, 44, wav);
        fclose(wav);
        wav = NULL;
    }
}

int plat_audio_enabled(void) { return wav != NULL || audio_flag; }

void plat_audio(const int16_t *samples, int frames)
{
    int i;
    if (!wav) return;
    for (i = 0; i < frames * 2; i++) {  /* little-endian int16 */
        fputc(samples[i] & 0xFF, wav);
        fputc((samples[i] >> 8) & 0xFF, wav);
    }
    wav_frames += (uint32_t)frames;
}

const char *plat_rom_path(void) { return rom_path; }

void plat_message(const char *title, const char *text)
{
    fprintf(stderr, "%s: %s\n", title, text);
}

static void save_ppm(const char *path, const uint32_t *px, int w, int h, int pitch)
{
    FILE *f = fopen(path, "wb");
    int x, y;
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            uint32_t c = px[y * pitch + x];
            fputc((c >> 16) & 0xFF, f);
            fputc((c >> 8) & 0xFF, f);
            fputc(c & 0xFF, f);
        }
    fclose(f);
}

void plat_present(const uint32_t *pixels, int width, int height, int pitch)
{
    int i;
    for (i = 0; i < nshots; i++)
        if (shots[i].frame == frame) save_ppm(shots[i].path, pixels, width, height, pitch);
    for (i = 0; i < ndumps; i++)
        if (dumps[i].frame == frame) {
            FILE *f = fopen(dumps[i].path, "wb");
            if (f) { fwrite(g_ram, 1, sizeof g_ram, f); fclose(f); }
        }
    for (i = 0; i < nvdumps; i++)
        if (vdumps[i].frame == frame) {
            /* VRAM (64 KB), then CRAM (64 words) and the VDP registers */
            FILE *f = fopen(vdumps[i].path, "wb");
            if (f) {
                fwrite(vdp_vram, 1, sizeof vdp_vram, f);
                fwrite(vdp_cram, 2, 64, f);
                fwrite(vdp_reg, 1, 32, f);
                fclose(f);
            }
        }
    frame++;
}

static unsigned rnd(void);
static uint16_t choices_p2(void);
static unsigned rnd(void)
{
    random_seed = random_seed * 1103515245u + 12345u;
    return (random_seed >> 16) & 0x7FFF;
}

int plat_poll(uint16_t pad[2])
{
    int i;
    pad[0] = pad[1] = 0;
    if (random_seed && frame >= (elevator_period ? 1000u : 500u)) {
        if (random_left == 0) {
            static const uint16_t choices[] = {PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT, PAD_A, PAD_B, PAD_C,
                                               PAD_UP | PAD_RIGHT, PAD_DOWN | PAD_LEFT, 0};
            random_buttons = choices[rnd() % 10];
            if (rnd() % 40 == 0) random_buttons = PAD_START;
            /* in elevator mode stick to walking so no menus are left open */
            if (elevator_period && frame > 1000) random_buttons &= PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT;
            random_left = 5 + rnd() % 120;
        }
        random_left--;
        pad[0] = random_buttons;
        pad[1] = (uint16_t)(rnd() % 8 == 0 ? choices_p2() : 0);
    }
    {
        /* report level changes ($FF912C = ToeJam's current level) */
        static uint32_t last_level;
        uint32_t lv = rd8(0xFF912C);
        if (lv != last_level) {
            fprintf(stderr, "frame %u: ToeJam on level %u\n", frame, lv);
            last_level = lv;
        }
    }
    if (getenv("TJE_TRACE_ELEV")) {
        /* floor/elevator/fall state each frame, for working out arrival timing */
        fprintf(stderr, "f%5u TJlv %2u Elv %2u cur %2u/%2u ELEVFLAG %u/%u fall %u/%u elev0 st %02X lv %u pl %u  elev1 st %02X\n",
                frame, rd8(0xFFA2A6), rd8(0xFFA326), rd8(0xFF912C), rd8(0xFF912E), rd8(0xFFDA6A), rd8(0xFFDA6B),
                rd8(0xFFDA22), rd8(0xFFDA23), rd8(0xFFDA4A + 5), rd8(0xFFDA4A + 4), rd8(0xFFDA4A + 6), rd8(0xFFDA4A + 0x15));
    }
    if (elevator_period && frame > 1000 && frame % elevator_period == 0) {
        /* ELEVATOR_FLAG ($FFDA6A, from the disassembly): 1 = ToeJam is in the elevator going up */
        if (rd8(0xFF912C) && rd8(0xFFDA6A) == 0) {
            wr8(0xFFDA6A, 1);
            if (getenv("TJE_DEBUG_ELEV")) fprintf(stderr, "frame %u: elevator flag set\n", frame);
        }
    }
    if (elevator_period && frame > 1000) {
        unsigned t = frame % elevator_period;
        pad[1] = 0;
        if (t < 150) pad[0] = 0;  /* hands off while the elevator rides */
    }
    for (i = 0; i < npokes; i++)
        if (pokes[i].frame == frame) wr8(pokes[i].addr, pokes[i].val);
    for (i = 0; i < nsaves; i++)
        if (saves[i].frame == frame) {
            if (saves[i].load) {
                fprintf(stderr, "frame %u: loading slot %d\n", frame, saves[i].slot);
                if (save_load(saves[i].slot) != 0) fprintf(stderr, "frame %u: load failed\n", frame);
            } else {
                int rc = save_write(saves[i].slot);
                fprintf(stderr, "frame %u: save to slot %d -> %d\n", frame, saves[i].slot, rc);
            }
        }
    for (i = 0; i < npresses; i++)
        if (frame >= presses[i].frame && frame < presses[i].frame + presses[i].len) pad[0] |= presses[i].buttons;
    return frame < max_frames;
}

static uint16_t choices_p2(void)
{
    return (uint16_t)(1u << (rnd() % 7));
}
