// The content helper's model-free parts (content_helper.h): the picture protocol against a full-picture reference and
// against ContentDetect's ClipInput, the evidence integrator (ContentDetector's tracker constants), the cut meter and
// the status file. CPU only, no model.
#include <cstdio>
#include <cstdlib>
#include <random>
#include "content_helper.h"

using namespace pushmpc::content;
using namespace pushmpc::content::mpvhelper;

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// a bgr0 picture with a smooth pattern and some detail (stride with padding, as mpv's images can have)
static std::vector<uint8_t> Pattern(int w, int h, ptrdiff_t stride, unsigned seed)
{
    std::vector<uint8_t> img(size_t(stride) * h, 0);
    std::mt19937 rng(seed);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = img.data() + ptrdiff_t(y) * stride + ptrdiff_t(x) * 4;
            p[0] = uint8_t((x * 7 + y * 3) & 255); p[1] = uint8_t((x ^ y) & 255); p[2] = uint8_t(rng() & 255); p[3] = 0;
        }
    return img;
}
// ClipInput's sample positions on the full picture (RGB, bilinear, clamped)
static float Ref(const std::vector<uint8_t>& img, ptrdiff_t stride, int w, int h, int i, int j, int c)
{
    const int side = std::min(w, h), x0 = (w - side) / 2, y0 = (h - side) / 2;
    const double fx = x0 + (i + 0.5) * side / 224.0 - 0.5, fy = y0 + (j + 0.5) * side / 224.0 - 0.5;
    const int ix = int(std::floor(fx)), iy = int(std::floor(fy));
    const double ax = fx - ix, ay = fy - iy;
    auto at = [&](int x, int y) { x = std::clamp(x, 0, w - 1); y = std::clamp(y, 0, h - 1); return double(img[size_t(y) * stride + size_t(x) * 4 + (2 - c)]); };
    return float((at(ix, iy) * (1 - ax) + at(ix + 1, iy) * ax) * (1 - ay) + (at(ix, iy + 1) * (1 - ax) + at(ix + 1, iy + 1) * ax) * ay);
}

static void TestProtocol()
{
    const int sizes[][2] = { { 640, 360 }, { 1920, 1080 }, { 3840, 2160 }, { 360, 640 }, { 100, 50 }, { 1281, 721 }, { 224, 224 } };
    for (auto& s : sizes) {
        const int w = s[0], h = s[1];
        const ptrdiff_t stride = ptrdiff_t(w) * 4 + 64;
        const auto img = Pattern(w, h, stride, unsigned(w * 7 + h));
        const std::string bytes = MakePictureBytes(img.data(), stride, w, h, 12.5, 1, 42);
        Picture p; std::string err;
        CHECK(ParsePicture(bytes, p, &err));
        if (!err.empty()) std::printf("  %dx%d: %s\n", w, h, err.c_str());
        CHECK(p.w == w && p.h == h && p.cut == 1 && p.seq == 42 && std::fabs(p.pts - 12.5) < 1e-9);
        // the payload the lua sends: two rows per output row, the centre columns plus one on each side
        CHECK(p.rows.size() == size_t(448) * p.ncol * 4 && p.ncol <= std::min(w, h) + 2);
        const auto rgb = Thumbnail(p);
        double worst = 0;
        for (int j = 0; j < 224; ++j)
            for (int i = 0; i < 224; ++i)
                for (int c = 0; c < 3; ++c) worst = std::max(worst, std::fabs(double(rgb[(size_t(j) * 224 + i) * 3 + c]) - Ref(img, stride, w, h, i, j, c)));
        CHECK(worst < 1e-3);
        if (worst >= 1e-3) std::printf("  %dx%d: largest difference to the full-picture reference %.5f\n", w, h, worst);
    }
    // ClipInput itself (ContentDetect's routes): a grey picture (R = G = B = Y, neutral chroma, full range) gives the
    // same model input through both
    {
        const int w = 1280, h = 720;
        std::vector<uint8_t> y(size_t(w) * h), u(size_t(w / 2) * (h / 2), 128), v(u.size(), 128), bgr0(size_t(w) * h * 4, 0);
        for (int yy = 0; yy < h; ++yy)
            for (int x = 0; x < w; ++x) {
                const uint8_t g = uint8_t((x * 3 + yy * 5 + ((x / 16 + yy / 16) & 1) * 60) & 255);
                y[size_t(yy) * w + x] = g;
                uint8_t* p = bgr0.data() + (size_t(yy) * w + x) * 4; p[0] = p[1] = p[2] = g;
            }
        const std::vector<float> a = ClipInput<uint8_t>(y.data(), w, u.data(), v.data(), w / 2, w, h, 8, false);
        Picture p; std::string err;
        CHECK(ParsePicture(MakePictureBytes(bgr0.data(), ptrdiff_t(w) * 4, w, h, 0, 0, 0), p, &err));
        const std::vector<float> b = ModelInput(Thumbnail(p));
        CHECK(a.size() == b.size());
        double worst = 0;
        for (size_t k = 0; k < std::min(a.size(), b.size()); ++k) worst = std::max(worst, double(std::fabs(a[k] - b[k])));
        CHECK(worst < 2e-3);
        std::printf("  ClipInput vs the helper's input (grey 1280x720): largest difference %.6f\n", worst);
    }
    // broken pictures are refused
    {
        const int w = 640, h = 360;
        const auto img = Pattern(w, h, w * 4, 1);
        const std::string good = MakePictureBytes(img.data(), w * 4, w, h, 1, 0, 1);
        Picture p; std::string err;
        CHECK(!ParsePicture(good.substr(0, good.size() - 1), p, &err));
        CHECK(!ParsePicture("PMCD2" + good.substr(5), p, &err));
        CHECK(!ParsePicture("garbage", p, &err));
        std::string wrongX = good; wrongX.replace(wrongX.find(" 140 "), 5, " 141 ");   // x0 of 640x360 is 140
        CHECK(!ParsePicture(wrongX, p, &err));
        std::string badCut = good; const size_t nl = badCut.find('\n'); badCut.replace(nl - 4, 3, " 7 ");
        CHECK(!ParsePicture(badCut, p, &err));
    }
}

static void TestTracker()
{
    using D = ContentDetector;
    // drawn evidence 0.97 a second: per step logit(sigmoid(0.85 * logit(0.97))) = 2.955; above kImgOn (12) after 5
    // steps; keyframes at 0, 1, 2 s give steps 1, 3, 5
    {
        ImageTracker t;
        t.Evidence(0.97, 0.0, -1); CHECK(t.steps() == 1 && t.Current().cls == kUnknown);
        t.Evidence(0.97, 1.0, -1); CHECK(t.steps() == 3 && t.Current().cls == kUnknown);
        t.Evidence(0.97, 2.0, -1); CHECK(t.steps() == 5 && t.Current().cls == kDrawn);
        CHECK(!t.Current().cg && t.Current().cgShare < 0);
        CHECK(t.Policy().animeUpscaler && !t.Policy().animeUpscalerDs);   // hand-drawn until CG is measured; _DS never automatic
        double L = 0; for (int k = 0; k < 5; ++k) L = D::kImgDecay * L + std::log((1.0 / (1.0 + std::exp(-0.85 * std::log(0.97 / 0.03)))) / (1.0 - 1.0 / (1.0 + std::exp(-0.85 * std::log(0.97 / 0.03)))));
        CHECK(std::fabs(t.Current().logOdds - L) < 1e-3);
        // live action from 3 s on: not drawn again, the CG flag stays off
        double ts = 3.0; while (t.Current().cls == kDrawn && ts < 120) { t.Evidence(0.02, ts, 0.9); ts += 1.0; }
        CHECK(t.Current().cls == kOther && t.Current().changes == 1 && !t.Current().cg);
        CHECK(!t.Policy().animeUpscaler && !t.Policy().animeUpscalerDs);
        std::printf("  drawn -> not drawn without a cut after %.0f s of live-action evidence\n", ts - 3.0);
    }
    // CG: the CG integrator runs once per keyframe while drawn (kCgDecay 0.9, on above kCgOn 4): logit(0.9) = 2.197, on
    // at the second drawn keyframe. The third keyframe's CG probability is spent on step 4 (still measuring: reset).
    {
        ImageTracker t;
        for (int k = 0; k < 4; ++k) t.Evidence(0.97, k, 0.9);
        CHECK(t.Current().cls == kDrawn && !t.Current().cg);
        t.Evidence(0.97, 4.0, 0.9);
        CHECK(t.Current().cg);
        CHECK(t.Policy().animeUpscaler && !t.Policy().animeUpscalerDs && t.Policy().holdRepair && !t.Policy().signHold && !t.Policy().lineArt && !t.Policy().animeDeband);
        // hand-drawn again: off below -kCgOff (3)
        int k = 5; while (t.Current().cg && k < 60) { t.Evidence(0.97, k, 0.05); ++k; }
        CHECK(!t.Current().cg && t.Current().cls == kDrawn && !t.Policy().animeUpscalerDs);
    }
    // neutral evidence never decides
    {
        ImageTracker t;
        for (int k = 0; k < 30; ++k) t.Evidence(0.5, k, -1);
        CHECK(t.Current().cls == kUnknown);
    }
    // step accounting: a jump (seek) is one step after a cut; going back too; pictures closer than a step add none
    {
        ImageTracker t;
        t.Evidence(0.9, 0.0, -1); CHECK(t.steps() == 1);
        t.Evidence(0.9, 0.3, -1); CHECK(t.steps() == 1);
        t.Evidence(0.9, 0.6, -1); CHECK(t.steps() == 2);
        t.Evidence(0.9, 60.0, -1); CHECK(t.steps() == 3);
        t.Evidence(0.9, 10.0, -1); CHECK(t.steps() == 4);
        t.Evidence(0.9, 12.9, -1); CHECK(t.steps() == 9);   // 2.9 s: 5 steps
    }
    // a cut makes the integrator forget faster (kImgCutDecay)
    {
        ImageTracker a, b;
        for (int k = 0; k < 20; ++k) { a.Evidence(0.97, k, -1); b.Evidence(0.97, k, -1); }
        b.Cut();
        a.Evidence(0.02, 20, -1); b.Evidence(0.02, 20, -1);
        CHECK(b.Current().logOdds < a.Current().logOdds);
    }
}

static void TestCutsAndStatus()
{
    std::vector<float> red(224 * 224 * 3, 0.f), blue = red;
    for (size_t i = 0; i < red.size(); i += 3) { red[i] = 220; blue[i + 2] = 220; }
    CutMeter m;
    CHECK(!m.Push(red)); CHECK(!m.Push(red)); CHECK(m.last() < 1e-6f);
    CHECK(m.Push(blue)); CHECK(m.last() > 1.9f);
    m.Reset(); CHECK(!m.Push(red));

    ImageTracker t;
    for (int k = 0; k < 5; ++k) t.Evidence(0.97, k, 0.1);
    StatusInfo i; i.state = "ready"; i.model = "ort cpu\nline"; i.seq = 7; i.pImage = 0.97; i.pCg = 0.1; i.ms = 12.5;
    const auto s = ParseStatus(StatusText(i, t));
    CHECK(s.at("pushmpc-content") == "1" && s.at("state") == "ready" && s.at("class") == "drawn" && s.at("cg") == "0");
    CHECK(s.at("seq") == "7" && s.at("animeUpscaler") == "1" && s.at("animeUpscalerDs") == "0" && s.at("model") == "ort cpu line");
    ImageTracker none;
    const auto s2 = ParseStatus(StatusText(StatusInfo{}, none));
    CHECK(s2.at("state") == "loading" && s2.at("class") == "measuring" && s2.at("animeUpscaler") == "0");

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "pushmpc-content-helper-test";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto f = dir / "status.st";
    CHECK(WriteAtomic(f, "a=1\n"));
    CHECK(WriteAtomic(f, "a=2\n"));   // over the old one
    std::string back;
    CHECK(ReadFile(f, back) && back == "a=2\n");
    CHECK(!std::filesystem::exists(dir / "status.st.tmp"));
    std::filesystem::remove_all(dir, ec);
}

int main()
{
    TestProtocol();
    TestTracker();
    TestCutsAndStatus();
    std::printf("content_helper_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
