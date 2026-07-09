/* Plume — a Steam Remote Play client built on IHSlib.
 *
 *   plume                 discover a host, request a stream, and play it
 *   plume --pair          pair with a host (enter the shown PIN in Steam)
 *   plume --host <ip>     target a specific host instead of the first found
 *   plume --no-audio      video only
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
#include "media.h"
#include "ui.h"

/* Shared SDL objects, created once and lent to the menu and the decoder. */
static SDL_Window *g_window;
static SDL_Renderer *g_renderer;
static TTF_Font *g_font, *g_titleFont;

/* -------- persistent device identity (so pairing survives restarts) -------- */
static uint64_t g_deviceId;
static uint8_t g_secretKey[32];
static char g_deviceName[64];
static IHS_ClientConfig g_config;

/* -------- user settings, from settings.conf then argv then the menu -------- */
static bool g_hevc = false; /* --hevc: request HEVC (HW decode on Pi5) */
static int g_width = 1920, g_height = 1080, g_fps = 60, g_kbps = 15000; /* host encode cap */
static bool g_audio = true;
static bool g_desktop = true; /* stream the host's desktop, else its game session */
static int g_scale = MEDIA_SCALE_FIT; /* fit / stretch / crop on aspect mismatch */

static void DataPath(char *out, size_t n, const char *name) {
    const char *home = getenv("HOME");
    if (!home) home = ".";
    char dir[400];
    snprintf(dir, sizeof(dir), "%s/.local/share/plume", home);
    if (mkdir(dir, 0700) == 0) {
        /* We just created it, so this is the first run since the project was
         * renamed from steamlink-ihs. Adopt the old directory whole, or the
         * user silently loses their pairing and has to re-enter the PIN. */
        char old[400];
        snprintf(old, sizeof(old), "%s/.local/share/steamlink-ihs", home);
        if (rmdir(dir) == 0 && rename(old, dir) != 0) {
            mkdir(dir, 0700); /* nothing to adopt; put ours back */
        }
    }
    snprintf(out, n, "%s/%s", dir, name);
}

static void LoadOrCreateCreds(void) {
    char path[512];
    DataPath(path, sizeof(path), "creds.bin");
    FILE *f = fopen(path, "rb");
    if (f && fread(&g_deviceId, 8, 1, f) == 1 && fread(g_secretKey, 32, 1, f) == 1) {
        fclose(f);
    } else {
        if (f) fclose(f);
        FILE *r = fopen("/dev/urandom", "rb");
        if (!r || fread(&g_deviceId, 8, 1, r) != 1 || fread(g_secretKey, 32, 1, r) != 1) {
            fprintf(stderr, "cannot read /dev/urandom\n");
            exit(1);
        }
        fclose(r);
        FILE *w = fopen(path, "wb");
        if (w) { fwrite(&g_deviceId, 8, 1, w); fwrite(g_secretKey, 32, 1, w); fclose(w); }
    }
    if (gethostname(g_deviceName, sizeof(g_deviceName)) != 0)
        strcpy(g_deviceName, "plume");
    g_config = (IHS_ClientConfig) {g_deviceId, g_secretKey, g_deviceName};
}

/* ----------------------------- logging ------------------------------------ */
static void LogPrint(IHS_LogLevel level, const char *tag, const char *message) {
    fprintf(stderr, "[IHS.%s %s] %s\n", tag, IHS_LogLevelName(level), message);
}

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

/* --------------------- discovery: pick a host ----------------------------- */
typedef struct {
    const char *wantIp;   /* NULL = accept first */
    bool found;
    IHS_HostInfo host;
} HostPick;

static void OnDiscovered(IHS_Client *client, const IHS_HostInfo *info, void *ctx) {
    HostPick *p = ctx;
    if (p->found) return;
    if (p->wantIp) {
        char *ip = IHS_IPAddressToString(&info->address.ip);
        bool match = ip && strcmp(ip, p->wantIp) == 0;
        free(ip);
        if (!match) return;
    }
    p->host = *info;
    p->found = true;
    printf("Found host: %s\n", info->hostname);
    IHS_ClientStop(client);
}

/* Discover with a timeout (seconds); returns picked host or NULL. */
static bool DiscoverHost(const char *wantIp, int timeoutSec, IHS_HostInfo *out) {
    IHS_Client *client = IHS_ClientCreate(&g_config);
    IHS_ClientSetLogFunction(client, LogPrint);
    HostPick pick = {.wantIp = wantIp};
    IHS_ClientDiscoveryCallbacks cb = {.discovered = OnDiscovered};
    IHS_ClientSetDiscoveryCallbacks(client, &cb, &pick);
    IHS_ClientStartDiscovery(client, 1000);
    for (int i = 0; i < timeoutSec * 10 && !pick.found; i++) SDL_Delay(100);
    IHS_ClientStopDiscovery(client);
    IHS_ClientStop(client);
    IHS_ClientThreadedJoin(client);
    IHS_ClientDestroy(client);
    if (pick.found) *out = pick.host;
    return pick.found;
}

/* ------------------------------ pairing ----------------------------------- */
typedef struct { bool done; bool ok; } AuthState;

static void OnAuthSuccess(IHS_Client *c, const IHS_HostInfo *h, uint64_t steamId, void *ctx) {
    (void) h;
    printf("Paired (steamId=%llu)\n", (unsigned long long) steamId);
    AuthState *s = ctx; s->done = true; s->ok = true;
    IHS_ClientStop(c);
}
static void OnAuthFailed(IHS_Client *c, const IHS_HostInfo *h, IHS_AuthorizationResult r, void *ctx) {
    (void) h;
    printf("Pairing failed (result=%d)\n", r);
    AuthState *s = ctx; s->done = true;
    IHS_ClientStop(c);
}
static void OnAuthProgress(IHS_Client *c, const IHS_HostInfo *h, void *ctx) { (void)c;(void)h;(void)ctx; }

static int DoPair(const IHS_HostInfo *host) {
    /* PIN the user must type into Steam on the host to approve this device. */
    uint32_t r = 0;
    FILE *ur = fopen("/dev/urandom", "rb");
    if (ur) { fread(&r, sizeof(r), 1, ur); fclose(ur); }
    char pin[5];
    snprintf(pin, sizeof(pin), "%04u", r % 10000);
    printf("\n  Enter this PIN in Steam on the host to approve:  %s\n\n", pin);

    IHS_Client *client = IHS_ClientCreate(&g_config);
    IHS_ClientSetLogFunction(client, LogPrint);
    AuthState st = {0};
    IHS_ClientAuthorizationCallbacks cb = {
            .progress = OnAuthProgress, .success = OnAuthSuccess, .failed = OnAuthFailed};
    IHS_ClientSetAuthorizationCallbacks(client, &cb, &st);
    /* Discovery keeps the worker thread alive and the socket serviced; without
     * it ThreadedJoin returns instantly and the request is never delivered. */
    IHS_ClientStartDiscovery(client, 0);
    IHS_ClientAuthorizationRequest(client, host, pin);
    IHS_ClientThreadedJoin(client);
    IHS_ClientStopDiscovery(client);
    IHS_ClientDestroy(client);
    return st.ok ? 0 : 1;
}

/* -------------------- streaming request -> session info ------------------- */
typedef struct { bool done; bool ok; IHS_SessionInfo info; IHS_StreamingResult result; } StreamReq;

static void OnStreamSuccess(IHS_Client *c, const IHS_HostInfo *h, const IHS_SocketAddress *addr,
                            const uint8_t *key, size_t keyLen, void *ctx) {
    (void) h;
    StreamReq *s = ctx;
    s->info.address = *addr;
    s->info.sessionKeyLen = keyLen;
    memcpy(s->info.sessionKey, key, keyLen);
    s->info.steamId = 0;
    s->done = true; s->ok = true;
    IHS_ClientStop(c);
}
static void OnStreamFailed(IHS_Client *c, const IHS_HostInfo *h, IHS_StreamingResult r, void *ctx) {
    (void) h;
    fprintf(stderr, "Streaming request failed (result=%d)\n", r);
    StreamReq *s = ctx; s->done = true; s->result = r;
    IHS_ClientStop(c);
}
static void OnStreamProgress(IHS_Client *c, const IHS_HostInfo *h, void *ctx) { (void)c;(void)h;(void)ctx; }

static bool RequestStream(const IHS_HostInfo *host, IHS_SessionInfo *out, IHS_StreamingResult *result) {
    IHS_Client *client = IHS_ClientCreate(&g_config);
    IHS_ClientSetLogFunction(client, LogPrint);
    StreamReq req = {0};
    IHS_ClientStreamingCallbacks cb = {
            .progress = OnStreamProgress, .success = OnStreamSuccess, .failed = OnStreamFailed};
    IHS_ClientSetStreamingCallbacks(client, &cb, &req);
    IHS_StreamingRequest r = {
            .streamingEnable = {true, true, true},
            .maxResolution = {g_width, g_height},
            .audioChannelCount = 2,
            /* Desktop off means we want the host's Big Picture / game session. */
            .streamingInterface = g_desktop ? IHS_StreamInterfaceDesktop : IHS_StreamInterfaceBigPicture,
            .streamDesktop = g_desktop,
    };
    IHS_ClientStartDiscovery(client, 0); /* keeps the worker thread alive (see DoPair) */
    IHS_ClientStreamingRequest(client, host, &r);
    IHS_ClientThreadedJoin(client);
    IHS_ClientStopDiscovery(client);
    IHS_ClientDestroy(client);
    if (req.ok) *out = req.info;
    if (result) *result = req.result;
    return req.ok;
}

/* ------------------------------- session ---------------------------------- */
static volatile bool g_running = true;

static void OnDisconnected(IHS_Session *s, void *ctx) { (void)s;(void)ctx; g_running = false; }

/* settings.conf: plain "key value" lines, so it can be inspected and hand-edited.
 * Unknown keys are ignored, so a newer file stays readable by an older build. */
static void LoadSettings(void) {
    char path[512];
    DataPath(path, sizeof(path), "settings.conf");
    FILE *f = fopen(path, "r");
    if (!f) {
        /* Silently running on defaults after the user picked 480p in the menu is
         * the kind of thing you chase for an hour. $HOME decides where this lives. */
        fprintf(stderr, "settings: %s absent, using defaults\n", path);
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
    fprintf(stderr, "settings: %s -> %dx%d @ %d fps, %d kbps, scaling %d, hevc %d, audio %d, desktop %d\n",
            path, g_width, g_height, g_fps, g_kbps, g_scale, g_hevc, g_audio, g_desktop);
}

static void SaveSettings(void) {
    char path[512];
    DataPath(path, sizeof(path), "settings.conf");
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
    if (!RequestStream(host, &sinfo, &res)) {
        /* Not paired yet -> pair from the UI, then retry once. */
        if (res != IHS_StreamingUnauthorized) return 1;
        if (!PairScreen(g_renderer, g_font, g_titleFont, &g_config, host)) return 1;
        if (!RequestStream(host, &sinfo, &res)) return 1;
    }

    MediaAttach(g_window, g_renderer, audio, (MediaScale) g_scale);

    g_running = true; /* reset: OnDisconnected clears it */
    IHS_StreamSessionCallbacks scb = {.configuring = OnConfiguring, .disconnected = OnDisconnected};
    IHS_Session *session = IHS_SessionCreate(&g_config, &sinfo);
    IHS_SessionSetLogFunction(session, LogPrint);
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
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT ||
                (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE)) {
                if (!disconnecting) { IHS_SessionDisconnect(session); disconnecting = true; }
            } else if (!disconnecting) {
                hidDirty |= ForwardInput(session, &e);
            }
        }
        if (!disconnecting && QuitComboHeld()) {
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

/* Find a usable TTF: env override, then common system/Recalbox locations. */
static TTF_Font *LoadFont(float pt) {
    const char *paths[] = {
            SDL_getenv("PLUME_FONT"),
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/TTF/DejaVuSans.ttf",
            "/recalbox/share_init/system/.emulationstation/fonts/OpenSans_Regular.ttf",
    };
    for (size_t i = 0; i < SDL_arraysize(paths); i++) {
        if (paths[i] && paths[i][0]) {
            TTF_Font *f = TTF_OpenFont(paths[i], pt);
            if (f) return f;
        }
    }
    return NULL;
}

/* -------------------------------- main ------------------------------------ */
int main(int argc, char *argv[]) {
    bool pair = false;
    const char *hostIp = NULL;
    LoadSettings(); /* before argv, so flags win over the saved file */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--pair")) pair = true;
        else if (!strcmp(argv[i], "--no-audio")) g_audio = false;
        else if (!strcmp(argv[i], "--no-desktop")) g_desktop = false;
        else if (!strcmp(argv[i], "--hevc")) g_hevc = true;
        else if (!strcmp(argv[i], "--host") && i + 1 < argc) hostIp = argv[++i];
        else {
            fprintf(stderr, "usage: %s [--pair] [--host <ip>] [--no-audio] [--no-desktop] [--hevc]\n", argv[0]);
            return 2;
        }
    }

    signal(SIGSEGV, OnFatalSignal);
    signal(SIGABRT, OnFatalSignal);
    signal(SIGBUS, OnFatalSignal);

    IHS_Init();
    LoadOrCreateCreds();

    /* --pair keeps the headless flow: discover one host, pair, exit. */
    if (pair) {
        IHS_HostInfo host;
        printf("Discovering hosts%s...\n", hostIp ? " (filtered)" : "");
        int rc = 1;
        if (DiscoverHost(hostIp, 10, &host)) rc = DoPair(&host);
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

    /* Size fonts from the real output height so the UI fits 240p/480p up to 1080p+. */
    int ow, oh;
    SDL_GetRenderOutputSize(g_renderer, &ow, &oh);
    if (oh <= 0) oh = 720;
    g_font = LoadFont(oh / 24.0f);
    g_titleFont = LoadFont(oh / 13.0f);
    if (!g_font || !g_titleFont) {
        fprintf(stderr, "Font unavailable (set PLUME_FONT to a .ttf)\n");
        IHS_Quit();
        return 1;
    }

    for (;;) {
        UIResult r = {.hevc = g_hevc, .audio = g_audio, .desktop = g_desktop, .scale = g_scale,
                      .width = g_width, .height = g_height, .fps = g_fps, .kbps = g_kbps};
        UIAction act = RunMenu(g_window, g_renderer, g_font, g_titleFont, &g_config, &r);
        g_hevc = r.hevc; g_audio = r.audio; g_desktop = r.desktop; g_scale = r.scale;
        g_width = r.width; g_height = r.height; g_fps = r.fps; g_kbps = r.kbps;
        SaveSettings();
        if (act != UI_START) break;
        DoStream(&r.host, r.audio);
    }

    TTF_CloseFont(g_font);
    TTF_CloseFont(g_titleFont);
    SDL_DestroyRenderer(g_renderer);
    SDL_DestroyWindow(g_window);
    TTF_Quit();
    SDL_Quit();
    IHS_Quit();
    return 0;
}
