// MlBackend (G15, Windows on Arm and any build with pmml): PushFlow (RIFE) through pmml behind pmeng, for frames in host
// memory. pmml runs the network on the best ONNX Runtime EP of this machine: on Snapdragon the QNN EP (the Hexagon NPU,
// "qnn", or the Adreno GPU, "qnn-gpu"), elsewhere what the runtime folder offers (vsplugin/src/ml_vs.cpp, the same rules).
// Auto: pmml's measured candidates for this card and size (pmml-autobench.json, `pmml_cli auto`) pick the EP, the tier,
// flow once and the multiplier within the frame budget; without a measurement the first accelerated EP in quality order,
// Fast, x2 (x the display's multiple when smoother= / display_fps= ask for more and flow once makes it affordable).
// The output keeps the input's format and size; real frames are copied unchanged. A hard cut shows the nearer real frame
// (ml_backend.h LooksLikeCut: RIFE has no cut awareness of its own).
// Where pmml lives: option ml_runtime, else the MlRuntime setting, else PMML_RUNTIME, else %LOCALAPPDATA%\PushMPC\pmml
// (Setup: ort\core, eps\qnn, graphs\, models\).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>
#include "engine.h"
#include "ml_backend.h"        // Planes16, LooksLikeCut
#include "platform.h"          // ReadSetting, EnvA, ParseSmoother
#include "pmml/autobench.h"
#include "pmml/pmml.h"

namespace pme {
namespace {

using pmvs::ml::Planes16;

bool Is10(uint32_t f) { return f == PMENG_FMT_YUV420P10 || f == PMENG_FMT_P010; }
bool IsSemi(uint32_t f) { return f == PMENG_FMT_NV12 || f == PMENG_FMT_P010; }

bool FileThere(const std::string& p) { if (FILE* f = std::fopen(p.c_str(), "rb")) { std::fclose(f); return true; } return false; }
bool RuntimePresent(const std::string& d) { return !d.empty() && FileThere(d + "/graphs/synth-yuv.onnx"); }

std::string RuntimeDir(const Options& o)
{
    std::string d = Opt(o, "ml_runtime");
    if (d.empty()) d = pmvs::ReadSetting("MlRuntime");
    if (d.empty()) d = pmvs::EnvA("PMML_RUNTIME");
    if (d.empty()) {
#ifdef _WIN32
        const std::string base = pmvs::EnvA("LOCALAPPDATA");
        if (!base.empty() && RuntimePresent(base + "\\PushMPC\\pmml")) d = base + "\\PushMPC\\pmml";
#else
        const std::string xdg = pmvs::EnvA("XDG_DATA_HOME"), home = pmvs::EnvA("HOME");
        for (const std::string& c : { !xdg.empty() && xdg[0] == '/' ? xdg + "/pushmpc/pmml" : std::string(), home.empty() ? std::string() : home + "/.local/share/pushmpc/pmml" })
            if (d.empty() && RuntimePresent(c)) d = c;
#endif
    }
    return d;
}

// pmml's measured candidates for this size (the autobench cache; any device entry of this size)
std::vector<pmml::ProbeResult> MeasuredAt(const std::string& file, int w, int h)
{
    std::vector<pmml::ProbeResult> r;
    std::string text;
    if (FILE* f = std::fopen(file.c_str(), "rb")) {
        char buf[4096]; size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
        std::fclose(f);
    }
    const std::string suffix = " " + std::to_string(w) + "x" + std::to_string(h);
    for (size_t p = 0; (p = text.find(suffix + "\"", p)) != std::string::npos; ++p) {
        const size_t q = text.rfind('"', p);
        if (q == std::string::npos) continue;
        if (pmml::cacheLoad(file, text.substr(q + 1, p + suffix.size() - q - 1), &r)) return r;
    }
    return r;
}
// this size, else the smallest of the sizes Setup measures (pmml_cli auto at 720p, 1080p, 1440p, 4K) that has at least as
// many pixels: the cost grows with the picture, so a larger measurement is the safe side; sizeOut = the one used
std::vector<pmml::ProbeResult> Measured(const std::string& file, int w, int h, std::string& sizeOut)
{
    auto r = MeasuredAt(file, w, h);
    if (!r.empty()) { sizeOut = std::to_string(w) + "x" + std::to_string(h); return r; }
    static const int sizes[][2] = { { 1280, 720 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 } };
    for (const auto& s : sizes) {
        if (int64_t(s[0]) * s[1] < int64_t(w) * h) continue;
        r = MeasuredAt(file, s[0], s[1]);
        if (!r.empty()) { sizeOut = std::to_string(s[0]) + "x" + std::to_string(s[1]); return r; }
    }
    return r;
}

class MlBackend : public Backend {
public:
    bool Init(const pmeng_stream& st, const Options& o, pmeng_output& out, bool explicitChoice, std::string& why)
    {
        m_fmt = st.fmt;
        if (m_fmt != PMENG_FMT_YUV420P && m_fmt != PMENG_FMT_YUV420P10 && m_fmt != PMENG_FMT_NV12 && m_fmt != PMENG_FMT_P010) {
            why = "the ml engine takes YUV 4:2:0 8 / 10 bit (planar, NV12, P010)";
            return false;
        }
        m_w = int(st.width); m_h = int(st.height);
        if ((m_w & 1) || (m_h & 1)) { why = "the ml engine needs an even size"; return false; }
        m_bits = Is10(m_fmt) ? 10 : 8;
        const std::string rt = RuntimeDir(o);
        if (!RuntimePresent(rt)) { why = "no pmml runtime (graphs\\synth-yuv.onnx) in '" + rt + "'"; return false; }
        if (!pmvs::ParseBool(Opt(o, "enable", "1"), true)) { why = "PushMPC is off (enable=0)"; return false; }

        const double srcFps = st.fps_num > 0 && st.fps_den > 0 ? double(st.fps_num) / st.fps_den : 23.976;
        const double displayHz = std::atof(Opt(o, "display_fps", "0").c_str());
        const int fixedK = Opt(o, "smoother").empty() ? 0 : pmvs::ParseSmoother(Opt(o, "smoother"), 0);
        const int wantK = fixedK >= 2 ? std::min(fixedK, 8) : displayHz > 0 ? std::clamp(int(std::floor(displayHz / srcFps + 0.02)), 1, 8) : 2;
        if (wantK <= 1) { why = "x1 at this display rate: nothing to generate"; return false; }

        pmml::Options po;
        po.runtimeDir = rt;
        // PushFlow's models: option ml_models, else <runtime>\models, else the MlModels setting (Windows Setup points it at
        // the installed runtime's PushFlow models, runtime\runtime\experimental\pushflow\models, instead of a second copy)
        po.modelsDir = Opt(o, "ml_models");
        if (po.modelsDir.empty()) po.modelsDir = rt + "/models";
        if (Opt(o, "ml_models").empty() && !FileThere(po.modelsDir + "/pushflow-fast.onnx") && !pmvs::ReadSetting("MlModels").empty()) po.modelsDir = pmvs::ReadSetting("MlModels");
        po.graphsDir = rt + "/graphs";
        po.ncnnModelsDir = rt + "/ncnn-models";
        po.cacheDir = rt;
        po.width = m_w; po.height = m_h;
        // FFmpeg's AVColorSpace: 5 / 6 BT.601, 9 / 10 BT.2020, else BT.709; AVColorRange 2 = full
        const float kr = st.matrix == 5 || st.matrix == 6 ? 0.299f : st.matrix == 9 || st.matrix == 10 ? 0.2627f : 0.2126f;
        const float kb = st.matrix == 5 || st.matrix == 6 ? 0.114f : st.matrix == 9 || st.matrix == 10 ? 0.0593f : 0.0722f;
        po.colour = st.range == 2 ? pmml::Colour::full(m_bits, kr, kb) : pmml::Colour::limited(m_bits, kr, kb);

        int k = wantK;
        std::string decision;
        const std::string reqBackend = Opt(o, "ml_backend"), reqTier = Opt(o, "ml_tier");
        std::string measuredAt;
        const auto results = Measured(rt + "/pmml-autobench.json", m_w, m_h, measuredAt);
        if (reqBackend.empty() && !results.empty()) {
            const auto c = pmml::choose(results, srcFps, wantK);
            if (c.ok) { po.backend = c.candidate.backend; po.tier = c.candidate.tier; m_flowOnce = c.candidate.flowOnce; k = fixedK >= 2 ? wantK : c.multiplier; decision = "Auto (measured at " + measuredAt + "): " + c.why; }
            else if (!explicitChoice) { why = "pmml's measured candidates do not fit this clip's frame budget (" + c.why + ")"; return false; }
        }
        if (decision.empty()) {
            if (!reqBackend.empty()) {
                if (!pmml::Backend::parse(reqBackend, &po.backend)) { why = "ml_backend '" + reqBackend + "' is not a pmml backend"; return false; }
                po.tier = reqTier.empty() ? "balanced" : reqTier;
                decision = "ml_backend " + po.backend.key() + " (your setting), x" + std::to_string(k);
            } else {
                // Auto never runs an unmeasured network in playback (it could miss the frame budget): the next engine runs and
                // Setup's pmml_cli auto measures this card. backend=ml without a measurement: the first accelerated EP this
                // runtime offers, in quality order (Snapdragon: QNN on the Adreno GPU, then the Hexagon NPU)
                if (!explicitChoice) { why = "PushFlow is not measured on this card yet (pmml_cli auto)"; return false; }
                std::string log;
                const auto devs = pmml::listDevices(rt, &log);
                static const char* order[] = { "trt-rtx", "dml", "amdgpu", "openvino", "migraphx", "coreml", "qnn-gpu", "qnn", "vulkan" };
                bool found = false;
                for (const char* ep : order) {
                    auto it = std::find_if(devs.begin(), devs.end(), [&](const pmml::DeviceInfo& d) { return d.backend.ep == ep; });
                    if (it != devs.end()) { po.backend = it->backend; found = true; break; }
                }
                if (!found) po.backend = pmml::Backend{};   // the CPU EP (asked for by name)
                po.tier = "fast";
                k = fixedK >= 2 ? wantK : 2;
                decision = "backend=ml, not measured yet: " + po.backend.key() + ", Fast, x" + std::to_string(k);
            }
            m_flowOnce = k > 2;
        }
        if (!reqTier.empty()) po.tier = reqTier;
        if (const std::string v = Opt(o, "ml_flow_once"); !v.empty()) m_flowOnce = pmvs::ParseBool(v, m_flowOnce);
        std::string e;
        m_interp = pmml::Interpolator::create(po, &e);
        if (!m_interp) { why = "pmml: " + e; return false; }
        m_k = std::clamp(k, 2, 8);
        m_cur.w = m_prev.w = m_out.w = m_w; m_cur.h = m_prev.h = m_out.h = m_h;
        for (int i = 0; i < 3; ++i) {
            const size_t n = i ? size_t(m_w / 2) * (m_h / 2) : size_t(m_w) * m_h;
            m_cur.p[i].resize(n); m_prev.p[i].resize(n); m_out.p[i].resize(n);
        }
        out.mem = PMENG_MEM_CPU;
        out.fmt = m_fmt;
        out.width = st.width; out.height = st.height;
        out.multiplier = uint32_t(m_k);
        out.range = st.range; out.matrix = st.matrix; out.primaries = st.primaries; out.transfer = st.transfer;
        out.keep_dynamic_hdr = 1;
        std::snprintf(out.engine, sizeof(out.engine), "ml");
        std::snprintf(out.description, sizeof(out.description), "PushFlow through pmml: %s, tier %s, %s, x%d, %dx%d %d-bit; %s",
                      m_interp->describe().c_str(), po.tier.c_str(), m_flowOnce ? "flow once" : "network per timestep", m_k, m_w, m_h, m_bits, decision.c_str());
        Log(std::string("ml: ") + out.description);
        return true;
    }

    int Submit(const pmeng_image& src, int64_t, uint32_t flags) override
    {
        if (!m_q.empty()) return PMENG_BUSY;
        if (src.mem != PMENG_MEM_CPU || src.fmt != m_fmt) { err = "ml engine: unexpected input memory / format"; return PMENG_ERROR; }
        std::swap(m_prev, m_cur);
        In(src, m_cur);
        const bool contiguous = !(flags & PMENG_SUBMIT_DISCONTINUITY) && m_seq > 0;
        m_pairCut = false;
        if (contiguous) {
            m_pairCut = pmvs::ml::LooksLikeCut(m_prev, m_cur, m_bits);
            if (m_pairCut) ++m_cuts;
            else {
                pmml::Frame A, B;
                for (int i = 0; i < 3; ++i) {
                    A.plane[i] = m_prev.p[i].data(); B.plane[i] = m_cur.p[i].data();
                    A.stride[i] = B.stride[i] = i ? m_w / 2 : m_w;
                }
                const auto t0 = std::chrono::steady_clock::now();
                std::string e;
                if (!m_interp->setPair(A, B, m_flowOnce ? m_k - 1 : 1, &e)) { err = "pmml: " + e; return PMENG_ERROR; }
                m_pairMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            }
            for (int j = 1; j < m_k; ++j) m_q.push_back({ m_seq, uint32_t(j), uint32_t(m_k), 1 });
        }
        m_q.push_back({ m_seq, 0, uint32_t(m_k), 0 });
        ++m_seq;
        return PMENG_OK;
    }
    int Peek(pmeng_out_info& info, uint32_t) override
    {
        if (m_q.empty()) return 0;
        info = m_q.front();
        return 1;
    }
    int Write(const pmeng_image& dst) override
    {
        if (m_q.empty()) { err = "write without an output"; return PMENG_ERROR; }
        const pmeng_out_info oi = m_q.front();
        m_q.pop_front();
        if (dst.mem != PMENG_MEM_CPU || dst.fmt != m_fmt) { err = "ml engine: unexpected output memory / format"; return PMENG_ERROR; }
        if (oi.subframe == 0) { OutPlanes(m_cur, dst); ++m_written; return PMENG_OK; }
        const float t = float(oi.subframe) / float(oi.multiplier);
        if (m_pairCut) { OutPlanes(t < 0.5f ? m_prev : m_cur, dst); ++m_written; ++m_generated; return PMENG_OK; }
        pmml::OutFrame O;
        for (int i = 0; i < 3; ++i) { O.plane[i] = m_out.p[i].data(); O.stride[i] = i ? m_w / 2 : m_w; }
        const auto t0 = std::chrono::steady_clock::now();
        std::string e;
        if (!m_interp->render(t, O, &e)) { err = "pmml: " + e; return PMENG_ERROR; }
        m_renderMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        OutPlanes(m_out, dst);
        ++m_written; ++m_generated;
        return PMENG_OK;
    }
    int Skip() override { if (!m_q.empty()) m_q.pop_front(); return PMENG_OK; }
    uint32_t Pending() override { return uint32_t(m_q.size()); }
    int Flush() override { m_q.clear(); m_seq = 0; return PMENG_OK; }
    std::string Stats() override
    {
        char b[320];
        const auto s = m_interp->stats();
        std::snprintf(b, sizeof(b), "ml: %llu sources, %llu outputs (%llu generated), %llu scene cuts; pair %.2f ms (network %.2f), each generated frame %.2f ms; %llu network runs",
                      (unsigned long long)m_seq, (unsigned long long)m_written, (unsigned long long)m_generated, (unsigned long long)m_cuts, m_pairMs, s.netMs, m_renderMs,
                      (unsigned long long)s.networkRuns);
        return b;
    }

private:
    // the caller's format -> 16-bit planes (codes 0 .. (1 << bits) - 1; P010's MSB alignment removed)
    void In(const pmeng_image& s, Planes16& p) const
    {
        const int cw = m_w / 2, ch = m_h / 2;
        for (int y = 0; y < m_h; ++y) {
            const uint8_t* row = s.data[0] + s.linesize[0] * y;
            uint16_t* d = &p.p[0][size_t(y) * m_w];
            if (m_bits == 8) for (int x = 0; x < m_w; ++x) d[x] = row[x];
            else if (IsSemi(m_fmt)) { const uint16_t* r = reinterpret_cast<const uint16_t*>(row); for (int x = 0; x < m_w; ++x) d[x] = uint16_t(r[x] >> 6); }
            else std::memcpy(d, row, size_t(m_w) * 2);
        }
        for (int y = 0; y < ch; ++y) {
            uint16_t* u = &p.p[1][size_t(y) * cw]; uint16_t* v = &p.p[2][size_t(y) * cw];
            if (IsSemi(m_fmt)) {
                const uint8_t* row = s.data[1] + s.linesize[1] * y;
                if (m_bits == 8) for (int x = 0; x < cw; ++x) { u[x] = row[2 * x]; v[x] = row[2 * x + 1]; }
                else { const uint16_t* r = reinterpret_cast<const uint16_t*>(row); for (int x = 0; x < cw; ++x) { u[x] = uint16_t(r[2 * x] >> 6); v[x] = uint16_t(r[2 * x + 1] >> 6); } }
            } else {
                const uint8_t* ru = s.data[1] + s.linesize[1] * y; const uint8_t* rv = s.data[2] + s.linesize[2] * y;
                if (m_bits == 8) for (int x = 0; x < cw; ++x) { u[x] = ru[x]; v[x] = rv[x]; }
                else { std::memcpy(u, ru, size_t(cw) * 2); std::memcpy(v, rv, size_t(cw) * 2); }
            }
        }
    }
    // 16-bit planes -> the caller's format
    void OutPlanes(const Planes16& p, const pmeng_image& d) const
    {
        const int cw = m_w / 2, ch = m_h / 2;
        for (int y = 0; y < m_h; ++y) {
            uint8_t* row = d.data[0] + d.linesize[0] * y;
            const uint16_t* s = &p.p[0][size_t(y) * m_w];
            if (m_bits == 8) for (int x = 0; x < m_w; ++x) row[x] = uint8_t(std::min<uint16_t>(s[x], 255));
            else if (IsSemi(m_fmt)) { uint16_t* r = reinterpret_cast<uint16_t*>(row); for (int x = 0; x < m_w; ++x) r[x] = uint16_t(s[x] << 6); }
            else std::memcpy(row, s, size_t(m_w) * 2);
        }
        for (int y = 0; y < ch; ++y) {
            const uint16_t* u = &p.p[1][size_t(y) * cw]; const uint16_t* v = &p.p[2][size_t(y) * cw];
            if (IsSemi(m_fmt)) {
                uint8_t* row = d.data[1] + d.linesize[1] * y;
                if (m_bits == 8) for (int x = 0; x < cw; ++x) { row[2 * x] = uint8_t(std::min<uint16_t>(u[x], 255)); row[2 * x + 1] = uint8_t(std::min<uint16_t>(v[x], 255)); }
                else { uint16_t* r = reinterpret_cast<uint16_t*>(row); for (int x = 0; x < cw; ++x) { r[2 * x] = uint16_t(u[x] << 6); r[2 * x + 1] = uint16_t(v[x] << 6); } }
            } else {
                uint8_t* ru = d.data[1] + d.linesize[1] * y; uint8_t* rv = d.data[2] + d.linesize[2] * y;
                if (m_bits == 8) for (int x = 0; x < cw; ++x) { ru[x] = uint8_t(std::min<uint16_t>(u[x], 255)); rv[x] = uint8_t(std::min<uint16_t>(v[x], 255)); }
                else { std::memcpy(ru, u, size_t(cw) * 2); std::memcpy(rv, v, size_t(cw) * 2); }
            }
        }
    }

    std::unique_ptr<pmml::Interpolator> m_interp;
    uint32_t m_fmt = 0;
    int m_w = 0, m_h = 0, m_bits = 8, m_k = 2;
    bool m_flowOnce = false, m_pairCut = false;
    uint64_t m_seq = 0, m_written = 0, m_generated = 0, m_cuts = 0;
    double m_pairMs = 0, m_renderMs = 0;
    Planes16 m_prev, m_cur, m_out;
    std::deque<pmeng_out_info> m_q;
};

} // namespace

Backend* OpenMl(const pmeng_stream& st, const Options& o, pmeng_output& out, bool explicitChoice, std::string& why)
{
    auto* b = new MlBackend();
    if (!b->Init(st, o, out, explicitChoice, why)) { delete b; return nullptr; }
    return b;
}

} // namespace pme
