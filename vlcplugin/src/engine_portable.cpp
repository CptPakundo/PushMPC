// pm_engine on vsplugin's portable engines (see pm_engine.h): the GPU engine (engines/gpu, Vulkan) when it is built in
// and a usable Vulkan device answers, else the CPU engine (engines/portable). Both are PushFPS's portable core: block
// matching both ways, motion-compensated interpolation, the scene-cut rule; the GPU one adds PushMPC's cut guard.
// macOS (PM_APPLE_MODULE): Auto tries Apple's VideoToolbox interpolation first, through libpushmpc_engine_apple.dylib next
// to this plugin (the same apple engine Plex for Mac and IINA use; its self-test decides on this Mac). The module asks
// for phases j/k only (k = the multiplier pm_engine_decide returned), so the apple engine's k - 1 frames per pair are
// made on push and each request takes its slot round(t * k). Order: apple, then the GPU engine (MoltenVK), then the CPU.
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
#ifdef PM_APPLE_MODULE
#include <dlfcn.h>
#include <vector>
#include "engine.h"        // pme::Backend: the engine module's C++ backend interface (mpvnative/engine)
#endif

struct pm_engine {
    bool gpu = false;
    int engineReq = PM_ENGINE_AUTO;
#ifdef PM_APPLE_MODULE
    pme::Backend* ap = nullptr;            // the apple engine (VideoToolbox), when it runs
    int apK = 1;
    bool apPair = false;
    uint64_t apSeq = 0;
    std::vector<uint8_t> apBuf[8][3];      // the pair's generated frames, slot j = phase j / k
    pm_engine_stats apSt{};
    std::string apName;
#endif
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

#ifdef PM_APPLE_MODULE
// libpushmpc_engine_apple.dylib from this plugin's folder; the backend for this stream, or null with the reason
static pme::Backend *OpenApple(pm_engine *e, int smoother, double fps, double hz, pmeng_output &out, std::string &why)
{
    using OpenFn = pme::Backend *(*)(const pmeng_stream *, const pme::Options *, pmeng_output *, std::string *, void (*)(const char *));
    static OpenFn fn = nullptr;
    static std::string loadWhy;
    static std::once_flag once;
    std::call_once(once, [] {
        Dl_info di;
        std::string dir;
        if (dladdr(reinterpret_cast<void *>(&pm_engine_open), &di) && di.dli_fname) { dir = di.dli_fname; dir = dir.substr(0, dir.rfind('/') + 1); }
        void *h = dlopen((dir + "libpushmpc_engine_apple.dylib").c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h) { const char *d = dlerror(); loadWhy = std::string("apple engine not loaded (") + (d ? d : "?") + ")"; return; }
        fn = reinterpret_cast<OpenFn>(dlsym(h, "pme_apple_open"));
        if (!fn) loadWhy = "apple engine: no pme_apple_open";
    });
    if (!fn) { why = loadWhy; return nullptr; }
    if (e->bits != 8 && e->bits != 10) { why = "apple engine: 8 or 10 bits only"; return nullptr; }
    pmeng_stream st{}; st.struct_size = sizeof(st);
    st.width = uint32_t(e->width); st.height = uint32_t(e->height); st.fmt = e->bits == 10 ? PMENG_FMT_YUV420P10 : PMENG_FMT_YUV420P;
    if (fps > 0) { st.fps_num = int32_t(fps * 1000 + 0.5); st.fps_den = 1000; }
    pme::Options o;
    o["smoother"] = smoother == 0 ? "auto" : std::to_string(smoother);
    if (hz > 0) o["display_fps"] = std::to_string(hz);
    std::memset(&out, 0, sizeof(out)); out.struct_size = sizeof(out);
    return fn(&st, &o, &out, &why, nullptr);
}
static void AppleClose(pm_engine *e) { delete e->ap; e->ap = nullptr; }
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
    e->engineReq = cfg->engine;
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
#ifdef PM_APPLE_MODULE
    if (e) AppleClose(e);
#endif
#ifdef PMVS_GPU
    if (e && e->gpu) GpuForget(e);
#endif
    delete e;
}

extern "C" int pm_engine_push(pm_engine *e, const pm_yuv *src, int contiguous, char *err, size_t errsize)
{
#ifdef PM_APPLE_MODULE
    if (e->ap) {
        pmeng_image im{};
        im.mem = PMENG_MEM_CPU; im.fmt = e->bits == 10 ? PMENG_FMT_YUV420P10 : PMENG_FMT_YUV420P;
        im.width = uint32_t(e->width); im.height = uint32_t(e->height);
        for (int i = 0; i < 3; ++i) { im.data[i] = const_cast<uint8_t *>(src->data[i]); im.linesize[i] = src->stride[i]; }
        if (e->ap->Submit(im, int64_t(e->apSeq) * 41667, contiguous ? 0u : PMENG_SUBMIT_DISCONTINUITY) != PMENG_OK) {
            SetErr(err, errsize, "apple engine: " + e->ap->err); return -1;
        }
        e->apSeq++; e->apSt.pushed++;
        e->apPair = false;
        const size_t bps = e->bits > 8 ? 2 : 1, cw = size_t((e->width + 1) / 2), ch = size_t((e->height + 1) / 2);
        pmeng_out_info oi{};
        while (e->ap->Peek(oi, 0) == 1) {
            if (oi.subframe == 0 || oi.subframe >= 8) { e->ap->Skip(); continue; }   // the real frame: VLC keeps its own
            auto &b = e->apBuf[oi.subframe];
            b[0].resize(size_t(e->width) * bps * size_t(e->height)); b[1].resize(cw * bps * ch); b[2].resize(cw * bps * ch);
            pmeng_image d{};
            d.mem = PMENG_MEM_CPU; d.fmt = im.fmt; d.width = im.width; d.height = im.height;
            for (int i = 0; i < 3; ++i) { d.data[i] = b[i].data(); d.linesize[i] = ptrdiff_t(i ? cw * bps : size_t(e->width) * bps); }
            if (e->ap->Write(d) != PMENG_OK) { SetErr(err, errsize, "apple engine: " + e->ap->err); return -1; }
            e->apPair = true;
        }
        if (e->apPair) e->apSt.pairs++;
        return 0;
    }
#endif
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
#ifdef PM_APPLE_MODULE
    if (e->ap) return e->apPair ? 1 : 0;
#endif
#ifdef PMVS_GPU
    if (e->gpu) return e->pair >= 0 ? 1 : 0;
#endif
    return e->cpu.HavePair() ? 1 : 0;
}

extern "C" int pm_engine_pair_is_cut(const pm_engine *e)
{
#ifdef PM_APPLE_MODULE
    if (e->ap) return 0;   // the apple engine holds the nearer real frame in its own slots on a cut
#endif
#ifdef PMVS_GPU
    if (e->gpu) return e->cut ? 1 : 0;
#endif
    return e->cpu.PairIsCut() ? 1 : 0;
}

extern "C" void pm_engine_generate(pm_engine *e, float t, const pm_yuv_out *out)
{
#ifdef PM_APPLE_MODULE
    if (e->ap) {
        if (!e->apPair) return;
        int j = int(t * float(e->apK) + 0.5f);
        j = j < 1 ? 1 : j > e->apK - 1 ? e->apK - 1 : j;
        const auto &b = e->apBuf[j];
        if (b[0].empty()) return;
        const size_t bps = e->bits > 8 ? 2 : 1, cw = size_t((e->width + 1) / 2), ch = size_t((e->height + 1) / 2);
        for (int r = 0; r < e->height; ++r) std::memcpy(out->data[0] + r * out->stride[0], b[0].data() + size_t(r) * e->width * bps, size_t(e->width) * bps);
        for (int i = 1; i < 3; ++i) for (size_t r = 0; r < ch; ++r) std::memcpy(out->data[i] + r * out->stride[i], b[i].data() + r * cw * bps, cw * bps);
        e->apSt.generated++;
        return;
    }
#endif
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
#ifdef PM_APPLE_MODULE
    if (e->ap) { e->ap->Flush(); e->apPair = false; return; }
#endif
#ifdef PMVS_GPU
    if (e->gpu) { GpuForget(e); return; }
#endif
    e->cpu.Forget();
}

extern "C" void pm_engine_stats_get(const pm_engine *e, pm_engine_stats *out)
{
#ifdef PM_APPLE_MODULE
    if (e->ap) { *out = e->apSt; return; }
#endif
#ifdef PMVS_GPU
    if (e->gpu) { *out = e->st; return; }
#endif
    const auto &s = e->cpu.Stat();
    out->pushed = s.pushed; out->pairs = s.pairs; out->cuts = s.cuts; out->generated = s.generated;
    out->me_ms = s.meMs; out->gen_ms = s.genMs;
}

extern "C" int pm_engine_threads(const pm_engine *e)
{
#ifdef PM_APPLE_MODULE
    if (e->ap) return 0;
#endif
    return e->gpu ? 0 : e->cpu.Threads();
}

extern "C" const char *pm_engine_name(const pm_engine *e)
{
    static thread_local std::string s;
#ifdef PM_APPLE_MODULE
    if (e->ap) { s = e->apName; return s.c_str(); }
#endif
#ifdef PMVS_GPU
    if (e->gpu) { s = "GPU engine (Vulkan) on " + e->device; return s.c_str(); }
#endif
    s = "CPU engine, " + std::to_string(e->cpu.Threads()) + " threads";
    return s.c_str();
}

extern "C" int pm_engine_decide(pm_engine *e, int smoother, double src_fps, double display_hz, char *why, size_t whysize)
{
#ifdef PM_APPLE_MODULE
    // Auto: VideoToolbox first (its own self-test and multiplier rule); else the engine opened above
    std::string appleWhy;
    if (e->engineReq == PM_ENGINE_AUTO && smoother != 1 && !std::getenv("PUSHMPC_VLC_NO_APPLE")) {
        AppleClose(e);
        pmeng_output out{};
        e->ap = OpenApple(e, smoother, src_fps, display_hz, out, appleWhy);
        if (e->ap && out.multiplier >= 2 && out.multiplier <= 8) {
            e->apK = int(out.multiplier);
            e->apName = std::string("apple engine (VideoToolbox): ") + out.description;
            SetErr(why, whysize, "x" + std::to_string(e->apK) + " (the apple engine's rule)");
            return e->apK;
        }
        AppleClose(e);
    }
#endif
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
    const pmvs::PortableK k = pmvs::DecidePortableK(smoother, src_fps, display_hz, e->height, me, gen, e->gpu);
    std::string pre = ok ? "" : "GPU trial run failed; ";
#ifdef PM_APPLE_MODULE
    if (!appleWhy.empty()) pre += "VideoToolbox not used (" + appleWhy + "); ";
#endif
    SetErr(why, whysize, pre + k.why);
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
    const pmvs::PortableK k = pmvs::DecidePortableK(smoother, src_fps, display_hz, height, cal.meMs, cal.genMs);
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
