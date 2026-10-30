/*
 * SDL2 platform backend (Windows, Linux, macOS).
 *
 * Default game controls (player 1): arrows, Z = Action, X = Present List, C = Map, Enter = Pause
 * Default game controls (player 2): W/A/S/D, J = Action, K = Present List, L = Map, Right Shift = Pause
 * Controllers (players 1 and 2 in the order connected): A = Action, B = Present List, X = Map,
 * Start = Pause. All of these can be rebound (Who screen -> Options -> Controls). Menus use fixed
 * controls (plat_menu_input): Enter/A choose, Escape/B go back.
 * --input-log writes what the keyboard and controllers send to tje_input.log.
 * F11 / Alt+Enter toggles fullscreen. Escape (or a controller's Back button) is
 * PAD_BACK: "back" on the port's menus, Start (pause) during play. --mute disables sound.
 */
#include "platform/platform.h"
#include "core/genesis.h"
#include "gfx/gfx_gl.h"
#include "gfx/present.h"
#include "gfx/view.h"
#include "gfx_sdlrender.h"

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SDL_Window *window;
static SDL_Renderer *renderer;      /* only when OpenGL is not available */
static SDL_GLContext gl_context;
static const GfxBackend *gfx;
static const char *rom_path = "tje.bin";
static SDL_GameController *pads[2];
static Uint64 next_frame_time;
static int vsync_ok;
static SDL_AudioDeviceID audio_dev;
static int audio_on = 1;
static FILE *input_log;     /* --input-log: what the keyboard and controllers send */

/* The first window: the largest whole-number zoom of 320x224 that fits in
 * about 80% of the screen. */
static void first_window_size(int *w, int *h)
{
    SDL_Rect r;
    int s = 3;
    if (SDL_GetDisplayUsableBounds(0, &r) == 0) {
        int sx = r.w * 4 / 5 / VIEW_BASE_W, sy = r.h * 4 / 5 / VIEW_BASE_H;
        s = sx < sy ? sx : sy;
        if (s < 1) s = 1;
    }
    *w = VIEW_BASE_W * s;
    *h = VIEW_BASE_H * s;
}

static void *gl_proc(const char *name) { return SDL_GL_GetProcAddress(name); }

static void target_size(int *w, int *h)
{
    if (gl_context) SDL_GL_GetDrawableSize(window, w, h);
    else SDL_GetRendererOutputSize(renderer, w, h);
}

static void draw_and_show(void)
{
    int w, h;
    target_size(&w, &h);
    present_draw(w, h);
    if (gl_context) SDL_GL_SwapWindow(window);
    else SDL_RenderPresent(renderer);
}

/* Windows stops the program while a window edge is being dragged, but SDL still
 * passes on the size changes here: draw the last frame again at the new size, so
 * the picture follows the mouse. */
static int SDLCALL resize_watch(void *data, SDL_Event *e)
{
    (void)data;
    if (e->type == SDL_WINDOWEVENT && SDL_GetWindowFromID(e->window.windowID) == window &&
        (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e->window.event == SDL_WINDOWEVENT_EXPOSED))
        draw_and_show();
    return 0;
}

/* OpenGL 2.1 if possible, else SDL's own renderer (Direct3D etc.). */
static int open_window(void)
{
    int w, h, vsync = 0;
    first_window_size(&w, &h);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    window = SDL_CreateWindow("ToeJam & Earl", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h,
                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI);
    if (window && !getenv("TJE_NO_GL")) {
        gl_context = SDL_GL_CreateContext(window);
        if (gl_context) {
            gfx = gfx_gl_init(gl_proc);
            if (!gfx) { SDL_GL_DeleteContext(gl_context); gl_context = NULL; }
            else vsync = SDL_GL_SetSwapInterval(1) == 0;
        }
    }
    if (!gfx) {
        if (window) SDL_DestroyWindow(window);
        window = SDL_CreateWindow("ToeJam & Earl", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h,
                                  SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
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
            vsync = SDL_GetRendererInfo(renderer, &info) == 0 && (info.flags & SDL_RENDERER_PRESENTVSYNC);
        }
        gfx = gfx_sdlrender_init(renderer);
    }
    present_init(gfx);
    fprintf(stderr, "Drawing with %s\n", gfx->name);
    SDL_SetWindowMinimumSize(window, 160, 112);
    /* only trust vsync for pacing on ~60 Hz displays */
    vsync_ok = vsync;
    {
        SDL_DisplayMode dm;
        if (SDL_GetCurrentDisplayMode(0, &dm) == 0 && (dm.refresh_rate < 59 || dm.refresh_rate > 61))
            vsync_ok = 0;
    }
    SDL_AddEventWatch(resize_watch, NULL);
    return 0;
}

int plat_init(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--state")) i++;  /* handled in main.c */
        else if (!strcmp(argv[i], "--mute")) audio_on = 0;
        else if (!strcmp(argv[i], "--input-log")) {
            char path[1100];
            snprintf(path, sizeof path, "%stje_input.log", plat_data_dir());
            input_log = fopen(path, "w");
        }
        else if (argv[i][0] != '-') rom_path = argv[i];
    }

    plat_default_bindings(0);
    plat_default_bindings(1);
    /* Read Steam Controllers directly (SDL's HIDAPI driver; off by default in
     * SDL2). Note: while Steam is running it usually takes the controller over and,
     * for programs it does not know, turns it into keyboard and mouse ("Desktop
     * configuration"). */
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_STEAM, "1");
    /* Real pixels on high-DPI screens (Windows would otherwise blur the picture) */
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    if (open_window() != 0) return 1;
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
    next_frame_time = SDL_GetPerformanceCounter();
    return 0;
}

void plat_shutdown(void)
{
    int i;
    for (i = 0; i < 2; i++)
        if (pads[i]) SDL_GameControllerClose(pads[i]);
    if (audio_dev) SDL_CloseAudioDevice(audio_dev);
    SDL_DelEventWatch(resize_watch, NULL);
    present_shutdown();
    if (gl_context) SDL_GL_DeleteContext(gl_context);
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
    present_set_frame(pixels, width, height, pitch);
    draw_and_show();

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

/* ---- input bindings ------------------------------------------------------- */

#define NBUTTONS 8
static SDL_Scancode bind_key[2][NBUTTONS];
static char bind_pad[2][NBUTTONS][24];       /* "a", "dpup", "+lefttrigger", "-leftx"... */

/* Actions in bit order: Up, Down, Left, Right, Present List, Map, Action, Pause */
static const SDL_Scancode default_keys[2][NBUTTONS] = {
    {SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
     SDL_SCANCODE_X, SDL_SCANCODE_C, SDL_SCANCODE_Z, SDL_SCANCODE_RETURN},
    {SDL_SCANCODE_W, SDL_SCANCODE_S, SDL_SCANCODE_A, SDL_SCANCODE_D,
     SDL_SCANCODE_K, SDL_SCANCODE_L, SDL_SCANCODE_J, SDL_SCANCODE_RSHIFT},
};
/* controller: A = Action, B = Present List, X = Map, Start = Pause */
static const char *const default_pad[NBUTTONS] = {"dpup", "dpdown", "dpleft", "dpright", "b", "x", "a", "start"};

void plat_default_bindings(int player)
{
    int b;
    for (b = 0; b < NBUTTONS; b++) {
        bind_key[player][b] = default_keys[player][b];
        snprintf(bind_pad[player][b], sizeof bind_pad[player][b], "%s", default_pad[b]);
    }
}

const char *plat_binding(int player, int button, int slot)
{
    if (player < 0 || player > 1 || button < 0 || button >= NBUTTONS) return "";
    if (slot == PLAT_BIND_KEY)
        return bind_key[player][button] == SDL_SCANCODE_UNKNOWN ? "" : SDL_GetScancodeName(bind_key[player][button]);
    return bind_pad[player][button];
}

static int pad_token_valid(const char *t)
{
    if (!*t) return 1;
    if (t[0] == '+' || t[0] == '-') return SDL_GameControllerGetAxisFromString(t + 1) != SDL_CONTROLLER_AXIS_INVALID;
    return SDL_GameControllerGetButtonFromString(t) != SDL_CONTROLLER_BUTTON_INVALID;
}

int plat_set_binding(int player, int button, int slot, const char *token)
{
    if (player < 0 || player > 1 || button < 0 || button >= NBUTTONS) return -1;
    if (slot == PLAT_BIND_KEY) {
        SDL_Scancode sc = *token ? SDL_GetScancodeFromName(token) : SDL_SCANCODE_UNKNOWN;
        if (*token && sc == SDL_SCANCODE_UNKNOWN) return -1;
        bind_key[player][button] = sc;
        return 0;
    }
    if (!pad_token_valid(token) || strlen(token) >= sizeof bind_pad[0][0]) return -1;
    snprintf(bind_pad[player][button], sizeof bind_pad[player][button], "%s", token);
    return 0;
}

void plat_binding_label(int slot, const char *token, char *out, int size)
{
    static const struct { const char *tok, *label; } names[] = {
        {"a", "A"}, {"b", "B"}, {"x", "X"}, {"y", "Y"}, {"back", "Back"}, {"guide", "Guide"},
        {"start", "Start"}, {"leftstick", "L Stick"}, {"rightstick", "R Stick"},
        {"leftshoulder", "LB"}, {"rightshoulder", "RB"}, {"dpup", "Up"}, {"dpdown", "Down"},
        {"dpleft", "Left"}, {"dpright", "Right"}, {"+lefttrigger", "LT"}, {"+righttrigger", "RT"},
        {"-leftx", "Stick Left"}, {"+leftx", "Stick Right"}, {"-lefty", "Stick Up"}, {"+lefty", "Stick Down"},
        {"-rightx", "R Stick Left"}, {"+rightx", "R Stick Right"}, {"-righty", "R Stick Up"}, {"+righty", "R Stick Down"},
    };
    unsigned i;
    if (!*token) { snprintf(out, (size_t)size, "--"); return; }
    if (slot == PLAT_BIND_KEY) { snprintf(out, (size_t)size, "%s", token); return; }
    for (i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcmp(names[i].tok, token)) { snprintf(out, (size_t)size, "Pad %s", names[i].label); return; }
    snprintf(out, (size_t)size, "Pad %s", token);
}

/* capture */
static int capturing, capture_player, capture_done, capture_slot;
static char capture_token[24];

static int axis_rested[SDL_CONTROLLER_AXIS_MAX];

void plat_capture_start(int player)
{
    int a;
    for (a = 0; a < SDL_CONTROLLER_AXIS_MAX; a++) axis_rested[a] = 0;
    if (pads[player])
        for (a = 0; a < SDL_CONTROLLER_AXIS_MAX; a++) {
            Sint16 v = SDL_GameControllerGetAxis(pads[player], (SDL_GameControllerAxis)a);
            axis_rested[a] = v > -8000 && v < 8000;
        }
    capturing = 1;
    capture_player = player;
    capture_done = 0;
    if (input_log) { fprintf(input_log, "[capture start, player %d]\n", player + 1); fflush(input_log); }
}

void plat_capture_cancel(void) { capturing = 0; capture_done = 0; }

int plat_capture_poll(int *slot, char *token, int size)
{
    if (!capture_done) return 0;
    capture_done = 0;
    capturing = 0;
    if (input_log) {
        fprintf(input_log, "[capture done: %s]\n", capture_slot < 0 ? "cancelled" : capture_token);
        fflush(input_log);
    }
    if (capture_slot < 0) return -1;
    *slot = capture_slot;
    snprintf(token, (size_t)size, "%s", capture_token);
    return 1;
}

static int pad_player(SDL_JoystickID id)
{
    int p;
    for (p = 0; p < 2; p++)
        if (pads[p] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[p])) == id) return p;
    return -1;
}

static void capture_event(const SDL_Event *e)
{
    if (capture_done) return;
    if (e->type == SDL_KEYDOWN && !e->key.repeat) {
        capture_done = 1;
        if (e->key.keysym.scancode == SDL_SCANCODE_ESCAPE) { capture_slot = -1; return; }
        capture_slot = PLAT_BIND_KEY;
        snprintf(capture_token, sizeof capture_token, "%s", SDL_GetScancodeName(e->key.keysym.scancode));
    } else if (e->type == SDL_CONTROLLERBUTTONDOWN && pad_player(e->cbutton.which) == capture_player) {
        const char *t = SDL_GameControllerGetStringForButton((SDL_GameControllerButton)e->cbutton.button);
        if (!t) return;
        capture_done = 1;
        capture_slot = PLAT_BIND_PAD;
        snprintf(capture_token, sizeof capture_token, "%s", t);
    } else if (e->type == SDL_CONTROLLERAXISMOTION && pad_player(e->caxis.which) == capture_player) {
        /* only a stick or trigger pushed well out of its resting range counts: an
         * axis that rests at an extreme (some triggers report -32768) or drifts
         * does not, and it must have been back near rest since capture began */
        int axis = e->caxis.axis, v = e->caxis.value;
        int trigger = axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
        const char *t;
        if (axis < 0 || axis >= SDL_CONTROLLER_AXIS_MAX) return;
        if (v > -8000 && v < 8000) { axis_rested[axis] = 1; return; }
        if (!axis_rested[axis] || (v > -24000 && v < 24000) || (trigger && v < 0)) return;
        t = SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)axis);
        if (!t) return;
        capture_done = 1;
        capture_slot = PLAT_BIND_PAD;
        snprintf(capture_token, sizeof capture_token, "%c%s", e->caxis.value > 0 ? '+' : '-', t);
    }
}

static int pad_input_down(SDL_GameController *c, const char *t)
{
    if (!c || !*t) return 0;
    if (t[0] == '+' || t[0] == '-') {
        SDL_GameControllerAxis ax = SDL_GameControllerGetAxisFromString(t + 1);
        Sint16 v;
        if (ax == SDL_CONTROLLER_AXIS_INVALID) return 0;
        v = SDL_GameControllerGetAxis(c, ax);
        return t[0] == '+' ? v > 12000 : v < -12000;
    } else {
        SDL_GameControllerButton b = SDL_GameControllerGetButtonFromString(t);
        return b != SDL_CONTROLLER_BUTTON_INVALID && SDL_GameControllerGetButton(c, b);
    }
}

static uint16_t read_player(int p, const Uint8 *k)
{
    uint16_t bits = 0;
    int b;
    SDL_GameController *c = pads[p];
    for (b = 0; b < NBUTTONS; b++) {
        int down = bind_key[p][b] != SDL_SCANCODE_UNKNOWN && k[bind_key[p][b]];
        if (b == 7 && down && bind_key[p][b] == SDL_SCANCODE_RETURN && (SDL_GetModState() & KMOD_ALT))
            down = 0;   /* Alt+Enter is the fullscreen toggle */
        if (down || pad_input_down(c, bind_pad[p][b])) bits |= (uint16_t)(1u << b);
    }
    if (p == 0 && k[SDL_SCANCODE_ESCAPE]) bits |= PAD_BACK;
    if (c && SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK)) bits |= PAD_BACK;
    if (c) {
        /* the left stick always works as a d-pad too */
        Sint16 x = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
        Sint16 y = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
        if (x < -12000) bits |= PAD_LEFT;
        if (x > 12000) bits |= PAD_RIGHT;
        if (y < -12000) bits |= PAD_UP;
        if (y > 12000) bits |= PAD_DOWN;
    }
    return bits;
}

static uint16_t menu_input_now(void);

uint16_t plat_menu_input(void)
{
    static uint16_t last;
    uint16_t m = menu_input_now();
    if (input_log && m != last) {
        fprintf(input_log, "[menu input:%s%s%s%s%s%s]\n", m & PAD_UP ? " up" : "", m & PAD_DOWN ? " down" : "",
                m & PAD_LEFT ? " left" : "", m & PAD_RIGHT ? " right" : "", m & MENU_OK ? " choose" : "",
                m & MENU_BACK ? " back" : "");
        fflush(input_log);
    }
    last = m;
    return m;
}

static uint16_t menu_input_now(void)
{
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    uint16_t m = 0;
    int p;
    if (capturing) return 0;
    if (k[SDL_SCANCODE_UP]) m |= PAD_UP;
    if (k[SDL_SCANCODE_DOWN]) m |= PAD_DOWN;
    if (k[SDL_SCANCODE_LEFT]) m |= PAD_LEFT;
    if (k[SDL_SCANCODE_RIGHT]) m |= PAD_RIGHT;
    if ((k[SDL_SCANCODE_RETURN] && !(SDL_GetModState() & KMOD_ALT)) || k[SDL_SCANCODE_SPACE] ||
        k[SDL_SCANCODE_KP_ENTER])
        m |= MENU_OK;
    if (k[SDL_SCANCODE_ESCAPE] || k[SDL_SCANCODE_BACKSPACE]) m |= MENU_BACK;
    for (p = 0; p < 2; p++) {
        SDL_GameController *c = pads[p];
        Sint16 x, y;
        if (!c) continue;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP)) m |= PAD_UP;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) m |= PAD_DOWN;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) m |= PAD_LEFT;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) m |= PAD_RIGHT;
        x = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
        y = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
        if (x < -16000) m |= PAD_LEFT;
        if (x > 16000) m |= PAD_RIGHT;
        if (y < -16000) m |= PAD_UP;
        if (y > 16000) m |= PAD_DOWN;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A) ||
            SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START)) m |= MENU_OK;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B) ||
            SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK)) m |= MENU_BACK;
    }
    return m;
}

const char *plat_data_dir(void)
{
    static char dir[1024];
    static int done;
    if (!done) {
        char *base = SDL_GetBasePath();
        done = 1;
        if (base) { snprintf(dir, sizeof dir, "%s", base); SDL_free(base); }
    }
    return dir;
}

static void log_event(const SDL_Event *e)
{
    switch (e->type) {
    case SDL_KEYDOWN:
        if (!e->key.repeat) fprintf(input_log, "key down: %s\n", SDL_GetScancodeName(e->key.keysym.scancode));
        break;
    case SDL_KEYUP:
        fprintf(input_log, "key up: %s\n", SDL_GetScancodeName(e->key.keysym.scancode));
        break;
    case SDL_CONTROLLERBUTTONUP:
        fprintf(input_log, "controller %d button up: %s\n", pad_player(e->cbutton.which) + 1,
                SDL_GameControllerGetStringForButton((SDL_GameControllerButton)e->cbutton.button));
        break;
    case SDL_CONTROLLERDEVICEADDED: {
        SDL_GameController *c = SDL_GameControllerFromInstanceID(e->cdevice.which);
        fprintf(input_log, "controller added: %s\n",
                SDL_GameControllerNameForIndex(e->cdevice.which) ? SDL_GameControllerNameForIndex(e->cdevice.which) : "?");
        (void)c;
        break;
    }
    case SDL_CONTROLLERBUTTONDOWN:
        fprintf(input_log, "controller %d button: %s\n", pad_player(e->cbutton.which) + 1,
                SDL_GameControllerGetStringForButton((SDL_GameControllerButton)e->cbutton.button));
        break;
    case SDL_CONTROLLERAXISMOTION:
        if (e->caxis.value > 24000 || e->caxis.value < -24000)
            fprintf(input_log, "controller %d axis: %s %d\n", pad_player(e->caxis.which) + 1,
                    SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)e->caxis.axis), e->caxis.value);
        break;
    case SDL_MOUSEBUTTONDOWN:
        fprintf(input_log, "mouse button %d\n", e->button.button);
        break;
    case SDL_JOYDEVICEADDED:
        fprintf(input_log, "joystick added: %s (game controller: %s)\n",
                SDL_JoystickNameForIndex(e->jdevice.which) ? SDL_JoystickNameForIndex(e->jdevice.which) : "?",
                SDL_IsGameController(e->jdevice.which) ? "yes" : "no");
        break;
    }
    fflush(input_log);
}

int plat_poll(uint16_t pad[2])
{
    SDL_Event e;
    const Uint8 *k;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) return 0;
        if (input_log) log_event(&e);
        if (capturing) { capture_event(&e); if (e.type == SDL_KEYDOWN) continue; }
        if (e.type == SDL_KEYDOWN && !e.key.repeat) {
            if (e.key.keysym.sym == SDLK_F11 ||
                (e.key.keysym.sym == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT))) {
                Uint32 fs = SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP;
                SDL_SetWindowFullscreen(window, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
            }
        }
        if (e.type == SDL_CONTROLLERDEVICEADDED || e.type == SDL_CONTROLLERDEVICEREMOVED) open_pads();
    }
    if (capturing) { pad[0] = pad[1] = 0; return 1; }
    k = SDL_GetKeyboardState(NULL);
    pad[0] = read_player(0, k);
    pad[1] = read_player(1, k);
    return 1;
}
