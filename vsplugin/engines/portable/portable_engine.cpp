// pushmpc.Enhance, backend "portable": see portable_engine.h.
#include "portable_engine.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <future>
#include <thread>

namespace pmvs::portable {
using veyra::portable::Gray;
using veyra::portable::MotionEstimator;
using veyra::portable::MotionField;
using veyra::portable::Pyramid;

namespace {

double NowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// Interpolate.cpp's smoothstep and sampleBilinear, on one 16-bit plane (same operations in the same order)
inline float Smoothstep(float e0, float e1, float x)
{
    if (e1 <= e0) return x < e0 ? 0.f : 1.f;
    const float t = std::clamp((x - e0) / (e1 - e0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}
inline float Sample(const Plane& p, float x, float y)
{
    // NaN (never from a field, but cheap to rule out) goes to 0; std::isfinite is a library call on MSVC
    const float fxc = std::min(std::max(x == x ? x - 0.5f : 0.f, 0.f), float(p.w - 1));
    const float fyc = std::min(std::max(y == y ? y - 0.5f : 0.f, 0.f), float(p.h - 1));
    const int x0 = int(fxc), y0 = int(fyc);
    const int x1 = std::min(x0 + 1, p.w - 1), y1 = std::min(y0 + 1, p.h - 1);
    const float fx = fxc - float(x0), fy = fyc - float(y0);
    const uint16_t* r0 = p.px.data() + size_t(y0) * size_t(p.w);
    const uint16_t* r1 = p.px.data() + size_t(y1) * size_t(p.w);
    const float p00 = float(r0[x0]), p10 = float(r0[x1]), p01 = float(r1[x0]), p11 = float(r1[x1]);
    const float top = p00 + (p10 - p00) * fx;
    const float bottom = p01 + (p11 - p01) * fx;
    return top + (bottom - top) * fy;
}

// MotionField::sample, row by row: the column terms are the same on every row and the row terms on every pixel of a
// row, so each pixel only does the lerps (the same operations in the same order as sample())
struct FieldCols { std::vector<int> i0, i1; std::vector<float> fx; };
struct FieldRow { int j0 = 0, j1 = 0; float fy = 0; };
FieldCols ColsOf(const MotionField& f, int W)
{
    FieldCols c;
    if (f.empty()) return c;
    c.i0.resize(size_t(W)); c.i1.resize(size_t(W)); c.fx.resize(size_t(W));
    for (int x = 0; x < W; ++x) {
        const float gx = std::clamp((float(x) + 0.5f) / float(f.blockSize) - 0.5f, 0.0f, float(f.blocksX - 1));
        const int i0 = int(gx);
        c.i0[size_t(x)] = i0; c.i1[size_t(x)] = std::min(i0 + 1, f.blocksX - 1); c.fx[size_t(x)] = gx - float(i0);
    }
    return c;
}
FieldRow RowOf(const MotionField& f, float py)
{
    FieldRow r;
    if (f.empty()) return r;
    const float gy = std::clamp(py / float(f.blockSize) - 0.5f, 0.0f, float(f.blocksY - 1));
    r.j0 = int(gy); r.j1 = std::min(r.j0 + 1, f.blocksY - 1); r.fy = gy - float(r.j0);
    return r;
}
inline void SampleAt(const MotionField& f, const FieldCols& c, const FieldRow& r, int x, float& ox, float& oy)
{
    const size_t i0 = size_t(c.i0[size_t(x)]), i1 = size_t(c.i1[size_t(x)]), r0 = size_t(r.j0) * size_t(f.blocksX), r1 = size_t(r.j1) * size_t(f.blocksX);
    const float fx = c.fx[size_t(x)], fy = r.fy;
    auto lerp2 = [&](const std::vector<float>& v) {
        const float top = v[r0 + i0] + (v[r0 + i1] - v[r0 + i0]) * fx;
        const float bottom = v[r1 + i0] + (v[r1 + i1] - v[r1 + i0]) * fx;
        return top + (bottom - top) * fy;
    };
    ox = lerp2(f.vx); oy = lerp2(f.vy);
}

template <class F> void ParallelRows(int rows, int threads, F&& fn)
{
    threads = std::clamp(threads, 1, 64);
    if (threads == 1 || rows < threads * 8) { fn(0, rows); return; }
    std::vector<std::thread> pool;
    const int per = (rows + threads - 1) / threads;
    for (int k = 0; k < threads; ++k) {
        const int y0 = k * per, y1 = std::min(rows, y0 + per);
        if (y0 >= y1) break;
        pool.emplace_back(fn, y0, y1);
    }
    for (auto& th : pool) th.join();
}

void FillPicture(const YuvView& v, Picture& pic)
{
    for (int i = 0; i < 3; ++i) {
        const int w = i ? (v.width + 1) / 2 : v.width, h = i ? (v.height + 1) / 2 : v.height;
        Plane& p = pic.p[i];
        p.w = w; p.h = h; p.px.resize(size_t(w) * size_t(h));
        for (int y = 0; y < h; ++y) {
            const uint8_t* row = v.data[i] + ptrdiff_t(y) * v.stride[i];
            uint16_t* o = p.px.data() + size_t(y) * size_t(w);
            if (v.bits <= 8) for (int x = 0; x < w; ++x) o[x] = row[x];
            else std::memcpy(o, row, size_t(w) * 2);
        }
    }
    Gray g(v.width, v.height);
    const int shift = std::max(0, v.bits - 8);
    const Plane& y = pic.p[0];
    if (shift == 0) for (size_t i = 0; i < y.px.size(); ++i) g.px[i] = uint8_t(y.px[i]);
    else {
        const int half = 1 << (shift - 1), top = 255;
        for (size_t i = 0; i < y.px.size(); ++i) g.px[i] = uint8_t(std::min(top, (int(y.px[i]) + half) >> shift));
    }
    pic.pyramid = std::make_shared<const Pyramid>(Pyramid::of(std::move(g), 4));
}

} // namespace

void InterpolatePlanes(const Picture& a, const Picture& b, const MotionField& fwd, const MotionField& bwd, float t, int maxValue, Picture& out, int threads,
                       veyra::portable::InterpolateSettings s)
{
    for (int i = 0; i < 3; ++i) { out.p[i].w = a.p[i].w; out.p[i].h = a.p[i].h; out.p[i].px.resize(a.p[i].px.size()); }
    if (!std::isfinite(t)) t = 0;
    t = std::clamp(t, 0.f, 1.f);
    const Picture& nearer = t < 0.5f ? a : b;
    const bool fieldsOk = !fwd.empty() && !bwd.empty() && !fwd.sceneCut && !bwd.sceneCut && fwd.width == a.p[0].w && fwd.height == a.p[0].h
                       && bwd.width == a.p[0].w && bwd.height == a.p[0].h;
    if (!fieldsOk || t == 0.f || t == 1.f) {
        for (int i = 0; i < 3; ++i) out.p[i].px = (t == 1.f ? b : nearer).p[i].px;
        return;
    }
    const float scale = 255.f / float(maxValue), maxV = float(maxValue);
    const FieldCols colsF = ColsOf(fwd, a.p[0].w), colsB = ColsOf(bwd, a.p[0].w);
    // the two candidates at luma position (px, py): their luma samples, errors and weights. When the forward and the
    // reversed backward vector agree (most of a picture), the two candidates are the same: computed once, same result.
    struct Cand { float fx, fy, kx, ky, e1, e2, w1, w2, occlusion; bool firstBetter, single; float ya1, yb1, ya2, yb2; };
    auto cand = [&](int x, float py, const FieldRow& rf, const FieldRow& rb, Cand& c) {
        const float px = float(x) + 0.5f;
        SampleAt(fwd, colsF, rf, x, c.fx, c.fy);
        SampleAt(bwd, colsB, rb, x, c.kx, c.ky);
        c.kx = -c.kx; c.ky = -c.ky;
        c.single = std::abs(c.fx - c.kx) + std::abs(c.fy - c.ky) < 1e-4f;
        c.ya1 = Sample(a.p[0], px - t * c.fx, py - t * c.fy);
        c.yb1 = Sample(b.p[0], px + (1.f - t) * c.fx, py + (1.f - t) * c.fy);
        c.e1 = std::abs(c.ya1 - c.yb1) * scale;
        c.w1 = 1.f / ((c.e1 + 4.f) * (c.e1 + 4.f));
        if (c.single) { c.ya2 = c.ya1; c.yb2 = c.yb1; c.e2 = c.e1; c.w2 = c.w1; }
        else {
            c.ya2 = Sample(a.p[0], px - t * c.kx, py - t * c.ky);
            c.yb2 = Sample(b.p[0], px + (1.f - t) * c.kx, py + (1.f - t) * c.ky);
            c.e2 = std::abs(c.ya2 - c.yb2) * scale;
            c.w2 = 1.f / ((c.e2 + 4.f) * (c.e2 + 4.f));
        }
        c.occlusion = Smoothstep(s.errorLow, s.errorHigh, std::min(c.e1, c.e2));
        c.firstBetter = c.e1 <= c.e2;
    };
    // Live HUD Lock is not used here (no mask): the core's held-pixel term is 0
    auto blend = [&](const Cand& c, float a1, float b1, float a2, float b2) {
        const float nearSample = t < 0.5f ? (c.firstBetter ? a1 : a2) : (c.firstBetter ? b1 : b2);
        const float c1 = a1 + (b1 - a1) * t;
        float v = c1;
        if (!c.single) {
            const float c2 = a2 + (b2 - a2) * t;
            v = (c1 * c.w1 + c2 * c.w2) / (c.w1 + c.w2);
        }
        v += (nearSample - v) * c.occlusion;
        return uint16_t(std::clamp(v + 0.5f, 0.f, maxV));
    };
    // chroma row cy with luma rows 2 cy and 2 cy + 1: a chroma sample takes the candidates (vectors, errors, weights) of
    // the top-left luma pixel of its 2x2 block (the field is one vector per 16 px: half a pixel moves it by nothing)
    const int W = a.p[0].w, H = a.p[0].h, CW = a.p[1].w;
    ParallelRows(a.p[1].h, threads, [&](int c0, int c1) {
        std::vector<Cand> stash(size_t(CW) + 1);
        for (int cy = c0; cy < c1; ++cy) {
            for (int y = 2 * cy; y < std::min(H, 2 * cy + 2); ++y) {
                const float py = float(y) + 0.5f;
                const FieldRow rf = RowOf(fwd, py), rb = RowOf(bwd, py);
                uint16_t* o = out.p[0].px.data() + size_t(y) * size_t(W);
                for (int x = 0; x < W; ++x) {
                    Cand& c = stash[size_t(x / 2)];
                    Cand tmp;
                    Cand& use = (y == 2 * cy && (x & 1) == 0) ? c : tmp;
                    cand(x, py, rf, rb, use);
                    o[x] = blend(use, use.ya1, use.yb1, use.ya2, use.yb2);
                }
            }
            uint16_t* ou = out.p[1].px.data() + size_t(cy) * size_t(CW);
            uint16_t* ov = out.p[2].px.data() + size_t(cy) * size_t(CW);
            for (int cx = 0; cx < CW; ++cx) {
                const Cand& c = stash[size_t(cx)];
                const float px = float(cx) + 0.5f, py = float(cy) + 0.5f;
                const float ax1 = px - t * c.fx * 0.5f, ay1 = py - t * c.fy * 0.5f, bx1 = px + (1.f - t) * c.fx * 0.5f, by1 = py + (1.f - t) * c.fy * 0.5f;
                const float ax2 = px - t * c.kx * 0.5f, ay2 = py - t * c.ky * 0.5f, bx2 = px + (1.f - t) * c.kx * 0.5f, by2 = py + (1.f - t) * c.ky * 0.5f;
                for (int pl = 1; pl <= 2; ++pl) {
                    const float a1 = Sample(a.p[pl], ax1, ay1), b1 = Sample(b.p[pl], bx1, by1);
                    const float a2 = c.single ? a1 : Sample(a.p[pl], ax2, ay2), b2 = c.single ? b1 : Sample(b.p[pl], bx2, by2);
                    (pl == 1 ? ou : ov)[cx] = blend(c, a1, b1, a2, b2);
                }
            }
        }
    });
}

bool Engine::Configure(const Config& c, std::string& err)
{
    if (c.width < 16 || c.height < 16 || c.bits < 8 || c.bits > 16) { err = "portable engine: the picture must be at least 16x16, 8..16 bits"; return false; }
    m_cfg = c;
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    m_threads = c.threads > 0 ? c.threads : int(std::min(16u, hw));
    m_ms = {};
    m_ms.finestLevel = c.quality <= 0 ? 2 : 1;   // as PushFPS's Engine
    Forget();
    m_stats = {};
    return true;
}

void Engine::Forget()
{
    m_haveCur = m_havePair = m_cut = false;
    m_fwd = {}; m_bwd = {};
}

bool Engine::Push(const YuvView& v, bool contiguous, std::string& err)
{
    if (v.width != m_cfg.width || v.height != m_cfg.height || v.bits != m_cfg.bits) { err = "portable engine: the frame's size or depth changed"; return false; }
    if (!contiguous) Forget();
    if (m_haveCur) std::swap(m_prev, m_cur);
    FillPicture(v, m_cur);
    m_stats.pushed++;
    if (!m_haveCur) { m_haveCur = true; m_havePair = false; return true; }
    const double t0 = NowMs();
    const MotionEstimator est(m_ms);
    const MotionField prevF = m_fwd, prevB = m_bwd;
    auto back = std::async(std::launch::async, [&] { return est.estimate(*m_cur.pyramid, *m_prev.pyramid, &prevB); });
    m_fwd = est.estimate(*m_prev.pyramid, *m_cur.pyramid, &prevF);
    m_bwd = back.get();
    m_cut = m_fwd.sceneCut || m_bwd.sceneCut;
    m_havePair = true;
    m_stats.pairs++;
    if (m_cut) m_stats.cuts++;
    const double ms = NowMs() - t0;
    m_stats.meMs = m_stats.pairs == 1 ? ms : m_stats.meMs * 0.9 + ms * 0.1;
    return true;
}

void Engine::Write(const Picture& pic, const YuvOut& out) const
{
    for (int i = 0; i < 3; ++i) {
        const Plane& p = pic.p[i];
        for (int y = 0; y < p.h; ++y) {
            const uint16_t* r = p.px.data() + size_t(y) * size_t(p.w);
            uint8_t* o = out.data[i] + ptrdiff_t(y) * out.stride[i];
            if (m_cfg.bits <= 8) for (int x = 0; x < p.w; ++x) o[x] = uint8_t(r[x]);
            else std::memcpy(o, r, size_t(p.w) * 2);
        }
    }
}

void Engine::Generate(float t, const YuvOut& out)
{
    if (!m_havePair) { Current(out); return; }
    const double t0 = NowMs();
    Picture tmp;
    InterpolatePlanes(m_prev, m_cur, m_fwd, m_bwd, t, (1 << m_cfg.bits) - 1, tmp, m_threads);
    Write(tmp, out);
    m_stats.generated++;
    const double ms = NowMs() - t0;
    m_stats.genMs = m_stats.generated == 1 ? ms : m_stats.genMs * 0.9 + ms * 0.1;
}

void Engine::Current(const YuvOut& out) const { Write(m_cur, out); }

Calibration Calibrate(int width, int height, int bits, int threads, int quality)
{
    Calibration cal;
    Engine e;
    std::string err;
    Config c; c.width = width; c.height = height; c.bits = bits; c.threads = threads; c.quality = quality;
    if (!e.Configure(c, err)) return cal;   // only for the thread count
    cal.threads = e.Threads();
    const int bps = bits > 8 ? 2 : 1, cw = (width + 1) / 2, ch = (height + 1) / 2;
    std::vector<uint8_t> planes[2][3];
    for (int f = 0; f < 2; ++f) {
        for (int i = 0; i < 3; ++i) {
            const int w = i ? cw : width, h = i ? ch : height;
            planes[f][i].assign(size_t(w) * size_t(h) * size_t(bps), 0);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    // a diagonal texture moving 6 px a frame: real search work on every block
                    const int xs = x + f * (i ? 3 : 6);
                    int v = i ? 128 : ((xs * 7 + y * 3) & 255) ^ ((xs / 16 + y / 16) & 1 ? 0x55 : 0);
                    v <<= (bits - 8);
                    if (bps == 1) planes[f][i][size_t(y) * size_t(w) + size_t(x)] = uint8_t(v);
                    else std::memcpy(&planes[f][i][(size_t(y) * size_t(w) + size_t(x)) * 2], &v, 2);
                }
        }
    }
    YuvView v[2];
    for (int f = 0; f < 2; ++f) {
        v[f].width = width; v[f].height = height; v[f].bits = bits;
        for (int i = 0; i < 3; ++i) { v[f].data[i] = planes[f][i].data(); v[f].stride[i] = ptrdiff_t(i ? cw : width) * bps; }
    }
    // timed like the engine: both motion directions in parallel, then one generated frame (never a cut: the
    // interpolation always runs) and its write into an output frame
    Picture pa, pb, out;
    FillPicture(v[0], pa); FillPicture(v[1], pb);
    veyra::portable::MotionSettings ms; ms.finestLevel = quality <= 0 ? 2 : 1;
    const MotionEstimator est(ms);
    const double t0 = NowMs();
    auto back = std::async(std::launch::async, [&] { return est.estimate(*pb.pyramid, *pa.pyramid, nullptr); });
    MotionField f = est.estimate(*pa.pyramid, *pb.pyramid, nullptr);
    MotionField k = back.get();
    const double t1 = NowMs();
    f.sceneCut = k.sceneCut = false;
    InterpolatePlanes(pa, pb, f, k, 0.5f, (1 << bits) - 1, out, e.Threads());
    std::vector<uint8_t> sink(size_t(width) * size_t(height) * size_t(bps));
    for (size_t i = 0; i < out.p[0].px.size(); ++i) sink[i * size_t(bps)] = uint8_t(out.p[0].px[i]);
    cal.meMs = t1 - t0; cal.genMs = NowMs() - t1;
    return cal;
}

} // namespace pmvs::portable
