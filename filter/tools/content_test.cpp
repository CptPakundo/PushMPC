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
                v = 60.f + 20.f * float((hsh >> 7) % 8u);   // a painted background: few flat tones
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
        std::snprintf(buf, sizeof buf, "drawn: hold %.2f twos %.2f flat %.2f / live: hold %.2f flat %.2f noise %.2f", sa.feat[fHold], sa.feat[fTwos], sa.feat[fAFlat], sb.feat[fHold], sb.feat[fAFlat], sb.feat[fANoise]);
        Check(sa.windows > 0 && sb.windows > 0 && sc.windows > 0, "windows evaluated on every synthetic clip");
        Check(sa.feat[fHold] > sb.feat[fHold] + 0.3f, "partial holds: a character on twos over a pan holds its cels between drawings, non-rigid live motion changes every frame", buf);
        Check(sa.feat[fAFlat] > sb.feat[fAFlat] + 0.3f, "appearance: the drawn picture is flat where the textured one is not (both grey: the palette is not tested here)", buf);
        Check(sa.feat[fAStep] > sb.feat[fAStep] && sa.feat[fALine] > 0.01f, "appearance: step edges and dark outlines on the drawn picture", std::to_string(sa.feat[fAStep]) + " vs " + std::to_string(sb.feat[fAStep]) + ", lines " + std::to_string(sa.feat[fALine]));
        Check(sa.feat[fCam] > 0.5f, "the camera pan is taken out (phase correlation)", std::to_string(sa.feat[fCam]));
        Check(std::fabs(sc.feat[fFps] - 60.f) < 0.5f && std::fabs(sb.feat[fFps] - 23.976f) < 0.5f, "frame rate from the timestamps: 60 fps vs 23.976", std::to_string(sc.feat[fFps]));
        Check(sb.feat[fTnoise] >= 0.f, "the temporal noise floor of still cells is measured", std::to_string(sb.feat[fTnoise]));
        ContentDetector e;
        Run(0, 23.976, 2, 30, e); e.Discontinuity();
        Check(e.Current().windows > 0, "a discontinuity keeps the decision state (same title)");
    }
    {   // 1b. the phase correlation's DFT: any length, circular (no padding), forward and inverse against a direct DFT,
        //     interleaved columns as a grid's columns; mixed radix (240, 135, 100, 180) and Bluestein (prime 131, 2 x 67)
        double worst = 0; std::string where;
        std::mt19937 rng(3); std::uniform_real_distribution<float> u(-100.f, 100.f);
        for (int n : { 1, 2, 3, 4, 5, 7, 8, 12, 13, 16, 17, 100, 131, 134, 135, 180, 240, 256 })
            for (int inv = 0; inv < 2; ++inv) {
                const int count = n % 3 == 0 ? 3 : 1;
                std::vector<detail::Cplx> x(size_t(n) * count), y; for (auto& v : x) v = { u(rng), u(rng) };
                y = x;
                detail::DftPlan pl; pl.Init(n); std::vector<detail::Cplx> sc(pl.ScratchSize(count));
                pl.Run(y.data(), sc.data(), count, inv != 0);
                double err = 0, mag = 0;
                for (int q = 0; q < count; ++q)
                    for (int k = 0; k < n; ++k) {
                        std::complex<double> acc;
                        for (int j = 0; j < n; ++j) acc += std::complex<double>(x[size_t(q) + size_t(count) * j]) * std::polar(1.0, (inv ? 2.0 : -2.0) * 3.141592653589793 * double((int64_t(j) * k) % n) / n);
                        err = std::max(err, std::abs(acc - std::complex<double>(y[size_t(q) + size_t(count) * k]))); mag = std::max(mag, std::abs(acc));
                    }
                if (err / std::max(mag, 1.0) > worst) { worst = err / std::max(mag, 1.0); where = std::to_string(n) + (inv ? " inverse" : " forward"); }
            }
        Check(worst < 2e-5, "DFT of any length (mixed radix / Bluestein) = direct DFT", "worst relative error " + std::to_string(worst) + " at " + where);
        // a pan on pictures whose half grids are not powers of two (16:9 -> 240 x 135, 2.39:1 -> 240 x 100, 4:3 -> 240 x 180)
        for (int hh : { 540, 402, 720 }) {
            ContentDetector d; std::mt19937 rr(7);
            const int w = 960, h = hh;
            for (int i = 0; i < 40; ++i) {
                std::vector<uint8_t> yy(size_t(w) * h), uv(size_t(w) * h / 2, 128);
                for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) { const int xx = x + 6 * i; const uint32_t hs = uint32_t(xx / 37) * 2654435761u ^ uint32_t(y / 29) * 40503u; yy[size_t(y) * w + x] = uint8_t(40 + (hs >> 9) % 170u); }
                d.Push(CellsFromYuv(yy.data(), w, uv.data(), static_cast<const uint8_t*>(nullptr), w, w, h, 8, false), i / 24.0);
            }
            Check(d.Current().feat[fCam] > 0.9f && d.Current().feat[fChgm] < 0.05f, "a pan is found and taken out on a " + std::to_string(w) + "x" + std::to_string(h) + " picture (half grid " + std::to_string(kCellsW / 2) + "x" + std::to_string(CellsHeight(w, h) / 2) + ")",
                  "cam " + std::to_string(d.Current().feat[fCam]) + " chg " + std::to_string(d.Current().feat[fChgm]));
        }
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
    {   // 2b. the image model's evidence (zero-shot against the prompt embeddings) and its weight in the tracker
        const double pd = ImageDrawnProbability(clip::kPrompt[0]), po = ImageDrawnProbability(clip::kPrompt[clip::kPromptsDrawn]);
        Check(pd > 0.6 && po < 0.4, "image model: a drawn prompt's own embedding reads as drawn, an other prompt's as not", std::to_string(pd) + " / " + std::to_string(po));
        std::vector<uint8_t> yy(640 * 360, 100), uv(640 * 180, 128);
        const auto in = ClipInput(yy.data(), 640, uv.data(), static_cast<const uint8_t*>(nullptr), 640, 640, 360, 8, true);
        Check(in.size() == 3 * 224 * 224, "image model input: 3 x 224 x 224 floats");
        ContentDetector d; float x[kFeatureCount] = {};
        for (int i = 0; i < 20; ++i) { const double t = 1.0 + 0.5 * i; d.SetImageEvidence(0.995, t); d.Classify(x, t, 0); }
        Check(d.Current().cls == kDrawn && d.Current().imageEvidence, "a steady drawn image evidence decides drawn", std::to_string(d.Current().logOdds));
        ContentDetector e; for (int i = 0; i < 20; ++i) { const double t = 1.0 + 0.5 * i; e.SetImageEvidence(0.01, t); e.Classify(x, t, 0); }
        Check(e.Current().cls == kOther, "a steady not-drawn image evidence decides not drawn", std::to_string(e.Current().logOdds));
        // 2c. rendered 3D CG among the drawn looks: the prompts' own embeddings, the CG tracker, the policy
        const double ch = ImageCgProbability(clip::kCgPrompt[0]), cr = ImageCgProbability(clip::kCgPrompt[clip::kCgPromptsHand]);
        Check(ch < 0.4 && cr > 0.6, "image model: a hand-drawn prompt's embedding reads as hand-drawn, a rendered prompt's as rendered CG", std::to_string(ch) + " / " + std::to_string(cr));
        ContentDetector g; double t = 1.0;
        for (int i = 0; i < 40; ++i, t += 0.5) { g.SetImageEvidence(0.995, t, 0.95); g.Classify(x, t, 0); }
        Check(g.Current().cls == kDrawn && g.Current().cg, "drawn with steady rendered-CG evidence: drawn, CG flag on", std::to_string(g.Current().cgLogOdds));
        const DrawnPolicy& pc = PolicyFor(g.Current().cls, g.Current().cg);
        Check(pc.holdRepair && pc.animeUpscaler && !pc.animeUpscalerDs && !pc.signHold && !pc.lineArt && !pc.animeDeband,
              "drawn CG policy: hold repair and the plain anime upscaler on; _DS, sign hold, line art and the anime deband preset off");
        for (int i = 0; i < 40; ++i, t += 0.5) { g.SetImageEvidence(0.995, t, 0.05); g.Classify(x, t, 0); }
        Check(g.Current().cls == kDrawn && !g.Current().cg, "the CG flag turns off with steady hand-drawn evidence", std::to_string(g.Current().cgLogOdds));
        const DrawnPolicy& ph = PolicyFor(kDrawn, false);
        Check(ph.holdRepair && ph.animeUpscaler && !ph.animeUpscalerDs && ph.signHold && ph.lineArt && ph.animeDeband, "hand-drawn policy: everything on except an automatic _DS upscaler (by hand only)");
        ContentDetector k; for (int i = 0; i < 30; ++i, t += 0.5) { k.SetImageEvidence(0.995, t, i < 26 ? 0.3 : 0.9); k.Classify(x, t, 0); }
        const float before = k.Current().cgLogOdds; const double kt = t - 0.5;   // one keyframe repeated (the model runs about once a second)
        for (int i = 0; i < 4; ++i, t += 0.5) { k.SetImageEvidence(0.995, kt, 0.9); k.Classify(x, t, 0); }
        Check(k.Current().cls == kDrawn && std::fabs(k.Current().cgLogOdds - before) < 1e-6f, "a repeated keyframe adds no new CG evidence", std::to_string(before) + " -> " + std::to_string(k.Current().cgLogOdds));
        for (int i = 0; i < 150; ++i, t += 0.5) { g.SetImageEvidence(0.01, t, 0.95); g.Classify(x, t, 0); }
        Check(g.Current().cls == kOther && !g.Current().cg && !PolicyFor(g.Current().cls, true).holdRepair, "not drawn: no CG flag, nothing of the drawn treatment (realistic CG stays not drawn)", std::to_string(g.Current().cls) + " " + std::to_string(g.Current().logOdds));
        // the GPU taps' packed words give the same cells and thumbnail as the planes (P010-like 16-bit and 8-bit)
        {
            const int w = 1280, h = 720;
            std::mt19937 rng(5);
            std::vector<uint8_t> y8(size_t(w) * h), uv8(size_t(w) * h / 2);
            for (auto& v : y8) v = uint8_t(16 + rng() % 220); for (auto& v : uv8) v = uint8_t(16 + rng() % 225);
            const Cells a = CellsFromYuv(y8.data(), w, uv8.data(), static_cast<const uint8_t*>(nullptr), w, w, h, 8, true);
            const auto pw = PackedCellsRef(y8.data(), w, uv8.data(), static_cast<const uint8_t*>(nullptr), w, w, h, 8);
            const Cells b = CellsFromPacked(pw.data(), kCellsW, kCellsW, CellsHeight(w, h), true);
            double dy = 0, dc = 0;
            for (size_t i = 0; i < a.y.size(); ++i) { dy = std::max(dy, double(std::fabs(a.y[i] - b.y[i]))); dc = std::max(dc, double(std::fabs(a.cb[i] - b.cb[i]) + std::fabs(a.cr[i] - b.cr[i]))); }
            Check(a.ok() && b.ok() && a.h == b.h && dy < 0.01 && dc < 1e-3, "GPU tap: packed cells = CellsFromYuv (8-bit)", std::to_string(dy) + " / " + std::to_string(dc));
            const auto ci = ClipInput(y8.data(), w, uv8.data(), static_cast<const uint8_t*>(nullptr), w, w, h, 8, true);
            const auto pc2 = PackedClipRef(y8.data(), w, uv8.data(), static_cast<const uint8_t*>(nullptr), w, w, h, 8);
            const auto cj = ClipInputFromPacked(pc2.data(), 224, h >= 720, true);
            double dm = 0; for (size_t i = 0; i < ci.size(); ++i) dm = std::max(dm, double(std::fabs(ci[i] - cj[i])));
            Check(cj.size() == ci.size() && dm < 0.01, "GPU tap: packed thumbnail = ClipInput", std::to_string(dm));
        }
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
