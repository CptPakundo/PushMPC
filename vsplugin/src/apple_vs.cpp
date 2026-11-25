// pushmpc.Enhance, backend "apple" (macOS on Apple silicon): VideoToolbox frame interpolation and super resolution
// (engines/apple) behind the same Pipeline as the other backends (schedule.h). Output: the source's format (YUV 4:2:0,
// same bits, same colour) at k x the rate, at 2x (or 4x) the size with super resolution. Real frames without super
// resolution are copied unchanged. When this Mac cannot run the configuration, the portable backend takes over.
// Settings: platform.h (~/.config/pushmpc/settings.ini: Smoother, AppleSuperResolution = auto|off|2|4|editing|artcnn|
// artcnn32|artcnn16|lowlatency, AppleInterpolation, Content = live|anime (Auto's upscaler order), AppleSrModelDownload =
// auto|never, AppleTenBit = auto|off), then the plugin arguments (smoother, apple_sr, apple_fi).
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "VapourSynth4.h"
#include "apple_engine.h"
#include "backend.h"
#include "platform.h"
#include "schedule.h"
#include "vslog.h"

namespace pmvs {

void PortableCreate(const VSMap* in, VSMap* out, VSCore* core, const VSAPI* a, VSNode* node, const std::string& why);

namespace {

struct AMeta { int64_t durNum = 0, durDen = 0; };

// the backend for the Pipeline: Submit runs the engine for one source frame and queues its outputs in order
struct AppleBackendImpl : Backend<void*> {
    apple::Engine engine;
    Grid grid;
    std::function<const VSFrame*(int64_t s)> source;
    std::function<void*(int64_t s)> alloc;                 // an output frame (VSFrame*), props copied from source s
    std::function<apple::PlanarOut(void* f)> planes;
    std::function<void(void* f)> release;
    std::function<void(int64_t pts, bool generated, void* f)> tag;
    const VSAPI* api = nullptr;
    int64_t last = -1000000;
    std::vector<Out<void*>> queue;

    bool CanSubmit() override { return true; }
    bool Submit(int64_t s, int64_t pts, bool discontinuity, std::string& err) override
    {
        const VSFrame* f = source(s);
        if (!f) { err = "apple: source frame " + std::to_string(s) + " is not available"; return false; }
        apple::PlanarView v;
        for (int i = 0; i < 3; ++i) { v.data[i] = api->getReadPtr(f, i); v.stride[i] = api->getStride(f, i); }
        v.width = api->getFrameWidth(f, 0); v.height = api->getFrameHeight(f, 0);
        v.bits = api->getVideoFrameFormat(f)->bitsPerSample;
        apple::Colour c;
        const VSMap* p = api->getFramePropertiesRO(f);
        int e = 0;
        int64_t x = api->mapGetInt(p, "_Matrix", 0, &e); if (!e && x != 2) c.matrix = int(x);
        x = api->mapGetInt(p, "_Transfer", 0, &e); if (!e && x != 2) c.transfer = int(x);
        x = api->mapGetInt(p, "_Primaries", 0, &e); if (!e && x != 2) c.primaries = int(x);
        x = api->mapGetInt(p, "_Range", 0, &e);   // API 4.2+ (R82): 1 full, 0 limited
        if (!e) c.fullRange = x == 1;
        else { x = api->mapGetInt(p, "_ColorRange", 0, &e); if (!e) c.fullRange = x == 0; }   // before 4.2: 0 full, 1 limited
        const bool contiguous = !discontinuity && last == s - 1;
        const int k = grid.k;
        std::vector<void*> genFrames;
        std::vector<apple::PlanarOut> gen;
        for (int j = 1; j < k; ++j) { void* g = alloc(s); if (!g) { for (void* q : genFrames) release(q); err = "apple: could not allocate an output frame"; return false; } genFrames.push_back(g); gen.push_back(planes(g)); }
        void* curF = alloc(s);
        if (!curF) { for (void* q : genFrames) release(q); err = "apple: could not allocate an output frame"; return false; }
        bool havePair = false;
        if (!engine.Process(v, c, pts, contiguous, gen, planes(curF), havePair, err)) {
            for (void* q : genFrames) release(q);
            release(curF);
            err = "apple: " + err;
            return false;
        }
        last = s;
        if (havePair)
            for (int j = 1; j < k; ++j) {
                const int64_t gp = grid.Pts(s - 1) + int64_t(std::llround(double(j) * grid.srcDur100ns / k));
                tag(gp, true, genFrames[size_t(j - 1)]);
                queue.push_back({ gp, true, genFrames[size_t(j - 1)] });
            }
        else for (void* q : genFrames) release(q);
        tag(pts, false, curF);
        queue.push_back({ pts, false, curF });
        return true;
    }
    bool Poll(uint32_t, std::vector<Out<void*>>& out, std::string&) override { for (auto& q : queue) out.push_back(q); queue.clear(); return true; }
    bool Flush(std::string&) override { for (auto& q : queue) Release(q.frame); queue.clear(); engine.Forget(); last = -1000000; return true; }
    void Release(void* f) override { if (release) release(f); }
    uint64_t NowMs() override { return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
};

struct AInst {
    VSNode* node = nullptr;
    VSVideoInfo vi{}, inVi{};
    int k = 1;
    std::mutex lock;
    Pipeline<void*> pipe;
    AppleBackendImpl be;
    std::map<int64_t, AMeta> meta;
    std::map<int64_t, const VSFrame*> srcHeld;   // per getFrame call
    uint64_t produced = 0, lastLogged = 0;
    VSCore* core = nullptr;
    const VSAPI* api = nullptr;
    VSFrameContext* fc = nullptr;
};

std::string AArg(const VSMap* in, const VSAPI* a, const char* key)
{
    int err = 0;
    const char* v = a->mapGetData(in, key, 0, &err);
    return err || !v ? std::string() : std::string(v);
}

void ASetProps(AInst* d, VSFrame* f, int64_t n, int kind)
{
    const VSAPI* a = d->api;
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

void ABind(AInst* d)
{
    d->be.source = [d](int64_t s) -> const VSFrame* {
        const VSFrame* f = d->api->getFrameFilter(int(s), d->node, d->fc);
        if (!f) return nullptr;
        auto old = d->srcHeld.find(s);
        if (old != d->srcHeld.end()) d->api->freeFrame(old->second);
        d->srcHeld[s] = f;
        const VSMap* p = d->api->getFramePropertiesRO(f);
        int e1 = 0, e2 = 0;
        AMeta m; m.durNum = d->api->mapGetInt(p, "_DurationNum", 0, &e1); m.durDen = d->api->mapGetInt(p, "_DurationDen", 0, &e2);
        if (e1 || e2) m = {};
        d->meta[s] = m;
        while (d->meta.size() > 64) d->meta.erase(d->meta.begin());
        return f;
    };
    d->be.alloc = [d](int64_t s) -> void* {
        auto it = d->srcHeld.find(s);
        // props (mpv's _MP_IMAGE and the colour) come from the source frame; the size is the output's
        return d->api->newVideoFrame(&d->vi.format, d->vi.width, d->vi.height, it != d->srcHeld.end() ? it->second : nullptr, d->core);
    };
    d->be.planes = [d](void* h) {
        apple::PlanarOut o;
        auto* f = static_cast<VSFrame*>(h);
        for (int i = 0; i < 3; ++i) { o.data[i] = d->api->getWritePtr(f, i); o.stride[i] = d->api->getStride(f, i); }
        return o;
    };
    d->be.release = [d](void* f) { d->api->freeFrame(static_cast<const VSFrame*>(f)); };
    d->be.tag = [d](int64_t pts, bool generated, void* f) { ASetProps(d, static_cast<VSFrame*>(f), d->pipe.GridOf().Index(pts), generated ? 1 : 0); };
}

const VSFrame* VS_CC AGetFrame(int n, int reason, void* inst, void** frameData, VSFrameContext* fc, VSCore* core, const VSAPI* a)
{
    auto* d = static_cast<AInst*>(inst);
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
    d->core = core; d->api = a; d->fc = fc; d->be.api = a;
    auto r = d->pipe.Produce(n, requestedFrom, d->be, 10000);
    for (auto& kv : d->srcHeld) a->freeFrame(kv.second);
    d->srcHeld.clear();
    d->fc = nullptr;
    if (!r.ok) {
        Log(std::format("apple: frame {}: {}", n, r.err));
        a->setFilterError(("pushmpc.Enhance (apple): " + r.err).c_str(), fc);
        return nullptr;
    }
    const VSFrame* out = nullptr;
    if (!r.repeated) out = a->addFrameRef(static_cast<const VSFrame*>(r.frame));
    else {
        VSFrame* c = a->copyFrame(static_cast<const VSFrame*>(r.frame), core);
        ASetProps(d, c, n, 2);
        out = c;
    }
    if (++d->produced - d->lastLogged >= 2400) {
        d->lastLogged = d->produced;
        const auto& c = d->pipe.Count();
        const auto& st = d->be.engine.Stat();
        Log(std::format("apple: {} frames out: {} submitted, {} generated, {} repeated, {} resets; {} pairs, {} scene cuts, {} VideoToolbox calls ({} failed); pair {:.1f} ms; {}",
            d->produced, c.submitted, c.generated, c.repeats, c.discontinuities, st.pairs, st.cuts, st.vtCalls, st.vtErrors, st.pairMs, d->be.engine.Describe()));
    }
    return out;
}

void VS_CC AFree(void* inst, VSCore*, const VSAPI* a)
{
    auto* d = static_cast<AInst*>(inst);
    {
        std::lock_guard<std::mutex> lk(d->lock);
        d->pipe.ClearWith([a](void* f) { a->freeFrame(static_cast<const VSFrame*>(f)); });
        const auto& st = d->be.engine.Stat();
        Log(std::format("apple: instance closed after {} frames ({} pairs, {} scene cuts, {} VideoToolbox calls, {} failed; {})", d->produced, st.pairs, st.cuts, st.vtCalls, st.vtErrors,
            d->be.engine.Describe()));
    }
    a->freeNode(d->node);
    delete d;
}

} // namespace

// what Auto needs: does this Mac offer a VideoToolbox interpolator (once per process; the log line says what exists)
bool AppleAvailable(std::string& why)
{
    const apple::Caps& c = apple::Probe();
    static std::once_flag once;
    std::call_once(once, [&] { Log("apple probe: " + c.detail); });
    why = c.AnyInterpolation() ? (c.lowLatencyFI ? "VideoToolbox low-latency frame interpolation" : "VideoToolbox frame-rate conversion") : c.detail;
    return c.AnyInterpolation();
}

// node: the validated YUV 4:2:0 clip (owned from here on); why: how the backend was chosen
void AppleCreate(const VSMap* in, VSMap* out, VSCore* core, const VSAPI* a, VSNode* node, const std::string& why)
{
    const VSVideoInfo* vi = a->getVideoInfo(node);
    PortableSettings s = LoadPortableSettings([](const char* n) { return ReadSetting(n); });
    std::string sr = ReadSetting("AppleSuperResolution"), fi = ReadSetting("AppleInterpolation"), overrides;
    if (const std::string v = AArg(in, a, "enable"); !v.empty()) { s.enable = ParseBool(v, s.enable); overrides += " enable=" + v; }
    if (const std::string v = AArg(in, a, "smoother"); !v.empty()) { s.smoother = ParseSmoother(v, s.smoother); overrides += " smoother=" + v; }
    if (const std::string v = AArg(in, a, "apple_sr"); !v.empty()) { sr = v; overrides += " apple_sr=" + v; }
    if (const std::string v = AArg(in, a, "apple_fi"); !v.empty()) { fi = v; overrides += " apple_fi=" + v; }
    if (sr.empty()) sr = "auto";
    if (fi.empty()) fi = "auto";
    Log("backend: " + why);
    if (!s.enable) {
        Log("apple: PushMPC is off (Enable = 0): the clip passes through");
        a->mapConsumeNode(out, "clip", node, maReplace);
        return;
    }
    int err0 = 0;
    const double displayHz = a->mapGetFloat(in, "display_fps", 0, &err0);
    const double srcFps = vi->fpsNum > 0 && vi->fpsDen > 0 ? double(vi->fpsNum) / double(vi->fpsDen) : 23.976;
    const apple::AppleK ak = apple::DecideAppleK(s.smoother, srcFps, err0 ? 0.0 : displayHz);
    Log(std::format("apple: {}x{} {}-bit, {}/{} fps, {} frames; display {:.3f} Hz; {}; super resolution {}, interpolator {}{}", vi->width, vi->height, vi->format.bitsPerSample,
        vi->fpsNum, vi->fpsDen, vi->numFrames, err0 ? 0.0 : displayHz, ak.why, sr, fi, overrides.empty() ? std::string() : "; overridden:" + overrides));

    auto* d = new AInst();
    d->node = node;
    d->inVi = *vi;
    apple::Config c;
    c.width = vi->width; c.height = vi->height; c.bits = vi->format.bitsPerSample; c.k = ak.k; c.sr = sr; c.fi = fi; c.srcFps = srcFps;
    c.colour.fullRange = false;
    {
        std::string content = ReadSetting("Content");
        for (auto& ch : content) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        c.anime = content == "anime";
        if (const std::string md = ReadSetting("AppleSrModelDownload"); !md.empty()) c.modelDownload = md;
        if (const std::string tb = ReadSetting("AppleTenBit"); !tb.empty()) c.tenBit = tb;
    }
    d->be.engine.log = [](const std::string& m) { Log(m); };
    std::string err, info;
    if (!d->be.engine.Configure(c, err, info)) {
        delete d;
        Log("apple: " + err);
        if (ak.k <= 1) { Log("apple: x1 and nothing else to do: the clip passes through"); a->mapConsumeNode(out, "clip", node, maReplace); return; }
        return PortableCreate(in, out, core, a, node, why + "; the apple backend cannot run this (" + err + "): portable");
    }
    Log("apple: " + info);
    d->k = std::max(1, ak.k);
    d->vi = *vi;
    d->vi.width = d->be.engine.OutWidth(); d->vi.height = d->be.engine.OutHeight();
    const Rational outFps = OutputRate(vi->fpsNum, vi->fpsDen, d->k);
    d->vi.fpsNum = outFps.num; d->vi.fpsDen = outFps.den;
    d->vi.numFrames = int(OutputFrames(vi->numFrames, d->k));
    d->pipe.Init(d->k, vi->numFrames, SrcDur100ns(vi->fpsNum, vi->fpsDen));
    d->pipe.Reset();
    d->be.grid = d->pipe.GridOf();
    d->be.api = a;
    ABind(d);
    Log(std::format("apple: output YUV 4:2:0 {}-bit {}x{} at {}/{} fps (x{})", vi->format.bitsPerSample, d->vi.width, d->vi.height, outFps.num, outFps.den, d->k));
    VSFilterDependency deps[] = { { node, rpGeneral } };
    a->createVideoFilter(out, "Enhance", &d->vi, AGetFrame, AFree, fmUnordered, deps, 1, d, core);
}

} // namespace pmvs
