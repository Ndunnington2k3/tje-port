/*
 * SDL2 platform backend (Windows, Linux, macOS).
 *
 * Controls (player 1):  arrows = d-pad, Z = A, X = B, C = C, Enter = Start
 * Controls (player 2):  W/A/S/D = d-pad, J = A, K = B, L = C, Right Shift = Start
 * Game controllers are assigned to players 1 and 2 in the order they are connected.
 * F11 / Alt+Enter toggles fullscreen, Escape quits. --mute disables sound.
 */
#include "platform/platform.h"
#include "core/genesis.h"

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <stdio.h>
#include <string.h>

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static int tex_w;
static const char *rom_path = "tje.bin";
static SDL_GameController *pads[2];
static Uint64 next_frame_time;
static int vsync_ok;
static SDL_AudioDeviceID audio_dev;
static int audio_on = 1;

int plat_init(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--state")) i++;  /* handled in main.c */
        else if (!strcmp(argv[i], "--mute")) audio_on = 0;
        else if (argv[i][0] != '-') rom_path = argv[i];
    }

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    window = SDL_CreateWindow("ToeJam & Earl", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              320 * 3, 224 * 3, SDL_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, 0);
    if (!renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return 1;
    }
    {
        SDL_RendererInfo info;
        vsync_ok = SDL_GetRendererInfo(renderer, &info) == 0 && (info.flags & SDL_RENDERER_PRESENTVSYNC);
        /* only trust vsync for pacing on ~60 Hz displays */
        {
            SDL_DisplayMode dm;
            if (SDL_GetCurrentDisplayMode(0, &dm) == 0 && (dm.refresh_rate < 59 || dm.refresh_rate > 61))
                vsync_ok = 0;
        }
    }
    if (audio_on) {
        SDL_AudioSpec want, have;
        SDL_zero(want);
        want.freq = 48000;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 1024;
        audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (audio_dev) SDL_PauseAudioDevice(audio_dev, 0);
        else { fprintf(stderr, "No audio: %s\n", SDL_GetError()); audio_on = 0; }
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    SDL_RenderSetLogicalSize(renderer, 320, 224);
    next_frame_time = SDL_GetPerformanceCounter();
    return 0;
}

void plat_shutdown(void)
{
    int i;
    for (i = 0; i < 2; i++)
        if (pads[i]) SDL_GameControllerClose(pads[i]);
    if (audio_dev) SDL_CloseAudioDevice(audio_dev);
    if (texture) SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
}

const char *plat_rom_path(void) { return rom_path; }

int plat_audio_enabled(void) { return audio_on; }

void plat_audio(const int16_t *samples, int frames)
{
    Uint32 queued;
    if (!audio_dev) return;
    queued = SDL_GetQueuedAudioSize(audio_dev) / 4;
    /* keep latency bounded: the game runs at ~59.92 fps, displays may run a bit faster */
    if (queued > 48000 / 8) return;
    SDL_QueueAudio(audio_dev, samples, (Uint32)frames * 4);
}

void plat_message(const char *title, const char *text)
{
    fprintf(stderr, "%s: %s\n", title, text);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, text, window);
}

void plat_present(const uint32_t *pixels, int width, int height, int pitch)
{
    if (!texture || tex_w != width) {
        if (texture) SDL_DestroyTexture(texture);
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, width, height);
        tex_w = width;
        SDL_RenderSetLogicalSize(renderer, width == 256 ? 256 : 320, height);
    }
    SDL_UpdateTexture(texture, NULL, pixels, pitch * 4);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);

    if (!vsync_ok) {
        /* NTSC Genesis runs at ~59.92 fps */
        Uint64 freq = SDL_GetPerformanceFrequency();
        Uint64 frame = freq * 1001 / 60000;
        Uint64 now = SDL_GetPerformanceCounter();
        next_frame_time += frame;
        if (next_frame_time > now) {
            Uint64 ms = (next_frame_time - now) * 1000 / freq;
            if (ms > 1) SDL_Delay((Uint32)(ms - 1));
            while (SDL_GetPerformanceCounter() < next_frame_time) {}
        } else if (now - next_frame_time > frame * 4) {
            next_frame_time = now;  /* fell far behind: resync */
        }
    }
}

static void open_pads(void)
{
    int i, n = SDL_NumJoysticks();
    for (i = 0; i < n; i++) {
        int slot;
        if (!SDL_IsGameController(i)) continue;
        for (slot = 0; slot < 2; slot++) {
            if (pads[slot] && SDL_GameControllerGetAttached(pads[slot]) &&
                SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[slot])) == SDL_JoystickGetDeviceInstanceID(i))
                break;
        }
        if (slot < 2) continue;  /* already open */
        for (slot = 0; slot < 2; slot++) {
            if (!pads[slot] || !SDL_GameControllerGetAttached(pads[slot])) {
                if (pads[slot]) SDL_GameControllerClose(pads[slot]);
                pads[slot] = SDL_GameControllerOpen(i);
                break;
            }
        }
    }
}

static uint16_t read_controller(SDL_GameController *c)
{
    uint16_t b = 0;
    Sint16 x, y;
    if (!c) return 0;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP)) b |= PAD_UP;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) b |= PAD_DOWN;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) b |= PAD_LEFT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) b |= PAD_RIGHT;
    x = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    y = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
    if (x < -12000) b |= PAD_LEFT;
    if (x > 12000) b |= PAD_RIGHT;
    if (y < -12000) b |= PAD_UP;
    if (y > 12000) b |= PAD_DOWN;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_X)) b |= PAD_A;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A)) b |= PAD_B;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B)) b |= PAD_C;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START)) b |= PAD_START;
    return b;
}

int plat_poll(uint16_t pad[2])
{
    SDL_Event e;
    const Uint8 *k;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) return 0;
        if (e.type == SDL_KEYDOWN && !e.key.repeat) {
            if (e.key.keysym.sym == SDLK_ESCAPE) return 0;
            if (e.key.keysym.sym == SDLK_F11 ||
                (e.key.keysym.sym == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT))) {
                Uint32 fs = SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP;
                SDL_SetWindowFullscreen(window, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
            }
        }
        if (e.type == SDL_CONTROLLERDEVICEADDED || e.type == SDL_CONTROLLERDEVICEREMOVED) open_pads();
    }
    k = SDL_GetKeyboardState(NULL);
    pad[0] = read_controller(pads[0]);
    pad[1] = read_controller(pads[1]);
    if (k[SDL_SCANCODE_UP]) pad[0] |= PAD_UP;
    if (k[SDL_SCANCODE_DOWN]) pad[0] |= PAD_DOWN;
    if (k[SDL_SCANCODE_LEFT]) pad[0] |= PAD_LEFT;
    if (k[SDL_SCANCODE_RIGHT]) pad[0] |= PAD_RIGHT;
    if (k[SDL_SCANCODE_Z]) pad[0] |= PAD_A;
    if (k[SDL_SCANCODE_X]) pad[0] |= PAD_B;
    if (k[SDL_SCANCODE_C]) pad[0] |= PAD_C;
    if (k[SDL_SCANCODE_RETURN] && !(SDL_GetModState() & KMOD_ALT)) pad[0] |= PAD_START;
    if (k[SDL_SCANCODE_W]) pad[1] |= PAD_UP;
    if (k[SDL_SCANCODE_S]) pad[1] |= PAD_DOWN;
    if (k[SDL_SCANCODE_A]) pad[1] |= PAD_LEFT;
    if (k[SDL_SCANCODE_D]) pad[1] |= PAD_RIGHT;
    if (k[SDL_SCANCODE_J]) pad[1] |= PAD_A;
    if (k[SDL_SCANCODE_K]) pad[1] |= PAD_B;
    if (k[SDL_SCANCODE_L]) pad[1] |= PAD_C;
    if (k[SDL_SCANCODE_RSHIFT]) pad[1] |= PAD_START;
    return 1;
}
