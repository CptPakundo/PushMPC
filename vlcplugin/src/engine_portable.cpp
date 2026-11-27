// pm_engine on vsplugin's portable engines (see pm_engine.h): the GPU engine (engines/gpu, Vulkan) when it is built in
// and a usable Vulkan device answers, else the CPU engine (engines/portable). Both are PushFPS's portable core: block
// matching both ways, motion-compensated interpolation, the scene-cut rule; the GPU one adds PushMPC's cut guard.
// macOS (PM_APPLE_MODULE): Auto tries Apple's VideoToolbox interpolation first, through libpushmpc_engine_apple.dylib next
// to this plugin (the same apple engine Plex for Mac and IINA use; its self-test decides on this Mac). The module asks
// for phases j/k only (k = the multiplier pm_engine_decide returned), so the apple engine's k - 1 frames per pair are
// made on push and each request takes its slot round(t * k). Order: apple, then the GPU engine (MoltenVK), then the CPU.
// The held-drawing repair (engine_portable.h): pairs lag kLookahead source frames behind the checks, so each held part
// can be laid in from its own pair first (in the GPU engine's slots, or on the CPU for the CPU and apple engines).
#include "pm_engine.h"
#include "engine_portable.h"
#include "portable_engine.h"
#include "backend.h"   // pmvs::DecidePortableK: the same Auto rule as the mpv route
#include "held_dedup.h" // held frames: PushFPS's duplicate test on the quarter-size luma (CPU engine)
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
#ifdef PMVS_GPU
#include "gpu_backend.h"   // CutGuard, CalibrateGpu, RegionRepair (region_repair.h)
#endif
#if __has_include("../../pushmpc/src/HeldRepair.h")
#include "../../pushmpc/src/HeldRepair.h"   // the repair's scheduler, the CPU cell grid and composite
#define PM_HELD_REPAIR 1
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
    int fullscreen = -1;   // pm_engine_set_fullscreen: VLC's "fullscreen" option (Auto: the window composition per shown frame)
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
    // held frames (pm_engine_set_dedup): checked = the frame tested by pm_engine_check_dup is a new picture and its
    // push comes next; CPU engine: the quarter-size luma of the last distinct frame and of the one tested; GPU engine:
    // the tested frame's slot (the push takes it), compared with slotCur (the last distinct frame)
    bool dedup = false, haveDistinct = false, lastDup = false, checked = false;
    std::vector<uint8_t> lastQ, curQ;
    int staged = -1;
    // the held-drawing repair (engine_portable.h): lag > 0 = the pairs follow the checks lag source frames behind.
    // Source frames are numbered by the checks (lagSrc: the newest); the new pictures wait in lagWait until lagReady
    // reaches them; lagA / lagB: the pictures of the pair advanced last. lagGpu: the frames live in the GPU engine's
    // slots (lagSlot) and are repaired there; else 16-bit CPU copies (lagPic) repaired on the CPU.
    int lag = 0;
    bool lagGpu = false, repOn = false;
    bool repDefer = false;           // pm_engine_defer_repair: the CPU repair of a checked frame waits for pm_engine_run_deferred
    std::deque<int64_t> repPending;  // frames whose CPU repair waits (in order)
    bool plainSaved = false, plainDedup = false;   // pm_engine_plain: lag and held frames set aside for a probe
    int plainLag = 0;
    double srcMs = 1000.0 / 23.976;
    int64_t lagSrc = -1, lagLastDistinct = -1, lagReady = -1, lagA = -1, lagB = -1;
    struct LagItem { int64_t s; bool contig; };
    std::deque<LagItem> lagWait;
    std::set<int64_t> lagRepaired;    // frames whose picture the repair changed (still kept)
    uint64_t repFailed = 0;
    std::vector<uint8_t> natA[3], natB[3], natOut[3];   // pictures in the engine's own format (8 bits narrowed)
#ifdef PMVS_GPU
    pmvs::gpu::RegionRepair gpuRep;
    std::map<int64_t, int> lagSlot;
#endif
#ifdef PM_HELD_REPAIR
    struct P16 { int w = 0, h = 0, cw = 0, ch = 0; std::vector<uint16_t> p[3]; };
    pushmpc::held::RepairScheduler sched;
    std::map<int64_t, P16> lagPic;
    pmvs::portable::Engine part;      // the CPU repair's parts: one pair at a time
    bool partReady = false;
    P16 rep;
#endif
};

static void SetErr(char *err, size_t n, const std::string &s)
{
    if (err && n) { std::snprintf(err, n, "%s", s.c_str()); }
}

#ifdef PMVS_GPU
static bool OpenGpu(pm_engine *e, const pm_engine_cfg *cfg, std::string &why, bool repair = false)
{
    auto g = std::make_unique<pmvs::gpu::Engine>();
    pmvs::gpu::Config c;
    c.width = cfg->width; c.height = cfg->height; c.bits = cfg->bits;
    c.quality = 2;                         // full-size motion vectors (the GPU engine's default in the mpv route)
    c.outWidth = 0; c.outHeight = 0;       // VLC keeps the source size
    c.upscaler = pmvs::gpu::Upscaler::None;
    c.sourceSlots = 6; c.outSlots = 4;
    // the held-drawing repair: storage source slots, and room for the frames a part's pair can start at (kLookahead + 1
    // behind) and end at (kLookahead ahead), as the other portable routes
    c.regionRepair = repair;
    if (repair) c.sourceSlots = 14;
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
    if (!e->lagGpu) {   // lagged on the GPU the pair's slots belong to lagSlot
        if (e->slotPrev >= 0) e->g->ReleaseSlot(e->slotPrev);
        if (e->slotCur >= 0) e->g->ReleaseSlot(e->slotCur);
    }
    e->pair = e->slotPrev = e->slotCur = -1;
    e->cut = false;
}
#endif
// held frames: the tested frame's upload is dropped (a new test); HeldDrop also the last distinct frame (a seek, the end)
static void Unstage(pm_engine *e)
{
#ifdef PMVS_GPU
    if (e->staged >= 0 && e->g) e->g->ReleaseSlot(e->staged);
#endif
    e->staged = -1;
    e->lastDup = e->checked = false;
}
static void HeldDrop(pm_engine *e)
{
    Unstage(e);
    e->haveDistinct = false;
}

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

static void LagDrop(pm_engine *e);

extern "C" void pm_engine_close(pm_engine *e)
{
#ifdef PM_APPLE_MODULE
    if (e) AppleClose(e);
#endif
    if (e) { LagDrop(e); HeldDrop(e); }
#ifdef PMVS_GPU
    if (e && e->gpu) GpuForget(e);
#endif
    delete e;
}

static int LagPush(pm_engine *e, const pm_yuv *src, int contiguous, char *err, size_t errsize);
static int LagCheckYuv(pm_engine *e, const pm_yuv *src, int contiguous, char *err, size_t errsize);
#ifdef PM_DMABUF
static int LagCheckDmabuf(pm_engine *e, const pm_dmabuf *src, int contiguous, char *err, size_t errsize);
#endif

// the frame into the running engine now (the held-frame test, if any, is done)
static int PushNow(pm_engine *e, const pm_yuv *src, int contiguous, char *err, size_t errsize)
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
        int slot = e->staged;   // held frames: pm_engine_check_dup's upload of this frame
        e->staged = -1;
        if (slot < 0) slot = e->g->Upload(v, why);
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

extern "C" int pm_engine_push(pm_engine *e, const pm_yuv *src, int contiguous, char *err, size_t errsize)
{
    if (e->lag) return LagPush(e, src, contiguous, err, errsize);
#ifdef PM_APPLE_MODULE
    if (e->ap) return PushNow(e, src, contiguous, err, errsize);   // the apple engine takes what the module pushes
#endif
    if (e->dedup) {   // held frames: a repeat of the last distinct frame leaves the pair as it is
        if (!e->checked && pm_engine_check_dup(e, src, contiguous, err, errsize) == 1) return 0;
        e->checked = false;
        std::swap(e->lastQ, e->curQ);
        e->haveDistinct = true;
    }
    return PushNow(e, src, contiguous, err, errsize);
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
    LagDrop(e);   // lagged: the pictures waiting and the repair's frames go too
#ifdef PM_APPLE_MODULE
    if (e->ap) { e->ap->Flush(); e->apPair = false; return; }
#endif
    HeldDrop(e);
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

extern "C" void pm_engine_set_fullscreen(pm_engine *e, int fullscreen) { if (e) e->fullscreen = fullscreen; }

extern "C" int pm_engine_decide(pm_engine *e, int smoother, double src_fps, double display_hz, char *why, size_t whysize)
{
    if (src_fps > 1) e->srcMs = 1000.0 / src_fps;   // the repair's budget: half a source frame

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
    const pmvs::PortableK k = pmvs::DecidePortableK(smoother, src_fps, display_hz, e->height, me, gen, e->gpu, e->fullscreen == 1 ? 0.0 : -1.0, pmvs::VrrActive(e->fullscreen));
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
// a dmabuf into a free source slot (the import lives until the upload has run); -2: the import failed
static int DbUpload(pm_engine *e, const pm_dmabuf *src, std::string &why)
{
    pmdb::Imported im;
    if (!e->db->Import(*src, false, im, why)) return -2;
    uint64_t ticket = 0;
    const int slot = e->g->UploadWith([&](VkCommandBuffer cb, const pmvs::gpu::Engine::PlanarDesc &d, std::string &er) {
        return e->db->RecordIn(cb, im, d, er); }, ticket, why);
    std::string werr;
    if (ticket) e->g->Wait(ticket, werr);
    e->db->Recycle();
    e->db->Release(im);
    return slot;
}
#endif

extern "C" int pm_engine_tap_ok(pm_engine *e)
{
#ifdef PM_DMABUF
    return e && e->gpu && e->g && e->g->TapAvailable() && DbReady(e) ? 1 : 0;
#else
    (void)e;
    return 0;
#endif
}

extern "C" int pm_engine_tap_dmabuf(pm_engine *e, const pm_dmabuf *src, int clip, uint32_t *words, size_t cap, int *cw, int *ch, char *err, size_t errsize)
{
#ifdef PM_DMABUF
    if (!pm_engine_tap_ok(e)) { SetErr(err, errsize, "no GPU tap here"); return -1; }
    std::string why;
    const int slot = DbUpload(e, src, why);   // its own slot: the pair's slots and the repair's stay as they are
    if (slot < 0) { SetErr(err, errsize, why); return -1; }
    std::vector<uint32_t> w;
    int x = 0, y = 0;
    const bool ok = e->g->Tap(slot, clip != 0, w, x, y, why);
    e->g->ReleaseSlot(slot);
    if (!ok) { SetErr(err, errsize, why); return -1; }
    if (w.size() > cap) { SetErr(err, errsize, "the tap's buffer is too small"); return -1; }
    std::memcpy(words, w.data(), w.size() * 4);
    *cw = x; *ch = y;
    return int(w.size());
#else
    (void)e; (void)src; (void)clip; (void)words; (void)cap; (void)cw; (void)ch;
    SetErr(err, errsize, "this build has no zero-copy path");
    return -1;
#endif
}

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
    if (e->lag) {   // lagged: the check took the frame in; the pair comes with pm_engine_next_pair
        if (!e->checked) {
            Unstage(e);
            const int r = LagCheckDmabuf(e, src, contiguous, err, errsize);
            if (r != 0) return r == 1 ? 0 : r;
        }
        e->checked = false;
        return 0;
    }
    std::string why;
    // held frames: a repeat of the last distinct frame leaves the pair as it is
    if (e->dedup && !e->checked && pm_engine_check_dup_dmabuf(e, src, contiguous, err, errsize) == 1) return 0;
    int slot = e->staged;   // pm_engine_check_dup*'s upload of this frame
    e->staged = -1;
    if (slot < 0) {
        slot = DbUpload(e, src, why);
        if (slot == -2) { SetErr(err, errsize, why); return -1; }   // not imported: the caller copies (the test stands)
        if (slot < 0) { SetErr(err, errsize, why); GpuForget(e); return -1; }
    }
    if (e->dedup) { e->checked = false; e->haveDistinct = true; }
    if (!contiguous) GpuForget(e);
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

// ---- held frames (pm_engine.h): PushFPS's duplicate test against the last distinct frame pushed
extern "C" void pm_engine_set_dedup(pm_engine *e, int on)
{
    HeldDrop(e);
    e->dedup = on != 0;
}

#ifdef PMVS_GPU
// the GPU engine: the uploaded frame against slotCur (the last distinct frame) with the engine's Duplicate pass; a new
// picture stays uploaded for its push, a repeat's slot is freed. A failed test counts as a new picture.
static int GpuStage(pm_engine *e, int slot, int contiguous, char *err, size_t errsize)
{
    bool dup = false;
    if (contiguous && e->slotCur >= 0) {
        pmvs::gpu::DedupStats ds;
        std::string why;
        if (!e->g->Duplicate(e->slotCur, slot, ds, why)) { e->staged = slot; e->checked = true; SetErr(err, errsize, why); return -1; }
        dup = ds.duplicate;
    }
    if (dup) e->g->ReleaseSlot(slot);
    else e->staged = slot;
    e->lastDup = dup;
    e->checked = !dup;
    return dup ? 1 : 0;
}
#endif

extern "C" int pm_engine_check_dup(pm_engine *e, const pm_yuv *src, int contiguous, char *err, size_t errsize)
{
    Unstage(e);   // only the last distinct frame stays (CPU: lastQ; GPU: slotCur)
    if (e->lag) return LagCheckYuv(e, src, contiguous, err, errsize);
    if (!e->dedup) return 0;
#ifdef PMVS_GPU
    if (e->gpu) {
        pmvs::portable::YuvView v;
        v.width = e->width; v.height = e->height; v.bits = e->bits;
        for (int i = 0; i < 3; ++i) { v.data[i] = src->data[i]; v.stride[i] = src->stride[i]; }
        std::string why;
        const int slot = e->g->Upload(v, why);
        if (slot < 0) { SetErr(err, errsize, why); return -1; }
        return GpuStage(e, slot, contiguous, err, errsize);
    }
#endif
    (void)err; (void)errsize;
    pmvs::held::QuarterLuma(src->data[0], src->stride[0], e->width, e->height, e->bits > 8 ? 2 : 1, e->bits > 8 ? e->bits - 8 : 0, e->curQ);
    e->lastDup = contiguous && e->haveDistinct && pmvs::held::QuarterDuplicate(e->lastQ, e->curQ, e->width, e->height);
    e->checked = !e->lastDup;   // a new picture: the next push takes this test
    return e->lastDup ? 1 : 0;
}

extern "C" int pm_engine_check_dup_dmabuf(pm_engine *e, const pm_dmabuf *src, int contiguous, char *err, size_t errsize)
{
#ifdef PM_DMABUF
    Unstage(e);
    if (e->lag) return LagCheckDmabuf(e, src, contiguous, err, errsize);
    if (!e->dedup) return 0;
    if (!DbReady(e)) { SetErr(err, errsize, e->dbWhy); return -1; }
    std::string why;
    const int slot = DbUpload(e, src, why);
    if (slot < 0) { SetErr(err, errsize, why); return -1; }
    return GpuStage(e, slot, contiguous, err, errsize);
#else
    (void)e; (void)src; (void)contiguous;
    SetErr(err, errsize, "no zero-copy path in this build");
    return -1;
#endif
}

extern "C" int pm_engine_last_dup(const pm_engine *e) { return e->lastDup ? 1 : 0; }

// ---- the held-drawing repair (engine_portable.h): the scheduling of the other portable routes (GpuBackend for
// VapourSynth, the mpv Vulkan engine) in the VLC module's push / generate interface. Each source frame's cell grid
// goes to the schedule at its check; kLag frames later its held parts are laid in, and only then does its picture
// advance the engine (pm_engine_next_pair), so every pair is made from repaired pictures.
#ifdef PM_HELD_REPAIR
static constexpr int kLag = pushmpc::held::kLookahead;
#else
static constexpr int kLag = 3;
#endif

// still needed: the advanced pair's pictures, those waiting, the last distinct one (the next duplicate test), and the
// frames a repair can still read (a part's pair starts up to kLag + 1 behind the frame repaired)
static bool LagKeeps(const pm_engine *e, int64_t f)
{
    if (f == e->lagA || f == e->lagB || f > e->lagB || f == e->lagLastDistinct) return true;
    return e->repOn && f >= e->lagSrc - 2 * int64_t(kLag) - 2;
}
static void LagPrune(pm_engine *e)
{
#ifdef PMVS_GPU
    for (auto it = e->lagSlot.begin(); it != e->lagSlot.end();) {
        if (LagKeeps(e, it->first)) { ++it; continue; }
        e->g->ReleaseSlot(it->second);
        it = e->lagSlot.erase(it);
    }
#endif
#ifdef PM_HELD_REPAIR
    for (auto it = e->lagPic.begin(); it != e->lagPic.end();) it = LagKeeps(e, it->first) ? std::next(it) : e->lagPic.erase(it);
#endif
    while (!e->lagRepaired.empty() && *e->lagRepaired.begin() < e->lagA) e->lagRepaired.erase(e->lagRepaired.begin());
}
static void LagDrop(pm_engine *e)
{
    if (!e->lag) return;
#ifdef PMVS_GPU
    if (e->lagGpu && e->g) {
        if (e->pair >= 0) e->g->ReleasePair(e->pair);
        e->pair = e->slotPrev = e->slotCur = -1;
        e->cut = false;
        for (auto &kv : e->lagSlot) e->g->ReleaseSlot(kv.second);
    }
    e->lagSlot.clear();
    e->gpuRep.Reset();
#endif
#ifdef PM_HELD_REPAIR
    e->sched.Reset();
    e->lagPic.clear();
#endif
    e->lagWait.clear();
    e->lagRepaired.clear();
    e->repPending.clear();
    e->lagSrc = e->lagLastDistinct = e->lagReady = e->lagA = e->lagB = -1;
    e->haveDistinct = e->lastDup = e->checked = false;
}
// a gap: every picture before it may advance (unrepaired where the gap cut its lookahead); the schedule starts over
static void LagGap(pm_engine *e)
{
    e->lagReady = std::max(e->lagReady, e->lagSrc);
#ifdef PMVS_GPU
    e->gpuRep.Reset();
#endif
#ifdef PM_HELD_REPAIR
    e->sched.Reset();
#endif
}
// after a check of frame s: the frame kLag behind may advance (its repair, if any, is done)
static int LagChecked(pm_engine *e, bool dup)
{
    e->lagReady = std::max(e->lagReady, e->lagSrc - kLag);
    e->lastDup = dup;
    e->checked = !dup;
    LagPrune(e);
    return dup ? 1 : 0;
}
static pmvs::portable::YuvView ViewOf(const pm_engine *e, const pm_yuv &y)
{
    pmvs::portable::YuvView v;
    v.width = e->width; v.height = e->height; v.bits = e->bits;
    for (int i = 0; i < 3; ++i) { v.data[i] = y.data[i]; v.stride[i] = y.stride[i]; }
    return v;
}

#ifdef PMVS_GPU
// the GPU engine's picture in a slot into CPU planes (a repaired real frame)
static bool GpuReadSlot(pm_engine *e, int slot, const pm_yuv_out *out, std::string &why)
{
    int o = -1; uint64_t ticket = 0;
    if (!e->g->Real(slot, o, ticket, why) || !e->g->Wait(ticket, why)) { if (o >= 0) e->g->ReleaseOut(o); return false; }
    pmvs::portable::YuvOut y;
    for (int i = 0; i < 3; ++i) { y.data[i] = out->data[i]; y.stride[i] = out->stride[i]; }
    e->g->Read(o, y);   // frees the readback slot
    return true;
}
// lagged on the GPU: the uploaded frame against the last distinct one (Dedup), its cell grid to the schedule (before a
// repeat's slot is freed), a new picture kept in its slot, then frame s - kLag repaired in its own slot
static int LagStageGpu(pm_engine *e, int slot, int contiguous, char *err, size_t errsize)
{
    const int64_t s = e->lagSrc + 1;
    const bool contig = contiguous && e->lagLastDistinct >= 0;
    if (!contig) LagGap(e);
    auto last = e->lagSlot.find(e->lagLastDistinct);
    const int prevSlot = contig && last != e->lagSlot.end() ? last->second : -1;
    std::string why;
    bool dup = false;
    if (e->dedup && prevSlot >= 0) {
        pmvs::gpu::DedupStats ds;
        if (!e->g->Duplicate(prevSlot, slot, ds, why)) { e->g->ReleaseSlot(slot); SetErr(err, errsize, why); return -1; }
        dup = ds.duplicate;
    }
    if (!e->gpuRep.Classify(*e->g, s, slot, prevSlot, dup, why)) {   // the repair stops; the pictures go on
        e->repFailed++;
        e->gpuRep.on = false;
        e->gpuRep.Reset();
    }
    e->lagSrc = s;
    if (dup) e->g->ReleaseSlot(slot);
    else { e->lagSlot[s] = slot; e->lagWait.push_back({ s, contig }); e->lagLastDistinct = s; }
    const int64_t q = s - kLag;
    if (q > e->lagB) {   // never a picture already handed out
        if (!e->gpuRep.RepairUpTo(*e->g, q, e->lagSlot, e->srcMs, [](int64_t) {}, why)) e->repFailed++;
        if (e->gpuRep.Repaired(q)) e->lagRepaired.insert(q);
    }
    return LagChecked(e, dup);
}
// lagged on the GPU: the picture advanced into the pair (its motion from the previous one), as a push did
static int GpuAdvance(pm_engine *e, const pm_engine::LagItem &it, char *err, size_t errsize)
{
    auto sl = e->lagSlot.find(it.s);
    if (sl == e->lagSlot.end()) { SetErr(err, errsize, "repair: a waiting picture is gone"); return -1; }
#ifdef PM_APPLE_MODULE
    if (e->ap) {   // the apple engine came up after the repair was set: the picture read back for it
        const size_t bps = e->bits > 8 ? 2 : 1, cw = size_t((e->width + 1) / 2), ch = size_t((e->height + 1) / 2);
        e->natOut[0].resize(size_t(e->width) * bps * size_t(e->height)); e->natOut[1].resize(cw * bps * ch); e->natOut[2].resize(cw * bps * ch);
        pm_yuv_out o{};
        for (int i = 0; i < 3; ++i) { o.data[i] = e->natOut[i].data(); o.stride[i] = ptrdiff_t(i ? cw * bps : size_t(e->width) * bps); }
        std::string why;
        if (!GpuReadSlot(e, sl->second, &o, why)) { SetErr(err, errsize, why); return -1; }
        pm_yuv v{};
        for (int i = 0; i < 3; ++i) { v.data[i] = o.data[i]; v.stride[i] = o.stride[i]; }
        return PushNow(e, &v, it.contig ? 1 : 0, err, errsize);
    }
#endif
    if (!it.contig) GpuForget(e);   // the pair restarts (the slots stay in lagSlot)
    e->st.pushed++;
    const int slot = sl->second;
    if (e->slotCur < 0) { e->slotCur = slot; return 0; }
    pmvs::gpu::PairStats ps;
    std::string why;
    const int pair = e->g->Estimate(e->slotCur, slot, e->pair, ps, why);
    if (e->pair >= 0) e->g->ReleasePair(e->pair);   // the pair before (its fields were the temporal predictors)
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

#ifdef PM_HELD_REPAIR
using P16 = pm_engine::P16;
// a 4:2:0 picture as 16-bit codes (8 bits widened, 9..16 bits as they are)
static void To16(const uint8_t *const data[3], const ptrdiff_t stride[3], int w, int h, int bits, P16 &o)
{
    o.w = w; o.h = h; o.cw = (w + 1) / 2; o.ch = (h + 1) / 2;
    for (int i = 0; i < 3; ++i) {
        const int pw = i ? o.cw : w, ph = i ? o.ch : h;
        o.p[i].resize(size_t(pw) * size_t(ph));
        for (int y = 0; y < ph; ++y) {
            const uint8_t *row = data[i] + ptrdiff_t(y) * stride[i];
            uint16_t *d = o.p[i].data() + size_t(y) * size_t(pw);
            if (bits > 8) std::memcpy(d, row, size_t(pw) * 2);
            else for (int x = 0; x < pw; ++x) d[x] = row[x];
        }
    }
}
// the engine's own format of a 16-bit picture (8 bits: narrowed into buf)
static pm_yuv NativeOf(const P16 &p, int bits, std::vector<uint8_t> (&buf)[3])
{
    pm_yuv v{};
    for (int i = 0; i < 3; ++i) {
        const int pw = i ? p.cw : p.w;
        if (bits > 8) { v.data[i] = reinterpret_cast<const uint8_t *>(p.p[i].data()); v.stride[i] = ptrdiff_t(pw) * 2; continue; }
        buf[i].resize(p.p[i].size());
        for (size_t k = 0; k < p.p[i].size(); ++k) buf[i][k] = uint8_t(p.p[i][k]);
        v.data[i] = buf[i].data(); v.stride[i] = pw;
    }
    return v;
}
static void WriteOut(const P16 &p, int bits, const pm_yuv_out *out)
{
    for (int i = 0; i < 3; ++i) {
        const int pw = i ? p.cw : p.w, ph = i ? p.ch : p.h;
        for (int y = 0; y < ph; ++y) {
            const uint16_t *s = p.p[i].data() + size_t(y) * size_t(pw);
            uint8_t *d = out->data[i] + ptrdiff_t(y) * out->stride[i];
            if (bits > 8) std::memcpy(d, s, size_t(pw) * 2);
            else for (int x = 0; x < pw; ++x) d[x] = uint8_t(s[x]);
        }
    }
}
// frame q's held parts on the CPU: each pair's picture (the part engine, at the cell-weighted mean phase of the parts
// that share the pair, as the ml route without a phase map) laid into the frame's 16-bit copy through the parts' masks
static void CpuRepair(pm_engine *e, int64_t q)
{
    using namespace pushmpc::held;
    // planned for a duplicate too (as the ml route): a wholly held frame is the animation gate's evidence
    const auto groups = e->sched.Plan(q, 0.5 * e->srcMs);
    auto dst = e->lagPic.find(q);
    if (dst == e->lagPic.end()) return;   // a duplicate: held frames re-time it whole
    const CellGrid *g = e->sched.Grid(q);
    if (groups.empty() || !g) return;
    if (!e->partReady) {
        pmvs::portable::Config c;
        c.width = e->width; c.height = e->height; c.bits = e->bits; c.threads = e->threads; c.quality = e->quality;
        std::string why;
        if (!e->part.Configure(c, why)) { e->repFailed++; e->repOn = false; return; }
        e->partReady = true;
    }
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::pair<uint64_t, uint64_t>> pairs;   // at most 3, as the GPU engine's parts
    for (const auto &gr : groups) {
        const auto key = std::make_pair(gr.pairFrom, gr.d1);
        if (pairs.size() < 3 && std::find(pairs.begin(), pairs.end(), key) == pairs.end()) pairs.push_back(key);
    }
    const size_t bps = e->bits > 8 ? 2 : 1, cw = size_t((e->width + 1) / 2), ch = size_t((e->height + 1) / 2);
    size_t parts = 0;
    for (const auto &pr : pairs) {
        auto a = e->lagPic.find(int64_t(pr.first)), b = e->lagPic.find(int64_t(pr.second));
        if (a == e->lagPic.end() || b == e->lagPic.end()) continue;
        std::vector<float> tmap, alpha;
        if (!PairPhaseMap(groups, pr.first, pr.second, tmap, alpha)) continue;
        double tw = 0, wn = 0;
        size_t n = 0;
        for (const auto &gr : groups) if (gr.pairFrom == pr.first && gr.d1 == pr.second) { tw += double(gr.t) * gr.cells; wn += gr.cells; ++n; }
        if (wn <= 0) continue;
        std::string why;
        const pm_yuv va = NativeOf(a->second, e->bits, e->natA), vb = NativeOf(b->second, e->bits, e->natB);
        e->part.Forget();
        if (!e->part.Push(ViewOf(e, va), false, why) || !e->part.Push(ViewOf(e, vb), true, why)) { e->repFailed++; continue; }
        if (!e->part.HavePair() || e->part.PairIsCut()) continue;   // a cut between the drawings: no part
        e->natOut[0].resize(size_t(e->width) * bps * size_t(e->height)); e->natOut[1].resize(cw * bps * ch); e->natOut[2].resize(cw * bps * ch);
        pmvs::portable::YuvOut o;
        for (int i = 0; i < 3; ++i) { o.data[i] = e->natOut[i].data(); o.stride[i] = ptrdiff_t(i ? cw * bps : size_t(e->width) * bps); }
        e->part.Generate(float(tw / wn), o);
        const uint8_t *od[3] = { e->natOut[0].data(), e->natOut[1].data(), e->natOut[2].data() };
        To16(od, o.stride, e->width, e->height, e->bits, e->rep);
        P16 &d = dst->second;
        uint16_t *const dp[3] = { d.p[0].data(), d.p[1].data(), d.p[2].data() };
        const uint16_t *const sp[3] = { e->rep.p[0].data(), e->rep.p[1].data(), e->rep.p[2].data() };
        const ptrdiff_t ds[3] = { d.w, d.cw, d.cw };
        CompositePlanes(dp, ds, sp, ds, d.w, d.h, alpha, int(g->w), int(g->h), int(g->cell));
        parts += n;
    }
    if (!parts) return;
    e->sched.Cost(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), parts);
    e->lagRepaired.insert(q);
}
// lagged on the CPU (the CPU engine, the apple engine, or a GPU engine without the repair's shaders): the duplicate
// test on the quarter-size luma, a 16-bit copy of the frame, its cell grid to the schedule, then frame s - kLag repaired
static int LagStageCpu(pm_engine *e, const pm_yuv *src, int contiguous)
{
    const int64_t s = e->lagSrc + 1;
    const bool contig = contiguous && e->lagLastDistinct >= 0;
    if (!contig) LagGap(e);
    bool dup = false;
    if (e->dedup) {
        pmvs::held::QuarterLuma(src->data[0], src->stride[0], e->width, e->height, e->bits > 8 ? 2 : 1, e->bits > 8 ? e->bits - 8 : 0, e->curQ);
        dup = contig && e->haveDistinct && pmvs::held::QuarterDuplicate(e->lastQ, e->curQ, e->width, e->height);
        if (!dup) { std::swap(e->lastQ, e->curQ); e->haveDistinct = true; }
    }
    P16 cur;
    if (!dup || e->repOn) To16(src->data, src->stride, e->width, e->height, e->bits, cur);
    if (e->repOn) {
        auto last = e->lagPic.find(e->lagLastDistinct);
        const uint16_t *prevY = contig && last != e->lagPic.end() ? last->second.p[0].data() : nullptr;
        e->sched.Push(s, pushmpc::held::CellsFromPlanes(cur.p[0].data(), cur.w, cur.p[1].data(), cur.p[2].data(), cur.cw, prevY,
                                                        cur.w, cur.h, e->bits), dup);
    }
    e->lagSrc = s;
    if (!dup) { e->lagPic[s] = std::move(cur); e->lagWait.push_back({ s, contig }); e->lagLastDistinct = s; }
    const int64_t q = s - kLag;
    if (e->repOn && q > e->lagB) {   // never a picture already handed out
        if (e->repDefer) e->repPending.push_back(q);   // the worker runs it (pm_engine_run_deferred) before the pair advances
        else CpuRepair(e, q);
    }
    return LagChecked(e, dup);
}
#endif

static int LagCheckYuv(pm_engine *e, const pm_yuv *src, int contiguous, char *err, size_t errsize)
{
#ifdef PMVS_GPU
    if (e->lagGpu) {
        std::string why;
        const int slot = e->g->Upload(ViewOf(e, *src), why);
        if (slot < 0) { SetErr(err, errsize, why); return -1; }
        return LagStageGpu(e, slot, contiguous, err, errsize);
    }
#endif
#ifdef PM_HELD_REPAIR
    (void)err; (void)errsize;
    return LagStageCpu(e, src, contiguous);
#else
    (void)e; (void)src; (void)contiguous;
    SetErr(err, errsize, "this build has no held-drawing repair");
    return -1;
#endif
}

#ifdef PM_DMABUF
static int LagCheckDmabuf(pm_engine *e, const pm_dmabuf *src, int contiguous, char *err, size_t errsize)
{
    if (!e->lagGpu) { SetErr(err, errsize, "the repair runs on the CPU: the frame is copied"); return -1; }
    if (!DbReady(e)) { SetErr(err, errsize, e->dbWhy); return -1; }
    std::string why;
    const int slot = DbUpload(e, src, why);
    if (slot < 0) { SetErr(err, errsize, why); return -1; }
    return LagStageGpu(e, slot, contiguous, err, errsize);
}
#endif

static int LagPush(pm_engine *e, const pm_yuv *src, int contiguous, char *err, size_t errsize)
{
    if (!e->checked) {   // no check came first: it runs here (a repeat is consumed)
        Unstage(e);
        const int r = LagCheckYuv(e, src, contiguous, err, errsize);
        if (r != 0) return r == 1 ? 0 : r;
    }
    e->checked = false;
    return 0;
}

extern "C" int pm_engine_set_repair(pm_engine *e, int on, char *why, size_t whysize)
{
#ifndef PM_HELD_REPAIR
    (void)e; (void)on;
    SetErr(why, whysize, "off (this build has no held-drawing repair)");
    return 0;
#else
    if (!on) {
        e->repOn = false;
#ifdef PMVS_GPU
        e->gpuRep.on = false;
#endif
        SetErr(why, whysize, "off");
        return 0;
    }
    if (const char *v = std::getenv("PUSHMPC_REGION_REPAIR"); v && std::atoi(v) == 0) { SetErr(why, whysize, "off (PUSHMPC_REGION_REPAIR=0)"); return 0; }
    if (!e->dedup) { SetErr(why, whysize, "off (it needs held frames)"); return 0; }
    std::string note;
    if (!e->lag) {
        pm_engine_forget(e);   // nothing pushed so far counts: the lag starts clean
#ifdef PMVS_GPU
        bool apple = false;
#ifdef PM_APPLE_MODULE
        apple = e->ap != nullptr;
#endif
        if (e->gpu && !apple) {
#ifdef PMVS_REGION_REPAIR
            if (!e->g->RepairAvailable()) {   // the GPU engine again, with the repair's storage slots
                const pm_engine_cfg c{ e->width, e->height, e->bits, e->threads, e->quality, e->engineReq };
                std::string w2;
#ifdef PM_DMABUF
                e->db.reset();
                e->dbTried = false;
#endif
                if (!OpenGpu(e, &c, w2, true)) note = "; the GPU engine could not take the repair's slots (" + w2 + ")";
            }
            e->lagGpu = e->g->RepairAvailable();
            if (!e->lagGpu && note.empty()) note = "; this GPU engine build has no repair shaders";
#endif
        }
#endif
        e->lag = kLag;
    }
    e->repOn = true;
#ifdef PMVS_GPU
    e->gpuRep.on = e->lagGpu;
#endif
    SetErr(why, whysize, std::string("on (") + (e->lagGpu ? "in the GPU engine" : "on the CPU") + ", pairs " + std::to_string(e->lag) +
                         " source frames behind)" + note);
    return 1;
#endif
}

extern "C" int pm_engine_repair_lag(const pm_engine *e) { return e->lag; }

extern "C" void pm_engine_defer_repair(pm_engine *e, int on) { e->repDefer = on != 0; }

extern "C" void pm_engine_run_deferred(pm_engine *e)
{
#ifdef PM_HELD_REPAIR
    while (!e->repPending.empty()) {
        const int64_t q = e->repPending.front();
        e->repPending.pop_front();
        if (e->repOn && q > e->lagB) CpuRepair(e, q);
    }
#else
    e->repPending.clear();
#endif
}

extern "C" void pm_engine_plain(pm_engine *e, int on)
{
    if (on) {
        if (e->plainSaved) return;
        LagDrop(e);
        HeldDrop(e);
        e->plainSaved = true; e->plainLag = e->lag; e->plainDedup = e->dedup;
        e->lag = 0; e->dedup = false;
        return;
    }
    if (!e->plainSaved) return;
    pm_engine_forget(e);   // the probe's frames go (lag 0 here: only the pair)
    e->plainSaved = false;
    e->lag = e->plainLag; e->dedup = e->plainDedup;
    LagDrop(e);
    HeldDrop(e);
}

extern "C" int pm_engine_pairs_ready(const pm_engine *e)
{
    int n = 0;
    if (e->lag)
        for (const auto &it : e->lagWait) { if (it.s > e->lagReady) break; ++n; }
    return n;
}

extern "C" int pm_engine_next_pair(pm_engine *e, char *err, size_t errsize)
{
    if (!e->lag || e->lagWait.empty() || e->lagWait.front().s > e->lagReady) return 0;
    pm_engine_run_deferred(e);   // a frame's repair before its pair advances
    const pm_engine::LagItem it = e->lagWait.front();
    e->lagWait.pop_front();
    e->lagA = e->lagB;
    e->lagB = it.s;
    int r = -1;
#ifdef PMVS_GPU
    if (e->lagGpu) r = GpuAdvance(e, it, err, errsize);
    else
#endif
    {
#ifdef PM_HELD_REPAIR
        auto p = e->lagPic.find(it.s);
        if (p == e->lagPic.end()) SetErr(err, errsize, "repair: a waiting picture is gone");
        else {
            const pm_yuv v = NativeOf(p->second, e->bits, e->natA);
            r = PushNow(e, &v, it.contig ? 1 : 0, err, errsize);
        }
#else
        SetErr(err, errsize, "this build has no held-drawing repair");
#endif
    }
    LagPrune(e);
    return r == 0 ? 1 : -1;
}

extern "C" void pm_engine_drain(pm_engine *e)
{
    if (e->lag) e->lagReady = std::max(e->lagReady, e->lagSrc);
}

extern "C" int pm_engine_repair_real(pm_engine *e, int which, const pm_yuv_out *out)
{
    const int64_t f = which ? e->lagB : e->lagA;
    if (!e->lag || f < 0 || !e->lagRepaired.count(f)) return 1;
#ifdef PMVS_GPU
    if (e->lagGpu) {
        auto sl = e->lagSlot.find(f);
        if (sl == e->lagSlot.end()) return 1;
        std::string why;
        return GpuReadSlot(e, sl->second, out, why) ? 0 : -1;
    }
#endif
#ifdef PM_HELD_REPAIR
    auto p = e->lagPic.find(f);
    if (p == e->lagPic.end()) return 1;
    WriteOut(p->second, e->bits, out);
    return 0;
#else
    (void)out;
    return 1;
#endif
}

extern "C" int pm_engine_repair_real_dmabuf(pm_engine *e, int which, const pm_dmabuf *dst, char *err, size_t errsize)
{
#ifdef PM_DMABUF
    const int64_t f = which ? e->lagB : e->lagA;
    if (!e->lag || !e->lagGpu || f < 0 || !e->lagRepaired.count(f)) return 1;
    auto sl = e->lagSlot.find(f);
    // the pair's real frame A (mode 1) or B (mode 2) straight into the surface
    if (sl == e->lagSlot.end() || e->pair < 0 || (which ? e->slotCur : e->slotPrev) != sl->second) return 1;
    if (!DbReady(e)) { SetErr(err, errsize, e->dbWhy); return -1; }
    std::string why;
    pmdb::Imported im;
    if (!e->db->Import(*dst, true, im, why)) { SetErr(err, errsize, why); return -1; }
    int out = -1;
    uint64_t ticket = 0;
    const bool ok = e->g->GenerateWith(e->pair, which ? 1.f : 0.f, which ? 2 : 1, [&](VkCommandBuffer cb, const pmvs::gpu::Engine::PackedDesc &d, std::string &er) {
        return e->db->RecordOut(cb, im, d, er); }, out, ticket, why);
    std::string werr;
    const bool waited = ticket && e->g->Wait(ticket, werr);
    if (out >= 0) e->g->ReleaseOut(out);
    e->db->Recycle();
    e->db->Release(im);
    if (!ok || !waited) { SetErr(err, errsize, ok ? werr : why); return -1; }
    return 0;
#else
    (void)e; (void)which; (void)dst;
    SetErr(err, errsize, "no zero-copy path in this build");
    return -1;
#endif
}

extern "C" void pm_engine_repair_stats_get(const pm_engine *e, pm_repair_stats *out)
{
    *out = pm_repair_stats{};
    out->lag = e->lag; out->on = e->repOn ? 1 : 0; out->gpu = e->lagGpu ? 1 : 0; out->failed = e->repFailed;
#ifdef PMVS_GPU
    if (e->lagGpu) {
        const auto r = e->gpuRep.Stats();
        out->frames = r.frames; out->parts = r.parts; out->gated = r.gated; out->over_budget = r.overBudget; out->ms = r.ms;
        return;
    }
#endif
#ifdef PM_HELD_REPAIR
    const auto &r = e->sched.stats;
    out->frames = r.frames; out->parts = r.parts; out->gated = r.gated; out->over_budget = r.overBudget; out->ms = r.ms;
#endif
}
