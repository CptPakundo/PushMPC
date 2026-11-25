// pushmpc.Enhance, backend "nvidia" on Linux: the scene-cut guard. A CPU port of the decision PushMPC's GPU pair test
// makes on Windows (PushFPS VfgShotChange.hlsl + PushMPC's patch, patches/pushfps-4.5.3-pushmpc.patch; thresholds from
// PushMpcCutGuard::Params). The source planes are on the CPU here anyway, so the guard samples them directly: 64 x 36
// luma samples per frame (microseconds), no GPU pass, no readback, and the decision is known before VFG runs (a cut pair
// skips VFG entirely and shows the pair's first real frame A held, as on Windows).
//
// Ported rules: PushFPS's fixed rules and flash veto; PushMPC's adaptive rule (mean-removed SAD against a running
// baseline with attack / release, plus the 16-bin histogram), the dark rule (contrast-normalised, stretched 16-bin
// histograms), the spike rule (against the last moving pair), held pairs (animation on twos: no baseline update), and the
// active picture area (letterbox / pillarbox bars from per-column / per-row maxima over the first frames).
// Not ported (open, see track notes): the bar-change cut and per-frame bar tracking, the scene score (zoom fit, chroma,
// orientation histograms, logistic weights), block matching, the profile warp check, NVOF and Fourier-Mellin.
// Pure C++: nvl_test.cpp checks it on synthetic frames.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace pmvs::nvl {

struct CutParams {
    // PushFPS SceneCadenceAnalyzer::Config
    float cutSad = 0.3f, cutHistogram = 0.5f, replacementSad = 0.08f, replacementHistogram = 0.9f, flashSad = 0.5f, flashHistogram = 0.1f;
    // PushMpcCutGuard::Params
    float adaptiveMin = 0.08f, adaptiveRatio = 5.0f, adaptiveHistogram16 = 0.15f, baselineAttack = 0.5f, baselineRelease = 0.1f;
    float darkMean = 0.25f, darkMin = 0.7f, darkRatio = 2.5f, darkHistogram = 0.32f;
    float spikeRatio = 3.0f, spikeMin = 0.09f, spikeHistogram = 0.27f, spikeHistogramRatio = 4.0f;
    float holdSad = 0.004f;
    float barLuma = 0.10f; int barTaps = 3; int barFrames = 48;
    bool bars = true, dark = true, spike = true, adaptive = true;
};

// one frame's luma, sampled: the 64 x 36 grid of the active area and the bar profiles of the whole frame
inline constexpr int kGridX = 64, kGridY = 36, kGridN = kGridX * kGridY;
inline constexpr int kProfX = 128, kProfY = 72, kProfTaps = 32;

struct CutMeasures { float sad = 0, histogram = 0, histogram16 = 0, histogramN16 = 0, mrSad = 0, mrSadN = 0, meanA = 0, meanB = 0; };
struct CutDecision { bool cut = false, compared = false, flash = false, hold = false, fixedRule = false, adaptiveRule = false, darkRule = false, spikeRule = false; CutMeasures m; };

class CutGuard {
public:
    CutParams p;
    // area in pixels [x0, x1) x [y0, y1) (the whole frame until bars are found)
    struct Rect { int x0 = 0, y0 = 0, x1 = 0, y1 = 0; };

    void Configure(int width, int height) { m_w = width; m_h = height; Reset(); m_framesSeen = 0; m_colMax.fill(0); m_rowMax.fill(0); m_area = { 0, 0, width, height }; m_barsFixed = false; }
    void Reset() { m_haveA = false; m_baseline = 0.02f; m_baselineN = 0.25f; m_refValid = false; m_init = false; }
    const Rect& Area() const { return m_area; }
    uint64_t Cuts() const { return m_cuts; }
    uint64_t Pairs() const { return m_pairs; }

    // Push the next real frame (luma(x, y) -> 0..255 full-range 8-bit). contiguous=false: a seek or reset, never a cut.
    // Returns the decision for the pair (previous, this).
    template <class Luma> CutDecision Push(Luma&& luma, bool contiguous)
    {
        if (p.bars && !m_barsFixed) Bars(luma);
        std::array<uint8_t, kGridN> g{};
        const int rw = m_area.x1 - m_area.x0, rh = m_area.y1 - m_area.y0;
        for (int i = 0; i < kGridN; ++i) {
            const int x = m_area.x0 + int(((int64_t(i % kGridX) * 2 + 1) * rw) / (2 * kGridX));
            const int y = m_area.y0 + int(((int64_t(i / kGridX) * 2 + 1) * rh) / (2 * kGridY));
            g[size_t(i)] = uint8_t(luma(x, y));
        }
        CutDecision d;
        if (m_haveA && contiguous) d = Decide(m_a, g);
        else if (!contiguous) { m_refValid = false; }
        m_a = g; m_haveA = true;
        if (d.compared) { ++m_pairs; if (d.cut) ++m_cuts; }
        return d;
    }

    // the measures of a pair (exposed for the tests)
    static CutMeasures Measure(const std::array<uint8_t, kGridN>& a, const std::array<uint8_t, kGridN>& b)
    {
        CutMeasures m;
        std::array<int, 256> ha{}, hb{};
        uint64_t sad = 0, sa = 0, sb = 0, qa = 0, qb = 0;
        for (int i = 0; i < kGridN; ++i) {
            const int x = a[size_t(i)], y = b[size_t(i)];
            ++ha[size_t(x)]; ++hb[size_t(y)];
            sad += uint64_t(std::abs(x - y)); sa += uint64_t(x); sb += uint64_t(y); qa += uint64_t(x * x); qb += uint64_t(y * y);
        }
        int l1 = 0, l16 = 0;
        for (int k = 0; k < 256; ++k) l1 += std::abs(ha[size_t(k)] - hb[size_t(k)]);
        for (int k = 0; k < 16; ++k) {
            int p16 = 0, c16 = 0;
            for (int j = 0; j < 16; ++j) { p16 += ha[size_t(k * 16 + j)]; c16 += hb[size_t(k * 16 + j)]; }
            l16 += std::abs(p16 - c16);
        }
        const float n = float(kGridN);
        m.sad = float(sad) / (255.f * n);
        m.histogram = float(l1) / (2.f * n);
        m.histogram16 = float(l16) / (2.f * n);
        const float meanA = float(sa) / n, meanB = float(sb) / n;
        const float sigA = std::sqrt(std::max(0.f, float(qa) / n - meanA * meanA)), sigB = std::sqrt(std::max(0.f, float(qb) / n - meanB * meanB));
        auto range = [&](const std::array<int, 256>& h, float& lo, float& hi) {
            const int loCount = kGridN / 50, hiCount = kGridN - kGridN / 50;
            int acc = 0; bool lf = false, hf = false; lo = 0; hi = 255;
            for (int k = 0; k < 256; ++k) {
                acc += h[size_t(k)];
                if (!lf && acc > loCount) { lo = float(k); lf = true; }
                if (!hf && acc >= hiCount) { hi = float(k); hf = true; }
            }
        };
        float loA, hiA, loB, hiB; range(ha, loA, hiA); range(hb, loB, hiB);
        const float shift = meanB - meanA, spanA = std::max(hiA - loA, 8.f), spanB = std::max(hiB - loB, 8.f);
        uint64_t centred = 0;
        std::array<int, 16> na{}, nb{};
        for (int i = 0; i < kGridN; ++i) {
            const float x = float(a[size_t(i)]), y = float(b[size_t(i)]);
            centred += uint64_t(std::fabs(y - x - shift) * 16.f + 0.5f);
            ++na[size_t(std::clamp((x - loA) * 16.f / spanA, 0.f, 15.f))];
            ++nb[size_t(std::clamp((y - loB) * 16.f / spanB, 0.f, 15.f))];
        }
        int ln = 0;
        for (int k = 0; k < 16; ++k) ln += std::abs(na[size_t(k)] - nb[size_t(k)]);
        m.histogramN16 = float(ln) / (2.f * n);
        m.mrSad = float(centred) / (16.f * 255.f * n);
        const float contrast = std::max(0.02f, 0.5f * (sigA + sigB) / 255.f);
        m.mrSadN = m.mrSad / contrast;
        m.meanA = meanA; m.meanB = meanB;
        return m;
    }

private:
    CutDecision Decide(const std::array<uint8_t, kGridN>& a, const std::array<uint8_t, kGridN>& b)
    {
        CutDecision d;
        d.compared = true;
        const CutMeasures m = Measure(a, b);
        d.m = m;
        d.flash = m.sad > p.flashSad && m.histogram < p.flashHistogram;
        d.fixedRule = (m.sad > p.cutSad && m.histogram > p.cutHistogram) || (m.sad > p.replacementSad && m.histogram > p.replacementHistogram);
        d.hold = m.sad < p.holdSad && m.mrSad < p.holdSad;
        if (!(m_baseline >= 0.f && m_baseline < 1.f)) m_baseline = 0.02f;
        if (!(m_baselineN >= 0.f && m_baselineN < 50.f)) m_baselineN = 0.25f;
        const float threshold = std::max(p.adaptiveMin, p.adaptiveRatio * m_baseline);
        d.adaptiveRule = p.adaptive && p.adaptiveMin > 0.f && m.mrSad > threshold && m.histogram16 > p.adaptiveHistogram16;
        const bool dark = p.dark && std::max(m.meanA, m.meanB) < p.darkMean * 255.f;
        d.darkRule = dark && m.mrSadN > std::max(p.darkMin, p.darkRatio * m_baselineN) && m.histogramN16 > p.darkHistogram;
        d.spikeRule = p.spike && m_refValid && m.mrSad > std::max(p.spikeMin, p.spikeRatio * m_refMrSad)
                   && (m.histogram16 > std::max(p.spikeHistogram, p.spikeHistogramRatio * m_refHist16) || (dark && m.histogramN16 > p.darkHistogram));
        d.cut = !d.flash && !d.hold && (d.fixedRule || d.adaptiveRule || d.darkRule || d.spikeRule);
        if (!d.hold) { m_refMrSad = m.mrSad; m_refHist16 = m.histogram16; m_refValid = true; }
        if (!d.cut && !d.hold) {
            m_baseline += (m.mrSad > m_baseline ? p.baselineAttack : p.baselineRelease) * (m.mrSad - m_baseline);
            m_baselineN += (m.mrSadN > m_baselineN ? p.baselineAttack : p.baselineRelease) * (m.mrSadN - m_baselineN);
        }
        return d;
    }

    // per-column / per-row maxima of the first barFrames frames, then the bars once (near-black runs of about the same
    // width at opposite edges, at least half the frame left; a bin is bar when at most barTaps of its taps are brighter)
    template <class Luma> void Bars(Luma&& luma)
    {
        const int thr = int(p.barLuma * 255.f + 0.5f);
        for (int c = 0; c < kProfX; ++c) {
            const int x = int(((int64_t(c) * 2 + 1) * m_w) / (2 * kProfX));
            int bright = 0, mx = 0;
            for (int t = 0; t < kProfTaps; ++t) { const int v = int(luma(x, int(((int64_t(t) * 2 + 1) * m_h) / (2 * kProfTaps)))); mx = std::max(mx, v); bright += v > thr; }
            m_colMax[size_t(c)] = std::max(m_colMax[size_t(c)], bright > p.barTaps ? 255 : std::min(mx, thr));
        }
        for (int r = 0; r < kProfY; ++r) {
            const int y = int(((int64_t(r) * 2 + 1) * m_h) / (2 * kProfY));
            int bright = 0, mx = 0;
            for (int t = 0; t < kProfTaps; ++t) { const int v = int(luma(int(((int64_t(t) * 2 + 1) * m_w) / (2 * kProfTaps)), y)); mx = std::max(mx, v); bright += v > thr; }
            m_rowMax[size_t(r)] = std::max(m_rowMax[size_t(r)], bright > p.barTaps ? 255 : std::min(mx, thr));
        }
        if (++m_framesSeen < p.barFrames) return;
        m_barsFixed = true;
        auto run = [&](const auto& v, int n, bool fromEnd) { int k = 0; while (k < n && v[size_t(fromEnd ? n - 1 - k : k)] <= thr) ++k; return k; };
        int l = run(m_colMax, kProfX, false), r = run(m_colMax, kProfX, true), t = run(m_rowMax, kProfY, false), b = run(m_rowMax, kProfY, true);
        if (std::abs(l - r) > 2 || l + r >= kProfX / 2) l = r = 0;     // not a symmetric pillarbox
        if (std::abs(t - b) > 2 || t + b >= kProfY / 2) t = b = 0;     // not a symmetric letterbox
        // a bin partly in the picture counts as bar: keep a bin's margin inside the picture
        Rect a;
        a.x0 = ((l + (l ? 1 : 0)) * m_w) / kProfX; a.x1 = m_w - ((r + (r ? 1 : 0)) * m_w) / kProfX;
        a.y0 = ((t + (t ? 1 : 0)) * m_h) / kProfY; a.y1 = m_h - ((b + (b ? 1 : 0)) * m_h) / kProfY;
        if (a.x1 > a.x0 + m_w / 4 && a.y1 > a.y0 + m_h / 4) m_area = a;
    }

    int m_w = 0, m_h = 0, m_framesSeen = 0;
    bool m_barsFixed = false, m_haveA = false, m_refValid = false, m_init = false;
    std::array<uint8_t, kGridN> m_a{};
    std::array<int, kProfX> m_colMax{};
    std::array<int, kProfY> m_rowMax{};
    Rect m_area;
    float m_baseline = 0.02f, m_baselineN = 0.25f, m_refMrSad = 0, m_refHist16 = 0;
    uint64_t m_cuts = 0, m_pairs = 0;
};

} // namespace pmvs::nvl
