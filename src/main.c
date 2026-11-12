#include "core/genesis.h"
#include "platform/platform.h"
#include "core/m68k_rt.h"
#include "game/options.h"
#include "game/settings.h"
#include "game/menu.h"
#include "game/net.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* (tests: a file of this name next to the program) */
static int test_file(const char *name)
{
    char path[1100];
    FILE *f;
    snprintf(path, sizeof path, "%s%s", plat_data_dir(), name);
    if (!(f = fopen(path, "rb"))) return 0;
    fclose(f);
    return 1;
}

int main(int argc, char **argv)
{
    const char *rom;
    int rc;

    if (plat_init(argc, argv) != 0)
        return 1;
    settings_load();                 /* tje.cfg: options and controls */
    options_from_args(argc, argv);   /* the command line wins (e.g. --vanilla) */
    genesis_set_input_hook(game_input_hook);   /* Escape: back in menus, pause in play */
    net_install();                             /* network play */
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
    if (getenv("TJE_FUNK_END_TEST") || test_file("funkend.on")) {
        /* (tests: on Funkotron the two families - who end the game - stand halfway
         * along the path, on it, instead of at its far end. Their entries
         * in the final floor's table at $0B0100: x, y, message) */
        static const struct { uint32_t entry; uint16_t x, y; } fam[] = {
            {260, 0x0320, 0x02F0}, {262, 0x02E0, 0x02F8}, {273, 0x0360, 0x0300}, {275, 0x0360, 0x0300}};
        unsigned k;
        for (k = 0; k < sizeof fam / sizeof fam[0]; k++) {
            genesis_patch_rom16(0x0B0100u + fam[k].entry * 6, fam[k].x);
            genesis_patch_rom16(0x0B0100u + fam[k].entry * 6 + 2, fam[k].y);
        }
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
    if (plat_start_state() && genesis_load_state(plat_start_state()) != 0)
        plat_message("ToeJam & Earl", "Could not load the start state.");
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
