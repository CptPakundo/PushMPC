// pushmpc.Enhance, backend "ml": PushFlow (RIFE) on the GPU through pmml (pmml/): ONNX Runtime with the best EP for
// the card (TensorRT-RTX on RTX, DirectML / AMD GPU / OpenVINO / QNN on Windows, MIGraphX on Linux AMD, CoreML on
// macOS) or ncnn on Vulkan. The output keeps the source's format (YUV 4:2:0, same bits); real frames are unchanged.
// Where pmml lives: the ml_runtime argument, else the MlRuntime setting, else PMML_RUNTIME, else <plugin dir>/pmml
// (ort/<flavor>/, eps/, models/, graphs/, ncnn-models/, pmml-autobench.json written by `pmml_cli auto`).
// Auto: pmml's measured candidates for this card and size (the autobench cache) decide the backend, the tier, flow once
// and the multiplier within the frame budget; with no measurement yet: the first GPU EP found, Fast, x2.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include "VapourSynth4.h"
#include "backend.h"
#include "ml_backend.h"
#include "platform.h"
#include "pmml/autobench.h"
#include "schedule.h"
#include "vslog.h"

namespace pmvs {

namespace {

struct MMeta { int64_t durNum = 0, durDen = 0; };

struct MInst {
    VSNode* node = nullptr;
    VSVideoInfo vi{}, inVi{};
    int k = 1;
    std::mutex lock;
    Pipeline<void*> pipe;
    ml::MlBackend<void*> be;
    std::map<int64_t, MMeta> meta;
    uint64_t produced = 0, lastLogged = 0;
    VSCore* core = nullptr;
    const VSAPI* api = nullptr;
    VSFrameContext* fc = nullptr;
    const VSFrame* curSrc = nullptr;
    std::vector<const VSFrame*> held;
};

std::string MArg(const VSMap* in, const VSAPI* a, const char* key)
{
    int err = 0;
    const char* v = a->mapGetData(in, key, 0, &err);
    return err || !v ? std::string() : std::string(v);
}

void MSetProps(MInst* d, const VSAPI* a, VSFrame* f, int64_t n, int kind)
{
    VSMap* p = a->getFramePropertiesRW(f);
    const int64_t s = n / d->k;
    auto it = d->meta.find(s);
    const int64_t dn = it != d->meta.end() && it->second.durNum > 0 ? it->second.durNum : d->inVi.fpsDen;
    const int64_t dd = it != d->meta.end() && it->second.durNum > 0 ? it->second.durDen : d->inVi.fpsNum;
    const Rational dur = OutputDuration(dn, dd, d->k);
    if (dur.num > 0) { a->mapSetInt(p, "_DurationNum", dur.num, maReplace); a->mapSetInt(p, "_DurationDen", dur.den, maReplace); }
    a->mapSetInt(p, "PushMPCFrameKind", kind, maReplace);
    a->mapSetInt(p, "PushMPCSource", s, maReplace);
}

void MBind(MInst* d)
{
    d->be.source = [d](int64_t s, portable::YuvView& v) -> bool {
        const VSFrame* f = d->api->getFrameFilter(int(s), d->node, d->fc);
        if (!f) return false;
        d->held.push_back(f);
        d->curSrc = f;
        const VSMap* p = d->api->getFramePropertiesRO(f);
        int e1 = 0, e2 = 0;
        MMeta m; m.durNum = d->api->mapGetInt(p, "_DurationNum", 0, &e1); m.durDen = d->api->mapGetInt(p, "_DurationDen", 0, &e2);
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
    d->be.tag = [d](int64_t pts, bool generated, void* f) { MSetProps(d, d->api, static_cast<VSFrame*>(f), d->pipe.GridOf().Index(pts), generated ? 1 : 0); };
}

const VSFrame* VS_CC MGetFrame(int n, int reason, void* inst, void** frameData, VSFrameContext* fc, VSCore* core, const VSAPI* a)
{
    auto* d = static_cast<MInst*>(inst);
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
        Log(std::format("ml: frame {}: {}", n, r.err));
        a->setFilterError(("pushmpc.Enhance (ml): " + r.err).c_str(), fc);
        return nullptr;
    }
    const VSFrame* out = nullptr;
    if (!r.repeated) out = a->addFrameRef(static_cast<const VSFrame*>(r.frame));
    else {
        VSFrame* c = a->copyFrame(static_cast<const VSFrame*>(r.frame), core);
        MSetProps(d, a, c, n, 2);
        out = c;
    }
    if (++d->produced - d->lastLogged >= 2400) {
        d->lastLogged = d->produced;
        const auto st = d->be.interp->stats();
        Log(std::format("ml: {} frames out, {} generated, {} cuts; last pair {:.1f} ms (network {:.1f} ms, rebuild {:.1f} ms)", d->produced, d->be.generated,
                        d->be.cuts, d->be.lastMs, st.netMs, st.synthMs));
    }
    return out;
}

void VS_CC MFree(void* inst, VSCore*, const VSAPI* a)
{
    auto* d = static_cast<MInst*>(inst);
    {
        std::lock_guard<std::mutex> lk(d->lock);
        d->pipe.ClearWith([a](void* f) { a->freeFrame(static_cast<const VSFrame*>(f)); });
        Log(std::format("ml: instance closed after {} frames", d->produced));
    }
    a->freeNode(d->node);
    delete d;
}

std::string Env(const char* n) { const char* v = std::getenv(n); return v ? v : ""; }

// the measurements of this size in the autobench cache (any card entry; `pmml_cli auto` writes one per card + driver)
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
        const std::string key = text.substr(q + 1, p + suffix.size() - q - 1);
        if (pmml::cacheLoad(file, key, &r)) return r;
    }
    return r;
}

} // namespace

// the multiplier pmml's measured candidates reach for this clip (the autobench cache): -1 = not measured, 0 = none fits;
// the nvidia backend's Auto compares it with its own optical-flow tier on GPUs without VFG (backend.h PickInterp)
int MlMeasuredMultiplier(const std::string& runtimeDir, int width, int height, double fps, double displayHz)
{
    const auto results = CachedResults(runtimeDir + "/pmml-autobench.json", width, height);
    if (results.empty()) return -1;
    if (fps <= 1) fps = 23.976;
    const int wantK = displayHz > 0 ? std::clamp(int(std::floor(displayHz / fps + 0.02)), 1, 8) : 2;
    const auto c = pmml::choose(results, fps, wantK);
    return c.ok ? c.multiplier : 0;
}

bool MlRuntimePresent(const std::string& dir)
{
    if (dir.empty()) return false;
    if (FILE* f = std::fopen((dir + "/graphs/synth-yuv.onnx").c_str(), "rb")) { std::fclose(f); return true; }
    return false;
}

std::string MlRuntimeDir(const VSMap* in, const VSAPI* a)
{
    std::string d = in ? MArg(in, a, "ml_runtime") : std::string();
    if (d.empty()) d = ReadSetting("MlRuntime");
    if (d.empty()) d = Env("PMML_RUNTIME");
    // where PushMPC's Setup puts it (Windows <LOCALAPPDATA>\PushMPC\pmml; Linux / macOS setup.sh
    // ${XDG_DATA_HOME:-~/.local/share}/pushmpc/pmml), when it is there
    if (d.empty()) {
#ifdef _WIN32
        const std::string base = Env("LOCALAPPDATA");
        const std::string def = base.empty() ? std::string() : base + "\\PushMPC\\pmml";
        if (MlRuntimePresent(def)) d = def;
#else
        // inside the Flathub mpv XDG_DATA_HOME is the app's own folder; the runtime is in the user's (read-only override)
        const std::string xdg = Env("XDG_DATA_HOME"), home = Env("HOME");
        for (const std::string& def : { !xdg.empty() && xdg[0] == '/' ? xdg + "/pushmpc/pmml" : std::string(), home.empty() ? std::string() : home + "/.local/share/pushmpc/pmml" })
            if (d.empty() && MlRuntimePresent(def)) d = def;
#endif
    }
    return d;
}

void MlCreate(const VSMap* in, VSMap* out, VSCore* core, const VSAPI* a, VSNode* node, const std::string& why)
{
    auto fail = [&](const std::string& m) { Log("ml: create failed: " + m); a->freeNode(node); a->mapSetError(out, ("pushmpc.Enhance (ml): " + m).c_str()); };
    Log("backend: " + why);
    const VSVideoInfo* vi = a->getVideoInfo(node);
    const std::string rt = MlRuntimeDir(in, a);
    if (!MlRuntimePresent(rt)) return fail("no pmml runtime (graphs/synth-yuv.onnx) in '" + rt + "'");
    const int w = vi->width, h = vi->height, bits = vi->format.bitsPerSample;
    const double srcFps = vi->fpsNum > 0 && vi->fpsDen > 0 ? double(vi->fpsNum) / double(vi->fpsDen) : 23.976;
    int err = 0;
    const double displayHz = a->mapGetFloat(in, "display_fps", 0, &err);
    const std::string smoother = MArg(in, a, "smoother");
    const int fixedK = smoother.empty() ? 0 : std::atoi(smoother.c_str());
    const int wantK = fixedK >= 2 ? std::min(fixedK, 8) : (!err && displayHz > 0 ? std::clamp(int(std::floor(displayHz / srcFps + 0.02)), 1, 8) : 2);
    if (wantK <= 1) { Log("ml: x1: the clip passes through"); a->mapConsumeNode(out, "clip", node, maReplace); return; }

    pmml::Options o;
    o.runtimeDir = rt;
    o.modelsDir = MArg(in, a, "ml_models").empty() ? rt + "/models" : MArg(in, a, "ml_models");
    o.graphsDir = rt + "/graphs";
    o.ncnnModelsDir = rt + "/ncnn-models";
    o.cacheDir = rt;
    o.width = w; o.height = h;
    // colour from the first frame's props (matrix 5/6 BT.601, 9 BT.2020, else BT.709; range 1 full)
    const VSFrame* first = a->getFrame(0, node, nullptr, 0);
    int matrix = 1, range = 0;
    if (first) {
        const VSMap* p = a->getFramePropertiesRO(first);
        int e = 0;
        const int64_t m = a->mapGetInt(p, "_Matrix", 0, &e); if (!e) matrix = int(m);
        const int64_t r = a->mapGetInt(p, "_Range", 0, &e);   // VapourSynth R82+: 1 full, 0 limited
        if (!e) range = r == 1 ? 1 : 0;
        else { const int64_t c = a->mapGetInt(p, "_ColorRange", 0, &e); if (!e) range = c == 0 ? 1 : 0; }   // older: 0 full
        a->freeFrame(first);
    }
    const float kr = matrix == 5 || matrix == 6 ? 0.299f : matrix == 9 ? 0.2627f : 0.2126f, kb = matrix == 5 || matrix == 6 ? 0.114f : matrix == 9 ? 0.0593f : 0.0722f;
    o.colour = range ? pmml::Colour::full(bits, kr, kb) : pmml::Colour::limited(bits, kr, kb);

    int k = wantK;
    bool flowOnce = false;
    std::string decision;
    const std::string reqBackend = MArg(in, a, "ml_backend"), reqTier = MArg(in, a, "ml_tier");
    const auto results = CachedResults(rt + "/pmml-autobench.json", w, h);
    if (reqBackend.empty() && !results.empty()) {
        const auto c = pmml::choose(results, srcFps, wantK);
        if (c.ok) { o.backend = c.candidate.backend; o.tier = c.candidate.tier; flowOnce = c.candidate.flowOnce; k = fixedK >= 2 ? wantK : c.multiplier; decision = "Auto (measured): " + c.why; }
    }
    if (decision.empty()) {
        if (!reqBackend.empty() && !pmml::Backend::parse(reqBackend, &o.backend)) return fail("ml_backend '" + reqBackend + "' is not a pmml backend");
        if (reqBackend.empty()) {
            // no measurement: the first GPU EP this runtime offers, in quality order
            std::string log;
            auto devs = pmml::listDevices(rt, &log);
            static const char* order[] = {"trt-rtx", "dml", "amdgpu", "openvino", "migraphx", "coreml", "qnn-gpu", "vulkan", "cpu"};
            for (const char* ep : order) {
                auto it = std::find_if(devs.begin(), devs.end(), [&](const pmml::DeviceInfo& d) { return d.backend.ep == ep; });
                if (it != devs.end()) { o.backend = it->backend; break; }
            }
            o.tier = "fast";
            k = fixedK >= 2 ? wantK : 2;
            decision = "Auto (not measured yet: run pmml_cli auto): " + o.backend.key() + ", Fast, x" + std::to_string(k);
        } else {
            o.tier = reqTier.empty() ? "balanced" : reqTier;
            decision = "ml_backend " + o.backend.key() + " (your setting), x" + std::to_string(k);
        }
        flowOnce = k > 2;
    }
    if (!reqTier.empty()) o.tier = reqTier;
    if (const std::string v = MArg(in, a, "ml_flow_once"); !v.empty()) flowOnce = ParseBool(v, flowOnce);
    std::string e;
    auto interp = pmml::Interpolator::create(o, &e);
    if (!interp) return fail(e);
    Log("ml: " + decision);
    Log("ml: " + interp->describe() + (flowOnce ? "; flow once" : "; network per timestep"));

    auto* d = new MInst();
    d->node = node;
    d->inVi = *vi;
    d->k = k;
    d->vi = *vi;
    const Rational outFps = OutputRate(vi->fpsNum, vi->fpsDen, k);
    d->vi.fpsNum = outFps.num; d->vi.fpsDen = outFps.den;
    d->vi.numFrames = int(OutputFrames(vi->numFrames, k));
    d->be.interp = std::move(interp);
    d->be.bits = bits;
    d->be.flowOnce = flowOnce;
    d->pipe.Init(k, vi->numFrames, SrcDur100ns(vi->fpsNum, vi->fpsDen));
    d->pipe.Reset();
    d->be.grid = d->pipe.GridOf();
    MBind(d);
    Log(std::format("ml: output YUV 4:2:0 {}-bit {}x{} at {}/{} fps (x{})", bits, w, h, outFps.num, outFps.den, k));
    VSFilterDependency deps[] = { { node, rpGeneral } };
    a->createVideoFilter(out, "Enhance", &d->vi, MGetFrame, MFree, fmUnordered, deps, 1, d, core);
}

} // namespace pmvs
