/*
 * pm_pushcore: a small C interface to pushcore (PushMPC's Windows engine: NVIDIA VFG / DLSS-G, PushFlow, FSR FG, AMD FRC
 * and PushFPS's scene-cut guard) for the VLC module, which is built with MinGW. This DLL is built with MSVC and reuses
 * the VapourSynth plugin's pushcore host (vsplugin/src/corehost.cpp: its own D3D11 device, CPU planes in through
 * NV12/P010 upload textures, YUV 4:2:0 out through a GPU conversion and a readback) and the filter's Auto rules
 * (filter/src/PushMpcAuto.h, with the user's PushMPC settings from HKCU\Software\PushMPC).
 *
 * In VLC the output size must stay the source size, so super resolution is off; real frames stay VLC's own decoded
 * pictures and only the generated frames come from pushcore, so per-frame look changes (AI clean-up, deband) are off
 * too. HD/UHD sources only (BT.709 / BT.2020): pushcore's YUV output uses those matrices.
 */
#ifndef PM_PUSHCORE_H
#define PM_PUSHCORE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define PM_PC_ABI 2u   /* 2: pm_pc_cfg.fullscreen appended */

typedef struct pm_pc pm_pc;

typedef struct pm_pc_cfg {
    uint32_t abi;                 /* PM_PC_ABI */
    uint32_t width, height;       /* visible source size */
    int bits;                     /* 8, or 10..16 (2 bytes per sample, little endian) */
    int fps_num, fps_den;         /* nominal source rate */
    int matrix, transfer, primaries, range;  /* H.273 codes; range 1 limited, 2 full */
    int smoother;                 /* VLC's setting: 0 Auto (PushMPC's own Smoother setting), 1 off, 2..16 */
    double display_hz;            /* 0 = the primary display */
    int warp;                     /* 1 = WARP adapter (CI with the stub core) */
    const wchar_t *core_dir;      /* folder with pushcore.dll; NULL = the PushMPC install (%LOCALAPPDATA%\PushMPC\core) */
    int fullscreen;               /* 1 = VLC plays full screen (its "fullscreen" option), 0 = windowed or not known (appended) */
} pm_pc_cfg;

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
