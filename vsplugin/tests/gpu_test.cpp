// gpu_test: the portable GPU engine (engines/gpu) against the CPU reference, on synthetic clips. Runs on any Vulkan 1.2
// device; CI uses Mesa lavapipe (PUSHMPC_VK_ALLOW_CPU=1, PUSHMPC_VK_VALIDATION=1) and MoltenVK on macOS. With no
// device it says so and exits 0, unless --require.
//  - interpolation parity: the CPU reference's own fields injected, GPU vs CPU InterpolatePlanes at t = 1/4, 1/2, 3/4,
//    8 and 10 bits: at most 1 code value apart (float rounding: up to 2 % of the samples on drivers that fuse operations);
//  - motion: the GPU block matcher's field vs the CPU's, and the generated middle frame of a known sub-pixel motion
//    against the true one (GPU not worse than the CPU engine by more than 0.5 dB);
//  - PushFPS's scene-cut rule on a hard cut;
//  - the backend behind the Pipeline: a moving box at x4 (count, order, real frames byte-identical, box positions),
//    anime on twos with Dedup (the duplicate's place shows the midpoint), a hard cut (no blend);
//  - Subtitle Hold: static strokes over a moving picture stay sharper in generated frames than without it;
//  - NIS and FSR1 upscaling of a 2x-reduced picture against the original, next to bilinear;
//  - the optical-flow plumbing (VK_NV_optical_flow) when the device has it;
//  - no validation-layer errors; the calibration timings printed.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "gpu_backend.h"
#include "portable_engine.h"
#include "veyra/portable/Motion.h"
#include "../../pushmpc/include/pushmpc/content_detect.h"   // PackedCellsRef / PackedClipRef: the reference for Engine::Tap

using namespace pmvs;
using namespace pmvs::gpu;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...) do { if (cond) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s | ", __FILE__, __LINE__, #cond); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

struct Pic {
    int w = 0, h = 0, bits = 8;
    std::vector<uint16_t> p[3];
    Pic() = default;
    Pic(int W, int H, int B) : w(W), h(H), bits(B) { for (int i = 0; i < 3; ++i) p[i].assign(size_t(i ? ((W + 1) / 2) * ((H + 1) / 2) : W * H), uint16_t(128 << (B - 8))); }
    int PW(int i) const { return i ? (w + 1) / 2 : w; }
    int PH(int i) const { return i ? (h + 1) / 2 : h; }
    uint16_t& At(int i, int x, int y) { return p[i][size_t(y) * size_t(PW(i)) + size_t(x)]; }
    uint16_t At(int i, int x, int y) const { return p[i][size_t(y) * size_t(PW(i)) + size_t(x)]; }
    // packed bytes as VapourSynth would hold them
    std::vector<uint8_t> bytes[3];
    YuvView View()
    {
        YuvView v; v.width = w; v.height = h; v.bits = bits;
        const int bps = bits > 8 ? 2 : 1;
        for (int i = 0; i < 3; ++i) {
            bytes[i].resize(p[i].size() * size_t(bps));
            for (size_t k = 0; k < p[i].size(); ++k) {
                if (bps == 1) bytes[i][k] = uint8_t(p[i][k]);
                else std::memcpy(&bytes[i][k * 2], &p[i][k], 2);
            }
            v.data[i] = bytes[i].data(); v.stride[i] = ptrdiff_t(PW(i)) * bps;
        }
        return v;
    }
    YuvOut Out()
    {
        const int bps = bits > 8 ? 2 : 1;
        YuvOut o;
        for (int i = 0; i < 3; ++i) { bytes[i].assign(p[i].size() * size_t(bps), 0); o.data[i] = bytes[i].data(); o.stride[i] = ptrdiff_t(PW(i)) * bps; }
        return o;
    }
    void FromBytes()
    {
        const int bps = bits > 8 ? 2 : 1;
        for (int i = 0; i < 3; ++i)
            for (size_t k = 0; k < p[i].size(); ++k) {
                if (bps == 1) p[i][k] = bytes[i][k];
                else std::memcpy(&p[i][k], &bytes[i][k * 2], 2);
            }
    }
};

// a smooth, textured picture: f(x, y) evaluated anywhere (sub-pixel motion is exact), shifted by (dx, dy)
static double Tex(double x, double y)
{
    return 128 + 50 * std::sin(x * 0.11 + y * 0.03) * std::cos(y * 0.07 - x * 0.02) + 30 * std::sin(x * 0.031 + 1.7) * std::sin(y * 0.043 + 0.4)
         + 20 * std::sin((x + 2 * y) * 0.21);
}
static Pic Make(int w, int h, int bits, double dx, double dy, int variant = 0)
{
    Pic f(w, h, bits);
    const double s = double(1 << (bits - 8));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            // variant 1: another scene (blocky noise of 6 px cells, nothing a vector can match)
            auto cellNoise = [](int cx, int cy) { uint32_t hsh = uint32_t(cx) * 73856093u ^ uint32_t(cy) * 19349663u; hsh ^= hsh >> 13; hsh *= 0x5bd1e995u; hsh ^= hsh >> 15; return double(30 + (hsh % 200)); };
            const double v = variant ? cellNoise(int(std::floor((x - dx) / 6.0)), y / 6) : Tex(x - dx, y - dy);
            f.At(0, x, y) = uint16_t(std::clamp(std::lround(v * s), 0L, long((1 << bits) - 1)));
        }
    for (int i = 1; i < 3; ++i)
        for (int y = 0; y < f.PH(i); ++y)
            for (int x = 0; x < f.PW(i); ++x) {
                const double v = 128 + (i == 1 ? 20 : -15) * std::sin((2 * x - dx) * 0.05 + (2 * y - dy) * 0.04 + variant);
                f.At(i, x, y) = uint16_t(std::clamp(std::lround(v * s), 0L, long((1 << bits) - 1)));
            }
    return f;
}
static portable::Picture ToPicture(const Pic& f)
{
    portable::Picture p;
    for (int i = 0; i < 3; ++i) { p.p[i].w = f.PW(i); p.p[i].h = f.PH(i); p.p[i].px = f.p[i]; }
    veyra::portable::Gray g(f.w, f.h);
    const int shift = f.bits - 8;
    for (size_t k = 0; k < f.p[0].size(); ++k) g.px[k] = uint8_t(shift ? std::min(255, (int(f.p[0][k]) + (1 << (shift - 1))) >> shift) : f.p[0][k]);
    p.pyramid = std::make_shared<const veyra::portable::Pyramid>(veyra::portable::Pyramid::of(std::move(g), 4));
    return p;
}
static double PsnrY(const Pic& a, const Pic& b)
{
    double se = 0;
    const double m = double((1 << a.bits) - 1);
    for (size_t k = 0; k < a.p[0].size(); ++k) { const double d = double(a.p[0][k]) - double(b.p[0][k]); se += d * d; }
    se /= double(a.p[0].size());
    return se <= 1e-12 ? 99.0 : 10 * std::log10(m * m / se);
}

static bool g_require = false;
static std::string g_device;

static bool MakeEngine(Engine& e, Config c, std::string& err)
{
    DeviceRequest r;
    r.device = g_device;
    r.allowCpu = true;
    return e.Configure(c, r, err);
}

static void TestInterpParity(int bits, bool refined)
{
    const int W = 350, H = 238;
    Config c; c.width = W; c.height = H; c.bits = bits; c.quality = 1; c.blockMatcherOnly = true; c.refined = refined;
    Engine e;
    std::string err;
    if (!MakeEngine(e, c, err)) { CHECK(false, "configure: %s", err.c_str()); return; }
    Pic a = Make(W, H, bits, 0, 0), b = Make(W, H, bits, 5, 2);
    const portable::Picture pa = ToPicture(a), pb = ToPicture(b);
    veyra::portable::MotionSettings ms; ms.finestLevel = 1;
    const veyra::portable::MotionEstimator est(ms);
    const veyra::portable::MotionField f = est.estimate(*pa.pyramid, *pb.pyramid, nullptr), k = est.estimate(*pb.pyramid, *pa.pyramid, nullptr);
    const int sa = e.Upload(a.View(), err), sb = e.Upload(b.View(), err);
    PairStats st;
    const int pair = sa >= 0 && sb >= 0 ? e.Estimate(sa, sb, -1, st, err) : -1;
    if (pair < 0 || !e.InjectFields(pair, f, k, err)) { CHECK(false, "pair: %s", err.c_str()); return; }
    for (float t : { 0.25f, 0.5f, 0.75f }) {
        portable::Picture ref;
        if (refined) portable::InterpolatePlanesRefined(pa, pb, f, k, t, (1 << bits) - 1, ref, 1);
        else portable::InterpolatePlanes(pa, pb, f, k, t, (1 << bits) - 1, ref, 1);
        Pic g(W, H, bits);
        YuvOut o = g.Out();
        int out = -1; uint64_t ticket = 0;
        if (!e.Generate(pair, t, 0, out, ticket, err) || !e.Wait(ticket, err)) { CHECK(false, "generate: %s", err.c_str()); return; }
        e.Read(out, o);
        g.FromBytes();
        int worst = 0; size_t diff = 0, n = 0;
        for (int i = 0; i < 3; ++i)
            for (size_t q = 0; q < g.p[i].size(); ++q) {
                const int d = std::abs(int(g.p[i][q]) - int(ref.p[i].px[q]));
                worst = std::max(worst, d); diff += d ? 1 : 0; ++n;
            }
        CHECK(worst <= 1 && double(diff) <= 0.02 * double(n), "parity %s %d-bit t=%.2f: worst %d, %zu of %zu samples differ", refined ? "refined" : "classic", bits, t, worst, diff, n);
    }
}

static void TestMotion(int quality)
{
    const int W = 480, H = 272;
    const double dx = 7.4, dy = -3.2;
    Config c; c.width = W; c.height = H; c.bits = 8; c.quality = quality; c.blockMatcherOnly = true;
    Engine e;
    std::string err;
    if (!MakeEngine(e, c, err)) { CHECK(false, "configure: %s", err.c_str()); return; }
    Pic a = Make(W, H, 8, 0, 0), m = Make(W, H, 8, dx / 2, dy / 2), b = Make(W, H, 8, dx, dy);
    const int sa = e.Upload(a.View(), err), sb = e.Upload(b.View(), err);
    PairStats st;
    const int pair = e.Estimate(sa, sb, -1, st, err);
    if (pair < 0) { CHECK(false, "estimate: %s", err.c_str()); return; }
    veyra::portable::MotionField gf, gk;
    CHECK(e.ReadFields(pair, gf, gk, err), "read fields: %s", err.c_str());
    size_t good = 0;
    for (size_t i = 0; i < gf.vx.size(); ++i) good += std::hypot(gf.vx[i] - dx, gf.vy[i] - dy) <= 1.0 ? 1 : 0;
    const double share = double(good) / double(std::max<size_t>(1, gf.vx.size()));
    Pic g(W, H, 8);
    YuvOut o = g.Out();
    int out = -1; uint64_t ticket = 0;
    CHECK(e.Generate(pair, 0.5f, 0, out, ticket, err) && e.Wait(ticket, err), "generate: %s", err.c_str());
    e.Read(out, o);
    g.FromBytes();
    // the CPU engine on the same pair
    const portable::Picture pa = ToPicture(a), pb = ToPicture(b);
    veyra::portable::MotionSettings ms; ms.finestLevel = 1;
    const veyra::portable::MotionEstimator est(ms);
    const auto cf = est.estimate(*pa.pyramid, *pb.pyramid, nullptr), ck = est.estimate(*pb.pyramid, *pa.pyramid, nullptr);
    portable::Picture ref;
    portable::InterpolatePlanes(pa, pb, cf, ck, 0.5f, 255, ref, 1);
    Pic cpu(W, H, 8);
    for (int i = 0; i < 3; ++i) cpu.p[i] = ref.p[i].px;
    Pic blend(W, H, 8);
    for (size_t q = 0; q < blend.p[0].size(); ++q) blend.p[0][q] = uint16_t((a.p[0][q] + b.p[0][q] + 1) / 2);
    const double pg = PsnrY(g, m), pc = PsnrY(cpu, m), pbl = PsnrY(blend, m);
    std::printf("  motion (quality %d): %.0f %% of blocks within 1 px of the true motion; middle frame PSNR-Y: GPU %.2f dB, CPU %.2f dB, blend %.2f dB; %.1f ms\n",
                quality, share * 100, pg, pc, pbl, st.ms);
    size_t cgood = 0;
    for (size_t i = 0; i < cf.vx.size(); ++i) cgood += std::hypot(cf.vx[i] - dx, cf.vy[i] - dy) <= 1.0 ? 1 : 0;
    const double cshare = double(cgood) / double(std::max<size_t>(1, cf.vx.size()));
    std::printf("  motion (quality %d): CPU field %.0f %% within 1 px\n", quality, cshare * 100);
    CHECK(share >= std::min(0.9, cshare - 0.05), "GPU field: %.2f within 1 px (CPU %.2f)", share, cshare);
    CHECK(pg >= pc - 0.5, "GPU %.2f dB vs CPU %.2f dB", pg, pc);
    CHECK(pg > pbl + 3, "GPU %.2f dB well above a blend %.2f dB", pg, pbl);
    CHECK(!st.cutRule, "no cut on motion");
    // a hard cut
    Pic x = Make(W, H, 8, 0, 0, 1);
    const int sx = e.Upload(x.View(), err);
    PairStats cs;
    const int cp = e.Estimate(sb, sx, pair, cs, err);
    CutGuard guard;
    const int decided = cp >= 0 ? guard.Decide(cs) : 0;
    CHECK(decided != 0, "hard cut: decided %d (PushFPS's rule %d, %.2f / %.2f unexplained, agreement %.2f)", decided, cs.cutRule ? 1 : 0, cs.unexplainedF, cs.unexplainedB, cs.consistent);
}

// ---- the backend on int handles
struct Harness {
    std::map<int, Pic> outs;
    std::map<int, int> kinds;
    std::map<int, int64_t> ptsOf;
    std::vector<Pic> src;
    int next = 1;
    Pipeline<int> pipe;
    GpuBackend<int> be;
    bool Init(int k, bool dedup, bool hold, Config c, std::string& err)
    {
        if (!MakeEngine(be.engine, c, err)) return false;
        be.dedup = dedup; be.subtitleHold = hold;
        be.cadence.Configure(k, dedup ? 2 : 0, int64_t(src.size()));
        pipe.Init(k, int64_t(src.size()), SrcDur100ns(24000, 1001));
        pipe.SetLookahead(be.Lookahead());
        pipe.Reset();
        be.grid = pipe.GridOf();
        be.source = [this](int64_t s, YuvView& v, void*& keep) { if (s < 0 || s >= int64_t(src.size())) return false; v = src[size_t(s)].View(); keep = nullptr; return true; };
        be.unkeep = [](void*) {};
        be.alloc = [this, c](YuvOut& o, int& f, void*) { f = next++; Pic& p = outs[f]; p = Pic(be.engine.OutWidth(), be.engine.OutHeight(), c.bits); o = p.Out(); return true; };
        be.release = [this](int f) { outs.erase(f); };
        be.tag = [this](int64_t pts, int kind, int f) { kinds[f] = kind; ptsOf[f] = pts; };
        return true;
    }
    bool Get(int64_t n, Pic& out, int& kind, std::string& err)
    {
        int64_t from, to;
        pipe.Requests(n, from, to);
        auto r = pipe.Produce(n, from, be, 10000);
        if (!r.ok) { err = r.err; return false; }
        auto it = outs.find(r.frame);
        if (it == outs.end()) { err = "frame missing"; return false; }
        it->second.FromBytes();
        out = it->second;
        kind = r.repeated ? 2 : kinds[r.frame];
        return true;
    }
};

// a bright box on a dark background; the box's luma centroid
static Pic Box(int w, int h, int bits, double x, double y, int boxW = 40)
{
    Pic f(w, h, bits);
    const int s = 1 << (bits - 8);
    for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
            const double t = (xx >= x && xx < x + boxW && yy >= y && yy < y + boxW) ? 220 : 40 + 6 * std::sin(xx * 0.3) * std::sin(yy * 0.2);
            f.At(0, xx, yy) = uint16_t(std::lround(t * s));
        }
    return f;
}
static void Centroid(const Pic& f, double& cx, double& cy)
{
    const int s = 1 << (f.bits - 8);
    double sx = 0, sy = 0, sw = 0;
    for (int y = 0; y < f.h; ++y)
        for (int x = 0; x < f.w; ++x) {
            const double v = double(f.At(0, x, y)) / s;
            if (v > 130) { const double w = v - 130; sx += w * x; sy += w * y; sw += w; }
        }
    cx = sw > 0 ? sx / sw : -1; cy = sw > 0 ? sy / sw : -1;
}

// ContentDetect's GPU tap (Engine::Tap, tap.comp) against the CPU reference the content library's own path matches
// (pushmpc/content_detect.h PackedCellsRef / PackedClipRef): 8 and 10 bits, a non-16:9 size, chroma that varies. A word is
// luma x 256 (16 bits) and Cb, Cr codes: luma within 1/256 code, Cb / Cr within 1 code (float rounding at .5).
static void TestTap(int bits, int W, int H)
{
    Pic p = Make(W, H, bits, 3.0, 1.0);
    const int s8 = 1 << (bits - 8);
    for (int y = 0; y < p.PH(1); ++y)
        for (int x = 0; x < p.PW(1); ++x) { p.At(1, x, y) = uint16_t((40 + (x * 7 + y * 3) % 180) * s8); p.At(2, x, y) = uint16_t((200 - (x + 2 * y) % 150) * s8); }
    Config c; c.width = W; c.height = H; c.bits = bits;
    Engine e; std::string err;
    if (!MakeEngine(e, c, err)) { CHECK(false, "engine (tap, %d-bit): %s", bits, err.c_str()); return; }
    if (!e.TapAvailable()) { std::printf("  content tap (%d-bit): skipped, this build has no tap shader\n", bits); return; }
    const int s = e.Upload(p.View(), err);
    CHECK(s >= 0, "upload: %s", err.c_str());
    if (s < 0) return;
    std::vector<uint32_t> words; int cw = 0, ch = 0;
    CHECK(e.Tap(s, true, words, cw, ch, err), "Tap: %s", err.c_str());
    const auto ref = pushmpc::content::PackedCellsRef(p.p[0].data(), W, p.p[1].data(), p.p[2].data(), p.PW(1), W, H, bits);
    const auto clip = pushmpc::content::PackedClipRef(p.p[0].data(), W, p.p[1].data(), p.p[2].data(), p.PW(1), W, H, bits);
    CHECK(cw == pushmpc::content::kCellsW && ch == pushmpc::content::CellsHeight(W, H) && words.size() == ref.size() + clip.size(),
          "tap %dx%d, %zu words (expected %dx%d + 224x224)", cw, ch, words.size(), pushmpc::content::kCellsW, pushmpc::content::CellsHeight(W, H));
    if (words.size() != ref.size() + clip.size()) return;
    auto worst = [](const uint32_t* a, const uint32_t* b, size_t n, int& dy, int& dc) {
        dy = dc = 0;
        for (size_t i = 0; i < n; ++i) {
            dy = std::max(dy, std::abs(int(a[i] & 0xffffu) - int(b[i] & 0xffffu)));
            dc = std::max(dc, std::max(std::abs(int((a[i] >> 16) & 0xffu) - int((b[i] >> 16) & 0xffu)), std::abs(int(a[i] >> 24) - int(b[i] >> 24))));
        }
    };
    int dy = 0, dc = 0, cy = 0, cc = 0;
    worst(words.data(), ref.data(), ref.size(), dy, dc);
    worst(words.data() + ref.size(), clip.data(), clip.size(), cy, cc);
    CHECK(dy <= 1 && dc <= 1, "tap cells vs the CPU (%d-bit, %dx%d): luma %d / 256 code, chroma %d codes", bits, W, H, dy, dc);
    CHECK(cy <= 2 && cc <= 1, "tap thumbnail vs the CPU (%d-bit, %dx%d): luma %d / 256 code, chroma %d codes", bits, W, H, cy, cc);
    std::printf("  content tap (%d-bit, %dx%d): cells %dx%d, worst luma %d / 256, chroma %d; thumbnail luma %d / 256, chroma %d\n", bits, W, H, cw, ch, dy, dc, cy, cc);
}

// The held-drawing repair (Engine::Cells / RepairSlot): a background panning 2 px a frame (on ones) and a dark sprite on
// twos (drawn at sx in d0 = A and held in s = S, drawn at sx + 12 in d1 = B). The part's mask covers the sprite's sweep;
// RepairSlot(S, pair (A, B), t 0.5) must put the sprite halfway (sx + 6) and keep S bit-exact away from the mask.
// Tolerances: sprite centre within 1.5 px of sx + 6; background inside the mask (sprite band excluded) mean |error|
// <= 3 codes (8-bit scale) against the true frame; outside the mask + feather (12 px off) 0 codes; cell means within
// 0.05 codes of the CPU means, Cb within 0.125.
static void TestRegionRepair(int bits)
{
    const int W = 640, H = 360, SW = 24, sy = 160, sx = 300;
    const int s8 = 1 << (bits - 8);
    auto frame = [&](int f, int spriteX) {
        Pic p = Make(W, H, bits, 2.0 * f, 0);
        for (int y = sy; y < sy + SW; ++y) for (int x = spriteX; x < spriteX + SW; ++x) p.At(0, x, y) = uint16_t(4 * s8);
        return p;
    };
    Pic A = frame(0, sx), S = frame(1, sx), B = frame(2, sx + 12), T = frame(1, sx + 6);   // T: the truth at s
    Config c; c.width = W; c.height = H; c.bits = bits; c.regionRepair = true; c.sourceSlots = 6;
    Engine e; std::string err;
    if (!MakeEngine(e, c, err)) { CHECK(false, "engine (repair, %d-bit): %s", bits, err.c_str()); return; }
    if (!e.RepairAvailable()) { std::printf("  region repair (%d-bit): skipped, this build has no repair shaders\n", bits); return; }
    const int a = e.Upload(A.View(), err), s = e.Upload(S.View(), err), b = e.Upload(B.View(), err);
    CHECK(a >= 0 && s >= 0 && b >= 0, "uploads: %s", err.c_str());
    if (a < 0 || s < 0 || b < 0) return;
    // the cell grid of S (against A): means and chroma as the CPU computes them
    Engine::CellGridOut g;
    CHECK(e.Cells(s, a, g, err), "Cells: %s", err.c_str());
    const int f = std::max(2, W / 480);
    CHECK(g.cell == f && g.w == W / f && g.h == H / f, "cell grid %dx%d of %d px (expected %dx%d of %d)", g.w, g.h, g.cell, W / f, H / f, f);
    // the CPU twin the other routes use (HeldRepair.h CellsFromPlanes: a sample x 255 / (2^bits - 1), as cells.comp and
    // the HLSL CS_Held; not >> (bits - 8)); the Cb the GPU packs is rounded to 1/8 code
    const pushmpc::held::CellGrid ref = pushmpc::held::CellsFromPlanes(S.p[0].data(), S.w, S.p[1].data(), S.p[2].data(), S.PW(1), nullptr, W, H, bits);
    CHECK(ref.ok() && int(ref.w) == g.w && int(ref.h) == g.h, "CPU cell grid %ux%u (GPU %dx%d)", ref.w, ref.h, g.w, g.h);
    if (!ref.ok() || int(ref.w) != g.w || int(ref.h) != g.h) return;
    double worstMean = 0, worstCb = 0;
    for (int cy = 0; cy < g.h; cy += 7)
        for (int cx = 0; cx < g.w; cx += 7) {
            const size_t i = size_t(cy) * size_t(g.w) + size_t(cx);
            worstMean = std::max(worstMean, std::fabs(double(ref.v[i]) - g.v[i]));
            worstCb = std::max(worstCb, std::fabs(double(ref.cb[i]) - double((g.aux[i] >> 10) & 2047u) / 8.0));
        }
    CHECK(worstMean <= 0.05 && worstCb <= 0.125 + 1e-6, "cell means vs the CPU (%d-bit): worst %.3f codes, Cb %.3f", bits, worstMean, worstCb);
    // the part: the sprite's sweep plus 16 px (sx - 16 .. sx + 12 + SW + 16, sy - 16 .. sy + SW + 16) in cells
    std::vector<uint8_t> mask(size_t(g.w) * size_t(g.h), 0);
    const int x0 = (sx - 16) / f, x1 = (sx + 12 + SW + 16) / f, y0 = (sy - 16) / f, y1 = (sy + SW + 16) / f;   // + 12 px of background
    for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x) mask[size_t(y) * size_t(g.w) + size_t(x)] = 255;
    Engine::RepairPart part; part.slotA = a; part.slotB = b; part.t = 0.5f; part.mask = &mask;
    CHECK(e.RepairSlot(s, &part, 1, 1.0f, true, err), "RepairSlot: %s", err.c_str());
    int out = -1; uint64_t tk = 0;
    Pic R(W, H, bits);
    if (!e.Real(s, out, tk, err) || !e.Wait(tk, err)) { CHECK(false, "read back: %s", err.c_str()); return; }
    YuvOut o = R.Out(); e.Read(out, o); R.FromBytes();
    // the sprite's centre (dark pixels inside the mask box)
    double sxs = 0, n = 0;
    for (int y = y0 * f; y < (y1 + 1) * f; ++y) for (int x = x0 * f; x < (x1 + 1) * f; ++x) if (R.At(0, x, y) < 16 * s8) { sxs += x; ++n; }
    const double cxs = n > 0 ? sxs / n - (SW - 1) / 2.0 : -1;
    CHECK(std::fabs(cxs - (sx + 6)) <= 1.5, "repaired sprite at x %.2f (truth %d, held %d), %d-bit", cxs, sx + 6, sx, bits);
    // background inside the mask, away from the sprite (sweep band +-4 px): against the true frame
    double eb = 0; int nb = 0;
    for (int y = y0 * f; y < (y1 + 1) * f; ++y)
        for (int x = x0 * f; x < (x1 + 1) * f; ++x) {
            if (y >= sy - 4 && y < sy + SW + 4 && x >= sx - 4 && x < sx + 12 + SW + 4) continue;
            eb += std::fabs(double(R.At(0, x, y)) - double(T.At(0, x, y))) / s8; ++nb;
        }
    CHECK(nb > 0 && eb / nb <= 3.0, "background in the mask: mean |error| %.2f codes over %d px (%d-bit)", nb ? eb / nb : -1.0, nb, bits);
    // away from the mask and its feather: S as it was
    int changed = 0;
    for (int pl = 0; pl < 3; ++pl) {
        const int sc = pl ? 2 : 1;
        for (int y = 0; y < R.PH(pl); ++y)
            for (int x = 0; x < R.PW(pl); ++x) {
                const int X = x * sc, Y = y * sc;
                if (X >= x0 * f - 12 && X < (x1 + 1) * f + 12 && Y >= y0 * f - 12 && Y < (y1 + 1) * f + 12) continue;
                changed += R.At(pl, x, y) != S.At(pl, x, y);
            }
    }
    CHECK(changed == 0, "outside the mask: %d samples changed (%d-bit)", changed, bits);
    for (int sl : { a, s, b }) e.ReleaseSlot(sl);
}

static void TestBackendBox(int bits)
{
    const int W = 320, H = 240, K = 4, N = 20;
    Harness hs;
    for (int i = 0; i < N; ++i) hs.src.push_back(Box(W, H, bits, 30 + 8 * i, 90));
    Config c; c.width = W; c.height = H; c.bits = bits; c.sourceSlots = 8; c.outSlots = 6;
    std::string err;
    if (!hs.Init(K, true, false, c, err)) { CHECK(false, "init: %s", err.c_str()); return; }
    double worst = 0;
    int realSame = 0, gen = 0;
    for (int64_t n = 0; n < int64_t(N) * K - K; ++n) {
        Pic o; int kind = 0;
        if (!hs.Get(n, o, kind, err)) { CHECK(false, "frame %lld: %s", (long long)n, err.c_str()); return; }
        double cx, cy; Centroid(o, cx, cy);
        const double want = 30 + 8 * (double(n) / K) + 19.5;
        worst = std::max(worst, std::fabs(cx - want));
        if (n % K == 0) { bool same = true; const Pic& s = hs.src[size_t(n / K)]; for (int i = 0; i < 3; ++i) same = same && s.p[i] == o.p[i]; realSame += same ? 1 : 0; }
        else gen += kind == 1 ? 1 : 0;
    }
    std::printf("  box %d-bit x%d: worst position error %.2f px; %d real frames byte-identical; %d generated; %llu pairs, %.1f ms motion a pair\n", bits, K, worst, realSame, gen,
                (unsigned long long)hs.be.stats.pairs, hs.be.stats.pairs ? hs.be.stats.meMs / double(hs.be.stats.pairs) : 0.0);
    CHECK(worst <= 1.5, "box position error %.2f px", worst);
    CHECK(realSame == N - 1, "real frames identical: %d", realSame);
    CHECK(gen == (N - 1) * (K - 1), "generated: %d", gen);
    CHECK(hs.be.stats.cuts == 0, "no cut");
}

static void TestBackendTwos()
{
    const int W = 320, H = 240, K = 2, N = 24;
    Harness hs;
    for (int i = 0; i < N; ++i) hs.src.push_back(Box(W, H, 8, 30 + 10 * (i / 2 * 2), 90));   // each drawing twice, 20 px a drawing
    Config c; c.width = W; c.height = H; c.bits = 8;
    std::string err;
    if (!hs.Init(K, true, false, c, err)) { CHECK(false, "init: %s", err.c_str()); return; }
    double worst = 0, worstNo = 0;
    for (int64_t n = 0; n < int64_t(N - 4) * K; ++n) {
        Pic o; int kind = 0;
        if (!hs.Get(n, o, kind, err)) { CHECK(false, "frame %lld: %s", (long long)n, err.c_str()); return; }
        double cx, cy; Centroid(o, cx, cy);
        const double want = 30 + 10 * (double(n) / K) + 19.5;   // smooth: the drawings' motion spread over both frames
        worst = std::max(worst, std::fabs(cx - want));
    }
    // without Dedup the box stands still every other frame: the same measure is large
    Harness h2;
    h2.src = hs.src;
    if (!h2.Init(K, false, false, c, err)) { CHECK(false, "init: %s", err.c_str()); return; }
    for (int64_t n = 0; n < int64_t(N - 4) * K; ++n) {
        Pic o; int kind = 0;
        if (!h2.Get(n, o, kind, err)) break;
        double cx, cy; Centroid(o, cx, cy);
        worstNo = std::max(worstNo, std::fabs(cx - (30 + 10 * (double(n) / K) + 19.5)));
    }
    std::printf("  on twos x2: worst distance from smooth motion %.2f px with Dedup, %.2f px without; %llu duplicates found\n", worst, worstNo, (unsigned long long)hs.be.stats.duplicates);
    CHECK(hs.be.stats.duplicates >= uint64_t(N / 2 - 2), "duplicates found: %llu", (unsigned long long)hs.be.stats.duplicates);
    CHECK(worst <= 2.0, "smooth with Dedup: %.2f px", worst);
    CHECK(worstNo >= 5.0, "judder without Dedup: %.2f px", worstNo);
}

static void TestBackendCut()
{
    const int W = 320, H = 240, K = 4, N = 12;
    Harness hs;
    for (int i = 0; i < N; ++i) hs.src.push_back(i < 6 ? Make(W, H, 8, 4.0 * i, 0) : Make(W, H, 8, 3.0 * i, 0, 1));
    Config c; c.width = W; c.height = H; c.bits = 8;
    std::string err;
    if (!hs.Init(K, true, false, c, err)) { CHECK(false, "init: %s", err.c_str()); return; }
    int blends = 0;
    for (int64_t n = 0; n < int64_t(N - 3) * K; ++n) {
        Pic o; int kind = 0;
        if (!hs.Get(n, o, kind, err)) { CHECK(false, "frame %lld: %s", (long long)n, err.c_str()); return; }
        if (n > 5 * K && n < 6 * K) {
            // the cut pair's frames are one real frame or the other
            const bool isA = o.p[0] == hs.src[5].p[0], isB = o.p[0] == hs.src[6].p[0];
            blends += isA || isB ? 0 : 1;
        }
    }
    CHECK(hs.be.stats.cuts == 1, "one cut: %llu (%llu by the guard)", (unsigned long long)hs.be.stats.cuts, (unsigned long long)hs.be.stats.guardCuts);
    CHECK(blends == 0, "no blend across the cut: %d", blends);
}

// static strokes (bright with dark outline, like subtitles) in the band, the picture behind moving
static void TestSubtitleHold()
{
    const int W = 640, H = 360, K = 2, N = 32;
    Harness hs;
    std::vector<Pic> clean;
    for (int i = 0; i < N; ++i) {
        Pic f = Make(W, H, 8, 5.0 * i, 0, 1);   // a detailed picture panning behind the text
        const veyra::pipeline::overlay::Zone z = veyra::pipeline::overlay::subtitleZoneFor(W, H);
        // letters: vertical and horizontal bars 2 px wide, 14 px long, every 14 px across the band's middle
        for (int L = 0; L < 14; ++L) {
            const int x0 = int(z.x) + 30 + L * 14, y0 = int(z.y) + int(z.h) / 2 - 9;
            for (int y = y0 - 1; y < y0 + 15; ++y)
                for (int x = x0 - 1; x < x0 + 9; ++x) {
                    const bool stroke = (x >= x0 && x < x0 + 2 && y >= y0 && y < y0 + 14) || (y >= y0 + 6 && y < y0 + 8 && x >= x0 && x < x0 + 8);
                    const bool edge = !stroke && ((x >= x0 - 1 && x < x0 + 3 && y >= y0 - 1 && y < y0 + 15) || (y >= y0 + 5 && y < y0 + 9 && x >= x0 - 1 && x < x0 + 9));
                    if (stroke) f.At(0, x, y) = 235; else if (edge) f.At(0, x, y) = 20;
                }
        }
        hs.src.push_back(f);
    }
    Config c; c.width = W; c.height = H; c.bits = 8;
    std::string err;
    auto run = [&](bool hold, double& err8) -> bool {
        Harness h;
        h.src = hs.src;
        if (!h.Init(K, false, hold, c, err)) return false;
        double se = 0; size_t n = 0;
        const veyra::pipeline::overlay::Zone z = veyra::pipeline::overlay::subtitleZoneFor(W, H);
        for (int64_t o = (N - 12) * K; o < int64_t(N - 3) * K; ++o) {
            if (o % K == 0) continue;
            Pic g; int kind = 0;
            if (!h.Get(o, g, kind, err)) return false;
            // the strokes' pixels against the real frame's (they do not move)
            const Pic& r = hs.src[size_t(o / K)];
            for (int y = int(z.y); y < int(z.y + z.h); ++y)
                for (int x = int(z.x) + 25; x < int(z.x) + 25 + 14 * 14; ++x)
                    if (r.At(0, x, y) == 235) { const double d = double(g.At(0, x, y)) - 235; se += d * d; ++n; }
        }
        err8 = n ? std::sqrt(se / double(n)) : 0;
        if (hold) std::printf("  subtitle hold: %llu pairs held, %llu pixels\n", (unsigned long long)h.be.stats.heldPairs, (unsigned long long)h.be.stats.heldPixels);
        return true;
    };
    double with = 0, without = 0;
    if (!run(true, with) || !run(false, without)) { CHECK(false, "subtitle hold run: %s", err.c_str()); return; }
    std::printf("  subtitle strokes in generated frames: RMS error %.1f with Subtitle Hold, %.1f without\n", with, without);
    CHECK(with < without * 0.7, "Subtitle Hold keeps the strokes: %.1f vs %.1f", with, without);
}

static void TestUpscale(Upscaler u, float sharp = 0.5f)
{
    const int W = 384, H = 216, OW = 768, OH = 432;
    Pic big = Make(OW, OH, 8, 0, 0);
    Pic small(W, H, 8);
    for (int i = 0; i < 3; ++i)
        for (int y = 0; y < small.PH(i); ++y)
            for (int x = 0; x < small.PW(i); ++x) {
                int s = 0;
                for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) s += big.At(i, std::min(2 * x + dx, big.PW(i) - 1), std::min(2 * y + dy, big.PH(i) - 1));
                small.At(i, x, y) = uint16_t((s + 2) / 4);
            }
    Config c; c.width = W; c.height = H; c.bits = 8; c.outWidth = OW; c.outHeight = OH; c.upscaler = u; c.sharpness = sharp;
    Engine e;
    std::string err;
    if (!MakeEngine(e, c, err)) { CHECK(false, "configure: %s", err.c_str()); return; }
    const int s = e.Upload(small.View(), err);
    Pic up(OW, OH, 8);
    YuvOut o = up.Out();
    int out = -1; uint64_t ticket = 0;
    CHECK(s >= 0 && e.Real(s, out, ticket, err) && e.Wait(ticket, err), "upscale: %s", err.c_str());
    e.Read(out, o);
    up.FromBytes();
    Pic bil(OW, OH, 8);
    for (int y = 0; y < OH; ++y)
        for (int x = 0; x < OW; ++x) {
            const double sx = std::clamp((x + 0.5) * 0.5 - 0.5, 0.0, double(W - 1)), sy = std::clamp((y + 0.5) * 0.5 - 0.5, 0.0, double(H - 1));
            const int x0 = int(sx), y0 = int(sy), x1 = std::min(x0 + 1, W - 1), y1 = std::min(y0 + 1, H - 1);
            const double fx = sx - x0, fy = sy - y0;
            const double v = (small.At(0, x0, y0) * (1 - fx) + small.At(0, x1, y0) * fx) * (1 - fy) + (small.At(0, x0, y1) * (1 - fx) + small.At(0, x1, y1) * fx) * fy;
            bil.At(0, x, y) = uint16_t(std::lround(v));
        }
    const double pu = PsnrY(up, big), pb = PsnrY(bil, big);
    std::printf("  %s 2x, sharpness %.2f: PSNR-Y %.2f dB (bilinear %.2f dB)\n", UpscalerName(u), sharp, pu, pb);
    CHECK(pu > 30 && (sharp > 0.3f || pu >= pb - 1.0), "%s %.2f dB vs bilinear %.2f dB", UpscalerName(u), pu, pb);
}

static void TestOpticalFlow()
{
    Config c; c.width = 480; c.height = 272; c.bits = 8; c.quality = 2; c.blockMatcherOnly = false;
    Engine e;
    std::string err;
    if (!MakeEngine(e, c, err)) { CHECK(false, "configure: %s", err.c_str()); return; }
    if (!e.OpticalFlowActive()) { std::printf("  optical flow: VK_NV_optical_flow not on this device (skipped)\n"); return; }
    Pic a = Make(480, 272, 8, 0, 0), b = Make(480, 272, 8, 6.5, 2.0);
    const int sa = e.Upload(a.View(), err), sb = e.Upload(b.View(), err);
    PairStats st;
    const int p = e.Estimate(sa, sb, -1, st, err);
    veyra::portable::MotionField f, k;
    CHECK(p >= 0 && e.ReadFields(p, f, k, err), "optical flow: %s", err.c_str());
    size_t good = 0;
    for (size_t i = 0; i < f.vx.size(); ++i) good += std::hypot(f.vx[i] - 6.5, f.vy[i] - 2.0) <= 1.0 ? 1 : 0;
    std::printf("  optical flow: %.0f %% of blocks within 1 px, %.1f ms\n", 100.0 * double(good) / double(f.vx.size()), st.ms);
    CHECK(double(good) >= 0.8 * double(f.vx.size()), "optical-flow field");
}

static void Timings()
{
    for (auto [w, h, bits, up, blocks] : { std::tuple{ 1920, 1080, 8, 0, 0 }, std::tuple{ 1920, 1080, 10, 1, 0 }, std::tuple{ 3840, 2160, 10, 0, 0 }, std::tuple{ 3840, 2160, 10, 0, 1 } }) {
        Config c; c.width = w; c.height = h; c.bits = bits; c.blockMatcherOnly = blocks != 0;
        if (up) { c.outWidth = 3840; c.outHeight = 2160; c.upscaler = Upscaler::Fsr; }
        Engine e;
        std::string err;
        if (!MakeEngine(e, c, err)) { std::printf("  timing %dx%d: %s\n", w, h, err.c_str()); continue; }
        const GpuCalibration cal = CalibrateGpu(e, 2);
        std::printf("  timing %dx%d %d-bit%s: source frame (upload, dedup, motion) %.1f ms, one output %.1f ms%s\n", w, h, bits, up ? " -> 4K FSR1" : "", cal.meMs, cal.genMs,
                    e.OpticalFlowActive() ? " (optical flow)" : " (block matcher)");
        const auto& tm = e.Times();
        if (tm.uploads && tm.pairs) std::printf("    of which: upload %.1f ms, motion %.1f ms (means)\n", tm.uploadMs / double(tm.uploads), tm.meMs / double(tm.pairs));
    }
}

int main(int argc, char** argv)
{
    bool timings = true;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--require") == 0) g_require = true;
        else if (std::strcmp(argv[i], "--no-timings") == 0) timings = false;
        else if (std::strcmp(argv[i], "--device") == 0 && i + 1 < argc) g_device = argv[++i];
    }
    std::string why;
    const auto list = Context::List(why);
    for (size_t i = 0; i < list.size(); ++i) std::printf("  device %zu: %s\n", i, list[i].Describe().c_str());
    DeviceRequest r; r.device = g_device; r.allowCpu = true;
    auto ctx = Context::Get(r, why);
    if (!ctx) {
        std::printf("gpu_test: no Vulkan device (%s): %s\n", why.c_str(), g_require ? "FAILED (--require)" : "skipped");
        return g_require ? 1 : 0;
    }
    std::printf("gpu_test on %s%s\n", ctx->Info().Describe().c_str(), ctx->OpticalFlow() ? " (VK_NV_optical_flow)" : "");
    TestInterpParity(8, false);
    TestInterpParity(10, false);
    TestInterpParity(8, true);
    TestInterpParity(10, true);
    TestMotion(1);
    TestMotion(2);
    TestBackendBox(8);
    TestBackendBox(10);
    TestBackendTwos();
    TestBackendCut();
    TestSubtitleHold();
    TestUpscale(Upscaler::Fsr);
    TestUpscale(Upscaler::Nis);
    TestUpscale(Upscaler::Nis, 0.f);
    TestUpscale(Upscaler::Fsr, 0.f);
    TestOpticalFlow();
    TestRegionRepair(8);
    TestRegionRepair(10);
    TestTap(8, 640, 360);
    TestTap(10, 720, 480);
    if (timings) Timings();
    const uint64_t ve = ctx->ValidationErrors();
    CHECK(ve == 0, "validation errors: %llu", (unsigned long long)ve);
    std::printf("gpu_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
