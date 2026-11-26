/*
 * pm_engine: the thin C interface between PushMPC's VLC module and a frame generation engine.
 *
 * Kept deliberately small so the engine behind it can change without touching the VLC side:
 *   - vsplugin's portable GPU engine (engines/gpu: Vulkan 1.2 compute, MoltenVK on macOS) when a Vulkan device answers,
 *     else its CPU engine (engines/portable): PushFPS's portable core (block-matching motion both ways, motion-
 *     compensated interpolation, the scene-cut rule; the GPU engine adds PushMPC's cut guard), engine_portable.cpp;
 *   - Windows: pushcore (pm_pushcore.dll) is a separate path in pushmpc_vlc.c, not behind this interface, because it
 *     runs its own multiplier and timing.
 * Pictures are planar YUV 4:2:0, 8 bits (1 byte per sample) or 9..16 bits (2 bytes, little endian), as VLC's
 * I420 / J420 / YV12 / I0AL hold them.
 */
#ifndef PM_ENGINE_H
#define PM_ENGINE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct pm_engine pm_engine;

typedef struct pm_engine_cfg {
    int width, height, bits; /* visible size; bits 8..16 */
    int threads;             /* 0 = hardware threads (at most 16) */
    int quality;             /* CPU engine: 0 fast, 1 balanced (the GPU engine always uses full-size vectors) */
    int engine;              /* PM_ENGINE_AUTO: the GPU engine (Vulkan) when a device answers, else the CPU engine */
} pm_engine_cfg;
#define PM_ENGINE_AUTO 0
#define PM_ENGINE_CPU 1
#define PM_ENGINE_GPU 2

typedef struct pm_yuv {      /* read-only source planes */
    const uint8_t *data[3];
    ptrdiff_t stride[3];
} pm_yuv;

typedef struct pm_yuv_out {  /* destination planes, same size and depth as the source */
    uint8_t *data[3];
    ptrdiff_t stride[3];
} pm_yuv_out;

typedef struct pm_engine_stats {
    uint64_t pushed, pairs, cuts, generated;
    double me_ms, gen_ms;    /* running means: motion (both ways) per pair, one generated frame */
} pm_engine_stats;

/* NULL on failure, with a reason in err */
pm_engine *pm_engine_open(const pm_engine_cfg *cfg, char *err, size_t errsize);
void pm_engine_close(pm_engine *e);
/* a new source frame; contiguous = it directly follows the previous one (else the pair starts over). 0 = ok */
int pm_engine_push(pm_engine *e, const pm_yuv *src, int contiguous, char *err, size_t errsize);
int pm_engine_have_pair(const pm_engine *e);  /* two contiguous frames pushed: Generate is meaningful */
int pm_engine_pair_is_cut(const pm_engine *e);
/* the frame at phase t of the current pair (0 = previous, 1 = current); the nearer real frame on a cut */
void pm_engine_generate(pm_engine *e, float t, const pm_yuv_out *out);
void pm_engine_forget(pm_engine *e);          /* a seek or a gap: no pair until two frames are pushed again */
void pm_engine_stats_get(const pm_engine *e, pm_engine_stats *out);
int pm_engine_threads(const pm_engine *e);

/* Auto: the multiplier for this source on this display within the CPU budget (vsplugin's DecidePortableK rule).
 * smoother: 0 Auto, 1 off, 2..8 fixed. display_hz <= 0: unknown. Measures the engine once at this size (cached per
 * size). why: a one-line explanation for the log. */
/* Auto for an open engine: its own measured costs (the GPU engine has no x2 cap above 1080p) */
int pm_engine_decide(pm_engine *e, int smoother, double src_fps, double display_hz, char *why, size_t whysize);
/* 1 = the player is full screen, 0 = windowed, -1 = unknown (before pm_engine_decide; Auto counts a window's composition) */
void pm_engine_set_fullscreen(pm_engine *e, int fullscreen);
const char *pm_engine_name(const pm_engine *e);

int pm_engine_decide_k(int smoother, double src_fps, double display_hz, int width, int height, int bits, int threads,
                       int quality, char *why, size_t whysize);

/* ---- zero copy (Linux): a VA-API surface exported as a dmabuf (DRM PRIME 2, separate layers) goes straight into the GPU
 * engine, and a generated frame straight into one (pm_dmabuf_vk.cpp: VK_EXT_external_memory_dma_buf +
 * VK_EXT_image_drm_format_modifier, any modifier the driver can import). The caller keeps the fds (the engine dups
 * what it imports). Without the GPU engine or the driver support, pm_engine_dmabuf_ok says why and the caller copies. */
typedef struct pm_dmabuf {
    int width, height, bits;            /* the picture (visible) size, from the surface origin; 8 (NV12) or 10 (P010) */
    int nobjects;
    int fd[4];
    uint64_t size[4], modifier[4];
    int nlayers;                        /* 2: luma (DRM R8 / R16), chroma (GR88 / GR1616), one plane each */
    struct { uint32_t drm_format; int object; uint32_t offset, pitch; } layer[2];
} pm_dmabuf;
int pm_engine_dmabuf_ok(const pm_engine *e, const char **why);
int pm_engine_push_dmabuf(pm_engine *e, const pm_dmabuf *src, int contiguous, char *err, size_t errsize);
/* the frame at phase t of the current pair into dst (written and finished when this returns). 0 = ok */
int pm_engine_generate_dmabuf(pm_engine *e, float t, const pm_dmabuf *dst, char *err, size_t errsize);

#ifdef __cplusplus
}
#endif
#endif
