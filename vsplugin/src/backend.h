// pushmpc.Enhance: which engine runs, and how far the CPU engine goes. Pure C++ (no VapourSynth, no OS headers):
// portable_test checks every rule.
//
//   nvidia    pushcore (Windows, any DirectX 12 GPU; the name is historical, "pushcore" is accepted too): on NVIDIA RTX
//             RTX VSR + NVIDIA VFG; on AMD / Intel / Snapdragon / GTX the methods pushcore has for that GPU (PushFlow,
//             FSR 3.1 / FSR 4 frame generation, AMD FRC, FSR / NIS upscaling) with Auto's benchmark; the scene-cut guard.
//             On Linux (and Windows on Arm) the Video Effects runtime through CUDA (engines/nvidia-linux): RTX VSR, VFG.
//   portable  PushFPS's portable core on the CPU (engines/portable): any GPU, Windows, Linux, macOS.
//   apple     Apple's VideoToolbox ML frame interpolation and super resolution (macOS on Apple silicon; engines/apple),
//             when this Mac offers an interpolator; else portable.
//   ml        PushFlow (RIFE) on the GPU through pmml (ONNX Runtime with the card's EP, or ncnn-Vulkan): any GPU, every
//             OS, when the pmml runtime is installed (ml_vs.cpp).
//   rife      a script preset: pushmpc.vpy chains vs-rife-ncnn-vulkan (core.rife) when the user has it installed. The
//             plugin never downloads it; it only reports whether the script found it.
//
// Auto: pushcore present and the GPU an NVIDIA RTX -> nvidia; pushcore present on any other DirectX 12 hardware GPU
// with at least one GPU frame generation method installed (PushFlow, FSR FG, AMD FRC) -> nvidia (pushcore) as well;
// otherwise apple when this Mac offers it; otherwise ml when the pmml runtime is installed;
// otherwise rife when available; otherwise portable. Off switch for the
// non-RTX part: PUSHMPC_DISABLE_ANYGPUCORE=1 (BackendProbe.anyGpuDisabled), then such GPUs go on down the list.
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include "vrr_probe.h"   // LinuxVrrActive: VRR for Auto's cadence (DecidePortableK callers)

namespace pmvs {

enum class BackendKind { Nvidia, Portable, Rife, Apple, Ml };
inline const char* BackendName(BackendKind k) { return k == BackendKind::Nvidia ? "nvidia" : k == BackendKind::Rife ? "rife" : k == BackendKind::Apple ? "apple" : k == BackendKind::Ml ? "ml" : "portable"; }

// what the probe found. gpuFamily: pushcore_capabilities' family (PushMpcAuto.h GpuFamily: 1 RTX 50, 2 RTX 40,
// 3 RTX 30, 4 RTX 20, 5 GTX, 6.. AMD / Intel / other; 0 unknown)
struct BackendProbe {
    bool nvidiaBuilt = false;     // this build has the nvidia backend (Windows: pushcore; Linux: engines/nvidia-linux)
    bool pushcoreLoaded = false;  // Windows: pushcore.dll answered; Linux: the CUDA driver and the Video Effects runtime load
    uint32_t gpuFamily = 0;
    bool rifeAvailable = false;   // the script found core.rife
    bool appleBuilt = false;      // this build has the VideoToolbox backend (macOS)
    bool appleAvailable = false;  // ... and this Mac offers a VideoToolbox interpolator (apple::Probe)
    std::string appleWhy;         // what the probe found
    bool mlBuilt = false;         // this build has the ml backend (pmml)
    bool mlAvailable = false;     // the pmml runtime is installed (ml_vs.cpp MlRuntimePresent)
    std::string gpuName;
    bool nvidiaNoFg = false;      // the nvidia backend has no VFG here (Linux: RTX 20, or RTX 30 before its self-test): the
                                  // interpolator is picked by PickInterp (ml, its own optical-flow tier, rife, portable)
    bool nvidiaNvof = false;      // ... but NVIDIA optical-flow hardware for its own fast tier (NVOFA + warp)
    int nvofK = 0;                // the multiplier that tier reaches here (its measured / prior cost; 0 = unknown or none)
    int mlK = -1;                 // pmml's measured multiplier for this clip (autobench cache; 0 = none fits, -1 = not measured)
    int srcHeight = 0;            // the clip (SelectBackend's optional width / height), 0 = unknown
    uint32_t capsAvailable = 0;   // pushcore_capabilities' available bits (PUSHCORE_CAP_*)
    bool anyGpuDisabled = false;  // PUSHMPC_DISABLE_ANYGPUCORE=1: pushcore on RTX only (the old rule)
};
inline bool IsRtxFamily(uint32_t f) { return f >= 1 && f <= 4; }
// a DirectX 12 hardware GPU pushcore knows (families 1..12; 0 = unknown / no hardware adapter)
inline bool IsD3d12Family(uint32_t f) { return f >= 1 && f <= 12; }
// PUSHCORE_CAP_PUSHFLOW 2, PUSHCORE_CAP_FSR_FG 3, PUSHCORE_CAP_AMDFRC 5 (pushcore_abi.h; this header has no OS includes)
inline constexpr uint32_t kGpuFgCaps = (1u << 2) | (1u << 3) | (1u << 5);
inline bool PushcoreUsable(const BackendProbe& p)
{
    if (!p.nvidiaBuilt || !p.pushcoreLoaded) return false;
    if (IsRtxFamily(p.gpuFamily)) return true;
    return !p.anyGpuDisabled && IsD3d12Family(p.gpuFamily) && (p.capsAvailable & kGpuFgCaps) != 0;
}

// Without VFG on an NVIDIA GPU (Linux): who interpolates before the nvidia backend upscales. Quality first, by measured
// cost: RIFE through pmml (TensorRT-RTX, else CUDA / CPU EPs) when its measured multiplier is at least the optical-flow
// tier's; the optical-flow tier (NVOFA + warp, empty interp: the nvidia backend generates) when it reaches more; with no
// measurement yet, pmml for sources up to 1080p (RIFE fits there) and the optical-flow tier above; then rife, portable.
inline std::string PickInterp(const BackendProbe& p, std::string* why)
{
    const bool ml = p.mlBuilt && p.mlAvailable;
    const int kNv = p.nvidiaNvof ? p.nvofK : 0;
    int kMl = 0;
    if (ml) kMl = p.mlK >= 0 ? p.mlK : (p.srcHeight > 0 && p.srcHeight > 1080 ? 1 : 2);
    std::string w;
    std::string out;
    if (ml && kMl >= 2 && kMl >= kNv) { out = "ml"; w = "RIFE through pmml (x" + std::to_string(kMl) + (p.mlK >= 0 ? ", measured" : ", not measured yet") + ")"; }
    else if (kNv >= 2) { out = ""; w = "NVIDIA optical flow + warp (x" + std::to_string(kNv) + (ml ? ", pmml reaches x" + std::to_string(kMl) : std::string()) + ")"; }
    else if (ml && kMl >= 1) { out = "ml"; w = "RIFE through pmml"; }
    else if (p.rifeAvailable) { out = "rife"; w = "vs-rife-ncnn-vulkan"; }
    else { out = "portable"; w = "the portable engine"; }
    if (why) *why = w;
    return out;
}

// interp: with kind nvidia, the interpolator the script chains before it ("rife" or "portable"; empty = nvidia does it)
struct BackendChoice { BackendKind kind = BackendKind::Portable; bool ok = true; std::string why; std::string interp; };

// requested: "auto" (or empty), "nvidia", "portable", "rife" (case-insensitive; anything else is an error)
inline BackendChoice SelectBackend(std::string requested, const BackendProbe& p)
{
    for (auto& c : requested) c = char(std::tolower(static_cast<unsigned char>(c)));
    BackendChoice r;
    const bool nvidiaOk = PushcoreUsable(p);
    const std::string gpu = p.gpuName.empty() ? std::string("the GPU") : p.gpuName;
    if (requested == "pushcore") requested = "nvidia";
    if (requested == "nvidia") {
        r.kind = BackendKind::Nvidia;   // as asked; when pushcore cannot run, its own error says why (the R1 behaviour)
        r.ok = p.nvidiaBuilt;
        r.why = p.nvidiaBuilt ? "nvidia (your setting)" : "nvidia: this build has no pushcore backend (Windows only)";
        if (p.nvidiaBuilt && p.nvidiaNoFg) { std::string w; r.interp = PickInterp(p, &w); r.why += "; frame generation by " + w; }
        return r;
    }
    if (requested == "portable") { r.kind = BackendKind::Portable; r.why = "portable (your setting)"; return r; }
    if (requested == "apple") {
        if (p.appleAvailable) { r.kind = BackendKind::Apple; r.why = "apple (your setting)"; }
        else { r.kind = BackendKind::Portable; r.why = std::string("apple asked for, but ") + (p.appleBuilt ? "this Mac offers no VideoToolbox interpolator (" + p.appleWhy + ")" : "this build has no VideoToolbox backend (macOS only)") + ": portable"; }
        return r;
    }
    if (requested == "ml") {
        if (p.mlBuilt && p.mlAvailable) { r.kind = BackendKind::Ml; r.why = "ml (your setting)"; }
        else { r.kind = BackendKind::Portable; r.why = std::string("ml asked for, but ") + (p.mlBuilt ? "the pmml runtime is not installed" : "this build has no ml backend") + ": portable"; }
        return r;
    }
    if (requested == "rife") {
        if (p.rifeAvailable) { r.kind = BackendKind::Rife; r.why = "rife (your setting)"; }
        else { r.kind = BackendKind::Portable; r.why = "rife asked for, but vs-rife-ncnn-vulkan (core.rife) is not installed: portable"; }
        return r;
    }
    if (!requested.empty() && requested != "auto") { r.ok = false; r.why = "backend must be auto, nvidia (pushcore), apple, ml, portable or rife (got '" + requested + "')"; return r; }
    if (nvidiaOk) {
        r.kind = BackendKind::Nvidia;
        r.why = IsRtxFamily(p.gpuFamily) ? "Auto: " + gpu + " is an NVIDIA RTX GPU and pushcore is present: nvidia"
                                         : "Auto: pushcore is present and runs GPU frame generation on " + gpu + " (DirectX 12): nvidia (pushcore)";
        if (p.nvidiaNoFg) { std::string w; r.interp = PickInterp(p, &w); r.why += " (no VFG on this GPU; frame generation by " + w + ")"; }
        return r;
    }
    std::string not_nv = !p.nvidiaBuilt ? "no pushcore backend in this build" : !p.pushcoreLoaded ? "pushcore is not present"
                       : !IsD3d12Family(p.gpuFamily) ? gpu + " is not a DirectX 12 hardware GPU pushcore knows"
                       : p.anyGpuDisabled ? gpu + " is not an NVIDIA RTX GPU (pushcore on other GPUs is switched off: PUSHMPC_DISABLE_ANYGPUCORE)"
                       : gpu + " has no GPU frame generation method installed for pushcore (PushFlow, FSR FG, AMD FRC)";
    if (p.appleAvailable) { r.kind = BackendKind::Apple; r.why = "Auto: " + not_nv + "; this Mac offers " + p.appleWhy + ": apple"; return r; }
    if (p.mlBuilt && p.mlAvailable) { r.kind = BackendKind::Ml; r.why = "Auto: " + not_nv + "; the pmml runtime is installed: ml (PushFlow on the GPU)"; return r; }
    if (p.rifeAvailable) { r.kind = BackendKind::Rife; r.why = "Auto: " + not_nv + "; vs-rife-ncnn-vulkan is installed: rife"; return r; }
    r.kind = BackendKind::Portable;
    r.why = "Auto: " + not_nv + "; vs-rife-ncnn-vulkan is not installed: portable (the GPU engine on a Vulkan device, else the CPU)";
    return r;
}

// ------------------------------------------------------------------------------------------------ the CPU budget
// The portable engine runs in VapourSynth's getFrame, one source frame at a time: per source frame it costs one motion
// estimation (both ways) and k - 1 generated frames. That must fit in kCpuShare of the source frame interval (the
// decoder, mpv's own work and the copies need the rest). Auto also caps the multiplier at 2 above 1080p.
inline constexpr double kCpuShare = 0.6;
inline constexpr int kAutoMaxHeightFull = 1080;
struct PortableK { int k = 1; double costMs = 0, budgetMs = 0; std::string why; };
// The player's own work per shown frame on the GPU (its scaling and presentation, a window's desktop composition):
// PushMpcAuto.h measured 0.33 ms per shown 4K frame for the composition on an RTX 5090; the GPUs these engines run on
// are taken as about 3x slower (prior). It counts against the GPU engine's budget only (the CPU engine's budget is CPU time).
inline constexpr double kPortableShowMs4k = 1.0;
// One new frame on every refresh of a fixed refresh rate (refresh / output fps < 1.5) has no slack for a late frame
// (PushMpcAuto.h kFixedRefreshSpareMs)
inline constexpr double kPortableSpareMs = 1.0;
// The multiplier up to maxK (all of which fit) that shows frames shortest at a fixed refresh (PushMpcAuto.h's rule: the
// shortest longest hold, a tie going to the even cadence, then to the larger); displayHz <= 0: maxK as it is
inline int CadenceK(int maxK, double srcFps, double displayHz)
{
    if (maxK <= 1 || displayHz <= 0 || srcFps <= 0) return maxK;
    int best = 1; double bestHold = 1e9; bool bestEven = true;
    for (int k = 1; k <= maxK; ++k) {
        const double q = displayHz / (srcFps * k), n = std::round(q);
        const bool even = n >= 1 && std::fabs(q - n) / n <= 0.02;
        const double hold = std::max(1.0, even ? n : std::ceil(q)) * 1000.0 / displayHz;
        if (k == 1 || hold < bestHold - 0.05 || (std::fabs(hold - bestHold) <= 0.05 && (even || !bestEven))) { best = k; bestHold = hold; bestEven = even; }
    }
    return best;
}
// smoother: 0 Auto, 1 off, 2..8 fixed (the same words as the registry's Smoother); displayHz <= 0: unknown (Auto then
// aims at x2)
// gpu: the GPU engine (no cap at 2 above 1080p: its costs are measured the same way, the cap was the CPU's)
// showMs: the player's GPU work per shown frame (< 0: the prior kPortableShowMs4k at a 16:9 picture of this height)
// showMs 0: the player is full screen (no composition). vrr: 1 = variable refresh runs (vrr_probe.h; any multiplier that
// fits, the larger), 0 / -1 = a fixed refresh (known / assumed): Auto takes the multiplier with the shortest longest hold
// on screen, a tie going to the even cadence, then to the larger (PushMpcAuto.h's rule; 240 Hz: 5x rather than an
// uneven 6x-9x), and one new frame per refresh needs kPortableSpareMs more.
inline PortableK DecidePortableK(int smoother, double srcFps, double displayHz, int height, double meMs, double genMs, bool gpu = false, double showMs = -1, int vrr = -1)
{
    PortableK r;
    if (srcFps <= 0) srcFps = 23.976;
    r.budgetMs = 1000.0 / srcFps * kCpuShare;
    if (showMs < 0) showMs = kPortableShowMs4k * (double(height) * double(height) * 16.0 / 9.0) / (3840.0 * 2160.0);
    auto cost = [&](int k) { return k <= 1 ? 0.0 : meMs + (k - 1) * genMs + (gpu ? k * showMs : 0.0); };
    auto hold = [&](int k) {
        const double out = srcFps * k;
        if (displayHz <= 0 || vrr == 1) return 1000.0 / out;
        const double q = displayHz / out, n = std::round(q);
        const bool even = n >= 1 && std::fabs(q - n) / n <= 0.02;
        return std::max(1.0, even ? n : std::ceil(q)) * 1000.0 / displayHz;
    };
    auto evenAt = [&](int k) { const double q = displayHz / (srcFps * k), n = std::round(q); return vrr == 1 || (displayHz > 0 && n >= 1 && std::fabs(q - n) / n <= 0.02); };
    auto spare = [&](int k) { return vrr != 1 && displayHz > 0 && displayHz / (srcFps * k) < 1.5 ? kPortableSpareMs : 0.0; };
    char b[256];
    if (smoother == 1) { r.k = 1; r.why = "Smoother off: no frame generation"; return r; }
    if (smoother >= 2) {
        r.k = std::min(smoother, 8); r.costMs = cost(r.k);
        snprintf(b, sizeof(b), "x%d (your setting); measured %.1f ms per source frame of %.1f ms%s", r.k, r.costMs, r.budgetMs,
                 r.costMs > r.budgetMs ? (gpu ? ": over the budget, expect dropped frames" : ": over the CPU budget, expect dropped frames") : "");
        r.why = b;
        return r;
    }
    const int displayK = displayHz > 0 ? std::clamp(int(std::floor(displayHz / srcFps + 0.02)), 1, 8) : 2;
    const int cap = !gpu && height > kAutoMaxHeightFull ? 2 : 8;
    const int want = std::min(displayK, cap);
    int k = 1, lastFit = 1;
    double bestHold = hold(1);
    bool bestEven = true;
    for (int c = 2; c <= want; ++c) {
        if (cost(c) + spare(c) > r.budgetMs) break;
        lastFit = c;
        const double h = hold(c);
        const bool ev = evenAt(c);
        if (k == 1 || h < bestHold - 0.05 || (std::fabs(h - bestHold) <= 0.05 && (ev || !bestEven))) { k = c; bestHold = h; bestEven = ev; }
    }
    r.k = k; r.costMs = cost(k);
    if (want <= 1) snprintf(b, sizeof(b), "Auto: the display (%.2f Hz) is not faster than the source (%.3f fps): x1", displayHz, srcFps);
    else if (k == 1) snprintf(b, sizeof(b), gpu ? "Auto: the GPU path is too slow here (x2 needs %.1f ms per source frame, budget %.1f ms): x1" : "Auto: the CPU is too slow here (x2 needs %.1f ms per source frame, budget %.1f ms): x1", cost(2), r.budgetMs);
    else snprintf(b, sizeof(b), "Auto: x%d (display x%d%s%s; %.1f ms per source frame of %.1f ms; motion %.1f ms, each generated frame %.1f ms%s)", k, displayK,
                  cap < displayK ? ", capped at x2 above 1080p" : "", lastFit > k ? (vrr == 0 ? ", the even cadence at a fixed refresh" : ", the even cadence at a fixed refresh (assumed)") : vrr == 1 ? ", VRR" : "", r.costMs, r.budgetMs, meMs, genMs,
                  gpu ? ", the player's share per shown frame included" : "");
    r.why = b;
    return r;
}

// ------------------------------------------------------------------------------------------------ GPU upscaling
// The GPU engine upscales luma with NIS or FSR1 (EASU + RCAS) and chroma bicubic, to the target: the screen (mpv's
// display resolution, when the script passes it), 1440p, 4K or 5K, keeping the aspect ratio, even sizes. Nothing is
// upscaled when the target is not more than 5 % larger than the source (mpv scales the rest) or unknown.
// upscaler: auto | nis | fsr | off (rtx / vsr mean auto: there is no RTX VSR outside pushcore). Auto picks kAutoUpscaler,
// the better of the two on the CI benchmark (2x from half size, vsplugin-gpu.yml): NIS at sharpness 0.1 led on PSNR on
// both clips (28.48 / 40.08 dB vs FSR1's best 28.34 / 39.84, bilinear 27.45 / 38.06). kDefaultSharpness* are the
// defaults when no sharpness is given.
enum class GpuUpscaler { None = 0, Nis = 1, Fsr = 2 };
inline constexpr GpuUpscaler kAutoUpscaler = GpuUpscaler::Nis;
inline constexpr double kDefaultSharpnessNis = 0.1, kDefaultSharpnessFsr = 0.5;
struct UpscaleChoice { GpuUpscaler kind = GpuUpscaler::None; int w = 0, h = 0; std::string why; };
inline UpscaleChoice DecideUpscale(std::string upscaler, std::string target, int displayW, int displayH, int srcW, int srcH)
{
    for (auto& c : upscaler) c = char(std::tolower(static_cast<unsigned char>(c)));
    for (auto& c : target) c = char(std::tolower(static_cast<unsigned char>(c)));
    UpscaleChoice r;
    r.w = srcW; r.h = srcH;
    if (upscaler == "off" || upscaler == "none" || upscaler == "0") { r.why = "upscaling off (your setting)"; return r; }
    const GpuUpscaler kind = upscaler == "nis" ? GpuUpscaler::Nis : upscaler == "fsr" || upscaler == "fsr1" ? GpuUpscaler::Fsr : kAutoUpscaler;
    int tw = 0, th = 0;
    if (target == "1440p") { tw = 2560; th = 1440; }
    else if (target == "4k" || target == "2160p") { tw = 3840; th = 2160; }
    else if (target == "5k") { tw = 5120; th = 2880; }
    else { tw = displayW; th = displayH; }
    if (tw <= 0 || th <= 0) { r.why = "no upscaling: the display size is unknown (mpv scales)"; return r; }
    const double s = std::min(double(tw) / double(srcW), double(th) / double(srcH));
    if (s <= 1.05) { r.why = "no upscaling: the target is not larger than the source"; return r; }
    r.kind = kind;
    r.w = std::max(2, int(std::lround(srcW * s / 2.0)) * 2);
    r.h = std::max(2, int(std::lround(srcH * s / 2.0)) * 2);
    char b[200];
    snprintf(b, sizeof(b), "%s %dx%d -> %dx%d (%s%s)", kind == GpuUpscaler::Nis ? "NIS" : "FSR1 (EASU + RCAS)", srcW, srcH, r.w, r.h,
             target == "screen" || target.empty() ? "the screen" : target.c_str(), upscaler == "auto" || upscaler.empty() ? ", Auto" : "");
    r.why = b;
    return r;
}

} // namespace pmvs
