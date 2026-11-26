// portable_test: the R3 parts that need no VapourSynth and no GPU, on every OS (CI: windows, ubuntu, macos).
//  - backend selection (backend.h) and the portable CPU budget;
//  - settings.ini parsing and the Linux / macOS folders (platform.h);
//  - the planar interpolation against PushFPS's own interpolate() (bit-exact up to float rounding on grey pictures);
//  - the portable engine behind the Pipeline on synthetic clips: a moving box at 8 and 10 bits (the generated frames
//    put it where it should be), a hard cut (PushFPS's scene-cut rule fires once and no frame blends the two scenes),
//    a seek, and the output count and order;
//  - the calibration that Auto's budget uses (timings printed).
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include "backend.h"
#include "platform.h"
#include "portable_backend.h"
#include "cadence.h"
#include "transitions.h"
#include <functional>
#include "veyra/portable/Interpolate.h"

using namespace pmvs;
using namespace pmvs::portable;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...) do { if (cond) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s | ", __FILE__, __LINE__, #cond); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// ------------------------------------------------------------------------------------------------ synthetic clips
struct Frame3 { int w = 0, h = 0, bits = 8; std::vector<uint8_t> p[3]; YuvView View() const; };
YuvView Frame3::View() const
{
    YuvView v; v.width = w; v.height = h; v.bits = bits;
    const int bps = bits > 8 ? 2 : 1;
    for (int i = 0; i < 3; ++i) { v.data[i] = p[i].data(); v.stride[i] = ptrdiff_t(i ? (w + 1) / 2 : w) * bps; }
    return v;
}
static void Put(Frame3& f, int plane, int x, int y, int v8)
{
    const int w = plane ? (f.w + 1) / 2 : f.w;
    const int v = v8 << (f.bits - 8);
    if (f.bits <= 8) f.p[plane][size_t(y) * size_t(w) + size_t(x)] = uint8_t(v);
    else { const uint16_t s = uint16_t(v); std::memcpy(&f.p[plane][(size_t(y) * size_t(w) + size_t(x)) * 2], &s, 2); }
}
static int Get(const std::vector<uint8_t>& p, int w, int bits, int x, int y)
{
    if (bits <= 8) return p[size_t(y) * size_t(w) + size_t(x)];
    uint16_t s; std::memcpy(&s, &p[(size_t(y) * size_t(w) + size_t(x)) * 2], 2); return s;
}
// scene 0: a horizontal gradient with a bright textured 64x64 box (U high) at (40 + 6 s, 30 + 2 s); scene 1: another picture
constexpr int kW = 384, kH = 224, kBox = 64;
static double BoxX(double s) { return 40 + 6 * s + (kBox - 1) / 2.0; }
static double BoxY(double s) { return 30 + 2 * s + (kBox - 1) / 2.0; }
static Frame3 MakeFrame(int s, int bits, int scene)
{
    Frame3 f; f.w = kW; f.h = kH; f.bits = bits;
    const int bps = bits > 8 ? 2 : 1;
    for (int i = 0; i < 3; ++i) f.p[i].assign(size_t(i ? (kW / 2) * (kH / 2) : kW * kH) * size_t(bps), 0);
    const int bx = 40 + 6 * s, by = 30 + 2 * s;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            int v;
            if (scene == 0) v = (x >= bx && x < bx + kBox && y >= by && y < by + kBox) ? 190 + (((x - bx) / 4 + (y - by) / 4) & 1) * 50 + ((x - bx) * 7 % 13) : 30 + x * 80 / kW;
            else v = 200 - ((x / 12 + y / 12) & 1) * 150;   // a checkerboard: nothing of scene 0 explains it
            Put(f, 0, x, y, v);
        }
    for (int y = 0; y < kH / 2; ++y)
        for (int x = 0; x < kW / 2; ++x) {
            const bool inBox = scene == 0 && 2 * x >= bx && 2 * x < bx + kBox && 2 * y >= by && 2 * y < by + kBox;
            Put(f, 1, x, y, inBox ? 200 : 128);
            Put(f, 2, x, y, scene == 1 ? 90 : 128);
        }
    return f;
}
// the centroid of the pixels above a threshold (8-bit units)
static bool Centroid(const std::vector<uint8_t>& p, int w, int h, int bits, int thr8, double& cx, double& cy, int& n)
{
    double sx = 0, sy = 0; n = 0;
    const int thr = thr8 << (bits - 8);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) if (Get(p, w, bits, x, y) > thr) { sx += x; sy += y; ++n; }
    if (!n) return false;
    cx = sx / n; cy = sy / n; return true;
}

// ------------------------------------------------------------------------------------------------ test backend glue
struct Harness {
    int bits = 8, k = 4, frames = 12, cutAt = -1;   // cutAt: the first frame of scene 1
    std::map<int64_t, Frame3> src;
    struct Buf { Frame3 f; bool live = false; int64_t pts = 0; bool generated = false; };
    std::vector<Buf> bufs;
    PortableBackend<int> be;
    Pipeline<int> pipe;
    void Init()
    {
        std::string err;
        Config c; c.width = kW; c.height = kH; c.bits = bits;
        be.engine.Configure(c, err);
        const double dur = SrcDur100ns(24000, 1001);
        pipe.Init(k, frames, dur);
        pipe.Reset();
        be.grid = pipe.GridOf();
        be.source = [this](int64_t s, YuvView& v) {
            if (s < 0 || s >= frames) return false;
            auto it = src.find(s);
            if (it == src.end()) it = src.emplace(s, MakeFrame(int(s), bits, cutAt >= 0 && s >= cutAt ? 1 : 0)).first;
            v = it->second.View();
            return true;
        };
        be.alloc = [this](YuvOut& o, int& h) {
            size_t i = 0;
            while (i < bufs.size() && bufs[i].live) ++i;
            if (i == bufs.size()) bufs.emplace_back();
            Buf& b = bufs[i];
            b.live = true;
            b.f = MakeFrame(0, bits, 0);   // overwritten
            for (int p = 0; p < 3; ++p) { o.data[p] = b.f.p[p].data(); o.stride[p] = b.f.View().stride[p]; }
            h = int(i);
            return true;
        };
        be.release = [this](int h) { bufs[size_t(h)].live = false; };
        be.tag = [this](int64_t pts, bool gen, int h) { bufs[size_t(h)].pts = pts; bufs[size_t(h)].generated = gen; };
    }
    // output n as the plugin would ask for it; copies the frame out
    bool Produce(int64_t n, Frame3& out, bool& generated, bool& repeated, std::string& err)
    {
        int64_t from, to;
        pipe.Requests(n, from, to);
        auto r = pipe.Produce(n, from, be, 2000);
        if (!r.ok) { err = r.err; return false; }
        out = bufs[size_t(r.frame)].f; generated = bufs[size_t(r.frame)].generated; repeated = r.repeated;
        return true;
    }
};

static bool SamePlanes(const Frame3& a, const Frame3& b) { for (int i = 0; i < 3; ++i) if (a.p[i] != b.p[i]) return false; return true; }

// ------------------------------------------------------------------------------------------------ tests
static void TestSelect()
{
    BackendProbe p;
    p.nvidiaBuilt = true; p.pushcoreLoaded = true; p.gpuFamily = 1; p.gpuName = "RTX 5090";
    auto c = SelectBackend("auto", p);
    CHECK(c.ok && c.kind == BackendKind::Nvidia, "%s", c.why.c_str());
    c = SelectBackend("", p); CHECK(c.kind == BackendKind::Nvidia, "empty = auto");
    p.rifeAvailable = true;
    c = SelectBackend("Auto", p); CHECK(c.kind == BackendKind::Nvidia, "RTX wins over rife: %s", c.why.c_str());
    p.gpuFamily = 5;   // GTX without any GPU frame generation files: not usable -> rife
    c = SelectBackend("auto", p); CHECK(c.kind == BackendKind::Rife, "GTX, no FG files -> rife: %s", c.why.c_str());
    p.capsAvailable = 1u << 3;   // FSR FG installed: pushcore on any DirectX 12 GPU (T5b item 1)
    c = SelectBackend("auto", p); CHECK(c.kind == BackendKind::Nvidia && c.why.find("DirectX 12") != std::string::npos, "GTX + FSR FG -> pushcore over rife: %s", c.why.c_str());
    p.anyGpuDisabled = true;     // off switch: the old RTX-only rule
    c = SelectBackend("auto", p); CHECK(c.kind == BackendKind::Rife && c.why.find("PUSHMPC_DISABLE_ANYGPUCORE") != std::string::npos, "%s", c.why.c_str());
    p.anyGpuDisabled = false; p.capsAvailable = 0;
    p.rifeAvailable = false;
    c = SelectBackend("auto", p); CHECK(c.kind == BackendKind::Portable, "GTX, no FG files, no rife -> portable: %s", c.why.c_str());
    p.gpuFamily = 7;   // RDNA 3
    c = SelectBackend("auto", p); CHECK(c.kind == BackendKind::Portable && c.why.find("no GPU frame generation method") != std::string::npos, "%s", c.why.c_str());
    for (uint32_t fam : { 6u, 7u, 8u, 9u, 11u, 12u }) {   // RDNA4..1, Intel, other (Snapdragon): PushFlow installed
        p.gpuFamily = fam; p.capsAvailable = 1u << 2;
        c = SelectBackend("auto", p); CHECK(c.kind == BackendKind::Nvidia, "family %u + PushFlow -> pushcore: %s", fam, c.why.c_str());
    }
    p.gpuFamily = 0; c = SelectBackend("auto", p); CHECK(c.kind == BackendKind::Portable, "unknown family (no hardware adapter) -> not pushcore: %s", c.why.c_str());
    c = SelectBackend("pushcore", p); CHECK(c.ok && c.kind == BackendKind::Nvidia, "pushcore is an alias of nvidia");
    p.gpuFamily = 7; p.capsAvailable = 0;
    p.gpuFamily = 1; p.pushcoreLoaded = false;   // RTX, but pushcore missing
    c = SelectBackend("auto", p); CHECK(c.kind == BackendKind::Portable && c.why.find("pushcore is not present") != std::string::npos, "%s", c.why.c_str());
    p.rifeAvailable = true;
    c = SelectBackend("auto", p); CHECK(c.kind == BackendKind::Rife, "%s", c.why.c_str());
    BackendProbe lin; lin.rifeAvailable = false;   // Linux / macOS build
    c = SelectBackend("auto", lin); CHECK(c.kind == BackendKind::Portable && c.why.find("no pushcore backend") != std::string::npos, "%s", c.why.c_str());
    c = SelectBackend("nvidia", lin); CHECK(!c.ok && c.kind == BackendKind::Nvidia, "nvidia asked on a build without it");
    c = SelectBackend("nvidia", p); CHECK(c.ok && c.kind == BackendKind::Nvidia, "nvidia asked: kept even without pushcore (its own error)");
    c = SelectBackend("rife", lin); CHECK(c.kind == BackendKind::Portable, "rife asked, absent -> portable");
    {   // the ml backend (pmml): after nvidia, before rife; only with its runtime installed
        BackendProbe m; m.mlBuilt = true; m.mlAvailable = true; m.rifeAvailable = true;
        c = SelectBackend("auto", m); CHECK(c.kind == BackendKind::Ml && c.why.find("pmml") != std::string::npos, "ml before rife: %s", c.why.c_str());
        m.nvidiaBuilt = true; m.pushcoreLoaded = true; m.gpuFamily = 2;
        c = SelectBackend("auto", m); CHECK(c.kind == BackendKind::Nvidia, "RTX wins over ml: %s", c.why.c_str());
        c = SelectBackend("ML", m); CHECK(c.kind == BackendKind::Ml, "ml asked: %s", c.why.c_str());
        m.mlAvailable = false;
        c = SelectBackend("ml", m); CHECK(c.kind == BackendKind::Portable && c.why.find("not installed") != std::string::npos, "%s", c.why.c_str());
        CHECK(std::string(BackendName(BackendKind::Ml)) == "ml", "name");
    }
    lin.rifeAvailable = true;
    c = SelectBackend("RIFE", lin); CHECK(c.kind == BackendKind::Rife, "rife asked");
    c = SelectBackend("portable", p); CHECK(c.kind == BackendKind::Portable, "portable asked on RTX");
    c = SelectBackend("cuda", p); CHECK(!c.ok, "unknown backend is an error");
}

static void TestBudget()
{
    // 1080p23.976 on 240 Hz: display x10; cost 8 + 9 x 3 = 35 <= 25? no -> x6 (8 + 15 = 23 <= 25.0)
    auto r = DecidePortableK(0, 23.976, 240, 1080, 8, 3);
    CHECK(r.k == 6, "k %d: %s", r.k, r.why.c_str());
    r = DecidePortableK(0, 23.976, 60, 1080, 8, 3); CHECK(r.k == 2, "60 Hz -> x2: %d", r.k);
    r = DecidePortableK(0, 23.976, 240, 2160, 8, 3); CHECK(r.k == 2 && r.why.find("capped") != std::string::npos, "4K capped at x2: %s", r.why.c_str());
    r = DecidePortableK(0, 23.976, 240, 2160, 40, 10); CHECK(r.k == 1 && r.why.find("too slow") != std::string::npos, "slow CPU: %s", r.why.c_str());
    r = DecidePortableK(0, 23.976, 0, 1080, 5, 2); CHECK(r.k == 2, "unknown display -> x2");
    r = DecidePortableK(0, 59.94, 60, 1080, 1, 1); CHECK(r.k == 1, "60 fps on 60 Hz -> x1");
    r = DecidePortableK(4, 23.976, 60, 2160, 40, 10); CHECK(r.k == 4 && r.why.find("over the CPU budget") != std::string::npos, "fixed x4 kept: %s", r.why.c_str());
    r = DecidePortableK(1, 23.976, 240, 1080, 1, 1); CHECK(r.k == 1, "off");
    r = DecidePortableK(0, 29.97, 120, 720, 2, 1); CHECK(r.k == 4, "720p30 on 120 Hz -> x4: %d", r.k);
    // up to kPortableMaxK (16, PUSHCORE_MAX_MULTIPLIER): the display's multiple when it fits the budget
    r = DecidePortableK(0, 23.976, 240, 1080, 1, 1, true); CHECK(r.k == 10, "GPU 24p on 240 Hz -> x10: %d %s", r.k, r.why.c_str());
    r = DecidePortableK(0, 23.976, 360, 1080, 1, 1, true); CHECK(r.k == 15, "GPU 24p on 360 Hz -> x15: %d", r.k);
    r = DecidePortableK(0, 23.976, 480, 1080, 1, 1, true); CHECK(r.k == kPortableMaxK, "GPU 24p on 480 Hz -> x16: %d", r.k);
    r = DecidePortableK(0, 23.976, 240, 2160, 1, 1); CHECK(r.k == 2, "CPU 4K still x2: %d", r.k);
    r = DecidePortableK(12, 23.976, 240, 1080, 1, 1); CHECK(r.k == 12, "fixed x12: %d", r.k);
    r = DecidePortableK(20, 23.976, 240, 1080, 1, 1); CHECK(r.k == kPortableMaxK, "fixed above the ceiling -> x16: %d", r.k);
}

static void TestPlatform()
{
    auto m = ParseIni("# PushMPC\n[PushMPC]\nSmoother = 4\r\n; c\nEnable=0\nPortableQuality=\"fast\"\nbad line\nSmoother=3\n");
    CHECK(m["smoother"] == "3" && m["enable"] == "0" && m["portablequality"] == "fast" && m.count("bad line") == 0, "ini");
    auto get = [&](const char* n) { auto it = m.find(LowerAscii(n)); return it == m.end() ? std::string() : it->second; };
    PortableSettings s = LoadPortableSettings(get);
    CHECK(!s.enable && s.smoother == 3 && s.quality == 0 && s.threads == 0, "settings");
    CHECK(ParseSmoother("Auto", 5) == 0 && ParseSmoother("Off", 0) == 1 && ParseSmoother("x4", 0) == 4 && ParseSmoother("9", 0) == 9 && ParseSmoother("x16", 0) == 16 && ParseSmoother("17", 0) == 0 && ParseSmoother("", 7) == 7, "smoother words");
    CHECK(ResolveSettingsIni("", "/home/u") == "/home/u/.config/pushmpc/settings.ini", "ini home");
    CHECK(ResolveSettingsIni("/x/cfg", "/home/u") == "/x/cfg/pushmpc/settings.ini", "ini xdg");
    CHECK(ResolveSettingsIni("relative", "/home/u") == "/home/u/.config/pushmpc/settings.ini", "relative XDG ignored (spec)");
    CHECK(ResolvePosixLogDir("", "", "/home/u", false) == "/home/u/.local/state/pushmpc/logs", "linux logs");
    CHECK(ResolvePosixLogDir("", "/st", "/home/u", false) == "/st/pushmpc/logs", "xdg state");
    CHECK(ResolvePosixLogDir("", "/st", "/Users/u", true) == "/Users/u/Library/Logs/PushMPC", "mac logs");
    CHECK(ResolvePosixLogDir("/tmp/l", "", "/home/u", true) == "/tmp/l", "override");
}

// the planar port against PushFPS's interpolate() on a grey picture (R = G = B = Y)
static void TestPortMatchesCore()
{
    using namespace veyra::portable;
    const Frame3 fa = MakeFrame(2, 8, 0), fb = MakeFrame(3, 8, 0);
    Picture pa, pb;
    Bgra ba(kW, kH), bb(kW, kH);
    for (int i = 0; i < 3; ++i) {
        const int w = i ? kW / 2 : kW, h = i ? kH / 2 : kH;
        pa.p[i].w = pb.p[i].w = w; pa.p[i].h = pb.p[i].h = h;
        pa.p[i].px.resize(size_t(w) * h); pb.p[i].px.resize(size_t(w) * h);
        for (size_t j = 0; j < pa.p[i].px.size(); ++j) { pa.p[i].px[j] = fa.p[i][j]; pb.p[i].px[j] = fb.p[i][j]; }
    }
    for (int y = 0; y < kH; ++y) for (int x = 0; x < kW; ++x) {
        uint8_t* a = ba.pixel(x, y); uint8_t* b = bb.pixel(x, y);
        a[0] = a[1] = a[2] = fa.p[0][size_t(y) * kW + x]; b[0] = b[1] = b[2] = fb.p[0][size_t(y) * kW + x];
    }
    const Pyramid ya = Pyramid::of(lumaOf(ba), 4), yb = Pyramid::of(lumaOf(bb), 4);
    const MotionEstimator est;
    const MotionField f = est.estimate(ya, yb), k = est.estimate(yb, ya);
    CHECK(!f.sceneCut && !k.sceneCut, "no cut on a moving box");
    for (float t : { 0.25f, 0.5f, 0.75f }) {
        Bgra core;
        interpolate(ba, bb, f, k, nullptr, t, core, 1);
        Picture mine;
        InterpolatePlanes(pa, pb, f, k, t, 255, mine, 3);
        int maxd = 0; size_t diff = 0;
        for (int y = 0; y < kH; ++y) for (int x = 0; x < kW; ++x) {
            const int d = std::abs(int(core.pixel(x, y)[1]) - int(mine.p[0].px[size_t(y) * kW + x]));
            maxd = std::max(maxd, d); diff += d ? 1 : 0;
        }
        CHECK(maxd <= 1 && diff * 1000 <= size_t(kW) * kH, "t %.2f: max diff %d, %zu pixels differ", double(t), maxd, diff);
    }
}

static void TestMovingBox(int bits)
{
    Harness hs; hs.bits = bits; hs.k = 4; hs.frames = 12;
    hs.Init();
    double worst = 0, worstC = 0; int generated = 0;
    std::string err;
    for (int64_t n = 0; n < 4 * 10; ++n) {
        Frame3 f; bool gen = false, rep = false;
        if (!hs.Produce(n, f, gen, rep, err)) { CHECK(false, "produce %lld: %s", (long long)n, err.c_str()); return; }
        const double s = double(n) / hs.k;
        CHECK(gen == (n % hs.k != 0) && !rep, "n %lld: generated %d repeated %d", (long long)n, gen, rep);
        if (n % hs.k == 0) { CHECK(SamePlanes(f, MakeFrame(int(n / hs.k), bits, 0)), "real frame %lld unchanged", (long long)n); continue; }
        ++generated;
        double cx, cy; int cnt;
        if (!Centroid(f.p[0], kW, kH, bits, 150, cx, cy, cnt)) { CHECK(false, "no box in %lld", (long long)n); continue; }
        const double e = std::hypot(cx - BoxX(s), cy - BoxY(s));
        worst = std::max(worst, e);
        CHECK(e <= 1.25 && cnt > kBox * kBox * 8 / 10 && cnt < kBox * kBox * 12 / 10, "%d-bit n %lld: box at %.2f,%.2f expected %.2f,%.2f (%d px)", bits, (long long)n, cx, cy, BoxX(s), BoxY(s), cnt);
        if (Centroid(f.p[1], kW / 2, kH / 2, bits, 164, cx, cy, cnt)) {
            const double ec = std::hypot(2 * cx + 0.5 - BoxX(s), 2 * cy + 0.5 - BoxY(s));
            worstC = std::max(worstC, ec);
            CHECK(ec <= 2.0, "%d-bit n %lld: chroma box off by %.2f px (luma px; one chroma sample = 2)", bits, (long long)n, ec);
        } else CHECK(false, "no chroma box in %lld", (long long)n);
    }
    CHECK(generated == 30, "generated %d", generated);
    CHECK(hs.be.engine.Stat().cuts == 0, "no cuts on a moving box: %llu", (unsigned long long)hs.be.engine.Stat().cuts);
    std::printf("  moving box %d-bit: %d generated frames, worst position error %.3f px (luma), %.3f px (chroma)\n", bits, generated, worst, worstC);
}

static void TestHardCut()
{
    Harness hs; hs.bits = 8; hs.k = 4; hs.frames = 12; hs.cutAt = 6;
    hs.Init();
    std::string err;
    const Frame3 before = MakeFrame(5, 8, 0), after = MakeFrame(6, 8, 1);
    for (int64_t n = 0; n < 4 * 10; ++n) {
        Frame3 f; bool gen = false, rep = false;
        if (!hs.Produce(n, f, gen, rep, err)) { CHECK(false, "produce %lld: %s", (long long)n, err.c_str()); return; }
        if (n > 5 * 4 && n < 6 * 4) {   // the pair (5, 6): the nearer real frame, never a blend
            const int j = int(n % 4);
            CHECK(SamePlanes(f, j * 2 < 4 ? before : after), "cut pair n %lld (t %.2f) is the nearer real frame", (long long)n, j / 4.0);
        }
    }
    CHECK(hs.be.engine.Stat().cuts == 1, "exactly one scene cut: %llu", (unsigned long long)hs.be.engine.Stat().cuts);
}

static void TestSeekAndOrder()
{
    Harness hs; hs.bits = 8; hs.k = 3; hs.frames = 40;
    hs.Init();
    std::string err;
    Frame3 f; bool gen, rep;
    for (int64_t n = 0; n < 9; ++n) CHECK(hs.Produce(n, f, gen, rep, err), "seq %lld: %s", (long long)n, err.c_str());
    // a seek forward far beyond kMaxGap, then a generated frame first
    const int64_t n = 30 * 3 + 1;
    CHECK(hs.Produce(n, f, gen, rep, err) && gen && !rep, "after the seek: %s", err.c_str());
    double cx, cy; int cnt;
    CHECK(Centroid(f.p[0], kW, kH, 8, 150, cx, cy, cnt) && std::hypot(cx - BoxX(30 + 1 / 3.0), cy - BoxY(30 + 1 / 3.0)) <= 1.0, "seek: box at %.2f,%.2f", cx, cy);
    CHECK(hs.pipe.Count().discontinuities >= 2, "the seek was a reset: %llu", (unsigned long long)hs.pipe.Count().discontinuities);
    // the last source frame: its generated outputs fall back to the real frame
    const int64_t last = 39 * 3 + 2;
    CHECK(hs.Produce(39 * 3, f, gen, rep, err) && hs.Produce(last, f, gen, rep, err) && rep, "the clip's end repeats the last real frame: %s", err.c_str());
    size_t live = 0; for (auto& b : hs.bufs) live += b.live ? 1 : 0;
    CHECK(live <= 96 + 8, "cache bounded: %zu frames live", live);
}

// ------------------------------------------------------------------------------------------------ GPU engine logic
// cadence.h (Dedup's re-timing), the GPU K rule and the upscale decision: pure C++, no GPU
static std::map<int64_t, pmvs::gpu::Shot> RunCadence(int k, int look, int64_t frames, const std::vector<bool>& dup)
{
    pmvs::gpu::Cadence c;
    c.Configure(k, look, frames);
    std::map<int64_t, pmvs::gpu::Shot> out;
    for (int64_t s = 0; s < int64_t(dup.size()); ++s) {
        c.Push(s, dup[size_t(s)]);
        std::vector<pmvs::gpu::Settled> st;
        c.Settle(st);
        for (auto& x : st) out[x.n] = x.shot;
    }
    return out;
}
static void TestCadence()
{
    using pmvs::gpu::Shot;
    auto approx = [](float a, float b) { return std::fabs(a - b) < 1e-5f; };
    // no Dedup: plain interpolation, every output settled at once
    auto m = RunCadence(4, 0, 100, std::vector<bool>(6, false));
    CHECK(m.size() == 6 + 5 * 3, "plain: %d outputs", int(m.size()));
    CHECK(m[4].kind == Shot::Real && m[4].src == 1, "plain real");
    CHECK(m[5].kind == Shot::Generated && m[5].d0 == 1 && m[5].d1 == 2 && approx(m[5].t, 0.25f), "plain generated 1/4");
    CHECK(m[7].kind == Shot::Generated && approx(m[7].t, 0.75f), "plain generated 3/4");
    // anime on twos (A A B B C C D D), x2, lookahead 2: the duplicates' places get the midpoints
    m = RunCadence(2, 2, 100, { false, true, false, true, false, true, false, true });
    CHECK(m.count(1) && m[1].kind == Shot::Generated && m[1].d0 == 0 && m[1].d1 == 2 && approx(m[1].t, 0.25f), "twos: tau 0.5 -> 0.25 of A..B");
    CHECK(m.count(2) && m[2].kind == Shot::Generated && approx(m[2].t, 0.5f), "twos: the duplicate's place is the midpoint");
    CHECK(m.count(3) && m[3].kind == Shot::Generated && approx(m[3].t, 0.75f), "twos: tau 1.5");
    CHECK(m.count(4) && m[4].kind == Shot::Real && m[4].src == 2, "twos: B real");
    CHECK(!m.count(12), "twos: not settled beyond the lookahead");
    // threes (A A A B B B), x2
    m = RunCadence(2, 2, 100, { false, true, true, false, true, true, false });
    CHECK(m.count(2) && m[2].kind == Shot::Generated && m[2].d0 == 0 && m[2].d1 == 3 && approx(m[2].t, 1.f / 3.f), "threes: 1/3");
    CHECK(m.count(4) && m[4].kind == Shot::Generated && approx(m[4].t, 2.f / 3.f), "threes: 2/3");
    // a still picture longer than the lookahead: held, only its last interval moves
    m = RunCadence(2, 2, 100, { false, true, true, true, true, true, false, false, false });
    CHECK(m.count(4) && m[4].kind == Shot::Hold && m[4].src == 2, "still: held");
    CHECK(m.count(10) && m[10].kind == Shot::Hold, "still: tau 5 held");
    CHECK(m.count(11) && m[11].kind == Shot::Generated && m[11].d0 == 0 && m[11].d1 == 6 && approx(m[11].t, 0.5f), "still: the last interval moves");
    // the end of the clip settles everything
    m = RunCadence(2, 2, 4, { false, false, false, false });
    CHECK(m.count(6) && m[6].kind == Shot::Real && m[6].src == 3, "end: all settled");
    // the GPU's K rule has no 1080p cap; the upscale decision
    auto r = DecidePortableK(0, 23.976, 240, 2160, 2, 1, true);
    CHECK(r.k == 8, "GPU at 4K: x8 when it fits (%d)", r.k);
    auto u = DecideUpscale("auto", "screen", 3840, 2160, 1920, 1080);
    CHECK(u.kind == kAutoUpscaler && u.w == 3840 && u.h == 2160, "upscale to the screen: %s", u.why.c_str());
    u = DecideUpscale("nis", "screen", 0, 0, 1920, 1080); CHECK(u.kind == GpuUpscaler::None, "unknown screen: none");
    u = DecideUpscale("auto", "4k", 0, 0, 1920, 800); CHECK(u.w == 3840 && u.h == 1600, "scope 4K keeps the aspect: %dx%d", u.w, u.h);
    u = DecideUpscale("off", "4k", 0, 0, 1280, 720); CHECK(u.kind == GpuUpscaler::None, "off");
    u = DecideUpscale("fsr", "screen", 1920, 1080, 1920, 1080); CHECK(u.kind == GpuUpscaler::None, "same size: none");
    CHECK(ParseQuality("high", -1) == 2 && ParseQuality("Auto", 1) == -1 && ParseTri("on", -1) == 1 && ParseTri("x", 0) == 0, "words");
}

// transitions.h: a dissolve is a linear ramp, motion is not; a flash is a bright frame between two matching ones
static pmvs::gpu::Thumb MakeThumb(int w, int h, std::function<int(int, int)> f)
{
    pmvs::gpu::Thumb t; t.w = w; t.h = h; t.px.resize(size_t(w) * size_t(h));
    double s = 0;
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) { const int v = std::clamp(f(x, y), 0, 255); t.px[size_t(y) * size_t(w) + size_t(x)] = uint8_t(v); s += v; }
    t.mean = s / double(t.px.size());
    return t;
}
static void TestTransitions()
{
    using namespace pmvs::gpu;
    auto sceneA = [](int x, int y) { return 60 + int(50 * std::sin(x * 0.4) * std::cos(y * 0.3)); };
    auto sceneB = [](int x, int y) { return 180 - int(40 * std::sin(x * 0.25 + y * 0.2)); };
    auto mix = [&](double a) { return MakeThumb(64, 36, [&](int x, int y) { return int(std::lround(sceneA(x, y) * (1 - a) + sceneB(x, y) * a)); }); };
    CHECK(LinearTransition(mix(0.25), mix(0.5), mix(0.75)), "dissolve: linear");
    auto fade = [&](double g) { return MakeThumb(64, 36, [&](int x, int y) { return int(std::lround(sceneA(x, y) * g)); }); };
    CHECK(LinearTransition(fade(1.0), fade(0.8), fade(0.6)), "fade: linear");
    auto moved = [&](int dx) { return MakeThumb(64, 36, [&](int x, int y) { return sceneA(x + dx, y); }); };
    CHECK(!LinearTransition(moved(0), moved(2), moved(4)), "motion: not linear");
    CHECK(!LinearTransition(moved(0), moved(0), moved(0)), "still: no transition");
    auto bright = MakeThumb(64, 36, [&](int x, int y) { return sceneA(x, y) + 70; });
    CHECK(Flash(moved(0), bright, moved(0)), "flash");
    CHECK(!Flash(fade(0.6), fade(0.8), fade(1.0)), "a fade is not a flash");
    CHECK(!Flash(moved(0), moved(1), moved(2)), "motion is not a flash");
}

static void TestCalibrate()
{
    const Calibration c = Calibrate(1920, 1080, 8, 0, 1);
    CHECK(c.meMs > 0 && c.genMs > 0, "calibration");
    const auto k = DecidePortableK(0, 23.976, 240, 1080, c.meMs, c.genMs);
    std::printf("  calibration 1080p 8-bit, %d threads: motion %.1f ms, one generated frame %.1f ms -> %s\n", c.threads, c.meMs, c.genMs, k.why.c_str());
    const Calibration c1 = Calibrate(1920, 1080, 8, 1, 1);
    std::printf("  calibration 1080p 8-bit, 1 thread: motion %.1f ms, one generated frame %.1f ms\n", c1.meMs, c1.genMs);
    const Calibration c4 = Calibrate(3840, 2160, 10, 0, 1);
    const auto k4 = DecidePortableK(0, 23.976, 240, 2160, c4.meMs, c4.genMs);
    std::printf("  calibration 2160p 10-bit, %d threads: motion %.1f ms, one generated frame %.1f ms -> %s\n", c4.threads, c4.meMs, c4.genMs, k4.why.c_str());
}

int main()
{
    TestSelect();
    TestBudget();
    TestPlatform();
    TestPortMatchesCore();
    TestMovingBox(8);
    TestMovingBox(10);
    TestHardCut();
    TestSeekAndOrder();
    TestCadence();
    TestTransitions();
    TestCalibrate();
    std::printf("portable_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
