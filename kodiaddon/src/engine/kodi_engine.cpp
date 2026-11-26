// kodi_engine: see kodi_engine.h.
#include "kodi_engine.h"
#include "backend.h"   // pmvs::DecidePortableK: Auto's rule, shared with the mpv route
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <map>
#include <tuple>

#ifdef PMVS_GPU
#include "gpu_backend.h"
#endif
#ifdef PMK_PMML
#include "anime_sr_engine.h"
#endif

namespace pmk {

// ------------------------------------------------------------------------------------------------- the CPU engine
class CpuEngine : public IEngine {
public:
    explicit CpuEngine(const EngineCfg& c) : m_cfg(c) {}
    ~CpuEngine() override { if (m_e) pm_engine_close(m_e); }
    bool Open(std::string& why)
    {
        pm_engine_cfg c{};
        c.width = m_cfg.width; c.height = m_cfg.height; c.bits = m_cfg.bits; c.threads = m_cfg.threads; c.quality = m_cfg.quality;
        char err[256] = {};
        m_e = pm_engine_open(&c, err, sizeof(err));
        if (!m_e) why = err;
        return m_e != nullptr;
    }
    std::string Describe() const override { return "CPU (PushFPS portable core, " + std::to_string(pm_engine_threads(m_e)) + " threads)"; }
    bool IsGpu() const override { return false; }
    int OutWidth() const override { return m_cfg.width; }
    int OutHeight() const override { return m_cfg.height; }
    bool Push(const pm_yuv& src, bool contiguous, std::string& err) override
    {
        char e[256] = {};
        if (pm_engine_push(m_e, &src, contiguous ? 1 : 0, e, sizeof(e)) != 0) { err = e; return false; }
        return true;
    }
    bool HavePair() const override { return pm_engine_have_pair(m_e) != 0; }
    bool PairIsCut() const override { return pm_engine_pair_is_cut(m_e) != 0; }
    bool Generate(float t, const pm_yuv_out& out, std::string&) override { pm_engine_generate(m_e, t, &out); return true; }
    bool Real(bool, const pm_yuv_out&, std::string&) override { return false; }
    void Forget() override { pm_engine_forget(m_e); }
    void SetDedup(bool on) override { pm_engine_set_dedup(m_e, on ? 1 : 0); }
    bool LastDup() const override { return pm_engine_last_dup(m_e) != 0; }
    EngineStats Stats() const override
    {
        pm_engine_stats s{};
        pm_engine_stats_get(m_e, &s);
        return EngineStats{s.pushed, s.pairs, s.cuts, s.generated, s.me_ms, s.gen_ms};
    }
    int MaxK() const override { return kPortableEngineMaxK; }
    int DecideK(int smoother, double srcFps, double displayHz, std::string& why) override
    {
        char b[512] = {};
        const int k = pm_engine_decide_k(smoother, srcFps, displayHz, m_cfg.width, m_cfg.height, m_cfg.bits, m_cfg.threads, m_cfg.quality, b, sizeof(b));
        why = b;
        return k;
    }

private:
    EngineCfg m_cfg;
    pm_engine* m_e = nullptr;
};

#ifdef PMVS_GPU
// ------------------------------------------------------------------------------------------------- the GPU engine
class GpuEngine : public IEngine {
public:
    explicit GpuEngine(const EngineCfg& c) : m_cfg(c) {}
    ~GpuEngine() override { ReleaseAll(); }
    bool Open(std::string& why)
    {
        pmvs::gpu::DeviceRequest req;
        if (const char* e = std::getenv("PUSHMPC_VK_ALLOW_CPU")) req.allowCpu = e[0] == '1';
        if (const char* d = std::getenv("PUSHMPC_VK_DEVICE")) req.device = d;
        pmvs::gpu::Config c;
        c.width = m_cfg.width; c.height = m_cfg.height; c.bits = m_cfg.bits;
        c.quality = m_cfg.quality <= 0 ? 1 : 2;
        c.upscaler = m_cfg.upscaler == 1 ? pmvs::gpu::Upscaler::Nis : m_cfg.upscaler == 2 ? pmvs::gpu::Upscaler::Fsr : pmvs::gpu::Upscaler::None;
        if (c.upscaler != pmvs::gpu::Upscaler::None) { c.outWidth = m_cfg.outWidth; c.outHeight = m_cfg.outHeight; }
        c.sharpness = c.upscaler == pmvs::gpu::Upscaler::Nis ? 0.1f : 0.5f;   // the mpv route's defaults
        c.sourceSlots = 6; c.outSlots = 4;
        c.semiPlanarOut = m_cfg.semiPlanar; c.msbOut = m_cfg.semiPlanar;
        c.importable = m_cfg.external;
        return m_e.Configure(c, req, why);
    }
    std::string Describe() const override
    {
        std::string s = "GPU (Vulkan: " + m_e.Device().Describe() + ")";
        if (m_cfg.upscaler) s += std::string(", ") + pmvs::gpu::UpscalerName(m_e.Cfg().upscaler) + " to " +
                                 std::to_string(OutWidth()) + "x" + std::to_string(OutHeight());
        return s;
    }
    bool IsGpu() const override { return true; }
    int OutWidth() const override { return m_e.OutWidth(); }
    int OutHeight() const override { return m_e.OutHeight(); }
    bool Push(const pm_yuv& src, bool contiguous, std::string& err) override
    {
        // keep the previous and the current picture; the one before is no longer referenced
        if (m_prevSlot >= 0 && m_prevPrevPending >= 0) { m_e.ReleaseSlot(m_prevPrevPending); m_prevPrevPending = -1; }
        pmvs::portable::YuvView v;
        v.width = m_cfg.width; v.height = m_cfg.height; v.bits = m_cfg.bits;
        for (int i = 0; i < 3; ++i) { v.data[i] = src.data[i]; v.stride[i] = src.stride[i]; }
        const int slot = m_e.Upload(v, err);
        if (slot < 0) return false;
        return PushSlot(slot, contiguous, err);
    }
    bool PushExternal(const SlotFiller& fill, bool contiguous, std::string& err) override
    {
        if (m_prevSlot >= 0 && m_prevPrevPending >= 0) { m_e.ReleaseSlot(m_prevPrevPending); m_prevPrevPending = -1; }
        const int slot = fill(&m_e, err);
        if (slot < 0) return false;
        return PushSlot(slot, contiguous, err);
    }
    bool PushSlot(int slot, bool contiguous, std::string& err)
    {
        ++m_stats.pushed;
        m_lastDup = false;
        if (m_dedup && contiguous && m_curSlot >= 0) {   // held frames: a repeat of the last distinct frame keeps the pair
            pmvs::gpu::DedupStats ds;
            if (!m_e.Duplicate(m_curSlot, slot, ds, err)) { m_e.ReleaseSlot(slot); return false; }
            if (ds.duplicate) { m_e.ReleaseSlot(slot); m_lastDup = true; return true; }
        }
        const int oldPair = m_pair;
        m_pair = -1;
        m_cut = false;
        if (contiguous && m_curSlot >= 0) {
            pmvs::gpu::PairStats st;
            m_pair = m_e.Estimate(m_curSlot, slot, oldPair, st, err);
            if (m_pair < 0) { if (oldPair >= 0) m_e.ReleasePair(oldPair); m_e.ReleaseSlot(slot); return false; }
            m_cut = m_guard.Decide(st) != 0;
            ++m_stats.pairs;
            if (m_cut) ++m_stats.cuts;
            m_stats.meMs += (st.ms - m_stats.meMs) / double(m_stats.pairs);
        }
        if (oldPair >= 0) m_e.ReleasePair(oldPair);
        m_prevPrevPending = m_prevSlot;
        m_prevSlot = m_curSlot;
        m_curSlot = slot;
        return true;
    }
    bool HavePair() const override { return m_pair >= 0; }
    bool PairIsCut() const override { return m_cut; }
    bool Generate(float t, const pm_yuv_out& out, std::string& err) override
    {
        if (m_pair < 0) return false;
        const int mode = m_cut ? (t < 0.5f ? 1 : 2) : 0;
        int o = -1;
        uint64_t ticket = 0;
        if (!m_e.Generate(m_pair, t, mode, o, ticket, err) || !m_e.Wait(ticket, err)) { if (o >= 0) m_e.ReleaseOut(o); return false; }
        Copy(o, out);
        ++m_stats.generated;
        return true;
    }
    bool Real(bool current, const pm_yuv_out& out, std::string& err) override
    {
        // without an upscaler the caller copies its own picture (exact bytes), unless the output layout differs (NV12 / P010)
        if (!m_cfg.upscaler && !m_cfg.semiPlanar && !m_cfg.external) return false;
        const int slot = current || m_prevSlot < 0 ? m_curSlot : m_prevSlot;
        if (slot < 0) return false;
        int o = -1;
        uint64_t ticket = 0;
        if (!m_e.Real(slot, o, ticket, err) || !m_e.Wait(ticket, err)) { if (o >= 0) m_e.ReleaseOut(o); return false; }
        Copy(o, out);
        return true;
    }
    void Forget() override { ReleaseAll(); }
    void SetDedup(bool on) override { m_dedup = on; m_lastDup = false; }
    bool LastDup() const override { return m_lastDup; }
    EngineStats Stats() const override { return m_stats; }
    bool Layout(OutLayout& l) const override
    {
        const pmvs::gpu::OutputLayout o = m_e.Layout();
        l.pitchY = o.pitchY; l.pitchC = o.pitchC; l.offU = o.offU; l.offV = o.offV; l.size = o.stride;
        return true;
    }
    bool CanImport() const override { return m_e.DmaBufImport(); }
    int Import(int fd, uint64_t size, std::string& err) override { return m_e.ImportOutput(fd, size, err); }
    void ReleaseImport(int id) override { m_e.ReleaseOutput(id); }
    uint64_t HostAlignment() const override { return m_e.HostImportAlignment(); }
    int ImportHost(void* ptr, uint64_t size, std::string& err) override { return m_e.ImportHostOutput(ptr, size, err); }
    bool GenerateInto(float t, int id, std::string& err) override
    {
        if (m_pair < 0) { err = "no pair"; return false; }
        uint64_t ticket = 0;
        if (!m_e.GenerateInto(m_pair, t, m_cut ? (t < 0.5f ? 1 : 2) : 0, id, ticket, err) || !m_e.Wait(ticket, err)) return false;
        ++m_stats.generated;
        return true;
    }
    bool RealInto(bool current, int id, std::string& err) override
    {
        const int slot = current || m_prevSlot < 0 ? m_curSlot : m_prevSlot;
        if (slot < 0) { err = "no picture"; return false; }
        uint64_t ticket = 0;
        return m_e.RealInto(slot, id, ticket, err) && m_e.Wait(ticket, err);
    }
    int MaxK() const override { return kPortableEngineMaxK; }
    int DecideK(int smoother, double srcFps, double displayHz, std::string& why) override
    {
        static std::mutex mu;
        static std::map<std::tuple<int, int, int, int, int, int>, pmvs::gpu::GpuCalibration> cache;
        pmvs::gpu::GpuCalibration cal;
        if (smoother != 1) {
            std::lock_guard<std::mutex> lk(mu);
            const auto key = std::make_tuple(m_cfg.width, m_cfg.height, m_cfg.bits, OutWidth(), OutHeight(), m_cfg.quality);
            auto it = cache.find(key);
            if (it == cache.end()) it = cache.emplace(key, pmvs::gpu::CalibrateGpu(m_e)).first;
            cal = it->second;
            m_stats.genMs = cal.genMs;
        }
        const pmvs::PortableK k = pmvs::DecidePortableK(smoother, srcFps, displayHz, m_cfg.height, cal.meMs, cal.genMs, true,
                                                        m_cfg.fullscreen == 1 ? 0.0 : -1.0, pmvs::VrrActive(m_cfg.fullscreen));
        why = k.why;
        return k.k;
    }

private:
    void Copy(int o, const pm_yuv_out& out)
    {
        pmvs::portable::YuvOut y;
        for (int i = 0; i < 3; ++i) { y.data[i] = out.data[i]; y.stride[i] = out.stride[i]; }
        m_e.Read(o, y);
    }
    void ReleaseAll()
    {
        if (m_pair >= 0) m_e.ReleasePair(m_pair);
        for (int* s : {&m_prevPrevPending, &m_prevSlot, &m_curSlot})
            if (*s >= 0) { m_e.ReleaseSlot(*s); *s = -1; }
        m_pair = -1;
        m_cut = false;
    }
    EngineCfg m_cfg;
    pmvs::gpu::Engine m_e;
    pmvs::gpu::CutGuard m_guard;
    int m_curSlot = -1, m_prevSlot = -1, m_prevPrevPending = -1, m_pair = -1;
    bool m_cut = false;
    EngineStats m_stats;
    bool m_dedup = false, m_lastDup = false;   // held frames (SetDedup)
};
bool GpuBuilt() { return true; }
#else
bool GpuBuilt() { return false; }
#endif

bool NonNull(const void* p) { return p != nullptr; }

std::unique_ptr<IEngine> OpenEngine(const EngineCfg& cfgIn, std::string& why)
{
    EngineCfg cfg = cfgIn;
#ifdef PMK_PMML
    // the anime upscaler: the inner engine runs at its output size; it falls back to the plain engine when it cannot start
    if (!cfg.animeModel.empty() && !cfg.external) {
        pmml::UpscaleOptions u;
        u.runtimeDir = cfg.pmmlRuntime; u.graphsDir = cfg.pmmlRuntime + "/graphs"; u.cacheDir = cfg.pmmlRuntime;
        u.model = cfg.animeModel; u.io = pmml::UpscaleIo::Yuv420; u.colour = pmml::Colour::limited(cfg.bits);
        u.width = cfg.width; u.height = cfg.height;
        std::string ue;
        if (!cfg.pmmlBackend.empty()) pmml::Backend::parse(cfg.pmmlBackend, &u.backend);
        else {
            std::string log;
            const auto devs = pmml::listDevices(cfg.pmmlRuntime, &log);
            for (const char* ep : {"trt-rtx", "dml", "amdgpu", "openvino", "migraphx", "coreml", "qnn-gpu"}) {
                auto it = std::find_if(devs.begin(), devs.end(), [&](const pmml::DeviceInfo& d) { return d.backend.ep == ep; });
                if (it != devs.end()) { u.backend = it->backend; break; }
            }
        }
        auto up = u.backend.ep == "cpu" && cfg.pmmlBackend.empty() ? nullptr : pmml::Upscaler::create(u, &ue);   // the CPU only when named (tests)
        if (up) {
            EngineCfg inner = cfg;
            inner.animeModel.clear();
            inner.width = up->outWidth(); inner.height = up->outHeight();
            if (inner.outWidth > 0 && inner.outWidth <= inner.width * 11 / 10 && inner.outHeight <= inner.height * 11 / 10) { inner.upscaler = 0; inner.outWidth = inner.outHeight = 0; }
            std::string innerWhy;
            auto e = OpenEngine(inner, innerWhy);
            if (e) {
                why = innerWhy + "; anime upscaler " + cfg.animeModel + " first";
                return std::make_unique<AnimeSrEngine>(std::move(up), std::move(e), cfg.width, cfg.height, cfg.bits);
            }
            ue = innerWhy;
        }
        why = "anime upscaler " + cfg.animeModel + " not used (" + (ue.empty() ? std::string("no GPU execution provider in the pmml runtime") : ue) + "); ";
        cfg.animeModel.clear();
    }
#endif
    std::string gpuWhy, nvlWhy;
#if defined(PM_NVL)
    // Linux with an NVIDIA GPU: NVIDIA VFG / optical flow and RTX VSR first (Auto, or the GPU choice); the portable
    // GPU engine wherever it does not run (no driver, no runtime installed, nothing to do). Off for the dma-buf
    // in-place path's P010 and for GPU frames (those need the portable GPU engine).
    if (cfg.backend != 1 && !cfg.semiPlanar && !cfg.external) {
        if (auto n = OpenNvl(cfg, nvlWhy)) { why = "NVIDIA engine"; return n; }
    }
#endif
#ifdef PMVS_GPU
    if (cfg.backend != 1) {
        auto g = std::make_unique<GpuEngine>(cfg);
        if (g->Open(gpuWhy)) { if (!nvlWhy.empty()) why = "NVIDIA engine not used: " + nvlWhy; return g; }
        if (cfg.backend == 2) { why = "GPU engine: " + gpuWhy; return nullptr; }
    }
#else
    gpuWhy = "this build has no GPU engine";
#endif
    auto c = std::make_unique<CpuEngine>(cfg);
    std::string cpuWhy;
    if (!c->Open(cpuWhy)) { why += "CPU engine: " + cpuWhy; return nullptr; }
    why += cfg.backend == 1 ? "CPU engine (your setting)" : "no usable GPU (" + gpuWhy + (nvlWhy.empty() ? "" : "; NVIDIA: " + nvlWhy) + "): CPU engine";
    return c;
}

} // namespace pmk
