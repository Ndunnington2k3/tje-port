#include "core/genesis.h"
#include "platform/platform.h"
#include "core/m68k_rt.h"
#include "game/options.h"
#include "game/settings.h"
#include "game/menu.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    const char *rom;
    int rc;

    if (plat_init(argc, argv) != 0)
        return 1;
    settings_load();                 /* tje.cfg: options and controls */
    options_from_args(argc, argv);   /* the command line wins (e.g. --vanilla) */
    genesis_set_input_hook(game_input_hook);   /* Escape: back in menus, pause in play */
    rom = plat_rom_path();
    if (genesis_load_rom(rom) != 0) {
        char msg[600];
        snprintf(msg, sizeof msg,
                 "Could not load the ToeJam & Earl ROM:\n%s\n\n"
                 "Put your ROM next to the program as 'tje.bin', or pass its path on the command line.",
                 rom);
        plat_message("ToeJam & Earl", msg);
        plat_shutdown();
        return 1;
    }
    {
        /* --state <file.gsx>: start from a Gens save state (testing / level select) */
        int i;
        for (i = 1; i + 1 < argc; i++)
            if (!strcmp(argv[i], "--state")) {
                if (genesis_load_state(argv[i + 1]) != 0) {
                    plat_message("ToeJam & Earl", "Could not load the save state given with --state.");
                    plat_shutdown();
                    return 1;
                }
            }
    }
    rc = genesis_run();
    rt_coverage_write(getenv("TJE_COVERAGE_OUT"));
    plat_shutdown();
    return rc;
}

#ifdef _WIN32
/* Windows GUI-subsystem entry point (no console window). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLine, int show)
{
    (void)hInst; (void)hPrev; (void)cmdLine; (void)show;
    return main(__argc, __argv);
}
#endif
