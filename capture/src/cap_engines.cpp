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
        c.width = r.w; c.height = r.h; c.bits = 8; c.quality = Quality();
        if (!m_e.Configure(c, why)) return false;
        const auto cal = pmvs::portable::Calibrate(r.w, r.h, 8, m_e.Threads(), c.quality);
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
        v.width = f.w; v.height = f.h; v.bits = 8;
        v.data[0] = f.y.data(); v.data[1] = f.u.data(); v.data[2] = f.v.data();
        v.stride[0] = f.w; v.stride[1] = v.stride[2] = f.cw();
        return m_e.Push(v, contiguous, err);
    }
    bool Render(float t, I420& out, std::string& err) override
    {
        if (!m_e.HavePair()) { err = "no pair"; return false; }
        const auto& c = m_e.Cfg();
        if (out.w != c.width || out.h != c.height) out.Resize(c.width, c.height);
        pmvs::portable::YuvOut o;
        o.data[0] = out.y.data(); o.data[1] = out.u.data(); o.data[2] = out.v.data();
        o.stride[0] = out.w; o.stride[1] = o.stride[2] = out.cw();
        m_e.Generate(t, o);
        return true;
    }
private:
    pmvs::portable::Engine m_e;
    int m_steps = 0;
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
        o.colour = pmml::Colour::limited(8);
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
        if (out.w != m_w || out.h != m_h) out.Resize(m_w, m_h);
        std::vector<uint8_t>* dst[3] = { &out.y, &out.u, &out.v };
        for (int i = 0; i < 3; ++i) for (size_t k = 0; k < m_o[i].size() && k < dst[i]->size(); ++k) (*dst[i])[k] = uint8_t(std::min<uint16_t>(255, m_o[i][k]));
        return true;
    }
private:
    struct P16 { std::vector<uint16_t> p[3]; };
    void To16(const I420& f, P16& o) const
    {
        const std::vector<uint8_t>* s[3] = { &f.y, &f.u, &f.v };
        for (int i = 0; i < 3; ++i) { o.p[i].resize(s[i]->size()); for (size_t k = 0; k < s[i]->size(); ++k) o.p[i][k] = (*s[i])[k]; }
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
    int m_cur = 0, m_w = 0, m_h = 0, m_phases = 1;
    bool m_have = false, m_seen = false;
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
        c.width = r.w; c.height = r.h; c.bits = 8; c.k = k; c.sr = "off"; c.srcFps = fps;
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
        v.width = f.w; v.height = f.h; v.bits = 8;
        v.data[0] = f.y.data(); v.data[1] = f.u.data(); v.data[2] = f.v.data();
        v.stride[0] = f.w; v.stride[1] = v.stride[2] = f.cw();
        m_gen.resize(size_t(std::max(0, m_k - 1)));
        std::vector<pmvs::apple::PlanarOut> outs(m_gen.size());
        for (size_t i = 0; i < m_gen.size(); ++i) {
            if (m_gen[i].w != m_w || m_gen[i].h != m_h) m_gen[i].Resize(m_w, m_h);
            outs[i].data[0] = m_gen[i].y.data(); outs[i].data[1] = m_gen[i].u.data(); outs[i].data[2] = m_gen[i].v.data();
            outs[i].stride[0] = m_w; outs[i].stride[1] = outs[i].stride[2] = m_gen[i].cw();
        }
        if (m_cur.w != m_w || m_cur.h != m_h) m_cur.Resize(m_w, m_h);
        pmvs::apple::PlanarOut cur;
        cur.data[0] = m_cur.y.data(); cur.data[1] = m_cur.u.data(); cur.data[2] = m_cur.v.data();
        cur.stride[0] = m_w; cur.stride[1] = cur.stride[2] = m_cur.cw();
        pmvs::apple::Colour col;   // BT.709 limited
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
    int m_k = 2, m_w = 0, m_h = 0;
    int64_t m_pts = 0;
    bool m_have = false;
    std::string m_desc;
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

std::unique_ptr<HostEngine> OpenHostEngine(const std::string& name, const EngineRequest& r, std::string& why)
{
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

const I420* HostPipeline::Get(const Pick& p, std::string& err)
{
    Entry* a = Find(p.a);
    if (!a) { err = "frame gone"; return nullptr; }
    if (!p.generated()) return &a->pic;
    Entry* b = Find(p.b);
    if (!b) return &a->pic;
    if (m_haveOut && m_oa == p.a && m_ob == p.b && m_ot == p.t) return &m_out;
    // the engine's pair must be (a, b): the next pair in order needs one push, any other two
    if (!(m_havePair && m_pa == p.a && m_pb == p.b)) {
        if (!(m_havePair && m_pb == p.a)) {
            if (!m_eng->Push(a->pic, false, err)) return nullptr;
            ++m_pushes;
        }
        if (!m_eng->Push(b->pic, true, err)) return nullptr;
        ++m_pushes;
        m_havePair = true; m_pa = p.a; m_pb = p.b;
        m_haveOut = false;
    }
    if (!m_eng->Render(p.t, m_out, err)) return nullptr;
    m_haveOut = true; m_oa = p.a; m_ob = p.b; m_ot = p.t;
    ++m_generated;
    return &m_out;
}

} // namespace pmc
