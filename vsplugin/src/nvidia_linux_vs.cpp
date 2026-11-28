// pushmpc.Enhance, backend "nvidia" on Linux (engines/nvidia-linux): RTX VSR, RTX Video HDR and NVIDIA VFG from the
// Video Effects runtime (the pinned `nvidia-vfx` wheel, fetched at install by scripts/nvidia-vfx-fetch.sh) through
// CUDA on the NVIDIA proprietary driver, with PushMPC's cut guard, behind the same Pipeline (schedule.h) as the
// other backends.
//
// The output is YUV 4:2:0 limited range: 8-bit BT.709 for an SDR pipeline, 10-bit (BT.709, or BT.2020 PQ for HDR)
// when the source is deeper than 8 bits without VSR, the source is HDR, or RTX Video HDR runs; at k x the source rate
// with _DurationNum/_DurationDen = source duration / k, the source frame's properties copied.
// mpv reloads the script on every seek: the engine (effects loaded, models warm) stays in the process and the next
// instance with the same configuration reuses it.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "VapourSynth4.h"
#include "backend.h"
#include "nvl_backend.h"
#include "nvl_engine.h"
#include "nvl_host.h"
#include "nvl_selftest.h"
#include "nvl_settings.h"
#include "platform.h"
#include "schedule.h"
#include "vslog.h"
#include "mp_out.h"   // the output's colour in mpv's _MP_IMAGE

namespace pmvs {

using namespace pmvs::nvl;

namespace {

struct Warm {
    std::mutex lock;
    std::shared_ptr<Engine> engine;
    uint64_t owner = 0, next = 0;
};
Warm& W() { static Warm w; return w; }

struct NMeta { int64_t durNum = 0, durDen = 0; std::shared_ptr<VSMap> props; };

struct NInst {
    VSNode* node = nullptr;
    std::string flowKey;          // where the optical-flow tier's measured costs go (FlowCostKey)
    VSVideoInfo vi{}, inVi{};
    int k = 1;
    uint64_t token = 0;
    std::shared_ptr<Engine> engine;
    Pipeline<void*> pipe;
    NvlBackend<void*> be;
    std::map<int64_t, NMeta> meta;
    InFormat colour;             // fallback when a frame carries no colour properties
    bool outHdr = false; int outBits = 8;
    bool srcHdr = false; float hdrPeak = 0;   // the source's HDR-ness and RTX Video HDR's peak: the output's _MP_IMAGE colour
    int mpTagLog = 0;
    uint64_t produced = 0, lastLogged = 0;
    VSCore* core = nullptr;
    const VSAPI* api = nullptr;
    VSFrameContext* fc = nullptr;
    std::vector<const VSFrame*> held;
};

// the optical-flow tier's measured costs (FlowCostKey, ReadFlowCosts, WriteFlowCosts) and the probe with the RTX 30 VFG
// gate: nvl_host.h (shared with capture mode)

std::string NArg(const VSMap* in, const VSAPI* a, const char* key)
{
    int err = 0;
    const char* v = a->mapGetData(in, key, 0, &err);
    return err || !v ? std::string() : std::string(v);
}
int64_t NArgI(const VSMap* in, const VSAPI* a, const char* key, int64_t def) { int err = 0; const int64_t v = a->mapGetInt(in, key, 0, &err); return err ? def : v; }
double NArgF(const VSMap* in, const VSAPI* a, const char* key, double def) { int err = 0; const double v = a->mapGetFloat(in, key, 0, &err); return err ? def : v; }
int64_t PInt(const VSMap* m, const VSAPI* a, const char* key, int64_t def, bool* present = nullptr)
{
    int err = 0;
    const int64_t v = a->mapGetInt(m, key, 0, &err);
    if (present) *present = !err;
    return err ? def : v;
}

// once per process: the GPU, the runtime, the RTX 30 self-test (a child process, cached per GPU / driver): nvl_host.h
const GpuInfo& Probe(const VSMap* in, const VSAPI* a)
{
    return ProbeGated(int(NArgI(in, a, "cuda_device", -1)), NArg(in, a, "runtime_dir"), [](const std::string& m) { Log(m); });
}

uint32_t FamilyOf(GpuClass c)
{
    switch (c) {
    case GpuClass::Rtx50: return 1; case GpuClass::Rtx40: case GpuClass::Datacenter: return 2; case GpuClass::Rtx30: return 3;
    case GpuClass::Rtx20: return 4; case GpuClass::Gtx16: return 5; default: return 0;
    }
}

void SetProps(NInst* d, const VSAPI* a, VSFrame* f, int64_t n, int kind)
{
    VSMap* p = a->getFramePropertiesRW(f);
    const int64_t s = n / d->k;
    auto it = d->meta.find(s);
    if (it != d->meta.end() && it->second.props) a->copyMap(it->second.props.get(), p);
    // mpv reads the output's colour only from _MP_IMAGE (the source's, just copied): RTX Video HDR (SDR in, PQ BT.2020
    // out) or an SDR output of an HDR source is told in the blob (mp_out.h); validated before the _ColorRange below
    if (d->srcHdr != d->outHdr) {
        std::string why;
        const bool ok = TagMpOutput(a, p, d->outHdr ? 16 : 1, d->outHdr ? 9 : 1, d->outHdr ? 9 : 1, 1, d->outHdr ? d->hdrPeak : 0.f, &why);
        if (d->mpTagLog++ == 0)
            Log(ok ? std::string("nvidia: output colour for mpv: ") + (d->outHdr ? "HDR10 (PQ BT.2020)" : "SDR BT.709") + ", the source is " + (d->srcHdr ? "HDR" : "SDR")
                   : "nvidia: output colour for mpv not set (" + why + "): mpv keeps the source's colour for this output");
    }
    const int64_t dn = it != d->meta.end() && it->second.durNum > 0 ? it->second.durNum : d->inVi.fpsDen;
    const int64_t dd = it != d->meta.end() && it->second.durNum > 0 ? it->second.durDen : d->inVi.fpsNum;
    const Rational dur = OutputDuration(dn, dd, d->k);
    if (dur.num > 0) { a->mapSetInt(p, "_DurationNum", dur.num, maReplace); a->mapSetInt(p, "_DurationDen", dur.den, maReplace); }
    a->mapSetInt(p, "_Matrix", d->outHdr ? 9 : 1, maReplace);
    a->mapSetInt(p, "_Transfer", d->outHdr ? 16 : 1, maReplace);
    a->mapSetInt(p, "_Primaries", d->outHdr ? 9 : 1, maReplace);
    a->mapSetInt(p, "_ColorRange", 1, maReplace);   // older VapourSynth: 1 limited
    a->mapSetInt(p, "_Range", 0, maReplace);        // API 4.2+: 0 limited
    a->mapSetInt(p, "_ChromaLocation", 1, maReplace);   // the 2x2 average: centre
    a->mapSetInt(p, "PushMPCFrameKind", kind, maReplace);
    a->mapSetInt(p, "PushMPCSource", s, maReplace);
}

void Bind(NInst* d)
{
    d->be.source = [d](int64_t s, SrcView& v) -> bool {
        const VSFrame* f = d->api->getFrameFilter(int(s), d->node, d->fc);
        if (!f) return false;
        d->held.push_back(f);
        const VSMap* p = d->api->getFramePropertiesRO(f);
        NMeta m;
        int e1 = 0, e2 = 0;
        m.durNum = d->api->mapGetInt(p, "_DurationNum", 0, &e1); m.durDen = d->api->mapGetInt(p, "_DurationDen", 0, &e2);
        if (e1 || e2) { m.durNum = 0; m.durDen = 0; }
        { const VSAPI* api = d->api; VSMap* cp = api->createMap(); api->copyMap(p, cp); m.props = std::shared_ptr<VSMap>(cp, [api](VSMap* x) { api->freeMap(x); }); }
        d->meta[s] = m;
        while (d->meta.size() > 64) d->meta.erase(d->meta.begin());
        for (int i = 0; i < 3; ++i) { v.data[i] = d->api->getReadPtr(f, i); v.stride[i] = d->api->getStride(f, i); }
        v.width = d->api->getFrameWidth(f, 0); v.height = d->api->getFrameHeight(f, 0);
        v.bits = d->api->getVideoFrameFormat(f)->bitsPerSample;
        v.fmt = d->colour; v.fmt.bits = v.bits;
        bool hm = false, hr = false, hn = false;
        int64_t mx = PInt(p, d->api, "_Matrix", 2, &hm);
        if (!hm) mx = PInt(p, d->api, "_ColorSpace", 2, &hm);   // mpv's vf_vapoursynth: _ColorSpace (AVColorSpace = H.273), no _Matrix
        if (hm && mx != 2) v.fmt.matrix = int(mx);
        const int64_t rn = PInt(p, d->api, "_Range", 0, &hn);   // API 4.2+: 1 full
        if (hn) v.fmt.full = rn == 1;
        else { const int64_t cr = PInt(p, d->api, "_ColorRange", 1, &hr); if (hr) v.fmt.full = cr == 0; }
        return true;
    };
    d->be.alloc = [d](const OutView& o, void*& h) -> bool {
        VSFrame* f = d->api->newVideoFrame(&d->vi.format, d->vi.width, d->vi.height, nullptr, d->core);
        if (!f) return false;
        const int bps = o.bits > 8 ? 2 : 1;
        for (int i = 0; i < 3; ++i) {
            uint8_t* dst = d->api->getWritePtr(f, i);
            const ptrdiff_t ds = d->api->getStride(f, i);
            const int rows = d->api->getFrameHeight(f, i), row = d->api->getFrameWidth(f, i) * bps;
            for (int y = 0; y < rows; ++y) std::memcpy(dst + ds * y, o.data[i] + o.pitch[i] * size_t(y), size_t(row));
        }
        h = f;
        return true;
    };
    d->be.release = [d](void* f) { d->api->freeFrame(static_cast<const VSFrame*>(f)); };
    d->be.tag = [d](int64_t pts, bool generated, void* f) { SetProps(d, d->api, static_cast<VSFrame*>(f), d->pipe.GridOf().Index(pts), generated ? 1 : 0); };
}

const VSFrame* VS_CC NGetFrame(int n, int reason, void* inst, void** frameData, VSFrameContext* fc, VSCore* core, const VSAPI* a)
{
    auto* d = static_cast<NInst*>(inst);
    Warm& w = W();
    if (reason == arInitial) {
        int64_t from, to;
        { std::lock_guard<std::mutex> lk(w.lock); d->pipe.Requests(n, from, to); }
        frameData[0] = reinterpret_cast<void*>(intptr_t(from));
        for (int64_t i = from; i <= to; ++i) a->requestFrameFilter(int(i), d->node, fc);
        return nullptr;
    }
    if (reason != arAllFramesReady) return nullptr;
    const int64_t requestedFrom = int64_t(reinterpret_cast<intptr_t>(frameData[0]));
    std::lock_guard<std::mutex> lk(w.lock);
    if (w.owner != d->token) { a->setFilterError("pushmpc.Enhance (nvidia): a newer script instance took over the engine (only one Enhance per process)", fc); return nullptr; }
    d->core = core; d->api = a; d->fc = fc;
    auto r = d->pipe.Produce(n, requestedFrom, d->be, 10000);
    for (const VSFrame* f : d->held) a->freeFrame(f);
    d->held.clear();
    d->fc = nullptr;
    if (!r.ok) {
        Log(std::format("nvidia: frame {}: {}", n, r.err));
        a->setFilterError(("pushmpc.Enhance (nvidia): " + r.err).c_str(), fc);
        return nullptr;
    }
    const VSFrame* out = nullptr;
    if (!r.repeated) out = a->addFrameRef(static_cast<const VSFrame*>(r.frame));
    else { VSFrame* c = a->copyFrame(static_cast<const VSFrame*>(r.frame), core); SetProps(d, a, c, n, 2); out = c; }
    if (++d->produced - d->lastLogged >= 2400) {
        d->lastLogged = d->produced;
        const auto& c = d->pipe.Count();
        Log(std::format("nvidia: {} frames out: {} submitted, {} generated by VFG, {} repeated, {} resets; cut guard held {} of {} pairs",
            d->produced, c.submitted, d->be.generated, c.repeats, c.discontinuities, d->be.heldCuts, d->be.guard.Pairs()));
    }
    return out;
}

void VS_CC NFree(void* inst, VSCore*, const VSAPI* a)
{
    auto* d = static_cast<NInst*>(inst);
    {
        Warm& w = W();
        std::lock_guard<std::mutex> lk(w.lock);
        d->pipe.ClearWith([a](void* f) { a->freeFrame(static_cast<const VSFrame*>(f)); });
        if (w.owner == d->token) {   // the engine stays warm in W()
            std::string e; d->engine->Drain(e); w.owner = 0;
            double fl = 0, wa = 0; d->engine->FlowCosts(fl, wa);
            if (fl > 0 && wa > 0) { WriteFlowCosts(d->flowKey, fl, wa); Log(std::format("nvidia: optical flow measured: {:.2f} ms per pair, warp {:.2f} ms per frame (kept for Auto)", fl, wa)); }
        }
        Log(std::format("nvidia: instance closed after {} frames ({} cuts held); the engine stays warm", d->produced, d->be.heldCuts));
    }
    a->freeNode(d->node);
    delete d;
}

} // namespace

// the backend probe for SelectBackend (Linux)
void NvLinuxProbe(const VSMap* in, const VSAPI* a, BackendProbe& p)
{
    const GpuInfo& g = Probe(in, a);
    p.nvidiaBuilt = true;
    p.pushcoreLoaded = g.driver && g.runtime && (g.caps.vsr || g.caps.vfg);
    p.gpuFamily = FamilyOf(g.cls);
    p.gpuName = g.name;
    p.nvidiaNoFg = p.pushcoreLoaded && !g.caps.vfg;
    p.nvidiaNvof = p.pushcoreLoaded && g.caps.nvof;
}

// the multiplier the optical-flow tier reaches for a clip (Auto, as NvLinuxCreate would decide it): SelectBackend's
// comparison with pmml on GPUs without VFG
int NvLinuxNvofMultiplier(const VSMap* in, const VSAPI* a, int width, int height, double fps, double displayHz)
{
    const GpuInfo& g = Probe(in, a);
    if (!g.driver || !g.caps.nvof) return 0;
    Settings s = LoadSettings([](const char* n) { return ReadSetting(n); });
    if (s.smoother == 1) return 0;
    s.fgMethod = 1;
    Source src; src.w = width; src.h = height; src.fps = fps;
    Display d; d.refreshHz = displayHz;
    Costs c; c.card = CardFactor(g.name, g.cls);
    ReadFlowCosts(FlowCostKey(g.name, width, height, s.targetH), c.flowMeasured, c.warpMeasured);
    Caps caps = g.caps; caps.vfg = false;
    const nvl::Plan pl = Decide(s, src, d, caps, c);
    return pl.nvof ? pl.k : 0;
}

// node: the validated YUV 4:2:0 clip (owned from here on)
void NvLinuxCreate(const VSMap* in, VSMap* out, VSCore* core, const VSAPI* a, VSNode* node, const std::string& why)
{
    auto fail = [&](const std::string& m) { Log("nvidia: create failed: " + m); a->mapSetError(out, ("pushmpc.Enhance (nvidia): " + m).c_str()); };
    const VSVideoInfo* vi = a->getVideoInfo(node);
    Log("backend: " + why);
    const GpuInfo& g = Probe(in, a);
    if (!g.driver || !g.runtime) { a->freeNode(node); return fail(!g.driver ? g.why : g.runtimeWhy); }
    Settings s = LoadSettings([](const char* n) { return ReadSetting(n); });
    std::string overrides;
    for (const char* key : { "enable", "smoother", "sharper", "fg_quality", "sdr_to_hdr", "vsr_style", "target", "hdr_peak", "fg_method" })
        if (const std::string v = NArg(in, a, key); !v.empty() && ApplyArg(s, key, v)) overrides += std::format(" {}={}", key, v);
    if (!s.enable) { Log("nvidia: PushMPC is off (Enable = 0): the clip passes through"); a->mapConsumeNode(out, "clip", node, maReplace); return; }

    // the colour to start from: the arguments, else a guess from the format (each frame's properties then win)
    const int bits = vi->format.bitsPerSample;
    const Colour guess = GuessColour(uint32_t(vi->width), uint32_t(vi->height), bits);
    const int64_t am = NArgI(in, a, "matrix", -1), at = NArgI(in, a, "transfer", -1), ar = NArgI(in, a, "range", -1);
    InFormat fmt; fmt.bits = bits; fmt.matrix = am >= 0 ? int(am) : guess.matrix; fmt.full = ar == 0;   // VapourSynth range codes: 0 full
    const int transfer = at >= 0 ? int(at) : guess.transfer;

    Source src; src.w = vi->width; src.h = vi->height;
    src.fps = vi->fpsNum > 0 && vi->fpsDen > 0 ? double(vi->fpsNum) / double(vi->fpsDen) : 23.976;
    src.hdr = IsHdrTransfer(transfer); src.tenBit = bits > 8;
    Display disp; disp.refreshHz = NArgF(in, a, "display_fps", 0); disp.hdr = NArgI(in, a, "display_hdr", 0) != 0;
    disp.fullscreen = NArgI(in, a, "display_fs", 0) != 0;   // mpv's fullscreen property (pushmpc-display.lua)
    disp.vrr = VrrActive(disp.fullscreen ? 1 : 0) == 1;     // DRM VRR_ENABLED / NV-CONTROL (vrr_probe.h); unknown: a fixed refresh
    // pushmpc-display.lua: the display's size (UpscaleTarget Screen) and its peak (RTX Video HDR) from mpv
    const int dispH = int(NArgI(in, a, "display_h", 0));
    if (s.targetScreen && dispH > 0) s.targetH = TargetForDisplay(dispH);
    const double dispPeak = NArgF(in, a, "display_peak", 0);
    if (!s.hdrPeakSet && dispPeak >= 400) s.hdrPeak = std::min(dispPeak, 2000.0);
    Costs costs; costs.card = CardFactor(g.name, g.cls);
    ReadFlowCosts(FlowCostKey(g.name, src.w, src.h, s.targetH), costs.flowMeasured, costs.warpMeasured);   // the optical-flow tier, measured before
    const nvl::Plan plan = Decide(s, src, disp, g.caps, costs);
    Log(std::format("nvidia: {}x{} {}-bit, {}/{} fps, {} frames; {}; display {:.3f} Hz{}, HDR {}; settings from {}{}", src.w, src.h, bits, vi->fpsNum, vi->fpsDen, vi->numFrames,
        src.hdr ? "HDR (PQ/HLG)" : "SDR", disp.refreshHz, disp.refreshHz > 1 ? " (mpv's display_fps)" : " (unknown: x2)", disp.hdr ? "on" : "off",
        ResolveSettingsIni(EnvA("XDG_CONFIG_HOME"), EnvA("HOME")), overrides.empty() ? std::string() : ", overridden:" + overrides));
    Log("nvidia: Auto: " + plan.why);
    if (plan.k <= 1 && !plan.vsr && !plan.hdr) { Log("nvidia: nothing to do: the clip passes through"); a->mapConsumeNode(out, "clip", node, maReplace); return; }

    EngineConfig cfg;
    cfg.srcW = src.w; cfg.srcH = src.h; cfg.bits = bits; cfg.srcHdr = src.hdr; cfg.plan = plan; cfg.hdrPeak = float(s.hdrPeak);
    cfg.ampere = plan.k >= 2 && !plan.nvof && VfgAmpere(g.major, g.minor);
    cfg.guardFlow = plan.guardFlow;
    Warm& w = W();
    std::unique_lock<std::mutex> lk(w.lock);
    std::string err;
    if (!w.engine || !(w.engine->Config() == cfg)) {
        w.owner = 0;
        w.engine.reset();   // one engine at a time (GPU memory)
        auto e = std::make_shared<Engine>();
        if (!e->Open(g, cfg, err)) {
            // a frame-generation failure falls back to upscaling only, as pushcore's fallback chain does
            if (cfg.plan.k >= 2) {
                Log(std::string("nvidia: ") + (cfg.plan.nvof ? "the optical-flow tier" : "VFG") + " could not start (" + err + "): upscaling only");
                cfg.plan.k = 1; cfg.plan.nvof = false; cfg.ampere = false; cfg.guardFlow = false;
                e = std::make_shared<Engine>();
                if (!(cfg.plan.vsr || cfg.plan.hdr) || !e->Open(g, cfg, err)) { lk.unlock(); a->freeNode(node); return fail(err); }
            } else { lk.unlock(); a->freeNode(node); return fail(err); }
        }
        w.engine = e;
        Log("nvidia: engine open: " + w.engine->Describe());
    } else {
        if (!w.engine->Drain(err)) { lk.unlock(); a->freeNode(node); return fail(err); }
        Log("nvidia: engine reused (warm): " + w.engine->Describe());
    }
    auto* d = new NInst();
    d->node = node;
    d->inVi = *vi;
    d->engine = w.engine;
    d->token = ++w.next;
    d->flowKey = FlowCostKey(g.name, src.w, src.h, s.targetH);
    w.owner = d->token;
    d->k = std::max(1, w.engine->Config().plan.k);
    d->colour = fmt;
    d->outHdr = w.engine->OutHdr();
    d->srcHdr = src.hdr; d->hdrPeak = float(s.hdrPeak);
    d->outBits = w.engine->OutTenBit() ? 10 : 8;
    d->vi = *vi;
    if (!a->queryVideoFormat(&d->vi.format, cfYUV, stInteger, d->outBits, 1, 1, core)) { w.owner = 0; lk.unlock(); a->freeNode(node); delete d; return fail("queryVideoFormat failed"); }
    const nvl::Plan& ep = w.engine->Config().plan;
    d->vi.width = ep.vsr ? ep.outW : vi->width;
    d->vi.height = ep.vsr ? ep.outH : vi->height;
    const Rational outFps = OutputRate(vi->fpsNum, vi->fpsDen, d->k);
    d->vi.fpsNum = outFps.num; d->vi.fpsDen = outFps.den;
    d->vi.numFrames = int(OutputFrames(vi->numFrames, d->k));
    d->pipe.Init(d->k, vi->numFrames, SrcDur100ns(vi->fpsNum, vi->fpsDen));
    d->pipe.Reset();
    d->be.engine = w.engine.get();
    d->be.grid = d->pipe.GridOf();
    d->be.guard.Configure(vi->width, vi->height);
    Bind(d);
    lk.unlock();
    Log(std::format("nvidia: output YUV 4:2:0 {}-bit {} {}x{} at {}/{} fps (x{})", d->outBits, d->outHdr ? "PQ BT.2020" : "BT.709", d->vi.width, d->vi.height, outFps.num, outFps.den, d->k));
    VSFilterDependency deps[] = { { node, rpGeneral } };
    a->createVideoFilter(out, "Enhance", &d->vi, NGetFrame, NFree, fmUnordered, deps, 1, d, core);
}

} // namespace pmvs
