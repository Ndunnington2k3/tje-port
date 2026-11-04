#include "options.h"
#include "core/vdp.h"
#include "core/genesis.h"
#include "gameview.h"
#include "core/sound.h"

#include "core/music.h"
#include "platform/platform.h"
#include <stdio.h>
#include <string.h>

GameOptions g_options = {
    1,  /* quick_start */
    1,  /* clean_transitions */
    1,  /* native_view */
    1,  /* extend_screens */
#ifdef __PSP__
    FM_SOUND_RECORDED,   /* fm_sound */
#else
    FM_SOUND_ACCURATE,   /* fm_sound */
#endif
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
        /* --fm accurate | light | light2 | light3 | recorded: the FM chip emulation,
         * or the recorded music (sound.h) */
        else if (!strcmp(argv[i], "--fm") && i + 1 < argc) {
            const char *f = argv[++i];
            g_options.fm_sound = !strcmp(f, "recorded") ? FM_SOUND_RECORDED
                               : !strncmp(f, "light", 5) ? (f[5] >= '1' && f[5] <= '3' ? f[5] - '0' : 1) : FM_SOUND_ACCURATE;
        }
    }
    options_apply();
}

void options_apply(void)
{
    {
        char d[300];
        snprintf(d, sizeof d, "%smusic/", plat_data_dir());
        music_set_dir(d);
    }
    if (g_options.fm_sound == FM_SOUND_RECORDED) snd_set_fm(SND_FM_NATIVE, 3);
    else snd_set_fm(g_options.fm_sound ? SND_FM_LIGHT : SND_FM_ACCURATE, g_options.fm_sound ? g_options.fm_sound : 1);
    vdp_set_clean_transitions(g_options.clean_transitions);
    genesis_set_line_hook(g_options.native_view ? gameview_line : NULL);
    genesis_set_picture_source(g_options.native_view ? gameview_frame : NULL);
    vdp_set_sat_dma_hook(g_options.native_view ? gameview_sat_dma : NULL);
    if (g_options.native_view) gameview_install_line_filter();
    else vdp_line_wanted = NULL;
}
