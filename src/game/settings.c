#include "settings.h"
#include "options.h"
#include "platform/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SETTINGS_FILE "tje.cfg"

static const char *const button_names[8] = {"up", "down", "left", "right", "presents", "map", "action", "pause"};
static const char *const slot_names[2] = {"key", "pad"};

/* port options kept in the file */
static struct { const char *name; int *value; } const option_list[] = {
    {"quick_start", &g_options.quick_start},
    {"clean_transitions", &g_options.clean_transitions},
    {"native_view", &g_options.native_view},
    {"extend_screens", &g_options.extend_screens},
};

void data_path(const char *name, char *out, size_t size)
{
    const char *dir = getenv("TJE_SAVE_DIR");
    if (dir && *dir) snprintf(out, size, "%s/%s", dir, name);
    else snprintf(out, size, "%s%s", plat_data_dir(), name);
}

static char *trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

void settings_load(void)
{
    char path[1024], line[256];
    FILE *f;
    data_path(SETTINGS_FILE, path, sizeof path);
    f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *eq, *key, *val;
        unsigned i;
        if (line[0] == '#' || !(eq = strchr(line, '='))) continue;
        *eq = 0;
        key = trim(line);
        val = trim(eq + 1);
        for (i = 0; i < sizeof option_list / sizeof option_list[0]; i++)
            if (!strcmp(key, option_list[i].name)) *option_list[i].value = atoi(val) != 0;
        /* bindings: p1.start.key = Return */
        if ((key[0] == 'p') && (key[1] == '1' || key[1] == '2') && key[2] == '.') {
            int p = key[1] - '1', b, s;
            for (b = 0; b < 8; b++)
                for (s = 0; s < 2; s++) {
                    char want[32];
                    snprintf(want, sizeof want, "%s.%s", button_names[b], slot_names[s]);
                    if (!strcmp(key + 3, want)) plat_set_binding(p, b, s, val);
                }
        }
    }
    fclose(f);
}

int settings_save(void)
{
    char path[1024];
    FILE *f;
    unsigned i;
    int p, b, s;
    data_path(SETTINGS_FILE, path, sizeof path);
    f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "# ToeJam & Earl port settings (also changed from Options on the Who screen)\n");
    for (i = 0; i < sizeof option_list / sizeof option_list[0]; i++)
        fprintf(f, "%s = %d\n", option_list[i].name, *option_list[i].value);
    for (p = 0; p < 2; p++)
        for (b = 0; b < 8; b++)
            for (s = 0; s < 2; s++)
                fprintf(f, "p%d.%s.%s = %s\n", p + 1, button_names[b], slot_names[s], plat_binding(p, b, s));
    return fclose(f) == 0 ? 0 : -1;
}
