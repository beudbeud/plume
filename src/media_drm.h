/* Zero-copy presentation of DRM_PRIME frames: import the decoder's dmabuf
 * planes as EGLImages bound to GL textures, wrapped in an SDL NV12 texture —
 * no readback, no upload. Every precondition is probed at runtime (EGL-backed
 * GL renderer, dma_buf import extension, acceptable format/modifier); when any
 * is missing the caller falls back to the readback path, so this is always an
 * optimisation, never a requirement. */
#pragma once

#include <SDL3/SDL.h>
#include <libavutil/frame.h>

/* Returns an SDL texture showing `f` (owned here, valid until the next call or
 * DrmPrimeReset), or NULL when this frame can't be imported. The caller must
 * keep `f` referenced while the texture is in use: the GPU reads the dmabuf. */
SDL_Texture *DrmPrimeToTexture(SDL_Renderer *r, const AVFrame *f);

/* Drop the cached GL/SDL objects (call from MediaDetach, renderer still live). */
void DrmPrimeReset(void);

/* ---- DRM video plane (KMSDRM only) ----
 * The GPU sampler can't read the Pi's SAND tiling, but the display controller
 * (HVS) scans it natively: put the frame on a DRM overlay plane above SDL's
 * primary plane and skip GPU, readback and upload entirely. Requires the
 * kmsdrm video driver (we borrow SDL's DRM fd, which holds master). */

/* Scan `f` out on the video plane, scaled per `scale` into an outW x outH
 * mode. Returns false when this stack can't (not kmsdrm, no NV12 overlay
 * plane, framebuffer import rejected) — the caller falls back. */
bool DrmPlanePresent(SDL_Window *win, const AVFrame *f, int outW, int outH, int scale);

/* Turn the plane off but keep the probe and framebuffer cache (stats overlay
 * needs the GPU path while it is visible). No-op when not shown. */
void DrmPlaneHide(void);

/* Disable the plane and free framebuffers + GEM handles (MediaDetach). */
void DrmPlaneReset(void);
