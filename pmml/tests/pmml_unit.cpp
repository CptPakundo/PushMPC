// SPDX-License-Identifier: GPL-3.0-or-later
// pmml unit tests (no runtime, no GPU): sizes, the grid mapping, parsing, Auto's choice and cache, the CPU graphs.
#include "internal.h"
#include "pmml/autobench.h"
#include "pmml/pmml.h"
#include "pmml/upscale.h"

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)
#define NEAR(a, b, eps) CHECK(std::fabs(double(a) - double(b)) <= (eps))

static void sizes() {
    auto n = pmml::networkSize(3840, 2160, "fast", 0, 128);
    CHECK(n.w == 960 && n.h == 540 && n.padW == 1024 && n.padH == 640);
    n = pmml::networkSize(3840, 2160, "balanced", 0, 128);
    CHECK(n.w == 1280 && n.h == 720 && n.padW == 1280 && n.padH == 768);
    n = pmml::networkSize(3840, 2160, "quality", 0, 64);
    CHECK(n.w == 1920 && n.h == 1080 && n.padW == 1920 && n.padH == 1088);
    n = pmml::networkSize(1280, 720, "balanced", 0, 128);   // never enlarged
    CHECK(n.w == 1280 && n.h == 720);
    n = pmml::networkSize(1280, 720, "fast", 0, 128);       // at most half size
    CHECK(n.w == 640 && n.h == 360 && n.padW == 640 && n.padH == 384);
    n = pmml::networkSize(1920, 1080, "fast", 1080, 64);    // a chosen motion size replaces the tier's cap
    CHECK(n.w == 1920 && n.h == 1080);
    CHECK(pmml::modelRole("quality") == "quality" && pmml::modelRole("balanced") == "fast");
}

static void affine() {
    // identity when the network runs at the full size without padding: the network's grid is already full-size
    pmml::NetworkSize n{1280, 720, 1280, 720};
    auto a = pmml::gridAffine(1280, 720, n);
    NEAR(a[0], 1, 1e-6); NEAR(a[1], 0, 1e-6); NEAR(a[2], 1, 1e-6); NEAR(a[3], 0, 1e-6);
    // half size, padded: a network pixel centre maps to the full-size centre of the same area
    n = {640, 360, 640, 384};
    a = pmml::gridAffine(1280, 720, n);
    const double gx = 2.0 * 10 / (640 - 1) - 1;   // network pixel x = 10 -> full-size x = (10 + 0.5) * 2 - 0.5 = 20.5
    NEAR((a[0] * gx + a[1] + 1) * 0.5 * (1280 - 1), 20.5, 1e-3);
    const double gy = 2.0 * 359 / (384 - 1) - 1;  // the last real network row -> full-size y = 359.5 * 2 - 0.5 = 718.5
    NEAR((a[2] * gy + a[3] + 1) * 0.5 * (720 - 1), 718.5, 1e-3);
}

static void parsing() {
    pmml::Backend b;
    CHECK(pmml::Backend::parse("dml", &b) && b.flavor == "directml" && b.ep == "dml");
    CHECK(pmml::Backend::parse("ort:core:trt-rtx:1", &b) && b.ep == "trt-rtx" && b.device == 1 && b.key() == "ort:core:trt-rtx:1");
    CHECK(pmml::Backend::parse("ncnn", &b) && b.runtime == "ncnn");
    CHECK(!pmml::Backend::parse("tpu", &b));
    pmml::Candidate c;
    CHECK(pmml::Candidate::parse("ort:core:openvino:0|quality|once", &c) && c.tier == "quality" && c.flowOnce && c.backend.ep == "openvino");
    CHECK(c.key() == "ort:core:openvino:0|quality|once");
    const std::string j = "{\"a\": {\"b.c\": 1, \"d\": [\"x\", \"y\"]}, \"e\": \"f\\\"g\"}";
    CHECK(pmml::detail::jsonStrings(j, "a.d").size() == 2);
    CHECK(pmml::detail::jsonString(j, "e") == "f\"g");
    CHECK(pmml::detail::jsonKeys(j, "a").size() == 2);
}

static pmml::ProbeResult result(const char* key, double pack, double net, double synth) {
    pmml::ProbeResult r;
    pmml::Candidate::parse(key, &r.candidate);
    r.ok = true; r.packMs = pack; r.netMs = net; r.synthMs = synth;
    return r;
}

static void autoChoice() {
    std::vector<pmml::ProbeResult> rs = {
        result("ort:core:trt-rtx:0|quality|pert", 1, 10, 1),   // 5x: 1 + 4 * 11 = 45 ms
        result("ort:core:trt-rtx:0|quality|once", 1, 10, 1),   // 5x: 1 + 10 + 4 = 15 ms
        result("ort:core:trt-rtx:0|balanced|pert", 1, 4, 1),   // 5x: 21 ms
        result("ort:core:trt-rtx:0|fast|pert", 1, 2, 1),
    };
    CHECK(std::fabs(rs[0].costMs(5) - 45) < 1e-9 && std::fabs(rs[1].costMs(5) - 15) < 1e-9);
    // 23.976 fps, 80 %: 33.4 ms. Balanced per t (rank 5) loses to quality flow once (rank 6)
    auto c = pmml::choose(rs, 23.976, 5);
    CHECK(c.ok && c.multiplier == 5 && c.candidate.tier == "quality" && c.candidate.flowOnce);
    // at 2x flow once does not apply: quality per t (1 + 11 = 12 ms) fits
    c = pmml::choose(rs, 23.976, 2);
    CHECK(c.ok && c.multiplier == 2 && c.candidate.tier == "quality" && !c.candidate.flowOnce);
    // 60 fps: 13.3 ms; 5x: only fast (1 + 4 * 3 = 13 ms) fits
    c = pmml::choose(rs, 60, 5);
    CHECK(c.ok && c.multiplier == 5 && c.candidate.tier == "fast");
    // nothing fits: steps down, then fails
    std::vector<pmml::ProbeResult> slow = {result("ort:core:cpu:0|fast|pert", 50, 900, 40)};
    c = pmml::choose(slow, 23.976, 4);
    CHECK(!c.ok);
    // a failed probe never wins
    rs[3].ok = false;
    c = pmml::choose(rs, 60, 5);
    CHECK(!c.ok || c.candidate.tier != "fast");
}

static void cache() {
    const std::string file = "pmml_unit_cache.json";
    std::remove(file.c_str());
    std::vector<pmml::ProbeResult> rs = {result("ort:directml:dml:0|balanced|pert", 1.5, 7.25, 2), result("ort:core:cpu:0|fast|once", 3, 300, 30)};
    rs[1].ok = false; rs[1].error = "a \"quoted\" error";
    const std::string k1 = pmml::cacheKey("NVIDIA GeForce RTX 5090", 0x10de, 0x2b85, "32.0.15.9186", 3840, 2160);
    const std::string k2 = pmml::cacheKey("Other", 1, 2, "1.2", 1920, 1080);
    CHECK(k1.find('.') == std::string::npos);
    CHECK(pmml::cacheStore(file, k1, rs));
    CHECK(pmml::cacheStore(file, k2, {rs[0]}));
    std::vector<pmml::ProbeResult> back;
    CHECK(pmml::cacheLoad(file, k1, &back) && back.size() == 2);
    if (back.size() == 2) {
        NEAR(back[0].netMs, 7.25, 1e-9);
        CHECK(back[0].candidate.key() == "ort:directml:dml:0|balanced|pert");
        CHECK(!back[1].ok && back[1].error == "a \"quoted\" error");
    }
    CHECK(pmml::cacheLoad(file, k2, &back) && back.size() == 1);
    CHECK(!pmml::cacheLoad(file, "missing", &back));
    std::remove(file.c_str());
}

static void cpuGraphs() {
    // colour round trip (4:4:4 RGB exact; 4:2:0 luma within a code on a flat-chroma picture)
    const int w = 64, h = 48;
    std::vector<uint16_t> y(w * h), u(w * h / 4, 128), v(w * h / 4, 128);
    for (int i = 0; i < w * h; ++i) y[size_t(i)] = uint16_t(16 + (i * 7) % 219);
    pmml::Frame f;
    f.plane[0] = y.data(); f.plane[1] = u.data(); f.plane[2] = v.data();
    f.stride[0] = w; f.stride[1] = f.stride[2] = w / 2;
    const auto col = pmml::Colour::limited(8);
    std::vector<float> rgb(size_t(3) * w * h);
    pmml::detail::planesToRgb(f, pmml::PlaneFormat::Yuv420, col, w, h, rgb.data());
    std::vector<uint16_t> y2(y.size()), u2(u.size()), v2(v.size());
    pmml::OutFrame o;
    o.plane[0] = y2.data(); o.plane[1] = u2.data(); o.plane[2] = v2.data();
    o.stride[0] = w; o.stride[1] = o.stride[2] = w / 2;
    pmml::detail::rgbToPlanes(rgb.data(), pmml::PlaneFormat::Yuv420, col, w, h, o);
    int worst = 0;
    for (size_t i = 0; i < y.size(); ++i) worst = std::max(worst, std::abs(int(y[i]) - int(y2[i])));
    CHECK(worst <= 1);
    CHECK(u2[5] == 128 && v2[7] == 128);
    // synth with identity grids at the full size, mask 1: frame a comes back exactly; mask 0: frame b
    pmml::NetworkSize n{w, h, w, h};
    std::vector<float> g(size_t(w) * h * 2), m(size_t(w) * h, 1.f), fb(rgb.size(), 0.25f), out(rgb.size());
    for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
            g[(size_t(yy) * w + xx) * 2] = 2.f * xx / (w - 1) - 1.f;
            g[(size_t(yy) * w + xx) * 2 + 1] = 2.f * yy / (h - 1) - 1.f;
        }
    const auto aff = pmml::gridAffine(w, h, n);
    pmml::detail::synthCpu(rgb.data(), fb.data(), w, h, g.data(), g.data(), m.data(), n, aff, 0.5f, 0.5f, out.data(), 2);
    double err = 0;
    for (size_t i = 0; i < out.size(); ++i) err = std::max(err, double(std::fabs(out[i] - rgb[i])));
    CHECK(err < 1e-4);
    std::fill(m.begin(), m.end(), 0.f);
    pmml::detail::synthCpu(rgb.data(), fb.data(), w, h, g.data(), g.data(), m.data(), n, aff, 1.f, 1.f, out.data(), 1);
    CHECK(std::fabs(out[100] - 0.25f) < 1e-5);
    // a grid displaced by +4 px in x scaled by 0.5 reads 2 px to the right
    for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) g[(size_t(yy) * w + xx) * 2] = 2.f * (xx + 4) / (w - 1) - 1.f;
    std::fill(m.begin(), m.end(), 1.f);
    pmml::detail::synthCpu(rgb.data(), fb.data(), w, h, g.data(), g.data(), m.data(), n, aff, 0.5f, 0.5f, out.data(), 1);
    NEAR(out[size_t(10) * w + 20], rgb[size_t(10) * w + 22], 1e-4);
    // synth_line (anime round 5): identical to synth where there are no lines (smooth gradients, any mask)
    for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) g[(size_t(yy) * w + xx) * 2] = 2.f * xx / (w - 1) - 1.f;
    const size_t P = size_t(w) * h;
    std::vector<float> ga(rgb.size()), gb(rgb.size()), o1(rgb.size()), o2(rgb.size());
    for (int c = 0; c < 3; ++c)
        for (int yy = 0; yy < h; ++yy)
            for (int xx = 0; xx < w; ++xx) { ga[c * P + size_t(yy) * w + xx] = 0.2f + 0.005f * xx; gb[c * P + size_t(yy) * w + xx] = 0.7f - 0.004f * yy; }
    std::fill(m.begin(), m.end(), 0.5f);
    pmml::detail::synthCpu(ga.data(), gb.data(), w, h, g.data(), g.data(), m.data(), n, aff, 1.f, 1.f, o1.data(), 1);
    pmml::detail::synthCpu(ga.data(), gb.data(), w, h, g.data(), g.data(), m.data(), n, aff, 1.f, 1.f, o2.data(), 1, true);
    double d = 0;
    for (size_t i = 0; i < o1.size(); ++i) d = std::max(d, double(std::fabs(o1[i] - o2[i])));
    CHECK(d < 1e-6);
    // a dark line in a at x = 20 and in b at x = 26 (a doubled line in the blend): with mask 0.6 the plain blend shows
    // two faint lines; synth_line one, nearly a's (smoothstep(0.35, 0.65, 0.6) = 0.94)
    std::fill(ga.begin(), ga.end(), 0.8f); std::fill(gb.begin(), gb.end(), 0.8f);
    for (int c = 0; c < 3; ++c)
        for (int yy = 0; yy < h; ++yy) { ga[c * P + size_t(yy) * w + 20] = 0.1f; gb[c * P + size_t(yy) * w + 26] = 0.1f; }
    std::fill(m.begin(), m.end(), 0.6f);
    pmml::detail::synthCpu(ga.data(), gb.data(), w, h, g.data(), g.data(), m.data(), n, aff, 1.f, 1.f, o1.data(), 1);
    pmml::detail::synthCpu(ga.data(), gb.data(), w, h, g.data(), g.data(), m.data(), n, aff, 1.f, 1.f, o2.data(), 1, true);
    const size_t at20 = size_t(24) * w + 20, at26 = size_t(24) * w + 26;
    CHECK(o1[at20] > 0.3f && o1[at26] > 0.45f);          // plain: 0.38 and 0.52, two faint lines
    CHECK(o2[at20] < 0.16f && o2[at26] > 0.7f);          // line: a's line (0.15), b's nearly gone (0.75)
    CHECK(std::fabs(o2[size_t(24) * w + 40] - 0.8f) < 1e-5);   // flat elsewhere untouched
}

// Anime upscalers (pmml/upscale.h): the model table agrees with scripts/anime-models-pin.json; missing files are named.
// With PMML_TEST_RUNTIME (a runtime folder with ort/core) and PMML_TEST_GRAPHS (make_graphs.py output whose anime-sr
// folder holds the fetched weights) every model also runs on the CPU: output size, and a reference picture (flat
// areas stay flat; a smooth gradient stays within a few codes of the source at the same place), 8- and 10-bit, YUV
// and RGBA8.
static void upscaleTable() {
#ifdef PMML_PIN_FILE
    const std::string pin = pmml::detail::readText(PMML_PIN_FILE);
    CHECK(!pin.empty());
    for (const auto& m : pmml::upscaleModels()) {
        CHECK(pin.find(std::string("\"id\": \"") + m.id + "\"") != std::string::npos);
        CHECK(pin.find(std::string("\"file\": \"") + m.file + "\"") != std::string::npos);
        CHECK(pin.find(std::string("\"graph\": \"") + m.graph + "\"") != std::string::npos);
        CHECK(m.scale == 2 || m.scale == 4);
    }
#endif
    CHECK(pmml::findUpscaleModel("artcnn-c4f32") && pmml::findUpscaleModel("cunny-fast") && pmml::findUpscaleModel("animesr-v2"));
    CHECK(!pmml::findUpscaleModel("apisr"));
    pmml::UpscaleOptions o;
    o.graphsDir = "no-such-folder";
    o.model = "cunny-fast";
    CHECK(pmml::upscaleFilesMissing(o).find("CuNNy-fast-NVL.pickle") != std::string::npos);
    std::string err;
    o.backend.runtime = "ncnn";
    CHECK(!pmml::Upscaler::create(o, &err) && !err.empty());
}

static void upscaleRun() {
    const char* rt = std::getenv("PMML_TEST_RUNTIME");
    const char* graphs = std::getenv("PMML_TEST_GRAPHS");
    if (!rt || !graphs || !*rt || !*graphs) { std::printf("upscale run: skipped (PMML_TEST_RUNTIME / PMML_TEST_GRAPHS not set)\n"); return; }
    const int w = 64, h = 48;
    for (const auto& m : pmml::upscaleModels()) {
        for (int bits : {8, 10}) {
            pmml::UpscaleOptions o;
            o.runtimeDir = rt; o.graphsDir = graphs; o.model = m.id; o.width = w; o.height = h;
            o.colour = pmml::Colour::limited(bits);
            std::string err;
            auto up = pmml::Upscaler::create(o, &err);
            if (!up) { std::printf("FAIL upscale %s %d-bit: %s\n", m.id, bits, err.c_str()); ++failures; continue; }
            CHECK(up->outWidth() == w * m.scale && up->outHeight() == h * m.scale);
            // left half flat grey, right half a smooth horizontal ramp; chroma neutral
            const int sh = bits - 8;
            std::vector<uint16_t> y(size_t(w) * h), u(size_t(w / 2) * (h / 2), uint16_t(128 << sh)), v(u);
            for (int yy = 0; yy < h; ++yy)
                for (int xx = 0; xx < w; ++xx) y[size_t(yy) * w + xx] = uint16_t((xx < w / 2 ? 120 : 60 + 3 * (xx - w / 2)) << sh);
            const int ow = up->outWidth(), oh = up->outHeight();
            std::vector<uint16_t> oy(size_t(ow) * oh), ou(size_t(ow / 2) * (oh / 2)), ov(ou);
            pmml::Frame in; in.plane[0] = y.data(); in.plane[1] = u.data(); in.plane[2] = v.data(); in.stride[0] = w; in.stride[1] = in.stride[2] = w / 2;
            pmml::OutFrame out; out.plane[0] = oy.data(); out.plane[1] = ou.data(); out.plane[2] = ov.data(); out.stride[0] = ow; out.stride[1] = out.stride[2] = ow / 2;
            bool ok = true;
            for (int rep = 0; rep < (std::string(m.kind) == "animesr" ? 3 : 1) && ok; ++rep) ok = up->run(in, out, &err);   // recurrent: settles
            if (!ok) { std::printf("FAIL upscale run %s: %s\n", m.id, err.c_str()); ++failures; continue; }
            double flat = 0, ramp = 0;
            const int s = m.scale;
            for (int yy = 4 * s; yy < oh - 4 * s; ++yy)
                for (int xx = 4 * s; xx < ow - 4 * s; ++xx) {
                    const double got = oy[size_t(yy) * ow + xx] / double(1 << sh);
                    const int sx = xx / s;
                    if (sx < w / 2 - 4) flat = std::max(flat, std::fabs(got - 120));
                    else if (sx > w / 2 + 4) ramp = std::max(ramp, std::fabs(got - (60 + 3 * ((xx + 0.5) / s - 0.5 - w / 2))));
                }
            double chroma = 0;
            for (size_t i = 0; i < ou.size(); ++i) chroma = std::max(chroma, std::fabs(ou[i] / double(1 << sh) - 128));
            std::printf("upscale %-16s %2d-bit %dx%d -> %dx%d: flat max %.2f, ramp max %.2f, chroma max %.2f codes (8-bit scale)\n", m.id, bits, w, h, ow, oh, flat, ramp, chroma);
            // AnimeSR v2 is a restoration GAN: it redraws a little even on flat colour (measured 3.5-4 codes, CPU EP)
            const double tol = std::string(m.kind) == "animesr" ? 6.0 : 3.0;
            CHECK(flat <= tol);
            CHECK(ramp <= 8.0);
            CHECK(chroma <= tol);
        }
        // RGBA8 to a caller's size (pushcore's route)
        pmml::UpscaleOptions o;
        o.runtimeDir = rt; o.graphsDir = graphs; o.model = m.id; o.io = pmml::UpscaleIo::Rgba8; o.width = w; o.height = h; o.outWidth = 150; o.outHeight = 100;
        o.inPitchPx = 72; o.outPitchPx = 160;   // D3D12-style padded rows (the zero-copy layout): cropped in, padded out
        std::string err;
        auto up = pmml::Upscaler::create(o, &err);
        if (!up) { std::printf("FAIL upscale rgba8 %s: %s\n", m.id, err.c_str()); ++failures; continue; }
        CHECK(up->outWidth() == 150 && up->outHeight() == 100);
        std::vector<uint8_t> src(size_t(72) * 4 * h), dst(size_t(160) * 4 * 100, 7);
        for (size_t i = 0; i < src.size(); i += 4) { src[i] = 200; src[i + 1] = 100; src[i + 2] = 50; src[i + 3] = 255; }
        for (int yy = 0; yy < h; ++yy) for (int xx = w; xx < 72; ++xx) src[size_t(yy) * 288 + xx * 4] = 0;   // the pad columns must not leak in
        CHECK(up->runRgba(src.data(), 72 * 4, dst.data(), 160 * 4, &err));
        double e = 0;
        for (int yy = 10; yy < 90; ++yy)
            for (int xx = 10; xx < 140; ++xx)
                for (int c = 0; c < 3; ++c) e = std::max(e, std::fabs(double(dst[size_t(yy) * 640 + xx * 4 + c]) - double(src[size_t(c)])));
        std::printf("upscale %-16s rgba8 %dx%d -> 150x100: flat colour max %.0f\n", m.id, w, h, e);
        CHECK(e <= (std::string(m.kind) == "animesr" ? 6 : 3));
        CHECK(dst[size_t(50) * 640 + 150 * 4] == 0);   // the graph pads the rows to the pitch
    }
}

int main() {
    sizes();
    affine();
    parsing();
    autoChoice();
    cache();
    cpuGraphs();
    upscaleTable();
    upscaleRun();
    std::printf(failures ? "pmml_unit: %d FAILED\n" : "pmml_unit: all passed%.0d\n", failures);
    return failures ? 1 : 0;
}
