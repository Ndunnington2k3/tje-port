#include "options.h"

#include <string.h>

GameOptions g_options = {
    1,  /* quick_start */
};

void options_from_args(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        /* --vanilla: behave exactly like the original game */
        if (!strcmp(argv[i], "--vanilla")) {
            g_options.quick_start = 0;
        }
    }
}
