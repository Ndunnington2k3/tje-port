/* Sega Genesis VDP (video display processor) model and line renderer. */
#ifndef TJE_VDP_H
#define TJE_VDP_H

#include <stdint.h>

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

const uint32_t *vdp_framebuffer(void);   /* 0x00RRGGBB, VDP_FB_PITCH pixels per row */
int vdp_width(void);                     /* 256 or 320 */

/* Raw access for tools/debugging and future native renderers */
extern uint8_t vdp_vram[0x10000];
extern uint16_t vdp_cram[64];
extern uint16_t vdp_vsram[40];
extern uint8_t vdp_reg[32];

#endif
