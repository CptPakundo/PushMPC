// pm_engine on vsplugin's portable engine (vsplugin/engines/portable, PushFPS's portable core). See pm_engine.h.
#include "pm_engine.h"
#include "portable_engine.h"
#include "backend.h"   // pmvs::DecidePortableK: the same Auto rule as the mpv route
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <tuple>

struct pm_engine {
    pmvs::portable::Engine eng;
    int width = 0, height = 0, bits = 8;
};

static void SetErr(char *err, size_t n, const std::string &s)
{
    if (err && n) { std::snprintf(err, n, "%s", s.c_str()); }
}

extern "C" pm_engine *pm_engine_open(const pm_engine_cfg *cfg, char *err, size_t errsize)
{
    if (!cfg || cfg->width < 16 || cfg->height < 16 || cfg->bits < 8 || cfg->bits > 16) {
        SetErr(err, errsize, "pm_engine: bad configuration");
        return nullptr;
    }
    pm_engine *e = new (std::nothrow) pm_engine();
    if (!e) { SetErr(err, errsize, "pm_engine: out of memory"); return nullptr; }
    pmvs::portable::Config c;
    c.width = cfg->width; c.height = cfg->height; c.bits = cfg->bits; c.threads = cfg->threads; c.quality = cfg->quality;
    std::string why;
    if (!e->eng.Configure(c, why)) { SetErr(err, errsize, why); delete e; return nullptr; }
    e->width = cfg->width; e->height = cfg->height; e->bits = cfg->bits;
    return e;
}

extern "C" void pm_engine_close(pm_engine *e) { delete e; }

extern "C" int pm_engine_push(pm_engine *e, const pm_yuv *src, int contiguous, char *err, size_t errsize)
{
    pmvs::portable::YuvView v;
    v.width = e->width; v.height = e->height; v.bits = e->bits;
    for (int i = 0; i < 3; ++i) { v.data[i] = src->data[i]; v.stride[i] = src->stride[i]; }
    std::string why;
    if (!e->eng.Push(v, contiguous != 0, why)) { SetErr(err, errsize, why); return -1; }
    return 0;
}

extern "C" int pm_engine_have_pair(const pm_engine *e) { return e->eng.HavePair() ? 1 : 0; }
extern "C" int pm_engine_pair_is_cut(const pm_engine *e) { return e->eng.PairIsCut() ? 1 : 0; }

extern "C" void pm_engine_generate(pm_engine *e, float t, const pm_yuv_out *out)
{
    pmvs::portable::YuvOut o;
    for (int i = 0; i < 3; ++i) { o.data[i] = out->data[i]; o.stride[i] = out->stride[i]; }
    e->eng.Generate(t, o);
}

extern "C" void pm_engine_forget(pm_engine *e) { e->eng.Forget(); }

extern "C" void pm_engine_stats_get(const pm_engine *e, pm_engine_stats *out)
{
    const auto &s = e->eng.Stat();
    out->pushed = s.pushed; out->pairs = s.pairs; out->cuts = s.cuts; out->generated = s.generated;
    out->me_ms = s.meMs; out->gen_ms = s.genMs;
}

extern "C" int pm_engine_threads(const pm_engine *e) { return e->eng.Threads(); }

extern "C" int pm_engine_decide_k(int smoother, double src_fps, double display_hz, int width, int height, int bits,
                                  int threads, int quality, char *why, size_t whysize)
{
    // the calibration costs one motion estimation and one generated frame; cache it per (size, depth, quality)
    static std::mutex mu;
    static std::map<std::tuple<int, int, int, int, int>, pmvs::portable::Calibration> cache;
    pmvs::portable::Calibration cal;
    if (smoother != 1) {
        std::lock_guard<std::mutex> lock(mu);
        auto key = std::make_tuple(width, height, bits, threads, quality);
        auto it = cache.find(key);
        if (it == cache.end())
            it = cache.emplace(key, pmvs::portable::Calibrate(width, height, bits, threads, quality)).first;
        cal = it->second;
    }
    const pmvs::PortableK k = pmvs::DecidePortableK(smoother, src_fps, display_hz, height, cal.meMs, cal.genMs);
    SetErr(why, whysize, k.why);
    return k.k;
}
