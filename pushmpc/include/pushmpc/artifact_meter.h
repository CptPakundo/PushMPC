// artifact_meter.h: compression damage measured in the picture (no reference, CPU, luma only).
//
// What the four bitrate-keyed Auto rules (Clean up, HDR boost, the RTX VSR style, deband) really want to know is how
// damaged the picture is, not how many bits the container says the stream has. This meter measures that on the frames:
//  - block:  energy of the 8x8 (and 4x4) transform grid. Along sampled lines, the step across each pixel boundary minus
//            the steps beside it, in low-activity areas, averaged per boundary phase (x mod 8); the best phase against
//            the median phase. Self-normalising: about 0 on clean or grainy pictures and on any crop offset. In-loop
//            deblocking (H.264 at moderate rates, HEVC, AV1) hides most of it: there the other scores carry the damage.
//  - ring:   mosquito noise and ringing: the activity (|second difference|) of flat pixels 3-5 px from a strong edge,
//            minus the same activity on flat pixels far from any edge (the grain floor). Grain is everywhere, codec
//            noise sits at edges, so a grainy master reads near 0.
//  - band:   CAMBI-like banding: share of the picture in plateaus (runs of one code value, at the native bit depth)
//            at least kBandRun px long next to another such plateau a visible step away (0.5-2 8-bit codes: an 8-bit
//            picture carried in 10 bits still bands, a true 10-bit ramp does not). Dither, grain and texture break
//            plateaus; an encoder that removes them leaves the steps.
//  - detail: loss of the finest detail: the share of 8-px windows (not black) whose largest |second difference| is at
//            most 1 code, i.e. smoother than any camera picture is (sensor noise and grain keep the finest octave alive;
//            an encoder at a high quantiser zeroes it). Camera masters read 0.0000-0.0001; flat drawn or rendered
//            pictures read high even when clean (a no-reference measure cannot tell a flat master from a flattened
//            encode), so the confidence is halved and the grain floor is reported beside it.
//  - noise:  the grain floor itself (mean |second difference| of flat pixels far from edges, 8-bit codes): falls as the
//            bitrate falls on every codec, but its clean level depends on the source, so it is context, not a score.
// Units: block, ring and noise in 8-bit code values, band and detail shares 0..1.
// Calibration (open 1080p50 camera masters, MPEG-2 / x264 / x265 / SVT-AV1 ladders 0.04-2.2 bits per pixel): detail
// and band rise as PSNR falls on every codec (weakest on x265, whose in-loop filters leave the least trace); block only
// on MPEG-2 and partly H.264; ring did not track the ladder (it falls with the near-edge texture) and is diagnostic.
//
// Sampling: kLines rows and kLines columns of native-resolution luma per sampled frame (a centre crop of at most
// 1920 x 1080 of them, the crop on the 8-pixel grid), a few frames a second: the copy (Sample) is cheap enough for
// a streaming thread, the analysis (Push) runs where the caller likes. Scores are per shot: medians of the sampled
// frames from one scene cut to the next (the meter's own cut test on the sampled lines, or the caller's cut signal).
// Header-only, plain C++17, no platform headers (every route can include it).
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace pushmpc::artifacts {

// one sampled frame: lines of native code values (8-bit: 0..255, 10-bit: 0..1023)
struct Tiles {
    int w = 0, h = 0, bits = 8;
    int x0 = 0, y0 = 0, rowLen = 0, colLen = 0;   // crop origin (multiples of 8) and line lengths
    std::vector<int> rowY, colX;                  // absolute positions of the sampled rows / columns
    std::vector<uint16_t> rows, cols;             // rowY.size() x rowLen, colX.size() x colLen
    double pts = 0;
    bool ok() const { return w > 0 && !rowY.empty() && !colX.empty(); }
};

struct FrameScores {
    float block = 0, ring = 0, band = 0, detail = 0;
    float blockN = 0, ringN = 0, bandN = 0, detailN = 0;   // qualifying samples behind each score
    float noise = 0;   // the grain floor (far-flat activity)
};

struct Shot {
    bool valid = false;
    double t0 = 0, t1 = 0;      // stream time of the first and last sampled frame
    float block = 0, ring = 0, band = 0, detail = 0;              // medians over the shot
    float noise = 0;                                              // the grain floor (median)
    int frames = 0;             // sampled frames
    float confBlock = 0, confRing = 0, confBand = 0, confDetail = 0;   // 0..1: how much of the picture qualified
    bool endedByCut = false;
};

class ArtifactMeter {
public:
    static constexpr int kLines = 64;            // rows and columns sampled per frame
    static constexpr int kMaxRow = 1920, kMaxCol = 1080;
    static constexpr int kBandRun = 8;           // plateau length (px at 1080p; scaled with the height)
    static constexpr int kMinFrames = 4;         // a shot shorter than this (sampled frames) merges into the next
    static constexpr double kMaxShotSec = 30.0;  // a long shot is reported in parts
    static constexpr double kSampleHz = 6.0;     // suggested sampling rate (the caller decides)

    void Reset() { *this = ArtifactMeter(); }
    // a seek / flush: the shot so far is dropped (the stream aggregate stays)
    void Discontinuity() { m_frames.clear(); m_havePrev = false; m_mads.clear(); }

    // Copy the sampled lines out of a luma plane. T = uint8_t (8-bit) or uint16_t; shift: bits to drop to reach the
    // native code (P010: 6), bits: the native depth. seq rotates the rows (different lines each frame).
    template <class T>
    static Tiles Sample(const T* y, ptrdiff_t stride, int w, int h, int shift, int bits, double pts, uint32_t seq = 0)
    {
        Tiles t;
        if (!y || w < 64 || h < 64) return t;
        t.w = w; t.h = h; t.bits = bits; t.pts = pts;
        t.rowLen = std::min(w, kMaxRow); t.colLen = std::min(h, kMaxCol);
        t.x0 = ((w - t.rowLen) / 2) & ~7; t.y0 = ((h - t.colLen) / 2) & ~7;
        const int nr = std::min(kLines, t.colLen / 4), nc = std::min(kLines, t.rowLen / 4);
        const int rs = t.colLen / nr, cs = t.rowLen / nc;
        const int ro = int(seq % uint32_t(std::max(1, rs / 2))), co = int((seq * 7u) % uint32_t(std::max(1, cs / 2)));
        t.rows.resize(size_t(nr) * t.rowLen); t.cols.resize(size_t(nc) * t.colLen);
        for (int r = 0; r < nr; ++r) {
            const int yy = t.y0 + r * rs + rs / 4 + ro; t.rowY.push_back(yy);
            const T* s = y + ptrdiff_t(yy) * stride + t.x0;
            uint16_t* d = &t.rows[size_t(r) * t.rowLen];
            for (int i = 0; i < t.rowLen; ++i) d[i] = uint16_t(unsigned(s[i]) >> shift);
        }
        // columns in groups of 8 neighbours (one cache line a row for each group, not one a column)
        const int groups = std::max(1, nc / 8), gs = t.rowLen / groups;
        for (int c = 0; c < nc; ++c) t.colX.push_back(std::min(t.x0 + t.rowLen - 1, t.x0 + (c / 8) * gs + gs / 4 + co + (c % 8)));
        for (int i = 0; i < t.colLen; ++i) {   // row by row (cache friendly): one value per column
            const T* s = y + ptrdiff_t(t.y0 + i) * stride;
            for (int c = 0; c < nc; ++c) t.cols[size_t(c) * t.colLen + i] = uint16_t(unsigned(s[t.colX[size_t(c)]]) >> shift);
        }
        return t;
    }

    // The analysis of one sampled frame (no state)
    static FrameScores Measure(const Tiles& t)
    {
        FrameScores f;
        if (!t.ok()) return f;
        const float u = 1.f / float(1 << std::max(0, t.bits - 8));   // native code -> 8-bit units
        const int bandRun = std::max(4, int(std::lround(kBandRun * std::min(t.h, 2160) / 1080.0)));
        Acc a;
        Scratch v;
        for (size_t r = 0; r < t.rowY.size(); ++r) Line(&t.rows[r * t.rowLen], t.rowLen, t.x0, u, bandRun, a, v);
        for (size_t c = 0; c < t.colX.size(); ++c) Line(&t.cols[c * t.colLen], t.colLen, t.y0, u, bandRun, a, v);
        // block: the best grid phase against the median phase
        float e[8]; double n = 0;
        for (int p = 0; p < 8; ++p) { e[p] = a.bn[p] > 0 ? float(a.be[p] / a.bn[p]) : 0.f; n += a.bn[p]; }
        float s[8]; std::copy(e, e + 8, s); std::nth_element(s, s + 4, s + 8);
        f.block = std::max(0.f, *std::max_element(e, e + 8) - s[4]); f.blockN = float(n);
        f.ring = a.edgeN > 0 && a.flatN > 0 ? std::max(0.f, float(a.edgeAct / a.edgeN - a.flatAct / a.flatN)) : 0.f;
        f.ringN = float(std::min(a.edgeN, a.flatN));
        f.band = a.bandTot > 0 ? float(a.bandPix / a.bandTot) : 0.f; f.bandN = float(a.bandTot);
        f.detail = a.winN > 0 ? float(a.winSmooth / a.winN) : 0.f; f.detailN = float(a.winN);
        f.noise = a.flatN > 0 ? float(a.flatAct / a.flatN) : 0.f;
        return f;
    }

    // One sampled frame. cut: the caller's scene-cut signal for the interval since the last push (1 cut, 0 none,
    // -1: the meter's own test). Returns true when a shot closed (LastShot()).
    bool Push(const Tiles& t, int cut = -1)
    {
        if (!t.ok()) return false;
        const bool isCut = OwnCut(t, cut);
        bool closed = false;
        if (isCut && int(m_frames.size()) >= kMinFrames) closed = Close(true);
        else if (!m_frames.empty() && t.pts - m_frames.front().t >= kMaxShotSec) closed = Close(false);
        Sampled s; s.t = t.pts; s.f = Measure(t);
        m_frames.push_back(s);
        return closed;
    }
    const Shot& LastShot() const { return m_last; }
    // the shot so far (not closed); valid once it has kMinFrames
    Shot Running() const { return Summarise(m_frames, false); }
    // the stream so far: closed shots weighted by their length
    Shot Stream() const
    {
        Shot s; if (m_sumW <= 0) return s;
        s.valid = true; s.frames = m_sumFrames; s.t0 = m_streamT0; s.t1 = m_last.t1;
        s.block = float(m_sum[0] / m_sumW); s.ring = float(m_sum[1] / m_sumW); s.band = float(m_sum[2] / m_sumW); s.detail = float(m_sum[3] / m_sumW);
        s.confBlock = float(m_sum[4] / m_sumW); s.confRing = float(m_sum[5] / m_sumW); s.confBand = float(m_sum[6] / m_sumW); s.confDetail = float(m_sum[7] / m_sumW);
        return s;
    }
    uint32_t shots() const { return m_shots; }

private:
    struct Acc {
        double be[8] = {}, bn[8] = {};
        double edgeAct = 0, edgeN = 0, flatAct = 0, flatN = 0;   // (not near / far: macros in windows.h)
        double bandPix = 0, bandTot = 0;
        double winSmooth = 0, winN = 0;
    };
    struct Sampled { double t = 0; FrameScores f; };

    struct Scratch { std::vector<float> v; std::vector<uint8_t> dist; };
    // one line: pos0 = absolute position of element 0 (the grid phase)
    static void Line(const uint16_t* c, int n, int pos0, float u, int bandRun, Acc& a, Scratch& sc)
    {
        if (n < 16) return;
        constexpr int kFar = 16;
        sc.v.resize(size_t(n)); sc.dist.assign(size_t(n), uint8_t(kFar));
        float* v = sc.v.data(); uint8_t* dist = sc.dist.data();
        for (int i = 0; i < n; ++i) v[i] = float(c[i]) * u;
        double be[8] = {}, bn[8] = {}, ws = 0, wn = 0;
        float wMax = 0, wSum = 0;   // the 8-px window (aligned to the line start): largest second difference, sum
        // block: step across the boundary i-1 | i minus the steps beside it, low-activity areas only;
        // detail: the over-smooth 8-px windows; ring, first: distance to the strong edge before
        int last = -1000;
        for (int i = 1; i + 1 < n; ++i) {
            const float g = std::fabs(v[i + 1] - v[i - 1]);
            if (g >= 24.f) last = i;
            dist[i] = uint8_t(std::min(kFar, i - last));
            wMax = std::max(wMax, std::fabs(2.f * v[i] - v[i - 1] - v[i + 1])); wSum += v[i];
            if (i % 8 == 0) { if (wSum > 8.f * 24.f) { ws += wMax <= 1.f ? 1 : 0; wn += 1; } wMax = 0; wSum = 0; }   // not black
            if (i >= 2) {
                const float step = std::fabs(v[i] - v[i - 1]);
                const float inner = 0.5f * (std::fabs(v[i - 1] - v[i - 2]) + std::fabs(v[i + 1] - v[i]));
                if (!(step == 0.f && inner == 0.f) && inner <= 3.f && step <= 12.f) { const int p = (pos0 + i) & 7; be[p] += step - inner; bn[p] += 1; }
            }
        }
        for (int p = 0; p < 8; ++p) { a.be[p] += be[p]; a.bn[p] += bn[p]; }
        a.winSmooth += ws; a.winN += wn;
        // ring: the strong edge after, then the activity of flat-ish pixels 3-5 px from an edge against far ones
        last = 1 << 20;
        double ea = 0, en = 0, fa = 0, fn = 0;
        for (int i = n - 2; i >= 1; --i) {
            const float g = std::fabs(v[i + 1] - v[i - 1]);
            if (g >= 24.f) last = i;
            const int d = std::min<int>(dist[i], std::min(kFar, last - i));
            if (g >= 8.f) continue;   // flat-ish pixels only
            const float act = std::fabs(2.f * v[i] - v[i - 1] - v[i + 1]);
            if (d >= 3 && d <= 5) { ea += act; en += 1; }
            else if (d >= kFar) { fa += act; fn += 1; }
        }
        a.edgeAct += ea; a.edgeN += en; a.flatAct += fa; a.flatN += fn;
        // band: runs of one native code value, decided once both neighbours are known (a 3-run window)
        struct Run { int len = 0; int val = -100000; };
        // a visible step: half an 8-bit code to two 8-bit codes (8-bit: 1-2 codes; 10-bit: 2-8, so an 8-bit picture carried
        // in 10 bits still bands, a true 10-bit ramp in 1-code steps does not)
        const int per8 = int(std::lround(1.f / u)), dMin = std::max(1, per8 / 2), dMax = 2 * per8;
        const auto banded = [&](const Run& r, const Run& x) { const int d = std::abs(x.val - r.val); return x.len >= bandRun && d >= dMin && d <= dMax; };
        double bp = 0, bt = 0;
        Run p0, p1;
        const auto finish = [&](const Run& r, const Run& before, const Run& after) {
            if (r.len <= 0) return;
            if (r.len >= bandRun && (banded(r, before) || banded(r, after))) { bp += r.len; bt += r.len; }
            else if (r.len < 32) bt += r.len;   // long lone flats (bars, flat graphics) do not count
        };
        int s = 0;
        for (int i = 1; i <= n; ++i)
            if (i == n || c[i] != c[s]) { const Run r{ i - s, int(c[s]) }; finish(p1, p0, r); p0 = p1; p1 = r; s = i; }
        finish(p1, p0, Run{});
        a.bandPix += bp; a.bandTot += bt;
    }

    // a scene cut from the sampled rows: mean |change| of 16 segment means per row against the last sampled frame,
    // above 12 codes and 3x the recent median, and the luma histogram (32 bins) moved by more than 0.4 (L1, of 2):
    // motion (water, crowds, a pan) moves the segment means but keeps the histogram
    bool OwnCut(const Tiles& t, int cut)
    {
        std::vector<float> m;
        const int seg = std::max(1, t.rowLen / 16);
        const float u = 1.f / float(1 << std::max(0, t.bits - 8));
        for (size_t r = 0; r < t.rowY.size(); ++r)
            for (int s0 = 0; s0 + seg <= t.rowLen; s0 += seg) {
                double sum = 0; const uint16_t* p = &t.rows[r * t.rowLen + s0];
                for (int i = 0; i < seg; ++i) sum += p[i];
                m.push_back(float(sum / seg) * u);
            }
        std::array<float, 32> hist{};
        for (uint16_t x : t.rows) hist[size_t(std::min(31, int(float(x) * u) >> 3))] += 1.f;
        for (float& x : hist) x /= float(t.rows.size());
        double hd = 0; for (size_t i = 0; i < hist.size(); ++i) hd += std::fabs(hist[i] - m_prevHist[i]);
        bool own = false;
        if (m_havePrev && m.size() == m_prevMeans.size()) {
            double mad = 0; for (size_t i = 0; i < m.size(); ++i) mad += std::fabs(m[i] - m_prevMeans[i]);
            mad /= double(m.size());
            float med = 0;
            if (!m_mads.empty()) { std::vector<float> q(m_mads.begin(), m_mads.end()); std::nth_element(q.begin(), q.begin() + q.size() / 2, q.end()); med = q[q.size() / 2]; }
            own = mad > 12.0 && mad > 3.0 * med && hd > 0.4;
            m_mads.push_back(float(mad)); if (m_mads.size() > 12) m_mads.erase(m_mads.begin());
        }
        m_prevMeans.swap(m); m_prevHist = hist; m_havePrev = true;
        return cut < 0 ? own : cut != 0;
    }

    static float Med(std::vector<float>& q) { if (q.empty()) return 0.f; std::nth_element(q.begin(), q.begin() + q.size() / 2, q.end()); return q[q.size() / 2]; }
    // per-frame confidence: share of the sampled lines that qualified, against what a typical picture gives
    static Shot Summarise(const std::vector<Sampled>& fr, bool cut)
    {
        Shot s;
        if (int(fr.size()) < kMinFrames) return s;
        s.valid = true; s.frames = int(fr.size()); s.t0 = fr.front().t; s.t1 = fr.back().t; s.endedByCut = cut;
        std::vector<float> q[8];
        for (const Sampled& x : fr) {
            q[0].push_back(x.f.block); q[1].push_back(x.f.ring); q[2].push_back(x.f.band); q[3].push_back(x.f.detail);
            q[4].push_back(std::min(1.f, x.f.blockN / 20000.f)); q[5].push_back(std::min(1.f, x.f.ringN / 2000.f));
            q[6].push_back(std::min(1.f, x.f.bandN / 20000.f)); q[7].push_back(std::min(1.f, x.f.detailN / 10000.f));
        }
        const float frames = std::min(1.f, float(fr.size()) / 12.f);
        s.block = Med(q[0]); s.ring = Med(q[1]); s.band = Med(q[2]); s.detail = Med(q[3]);
        {
            std::vector<float> n;
            for (const Sampled& x : fr) n.push_back(x.f.noise);
            s.noise = Med(n);
        }
        s.confBlock = frames * Med(q[4]); s.confRing = frames * Med(q[5]); s.confBand = frames * Med(q[6]);
        s.confDetail = 0.5f * frames * Med(q[7]);   // detail: a flat master reads as damage (see the top)
        return s;
    }
    bool Close(bool cut)
    {
        m_last = Summarise(m_frames, cut);
        m_frames.clear();
        if (!m_last.valid) return false;
        const double w = std::max(1e-3, m_last.t1 - m_last.t0) + 1.0 / kSampleHz;
        const float vals[8] = { m_last.block, m_last.ring, m_last.band, m_last.detail, m_last.confBlock, m_last.confRing, m_last.confBand, m_last.confDetail };
        for (int i = 0; i < 8; ++i) m_sum[i] += w * vals[i];
        if (m_sumW <= 0) m_streamT0 = m_last.t0;
        m_sumW += w; m_sumFrames += m_last.frames; ++m_shots;
        return true;
    }

    std::vector<Sampled> m_frames;
    std::vector<float> m_prevMeans, m_mads;
    std::array<float, 32> m_prevHist{};
    bool m_havePrev = false;
    Shot m_last;
    double m_sum[8] = {}, m_sumW = 0, m_streamT0 = 0;
    int m_sumFrames = 0; uint32_t m_shots = 0;
};

} // namespace pushmpc::artifacts
