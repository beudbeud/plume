/* Video + audio sinks for the IHSlib session: FFmpeg decode -> SDL3 present.
 * The SDL window/renderer are owned by main.c and shared with the UI; media
 * only borrows them. */
#pragma once

#include <SDL3/SDL.h>
#include "ihslib.h"

/* Callback tables handed to IHS_SessionSetVideoCallbacks / ...Audio... */
extern const IHS_StreamVideoCallbacks VideoCallbacks;
extern const IHS_StreamAudioCallbacks AudioCallbacks;

/* How to map a stream onto a display of a different aspect ratio. */
typedef enum {
    MEDIA_SCALE_FIT,     /* whole picture, black bars */
    MEDIA_SCALE_STRETCH, /* fills, distorts */
    MEDIA_SCALE_CROP,    /* fills, no distortion, edges cut off */
} MediaScale;

/* Borrow the shared window/renderer for one streaming session. */
void MediaAttach(SDL_Window *window, SDL_Renderer *renderer, bool enableAudio, MediaScale scale);
void MediaDetach(void);

/* Present the most-recently decoded frame. Call from the main thread. */
void MediaPresent(void);
