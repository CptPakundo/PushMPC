// kodi_nvl.cpp: the add-on's engine on Linux with an NVIDIA GPU (vsplugin/engines/nvidia-linux, "nvl"): NVIDIA's
// VFG frame generation (or NVIDIA optical flow + PushMPC's warp where VFG does not run), RTX Video Super Resolution to
// the screen size, PushFPS's cut guard, through CUDA on the proprietary driver and NVIDIA's Video Effects runtime
// (installed by PushMPC's setup.sh with the licence step). Settings: ~/.config/pushmpc/settings.ini, as for mpv.
// The multiplier is the engine's own Auto plan (nvl_auto.h), fixed for the stream (VFG's FrameMultiplier).
// Built when nvcc is available at build time (PM_NVL); everything is loaded at run time, so a machine without the
// driver or the runtime just falls through to the portable GPU engine.
#include "kodi_engine.h"

#if defined(PM_NVL)
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include "nvl_auto.h"
#include "nvl_cutguard.h"
#include "nvl_engine.h"
#include "nvl_selftest.h"
#include "nvl_settings.h"
#include "platform.h"
#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace pmk {

using namespace pmvs::nvl;

namespace {
const GpuInfo& Probe()
{
    static std::mutex m;
    static bool done = false;
    static GpuInfo g;
    std::lock_guard<std::mutex> lk(m);
    if (done) return g;
    done = true;
    g = ProbeGpu(-1, "");
    if (g.driver && g.runtime && VfgAmpere(g.major, g.minor)) {   // RTX 30: VFG only after the self-test passed
        const char* force = std::getenv("PUSHMPC_VFG_AMPERE");
        const std::string key = SelfTestKey(g);
        int r = force ? (std::strcmp(force, "0") ? 1 : 0) : ReadSelfTest(key);
        std::string log;
        const std::string exe = SelfTestExe();
#if !defined(_WIN32)
        // Kodi's "Install from zip file" drops the executable bit of the self-test next to the library: put it back
        if (!exe.empty() && access(exe.c_str(), F_OK) == 0 && access(exe.c_str(), X_OK) != 0) chmod(exe.c_str(), 0755);
#endif
        if (r < 0) r = RunSelfTest(exe, g.runtimeDir, g.device, 120, log);
        if (r == 1) { g.caps.vfg = true; g.caps.vfgWhy.clear(); }
        else g.caps.vfgWhy = "RTX 30: the VFG compatibility self-test did not pass";
    }
    return g;
}
} // namespace

class NvlEngine final : public IEngine {
public:
    explicit NvlEngine(const EngineCfg& c) : m_cfg(c) {}
    bool Open(std::string& why)
    {
        if (std::getenv("PUSHMPC_KODI_NO_NVL")) { why = "off (PUSHMPC_KODI_NO_NVL)"; return false; }
        const GpuInfo& g = Probe();
        if (!g.driver) { why = g.why; return false; }
        if (!g.runtime) { why = "NVIDIA's Video Effects runtime is not installed (" + g.runtimeWhy + ")"; return false; }
        Settings s = LoadSettings([](const char* n) { return pmvs::ReadSetting(n); });
        if (m_cfg.smoother == 1) s.smoother = 1;
        else if (m_cfg.smoother >= 2) s.smoother = std::clamp(m_cfg.smoother, 2, 8);
        s.sdrToHdr = -1;   // Kodi was told the stream's colour: no SDR-to-HDR conversion here
        // Kodi's Auto upscaling (outHeight set): RTX VSR to that size; else the source size
        if (m_cfg.outHeight > 0 && m_cfg.upscaler) { s.targetH = m_cfg.outHeight; s.targetScreen = false; }
        else { s.targetH = m_cfg.height; s.targetScreen = false; s.sharper = -1; }
        Source src; src.w = m_cfg.width; src.h = m_cfg.height;
        src.fps = m_cfg.srcFps > 1 ? m_cfg.srcFps : 23.976; src.hdr = m_cfg.hdr; src.tenBit = m_cfg.bits > 8;
        Display d; d.refreshHz = m_cfg.displayHz;
        Costs costs; costs.card = CardFactor(g.name, g.cls);
        const Plan plan = Decide(s, src, d, g.caps, costs);
        m_why = plan.why;
        if (plan.k <= 1 && !plan.vsr) { why = "nothing to do on this GPU for this video (" + plan.why + ")"; return false; }
        EngineConfig ec;
        ec.srcW = m_cfg.width; ec.srcH = m_cfg.height; ec.bits = m_cfg.bits; ec.srcHdr = m_cfg.hdr; ec.plan = plan;
        ec.ampere = plan.k >= 2 && !plan.nvof && VfgAmpere(g.major, g.minor);
        ec.guardFlow = plan.guardFlow;
        std::string err;
        if (!m_e.Open(g, ec, err)) {
            if (plan.k >= 2 && plan.vsr) {   // frame generation failed: upscaling only, as pushcore's fallback chain does
                ec.plan.k = 1; ec.plan.nvof = false; ec.ampere = false; ec.guardFlow = false;
                if (!m_e.Open(g, ec, err)) { why = err; return false; }
            } else { why = err; return false; }
        }
        m_k = std::max(1, m_e.Config().plan.k);
        const Plan& ep = m_e.Config().plan;
        m_outW = ep.vsr ? ep.outW : m_cfg.width;
        m_outH = ep.vsr ? ep.outH : m_cfg.height;
        m_guard.Configure(m_cfg.width, m_cfg.height);
        m_fmt.bits = m_cfg.bits; m_fmt.full = m_cfg.fullRange; m_fmt.matrix = m_cfg.matrix > 0 ? m_cfg.matrix : 1;
        m_desc = "NVIDIA (" + g.name + "): " + m_e.Describe();
        return true;
    }
    std::string Describe() const override { return m_desc; }
    bool IsGpu() const override { return true; }
    int OutWidth() const override { return m_outW; }
    int OutHeight() const override { return m_outH; }
    int FixedK() const override { return m_k; }
    bool Push(const pm_yuv& src, bool contiguous, std::string& err) override
    {
        SrcView v;
        for (int i = 0; i < 3; ++i) { v.data[i] = src.data[i]; v.stride[i] = src.stride[i]; }
        v.width = m_cfg.width; v.height = m_cfg.height; v.bits = m_cfg.bits; v.fmt = m_fmt;
        const bool cont = contiguous && m_have;
        m_parity ^= 1;
        CutDecision cut;
        const bool gpuGuard = m_e.HasGpuGuard();
        if (!gpuGuard)
            cut = m_guard.Push([&](int x, int y) -> uint32_t {
                const uint8_t* row = v.data[0] + v.stride[0] * y;
                const uint32_t c = v.bits > 8 ? reinterpret_cast<const uint16_t*>(row)[x] : row[x];
                return GuardLuma(c, v.bits, v.fmt.full);
            }, cont);
        if (!m_e.Upload(v, m_parity, err)) { m_have = false; return false; }
        if (cont && m_k >= 2 && m_e.HasFlow() && !m_e.Flow(m_parity, err)) { m_have = false; return false; }
        if (gpuGuard && m_k >= 2 && !m_e.Guard(m_parity, cont, err)) { m_have = false; return false; }
        m_cut = gpuGuard ? false : cut.cut;   // the GPU guard holds A on the GPU by itself
        if (m_cut) ++m_stats.cuts;
        m_pair = cont;
        m_have = true;
        m_nextJ = 1;
        ++m_stats.pushed;
        if (m_pair) ++m_stats.pairs;
        return true;
    }
    bool HavePair() const override { return m_pair; }
    bool PairIsCut() const override { return m_cut; }
    bool Generate(float t, const pm_yuv_out& out, std::string& err) override
    {
        if (!m_pair) { err = "no pair"; return false; }
        const int j = std::clamp(int(std::lround(double(t) * m_k)), 1, m_k - 1);
        const int slot = m_cut ? m_e.Emit(1 - m_parity, err) : m_e.Generate(m_parity, j, m_k, m_nextJ == 1, err);   // the first of the pair rebinds the inputs
        m_nextJ = j + 1;
        if (slot < 0) return false;
        const bool ok = Take(slot, out, err);
        if (ok && !m_cut) ++m_stats.generated;
        return ok;
    }
    bool Real(bool current, const pm_yuv_out& out, std::string& err) override
    {
        if (!m_have) { err = "no picture"; return false; }
        if (m_outW == m_cfg.width && m_outH == m_cfg.height) return false;   // the caller copies its own frame
        const int slot = m_e.Emit(current || !m_pair ? m_parity : 1 - m_parity, err);
        return slot >= 0 && Take(slot, out, err);
    }
    void Forget() override
    {
        std::string err;
        m_e.Drain(err);
        m_guard.Reset();
        m_have = m_pair = m_cut = false;
    }
    EngineStats Stats() const override { return m_stats; }
    int DecideK(int, double, double, std::string& why) override { why = "x" + std::to_string(m_k) + " (NVIDIA, Auto: " + m_why + ")"; return m_k; }

private:
    // a finished slot into the caller's planes, converted to the stream's depth (8 bits, or 10 in 16-bit words)
    bool Take(int slot, const pm_yuv_out& out, std::string& err)
    {
        if (!m_e.Wait(slot, 2000)) { m_e.Release(slot); err = "the GPU did not finish a frame in time"; return false; }
        const OutView o = m_e.View(slot);
        const int ib = o.bits > 8 ? 2 : 1, ob = m_cfg.bits > 8 ? 2 : 1;
        for (int i = 0; i < 3; ++i) {
            const int w = i ? (m_outW + 1) / 2 : m_outW, h = i ? (m_outH + 1) / 2 : m_outH;
            for (int y = 0; y < h; ++y) {
                const uint8_t* s = o.data[i] + o.pitch[i] * size_t(y);
                uint8_t* d = out.data[i] + out.stride[i] * y;
                if (ib == ob) { std::memcpy(d, s, size_t(w) * ib); continue; }
                if (ib == 2) { const auto* s16 = reinterpret_cast<const uint16_t*>(s); for (int x = 0; x < w; ++x) d[x] = uint8_t(std::min(255, (s16[x] + 2) >> 2)); }
                else { auto* d16 = reinterpret_cast<uint16_t*>(d); for (int x = 0; x < w; ++x) d16[x] = uint16_t(s[x] << 2); }
            }
        }
        m_e.Release(slot);
        return true;
    }
    EngineCfg m_cfg;
    Engine m_e;
    CutGuard m_guard;
    InFormat m_fmt;
    int m_k = 1, m_outW = 0, m_outH = 0, m_parity = 1, m_nextJ = 1;
    bool m_have = false, m_pair = false, m_cut = false;
    std::string m_desc, m_why;
    EngineStats m_stats;
};

std::unique_ptr<IEngine> OpenNvl(const EngineCfg& cfg, std::string& why)
{
    auto e = std::make_unique<NvlEngine>(cfg);
    if (!e->Open(why)) return nullptr;
    return e;
}

} // namespace pmk
#else
namespace pmk {
std::unique_ptr<IEngine> OpenNvl(const EngineCfg&, std::string& why) { why = "this build has no NVIDIA engine"; return nullptr; }
} // namespace pmk
#endif
