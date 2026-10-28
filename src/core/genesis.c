#include "genesis.h"
#include "m68k_rt.h"
#include "vdp.h"
#include "platform/platform.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint16_t g_pad[2];
uint32_t g_frame;

static uint8_t *rom_data;
static jmp_buf quit_jmp;
static int quit_code;
static int line;           /* current scanline 0..261 */
static int hint_counter;
static int vint_pending, hint_pending;
static int in_events;
static uint8_t *pending_state;   /* Gens save state to start from, if any */

void genesis_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[tje] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

void genesis_fatal(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    genesis_log("FATAL: %s", buf);
    genesis_log("D0-7 %08X %08X %08X %08X %08X %08X %08X %08X",
                R.d[0], R.d[1], R.d[2], R.d[3], R.d[4], R.d[5], R.d[6], R.d[7]);
    genesis_log("A0-7 %08X %08X %08X %08X %08X %08X %08X %08X",
                R.a[0], R.a[1], R.a[2], R.a[3], R.a[4], R.a[5], R.a[6], R.a[7]);
    plat_message("ToeJam & Earl - error", buf);
    quit_code = 2;
    longjmp(quit_jmp, 1);
}

int genesis_load_rom(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0x200 || n > 0x400000) { fclose(f); return -2; }
    rom_data = (uint8_t *)malloc((size_t)n);
    if (!rom_data || fread(rom_data, 1, (size_t)n, f) != (size_t)n) { fclose(f); return -3; }
    fclose(f);
    /* Accept interleaved .smd? Not needed for now: expect a plain .bin */
    g_rom = rom_data;
    g_rom_size = (uint32_t)n;
    return 0;
}

static void raise_interrupt(int level, uint32_t vector)
{
    uint32_t sp0;
    push32(0);                      /* return PC (unused by the C translation) */
    push16(R.sr);                   /* CCR of the interrupted code stays in its locals */
    R.sr = (R.sr & ~0x0700u) | ((uint32_t)level << 8) | 0x2000u;
    sp0 = R.a[7];
    (void)sp0;
    rt_call(vector);
}

void genesis_check_interrupts(void)
{
    uint32_t mask = (R.sr >> 8) & 7;
    if (vint_pending && mask < 6 && vdp_vint_enabled()) {
        vint_pending = 0;
        vdp_ack_vint();
        raise_interrupt(6, rt_vector_vint);
        return;
    }
    if (hint_pending && mask < 4 && vdp_hint_enabled()) {
        hint_pending = 0;
        raise_interrupt(4, rt_vector_hint);
    }
}

static void end_of_frame(void)
{
    g_frame++;
    plat_present(vdp_framebuffer(), vdp_width(), VDP_HEIGHT, VDP_FB_PITCH);
    if (!plat_poll(g_pad)) {
        quit_code = 0;
        longjmp(quit_jmp, 1);
    }
}

void genesis_run_events(void)
{
    if (in_events)
        return;
    while ((int32_t)(R.cycles - R.next_event) >= 0) {
        in_events = 1;
        R.next_event += GEN_CYCLES_PER_LINE;
        if (line < VDP_HEIGHT) {
            vdp_render_line(line);
            /* horizontal interrupt counter runs during active display */
            if (--hint_counter < 0) {
                hint_counter = vdp_hint_reload();
                hint_pending = 1;
            }
        } else {
            hint_counter = vdp_hint_reload();
        }
        line++;
        if (line == VDP_HEIGHT) {
            vdp_set_vblank(1);
            vint_pending = 1;
            end_of_frame();
        } else if (line == GEN_LINES_NTSC) {
            line = 0;
            vdp_set_vblank(0);
        }
        vdp_set_line(line);
        in_events = 0;
        genesis_check_interrupts();
    }
}

int genesis_load_state(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0x22478) { fclose(f); return -2; }
    free(pending_state);
    pending_state = (uint8_t *)malloc((size_t)n);
    if (!pending_state || fread(pending_state, 1, (size_t)n, f) != (size_t)n) { fclose(f); return -3; }
    fclose(f);
    if (memcmp(pending_state, "GST", 3) != 0) { free(pending_state); pending_state = NULL; return -4; }
    return 0;
}

static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

/* Gens state layout: 68k regs at $80, VDP regs $FA, CRAM $112, VSRAM $192,
 * Z80 RAM $474, 68k RAM $2478 and VRAM $12478 (both big-endian byte order). */
static void apply_state(const uint8_t *st)
{
    int i;
    for (i = 0; i < 24; i++) vdp_reg[i] = st[0xFA + i];
    for (i = 0; i < 64; i++) vdp_cram[i] = (uint16_t)((st[0x112 + i * 2] | (st[0x113 + i * 2] << 8)) & 0x0EEE);
    for (i = 0; i < 40; i++) vdp_vsram[i] = (uint16_t)((st[0x192 + i * 2] | (st[0x193 + i * 2] << 8)) & 0x07FF);
    vdp_refresh_palette();
    memcpy(g_ram, st + 0x2478, 0x10000);
    memcpy(vdp_vram, st + 0x12478, 0x10000);
    for (i = 0; i < 8; i++) {
        R.d[i] = le32(st + 0x80 + i * 4);
        R.a[i] = le32(st + 0xA0 + i * 4);
    }
}

int genesis_run(void)
{
    memset(&R, 0, sizeof R);
    memset(g_ram, 0, sizeof g_ram);
    vdp_reset();
    line = 0;
    hint_counter = 0;
    vint_pending = hint_pending = 0;
    in_events = 0;
    R.a[7] = rt_reset_sp;
    R.sr = 0x2700;
    R.cycles = 0;
    R.next_event = GEN_CYCLES_PER_LINE;
    quit_code = 0;
    if (setjmp(quit_jmp) == 0) {
        if (pending_state && rt_entry_ingameloop) {
            apply_state(pending_state);
            /* Enter the main game loop at its own stack depth (see Misc/Code/InGameLoop.X68). */
            R.a[7] = 0xFF7FE4;
            R.sr = 0x2000;
            rt_call(rt_entry_ingameloop);
            genesis_log("main loop returned");
            return 1;
        }
        rt_call(rt_reset_pc);
        genesis_log("reset routine returned");
        return 1;
    }
    return quit_code;
}
