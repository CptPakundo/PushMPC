// pushmpc.Enhance, backend "nvidia" on Linux: what the GPU can run, and Auto. The rules are the Windows filter's
// (filter/src/PushMpcAuto.h: "highest multiplier that fits", VFG Medium then High with 3 ms to spare, RTX VSR quality
// with the rest of the budget, the same cost table measured on an RTX 5090 and the same card factors), restated here
// because PushMpcAuto.h is Windows-only (wide strings, the registry). Pure C++: nvl_test.cpp checks every rule.
//
// GPU classes (compute capability + name, from the CUDA driver):
//   RTX 50 (12.x), RTX 40 (8.9), Hopper (9.0), Blackwell data centre (10.x): RTX VSR, RTX Video HDR, VFG native.
//   RTX 30 (8.6): RTX VSR, RTX Video HDR; VFG through the Linux port of VfgAmpereCompat, only after its self-test passed.
//   RTX 20 (7.5, not GTX 16): RTX VSR, RTX Video HDR; no VFG (SASS-only for SM89+): interpolation from rife / portable.
//   GTX 16 (7.5 "GTX"), A100 (8.0 has no VFG either): VSR where it loads; else nothing from this backend.
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>

namespace pmvs::nvl {

enum class GpuClass { None, Rtx50, Rtx40, Rtx30, Rtx20, Gtx16, Datacenter, Other };
inline const char* GpuClassName(GpuClass c)
{
    switch (c) {
    case GpuClass::Rtx50: return "RTX 50 / Blackwell"; case GpuClass::Rtx40: return "RTX 40 / Ada"; case GpuClass::Rtx30: return "RTX 30 / Ampere";
    case GpuClass::Rtx20: return "RTX 20 / Turing"; case GpuClass::Gtx16: return "GTX 16 / Turing without tensor cores"; case GpuClass::Datacenter: return "data centre";
    case GpuClass::Other: return "older NVIDIA"; default: return "no NVIDIA GPU";
    }
}
inline std::string Upper(std::string s) { for (auto& c : s) c = char(std::toupper(static_cast<unsigned char>(c))); return s; }
inline GpuClass ClassOf(int major, int minor, const std::string& name)
{
    const std::string n = Upper(name);
    if (major <= 0) return GpuClass::None;
    if (major == 12 || major == 10 || major == 11) return major == 12 ? GpuClass::Rtx50 : GpuClass::Datacenter;
    if (major == 9) return GpuClass::Datacenter;   // Hopper: VFG native on Linux (NVIDIA's VFX 1.3 docs)
    if (major == 8 && minor == 9) return GpuClass::Rtx40;
    if (major == 8 && minor == 6) return GpuClass::Rtx30;   // GA10x (RTX 30, A-series workstation)
    if (major == 8) return GpuClass::Datacenter;            // A100 (8.0), Orin (8.7)
    if (major == 7 && minor == 5) return n.find("GTX") != std::string::npos ? GpuClass::Gtx16 : GpuClass::Rtx20;
    return GpuClass::Other;
}
// VFG as NVIDIA ships it (SM89 SASS and newer: Ada, Hopper, Blackwell)
inline bool VfgNative(int major, int minor) { return (major == 8 && minor == 9) || major == 9 || major == 10 || major == 11 || major == 12; }
inline bool VfgAmpere(int major, int minor) { return major == 8 && minor == 6; }
// RTX VSR needs tensor cores (Turing RTX and newer); GTX 16 is tried anyway (the load says no) only when asked
inline bool VsrCapable(GpuClass c) { return c == GpuClass::Rtx50 || c == GpuClass::Rtx40 || c == GpuClass::Rtx30 || c == GpuClass::Rtx20 || c == GpuClass::Datacenter; }

// PushMpcAuto.h CardFactor (cost relative to an RTX 5090 / 4090), by name, else by class
inline double CardFactor(const std::string& name, GpuClass c)
{
    struct E { const char* k; double f; };
    static const E table[] = { { "5090", 1.0 }, { "4090", 1.0 }, { "5080", 1.6 }, { "4080", 1.7 }, { "5070 TI", 2.0 }, { "5070", 2.5 }, { "4070 TI", 2.3 },
        { "4070", 2.8 }, { "5060", 3.5 }, { "4060", 4.5 }, { "3090", 2.7 }, { "3080", 3.0 }, { "3070", 4.0 }, { "3060", 5.5 }, { "2080", 4.0 },
        { "2070", 5.5 }, { "2060", 8.0 } };
    const std::string n = Upper(name);
    for (const auto& e : table) if (n.find(e.k) != std::string::npos) return e.f;
    return c == GpuClass::Rtx50 || c == GpuClass::Rtx40 ? 2.0 : c == GpuClass::Rtx30 ? 3.5 : c == GpuClass::Datacenter ? 1.5 : 5.0;
}

// PushMpcAuto.h Costs (RTX 5090, p95 ms per source frame for a 4K output) plus what this route adds: the upload, the
// two conversions and the readback of every output frame (CUDA copies over PCIe, overlapped; kOverheadPerOutput4k is
// the GPU-side share at 4K).
struct Costs {
    double vfgMedium[7] = { 4.22, 7.12, 10.02, 13.78, 14.91, 18.71, 22.51 };
    double vfgHigh[7]   = { 9.39, 17.72, 26.04, 34.74, 44.14, 53.50, 62.91 };
    double vfgLowShare = 0.40;
    double vsrUp[4] = { 0.61, 0.76, 1.59, 2.40 };   // standard 1080p -> 4K
    double vsrUpHb[4] = { 1.19, 1.80, 2.73, 4.40 }; // high bitrate 16..19
    double vsrUpSt[4] = { 1.59, 1.59, 4.52, 4.52 }; // streaming 21 / 23
    double vsrP95 = 1.20;
    double hdr4k = 2.0;                             // RTX Video HDR at 4K (prior)
    double overheadPerOutput4k = 0.35;              // conversion + copy engine share per output frame at 4K
    double guard = 0.0;                             // the cut guard runs on the CPU here
    double card = 1.0;
    // NVIDIA optical flow + our warp (the fast tier on GPUs without VFG): both directions of NVOFA at a 1080p source per
    // source frame, one warped frame at a 4K output. Priors (NVOFA is fixed-function: per class, not per card factor);
    // the engine measures both with CUDA events and Auto then uses the measurement (flowMeasured / warpMeasured > 0).
    double nvofPair1080 = 3.0, warp4k = 0.35;
    double flowMeasured = 0, warpMeasured = 0;      // ms at the measured source / output size (as is)
    double guardFlow = 1.0;                         // the guard's flow measures on top of VFG (Windows: +1.0 ms at 1080p)
};
// the optical-flow tier's cost per source frame at multiplier m (src = source pixels / 1080p, out = output pixels / 4K)
inline double NvofCost(const Costs& c, int m, double srcPix1080, double pixOut)
{
    if (m < 2) return 0;
    const double flow = c.flowMeasured > 0 ? c.flowMeasured : c.nvofPair1080 * srcPix1080;
    const double warp = c.warpMeasured > 0 ? c.warpMeasured : c.warp4k * pixOut * c.card;
    return flow + (m - 1) * warp;
}
inline constexpr double kBudgetShare = 0.85, kRefreshSlack = 1.02, kCadenceTolerance = 0.02, kHighSpareMs = 3.0;

inline bool CadenceEven(double refresh, double outFps)
{
    if (outFps <= 0) return false;
    const double r = refresh / outFps, k = std::round(r);
    return k >= 1 && std::fabs(r - k) / k <= kCadenceTolerance;
}
inline double HoldMs(double refresh, bool vrr, double outFps)
{
    if (outFps <= 0) return 1e9;
    if (vrr) return 1000.0 / outFps;
    const double k = refresh / outFps;
    const double n = CadenceEven(refresh, outFps) ? std::round(k) : std::ceil(k);
    return std::max(1.0, n) * 1000.0 / refresh;
}
inline double Pix(int w, int h) { return double(w) * double(h) / (3840.0 * 2160.0); }
inline double VfgCost(const Costs& c, int quality, int m, double pix)
{
    if (m < 2) return 0;
    const double sizeVfg = 0.75 + 0.25 * pix;
    const double med = c.vfgMedium[std::min(m, 8) - 2], high = c.vfgHigh[std::min(m, 8) - 2];
    const double v = quality >= 2 ? high : quality <= 0 ? med * c.vfgLowShare : med;
    return v * sizeVfg * c.card;
}
// style: 0 standard, 1 high bitrate, 2 streaming
inline double VsrCost(const Costs& c, int level, int style, double pix)
{
    if (level < 1) return 0;
    const double* t = style == 1 ? c.vsrUpHb : style == 2 ? c.vsrUpSt : c.vsrUp;
    return t[std::min(level, 4) - 1] * pix * c.vsrP95 * c.card;
}
// the VFX VideoSuperRes QualityLevel for a style and tier (PushFPS videoSrQualityFor)
inline int VsrQualityLevel(int style, int tier)
{
    tier = std::clamp(tier, 1, 4);
    return style == 1 ? 15 + tier : style == 2 ? (tier <= 2 ? 21 : 23) : tier;
}

struct Caps {
    GpuClass gpu = GpuClass::None;
    bool vsr = false;     // VideoSuperRes loaded
    bool hdr = false;     // TrueHDR loaded
    bool vfg = false;     // VideoFrameGeneration usable (native, or the Ampere compat with its self-test passed)
    std::string vfgWhy;   // why not
    bool nvof = false;    // NVIDIA optical-flow hardware through the driver's library (the fast tier, the guard's flow)
};
struct Settings {
    bool enable = true;
    int smoother = 0;       // 0 Auto, 1 off, 2..8
    int sharper = 0;        // 0 Auto, 1..4 fixed VSR level, -1 off
    int fgQuality = -1;     // -1 Auto, 0 Low, 1 Medium, 2 High
    int fgMethod = -1;      // -1 Auto (VFG where it runs, else optical flow), 0 VFG, 1 optical flow (NVOF + warp)
    int sdrToHdr = 0;       // 0 Auto (on when the output is HDR), 1 on, -1 off
    int vsrStyle = -1;      // -1 Auto, 0 standard, 1 high bitrate, 2 streaming
    int targetH = 2160;     // the upscale target height (Screen = the display's when known, else 2160)
    bool targetScreen = true;   // UpscaleTarget Screen: the display's size decides (TargetForDisplay)
    double hdrPeak = 1000;  // RTX Video HDR peak (cd/m2)
    bool hdrPeakSet = false;    // the user chose it (else the display's peak when mpv reports one)
};
struct Source { int w = 1920, h = 1080; double fps = 23.976; bool hdr = false; bool tenBit = false; double bpp = -1; };
struct Display { double refreshHz = 0; bool vrr = false; bool hdr = false; };   // refreshHz <= 0: unknown

struct Plan {
    int k = 1, vfgQuality = 1;
    bool vsr = false; int vsrLevel = 0, vsrStyle = 0, vsrQuality = 0;
    int outW = 0, outH = 0;
    bool hdr = false;            // RTX Video HDR (SDR source, HDR output)
    bool interpElsewhere = false;   // this backend upscales only: interpolation by ml / rife / portable before it
    bool nvof = false;              // frames generated by NVIDIA optical flow + our warp (no VFG on this GPU, or asked for)
    bool guardFlow = false;         // the GPU cut guard gets the optical flow (its flow measures)
    double predictedMs = 0, budgetMs = 0;
    std::string why;
};

// UpscaleTarget Screen with a known display height: the Windows rule (PushMpcAuto.h TargetExtent): up to 1440 -> 1440p,
// up to 2160 -> 4K, above -> 5K
inline int TargetForDisplay(int displayHeight) { return displayHeight <= 0 ? 2160 : displayHeight <= 1440 ? 1440 : displayHeight <= 2160 ? 2160 : 2880; }
inline int EvenUp(double v) { return int(v + 0.5) & ~1; }

inline Plan Decide(const Settings& s, const Source& src, const Display& d, const Caps& caps, Costs c)
{
    Plan r;
    const double fps = src.fps > 1 ? src.fps : 23.976;
    r.budgetMs = 1000.0 / fps * kBudgetShare;
    char b[512];
    std::string notes;
    // --- size: VSR to the target, or the source size when it is already there
    const double scale = double(s.targetH) / double(src.h);
    const bool atOutput = scale <= 1.05;
    int upW = src.w, upH = src.h;
    if (!atOutput) { upW = EvenUp(src.w * scale); upH = EvenUp(src.h * scale); }
    const bool wantVsr = caps.vsr && s.sharper != -1 && !atOutput;
    // --- style: Auto = high bitrate for a Blu-ray-class bitrate, streaming for a low one (not RTX 20), else standard
    int style = s.vsrStyle >= 0 ? s.vsrStyle : 0;
    if (s.vsrStyle < 0 && src.bpp >= 0) style = src.bpp >= 0.40 ? 1 : (src.bpp <= 0.18 && caps.gpu != GpuClass::Rtx20 ? 2 : 0);
    if (style == 2 && caps.gpu == GpuClass::Rtx20) style = 0;   // streaming needs Ampere or newer
    // --- SDR to HDR (RTX Video HDR): Auto when the output is HDR and the source SDR
    r.hdr = caps.hdr && !src.hdr && (s.sdrToHdr == 1 || (s.sdrToHdr == 0 && d.hdr));
    if (s.sdrToHdr >= 0 && !src.hdr && d.hdr && !caps.hdr) notes += "; RTX Video HDR not available";
    const int outW = wantVsr ? upW : src.w, outH = wantVsr ? upH : src.h;
    const double pixOut = Pix(outW, outH);
    const double base = (r.hdr ? c.hdr4k * pixOut * c.card : 0.0);
    auto vsr = [&](int level) { return wantVsr ? VsrCost(c, level, style, pixOut) : 0.0; };
    auto over = [&](int m) { return c.overheadPerOutput4k * pixOut * c.card * m; };
    int level = wantVsr ? (s.sharper >= 1 ? s.sharper : 1) : 0;
    // --- frame generation
    const double refresh = d.refreshHz > 1 ? d.refreshHz : fps * 2;   // unknown display: aim at x2
    int k = 1, q = s.fgQuality >= 0 ? s.fgQuality : 1;
    std::string stop;
    // the method: VFG where it runs (unless optical flow is asked for), else NVIDIA optical flow + our warp
    const bool useNvof = caps.nvof && (s.fgMethod == 1 || (!caps.vfg && s.fgMethod != 0));
    const double srcPix1080 = double(src.w) * double(src.h) / (1920.0 * 1080.0);
    auto fgCost = [&](int quality, int m) { return useNvof ? NvofCost(c, m, srcPix1080, pixOut) : VfgCost(c, quality, m, pixOut); };
    if (s.smoother == 1) stop = "Smoother off";
    else if (!caps.vfg && !useNvof) { r.interpElsewhere = true; stop = "no VFG on this GPU (" + caps.vfgWhy + ") and no optical-flow hardware: interpolation by ml / rife / portable, this backend upscales"; }
    else if (s.smoother >= 2) { k = std::min(s.smoother, 8); stop = "your setting"; }
    else {
        auto pick = [&](int quality, std::string& why) {
            int best = 1, lastFit = 1; double bestHold = HoldMs(refresh, d.vrr, fps); bool bestEven = true;
            why.clear();
            for (int m = 2; m <= 8; ++m) {
                const double out = fps * m;
                if (out > refresh * kRefreshSlack) { snprintf(b, sizeof(b), "refresh %.2f Hz (%dx would be %.1f fps)", refresh, m, out); why = b; break; }
                const double cost = base + vsr(level) + fgCost(quality, m) + over(m);
                if (cost > r.budgetMs) { snprintf(b, sizeof(b), "GPU budget (%dx would take %.1f of %.1f ms)", m, cost, r.budgetMs); why = b; break; }
                lastFit = m;
                const double h = HoldMs(refresh, d.vrr, out);
                const bool even = d.vrr || CadenceEven(refresh, out);
                const bool better = h < bestHold - 0.05 || (std::fabs(h - bestHold) <= 0.05 && ((even && !bestEven) || even == bestEven));
                if (best == 1 || better) { best = m; bestHold = h; bestEven = even; }
            }
            if (why.empty()) why = useNvof ? "8x, the most this backend makes" : "8x, the most VFG makes";
            if (lastFit > best) { snprintf(b, sizeof(b), "cadence at %.2f Hz (%dx would not show frames shorter); next: ", refresh, lastFit); why = b + why; }
            return best;
        };
        k = pick(q, stop);
        if (k < 2 && s.fgQuality < 0 && !useNvof) { std::string s2; const int k2 = pick(0, s2); if (k2 >= 2) { k = k2; q = 0; stop = s2; } }
        // High at the same multiplier with 3 ms to spare (Auto quality only)
        if (k >= 2 && s.fgQuality < 0 && q == 1 && !useNvof) {
            const double up = base + vsr(level) + VfgCost(c, 2, k, pixOut) + over(k);
            if (up <= r.budgetMs - kHighSpareMs) q = 2;
        }
    }
    r.k = k; r.vfgQuality = q;
    // --- the rest of the budget to VSR quality (frame generation is never lowered for it)
    if (wantVsr && s.sharper == 0) {
        const double fixed = base + fgCost(q, k) + over(k);
        while (level < 4 && fixed + vsr(level + 1) <= r.budgetMs) ++level;
    }
    r.vsr = wantVsr; r.vsrLevel = level; r.vsrStyle = style; r.vsrQuality = wantVsr ? VsrQualityLevel(style, level) : 0;
    r.outW = outW; r.outH = outH;
    r.predictedMs = base + vsr(level) + fgCost(q, k) + over(k);
    r.nvof = useNvof && k >= 2;
    // the guard's flow: with the optical-flow tier it is there anyway; beside VFG when the budget has room for it
    r.guardFlow = caps.nvof && k >= 2 && (r.nvof || r.predictedMs + c.guardFlow * srcPix1080 * c.card <= r.budgetMs);
    if (r.guardFlow && !r.nvof) r.predictedMs += c.guardFlow * srcPix1080 * c.card;
    snprintf(b, sizeof(b), "x%d%s, VSR %s, RTX Video HDR %s, %dx%d, predicted %.2f of %.2f ms; stopped at: %s%s",
             k, k >= 2 ? (r.nvof ? " optical flow (NVOFA + warp)" : q >= 2 ? " VFG High" : q <= 0 ? " VFG Low" : " VFG Medium") : "",
             wantVsr ? (std::string("level ") + std::to_string(level) + (style == 1 ? " high bitrate" : style == 2 ? " streaming" : "")).c_str() : (atOutput ? "off (source at the target)" : caps.vsr ? "off" : "not available"),
             r.hdr ? "on" : "off", outW, outH, r.predictedMs, r.budgetMs, stop.c_str(), notes.c_str());
    r.why = b;
    return r;
}

} // namespace pmvs::nvl
