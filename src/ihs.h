/* The IHSlib bits both front-ends need: device identity, logging, discovery and
 * the streaming request. main.c drives them from a window, libretro.c from
 * inside RetroArch; neither should own a second copy of the creds file format. */
#pragma once

#include <stdbool.h>
#include "ihslib.h"
#include "ihslib/client.h"

extern bool PlumeVerbose;          /* below Warn is chatter unless this is set */
extern IHS_ClientConfig PlumeClientConfig; /* valid after PlumeInitCreds() */

/* Path of `name` under ~/.local/share/plume, creating the directory. */
void PlumeDataPath(char *out, size_t n, const char *name);

/* Load creds.bin, or make and save a fresh identity. Fills PlumeClientConfig. */
void PlumeInitCreds(void);

/* Print a stack trace on a fatal signal. The target has no debugger, and a bare
 * "Segmentation fault" says nothing about which thread or which call. */
void PlumeInstallCrashHandler(void);

/* How many SDL gamepads are open right now — for the request's slot reservation. */
int PlumeCountGamepads(void);

/* ---------------------------- offered resolutions -------------------------- */
/* One table, because the launcher menu and the core's options drifted apart once
 * already: the CRT modes landed in the core and never reached the menu.
 *
 * These are a bounding box, not an aspect request: the host scales its own
 * desktop to fit inside, keeping ITS aspect ratio. Bitrate follows the
 * resolution — a 240p stream on a narrow wifi link is pointless if the host still
 * spends 15 Mbps on it. */
typedef struct { const char *label; int w, h, fps, kbps; } PlumeRes;
extern const PlumeRes PlumeResList[];
extern const int PlumeResCount;

/* Index into PlumeResList, falling back to 1080p for a size that isn't offered. */
int PlumeResIndex(int w, int h);

/* IHS_LogFunction for IHS_{Client,Session}SetLogFunction. */
void PlumeLog(IHS_LogLevel level, const char *tag, const char *message);

/* Discover with a timeout (seconds). wantIp NULL accepts the first host. */
bool PlumeDiscoverHost(const char *wantIp, int timeoutSec, IHS_HostInfo *out);

/* Ask the host for a stream. `result` (may be NULL) tells Unauthorized (= not
 * paired) apart from the other failures. */
bool PlumeRequestStream(const IHS_HostInfo *host, int maxWidth, int maxHeight, bool desktop,
                        int gamepadCount, IHS_SessionInfo *out, IHS_StreamingResult *result);

/* ------------------------------- pairing ---------------------------------- */
/* Split in two because the caller has to show the PIN while the host waits for
 * it: a blocking pair() would never get a chance to draw it. Start, then poll
 * `done` from your frame loop, then Finish. */
typedef struct {
    IHS_Client *client;
    volatile bool done;   /* the host answered, either way */
    volatile bool ok;
} PlumePairing;

/* Four digits the user types into Steam on the host. */
void PlumeMakePin(char pin[5]);

bool PlumePairStart(PlumePairing *p, const IHS_HostInfo *host, const char *pin);

/* Tears the client down and reports the outcome. Safe before `done` — that is
 * how you abort a pairing that nobody ever approved. */
bool PlumePairFinish(PlumePairing *p);
