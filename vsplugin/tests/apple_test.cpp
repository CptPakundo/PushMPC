// apple_test: the parts of the VideoToolbox backend that need no Apple API, on every OS (CI: Windows, Linux, macOS):
//  - planar <-> bi-planar conversion: lossless round trips (8-bit via '420v', 8/9/10-bit via 'x420'), the expected
//    rounding when the bi-planar format has fewer bits, odd chroma sizes, padded strides; limited-range and PQ code
//    values pass unchanged (no range or transfer conversion);
//  - the pixel-format pick (lossless first, range tag, then lossy, then none);
//  - the scene-cut guard on synthetic pictures: a moving box, a textured pan, a fade and small noise are not cuts; a
//    hard cut to another picture and a cut between two pans are;
//  - the phase plans (k = 2..8 on the 2^x grid; FRC exact; cut / off plans) and their worst phase error;
//  - Auto's multiplier and super-resolution decisions, the step-down ladder;
//  - backend selection with the apple backend (backend.h);
//  - apple_nn.h: the .pmnn parser and the ArtCNN float reference (padding, ReLU, long skip, DCR depth-to-space, clip),
//    the 10-bit recovery (a PQ gradient pan through a simulated 8-bit interpolator), the super-resolution order and the
//    real-time check.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>
#include "apple_convert.h"
#include "apple_nn.h"
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
    for (int k = 2; k <= kAppleMaxK; ++k) {   // x9-x15 on the sixteenth grid (x = 4: max error 1/32), x16 exact
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
    CHECK(DecideAppleK(0, 23.976, 240).k == 5, "Auto 24 -> 240 Hz: x5, the even cadence (x8 is uneven and holds as long)");
    CHECK(DecideAppleK(0, 60, 60).k == 1, "Auto 60 -> 60: x1");
    CHECK(DecideAppleK(0, 29.97, 59.94).k == 2, "Auto 30 -> 60: x2");
    CHECK(DecideAppleK(1, 24, 120).k == 1 && DecideAppleK(3, 24, 60).k == 3, "fixed / off");
    CHECK(kAppleMaxK == 16 && DecideAppleK(16, 24, 60).k == 16 && DecideAppleK(20, 24, 60).k == 16 && DecideAppleK(12, 24, 60).k == 12, "fixed up to x16 (the other engines' ceiling)");
    CHECK(DecideAppleK(0, 24, 400).k == 16, "Auto 24 -> 400 Hz: x16 (shortest hold, no even cadence below it)");
    CHECK(DecideAppleK(0, 23.976, 480).k == 10, "Auto 24 -> 480 Hz: x10, the even cadence (x16 holds as long)");
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

static void PutU32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); }
static void PutF32(std::vector<uint8_t>& v, float f) { uint32_t x; std::memcpy(&x, &f, 4); PutU32(v, x); }
static std::vector<uint8_t> Serialize(const NnModel& m)
{
    std::vector<uint8_t> v = { 'P', 'M', 'N', 'N' };
    PutU32(v, 1); PutU32(v, uint32_t(m.layers.size()));
    for (auto& l : m.layers) {
        PutU32(v, uint32_t(l.cin)); PutU32(v, uint32_t(l.cout)); PutU32(v, l.flags); PutF32(v, l.lo); PutF32(v, l.hi);
        for (float f : l.w) PutF32(v, f);
        for (float f : l.b) PutF32(v, f);
    }
    return v;
}

static void TestNn()
{
    // layer 0: 1 -> 4, channel c = 0.1 (c + 1) * centre tap, saved as the skip; layer 1: 4 -> 4, all-ones 3x3 on
    // channel 0 only into output channel 0 (tests the zero padding), ReLU on a negative bias for channel 3;
    // layer 2: 4 -> 4 identity + the skip, final (depth-to-space, clip to [0, 1])
    NnModel m;
    NnLayer a; a.cin = 1; a.cout = 4; a.flags = NnLayer::kSaveSkip; a.w.assign(36, 0); a.b.assign(4, 0);
    for (int c = 0; c < 4; ++c) a.w[size_t(c) * 9 + 4] = 0.1f * float(c + 1);
    NnLayer b; b.cin = 4; b.cout = 4; b.flags = NnLayer::kRelu; b.w.assign(144, 0); b.b = { 0, 0, 0, -5 };
    for (int t = 0; t < 9; ++t) b.w[size_t(0 * 4 + 0) * 9 + size_t(t)] = 1.0f;
    for (int c = 1; c < 4; ++c) b.w[size_t(c * 4 + c) * 9 + 4] = 1.0f;
    NnLayer f; f.cin = 4; f.cout = 4; f.flags = NnLayer::kFinal | NnLayer::kAddSkip; f.w.assign(144, 0); f.b.assign(4, 0);
    for (int c = 0; c < 4; ++c) f.w[size_t(c * 4 + c) * 9 + 4] = 1.0f;
    m.layers = { a, b, f };
    const auto bytes = Serialize(m);
    NnModel p; std::string err;
    CHECK(p.Parse(bytes.data(), bytes.size(), err), "parse: %s", err.c_str());
    CHECK(p.layers.size() == 3 && p.Parameters() == m.Parameters(), "parsed sizes");
    NnModel bad; auto trunc = bytes; trunc.resize(trunc.size() - 3);
    CHECK(!bad.Parse(trunc.data(), trunc.size(), err), "truncated file refused");
    const int w = 5, h = 4;
    std::vector<float> in(size_t(w * h), 1.0f), out(size_t(4 * w * h));
    NnReference(p, in.data(), w, h, out.data(), 2);
    auto at = [&](int x, int y) { return out[size_t(y) * size_t(2 * w) + size_t(x)]; };
    // interior pixel (2, 1): channel 0 = 9 taps x 0.1 + skip 0.1 = 1.0 (clip); channel 1 = 0.2 + 0.2; channel 2 =
    // 0.3 + 0.3; channel 3 = relu(0.4 - 5) = 0, + skip 0.4
    CHECK(std::fabs(at(4, 2) - 1.0f) < 1e-5f, "DCR (0,0) = channel 0 (clipped): %f", at(4, 2));
    CHECK(std::fabs(at(5, 2) - 0.4f) < 1e-5f, "DCR (0,1) = channel 1: %f", at(5, 2));
    CHECK(std::fabs(at(4, 3) - 0.6f) < 1e-5f, "DCR (1,0) = channel 2: %f", at(4, 3));
    CHECK(std::fabs(at(5, 3) - 0.4f) < 1e-5f, "DCR (1,1) = channel 3 after ReLU + skip: %f", at(5, 3));
    // corner (0, 0): channel 0 sees 4 taps (zero padding): 0.4 + skip 0.1
    CHECK(std::fabs(at(0, 0) - 0.5f) < 1e-5f, "zero padding at the corner: %f", at(0, 0));
    std::vector<float> o2 = out; o2[7] += 0.01f;
    const double ps = PsnrUnit(out.data(), o2.data(), out.size());
    CHECK(ps > 55 && ps < 70, "PSNR of one small difference: %.2f", ps);
}

static void TestRecovery()
{
    // a smooth 10-bit gradient (codes 300..360 over 256 px: banding territory) panning 3.5 px per frame
    const int w = 256, h = 16;
    const double pan = 3.5;
    auto truth = [&](double x) { return 300.0 + 60.0 * std::clamp(x, 0.0, double(w - 1)) / double(w - 1); };
    auto plane = [&](double shift) {
        Plane16 p; p.w = w; p.h = h; p.v.resize(size_t(w * h));
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) p.v[size_t(y * w + x)] = uint16_t(std::lround(truth(x - shift)));
        return p;
    };
    const Plane16 A = plane(0), B = plane(pan);
    for (float t : { 0.25f, 0.5f, 0.75f }) {
        Plane16 T = plane(pan * t), G8; G8.w = w; G8.h = h; G8.v.resize(T.v.size());
        for (size_t i = 0; i < T.v.size(); ++i) G8.v[i] = uint16_t(To8(T.v[i]));   // an ideal 8-bit interpolator
        std::vector<float> fx(size_t(w * h), float(pan)), fy(size_t(w * h), 0.f);
        Plane16 R, R0;
        RecoverTenBitPlane(A, B, G8, fx, fy, t, R);
        RecoverTenBitPlane(A, B, G8, {}, {}, t, R0);   // no flow
        double e8 = 0, er = 0, e0 = 0; int maxr = 0, n = 0;
        for (int y = 2; y < h - 2; ++y) for (int x = 8; x < w - 8; ++x) {
            const size_t i = size_t(y * w + x);
            e8 += std::fabs(4.0 * G8.v[i] - T.v[i]); er += std::fabs(double(R.v[i]) - T.v[i]); e0 += std::fabs(double(R0.v[i]) - T.v[i]);
            maxr = std::max(maxr, std::abs(int(R.v[i]) - int(T.v[i]))); ++n;
        }
        e8 /= n; er /= n; e0 /= n;
        std::printf("recovery t=%.2f: mean error 8-bit %.3f, recovered %.3f (no flow %.3f), max %d codes\n", t, e8, er, e0, maxr);
        CHECK(er < 0.6 * e8, "t=%.2f: recovered mean error %.3f codes vs plain 8-bit %.3f", t, er, e8);
        CHECK(maxr <= 2, "t=%.2f: recovered max error %d codes", t, maxr);
        CHECK(e0 <= e8 + 0.5, "t=%.2f: without flow never much worse than 8-bit (%.3f vs %.3f)", t, e0, e8);
    }
    CHECK(Residual(1023) == 3 && Residual(2) == -2 && Residual(1) == 1 && To8(1023) == 255, "residual range");
}

static void TestSrOrder()
{
    SrInputs in; in.w = 1920; in.h = 1080; in.editing = true; in.artcnn32 = in.artcnn16 = true;
    auto r = DecideSrOrder(in);
    CHECK(r.order.size() == 3 && r.order[0].kind == SrKind::Editing && r.order[1].kind == SrKind::ArtCnn32, "live: editing first: %s", r.why.c_str());
    in.anime = true; r = DecideSrOrder(in);
    CHECK(r.order.size() == 3 && r.order[0].kind == SrKind::ArtCnn32 && r.order[2].kind == SrKind::Editing, "anime: ArtCNN first: %s", r.why.c_str());
    in.anime = false; in.editingModel = 0; r = DecideSrOrder(in);
    CHECK(r.wantModelDownload && !r.order.empty() && r.order[0].kind == SrKind::ArtCnn32, "model missing: download asked, ArtCNN now: %s", r.why.c_str());
    in.editingModel = 1; r = DecideSrOrder(in);
    CHECK(!r.wantModelDownload && !r.order.empty() && r.order[0].kind == SrKind::ArtCnn32 && r.why.find("downloading") != std::string::npos, "downloading: %s", r.why.c_str());
    in.editingModel = 2; in.w = 3840; in.h = 2160; r = DecideSrOrder(in);
    CHECK(r.order.empty(), "4K source: no SR: %s", r.why.c_str());
    SrInputs sd; sd.w = 640; sd.h = 360; sd.llFactors = { 2, 4 }; sd.llMaxDim2 = 960;
    r = DecideSrOrder(sd); CHECK(r.order.size() == 1 && r.order[0].kind == SrKind::LowLatency, "only LL: %s", r.why.c_str());
    sd.mode = "off"; r = DecideSrOrder(sd); CHECK(r.order.empty(), "off");
    sd.mode = "4"; r = DecideSrOrder(sd); CHECK(r.order.size() == 1 && r.order[0].factor == 4, "x4 asked: LL x4");
    SrInputs e; e.w = 1280; e.h = 720; e.editing = true; e.artcnn16 = true; e.mode = "artcnn";
    r = DecideSrOrder(e); CHECK(r.order.size() == 1 && r.order[0].kind == SrKind::ArtCnn16, "artcnn asked");
    double cost = 0;
    CHECK(FitsRealTime(8, 10, 2, 2, 23.976, &cost) && std::fabs(cost - 28) < 1e-9, "x2 at 24 fps: 28 ms fits");
    CHECK(!FitsRealTime(12, 10, 2, 4, 23.976), "x4 with 12 ms SR does not fit");
}

int main()
{
    TestConversion();
    TestFormats();
    TestCuts();
    TestPlans();
    TestDecisions();
    TestSelection();
    TestNn();
    TestRecovery();
    TestSrOrder();
    std::printf("apple_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
