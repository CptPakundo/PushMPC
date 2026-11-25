// pushmpc.Enhance: which engine runs, and how far the CPU engine goes. Pure C++ (no VapourSynth, no OS headers):
// portable_test checks every rule.
//
//   nvidia    pushcore (Windows, RTX): RTX VSR, NVIDIA VFG and the other methods, the scene-cut guard. The R1 path.
//   portable  PushFPS's portable core on the CPU (engines/portable): any GPU, Windows, Linux, macOS.
//   rife      a script preset: pushmpc.vpy chains vs-rife-ncnn-vulkan (core.rife) when the user has it installed. The
//             plugin never downloads it; it only reports whether the script found it.
//
// Auto: an NVIDIA RTX GPU with pushcore present -> nvidia; otherwise rife when available; otherwise portable.
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace pmvs {

enum class BackendKind { Nvidia, Portable, Rife };
inline const char* BackendName(BackendKind k) { return k == BackendKind::Nvidia ? "nvidia" : k == BackendKind::Rife ? "rife" : "portable"; }

// what the probe found. gpuFamily: pushcore_capabilities' family (PushMpcAuto.h GpuFamily: 1 RTX 50, 2 RTX 40,
// 3 RTX 30, 4 RTX 20, 5 GTX, 6.. AMD / Intel / other; 0 unknown)
struct BackendProbe {
    bool nvidiaBuilt = false;     // this build has the pushcore backend (Windows)
    bool pushcoreLoaded = false;  // pushcore.dll loaded and pushcore_capabilities answered
    uint32_t gpuFamily = 0;
    bool rifeAvailable = false;   // the script found core.rife
    std::string gpuName;
};
inline bool IsRtxFamily(uint32_t f) { return f >= 1 && f <= 4; }

struct BackendChoice { BackendKind kind = BackendKind::Portable; bool ok = true; std::string why; };

// requested: "auto" (or empty), "nvidia", "portable", "rife" (case-insensitive; anything else is an error)
inline BackendChoice SelectBackend(std::string requested, const BackendProbe& p)
{
    for (auto& c : requested) c = char(std::tolower(static_cast<unsigned char>(c)));
    BackendChoice r;
    const bool nvidiaOk = p.nvidiaBuilt && p.pushcoreLoaded && IsRtxFamily(p.gpuFamily);
    const std::string gpu = p.gpuName.empty() ? std::string("the GPU") : p.gpuName;
    if (requested == "nvidia") {
        r.kind = BackendKind::Nvidia;   // as asked; when pushcore cannot run, its own error says why (the R1 behaviour)
        r.ok = p.nvidiaBuilt;
        r.why = p.nvidiaBuilt ? "nvidia (your setting)" : "nvidia: this build has no pushcore backend (Windows only)";
        return r;
    }
    if (requested == "portable") { r.kind = BackendKind::Portable; r.why = "portable (your setting)"; return r; }
    if (requested == "rife") {
        if (p.rifeAvailable) { r.kind = BackendKind::Rife; r.why = "rife (your setting)"; }
        else { r.kind = BackendKind::Portable; r.why = "rife asked for, but vs-rife-ncnn-vulkan (core.rife) is not installed: portable"; }
        return r;
    }
    if (!requested.empty() && requested != "auto") { r.ok = false; r.why = "backend must be auto, nvidia, portable or rife (got '" + requested + "')"; return r; }
    if (nvidiaOk) { r.kind = BackendKind::Nvidia; r.why = "Auto: " + gpu + " is an NVIDIA RTX GPU and pushcore is present: nvidia"; return r; }
    std::string not_nv = !p.nvidiaBuilt ? "no pushcore backend in this build" : !p.pushcoreLoaded ? "pushcore is not present" : gpu + " is not an NVIDIA RTX GPU";
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
// smoother: 0 Auto, 1 off, 2..8 fixed (the same words as the registry's Smoother); displayHz <= 0: unknown (Auto then
// aims at x2)
// gpu: the GPU engine (no cap at 2 above 1080p: its costs are measured the same way, the cap was the CPU's)
inline PortableK DecidePortableK(int smoother, double srcFps, double displayHz, int height, double meMs, double genMs, bool gpu = false)
{
    PortableK r;
    if (srcFps <= 0) srcFps = 23.976;
    r.budgetMs = 1000.0 / srcFps * kCpuShare;
    auto cost = [&](int k) { return k <= 1 ? 0.0 : meMs + (k - 1) * genMs; };
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
    int k = 1;
    for (int c = want; c >= 2; --c) if (cost(c) <= r.budgetMs) { k = c; break; }
    r.k = k; r.costMs = cost(k);
    if (want <= 1) snprintf(b, sizeof(b), "Auto: the display (%.2f Hz) is not faster than the source (%.3f fps): x1", displayHz, srcFps);
    else if (k == 1) snprintf(b, sizeof(b), gpu ? "Auto: the GPU path is too slow here (x2 needs %.1f ms per source frame, budget %.1f ms): x1" : "Auto: the CPU is too slow here (x2 needs %.1f ms per source frame, budget %.1f ms): x1", cost(2), r.budgetMs);
    else snprintf(b, sizeof(b), "Auto: x%d (display x%d%s; %.1f ms per source frame of %.1f ms; motion %.1f ms, each generated frame %.1f ms)", k, displayK,
                  cap < displayK ? ", capped at x2 above 1080p" : "", r.costMs, r.budgetMs, meMs, genMs);
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
