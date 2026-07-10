/* Shared IHSlib plumbing — see ihs.h. Lifted verbatim out of main.c when the
 * libretro core needed the same discovery/pairing identity. */
#define _DEFAULT_SOURCE  /* gethostname, mkdir */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <signal.h>
#include <execinfo.h>

#include <SDL3/SDL.h>
#include "ihs.h"

bool PlumeVerbose = false;
IHS_ClientConfig PlumeClientConfig;

static uint64_t g_deviceId;
static uint8_t g_secretKey[32];
static char g_deviceName[64];

void PlumeDataPath(char *out, size_t n, const char *name) {
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

void PlumeInitCreds(void) {
    char path[512];
    PlumeDataPath(path, sizeof(path), "creds.bin");
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
    PlumeClientConfig = (IHS_ClientConfig) {g_deviceId, g_secretKey, g_deviceName};
}

static void OnFatalSignal(int sig) {
    void *frames[32];
    int n = backtrace(frames, 32);
    fprintf(stderr, "\n*** fatal signal %d (%s), %d frames:\n", sig, strsignal(sig), n);
    fflush(stderr);
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
    signal(sig, SIG_DFL);
    raise(sig); /* let it dump a core if the system is configured for it */
}

void PlumeInstallCrashHandler(void) {
    signal(SIGSEGV, OnFatalSignal);
    signal(SIGABRT, OnFatalSignal);
    signal(SIGBUS, OnFatalSignal);
}

void PlumeLog(IHS_LogLevel level, const char *tag, const char *message) {
    /* Fatal < Error < Warn < Info < Debug < Verbose: below Warn is chatter. */
    if (!PlumeVerbose && level > IHS_LogLevelWarn) return;
    fprintf(stderr, "[IHS.%s %s] %s\n", tag, IHS_LogLevelName(level), message);
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
    if (PlumeVerbose) printf("Found host: %s\n", info->hostname);
    IHS_ClientStop(client);
}

bool PlumeDiscoverHost(const char *wantIp, int timeoutSec, IHS_HostInfo *out) {
    IHS_Client *client = IHS_ClientCreate(&PlumeClientConfig);
    IHS_ClientSetLogFunction(client, PlumeLog);
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

/* ------------------------------- pairing ---------------------------------- */
static void OnAuthSuccess(IHS_Client *c, const IHS_HostInfo *h, uint64_t steamId, void *ctx) {
    (void) h;
    printf("Paired (steamId=%llu)\n", (unsigned long long) steamId);
    PlumePairing *p = ctx;
    p->ok = true;
    p->done = true;
    IHS_ClientStop(c);
}
static void OnAuthFailed(IHS_Client *c, const IHS_HostInfo *h, IHS_AuthorizationResult r, void *ctx) {
    (void) h;
    fprintf(stderr, "Pairing failed (result=%d)\n", r);
    ((PlumePairing *) ctx)->done = true;
    IHS_ClientStop(c);
}
static void OnAuthProgress(IHS_Client *c, const IHS_HostInfo *h, void *ctx) { (void)c;(void)h;(void)ctx; }

void PlumeMakePin(char pin[5]) {
    uint32_t r = 0;
    FILE *ur = fopen("/dev/urandom", "rb");
    if (ur) {
        if (fread(&r, sizeof(r), 1, ur) != 1) r = 0;
        fclose(ur);
    }
    snprintf(pin, 5, "%04u", r % 10000);
}

bool PlumePairStart(PlumePairing *p, const IHS_HostInfo *host, const char *pin) {
    *p = (PlumePairing) {0};
    p->client = IHS_ClientCreate(&PlumeClientConfig);
    if (!p->client) return false;
    IHS_ClientSetLogFunction(p->client, PlumeLog);
    IHS_ClientAuthorizationCallbacks cb = {
            .progress = OnAuthProgress, .success = OnAuthSuccess, .failed = OnAuthFailed};
    IHS_ClientSetAuthorizationCallbacks(p->client, &cb, p);
    /* Discovery keeps the worker thread alive and the socket serviced; without
     * it the request is never delivered. */
    IHS_ClientStartDiscovery(p->client, 0);
    IHS_ClientAuthorizationRequest(p->client, host, pin);
    return true;
}

bool PlumePairFinish(PlumePairing *p) {
    if (!p->client) return false;
    IHS_ClientStop(p->client); /* no-op if a callback already stopped it */
    IHS_ClientThreadedJoin(p->client);
    IHS_ClientStopDiscovery(p->client);
    IHS_ClientDestroy(p->client);
    p->client = NULL;
    return p->ok;
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

bool PlumeRequestStream(const IHS_HostInfo *host, int maxWidth, int maxHeight, bool desktop,
                        IHS_SessionInfo *out, IHS_StreamingResult *result) {
    IHS_Client *client = IHS_ClientCreate(&PlumeClientConfig);
    IHS_ClientSetLogFunction(client, PlumeLog);
    StreamReq req = {0};
    IHS_ClientStreamingCallbacks cb = {
            .progress = OnStreamProgress, .success = OnStreamSuccess, .failed = OnStreamFailed};
    IHS_ClientSetStreamingCallbacks(client, &cb, &req);
    IHS_StreamingRequest r = {
            .streamingEnable = {true, true, true},
            .maxResolution = {maxWidth, maxHeight},
            .audioChannelCount = 2,
            /* Desktop off means we want the host's Big Picture / game session. */
            .streamingInterface = desktop ? IHS_StreamInterfaceDesktop : IHS_StreamInterfaceBigPicture,
            .streamDesktop = desktop,
    };
    /* Discovery keeps the worker thread alive and the socket serviced; without
     * it ThreadedJoin returns instantly and the request is never delivered. */
    IHS_ClientStartDiscovery(client, 0);
    IHS_ClientStreamingRequest(client, host, &r);
    IHS_ClientThreadedJoin(client);
    IHS_ClientStopDiscovery(client);
    IHS_ClientDestroy(client);
    if (req.ok) *out = req.info;
    if (result) *result = req.result;
    return req.ok;
}
