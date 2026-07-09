/* FFmpeg (H264/HEVC + Opus) decode into an SDL3 window.
 *
 * IHSlib calls submit() on its own network thread; we decode there and latch a
 * reference to the newest frame under a mutex. MediaPresent() runs on the main
 * thread and uploads whatever is latest. Audio decodes straight to SDL's queue
 * (thread safe), so no locking needed for it.
 */
#include "media.h"

#include <string.h>
#include <SDL3/SDL.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>

/* ---- SDL window / renderer (borrowed from main.c) ---- */
static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static int texW, texH;
static SDL_PixelFormat texFmt;

/* ---- Video decode (network thread writes, main thread reads) ---- */
static AVCodecContext *vctx;
static AVBufferRef *hwDeviceCtx;              /* HW decode device (Pi5 HEVC via V4L2-request/DRM) */
static enum AVPixelFormat hwPixFmt = AV_PIX_FMT_NONE;
static struct SwsContext *sws;
static SDL_Mutex *frameLock;
static AVFrame *latched; /* newest decoded frame; a ref into the decoder's pool, no copy */
static bool frameDirty;
/* The host's adaptive bitrate loop runs on what we report per frame, so the latch
 * tracks which frame is waiting: replacing an unshown one is a real dropped frame. */
static IHS_Session *statsSession;
static uint16_t pendingFrameId;
static bool framePending;

static MediaScale scaleMode;

/* ---- Audio decode (network thread -> SDL queue) ---- */
static bool audioEnabled;
static SDL_AudioStream *audioStream;
static AVCodecContext *actx;
static SwrContext *swr;
static int audioChannels;

void MediaAttach(SDL_Window *w, SDL_Renderer *r, bool enableAudio, MediaScale scale) {
    window = w;
    renderer = r;
    audioEnabled = enableAudio;
    scaleMode = scale;
    statsSession = NULL;
    framePending = false;
    frameLock = SDL_CreateMutex();
    latched = av_frame_alloc();
    frameDirty = false;
}

void MediaDetach(void) {
    statsSession = NULL; /* the session is being torn down; never report into it */
    framePending = false;
    if (texture) { SDL_DestroyTexture(texture); texture = NULL; texW = texH = 0; }
    if (frameLock) { SDL_DestroyMutex(frameLock); frameLock = NULL; }
    av_frame_free(&latched);
    window = NULL;
    renderer = NULL;
}

/* ------------------------------- video -------------------------------- */

/* Tell FFmpeg to keep frames in the HW (DRM_PRIME) format when the hardware
 * decoder offers it; av_hwframe_transfer_data() pulls them down in VideoSubmit. */
static enum AVPixelFormat GetHwFormat(AVCodecContext *ctx, const enum AVPixelFormat *fmts) {
    (void) ctx;
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == hwPixFmt) return *p;
    }
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "hw pixel format unavailable, falling back to software output");
    return fmts[0];
}

static int VideoStart(IHS_Session *session, const IHS_StreamVideoConfig *config, void *context) {
    (void) session; (void) context;
    bool hevc = config->codec == IHS_StreamVideoCodecHEVC;
    enum AVCodecID id = hevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
    const AVCodec *sw = avcodec_find_decoder(id);
    if (!sw) { SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "no decoder for codec %d", config->codec); return -1; }

    /* Two shapes of hardware decode, and the Pi generations disagree on which:
     *
     *  - hwaccel bound to a device context, discovered from the codec. That's the
     *    Pi5's HEVC block (V4L2-request/DRM). It has no H264 block at all.
     *  - a whole standalone decoder, only findable by name. That's the VideoCore
     *    H264 block of the Pi3/Pi4, exposed as `h264_v4l2m2m`.
     *
     * Neither exists on a PC, where avcodec_find_decoder_by_name returns NULL and
     * the hwaccel loop finds nothing, so we fall through to software. */
    const AVCodec *hwCodec = NULL;
    enum AVHWDeviceType hwType = AV_HWDEVICE_TYPE_NONE;
    hwPixFmt = AV_PIX_FMT_NONE;
    if (hevc) {
        for (int i = 0;; i++) {
            const AVCodecHWConfig *hw = avcodec_get_hw_config(sw, i);
            if (!hw) break;
            if (hw->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) {
                hwCodec = sw;
                hwType = hw->device_type;
                hwPixFmt = hw->pix_fmt;
                break;
            }
        }
    } else {
        hwCodec = avcodec_find_decoder_by_name("h264_v4l2m2m");
    }
    /* A V4L2 decoder that opens and then decodes badly can't be told apart from a
     * good one at open time. Escape hatch, since the fallback is known to work. */
    if (SDL_getenv("PLUME_NO_HWDEC")) hwCodec = NULL;

    /* Attempt 0: hardware (if available). Attempt 1: software fallback. Opening
     * the V4L2 decoder fails on a Pi5 (no H264 device node) and on any kernel
     * without the driver, so the fallback is the load-bearing path, not a nicety. */
    for (int attempt = 0; attempt < 2; attempt++) {
        bool useHw = attempt == 0 && hwCodec != NULL;
        const AVCodec *codec = useHw ? hwCodec : sw;
        vctx = avcodec_alloc_context3(codec);
        /* h264_v4l2m2m sizes the driver's OUTPUT and CAPTURE queues at open time.
         * Left at zero it opens blind and has to renegotiate on the first packet,
         * which some kernels fail outright. Free for the software decoder, which
         * would have read the same numbers out of the first SPS. */
        vctx->width = (int) config->width;
        vctx->height = (int) config->height;
        vctx->thread_count = 0; /* auto: one thread per core */
        if (useHw) {
            /* Hardware decode is single-frame-latency anyway; ask for it explicitly. */
            vctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
        } else {
            /* AV_CODEC_FLAG_LOW_DELAY disables frame threading (libavcodec
             * validate_thread_parameters), and NVENC emits one slice per frame so
             * slice threading buys nothing — the flag would pin 1440p60 H264 to a
             * single core. Trade ~thread_count frames of latency for keeping up. */
            vctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        }
        if (useHw && hwType != AV_HWDEVICE_TYPE_NONE &&
            av_hwdevice_ctx_create(&hwDeviceCtx, hwType, NULL, NULL, 0) == 0) {
            vctx->hw_device_ctx = av_buffer_ref(hwDeviceCtx);
            vctx->get_format = GetHwFormat;
        }
        if (avcodec_open2(vctx, codec, NULL) == 0) {
            SDL_Log("video decoder: %s (%s)", codec->name, useHw ? "hardware" : "software");
            return 0;
        }
        avcodec_free_context(&vctx);
        if (hwDeviceCtx) av_buffer_unref(&hwDeviceCtx);
    }
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "no usable decoder for codec %d", config->codec);
    return -1;
}

/* Take ownership of the decoded frame (moves the ref out of src). The decoder's
 * buffers are refcounted, so holding one costs the pool a single extra frame —
 * against a full-frame memcpy per frame under the lock. */
static void StashFrame(AVFrame *src, uint16_t frameId) {
    SDL_LockMutex(frameLock);
    av_frame_unref(latched);
    av_frame_move_ref(latched, src);
    /* A frame still waiting when the next one lands was never shown. */
    bool hadPending = framePending;
    uint16_t dropped = pendingFrameId;
    pendingFrameId = frameId;
    framePending = true;
    frameDirty = true;
    SDL_UnlockMutex(frameLock);

    if (hadPending) {
        IHS_SessionReportVideoFrameComplete(statsSession, dropped, IHS_VideoFrameResultDroppedLate);
    }
}

static IHS_StreamVideoSubmitResult VideoSubmit(IHS_Session *session, uint16_t frameId, IHS_Buffer *data,
                                               IHS_StreamVideoFrameFlag flags, void *context) {
    (void) context;
    statsSession = session;
    /* After dropped frames the decoder holds stale references and rejects every
     * later slice ("Frame num change"). A keyframe is a clean restart point. */
    if (flags & IHS_StreamVideoFrameKeyFrame) {
        avcodec_flush_buffers(vctx);
    }
    /* av_new_packet allocates AV_INPUT_BUFFER_PADDING_SIZE zeroed bytes past the
     * end — the bitstream reader over-reads, and IHS_Buffer has no such padding. */
    AVPacket *pkt = av_packet_alloc();
    if (av_new_packet(pkt, (int) data->size) < 0) {
        av_packet_free(&pkt);
        IHS_SessionReportVideoFrameComplete(session, frameId, IHS_VideoFrameResultDroppedNetworkLost);
        return IHS_StreamVideoSubmitReportLost;
    }
    memcpy(pkt->data, IHS_BufferPointer(data), data->size);
    IHS_StreamVideoSubmitResult ret = IHS_StreamVideoSubmitOK;
    Uint64 t0 = SDL_GetTicksNS();
    IHS_SessionReportVideoFrameStage(session, frameId, IHS_VideoFrameStageDecodeBegin, 0);
    if (avcodec_send_packet(vctx, pkt) == 0) {
        AVFrame *f = av_frame_alloc();
        while (avcodec_receive_frame(vctx, f) == 0) {
            /* If a HW decoder handed back GPU frames, pull them into system
             * memory (offloads decode; pays one readback — still far cheaper
             * than software HEVC). Most Pi v4l2m2m builds return NV12 directly,
             * so this branch is usually skipped. */
            AVFrame *sw = f, *dl = NULL;
            if (f->hw_frames_ctx) {
                dl = av_frame_alloc();
                if (av_hwframe_transfer_data(dl, f, 0) == 0) sw = dl;
            }
            /* YUV420P and NV12 upload as-is (IYUV/NV12 textures); anything else
             * goes via swscale. */
            if (sw->format == AV_PIX_FMT_YUV420P || sw->format == AV_PIX_FMT_NV12) {
                StashFrame(sw, frameId);
            } else {
                sws = sws_getCachedContext(sws, sw->width, sw->height, sw->format,
                                           sw->width, sw->height, AV_PIX_FMT_YUV420P,
                                           SWS_BILINEAR, NULL, NULL, NULL);
                AVFrame *o = av_frame_alloc();
                o->format = AV_PIX_FMT_YUV420P; o->width = sw->width; o->height = sw->height;
                av_frame_get_buffer(o, 32);
                sws_scale(sws, (const uint8_t *const *) sw->data, sw->linesize, 0, sw->height,
                          o->data, o->linesize);
                StashFrame(o, frameId);
                av_frame_free(&o);
            }
            av_frame_free(&dl);
        }
        av_frame_free(&f);
    } else {
        ret = IHS_StreamVideoSubmitReportLost;
        IHS_SessionReportVideoFrameComplete(session, frameId, IHS_VideoFrameResultDroppedDecodeCorrupt);
    }
    IHS_SessionReportVideoFrameStage(session, frameId, IHS_VideoFrameStageDecodeEnd, 0);
    av_packet_free(&pkt);

    /* Decode runs inline on the session worker thread, so >16ms here starves
     * audio too and overflows the video ring. Report the average periodically. */
    static Uint64 accNs; static int accN;
    accNs += SDL_GetTicksNS() - t0;
    if (++accN == 120) {
        SDL_Log("decode: %.1f ms/frame avg", (double) accNs / accN / 1e6);
        accNs = 0; accN = 0;
    }
    return ret;
}

static void VideoStop(IHS_Session *session, void *context) {
    (void) session; (void) context;
    if (sws) { sws_freeContext(sws); sws = NULL; }
    if (vctx) { avcodec_free_context(&vctx); }
    if (hwDeviceCtx) { av_buffer_unref(&hwDeviceCtx); }
}

void MediaPresent(void) {
    uint16_t shownId = 0;
    bool shown = false;
    SDL_LockMutex(frameLock);
    if (frameDirty && latched && latched->width > 0) {
        SDL_PixelFormat fmt = latched->format == AV_PIX_FMT_NV12 ? SDL_PIXELFORMAT_NV12
                                                                 : SDL_PIXELFORMAT_IYUV;
        if (!texture || texW != latched->width || texH != latched->height || texFmt != fmt) {
            if (texture) SDL_DestroyTexture(texture);
            texture = SDL_CreateTexture(renderer, fmt, SDL_TEXTUREACCESS_STREAMING,
                                        latched->width, latched->height);
            texW = latched->width; texH = latched->height; texFmt = fmt;
        }
        if (framePending) IHS_SessionReportVideoFrameStage(statsSession, pendingFrameId,
                                                           IHS_VideoFrameStageUploadBegin, 0);
        if (fmt == SDL_PIXELFORMAT_NV12) {
            SDL_UpdateNVTexture(texture, NULL,
                                latched->data[0], latched->linesize[0],
                                latched->data[1], latched->linesize[1]);
        } else {
            SDL_UpdateYUVTexture(texture, NULL,
                                 latched->data[0], latched->linesize[0],
                                 latched->data[1], latched->linesize[1],
                                 latched->data[2], latched->linesize[2]);
        }
        frameDirty = false;
        shown = framePending;
        shownId = pendingFrameId;
        framePending = false;
    }
    SDL_UnlockMutex(frameLock);

    if (shown) IHS_SessionReportVideoFrameStage(statsSession, shownId, IHS_VideoFrameStageUploadEnd, 0);

    SDL_RenderClear(renderer);
    if (texture) {
        if (scaleMode == MEDIA_SCALE_STRETCH) {
            SDL_RenderTexture(renderer, texture, NULL, NULL);
        } else {
            /* The host always streams its own aspect ratio, so a 16:9 desktop on a
             * 4:3 display needs bars (fit) or lost edges (crop). min() leaves bars,
             * max() overflows the screen and SDL clips the excess. */
            int ow, oh;
            SDL_GetRenderOutputSize(renderer, &ow, &oh);
            float sx = (float) ow / texW, sy = (float) oh / texH;
            float scale = scaleMode == MEDIA_SCALE_CROP ? SDL_max(sx, sy) : SDL_min(sx, sy);
            SDL_FRect dst = {.w = texW * scale, .h = texH * scale};
            dst.x = (ow - dst.w) / 2;
            dst.y = (oh - dst.h) / 2;
            SDL_RenderTexture(renderer, texture, NULL, &dst);
        }
    }
    SDL_RenderPresent(renderer);

    /* Reported after present: this is the cursor the 1 Hz stats flush walks, and
     * it's what tells the host our end of the pipeline kept up. */
    if (shown) IHS_SessionReportVideoFrameComplete(statsSession, shownId, IHS_VideoFrameResultDisplayed);
}

const IHS_StreamVideoCallbacks VideoCallbacks = {
        .start = VideoStart,
        .submit = VideoSubmit,
        .stop = VideoStop,
};

/* ------------------------------- audio -------------------------------- */

static int AudioStart(IHS_Session *session, const IHS_StreamAudioConfig *config, void *context) {
    (void) session; (void) context;
    if (!audioEnabled) return 0;
    if (config->codec != IHS_StreamAudioCodecOpus) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "unsupported audio codec %d, muting", config->codec);
        audioEnabled = false;
        return 0;
    }
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
    if (!codec) { audioEnabled = false; return 0; }
    actx = avcodec_alloc_context3(codec);
    actx->sample_rate = (int) config->frequency;
    actx->ch_layout.order = AV_CHANNEL_ORDER_UNSPEC;
    actx->ch_layout.nb_channels = (int) config->channels;
    if (avcodec_open2(actx, codec, NULL) < 0) { audioEnabled = false; return 0; }
    audioChannels = (int) config->channels;

    swr = swr_alloc();
    av_opt_set_chlayout(swr, "in_chlayout", &actx->ch_layout, 0);
    av_opt_set_chlayout(swr, "out_chlayout", &actx->ch_layout, 0);
    av_opt_set_int(swr, "in_sample_rate", actx->sample_rate, 0);
    av_opt_set_int(swr, "out_sample_rate", actx->sample_rate, 0);
    av_opt_set_sample_fmt(swr, "in_sample_fmt", AV_SAMPLE_FMT_FLTP, 0);
    av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
    swr_init(swr);

    SDL_AudioSpec want = {SDL_AUDIO_S16, audioChannels, actx->sample_rate};
    audioStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want, NULL, NULL);
    if (!audioStream) { audioEnabled = false; return 0; }
    SDL_ResumeAudioStreamDevice(audioStream);
    return 0;
}

static int AudioSubmit(IHS_Session *session, IHS_Buffer *data, void *context) {
    (void) session; (void) context;
    if (!audioEnabled) return 0;
    AVPacket *pkt = av_packet_alloc();
    if (av_new_packet(pkt, (int) data->size) < 0) { /* needs FFmpeg's zero padding, see VideoSubmit */
        av_packet_free(&pkt);
        return 0;
    }
    memcpy(pkt->data, IHS_BufferPointer(data), data->size);
    if (avcodec_send_packet(actx, pkt) == 0) {
        AVFrame *f = av_frame_alloc();
        while (avcodec_receive_frame(actx, f) == 0) {
            /* Opus caps at 120 ms per packet: 5760 samples/channel at 48 kHz. */
            static int16_t pcm[5760 * 2];
            uint8_t *out = (uint8_t *) pcm;
            int cap = (int) (sizeof(pcm) / sizeof(pcm[0])) / audioChannels;
            int n = swr_convert(swr, &out, cap, (const uint8_t **) f->extended_data, f->nb_samples);
            if (n > 0)
                SDL_PutAudioStreamData(audioStream, pcm, n * audioChannels * (int) sizeof(int16_t));
        }
        av_frame_free(&f);
    }
    av_packet_free(&pkt);
    return 0;
}

static void AudioStop(IHS_Session *session, void *context) {
    (void) session; (void) context;
    if (audioStream) { SDL_DestroyAudioStream(audioStream); audioStream = NULL; }
    if (swr) { swr_free(&swr); }
    if (actx) { avcodec_free_context(&actx); }
}

const IHS_StreamAudioCallbacks AudioCallbacks = {
        .start = AudioStart,
        .submit = AudioSubmit,
        .stop = AudioStop,
};
