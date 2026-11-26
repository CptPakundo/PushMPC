// NvlBackend: the Linux NVIDIA engine (vsplugin/engines/nvidia-linux: RTX VSR, RTX Video HDR, NVIDIA VFG or NVIDIA
// optical flow + warp, PushMPC's cut guard on the GPU) behind pmeng, for frames in host memory: the native hook's
// software / NVDEC-copied frames in distro mpv and the libmpv players, and Plex; and for the decoder's own CUDA frames
// (hwdec=nvdec: PMENG_MEM_CUDA), device to device with no copy through host memory. It lives in its own module,
// libpushmpc_engine_nvl.so (built with the kernels' fatbin), which libpushmpc_engine.so loads next to itself; CUDA and
// NVIDIA's Video Effects runtime are loaded at run time (nothing NVIDIA is linked or shipped).
//
// The same Auto as the VapourSynth route (nvl_auto.h Decide: the display's multiple within the GPU budget, VSR to the
// target, VFG quality), the same warm engine across mpv's filter rebuilds (seek), the same fallback to upscaling only
// when frame generation cannot start. The filter keeps the stream's CPU format, so the engine's 4:2:0 output is written
// back in it (NV12 / P010 re-interleaved; an 8-bit engine output on a 10-bit stream shifted up), or in the format the
// RTX Video HDR needs 10-bit output: on an 8-bit stream it runs when the output is CUDA frames (their pool takes P010);
// with frames in host memory the engine asks for a 10-bit stream (pmeng_output.want_out_depth = 10, passed on by the
// filter as frame metadata) and the mpv hook converts the stream to 10 bits ahead of the filter (one rebuild).
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include "engine.h"
#include "platform.h"
#include "schedule.h"
#include "yuv_io.h"
#include "nvl_cutguard.h"
#include "nvl_engine.h"
#include "nvl_selftest.h"
#include "nvl_settings.h"

namespace pme {
namespace {

using namespace pmvs;
using namespace pmvs::nvl;

// once per process: the GPU, the runtime, the RTX 30 self-test (a child process next to this module, cached)
const GpuInfo& Probe(const Options& o)
{
    static std::mutex m;
    static bool done = false;
    static GpuInfo g;
    std::lock_guard<std::mutex> lk(m);
    if (done) return g;
    done = true;
    g = ProbeGpu(std::atoi(Opt(o, "cuda_device", "-1").c_str()), Opt(o, "runtime_dir"));
    if (!g.driver) { Log("nvidia (Linux): " + g.why); return g; }
    char b[400];
    std::snprintf(b, sizeof(b), "nvidia (Linux): %s (SM %d.%d, %s, %zu MB), driver %d.%d; runtime %s%s", g.name.c_str(), g.major, g.minor, GpuClassName(g.cls),
                  g.memBytes >> 20, g.driverVersion / 1000, (g.driverVersion % 1000) / 10, g.runtimeDir.c_str(), g.runtime ? "" : (" MISSING (" + g.runtimeWhy + ")").c_str());
    Log(b);
    if (g.runtime && VfgAmpere(g.major, g.minor)) {
        const char* force = std::getenv("PUSHMPC_VFG_AMPERE");
        const std::string key = SelfTestKey(g);
        int r = force ? (std::strcmp(force, "0") ? 1 : 0) : ReadSelfTest(key);
        std::string log = force ? std::string("PUSHMPC_VFG_AMPERE=") + force : std::string("cached");
        if (r < 0) r = RunSelfTest(SelfTestExe(), g.runtimeDir, g.device, 120, log);
        if (r == 1) { g.caps.vfg = true; g.caps.vfgWhy.clear(); }
        else g.caps.vfgWhy = r == 0 ? "RTX 30: the VFG compatibility self-test failed on this GPU / driver (" + log + ")" : "RTX 30: the VFG compatibility self-test could not run (" + log + ")";
        Log("nvidia (Linux): RTX 30 VFG compatibility: " + std::string(r == 1 ? "on" : "off") + " (" + (r == 1 ? log : g.caps.vfgWhy) + ")");
    }
    return g;
}

// one engine at a time in the process (GPU memory), kept warm across instances: mpv rebuilds its filters on every seek
struct Warm { std::mutex lock; std::shared_ptr<Engine> engine; uint64_t owner = 0, next = 0; };
Warm& W() { static Warm w; return w; }

class NvlBackend : public Backend {
public:
    ~NvlBackend() override
    {
        Warm& w = W();
        std::lock_guard<std::mutex> lk(w.lock);
        std::string e;
        if (m_eng && w.owner == m_token) { m_eng->Drain(e); w.owner = 0; }
        Log("nvidia: instance closed; the engine stays warm");
    }

    bool Init(const pmeng_stream& st, const Options& o, pmeng_output& out, std::string& why, const pmeng_device* dev = nullptr)
    {
        if (!yuv::Supported(st.fmt)) { why = "the nvidia engine takes YUV 4:2:0 8 / 10 bit (planar, NV12, P010)"; return false; }
        m_fmt = st.fmt; m_w = int(st.width); m_h = int(st.height); m_bits = yuv::Ten(st.fmt) ? 10 : 8;
        // the decoder's CUDA frames (hwdec=nvdec): NV12 / P010 in its own context
        m_cuda = dev && dev->mem == PMENG_MEM_CUDA;
        if (m_cuda) {
            if (dev->struct_size < offsetof(pmeng_device, cuda_stream) + sizeof(void*)) { why = "CUDA frames from a filter without the CUDA fields"; return false; }
            if (!yuv::Semi(st.fmt)) { why = "CUDA frames: NV12 / P010 only"; return false; }
            m_devCtx = static_cast<CUcontext>(dev->cuda_ctx);
            m_devStream = static_cast<CUstream>(dev->cuda_stream);
        }
        m_outFmt = m_fmt;   // frames in host memory keep the stream's format (the filter does not renegotiate)
        const GpuInfo& g = Probe(o);
        if (!g.driver) { why = g.why; return false; }
        if (!g.runtime) { why = g.runtimeWhy; return false; }
        if (!g.caps.vsr && !g.caps.vfg && !g.caps.nvof) { why = g.name + ": no RTX VSR, VFG or optical-flow hardware"; return false; }
        if (m_cuda && !Engine::ContextOnDevice(m_devCtx, g.device, why)) { why = "CUDA frames: " + why; return false; }
        Settings s = LoadSettings([](const char* n) { return ReadSetting(n); });
        std::string overrides;
        for (const char* key : { "enable", "smoother", "sharper", "fg_quality", "sdr_to_hdr", "vsr_style", "target", "hdr_peak", "fg_method" })
            if (const std::string v = Opt(o, key); !v.empty() && LowerAscii(v) != "auto" && ApplyArg(s, key, v)) overrides += std::string(" ") + key + "=" + v;
        if (!s.enable) { why = "PushMPC is off (Enable = 0)"; return false; }

        // colour: FFmpeg's enums are H.273's codes (range: 1 limited, 2 full)
        // source_bits=8: an 8-bit source the player converted to 10 bits for this engine (the mpv hook's route): the
        // colour guess of an untagged stream is the 8-bit one (no PQ from the 10-bit UHD rule). source_bits=-1: the
        // player converts every stream to 10 bits (Plex's static line where it has no hook): SDR or HDR is decided at
        // the first frame (its transfer), below
        const int srcBits = std::atoi(Opt(o, "source_bits", "0").c_str());
        const Colour guess = GuessColour(st.width, st.height, srcBits == 8 ? 8 : m_bits);
        m_in.bits = m_bits;
        m_in.matrix = st.matrix > 0 && st.matrix != 2 ? st.matrix : guess.matrix;
        m_in.full = st.range == 2;
        const int transfer = st.transfer > 0 && st.transfer != 2 ? st.transfer : guess.transfer;
        Source src; src.w = m_w; src.h = m_h;
        src.fps = st.fps_num > 0 && st.fps_den > 0 ? double(st.fps_num) / double(st.fps_den) : 23.976;
        src.hdr = IsHdrTransfer(transfer); src.tenBit = m_bits > 8;
        Display disp; disp.refreshHz = std::atof(Opt(o, "display_fps", "0").c_str()); disp.hdr = Opt(o, "display_hdr") == "1";
        const int dispH = std::atoi(Opt(o, "display_h", "0").c_str());
        if (s.targetScreen && dispH > 0) s.targetH = TargetForDisplay(dispH);
        const double dispPeak = std::atof(Opt(o, "display_peak", "0").c_str());
        if (!s.hdrPeakSet && dispPeak >= 400) s.hdrPeak = std::min(dispPeak, 2000.0);
        std::string notes;
        Caps caps = g.caps;
        Costs costs; costs.card = CardFactor(g.name, g.cls);
        // RTX Video HDR makes 10-bit output: on an 8-bit stream it needs CUDA frames (their pool takes P010) or a 10-bit
        // CPU output from the filter; without either, the engine runs without it and asks for the 10-bit output
        const bool deepOut = m_cuda;
        bool wantDeep = false;
        if (m_bits == 8 && !src.hdr && caps.hdr && !deepOut) {
            wantDeep = Decide(s, src, disp, caps, costs).hdr;
            caps.hdr = false;
            if (wantDeep) notes = "; RTX Video HDR waits for a 10-bit stream (asked for: want_out_depth=10)";
        }
        m_gpu = &g; m_hdrPeak = float(s.hdrPeak);
        if (srcBits == -1 && m_bits > 8 && !m_cuda && !(st.transfer > 0 && st.transfer != 2)) {
            // the stream may be an 8-bit SDR one the player converted: when RTX Video HDR would run on it as SDR and
            // both readings give the same picture size, the output is HDR10 either way; the engine opens at the first
            // frame with the reading its transfer gives (an untagged frame keeps the resolution guess)
            Source sdr = src, hdrSrc = src;
            sdr.hdr = false; hdrSrc.hdr = true;
            const nvl::Plan pS = Decide(s, sdr, disp, caps, costs), pH = Decide(s, hdrSrc, disp, caps, costs);
            if (pS.hdr && pS.vsr == pH.vsr && (!pS.vsr || (pS.outW == pH.outW && pS.outH == pH.outH)) && (pS.k > 1 || pS.vsr || pS.hdr)) {
                m_defer = true; m_planSdr = pS; m_planHdr = pH; m_guessHdr = src.hdr;
                m_k = std::max(1, std::min(pS.k, pH.k));
                m_outW = pS.vsr ? pS.outW : m_w; m_outH = pS.vsr ? pS.outH : m_h;
                Log("nvidia: Auto (decided at the first frame): SDR " + pS.why + " | HDR " + pH.why + "; x" + std::to_string(m_k) + notes);
                out.mem = PMENG_MEM_CPU; out.fmt = m_outFmt;
                out.width = uint32_t(m_outW); out.height = uint32_t(m_outH);
                out.multiplier = uint32_t(m_k);
                out.range = 1; out.matrix = 9; out.primaries = 9; out.transfer = 16; out.hdr_output = 1;
                out.keep_dynamic_hdr = pS.vsr ? 0u : 1u;   // an SDR source has none; an HDR one keeps its own unless upscaled
                out.mastering_peak_nits = m_hdrPeak; out.mastering_min_nits = 0.0001f; out.max_cll_nits = m_hdrPeak;   // used only when a frame has none
                std::snprintf(out.engine, sizeof(out.engine), "nvidia");
                std::snprintf(out.description, sizeof(out.description), "NVIDIA (Linux, CUDA + Video Effects): HDR10 out, SDR (RTX Video HDR) or HDR decided at the first frame; %dx%d -> %dx%d x%d",
                              m_w, m_h, m_outW, m_outH, m_k);
                return true;
            }
        }
        nvl::Plan plan = Decide(s, src, disp, caps, costs);
        Log("nvidia: Auto: " + plan.why + notes + (overrides.empty() ? std::string() : "; overridden:" + overrides));
        if (plan.k <= 1 && !plan.vsr && !plan.hdr) { why = "nothing to do here (" + plan.why + ")"; return false; }
        if (!OpenEngine(plan, src.hdr, why)) return false;
        if (m_cuda && (!m_eng->CanTakeDevice() || !m_eng->CanWriteDevice() || !m_eng->HasGpuGuard())) {
            why = std::string("CUDA frames: ") + (!m_eng->HasGpuGuard() ? "this build has no GPU cut guard" : "this build's kernels or the driver lack the device copies") + " (the copy route)";
            return false;
        }
        const nvl::Plan& ep = m_eng->Config().plan;

        out.mem = m_cuda ? PMENG_MEM_CUDA : PMENG_MEM_CPU;
        out.fmt = m_cuda ? (m_eng->OutTenBit() ? PMENG_FMT_P010 : PMENG_FMT_NV12) : m_outFmt;
        out.want_out_depth = wantDeep ? 10u : 0u;
        out.width = uint32_t(m_outW); out.height = uint32_t(m_outH);
        out.multiplier = uint32_t(m_k);
        // the engine's output colour (nvl_colour.h): limited range, the BT.709 matrix for SDR, BT.2020 for HDR; primaries
        // and transfer are converted only by RTX Video HDR (0 = the frame keeps its own; vf_pushmpc applies the rest)
        const bool hdr = m_eng->OutHdr();
        const bool madeHdr = hdr && !src.hdr;
        out.range = 1;
        out.matrix = hdr ? 9 : 1;
        out.primaries = madeHdr ? 9 : 0;
        out.transfer = madeHdr ? 16 : 0;
        out.hdr_output = hdr ? 1u : 0u;
        out.keep_dynamic_hdr = src.hdr && !ep.vsr ? 1u : 0u;   // per-frame HDR metadata no longer matches an upscaled / converted picture
        if (hdr && !src.hdr) { out.mastering_peak_nits = float(s.hdrPeak); out.mastering_min_nits = 0.0001f; out.max_cll_nits = float(s.hdrPeak); }
        std::snprintf(out.engine, sizeof(out.engine), "nvidia");
        std::snprintf(out.description, sizeof(out.description), "NVIDIA (Linux, CUDA + Video Effects): %s; %dx%d -> %dx%d x%d%s%s",
                      m_eng->Describe().c_str(), m_w, m_h, m_outW, m_outH, m_k, m_eng->HasGpuGuard() ? "" : "; CPU cut guard",
                      m_cuda ? "; the decoder's CUDA frames, device to device" : "");
        return true;
    }

    // the engine for a plan (warm when the same one is open), its multiplier and output size
    bool OpenEngine(nvl::Plan plan, bool srcHdr, std::string& why)
    {
        const GpuInfo& g = *m_gpu;
        EngineConfig cfg;
        cfg.srcW = m_w; cfg.srcH = m_h; cfg.bits = m_bits; cfg.srcHdr = srcHdr; cfg.plan = plan; cfg.hdrPeak = m_hdrPeak;
        cfg.ampere = plan.k >= 2 && !plan.nvof && VfgAmpere(g.major, g.minor);
        cfg.guardFlow = plan.guardFlow;
        cfg.deviceOut = m_cuda;
        Warm& w = W();
        std::lock_guard<std::mutex> lk(w.lock);
        if (!w.engine || !(w.engine->Config() == cfg)) {
            w.owner = 0;
            w.engine.reset();
            auto e = std::make_shared<Engine>();
            std::string err;
            if (!e->Open(g, cfg, err)) {
                if (cfg.plan.k < 2 || !(cfg.plan.vsr || cfg.plan.hdr)) { why = "nvidia: " + err; return false; }
                Log(std::string("nvidia: ") + (cfg.plan.nvof ? "the optical-flow tier" : "VFG") + " could not start (" + err + "): upscaling only");
                cfg.plan.k = 1; cfg.plan.nvof = false; cfg.ampere = false; cfg.guardFlow = false;
                e = std::make_shared<Engine>();
                if (!e->Open(g, cfg, err)) { why = "nvidia: " + err; return false; }
            }
            w.engine = e;
            Log("nvidia: engine open: " + w.engine->Describe());
        } else {
            std::string err;
            if (!w.engine->Drain(err)) { why = "nvidia: " + err; return false; }
            Log("nvidia: engine reused (warm): " + w.engine->Describe());
        }
        m_eng = w.engine;
        m_token = ++w.next;
        w.owner = m_token;
        const nvl::Plan& ep = m_eng->Config().plan;
        if (m_defer && std::max(1, ep.k) != m_k) { why = "nvidia: frame generation could not start at x" + std::to_string(m_k); return false; }
        m_k = std::max(1, ep.k);
        m_outW = ep.vsr ? ep.outW : m_w; m_outH = ep.vsr ? ep.outH : m_h;
        m_guard.Configure(m_w, m_h);
        if (yuv::Semi(m_fmt)) m_src.Resize(m_w, m_h, m_bits);
        return true;
    }

    int Submit(const pmeng_image& src, int64_t, uint32_t flags) override
    {
        if (m_defer && !m_eng) {   // source_bits=-1: SDR or HDR from the first frame's transfer
            const bool tagged = src.transfer > 0 && src.transfer != 2;
            const bool hdr = tagged ? IsHdrTransfer(src.transfer) : m_guessHdr;
            if (src.matrix > 0 && src.matrix != 2) m_in.matrix = src.matrix;
            nvl::Plan plan = hdr ? m_planHdr : m_planSdr;
            plan.k = m_k;
            std::string why;
            if (!OpenEngine(plan, hdr, why)) { err = why; return PMENG_ERROR; }
            Log(std::string("nvidia: first frame ") + (tagged ? "tagged " : "untagged, guessed ") + (hdr ? "HDR" : "SDR") + ": " + plan.why);
        }
        if (!m_q.empty() && m_eng->FreeSlots() < m_k) return PMENG_BUSY;
        if (m_eng->FreeSlots() < m_k) { err = "nvidia: no free readback slot"; return PMENG_ERROR; }
        if (src.mem != (m_cuda ? PMENG_MEM_CUDA : PMENG_MEM_CPU) || src.fmt != m_fmt) { err = "nvidia: unexpected input memory / format"; return PMENG_ERROR; }
        const bool contiguousC = !(flags & PMENG_SUBMIT_DISCONTINUITY) && m_seq > 0;
        if (m_cuda) {   // device to device; the cut guard runs on the GPU (checked at open)
            DevFrame d;
            d.ctx = m_devCtx; d.stream = m_devStream; d.width = m_w; d.height = m_h; d.p010 = yuv::Ten(m_fmt); d.fmt = m_in;
            d.y = src.cuda_ptr[0]; d.uv = src.cuda_ptr[1]; d.yPitch = size_t(src.cuda_pitch[0]); d.uvPitch = size_t(src.cuda_pitch[1]);
            const int parity = int(m_seq & 1);
            if (!m_eng->UploadDevice(d, parity, err)) return PMENG_ERROR;
            return Generated(parity, contiguousC, CutDecision{}, true);
        }
        SrcView v;
        v.width = m_w; v.height = m_h; v.bits = m_bits; v.fmt = m_in;
        if (yuv::Semi(m_fmt)) {
            yuv::ToPlanar(src, m_fmt, m_src);
            for (int i = 0; i < 3; ++i) { v.data[i] = m_src.p[i].data(); v.stride[i] = ptrdiff_t(m_src.stride[i]); }
        } else {
            for (int i = 0; i < 3; ++i) { v.data[i] = src.data[i]; v.stride[i] = src.linesize[i]; }
        }
        const bool contiguous = !(flags & PMENG_SUBMIT_DISCONTINUITY) && m_seq > 0;
        if (!contiguous) m_guard.Reset();
        const bool gpuGuard = m_eng->HasGpuGuard();
        CutDecision cut;
        if (!gpuGuard)
            cut = m_guard.Push([&](int x, int y) -> uint32_t {
                const uint8_t* row = v.data[0] + v.stride[0] * y;
                const uint32_t c = v.bits > 8 ? reinterpret_cast<const uint16_t*>(row)[x] : row[x];
                return GuardLuma(c, v.bits, v.fmt.full);
            }, contiguous);
        const int parity = int(m_seq & 1);
        if (!m_eng->Upload(v, parity, err)) return PMENG_ERROR;
        return Generated(parity, contiguous, cut, gpuGuard);
    }
    // after the upload: flow, guard, the generated frames of the pair and the real frame, queued in output order
    int Generated(int parity, bool contiguous, CutDecision cut, bool gpuGuard)
    {
        if (contiguous && m_k >= 2 && m_eng->HasFlow() && !m_eng->Flow(parity, err)) return PMENG_ERROR;
        if (gpuGuard && m_k >= 2 && !m_eng->Guard(parity, contiguous, err)) return PMENG_ERROR;
        if (contiguous && m_k >= 2) {
            if (cut.cut) ++m_cuts;
            if (gpuGuard) m_cuts = m_eng->GuardCuts();
            for (int j = 1; j < m_k; ++j) {
                const int slot = cut.cut ? m_eng->Emit(1 - parity, err) : m_eng->Generate(parity, j, m_k, j == 1, err);
                if (slot < 0) return PMENG_ERROR;
                if (!cut.cut) ++m_generated;
                m_q.push_back({ { m_seq, uint32_t(j), uint32_t(m_k), 1 }, slot });
            }
        }
        const int slot = m_eng->Emit(parity, err);
        if (slot < 0) return PMENG_ERROR;
        m_q.push_back({ { m_seq, 0, uint32_t(m_k), 0 }, slot });
        ++m_seq;
        return PMENG_OK;
    }
    int Peek(pmeng_out_info& info, uint32_t timeoutMs) override
    {
        if (m_q.empty()) return 0;
        int st = m_eng->Query(m_q.front().slot);
        if (st == 0 && timeoutMs > 0) st = m_eng->Wait(m_q.front().slot, timeoutMs) ? 1 : m_eng->Query(m_q.front().slot);
        if (st < 0) { err = "nvidia: the GPU failed on a frame (CUDA error)"; return PMENG_ERROR; }
        if (st == 0) return 0;
        info = m_q.front().info;
        return 1;
    }
    int Write(const pmeng_image& dst) override
    {
        if (m_q.empty()) { err = "write without an output"; return PMENG_ERROR; }
        const Queued q = m_q.front();
        m_q.pop_front();
        if (m_cuda) {
            if (dst.mem != PMENG_MEM_CUDA) { m_eng->Release(q.slot); err = "nvidia: unexpected output memory"; return PMENG_ERROR; }
            DevFrame d;
            d.ctx = m_devCtx; d.y = dst.cuda_ptr[0]; d.uv = dst.cuda_ptr[1]; d.yPitch = size_t(dst.cuda_pitch[0]); d.uvPitch = size_t(dst.cuda_pitch[1]);
            const bool ok = m_eng->CopyOut(q.slot, d, err);
            m_eng->Release(q.slot);
            if (!ok) return PMENG_ERROR;
            ++m_written;
            return PMENG_OK;
        }
        if (dst.mem != PMENG_MEM_CPU || dst.fmt != m_outFmt) { m_eng->Release(q.slot); err = "nvidia: unexpected output memory / format"; return PMENG_ERROR; }
        const OutView o = m_eng->View(q.slot);
        const uint8_t* planes[3] = { o.data[0], o.data[1], o.data[2] };
        yuv::FromPlanes(planes, o.pitch, o.bits, o.width, o.height, m_outFmt, dst);
        m_eng->Release(q.slot);
        ++m_written;
        return PMENG_OK;
    }
    int Skip() override { if (!m_q.empty()) { m_eng->Release(m_q.front().slot); m_q.pop_front(); } return PMENG_OK; }
    uint32_t Pending() override { return uint32_t(m_q.size()); }
    int Flush() override
    {
        m_q.clear();
        m_guard.Reset();
        if (!m_eng) return PMENG_OK;
        std::string e;
        if (!m_eng->Drain(e)) { err = e; return PMENG_ERROR; }
        return PMENG_OK;
    }
    std::string Stats() override
    {
        char b[256];
        std::snprintf(b, sizeof(b), "nvidia: %llu sources, %llu outputs (%llu generated), cut guard held %llu pairs",
                      (unsigned long long)m_seq, (unsigned long long)m_written, (unsigned long long)m_generated, (unsigned long long)m_cuts);
        return b;
    }

private:
    struct Queued { pmeng_out_info info; int slot; };
    std::shared_ptr<Engine> m_eng;
    uint64_t m_token = 0;
    CutGuard m_guard;
    InFormat m_in;
    yuv::Planes m_src;
    uint32_t m_fmt = 0, m_outFmt = 0;
    const GpuInfo* m_gpu = nullptr;
    float m_hdrPeak = 1000;
    bool m_defer = false, m_guessHdr = false;   // source_bits=-1: the engine opens at the first frame
    nvl::Plan m_planSdr, m_planHdr;
    bool m_cuda = false;
    CUcontext m_devCtx = nullptr;
    CUstream m_devStream = nullptr;
    int m_w = 0, m_h = 0, m_bits = 8, m_k = 1, m_outW = 0, m_outH = 0;
    uint64_t m_seq = 0, m_written = 0, m_generated = 0, m_cuts = 0;
    std::deque<Queued> m_q;
};

} // namespace

Backend* OpenSidecar(const pmeng_stream& st, const Options& o, pmeng_output& out, std::string& why)
{
    auto* b = new NvlBackend();
    if (!b->Init(st, o, out, why)) { delete b; return nullptr; }
    return b;
}

// the decoder's CUDA frames in and out (PMENG_MEM_CUDA)
Backend* OpenSidecarDevice(const pmeng_device& dev, const pmeng_stream& st, const Options& o, pmeng_output& out, std::string& why)
{
    if (dev.mem != PMENG_MEM_CUDA) { why = "this engine takes CUDA frames or frames in host memory"; return nullptr; }
    auto* b = new NvlBackend();
    if (!b->Init(st, o, out, why, &dev)) { delete b; return nullptr; }
    return b;
}

// cheap: would this engine run on this machine (driver, runtime, something to do)? The main module asks before it
// gives FFmpeg's Vulkan frames to its zero-copy engine: NVIDIA's engine does more on an RTX card.
bool ProbeSidecar(const Options& o, std::string& why)
{
    const GpuInfo& g = Probe(o);
    if (!g.driver) { why = g.why; return false; }
    if (!g.runtime) { why = g.runtimeWhy; return false; }
    if (!g.caps.vsr && !g.caps.vfg && !g.caps.nvof) { why = g.name + ": no RTX VSR, VFG or optical-flow hardware"; return false; }
    why = g.name + (g.caps.vfg ? ": VFG" : g.caps.nvof ? ": optical flow" : "") + (g.caps.vsr ? " + RTX VSR" : "");
    return true;
}

} // namespace pme
