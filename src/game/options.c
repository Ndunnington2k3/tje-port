#include "options.h"
#include "core/vdp.h"
#include "core/genesis.h"
#include "gameview.h"

#include <string.h>

GameOptions g_options = {
    1,  /* quick_start */
    1,  /* clean_transitions */
    1,  /* native_view */
    1,  /* extend_screens */
};

void options_from_args(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        /* --vanilla: behave exactly like the original game */
        if (!strcmp(argv[i], "--vanilla")) {
            g_options.quick_start = 0;
            g_options.clean_transitions = 0;
            g_options.native_view = 0;
            g_options.extend_screens = 0;
        }
        else if (!strcmp(argv[i], "--vdp-view")) g_options.native_view = 0;
    }
    options_apply();
}

void options_apply(void)
{
    vdp_set_clean_transitions(g_options.clean_transitions);
    genesis_set_line_hook(g_options.native_view ? gameview_line : NULL);
    genesis_set_picture_source(g_options.native_view ? gameview_frame : NULL);
}
