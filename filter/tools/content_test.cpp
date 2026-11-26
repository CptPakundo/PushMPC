// content_test: ContentDetect (pushmpc/content_detect.h) on the CPU, no GPU, no media files.
//  1. synthetic pictures: drawn animation on twos over a pan (partial holds), live action (moving texture with grain),
//     game footage at 60 fps with a fixed HUD; the window features must show what each one is made of
//  2. replay of measured feature traces (content_trace.inc: window features measured on a labelled library, anonymised):
//     the C++ classifier + tracker must make the same decisions as the reference implementation
//  3. CombMeter: progressive vs woven fields; IntervalMeter: a "29.97" tag on a 24p body, a VFR switch
#include <cstdio>
#include <cmath>
#include <random>
#include <string>
#include <vector>
#include "pushmpc/content_detect.h"

using namespace pushmpc::content;
static int g_fail = 0, g_pass = 0;
static void Check(bool ok, const std::string& what, const std::string& detail = {})
{
    std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.empty() ? "" : "  -- ", detail.c_str());
    (ok ? g_pass : g_fail)++;
}

// a 960x540 8-bit picture: flat-colour background with lines (drawn), or noisy texture (live)
struct Pic { int w = 960, h = 540; std::vector<uint8_t> y, uv; };
static Pic Make(int kind, double t, double drawT, int frame, std::mt19937& rng)
{
    Pic p; p.y.assign(size_t(p.w) * p.h, 0); p.uv.assign(size_t(p.w) * p.h / 2, 128);
    std::normal_distribution<float> grain(0.f, kind == 1 ? 3.0f : 0.4f);
    const int pan = frame * 4;                  // the camera pans 4 px (2 cells) every frame
    const int bx = 200 + int(drawT * 240.0);    // the character moves only when it is redrawn
    for (int y = 0; y < p.h; ++y)
        for (int x = 0; x < p.w; ++x) {
            float v;
            if (kind == 0) {   // drawn: flat bands of colour with dark lines, panning; character on twos
                const int xx = x + pan;   // non-periodic flat patches (a painted background), dark outlines
                const uint32_t hsh = uint32_t(xx / 90) * 2654435761u ^ uint32_t(y / 70) * 40503u;
                v = 60.f + float((hsh >> 7) % 160u);
                if (xx % 90 < 3 || y % 70 < 3) v = 30.f;
                if (x >= bx && x < bx + 140 && y >= 180 && y < 420) v = (x - bx < 4 || bx + 140 - x <= 4 || y - 180 < 4 || 420 - y <= 4) ? 20.f : 200.f;
            } else {          // live / game: texture that moves differently everywhere, every frame (no single camera shift)
                const double ph = t * 14.0 * (1.0 + 0.6 * std::sin(y * 0.013)), ph2 = t * 9.0 * (1.0 + 0.5 * std::cos(x * 0.011));
                v = float(120.0 + 50.0 * std::sin(x * 0.031 + ph) * std::cos(y * 0.027 - ph2) + 25.0 * std::sin((x + y) * 0.11 + 2.0 * ph));
                if (kind == 2 && y > 470 && x < 300) v = ((x / 6 + y / 6) % 2) ? 230.f : 40.f;   // HUD: fixed, textured
            }
            p.y[size_t(y) * p.w + x] = uint8_t(std::clamp(v + grain(rng), 0.f, 255.f));
        }
    return p;
}
static State Run(int kind, double fps, int hold, int frames, ContentDetector& d)
{
    std::mt19937 rng(11);
    for (int i = 0; i < frames; ++i) {
        const double t = i / fps;
        const double drawT = (i / hold) * hold / fps;
        const Pic p = Make(kind, t, drawT, i, rng);
        d.Push(CellsFromYuv(p.y.data(), p.w, p.uv.data(), static_cast<const uint8_t*>(nullptr), p.w, p.w, p.h, 8, false), t);
    }
    return d.Current();
}

struct TraceRow { int id, cls; double t; int cut; double f[kFeatureCount]; int expect; };
static const TraceRow kTrace[] = {
#include "content_trace.inc"
};

int main()
{
    {   // 1. synthetic pictures: the features say what they are made of
        ContentDetector a, b, c;
        const State sa = Run(0, 23.976, 2, 96, a), sb = Run(1, 23.976, 1, 96, b), sc = Run(2, 60.0, 1, 240, c);
        char buf[256];
        std::snprintf(buf, sizeof buf, "drawn: hold %.2f twos %.2f flat %.2f / live: hold %.2f flat %.2f noise %.2f", sa.feat[fHold], sa.feat[fTwos], sa.feat[fFlat], sb.feat[fHold], sb.feat[fFlat], sb.feat[fNoise]);
        Check(sa.windows > 0 && sb.windows > 0 && sc.windows > 0, "windows evaluated on every synthetic clip");
        Check(sa.feat[fHold] > sb.feat[fHold] + 0.3f, "partial holds: a character on twos over a pan holds its cels between drawings, non-rigid live motion changes every frame", buf);
        Check(sa.feat[fFlat] > sb.feat[fFlat], "flat colour: the drawn picture is flatter than the textured one", buf);
        Check(sa.feat[fCam] > 0.5f, "the camera pan is taken out (phase correlation)", std::to_string(sa.feat[fCam]));
        Check(sc.feat[fFps60] == 1.f && sb.feat[fFps60] == 0.f, "frame rate from the timestamps: 60 fps vs 23.976");
        Check(sc.feat[fHud] > 0.f && sb.feat[fHud] == 0.f, "a fixed textured overlay in a moving picture (HUD) is found", std::to_string(sc.feat[fHud]) + " chg " + std::to_string(sc.feat[fChg]) + " active " + std::to_string(sc.feat[fActive]));
        ContentDetector e;
        Run(0, 23.976, 2, 30, e); e.Discontinuity();
        Check(e.Current().windows > 0, "a discontinuity keeps the decision state (same title)");
    }
    {   // 2. replay of measured feature traces
        int rows = 0, same = 0, traces = 0, right = 0, decided = 0;
        ContentDetector d; int cur = -1;
        for (const TraceRow& r : kTrace) {
            if (r.id != cur) { d.Reset(); cur = r.id; ++traces; }
            float x[kFeatureCount]; for (int i = 0; i < kFeatureCount; ++i) x[i] = float(r.f[i]);
            d.Classify(x, r.t, r.cut);
            ++rows; same += d.Current().cls == r.expect;
            if (d.Current().cls != kUnknown) { ++decided; right += d.Current().cls == r.cls; }
        }
        char buf[200];
        std::snprintf(buf, sizeof buf, "%d traces, %d rows: %d same decisions; decided rows right %d of %d", traces, rows, same, right, decided);
        Check(rows > 0 && same >= rows * 97 / 100, "replay: C++ classifier + tracker = reference decisions on measured traces (>= 97 % of rows)", buf);
        Check(decided > 0 && right >= decided * 80 / 100, "replay: decided class right on >= 80 % of rows (a consistency check: the shipped model was trained on these titles; held-out accuracy is measured offline)", buf);
    }
    {   // 3. interlacing and frame interval
        const int w = 720, h = 480;
        std::vector<uint8_t> f0(size_t(w) * h), f1 = f0, wv = f0;
        auto frame = [&](std::vector<uint8_t>& f, double t) { for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) f[size_t(y) * w + x] = uint8_t(128 + 90 * std::sin((x + t * 300) * 0.05) * std::cos(y * 0.03)); };
        CombMeter prog, inter;
        for (int i = 0; i < 40; ++i) {
            frame(f0, i / 60.0); frame(f1, (i + 0.5) / 60.0);
            prog.Push(f0.data(), w, w, h);
            for (int y = 0; y < h; ++y) std::copy_n((y & 1 ? f1 : f0).data() + size_t(y) * w, w, wv.data() + size_t(y) * w);
            inter.Push(wv.data(), w, w, h);
        }
        Check(prog.verdict() == CombMeter::kProgressive, "comb meter: progressive motion is progressive", std::to_string(prog.combed()) + "/" + std::to_string(prog.measured()));
        Check(inter.verdict() == CombMeter::kInterlaced, "comb meter: fields woven from two moments are interlaced", std::to_string(inter.combed()) + "/" + std::to_string(inter.measured()));
        IntervalMeter iv; double t = 0;
        for (int i = 0; i < 40; ++i) { t += (i % 2 ? 0.042 : 0.041); iv.Push(t, false); }   // Matroska ms rounding of 24p
        Check(std::fabs(iv.fps() - 23.98) < 0.4, "interval: a 24p body (ms timestamps) measures 24 fps whatever the tag", std::to_string(iv.fps()));
        for (int i = 0; i < 64; ++i) { t += 1.0 / 29.97; iv.Push(t, false); }
        Check(std::fabs(iv.fps() - 29.97) < 0.3 && iv.changes() == 1, "interval: a 30p segment is re-measured once", std::to_string(iv.fps()));
    }
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
