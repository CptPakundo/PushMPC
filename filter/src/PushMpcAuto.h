// PushMPC "Auto": the upgrade ladder.
//
//  1. Frame generation first. The method by GPU family (RTX 50/40/30: NVIDIA VFG, then DLSS-G, PushFlow, FSR FG;
//     RTX 20 and GTX: PushFlow, then FSR FG; Radeon: PushFlow, then FSR FG (FSR 4 ML on RX 9000), then AMD FRC; Intel:
//     PushFlow, then FSR FG), the next one only when the method is not available on this GPU or cannot reach 2x
//     within the budget. The multiplier with the method's base quality (VFG Medium, PushFlow Balanced): the largest M
//     whose output fits the display (fps x M <= refresh x 1.02) and whose predicted p95 GPU cost fits the budget
//     (source interval x 0.85), with the upscaler at its lowest Auto level; among those the one with the shortest
//     longest on-screen hold, a tie going to the cadence-even one (refresh / (fps x M) within 2 % of a whole number;
//     every M while VRR is active), then to the larger ("highest that fits").
//  2. Then the upgrade to High (PushFlow: Quality) only if it fits at that SAME multiplier with at least 3 ms p95 to
//     spare. A multiplier step is never given up for it. The user's "FG quality" overrides this.
//  3. Then the rest of the budget goes to RTX VSR quality 1 -> 4. Frame generation is never lowered to raise VSR.
//  4. A source already at the output size gets VSR's artifact reduction only when its bitrate is low and it fits after
//     frame generation (never at the cost of a frame generation step).
//  5. Clean up (AI enhancement) Auto: on for a low bitrate per pixel where the GPU has an NR runtime; it may cost
//     frame generation steps above 2x but never 2x itself; its cost (per family, then measured) gates it.
//  6. SDR to HDR Auto: on when Windows HDR is on for the window's display and the source is SDR: RTX Video HDR on an
//     NVIDIA RTX card, else PushMPC's own inverse tone map (BT.2446 method A) in the filter.
//  7. Deband Auto (PushMPC's own pass on the source): on for 8-bit SDR sources with a low bitrate; never on 10-bit or
//     HDR; it may never cost a frame generation step.
//  8. The VSR style (RTX VSR model family) Auto by the source bitrate: Streaming (cleans hardest, RTX 30+) at a low
//     bitrate, High bitrate (keeps clean detail) at a Blu-ray-class bitrate, Standard otherwise.
// The costs start from the measured RTX 5090 tables (times a per-card factor) and are refined from pushcore's GPU
// timings and PushFlow's once-per-card speed test (Costs, HKCU\Software\PushMPC\Learned).
#pragma once
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <format>
#include <string>
#include <vector>
#include "PushMpcSettings.h"

namespace pm {

// ------------------------------------------------------------------------------------------------ inputs
struct Display {
    std::wstring name = L"display";   // monitor friendly name
    double refreshHz = 60.0;          // exact rational rate of the mode
    UINT width = 1920, height = 1080; // desktop mode
    bool hdr = false;                 // Windows HDR (advanced colour) on for this display
    bool vrr = false;                 // variable refresh active for MPC-HC's window (detected, or the user's override)
    bool vrrKnown = false;            // false: could not be determined (fixed refresh assumed)
    std::wstring vrrHow;              // how vrr was decided (status line)
    bool vrrCapable = false;          // the display can do G-SYNC / VRR in this mode (NVIDIA), whatever the profile says
    bool battery = false;             // the system runs on battery (GetSystemPowerStatus)
    bool operator==(const Display& o) const { return name == o.name && std::fabs(refreshHz - o.refreshHz) < 0.01 && width == o.width && height == o.height && hdr == o.hdr && vrr == o.vrr && battery == o.battery; }
};
struct Source {
    UINT w = 1920, h = 1080;
    double fps = 23.976;      // in stream time (playback rate included)
    bool hdr = false;         // PQ / HLG
    double bppEq = -1;        // H.264-equivalent bits per pixel per frame (< 0 = unknown)
    bool tenBit = false;      // P010 input (10-bit)
};
// The GPU (pushcore_capabilities): family and what it can run with the installed files. Bit numbers = PUSHCORE_CAP_*.
enum GpuFamily : uint32_t { kFamUnknown = 0, kRtx50, kRtx40, kRtx30, kRtx20, kGtx, kRdna4, kRdna3, kRdna2, kRdna1, kAmdOther, kIntel, kOtherGpu };
enum Cap : uint32_t { kCapVfg = 0, kCapDlssg, kCapPushFlow, kCapFsrFg, kCapFsr4Fg, kCapFrc, kCapRtxVsr, kCapVsrFamilies, kCapFsrSr, kCapNis, kCapRtxHdr, kCapNr, kCapDlssgLite, kCapNvofFg,
                     kCapXefg, kCapVplFi, kCapXessSr, kCapIvsr, kCapCount };   // = PUSHCORE_CAP_* (18 ZLUDA VSR, 19-20 community: at merge)
struct Gpu {
    uint32_t family = kRtx50;          // default for the headless test: this machine
    uint32_t available = 0xFFFFFFFFu;  // bit per Cap
    uint32_t hardware = 0xFFFFFFFFu;   // bit per Cap: the GPU could run it (with its files); 0 = a GPU limit
    std::wstring name;
    std::wstring reason[kCapCount];    // why not available
    // Intel video methods: the self-tests' measured numbers (pushcore_capabilities reason[] of an available cap)
    double ivsrMs4k = -1;              // Intel VSR, 1080p -> 4K blit, ms
    double vplMs1080 = -1;             // Intel VPL FI, ms per source frame at 1080p 2x
    bool vplGate = false;              // VPL passed the default-on gate (discrete Arc, quality and speed margins)
    double xessMs4k = -1;              // Intel XeSS SR, ms per frame for a 4K output (its self-test)
    double xefgMs4k = -1;              // Intel XeSS FG, ms per generated frame for a 4K output (its self-test)
    bool has(uint32_t cap) const { return (available >> cap) & 1u; }
    bool hw(uint32_t cap) const { return (hardware >> cap) & 1u; }
    bool nvidia() const { return family >= kRtx50 && family <= kGtx; }
    bool rtx() const { return family >= kRtx50 && family <= kRtx20; }
};
inline const wchar_t* FamilyName(uint32_t f)
{
    static const wchar_t* n[] = { L"unknown GPU", L"RTX 50", L"RTX 40", L"RTX 30", L"RTX 20", L"GTX", L"RX 9000", L"RX 7000", L"RX 6000", L"RX 5000", L"Radeon", L"Intel", L"other GPU" };
    return f < 13 ? n[f] : n[0];
}
// GPU time relative to the RTX 5090 for the priors, before anything was measured (estimates)
inline double CardFactor(const std::wstring& name, uint32_t family)
{
    auto has = [&](const wchar_t* s) { std::wstring a = name; for (auto& c : a) c = wchar_t(towupper(c)); std::wstring b = s; for (auto& c : b) c = wchar_t(towupper(c)); return a.find(b) != std::wstring::npos; };
    struct E { const wchar_t* k; double f; };
    static const E table[] = { { L"5090", 1.0 }, { L"4090", 1.0 }, { L"5080", 1.6 }, { L"4080", 1.7 }, { L"5070 TI", 2.0 }, { L"5070", 2.5 }, { L"4070 TI", 2.3 },
        { L"4070", 2.8 }, { L"5060", 3.5 }, { L"4060", 4.5 }, { L"3090", 2.7 }, { L"3080", 3.0 }, { L"3070", 4.0 }, { L"3060", 5.5 }, { L"2080", 4.0 },
        { L"2070", 5.5 }, { L"2060", 8.0 }, { L"9070 XT", 2.2 }, { L"9070", 2.6 }, { L"9060", 4.0 }, { L"7900", 2.4 }, { L"7800", 3.5 }, { L"7700", 4.0 },
        { L"7600", 6.0 }, { L"6900", 3.5 }, { L"6800", 4.0 }, { L"6700", 5.5 }, { L"6600", 7.5 }, { L"5700", 8.0 } };
    for (const auto& e : table) if (has(e.k)) return e.f;
    return family == kRtx50 || family == kRtx40 ? 2.0 : family == kRtx30 ? 3.5 : 5.0;
}

// A GPU where PushFlow (RIFE) often cannot keep up and FSR 3.1 frame generation is the cold-start default (T5b item 2):
// NVIDIA GTX (no tensor cores), RX 5000 and older Radeon, Intel integrated graphics (not Arc), other GPUs (Snapdragon
// Adreno and the rest). Only the cold start is decided by this list: once PushFlow's speed test has run on the card
// (Costs.pfBenched), the measured costs decide between PushFlow and FSR FG like everywhere else.
inline bool IsWeakGpu(uint32_t family, const std::wstring& name)
{
    if (family == kGtx || family == kRdna1 || family == kAmdOther || family == kOtherGpu) return true;
    if (family == kIntel) {
        std::wstring a = name; for (auto& ch : a) ch = wchar_t(towupper(ch));
        return a.find(L"ARC") == std::wstring::npos;   // "Intel(R) Arc(TM) B580" is discrete-class; "Iris Xe", "UHD", "Graphics" are not
    }
    return false;
}

// Frame generation methods as Auto sees them (index = FgMethod - 1 = PUSHCORE_FG_*)
enum FgKey : int { kKeyVfgLow = 0, kKeyVfgMed, kKeyVfgHigh, kKeyDlssg, kKeyPfFast, kKeyPfBal, kKeyPfQual, kKeyFsr, kKeyFrc, kKeyNvof, kKeyVpl, kKeyXefg, kFgKeys };

// Measured costs (RTX 5090), ms per source frame at p95 for a
// 4K output; refined by measurement (fgScale per method and quality, PushFlow's own speed test).
struct Costs {
    // FG batch p95 per multiplier M = 2..8 at 3840x2160 after VSR (the slower of the two runs, 3840x2080 x 1/0.963;
    // 3x and 7x interpolated). The old single 7.4 ms per frame was High's.
    double vfgMedium[7] = { 4.22, 7.12, 10.02, 13.78, 14.91, 18.71, 22.51 };
    double vfgHigh[7]   = { 9.39, 17.72, 26.04, 34.74, 44.14, 53.50, 62.91 };
    double vfgLowShare = 0.40;                      // VFG Low ~0.35-0.4 x Medium (1080p quality runs)
    // DLSS-G with FidelityFX flow: flow + refine per source frame, then per generated frame (2x 1.47, 4x 3.87 p95)
    double dlssgFlow = 1.8, dlssgPerFrame = 1.2;
    // PushFlow (4K p95, gpu-best 4b): first generated frame of a pair, each further one; Fast, Balanced, Quality
    double pfFirst[3] = { 4.49, 5.68, 11.96 }, pfNext[3] = { 3.15, 3.93, 11.40 };
    bool pfBenched = false;                         // replaced by PushFlowBench on this card (already absolute)
    double fsrFg = 4.2;                             // FSR 3.1 FG 2x incl. FidelityFX flow
    // NVOF FG 2x: NVIDIA's optical-flow engine (fixed time per source frame, off the shaders; Turing's NVOFA about half
    // the speed of Ampere+'s) + FSR 3.1's interpolation without its own flow (~60 % of fsrFg). RTX 5090, clip A 1080p ->
    // 4K NIS, 2x: graph span 4.9-5.0 ms against FSR FG's 3.0 ms (T5b, 2026-11-25)
    double nvofEngine = 2.0, nvofInterpShare = 0.6;
    bool nvofTuring = false;                        // set by Decide for RTX 20 / GTX 16
    double frc = 6.7;                               // AMD FRC 2x
    // Intel VPL AI frame interpolation: ms per source frame at 1080p 2x (an estimate for Arc until its self-test measured it;
    // then that number, absolute), per further generated frame the same again, with the output pixels
    double vplFi = 12.0; bool vplMeasured = false;
    // Intel VSR (the driver's video processor super resolution): ms for a 4K output (estimate; the self-test's measurement)
    double ivsrSr = 2.0; bool ivsrMeasured = false;
    double ivsrScale = 1.0; bool learnedIvsr = false;   // learned measured/prior ratio (pushcore's srMs while it runs)
    // Intel XeSS frame generation (headless swap chain): the graph's flow + refine per source frame (as DLSS-G's), XeFG's
    // interpolation per generated frame at 4K (estimate for Arc with the card factor; learned through fgScale)
    double xefgFlow = 1.8, xefgPerFrame = 4.0; bool xefgMeasured = false;   // measured: per frame absolute (the self-test)
    // Intel XeSS SR (temporal, XMX on Arc): ms for a 4K output (estimate; the self-test's measurement), learned ratio
    double xessSr = 3.5; bool xessMeasured = false;
    double xessScale = 1.0; bool learnedXess = false;
    double vsrUp[4] = { 0.61, 0.76, 1.59, 2.40 };   // RTX VSR standard 1080p -> 4K, median
    double vsrUpHb[4] = { 1.19, 1.80, 2.73, 4.40 }; // VFX "high bitrate" 16..19 (measured, gpu-best 4b)
    double vsrUpSt[4] = { 1.59, 1.59, 4.52, 4.52 }; // VFX "streaming" 21 (tiers 1-2) / 23 (tiers 3-4)
    double vsrAr[4] = { 1.46, 3.60, 5.70, 7.77 };   // 4K -> 4K artifact reduction (L2, L3 interpolated)
    double fsrSr = 0.65, nisSr = 0.19;              // FSR upscale incl. its flow, NIS (4K)
    double deband1080 = 0.15;                       // deband pass at 1080p (medium), p95 prior
    double vsrP95 = 1.20;
    double base4k = 0.45;                           // colour, conversion, upload (whole graph minus stages)
    double nrMs = 4.0;                              // AI enhancement p95 (prior; per family below until measured)
    double hdr4k = 2.0;                             // RTX Video HDR at 4K (prior)
    double itm4k = 0.2;                             // PushMPC's inverse tone map in the filter (estimate)
    double guardScore = 0.3;                        // the cut guard's own test per source frame (scene score + Fourier-Mellin: 0.25-0.29 ms, round 5)
    double guardFlow = 1.0;                         // the cut guard's NVOF pass per source frame: +1.0 ms graph time at 1080p, RTX 5090 (round 4)
    double card = 1.0;                              // CardFactor for the priors (or the measured one, ApplyMeasuredCard)
    bool cardMeasured = false;                      // card comes from this GPU's own speed test, not the name table
    double vsrScale = 1.0;                          // learned measured/prior ratio for VSR
    double fgScale[kFgKeys] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };   // learned measured/prior ratio per method and quality
    bool fgLearned[kFgKeys] = {};
    bool learnedVsr = false, learnedNr = false, learnedHdr = false, learnedDeband = false;
};

struct Choice {               // resolved values for pushcore and the filter
    bool sr = false; int srLevel = 0; bool artifactReduction = false; UINT outW = 0, outH = 0;
    int srFamily = 0;         // VsrStyle actually used (Standard for artifact reduction)
    Upscaler srMethod = Upscaler::Rtx;
    int multiplier = 1; bool nr = false; bool hdr = false; bool hdrItm = false;
    HdrBoost hdrBoost = HdrBoost::Off;   // resolved boost (never Auto): PushMPC's curve M, or RTX Video HDR's peak
    int rtxHdrPeak = 1000;               // RTX Video HDR peakNits for the boost (Low 500 / Medium 700 / High 1000)
    bool hdrLimit = true;                // the bright-scene limiter (PushMPC's curve, or on RTX Video HDR's output)
    bool guardFlow = false;              // the cut guard also uses NVIDIA's optical flow (else its scene score alone)
    std::wstring guardNote;
    std::wstring batteryNote;            // "on battery: light profile" when it applies
    FgMethod fgMethod = FgMethod::Vfg;   // the method in use (Auto never)
    int fgQuality = 1;        // VFG model 0..2, or PushFlow tier 0..2
    bool deband = false; int debandStrength = 1;
    double predictedMs = 0, budgetMs = 0;
    std::wstring smootherNote, sharperNote, cleanNote, hdrNote, debandNote, stoppedAt, fgNote, qualityNote;
    bool sameGraph(const Choice& o) const { return sr == o.sr && srLevel == o.srLevel && srFamily == o.srFamily && multiplier == o.multiplier && nr == o.nr && hdr == o.hdr && outW == o.outW && outH == o.outH && fgMethod == o.fgMethod && fgQuality == o.fgQuality && srMethod == o.srMethod; }
};

// ------------------------------------------------------------------------------------------------ rules
inline constexpr double kBudgetShare = 0.85;      // 15 % headroom against the p95 cost
inline constexpr double kRefreshSlack = 1.02;     // output fps <= refresh x 1.02
inline constexpr double kCadenceTolerance = 0.02; // refresh / output fps within 2 % of a whole number
inline constexpr double kHighSpareMs = 3.0;       // High (PushFlow Quality) only with this much p95 to spare
// Clean up Auto: H.264-equivalent bits per pixel per frame at or below this is "streaming level". Blu-ray 1080p H.264
// at 25-35 Mbit/s is 0.5-0.7; a web stream 1080p24 at 6 Mbit/s H.264 is 0.12, YouTube AV1 at 2.5 Mbit/s ~0.09
// equivalent; a UHD Blu-ray HEVC at 60 Mbit/s is ~0.48 equivalent. Codec factors in CodecEfficiency.
inline constexpr double kLowBitrateBpp = 0.18;
inline constexpr double kDebandBpp8Bit = 0.25;
// VSR style Auto: Blu-ray class at or above this (1080p H.264 remux 0.5-0.7, UHD HEVC ~0.48): high bitrate
inline constexpr double kHighBitrateBpp = 0.40;

inline bool CadenceEven(double refresh, double outFps)
{
    if (outFps <= 0) return false;
    const double r = refresh / outFps, k = std::round(r);
    return k >= 1 && std::fabs(r - k) / k <= kCadenceTolerance;
}
// The longest time one output frame stays on screen (ms): with VRR 1 / fps; on a fixed refresh round(k) refreshes for an
// even cadence and ceil(k) for an uneven one (k = refresh / fps).
inline double HoldMs(const Display& d, double outFps)
{
    if (outFps <= 0) return 1e9;
    if (d.vrr) return 1000.0 / outFps;
    const double k = d.refreshHz / outFps;
    const double n = CadenceEven(d.refreshHz, outFps) ? std::round(k) : std::ceil(k);
    return std::max(1.0, n) * 1000.0 / d.refreshHz;
}
inline UINT Even(double v) { return UINT(v + 0.5) & ~1u; }

inline void TargetExtent(UpscaleTarget t, const Display& d, UINT& w, UINT& h)
{
    if (t == UpscaleTarget::Screen) t = d.height <= 1440 ? UpscaleTarget::P1440 : d.height <= 2160 ? UpscaleTarget::K4 : UpscaleTarget::K5;
    if (t == UpscaleTarget::P1440) { w = 2560; h = 1440; } else if (t == UpscaleTarget::K5) { w = 5120; h = 2880; } else { w = 3840; h = 2160; }
}
inline UpscaleTarget ResolvedTarget(UpscaleTarget t, const Display& d)
{
    if (t != UpscaleTarget::Screen) return t;
    return d.height <= 1440 ? UpscaleTarget::P1440 : d.height <= 2160 ? UpscaleTarget::K4 : UpscaleTarget::K5;
}

inline double Pix(UINT w, UINT h) { return double(w) * double(h) / (3840.0 * 2160.0); }
inline double VsrCost(const Costs& c, int level, bool ar, double pix, int family = 0, Upscaler method = Upscaler::Rtx)
{
    if (level < 1) return 0;
    if (method == Upscaler::Fsr) return c.fsrSr * pix * c.vsrP95 * c.card;
    if (method == Upscaler::Nis) return c.nisSr * pix * c.vsrP95 * c.card;
    if (method == Upscaler::IntelVsr) return c.ivsrSr * pix * c.vsrP95 * c.ivsrScale * (c.ivsrMeasured ? 1.0 : c.card);
    if (method == Upscaler::Xess) return c.xessSr * pix * c.vsrP95 * c.xessScale * (c.xessMeasured ? 1.0 : c.card);
    const double* table = ar ? c.vsrAr : family == int(VsrStyle::HighBitrate) ? c.vsrUpHb : family == int(VsrStyle::Streaming) ? c.vsrUpSt : c.vsrUp;
    return table[std::min(level, 4) - 1] * pix * c.vsrP95 * c.vsrScale * c.card;
}
inline double DebandCost(const Costs& c, int strength, UINT w, UINT h)
{
    const double k = strength <= 0 ? 0.6 : strength >= 2 ? 1.8 : 1.0;
    return c.deband1080 * k * double(w) * double(h) / (1920.0 * 1080.0);
}
inline int FgKeyOf(FgMethod m, int quality)
{
    switch (m) {
    case FgMethod::Vfg: return quality <= 0 ? kKeyVfgLow : quality >= 2 ? kKeyVfgHigh : kKeyVfgMed;
    case FgMethod::Dlssg: return kKeyDlssg;
    case FgMethod::PushFlow: return kKeyPfFast + std::clamp(quality, 0, 2);
    case FgMethod::Fsr: return kKeyFsr;
    case FgMethod::AmdFrc: return kKeyFrc;
    case FgMethod::Nvof: return kKeyNvof;
    case FgMethod::Vpl: return kKeyVpl;
    case FgMethod::XeFg: return kKeyXefg;
    default: return kKeyVfgMed;
    }
}
inline int MaxMultiplier(FgMethod m) { return m == FgMethod::Vfg ? 8 : m == FgMethod::Dlssg ? 6 : m == FgMethod::PushFlow || m == FgMethod::Vpl || m == FgMethod::XeFg ? 4 : 2; }
// The prior (before fgScale): p95 GPU ms per source frame of the frame generation group at multiplier m.
// pix = output pixels / 4K. VFG and DLSS-G barely depend on the size (2.2-2.8 ms per Medium frame from 1080p to 4K).
inline double FgPrior(const Costs& c, FgMethod method, int quality, int m, double pix)
{
    if (m < 2) return 0;
    const double sizeVfg = 0.75 + 0.25 * pix;
    switch (method) {
    case FgMethod::Vfg: {
        const double med = c.vfgMedium[std::min(m, 8) - 2], high = c.vfgHigh[std::min(m, 8) - 2];
        const double v = quality >= 2 ? high : quality <= 0 ? med * c.vfgLowShare : med;
        return v * sizeVfg * c.card;
    }
    case FgMethod::Dlssg: return (c.dlssgFlow * pix + c.dlssgPerFrame * (m - 1) * sizeVfg) * c.card;
    case FgMethod::PushFlow: {
        const int t = std::clamp(quality, 0, 2);
        const double v = c.pfFirst[t] + c.pfNext[t] * (m - 2);
        return v * (0.6 + 0.4 * pix) * (c.pfBenched ? 1.0 : c.card);
    }
    case FgMethod::Fsr: return c.fsrFg * pix * c.card;
    case FgMethod::AmdFrc: return c.frc * pix * c.card;
    case FgMethod::Nvof: return c.nvofEngine * (c.nvofTuring ? 2.0 : 1.0) + c.fsrFg * c.nvofInterpShare * pix * c.card;
    case FgMethod::Vpl: return c.vplFi * (pix / 0.25) * (m - 1) * (c.vplMeasured ? 1.0 : c.card);
    case FgMethod::XeFg: return (c.xefgFlow * c.card + c.xefgPerFrame * (m - 1) * (c.xefgMeasured ? 1.0 : c.card)) * pix;
    default: return 0;
    }
}
inline double FgCost(const Costs& c, FgMethod method, int quality, int m, double pix)
{
    return FgPrior(c, method, quality, m, pix) * c.fgScale[FgKeyOf(method, quality)];
}
inline std::vector<FgMethod> FgOrder(const Gpu& g);
// The measured card factor (T5b item 11): where no per-method measurement exists, the priors scale with the card. The
// name table (CardFactor) is a guess; PushFlow's speed test (pushcore_pushFlowBench, run once per card, cached in
// HKCU\Software\PushMPC\Learned) is a measurement of this GPU's shader + ML throughput at the real size. Its Balanced
// first-frame p95, normalised to 4K, against the RTX 5090's 5.68 ms, replaces the table once it exists.
inline constexpr double kPfBalanced5090 = 5.68;
inline bool ApplyMeasuredCard(Costs& c)
{
    if (!c.pfBenched || c.pfFirst[1] <= 0) return false;
    c.card = std::clamp(c.pfFirst[1] / kPfBalanced5090, 0.25, 80.0);
    c.cardMeasured = true;
    return true;
}
// The order Auto tries the frame generation methods in, by GPU family. The weak GPUs
// (IsWeakGpu) start with FSR 3.1 FG until PushFlow's speed test has measured the card (the cold start), then PushFlow
// first again: Decide takes it only where its measured cost reaches 2x, else FSR FG (decided by the benchmark, never by a
// fixed table).
inline std::vector<FgMethod> FgOrder(const Gpu& g, const Costs& c)
{
    if (IsWeakGpu(g.family, g.name) && !c.pfBenched) return { FgMethod::Nvof, FgMethod::Fsr, FgMethod::Vpl, FgMethod::XeFg, FgMethod::PushFlow, FgMethod::AmdFrc };
    return FgOrder(g);
}
inline std::vector<FgMethod> FgOrder(const Gpu& g)
{
    switch (g.family) {
    // NVOF FG (NVIDIA's optical-flow hardware + FSR 3.1 interpolation) before FSR FG where the card has NVOFA: on held-out
    // frames it measured 30.29 dB against FSR FG's 29.96 and PushFlow Fast's 28.82 (synthetic pan/zoom, 1080p 30 -> 60,
    // RTX 5090, T5b), and its flow costs no shader time; Decide takes it only where it fits (else FSR FG)
    case kRtx50: case kRtx40: case kRtx30: return { FgMethod::Vfg, FgMethod::Dlssg, FgMethod::PushFlow, FgMethod::Nvof, FgMethod::Fsr };
    case kRtx20: return { FgMethod::Dlssg, FgMethod::PushFlow, FgMethod::Nvof, FgMethod::Fsr };
    // GTX 10 / 16, RX 5000 and older Radeons, other GPUs: XeSS FG (any Shader Model 6.4 GPU since XeSS 2.1, after its self-test
    // passed on this card) right after PushFlow, before the 2x-only methods; Decide takes the first that reaches 2x in budget
    case kGtx: return { FgMethod::PushFlow, FgMethod::XeFg, FgMethod::Nvof, FgMethod::Fsr };
    // Intel: VPL AI frame interpolation first where it passed its default-on gate (discrete Arc: quality margin over a blend like
    // PushFlow Fast's on the probe material, <= 20.8 ms per source frame), then XeSS FG (Intel's own frame generation on the
    // XMX engines, the graph's flow as its motion; after its self-test), PushFlow, VPL as a candidate; Decide takes the first
    // that reaches 2x within the budget (costs: the self-tests' measurements, then live)
    case kIntel: return g.vplGate ? std::vector<FgMethod>{ FgMethod::Vpl, FgMethod::XeFg, FgMethod::PushFlow, FgMethod::Fsr }
                                  : std::vector<FgMethod>{ FgMethod::XeFg, FgMethod::PushFlow, FgMethod::Vpl, FgMethod::Fsr };
    case kOtherGpu: return { FgMethod::PushFlow, FgMethod::XeFg, FgMethod::Fsr };
    case kRdna1: case kAmdOther: return { FgMethod::PushFlow, FgMethod::XeFg, FgMethod::Fsr, FgMethod::AmdFrc };
    default: return { FgMethod::PushFlow, FgMethod::Fsr, FgMethod::AmdFrc };   // Radeon RX 6000+
    }
}
// pushcore's numbering of the upscaler (PUSHCORE_SR_METHOD_* = Upscaler - 1; Auto never reaches pushcore: RTX)
inline uint32_t CoreSrMethod(Upscaler u) { return u == Upscaler::Auto ? 0u : uint32_t(int(u) - 1); }
inline Upscaler UpscalerOfCore(uint32_t m) { return m <= 4u ? Upscaler(int(m) + 1) : Upscaler::Rtx; }
// The Intel methods' self-test numbers, which pushcore_capabilities puts in reason[] of an AVAILABLE capability ("ms4k="
// Intel VSR / XeSS SR, "gate passed ... ms=" VPL FI): every route calls this after copying the reasons
inline void ReadMeasured(Gpu& g)
{
    auto num = [&](uint32_t cap, const wchar_t* key) {
        if (!g.has(cap)) return -1.0;
        const size_t p = g.reason[cap].find(key);
        return p == std::wstring::npos ? -1.0 : _wtof(g.reason[cap].c_str() + p + wcslen(key));
    };
    g.ivsrMs4k = num(kCapIvsr, L"ms4k=");
    g.xessMs4k = num(kCapXessSr, L"ms4k=");
    g.xefgMs4k = num(kCapXefg, L"ms4k=");
    g.vplMs1080 = num(kCapVplFi, L"ms=");
    g.vplGate = g.has(kCapVplFi) && g.reason[kCapVplFi].rfind(L"gate passed", 0) == 0;
}
inline uint32_t CapOf(FgMethod m) { return m == FgMethod::Vfg ? kCapVfg : m == FgMethod::Dlssg ? kCapDlssg : m == FgMethod::PushFlow ? kCapPushFlow : m == FgMethod::Fsr ? kCapFsrFg : m == FgMethod::Nvof ? kCapNvofFg : m == FgMethod::Vpl ? kCapVplFi : m == FgMethod::XeFg ? kCapXefg : kCapFrc; }
// base quality, upgrade, and the fallback when the base cannot reach 2x (VFG: Medium -> High, Low; PushFlow: Balanced ->
// Quality, Fast); the user's FG quality fixes it (PushFlow: Low/Medium/High = Fast/Balanced/Quality)
struct Tiers { int base, upgrade, fallback; };
inline Tiers TiersOf(FgMethod m, FgQuality q)
{
    if (m != FgMethod::Vfg && m != FgMethod::PushFlow) return { 0, -1, -1 };
    if (q != FgQuality::Auto) return { int(q), -1, -1 };
    return { 1, 2, 0 };
}
inline const wchar_t* QualityName(FgMethod m, int q)
{
    if (m == FgMethod::PushFlow) return q <= 0 ? L"Fast" : q >= 2 ? L"Quality" : L"Balanced";
    if (m == FgMethod::Vfg) return q <= 0 ? L"Low" : q >= 2 ? L"High" : L"Medium";
    return L"";
}

inline Choice Decide(const Settings& s, const Source& src, const Display& d, const Costs& costsIn, const Gpu& g = Gpu{})
{
    Choice r;
    Costs c = costsIn;
    // RTX VSR on GTX 16 (unlock Gtx16Vsr): no tensor cores, the HMMA work runs on the FP16 units at a fraction of the
    // rate, so its prior is 4x the RTX card factor until the running graph has measured it (vsrScale learned)
    if (g.family == kGtx && !c.learnedVsr) c.vsrScale *= 4.0;
    c.nvofTuring = g.family == kGtx || g.family == kRtx20;
    // FSR upscaling on RX 7000 / RX 6000 is FSR 4.1 INT8 where the driver offers it (the shipped upscaler DLL carries the
    // INT8 provider): ~4.1 ms to 4K on an RX 7800 XT, about twice the FSR 3.1 prior with that card's factor; RX 9000 runs
    // the FP8 model on its matrix units. The running graph's measurement replaces it (vsrScale learned).
    if ((g.family == kRdna3 || g.family == kRdna2) && !c.learnedVsr) c.fsrSr *= 2.0;
    const double fps = src.fps > 1 ? src.fps : 23.976;
    r.budgetMs = 1000.0 / fps * kBudgetShare;
    const bool lowBitrate = src.bppEq >= 0 && src.bppEq <= kLowBitrateBpp;
    const std::wstring rateNote = src.bppEq < 0 ? L"bitrate unknown" : std::format(L"{} bitrate ({:.2f} bits per pixel)", lowBitrate ? L"low" : L"high", src.bppEq);

    // the Intel self-tests' measurements are the priors (absolute: measured on this GPU)
    if (g.ivsrMs4k > 0 && !c.ivsrMeasured) { c.ivsrSr = g.ivsrMs4k; c.ivsrMeasured = true; }
    if (g.vplMs1080 > 0 && !c.vplMeasured) { c.vplFi = g.vplMs1080; c.vplMeasured = true; }
    if (g.xessMs4k > 0 && !c.xessMeasured) { c.xessSr = g.xessMs4k; c.xessMeasured = true; }
    if (g.xefgMs4k > 0 && !c.xefgMeasured) { c.xefgPerFrame = g.xefgMs4k; c.xefgMeasured = true; }
    // Intel's upscalers (Auto, no RTX VSR): XeSS SR (temporal AI on an Intel GPU's XMX engines) and Intel VSR (the driver's
    // video processor) are candidates against FSR / NIS by cost, in that order of expected quality: Auto takes the first
    // that costs no frame generation step against the shader upscaler and fits the budget (the same costs, measured where
    // known). XeSS SR also on the GPUs without a better upscaler (GTX, RX 5000 and older, other GPUs: DP4a), after its self-test.
    const bool xessFamily = g.family == kIntel || g.family == kGtx || g.family == kRdna1 || g.family == kAmdOther || g.family == kOtherGpu;
    const bool xessAuto = g.has(kCapXessSr) && xessFamily;
    if (s.upscaler == Upscaler::Auto && !g.has(kCapRtxVsr) && (xessAuto || g.has(kCapIvsr))) {
        Settings sf = s; sf.upscaler = g.has(kCapFsrSr) ? Upscaler::Fsr : Upscaler::Nis;
        Choice cf = Decide(sf, src, d, c, g);
        std::wstring why;
        for (const Upscaler u : { Upscaler::Xess, Upscaler::IntelVsr }) {
            if (u == Upscaler::Xess ? !xessAuto : !g.has(kCapIvsr)) continue;
            const wchar_t* name = u == Upscaler::Xess ? L"XeSS SR" : L"Intel VSR";
            Settings si = s; si.upscaler = u;
            Choice ci = Decide(si, src, d, c, g);
            if (!ci.sr) return cf;   // nothing to upscale: the same either way
            if (ci.multiplier >= cf.multiplier && (ci.predictedMs <= ci.budgetMs || ci.predictedMs <= cf.predictedMs)) {
                ci.sharperNote += std::format(L" ({}: no frame generation step lost){}", name, why);
                return ci;
            }
            why += std::format(L" ({} would cost frame generation: {}x instead of {}x, or {:.1f} of {:.1f} ms)", name, ci.multiplier, cf.multiplier, ci.predictedMs, ci.budgetMs);
        }
        cf.sharperNote += why;
        return cf;
    }
    // --- the upscaler: RTX VSR on NVIDIA RTX, else FSR, else NIS (the user's choice when this GPU can run it)
    Upscaler method = g.has(kCapRtxVsr) ? Upscaler::Rtx : g.has(kCapFsrSr) ? Upscaler::Fsr : Upscaler::Nis;
    std::wstring methodNote;
    if (s.upscaler == Upscaler::Rtx && !g.has(kCapRtxVsr)) methodNote = L" (RTX VSR: " + g.reason[kCapRtxVsr] + L")";
    else if (s.upscaler == Upscaler::Fsr && !g.has(kCapFsrSr)) methodNote = L" (FSR: " + g.reason[kCapFsrSr] + L")";
    else if (s.upscaler == Upscaler::IntelVsr && !g.has(kCapIvsr)) methodNote = L" (Intel VSR: " + g.reason[kCapIvsr] + L")";
    else if (s.upscaler == Upscaler::Xess && !g.has(kCapXessSr)) methodNote = L" (XeSS SR: " + g.reason[kCapXessSr] + L")";
    else if (s.upscaler != Upscaler::Auto) method = s.upscaler;
    r.srMethod = method;
    const bool rtxSr = method == Upscaler::Rtx;

    // --- output size: VSR to the target, or the source size (a source at or above the target: artifact reduction)
    UINT tw, th; TargetExtent(s.target, d, tw, th);
    const double scale = std::min(double(tw) / src.w, double(th) / src.h);
    const bool fillsScreen = s.target == UpscaleTarget::Screen && d.height < 1440 && std::min(double(d.width) / src.w, double(d.height) / src.h) <= 1.05;
    const bool atOutput = scale <= 1.05 || (fillsScreen && s.sharper == kAuto);
    UINT upW = src.w, upH = src.h;
    if (!atOutput) { upW = Even(src.w * scale); upH = Even(src.h * scale); }

    // --- SDR to HDR (display mode first: it is part of the base cost)
    if (s.sdrToHdr == kOff) { r.hdr = false; r.hdrNote = L"off"; }
    else if (src.hdr) { r.hdr = false; r.hdrNote = L"off, the video is already HDR"; }
    else if (!d.hdr) { r.hdr = false; r.hdrNote = L"off, Windows HDR is off"; }
    else {
        r.hdr = true;
        // the method: PushMPC's curve on every GPU (the default); RTX Video HDR only when asked for and available
        r.hdrItm = !(s.hdrMethod == HdrMethod::Rtx && g.has(kCapRtxHdr));
        // the boost: Auto by the source (High for a clean source, Low where the low-bitrate clean-up rule fires)
        std::wstring why;
        if (s.hdrBoost != HdrBoost::Auto) r.hdrBoost = s.hdrBoost;
        else if (src.bppEq >= 0 && src.bppEq <= kLowBitrateBpp) { r.hdrBoost = HdrBoost::Low; why = std::format(L", low bitrate ({:.2f} bits per pixel)", src.bppEq); }
        else if (src.tenBit || src.bppEq > kDebandBpp8Bit) { r.hdrBoost = HdrBoost::High; why = src.tenBit ? L", 10-bit source" : std::format(L", high bitrate ({:.2f} bits per pixel)", src.bppEq); }
        else { r.hdrBoost = HdrBoost::Medium; why = src.bppEq < 0 ? L", bitrate unknown" : std::format(L", {:.2f} bits per pixel", src.bppEq); }
        r.rtxHdrPeak = r.hdrBoost == HdrBoost::Low ? 500 : r.hdrBoost == HdrBoost::Medium ? 700 : 1000;
        const std::wstring boost = r.hdrBoost == HdrBoost::Off ? L"boost off (SDR at 203 cd/m2)" : std::wstring(L"boost ") + HdrBoostStr(r.hdrBoost) + (s.hdrBoost == HdrBoost::Auto ? why : L"");
        std::wstring method = r.hdrItm ? L"PushMPC's curve" : std::format(L"RTX Video HDR, peak {} cd/m2", r.rtxHdrPeak);
        if (r.hdrItm && s.hdrMethod == HdrMethod::Rtx) method += L"; RTX Video HDR: " + g.reason[kCapRtxHdr];
        if (!r.hdrItm && r.hdrBoost == HdrBoost::Off) method = L"PushMPC's curve (boost off)";
        if (r.hdrBoost == HdrBoost::Off) r.hdrItm = true;   // Off: the direct mapping, the same on every GPU
        r.hdrLimit = s.hdrLimit && r.hdrBoost != HdrBoost::Off;
        r.hdrNote = std::wstring(s.sdrToHdr == kAuto ? L"on, Windows HDR is on" : L"on") + L" (" + method + L", " + boost
                  + (r.hdrBoost == HdrBoost::Off ? L"" : r.hdrLimit ? L", bright scenes limited" : L", no bright-scene limit") + L")";
    }

    // --- sharpening: fixed level, Off, or Auto (starts at level 1; artifact reduction only on a low bitrate, RTX only)
    bool srOn = false; int level = 0;
    if (s.sharper == kOff) { srOn = false; }
    else if (s.sharper != kAuto) { srOn = !(atOutput && !rtxSr); level = srOn ? s.sharper : 0; }
    else if (!atOutput) { srOn = true; level = 1; }
    else { srOn = false; } // decided after frame generation
    if (!rtxSr && srOn) level = 1;   // FSR / NIS have no levels
    const double pixOut = Pix(atOutput ? src.w : (s.sharper == kOff ? src.w : upW), atOutput ? src.h : (s.sharper == kOff ? src.h : upH));
    const double pixFg = s.sharper == kOff || atOutput ? Pix(src.w, src.h) : Pix(upW, upH);
    const bool upscaled = srOn && !atOutput;
    (void)upscaled;

    // --- VSR style: the user's, or Auto by the bitrate (streaming needs RTX 30+; both VFX families need their files)
    int family = int(VsrStyle::Standard);
    std::wstring styleNote;
    if (rtxSr && !atOutput) {
        VsrStyle st = s.vsrStyle;
        if (st == VsrStyle::Auto) {
            st = VsrStyle::Standard;
            if (g.has(kCapVsrFamilies) && src.bppEq >= 0) {
                if (src.bppEq <= kLowBitrateBpp && g.family != kRtx20) { st = VsrStyle::Streaming; styleNote = L", streaming style (low bitrate)"; }
                else if (src.bppEq >= kHighBitrateBpp) { st = VsrStyle::HighBitrate; styleNote = L", high bitrate style (Blu-ray class bitrate)"; }
            }
        } else if (st != VsrStyle::Standard && !g.has(kCapVsrFamilies)) {
            styleNote = L", standard style (" + g.reason[kCapVsrFamilies] + L")"; st = VsrStyle::Standard;
        } else if (st != VsrStyle::Standard) styleNote = std::format(L", {} style", VsrStyleLabel(st));
        family = int(st);
    }
    r.srFamily = family;

    // --- deband (PushMPC's pass on the source; 8-bit SDR only)
    const bool debandEligible = !src.hdr && !src.tenBit;
    const bool debandRate = src.bppEq >= 0 && (lowBitrate || src.bppEq <= kDebandBpp8Bit);
    bool deband = false;
    if (s.deband == kOff) r.debandNote = L"off";
    else if (!debandEligible) r.debandNote = src.hdr ? L"off, HDR source (banding is rare there)" : L"off, 10-bit source (banding is rare there)";
    else if (s.deband != kAuto) { deband = true; r.debandNote = L"on"; }
    else if (debandRate) { deband = true; r.debandNote = std::format(L"on, 8-bit source at {:.2f} bits per pixel", src.bppEq); }
    else r.debandNote = L"off, " + rateNote;
    r.debandStrength = int(s.debandStrength);
    double debandMs = deband ? DebandCost(c, r.debandStrength, src.w, src.h) : 0.0;

    // NR cost by family until measured (heavy on RTX 30 and on the RX 7000 worker)
    const double nrPrior = c.learnedNr ? c.nrMs : g.family == kRtx50 ? c.nrMs : g.family == kRtx40 ? c.nrMs * 1.5 : g.family == kRtx30 ? c.nrMs * 4
                         : g.family == kRdna4 ? 6.0 * 1.2 : g.family == kRdna3 ? 30.0 : c.nrMs * 4;
    auto baseCost = [&](bool nr) {
        return c.base4k * pixFg * c.card + debandMs + (nr ? nrPrior : 0) + (r.hdr ? (r.hdrItm ? c.itm4k : c.hdr4k * c.card) * pixFg : 0);
    };
    auto vsr = [&](int l) { return srOn ? VsrCost(c, l, atOutput, pixOut, family, method) : 0.0; };

    // --- frame generation: method, multiplier (base quality), then the quality upgrade
    struct Pick { FgMethod m = FgMethod::Vfg; int mult = 1; int q = 1; std::wstring stop; };
    auto pickFor = [&](FgMethod m, int q, double base, std::wstring& stop) {
        // "Highest that fits": among the multipliers that fit the display and the budget, the one with the shortest
        // longest on-screen hold (fixed refresh: round(k) refreshes for an even cadence, ceil(k) for an uneven one; with
        // VRR 1 / fps, so the largest). A tie (within 0.05 ms) goes to the cadence-even one, then to the larger. So an
        // even multiplier wins whenever it is as smooth (5x over 6x-8x at 240 Hz), but Auto never drops to a much lower
        // multiplier just for evenness (High at 240 Hz: 4x, uneven, holds 12.5 ms; 2x, even, would hold 20.8 ms).
        int best = 1, lastFit = 1;
        double bestHold = HoldMs(d, fps);
        bool bestEven = true;
        std::wstring brk;
        const int maxM = MaxMultiplier(m);
        for (int mm = 2; mm <= maxM; ++mm) {
            const double out = fps * mm;
            if (out > d.refreshHz * kRefreshSlack) { brk = std::format(L"refresh {:.2f} Hz ({}x would be {:.1f} fps)", d.refreshHz, mm, out); break; }
            const double cost = base + vsr(level) + FgCost(c, m, q, mm, pixFg);
            if (cost > r.budgetMs) { brk = std::format(L"GPU budget ({} {}x would take {:.1f} of {:.1f} ms)", FgMethodLabel(m), mm, cost, r.budgetMs); break; }
            lastFit = mm;
            const double h = HoldMs(d, out);
            const bool even = d.vrr || CadenceEven(d.refreshHz, out);
            const bool better = h < bestHold - 0.05 || (std::fabs(h - bestHold) <= 0.05 && (even && !bestEven || even == bestEven));
            if (best == 1 || better) { best = mm; bestHold = h; bestEven = even; }
        }
        if (brk.empty()) brk = std::format(L"{}x, the most {} makes", maxM, FgMethodLabel(m));
        if (best >= 2 && !bestEven) brk = std::format(L"{}x is uneven at {:.2f} Hz but holds shortest ({:.1f} ms); next: {}", best, d.refreshHz, bestHold, brk);
        else if (lastFit > best) brk = std::format(L"cadence at {:.2f} Hz ({}x would not show frames shorter); next: {}", d.refreshHz, lastFit, brk);
        stop = brk;
        return best;
    };    auto chooseFg = [&](double base) {
        Pick p;
        if (s.smoother == kOff) { p.mult = 1; p.stop = L"off"; return p; }
        std::vector<FgMethod> order = FgOrder(g, c);
        std::wstring notes;
        if (IsWeakGpu(g.family, g.name) && !c.pfBenched && s.fgMethod == FgMethod::Auto) notes = L"cold start: FSR FG first until PushFlow's speed test has measured this GPU; ";
        if (s.fgMethod != FgMethod::Auto) {
            if (g.has(CapOf(s.fgMethod))) order.insert(order.begin(), s.fgMethod);
            else notes = std::format(L"{} not available ({}); ", FgMethodLabel(s.fgMethod), g.reason[CapOf(s.fgMethod)]);
        }
        for (size_t i = 0; i < order.size(); ++i) {
            const FgMethod m = order[i];
            if (i > 0 && m == order[0] && s.fgMethod != FgMethod::Auto) continue;
            if (!g.has(CapOf(m))) { notes += std::format(L"{} not available ({}); ", FgMethodLabel(m), g.reason[CapOf(m)]); continue; }
            const Tiers t = TiersOf(m, s.fgQuality);
            std::wstring stop;
            int q = t.base;
            int mult = s.smoother != kAuto ? std::min(s.smoother, MaxMultiplier(m)) : pickFor(m, q, base, stop);
            if (s.smoother == kAuto && mult < 2 && t.fallback >= 0) { std::wstring s2; const int m2 = pickFor(m, t.fallback, base, s2); if (m2 >= 2) { mult = m2; q = t.fallback; stop = s2; } }
            if (mult >= 2) {
                p.m = m; p.mult = mult; p.q = q; p.stop = stop.empty() ? L"fixed" : stop;
                if (!notes.empty()) p.stop = notes + p.stop;
                return p;
            }
            notes += std::format(L"{} cannot reach 2x ({}); ", FgMethodLabel(m), stop);
            if (s.fgMethod != FgMethod::Auto && m == s.fgMethod) break;   // a forced method: no other
        }
        p.mult = 1; p.stop = notes.empty() ? L"no frame generation method on this GPU" : notes;
        return p;
    };

    // deband Auto must never cost a frame generation step
    if (deband && s.deband == kAuto) {
        const double keep = debandMs; debandMs = 0;
        const int without = chooseFg(baseCost(false)).mult;
        debandMs = keep;
        if (chooseFg(baseCost(false)).mult < without) { deband = false; debandMs = 0; r.debandNote = L"off, it would cost a frame generation step"; }
    }
    r.deband = deband;

    // --- clean up: fixed, off, or Auto (low bitrate, an NR runtime on this GPU; never at the cost of 2x)
    const int mNoNr = chooseFg(baseCost(false)).mult;
    bool nr = false;
    if (s.cleanUp == kOff) { r.cleanNote = L"off"; }
    else if (!g.has(kCapNr)) { r.cleanNote = L"off, " + g.reason[kCapNr]; }
    else if (s.cleanUp != kAuto) { nr = true; r.cleanNote = L"on"; }
    else if (!lowBitrate) { r.cleanNote = L"off, " + rateNote; }
    else {
        const int mNr = chooseFg(baseCost(true)).mult;
        if (mNoNr >= 2 && mNr < 2) r.cleanNote = L"off, " + rateNote + L" but it would cost frame generation";
        else if (baseCost(true) + vsr(level) > r.budgetMs) r.cleanNote = std::format(L"off, {:.1f} ms would not fit the budget", nrPrior);
        else { nr = true; r.cleanNote = L"on, " + rateNote; }
    }
    r.nr = nr;
    Pick fg = chooseFg(baseCost(nr));
    r.multiplier = fg.mult; r.fgMethod = fg.m; r.fgQuality = fg.q; r.stoppedAt = fg.stop;

    // --- the quality upgrade at the same multiplier (Auto quality only)
    if (r.multiplier >= 2 && s.fgQuality == FgQuality::Auto) {
        const Tiers t = TiersOf(r.fgMethod, s.fgQuality);
        if (t.upgrade >= 0 && r.fgQuality == t.base) {
            const double now = baseCost(nr) + vsr(level) + FgCost(c, r.fgMethod, t.base, r.multiplier, pixFg);
            const double up = baseCost(nr) + vsr(level) + FgCost(c, r.fgMethod, t.upgrade, r.multiplier, pixFg);
            if (up <= r.budgetMs - kHighSpareMs) { r.fgQuality = t.upgrade; r.qualityNote = std::format(L"{} ({:.1f} ms of {:.1f}, 3 ms spare)", QualityName(r.fgMethod, t.upgrade), up, r.budgetMs); }
            else r.qualityNote = std::format(L"{} ({} at {}x would take {:.1f} of {:.1f} ms; it needs 3 ms spare)", QualityName(r.fgMethod, t.base),
                                             QualityName(r.fgMethod, t.upgrade), r.multiplier, up, r.budgetMs);
            (void)now;
        } else r.qualityNote = QualityName(r.fgMethod, r.fgQuality);
    } else if (r.multiplier >= 2) r.qualityNote = std::wstring(QualityName(r.fgMethod, r.fgQuality)) + L" (your choice)";

    // --- the rest goes to VSR (Auto): raise 1 -> 4 while it fits; frame generation is never lowered for it
    const double fixedCost = baseCost(nr) + FgCost(c, r.fgMethod, r.fgQuality, r.multiplier, pixFg);
    const wchar_t* mname = method == Upscaler::Rtx ? L"RTX VSR" : method == Upscaler::Fsr ? L"FSR" : method == Upscaler::IntelVsr ? L"Intel VSR" : method == Upscaler::Xess ? L"XeSS SR" : L"NIS";
    if (s.sharper == kAuto) {
        if (!atOutput) {
            if (rtxSr) while (level < 4 && fixedCost + vsr(level + 1) <= r.budgetMs) ++level;
            r.sharperNote = rtxSr ? std::format(L"level {}, to {}x{}", level, upW, upH) : std::format(L"{} to {}x{}", mname, upW, upH);
        } else if (scale > 1.05) {
            r.sharperNote = std::format(L"off, the video already fills the {}x{} screen", d.width, d.height);
        } else if (lowBitrate && rtxSr) {
            srOn = true;
            if (fixedCost + vsr(1) <= r.budgetMs) { level = 1; while (level < 4 && fixedCost + vsr(level + 1) <= r.budgetMs) ++level; r.sharperNote = std::format(L"artifact reduction level {}, {}", level, rateNote); }
            else { srOn = false; level = 0; r.sharperNote = L"off, no GPU time left after frame generation"; }
        } else {
            r.sharperNote = std::format(L"off, the video is already {}x{} ({})", src.w, src.h, rateNote);
        }
    } else if (s.sharper == kOff) r.sharperNote = L"off";
    else r.sharperNote = !srOn ? L"off, artifact reduction needs RTX VSR" : atOutput ? std::format(L"artifact reduction level {}", level)
                       : rtxSr ? std::format(L"level {}, to {}x{}", level, upW, upH) : std::format(L"{} to {}x{}", mname, upW, upH);
    r.sr = srOn; r.srLevel = srOn ? level : 0; r.artifactReduction = srOn && atOutput;
    if (r.artifactReduction) r.srFamily = int(VsrStyle::Standard);
    r.outW = srOn && !atOutput ? upW : src.w; r.outH = srOn && !atOutput ? upH : src.h;
    r.predictedMs = fixedCost + (srOn ? VsrCost(c, level, atOutput, pixOut, r.srFamily, method) : 0);
    if (srOn && !atOutput) r.sharperNote += styleNote;
    if (!methodNote.empty()) r.sharperNote += methodNote;
    r.smootherNote = r.multiplier >= 2 ? std::format(L"{} {}x, {:.1f} fps{}", FgMethodLabel(r.fgMethod), r.multiplier, fps * r.multiplier,
                                                     r.qualityNote.empty() ? std::wstring() : L", " + r.qualityNote)
                                       : (s.smoother == kOff ? L"off" : L"off, " + r.stoppedAt);
    r.fgNote = r.multiplier >= 2 ? std::format(L"{} ({})", FgMethodLabel(r.fgMethod), QualityName(r.fgMethod, r.fgQuality)) : L"off";
    // the cut guard's optical flow (NVIDIA RTX's optical flow engine): with frame generation, when it fits in what is
    // left of the budget; otherwise the guard runs its scene score alone (the fallback)
    if (r.multiplier >= 2) r.predictedMs += c.guardScore * c.card;   // the guard's test always runs with frame generation
    if (r.multiplier >= 2 && g.rtx()) {
        const double flowMs = c.guardFlow * c.card;
        r.guardFlow = r.predictedMs + flowMs <= r.budgetMs;
        if (r.guardFlow) r.predictedMs += flowMs;
        r.guardNote = r.guardFlow ? L"with optical flow" : std::format(L"scene score only (optical flow would take {:.1f} ms more)", flowMs);
    } else r.guardNote = r.multiplier >= 2 ? L"scene score only (no NVIDIA optical flow)" : L"";
    // on battery (OnBattery = Light): frame generation 2x at most, VSR level 2 at most, no clean-up / deband, SDR to HDR
    // boost Low, the cut guard without optical flow
    if (d.battery && s.onBattery == BatteryMode::Light) {
        if (r.multiplier > 2) { r.multiplier = 2; r.smootherNote = std::format(L"{} 2x (on battery)", FgMethodLabel(r.fgMethod)); }
        if (r.sr && r.srLevel > 2) { r.srLevel = 2; r.sharperNote = std::format(L"level 2 (on battery), to {}x{}", r.outW, r.outH); }
        r.stoppedAt = L"on battery: light profile (OnBattery = Same keeps the full one)";
        if (r.nr) { r.nr = false; r.cleanNote = L"off (on battery)"; }
        if (r.deband) { r.deband = false; r.debandNote = L"off (on battery)"; }
        if (r.hdr && (r.hdrBoost == HdrBoost::Medium || r.hdrBoost == HdrBoost::High)) { r.hdrBoost = HdrBoost::Low; r.rtxHdrPeak = 500; }
        if (r.guardFlow) { r.guardFlow = false; r.guardNote = L"scene score only (on battery)"; }
        r.batteryNote = L"on battery: light profile";
    }
    return r;
}

// ------------------------------------------------------------------------------------------------ source frame interval
// The source frame interval from the samples' timestamps (RC1 F8, F9), fed by the streaming thread. The first value:
// the median of the first kFirst steps (LAV delivers them within a few hundred ms), times the playback rate they were
// made at (the splitter divides timestamps by the rate), so `base` is the interval at 1.0x. A rate change only divides
// `base` by the new rate (no estimate converging over seconds). After the first value, only a large (> 10 %) mismatch
// sustained over 3 windows of kWindow steps (~4 s at 24 fps) re-measures it (a file whose frame rate really changes).
// Steps across a discontinuity (seek, flush, new segment: Reset) and outside 1..250 ms are not counted.
struct StepTracker {
    static constexpr size_t kFirst = 8, kWindow = 32;
    double base = 0;                 // the interval at 1.0x in ms, 0 = not measured yet
    long long last = LLONG_MIN;      // the previous timestamp (100 ns)
    long long winFrom = LLONG_MIN;   // the timestamp the first window's steps start at
    std::vector<double> win;
    int mismatch = 0;
    enum Event { None, First, Remeasured };
    void Reset() { last = LLONG_MIN; winFrom = LLONG_MIN; win.clear(); mismatch = 0; }
    static double Median(std::vector<double> v)
    {
        if (v.empty()) return 0;
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        return v[v.size() / 2];
    }
    double Step(double rate) const { return base > 0 ? base / (rate > 0 ? rate : 1.0) : 0; }
    Event Add(long long pts, bool disc, double rate)
    {
        if (rate <= 0) rate = 1.0;
        const long long prev = last;
        last = pts;
        if (disc || prev == LLONG_MIN || pts <= prev) return None;
        const double step = double(pts - prev) / 1e4;
        if (step < 1.0 || step > 250.0) return None;
        if (win.empty() || winFrom == LLONG_MIN) winFrom = prev;
        win.push_back(step);
        if (base <= 0) {
            if (win.size() < kFirst) return None;
            // the median finds the frame interval (a dropped or repeated frame does not move it); when every step is
            // near it, the span over all steps is the finer value (timestamps rounded or jittering by a ms or so)
            const double med = Median(win);
            bool allNear = true;
            for (double v : win) allNear = allNear && v > med * 0.5 && v < med * 1.5;
            base = (allNear ? double(pts - winFrom) / 1e4 / double(win.size()) : med) * rate;
            win.clear(); winFrom = LLONG_MIN;
            return First;
        }
        if (win.size() < kWindow) return None;
        const double med = Median(win), expect = base / rate;
        win.clear(); winFrom = LLONG_MIN;
        if (std::fabs(med - expect) / expect <= 0.10) { mismatch = 0; return None; }
        if (++mismatch < 3) return None;
        mismatch = 0;
        base = med * rate;
        return Remeasured;
    }
};

// ------------------------------------------------------------------------------------------------ bitrate
inline double CodecEfficiency(DWORD fourcc)
{
    auto is = [&](const char* f) { return fourcc == DWORD(f[0] | (f[1] << 8) | (f[2] << 16) | (f[3] << 24)); };
    if (is("HEVC") || is("hevc") || is("HVC1") || is("hvc1") || is("HEV1") || is("hev1") || is("H265") || is("x265")) return 1.6;
    if (is("AV01") || is("av01")) return 1.9;
    if (is("VP90") || is("vp90") || is("VP09") || is("vp09")) return 1.5;
    if (is("MPG2") || is("mpg2") || fourcc == 0xE06D8026 /* MEDIASUBTYPE_MPEG2_VIDEO Data1 */) return 0.6;
    if (is("WVC1") || is("wvc1")) return 0.9;
    return 1.0; // H.264 and anything else
}
inline double BitsPerPixel(double bitsPerSecond, UINT w, UINT h, double fps, DWORD fourcc)
{
    if (bitsPerSecond <= 0 || !w || !h || fps <= 0) return -1;
    return bitsPerSecond * CodecEfficiency(fourcc) / (double(w) * h * fps);
}

// ------------------------------------------------------------------------------------------------ VRR (G-SYNC)
// Whether variable refresh is active for the display MPC-HC's window is on. NVIDIA: NVAPI (System32 nvapi64.dll, no SDK
// header or runtime shipped; function IDs and layouts from NVIDIA's MIT nvapi headers, as PushFPS's NvidiaVrr.h):
//   NvAPI_Disp_GetVRRInfo on the display's ID: bIsVRREnabled / bIsDisplayInVRRMode = VRR is running now;
//   NvAPI_DISP_GetMonitorCapabilities: the monitor supports VRR in the applied mode;
//   the driver profile (DRS) for mpc-hc64.exe, else the global one: two G-SYNC SCOPE values, both 0 off, 1 fullscreen
//   only, 2 fullscreen and windowed (PushFPS docs\NVIDIA_GSYNC_PROFILE_2026-11-14.md; Profile Inspector names):
//     VRR_MODE        0x1194F158 "GSYNC - Application Mode",
//     VRRREQUESTSTATE 0x1094F1F7 "GSYNC - Global Mode".
// The effective scope is the smaller of the two (each missing value = the driver default 1, fullscreen only). VRR
// counts as active when the monitor supports it in its mode and the scope covers the window: scope 2, or scope 1 with
// MPC-HC's window covering its monitor. The driver's "running" flag is display-wide, so it only confirms that
// prediction (it is not a signal of its own). Fullscreen <-> windowed during playback: the Auto thread re-queries the
// display every 2 s (pm::Display::operator== includes vrr), so a toggle re-runs the decision once.
// Windows' own "Variable refresh rate" switch (DirectXUserGlobalSettings VRROptimizeEnable) only lets older DirectX 11
// games use VRR; MPC Video Renderer presents with a flip-model swap chain that does not need it. It is logged.
// AMD (since PushFPS 4.5.3's PushFreeSync.h / PushAmdAdl.h, the same queries): AMD's Display Library (System32
// atiadlxx.dll) gives the display's FreeSync capability and whether FreeSync is switched on in AMD Software. It cannot
// say whether a flip is shown in VRR right now and has no per-program scope, so the NVIDIA rule's driver default is
// used: VRR counts as active when FreeSync is on and usable in the applied mode and MPC-HC's window covers its monitor
// (FreeSync engages for fullscreen flip presentation). Also asked when NVAPI does not know the display (a display on
// an AMD card next to an NVIDIA one). Intel and others: no public state: fixed refresh is assumed, and said so.
struct VrrProbe { bool known = false, active = false, possible = false, caps = false; int mode = -1, request = -1, scope = 1; std::wstring how; };
// AMD FreeSync as ADL answers it (PushFPS PushFreeSync.h pushFreeSyncFrom): capStatus / stateStatus ADL return codes
// (0 = ADL_OK; a display without FreeSync makes the driver refuse the capability query), caps the capability bits
// (ADL_FREESYNC_CAP_DISPLAYSUPPORTED 1<<2, ADL_FREESYNC_CAP_CURRENTMODESUPPORTED 1<<3), useCases the use cases
// FreeSync is switched on for (0 = off)
struct FreeSyncState { bool supported = false, usableInMode = false, switchedOn = false; };
inline FreeSyncState FreeSyncFrom(int capStatus, int caps, int stateStatus, int useCases)
{
    FreeSyncState s;
    if (capStatus != 0) return s;
    s.supported = (caps & (1 << 2)) != 0;
    s.usableInMode = (caps & (1 << 3)) != 0;
    s.switchedOn = s.supported && s.usableInMode && stateStatus == 0 && useCases != 0;
    return s;
}
// the AMD decision (separate so the headless check can run it): the NVIDIA rule with the driver default scope
inline void VrrDecideAmd(VrrProbe& p, const FreeSyncState& f, bool fullscreen)
{
    p.known = true;
    p.caps = f.supported && f.usableInMode;
    p.possible = f.switchedOn;
    p.scope = f.switchedOn ? 1 : 0;   // the NVIDIA driver default, fullscreen only
    p.active = f.switchedOn && fullscreen;
    if (p.active) p.how = L"FreeSync for this window (AMD driver: on, player fullscreen)";
    else if (!f.supported) p.how = L"the monitor has no FreeSync (AMD driver)";
    else if (!f.usableInMode) p.how = L"FreeSync is not available in this display mode (AMD driver)";
    else if (!f.switchedOn) p.how = L"FreeSync is off in AMD Software";
    else p.how = L"FreeSync engages in fullscreen and the player is windowed (AMD driver)";
}
inline constexpr int kNvVrrScopeDefault = 1;   // the driver default of both values: fullscreen only
// the effective G-SYNC scope from the profile's two values (-1 = not set anywhere: driver default)
inline int VrrScope(int mode, int request)
{
    const int m = mode < 0 ? kNvVrrScopeDefault : std::clamp(mode, 0, 2), r = request < 0 ? kNvVrrScopeDefault : std::clamp(request, 0, 2);
    return std::min(m, r);
}
// the decision itself (separate so the headless check can run it): capable = the monitor can do VRR in its mode
inline void VrrDecide(VrrProbe& p, bool capable, bool running, bool fullscreen)
{
    p.scope = VrrScope(p.mode, p.request);
    const bool covers = p.scope == 2 || (p.scope == 1 && fullscreen);
    const wchar_t* other = running ? L"; VRR running for another program" : L"";
    const std::wstring values = std::format(L"NVIDIA profile: application mode {}, global mode {}", p.mode < 0 ? std::wstring(L"default") : std::to_wstring(p.mode),
                                            p.request < 0 ? std::wstring(L"default") : std::to_wstring(p.request));
    p.active = capable && covers;
    if (p.active)
        p.how = std::format(L"G-SYNC for this window ({}{}){}", p.scope == 2 ? L"fullscreen and windowed" : L"fullscreen only", p.scope == 1 ? L", player fullscreen" : L"",
                            running ? L", confirmed running by the driver" : L"");
    else if (!capable) p.how = L"the monitor or its mode has no VRR (NVIDIA driver)";
    else if (p.scope == 0) p.how = std::format(L"G-SYNC is off for this program ({}){}", values, other);
    else p.how = std::format(L"G-SYNC is fullscreen-only and the player is windowed ({}){}", values, other);
}
namespace detail {
using NvQuery = void*(__cdecl*)(uint32_t);
inline NvQuery NvApi()
{
    static NvQuery q = [] () -> NvQuery {
        wchar_t sys[MAX_PATH]{}; GetSystemDirectoryW(sys, MAX_PATH);
        HMODULE m = LoadLibraryExW((std::wstring(sys) + L"\\nvapi64.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!m) return nullptr;
        HMODULE pinned = nullptr; GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(m), &pinned);
        auto qi = reinterpret_cast<NvQuery>(GetProcAddress(m, "nvapi_QueryInterface"));
        if (!qi) return nullptr;
        auto init = reinterpret_cast<int(__cdecl*)()>(qi(0x0150E828));
        return init && init() == 0 ? qi : nullptr;
    }();
    return q;
}
#pragma pack(push, 4)
union DrsValue { uint32_t dword; struct { uint32_t length; unsigned char bytes[4096]; } binary; wchar_t text[2048]; uint64_t qword; };
struct DrsSetting { uint32_t version; wchar_t name[2048]; uint32_t id, type, location, predefined, predefinedValid; DrsValue defaultValue, current; };
struct DrsApplication { uint32_t version, predefined; wchar_t name[2048], friendly[2048], launcher[2048]; };
#pragma pack(pop)
static_assert(sizeof(DrsSetting) == 12320 && sizeof(DrsApplication) == 12296, "NVAPI DRS layouts");
// the DWORD value of a driver setting for this program's profile, else the global profile; -1 = not found / no NVAPI
inline void DrsRead(NvQuery q, const std::wstring& exe, int& mode, int& request)
{
    mode = request = -1;
    using Fn0 = int(__cdecl*)(void**);
    using Fn1 = int(__cdecl*)(void*);
    using FnProf = int(__cdecl*)(void*, void**);
    using FnFind = int(__cdecl*)(void*, wchar_t*, void**, DrsApplication*);
    using FnGet = int(__cdecl*)(void*, void*, uint32_t, DrsSetting*);
    auto create = reinterpret_cast<Fn0>(q(0x0694D52E)); auto load = reinterpret_cast<Fn1>(q(0x375DBD6B));
    auto global = reinterpret_cast<FnProf>(q(0x617BFF9F)); auto find = reinterpret_cast<FnFind>(q(0xEEE566B2));
    auto get = reinterpret_cast<FnGet>(q(0x73BF8338)); auto destroy = reinterpret_cast<Fn1>(q(0xDAD9CFF8));
    if (!create || !load || !global || !find || !get || !destroy) return;
    void* session = nullptr;
    if (create(&session) != 0 || !session) return;
    if (load(session) == 0) {
        void* app = nullptr; void* glob = nullptr;
        auto* a = new DrsApplication{}; a->version = uint32_t(sizeof(DrsApplication)) | (1u << 16);
        std::wstring name = exe; name.resize(std::min<size_t>(name.size(), 2047));
        std::vector<wchar_t> buf(name.begin(), name.end()); buf.push_back(0);
        if (find(session, buf.data(), &app, a) != 0) app = nullptr;
        global(session, &glob);
        auto* st = new DrsSetting{};
        auto read = [&](uint32_t id) -> int {
            for (void* prof : { app, glob }) {
                if (!prof) continue;
                *st = {}; st->version = uint32_t(sizeof(DrsSetting)) | (1u << 16);
                if (get(session, prof, id, st) == 0 && st->type == 0) return int(st->current.dword);
            }
            return -1;
        };
        mode = read(0x1194F158); request = read(0x1094F1F7);
        delete st; delete a;
    }
    destroy(session);
}
// AMD Display Library, as PushFPS's PushAmdAdl.h: structure layouts and entry points from AMD's public ADL interface
// (GPUOpen display-library adl_structures.h / adl_defines.h, MIT; no SDK file shipped). System32 copy only, pinned,
// one context; read only. PUSHMPC_NO_AMD_ADL=1 (or PushFPS's VEYRA_NO_AMD_ADL=1) skips it.
namespace adl {
struct Adapter {
    int size, index; char udid[256]; int bus, device, function, vendor;
    char adapterName[256], displayName[256]; int present, exist;
    char driverPath[256], driverPathExt[256], pnp[256]; int osDisplayIndex;
};
struct Display {
    int logical, physical, logicalAdapter, physicalAdapter, controller;
    char name[256], manufacturer[256]; int type, outputType, connector, mask, value;
};
struct Cap { int caps, minMicroHz, maxMicroHz; unsigned char label; char reserved[3]; int reservedEx[4]; };
static_assert(sizeof(Adapter) == 1572 && sizeof(Display) == 552 && sizeof(Cap) == 32, "ADL layouts");
struct Library {
    using Alloc = void*(__stdcall*)(int);
    using Create = int(__cdecl*)(Alloc, int, void**);
    using Count = int(__cdecl*)(void*, int*);
    using Adapters = int(__cdecl*)(void*, Adapter*, int);
    using Displays = int(__cdecl*)(void*, int, int*, Display**, int);
    using GetCap = int(__cdecl*)(void*, int, int, Cap*);
    using GetState = int(__cdecl*)(void*, int, int, int*, int*, int*, int*);
    void* context = nullptr; Count count = nullptr; Adapters adapters = nullptr; Displays displays = nullptr;
    GetCap getCap = nullptr; GetState getState = nullptr;
    bool ready() const { return context && count && adapters && displays && getCap && getState; }
};
inline void* __stdcall Allocate(int size) { return size > 0 ? std::malloc(size_t(size)) : nullptr; }
inline const Library& Lib()
{
    static const Library lib = [] {
        Library l;
        wchar_t skip[4]{};
        if ((GetEnvironmentVariableW(L"PUSHMPC_NO_AMD_ADL", skip, 4) && skip[0] == L'1') || (GetEnvironmentVariableW(L"VEYRA_NO_AMD_ADL", skip, 4) && skip[0] == L'1')) return l;
        wchar_t sys[MAX_PATH]{};
        const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return l;
        HMODULE m = LoadLibraryExW((std::wstring(sys) + L"\\atiadlxx.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!m) return l;
        HMODULE pinned = nullptr; GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(m), &pinned);
        auto create = reinterpret_cast<Library::Create>(GetProcAddress(m, "ADL2_Main_Control_Create"));
        Library t;
        t.count = reinterpret_cast<Library::Count>(GetProcAddress(m, "ADL2_Adapter_NumberOfAdapters_Get"));
        t.adapters = reinterpret_cast<Library::Adapters>(GetProcAddress(m, "ADL2_Adapter_AdapterInfo_Get"));
        t.displays = reinterpret_cast<Library::Displays>(GetProcAddress(m, "ADL2_Display_DisplayInfo_Get"));
        t.getCap = reinterpret_cast<Library::GetCap>(GetProcAddress(m, "ADL2_Display_FreeSync_Cap"));
        t.getState = reinterpret_cast<Library::GetState>(GetProcAddress(m, "ADL2_Display_FreeSyncState_Get"));
        if (!create || !t.count || !t.adapters || !t.displays || !t.getCap || !t.getState) return l;
        void* context = nullptr;
        if (create(&Allocate, 1, &context) != 0 || !context) return l;   // 1 = connected adapters only
        t.context = context;
        return t;
    }();
    return lib;
}
// FreeSync of the display with this GDI name (MONITORINFOEXW::szDevice); false = the AMD driver does not know it
inline bool QueryFreeSync(const wchar_t* gdiDevice, FreeSyncState& out)
{
    const Library& lib = Lib();
    if (!lib.ready() || !gdiDevice || !gdiDevice[0]) return false;
    char device[CCHDEVICENAME * 2]{};
    if (!WideCharToMultiByte(CP_ACP, 0, gdiDevice, -1, device, int(sizeof(device)), nullptr, nullptr)) return false;
    int count = 0;
    if (lib.count(lib.context, &count) != 0 || count <= 0 || count > 64) return false;
    std::vector<Adapter> list(static_cast<size_t>(count));
    for (auto& a : list) { std::memset(&a, 0, sizeof(a)); a.size = int(sizeof(Adapter)); }
    if (lib.adapters(lib.context, list.data(), int(sizeof(Adapter)) * count) != 0) return false;
    for (auto& a : list) {
        a.displayName[sizeof(a.displayName) - 1] = 0;
        if (std::strcmp(a.displayName, device) != 0) continue;
        int n = 0; Display* info = nullptr; int display = -1;
        if (lib.displays(lib.context, a.index, &n, &info, 0) == 0 && info && n > 0 && n <= 64)
            for (int i = 0; i < n; ++i)
                if (info[i].logicalAdapter == a.index && (info[i].value & 0x3) == 0x3) { display = info[i].logical; break; }   // connected | mapped
        std::free(info);
        if (display < 0) continue;
        Cap cap{}; int useCases = 0, defaults = 0, minHz = 0, maxHz = 0;
        const int capStatus = lib.getCap(lib.context, a.index, display, &cap);
        const int stateStatus = capStatus == 0 ? lib.getState(lib.context, a.index, display, &useCases, &defaults, &minHz, &maxHz) : capStatus;
        out = FreeSyncFrom(capStatus, cap.caps, stateStatus, useCases);
        return true;
    }
    return false;
}
}  // namespace adl
}  // namespace detail
inline bool WindowFullscreen(HWND wnd, const MONITORINFOEXW& mi)
{
    RECT wr{};
    return wnd && GetWindowRect(wnd, &wr) && wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top && wr.right >= mi.rcMonitor.right && wr.bottom >= mi.rcMonitor.bottom;
}
// AMD FreeSync for the window's display; false = the AMD driver does not know it
inline bool ProbeFreeSync(HWND wnd, const MONITORINFOEXW& mi, VrrProbe& p)
{
    FreeSyncState f;
    if (!detail::adl::QueryFreeSync(mi.szDevice, f)) return false;
    VrrDecideAmd(p, f, WindowFullscreen(wnd, mi));
    return true;
}
inline VrrProbe ProbeVrr(HWND wnd, const MONITORINFOEXW& mi, bool nvidia)
{
    VrrProbe p;
    if (!nvidia) {
        if (ProbeFreeSync(wnd, mi, p)) return p;
        p.how = L"not detected on this GPU (no NVIDIA or AMD driver answered for this display): fixed refresh assumed"; return p;
    }
    auto q = detail::NvApi();
    if (!q) { if (ProbeFreeSync(wnd, mi, p)) return p; p.how = L"NVAPI unavailable: fixed refresh assumed"; return p; }
    auto idByName = reinterpret_cast<int(__cdecl*)(const char*, uint32_t*)>(q(0xAE457190));
    struct Info { uint32_t version, flags, reserved[4]; };
    auto getVrr = reinterpret_cast<int(__cdecl*)(uint32_t, Info*)>(q(0xDF8FDA57));
    struct Caps { uint32_t version; uint16_t size; uint32_t infoType, connectorType; uint8_t valid; uint8_t data[49]; };
    static_assert(sizeof(Caps) == 68, "NV_MONITOR_CAPABILITIES_V1");
    auto getCaps = reinterpret_cast<int(__cdecl*)(uint32_t, Caps*)>(q(0x3B05C7E1));
    char name[CCHDEVICENAME * 2]{};
    WideCharToMultiByte(CP_ACP, 0, mi.szDevice, -1, name, int(sizeof(name)), nullptr, nullptr);
    uint32_t id = 0;
    if (!idByName || !getVrr || idByName(name, &id) != 0) { if (ProbeFreeSync(wnd, mi, p)) return p; p.how = L"NVAPI does not know this display: fixed refresh assumed"; return p; }
    Info info{}; info.version = uint32_t(sizeof(Info)) | (1u << 16);
    if (getVrr(id, &info) != 0) { p.how = L"NVAPI VRR query failed: fixed refresh assumed"; return p; }
    p.known = true;
    const bool running = (info.flags & 1u) || (info.flags & 16u);
    p.possible = (info.flags & 2u) || (info.flags & 4u);
    if (getCaps) { Caps c{}; c.version = uint32_t(sizeof(Caps)) | (1u << 16); c.size = uint16_t(sizeof(Caps)); c.infoType = 0x1002;
                   if (getCaps(id, &c) == 0 && (c.valid & 1u)) p.caps = (c.data[0] & 1u) && (c.data[0] & 16u); }
    wchar_t exe[MAX_PATH]{}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    detail::DrsRead(q, exe, p.mode, p.request);
    RECT wr{}; bool fullscreen = false;
    if (wnd && GetWindowRect(wnd, &wr)) fullscreen = wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top && wr.right >= mi.rcMonitor.right && wr.bottom >= mi.rcMonitor.bottom;
    // the prediction for OUR window (the monitor can do VRR in its mode, and the profile's scope covers the window). The
    // driver's "VRR running" flag is display-wide (any other program presenting in VRR mode sets it), so it only confirms
    // a positive prediction, never makes one.
    VrrDecide(p, p.possible || p.caps || running, running, fullscreen);
    return p;
}

// ------------------------------------------------------------------------------------------------ the host player
// Round 2 (MPC-BE): the player hosting this filter, from the process's exe name.
enum class Host { MpcHc, MpcBe, PotPlayer, Other };
inline Host HostFromExe(const std::wstring& exe)
{
    std::wstring n = exe;
    if (const size_t s = n.find_last_of(L"\\/"); s != std::wstring::npos) n.erase(0, s + 1);
    for (auto& ch : n) ch = wchar_t(towlower(ch));
    if (n.rfind(L"mpc-hc", 0) == 0) return Host::MpcHc;
    if (n.rfind(L"mpc-be", 0) == 0) return Host::MpcBe;
    if (n.rfind(L"potplayer", 0) == 0) return Host::PotPlayer;
    return Host::Other;
}
inline Host HostPlayer()
{
    static const Host h = [] { wchar_t p[MAX_PATH] = {}; GetModuleFileNameW(nullptr, p, MAX_PATH); return HostFromExe(p); }();
    return h;
}
inline const wchar_t* HostName(Host h)
{
    switch (h) { case Host::MpcHc: return L"MPC-HC"; case Host::MpcBe: return L"MPC-BE"; case Host::PotPlayer: return L"PotPlayer"; default: return L"the player"; }
}
inline const wchar_t* HostName() { return HostName(HostPlayer()); }
// the main window class: MPC-HC MediaPlayerClassicW, MPC-BE "MPC-BE" (mpc_defines.h MPC_WND_CLASS_NAMEW),
// PotPlayer x64 PotPlayer64 (unverified); another host: none (the display is then the primary monitor)
inline const wchar_t* HostWindowClass(Host h)
{
    switch (h) { case Host::MpcHc: return L"MediaPlayerClassicW"; case Host::MpcBe: return L"MPC-BE"; case Host::PotPlayer: return L"PotPlayer64"; default: return nullptr; }
}
// WM_COMMAND ID_FILE_REOPEN: 976 in MPC-HC and MPC-BE (resource.h of both); PotPlayer has no known reopen command
inline bool HostCanReopen(Host h) { return h == Host::MpcHc || h == Host::MpcBe; }

// ------------------------------------------------------------------------------------------------ the window's display
inline HWND PlayerWindow()
{
    const wchar_t* want = HostWindowClass(HostPlayer());
    if (!want) return nullptr;
    struct Ctx { HWND found; DWORD pid; const wchar_t* cls; } ctx = { nullptr, GetCurrentProcessId(), want };
    EnumWindows([](HWND h, LPARAM l) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(l);
        DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
        wchar_t cls[64] = {};
        if (pid == c->pid && GetClassNameW(h, cls, 64) && wcscmp(cls, c->cls) == 0) { c->found = h; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}
// vrrMode: the user's override (Auto = detect); nvidia: the GPU is NVIDIA (VRR detection through NVAPI)
inline Display QueryDisplay(HWND wnd, VrrMode vrrMode = VrrMode::Auto, bool nvidia = true)
{
    Display d;
    HMONITOR mon = wnd ? MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST) : MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi = {}; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return d;
    DEVMODEW dm = {}; dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
        d.width = dm.dmPelsWidth; d.height = dm.dmPelsHeight;
        if (dm.dmDisplayFrequency > 1) d.refreshHz = dm.dmDisplayFrequency;
    }
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) == ERROR_SUCCESS && np) {
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) == ERROR_SUCCESS) {
            for (UINT32 i = 0; i < np; ++i) {
                const auto& p = paths[i];
                DISPLAYCONFIG_SOURCE_DEVICE_NAME sn = {};
                sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME; sn.header.size = sizeof(sn);
                sn.header.adapterId = p.sourceInfo.adapterId; sn.header.id = p.sourceInfo.id;
                if (DisplayConfigGetDeviceInfo(&sn.header) != ERROR_SUCCESS || wcscmp(sn.viewGdiDeviceName, mi.szDevice) != 0) continue;
                if (p.targetInfo.refreshRate.Denominator) d.refreshHz = double(p.targetInfo.refreshRate.Numerator) / p.targetInfo.refreshRate.Denominator;
                DISPLAYCONFIG_TARGET_DEVICE_NAME tn = {};
                tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME; tn.header.size = sizeof(tn);
                tn.header.adapterId = p.targetInfo.adapterId; tn.header.id = p.targetInfo.id;
                if (DisplayConfigGetDeviceInfo(&tn.header) == ERROR_SUCCESS && tn.monitorFriendlyDeviceName[0]) d.name = tn.monitorFriendlyDeviceName;
                DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO ac = {};
                ac.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO; ac.header.size = sizeof(ac);
                ac.header.adapterId = p.targetInfo.adapterId; ac.header.id = p.targetInfo.id;
                if (DisplayConfigGetDeviceInfo(&ac.header) == ERROR_SUCCESS) d.hdr = ac.advancedColorEnabled && !ac.wideColorEnforced;
                break;
            }
        }
    }
    if (vrrMode == VrrMode::On) { d.vrr = true; d.vrrKnown = true; d.vrrHow = L"assumed (your setting)"; }
    else if (vrrMode == VrrMode::Off) { d.vrr = false; d.vrrKnown = true; d.vrrHow = L"fixed refresh assumed (your setting)"; }
    else { const VrrProbe p = ProbeVrr(wnd, mi, nvidia); d.vrr = p.active; d.vrrKnown = p.known; d.vrrHow = p.how; d.vrrCapable = p.possible || p.caps; }
    SYSTEM_POWER_STATUS ps{}; d.battery = GetSystemPowerStatus(&ps) && ps.ACLineStatus == 0;
    return d;
}

} // namespace pm
