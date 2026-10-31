/*
 * Diagnostics for the wider gameplay view (gameview.c): finds objects and sprite
 * pixels that drop out for one frame, and keeps the last frames so they can be
 * saved when something looks wrong (PC: --view-log, F12).
 */
#ifndef TJE_VIEWDIAG_H
#define TJE_VIEWDIAG_H

#include <stdint.h>

/* Start: findings go to log_path; captures are written as capture_prefix
 * + "N_frame.bmp" / ".ram" / ".vram". */
void viewdiag_open(const char *log_path, const char *capture_prefix);
int viewdiag_active(void);

/* Each frame shown (the pixels handed to the screen). */
void viewdiag_frame(const uint32_t *pixels, int width, int height, int pitch);

/* Save the frames kept (the last few seconds' worth, up to VIEWDIAG_KEEP). */
void viewdiag_capture(const char *why);

#endif
