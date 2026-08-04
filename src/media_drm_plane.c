/* DRM video plane presentation — see media_drm.h. The plane sits above SDL's
 * primary plane (best-effort zpos), so the stats overlay is invisible while it
 * runs; media.c routes through the GPU path when the overlay is on.
 *
 * Framebuffers are cached per GEM handle: the decoder's pool is a fixed ring
 * of buffers, drmPrimeFDToHandle returns the same handle for the same BO, and
 * closing a handle per-frame would yank it from under the FB of a pool
 * neighbour that shares it. Everything is freed in DrmPlaneReset.
 *
 * Every step degrades: any failure returns false once, logs why, and the
 * caller's fallback chain (EGL import -> readback) takes over for good. */
#include "media_drm.h"

#include <string.h>
#include <libavutil/frame.h>

#ifndef PLUME_HAVE_LIBDRM

bool DrmPlanePresent(SDL_Window *win, const AVFrame *f, int outW, int outH, int scale) {
    (void) win; (void) f; (void) outW; (void) outH; (void) scale;
    return false;
}
void DrmPlaneHide(void) {}
void DrmPlaneReset(void) {}

#else

#include <inttypes.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <libavutil/hwcontext_drm.h>
#include "media.h" /* MediaScale */

static int probed;     /* 0 = not yet, 1 = usable, -1 = not on this stack */
static int fd = -1;    /* SDL's DRM fd — it holds master, we ride along */
static uint32_t planeId, crtcId;
static bool visible;

static struct { uint32_t handle; uint32_t fb; } fbCache[16];
static int fbCount;
/* The plane scans a frame until the flip after the next one lands; hold the
 * last two so their buffers can't return to the decoder pool mid-scanout. */
static AVFrame *held[2];
static int heldIdx;

static uint32_t PlanePropId(uint32_t plane, const char *name, uint64_t *max) {
    uint32_t id = 0;
    drmModeObjectProperties *props = drmModeObjectGetProperties(fd, plane, DRM_MODE_OBJECT_PLANE);
    if (!props) return 0;
    for (uint32_t i = 0; i < props->count_props && !id; i++) {
        drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
        if (!p) continue;
        if (strcmp(p->name, name) == 0) {
            id = p->prop_id;
            if (max) *max = p->count_values > 1 ? (uint64_t) p->values[1] : props->prop_values[i];
        }
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
    return id;
}

static uint64_t PlanePropValue(uint32_t plane, const char *name, uint64_t fallback) {
    uint64_t v = fallback;
    drmModeObjectProperties *props = drmModeObjectGetProperties(fd, plane, DRM_MODE_OBJECT_PLANE);
    if (!props) return v;
    for (uint32_t i = 0; i < props->count_props; i++) {
        drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
        if (!p) continue;
        if (strcmp(p->name, name) == 0) v = props->prop_values[i];
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
    return v;
}

static bool Probe(SDL_Window *win) {
    const char *driver = SDL_GetCurrentVideoDriver();
    if (!driver || strcmp(driver, "kmsdrm") != 0) {
        SDL_Log("drm-plane: video driver '%s' is not kmsdrm, no plane path", driver ? driver : "?");
        return false;
    }
    fd = (int) SDL_GetNumberProperty(SDL_GetWindowProperties(win),
                                     SDL_PROP_WINDOW_KMSDRM_DRM_FD_NUMBER, -1);
    if (fd < 0) {
        SDL_Log("drm-plane: SDL exposed no DRM fd, no plane path");
        return false;
    }
    if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0)
        SDL_Log("drm-plane: universal planes cap refused (legacy plane list)");

    /* The active CRTC is the one SDL is flipping. */
    int crtcIndex = -1;
    drmModeRes *res = drmModeGetResources(fd);
    if (!res) return false;
    for (int i = 0; i < res->count_crtcs && crtcIndex < 0; i++) {
        drmModeCrtc *c = drmModeGetCrtc(fd, res->crtcs[i]);
        if (c) {
            if (c->mode_valid) { crtcId = c->crtc_id; crtcIndex = i; }
            drmModeFreeCrtc(c);
        }
    }
    drmModeFreeResources(res);
    if (crtcIndex < 0) {
        SDL_Log("drm-plane: no active CRTC found");
        return false;
    }

    /* First free overlay plane on that CRTC that speaks NV12. A plane with no
     * "type" property comes from the legacy list, which only ever contains
     * overlays — treat it as one instead of rejecting it. */
    drmModePlaneRes *pr = drmModeGetPlaneResources(fd);
    if (!pr) return false;
    for (uint32_t i = 0; i < pr->count_planes && !planeId; i++) {
        drmModePlane *p = drmModeGetPlane(fd, pr->planes[i]);
        if (!p) continue;
        bool onCrtc = (p->possible_crtcs & (1u << crtcIndex)) != 0;
        bool inUse = p->fb_id != 0; /* SDL's primary, or someone else's */
        bool nv12 = false;
        for (uint32_t j = 0; j < p->count_formats; j++) {
            if (p->formats[j] == DRM_FORMAT_NV12) nv12 = true;
        }
        uint64_t type = PlanePropValue(pr->planes[i], "type", UINT64_MAX);
        bool overlay = type == DRM_PLANE_TYPE_OVERLAY || type == UINT64_MAX;
        if (onCrtc && !inUse && nv12 && overlay) {
            planeId = pr->planes[i];
        } else {
            /* One line per rejected plane: this is the whole diagnosis when a
             * board refuses the fast path, and it only shows under --verbose. */
            SDL_Log("drm-plane: plane %u skipped (type=%d crtcs=0x%x fb=%u nv12=%d)",
                    pr->planes[i], type == UINT64_MAX ? -1 : (int) type,
                    p->possible_crtcs, p->fb_id, nv12);
        }
        drmModeFreePlane(p);
    }
    drmModeFreePlaneResources(pr);
    if (!planeId) {
        SDL_Log("drm-plane: no usable plane on CRTC %u (index %d), no plane path", crtcId, crtcIndex);
        return false;
    }

    /* Above the primary, so the video is visible over SDL's cleared frame.
     * Best effort: on vc4 overlays already sit above by default. */
    uint64_t zmax = 0;
    uint32_t zprop = PlanePropId(planeId, "zpos", &zmax);
    if (zprop) drmModeObjectSetProperty(fd, planeId, DRM_MODE_OBJECT_PLANE, zprop, zmax);

    SDL_Log("drm-plane: using plane %u on CRTC %u", planeId, crtcId);
    return true;
}

static uint32_t FbForFrame(const AVFrame *f) {
    const AVDRMFrameDescriptor *d = (const AVDRMFrameDescriptor *) f->data[0];
    uint32_t objHandle[AV_DRM_MAX_PLANES] = {0};
    for (int o = 0; o < d->nb_objects; o++) {
        if (drmPrimeFDToHandle(fd, d->objects[o].fd, &objHandle[o]) != 0) return 0;
    }
    for (int i = 0; i < fbCount; i++) {
        if (fbCache[i].handle == objHandle[0]) return fbCache[i].fb;
    }
    if (fbCount == (int) SDL_arraysize(fbCache)) return 0; /* pool bigger than expected */

    uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
    uint64_t mods[4] = {0};
    int np = 0;
    bool modifiers = false;
    for (int l = 0; l < d->nb_layers; l++) {
        for (int p = 0; p < d->layers[l].nb_planes && np < 4; p++) {
            int obj = d->layers[l].planes[p].object_index;
            handles[np] = objHandle[obj];
            pitches[np] = (uint32_t) d->layers[l].planes[p].pitch;
            offsets[np] = (uint32_t) d->layers[l].planes[p].offset;
            mods[np] = d->objects[obj].format_modifier;
            if (mods[np] != DRM_FORMAT_MOD_INVALID && mods[np] != DRM_FORMAT_MOD_LINEAR)
                modifiers = true;
            np++;
        }
    }
    uint32_t fb = 0;
    int err = drmModeAddFB2WithModifiers(fd, (uint32_t) f->width, (uint32_t) f->height,
                                         d->layers[0].format, handles, pitches, offsets,
                                         modifiers ? mods : NULL, &fb,
                                         modifiers ? DRM_MODE_FB_MODIFIERS : 0);
    if (err != 0) {
        SDL_Log("drm-plane: AddFB2 rejected (%d, modifier 0x%" PRIx64 ")", err, mods[0]);
        return 0;
    }
    fbCache[fbCount].handle = objHandle[0];
    fbCache[fbCount].fb = fb;
    fbCount++;
    return fb;
}

bool DrmPlanePresent(SDL_Window *win, const AVFrame *f, int outW, int outH, int scale) {
    if (probed == 0) probed = Probe(win) ? 1 : -1;
    if (probed < 0) return false;

    uint32_t fb = FbForFrame(f);
    if (!fb) { probed = -1; return false; }

    /* Fit/stretch place the full frame in a dst rect; crop keeps dst full
     * screen and trims the source instead — the HVS scales either way. */
    int32_t dx = 0, dy = 0;
    uint32_t dw = (uint32_t) outW, dh = (uint32_t) outH;
    uint32_t sx = 0, sy = 0, sw = (uint32_t) f->width, sh = (uint32_t) f->height;
    if (scale == MEDIA_SCALE_FIT) {
        float s = SDL_min((float) outW / f->width, (float) outH / f->height);
        dw = (uint32_t) (f->width * s);
        dh = (uint32_t) (f->height * s);
        dx = (int32_t) ((outW - (int) dw) / 2);
        dy = (int32_t) ((outH - (int) dh) / 2);
    } else if (scale == MEDIA_SCALE_CROP) {
        float s = SDL_max((float) outW / f->width, (float) outH / f->height);
        sw = (uint32_t) (outW / s) & ~1u;
        sh = (uint32_t) (outH / s) & ~1u;
        sx = ((uint32_t) f->width - sw) / 2;
        sy = ((uint32_t) f->height - sh) / 2;
    }
    if (drmModeSetPlane(fd, planeId, crtcId, fb, 0, dx, dy, dw, dh,
                        sx << 16, sy << 16, sw << 16, sh << 16) != 0) {
        SDL_Log("drm-plane: SetPlane failed, falling back");
        probed = -1;
        return false;
    }
    if (!visible) SDL_Log("drm-plane: zero-copy scanout active (%dx%d)", f->width, f->height);
    visible = true;

    if (!held[heldIdx]) held[heldIdx] = av_frame_alloc();
    if (held[heldIdx]) {
        av_frame_unref(held[heldIdx]);
        av_frame_ref(held[heldIdx], f);
    }
    heldIdx ^= 1;
    return true;
}

void DrmPlaneHide(void) {
    if (!visible) return;
    drmModeSetPlane(fd, planeId, crtcId, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    visible = false;
}

void DrmPlaneReset(void) {
    DrmPlaneHide();
    for (int i = 0; i < fbCount; i++) {
        drmModeRmFB(fd, fbCache[i].fb);
        drmCloseBufferHandle(fd, fbCache[i].handle);
    }
    fbCount = 0;
    av_frame_free(&held[0]);
    av_frame_free(&held[1]);
    heldIdx = 0;
    /* Keep probe results: the fd, CRTC and plane outlive sessions. */
}

#endif /* PLUME_HAVE_LIBDRM */
