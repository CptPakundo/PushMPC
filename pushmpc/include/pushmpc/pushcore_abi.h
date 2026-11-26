#pragma once
/* pushcore.dll: PushFPS's enhancement graph (RTX Video SR, NVIDIA VFG and the rest of the saved
 * chain) as a GPU-in, GPU-out service for a D3D11 video renderer (the MPC-VR fork).
 *
 * C ABI only: plain structs, opaque handles and COM pointers. No STL object, CRT heap block or
 * FILE* crosses this boundary (both sides link the static CRT, /MT).
 *
 * Threading: one PushCore is not thread-safe. Serialise all calls on one instance (the caller's
 * bridge holds a lock). pushcore records D3D11 work on the device's immediate context
 * (ingress copy, fence signals/waits), so the D3D11 device must be multithread-protected
 * (ID3D10Multithread::SetMultithreadProtected(TRUE), which MPC-VR and FFmpeg's D3D11VA set) when
 * other threads use the same context.
 *
 * Frame flow:
 *   pushcore_canSubmit -> pushcore_submit(texture, slice, pts)      (decoder thread, no CPU wait)
 *   pushcore_poll -> frames in presentation order, each with a D3D11 texture and a fence to wait
 *   caller: ID3D11DeviceContext4::Wait(frame.readyFence, frame.readyValue); draw frame.texture
 *   pushcore_presented(lease, context)  (signals the release fence on `context` after the draw)
 * Frames must be returned with pushcore_presented in the order poll handed them out.
 * A source frame can only be submitted once the outputs of the source frame two before it have
 * been returned (the graph has two real-frame slots, by design parity with PushFPS).
 */
#include <stdint.h>

#ifdef __cplusplus
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct ID3D11Fence;
extern "C" {
#else
typedef struct ID3D11Device ID3D11Device;
typedef struct ID3D11DeviceContext ID3D11DeviceContext;
typedef struct ID3D11Texture2D ID3D11Texture2D;
typedef struct ID3D11Fence ID3D11Fence;
#endif

#ifdef PUSHCORE_BUILD
#define PUSHCORE_API __declspec(dllexport)
#else
#define PUSHCORE_API
#endif

#define PUSHCORE_ABI_VERSION 3u  /* 2 (M2b): HDR fields in StreamInfo/OutputInfo, submit2 with the clock (FG admission),
                                    admission counters in Stats. 3: PushCoreSettings (PushMPC's own settings instead of a
                                    PushFPS chain session), shaderDir, per-stage NR / RTX Video HDR timings. A caller built
                                    for another version refuses this DLL and vice versa (the filter and the core ship together). */

typedef struct PushCore PushCore; /* opaque */

/* Return codes */
#define PUSHCORE_OK 0
#define PUSHCORE_BUSY 1      /* not now: return outputs (presented) or poll first, then retry */
#define PUSHCORE_ERROR (-1)  /* see pushcore_lastError; the instance should be closed */
#define PUSHCORE_NEEDS_REBUILD 2 /* M2c, pushcore_applySettings: the new settings change the graph; call pushcore_reload */
#define PUSHCORE_DEVICE_LOST (-2) /* the D3D12 or the D3D11 device was removed/reset (TDR): close the instance;
                                     the caller's D3D11 device is probably gone too (lastError has the reason) */

/* Input formats (the D3D11 texture's DXGI format must match) */
#define PUSHCORE_INPUT_NV12 0u /* DXGI_FORMAT_NV12 */
#define PUSHCORE_INPUT_P010 1u /* DXGI_FORMAT_P010 (P016 is passed as P010) */

/* Config flags */
#define PUSHCORE_FLAG_EGRESS_COPY 1u /* force the fallback egress (one D3D12 copy per output into a shared ring) */
#define PUSHCORE_FLAG_NO_WARMUP   2u /* skip the warm-up frames pushcore_open runs through the graph (first-frame stalls) */
#define PUSHCORE_FLAG_NO_WARM_CACHE 4u /* do not keep the NGX core (and its D3D12 device) alive for the next instance */

/* Egress modes (PushCoreOutputInfo.egressMode) */
#define PUSHCORE_EGRESS_DIRECT 0u /* graph output pool shared and opened in D3D11 (design option A) */
#define PUSHCORE_EGRESS_COPY 1u   /* fallback copy ring (design R2 fallback) */

/* Submit flags */
#define PUSHCORE_SUBMIT_DISCONTINUITY 1u /* first frame after open / seek / flush: real frame only, history reset */

/* Frame kinds */
#define PUSHCORE_FRAME_REAL 0u
#define PUSHCORE_FRAME_GENERATED 1u

typedef struct PushCoreConfig {
    uint32_t structSize;       /* sizeof(PushCoreConfig) */
    uint32_t abiVersion;       /* PUSHCORE_ABI_VERSION */
    const wchar_t* pushfpsDir; /* PushFPS folder (shaders, runtime, runtime_local). NULL = pushcore.dll's folder */
    const wchar_t* dataDir;    /* folder with chain-session.v1. NULL = <pushfpsDir>\runtime_local\user-data-2.0.0;
                                  L"" = built-in defaults */
    const wchar_t* runtimeDir; /* NVIDIA runtimes. NULL = PushFPS's own rule (<pushfpsDir>\runtime\experimental) */
    const wchar_t* logFile;    /* PushFPS engine log file. NULL = no file log */
    int32_t sr, fg;            /* overrides on the saved chain: -1 keep, 0 off, 1 on */
    int32_t multiplier;        /* -1 keep, else 2..8 (2..PUSHCORE_MAX_MULTIPLIER with PUSHCORE_FEATURE_VFG16) */
    int32_t vfgQuality;        /* -1 keep */
    uint32_t flags;            /* PUSHCORE_FLAG_* */
    /* ABI 3 */
    const wchar_t* shaderDir;  /* compiled shaders (*.dxil). NULL = <pushfpsDir>\shaders */
    const struct PushCoreSettings* settings; /* NULL = the saved PushFPS chain session in dataDir (as before ABI 3) */
} PushCoreConfig;

/* ABI 3: PushMPC's settings, already resolved (the caller decides "Auto"). pushcore builds PushFPS's EnhancementSettings
   from a fresh PushFPS install's defaults, then sets exactly the fields PushFPS's simple video page (WatchPage.qml:
   smoother / sharper / AI picture enhancement / SDR to HDR) sets for its switches, plus these values. */
#define PUSHCORE_SETTINGS_PUSHMPC 0u    /* the values below */
#define PUSHCORE_SR_TARGET_1440P 0u     /* veyra::pipeline::SrTarget values */
#define PUSHCORE_SR_TARGET_4K 1u
#define PUSHCORE_SR_TARGET_5K 3u
typedef struct PushCoreSettings {
    uint32_t structSize;       /* sizeof(PushCoreSettings) */
    uint32_t source;           /* PUSHCORE_SETTINGS_PUSHMPC */
    int32_t sr;                /* 1 = RTX Video Super Resolution (on a source at the output size: its artifact reduction) */
    uint32_t srQuality;        /* 1..4 = RTX VSR quality (videoSrQuality, standard family) */
    uint32_t srTarget;         /* PUSHCORE_SR_TARGET_* */
    uint32_t multiplier;       /* 1 = no frame generation, 2..8 = NVIDIA VFG multiplier (2..16 with PUSHCORE_FEATURE_VFG16) */
    uint32_t vfgQuality;       /* 0 Low, 1 Medium, 2 High (NvVFX model) */
    int32_t nr;                /* 1 = AI picture enhancement (PushFPS "AI 画面增强", NR) */
    float nrTone, nrStructure; /* 0..1, PushFPS's look presets: 0.4/0.5 subtle, 0.7/0.8 balanced, 1/1 strong */
    int32_t videoHdr;          /* 1 = RTX Video HDR (SDR to HDR; only shown as HDR when StreamInfo.displayHdr) */
    /* Appended in the same ABI (read only when structSize covers them; a smaller struct gets the defaults below):
       pushcore_features() has PUSHCORE_FEATURE_SR_FAMILY and PUSHCORE_FEATURE_DEBAND. */
    uint32_t srFamily;         /* PUSHCORE_SR_FAMILY_*: RTX VSR model family; default standard. High bitrate and streaming
                                  are NVIDIA Video Effects VideoSuperRes (runtime\nvidia-vfg\nvVFXVideoSuperRes.dll + its
                                  nvngx_vsr.dll); without those files pushcore keeps the standard family and logs it.
                                  Only the standard family restores a source already at the target size. */
    int32_t deband;            /* 1 = PushMPC's deband pass on the source before the graph (8-bit SDR sources only;
                                  10-bit and HDR sources are left as they are); default 0 */
    uint32_t debandStrength;   /* PUSHCORE_DEBAND_*; default medium */
    /* Appended (PushMPC 0.6, pushcore_features() PUSHCORE_FEATURE_FG_METHODS; a smaller struct gets the defaults):
       the frame generation method, the upscaler, the scene-cut guard. */
    uint32_t fgBackend;        /* PUSHCORE_FG_*; default VFG. Unusable on this GPU or without its files: pushcore falls
                                  back along VFG -> DLSS-G -> PushFlow -> FSR FG -> none and reports what runs in
                                  PushCoreStats.fgBackend / PushCoreOutputInfo.description */
    uint32_t fgTier;           /* PushFlow: 0 Fast, 1 Balanced, 2 Quality (VFG uses vfgQuality); default 1 */
    uint32_t srMethod;         /* PUSHCORE_SR_METHOD_*; default RTX VSR (on a non-NVIDIA GPU it becomes FSR) */
    int32_t cutGuard;          /* 1 = the scene-cut guard for every frame generation method (default); 0 = VFG's own
                                  pair test only, as PushFPS */
    float activeArea[4];       /* x0, y0, x1, y1: the picture area the cut test looks at (fractions of the source);
                                  x1 <= x0 = find letterbox / pillarbox bars itself (default) */
    int32_t fgLite;            /* 1 = DLSS-G performance mode (NVIDIA's "FG Lite" runtime, RTX 50) when DLSS-G runs */
    /* Appended (PUSHCORE_FEATURE_HDR_PEAK): RTX Video HDR's peak luminance, cd/m2 (400..2000; 0 = PushFPS's 1000) */
    uint32_t videoHdrPeak;
    /* Appended (PUSHCORE_FEATURE_SUBTITLE_HOLD, PushFPS 4.5): PushFPS's Subtitle Hold. Text that stands still in the
       bottom subtitle band (the middle 62.5 % of the width, 400/2160 of the height, 4 % above the bottom edge) while the
       picture round it moves is taken out of what frame generation is given and laid back over what it makes, so
       burned-in subtitles do not warp or ghost. Only thin, contrasting bars that have stood still for several moving
       frames are masked, never the whole band. Soft subtitles are drawn by the player after the filter and never reach
       it. 1 = on; default 0 (a smaller struct). Runs with VFG, DLSS-G, PushFlow (not Predict), FSR FG and AMD FRC.
       2 = Auto (PUSHCORE_FEATURE_SUBTITLE_HOLD_AUTO): the pass detects on every frame but holds only while burned-in text
       is found (>= 0.4 % of the frame for ~1.5 s; off after ~5 s below 0.05 %); PushCoreStats.subtitleHoldState 4 =
       running, waiting for text. */
    int32_t subtitleHold;
    /* Appended (PUSHCORE_FEATURE_HELD_FRAMES): held-frame re-timing. 1 = a source frame that repeats the one before it
       (animation drawn on twos or threes) is not given to frame generation; the frames generated between the two
       distinct frames around it are spread over the whole held span, so the motion no longer moves in one step and
       holds in the next. Runs of more than three held frames (a still) are shown as they are. The outputs keep the
       source timestamps; pushcore takes up to 9 source frames ahead to wait for the next distinct one. 2 = the same, and
       the per-region source repair runs without waiting for held drawings to be found (Animation On). Default 0. */
    int32_t heldFrames;
    /* Appended (PUSHCORE_FEATURE_ANIME_HOLD): how drawn animation is shown (with heldFrames != 0).
       animationMode: 0 Full (default: held drawings re-timed, every part moves at its own cadence), 1 Camera only (keep
       the drawn feel: what slides rigidly on ones - pans, scrolls, a cel slid over the background - stays smooth; what
       is redrawn shows each drawing for its own time, camera-warped, as drawn; no source repair), 2 Off (plain frame
       generation, no re-timing). holdFlags (PUSHCORE_HOLD_*): generated frames show the real frame where frame
       generation cannot be right; default 0 (a smaller struct). */
    int32_t animationMode;
    uint32_t holdFlags;
} PushCoreSettings;
#define PUSHCORE_ANIM_FULL 0
#define PUSHCORE_ANIM_CAMERA_ONLY 1
#define PUSHCORE_ANIM_OFF 2
#define PUSHCORE_HOLD_TRANSIENTS 1u      /* flashes and impact frames shown as drawn (nearest real frame, whole picture), and
                                            objects no motion explains (smears, appearing objects) on animation */
#define PUSHCORE_HOLD_LINE_ART 2u        /* line contrast of generated frames matched to the drawing; doubled lines replaced */
#define PUSHCORE_HOLD_SIGNS 4u           /* typeset signs (text drawn into the picture) held with the Subtitle Hold pass */
#define PUSHCORE_HOLD_FILM_TIME 16u      /* P9: drawn parts of 24p-grid shots in 29.97 video switch at film time (Camera only;
                                            off by default until the verdict) */
#define PUSHCORE_HOLD_LIVE_TRANSIENTS 8u /* flash / impact hold on every video, live action too */
#define PUSHCORE_SETTINGS_SIZE_BASE 44u  /* sizeof(PushCoreSettings) up to videoHdr (the first ABI 3 layout) */
#define PUSHCORE_SETTINGS_SIZE_V2 56u    /* ... up to debandStrength (before the frame generation methods) */
#define PUSHCORE_FG_VFG 0u         /* NVIDIA Video Effects frame generation (RTX 40/50; RTX 30 through VfgAmpereCompat) */
#define PUSHCORE_FG_DLSSG 1u       /* NVIDIA DLSS frame generation, FidelityFX optical flow (RTX 40/50; RTX 30 unlocked) */
#define PUSHCORE_FG_PUSHFLOW 2u    /* PushFlow (RIFE on DirectML), any DirectX 12 GPU, up to 4x */
#define PUSHCORE_FG_FSR 3u         /* AMD FSR 3.1 frame generation (any DirectX 12 GPU; FSR 4 ML on RX 9000), 2x */
#define PUSHCORE_FG_AMDFRC 4u      /* AMD AMF frame rate conversion (AMD driver), 2x */
#define PUSHCORE_FG_NVOF 5u        /* PUSHCORE_FEATURE_NVOF_FG: NVIDIA's optical-flow hardware (NVOFA: Turing except TU117, and
                                      later) as the motion of FSR 3.1's frame interpolation and warp, 2x: the flow costs no
                                      shader time (GTX 16 / RTX 20 where VFG does not run and PushFlow is too slow) */
#define PUSHCORE_FG_NONE 255u      /* (reported only) no frame generation could start */
#define PUSHCORE_SR_METHOD_RTX 0u  /* RTX Video Super Resolution (srQuality / srFamily) */
#define PUSHCORE_SR_METHOD_FSR 1u  /* AMD FSR upscaler (FSR 4.1 / INT8 where the driver offers it, else FSR 3.1) */
#define PUSHCORE_SR_METHOD_NIS 2u  /* NVIDIA Image Scaling (shaders, any GPU) */
#define PUSHCORE_SR_FAMILY_STANDARD 0u      /* RTX Video SDK (NGX), tiers 1..4 = videoSrQuality 1..4 */
#define PUSHCORE_SR_FAMILY_HIGH_BITRATE 1u  /* VFX VideoSuperRes, keeps detail (no compression clean-up): 16..19 */
#define PUSHCORE_SR_FAMILY_STREAMING 2u     /* VFX VideoSuperRes, cleans hardest (Ampere+): tiers 1-2 -> 21, 3-4 -> 23 */
#define PUSHCORE_DEBAND_LOW 0u
#define PUSHCORE_DEBAND_MEDIUM 1u
#define PUSHCORE_DEBAND_HIGH 2u
#define PUSHCORE_DEBAND_ANIME 3u   /* anime round 5 (PUSHCORE_FEATURE_ANIME_HOLD cores): wider radius for flat fills, less grain */

typedef struct PushCoreStreamInfo {
    uint32_t structSize;       /* sizeof(PushCoreStreamInfo) */
    uint32_t width, height;    /* visible size (the texture may be larger: decoder padding) */
    uint32_t format;           /* PUSHCORE_INPUT_* */
    int32_t fpsNum, fpsDen;    /* nominal source rate, 0 = unknown */
    /* FFmpeg enum values: AVColorRange (0 unspecified, 1 limited, 2 full), AVColorSpace,
       AVColorPrimaries, AVColorTransferCharacteristic (2 = unspecified). */
    int32_t range, matrix, primaries, transfer;
    /* ABI 2: static HDR metadata of a PQ/HLG source (cd/m2, 0 = absent) and the presenter's mode */
    float masteringPeakNits, maxCllNits, maxFallNits;
    uint32_t displayHdr;       /* 1 = the renderer's swap chain presents HDR10 (PQ, BT.2020): PushFPS's rule
                                  useHdrPreview(hdrInput, displayHdr) then decides HDR10 output; 0 = SDR presenter */
} PushCoreStreamInfo;

typedef struct PushCoreOutputInfo {
    uint32_t width, height;    /* output (work) size, e.g. 3840x2080 */
    uint32_t dxgiFormat;       /* DXGI_FORMAT of the output textures (28 = R8G8B8A8_UNORM for SDR) */
    uint32_t multiplier;       /* outputs per source frame in steady state (1 = no frame generation) */
    uint32_t egressMode;       /* PUSHCORE_EGRESS_* actually in use */
    uint64_t adapterLuid;      /* adapter of both devices */
    char description[512];     /* UTF-8, e.g. "source 1920x1040 NV12 -> output 3840x2080; SR on ...; FG on NVIDIA-VFG 4X" */
    /* ABI 2 */
    uint32_t hdrInput;         /* 1 = PQ/HLG source on PushFPS's HDR path */
    uint32_t hdrOutput;        /* 1 = HDR10 output (R10G10B10A2, PQ, BT.2020): present with
                                  DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020; 0 = SDR (sRGB-encoded RGB8, tone-mapped
                                  by PushFPS when hdrInput): present with DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 */
    uint32_t hdrMetadataHint;  /* 1 = send hdr10Metadata (PushFPS's "HDR metadata" setting); 0 = clear the swap chain's */
    uint32_t hdrCurveNotApplied; /* 1 = the saved PushFPS HDR curve is on; it lives in PushFPS's present shader, not here */
    struct {                   /* same layout as DXGI_HDR_METADATA_HDR10 */
        uint16_t redPrimary[2], greenPrimary[2], bluePrimary[2], whitePoint[2];
        uint32_t maxMasteringLuminance, minMasteringLuminance;
        uint16_t maxContentLightLevel, maxFrameAverageLightLevel;
    } hdr10Metadata;
} PushCoreOutputInfo;

typedef struct PushCoreFrame {
    ID3D11Texture2D* texture;  /* borrowed, valid until pushcore_presented(lease) */
    ID3D11Fence* readyFence;   /* borrowed; the caller's context waits on it before reading texture */
    uint64_t readyValue;
    int64_t pts100ns;          /* presentation time; real frames keep the submitted pts */
    uint64_t sourceIndex;      /* 0-based index of the submitted source frame this output belongs to */
    uint64_t lease;            /* token for pushcore_presented */
    uint32_t kind;             /* PUSHCORE_FRAME_* */
    uint32_t subframe;         /* 0 for real, 1..multiplier-1 for generated */
    uint32_t width, height;
} PushCoreFrame;

typedef struct PushCoreStats {
    uint32_t structSize;       /* sizeof(PushCoreStats) */
    uint64_t submitted, outputs, real, generated;
    uint64_t skippedGenerated; /* generated frames the graph did not produce (budget, failure) */
    uint64_t busy;             /* submit calls answered PUSHCORE_BUSY */
    uint64_t leasesOut;        /* frames handed out and not yet returned */
    uint64_t pendingFrames;    /* frames submitted to the GPU and not yet handed out */
    double submitCpuMs;        /* total CPU time inside pushcore_submit */
    /* ABI 2: frame generation admission (PushFPS's deadline rule for files), per source pair */
    uint64_t fgFull;           /* every generated frame of the pair evaluated */
    uint64_t fgReduced;        /* only the midpoint (2X group): the full group would miss its deadline */
    uint64_t fgSkipped;        /* no generated frame: not even the midpoint fits (history kept) */
    /* M2c (additive, same ABI version: .ax and pushcore.dll ship together): measured GPU cost per source frame
       (colour + VSR + FG group, D3D12 timestamps), median; < 0 = no sample. "Recent" = the last 3 s, "window" = the
       last 20 s (the last few samples are kept beyond that). */
    double fullCostRecentMs, reducedCostRecentMs;
    double fullCostWindowMs, reducedCostWindowMs;
    uint32_t fullCostRecentN, reducedCostRecentN;
    /* Appended (copied only as far as structSize reaches): PushMPC's deband pass (D3D11, before the graph), GPU ms per
       source frame from D3D11 timestamps, median of the last 64 frames; < 0 = not measured. debandActive = the pass runs
       on this stream (asked for, 8-bit SDR, supported by the device). */
    double debandMs;
    uint32_t debandActive;
    uint32_t debandReserved;
    /* Appended (PUSHCORE_FEATURE_FG_METHODS): what runs */
    uint32_t fgBackend;        /* PUSHCORE_FG_* in use (PUSHCORE_FG_NONE when frame generation could not start) */
    uint32_t fgFallback;       /* 1 = the asked-for method did not start and pushcore fell back (fgNote says why) */
    uint32_t srMethod;         /* PUSHCORE_SR_METHOD_* in use */
    uint32_t cutGuardActive;   /* 1 = the scene-cut guard runs for the frame generation in use */
    char fgNote[160];          /* UTF-8, e.g. "VFG did not start (failed backend 5): DLSS-G" */
    /* Appended (PUSHCORE_FEATURE_CUT_COUNT): scene cuts the guard held since open, cumulative over reloads; read back
       from the GPU a few frames after the decision (a caller watching it for changes sees a cut late, not never) */
    uint64_t cutGuardCuts;
    /* Appended (PUSHCORE_FEATURE_SUBTITLE_HOLD): PushFPS's EnhanceGraph::subtitleHoldState(): 0 not asked for, 1 running,
       2 asked for and unavailable (failed to start), 3 asked for and not part of this graph (no frame generation, or a
       method without it), 4 Auto: running and detecting, not holding (no burned-in text found); subtitleHoldPixels = pixels of the band it held in the frame sampled last */
    uint32_t subtitleHoldState;
    uint32_t subtitleHoldPixels;
    /* Appended (PUSHCORE_FEATURE_HELD_FRAMES): held-frame re-timing (PushCoreSettings.heldFrames) */
    uint32_t heldActive;          /* 1 = the held-frame test runs on this graph (asked for, frame generation on) */
    uint32_t heldRatioPermille;   /* held frames among the last heldTested (up to 240) source frames, per mille */
    uint32_t heldTested;
    uint32_t heldGraphMultiplier; /* the graph's built multiplier (the largest per-span multiplier) */
    uint64_t heldFrames;          /* source frames found held since open */
    uint64_t heldDropped;         /* held frames covered by a re-timed span (not given to the graph) */
    uint64_t heldSpans;           /* re-timed spans */
    /* Appended (PUSHCORE_FEATURE_HELD_FRAMES): per-region re-timing by source repair: held parts of a frame between two of
       their drawings get the span stream's in-between (a second VFG effect at the source size) before the graph */
    uint32_t repairActive;        /* 1 = the source repair runs (VFG, animation found or heldFrames 2) */
    uint32_t repairReserved;
    uint64_t repairedFrames;      /* source frames repaired */
    uint64_t repairedParts;       /* parts laid in (one span-stream frame each) */
    /* Appended (PUSHCORE_FEATURE_ANIME_HOLD): the hold compose on generated frames */
    uint32_t holdActive;          /* 1 = hold plans are made for this stream */
    uint32_t animClass;           /* the picture's class from its cadence (Auto per show): 0 not known yet, 1 feature
                                     (moving parts mostly on ones), 2 TV limited (twos / threes), 3 mixed CG (rarely
                                     drawn on twos); + 16 once final (after ~2 minutes), provisional before */
    uint64_t holdPairs;           /* source pairs with a hold */
    uint64_t holdFrames;          /* generated frames composed */
    uint64_t holdFlashes;         /* flash / impact pairs held whole */
} PushCoreStats;
#define PUSHCORE_STATS_SIZE_M2C 144u     /* sizeof(PushCoreStats) before the deband fields (x64) */

/* Per-source-frame GPU timestamp sample (D3D12 queue timestamps; ms, < 0 = not measured). */
typedef struct PushCoreGpuTiming {
    uint64_t sourceFrameId;    /* graph source frame id (1-based submit sequence) */
    double colorMs, srMs, fgBatchMs, fg1Ms, graphMs;
    double nrMs, videoHdrMs;   /* ABI 3: AI enhancement (NR, all layers) and RTX Video HDR */
} PushCoreGpuTiming;

PUSHCORE_API uint32_t pushcore_abiVersion(void);
/* Creates the D3D12 device on `device`'s adapter, the interop fences, the graph (PushFPS playback
   configuration) and the egress. Slow (model load, tens of seconds the first time): call it from a
   worker thread. Returns NULL on failure; the reason is copied into errorBuffer (UTF-8). */
PUSHCORE_API PushCore* pushcore_open(const PushCoreConfig* config, const PushCoreStreamInfo* stream,
                                     ID3D11Device* device, char* errorBuffer, uint32_t errorBufferSize);
/* True when a new source frame can be submitted without PUSHCORE_BUSY. */
PUSHCORE_API int pushcore_canSubmit(PushCore* core);
/* Copies `texture`[arraySlice] (NV12/P010, on the open device) into the ingress ring on the
   immediate context, signals the shared fence and submits the frame to the graph. No CPU wait on
   the GPU. The caller may reuse or release its texture as soon as this returns. */
PUSHCORE_API int pushcore_submit(PushCore* core, ID3D11Texture2D* texture, uint32_t arraySlice,
                                 int64_t pts100ns, uint32_t flags);
/* ABI 2: as submit, with the presentation clock and the frame generation level the renderer wants for this pair
   (fgLevel; PushFPS's reduced groups when the full group cannot be presented in time). streamNow100ns = the
   renderer's stream time when called (INT64_MIN = clock not running: always the full group). */
typedef struct PushCoreSubmitTiming {
    uint32_t structSize;       /* sizeof(PushCoreSubmitTiming) */
    int64_t streamNow100ns;
    int64_t presentLead100ns; /* extra lead the presenter needs before a frame's time (draw + present), 100 ns */
    int32_t fgLevel;           /* 0 = full group, 1 = the 2X midpoint only (reduced group), 2 = no generated frame;
                                  the renderer sets it from the late generated frames it observes */
    /* Filter route (additive; read only when structSize covers it, see pushcore_features PUSHCORE_FEATURE_DV_P5;
       callers built before it pass the smaller size and keep their behaviour):
       Dolby Vision profile 5 (IPT-PQ-C2) base layer of THIS source frame, as PushFPS's own player converts it
       (veyra::pipeline::DolbyVisionP5, built from the frame's RPU like DolbyVisionRpu.h buildDolbyVisionP5):
       per-component reshaping curve on {1, sqrt(x), x}, the RPU's ycc_to_rgb, and rgb_to_lms folded with the fixed
       HPE LMS->BT.2020 matrix. Applied on the GPU by the graph's YUV pass; the stream must be opened as PQ BT.2020.
       dvP5Active 0 = an ordinary frame. */
    uint32_t dvP5Active;
    float dvP5YccToRgb[9];
    float dvP5LmsToRgb[9];
    float dvP5Curve[9];        /* [component][term] */
    /* Appended (PUSHCORE_FEATURE_DV_P5_EXACT): the exact reshaping of THIS frame (piece-wise polynomial and MMR), in
       veyra::pipeline::DolbyVisionP5::exactCurve's layout ([component][59][4], see FramePacket.h); dvP5Exact 0 = the
       {1, sqrt(x), x} projection above */
    uint32_t dvP5Exact;
    float dvP5ExactCurve[3 * 59 * 4];
} PushCoreSubmitTiming;
#define PUSHCORE_SUBMIT_TIMING_SIZE_V3 144u /* sizeof(PushCoreSubmitTiming) before the exact reshaping (x64) */
#define PUSHCORE_SUBMIT_TIMING_SIZE_V2 32u /* sizeof(PushCoreSubmitTiming) before the Dolby Vision block (x64) */
#define PUSHCORE_FEATURE_DV_P5 1u  /* pushcore_features(): submit2 applies the Dolby Vision profile 5 block above */
#define PUSHCORE_FEATURE_SR_FAMILY 2u  /* PushCoreSettings.srFamily is honoured */
#define PUSHCORE_FEATURE_DEBAND 4u     /* PushCoreSettings.deband / debandStrength and PushCoreStats.debandMs */
#define PUSHCORE_FEATURE_CUT_HOLD 8u   /* VFG scene-cut guard: adaptive GPU cut test, the pair's first real frame held */
#define PUSHCORE_FEATURE_CUT_COUNT 32u  /* PushCoreStats.cutGuardCuts */
#define PUSHCORE_FEATURE_HDR_PEAK 64u   /* PushCoreSettings.videoHdrPeak */
#define PUSHCORE_FEATURE_DV_P5_EXACT 128u /* PushCoreSubmitTiming.dvP5Exact / dvP5ExactCurve */
#define PUSHCORE_FEATURE_SUBTITLE_HOLD 256u /* PushCoreSettings.subtitleHold, PushCoreStats.subtitleHoldState/Pixels */
#define PUSHCORE_FEATURE_NVOF_FG 1024u /* PushCoreSettings.fgBackend = PUSHCORE_FG_NVOF, PUSHCORE_CAP_NVOF_FG */
#define PUSHCORE_FEATURE_HELD_FRAMES 2048u /* PushCoreSettings.heldFrames, PushCoreStats.held* */
#define PUSHCORE_FEATURE_ANIME_HOLD (1u << 19) /* PushCoreSettings.animationMode / holdFlags, PushCoreStats.hold* / animClass */
#define PUSHCORE_FEATURE_VFG16 65536u /* NVIDIA VFG up to PUSHCORE_MAX_MULTIPLIER (16x; above 8x in the SDK timestep mode); older cores stop at 8x */
#define PUSHCORE_MAX_MULTIPLIER 16u
#define PUSHCORE_FEATURE_SUBTITLE_HOLD_AUTO 512u /* PushCoreSettings.subtitleHold = 2: held only while burned-in text is found */
#define PUSHCORE_FEATURE_FG_METHODS 16u /* PushCoreSettings.fgBackend/fgTier/srMethod/cutGuard/activeArea/fgLite, the guard
                                           for every backend, non-NVIDIA GPUs, pushcore_capabilities, pushcore_pushFlowBench */
PUSHCORE_API uint32_t pushcore_features(void); /* additive export; absent in older builds (= 0) */

/* What this GPU and the installed runtime can run (PUSHCORE_FEATURE_FG_METHODS). No instance and no D3D12 device needed;
   only file and adapter checks. rootDir = the runtime root (as PushCoreConfig.pushfpsDir). */
#define PUSHCORE_FAMILY_UNKNOWN 0u
#define PUSHCORE_FAMILY_RTX50 1u
#define PUSHCORE_FAMILY_RTX40 2u
#define PUSHCORE_FAMILY_RTX30 3u
#define PUSHCORE_FAMILY_RTX20 4u
#define PUSHCORE_FAMILY_GTX 5u       /* NVIDIA without tensor cores (GTX 16 / 10 and older) */
#define PUSHCORE_FAMILY_RDNA4 6u     /* RX 9000 */
#define PUSHCORE_FAMILY_RDNA3 7u     /* RX 7000 */
#define PUSHCORE_FAMILY_RDNA2 8u     /* RX 6000 */
#define PUSHCORE_FAMILY_RDNA1 9u     /* RX 5000 */
#define PUSHCORE_FAMILY_AMD_OTHER 10u
#define PUSHCORE_FAMILY_INTEL 11u
#define PUSHCORE_FAMILY_OTHER 12u
#define PUSHCORE_CAP_VFG 0u          /* bit numbers of PushCoreCaps.available, index of PushCoreCaps.reason */
#define PUSHCORE_CAP_DLSSG 1u
#define PUSHCORE_CAP_PUSHFLOW 2u
#define PUSHCORE_CAP_FSR_FG 3u
#define PUSHCORE_CAP_FSR4_FG 4u
#define PUSHCORE_CAP_AMDFRC 5u
#define PUSHCORE_CAP_RTX_VSR 6u
#define PUSHCORE_CAP_VSR_FAMILIES 7u /* high bitrate / streaming styles */
#define PUSHCORE_CAP_FSR_SR 8u
#define PUSHCORE_CAP_NIS 9u
#define PUSHCORE_CAP_RTX_HDR 10u     /* RTX Video HDR */
#define PUSHCORE_CAP_NR 11u          /* AI clean-up: NVIDIA's NR runtimes, or the AMD ones (RX 9000 lmxxf, RX 7000 RedDuke) */
#define PUSHCORE_CAP_DLSSG_LITE 12u
#define PUSHCORE_CAP_NVOF_FG 13u     /* PUSHCORE_FG_NVOF: NVOFA + this build's NVOF support + the FidelityFX files */
#define PUSHCORE_CAP_COUNT 14u
typedef struct PushCoreCaps {
    uint32_t structSize;       /* sizeof(PushCoreCaps) */
    uint32_t vendorId, deviceId;
    uint32_t family;           /* PUSHCORE_FAMILY_* */
    uint64_t adapterLuid;
    uint64_t dedicatedVideoMemory;
    uint32_t available;        /* bit PUSHCORE_CAP_* set = the GPU can run it and its files are installed */
    uint32_t hardware;         /* bit set = the GPU can run it (files aside) */
    char adapter[128];         /* UTF-8 adapter name */
    char reason[PUSHCORE_CAP_COUNT][96]; /* UTF-8: why a capability is not available (empty when it is) */
    char nrRuntime[48];        /* the AI clean-up runtime that would run ("NVIDIA 310.8.0", "lmxxf (RX 9000)", ...) */
} PushCoreCaps;
/* adapterLuid 0 = the first hardware adapter in DXGI's high-performance order */
PUSHCORE_API int pushcore_capabilities(uint64_t adapterLuid, const wchar_t* rootDir, PushCoreCaps* out);

/* PushFlow's speed on this GPU (a port of PushFPS's PushFlowBench): per tier (Fast, Balanced, Quality) the GPU time of
   the first generated frame of a pair (pack + network + rebuild) and of each further one (time-only pack + network +
   rebuild), at the given output size, from D3D12 timestamps on synthetic frames. Several seconds of full GPU load:
   run it once per card and keep the result (the filter stores it in HKCU\Software\PushMPC\Learned). */
typedef struct PushCoreFlowBench {
    uint32_t structSize;       /* sizeof(PushCoreFlowBench) */
    uint32_t width, height;    /* in: the frame size to measure (the output size PushFlow would run at) */
    uint32_t pairs;            /* in: measured pairs per tier (0 = 12) */
    uint32_t tierMask;         /* in: bit per tier to measure (0 = all three) */
    double firstMsP50[3], firstMsP95[3]; /* out, per tier; < 0 = the tier did not run */
    double nextMsP50[3], nextMsP95[3];
    double initMs[3];
    char adapter[128];         /* out */
    char note[256];            /* out: model / precision per tier, or why it failed */
} PushCoreFlowBench;
PUSHCORE_API int pushcore_pushFlowBench(uint64_t adapterLuid, const wchar_t* rootDir, PushCoreFlowBench* io);
typedef int (*PushCoreCapabilitiesFn)(uint64_t, const wchar_t*, PushCoreCaps*);
typedef int (*PushCoreFlowBenchFn)(uint64_t, const wchar_t*, PushCoreFlowBench*);
PUSHCORE_API int pushcore_submit2(PushCore* core, ID3D11Texture2D* texture, uint32_t arraySlice,
                                  int64_t pts100ns, uint32_t flags, const PushCoreSubmitTiming* timing);
/* Hands out up to `capacity` finished frames in presentation order. Waits up to timeoutMs (0 = none)
   for at least one. Never reads pixels back. */
PUSHCORE_API int pushcore_poll(PushCore* core, PushCoreFrame* frames, uint32_t capacity, uint32_t* count,
                               uint32_t timeoutMs);
/* Returns a frame. With `context` non-NULL pushcore signals its release fence on that context
   (call right after recording the draw that read the texture); NULL = the frame was not drawn. */
PUSHCORE_API int pushcore_presented(PushCore* core, uint64_t lease, ID3D11DeviceContext* context);
/* Re-reads the saved chain session and rebuilds the graph. All leases must be returned first
   (else PUSHCORE_BUSY). The next submit must carry PUSHCORE_SUBMIT_DISCONTINUITY. */
PUSHCORE_API int pushcore_reload(PushCore* core);
PUSHCORE_API int pushcore_applySettings(PushCore* core); /* M2c: see PushCoreApi::applySettings */
PUSHCORE_API int pushcore_setSettings(PushCore* core, const PushCoreSettings* settings); /* ABI 3: see PushCoreApi */
PUSHCORE_API int pushcore_importChainSession(const wchar_t* dataDir, PushCoreSettings* out, char* errorBuffer, uint32_t errorBufferSize);
/* Drops everything queued (seek/flush). All leases must be returned first (else PUSHCORE_BUSY). */
PUSHCORE_API int pushcore_flush(PushCore* core);
PUSHCORE_API int pushcore_outputInfo(PushCore* core, PushCoreOutputInfo* info);
PUSHCORE_API int pushcore_stats(PushCore* core, PushCoreStats* stats);
/* Drains collected GPU timestamp samples (oldest first). */
PUSHCORE_API int pushcore_takeGpuTimings(PushCore* core, PushCoreGpuTiming* samples, uint32_t capacity, uint32_t* count);
/* Last error, UTF-8, owned by the instance, valid until the next call on it. */
PUSHCORE_API const char* pushcore_lastError(PushCore* core);
/* Waits for the GPU, releases everything. All leases are invalid afterwards. */
PUSHCORE_API void pushcore_close(PushCore* core);

/* One GetProcAddress for loaders: pushcore_getApi(PUSHCORE_ABI_VERSION). */
typedef struct PushCoreApi {
    uint32_t structSize, abiVersion;
    uint32_t (*abiVersionFn)(void);
    PushCore* (*open)(const PushCoreConfig*, const PushCoreStreamInfo*, ID3D11Device*, char*, uint32_t);
    int (*canSubmit)(PushCore*);
    int (*submit)(PushCore*, ID3D11Texture2D*, uint32_t, int64_t, uint32_t);
    int (*poll)(PushCore*, PushCoreFrame*, uint32_t, uint32_t*, uint32_t);
    int (*presented)(PushCore*, uint64_t, ID3D11DeviceContext*);
    int (*reload)(PushCore*);
    int (*flush)(PushCore*);
    int (*outputInfo)(PushCore*, PushCoreOutputInfo*);
    int (*stats)(PushCore*, PushCoreStats*);
    int (*takeGpuTimings)(PushCore*, PushCoreGpuTiming*, uint32_t, uint32_t*);
    const char* (*lastError)(PushCore*);
    void (*close)(PushCore*);
    int (*submit2)(PushCore*, ID3D11Texture2D*, uint32_t, int64_t, uint32_t, const PushCoreSubmitTiming*); /* ABI 2 */
    /* M2c (additive; check structSize): re-reads the saved settings and applies them to the running graph when
       PushFPS itself would (EnhanceGraph::applySettings: colour, sharpening and other live parameters), without a
       rebuild, a drain or a history reset; frames may stay leased. PUSHCORE_OK = applied (or nothing on the GPU
       changed), PUSHCORE_NEEDS_REBUILD = use reload. */
    int (*applySettings)(PushCore*);
    /* ABI 3: replaces the PushCoreSettings the next applySettings / reload uses (copied; NULL = back to the chain
       session in dataDir). Nothing changes on the GPU until then. */
    int (*setSettings)(PushCore*, const PushCoreSettings*);
    /* ABI 3: reads a PushFPS chain session (chain-session.v1 in dataDir, read-only, with PushFPS's own store) and maps
       its active configuration onto PushCoreSettings (sr/srQuality/srTarget/multiplier (0 = frame generation off)/
       vfgQuality/nr/nrTone/nrStructure/videoHdr). No instance needed. PUSHCORE_OK or PUSHCORE_ERROR + errorBuffer. */
    int (*importChainSession)(const wchar_t* dataDir, PushCoreSettings* out, char* errorBuffer, uint32_t errorBufferSize);
} PushCoreApi;
typedef const PushCoreApi* (*PushCoreGetApiFn)(uint32_t abiVersion);
PUSHCORE_API const PushCoreApi* pushcore_getApi(uint32_t abiVersion);

#ifdef __cplusplus
}
#endif
