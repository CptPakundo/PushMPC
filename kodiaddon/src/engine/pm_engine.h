/*
 * pm_engine: the thin C interface between PushMPC's VLC module and a frame generation engine.
 *
 * Kept deliberately small so the engine behind it can change without touching the VLC side:
 *   - today: vsplugin's portable engine (PushFPS's portable core on the CPU: block-matching motion both ways, motion-
 *     compensated interpolation, PushFPS's scene-cut rule), engine_portable.cpp;
 *   - next: track T1's Vulkan GPU engine (branch track/portable-gpu) implements the same calls;
 *   - Windows RTX: pushcore (D3D11 in, D3D11 out) is a separate path in pushmpc_vlc.c, not behind this interface,
 *     because its frames never touch the CPU.
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
    int quality;             /* 0 fast, 1 balanced */
} pm_engine_cfg;

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
int pm_engine_decide_k(int smoother, double src_fps, double display_hz, int width, int height, int bits, int threads,
                       int quality, char *why, size_t whysize);

#ifdef __cplusplus
}
#endif
#endif
