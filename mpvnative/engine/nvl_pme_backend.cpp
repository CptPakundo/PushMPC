// NvlBackend (Windows on Arm: NVIDIA RTX Spark / N1X): the CUDA + Video Effects engine (vsplugin/engines/nvidia-linux,
// the same engine the VapourSynth route runs on Linux) behind pmeng, for frames in host memory. NVIDIA VFG (or NVIDIA
// optical flow + our warp where VFG does not run), RTX VSR and the cut guard, decided by the same Auto (nvl_auto.h
// Decide) with the same settings (HKCU\Software\PushMPC, as every Windows route) and the filter's options on top.
// pushcore has no Windows ARM64 build (Veyra's DLLs are x64 only), so this is the NVIDIA engine of the ARM64 module.
//
// The runtime: NVIDIA's win_arm64 nvidia-vfx wheel (scripts/nvidia-vfx-pin.json), installed by Setup with
// scripts/nvidia-vfx-fetch.ps1 -Platform win_arm64 -Layout wheel into %LOCALAPPDATA%\PushMPC\runtime\nvidia-vfx (option
// runtime_dir / PUSHMPC_NVVFX_DIR override it). Without an NVIDIA driver, GPU or runtime, open() fails with the reason and
// Auto goes on to the portable GPU engine, then the CPU engine.
//
// The filter cannot change the CPU format: the output keeps the input's (8-bit in, 8-bit out; 10-bit in, 10-bit out);
// RTX Video HDR (SDR -> HDR) therefore runs only on 10-bit input. The size may change (RTX VSR).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>
#include "engine.h"
#include "nvl_cutguard.h"
#include "nvl_engine.h"
#include "nvl_settings.h"
#include "platform.h"

namespace pme {
namespace {

using namespace pmvs;
using namespace pmvs::nvl;

bool Ten(uint32_t f) { return f == PMENG_FMT_P010 || f == PMENG_FMT_YUV420P10; }
bool Semi(uint32_t f) { return f == PMENG_FMT_NV12 || f == PMENG_FMT_P010; }

// once per process (a driver probe loads nvcuda.dll and the runtime's file list)
const GpuInfo& Probe(const std::string& runtimeDir)
{
    static std::mutex m;
    static bool done = false;
    static GpuInfo g;
    std::lock_guard<std::mutex> lk(m);
    if (done) return g;
    done = true;
    g = ProbeGpu(-1, runtimeDir);
    if (g.driver) {
        char b[512];
        std::snprintf(b, sizeof(b), "nvidia (Windows on Arm): %s (SM %d.%d, %s, %zu MB), driver %d.%d; runtime %s%s", g.name.c_str(), g.major, g.minor, GpuClassName(g.cls),
                      g.memBytes >> 20, g.driverVersion / 1000, (g.driverVersion % 1000) / 10, g.runtimeDir.c_str(), g.runtime ? "" : (" MISSING (" + g.runtimeWhy + ")").c_str());
        Log(b);
        // RTX 30's VFG compatibility needs its out-of-process self-test, which this build does not carry (no RTX 30 on
        // Windows on Arm): forced on only by PUSHMPC_VFG_AMPERE=1
        if (VfgAmpere(g.major, g.minor)) {
            const char* f = std::getenv("PUSHMPC_VFG_AMPERE");
            if (f && !std::strcmp(f, "1")) { g.caps.vfg = true; g.caps.vfgWhy.clear(); }
        }
    } else {
        Log("nvidia (Windows on Arm): " + g.why);
    }
    return g;
}

// one engine per process, kept warm across mpv's graph rebuilds (seeks); a newer instance takes it over
struct Warm {
    std::mutex lock;
    std::shared_ptr<Engine> engine;
    uint64_t owner = 0, next = 0;
};
Warm& W() { static Warm w; return w; }

class NvlPmeBackend : public Backend {
public:
    ~NvlPmeBackend() override
    {
        Warm& w = W();
        std::lock_guard<std::mutex> lk(w.lock);
        if (w.owner == m_token && m_engine) { std::string e; m_engine->Drain(e); w.owner = 0; }
    }

    bool Init(const pmeng_stream& st, const Options& o, pmeng_output& out, bool explicitChoice, std::string& why)
    {
        m_fmt = st.fmt;
        if (m_fmt != PMENG_FMT_YUV420P && m_fmt != PMENG_FMT_YUV420P10 && m_fmt != PMENG_FMT_NV12 && m_fmt != PMENG_FMT_P010) {
            why = "the NVIDIA engine takes YUV 4:2:0 8 / 10 bit (planar, NV12, P010)";
            return false;
        }
        const GpuInfo& g = Probe(Opt(o, "runtime_dir"));
        if (!g.driver) { why = g.why; return false; }
        if (!g.runtime) { why = g.runtimeWhy; return false; }
        if (!g.caps.vsr && !g.caps.vfg && !g.caps.nvof) { why = std::string(GpuClassName(g.cls)) + ": nothing of the Video Effects runtime runs on " + g.name; return false; }
        m_w = int(st.width); m_h = int(st.height); m_bits = Ten(m_fmt) ? 10 : 8;

        Settings s = LoadSettings([](const char* n) { return ReadSetting(n); });
        for (const char* key : { "enable", "smoother", "sharper", "fg_quality", "sdr_to_hdr", "vsr_style", "target", "hdr_peak", "fg_method" })
            if (const std::string v = Opt(o, key); !v.empty()) ApplyArg(s, key, v);
        if (!s.enable) { why = "PushMPC is off (Enable = 0)"; return false; }
        if (m_bits == 8) s.sdrToHdr = -1;   // an HDR output needs 10 bits, and the filter keeps the CPU format

        m_colour.bits = m_bits;
        m_colour.full = st.range == 2;
        const bool hdr = st.transfer == 16 || st.transfer == 18;
        m_colour.matrix = st.matrix > 0 && st.matrix != 2 ? st.matrix : hdr ? 9 : (st.width < 1024 && st.height < 600 ? 6 : 1);
        Source src; src.w = m_w; src.h = m_h;
        src.fps = st.fps_num > 0 && st.fps_den > 0 ? double(st.fps_num) / st.fps_den : 23.976;
        src.hdr = hdr; src.tenBit = m_bits > 8;
        Display d; d.refreshHz = std::atof(Opt(o, "display_fps", "0").c_str()); d.hdr = Opt(o, "display_hdr", "0") == "1";
        Costs c; c.card = CardFactor(g.name, g.cls);
        Plan plan = Decide(s, src, d, g.caps, c);
        Log("nvidia: Auto: " + plan.why);
        // Auto: this engine only when it generates frames (else the portable GPU engine does more for smoothness);
        // backend=nvidia: also for upscaling alone
        if (plan.k < 2 && !explicitChoice) { why = "no frame generation here (" + plan.why + ")"; return false; }
        if (plan.k < 2 && !plan.vsr && !plan.hdr) { why = "nothing to do (" + plan.why + ")"; return false; }

        EngineConfig cfg;
        cfg.srcW = m_w; cfg.srcH = m_h; cfg.bits = m_bits; cfg.srcHdr = src.hdr; cfg.plan = plan; cfg.hdrPeak = float(s.hdrPeak);
        cfg.ampere = plan.k >= 2 && !plan.nvof && VfgAmpere(g.major, g.minor);
        cfg.guardFlow = plan.guardFlow;
        Warm& w = W();
        std::lock_guard<std::mutex> lk(w.lock);
        if (!w.engine || !(w.engine->Config() == cfg)) {
            w.owner = 0;
            w.engine.reset();   // one engine at a time (GPU memory)
            auto e = std::make_shared<Engine>();
            if (!e->Open(g, cfg, why)) {
                if (cfg.plan.k < 2 || !explicitChoice || !(cfg.plan.vsr || cfg.plan.hdr)) { why = "nvidia engine: " + why; return false; }
                Log("nvidia: frame generation could not start (" + why + "): upscaling only");
                cfg.plan.k = 1; cfg.plan.nvof = false; cfg.ampere = false; cfg.guardFlow = false;
                e = std::make_shared<Engine>();
                if (!e->Open(g, cfg, why)) { why = "nvidia engine: " + why; return false; }
            }
            w.engine = e;
            Log("nvidia: engine open: " + w.engine->Describe());
        } else if (!w.engine->Drain(why)) {
            return false;
        } else {
            Log("nvidia: engine reused (warm): " + w.engine->Describe());
        }
        m_engine = w.engine;
        m_token = ++w.next;
        w.owner = m_token;
        const Plan& ep = m_engine->Config().plan;
        m_k = std::max(1, ep.k);
        m_ow = ep.vsr ? ep.outW : m_w; m_oh = ep.vsr ? ep.outH : m_h;
        m_outHdr = m_engine->OutHdr();
        m_guard.Configure(m_w, m_h);
        if (Semi(m_fmt)) {
            const size_t bps = m_bits > 8 ? 2 : 1;
            m_sy = size_t(m_w) * bps; m_sc = size_t((m_w + 1) / 2) * bps;
            for (int i = 0; i < 3; ++i) m_p[i].resize((i ? m_sc : m_sy) * size_t(i ? (m_h + 1) / 2 : m_h));
        }
        out.mem = PMENG_MEM_CPU; out.fmt = m_fmt; out.width = uint32_t(m_ow); out.height = uint32_t(m_oh);
        out.multiplier = uint32_t(m_k);
        out.range = 1;   // limited (AVCOL_RANGE_MPEG)
        out.matrix = m_outHdr ? 9 : 1; out.primaries = m_outHdr ? 9 : 1; out.transfer = m_outHdr ? 16 : 1;
        out.hdr_output = m_outHdr ? 1 : 0;
        out.keep_dynamic_hdr = src.hdr ? 1 : 0;
        if (ep.hdr) { out.mastering_peak_nits = float(s.hdrPeak); out.max_cll_nits = float(s.hdrPeak); }
        std::snprintf(out.engine, sizeof(out.engine), "nvidia");
        std::snprintf(out.description, sizeof(out.description), "NVIDIA Video Effects on %s (CUDA, frames in host memory): %s", g.name.c_str(), m_engine->Describe().c_str());
        return true;
    }

    int Submit(const pmeng_image& src, int64_t, uint32_t flags) override
    {
        if (!Mine()) return PMENG_ERROR;
        if (src.mem != PMENG_MEM_CPU || src.fmt != m_fmt) { err = "nvidia: unexpected input memory / format"; return PMENG_ERROR; }
        if (m_engine->FreeSlots() < m_k) return PMENG_BUSY;
        SrcView v;
        v.width = m_w; v.height = m_h; v.bits = m_bits; v.fmt = m_colour;
        if (src.range == 2) v.fmt.full = true; else if (src.range == 1) v.fmt.full = false;
        if (src.matrix > 0 && src.matrix != 2) v.fmt.matrix = src.matrix;
        ToPlanar(src, v);
        const bool contiguous = !(flags & PMENG_SUBMIT_DISCONTINUITY) && m_seq > 0 && m_last == int64_t(m_seq) - 1;
        const bool gpuGuard = m_engine->HasGpuGuard();
        CutDecision cut;
        if (!gpuGuard)
            cut = m_guard.Push([&](int x, int y) -> uint32_t {
                const uint8_t* row = v.data[0] + v.stride[0] * y;
                const uint32_t c = v.bits > 8 ? reinterpret_cast<const uint16_t*>(row)[x] : row[x];
                return GuardLuma(c, v.bits, v.fmt.full);
            }, contiguous);
        const int parity = int(m_seq & 1);
        if (!m_engine->Upload(v, parity, err)) return PMENG_ERROR;
        if (contiguous && m_k >= 2 && m_engine->HasFlow() && !m_engine->Flow(parity, err)) return PMENG_ERROR;
        if (gpuGuard && m_k >= 2 && !m_engine->Guard(parity, contiguous, err)) return PMENG_ERROR;
        if (contiguous && m_k >= 2) {
            if (cut.cut) ++m_cuts;
            if (gpuGuard) m_cuts = m_engine->GuardCuts();
            for (int j = 1; j < m_k; ++j) {
                const int slot = cut.cut ? m_engine->Emit(1 - parity, err) : m_engine->Generate(parity, j, m_k, j == 1, err);
                if (slot < 0) return PMENG_ERROR;
                if (!cut.cut) ++m_generated;
                m_q.push_back({ { m_seq, uint32_t(j), uint32_t(m_k), 1 }, slot });
            }
        }
        const int slot = m_engine->Emit(parity, err);
        if (slot < 0) return PMENG_ERROR;
        m_q.push_back({ { m_seq, 0, uint32_t(m_k), 0 }, slot });
        m_last = int64_t(m_seq);
        ++m_seq;
        return PMENG_OK;
    }
    int Peek(pmeng_out_info& info, uint32_t timeoutMs) override
    {
        if (!Mine()) return PMENG_ERROR;
        if (m_q.empty()) return 0;
        int st = m_engine->Query(m_q.front().slot);
        if (st == 0 && timeoutMs > 0) st = m_engine->Wait(m_q.front().slot, timeoutMs) ? 1 : m_engine->Query(m_q.front().slot);
        if (st < 0) { err = "nvidia: the GPU failed on a frame (CUDA error)"; return PMENG_ERROR; }
        if (st == 0) return 0;
        info = m_q.front().info;
        return 1;
    }
    int Write(const pmeng_image& dst) override
    {
        if (!Mine()) return PMENG_ERROR;
        if (m_q.empty()) { err = "write without an output"; return PMENG_ERROR; }
        const Queued q = m_q.front();
        m_q.pop_front();
        if (dst.mem != PMENG_MEM_CPU || dst.fmt != m_fmt) { m_engine->Release(q.slot); err = "nvidia: unexpected output memory / format"; return PMENG_ERROR; }
        FromView(m_engine->View(q.slot), dst);
        m_engine->Release(q.slot);
        ++m_written;
        return PMENG_OK;
    }
    int Skip() override
    {
        if (!m_q.empty()) { if (Mine()) m_engine->Release(m_q.front().slot); m_q.pop_front(); }
        return PMENG_OK;
    }
    uint32_t Pending() override { return uint32_t(m_q.size()); }
    int Flush() override
    {
        m_q.clear();
        m_guard.Reset();
        m_last = -1000000;
        if (!Mine()) return PMENG_ERROR;
        return m_engine->Drain(err) ? PMENG_OK : PMENG_ERROR;
    }
    std::string Stats() override
    {
        char b[320];
        std::snprintf(b, sizeof(b), "nvidia: %llu sources, %llu outputs (%llu generated), cut guard held %llu pairs (%s)", (unsigned long long)m_seq,
                      (unsigned long long)m_written, (unsigned long long)m_generated, (unsigned long long)m_cuts, m_engine && m_engine->HasGpuGuard() ? "GPU" : "CPU");
        return b;
    }

private:
    struct Queued { pmeng_out_info info; int slot; };
    bool Mine()
    {
        Warm& w = W();
        std::lock_guard<std::mutex> lk(w.lock);
        if (w.owner == m_token) return true;
        err = "nvidia: a newer instance took over the engine";
        return false;
    }
    // NV12 / P010 -> planar low-bit samples (the engine's SrcView); planar input as it is
    void ToPlanar(const pmeng_image& s, SrcView& v)
    {
        if (!Semi(m_fmt)) { for (int i = 0; i < 3; ++i) { v.data[i] = s.data[i]; v.stride[i] = s.linesize[i]; } return; }
        const int ch = (m_h + 1) / 2, cw = (m_w + 1) / 2;
        if (m_bits == 8) {
            for (int r = 0; r < m_h; ++r) std::memcpy(&m_p[0][r * m_sy], s.data[0] + r * s.linesize[0], m_sy);
            for (int r = 0; r < ch; ++r) {
                const uint8_t* q = s.data[1] + r * s.linesize[1];
                uint8_t* u = &m_p[1][r * m_sc]; uint8_t* vv = &m_p[2][r * m_sc];
                for (int x = 0; x < cw; ++x) { u[x] = q[2 * x]; vv[x] = q[2 * x + 1]; }
            }
        } else {
            for (int r = 0; r < m_h; ++r) {
                const uint16_t* q = reinterpret_cast<const uint16_t*>(s.data[0] + r * s.linesize[0]);
                uint16_t* y = reinterpret_cast<uint16_t*>(&m_p[0][r * m_sy]);
                for (int x = 0; x < m_w; ++x) y[x] = uint16_t(q[x] >> 6);
            }
            for (int r = 0; r < ch; ++r) {
                const uint16_t* q = reinterpret_cast<const uint16_t*>(s.data[1] + r * s.linesize[1]);
                uint16_t* u = reinterpret_cast<uint16_t*>(&m_p[1][r * m_sc]); uint16_t* vv = reinterpret_cast<uint16_t*>(&m_p[2][r * m_sc]);
                for (int x = 0; x < cw; ++x) { u[x] = uint16_t(q[2 * x] >> 6); vv[x] = uint16_t(q[2 * x + 1] >> 6); }
            }
        }
        for (int i = 0; i < 3; ++i) { v.data[i] = m_p[i].data(); v.stride[i] = ptrdiff_t(i ? m_sc : m_sy); }
    }
    // the engine's planar output (8 bits, or 10 in 16-bit LSB words) -> the caller's format and depth
    void FromView(const OutView& o, const pmeng_image& d) const
    {
        const int w = m_ow, h = m_oh, cw = (w + 1) / 2, ch = (h + 1) / 2;
        const bool in10 = o.bits > 8, out10 = m_bits > 8;
        auto sample = [&](int plane, int x, int y) -> uint32_t {
            const uint8_t* row = o.data[plane] + o.pitch[plane] * size_t(y);
            const uint32_t v = in10 ? reinterpret_cast<const uint16_t*>(row)[x] : row[x];
            if (in10 == out10) return v;
            return out10 ? v << 2 : std::min<uint32_t>(255, (v + 2) >> 2);
        };
        const int shift = Semi(m_fmt) && out10 ? 6 : 0;   // P010: MSB-aligned
        auto put = [&](uint8_t* row, int x, uint32_t v) { if (out10) reinterpret_cast<uint16_t*>(row)[x] = uint16_t(v << shift); else row[x] = uint8_t(v); };
        for (int y = 0; y < h; ++y) { uint8_t* row = d.data[0] + d.linesize[0] * y; for (int x = 0; x < w; ++x) put(row, x, sample(0, x, y)); }
        if (Semi(m_fmt)) {
            for (int y = 0; y < ch; ++y) {
                uint8_t* row = d.data[1] + d.linesize[1] * y;
                for (int x = 0; x < cw; ++x) { put(row, 2 * x, sample(1, x, y)); put(row, 2 * x + 1, sample(2, x, y)); }
            }
        } else {
            for (int i = 1; i < 3; ++i)
                for (int y = 0; y < ch; ++y) { uint8_t* row = d.data[i] + d.linesize[i] * y; for (int x = 0; x < cw; ++x) put(row, x, sample(i, x, y)); }
        }
    }

    std::shared_ptr<Engine> m_engine;
    uint64_t m_token = 0;
    CutGuard m_guard;
    InFormat m_colour;
    uint32_t m_fmt = 0;
    int m_w = 0, m_h = 0, m_ow = 0, m_oh = 0, m_bits = 8, m_k = 1;
    bool m_outHdr = false;
    uint64_t m_seq = 0, m_written = 0, m_generated = 0, m_cuts = 0;
    int64_t m_last = -1000000;
    size_t m_sy = 0, m_sc = 0;
    std::vector<uint8_t> m_p[3];
    std::deque<Queued> m_q;
};

} // namespace

Backend* OpenNvl(const pmeng_stream& st, const Options& o, pmeng_output& out, bool explicitChoice, std::string& why)
{
    auto* b = new NvlPmeBackend();
    if (!b->Init(st, o, out, explicitChoice, why)) { delete b; return nullptr; }
    return b;
}

} // namespace pme
