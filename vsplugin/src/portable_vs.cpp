// pushmpc.Enhance, backend "portable" (Windows with any GPU, Linux, macOS): PushFPS's portable core on the CPU behind the
// same Pipeline as the nvidia backend (schedule.h). The output keeps the source's format and colour (YUV 4:2:0, same
// bits); real frames are copied unchanged, generated ones come from engines/portable. Settings: platform.h (registry on
// Windows, ~/.config/pushmpc/settings.ini elsewhere), then the plugin arguments. Auto picks k from the display rate
// (mpv's display_fps) within the CPU budget this machine measures (backend.h DecidePortableK).
// Each instance has its own engine (no process-wide core: nothing is expensive to open).
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
#include "vslog.h"

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
    uint64_t produced = 0, lastLogged = 0;
    // per getFrame call (under lock)
    VSCore* core = nullptr;
    const VSAPI* api = nullptr;
    VSFrameContext* fc = nullptr;
    const VSFrame* curSrc = nullptr;           // the source frame being submitted (the outputs copy its props)
    std::vector<const VSFrame*> held;
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

void SetProps(PInst* d, const VSAPI* a, VSFrame* f, int64_t n, int kind)
{
    VSMap* p = a->getFramePropertiesRW(f);
    const int64_t s = n / d->k;
    auto it = d->meta.find(s);
    const int64_t dn = it != d->meta.end() && it->second.durNum > 0 ? it->second.durNum : d->inVi.fpsDen;
    const int64_t dd = it != d->meta.end() && it->second.durNum > 0 ? it->second.durDen : d->inVi.fpsNum;
    const Rational dur = OutputDuration(dn, dd, d->k);
    if (dur.num > 0) { a->mapSetInt(p, "_DurationNum", dur.num, maReplace); a->mapSetInt(p, "_DurationDen", dur.den, maReplace); }
    a->mapSetInt(p, "PushMPCFrameKind", kind, maReplace);   // 0 real, 1 generated, 2 repeated
    a->mapSetInt(p, "PushMPCSource", s, maReplace);
}

void Bind(PInst* d)
{
    d->be.source = [d](int64_t s, portable::YuvView& v) -> bool {
        const VSFrame* f = d->api->getFrameFilter(int(s), d->node, d->fc);
        if (!f) return false;
        d->held.push_back(f);
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
        Log(std::format("portable: instance closed after {} frames ({} scene cuts)", d->produced, d->be.engine.Stat().cuts));
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

} // namespace

// node: the validated YUV 4:2:0 clip (owned from here on); why: how the backend was chosen
void PortableCreate(const VSMap* in, VSMap* out, VSCore* core, const VSAPI* a, VSNode* node, const std::string& why)
{
    auto fail = [&](const std::string& m) { Log("portable: create failed: " + m); a->mapSetError(out, ("pushmpc.Enhance (portable): " + m).c_str()); };
    const VSVideoInfo* vi = a->getVideoInfo(node);
    PortableSettings s = LoadPortableSettings([](const char* n) { return ReadSetting(n); });
    std::string overrides;
    if (const std::string v = PArg(in, a, "enable"); !v.empty()) { s.enable = ParseBool(v, s.enable); overrides += " enable=" + v; }
    if (const std::string v = PArg(in, a, "smoother"); !v.empty()) { s.smoother = ParseSmoother(v, s.smoother); overrides += " smoother=" + v; }
    if (const std::string v = PArg(in, a, "portable_quality"); !v.empty()) { s.quality = LowerAscii(v) == "fast" || v == "0" ? 0 : 1; overrides += " portable_quality=" + v; }
    if (const std::string v = PArg(in, a, "portable_threads"); !v.empty()) { const int t = std::atoi(v.c_str()); s.threads = t > 0 && t <= 64 ? t : 0; overrides += " portable_threads=" + v; }
    Log("backend: " + why);
    if (!s.enable) {
        Log("portable: PushMPC is off (Enable = 0): the clip passes through");
        a->mapConsumeNode(out, "clip", node, maReplace);
        return;
    }
    const int w = vi->width, h = vi->height, bits = vi->format.bitsPerSample;
    const double srcFps = vi->fpsNum > 0 && vi->fpsDen > 0 ? double(vi->fpsNum) / double(vi->fpsDen) : 23.976;
    const double displayHz = PArgF(in, a, "display_fps", 0);
    const portable::Calibration cal = CalibrateCached(w, h, bits, s.threads, s.quality);
    const PortableK pk = DecidePortableK(s.smoother, srcFps, displayHz, h, cal.meMs, cal.genMs);
    Log(std::format("portable: {}x{} {}-bit, {}/{} fps, {} frames; display {:.3f} Hz{}; {} threads, quality {}; settings from {}{}", w, h, bits, vi->fpsNum, vi->fpsDen,
        vi->numFrames, displayHz, displayHz > 1 ? " (mpv's display_fps)" : " (unknown)", cal.threads, s.quality ? "balanced" : "fast",
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
    d->k = pk.k;
    d->vi = *vi;
    const Rational outFps = OutputRate(vi->fpsNum, vi->fpsDen, d->k);
    d->vi.fpsNum = outFps.num; d->vi.fpsDen = outFps.den;
    d->vi.numFrames = int(OutputFrames(vi->numFrames, d->k));
    std::string err;
    portable::Config c; c.width = w; c.height = h; c.bits = bits; c.threads = s.threads; c.quality = s.quality;
    if (!d->be.engine.Configure(c, err)) { a->freeNode(node); delete d; return fail(err); }
    d->pipe.Init(d->k, vi->numFrames, SrcDur100ns(vi->fpsNum, vi->fpsDen));
    d->pipe.Reset();
    d->be.grid = d->pipe.GridOf();
    Bind(d);
    Log(std::format("portable: output YUV 4:2:0 {}-bit {}x{} at {}/{} fps (x{})", bits, w, h, outFps.num, outFps.den, d->k));
    VSFilterDependency deps[] = { { node, rpGeneral } };
    a->createVideoFilter(out, "Enhance", &d->vi, PGetFrame, PFree, fmUnordered, deps, 1, d, core);
}

} // namespace pmvs
