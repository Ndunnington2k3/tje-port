/* Port options: behaviour that differs from the original game, and how it is configured. */
#ifndef TJE_OPTIONS_H
#define TJE_OPTIONS_H

typedef struct {
    /* Start on the SEGA screen goes straight to the Who menu (original: no effect
     * until the title screen). */
    int quick_start;
    /* No half-built screens during scene changes (the original shows a frame or
     * two of garbage, most visibly when the elevator takes off). */
    int clean_transitions;
} GameOptions;

extern GameOptions g_options;

/* Parse port options from the command line (e.g. --vanilla). */
void options_from_args(int argc, char **argv);

#endif
