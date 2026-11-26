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

enum Class : uint8_t { kUnknown = 0, kDrawn = 1, kCg = 2, kLive = 3, kGame = 4 };
inline const char* ClassName(int c)
{
    static const char* n[] = { "measuring", "hand-drawn animation", "3D CG animation", "live action", "game footage" };
    return c >= 0 && c <= 4 ? n[c] : n[0];
}

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
    // luma: the mean of every second row and column of the cell (a 4x4 cell reads 4 pixels; the mean of a cell barely
    // moves, and this keeps the cost near 0.5 ms at 1080p)
    for (int j = 0; j < c.h; ++j) {
        for (int i = 0; i < c.w; ++i) {
            double s = 0; int n = 0;
            const int xs = x0[i], xe = std::max(x0[i + 1], xs + 1), ysr = y0[j], ye = std::max(y0[j + 1], ysr + 1);
            const int st = (xe - xs) >= 4 ? 2 : 1, sr = (ye - ysr) >= 4 ? 2 : 1;
            for (int yy = ysr; yy < ye; yy += sr) { const T* r = y + ptrdiff_t(yy) * yStride; for (int xx = xs; xx < xe; xx += st) { s += r[xx]; ++n; } }
            float m = float(s / std::max(1, n)) * scale;
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
inline void Fft(std::vector<std::complex<float>>& a, bool inv)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) { size_t bit = n >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) std::swap(a[i], a[j]); }
    for (size_t len = 2; len <= n; len <<= 1) {
        const float ang = 6.2831853f / float(len) * (inv ? 1.f : -1.f);
        const std::complex<float> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> wv(1.f, 0.f);
            for (size_t k = 0; k < len / 2; ++k) { const auto u = a[i + k], t = a[i + k + len / 2] * wv; a[i + k] = u + t; a[i + k + len / 2] = u - t; wv *= wl; }
        }
    }
}
// 2-D FFT of a W x H grid zero-padded to pw x ph (powers of two), in place, row-major
inline void Fft2(std::vector<std::complex<float>>& g, int pw, int ph, bool inv)
{
    std::vector<std::complex<float>> line;
    line.resize(size_t(pw));
    for (int y = 0; y < ph; ++y) { std::copy_n(g.begin() + ptrdiff_t(y) * pw, pw, line.begin()); Fft(line, inv); std::copy_n(line.begin(), pw, g.begin() + ptrdiff_t(y) * pw); }
    line.resize(size_t(ph));
    for (int x = 0; x < pw; ++x) { for (int y = 0; y < ph; ++y) line[size_t(y)] = g[size_t(y) * pw + x]; Fft(line, inv); for (int y = 0; y < ph; ++y) g[size_t(y) * pw + x] = line[size_t(y)]; }
}
inline int Pow2(int v) { int p = 1; while (p < v) p <<= 1; return p; }
// area mean of a w x h grid into ow x oh (bin edges rounded as the offline extractor's reduceat)
inline std::vector<float> Area(const std::vector<float>& a, int w, int h, int ow, int oh)
{
    std::vector<float> o(size_t(ow) * oh, 0.f);
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
enum Feature { fBActive, fBHold, fBStep2, fHold, fTwos, fStep, fRun1, fRun2, fRun3, fRun4, fDup, fChg, fCam, fHud, fFps60, fActive, fFlat, fTex, fEdge, fNoise, fEnt, kFeatureCount };

struct State {
    int cls = kUnknown;            // decided class (kUnknown while measuring)
    float prob[5] = {};            // smoothed class probabilities (index = Class)
    float confidence = 0.f;        // prob of the decided class
    double firstDecisionSec = -1;  // stream time of the first decision (-1 none yet)
    uint32_t changes = 0;          // decided-class changes since the stream started (flip count)
    uint32_t windows = 0;          // windows evaluated
    float drawnShare = 0.f;        // smoothed probability of drawn animation (for "live action with drawn overlays")
    float feat[kFeatureCount] = {};// the last window's raw features (log, tests)
};

class ContentDetector {
public:
    // Evaluation cadence and window (stream time)
    static constexpr double kWindowSec = 2.0, kStepSec = 0.5, kHudSec = 6.0, kFirstSec = 1.5;
    static constexpr float kFirstProb = 0.55f, kAlpha = 0.20f, kMargin = 0.30f;
    static constexpr int kHoldSteps = 4;

    void Reset() { *this = ContentDetector(); }
    // A discontinuity (seek, flush): the window and the previous frame are dropped, the decision stays (same title).
    void Discontinuity() { m_frames.clear(); m_prev = Cells(); m_nextEval = -1; m_cutPending = true; }
    const State& Current() const { return m_state; }

    // One source frame in presentation order; pts in seconds (stream time). Returns true when a window was evaluated.
    bool Push(const Cells& c, double pts)
    {
        if (!c.ok()) return false;
        if (m_prev.ok() && (m_prev.w != c.w || m_prev.h != c.h)) Discontinuity();
        Frame f; f.pts = pts;
        Spatial(c, f);
        if (!m_prev.ok()) {
            f.first = true; f.cut = true;
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
        // boosted trees (depth 3, one per class per round): x >= threshold goes right
        double z[5] = { 0, 0, 0, 0, 0 };
        for (int t = 0; t < kModelTrees; ++t) {
            int node = 0;
            for (int level = 0; level < 3; ++level) {
                const int j = kModelFeat[t][node];
                node = 2 * node + 1 + (j >= 0 && x[j] >= kModelThr[t][node] ? 1 : 0);
            }
            z[1 + t % 4] += kModelRate * kModelLeaf[t][node - 7];
        }
        double m = std::max(std::max(z[1], z[2]), std::max(z[3], z[4])), sum = 0, p[5] = {};
        for (int k = 1; k <= 4; ++k) { p[k] = std::exp(z[k] - m); sum += p[k]; }
        for (int k = 1; k <= 4; ++k) p[k] /= sum;
        State& s = m_state;
        std::copy(x, x + kFeatureCount, s.feat);
        ++s.windows;
        for (int k = 1; k <= 4; ++k) s.prob[k] = s.windows == 1 ? float(p[k]) : s.prob[k] + kAlpha * (float(p[k]) - s.prob[k]);
        int best = 1;
        for (int k = 2; k <= 4; ++k) if (s.prob[k] > s.prob[best]) best = k;
        if (m_t0 < 0) m_t0 = pts - 1.0;
        const bool isCut = cut < 0 ? m_cutPending : cut != 0; m_cutPending = false;
        if (s.cls == kUnknown) {
            if (pts - m_t0 >= kFirstSec && s.prob[best] >= kFirstProb) { s.cls = best; s.firstDecisionSec = pts; }
        } else if (best != s.cls && s.prob[best] - s.prob[s.cls] >= kMargin) {
            if (++m_run >= (isCut ? kHoldSteps / 2 : kHoldSteps)) { s.cls = best; m_run = 0; ++s.changes; }
        } else m_run = 0;
        s.confidence = s.cls == kUnknown ? 0.f : s.prob[s.cls];
        s.drawnShare = s.prob[kDrawn];
    }

private:
    struct Frame {
        double pts = 0;
        bool first = false, cut = false, dup = false;
        float chgShare = 0, cam = 0;
        float flat = 0, tex = 0, edge = 0, noise = -1, ent = 0;
        std::vector<uint64_t> chg, big;   // per cell bits (row-major)
        std::vector<uint8_t> grad;         // coarse texture (kHudW x hudH)
        std::vector<uint8_t> coarse;       // share of changed cells per coarse cell (0..255)
    };
    static constexpr int kHudW = 60;
    static size_t Words(const Cells& c) { return (size_t(c.w) * c.h + 63) / 64; }
    static bool Bit(const std::vector<uint64_t>& b, size_t i) { return (b[i >> 6] >> (i & 63)) & 1u; }

    static std::array<float, 32> Histogram(const Cells& c)
    {
        std::array<float, 32> h{};
        for (float v : c.y) h[std::min(31, int(v / 8.f))] += 1.f;
        for (auto& v : h) v /= float(c.y.size());
        return h;
    }
    // flat / textured / line cells, the noise of flat areas and the colour entropy of the cell grid
    void Spatial(const Cells& c, Frame& f) const
    {
        const int w = c.w, h = c.h;
        size_t nf = 0, nt = 0, ne = 0, n = 0;
        std::vector<float> g(size_t(w) * h, 0.f), res;
        for (int y = 0; y + 1 < h; ++y)
            for (int x = 0; x + 1 < w; ++x) {
                const float v = c.y[size_t(y) * w + x];
                const float gg = std::max(std::fabs(c.y[size_t(y) * w + x + 1] - v), std::fabs(c.y[size_t(y + 1) * w + x] - v));
                g[size_t(y) * w + x] = gg; ++n;
                if (gg <= 1.f) ++nf; else if (gg <= 6.f) ++nt;
                if (gg > 16.f) ++ne;
            }
        f.flat = n ? float(nf) / n : 0; f.tex = n ? float(nt) / n : 0; f.edge = n ? float(ne) / n : 0;
        for (int y = 1; y + 1 < h; ++y)
            for (int x = 1; x + 1 < w; ++x) {
                if (g[size_t(y - 1) * w + x - 1] >= 4.f) continue;   // the extractor's mask is offset by one cell
                const float b = (c.y[size_t(y - 1) * w + x] + c.y[size_t(y + 1) * w + x] + c.y[size_t(y) * w + x - 1] + c.y[size_t(y) * w + x + 1]) * 0.25f;
                res.push_back(std::fabs(c.y[size_t(y) * w + x] - b));
            }
        f.noise = res.size() > 100 ? detail::Median(std::move(res)) : -1.f;
        std::vector<uint32_t> hist(4096, 0);
        const bool chroma = c.cb.size() == c.y.size() && c.cr.size() == c.y.size();
        for (size_t i = 0; i < c.y.size(); ++i) {
            const int qy = std::clamp(int(c.y[i] / 16.f), 0, 15);
            const int qb = chroma ? std::clamp(int(std::floor(c.cb[i] / 8.f)) + 8, 0, 15) : 8, qr = chroma ? std::clamp(int(std::floor(c.cr[i] / 8.f)) + 8, 0, 15) : 8;
            ++hist[size_t(qy) * 256 + qb * 16 + qr];
        }
        double e = 0; const double tot = double(c.y.size());
        for (uint32_t k : hist) if (k) { const double p = k / tot; e -= p * std::log2(p); }
        f.ent = float(e);
        // coarse texture for the overlay test: the cell gradient area-averaged to 60 across, in the extractor's
        // units (its gradient is taken at twice the cell resolution, so half the cell gradient)
        const int hh = std::max(1, int(std::lround(double(kHudW) * h / w)));
        const auto a = detail::Area(g, w, h, kHudW, hh);
        f.grad.resize(a.size());
        for (size_t i = 0; i < a.size(); ++i) f.grad[i] = uint8_t(std::clamp(a[i] * 0.5f, 0.f, 255.f));
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
        std::vector<float> best = d0; double bestMean = mean;
        if (dx || dy) {
            std::vector<float> ds(n);
            for (int ox = -1; ox <= 1; ++ox)
                for (int oy = -1; oy <= 1; ++oy) {
                    const int sx = dx + ox, sy = dy + oy; double m = 0;
                    for (int y = 0; y < h; ++y) {
                        const int py = std::clamp(y - sy, 0, h - 1);
                        for (int x = 0; x < w; ++x) { const int px = std::clamp(x - sx, 0, w - 1); const float v = std::fabs(c.y[size_t(y) * w + x] - m_prev.y[size_t(py) * w + px]); ds[size_t(y) * w + x] = v; m += v; }
                    }
                    m /= double(n);
                    if (m < bestMean) { bestMean = m; best = ds; f.cam = 1.f; }
                }
        }
        f.chg.assign(Words(c), 0); f.big.assign(Words(c), 0);
        size_t nc = 0;
        for (size_t i = 0; i < n; ++i) {
            const float d = std::min(d0[i], best[i]);
            if (d > thr) { f.chg[i >> 6] |= 1ull << (i & 63); ++nc; }
            if (d > 3.f * thr) f.big[i >> 6] |= 1ull << (i & 63);
        }
        f.chgShare = float(nc) / float(n);
        {   // per coarse cell (kHudW across): the share of its cells that changed, for the overlay test
            // bin edges rounded over the whole grid (the bottom rows, where HUDs sit, are not cut off)
            const int gw = kHudW, gh = std::max(1, int(std::lround(double(kHudW) * h / w)));
            f.coarse.assign(size_t(gw) * gh, 0);
            for (int gy = 0; gy < gh; ++gy)
                for (int gx = 0; gx < gw; ++gx) {
                    const int y0 = int(std::lround(double(gy) * h / gh)), y1 = std::max(y0 + 1, int(std::lround(double(gy + 1) * h / gh)));
                    const int x0 = int(std::lround(double(gx) * w / gw)), x1 = std::max(x0 + 1, int(std::lround(double(gx + 1) * w / gw)));
                    int c = 0;
                    for (int y = y0; y < y1 && y < h; ++y) for (int xx = x0; xx < x1 && xx < w; ++xx) c += Bit(f.chg, size_t(y) * w + xx);
                    f.coarse[size_t(gy) * gw + gx] = uint8_t(std::lround(255.0 * c / double((y1 - y0) * (x1 - x0))));
                }
        }
        f.dup = over8 <= std::max<size_t>(3, n / 20000) && mean < 3.0;
        if (f.dup) {
            std::vector<float> s = d0; const size_t k = n * 99 / 100; std::nth_element(s.begin(), s.begin() + k, s.end());
            m_nf = m_nfN == 0 ? s[k] : m_nf + (s[k] - m_nf) * 0.05f; ++m_nfN;
        }
        const auto hs = Histogram(c); float l1 = 0; for (int k = 0; k < 32; ++k) l1 += std::fabs(hs[k] - m_hist[k]);
        f.cut = l1 >= 0.30f && f.chgShare > 0.5f;
    }
    static void PhaseShift(const Cells& a, const Cells& b, int& dx, int& dy)
    {
        using cf = std::complex<float>;
        const int w2 = a.w / 2, h2 = std::max(1, a.h / 2);
        const auto A = detail::Area(a.y, a.w, a.h, w2, h2), B = detail::Area(b.y, b.w, b.h, w2, h2);
        const int pw = detail::Pow2(w2), ph = detail::Pow2(h2);
        double ma = 0, mb = 0; for (size_t i = 0; i < A.size(); ++i) { ma += A[i]; mb += B[i]; } ma /= A.size(); mb /= B.size();
        std::vector<cf> fa(size_t(pw) * ph), fb(size_t(pw) * ph);
        for (int y = 0; y < h2; ++y) for (int x = 0; x < w2; ++x) { fa[size_t(y) * pw + x] = float(A[size_t(y) * w2 + x] - ma); fb[size_t(y) * pw + x] = float(B[size_t(y) * w2 + x] - mb); }
        detail::Fft2(fa, pw, ph, false); detail::Fft2(fb, pw, ph, false);
        for (size_t i = 0; i < fa.size(); ++i) { const cf r = fb[i] * std::conj(fa[i]); const float m = std::abs(r); fa[i] = r / (m + 1e-6f); }
        detail::Fft2(fa, pw, ph, true);
        size_t bi = 0; float bv = -1e30f;
        for (size_t i = 0; i < fa.size(); ++i) if (fa[i].real() > bv) { bv = fa[i].real(); bi = i; }
        int py = int(bi / pw), px = int(bi % pw);
        if (py > ph / 2) py -= ph;
        if (px > pw / 2) px -= pw;
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
        for (size_t t = 0; t < T; ++t) { const auto& b = m_frames[i0 + 1 + t].chg; for (size_t i = 0; i < n; ++i) cnt[i] += uint16_t(Bit(b, i)); }
        std::vector<uint32_t> act; act.reserve(n);
        for (size_t i = 0; i < n; ++i) if (cnt[i] >= 2) act.push_back(uint32_t(i));
        const size_t na = act.size();
        std::fill(x, x + kFeatureCount, 0.f);
        x[fActive] = float(na) / float(n);
        {   // cells that make >= 2 BIG steps (> 3x the noise threshold): drawn parts jump between drawings and hold
            // still in between; live action that moves that much moves on every frame
            std::vector<uint16_t> bc(n, 0);
            for (size_t t = 0; t < T; ++t) { const auto& b = m_frames[i0 + 1 + t].big; for (size_t i = 0; i < n; ++i) bc[i] += uint16_t(Bit(b, i)); }
            std::vector<uint32_t> ba; for (size_t i = 0; i < n; ++i) if (bc[i] >= 2) ba.push_back(uint32_t(i));
            x[fBActive] = float(ba.size()) / float(n);
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
                x[fBHold] = float(hs / ba.size()); x[fBStep2] = float(double(step2) / ba.size());
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
        x[fDup] = float(dup / T); x[fChg] = float(chg / T); x[fCam] = float(cam / T);
        std::vector<float> fl, tx, ed, en, no;
        for (size_t t = i0; t < i1; ++t) { fl.push_back(m_frames[t].flat); tx.push_back(m_frames[t].tex); ed.push_back(m_frames[t].edge); en.push_back(m_frames[t].ent); if (m_frames[t].noise >= 0) no.push_back(m_frames[t].noise); }
        x[fFlat] = detail::Median(fl); x[fTex] = detail::Median(tx); x[fEdge] = detail::Median(ed); x[fEnt] = detail::Median(en); x[fNoise] = no.empty() ? 0.f : detail::Median(no);
        x[fHud] = Hud(t1, i1);
        // frame rate from the window's timestamps (a 50 / 60 fps source is a prior for game footage, never alone)
        std::vector<float> st;
        for (size_t t = i0 + 1; t < i1; ++t) { const double s = m_frames[t].pts - m_frames[t - 1].pts; if (s > 0.001 && s < 0.25) st.push_back(float(s)); }
        x[fFps60] = !st.empty() && 1.0 / detail::Median(st) > 45.0 ? 1.f : 0.f;
        return true;
    }
    // overlays that stay while the scene moves: coarse cells that never change for 6 s, textured, in a moving picture
    float Hud(double t1, size_t i1) const
    {
        size_t j0 = 0;
        while (j0 < i1 && m_frames[j0].pts < t1 - kHudSec) ++j0;
        if (i1 - j0 < 24) return 0.f;
        double chg = 0; for (size_t t = j0 + 1; t < i1; ++t) chg += m_frames[t].chgShare;
        if (chg / double(i1 - j0 - 1) <= 0.15) return 0.f;
        const size_t cells = m_frames.back().coarse.size();
        if (!cells) return 0.f;
        std::vector<uint16_t> still(cells, 0), moving(cells, 0);
        const size_t T = i1 - j0 - 1;
        for (size_t t = j0 + 1; t < i1; ++t) {
            const auto& cc = m_frames[t].coarse;
            if (cc.size() != cells) continue;
            for (size_t i = 0; i < cells; ++i) { still[i] += cc[i] <= 5; moving[i] += cc[i] > 51; }   // < 2 %, > 20 % (8-bit shares)
        }
        size_t movingCells = 0, hud = 0;
        for (size_t i = 0; i < still.size(); ++i) {
            if (double(moving[i]) / T > 0.3) ++movingCells;
            std::vector<float> g; for (size_t t = j0; t < i1; ++t) if (m_frames[t].grad.size() == still.size()) g.push_back(m_frames[t].grad[i]);
            if (double(still[i]) / T > 0.97 && !g.empty() && detail::Median(g) > 6.f) ++hud;
        }
        return double(movingCells) / still.size() > 0.2 ? float(double(hud) / still.size()) : 0.f;
    }

    std::deque<Frame> m_frames;
    Cells m_prev;
    std::array<float, 32> m_hist{};
    float m_nf = 0; uint32_t m_nfN = 0;
    double m_nextEval = -1, m_t0 = -1;
    bool m_cutPending = true;
    int m_run = 0;
    State m_state;

public:
    // ---- MODEL (generated by the offline evaluation: boosted trees, depth 3, one per class per round; features in
    // Feature order, raw values; x >= threshold goes right; feature -1 = no split)
    static constexpr int kModelVersion = 2;
    static constexpr int kModelTrees = 480;
    static constexpr double kModelRate = 0.15;
    static constexpr int8_t kModelFeat[kModelTrees][7] = {
        { 5, 12, 11, 1, 6, 7, 16 },
        { 6, 3, 17, 18, 7, 18, -1 },
        { 1, 14, 9, 5, -1, 12, 10 },
        { 14, 15, -1, 18, 7, -1, -1 },
        { 1, 12, 18, 5, 11, 12, 15 },
        { 3, 18, 7, 20, 19, 10, 19 },
        { 1, 14, 9, 5, -1, 5, 4 },
        { 14, 15, -1, 18, 1, -1, -1 },
        { 5, 20, 11, 12, 16, 7, 7 },
        { 6, 3, 9, 18, 9, 17, 18 },
        { 5, 14, 3, 7, -1, 7, 9 },
        { 14, 15, -1, 18, 12, -1, -1 },
        { 5, 20, 11, 12, 0, 19, 5 },
        { 6, 3, 8, 19, 9, 4, 8 },
        { 1, 14, 9, 16, -1, 20, 18 },
        { 14, 15, -1, 7, 1, -1, -1 },
        { 10, 17, 20, 16, 20, 19, 14 },
        { 6, 3, 9, 16, 7, 10, 18 },
        { 10, 5, 20, 16, 7, 9, 1 },
        { 14, 15, -1, 18, 12, -1, -1 },
        { 5, 0, 4, 20, 16, 11, 5 },
        { 6, 3, 8, 19, 9, 1, 8 },
        { 5, 14, 1, 8, -1, 16, 9 },
        { 14, 5, -1, 1, 5, -1, -1 },
        { 10, 12, 20, 8, 9, 19, 14 },
        { 3, 20, 7, 20, 8, 20, 19 },
        { 10, 8, 20, 19, 8, 9, 16 },
        { 14, 5, -1, 1, 15, -1, -1 },
        { 6, 18, 5, 12, 11, 20, 17 },
        { 16, 17, 20, 6, 20, 11, 3 },
        { 5, 18, 3, 20, 14, 12, 19 },
        { 14, 5, -1, 3, 12, -1, -1 },
        { 10, 17, 20, 16, 4, 6, 15 },
        { 16, 17, 20, 18, 7, 3, 19 },
        { 7, 7, 1, 6, 16, 17, 12 },
        { 14, 5, -1, 1, 12, -1, -1 },
        { 18, 18, 5, 15, 17, 8, -1 },
        { 3, 20, 7, 2, 16, 20, 19 },
        { 3, 9, 18, 3, 16, 6, 16 },
        { 14, 7, -1, 18, 5, -1, -1 },
        { 6, 18, 20, 2, 11, 17, 18 },
        { 6, 3, 18, 19, 9, 16, 16 },
        { 3, 9, 2, 3, 3, 16, 18 },
        { 14, 7, -1, 15, 8, -1, -1 },
        { 4, 20, 5, -1, 17, 4, 1 },
        { 16, 17, 20, -1, 7, 3, 17 },
        { 18, 20, 1, 17, 3, 14, -1 },
        { 14, 0, -1, 19, 12, -1, -1 },
        { 12, 9, 18, 0, 7, 6, -1 },
        { 16, 17, 19, 9, 7, 19, 5 },
        { 7, 18, 3, 6, 10, 5, 18 },
        { 14, 12, -1, 11, 7, -1, -1 },
        { 18, 18, 5, 12, 19, 15, -1 },
        { 20, 11, 18, 3, 3, 19, 7 },
        { 5, 20, 7, 20, 16, 16, 18 },
        { 14, 0, -1, 19, 18, -1, -1 },
        { 5, 11, 5, 20, -1, 18, 18 },
        { 18, 15, 7, -1, 1, 8, 18 },
        { 18, 20, 1, 17, 5, 14, -1 },
        { 14, 5, -1, 1, 12, -1, -1 },
        { 20, 8, 17, -1, -1, 18, 19 },
        { 20, 15, 16, 18, 3, 17, 19 },
        { 5, 16, 19, 19, 20, 16, 16 },
        { 14, 12, -1, 11, 16, -1, -1 },
        { 18, 19, 16, 17, 3, 10, 19 },
        { 20, 20, 18, 11, 18, -1, 17 },
        { 7, 7, 6, 18, 16, 17, 5 },
        { 12, 20, 7, 4, 15, 12, -1 },
        { 4, 20, 5, -1, 20, 3, 2 },
        { 7, 18, 16, 11, 17, 8, -1 },
        { 8, 8, 19, 18, 16, 16, 3 },
        { 5, 7, 8, 15, 12, 17, 18 },
        { 12, 5, 3, 5, 18, -1, 15 },
        { 8, 20, 11, 20, 18, 17, -1 },
        { 4, 5, 7, 20, 9, 16, 18 },
        { 14, 7, -1, 18, 8, -1, -1 },
        { 18, 20, 5, 7, 0, 17, -1 },
        { 15, 20, 7, 11, 18, -1, 19 },
        { 18, 7, 1, 8, 5, 14, -1 },
        { 14, 11, -1, 19, 18, -1, -1 },
        { 20, -1, 17, -1, -1, -1, 20 },
        { 7, 20, 3, 10, 20, 18, 17 },
        { 18, 19, 16, 18, 19, 19, 0 },
        { 14, 5, -1, 10, 1, -1, -1 },
        { 2, 20, 7, 19, 17, 12, 3 },
        { 20, 3, 17, 17, 17, 17, 10 },
        { 16, 19, 19, 2, 16, 15, 18 },
        { 5, 8, 8, 8, 15, 20, 4 },
        { 20, 8, 17, -1, -1, 20, 19 },
        { 18, 18, 16, 10, 19, 16, 12 },
        { 18, 11, 1, 2, 4, 14, -1 },
        { 12, 18, 16, 1, 15, -1, 5 },
        { 18, 5, 19, 19, 18, 16, 0 },
        { 20, 3, 9, 6, 5, 6, 18 },
        { 7, 8, 18, 19, -1, 1, 16 },
        { 12, 17, 4, 17, 10, 5, 16 },
        { 20, -1, 17, -1, -1, -1, 12 },
        { 18, -1, 7, -1, -1, 8, 3 },
        { 12, 20, 18, 15, 12, 3, -1 },
        { 14, 11, -1, 18, 8, -1, -1 },
        { 5, 15, 5, 4, 6, 2, 7 },
        { 20, 18, 18, 19, 5, -1, 7 },
        { 15, 2, 0, 16, 15, 20, 16 },
        { 14, 18, -1, 12, 7, -1, -1 },
        { 19, 1, 17, 20, 3, 18, 19 },
        { 3, 20, 20, 1, 12, 12, 7 },
        { 18, 20, 16, 15, -1, 19, 9 },
        { 17, 19, 4, 18, 5, -1, -1 },
        { 10, 16, 17, 20, 20, 19, -1 },
        { 19, 19, 16, 7, 18, 7, 5 },
        { 6, 3, 11, 1, 7, 17, 3 },
        { 11, 14, 12, 20, -1, 3, 16 },
        { 18, 20, -1, 7, 0, -1, -1 },
        { 8, 18, -1, 15, 20, -1, -1 },
        { 16, 19, 2, 20, 18, 11, 15 },
        { 5, 5, 19, 10, 1, 8, 15 },
        { 18, 20, -1, 18, 0, -1, -1 },
        { 19, 19, 16, 3, 18, 7, 18 },
        { 15, 6, 0, 16, 9, 16, 16 },
        { 12, 0, 7, 19, 6, -1, 14 },
        { 20, -1, 17, -1, -1, 15, 19 },
        { 20, 15, 17, 11, 17, 12, 19 },
        { 19, 16, 18, 17, 20, 20, 16 },
        { 14, 18, -1, 19, 8, -1, -1 },
        { 4, 20, 18, 16, 18, 17, 17 },
        { 7, 19, 17, 17, 16, -1, 8 },
        { 8, 8, 19, 19, 16, 15, 7 },
        { 12, 11, 7, 18, 3, 15, 17 },
        { 2, 8, 19, 16, 6, 4, 18 },
        { 8, 7, 11, 15, 19, 5, -1 },
        { 18, 19, 16, 18, 19, 19, 1 },
        { 14, 5, -1, 16, 1, -1, -1 },
        { 10, 17, 16, 20, 20, 11, 17 },
        { 20, 18, 18, 18, 4, -1, 1 },
        { 12, 8, 18, 2, 8, 16, -1 },
        { 17, 19, 16, 20, 19, -1, -1 },
        { 20, 15, 0, 20, 5, 15, 16 },
        { 20, 20, 18, 19, 8, -1, 4 },
        { 20, 1, 19, 5, 15, 16, 16 },
        { 14, 7, -1, 18, 20, -1, -1 },
        { 20, 18, 18, 15, 20, 7, 5 },
        { 18, 15, 11, -1, -1, 16, 12 },
        { 18, 20, 10, 18, 18, 1, -1 },
        { 5, 17, 20, 19, 10, 8, -1 },
        { 20, -1, 17, -1, -1, 20, 20 },
        { 11, 7, 17, 7, 3, 12, -1 },
        { 18, 20, 16, 19, 19, 1, 20 },
        { 17, 17, 3, 3, 12, 15, -1 },
        { 19, 10, 11, 17, 1, -1, 3 },
        { 17, 16, 10, 18, 18, 19, -1 },
        { 15, 0, 0, 18, 17, 20, 16 },
        { 18, 11, 11, 5, -1, 14, 4 },
        { 3, 15, 15, 4, 17, 19, 19 },
        { 15, 18, 7, 18, 12, -1, 16 },
        { 12, 15, 8, 3, 18, 15, 17 },
        { 14, 1, -1, 18, 11, -1, -1 },
        { 20, -1, 17, -1, -1, 1, 16 },
        { 18, 15, 20, -1, -1, 20, 18 },
        { 18, 19, 16, 18, 20, 5, 19 },
        { 3, 5, 11, 18, 16, 6, -1 },
        { 20, 11, 16, 4, 17, 19, 7 },
        { 4, 3, 16, 6, 20, 8, -1 },
        { 0, 15, 18, 12, 3, 3, 18 },
        { 5, 7, 8, 15, 7, 11, 4 },
        { 10, 1, 16, 15, 17, 11, 16 },
        { 19, 17, 16, 3, 16, 5, 0 },
        { 18, 12, 4, 6, 20, 19, 16 },
        { 18, 1, 8, 5, 8, 11, 14 },
        { 9, 2, 2, 8, 19, 20, 3 },
        { 15, 11, 19, 12, 17, -1, 16 },
        { 19, 17, 16, 19, 16, 20, 12 },
        { 17, 19, 8, 20, 5, 20, 10 },
        { 18, 3, 5, 19, 15, 11, -1 },
        { 19, 3, 17, 1, 5, 12, 5 },
        { 11, 0, 7, 19, 5, 1, 0 },
        { 14, 17, -1, 11, 11, -1, -1 },
        { 5, 20, 17, 17, 17, 11, 8 },
        { 20, 17, 18, 19, 7, 9, 20 },
        { 8, 8, 20, 18, 19, -1, -1 },
        { 18, 12, 8, 7, 0, 11, 15 },
        { 2, 7, 5, 6, 16, 11, 7 },
        { 15, 20, 7, 20, 18, -1, -1 },
        { 17, 15, 20, 0, 18, 5, 17 },
        { 14, 5, -1, 12, 4, -1, -1 },
        { 7, 20, 20, 15, 19, 19, 11 },
        { 17, 16, 5, 9, 19, -1, 7 },
        { 3, 6, 2, 7, 19, 17, 19 },
        { 8, 12, 15, 18, 17, 19, 5 },
        { 18, 19, -1, 16, 17, -1, -1 },
        { 7, 2, 16, 18, 18, -1, -1 },
        { 19, 18, 18, 18, 16, 20, 16 },
        { 4, 11, 3, 18, 18, 11, 20 },
        { 9, 2, 18, 20, 5, 5, 20 },
        { 17, 16, 5, 8, 18, -1, 12 },
        { 4, 5, 5, 20, 20, 4, 18 },
        { 3, 14, 11, 18, -1, 10, 5 },
        { 20, 11, 5, 19, 17, 4, 11 },
        { 8, 18, 5, 2, 17, 7, 2 },
        { 5, 11, 9, 19, 16, 3, -1 },
        { 12, 1, 18, 7, 11, 20, 18 },
        { 2, 2, 17, 17, 17, 12, 0 },
        { 19, 7, 19, 0, 5, 18, 3 },
        { 12, 8, 20, 7, 18, -1, 17 },
        { 8, 11, 18, 20, 20, 9, 14 },
        { 18, 20, -1, 18, 0, -1, -1 },
        { 0, 8, 6, 3, 0, 7, 17 },
        { 4, 11, 5, 0, 5, -1, 1 },
        { 5, 20, 17, 7, 17, 5, 8 },
        { 2, 18, 5, 6, 16, 11, 1 },
        { 4, 11, 6, 19, 7, 6, 11 },
        { 20, 20, 7, 20, 18, 19, 7 },
        { 11, 0, 1, 14, 18, 18, 10 },
        { 6, 7, 15, 3, 0, 3, 19 },
        { 10, 7, -1, 17, 10, -1, -1 },
        { 16, 5, 5, 19, 4, 20, 17 },
        { 17, 19, 16, 19, 5, 3, 2 },
        { 6, 3, 9, 3, 15, 17, 2 },
        { 20, 12, 18, 17, 7, -1, 4 },
        { 16, 20, 11, 19, 12, 0, 4 },
        { 5, 18, 1, 19, 3, 18, 20 },
        { 18, 20, 5, 18, 16, 8, -1 },
        { 11, 0, 8, 18, 18, 17, 17 },
        { 17, 11, 18, 18, 5, 20, 19 },
        { 12, 6, 7, 5, 20, -1, 8 },
        { 17, 20, 16, -1, 10, 20, 3 },
        { 19, 19, 17, 9, 18, 12, 5 },
        { 10, 16, 4, 19, 11, 18, 8 },
        { 18, 1, 16, 2, 20, 17, 7 },
        { 18, 1, 11, 18, 20, 1, 20 },
        { 20, 2, 19, 9, 2, 0, 17 },
        { 15, 18, 0, 20, 10, 3, 18 },
        { 14, 3, -1, 17, 1, -1, -1 },
        { 2, 20, 5, 19, 18, 11, 5 },
        { 17, 9, 16, 18, -1, 20, 7 },
        { 8, 3, 20, 16, 2, -1, -1 },
        { 18, 11, 3, 18, 18, 4, 11 },
        { 20, 7, 19, 17, 19, 15, 18 },
        { 5, 6, 7, 18, 18, -1, -1 },
        { 15, 2, 9, 2, 12, 3, 16 },
        { 16, 20, 15, 11, 17, 12, 7 },
        { 18, 19, -1, 12, 20, -1, -1 },
        { 4, 5, 10, 20, 11, 10, 16 },
        { 16, 19, 5, 17, 20, 5, 12 },
        { 7, 11, 20, 18, 18, 4, 4 },
        { 17, 17, 20, 20, 20, -1, 5 },
        { 7, 20, 3, 15, 11, 20, 7 },
        { 6, 4, 11, 5, 9, 17, 3 },
        { 14, 19, -1, 5, 5, -1, -1 },
        { 12, 15, 18, 10, 1, 19, -1 },
        { 18, 17, 11, -1, -1, 18, 17 },
        { 12, 12, 18, 15, 7, 16, -1 },
        { 12, 5, 8, 11, 4, 18, -1 },
        { 7, 12, 20, 20, 7, 15, 16 },
        { 7, 20, 3, 17, 20, 20, 7 },
        { 18, 20, 3, 18, 9, 14, -1 },
        { 17, 11, 15, 18, 18, 7, 10 },
        { 17, 20, 1, 20, 19, 4, 10 },
        { 5, 8, 18, 12, 5, 11, 16 },
        { 3, 7, 9, 4, 6, 8, 2 },
        { 14, 3, -1, 18, 1, -1, -1 },
        { 18, 19, 5, 17, 11, 8, -1 },
        { 7, 10, 17, 11, 19, -1, -1 },
        { 9, 0, 10, 4, 3, 1, 20 },
        { 19, 19, 5, 0, 4, 7, 12 },
        { 6, 17, 8, 16, 20, -1, 19 },
        { 12, 19, 2, 17, -1, -1, 11 },
        { 16, 19, 18, 4, 18, 11, 19 },
        { 14, 5, -1, 1, 1, -1, -1 },
        { 16, 20, 19, -1, 10, 18, 16 },
        { 3, 6, 7, 16, 6, 18, 0 },
        { 11, 16, 4, 1, 15, 20, 18 },
        { 11, 18, 18, 19, 3, -1, 0 },
        { 20, 18, 11, 18, 20, 16, 15 },
        { 4, 20, 8, 9, 18, 5, 16 },
        { 11, 20, 5, 18, 16, 20, 18 },
        { 8, 1, 11, 2, 4, 7, 5 },
        { 20, 15, 0, 20, 12, 18, 19 },
        { 4, 4, 6, 2, 18, 18, 11 },
        { 0, 8, 8, 11, 20, 16, 2 },
        { 17, 19, 18, 0, 5, 3, 0 },
        { 15, 3, 1, 5, 9, 16, 17 },
        { 0, 11, 9, 8, 0, 17, -1 },
        { 16, 19, 19, 19, 17, 2, 18 },
        { 14, 2, -1, 5, 9, -1, -1 },
        { 18, 9, 15, 1, 7, 18, 7 },
        { 19, 19, 18, 18, 18, 15, 16 },
        { 11, 6, 5, 2, 3, 0, 6 },
        { 17, 5, 10, 8, 1, -1, -1 },
        { 1, 4, 8, 15, 20, 7, 19 },
        { 0, 18, 10, 18, 3, 18, 16 },
        { 18, 12, 16, 2, 17, 17, 19 },
        { 14, 16, -1, 3, 19, -1, -1 },
        { 6, 1, 7, 11, 19, 12, 1 },
        { 2, 3, 18, 16, 2, 17, 19 },
        { 1, 4, 19, 15, 10, 12, 17 },
        { 3, 8, 11, 20, 20, 19, 1 },
        { 1, 4, 17, 15, 16, 10, 19 },
        { 0, 12, 6, 18, 19, 19, 17 },
        { 17, 6, 19, 7, 20, 16, 16 },
        { 20, 12, 17, 16, -1, 2, 20 },
        { 19, 19, 17, 8, 5, 18, 20 },
        { 2, 11, 11, 2, 19, 20, 17 },
        { 19, 16, 6, 2, 19, 12, 7 },
        { 5, 12, 4, 0, 4, 8, 5 },
        { 4, 20, 7, 16, 5, 10, 10 },
        { 20, 4, 18, 17, 3, 12, 19 },
        { 8, 17, 6, 19, 19, 1, 15 },
        { 6, 7, 18, 15, 20, 5, 9 },
        { 2, 18, 12, 18, 16, 16, 6 },
        { 4, 20, 19, 0, 4, -1, 17 },
        { 20, 1, 19, 7, 16, -1, 19 },
        { 14, 2, -1, 18, 18, -1, -1 },
        { 2, 18, 3, 6, 16, 20, 20 },
        { 19, 18, 19, 17, 7, 18, 7 },
        { 15, 0, 17, 2, 20, 18, 2 },
        { 6, 7, 20, 5, 17, -1, 17 },
        { 17, 20, 5, -1, 7, 15, 19 },
        { 18, 19, 19, 7, 7, 12, 7 },
        { 9, 3, 1, 7, 11, 10, 7 },
        { 0, 4, 1, 5, 17, 16, 11 },
        { 6, 6, 8, 18, 3, 1, 18 },
        { 3, 6, 17, 18, 9, 8, 19 },
        { 8, 18, 10, 5, 5, 20, 12 },
        { 9, 20, 20, 6, 1, 20, 10 },
        { 20, -1, 17, -1, -1, -1, 7 },
        { 16, 17, 5, 7, 5, 3, 1 },
        { 18, 11, 19, 5, 1, 18, 17 },
        { 19, 17, 12, 17, 6, 16, 1 },
        { 7, 20, 18, 20, 1, 17, 18 },
        { 15, 19, 17, 19, 20, 5, 7 },
        { 11, 20, 15, 18, -1, 18, 17 },
        { 5, 7, 4, 0, 18, 8, 5 },
        { 19, 7, 20, 20, 17, 8, 16 },
        { 17, 17, 17, 12, 3, 5, 20 },
        { 11, 19, 7, 16, 18, 8, 16 },
        { 4, 6, 6, 17, 2, 20, 16 },
        { 15, 11, 1, 20, 5, 16, 16 },
        { 20, 20, -1, 19, 17, -1, -1 },
        { 16, 20, 19, 20, 18, 17, 2 },
        { 19, 20, 5, 7, 7, 8, 16 },
        { 18, 20, 5, 20, 17, 11, -1 },
        { 15, 15, 16, 11, 5, 8, -1 },
        { 5, 15, 16, 3, 17, 4, 1 },
        { 9, 9, 5, 3, 6, 4, 19 },
        { 20, 7, 17, 7, 1, 11, 19 },
        { 4, 19, 11, 19, 16, -1, 9 },
        { 8, 8, 18, 12, 17, -1, 17 },
        { 14, 2, -1, 18, 9, -1, -1 },
        { 19, 18, 11, 19, 19, 16, 15 },
        { 1, 0, 16, 18, 16, 18, 18 },
        { 18, 9, 16, 11, 17, 18, 9 },
        { 12, 19, 1, 16, 5, 9, 10 },
        { 2, 4, 6, 4, 0, 20, 12 },
        { 20, 12, 17, 18, 5, 20, 17 },
        { 15, 0, 4, 4, 7, 16, 8 },
        { 4, 19, 6, 3, 16, 19, 8 },
        { 20, 18, 3, 18, 19, 20, 18 },
        { 15, 0, 17, 18, 8, 8, 5 },
        { 20, 19, 17, 12, 18, 16, 17 },
        { 8, 12, 17, 7, 3, 5, 12 },
        { 17, 20, 16, 4, 15, 19, -1 },
        { 16, 10, 1, 17, 12, 18, 8 },
        { 17, 12, 20, 16, -1, 17, 8 },
        { 1, 3, 7, 18, 20, 18, 8 },
        { 18, 19, 16, 0, 19, 7, 19 },
        { 16, 9, 19, 15, 20, 19, 16 },
        { 16, 2, 0, 19, 1, 11, 19 },
        { 1, 18, 20, 5, 6, 16, 12 },
        { 18, 5, 18, 18, 17, 3, 5 },
        { 8, 8, 5, 5, 3, 15, 11 },
        { 16, 15, 19, 6, 19, 16, 11 },
        { 8, 9, 18, 7, 20, 17, 17 },
        { 9, 18, 18, 1, 8, 12, 15 },
        { 17, 9, 11, 18, -1, 20, 20 },
        { 20, 18, 16, 19, 20, 20, 19 },
        { 14, 11, -1, 18, 18, -1, -1 },
        { 2, 7, 8, 18, 12, 5, 3 },
        { 18, 19, 19, 18, 7, 2, 7 },
        { 19, 16, 16, 8, 18, 20, 19 },
        { 14, 11, -1, 19, 3, -1, -1 },
        { 17, 20, 16, 2, 20, 1, 3 },
        { 16, 10, 7, 1, 12, 6, 19 },
        { 11, 0, 17, 15, 8, 4, 10 },
        { 17, 0, 15, 18, 1, 1, 3 },
        { 20, -1, 17, -1, -1, -1, 7 },
        { 2, 20, 17, 9, 18, 7, 16 },
        { 20, 8, 8, 18, 16, 2, 17 },
        { 0, 5, 7, 5, 17, -1, 17 },
        { 5, 4, 5, 19, 20, 15, 18 },
        { 20, 18, 18, 9, 2, 20, 18 },
        { 5, 6, 9, 2, 3, 15, -1 },
        { 6, 2, 8, 20, 17, 18, 17 },
        { 20, 8, 6, 3, 18, 4, 18 },
        { 17, 5, 19, 18, 16, 16, 9 },
        { 17, 20, 19, 17, 5, 10, 16 },
        { 14, 18, -1, 16, 20, -1, -1 },
        { 3, 18, 15, 9, 15, 2, 6 },
        { 20, 20, 18, 18, 8, -1, 11 },
        { 16, 5, 7, 19, 7, 4, 17 },
        { 7, 3, 18, 4, 11, -1, 19 },
        { 3, 4, 15, 5, 10, 2, 10 },
        { 17, 19, 12, 19, 12, 16, 18 },
        { 4, 5, 9, 16, 10, 3, 1 },
        { 20, 4, 7, 8, 6, 17, 17 },
        { 16, 19, 18, 7, 16, 8, 19 },
        { 3, 20, 20, 18, 7, 16, 7 },
        { 7, 9, 5, 4, 0, 11, 8 },
        { 7, 11, 19, 18, 5, 0, 19 },
        { 19, 16, 3, 10, 20, 17, 12 },
        { 7, 18, 11, 12, 19, 15, 19 },
        { 19, 16, 16, 18, 11, 8, 18 },
        { 20, 0, 16, 16, 19, 16, 3 },
        { 2, 3, 6, 15, 11, 11, 8 },
        { 4, 7, 3, 19, 2, -1, 17 },
        { 18, 11, 0, 11, 17, 3, 7 },
        { 14, 11, -1, 5, 15, -1, -1 },
        { 9, 15, 16, 15, 11, 17, 9 },
        { 20, 20, 18, 19, 8, -1, 11 },
        { 11, 16, 15, -1, 19, 19, 5 },
        { 5, 10, 1, 19, 18, 18, 20 },
        { 15, 0, 16, 15, 17, 8, 8 },
        { 7, 11, 8, 2, 16, -1, 17 },
        { 15, 0, 18, 18, 19, 20, 18 },
        { 8, 6, 6, 16, 10, 12, 15 },
        { 7, 11, 8, 20, 9, 1, 16 },
        { 5, 11, 17, 18, 19, 8, 17 },
        { 11, 6, 4, 12, 3, 9, 18 },
        { 7, 11, 18, 18, 5, 4, 1 },
        { 20, 18, 16, 18, 16, 4, 11 },
        { 18, 19, 19, 18, 6, 2, 19 },
        { 20, 15, 18, 1, 16, 19, 18 },
        { 19, 3, 12, 20, 5, 5, 3 },
        { 18, 20, 5, 17, 8, 17, -1 },
        { 4, 18, 18, 20, 17, 1, 17 },
        { 6, 7, 7, 20, 0, 8, 8 },
        { 5, 18, 4, 18, 8, 1, 12 },
        { 15, 1, 19, 18, 19, 2, 20 },
        { 3, 1, 5, 9, 4, 9, 20 },
        { 19, 18, 16, 16, 17, 19, 18 },
        { 0, 0, 11, 20, 11, -1, 17 },
        { 20, 11, 18, 4, 0, 18, 15 },
        { 0, 19, 7, 17, 19, -1, 19 },
        { 8, 18, 18, 20, 3, -1, 17 },
        { 0, 3, 4, 17, 1, -1, 7 },
        { 19, 0, 8, 11, 6, 17, 5 },
        { 5, 7, 18, 5, 3, 16, 16 },
        { 0, 11, 8, 5, 8, 7, 19 },
        { 7, 18, 20, 19, 4, 8, 2 },
        { 15, 0, 16, 15, 7, 19, 1 },
        { 17, 5, 19, 18, 19, 5, 19 },
        { 18, 20, 19, 19, 17, 17, 18 },
        { 7, 15, 19, 18, 5, 9, 17 },
        { 0, 19, 2, 10, 20, 17, 5 },
        { 17, 4, 16, 18, 5, 20, 5 },
        { 4, 6, 0, 9, 17, 8, 18 },
        { 4, 11, 11, 15, 11, 18, 18 },
        { 20, 20, 7, 6, 2, 18, 8 },
        { 10, 6, 5, 18, 8, 19, 8 },
        { 16, 19, 5, 20, 2, 20, 16 },
        { 5, 16, 19, 7, 0, 6, 18 },
        { 18, 19, 19, 19, -1, 4, 18 },
        { 3, 18, 9, 2, 16, -1, 17 },
        { 17, 5, 19, 20, 20, 10, 18 },
        { 10, 19, 18, 6, 5, 7, 18 },
        { 20, 17, 17, 12, 4, 11, 19 },
        { 7, 18, 10, -1, 16, 2, 17 },
        { 15, 8, 2, 18, 12, 6, 17 },
        { 14, 18, -1, 18, 20, -1, -1 },
        { 16, 20, 5, -1, 20, 7, 19 },
        { 17, 17, 20, 7, 5, 6, 4 },
        { 16, 20, 19, 14, 20, 16, 5 },
        { 14, 2, -1, 1, 11, -1, -1 },
        { 19, 20, 5, 19, 17, 20, 5 },
        { 11, 20, 16, 9, 16, 1, 1 },
        { 15, 0, 20, 15, 2, 17, 18 },
        { 3, 8, 11, 17, 19, 16, 19 },
        { 2, 7, 10, 3, 17, 10, 16 },
        { 5, 3, 17, 19, 20, 20, 17 },
        { 11, 4, 5, 0, 16, 19, 11 },
        { 2, 2, 9, 0, 7, 18, 18 },
    };
    static constexpr float kModelThr[kModelTrees][7] = {
        { 2.34328370e-01f, 1.00000000e+00f, 1.45752591e-01f, 7.36404336e-01f, 3.28171435e-01f, 9.81136834e-03f, 3.33857518e-01f },
        { 6.53392342e-01f, 5.23122382e-01f, 6.08251296e-01f, 6.48081408e-02f, 3.83843003e-02f, 9.29238171e-02f, 0.00000000e+00f },
        { 5.97072357e-01f, 1.00000000e+00f, 4.02951018e-01f, 1.56133437e-01f, 0.00000000e+00f, 7.82608696e-01f, 2.41180314e-02f },
        { 1.00000000e+00f, 8.27482123e-01f, 0.00000000e+00f, 4.69687371e-02f, 2.19089038e-01f, 0.00000000e+00f, 0.00000000e+00f },
        { 7.02757147e-01f, 1.00000000e+00f, 1.86618674e-02f, 3.19868219e-01f, 6.22014731e-02f, 4.25531915e-02f, 7.48102816e-01f },
        { 5.23122382e-01f, 5.99895907e-02f, 3.83843003e-02f, 3.49344565e+00f, 2.44499236e-01f, 2.50000000e-01f, 1.87918216e-01f },
        { 6.29749985e-01f, 1.00000000e+00f, 4.51681154e-01f, 1.07031098e-01f, 0.00000000e+00f, 5.62788040e-02f, 2.51835615e-01f },
        { 1.00000000e+00f, 7.48102816e-01f, 0.00000000e+00f, 5.07989849e-02f, 5.64705252e-01f, 0.00000000e+00f, 0.00000000e+00f },
        { 2.34328370e-01f, 4.71570748e+00f, 1.45752591e-01f, 1.00000000e+00f, 3.80472285e-01f, 9.81136834e-03f, 1.47679136e-01f },
        { 6.53392342e-01f, 5.23122382e-01f, 1.79432010e-01f, 6.48081408e-02f, 6.21624525e-01f, 5.60698501e-01f, 9.93747138e-02f },
        { 1.56133437e-01f, 1.00000000e+00f, 6.34728506e-01f, 2.40191026e-01f, 0.00000000e+00f, 1.40408140e-01f, 4.51681154e-01f },
        { 1.00000000e+00f, 7.48102816e-01f, 0.00000000e+00f, 5.07989849e-02f, 8.51063830e-02f, 0.00000000e+00f, 0.00000000e+00f },
        { 1.79519595e-01f, 4.71570748e+00f, 1.45752591e-01f, 1.00000000e+00f, 1.91088325e-01f, 2.91013718e-02f, 4.42364978e-01f },
        { 6.53392342e-01f, 5.70264661e-01f, 7.39227084e-02f, 1.87918216e-01f, 6.21624525e-01f, 2.03509659e-01f, 1.39736263e-01f },
        { 6.29749985e-01f, 1.00000000e+00f, 4.65510775e-01f, 5.15108051e-01f, 0.00000000e+00f, 3.84389091e+00f, 6.97127439e-02f },
        { 1.00000000e+00f, 7.48102816e-01f, 0.00000000e+00f, 9.81136834e-03f, 5.46841557e-01f, 0.00000000e+00f, 0.00000000e+00f },
        { 9.54427083e-02f, 1.97790101e-01f, 2.30979263e+00f, 3.33857518e-01f, 4.93849709e+00f, 8.06933343e-02f, 1.00000000e+00f },
        { 6.53392342e-01f, 5.23122382e-01f, 1.16042556e-01f, 3.33857518e-01f, 7.67568454e-02f, 1.56439067e-01f, 9.93747138e-02f },
        { 2.41180314e-02f, 1.22617022e-01f, 2.30979263e+00f, 5.15108051e-01f, 2.19089038e-01f, 4.51681154e-01f, 2.74225040e-01f },
        { 1.00000000e+00f, 8.27482123e-01f, 0.00000000e+00f, 5.07989849e-02f, 8.51063830e-02f, 0.00000000e+00f, 0.00000000e+00f },
        { 2.34328370e-01f, 1.91088325e-01f, 4.38044238e-01f, 4.71570748e+00f, 3.20727405e-01f, 9.16707678e-02f, 4.42364978e-01f },
        { 6.53392342e-01f, 5.88125896e-01f, 7.92058433e-02f, 2.15363503e-01f, 6.21624525e-01f, 7.02757147e-01f, 1.39736263e-01f },
        { 1.22617022e-01f, 1.00000000e+00f, 5.97072357e-01f, 1.31085329e-01f, 0.00000000e+00f, 3.09487315e-01f, 4.65510775e-01f },
        { 1.00000000e+00f, 5.62788040e-02f, 0.00000000e+00f, 7.98480479e-01f, 4.42364978e-01f, 0.00000000e+00f, 0.00000000e+00f },
        { 9.54427083e-02f, 1.00000000e+00f, 2.30979263e+00f, 1.19561110e-01f, 4.29343799e-01f, 8.06933343e-02f, 1.00000000e+00f },
        { 5.23122382e-01f, 5.08736477e+00f, 3.83843003e-02f, 3.49344565e+00f, 1.19561110e-01f, 4.93849709e+00f, 1.87918216e-01f },
        { 2.41180314e-02f, 7.92058433e-02f, 3.91646858e+00f, 3.05376142e-01f, 1.19561110e-01f, 4.95637591e-01f, 7.37999948e-02f },
        { 1.00000000e+00f, 5.62788040e-02f, 0.00000000e+00f, 7.98480479e-01f, 8.27482123e-01f, 0.00000000e+00f, 0.00000000e+00f },
        { 2.30819441e-01f, 3.79015782e-02f, 1.79519595e-01f, 1.00000000e+00f, 9.16707678e-02f, 4.83427760e+00f, 3.07348216e-01f },
        { 3.33857518e-01f, 5.60698501e-01f, 3.76785400e+00f, 7.22001320e-01f, 5.60648320e+00f, 5.54814041e-02f, 5.23122382e-01f },
        { 1.79519595e-01f, 2.28279175e-01f, 6.34728506e-01f, 4.83427760e+00f, 1.00000000e+00f, 3.40425532e-01f, 2.91013718e-02f },
        { 1.00000000e+00f, 5.62788040e-02f, 0.00000000e+00f, 9.08112869e-01f, 3.40425532e-01f, 0.00000000e+00f, 0.00000000e+00f },
        { 5.71810474e-02f, 1.97790101e-01f, 2.30979263e+00f, 3.33857518e-01f, 2.27907798e-01f, 7.54290178e-02f, 2.37920766e-01f },
        { 3.33857518e-01f, 5.60698501e-01f, 3.64115986e+00f, 2.91503762e-01f, 1.31569103e-01f, 7.17612677e-01f, 3.55672926e-01f },
        { 2.19089038e-01f, 1.17116054e-01f, 3.74271559e-01f, 3.58458223e-01f, 6.31495147e-01f, 4.65182653e-01f, 1.50196954e-01f },
        { 1.00000000e+00f, 4.98356736e-02f, 0.00000000e+00f, 7.36404336e-01f, 5.95744681e-01f, 0.00000000e+00f, 0.00000000e+00f },
        { 2.28279175e-01f, 3.79015782e-02f, 5.15700453e-01f, 8.27482123e-01f, 3.38872637e-01f, 4.62697480e-02f, 0.00000000e+00f },
        { 4.59422402e-01f, 5.08736477e+00f, 7.67568454e-02f, 1.43869671e-01f, 2.08109314e-01f, 4.93849709e+00f, 1.41472816e-01f },
        { 7.77161370e-01f, 3.23789880e-01f, 4.22445305e-02f, 6.34728506e-01f, 3.65604412e-01f, 2.82459586e-01f, 7.37999948e-02f },
        { 1.00000000e+00f, 9.81136834e-03f, 0.00000000e+00f, 3.79015782e-02f, 5.62788040e-02f, 0.00000000e+00f, 0.00000000e+00f },
        { 2.30819441e-01f, 3.40840199e-02f, 4.83427760e+00f, 2.52834264e-01f, 9.16707678e-02f, 2.16184779e-01f, 1.97657384e-01f },
        { 5.91395256e-01f, 6.04630904e-01f, 9.29238171e-02f, 1.87918216e-01f, 6.21624525e-01f, 2.58932688e-01f, 3.94425820e-01f },
        { 7.77161370e-01f, 3.36864387e-01f, 1.84604774e-01f, 6.34728506e-01f, 4.59422402e-01f, 5.56667197e-01f, 3.40840199e-02f },
        { 1.00000000e+00f, 9.81136834e-03f, 0.00000000e+00f, 2.37920766e-01f, 7.92058433e-02f, 0.00000000e+00f, 0.00000000e+00f },
        { 2.27907798e-01f, 1.54447619e+00f, 9.67341043e-03f, 0.00000000e+00f, 1.64833143e-01f, 4.89172847e-01f, 6.47744974e-01f },
        { 3.33857518e-01f, 5.60698501e-01f, 3.64115986e+00f, 0.00000000e+00f, 1.31569103e-01f, 6.77981898e-01f, 3.38872637e-01f },
        { 2.28279175e-01f, 4.44974194e+00f, 6.29749985e-01f, 2.37518650e-01f, 7.05644037e-01f, 1.00000000e+00f, 0.00000000e+00f },
        { 1.00000000e+00f, 1.51450602e-02f, 0.00000000e+00f, 2.91013718e-02f, 5.95744681e-01f, 0.00000000e+00f, 0.00000000e+00f },
        { 1.00000000e+00f, 3.63273695e-01f, 1.23732063e-01f, 1.91088325e-01f, 2.40191026e-01f, 3.37633832e-01f, 0.00000000e+00f },
        { 3.33857518e-01f, 5.60698501e-01f, 3.55672926e-01f, 1.51140431e-01f, 1.40408140e-01f, 1.87918216e-01f, 8.48016517e-02f },
        { 2.19089038e-01f, 1.97657384e-01f, 6.47770921e-01f, 4.05461890e-01f, 1.56439067e-01f, 6.23041595e-02f, 3.40840199e-02f },
        { 1.00000000e+00f, 5.95744681e-01f, 0.00000000e+00f, 7.89725885e-02f, 1.25656780e-01f, 0.00000000e+00f, 0.00000000e+00f },
        { 2.28279175e-01f, 1.75823274e-01f, 5.15700453e-01f, 1.00000000e+00f, 7.07567859e-01f, 4.44482741e-02f, 0.00000000e+00f },
        { 3.13102986e+00f, 5.54814041e-02f, 4.69687371e-02f, 4.26960979e-01f, 5.88125896e-01f, 1.87918216e-01f, 3.83843003e-02f },
        { 6.86242327e-02f, 3.91646858e+00f, 2.19089038e-01f, 3.49344565e+00f, 5.15108051e-01f, 3.20727405e-01f, 2.61360589e-02f },
        { 1.00000000e+00f, 1.51450602e-02f, 0.00000000e+00f, 8.06933343e-02f, 3.79015782e-02f, 0.00000000e+00f, 0.00000000e+00f },
        { 9.67341043e-03f, 4.00425255e-02f, 3.73967795e-01f, 3.37223068e+00f, 0.00000000e+00f, 3.79015782e-02f, 9.93747138e-02f },
        { 1.02599126e-02f, 5.75556420e-01f, 2.40191026e-01f, 0.00000000e+00f, 5.27715860e-01f, 1.75795077e-01f, 3.02967922e-02f },
        { 2.28279175e-01f, 4.83427760e+00f, 6.29749985e-01f, 3.38872637e-01f, 3.55578004e-02f, 1.00000000e+00f, 0.00000000e+00f },
        { 1.00000000e+00f, 5.62788040e-02f, 0.00000000e+00f, 7.98480479e-01f, 2.39166667e-01f, 0.00000000e+00f, 0.00000000e+00f },
        { 1.54447619e+00f, 1.54603951e-01f, 1.97790101e-01f, 0.00000000e+00f, 0.00000000e+00f, 1.02599126e-02f, 1.41472816e-01f },
        { 3.13102986e+00f, 3.14359568e-01f, 3.33857518e-01f, 4.69687371e-02f, 5.88125896e-01f, 5.60698501e-01f, 3.55672926e-01f },
        { 9.43976433e-02f, 5.15108051e-01f, 3.17844391e-01f, 3.55672926e-01f, 3.25238639e+00f, 6.58364357e-01f, 2.79770276e-01f },
        { 1.00000000e+00f, 5.95744681e-01f, 0.00000000e+00f, 7.89725885e-02f, 5.35239225e-01f, 0.00000000e+00f, 0.00000000e+00f },
        { 3.79015782e-02f, 2.50000000e-01f, 6.31495147e-01f, 1.64833143e-01f, 6.47770921e-01f, 9.54427083e-02f, 1.41472816e-01f },
        { 6.27204726e+00f, 3.49344565e+00f, 2.91503762e-01f, 5.54814041e-02f, 4.69687371e-02f, 0.00000000e+00f, 2.84861373e-01f },
        { 2.19089038e-01f, 1.17116054e-01f, 3.37633832e-01f, 4.51107311e-03f, 6.31495147e-01f, 4.79019129e-01f, 4.98356736e-02f },
        { 5.95744681e-01f, 1.54447619e+00f, 1.25656780e-01f, 5.56854111e-01f, 2.60697097e-01f, 7.82608696e-01f, 0.00000000e+00f },
        { 2.27907798e-01f, 1.54447619e+00f, 9.67341043e-03f, 0.00000000e+00f, 3.00722655e+00f, 6.34728506e-01f, 4.25818187e-01f },
        { 2.78821044e-01f, 1.02599126e-02f, 6.94789568e-01f, 4.43587164e-02f, 3.38872637e-01f, 1.54603951e-01f, 0.00000000e+00f },
        { 1.31085329e-01f, 8.26976774e-02f, 4.19982322e-01f, 2.17813393e-02f, 4.52567064e-01f, 3.94425820e-01f, 7.63619423e-01f },
        { 4.42364978e-01f, 9.81136834e-03f, 2.56907719e-03f, 1.86553819e-01f, 1.50196954e-01f, 3.59220336e-01f, 2.28279175e-01f },
        { 1.00000000e+00f, 3.73967795e-01f, 6.47770921e-01f, 9.67341043e-03f, 9.93747138e-02f, 0.00000000e+00f, 5.14031621e-01f },
        { 1.54603951e-01f, 6.27204726e+00f, 1.73078306e-01f, 5.94841641e+00f, 2.91503762e-01f, 2.37518650e-01f, 0.00000000e+00f },
        { 2.27907798e-01f, 2.58110080e-02f, 2.19089038e-01f, 3.91646858e+00f, 6.21624525e-01f, 6.31495147e-01f, 3.79015782e-02f },
        { 1.00000000e+00f, 9.81136834e-03f, 0.00000000e+00f, 6.97127439e-02f, 7.92058433e-02f, 0.00000000e+00f, 0.00000000e+00f },
        { 2.28279175e-01f, 5.42220845e+00f, 5.15700453e-01f, 3.43513652e-01f, 1.91088325e-01f, 3.19369975e-01f, 0.00000000e+00f },
        { 8.27482123e-01f, 3.49344565e+00f, 7.67568454e-02f, 5.54814041e-02f, 4.69687371e-02f, 0.00000000e+00f, 1.87918216e-01f },
        { 2.28279175e-01f, 2.40191026e-01f, 6.29749985e-01f, 1.24332202e-01f, 1.52606456e-02f, 1.00000000e+00f, 0.00000000e+00f },
        { 1.00000000e+00f, 8.30780596e-03f, 0.00000000e+00f, 2.91013718e-02f, 4.69687371e-02f, 0.00000000e+00f, 0.00000000e+00f },
        { 1.54447619e+00f, 0.00000000e+00f, 9.36280610e-02f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f, 3.91646858e+00f },
        { 3.83843003e-02f, 4.93849709e+00f, 7.05644037e-01f, 2.41180314e-02f, 5.42220845e+00f, 5.99895907e-02f, 6.08251296e-01f },
        { 2.17813393e-02f, 1.41472816e-01f, 6.31495147e-01f, 1.02599126e-02f, 2.50000000e-01f, 3.17844391e-01f, 1.40600887e-03f },
        { 1.00000000e+00f, 4.53521502e-02f, 0.00000000e+00f, 1.56439067e-01f, 3.47862269e-01f, 0.00000000e+00f, 0.00000000e+00f },
        { 4.25818187e-01f, 3.00722655e+00f, 9.81136834e-03f, 3.17844391e-01f, 1.64833143e-01f, 2.12765957e-02f, 6.77981898e-01f },
        { 3.91646858e+00f, 5.88125896e-01f, 4.65182653e-01f, 4.40243553e-01f, 6.08251296e-01f, 3.80353325e-01f, 2.41180314e-02f },
        { 3.65604412e-01f, 3.96543071e-01f, 3.05376142e-01f, 5.78841603e-01f, 2.58932688e-01f, 6.32218364e-01f, 2.17813393e-02f },
        { 4.42364978e-01f, 1.54603951e-01f, 2.56907719e-03f, 7.39227084e-02f, 1.86553819e-01f, 6.27204726e+00f, 7.69197598e-01f },
        { 1.54447619e+00f, 1.54603951e-01f, 1.97790101e-01f, 0.00000000e+00f, 0.00000000e+00f, 2.61713124e+00f, 1.41472816e-01f },
        { 9.29238171e-02f, 6.48081408e-02f, 3.94425820e-01f, 1.56439067e-01f, 3.05376142e-01f, 3.33857518e-01f, 3.40425532e-01f },
        { 2.28279175e-01f, 7.89725885e-02f, 6.29749985e-01f, 5.51747691e-01f, 4.07269911e-01f, 1.00000000e+00f, 0.00000000e+00f },
        { 5.95744681e-01f, 5.07989849e-02f, 5.35239225e-01f, 4.46373731e-01f, 5.75556420e-01f, 0.00000000e+00f, 6.23041595e-02f },
        { 8.19340639e-02f, 3.73967795e-01f, 1.73064291e-01f, 6.30402647e-01f, 6.97127439e-02f, 3.09487315e-01f, 2.75178434e-02f },
        { 3.91646858e+00f, 4.26960979e-01f, 5.62261641e-01f, 3.28171435e-01f, 4.01676029e-02f, 2.10454796e-01f, 6.48081408e-02f },
        { 7.67568454e-02f, 2.28263583e-01f, 2.17813393e-02f, 6.30402647e-01f, 0.00000000e+00f, 3.74271559e-01f, 6.31495147e-01f },
        { 5.95744681e-01f, 6.08251296e-01f, 2.27907798e-01f, 4.54152471e-01f, 2.41180314e-02f, 6.23041595e-02f, 1.49778499e-01f },
        { 1.54447619e+00f, 0.00000000e+00f, 9.36280610e-02f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f, 1.00000000e+00f },
        { 1.02599126e-02f, 0.00000000e+00f, 7.67568454e-02f, 0.00000000e+00f, 0.00000000e+00f, 1.54603951e-01f, 7.17612677e-01f },
        { 1.00000000e+00f, 3.91646858e+00f, 1.23732063e-01f, 8.27482123e-01f, 8.51063830e-02f, 6.77981898e-01f, 0.00000000e+00f },
        { 1.00000000e+00f, 8.30780596e-03f, 0.00000000e+00f, 1.02599126e-02f, 7.39227084e-02f, 0.00000000e+00f, 0.00000000e+00f },
        { 9.67341043e-03f, 2.10182774e-01f, 1.79519595e-01f, 4.23426081e-01f, 2.58816057e-01f, 5.78841603e-01f, 2.76418549e-04f },
        { 6.27204726e+00f, 9.93747138e-02f, 2.91503762e-01f, 1.41472816e-01f, 6.86242327e-02f, 0.00000000e+00f, 1.51643907e-01f },
        { 5.75556420e-01f, 5.78841603e-01f, 2.22238179e-01f, 5.15108051e-01f, 3.79684968e-01f, 4.04397337e+00f, 3.20727405e-01f },
        { 1.00000000e+00f, 1.16250271e-01f, 0.00000000e+00f, 2.12765957e-02f, 2.07355783e-01f, 0.00000000e+00f, 0.00000000e+00f },
        { 2.91013718e-02f, 4.23469599e-01f, 1.97790101e-01f, 1.54447619e+00f, 8.81523697e-01f, 2.17813393e-02f, 1.41472816e-01f },
        { 5.88125896e-01f, 5.08736477e+00f, 4.93849709e+00f, 5.97072357e-01f, 3.40425532e-01f, 4.25531915e-02f, 7.67568454e-02f },
        { 4.51107311e-03f, 2.61713124e+00f, 7.39117303e-01f, 5.54267036e-04f, 0.00000000e+00f, 8.06933343e-02f, 4.79267501e-01f },
        { 6.08251296e-01f, 4.37500000e-01f, 2.51835615e-01f, 3.79015782e-02f, 9.43976433e-02f, 0.00000000e+00f, 0.00000000e+00f },
        { 9.16666667e-01f, 3.80472285e-01f, 3.80353325e-01f, 2.61713124e+00f, 3.91646858e+00f, 2.87980437e-01f, 0.00000000e+00f },
        { 3.05376142e-01f, 2.15363503e-01f, 3.65604412e-01f, 1.59288710e-01f, 1.07494800e-01f, 1.44550810e-01f, 9.43976433e-02f },
        { 5.91395256e-01f, 7.17612677e-01f, 5.98662936e-03f, 2.27827935e-01f, 1.76396574e-01f, 4.01319363e-01f, 3.76801078e-01f },
        { 8.30780596e-03f, 1.00000000e+00f, 5.95744681e-01f, 3.37223068e+00f, 0.00000000e+00f, 6.47770921e-01f, 6.08682874e-01f },
        { 2.91503762e-01f, 5.42220845e+00f, 0.00000000e+00f, 3.43513652e-01f, 1.99074074e-02f, 0.00000000e+00f, 0.00000000e+00f },
        { 1.75795077e-01f, 1.02599126e-02f, 0.00000000e+00f, 4.17933207e-01f, 6.27204726e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { 4.31115688e-01f, 3.80335569e-01f, 7.06257576e-01f, 4.44974194e+00f, 1.97657384e-01f, 7.89725885e-02f, 3.79684968e-01f },
        { 2.72866451e-01f, 4.53521502e-02f, 4.63412762e-01f, 2.41180314e-02f, 3.14705378e-01f, 2.56907719e-03f, 5.75556420e-01f },
        { 2.91503762e-01f, 5.42220845e+00f, 0.00000000e+00f, 1.75823274e-01f, 1.99074074e-02f, 0.00000000e+00f, 0.00000000e+00f },
        { 3.05376142e-01f, 2.15363503e-01f, 3.65604412e-01f, 6.91860026e-01f, 9.93747138e-02f, 1.25656780e-01f, 2.17813393e-02f },
        { 6.32218364e-01f, 5.91395256e-01f, 1.91088325e-01f, 2.58932688e-01f, 6.80928062e-02f, 7.37999948e-02f, 3.20727405e-01f },
        { 3.40425532e-01f, 5.60407022e-02f, 9.81136834e-03f, 2.31129766e-01f, 4.42376860e-01f, 0.00000000e+00f, 1.00000000e+00f },
        { 1.54447619e+00f, 0.00000000e+00f, 1.64833143e-01f, 0.00000000e+00f, 0.00000000e+00f, 2.10182774e-01f, 1.41472816e-01f },
        { 3.13102986e+00f, 3.14359568e-01f, 3.38872637e-01f, 1.05296097e-04f, 3.87248159e-01f, 8.51063830e-02f, 2.87980437e-01f },
        { 3.17844391e-01f, 4.52567064e-01f, 2.17813393e-02f, 3.07348216e-01f, 2.61713124e+00f, 3.76785400e+00f, 3.65604412e-01f },
        { 1.00000000e+00f, 1.16250271e-01f, 0.00000000e+00f, 3.35494995e-01f, 6.64276333e-02f, 0.00000000e+00f, 0.00000000e+00f },
        { 2.03509659e-01f, 3.00722655e+00f, 1.02599126e-02f, 5.89536436e-01f, 7.61382452e-02f, 2.69234183e-01f, 1.97790101e-01f },
        { 2.78821044e-01f, 2.87980437e-01f, 1.97790101e-01f, 3.30147052e-01f, 3.94425820e-01f, 0.00000000e+00f, 1.54603951e-01f },
        { 1.24332202e-01f, 9.06356450e-02f, 4.19982322e-01f, 3.17844391e-01f, 4.93386620e-01f, 5.14031621e-01f, 2.40191026e-01f },
        { 2.12765957e-02f, 6.91933593e-02f, 9.81136834e-03f, 5.07989849e-02f, 6.04630904e-01f, 1.86553819e-01f, 4.08324829e-01f },
        { 4.09792664e-01f, 1.15655035e-01f, 7.07567859e-01f, 3.80472285e-01f, 3.37633832e-01f, 3.43191552e-01f, 2.28279175e-01f },
        { 1.54603951e-01f, 2.78821044e-01f, 1.45752591e-01f, 8.27482123e-01f, 1.41472816e-01f, 5.15700453e-01f, 0.00000000e+00f },
        { 2.17813393e-02f, 1.41472816e-01f, 6.31495147e-01f, 1.02599126e-02f, 2.50000000e-01f, 1.41472816e-01f, 1.68466854e-01f },
        { 1.00000000e+00f, 4.53521502e-02f, 0.00000000e+00f, 4.52567064e-01f, 4.23469599e-01f, 0.00000000e+00f, 0.00000000e+00f },
        { 9.54427083e-02f, 1.97790101e-01f, 6.58364357e-01f, 2.30979263e+00f, 2.61713124e+00f, 4.99511253e-02f, 1.64833143e-01f },
        { 6.27204726e+00f, 9.93747138e-02f, 2.91503762e-01f, 6.48081408e-02f, 3.43191552e-01f, 0.00000000e+00f, 4.88016660e-01f },
        { 1.00000000e+00f, 6.64276333e-02f, 1.23732063e-01f, 2.34180133e-01f, 1.19561110e-01f, 6.31495147e-01f, 0.00000000e+00f },
        { 6.08251296e-01f, 4.93204981e-01f, 2.58932688e-01f, 3.64115986e+00f, 5.20720273e-01f, 0.00000000e+00f, 0.00000000e+00f },
        { 3.91646858e+00f, 8.27482123e-01f, 2.22238179e-01f, 3.13102986e+00f, 4.01676029e-02f, 6.32218364e-01f, 3.20727405e-01f },
        { 6.27204726e+00f, 5.94841641e+00f, 2.91503762e-01f, 7.07567859e-01f, 9.06356450e-02f, 0.00000000e+00f, 5.10352990e-01f },
        { 3.91646858e+00f, 4.23469599e-01f, 3.35494995e-01f, 6.23041595e-02f, 8.27482123e-01f, 4.93386620e-01f, 3.20727405e-01f },
        { 1.00000000e+00f, 2.07355783e-01f, 0.00000000e+00f, 1.16250271e-01f, 4.04397337e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { 5.42220845e+00f, 1.75823274e-01f, 1.97657384e-01f, 8.99784433e-01f, 4.56878281e+00f, 2.78821044e-01f, 4.42364978e-01f },
        { 1.02599126e-02f, 5.75556420e-01f, 3.30727086e-01f, 0.00000000e+00f, 0.00000000e+00f, 6.94789568e-01f, 3.40425532e-01f },
        { 2.28279175e-01f, 5.42220845e+00f, 1.56439067e-01f, 1.75823274e-01f, 1.97657384e-01f, 5.80549685e-01f, 0.00000000e+00f },
        { 4.42364978e-01f, 6.08251296e-01f, 7.01318023e+00f, 4.93204981e-01f, 1.56439067e-01f, 2.56907719e-03f, 0.00000000e+00f },
        { 1.54447619e+00f, 0.00000000e+00f, 1.64833143e-01f, 0.00000000e+00f, 0.00000000e+00f, 3.00722655e+00f, 3.00722655e+00f },
        { 3.30727086e-01f, 2.40191026e-01f, 2.96895494e-01f, 1.86372259e-01f, 3.76801078e-01f, 2.39166667e-01f, 0.00000000e+00f },
        { 1.97657384e-01f, 5.60648320e+00f, 3.09487315e-01f, 6.30402647e-01f, 2.56470472e-01f, 7.36404336e-01f, 5.60648320e+00f },
        { 5.60698501e-01f, 4.54152471e-01f, 8.40729270e-01f, 6.47770921e-01f, 4.25531915e-02f, 6.05195155e-02f, 0.00000000e+00f },
        { 1.13475952e+00f, 9.54427083e-02f, 2.35408122e-01f, 1.97790101e-01f, 6.70166395e-01f, 0.00000000e+00f, 6.34728506e-01f },
        { 5.60698501e-01f, 3.33857518e-01f, 2.41180314e-02f, 2.91503762e-01f, 9.93747138e-02f, 8.75792699e-01f, 0.00000000e+00f },
        { 6.32218364e-01f, 3.02220293e-01f, 2.22238179e-01f, 4.69687371e-02f, 4.01319363e-01f, 3.25238639e+00f, 3.20727405e-01f },
        { 4.69687371e-02f, 2.06511061e-01f, 7.89725885e-02f, 4.01676029e-02f, 0.00000000e+00f, 1.00000000e+00f, 3.43191552e-01f },
        { 6.77981898e-01f, 6.32218364e-01f, 3.79684968e-01f, 2.92726074e-01f, 4.54152471e-01f, 1.87918216e-01f, 3.35494995e-01f },
        { 8.27482123e-01f, 9.93747138e-02f, 7.67568454e-02f, 6.48081408e-02f, 1.50196954e-01f, 0.00000000e+00f, 5.35239225e-01f },
        { 1.00000000e+00f, 6.32218364e-01f, 1.04134195e-01f, 4.59422402e-01f, 8.19340639e-02f, 3.79684968e-01f, 4.30439277e-01f },
        { 1.00000000e+00f, 4.46373731e-01f, 0.00000000e+00f, 9.29238171e-02f, 7.89725885e-02f, 0.00000000e+00f, 0.00000000e+00f },
        { 1.54447619e+00f, 0.00000000e+00f, 1.97790101e-01f, 0.00000000e+00f, 0.00000000e+00f, 6.70166395e-01f, 6.94789568e-01f },
        { 1.02599126e-02f, 5.75556420e-01f, 6.27204726e+00f, 0.00000000e+00f, 0.00000000e+00f, 5.94841641e+00f, 2.91503762e-01f },
        { 2.17813393e-02f, 1.41472816e-01f, 3.94425820e-01f, 4.51107311e-03f, 3.37223068e+00f, 9.43976433e-02f, 2.87980437e-01f },
        { 9.08112869e-01f, 2.72866451e-01f, 2.57383302e-02f, 2.17813393e-02f, 3.33857518e-01f, 3.19970345e-01f, 0.00000000e+00f },
        { 3.91646858e+00f, 1.24474685e-01f, 3.94425820e-01f, 2.03509659e-01f, 4.08324829e-01f, 7.07567859e-01f, 2.76418549e-04f },
        { 6.56932093e-01f, 5.88125896e-01f, 6.58364357e-01f, 3.19970345e-01f, 4.93849709e+00f, 8.26976774e-02f, 0.00000000e+00f },
        { 2.57897455e-01f, 4.67083092e-01f, 7.61382452e-02f, 1.00000000e+00f, 6.20532884e-01f, 5.46540414e-01f, 1.34181406e-01f },
        { 3.73967795e-01f, 9.81136834e-03f, 2.56907719e-03f, 2.37920766e-01f, 7.67568454e-02f, 5.54814041e-02f, 7.69197598e-01f },
        { 5.71810474e-02f, 1.68466854e-01f, 6.58364357e-01f, 2.10182774e-01f, 1.97790101e-01f, 4.99511253e-02f, 8.53743010e-01f },
        { 3.05376142e-01f, 3.38872637e-01f, 5.56667197e-01f, 7.29505416e-01f, 3.51205195e-01f, 6.23041595e-02f, 4.60332272e-02f },
        { 4.51107311e-03f, 2.12765957e-02f, 5.45159785e-02f, 2.30819441e-01f, 2.30979263e+00f, 6.30402647e-01f, 6.31495147e-01f },
        { 1.16250271e-01f, 4.46373731e-01f, 7.92058433e-02f, 4.53521502e-02f, 1.24332202e-01f, 1.07572646e-01f, 1.00000000e+00f },
        { 5.12037033e-01f, 1.43869671e-01f, 2.82324350e-01f, 8.56408574e-02f, 1.13475952e+00f, 2.61713124e+00f, 7.05644037e-01f },
        { 8.27482123e-01f, 5.54814041e-02f, 1.73064291e-01f, 2.12765957e-02f, 3.87248159e-01f, 0.00000000e+00f, 1.78534703e-01f },
        { 4.19982322e-01f, 3.38872637e-01f, 2.37075839e-01f, 1.24996543e-01f, 3.65604412e-01f, 4.83427760e+00f, 2.12765957e-02f },
        { 5.60698501e-01f, 4.37500000e-01f, 6.64276333e-02f, 3.64115986e+00f, 7.61664733e-02f, 4.23772177e+00f, 2.41180314e-02f },
        { 2.28279175e-01f, 6.77981898e-01f, 5.15700453e-01f, 1.13475952e+00f, 3.42566551e-01f, 1.89444427e-02f, 0.00000000e+00f },
        { 3.05376142e-01f, 7.29505416e-01f, 3.87248159e-01f, 4.46373731e-01f, 5.15700453e-01f, 1.50196954e-01f, 4.98356736e-02f },
        { 7.89725885e-02f, 2.22238179e-01f, 1.40408140e-01f, 6.30402647e-01f, 2.34328370e-01f, 4.67771072e-01f, 3.55660520e-01f },
        { 1.00000000e+00f, 5.30693376e-01f, 0.00000000e+00f, 7.89725885e-02f, 1.07572646e-01f, 0.00000000e+00f, 0.00000000e+00f },
        { 1.79519595e-01f, 3.00722655e+00f, 2.96895494e-01f, 4.54152471e-01f, 1.64833143e-01f, 6.22014731e-02f, 2.56907719e-03f },
        { 3.64115986e+00f, 4.20242421e-01f, 4.69687371e-02f, 3.55672926e-01f, 3.83843003e-02f, 1.99336648e-01f, 4.44974194e+00f },
        { 2.28263583e-01f, 1.24332202e-01f, 4.31939169e+00f, 2.17813393e-02f, 4.19982322e-01f, 0.00000000e+00f, 0.00000000e+00f },
        { 1.16250271e-01f, 2.12765957e-02f, 2.56907719e-03f, 1.71376376e-01f, 3.55660520e-01f, 7.89725885e-02f, 8.27482123e-01f },
        { 4.09792664e-01f, 1.17116054e-01f, 3.06955290e-02f, 3.10637406e-01f, 5.89536436e-01f, 6.91933593e-02f, 2.76418549e-04f },
        { 8.27482123e-01f, 6.27204726e+00f, 3.83843003e-02f, 5.94841641e+00f, 2.91503762e-01f, 0.00000000e+00f, 0.00000000e+00f },
        { 3.38872637e-01f, 5.75556420e-01f, 5.94841641e+00f, 2.22238179e-01f, 3.40840199e-02f, 9.43976433e-02f, 3.87248159e-01f },
        { 1.00000000e+00f, 4.42364978e-01f, 0.00000000e+00f, 2.12765957e-02f, 7.69197598e-01f, 0.00000000e+00f, 0.00000000e+00f },
        { 3.43513652e-01f, 4.71570748e+00f, 5.08736477e+00f, 8.99784433e-01f, 1.87918216e-01f, 2.31129766e-01f, 1.07572646e-01f },
        { 5.60698501e-01f, 3.33857518e-01f, 1.52606456e-02f, 1.16042556e-01f, 3.55672926e-01f, 0.00000000e+00f, 1.66825018e-01f },
        { 6.47770921e-01f, 3.10637406e-01f, 4.09792664e-01f, 1.86372259e-01f, 3.55672926e-01f, 3.80353325e-01f, 1.73064291e-01f },
        { 1.39736263e-01f, 3.40425532e-01f, 1.65574846e-01f, 5.07989849e-02f, 3.59220336e-01f, 1.87918216e-01f, 2.04403902e-01f },
        { 2.91503762e-01f, 5.20720273e-01f, 0.00000000e+00f, 3.09487315e-01f, 5.60698501e-01f, 0.00000000e+00f, 0.00000000e+00f },
        { 2.78821044e-01f, 5.78841603e-01f, 6.94789568e-01f, 6.48081408e-02f, 9.29238171e-02f, 0.00000000e+00f, 0.00000000e+00f },
        { 1.24996543e-01f, 3.02967922e-02f, 2.17813393e-02f, 1.02599126e-02f, 7.39117303e-01f, 3.64115986e+00f, 6.31495147e-01f },
        { 4.07269911e-01f, 1.07572646e-01f, 5.46540414e-01f, 3.79015782e-02f, 3.79015782e-02f, 1.89444427e-02f, 4.31939169e+00f },
        { 5.12037033e-01f, 2.52834264e-01f, 3.79015782e-02f, 3.13102986e+00f, 5.35578923e-03f, 9.67341043e-03f, 3.13102986e+00f },
        { 5.60698501e-01f, 3.33857518e-01f, 1.52606456e-02f, 1.39736263e-01f, 9.29238171e-02f, 0.00000000e+00f, 2.39166667e-01f },
        { 2.27907798e-01f, 2.58110080e-02f, 1.37248444e-03f, 3.00722655e+00f, 3.00722655e+00f, 3.59476125e-01f, 1.86618674e-02f },
        { 9.08112869e-01f, 1.00000000e+00f, 1.89444427e-02f, 1.16250271e-01f, 0.00000000e+00f, 6.16666667e-01f, 4.42364978e-01f },
        { 3.91646858e+00f, 1.24474685e-01f, 3.55578004e-02f, 1.73064291e-01f, 3.87248159e-01f, 4.07269911e-01f, 4.00425255e-02f },
        { 1.54603951e-01f, 3.79015782e-02f, 4.42364978e-01f, 2.99446163e-01f, 1.97790101e-01f, 2.19089038e-01f, 7.06257576e-01f },
        { 3.19868219e-01f, 7.89725885e-02f, 4.51681154e-01f, 3.17844391e-01f, 3.20727405e-01f, 3.76801078e-01f, 0.00000000e+00f },
        { 5.95744681e-01f, 8.49680796e-01f, 1.51587396e-02f, 1.47679136e-01f, 4.00425255e-02f, 2.61713124e+00f, 1.75823274e-01f },
        { 4.09792664e-01f, 8.78846524e-02f, 4.30439277e-01f, 2.69234183e-01f, 2.96895494e-01f, 3.40425532e-01f, 7.66108700e-01f },
        { 2.15363503e-01f, 1.59288710e-01f, 2.87980437e-01f, 2.57897455e-01f, 1.37865925e-01f, 9.93747138e-02f, 3.11203504e-01f },
        { 1.00000000e+00f, 2.28263583e-01f, 3.64115986e+00f, 3.83843003e-02f, 6.48081408e-02f, 0.00000000e+00f, 2.84861373e-01f },
        { 7.39227084e-02f, 1.45752591e-01f, 1.16250271e-01f, 3.64115986e+00f, 4.16850236e+00f, 2.86242173e-01f, 1.00000000e+00f },
        { 2.91503762e-01f, 5.42220845e+00f, 0.00000000e+00f, 1.97657384e-01f, 2.75178434e-02f, 0.00000000e+00f, 0.00000000e+00f },
        { 3.02220293e-01f, 1.54603951e-01f, 3.48857065e-01f, 5.88125896e-01f, 2.22238179e-01f, 1.92379700e-01f, 3.38872637e-01f },
        { 4.55558447e-01f, 7.89725885e-02f, 1.37248444e-03f, 2.22238179e-01f, 2.58110080e-02f, 0.00000000e+00f, 5.80549685e-01f },
        { 2.72866451e-01f, 3.00722655e+00f, 1.64833143e-01f, 3.83843003e-02f, 2.16184779e-01f, 5.15700453e-01f, 6.64276333e-02f },
        { 4.09792664e-01f, 6.48081408e-02f, 2.58110080e-02f, 3.28171435e-01f, 5.56667197e-01f, 6.22014731e-02f, 3.74271559e-01f },
        { 7.03776698e-01f, 1.45752591e-01f, 4.05461890e-01f, 3.55672926e-01f, 1.86372259e-01f, 3.67899349e-01f, 2.91408145e-02f },
        { 3.91646858e+00f, 3.49344565e+00f, 1.17116054e-01f, 3.25238639e+00f, 4.69687371e-02f, 6.30402647e-01f, 2.40191026e-01f },
        { 8.30780596e-03f, 1.51450602e-02f, 4.46373731e-01f, 1.00000000e+00f, 2.61360589e-02f, 9.29238171e-02f, 2.50000000e-01f },
        { 2.30819441e-01f, 1.47679136e-01f, 5.14031621e-01f, 4.59422402e-01f, 1.51835395e-01f, 6.63744428e-01f, 1.73064291e-01f },
        { 9.16666667e-01f, 2.76418549e-04f, 0.00000000e+00f, 3.87248159e-01f, 2.50000000e-01f, 0.00000000e+00f, 0.00000000e+00f },
        { 5.15108051e-01f, 8.48016517e-02f, 9.43976433e-02f, 2.87980437e-01f, 4.72139446e-01f, 3.25238639e+00f, 2.37518650e-01f },
        { 5.60698501e-01f, 4.93204981e-01f, 3.09487315e-01f, 2.56470472e-01f, 8.48016517e-02f, 3.11203504e-01f, 3.44631486e-01f },
        { 4.42376860e-01f, 7.17612677e-01f, 2.69956384e-01f, 4.59422402e-01f, 3.14359568e-01f, 2.96895494e-01f, 5.04010599e-01f },
        { 6.27204726e+00f, 1.00000000e+00f, 2.91503762e-01f, 5.60698501e-01f, 1.86372259e-01f, 0.00000000e+00f, 5.10352990e-01f },
        { 4.31115688e-01f, 4.23772177e+00f, 7.89725885e-02f, 3.96543071e-01f, 8.51063830e-02f, 1.35557485e-01f, 6.16345868e-01f },
        { 4.01676029e-02f, 6.97127439e-02f, 3.74271559e-01f, 1.41472816e-01f, 6.47770921e-01f, 9.29238171e-02f, 4.31939169e+00f },
        { 2.28279175e-01f, 5.42220845e+00f, 5.15700453e-01f, 1.97657384e-01f, 3.51205195e-01f, 5.93269823e-02f, 0.00000000e+00f },
        { 5.54814041e-02f, 6.58849996e-02f, 7.92058433e-02f, 7.61382452e-02f, 1.07494800e-01f, 3.07348216e-01f, 3.87248159e-01f },
        { 9.36280610e-02f, 1.05296097e-04f, 2.17813393e-02f, 4.51107311e-03f, 3.55578004e-02f, 2.61713124e+00f, 1.24996543e-01f },
        { 2.39166667e-01f, 6.53392342e-01f, 9.81136834e-03f, 5.62788040e-02f, 2.83434241e+00f, 0.00000000e+00f, 8.56408574e-02f },
        { 1.97790101e-01f, 1.54447619e+00f, 6.94789568e-01f, 0.00000000e+00f, 2.41180314e-02f, 2.61713124e+00f, 7.92510523e-01f },
        { 3.05376142e-01f, 2.15363503e-01f, 3.87248159e-01f, 1.51140431e-01f, 9.93747138e-02f, 2.39166667e-01f, 4.98356736e-02f },
        { 6.16666667e-01f, 3.20727405e-01f, 5.45159785e-02f, 4.93204981e-01f, 1.07572646e-01f, 4.22445305e-02f, 8.56408574e-02f },
        { 1.86618674e-02f, 6.29749985e-01f, 8.53743010e-01f, 4.09792664e-01f, 3.76785400e+00f, 2.16184779e-01f, 2.76418549e-04f },
        { 1.02599126e-02f, 3.74271559e-01f, 2.62686741e-03f, 4.51107311e-03f, 3.25238639e+00f, 7.36404336e-01f, 3.13102986e+00f },
        { 3.76785400e+00f, 2.82324350e-01f, 8.06933343e-02f, 4.79267501e-01f, 4.09792664e-01f, 5.14390914e-01f, 3.30147052e-01f },
        { 4.67083092e-01f, 5.99895907e-02f, 2.57897455e-01f, 3.25238639e+00f, 9.54427083e-02f, 6.20532884e-01f, 7.61382452e-02f },
        { 1.00000000e+00f, 4.92767464e-01f, 0.00000000e+00f, 4.20242421e-01f, 4.46373731e-01f, 0.00000000e+00f, 0.00000000e+00f },
        { 4.25818187e-01f, 3.00722655e+00f, 2.58110080e-02f, 4.19982322e-01f, 8.19340639e-02f, 4.99511253e-02f, 6.23041595e-02f },
        { 1.64833143e-01f, 1.79432010e-01f, 6.94789568e-01f, 1.02599126e-02f, 0.00000000e+00f, 3.49344565e+00f, 1.59288710e-01f },
        { 2.28263583e-01f, 7.77161370e-01f, 3.84389091e+00f, 2.58932688e-01f, 1.84604774e-01f, 0.00000000e+00f, 0.00000000e+00f },
        { 1.16250271e-01f, 2.62686741e-03f, 7.39951300e-01f, 4.51107311e-03f, 4.51107311e-03f, 7.69197598e-01f, 1.07572646e-01f },
        { 4.71570748e+00f, 3.43513652e-01f, 3.55672926e-01f, 2.16184779e-01f, 2.31129766e-01f, 5.14031621e-01f, 8.19340639e-02f },
        { 6.03745111e-01f, 3.10637406e-01f, 9.81136834e-03f, 7.61382452e-02f, 9.93747138e-02f, 0.00000000e+00f, 0.00000000e+00f },
        { 5.14031621e-01f, 5.04010599e-01f, 1.99336648e-01f, 3.92579451e-01f, 2.12765957e-02f, 5.23122382e-01f, 3.20727405e-01f },
        { 7.39117303e-01f, 2.61713124e+00f, 1.22743268e-01f, 1.24474685e-01f, 2.16184779e-01f, 2.12765957e-02f, 9.81136834e-03f },
        { 2.91503762e-01f, 7.07567859e-01f, 0.00000000e+00f, 1.00000000e+00f, 4.83427760e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { 2.72715400e-01f, 7.61664733e-02f, 5.00000000e-01f, 4.16850236e+00f, 4.43587164e-02f, 2.50000000e-01f, 3.51205195e-01f },
        { 5.15108051e-01f, 3.05376142e-01f, 9.43976433e-02f, 3.07348216e-01f, 4.04397337e+00f, 4.53521502e-02f, 5.95744681e-01f },
        { 9.81136834e-03f, 1.57260091e-02f, 2.61713124e+00f, 1.02599126e-02f, 5.99895907e-02f, 5.81162234e-01f, 4.72139446e-01f },
        { 5.60698501e-01f, 4.93880286e-01f, 3.00722655e+00f, 3.91646858e+00f, 2.61713124e+00f, 0.00000000e+00f, 2.04403902e-01f },
        { 3.83843003e-02f, 4.93849709e+00f, 7.17612677e-01f, 2.85170718e-01f, 5.98662936e-03f, 5.08736477e+00f, 1.17116054e-01f },
        { 5.91395256e-01f, 2.27907798e-01f, 5.98662936e-03f, 2.58110080e-02f, 4.95637591e-01f, 3.87248159e-01f, 3.76801078e-01f },
        { 1.00000000e+00f, 2.56470472e-01f, 0.00000000e+00f, 1.22617022e-01f, 9.43976433e-02f, 0.00000000e+00f, 0.00000000e+00f },
        { 1.00000000e+00f, 5.14031621e-01f, 1.23732063e-01f, 5.71810474e-02f, 5.08736112e-01f, 2.31129766e-01f, 0.00000000e+00f },
        { 1.02599126e-02f, 5.60698501e-01f, 2.18733064e-02f, 0.00000000e+00f, 0.00000000e+00f, 5.99895907e-02f, 5.60698501e-01f },
        { 1.00000000e+00f, 7.82608696e-01f, 1.23732063e-01f, 5.14031621e-01f, 1.35805554e-01f, 6.31495147e-01f, 0.00000000e+00f },
        { 5.95744681e-01f, 4.42364978e-01f, 1.54603951e-01f, 4.43587164e-02f, 7.69197598e-01f, 1.75823274e-01f, 0.00000000e+00f },
        { 9.81136834e-03f, 2.12765957e-02f, 4.71570748e+00f, 4.71570748e+00f, 2.76418549e-04f, 8.99784433e-01f, 3.80472285e-01f },
        { 3.83843003e-02f, 4.93849709e+00f, 7.39951300e-01f, 2.96895494e-01f, 5.42220845e+00f, 5.08736477e+00f, 7.67568454e-02f },
        { 2.28279175e-01f, 5.42220845e+00f, 8.07578250e-01f, 1.75823274e-01f, 2.69956384e-01f, 1.00000000e+00f, 0.00000000e+00f },
        { 5.60698501e-01f, 8.30780596e-03f, 1.01377315e-01f, 1.02599126e-02f, 1.16250271e-01f, 7.67568454e-02f, 2.41180314e-02f },
        { 2.16184779e-01f, 5.24355258e+00f, 1.68466854e-01f, 2.61713124e+00f, 1.24996543e-01f, 3.92270074e-01f, 5.71810474e-02f },
        { 3.73967795e-01f, 1.54603951e-01f, 9.93747138e-02f, 1.00000000e+00f, 3.19868219e-01f, 1.45752591e-01f, 3.94425820e-01f },
        { 7.29505416e-01f, 1.55792384e-01f, 4.65510775e-01f, 5.56854111e-01f, 2.46271310e-01f, 1.75795077e-01f, 3.61112186e-01f },
        { 1.00000000e+00f, 6.34728506e-01f, 0.00000000e+00f, 8.19340639e-02f, 4.46373731e-01f, 0.00000000e+00f, 0.00000000e+00f },
        { 2.28279175e-01f, 1.13475952e+00f, 5.15700453e-01f, 1.97790101e-01f, 2.75296998e-01f, 5.93269823e-02f, 0.00000000e+00f },
        { 2.78821044e-01f, 7.23404255e-01f, 1.97790101e-01f, 1.57260091e-02f, 1.24996543e-01f, 0.00000000e+00f, 0.00000000e+00f },
        { 1.99336648e-01f, 4.31889468e-01f, 6.16666667e-01f, 5.10352990e-01f, 4.92767464e-01f, 3.14705378e-01f, 5.08736477e+00f },
        { 3.35494995e-01f, 2.56470472e-01f, 7.61664733e-02f, 1.35557485e-01f, 4.89172847e-01f, 1.01500707e-01f, 8.51063830e-02f },
        { 1.83409739e-01f, 4.54152471e-01f, 2.56907719e-03f, 5.89536436e-01f, 3.64115986e+00f, 0.00000000e+00f, 7.07567859e-01f },
        { 1.00000000e+00f, 8.75792699e-01f, 3.28253058e-01f, 5.60698501e-01f, 0.00000000e+00f, 0.00000000e+00f, 5.54814041e-02f },
        { 3.09487315e-01f, 4.63412762e-01f, 1.75823274e-01f, 2.27907798e-01f, 1.97657384e-01f, 1.45752591e-01f, 8.06933343e-02f },
        { 1.00000000e+00f, 4.01676029e-02f, 0.00000000e+00f, 4.88016660e-01f, 4.23469599e-01f, 0.00000000e+00f, 0.00000000e+00f },
        { 2.58932688e-01f, 2.83434241e+00f, 5.20720273e-01f, 0.00000000e+00f, 1.56439067e-01f, 1.97657384e-01f, 2.79770276e-01f },
        { 4.59422402e-01f, 1.83409739e-01f, 1.99692531e-01f, 5.56667197e-01f, 3.28171435e-01f, 1.51587396e-02f, 1.04691358e-01f },
        { 4.99511253e-02f, 6.31495147e-01f, 4.55558447e-01f, 2.27827935e-01f, 7.93086902e-02f, 3.13102986e+00f, 1.86618674e-02f },
        { 1.45752591e-01f, 5.07989849e-02f, 2.17813393e-02f, 2.31129766e-01f, 7.77161370e-01f, 0.00000000e+00f, 4.31889468e-01f },
        { 5.42220845e+00f, 1.75823274e-01f, 6.22014731e-02f, 9.29238171e-02f, 4.44974194e+00f, 1.49778499e-01f, 5.75556420e-01f },
        { 2.72715400e-01f, 4.16850236e+00f, 7.39227084e-02f, 1.51140431e-01f, 7.61382452e-02f, 4.42364978e-01f, 3.94425820e-01f },
        { 4.00425255e-02f, 5.42220845e+00f, 9.67341043e-03f, 9.29238171e-02f, 1.49778499e-01f, 5.42220845e+00f, 1.51587396e-02f },
        { 7.92058433e-02f, 7.02757147e-01f, 2.91408145e-02f, 7.77986059e-01f, 5.45159785e-02f, 2.07355783e-01f, 9.43976433e-02f },
        { 3.91646858e+00f, 7.48102816e-01f, 2.22238179e-01f, 3.13102986e+00f, 8.51063830e-02f, 4.22445305e-02f, 2.44499236e-01f },
        { 6.56932093e-01f, 1.78480191e-01f, 4.98774380e-01f, 8.78846524e-02f, 9.29238171e-02f, 8.71549309e-02f, 2.18733064e-02f },
        { 2.57897455e-01f, 9.06356450e-02f, 1.12821614e-01f, 4.00425255e-02f, 3.25238639e+00f, 3.80472285e-01f, 4.09792664e-01f },
        { 5.60698501e-01f, 4.93204981e-01f, 2.17813393e-02f, 1.19384398e-01f, 7.61664733e-02f, 5.70264661e-01f, 1.99074074e-02f },
        { 6.32218364e-01f, 6.77981898e-01f, 5.08736112e-01f, 3.55578004e-02f, 4.51681154e-01f, 3.20727405e-01f, 5.30693376e-01f },
        { 3.02220293e-01f, 1.45752591e-01f, 4.15348355e-01f, 1.12821614e-01f, 2.22238179e-01f, 3.38872637e-01f, 0.00000000e+00f },
        { 3.94425820e-01f, 3.55672926e-01f, 2.50000000e-01f, 2.31129766e-01f, 4.30439277e-01f, 3.92579451e-01f, 5.99895907e-02f },
        { 1.00000000e+00f, 2.14697073e-01f, 0.00000000e+00f, 1.79519595e-01f, 5.34556384e-01f, 0.00000000e+00f, 0.00000000e+00f },
        { 1.02599126e-02f, 5.62261641e-01f, 3.14293981e-02f, 6.70166395e-01f, 1.01500707e-01f, 3.79015782e-02f, 1.31569103e-01f },
        { 2.87980437e-01f, 2.15363503e-01f, 2.17813393e-02f, 5.48918260e-02f, 9.93747138e-02f, 5.75556420e-01f, 3.94425820e-01f },
        { 4.00425255e-02f, 2.58816057e-01f, 1.52606456e-02f, 2.34180133e-01f, 6.47770921e-01f, 6.58849996e-02f, 2.58816057e-01f },
        { 6.08251296e-01f, 2.72866451e-01f, 1.56439067e-01f, 1.54603951e-01f, 3.74271559e-01f, 0.00000000e+00f, 0.00000000e+00f },
        { 9.12907536e-01f, 2.03509659e-01f, 5.93269823e-02f, 3.14293981e-02f, 3.84389091e+00f, 1.01500707e-01f, 2.68685192e-01f },
        { 3.30343364e-04f, 1.51587396e-02f, 5.00000000e-01f, 1.02599126e-02f, 8.07578250e-01f, 4.69687371e-02f, 4.31115688e-01f },
        { 4.51107311e-03f, 2.12765957e-02f, 6.31495147e-01f, 2.14697073e-01f, 3.47550299e-01f, 2.69234183e-01f, 2.56470472e-01f },
        { 1.00000000e+00f, 2.08109314e-01f, 0.00000000e+00f, 8.23790361e-01f, 4.93204981e-01f, 0.00000000e+00f, 0.00000000e+00f },
        { 4.42376860e-01f, 4.46373731e-01f, 2.76418549e-04f, 7.89725885e-02f, 1.41472816e-01f, 2.12765957e-02f, 3.74271559e-01f },
        { 3.77176518e-01f, 6.91860026e-01f, 9.93747138e-02f, 3.65604412e-01f, 1.84604774e-01f, 5.60698501e-01f, 1.73064291e-01f },
        { 1.68466854e-01f, 3.28331970e-01f, 1.24996543e-01f, 1.86553819e-01f, 2.41180314e-02f, 1.50196954e-01f, 2.37518650e-01f },
        { 6.47770921e-01f, 1.06117499e-01f, 1.89444427e-02f, 4.04397337e+00f, 3.64115986e+00f, 2.56470472e-01f, 4.88016660e-01f },
        { 1.68466854e-01f, 3.28331970e-01f, 2.16184779e-01f, 1.86553819e-01f, 5.56667197e-01f, 3.53630515e-01f, 2.56470472e-01f },
        { 1.19384398e-01f, 2.12765957e-02f, 2.58816057e-01f, 7.61382452e-02f, 2.91013718e-02f, 6.30402647e-01f, 4.20242421e-01f },
        { 3.38872637e-01f, 3.37633832e-01f, 2.68685192e-01f, 1.47679136e-01f, 4.44974194e+00f, 5.35239225e-01f, 3.94425820e-01f },
        { 1.54447619e+00f, 4.25531915e-02f, 2.16184779e-01f, 8.53743010e-01f, 0.00000000e+00f, 6.16453615e-01f, 2.61713124e+00f },
        { 3.80335569e-01f, 3.73194814e-01f, 5.60698501e-01f, 1.08075453e-01f, 3.73967795e-01f, 1.97657384e-01f, 3.00722655e+00f },
        { 5.51747691e-01f, 2.06511061e-01f, 2.75296998e-01f, 5.27236035e-01f, 3.35494995e-01f, 3.91646858e+00f, 2.96895494e-01f },
        { 3.96543071e-01f, 4.31115688e-01f, 3.67899349e-01f, 6.16453615e-01f, 2.50000000e-01f, 5.95744681e-01f, 1.51643907e-01f },
        { 4.42364978e-01f, 5.95744681e-01f, 6.56932093e-01f, 5.60407022e-02f, 2.27907798e-01f, 2.56907719e-03f, 5.15700453e-01f },
        { 1.78480191e-01f, 3.49344565e+00f, 2.76418549e-04f, 5.56667197e-01f, 6.23041595e-02f, 5.00000000e-01f, 9.54427083e-02f },
        { 3.91646858e+00f, 3.11113237e-01f, 3.40840199e-02f, 1.97790101e-01f, 5.88125896e-01f, 2.39166667e-01f, 1.24996543e-01f },
        { 9.06356450e-02f, 4.40243553e-01f, 5.91395256e-01f, 2.15363503e-01f, 3.05376142e-01f, 4.23469599e-01f, 3.14293981e-02f },
        { 6.53392342e-01f, 9.81136834e-03f, 3.02967922e-02f, 2.37920766e-01f, 3.00722655e+00f, 2.72866451e-01f, 6.80928062e-02f },
        { 4.09792664e-01f, 3.79015782e-02f, 3.40425532e-01f, 1.86618674e-02f, 6.31495147e-01f, 2.58932688e-01f, 3.10637406e-01f },
        { 7.69197598e-01f, 6.27204726e+00f, 1.24996543e-01f, 1.35557485e-01f, 5.10352990e-01f, 0.00000000e+00f, 2.16184779e-01f },
        { 4.44974194e+00f, 4.46373731e-01f, 2.91013718e-02f, 2.19089038e-01f, 4.93386620e-01f, 0.00000000e+00f, 2.31129766e-01f },
        { 1.00000000e+00f, 2.34180133e-01f, 0.00000000e+00f, 6.48081408e-02f, 1.07494800e-01f, 0.00000000e+00f, 0.00000000e+00f },
        { 4.09792664e-01f, 3.79015782e-02f, 6.04630904e-01f, 3.37633832e-01f, 6.31495147e-01f, 4.71570748e+00f, 5.24355258e+00f },
        { 1.87918216e-01f, 5.48918260e-02f, 3.05376142e-01f, 1.97790101e-01f, 1.55792384e-01f, 9.29238171e-02f, 1.55792384e-01f },
        { 8.99784433e-01f, 3.02220293e-01f, 5.60698501e-01f, 5.51747691e-01f, 4.23772177e+00f, 1.59569405e-01f, 2.52834264e-01f },
        { 6.53392342e-01f, 9.81136834e-03f, 2.83434241e+00f, 3.73967795e-01f, 2.16184779e-01f, 0.00000000e+00f, 5.08337922e-01f },
        { 1.64833143e-01f, 1.54447619e+00f, 1.52606456e-02f, 0.00000000e+00f, 2.07355783e-01f, 2.10182774e-01f, 2.91013718e-02f },
        { 2.17813393e-02f, 1.41472816e-01f, 1.24996543e-01f, 1.86372259e-01f, 3.83843003e-02f, 7.82608696e-01f, 2.07355783e-01f },
        { 1.99336648e-01f, 5.23122382e-01f, 3.47862269e-01f, 1.31569103e-01f, 8.30780596e-03f, 2.41180314e-02f, 1.86372259e-01f },
        { 5.60407022e-02f, 2.27907798e-01f, 5.27715860e-01f, 5.62788040e-02f, 4.40243553e-01f, 2.79770276e-01f, 6.91933593e-02f },
        { 6.53392342e-01f, 2.10454796e-01f, 1.24332202e-01f, 2.61360589e-02f, 5.88125896e-01f, 6.70166395e-01f, 9.93747138e-02f },
        { 6.63744428e-01f, 3.19970345e-01f, 6.08251296e-01f, 7.61382452e-02f, 4.02951018e-01f, 9.45474131e-02f, 8.75792699e-01f },
        { 9.06356450e-02f, 4.69687371e-02f, 9.54427083e-02f, 3.55578004e-02f, 5.62788040e-02f, 6.27204726e+00f, 3.40425532e-01f },
        { 5.12037033e-01f, 3.25238639e+00f, 3.49344565e+00f, 3.67899349e-01f, 5.08736112e-01f, 2.61713124e+00f, 2.50000000e-01f },
        { 1.54447619e+00f, 0.00000000e+00f, 9.36280610e-02f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f, 9.81136834e-03f },
        { 3.33857518e-01f, 5.60698501e-01f, 6.03745111e-01f, 2.40191026e-01f, 1.52606456e-02f, 7.17612677e-01f, 7.98480479e-01f },
        { 1.86618674e-02f, 8.30780596e-03f, 1.24996543e-01f, 1.52606456e-02f, 2.27827935e-01f, 3.02967922e-02f, 2.37518650e-01f },
        { 4.93204981e-01f, 4.93880286e-01f, 2.12765957e-02f, 4.65182653e-01f, 1.83409739e-01f, 1.78534703e-01f, 7.66631354e-01f },
        { 1.35805554e-01f, 3.91646858e+00f, 1.02599126e-02f, 3.49344565e+00f, 7.36404336e-01f, 2.69234183e-01f, 2.61360589e-02f },
        { 7.48102816e-01f, 3.55672926e-01f, 3.38872637e-01f, 2.15363503e-01f, 3.91646858e+00f, 1.37865925e-01f, 1.17116054e-01f },
        { 1.05296097e-04f, 3.76785400e+00f, 6.32218364e-01f, 4.51107311e-03f, 0.00000000e+00f, 4.69687371e-02f, 3.70478304e-01f },
        { 4.42364978e-01f, 9.81136834e-03f, 6.56932093e-01f, 5.60407022e-02f, 1.02599126e-02f, 2.56907719e-03f, 5.15700453e-01f },
        { 1.41472816e-01f, 1.63048254e-01f, 3.13102986e+00f, 3.91646858e+00f, 1.64833143e-01f, 1.24332202e-01f, 5.89536436e-01f },
        { 4.20242421e-01f, 4.08324829e-01f, 4.65182653e-01f, 8.51063830e-02f, 4.59422402e-01f, 9.67341043e-03f, 3.91646858e+00f },
        { 7.89725885e-02f, 3.17844391e-01f, 1.44550810e-01f, 4.31115688e-01f, 1.86618674e-02f, 9.83702534e-02f, 3.20727405e-01f },
        { 4.72139446e-01f, 4.20872854e-01f, 4.05461890e-01f, 2.16184779e-01f, 5.04010599e-01f, 4.71570748e+00f, 1.78534703e-01f },
        { 6.32218364e-01f, 1.45752591e-01f, 5.27715860e-01f, 3.91646858e+00f, 5.62788040e-02f, 3.20727405e-01f, 3.51205195e-01f },
        { 6.27204726e+00f, 5.94841641e+00f, 0.00000000e+00f, 6.30402647e-01f, 3.47550299e-01f, 0.00000000e+00f, 0.00000000e+00f },
        { 2.58932688e-01f, 5.60648320e+00f, 3.96543071e-01f, 2.83434241e+00f, 1.59569405e-01f, 4.93880286e-01f, 2.99446163e-01f },
        { 2.56470472e-01f, 3.84389091e+00f, 1.22617022e-01f, 1.86372259e-01f, 1.71376376e-01f, 1.39736263e-01f, 5.15108051e-01f },
        { 2.28279175e-01f, 5.42220845e+00f, 5.15700453e-01f, 5.24355258e+00f, 3.87248159e-01f, 1.89444427e-02f, 0.00000000e+00f },
        { 8.27482123e-01f, 7.48102816e-01f, 1.78534703e-01f, 1.73078306e-01f, 1.22617022e-01f, 1.31085329e-01f, 0.00000000e+00f },
        { 6.86242327e-02f, 2.37920766e-01f, 5.89536436e-01f, 4.26960979e-01f, 2.96895494e-01f, 4.23426081e-01f, 5.46841557e-01f },
        { 5.12037033e-01f, 4.79267501e-01f, 8.48016517e-02f, 6.47770921e-01f, 2.71474988e-01f, 1.78480191e-01f, 8.06933343e-02f },
        { 5.42220845e+00f, 3.43513652e-01f, 3.87248159e-01f, 1.81123927e-01f, 6.29749985e-01f, 4.43587164e-02f, 4.63412762e-01f },
        { 7.03776698e-01f, 2.87980437e-01f, 2.57383302e-02f, 2.15363503e-01f, 3.65604412e-01f, 0.00000000e+00f, 2.86242173e-01f },
        { 2.28263583e-01f, 1.54603951e-01f, 4.69687371e-02f, 1.00000000e+00f, 2.51937466e-01f, 0.00000000e+00f, 4.01319363e-01f },
        { 1.00000000e+00f, 2.14697073e-01f, 0.00000000e+00f, 6.48081408e-02f, 5.34556384e-01f, 0.00000000e+00f, 0.00000000e+00f },
        { 1.13475952e+00f, 9.29238171e-02f, 2.75296998e-01f, 1.73064291e-01f, 1.73064291e-01f, 7.37999948e-02f, 9.69424672e-01f },
        { 4.46373731e-01f, 6.58849996e-02f, 4.93386620e-01f, 6.48081408e-02f, 5.89536436e-01f, 1.34181406e-01f, 1.51587396e-02f },
        { 4.51107311e-03f, 3.50635231e-01f, 7.39117303e-01f, 1.05296097e-04f, 3.70478304e-01f, 1.02599126e-02f, 4.65510775e-01f },
        { 5.95744681e-01f, 3.35494995e-01f, 3.47862269e-01f, 4.52567064e-01f, 7.61664733e-02f, 3.63273695e-01f, 5.71810474e-02f },
        { 4.09792664e-01f, 5.10352990e-01f, 3.28171435e-01f, 1.78480191e-01f, 6.58849996e-02f, 6.27204726e+00f, 2.39166667e-01f },
        { 3.13102986e+00f, 4.25531915e-02f, 3.38872637e-01f, 5.07989849e-02f, 1.22617022e-01f, 3.37223068e+00f, 5.60698501e-01f },
        { 6.32218364e-01f, 3.02220293e-01f, 4.72139446e-01f, 5.56854111e-01f, 1.25656780e-01f, 4.73524167e-01f, 1.12821614e-01f },
        { 4.55558447e-01f, 2.56470472e-01f, 3.48857065e-01f, 4.59422402e-01f, 4.93386620e-01f, 4.93204981e-01f, 7.39227084e-02f },
        { 5.42220845e+00f, 1.23732063e-01f, 4.59422402e-01f, 9.29238171e-02f, 1.73064291e-01f, 7.01318023e+00f, 5.99895907e-02f },
        { 7.48102816e-01f, 3.55660520e-01f, 3.07348216e-01f, 1.02599126e-02f, 1.12821614e-01f, 7.39227084e-02f, 1.79519595e-01f },
        { 4.04397337e+00f, 6.30402647e-01f, 2.51937466e-01f, 1.00000000e+00f, 1.23732063e-01f, 6.58364357e-01f, 3.30147052e-01f },
        { 7.39227084e-02f, 1.50196954e-01f, 3.87248159e-01f, 2.19089038e-01f, 7.17612677e-01f, 8.48016517e-02f, 2.12765957e-02f },
        { 1.64833143e-01f, 2.30979263e+00f, 7.39117303e-01f, 1.07386732e-01f, 2.60697097e-01f, 5.20720273e-01f, 0.00000000e+00f },
        { 5.89536436e-01f, 2.41180314e-02f, 5.27715860e-01f, 2.69234183e-01f, 2.39166667e-01f, 3.40840199e-02f, 1.15655035e-01f },
        { 9.36280610e-02f, 4.25531915e-02f, 2.30979263e+00f, 8.53743010e-01f, 0.00000000e+00f, 2.84861373e-01f, 4.62697480e-02f },
        { 5.27715860e-01f, 5.88125896e-01f, 1.92379700e-01f, 8.71549309e-02f, 3.25238639e+00f, 1.51587396e-02f, 1.24332202e-01f },
        { 3.40840199e-02f, 8.06933343e-02f, 6.31495147e-01f, 5.60407022e-02f, 2.44499236e-01f, 1.31569103e-01f, 1.73064291e-01f },
        { 2.37075839e-01f, 4.79267501e-01f, 2.87980437e-01f, 8.27482123e-01f, 4.93849709e+00f, 2.15363503e-01f, 3.94425820e-01f },
        { 4.31115688e-01f, 5.78841603e-01f, 6.69452643e-01f, 3.17844391e-01f, 3.98432113e-01f, 9.16707678e-02f, 2.31129766e-01f },
        { 3.74271559e-01f, 8.19340639e-02f, 4.23772177e+00f, 4.98356736e-02f, 2.82459586e-01f, 6.08682874e-01f, 8.51063830e-02f },
        { 3.79015782e-02f, 8.48016517e-02f, 4.22445305e-02f, 1.02599126e-02f, 3.38872637e-01f, 5.46540414e-01f, 4.01676029e-02f },
        { 1.02041694e-01f, 1.00286658e-01f, 7.61664733e-02f, 8.48016517e-02f, 6.77981898e-01f, 1.22743268e-01f, 5.54814041e-02f },
        { 3.65604412e-01f, 2.10182774e-01f, 2.68685192e-01f, 4.42376860e-01f, 4.93204981e-01f, 4.93386620e-01f, 1.57260091e-02f },
        { 2.56907719e-03f, 3.23789880e-01f, 1.16250271e-01f, 6.49678831e-01f, 4.71570748e+00f, 5.60698501e-01f, 2.16184779e-01f },
        { 4.95637591e-01f, 1.02599126e-02f, 5.99895907e-02f, 3.14705378e-01f, 2.56907719e-03f, 1.00000000e+00f, 2.37920766e-01f },
        { 1.64833143e-01f, 1.16042556e-01f, 1.34235224e-02f, 1.02599126e-02f, 0.00000000e+00f, 3.49344565e+00f, 3.37223068e+00f },
        { 4.44974194e+00f, 9.93747138e-02f, 3.09487315e-01f, 6.30402647e-01f, 2.83434241e+00f, 5.60648320e+00f, 1.41472816e-01f },
        { 1.00000000e+00f, 6.91933593e-02f, 0.00000000e+00f, 7.61382452e-02f, 1.86618674e-02f, 0.00000000e+00f, 0.00000000e+00f },
        { 4.09792664e-01f, 1.17116054e-01f, 1.04134195e-01f, 4.51107311e-03f, 1.50196954e-01f, 1.37865925e-01f, 5.23122382e-01f },
        { 2.17813393e-02f, 1.24996543e-01f, 1.24996543e-01f, 1.02599126e-02f, 1.31569103e-01f, 3.77176518e-01f, 2.19089038e-01f },
        { 4.19982322e-01f, 3.09487315e-01f, 2.37075839e-01f, 8.26976774e-02f, 1.75823274e-01f, 4.83427760e+00f, 6.30402647e-01f },
        { 1.00000000e+00f, 4.43587164e-02f, 0.00000000e+00f, 2.31129766e-01f, 7.77161370e-01f, 0.00000000e+00f, 0.00000000e+00f },
        { 1.97790101e-01f, 2.61713124e+00f, 6.94789568e-01f, 3.44631486e-01f, 5.24355258e+00f, 1.68466854e-01f, 6.63744428e-01f },
        { 6.08682874e-01f, 2.41180314e-02f, 2.07355783e-01f, 5.80549685e-01f, 2.39166667e-01f, 3.10637406e-01f, 1.41472816e-01f },
        { 1.45752591e-01f, 2.22238179e-01f, 5.60698501e-01f, 4.17933207e-01f, 9.83702534e-02f, 4.89172847e-01f, 2.41180314e-02f },
        { 5.30693376e-01f, 5.60407022e-02f, 2.85170718e-01f, 1.02599126e-02f, 4.88016660e-01f, 4.46373731e-01f, 8.40729270e-01f },
        { 1.54447619e+00f, 0.00000000e+00f, 9.36280610e-02f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f, 9.81136834e-03f },
        { 2.82324350e-01f, 4.16850236e+00f, 1.97790101e-01f, 4.29343799e-01f, 5.99895907e-02f, 1.92379700e-01f, 4.93386620e-01f },
        { 4.44974194e+00f, 1.75795077e-01f, 1.31085329e-01f, 9.93747138e-02f, 6.31495147e-01f, 5.78841603e-01f, 4.93880286e-01f },
        { 8.88512973e-01f, 2.04403902e-01f, 1.81123927e-01f, 4.01676029e-02f, 4.20242421e-01f, 0.00000000e+00f, 5.60698501e-01f },
        { 3.55578004e-02f, 3.92270074e-01f, 9.43976433e-02f, 2.15363503e-01f, 4.16850236e+00f, 5.14031621e-01f, 5.48918260e-02f },
        { 3.13102986e+00f, 4.22445305e-02f, 1.51587396e-02f, 6.80928062e-02f, 2.99446163e-01f, 4.23772177e+00f, 3.02967922e-02f },
        { 3.19868219e-01f, 4.98774380e-01f, 4.51681154e-01f, 6.16453615e-01f, 4.92767464e-01f, 7.48102816e-01f, 0.00000000e+00f },
        { 4.98774380e-01f, 3.44631486e-01f, 5.93269823e-02f, 3.49344565e+00f, 2.96895494e-01f, 3.02967922e-02f, 3.19369975e-01f },
        { 6.27204726e+00f, 1.75795077e-01f, 3.67899349e-01f, 7.39951300e-01f, 9.29238171e-02f, 4.23426081e-01f, 2.28279175e-01f },
        { 3.38872637e-01f, 3.06955290e-02f, 2.68685192e-01f, 3.40840199e-02f, 5.56667197e-01f, 3.51205195e-01f, 4.15348355e-01f },
        { 3.38872637e-01f, 3.25238639e+00f, 2.68685192e-01f, 2.37518650e-01f, 1.07031098e-01f, 9.54427083e-02f, 4.31115688e-01f },
        { 1.00000000e+00f, 1.16250271e-01f, 0.00000000e+00f, 2.79770276e-01f, 5.42220845e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { 8.81523697e-01f, 1.02599126e-02f, 2.10182774e-01f, 4.51681154e-01f, 1.85401135e-02f, 7.06257576e-01f, 3.67899349e-01f },
        { 6.27204726e+00f, 5.94841641e+00f, 2.91503762e-01f, 4.69687371e-02f, 9.06356450e-02f, 0.00000000e+00f, 4.99511253e-02f },
        { 4.31115688e-01f, 9.43976433e-02f, 1.47679136e-01f, 3.17844391e-01f, 1.47679136e-01f, 5.10352990e-01f, 3.87248159e-01f },
        { 1.47679136e-01f, 9.08112869e-01f, 1.02599126e-02f, 5.33790060e-01f, 1.89444427e-02f, 0.00000000e+00f, 2.56470472e-01f },
        { 8.81523697e-01f, 1.78480191e-01f, 2.10182774e-01f, 1.37248444e-03f, 5.00000000e-01f, 7.06257576e-01f, 6.16666667e-01f },
        { 4.20242421e-01f, 2.87980437e-01f, 2.12765957e-02f, 2.15363503e-01f, 2.39166667e-01f, 3.80472285e-01f, 1.02599126e-02f },
        { 2.27907798e-01f, 6.23041595e-02f, 4.95637591e-01f, 3.80472285e-01f, 7.23404255e-01f, 6.47770921e-01f, 4.88016660e-01f },
        { 4.23772177e+00f, 4.07269911e-01f, 2.07355783e-01f, 1.15655035e-01f, 4.05461890e-01f, 3.38872637e-01f, 3.87248159e-01f },
        { 6.31495147e-01f, 2.56470472e-01f, 3.79015782e-02f, 1.31569103e-01f, 5.89536436e-01f, 1.02041694e-01f, 1.41472816e-01f },
        { 7.39951300e-01f, 5.08736477e+00f, 4.93849709e+00f, 9.93747138e-02f, 1.01500707e-01f, 2.58932688e-01f, 1.25656780e-01f },
        { 1.35805554e-01f, 4.51681154e-01f, 1.52606456e-02f, 3.11113237e-01f, 3.12692901e-03f, 1.01404934e-03f, 1.04134195e-01f },
        { 9.81136834e-03f, 4.00425255e-02f, 2.50000000e-01f, 1.02599126e-02f, 3.73967795e-01f, 1.35557485e-01f, 2.56470472e-01f },
        { 5.20720273e-01f, 3.09487315e-01f, 6.20532884e-01f, 1.56439067e-01f, 5.42220845e+00f, 5.30693376e-01f, 2.39166667e-01f },
        { 2.40191026e-01f, 4.69687371e-02f, 1.45752591e-01f, 4.74018733e-01f, 1.87918216e-01f, 6.08362269e-03f, 3.17844391e-01f },
        { 4.93204981e-01f, 3.09487315e-01f, 2.37075839e-01f, 3.40840199e-02f, 1.45752591e-01f, 1.06117499e-01f, 4.22445305e-02f },
        { 3.25238639e+00f, 1.51835395e-01f, 6.58364357e-01f, 6.58364357e-01f, 3.17844391e-01f, 5.56667197e-01f, 7.92510523e-01f },
        { 8.67139443e-01f, 6.77981898e-01f, 7.22001320e-01f, 6.32218364e-01f, 4.43587164e-02f, 2.91408145e-02f, 7.92058433e-02f },
        { 7.03776698e-01f, 1.86372259e-01f, 5.46540414e-01f, 3.96543071e-01f, 3.28253058e-01f, 0.00000000e+00f, 3.07348216e-01f },
        { 4.51107311e-03f, 2.62686741e-03f, 1.04691358e-01f, 1.05296097e-04f, 4.01319363e-01f, 7.17612677e-01f, 2.40191026e-01f },
        { 1.00000000e+00f, 8.30780596e-03f, 0.00000000e+00f, 1.56133437e-01f, 4.44482741e-02f, 0.00000000e+00f, 0.00000000e+00f },
        { 1.16042556e-01f, 7.93086902e-02f, 6.31495147e-01f, 6.08362269e-03f, 1.45752591e-01f, 2.37518650e-01f, 5.62261641e-01f },
        { 6.27204726e+00f, 5.94841641e+00f, 2.91503762e-01f, 6.30402647e-01f, 9.45474131e-02f, 0.00000000e+00f, 4.99511253e-02f },
        { 1.05296097e-04f, 2.37075839e-01f, 3.14293981e-02f, 0.00000000e+00f, 2.15363503e-01f, 2.31129766e-01f, 1.52606456e-02f },
        { 2.72866451e-01f, 2.50000000e-01f, 5.27715860e-01f, 3.35494995e-01f, 6.48081408e-02f, 8.19340639e-02f, 5.24355258e+00f },
        { 8.99784433e-01f, 1.91088325e-01f, 3.51205195e-01f, 2.37920766e-01f, 4.01319363e-01f, 1.12821614e-01f, 1.10333558e-01f },
        { 2.78821044e-01f, 1.73078306e-01f, 1.39736263e-01f, 6.16453615e-01f, 5.56667197e-01f, 0.00000000e+00f, 3.38872637e-01f },
        { 8.99784433e-01f, 5.91622559e-01f, 5.07989849e-02f, 4.51107311e-03f, 1.41472816e-01f, 3.00722655e+00f, 1.59569405e-01f },
        { 1.04134195e-01f, 3.19970345e-01f, 3.00324973e-01f, 6.08682874e-01f, 2.41180314e-02f, 2.12765957e-02f, 1.42302758e-01f },
        { 1.86372259e-01f, 2.35408122e-01f, 1.02041694e-01f, 3.91646858e+00f, 2.37245728e-01f, 2.74225040e-01f, 1.10847025e-01f },
        { 3.55578004e-02f, 2.18733064e-02f, 3.87248159e-01f, 7.61382452e-02f, 4.37500000e-01f, 7.39227084e-02f, 4.79019129e-01f },
        { 4.99511253e-02f, 2.58816057e-01f, 4.23426081e-01f, 2.12765957e-02f, 6.47770921e-01f, 4.02951018e-01f, 2.17813393e-02f },
        { 9.81136834e-03f, 4.00425255e-02f, 1.02599126e-02f, 1.02599126e-02f, 3.73967795e-01f, 2.03509659e-01f, 3.47862269e-01f },
        { 5.42220845e+00f, 1.97657384e-01f, 3.51205195e-01f, 9.29238171e-02f, 2.37075839e-01f, 5.10352990e-01f, 1.07572646e-01f },
        { 2.17813393e-02f, 1.24996543e-01f, 1.24996543e-01f, 1.02599126e-02f, 6.53392342e-01f, 4.09792664e-01f, 2.15363503e-01f },
        { 3.76785400e+00f, 8.27482123e-01f, 1.86618674e-02f, 3.47862269e-01f, 2.58932688e-01f, 1.73064291e-01f, 4.69687371e-02f },
        { 3.35494995e-01f, 6.47770921e-01f, 4.25531915e-02f, 3.13102986e+00f, 2.04403902e-01f, 7.61664733e-02f, 8.07578250e-01f },
        { 2.28279175e-01f, 6.27204726e+00f, 3.73967795e-01f, 1.97790101e-01f, 2.15848997e-02f, 3.38872637e-01f, 0.00000000e+00f },
        { 3.11113237e-01f, 7.61382452e-02f, 9.93747138e-02f, 3.76785400e+00f, 2.16184779e-01f, 4.46373731e-01f, 4.20242421e-01f },
        { 4.05461890e-01f, 1.86372259e-01f, 1.63048254e-01f, 5.08736477e+00f, 3.30343364e-04f, 7.39227084e-02f, 1.04134195e-01f },
        { 9.67341043e-03f, 7.61382452e-02f, 4.07269911e-01f, 4.51107311e-03f, 7.92058433e-02f, 5.80549685e-01f, 2.39166667e-01f },
        { 5.75556420e-01f, 5.46841557e-01f, 1.73064291e-01f, 1.51587396e-02f, 2.15363503e-01f, 4.45921061e-01f, 3.91646858e+00f },
        { 4.59422402e-01f, 3.74271559e-01f, 4.01676029e-02f, 5.12037033e-01f, 3.28331970e-01f, 5.34556384e-01f, 3.49344565e+00f },
        { 3.17844391e-01f, 1.97657384e-01f, 3.65604412e-01f, 4.73524167e-01f, 2.37518650e-01f, 4.93204981e-01f, 1.34181406e-01f },
        { 8.88512973e-01f, 1.19384398e-01f, 2.35408122e-01f, 3.25238639e+00f, 4.99511253e-02f, 0.00000000e+00f, 3.07348216e-01f },
        { 3.91646858e+00f, 1.73078306e-01f, 4.22445305e-02f, 2.03509659e-01f, 6.69452643e-01f, 3.40840199e-02f, 6.05195155e-02f },
        { 5.91622559e-01f, 2.15363503e-01f, 7.67568454e-02f, 2.84861373e-01f, 2.87980437e-01f, 0.00000000e+00f, 1.24996543e-01f },
        { 2.28263583e-01f, 2.28279175e-01f, 6.48081408e-02f, 4.71570748e+00f, 8.23790361e-01f, 0.00000000e+00f, 4.40243553e-01f },
        { 8.88512973e-01f, 5.23122382e-01f, 7.03776698e-01f, 4.20242421e-01f, 5.27715860e-01f, 0.00000000e+00f, 1.81123927e-01f },
        { 2.91013718e-02f, 1.19384398e-01f, 1.24332202e-01f, 2.91408145e-02f, 5.40438754e-01f, 4.65182653e-01f, 1.37865925e-01f },
        { 1.22617022e-01f, 1.66825018e-01f, 9.29238171e-02f, 4.01676029e-02f, 4.59422402e-01f, 2.58932688e-01f, 4.93386620e-01f },
        { 1.19384398e-01f, 6.91933593e-02f, 9.83702534e-02f, 9.43976433e-02f, 9.45474131e-02f, 1.81123927e-01f, 2.15363503e-01f },
        { 2.07355783e-01f, 3.79015782e-02f, 4.04397337e+00f, 3.96543071e-01f, 5.81162234e-01f, 8.26976774e-02f, 4.25818187e-01f },
        { 6.32218364e-01f, 1.35557485e-01f, 3.51205195e-01f, 2.37920766e-01f, 1.25656780e-01f, 7.07567859e-01f, 5.08736112e-01f },
        { 2.84861373e-01f, 2.04403902e-01f, 1.87918216e-01f, 3.79015782e-02f, 1.24996543e-01f, 4.53521502e-02f, 2.68685192e-01f },
        { 1.45871006e-01f, 5.42220845e+00f, 7.07567859e-01f, 6.30402647e-01f, 3.47550299e-01f, 3.59220336e-01f, 1.97657384e-01f },
        { 9.81136834e-03f, 1.42302758e-01f, 1.24996543e-01f, 4.51107311e-03f, 3.73967795e-01f, 1.16042556e-01f, 3.59220336e-01f },
        { 2.22238179e-01f, 4.19982322e-01f, 5.78841603e-01f, 5.00000000e-01f, 2.83434241e+00f, 3.80353325e-01f, 1.22617022e-01f },
        { 4.20242421e-01f, 3.11113237e-01f, 3.80472285e-01f, 3.40840199e-02f, 4.01676029e-02f, 4.31939169e+00f, 4.98356736e-02f },
        { 4.55558447e-01f, 7.22001320e-01f, 1.51450602e-02f, 4.51681154e-01f, 3.87248159e-01f, 1.10333558e-01f, 5.99895907e-02f },
        { 2.72715400e-01f, 9.16707678e-02f, 1.73078306e-01f, 1.22743268e-01f, 1.24474685e-01f, 3.79015782e-02f, 4.22445305e-02f },
        { 7.01318023e+00f, 6.27204726e+00f, 1.55792384e-01f, 1.83409739e-01f, 5.04010599e-01f, 2.28279175e-01f, 1.12821614e-01f },
        { 6.16666667e-01f, 3.10637406e-01f, 6.03745111e-01f, 6.48081408e-02f, 7.39227084e-02f, 1.24996543e-01f, 5.93269823e-02f },
        { 4.52567064e-01f, 3.55672926e-01f, 8.48016517e-02f, 4.44974194e+00f, 6.53874878e-01f, 3.25238639e+00f, 5.56667197e-01f },
        { 8.48016517e-02f, 4.52567064e-01f, 3.35494995e-01f, 1.01500707e-01f, 6.58849996e-02f, 2.58816057e-01f, 1.16250271e-01f },
        { 8.19340639e-02f, 6.30402647e-01f, 1.73064291e-01f, 2.68685192e-01f, 0.00000000e+00f, 7.03776698e-01f, 9.29238171e-02f },
        { 8.62297994e-01f, 1.07494800e-01f, 6.21624525e-01f, 3.77176518e-01f, 3.94425820e-01f, 0.00000000e+00f, 4.08324829e-01f },
        { 3.38872637e-01f, 9.43976433e-02f, 2.87980437e-01f, 3.25238639e+00f, 3.37223068e+00f, 9.54427083e-02f, 1.45871006e-01f },
        { 2.50000000e-01f, 8.06933343e-02f, 5.07989849e-02f, 5.40438754e-01f, 3.55578004e-02f, 7.67568454e-02f, 8.19340639e-02f },
        { 3.13102986e+00f, 4.54152471e-01f, 1.64833143e-01f, 4.25531915e-02f, 5.33790060e-01f, 7.89725885e-02f, 1.41472816e-01f },
        { 2.76418549e-04f, 1.02599126e-02f, 2.50000000e-01f, 0.00000000e+00f, 5.56667197e-01f, 5.78841603e-01f, 2.16184779e-01f },
        { 4.67083092e-01f, 1.12821614e-01f, 2.82324350e-01f, 7.61382452e-02f, 2.12765957e-02f, 2.58816057e-01f, 4.93880286e-01f },
        { 1.00000000e+00f, 1.07494800e-01f, 0.00000000e+00f, 6.97127439e-02f, 5.42220845e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { 7.37999948e-02f, 5.80575591e+00f, 5.15700453e-01f, 0.00000000e+00f, 6.27204726e+00f, 9.81136834e-03f, 3.05376142e-01f },
        { 4.65182653e-01f, 4.20242421e-01f, 3.91646858e+00f, 1.86372259e-01f, 5.35578923e-03f, 5.91395256e-01f, 4.07269911e-01f },
        { 7.37999948e-02f, 5.60648320e+00f, 4.93204981e-01f, 1.00000000e+00f, 6.27204726e+00f, 3.09487315e-01f, 4.53521502e-02f },
        { 1.00000000e+00f, 8.67139443e-01f, 0.00000000e+00f, 5.27715860e-01f, 4.43587164e-02f, 0.00000000e+00f, 0.00000000e+00f },
        { 1.13475952e+00f, 3.13102986e+00f, 6.86242327e-02f, 4.19982322e-01f, 1.64833143e-01f, 5.80575591e+00f, 1.37865925e-01f },
        { 1.57260091e-02f, 3.49344565e+00f, 5.89536436e-01f, 1.16042556e-01f, 3.65604412e-01f, 5.64705252e-01f, 4.46373731e-01f },
        { 5.14031621e-01f, 1.04691358e-01f, 3.13102986e+00f, 2.37920766e-01f, 5.78841603e-01f, 4.30439277e-01f, 7.61382452e-02f },
        { 4.26960979e-01f, 1.06117499e-01f, 2.91408145e-02f, 4.01319363e-01f, 1.41472816e-01f, 6.58364357e-01f, 2.15363503e-01f },
        { 8.78846524e-02f, 1.01500707e-01f, 5.00000000e-01f, 3.11203504e-01f, 2.69234183e-01f, 2.41180314e-02f, 3.33857518e-01f },
        { 3.55578004e-02f, 4.59422402e-01f, 3.87248159e-01f, 2.68685192e-01f, 4.93849709e+00f, 3.37223068e+00f, 4.79019129e-01f },
        { 4.99511253e-02f, 5.33790060e-01f, 6.23041595e-02f, 1.69077932e-01f, 3.94425820e-01f, 5.20720273e-01f, 6.91933593e-02f },
        { 3.44631486e-01f, 2.99446163e-01f, 5.12037033e-01f, 1.99074074e-02f, 2.78821044e-01f, 1.86618674e-02f, 4.69687371e-02f },
    };
    static constexpr float kModelLeaf[kModelTrees][8] = {
        { -5.56211747e-01f, 1.43936382e+00f, 3.05630027e+00f, -3.88349515e-02f, 1.79310345e+00f, 3.45035294e+00f, -5.71428571e-01f, 1.90393013e+00f },
        { -1.10381078e-01f, -1.05071915e+00f, -7.48959778e-01f, -1.20902638e+00f, -9.20273349e-01f, 6.21359223e-01f, 3.14893617e+00f, 0.00000000e+00f },
        { 2.39744000e+00f, 4.66595970e-02f, -1.31243879e+00f, 0.00000000e+00f, -1.25660476e+00f, 2.92682927e-01f, 1.02050861e+00f, -8.26229508e-01f },
        { -5.05928854e-01f, -1.17819032e+00f, -2.21970555e-01f, 3.31598513e+00f, 3.96014944e+00f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { -4.54157434e-01f, 1.58519756e+00f, 2.36379869e+00f, 3.90899021e-01f, 4.66207862e-01f, -1.01269858e+00f, 2.11409469e+00f, 1.96838535e-01f },
        { -7.73161331e-01f, 1.02100994e+00f, -3.45785888e-01f, -1.14231763e+00f, 1.33772653e+00f, -7.33350225e-01f, -8.92484888e-01f, -1.16821509e+00f },
        { 1.63294141e+00f, 2.22505448e-01f, -1.21601110e+00f, 0.00000000e+00f, -6.22029055e-01f, -1.19587110e+00f, -5.86595996e-01f, 1.39398646e+00f },
        { -4.52471584e-01f, -1.11805936e+00f, -7.79563563e-01f, 1.60908666e+00f, 2.44569344e+00f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { -5.76958692e-01f, 1.26999824e+00f, -1.44295123e-01f, 2.34893272e+00f, 7.22810570e-01f, 1.70676586e+00f, 1.94435599e+00f, -3.74901065e-01f },
        { 6.41463802e-02f, -8.70337268e-01f, -1.03060419e+00f, -2.46590607e-01f, -3.89591681e-01f, 1.10713288e+00f, 8.80438975e-02f, 2.59682285e+00f },
        { 1.14173631e+00f, -6.57641884e-01f, -1.15769315e+00f, 0.00000000e+00f, -1.00293574e+00f, 4.43674259e-01f, -1.16059992e+00f, -5.35441115e-01f },
        { -3.82310984e-01f, -1.05977549e+00f, 1.27694092e+00f, -7.49289847e-01f, 1.88945165e+00f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { -5.46568269e-01f, 1.12711097e+00f, 1.13956801e+00f, -7.89564245e-01f, -7.39780342e-01f, 1.27487935e+00f, -3.96941776e-01f, 1.50863380e+00f },
        { 4.32326092e-01f, -6.39310102e-01f, -1.00047057e+00f, -1.46335048e-01f, -1.11079742e-01f, -9.38193024e-01f, 1.25868885e+00f, -6.70229129e-01f },
        { 1.00992781e+00f, 1.78507232e-01f, -1.09388262e+00f, 0.00000000e+00f, -6.43860102e-01f, -1.15128880e+00f, 9.42018075e-01f, -9.94876232e-01f },
        { 3.83977275e-01f, -7.92272736e-01f, -7.72376033e-01f, 1.00525099e+00f, 1.59762640e+00f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { -1.11336453e+00f, 2.25997857e+00f, -4.79570498e-01f, 4.10987048e-01f, -1.23571481e+00f, -1.49899812e-01f, 1.28533735e+00f, -1.11477926e+00f },
        { -9.16409993e-01f, 8.74095637e-02f, -4.89111631e-01f, -9.36500519e-01f, 2.39006999e-01f, -5.69460727e-01f, 2.33593785e-01f, 1.95209721e+00f },
        { 9.79752138e-01f, 1.63801847e-01f, -3.24519546e-02f, -1.00747230e+00f, 1.65316511e-01f, 2.14303538e+00f, -1.31237842e-01f, -1.08500689e+00f },
        { -1.88333591e-01f, -9.18229176e-01f, 1.00948228e+00f, -6.61088203e-01f, 1.42720088e+00f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { -2.33385527e-01f, 9.49618198e-01f, -9.98602361e-01f, -2.88075858e-01f, 1.07453075e+00f, 3.21338662e-01f, -6.88373133e-02f, 9.09457807e-01f },
        { 3.82267547e-01f, -5.72729176e-01f, -9.35771782e-01f, -1.61378215e-01f, -7.52801234e-01f, 2.04323724e-01f, 1.12837719e+00f, -5.61440567e-01f },
        { 6.87161217e-01f, -4.17086701e-01f, -1.04932998e+00f, 0.00000000e+00f, 9.70309876e-01f, -4.65523251e-01f, -1.02445280e+00f, -1.86294158e-01f },
        { -9.91028726e-01f, 1.67915015e+00f, 1.58534804e-01f, -9.55889663e-01f, 1.31640796e+00f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { -3.28456700e-01f, 5.24684522e-01f, -8.92890408e-02f, 1.55356864e+00f, -1.21478988e+00f, 2.59822558e-02f, 9.47879042e-01f, -1.03501273e+00f },
        { -6.50641451e-01f, 7.30746032e-01f, -1.09616901e+00f, -5.28883247e-01f, -5.89036915e-01f, 1.46727668e+00f, -3.84340562e-01f, -9.14073792e-01f },
        { -7.72176871e-02f, -1.02027366e+00f, 6.38696795e-01f, -1.87261737e-01f, -2.76286840e-01f, 1.01897054e+00f, 7.97813738e-01f, -1.08248077e+00f },
        { -8.98802778e-01f, 1.29897372e+00f, -2.32819921e-01f, 5.31352340e-01f, 1.23551247e+00f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { -3.49003111e-01f, 1.17103763e+00f, 1.23245474e+00f, -6.65536579e-01f, -4.42241237e-01f, 2.31046426e-01f, -1.07649860e-01f, 6.53090705e-01f },
        { -1.07744224e+00f, -4.19554839e-01f, -1.67068267e-01f, 2.86824676e+00f, -8.60210785e-01f, -1.61390374e-02f, 7.95517680e-01f, -1.88505687e-01f },
        { 3.38102824e-01f, -3.64227668e-01f, 1.47797505e+00f, -6.46049290e-01f, -4.12696907e-01f, 8.99349275e-01f, -2.04373463e-01f, -1.06395439e+00f },
        { -8.34182397e-01f, 1.81455726e+00f, 1.67441325e-01f, -8.42621456e-01f, 1.18158411e+00f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { -9.96511236e-01f, 1.81999886e+00f, 5.22672501e-01f, -2.78944130e-01f, -3.21596900e-01f, -1.22209407e+00f, 9.82369665e-01f, 1.18499015e-01f },
        { -1.07782871e+00f, -6.47535636e-01f, 9.93914469e-01f, -6.60949916e-01f, -3.52453196e-01f, -1.07884811e+00f, -7.92706473e-02f, 8.32115604e-01f },
        { -2.30010882e-02f, -1.06011751e+00f, 4.83888084e-01f, -6.29986580e-01f, -2.53011273e-01f, 8.25873337e-01f, -1.27522773e+00f, 2.13937098e-02f },
        { -8.64036625e-01f, 8.52717438e-01f, 9.76634442e-02f, -1.03231641e+00f, 1.14546159e+00f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { -3.81366971e-01f, 1.01659591e+00f, 8.65335622e-01f, 4.53748349e-02f, -4.55903404e-01f, -1.18504445e+00f, 9.05398259e-01f, 0.00000000e+00f },
        { -5.16669711e-01f, 6.39914566e-01f, -3.09704504e-01f, -1.03865302e+00f, -5.24950933e-01f, 1.23171495e+00f, 3.60054215e-02f, -7.73628903e-01f },
        { -9.80577901e-03f, -9.89732855e-01f, 8.11753871e-01f, 1.28769285e-01f, 5.75516014e-01f, -9.11614732e-01f, 9.87143604e-02f, -1.09435622e+00f },
        { 1.43207077e+00f, -9.24424094e-01f, -8.37182175e-01f, -5.36110524e-02f, 1.11442995e+00f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { -4.20933258e-01f, 7.72884718e-01f, 9.34505577e-01f, -5.99692415e-01f, 5.07665906e-01f, -3.56875360e-01f, 4.61928694e-01f, -7.58123899e-01f },
        { 4.44575980e-01f, -3.59803202e-01f, -7.60399470e-01f, 5.28735474e-01f, 1.33457895e+00f, -5.31578555e-01f, -5.31194159e-01f, 1.68313810e+00f },
        { 8.05513661e-02f, -8.67409585e-01f, -5.04195531e-01f, 4.78463307e-01f, -2.14634448e-01f, 1.34621357e+00f, -3.77805917e-01f, -1.04124340e+00f },
        { -3.29021039e-01f, 1.60311470e+00f, 2.90327478e-01f, -4.64360612e-01f, 1.09109394e+00f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { -1.11330840e+00f, 0.00000000e+00f, 1.90891220e+00f, 4.83221665e-01f, 1.57380257e-01f, 2.57308874e+00f, -3.09678689e-01f, 2.61867250e-01f },
        { -1.00763176e+00f, 0.00000000e+00f, 9.33901032e-01f, -9.00427713e-01f, -3.22985887e-01f, -1.01344842e+00f, 6.68402543e-01f, -1.57983630e-01f },
        { -4.34780353e-01f, 3.35103731e-01f, -1.26881671e-01f, -8.49153654e-01f, 1.26994045e+00f, -6.57716171e-01f, -7.14284493e-01f, 0.00000000e+00f },
        { 5.08020151e-01f, -9.62344561e-01f, 7.33443767e-02f, -1.05014687e+00f, 1.06903106e+00f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { 4.46622251e-01f, -2.08163542e-01f, -3.37826832e-01f, 1.00275376e+00f, 1.38246403e+00f, -2.00170703e-01f, -6.28873660e-01f, 0.00000000e+00f },
        { -5.99153412e-01f, -1.02901201e+00f, 8.02899903e-01f, -7.56496821e-01f, 1.68967529e-01f, -5.71698332e-01f, 1.60782754e+00f, 7.82698337e-02f },
        { 1.50597102e-01f, -3.91784140e-01f, 1.04538519e+00f, -8.80572936e-01f, 2.84369781e-01f, -6.89402334e-01f, -4.53050874e-01f, -1.22579540e+00f },
        { -2.50315532e-01f, 4.43167782e-01f, -4.14923532e-01f, -1.08991743e+00f, 1.05300212e+00f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { 8.38783223e-03f, 8.50488559e-01f, 3.85462166e-01f, 1.95490809e+00f, -1.62040429e-01f, -1.10222194e+00f, 8.66811950e-01f, 0.00000000e+00f },
        { -5.42421181e-01f, -1.05299628e+00f, 1.19955494e+00f, -6.74144015e-01f, 1.37094330e+00f, -6.90744493e-02f, 5.95607070e-01f, -4.66985058e-01f },
        { 1.86853776e-01f, 8.98007797e-01f, 1.01983391e-01f, -1.30773738e+00f, 3.71573956e-01f, -3.35170242e-01f, 1.71387119e-01f, -1.01998514e+00f },
        { 3.10328130e-01f, -9.25440001e-01f, 4.56629296e-01f, -2.08411545e-01f, 1.03445737e+00f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { 8.43204799e-01f, -2.18381869e-01f, 1.95581015e+00f, 0.00000000e+00f, -4.60244378e-01f, -1.06420120e-02f, 1.07782576e+00f, -3.63959613e-02f },
        { -1.07272733e+00f, 0.00000000e+00f, 1.26837365e-01f, -5.80093034e-01f, 1.62122795e-01f, -1.00967740e+00f, 4.28859892e-01f, -9.62504002e-01f },
        { -2.01482730e-01f, 2.67502934e-01f, -1.10822892e+00f, -1.32393088e-01f, 1.16119669e+00f, -6.09801826e-01f, -6.67677725e-01f, 0.00000000e+00f },
        { -7.39162142e-01f, 7.67178852e-01f, 2.47076302e-01f, -5.60432989e-01f, 1.02105325e+00f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { -1.18618554e+00f, 0.00000000e+00f, -1.04247455e-01f, 0.00000000e+00f, -6.71039193e-01f, 1.10678814e+00f, -8.84420936e-01f, 8.24542252e-02f },
        { -9.66079212e-01f, 1.24435426e-01f, 8.88662489e-01f, -6.04019146e-01f, -9.56973972e-01f, 3.58837215e-01f, -9.13129471e-02f, 7.56618187e-01f },
        { 7.67082392e-01f, -2.46209404e-02f, 2.95046500e-01f, -7.39723839e-01f, 2.58112009e-01f, -6.73659018e-01f, 2.59694597e-02f, -9.13409379e-01f },
        { -2.07320801e-01f, 3.59070139e-01f, -1.09464054e+00f, -2.81071958e-01f, 1.00594283e+00f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { -1.89712710e-02f, -9.71066271e-01f, -5.36179034e-01f, 3.26291556e-01f, -1.29482822e-01f, 5.26437050e-01f, 4.12351497e-01f, 1.52584732e+00f },
        { -6.70444871e-01f, 3.05395751e-01f, 5.64097434e-01f, -7.08695137e-02f, -1.02686293e+00f, 0.00000000e+00f, -5.95891377e-01f, 4.17203631e-01f },
        { 6.98115125e-01f, -5.17716069e-01f, 2.59145141e-01f, -4.16699205e-01f, -1.09718771e+00f, -2.38637426e-02f, 7.18529918e-01f, -6.03877363e-01f },
        { 7.15170034e-01f, 3.34561166e+00f, -3.19494554e-01f, 3.07427888e-01f, 3.53337997e-01f, -7.11423450e-01f, -1.05276404e+00f, 0.00000000e+00f },
        { -8.80619106e-01f, 0.00000000e+00f, 1.26727576e+00f, 2.16549710e-01f, -1.87974363e-01f, 1.27505705e+00f, -2.99304371e-01f, 1.18460046e-01f },
        { -1.01703018e+00f, -7.63686786e-03f, 3.41775982e-01f, -1.42628628e-01f, -1.01615533e+00f, -4.26190395e-02f, 2.80942212e-01f, 0.00000000e+00f },
        { 1.51162360e-01f, -5.43096690e-01f, 3.55928168e-01f, -2.01747374e-01f, 4.18743105e-01f, -5.18142546e-01f, -1.55379304e+00f, -5.18367284e-01f },
        { -1.08773480e-01f, 1.59878469e+00f, 1.90093048e-01f, -6.62402678e-01f, -6.56477169e-01f, 5.98396550e-01f, -1.04590991e+00f, -3.09274287e-01f },
        { 3.50281493e-01f, -1.58743990e-01f, 9.82040931e-01f, -4.17008643e-02f, -3.96696440e-01f, 0.00000000e+00f, 1.16717780e+00f, 2.82654123e-01f },
        { 2.58458539e-03f, 1.49223613e+00f, -1.00850569e+00f, -1.13534825e-01f, -4.39901731e-01f, -1.05644959e+00f, 6.11120098e-01f, 0.00000000e+00f },
        { 1.75245467e-01f, -6.45760314e-01f, -1.01622110e+00f, -2.42274480e-02f, 2.74623820e-01f, -3.82443402e-01f, 1.32112325e-01f, -8.76175122e-01f },
        { 8.93063237e-01f, -9.67887070e-01f, 3.11295187e-01f, -3.07114370e-01f, 9.91728485e-01f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { -1.06847716e-01f, 1.02082077e+00f, 8.52950124e-01f, -2.58553857e-01f, -1.15123086e+00f, -4.05514467e-01f, 7.21594759e-01f, 0.00000000e+00f },
        { -6.52355689e-01f, 2.97570385e-01f, 4.81294293e-01f, -8.66432076e-02f, 5.99567448e-02f, 0.00000000e+00f, 2.59668082e-01f, -9.99237656e-01f },
        { 9.64412198e-02f, -3.95717245e-01f, 4.10568085e-01f, -9.70100271e-01f, 1.10553741e+00f, -5.05846268e-01f, -5.86812273e-01f, 0.00000000e+00f },
        { 4.15516941e-01f, -8.84365845e-01f, 3.12982877e-01f, -1.69255076e-01f, 9.79337970e-01f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { -1.14721492e+00f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f, 1.99028128e+00f, 0.00000000e+00f, -1.67687165e-01f, 1.48339644e-01f },
        { 9.78580890e-01f, -7.26384700e-01f, 2.06492573e+00f, 4.22526179e-01f, 2.25637560e-01f, -3.30852605e-01f, -8.72013892e-01f, 7.53179567e-01f },
        { 3.96691677e-01f, -1.15413004e+00f, 1.35480419e+00f, 3.04996331e-01f, 3.22033142e-01f, -3.36544978e-01f, 4.91430998e-01f, -1.07068835e+00f },
        { -7.67864472e-01f, 2.18268433e-01f, 9.74887366e-01f, -4.63085803e-02f, 9.64051923e-01f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { -1.38028253e-01f, 1.59807193e+00f, 1.39793807e+00f, -2.90592347e-01f, -1.02733315e-01f, -1.17277938e+00f, 7.94660476e-02f, 6.62053054e-01f },
        { 3.35033549e-01f, -8.98650063e-01f, -7.98392959e-01f, 3.57921061e-01f, 3.12938279e-01f, -6.99712406e-01f, 1.29314888e+00f, -9.70634440e-01f },
        { 9.56773208e-01f, -1.58418457e-01f, 2.17836446e-01f, -5.65812190e-01f, 1.27154560e-01f, -8.62589885e-01f, 6.74020970e-01f, -1.05666619e+00f },
        { 3.86319199e-01f, -1.98295568e-01f, -7.96977648e-01f, 1.17251866e+00f, -5.96070025e-01f, 7.34233702e-01f, -1.01199638e+00f, 1.23834776e-01f },
        { -1.08405465e+00f, 0.00000000e+00f, -2.68198772e-01f, 0.00000000e+00f, -3.00575880e-01f, 1.00397560e+00f, -7.05610142e-01f, 4.73749125e-02f },
        { 6.37428093e-02f, -6.84464804e-01f, -1.05251547e+00f, -2.92397969e-01f, -9.07640308e-01f, 1.05299987e-01f, 8.28576589e-01f, 3.59906998e+00f },
        { 1.61057864e-01f, -6.52770555e-01f, -9.64674309e-01f, -7.62437732e-02f, 1.03704821e+00f, -3.79567010e-01f, -5.45280152e-01f, 0.00000000e+00f },
        { 8.17660620e-01f, -6.29756549e-02f, -7.41768761e-01f, 4.58508456e-01f, -1.01886760e+00f, 0.00000000e+00f, -7.88894038e-01f, 4.91501066e-01f },
        { -1.19461893e-01f, -1.11343341e+00f, 9.74776829e-01f, 2.00924160e-01f, -8.41908214e-01f, 1.33154479e+00f, 7.62266154e-01f, -9.41378348e-02f },
        { 8.69297684e-01f, -3.08321136e-01f, -9.87765591e-01f, -2.26614021e-01f, -8.93975649e-01f, 1.27278031e-01f, 1.67601998e+00f, -6.77827933e-01f },
        { -7.06128135e-01f, 4.75054195e-01f, 1.30278592e+00f, 0.00000000e+00f, -2.60481239e-01f, 8.06720060e-01f, 2.35372259e-02f, -7.92577317e-01f },
        { -1.03799400e-02f, 5.26804799e-01f, -1.16532780e+00f, -1.61582485e-01f, -6.82435205e-01f, 9.66293416e-01f, -1.68087685e-01f, -1.02540297e+00f },
        { -1.05610413e+00f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f, 1.52393739e+00f, 0.00000000e+00f, -9.17764535e-03f, 6.24409826e-01f },
        { -9.47009867e-01f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f, 6.64686354e-01f, -7.64531257e-01f, 8.90742227e-02f, -8.02160266e-01f },
        { 2.26610664e-01f, -1.08499916e+00f, -2.75268035e-01f, 3.33012769e-01f, -2.67359535e-01f, -1.08479756e+00f, 5.75081292e-01f, 0.00000000e+00f },
        { -1.42324265e-01f, -9.11183715e-01f, 4.59734624e-01f, -1.15332299e-01f, 9.47107159e-01f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { -7.08791875e-02f, 1.22180265e+00f, 1.85405455e+00f, 7.47940359e-01f, -2.64554798e-01f, 3.25136980e-01f, -5.30993572e-01f, 2.72294350e-01f },
        { 3.59742928e-01f, -2.11624732e-01f, -8.42837582e-01f, 1.02724082e+00f, -9.64903349e-01f, 0.00000000e+00f, 4.51296522e-01f, -3.95569049e-01f },
        { 3.08860762e-01f, -8.45450125e-02f, -8.06010621e-01f, 8.13398912e-01f, -4.29762093e-01f, -1.59986765e+00f, 4.14847370e-01f, -3.98552586e-01f },
        { 2.23789362e-01f, -2.58805267e-01f, -9.83100298e-01f, 1.77473372e-01f, 9.32007530e-01f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { -8.63108334e-01f, 5.27093696e-01f, -1.10555908e+00f, -2.69029554e-02f, -6.00287110e-02f, 1.05157700e+00f, -7.07573384e-01f, 6.72458352e-02f },
        { 2.31851841e-01f, 2.00583967e+00f, -9.06592641e-01f, 4.37906974e-01f, -6.88725155e-01f, -5.26134384e-02f, 7.86309271e-01f, -6.06848165e-01f },
        { -5.19901515e-02f, 1.19512107e+00f, -1.48658227e-01f, 0.00000000e+00f, 6.51876906e-01f, -6.14848686e-02f, -1.16109023e+00f, 4.17500409e-02f },
        { 2.06517637e-01f, -3.74838425e-01f, -3.61470840e-01f, 7.11660127e-01f, -3.53868671e-01f, 0.00000000e+00f, -1.13869667e+00f, 0.00000000e+00f },
        { 1.91303866e+00f, -2.66570926e-01f, -1.78629734e-01f, 3.25076021e-01f, 3.76048718e-01f, -6.42079679e-01f, 1.65328993e+00f, 0.00000000e+00f },
        { -3.29586215e-01f, 4.60896719e-01f, -9.71366582e-01f, 5.55974383e-01f, 2.77898542e-01f, -8.08456641e-01f, 1.34464604e+00f, 1.61501518e-01f },
        { -2.74165072e-01f, 1.99449852e-01f, -4.90620840e-02f, -1.09515019e+00f, 7.26553626e-01f, -1.16732987e+00f, -3.85595049e-01f, -1.21463485e+00f },
        { -3.38745345e-01f, -9.84305521e-01f, 7.26078654e-01f, 0.00000000e+00f, -7.73839123e-02f, 3.33295165e-01f, -9.58026144e-01f, 1.36150155e-01f },
        { -7.24084098e-02f, 9.39530761e-01f, 1.21027967e+00f, 1.40896381e-01f, -1.00352942e+00f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { -9.46018716e-01f, -1.30446915e-01f, 1.00766043e-01f, -8.28321172e-01f, -9.35983408e-01f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { 8.46205670e-01f, 1.58147160e-01f, -2.43514350e-01f, 7.60630695e-01f, -9.34778880e-02f, -8.18887942e-01f, -4.74532116e-02f, 2.12410437e+00f },
        { -6.83106486e-01f, 1.77236410e-01f, 8.76691977e-01f, 6.71611548e-02f, 4.10475521e-01f, -9.64815316e-01f, -6.94593266e-01f, 7.82719394e-01f },
        { -9.08370034e-02f, 1.43481584e+00f, 1.19311331e+00f, 1.19723500e-01f, -1.03170102e+00f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { 2.39492259e-01f, -6.26418449e-01f, -9.79456165e-01f, 2.74686299e-01f, 4.36737851e-01f, -7.51182458e-01f, -8.41130742e-01f, 8.32841033e-01f },
        { 5.17984688e-01f, 3.68838888e-02f, -2.62413477e-02f, -8.48566095e-01f, -6.59646081e-01f, -1.64763149e+00f, 3.02790057e-01f, -5.38981361e-01f },
        { 1.41434342e-01f, -5.24217275e-01f, 3.65873674e-01f, -1.74236380e-01f, 9.30545823e-01f, 0.00000000e+00f, -8.45273333e-01f, 4.23455741e-01f },
        { -1.02392512e+00f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f, 1.19917588e+00f, -1.58732331e-01f, -6.32195197e-01f, 5.38586229e-02f },
        { 4.01722451e-02f, -9.62208578e-01f, 6.18728987e-01f, -8.31142002e-01f, 1.47739021e-01f, 9.39321543e-01f, -8.09930354e-01f, 4.91247041e-02f },
        { -2.87370828e-01f, 8.68401188e-01f, 3.71172819e-01f, -1.39074397e-01f, -1.74370479e-01f, 1.10711309e+00f, -7.85920974e-02f, -7.99222221e-01f },
        { -1.29152874e-01f, 3.86699440e-01f, 3.38834394e-01f, -8.00018666e-01f, 9.16316164e-01f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { 1.75464763e+00f, -1.97768718e-01f, -4.18732540e-01f, 5.23633709e-01f, -9.87165929e-01f, 1.52434223e+00f, 5.94478379e-01f, -1.66909227e-01f },
        { 1.05372187e-01f, -8.33751711e-01f, -7.92781053e-02f, 7.93001587e-01f, 3.51053745e-01f, 0.00000000e+00f, -9.94174005e-01f, 9.26206812e-02f },
        { 7.59228396e-02f, -4.42598920e-01f, 3.43607335e-01f, -1.44345474e-01f, 7.07803705e-02f, -9.25624623e-01f, -1.02155717e+00f, 4.08473108e-01f },
        { 2.40344461e-01f, -7.32484548e-01f, -3.14684054e-01f, 9.06399856e-01f, -5.85186085e-01f, 1.05910606e+00f, -8.96667947e-02f, -1.00773399e+00f },
        { -4.92758644e-01f, -2.38814865e-03f, -9.47184915e-02f, 7.37425820e-01f, 5.13001911e-01f, -2.17884490e-02f, 1.33441938e+00f, -6.74053804e-01f },
        { 1.53493565e-01f, -5.80477463e-01f, 1.85229712e-01f, -9.66857353e-01f, -8.91300566e-01f, 1.13436326e-02f, 4.83712568e-01f, 0.00000000e+00f },
        { 3.33568830e-01f, -1.07412377e+00f, 1.23338837e+00f, 1.53702613e-01f, 8.65304841e-01f, -1.24176027e-01f, 9.24493751e-01f, -8.60115539e-01f },
        { -8.72624936e-01f, -1.76718463e-01f, 4.01342793e-01f, -1.49479231e-01f, 9.05210012e-01f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { -4.39286819e-01f, 9.52012422e-01f, 5.99554455e-01f, -1.91751026e-01f, 6.04864151e-01f, -1.45084492e-01f, -4.83603333e-02f, -8.88811692e-01f },
        { 5.91265580e-02f, -6.10810883e-01f, -6.73523901e-01f, 8.19765259e-01f, -9.14461803e-01f, 0.00000000e+00f, -3.48498707e-01f, 3.24264541e-01f },
        { -2.38900783e-02f, -8.98487525e-01f, 1.61449245e-01f, -2.07105942e-01f, -8.58006106e-01f, 1.62597320e-01f, 4.71798118e-01f, 0.00000000e+00f },
        { 2.09306353e-01f, -2.32890211e-01f, 1.29731904e+00f, 1.71292739e-01f, -1.02539822e+00f, 0.00000000e+00f, 3.71819948e-02f, 0.00000000e+00f },
        { -2.01923956e-02f, -6.02628441e-01f, -2.16357269e-01f, 1.65344998e+00f, 1.59825329e-01f, 1.29746582e+00f, -6.43592056e-01f, 1.24750011e-01f },
        { 4.42256738e-02f, -9.33689989e-01f, -6.35752096e-01f, 1.56879662e+00f, -9.02637476e-01f, 0.00000000e+00f, -3.09317966e-01f, 3.37136016e-01f },
        { 1.08914227e-01f, -7.00806675e-01f, 5.51038634e-01f, -1.04291947e+00f, 4.90777659e-01f, -3.11969861e-01f, -5.49198234e-02f, -9.82357723e-01f },
        { -2.81124745e-02f, -9.13104835e-01f, -4.01878964e-01f, 5.24305315e-01f, 8.94168664e-01f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { -1.44158701e-01f, 5.51674934e-01f, -3.19621385e-01f, 2.14444447e+00f, 6.40494143e-01f, -4.83050767e-01f, -8.96771204e-01f, 1.00239680e+00f },
        { -9.30876026e-01f, 0.00000000e+00f, 6.64750080e-02f, 0.00000000e+00f, 1.19008519e-02f, 5.35890865e-01f, -1.20868703e+00f, -3.98768009e-02f },
        { 3.99774732e-02f, -1.43351661e+00f, -6.41803868e-01f, 6.29106087e-01f, 9.49134586e-01f, -3.67576869e-01f, -5.40606750e-01f, 0.00000000e+00f },
        { 1.20295012e-02f, 5.15392566e-01f, -1.10134670e+00f, -7.50420146e-02f, -2.14681557e-01f, -1.01282160e+00f, 9.31659502e-01f, 0.00000000e+00f },
        { -9.38582258e-01f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f, 5.28040789e-02f, 1.27136445e+00f, 3.81107860e-01f, -7.85634204e-02f },
        { 1.32311568e-02f, 4.72339711e-01f, 4.53303363e-01f, -9.33583449e-01f, -4.62760849e-01f, 4.40161422e-01f, -1.10635437e+00f, 0.00000000e+00f },
        { -4.86959294e-02f, 6.07126097e-01f, 3.08367958e-01f, -9.97166125e-01f, 9.02272453e-01f, -3.96003718e-01f, -1.80824307e+00f, 3.31819920e-01f },
        { -2.50102835e-01f, 1.52201059e-01f, 7.84409676e-01f, -6.78145967e-01f, 2.37717833e-01f, -1.12487273e+00f, 5.04743862e-01f, 0.00000000e+00f },
        { 5.67117709e-01f, -1.25459665e-01f, 5.50579184e-01f, -3.71829943e-02f, -7.92994339e-01f, 0.00000000e+00f, 2.77169592e+00f, 5.78630165e-01f },
        { -9.06852387e-01f, 4.38966280e-02f, -5.20476140e-02f, 4.43605338e-01f, 1.26728027e+00f, -6.99763007e-01f, -8.69129391e-01f, 0.00000000e+00f },
        { -1.66603103e-01f, 1.68798634e-01f, 1.23720824e+00f, -3.76974203e-01f, -3.61094413e-01f, -1.30416844e+00f, 2.69752532e-01f, -4.70834530e-01f },
        { -2.97250813e-01f, 4.15522610e-01f, -1.31163361e+00f, 0.00000000e+00f, -7.31156907e-01f, 7.37432767e-01f, 1.50551981e+00f, -9.50093747e-03f },
        { 1.05854754e-01f, -4.38562456e-01f, 8.11427765e-01f, -4.48907718e-01f, -2.95048566e-01f, 5.67484443e-01f, 2.09213120e-01f, -4.12561248e-01f },
        { 7.94853392e-02f, -5.68722240e-01f, 2.20260457e-02f, 1.79193434e+00f, 3.82092091e-01f, 0.00000000e+00f, -9.49266526e-01f, 4.79549569e-02f },
        { -1.74900053e-01f, 1.77976669e-01f, 1.30973660e-01f, -6.85495049e-01f, -1.20363952e+00f, -1.64238258e-01f, 4.04960869e-01f, -5.44620735e-01f },
        { 4.09860654e-01f, -8.88017276e-01f, -4.25169846e-01f, 1.78261151e-01f, 8.62664622e-01f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { -9.33495367e-01f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f, 9.18209580e-01f, -2.45904877e-01f, 3.43822178e-02f, -9.61100630e-01f },
        { -9.10367092e-01f, 0.00000000e+00f, 2.70760339e-01f, 0.00000000e+00f, 3.31244361e-02f, 9.64621001e-01f, -8.58614623e-01f, 7.22680031e-02f },
        { 5.49739750e-01f, -7.14880794e-01f, 1.26259567e-01f, 1.00059849e+00f, 2.83189639e-01f, -3.44813162e-01f, -1.17059683e-01f, -8.03990753e-01f },
        { -3.13850279e-01f, 1.16719121e-01f, 1.32393345e-01f, -7.79774870e-01f, -5.91567968e-01f, 5.71108676e-01f, 1.59145122e+00f, 0.00000000e+00f },
        { 1.62548339e-01f, -4.53085466e-01f, 7.25050661e-02f, 1.24439382e+00f, -2.09718867e-01f, 5.06529592e-01f, -7.91083346e-01f, 4.03123663e-01f },
        { 5.76748233e-01f, -1.24422222e-02f, -4.07190781e-01f, 3.08383108e-01f, 4.07009167e-01f, -9.50687275e-01f, 5.62627471e-01f, 0.00000000e+00f },
        { 4.40544162e-02f, -7.20001854e-01f, -1.49548154e+00f, -4.74845903e-02f, 1.26393251e+00f, 4.35324097e-01f, -7.43885662e-01f, 3.18533122e-01f },
        { -6.24607246e-02f, 1.13086371e+00f, -8.44333377e-01f, 5.15384714e-02f, -3.83358894e-01f, 1.18242075e+00f, -9.38263619e-01f, 2.76856735e-01f },
        { -1.52787731e-02f, 1.92200467e+00f, 5.39793674e-01f, -2.11152696e-01f, 5.40409659e-01f, -2.36699235e-01f, -7.20361719e-01f, 1.76169373e-01f },
        { 1.52272284e-01f, -6.90248359e-01f, 6.30953594e-01f, -7.76644783e-01f, 4.32722816e-01f, -1.89076287e-01f, 5.01963311e-01f, 2.29158016e+00f },
        { -2.57852809e-01f, 6.22269259e-01f, 1.11926120e+00f, 2.95045853e-01f, -9.18097264e-01f, 8.47755830e-01f, 5.86723520e-02f, -3.17727715e-01f },
        { -4.11286666e-01f, 6.91560901e-01f, -2.31229175e-01f, 3.49845410e-01f, -5.77934770e-01f, 6.49167692e-01f, -7.92055024e-01f, 6.01159397e-01f },
        { -5.46411158e-02f, 1.05710342e+00f, -2.94008572e-02f, 9.90651363e-01f, 1.83958742e-01f, -6.93861429e-01f, -4.37051906e-01f, 6.25759715e-01f },
        { 1.97257318e-02f, -7.46019889e-01f, 5.67869097e-01f, -4.30307206e-01f, 2.67452949e-01f, 0.00000000e+00f, 7.72735166e-02f, -9.06313088e-01f },
        { 1.86414221e-01f, -3.02452511e-01f, 6.98203267e-01f, 1.18359525e-01f, 7.08844223e-01f, -2.93239568e-01f, -1.04250170e+00f, -5.97890533e-02f },
        { 1.96900375e-01f, -2.82162880e-01f, -5.89217782e-01f, 6.01239344e-01f, 8.62577633e-01f, -5.25442458e-01f, -1.04384266e+00f, 1.92391658e-01f },
        { -8.01794067e-02f, 1.54515901e+00f, 3.78186630e-01f, -5.82598295e-02f, -3.10719767e-01f, -9.64461099e-01f, 5.48438705e-01f, 0.00000000e+00f },
        { -3.17307495e-01f, 3.38170365e-01f, -1.02706691e+00f, -3.02560868e-01f, -6.10077908e-03f, 2.63249762e+00f, 4.55782215e-01f, -5.61912683e-01f },
        { -6.21992246e-04f, 5.22934207e-01f, 1.08553609e+00f, -5.56926934e-01f, -1.68736336e+00f, -6.38550942e-01f, -3.05270704e-01f, 2.84744980e-01f },
        { -1.60324020e-01f, 1.71915057e-01f, -1.63896571e-01f, -1.02114967e+00f, 8.47078169e-01f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { -7.80627075e-02f, 1.08715576e+00f, 9.82961288e-01f, -2.02174847e-01f, -7.23289580e-01f, 2.54778385e-01f, -1.00520753e+00f, 3.95389246e-01f },
        { -2.24649849e-01f, 1.89066099e+00f, 1.01876504e-01f, -9.81142290e-01f, -4.50155051e-01f, 5.92380805e-01f, -4.74104808e-01f, 1.40609930e-01f },
        { 3.18585131e-01f, -2.99009656e-02f, -1.26626430e-01f, -8.07806456e-01f, 1.35950714e+00f, 0.00000000e+00f, -5.49882305e-01f, 0.00000000e+00f },
        { -4.74777258e-02f, 6.39636154e-01f, 1.19556083e-01f, -7.22750063e-01f, -1.17873192e-02f, 1.29892945e+00f, -8.35472587e-01f, -5.41427597e-02f },
        { -9.85216054e-02f, 6.12022554e-01f, -3.74052261e-01f, 2.04635693e-01f, 4.32245834e-01f, 1.89339574e+00f, -5.57047126e-01f, 9.40230942e-02f },
        { 5.91547888e-02f, 1.09287388e+00f, -8.27424936e-01f, 2.00797649e-02f, -1.23819452e-03f, 0.00000000e+00f, -9.18608571e-01f, 0.00000000e+00f },
        { -1.74794592e-01f, 9.47089039e-01f, 3.46883687e-01f, -9.49212906e-01f, 2.21939660e-01f, -1.55739676e-01f, 5.34128495e-02f, -1.19790740e+00f },
        { 8.24944577e-02f, -1.94212731e-01f, -8.20072508e-01f, 3.36340337e-01f, 8.33407019e-01f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { -1.54383695e-01f, 6.50191100e-01f, 8.84672579e-01f, 3.85381019e-03f, 8.29603320e-02f, 1.27180333e+00f, 5.77295654e-01f, -1.03149660e+00f },
        { -1.37369774e-01f, -9.09724260e-01f, -8.13286427e-02f, 2.99601724e-01f, -8.40389953e-01f, 0.00000000e+00f, 1.12466195e+00f, -7.62964416e-01f },
        { -5.01005699e-02f, -9.62132557e-01f, 3.88722909e-01f, -1.64934857e-01f, -1.91555076e-01f, 2.96073590e-01f, 6.18466522e-02f, -8.65433058e-01f },
        { 1.33913972e-01f, -1.70526794e-01f, -1.90419599e-01f, -9.31178295e-01f, 2.01283743e-01f, -9.03596395e-01f, 9.86010585e-01f, -2.23972750e-01f },
        { -4.07699134e-01f, 3.37956149e-02f, 5.87282835e-01f, -2.23704600e-01f, -9.23951680e-01f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { 1.25758897e-01f, -3.03311027e-01f, -7.61191695e-02f, 6.74118551e-01f, -8.98484454e-01f, 0.00000000e+00f, 8.85867286e-02f, 0.00000000e+00f },
        { 3.16366315e-01f, -1.01206807e+00f, 1.09319115e+00f, -6.90473673e-01f, 2.56191109e-02f, 9.29769827e-01f, -1.09924992e-01f, -8.58153193e-01f },
        { 2.92543767e-01f, -3.56815848e-01f, -2.85551980e-01f, 1.10544797e+00f, 3.16934894e-01f, -7.69459193e-01f, -2.90011702e-01f, 3.43875391e-01f },
        { 9.81328367e-01f, 1.06702783e-01f, 1.03129727e+00f, -3.84049158e-02f, -9.45848015e-02f, -8.35142840e-01f, 8.73639010e-01f, -2.11547193e-01f },
        { -8.70845195e-01f, 1.37023139e-01f, -9.32777437e-02f, 3.53797968e-01f, -7.93413089e-01f, 0.00000000e+00f, 9.80776259e-01f, -5.10969220e-01f },
        { -3.33549401e-01f, 3.72295484e-01f, -5.33122838e-02f, -1.08783462e+00f, 2.12551530e-01f, -1.50296402e+00f, 5.19679104e-01f, 1.45870679e-02f },
        { 9.93160527e-03f, -3.79880589e-01f, 8.03132546e-01f, 0.00000000e+00f, -3.82238651e-01f, 4.68018964e-01f, 1.43544473e+00f, -1.65361400e-01f },
        { -6.70732749e-01f, -1.03394225e-01f, 8.43546588e-02f, 1.21675748e+00f, 1.11848337e-01f, 1.28214514e+00f, 3.09253147e-01f, -1.32193497e-01f },
        { -1.55491437e-01f, 4.89650963e-01f, -9.99813298e-01f, 5.09586283e-02f, -9.07530746e-01f, -1.81057695e-02f, -4.23029251e-01f, 1.13755543e+00f },
        { 1.84477972e-01f, -1.49623069e-01f, 1.66332196e-01f, -4.62924190e-01f, -2.34230508e-01f, -9.57237697e-01f, 2.69334058e-01f, 0.00000000e+00f },
        { -1.96547344e-01f, 7.45491306e-02f, -2.32575397e-02f, 1.36384083e+00f, -5.50120617e-01f, 9.08785785e-01f, -9.05431596e-01f, -5.23827657e-02f },
        { -6.13924275e-01f, 5.08112971e-01f, 1.69361279e-01f, -3.51457024e-01f, 6.04100454e-02f, -5.90306188e-01f, 4.58408696e-01f, -7.16407590e-01f },
        { 1.02575045e-01f, -9.82915197e-01f, 1.50721842e-01f, 9.53591433e-01f, -9.48496136e-01f, 4.77053565e-01f, -6.88752679e-01f, 1.48703584e-01f },
        { -4.34711833e-01f, 2.95069032e-02f, 1.26678839e+00f, -5.53181891e-01f, -1.12107654e+00f, 0.00000000e+00f, -9.55304809e-01f, 3.24227175e-01f },
        { 5.74744998e-01f, -3.73549392e-01f, -4.08043127e-01f, 1.14459292e+00f, -2.69113141e-01f, 1.17992736e-01f, -7.42505038e-01f, 5.11519979e-01f },
        { -2.29726577e-02f, 1.33768767e+00f, 9.88025533e-01f, 1.03087703e-01f, -9.06186574e-01f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { 1.96065690e-01f, -1.14354781e-01f, -8.11566007e-01f, 5.13815986e-01f, -1.08154911e+00f, -3.06793209e-01f, 3.27788057e-01f, -5.80514309e-01f },
        { -8.34037334e-02f, 7.97657545e-01f, -1.33510327e+00f, -3.54385607e-01f, -1.26159672e+00f, 0.00000000e+00f, 2.87856750e-01f, -3.25257480e-01f },
        { 3.44237306e-01f, -4.60766660e-01f, -5.83167311e-01f, 2.20721746e-01f, 6.99829555e-01f, -1.44781035e-01f, -3.02812484e-02f, -7.40402868e-01f },
        { -4.58033452e-01f, 1.71249488e-01f, -1.19356561e-01f, 9.51466187e-01f, 2.98736303e-01f, 1.85810866e+00f, -3.12974323e-01f, 1.20240315e-01f },
        { -1.49167721e-01f, 1.41674453e-01f, -3.40169741e-01f, 1.10000729e+00f, -5.33706115e-01f, 7.20345247e-01f, 3.18485688e-01f, -9.69914667e-01f },
        { 1.21335927e-01f, -5.04629103e-01f, 6.26763538e-02f, 9.73124252e-01f, -8.56636020e-01f, 3.11988159e-01f, 1.53822658e-02f, -6.97490676e-01f },
        { -7.13692231e-01f, 5.07615834e-01f, 1.22481820e+00f, -5.41132003e-01f, 5.82508148e-01f, -7.90138688e-01f, -1.51610991e-01f, 4.96718957e-01f },
        { 4.90100931e-01f, -2.85735318e-01f, 9.44856569e-01f, -2.15400383e-01f, -2.69422415e-01f, 9.50964178e-02f, 8.74255500e-01f, 2.81036526e-02f },
        { 8.57104550e-01f, -7.27854169e-01f, 5.02900487e-02f, -5.04083290e-01f, -7.22986601e-01f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { 6.13786665e-01f, 7.75819669e-02f, -4.55583428e-01f, 1.03795430e-01f, -1.95168889e-02f, -6.65191767e-01f, -4.03198721e-01f, 5.19599309e-01f },
        { 1.03159776e-01f, -2.32658315e-01f, -2.99300018e-01f, 6.60159055e-01f, 1.17895178e-01f, -9.77070633e-01f, -4.02148412e-01f, 2.85583143e-01f },
        { 1.72932050e-01f, -2.92708790e-01f, 3.83678082e-01f, -1.12854611e-01f, -2.93953583e-01f, 1.77997356e-01f, 9.30010613e-01f, -1.46038848e-01f },
        { -5.11190065e-02f, 3.61081977e-01f, -4.30508958e-01f, 1.47917434e+00f, -8.12858153e-01f, 0.00000000e+00f, -2.64180045e-01f, 2.63548368e-01f },
        { 7.67870686e-01f, 3.92235695e-02f, -2.71007200e-01f, 3.66620003e-01f, -1.08888046e-01f, 4.23509800e-01f, -8.98832856e-01f, 4.49896644e-01f },
        { 3.02746843e-02f, -7.64119497e-01f, -3.15517672e-01f, 1.31925951e+00f, 8.12491819e-01f, -9.51945585e-01f, -2.33274806e-01f, 2.09887721e-01f },
        { -6.76467368e-02f, 1.22456265e+00f, -6.67548302e-02f, 5.94825066e-01f, -1.28538194e-01f, -8.97603245e-01f, 4.65623020e-01f, 0.00000000e+00f },
        { 2.54531450e-01f, -5.39044442e-01f, -7.20212679e-01f, 3.32462135e-01f, -1.23709495e+00f, -7.44628998e-02f, 5.01402576e-01f, -2.89864913e-01f },
        { -4.49828829e-01f, -1.04470454e+00f, 5.15421645e-01f, -7.82660410e-01f, 6.64358989e-01f, 8.62086379e-02f, 5.23888925e-01f, -6.59625590e-02f },
        { -2.05095855e-01f, 1.47324598e-01f, 4.47058999e-01f, -8.52416615e-01f, 7.63445750e-01f, 0.00000000e+00f, 6.67097341e-02f, -8.22688359e-01f },
        { -8.63902660e-01f, 0.00000000e+00f, 8.86719969e-01f, -2.01438332e-02f, 6.35318485e-01f, -3.44020776e-02f, -9.55889090e-01f, 5.04362923e-02f },
        { 4.71846065e-01f, -8.73671031e-02f, -8.23977291e-01f, 1.31171888e-01f, 1.05284463e-01f, 1.91911962e+00f, 3.78524832e-01f, -5.57959269e-01f },
        { 5.38095387e-01f, -9.10542002e-02f, -3.58872752e-02f, -4.26943035e-01f, 3.64735953e-01f, -1.03226873e+00f, 1.33371436e+00f, -1.00394548e-01f },
        { -4.03932186e-01f, -1.16033806e+00f, 5.20231904e-01f, -5.74577874e-01f, -6.66788013e-01f, 9.31595698e-02f, 7.75744842e-02f, 1.31291593e+00f },
        { -8.67341765e-03f, 1.37503347e+00f, -8.89022324e-01f, 7.86518696e-01f, -7.09821661e-01f, 5.36349811e-01f, 2.83898582e-01f, -6.75230145e-02f },
        { -2.79488105e-01f, -9.67889798e-01f, 5.17377998e-01f, -5.01190329e-01f, -1.03596562e+00f, 2.63140530e-01f, 4.05165175e-01f, -4.57021084e-02f },
        { 1.68038217e-01f, -2.62520193e-01f, 4.34709985e-01f, -4.36601659e-01f, -1.14335427e+00f, -1.38212409e-01f, 3.39220883e-01f, -3.40304903e-01f },
        { -4.64879503e-01f, 2.83252030e-01f, 3.95647332e-01f, -3.76533239e-02f, 8.15821219e-01f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { 6.73839248e-02f, 9.80488526e-01f, -3.19482446e-01f, 7.61540287e-02f, 2.77809717e-01f, 1.37313108e+00f, -3.03383302e-01f, 1.37120881e-01f },
        { -6.15545098e-01f, 7.12695890e-01f, -9.96701487e-01f, 0.00000000e+00f, -3.98574872e-01f, 1.99910596e-02f, -4.49706069e-01f, 7.93347604e-01f },
        { 3.09201508e-01f, -3.29525479e-02f, 4.01800576e-01f, -6.24644957e-01f, 1.12550463e+00f, 0.00000000e+00f, -4.53282545e-01f, 0.00000000e+00f },
        { 7.75052761e-02f, -8.76692426e-01f, -7.31966016e-01f, 1.06295579e-01f, -7.78417775e-01f, 3.73934432e-01f, -5.38517475e-01f, 6.58302135e-01f },
        { 2.65511352e-01f, -1.71100091e-01f, -4.85939066e-01f, 1.03551650e+00f, 2.17934591e-01f, 1.03043082e+00f, -6.79780233e-01f, 7.30386002e-02f },
        { 4.02522195e-01f, -7.71968098e-01f, -2.92132301e-01f, 1.64622891e-01f, 1.26706682e+00f, 0.00000000e+00f, -5.16798653e-01f, 0.00000000e+00f },
        { -4.51383494e-03f, 5.59753744e-01f, -5.30711929e-01f, 2.55406750e-01f, 8.95021697e-01f, -8.56724233e-01f, 5.82233101e-02f, -5.71440117e-01f },
        { -9.03422084e-01f, 4.12808690e-01f, -6.68429556e-01f, 4.91750849e-02f, 9.69130215e-01f, -5.10164361e-01f, 5.70806456e-01f, -8.78338547e-01f },
        { -5.12058462e-02f, 3.61198948e-01f, -4.62968538e-01f, 8.13517995e-01f, -8.45771364e-01f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { -2.86960750e-01f, 5.69763760e-01f, -9.03391295e-01f, 1.07351825e-01f, 1.19735850e-01f, -7.17748598e-01f, -4.15199308e-01f, 8.49012896e-01f },
        { -5.04628417e-01f, 6.41629702e-01f, 1.33933445e-01f, -2.37718265e-01f, -5.11280815e-02f, -6.54633954e-01f, 2.55000182e-01f, -9.67759761e-01f },
        { 7.18774219e-01f, -6.76691931e-01f, 9.58707210e-01f, -5.03330571e-01f, -8.05851552e-01f, 5.71735883e-01f, 1.36108784e-01f, -1.80896578e-01f },
        { -1.50355109e-01f, 9.24548095e-02f, 1.24026446e+00f, 1.31863926e-01f, 6.58122901e-01f, 0.00000000e+00f, -8.34479020e-01f, 2.50478201e-01f },
        { 1.22250668e-01f, -7.93675581e-01f, -3.51796767e-01f, 7.65404967e-01f, 9.02784338e-02f, -5.55487474e-01f, 1.03839741e-01f, -8.77774337e-01f },
        { -3.21314207e-02f, -5.67371680e-01f, 1.54030457e-02f, 4.49691190e-01f, 5.69232367e-01f, -5.57806271e-01f, -2.22838120e-01f, -1.03295559e+00f },
        { 4.44759071e-01f, -3.92951606e-01f, -5.77427100e-01f, 4.92690467e-02f, 8.04047833e-01f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { -2.07190779e-01f, 1.43183953e-01f, 5.72664237e-01f, -1.63578108e-01f, -3.85292801e-01f, 7.37555416e-01f, -4.82809461e-01f, 0.00000000e+00f },
        { -8.46606573e-01f, 0.00000000e+00f, 2.19993101e-01f, 0.00000000e+00f, 3.50102715e-01f, -2.21925707e-01f, -1.64181476e-01f, 5.94673616e-01f },
        { 6.57922123e-02f, -1.80831726e-01f, -1.33957150e-01f, 7.63485116e-01f, -7.45139829e-01f, 3.59747626e-01f, 6.27218925e-01f, 0.00000000e+00f },
        { -6.61653924e-02f, 1.92083066e-01f, -6.98304709e-01f, 2.85791080e-01f, -8.79157925e-01f, -2.55130419e-02f, 1.42194484e-01f, 0.00000000e+00f },
        { 3.72659582e-01f, -5.01499425e-01f, -1.21377066e+00f, -3.63592463e-01f, -8.69847455e-02f, 6.57170447e-01f, -3.51839367e-02f, 6.15138677e-01f },
        { 2.10816577e-01f, -4.37223445e-01f, 1.02962797e+00f, 1.75097431e-01f, 3.21792301e-02f, -4.80009851e-01f, 1.83389223e-01f, -8.18343205e-01f },
        { 2.06209649e-02f, -8.48952225e-01f, -8.73762273e-01f, -5.79542921e-02f, 8.40476773e-01f, -1.32520696e-01f, -2.04268286e-01f, 0.00000000e+00f },
        { 8.27918714e-02f, -6.94224594e-01f, 1.62963204e-01f, -3.19800970e-01f, -4.06556919e-01f, 7.98879576e-01f, -1.02386429e+00f, 3.79254065e-01f },
        { -3.04033093e-01f, 7.18845324e-01f, -2.02944408e-01f, -1.23319096e+00f, 6.61862158e-02f, 1.20295866e+00f, -1.68705414e-01f, 1.77782066e-01f },
        { -6.72296446e-02f, 7.58501621e-01f, -7.80797390e-01f, 2.64515159e-01f, -7.63006394e-01f, 7.14607125e-01f, -4.49180033e-01f, 1.12742284e+00f },
        { 1.84409000e-02f, -7.21837200e-01f, -6.15967726e-01f, 2.45333653e-01f, -8.23213301e-01f, 5.60011946e-01f, 3.25854279e-01f, -7.40723453e-01f },
        { -5.84815956e-02f, -6.77779341e-01f, 8.61068350e-01f, -3.61979417e-02f, 7.90525294e-01f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { 2.77710015e-01f, -1.78949826e-02f, -4.05307524e-01f, 1.36850599e+00f, -9.71843905e-02f, -8.59447754e-01f, 3.78297286e-01f, 0.00000000e+00f },
        { 2.70855008e-01f, -2.81686984e-02f, 2.15446078e-01f, -7.58410762e-01f, 1.09206276e-01f, 0.00000000e+00f, -9.06490816e-01f, 0.00000000e+00f },
        { 1.99149281e-01f, -6.75302846e-01f, 1.39026139e+00f, -1.72369704e-01f, -3.63936830e-01f, 1.98536007e-02f, 1.01226307e+00f, -2.60308900e-01f },
        { 2.30239323e-01f, -3.57493010e-01f, -8.12990411e-01f, -7.44574955e-02f, 3.65389521e-01f, -8.28495180e-01f, 5.62212954e-01f, -5.71988965e-01f },
        { 8.29739325e-01f, -2.22488927e-02f, 3.91808787e-01f, -5.73711782e-01f, -1.02531530e+00f, 0.00000000e+00f, -5.55084015e-02f, 3.59713516e-01f },
        { -2.72054827e-02f, 4.33812007e-01f, -7.31464675e-01f, 0.00000000e+00f, -5.06973454e-01f, 0.00000000e+00f, -3.30004877e-01f, 1.42408803e+00f },
        { -4.28305765e-01f, 8.70859355e-01f, -2.18845336e-01f, 6.56679294e-01f, 1.03792637e-02f, -4.19629943e-01f, 2.57627016e-01f, -1.47333548e+00f },
        { -5.40332574e-01f, 4.04547952e-01f, 2.92558641e-01f, -6.47812593e-02f, 7.79129327e-01f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { 1.09577010e+00f, 0.00000000e+00f, -4.38021267e-01f, 3.95969271e-01f, 8.04002546e-03f, 8.29101590e-01f, 1.95319409e+00f, -1.05524474e-01f },
        { -9.24121526e-01f, -1.09884774e-02f, 7.27636112e-01f, 6.51083367e-02f, -9.14101166e-01f, 8.03706385e-02f, -1.00009304e+00f, -3.32145924e-01f },
        { -1.53208269e-01f, 3.41028091e-01f, 1.46929102e-01f, -5.72196312e-01f, -9.92432357e-01f, -2.26922243e-01f, 8.38863619e-01f, -5.50854370e-02f },
        { 3.20798638e-01f, -1.87343027e-01f, -6.42535607e-01f, 2.20942648e-01f, -8.08151440e-01f, 0.00000000e+00f, 9.95600186e-01f, 9.03568221e-02f },
        { 2.89742298e-02f, -4.34813378e-01f, -5.92386750e-01f, 1.42267218e+00f, -7.19119767e-01f, 6.66214083e-01f, -7.08796682e-01f, 1.55493392e-01f },
        { 1.50792815e-01f, -7.62428926e-01f, 5.41938844e-01f, -5.92968935e-01f, -9.17439121e-01f, 5.33092026e-01f, -1.84267962e-01f, 2.33045188e-01f },
        { 4.58656303e-02f, 7.26681629e-01f, 7.76985361e-01f, -9.57580808e-01f, -1.34679211e+00f, -1.21614407e-01f, 5.05868365e-01f, -1.17958192e-01f },
        { 4.83476537e-01f, -7.42549198e-01f, 7.36775865e-01f, -4.35952692e-01f, -5.32041817e-01f, 2.59764610e-01f, 3.91146113e-01f, -1.68512694e-01f },
        { 2.99201247e-02f, -4.63357043e-01f, -3.59429064e-01f, 8.14309674e-01f, 6.14290470e-01f, 1.44307866e-01f, 2.90986468e-01f, -3.68621356e-01f },
        { 4.77934882e-02f, -6.78889387e-01f, 8.72691755e-03f, 3.60992021e-01f, 5.44235422e-01f, -7.78240572e-01f, 1.37452633e-02f, -1.01768117e+00f },
        { -6.00589474e-02f, -7.41002689e-01f, 3.06294066e-01f, -4.86516314e-02f, 1.16782788e-01f, 7.04202899e-01f, 1.85605166e-01f, -5.76231836e-01f },
        { 1.03755068e-01f, -1.63958071e-01f, -4.57888721e-01f, 5.55001037e-01f, -4.11705984e-01f, 3.46774942e-01f, 1.48580250e-02f, -9.18248745e-01f },
        { 4.41455022e-02f, -3.59301801e-01f, 2.45998096e-01f, -2.68154680e-01f, -1.92822562e-01f, 1.04323101e+00f, -2.27277540e-01f, 5.76665195e-01f },
        { 1.61281985e-01f, -2.21430094e-01f, -4.11070970e-02f, 1.60778397e+00f, 3.41767605e-01f, -6.32780377e-01f, -1.02065333e+00f, 0.00000000e+00f },
        { -2.18667781e-01f, 6.97850960e-01f, 3.30256478e-01f, -1.30864947e-01f, -1.05106228e-01f, 3.72603703e-01f, -8.31946171e-02f, -7.97716148e-01f },
        { -4.33638907e-01f, 3.35948337e-01f, -2.36993412e-02f, 7.01524586e-01f, 7.59892663e-01f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { 7.38279437e-01f, -6.13016505e-01f, -8.66390721e-01f, -1.69997895e-01f, -8.91133495e-01f, 5.61574239e-02f, 2.02711185e-01f, -6.92603303e-02f },
        { 2.53372439e-01f, -3.20210605e-01f, -9.66144834e-01f, 3.75183881e-01f, -7.78407038e-01f, 4.23683104e-02f, -8.08605282e-02f, 4.83722205e-01f },
        { 1.22828086e-01f, -6.34491808e-01f, 2.94862224e-01f, -1.01567433e-01f, -1.39261464e+00f, -8.36564209e-02f, 3.46196327e-01f, -1.14685974e-01f },
        { 6.30923254e-02f, 4.83852510e-01f, 6.32173429e-01f, -3.57544047e-01f, -8.09879265e-01f, 0.00000000e+00f, 3.59913172e-02f, 0.00000000e+00f },
        { -4.01065598e-02f, 4.94164174e-01f, -2.03403795e-01f, 1.12779446e-01f, -1.26851701e+00f, 1.10075589e-01f, -4.51265430e-01f, 5.10456233e-01f },
        { -6.22164574e-01f, 8.83837744e-01f, -7.49662617e-01f, 8.94642820e-02f, 1.41645355e-01f, -1.33453011e-01f, -4.27800031e-01f, 8.60227360e-01f },
        { 2.22908567e-01f, -5.56363681e-01f, 9.50119266e-01f, 1.89261826e-01f, 5.11754465e-01f, -5.94739484e-02f, -3.95595214e-01f, 1.08617372e+00f },
        { -6.71813610e-01f, 7.64780129e-01f, -3.90563393e-02f, 3.79199741e-01f, 7.44425941e-01f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { 8.95369496e-03f, 7.69062511e-01f, -7.03153061e-01f, -8.93448823e-02f, 2.12162032e-02f, -1.12013968e+00f, -7.22842996e-02f, 4.35484535e-01f },
        { -5.16806087e-01f, 3.03040767e-01f, 4.17603131e-01f, -7.10383646e-01f, -4.74457092e-01f, 5.26183199e-01f, -7.90703706e-01f, 4.24355399e-01f },
        { 1.09247630e-01f, -7.33088127e-01f, -1.54477571e+00f, -5.70584834e-02f, 5.98006725e-01f, -1.81039142e-01f, -5.77985873e-01f, 5.91955376e-02f },
        { 2.41548972e-01f, -4.31261170e-01f, -9.20431522e-01f, -1.75139907e-01f, 1.09657699e-01f, -7.96332461e-01f, 9.03391143e-01f, 1.11883206e-01f },
        { -1.25595128e-01f, 9.51187137e-01f, 1.76332959e-01f, 1.41425310e+00f, 4.23231669e-01f, -4.72194405e-01f, -2.89877272e-01f, -1.83315914e-02f },
        { 1.63540528e-01f, -3.54592515e-01f, 7.74720400e-02f, -9.40522762e-01f, -8.84030896e-01f, 5.18638384e-01f, 3.70372097e-01f, -2.55415890e-01f },
        { -8.65642852e-02f, -6.67897509e-01f, 2.51169259e-01f, -3.70470108e-01f, 6.57755354e-01f, -2.66295167e-01f, 1.27040282e-01f, -4.21487923e-01f },
        { -2.25609213e-01f, 8.37428722e-01f, -3.20219586e-01f, 0.00000000e+00f, -6.45276255e-01f, 4.69624342e-01f, -6.10469461e-01f, 1.19283455e-01f },
        { 7.31513117e-02f, -1.82107755e-01f, -9.33521945e-01f, 2.52430332e-01f, 3.15340625e-01f, -4.53397265e-01f, 5.61397092e-01f, -7.45093163e-01f },
        { -1.03386944e-01f, -7.94199139e-01f, 1.21093736e+00f, -6.26605078e-01f, -2.71319019e-01f, 4.82168393e-01f, 3.25754309e-01f, -7.91317803e-01f },
        { 3.71302822e-01f, -4.60743758e-01f, 9.11670473e-02f, -2.18676459e-01f, 1.97596025e-01f, -4.01640152e-01f, -1.13897052e+00f, -2.37890073e-01f },
        { -1.22396813e-01f, 1.27970411e-01f, 3.20045740e-01f, -8.38532092e-01f, -8.89354375e-02f, -8.43276025e-01f, 2.69287012e-01f, -1.97574722e-01f },
        { 8.98148047e-01f, 3.03081321e-02f, -4.39749606e-01f, 5.00695148e-01f, 1.49751810e-02f, -7.88403888e-01f, -7.43407608e-02f, 2.89333401e-01f },
        { 2.29232584e-01f, -6.72996586e-01f, 3.25234186e-01f, -4.06333135e-01f, 8.81996790e-01f, -7.13559958e-01f, -9.96111936e-01f, 8.04671011e-02f },
        { -2.97585354e-01f, 2.00238891e-01f, 9.15132043e-01f, -5.34473576e-01f, -5.15142469e-02f, 1.89509505e-01f, 1.82029213e-01f, -8.96793749e-01f },
        { 1.37743768e-02f, 7.23073211e-01f, -4.01137236e-01f, 6.81280565e-02f, -1.07346278e+00f, 2.73285148e-01f, 7.70074721e-01f, -4.99703434e-01f },
        { -2.32141319e-04f, -5.57397538e-01f, -7.91621314e-02f, 8.65043483e-01f, -1.72828610e-01f, 2.09865029e-01f, 5.72874615e-01f, -6.00602521e-01f },
        { -4.74620782e-02f, 1.64918776e-01f, -7.12696822e-01f, -3.98080585e-02f, 3.16763525e-02f, 0.00000000e+00f, 1.28935668e-01f, -9.16349385e-01f },
        { -1.41911601e-01f, 4.28787453e-01f, 4.67658443e-01f, 3.51615401e-03f, 9.36362439e-01f, 0.00000000e+00f, -8.43961594e-01f, -5.84640302e-02f },
        { -3.83179001e-01f, 4.84507406e-01f, 1.08738509e-01f, -2.28621825e-01f, 7.29148680e-01f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { -4.31768614e-01f, 1.37872891e-01f, -6.82660921e-02f, 7.85674310e-01f, -3.29063117e-01f, 3.32513455e-01f, 4.89018828e-01f, -2.68989187e-01f },
        { -1.73022044e-01f, 5.96014310e-01f, -7.66074596e-01f, 9.82029769e-02f, -7.92949169e-01f, 2.22073391e-01f, 2.88851223e-01f, -2.68158515e-01f },
        { 3.41472326e-02f, -3.34070044e-01f, 5.65620616e-01f, -5.83372471e-02f, -8.60924598e-01f, 3.13684366e-01f, -1.64519813e-01f, 7.42119861e-01f },
        { 4.69684044e-01f, -5.67465606e-01f, -5.73083819e-01f, -1.78676164e-02f, 1.69721836e-01f, 0.00000000e+00f, -9.34047223e-01f, 3.49192340e-02f },
        { -7.10056303e-01f, 0.00000000e+00f, 7.02269666e-01f, -4.58011389e-01f, -5.77427346e-02f, 9.36824400e-01f, -8.70398740e-01f, -8.81357855e-02f },
        { -2.46669175e-02f, 9.53327854e-01f, -2.29655903e-01f, -9.63602674e-01f, -6.06809635e-01f, 9.58417450e-01f, 2.04573255e-01f, -1.92363290e-01f },
        { -1.20432441e-01f, 6.33160165e-01f, 9.07084676e-01f, -1.06806208e+00f, -3.83280604e-01f, 3.73942589e-01f, 1.27654086e-01f, -2.74575874e-01f },
        { -2.00901289e-01f, 7.09716527e-01f, -5.49385304e-01f, 1.92449569e-01f, -6.06235505e-01f, 4.73998433e-01f, -4.69478547e-01f, 1.44991653e-01f },
        { -2.08183825e-01f, 4.33306566e-01f, -2.04244015e-01f, 1.26040575e-02f, 5.00656552e-01f, -4.76986217e-01f, 1.01396857e+00f, 2.12176366e-01f },
        { 4.47244358e-01f, -5.63485564e-01f, 6.56615210e-02f, -7.19887227e-01f, 6.18225207e-02f, -4.76721240e-01f, 8.55854228e-01f, -3.36470929e-01f },
        { 1.03360551e-01f, -6.11507226e-01f, -2.95932870e-01f, 3.80523829e-01f, 5.70384558e-02f, 7.04274919e-01f, -1.83703362e-01f, -1.05153535e+00f },
        { -6.51825230e-01f, 2.12763786e-01f, 3.23822094e-01f, -1.20701832e-01f, -7.67433549e-01f, 1.14875682e+00f, -6.63102852e-01f, 8.37945065e-01f },
        { -7.34047916e-01f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f, 7.41036699e-01f, 0.00000000e+00f, -2.31124021e-01f, 3.69062307e-02f },
        { -8.18376678e-01f, 1.97067038e-01f, -7.41427625e-01f, 4.41526940e-01f, 1.09381365e-01f, -2.65956416e-01f, -4.37065173e-01f, 9.58784502e-01f },
        { 4.94968294e-02f, -6.98822555e-01f, -5.56859232e-01f, 5.56326170e-01f, -8.14827121e-01f, 4.57959319e-01f, -6.72689691e-01f, -6.45748838e-02f },
        { -4.83078212e-02f, 4.90643705e-01f, 5.73255490e-01f, -7.57952080e-01f, -2.65073560e-01f, 7.08657888e-01f, -7.44872130e-01f, 5.25568656e-01f },
        { 6.33712368e-02f, -8.77415497e-01f, 5.87245840e-01f, -1.34778110e-01f, -6.91583248e-01f, 8.82036304e-01f, -5.58589376e-01f, -6.69543246e-02f },
        { -1.42788436e-02f, -4.51487628e-01f, -3.58157646e-01f, 3.12394963e-01f, -5.81352402e-01f, 1.11031586e+00f, 3.54699972e-01f, -8.30561614e-01f },
        { -2.40494876e-01f, -8.80027068e-01f, 4.17281427e-01f, 0.00000000e+00f, -5.63674016e-02f, 1.88423318e-01f, -5.57702794e-01f, 5.06596851e-02f },
        { 2.55671508e-02f, 8.61146719e-01f, -5.65649097e-01f, 5.94364089e-02f, -7.20641640e-02f, -7.77916334e-01f, 1.64172915e-01f, -1.86448055e-01f },
        { -3.29516115e-01f, 8.08126544e-01f, 1.90280902e-01f, -9.64826223e-01f, 1.25235605e-02f, 7.86803474e-01f, -5.01045614e-02f, 3.19782559e-01f },
        { -6.04668971e-02f, 3.79044214e-01f, 1.38220610e+00f, -5.27056838e-01f, 4.17066586e-01f, -9.83782908e-01f, -6.67854450e-01f, 4.07859720e-01f },
        { 6.11792855e-01f, 4.86777397e-02f, 4.77988561e-01f, -2.02594285e-01f, -3.11005346e-01f, -1.14456039e+00f, 2.49444780e-01f, -2.52587261e-01f },
        { -2.45247190e-01f, 2.69570499e-01f, -5.24536963e-01f, 1.94071995e-01f, -7.16954295e-01f, 1.30050211e-01f, -4.03730449e-01f, 3.62907868e-01f },
        { -1.72544963e-01f, 1.11159226e-01f, 4.51214230e-01f, -9.76349182e-01f, -6.42488918e-02f, 8.68709655e-01f, -3.65321582e-01f, 1.93316537e-01f },
        { 4.19232390e-02f, -6.58587255e-01f, -3.93609663e-01f, 9.53848802e-01f, -6.56617182e-01f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { -8.89462103e-01f, 4.93642055e-01f, -1.00431964e+00f, 1.96301138e-01f, -3.04715752e-02f, 8.01646114e-01f, 2.81394941e-01f, -6.14974355e-01f },
        { 5.30660322e-01f, -3.69733765e-01f, -9.80359734e-01f, 4.85351137e-01f, -5.92794382e-01f, 4.79338066e-01f, 2.31461707e-01f, -9.06394308e-01f },
        { 2.38834406e-02f, -6.50073799e-01f, -4.59119270e-02f, 5.56175977e-01f, -1.40627586e-01f, -8.19403585e-01f, 2.84205875e-01f, 0.00000000e+00f },
        { -4.68218925e-03f, -6.89258777e-01f, -1.00514934e+00f, 7.80670828e-01f, -9.43092056e-02f, 2.91506792e-01f, -8.30617347e-01f, 0.00000000e+00f },
        { -8.63240526e-02f, 3.69728930e-01f, -8.10312590e-01f, 1.21631243e-01f, -5.06066910e-01f, -6.07481800e-02f, 5.70260338e-01f, -3.14902395e-01f },
        { -1.27160672e-01f, 1.77711108e-01f, -8.68769511e-01f, -1.47719051e-01f, 4.99583012e-01f, -7.70153599e-01f, -6.20833487e-01f, 1.01916584e+00f },
        { 3.28381116e-02f, -2.52577050e-01f, -3.87323290e-01f, 9.99278014e-01f, 3.65773007e-01f, -3.14262117e-01f, 1.03260073e+00f, 1.10541166e-01f },
        { 4.31677583e-02f, -5.54689811e-01f, -1.43702965e-01f, 2.97789786e-01f, 3.01879845e-01f, 0.00000000e+00f, -7.70348162e-01f, 1.19771969e-02f },
        { 3.25273559e-02f, -3.09686892e-01f, 3.41638880e-01f, -5.86180567e-01f, 9.65733602e-01f, 0.00000000e+00f, -4.15132223e-01f, 3.15768400e-02f },
        { -4.29294267e-01f, 3.65964447e-01f, -3.21158675e-02f, 4.67065370e-01f, 7.14859044e-01f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { -2.86120281e-01f, 1.08417347e-01f, 4.86913294e-01f, -2.28398379e-01f, -5.72691122e-01f, 8.56184763e-03f, 1.27188786e+00f, -1.69040168e-01f },
        { 2.88749443e-01f, -6.19468542e-01f, -2.18065252e-02f, -7.65455109e-01f, -3.35024937e-01f, 2.48608164e-01f, -5.54283991e-01f, 5.44891828e-01f },
        { 1.83031141e-01f, -9.97141329e-02f, 8.39765578e-01f, 1.34643096e-01f, -4.84233297e-01f, 2.92339707e-02f, -7.51631484e-01f, 6.19245734e-02f },
        { -7.70252154e-01f, 1.22462467e-02f, -4.78965970e-01f, 3.61684369e-01f, -2.09578391e-01f, 2.75469476e-01f, -8.51861061e-01f, -2.60627305e-02f },
        { 9.99107190e-02f, -2.48089362e-01f, 1.05045094e+00f, -1.09642350e-01f, 4.69478122e-01f, -8.93227267e-01f, 1.10107955e-01f, -3.82233611e-01f },
        { -7.52017445e-01f, 5.43942605e-01f, 4.90257432e-01f, -4.94067189e-01f, 1.12618914e+00f, 1.04763678e-01f, -2.36182135e-01f, 3.68182508e-01f },
        { 5.71400334e-02f, -4.29032411e-01f, -1.80285866e-01f, 8.11271908e-01f, -2.53907361e-01f, -1.04958892e+00f, 1.99328008e-01f, -3.79280626e-01f },
        { -2.41011648e-01f, 5.04290420e-01f, 3.40809957e-02f, -8.38374315e-01f, -6.97102475e-01f, 4.24522156e-01f, 5.03135086e-01f, -8.88926183e-02f },
        { 4.48725499e-03f, -4.33290388e-01f, 1.16039261e+00f, 7.47896516e-02f, 9.00415873e-01f, 1.12371154e-02f, 7.72038908e-01f, -1.57254446e-02f },
        { -6.28297593e-01f, 5.09612068e-02f, -8.68106404e-01f, 3.43632156e-01f, -7.67847273e-01f, 1.03504111e+00f, 2.47128268e-01f, -7.58914906e-01f },
        { 5.21640625e-02f, -5.83150585e-01f, 9.32776697e-01f, -1.93832060e-01f, 7.47791742e-01f, -7.19050450e-01f, -5.26634601e-01f, -1.16071164e-01f },
        { 5.36622691e-01f, -1.08245739e-02f, -9.57109396e-01f, 3.89269229e-01f, 2.00100562e-01f, -4.64821741e-01f, 3.33851816e-01f, -4.43619455e-01f },
        { 5.81622030e-01f, -6.07523643e-01f, 1.09240936e+00f, 1.16221580e-01f, -5.67449913e-02f, 1.91974861e-01f, -8.13467125e-01f, 0.00000000e+00f },
        { 8.86022668e-01f, 4.24984908e-02f, -5.36161072e-01f, 7.39885233e-01f, -3.89327569e-02f, -9.63697618e-01f, 5.61520477e-01f, -5.94910365e-01f },
        { 2.02347091e-01f, -6.20320632e-01f, 1.41563298e-01f, 0.00000000e+00f, 7.91816138e-01f, -1.28592825e-01f, -3.57867116e-01f, 2.42172839e-02f },
        { 7.63382171e-02f, -5.03744902e-01f, -1.81031302e-01f, 8.37311169e-01f, 2.68296939e-01f, -4.01463638e-01f, -9.20342022e-03f, 1.23743826e+00f },
        { 6.87366194e-01f, -5.12600202e-01f, -6.49810807e-01f, -3.10743062e-02f, 2.26796283e-01f, -9.71147387e-02f, 8.11120678e-02f, 9.54026545e-01f },
        { -7.68077991e-01f, 6.63282567e-03f, -2.88758182e-01f, 5.18424277e-01f, 6.19311480e-02f, -5.41344440e-01f, -6.28951101e-02f, 3.84205384e-01f },
        { 5.32731277e-01f, 4.98121348e-02f, 8.39820363e-02f, -6.65153257e-01f, -7.96120680e-02f, -5.63455258e-01f, 9.42610877e-01f, -2.90548136e-01f },
        { -1.56231237e-01f, 6.55815021e-01f, 4.93941619e-01f, -7.34073768e-01f, -4.99623107e-01f, 1.32335699e-01f, 3.39252253e-01f, -4.53159570e-01f },
        { 1.12816744e-01f, -5.57162176e-01f, -3.34621920e-01f, 4.49191632e-01f, 1.56373297e+00f, 8.93123273e-02f, 2.84777634e-01f, -9.57117487e-02f },
        { 1.92354538e-01f, -1.63266025e-01f, 1.10306590e+00f, -3.11384637e-01f, 2.05757671e-01f, -8.46025195e-01f, -2.53417883e-01f, 2.60812065e-01f },
        { 6.63897158e-01f, 1.70291056e-02f, 2.37677206e-01f, -3.72742600e-01f, 5.71671697e-01f, -2.55446884e-02f, -7.12151576e-01f, 3.49636687e-02f },
        { 5.38108587e-01f, -6.66030706e-02f, -3.65448420e-01f, 1.06830508e-01f, 1.64945969e-02f, -5.71347214e-01f, 6.73746332e-01f, -5.33052405e-01f },
        { 8.29775444e-01f, -1.51458402e-01f, -4.74745199e-01f, 5.41799338e-02f, -5.99729685e-01f, 3.73296778e-01f, -2.06128525e-01f, 6.52066467e-01f },
        { -5.80487844e-01f, 7.90450456e-01f, -8.74305771e-01f, 0.00000000e+00f, -7.05311515e-01f, 3.91283458e-01f, 3.14408110e-01f, -1.05962842e-01f },
        { -8.09837517e-04f, 9.06935940e-01f, -4.30521312e-01f, 7.58215456e-01f, 3.68620983e-01f, -2.64981954e-01f, 4.10277682e-01f, -4.42079444e-01f },
        { -4.47660560e-02f, -9.28549614e-01f, -7.25519944e-01f, 1.61096472e-01f, 7.17560916e-01f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { -7.89104493e-01f, 2.19673735e-01f, -2.77587202e-01f, 9.46671294e-02f, 5.48544109e-01f, 3.57966657e-03f, -4.74775764e-01f, 7.19234648e-02f },
        { -6.70564589e-01f, 5.86368696e-01f, -1.00292776e-01f, -9.11319109e-01f, 1.71775634e-01f, -8.83630985e-01f, 1.49155077e-01f, -3.50489086e-01f },
        { -3.88655918e-01f, 7.93092810e-01f, 5.09128081e-02f, -8.06228232e-01f, 6.33508235e-01f, -2.30932836e-01f, -4.59947096e-01f, 7.50653356e-01f },
        { 1.02494019e-01f, -4.48727950e-01f, -7.32846654e-02f, 5.91856396e-01f, 6.79327551e-01f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { 2.10538933e-01f, -7.62704552e-01f, 7.49358410e-01f, -6.60336220e-01f, 2.59291659e-01f, -2.12331959e-02f, -7.57566427e-01f, -1.75162973e-02f },
        { 2.70190574e-01f, -2.52740124e-01f, -3.87586247e-01f, 9.16683269e-01f, 1.37530978e-01f, -5.37513763e-01f, 8.75005476e-01f, -4.41313616e-01f },
        { 2.99400317e-02f, -3.19058199e-01f, 7.15793036e-01f, -9.00919362e-02f, -8.41718152e-01f, -1.37778393e-01f, 7.52476268e-01f, -1.59048567e-02f },
        { 1.63037252e-01f, -3.84191873e-01f, 3.06548624e-01f, -1.40569011e-01f, 3.96142443e-01f, -4.93428903e-01f, -9.47446999e-01f, 4.05988508e-01f },
        { -6.89421207e-01f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f, 7.18003402e-01f, 0.00000000e+00f, -2.44911664e-01f, 1.20955969e-02f },
        { -1.96425538e-01f, -8.43421430e-01f, 6.02149762e-01f, -2.86768281e-01f, -7.38803743e-01f, 4.62234748e-01f, -1.89656578e-02f, 3.12586971e-01f },
        { 3.08618090e-02f, 5.33570923e-01f, 9.22222879e-01f, -1.42695258e-01f, 4.29979465e-02f, -4.22622647e-01f, -9.81238486e-01f, -1.46501707e-02f },
        { -2.40787846e-01f, 7.60866487e-02f, -4.34520135e-01f, 2.35970342e-01f, -2.10079725e-01f, 0.00000000e+00f, 1.29371772e+00f, -2.13796276e-02f },
        { 3.88215280e-01f, -1.85022207e-01f, 9.06590909e-03f, 9.52651307e-01f, -4.38272232e-01f, 1.47748106e-01f, 2.98081070e-01f, -1.02749497e-01f },
        { 2.07130428e-02f, -7.65310149e-01f, 8.65102658e-01f, -3.25154420e-01f, -7.92142998e-01f, 2.31866104e-01f, 3.97316357e-01f, -1.27664398e-02f },
        { -3.99248045e-03f, -4.53785419e-01f, 3.98747025e-01f, -3.02862681e-01f, -8.11545406e-01f, 1.17649429e-02f, 3.35758567e-01f, 0.00000000e+00f },
        { 1.79153063e-01f, -3.62973738e-01f, -1.51772588e-01f, 2.93823979e-01f, -4.42974253e-01f, 1.07187572e+00f, 4.74284165e-02f, -6.21864181e-01f },
        { -3.34858058e-02f, 1.62879276e-01f, -5.87952676e-01f, 4.53036116e-01f, 2.65259567e-01f, -6.70860065e-01f, 7.75548955e-01f, -2.90575244e-01f },
        { 8.99501066e-02f, -9.04231762e-01f, 3.90448799e-01f, -3.31483926e-02f, 5.13422923e-01f, -8.40757911e-01f, -2.58248969e-01f, 2.90887893e-01f },
        { -2.52435724e-01f, 4.19758095e-01f, -4.51542542e-01f, 1.84005310e-01f, 5.55827826e-01f, -4.98313197e-01f, 9.95858642e-02f, -4.83096916e-01f },
        { -3.88287450e-01f, 4.04967804e-02f, -9.30030279e-01f, -6.28864120e-03f, 6.82620416e-01f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { 5.54618885e-01f, -1.91694840e-01f, -4.44080689e-01f, -2.09690355e-02f, 5.93939894e-01f, -5.97742003e-01f, -9.41039492e-01f, -3.73380539e-02f },
        { 1.21377669e-01f, -9.03859086e-02f, -4.17153209e-01f, 9.59508703e-01f, -6.66739646e-01f, 0.00000000e+00f, 2.72619455e-01f, -2.26517419e-01f },
        { 6.49923931e-01f, 1.14147110e-01f, -6.03922798e-01f, -6.97714504e-03f, 1.47469222e-01f, -6.00761159e-01f, -9.96433467e-02f, -7.38085576e-01f },
        { -2.31015907e-01f, 3.15271173e-01f, 1.01628855e-01f, 9.15626352e-01f, -8.20229477e-01f, 0.00000000e+00f, 3.53564098e-01f, -1.78193003e-02f },
        { -1.82151647e-01f, 5.36273354e-01f, 1.15441523e-02f, -4.53675800e-01f, 5.08490885e-01f, -6.21105211e-01f, -7.84094734e-01f, 2.23056829e-02f },
        { 6.13569384e-02f, -4.40456405e-01f, -2.30356707e-02f, 9.61855519e-01f, 3.16827863e-01f, -4.23134368e-01f, 2.38915354e-01f, -9.03815334e-01f },
        { -3.80171043e-01f, 1.13561501e-01f, -8.59272991e-01f, 3.31388924e-01f, 6.97701426e-02f, -2.29469890e-01f, -2.34771551e-01f, 6.66639802e-01f },
        { 2.35966508e-01f, -3.59353641e-01f, -5.71331287e-01f, 2.64271074e-01f, -8.00692464e-01f, 3.11133101e-01f, 8.70500127e-01f, -1.38369294e-01f },
        { 1.75080161e-01f, -4.41054809e-01f, 7.69920276e-03f, 6.19094580e-01f, -3.65432500e-01f, 2.45524759e-01f, 1.02519001e-01f, 1.09999583e+00f },
        { 4.72305814e-02f, 4.99814549e-01f, 3.48535775e-01f, -6.15516858e-01f, 2.12989860e-01f, -7.98835322e-01f, 2.72054073e-01f, -5.85561452e-01f },
        { -1.35634324e-01f, -7.95997805e-01f, 4.59398841e-01f, -6.63167605e-02f, 7.02518775e-01f, -3.58878702e-01f, -1.68978719e-02f, 2.72716390e-01f },
        { 5.46197411e-01f, -5.53293922e-01f, 1.04041367e+00f, -2.89042840e-01f, -1.98111625e-02f, -6.94190138e-01f, 5.58825576e-01f, -6.48101869e-02f },
        { -6.29294411e-01f, 7.31022880e-01f, -5.18698962e-02f, 3.61828205e-01f, 8.66860934e-01f, 2.69245139e-02f, -4.22041647e-01f, 3.16261275e-01f },
        { 1.97481923e-01f, -4.36016937e-01f, -4.42117250e-01f, 6.38955947e-02f, 3.77495675e-01f, -9.10851412e-01f, 8.33351137e-01f, -2.98832503e-01f },
        { -4.42882656e-01f, 5.65371061e-01f, 5.62044882e-02f, -3.23310585e-01f, 3.67991952e-01f, -4.12829550e-01f, 2.33006753e-01f, -1.03665130e+00f },
        { -8.02231487e-01f, 1.21708852e-03f, 6.19213532e-01f, -4.68120515e-01f, 4.71060259e-02f, 5.27063632e-01f, -9.16723919e-01f, 2.76988964e-01f },
        { -1.36570449e-01f, 1.91084967e-01f, 2.56037562e-01f, -5.40809172e-02f, 3.99885302e-01f, -2.33173580e-01f, -1.05571647e+00f, -1.31896548e-01f },
        { -1.52172949e-01f, 2.66160703e-01f, -5.78805898e-01f, 3.78391879e-01f, -8.49080415e-01f, 0.00000000e+00f, 4.15885203e-01f, -5.85811475e-01f },
        { 2.91399780e-01f, -3.94216866e-01f, 7.93141656e-01f, -2.16311473e-02f, -6.28268221e-03f, -4.11053857e-01f, 1.33703411e-01f, -6.82294725e-01f },
        { -4.85790491e-01f, 3.08971457e-01f, 7.71871270e-01f, 1.47423134e-03f, 6.56093459e-01f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { 3.29172634e-01f, -6.97035487e-01f, 7.44428012e-01f, -3.18786034e-02f, -6.53361050e-01f, -5.26441080e-02f, 3.09426384e-01f, -7.56123107e-01f },
        { 5.85264542e-02f, -5.56048225e-01f, -3.91485719e-01f, 7.88625143e-01f, -6.40664210e-01f, 0.00000000e+00f, 2.36591825e-01f, -1.79240764e-01f },
        { 5.36594609e-01f, 0.00000000e+00f, -7.52218484e-01f, -1.76996759e-01f, 8.44566525e-01f, -3.84586887e-02f, -3.41683510e-01f, 7.81317690e-04f },
        { -1.07099698e-01f, 1.80592153e-01f, 9.95764103e-02f, 1.60591531e+00f, 7.33536788e-01f, -4.62337050e-01f, -8.16243231e-01f, 7.45134684e-02f },
        { -8.36180199e-02f, 2.26489638e-01f, -3.07343589e-02f, -5.35373500e-01f, -5.94107934e-01f, 4.61444465e-01f, 1.09219098e+00f, 1.05519289e-01f },
        { -1.88680023e-02f, 3.34919316e-01f, -6.59143139e-01f, 2.40065454e-01f, -8.00676918e-01f, 0.00000000e+00f, 2.89362909e-01f, -5.49354521e-02f },
        { 3.50650105e-01f, -9.38258815e-03f, -2.50143205e-01f, 7.63424717e-01f, -4.58028948e-01f, 6.68220829e-01f, -8.32423689e-01f, 2.59825950e-01f },
        { -3.58163597e-01f, 1.44946642e-01f, 3.63736025e-01f, -4.17284587e-01f, 6.21523907e-01f, -4.42093278e-01f, -8.36578570e-01f, -1.34310305e-01f },
        { -1.70084175e-01f, 6.34953251e-02f, 7.48855583e-02f, -8.98448439e-01f, -1.87225892e-01f, 5.42149051e-01f, 8.27639111e-01f, -3.14886431e-01f },
        { 1.52411456e-01f, -7.31609174e-01f, -8.91174370e-01f, 1.32929329e-01f, -4.02058159e-01f, 2.00774534e-01f, -7.59686713e-01f, 2.97572982e-01f },
        { -2.56198137e-01f, 3.59200477e-01f, 2.98458260e-01f, -1.54253278e-01f, -7.25784088e-01f, -4.56138310e-02f, 5.18584196e-01f, -3.52464010e-02f },
        { 4.69218826e-01f, -4.95229612e-01f, 1.00234294e+00f, -1.24418174e-01f, -8.17748655e-03f, -9.29892459e-01f, 2.08204214e-01f, -6.43403089e-02f },
        { 2.58943937e-02f, -2.72247887e-01f, -4.42715994e-01f, 1.10840462e+00f, 2.05029480e-01f, -5.64124402e-01f, 2.47054663e-01f, 1.09316170e+00f },
        { -3.47938915e-01f, 4.90267428e-01f, -7.63418154e-01f, 2.67523091e-01f, 1.01378396e-01f, -9.35596880e-01f, 2.76352652e-01f, -6.13012487e-02f },
        { -1.05216915e-01f, 2.89095671e-01f, 2.78120076e-01f, -8.07415578e-01f, -5.87954803e-01f, 9.74048968e-01f, -5.70670777e-01f, 5.88086674e-03f },
        { 1.23329167e-01f, -5.13318467e-01f, 2.96098576e-01f, -3.78312429e-01f, -4.98915446e-01f, 5.37085341e-01f, -6.13828683e-01f, 4.05260509e-01f },
        { 3.08103927e-01f, -2.60743516e-02f, -5.35031347e-01f, 5.54509631e-01f, -7.46621319e-01f, -9.21220837e-02f, 1.58195715e-01f, 0.00000000e+00f },
        { -3.61153241e-01f, 2.50640876e-01f, 3.64303287e-01f, -7.95813430e-01f, -2.54285583e-01f, 1.68628132e-01f, 4.57269930e-01f, -6.47545421e-01f },
        { 3.65654727e-02f, 4.52455712e-01f, 6.05921230e-01f, -3.38317891e-01f, 4.27026464e-03f, -8.05684295e-01f, -3.61110121e-01f, 2.25463516e-01f },
        { -2.50164272e-01f, -7.85735117e-01f, 7.58018995e-01f, -1.65456828e-01f, 4.34108678e-01f, -5.56003132e-02f, 1.03397339e-02f, -5.59843251e-01f },
        { 2.33610460e-01f, -2.41875059e-01f, -2.94347706e-01f, 2.63404071e-01f, 1.25091063e+00f, -3.41518974e-01f, 4.51739170e-01f, -1.16002753e-01f },
        { 3.42756524e-01f, -3.79303045e-01f, -6.76293789e-01f, -4.87418607e-02f, -8.16872142e-01f, 2.19819636e-01f, 4.84186293e-01f, -8.76918772e-02f },
        { 3.90379470e-01f, 1.40674417e-02f, 2.98766004e-01f, -9.54039921e-01f, 2.54144861e-01f, -1.03994479e-01f, -4.65920220e-01f, 1.08632284e+00f },
        { -1.47293099e-01f, 2.49888651e-01f, -5.40148818e-01f, -5.10851657e-02f, -2.23776760e-01f, 0.00000000e+00f, 5.06196675e-02f, 1.19703311e+00f },
        { 1.16989799e-01f, -3.19893084e-01f, 8.52005116e-01f, -2.56258449e-01f, 9.52284894e-02f, 9.01103582e-01f, 4.18757920e-01f, -8.09885369e-02f },
        { -2.27815716e-03f, 4.97252716e-01f, -4.98262736e-01f, 9.40632943e-02f, 3.80711727e-01f, 0.00000000e+00f, 4.08710045e-01f, -6.78334924e-01f },
        { 2.36557483e-02f, -1.50360395e-01f, 5.87589970e-01f, -1.73613556e-01f, 7.25917631e-01f, 0.00000000e+00f, -2.16636865e-01f, 5.19768664e-05f },
        { -3.26719512e-01f, 2.75272167e-01f, 2.76577529e-01f, -9.70860746e-02f, -2.15194983e-01f, 0.00000000e+00f, 1.19423672e-02f, 1.13505254e+00f },
        { -3.71465419e-01f, 5.56994176e-01f, -8.97001517e-01f, -1.97753397e-01f, 5.56615740e-02f, -2.55782848e-01f, -4.18343734e-02f, 5.02713567e-01f },
        { -5.94111705e-02f, 4.34716054e-01f, 1.18281057e-01f, -5.26558016e-01f, 5.06464849e-01f, -6.27537051e-01f, -6.79616135e-02f, 6.00892157e-01f },
        { 5.54044450e-02f, -3.22173597e-01f, -9.33258217e-01f, -1.36946189e-01f, 5.01690250e-01f, -4.28259730e-01f, 3.23326739e-01f, -1.86867082e-01f },
        { 1.11637346e-01f, -4.11834629e-01f, -4.47029903e-01f, 1.54000739e-01f, -8.02612009e-01f, 2.18147126e-01f, 8.73490316e-01f, -4.28628909e-03f },
        { -5.41897920e-02f, 2.07305559e-01f, 1.23771814e-01f, -4.35695874e-01f, -3.64695284e-01f, 3.63886940e-01f, 7.68591548e-01f, 1.28940288e-01f },
        { 9.84722632e-02f, -7.31546151e-01f, -5.76679451e-01f, 6.22508574e-01f, -4.05142906e-01f, 6.47395609e-01f, -5.22337412e-01f, 1.06636234e-01f },
        { -3.05905071e-02f, 4.36743326e-01f, 1.65846432e-01f, -7.81164352e-01f, -5.49395269e-02f, 8.19085238e-01f, -7.76617746e-01f, 2.73666837e-01f },
        { 4.49377879e-01f, -5.29161955e-01f, 8.80036728e-01f, -2.56134071e-01f, 6.43590937e-01f, -5.30016255e-01f, 2.09150663e-01f, -1.00858728e-01f },
        { 5.42276891e-02f, -4.10240983e-01f, 9.43601220e-01f, 9.14062451e-02f, 2.04508234e-01f, -2.88573184e-01f, 4.04022511e-01f, -5.00571797e-01f },
        { 3.09139709e-01f, -4.52893861e-01f, -2.48864695e-01f, 2.35662180e-01f, -3.93110247e-01f, 4.00995495e-01f, -5.44493744e-02f, -9.20205291e-01f },
        { -1.80645391e-01f, 3.40103726e-02f, 1.05873876e+00f, -2.09401576e-01f, -8.65204516e-01f, 1.10646177e-02f, 3.55957813e-01f, -1.03406271e-02f },
        { 3.09536495e-01f, -1.90405807e-01f, 1.02484226e+00f, 1.27486014e-01f, 9.48033360e-02f, -2.87393575e-01f, -6.60916278e-01f, 4.10454939e-01f },
        { 2.46179608e-01f, -1.43916247e-02f, -6.19301522e-02f, 7.59171973e-01f, 5.16913083e-01f, -2.67703822e-01f, -8.85569553e-01f, -3.88260859e-03f },
        { 3.89845126e-01f, -4.32133376e-01f, -6.18595610e-01f, 1.95280302e-03f, -1.12556908e-01f, -7.91439103e-01f, 4.66076121e-01f, -4.77900850e-02f },
        { 6.99191263e-01f, -2.56049118e-02f, -5.93342762e-03f, -8.06806742e-01f, 6.22474196e-02f, -4.75178635e-01f, -3.23840106e-01f, 2.91869712e-01f },
        { 4.52698747e-01f, -6.31642053e-01f, -8.95980671e-02f, 8.26327135e-01f, 3.53142604e-01f, -4.52998688e-01f, 4.08323456e-01f, -3.51957304e-01f },
        { -1.85676518e-01f, 8.41603253e-02f, -8.59505863e-01f, 0.00000000e+00f, 6.66414559e-01f, -5.60301599e-01f, 4.13829877e-01f, -1.02231541e-01f },
        { 9.38641206e-02f, -1.67652639e-01f, -1.95711226e-01f, 5.44953556e-01f, -6.91128935e-01f, 0.00000000e+00f, -5.69338394e-02f, 3.06538913e-01f },
        { 5.13927871e-02f, -3.99759450e-01f, -3.48457649e-01f, 3.52802111e-01f, 5.43299066e-01f, -6.43062089e-01f, -7.78755334e-02f, 3.87715671e-01f },
        { -7.56124141e-01f, 4.18134776e-01f, -4.49537416e-01f, 6.90884520e-02f, 2.43513519e-01f, -6.90285555e-01f, 1.73841234e+00f, -7.63705701e-02f },
        { 1.99966470e-01f, -3.11482310e-01f, 6.54617828e-01f, -2.37625593e-01f, 8.04007941e-01f, -2.26512777e-01f, -5.26227053e-01f, -4.79335179e-02f },
        { -5.31787865e-01f, 0.00000000e+00f, -9.65833415e-02f, 8.56880647e-01f, -4.84327651e-02f, 2.53637081e-01f, 2.28745510e-01f, -6.73878787e-01f },
        { -8.70127888e-02f, 1.91769705e-01f, 5.05510805e-02f, 5.80142467e-01f, 6.17820483e-01f, -8.99798753e-02f, -3.18455066e-01f, 2.64206937e-01f },
        { 2.21216685e-03f, 3.29450545e-01f, -7.26228743e-01f, 1.00814803e-01f, 6.59433811e-01f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { -8.12282758e-01f, 0.00000000e+00f, 4.63793490e-01f, -2.31759191e-01f, -2.16545282e-01f, 7.82314269e-02f, 7.63012490e-01f, 7.16271382e-02f },
        { -1.27767225e-01f, 1.78669713e-01f, 3.36157840e-01f, -8.47131358e-01f, -8.33481969e-01f, 5.34403305e-01f, 7.91403361e-01f, -1.68574886e-01f },
        { 7.43117296e-01f, -3.60058919e-02f, -2.69053933e-01f, 2.40713031e-01f, 3.99096267e-01f, -1.77570575e-02f, -4.13958658e-01f, 7.04800202e-02f },
        { 1.24655089e-02f, -1.92998543e-01f, -2.30120927e-01f, 9.91119148e-01f, 6.33058252e-01f, 0.00000000e+00f, 0.00000000e+00f, 0.00000000e+00f },
        { -2.42840610e-02f, 5.80109568e-01f, 5.88280531e-01f, -7.03201289e-02f, -4.03082810e-01f, 4.64212132e-02f, 1.01502158e+00f, -3.80036353e-02f },
        { 3.00266648e-02f, -6.74234929e-01f, -2.57773887e-01f, 4.57488167e-01f, 2.07813302e-01f, -2.82507122e-01f, -6.64181458e-01f, 4.44764668e-02f },
        { 5.04703610e-02f, -3.18555210e-01f, 3.08192572e-01f, -4.87313116e-01f, -9.83595416e-01f, 6.71594422e-02f, 3.11733599e-01f, -3.95862481e-01f },
        { -3.07529213e-01f, 5.12924840e-01f, 1.91960997e-01f, -9.49110565e-01f, -4.31492782e-01f, 2.72594828e-01f, -1.59175679e-01f, 2.24831483e-01f },
        { 1.53039070e-01f, -4.85720015e-01f, -5.19253545e-01f, 6.93187927e-01f, -7.27020112e-02f, 2.06544105e-01f, 4.73343759e-01f, -4.37695328e-01f },
        { -2.86465102e-01f, 4.55821213e-01f, -7.16293331e-01f, 2.52201186e-01f, 5.38659913e-01f, 6.87119421e-02f, -5.93298883e-01f, 2.63165610e-01f },
        { 8.90516571e-02f, 5.82128411e-01f, 3.99196915e-01f, -6.79475957e-01f, 3.30547039e-01f, -4.13434207e-01f, -5.80769339e-01f, -6.91762243e-02f },
        { -3.15881607e-01f, 1.22150664e-01f, -5.41319403e-01f, 5.68720140e-01f, -3.48862606e-01f, 6.73123642e-02f, 1.01898176e+00f, -1.09918491e-01f },
    };
    // ---- END MODEL
};

// ------------------------------------------------------------------------------------------------ interlacing
// Field combing on moving areas of native-resolution luma (never a scaled picture: scaling removes combs).
// R = sum |Y[y-1] - 2Y[y] + Y[y+1]| / sum |Y[y-2] - 2Y[y] + Y[y+2]| over pixels that moved against the frame before.
// Progressive pictures are smooth at one-line distance (R 0.35-0.66 on 51 titles, one frame in 2237 above 1.0);
// two fields from different moments disagree line by line (woven 50/60p captures: R 1.4-4.3, 92 % of frames > 1.0).
class CombMeter {
public:
    static constexpr float kCombR = 1.0f;
    static constexpr int kNeed = 24;           // measurable (moving) frames for a verdict
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
