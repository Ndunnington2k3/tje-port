/*
 * Genesis I/O space: VDP ports, controller ports, Z80 area (RAM, YM2612),
 * PSG and system registers.
 *
 * Sound hardware (Z80, YM2612, PSG) is handled in sound.c.
 */
#include "m68k_rt.h"
#include "genesis.h"
#include "vdp.h"
#include "sound.h"

static uint8_t pad_ctrl[3];     /* controller port control registers */
static uint8_t pad_data[3];     /* last value written to data ports (TH output) */

static uint32_t read_pad(int port)
{
    uint16_t b = port < 2 ? g_pad[port] : 0;
    uint32_t th = (pad_data[port] & pad_ctrl[port] & 0x40) ? 1 : 0;
    uint32_t v;
    if (!(pad_ctrl[port] & 0x40)) th = 1;  /* TH defaults high when not driven */
    if (th) {
        /* x1CBRLDU, active low */
        v = 0x40 | 0x3F;
        if (b & PAD_UP) v &= ~0x01u;
        if (b & PAD_DOWN) v &= ~0x02u;
        if (b & PAD_LEFT) v &= ~0x04u;
        if (b & PAD_RIGHT) v &= ~0x08u;
        if (b & PAD_B) v &= ~0x10u;
        if (b & PAD_C) v &= ~0x20u;
    } else {
        /* x0SA00DU */
        v = 0x33;
        if (b & PAD_UP) v &= ~0x01u;
        if (b & PAD_DOWN) v &= ~0x02u;
        if (b & PAD_A) v &= ~0x10u;
        if (b & PAD_START) v &= ~0x20u;
    }
    /* bits configured as outputs read back what was written */
    v = (v & ~pad_ctrl[port]) | (pad_data[port] & pad_ctrl[port]);
    return v & 0x7F;
}

uint32_t io_rd8(uint32_t a)
{
    if (a >= 0xA00000u && a < 0xA10000u) return snd_z80_area_read(a);
    if (a >= 0xA10000u && a < 0xA10020u) {
        switch (a & 0x1F) {
        case 0x01: return 0xA0;                   /* version: overseas, NTSC, no expansion */
        case 0x03: return read_pad(0);
        case 0x05: return read_pad(1);
        case 0x07: return read_pad(2);
        case 0x09: return pad_ctrl[0];
        case 0x0B: return pad_ctrl[1];
        case 0x0D: return pad_ctrl[2];
        default: return 0;
        }
    }
    if (a >= 0xA11100u && a < 0xA11200u) return (a & 1) ? 0x00 : snd_busreq_read();
    if (a >= 0xC00000u && a < 0xC00020u) {
        uint32_t w = io_rd16(a & ~1u);
        return (a & 1) ? (w & 0xFF) : (w >> 8);
    }
    return 0xFF;
}

uint32_t io_rd16(uint32_t a)
{
    if (a >= 0xC00000u && a < 0xC00020u) {
        switch (a & 0x1E) {
        case 0x00: case 0x02: return vdp_read_data();
        case 0x04: case 0x06: return vdp_read_status();
        case 0x08: case 0x0A: case 0x0C: case 0x0E: return vdp_read_hv();
        default: return 0xFFFF;
        }
    }
    if (a >= 0xA11100u && a < 0xA11200u) return snd_busreq_read() << 8;
    if (a >= 0xA00000u && a < 0xA10000u) {
        uint32_t b = io_rd8(a);
        return (b << 8) | b;
    }
    return (io_rd8(a) << 8) | io_rd8(a + 1);
}

void io_wr8(uint32_t a, uint32_t v)
{
    if (a >= 0xA00000u && a < 0xA10000u) { snd_z80_area_write(a, v); return; }
    if (a >= 0xA11100u && a < 0xA11200u) { if (!(a & 1)) snd_busreq_write(v << 8); return; }
    if (a >= 0xA11200u && a < 0xA11300u) { if (!(a & 1)) snd_reset_write(v << 8); return; }
    if (a >= 0xA10000u && a < 0xA10020u) {
        switch (a & 0x1F) {
        case 0x03: pad_data[0] = (uint8_t)v; break;
        case 0x05: pad_data[1] = (uint8_t)v; break;
        case 0x07: pad_data[2] = (uint8_t)v; break;
        case 0x09: pad_ctrl[0] = (uint8_t)v; break;
        case 0x0B: pad_ctrl[1] = (uint8_t)v; break;
        case 0x0D: pad_ctrl[2] = (uint8_t)v; break;
        default: break;
        }
        return;
    }
    if (a >= 0xC00000u && a < 0xC00020u) {
        if ((a & 0x1E) == 0x10 || (a & 0x1E) == 0x12) { if (a & 1) snd_psg_write(v); return; }
        io_wr16(a & ~1u, (v << 8) | v);
        return;
    }
    /* TMSS and anything else: ignored */
}

void io_wr16(uint32_t a, uint32_t v)
{
    if (a >= 0xC00000u && a < 0xC00020u) {
        switch (a & 0x1E) {
        case 0x00: case 0x02: vdp_write_data(v); return;
        case 0x04: case 0x06: vdp_write_control(v); return;
        case 0x10: case 0x12: snd_psg_write(v & 0xFF); return;  /* PSG (word write: low byte) */
        default: return;
        }
    }
    if (a >= 0xA00000u && a < 0xA10000u) {
        snd_z80_area_write(a, v >> 8);
        return;
    }
    if (a >= 0xA11100u && a < 0xA11200u) { snd_busreq_write(v); return; }
    if (a >= 0xA11200u && a < 0xA11300u) { snd_reset_write(v); return; }
    if (a >= 0xA10000u && a < 0xA10020u) {
        io_wr8(a | 1, v & 0xFF);
        return;
    }
}
