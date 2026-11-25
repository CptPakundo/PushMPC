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

#include "zero_copy_test.inc"

int main()
{
    Schedule();
    Engine();
    KodiEngine(0);
    KodiEngine(1);
    ZeroCopy();
    HostImport();
    SemiPlanar(8);
    SemiPlanar(10);
    ToneMap();
    std::printf(g_fail ? "cadence_test: %d failure(s)\n" : "cadence_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
