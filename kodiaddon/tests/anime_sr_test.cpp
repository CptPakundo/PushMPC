// The anime upscaler in front of the Kodi engines (src/engine/anime_sr_engine.h) on the CPU: the engine runs at the
// upscaler's size, real frames come back upscaled (flat stays flat), generated frames have that size too, and a model
// that cannot start leaves the plain engine. Needs PMML_TEST_RUNTIME (a pmml runtime with ort/core) and
// PMML_TEST_GRAPHS (make_graphs.py output with the fetched weights in anime-sr/); skipped without them.
#include "kodi_engine.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

int main()
{
    const char* rt = std::getenv("PMML_TEST_RUNTIME");
    const char* graphs = std::getenv("PMML_TEST_GRAPHS");
    if (!rt || !graphs) { std::printf("anime_sr_test: skipped (PMML_TEST_RUNTIME / PMML_TEST_GRAPHS not set)\n"); return 0; }
    // the graphs folder must sit at <runtime>/graphs for the engine: point the runtime at a folder that has both
    std::string runtime = rt;
    const int w = 64, h = 48;
    for (const char* model : {"cunny-fast", "artcnn-c4f16", "animesr-v2", "no-such-model"}) {
        pmk::EngineCfg c;
        c.width = w; c.height = h; c.bits = 8; c.backend = 1;   // the CPU engine (no Vulkan here)
        c.animeModel = model; c.pmmlRuntime = runtime; c.pmmlBackend = "ort:core:cpu:0";
        std::string why;
        auto e = pmk::OpenEngine(c, why);
        CHECK(e != nullptr);
        if (!e) continue;
        const bool known = std::string(model) != "no-such-model";
        const int scale = std::string(model) == "animesr-v2" ? 4 : 2;
        std::printf("%-14s %dx%d -> %dx%d: %s\n", model, w, h, e->OutWidth(), e->OutHeight(), why.c_str());
        CHECK(e->OutWidth() == (known ? w * scale : w) && e->OutHeight() == (known ? h * scale : h));
        std::vector<uint8_t> y(size_t(w) * h, 120), u(size_t(w / 2) * (h / 2), 128), v(u);
        pm_yuv src{};
        src.data[0] = y.data(); src.data[1] = u.data(); src.data[2] = v.data(); src.stride[0] = w; src.stride[1] = src.stride[2] = w / 2;
        std::string err;
        for (int i = 0; i < 3; ++i) CHECK(e->Push(src, i > 0, err));
        const int ow = e->OutWidth(), oh = e->OutHeight();
        std::vector<uint8_t> oy(size_t(ow) * oh), ou(size_t(ow / 2) * (oh / 2)), ov(ou);
        pm_yuv_out o{};
        o.data[0] = oy.data(); o.data[1] = ou.data(); o.data[2] = ov.data(); o.stride[0] = ow; o.stride[1] = o.stride[2] = ow / 2;
        if (known) {
            CHECK(e->Real(true, o, err));
            int worst = 0;
            for (int yy = 8; yy < oh - 8; ++yy) for (int xx = 8; xx < ow - 8; ++xx) worst = std::max(worst, std::abs(int(oy[size_t(yy) * ow + xx]) - 120));
            std::printf("  real frame: flat within %d codes\n", worst);
            CHECK(worst <= (std::string(model) == "animesr-v2" ? 6 : 2));
        }
        CHECK(e->HavePair());
        CHECK(e->Generate(0.5f, o, err));
    }
    std::printf(fails ? "anime_sr_test: %d FAILED\n" : "anime_sr_test: all passed%.0d\n", fails);
    return fails ? 1 : 0;
}
