// Unit tests for pushmpc.Enhance's scheduling, rate, reuse, bandwidth and Auto glue (no VapourSynth, no GPU).
#include <cstdio>
#include <deque>
#include <random>
#include <set>
#include "autopick.h"
#include "schedule.h"
#include "mp_params.h"

using namespace pmvs;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// a model of pushcore's output order: on submit s, the generated frames of (s - 1, s), then s's real frame
struct FakeCore : Backend<int> {
    int k = 1; double dur = 417083.3; int skipEvery = 0; int pairs = 0;
    int64_t prevPts = 0; bool havePrev = false;
    std::deque<Out<int>> q; int busyLeft = 0;
    int live = 0, flushes = 0, submits = 0; uint64_t now = 0;
    std::vector<int64_t> submitted;
    bool CanSubmit() override { if (busyLeft > 0) { --busyLeft; return false; } return true; }
    bool Submit(int64_t s, int64_t pts, bool disc, std::string&) override
    {
        submitted.push_back(disc ? -1 - s : s);
        ++submits;
        if (disc) havePrev = false;
        if (havePrev && k > 1 && !(skipEvery && ++pairs % skipEvery == 0))
            for (int i = 1; i < k; ++i) { q.push_back({ prevPts + int64_t(std::llround(double(pts - prevPts) * i / k)), true, 1 }); ++live; }
        q.push_back({ pts, false, 1 }); ++live;
        prevPts = pts; havePrev = true;
        return true;
    }
    bool Poll(uint32_t timeoutMs, std::vector<Out<int>>& out, std::string&) override
    {
        now += timeoutMs ? timeoutMs : 1;
        while (!q.empty()) { out.push_back(q.front()); q.pop_front(); }
        return true;
    }
    bool Flush(std::string&) override { live -= int(q.size()); q.clear(); havePrev = false; ++flushes; return true; }
    void Release(int) override { --live; }
    uint64_t NowMs() override { return now; }
};

static void TestRates()
{
    Rational r = OutputRate(24000, 1001, 5);
    CHECK(r.num == 120000 && r.den == 1001);
    r = OutputRate(25, 1, 4); CHECK(r.num == 100 && r.den == 1);
    r = OutputRate(0, 1, 4); CHECK(r.num == 0);
    r = OutputDuration(1001, 24000, 5); CHECK(r.num == 1001 && r.den == 120000);
    r = OutputDuration(1, 24, 2); CHECK(r.num == 1 && r.den == 48);
    r = OutputDuration(2, 48, 4); CHECK(r.num == 1 && r.den == 96);
    CHECK(OutputFrames(100, 8) == 800);
    Pos p = Map(17, 4); CHECK(p.src == 4 && p.sub == 1);
    int64_t lo, hi;
    Needs(16, 4, 10, lo, hi); CHECK(lo == 4 && hi == 4);
    Needs(17, 4, 10, lo, hi); CHECK(lo == 4 && hi == 5);
    Needs(39, 4, 10, lo, hi); CHECK(lo == 9 && hi == 9);     // the clip's last generated slots: the last frame only
    CHECK(NearestReal(17, 4, 10) == 4 && NearestReal(19, 4, 10) == 5 && NearestReal(18, 4, 10) == 4);
    CHECK(NearestReal(39, 4, 10) == 9);
    Grid g; g.k = 5; g.srcDur100ns = SrcDur100ns(24000, 1001);
    for (int64_t s = 0; s < 2000; s += 37) {
        CHECK(g.Index(g.Pts(s)) == s * 5);
        const int64_t a = g.Pts(s), b = g.Pts(s + 1);
        for (int i = 1; i < 5; ++i) CHECK(g.Index(a + int64_t(std::llround(double(b - a) * i / 5))) == s * 5 + i);
    }
}

static void TestPlan()
{
    // sequential: submit what is new
    Plan p = PlanSubmit(5, 4, 5, 4, false, false); CHECK(!p.discontinuity && p.from == 5 && p.to == 5);
    // nothing to do when it is cached
    p = PlanSubmit(5, 4, 5, 4, true, false); CHECK(p.to < p.from);
    // a short gap forward is filled in
    p = PlanSubmit(5, 8, 9, 5, false, false); CHECK(!p.discontinuity && p.from == 5 && p.to == 9);
    CHECK(RequestFrom(5, 8, 9) == 5);
    // a long jump forward or a jump back resets
    p = PlanSubmit(5, 100, 101, 100, false, false); CHECK(p.discontinuity && p.from == 100 && p.to == 101);
    CHECK(RequestFrom(5, 100, 101) == 100);
    p = PlanSubmit(50, 10, 11, 10, false, true); CHECK(p.discontinuity && p.from == 10);   // a jump back to dropped outputs
    p = PlanSubmit(50, 10, 11, 10, false, false); CHECK(!p.discontinuity && p.to < p.from);  // skipped, not dropped: the fallback
    // already submitted through hi, frame missing (a skipped generated one): no submit, the fallback handles it
    p = PlanSubmit(6, 4, 5, 4, false, false); CHECK(!p.discontinuity && p.to < p.from);
    // first frame of a fresh pipeline
    p = PlanSubmit(-1000000, 0, 0, 0, false, false); CHECK(p.discontinuity && p.from == 0 && p.to == 0);
}

static int RunOrder(int k, int64_t srcFrames, const std::vector<int64_t>& order, int skipEvery, FakeCore& core, Pipeline<int>& pipe, int& repeats)
{
    core.k = k; core.skipEvery = skipEvery;
    pipe.Init(k, srcFrames, SrcDur100ns(24000, 1001));
    core.dur = SrcDur100ns(24000, 1001);
    pipe.Reset();
    int ok = 0; repeats = 0;
    for (int64_t n : order) {
        int64_t from, to; pipe.Requests(n, from, to);
        auto r = pipe.Produce(n, from, core, 2000);
        if (r.ok) ++ok;
        if (r.repeated) ++repeats;
    }
    return ok;
}

static void TestPipeline()
{
    // sequential, k = 1..8: every output, no repeat, each source submitted once, one reset at the start
    for (int k = 1; k <= 8; ++k) {
        FakeCore core; Pipeline<int> pipe; int rep = 0;
        std::vector<int64_t> order; for (int64_t n = 0; n < 50 * k; ++n) order.push_back(n);
        CHECK(RunOrder(k, 50, order, 0, core, pipe, rep) == 50 * k);
        CHECK(rep == k - 1);                     // only the clip's last slots repeat the last frame
        CHECK(core.submits == 50);
        CHECK(pipe.Count().discontinuities == 1);
        CHECK(pipe.Count().offGrid == 0);
        CHECK(pipe.Cached() <= size_t(3 * k + 1));
        pipe.ClearWith([&](int) { --core.live; });
        CHECK(core.live == 0);
    }
    // slightly out of order (VapourSynth's parallel requests): still each source once, no reset after the first
    {
        FakeCore core; Pipeline<int> pipe; int rep = 0; const int k = 4;
        std::vector<int64_t> order; for (int64_t n = 0; n < 400; ++n) order.push_back(n);
        std::mt19937 rng(7);
        for (size_t i = 0; i + 3 < order.size(); i += 3) std::shuffle(order.begin() + ptrdiff_t(i), order.begin() + ptrdiff_t(i + 3), rng);
        CHECK(RunOrder(k, 100, order, 0, core, pipe, rep) == 400);
        CHECK(core.submits == 100);
        CHECK(pipe.Count().discontinuities == 1);
        CHECK(rep == 3);
    }
    // a seek (jump forward and back): resets, never a stall
    {
        FakeCore core; Pipeline<int> pipe; int rep = 0; const int k = 5;
        std::vector<int64_t> order;
        for (int64_t n = 0; n < 50; ++n) order.push_back(n);
        for (int64_t n = 500; n < 550; ++n) order.push_back(n);
        for (int64_t n = 100; n < 150; ++n) order.push_back(n);
        CHECK(RunOrder(k, 200, order, 0, core, pipe, rep) == 150);
        CHECK(pipe.Count().discontinuities == 3);
        CHECK(rep == 0);
    }
    // pushcore skips generated frames: the nearest real frame stands in
    {
        FakeCore core; Pipeline<int> pipe; int rep = 0; const int k = 4;
        std::vector<int64_t> order; for (int64_t n = 0; n < 200; ++n) order.push_back(n);
        CHECK(RunOrder(k, 50, order, 5, core, pipe, rep) == 200);
        // 49 pairs, every 5th skipped (9 pairs x 3 frames) + the clip's last 3 slots
        CHECK(rep == 9 * 3 + 3);
        CHECK(core.submits == 50);
    }
    // the core is busy for a while: the pipeline drains and retries
    {
        FakeCore core; core.busyLeft = 3; Pipeline<int> pipe; int rep = 0;
        std::vector<int64_t> order; for (int64_t n = 0; n < 20; ++n) order.push_back(n);
        CHECK(RunOrder(2, 10, order, 0, core, pipe, rep) == 20);
    }
}

static void TestReuse()
{
    CoreKey a; a.width = 1920; a.height = 1080; a.fpsNum = 24000; a.fpsDen = 1001; a.settings = { 1, 2, 3 };
    CoreKey b = a;
    CHECK(DecideReuse(false, a, b) == Reuse::Open);
    CHECK(DecideReuse(true, a, b) == Reuse::Flush);            // mpv's reload after a seek: same stream, same settings
    b.settings = { 1, 2, 4 };
    CHECK(DecideReuse(true, a, b) == Reuse::Reload);           // settings changed between reloads
    b = a; b.transfer = 16;
    CHECK(DecideReuse(true, a, b) == Reuse::Reopen);           // another file
    b = a; b.displayHdr = 1;
    CHECK(DecideReuse(true, a, b) == Reuse::Reopen);
}

static void TestChroma()
{
    // 4K HDR 4:4:4 (49.8 MB) at 240 = 11.9 GB/s -> 4:2:0; the measured budget is 1.6 GB/s
    ChromaDecision d = DecideChroma(Chroma::Auto, 3840, 2160, true, 240);
    CHECK(d.use420 && d.gbs444 > 11.8 && d.gbs444 < 12.0 && d.gbs420 < 6.0);
    d = DecideChroma(Chroma::Auto, 3840, 2160, false, 47.952); CHECK(!d.use420);   // 4K RGB24 x2: 1.19 GB/s, played clean
    d = DecideChroma(Chroma::Auto, 3840, 2160, false, 95.904); CHECK(d.use420);    // x4: 2.39 GB/s dropped frames as RGB
    d = DecideChroma(Chroma::Auto, 3840, 2160, true, 120); CHECK(d.use420);
    d = DecideChroma(Chroma::Auto, 1920, 1080, false, 239.76); CHECK(!d.use420);   // 1080p x10: 1.49 GB/s
    d = DecideChroma(Chroma::Auto, 1920, 1080, true, 480); CHECK(d.use420);
    // Auto's multiplier at the budget: 4K 8-bit 23.976 fps 4:2:0 = 0.30 GB/s per x -> x5; 10-bit -> x2; 1080p: no cap
    CHECK(ReadbackMaxK(3840, 2160, false, 23.976) == 5);
    CHECK(ReadbackMaxK(3840, 2160, true, 23.976) == 2);
    CHECK(ReadbackMaxK(1920, 1080, false, 23.976) >= 16);
    CHECK(ReadbackMaxK(7680, 4320, true, 60) == 1);
    d = DecideChroma(Chroma::C444, 3840, 2160, true, 240); CHECK(!d.use420);
    d = DecideChroma(Chroma::C420, 1920, 1080, false, 48); CHECK(d.use420);
}

static void TestColour()
{
    Colour c = GuessColour(3840, 2160, 10); CHECK(c.transfer == 16 && c.primaries == 9 && c.matrix == 9);
    c = GuessColour(1920, 1080, 10); CHECK(c.transfer == 1 && c.matrix == 1);
    c = GuessColour(720, 480, 8); CHECK(c.matrix == 6);
    Colour f = GuessColour(1920, 1080, 8);
    c = ColourFromProps(f, 9, true, 16, true, 9, true, 1, true); CHECK(c.matrix == 9 && c.transfer == 16 && c.primaries == 9 && c.range == 1);
    c = ColourFromProps(f, 2, true, 2, true, 2, true, 0, true); CHECK(c.matrix == 1 && c.transfer == 1 && c.range == 2);  // unspecified keeps; VS full -> FFmpeg 2
    c = ColourFromProps(f, 0, false, 0, false, 0, false, 0, false); CHECK(c == f);
    CHECK(FfRangeFromVs(1, true) == 1 && FfRangeFromVs(0, true) == 2 && FfRangeFromVs(0, false) == 1);
}

// mpv's struct mp_image prefix as at the tags v0.39.0 .. v0.42.0 (video/mp_image.h) with libplacebo's colour structs
// (v6.338.2 .. master, src/include/libplacebo/colorspace.h): what vf_vapoursynth copies into _MP_IMAGE
namespace mpimg {
struct CieXy { float x, y; };
struct RawPrim { CieXy r, g, b, w; };
struct Bezier { float target, kx, ky; float anchors[15]; uint8_t num; };
struct Hdr { RawPrim prim; float minL, maxL, cll, fall; float sceneMax[3]; float sceneAvg; Bezier ootf; float maxPq, avgPq; };
struct ColorSpace { int primaries, transfer; Hdr hdr; };
struct BitEnc { int sample, color, shift; };
struct ColorRepr { int sys, levels, alpha; BitEnc bits; const void* dovi; };
struct Params { int imgfmt; const char* name; int hwSubfmt; int w, h, pw, ph; bool forceWindow; ColorSpace color; ColorRepr repr; int primOrig, trcOrig, sysOrig; int light, chroma; bool vflip; int rotate, stereo; int crop[4]; bool noDovi, noEl; };
struct Image { int w, h; Params params; int num_planes; void* planes[4]; int stride[4]; void* bufs[8]; double pts; };
}

static void TestMpImage()
{
    static_assert(sizeof(mpimg::Hdr) == 148, "pl_hdr_metadata is 148 bytes");
    // a 1080p PQ BT.2020 frame (libplacebo: BT_2020 6, PQ 12, BT_2020_NC 4, LIMITED 1); _ColorSpace 9, _ColorRange 1
    mpimg::Image img{};
    img.w = 1920; img.h = 1080;
    img.params.imgfmt = 1000; img.params.w = 1920; img.params.h = 1080; img.params.pw = 1; img.params.ph = 1;
    img.params.color.primaries = 6; img.params.color.transfer = 12; img.params.repr.sys = 4; img.params.repr.levels = 1;
    img.params.color.hdr.maxL = 1000; img.params.color.hdr.minL = 0.005f;
    auto blob = [](const mpimg::Image& i) { std::vector<uint8_t> b(sizeof i); std::memcpy(b.data(), &i, sizeof i); return b; };
    std::vector<uint8_t> b = blob(img);
    MpFrameColour r = ParseMpImage(b.data(), b.size(), 1920, 1080, 1, 1, 9, 1);
    CHECK(r.ok && r.transfer == 16 && r.primaries == 9 && r.matrix == 9 && r.range == 1);
    CHECK(r.anchor == offsetof(mpimg::Image, params) + offsetof(mpimg::Params, w));
    CHECK(offsetof(mpimg::Image, params) + offsetof(mpimg::Params, repr) == (r.anchor + 176 + sizeof(void*) - 1) / sizeof(void*) * sizeof(void*));
    // HLG (13 -> 18), full range (FULL 2, _ColorRange 0 -> range argument 0)
    img.params.color.transfer = 13; img.params.repr.levels = 2; b = blob(img);
    r = ParseMpImage(b.data(), b.size(), 1920, 1080, 1, 1, 9, 0); CHECK(r.ok && r.transfer == 18 && r.range == 0);
    // UHD 10-bit SDR: BT_1886 1 -> 1, BT_709 3 -> 1, BT_709 system 2 -> _ColorSpace 1
    img.w = img.params.w = 3840; img.h = img.params.h = 2160; img.params.color.transfer = 1; img.params.color.primaries = 3; img.params.repr.sys = 2; img.params.repr.levels = 1; b = blob(img);
    r = ParseMpImage(b.data(), b.size(), 3840, 2160, 1, 1, 1, 1); CHECK(r.ok && r.transfer == 1 && r.primaries == 1 && r.matrix == 1);
    // an SDR curve H.273 has no code for (GAMMA24 7): SDR (1), never the format's PQ guess
    img.params.color.transfer = 7; b = blob(img);
    r = ParseMpImage(b.data(), b.size(), 3840, 2160, 1, 1, 1, 1); CHECK(r.ok && r.transfer == 1);
    img.params.color.transfer = 1; b = blob(img);
    // refused: _ColorSpace disagrees, _ColorRange disagrees, levels set without _ColorRange, no _ColorSpace
    CHECK(!ParseMpImage(b.data(), b.size(), 3840, 2160, 1, 1, 9, 1).ok);
    CHECK(!ParseMpImage(b.data(), b.size(), 3840, 2160, 1, 1, 1, 0).ok);
    CHECK(!ParseMpImage(b.data(), b.size(), 3840, 2160, 1, 1, 1, -1).ok);
    CHECK(!ParseMpImage(b.data(), b.size(), 3840, 2160, 1, 1, -1, 1).ok);
    // refused: another size or aspect (no anchor), a truncated blob, none
    CHECK(!ParseMpImage(b.data(), b.size(), 1920, 1080, 1, 1, 1, 1).ok);
    CHECK(!ParseMpImage(b.data(), b.size(), 3840, 2160, 4, 3, 1, 1).ok);
    CHECK(!ParseMpImage(b.data(), r.anchor + 100, 3840, 2160, 1, 1, 1, 1).ok);
    CHECK(!ParseMpImage(nullptr, 0, 3840, 2160, 1, 1, 1, 1).ok);
    // refused: the anchor twice (crop = [w, h, 1, 1] by chance)
    img.params.crop[0] = 3840; img.params.crop[1] = 2160; img.params.crop[2] = 1; img.params.crop[3] = 1; b = blob(img);
    r = ParseMpImage(b.data(), b.size(), 3840, 2160, 1, 1, 1, 1); CHECK(!r.ok && r.why.find("not unique") != std::string::npos);
    img.params.crop[0] = img.params.crop[1] = img.params.crop[2] = img.params.crop[3] = 0;
    // refused: a layout shifted by 4 bytes (another struct before color): repr no longer agrees
    {
        std::vector<uint8_t> s(sizeof img + 8, 0);
        const size_t a = offsetof(mpimg::Image, params) + offsetof(mpimg::Params, w);
        std::memcpy(s.data(), &img, a + 20);   // up to color
        std::memcpy(s.data() + a + 24, reinterpret_cast<const uint8_t*>(&img) + a + 20, sizeof img - a - 20);
        CHECK(!ParseMpImage(s.data(), s.size(), 3840, 2160, 1, 1, 1, 1).ok);
    }
    // a 32-bit process: pointers of 4 bytes, repr right after hdr (anchor + 176)
    {
        std::vector<uint8_t> s(400, 0);
        auto put = [&](size_t o, int32_t v) { std::memcpy(s.data() + o, &v, 4); };
        put(0, 1280); put(4, 720);                       // mp_image w, h
        put(8, 1000); put(12, 0); put(16, 0);            // imgfmt, imgfmt_name, hw_subfmt
        const size_t a = 20;
        put(a, 1280); put(a + 4, 720); put(a + 8, 1); put(a + 12, 1);
        put(a + 20, 6); put(a + 24, 12);                 // BT_2020, PQ
        put(a + 176, 4); put(a + 180, 1);                // BT_2020_NC, LIMITED
        r = ParseMpImage(s.data(), s.size(), 1280, 720, 1, 1, 9, 1, 4); CHECK(r.ok && r.transfer == 16 && r.anchor == a);
        CHECK(!ParseMpImage(s.data(), s.size(), 1280, 720, 1, 1, 9, 1, 8).ok);   // read as a 64-bit layout: refused
    }
    // libplacebo's system numbering before and after v7.360 (RGB 10 / 12)
    CHECK(MpSystemMatchesAv(10, 0) && MpSystemMatchesAv(12, 0) && !MpSystemMatchesAv(4, 1) && MpSystemMatchesAv(6, 14));
    int ma = 0, mi = 0;
    CHECK(MpVersion("0.42.0", ma, mi) && ma == 0 && mi == 42);
    CHECK(MpVersion("mpv v0.39.0-dev-g1234", ma, mi) && mi == 39);
    CHECK(!MpVersion("", ma, mi) && !MpVersion("mpv git", ma, mi));

    // the output's colour written back (PatchMpImage): a 1080p HLG source tone-mapped to SDR, as mpv would read it
    {
        mpimg::Image s{};
        s.w = s.params.w = 1920; s.h = s.params.h = 1080; s.params.pw = s.params.ph = 1; s.params.imgfmt = 1000;
        s.params.color.primaries = 6; s.params.color.transfer = 13; s.params.color.hdr.maxL = 1000; s.params.color.hdr.maxPq = 0.75f;
        s.params.repr.sys = 4; s.params.repr.levels = 1; s.params.repr.dovi = &s;
        s.params.primOrig = 6; s.params.trcOrig = 13; s.params.sysOrig = 4; s.params.light = 2; s.params.chroma = 1; s.params.rotate = 90; s.params.crop[2] = 1920;
        s.num_planes = 3; s.pts = 1.5;
        std::vector<uint8_t> b = blob(s);
        MpOutColour o; o.plPrimaries = 3; o.plTransfer = 2; o.plSystem = 2; o.plLevels = 1;
        std::string why;
        CHECK(PatchMpImage(b, 1, 1, 9, 1, o, &why));
        mpimg::Image t{}; std::memcpy(&t, b.data(), sizeof t);
        CHECK(t.params.color.primaries == 3 && t.params.color.transfer == 2 && t.params.repr.sys == 2 && t.params.repr.levels == 1);
        CHECK(t.params.primOrig == 3 && t.params.trcOrig == 2 && t.params.sysOrig == 2 && t.params.light == 0 && t.params.repr.dovi == nullptr);
        mpimg::Hdr zero{}; CHECK(std::memcmp(&t.params.color.hdr, &zero, sizeof zero) == 0);
        CHECK(t.w == 1920 && t.params.chroma == 1 && t.params.rotate == 90 && t.params.crop[2] == 1920 && t.num_planes == 3 && t.pts == 1.5 && t.params.repr.alpha == s.params.repr.alpha);
        // read back as mpv's next filter would see it (_ColorSpace 1 for BT.709 now): SDR sRGB BT.709
        r = ParseMpImage(b.data(), b.size(), 1920, 1080, 1, 1, 1, 1); CHECK(r.ok && r.transfer == 13 && r.primaries == 1 && r.matrix == 1);
        // SDR to HDR10 with a peak (RTX Video HDR on Linux): PQ BT.2020, max_luma set
        b = blob(t); MpOutColour h; h.plPrimaries = 6; h.plTransfer = 12; h.plSystem = 4; h.plLevels = 1; h.maxLuma = 1000;
        CHECK(PatchMpImage(b, 1, 1, 1, 1, h, &why));
        std::memcpy(&t, b.data(), sizeof t); CHECK(t.params.color.transfer == 12 && t.params.color.hdr.maxL == 1000 && t.params.color.hdr.minL == 0 && t.params.trcOrig == 12);
        // refused: the source's props disagree (validated before the backend writes its own), an out-of-range light,
        // no libplacebo value for the output
        b = blob(s); CHECK(!PatchMpImage(b, 1, 1, 1, 1, o, &why));
        s.params.light = 9; b = blob(s); CHECK(!PatchMpImage(b, 1, 1, 9, 1, o, &why) && why.find("light") != std::string::npos); s.params.light = 2;
        MpOutColour bad = o; bad.plTransfer = -1; b = blob(s); CHECK(!PatchMpImage(b, 1, 1, 9, 1, bad, &why));
        // keep the transfer (SDR in, SDR out): only the matrix and the levels, sys_orig with them; a BT.601 full-range
        // source made BT.709 limited; the HDR metadata, light, dovi and *_orig transfer untouched
        {
            mpimg::Image f = s; f.params.color.primaries = 3; f.params.color.transfer = 1; f.params.repr.sys = 1; f.params.repr.levels = 2;
            f.params.primOrig = 3; f.params.trcOrig = 1; f.params.sysOrig = 1; f.params.light = 1;
            b = blob(f); MpOutColour k; k.plSystem = 2; k.plLevels = 1;
            CHECK(PatchMpImage(b, 1, 1, 6, 0, k, &why));
            mpimg::Image g{}; std::memcpy(&g, b.data(), sizeof g);
            CHECK(g.params.repr.sys == 2 && g.params.repr.levels == 1 && g.params.sysOrig == 2 && g.params.color.transfer == 1 && g.params.color.primaries == 3);
            CHECK(g.params.light == 1 && g.params.repr.dovi == f.params.repr.dovi && g.params.color.hdr.maxL == 1000 && g.params.trcOrig == 1);
            // Dolby Vision (system 8): kept as it is in this mode
            f.params.repr.sys = 8; f.params.repr.levels = 1; b = blob(f); const std::vector<uint8_t> before = b;
            CHECK(PatchMpImage(b, 1, 1, 2, 1, k, &why) && b == before);
            // one of transfer / primaries alone: refused
            MpOutColour half = k; half.plTransfer = 2; b = blob(s); CHECK(!PatchMpImage(b, 1, 1, 9, 1, half, &why));
        }        CHECK(H273TransferToPl(16) == 12 && H273TransferToPl(13) == 2 && H273TransferToPl(1) == 1 && H273PrimariesToPl(9) == 6 && H273MatrixToPlSystem(9) == 4 && PlSystemToAv(2) == 1);
        CHECK(VpyRgbMatrix(3840, 2160, 10) == 9 && VpyRgbMatrix(3840, 2160, 8) == 1 && VpyRgbMatrix(720, 480, 8) == 6);
        // the replica's offsets equal the ones PatchMpImage writes (64-bit here)
        const size_t a = offsetof(mpimg::Image, params) + offsetof(mpimg::Params, w), ss = (a + 176 + 7) / 8 * 8;
        CHECK(offsetof(mpimg::Image, params) + offsetof(mpimg::Params, primOrig) == ss + 32);
        CHECK(offsetof(mpimg::Image, params) + offsetof(mpimg::Params, light) == ss + 32 + 12);
        CHECK(offsetof(mpimg::Image, params) + offsetof(mpimg::Params, repr) + offsetof(mpimg::ColorRepr, dovi) == ss + 24);
    }
}

static void TestAuto()
{
    pm::Settings s;   // all Auto
    pm::Source src; src.w = 1920; src.h = 1080; src.fps = 24000.0 / 1001;
    pm::Costs costs; pm::Gpu g;   // RTX 50, everything available
    // the user's 240 Hz display, no window known (embedded mpv): fixed refresh
    pm::Display q; q.refreshHz = 60; q.width = 3840; q.height = 2160; q.vrr = true; q.vrrKnown = true;
    pm::Display d = ResolveDisplay(q, false, pm::VrrMode::Auto, 239.96, -1);
    CHECK(d.refreshHz > 239.9 && !d.vrr && !d.vrrKnown);
    pm::Choice c = pm::Decide(s, src, d, costs, g);
    CHECK(c.multiplier >= 2 && src.fps * c.multiplier <= d.refreshHz * pm::kRefreshSlack);
    std::printf("  Auto 1080p23.976 on 240 Hz fixed: x%d %ls, SR %d level %d -> %ux%u (%.1f of %.1f ms)\n", c.multiplier, pm::FgMethodLabel(c.fgMethod), c.sr, c.srLevel, c.outW, c.outH, c.predictedMs, c.budgetMs);
    // a 60 Hz display: 2x at most
    d = ResolveDisplay(q, false, pm::VrrMode::Auto, 60.0, -1);
    c = pm::Decide(s, src, d, costs, g);
    CHECK(c.multiplier == 2);
    // the user's VRR override is kept without a window
    d = ResolveDisplay(q, false, pm::VrrMode::On, 240, -1);
    CHECK(d.vrr);
    // with mpv's window known the probed state stays
    d = ResolveDisplay(q, true, pm::VrrMode::Auto, 240, -1);
    CHECK(d.vrr);
    // mpv started with --fs: its window is not full screen yet when the filter starts; G-SYNC fullscreen-only covers it
    CHECK(MpvStartsFullscreen(L"mpv.exe --fs clip.mkv") && MpvStartsFullscreen(L"mpv --fullscreen=yes x") && !MpvStartsFullscreen(L"mpv --fs=no x")
          && !MpvStartsFullscreen(L"mpv --fs-screen=1 x") && !MpvStartsFullscreen(L"mpv clip.mkv"));
    {
        pm::Display w = q; w.vrr = false; w.vrrCapable = true;
        w.vrrHow = L"G-SYNC is fullscreen-only and the player is windowed (NVIDIA profile: application mode 1, global mode 1)";
        pm::Display f = ResolveDisplay(w, true, pm::VrrMode::Auto, 240, -1, 1);
        CHECK(f.vrr && f.vrrHow.find(L"mpv started full screen") != std::wstring::npos);
        f = ResolveDisplay(w, true, pm::VrrMode::Auto, 240, -1, 0);
        CHECK(!f.vrr && f.vrrHow.find(L"not full screen when PushMPC started") != std::wstring::npos);
    }
    {   // full screen for the renderer's share per shown frame: display_fs / --fs with a window; none known = a window
        pm::Display w = q; w.fullscreen = false;
        CHECK(ResolveDisplay(w, true, pm::VrrMode::Auto, 240, -1, 1).fullscreen && !ResolveDisplay(w, true, pm::VrrMode::Auto, 240, -1, 0).fullscreen);
        CHECK(!ResolveDisplay(w, false, pm::VrrMode::Auto, 240, -1).fullscreen && ResolveDisplay(w, false, pm::VrrMode::Auto, 240, -1, 1).fullscreen);
        // who tone-maps is the route's: pushcore (mpv routes) or the player (VLC); the player's map costs per shown frame
        CHECK(WithToneMapBy(s, true).hdrToSdr == pm::HdrToSdr::PushMpc && WithToneMapBy(s, false).hdrToSdr == pm::HdrToSdr::Player);
        pm::Source hdrSrc; hdrSrc.w = 3840; hdrSrc.h = 2160; hdrSrc.fps = 24000.0 / 1001; hdrSrc.hdr = true; hdrSrc.tenBit = true;
        pm::Display sdr = ResolveDisplay(w, true, pm::VrrMode::Auto, 239.96, 0, 0);
        const pm::Choice core = pm::Decide(WithToneMapBy(s, true), hdrSrc, sdr, costs, g), player = pm::Decide(WithToneMapBy(s, false), hdrSrc, sdr, costs, g);
        CHECK(core.coreToneMap && !player.coreToneMap && player.multiplier <= core.multiplier);
        std::printf("  4K HDR on an SDR 240 Hz window: pushcore tone map x%d, the player's x%d\n", core.multiplier, player.multiplier);
    }
    // display_hdr forces the HDR flag
    d = ResolveDisplay(q, false, pm::VrrMode::Auto, 0, 1); CHECK(d.hdr && d.refreshHz == 60);
    // the user's Smoother setting caps it
    pm::Settings s3 = s; CHECK(ApplyOverride(s3, L"smoother", L"3"));
    d = ResolveDisplay(q, false, pm::VrrMode::Auto, 240, -1);
    c = pm::Decide(s3, src, d, costs, g); CHECK(c.multiplier == 3);
    // ToCore carries the choice
    const PushCoreSettings ps = ToCore(s3, c, d, src.w, src.h, false, false, nullptr);
    CHECK(ps.structSize == sizeof(PushCoreSettings) && ps.multiplier == 3 && ps.fgBackend == PUSHCORE_FG_VFG && ps.cutGuard >= 1);
    // overrides use the registry words
    pm::Settings o;
    CHECK(ApplyOverride(o, L"smoother", L"off") && o.smoother == pm::kOff);
    CHECK(ApplyOverride(o, L"smoother", L"1") && o.smoother == pm::kOff);
    CHECK(ApplyOverride(o, L"sharper", L"4") && o.sharper == 4);
    CHECK(ApplyOverride(o, L"fg_method", L"PushFlow") && o.fgMethod == pm::FgMethod::PushFlow);
    CHECK(ApplyOverride(o, L"fg_quality", L"high") && o.fgQuality == pm::FgQuality::High);
    CHECK(ApplyOverride(o, L"vrr", L"Off") && o.vrr == pm::VrrMode::Off);
    CHECK(ApplyOverride(o, L"target", L"1440p") && o.target == pm::UpscaleTarget::P1440);
    CHECK(ApplyOverride(o, L"enable", L"0") && !o.enable);
    CHECK(!ApplyOverride(o, L"nonsense", L"1"));
    // disabled: pass-through settings
    const PushCoreSettings off = ToCore(o, c, d, src.w, src.h, false, false, nullptr);
    CHECK(off.multiplier == 1 && off.sr == 0);
}

int main()
{
    TestRates(); TestPlan(); TestPipeline(); TestReuse(); TestChroma(); TestColour(); TestMpImage(); TestAuto();
    std::printf("sched_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
