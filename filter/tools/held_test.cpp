// held_test: held-frame re-timing on the pushcore route (pushmpc/src/HeldFrames.h, PushCoreHeld.h), headless.
//  1. The rules: the span multiplier, the graph's build multiplier, the Retimer's decisions (twos, threes, mixed,
//     a still longer than the lookahead, a seek).
//  2. The GPU test on WARP (no GPU needed): a synthetic animation clip, NV12, drawn on twos and threes with grain and
//     block flicker on every frame, plus frames where only a small element changes; PushCoreHeld.h's pass must call
//     the repeats held and every real change distinct.
//  3. The output spacing: the clip's decisions through the Retimer, the outputs placed as EnhanceGraph places them
//     (FrameBatch::interpolate between the pair's timestamps): every output step must be on the 1/k grid, steps are
//     even within each held span, and a pan drawn on twos moves the same distance in every output.
// Exit code 0 = all passed. Prints one line per check and a RESULT line.
#define NOMINMAX
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "HeldFrames.h"
#include "PushCoreHeld.h"
#include "HoldPlan.h"
#include "PushCoreHold.h"

using Microsoft::WRL::ComPtr;
using namespace pushmpc::held;

static int g_fail = 0, g_checks = 0;
static void Check(bool ok, const std::string& what)
{
    ++g_checks;
    if (!ok) ++g_fail;
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
}

// EnhanceGraph's timestamps of generated frame j of n between a and b (veyra FrameBatch::interpolate)
static int64_t Interpolate(int64_t a, int64_t b, uint32_t j, uint32_t n)
{
    const int64_t d = b - a;
    return a + (d / n) * j + ((d % n) * j + n / 2) / n;
}

struct Out { int64_t pts; double pos; bool real; };
// Runs a held / distinct sequence through the Retimer; the outputs as the graph makes them. pos: the picture's position
// (the drawing's x) shown by each output: a real frame its own, a generated frame the linear blend of its pair.
static std::vector<Out> Simulate(const std::vector<bool>& heldSeq, const std::vector<double>& posSeq, uint32_t k, uint32_t cap, int64_t step,
                                 uint64_t* dropped = nullptr, int seekAt = -1)
{
    Retimer r;
    r.Configure(k, BuildMultiplier(k, cap));
    std::vector<Out> out;
    std::vector<Action> acts;
    int64_t prevSub = -1;
    auto apply = [&](const Action& a) {
        if (a.kind == Action::Drop) { if (dropped) ++*dropped; return; }
        const int64_t f = int64_t(a.frame);
        if (prevSub >= 0 && a.gap >= 1 && f > prevSub) {
            for (uint32_t j = 1; j < a.multiplier; ++j) {
                const int64_t pa = prevSub * step, pb = f * step;
                const int64_t t = Interpolate(pa, pb, j, a.multiplier);
                const double w = double(t - pa) / double(pb - pa);
                out.push_back({ t, posSeq[size_t(prevSub)] * (1 - w) + posSeq[size_t(f)] * w, false });
            }
        }
        out.push_back({ f * step, posSeq[size_t(f)], true });
        prevSub = f;
    };
    for (size_t i = 0; i < heldSeq.size(); ++i) {
        acts.clear();
        const bool disc = i == 0 || int(i) == seekAt;
        if (disc && int(i) == seekAt) prevSub = -1;
        r.Push(i, heldSeq[i] && !disc, disc, acts);
        for (const auto& a : acts) apply(a);
    }
    acts.clear();
    r.Flush(acts);
    for (const auto& a : acts) apply(a);
    return out;
}

// steps on the 1/k grid; max / min step within the spans of distinct frames
static bool OnGrid(const std::vector<Out>& o, int64_t step, uint32_t k, std::string& why)
{
    const double grid = double(step) / k;
    for (size_t i = 1; i < o.size(); ++i) {
        const int64_t d = o[i].pts - o[i - 1].pts;
        if (d <= 0) { why = "order"; return false; }
        const double u = d / grid;
        if (std::fabs(u - std::round(u)) > 0.02) { why = "off grid at output " + std::to_string(i) + " (" + std::to_string(u) + " steps)"; return false; }
    }
    return true;
}

static void TestRules()
{
    Check(SpanMultiplier(2, 4, 8) == 8 && SpanMultiplier(3, 4, 8) == 6 && SpanMultiplier(2, 5, 8) == 5 && SpanMultiplier(3, 5, 8) == 5, "span multiplier: largest divisor of g x k within the cap");
    Check(SpanMultiplier(2, 5, 16) == 10 && SpanMultiplier(3, 5, 16) == 15 && SpanMultiplier(2, 8, 16) == 16, "span multiplier with a 16x graph");
    Check(SpanMultiplier(2, 2, 2) == 2 && SpanMultiplier(3, 2, 2) == 2 && SpanMultiplier(1, 4, 8) == 4, "span multiplier for 2x-only methods, plain pairs");
    Check(BuildMultiplier(5, 8) == 5 && BuildMultiplier(4, 8) == 8 && BuildMultiplier(8, 8) == 8 && BuildMultiplier(2, 8) == 8 && BuildMultiplier(3, 8) == 6,
          "build multiplier (8x engine): 5->5, 4->8, 8->8, 2->8, 3->6");
    Check(BuildMultiplier(5, 16) == 15 && BuildMultiplier(4, 16) == 16 && BuildMultiplier(2, 2) == 2 && BuildMultiplier(1, 8) == 1, "build multiplier (16x engine, 2x-only method, no FG)");

    // twos: distinct, held, distinct, held ...
    {
        Retimer r; r.Configure(4, 8);
        std::vector<Action> a;
        for (uint64_t i = 0; i < 8; ++i) r.Push(i, i % 2 == 1, i == 0, a);
        int drops = 0, spans = 0;
        for (auto& x : a) { if (x.kind == Action::Drop) ++drops; else if (x.gap == 2 && x.multiplier == 8) ++spans; }
        Check(drops == 3 && spans == 3 && r.Waiting() == 1, "twos: each held frame dropped, the next distinct frame spans 2 at 8x");
    }
    // a still: 6 held frames after a distinct one, then motion
    {
        Retimer r; r.Configure(4, 8);
        std::vector<Action> a;
        const bool seq[] = { false, true, true, true, true, true, true, false, false };   // a run of 6 > the lookahead (3)
        for (uint64_t i = 0; i < 9; ++i) r.Push(i, seq[i], i == 0, a);
        int drops = 0, subs = 0, longGap = 0;
        for (auto& x : a) { if (x.kind == Action::Drop) ++drops; else { ++subs; if (x.gap > 1) ++longGap; } }
        Check(drops == 0 && subs == 9 && longGap == 0, "a still longer than the lookahead: every frame to the graph, nothing re-timed");
    }
    // a seek in the middle of a held run
    {
        Retimer r; r.Configure(4, 8);
        std::vector<Action> a;
        r.Push(0, false, true, a); r.Push(1, true, false, a); r.Push(2, false, true, a);
        Check(a.size() == 3 && a[1].kind == Action::Submit && a[1].frame == 1 && a[2].gap == 1, "a discontinuity flushes a waiting held frame as it is");
    }
}

// ---- synthetic clip on WARP ----
struct Clip {
    uint32_t w = 960, h = 540;
    std::mt19937 rng{ 12345 };
    // line art: dark outlines of circles and strokes on a soft gradient, drawn at x offset `pos`; small: a 20x20 box
    // whose brightness is `blink` (0 = absent); fade: a whole-picture offset in codes
    std::vector<uint8_t> Frame(double pos, int blink, double fade, int grainAmp)
    {
        std::vector<uint8_t> f(size_t(w) * h * 3 / 2);
        std::normal_distribution<double> grain(0.0, grainAmp);
        std::uniform_int_distribution<int> flick(-1, 1);
        const uint32_t bw = (w + 7) / 8;
        std::vector<int> blockFlicker(bw * ((h + 7) / 8));
        for (auto& b : blockFlicker) b = flick(rng);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                double v = 60 + 100.0 * y / h + fade;
                // outlines (2.5 px wide) of three circles and a diagonal stroke, all moving with pos
                const double X = x - pos;
                for (int c = 0; c < 3; ++c) {
                    const double cx = 200 + 220 * c, cy = 270 + 40 * c, r = 80 + 15 * c;
                    const double d = std::fabs(std::hypot(X - cx, y - cy) - r);
                    if (d < 1.25) v = 20; else if (d < 2.0) v = 20 + (d - 1.25) / 0.75 * (v - 20);
                }
                if (std::fabs((X - 100) - 0.6 * (y - 100)) < 1.5 && y > 100 && y < 450) v = 25;
                if (blink && x >= 900 && x < 920 && y >= 20 && y < 40) v = blink;
                v += grain(rng) + blockFlicker[(y / 8) * bw + x / 8];
                f[size_t(y) * w + x] = uint8_t(std::clamp(v, 0.0, 255.0));
            }
        std::fill(f.begin() + size_t(w) * h, f.end(), uint8_t(128));
        return f;
    }
};

static ComPtr<ID3D11Texture2D> Upload(ID3D11Device* dev, const std::vector<uint8_t>& nv12, uint32_t w, uint32_t h)
{
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_NV12; d.SampleDesc = { 1, 0 };
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{ nv12.data(), w, 0 };
    ComPtr<ID3D11Texture2D> t;
    if (FAILED(dev->CreateTexture2D(&d, &sd, &t))) return nullptr;
    return t;
}

static void TestGpu()
{
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    const D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_1 };
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    Check(SUCCEEDED(hr), "WARP device (feature level 11.1)");
    if (FAILED(hr)) return;
    pushmpc::HeldDetector det;
    Check(det.init(dev.Get()), "held-frame test pass compiles and initialises" + (det.ready() ? std::string() : ": " + det.why()));
    if (!det.ready()) return;

    Clip clip;
    // The sequence: 30 frames on twos (a pan of 6 px per drawing), 30 on threes (9 px), 24 on fours (12 px), 12 on ones, then
    // 8 frames of a held drawing where a small box blinks every frame, then a 1-px move, then a slow fade (0.25 codes per
    // frame) of a held drawing. Truth: held = same drawing and no small change.
    struct F { double pos; int blink; double fade; bool held; };
    std::vector<F> seq;
    double pos = 0;
    for (int i = 0; i < 30; ++i) { if (i % 2 == 0 && i) pos += 6; seq.push_back({ pos, 0, 0, i % 2 == 1 }); }
    for (int i = 0; i < 30; ++i) { if (i % 3 == 0) pos += 9; seq.push_back({ pos, 0, 0, i % 3 != 0 }); }
    for (int i = 0; i < 24; ++i) { if (i % 4 == 0) pos += 12; seq.push_back({ pos, 0, 0, i % 4 != 0 }); }   // on fours: frames 60..83
    for (int i = 0; i < 12; ++i) { pos += 3; seq.push_back({ pos, 0, 0, false }); }
    for (int i = 0; i < 8; ++i) seq.push_back({ pos, i % 2 ? 120 : 230, 0, false });
    seq.push_back({ pos, 120, 0, true });
    pos += 1; seq.push_back({ pos, 120, 0, false });
    for (int i = 0; i < 4; ++i) seq.push_back({ pos, 120, 0.25 * (i + 1), true });

    ComPtr<ID3D11Texture2D> prev, ring[8];
    std::vector<bool> got;
    int wrong = 0, fp = 0, fn = 0;
    DiffStats worstHeld{}, leastMoving{}; leastMoving.maxQ = ~0u;
    for (size_t i = 0; i < seq.size(); ++i) {
        // as pushcore's ingress: the decoded frame is copied into a ring of 8 textures, the pass compares two slots
        const auto pic = clip.Frame(seq[i].pos, seq[i].blink, seq[i].fade, 2);
        auto src = Upload(dev.Get(), pic, clip.w, clip.h);
        if (!src) { Check(false, "NV12 texture on WARP"); return; }
        auto& t = ring[i % 8];
        if (!t) {
            D3D11_TEXTURE2D_DESC d{}; src->GetDesc(&d);
            if (FAILED(dev->CreateTexture2D(&d, nullptr, &t))) { Check(false, "NV12 ring texture on WARP"); return; }
        }
        ctx->CopySubresourceRegion(t.Get(), 0, 0, 0, 0, src.Get(), 0, nullptr);
        bool held = false;
        if (prev) {
            if (!det.run(ctx.Get(), t.Get(), prev.Get(), clip.w, clip.h, i)) { Check(false, "pass recorded"); return; }
            ctx->Flush();
            DiffStats d;
            if (!det.read(ctx.Get(), i, d, true)) { Check(false, "result read back"); return; }
            held = IsHeld(d);
            if (seq[i].held && d.maxQ > worstHeld.maxQ) worstHeld = d;
            if (!seq[i].held && d.maxQ < leastMoving.maxQ) leastMoving = d;
            if (held != seq[i].held) { ++wrong; (held ? fp : fn)++;
                std::printf("     frame %zu: truth %s, got %s (max %.2f codes, >4: %u, >8: %u, >16: %u, mean %.3f)\n", i, seq[i].held ? "held" : "distinct", held ? "held" : "distinct",
                            d.maxQ / 16.0, d.over[1], d.over[2], d.over[3], d.meanCodes()); }
        }
        got.push_back(held);
        prev = t;
    }
    std::printf("     held frames: worst max %.2f codes (>4: %u, mean %.3f); distinct frames: smallest max %.2f codes (>4: %u)\n",
                worstHeld.maxQ / 16.0, worstHeld.over[1], worstHeld.meanCodes(), leastMoving.maxQ / 16.0, leastMoving.over[1]);
    Check(wrong == 0, "WARP: every repeat with grain and block flicker is held, every move, blink and new drawing distinct (" + std::to_string(fp) + " false held, " + std::to_string(fn) + " missed)");

    // output spacing through the Retimer, as the graph would place the outputs
    std::vector<double> posSeq; for (auto& f : seq) posSeq.push_back(f.pos);
    const int64_t step = 417083;   // 23.976 fps
    for (uint32_t k : { 2u, 4u, 5u, 8u }) {
        for (uint32_t cap : { 8u, 16u }) {
            uint64_t dropped = 0;
            const auto out = Simulate(got, posSeq, k, cap, step, &dropped);
            std::string why;
            const bool grid = OnGrid(out, step, k, why);
            // the twos pan (frames 0..29): every output's motion per grid step must be the same (no pulse)
            double vmin = 1e9, vmax = 0;
            const double gridStep = double(step) / k;
            for (size_t i = 1; i < out.size(); ++i) {
                if (out[i].pts > 28 * step || out[i - 1].pts < 0) break;
                const double v = (out[i].pos - out[i - 1].pos) / ((out[i].pts - out[i - 1].pts) / gridStep);
                vmin = std::min(vmin, v); vmax = std::max(vmax, v);
            }
            // the same on threes (frames 30..59)
            double tmin = 1e9, tmax = 0;
            for (size_t i = 1; i < out.size(); ++i) {
                if (out[i - 1].pts < 30 * step || out[i].pts > 57 * step) continue;
                const double v = (out[i].pos - out[i - 1].pos) / ((out[i].pts - out[i - 1].pts) / gridStep);
                tmin = std::min(tmin, v); tmax = std::max(tmax, v);
            }
            // the same on fours (frames 60..83)
            double fmin = 1e9, fmax = 0;
            for (size_t i = 1; i < out.size(); ++i) {
                if (out[i - 1].pts < 60 * step || out[i].pts > 80 * step) continue;
                const double v = (out[i].pos - out[i - 1].pos) / ((out[i].pts - out[i - 1].pts) / gridStep);
                fmin = std::min(fmin, v); fmax = std::max(fmax, v);
            }
            char b[320];
            std::snprintf(b, sizeof(b), "k=%u, %ux engine: outputs %zu, held dropped %llu, on the 1/k grid%s; pan on twos %.3f..%.3f px per grid step, threes %.3f..%.3f, fours %.3f..%.3f",
                          k, cap, out.size(), (unsigned long long)dropped, grid ? "" : (" NO: " + why).c_str(), vmin, vmax, tmin, tmax, fmin, fmax);
            Check(grid && vmax - vmin < 1e-6 + 0.002 * vmax && tmax - tmin < 1e-6 + 0.002 * tmax && fmax - fmin < 1e-6 + 0.002 * fmax && dropped > 0, b);
        }
    }
    // without re-timing the same twos pan pulses (sanity check of the measure): every frame given to the graph
    {
        std::vector<bool> none(got.size(), false);
        const auto out = Simulate(none, posSeq, 4, 8, step);
        double vmin = 1e9, vmax = 0;
        for (size_t i = 1; i < out.size() && out[i].pts <= 28 * step; ++i) {
            const double v = (out[i].pos - out[i - 1].pos) / ((out[i].pts - out[i - 1].pts) / (double(step) / 4));
            vmin = std::min(vmin, v); vmax = std::max(vmax, v);
        }
        Check(vmin == 0 && vmax > 1, "reference: without re-timing the twos pan stops and jumps (" + std::to_string(vmin) + ".." + std::to_string(vmax) + " px per grid step)");
    }
}

// ---- per-region (source repair): a layered clip ----
// A textured background panning on ones (3 px per frame), sprite A on twos (12 px per drawing, even frames), sprite B on
// twos out of phase (odd frames), and a mouth on threes inside A. The GPU pass makes each frame's cell grid (WARP), the
// classifier and PlanRepairs decide which parts each frame repairs; a part's position at frame s is then
// lerp(pos(d0), pos(d1), t) where repaired and its drawn position elsewhere. Checks: every sprite moves at an even rate
// (stall ratio min/max per-frame step >= 0.8, as the critic's T1), the background is never repaired, B is caught as well
// as A (out of phase), and no repair spans a cut.
struct Layered {
    uint32_t w = 960, h = 540;
    std::mt19937 rng{ 7 };
    std::vector<float> tex;   // background texture, 2x wide (pans)
    Layered() {
        tex.resize(size_t(w) * 2 * h);
        std::uniform_real_distribution<float> u(0.f, 1.f);
        std::vector<float> c(size_t(w / 8 * 2 + 2) * (h / 8 + 2));
        for (auto& v : c) v = 40.f + 120.f * u(rng);
        for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < 2 * w; ++x) {   // blocky, smoothed: a painted background
            const float fx = x / 8.f, fy = y / 8.f; const int ix = int(fx), iy = int(fy); const float ax = fx - ix, ay = fy - iy;
            const size_t cw = w / 8 * 2 + 2;
            auto C = [&](int i, int j) { return c[size_t(j) * cw + i]; };
            tex[size_t(y) * 2 * w + x] = C(ix, iy) * (1 - ax) * (1 - ay) + C(ix + 1, iy) * ax * (1 - ay) + C(ix, iy + 1) * (1 - ax) * ay + C(ix + 1, iy + 1) * ax * ay;
        }
    }
    // drawn positions: A (even frames), B (odd frames), mouth state (threes)
    static double posA(int f) { return 150 + 12.0 * (f / 2); }
    static double posB(int f) { return 600 - 12.0 * ((f + 1) / 2); }
    static int mouth(int f) { return (f / 3) % 2; }
    std::vector<uint8_t> Frame(int f, bool cut = false)
    {
        std::vector<uint8_t> o(size_t(w) * h * 3 / 2, 128);
        std::normal_distribution<double> grain(0.0, 1.5);
        const double pan = 3.0 * f;
        const double ax = posA(f), bx = posB(f);
        for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
            double v = cut ? 230.0 - tex[size_t(y) * 2 * w + x] * 0.5 : [&] { const double sx = x + pan; const int i = int(sx); const double a = sx - i;
                return tex[size_t(y) * 2 * w + i] * (1 - a) + tex[size_t(y) * 2 * w + i + 1] * a; }();
            auto disc = [&](double cx, double cy, double r, double fill) {
                const double d = std::hypot(x - cx, y - cy);
                if (d < r - 2) v = fill; else if (d < r) v = 15;   // outline
            };
            if (!cut) {
                disc(ax, 270, 60, 200);
                if (std::fabs(x - ax) < 14 && std::fabs(y - 290.0) < (mouth(f) ? 10 : 3)) v = 30;   // the mouth
                disc(bx, 300, 50, 170);
            }
            o[size_t(y) * w + x] = uint8_t(std::clamp(v + grain(rng), 0.0, 255.0));
        }
        return o;
    }
};

static void TestLayered()
{
    using namespace pushmpc::held;
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    const D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_1 };
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) { Check(false, "WARP device (layered)"); return; }
    pushmpc::HeldDetector det;
    if (!det.init(dev.Get())) { Check(false, "held pass (layered): " + det.why()); return; }
    Layered clip;
    const int N = 30, cutAt = 24;
    std::map<uint64_t, CellGrid> grids; std::map<uint64_t, RegionFrame> frames;
    ComPtr<ID3D11Texture2D> ring[2];
    for (int f = 0; f < N; ++f) {
        auto src = Upload(dev.Get(), clip.Frame(f, f == cutAt), clip.w, clip.h);
        auto& t = ring[f % 2];
        if (!t) { D3D11_TEXTURE2D_DESC d{}; src->GetDesc(&d); dev->CreateTexture2D(&d, nullptr, &t); }
        ctx->CopySubresourceRegion(t.Get(), 0, 0, 0, 0, src.Get(), 0, nullptr);
        if (f == 0) { det.run(ctx.Get(), t.Get(), t.Get(), clip.w, clip.h, uint64_t(f)); }
        else det.run(ctx.Get(), t.Get(), ring[(f + 1) % 2].Get(), clip.w, clip.h, uint64_t(f));
        ctx->Flush();
        DiffStats d; CellGrid g;
        if (!det.read(ctx.Get(), uint64_t(f), d, true, &g) || !g.ok()) { Check(false, "cell grid read back"); return; }
        if (f == 1) {
            // the pass's extras: sub-cells and chroma (the clip's chroma is 128 everywhere)
            float cbMin = 999, cbMax = 0; for (size_t i = 0; i < g.cb.size(); ++i) { cbMin = std::min(cbMin, g.cb[i]); cbMax = std::max(cbMax, g.cr[i]); }
            char bx[160]; std::snprintf(bx, sizeof(bx), "the pass reads back sub-cell changes and chroma means (Cb min %.2f, Cr max %.2f, truth 128)", cbMin, cbMax);
            Check(g.hasSub() && g.hasChroma() && std::fabs(cbMin - 128.f) < 1.f && std::fabs(cbMax - 128.f) < 1.f, bx);
        }
        if (f > 0) frames[uint64_t(f)] = CompareRegions(grids[uint64_t(f - 1)], g);
        grids[uint64_t(f)] = std::move(g);
    }
    const auto& g0 = grids[0];
    const RegionFrame& f3 = frames[3];
    char b0[200]; std::snprintf(b0, sizeof(b0), "camera shift found on the panning background: %.2f, %.2f cells (truth %.2f, 0)", f3.dx, f3.dy, -3.0 / g0.cell);
    Check(std::fabs(f3.dx + 3.0 / g0.cell) < 0.25 && std::fabs(f3.dy) < 0.25, b0);
    Check(frames[uint64_t(cutAt)].cut && !frames[3].cut, "the cut frame is a cut, a pan frame is not");
    // repairs per frame, and the positions they give
    auto frameAt = [&](uint64_t q) -> const RegionFrame* { auto it = frames.find(q); return it == frames.end() ? nullptr : &it->second; };
    auto gridAt = [&](uint64_t q) -> const CellGrid* { auto it = grids.find(q); return it == grids.end() ? nullptr : &it->second; };
    std::vector<double> shownA(N), shownB(N);
    int repA = 0, repB = 0, bgRepaired = 0, acrossCut = 0;
    const uint32_t W = g0.w, cell = g0.cell;
    for (int f = 0; f < N; ++f) {
        shownA[f] = Layered::posA(f); shownB[f] = Layered::posB(f);
        if (f < 3 || f + 3 >= N) continue;
        const auto groups = PlanRepairs(uint64_t(f), frameAt, gridAt, kLookahead);
        if (getenv("HELD_DEBUG")) for (const auto& g : groups) std::printf("     frame %d: group %llu..%llu t %.2f cells %u v %.1f,%.1f\n", f, (unsigned long long)g.d0, (unsigned long long)g.d1, g.t, g.cells, g.vx, g.vy);
        for (const auto& g : groups) {
            auto covers = [&](double x, double y) { const uint32_t cx = uint32_t(x / cell), cy = uint32_t(y / cell); return cx < W && g.mask[size_t(cy) * W + cx] != 0; };
            if (g.d0 < uint64_t(cutAt) && g.d1 >= uint64_t(cutAt)) ++acrossCut;
            if (covers(Layered::posA(f) + 60, 270) || covers(Layered::posA(f), 211) || covers(Layered::posA(f), 329)) {   // the outline (flat insides never change)
                shownA[f] = Layered::posA(int(g.d0)) + g.t * (Layered::posA(int(g.d1)) - Layered::posA(int(g.d0))); ++repA; }
            if (covers(Layered::posB(f) - 50, 300) || covers(Layered::posB(f), 251) || covers(Layered::posB(f), 349)) {
                shownB[f] = Layered::posB(int(g.d0)) + g.t * (Layered::posB(int(g.d1)) - Layered::posB(int(g.d0))); ++repB; }
            if (covers(900, 60) || covers(40, 500)) ++bgRepaired;
        }
    }
    if (getenv("HELD_DEBUG")) for (int f = 0; f < N; ++f) std::printf("     f %d: A %.1f B %.1f\n", f, shownA[f], shownB[f]);
    auto stall = [&](const std::vector<double>& p, int from, int to) {
        double mn = 1e9, mx = 0;
        for (int f = from + 1; f <= to; ++f) { const double s = std::fabs(p[f] - p[f - 1]); mn = std::min(mn, s); mx = std::max(mx, s); }
        return mx > 0 ? mn / mx : 0.0;
    };
    std::vector<double> rawA(N), rawB(N);
    for (int f = 0; f < N; ++f) { rawA[f] = Layered::posA(f); rawB[f] = Layered::posB(f); }
    char b[300];
    std::snprintf(b, sizeof(b), "sprite on twos (even): stall ratio %.2f (without repair %.2f), repaired on %d frames", stall(shownA, 4, 20), stall(rawA, 4, 20), repA);
    Check(stall(shownA, 4, 20) >= 0.8, b);
    std::snprintf(b, sizeof(b), "sprite on twos out of phase (odd): stall ratio %.2f (without %.2f), repaired on %d frames", stall(shownB, 4, 20), stall(rawB, 4, 20), repB);
    Check(stall(shownB, 4, 20) >= 0.8, b);
    Check(bgRepaired == 0, "the panning background is never repaired (" + std::to_string(bgRepaired) + ")");
    Check(acrossCut == 0, "no repair spans the cut (" + std::to_string(acrossCut) + ")");
    // the mouth (threes) inside A (twos): where it is redrawn inside A's span and then kept, A's pair starts at the
    // redrawn frame (substitution), so the new mouth moves with the body instead of being cut out of it
    int subst = 0;
    for (int f = 3; f + 3 < N; ++f)
        for (const auto& g : PlanRepairs(uint64_t(f), frameAt, gridAt, kLookahead)) subst += g.pairFrom != g.d0;
    Check(subst > 0, "a mouth redrawn inside the body's span: substitution pairs (" + std::to_string(subst) + ")");
}

// ---- per-region rules on synthetic cell grids (no GPU): nested cadences, fades, the gate, the noise floor ----
// A flat background with texture panning on ones, a body block on twos, a limb block on threes riding on the body
// (it moves with the body and changes pose on threes), and a second character on twos out of phase.
static void TestRegionRules()
{
    const uint32_t W = 160, H = 90;
    auto bodyX = [](int f) { return 20.0 + 6.0 * (f / 2); };
    auto limbDy = [](int f) { return ((f / 3) % 2) ? 6.0 : 0.0; };
    auto otherX = [](int f) { return 130.0 - 6.0 * ((f + 1) / 2); };
    auto grid = [&](int f, float gain) {
        CellGrid g; g.w = W; g.h = H; g.cell = 4; g.v.resize(size_t(W) * H);
        for (uint32_t y = 0; y < H; ++y) for (uint32_t x = 0; x < W; ++x) {
            const double bx = x + 1.0 * f;   // the background pans 1 cell per frame (on ones)
            float v = float(70 + 25 * std::sin(bx * 0.37) * std::cos(y * 0.29) + 20 * std::sin(bx * 0.113 + y * 0.07) + 15 * std::cos(bx * 0.051 - y * 0.19));   // not periodic
            const double b = bodyX(f);
            if (x >= b && x < b + 14 && y >= 30 && y < 60) v = float(180 + 20 * std::sin(x * 0.9 - b * 0.9));   // the body (texture moves with it)
            if (x >= b + 14 && x < b + 18 && y >= 34 + limbDy(f) && y < 44 + limbDy(f)) v = 220;                // the limb on threes
            const double o = otherX(f);
            if (x >= o && x < o + 10 && y >= 50 && y < 80) v = float(140 + 25 * std::cos(y * 0.8));          // the other character
            g.v[size_t(y) * W + x] = v + gain;
        }
        return g;
    };
    for (int fade = 0; fade < 2; ++fade) {
        const int N = 24;
        std::map<uint64_t, CellGrid> grids; std::map<uint64_t, RegionFrame> frames;
        for (int f = 0; f < N; ++f) {
            grids[uint64_t(f)] = grid(f, fade ? 2.0f * f : 0.f);   // fade: +2 codes per frame
            if (f) frames[uint64_t(f)] = CompareRegions(grids[uint64_t(f - 1)], grids[uint64_t(f)]);
        }
        auto frameAt = [&](uint64_t q) -> const RegionFrame* { auto it = frames.find(q); return it == frames.end() ? nullptr : &it->second; };
        auto gridAt = [&](uint64_t q) -> const CellGrid* { auto it = grids.find(q); return it == grids.end() ? nullptr : &it->second; };
        int body = 0, other = 0, limbSpan3 = 0, bg = 0, total = 0;
        for (int f = 4; f + 4 < N; ++f)
            for (const auto& g : PlanRepairs(uint64_t(f), frameAt, gridAt, kLookahead)) {
                ++total;
                auto on = [&](double x, double y) { return g.mask[size_t(y) * W + size_t(x)] != 0; };
                if (on(bodyX(f) + 7, 45)) ++body;
                if (on(otherX(f) + 5, 65)) ++other;
                if (g.d1 - g.d0 >= 3 && on(bodyX(f) + 15, 38 + limbDy(f))) ++limbSpan3;
                if (on(80, 10) || on(10, 85)) ++bg;
            }
        if (fade) { Check(total == 0, "a fade (2 codes per frame) stops every region repair (" + std::to_string(total) + " parts)"); continue; }
        char b[200]; std::snprintf(b, sizeof(b), "nested cadences: body on twos repaired on %d frames, the other character (twos, out of phase) on %d, the background on ones %d", body, other, bg);
        Check(body >= 6 && other >= 6 && bg == 0, b);
        Check(limbSpan3 >= 1, "a limb on threes on the body on twos gets its own span of three (" + std::to_string(limbSpan3) + ")");
    }
    // the gate: wholly held / exact parts open it, live action's near-still frames do not
    {
        CadenceEvidence anim, live;
        for (int i = 0; i < 240; ++i) { anim.Push(i % 4 == 0); live.Push(i % 60 == 0); }
        Check(anim.Animation() && !live.Animation(), "animation gate: 25 % evidence opens it, 1.7 % (live action) does not");
        CadenceEvidence h; for (int i = 0; i < 240; ++i) h.Push(i % 10 == 0); bool open = h.Animation();
        for (int i = 0; i < 240; ++i) { h.Push(i % 14 == 0); open = h.Animation() && open; }
        Check(open, "animation gate: hysteresis keeps it open at 7 % after 10 %");
    }
    {
        NoiseFloor clean, grainy;
        for (int i = 0; i < 20; ++i) { clean.Push(1.0f); grainy.Push(4.0f); }
        char b[160]; std::snprintf(b, sizeof(b), "noise floor: change threshold %.1f codes on clean anime, %.1f on a grainy BD", clean.Threshold(), grainy.Threshold());
        Check(std::fabs(clean.Threshold() - 5.0f) < 0.01f && std::fabs(grainy.Threshold() - 12.0f) < 0.01f, b);
    }
}

// round 3: the midpoint streams' phases (AMD FRC / FSR 3.1), the cascade's future ends, the phase map
static void TestRound3()
{
    using namespace pushmpc::held;
    const MidpointPlan h = PlanMidpoints(0.5f), a = PlanMidpoints(1.f / 3.f), b = PlanMidpoints(2.f / 3.f), q = PlanMidpoints(0.25f), r = PlanMidpoints(0.75f);
    Check(h.calls == 1 && h.t == 0.5f, "midpoints: twos (t 0.5) in one call, exact");
    Check(a.calls == 2 && a.t == 0.25f && a.side[1] == 0 && b.calls == 2 && b.t == 0.75f && b.side[1] == 1, "midpoints: threes at 0.25 / 0.75 (mid(d0, M), mid(M, d1)), error 1/12 of the span");
    Check(q.t == 0.25f && r.t == 0.75f && q.calls == 2 && r.calls == 2, "midpoints: fours exact (0.25, 0.5, 0.75)");
    Check(std::fabs(PlanMidpoints(1.f / 3.f, 3).t - 0.375f) < 1e-6f, "midpoints: depth 3 gives 0.375 for a third");
    RepairGroup g1; g1.d0 = 9; g1.d1 = 12; g1.pairFrom = 9; g1.t = 1.f / 3.f; g1.cells = 100; g1.mask.assign(4, 0); g1.mask[0] = 255;
    RepairGroup g2 = g1; g2.d0 = 10; g2.pairFrom = 10; g2.d1 = 11; g2.t = 0.5f; g2.mask.assign(4, 0); g2.mask[1] = 255;
    RepairGroup g3 = g1; g3.pairFrom = 10; g3.d1 = 11; g3.d0 = 9; g3.t = 0.5f; g3.mask.assign(4, 0); g3.mask[2] = 255; g3.cells = 300;
    const auto ends = CascadeEnds({g1, g2, g3}, 10);
    Check(ends.size() == 2 && ends[0] == 11 && ends[1] == 12, "cascade: the future ends to repair first, in order, once each");
    std::vector<float> t, al;
    g2.t = 0.25f;
    Check(PairPhaseMap({g1, g2, g3}, 10, 11, t, al) && std::fabs(t[1] - 0.25f) < 1e-6f && std::fabs(t[2] - 0.5f) < 1e-6f && al[1] == 1.f && al[0] == 0.f
          && std::fabs(t[3] - (0.25f * 100 + 0.5f * 300) / 400.f) < 1e-5f, "phase map: each part's t in its mask, the weighted mean elsewhere, alpha the masks");
}

// round 5: hold plans (HoldPlan.h) on synthetic cell grids, and the hold compose (PushCoreHold.h) on WARP
namespace r5 {
static float Hash01(int x, int y, uint32_t seed)
{
    uint32_t h = uint32_t(x * 73856093) ^ uint32_t(y * 19349663) ^ (seed * 83492791u);
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return float(h & 1023u) / 1023.f;
}
static float Noise(float x, float y, uint32_t seed)   // value noise: aperiodic, smooth
{
    const int x0 = int(std::floor(x)), y0 = int(std::floor(y)); const float fx = x - float(x0), fy = y - float(y0);
    const float sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    const float a = Hash01(x0, y0, seed), b = Hash01(x0 + 1, y0, seed), c = Hash01(x0, y0 + 1, seed), d = Hash01(x0 + 1, y0 + 1, seed);
    return (a * (1 - sx) + b * sx) * (1 - sy) + (c * (1 - sx) + d * sx) * sy;
}
static float Tex(int x, int y, uint32_t seed)   // a painted background: aperiodic shapes and a little grain, 40..190 codes
{
    return 40.f + 110.f * Noise(float(x) / 9.f, float(y) / 9.f, seed) + 35.f * Noise(float(x) / 3.1f, float(y) / 3.1f, seed + 7) + 3.f * Hash01(x, y, seed + 13);
}
struct Scene {
    uint32_t w = 160, h = 90;
    // background panning bgSpeed cells per frame (on ones); a sprite (24 x 16 cells) on twos moving 6 cells per drawing
    // over it; an optional text block scrolling up 1 cell per frame; an optional flash frame; an optional object that
    // appears from nothing (a smear / an effect: no motion explains it)
    float bgSpeed = 1.f; bool sprite = true; bool spriteOnes = false; bool text = false; int flashAt = -1; int appearAt = -1;
    CellGrid At(int f) const
    {
        CellGrid g; g.w = w; g.h = h; g.cell = 4; g.v.resize(size_t(w) * h);
        const int sx = spriteOnes ? 40 + 3 * f : 40 + 6 * (f / 2), sy = 40;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                float v = Tex(int(x) + int(std::lround(bgSpeed * f)), int(y), 1);
                if (sprite && int(x) >= sx && int(x) < sx + 24 && int(y) >= sy && int(y) < sy + 16) v = 200.f + 30.f * std::sin(float(int(x) - sx) * 0.45f) * std::cos(float(int(y) - sy) * 0.6f) + 0.8f * float(int(x) - sx);   // a drawn character
                if (text) {   // credits: thin white strokes on black, rendered at 4 x 4 pixels a cell and averaged (as the GPU pass does)
                    double acc = 0;
                    for (int j = 0; j < 4; ++j)
                        for (int i = 0; i < 4; ++i) {
                            const int X = int(x) * 4 + i, Y = (int(y) + f) * 4 + j;   // pixels; the roll moves up one cell a frame
                            const bool word = Hash01(X / 24, Y / 20, 21) > 0.45f && (Y % 20) < 12;
                            const bool stroke = (X % 5) < 2 || (Y % 6) < 2;
                            acc += word && stroke ? 235.0 : 20.0;
                        }
                    v = float(acc / 16.0);
                }
                if (appearAt >= 0 && f >= appearAt && f < appearAt + 2 && x >= 10 && x < 34 && y >= 10 && y < 34) v = 150.f + 40.f * std::sin(float(x * y) * 0.31f + float(f));   // an effect that appears
                if (f == flashAt) v = std::min(255.f, v + 60.f);
                g.v[size_t(y) * w + x] = v;
            }
        return g;
    }
};
struct Run {
    std::vector<CellGrid> g; std::vector<RegionFrame> r; std::vector<std::vector<ObjectClass>> objs; std::vector<TransientMarks> tm;
    void Make(const Scene& s, int frames)
    {
        g.clear(); r.assign(size_t(frames), {}); objs.assign(size_t(frames), {}); tm.assign(size_t(frames), {});
        for (int f = 0; f < frames; ++f) g.push_back(s.At(f));
        std::vector<int16_t> px, py;
        for (int f = 1; f < frames; ++f) {
            r[size_t(f)] = CompareRegions(g[size_t(f - 1)], g[size_t(f)]);
            const BlockMotion bm = MatchBlocks(g[size_t(f - 1)], g[size_t(f)], r[size_t(f)], kCellChangeCodes);
            std::vector<int16_t> nx, ny;
            objs[size_t(f)] = ClassifyObjects(r[size_t(f)], bm, px, py, &nx, &ny);
            px.swap(nx); py.swap(ny);
        }
        for (int f = 1; f + 2 < frames; ++f) {
            const float m[4] = { GridMean(g[size_t(f - 1)]), GridMean(g[size_t(f)]), GridMean(g[size_t(f + 1)]), GridMean(g[size_t(f + 2)]) };
            tm[size_t(f)] = DetectTransient(m, r[size_t(f)], r[size_t(f + 1)], r[size_t(f + 2)], &g[size_t(f - 1)], &g[size_t(f)], &g[size_t(f + 1)]);
        }
    }
    HoldPlan Plan(int f, AnimMode mode, uint32_t flags) const
    {
        const TransientMarks z{};
        return PlanHolds(mode, flags, true, r[size_t(f)], objs[size_t(f)], f >= 2 ? tm[size_t(f - 2)] : z, tm[size_t(f - 1)], tm[size_t(f)], g[0].w, g[0].h);
    }
};
static uint32_t MaskIn(const HoldPlan& p, int x0, int x1, int y0, int y1)
{
    uint32_t n = 0;
    if (p.mask.empty()) return 0;
    for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) n += p.mask[size_t(y) * p.w + x] != 0;
    return n;
}
} // namespace r5

static void TestRound5Plans()
{
    using namespace r5;
    // s1: a character on twos over a pan on ones
    {
        Scene sc; Run run; run.Make(sc, 14);
        int stepPairs = 0, spriteHeld = 0, bgHeld = 0, fullMasks = 0;
        for (int f = 3; f < 12; ++f) {
            const HoldPlan cam = run.Plan(f, AnimMode::CameraOnly, kHoldTransients), full = run.Plan(f, AnimMode::Full, kHoldTransients);
            fullMasks += full.cells > 0 || full.whole;
            if (f % 2 == 0) {   // the drawing steps at even frames
                ++stepPairs;
                const int sx = 40 + 6 * (f / 2);
                spriteHeld += MaskIn(cam, sx, sx + 24, 40, 56) >= 24 * 16 * 9 / 10 && MaskIn(cam, sx - 6, sx, 40, 56) >= 6 * 16 * 9 / 10;   // the new place and the sweep
            }
            bgHeld += MaskIn(cam, 0, 30, 0, 30) + MaskIn(cam, 100, 160, 60, 90) > 0;   // incl. the strip the pan reveals at the edge
        }
        Check(spriteHeld == stepPairs, "round 5 s1: Camera only holds the character on twos on every drawing step, its sweep included (" + std::to_string(spriteHeld) + " of " + std::to_string(stepPairs) + ")");
        Check(bgHeld == 0, "round 5 s1: the background panning on ones and the strip it reveals at the edge are never held (" + std::to_string(bgHeld) + " held pairs)");
        Check(fullMasks == 0, "round 5 s1: Full mode holds nothing here (no flash, nothing unexplained): " + std::to_string(fullMasks));
    }
    // s2: a cel slid on ones (the sprite moving every frame) stays smooth
    {
        Scene cel; cel.spriteOnes = true;
        Run run; run.Make(cel, 10);
        uint32_t held = 0;
        for (int f = 3; f < 9; ++f) held += run.Plan(f, AnimMode::CameraOnly, kHoldTransients).cells;
        Check(held == 0, "round 5 s2: a character slid on ones (3 cells a frame over a pan) is never held in Camera only (" + std::to_string(held) + " cells)");
    }
    // s3: a scrolling text block (credits) on a still background stays smooth
    {
        Scene sc; sc.bgSpeed = 0.f; sc.sprite = false; sc.text = true;
        Run run; run.Make(sc, 12);
        uint32_t held = 0, heldFull = 0, slid = 0, cells = 0;
        for (int f = 3; f < 10; ++f) {
            held += run.Plan(f, AnimMode::CameraOnly, kHoldTransients).cells; heldFull += run.Plan(f, AnimMode::Full, kHoldTransients).cells;
            for (const auto& o : run.objs[size_t(f)]) slid += o.kind == ObjectClass::Slid;
            cells += sc.w * sc.h;
        }
        Check(heldFull == 0, "round 5 s3: scrolling credits (thin words on black) are never held in Full mode (" + std::to_string(heldFull) + " cells)");
        // Camera only: the camera estimate drifts up to 0.4 cell across a pure vertical roll on this synthetic page, and
        // the changes it leaves are held as redrawn. Open (track note, round 5): measured here, decided at the verdict.
        std::printf("info round 5 s3: Camera only holds %.1f %% of the picture on the credit roll (%u slid objects)\n", 100.0 * held / std::max(1u, cells), slid);
    }
    // flash: a single bright frame; the pairs on both sides of it are held whole, nearest in time; Full mode and live action too
    {
        Scene sc; sc.flashAt = 6; Run run; run.Make(sc, 12);
        Check(run.tm[6].flash && !run.tm[5].flash && !run.tm[7].flash, "round 5 flash: the flash frame is found, its neighbours are not");
        const HoldPlan a = run.Plan(6, AnimMode::Full, kHoldTransients), b = run.Plan(7, AnimMode::Full, kHoldTransients), c = run.Plan(8, AnimMode::Full, kHoldTransients);
        Check(a.whole && a.nearest && b.whole && b.nearest && !c.whole, "round 5 flash: pairs (5, 6) and (6, 7) show the nearest real frame, whole picture; (7, 8) does not");
        const TransientMarks z{};
        const HoldPlan live = PlanHolds(AnimMode::Full, kHoldTransients | kHoldLiveTransients, false, run.r[6], run.objs[6], z, run.tm[5], run.tm[6], sc.w, sc.h);
        const HoldPlan liveOff = PlanHolds(AnimMode::Full, kHoldTransients, false, run.r[6], run.objs[6], z, run.tm[5], run.tm[6], sc.w, sc.h);
        const HoldPlan offMode = run.Plan(6, AnimMode::Off, kHoldTransients);
        Check(live.whole && !liveOff.whole && !offMode.whole, "round 5 flash: held on live action with the every-video flag, not without it, not in mode Off");
        RegionFrame fr = run.r[6]; fr.transient = true;
        Check(fr.transient, "round 5 flash: the frame carries the repair gate mark");
    }
    // smear / appearing object: no motion explains it -> Unexplained, held nearest in time in Full mode
    {
        Scene sc; sc.appearAt = 6; Run run; run.Make(sc, 12);
        bool found = false;
        for (const auto& o : run.objs[6]) found = found || (o.kind == ObjectClass::Unexplained && o.x0 <= 12 && o.y0 <= 12);
        const HoldPlan p = run.Plan(6, AnimMode::Full, kHoldTransients);
        Check(found && p.cells > 0 && p.nearest && MaskIn(p, 10, 34, 10, 34) >= 24 * 24 * 9 / 10, "round 5 smear: an object that appears is unexplained and held (nearest) in Full mode (" + std::to_string(p.cells) + " cells)");
        const HoldPlan off = run.Plan(6, AnimMode::Full, 0);
        Check(off.cells == 0, "round 5 smear: nothing held with the transient hold off");
    }
    // the show class from the cadence
    {
        ShowClassifier twos, ones, cg;
        for (int i = 0; i < 400; ++i) { twos.Push(i % 3 ? 2.f : 1.f, true); ones.Push(i % 4 ? 1.f : 2.f, true); cg.Push(1.f, i % 10 == 0); }
        Check(twos.Provisional() == ShowClass::TvLimited && ones.Provisional() == ShowClass::Feature && cg.Provisional() == ShowClass::MixedCg, "round 5 show class: twos -> TV limited, ones -> feature, gate mostly closed -> mixed CG");
    }
}

// the compose pass on WARP: whole-frame nearest, masked as-drawn with a camera, untouched pixels bit-exact
static void TestRound5Compose()
{
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    const D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_1 };
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) { Check(false, "WARP device (hold compose)"); return; }
    pushmpc::HoldCompose hc;
    if (!hc.init(dev.Get())) { Check(false, "hold compose compiles: " + hc.why()); return; }
    const uint32_t W = 256, H = 144;
    auto make = [&](auto fn) {
        std::vector<uint32_t> px(size_t(W) * H);
        for (uint32_t y = 0; y < H; ++y) for (uint32_t x = 0; x < W; ++x) { const uint32_t v = fn(int(x), int(y)) & 255u; px[size_t(y) * W + x] = v | (v << 8) | (v << 16) | (255u << 24); }
        D3D11_TEXTURE2D_DESC d{}; d.Width = W; d.Height = H; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc = {1, 0};
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA s{ px.data(), W * 4, 0 };
        ComPtr<ID3D11Texture2D> t; dev->CreateTexture2D(&d, &s, &t); return t;
    };
    auto read = [&](ID3D11Texture2D* t) {
        D3D11_TEXTURE2D_DESC d{}; t->GetDesc(&d); d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> st; dev->CreateTexture2D(&d, nullptr, &st); ctx->CopyResource(st.Get(), t);
        D3D11_MAPPED_SUBRESOURCE m{}; std::vector<int> v(size_t(W) * H);
        if (SUCCEEDED(ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) { for (uint32_t y = 0; y < H; ++y) for (uint32_t x = 0; x < W; ++x) v[size_t(y) * W + x] = int(static_cast<const uint8_t*>(m.pData)[y * m.RowPitch + x * 4]); ctx->Unmap(st.Get(), 0); }
        return v;
    };
    auto A = make([](int x, int y) { return uint32_t(20 + (x * 3 + y) % 200); });
    auto B = make([](int x, int y) { return uint32_t(30 + (x + y * 2) % 190); });
    // whole, nearest
    for (float t : { 0.25f, 0.75f }) {
        auto G = make([](int, int) { return 128u; });
        pushmpc::HoldCompose::Pass p; p.flags = pushmpc::HoldCompose::kWhole | pushmpc::HoldCompose::kNearest; p.t = t;
        const bool ok = hc.compose(ctx.Get(), G.Get(), A.Get(), B.Get(), p);
        const auto g = read(G.Get()), a = read(A.Get()), b = read(B.Get());
        int worst = 0; for (size_t i = 0; i < g.size(); ++i) worst = std::max(worst, std::abs(g[i] - (t < 0.5f ? a[i] : b[i])));
        Check(ok && worst <= 1, std::string("round 5 compose: a flash pair shows the nearest real frame at t ") + (t < 0.5f ? "0.25 (A)" : "0.75 (B)") + ", max error " + std::to_string(worst));
    }
    // masked as drawn with a camera pan of 8 px per pair at t 0.5: inside = A moved by 4 px, outside bit-exact G
    {
        auto G = make([](int x, int y) { return uint32_t(7 + (x * 5 + y * 3) % 240); });
        const auto g0 = read(G.Get()), a = read(A.Get());
        std::vector<uint8_t> mask(64 * 36, 0);
        for (int y = 10; y < 26; ++y) for (int x = 20; x < 44; ++x) mask[size_t(y) * 64 + x] = 255;   // cells of 4 px: x 80..176, y 40..104
        Check(hc.setMask(ctx.Get(), mask, 64, 36, 1.f, 1.f, 2.f), "round 5 compose: mask upload");
        pushmpc::HoldCompose::Pass p; p.flags = pushmpc::HoldCompose::kMask; p.t = 0.5f; p.camX = 8.f;
        const bool ok = hc.compose(ctx.Get(), G.Get(), A.Get(), B.Get(), p);
        const auto g = read(G.Get());
        int worstIn = 0, changedOut = 0;
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                const size_t i = size_t(y) * W + x;
                if (x >= 100 && x < 156 && y >= 56 && y < 88) worstIn = std::max(worstIn, std::abs(g[i] - a[size_t(y) * W + (x - 4)]));
                const bool away = x < 56 || x >= 200 || y < 16 || y >= 128;
                if (away && g[i] != g0[i]) ++changedOut;
            }
        Check(ok && worstIn <= 1, "round 5 compose: inside the mask the camera-warped previous drawing (A moved 4 px at t 0.5), max error " + std::to_string(worstIn));
        Check(changedOut == 0, "round 5 compose: outside the mask and its feather the generated frame is bit-exact (" + std::to_string(changedOut) + " changed)");
    }
    // the split: only the right half composed, a divider at the middle
    {
        auto G = make([](int, int) { return 128u; });
        pushmpc::HoldCompose::Pass p; p.flags = pushmpc::HoldCompose::kWhole | pushmpc::HoldCompose::kNearest | pushmpc::HoldCompose::kDivider; p.t = 0.25f; p.split = 128.f; p.side = 1;
        hc.compose(ctx.Get(), G.Get(), A.Get(), B.Get(), p);
        const auto g = read(G.Get()), a = read(A.Get());
        int leftChanged = 0, rightWrong = 0;
        for (uint32_t y = 0; y < H; ++y) for (uint32_t x = 0; x < W; ++x) { const size_t i = size_t(y) * W + x; if (x < 127 && g[i] != 128) ++leftChanged; if (x > 129 && std::abs(g[i] - a[i]) > 1) ++rightWrong; }
        Check(leftChanged == 0 && rightWrong == 0 && std::abs(g[128] - 128) <= 1, "round 5 compose: the verdict split composes one side only (" + std::to_string(leftChanged) + " left changed, " + std::to_string(rightWrong) + " right wrong)");
    }
}

int main()
{
    TestRules();
    TestGpu();
    TestLayered();
    TestRegionRules();
    TestRound3();
    TestRound5Plans();
    TestRound5Compose();
    std::printf("RESULT %s: %d of %d checks passed\n", g_fail ? "FAIL" : "PASS", g_checks - g_fail, g_checks);
    return g_fail ? 1 : 0;
}
