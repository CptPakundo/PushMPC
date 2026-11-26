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

int main()
{
    TestRules();
    TestGpu();
    std::printf("RESULT %s: %d of %d checks passed\n", g_fail ? "FAIL" : "PASS", g_checks - g_fail, g_checks);
    return g_fail ? 1 : 0;
}
