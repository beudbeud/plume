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

/* Borrow the shared window/renderer for one streaming session. Both NULL puts
 * media in headless mode: no window, no audio device, and the frames and samples
 * are pulled instead of presented (that's what the libretro core does). */
void MediaAttach(SDL_Window *window, SDL_Renderer *renderer, bool enableAudio, MediaScale scale);
void MediaDetach(void);

/* Present the most-recently decoded frame. Call from the main thread. */
void MediaPresent(void);

/* ---- headless mode only ---- */

/* Convert the newest decoded frame into `pixels` as XRGB8888, `pitch` bytes per
 * row, at most `maxHeight` rows. False when nothing new was decoded since the
 * last call (dup the frame), or when the frame does not fit. */
bool MediaPullVideo(void *pixels, int pitch, int maxHeight, int *outW, int *outH);

/* Drain up to `maxFrames` stereo S16 frames. Returns frames written. */
int MediaPullAudio(int16_t *out, int maxFrames);
