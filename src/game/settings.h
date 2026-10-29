/*
 * The port's settings file (tje.cfg next to the program): port options and
 * control bindings, as "key = value" lines. Saved when the Options menu closes.
 */
#ifndef TJE_SETTINGS_H
#define TJE_SETTINGS_H

#include <stddef.h>

/* Full path of a file in the data folder (settings, saved games). The
 * TJE_SAVE_DIR environment variable overrides the folder. */
void data_path(const char *name, char *out, size_t size);

void settings_load(void);
int settings_save(void);     /* 0 = ok */

#endif
