// apple_vt_test (macOS): the VideoToolbox engine on this Mac.
//   1. the probe report (which effects exist, their pixel formats, scale factors and size limits);
//   2. for every chain the Mac offers, a synthetic clip through apple::Engine: a bright box moving 12 px a frame on a
//      dark background, a hard cut to a flat picture at frame 6, then the box again:
//        - the generated frames put the box between the two real ones (centroid within 8 px of where the phase says);
//        - the cut pair generates nothing new: each generated frame equals the nearer real frame;
//        - real frames come back unchanged (bit-exact without super resolution);
//        - 10-bit limited range: the background keeps its code value (no range or transfer conversion);
//        - super resolution: the output is 2x the size and the box sits at 2x its position;
//      timings are printed (a virtual machine's numbers say nothing about a real Mac);
//   3. when the Mac offers nothing (a VM without the ML effects), the reasons are printed and Configure must fail
//      cleanly (the plugin then falls back to the portable backend). Exit 0 in that case: CI records it.
//   4. the temporal noise filter (denoise) against a clean master with synthetic noise, PSNR per strength and a null
//      arm (TestNoiseFilter); APPLE_TNF_RESULT says whether its output was measured at all.
// Exit code: 1 when an offered chain fails a check.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>
#include "apple_engine.h"
#include "apple_metal.h"

using namespace pmvs::apple;

static int g_pass = 0, g_fail = 0, g_configured = 0, g_unavailable = 0;
#define CHECK(cond, ...) do { if (cond) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s | ", __FILE__, __LINE__, #cond); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

struct Pic {
    int w, h, bits; std::vector<uint8_t> p[3]; ptrdiff_t stride[3];
    Pic(int w_, int h_, int bits_) : w(w_), h(h_), bits(bits_)
    {
        const int bps = bits > 8 ? 2 : 1;
        for (int i = 0; i < 3; ++i) { stride[i] = ptrdiff_t((i ? ChromaW(w) : w) * bps); p[i].assign(size_t(stride[i] * (i ? ChromaH(h) : h)), 0); }
    }
    int Get(int i, int x, int y) const { const uint8_t* r = p[i].data() + y * stride[i]; if (bits <= 8) return r[x]; uint16_t v; std::memcpy(&v, r + 2 * x, 2); return v; }
    void Set(int i, int x, int y, int v) { uint8_t* r = p[i].data() + y * stride[i]; if (bits <= 8) r[x] = uint8_t(v); else { const uint16_t u = uint16_t(v); std::memcpy(r + 2 * x, &u, 2); } }
    PlanarView View() const { PlanarView v; v.width = w; v.height = h; v.bits = bits; for (int i = 0; i < 3; ++i) { v.data[i] = p[i].data(); v.stride[i] = stride[i]; } return v; }
    PlanarOut Out() { PlanarOut o; for (int i = 0; i < 3; ++i) { o.data[i] = p[i].data(); o.stride[i] = stride[i]; } return o; }
    bool Equal(const Pic& o) const { for (int i = 0; i < 3; ++i) if (p[i] != o.p[i]) return false; return true; }
};

static const int kStep = 12, kBox = 96, kCutAt = 6, kFrames = 12;
static int BoxX(int f) { return 64 + kStep * f; }
static Pic Frame(int w, int h, int bits, int f)
{
    Pic p(w, h, bits);
    const int s = bits > 8 ? 4 : 1, by = h / 2 - kBox / 2;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int v = 40;
            if (f == kCutAt) v = 150 + (x * 40) / w;                                          // the cut: another picture
            else if (x >= BoxX(f) && x < BoxX(f) + kBox && y >= by && y < by + kBox) v = 220;
            p.Set(0, x, y, v * s);
        }
    for (int i = 1; i < 3; ++i) for (int y = 0; y < ChromaH(h); ++y) for (int x = 0; x < ChromaW(w); ++x) p.Set(i, x, y, 128 * s);
    return p;
}
// x of the centroid of bright luma (above the midpoint between background and box)
static double Centroid(const Pic& p)
{
    const int s = p.bits > 8 ? 4 : 1, thr = 130 * s;
    double sx = 0, n = 0;
    for (int y = 0; y < p.h; ++y) for (int x = 0; x < p.w; ++x) if (p.Get(0, x, y) > thr) { sx += x; n += 1; }
    return n > 0 ? sx / n : -1;
}

static void Run(const char* name, int w, int h, int bits, int k, const char* sr, const char* fi, bool vt = true)
{
    std::printf("--- %s: %dx%d %d-bit, x%d, sr=%s, fi=%s\n", name, w, h, bits, k, sr, fi);
    Engine e;
    e.log = [](const std::string& m) { std::printf("    log: %s\n", m.c_str()); };
    Config c; c.width = w; c.height = h; c.bits = bits; c.k = k; c.sr = sr; c.fi = fi; c.srcFps = 23.976;
    std::string err, info;
    if (!e.Configure(c, err, info)) { if (vt) ++g_unavailable; std::printf("    not available: %s\n", err.c_str()); return; }
    if (info.rfind("no interpolation + ArtCNN", 0) == 0) vt = false;   // Metal only: says nothing about VideoToolbox
    if (vt) ++g_configured;
    std::printf("    chain: %s\n", info.c_str());
    const int ow = e.OutWidth(), oh = e.OutHeight(), scale = ow / w;
    CHECK(ow == w * scale && oh == h * scale && (scale == 1 || scale == 2 || scale == 4), "%s: output %dx%d", name, ow, oh);
    Colour col; col.fullRange = false;
    if (bits > 8) { col.matrix = 9; col.transfer = 16; col.primaries = 9; }
    double totalMs = 0; int pairs = 0;
    std::vector<Pic> prevOut;
    Pic prevReal(ow, oh, bits);
    for (int f = 0; f < kFrames; ++f) {
        const Pic src = Frame(w, h, bits, f);
        std::vector<Pic> gen(size_t(k - 1), Pic(ow, oh, bits));
        std::vector<PlanarOut> go;
        for (auto& g : gen) go.push_back(g.Out());
        Pic cur(ow, oh, bits);
        bool havePair = false;
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = e.Process(src.View(), col, int64_t(f) * 417083, true, go, cur.Out(), havePair, err);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        CHECK(ok, "%s: frame %d: %s", name, f, err.c_str());
        if (!ok) return;
        if (scale == 1) CHECK(cur.Equal(src), "%s: real frame %d unchanged", name, f);
        else if (f != kCutAt) { const double cx = Centroid(cur), want = (BoxX(f) + kBox / 2.0) * scale; CHECK(std::fabs(cx - want) <= 4.0 * scale, "%s: upscaled real frame %d: box at %.1f, want %.1f", name, f, cx, want); }
        CHECK(havePair == (f > 0 && k >= 2), "%s: pair at frame %d", name, f);
        if (havePair) {
            totalMs += ms; ++pairs;
            const bool cutPair = f == kCutAt || f == kCutAt + 1;
            CHECK(e.LastPairWasCut() == cutPair, "%s: frame %d cut flag %d", name, f, int(e.LastPairWasCut()));
            for (int j = 1; j < k; ++j) {
                const Pic& g = gen[size_t(j - 1)];
                if (cutPair) {
                    const Pic& nearer = j * 2 <= k ? prevReal : cur;
                    CHECK(g.Equal(nearer), "%s: cut pair %d-%d, slot %d is the nearer real frame", name, f - 1, f, j);
                } else {
                    const double t = double(j) / k, want = (BoxX(f - 1) + kStep * t + kBox / 2.0) * scale, cx = Centroid(g);
                    CHECK(std::fabs(cx - want) <= 8.0 * scale, "%s: pair %d-%d slot %d: box at %.1f, want %.1f", name, f - 1, f, j, cx, want);
                    if (bits > 8) { const int bg = g.Get(0, 4, 4); CHECK(std::abs(bg - 160) <= 6, "%s: 10-bit background code %d (want 160: no range change)", name, bg); }
                }
            }
        }
        prevReal = cur;
    }
    const Stats& st = e.Stat();
    std::printf("    %d pairs, %.2f ms per pair (VM timing), %llu VT calls, %llu failed, %llu cuts; %s\n", pairs, pairs ? totalMs / pairs : 0.0,
                (unsigned long long)st.vtCalls, (unsigned long long)st.vtErrors, (unsigned long long)st.cuts, e.Describe().c_str());
    CHECK(st.vtErrors == 0, "%s: VideoToolbox errors", name);
}

// ---- the temporal noise filter against a ground truth: a clean master (a panning texture with a moving box), the
// same with Gaussian luma noise (sigma 8 codes, fixed seed). Each strength, and no denoise, is scored by luma PSNR
// against the clean master over frames 4..15 (after the filter has references). Checks, only when the filter runs:
//   - every frame from the second on comes back denoised;
//   - Balanced and Strong move the noisy clip toward the master (at least +0.5 dB over no denoise);
//   - the null arm: the clean master through Strong stays at 40 dB or more against itself (no restyling).
// APPLE_TNF_RESULT=ok | failed | unsupported | notrun (a self-test failed: the VM accepts calls and returns nothing,
// or the session does not start). Only ok means the output was measured.
static const int kTnfFrames = 16, kTnfW = 1280, kTnfH = 720;
static Pic TnfMaster(int f)
{
    Pic p(kTnfW, kTnfH, 8);
    const double pi = 3.14159265358979;
    for (int y = 0; y < kTnfH; ++y)
        for (int x = 0; x < kTnfW; ++x) {
            double v = 110.0 + 50.0 * std::sin(2 * pi * (x + 3 * f) / 23.0) * std::cos(2 * pi * y / 17.0);
            if (x >= 200 + 8 * f && x < 296 + 8 * f && y >= 300 && y < 396) v = 220;
            p.Set(0, x, y, int(std::lround(v)));
        }
    for (int i = 1; i < 3; ++i) for (int y = 0; y < ChromaH(kTnfH); ++y) for (int x = 0; x < ChromaW(kTnfW); ++x) p.Set(i, x, y, 128);
    return p;
}
static Pic AddNoise(const Pic& m, std::mt19937& rng)
{
    Pic p = m;
    std::normal_distribution<double> nd(0.0, 8.0);
    for (int y = 0; y < m.h; ++y) for (int x = 0; x < m.w; ++x) p.Set(0, x, y, std::clamp(int(std::lround(m.Get(0, x, y) + nd(rng))), 16, 235));
    return p;
}
static double LumaPsnr(const Pic& a, const Pic& b)
{
    double se = 0;
    for (int y = 0; y < a.h; ++y) for (int x = 0; x < a.w; ++x) { const double d = a.Get(0, x, y) - b.Get(0, x, y); se += d * d; }
    const double mse = se / double(a.w * a.h);
    return mse <= 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}
// one arm: the clip through a denoise-only chain (x1); returns the mean PSNR, or -1 when it does not configure (why)
static double TnfArm(const char* strength, const std::vector<Pic>& in, const std::vector<Pic>& master, std::string& why)
{
    Engine e;
    e.log = [](const std::string& m) { std::printf("    log: %s\n", m.c_str()); };
    Config c; c.width = kTnfW; c.height = kTnfH; c.bits = 8; c.k = 1; c.sr = "off"; c.srcFps = 23.976;
    c.denoise = "on"; c.denoiseStrength = strength;
    std::string err, info;
    if (!e.Configure(c, err, info)) { why = err; return -1; }
    std::printf("    chain (%s): %s\n", strength, info.c_str());
    if (!e.Denoising()) { why = "configured without the noise filter: " + info; return -1; }
    Colour col;
    double sum = 0; int n = 0;
    for (int f = 0; f < kTnfFrames; ++f) {
        Pic out(kTnfW, kTnfH, 8);
        bool havePair = false;
        const bool ok = e.Process(in[size_t(f)].View(), col, int64_t(f) * 417083, true, {}, out.Out(), havePair, err);
        CHECK(ok, "denoise %s: frame %d: %s", strength, f, err.c_str());
        if (!ok) return -2;
        if (f >= 4) { sum += LumaPsnr(out, master[size_t(f)]); ++n; }
    }
    const Stats& st = e.Stat();
    std::printf("    denoise %s: %llu frames denoised, %llu without a reference, %llu failed; %.1f ms a frame in Configure's test (VM timing)\n", strength,
                (unsigned long long)st.tnfFrames, (unsigned long long)st.tnfPassed, (unsigned long long)st.tnfErrors, st.benchTnfMs);
    CHECK(st.tnfFrames == uint64_t(kTnfFrames - 1) && st.tnfErrors == 0, "denoise %s: every frame from the second on denoised", strength);
    return n ? sum / n : -1;
}
static std::string TestNoiseFilter(const Caps& c)
{
    std::printf("--- temporal noise filter (denoise), %dx%d 8-bit, Gaussian luma noise sigma 8\n", kTnfW, kTnfH);
    if (!c.noiseFilter) { std::printf("    not exercisable here: the temporal noise filter is not supported (%s); nothing about its output is verified\n", c.os.c_str()); return "unsupported"; }
    std::vector<Pic> master, noisy;
    std::mt19937 rng(20261128u);
    for (int f = 0; f < kTnfFrames; ++f) { master.push_back(TnfMaster(f)); noisy.push_back(AddNoise(master.back(), rng)); }
    double off = 0;
    for (int f = 4; f < kTnfFrames; ++f) off += LumaPsnr(noisy[size_t(f)], master[size_t(f)]);
    off /= double(kTnfFrames - 4);
    std::printf("    no denoise: %.2f dB against the clean master\n", off);
    std::string why;
    const double sub = TnfArm("subtle", noisy, master, why);
    if (sub == -1) { std::printf("    not exercisable here: %s; nothing about its output is verified\n", why.c_str()); return "notrun"; }
    if (sub < 0) return "failed";
    const double bal = TnfArm("balanced", noisy, master, why), str = TnfArm("strong", noisy, master, why);
    const double nul = TnfArm("strong", master, master, why);
    std::printf("    PSNR against the clean master: no denoise %.2f, subtle %.2f, balanced %.2f, strong %.2f dB; null arm (clean master, strong) %.2f dB\n", off, sub, bal, str, nul);
    CHECK(bal >= off + 0.5, "balanced moves toward the master: %.2f vs %.2f dB", bal, off);
    CHECK(str >= off + 0.5, "strong moves toward the master: %.2f vs %.2f dB", str, off);
    CHECK(nul >= 40.0, "null arm: the clean master is not restyled (%.2f dB)", nul);
    return g_fail ? "failed" : "ok";
}

int main()
{
    std::printf("%s", ProbeReport().c_str());
    const Caps& c = Probe();
    std::printf("summary: built=%d frameProcessor=%d lowLatencyFI=%d lowLatencySR=%d frc=%d superResolution=%d noiseFilter=%d sizeQueries=%d\n", int(c.built), int(c.frameProcessor),
                int(c.lowLatencyFI), int(c.lowLatencySR), int(c.frc), int(c.superResolution), int(c.noiseFilter), int(c.sizeQueries));
    std::printf("APPLE_TNF_RESULT=%s\n", TestNoiseFilter(c).c_str());
    if (!c.AnyInterpolation() && !c.lowLatencySR) {
        Engine e; Config cfg; cfg.width = 1280; cfg.height = 720;
        std::string err, info;
        CHECK(!e.Configure(cfg, err, info), "Configure must fail when nothing is offered");
        std::printf("apple_vt_test: VideoToolbox offers no ML effect here (%s); Configure fails cleanly: %s\n", c.detail.c_str(), err.c_str());
        std::printf("APPLE_VT_RESULT=unavailable\n");
        std::printf("apple_vt_test: %d passed, %d failed\n", g_pass, g_fail);
        return g_fail ? 1 : 0;
    }
    if (c.lowLatencyFI) {
        Run("LL x2 8-bit", 1280, 720, 8, 2, "off", "lowlatency");
        Run("LL x4 8-bit", 1280, 720, 8, 4, "off", "lowlatency");
        Run("LL x8 8-bit (grid 1/8, unlock)", 1280, 720, 8, 8, "off", "lowlatency");
        Run("auto x3 (FRC, else LL eighths)", 1280, 720, 8, 3, "off", "auto");
        Run("LL x2 10-bit PQ", 1280, 720, 10, 2, "off", "lowlatency");
        Run("LL x2 1080p", 1920, 1080, 8, 2, "off", "lowlatency");
    }
    if (c.frc) {
        Run("FRC x2 8-bit", 1280, 720, 8, 2, "off", "frc");
        Run("FRC x3 10-bit", 1280, 720, 10, 3, "off", "frc");
    }
    if (c.lowLatencySR) {
        Run("SR x2 only", 640, 360, 8, 1, "2", "auto");
        if (c.AnyInterpolation()) Run("FI x2 + SR x2", 640, 360, 8, 2, "2", "auto");
    }
    // ArtCNN on Metal (no VideoToolbox: runs in a VM too) when the weights are found (PUSHMPC_MODELS)
    if (!FindModel("ArtCNN_C4F16").empty()) {
        Run("ArtCNN x2 only (Metal)", 640, 360, 8, 1, "artcnn16", "auto", false);
        Run("ArtCNN x2, 10-bit (Metal)", 640, 360, 10, 1, "artcnn16", "auto", false);
    }
    // a VM may list the effects and fail their self-test: then every chain is unavailable and the plugin falls back
    std::printf("APPLE_VT_RESULT=%s\n", g_fail ? "failed" : g_configured ? "ok" : "unavailable");
    std::printf("chains configured: %d, unavailable: %d\n", g_configured, g_unavailable);
    std::printf("apple_vt_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
