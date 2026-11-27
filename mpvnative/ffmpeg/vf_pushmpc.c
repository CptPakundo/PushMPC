/*
 * PushMPC video enhancement filter (frame generation, upscaling) for libavfilter.
 * Copyright (c) 2026 PushMPC contributors
 *
 * This file is part of FFmpeg as patched by PushMPC. It is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free Software Foundation, version 3 or later
 * (the engines it loads are GPL-3). See the PushMPC repository for the full licence text.
 *
 * vf_pushmpc: a thin libavfilter front end over the PushMPC engine module (pmeng.h), loaded at run time.
 *   - hardware frames in and out: D3D11 (zero copy into pushcore's D3D11 ingress; the output is a GPU texture of the
 *     same device), Vulkan (handed to the module when it takes Vulkan images; otherwise the copy fallback), CUDA
 *     (Linux, hwdec=nvdec: NVIDIA's engine copies device to device in and out; otherwise the copy fallback);
 *   - the copy fallback: when the module cannot take the input memory (no pushcore, no Vulkan engine) the filter
 *     downloads the frame, runs the module's CPU engine and uploads the result into a pool of the input's kind, so the
 *     graph's formats never change;
 *   - output rate x multiplier: out time base = in time base / multiplier, generated pts on the pair's real interval
 *     (exact for VFR), out frame_rate = in x multiplier (mpv reads it through buffersink as the nominal fps);
 *   - size change (upscaling), with the display aspect kept;
 *   - HDR: static and dynamic metadata pass through when the engine keeps the signal; pushcore's RGB output gets its
 *     own colour tags (HDR10 or sRGB);
 *   - cut guard: a timestamp discontinuity (seek, a backward or a long jump) never makes a pair; the engines add their
 *     own content-based scene-cut guard;
 *   - when the module is missing or fails to open: pass-through (the video keeps playing), with the reason logged.
 * Builds against libavfilter 8 (FFmpeg 5.0) up to 12 (FFmpeg 9.1).
 */

#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1   /* dladdr */
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/frame.h"
#include "libavutil/hwcontext.h"
#include "libavutil/imgutils.h"
#include "libavutil/internal.h"
#include "libavutil/mastering_display_metadata.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#include "config.h"
#include "version.h"   /* LIBAVFILTER_VERSION_INT (avfilter.h only has version_major.h) */
#include "avfilter.h"
#include "filters.h"
#include "formats.h"
#include "video.h"

#if LIBAVFILTER_VERSION_INT < AV_VERSION_INT(10, 4, 100)
#include "internal.h"
#endif

#if CONFIG_D3D11VA
#include "libavutil/hwcontext_d3d11va.h"
#endif
/* PM_HOOK_VULKAN: the runtime hook's private libavfilter (configured with --disable-everything, no --enable-vulkan, so it
 * links no Vulkan code of its own) still takes FFmpeg's Vulkan frames: only the public hwcontext_vulkan.h types are used,
 * against the pinned Vulkan-Headers (build-hook.sh, FFmpeg 7.1 and newer) */
#if CONFIG_VULKAN || defined(PM_HOOK_VULKAN)
#define PM_HAVE_VULKAN 1
#include "libavutil/hwcontext_vulkan.h"
/* zero copy into the engine's Vulkan engine: needs AVVulkanDeviceContext.qf (lavu 59.34, FFmpeg 7.1) */
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(59, 34, 100)
#define PM_VK_ZERO_COPY 1
#else
#define PM_VK_ZERO_COPY 0
#endif
#else
#define PM_HAVE_VULKAN 0
#define PM_VK_ZERO_COPY 0
#endif

/* CUDA frames (AV_PIX_FMT_CUDA, NVDEC) on Linux: only AVCUDADeviceContext's public fields are read (cuda_ctx, stream),
 * so neither the CUDA SDK nor ffnvcodec is needed; the hook's private libavfilter gets it too. PM_NO_CUDA turns it off. */
#if defined(__linux__) && !defined(PM_NO_CUDA)
#define PM_HAVE_CUDA 1
#ifndef CUDA_VERSION
typedef struct CUctx_st *CUcontext;
typedef struct CUstream_st *CUstream;
/* FFmpeg 9.1's hwcontext_cuda.h also names CUDA arrays (frames context members only; never touched here): the same
 * definitions as ffnvcodec's dynlink_cuda.h so the structs' layout matches; unused by older FFmpeg */
typedef struct CUarray_st *CUarray;
typedef enum CUarray_format_enum {
    CU_AD_FORMAT_UNSIGNED_INT8 = 0x01,
    CU_AD_FORMAT_UNSIGNED_INT16 = 0x02,
    CU_AD_FORMAT_UNSIGNED_INT32 = 0x03,
    CU_AD_FORMAT_SIGNED_INT8 = 0x08,
    CU_AD_FORMAT_SIGNED_INT16 = 0x09,
    CU_AD_FORMAT_SIGNED_INT32 = 0x0a,
    CU_AD_FORMAT_HALF = 0x10,
    CU_AD_FORMAT_FLOAT = 0x20
} CUarray_format;
typedef struct CUDA_ARRAY3D_DESCRIPTOR_st {
    size_t Width;
    size_t Height;
    size_t Depth;
    CUarray_format Format;
    unsigned int NumChannels;
    unsigned int Flags;
} CUDA_ARRAY3D_DESCRIPTOR;
#define CUDA_VERSION 7050
#define PM_CUDA_OWN_TYPES 1
#endif
#include "libavutil/hwcontext_cuda.h"
#else
#define PM_HAVE_CUDA 0
#endif

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "pmeng.h"

/* libavfilter 10.4 (FFmpeg 7.1) moved frame_rate and hw_frames_ctx from AVFilterLink to FilterLink */
#if LIBAVFILTER_VERSION_INT >= AV_VERSION_INT(10, 4, 100)
#define PM_FRAME_RATE(l) (ff_filter_link(l)->frame_rate)
#define PM_HW_FRAMES(l)  (ff_filter_link(l)->hw_frames_ctx)
#else
#define PM_FRAME_RATE(l) ((l)->frame_rate)
#define PM_HW_FRAMES(l)  ((l)->hw_frames_ctx)
#endif

#define PM_RING 32

/* the bit depth the newest stream's engine asked for (0: none): the mpv hook reads it in-process through its private
 * libavfilter (pushmpc_hook_glue.c avfilter_pushmpc_want_depth), not as vf-metadata (that crashed mpv 0.37) */
volatile int ff_pushmpc_want_depth = 0;

typedef struct PushMPCContext {
    const AVClass *class;

    /* options (the words of PushMPC's settings; empty = the saved setting / Auto) */
    char *smoother, *sharper, *backend, *engine_path, *core_dir, *adapter, *extra;
    double display_fps;
    int display_hdr;
    int passthrough_on_error;
    int wait_ms;

    const pmeng_api *api;
    void *eng;
    pmeng_output out;
    int passthrough;
    int fallback;                 /* 1: download / CPU engine / upload */
    int colour_own;               /* CPU output with the engine's own colour (Linux NVIDIA engine: BT.709 / HDR10) */
    int want_depth;               /* the engine asked for a deeper stream (pmeng_output.want_out_depth): frame metadata */
    int act_logged;               /* activate() calls described in the verbose log (the first few of a stream) */
    int source_depth;             /* option: the bits of the source before the player converted it (8 -> 10) */
    int K;                        /* output multiplier (time base divisor) */
    enum AVPixelFormat in_fmt, sw_fmt;
    AVBufferRef *out_frames;      /* the output pool (hardware output) */
    AVFrame *cpu_in;              /* fallback: the downloaded input */
    int in_w, in_h;
    AVRational in_sar;

    /* source bookkeeping, by submit sequence */
    uint64_t seq;                 /* next sequence number */
    int64_t pts[PM_RING];
    AVFrame *props[PM_RING];      /* props-only copies of the source frames */
    int64_t last_pts, delta;      /* last source pts (in time base) and the typical interval */
    int have_last;
    int64_t last_out_pts, last_out_dur;
    uint64_t frames_in, frames_out, generated, discontinuities;
} PushMPCContext;

/* ------------------------------------------------------------------------------------------- the engine module */

static const pmeng_api *g_api;     /* process-wide: the module holds warm engine state and is never unloaded */
static char g_api_from[1024];

#ifdef _WIN32
static void *pm_dlopen(const char *path)
{
    wchar_t w[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1024))
        return NULL;
    return LoadLibraryExW(w, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
}
static void *pm_dlsym(void *h, const char *name) { return (void *)GetProcAddress((HMODULE)h, name); }
#define PM_ENGINE_FILE "pushmpc_engine.dll"
#else
static void *pm_dlopen(const char *path) { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
static void *pm_dlsym(void *h, const char *name) { return dlsym(h, name); }
#ifdef __APPLE__
#define PM_ENGINE_FILE "libpushmpc_engine.dylib"
#else
#define PM_ENGINE_FILE "libpushmpc_engine.so"
#endif
#endif

static const pmeng_api *try_module(AVFilterContext *ctx, const char *path)
{
    void *h;
    pmeng_get_api_fn get;
    const pmeng_api *api;
    if (!path || !*path)
        return NULL;
    h = pm_dlopen(path);
    if (!h) {
        av_log(ctx, AV_LOG_DEBUG, "engine module not at %s\n", path);
        return NULL;
    }
    get = (pmeng_get_api_fn)pm_dlsym(h, "pmeng_get_api");
    api = get ? get(PMENG_ABI_VERSION) : NULL;
    if (!api || api->abi_version != PMENG_ABI_VERSION) {
        av_log(ctx, AV_LOG_WARNING, "%s is not a PushMPC engine module of ABI %u\n", path, PMENG_ABI_VERSION);
        return NULL;   /* left loaded: unloading foreign code is riskier than keeping it */
    }
    av_strlcpy(g_api_from, path, sizeof(g_api_from));
    return api;
}

/* the folder of this code's own binary (libavfilter / avfilter-N.dll / the hook), with a trailing separator */
static void own_dir(char *buf, size_t size)
{
    buf[0] = 0;
#ifdef _WIN32
    {
        HMODULE m = NULL;
        wchar_t w[1024];
        DWORD n;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)(void *)own_dir, &m) &&
            (n = GetModuleFileNameW(m, w, 1024)) > 0 && n < 1024) {
            while (n > 0 && w[n - 1] != L'\\' && w[n - 1] != L'/')
                w[--n] = 0;
            WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)size, NULL, NULL);
        }
    }
#else
    {
        Dl_info di;
        if (dladdr((void *)own_dir, &di) && di.dli_fname) {
            const char *slash = strrchr(di.dli_fname, '/');
            if (slash && (size_t)(slash - di.dli_fname + 2) <= size) {
                memcpy(buf, di.dli_fname, slash - di.dli_fname + 1);
                buf[slash - di.dli_fname + 1] = 0;
            }
        }
    }
#endif
}

static const pmeng_api *load_module(AVFilterContext *ctx, const char *opt_path)
{
    char p[1024];
    const char *env;
    if (g_api)
        return g_api;
    /* 1. the option (when given, nothing else is tried), 2. PUSHMPC_ENGINE, 3. next to this binary, 4. the per-user
     *    install, 5. the loader's search path */
    if (opt_path && *opt_path)
        return g_api = try_module(ctx, opt_path);
    env = getenv("PUSHMPC_ENGINE");
    if ((g_api = try_module(ctx, env)))
        return g_api;
    own_dir(p, sizeof(p));
    if (p[0]) {
        av_strlcat(p, PM_ENGINE_FILE, sizeof(p));
        if ((g_api = try_module(ctx, p)))
            return g_api;
    }
#ifdef _WIN32
    env = getenv("LOCALAPPDATA");
    if (env) {
        snprintf(p, sizeof(p), "%s\\PushMPC\\mpv\\" PM_ENGINE_FILE, env);
        if ((g_api = try_module(ctx, p)))
            return g_api;
    }
#elif defined(__APPLE__)
    env = getenv("HOME");
    if (env) {
        snprintf(p, sizeof(p), "%s/Library/Application Support/PushMPC/lib/" PM_ENGINE_FILE, env);
        if ((g_api = try_module(ctx, p)))
            return g_api;
    }
#else
    env = getenv("XDG_DATA_HOME");
    if (env && env[0] == '/')
        snprintf(p, sizeof(p), "%s/pushmpc/lib/" PM_ENGINE_FILE, env);
    else if ((env = getenv("HOME")))
        snprintf(p, sizeof(p), "%s/.local/share/pushmpc/lib/" PM_ENGINE_FILE, env);
    else
        p[0] = 0;
    if ((g_api = try_module(ctx, p)))
        return g_api;
#endif
    return g_api = try_module(ctx, PM_ENGINE_FILE);
}

/* ------------------------------------------------------------------------------------------- formats */

static const enum AVPixelFormat pix_fmts[] = {
#if CONFIG_D3D11VA
    AV_PIX_FMT_D3D11,
#endif
#if PM_HAVE_VULKAN
    AV_PIX_FMT_VULKAN,
#endif
#if PM_HAVE_CUDA
    AV_PIX_FMT_CUDA,
#endif
    AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV420P10, AV_PIX_FMT_NV12, AV_PIX_FMT_P010,
    AV_PIX_FMT_NONE
};

static uint32_t pmeng_fmt_of(enum AVPixelFormat f)
{
    switch (f) {
    case AV_PIX_FMT_YUV420P:   return PMENG_FMT_YUV420P;
    case AV_PIX_FMT_YUV420P10: return PMENG_FMT_YUV420P10;
    case AV_PIX_FMT_NV12:      return PMENG_FMT_NV12;
    case AV_PIX_FMT_P010:      return PMENG_FMT_P010;
    case AV_PIX_FMT_P016:      return PMENG_FMT_P010;
    case AV_PIX_FMT_BGRA:      return PMENG_FMT_BGRA;
    case AV_PIX_FMT_X2BGR10:   return PMENG_FMT_X2BGR10;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 33, 101)
    case AV_PIX_FMT_RGBAF16:   return PMENG_FMT_RGBAF16;
#endif
    default:                   return PMENG_FMT_NONE;
    }
}

static enum AVPixelFormat av_fmt_of(uint32_t f)
{
    switch (f) {
    case PMENG_FMT_YUV420P:   return AV_PIX_FMT_YUV420P;
    case PMENG_FMT_YUV420P10: return AV_PIX_FMT_YUV420P10;
    case PMENG_FMT_NV12:      return AV_PIX_FMT_NV12;
    case PMENG_FMT_P010:      return AV_PIX_FMT_P010;
    case PMENG_FMT_BGRA:      return AV_PIX_FMT_BGRA;
    case PMENG_FMT_X2BGR10:   return AV_PIX_FMT_X2BGR10;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 33, 101)
    case PMENG_FMT_RGBAF16:   return AV_PIX_FMT_RGBAF16;
#endif
    default:                  return AV_PIX_FMT_NONE;
    }
}

/* ------------------------------------------------------------------------------------------- setup */

#if CONFIG_D3D11VA
static void d3d11_lock(void *c)   { AVD3D11VADeviceContext *d = c; if (d->lock) d->lock(d->lock_ctx); }
static void d3d11_unlock(void *c) { AVD3D11VADeviceContext *d = c; if (d->unlock) d->unlock(d->lock_ctx); }
#endif

#if PM_VK_ZERO_COPY
#if !defined(FF_API_VULKAN_SYNC_QUEUES) || FF_API_VULKAN_SYNC_QUEUES
static void vk_lock_queue(void *c, uint32_t family, uint32_t index)
{
    AVHWDeviceContext *dc = c;
    AVVulkanDeviceContext *h = dc->hwctx;
FF_DISABLE_DEPRECATION_WARNINGS
    if (h->lock_queue)
        h->lock_queue(dc, family, index);
FF_ENABLE_DEPRECATION_WARNINGS
}
static void vk_unlock_queue(void *c, uint32_t family, uint32_t index)
{
    AVHWDeviceContext *dc = c;
    AVVulkanDeviceContext *h = dc->hwctx;
FF_DISABLE_DEPRECATION_WARNINGS
    if (h->unlock_queue)
        h->unlock_queue(dc, family, index);
FF_ENABLE_DEPRECATION_WARNINGS
}
#define PM_VK_HAVE_QUEUE_LOCK 1
#else
#define PM_VK_HAVE_QUEUE_LOCK 0
#endif

/* the engine's view of an FFmpeg Vulkan frame: one entry per plane (image, aspect, layout, timeline semaphore) */
static void vk_fill(pmeng_image *im, AVFrame *f)
{
    AVHWFramesContext *hwfc = (AVHWFramesContext *)f->hw_frames_ctx->data;
    AVVkFrame *vkf = (AVVkFrame *)f->data[0];
    const int planes = av_pix_fmt_count_planes(hwfc->sw_format);
    int nb_images = 0, p;
    while (nb_images < AV_NUM_DATA_POINTERS && vkf->img[nb_images])
        nb_images++;
    im->vk_planes = planes > 3 ? 3 : planes;
    for (p = 0; p < (int)im->vk_planes; p++) {
        const int img = nb_images == 1 ? 0 : p;
        im->vk[p].image = (void *)vkf->img[img];
        im->vk[p].aspect = nb_images == 1 && planes > 1 ? (VK_IMAGE_ASPECT_PLANE_0_BIT << p) : VK_IMAGE_ASPECT_COLOR_BIT;
        im->vk[p].layout = vkf->layout[img];
        im->vk[p].semaphore = (void *)vkf->sem[img];
        im->vk[p].value = vkf->sem_value[img];
    }
}
/* after the engine waited for value and signalled value + 1, leaving the images in GENERAL */
static void vk_after(AVFrame *f, int wrote)
{
    AVVkFrame *vkf = (AVVkFrame *)f->data[0];
    int i;
    for (i = 0; i < AV_NUM_DATA_POINTERS && vkf->img[i]; i++) {
        vkf->sem_value[i]++;
        vkf->layout[i] = VK_IMAGE_LAYOUT_GENERAL;
        vkf->access[i] = wrote ? VK_ACCESS_2_SHADER_WRITE_BIT : VK_ACCESS_2_SHADER_READ_BIT;
        vkf->queue_family[i] = VK_QUEUE_FAMILY_IGNORED;
    }
}
static void vk_lock_frame(AVFrame *f, int lock)
{
    AVHWFramesContext *hwfc = (AVHWFramesContext *)f->hw_frames_ctx->data;
    AVVulkanFramesContext *vfc = hwfc->hwctx;
    if (lock && vfc->lock_frame)
        vfc->lock_frame(hwfc, (AVVkFrame *)f->data[0]);
    else if (!lock && vfc->unlock_frame)
        vfc->unlock_frame(hwfc, (AVVkFrame *)f->data[0]);
}
#endif

static void add_opt(char *buf, size_t size, const char *key, const char *val)
{
    if (!val || !*val)
        return;
    av_strlcatf(buf, size, "%s=%s\n", key, val);
}

static int start_passthrough(AVFilterContext *ctx, const char *why)
{
    PushMPCContext *s = ctx->priv;
    ff_pushmpc_want_depth = 0;   /* no engine: no request */
    if (!s->passthrough_on_error) {
        av_log(ctx, AV_LOG_ERROR, "%s\n", why);
        return AVERROR(EINVAL);
    }
    av_log(ctx, AV_LOG_WARNING, "%s: the video passes through unchanged\n", why);
    s->passthrough = 1;
    return 0;
}

static int alloc_hw_pool(AVFilterContext *ctx, AVBufferRef *device_ref, enum AVPixelFormat hw, enum AVPixelFormat sw,
                         int w, int h, int render_target)
{
    PushMPCContext *s = ctx->priv;
    AVHWFramesContext *fc;
    int ret;
    av_buffer_unref(&s->out_frames);
    s->out_frames = av_hwframe_ctx_alloc(device_ref);
    if (!s->out_frames)
        return AVERROR(ENOMEM);
    fc = (AVHWFramesContext *)s->out_frames->data;
    fc->format = hw;
    fc->sw_format = sw;
    fc->width = w;
    fc->height = h;
    fc->initial_pool_size = 0;    /* single textures, grown on demand (mpv holds a varying number of frames) */
#if CONFIG_D3D11VA
    if (hw == AV_PIX_FMT_D3D11) {
        AVD3D11VAFramesContext *dh = fc->hwctx;
        dh->BindFlags = D3D11_BIND_SHADER_RESOURCE | (render_target ? D3D11_BIND_RENDER_TARGET : 0);
    }
#endif
#if PM_VK_ZERO_COPY
    if (hw == AV_PIX_FMT_VULKAN && render_target) {   /* the engine writes it: one image per plane, storage usage */
        AVVulkanFramesContext *vh = fc->hwctx;
        vh->flags |= AV_VK_FRAME_FLAG_DISABLE_MULTIPLANE;
        vh->usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        vh->tiling = VK_IMAGE_TILING_OPTIMAL;
    }
#endif
    ret = av_hwframe_ctx_init(s->out_frames);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "cannot create the output pool (%s, %dx%d): %s\n", av_get_pix_fmt_name(sw), w, h,
               av_err2str(ret));
        av_buffer_unref(&s->out_frames);
    }
    return ret;
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    PushMPCContext *s = ctx->priv;
    AVFilterLink *inlink = ctx->inputs[0];
    AVBufferRef *in_frames = PM_HW_FRAMES(inlink);
    AVHWFramesContext *ifc = in_frames ? (AVHWFramesContext *)in_frames->data : NULL;
    pmeng_device dev = { sizeof(dev) };
    pmeng_stream st = { sizeof(st) };
    char opts[4096] = "", err[512] = "", num[64];
    int ret;

    s->in_fmt = inlink->format;
    s->sw_fmt = ifc ? ifc->sw_format : inlink->format;
    s->in_w = inlink->w;
    s->in_h = inlink->h;
    s->in_sar = inlink->sample_aspect_ratio;
    s->K = 1;
    s->passthrough = 0;
    s->fallback = 0;

    /* pass-through defaults (kept unless an engine opens) */
    outlink->w = inlink->w;
    outlink->h = inlink->h;
    outlink->time_base = inlink->time_base;
    outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;
    PM_FRAME_RATE(outlink) = PM_FRAME_RATE(inlink);

    if (s->in_fmt == AV_PIX_FMT_D3D11)
        dev.mem = PMENG_MEM_D3D11;
    else if (s->in_fmt == AV_PIX_FMT_VULKAN)
        dev.mem = PMENG_MEM_VULKAN;
#if PM_HAVE_CUDA
    else if (s->in_fmt == AV_PIX_FMT_CUDA)
        dev.mem = PMENG_MEM_CUDA;
#endif
    else
        dev.mem = PMENG_MEM_CPU;
    if (dev.mem != PMENG_MEM_CPU && !ifc)
        return AVERROR(EINVAL);
#if CONFIG_D3D11VA
    if (dev.mem == PMENG_MEM_D3D11) {
        AVD3D11VADeviceContext *d = ifc->device_ctx->hwctx;
        dev.d3d11_device = d->device;
        dev.lock = d3d11_lock;
        dev.unlock = d3d11_unlock;
        dev.lock_ctx = d;
    }
#endif
#if PM_VK_ZERO_COPY
    if (dev.mem == PMENG_MEM_VULKAN) {
        AVHWDeviceContext *dc = ifc->device_ctx;
        AVVulkanDeviceContext *vk = dc->hwctx;
        int i;
        dev.vk_instance = (void *)vk->inst;
        dev.vk_physical_device = (void *)vk->phys_dev;
        dev.vk_device = (void *)vk->act_dev;
        dev.vk_get_instance_proc_addr = (void *)vk->get_proc_addr;
        for (i = 0; i < vk->nb_qf; i++)
            if (vk->qf[i].flags & VK_QUEUE_COMPUTE_BIT) {
                dev.vk_queue_family = vk->qf[i].idx;
                dev.vk_queue_index = 0;
                break;
            }
        if (i == vk->nb_qf)
            dev.vk_device = NULL;   /* no compute queue: the module refuses, the copy fallback runs */
#if PM_VK_HAVE_QUEUE_LOCK
        dev.vk_lock_queue = vk_lock_queue;
        dev.vk_unlock_queue = vk_unlock_queue;
        dev.vk_lock_ctx = dc;
#endif
    }
#endif

#if PM_HAVE_CUDA
    if (dev.mem == PMENG_MEM_CUDA) {
        AVCUDADeviceContext *cu = ifc->device_ctx->hwctx;
        dev.cuda_ctx = (void *)cu->cuda_ctx;
        dev.cuda_stream = (void *)cu->stream;
    }
#endif

    st.width = inlink->w;
    st.height = inlink->h;
    st.fmt = pmeng_fmt_of(s->sw_fmt);
    st.fps_num = PM_FRAME_RATE(inlink).num;
    st.fps_den = PM_FRAME_RATE(inlink).den;
    st.range = AVCOL_RANGE_UNSPECIFIED;
    st.matrix = AVCOL_SPC_UNSPECIFIED;
    st.primaries = AVCOL_PRI_UNSPECIFIED;
    st.transfer = AVCOL_TRC_UNSPECIFIED;
#if LIBAVFILTER_VERSION_INT >= AV_VERSION_INT(9, 15, 100)
    st.range = inlink->color_range;
    st.matrix = inlink->colorspace;
#endif
    if (st.fmt == PMENG_FMT_NONE)
        return start_passthrough(ctx, "unsupported input format");

    s->api = load_module(ctx, s->engine_path);
    if (!s->api)
        return start_passthrough(ctx, "the PushMPC engine module (" PM_ENGINE_FILE ") was not found");

    add_opt(opts, sizeof(opts), "smoother", s->smoother);
    add_opt(opts, sizeof(opts), "sharper", s->sharper);
    add_opt(opts, sizeof(opts), "backend", s->backend);
    add_opt(opts, sizeof(opts), "core_dir", s->core_dir);
    add_opt(opts, sizeof(opts), "adapter", s->adapter);
    if (s->display_fps > 0) {
        snprintf(num, sizeof(num), "%.6f", s->display_fps);
        add_opt(opts, sizeof(opts), "display_fps", num);
    }
    if (s->display_hdr >= 0) {
        snprintf(num, sizeof(num), "%d", s->display_hdr);
        add_opt(opts, sizeof(opts), "display_hdr", num);
    }
    if (s->source_depth != 0) {
        snprintf(num, sizeof(num), "%d", s->source_depth);
        add_opt(opts, sizeof(opts), "source_bits", num);
    }
    if (s->extra && *s->extra) {      /* "key=value|key=value" */
        char *e = av_strdup(s->extra), *p;
        if (!e)
            return AVERROR(ENOMEM);
        for (p = e; *p; p++)
            if (*p == '|')
                *p = '\n';
        av_strlcatf(opts, sizeof(opts), "%s\n", e);
        av_free(e);
    }

    memset(&s->out, 0, sizeof(s->out));
    s->out.struct_size = sizeof(s->out);
    s->eng = s->api->open(&dev, &st, opts, &s->out, err, sizeof(err));
    if (!s->eng && dev.mem != PMENG_MEM_CPU) {
        /* the module cannot take this memory at all: try its CPU engine behind the copy fallback */
        av_log(ctx, AV_LOG_INFO, "engine on %s frames: %s; trying the copy fallback\n",
               av_get_pix_fmt_name(s->in_fmt), err);
        dev.mem = PMENG_MEM_CPU;
        memset(&s->out, 0, sizeof(s->out));
        s->out.struct_size = sizeof(s->out);
        s->eng = s->api->open(&dev, &st, opts, &s->out, err, sizeof(err));
    }
    if (!s->eng) {
        char why[640];
        snprintf(why, sizeof(why), "the engine did not open: %s", err);
        return start_passthrough(ctx, why);
    }
    s->fallback = dev.mem != PMENG_MEM_CPU ? s->out.mem == PMENG_MEM_CPU : 0;
    if (dev.mem == PMENG_MEM_CPU && s->in_fmt != s->sw_fmt)
        s->fallback = 1;
    s->K = s->out.multiplier >= 1 && s->out.multiplier <= 16 ? (int)s->out.multiplier : 1;
    /* minor 1: an older module's struct ends before want_out_depth */
    s->want_depth = s->out.struct_size >= offsetof(pmeng_output, want_out_depth) + sizeof(s->out.want_out_depth) ? (int)s->out.want_out_depth : 0;
    ff_pushmpc_want_depth = s->want_depth;
    if (s->want_depth)
        av_log(ctx, AV_LOG_INFO, "the engine would do more with a %d-bit stream (frames tagged pushmpc.want_depth=%d)\n",
               s->want_depth, s->want_depth);
    /* an engine writing host memory or CUDA frames may convert the colour (non-zero fields that differ, or HDR10) */
    s->colour_own = (s->out.mem == PMENG_MEM_CPU || s->out.mem == PMENG_MEM_CUDA) &&
        (s->out.hdr_output || (s->out.range && s->out.range != st.range) || (s->out.matrix && s->out.matrix != st.matrix) ||
         (s->out.primaries && s->out.primaries != st.primaries) || (s->out.transfer && s->out.transfer != st.transfer));

    outlink->w = s->out.width ? (int)s->out.width : inlink->w;
    outlink->h = s->out.height ? (int)s->out.height : inlink->h;
    if (outlink->w != inlink->w || outlink->h != inlink->h) {
        AVRational sar = inlink->sample_aspect_ratio.num > 0 ? inlink->sample_aspect_ratio : (AVRational){ 1, 1 };
        outlink->sample_aspect_ratio = av_mul_q(sar, (AVRational){ inlink->w * outlink->h, outlink->w * inlink->h });
    }
    outlink->time_base = av_mul_q(inlink->time_base, (AVRational){ 1, s->K });
    if (PM_FRAME_RATE(inlink).num > 0 && PM_FRAME_RATE(inlink).den > 0)
        PM_FRAME_RATE(outlink) = av_mul_q(PM_FRAME_RATE(inlink), (AVRational){ s->K, 1 });

    /* output memory */
    if (s->in_fmt == AV_PIX_FMT_D3D11 || s->in_fmt == AV_PIX_FMT_VULKAN || s->in_fmt == AV_PIX_FMT_CUDA) {
        enum AVPixelFormat sw = s->fallback ? s->sw_fmt : av_fmt_of(s->out.fmt);
        if (sw == AV_PIX_FMT_NONE)
            return start_passthrough(ctx, "the engine reported an unknown output format");
        ret = alloc_hw_pool(ctx, ifc->device_ref, s->in_fmt, sw, outlink->w, outlink->h,
                            !s->fallback);
        if (ret < 0)
            return ret;
        av_buffer_unref(&PM_HW_FRAMES(outlink));
        PM_HW_FRAMES(outlink) = av_buffer_ref(s->out_frames);
        if (!PM_HW_FRAMES(outlink))
            return AVERROR(ENOMEM);
    } else if (av_fmt_of(s->out.fmt) != s->in_fmt) {
        return start_passthrough(ctx, "the engine changed the CPU format, which this filter does not negotiate");
    }

    av_log(ctx, AV_LOG_INFO, "%s (%s) on %s frames%s: %dx%d -> %dx%d, x%d; %s\n", s->out.engine, g_api_from,
           av_get_pix_fmt_name(s->in_fmt), s->fallback ? " through the copy fallback" : (s->in_fmt == AV_PIX_FMT_D3D11 || s->in_fmt == AV_PIX_FMT_VULKAN || s->in_fmt == AV_PIX_FMT_CUDA) ? " (zero copy)" : "",
           inlink->w, inlink->h, outlink->w, outlink->h, s->K, s->out.description);
    return 0;
}

/* pass-through needs the output pool of the input (set before formats are final, so do it here as well) */
static int config_output_wrap(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    PushMPCContext *s = ctx->priv;
    int ret = config_output(outlink);
    if (ret < 0)
        return ret;
    if (s->passthrough) {
        AVBufferRef *in_frames = PM_HW_FRAMES(ctx->inputs[0]);
        if (in_frames) {
            av_buffer_unref(&PM_HW_FRAMES(outlink));
            PM_HW_FRAMES(outlink) = av_buffer_ref(in_frames);
            if (!PM_HW_FRAMES(outlink))
                return AVERROR(ENOMEM);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------- frames */

static void fill_image(pmeng_image *im, AVFrame *f, uint32_t mem, uint32_t fmt)
{
    int i;
    memset(im, 0, sizeof(*im));
    im->mem = mem;
    im->fmt = fmt;
    im->width = f->width;
    im->height = f->height;
    if (mem == PMENG_MEM_CPU) {
        for (i = 0; i < 4; i++) {
            im->data[i] = f->data[i];
            im->linesize[i] = f->linesize[i];
        }
    } else if (mem == PMENG_MEM_D3D11) {
        im->d3d11_texture = f->data[0];
        im->d3d11_slice = (uint32_t)(intptr_t)f->data[1];
    }
#if PM_VK_ZERO_COPY
    else if (mem == PMENG_MEM_VULKAN && f->hw_frames_ctx) {
        vk_fill(im, f);
    }
#endif
#if PM_HAVE_CUDA
    else if (mem == PMENG_MEM_CUDA) {   /* device pointers of the planes in the decoder's context */
        const AVHWFramesContext *fc = f->hw_frames_ctx ? (const AVHWFramesContext *)f->hw_frames_ctx->data : NULL;
        const int planes = fc ? av_pix_fmt_count_planes(fc->sw_format) : 2;
        for (i = 0; i < 3 && i < planes; i++) {
            im->cuda_ptr[i] = (uint64_t)(uintptr_t)f->data[i];
            im->cuda_pitch[i] = f->linesize[i];
        }
        im->cuda_planes = planes > 3 ? 3 : (uint32_t)planes;
    }
#endif
}

static void strip_side_data(AVFrame *f, const enum AVFrameSideDataType *types, int n)
{
    int i;
    for (i = 0; i < n; i++)
        av_frame_remove_side_data(f, types[i]);
}

static int emit_one(AVFilterContext *ctx, const pmeng_out_info *oi)
{
    PushMPCContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    const uint64_t src = oi->source;
    const int gen = oi->subframe > 0;
    const int m = oi->multiplier ? (int)oi->multiplier : s->K;
    const int64_t p1 = s->pts[src % PM_RING];
    const int64_t p0 = src > 0 ? s->pts[(src - 1) % PM_RING] : p1 - s->delta;
    const int64_t d = p1 > p0 ? p1 - p0 : s->delta;
    AVFrame *props = s->props[(gen && (int)oi->subframe * 2 < m && src > 0 ? src - 1 : src) % PM_RING];
    AVFrame *out = NULL, *cpu = NULL;
    pmeng_image im;
    int ret;

    if (s->fallback || s->in_fmt == AV_PIX_FMT_YUV420P || s->in_fmt == AV_PIX_FMT_YUV420P10 ||
        s->in_fmt == AV_PIX_FMT_NV12 || s->in_fmt == AV_PIX_FMT_P010) {
        if (s->out_frames) {      /* fallback into a hardware pool: write a CPU frame, then upload */
            cpu = av_frame_alloc();
            if (!cpu)
                return AVERROR(ENOMEM);
            cpu->format = s->sw_fmt;
            cpu->width = outlink->w;
            cpu->height = outlink->h;
            if ((ret = av_frame_get_buffer(cpu, 0)) < 0)
                goto fail;
            fill_image(&im, cpu, PMENG_MEM_CPU, pmeng_fmt_of(s->sw_fmt));
        } else {
            out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
            if (!out)
                return AVERROR(ENOMEM);
            fill_image(&im, out, PMENG_MEM_CPU, pmeng_fmt_of(s->in_fmt));
        }
    } else {
        out = av_frame_alloc();
        if (!out)
            return AVERROR(ENOMEM);
        if ((ret = av_hwframe_get_buffer(s->out_frames, out, 0)) < 0)
            goto fail;
        out->width = outlink->w;
        out->height = outlink->h;
        fill_image(&im, out, s->out.mem, s->out.fmt);
    }

    {
#if PM_VK_ZERO_COPY
        const int vkw = !cpu && out && s->out.mem == PMENG_MEM_VULKAN;
        if (vkw)
            vk_lock_frame(out, 1);
#endif
        ret = s->api->write(s->eng, &im);
#if PM_VK_ZERO_COPY
        if (vkw) {
            if (ret >= 0)
                vk_after(out, 1);
            vk_lock_frame(out, 0);
        }
#endif
    }
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "engine write: %s\n", s->api->last_error(s->eng));
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    if (cpu) {
        out = av_frame_alloc();
        if (!out) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        if ((ret = av_hwframe_get_buffer(s->out_frames, out, 0)) < 0 ||
            (ret = av_hwframe_transfer_data(out, cpu, 0)) < 0)
            goto fail;
        av_frame_free(&cpu);
    }

    if (props && (ret = av_frame_copy_props(out, props)) < 0)
        goto fail;
    out->width = outlink->w;
    out->height = outlink->h;
    out->sample_aspect_ratio = outlink->sample_aspect_ratio;
    out->pts = gen ? p0 * s->K + av_rescale(d * s->K, oi->subframe, m) : p1 * s->K;
    s->last_out_dur = av_rescale(d * s->K, 1, m);
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 30, 100)   /* AVFrame.duration (FFmpeg 6.0) */
    out->duration = s->last_out_dur;
#else
    out->pkt_duration = s->last_out_dur;
#endif
    if (gen) {
        static const enum AVFrameSideDataType per_frame[] = { AV_FRAME_DATA_A53_CC, AV_FRAME_DATA_SEI_UNREGISTERED,
                                                              AV_FRAME_DATA_AFD };
        strip_side_data(out, per_frame, FF_ARRAY_ELEMS(per_frame));
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(58, 7, 100)
        out->flags &= ~AV_FRAME_FLAG_KEY;
#endif
        out->pict_type = AV_PICTURE_TYPE_NONE;
    }
    if ((!s->fallback && s->out.mem != PMENG_MEM_CPU && s->out.fmt != pmeng_fmt_of(s->sw_fmt)) || s->colour_own) {
        /* the engine's own colour (pushcore: RGB, HDR10 or sRGB) */
        static const enum AVFrameSideDataType dynamic[] = { AV_FRAME_DATA_DOVI_RPU_BUFFER, AV_FRAME_DATA_DOVI_METADATA,
                                                            AV_FRAME_DATA_DYNAMIC_HDR_PLUS,
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
                                                            AV_FRAME_DATA_DYNAMIC_HDR_VIVID,
#endif
                                                          };
        static const enum AVFrameSideDataType stat[] = { AV_FRAME_DATA_MASTERING_DISPLAY_METADATA,
                                                         AV_FRAME_DATA_CONTENT_LIGHT_LEVEL };
        /* colour_own: only what the engine names (0 keeps the frame's own) */
        if (s->out.range || !s->colour_own) out->color_range = s->out.range;
        if (s->out.matrix || !s->colour_own) out->colorspace = s->out.matrix;
        if (s->out.primaries || !s->colour_own) out->color_primaries = s->out.primaries;
        if (s->out.transfer || !s->colour_own) out->color_trc = s->out.transfer;
        if (!s->out.keep_dynamic_hdr)
            strip_side_data(out, dynamic, FF_ARRAY_ELEMS(dynamic));
        if (!s->out.hdr_output) {
            strip_side_data(out, stat, FF_ARRAY_ELEMS(stat));
        } else if (s->out.mastering_peak_nits > 0 &&
                   !av_frame_get_side_data(out, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA)) {
            AVMasteringDisplayMetadata *md = av_mastering_display_metadata_create_side_data(out);
            if (md) {     /* BT.2020 primaries, D65 */
                static const int prim[3][2] = { { 34000, 16000 }, { 13250, 34500 }, { 7500, 3000 } };
                int i;
                for (i = 0; i < 3; i++) {
                    md->display_primaries[i][0] = av_make_q(prim[i][0], 50000);
                    md->display_primaries[i][1] = av_make_q(prim[i][1], 50000);
                }
                md->white_point[0] = av_make_q(15635, 50000);
                md->white_point[1] = av_make_q(16450, 50000);
                md->max_luminance = av_d2q(s->out.mastering_peak_nits, 1000000);
                md->min_luminance = av_d2q(s->out.mastering_min_nits, 1000000);
                md->has_primaries = md->has_luminance = 1;
            }
        }
    }

    if (s->want_depth) {   /* for other readers (vf-metadata); the mpv hook reads ff_pushmpc_want_depth */
        char d[16];
        snprintf(d, sizeof(d), "%d", s->want_depth);
        av_dict_set(&out->metadata, "pushmpc.want_depth", d, 0);
    }

    s->last_out_pts = out->pts;
    s->frames_out++;
    if (gen)
        s->generated++;
    return ff_filter_frame(outlink, out);

fail:
    av_frame_free(&cpu);
    av_frame_free(&out);
    s->api->skip(s->eng);
    return ret;
}

/* takes the ready outputs; timeout_ms for the first one; until = keep waiting while outputs are pending */
static int drain(AVFilterContext *ctx, uint32_t timeout_ms, int until_empty)
{
    PushMPCContext *s = ctx->priv;
    pmeng_out_info oi;
    int ret, n = 0;
    for (;;) {
        ret = s->api->peek(s->eng, &oi, n == 0 || until_empty ? timeout_ms : 0);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "engine: %s\n", s->api->last_error(s->eng));
            return AVERROR_EXTERNAL;
        }
        if (ret == 0) {
            if (until_empty && s->api->pending(s->eng) > 0 && timeout_ms > 0) {
                av_log(ctx, AV_LOG_WARNING, "engine: %u outputs did not arrive in time\n", s->api->pending(s->eng));
                s->api->flush(s->eng);
            }
            return n;
        }
        if (oi.source >= s->seq) {   /* nothing of ours (cannot happen with a well-behaved module) */
            s->api->skip(s->eng);
            continue;
        }
        if ((ret = emit_one(ctx, &oi)) < 0)
            return ret;
        n++;
    }
}

static int process(AVFilterContext *ctx, AVFrame *in)
{
    PushMPCContext *s = ctx->priv;
    uint32_t flags = 0;
    pmeng_image im;
    AVFrame *src = in, *props;
    int64_t pts = in->pts;
    int ret, tries;

    s->frames_in++;
    if (pts == AV_NOPTS_VALUE)
        pts = s->have_last ? s->last_pts + (s->delta > 0 ? s->delta : 1) : 0;
    /* timestamp cut guard: first frame, backwards, or a gap of more than 4 typical intervals */
    if (!s->have_last || pts <= s->last_pts || (s->delta > 0 && pts - s->last_pts > 4 * s->delta)) {
        flags |= PMENG_SUBMIT_DISCONTINUITY;
        if (s->have_last)
            s->discontinuities++;
    } else {
        const int64_t d = pts - s->last_pts;
        s->delta = s->delta > 0 ? (s->delta * 7 + d + 4) / 8 : d;
    }
    if (!s->have_last && s->delta <= 0 && PM_FRAME_RATE(ctx->inputs[0]).num > 0)
        s->delta = av_rescale_q(1, av_inv_q(PM_FRAME_RATE(ctx->inputs[0])), ctx->inputs[0]->time_base);
    if (s->delta <= 0)
        s->delta = 1;

    if (s->fallback && in->hw_frames_ctx) {
        if (!s->cpu_in && !(s->cpu_in = av_frame_alloc())) {
            av_frame_free(&in);
            return AVERROR(ENOMEM);
        }
        av_frame_unref(s->cpu_in);
        s->cpu_in->format = s->sw_fmt;
        if ((ret = av_hwframe_transfer_data(s->cpu_in, in, 0)) < 0) {
            av_log(ctx, AV_LOG_ERROR, "download: %s\n", av_err2str(ret));
            av_frame_free(&in);
            return ret;
        }
        src = s->cpu_in;
        fill_image(&im, src, PMENG_MEM_CPU, pmeng_fmt_of(s->sw_fmt));
        im.width = s->in_w;
        im.height = s->in_h;
    } else if (s->in_fmt == AV_PIX_FMT_D3D11) {
        fill_image(&im, in, PMENG_MEM_D3D11, pmeng_fmt_of(s->sw_fmt));
    } else if (s->in_fmt == AV_PIX_FMT_VULKAN) {
        fill_image(&im, in, PMENG_MEM_VULKAN, pmeng_fmt_of(s->sw_fmt));
    } else if (s->in_fmt == AV_PIX_FMT_CUDA) {
        fill_image(&im, in, PMENG_MEM_CUDA, pmeng_fmt_of(s->sw_fmt));
    } else {
        fill_image(&im, in, PMENG_MEM_CPU, pmeng_fmt_of(s->in_fmt));
    }
    im.range = in->color_range;
    im.matrix = in->colorspace;
    im.primaries = in->color_primaries;
    im.transfer = in->color_trc;

    /* bookkeeping before submit: outputs of this frame may be ready right away */
    props = s->props[s->seq % PM_RING];
    if (!props && !(props = s->props[s->seq % PM_RING] = av_frame_alloc())) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }
    av_frame_unref(props);
    if ((ret = av_frame_copy_props(props, in)) < 0) {
        av_frame_free(&in);
        return ret;
    }
    s->pts[s->seq % PM_RING] = pts;

    for (tries = 0;; tries++) {
#if PM_VK_ZERO_COPY
        const int vkin = !s->fallback && s->in_fmt == AV_PIX_FMT_VULKAN;
        if (vkin)
            vk_lock_frame(in, 1);
#endif
        ret = s->api->submit(s->eng, &im, av_rescale_q(pts, ctx->inputs[0]->time_base, AV_TIME_BASE_Q), flags);
#if PM_VK_ZERO_COPY
        if (vkin) {
            if (ret == PMENG_OK)
                vk_after(in, 0);
            vk_lock_frame(in, 0);
        }
#endif
        if (ret != PMENG_BUSY)
            break;
        if (tries * 20 > s->wait_ms) {
            av_log(ctx, AV_LOG_ERROR, "engine stayed busy for %d ms\n", s->wait_ms);
            ret = PMENG_ERROR;
            break;
        }
        if ((ret = drain(ctx, 20, 0)) < 0) {
            av_frame_free(&in);
            return ret;
        }
    }
    av_frame_free(&in);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "engine submit: %s\n", s->api->last_error(s->eng));
        return AVERROR_EXTERNAL;
    }
    s->seq++;
    s->last_pts = pts;
    s->have_last = 1;
    return drain(ctx, 0, 0);
}

static int activate(AVFilterContext *ctx)
{
    PushMPCContext *s = ctx->priv;
    AVFilterLink *inlink = ctx->inputs[0];
    AVFilterLink *outlink = ctx->outputs[0];
    AVFrame *in = NULL;
    int64_t pts;
    int ret, status;

    FF_FILTER_FORWARD_STATUS_BACK(outlink, inlink);

    ret = ff_inlink_consume_frame(inlink, &in);
    if (ret < 0)
        return ret;
    if (ret > 0) {
        if (s->passthrough)
            return ff_filter_frame(outlink, in);
        ret = process(ctx, in);
        if (ret < 0)
            return ret;
        if (ff_inlink_queued_frames(inlink) > 0) {
            ff_filter_set_ready(ctx, 100);
            return 0;
        }
        /* the frame gave no output yet (an engine holding outputs back: Dedup's lookahead, a GPU pair): pass the
           request on, or nothing asks the source again. mpv's lavfi wrapper feeds a frame only when the buffersrc saw
           a failed request, and the buffersink does not ask again while its own request stands: Plex for Mac with the
           Vulkan host engine stopped after frame 0 (mpv-native run 45948718958: 1 source, 0 outputs, 120 s) */
        if (s->act_logged < 24) {   /* where a host that feeds on request stops (Plex for Mac, macOS 15) */
            s->act_logged++;
            av_log(ctx, AV_LOG_VERBOSE, "activate: frame %" PRIu64 " in, %" PRIu64 " out; output wanted %d (then asked upstream), engine holds %u\n",
                   s->frames_in, s->frames_out, ff_outlink_frame_wanted(outlink),
                   s->eng ? s->api->pending(s->eng) : 0u);
        }
        FF_FILTER_FORWARD_WANTED(outlink, inlink);
        return 0;
    }

    if (ff_inlink_acknowledge_status(inlink, &status, &pts)) {
        if (!s->passthrough && s->eng) {
            if (s->api->struct_size >= offsetof(pmeng_api, finish) + sizeof(s->api->finish) && s->api->finish)
                s->api->finish(s->eng);   /* Dedup's held-back outputs */
            if ((ret = drain(ctx, (uint32_t)s->wait_ms, 1)) < 0)
                return ret;
            pts = s->frames_out ? s->last_out_pts + s->last_out_dur : av_rescale(pts, s->K, 1);
        }
        ff_outlink_set_status(outlink, status, pts);
        return 0;
    }

    if (s->act_logged < 24 && !s->passthrough) {
        s->act_logged++;
        av_log(ctx, AV_LOG_VERBOSE, "activate: no frame queued (%" PRIu64 " in, %" PRIu64 " out); output wanted %d\n",
               s->frames_in, s->frames_out, ff_outlink_frame_wanted(outlink));
    }
    FF_FILTER_FORWARD_WANTED(outlink, inlink);
    return FFERROR_NOT_READY;
}

static av_cold int init(AVFilterContext *ctx)
{
    PushMPCContext *s = ctx->priv;
    s->K = 1;
    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    PushMPCContext *s = ctx->priv;
    int i;
    if (s->eng && s->api) {
        char st[512] = "";
        if (s->api->stats)
            s->api->stats(s->eng, st, sizeof(st));
        av_log(ctx, AV_LOG_VERBOSE, "%" PRIu64 " frames in, %" PRIu64 " out (%" PRIu64 " generated), %" PRIu64
               " timestamp cuts; %s\n", s->frames_in, s->frames_out, s->generated, s->discontinuities, st);
        s->api->close(s->eng);
        s->eng = NULL;
    }
    for (i = 0; i < PM_RING; i++)
        av_frame_free(&s->props[i]);
    av_frame_free(&s->cpu_in);
    av_buffer_unref(&s->out_frames);
}

#define OFFSET(x) offsetof(PushMPCContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)
static const AVOption pushmpc_options[] = {
    { "smoother", "frame generation: auto, off, 2..16 (multiplier)", OFFSET(smoother), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, FLAGS },
    { "sharper", "upscaling / clean-up: auto, off, 1..4", OFFSET(sharper), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, FLAGS },
    { "backend", "engine: auto, nvidia (pushcore), portable", OFFSET(backend), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, FLAGS },
    { "engine", "path of the engine module", OFFSET(engine_path), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, FLAGS },
    { "core_dir", "pushcore folder (Windows)", OFFSET(core_dir), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, FLAGS },
    { "adapter", "GPU for the engine (warp: the software adapter, for tests)", OFFSET(adapter), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, FLAGS },
    { "extra", "more engine settings, key=value|key=value", OFFSET(extra), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, FLAGS },
    { "display_fps", "display refresh rate (0: the engine finds out, or x2)", OFFSET(display_fps), AV_OPT_TYPE_DOUBLE, { .dbl = 0 }, 0, 1000, FLAGS },
    { "display_hdr", "the player presents HDR10 (-1: the engine finds out)", OFFSET(display_hdr), AV_OPT_TYPE_INT, { .i64 = -1 }, -1, 1, FLAGS },
    { "passthrough", "pass the video through when the engine is missing or fails to open", OFFSET(passthrough_on_error), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, FLAGS },
    { "wait_ms", "longest wait for the engine", OFFSET(wait_ms), AV_OPT_TYPE_INT, { .i64 = 10000 }, 100, 120000, FLAGS },
    { "source_depth", "bits of the source before the player converted it (0: as it arrives; -1: every stream converted, unknown)", OFFSET(source_depth), AV_OPT_TYPE_INT, { .i64 = 0 }, -1, 16, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(pushmpc);

static const AVFilterPad pushmpc_inputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
};

static const AVFilterPad pushmpc_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_output_wrap,
    },
};

#if LIBAVFILTER_VERSION_MAJOR >= 11
const FFFilter ff_vf_pushmpc = {
    .p.name         = "pushmpc",
    .p.description  = NULL_IF_CONFIG_SMALL("PushMPC frame generation and upscaling (pushcore, portable engines)."),
    .p.priv_class   = &pushmpc_class,
    .p.flags        = AVFILTER_FLAG_HWDEVICE,
    .priv_size      = sizeof(PushMPCContext),
    .init           = init,
    .uninit         = uninit,
    .activate       = activate,
    FILTER_INPUTS(pushmpc_inputs),
    FILTER_OUTPUTS(pushmpc_outputs),
    FILTER_PIXFMTS_ARRAY(pix_fmts),
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
#else
const AVFilter ff_vf_pushmpc = {
    .name           = "pushmpc",
    .description    = NULL_IF_CONFIG_SMALL("PushMPC frame generation and upscaling (pushcore, portable engines)."),
    .priv_class     = &pushmpc_class,
#ifdef AVFILTER_FLAG_HWDEVICE
    .flags          = AVFILTER_FLAG_HWDEVICE,
#endif
    .priv_size      = sizeof(PushMPCContext),
    .init           = init,
    .uninit         = uninit,
    .activate       = activate,
    FILTER_INPUTS(pushmpc_inputs),
    FILTER_OUTPUTS(pushmpc_outputs),
    FILTER_PIXFMTS_ARRAY(pix_fmts),
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
#endif
