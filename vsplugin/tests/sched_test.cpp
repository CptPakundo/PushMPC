// Unit tests for pushmpc.Enhance's scheduling, rate, reuse, bandwidth and Auto glue (no VapourSynth, no GPU).
#include <cstdio>
#include <deque>
#include <random>
#include <set>
#include "autopick.h"
#include "schedule.h"

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
    // players-all.md section 3: 4K HDR 4:4:4 (49.8 MB) at 240 = 11.9 GB/s -> 4:2:0; at 120 = 5.97 -> 4:4:4
    ChromaDecision d = DecideChroma(Chroma::Auto, 3840, 2160, true, 240);
    CHECK(d.use420 && d.gbs444 > 11.8 && d.gbs444 < 12.0 && d.gbs420 < 6.0);
    d = DecideChroma(Chroma::Auto, 3840, 2160, true, 120); CHECK(!d.use420);
    d = DecideChroma(Chroma::Auto, 3840, 2160, true, 144); CHECK(d.use420);
    d = DecideChroma(Chroma::Auto, 3840, 2160, false, 240); CHECK(!d.use420);   // 8-bit 4K 240 = 5.97 GB/s
    d = DecideChroma(Chroma::Auto, 1920, 1080, true, 480); CHECK(!d.use420);
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
    TestRates(); TestPlan(); TestPipeline(); TestReuse(); TestChroma(); TestColour(); TestAuto();
    std::printf("sched_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
