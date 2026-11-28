/*
 * pm_pushcore: a small C interface to pushcore (PushMPC's Windows engine: NVIDIA VFG / DLSS-G, PushFlow, FSR FG, AMD FRC
 * and PushFPS's scene-cut guard) for the VLC module, which is built with MinGW. This DLL is built with MSVC and reuses
 * the VapourSynth plugin's pushcore host (vsplugin/src/corehost.cpp: its own D3D11 device, CPU planes in through
 * NV12/P010 upload textures, YUV 4:2:0 out through a GPU conversion and a readback) and the filter's Auto rules
 * (filter/src/PushMpcAuto.h, with the user's PushMPC settings from HKCU\Software\PushMPC).
 *
 * By default (ABI 2, and ABI 3 with sr 0 and real_out 0) the output size is the source size and super resolution is
 * off; real frames stay the player's own decoded pictures and only the generated frames come from pushcore, so per-frame
 * look changes (Neural Rendering, deband) are off too. HD/UHD sources only (BT.709 / BT.2020): pushcore's YUV output uses
 * those matrices.
 *
 * ABI 3 (appended, an ABI 2 caller keeps working):
 *  - sr 1: pushcore's own upscaler to a fixed output size for the whole stream (pushcore's target box that holds
 *    out_width x out_height, the source's shape kept; pm_pc_output gives the real size). Every picture, real ones too,
 *    then comes from pushcore. The upscaler is Video Super Resolution (RTX VSR; Intel VSR / FSR / NIS on other GPUs)
 *    or, while the picture is drawn, the anime upscaler (PUSHCORE_SR_METHOD_PMML) at the same output size;
 *  - the measured content (ContentDetect's class and CG flag) goes into Auto as on every other route: Auto's drawn
 *    treatment (held-frame re-timing, the animation mode, line art, sign hold, the anime deband preset, the anime
 *    upscaler) follows it, unless the caller sets those fields itself;
 *  - pm_pc_update: new values for a running stream (the measured class changed, another anime model), applied warm
 *    with pushcore's setSettings + reload (frames in flight are dropped, as at a seek). The caller limits how often.
 * Held-frame re-timing makes pushcore wait for up to a dozen source frames before it hands out a pair's frames (the
 * outputs keep their times): only a caller that lets its source run that far ahead can have it (held_frames).
 */
#ifndef PM_PUSHCORE_H
#define PM_PUSHCORE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define PM_PC_ABI 3u       /* 2: pm_pc_cfg.fullscreen appended; 3: struct_size and the fields after it */
#define PM_PC_ABI_MIN 2u   /* the oldest pm_pc_cfg this bridge still takes */

typedef struct pm_pc pm_pc;

#define PM_PC_AUTO (-1)          /* held_frames / deband: Auto decides (from the measured content) */
#define PM_PC_ANIME_AUTO (-2)    /* anime_model: Auto's pick, from the measured class and the GPU */
#define PM_PC_ANIME_NONE (-1)    /* anime_model: Video Super Resolution only */

typedef struct pm_pc_cfg {
    uint32_t abi;                 /* PM_PC_ABI (2 = a caller built before ABI 3: its struct ends at fullscreen) */
    uint32_t width, height;       /* visible source size */
    int bits;                     /* 8, or 10..16 (2 bytes per sample, little endian) */
    int fps_num, fps_den;         /* nominal source rate */
    int matrix, transfer, primaries, range;  /* H.273 codes; range 1 limited, 2 full */
    int smoother;                 /* VLC's setting: 0 Auto (PushMPC's own Smoother setting), 1 off, 2..16 */
    double display_hz;            /* 0 = the primary display */
    int warp;                     /* 1 = WARP adapter (CI with the stub core) */
    const wchar_t *core_dir;      /* folder with pushcore.dll; NULL = the PushMPC install (%LOCALAPPDATA%\PushMPC\core) */
    int fullscreen;               /* 1 = VLC plays full screen (its "fullscreen" option), 0 = windowed or not known (appended) */
    /* ---- ABI 3 ---- */
    uint32_t struct_size;         /* sizeof(pm_pc_cfg) as the caller was built (later ABIs append after this) */
    int sr;                       /* 1 = pushcore's upscaler to a fixed output size (out_width x out_height); 0 = the source size */
    uint32_t out_width, out_height;   /* sr 1: the size the picture is shown at (the screen, the source's shape kept) */
    int anime_model;              /* sr 1, while the picture is drawn: PM_PC_ANIME_AUTO, PM_PC_ANIME_NONE, or a model
                                     (PUSHCORE_ANIME_SR_*: the caller's own switch); never when the GPU cannot run it */
    int content_class;            /* ContentDetect's class (pushmpc/content_detect.h: 0 measuring, 1 drawn, 2 3D CG,
                                     3 not drawn, 4 game footage), measured in the picture */
    int content_cg;               /* 1 = drawn and rendered 3D CG (no sign hold, line art, anime deband or _DS model) */
    int held_frames;              /* PM_PC_AUTO (Auto: on with frame generation unless Animation is Off), 0 off, 1, 2
                                     (PushCoreSettings.heldFrames). Adds latency (see above): Auto here is off unless
                                     real_out is 1 and the caller says it waits (held_wait) */
    int deband;                   /* PM_PC_AUTO (Auto with the measured class: the anime preset on drawn pictures), 0 off,
                                     1 on; off whenever the real frames are the player's own (real_out 0, sr 0) */
    int real_out;                 /* 1 = the real frames also come out of pushcore (dest / done, at their source time);
                                     always with sr 1 */
    int held_wait;                /* 1 = the caller takes a pair's frames up to a dozen source frames later (held-frame
                                     re-timing then runs in Auto) */
    int measure;                  /* zero copy: 1 = ContentDetect measures the textures pushed, on the GPU (the cells and
                                     nothing else are read back; the player's pictures are never read on the CPU); the
                                     class in pm_pc_info.measured_class, for the caller to hand back through pm_pc_update */
} pm_pc_cfg;

/* what runs (pm_pc_output) */
typedef struct pm_pc_info {
    uint32_t struct_size;         /* sizeof(pm_pc_info) as the caller was built */
    uint32_t width, height;       /* the output size: every picture pushcore hands out */
    int k;                        /* outputs per source frame */
    int sr;                       /* pushcore's upscaler runs */
    uint32_t sr_method;           /* PUSHCORE_SR_METHOD_* */
    int anime_model;              /* PUSHCORE_ANIME_SR_* running (sr_method PMML), else -1 */
    int held_frames;              /* PushCoreSettings.heldFrames */
    int animation_mode;           /* PUSHCORE_ANIM_* */
    uint32_t hold_flags;          /* PUSHCORE_HOLD_* */
    int deband;                   /* the deband pass runs */
    uint32_t deband_strength;     /* PUSHCORE_DEBAND_* */
    int real_out;                 /* real frames come out of pushcore */
    int content_class, content_cg;   /* what Auto was given */
    int updates;                  /* pm_pc_update calls that changed pushcore's settings */
    int measured_class, measured_cg;  /* measure 1: ContentDetect's class of the pictures so far (0 = measuring) */
} pm_pc_info;

typedef struct pm_pc_planes { const uint8_t *data[3]; ptrdiff_t stride[3]; } pm_pc_planes;
typedef struct pm_pc_out { uint8_t *data[3]; ptrdiff_t stride[3]; } pm_pc_out;

/* for each generated frame pushcore hands out: return where to write it (NULL = not wanted). pts in 100 ns, on the
 * clock of the submitted pts */
typedef const pm_pc_out *(*pm_pc_dest_fn)(void *user, int64_t pts100ns);
/* after the pixels are in */
typedef void (*pm_pc_done_fn)(void *user, int64_t pts100ns);

/* NULL on failure (reason in err). *k = outputs per source frame pushcore will produce (1 = no frame generation) */
__declspec(dllexport) pm_pc *pm_pc_open(const pm_pc_cfg *cfg, int *k, char *err, size_t errsize);
__declspec(dllexport) int pm_pc_push(pm_pc *pc, const pm_pc_planes *src, int64_t pts100ns, int discontinuity, char *err, size_t errsize);
/* hands out what pushcore has finished, waiting up to timeout_ms for the first frame; returns the number of frames
 * polled (real and generated; real ones are returned to pushcore at once) or < 0 */
__declspec(dllexport) int pm_pc_poll(pm_pc *pc, uint32_t timeout_ms, pm_pc_dest_fn dest, pm_pc_done_fn done, void *user, char *err, size_t errsize);
__declspec(dllexport) int pm_pc_flush(pm_pc *pc);
__declspec(dllexport) void pm_pc_close(pm_pc *pc);
/* one line: what runs (pushcore's description and the Auto decision) */
__declspec(dllexport) const char *pm_pc_describe(pm_pc *pc);

/* Zero copy (VLC's Direct3D 11 hardware decoding, 'DX11' / 'DX10' pictures): pushcore runs on the player's own D3D11
 * device and reads the decoder's texture slices; each generated frame is converted to NV12 / P010 on that device and
 * copied into a texture slice of the player's (a picture of VLC's pool). context_mutex is VLC's lock on the immediate
 * context (GUID_CONTEXT_MUTEX, may be NULL); every use of the context here holds it, and the conversion runs in its
 * own device context state, so VLC's pipeline state is untouched. */
typedef struct pm_pc_tex { void *texture; unsigned slice; } pm_pc_tex;   /* ID3D11Texture2D *, array slice */
typedef const pm_pc_tex *(*pm_pc_dest_tex_fn)(void *user, int64_t pts100ns);
__declspec(dllexport) pm_pc *pm_pc_open_d3d11(const pm_pc_cfg *cfg, void *d3d11_device, void *context_mutex, int *k, char *err, size_t errsize);
__declspec(dllexport) int pm_pc_push_d3d11(pm_pc *pc, void *texture, unsigned slice, int64_t pts100ns, int discontinuity, char *err, size_t errsize);
__declspec(dllexport) int pm_pc_poll_d3d11(pm_pc *pc, uint32_t timeout_ms, pm_pc_dest_tex_fn dest, pm_pc_done_fn done, void *user, char *err, size_t errsize);
/* a generated picture pushcore did not deliver in time: the real frame copied into it, on the GPU */
__declspec(dllexport) int pm_pc_copy_d3d11(pm_pc *pc, void *src, unsigned src_slice, void *dst, unsigned dst_slice);
typedef int (*pm_pc_copy_d3d11_fn)(pm_pc *, void *, unsigned, void *, unsigned);

typedef pm_pc *(*pm_pc_open_d3d11_fn)(const pm_pc_cfg *, void *, void *, int *, char *, size_t);
typedef int (*pm_pc_push_d3d11_fn)(pm_pc *, void *, unsigned, int64_t, int, char *, size_t);
typedef int (*pm_pc_poll_d3d11_fn)(pm_pc *, uint32_t, pm_pc_dest_tex_fn, pm_pc_done_fn, void *, char *, size_t);

/* ABI 3. pm_pc_abi: the newest pm_pc_cfg this bridge takes (an older bridge has no such export: ABI 2).
 * pm_pc_update: the stream's settings again from cfg (same source, output size and sr as at the open: only
 * content_class, content_cg, anime_model, held_frames, deband, held_wait and real_out are read; real_out switches where
 * the real frames come from at the next picture, so a caller turns held frames on only once it needs them); returns 0 = applied (pushcore
 * reloaded warm), 1 = nothing changed, < 0 = refused or failed (the open settings keep running). The multiplier stays the
 * open's; at that multiplier Auto's frame generation method follows the class (animation: DLSS-G after PushFlow).
 * pm_pc_output: what runs; 0 or < 0. */
__declspec(dllexport) uint32_t pm_pc_abi(void);
__declspec(dllexport) int pm_pc_update(pm_pc *pc, const pm_pc_cfg *cfg, char *err, size_t errsize);
__declspec(dllexport) int pm_pc_output(pm_pc *pc, pm_pc_info *info);
typedef uint32_t (*pm_pc_abi_fn)(void);
typedef int (*pm_pc_update_fn)(pm_pc *, const pm_pc_cfg *, char *, size_t);
typedef int (*pm_pc_output_fn)(pm_pc *, pm_pc_info *);

/* Sharing the process's one pushcore (additive exports, no ABI change: a caller looks them up and keeps its older
 * behaviour on a bridge without them). The host hands the core to the instance that opened it last; the previous one's
 * calls then fail ("taken over").
 * pm_pc_decide: the open's decision (the same configuration checks, the user's settings, Auto) up to the multiplier,
 *   without taking the core or a device; *k = the multiplier Auto asks pushcore for (the open's own k can still differ:
 *   honour it). 0, or < 0 with the reason in err (on 0, err holds a short summary).
 * pm_pc_busy: 1 while an instance holds the core (lock-free; also 1 when that instance is the caller's own).
 * pm_pc_measure_open / pm_pc_measure_push: ContentDetect alone on the caller's D3D11 device and immediate context (the
 *   cells only are read back), without the core; the class in pm_pc_output's measured_class / measured_cg; pm_pc_close
 *   ends it. Only width, height, bits and range of cfg are read. */
__declspec(dllexport) int pm_pc_decide(const pm_pc_cfg *cfg, int *k, char *err, size_t errsize);
__declspec(dllexport) int pm_pc_busy(void);
__declspec(dllexport) pm_pc *pm_pc_measure_open(const pm_pc_cfg *cfg, void *d3d11_device, void *context_mutex, char *err, size_t errsize);
__declspec(dllexport) int pm_pc_measure_push(pm_pc *pc, void *texture, unsigned slice, int64_t pts100ns, int discontinuity, char *err, size_t errsize);
typedef int (*pm_pc_decide_fn)(const pm_pc_cfg *, int *, char *, size_t);
typedef int (*pm_pc_busy_fn)(void);
typedef pm_pc *(*pm_pc_measure_open_fn)(const pm_pc_cfg *, void *, void *, char *, size_t);
typedef int (*pm_pc_measure_push_fn)(pm_pc *, void *, unsigned, int64_t, int, char *, size_t);

typedef pm_pc *(*pm_pc_open_fn)(const pm_pc_cfg *, int *, char *, size_t);
typedef int (*pm_pc_push_fn)(pm_pc *, const pm_pc_planes *, int64_t, int, char *, size_t);
typedef int (*pm_pc_poll_fn)(pm_pc *, uint32_t, pm_pc_dest_fn, pm_pc_done_fn, void *, char *, size_t);
typedef int (*pm_pc_flush_fn)(pm_pc *);
typedef void (*pm_pc_close_fn)(pm_pc *);
typedef const char *(*pm_pc_describe_fn)(pm_pc *);

#ifdef __cplusplus
}
#endif
#endif
