// PushMPC ContentDetect: what the picture is, measured while it plays (header-only, pure C++17, no platform calls).
//
// Every automatic content decision in PushMPC comes from here or from other measurements of frames and timestamps:
// never from file names, folder names or paths, and container / stream flags are a prior at most.
//
//  - ContentDetector: hand-drawn 2D animation / 3D CG animation / live action / game footage, per shot, with
//    hysteresis. Input: one luma cell grid (~480 cells across: the same grid pushcore's held-frame pass reads back)
//    plus the Cb / Cr cell means and the frame's timestamp. Signals: per-cell holds between drawing steps (partial
//    holds, not only whole-frame repeats: characters on ones over panning backgrounds still hold their cels), the hold
//    length mix (twos / threes / fours), the step signature of a held drawing, flat-colour and line statistics of the
//    cell grid, its noise floor, camera motion, and overlays that stay put while the scene moves (game HUDs).
//    First decision after ~1.5 s; afterwards a change needs a clear margin for 2 s (1 s after a scene cut).
//  - CombMeter: interlacing from the picture (field combing on moving areas), whatever the stream's flags say.
//  - IntervalMeter: the source frame interval from timestamps (a "29.97" tag on a 24p body, VFR segments).
//
// The classifier is a multinomial logistic model over window features (2 s window, cut-limited), trained offline on
// a labelled corpus and checked leave-one-title-out; the weights are at the end of this file (kModel*). The feature
// code mirrors the offline extractor exactly where it can; see each comment for the differences.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

namespace pushmpc::content {

enum Class : uint8_t { kUnknown = 0, kDrawn = 1, kCg = 2, kLive = 3, kGame = 4, kOther = 3 };   // kOther: not drawn (live, 3D CG, game)
inline const char* ClassName(int c)
{
    static const char* n[] = { "measuring", "drawn animation", "3D CG animation", "not drawn (live action, 3D CG or game)", "game footage" };
    return c >= 0 && c <= 4 ? n[c] : n[0];
}

// What Auto's drawn-animation treatment does, per measured kind (every route uses this; the user's explicit settings win).
// Drawn + rendered CG (cel-shaded / toon-rendered 3D, measured offline against high-resolution masters): the held-drawing
// repair helps (the CG is animated on twos like drawn cels) and the anime upscalers' plain models come closest to the
// master (+3 to +5 dB against the default scaler), but their denoise-and-sharpen variant (_DS) is worse than the default
// scaler on every class measured, the sign hold finds false holds on rendered surfaces, the anime deband preset leaves
// more steps than Medium, and the line-art protection makes no measurable difference. Realistic CG is not drawn.
struct DrawnPolicy {
    bool holdRepair;      // pushcore's held-drawing repair in Animation (drawn) mode
    bool animeUpscaler;   // the anime upscalers (plain models)
    bool animeUpscalerDs; // Auto may pick an upscaler's _DS (denoise + sharpen) variant (low bitrate)
    bool signHold;        // typeset signs / credits held over a pan
    bool lineArt;         // line-art protection
    bool animeDeband;     // the anime deband preset (else the normal Auto deband rule)
};
// _DS (denoise + sharpen) is never picked automatically, on any kind: under realistic degradation (Lanczos plus
// x264 / x265 release encodes) it measured 4-5 dB worse on clean input and -0.9 to +0.5 dB on encodes, and at the low
// bitrate where Auto used to pick it the plain model won (+0.69 dB over 48 frames). It stays selectable by hand.
inline constexpr bool kAnimeUpscalerDsAuto = false;
inline constexpr DrawnPolicy kPolicyHandDrawn{ true, true, kAnimeUpscalerDsAuto, true, true, true };
inline constexpr DrawnPolicy kPolicyDrawnCg{ true, true, false, false, false, false };
inline constexpr DrawnPolicy kPolicyNotDrawn{ false, false, false, false, false, false };
inline const DrawnPolicy& PolicyFor(int cls, bool cg) { return cls == kDrawn ? (cg ? kPolicyDrawnCg : kPolicyHandDrawn) : kPolicyNotDrawn; }
// The sign hold's text gate, per measured class (pushcore: PUSHCORE_HOLD_SIGNS_STRICT). Both gates hold typeset text
// standing over a moving picture. The strict gate also asks for glyph strokes and a surround that moves several
// times more than the text: it holds far fewer outlines and patterns that are no text (cel-shaded CG, textured
// scenery), but it lets go of more real text that stands over a still or slowly moving picture (hardsubs, chat
// overlays). The first gate (dense strokes only) keeps that text. Hand-drawn pictures run the first gate: there the
// false holds it allows were rare in replay while real signs are common; drawn CG and everything not drawn (also a
// stream not measured yet, where the user turned the animation treatment on) run the strict one.
enum class SignGate : uint8_t { First = 0, Strict = 1 };
inline constexpr SignGate kSignGateHandDrawn = SignGate::First;
inline constexpr SignGate kSignGateDrawnCg = SignGate::Strict;
inline constexpr SignGate kSignGateNotDrawn = SignGate::Strict;
inline constexpr SignGate SignGateFor(int cls, bool cg) { return cls == kDrawn ? (cg ? kSignGateDrawnCg : kSignGateHandDrawn) : kSignGateNotDrawn; }

// Auto's anime upscaler next to the GPU driver's Video Super Resolution (RTX VSR, also through ZLUDA; Intel VSR), the
// same in every player (MPC-HC / BE, Kodi, VLC, the VapourSynth plugin): until the phase-4 fidelity verdict (test plan
// V8 / V9) the driver's VSR stays first wherever it runs, and Auto picks the anime model only where there is none.
// The verdict flips this one constant for all of them. Asked for by name ("anime"), the anime model runs regardless.
inline constexpr bool kAnimeSrBeforeDriverVsr = false;
inline constexpr bool AutoAnimeSrBesideVsr(bool driverVsr) { return !driverVsr || kAnimeSrBeforeDriverVsr; }

// HDR (PQ / HLG) and the anime models, which are trained on SDR pictures. Where they run in pushcore's graph (MPC, the
// Kodi pushcore route, VLC's output) they take the picture RTX VSR would: an HDR source arrives as the graph's SDR-like
// proxy (linear light with 203-nit diffuse white at SDR reference white, highlights soft-compressed, sRGB coded:
// PushFPS ScaleBlit.hlsl encodeSrgb 3 / HdrColor.hlsli HdrProxy) and the result goes back onto the HDR picture as a detail
// delta (HdrVideoSrRestore.hlsl / HdrRestore), so Auto allows HDR there. Where they read the decoded planes directly
// (pmml's Yuv420 input: the Kodi add-on's own engines, the VapourSynth plugin) they would see PQ / HLG code values, so
// Auto leaves HDR sources out there. transfer: H.273 (16 PQ, 18 HLG).
enum class AnimeSrFeed : uint8_t { PushcoreProxy, CodedPlanes };
inline constexpr bool IsHdrTransfer(int transfer) { return transfer == 16 || transfer == 18; }
inline constexpr bool AutoAnimeSrTakesTransfer(AnimeSrFeed feed, int transfer) { return !IsHdrTransfer(transfer) || feed == AnimeSrFeed::PushcoreProxy; }

// ------------------------------------------------------------------------------------------------ cell grid
// Cell means of one frame: luma in full-range 8-bit codes (0..255), Cb / Cr centred on 0 (8-bit codes).
struct Cells {
    int w = 0, h = 0;
    std::vector<float> y, cb, cr;
    bool ok() const { return w > 0 && h > 0 && y.size() == size_t(w) * size_t(h); }
};
inline constexpr int kCellsW = 480;

// Cell means from planar or semi-planar YUV 4:2:0 (8-bit, or 16-bit containers with `bits` significant bits, e.g.
// P010 = 16-bit words with the value in the top 10 bits: pass bits 16 and msb = true). uv: interleaved Cb Cr (NV12,
// P010) when v == nullptr, else separate planes. limited: video range (16..235) expanded to full range.
template <class T>
inline Cells CellsFromYuv(const T* y, ptrdiff_t yStride, const T* u, const T* v, ptrdiff_t cStride, int w, int h, int bits, bool limited)
{
    Cells c;
    if (!y || w < 16 || h < 16) return c;
    c.w = kCellsW;   // always 480 across (a narrower picture is spread over the cells, as the offline extractor did)
    c.h = std::max(1, int(std::lround(double(c.w) * h / w)));
    c.y.assign(size_t(c.w) * c.h, 0.f); c.cb.assign(c.y.size(), 0.f); c.cr.assign(c.y.size(), 0.f);
    const float scale = 255.f / float((1 << bits) - 1);
    const float ys = limited ? 255.f / 219.f : 1.f, cs = limited ? 255.f / 224.f : 1.f;
    std::vector<int> x0(c.w + 1), y0(c.h + 1);
    for (int i = 0; i <= c.w; ++i) x0[i] = int(std::lround(double(i) * w / c.w));
    for (int j = 0; j <= c.h; ++j) y0[j] = int(std::lround(double(j) * h / c.h));
    // luma: the full mean of the cell, every row and column. (Reading every second row saw one field of telecined
    // pictures, so field repeats looked held, and it raised the noise floor of clean CG: measured against the offline
    // reference on real windows, held share 0.42 vs 0.13 on a telecined 29.97 title.) 1.5-2.1 ms at 1080p, one thread (measured).
    std::vector<uint32_t> bandSum(static_cast<size_t>(w));   // a band of rows summed per column (integer, exact), then cut into cells
    for (int j = 0; j < c.h; ++j) {
        const int ysr = y0[j], ye = std::max(y0[j + 1], ysr + 1);
        std::fill(bandSum.begin(), bandSum.end(), 0u);
        for (int yy = ysr; yy < ye; ++yy) { const T* r = y + ptrdiff_t(yy) * yStride; for (int xx = 0; xx < w; ++xx) bandSum[size_t(xx)] += uint32_t(r[xx]); }
        for (int i = 0; i < c.w; ++i) {
            const int xs = x0[i], xe = std::max(x0[i + 1], xs + 1);
            uint64_t s = 0;
            for (int xx = xs; xx < xe; ++xx) s += bandSum[size_t(xx)];
            const int n = (xe - xs) * (ye - ysr);
            float m = float(double(s) / std::max(1, n)) * scale;
            if (limited) m = (m - 16.f) * ys;
            c.y[size_t(j) * c.w + i] = std::clamp(m, 0.f, 255.f);
            if (u) {
                const int cx = std::min((xs + xe) / 4, w / 2 - 1), cy = std::min((ysr + ye) / 4, h / 2 - 1);
                float cb, cr;
                if (v) { cb = float(u[ptrdiff_t(cy) * cStride + cx]); cr = float(v[ptrdiff_t(cy) * cStride + cx]); }
                else { cb = float(u[ptrdiff_t(cy) * cStride + 2 * cx]); cr = float(u[ptrdiff_t(cy) * cStride + 2 * cx + 1]); }
                c.cb[size_t(j) * c.w + i] = (cb * scale - 128.f) * cs; c.cr[size_t(j) * c.w + i] = (cr * scale - 128.f) * cs;
            }
        }
    }
    return c;
}

// ------------------------------------------------------------------------------------------------ helpers
namespace detail {
using Cplx = std::complex<float>;
// Complex DFT of any length n, unnormalised (the inverse is not divided by n), without zero padding: a circular
// transform like the offline reference's numpy FFT. Self-sorting mixed radix (Stockham: radices 4, 2, 3 and any other
// prime up to kMaxRadix, e.g. 240 = 4 x 4 x 3 x 5, 135 = 3 x 3 x 3 x 5); a length with a larger prime factor goes through
// Bluestein's chirp-z transform on the shortest 2-3-5-smooth length >= 2n - 1. `count` transforms run interleaved (element j of
// transform q at q + count * j): the columns of a row-major grid in one call, the inner loop over contiguous memory.
struct DftPlan {
    static constexpr int kMaxRadix = 13;
    int n = 0;
    std::vector<int> radix;            // per stage
    std::vector<size_t> twOff;         // per stage: offset of its twiddles W_len^(p t) (p < len / r, t < r; forward sign)
    std::vector<Cplx> tw;
    // Bluestein (n has a prime factor > kMaxRadix)
    static constexpr int kBlueBatch = 64;   // transforms at a time (bounds the scratch)
    int bm = 0;                        // the smooth length
    std::vector<DftPlan> sub;          // its plan (one element)
    std::vector<Cplx> chirp, kernF, kernI;   // e^{-i pi j^2 / n}; the transformed kernels (forward, inverse), divided by bm

    void Init(int len)
    {
        if (n == len) return;
        *this = DftPlan(); n = len;
        int rem = len;
        while (rem % 4 == 0 && rem > 4) { radix.push_back(4); rem /= 4; }
        for (int p = 2; p <= kMaxRadix && rem > 1; ++p) while (rem % p == 0) { radix.push_back(p); rem /= p; }
        if (rem > 1) {   // Bluestein
            radix.clear();
            auto smooth = [](int v) { for (int p : { 2, 3, 5 }) while (v % p == 0) v /= p; return v == 1; };
            bm = 2 * len - 1; while (!smooth(bm)) ++bm;
            sub.resize(1); sub[0].Init(bm);
            chirp.resize(size_t(len)); kernF.assign(size_t(bm), Cplx()); kernI.assign(size_t(bm), Cplx());
            for (int j = 0; j < len; ++j) {
                const double a = 3.141592653589793 * double((int64_t(j) * j) % (2 * int64_t(len))) / len;
                chirp[size_t(j)] = Cplx(float(std::cos(a)), float(-std::sin(a)));
                const Cplx b(float(std::cos(a) / bm), float(std::sin(a) / bm));   // conj(chirp) / bm
                kernF[size_t(j)] = b; kernI[size_t(j)] = std::conj(b);
                if (j) { kernF[size_t(bm - j)] = b; kernI[size_t(bm - j)] = std::conj(b); }
            }
            std::vector<Cplx> s(static_cast<size_t>(bm));
            sub[0].Run(kernF.data(), s.data(), 1, false); sub[0].Run(kernI.data(), s.data(), 1, false);
            return;
        }
        int l = len;
        for (int r : radix) {
            const int m = l / r;
            twOff.push_back(tw.size());
            for (int p = 0; p < m; ++p)
                for (int t = 0; t < r; ++t) { const double a = -6.283185307179586 * double(p) * t / l; tw.emplace_back(float(std::cos(a)), float(std::sin(a))); }
            l = m;
        }
    }
    // scratch: at least ScratchSize(count) elements
    size_t ScratchSize(int count) const { return bm ? size_t(2 * bm) * size_t(std::min(count, kBlueBatch)) : size_t(n) * size_t(count); }
    void Run(Cplx* x, Cplx* scratch, int count, bool inv) const
    {
        if (n <= 1) return;
        if (bm) {   // interleaved batches: a[q + c * k] for the batch's c transforms
            const std::vector<Cplx>& kern = inv ? kernI : kernF;
            for (int q0 = 0; q0 < count; q0 += kBlueBatch) {
                const int c = std::min(kBlueBatch, count - q0);
                Cplx* a = scratch; Cplx* s = scratch + size_t(bm) * c;
                for (int j = 0; j < n; ++j) {
                    const Cplx w = inv ? std::conj(chirp[size_t(j)]) : chirp[size_t(j)];
                    for (int q = 0; q < c; ++q) a[size_t(j) * c + q] = x[size_t(q0 + q) + size_t(count) * j] * w;
                }
                std::fill(a + size_t(n) * c, a + size_t(bm) * c, Cplx());
                sub[0].Run(a, s, c, false);
                for (int k = 0; k < bm; ++k) { const Cplx w = kern[size_t(k)]; for (int q = 0; q < c; ++q) a[size_t(k) * c + q] *= w; }
                sub[0].Run(a, s, c, true);
                for (int k = 0; k < n; ++k) {
                    const Cplx w = inv ? std::conj(chirp[size_t(k)]) : chirp[size_t(k)];
                    for (int q = 0; q < c; ++q) x[size_t(q0 + q) + size_t(count) * k] = a[size_t(k) * c + q] * w;
                }
            }
            return;
        }
        Cplx* src = x; Cplx* dst = scratch;
        int l = n; size_t s = size_t(count);
        const float sg = inv ? 1.f : -1.f;
        for (size_t st = 0; st < radix.size(); ++st) {
            const int r = radix[st], m = l / r;
            const Cplx* w = tw.data() + twOff[st];
            for (int p = 0; p < m; ++p) {
                const Cplx* in = src + s * size_t(p);
                Cplx* out = dst + s * size_t(r) * p;
                if (r == 2) {
                    const Cplx w1 = inv ? std::conj(w[size_t(p) * 2 + 1]) : w[size_t(p) * 2 + 1];
                    const Cplx* i1 = in + s * size_t(m);
                    for (size_t q = 0; q < s; ++q) { const Cplx a0 = in[q], a1 = i1[q]; out[q] = a0 + a1; out[q + s] = (a0 - a1) * w1; }
                } else if (r == 3) {
                    const Cplx w1 = inv ? std::conj(w[size_t(p) * 3 + 1]) : w[size_t(p) * 3 + 1], w2 = inv ? std::conj(w[size_t(p) * 3 + 2]) : w[size_t(p) * 3 + 2];
                    const Cplx* i1 = in + s * size_t(m); const Cplx* i2 = i1 + s * size_t(m);
                    const float h = sg * 0.8660254037844386f;   // the imaginary part of W_3 (forward: -sin 60)
                    for (size_t q = 0; q < s; ++q) {
                        const Cplx a0 = in[q], a1 = i1[q], a2 = i2[q], t1 = a1 + a2, t2 = a0 - 0.5f * t1, d = a1 - a2;
                        const Cplx jd(-h * d.imag(), h * d.real());   // i h d
                        out[q] = a0 + t1; out[q + s] = (t2 + jd) * w1; out[q + 2 * s] = (t2 - jd) * w2;
                    }
                } else if (r == 4) {
                    const Cplx w1 = inv ? std::conj(w[size_t(p) * 4 + 1]) : w[size_t(p) * 4 + 1], w2 = inv ? std::conj(w[size_t(p) * 4 + 2]) : w[size_t(p) * 4 + 2],
                               w3 = inv ? std::conj(w[size_t(p) * 4 + 3]) : w[size_t(p) * 4 + 3];
                    const Cplx* i1 = in + s * size_t(m); const Cplx* i2 = i1 + s * size_t(m); const Cplx* i3 = i2 + s * size_t(m);
                    for (size_t q = 0; q < s; ++q) {
                        const Cplx a0 = in[q], a1 = i1[q], a2 = i2[q], a3 = i3[q];
                        const Cplx b0 = a0 + a2, b1 = a0 - a2, b2 = a1 + a3, d = a1 - a3;
                        const Cplx b3(-sg * d.imag(), sg * d.real());   // sg * i * (a1 - a3): forward -i, inverse +i
                        out[q] = b0 + b2; out[q + s] = (b1 + b3) * w1; out[q + 2 * s] = (b0 - b2) * w2; out[q + 3 * s] = (b1 - b3) * w3;
                    }
                } else {   // any other prime radix: the r-point DFT directly
                    Cplx a[kMaxRadix], wr[kMaxRadix];
                    for (int k = 0; k < r; ++k) { const double ang = sg * 6.283185307179586 * k / r; wr[k] = Cplx(float(std::cos(ang)), float(std::sin(ang))); }
                    for (size_t q = 0; q < s; ++q) {
                        for (int k = 0; k < r; ++k) a[k] = in[q + s * size_t(m) * k];
                        for (int t = 0; t < r; ++t) {
                            Cplx acc = a[0];
                            for (int k = 1; k < r; ++k) acc += a[k] * wr[(k * t) % r];
                            const Cplx wt = inv ? std::conj(w[size_t(p) * r + t]) : w[size_t(p) * r + t];
                            out[q + s * size_t(t)] = acc * wt;
                        }
                    }
                }
            }
            std::swap(src, dst);
            l = m; s *= size_t(r);
        }
        if (src != x) std::copy_n(src, size_t(n) * count, x);
    }
};
// 2-D DFT of a w x h row-major grid in place (rows one by one, then all columns interleaved)
inline void Dft2(Cplx* g, int w, int h, bool inv, DftPlan& rows, DftPlan& cols, std::vector<Cplx>& scratch)
{
    rows.Init(w); cols.Init(h);
    scratch.resize(std::max(rows.ScratchSize(1), cols.ScratchSize(w)));
    for (int y = 0; y < h; ++y) rows.Run(g + size_t(y) * w, scratch.data(), 1, inv);
    cols.Run(g, scratch.data(), w, inv);
}
// area mean of a w x h grid into ow x oh (bin edges rounded as the offline extractor's reduceat)
inline std::vector<float> Area(const std::vector<float>& a, int w, int h, int ow, int oh)
{
    std::vector<float> o(size_t(ow) * oh, 0.f);
    if (ow * 2 == w && oh * 2 == h) {   // exact halving: 2 x 2 means (the bins below give the same result)
        for (int j = 0; j < oh; ++j) {
            const float* r0 = a.data() + size_t(2 * j) * w; const float* r1 = r0 + w; float* d = o.data() + size_t(j) * ow;
            for (int i = 0; i < ow; ++i) d[i] = (r0[2 * i] + r0[2 * i + 1] + r1[2 * i] + r1[2 * i + 1]) * 0.25f;
        }
        return o;
    }
    std::vector<int> xe(ow + 1), ye(oh + 1);
    for (int i = 0; i <= ow; ++i) xe[i] = int(std::lround(double(i) * w / ow));
    for (int j = 0; j <= oh; ++j) ye[j] = int(std::lround(double(j) * h / oh));
    for (int j = 0; j < oh; ++j)
        for (int i = 0; i < ow; ++i) {
            double s = 0; int n = 0;
            for (int y = ye[j]; y < std::max(ye[j + 1], ye[j] + 1) && y < h; ++y) for (int x = xe[i]; x < std::max(xe[i + 1], xe[i] + 1) && x < w; ++x) { s += a[size_t(y) * w + x]; ++n; }
            o[size_t(j) * ow + i] = float(s / std::max(1, n));
        }
    return o;
}
inline float Median(std::vector<float> v) { if (v.empty()) return 0.f; std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end()); return v[v.size() / 2]; }
} // namespace detail

// ------------------------------------------------------------------------------------------------ features
// Window features, in the model's order (kFeatureCount). Names as the offline evaluation.
// window features in the offline evaluation's order (sorted names): the appearance of the current picture (a_*, on
// a 256-wide thumbnail of the cell picture) and the cadence of the last 2 s
enum Feature { fAEdge, fAEnt, fAFlat, fAFlat2, fAHf, fALine, fAMid, fANoise, fAOrient, fAPal16, fAPal64, fAQerr, fASat, fASat90, fASatent, fASatlo, fASharp, fAStep, fActive, fBactive, fBhold, fBstep2, fCam, fChgm, fDup, fDup6, fFps, fHold, fRun1, fRun2, fRun3, fRun4, fStep, fTnoise, fTwos, kFeatureCount };
// the feature names (logs, bug reports)
inline const char* FeatureName(int i)
{
    static const char* const n[kFeatureCount] = { "aEdge", "aEnt", "aFlat", "aFlat2", "aHf", "aLine", "aMid", "aNoise", "aOrient", "aPal16", "aPal64", "aQerr",
                                                  "aSat", "aSat90", "aSatent", "aSatlo", "aSharp", "aStep", "active", "bactive", "bhold", "bstep2", "cam", "chgm",
                                                  "dup", "dup6", "fps", "hold", "run1", "run2", "run3", "run4", "step", "tnoise", "twos" };
    return i >= 0 && i < kFeatureCount ? n[i] : "?";
}

struct State {
    int cls = kUnknown;            // decided class (kUnknown while measuring)
    float prob[5] = {};            // smoothed class probabilities (index = Class)
    float confidence = 0.f;        // prob of the decided class
    double firstDecisionSec = -1;  // stream time of the first decision (-1 none yet)
    uint32_t changes = 0;          // decided-class changes since the stream started (flip count)
    uint32_t windows = 0;          // windows evaluated
    float drawnShare = 0.f;        // the last window's probability of drawn animation
    float logOdds = 0.f;           // the evidence integrator (> kOn: drawn, < -kOff: not drawn)
    bool imageEvidence = false;    // the last decision had the image model's evidence
    bool cg = false;               // drawn, and the drawn look is rendered 3D CG (cel-shaded / toon-rendered), not hand
                                   // drawn: from the image model (CgPolicy below); false while not drawn or without it
    float cgShare = 0.f;           // the image model's latest probability of CG among the drawn looks (-1: none yet)
    float cgLogOdds = 0.f;         // the CG integrator
    float feat[kFeatureCount] = {};// the last window's raw features (log, tests)
};

class ContentDetector {
public:
    // Evaluation cadence and window (stream time)
    static constexpr double kWindowSec = 2.0, kStepSec = 0.5, kHudSec = 6.0, kFirstSec = 1.5;
    // drawn vs not: a log-odds integrator of the per-window probability with a decay (stable within a scene) that
    // forgets faster at a scene cut (re-decided mainly there). Asymmetric: turning the animation extras on needs more
    // evidence than turning them off. Until the first decision: off (the safe side).
    // with the image model's evidence (stronger, so the integrator is slower and needs more of it)
    static constexpr float kImgDecay = 0.99f, kImgCutDecay = 0.85f, kImgOn = 12.0f, kImgOff = 6.0f;
    static constexpr float kDecay = 0.97f, kCutDecay = 0.6f, kOn = 6.0f, kOff = 4.0f, kClip = 3.0f;   // TRACKER
    // drawn -> rendered 3D CG: one image-model keyframe a second; on after ~4 s of clear CG evidence, off after ~3 s
    static constexpr float kCgDecay = 0.9f, kCgCutDecay = 0.7f, kCgOn = 4.0f, kCgOff = 3.0f;   // CG TRACKER

    void Reset() { *this = ContentDetector(); }
    // A discontinuity (seek, flush): the window and the previous frame are dropped, the decision stays (same title).
    // The image model's evidence (optional): the probability that the picture is drawn animation, from the CLIP image
    // embedding of a 224x224 thumbnail (ClipInput) about once a second (ImageDrawnProbability), stamped with the frame's pts.
    // pCg (optional): the probability that a drawn look is rendered 3D CG rather than hand-drawn (ImageCgProbability)
    void SetImageEvidence(double pDrawn, double pts, double pCg = -1.0) { m_img = pDrawn; m_imgT = pts; if (pCg >= 0 && pts != m_cgT) { m_imgCg = pCg; m_cgT = pts; m_cgNew = true; } }
    void Discontinuity() { m_frames.clear(); m_prev = Cells(); m_nextEval = -1; m_cutPending = true; }
    const State& Current() const { return m_state; }

    // One source frame in presentation order; pts in seconds (stream time). Returns true when a window was evaluated.
    bool Push(const Cells& c, double pts)
    {
        if (!c.ok()) return false;
        if (m_prev.ok() && (m_prev.w != c.w || m_prev.h != c.h)) Discontinuity();
        Frame f; f.pts = pts;
        if (!m_prev.ok()) {
            f.first = true; f.cut = true; m_specValid = false;
            f.chg.assign(Words(c), ~0ull); f.big = f.chg;
            f.chgShare = 1.f; f.dup = false;
        } else Temporal(c, f);
        m_hist = Histogram(c);
        m_prev = c;
        if (f.cut) m_cutPending = true;
        m_frames.push_back(std::move(f));
        while (m_frames.size() > 2 && m_frames.front().pts < pts - kHudSec - 0.5) m_frames.pop_front();
        if (m_nextEval < 0) m_nextEval = pts + 1.0;   // the first window after 1 s of frames (as the offline evaluation)
        if (pts + 1e-6 < m_nextEval) return false;
        m_nextEval = pts + kStepSec;
        float x[kFeatureCount];
        if (!WindowFeatures(x)) return false;
        Classify(x, pts);
        return true;
    }

    // The classifier alone (window features in, decision out): used by the replay of measured feature traces.
    // cut: a scene cut since the last window (-1: as Push saw it)
    void Classify(const float* x, double pts, int cut = -1)
    {
        // boosted trees (depth 3, one per class per round, 2 classes: not drawn / drawn): x >= threshold goes right
        double z[2] = { 0, 0 };
        for (int t = 0; t < kModelTrees; ++t) {
            int node = 0;
            for (int level = 0; level < 3; ++level) {
                const int j = kModelFeat[t][node];
                node = 2 * node + 1 + (j >= 0 && x[j] >= kModelThr[t][node] ? 1 : 0);
            }
            z[t % 2] += kModelRate * kModelLeaf[t][node - 7];
        }
        double pd = 1.0 / (1.0 + std::exp(z[0] - z[1]));
        // the image model's evidence (when it runs: about once a second, at most 3 s old) dominates the blend; the
        // hand-built cues keep 15 % (offline: drawn recall 98.1 %, live / game called drawn 0.9 % of their run time)
        const bool img = m_imgT > -1e17 && pts - m_imgT <= 3.0 && pts >= m_imgT - 1.0;
        if (img) {
            auto lg = [](double q) { q = std::clamp(q, 1e-4, 1.0 - 1e-4); return std::log(q / (1.0 - q)); };
            pd = 1.0 / (1.0 + std::exp(-(0.15 * lg(pd) + 0.85 * lg(m_img))));
        }
        const float decay = img ? kImgDecay : kDecay, cutDecay = img ? kImgCutDecay : kCutDecay, on = img ? kImgOn : kOn, off = img ? kImgOff : kOff;
        State& s = m_state;
        std::copy(x, x + kFeatureCount, s.feat);
        ++s.windows;
        const bool isCut = cut < 0 ? m_cutPending : cut != 0; m_cutPending = false;
        const double q = std::clamp(pd, 1e-4, 1.0 - 1e-4);
        m_L = (isCut ? cutDecay : decay) * m_L + std::clamp(std::log(q / (1.0 - q)), -double(kClip), double(kClip));
        int next = s.cls;
        if (m_L > on) next = kDrawn;
        else if (m_L < -off) next = kOther;
        if (next != s.cls) {
            if (s.cls != kUnknown) ++s.changes; else s.firstDecisionSec = pts;
            s.cls = next;
        }
        s.drawnShare = float(pd); s.logOdds = float(m_L); s.imageEvidence = img;
        // drawn: rendered 3D CG or hand-drawn, from the image model only (one new keyframe a step at most), with its own
        // integrator (hysteresis); reset with the drawn decision so a new drawn stretch starts undecided (hand-drawn)
        if (s.cls != kDrawn) { m_Lcg = 0; s.cg = false; }
        else if (img && m_cgNew) {
            const double c = std::clamp(m_imgCg, 1e-4, 1.0 - 1e-4);
            m_Lcg = (isCut ? kCgCutDecay : kCgDecay) * m_Lcg + std::clamp(std::log(c / (1.0 - c)), -double(kClip), double(kClip));
            if (m_Lcg > kCgOn) s.cg = true; else if (m_Lcg < -kCgOff) s.cg = false;
        }
        m_cgNew = false;
        s.cgShare = m_cgT > -1e17 ? float(m_imgCg) : -1.f; s.cgLogOdds = float(m_Lcg);
        s.prob[kDrawn] = float(pd); s.prob[kOther] = float(1.0 - pd);
        s.confidence = s.cls == kDrawn ? float(pd) : s.cls == kOther ? float(1.0 - pd) : 0.f;
    }

private:
    struct Frame {
        double pts = 0;
        bool first = false, cut = false, dup = false;
        float chgShare = 0, cam = 0;
        float tnoise = -1;                 // median change of the cells that did not change (temporal noise floor)
        std::vector<uint64_t> chg, big;   // per cell bits (row-major)
        std::vector<uint8_t> grad;         // coarse texture (kHudW x hudH)
        std::vector<uint8_t> coarse;       // share of changed cells per coarse cell (0..255)
    };
    static constexpr int kHudW = 60;
    static size_t Words(const Cells& c) { return (size_t(c.w) * c.h + 63) / 64; }
    static int CountTrailingZeros(uint64_t m) { int k = 0; while (!(m & 1u)) { m >>= 1; ++k; } return k; }
    static bool Bit(const std::vector<uint64_t>& b, size_t i) { return (b[i >> 6] >> (i & 63)) & 1u; }

    static std::array<float, 32> Histogram(const Cells& c)
    {
        std::array<float, 32> h{};
        for (float v : c.y) h[std::min(31, int(v / 8.f))] += 1.f;
        for (auto& v : h) v /= float(c.y.size());
        return h;
    }
    // changed cells against the previous frame, with the camera shift taken out (phase correlation at half size,
    // refined by +-1 cell), the noise floor of held frames, cuts
    void Temporal(const Cells& c, Frame& f)
    {
        const int w = c.w, h = c.h; const size_t n = size_t(w) * h;
        const float thr = m_nfN < 8 ? 8.f : std::clamp(2.5f * m_nf + 2.f, 5.f, 12.f);
        std::vector<float> d0(n);
        double mean = 0; size_t over8 = 0;
        for (size_t i = 0; i < n; ++i) { d0[i] = std::fabs(c.y[i] - m_prev.y[i]); mean += d0[i]; over8 += d0[i] > 8.f; }
        mean /= double(n);
        int dx = 0, dy = 0;
        PhaseShift(m_prev, c, dx, dy);
        // the camera shift refined by +-1 cell: the mean difference of each candidate first (rows with the clamped
        // source row, the interior without per-pixel clamps), then the best one's map once
        auto shiftedDiff = [&](int sx, int sy, float* out) {
            double m = 0;
            const int xa = std::clamp(sx, 0, w), xb = std::clamp(w + sx, 0, w);   // x - sx inside [0, w) for x in [xa, xb)
            for (int y = 0; y < h; ++y) {
                const float* cur = c.y.data() + size_t(y) * w;
                const float* pr = m_prev.y.data() + size_t(std::clamp(y - sy, 0, h - 1)) * w;
                float* o = out ? out + size_t(y) * w : nullptr;
                for (int x = 0; x < w; ++x) {
                    const float v = std::fabs(cur[x] - pr[x >= xa && x < xb ? x - sx : std::clamp(x - sx, 0, w - 1)]);
                    m += v; if (o) o[x] = v;
                }
            }
            return m / double(n);
        };
        std::vector<float> best;
        double bestMean = mean; int bx = 0, by = 0;
        if (dx || dy) {
            for (int ox = -1; ox <= 1; ++ox)
                for (int oy = -1; oy <= 1; ++oy) {
                    const double m = shiftedDiff(dx + ox, dy + oy, nullptr);
                    if (m < bestMean) { bestMean = m; bx = dx + ox; by = dy + oy; f.cam = 1.f; }
                }
        }
        if (f.cam > 0.f) { best.resize(n); shiftedDiff(bx, by, best.data()); } else best = d0;
        f.chg.assign(Words(c), 0); f.big.assign(Words(c), 0);
        size_t nc = 0;
        for (size_t i = 0; i < n; ++i) {
            const float d = std::min(d0[i], best[i]);
            if (d > thr) { f.chg[i >> 6] |= 1ull << (i & 63); ++nc; }
            if (d > 3.f * thr) f.big[i >> 6] |= 1ull << (i & 63);
        }
        f.chgShare = float(nc) / float(n);
        if (n - nc > 1000) {   // the noise of what did not move (live action: grain and sensor noise; drawn: a still painting)
            // the median from a histogram of 1/64 code steps (the values that did not change are below 12 codes)
            static constexpr int kBins = 64 * 16;
            uint32_t hist[kBins + 1] = {};
            for (size_t i = 0; i < n; ++i) if (!Bit(f.chg, i)) ++hist[std::min(kBins, int(d0[i] * 64.f))];
            const size_t half = (n - nc) / 2; size_t acc = 0; int b = 0;
            for (; b <= kBins; ++b) { acc += hist[b]; if (acc > half) break; }
            f.tnoise = (float(b) + 0.5f) / 64.f;
        }
        f.dup = over8 <= std::max<size_t>(3, n / 20000) && mean < 3.0;
        if (f.dup) {
            std::vector<float> s = d0; const size_t k = n * 99 / 100; std::nth_element(s.begin(), s.begin() + k, s.end());
            m_nf = m_nfN == 0 ? s[k] : m_nf + (s[k] - m_nf) * 0.05f; ++m_nfN;
        }
        const auto hs = Histogram(c); float l1 = 0; for (int k = 0; k < 32; ++k) l1 += std::fabs(hs[k] - m_hist[k]);
        f.cut = l1 >= 0.30f && f.chgShare > 0.5f;
    }
    // phase correlation at half size (the area-halved cell picture, w/2 x h/2), circular on the grid itself as the offline
    // reference (no zero padding: padding to powers of two moved the peak on ~3 % of real frames, measured); the current frame's
    // spectrum is kept for the next frame (one forward and one inverse transform a frame)
    void PhaseShift(const Cells& a, const Cells& b, int& dx, int& dy)
    {
        const int w2 = std::max(1, a.w / 2), h2 = std::max(1, a.h / 2);
        const size_t N = size_t(w2) * h2;
        auto spectrum = [&](const Cells& c, std::vector<detail::Cplx>& s) {
            const auto A = detail::Area(c.y, c.w, c.h, w2, h2);
            double m = 0; for (float v : A) m += v; m /= double(A.size());
            s.resize(N);
            for (size_t i = 0; i < N; ++i) s[i] = detail::Cplx(float(A[i] - m), 0.f);
            detail::Dft2(s.data(), w2, h2, false, m_dftRows, m_dftCols, m_dftScratch);
        };
        if (!m_specValid || m_spec.size() != N) spectrum(a, m_spec);
        spectrum(b, m_specNext);
        m_corr.resize(N);
        for (size_t i = 0; i < N; ++i) {   // B x conj(A), normalised
            const detail::Cplx x = m_specNext[i] * std::conj(m_spec[i]);
            m_corr[i] = x / (std::abs(x) + 1e-6f);
        }
        m_spec.swap(m_specNext); m_specValid = true;   // b becomes the next frame's a (m_prev)
        detail::Dft2(m_corr.data(), w2, h2, true, m_dftRows, m_dftCols, m_dftScratch);
        size_t bi0 = 0; float bv = -1e30f;
        for (size_t i = 0; i < N; ++i) if (m_corr[i].real() > bv) { bv = m_corr[i].real(); bi0 = i; }
        int py = int(bi0 / size_t(w2)), px = int(bi0 % size_t(w2));
        if (py > h2 / 2) py -= h2;
        if (px > w2 / 2) px -= w2;
        dx = px * 2; dy = py * 2;
    }
    bool WindowFeatures(float* x) const
    {
        if (m_frames.size() < 8) return false;
        const double t1 = m_frames.back().pts;
        size_t i1 = m_frames.size(), i0 = 0;
        while (i0 < i1 && m_frames[i0].pts < t1 - kWindowSec) ++i0;
        for (size_t i = i0; i < i1; ++i) if (m_frames[i].cut) i0 = i;   // from the last cut (that frame = reference)
        if (i1 - i0 < 8) return false;
        const size_t T = i1 - i0 - 1;   // frames after the reference
        if (T < 6) return false;
        const size_t n = size_t(m_prev.w) * m_prev.h;
        // active cells: changed in >= 2 frames of the window
        std::vector<uint16_t> cnt(n, 0);
        for (size_t t = 0; t < T; ++t) { const auto& b = m_frames[i0 + 1 + t].chg; for (size_t wd = 0; wd < b.size(); ++wd) for (uint64_t m = b[wd]; m; m &= m - 1) { const size_t i = wd * 64 + size_t(CountTrailingZeros(m)); if (i < n) ++cnt[i]; } }
        std::vector<uint32_t> act; act.reserve(n);
        for (size_t i = 0; i < n; ++i) if (cnt[i] >= 2) act.push_back(uint32_t(i));
        const size_t na = act.size();
        std::fill(x, x + kFeatureCount, 0.f);
        x[fActive] = float(na) / float(n);
        {   // cells that make >= 2 BIG steps (> 3x the noise threshold): drawn parts jump between drawings and hold
            // still in between; live action that moves that much moves on every frame
            std::vector<uint16_t> bc(n, 0);
            for (size_t t = 0; t < T; ++t) { const auto& b = m_frames[i0 + 1 + t].big; for (size_t wd = 0; wd < b.size(); ++wd) for (uint64_t m = b[wd]; m; m &= m - 1) { const size_t i = wd * 64 + size_t(CountTrailingZeros(m)); if (i < n) ++bc[i]; } }
            std::vector<uint32_t> ba; for (size_t i = 0; i < n; ++i) if (bc[i] >= 2) ba.push_back(uint32_t(i));
            x[fBactive] = float(ba.size()) / float(n);
            if (ba.size() >= 30) {
                std::vector<int16_t> f0(ba.size(), -1), f1(ba.size(), -1);
                for (size_t t = 0; t < T; ++t) { const auto& b = m_frames[i0 + 1 + t].big; for (size_t k = 0; k < ba.size(); ++k) if (Bit(b, ba[k])) { if (f0[k] < 0) f0[k] = int16_t(t); f1[k] = int16_t(t); } }
                std::vector<uint16_t> hl(ba.size(), 0);
                for (size_t t = 0; t < T; ++t) { const auto& c = m_frames[i0 + 1 + t].chg; for (size_t k = 0; k < ba.size(); ++k) hl[k] += int(t) > f0[k] && int(t) < f1[k] && !Bit(c, ba[k]); }
                double hs = 0; size_t step2 = 0;
                for (size_t k = 0; k < ba.size(); ++k) {
                    hs += double(hl[k]) / double(std::max(f1[k] - f0[k] - 1, 1));
                    const double gap = double(f1[k] - f0[k]) / double(std::max(bc[ba[k]] - 1, 1));
                    step2 += gap >= 1.5 && gap <= 4.5;
                }
                x[fBhold] = float(hs / ba.size()); x[fBstep2] = float(double(step2) / ba.size());
            }
        }
        if (na >= 50) {
            // per active cell: frames held between its first and last change, hold runs, the step signature
            std::vector<int16_t> first(na, -1), last(na, -1), run(na, 0);
            for (size_t t = 0; t < T; ++t) { const auto& b = m_frames[i0 + 1 + t].chg; for (size_t k = 0; k < na; ++k) if (Bit(b, act[k])) { if (first[k] < 0) first[k] = int16_t(t); last[k] = int16_t(t); } }
            std::vector<uint16_t> held(na, 0);
            double runs[4] = {}; size_t stepHits = 0, holds = 0;
            for (size_t t = 0; t < T; ++t) {
                const auto& b = m_frames[i0 + 1 + t].chg; const auto& bg = m_frames[i0 + 1 + t].big;
                for (size_t k = 0; k < na; ++k) {
                    const bool c = Bit(b, act[k]);
                    if (c && run[k] > 0) {
                        const int L = run[k]; runs[std::min(L, 4) - 1] += 1;
                        const int tb = int(t) - L - 1;
                        if (tb >= 0) { ++holds; stepHits += Bit(bg, act[k]) && Bit(m_frames[i0 + 1 + size_t(tb)].big, act[k]); }
                    }
                    const bool h = int(t) > first[k] && int(t) < last[k] && !c;
                    if (h) ++held[k];
                    run[k] = h ? int16_t(run[k] + 1) : int16_t(0);
                }
            }
            double hs = 0; size_t twos = 0;
            for (size_t k = 0; k < na; ++k) { const double hk = double(held[k]) / double(std::max(last[k] - first[k] - 1, 1)); hs += hk; twos += hk > 0.3 && hk < 0.8; }
            x[fHold] = float(hs / na); x[fTwos] = float(double(twos) / na);
            x[fStep] = float(double(stepHits) / double(std::max<size_t>(1, holds)));
            const double tot = std::max(1.0, runs[0] + runs[1] + runs[2] + runs[3]);
            x[fRun1] = float(runs[0] / tot); x[fRun2] = float(runs[1] / tot); x[fRun3] = float(runs[2] / tot); x[fRun4] = float(runs[3] / tot);
        }
        double dup = 0, chg = 0, cam = 0;
        for (size_t t = i0 + 1; t < i1; ++t) { dup += m_frames[t].dup; chg += m_frames[t].chgShare; cam += m_frames[t].cam; }
        x[fDup] = float(dup / T); x[fChgm] = float(chg / T); x[fCam] = float(cam / T);
        double dup6 = 0; for (const Frame& fr : m_frames) dup6 += fr.dup;
        x[fDup6] = float(dup6 / double(m_frames.size()));
        std::vector<float> tn; for (size_t t = i0; t < i1; ++t) if (m_frames[t].tnoise >= 0) tn.push_back(m_frames[t].tnoise);
        x[fTnoise] = tn.empty() ? -1.f : detail::Median(tn);
        std::vector<float> st;
        for (size_t t = i0 + 1; t < i1; ++t) { const double d = m_frames[t].pts - m_frames[t - 1].pts; if (d > 0.001 && d < 0.25) st.push_back(float(d)); }
        x[fFps] = st.empty() ? 0.f : 1.f / detail::Median(st);
        Appearance(m_prev, x);
        return true;
    }
    // Appearance of the current picture (classic cel-vs-photo cues) on a 256-wide thumbnail of the cell picture (RGB from
    // the cell means of Y, Cb, Cr, BT.601): flat share, step vs blur edges, dark outlines, palette size and quantisation
    // error, gradient orientation entropy, high-frequency share, saturation distribution, flat-area noise.
    static void Appearance(const Cells& c, float* x)
    {
        const int TW = 256;
        const int th = std::fabs(double(c.w) / c.h - 16.0 / 9.0) < 0.3 ? 144 : std::max(8, int(std::lround(double(TW) * c.h / c.w)));
        const bool chroma = c.cb.size() == c.y.size() && c.cr.size() == c.y.size();
        std::vector<float> R(c.y.size()), G(c.y.size()), B(c.y.size());
        for (size_t i = 0; i < c.y.size(); ++i) {
            const float y = c.y[i], cb = chroma ? c.cb[i] : 0.f, cr = chroma ? c.cr[i] : 0.f;
            R[i] = std::clamp(y + 1.402f * cr, 0.f, 255.f); G[i] = std::clamp(y - 0.344f * cb - 0.714f * cr, 0.f, 255.f); B[i] = std::clamp(y + 1.772f * cb, 0.f, 255.f);
        }
        const auto r = detail::Area(R, c.w, c.h, TW, th), g = detail::Area(G, c.w, c.h, TW, th), b = detail::Area(B, c.w, c.h, TW, th);
        const int W = TW, H = th; const size_t N = size_t(W) * H;
        std::vector<float> Y(N), gx(N, 0.f), gy(N, 0.f), gm(N, 0.f);
        for (size_t i = 0; i < N; ++i) Y[i] = r[i] * 0.299f + g[i] * 0.587f + b[i] * 0.114f;
        auto at = [&](int xx, int yy) { return Y[size_t(yy) * W + xx]; };
        for (int yy = 0; yy < H; ++yy)
            for (int xx = 0; xx < W; ++xx) {
                const size_t i = size_t(yy) * W + xx;
                if (xx > 0 && xx < W - 1) gx[i] = (at(xx + 1, yy) - at(xx - 1, yy)) * 0.5f;
                if (yy > 0 && yy < H - 1) gy[i] = (at(xx, yy + 1) - at(xx, yy - 1)) * 0.5f;
                gm[i] = std::hypot(gx[i], gy[i]);
            }
        size_t flat = 0, flat2 = 0, mid = 0, edge = 0, line = 0;
        std::vector<float> sharp;
        for (int yy = 0; yy < H; ++yy)
            for (int xx = 0; xx < W; ++xx) {
                const size_t i = size_t(yy) * W + xx; const float v = gm[i];
                flat += v < 1.f; flat2 += v < 2.5f; mid += v >= 2.5f && v < 12.f; edge += v >= 20.f;
                if (v >= 20.f) {   // step vs blur: the gradient against the range within +-2 pixels across x and y
                    float mx = at(xx, yy), mn = mx;
                    for (int d = -2; d <= 2; ++d) {
                        const float a1 = at(std::clamp(xx + d, 0, W - 1), yy), a2 = at(xx, std::clamp(yy + d, 0, H - 1));
                        mx = std::max(mx, std::max(a1, a2)); mn = std::min(mn, std::min(a1, a2));
                    }
                    sharp.push_back(2.f * v / std::max(mx - mn, 1.f));
                }
                bool l = false;   // a dark outline: darker than both neighbours across x or y by > 20 codes
                if (xx > 0 && xx < W - 1) l = l || (at(xx - 1, yy) - at(xx, yy) > 20.f && at(xx + 1, yy) - at(xx, yy) > 20.f);
                if (yy > 0 && yy < H - 1) l = l || (at(xx, yy - 1) - at(xx, yy) > 20.f && at(xx, yy + 1) - at(xx, yy) > 20.f);
                line += l;
            }
        x[fAFlat] = float(flat) / N; x[fAFlat2] = float(flat2) / N; x[fAMid] = float(mid) / N; x[fAEdge] = float(edge) / N; x[fALine] = float(line) / N;
        if (sharp.size() > 50) {
            size_t st = 0; for (float v : sharp) st += v > 0.8f;
            x[fAStep] = float(st) / sharp.size(); x[fASharp] = detail::Median(std::move(sharp));
        } else { x[fAStep] = 0.f; x[fASharp] = 0.f; }
        // palette (4 bits per channel): share of the 16 / 64 most common colours, entropy; quantisation error to 3 bits
        std::vector<uint32_t> hist(4096, 0); double qerr = 0;
        for (size_t i = 0; i < N; ++i) {
            const float ch[3] = { r[i], g[i], b[i] };
            int k = 0;
            for (int cc = 0; cc < 3; ++cc) { k = k * 16 + std::min(15, int(ch[cc] / 16.f)); qerr += std::fabs(ch[cc] - (std::floor(ch[cc] / 32.f) * 32.f + 16.f)); }
            ++hist[size_t(k)];
        }
        std::vector<uint32_t> sorted(hist); std::sort(sorted.begin(), sorted.end(), std::greater<uint32_t>());
        double p16 = 0, p64 = 0, ent = 0;
        for (int k = 0; k < 64; ++k) { if (k < 16) p16 += sorted[size_t(k)]; p64 += sorted[size_t(k)]; }
        for (uint32_t h : hist) if (h) { const double pp = double(h) / N; ent -= pp * std::log2(pp); }
        x[fAPal16] = float(p16 / N); x[fAPal64] = float(p64 / N); x[fAEnt] = float(ent); x[fAQerr] = float(qerr / (3.0 * N));
        // orientation entropy (8 bins weighted by magnitude, where the gradient > 3) and the high-frequency share
        double ob[8] = {}, osum = 0; size_t on = 0;
        for (size_t i = 0; i < N; ++i) if (gm[i] > 3.f) {
            double a = std::atan2(double(gy[i]), double(gx[i])); a = std::fmod(a + 3.14159265358979, 3.14159265358979);
            ob[std::min(7, int(a / 3.14159265358979 * 8.0))] += gm[i]; osum += gm[i]; ++on;
        }
        double oe = 0; if (on > 50) for (double v : ob) if (v > 0) { const double pp = v / osum; oe -= pp * std::log2(pp); }
        x[fAOrient] = float(oe);
        double lap = 0, gin = 0; size_t nin = 0; std::vector<float> res;
        for (int yy = 1; yy < H - 1; ++yy)
            for (int xx = 1; xx < W - 1; ++xx) {
                const float nb = at(xx - 1, yy) + at(xx + 1, yy) + at(xx, yy - 1) + at(xx, yy + 1);
                lap += std::fabs(4.f * at(xx, yy) - nb); gin += gm[size_t(yy) * W + xx]; ++nin;
                if (gm[size_t(yy) * W + xx] < 2.f) res.push_back(std::fabs(at(xx, yy) - nb * 0.25f));
            }
        x[fAHf] = float((lap / std::max<size_t>(1, nin)) / (gin / std::max<size_t>(1, nin) + 1e-3));
        x[fANoise] = res.size() > 100 ? detail::Median(std::move(res)) : -1.f;
        // saturation distribution
        std::vector<float> sat(N); double ss = 0; size_t lo = 0; double sh[16] = {};
        for (size_t i = 0; i < N; ++i) {
            const float mx = std::max(r[i], std::max(g[i], b[i])), mn = std::min(r[i], std::min(g[i], b[i]));
            sat[i] = (mx - mn) / (mx + 1.f); ss += sat[i]; lo += sat[i] < 0.08f; sh[std::min(15, int(sat[i] * 16.f))] += 1.0;
        }
        double se = 0; for (double v : sh) if (v > 0) { const double pp = v / N; se -= pp * std::log2(pp); }
        std::vector<float> s9(sat); const size_t k9 = size_t(0.9 * double(N - 1)); std::nth_element(s9.begin(), s9.begin() + ptrdiff_t(k9), s9.end());
        x[fASat] = float(ss / N); x[fASat90] = s9[k9]; x[fASatlo] = float(lo) / N; x[fASatent] = float(se);
    }

    std::deque<Frame> m_frames;
    Cells m_prev;
    std::array<float, 32> m_hist{};
    float m_nf = 0; uint32_t m_nfN = 0;
    detail::DftPlan m_dftRows, m_dftCols;
    std::vector<detail::Cplx> m_spec, m_specNext, m_corr, m_dftScratch;   // the previous frame's spectrum, work buffers (PhaseShift)
    bool m_specValid = false;   // m_specRe/Im hold m_prev's spectrum
    double m_nextEval = -1, m_t0 = -1, m_L = 0, m_img = 0.5, m_imgT = -1e18, m_Lcg = 0, m_imgCg = 0.5, m_cgT = -1e18;
    bool m_cgNew = false;
    bool m_cutPending = true;
    int m_run = 0;
    State m_state;

public:
    // ---- MODEL (generated by the offline evaluation: boosted trees, depth 3, 2 classes (not drawn, drawn), one tree per
    // class per round; features in Feature order, raw values; x >= threshold goes right; feature -1 = no split)
    static constexpr int kModelVersion = 3;
    static constexpr int kModelTrees = 240;
    static constexpr double kModelRate = 0.15;
    static constexpr int8_t kModelFeat[kModelTrees][7] = {
        { 20, 26, 21, 5, 12, 26, 26 },
        { 20, 26, 21, 5, 12, 26, 26 },
        { 32, 25, 26, 6, 26, 25, 32 },
        { 32, 25, 26, 6, 26, 25, 32 },
        { 32, 25, 18, 29, 15, 29, 26 },
        { 32, 25, 18, 29, 15, 29, 26 },
        { 25, 6, 26, 1, 29, 5, 32 },
        { 25, 6, 26, 1, 29, 5, 32 },
        { 29, 26, 26, 25, 32, 32, 23 },
        { 29, 26, 26, 25, 32, 32, 23 },
        { 32, 26, 4, 10, 22, 23, 30 },
        { 32, 26, 4, 10, 22, 23, 30 },
        { 28, 26, 12, 31, 32, 26, 14 },
        { 28, 26, 12, 31, 32, 26, 14 },
        { 32, 2, 26, 9, 6, 15, 7 },
        { 32, 2, 26, 9, 6, 15, 7 },
        { 26, 5, 1, 11, 17, 22, -1 },
        { 26, 5, 1, 11, 17, 22, -1 },
        { 30, 31, 1, 26, 4, 13, 17 },
        { 30, 31, 1, 26, 4, 13, 17 },
        { 26, 28, 22, 5, 12, 10, 19 },
        { 26, 28, 22, 5, 12, 10, 19 },
        { 26, 10, 7, 4, 11, 16, 19 },
        { 26, 10, 7, 4, 11, 16, 19 },
        { 1, 6, 17, 13, 26, 26, 26 },
        { 1, 6, 17, 13, 26, 26, 26 },
        { 25, 13, 15, 26, 10, 26, 11 },
        { 25, 13, 15, 26, 10, 26, 11 },
        { 29, 22, 26, 6, 10, 20, 4 },
        { 29, 22, 26, 6, 10, 20, 4 },
        { 26, 25, 22, 10, 19, 29, 20 },
        { 26, 25, 22, 10, 19, 29, 20 },
        { 21, 6, 19, 33, 10, 33, 29 },
        { 21, 6, 19, 33, 10, 33, 29 },
        { 14, 13, 26, 10, 1, 4, 25 },
        { 14, 13, 26, 10, 1, 4, 25 },
        { 29, 14, 26, 7, 15, 27, 26 },
        { 29, 14, 26, 7, 15, 27, 26 },
        { 14, 10, 16, 19, 7, 1, 10 },
        { 14, 10, 16, 19, 7, 1, 10 },
        { 26, 8, 4, 7, 4, 1, 10 },
        { 26, 8, 4, 7, 4, 1, 10 },
        { 30, 31, 30, 26, 33, 12, 4 },
        { 30, 31, 30, 26, 33, 12, 4 },
        { 13, 9, 10, 7, 7, 26, 13 },
        { 13, 9, 10, 7, 7, 26, 13 },
        { 22, 11, 25, 33, 10, 20, 11 },
        { 22, 11, 25, 33, 10, 20, 11 },
        { 14, 13, 7, 7, 1, 11, 26 },
        { 14, 13, 7, 7, 1, 11, 26 },
        { 10, 17, 5, 13, 26, 16, 29 },
        { 10, 17, 5, 13, 26, 16, 29 },
        { 25, 10, 26, 7, 13, 26, 4 },
        { 25, 10, 26, 7, 13, 26, 4 },
        { 13, 26, 4, 26, 26, 8, 26 },
        { 13, 26, 4, 26, 26, 8, 26 },
        { 1, 26, 7, 4, 2, 11, 6 },
        { 1, 26, 7, 4, 2, 11, 6 },
        { 6, 25, 20, 26, 33, 17, 13 },
        { 6, 25, 20, 26, 33, 17, 13 },
        { 26, 14, 22, 15, 10, 9, 27 },
        { 26, 14, 22, 15, 10, 9, 27 },
        { 16, 1, 15, 5, 14, 14, 5 },
        { 16, 1, 15, 5, 14, 14, 5 },
        { 11, 7, 12, 4, 20, 25, 26 },
        { 11, 7, 12, 4, 20, 25, 26 },
        { 29, 20, 26, 23, 5, 27, 26 },
        { 29, 20, 26, 23, 5, 27, 26 },
        { 14, 10, 26, 32, 13, 4, 25 },
        { 14, 10, 26, 32, 13, 4, 25 },
        { 14, 10, 17, 7, 31, 1, 10 },
        { 14, 10, 17, 7, 31, 1, 10 },
        { 8, 33, 30, 3, 7, 8, 30 },
        { 8, 33, 30, 3, 7, 8, 30 },
        { 11, 16, 1, 10, 5, 20, 16 },
        { 11, 16, 1, 10, 5, 20, 16 },
        { 26, 14, 22, 26, 4, 29, 4 },
        { 26, 14, 22, 26, 4, 29, 4 },
        { 7, 23, 26, 11, 12, 25, 7 },
        { 7, 23, 26, 11, 12, 25, 7 },
        { 22, 31, 14, 29, 33, 5, 26 },
        { 22, 31, 14, 29, 33, 5, 26 },
        { 28, 5, 29, 29, 3, 5, 29 },
        { 28, 5, 29, 29, 3, 5, 29 },
        { 11, 11, 13, 7, 4, 0, 26 },
        { 11, 11, 13, 7, 4, 0, 26 },
        { 21, 20, 5, 18, 22, 7, 33 },
        { 21, 20, 5, 18, 22, 7, 33 },
        { 26, 25, 22, 29, 4, 28, 23 },
        { 26, 25, 22, 29, 4, 28, 23 },
        { 8, 14, 26, 10, 7, 30, 9 },
        { 8, 14, 26, 10, 7, 30, 9 },
        { 10, 16, 2, 22, 26, 26, 11 },
        { 10, 16, 2, 22, 26, 26, 11 },
        { 1, 26, 7, 10, 29, 13, 12 },
        { 1, 26, 7, 10, 29, 13, 12 },
        { 27, 23, 18, 4, 32, 5, 22 },
        { 27, 23, 18, 4, 32, 5, 22 },
        { 0, 2, 10, 9, 31, 17, 20 },
        { 0, 2, 10, 9, 31, 17, 20 },
        { 8, 33, 25, 1, 13, 16, 4 },
        { 8, 33, 25, 1, 13, 16, 4 },
        { 10, 17, 16, 29, 29, 9, 14 },
        { 10, 17, 16, 29, 29, 9, 14 },
        { 23, 30, 0, 31, 34, 32, 12 },
        { 23, 30, 0, 31, 34, 32, 12 },
        { 13, 33, 15, 8, 6, 6, 32 },
        { 13, 33, 15, 8, 6, 6, 32 },
        { 13, 1, 15, 14, 8, 26, 5 },
        { 13, 1, 15, 14, 8, 26, 5 },
        { 13, 9, 25, 17, 7, 29, 4 },
        { 13, 9, 25, 17, 7, 29, 4 },
        { 32, 32, 33, 5, 4, 6, 23 },
        { 32, 32, 33, 5, 4, 6, 23 },
        { 14, 7, 12, 11, 26, 17, 17 },
        { 14, 7, 12, 11, 26, 17, 17 },
        { 26, 3, 4, 14, 7, 0, -1 },
        { 26, 3, 4, 14, 7, 0, -1 },
        { 32, 32, 4, 4, 4, 6, 33 },
        { 32, 32, 4, 4, 4, 6, 33 },
        { 26, 10, 7, 13, 15, 17, 2 },
        { 26, 10, 7, 13, 15, 17, 2 },
        { 14, 13, 4, 12, 0, 18, 28 },
        { 14, 13, 4, 12, 0, 18, 28 },
        { 26, 11, 10, 26, 10, 30, 22 },
        { 26, 11, 10, 26, 10, 30, 22 },
        { 1, 14, 14, 13, 17, 26, 26 },
        { 1, 14, 14, 13, 17, 26, 26 },
        { 13, 4, 10, 19, 32, 26, 26 },
        { 13, 4, 10, 19, 32, 26, 26 },
        { 31, 0, 34, 6, 28, 22, 20 },
        { 31, 0, 34, 6, 28, 22, 20 },
        { 21, 5, 8, 16, 27, 24, 9 },
        { 21, 5, 8, 16, 27, 24, 9 },
        { 1, 26, 26, 25, 24, 13, 27 },
        { 1, 26, 26, 25, 24, 13, 27 },
        { 28, 7, 23, 3, 12, 27, 24 },
        { 28, 7, 23, 3, 12, 27, 24 },
        { 7, 1, 20, 32, 12, 25, 26 },
        { 7, 1, 20, 32, 12, 25, 26 },
        { 25, 26, 24, 26, 4, 26, 33 },
        { 25, 26, 24, 26, 4, 26, 33 },
        { 13, 6, 15, 7, 16, 14, 26 },
        { 13, 6, 15, 7, 16, 14, 26 },
        { 14, 15, 4, 9, 6, 28, 13 },
        { 14, 15, 4, 9, 6, 28, 13 },
        { 26, -1, 18, -1, -1, 25, 26 },
        { 26, -1, 18, -1, -1, 25, 26 },
        { 14, 7, 10, 26, 10, 13, 5 },
        { 14, 7, 10, 26, 10, 13, 5 },
        { 13, 26, 15, 26, 24, 25, 0 },
        { 13, 26, 15, 26, 24, 25, 0 },
        { 30, 5, 15, 14, 26, 7, 4 },
        { 30, 5, 15, 14, 26, 7, 4 },
        { 26, 25, 4, 24, 4, 1, 14 },
        { 26, 25, 4, 24, 4, 1, 14 },
        { 10, 25, 11, 26, 8, 12, 1 },
        { 10, 25, 11, 26, 8, 12, 1 },
        { 0, 2, 5, 12, 15, 23, 16 },
        { 0, 2, 5, 12, 15, 23, 16 },
        { 2, 9, 12, 0, 15, 15, 14 },
        { 2, 9, 12, 0, 15, 15, 14 },
        { 17, 7, 5, 15, 1, 32, 28 },
        { 17, 7, 5, 15, 1, 32, 28 },
        { 11, 11, 26, 4, 4, 10, 4 },
        { 11, 11, 26, 4, 4, 10, 4 },
        { 0, 1, 5, 32, 14, 23, 29 },
        { 0, 1, 5, 32, 14, 23, 29 },
        { 32, 6, 33, 20, 29, 15, 6 },
        { 32, 6, 33, 20, 29, 15, 6 },
        { 1, 2, 12, 29, 18, 11, 13 },
        { 1, 2, 12, 29, 18, 11, 13 },
        { 6, 7, 20, 15, 26, 5, 23 },
        { 6, 7, 20, 15, 26, 5, 23 },
        { 25, 1, 5, 16, 2, 26, 29 },
        { 25, 1, 5, 16, 2, 26, 29 },
        { 1, 14, 8, 13, 17, 14, 18 },
        { 1, 14, 8, 13, 17, 14, 18 },
        { 26, 26, 4, 27, 2, 13, 24 },
        { 26, 26, 4, 27, 2, 13, 24 },
        { 23, 33, 12, 0, 25, 14, 8 },
        { 23, 33, 12, 0, 25, 14, 8 },
        { 20, 4, 33, 29, 1, 23, 29 },
        { 20, 4, 33, 29, 1, 23, 29 },
        { 31, 29, 29, 4, 2, 21, 16 },
        { 31, 29, 29, 4, 2, 21, 16 },
        { 23, 27, 32, 0, 29, 4, 4 },
        { 23, 27, 32, 0, 29, 4, 4 },
        { 7, 4, 11, 8, 8, 14, 14 },
        { 7, 4, 11, 8, 8, 14, 14 },
        { 31, 29, 5, 6, 26, 20, 16 },
        { 31, 29, 5, 6, 26, 20, 16 },
        { 32, 29, 34, 5, 6, 6, 26 },
        { 32, 29, 34, 5, 6, 6, 26 },
        { 26, 26, 22, 26, 24, 28, 19 },
        { 26, 26, 22, 26, 24, 28, 19 },
        { 32, 32, 6, 5, 33, 22, 11 },
        { 32, 32, 6, 5, 33, 22, 11 },
        { 18, 7, 15, 23, 17, 21, 28 },
        { 18, 7, 15, 23, 17, 21, 28 },
        { 23, 11, 8, 26, 10, 7, 7 },
        { 23, 11, 8, 26, 10, 7, 7 },
        { 14, 15, 12, 7, 33, 17, 13 },
        { 14, 15, 12, 7, 33, 17, 13 },
        { 25, 16, 8, 11, 13, 0, 6 },
        { 25, 16, 8, 11, 13, 0, 6 },
        { 26, 26, -1, 11, 4, -1, -1 },
        { 26, 26, -1, 11, 4, -1, -1 },
        { 29, 30, 26, 18, 5, 20, 4 },
        { 29, 30, 26, 18, 5, 20, 4 },
        { 14, 10, 10, 32, 9, 3, 26 },
        { 14, 10, 10, 32, 9, 3, 26 },
        { 26, 4, 1, 29, 29, 22, 2 },
        { 26, 4, 1, 29, 29, 22, 2 },
        { 32, 28, 33, 24, 33, 8, 13 },
        { 32, 28, 33, 24, 33, 8, 13 },
        { 22, 31, 11, 17, 15, 12, 23 },
        { 22, 31, 11, 17, 15, 12, 23 },
        { 13, 33, 23, 4, 34, 33, 17 },
        { 13, 33, 23, 4, 34, 33, 17 },
        { 25, 23, 12, 27, 30, 15, 5 },
        { 25, 23, 12, 27, 30, 15, 5 },
        { 0, 7, 13, 12, 29, 16, 33 },
        { 0, 7, 13, 12, 29, 16, 33 },
        { 20, 26, 15, 4, 0, 31, 19 },
        { 20, 26, 15, 4, 0, 31, 19 },
        { 7, 4, 4, 23, 33, 15, 26 },
        { 7, 4, 4, 23, 33, 15, 26 },
        { 13, 12, 26, 0, 20, 0, 3 },
        { 13, 12, 26, 0, 20, 0, 3 },
        { 14, 1, 10, 3, 27, 11, 26 },
        { 14, 1, 10, 3, 27, 11, 26 },
        { 14, 1, 4, 26, 3, 4, 8 },
        { 14, 1, 4, 26, 3, 4, 8 },
        { 20, 29, 6, 28, 16, 29, 26 },
        { 20, 29, 6, 28, 16, 29, 26 },
        { 2, 9, 4, 13, 15, 12, 26 },
        { 2, 9, 4, 13, 15, 12, 26 },
        { 32, 15, 17, 26, 6, 30, 10 },
        { 32, 15, 17, 26, 6, 30, 10 },
    };
    static constexpr float kModelThr[kModelTrees][7] = {
        { 7.22815335e-01f, 2.39744911e+01f, 1.82572618e-01f, 3.82486987e-03f, 7.43589699e-01f, 2.39744911e+01f, 2.50000000e+01f },
        { 7.22815335e-01f, 2.39744911e+01f, 1.82572618e-01f, 3.82486987e-03f, 7.43589699e-01f, 2.39744911e+01f, 2.50000000e+01f },
        { 2.03907996e-01f, 7.05128238e-02f, 2.94117641e+01f, 1.97916672e-01f, 3.00002995e+01f, 5.10204071e-03f, 4.33690190e-01f },
        { 2.03907996e-01f, 7.05128238e-02f, 2.94117641e+01f, 1.97916672e-01f, 3.00002995e+01f, 5.10204071e-03f, 4.33690190e-01f },
        { 1.77779719e-01f, 2.10191086e-01f, 7.11114466e-01f, 2.27282107e-01f, 1.16102427e-01f, 2.53994763e-01f, 2.50000000e+01f },
        { 1.77779719e-01f, 2.10191086e-01f, 7.11114466e-01f, 2.27282107e-01f, 1.16102427e-01f, 2.53994763e-01f, 2.50000000e+01f },
        { 5.10204071e-03f, 2.33479813e-01f, 2.50000000e+01f, 3.86348891e+00f, 1.19639620e-01f, 1.13932288e-03f, 3.43064517e-01f },
        { 5.10204071e-03f, 2.33479813e-01f, 2.50000000e+01f, 3.86348891e+00f, 1.19639620e-01f, 1.13932288e-03f, 3.43064517e-01f },
        { 2.99119502e-01f, 3.03030300e+01f, 2.50000000e+01f, 5.10204071e-03f, 3.05247247e-01f, 2.38358900e-02f, 6.52626082e-02f },
        { 2.99119502e-01f, 3.03030300e+01f, 2.50000000e+01f, 5.10204071e-03f, 3.05247247e-01f, 2.38358900e-02f, 6.52626082e-02f },
        { 2.68609285e-01f, 2.39744911e+01f, 2.24181747e+00f, 9.61725891e-01f, 7.44680822e-01f, 8.33485723e-02f, 8.79866928e-02f },
        { 2.68609285e-01f, 2.39744911e+01f, 2.24181747e+00f, 9.61725891e-01f, 7.44680822e-01f, 8.33485723e-02f, 8.79866928e-02f },
        { 1.43004924e-01f, 3.03030300e+01f, 5.98597348e-01f, 5.05192280e-01f, 3.85603547e-01f, 2.38095245e+01f, 3.43844676e+00f },
        { 1.43004924e-01f, 3.03030300e+01f, 5.98597348e-01f, 5.05192280e-01f, 3.85603547e-01f, 2.38095245e+01f, 3.43844676e+00f },
        { 1.38396323e-01f, 6.25287592e-02f, 2.38095245e+01f, 4.57784027e-01f, 1.97916672e-01f, 1.16102427e-01f, 3.10974598e-01f },
        { 1.38396323e-01f, 6.25287592e-02f, 2.38095245e+01f, 4.57784027e-01f, 1.97916672e-01f, 1.16102427e-01f, 3.10974598e-01f },
        { 3.03030300e+01f, 2.00737850e-03f, 7.15707827e+00f, 8.57917023e+00f, 5.52129269e-01f, 7.44680822e-01f, 0.00000000e+00f },
        { 3.03030300e+01f, 2.00737850e-03f, 7.15707827e+00f, 8.57917023e+00f, 5.52129269e-01f, 7.44680822e-01f, 0.00000000e+00f },
        { 4.48845923e-02f, 6.26164794e-01f, 5.84126997e+00f, 3.03030300e+01f, 1.88371074e+00f, 9.39393938e-01f, 5.62674701e-01f },
        { 4.48845923e-02f, 6.26164794e-01f, 5.84126997e+00f, 3.03030300e+01f, 1.88371074e+00f, 9.39393938e-01f, 5.62674701e-01f },
        { 3.03030300e+01f, 2.06195295e-01f, 7.44680822e-01f, 1.13932288e-03f, 5.25759637e-01f, 7.41400838e-01f, 3.73499215e-01f },
        { 3.03030300e+01f, 2.06195295e-01f, 7.44680822e-01f, 1.13932288e-03f, 5.25759637e-01f, 7.41400838e-01f, 3.73499215e-01f },
        { 2.39744911e+01f, 9.90581095e-01f, 3.10974598e-01f, 1.61054564e+00f, 8.46929741e+00f, 8.43755662e-01f, 8.56481493e-03f },
        { 2.39744911e+01f, 9.90581095e-01f, 3.10974598e-01f, 1.61054564e+00f, 8.46929741e+00f, 8.43755662e-01f, 8.56481493e-03f },
        { 6.87625408e+00f, 2.44791672e-01f, 5.27765155e-01f, 1.94444448e-01f, 2.50000000e+01f, 2.39744911e+01f, 2.94117641e+01f },
        { 6.87625408e+00f, 2.44791672e-01f, 5.27765155e-01f, 1.94444448e-01f, 2.50000000e+01f, 2.39744911e+01f, 2.94117641e+01f },
        { 5.10204071e-03f, 1.94444448e-01f, 1.01175942e-01f, 2.50000000e+01f, 9.47482646e-01f, 3.03030300e+01f, 8.38469982e+00f },
        { 5.10204071e-03f, 1.94444448e-01f, 1.01175942e-01f, 2.50000000e+01f, 9.47482646e-01f, 3.03030300e+01f, 8.38469982e+00f },
        { 3.76749665e-01f, 9.57446814e-01f, 2.50000000e+01f, 2.10286453e-01f, 9.90581095e-01f, 4.48983818e-01f, 1.80532515e+00f },
        { 3.76749665e-01f, 9.57446814e-01f, 2.50000000e+01f, 2.10286453e-01f, 9.90581095e-01f, 4.48983818e-01f, 1.80532515e+00f },
        { 3.03030300e+01f, 5.08597732e-01f, 7.44680822e-01f, 9.80767131e-01f, 4.25763875e-01f, 2.99119502e-01f, 6.37680769e-01f },
        { 3.03030300e+01f, 5.08597732e-01f, 7.44680822e-01f, 9.80767131e-01f, 4.25763875e-01f, 2.99119502e-01f, 6.37680769e-01f },
        { 6.05357468e-01f, 2.44791672e-01f, 7.79861137e-02f, 5.69992065e-02f, 8.26958537e-01f, 5.05499840e-01f, 3.99184460e-03f },
        { 6.05357468e-01f, 2.44791672e-01f, 7.79861137e-02f, 5.69992065e-02f, 8.26958537e-01f, 5.05499840e-01f, 3.99184460e-03f },
        { 3.52428627e+00f, 7.59259284e-01f, 2.39744911e+01f, 8.02978516e-01f, 2.83839798e+00f, 1.56634188e+00f, 5.10204071e-03f },
        { 3.52428627e+00f, 7.59259284e-01f, 2.39744911e+01f, 8.02978516e-01f, 2.83839798e+00f, 1.56634188e+00f, 5.10204071e-03f },
        { 2.11912185e-01f, 1.95590281e+00f, 2.39744911e+01f, 1.87500000e-01f, 1.01175942e-01f, 5.14696002e-01f, 2.94117641e+01f },
        { 2.11912185e-01f, 1.95590281e+00f, 2.39744911e+01f, 1.87500000e-01f, 1.01175942e-01f, 5.14696002e-01f, 2.94117641e+01f },
        { 3.18362498e+00f, 9.94086385e-01f, 8.97214711e-01f, 2.53761321e-01f, 1.99750006e-01f, 7.15707827e+00f, 9.20871317e-01f },
        { 3.18362498e+00f, 9.94086385e-01f, 8.97214711e-01f, 2.53761321e-01f, 1.99750006e-01f, 7.15707827e+00f, 9.20871317e-01f },
        { 3.03030300e+01f, 2.47526526e+00f, 1.71432221e+00f, 1.87500000e-01f, 1.39252448e+00f, 5.59257126e+00f, 7.41400838e-01f },
        { 3.03030300e+01f, 2.47526526e+00f, 1.71432221e+00f, 1.87500000e-01f, 1.39252448e+00f, 5.59257126e+00f, 7.41400838e-01f },
        { 7.81393722e-02f, 5.67311347e-01f, 1.58154845e-01f, 2.50000000e+01f, 3.42000008e-01f, 5.98597348e-01f, 2.24181747e+00f },
        { 7.81393722e-02f, 5.67311347e-01f, 1.58154845e-01f, 2.50000000e+01f, 3.42000008e-01f, 5.98597348e-01f, 2.24181747e+00f },
        { 1.23252355e-01f, 9.97480989e-01f, 1.00000000e+00f, 2.17999995e-01f, 3.55250001e-01f, 2.38095245e+01f, 9.75460112e-01f },
        { 1.23252355e-01f, 9.97480989e-01f, 1.00000000e+00f, 2.17999995e-01f, 3.55250001e-01f, 2.38095245e+01f, 9.75460112e-01f },
        { 9.57446814e-01f, 7.03355837e+00f, 5.10204071e-03f, 5.69992065e-02f, 9.54698324e-01f, 6.91077054e-01f, 9.83423710e+00f },
        { 9.57446814e-01f, 7.03355837e+00f, 5.10204071e-03f, 5.69992065e-02f, 9.54698324e-01f, 6.91077054e-01f, 9.83423710e+00f },
        { 2.91655970e+00f, 5.96491218e-01f, 2.50000000e-01f, 1.87500000e-01f, 2.50060940e+00f, 8.57917023e+00f, 2.38095245e+01f },
        { 2.91655970e+00f, 5.96491218e-01f, 2.50000000e-01f, 1.87500000e-01f, 2.50060940e+00f, 8.57917023e+00f, 2.38095245e+01f },
        { 6.95827901e-01f, 5.40492356e-01f, 4.27517369e-02f, 4.93227422e-01f, 2.94117641e+01f, 9.74499226e-01f, 9.09824595e-02f },
        { 6.95827901e-01f, 5.40492356e-01f, 4.27517369e-02f, 4.93227422e-01f, 2.94117641e+01f, 9.74499226e-01f, 9.09824595e-02f },
        { 5.08597732e-01f, 9.75151896e-01f, 3.00002995e+01f, 2.50000000e-01f, 9.39393938e-01f, 2.50000000e+01f, 1.77630544e+00f },
        { 5.08597732e-01f, 9.75151896e-01f, 3.00002995e+01f, 2.50000000e-01f, 9.39393938e-01f, 2.50000000e+01f, 1.77630544e+00f },
        { 1.94444448e-01f, 2.94117641e+01f, 1.51777995e+00f, 2.39744911e+01f, 3.03030300e+01f, 2.74960303e+00f, 2.39744911e+01f },
        { 1.94444448e-01f, 2.94117641e+01f, 1.51777995e+00f, 2.39744911e+01f, 3.03030300e+01f, 2.74960303e+00f, 2.39744911e+01f },
        { 5.99597216e+00f, 2.50000000e+01f, 3.22124988e-01f, 1.64917612e+00f, 4.93580729e-01f, 8.70279026e+00f, 2.78135329e-01f },
        { 5.99597216e+00f, 2.50000000e+01f, 3.22124988e-01f, 1.64917612e+00f, 4.93580729e-01f, 8.70279026e+00f, 2.78135329e-01f },
        { 5.18195927e-01f, 5.10204071e-03f, 4.48983818e-01f, 3.03030300e+01f, 2.88000107e-01f, 5.72518885e-01f, 4.03100789e-01f },
        { 5.18195927e-01f, 5.10204071e-03f, 4.48983818e-01f, 3.03030300e+01f, 2.88000107e-01f, 5.72518885e-01f, 4.03100789e-01f },
        { 3.03030300e+01f, 2.16147256e+00f, 7.44680822e-01f, 3.52376290e-02f, 9.99620199e-01f, 6.18100464e-01f, 7.39740312e-01f },
        { 3.03030300e+01f, 2.16147256e+00f, 7.44680822e-01f, 3.52376290e-02f, 9.99620199e-01f, 6.18100464e-01f, 7.39740312e-01f },
        { 1.00000000e+00f, 7.54834652e+00f, 6.53754324e-02f, 2.74793841e-02f, 3.73352146e+00f, 3.52428627e+00f, 6.75455714e-03f },
        { 1.00000000e+00f, 7.54834652e+00f, 6.53754324e-02f, 2.74793841e-02f, 3.73352146e+00f, 3.52428627e+00f, 6.75455714e-03f },
        { 8.24425220e+00f, 2.50000000e-01f, 4.77600396e-01f, 2.31922388e+00f, 6.62081599e-01f, 5.08597732e-01f, 3.03030300e+01f },
        { 8.24425220e+00f, 2.50000000e-01f, 4.77600396e-01f, 2.31922388e+00f, 6.62081599e-01f, 5.08597732e-01f, 3.03030300e+01f },
        { 1.94773257e-01f, 3.78763825e-01f, 2.39744911e+01f, 3.03148776e-02f, 2.41427943e-02f, 6.11556768e-01f, 2.94117641e+01f },
        { 1.94773257e-01f, 3.78763825e-01f, 2.39744911e+01f, 3.03148776e-02f, 2.41427943e-02f, 6.11556768e-01f, 2.94117641e+01f },
        { 3.57048440e+00f, 8.02978516e-01f, 2.39744911e+01f, 1.18498076e-02f, 6.15384638e-01f, 1.61054564e+00f, 1.22699384e-02f },
        { 3.57048440e+00f, 8.02978516e-01f, 2.39744911e+01f, 1.18498076e-02f, 6.15384638e-01f, 1.61054564e+00f, 1.22699384e-02f },
        { 3.01994729e+00f, 1.00000000e+00f, 6.14977062e-01f, 1.59804691e-02f, 4.20277685e-01f, 7.54834652e+00f, 9.20871317e-01f },
        { 3.01994729e+00f, 1.00000000e+00f, 6.14977062e-01f, 1.59804691e-02f, 4.20277685e-01f, 7.54834652e+00f, 9.20871317e-01f },
        { 2.74960303e+00f, 2.27999970e-01f, 8.52978155e-02f, 2.85125315e-01f, 1.71234369e-01f, 2.95534182e+00f, 1.41681805e-01f },
        { 2.74960303e+00f, 2.27999970e-01f, 8.52978155e-02f, 2.85125315e-01f, 1.71234369e-01f, 2.95534182e+00f, 1.41681805e-01f },
        { 1.04729786e+01f, 1.00000000e+00f, 3.10296798e+00f, 9.20871317e-01f, 8.42737313e-03f, 3.51580083e-01f, 8.57121170e-01f },
        { 1.04729786e+01f, 1.00000000e+00f, 3.10296798e+00f, 9.20871317e-01f, 8.42737313e-03f, 3.51580083e-01f, 8.57121170e-01f },
        { 3.03030300e+01f, 2.45111537e+00f, 4.16666657e-01f, 3.00002995e+01f, 1.39252448e+00f, 2.53994763e-01f, 1.68251359e+00f },
        { 3.03030300e+01f, 2.45111537e+00f, 4.16666657e-01f, 3.00002995e+01f, 1.39252448e+00f, 2.53994763e-01f, 1.68251359e+00f },
        { 4.93992180e-01f, 3.47291976e-01f, 2.39744911e+01f, 8.24425220e+00f, 3.88175637e-01f, 5.10204071e-03f, 8.70175779e-01f },
        { 4.93992180e-01f, 3.47291976e-01f, 2.39744911e+01f, 8.24425220e+00f, 3.88175637e-01f, 5.10204071e-03f, 8.70175779e-01f },
        { 9.57446814e-01f, 4.82859820e-01f, 1.65845704e+00f, 1.45582244e-01f, 5.69992065e-02f, 1.13932288e-03f, 3.00002995e+01f },
        { 9.57446814e-01f, 4.82859820e-01f, 1.65845704e+00f, 1.45582244e-01f, 5.69992065e-02f, 1.13932288e-03f, 3.00002995e+01f },
        { 2.28809908e-01f, 6.75455714e-03f, 3.99184460e-03f, 1.45582244e-01f, 3.15119416e-01f, 1.72254778e-02f, 1.28108263e-01f },
        { 2.28809908e-01f, 6.75455714e-03f, 3.99184460e-03f, 1.45582244e-01f, 3.15119416e-01f, 1.72254778e-02f, 1.28108263e-01f },
        { 1.04729786e+01f, 8.70279026e+00f, 8.70967746e-01f, 2.35750005e-01f, 1.68251359e+00f, 5.96516915e-02f, 2.94117641e+01f },
        { 1.04729786e+01f, 8.70279026e+00f, 8.70967746e-01f, 2.35750005e-01f, 1.68251359e+00f, 5.96516915e-02f, 2.94117641e+01f },
        { 5.29856920e-01f, 4.04248178e-01f, 3.17382812e-03f, 2.19922841e-01f, 4.16666657e-01f, 1.87500000e-01f, 5.69992065e-02f },
        { 5.29856920e-01f, 4.04248178e-01f, 3.17382812e-03f, 2.19922841e-01f, 4.16666657e-01f, 1.87500000e-01f, 5.69992065e-02f },
        { 3.03030300e+01f, 6.17346942e-01f, 5.55555582e-01f, 3.99184460e-03f, 1.77630544e+00f, 6.02006167e-03f, 9.41233337e-02f },
        { 3.03030300e+01f, 6.17346942e-01f, 5.55555582e-01f, 3.99184460e-03f, 1.77630544e+00f, 6.02006167e-03f, 9.41233337e-02f },
        { 2.97230124e+00f, 3.14819789e+00f, 2.94117641e+01f, 9.80767131e-01f, 2.17999995e-01f, 9.45440382e-02f, 6.80582702e-01f },
        { 2.97230124e+00f, 3.14819789e+00f, 2.94117641e+01f, 9.80767131e-01f, 2.17999995e-01f, 9.45440382e-02f, 6.80582702e-01f },
        { 6.95827901e-01f, 7.98027992e-01f, 2.46358663e-01f, 2.94117659e-01f, 2.94117641e+01f, 2.39744911e+01f, 7.62012815e+00f },
        { 6.95827901e-01f, 7.98027992e-01f, 2.46358663e-01f, 2.94117659e-01f, 2.94117641e+01f, 2.39744911e+01f, 7.62012815e+00f },
        { 6.45826864e+00f, 2.50000000e+01f, 3.22124988e-01f, 9.98562276e-01f, 1.40594378e-01f, 9.75460112e-01f, 2.79961020e-01f },
        { 6.45826864e+00f, 2.50000000e+01f, 3.22124988e-01f, 9.98562276e-01f, 1.40594378e-01f, 9.75460112e-01f, 2.79961020e-01f },
        { 8.23936462e-01f, 1.97094437e-02f, 7.58934200e-01f, 1.90635729e+00f, 1.76085569e-02f, 2.41427943e-02f, 2.00000003e-01f },
        { 8.23936462e-01f, 1.97094437e-02f, 7.58934200e-01f, 1.90635729e+00f, 1.76085569e-02f, 2.41427943e-02f, 2.00000003e-01f },
        { 3.55360238e-03f, 4.93580729e-01f, 1.00000000e+00f, 9.86507833e-01f, 4.48386252e-01f, 6.22658014e-01f, 7.80921936e-01f },
        { 3.55360238e-03f, 4.93580729e-01f, 1.00000000e+00f, 9.86507833e-01f, 4.48386252e-01f, 6.22658014e-01f, 7.80921936e-01f },
        { 2.74960303e+00f, 2.27999970e-01f, 3.68098170e-02f, 4.70902443e+00f, 8.09362888e-01f, 9.39824402e-01f, 2.24181747e+00f },
        { 2.74960303e+00f, 2.27999970e-01f, 3.68098170e-02f, 4.70902443e+00f, 8.09362888e-01f, 9.39824402e-01f, 2.24181747e+00f },
        { 9.47482646e-01f, 5.52129269e-01f, 7.61503875e-01f, 1.45582244e-01f, 2.91344989e-02f, 8.22367370e-01f, 2.91655970e+00f },
        { 9.47482646e-01f, 5.52129269e-01f, 7.61503875e-01f, 1.45582244e-01f, 2.91344989e-02f, 8.22367370e-01f, 2.91655970e+00f },
        { 3.47291976e-01f, 8.20361823e-02f, 4.41887528e-02f, 4.82859820e-01f, 1.97090253e-01f, 1.77779719e-01f, 4.39364105e-01f },
        { 3.47291976e-01f, 8.20361823e-02f, 4.41887528e-02f, 4.82859820e-01f, 1.97090253e-01f, 1.77779719e-01f, 4.39364105e-01f },
        { 6.15384638e-01f, 2.27999970e-01f, 2.11886942e-01f, 2.95069265e+00f, 2.68012166e-01f, 2.10286453e-01f, 2.34304592e-01f },
        { 6.15384638e-01f, 2.27999970e-01f, 2.11886942e-01f, 2.95069265e+00f, 2.68012166e-01f, 2.10286453e-01f, 2.34304592e-01f },
        { 8.70967746e-01f, 6.29339170e+00f, 1.99830793e-02f, 3.35618114e+00f, 2.91174603e+00f, 2.39744911e+01f, 1.90429688e-02f },
        { 8.70967746e-01f, 6.29339170e+00f, 1.99830793e-02f, 3.35618114e+00f, 2.91174603e+00f, 2.39744911e+01f, 1.90429688e-02f },
        { 1.23252355e-01f, 9.97480989e-01f, 1.53846160e-01f, 7.47035682e-01f, 3.55250001e-01f, 2.02013552e-01f, 2.01699305e+00f },
        { 1.23252355e-01f, 9.97480989e-01f, 1.53846160e-01f, 7.47035682e-01f, 3.55250001e-01f, 2.02013552e-01f, 2.01699305e+00f },
        { 5.50404251e-01f, 2.03907996e-01f, 5.69992065e-02f, 1.40177412e-02f, 2.14746308e+00f, 1.97916672e-01f, 1.57232001e-01f },
        { 5.50404251e-01f, 2.03907996e-01f, 5.69992065e-02f, 1.40177412e-02f, 2.14746308e+00f, 1.97916672e-01f, 1.57232001e-01f },
        { 2.45111537e+00f, 1.59804691e-02f, 2.40881965e-01f, 1.14200926e+01f, 3.03030300e+01f, 7.29623973e-01f, 6.22658014e-01f },
        { 2.45111537e+00f, 1.59804691e-02f, 2.40881965e-01f, 1.14200926e+01f, 3.03030300e+01f, 7.29623973e-01f, 6.22658014e-01f },
        { 3.03030300e+01f, 1.87326252e-01f, 1.77630544e+00f, 3.14819789e+00f, 6.32125020e-01f, 1.13281250e-01f, 0.00000000e+00f },
        { 3.03030300e+01f, 1.87326252e-01f, 1.77630544e+00f, 3.14819789e+00f, 6.32125020e-01f, 1.13281250e-01f, 0.00000000e+00f },
        { 3.85603547e-01f, 2.03907996e-01f, 1.92764568e+00f, 2.08528662e+00f, 1.88371074e+00f, 4.77600098e-01f, 5.69992065e-02f },
        { 3.85603547e-01f, 2.03907996e-01f, 1.92764568e+00f, 2.08528662e+00f, 1.88371074e+00f, 4.77600098e-01f, 5.69992065e-02f },
        { 2.39744911e+01f, 9.90581095e-01f, 3.74062508e-01f, 9.75460112e-01f, 2.40288630e-01f, 5.99082351e-01f, 6.25287592e-02f },
        { 2.39744911e+01f, 9.90581095e-01f, 3.74062508e-01f, 9.75460112e-01f, 2.40288630e-01f, 5.99082351e-01f, 6.25287592e-02f },
        { 2.73317146e+00f, 9.39393938e-01f, 1.39252448e+00f, 7.43589699e-01f, 5.45518659e-02f, 6.58136547e-01f, 2.64644146e-01f },
        { 2.73317146e+00f, 9.39393938e-01f, 1.39252448e+00f, 7.43589699e-01f, 5.45518659e-02f, 6.58136547e-01f, 2.64644146e-01f },
        { 3.03030300e+01f, 7.03355837e+00f, 8.97135437e-01f, 2.50000000e+01f, 9.80767131e-01f, 2.04795092e-01f, 4.16666657e-01f },
        { 3.03030300e+01f, 7.03355837e+00f, 8.97135437e-01f, 2.50000000e+01f, 9.80767131e-01f, 2.04795092e-01f, 4.16666657e-01f },
        { 7.54834652e+00f, 3.57048440e+00f, 3.67216897e+00f, 5.96491218e-01f, 7.65054345e-01f, 2.94117641e+01f, 2.39744911e+01f },
        { 7.54834652e+00f, 3.57048440e+00f, 3.67216897e+00f, 5.96491218e-01f, 7.65054345e-01f, 2.94117641e+01f, 2.39744911e+01f },
        { 1.23252355e-01f, 1.61054564e+00f, 1.00000000e+00f, 2.22005203e-01f, 2.03907996e-01f, 2.38095245e+01f, 2.38095245e+01f },
        { 1.23252355e-01f, 1.61054564e+00f, 1.00000000e+00f, 2.22005203e-01f, 2.03907996e-01f, 2.38095245e+01f, 2.38095245e+01f },
        { 4.82859820e-01f, 1.93684902e-02f, 3.97887737e-01f, 4.54074442e-01f, 2.79260665e-01f, 9.57446814e-01f, 4.48983818e-01f },
        { 4.82859820e-01f, 1.93684902e-02f, 3.97887737e-01f, 4.54074442e-01f, 2.79260665e-01f, 9.57446814e-01f, 4.48983818e-01f },
        { 3.87123704e-01f, 3.17382812e-03f, 2.74960303e+00f, 9.85245168e-01f, 5.94917834e-01f, 1.42857149e-01f, 7.79830933e-01f },
        { 3.87123704e-01f, 3.17382812e-03f, 2.74960303e+00f, 9.85245168e-01f, 5.94917834e-01f, 1.42857149e-01f, 7.79830933e-01f },
        { 6.87625408e+00f, 2.50000000e+01f, 2.94117641e+01f, 5.08597732e-01f, 4.18410031e-03f, 6.15384638e-01f, 5.78235924e-01f },
        { 6.87625408e+00f, 2.50000000e+01f, 2.94117641e+01f, 5.08597732e-01f, 4.18410031e-03f, 6.15384638e-01f, 5.78235924e-01f },
        { 1.79576486e-01f, 2.35750005e-01f, 4.87580942e-03f, 4.00119364e-01f, 2.67356277e-01f, 6.83740020e-01f, 5.00000000e-01f },
        { 1.79576486e-01f, 2.35750005e-01f, 4.87580942e-03f, 4.00119364e-01f, 2.67356277e-01f, 6.83740020e-01f, 5.00000000e-01f },
        { 3.22124988e-01f, 5.59257126e+00f, 6.37680769e-01f, 2.03907996e-01f, 1.71116263e-01f, 4.41717803e-01f, 2.50000000e+01f },
        { 3.22124988e-01f, 5.59257126e+00f, 6.37680769e-01f, 2.03907996e-01f, 1.71116263e-01f, 4.41717803e-01f, 2.50000000e+01f },
        { 5.10204071e-03f, 3.03030300e+01f, 1.42857149e-01f, 2.39744911e+01f, 1.90635729e+00f, 3.03030300e+01f, 5.69992065e-02f },
        { 5.10204071e-03f, 3.03030300e+01f, 1.42857149e-01f, 2.39744911e+01f, 1.90635729e+00f, 3.03030300e+01f, 5.69992065e-02f },
        { 8.70967746e-01f, 5.18195927e-01f, 1.56792533e-02f, 6.32125020e-01f, 9.74499226e-01f, 1.95590281e+00f, 3.03030300e+01f },
        { 8.70967746e-01f, 5.18195927e-01f, 1.56792533e-02f, 6.32125020e-01f, 9.74499226e-01f, 1.95590281e+00f, 3.03030300e+01f },
        { 2.91655970e+00f, 2.90066183e-01f, 1.39252448e+00f, 9.27218974e-01f, 1.08975731e-01f, 5.09140372e-01f, 9.75460112e-01f },
        { 2.91655970e+00f, 2.90066183e-01f, 1.39252448e+00f, 9.27218974e-01f, 1.08975731e-01f, 5.09140372e-01f, 9.75460112e-01f },
        { 2.38095245e+01f, 0.00000000e+00f, 6.58136547e-01f, 0.00000000e+00f, 0.00000000e+00f, 2.43589744e-01f, 2.39744911e+01f },
        { 2.38095245e+01f, 0.00000000e+00f, 6.58136547e-01f, 0.00000000e+00f, 0.00000000e+00f, 2.43589744e-01f, 2.39744911e+01f },
        { 2.73317146e+00f, 1.59804691e-02f, 9.20871317e-01f, 2.50000000e+01f, 1.00000000e+00f, 9.75460112e-01f, 2.41427943e-02f },
        { 2.73317146e+00f, 1.59804691e-02f, 9.20871317e-01f, 2.50000000e+01f, 1.00000000e+00f, 9.75460112e-01f, 2.41427943e-02f },
        { 8.70967746e-01f, 3.03030300e+01f, 5.01844613e-03f, 2.50000000e+01f, 4.18410031e-03f, 1.53846160e-01f, 1.37109131e-01f },
        { 8.70967746e-01f, 3.03030300e+01f, 5.01844613e-03f, 2.50000000e+01f, 4.18410031e-03f, 1.53846160e-01f, 1.37109131e-01f },
        { 8.79866928e-02f, 6.75455714e-03f, 8.81347656e-02f, 2.56211805e+00f, 2.39744911e+01f, 1.87500000e-01f, 1.80532515e+00f },
        { 8.79866928e-02f, 6.75455714e-03f, 8.81347656e-02f, 2.56211805e+00f, 2.39744911e+01f, 1.87500000e-01f, 1.80532515e+00f },
        { 3.03030300e+01f, 6.17346942e-01f, 1.74747121e+00f, 5.00000000e-01f, 1.85867417e+00f, 4.98037624e+00f, 3.47884989e+00f },
        { 3.03030300e+01f, 6.17346942e-01f, 1.74747121e+00f, 5.00000000e-01f, 1.85867417e+00f, 4.98037624e+00f, 3.47884989e+00f },
        { 6.28173828e-01f, 5.10204071e-03f, 7.03355837e+00f, 2.39744911e+01f, 2.82281375e+00f, 3.73976737e-01f, 3.30982518e+00f },
        { 6.28173828e-01f, 5.10204071e-03f, 7.03355837e+00f, 2.39744911e+01f, 2.82281375e+00f, 3.73976737e-01f, 3.30982518e+00f },
        { 3.55360238e-03f, 4.93580729e-01f, 3.25520843e-04f, 3.88175637e-01f, 2.40288630e-01f, 5.77734746e-02f, 8.97214711e-01f },
        { 3.55360238e-03f, 4.93580729e-01f, 3.25520843e-04f, 3.88175637e-01f, 2.40288630e-01f, 5.77734746e-02f, 8.97214711e-01f },
        { 7.45377839e-01f, 9.86507833e-01f, 3.45954537e-01f, 1.47405127e-02f, 1.81667179e-01f, 1.16102427e-01f, 3.43844676e+00f },
        { 7.45377839e-01f, 9.86507833e-01f, 3.45954537e-01f, 1.47405127e-02f, 1.81667179e-01f, 1.16102427e-01f, 3.43844676e+00f },
        { 3.20513695e-01f, 4.34437513e-01f, 1.62760422e-04f, 3.79774312e-04f, 2.50060940e+00f, 7.12461993e-02f, 2.28809908e-01f },
        { 3.20513695e-01f, 4.34437513e-01f, 1.62760422e-04f, 3.79774312e-04f, 2.50060940e+00f, 7.12461993e-02f, 2.28809908e-01f },
        { 9.83423710e+00f, 9.02133656e+00f, 3.00002995e+01f, 2.24181747e+00f, 1.71432221e+00f, 1.00000000e+00f, 1.18835747e+00f },
        { 9.83423710e+00f, 9.02133656e+00f, 3.00002995e+01f, 2.24181747e+00f, 1.71432221e+00f, 1.00000000e+00f, 1.18835747e+00f },
        { 1.93684902e-02f, 3.86348891e+00f, 1.13932288e-03f, 1.10152490e-01f, 2.91655970e+00f, 4.87580942e-03f, 2.91344989e-02f },
        { 1.93684902e-02f, 3.86348891e+00f, 1.13932288e-03f, 1.10152490e-01f, 2.91655970e+00f, 4.87580942e-03f, 2.91344989e-02f },
        { 3.85603547e-01f, 1.70410156e-01f, 5.69992065e-02f, 6.91077054e-01f, 1.34907722e-01f, 1.55865699e-01f, 2.22357854e-01f },
        { 3.85603547e-01f, 1.70410156e-01f, 5.69992065e-02f, 6.91077054e-01f, 1.34907722e-01f, 1.55865699e-01f, 2.22357854e-01f },
        { 6.87625408e+00f, 2.46358663e-01f, 2.79961020e-01f, 1.19639620e-01f, 6.58136547e-01f, 8.24425220e+00f, 5.96491218e-01f },
        { 6.87625408e+00f, 2.46358663e-01f, 2.79961020e-01f, 1.19639620e-01f, 6.58136547e-01f, 8.24425220e+00f, 5.96491218e-01f },
        { 5.18195927e-01f, 4.34437513e-01f, 5.72483599e-01f, 2.53363717e-02f, 2.39744911e+01f, 9.35872365e-03f, 7.39739239e-02f },
        { 5.18195927e-01f, 4.34437513e-01f, 5.72483599e-01f, 2.53363717e-02f, 2.39744911e+01f, 9.35872365e-03f, 7.39739239e-02f },
        { 1.53846160e-01f, 3.50907969e+00f, 2.74793841e-02f, 7.61503875e-01f, 4.93580729e-01f, 3.03030300e+01f, 1.19639620e-01f },
        { 1.53846160e-01f, 3.50907969e+00f, 2.74793841e-02f, 7.61503875e-01f, 4.93580729e-01f, 3.03030300e+01f, 1.19639620e-01f },
        { 6.87625408e+00f, 3.57048440e+00f, 2.97653842e+00f, 5.96491218e-01f, 5.52129269e-01f, 3.22480273e+00f, 9.75341439e-01f },
        { 6.87625408e+00f, 3.57048440e+00f, 2.97653842e+00f, 5.96491218e-01f, 5.52129269e-01f, 3.22480273e+00f, 9.75341439e-01f },
        { 3.03030300e+01f, 2.50000000e+01f, 1.85867417e+00f, 6.83740020e-01f, 6.25287592e-02f, 4.71881390e-01f, 2.27272734e-01f },
        { 3.03030300e+01f, 2.50000000e+01f, 1.85867417e+00f, 6.83740020e-01f, 6.25287592e-02f, 4.71881390e-01f, 2.27272734e-01f },
        { 3.47291976e-01f, 1.88600016e+00f, 4.39364105e-01f, 9.36101303e-02f, 5.10204071e-03f, 2.56211805e+00f, 2.94606924e+00f },
        { 3.47291976e-01f, 1.88600016e+00f, 4.39364105e-01f, 9.36101303e-02f, 5.10204071e-03f, 2.56211805e+00f, 2.94606924e+00f },
        { 7.22815335e-01f, 1.90635729e+00f, 5.69992065e-02f, 1.28108263e-01f, 3.30982518e+00f, 1.97094437e-02f, 1.07944429e-01f },
        { 7.22815335e-01f, 1.90635729e+00f, 5.69992065e-02f, 1.28108263e-01f, 3.30982518e+00f, 1.97094437e-02f, 1.07944429e-01f },
        { 4.48386252e-01f, 1.50023594e-01f, 1.58208996e-01f, 1.90635729e+00f, 4.70784515e-01f, 3.87123704e-01f, 9.46807444e-01f },
        { 4.48386252e-01f, 1.50023594e-01f, 1.58208996e-01f, 1.90635729e+00f, 4.70784515e-01f, 3.87123704e-01f, 9.46807444e-01f },
        { 3.03148776e-02f, 7.53722370e-01f, 1.56337425e-01f, 8.70225728e-02f, 1.40594378e-01f, 2.43809223e+00f, 2.24181747e+00f },
        { 3.03148776e-02f, 7.53722370e-01f, 1.56337425e-01f, 8.70225728e-02f, 1.40594378e-01f, 2.43809223e+00f, 2.24181747e+00f },
        { 1.59804691e-02f, 2.24181747e+00f, 1.14200926e+01f, 2.83921528e+00f, 2.85362434e+00f, 1.19374979e+00f, 2.56211805e+00f },
        { 1.59804691e-02f, 2.24181747e+00f, 1.14200926e+01f, 2.83921528e+00f, 2.85362434e+00f, 1.19374979e+00f, 2.56211805e+00f },
        { 3.94726276e-01f, 1.54271901e-01f, 1.03895403e-02f, 2.78135329e-01f, 2.39744911e+01f, 2.82384664e-01f, 9.20199156e-01f },
        { 3.94726276e-01f, 1.54271901e-01f, 1.03895403e-02f, 2.78135329e-01f, 2.39744911e+01f, 2.82384664e-01f, 9.20199156e-01f },
        { 5.50404251e-01f, 3.99184460e-03f, 5.55590332e-01f, 1.03895403e-02f, 1.70410156e-01f, 2.10286453e-01f, 2.50000000e+01f },
        { 5.50404251e-01f, 3.99184460e-03f, 5.55590332e-01f, 1.03895403e-02f, 1.70410156e-01f, 2.10286453e-01f, 2.50000000e+01f },
        { 3.03030300e+01f, 3.03030300e+01f, 4.16666657e-01f, 3.00002995e+01f, 4.18410031e-03f, 6.02006167e-03f, 1.71087965e-01f },
        { 3.03030300e+01f, 3.03030300e+01f, 4.16666657e-01f, 3.00002995e+01f, 4.18410031e-03f, 6.02006167e-03f, 1.71087965e-01f },
        { 3.85603547e-01f, 2.03907996e-01f, 1.97916672e-01f, 1.40177412e-02f, 5.69992065e-02f, 3.33333351e-02f, 8.57917023e+00f },
        { 3.85603547e-01f, 2.03907996e-01f, 1.97916672e-01f, 1.40177412e-02f, 5.69992065e-02f, 3.33333351e-02f, 8.57917023e+00f },
        { 9.95554447e-01f, 6.32125020e-01f, 3.04726493e-02f, 4.87580942e-03f, 7.47035682e-01f, 6.76944077e-01f, 5.09140372e-01f },
        { 9.95554447e-01f, 6.32125020e-01f, 3.04726493e-02f, 4.87580942e-03f, 7.47035682e-01f, 6.76944077e-01f, 5.09140372e-01f },
        { 3.47291976e-01f, 7.70254946e+00f, 2.63166118e+00f, 2.50000000e+01f, 9.75151896e-01f, 2.60749996e-01f, 3.74062508e-01f },
        { 3.47291976e-01f, 7.70254946e+00f, 2.63166118e+00f, 2.50000000e+01f, 9.75151896e-01f, 2.60749996e-01f, 3.74062508e-01f },
        { 2.45111537e+00f, 3.79774312e-04f, 2.40881965e-01f, 1.99750006e-01f, 1.51737094e+00f, 7.14647233e-01f, 4.49907839e-01f },
        { 2.45111537e+00f, 3.79774312e-04f, 2.40881965e-01f, 1.99750006e-01f, 1.51737094e+00f, 7.14647233e-01f, 4.49907839e-01f },
        { 5.10204071e-03f, 8.43755662e-01f, 2.96412277e+00f, 9.21048546e+00f, 6.53469145e-01f, 1.93684902e-02f, 2.22357854e-01f },
        { 5.10204071e-03f, 8.43755662e-01f, 2.96412277e+00f, 9.21048546e+00f, 6.53469145e-01f, 1.93684902e-02f, 2.22357854e-01f },
        { 1.17495003e+02f, 3.03030300e+01f, 0.00000000e+00f, 1.00485983e+01f, 1.80532515e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { 1.17495003e+02f, 3.03030300e+01f, 0.00000000e+00f, 1.00485983e+01f, 1.80532515e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { 1.94773257e-01f, 1.13245331e-01f, 2.39744911e+01f, 2.76796401e-01f, 9.35872365e-03f, 5.51199913e-01f, 1.61054564e+00f },
        { 1.94773257e-01f, 1.13245331e-01f, 2.39744911e+01f, 2.76796401e-01f, 9.35872365e-03f, 5.51199913e-01f, 1.61054564e+00f },
        { 3.57048440e+00f, 9.94086385e-01f, 1.00000000e+00f, 1.76085569e-02f, 7.79830933e-01f, 6.16590738e-01f, 2.38095245e+01f },
        { 3.57048440e+00f, 9.94086385e-01f, 1.00000000e+00f, 1.76085569e-02f, 7.79830933e-01f, 6.16590738e-01f, 2.38095245e+01f },
        { 3.03030300e+01f, 1.77630544e+00f, 4.98037624e+00f, 1.28108263e-01f, 2.91344989e-02f, 7.44680822e-01f, 3.90462250e-01f },
        { 3.03030300e+01f, 1.77630544e+00f, 4.98037624e+00f, 1.28108263e-01f, 2.91344989e-02f, 7.44680822e-01f, 3.90462250e-01f },
        { 4.33690190e-01f, 7.21541941e-01f, 5.69992065e-02f, 1.00000001e-01f, 5.69992065e-02f, 2.63166118e+00f, 5.96491218e-01f },
        { 4.33690190e-01f, 7.21541941e-01f, 5.69992065e-02f, 1.00000001e-01f, 5.69992065e-02f, 2.63166118e+00f, 5.96491218e-01f },
        { 9.57446814e-01f, 3.82497847e-01f, 7.25296879e+00f, 7.47035682e-01f, 2.53363717e-02f, 3.88175637e-01f, 1.06509961e-01f },
        { 9.57446814e-01f, 3.82497847e-01f, 7.25296879e+00f, 7.47035682e-01f, 2.53363717e-02f, 3.88175637e-01f, 1.06509961e-01f },
        { 5.96491218e-01f, 5.69992065e-02f, 1.80999517e-01f, 1.92764568e+00f, 7.24008203e-01f, 5.69992065e-02f, 6.57555938e-01f },
        { 5.96491218e-01f, 5.69992065e-02f, 1.80999517e-01f, 1.92764568e+00f, 7.24008203e-01f, 5.69992065e-02f, 6.57555938e-01f },
        { 5.08597732e-01f, 3.46162096e-02f, 7.43589699e-01f, 7.39740312e-01f, 8.79866928e-02f, 4.78786901e-02f, 3.17382812e-03f },
        { 5.08597732e-01f, 3.46162096e-02f, 7.43589699e-01f, 7.39740312e-01f, 8.79866928e-02f, 4.78786901e-02f, 3.17382812e-03f },
        { 5.45518659e-02f, 1.71234369e-01f, 9.75460112e-01f, 3.88175637e-01f, 2.91344989e-02f, 7.98027992e-01f, 5.69992065e-02f },
        { 5.45518659e-02f, 1.71234369e-01f, 9.75460112e-01f, 3.88175637e-01f, 2.91344989e-02f, 7.98027992e-01f, 5.69992065e-02f },
        { 4.04248178e-01f, 3.03030300e+01f, 6.53754324e-02f, 1.96986043e+00f, 1.28336594e-01f, 4.82859820e-01f, 5.38036823e-02f },
        { 4.04248178e-01f, 3.03030300e+01f, 6.53754324e-02f, 1.96986043e+00f, 1.28336594e-01f, 4.82859820e-01f, 5.38036823e-02f },
        { 4.93992180e-01f, 2.14746308e+00f, 2.11382842e+00f, 2.65411232e-02f, 2.88000107e-01f, 1.99830793e-02f, 3.00002995e+01f },
        { 4.93992180e-01f, 2.14746308e+00f, 2.11382842e+00f, 2.65411232e-02f, 2.88000107e-01f, 1.99830793e-02f, 3.00002995e+01f },
        { 9.06976759e-01f, 7.43589699e-01f, 3.03030300e+01f, 1.56250000e-01f, 6.14965975e-01f, 1.00036621e-01f, 5.46641052e-01f },
        { 9.06976759e-01f, 7.43589699e-01f, 3.03030300e+01f, 1.56250000e-01f, 6.14965975e-01f, 1.00036621e-01f, 5.46641052e-01f },
        { 3.57048440e+00f, 7.54834652e+00f, 1.00000000e+00f, 3.15119416e-01f, 4.52361375e-01f, 1.00485983e+01f, 2.38095245e+01f },
        { 3.57048440e+00f, 7.54834652e+00f, 1.00000000e+00f, 3.15119416e-01f, 4.52361375e-01f, 1.00485983e+01f, 2.38095245e+01f },
        { 3.18362498e+00f, 6.65183210e+00f, 2.43809223e+00f, 3.03030300e+01f, 4.51877177e-01f, 1.94822180e+00f, 2.83921528e+00f },
        { 3.18362498e+00f, 6.65183210e+00f, 2.43809223e+00f, 3.03030300e+01f, 4.51877177e-01f, 1.94822180e+00f, 2.83921528e+00f },
        { 7.22815335e-01f, 1.45582244e-01f, 2.22357854e-01f, 3.16339970e-01f, 8.63592207e-01f, 9.09824595e-02f, 2.50000000e+01f },
        { 7.22815335e-01f, 1.45582244e-01f, 2.22357854e-01f, 3.16339970e-01f, 8.63592207e-01f, 9.09824595e-02f, 2.50000000e+01f },
        { 7.45377839e-01f, 9.86507833e-01f, 1.83261979e+00f, 1.94444448e-01f, 2.40288630e-01f, 7.43589699e-01f, 2.38095245e+01f },
        { 7.45377839e-01f, 9.86507833e-01f, 1.83261979e+00f, 1.94444448e-01f, 2.40288630e-01f, 7.43589699e-01f, 2.38095245e+01f },
        { 5.50404251e-01f, 2.53363717e-02f, 6.31218135e-01f, 2.38095245e+01f, 3.18006724e-01f, 1.13245331e-01f, 9.68505859e-01f },
        { 5.50404251e-01f, 2.53363717e-02f, 6.31218135e-01f, 2.38095245e+01f, 3.18006724e-01f, 1.13245331e-01f, 9.68505859e-01f },
    };
    static constexpr float kModelLeaf[kModelTrees][8] = {
        { 1.07934931e+00f, -8.78369616e-01f, 1.57408824e+00f, -9.72046889e-01f, -4.95176849e-01f, 1.33000000e+00f, -1.72916667e+00f, -8.37177748e-01f },
        { -1.07934931e+00f, 8.78369616e-01f, -1.57408824e+00f, 9.72046889e-01f, 4.95176849e-01f, -1.33000000e+00f, 1.72916667e+00f, 8.37177748e-01f },
        { 1.66535602e-01f, 1.24057968e+00f, -6.33786272e-01f, 1.19291611e+00f, -3.74945375e-01f, -1.36648125e+00f, 8.84261796e-01f, -1.60203574e+00f },
        { -1.66535602e-01f, -1.24057968e+00f, 6.33786272e-01f, -1.19291611e+00f, 3.74945375e-01f, 1.36648125e+00f, -8.84261796e-01f, 1.60203574e+00f },
        { 8.79413576e-01f, -4.79511524e-01f, -1.10312089e+00f, 2.72266525e-01f, -7.00247622e-01f, -1.35974701e+00f, -7.12777512e-01f, 9.19449942e-01f },
        { -8.79413576e-01f, 4.79511524e-01f, 1.10312089e+00f, -2.72266525e-01f, 7.00247622e-01f, 1.35974701e+00f, 7.12777512e-01f, -9.19449942e-01f },
        { 5.94092273e-01f, -6.54679574e-01f, 5.81960344e-02f, 1.00387112e+00f, 3.05753350e-01f, -1.08283514e+00f, 7.50276022e-01f, -9.91594324e-01f },
        { -5.94092273e-01f, 6.54679574e-01f, -5.81960344e-02f, -1.00387112e+00f, -3.05753350e-01f, 1.08283514e+00f, -7.50276022e-01f, 9.91594324e-01f },
        { 5.41804585e-01f, -2.31317528e-01f, 1.28522874e+00f, 1.87581712e+00f, -1.84259017e-01f, -1.24846414e+00f, -3.46127105e-01f, 1.36859487e+00f },
        { -5.41804585e-01f, 2.31317528e-01f, -1.28522874e+00f, -1.87581712e+00f, 1.84259017e-01f, 1.24846414e+00f, 3.46127105e-01f, -1.36859487e+00f },
        { -1.08293436e+00f, 3.98744457e-01f, 7.86734850e-01f, -1.60121737e+00f, -9.72349677e-01f, -3.53993538e-01f, -8.10101606e-01f, 1.17290648e+00f },
        { 1.08293436e+00f, -3.98744457e-01f, -7.86734850e-01f, 1.60121737e+00f, 9.72349677e-01f, 3.53993538e-01f, 8.10101606e-01f, -1.17290648e+00f },
        { -1.09535443e+00f, -8.90016454e-02f, 1.16106530e+00f, 1.92084791e+00f, -6.16688480e-02f, 6.02410384e-01f, -1.06955070e+00f, 2.34009293e-01f },
        { 1.09535443e+00f, 8.90016454e-02f, -1.16106530e+00f, -1.92084791e+00f, 6.16688480e-02f, -6.02410384e-01f, 1.06955070e+00f, -2.34009293e-01f },
        { -2.77222567e+00f, -1.42940541e-01f, -2.79914360e-01f, 4.83459706e-01f, -1.02913455e+00f, -2.46121520e-01f, -4.67785574e-01f, 6.09072578e-01f },
        { 2.77222567e+00f, 1.42940541e-01f, 2.79914360e-01f, -4.83459706e-01f, 1.02913455e+00f, 2.46121520e-01f, 4.67785574e-01f, -6.09072578e-01f },
        { -1.51596090e-02f, 7.56892653e-01f, 4.28041051e-01f, -4.96207261e-01f, 1.13479706e+00f, 4.28521193e-01f, -7.19192471e-01f, 0.00000000e+00f },
        { 1.51596090e-02f, -7.56892653e-01f, -4.28041051e-01f, 4.96207261e-01f, -1.13479706e+00f, -4.28521193e-01f, 7.19192471e-01f, 0.00000000e+00f },
        { -8.65437000e-01f, 1.06618740e+00f, 8.52177444e-01f, -2.12612763e-01f, 3.46176493e-01f, -9.44835725e-01f, 2.44580702e-01f, -8.28906967e-01f },
        { 8.65437000e-01f, -1.06618740e+00f, -8.52177444e-01f, 2.12612763e-01f, -3.46176493e-01f, 9.44835725e-01f, -2.44580702e-01f, 8.28906967e-01f },
        { 1.23594166e-01f, -7.99368320e-01f, 1.88697636e-01f, -5.08272298e-01f, 3.92079705e-01f, 1.09554826e+00f, -1.39062521e+00f, 1.02422309e+00f },
        { -1.23594166e-01f, 7.99368320e-01f, -1.88697636e-01f, 5.08272298e-01f, -3.92079705e-01f, -1.09554826e+00f, 1.39062521e+00f, -1.02422309e+00f },
        { 4.53434178e-02f, -7.87760572e-01f, -1.06847055e-01f, 7.20346901e-01f, 4.71568805e-01f, -5.44260723e-01f, -4.92094296e-01f, 9.46182513e-01f },
        { -4.53434178e-02f, 7.87760572e-01f, 1.06847055e-01f, -7.20346901e-01f, -4.71568805e-01f, 5.44260723e-01f, 4.92094296e-01f, -9.46182513e-01f },
        { -1.17167714e+00f, -1.31927205e-01f, 4.35794927e-02f, 7.71616926e-01f, -1.08493233e+00f, 7.00462575e-01f, -7.23892673e-01f, -2.06996896e+00f },
        { 1.17167714e+00f, 1.31927205e-01f, -4.35794927e-02f, -7.71616926e-01f, 1.08493233e+00f, -7.00462575e-01f, 7.23892673e-01f, 2.06996896e+00f },
        { -3.90546100e-01f, -3.37824657e+00f, 2.56018491e-02f, 5.38612520e-01f, -5.77781154e-01f, 5.55021298e-01f, -2.15676582e-01f, 4.74334410e-01f },
        { 3.90546100e-01f, 3.37824657e+00f, -2.56018491e-02f, -5.38612520e-01f, 5.77781154e-01f, -5.55021298e-01f, 2.15676582e-01f, -4.74334410e-01f },
        { -1.92240233e-01f, 2.21912988e-01f, -1.12820829e+00f, -2.03726458e-01f, -3.46185420e-01f, -1.10214086e+00f, -9.02616082e-01f, 1.13586779e+00f },
        { 1.92240233e-01f, -2.21912988e-01f, 1.12820829e+00f, 2.03726458e-01f, 3.46185420e-01f, 1.10214086e+00f, 9.02616082e-01f, -1.13586779e+00f },
        { -1.54507562e-01f, 2.44678316e-01f, -7.70401630e-01f, 7.08479225e-01f, 1.03920172e+00f, 1.57147488e-01f, 8.02535541e-01f, -1.63198556e+00f },
        { 1.54507562e-01f, -2.44678316e-01f, 7.70401630e-01f, -7.08479225e-01f, -1.03920172e+00f, -1.57147488e-01f, -8.02535541e-01f, 1.63198556e+00f },
        { 8.62975678e-02f, -4.93833641e-01f, -2.74672474e-01f, 3.58188231e-01f, -6.59916153e-01f, -1.33675619e+00f, 9.38484234e-01f, -2.62926400e-01f },
        { -8.62975678e-02f, 4.93833641e-01f, 2.74672474e-01f, -3.58188231e-01f, 6.59916153e-01f, 1.33675619e+00f, -9.38484234e-01f, 2.62926400e-01f },
        { -7.49850939e-01f, 1.02868059e-01f, 5.46835943e-01f, -6.93402091e-01f, 8.08426320e-01f, -4.44768939e-01f, 1.05349689e+00f, 4.33831810e-01f },
        { 7.49850939e-01f, -1.02868059e-01f, -5.46835943e-01f, 6.93402091e-01f, -8.08426320e-01f, 4.44768939e-01f, -1.05349689e+00f, -4.33831810e-01f },
        { 1.27807312e-02f, -7.94712498e-01f, -9.05771323e-02f, 4.07638931e-01f, -2.68138454e-02f, -1.00353048e+00f, 1.15449815e+00f, 4.08112957e-04f },
        { -1.27807312e-02f, 7.94712498e-01f, 9.05771323e-02f, -4.07638931e-01f, 2.68138454e-02f, 1.00353048e+00f, -1.15449815e+00f, -4.08112957e-04f },
        { -6.07605413e-01f, 2.99351648e-02f, 4.77047542e-01f, -2.01500387e-01f, 5.69771874e-01f, -3.88995482e-01f, -8.04601701e-01f, -4.38945640e-02f },
        { 6.07605413e-01f, -2.99351648e-02f, -4.77047542e-01f, 2.01500387e-01f, -5.69771874e-01f, 3.88995482e-01f, 8.04601701e-01f, 4.38945640e-02f },
        { -1.77191254e-02f, -1.21622388e+00f, 5.66391227e-01f, -4.70856401e-02f, 8.25098161e-01f, -1.37019176e+00f, 5.94817615e-01f, 1.04194153e+00f },
        { 1.77191254e-02f, 1.21622388e+00f, -5.66391227e-01f, 4.70856401e-02f, -8.25098161e-01f, 1.37019176e+00f, -5.94817615e-01f, -1.04194153e+00f },
        { -6.08427204e-01f, 7.31374111e-02f, 6.58601534e-01f, -3.34909303e-01f, 2.17130319e-01f, -4.00422496e-01f, -5.69424008e-01f, 3.97830718e-01f },
        { 6.08427204e-01f, -7.31374111e-02f, -6.58601534e-01f, 3.34909303e-01f, -2.17130319e-01f, 4.00422496e-01f, 5.69424008e-01f, -3.97830718e-01f },
        { -1.14867215e-01f, -1.52286397e+00f, -6.88364151e-01f, 1.37719750e+00f, -3.24676976e-01f, 2.03429172e-01f, 4.40807541e-01f, -1.88967273e+00f },
        { 1.14867215e-01f, 1.52286397e+00f, 6.88364151e-01f, -1.37719750e+00f, 3.24676976e-01f, -2.03429172e-01f, -4.40807541e-01f, 1.88967273e+00f },
        { 6.68905729e-02f, -9.52883324e-01f, -9.62154419e-02f, 2.04816811e-01f, -1.32489675e-02f, -1.19459392e+00f, -1.16402061e+00f, 2.00010956e-01f },
        { -6.68905729e-02f, 9.52883324e-01f, 9.62154419e-02f, -2.04816811e-01f, 1.32489675e-02f, 1.19459392e+00f, 1.16402061e+00f, -2.00010956e-01f },
        { 3.76318037e-01f, -2.56645895e-01f, 4.41263305e-01f, -8.00898370e-01f, -8.72267850e-01f, 2.97691342e-01f, 7.68426844e-03f, 5.28054063e-01f },
        { -3.76318037e-01f, 2.56645895e-01f, -4.41263305e-01f, 8.00898370e-01f, 8.72267850e-01f, -2.97691342e-01f, -7.68426844e-03f, -5.28054063e-01f },
        { -1.94391967e+00f, 4.17001933e-01f, -6.41276301e-01f, -1.69757433e+00f, 5.83113397e-02f, -4.03833917e-01f, 3.05892336e+00f, -3.17718116e-01f },
        { 1.94391967e+00f, -4.17001933e-01f, 6.41276301e-01f, 1.69757433e+00f, -5.83113397e-02f, 4.03833917e-01f, -3.05892336e+00f, 3.17718116e-01f },
        { -5.38611963e-01f, 1.27872095e-01f, 2.23935959e-01f, -8.22883904e-01f, -6.83300926e-01f, -1.57103828e+00f, -1.42378197e+00f, 9.39707630e-01f },
        { 5.38611963e-01f, -1.27872095e-01f, -2.23935959e-01f, 8.22883904e-01f, 6.83300926e-01f, 1.57103828e+00f, 1.42378197e+00f, -9.39707630e-01f },
        { -4.37594864e-01f, 8.03818283e-01f, -1.96834846e+00f, 8.98098637e-01f, -2.40533503e-01f, 5.37475313e-01f, -1.32552277e-01f, 1.78559605e-01f },
        { 4.37594864e-01f, -8.03818283e-01f, 1.96834846e+00f, -8.98098637e-01f, 2.40533503e-01f, -5.37475313e-01f, 1.32552277e-01f, -1.78559605e-01f },
        { 2.61894183e-01f, -1.68377607e-01f, 6.99677293e-01f, -4.16750520e-01f, -9.50388903e-01f, 1.19979812e-01f, 1.39427573e+00f, -1.08850924e-01f },
        { -2.61894183e-01f, 1.68377607e-01f, -6.99677293e-01f, 4.16750520e-01f, 9.50388903e-01f, -1.19979812e-01f, -1.39427573e+00f, 1.08850924e-01f },
        { 2.30276671e-01f, -6.84129384e-01f, 4.24803116e-02f, -3.91750484e-01f, 5.59104506e-01f, -1.43411299e+00f, 8.39992282e-01f, -4.70030640e-01f },
        { -2.30276671e-01f, 6.84129384e-01f, -4.24803116e-02f, 3.91750484e-01f, -5.59104506e-01f, 1.43411299e+00f, -8.39992282e-01f, 4.70030640e-01f },
        { 4.82425290e-01f, -4.70201300e-01f, -4.11368733e-02f, 4.17871616e-01f, 6.77583293e-01f, 1.04630768e+00f, 6.71026786e-01f, -1.24495849e+00f },
        { -4.82425290e-01f, 4.70201300e-01f, 4.11368733e-02f, -4.17871616e-01f, -6.77583293e-01f, -1.04630768e+00f, -6.71026786e-01f, 1.24495849e+00f },
        { 9.23153622e-03f, 5.39799532e-01f, -1.14829797e+00f, 3.26737264e-01f, -1.10982447e+00f, -2.62324304e+00f, -2.41580905e-01f, 1.76238212e+00f },
        { -9.23153622e-03f, -5.39799532e-01f, 1.14829797e+00f, -3.26737264e-01f, 1.10982447e+00f, 2.62324304e+00f, 2.41580905e-01f, -1.76238212e+00f },
        { -6.19312600e-01f, 1.02865502e+00f, 1.47488954e-01f, -3.58029789e-01f, 3.13805719e-01f, -4.28520882e-01f, -3.94645135e-01f, 1.01856213e+00f },
        { 6.19312600e-01f, -1.02865502e+00f, -1.47488954e-01f, 3.58029789e-01f, -3.13805719e-01f, 4.28520882e-01f, 3.94645135e-01f, -1.01856213e+00f },
        { 9.40558734e-02f, -5.37481758e-01f, 9.40395309e-02f, 7.60986993e-01f, -8.71387969e-02f, -9.06804880e-01f, 8.98204686e-01f, 5.99914941e-05f },
        { -9.40558734e-02f, 5.37481758e-01f, -9.40395309e-02f, -7.60986993e-01f, 8.71387969e-02f, 9.06804880e-01f, -8.98204686e-01f, -5.99914941e-05f },
        { -1.37660286e+00f, -2.55847807e-01f, 1.02582703e-01f, -1.72007523e-01f, 7.25111988e-01f, -3.06100681e-01f, 9.30116764e-01f, 2.20323807e-01f },
        { 1.37660286e+00f, 2.55847807e-01f, -1.02582703e-01f, 1.72007523e-01f, -7.25111988e-01f, 3.06100681e-01f, -9.30116764e-01f, -2.20323807e-01f },
        { 4.83317716e-01f, -3.12932120e-01f, -1.21980820e-01f, 4.75819841e-01f, 3.22841550e-01f, -6.26082996e-01f, -6.11824868e-01f, 1.13389935e-01f },
        { -4.83317716e-01f, 3.12932120e-01f, 1.21980820e-01f, -4.75819841e-01f, -3.22841550e-01f, 6.26082996e-01f, 6.11824868e-01f, -1.13389935e-01f },
        { -1.41747806e+00f, 3.44898328e-01f, 1.32971335e-01f, -8.11659046e-01f, 1.12078048e-02f, -5.99307616e-01f, 1.99354178e-01f, -2.28954708e-01f },
        { 1.41747806e+00f, -3.44898328e-01f, -1.32971335e-01f, 8.11659046e-01f, -1.12078048e-02f, 5.99307616e-01f, -1.99354178e-01f, 2.28954708e-01f },
        { -1.05119900e-01f, 1.36135769e-01f, -6.95752473e-01f, 1.11252913e+00f, -5.01357493e-01f, 2.94714968e-01f, -5.66261347e-02f, -1.08399141e+00f },
        { 1.05119900e-01f, -1.36135769e-01f, 6.95752473e-01f, -1.11252913e+00f, 5.01357493e-01f, -2.94714968e-01f, 5.66261347e-02f, 1.08399141e+00f },
        { -1.13730740e-01f, -8.01993440e-01f, 4.33676558e-01f, -6.50742831e-03f, 9.79768544e-01f, 2.21133218e-01f, -1.72954563e+00f, 6.97061536e-01f },
        { 1.13730740e-01f, 8.01993440e-01f, -4.33676558e-01f, 6.50742831e-03f, -9.79768544e-01f, -2.21133218e-01f, 1.72954563e+00f, -6.97061536e-01f },
        { -1.46618687e-01f, 8.49539374e-02f, -2.29483974e-01f, -1.49065646e+00f, 3.38467344e-01f, -7.75975596e-01f, 9.56469002e-01f, -5.21791104e-01f },
        { 1.46618687e-01f, -8.49539374e-02f, 2.29483974e-01f, 1.49065646e+00f, -3.38467344e-01f, 7.75975596e-01f, -9.56469002e-01f, 5.21791104e-01f },
        { -2.00068548e-01f, 1.05283501e-01f, 5.65985558e-01f, -7.79228595e-02f, -3.11000415e-01f, 1.14359474e+00f, -3.76654465e-01f, -1.55341641e+00f },
        { 2.00068548e-01f, -1.05283501e-01f, -5.65985558e-01f, 7.79228595e-02f, 3.11000415e-01f, -1.14359474e+00f, 3.76654465e-01f, 1.55341641e+00f },
        { 1.85808441e-01f, -3.35469040e-01f, 2.42056866e-01f, -7.62907986e-01f, -5.79574864e-01f, 2.45986546e+00f, -2.69760028e-01f, 9.52730341e-02f },
        { -1.85808441e-01f, 3.35469040e-01f, -2.42056866e-01f, 7.62907986e-01f, 5.79574864e-01f, -2.45986546e+00f, 2.69760028e-01f, -9.52730341e-02f },
        { -2.95260174e-01f, 5.08260783e-02f, 4.75722040e-01f, 2.27965248e-02f, 1.97995513e-01f, -6.32457524e-01f, -1.07852166e+00f, 1.27561428e+00f },
        { 2.95260174e-01f, -5.08260783e-02f, -4.75722040e-01f, -2.27965248e-02f, -1.97995513e-01f, 6.32457524e-01f, 1.07852166e+00f, -1.27561428e+00f },
        { 1.38971515e-01f, -4.69493379e-01f, 2.57036270e-01f, -1.08791230e-01f, 1.30048608e-01f, -7.09140963e-01f, -2.65504284e-01f, 3.03718104e-01f },
        { -1.38971515e-01f, 4.69493379e-01f, -2.57036270e-01f, 1.08791230e-01f, -1.30048608e-01f, 7.09140963e-01f, 2.65504284e-01f, -3.03718104e-01f },
        { 3.53769054e-01f, -1.65983676e-02f, -9.38938151e-01f, -4.06711904e-03f, 2.80208298e-02f, 9.78358121e-01f, -8.51792103e-01f, 6.89416768e-01f },
        { -3.53769054e-01f, 1.65983676e-02f, 9.38938151e-01f, 4.06711904e-03f, -2.80208298e-02f, -9.78358121e-01f, 8.51792103e-01f, -6.89416768e-01f },
        { -2.48020139e-01f, 6.64062105e-02f, -2.05683926e-01f, 2.72764364e-01f, -4.79442665e-01f, 6.17542388e-02f, -1.18794181e+00f, 3.16154382e-01f },
        { 2.48020139e-01f, -6.64062105e-02f, 2.05683926e-01f, -2.72764364e-01f, 4.79442665e-01f, -6.17542388e-02f, 1.18794181e+00f, -3.16154382e-01f },
        { 5.91635252e-01f, -1.01689556e+00f, -2.83030981e-01f, -1.07049436e+00f, -5.48983051e-02f, 5.59291636e-01f, -2.92120316e-01f, 2.36772867e-02f },
        { -5.91635252e-01f, 1.01689556e+00f, 2.83030981e-01f, 1.07049436e+00f, 5.48983051e-02f, -5.59291636e-01f, 2.92120316e-01f, -2.36772867e-02f },
        { -1.32219949e-01f, 1.63381580e-01f, -1.76413148e-01f, 4.69499909e-01f, -8.77846112e-01f, 1.44241360e+00f, -7.11682442e-01f, 2.63494369e-01f },
        { 1.32219949e-01f, -1.63381580e-01f, 1.76413148e-01f, -4.69499909e-01f, 8.77846112e-01f, -1.44241360e+00f, 7.11682442e-01f, -2.63494369e-01f },
        { 3.88418873e-01f, -1.20523011e-01f, -5.20601636e-01f, 2.39863253e-02f, -4.55068051e-01f, 3.57281177e-01f, 1.21901930e+00f, 1.21223493e-01f },
        { -3.88418873e-01f, 1.20523011e-01f, 5.20601636e-01f, -2.39863253e-02f, 4.55068051e-01f, -3.57281177e-01f, -1.21901930e+00f, -1.21223493e-01f },
        { -2.08607749e+00f, -4.05425950e-01f, -5.00069013e-01f, 4.93578414e-01f, 1.07753042e-01f, -1.60294771e-01f, 3.41376230e-01f, -6.33839687e-01f },
        { 2.08607749e+00f, 4.05425950e-01f, 5.00069013e-01f, -4.93578414e-01f, -1.07753042e-01f, 1.60294771e-01f, -3.41376230e-01f, 6.33839687e-01f },
        { 3.15011710e-01f, -3.29600819e-01f, -6.09648538e-01f, 5.47816031e-01f, 2.31760532e-01f, -2.99937261e-01f, -1.61076840e-01f, 4.69769541e-01f },
        { -3.15011710e-01f, 3.29600819e-01f, 6.09648538e-01f, -5.47816031e-01f, -2.31760532e-01f, 2.99937261e-01f, 1.61076840e-01f, -4.69769541e-01f },
        { -2.48948529e-01f, 4.03390925e-01f, 4.76295292e-01f, -3.98144009e-01f, 4.64080225e-01f, -4.78748736e-01f, 4.49652500e-03f, 2.80554485e-01f },
        { 2.48948529e-01f, -4.03390925e-01f, -4.76295292e-01f, 3.98144009e-01f, -4.64080225e-01f, 4.78748736e-01f, -4.49652500e-03f, -2.80554485e-01f },
        { -2.83322487e-01f, 2.55247065e-01f, -2.30433329e-01f, 8.82623271e-02f, -1.89619024e+00f, -2.38900452e-01f, 2.50287277e-01f, -8.79572242e-01f },
        { 2.83322487e-01f, -2.55247065e-01f, 2.30433329e-01f, -8.82623271e-02f, 1.89619024e+00f, 2.38900452e-01f, -2.50287277e-01f, 8.79572242e-01f },
        { 3.37596864e-01f, -1.64388389e-01f, -4.32797999e-01f, 1.55436979e-01f, 3.89233436e-01f, -1.56276405e-01f, -7.39864145e-01f, 3.55662173e-01f },
        { -3.37596864e-01f, 1.64388389e-01f, 4.32797999e-01f, -1.55436979e-01f, -3.89233436e-01f, 1.56276405e-01f, 7.39864145e-01f, -3.55662173e-01f },
        { 2.24506816e-02f, 3.57503437e-01f, 1.08973100e-01f, -5.70071140e-01f, -1.62156309e-01f, 1.15121528e+00f, -7.25698409e-01f, 8.99059240e-01f },
        { -2.24506816e-02f, -3.57503437e-01f, -1.08973100e-01f, 5.70071140e-01f, 1.62156309e-01f, -1.15121528e+00f, 7.25698409e-01f, -8.99059240e-01f },
        { -3.61942959e-01f, -1.36004499e+00f, -3.28935470e-01f, 1.28637539e+00f, 1.20265766e-01f, -1.75193120e-01f, -2.71519363e-01f, 2.54954668e-01f },
        { 3.61942959e-01f, 1.36004499e+00f, 3.28935470e-01f, -1.28637539e+00f, -1.20265766e-01f, 1.75193120e-01f, 2.71519363e-01f, -2.54954668e-01f },
        { 5.26870927e-02f, -4.77610764e-01f, -7.89501826e-02f, 8.72924958e-01f, -2.56805558e-01f, -9.34190426e-01f, 1.99978395e+00f, -9.02070416e-01f },
        { -5.26870927e-02f, 4.77610764e-01f, 7.89501826e-02f, -8.72924958e-01f, 2.56805558e-01f, 9.34190426e-01f, -1.99978395e+00f, 9.02070416e-01f },
        { 8.68306304e-01f, -1.52115899e-01f, -1.27539476e-01f, -9.01187118e-01f, 1.04805564e-01f, 8.02643914e-01f, 1.36657105e-01f, -1.92277414e-01f },
        { -8.68306304e-01f, 1.52115899e-01f, 1.27539476e-01f, 9.01187118e-01f, -1.04805564e-01f, -8.02643914e-01f, -1.36657105e-01f, 1.92277414e-01f },
        { -1.02577450e+00f, -1.34585240e-02f, -1.52249787e-02f, 3.05003446e-01f, 7.00086325e-01f, -1.25910182e+00f, 9.58849451e-01f, 0.00000000e+00f },
        { 1.02577450e+00f, 1.34585240e-02f, 1.52249787e-02f, -3.05003446e-01f, -7.00086325e-01f, 1.25910182e+00f, -9.58849451e-01f, 0.00000000e+00f },
        { 5.67904602e-02f, -3.69742842e-01f, -6.93200919e-02f, 4.97793884e-01f, -8.83813845e-01f, 7.54875627e-01f, -1.94476471e-01f, 1.13851779e+00f },
        { -5.67904602e-02f, 3.69742842e-01f, 6.93200919e-02f, -4.97793884e-01f, 8.83813845e-01f, -7.54875627e-01f, 1.94476471e-01f, -1.13851779e+00f },
        { -2.22038457e-01f, 1.44859149e+00f, -1.23686981e-01f, 3.35311651e-01f, 2.03290495e-01f, -4.37683496e-01f, -4.30455366e-01f, 6.83779343e-01f },
        { 2.22038457e-01f, -1.44859149e+00f, 1.23686981e-01f, -3.35311651e-01f, -2.03290495e-01f, 4.37683496e-01f, 4.30455366e-01f, -6.83779343e-01f },
        { -8.96707917e-02f, 1.30554062e+00f, -1.08909139e+00f, 4.05767991e-01f, 6.53726386e-01f, -2.19022489e-01f, -1.98752737e-01f, 8.39102021e-02f },
        { 8.96707917e-02f, -1.30554062e+00f, 1.08909139e+00f, -4.05767991e-01f, -6.53726386e-01f, 2.19022489e-01f, 1.98752737e-01f, -8.39102021e-02f },
        { -3.72638416e-01f, 5.47213462e-01f, -7.86003234e-02f, 1.01982059e-01f, 5.31472775e-01f, -1.42337060e+00f, 1.01731862e+00f, -2.93516853e-03f },
        { 3.72638416e-01f, -5.47213462e-01f, 7.86003234e-02f, -1.01982059e-01f, -5.31472775e-01f, 1.42337060e+00f, -1.01731862e+00f, 2.93516853e-03f },
        { 5.42909095e-02f, -1.01141144e-01f, 3.51040972e-01f, -9.48652377e-01f, -5.44589297e-01f, -1.44500297e+00f, -1.05187788e+00f, 3.79807366e-01f },
        { -5.42909095e-02f, 1.01141144e-01f, -3.51040972e-01f, 9.48652377e-01f, 5.44589297e-01f, 1.44500297e+00f, 1.05187788e+00f, -3.79807366e-01f },
        { -5.21182927e-01f, -1.71025315e+00f, 1.94631855e-01f, -1.05350942e+00f, -1.41851905e-01f, 9.24475651e-02f, 3.27938405e-01f, -3.84163391e-01f },
        { 5.21182927e-01f, 1.71025315e+00f, -1.94631855e-01f, 1.05350942e+00f, 1.41851905e-01f, -9.24475651e-02f, -3.27938405e-01f, 3.84163391e-01f },
        { -2.44455354e-01f, -1.24426998e+00f, -2.43457511e-01f, 7.31187624e-02f, 3.37679421e-01f, -3.14186653e-01f, -1.17549839e+00f, 4.54170087e-02f },
        { 2.44455354e-01f, 1.24426998e+00f, 2.43457511e-01f, -7.31187624e-02f, -3.37679421e-01f, 3.14186653e-01f, 1.17549839e+00f, -4.54170087e-02f },
        { 2.95443350e-01f, -3.60313016e-01f, -4.81102271e-01f, 1.41981437e-01f, -5.33960163e-01f, 2.70982892e-01f, 1.24756483e-01f, -1.68162707e-01f },
        { -2.95443350e-01f, 3.60313016e-01f, 4.81102271e-01f, -1.41981437e-01f, 5.33960163e-01f, -2.70982892e-01f, -1.24756483e-01f, 1.68162707e-01f },
        { 6.87536008e-03f, -5.41238618e-01f, -1.10994048e-01f, 4.23240585e-01f, 7.44502179e-01f, -2.30686617e-01f, -1.24678277e+00f, -1.08719315e-01f },
        { -6.87536008e-03f, 5.41238618e-01f, 1.10994048e-01f, -4.23240585e-01f, -7.44502179e-01f, 2.30686617e-01f, 1.24678277e+00f, 1.08719315e-01f },
        { 1.19010285e+00f, -3.96783195e-02f, -7.92531170e-01f, -1.34823419e-01f, 6.28515844e-01f, -4.01521405e-01f, -2.16155084e-02f, 4.12999990e-01f },
        { -1.19010285e+00f, 3.96783195e-02f, 7.92531170e-01f, 1.34823419e-01f, -6.28515844e-01f, 4.01521405e-01f, 2.16155084e-02f, -4.12999990e-01f },
        { -5.83496372e-02f, 2.90203448e-01f, 8.26592928e-01f, -5.17375521e-01f, 2.38595171e-01f, -6.12590775e-01f, -4.36879007e-01f, 4.84813851e-01f },
        { 5.83496372e-02f, -2.90203448e-01f, -8.26592928e-01f, 5.17375521e-01f, -2.38595171e-01f, 6.12590775e-01f, 4.36879007e-01f, -4.84813851e-01f },
        { -8.56948462e-03f, 6.43775823e-01f, -1.26840375e+00f, 2.10846088e-01f, -9.03623984e-02f, -6.38634599e-01f, -5.67639451e-02f, 6.49825100e-01f },
        { 8.56948462e-03f, -6.43775823e-01f, 1.26840375e+00f, -2.10846088e-01f, 9.03623984e-02f, 6.38634599e-01f, 5.67639451e-02f, -6.49825100e-01f },
        { 1.59225076e-02f, 4.03924522e-01f, -1.88167476e-01f, -1.54709425e+00f, 1.58022528e+00f, 8.71558467e-02f, -5.17805316e-01f, 1.09248130e+00f },
        { -1.59225076e-02f, -4.03924522e-01f, 1.88167476e-01f, 1.54709425e+00f, -1.58022528e+00f, -8.71558467e-02f, 5.17805316e-01f, -1.09248130e+00f },
        { -3.10454872e-01f, 5.60488440e-02f, -4.47349943e-01f, 1.65903546e-01f, 5.76433732e-01f, -3.33853420e-01f, 2.36548360e-02f, 9.10300508e-01f },
        { 3.10454872e-01f, -5.60488440e-02f, 4.47349943e-01f, -1.65903546e-01f, -5.76433732e-01f, 3.33853420e-01f, -2.36548360e-02f, -9.10300508e-01f },
        { 1.84821748e+00f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f, 9.04601879e-02f, -1.40242215e-01f, -3.25927944e-01f, 2.27212655e-01f },
        { -1.84821748e+00f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f, -9.04601879e-02f, 1.40242215e-01f, 3.25927944e-01f, -2.27212655e-01f },
        { -4.93265030e-02f, 6.72388152e-01f, -2.63168540e-01f, 8.59145600e-02f, -1.02817478e-01f, 9.53347871e-01f, 7.82923661e-02f, 1.04635951e+00f },
        { 4.93265030e-02f, -6.72388152e-01f, 2.63168540e-01f, -8.59145600e-02f, 1.02817478e-01f, -9.53347871e-01f, -7.82923661e-02f, -1.04635951e+00f },
        { -3.85420722e-05f, 4.93798792e-01f, -8.55728131e-01f, 3.01567524e-01f, 7.00760779e-01f, -7.61317079e-01f, -5.21650198e-01f, 3.62141079e-01f },
        { 3.85420722e-05f, -4.93798792e-01f, 8.55728131e-01f, -3.01567524e-01f, -7.00760779e-01f, 7.61317079e-01f, 5.21650198e-01f, -3.62141079e-01f },
        { -1.86410608e-01f, 2.15148410e-01f, -6.23197038e-01f, 3.05793985e-02f, -6.01986350e-01f, 3.55748136e-02f, -1.68130156e-02f, 2.89181864e-01f },
        { 1.86410608e-01f, -2.15148410e-01f, 6.23197038e-01f, -3.05793985e-02f, 6.01986350e-01f, -3.55748136e-02f, 1.68130156e-02f, -2.89181864e-01f },
        { -1.30844147e-02f, 2.27730299e-01f, -6.28476238e-01f, 8.06836304e-02f, 7.86653533e-01f, -9.95606173e-01f, 9.71635369e-01f, 4.60394196e-01f },
        { 1.30844147e-02f, -2.27730299e-01f, 6.28476238e-01f, -8.06836304e-02f, -7.86653533e-01f, 9.95606173e-01f, -9.71635369e-01f, -4.60394196e-01f },
        { -9.63267682e-01f, 4.23447961e-01f, -1.41384423e-01f, -1.04101769e+00f, -3.92179466e-01f, 3.26467906e-01f, 1.70711920e-01f, -1.35122243e-02f },
        { 9.63267682e-01f, -4.23447961e-01f, 1.41384423e-01f, 1.04101769e+00f, 3.92179466e-01f, -3.26467906e-01f, -1.70711920e-01f, 1.35122243e-02f },
        { -1.50927647e+00f, -1.54685740e-01f, -4.85195542e-01f, 2.80257869e-01f, 4.34445719e-01f, -2.07208669e-01f, 7.34987983e-02f, -1.29862192e-01f },
        { 1.50927647e+00f, 1.54685740e-01f, 4.85195542e-01f, -2.80257869e-01f, -4.34445719e-01f, 2.07208669e-01f, -7.34987983e-02f, 1.29862192e-01f },
        { -3.04860291e-01f, 1.78845478e-02f, -1.10035149e-01f, 6.48593158e-01f, -1.74608421e+00f, -3.63274101e-01f, 2.18934768e-01f, -1.92562520e+00f },
        { 3.04860291e-01f, -1.78845478e-02f, 1.10035149e-01f, -6.48593158e-01f, 1.74608421e+00f, 3.63274101e-01f, -2.18934768e-01f, 1.92562520e+00f },
        { 5.29168086e-01f, -2.27348645e-01f, 4.32652001e-01f, -2.07337537e+00f, 4.41542304e-01f, -1.88238534e-01f, -1.98809670e-01f, 3.60535273e-02f },
        { -5.29168086e-01f, 2.27348645e-01f, -4.32652001e-01f, 2.07337537e+00f, -4.41542304e-01f, 1.88238534e-01f, 1.98809670e-01f, -3.60535273e-02f },
        { -3.95621418e-02f, 2.93057566e-01f, 5.17817281e-01f, -2.21658996e-02f, -4.62301125e-01f, 1.76436232e-01f, -6.66349863e-01f, 1.05074465e+00f },
        { 3.95621418e-02f, -2.93057566e-01f, -5.17817281e-01f, 2.21658996e-02f, 4.62301125e-01f, -1.76436232e-01f, 6.66349863e-01f, -1.05074465e+00f },
        { 1.21893193e-01f, -4.51562048e-01f, -8.77417834e-01f, -2.77141562e-02f, 8.60328070e-01f, 8.00849880e-02f, 2.69387139e-01f, -5.82375740e-02f },
        { -1.21893193e-01f, 4.51562048e-01f, 8.77417834e-01f, 2.77141562e-02f, -8.60328070e-01f, -8.00849880e-02f, -2.69387139e-01f, 5.82375740e-02f },
        { -2.63769699e-01f, 4.05943666e-01f, -7.29596139e-02f, 1.30926712e-01f, -7.53942992e-01f, -5.33793284e-02f, 1.09787689e+00f, -3.25820228e-01f },
        { 2.63769699e-01f, -4.05943666e-01f, 7.29596139e-02f, -1.30926712e-01f, 7.53942992e-01f, 5.33793284e-02f, -1.09787689e+00f, 3.25820228e-01f },
        { -2.40804756e-01f, 2.44475489e-01f, 1.92177358e-02f, -2.41399063e-01f, -1.13402310e+00f, 8.32069167e-01f, 8.00442941e-01f, -2.69020631e-01f },
        { 2.40804756e-01f, -2.44475489e-01f, -1.92177358e-02f, 2.41399063e-01f, 1.13402310e+00f, -8.32069167e-01f, -8.00442941e-01f, 2.69020631e-01f },
        { 1.61646110e-01f, -8.03186361e-02f, -1.63803654e-02f, 5.83672766e-01f, -7.63494085e-01f, 4.92580976e-01f, -2.61254086e-01f, 1.01471046e+00f },
        { -1.61646110e-01f, 8.03186361e-02f, 1.63803654e-02f, -5.83672766e-01f, 7.63494085e-01f, -4.92580976e-01f, 2.61254086e-01f, -1.01471046e+00f },
        { -2.49231913e-01f, 2.95255667e-01f, 3.29217872e-02f, -3.32499941e-01f, -2.26737759e-01f, 4.39409790e-01f, 1.03778102e+00f, -5.85085336e-01f },
        { 2.49231913e-01f, -2.95255667e-01f, -3.29217872e-02f, 3.32499941e-01f, 2.26737759e-01f, -4.39409790e-01f, -1.03778102e+00f, 5.85085336e-01f },
        { 5.22341243e-02f, -8.31067358e-02f, 7.86577011e-01f, 3.49701682e-02f, -6.76835733e-01f, 7.59843809e-02f, -1.01741794e+00f, 4.02369410e-01f },
        { -5.22341243e-02f, 8.31067358e-02f, -7.86577011e-01f, -3.49701682e-02f, 6.76835733e-01f, -7.59843809e-02f, 1.01741794e+00f, -4.02369410e-01f },
        { 6.52190255e-02f, -1.17347148e-01f, -1.07142196e+00f, 3.96685466e-01f, -1.31017710e+00f, -1.55675222e-01f, 6.23954972e-03f, 1.01932278e+00f },
        { -6.52190255e-02f, 1.17347148e-01f, 1.07142196e+00f, -3.96685466e-01f, 1.31017710e+00f, 1.55675222e-01f, -6.23954972e-03f, -1.01932278e+00f },
        { 3.43287030e-02f, -1.13098105e-01f, 5.22100013e-01f, -2.36762447e-01f, -7.92312470e-01f, 3.76687093e-01f, -1.23457069e+00f, 9.36387732e-02f },
        { -3.43287030e-02f, 1.13098105e-01f, -5.22100013e-01f, 2.36762447e-01f, 7.92312470e-01f, -3.76687093e-01f, 1.23457069e+00f, -9.36387732e-02f },
        { -2.23196613e-01f, 1.56926667e-01f, 1.10159079e-01f, -2.55438003e-01f, -5.36930411e-01f, 9.91628209e-02f, 1.15919162e+00f, -1.79708593e-01f },
        { 2.23196613e-01f, -1.56926667e-01f, -1.10159079e-01f, 2.55438003e-01f, 5.36930411e-01f, -9.91628209e-02f, -1.15919162e+00f, 1.79708593e-01f },
        { -5.21226180e-01f, 1.73745367e-01f, 1.30102935e-01f, -1.99551584e-01f, 3.20929059e-01f, -1.74816130e-01f, -1.13934614e-01f, -1.04424978e+00f },
        { 5.21226180e-01f, -1.73745367e-01f, -1.30102935e-01f, 1.99551584e-01f, -3.20929059e-01f, 1.74816130e-01f, 1.13934614e-01f, 1.04424978e+00f },
        { 3.24487797e-01f, -1.77831191e-01f, -7.78091516e-02f, -6.61242440e-01f, -1.15172328e-01f, -1.14915459e+00f, 2.68801623e-02f, 4.70475981e-01f },
        { -3.24487797e-01f, 1.77831191e-01f, 7.78091516e-02f, 6.61242440e-01f, 1.15172328e-01f, 1.14915459e+00f, -2.68801623e-02f, -4.70475981e-01f },
        { 7.20471940e-03f, 6.31007079e-01f, 6.17900944e-01f, -1.00455301e+00f, -4.27623526e-01f, 3.18878446e-03f, -1.18489534e+00f, 6.13507623e-03f },
        { -7.20471940e-03f, -6.31007079e-01f, -6.17900944e-01f, 1.00455301e+00f, 4.27623526e-01f, -3.18878446e-03f, 1.18489534e+00f, -6.13507623e-03f },
        { 6.94074447e-02f, -5.61066768e-01f, -6.20238593e-02f, 2.60456469e-01f, -1.04147874e-01f, 2.35313482e-01f, -5.11751288e-03f, -8.38475575e-01f },
        { -6.94074447e-02f, 5.61066768e-01f, 6.20238593e-02f, -2.60456469e-01f, 1.04147874e-01f, -2.35313482e-01f, 5.11751288e-03f, 8.38475575e-01f },
        { -1.31924192e-01f, 7.48647965e-01f, -1.69143935e-01f, 3.03905221e-02f, 3.41527601e-01f, -5.97122485e-01f, -1.15724316e+00f, -1.42157076e-01f },
        { 1.31924192e-01f, -7.48647965e-01f, 1.69143935e-01f, -3.03905221e-02f, -3.41527601e-01f, 5.97122485e-01f, 1.15724316e+00f, 1.42157076e-01f },
        { 1.67802474e-02f, 1.02274967e+00f, -7.21412129e-01f, 2.32735374e-01f, -1.04987832e-02f, 8.86041645e-01f, -9.84120792e-01f, 6.27786005e-01f },
        { -1.67802474e-02f, -1.02274967e+00f, 7.21412129e-01f, -2.32735374e-01f, 1.04987832e-02f, -8.86041645e-01f, 9.84120792e-01f, -6.27786005e-01f },
        { 1.79859753e-02f, -2.61292663e-01f, -4.81919393e-02f, 4.49309374e-01f, -1.55739111e-01f, 7.03694650e-01f, -1.89284787e-01f, -9.56214336e-01f },
        { -1.79859753e-02f, 2.61292663e-01f, 4.81919393e-02f, -4.49309374e-01f, 1.55739111e-01f, -7.03694650e-01f, 1.89284787e-01f, 9.56214336e-01f },
        { 1.55509085e-01f, -4.14721870e-02f, 2.99144425e-01f, -6.19307021e-01f, -1.41139954e+00f, 2.43711993e-01f, 5.00192441e-01f, -7.53256959e-01f },
        { -1.55509085e-01f, 4.14721870e-02f, -2.99144425e-01f, 6.19307021e-01f, 1.41139954e+00f, -2.43711993e-01f, -5.00192441e-01f, 7.53256959e-01f },
        { -2.14421319e-01f, 4.11211637e-01f, -5.07540172e-02f, 1.27389321e-01f, 5.92344346e-01f, -1.66224333e+00f, -5.43207946e-01f, 1.62004342e-02f },
        { 2.14421319e-01f, -4.11211637e-01f, 5.07540172e-02f, -1.27389321e-01f, -5.92344346e-01f, 1.66224333e+00f, 5.43207946e-01f, -1.62004342e-02f },
        { -9.64760556e-01f, 9.78357802e-01f, -2.01506168e-01f, 3.78852207e-01f, 2.28301755e-02f, 5.91559162e-01f, -3.85350439e-01f, 1.74952279e-02f },
        { 9.64760556e-01f, -9.78357802e-01f, 2.01506168e-01f, -3.78852207e-01f, -2.28301755e-02f, -5.91559162e-01f, 3.85350439e-01f, -1.74952279e-02f },
        { 1.32208265e-01f, 6.84701443e-01f, 9.53174404e-02f, -2.97813984e-01f, -2.67674313e-01f, 7.23314378e-02f, 2.01261653e-01f, -4.67251512e-01f },
        { -1.32208265e-01f, -6.84701443e-01f, -9.53174404e-02f, 2.97813984e-01f, 2.67674313e-01f, -7.23314378e-02f, -2.01261653e-01f, 4.67251512e-01f },
        { 4.20312973e-02f, -2.15037273e-01f, -5.32605479e-01f, 2.51753608e-01f, 9.40187048e-01f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { -4.20312973e-02f, 2.15037273e-01f, 5.32605479e-01f, -2.51753608e-01f, -9.40187048e-01f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { 2.23758657e-01f, 1.20505186e-02f, -3.35998723e-01f, 2.55381802e-01f, -2.88962256e-02f, -7.17947858e-01f, -3.48040775e-01f, 4.72313904e-01f },
        { -2.23758657e-01f, -1.20505186e-02f, 3.35998723e-01f, -2.55381802e-01f, 2.88962256e-02f, 7.17947858e-01f, 3.48040775e-01f, -4.72313904e-01f },
        { -3.58902286e-01f, 7.14318342e-03f, 8.30584970e-01f, 1.90662719e-02f, 7.82973648e-02f, 8.44753645e-01f, 4.16435636e-01f, -1.88169722e+00f },
        { 3.58902286e-01f, -7.14318342e-03f, -8.30584970e-01f, -1.90662719e-02f, -7.82973648e-02f, -8.44753645e-01f, -4.16435636e-01f, 1.88169722e+00f },
        { -2.02045715e-01f, 1.47262742e-01f, 2.16859838e-01f, -1.04774858e-01f, 1.01446285e+00f, -1.70181489e-01f, 4.86524273e-01f, -1.32907283e+00f },
        { 2.02045715e-01f, -1.47262742e-01f, -2.16859838e-01f, 1.04774858e-01f, -1.01446285e+00f, 1.70181489e-01f, -4.86524273e-01f, 1.32907283e+00f },
        { -1.39405382e-02f, 1.21233428e-01f, -2.33444316e-02f, 1.21672074e+00f, 7.44724740e-01f, -4.90675236e-01f, -4.92612080e-01f, 7.31010706e-01f },
        { 1.39405382e-02f, -1.21233428e-01f, 2.33444316e-02f, -1.21672074e+00f, -7.44724740e-01f, 4.90675236e-01f, 4.92612080e-01f, -7.31010706e-01f },
        { -7.49232814e-02f, 1.70995601e-01f, 3.98880320e-01f, 3.19741260e-03f, -1.34823075e-01f, 1.71531031e+00f, -5.54031013e-01f, 7.50215091e-02f },
        { 7.49232814e-02f, -1.70995601e-01f, -3.98880320e-01f, -3.19741260e-03f, 1.34823075e-01f, -1.71531031e+00f, 5.54031013e-01f, -7.50215091e-02f },
        { 3.33043467e-01f, -7.06998076e-02f, -1.38452491e-01f, 5.12109095e-01f, -1.85056625e-01f, 1.48477423e-01f, -1.95300197e-01f, -1.06828101e+00f },
        { -3.33043467e-01f, 7.06998076e-02f, 1.38452491e-01f, -5.12109095e-01f, 1.85056625e-01f, -1.48477423e-01f, 1.95300197e-01f, 1.06828101e+00f },
        { 1.66691889e-01f, -1.57977018e-01f, -2.30477899e-01f, 2.45320580e-02f, -1.00441862e+00f, -1.35467590e-01f, 1.57977088e+00f, -2.14380992e-01f },
        { -1.66691889e-01f, 1.57977018e-01f, 2.30477899e-01f, -2.45320580e-02f, 1.00441862e+00f, 1.35467590e-01f, -1.57977088e+00f, 2.14380992e-01f },
        { 4.31008469e-01f, -3.05060030e-01f, -7.52236383e-01f, -1.04557537e-01f, 3.30869927e-01f, -1.08972537e-02f, -4.14105633e-01f, 1.29239156e+00f },
        { -4.31008469e-01f, 3.05060030e-01f, 7.52236383e-01f, 1.04557537e-01f, -3.30869927e-01f, 1.08972537e-02f, 4.14105633e-01f, -1.29239156e+00f },
        { 7.18076303e-02f, -2.39003820e-01f, -1.63996039e-01f, -1.33085828e+00f, -1.74767224e-01f, 3.70568577e-01f, -1.79678790e-01f, 2.37668144e-01f },
        { -7.18076303e-02f, 2.39003820e-01f, 1.63996039e-01f, 1.33085828e+00f, 1.74767224e-01f, -3.70568577e-01f, 1.79678790e-01f, -2.37668144e-01f },
        { 8.59556086e-02f, -1.35789640e-01f, -1.45638679e-01f, 6.30429076e-01f, -1.19986986e-01f, 4.79315351e-01f, -6.18328016e-01f, 8.17743798e-01f },
        { -8.59556086e-02f, 1.35789640e-01f, 1.45638679e-01f, -6.30429076e-01f, 1.19986986e-01f, -4.79315351e-01f, 6.18328016e-01f, -8.17743798e-01f },
        { 3.35293032e-02f, -1.69697608e-01f, 7.51786769e-01f, 2.32766298e+00f, -4.69859524e-01f, 2.53610700e-01f, -1.87579276e-01f, 1.30395974e+00f },
        { -3.35293032e-02f, 1.69697608e-01f, -7.51786769e-01f, -2.32766298e+00f, 4.69859524e-01f, -2.53610700e-01f, 1.87579276e-01f, -1.30395974e+00f },
        { 2.02159499e-01f, -3.23518353e-02f, 1.02611923e-01f, -9.66457706e-01f, 3.00144224e-01f, -9.47806890e-01f, 3.93070899e-01f, -1.68444958e+00f },
        { -2.02159499e-01f, 3.23518353e-02f, -1.02611923e-01f, 9.66457706e-01f, -3.00144224e-01f, 9.47806890e-01f, -3.93070899e-01f, 1.68444958e+00f },
        { 9.07475041e-03f, -2.65433050e-01f, -8.58677627e-01f, 1.39635786e-01f, -8.44829188e-03f, 2.85812504e-01f, 6.38169501e-01f, -1.28446410e+00f },
        { -9.07475041e-03f, 2.65433050e-01f, 8.58677627e-01f, -1.39635786e-01f, 8.44829188e-03f, -2.85812504e-01f, -6.38169501e-01f, 1.28446410e+00f },
        { 6.00049008e-02f, -3.11323535e-01f, 1.93820125e-01f, -6.82462712e-02f, 6.55131994e-01f, -1.55842739e-01f, -3.74919904e-01f, 4.83535807e-01f },
        { -6.00049008e-02f, 3.11323535e-01f, -1.93820125e-01f, 6.82462712e-02f, -6.55131994e-01f, 1.55842739e-01f, 3.74919904e-01f, -4.83535807e-01f },
        { -2.28170062e-01f, 1.03893265e-02f, -1.41879764e-01f, 5.89264054e-01f, -6.10821650e-01f, 6.57505494e-01f, 4.34982121e-01f, -1.83712436e-01f },
        { 2.28170062e-01f, -1.03893265e-02f, 1.41879764e-01f, -5.89264054e-01f, 6.10821650e-01f, -6.57505494e-01f, -4.34982121e-01f, 1.83712436e-01f },
        { -1.22423407e-01f, 3.37106773e-01f, -1.05025611e-01f, 9.12786570e-02f, -5.05211230e-01f, 5.45917466e-01f, -1.00329857e+00f, -8.97104760e-02f },
        { 1.22423407e-01f, -3.37106773e-01f, 1.05025611e-01f, -9.12786570e-02f, 5.05211230e-01f, -5.45917466e-01f, 1.00329857e+00f, 8.97104760e-02f },
    };
    // ---- END MODEL
};

// ------------------------------------------------------------------------------------------------ image model
// CLIP ViT-B/32 image encoder (OpenAI, MIT; pinned ONNX file, fetched by Setup): 224x224 RGB in, 512-d embedding out.
// Zero-shot against fixed prompt embeddings (content_prompts.inc): softmax of 100 x cosine over all prompts, the drawn
// prompts summed. Offline, per keyframe, leave-one-group-out is not needed (no training): drawn 94.9 %, live action
// 3.2 %, game footage 4.9 %, 3D CG animation 89 % (CG animation looks animated to it).
namespace clip {
#include "content_prompts.inc"
}
inline double ImageDrawnProbability(const float* emb)
{
    double n = 0; for (int i = 0; i < 512; ++i) n += double(emb[i]) * emb[i];
    n = n > 0 ? 1.0 / std::sqrt(n) : 0.0;
    double z[clip::kPromptsDrawn + clip::kPromptsOther], m = -1e30;
    for (int k = 0; k < clip::kPromptsDrawn + clip::kPromptsOther; ++k) {
        double d = 0; for (int i = 0; i < 512; ++i) d += double(emb[i]) * clip::kPrompt[k][i];
        z[k] = 100.0 * d * n; m = std::max(m, z[k]);
    }
    double sd = 0, sa = 0;
    for (int k = 0; k < clip::kPromptsDrawn + clip::kPromptsOther; ++k) { const double e = std::exp(z[k] - m); sa += e; if (k < clip::kPromptsDrawn) sd += e; }
    return sd / sa;
}
// Rendered 3D CG among drawn looks (State.cg): zero-shot hand-drawn vs rendered prompts (kCgPrompt), the rendered
// share. Offline, leave-one-group-out over the prompt sets (19 groups): the cel-shaded / stylised / realistic CG groups
// are flagged for 70 % of their drawn-look time on average (33-98 % per group), hand-drawn groups 10 % (below 3 % on 12
// of 15 groups; western digitally coloured TV and features 26-79 %). Only Auto's extras differ (DrawnPolicy).
inline double ImageCgProbability(const float* emb)
{
    constexpr int n = clip::kCgPromptsHand + clip::kCgPromptsRender;
    double nn = 0; for (int i = 0; i < 512; ++i) nn += double(emb[i]) * emb[i];
    nn = nn > 0 ? 1.0 / std::sqrt(nn) : 0.0;
    double z[n], m = -1e30;
    for (int k = 0; k < n; ++k) {
        double d = 0; for (int i = 0; i < 512; ++i) d += double(emb[i]) * clip::kCgPrompt[k][i];
        z[k] = 100.0 * d * nn; m = std::max(m, z[k]);
    }
    double sr = 0, sa = 0;
    for (int k = 0; k < n; ++k) { const double e = std::exp(z[k] - m); sa += e; if (k >= clip::kCgPromptsHand) sr += e; }
    return sr / sa;
}
// The model's input from YUV 4:2:0 planes (as CellsFromYuv): the centre square, bilinear to 224 x 224, RGB (BT.709
// for HD, BT.601 below 720 lines), CLIP's mean / std, planar NCHW float.
template <class T>
inline std::vector<float> ClipInput(const T* y, ptrdiff_t yStride, const T* u, const T* v, ptrdiff_t cStride, int w, int h, int bits, bool limited)
{
    std::vector<float> out(3 * 224 * 224);
    if (!y || !u || w < 16 || h < 16) return {};
    const int side = std::min(w, h), x0 = (w - side) / 2, y0 = (h - side) / 2;
    const float scale = 255.f / float((1 << bits) - 1), ys = limited ? 255.f / 219.f : 1.f, cs = limited ? 255.f / 224.f : 1.f;
    const bool hd = h >= 720;
    const float kr = hd ? 1.5748f : 1.402f, kgb = hd ? 0.1873f : 0.344f, kgr = hd ? 0.4681f : 0.714f, kb = hd ? 1.8556f : 1.772f;
    static const float mean[3] = { 0.48145466f, 0.4578275f, 0.40821073f }, sd[3] = { 0.26862954f, 0.26130258f, 0.27577711f };
    auto luma = [&](int xx, int yy) { return float(y[ptrdiff_t(std::clamp(yy, 0, h - 1)) * yStride + std::clamp(xx, 0, w - 1)]); };
    for (int j = 0; j < 224; ++j)
        for (int i = 0; i < 224; ++i) {
            const float fx = x0 + (i + 0.5f) * side / 224.f - 0.5f, fy = y0 + (j + 0.5f) * side / 224.f - 0.5f;
            const int ix = int(std::floor(fx)), iy = int(std::floor(fy)); const float ax = fx - ix, ay = fy - iy;
            const float l = (luma(ix, iy) * (1 - ax) + luma(ix + 1, iy) * ax) * (1 - ay) + (luma(ix, iy + 1) * (1 - ax) + luma(ix + 1, iy + 1) * ax) * ay;
            const int cx = std::clamp(int(fx) / 2, 0, w / 2 - 1), cy = std::clamp(int(fy) / 2, 0, h / 2 - 1);
            float cb, cr;
            if (v) { cb = float(u[ptrdiff_t(cy) * cStride + cx]); cr = float(v[ptrdiff_t(cy) * cStride + cx]); }
            else { cb = float(u[ptrdiff_t(cy) * cStride + 2 * cx]); cr = float(u[ptrdiff_t(cy) * cStride + 2 * cx + 1]); }
            float Y = l * scale; if (limited) Y = (Y - 16.f) * ys;
            const float Cb = (cb * scale - 128.f) * cs, Cr = (cr * scale - 128.f) * cs;
            const float rgb[3] = { Y + kr * Cr, Y - kgb * Cb - kgr * Cr, Y + kb * Cb };
            for (int c = 0; c < 3; ++c) out[size_t(c) * 224 * 224 + size_t(j) * 224 + i] = (std::clamp(rgb[c], 0.f, 255.f) / 255.f - mean[c]) / sd[c];
        }
    return out;
}

// ------------------------------------------------------------------------------------------------ GPU taps
// Routes whose frames stay on the GPU (the engine module's D3D11 and Vulkan zero-copy paths) compute the cell grid and the
// model's thumbnail in a compute pass and read back only those: one 32-bit word per cell / thumbnail pixel,
//   bits  0-15  luma, in 8-bit code units x 256 (the cell's mean, or the bilinear sample), before the range expansion
//   bits 16-23  Cb, 8-bit code (the top 8 bits of a deeper sample), the same point sample CellsFromYuv takes
//   bits 24-31  Cr, likewise
// 480 x CellsHeight words for the cells (~0.5 MB at 16:9), 224 x 224 for the thumbnail (196 KB, about once a second).
// The cells' geometry and sample positions are those of CellsFromYuv / ClipInput (the shaders mirror them).
inline int CellsHeight(int w, int h) { return std::max(1, int(std::lround(double(kCellsW) * h / w))); }
inline uint32_t PackYcc(float y8, float cb8, float cr8)
{
    return uint32_t(std::clamp(int(std::lround(y8 * 256.f)), 0, 65535)) | (uint32_t(std::clamp(int(std::lround(cb8)), 0, 255)) << 16) |
           (uint32_t(std::clamp(int(std::lround(cr8)), 0, 255)) << 24);
}
inline Cells CellsFromPacked(const uint32_t* words, ptrdiff_t strideWords, int cw, int ch, bool limited)
{
    Cells c;
    if (!words || cw != kCellsW || ch < 1) return c;
    c.w = cw; c.h = ch;
    c.y.resize(size_t(cw) * ch); c.cb.resize(c.y.size()); c.cr.resize(c.y.size());
    const float ys = limited ? 255.f / 219.f : 1.f, cs = limited ? 255.f / 224.f : 1.f;
    for (int j = 0; j < ch; ++j)
        for (int i = 0; i < cw; ++i) {
            const uint32_t v = words[ptrdiff_t(j) * strideWords + i];
            float m = float(v & 0xffffu) / 256.f;
            if (limited) m = (m - 16.f) * ys;
            const size_t k = size_t(j) * cw + i;
            c.y[k] = std::clamp(m, 0.f, 255.f);
            c.cb[k] = (float((v >> 16) & 0xffu) - 128.f) * cs; c.cr[k] = (float(v >> 24) - 128.f) * cs;
        }
    return c;
}
// the model's input from a 224 x 224 packed thumbnail (hd: BT.709, else BT.601, as ClipInput decides by the source height)
inline std::vector<float> ClipInputFromPacked(const uint32_t* words, ptrdiff_t strideWords, bool hd, bool limited)
{
    std::vector<float> out(3 * 224 * 224);
    if (!words) return {};
    const float ys = limited ? 255.f / 219.f : 1.f, cs = limited ? 255.f / 224.f : 1.f;
    const float kr = hd ? 1.5748f : 1.402f, kgb = hd ? 0.1873f : 0.344f, kgr = hd ? 0.4681f : 0.714f, kb = hd ? 1.8556f : 1.772f;
    static const float mean[3] = { 0.48145466f, 0.4578275f, 0.40821073f }, sd[3] = { 0.26862954f, 0.26130258f, 0.27577711f };
    for (int j = 0; j < 224; ++j)
        for (int i = 0; i < 224; ++i) {
            const uint32_t v = words[ptrdiff_t(j) * strideWords + i];
            float Y = float(v & 0xffffu) / 256.f; if (limited) Y = (Y - 16.f) * ys;
            const float Cb = (float((v >> 16) & 0xffu) - 128.f) * cs, Cr = (float(v >> 24) - 128.f) * cs;
            const float rgb[3] = { Y + kr * Cr, Y - kgb * Cb - kgr * Cr, Y + kb * Cb };
            for (int c = 0; c < 3; ++c) out[size_t(c) * 224 * 224 + size_t(j) * 224 + i] = (std::clamp(rgb[c], 0.f, 255.f) / 255.f - mean[c]) / sd[c];
        }
    return out;
}
// CPU reference of the shaders (tests): the packed words CellsFromYuv / ClipInput would see, from 4:2:0 planes
template <class T>
inline std::vector<uint32_t> PackedCellsRef(const T* y, ptrdiff_t yStride, const T* u, const T* v, ptrdiff_t cStride, int w, int h, int bits)
{
    const int cw = kCellsW, ch = CellsHeight(w, h);
    std::vector<uint32_t> o(size_t(cw) * ch);
    const float sc = 255.f / float((1 << bits) - 1);
    for (int j = 0; j < ch; ++j)
        for (int i = 0; i < cw; ++i) {
            const int xs = int(std::lround(double(i) * w / cw)), xe = std::max(int(std::lround(double(i + 1) * w / cw)), xs + 1);
            const int ysr = int(std::lround(double(j) * h / ch)), ye = std::max(int(std::lround(double(j + 1) * h / ch)), ysr + 1);
            double s = 0; int n = 0;
            for (int yy = ysr; yy < ye; ++yy) for (int xx = xs; xx < xe; ++xx) { s += y[ptrdiff_t(yy) * yStride + xx]; ++n; }
            const int cx = std::min((xs + xe) / 4, w / 2 - 1), cy = std::min((ysr + ye) / 4, h / 2 - 1);
            const float cb = float(v ? u[ptrdiff_t(cy) * cStride + cx] : u[ptrdiff_t(cy) * cStride + 2 * cx]), cr = float(v ? v[ptrdiff_t(cy) * cStride + cx] : u[ptrdiff_t(cy) * cStride + 2 * cx + 1]);
            o[size_t(j) * cw + i] = PackYcc(float(s / n) * sc, cb * sc, cr * sc);
        }
    return o;
}
template <class T>
inline std::vector<uint32_t> PackedClipRef(const T* y, ptrdiff_t yStride, const T* u, const T* v, ptrdiff_t cStride, int w, int h, int bits)
{
    std::vector<uint32_t> o(224 * 224);
    const int side = std::min(w, h), x0 = (w - side) / 2, y0 = (h - side) / 2;
    const float sc = 255.f / float((1 << bits) - 1);
    auto luma = [&](int xx, int yy) { return float(y[ptrdiff_t(std::clamp(yy, 0, h - 1)) * yStride + std::clamp(xx, 0, w - 1)]); };
    for (int j = 0; j < 224; ++j)
        for (int i = 0; i < 224; ++i) {
            const float fx = x0 + (i + 0.5f) * side / 224.f - 0.5f, fy = y0 + (j + 0.5f) * side / 224.f - 0.5f;
            const int ix = int(std::floor(fx)), iy = int(std::floor(fy)); const float ax = fx - ix, ay = fy - iy;
            const float l = (luma(ix, iy) * (1 - ax) + luma(ix + 1, iy) * ax) * (1 - ay) + (luma(ix, iy + 1) * (1 - ax) + luma(ix + 1, iy + 1) * ax) * ay;
            const int cx = std::clamp(int(fx) / 2, 0, w / 2 - 1), cy = std::clamp(int(fy) / 2, 0, h / 2 - 1);
            const float cb = float(v ? u[ptrdiff_t(cy) * cStride + cx] : u[ptrdiff_t(cy) * cStride + 2 * cx]), cr = float(v ? v[ptrdiff_t(cy) * cStride + cx] : u[ptrdiff_t(cy) * cStride + 2 * cx + 1]);
            o[size_t(j) * 224 + i] = PackYcc(l * sc, cb * sc, cr * sc);
        }
    return o;
}

// ------------------------------------------------------------------------------------------------ interlacing
// Field combing on moving areas of native-resolution luma (never a scaled picture: scaling removes combs).
// R = sum |Y[y-1] - 2Y[y] + Y[y+1]| / sum |Y[y-2] - 2Y[y] + Y[y+2]| over pixels that moved against the frame before.
// Progressive pictures are smooth at one-line distance (R 0.35-0.66 on 51 titles, one frame in 2237 above 1.0);
// two fields from different moments disagree line by line (woven 50/60p captures: R 1.4-4.3, 92 % of frames > 1.0).
class CombMeter {
public:
    static constexpr float kCombR = 1.0f;
    static constexpr int kNeed = 24;           // measurable (moving) frames for a verdict
    // the routes that measure while playing (the DirectShow filter, mpv, VLC): a still picture shows no combing whatever
    // its fields, so after this many frames without a verdict (~10 s) the stream counts as progressive. The routes that
    // probe ahead (Kodi, a VapourSynth clip of known length) read kProbeFlagged frames when the stream's own flags say
    // progressive, kProbe otherwise (the flags are a prior that sizes the look, never the verdict).
    static constexpr int kGiveUp = 240, kProbe = 400, kProbeFlagged = 120;
    enum Verdict { kMeasuring = 0, kProgressive = 1, kInterlaced = 2 };
    void Reset() { *this = CombMeter(); }
    Verdict verdict() const { return m_v; }
    int measured() const { return m_n; }
    int combed() const { return m_comb; }
    // y: luma of one frame (8-bit, or 16-bit words: shift = bits to drop for 8-bit codes); a centre crop of at most
    // 1280 x 720 is read (field parity kept: the crop starts on an even line)
    template <class T>
    Verdict Push(const T* y, ptrdiff_t stride, int w, int h, int shift = 0)
    {
        if (m_v != kMeasuring || !y || w < 64 || h < 64) return m_v;
        const int cw = std::min(w, 1280), chh = std::min(h, 720) & ~1, x0 = (w - cw) / 2, y0 = ((h - chh) / 2) & ~1;
        std::vector<uint8_t> cur(size_t(cw) * chh);
        for (int r = 0; r < chh; ++r) { const T* s = y + ptrdiff_t(y0 + r) * stride + x0; for (int c = 0; c < cw; ++c) cur[size_t(r) * cw + c] = uint8_t(std::min<int>(255, int(s[c]) >> shift)); }
        if (m_prev.size() == cur.size()) {
            double d1 = 0, d2 = 0; size_t moving = 0;
            for (int r = 2; r + 2 < chh; ++r)
                for (int c = 0; c < cw; ++c) {
                    const size_t i = size_t(r) * cw + c;
                    if (std::abs(int(cur[i]) - int(m_prev[i])) <= 6) continue;
                    ++moving;
                    d1 += std::abs(int(cur[i - cw]) - 2 * int(cur[i]) + int(cur[i + cw]));
                    d2 += std::abs(int(cur[i - 2 * size_t(cw)]) - 2 * int(cur[i]) + int(cur[i + 2 * size_t(cw)]));
                }
            if (moving >= 2000) {
                ++m_n; m_comb += d1 / (d2 + 1e-6) > kCombR;
                if (m_n >= kNeed) m_v = m_comb * 2 >= m_n ? kInterlaced : kProgressive;
            }
        }
        m_prev.swap(cur);
        return m_v;
    }
private:
    std::vector<uint8_t> m_prev;
    int m_n = 0, m_comb = 0;
    Verdict m_v = kMeasuring;
};

// ------------------------------------------------------------------------------------------------ frame interval
// The source frame interval from presentation timestamps: the median of the first 8 steps, then re-measured per
// segment (2 windows of 16 steps more than 8 % off). Measured on the library: "29.97"-tagged files whose body is 24p
// (nominal rate wrong on 86-91 % of frames) -> wrong on 0.1-0.4 % of frames; CFR files 0 %.
class IntervalMeter {
public:
    void Reset() { *this = IntervalMeter(); }
    // seconds; disc: a seek / flush (the step across it is not counted). Returns true when the value changed.
    bool Push(double pts, bool disc)
    {
        const double prev = m_last; m_last = pts;
        if (disc || prev < -1e17 || pts <= prev) return false;
        const double s = pts - prev;
        if (s < 0.001 || s > 0.25) return false;
        m_w.push_back(s);
        if (m_base <= 0) {
            if (m_w.size() < 8) return false;
            m_base = MedianD(m_w); m_w.clear(); return true;
        }
        if (m_w.size() < 16) return false;
        const double med = MedianD(m_w); m_w.clear();
        if (std::fabs(med - m_base) / m_base <= 0.08) { m_mis = 0; return false; }
        if (++m_mis < 2) return false;
        m_mis = 0; m_base = med; ++m_changes; return true;
    }
    double seconds() const { return m_base; }     // 0 = not measured yet
    double fps() const { return m_base > 0 ? 1.0 / m_base : 0.0; }
    uint32_t changes() const { return m_changes; }
private:
    static double MedianD(std::vector<double> v) { std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end()); return v[v.size() / 2]; }
    std::vector<double> m_w;
    double m_base = 0, m_last = -1e18;
    int m_mis = 0; uint32_t m_changes = 0;
};

} // namespace pushmpc::content
