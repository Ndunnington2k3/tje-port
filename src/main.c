#include "core/genesis.h"
#include "platform/platform.h"

#include <stdio.h>

int main(int argc, char **argv)
{
    const char *rom;
    int rc;

    if (plat_init(argc, argv) != 0)
        return 1;
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
    rc = genesis_run();
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
