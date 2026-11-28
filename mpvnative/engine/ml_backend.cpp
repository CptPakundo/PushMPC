// MlBackend: PushFlow (RIFE) on the GPU through pmml (ONNX Runtime with the card's EP: TensorRT-RTX, OpenVINO,
// MIGraphX, WebGPU; or ncnn on Vulkan) behind pmeng, for frames in host memory: the native hook in distro mpv and the
// libmpv players, and Plex. Its own module, libpushmpc_engine_ml.so (pmml and ncnn linked in, ONNX Runtime loaded at
// run time from the pmml runtime folder setup.sh installs), which libpushmpc_engine.so loads next to itself.
//
// Auto, as the VapourSynth route's ml backend: the autobench cache (pmml_cli auto: every runtime x EP x tier measured in
// its own process) decides backend, tier, flow once and the multiplier within the frame budget. With no measurement for
// this size yet, the first GPU EP in quality order runs its Fast tier, timed here on two synthetic pictures at the real
// size (per timestep and flow once), and pmml::choose picks the multiplier from that; pmml_cli auto is then started in
// the background (once per size) so the next open has the full measurement. The output keeps the stream's format;
// real frames pass unchanged; a hard cut shows the nearer real frame (RIFE has no cut awareness).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dlfcn.h>
#include "engine.h"
#include "platform.h"
#include "backend.h"   // MlWantK / MlAutoK: the multiplier asked of pmml and Auto's pick from its measurement (every PushFlow route)
#include "yuv_io.h"
#include "pmml/autobench.h"
#include "pmml/pmml.h"

extern char** environ;

namespace pme {
namespace {

using namespace pmvs;

bool Present(const std::string& dir)
{
    if (dir.empty()) return false;
    struct stat sb{};
    return stat((dir + "/graphs/synth-yuv.onnx").c_str(), &sb) == 0;
}
std::string Env(const char* n) { const char* v = std::getenv(n); return v ? v : ""; }

// where pmml lives: the ml_runtime option, the MlRuntime setting, PMML_RUNTIME, setup.sh's folder
std::string RuntimeDir(const Options& o)
{
    std::string d = Opt(o, "ml_runtime");
    if (d.empty()) d = ReadSetting("MlRuntime");
    if (d.empty()) d = Env("PMML_RUNTIME");
    if (d.empty()) {
        const std::string xdg = Env("XDG_DATA_HOME"), home = Env("HOME");
        for (const std::string& def : { !xdg.empty() && xdg[0] == '/' ? xdg + "/pushmpc/pmml" : std::string(), home.empty() ? std::string() : home + "/.local/share/pushmpc/pmml" })
            if (d.empty() && Present(def)) d = def;
    }
    return d;
}

std::string ModuleDir()
{
    Dl_info di{};
    if (!dladdr(reinterpret_cast<void*>(&ModuleDir), &di) || !di.dli_fname) return {};
    std::string p = di.dli_fname;
    return p.substr(0, p.rfind('/') + 1);
}

// the measurements of this size in the autobench cache (any card entry: `pmml_cli auto` writes one per card + driver)
std::vector<pmml::ProbeResult> CachedResults(const std::string& file, int w, int h)
{
    std::vector<pmml::ProbeResult> r;
    const std::string suffix = " " + std::to_string(w) + "x" + std::to_string(h);
    std::string text;
    if (FILE* f = std::fopen(file.c_str(), "rb")) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
        std::fclose(f);
    }
    for (size_t p = 0; (p = text.find(suffix + "\"", p)) != std::string::npos; ++p) {
        const size_t q = text.rfind('"', p);
        if (q == std::string::npos) continue;
        if (pmml::cacheLoad(file, text.substr(q + 1, p + suffix.size() - q - 1), &r)) return r;
    }
    return r;
}

// pmml_cli auto for this size, detached, once (a marker per size; PUSHMPC_NO_AUTOBENCH=1 never)
void StartAutobench(const std::string& rt, int w, int h, double fps, int wantK)
{
    if (Env("PUSHMPC_NO_AUTOBENCH") == "1") return;
    std::string cli;
    for (const std::string& c : { rt + "/pmml_cli", ModuleDir() + "pmml_cli" })
        if (access(c.c_str(), X_OK) == 0) { cli = c; break; }
    if (cli.empty()) { Log("ml: no pmml_cli next to the runtime: Auto keeps the quick measurement"); return; }
    const std::string size = std::to_string(w) + "x" + std::to_string(h);
    const std::string marker = rt + "/pmml-autobench-" + size + ".started";
    struct stat sb{};
    if (stat(marker.c_str(), &sb) == 0 && std::time(nullptr) - sb.st_mtime < 24 * 3600) return;
    if (FILE* f = std::fopen(marker.c_str(), "wb")) std::fclose(f);
    char fpsText[32], mult[8];
    std::snprintf(fpsText, sizeof(fpsText), "%.3f", fps);
    std::snprintf(mult, sizeof(mult), "%d", std::max(2, wantK));
    const std::string ncnn = rt + "/ncnn-models", log = rt + "/pmml-autobench-" + size + ".log";
    std::vector<std::string> args = { cli, "auto", "--runtime", rt, "--cache", rt, "--size", size, "--fps", fpsText, "--mult", mult };
    if (stat(ncnn.c_str(), &sb) == 0) { args.push_back("--ncnn-models"); args.push_back(ncnn); }
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    // a grandchild, so the player never has to reap it; its output to the log next to the cache; lowest priority
    const pid_t c = fork();
    if (c == 0) {
        if (fork() == 0) {
            setsid();
            const int fd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
            if (nice(19) == -1) {}
            execv(cli.c_str(), argv.data());
            _exit(127);
        }
        _exit(0);
    }
    if (c > 0) waitpid(c, nullptr, 0);
    Log("ml: pmml_cli auto started for " + size + " (results in " + rt + "/pmml-autobench.json)");
}

// a moving gradient at the real size (Planes16 layout)
void Synthetic(std::vector<uint16_t> (&p)[3], int w, int h, int bits, int shift)
{
    const int cw = w / 2, ch = h / 2, mx = (1 << bits) - 1;
    p[0].resize(size_t(w) * h); p[1].assign(size_t(cw) * ch, uint16_t(1 << (bits - 1))); p[2].assign(size_t(cw) * ch, uint16_t(1 << (bits - 1)));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) p[0][size_t(y) * w + x] = uint16_t(((x + shift) * 7 + y * 3) % (mx + 1));
}

class MlBackend : public Backend {
public:
    bool Init(const pmeng_stream& st, const Options& o, pmeng_output& out, std::string& why)
    {
        if (!yuv::Supported(st.fmt)) { why = "the ml engine takes YUV 4:2:0 8 / 10 bit (planar, NV12, P010)"; return false; }
        if ((st.width & 1) || (st.height & 1)) { why = "the ml engine needs an even picture size"; return false; }
        m_fmt = st.fmt; m_w = int(st.width); m_h = int(st.height); m_bits = yuv::Ten(st.fmt) ? 10 : 8;
        const std::string rt = RuntimeDir(o);
        if (!Present(rt)) { why = "the pmml runtime is not installed (setup.sh installs it; graphs/synth-yuv.onnx in '" + rt + "')"; return false; }
        PortableSettings ps = LoadPortableSettings([](const char* n) { return ReadSetting(n); });
        if (!Opt(o, "smoother").empty()) ps.smoother = ParseSmoother(Opt(o, "smoother"), ps.smoother);
        if (!Opt(o, "enable").empty()) ps.enable = ParseBool(Opt(o, "enable"), ps.enable);
        if (!ps.enable) { why = "PushMPC is off (Enable = 0)"; return false; }
        if (ps.smoother == 1) { why = "Smoother off"; return false; }
        const double fps = st.fps_num > 0 && st.fps_den > 0 ? double(st.fps_num) / st.fps_den : 23.976;
        const double displayHz = std::atof(Opt(o, "display_fps", "0").c_str());
        const int fixedK = ps.smoother >= 2 ? std::min(ps.smoother, kPortableMaxK) : 0;
        const int wantK = MlWantK(fixedK, fps, displayHz);
        if (wantK <= 1) { why = "the display is not faster than the source: x1"; return false; }

        pmml::Options po;
        po.runtimeDir = rt; po.modelsDir = Opt(o, "ml_models", rt + "/models"); po.graphsDir = rt + "/graphs";
        po.ncnnModelsDir = rt + "/ncnn-models"; po.cacheDir = rt;
        po.width = m_w; po.height = m_h;
        const int matrix = st.matrix > 0 && st.matrix != 2 ? st.matrix : (m_w < 1024 && m_h < 600 ? 6 : m_bits > 8 && m_w >= 3200 ? 9 : 1);
        const float kr = matrix == 5 || matrix == 6 ? 0.299f : matrix == 9 ? 0.2627f : 0.2126f, kb = matrix == 5 || matrix == 6 ? 0.114f : matrix == 9 ? 0.0593f : 0.0722f;
        po.colour = st.range == 2 ? pmml::Colour::full(m_bits, kr, kb) : pmml::Colour::limited(m_bits, kr, kb);

        std::string decision;
        const std::string reqBackend = Opt(o, "ml_backend"), reqTier = Opt(o, "ml_tier");
        const auto cached = CachedResults(rt + "/pmml-autobench.json", m_w, m_h);
        if (reqBackend.empty() && !cached.empty()) {
            const auto c = pmml::choose(cached, fps, wantK);
            if (c.ok) { po.backend = c.candidate.backend; po.tier = c.candidate.tier; m_flowOnce = c.candidate.flowOnce; m_k = MlAutoK(fixedK, c.multiplier, fps, displayHz); decision = "Auto (measured by pmml_cli auto): " + c.why; }
            else decision.clear();
        }
        bool quick = false;
        if (decision.empty()) {
            if (!reqBackend.empty()) {
                if (!pmml::Backend::parse(reqBackend, &po.backend)) { why = "ml_backend '" + reqBackend + "' is not a pmml backend"; return false; }
            } else {
                std::string log;
                const auto devs = pmml::listDevices(rt, &log);
                static const char* order[] = { "trt-rtx", "openvino", "migraphx", "vulkan", "webgpu", "amdgpu", "cpu" };
                bool found = false;
                for (const char* ep : order) {
                    auto it = std::find_if(devs.begin(), devs.end(), [&](const pmml::DeviceInfo& d) { return d.backend.ep == ep && d.type != "CPU"; });
                    if (it != devs.end()) { po.backend = it->backend; found = true; break; }
                }
                if (!found) { why = "pmml finds no GPU here (" + log + ")"; return false; }
            }
            po.tier = reqTier.empty() ? "fast" : reqTier;
            quick = true;
        }
        if (!reqTier.empty()) po.tier = reqTier;
        std::string e;
        m_interp = pmml::Interpolator::create(po, &e);
        if (!m_interp) { why = "pmml (" + po.backend.key() + "): " + e; return false; }
        if (quick) {
            // the quick measurement: per timestep and flow once on two synthetic pictures at the real size
            std::vector<uint16_t> a[3], b[3], o3[3];
            Synthetic(a, m_w, m_h, m_bits, 0); Synthetic(b, m_w, m_h, m_bits, 8);
            for (int i = 0; i < 3; ++i) o3[i].resize(a[i].size());
            pmml::Frame A, B; pmml::OutFrame O;
            for (int i = 0; i < 3; ++i) {
                A.plane[i] = a[i].data(); B.plane[i] = b[i].data(); O.plane[i] = o3[i].data();
                A.stride[i] = B.stride[i] = O.stride[i] = i ? m_w / 2 : m_w;
            }
            auto ms = [](auto t0) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
            double pairPerT = 0, renderPerT = 0, pairOnce = 0, renderOnce = 0;
            for (int it = 0; it < 3; ++it) {   // the first round warms up (engine builds, allocations)
                auto t0 = std::chrono::steady_clock::now();
                if (!m_interp->setPair(A, B, 1, &e)) { why = "pmml: " + e; return false; }
                pairPerT = ms(t0); t0 = std::chrono::steady_clock::now();
                if (!m_interp->render(0.5f, O, &e)) { why = "pmml: " + e; return false; }
                renderPerT = ms(t0); t0 = std::chrono::steady_clock::now();
                if (!m_interp->setPair(A, B, 3, &e)) { why = "pmml: " + e; return false; }
                pairOnce = ms(t0); t0 = std::chrono::steady_clock::now();
                if (!m_interp->render(0.25f, O, &e)) { why = "pmml: " + e; return false; }
                renderOnce = ms(t0);
            }
            std::vector<pmml::ProbeResult> rs(2);
            for (int i = 0; i < 2; ++i) {
                rs[i].ok = true; rs[i].candidate.backend = po.backend; rs[i].candidate.tier = po.tier; rs[i].candidate.flowOnce = i == 1;
                rs[i].packMs = pairPerT; rs[i].synthMs = renderOnce; rs[i].netMs = std::max(0.0, std::max(renderPerT - renderOnce, pairOnce - pairPerT));
            }
            const auto c = pmml::choose(rs, fps, wantK);
            if (!c.ok && !fixedK) { why = "pmml " + po.backend.key() + " is too slow here (" + c.why + ")"; return false; }
            m_k = MlAutoK(fixedK, c.multiplier, fps, displayHz);
            m_flowOnce = c.ok ? c.candidate.flowOnce : m_k > 2;
            char b2[256];
            std::snprintf(b2, sizeof(b2), "Auto (quick measurement: pair %.1f ms, network %.1f ms, rebuild %.1f ms): ", rs[0].packMs, rs[0].netMs, rs[0].synthMs);
            decision = b2 + c.why;
            if (reqBackend.empty()) StartAutobench(rt, m_w, m_h, fps, wantK);
        }
        if (const std::string v = Opt(o, "ml_flow_once"); !v.empty()) m_flowOnce = ParseBool(v, m_flowOnce);
        m_k = std::clamp(m_k, 2, kPortableMaxK);
        Log("ml: " + decision);
        Log("ml: " + m_interp->describe() + (m_flowOnce ? "; flow once" : "; network per timestep"));
        for (int i = 0; i < 2; ++i) m_pic[i].Resize(m_w, m_h, 16);
        m_out.Resize(m_w, m_h, 16);
        m_tmp.Resize(m_w, m_h, m_bits);

        out.mem = PMENG_MEM_CPU; out.fmt = m_fmt; out.width = st.width; out.height = st.height;
        out.multiplier = uint32_t(m_k);
        out.range = st.range; out.matrix = st.matrix; out.primaries = st.primaries; out.transfer = st.transfer;
        out.keep_dynamic_hdr = 1;
        std::snprintf(out.engine, sizeof(out.engine), "ml");
        std::snprintf(out.description, sizeof(out.description), "PushFlow (RIFE) through pmml: %s, %s tier%s, x%d; %s", m_interp->describe().c_str(), po.tier.c_str(),
                      m_flowOnce ? ", flow once" : "", m_k, decision.c_str());
        return true;
    }

    int Submit(const pmeng_image& src, int64_t, uint32_t flags) override
    {
        if (!m_q.empty()) return PMENG_BUSY;
        if (src.mem != PMENG_MEM_CPU || src.fmt != m_fmt) { err = "ml: unexpected input memory / format"; return PMENG_ERROR; }
        const int cur = int(m_seq & 1), prev = cur ^ 1;
        // the frame as 16-bit samples (pmml's layout)
        yuv::ToPlanar(src, m_fmt, m_tmp);
        Widen(m_tmp, m_pic[cur]);
        const bool contiguous = !(flags & PMENG_SUBMIT_DISCONTINUITY) && m_seq > 0;
        m_cut = false;
        if (contiguous) {
            m_cut = LooksLikeCut(m_pic[prev], m_pic[cur]);
            if (m_cut) ++m_cuts;
            else {
                pmml::Frame A, B;
                for (int i = 0; i < 3; ++i) {
                    A.plane[i] = Words(m_pic[prev], i); B.plane[i] = Words(m_pic[cur], i);
                    A.stride[i] = B.stride[i] = i ? m_w / 2 : m_w;
                }
                if (!m_interp->setPair(A, B, m_flowOnce ? m_k - 1 : 1, &err)) return PMENG_ERROR;
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
        if (dst.mem != PMENG_MEM_CPU || dst.fmt != m_fmt) { err = "ml: unexpected output memory / format"; return PMENG_ERROR; }
        const int cur = int((m_seq - 1) & 1), prev = cur ^ 1;
        const float t = float(oi.subframe) / float(oi.multiplier);
        const yuv::Planes* pic = &m_pic[cur];
        if (oi.subframe > 0 && m_cut) pic = t < 0.5f ? &m_pic[prev] : &m_pic[cur];   // never morph a cut
        else if (oi.subframe > 0) {
            pmml::OutFrame O;
            for (int i = 0; i < 3; ++i) { O.plane[i] = Words(m_out, i); O.stride[i] = i ? m_w / 2 : m_w; }
            if (!m_interp->render(t, O, &err)) return PMENG_ERROR;
            pic = &m_out;
            ++m_generated;
        }
        const uint8_t* planes[3] = { pic->p[0].data(), pic->p[1].data(), pic->p[2].data() };
        // 16-bit words holding `m_bits` samples
        yuv::FromPlanes(planes, pic->stride, m_bits, m_w, m_h, m_fmt, dst, 1);
        ++m_written;
        return PMENG_OK;
    }
    int Skip() override { if (!m_q.empty()) m_q.pop_front(); return PMENG_OK; }
    uint32_t Pending() override { return uint32_t(m_q.size()); }
    int Flush() override { m_q.clear(); m_cut = false; return PMENG_OK; }
    std::string Stats() override
    {
        char b[256];
        const auto s = m_interp->stats();
        std::snprintf(b, sizeof(b), "ml: %llu sources, %llu outputs (%llu generated), %llu cuts held; network %.1f ms, rebuild %.1f ms",
                      (unsigned long long)m_seq, (unsigned long long)m_written, (unsigned long long)m_generated, (unsigned long long)m_cuts, s.netMs, s.synthMs);
        return b;
    }

private:
    static uint16_t* Words(yuv::Planes& p, int i) { return reinterpret_cast<uint16_t*>(p.p[i].data()); }
    static const uint16_t* Words(const yuv::Planes& p, int i) { return reinterpret_cast<const uint16_t*>(p.p[i].data()); }
    void Widen(const yuv::Planes& s, yuv::Planes& d) const
    {
        for (int i = 0; i < 3; ++i) {
            const size_t n = d.p[i].size() / 2;
            uint16_t* o = Words(d, i);
            if (s.bits > 8) std::memcpy(o, s.p[i].data(), n * 2);
            else for (size_t x = 0; x < n; ++x) o[x] = s.p[i][x];
        }
    }
    // the mean absolute luma difference of 1/8-size pictures above 12 % of full scale (ml_backend.h LooksLikeCut)
    bool LooksLikeCut(const yuv::Planes& a, const yuv::Planes& b) const
    {
        const int f = 8, w = m_w / f, h = m_h / f;
        if (w < 4 || h < 4) return false;
        const uint16_t* pa = Words(a, 0); const uint16_t* pb = Words(b, 0);
        double sum = 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                double sa = 0, sb = 0;
                for (int dy = 0; dy < f; dy += 2)
                    for (int dx = 0; dx < f; dx += 2) { const size_t i = size_t(y * f + dy) * m_w + x * f + dx; sa += pa[i]; sb += pb[i]; }
                sum += std::fabs(sa - sb) / 16.0;
            }
        return sum / (double(w) * h) / double((1 << m_bits) - 1) > 0.12;
    }

    std::unique_ptr<pmml::Interpolator> m_interp;
    uint32_t m_fmt = 0;
    int m_w = 0, m_h = 0, m_bits = 8, m_k = 2;
    bool m_flowOnce = false, m_cut = false;
    uint64_t m_seq = 0, m_written = 0, m_generated = 0, m_cuts = 0;
    yuv::Planes m_pic[2], m_out, m_tmp;
    std::deque<pmeng_out_info> m_q;
};

} // namespace

Backend* OpenSidecar(const pmeng_stream& st, const Options& o, pmeng_output& out, std::string& why)
{
    auto* b = new MlBackend();
    if (!b->Init(st, o, out, why)) { delete b; return nullptr; }
    return b;
}

Backend* OpenSidecarDevice(const pmeng_device&, const pmeng_stream&, const Options&, pmeng_output&, std::string& why)
{
    why = "PushFlow takes frames in host memory";
    return nullptr;
}

bool ProbeSidecar(const Options& o, std::string& why)
{
    const std::string rt = RuntimeDir(o);
    if (!Present(rt)) { why = "the pmml runtime is not installed"; return false; }
    why = "pmml runtime in " + rt;
    return true;
}

} // namespace pme
