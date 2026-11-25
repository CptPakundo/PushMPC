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
// Exit code: 1 when an offered chain fails a check.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "apple_engine.h"

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

static void Run(const char* name, int w, int h, int bits, int k, const char* sr, const char* fi)
{
    std::printf("--- %s: %dx%d %d-bit, x%d, sr=%s, fi=%s\n", name, w, h, bits, k, sr, fi);
    Engine e;
    e.log = [](const std::string& m) { std::printf("    log: %s\n", m.c_str()); };
    Config c; c.width = w; c.height = h; c.bits = bits; c.k = k; c.sr = sr; c.fi = fi; c.srcFps = 23.976;
    std::string err, info;
    if (!e.Configure(c, err, info)) { std::printf("    not available: %s\n", err.c_str()); return; }
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

int main()
{
    std::printf("%s", ProbeReport().c_str());
    const Caps& c = Probe();
    std::printf("summary: built=%d frameProcessor=%d lowLatencyFI=%d lowLatencySR=%d frc=%d superResolution=%d sizeQueries=%d\n", int(c.built), int(c.frameProcessor),
                int(c.lowLatencyFI), int(c.lowLatencySR), int(c.frc), int(c.superResolution), int(c.sizeQueries));
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
        Run("LL x5 8-bit (grid 1/16)", 1280, 720, 8, 5, "off", "lowlatency");
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
    std::printf("APPLE_VT_RESULT=%s\n", g_fail ? "failed" : "ok");
    std::printf("apple_vt_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
