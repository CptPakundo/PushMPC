// pushmpc.Enhance, backend "portable" (Windows with any GPU, Linux, macOS): PushFPS's portable frame generation behind
// the same Pipeline as the nvidia backend (schedule.h), on one of two engines:
//   GPU  engines/gpu: Vulkan 1.2 compute (MoltenVK on macOS) on the best Vulkan device: GPU motion estimation (PushFPS's
//        block matcher; NVIDIA's optical-flow hardware through VK_NV_optical_flow on request), interpolation, the
//        cut guard, Dedup (anime on twos / threes), Subtitle Hold, NIS / FSR1 upscaling to the screen;
//   CPU  engines/portable: PushFPS's portable core on the CPU, when there is no usable Vulkan device (the log says why)
//        or PortableDevice = cpu.
// The output keeps the source's format and colour (YUV 4:2:0, same bits), at the upscaled size when the GPU engine
// upscales; real frames are copied unchanged when it does not. Settings: platform.h (registry on Windows,
// ~/.config/pushmpc/settings.ini elsewhere), then the plugin arguments. Auto picks k from the display rate (mpv's
// display_fps) within the budget this machine measures (backend.h DecidePortableK). Frame property PushMPCEngine says
// which engine made the frame ("gpu" or "cpu").
// Each instance has its own engine; the Vulkan device is shared by the process (vk_context.h).
#include <algorithm>
#include <format>
#include <cstdlib>
#include <map>
#include <mutex>
#include <tuple>
#include <string>
#include <vector>
#include "VapourSynth4.h"
#include "backend.h"
#include "platform.h"
#include "portable_backend.h"
#include "schedule.h"
#include "vs_content.h"
#include "vslog.h"
#ifdef PMVS_GPU
#include "gpu_backend.h"
#endif

namespace pmvs {

namespace {

struct PMeta { int64_t durNum = 0, durDen = 0; };

struct PInst {
    VSNode* node = nullptr;
    VSVideoInfo vi{}, inVi{};
    int k = 1;
    std::mutex lock;
    Pipeline<void*> pipe;
    portable::PortableBackend<void*> be;
    std::map<int64_t, PMeta> meta;
    uint64_t produced = 0, lastLogged = 0, srcErrors = 0;
    // per getFrame call (under lock)
    VSCore* core = nullptr;
    const VSAPI* api = nullptr;
    VSFrameContext* fc = nullptr;
    const VSFrame* curSrc = nullptr;           // the source frame being submitted (the outputs copy its props)
    std::vector<const VSFrame*> held;
    pmvs::VsContent content;
};

std::string PArg(const VSMap* in, const VSAPI* a, const char* key)
{
    int err = 0;
    const char* v = a->mapGetData(in, key, 0, &err);
    return err || !v ? std::string() : std::string(v);
}
double PArgF(const VSMap* in, const VSAPI* a, const char* key, double def)
{
    int err = 0;
    const double v = a->mapGetFloat(in, key, 0, &err);
    return err ? def : v;
}
int64_t PArgI(const VSMap* in, const VSAPI* a, const char* key, int64_t def)
{
    int err = 0;
    const int64_t v = a->mapGetInt(in, key, 0, &err);
    return err ? def : v;
}

// the frame properties every output gets: duration (source duration / k), kind, source index, engine
void SetPropsOf(const VSAPI* a, VSFrame* f, int64_t n, int k, const std::map<int64_t, PMeta>& meta, const VSVideoInfo& inVi, int kind, const char* engine)
{
    VSMap* p = a->getFramePropertiesRW(f);
    const int64_t s = n / k;
    auto it = meta.find(s);
    const int64_t dn = it != meta.end() && it->second.durNum > 0 ? it->second.durNum : inVi.fpsDen;
    const int64_t dd = it != meta.end() && it->second.durNum > 0 ? it->second.durDen : inVi.fpsNum;
    const Rational dur = OutputDuration(dn, dd, k);
    if (dur.num > 0) { a->mapSetInt(p, "_DurationNum", dur.num, maReplace); a->mapSetInt(p, "_DurationDen", dur.den, maReplace); }
    a->mapSetInt(p, "PushMPCFrameKind", kind, maReplace);   // 0 real, 1 generated, 2 repeated
    a->mapSetInt(p, "PushMPCSource", s, maReplace);
    a->mapSetData(p, "PushMPCEngine", engine, -1, dtUtf8, maReplace);
}
void SetProps(PInst* d, const VSAPI* a, VSFrame* f, int64_t n, int kind) { SetPropsOf(a, f, n, d->k, d->meta, d->inVi, kind, "cpu"); }

void Bind(PInst* d)
{
    d->be.source = [d](int64_t s, portable::YuvView& v) -> bool {
        const VSFrame* f = d->api->getFrameFilter(int(s), d->node, d->fc);
        if (!f) return false;
        d->held.push_back(f);
        d->content.Push(d->api, f, s);
        d->curSrc = f;
        const VSMap* p = d->api->getFramePropertiesRO(f);
        int e1 = 0, e2 = 0;
        PMeta m; m.durNum = d->api->mapGetInt(p, "_DurationNum", 0, &e1); m.durDen = d->api->mapGetInt(p, "_DurationDen", 0, &e2);
        if (e1 || e2) m = {};
        d->meta[s] = m;
        while (d->meta.size() > 64) d->meta.erase(d->meta.begin());
        for (int i = 0; i < 3; ++i) { v.data[i] = d->api->getReadPtr(f, i); v.stride[i] = d->api->getStride(f, i); }
        v.width = d->api->getFrameWidth(f, 0); v.height = d->api->getFrameHeight(f, 0);
        v.bits = d->api->getVideoFrameFormat(f)->bitsPerSample;
        return true;
    };
    d->be.alloc = [d](portable::YuvOut& o, void*& h) -> bool {
        VSFrame* f = d->api->newVideoFrame(&d->vi.format, d->vi.width, d->vi.height, d->curSrc, d->core);
        if (!f) return false;
        for (int i = 0; i < 3; ++i) { o.data[i] = d->api->getWritePtr(f, i); o.stride[i] = d->api->getStride(f, i); }
        h = f;
        return true;
    };
    d->be.release = [d](void* f) { d->api->freeFrame(static_cast<const VSFrame*>(f)); };
    d->be.tag = [d](int64_t pts, bool generated, void* f) { SetProps(d, d->api, static_cast<VSFrame*>(f), d->pipe.GridOf().Index(pts), generated ? 1 : 0); };
}

const VSFrame* VS_CC PGetFrame(int n, int reason, void* inst, void** frameData, VSFrameContext* fc, VSCore* core, const VSAPI* a)
{
    auto* d = static_cast<PInst*>(inst);
    if (reason == arInitial) {
        int64_t from, to;
        { std::lock_guard<std::mutex> lk(d->lock); d->pipe.Requests(n, from, to); }
        frameData[0] = reinterpret_cast<void*>(intptr_t(from));
        for (int64_t i = from; i <= to; ++i) a->requestFrameFilter(int(i), d->node, fc);
        return nullptr;
    }
    if (reason == arError) {
        std::lock_guard<std::mutex> lk(d->lock);
        LogSourceError("portable (CPU)", n, int64_t(reinterpret_cast<intptr_t>(frameData[0])), d->srcErrors);
        return nullptr;
    }
    if (reason != arAllFramesReady) return nullptr;
    const int64_t requestedFrom = int64_t(reinterpret_cast<intptr_t>(frameData[0]));
    std::lock_guard<std::mutex> lk(d->lock);
    d->core = core; d->api = a; d->fc = fc;
    auto r = d->pipe.Produce(n, requestedFrom, d->be, 10000);
    for (const VSFrame* f : d->held) a->freeFrame(f);
    d->held.clear();
    d->fc = nullptr; d->curSrc = nullptr;
    if (!r.ok) {
        Log(std::format("portable: frame {}: {}", n, r.err));
        a->setFilterError(("pushmpc.Enhance (portable): " + r.err).c_str(), fc);
        return nullptr;
    }
    const VSFrame* out = nullptr;
    if (!r.repeated) out = a->addFrameRef(static_cast<const VSFrame*>(r.frame));
    else {
        VSFrame* c = a->copyFrame(static_cast<const VSFrame*>(r.frame), core);
        SetProps(d, a, c, n, 2);
        out = c;
    }
    if (++d->produced - d->lastLogged >= 2400) {
        d->lastLogged = d->produced;
        const auto& c = d->pipe.Count();
        const auto& st = d->be.engine.Stat();
        Log(std::format("portable: {} frames out: {} submitted, {} generated, {} repeated, {} resets; {} scene cuts; motion {:.1f} ms, generated frame {:.1f} ms",
            d->produced, c.submitted, c.generated, c.repeats, c.discontinuities, st.cuts, st.meMs, st.genMs));
    }
    return out;
}

void VS_CC PFree(void* inst, VSCore*, const VSAPI* a)
{
    auto* d = static_cast<PInst*>(inst);
    {
        std::lock_guard<std::mutex> lk(d->lock);
        d->pipe.ClearWith([a](void* f) { a->freeFrame(static_cast<const VSFrame*>(f)); });
        Log(std::format("portable: instance closed after {} frames ({} scene cuts, {} held repeats)", d->produced, d->be.engine.Stat().cuts, d->be.repeats));
    }
    a->freeNode(d->node);
    delete d;
}

// the measured cost per size (one calibration per process and size: mpv reloads the script on every seek)
portable::Calibration CalibrateCached(int w, int h, int bits, int threads, int quality)
{
    static std::mutex m;
    static std::map<std::tuple<int, int, int, int, int>, portable::Calibration> cache;
    std::lock_guard<std::mutex> lk(m);
    auto key = std::make_tuple(w, h, bits, threads, quality);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    const portable::Calibration c = portable::Calibrate(w, h, bits, threads, quality);
    cache[key] = c;
    return c;
}

#ifdef PMVS_GPU
// ------------------------------------------------------------------------------------------------ the GPU engine
struct GInst {
    VSNode* node = nullptr;
    VSVideoInfo vi{}, inVi{};
    int k = 1;
    std::mutex lock;
    Pipeline<void*> pipe;
    pmvs::VsContent content;
    gpu::GpuBackend<void*> be;
    std::map<int64_t, PMeta> meta;
    uint64_t produced = 0, lastLogged = 0, srcErrors = 0;
    VSCore* core = nullptr;
    const VSAPI* api = nullptr;
    VSFrameContext* fc = nullptr;
};

void BindGpu(GInst* d)
{
    d->be.source = [d](int64_t s, gpu::YuvView& v, void*& keep) -> bool {
        const VSFrame* f = d->api->getFrameFilter(int(s), d->node, d->fc);
        if (!f) return false;
        keep = const_cast<VSFrame*>(f);
        d->content.Push(d->api, f, s);
        const VSMap* p = d->api->getFramePropertiesRO(f);
        int e1 = 0, e2 = 0;
        PMeta m; m.durNum = d->api->mapGetInt(p, "_DurationNum", 0, &e1); m.durDen = d->api->mapGetInt(p, "_DurationDen", 0, &e2);
        if (e1 || e2) m = {};
        d->meta[s] = m;
        while (d->meta.size() > 64) d->meta.erase(d->meta.begin());
        for (int i = 0; i < 3; ++i) { v.data[i] = d->api->getReadPtr(f, i); v.stride[i] = d->api->getStride(f, i); }
        v.width = d->api->getFrameWidth(f, 0); v.height = d->api->getFrameHeight(f, 0);
        v.bits = d->api->getVideoFrameFormat(f)->bitsPerSample;
        return true;
    };
    d->be.unkeep = [d](void* k) { d->api->freeFrame(static_cast<const VSFrame*>(k)); };
    d->be.alloc = [d](gpu::YuvOut& o, void*& h, void* propSrc) -> bool {
        VSFrame* f = d->api->newVideoFrame(&d->vi.format, d->vi.width, d->vi.height, static_cast<const VSFrame*>(propSrc), d->core);
        if (!f) return false;
        for (int i = 0; i < 3; ++i) { o.data[i] = d->api->getWritePtr(f, i); o.stride[i] = d->api->getStride(f, i); }
        h = f;
        return true;
    };
    d->be.release = [d](void* f) { d->api->freeFrame(static_cast<const VSFrame*>(f)); };
    d->be.tag = [d](int64_t pts, int kind, void* f) { SetPropsOf(d->api, static_cast<VSFrame*>(f), d->pipe.GridOf().Index(pts), d->k, d->meta, d->inVi, kind, "gpu"); };
}

const VSFrame* VS_CC GGetFrame(int n, int reason, void* inst, void** frameData, VSFrameContext* fc, VSCore* core, const VSAPI* a)
{
    auto* d = static_cast<GInst*>(inst);
    if (reason == arInitial) {
        int64_t from, to;
        { std::lock_guard<std::mutex> lk(d->lock); d->pipe.Requests(n, from, to); }
        frameData[0] = reinterpret_cast<void*>(intptr_t(from));
        for (int64_t i = from; i <= to; ++i) a->requestFrameFilter(int(i), d->node, fc);
        return nullptr;
    }
    if (reason == arError) {
        std::lock_guard<std::mutex> lk(d->lock);
        LogSourceError("portable (GPU)", n, int64_t(reinterpret_cast<intptr_t>(frameData[0])), d->srcErrors);
        return nullptr;
    }
    if (reason != arAllFramesReady) return nullptr;
    const int64_t requestedFrom = int64_t(reinterpret_cast<intptr_t>(frameData[0]));
    std::lock_guard<std::mutex> lk(d->lock);
    d->core = core; d->api = a; d->fc = fc;
    auto r = d->pipe.Produce(n, requestedFrom, d->be, 10000);
    d->fc = nullptr;
    if (!r.ok) {
        Log(std::format("portable (GPU): frame {}: {}", n, r.err));
        a->setFilterError(("pushmpc.Enhance (portable, GPU): " + r.err).c_str(), fc);
        return nullptr;
    }
    const VSFrame* out = nullptr;
    if (!r.repeated) out = a->addFrameRef(static_cast<const VSFrame*>(r.frame));
    else {
        VSFrame* c = a->copyFrame(static_cast<const VSFrame*>(r.frame), core);
        SetPropsOf(a, c, n, d->k, d->meta, d->inVi, 2, "gpu");
        out = c;
    }
    if (++d->produced - d->lastLogged >= 2400) {
        d->lastLogged = d->produced;
        const auto& c = d->pipe.Count();
        const auto& st = d->be.stats;
        const auto rs = d->be.repair.Stats();
        Log(std::format("portable (GPU): {} frames out: {} submitted ({} duplicates), {} generated, {} repeated, {} resets; {} pairs, {} cuts ({} by the guard), "
                        "{} pairs with subtitles held; motion {:.1f} ms a pair; held-drawing repair: {} frames, {} parts, {:.1f} ms a frame, spans gated {}, over budget {}",
            d->produced, c.submitted, st.duplicates, st.generated, c.repeats, c.discontinuities, st.pairs, st.cuts, st.guardCuts, st.heldPairs,
            st.pairs ? st.meMs / double(st.pairs) : 0.0, rs.frames, rs.parts, rs.frames ? rs.ms / double(rs.frames) : 0.0, rs.gated, rs.overBudget));
    }
    return out;
}

void VS_CC GFree(void* inst, VSCore*, const VSAPI* a)
{
    auto* d = static_cast<GInst*>(inst);
    {
        std::lock_guard<std::mutex> lk(d->lock);
        d->api = a;
        d->pipe.ClearWith([a](void* f) { a->freeFrame(static_cast<const VSFrame*>(f)); });
        std::string err;
        d->be.Flush(err);
        const auto& st = d->be.stats;
        Log(std::format("portable (GPU): instance closed after {} frames ({} duplicates, {} cuts, {} by the guard)", d->produced, st.duplicates, st.cuts, st.guardCuts));
    }
    a->freeNode(d->node);
    delete d;
}

struct GpuCalKey { int w, h, bits, ow, oh, up, quality; std::string device; bool operator<(const GpuCalKey& o) const { return std::tie(w, h, bits, ow, oh, up, quality, device) < std::tie(o.w, o.h, o.bits, o.ow, o.oh, o.up, o.quality, o.device); } };

// the GPU path, or false (with the reason) when there is no usable Vulkan device: the caller then runs the CPU engine
bool GpuCreate(const VSMap* in, VSMap* out, VSCore* core, const VSAPI* a, VSNode* node, const PortableSettings& s, const std::string& overrides, std::string& why)
{
    // the declared rate is replaced by the one measured on this stream by an earlier instance (vs_content.h)
    VSVideoInfo viM = *a->getVideoInfo(node);
    std::string rateNote; const std::string cdKey = pmvs::ApplyMeasuredRate(viM, &rateNote);
    if (!rateNote.empty()) Log(rateNote);
    const pmvs::StreamMemo cdMemo = pmvs::Recall(cdKey);
    const VSVideoInfo* vi = &viM;
    const int w = vi->width, h = vi->height, bits = vi->format.bitsPerSample;
    gpu::DeviceRequest req;
    req.device = s.device;
    if (const char* e = std::getenv("PUSHMPC_VK_ALLOW_CPU")) req.allowCpu = e[0] == '1';
    const int dispW = int(PArgI(in, a, "display_w", 0)), dispH = int(PArgI(in, a, "display_h", 0));
    const UpscaleChoice up = DecideUpscale(s.upscaler, s.target, dispW, dispH, w, h);
    auto* d = new GInst();
    gpu::Config c;
    c.width = w; c.height = h; c.bits = bits;
    c.quality = s.quality < 0 ? 1 : s.quality;   // Auto: 16 px vectors (the CI benchmark's best on average with the refined rule)
    c.upscaler = up.kind == GpuUpscaler::Nis ? gpu::Upscaler::Nis : up.kind == GpuUpscaler::Fsr ? gpu::Upscaler::Fsr : gpu::Upscaler::None;
    c.outWidth = up.w; c.outHeight = up.h;
    c.sharpness = float(std::clamp(PArgF(in, a, "sharpness", up.kind == GpuUpscaler::Nis ? kDefaultSharpnessNis : kDefaultSharpnessFsr), 0.0, 1.0));
    c.sourceSlots = 8; c.outSlots = 10;
    // motion: Auto = hybrid where the device has NVIDIA optical flow (VK_NV_optical_flow, RTX): the flow is one more
    // block-matcher candidate and the block matcher's SAD picks. The release test matrix (RTX 5090, 11 clips, held-out
    // frames): hybrid 34.44 / 31.55 dB PSNR-Y at distance 2 / 4 against the block matcher's 34.42 / 31.50, and faster at
    // 1080p x4 (477 vs 426 outputs/s); the flow alone was worse (34.06 dB, 4 dB under on zooms). Without the flow hardware
    // the engine runs the block matcher. motion=blocks / nvof / hybrid (PortableMotion the same) choose by hand.
    const bool hybrid = s.motion == "hybrid" || s.motion == "auto" || s.motion.empty();
    c.blockMatcherOnly = s.motion != "nvof" && !hybrid;
    c.nvofHybrid = hybrid;
    // the held-drawing repair (HeldFrames.h, with Dedup; PUSHMPC_REGION_REPAIR=0 turns it off): the slots also hold the
    // frames a part's pair can start at (kLookahead + 1 behind) and end at (kLookahead ahead)
    c.regionRepair = gpu::RegionRepairWanted(s.dedup != 0);
    if (c.regionRepair) c.sourceSlots = 14;
    if (!d->be.engine.Configure(c, req, why)) { delete d; return false; }
    const double srcFps = vi->fpsNum > 0 && vi->fpsDen > 0 ? double(vi->fpsNum) / double(vi->fpsDen) : 23.976;
    const double displayHz = PArgF(in, a, "display_fps", 0);
    // Auto's budget: measured once per process, size and device (mpv reloads the script on every seek)
    static std::mutex calLock;
    static std::map<GpuCalKey, gpu::GpuCalibration> calCache;
    gpu::GpuCalibration cal;
    {
        std::lock_guard<std::mutex> lk(calLock);
        const GpuCalKey key{ w, h, bits, c.outWidth, c.outHeight, int(c.upscaler), c.quality, s.device };
        auto it = calCache.find(key);
        if (it != calCache.end()) cal = it->second;
        else { cal = gpu::CalibrateGpu(d->be.engine); calCache[key] = cal; }
    }
    if (!cal.ok) { why = "the GPU engine failed its trial run on " + d->be.engine.Device().name; delete d; return false; }
    const PortableK pk = DecidePortableK(s.smoother, srcFps, displayHz, h, cal.meMs, cal.genMs, true,
                                         PArgI(in, a, "display_fs", -1) == 1 ? 0.0 : -1.0, VrrActive(int(PArgI(in, a, "display_fs", -1))));   // pushmpc-display.lua; vrr_probe.h
    Log(std::format("portable: GPU engine on {}; motion from {}; {}x{} {}-bit, {}/{} fps, {} frames; display {:.3f} Hz{}; quality {}; settings from {}{}",
        d->be.engine.Device().Describe(), !d->be.engine.OpticalFlowActive() ? "PushFPS's block matcher on the GPU" : c.nvofHybrid ? "PushFPS's block matcher with NVIDIA optical flow as a candidate (hybrid)" : "NVIDIA optical flow (VK_NV_optical_flow)",
        w, h, bits, vi->fpsNum, vi->fpsDen, vi->numFrames, displayHz, displayHz > 1 ? " (mpv's display_fps)" : " (unknown)",
        c.quality == 0 ? "fast" : c.quality == 1 ? "balanced" : "high",
#ifdef _WIN32
        "HKCU\\Software\\PushMPC",
#else
        ResolveSettingsIni(EnvA("XDG_CONFIG_HOME"), EnvA("HOME")),
#endif
        overrides.empty() ? std::string() : ", overridden:" + overrides));
    Log("portable: " + up.why);
    Log("portable: " + pk.why);
    if (pk.k <= 1 && c.upscaler == gpu::Upscaler::None) {
        Log("portable: x1: the clip passes through");
        delete d;
        a->mapConsumeNode(out, "clip", node, maReplace);
        return true;
    }
    d->node = node;
    d->inVi = *vi;
    d->content.Init(cdKey, *vi, cdMemo.cls, cdMemo.cg);
    d->k = std::max(1, pk.k);
    d->vi = *vi;
    d->vi.width = d->be.engine.OutWidth(); d->vi.height = d->be.engine.OutHeight();
    const Rational outFps = OutputRate(vi->fpsNum, vi->fpsDen, d->k);
    d->vi.fpsNum = outFps.num; d->vi.fpsDen = outFps.den;
    d->vi.numFrames = int(OutputFrames(vi->numFrames, d->k));
    d->be.dedup = s.dedup != 0;
    d->be.subtitleHold = s.subtitleHold != 0;
    d->be.repair.on = d->be.dedup && d->be.engine.RepairAvailable();
    d->be.cadence.Configure(d->k, d->be.dedup ? 3 : 0, vi->numFrames);   // lookahead 3: twos, threes and fours (TV anime holds slow motion for four)
    d->pipe.Init(d->k, vi->numFrames, SrcDur100ns(vi->fpsNum, vi->fpsDen));
    d->pipe.SetLookahead(d->be.Lookahead());
    d->pipe.Reset();
    d->be.grid = d->pipe.GridOf();
    d->api = a;
    BindGpu(d);
    Log(std::format("portable: output YUV 4:2:0 {}-bit {}x{} at {}/{} fps (x{}); Dedup {} (lookahead {}), held-drawing repair {}, Subtitle Hold {}, cut guard on",
        bits, d->vi.width, d->vi.height, outFps.num, outFps.den, d->k, d->be.dedup ? "on" : "off", d->be.Lookahead(),
        d->be.repair.on ? "on" : c.regionRepair ? "off (this build has no repair shaders)" : "off", d->be.subtitleHold ? "on" : "off"));
    VSFilterDependency deps[] = { { node, rpGeneral } };
    a->createVideoFilter(out, "Enhance", &d->vi, GGetFrame, GFree, fmUnordered, deps, 1, d, core);
    return true;
}
#endif

} // namespace

// node: the validated YUV 4:2:0 clip (owned from here on); why: how the backend was chosen
void PortableCreate(const VSMap* in, VSMap* out, VSCore* core, const VSAPI* a, VSNode* node, const std::string& why)
{
    auto fail = [&](const std::string& m) { Log("portable: create failed: " + m); a->mapSetError(out, ("pushmpc.Enhance (portable): " + m).c_str()); };
    // the declared rate is replaced by the one measured on this stream by an earlier instance (vs_content.h)
    VSVideoInfo viM = *a->getVideoInfo(node);
    std::string rateNote; const std::string cdKey = pmvs::ApplyMeasuredRate(viM, &rateNote);
    if (!rateNote.empty()) Log(rateNote);
    const pmvs::StreamMemo cdMemo = pmvs::Recall(cdKey);
    const VSVideoInfo* vi = &viM;
    PortableSettings s = LoadPortableSettings([](const char* n) { return ReadSetting(n); });
    std::string overrides;
    if (const std::string v = PArg(in, a, "enable"); !v.empty()) { s.enable = ParseBool(v, s.enable); overrides += " enable=" + v; }
    if (const std::string v = PArg(in, a, "smoother"); !v.empty()) { s.smoother = ParseSmoother(v, s.smoother); overrides += " smoother=" + v; }
    if (const std::string v = PArg(in, a, "portable_quality"); !v.empty()) { s.quality = ParseQuality(v, s.quality); overrides += " portable_quality=" + v; }
    if (const std::string v = PArg(in, a, "portable_threads"); !v.empty()) { const int t = std::atoi(v.c_str()); s.threads = t > 0 && t <= 64 ? t : 0; overrides += " portable_threads=" + v; }
    if (const std::string v = PArg(in, a, "portable_device"); !v.empty()) { s.device = v; overrides += " portable_device=" + v; }
    if (const std::string v = PArg(in, a, "upscaler"); !v.empty()) { s.upscaler = LowerAscii(v); overrides += " upscaler=" + v; }
    if (const std::string v = PArg(in, a, "target"); !v.empty()) { s.target = LowerAscii(v); overrides += " target=" + v; }
    if (const std::string v = PArg(in, a, "dedup"); !v.empty()) { s.dedup = ParseTri(v, s.dedup); overrides += " dedup=" + v; }
    if (const std::string v = PArg(in, a, "subtitle_hold"); !v.empty()) { s.subtitleHold = ParseTri(v, s.subtitleHold); overrides += " subtitle_hold=" + v; }
    if (const std::string v = PArg(in, a, "motion"); !v.empty()) { s.motion = LowerAscii(v); overrides += " motion=" + v; }
    Log("backend: " + why);
    if (!s.enable) {
        Log("portable: PushMPC is off (Enable = 0): the clip passes through");
        a->mapConsumeNode(out, "clip", node, maReplace);
        return;
    }
    const int w = vi->width, h = vi->height, bits = vi->format.bitsPerSample;
#ifdef PMVS_GPU
    if (LowerAscii(s.device) != "cpu") {
        std::string gwhy;
        if (GpuCreate(in, out, core, a, node, s, overrides, gwhy)) return;
        Log("portable: no GPU engine (" + gwhy + "): the CPU engine runs");
    } else Log("portable: the CPU engine (PortableDevice = cpu)");
#else
    Log("portable: this build has no GPU engine: the CPU engine runs");
#endif
    const int cpuQuality = s.quality < 0 ? 1 : std::min(s.quality, 1);
    const double srcFps = vi->fpsNum > 0 && vi->fpsDen > 0 ? double(vi->fpsNum) / double(vi->fpsDen) : 23.976;
    const double displayHz = PArgF(in, a, "display_fps", 0);
    const portable::Calibration cal = CalibrateCached(w, h, bits, s.threads, cpuQuality);
    const PortableK pk = DecidePortableK(s.smoother, srcFps, displayHz, h, cal.meMs, cal.genMs, false, -1, VrrActive(int(PArgI(in, a, "display_fs", -1))));
    Log(std::format("portable: {}x{} {}-bit, {}/{} fps, {} frames; display {:.3f} Hz{}; {} threads, quality {}; settings from {}{}", w, h, bits, vi->fpsNum, vi->fpsDen,
        vi->numFrames, displayHz, displayHz > 1 ? " (mpv's display_fps)" : " (unknown)", cal.threads, cpuQuality ? "balanced" : "fast",
#ifdef _WIN32
        "HKCU\\Software\\PushMPC",
#else
        ResolveSettingsIni(EnvA("XDG_CONFIG_HOME"), EnvA("HOME")),
#endif
        overrides.empty() ? std::string() : ", overridden:" + overrides));
    Log("portable: " + pk.why);
    if (pk.k <= 1) {
        Log("portable: x1: the clip passes through");
        a->mapConsumeNode(out, "clip", node, maReplace);
        return;
    }
    auto* d = new PInst();
    d->node = node;
    d->inVi = *vi;
    d->content.Init(cdKey, *vi, cdMemo.cls, cdMemo.cg);
    d->k = pk.k;
    d->vi = *vi;
    const Rational outFps = OutputRate(vi->fpsNum, vi->fpsDen, d->k);
    d->vi.fpsNum = outFps.num; d->vi.fpsDen = outFps.den;
    d->vi.numFrames = int(OutputFrames(vi->numFrames, d->k));
    std::string err;
    portable::Config c; c.width = w; c.height = h; c.bits = bits; c.threads = s.threads; c.quality = cpuQuality;
    if (!d->be.engine.Configure(c, err)) { a->freeNode(node); delete d; return fail(err); }
    d->pipe.Init(d->k, vi->numFrames, SrcDur100ns(vi->fpsNum, vi->fpsDen));
    d->be.dedup = s.dedup != 0;          // held frames: lookahead 3 (twos, threes, fours), as the GPU route
    d->be.srcFrames = vi->numFrames;
    d->pipe.SetLookahead(d->be.Lookahead());
    d->pipe.Reset();
    d->be.grid = d->pipe.GridOf();
    Bind(d);
    Log(std::format("portable: output YUV 4:2:0 {}-bit {}x{} at {}/{} fps (x{}), CPU engine; Dedup {} (lookahead {})", bits, w, h, outFps.num, outFps.den, d->k,
        d->be.dedup ? "on" : "off", d->be.Lookahead()));
    VSFilterDependency deps[] = { { node, rpGeneral } };
    a->createVideoFilter(out, "Enhance", &d->vi, PGetFrame, PFree, fmUnordered, deps, 1, d, core);
}

} // namespace pmvs
