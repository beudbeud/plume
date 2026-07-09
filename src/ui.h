/* IHSplay-style launcher: pick a host and start streaming. SDL3 + SDL3_ttf,
 * navigable by keyboard and gamepad. */
#pragma once

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include "ihslib.h"
#include "media.h"

typedef enum { UI_QUIT, UI_START } UIAction;

typedef struct {
    IHS_HostInfo host;   /* chosen host, valid when action == UI_START */
    bool hevc;           /* settings: request HEVC (HW decode on Pi5) */
    bool audio;          /* settings: audio on/off */
    bool desktop;        /* settings: stream the host's desktop, else its game session */
    int scale;           /* settings: MediaScale — fit / stretch / crop */
    int width, height;   /* settings: cap on the host's encode resolution */
    int fps;             /* settings: cap on the host's framerate */
    int kbps;            /* settings: cap on the host's bitrate, scaled to resolution */
} UIResult;

/* Fonts are owned by the UI, sized from the real render output, and re-sized if
 * the output mode changes. Call once after renderer creation; screens call it
 * every frame. Returns false when no usable TTF exists. */
bool UIEnsureFonts(SDL_Renderer *renderer);
void UICloseFonts(void);

/* Runs the launcher. Uses the shared client config for LAN discovery.
 * Returns UI_START (out->host filled) or UI_QUIT. */
UIAction RunMenu(SDL_Window *window, SDL_Renderer *renderer,
                 const IHS_ClientConfig *config, UIResult *out);

/* Full-screen pairing prompt: shows a PIN to type into Steam on the host and
 * waits for approval. Returns true once paired, false on cancel/failure. */
bool PairScreen(SDL_Renderer *renderer, const IHS_ClientConfig *config,
                const IHS_HostInfo *host);
