/* Plume — a Steam Remote Play client built on IHSlib.
 *
 *   plume                 open the launcher: pick a host, stream it
 *   plume --host <ip>     stream that host directly, exit when the stream ends
 *   plume --pair [--host <ip>]   pair headlessly; stdout: "PIN <4 digits>"
 *   plume --discover      list hosts, one "<ip>\t<1|?>\t<hostname>" line each
 *   plume --timeout <s>   discovery timeout (3 for --discover, else 10)
 *   plume --width/--height/--fps/--kbps <n>   override settings.conf for this run
 *   plume --no-audio      video only
 *   plume --verbose       show IHSlib/decoder chatter, not just problems
 *
 * Linux amd64 + arm64: nothing here is arch-specific; FFmpeg software decode
 * works on both, and picks hardware/threaded paths where available.
 *
 * A frontend drives the headless modes, so their stdout and exit codes are a
 * contract: stdout carries only the lines above (everything else is stderr), and
 * 0 = ok, 1 = failure, 2 = usage, 3 = host not found, 4 = not paired / pairing
 * refused by the host.
 */
#define _DEFAULT_SOURCE  /* gethostname, mkdir */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>

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

/* ------------------------------ pairing ----------------------------------- */
enum { EXIT_NO_HOST = 3, EXIT_NOT_PAIRED = 4 };

static volatile sig_atomic_t g_term;
static void (*g_prevTerm)(int); /* SDL's handler: it queues SDL_EVENT_QUIT */
/* alarm: SIGALRM's default action kills us if the clean exit is itself stuck,
 * so SIGTERM ends plume within 5 s whatever state it is in. */
static void OnTerm(int sig) { g_term = 1; alarm(5); if (g_prevTerm) g_prevTerm(sig); }
static void InstallTerm(void) {
    void (*old)(int) = signal(SIGTERM, OnTerm);
    if (old != SIG_DFL && old != SIG_IGN && old != SIG_ERR && old != OnTerm) g_prevTerm = old;
    signal(SIGINT, OnTerm);
}

static int DoPair(const IHS_HostInfo *host) {
    /* PIN the user must type into Steam on the host to approve this device. */
    char pin[5];
    PlumeMakePin(pin);

    /* The frontend kills us when its viewer backs out of the PIN screen: tear
     * the client down instead of dying mid-callback. */
    InstallTerm();
    PlumePairing p;
    if (!PlumePairStart(&p, host, pin)) return 1;
    printf("PIN %s\n", pin);
    fflush(stdout); /* a pipe is block-buffered; the reader needs it now */
    fprintf(stderr, "Enter this PIN in Steam on %s to approve this device.\n", host->hostname);

    while (!SDL_GetAtomicInt(&p.done) && !g_term) SDL_Delay(100);
    bool answered = SDL_GetAtomicInt(&p.done);
    if (PlumePairFinish(&p)) return 0;
    return answered ? EXIT_NOT_PAIRED : 1; /* the host said no / we were stopped */
}

/* ------------------------------- session ---------------------------------- */
static volatile bool g_running = true;

static void OnDisconnected(IHS_Session *s, void *ctx) { (void)s;(void)ctx; g_running = false; }

/* IHS_SessionHIDAddProvider only registers the provider — nothing enumerates it.
 * The device list goes out on IHS_SessionHIDNotifyDeviceChange, which the SDL
 * backend otherwise only reaches from GAMEPAD_ADDED events (sdl_hid_event.c). By
 * the time we stream, those fired at SDL_Init and the launcher menu ate them, so
 * a pad plugged in before launch is never announced and the host sees no
 * controller. Ask for the enumeration once the control channel is up. */
static void OnConnected(IHS_Session *s, void *ctx) {
    (void) ctx;
    IHS_SessionHIDNotifyDeviceChange(s);
}

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
    const char *ff = getenv("PLUME_FORM_FACTOR"); /* debug knob, see PlumeRequestStream */
    if (ff && ff[0] >= '1' && ff[0] <= '5' && !ff[1]) cfg->formFactor = ff[0] - '0';
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
            /* 0x0 during a mode switch would send inf/nan to the host. */
            if (w > 0 && h > 0) IHS_SessionSendMousePosition(s, e->motion.x / w, e->motion.y / h);
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

/* Hotkey = Guide (Home) or Back (Select) — 8BitDo pads label these differently,
 * and some have no Guide at all. */
static bool HotkeyHeld(SDL_JoystickID id) {
    SDL_Gamepad *g = SDL_GetGamepadFromID(id); /* opened by the HID provider */
    return g && (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_GUIDE) ||
                 SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_BACK));
}

/* Hotkey + Start leaves the stream. Polled rather than event-driven: we need
 * both buttons held at the same instant, and the presses are also forwarded to
 * the host as normal HID input. */
static bool QuitComboHeld(void) {
    int n;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    if (!ids) return false;
    bool held = false;
    for (int i = 0; i < n && !held; i++) {
        SDL_Gamepad *g = SDL_GetGamepadFromID(ids[i]);
        held = g && SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_START) && HotkeyHeld(ids[i]);
    }
    SDL_free(ids);
    return held;
}

/* One connected session, start to finish. */
typedef enum { SESSION_FAILED, SESSION_USER_QUIT, SESSION_DROPPED } SessionEnd;

static SessionEnd RunSession(const IHS_SessionInfo *sinfo, bool audio) {
    MediaAttach(g_window, g_renderer, audio, (MediaScale) g_scale);

    g_running = true; /* reset: OnDisconnected clears it */
    /* Static: IHSlib keeps the callbacks pointer and calls configuring() from
     * its worker thread (session_pri.h) — same trap as the client callbacks. */
    static const IHS_StreamSessionCallbacks SESSION_CALLBACKS = {
            .configuring = OnConfiguring, .connected = OnConnected, .disconnected = OnDisconnected};
    IHS_Session *session = IHS_SessionCreate(&PlumeClientConfig, sinfo);
    IHS_SessionSetLogFunction(session, PlumeLog);
    IHS_SessionSetSessionCallbacks(session, &SESSION_CALLBACKS, NULL);
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
        return SESSION_FAILED;
    }

    bool disconnecting = false;
    bool stalled = false;
    Uint64 disconnectAt = 0;
    Uint64 sessionStart = SDL_GetTicksNS();
    while (g_running) {
        bool hidDirty = false;
        bool padButton = false;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT ||
                (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE)) {
                if (!disconnecting) { IHS_SessionDisconnect(session); disconnecting = true; }
            } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_F3) {
                if (!e.key.repeat) MediaToggleStats(); /* ours; not forwarded */
            } else if (!disconnecting) {
                /* Mid-stream (dis)appearance is the signature of a controller or USB
                 * hiccup — the one thing a "controls went dead" report can't show. */
                if (e.type == SDL_EVENT_GAMEPAD_REMOVED || e.type == SDL_EVENT_GAMEPAD_ADDED)
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "gamepad %s mid-stream (id=%d)",
                                e.type == SDL_EVENT_GAMEPAD_REMOVED ? "removed" : "added",
                                (int) e.gdevice.which);
                /* Hotkey + Y toggles the stats overlay (forwarded too, like the
                 * quit combo — the host just sees a Y press). */
                if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
                    e.gbutton.button == SDL_GAMEPAD_BUTTON_NORTH && HotkeyHeld(e.gbutton.which))
                    MediaToggleStats();
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

        /* disconnected() never fires if the session never got connected. */
        if (disconnecting && !disconnectAt) disconnectAt = SDL_GetTicks();
        if (disconnecting && SDL_GetTicks() - disconnectAt > 2000) break;

        /* IHSlib's keepalive only transmits — a hard link drop never fires
         * disconnected(), the picture just freezes. The host streams frames
         * continuously (even a static desktop), so a long silence IS the drop. */
        if (!disconnecting) {
            Uint64 last = MediaLastFrameNS();
            if (last == 0) last = sessionStart; /* nothing decoded yet: count from connect */
            if (SDL_GetTicksNS() - last > 8000000000ull) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "no video for 8 s, dropping the session");
                stalled = true;
                break;
            }
        }
    }
    /* Unacked StopRequest times out inside IHSlib (250 ms), so this returns
     * promptly even when the host is gone. */
    if (stalled) IHS_SessionDisconnect(session);

    IHS_SessionThreadedJoin(session);
    IHS_SessionDestroy(session); /* closes its devices, so destroy the provider after */
    IHS_HIDProviderSDLDestroy(hid);
    MediaDetach();
    return disconnecting ? SESSION_USER_QUIT : SESSION_DROPPED;
}

/* Quit asked while no session is up (request pending, reconnect wait):
 * SIGTERM / window close, Esc, East button, Hotkey+Start. Peeks, so the
 * events stay queued and every later check agrees. Pads must be open. */
static bool QuitAsked(void) {
    SDL_Event ev[16];
    SDL_PumpEvents();
    if (g_term || SDL_HasEvent(SDL_EVENT_QUIT) || QuitComboHeld()) return true;
    int n = SDL_PeepEvents(ev, 16, SDL_PEEKEVENT, SDL_EVENT_KEY_DOWN, SDL_EVENT_KEY_DOWN);
    for (int i = 0; i < n; i++) if (ev[i].key.key == SDLK_ESCAPE) return true;
    n = SDL_PeepEvents(ev, 16, SDL_PEEKEVENT, SDL_EVENT_GAMEPAD_BUTTON_DOWN, SDL_EVENT_GAMEPAD_BUTTON_DOWN);
    for (int i = 0; i < n; i++) if (ev[i].gbutton.button == SDL_GAMEPAD_BUTTON_EAST) return true;
    return false;
}

/* Between sessions nobody holds the pads (the HID provider owns them only
 * while streaming), and a closed pad sends no button. */
static int OpenPads(SDL_Gamepad *pads[8]) {
    int n = 0, np = 0;
    SDL_PumpEvents(); /* let SDL notice pads plugged since init */
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; ids && i < n && np < 8; i++) pads[np++] = SDL_OpenGamepad(ids[i]);
    SDL_free(ids);
    return np;
}
static void ClosePads(SDL_Gamepad *pads[8], int np) {
    for (int i = 0; i < np; i++) if (pads[i]) SDL_CloseGamepad(pads[i]);
}

/* Show `msg` for `ms` while watching for a cancel (see QuitAsked).
 * Returns false when the user cancelled. */
static bool ReconnectWait(const char *msg, int ms) {
    Uint64 until = SDL_GetTicks() + (Uint64) ms;
    do {
        if (QuitAsked()) return false;
        SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
        UIMessage(g_renderer, msg);
        SDL_Delay(50);
    } while (SDL_GetTicks() < until);
    return true;
}

#define RECONNECT_TRIES 5

/* Play a granted stream until the user leaves or reconnecting gives up. */
static int StreamLoop(const IHS_HostInfo *host, IHS_SessionInfo sinfo, bool audio) {
    for (;;) {
        SessionEnd end = RunSession(&sinfo, audio);
        if (end != SESSION_DROPPED) return end == SESSION_USER_QUIT ? 0 : 1;
        /* The link died under us (Wi-Fi blip, host hiccup). The old grant is
         * spent — ask the host for a fresh session and resume, a few times,
         * before giving the user their menu back. */
        bool got = false, cancelled = false;
        SDL_Gamepad *pads[8];
        int np = OpenPads(pads);
        for (int i = 1; i <= RECONNECT_TRIES && !got && !cancelled; i++) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Connection lost - reconnecting (%d/%d)...", i, RECONNECT_TRIES);
            cancelled = !ReconnectWait(msg, i == 1 ? 500 : 2000);
            if (!cancelled)
                got = PlumeRequestStream(host, g_width, g_height, g_fps, g_desktop, PlumeCountGamepads(), &sinfo, NULL);
            cancelled = cancelled || (!got && QuitAsked());
        }
        ClosePads(pads, np); /* the next session's HID provider opens its own */
        if (cancelled) return 0;
        if (!got) return 1;
    }
}

static int DoStream(const IHS_HostInfo *host, bool audio) {
    IHS_SessionInfo sinfo;
    IHS_StreamingResult res;
    if (!PlumeRequestStream(host, g_width, g_height, g_fps, g_desktop, PlumeCountGamepads(), &sinfo, &res)) {
        /* Not paired yet -> pair from the UI, then retry once. */
        if (res != IHS_StreamingUnauthorized) return 1;
        if (!PairScreen(g_renderer, host)) return 1;
        if (!PlumeRequestStream(host, g_width, g_height, g_fps, g_desktop, PlumeCountGamepads(), &sinfo, &res)) return 1;
    }
    return StreamLoop(host, sinfo, audio);
}

/* "--name <n>" with n > 0; anything else falls through to the usage line. */
static bool IntFlag(int argc, char *argv[], int *i, const char *name, int *out) {
    if (strcmp(argv[*i], name) || *i + 1 >= argc || atoi(argv[*i + 1]) <= 0) return false;
    *out = atoi(argv[++*i]);
    return true;
}

/* -------------------------------- main ------------------------------------ */
int main(int argc, char *argv[]) {
    bool pair = false, discover = false;
    int timeout = 0; /* seconds; 0 = the mode's default */
    const char *hostIp = NULL;
    /* --verbose is read first: LoadSettings already logs. */
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--verbose")) PlumeVerbose = true;
    SDL_SetLogPriorities(PlumeVerbose ? SDL_LOG_PRIORITY_INFO : SDL_LOG_PRIORITY_WARN);
    LoadSettings(); /* before argv, so flags win over the saved file */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--pair")) pair = true;
        else if (!strcmp(argv[i], "--discover")) discover = true;
        else if (IntFlag(argc, argv, &i, "--timeout", &timeout)) ;
        else if (IntFlag(argc, argv, &i, "--width", &g_width)) ;
        else if (IntFlag(argc, argv, &i, "--height", &g_height)) ;
        else if (IntFlag(argc, argv, &i, "--fps", &g_fps)) ;
        else if (IntFlag(argc, argv, &i, "--kbps", &g_kbps)) ;
        else if (!strcmp(argv[i], "--no-audio")) g_audio = false;
        else if (!strcmp(argv[i], "--no-desktop")) g_desktop = false;
        else if (!strcmp(argv[i], "--hevc")) g_hevc = true;
        else if (!strcmp(argv[i], "--verbose")) ; /* handled above */
        else if (!strcmp(argv[i], "--host") && i + 1 < argc) hostIp = argv[++i];
        else {
            fprintf(stderr, "usage: %s [--discover | --pair] [--host <ip>] [--timeout <sec>]\n"
                            "       [--width <n>] [--height <n>] [--fps <n>] [--kbps <n>]\n"
                            "       [--no-audio] [--no-desktop] [--hevc] [--verbose]\n", argv[0]);
            return 2;
        }
    }

    PlumeInstallCrashHandler();

    IHS_Init();
    PlumeInitCreds();

    /* Headless: every host that answers, one tab-separated line each. */
    if (discover) {
        IHS_HostInfo found[16];
        int n = PlumeDiscoverAll(timeout ? timeout : 3, found, 16);
        for (int i = 0; i < n; i++) {
            char *ip = IHS_IPAddressToString(&found[i].address.ip);
            PlumeCleanName(found[i].hostname);
            /* The status broadcast doesn't say whether this device is approved;
             * only a stream request does, and that wakes the host. What we do
             * know is whether we hold a key for it: "1", else "?" (it may still
             * be paired the pre-key-exchange way). */
            uint8_t key[32];
            if (ip) printf("%s\t%s\t%s\n", ip, PlumeLoadHostKey(&found[i], key) ? "1" : "?", found[i].hostname);
            free(ip);
        }
        IHS_Quit();
        return 0;
    }

    /* --pair and --host both target one host; neither opens the launcher. */
    IHS_HostInfo host;
    if (pair || hostIp) {
        fprintf(stderr, "Discovering hosts%s...\n", hostIp ? " (filtered)" : "");
        if (!PlumeDiscoverHost(hostIp, timeout ? timeout : 10, &host)) {
            fprintf(stderr, "No host found. Is Steam running on the LAN with Remote Play enabled?\n");
            IHS_Quit();
            return EXIT_NO_HOST;
        }
    }
    if (pair) {
        int rc = DoPair(&host);
        IHS_Quit();
        return rc;
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD) || !TTF_Init()) {
        fprintf(stderr, "SDL/TTF init failed: %s\n", SDL_GetError());
        IHS_Quit();
        return 1;
    }

    /* --host: ask before taking the display, so "not paired" hands the screen
     * straight back to the frontend's pairing page without a window flash. */
    IHS_SessionInfo sinfo;
    InstallTerm(); /* after SDL_Init: chains to SDL's own handler */
    PlumeRequestAbort = QuitAsked;
    if (hostIp) {
        IHS_StreamingResult res;
        SDL_Gamepad *pads[8];
        int np = OpenPads(pads);
        bool got = PlumeRequestStream(&host, g_width, g_height, g_fps, g_desktop, PlumeCountGamepads(), &sinfo, &res);
        ClosePads(pads, np);
        if (!got) {
            SDL_Quit();
            IHS_Quit();
            return res == IHS_StreamingUnauthorized ? EXIT_NOT_PAIRED : 1;
        }
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

    int rc = 0;
    if (hostIp) rc = StreamLoop(&host, sinfo, g_audio); /* no launcher, no SaveSettings */
    else for (;;) {
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
    return rc;
}
