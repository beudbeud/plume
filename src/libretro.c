/* Plume as a libretro core: RetroArch drives the frame loop, we hand it the
 * decoded Steam Remote Play stream.
 *
 * Content is optional. A `.plume` file holding a host IP on its first line
 * targets that host; with no content the core streams from the first host it
 * discovers on the LAN. An unpaired host puts the core in a pairing state: the
 * PIN is shown as a frontend message until Steam approves it, and the stream
 * starts on its own. That is the only way in on a Recalbox box, where the
 * standalone `plume` binary is not installed.
 *
 * Gamepads go to the host through IHSlib's SDL HID provider, which opens the
 * pads itself. RetroArch reads the same evdev nodes, and both can: the kernel
 * allows several readers. That is why this core links SDL for input while
 * taking video and audio through the libretro callbacks.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>
#include "ihslib.h"
#include "ihslib/hid/sdl.h"
#include "libretro.h"
#include "ihs.h"
#include "media.h"

/* The host never streams more than we ask for, and we never ask for more than
 * 1080p — so one buffer of this size fits every frame. */
#define MAX_W 1920
#define MAX_H 1080
#define AUDIO_RATE 48000

static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_environment_t environ_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_log_printf_t log_cb;

static uint32_t *g_frame;          /* XRGB8888, MAX_W * MAX_H */
static int g_frameW = 1280, g_frameH = 720;
static IHS_Session *g_session;
static IHS_HIDProvider *g_hid;
static volatile bool g_running;

/* The core pairs on its own: RetroArch is the only front-end on a Recalbox box,
 * and `plume --pair` may not be installed there. The PIN has to be drawn while
 * the host waits for it, so pairing is a state retro_run() sits in, not a
 * blocking call inside retro_load_game(). */
static enum { ST_STREAM, ST_PAIRING } g_state;
static PlumePairing g_pairing;
static char g_pin[5];
static IHS_HostInfo g_host;
static int g_pairTicks;
#define PAIR_TIMEOUT_TICKS (90 * 60) /* 90 s at 60 Hz: time to walk to the host */

/* Negotiated caps, from the core options. Bitrate follows the resolution, same
 * table as the launcher's menu (ui.c): a 240p stream on a narrow wifi link is
 * pointless if we still let the host spend 15 Mbps on it. */
static const struct { const char *label; int w, h, kbps; } RES[] = {
        {"240p",  426,  240,  3000},
        {"480p",  854,  480,  6000},
        {"720p",  1280, 720,  10000},
        {"1080p", 1920, 1080, 15000},
};
static int g_width = 1280, g_height = 720, g_kbps = 10000;
static bool g_audio = true, g_desktop = true, g_hevc = false;

static void Log(enum retro_log_level level, const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (log_cb) log_cb(level, "%s\n", msg);
    else fprintf(stderr, "[plume] %s\n", msg);
}

/* ------------------------------ core options ------------------------------ */
static const struct retro_variable OPTIONS[] = {
        {"plume_resolution", "Resolution; 720p|1080p|480p|240p"},
        {"plume_audio",      "Audio; enabled|disabled"},
        {"plume_desktop",    "Desktop mode; enabled|disabled"},
        {"plume_hevc",       "HEVC video; disabled|enabled"},
        {NULL, NULL},
};

static bool OptionIs(const char *key, const char *value) {
    struct retro_variable v = {key, NULL};
    return environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &v) && v.value && !strcmp(v.value, value);
}

/* Read once, at load: all of these are negotiated with the host up front. */
static void LoadOptions(void) {
    for (size_t i = 0; i < sizeof(RES) / sizeof(RES[0]); i++) {
        if (!OptionIs("plume_resolution", RES[i].label)) continue;
        g_width = RES[i].w;
        g_height = RES[i].h;
        g_kbps = RES[i].kbps;
        break;
    }
    g_audio = !OptionIs("plume_audio", "disabled");
    /* Same default as the client: a Linux host advertises no Big Picture session,
     * and asking for one gets the stream torn down a few seconds in. */
    g_desktop = !OptionIs("plume_desktop", "disabled");
    g_hevc = OptionIs("plume_hevc", "enabled");
}

/* ------------------------------- session ---------------------------------- */
static void OnConfiguring(IHS_Session *s, IHS_SessionConfig *cfg, void *ctx) {
    (void) s; (void) ctx;
    cfg->enableAudio = g_audio;
    cfg->enableHevc = g_hevc;
    cfg->maxWidth = g_width;
    cfg->maxHeight = g_height;
    cfg->maxFps = 60;
    cfg->maxBitrateKbps = g_kbps;
}

static void OnDisconnected(IHS_Session *s, void *ctx) { (void) s; (void) ctx; g_running = false; }

/* Claim the gamepads only once the session is up. IHSlib enumerates a provider
 * on its timer thread, and a provider added before IHS_SessionConnect() races
 * the handshake: here RetroArch spends over a second in its KMS/EGL init while
 * our ClientHandshake is being retransmitted, the timer wins, and RemoteHID
 * goes out before AuthenticationRequest. The host answers nothing after that.
 * `connected` fires right after NegotiationComplete (ch_control_negotiation.c),
 * which is where the standalone client happens to send its own RemoteHID. */
static void OnConnected(IHS_Session *s, void *ctx) {
    (void) ctx;
    g_hid = IHS_HIDProviderSDLCreateManaged();
    IHS_SessionHIDAddProvider(s, g_hid);
    /* AddProvider only registers it. The device list is announced on enumeration,
     * which the SDL backend drives from GAMEPAD_ADDED events — and those fired
     * before the session existed, since the pad was plugged in long ago. Ask for
     * the enumeration ourselves, or the host never learns of any controller. */
    IHS_SessionHIDNotifyDeviceChange(s);
}

/* IHSlib keeps the pointer, not a copy (session_pri.h: `const
 * IHS_StreamSessionCallbacks *session`), and calls configuring() from its worker
 * thread once negotiation starts — long after whoever set it has returned. A
 * local would be a dead stack frame by then. */
static const IHS_StreamSessionCallbacks SESSION_CALLBACKS = {
        .configuring = OnConfiguring,
        .connected = OnConnected,
        .disconnected = OnDisconnected,
};

/* ------------------------------- input ------------------------------------ */
/* Only mouse and gamepad. Keyboard would need RETROK_* -> USB HID usage, and
 * the pads are what a RetroArch user has in their hands.
 * ponytail: no keyboard, add a RETROK table when someone streams a desktop. */
static void ForwardMouse(void) {
    static const struct { unsigned id; IHS_StreamInputMouseButton btn; } BUTTONS[] = {
            {RETRO_DEVICE_ID_MOUSE_LEFT,   IHS_MOUSE_BUTTON_LEFT},
            {RETRO_DEVICE_ID_MOUSE_RIGHT,  IHS_MOUSE_BUTTON_RIGHT},
            {RETRO_DEVICE_ID_MOUSE_MIDDLE, IHS_MOUSE_BUTTON_MIDDLE},
    };
    static bool held[3];

    int dx = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
    int dy = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);
    if (dx || dy) IHS_SessionSendMouseMovement(g_session, dx, dy);

    for (int i = 0; i < 3; i++) {
        bool down = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, BUTTONS[i].id) != 0;
        if (down == held[i]) continue;
        held[i] = down;
        if (down) IHS_SessionSendMouseDown(g_session, BUTTONS[i].btn);
        else IHS_SessionSendMouseUp(g_session, BUTTONS[i].btn);
    }
}

/* --------------------------- libretro entry points ------------------------ */
void retro_init(void) {
    PlumeVerbose = getenv("PLUME_VERBOSE") != NULL;
    SDL_SetLogPriorities(PlumeVerbose ? SDL_LOG_PRIORITY_INFO : SDL_LOG_PRIORITY_WARN);
    /* A core that segfaults inside RetroArch prints nothing but "Segmentation
     * fault", and the frame loop spans three threads. */
    PlumeInstallCrashHandler();
    g_frame = calloc(MAX_W * MAX_H, sizeof(*g_frame));
}

void retro_deinit(void) {
    free(g_frame);
    g_frame = NULL;
}

unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_get_system_info(struct retro_system_info *info) {
    memset(info, 0, sizeof(*info));
    info->library_name = "Plume (Steam Remote Play)";
    info->library_version = "1.0";
    info->valid_extensions = "plume";
    info->need_fullpath = true;
}

void retro_get_system_av_info(struct retro_system_av_info *info) {
    memset(info, 0, sizeof(*info));
    info->geometry.base_width = g_frameW;
    info->geometry.base_height = g_frameH;
    info->geometry.max_width = MAX_W;
    info->geometry.max_height = MAX_H;
    info->geometry.aspect_ratio = 0.0f; /* base_width / base_height */
    info->timing.fps = 60.0;
    info->timing.sample_rate = AUDIO_RATE;
}

void retro_set_environment(retro_environment_t cb) {
    environ_cb = cb;
    bool noGame = true;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &noGame);
    cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void *) OPTIONS);
}

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void) cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }
void retro_set_controller_port_device(unsigned port, unsigned device) { (void) port; (void) device; }

/* First line of the content file, if any, is the host's IP. */
static bool ReadHostIp(const char *path, char *out, size_t n) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool ok = fgets(out, (int) n, f) != NULL;
    fclose(f);
    if (!ok) return false;
    out[strcspn(out, " \t\r\n")] = '\0';
    return out[0] != '\0';
}

/* Brings the session up on an already-granted streaming request. The host
 * allocates a session per request, so it must be asked exactly once. */
static bool StartSession(const IHS_SessionInfo *sinfo) {
    MediaAttach(NULL, NULL, g_audio, MEDIA_SCALE_FIT); /* headless: RetroArch scales */

    g_running = true;
    g_session = IHS_SessionCreate(&PlumeClientConfig, sinfo);
    IHS_SessionSetLogFunction(g_session, PlumeLog);
    IHS_SessionSetSessionCallbacks(g_session, &SESSION_CALLBACKS, NULL);
    IHS_SessionSetVideoCallbacks(g_session, &VideoCallbacks, NULL);
    IHS_SessionSetAudioCallbacks(g_session, &AudioCallbacks, NULL);
    if (!IHS_SessionConnect(g_session)) { /* OnConnected adds the HID provider */
        Log(RETRO_LOG_ERROR, "Failed to connect the session");
        IHS_SessionDestroy(g_session);
        g_session = NULL;
        MediaDetach();
        return false;
    }
    g_state = ST_STREAM;
    Log(RETRO_LOG_INFO, "streaming from %s at %dx%d", g_host.hostname, g_width, g_height);
    return true;
}

bool retro_load_game(const struct retro_game_info *info) {
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
        Log(RETRO_LOG_ERROR, "XRGB8888 is not supported by this frontend");
        return false;
    }
    struct retro_log_callback logging;
    if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging)) log_cb = logging.log;

    LoadOptions();
    g_frameW = g_width;
    g_frameH = g_height;

    /* The HID provider opens the gamepads; RetroArch keeps reading them too. */
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        Log(RETRO_LOG_ERROR, "SDL gamepad init failed: %s", SDL_GetError());
        return false;
    }
    IHS_Init();
    PlumeInitCreds();

    char wantIp[64];
    const char *hostIp = info && info->path && ReadHostIp(info->path, wantIp, sizeof(wantIp))
                         ? wantIp : NULL;
    if (!PlumeDiscoverHost(hostIp, 10, &g_host)) {
        Log(RETRO_LOG_ERROR, "No Steam host found on the LAN%s", hostIp ? " at that address" : "");
        goto fail;
    }

    /* Unauthorized just means this device has never been approved. Ask for a
     * PIN and let retro_run() show it; everything else is fatal. */
    IHS_StreamingResult res;
    IHS_SessionInfo sinfo;
    if (!PlumeRequestStream(&g_host, g_width, g_height, g_desktop, &sinfo, &res)) {
        if (res != IHS_StreamingUnauthorized) {
            Log(RETRO_LOG_ERROR, "%s refused the stream (result=%d)", g_host.hostname, res);
            goto fail;
        }
        PlumeMakePin(g_pin);
        if (!PlumePairStart(&g_pairing, &g_host, g_pin)) {
            Log(RETRO_LOG_ERROR, "Could not start pairing with %s", g_host.hostname);
            goto fail;
        }
        g_state = ST_PAIRING;
        g_pairTicks = 0;
        return true;
    }

    return StartSession(&sinfo);

fail:
    IHS_Quit();
    SDL_Quit();
    return false;
}

bool retro_load_game_special(unsigned t, const struct retro_game_info *i, size_t n) {
    (void) t; (void) i; (void) n;
    return false;
}

void retro_unload_game(void) {
    if (g_state == ST_PAIRING) {
        PlumePairFinish(&g_pairing); /* nobody approved it; drop the request */
        g_state = ST_STREAM;
    }
    if (!g_session) {
        IHS_Quit();
        SDL_Quit();
        return;
    }
    /* Only if the session is still up. The host's own Disconnect already ran
     * IHS_SessionInterrupt, and a second disconnect queues a timer task the
     * stopped worker never drains — IHS_SessionDestroy then fires it while
     * tearing the timer down, and IHS_QueueAppend writes into freed memory. */
    if (g_running) IHS_SessionDisconnect(g_session);
    IHS_SessionThreadedJoin(g_session);
    IHS_SessionDestroy(g_session); /* closes its devices, so destroy the provider after */
    if (g_hid) { IHS_HIDProviderSDLDestroy(g_hid); g_hid = NULL; } /* NULL if we never connected */
    g_session = NULL;
    MediaDetach();
    IHS_Quit();
    SDL_Quit();
}

/* Sit on a black frame showing the PIN until the host answers. RetroArch draws
 * the message over whatever we hand video_cb, so a blank frame is enough. */
static void PairTick(void) {
    if (g_pairTicks % 120 == 0) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Plume: enter PIN %s in Steam on %s", g_pin, g_host.hostname);
        struct retro_message m = {msg, 130}; /* outlast the gap to the next one */
        environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &m);
    }
    video_cb(g_frame, g_frameW, g_frameH, MAX_W * sizeof(*g_frame)); /* still the calloc'd black */

    if (!g_pairing.done) {
        if (++g_pairTicks < PAIR_TIMEOUT_TICKS) return;
        Log(RETRO_LOG_ERROR, "Pairing timed out");
    } else if (PlumePairFinish(&g_pairing)) {
        g_pairTicks = 0;
        IHS_SessionInfo sinfo;
        if (PlumeRequestStream(&g_host, g_width, g_height, g_desktop, &sinfo, NULL) &&
            StartSession(&sinfo)) {
            return; /* g_state is now ST_STREAM */
        }
        environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
        return;
    }
    PlumePairFinish(&g_pairing); /* no-op once the client is gone */
    environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
}

void retro_run(void) {
    if (g_state == ST_PAIRING) {
        PairTick();
        return;
    }
    if (!g_session) return;

    /* Gamepads reach the host over the HID channel, not the input channel: the
     * provider accumulates state from SDL events and we flush one packet per
     * frame. Per-event packets saturate the uplink and starve the video. */
    bool hidDirty = false;
    SDL_Event e;
    while (SDL_PollEvent(&e)) hidDirty |= IHS_HIDHandleSDLEvent(g_session, &e);
    if (hidDirty) IHS_SessionHIDSendReport(g_session);

    input_poll_cb();
    ForwardMouse();

    int w = g_frameW, h = g_frameH;
    if (MediaPullVideo(g_frame, MAX_W * (int) sizeof(*g_frame), MAX_H, &w, &h)) {
        if (w != g_frameW || h != g_frameH) {
            g_frameW = w;
            g_frameH = h;
            struct retro_game_geometry geom = {.base_width = w, .base_height = h,
                                               .max_width = MAX_W, .max_height = MAX_H};
            environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geom);
        }
        video_cb(g_frame, w, h, MAX_W * sizeof(*g_frame));
    } else {
        video_cb(NULL, g_frameW, g_frameH, MAX_W * sizeof(*g_frame)); /* nothing new: dup */
    }

    /* One Opus packet is at most 120 ms; a 60 Hz tick drains ~800 frames. */
    static int16_t pcm[4096 * 2];
    int frames;
    while ((frames = MediaPullAudio(pcm, 4096)) > 0) audio_batch_cb(pcm, frames);

    if (!g_running) environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
}

/* A live stream has no state to reset, save or rewind. */
void retro_reset(void) {}
size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void *d, size_t n) { (void) d; (void) n; return false; }
bool retro_unserialize(const void *d, size_t n) { (void) d; (void) n; return false; }
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned i, bool e, const char *c) { (void) i; (void) e; (void) c; }
void *retro_get_memory_data(unsigned id) { (void) id; return NULL; }
size_t retro_get_memory_size(unsigned id) { (void) id; return 0; }
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
