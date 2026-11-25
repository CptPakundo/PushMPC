// pushmpc.Enhance: which engine runs, and how far the CPU engine goes. Pure C++ (no VapourSynth, no OS headers):
// portable_test checks every rule.
//
//   nvidia    pushcore (Windows, RTX): RTX VSR, NVIDIA VFG and the other methods, the scene-cut guard. The R1 path.
//   portable  PushFPS's portable core on the CPU (engines/portable): any GPU, Windows, Linux, macOS.
//   ml        PushFlow (RIFE) on the GPU through pmml (ONNX Runtime with the card's EP, or ncnn-Vulkan): any GPU, every
//             OS, when the pmml runtime is installed (ml_vs.cpp).
//   rife      a script preset: pushmpc.vpy chains vs-rife-ncnn-vulkan (core.rife) when the user has it installed. The
//             plugin never downloads it; it only reports whether the script found it.
//
// Auto: an NVIDIA RTX GPU with pushcore present -> nvidia; otherwise ml when the pmml runtime is installed; otherwise
// rife when available; otherwise portable.
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace pmvs {

enum class BackendKind { Nvidia, Portable, Rife, Ml };
inline const char* BackendName(BackendKind k) { return k == BackendKind::Nvidia ? "nvidia" : k == BackendKind::Rife ? "rife" : k == BackendKind::Ml ? "ml" : "portable"; }

// what the probe found. gpuFamily: pushcore_capabilities' family (PushMpcAuto.h GpuFamily: 1 RTX 50, 2 RTX 40,
// 3 RTX 30, 4 RTX 20, 5 GTX, 6.. AMD / Intel / other; 0 unknown)
struct BackendProbe {
    bool nvidiaBuilt = false;     // this build has the pushcore backend (Windows)
    bool pushcoreLoaded = false;  // pushcore.dll loaded and pushcore_capabilities answered
    uint32_t gpuFamily = 0;
    bool rifeAvailable = false;   // the script found core.rife
    bool mlBuilt = false;         // this build has the ml backend (pmml)
    bool mlAvailable = false;     // the pmml runtime is installed (ml_vs.cpp MlRuntimePresent)
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
    if (!requested.empty() && requested != "auto") { r.ok = false; r.why = "backend must be auto, nvidia, ml, portable or rife (got '" + requested + "')"; return r; }
    if (nvidiaOk) { r.kind = BackendKind::Nvidia; r.why = "Auto: " + gpu + " is an NVIDIA RTX GPU and pushcore is present: nvidia"; return r; }
    std::string not_nv = !p.nvidiaBuilt ? "no pushcore backend in this build" : !p.pushcoreLoaded ? "pushcore is not present" : gpu + " is not an NVIDIA RTX GPU";
    if (p.mlBuilt && p.mlAvailable) { r.kind = BackendKind::Ml; r.why = "Auto: " + not_nv + "; the pmml runtime is installed: ml (PushFlow on the GPU)"; return r; }
    if (p.rifeAvailable) { r.kind = BackendKind::Rife; r.why = "Auto: " + not_nv + "; vs-rife-ncnn-vulkan is installed: rife"; return r; }
    r.kind = BackendKind::Portable;
    r.why = "Auto: " + not_nv + "; vs-rife-ncnn-vulkan is not installed: portable (CPU)";
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
inline PortableK DecidePortableK(int smoother, double srcFps, double displayHz, int height, double meMs, double genMs)
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
                 r.costMs > r.budgetMs ? ": over the CPU budget, expect dropped frames" : "");
        r.why = b;
        return r;
    }
    const int displayK = displayHz > 0 ? std::clamp(int(std::floor(displayHz / srcFps + 0.02)), 1, 8) : 2;
    const int cap = height > kAutoMaxHeightFull ? 2 : 8;
    const int want = std::min(displayK, cap);
    int k = 1;
    for (int c = want; c >= 2; --c) if (cost(c) <= r.budgetMs) { k = c; break; }
    r.k = k; r.costMs = cost(k);
    if (want <= 1) snprintf(b, sizeof(b), "Auto: the display (%.2f Hz) is not faster than the source (%.3f fps): x1", displayHz, srcFps);
    else if (k == 1) snprintf(b, sizeof(b), "Auto: the CPU is too slow here (x2 needs %.1f ms per source frame, budget %.1f ms): x1", cost(2), r.budgetMs);
    else snprintf(b, sizeof(b), "Auto: x%d (display x%d%s; %.1f ms per source frame of %.1f ms; motion %.1f ms, each generated frame %.1f ms)", k, displayK,
                  cap < displayK ? ", capped at x2 above 1080p" : "", r.costMs, r.budgetMs, meMs, genMs);
    r.why = b;
    return r;
}

} // namespace pmvs
