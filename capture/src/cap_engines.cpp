// SPDX-License-Identifier: GPL-3.0-or-later
// PushMPC capture mode: engines on frames in host memory (cap_engines.h).
#include "cap_engines.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "backend.h"            // DecidePortableK (vsplugin/src)
#include "platform.h"           // ReadSetting, the settings' parsers
#include "portable_engine.h"
#ifdef PMC_HAVE_PMML
#include "pmml/pmml.h"
#endif
#if defined(__APPLE__) && defined(PMC_HAVE_APPLE)
#include "apple_engine.h"
#endif
#ifdef PMC_HAVE_NVL
#include <cstring>
#include <deque>
#include <memory>
#include "nvl_backend.h"        // the plugin's nvidia backend on Linux (engines/nvidia-linux), used as it is
#include "nvl_host.h"
#include "nvl_settings.h"
#endif

namespace pmc {
namespace {

int Quality()
{
    const int q = pmvs::ParseQuality(pmvs::ReadSetting("PortableQuality"), -1);
    return q == 0 ? 0 : 1;
}

// ------------------------------------------------------------------------------------------------ cpu
class CpuEngine : public HostEngine {
public:
    bool Open(const EngineRequest& r, std::string& why)
    {
        pmvs::portable::Config c;
        c.width = r.w; c.height = r.h; c.bits = r.bits; c.quality = Quality();
        m_pq = r.pq;
        if (!m_e.Configure(c, why)) return false;
        const auto cal = pmvs::portable::Calibrate(r.w, r.h, r.bits, m_e.Threads(), c.quality);
        const double fps = r.srcFps > 0 ? r.srcFps : 23.976, hz = r.displayHz > 0 ? r.displayHz : 60;
        const double perSource = hz / fps;   // screen refreshes per source frame
        const double budget = 1000.0 / fps * pmvs::kCpuShare;
        const double anyT = cal.meMs + std::max(0.0, std::ceil(perSource) - 1) * cal.genMs;
        char b[320];
        if (r.smoother == 0 && anyT <= budget) {
            m_steps = 0;
            std::snprintf(b, sizeof(b), "PushFPS portable engine on the CPU (%d threads), every screen refresh its own frame (%.1f of %.1f ms per source frame)",
                          m_e.Threads(), anyT, budget);
        } else {
            const auto pk = pmvs::DecidePortableK(r.smoother, fps, hz, r.h, cal.meMs, cal.genMs, false);
            m_steps = r.smoother == 0 ? pk.k : 0;
            std::snprintf(b, sizeof(b), "PushFPS portable engine on the CPU (%d threads): %s", m_e.Threads(), pk.why.c_str());
        }
        m_desc = b;
        return true;
    }
    std::string Describe() const override { return m_desc; }
    int Steps() const override { return m_steps; }
    bool Push(const I420& f, bool contiguous, std::string& err) override
    {
        pmvs::portable::YuvView v;
        v.width = f.w; v.height = f.h; v.bits = f.bits;
        v.data[0] = f.y.data(); v.data[1] = f.u.data(); v.data[2] = f.v.data();
        v.stride[0] = f.w * f.bps(); v.stride[1] = v.stride[2] = f.cw() * f.bps();
        return m_e.Push(v, contiguous, err);
    }
    bool Render(float t, I420& out, std::string& err) override
    {
        if (!m_e.HavePair()) { err = "no pair"; return false; }
        const auto& c = m_e.Cfg();
        if (!out.Same(c.width, c.height, c.bits)) out.Resize(c.width, c.height, c.bits);
        out.pq = m_pq;
        pmvs::portable::YuvOut o;
        o.data[0] = out.y.data(); o.data[1] = out.u.data(); o.data[2] = out.v.data();
        o.stride[0] = out.w * out.bps(); o.stride[1] = o.stride[2] = out.cw() * out.bps();
        m_e.Generate(t, o);
        return true;
    }
private:
    pmvs::portable::Engine m_e;
    int m_steps = 0;
    bool m_pq = false;
    std::string m_desc;
};

// ------------------------------------------------------------------------------------------------ ml (pmml)
std::string MlDir()
{
    auto present = [](const std::string& d) {
        if (d.empty()) return false;
        if (FILE* f = std::fopen((d + "/graphs/synth-yuv.onnx").c_str(), "rb")) { std::fclose(f); return true; }
        return false;
    };
    std::string d = pmvs::ReadSetting("MlRuntime");
    if (d.empty()) d = pmvs::EnvA("PMML_RUNTIME");
    if (!d.empty()) return present(d) ? d : std::string();
    const std::string xdg = pmvs::EnvA("XDG_DATA_HOME"), home = pmvs::EnvA("HOME");
    for (const std::string& c : { !xdg.empty() && xdg[0] == '/' ? xdg + "/pushmpc/pmml" : std::string(), home.empty() ? std::string() : home + "/.local/share/pushmpc/pmml" })
        if (present(c)) return c;
    return {};
}

#ifdef PMC_HAVE_PMML
class MlEngine : public HostEngine {
public:
    bool Open(const EngineRequest& r, std::string& why)
    {
        const std::string rt = MlDir();
        if (rt.empty()) { why = "the PushFlow runtime (pmml) is not installed"; return false; }
        pmml::Options o;
        o.runtimeDir = rt; o.modelsDir = rt + "/models"; o.graphsDir = rt + "/graphs"; o.ncnnModelsDir = rt + "/ncnn-models"; o.cacheDir = rt;
        o.width = r.w; o.height = r.h;
        // the network's float input from 16-bit codes: pmml's own conversion with this picture's depth and matrix
        // (an HDR capture: 10 bit, BT.2020 NCL; it interpolates the PQ signal, as every route does); output in the same codes
        o.colour = r.pq ? pmml::Colour::limited(r.bits, 0.2627f, 0.0593f) : pmml::Colour::limited(r.bits);
        m_bits = r.bits; m_pq = r.pq;
        std::string log;
        const auto devs = pmml::listDevices(rt, &log);
        static const char* order[] = { "trt-rtx", "dml", "amdgpu", "openvino", "migraphx", "coreml", "qnn-gpu", "vulkan" };
        bool gpu = false;
        for (const char* ep : order) {
            auto it = std::find_if(devs.begin(), devs.end(), [&](const pmml::DeviceInfo& d) { return d.backend.ep == ep; });
            if (it != devs.end()) { o.backend = it->backend; gpu = true; break; }
        }
        if (!gpu) { why = "pmml lists no GPU here"; return false; }
        o.tier = "fast";
        const double fps = r.srcFps > 0 ? r.srcFps : 23.976, hz = r.displayHz > 0 ? r.displayHz : 60;
        m_phases = std::max(1, int(std::lround(hz / fps)) - 1);
        m_interp = pmml::Interpolator::create(o, &why);
        if (!m_interp) return false;
        m_w = r.w; m_h = r.h;
        m_desc = "PushFlow (RIFE) through pmml: " + m_interp->describe() + (m_phases > 1 ? " (flow once per pair)" : "");
        return true;
    }
    std::string Describe() const override { return m_desc; }
    bool Push(const I420& f, bool contiguous, std::string& err) override
    {
        m_cur ^= 1;
        To16(f, m_p[m_cur]);
        m_have = contiguous && m_seen;
        m_seen = true;
        if (!m_have) return true;
        pmml::Frame a = View(m_p[m_cur ^ 1]), b = View(m_p[m_cur]);
        return m_interp->setPair(a, b, m_phases, &err);
    }
    bool Render(float t, I420& out, std::string& err) override
    {
        if (!m_have) { err = "no pair"; return false; }
        for (int i = 0; i < 3; ++i) m_o[i].resize(i ? size_t(m_w / 2) * size_t(m_h / 2) : size_t(m_w) * size_t(m_h));
        pmml::OutFrame o;
        for (int i = 0; i < 3; ++i) { o.plane[i] = m_o[i].data(); o.stride[i] = i ? m_w / 2 : m_w; }
        if (!m_interp->render(t, o, &err)) return false;
        if (!out.Same(m_w, m_h, m_bits)) out.Resize(m_w, m_h, m_bits);
        out.pq = m_pq;
        std::vector<uint8_t>* dst[3] = { &out.y, &out.u, &out.v };
        const uint16_t top = uint16_t((1u << m_bits) - 1);
        for (int i = 0; i < 3; ++i) {
            const size_t n = std::min(m_o[i].size(), dst[i]->size() / size_t(out.bps()));
            if (m_bits <= 8) { for (size_t k = 0; k < n; ++k) (*dst[i])[k] = uint8_t(std::min<uint16_t>(255, m_o[i][k])); }
            else for (size_t k = 0; k < n; ++k) { const uint16_t v = std::min(top, m_o[i][k]); (*dst[i])[2 * k] = uint8_t(v & 255); (*dst[i])[2 * k + 1] = uint8_t(v >> 8); }
        }
        return true;
    }
private:
    struct P16 { std::vector<uint16_t> p[3]; };
    void To16(const I420& f, P16& o) const
    {
        const std::vector<uint8_t>* s[3] = { &f.y, &f.u, &f.v };
        for (int i = 0; i < 3; ++i) {
            const size_t n = s[i]->size() / size_t(f.bps());
            o.p[i].resize(n);
            if (f.bits <= 8) for (size_t k = 0; k < n; ++k) o.p[i][k] = (*s[i])[k];
            else for (size_t k = 0; k < n; ++k) o.p[i][k] = uint16_t((*s[i])[2 * k] | ((*s[i])[2 * k + 1] << 8));
        }
    }
    pmml::Frame View(const P16& p) const
    {
        pmml::Frame f;
        for (int i = 0; i < 3; ++i) { f.plane[i] = p.p[i].data(); f.stride[i] = i ? m_w / 2 : m_w; }
        return f;
    }
    std::unique_ptr<pmml::Interpolator> m_interp;
    P16 m_p[2];
    std::vector<uint16_t> m_o[3];
    int m_cur = 0, m_w = 0, m_h = 0, m_phases = 1, m_bits = 8;
    bool m_have = false, m_seen = false, m_pq = false;
    std::string m_desc;
};
#endif

// ------------------------------------------------------------------------------------------------ apple
#if defined(__APPLE__) && defined(PMC_HAVE_APPLE)
class AppleEngine : public HostEngine {
public:
    bool Open(const EngineRequest& r, std::string& why)
    {
        const auto& caps = pmvs::apple::Probe();
        if (!caps.AnyInterpolation()) { why = "this Mac offers no VideoToolbox frame interpolation (macOS 15.4 or later)"; return false; }
        const double fps = r.srcFps > 0 ? r.srcFps : 23.976, hz = r.displayHz > 0 ? r.displayHz : 60;
        int k = r.smoother >= 2 ? r.smoother : std::clamp(int(std::ceil(hz / fps - 0.02)), 2, 8);
        pmvs::apple::Config c;
        c.width = r.w; c.height = r.h; c.bits = r.bits; c.k = k; c.sr = "off"; c.srcFps = fps;
        m_bits = r.bits; m_pq = r.pq;
        std::string info;
        if (!m_e.Configure(c, why, info)) return false;
        m_k = m_e.K();
        m_w = m_e.OutWidth(); m_h = m_e.OutHeight();
        m_desc = "Apple VideoToolbox: " + info;
        return true;
    }
    std::string Describe() const override { return m_desc; }
    int Steps() const override { return m_k; }
    bool Push(const I420& f, bool contiguous, std::string& err) override
    {
        pmvs::apple::PlanarView v;
        v.width = f.w; v.height = f.h; v.bits = f.bits;
        v.data[0] = f.y.data(); v.data[1] = f.u.data(); v.data[2] = f.v.data();
        v.stride[0] = f.w * f.bps(); v.stride[1] = v.stride[2] = f.cw() * f.bps();
        m_gen.resize(size_t(std::max(0, m_k - 1)));
        std::vector<pmvs::apple::PlanarOut> outs(m_gen.size());
        for (size_t i = 0; i < m_gen.size(); ++i) {
            if (!m_gen[i].Same(m_w, m_h, m_bits)) m_gen[i].Resize(m_w, m_h, m_bits);
            m_gen[i].pq = m_pq;
            outs[i].data[0] = m_gen[i].y.data(); outs[i].data[1] = m_gen[i].u.data(); outs[i].data[2] = m_gen[i].v.data();
            outs[i].stride[0] = m_w * m_gen[i].bps(); outs[i].stride[1] = outs[i].stride[2] = m_gen[i].cw() * m_gen[i].bps();
        }
        if (!m_cur.Same(m_w, m_h, m_bits)) m_cur.Resize(m_w, m_h, m_bits);
        m_cur.pq = m_pq;
        pmvs::apple::PlanarOut cur;
        cur.data[0] = m_cur.y.data(); cur.data[1] = m_cur.u.data(); cur.data[2] = m_cur.v.data();
        cur.stride[0] = m_w * m_cur.bps(); cur.stride[1] = cur.stride[2] = m_cur.cw() * m_cur.bps();
        pmvs::apple::Colour col;   // BT.709 limited; an HDR capture: BT.2020 PQ (VapourSynth codes 9 / 16 / 9)
        if (m_pq) { col.matrix = 9; col.transfer = 16; col.primaries = 9; }
        m_pts += 417083;
        return m_e.Process(v, col, m_pts, contiguous, outs, cur, m_have, err);
    }
    bool Render(float t, I420& out, std::string& err) override
    {
        if (!m_have || m_gen.empty()) { err = "no pair"; return false; }
        const int j = std::clamp(int(std::lround(t * m_k)), 1, m_k - 1);
        out = m_gen[size_t(j - 1)];
        return true;
    }
private:
    pmvs::apple::Engine m_e;
    std::vector<I420> m_gen;
    I420 m_cur;
    int m_k = 2, m_w = 0, m_h = 0, m_bits = 8;
    bool m_pq = false;
    int64_t m_pts = 0;
    bool m_have = false;
    std::string m_desc;
};
#endif


// ------------------------------------------------------------------------------------------------ nvl (Linux NVIDIA)
#ifdef PMC_HAVE_NVL
void NvlLog(const std::string& m) { std::fprintf(stderr, "pushmpc-capture: %s\n", m.c_str()); }

// The plugin's Linux nvidia backend as a k-step engine: nvl::NvlBackend (the same submit / poll order, cut guard and
// optical-flow tier as in mpv) on nvl::Engine, decided by nvl::Decide (Auto: VFG where it runs, RTX 30 after its
// self-test, else the optical-flow tier; RTX VSR to the window's size; RTX Video HDR off: the overlay is SDR). Each push
// runs the pair to the end and keeps its k - 1 generated frames and the enhanced real frame.
class NvlEngine : public HostEngine {
public:
    ~NvlEngine() override
    {
        if (!m_eng) return;
        std::string e;
        m_be.Flush(e);
        double fl = 0, wa = 0;
        m_eng->FlowCosts(fl, wa);
        if (fl > 0 && wa > 0) pmvs::nvl::WriteFlowCosts(m_flowKey, fl, wa);   // Auto's optical-flow costs, measured here
    }
    bool Open(const EngineRequest& r, std::string& why)
    {
        using namespace pmvs::nvl;
        const GpuInfo& g = ProbeGated(-1, pmvs::ReadSetting("NvidiaVfxRuntime"), NvlLog);
        if (!g.driver) { why = g.why; return false; }
        if (!g.runtime) { why = g.runtimeWhy; return false; }
        Settings s = LoadSettings([](const char* n) { return pmvs::ReadSetting(n); });
        if (!s.enable) { why = "PushMPC is off (Enable = 0)"; return false; }
        if (r.smoother >= 0) s.smoother = r.smoother;
        // an SDR overlay: no RTX Video HDR; an HDR overlay (HDR10): the user's SdrToHdr setting (Auto: on), HDR captures stay HDR
        if (!r.hdrOut) s.sdrToHdr = -1;
        if (s.targetScreen && r.displayH > 0) s.targetH = TargetForDisplay(r.displayH);
        Source src; src.w = r.w; src.h = r.h; src.fps = r.srcFps > 0 ? r.srcFps : 23.976;
        src.hdr = r.pq; src.tenBit = r.bits > 8;
        Display d; d.refreshHz = r.displayHz; d.hdr = r.hdrOut;
        Costs c; c.card = CardFactor(g.name, g.cls);
        m_flowKey = FlowCostKey(g.name, src.w, src.h, s.targetH);
        ReadFlowCosts(m_flowKey, c.flowMeasured, c.warpMeasured);
        Plan plan = Decide(s, src, d, g.caps, c);
        if (!r.hdrOut) plan.hdr = false;
        if (plan.k <= 1 && !plan.vsr) { why = "nothing to do here (" + plan.why + ")"; return false; }
        EngineConfig cfg;
        cfg.srcW = r.w; cfg.srcH = r.h; cfg.bits = r.bits; cfg.srcHdr = r.pq; cfg.plan = plan;
        cfg.ampere = plan.k >= 2 && !plan.nvof && VfgAmpere(g.major, g.minor);
        cfg.guardFlow = plan.guardFlow;
        m_eng = std::make_unique<Engine>();
        std::string err;
        if (!m_eng->Open(g, cfg, err)) {
            if (cfg.plan.k < 2 || !cfg.plan.vsr) { why = err; m_eng.reset(); return false; }
            // frame generation failed: upscaling only, as pushcore's fallback chain does
            NvlLog(std::string("nvidia: ") + (cfg.plan.nvof ? "the optical-flow tier" : "VFG") + " could not start (" + err + "): upscaling only");
            cfg.plan.k = 1; cfg.plan.nvof = false; cfg.ampere = false; cfg.guardFlow = false;
            m_eng = std::make_unique<Engine>();
            if (!m_eng->Open(g, cfg, err)) { why = err; m_eng.reset(); return false; }
        }
        const Plan& ep = m_eng->Config().plan;
        m_k = std::max(1, ep.k);
        m_be.engine = m_eng.get();
        m_be.grid.k = m_k;
        m_be.grid.srcDur100ns = 1e7 / src.fps;
        m_be.guard.Configure(r.w, r.h);
        m_be.source = [this](int64_t, SrcView& v) -> bool {
            if (!m_src) return false;
            v.data[0] = m_src->y.data(); v.data[1] = m_src->u.data(); v.data[2] = m_src->v.data();
            v.stride[0] = m_src->w * m_src->bps(); v.stride[1] = v.stride[2] = m_src->cw() * m_src->bps();
            v.width = m_src->w; v.height = m_src->h; v.bits = m_src->bits;
            v.fmt.bits = m_src->bits; v.fmt.full = false; v.fmt.matrix = m_src->pq ? 9 : 1;   // the converter's: BT.709, or BT.2020 for PQ
            return true;
        };
        m_be.alloc = [this](const OutView& o, int& slot) -> bool {
            m_store.emplace_back();
            I420& p = m_store.back();
            // HDR (the engine's PQ BT.2020 output) stays 10 bit; a deeper SDR output goes to 8 bit
            const bool keep = m_eng->OutHdr() && o.bits > 8;
            p.Resize(o.width, o.height, keep ? o.bits : 8);
            p.pq = m_eng->OutHdr();
            uint8_t* dst[3] = { p.y.data(), p.u.data(), p.v.data() };
            for (int i = 0; i < 3; ++i) {
                const int w = i ? p.cw() : p.w, h = i ? p.ch() : p.h;
                for (int y = 0; y < h; ++y) {
                    const uint8_t* row = o.data[i] + o.pitch[i] * size_t(y);
                    uint8_t* out = dst[i] + size_t(y) * size_t(w) * size_t(p.bps());
                    if (keep || o.bits <= 8) {
                        std::memcpy(out, row, size_t(w) * size_t(p.bps()));
                    } else {
                        const uint16_t* r16 = reinterpret_cast<const uint16_t*>(row);
                        const int sh = o.bits - 8;
                        for (int x = 0; x < w; ++x) out[x] = uint8_t(std::min(255, (int(r16[x]) + (1 << (sh - 1))) >> sh));
                    }
                }
            }
            slot = int(m_store.size()) - 1;
            return true;
        };
        m_be.release = [](int) {};
        m_desc = "NVIDIA Video Effects on " + g.name + ": " + ep.why;
        return true;
    }
    std::string Describe() const override { return m_desc; }
    int Steps() const override { return m_k; }
    bool EnhancesReal() const override { return true; }
    bool Push(const I420& f, bool contiguous, std::string& err) override
    {
        std::vector<pmvs::Out<int>> outs;
        m_store.clear();
        const int64_t s = ++m_s;
        m_src = &f;
        const bool ok = m_be.CanSubmit() && m_be.Submit(s, m_be.grid.Pts(s), !contiguous, err);
        m_src = nullptr;
        if (!ok) { if (err.empty()) err = "nvidia: no free output slot"; return false; }
        // the pair's generated frames (k - 1, or none after a jump), then the real frame
        const size_t want = size_t(contiguous && m_k >= 2 ? m_k : 1);
        while (outs.size() < want) {
            const size_t before = outs.size();
            if (!m_be.Poll(2000, outs, err)) return false;
            if (outs.size() == before) { err = "nvidia: the GPU did not finish in time"; return false; }
        }
        m_gen.clear();
        m_have = false;
        for (auto& o : outs) {
            if (o.generated) m_gen.push_back(std::move(m_store[size_t(o.frame)]));
            else { m_real = std::move(m_store[size_t(o.frame)]); m_have = true; }
        }
        m_store.clear();
        return m_have;
    }
    bool Render(float t, I420& out, std::string& err) override
    {
        if (m_gen.empty()) { err = "no pair"; return false; }
        const int j = std::clamp(int(std::lround(t * m_k)), 1, int(m_gen.size()));
        out = m_gen[size_t(j - 1)];
        return true;
    }
    bool RenderReal(I420& out, std::string& err) override
    {
        if (!m_have) { err = "no frame yet"; return false; }
        out = m_real;
        return true;
    }

private:
    using SrcView = pmvs::nvl::SrcView;
    using OutView = pmvs::nvl::OutView;
    std::unique_ptr<pmvs::nvl::Engine> m_eng;
    pmvs::nvl::NvlBackend<int> m_be;
    const I420* m_src = nullptr;
    std::deque<I420> m_store;   // the outputs of one push (a deque: what alloc filled stays where it is)
    std::vector<I420> m_gen;
    I420 m_real;
    bool m_have = false;
    int m_k = 1;
    int64_t m_s = 0;
    std::string m_desc, m_flowKey;
};
#endif

} // namespace

bool MlInstalled()
{
#ifdef PMC_HAVE_PMML
    return !MlDir().empty();
#else
    return false;
#endif
}

bool AppleAvailable(std::string* why)
{
#if defined(__APPLE__) && defined(PMC_HAVE_APPLE)
    const bool ok = pmvs::apple::Probe().AnyInterpolation();
    if (why) *why = ok ? "VideoToolbox frame interpolation" : "no VideoToolbox frame interpolation on this macOS";
    return ok;
#else
    if (why) *why = "not a macOS build";
    return false;
#endif
}

bool NvlAvailable(std::string* why)
{
#ifdef PMC_HAVE_NVL
    const auto& g = pmvs::nvl::ProbeGated(-1, pmvs::ReadSetting("NvidiaVfxRuntime"), NvlLog);
    const bool ok = g.driver && g.runtime && (g.caps.vfg || g.caps.vsr || g.caps.nvof);
    if (why) *why = ok ? "NVIDIA Video Effects" : !g.driver ? g.why : !g.runtime ? g.runtimeWhy : "no NVIDIA effect runs on this GPU";
    return ok;
#else
    if (why) *why = "not in this build (Linux with the nvidia backend)";
    return false;
#endif
}

std::unique_ptr<HostEngine> OpenHostEngine(const std::string& name, const EngineRequest& r, std::string& why)
{
#ifdef PMC_HAVE_NVL
    if (name == "nvl") { auto e = std::make_unique<NvlEngine>(); if (e->Open(r, why)) return e; return nullptr; }
#endif
    if (name == "cpu") { auto e = std::make_unique<CpuEngine>(); if (e->Open(r, why)) return e; return nullptr; }
#ifdef PMC_HAVE_PMML
    if (name == "ml") { auto e = std::make_unique<MlEngine>(); if (e->Open(r, why)) return e; return nullptr; }
#endif
#if defined(__APPLE__) && defined(PMC_HAVE_APPLE)
    if (name == "apple") { auto e = std::make_unique<AppleEngine>(); if (e->Open(r, why)) return e; return nullptr; }
#endif
    why = name + ": not in this build";
    return nullptr;
}

// ------------------------------------------------------------------------------------------------ the pipeline
void HostPipeline::Add(uint64_t idx, bool discontinuity, I420&& pic)
{
    Entry e;
    e.idx = idx; e.disc = discontinuity; e.pic = std::move(pic);
    m_q.push_back(std::move(e));
    while (m_q.size() > 6) m_q.pop_front();
}

std::vector<uint64_t> HostPipeline::Frames() const
{
    std::vector<uint64_t> f;
    for (auto& e : m_q) f.push_back(e.idx);
    return f;
}

std::vector<uint64_t> HostPipeline::Discontinuities() const
{
    std::vector<uint64_t> f;
    for (auto& e : m_q) if (e.disc) f.push_back(e.idx);
    return f;
}

HostPipeline::Entry* HostPipeline::Find(uint64_t idx)
{
    for (auto& e : m_q) if (e.idx == idx) return &e;
    return nullptr;
}

// the newest pushed frame is b of the engine's pair; a push is contiguous when it follows the last pushed frame in order
bool HostPipeline::Real(Entry& a, std::string& err)
{
    if (m_realIdx == a.idx) return true;
    if (!(m_pushed && m_pb == a.idx)) {
        bool contiguous = false;
        if (m_pushed && !a.disc)
            for (size_t i = 1; i < m_q.size(); ++i) if (&m_q[i] == &a) { contiguous = m_q[i - 1].idx == m_pb; break; }
        if (!m_eng->Push(a.pic, contiguous, err)) return false;
        ++m_pushes;
        m_havePair = contiguous; m_pa = m_pb; m_pb = a.idx; m_pushed = true;
        m_haveOut = false;
    }
    if (!m_eng->RenderReal(m_real, err)) return false;
    m_realIdx = a.idx;
    return true;
}

const I420* HostPipeline::Get(const Pick& p, std::string& err)
{
    Entry* a = Find(p.a);
    if (!a) { err = "frame gone"; return nullptr; }
    const bool enh = m_eng->EnhancesReal();
    if (!p.generated()) {
        if (!enh) return &a->pic;
        if (m_pushed && a->idx < m_pb) return &a->pic;   // behind the engine (after a jump back): the captured picture
        return Real(*a, err) ? &m_real : nullptr;
    }
    Entry* b = Find(p.b);
    if (!b) return enh ? (Real(*a, err) ? &m_real : nullptr) : &a->pic;
    if (m_haveOut && m_oa == p.a && m_ob == p.b && m_ot == p.t) return &m_out;
    // the engine's pair must be (a, b): the next pair in order needs one push, any other two
    if (!(m_havePair && m_pa == p.a && m_pb == p.b)) {
        if (!(m_pushed && m_pb == p.a)) {
            if (!m_eng->Push(a->pic, false, err)) return nullptr;
            ++m_pushes;
        }
        if (!m_eng->Push(b->pic, true, err)) return nullptr;
        ++m_pushes;
        m_havePair = true; m_pushed = true; m_pa = p.a; m_pb = p.b;
        m_haveOut = false;
        m_realIdx = ~0ull;
    }
    if (!m_eng->Render(p.t, m_out, err)) return nullptr;
    m_haveOut = true; m_oa = p.a; m_ob = p.b; m_ot = p.t;
    ++m_generated;
    return &m_out;
}

} // namespace pmc
