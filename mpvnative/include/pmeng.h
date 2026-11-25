/* pmeng.h: the PushMPC engine module ABI (C only, versioned). FFmpeg's vf_pushmpc loads the module at run time
 * (pushmpc_engine.dll / libpushmpc_engine.so / .dylib) and drives it through this table; the module holds the engines
 * (pushcore on D3D11, the portable engine on the CPU, later the portable Vulkan engine of track T1).
 *
 * Why a separate module: the FFmpeg side stays a small, stable C file that compiles against any libavfilter 9..12
 * (our own FFmpeg build, and the per-version builds of the runtime hook), while the engines, Auto and the warm pushcore
 * instance live in one C++ binary that ships and updates with PushMPC. No CRT object, STL type or FILE* crosses this
 * boundary (the Windows module is MSVC /MT, FFmpeg is MinGW).
 *
 * Frame flow (one instance, one thread at a time; the filter serialises calls):
 *   open(device, stream, options) -> output description (memory, format, size, multiplier, colour)
 *   for each source frame: submit(src, pts_us, flags)   (PMENG_BUSY: take outputs first, then retry)
 *   while peek(&out, timeout) == 1: the filter allocates a frame of the output description, write(dst) fills it
 *   seek / discontinuity: submit with PMENG_SUBMIT_DISCONTINUITY (or flush)
 *   end of stream: pending() > 0 -> peek with a timeout until it is 0
 * Output order: for source s, the generated frames of the pair (s - 1, s) (subframe 1..multiplier-1), then the real
 * frame s (subframe 0). The filter computes the pts from the pair's real pts (exact for VFR sources too).
 */
#ifndef PMENG_H
#define PMENG_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PMENG_ABI_VERSION 1u

#if defined(_WIN32)
#define PMENG_EXPORT __declspec(dllexport)
#else
#define PMENG_EXPORT __attribute__((visibility("default")))
#endif

/* memory kinds */
#define PMENG_MEM_CPU    0u
#define PMENG_MEM_D3D11  1u   /* ID3D11Texture2D + array slice (FFmpeg AV_PIX_FMT_D3D11) */
#define PMENG_MEM_VULKAN 2u   /* AVVkFrame-style image + timeline semaphore (FFmpeg AV_PIX_FMT_VULKAN) */

/* pixel formats (the software format for hardware memory) */
#define PMENG_FMT_NONE      0u
#define PMENG_FMT_YUV420P   1u   /* 8-bit planar */
#define PMENG_FMT_YUV420P10 2u   /* 10-bit planar, LSB-aligned 16-bit words */
#define PMENG_FMT_NV12      3u
#define PMENG_FMT_P010      4u   /* 10-bit, MSB-aligned 16-bit words */
#define PMENG_FMT_BGRA      5u   /* DXGI_FORMAT_B8G8R8A8_UNORM / AV_PIX_FMT_BGRA */
#define PMENG_FMT_X2BGR10   6u   /* DXGI_FORMAT_R10G10B10A2_UNORM / AV_PIX_FMT_X2BGR10 */
#define PMENG_FMT_RGBAF16   7u   /* DXGI_FORMAT_R16G16B16A16_FLOAT / AV_PIX_FMT_RGBAF16 */

/* return codes */
#define PMENG_OK     0
#define PMENG_BUSY   1    /* submit: outputs must be taken first */
#define PMENG_ERROR  (-1) /* last_error has the reason; close the instance */
#define PMENG_DEVICE_LOST (-2)

#define PMENG_SUBMIT_DISCONTINUITY 1u  /* first frame after open / seek / a cut in the timestamps: no pair across it */

typedef struct pmeng_image {
    uint32_t mem, fmt, width, height;
    /* colour of this frame (input only; FFmpeg enums, 0 / 2 = unspecified) */
    int32_t range, matrix, primaries, transfer;
    /* CPU */
    uint8_t* data[4];
    ptrdiff_t linesize[4];
    /* D3D11 */
    void* d3d11_texture;          /* ID3D11Texture2D* */
    uint32_t d3d11_slice;
    /* Vulkan (reserved for the T1 engine; the module reports whether it takes it in pmeng_output.mem) */
    void* vk_frame;               /* AVVkFrame* */
    void* vk_frames_ctx;          /* AVVulkanFramesContext* */
} pmeng_image;

typedef struct pmeng_device {
    uint32_t struct_size;
    uint32_t mem;                 /* PMENG_MEM_* of the input frames */
    /* D3D11: the decoder's device (FFmpeg's AVD3D11VADeviceContext); lock/unlock guard the immediate context */
    void* d3d11_device;           /* ID3D11Device* */
    void (*lock)(void* ctx);
    void (*unlock)(void* ctx);
    void* lock_ctx;
    /* Vulkan: FFmpeg's AVVulkanDeviceContext* (instance, physical device, device, queues, get_proc_addr) */
    void* vk_device_ctx;
} pmeng_device;

typedef struct pmeng_stream {
    uint32_t struct_size;
    uint32_t width, height;       /* visible size */
    uint32_t fmt;                 /* PMENG_FMT_* (the software format) */
    int32_t fps_num, fps_den;     /* nominal source rate, 0 = unknown */
    /* FFmpeg enum values: AVColorRange, AVColorSpace, AVColorPrimaries, AVColorTransferCharacteristic */
    int32_t range, matrix, primaries, transfer;
    float mastering_peak_nits, max_cll_nits, max_fall_nits;  /* 0 = absent */
} pmeng_stream;

typedef struct pmeng_output {
    uint32_t struct_size;
    uint32_t mem;                 /* memory the module writes: equals the input memory, or PMENG_MEM_CPU (the filter
                                     then downloads the input and uploads the output: the copy fallback) */
    uint32_t fmt, width, height;  /* output frames */
    uint32_t multiplier;          /* outputs per source frame in steady state (1 = pass-through rate) */
    int32_t range, matrix, primaries, transfer;  /* output colour (FFmpeg enums) */
    uint32_t hdr_output;          /* 1 = HDR10 output (PQ, BT.2020) */
    uint32_t keep_dynamic_hdr;    /* 1 = the source's per-frame HDR / Dolby Vision metadata still applies */
    float mastering_peak_nits, mastering_min_nits, max_cll_nits, max_fall_nits; /* output static metadata, 0 = keep */
    char engine[32];              /* "pushcore", "portable-cpu", "portable-vulkan" */
    char description[512];        /* one line for the log */
} pmeng_output;

typedef struct pmeng_out_info {
    uint64_t source;              /* submit sequence number of the source frame (0-based since open) */
    uint32_t subframe;            /* 0 = the real frame `source`; j > 0 = generated, between source - 1 and source */
    uint32_t multiplier;          /* phase = subframe / multiplier */
    uint32_t generated;           /* 1 = generated (a held real frame on a cut still counts as generated) */
} pmeng_out_info;

typedef struct pmeng_api {
    uint32_t struct_size, abi_version;
    const char* (*version)(void);
    /* options: "key=value" lines separated by newlines (smoother, sharper, backend, display_fps, display_hdr, core_dir, adapter ...).
       Returns NULL on failure (err holds the reason). */
    void* (*open)(const pmeng_device* dev, const pmeng_stream* stream, const char* options, pmeng_output* out,
                  char* err, uint32_t err_size);
    int (*submit)(void* h, const pmeng_image* src, int64_t pts_us, uint32_t flags);
    /* 1 = an output is ready (described in *info), 0 = none within timeout_ms, < 0 = error */
    int (*peek)(void* h, pmeng_out_info* info, uint32_t timeout_ms);
    int (*write)(void* h, const pmeng_image* dst);   /* fills dst with the peeked output and pops it */
    int (*skip)(void* h);                            /* pops the peeked output without writing it */
    uint32_t (*pending)(void* h);                    /* outputs still to come for frames already submitted */
    int (*flush)(void* h);                           /* drops everything; the next submit starts a new pair */
    void (*close)(void* h);                          /* warm state (pushcore) may stay for the next open */
    const char* (*last_error)(void* h);
    int (*stats)(void* h, char* buf, uint32_t size); /* one line for the log */
} pmeng_api;

typedef const pmeng_api* (*pmeng_get_api_fn)(uint32_t abi_version);
PMENG_EXPORT const pmeng_api* pmeng_get_api(uint32_t abi_version);

#ifdef __cplusplus
}
#endif
#endif
