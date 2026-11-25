// pushmpc.Enhance, backend "apple": the parts of the VideoToolbox backend that need no Apple API, so apple_test checks
// them on every OS (CI: Windows, Linux, macOS):
//   - planar YUV 4:2:0 (VapourSynth: Y, U, V planes; 8 bits, or 9..16 bits in the low bits of 16-bit words) to and from
//     the bi-planar layouts VideoToolbox takes (Y plane + interleaved CbCr: '420v' / '420f' 8-bit, 'x420' / 'xf20' 10-bit
//     with the sample in the high bits of a 16-bit word). The round trip is lossless whenever the bi-planar depth is at
//     least the source depth; values are never range-converted (limited stays limited, PQ / HLG code values stay as
//     they are: HDR passes through);
//   - the scene-cut guard: VideoToolbox's interpolators have none, so a cut pair would blend two scenes. A 64x36 luma
//     thumbnail per frame; a pair is a cut when the picture changed a lot AND its brightness histogram changed (a fast
//     pan changes the picture but keeps the histogram). Generated frames of a cut pair are the nearer real frame, as in
//     PushFPS;
//   - the phase plan: VTLowLatencyFrameInterpolation configured for numberOfInterpolatedFrames = x offers the phases
//     m / 2^x (x = 1: 0.5; x = 2: 0.25, 0.5, 0.75; ...). k = 2, 4, 8 hit the grid exactly; other k use the nearest grid
//     phase (at most 1 / 2^(x+1) of a source frame off, x chosen one finer than needed). Frame-rate conversion (FRC)
//     takes any phase, so its plan is exact;
//   - Auto's multiplier and super-resolution decisions, and the step-down ladder that keeps playback real time on a
//     slow Mac (drop super resolution to a plain scale, then fewer interpolated phases, then no interpolation).
#pragma once
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace pmvs::apple {

// ------------------------------------------------------------------------------------------------ pictures
// a planar YUV 4:2:0 picture as VapourSynth holds it (bits 8: 1 byte per sample; 9..16: 2 bytes, low-bit aligned)
struct PlanarView { const uint8_t* data[3] = {}; ptrdiff_t stride[3] = {}; int width = 0, height = 0, bits = 8; };
struct PlanarOut { uint8_t* data[3] = {}; ptrdiff_t stride[3] = {}; };
// a bi-planar 4:2:0 picture (a locked CVPixelBuffer): plane 0 Y, plane 1 CbCr interleaved. bytes 1: 8-bit samples;
// bytes 2: 16-bit words with the sample in the high bits ('x420' / 'xf20': 10 significant bits)
struct BiView { uint8_t* data[2] = {}; ptrdiff_t stride[2] = {}; int width = 0, height = 0, bytes = 1; };

inline int ChromaW(int w) { return (w + 1) / 2; }
inline int ChromaH(int h) { return (h + 1) / 2; }

namespace detail {
inline uint16_t Rd16(const uint8_t* row, int x) { uint16_t v; std::memcpy(&v, row + 2 * x, 2); return v; }
inline void Wr16(uint8_t* row, int x, uint16_t v) { std::memcpy(row + 2 * x, &v, 2); }
// a source sample (srcBits) as a destination word: 8-bit (bytes 1) or a high-aligned 16-bit word (bytes 2)
inline uint32_t ToBi(uint32_t v, int srcBits, int bytes)
{
    if (bytes == 2) return srcBits >= 16 ? v : v << (16 - srcBits);
    if (srcBits <= 8) return v;
    const int sh = srcBits - 8;
    return std::min<uint32_t>(255u, (v + (1u << (sh - 1))) >> sh);
}
inline uint32_t FromBi(uint32_t v, int dstBits, int bytes)
{
    if (bytes == 1) return dstBits <= 8 ? v : v << (dstBits - 8);
    if (dstBits >= 16) return v;
    const int sh = 16 - dstBits;
    const uint32_t maxV = (1u << dstBits) - 1;
    return std::min<uint32_t>(maxV, (v + (1u << (sh - 1))) >> sh);
}
} // namespace detail

// planar (VapourSynth) -> bi-planar (VideoToolbox). The bi-planar picture has the planar one's size.
inline void PlanarToBi(const PlanarView& in, const BiView& out)
{
    using namespace detail;
    const int w = in.width, h = in.height, cw = ChromaW(w), ch = ChromaH(h), sb = in.bits, ob = out.bytes;
    for (int y = 0; y < h; ++y) {
        const uint8_t* s = in.data[0] + y * in.stride[0];
        uint8_t* d = out.data[0] + y * out.stride[0];
        if (sb <= 8 && ob == 1) { std::memcpy(d, s, size_t(w)); continue; }
        for (int x = 0; x < w; ++x) {
            const uint32_t v = ToBi(sb <= 8 ? s[x] : Rd16(s, x), sb, ob);
            if (ob == 1) d[x] = uint8_t(v); else Wr16(d, x, uint16_t(v));
        }
    }
    for (int y = 0; y < ch; ++y) {
        const uint8_t* su = in.data[1] + y * in.stride[1];
        const uint8_t* sv = in.data[2] + y * in.stride[2];
        uint8_t* d = out.data[1] + y * out.stride[1];
        for (int x = 0; x < cw; ++x) {
            const uint32_t u = ToBi(sb <= 8 ? su[x] : Rd16(su, x), sb, ob), v = ToBi(sb <= 8 ? sv[x] : Rd16(sv, x), sb, ob);
            if (ob == 1) { d[2 * x] = uint8_t(u); d[2 * x + 1] = uint8_t(v); }
            else { Wr16(d, 2 * x, uint16_t(u)); Wr16(d, 2 * x + 1, uint16_t(v)); }
        }
    }
}

// bi-planar (VideoToolbox) -> planar (VapourSynth) at dstBits, the size of the bi-planar picture
inline void BiToPlanar(const BiView& in, const PlanarOut& out, int dstBits)
{
    using namespace detail;
    const int w = in.width, h = in.height, cw = ChromaW(w), ch = ChromaH(h), ib = in.bytes;
    for (int y = 0; y < h; ++y) {
        const uint8_t* s = in.data[0] + y * in.stride[0];
        uint8_t* d = out.data[0] + y * out.stride[0];
        if (dstBits <= 8 && ib == 1) { std::memcpy(d, s, size_t(w)); continue; }
        for (int x = 0; x < w; ++x) {
            const uint32_t v = FromBi(ib == 1 ? s[x] : Rd16(s, x), dstBits, ib);
            if (dstBits <= 8) d[x] = uint8_t(v); else Wr16(d, x, uint16_t(v));
        }
    }
    for (int y = 0; y < ch; ++y) {
        const uint8_t* s = in.data[1] + y * in.stride[1];
        uint8_t* du = out.data[1] + y * out.stride[1];
        uint8_t* dv = out.data[2] + y * out.stride[2];
        for (int x = 0; x < cw; ++x) {
            const uint32_t u = FromBi(ib == 1 ? s[2 * x] : Rd16(s, 2 * x), dstBits, ib);
            const uint32_t v = FromBi(ib == 1 ? s[2 * x + 1] : Rd16(s, 2 * x + 1), dstBits, ib);
            if (dstBits <= 8) { du[x] = uint8_t(u); dv[x] = uint8_t(v); }
            else { Wr16(du, x, uint16_t(u)); Wr16(dv, x, uint16_t(v)); }
        }
    }
}

// planar -> planar copy (real frames without super resolution stay bit-exact)
inline void CopyPlanar(const PlanarView& in, const PlanarOut& out)
{
    const int bps = in.bits > 8 ? 2 : 1;
    for (int p = 0; p < 3; ++p) {
        const int w = (p ? ChromaW(in.width) : in.width) * bps, h = p ? ChromaH(in.height) : in.height;
        for (int y = 0; y < h; ++y) std::memcpy(out.data[p] + y * out.stride[p], in.data[p] + y * in.stride[p], size_t(w));
    }
}

// ------------------------------------------------------------------------------------------------ pixel formats
// FourCCs (CoreVideo's kCVPixelFormatType_* values, spelled out so this header needs no Apple header)
constexpr uint32_t FourCC(char a, char b, char c, char d) { return uint32_t(uint8_t(a)) << 24 | uint32_t(uint8_t(b)) << 16 | uint32_t(uint8_t(c)) << 8 | uint32_t(uint8_t(d)); }
constexpr uint32_t k420v = FourCC('4', '2', '0', 'v');   // 8-bit 4:2:0 bi-planar, video (limited) range
constexpr uint32_t k420f = FourCC('4', '2', '0', 'f');   // 8-bit, full range
constexpr uint32_t kx420 = FourCC('x', '4', '2', '0');   // 10-bit (high bits of 16), video range
constexpr uint32_t kxf20 = FourCC('x', 'f', '2', '0');   // 10-bit, full range
constexpr uint32_t kRGhA = FourCC('R', 'G', 'h', 'A');   // 64-bit RGBA half float (what frame-rate conversion lists)
inline std::string FourCCText(uint32_t f)
{
    std::string s;
    for (int i = 3; i >= 0; --i) { const char c = char((f >> (8 * i)) & 0xff); s += (c >= 32 && c < 127) ? c : '?'; }
    return s;
}
inline int FormatBytes(uint32_t f) { return f == kx420 || f == kxf20 ? 2 : 1; }
inline int FormatBits(uint32_t f) { return f == kx420 || f == kxf20 ? 10 : 8; }
// the bi-planar format for a source: lossless first (same or deeper bits, the source's range tag), then the other
// range tag (the values pass unchanged either way: only the tag differs), then a lossy one. 0: none of ours offered.
inline uint32_t PickFormat(const std::vector<uint32_t>& supported, int srcBits, bool fullRange, bool* lossy = nullptr)
{
    const uint32_t v8 = fullRange ? k420f : k420v, o8 = fullRange ? k420v : k420f;
    const uint32_t v10 = fullRange ? kxf20 : kx420, o10 = fullRange ? kx420 : kxf20;
    std::vector<uint32_t> pref = srcBits <= 8 ? std::vector<uint32_t>{ v8, o8, v10, o10 } : std::vector<uint32_t>{ v10, o10, v8, o8 };
    for (uint32_t f : pref)
        if (std::find(supported.begin(), supported.end(), f) != supported.end()) {
            if (lossy) *lossy = FormatBits(f) < std::min(srcBits, 10);
            return f;
        }
    if (lossy) *lossy = false;
    return 0;
}

// ------------------------------------------------------------------------------------------------ scene cuts
struct Thumb { static constexpr int W = 64, H = 36; std::array<uint8_t, W * H> px{}; std::array<uint16_t, 32> hist{}; bool valid = false; };
// box-averaged luma of the picture, scaled to 8 bits
inline Thumb MakeThumb(const PlanarView& v)
{
    Thumb t;
    if (v.width <= 0 || v.height <= 0) return t;
    const int bps = v.bits > 8 ? 2 : 1, sh = std::max(0, v.bits - 8);
    for (int ty = 0; ty < Thumb::H; ++ty) {
        const int y0 = ty * v.height / Thumb::H, y1 = std::max(y0 + 1, (ty + 1) * v.height / Thumb::H);
        for (int tx = 0; tx < Thumb::W; ++tx) {
            const int x0 = tx * v.width / Thumb::W, x1 = std::max(x0 + 1, (tx + 1) * v.width / Thumb::W);
            // sample a sparse grid inside the box (at most 8x8 samples): enough for a cut test, cheap at 4K
            const int sy = std::max(1, (y1 - y0) / 8), sx = std::max(1, (x1 - x0) / 8);
            uint64_t sum = 0, n = 0;
            for (int y = y0; y < y1; y += sy) {
                const uint8_t* row = v.data[0] + y * v.stride[0];
                for (int x = x0; x < x1; x += sx) { sum += bps == 1 ? row[x] : (uint32_t(detail::Rd16(row, x)) >> sh); ++n; }
            }
            const uint8_t m = uint8_t(std::min<uint64_t>(255, n ? sum / n : 0));
            t.px[size_t(ty * Thumb::W + tx)] = m;
            ++t.hist[m >> 3];
        }
    }
    t.valid = true;
    return t;
}
struct CutScore { double mad = 0, hist = 0; };   // mad: mean |a - b| (0..255); hist: half the L1 distance (0..1)
inline CutScore ScoreCut(const Thumb& a, const Thumb& b)
{
    CutScore s;
    uint64_t d = 0;
    for (size_t i = 0; i < a.px.size(); ++i) d += uint64_t(std::abs(int(a.px[i]) - int(b.px[i])));
    s.mad = double(d) / double(a.px.size());
    uint32_t hd = 0;
    for (size_t i = 0; i < a.hist.size(); ++i) hd += uint32_t(std::abs(int(a.hist[i]) - int(b.hist[i])));
    s.hist = double(hd) / (2.0 * double(a.px.size()));
    return s;
}
inline constexpr double kCutMad = 22.0, kCutHist = 0.28, kCutMadAlone = 64.0;
inline bool IsCut(const CutScore& s) { return (s.mad >= kCutMad && s.hist >= kCutHist) || s.mad >= kCutMadAlone; }

// ------------------------------------------------------------------------------------------------ phase plan
// For output slot j = 1 .. k-1 of a pair: where it comes from. src >= 0: VideoToolbox output src (phases[src]);
// kPrev / kCur: the real frame itself (a phase that rounds to 0 or 1, or interpolation off)
inline constexpr int kPrev = -1, kCur = -2;
struct PhasePlan { std::vector<float> phases; std::vector<int> slot; double maxError = 0; int exponent = 0; };
// exponent x: the grid m / 2^x (0: no interpolation, every slot is the nearer real frame); exact: any phase (FRC)
inline PhasePlan MakePlan(int k, int exponent, bool exact = false)
{
    PhasePlan p;
    p.exponent = exponent;
    if (k < 2) return p;
    p.slot.assign(size_t(k - 1), kPrev);
    for (int j = 1; j < k; ++j) {
        const double t = double(j) / k;
        double q;
        if (exact) q = t;
        else if (exponent <= 0) q = t <= 0.5 ? 0.0 : 1.0;
        else { const double g = double(1 << exponent); q = std::round(t * g) / g; }
        p.maxError = std::max(p.maxError, std::fabs(q - t));
        if (q <= 0.0) { p.slot[size_t(j - 1)] = kPrev; continue; }
        if (q >= 1.0) { p.slot[size_t(j - 1)] = kCur; continue; }
        const float qf = float(q);
        auto it = std::find(p.phases.begin(), p.phases.end(), qf);
        if (it == p.phases.end()) { p.phases.push_back(qf); p.slot[size_t(j - 1)] = int(p.phases.size() - 1); }
        else p.slot[size_t(j - 1)] = int(it - p.phases.begin());
    }
    return p;
}
// the grid exponent for k: exact for powers of two, else one finer than needed; at most maxExp
inline int ExponentFor(int k, int maxExp = 4)
{
    if (k < 2) return 0;
    int x = 0;
    while ((1 << x) < k) ++x;
    if ((1 << x) != k) ++x;
    return std::clamp(x, 1, maxExp);
}

// ------------------------------------------------------------------------------------------------ Auto decisions
struct AppleK { int k = 1; std::string why; };
// smoother: 0 Auto, 1 off, 2..8 fixed; displayHz <= 0 unknown (Auto then aims at x2)
inline AppleK DecideAppleK(int smoother, double srcFps, double displayHz)
{
    AppleK r;
    char b[200];
    if (srcFps <= 0) srcFps = 23.976;
    if (smoother == 1) { r.k = 1; r.why = "Smoother off: no frame generation"; return r; }
    if (smoother >= 2) { r.k = std::min(smoother, 8); snprintf(b, sizeof(b), "x%d (your setting)", r.k); r.why = b; return r; }
    if (displayHz <= 0) { r.k = 2; r.why = "Auto: display rate unknown: x2"; return r; }
    r.k = std::clamp(int(std::floor(displayHz / srcFps + 0.02)), 1, 8);
    snprintf(b, sizeof(b), "Auto: display %.2f Hz / source %.3f fps: x%d", displayHz, srcFps, r.k);
    r.why = b;
    return r;
}
// super resolution: mode "auto" | "off" | "2" | "4" (also "x2"); supported: the factors VideoToolbox offers for this
// size (empty: none); maxDim: its largest source side for the factor (0 = unknown). Auto: x2 for sources up to
// 1920x1080 (the output stays within 4K), when the factor is offered for the size.
struct SrChoice { int factor = 0; std::string why; };
inline SrChoice DecideSr(std::string mode, int w, int h, const std::vector<float>& supported, int maxDim2 = 0, int maxDim4 = 0)
{
    for (auto& c : mode) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (!mode.empty() && mode[0] == 'x') mode = mode.substr(1);
    SrChoice r;
    auto has = [&](float f) { return std::find(supported.begin(), supported.end(), f) != supported.end(); };
    auto fits = [&](int f) { const int m = f == 4 ? maxDim4 : maxDim2; return m <= 0 || std::max(w, h) <= m; };
    char b[200];
    if (mode == "off" || mode == "0" || mode == "1" || mode == "false") { r.why = "super resolution off (your setting)"; return r; }
    if (mode == "2" || mode == "4") {
        const int f = mode == "4" ? 4 : 2;
        if (has(float(f)) && fits(f)) { r.factor = f; snprintf(b, sizeof(b), "super resolution x%d (your setting)", f); }
        else snprintf(b, sizeof(b), "super resolution x%d asked, but VideoToolbox does not offer it for %dx%d: off", f, w, h);
        r.why = b;
        return r;
    }
    if (supported.empty()) { r.why = "Auto: VideoToolbox super resolution is not available here: off"; return r; }
    if (w > 1920 || h > 1088) { snprintf(b, sizeof(b), "Auto: %dx%d is above 1080p: no super resolution", w, h); r.why = b; return r; }
    if (!has(2.0f) || !fits(2)) { snprintf(b, sizeof(b), "Auto: x2 super resolution is not offered for %dx%d: off", w, h); r.why = b; return r; }
    r.factor = 2;
    snprintf(b, sizeof(b), "Auto: super resolution x2 (%dx%d -> %dx%d)", w, h, 2 * w, 2 * h);
    r.why = b;
    return r;
}

// ------------------------------------------------------------------------------------------------ super-resolution order
// The upscalers the apple backend can run, best first :
//   Editing     VTSuperResolutionScaler (macOS 26; editing grade, temporal, takes the pair's optical flow; video up to
//               1920x1080 on macOS; needs a one-time model download through Apple's API)
//   ArtCnn32/16 ArtCNN C4F32 / C4F16 on Metal (MIT weights, tools/artcnn_tool.py; trained on anime / art)
//   LowLatency  VTLowLatencySuperResolutionScaler (macOS 26; a video-conferencing model; small sizes only)
// Auto: live action Editing > ArtCNN 32 > ArtCNN 16 > LowLatency; anime ArtCNN 32 > ArtCNN 16 > Editing > LowLatency.
// Each candidate must also pass its self-test and keep real time (Configure benchmarks it); the first that does wins.
enum class SrKind { None, Editing, ArtCnn32, ArtCnn16, LowLatency };
inline const char* SrKindName(SrKind k)
{
    switch (k) {
    case SrKind::Editing: return "VideoToolbox super resolution (editing grade)";
    case SrKind::ArtCnn32: return "ArtCNN C4F32 on Metal";
    case SrKind::ArtCnn16: return "ArtCNN C4F16 on Metal";
    case SrKind::LowLatency: return "VideoToolbox low-latency super resolution";
    default: return "none";
    }
}
struct SrCandidate { SrKind kind = SrKind::None; int factor = 0; };
struct SrInputs {
    std::string mode = "auto";       // auto | off | 2 | 4 | editing | artcnn | artcnn32 | artcnn16 | lowlatency
    int w = 0, h = 0;
    bool anime = false;
    bool editing = false;            // VTSuperResolutionScalerConfiguration.isSupported
    std::vector<float> editingFactors;
    int editingModel = 2;            // 0 download required, 1 downloading, 2 ready
    bool artcnn32 = false, artcnn16 = false;   // Metal device + weights found
    std::vector<float> llFactors;    // what the low-latency scaler offers for this size
    int llMaxDim2 = 0, llMaxDim4 = 0;
};
struct SrOrderResult { std::vector<SrCandidate> order; std::string why; bool wantModelDownload = false; };
inline SrOrderResult DecideSrOrder(const SrInputs& in)
{
    SrOrderResult r;
    std::string mode = in.mode;
    for (auto& c : mode) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (!mode.empty() && mode[0] == 'x') mode = mode.substr(1);
    if (mode.empty()) mode = "auto";
    char b[240];
    if (mode == "off" || mode == "0" || mode == "1" || mode == "false") { r.why = "super resolution off (your setting)"; return r; }
    auto has = [](const std::vector<float>& v, float f) { return std::find(v.begin(), v.end(), f) != v.end(); };
    const bool fitsEditing = in.w <= 1920 && in.h <= 1080;
    const bool editingOk = in.editing && fitsEditing && (in.editingFactors.empty() || has(in.editingFactors, 2.0f));
    auto llFits = [&](int f) { const int m = f == 4 ? in.llMaxDim4 : in.llMaxDim2; return has(in.llFactors, float(f)) && (m <= 0 || std::max(in.w, in.h) <= m); };
    auto addEditing = [&](int f) {
        if (!in.editing || !fitsEditing || (!in.editingFactors.empty() && !has(in.editingFactors, float(f)))) return;
        if (in.editingModel != 2) { r.wantModelDownload = in.editingModel == 0; return; }
        r.order.push_back({ SrKind::Editing, f });
    };
    auto addArt = [&] { if (in.artcnn32) r.order.push_back({ SrKind::ArtCnn32, 2 }); if (in.artcnn16) r.order.push_back({ SrKind::ArtCnn16, 2 }); };
    auto addLl = [&](int f) { if (llFits(f)) r.order.push_back({ SrKind::LowLatency, f }); };
    if (mode == "editing") { addEditing(2); r.why = "super resolution: editing grade (your setting)"; }
    else if (mode == "artcnn") { addArt(); r.why = "super resolution: ArtCNN (your setting)"; }
    else if (mode == "artcnn32") { if (in.artcnn32) r.order.push_back({ SrKind::ArtCnn32, 2 }); r.why = "super resolution: ArtCNN C4F32 (your setting)"; }
    else if (mode == "artcnn16") { if (in.artcnn16) r.order.push_back({ SrKind::ArtCnn16, 2 }); r.why = "super resolution: ArtCNN C4F16 (your setting)"; }
    else if (mode == "lowlatency") { addLl(2); r.why = "super resolution: low latency (your setting)"; }
    else if (mode == "4") { addEditing(4); addLl(4); r.why = "super resolution x4 (your setting)"; }
    else if (mode == "2" || mode == "auto") {
        if (mode == "auto" && (in.w > 1920 || in.h > 1088)) { snprintf(b, sizeof(b), "Auto: %dx%d is above 1080p: no super resolution", in.w, in.h); r.why = b; return r; }
        if (in.anime) { addArt(); addEditing(2); }
        else { addEditing(2); addArt(); }
        addLl(2);
        snprintf(b, sizeof(b), "%s: %s content, %dx%d", mode == "auto" ? "Auto" : "x2 (your setting)", in.anime ? "anime" : "live-action", in.w, in.h);
        r.why = b;
    } else { r.why = "super resolution: unknown setting '" + in.mode + "': off"; return r; }
    if (r.wantModelDownload) r.why += "; the editing-grade model is not downloaded yet (it downloads in the background; used from the next video)";
    else if (in.editing && fitsEditing && in.editingModel == 1) r.why += "; the editing-grade model is downloading";
    if (r.order.empty()) r.why += ": nothing offered here: off";
    else {
        r.why += ": try";
        for (auto& c : r.order) { snprintf(b, sizeof(b), " %s x%d,", SrKindName(c.kind), c.factor); r.why += b; }
        r.why.pop_back();
    }
    (void)editingOk;
    return r;
}
// Configure's real-time check: per source frame the chain runs one interpolation call (fiMs, for all phases), the
// optical flow (flowMs) and k super-resolution calls (srMs each); it must fit Ladder's share of the frame interval
inline bool FitsRealTime(double srMs, double fiMs, double flowMs, int k, double srcFps, double* costOut = nullptr)
{
    const double budget = 1000.0 / (srcFps > 0 ? srcFps : 23.976) * 0.85;
    const double cost = srMs * double(std::max(1, k)) + fiMs + flowMs;
    if (costOut) *costOut = cost;
    return cost <= budget;
}

// ------------------------------------------------------------------------------------------------ step-down ladder
// Each pair's processing time against the source frame interval. After kOverRun pairs in a row over budget it steps
// down once: 0 full -> 1 super resolution replaced by a plain scale (if it was on) -> 2 one interpolated phase (0.5;
// the other slots repeat the nearer frame) -> 3 no interpolation (every slot repeats). Never steps back up (a stable
// picture beats one that changes character every few seconds).
class Ladder {
public:
    static constexpr int kOverRun = 24;
    static constexpr double kShare = 0.85;   // of the source frame interval (VideoToolbox runs on the ANE / GPU; the copies need the rest)
    void Init(double srcFps, bool srOn, int exponent) { m_budget = 1000.0 / (srcFps > 0 ? srcFps : 23.976) * kShare; m_sr = srOn; m_exp = exponent; m_level = 0; m_over = 0; }
    // returns true when this pair changed the level
    bool Record(double ms)
    {
        m_ema = m_ema <= 0 ? ms : m_ema * 0.9 + ms * 0.1;
        if (ms <= m_budget) { m_over = 0; return false; }
        if (++m_over < kOverRun || m_level >= 3) return false;
        m_over = 0;
        ++m_level;
        if (m_level == 1 && !m_sr) ++m_level;           // nothing to drop at level 1
        if (m_level == 2 && m_exp <= 1) ++m_level;       // already a single phase
        return true;
    }
    int Level() const { return m_level; }
    bool SrActive() const { return m_sr && m_level < 1; }
    int Exponent() const { return m_level >= 3 ? 0 : m_level >= 2 ? std::min(m_exp, 1) : m_exp; }
    double BudgetMs() const { return m_budget; }
    double MeanMs() const { return m_ema; }
    std::string Describe() const
    {
        static const char* names[] = { "full", "super resolution replaced by a plain scale", "one interpolated phase", "interpolation off (frames repeated)" };
        return names[std::clamp(m_level, 0, 3)];
    }
private:
    double m_budget = 35, m_ema = 0;
    bool m_sr = false;
    int m_exp = 1, m_level = 0, m_over = 0;
};

} // namespace pmvs::apple
