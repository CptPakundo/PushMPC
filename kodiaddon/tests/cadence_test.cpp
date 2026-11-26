// cadence_test: the add-on's picture schedule (cadence.h) and the engine glue it drives (pm_engine.h), with no Kodi
// and no FFmpeg. It replays what CPushVideoCodec does: frames at 23.976 fps, k pictures per frame, a seek gap, a step
// back, and checks every timestamp Kodi would receive, then a moving box through the real engine.
#include "cadence.h"
#include "pm_engine.h"
#include "kodi_engine.h"
#include <string>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static void Schedule()
{
    const double fu = 1e6 * 1001.0 / 24000.0;   // 41708.33 us
    for (int k : {1, 2, 3, 4, 5, 10}) {
        std::vector<pmk::Slot> all;
        int64_t prev = 0;
        for (int i = 1; i <= 48; ++i) {
            const int64_t cur = int64_t(std::llround(i * fu / 1000.0)) * 1000;   // mkv: millisecond timestamps
            const bool c = pmk::Contiguous(prev, cur, fu);
            CHECK(c);
            for (auto& s : pmk::SlotsForPair(prev, cur, k, c, true)) all.push_back(s);
            prev = cur;
        }
        CHECK(int(all.size()) == 48 * k);
        int real = 0;
        for (size_t i = 1; i < all.size(); ++i) {
            CHECK(all[i].pts > all[i - 1].pts);                         // strictly increasing
            const double d = double(all[i].pts - all[i - 1].pts);
            CHECK(std::fabs(d - fu / k) <= 1000.0 / k + 1.0);           // on the k-grid within the 1 ms source rounding
            CHECK(d >= 2500.0 || k > 10);                               // Kodi's PTS tracker resolves >= 2.5 ms steps
        }
        for (auto& s : all) real += s.real ? 1 : 0;
        CHECK(real == 48);
        int r = 0, sc = 0;
        pmk::ReportedRate(24000, 1001, k, r, sc);
        CHECK(r == 24000 * k && sc == 1001);
    }
    // a seek (gap of 10 s) and a step back: the previous frame goes out alone
    CHECK(!pmk::Contiguous(0, 10000000, 41708.0));
    CHECK(!pmk::Contiguous(500000, 400000, 41708.0));
    CHECK(pmk::SlotsForPair(0, 10000000, 4, false, true).size() == 1);
    CHECK(pmk::SlotsForPair(0, 41708, 4, true, false).size() == 1);     // engine has no pair yet
    CHECK(pmk::Contiguous(0, 83416, 41708.0));                           // one dropped frame still interpolates
    CHECK(pmk::Contiguous(0, 200000, 0.0) && !pmk::Contiguous(0, 300000, 0.0));
}

enum { W = 320, H = 192 };
static unsigned char Y[2][W * H], U[2][W * H / 4], V[2][W * H / 4], OY[W * H], OU[W * H / 4], OV[W * H / 4];
static void Frame(int f, int x0, int lum)
{
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            Y[f & 1][y * W + x] = (unsigned char)((x >= x0 && x < x0 + 48 && y >= 64 && y < 112) ? lum : 40 + ((x / 8 + y / 8) & 1) * 4);
    std::memset(U[f & 1], 128, sizeof(U[0]));
    std::memset(V[f & 1], 128, sizeof(V[0]));
}
static double BoxCentre(const unsigned char* p)
{
    long sx = 0, n = 0;
    for (int y = 64; y < 112; y++)
        for (int x = 0; x < W; x++)
            if (p[y * W + x] > 150) { sx += x; n++; }
    return n ? double(sx) / n : -1;
}

static void Engine()
{
    pm_engine_cfg cfg{};
    cfg.width = W; cfg.height = H; cfg.bits = 8; cfg.threads = 2; cfg.quality = 1;
    char err[256] = {};
    pm_engine* e = pm_engine_open(&cfg, err, sizeof(err));
    CHECK(e != nullptr);
    if (!e) return;
    pm_yuv_out o{{OY, OU, OV}, {W, W / 2, W / 2}};
    for (int f = 0; f < 6; ++f) {
        Frame(f, 40 + 8 * f, 220);
        pm_yuv s{{Y[f & 1], U[f & 1], V[f & 1]}, {W, W / 2, W / 2}};
        CHECK(pm_engine_push(e, &s, f > 0, err, sizeof(err)) == 0);
        if (f >= 2) {
            CHECK(pm_engine_have_pair(e));
            pm_engine_generate(e, 0.5f, &o);
            const double c = BoxCentre(OY), want = 40 + 8 * f - 4 + 24;
            CHECK(std::fabs(c - want) <= 3.0);
        }
    }
    // a hard cut: the nearer real frame is repeated, never a blend
    Frame(6, 200, 90);
    std::memset(Y[0], 200, sizeof(Y[0]));
    pm_yuv s{{Y[0], U[0], V[0]}, {W, W / 2, W / 2}};
    CHECK(pm_engine_push(e, &s, 1, err, sizeof(err)) == 0);
    CHECK(pm_engine_pair_is_cut(e));
    pm_engine_close(e);
}

// the same moving box through the add-on's engine interface: the GPU engine when there is a Vulkan device (CI: lavapipe
// with PUSHMPC_VK_ALLOW_CPU=1; PM_REQUIRE_GPU=1 makes its absence a failure), else the CPU fallback
static void KodiEngine(int upscale)
{
    pmk::EngineCfg c;
    c.width = W; c.height = H; c.bits = 8; c.threads = 2; c.quality = 1;
    if (upscale) { c.upscaler = 1; c.outWidth = W * 2; c.outHeight = H * 2; }
    std::string why;
    auto e = pmk::OpenEngine(c, why);
    CHECK(e != nullptr);
    if (!e) return;
    std::printf("kodi engine: %s [%s]\n", e->Describe().c_str(), why.c_str());
    const char* req = std::getenv("PM_REQUIRE_GPU");
    if (req && req[0] == '1') CHECK(e->IsGpu());
    const int ow = e->OutWidth(), oh = e->OutHeight();
    if (upscale && e->IsGpu()) CHECK(ow == W * 2 && oh == H * 2);
    std::vector<unsigned char> y(size_t(ow) * oh), u(size_t(ow / 2) * (oh / 2)), v(u.size());
    pm_yuv_out o{{y.data(), u.data(), v.data()}, {ow, ow / 2, ow / 2}};
    std::string err;
    for (int f = 0; f < 6; ++f) {
        Frame(f, 40 + 8 * f, 220);
        pm_yuv s{{Y[f & 1], U[f & 1], V[f & 1]}, {W, W / 2, W / 2}};
        CHECK(e->Push(s, f > 0, err));
        if (f >= 2) {
            CHECK(e->HavePair());
            CHECK(e->Generate(0.5f, o, err));
            if (ow == W) {
                const double cpos = BoxCentre(y.data()), want = 40 + 8 * f - 4 + 24;
                CHECK(std::fabs(cpos - want) <= 3.0);
                if (f == 5) std::printf("kodi engine: box at t=0.5 %.2f (expected %.2f)\n", cpos, want);
            }
            if (upscale && e->IsGpu()) CHECK(e->Real(false, o, err));
        }
    }
    std::memset(Y[0], 200, sizeof(Y[0]));
    pm_yuv s{{Y[0], U[0], V[0]}, {W, W / 2, W / 2}};
    CHECK(e->Push(s, true, err));
    CHECK(e->PairIsCut());
    std::string kwhy;
    const int k = e->DecideK(0, 23.976, 120.0, kwhy);
    std::printf("kodi engine: Auto on 120 Hz: x%d (%s)\n", k, kwhy.c_str());
    CHECK(k >= 1 && k <= 5);
}

// held frames (cadence.h HeldCadence): anime drawn on twos / threes, a still, a gap, the end of the stream. 25 fps
// (40000 us a frame) so every expected timestamp is a round number.
static bool Is(const pmk::Slot& s, int64_t pts, double t, bool real, bool newest = false)
{
    return s.pts == pts && std::fabs(double(s.t) - t) < 1e-6 && s.real == real && s.newest == newest;
}
static void HeldSchedule()
{
    using V = std::vector<pmk::Slot>;
    {   // twos at x2: drawings at 0, 80000, 160000; repeats at 40000, 120000
        pmk::HeldCadence h;
        CHECK(h.Next(0, false, false, 2, false).empty());         // the first frame waits for its successor
        CHECK(h.Next(40000, true, true, 2, true).empty());        // a repeat waits for the next drawing
        V a = h.Next(80000, false, true, 2, true);                // g = 2, k = 2: real 0, then t = 1/4, 2/4, 3/4
        CHECK(a.size() == 4 && Is(a[0], 0, 0, true) && Is(a[1], 20000, 0.25, false) && Is(a[2], 40000, 0.5, false) && Is(a[3], 60000, 0.75, false));
        CHECK(h.Next(120000, true, true, 2, true).empty());
        V b = h.Next(160000, false, true, 2, true);               // real 80000, then 100000 / 120000 / 140000
        CHECK(b.size() == 4 && Is(b[0], 80000, 0, true) && Is(b[1], 100000, 0.25, false) && Is(b[3], 140000, 0.75, false));
        CHECK(h.Repeats() == 2 && h.Stills() == 0);
    }
    {   // threes at x2: g = 3, six pictures 20000 apart, t = j / 6
        pmk::HeldCadence h;
        h.Next(0, false, false, 2, false);
        CHECK(h.Next(40000, true, true, 2, true).empty() && h.Next(80000, true, true, 2, true).empty());
        V a = h.Next(120000, false, true, 2, true);
        CHECK(a.size() == 6 && Is(a[0], 0, 0, true));
        for (int j = 1; j < 6 && j < int(a.size()); ++j) CHECK(Is(a[size_t(j)], 20000 * j, j / 6.0, false));
    }
    {   // a still: the 4th repeat (160000) shows the drawing and the 3 waiting repeats as they are (the engine's newest
        // frame); a 5th repeat goes out as it comes; the next drawing moves only the last interval (t = 1/2 at x2)
        pmk::HeldCadence h;
        h.Next(0, false, false, 2, false);
        CHECK(h.Next(40000, true, true, 2, true).empty() && h.Next(80000, true, true, 2, true).empty() && h.Next(120000, true, true, 2, true).empty());
        V a = h.Next(160000, true, true, 2, true);                // real 0, 40000, 80000, 120000 (newest)
        CHECK(a.size() == 4 && Is(a[0], 0, 0, true, true) && Is(a[1], 40000, 0, true, true) && Is(a[3], 120000, 0, true, true));
        V b = h.Next(200000, true, true, 2, true);                // real 160000 (newest)
        CHECK(b.size() == 1 && Is(b[0], 160000, 0, true, true));
        V c = h.Next(240000, false, true, 2, true);               // real 200000, then 220000 at t = 0.5 of (drawing, new)
        CHECK(c.size() == 2 && Is(c[0], 200000, 0, true) && Is(c[1], 220000, 0.5, false));
        CHECK(h.Repeats() == 5 && h.Stills() == 1);
        // a gap with a repeat waiting: the base and the repeat as they are, no pair across it
        CHECK(h.Next(280000, true, true, 2, true).empty());
        V d = h.Next(9000000, false, false, 2, true);             // real 240000, real 280000
        CHECK(d.size() == 2 && Is(d[0], 240000, 0, true) && Is(d[1], 280000, 0, true));
        // the end of the stream with a repeat waiting: the base and the repeat, as they are (the engine's newest)
        CHECK(h.Next(9040000, true, true, 2, true).empty());
        V e = h.Drain();                                          // real 9000000, real 9040000 (newest)
        CHECK(e.size() == 2 && Is(e[0], 9000000, 0, true, true) && Is(e[1], 9040000, 0, true, true));
        CHECK(!h.Have());
    }
    {   // the engine has no pair (an error): the base and the repeats go out as they are
        pmk::HeldCadence h;
        h.Next(0, false, false, 2, false);
        h.Next(40000, true, true, 2, false);
        V a = h.Next(80000, false, true, 2, false);               // real 0, real 40000
        CHECK(a.size() == 2 && Is(a[0], 0, 0, true) && Is(a[1], 40000, 0, true));
    }
    // without repeats HeldCadence is SlotsForPair (the 23.976 fps clip of Schedule, x5)
    const double fu = 1e6 * 1001.0 / 24000.0;
    pmk::HeldCadence h;
    int64_t prev = 0;
    h.Next(0, false, false, 5, false);
    for (int i = 1; i <= 24; ++i) {
        const int64_t cur = int64_t(std::llround(i * fu / 1000.0)) * 1000;
        const V a = h.Next(cur, false, true, 5, true), b = pmk::SlotsForPair(prev, cur, 5, true, true);
        CHECK(a.size() == b.size());
        for (size_t j = 0; j < a.size() && j < b.size(); ++j) CHECK(a[j].pts == b[j].pts && a[j].t == b[j].t && a[j].real == b[j].real);
        prev = cur;
    }
}

// the CPU engine's duplicate test (pm_engine_set_dedup): a repeat leaves the pair as it is. The box moves 16 px per
// drawing (A at 40, B at 56, C at 72; its centre is x0 + 24)
static void EngineDedup()
{
    pm_engine_cfg cfg{};
    cfg.width = W; cfg.height = H; cfg.bits = 8; cfg.threads = 2; cfg.quality = 1;
    char err[256] = {};
    pm_engine* e = pm_engine_open(&cfg, err, sizeof(err));
    CHECK(e != nullptr);
    if (!e) return;
    pm_engine_set_dedup(e, 1);
    pm_yuv_out o{{OY, OU, OV}, {W, W / 2, W / 2}};
    pm_yuv s0{{Y[0], U[0], V[0]}, {W, W / 2, W / 2}}, s1{{Y[1], U[1], V[1]}, {W, W / 2, W / 2}};
    Frame(0, 40, 220);                                                       // A
    CHECK(pm_engine_push(e, &s0, 0, err, sizeof(err)) == 0 && !pm_engine_last_dup(e));
    CHECK(pm_engine_push(e, &s0, 1, err, sizeof(err)) == 0 && pm_engine_last_dup(e) && !pm_engine_have_pair(e));   // A again: a repeat, no pair yet
    Frame(1, 56, 220);                                                       // B
    CHECK(pm_engine_push(e, &s1, 1, err, sizeof(err)) == 0 && !pm_engine_last_dup(e) && pm_engine_have_pair(e));
    pm_engine_generate(e, 0.5f, &o);
    CHECK(std::fabs(BoxCentre(OY) - 72.0) <= 3.0);                          // (A, B) at 0.5: 48 + 24 = 72
    CHECK(pm_engine_check_dup(e, &s1, 1, err, sizeof(err)) == 1);            // B again: the test alone says repeat
    CHECK(pm_engine_push(e, &s1, 1, err, sizeof(err)) == 0 && pm_engine_last_dup(e));
    pm_engine_generate(e, 0.25f, &o);
    const double c = BoxCentre(OY);
    CHECK(std::fabs(c - 68.0) <= 3.0);                                       // still the pair (A, B): 44 + 24 = 68
    std::printf("engine dedup: a repeat kept the pair, box at t=0.25 %.2f (expected 68.00)\n", c);
    Frame(0, 72, 220);                                                       // C
    CHECK(pm_engine_check_dup(e, &s0, 1, err, sizeof(err)) == 0);            // a new drawing; the push takes the test
    CHECK(pm_engine_push(e, &s0, 1, err, sizeof(err)) == 0 && !pm_engine_last_dup(e));
    pm_engine_generate(e, 0.5f, &o);
    CHECK(std::fabs(BoxCentre(OY) - 88.0) <= 3.0);                          // (B, C) at 0.5: 64 + 24 = 88
    CHECK(pm_engine_push(e, &s0, 0, err, sizeof(err)) == 0 && !pm_engine_last_dup(e));   // not contiguous: never a repeat
    pm_engine_close(e);
}

#include "zero_copy_test.inc"

int main()
{
    Schedule();
    HeldSchedule();
    Engine();
    EngineDedup();
    KodiEngine(0);
    KodiEngine(1);
    ZeroCopy();
    ZeroCopyP010();
    HostImport();
    SemiPlanar(8);
    SemiPlanar(10);
    ToneMap();
    PlatformChecks();
    std::printf(g_fail ? "cadence_test: %d failure(s)\n" : "cadence_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
