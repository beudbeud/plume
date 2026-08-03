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
