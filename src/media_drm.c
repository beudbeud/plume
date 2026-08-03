/* See media_drm.h. The Y plane imports as an R8 texture and the UV plane as
 * GR88 (byte0 = U -> .r, byte1 = V -> .g, same mapping mpv uses), which is
 * exactly what SDL's own NV12 shader samples — so a wrapped SDL texture renders
 * them with no shader work on our side.
 *
 * Everything is resolved through SDL_EGL_GetProcAddress / SDL_GL_GetProcAddress
 * at runtime, and the handful of EGL/GL constants are declared locally (they
 * are frozen by the Khronos specs), so this file adds no build dependency: on
 * a GLX or Vulkan-backed renderer the probe just fails and readback runs. */
#include "media_drm.h"

#include <string.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixfmt.h>

/* ---- minimal EGL/GLES declarations (Khronos-spec constants, frozen) ---- */
typedef void *EGLDisplay_, *EGLImage_;
typedef int32_t EGLint_;
typedef intptr_t EGLAttrib_;
typedef unsigned GLuint_;
typedef unsigned GLenum_;
typedef int GLint_;
typedef int GLsizei_;

#define EGL_NO_IMAGE            ((EGLImage_) 0)
#define EGL_EXTENSIONS_         0x3055
#define EGL_LINUX_DMA_BUF_EXT   0x3270
#define EGL_LINUX_DRM_FOURCC_EXT 0x3271
#define EGL_DMA_BUF_PLANE0_FD_EXT     0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT  0x3274
#define EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT 0x3443
#define EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT 0x3444
#define EGL_WIDTH_              0x3057
#define EGL_HEIGHT_             0x3056
#define GL_TEXTURE_2D_          0x0DE1
#define GL_TEXTURE_MIN_FILTER_  0x2801
#define GL_TEXTURE_MAG_FILTER_  0x2800
#define GL_TEXTURE_WRAP_S_      0x2802
#define GL_TEXTURE_WRAP_T_      0x2803
#define GL_LINEAR_              0x2601
#define GL_CLAMP_TO_EDGE_       0x812F

#define DRM_FOURCC(a, b, c, d) ((uint32_t) (a) | ((uint32_t) (b) << 8) | \
                                ((uint32_t) (c) << 16) | ((uint32_t) (d) << 24))
#define FOURCC_R8   DRM_FOURCC('R', '8', ' ', ' ')
#define FOURCC_GR88 DRM_FOURCC('G', 'R', '8', '8')
#define DRM_MOD_INVALID ((uint64_t) 0x00ffffffffffffffULL)
#define DRM_MOD_LINEAR  ((uint64_t) 0)

static const char *(*pQueryString)(EGLDisplay_, EGLint_);
static EGLImage_ (*pCreateImage)(EGLDisplay_, void *ctx, unsigned target, void *buf, const EGLint_ *attribs);
static unsigned (*pDestroyImage)(EGLDisplay_, EGLImage_);
static void (*pGenTextures)(GLsizei_, GLuint_ *);
static void (*pBindTexture)(GLenum_, GLuint_);
static void (*pTexParameteri)(GLenum_, GLenum_, GLint_);
static void (*pImageTargetTexture)(GLenum_, EGLImage_); /* glEGLImageTargetTexture2DOES */

static int probed;         /* 0 = not yet, 1 = usable, -1 = not on this stack */
static bool haveModifiers; /* EGL_EXT_image_dma_buf_import_modifiers */
static bool isGles;        /* which SDL wrap property to use */
static EGLDisplay_ dpy;
static GLuint_ texY, texUV;
static SDL_Texture *wrapper;
static int wrapW, wrapH;

static bool Probe(SDL_Renderer *r) {
    const char *name = SDL_GetRendererName(r);
    if (!name || (strcmp(name, "opengl") != 0 && strcmp(name, "opengles2") != 0)) {
        SDL_Log("zero-copy: renderer '%s' is not GL, using readback", name ? name : "?");
        return false;
    }
    isGles = strcmp(name, "opengles2") == 0;
    dpy = SDL_EGL_GetCurrentDisplay();
    if (!dpy) { /* GLX on X11 lands here */
        SDL_Log("zero-copy: no EGL display (GLX?), using readback");
        return false;
    }
    pQueryString = (const char *(*)(EGLDisplay_, EGLint_)) SDL_EGL_GetProcAddress("eglQueryString");
    const char *ext = pQueryString ? pQueryString(dpy, EGL_EXTENSIONS_) : NULL;
    if (!ext || !strstr(ext, "EGL_EXT_image_dma_buf_import")) {
        SDL_Log("zero-copy: EGL_EXT_image_dma_buf_import missing, using readback");
        return false;
    }
    haveModifiers = strstr(ext, "EGL_EXT_image_dma_buf_import_modifiers") != NULL;
    pCreateImage = (void *) SDL_EGL_GetProcAddress("eglCreateImageKHR");
    pDestroyImage = (void *) SDL_EGL_GetProcAddress("eglDestroyImageKHR");
    pImageTargetTexture = (void *) SDL_GL_GetProcAddress("glEGLImageTargetTexture2DOES");
    pGenTextures = (void *) SDL_GL_GetProcAddress("glGenTextures");
    pBindTexture = (void *) SDL_GL_GetProcAddress("glBindTexture");
    pTexParameteri = (void *) SDL_GL_GetProcAddress("glTexParameteri");
    if (!pCreateImage || !pDestroyImage || !pImageTargetTexture ||
        !pGenTextures || !pBindTexture || !pTexParameteri) {
        SDL_Log("zero-copy: EGL/GL entry points missing, using readback");
        return false;
    }
    pGenTextures(1, &texY);
    pGenTextures(1, &texUV);
    return true;
}

/* One plane -> one EGLImage bound to `tex`. */
static bool ImportPlane(GLuint_ tex, uint32_t fourcc, int w, int h,
                        int fd, int offset, int pitch, uint64_t modifier) {
    EGLint_ attribs[32];
    int n = 0;
    #define A(k, v) do { attribs[n++] = (EGLint_) (k); attribs[n++] = (EGLint_) (v); } while (0)
    A(EGL_WIDTH_, w);
    A(EGL_HEIGHT_, h);
    A(EGL_LINUX_DRM_FOURCC_EXT, fourcc);
    A(EGL_DMA_BUF_PLANE0_FD_EXT, fd);
    A(EGL_DMA_BUF_PLANE0_OFFSET_EXT, offset);
    A(EGL_DMA_BUF_PLANE0_PITCH_EXT, pitch);
    if (modifier != DRM_MOD_INVALID && modifier != DRM_MOD_LINEAR) {
        /* A tiled buffer (e.g. the Pi's SAND layout) can only be described with
         * the modifiers extension; without it the import would be a lie. */
        if (!haveModifiers) return false;
        A(EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (uint32_t) modifier);
        A(EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (uint32_t) (modifier >> 32));
    }
    #undef A
    attribs[n] = 0x3038; /* EGL_NONE */
    EGLImage_ img = pCreateImage(dpy, NULL, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
    if (img == EGL_NO_IMAGE) return false;
    pBindTexture(GL_TEXTURE_2D_, tex);
    pImageTargetTexture(GL_TEXTURE_2D_, img);
    pTexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MIN_FILTER_, GL_LINEAR_);
    pTexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MAG_FILTER_, GL_LINEAR_);
    pTexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_WRAP_S_, GL_CLAMP_TO_EDGE_);
    pTexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_WRAP_T_, GL_CLAMP_TO_EDGE_);
    /* The texture now holds its own reference to the buffer storage. */
    pDestroyImage(dpy, img);
    return true;
}

SDL_Texture *DrmPrimeToTexture(SDL_Renderer *r, const AVFrame *f) {
    if (probed == 0) probed = Probe(r) ? 1 : -1;
    if (probed < 0) return NULL;

    const AVDRMFrameDescriptor *d = (const AVDRMFrameDescriptor *) f->data[0];
    /* NV12 arrives either as one layer of two planes or two single-plane
     * layers; flatten to (Y, UV). Anything else is not our NV12. */
    struct { int obj, offset, pitch; } pl[2];
    int np = 0;
    for (int l = 0; l < d->nb_layers && np < 2; l++) {
        for (int p = 0; p < d->layers[l].nb_planes && np < 2; p++) {
            pl[np].obj = d->layers[l].planes[p].object_index;
            pl[np].offset = (int) d->layers[l].planes[p].offset;
            pl[np].pitch = (int) d->layers[l].planes[p].pitch;
            np++;
        }
    }
    if (np != 2) return NULL;

    if (!ImportPlane(texY, FOURCC_R8, f->width, f->height,
                     d->objects[pl[0].obj].fd, pl[0].offset, pl[0].pitch,
                     d->objects[pl[0].obj].format_modifier) ||
        !ImportPlane(texUV, FOURCC_GR88, f->width / 2, f->height / 2,
                     d->objects[pl[1].obj].fd, pl[1].offset, pl[1].pitch,
                     d->objects[pl[1].obj].format_modifier)) {
        /* Typically a modifier the GPU can't sample (tiled SAND on the Pi).
         * Fail permanently: retrying per frame would just burn CPU. */
        SDL_Log("zero-copy: dmabuf import rejected (modifier?), using readback");
        probed = -1;
        return NULL;
    }

    if (!wrapper || wrapW != f->width || wrapH != f->height) {
        if (wrapper) SDL_DestroyTexture(wrapper);
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_NV12);
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STATIC);
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, f->width);
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, f->height);
        SDL_SetNumberProperty(props, isGles ? SDL_PROP_TEXTURE_CREATE_OPENGLES2_TEXTURE_NUMBER
                                            : SDL_PROP_TEXTURE_CREATE_OPENGL_TEXTURE_NUMBER, texY);
        SDL_SetNumberProperty(props, isGles ? SDL_PROP_TEXTURE_CREATE_OPENGLES2_TEXTURE_UV_NUMBER
                                            : SDL_PROP_TEXTURE_CREATE_OPENGL_TEXTURE_UV_NUMBER, texUV);
        wrapper = SDL_CreateTextureWithProperties(r, props);
        SDL_DestroyProperties(props);
        if (!wrapper) {
            SDL_Log("zero-copy: SDL texture wrap failed (%s), using readback", SDL_GetError());
            probed = -1;
            return NULL;
        }
        wrapW = f->width;
        wrapH = f->height;
        SDL_Log("zero-copy: dmabuf import active (%dx%d)", wrapW, wrapH);
    }
    return wrapper;
}

void DrmPrimeReset(void) {
    if (wrapper) { SDL_DestroyTexture(wrapper); wrapper = NULL; }
    wrapW = wrapH = 0;
    /* texY/texUV stay allocated: the renderer (and its GL context) outlives
     * sessions, and the next import rebinds them. */
}
