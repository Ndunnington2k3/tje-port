/* Sega Genesis VDP (video display processor) model and line renderer. */
#ifndef TJE_VDP_H
#define TJE_VDP_H

#include <stdint.h>
#include "ser.h"

#define VDP_MAX_WIDTH 320
#define VDP_HEIGHT    224
#define VDP_FB_PITCH  VDP_MAX_WIDTH   /* in pixels */

void vdp_reset(void);
void vdp_refresh_palette(void);  /* after writing vdp_cram directly */

uint32_t vdp_read_data(void);
void vdp_write_data(uint32_t v);
uint32_t vdp_read_status(void);
void vdp_write_control(uint32_t v);
uint32_t vdp_read_hv(void);

void vdp_render_line(int line);
void vdp_set_line(int line);
void vdp_set_vblank(int on);
void vdp_ack_vint(void);
int vdp_vint_enabled(void);
int vdp_hint_enabled(void);
int vdp_hint_reload(void);

/* Machine snapshots: all VDP memory and internal state. */
void vdp_save(Ser *s);
void vdp_load(Ser *s);

/* Clean scene transitions (port option, see vdp.c). vdp_freeze(-1) keeps showing
 * the current picture while game code rebuilds the screen; vdp_freeze(n) then
 * lets n more frames pass frozen before drawing resumes. */
void vdp_set_clean_transitions(int on);
void vdp_freeze(int frames);

/* Port overlays: a picture of colour numbers (0 = clear) shown at x, y over
 * everything the VDP draws, with its own 16 Genesis colours. Slots 0-7. The
 * pixels are not copied: keep them alive while shown. Cleared on reset. */
void vdp_overlay(int slot, int x, int y, int w, int h, const uint8_t *pixels, const uint16_t colors[16]);
void vdp_overlay_off(int slot);

/* Since the last call: VRAM words written to [other, plane A, plane B, sprite table],
 * and lines drawn with the display off. Resets the counters. */
void vdp_frame_stats(uint32_t out[5]);

const uint32_t *vdp_framebuffer(void);   /* 0x00RRGGBB, VDP_FB_PITCH pixels per row */
int vdp_width(void);                     /* 256 or 320 */

/* Is the picture of this frame a live one (display on, not frozen, not hidden by
 * clean transitions)? For renderers that draw the screen themselves. */
int vdp_live_picture(void);
/* CRAM entry i (0-63) as 0x00RRGGBB */
uint32_t vdp_palette_rgb(int i);

/* Raw access for tools/debugging and future native renderers */
extern uint8_t vdp_vram[0x10000];
extern uint16_t vdp_cram[64];
extern uint16_t vdp_vsram[40];
extern uint8_t vdp_reg[32];

#endif
