// pm_engine on vsplugin's portable engines (see pm_engine.h): the GPU engine (engines/gpu, Vulkan) when it is built in
// and a usable Vulkan device answers, else the CPU engine (engines/portable). Both are PushFPS's portable core: block
// matching both ways, motion-compensated interpolation, the scene-cut rule; the GPU one adds PushMPC's cut guard.
#include "pm_engine.h"
#include "portable_engine.h"
#include "backend.h"   // pmvs::DecidePortableK: the same Auto rule as the mpv route
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <tuple>
#ifdef PMVS_GPU
#include "gpu_backend.h"   // CutGuard, CalibrateGpu
#endif
#ifdef PM_DMABUF
#include "pm_dmabuf_vk.h"  // zero copy: VA-API dmabufs into and out of the GPU engine
#endif

struct pm_engine {
    bool gpu = false;
    int fullscreen = -1;   // pm_engine_set_fullscreen: VLC's "fullscreen" option (Auto: the window composition per shown frame)
    int width = 0, height = 0, bits = 8, threads = 0, quality = 1;
    pmvs::portable::Engine cpu;
#ifdef PMVS_GPU
    std::unique_ptr<pmvs::gpu::Engine> g;
    pmvs::gpu::CutGuard guard;
    int slotPrev = -1, slotCur = -1, pair = -1;
    bool cut = false;
    pm_engine_stats st{};
    std::string device;
#ifdef PM_DMABUF
    std::unique_ptr<pmdb::DmabufIO> db;   // made on first use
    bool dbTried = false;
    std::string dbWhy;
#endif
#endif
};

static void SetErr(char *err, size_t n, const std::string &s)
{
    if (err && n) { std::snprintf(err, n, "%s", s.c_str()); }
}

#ifdef PMVS_GPU
static bool OpenGpu(pm_engine *e, const pm_engine_cfg *cfg, std::string &why)
{
    auto g = std::make_unique<pmvs::gpu::Engine>();
    pmvs::gpu::Config c;
    c.width = cfg->width; c.height = cfg->height; c.bits = cfg->bits;
    c.quality = 2;                         // full-size motion vectors (the GPU engine's default in the mpv route)
    c.outWidth = 0; c.outHeight = 0;       // VLC keeps the source size
    c.upscaler = pmvs::gpu::Upscaler::None;
    c.sourceSlots = 6; c.outSlots = 4;
    c.blockMatcherOnly = false; c.nvofHybrid = true;   // Auto: NVIDIA optical flow as one more block-matcher candidate where
                                                     // the device has it (the release test matrix's best), else the block matcher
    pmvs::gpu::DeviceRequest req;
    if (const char *d = std::getenv("PUSHMPC_VK_DEVICE")) req.device = d;
    if (const char *a = std::getenv("PUSHMPC_VK_ALLOW_CPU")) req.allowCpu = a[0] == '1';
    if (!g->Configure(c, req, why)) return false;
    e->device = g->Device().name;
    e->g = std::move(g);
    e->gpu = true;
    return true;
}
static void GpuForget(pm_engine *e)
{
    if (e->pair >= 0) e->g->ReleasePair(e->pair);
    if (e->slotPrev >= 0) e->g->ReleaseSlot(e->slotPrev);
    if (e->slotCur >= 0) e->g->ReleaseSlot(e->slotCur);
    e->pair = e->slotPrev = e->slotCur = -1;
    e->cut = false;
}
#endif

extern "C" pm_engine *pm_engine_open(const pm_engine_cfg *cfg, char *err, size_t errsize)
{
    if (!cfg || cfg->width < 16 || cfg->height < 16 || cfg->bits < 8 || cfg->bits > 16) {
        SetErr(err, errsize, "pm_engine: bad configuration");
        return nullptr;
    }
    pm_engine *e = new (std::nothrow) pm_engine();
    if (!e) { SetErr(err, errsize, "pm_engine: out of memory"); return nullptr; }
    e->width = cfg->width; e->height = cfg->height; e->bits = cfg->bits; e->threads = cfg->threads; e->quality = cfg->quality;
    std::string why;
#ifdef PMVS_GPU
    if (cfg->engine != PM_ENGINE_CPU) {
        if (OpenGpu(e, cfg, why)) return e;
        if (cfg->engine == PM_ENGINE_GPU) { SetErr(err, errsize, "GPU engine: " + why); delete e; return nullptr; }
        SetErr(err, errsize, "GPU engine unavailable (" + why + "): CPU engine");
    }
#else
    if (cfg->engine == PM_ENGINE_GPU) { SetErr(err, errsize, "this build has no GPU engine"); delete e; return nullptr; }
#endif
    pmvs::portable::Config c;
    c.width = cfg->width; c.height = cfg->height; c.bits = cfg->bits; c.threads = cfg->threads; c.quality = cfg->quality;
    std::string cpuWhy;
    if (!e->cpu.Configure(c, cpuWhy)) { SetErr(err, errsize, cpuWhy); delete e; return nullptr; }
    return e;
}

extern "C" void pm_engine_close(pm_engine *e)
{
#ifdef PMVS_GPU
    if (e && e->gpu) GpuForget(e);
#endif
    delete e;
}

extern "C" int pm_engine_push(pm_engine *e, const pm_yuv *src, int contiguous, char *err, size_t errsize)
{
    pmvs::portable::YuvView v;
    v.width = e->width; v.height = e->height; v.bits = e->bits;
    for (int i = 0; i < 3; ++i) { v.data[i] = src->data[i]; v.stride[i] = src->stride[i]; }
    std::string why;
#ifdef PMVS_GPU
    if (e->gpu) {
        if (!contiguous) GpuForget(e);
        const int slot = e->g->Upload(v, why);
        if (slot < 0) { SetErr(err, errsize, why); GpuForget(e); return -1; }
        e->st.pushed++;
        if (e->slotCur < 0) { e->slotCur = slot; return 0; }
        pmvs::gpu::PairStats ps;
        const int pair = e->g->Estimate(e->slotCur, slot, e->pair, ps, why);
        // the pair before (its fields were the temporal predictors) and its first frame are done with
        if (e->pair >= 0) e->g->ReleasePair(e->pair);
        if (e->slotPrev >= 0) e->g->ReleaseSlot(e->slotPrev);
        e->slotPrev = e->slotCur;
        e->slotCur = slot;
        e->pair = pair;
        if (pair < 0) { SetErr(err, errsize, why); return -1; }
        e->cut = e->guard.Decide(ps) != 0;
        e->st.pairs++;
        if (e->cut) e->st.cuts++;
        e->st.me_ms += (ps.ms - e->st.me_ms) / double(e->st.pairs);
        return 0;
    }
#endif
    if (!e->cpu.Push(v, contiguous != 0, why)) { SetErr(err, errsize, why); return -1; }
    return 0;
}

extern "C" int pm_engine_have_pair(const pm_engine *e)
{
#ifdef PMVS_GPU
    if (e->gpu) return e->pair >= 0 ? 1 : 0;
#endif
    return e->cpu.HavePair() ? 1 : 0;
}

extern "C" int pm_engine_pair_is_cut(const pm_engine *e)
{
#ifdef PMVS_GPU
    if (e->gpu) return e->cut ? 1 : 0;
#endif
    return e->cpu.PairIsCut() ? 1 : 0;
}

extern "C" void pm_engine_generate(pm_engine *e, float t, const pm_yuv_out *out)
{
    pmvs::portable::YuvOut o;
    for (int i = 0; i < 3; ++i) { o.data[i] = out->data[i]; o.stride[i] = out->stride[i]; }
#ifdef PMVS_GPU
    if (e->gpu) {
        if (e->pair < 0) return;
        const int mode = e->cut ? (t < 0.5f ? 1 : 2) : 0;   // a cut: the nearer real frame, no morph
        int slot = -1; uint64_t ticket = 0; std::string why;
        const auto t0 = std::chrono::steady_clock::now();
        if (e->g->Generate(e->pair, t, mode, slot, ticket, why) && e->g->Wait(ticket, why)) {
            e->g->Read(slot, o);    // frees the readback slot
            e->st.generated++;
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            e->st.gen_ms += (ms - e->st.gen_ms) / double(e->st.generated);
        } else if (slot >= 0) {
            e->g->ReleaseOut(slot);
        }
        return;
    }
#endif
    e->cpu.Generate(t, o);
}

extern "C" void pm_engine_forget(pm_engine *e)
{
#ifdef PMVS_GPU
    if (e->gpu) { GpuForget(e); return; }
#endif
    e->cpu.Forget();
}

extern "C" void pm_engine_stats_get(const pm_engine *e, pm_engine_stats *out)
{
#ifdef PMVS_GPU
    if (e->gpu) { *out = e->st; return; }
#endif
    const auto &s = e->cpu.Stat();
    out->pushed = s.pushed; out->pairs = s.pairs; out->cuts = s.cuts; out->generated = s.generated;
    out->me_ms = s.meMs; out->gen_ms = s.genMs;
}

extern "C" int pm_engine_threads(const pm_engine *e) { return e->gpu ? 0 : e->cpu.Threads(); }

extern "C" const char *pm_engine_name(const pm_engine *e)
{
    static thread_local std::string s;
#ifdef PMVS_GPU
    if (e->gpu) { s = "GPU engine (Vulkan) on " + e->device; return s.c_str(); }
#endif
    s = "CPU engine, " + std::to_string(e->cpu.Threads()) + " threads";
    return s.c_str();
}

extern "C" void pm_engine_set_fullscreen(pm_engine *e, int fullscreen) { if (e) e->fullscreen = fullscreen; }

extern "C" int pm_engine_decide(pm_engine *e, int smoother, double src_fps, double display_hz, char *why, size_t whysize)
{
    // the calibration costs one motion estimation and one generated frame; cached per (engine kind, size, depth)
    static std::mutex mu;
    static std::map<std::tuple<bool, int, int, int, int, int>, std::pair<double, double>> cache;
    double me = 0, gen = 0;
    bool ok = true;
    if (smoother != 1) {
        std::lock_guard<std::mutex> lock(mu);
        auto key = std::make_tuple(e->gpu, e->width, e->height, e->bits, e->threads, e->quality);
        auto it = cache.find(key);
        if (it == cache.end()) {
#ifdef PMVS_GPU
            if (e->gpu) {
                GpuForget(e);
                const pmvs::gpu::GpuCalibration cal = pmvs::gpu::CalibrateGpu(*e->g);
                ok = cal.ok;
                it = cache.emplace(key, std::make_pair(cal.meMs, cal.genMs)).first;
            } else
#endif
            {
                const pmvs::portable::Calibration cal = pmvs::portable::Calibrate(e->width, e->height, e->bits, e->threads, e->quality);
                it = cache.emplace(key, std::make_pair(cal.meMs, cal.genMs)).first;
            }
        }
        me = it->second.first; gen = it->second.second;
    }
    const pmvs::PortableK k = pmvs::DecidePortableK(smoother, src_fps, display_hz, e->height, me, gen, e->gpu, e->fullscreen == 1 ? 0.0 : -1.0, pmvs::VrrActive(e->fullscreen));
    SetErr(why, whysize, std::string(ok ? "" : "GPU trial run failed; ") + k.why);
    return k.k;
}

extern "C" int pm_engine_decide_k(int smoother, double src_fps, double display_hz, int width, int height, int bits,
                                  int threads, int quality, char *why, size_t whysize)
{
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
    const pmvs::PortableK k = pmvs::DecidePortableK(smoother, src_fps, display_hz, height, cal.meMs, cal.genMs, false, -1, pmvs::VrrActive(-1));
    SetErr(why, whysize, k.why);
    return k.k;
}

// ---- zero copy (pm_engine.h): VA-API dmabufs straight into the GPU engine and back
#ifdef PM_DMABUF
static bool DbReady(pm_engine *e)
{
    if (!e->gpu) { e->dbWhy = "the CPU engine runs (no Vulkan device)"; return false; }
    if (e->bits != 8 && e->bits != 10) { e->dbWhy = "only 8-bit (NV12) and 10-bit (P010) surfaces"; return false; }
    if (!e->dbTried) {
        e->dbTried = true;
        auto db = std::make_unique<pmdb::DmabufIO>();
        if (db->Init(e->g->Ctx(), e->dbWhy)) e->db = std::move(db);
    }
    return e->db != nullptr;
}
#endif

extern "C" int pm_engine_dmabuf_ok(const pm_engine *e, const char **why)
{
#ifdef PM_DMABUF
    pm_engine *m = const_cast<pm_engine *>(e);
    const bool ok = DbReady(m);
    if (why) *why = ok ? "" : m->dbWhy.c_str();
    return ok ? 1 : 0;
#else
    (void)e;
    if (why) *why = "this build has no zero-copy path (Linux, the GPU engine and the SPIR-V of vk/*.comp)";
    return 0;
#endif
}

extern "C" int pm_engine_push_dmabuf(pm_engine *e, const pm_dmabuf *src, int contiguous, char *err, size_t errsize)
{
#ifdef PM_DMABUF
    if (!DbReady(e)) { SetErr(err, errsize, e->dbWhy); return -1; }
    std::string why;
    pmdb::Imported im;
    if (!e->db->Import(*src, false, im, why)) { SetErr(err, errsize, why); return -1; }
    if (!contiguous) GpuForget(e);
    uint64_t ticket = 0;
    const int slot = e->g->UploadWith([&](VkCommandBuffer cb, const pmvs::gpu::Engine::PlanarDesc &d, std::string &er) {
        return e->db->RecordIn(cb, im, d, er); }, ticket, why);
    // the import lives until the upload has run
    std::string werr;
    if (ticket) e->g->Wait(ticket, werr);
    e->db->Recycle();
    e->db->Release(im);
    if (slot < 0) { SetErr(err, errsize, why); GpuForget(e); return -1; }
    e->st.pushed++;
    if (e->slotCur < 0) { e->slotCur = slot; return 0; }
    pmvs::gpu::PairStats ps;
    const int pair = e->g->Estimate(e->slotCur, slot, e->pair, ps, why);
    if (e->pair >= 0) e->g->ReleasePair(e->pair);
    if (e->slotPrev >= 0) e->g->ReleaseSlot(e->slotPrev);
    e->slotPrev = e->slotCur;
    e->slotCur = slot;
    e->pair = pair;
    if (pair < 0) { SetErr(err, errsize, why); return -1; }
    e->cut = e->guard.Decide(ps) != 0;
    e->st.pairs++;
    if (e->cut) e->st.cuts++;
    e->st.me_ms += (ps.ms - e->st.me_ms) / double(e->st.pairs);
    return 0;
#else
    (void)e; (void)src; (void)contiguous;
    SetErr(err, errsize, "no zero-copy path in this build");
    return -1;
#endif
}

extern "C" int pm_engine_generate_dmabuf(pm_engine *e, float t, const pm_dmabuf *dst, char *err, size_t errsize)
{
#ifdef PM_DMABUF
    if (!DbReady(e)) { SetErr(err, errsize, e->dbWhy); return -1; }
    if (e->pair < 0) { SetErr(err, errsize, "no pair yet"); return -1; }
    std::string why;
    pmdb::Imported im;
    if (!e->db->Import(*dst, true, im, why)) { SetErr(err, errsize, why); return -1; }
    const int mode = e->cut ? (t < 0.5f ? 1 : 2) : 0;   // a cut: the nearer real frame, as pm_engine_generate
    int out = -1;
    uint64_t ticket = 0;
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = e->g->GenerateWith(e->pair, t, mode, [&](VkCommandBuffer cb, const pmvs::gpu::Engine::PackedDesc &d, std::string &er) {
        return e->db->RecordOut(cb, im, d, er); }, out, ticket, why);
    std::string werr;
    const bool waited = ticket && e->g->Wait(ticket, werr);
    if (out >= 0) e->g->ReleaseOut(out);
    e->db->Recycle();
    e->db->Release(im);
    if (!ok || !waited) { SetErr(err, errsize, ok ? werr : why); return -1; }
    e->st.generated++;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    e->st.gen_ms += (ms - e->st.gen_ms) / double(e->st.generated);
    return 0;
#else
    (void)e; (void)t; (void)dst;
    SetErr(err, errsize, "no zero-copy path in this build");
    return -1;
#endif
}
