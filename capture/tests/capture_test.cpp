// SPDX-License-Identifier: GPL-3.0-or-later
// capture_test: capture mode's shared parts on synthetic input, on any OS (no window system, no GPU).
//   - the converter (BT.709 round trip), fingerprints
//   - the timeline on a 24 fps player that a 60 Hz screen shows in a 3:2 cadence (plus jitter): the fitted rate, the delay
//   - the whole host path (timeline -> pacer -> the CPU engine) on a moving square: the square's steps per screen
//     refresh must be even with frame generation (and are uneven without it, the 3:2 judder), never backwards; a pause
//     and a resume start the line again without a morph across the gap
//   - copy-protection detection, the lip-sync part of mpv.conf, the options
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>
#include "cap_core.h"
#include "cap_engines.h"

using namespace pmc;

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

// a white square on a dark grey ground at x (BGRX)
static void Draw(std::vector<uint8_t>& px, int w, int h, double x)
{
    px.assign(size_t(w) * size_t(h) * 4, 0);
    for (size_t i = 0; i < px.size(); i += 4) { px[i] = px[i + 1] = px[i + 2] = 40; px[i + 3] = 255; }
    const int sq = h / 4, y0 = h / 2 - sq / 2;
    for (int y = y0; y < y0 + sq; ++y)
        for (int xx = 0; xx < sq; ++xx) {
            const double fx = x + xx;
            const int ix = int(std::floor(fx));
            const double fr = fx - ix;   // anti-aliased left/right edges so sub-pixel motion is visible
            for (int k = 0; k < 2; ++k) {
                const int X = ix + k;
                if (X < 0 || X >= w) continue;
                const double cov = k == 0 ? 1 - fr : fr;
                uint8_t* p = &px[(size_t(y) * size_t(w) + size_t(X)) * 4];
                const int v = std::min(255, int(p[0] + cov * (255 - 40) + 0.5));
                p[0] = p[1] = p[2] = uint8_t(std::max<int>(p[0], v));
            }
        }
}

// the square's centre from luma (columns above mid grey, weighted)
static double Centroid(const I420& f)
{
    double s = 0, sx = 0;
    const int y = f.h / 2;
    for (int x = 0; x < f.w; ++x) {
        const double v = std::max(0, f.y[size_t(y) * size_t(f.w) + size_t(x)] - 60);
        s += v; sx += v * x;
    }
    return s > 0 ? sx / s : -1;
}

static void TestConvert()
{
    const int w = 64, h = 36;
    std::vector<uint8_t> px(size_t(w) * h * 4), back(px.size());
    std::mt19937 rng(1);
    // smooth colours (chroma is subsampled): a gradient per channel
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) { uint8_t* p = &px[(size_t(y) * w + x) * 4]; p[0] = uint8_t(x * 4); p[1] = uint8_t(y * 7); p[2] = uint8_t(255 - x * 3); p[3] = 255; }
    I420 f;
    ToI420(px.data(), w * 4, w, h, PixFmt::BGRX, f, 3);
    FromI420(f, back.data(), w * 4, PixFmt::BGRX);
    int worst = 0;
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) for (int c = 0; c < 3; ++c) worst = std::max(worst, std::abs(int(px[(size_t(y) * w + x) * 4 + c]) - int(back[(size_t(y) * w + x) * 4 + c])));
    CHECK(worst <= 6, "BT.709 round trip: worst channel error %d", worst);
    // white and black at the range ends
    std::vector<uint8_t> wpx(size_t(w) * h * 4, 255);
    ToI420(wpx.data(), w * 4, w, h, PixFmt::RGBX, f);
    CHECK(f.y[0] == 235 && f.u[0] == 128 && f.v[0] == 128, "white -> Y %d U %d V %d", f.y[0], f.u[0], f.v[0]);
    std::vector<uint8_t> bpx(size_t(w) * h * 4, 0);
    ToI420(bpx.data(), w * 4, w, h, PixFmt::RGBX, f);
    CHECK(f.y[0] == 16, "black -> Y %d", f.y[0]);
    // fingerprints
    std::vector<uint8_t> a, b;
    Draw(a, 640, 360, 100); Draw(b, 640, 360, 104);
    CHECK(Fingerprint(a.data(), 640 * 4, 640, 360) != Fingerprint(b.data(), 640 * 4, 640, 360), "a moved square changes the fingerprint");
    CHECK(Fingerprint(a.data(), 640 * 4, 640, 360) == Fingerprint(a.data(), 640 * 4, 640, 360), "the same frame has the same fingerprint");
    std::printf("convert: worst round-trip error %d\n", worst);
}

// the arrival times of a 24000/1001 fps player on a 60 Hz screen: each frame appears at the first refresh at or after
// its own time (a 3:2 cadence), plus up to `jitter` seconds of capture delay
static std::vector<double> Arrivals(int n, double t0, double jitter, unsigned seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> j(0, jitter);
    const double fps = 24000.0 / 1001, hz = 60.0;
    std::vector<double> t;
    for (int i = 0; i < n; ++i) {
        const double own = t0 + i / fps;
        t.push_back(std::ceil(own * hz - 1e-9) / hz + j(rng));
    }
    return t;
}

static void TestTimeline()
{
    Timeline tl;
    const auto arr = Arrivals(120, 10.0, 0.002, 7);
    bool disc = false;
    uint64_t last = 0;
    for (size_t i = 0; i < arr.size(); ++i) {
        last = tl.Add(arr[i], disc);
        if (i == 0) CHECK(disc, "the first frame starts the line");
        else CHECK(!disc, "frame %zu: no restart", i);
    }
    CHECK(last == 119, "indices follow the frames (last %llu)", (unsigned long long)last);
    CHECK(std::fabs(tl.Interval() - 1001.0 / 24000) < 0.0005, "fitted interval %.5f s (want %.5f)", tl.Interval(), 1001.0 / 24000);
    CHECK(tl.Delay() < tl.Interval() + 0.025, "delay %.1f ms", tl.Delay() * 1000);
    // a missed frame: the index skips it
    Timeline t2;
    for (int i = 0; i < 30; ++i) { if (i == 20) continue; last = t2.Add(arr[size_t(i)], disc); }
    CHECK(last == 29, "a missed frame is skipped in the indices (last %llu)", (unsigned long long)last);
    // a pause: the line starts again
    const uint64_t r = t2.Add(arr[29] + 2.0, disc);
    CHECK(disc && r == 30, "a pause restarts the line (disc %d, idx %llu)", int(disc), (unsigned long long)r);
    std::printf("timeline: %.3f fps, delay %.1f ms, lateness %.1f ms\n", tl.Fps(), tl.Delay() * 1000, tl.Lateness() * 1000);
}

static void TestChoose()
{
    const std::vector<uint64_t> f = { 10, 11, 12 };
    Pick p = Choose(10.5, 0, f, {});
    CHECK(p.valid && p.a == 10 && p.b == 11 && std::fabs(p.t - 0.5f) < 1e-6, "mid pair");
    p = Choose(12.7, 0, f, {});
    CHECK(p.a == 12 && !p.generated(), "the newest frame is held");
    p = Choose(9.0, 0, f, {});
    CHECK(p.a == 10 && !p.generated(), "before the first: the first");
    p = Choose(11.3, 0, f, { 12 });
    CHECK(p.a == 11 && !p.generated(), "no pair across a restart");
    p = Choose(10.6, 4, f, {});
    CHECK(std::fabs(p.t - 0.5f) < 1e-6, "x4 grid: 0.6 -> 0.5 (got %.3f)", p.t);
    p = Choose(10.6, 1, f, {});
    CHECK(!p.generated() && p.a == 10, "off: the real frame of the moment");
    const std::vector<uint64_t> g = { 10, 12 };
    p = Choose(11.0, 0, g, {});
    CHECK(p.a == 10 && p.b == 12 && std::fabs(p.t - 0.5f) < 1e-6, "a gap: t over two indices");
}

struct RunResult { double meanStep = 0, sdStep = 0, minStep = 0; int frames = 0, generated = 0, backwards = 0; };

// the player (a 24 fps square at `speed` px per source frame, 3:2 on 60 Hz), capture, timeline, engine, 60 Hz screen
static RunResult Simulate(int smoother, const std::string& engine, bool withPause)
{
    const int w = 480, h = 180;
    const double speed = 4.0, fps = 24000.0 / 1001, hz = 60.0;
    auto arr = Arrivals(96, 100.0, 0.003, 3);
    std::vector<double> x(arr.size());
    for (size_t i = 0; i < arr.size(); ++i) x[i] = 10 + speed * double(i);
    if (withPause) for (size_t i = 48; i < arr.size(); ++i) arr[i] += 1.0;   // the player pauses for a second after frame 47
    EngineRequest req; req.w = w; req.h = h; req.srcFps = fps; req.displayHz = hz; req.smoother = smoother == 1 ? 1 : 0;
    std::string why;
    auto eng = OpenHostEngine(engine, req, why);
    RunResult rr;
    if (!eng) { CHECK(false, "engine %s: %s", engine.c_str(), why.c_str()); return rr; }
    const int eff = smoother == 0 && eng->Steps() > 0 ? eng->Steps() : smoother;
    HostPipeline pipe(std::move(eng));
    Timeline tl;
    size_t next = 0;
    std::vector<uint8_t> px;
    std::vector<double> pos;
    double lastC = -1;
    const double tEnd = arr.back() + 0.2;
    bool morphAcrossPause = false;
    for (double T = arr.front(); T < tEnd; T += 1.0 / hz) {
        while (next < arr.size() && arr[next] <= T) {
            Draw(px, w, h, x[next]);
            I420 f;
            ToI420(px.data(), w * 4, w, h, PixFmt::BGRX, f, 1);
            bool disc = false;
            const uint64_t idx = tl.Add(arr[next], disc);
            pipe.Add(idx, disc, std::move(f));
            ++next;
        }
        const double p = tl.Position(T);
        const Pick pk = Choose(p, eff, pipe.Frames(), pipe.Discontinuities());
        if (!pk.valid) continue;
        if (withPause && pk.generated() && pk.a < 48 && pk.b >= 48) morphAcrossPause = true;
        std::string err;
        const I420* pic = pipe.Get(pk, err);
        CHECK(pic, "frame: %s", err.c_str());
        if (!pic) break;
        const double c = Centroid(*pic);
        if (lastC >= 0 && c < lastC - 0.5) ++rr.backwards;
        lastC = c;
        if (withPause ? T < arr[47] : T < arr.back() - 0.1) pos.push_back(c);
        ++rr.frames;
        if (pk.generated()) ++rr.generated;
    }
    CHECK(!morphAcrossPause, "no generated frame across a pause");
    // steps between screen refreshes in steady motion (skip the start)
    std::vector<double> st;
    for (size_t i = 12; i + 1 < pos.size(); ++i) st.push_back(pos[i + 1] - pos[i]);
    if (st.empty()) return rr;
    double m = 0; for (double s : st) m += s; m /= double(st.size());
    double v = 0; for (double s : st) v += (s - m) * (s - m); v /= double(st.size());
    rr.meanStep = m; rr.sdStep = std::sqrt(v); rr.minStep = *std::min_element(st.begin(), st.end());
    return rr;
}

static void TestPipeline()
{
    const double want = 4.0 * (24000.0 / 1001) / 60.0;   // px per refresh
    const RunResult on = Simulate(0, "cpu", false);
    const RunResult off = Simulate(1, "cpu", false);
    std::printf("pipeline (cpu, Auto): %d refreshes, %d generated, step %.2f px (want %.2f), sd %.2f, min %.2f, backwards %d\n",
                on.frames, on.generated, on.meanStep, want, on.sdStep, on.minStep, on.backwards);
    std::printf("pipeline (cpu, off):  %d refreshes, %d generated, step %.2f px, sd %.2f, min %.2f (the 3:2 judder)\n",
                off.frames, off.generated, off.meanStep, off.sdStep, off.minStep);
    CHECK(std::fabs(on.meanStep - want) < 0.25, "mean step %.2f px, want %.2f", on.meanStep, want);
    CHECK(on.sdStep < 0.25 * want, "even motion with frame generation: sd %.2f px of %.2f", on.sdStep, want);
    CHECK(on.backwards == 0, "never backwards (%d)", on.backwards);
    CHECK(on.generated > on.frames / 3, "most refreshes generated (%d of %d)", on.generated, on.frames);
    CHECK(off.sdStep > 2 * on.sdStep, "without frame generation the steps are uneven (sd %.2f vs %.2f)", off.sdStep, on.sdStep);
    CHECK(off.generated == 0, "off: no generated frames");
    const RunResult pz = Simulate(0, "cpu", true);
    std::printf("pipeline (cpu, pause): %d refreshes, %d generated, backwards %d\n", pz.frames, pz.generated, pz.backwards);
    CHECK(pz.backwards == 0, "pause: never backwards (%d)", pz.backwards);
}

static void TestProtected()
{
    const int w = 320, h = 180;
    std::vector<uint8_t> black(size_t(w) * h * 4, 0), pic;
    for (size_t i = 3; i < black.size(); i += 4) black[i] = 255;
    Draw(pic, w, h, 50);
    Protected pr;
    bool told = false;
    int hid = 0, toldAt = -1, shown = 0;
    for (int i = 0; i < 24 * 6; ++i) {
        const int r = pr.Feed(black.data(), w * 4, w, h, i / 24.0, told);
        if (r == 1) hid = i;
        if (told) toldAt = i;
    }
    CHECK(hid >= 35 && hid <= 37, "hidden after 1.5 s (frame %d)", hid);
    CHECK(toldAt >= 95 && toldAt <= 97, "told after 4 s (frame %d)", toldAt);
    shown = pr.Feed(pic.data(), w * 4, w, h, 7.0, told);
    CHECK(shown == -1 && !pr.Hidden(), "a picture shows the overlay again");
    // a dark scene is not black: dark grey with a square
    CHECK(!Protected::Black(pic.data(), w * 4, w, h), "a normal picture is not black");
    // letterboxed black with a picture inside (60 %) is not "protected"
    std::vector<uint8_t> lb = pic;
    for (int y = 0; y < h / 5; ++y) std::memset(&lb[size_t(y) * w * 4], 0, size_t(w) * 4);
    CHECK(!Protected::Black(lb.data(), w * 4, w, h), "letterbox bars are not protection");
}

static void TestAudio()
{
    const std::string base = "hwdec=auto\nvo=gpu\n";
    const std::string a = SetAudioDelayText(base, 80);
    CHECK(a.find("audio-delay=+0.080") != std::string::npos && a.find(kDelayBegin) != std::string::npos, "the block is written");
    const std::string b = SetAudioDelayText(a, 95);
    CHECK(b.find("audio-delay=+0.095") != std::string::npos && b.find("+0.080") == std::string::npos, "the block is replaced, not repeated");
    CHECK(ClearAudioDelayText(b) == base, "cleared back to the original");
    CHECK(ClearAudioDelayText(SetAudioDelayText("", 80)).empty(), "an empty file stays empty");
    const std::string crlf = SetAudioDelayText("a=1\r\n", 50);
    CHECK(crlf.find("\r\naudio-delay=+0.050\r\n") != std::string::npos, "CRLF files keep CRLF");
}

static void TestOptions()
{
    const char* argv[] = { "x", "--player", "celluloid", "--smoother", "x4", "--display-hz", "144", "--no-present", "--frames", "10", "--audio-conf", "/tmp/mpv.conf" };
    Options o; std::string why;
    CHECK(ParseOptions(12, const_cast<char**>(argv), o, why), "parse: %s", why.c_str());
    CHECK(o.player == "celluloid" && o.smoother == 4 && o.displayHz == 144 && !o.present && o.frames == 10 && o.audioConf == "/tmp/mpv.conf", "values");
    const char* bad[] = { "x", "--smoother", "12" };
    Options o2;
    CHECK(!ParseOptions(3, const_cast<char**>(bad), o2, why), "x12 is refused");
}

int main()
{
    TestConvert();
    TestTimeline();
    TestChoose();
    TestPipeline();
    TestProtected();
    TestAudio();
    TestOptions();
    if (g_fail) { std::printf("capture_test: %d FAILED\n", g_fail); return 1; }
    std::printf("capture_test: all passed\n");
    return 0;
}
