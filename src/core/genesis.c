#include "genesis.h"
#include "m68k_rt.h"
#include "vdp.h"
#include "sound.h"
#include "ser.h"
#include "platform/platform.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint16_t g_pad[2];
uint16_t g_menu_pad;
uint32_t g_frame;

static uint8_t *rom_data;
#define PORT_ROM_SIZE 0x10000u
static uint32_t port_rom_base, port_rom_used;
static jmp_buf quit_jmp;
static int quit_code;
static int line;           /* current scanline 0..261 */
static int hint_counter;
static int vint_pending, hint_pending;
static int in_events;
static void (*line0_hook)(void);
static void (*input_hook)(uint16_t pad[2]);

void genesis_set_input_hook(void (*fn)(uint16_t pad[2])) { input_hook = fn; }

void genesis_set_frame_hook(void (*fn)(void)) { line0_hook = fn; }
static uint8_t *pending_state;   /* Gens save state to start from, if any */

/* Restarts requested from inside the running game (see genesis_restart_*). */
enum { RESTART_NONE, RESTART_BOOT, RESTART_SNAPSHOT };
static int restart_kind;
static uint8_t *restart_blob;
static size_t restart_len;

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
    /* room after the ROM for data the port adds (genesis_port_rom) */
    port_rom_base = ((uint32_t)n + 0xFFFFu) & ~0xFFFFu;
    port_rom_used = 0;
    rom_data = (uint8_t *)malloc(port_rom_base + PORT_ROM_SIZE);
    if (!rom_data) { fclose(f); return -3; }
    memset(rom_data, 0xFF, port_rom_base + PORT_ROM_SIZE);
    if (fread(rom_data, 1, (size_t)n, f) != (size_t)n) { fclose(f); return -3; }
    fclose(f);
    /* Accept interleaved .smd? Not needed for now: expect a plain .bin */
    g_rom = rom_data;
    g_rom_size = port_rom_base + PORT_ROM_SIZE;
    return 0;
}

uint32_t genesis_port_rom(const void *data, uint32_t size)
{
    uint32_t a;
    port_rom_used = (port_rom_used + 1u) & ~1u;          /* keep words aligned */
    if (port_rom_used + size > PORT_ROM_SIZE) return 0;
    a = port_rom_base + port_rom_used;
    memcpy(rom_data + a, data, size);
    port_rom_used += size;
    return a;
}

uint8_t *genesis_port_rom_ptr(uint32_t addr)
{
    if (addr < port_rom_base || addr >= port_rom_base + PORT_ROM_SIZE) return NULL;
    return rom_data + addr;
}

void genesis_port_rom_reset(void)
{
    port_rom_used = 0;
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
    const int16_t *audio;
    int n = snd_end_frame(&audio);
    if (n > 0) plat_audio(audio, n);
    g_frame++;
    plat_present(vdp_framebuffer(), vdp_width(), VDP_HEIGHT, VDP_FB_PITCH);
    if (!plat_poll(g_pad)) {
        quit_code = 0;
        longjmp(quit_jmp, 1);
    }
    g_menu_pad = plat_menu_input();
    if (input_hook) input_hook(g_pad);
}

void genesis_run_events(void)
{
    if (in_events)
        return;
    while ((int32_t)(R.cycles - R.next_event) >= 0) {
        in_events = 1;
        R.next_event += GEN_CYCLES_PER_LINE;
        snd_line(line);
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
            snd_vint();
            end_of_frame();
        } else if (line == GEN_LINES_NTSC) {
            line = 0;
            vdp_set_vblank(0);
            if (line0_hook) line0_hook();
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

/* Gens state layout: 68k regs at $80, VDP regs $FA, CRAM $112, VSRAM $192, YM2612 regs $1E4,
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
    snd_load_state(st + 0x474, st + 0x1E4);
}

/* ---- machine snapshots ---------------------------------------------------- */
#define SNAP_MAGIC   0x50414E53u   /* "SNAP" */
#define SNAP_VERSION 1u

static void machine_save(Ser *s)
{
    int i;
    ser_u32(s, SNAP_MAGIC);
    ser_u32(s, SNAP_VERSION);
    for (i = 0; i < 8; i++) ser_u32(s, R.d[i]);
    for (i = 0; i < 8; i++) ser_u32(s, R.a[i]);
    ser_u32(s, R.usp);
    ser_u32(s, R.sr);
    ser_u32(s, R.next_event - R.cycles);
    ser_u16(s, (uint32_t)line);
    ser_u16(s, (uint32_t)hint_counter);
    ser_u8(s, (uint32_t)vint_pending);
    ser_u8(s, (uint32_t)hint_pending);
    ser_bytes(s, g_ram, sizeof g_ram);
    vdp_save(s);
    snd_save(s);
}

size_t genesis_snapshot(uint8_t *buf, size_t cap)
{
    Ser s = {NULL, 0, 0, 0};
    machine_save(&s);
    if (!buf) return s.pos;
    if (cap < s.pos) return 0;
    s.buf = buf; s.pos = 0; s.cap = cap; s.err = 0;
    machine_save(&s);
    return s.err ? 0 : s.pos;
}

static int machine_load(const uint8_t *blob, size_t len)
{
    Ser s = {(uint8_t *)blob, 0, len, 0};
    int i;
    if (des_u32(&s) != SNAP_MAGIC || des_u32(&s) != SNAP_VERSION) return -1;
    for (i = 0; i < 8; i++) R.d[i] = des_u32(&s);
    for (i = 0; i < 8; i++) R.a[i] = des_u32(&s);
    R.usp = des_u32(&s);
    R.sr = des_u32(&s);
    R.next_event = R.cycles + des_u32(&s);
    line = (int)des_u16(&s);
    hint_counter = (int16_t)des_u16(&s);
    vint_pending = (int)des_u8(&s);
    hint_pending = (int)des_u8(&s);
    des_bytes(&s, g_ram, sizeof g_ram);
    vdp_load(&s);
    snd_load(&s);
    return s.err ? -1 : 0;
}

size_t genesis_snapshot_ram_offset(void)
{
    return 4 + 4 + 8 * 4 + 8 * 4 + 4 + 4 + 4 + 2 + 2 + 1 + 1;
}

int genesis_snapshot_valid(const uint8_t *blob, size_t len)
{
    Ser s = {(uint8_t *)blob, 0, len, 0};
    return des_u32(&s) == SNAP_MAGIC && des_u32(&s) == SNAP_VERSION && len == genesis_snapshot(NULL, 0);
}

void genesis_quit(void)
{
    restart_kind = RESTART_NONE;
    quit_code = 0;
    longjmp(quit_jmp, 1);
}

void genesis_restart_boot(void)
{
    restart_kind = RESTART_BOOT;
    longjmp(quit_jmp, 1);
}

void genesis_restart_snapshot(const uint8_t *blob, size_t len)
{
    uint8_t *copy = (uint8_t *)malloc(len);
    if (!copy) return;
    memcpy(copy, blob, len);
    free(restart_blob);
    restart_blob = copy;
    restart_len = len;
    restart_kind = RESTART_SNAPSHOT;
    longjmp(quit_jmp, 1);
}

static void machine_reset(void)
{
    line0_hook = NULL;
    memset(&R, 0, sizeof R);
    memset(g_ram, 0, sizeof g_ram);
    vdp_reset();
    snd_init(plat_audio_enabled());
    line = 0;
    hint_counter = 0;
    vint_pending = hint_pending = 0;
    in_events = 0;
    R.a[7] = rt_reset_sp;
    R.sr = 0x2700;
    R.cycles = 0;
    R.next_event = GEN_CYCLES_PER_LINE;
}

int genesis_run(void)
{
    for (;;) {
        int kind;
        machine_reset();
        quit_code = 0;
        kind = restart_kind;
        restart_kind = RESTART_NONE;
        if (setjmp(quit_jmp) == 0) {
            if (kind == RESTART_SNAPSHOT && machine_load(restart_blob, restart_len) == 0) {
                /* the snapshot was taken at the top of the main game loop */
                rt_call(rt_entry_ingameloop);
                genesis_log("main loop returned");
                return 1;
            }
            if (kind == RESTART_NONE && pending_state && rt_entry_ingameloop) {
                apply_state(pending_state);
                free(pending_state);
                pending_state = NULL;
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
        if (restart_kind == RESTART_NONE)
            return quit_code;
        /* else: a restart was requested; go round again */
    }
}
