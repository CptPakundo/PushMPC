// SPDX-License-Identifier: GPL-3.0-or-later
// pmml unit tests (no runtime, no GPU): sizes, the grid mapping, parsing, Auto's choice and cache, the CPU graphs.
#include "internal.h"
#include "pmml/autobench.h"
#include "pmml/pmml.h"

#include <cmath>
#include <cstdio>
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
}

int main() {
    sizes();
    affine();
    parsing();
    autoChoice();
    cache();
    cpuGraphs();
    std::printf(failures ? "pmml_unit: %d FAILED\n" : "pmml_unit: all passed%.0d\n", failures);
    return failures ? 1 : 0;
}
