/*
 * pushmpc_content: ContentDetect (pushmpc/content_detect.h) and its optional image model (pushmpc/content_model.h) behind
 * a plain C interface, for players whose plugins are not C++ (the VLC 3 module is C, built with MinGW on Windows).
 *
 * The library is C++ inside; nothing but C types crosses this interface (no CRT objects, no allocations handed over),
 * so a plugin built with any compiler loads it at run time (LoadLibrary / dlopen) and resolves these functions by name.
 * File names: pushmpc_content.dll (Windows), libpushmpc_content.so (Linux), libpushmpc_content.dylib (macOS).
 *
 * What it measures: the class of the picture (drawn animation or not, and for drawn animation whether it is rendered 3D
 * CG or hand-drawn), per shot, with hysteresis, from the decoded frames and their timestamps only (never from names,
 * paths or stream flags). The caller hands over each source frame (YUV 4:2:0); the library takes the cell grid
 * (~0.5 ms at 1080p) and, about once a second, a 224x224 thumbnail for the image model on the caller's thread. The
 * detector itself runs on a worker thread of its own (~4 ms a frame on average at 1080p), the image model on another
 * (pmml: DirectML on Windows, CoreML on macOS, ONNX Runtime CPU elsewhere). Without the model (no pmml runtime, no model
 * file, a build without pmml, or a failure) the hand-built cues alone decide; pmc_state.model says which.
 *
 * Threads: one caller thread pushes frames; pmc_get_state may be called from any thread. The log callback is called from
 * the library's threads (worker, model) and from the caller's thread; it must be thread-safe. All threads end in
 * pmc_destroy, which returns after the last callback.
 *
 * Licence: GPL-3.0-or-later.
 */
#ifndef PUSHMPC_CONTENT_H
#define PUSHMPC_CONTENT_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
# if defined(PMC_BUILD)
#  define PMC_API __declspec(dllexport)
# else
#  define PMC_API
# endif
#else
# define PMC_API __attribute__((visibility("default")))
#endif

#define PMC_ABI 1u   /* pmc_abi(): a caller built for another ABI must not use the library */

/* pmc_config.flags */
#define PMC_FLAG_NO_MODEL 1u   /* the hand-built cues only (the image model is not started) */
#define PMC_FLAG_SYNC     2u   /* the detector runs on the caller's thread inside pmc_push_yuv420 (tests) */

typedef void (*pmc_log_fn)(void *user, const char *line);   /* one line, UTF-8, no newline */

typedef struct pmc_config {
    uint32_t struct_size;   /* sizeof(pmc_config) */
    uint32_t abi;           /* PMC_ABI */
    pmc_log_fn log;         /* may be NULL */
    void *log_user;
    uint32_t flags;         /* PMC_FLAG_* */
} pmc_config;

/* pmc_state.cls: as pushmpc::content::Class */
#define PMC_CLASS_MEASURING 0
#define PMC_CLASS_DRAWN     1
#define PMC_CLASS_NOT_DRAWN 3   /* live action, realistic 3D CG or game footage */

/* pmc_state.model: which evidence decides */
#define PMC_MODEL_NOT_BUILT 0   /* this library was built without pmml: the hand-built cues decide */
#define PMC_MODEL_OFF       1   /* PMC_FLAG_NO_MODEL: the hand-built cues decide */
#define PMC_MODEL_STARTING  2   /* the image model is loading (the hand-built cues decide meanwhile) */
#define PMC_MODEL_RUNNING   3   /* the image model has returned evidence */
#define PMC_MODEL_FAILED    4   /* no model file, no runtime, or it failed: the hand-built cues decide */

/* pmc_state.policy: what Auto's drawn-animation treatment does for the measured kind (pushmpc::content::PolicyFor;
 * the user's explicit settings win over it) */
#define PMC_POLICY_HOLD_REPAIR       1u    /* the held-drawing repair (animation mode) */
#define PMC_POLICY_ANIME_UPSCALER    2u    /* the anime upscalers' plain models */
#define PMC_POLICY_ANIME_UPSCALER_DS 4u    /* Auto may pick an upscaler's _DS (denoise + sharpen) variant */
#define PMC_POLICY_SIGN_HOLD         8u    /* typeset signs held over a pan */
#define PMC_POLICY_LINE_ART          16u   /* line-art protection */
#define PMC_POLICY_ANIME_DEBAND      32u   /* the anime deband preset */

typedef struct pmc_state {
    uint32_t struct_size;       /* set by the caller: sizeof(pmc_state) of its header (the library fills what both know) */
    int32_t cls;                /* PMC_CLASS_* (decided, with hysteresis) */
    int32_t cg;                 /* drawn, and the drawn look is rendered 3D CG (image model only); 0 otherwise */
    uint32_t policy;            /* PMC_POLICY_* for (cls, cg) */
    float confidence;           /* smoothed probability of the decided class (0 while measuring) */
    float drawn_share;          /* the last window's probability of drawn animation */
    float cg_share;             /* the image model's latest probability of CG among drawn looks (-1: none yet) */
    int32_t image_evidence;     /* 1: the last decision had the image model's evidence */
    int32_t model;              /* PMC_MODEL_* */
    float model_ms;             /* the image model's last run (ms; < 0 none yet) */
    uint32_t changes;           /* decided-class changes since the start */
    uint32_t windows;           /* windows evaluated */
    double first_decision_sec;  /* stream time of the first decision (-1 none yet) */
    uint32_t serial;            /* +1 on every change of cls, cg or model; a caller compares it to see a change */
    uint32_t reserved;
    uint64_t frames;            /* frames handed over */
    uint64_t processed;         /* frames the detector has taken */
    uint64_t dropped;           /* frames dropped because the detector fell behind (counted as a gap) */
    double caller_ms;           /* mean cost on the caller's thread per frame (cell grid, thumbnail, queue) */
    double detect_ms;           /* mean detector cost per frame (worker thread) */
    double detect_ms_max;       /* its maximum */
} pmc_state;

typedef struct pmc_ctx pmc_ctx;

PMC_API uint32_t pmc_abi(void);
/* NULL on failure (an ABI or struct_size mismatch, out of memory) */
PMC_API pmc_ctx *pmc_create(const pmc_config *cfg);
PMC_API void pmc_destroy(pmc_ctx *c);
/* One source frame in presentation order. y, u, v: YUV 4:2:0 planes (u holds Cb Cr interleaved when v is NULL: NV12,
 * P010); strides in bytes; bits: significant bits per sample, 8 (one byte a sample) or 9..16 (two bytes, little endian;
 * P010 = 16, its values sit in the top bits); limited: video range (16..235) rather than full range; pts in seconds;
 * discontinuity: a seek, a flush or a gap before this frame. 0 = taken, < 0 = refused (bad arguments). */
PMC_API int pmc_push_yuv420(pmc_ctx *c, const void *y, ptrdiff_t ystride, const void *u, const void *v, ptrdiff_t cstride,
                            int w, int h, int bits, int limited, double pts, int discontinuity);
/* the latest state; 0 = ok */
PMC_API int pmc_get_state(pmc_ctx *c, pmc_state *out);
/* the image model's evidence from the caller (tests, or a host that runs the model itself): pdrawn, pcg in 0..1 (pcg < 0:
 * none), stamped with the frame time pts. Applied before the next frame's window. */
PMC_API void pmc_set_image_evidence(pmc_ctx *c, double pdrawn, double pts, double pcg);
/* "measuring", "drawn animation", ... (static strings) */
PMC_API const char *pmc_class_name(int cls);

#ifdef __cplusplus
}
#endif
#endif
