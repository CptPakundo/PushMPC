// apple_test: the parts of the VideoToolbox backend that need no Apple API, on every OS (CI: Windows, Linux, macOS):
//  - planar <-> bi-planar conversion: lossless round trips (8-bit via '420v', 8/9/10-bit via 'x420'), the expected
//    rounding when the bi-planar format has fewer bits, odd chroma sizes, padded strides; limited-range and PQ code
//    values pass unchanged (no range or transfer conversion);
//  - the pixel-format pick (lossless first, range tag, then lossy, then none);
//  - the scene-cut guard on synthetic pictures: a moving box, a textured pan, a fade and small noise are not cuts; a
//    hard cut to another picture and a cut between two pans are;
//  - the phase plans (k = 2..8 on the 2^x grid; FRC exact; cut / off plans) and their worst phase error;
//  - Auto's multiplier and super-resolution decisions, the step-down ladder;
//  - backend selection with the apple backend (backend.h).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>
#include "apple_convert.h"
#include "backend.h"

using namespace pmvs;
using namespace pmvs::apple;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...) do { if (cond) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s | ", __FILE__, __LINE__, #cond); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// a planar picture with padded strides
struct Pic {
    int w = 0, h = 0, bits = 8, pad = 0;
    std::vector<uint8_t> p[3];
    ptrdiff_t stride[3] = {};
    Pic(int w_, int h_, int bits_, int pad_ = 0) : w(w_), h(h_), bits(bits_), pad(pad_)
    {
        const int bps = bits > 8 ? 2 : 1;
        for (int i = 0; i < 3; ++i) {
            const int pw = i ? ChromaW(w) : w, ph = i ? ChromaH(h) : h;
            stride[i] = ptrdiff_t(pw * bps + pad);
            p[i].assign(size_t(stride[i] * ph), 0xEE);   // padding bytes stay 0xEE
        }
    }
    int Get(int i, int x, int y) const
    {
        const uint8_t* r = p[i].data() + y * stride[i];
        if (bits <= 8) return r[x];
        uint16_t v; std::memcpy(&v, r + 2 * x, 2); return v;
    }
    void Set(int i, int x, int y, int v)
    {
        uint8_t* r = p[i].data() + y * stride[i];
        if (bits <= 8) r[x] = uint8_t(v); else { const uint16_t u = uint16_t(v); std::memcpy(r + 2 * x, &u, 2); }
    }
    PlanarView View() const { PlanarView v; v.width = w; v.height = h; v.bits = bits; for (int i = 0; i < 3; ++i) { v.data[i] = p[i].data(); v.stride[i] = stride[i]; } return v; }
    PlanarOut Out() { PlanarOut o; for (int i = 0; i < 3; ++i) { o.data[i] = p[i].data(); o.stride[i] = stride[i]; } return o; }
};
struct Bi {
    int w, h, bytes; std::vector<uint8_t> p[2]; ptrdiff_t stride[2];
    Bi(int w_, int h_, int bytes_, int pad = 16) : w(w_), h(h_), bytes(bytes_)
    {
        stride[0] = ptrdiff_t(w * bytes + pad); stride[1] = ptrdiff_t(ChromaW(w) * 2 * bytes + pad);
        p[0].assign(size_t(stride[0] * h), 0); p[1].assign(size_t(stride[1] * ChromaH(h)), 0);
    }
    BiView View() { BiView v; v.width = w; v.height = h; v.bytes = bytes; for (int i = 0; i < 2; ++i) { v.data[i] = p[i].data(); v.stride[i] = stride[i]; } return v; }
};

static void FillRandom(Pic& a, uint32_t seed, int lo, int hi)
{
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> d(lo, hi);
    for (int i = 0; i < 3; ++i)
        for (int y = 0; y < (i ? ChromaH(a.h) : a.h); ++y)
            for (int x = 0; x < (i ? ChromaW(a.w) : a.w); ++x) a.Set(i, x, y, d(rng));
}
static int MaxDiff(const Pic& a, const Pic& b)
{
    int m = 0;
    for (int i = 0; i < 3; ++i)
        for (int y = 0; y < (i ? ChromaH(a.h) : a.h); ++y)
            for (int x = 0; x < (i ? ChromaW(a.w) : a.w); ++x) m = std::max(m, std::abs(a.Get(i, x, y) - b.Get(i, x, y)));
    return m;
}
static bool PaddingIntact(const Pic& a)
{
    const int bps = a.bits > 8 ? 2 : 1;
    for (int i = 0; i < 3; ++i)
        for (int y = 0; y < (i ? ChromaH(a.h) : a.h); ++y)
            for (int x = (i ? ChromaW(a.w) : a.w) * bps; x < a.stride[i]; ++x) if (a.p[i][size_t(y * a.stride[i] + x)] != 0xEE) return false;
    return true;
}

static void TestConversion()
{
    struct Case { int w, h, bits, bytes, maxErr; const char* name; };
    const Case cases[] = {
        { 64, 36, 8, 1, 0, "8-bit via 420v" },   { 33, 17, 8, 1, 0, "8-bit odd size via 420v" },
        { 64, 36, 10, 2, 0, "10-bit via x420" }, { 33, 17, 10, 2, 0, "10-bit odd size via x420" },
        { 64, 36, 8, 2, 0, "8-bit via x420" },   { 64, 36, 9, 2, 0, "9-bit via x420" },
        { 64, 36, 16, 2, 0, "16-bit via 16-bit words" },
        { 64, 36, 10, 1, 3, "10-bit via 420v (lossy: rounds to 8 bits, clamps at 255)" },
    };
    for (const Case& c : cases) {
        Pic a(c.w, c.h, c.bits, 6), b(c.w, c.h, c.bits, 6);
        FillRandom(a, uint32_t(c.w * 31 + c.bits), 0, (1 << c.bits) - 1);
        Bi bi(c.w, c.h, c.bytes);
        PlanarToBi(a.View(), bi.View());
        BiToPlanar(bi.View(), b.Out(), c.bits);
        const int md = MaxDiff(a, b);
        CHECK(md <= c.maxErr, "%s: round trip max diff %d (allowed %d)", c.name, md, c.maxErr);
        CHECK(PaddingIntact(b), "%s: padding untouched", c.name);
    }
    // 10-bit samples sit in the high bits of x420 words; CbCr interleaved U, V
    Pic a(4, 2, 10);
    a.Set(0, 0, 0, 940); a.Set(0, 1, 0, 64); a.Set(1, 0, 0, 512); a.Set(2, 0, 0, 960); a.Set(1, 1, 0, 64); a.Set(2, 1, 0, 1);
    Bi bi(4, 2, 2, 0);
    PlanarToBi(a.View(), bi.View());
    auto w16 = [&](int plane, int i) { uint16_t v; std::memcpy(&v, bi.p[plane].data() + 2 * i, 2); return int(v); };
    CHECK(w16(0, 0) == 940 << 6 && w16(0, 1) == 64 << 6, "x420 luma high-aligned: %d %d", w16(0, 0), w16(0, 1));
    CHECK(w16(1, 0) == 512 << 6 && w16(1, 1) == 960 << 6 && w16(1, 2) == 64 << 6 && w16(1, 3) == 1 << 6, "x420 CbCr interleaved U,V");
    // a word with junk in the low 6 bits (a processor's output) rounds to the nearest 10-bit value
    Bi j(2, 2, 2, 0);
    const uint16_t words[] = { uint16_t((700 << 6) | 31), uint16_t((700 << 6) | 32), 0xFFFF, 0 };
    std::memcpy(j.p[0].data(), words, 8);
    Pic o(2, 2, 10);
    BiToPlanar(j.View(), o.Out(), 10);
    CHECK(o.Get(0, 0, 0) == 700 && o.Get(0, 1, 0) == 701 && o.Get(0, 0, 1) == 1023 && o.Get(0, 1, 1) == 0, "rounding from 16-bit words: %d %d %d %d", o.Get(0, 0, 0), o.Get(0, 1, 0), o.Get(0, 0, 1), o.Get(0, 1, 1));
    // limited range / PQ code values unchanged through 8-bit and 10-bit
    Pic pq(16, 16, 10), back(16, 16, 10);
    for (int y = 0; y < 16; ++y) for (int x = 0; x < 16; ++x) pq.Set(0, x, y, 64 + (x + 16 * y) * 3);
    for (int i = 1; i < 3; ++i) for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) pq.Set(i, x, y, 64 + 13 * (x + 8 * y));
    Bi b2(16, 16, 2);
    PlanarToBi(pq.View(), b2.View()); BiToPlanar(b2.View(), back.Out(), 10);
    CHECK(MaxDiff(pq, back) == 0, "PQ / limited code values pass unchanged");
    // CopyPlanar is bit-exact
    Pic c1(33, 17, 10, 4), c2(33, 17, 10, 4);
    FillRandom(c1, 7, 0, 1023);
    CopyPlanar(c1.View(), c2.Out());
    CHECK(MaxDiff(c1, c2) == 0 && PaddingIntact(c2), "CopyPlanar exact");
}

static void TestFormats()
{
    bool lossy = true;
    CHECK(PickFormat({ k420v, kx420 }, 8, false, &lossy) == k420v && !lossy, "8-bit limited -> 420v");
    CHECK(PickFormat({ k420v, k420f, kx420 }, 8, true) == k420f, "8-bit full -> 420f");
    CHECK(PickFormat({ k420v }, 8, true) == k420v, "only the other range tag: taken (values unchanged)");
    CHECK(PickFormat({ k420v, kx420 }, 10, false, &lossy) == kx420 && !lossy, "10-bit -> x420");
    CHECK(PickFormat({ kx420 }, 8, false, &lossy) == kx420 && !lossy, "8-bit with only x420: lossless");
    CHECK(PickFormat({ k420v }, 10, false, &lossy) == k420v && lossy, "10-bit with only 420v: lossy");
    CHECK(PickFormat({ FourCC('B', 'G', 'R', 'A') }, 8, false) == 0, "no 4:2:0 format: 0");
    CHECK(PickFormat({}, 8, false) == 0, "nothing listed: 0");
    CHECK(FourCCText(kx420) == "x420" && FourCCText(k420v) == "420v", "FourCC text");
}

// synthetic scenes (8-bit unless asked)
static Pic Scene(int kind, int t, int bits = 8)
{
    const int W = 640, H = 360;
    Pic p(W, H, bits);
    const int s = bits > 8 ? 1 << (bits - 8) : 1;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            int v = 0;
            switch (kind) {
            case 0: v = (x >= 40 + 8 * t && x < 120 + 8 * t && y >= 120 && y < 200) ? 220 : 40; break;          // moving box
            case 1: v = 128 + int(60 * std::sin((x + 12 * t) * 0.07) * std::cos(y * 0.05)); break;              // textured pan
            case 2: v = 30 + 4 * t + (x * 100) / W; break;                                                      // slow fade
            case 3: v = 128; break;                                                                             // flat grey
            case 4: v = 200 - int(50 * std::cos((y + 9 * t) * 0.04)) - (x * 60) / W; break;                     // another pan
            }
            p.Set(0, x, y, std::clamp(v, 0, 255) * s);
        }
    for (int i = 1; i < 3; ++i) for (int y = 0; y < ChromaH(H); ++y) for (int x = 0; x < ChromaW(W); ++x) p.Set(i, x, y, 128 * s);
    return p;
}

static void TestCuts()
{
    auto cut = [](const Pic& a, const Pic& b, CutScore* sc = nullptr) { const CutScore s = ScoreCut(MakeThumb(a.View()), MakeThumb(b.View())); if (sc) *sc = s; return IsCut(s); };
    CutScore s;
    if (std::getenv("APPLE_TEST_SCORES"))
        for (int kind : { 0, 1, 2, 4 }) { double mm = 0, mh = 0; for (int t = 0; t < 20; ++t) { cut(Scene(kind, t), Scene(kind, t + 1), &s); mm = std::max(mm, s.mad); mh = std::max(mh, s.hist); } std::printf("scene %d: max mad %.1f max hist %.2f\n", kind, mm, mh); }
    bool any = false;
    for (int t = 0; t < 20; ++t) any |= cut(Scene(0, t), Scene(0, t + 1), &s);
    CHECK(!any, "moving box: no cut (last mad %.1f hist %.2f)", s.mad, s.hist);
    any = false;
    for (int t = 0; t < 20; ++t) any |= cut(Scene(1, t), Scene(1, t + 1), &s);
    CHECK(!any, "textured pan: no cut (last mad %.1f hist %.2f)", s.mad, s.hist);
    any = false;
    for (int t = 0; t < 20; ++t) any |= cut(Scene(2, t), Scene(2, t + 1), &s);
    CHECK(!any, "slow fade: no cut");
    CHECK(cut(Scene(0, 10), Scene(3, 0), &s), "box -> flat grey: cut (mad %.1f hist %.2f)", s.mad, s.hist);
    CHECK(cut(Scene(1, 5), Scene(4, 0), &s), "pan -> another pan: cut (mad %.1f hist %.2f)", s.mad, s.hist);
    CHECK(cut(Scene(3, 0), Scene(1, 0), &s), "grey -> texture: cut (mad %.1f hist %.2f)", s.mad, s.hist);
    // 10-bit thumbnails match 8-bit ones
    const Thumb t8 = MakeThumb(Scene(1, 3).View()), t10 = MakeThumb(Scene(1, 3, 10).View());
    CHECK(ScoreCut(t8, t10).mad < 0.6, "10-bit thumbnail = 8-bit thumbnail (mad %.2f)", ScoreCut(t8, t10).mad);
    // noise only
    Pic n1 = Scene(1, 0), n2 = Scene(1, 0);
    std::mt19937 rng(3); std::uniform_int_distribution<int> d(-12, 12);
    for (int y = 0; y < n2.h; ++y) for (int x = 0; x < n2.w; ++x) n2.Set(0, x, y, std::clamp(n2.Get(0, x, y) + d(rng), 0, 255));
    CHECK(!cut(n1, n2, &s), "grain: no cut (mad %.1f)", s.mad);
}

static void TestPlans()
{
    for (int k = 2; k <= 8; ++k) {
        const int x = ExponentFor(k);
        const PhasePlan p = MakePlan(k, x);
        CHECK(int(p.slot.size()) == k - 1, "k=%d slots", k);
        const bool pow2 = (k & (k - 1)) == 0;
        CHECK(pow2 ? p.maxError < 1e-9 : p.maxError <= 1.0 / (1 << (x + 1)) + 1e-9, "k=%d grid 1/%d: max error %.4f", k, 1 << x, p.maxError);
        for (float t : p.phases) CHECK(t > 0 && t < 1 && std::fabs(t * (1 << x) - std::round(t * (1 << x))) < 1e-6, "k=%d phase %.4f on the grid", k, t);
        // slots in time order map to non-decreasing phases
        float lastT = 0; bool mono = true;
        for (int s : p.slot) { const float t = s == kPrev ? 0.f : s == kCur ? 1.f : p.phases[size_t(s)]; mono &= t >= lastT; lastT = t; }
        CHECK(mono, "k=%d phases in order", k);
        const PhasePlan e = MakePlan(k, x, true);
        CHECK(e.maxError < 1e-6 && int(e.phases.size()) == k - 1, "k=%d exact plan (FRC)", k);
    }
    CHECK(ExponentFor(2) == 1 && ExponentFor(4) == 2 && ExponentFor(8) == 3 && ExponentFor(3) == 3 && ExponentFor(5) == 4 && ExponentFor(6) == 4, "grid exponents");
    const PhasePlan off = MakePlan(5, 0);
    CHECK(off.phases.empty() && off.slot[0] == kPrev && off.slot[1] == kPrev && off.slot[2] == kCur && off.slot[3] == kCur, "off plan: nearer real frame");
    const PhasePlan mid = MakePlan(4, 1);
    CHECK(mid.phases.size() == 1 && mid.phases[0] == 0.5f, "one-phase plan for x4: 0.5 only");
    CHECK(MakePlan(2, 1).phases.size() == 1 && MakePlan(2, 1).phases[0] == 0.5f, "x2: one phase 0.5");
}

static void TestDecisions()
{
    CHECK(DecideAppleK(0, 23.976, 0).k == 2, "Auto, display unknown: x2");
    CHECK(DecideAppleK(0, 23.976, 120).k == 5, "Auto 24 -> 120 Hz: x5");
    CHECK(DecideAppleK(0, 23.976, 240).k == 8, "Auto 24 -> 240 Hz: capped x8");
    CHECK(DecideAppleK(0, 60, 60).k == 1, "Auto 60 -> 60: x1");
    CHECK(DecideAppleK(0, 29.97, 59.94).k == 2, "Auto 30 -> 60: x2");
    CHECK(DecideAppleK(1, 24, 120).k == 1 && DecideAppleK(3, 24, 60).k == 3, "fixed / off");
    const std::vector<float> f24 = { 2.0f, 4.0f };
    CHECK(DecideSr("auto", 1920, 1080, f24).factor == 2, "Auto SR 1080p -> x2");
    CHECK(DecideSr("auto", 1280, 720, f24).factor == 2, "Auto SR 720p -> x2");
    CHECK(DecideSr("auto", 3840, 2160, f24).factor == 0, "Auto SR 4K: off");
    CHECK(DecideSr("auto", 1920, 1080, {}).factor == 0, "Auto SR unavailable: off");
    CHECK(DecideSr("auto", 1920, 1080, f24, 1280).factor == 0, "Auto SR beyond the size limit: off");
    CHECK(DecideSr("off", 1280, 720, f24).factor == 0, "SR off");
    CHECK(DecideSr("x4", 640, 360, f24).factor == 4, "SR x4 asked");
    CHECK(DecideSr("4", 640, 360, { 2.0f }).factor == 0, "SR x4 asked, not offered: off");

    Ladder l; l.Init(24, true, 2);
    for (int i = 0; i < 100; ++i) l.Record(10);
    CHECK(l.Level() == 0 && l.SrActive() && l.Exponent() == 2, "fast pairs: full");
    int changes = 0;
    for (int i = 0; i < Ladder::kOverRun; ++i) changes += l.Record(80);
    CHECK(changes == 1 && l.Level() == 1 && !l.SrActive() && l.Exponent() == 2, "slow: drop SR first");
    for (int i = 0; i < Ladder::kOverRun - 1; ++i) l.Record(80);
    l.Record(5);   // one fast pair resets the run
    for (int i = 0; i < Ladder::kOverRun - 1; ++i) l.Record(80);
    CHECK(l.Level() == 1, "the run must be consecutive");
    l.Record(80);
    CHECK(l.Level() == 2 && l.Exponent() == 1, "then one phase");
    for (int i = 0; i < Ladder::kOverRun; ++i) l.Record(80);
    CHECK(l.Level() == 3 && l.Exponent() == 0, "then off");
    for (int i = 0; i < 200; ++i) l.Record(1);
    CHECK(l.Level() == 3, "never steps back up");
    Ladder n; n.Init(24, false, 1);
    for (int i = 0; i < Ladder::kOverRun; ++i) n.Record(80);
    CHECK(n.Level() == 3, "no SR, single phase: straight to off");
}

static void TestSelection()
{
    BackendProbe mac; mac.appleBuilt = true; mac.appleAvailable = true; mac.appleWhy = "VideoToolbox low-latency frame interpolation";
    auto c = SelectBackend("auto", mac);
    CHECK(c.ok && c.kind == BackendKind::Apple && std::string(BackendName(c.kind)) == "apple", "%s", c.why.c_str());
    mac.rifeAvailable = true;
    c = SelectBackend("auto", mac); CHECK(c.kind == BackendKind::Apple, "apple beats rife on a Mac: %s", c.why.c_str());
    c = SelectBackend("rife", mac); CHECK(c.kind == BackendKind::Rife, "rife asked");
    c = SelectBackend("portable", mac); CHECK(c.kind == BackendKind::Portable, "portable asked");
    c = SelectBackend("Apple", mac); CHECK(c.kind == BackendKind::Apple, "apple asked");
    BackendProbe vm; vm.appleBuilt = true; vm.appleAvailable = false; vm.appleWhy = "not supported";
    c = SelectBackend("auto", vm); CHECK(c.kind == BackendKind::Portable, "no VT interpolator: portable: %s", c.why.c_str());
    c = SelectBackend("apple", vm); CHECK(c.ok && c.kind == BackendKind::Portable && c.why.find("no VideoToolbox interpolator") != std::string::npos, "%s", c.why.c_str());
    BackendProbe lin;
    c = SelectBackend("apple", lin); CHECK(c.kind == BackendKind::Portable && c.why.find("macOS only") != std::string::npos, "%s", c.why.c_str());
    BackendProbe rtx; rtx.nvidiaBuilt = true; rtx.pushcoreLoaded = true; rtx.gpuFamily = 1; rtx.appleAvailable = true;
    c = SelectBackend("auto", rtx); CHECK(c.kind == BackendKind::Nvidia, "nvidia first");
    c = SelectBackend("metal", mac); CHECK(!c.ok, "unknown backend is an error");
}

int main()
{
    TestConversion();
    TestFormats();
    TestCuts();
    TestPlans();
    TestDecisions();
    TestSelection();
    std::printf("apple_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
