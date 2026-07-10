/* Plume — a Steam Remote Play client built on IHSlib.
 *
 *   plume                 discover a host, request a stream, and play it
 *   plume --pair          pair with a host (enter the shown PIN in Steam)
 *   plume --host <ip>     target a specific host instead of the first found
 *   plume --no-audio      video only
 *   plume --verbose       show IHSlib/decoder chatter, not just problems
 *
 * Linux amd64 + arm64: nothing here is arch-specific; FFmpeg software decode
 * works on both, and picks hardware/threaded paths where available.
 */
#define _DEFAULT_SOURCE  /* gethostname, mkdir */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <signal.h>
#include <execinfo.h>

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include "ihslib.h"
#include "ihslib/client.h"
#include "ihslib/hid/sdl.h"
#include "ihs.h"
#include "media.h"
#include "ui.h"

/* Shared SDL objects, created once and lent to the menu and the decoder. */
static SDL_Window *g_window;
static SDL_Renderer *g_renderer;

/* -------- user settings, from settings.conf then argv then the menu -------- */
static bool g_hevc = false; /* --hevc: request HEVC (HW decode on Pi5) */
static int g_width = 1920, g_height = 1080, g_fps = 60, g_kbps = 15000; /* host encode cap */
static bool g_audio = true;
static bool g_desktop = true; /* stream the host's desktop, else its game session */
static int g_scale = MEDIA_SCALE_FIT; /* fit / stretch / crop on aspect mismatch */

/* ----------------------------- logging ------------------------------------ */
/* Print a stack trace on a fatal signal. The target has no debugger, and a bare
 * "Segmentation fault" says nothing about which thread or which call. */
static void OnFatalSignal(int sig) {
    void *frames[32];
    int n = backtrace(frames, 32);
    fprintf(stderr, "\n*** fatal signal %d (%s), %d frames:\n", sig, strsignal(sig), n);
    fflush(stderr);
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
    signal(sig, SIG_DFL);
    raise(sig); /* let it dump a core if the system is configured for it */
}

/* ------------------------------ pairing ----------------------------------- */
static int DoPair(const IHS_HostInfo *host) {
    /* PIN the user must type into Steam on the host to approve this device. */
    char pin[5];
    PlumeMakePin(pin);
    printf("\n  Enter this PIN in Steam on the host to approve:  %s\n\n", pin);

    PlumePairing p;
    if (!PlumePairStart(&p, host, pin)) return 1;
    while (!p.done) SDL_Delay(100); /* nothing to draw here; just wait it out */
    return PlumePairFinish(&p) ? 0 : 1;
}

/* ------------------------------- session ---------------------------------- */
static volatile bool g_running = true;

static void OnDisconnected(IHS_Session *s, void *ctx) { (void)s;(void)ctx; g_running = false; }

/* settings.conf: plain "key value" lines, so it can be inspected and hand-edited.
 * Unknown keys are ignored, so a newer file stays readable by an older build. */
static void LoadSettings(void) {
    char path[512];
    PlumeDataPath(path, sizeof(path), "settings.conf");
    FILE *f = fopen(path, "r");
    if (!f) {
        /* Silently running on defaults after the user picked 480p in the menu is
         * the kind of thing you chase for an hour. $HOME decides where this lives. */
        if (PlumeVerbose) fprintf(stderr, "settings: %s absent, using defaults\n", path);
        return;
    }
    char key[64];
    int value;
    while (fscanf(f, "%63s %d", key, &value) == 2) {
        if (!strcmp(key, "width")) g_width = value;
        else if (!strcmp(key, "height")) g_height = value;
        else if (!strcmp(key, "fps")) g_fps = value;
        else if (!strcmp(key, "kbps")) g_kbps = value;
        else if (!strcmp(key, "hevc")) g_hevc = value != 0;
        else if (!strcmp(key, "audio")) g_audio = value != 0;
        else if (!strcmp(key, "desktop")) g_desktop = value != 0;
        else if (!strcmp(key, "scaling")) g_scale = value;
        /* superseded by "scaling"; still honoured so old files keep working */
        else if (!strcmp(key, "stretch")) g_scale = value ? MEDIA_SCALE_STRETCH : MEDIA_SCALE_FIT;
    }
    fclose(f);
    /* A truncated or hand-mangled file must not wedge the negotiation. */
    if (g_width <= 0 || g_height <= 0) { g_width = 1920; g_height = 1080; }
    if (g_fps <= 0) g_fps = 60;
    if (g_kbps <= 0) g_kbps = 15000;
    if (g_scale < MEDIA_SCALE_FIT || g_scale > MEDIA_SCALE_CROP) g_scale = MEDIA_SCALE_FIT;
    if (PlumeVerbose)
        fprintf(stderr, "settings: %s -> %dx%d @ %d fps, %d kbps, scaling %d, hevc %d, audio %d, desktop %d\n",
                path, g_width, g_height, g_fps, g_kbps, g_scale, g_hevc, g_audio, g_desktop);
}

static void SaveSettings(void) {
    char path[512];
    PlumeDataPath(path, sizeof(path), "settings.conf");
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "width %d\nheight %d\nfps %d\nkbps %d\nhevc %d\naudio %d\ndesktop %d\nscaling %d\n",
            g_width, g_height, g_fps, g_kbps, g_hevc, g_audio, g_desktop, g_scale);
    fclose(f);
}

static void OnConfiguring(IHS_Session *s, IHS_SessionConfig *cfg, void *ctx) {
    (void) s; (void) ctx;
    cfg->enableAudio = g_audio;
    cfg->enableHevc = g_hevc; /* default H264: widest decode support across arches */
    cfg->maxWidth = g_width;
    cfg->maxHeight = g_height;
    cfg->maxFps = g_fps;
    cfg->maxBitrateKbps = g_kbps;
}

/* SDL mouse button -> IHS button. */
static IHS_StreamInputMouseButton MapButton(Uint8 b) {
    switch (b) {
        case SDL_BUTTON_LEFT: return IHS_MOUSE_BUTTON_LEFT;
        case SDL_BUTTON_RIGHT: return IHS_MOUSE_BUTTON_RIGHT;
        case SDL_BUTTON_MIDDLE: return IHS_MOUSE_BUTTON_MIDDLE;
        case SDL_BUTTON_X1: return IHS_MOUSE_BUTTON_X1;
        default: return IHS_MOUSE_BUTTON_X2;
    }
}

/* Returns true when the gamepad's HID report changed and needs flushing. */
static bool ForwardInput(IHS_Session *s, const SDL_Event *e) {
    int w, h;
    switch (e->type) {
        case SDL_EVENT_MOUSE_MOTION:
            SDL_GetWindowSize(g_window, &w, &h);
            IHS_SessionSendMousePosition(s, e->motion.x / w, e->motion.y / h);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            IHS_SessionSendMouseDown(s, MapButton(e->button.button));
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            IHS_SessionSendMouseUp(s, MapButton(e->button.button));
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            IHS_SessionSendMouseWheel(s, e->wheel.y >= 0 ? IHS_MOUSE_WHEEL_UP : IHS_MOUSE_WHEEL_DOWN);
            break;
        case SDL_EVENT_KEY_DOWN:
            /* SDL scancodes are USB HID usages, which is what IHSlib expects. */
            IHS_SessionSendKeyDown(s, e->key.scancode);
            break;
        case SDL_EVENT_KEY_UP:
            IHS_SessionSendKeyUp(s, e->key.scancode);
            break;
        default:
            /* Gamepads travel over the HID channel; this only updates the report,
             * the caller flushes it once per frame. */
            return IHS_HIDHandleSDLEvent(s, e);
    }
    return false;
}

/* Hotkey + Start leaves the stream. Hotkey is Guide (Home) or Back (Select) —
 * 8BitDo pads label these differently, and some have no Guide at all. Polled
 * rather than event-driven: we need both buttons held at the same instant, and
 * the presses are also forwarded to the host as normal HID input. */
static bool QuitComboHeld(void) {
    int n;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    if (!ids) return false;
    bool held = false;
    for (int i = 0; i < n && !held; i++) {
        SDL_Gamepad *g = SDL_GetGamepadFromID(ids[i]); /* opened by the HID provider */
        if (!g || !SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_START)) continue;
        held = SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_GUIDE) ||
               SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_BACK);
    }
    SDL_free(ids);
    return held;
}

static int DoStream(const IHS_HostInfo *host, bool audio) {
    IHS_SessionInfo sinfo;
    IHS_StreamingResult res;
    if (!PlumeRequestStream(host, g_width, g_height, g_desktop, &sinfo, &res)) {
        /* Not paired yet -> pair from the UI, then retry once. */
        if (res != IHS_StreamingUnauthorized) return 1;
        if (!PairScreen(g_renderer, &PlumeClientConfig, host)) return 1;
        if (!PlumeRequestStream(host, g_width, g_height, g_desktop, &sinfo, &res)) return 1;
    }

    MediaAttach(g_window, g_renderer, audio, (MediaScale) g_scale);

    g_running = true; /* reset: OnDisconnected clears it */
    IHS_StreamSessionCallbacks scb = {.configuring = OnConfiguring, .disconnected = OnDisconnected};
    IHS_Session *session = IHS_SessionCreate(&PlumeClientConfig, &sinfo);
    IHS_SessionSetLogFunction(session, PlumeLog);
    IHS_SessionSetSessionCallbacks(session, &scb, NULL);
    IHS_SessionSetVideoCallbacks(session, &VideoCallbacks, NULL);
    IHS_SessionSetAudioCallbacks(session, &AudioCallbacks, NULL);

    /* Owns the gamepads for the duration of the stream: it opens them itself, so
     * the launcher must have closed its own handles before we get here. */
    IHS_HIDProvider *hid = IHS_HIDProviderSDLCreateManaged();
    IHS_SessionHIDAddProvider(session, hid);

    if (!IHS_SessionConnect(session)) {
        fprintf(stderr, "Failed to connect session\n");
        IHS_SessionDestroy(session);
        IHS_HIDProviderSDLDestroy(hid);
        MediaDetach();
        return 1;
    }

    bool disconnecting = false;
    while (g_running) {
        bool hidDirty = false;
        bool padButton = false;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT ||
                (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE)) {
                if (!disconnecting) { IHS_SessionDisconnect(session); disconnecting = true; }
            } else if (!disconnecting) {
                /* Mid-stream (dis)appearance is the signature of a controller or USB
                 * hiccup — the one thing a "controls went dead" report can't show. */
                if (e.type == SDL_EVENT_GAMEPAD_REMOVED || e.type == SDL_EVENT_GAMEPAD_ADDED)
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "gamepad %s mid-stream (id=%d)",
                                e.type == SDL_EVENT_GAMEPAD_REMOVED ? "removed" : "added",
                                (int) e.gdevice.which);
                padButton |= e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
                hidDirty |= ForwardInput(session, &e);
            }
        }
        /* The combo can only become held on a button-down; skip the poll (and its
         * SDL_GetGamepads allocation) on the other ~59 frames each second. */
        if (!disconnecting && padButton && QuitComboHeld()) {
            IHS_SessionDisconnect(session);
            disconnecting = true;
        }
        /* One HID packet per frame at most, carrying the coalesced state. */
        if (hidDirty && !disconnecting) {
            IHS_SessionHIDSendReport(session);
        }
        MediaPresent(); /* blocks on vsync, paces the loop */
    }

    IHS_SessionThreadedJoin(session);
    IHS_SessionDestroy(session); /* closes its devices, so destroy the provider after */
    IHS_HIDProviderSDLDestroy(hid);
    MediaDetach();
    return 0;
}

/* -------------------------------- main ------------------------------------ */
int main(int argc, char *argv[]) {
    bool pair = false;
    const char *hostIp = NULL;
    /* --verbose is read first: LoadSettings already logs. */
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--verbose")) PlumeVerbose = true;
    SDL_SetLogPriorities(PlumeVerbose ? SDL_LOG_PRIORITY_INFO : SDL_LOG_PRIORITY_WARN);
    LoadSettings(); /* before argv, so flags win over the saved file */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--pair")) pair = true;
        else if (!strcmp(argv[i], "--no-audio")) g_audio = false;
        else if (!strcmp(argv[i], "--no-desktop")) g_desktop = false;
        else if (!strcmp(argv[i], "--hevc")) g_hevc = true;
        else if (!strcmp(argv[i], "--verbose")) ; /* handled above */
        else if (!strcmp(argv[i], "--host") && i + 1 < argc) hostIp = argv[++i];
        else {
            fprintf(stderr, "usage: %s [--pair] [--host <ip>] [--no-audio] [--no-desktop] [--hevc] [--verbose]\n", argv[0]);
            return 2;
        }
    }

    signal(SIGSEGV, OnFatalSignal);
    signal(SIGABRT, OnFatalSignal);
    signal(SIGBUS, OnFatalSignal);

    IHS_Init();
    PlumeInitCreds();

    /* --pair keeps the headless flow: discover one host, pair, exit. */
    if (pair) {
        IHS_HostInfo host;
        printf("Discovering hosts%s...\n", hostIp ? " (filtered)" : ""); /* --pair is interactive: always shown */
        int rc = 1;
        if (PlumeDiscoverHost(hostIp, 10, &host)) rc = DoPair(&host);
        else fprintf(stderr, "No host found. Is Steam running on the LAN with Remote Play enabled?\n");
        IHS_Quit();
        return rc;
    }

    /* Launcher UI. */
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD) || !TTF_Init()) {
        fprintf(stderr, "SDL/TTF init failed: %s\n", SDL_GetError());
        IHS_Quit();
        return 1;
    }
    g_window = SDL_CreateWindow("Plume", 1280, 720,
                                SDL_WINDOW_RESIZABLE | SDL_WINDOW_FULLSCREEN);
    g_renderer = SDL_CreateRenderer(g_window, NULL);
    if (!g_window || !g_renderer) {
        fprintf(stderr, "Window/renderer unavailable: %s\n", SDL_GetError());
        IHS_Quit();
        return 1;
    }
    SDL_SetRenderVSync(g_renderer, 1);

    if (!UIEnsureFonts(g_renderer)) {
        fprintf(stderr, "Font unavailable (set PLUME_FONT to a .ttf)\n");
        IHS_Quit();
        return 1;
    }

    for (;;) {
        UIResult r = {.hevc = g_hevc, .audio = g_audio, .desktop = g_desktop, .scale = g_scale,
                      .width = g_width, .height = g_height, .fps = g_fps, .kbps = g_kbps};
        UIAction act = RunMenu(g_window, g_renderer, &PlumeClientConfig, &r);
        g_hevc = r.hevc; g_audio = r.audio; g_desktop = r.desktop; g_scale = r.scale;
        g_width = r.width; g_height = r.height; g_fps = r.fps; g_kbps = r.kbps;
        SaveSettings();
        if (act != UI_START) break;
        DoStream(&r.host, r.audio);
    }

    UICloseFonts();
    SDL_DestroyRenderer(g_renderer);
    SDL_DestroyWindow(g_window);
    TTF_Quit();
    SDL_Quit();
    IHS_Quit();
    return 0;
}
