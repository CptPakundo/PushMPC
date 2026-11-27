// pushmpc.Enhance, backend "ml": PushFlow (RIFE) on the GPU through pmml (pmml/): ONNX Runtime with the best EP for
// the card (TensorRT-RTX on RTX, DirectML / AMD GPU / OpenVINO / QNN on Windows, MIGraphX on Linux AMD, CoreML on
// macOS) or ncnn on Vulkan. The output keeps the source's format (YUV 4:2:0, same bits); real frames are unchanged.
// Where pmml lives: the ml_runtime argument, else the MlRuntime setting, else PMML_RUNTIME, else <plugin dir>/pmml
// (ort/<flavor>/, eps/, models/, graphs/, ncnn-models/, pmml-autobench.json written by `pmml_cli auto`).
// Auto: pmml's measured candidates for this card and size (the autobench cache) decide the backend, the tier, flow once
// and the multiplier within the frame budget; with no measurement yet: the first GPU EP found, Fast, x2.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include "VapourSynth4.h"
#include "backend.h"
#include "ml_backend.h"
#include "vs_content.h"
#include "pmml/upscale.h"
#include "../../filter/src/PushMpcAnimeSr.h"   // the anime upscalers: model table, costs, Auto's pick
#include "../../filter/src/PushMpcAnimeKeepUp.h"   // ... whether Auto's pick keeps up in playback
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
    pmvs::VsContent content;
    // the anime upscaler (pmml::Upscaler) on each source frame before frame generation; its result per source frame
    // (the repair asks for a frame more than once), AnimeSR's carry restarts when the sources are not consecutive
    std::unique_ptr<pmml::Upscaler> up;
    struct UpFrame { std::vector<uint16_t> p16[3]; std::vector<uint8_t> p8[3]; };
    std::map<int64_t, UpFrame> upFrames;
    int64_t upLast = -2;
    std::vector<uint16_t> upIn[3];
    double upMs = 0;
    // the output size stays the upscaler's for the clip: when the model failed, or Auto's pick does not keep up in
    // playback (PushMpcAnimeKeepUp.h; only with display_fps, never in an encode, where slower than real time is normal),
    // a bicubic resize to that size runs instead: for a while (transient), or for the clip when no cheaper model of the
    // same scale fits (a real trip steps down to one first)
    int upW = 0, upH = 0;
    bool upAuto = false, upRealtime = false, upPaused = false;
    int upModel = -1;
    pmml::UpscaleOptions upOpts;
    pm::AnimeKeepUp upGuard;
};

double MNowS() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// a plane resized with a separable bicubic (Catmull-Rom), 16-bit samples in and out, clamped to maxV. leftSited: the
// 4:2:0 chroma of MPEG-2 / H.264 / HEVC (horizontally co-sited with the left luma sample, vertically centred), else
// centre-aligned (luma)
void MResize16(const uint16_t* src, ptrdiff_t sstride, int sw, int sh, uint16_t* dst, int dw, int dh, bool leftSited, int maxV)
{
    struct Tap { int i[4]; float w[4]; };
    auto taps = [](int sn, int dn, bool left) {
        std::vector<Tap> t(static_cast<size_t>(dn));
        for (int x = 0; x < dn; ++x) {
            const double f = left ? ((2.0 * x + 0.5) * sn / dn - 0.5) / 2.0 : (x + 0.5) * sn / dn - 0.5;
            const int b = int(std::floor(f));
            const double u = f - b;
            const double w[4] = {((-0.5 * u + 1.0) * u - 0.5) * u, (1.5 * u - 2.5) * u * u + 1.0, ((-1.5 * u + 2.0) * u + 0.5) * u, (0.5 * u - 0.5) * u * u};
            for (int k = 0; k < 4; ++k) { t[size_t(x)].i[k] = std::clamp(b - 1 + k, 0, sn - 1); t[size_t(x)].w[k] = float(w[k]); }
        }
        return t;
    };
    const std::vector<Tap> tx = taps(sw, dw, leftSited), ty = taps(sh, dh, false);
    std::vector<float> rows(size_t(sh) * dw);
    for (int y = 0; y < sh; ++y) {
        const uint16_t* r = src + y * sstride;
        float* o = rows.data() + size_t(y) * dw;
        for (int x = 0; x < dw; ++x) { const Tap& t = tx[size_t(x)]; o[x] = t.w[0] * r[t.i[0]] + t.w[1] * r[t.i[1]] + t.w[2] * r[t.i[2]] + t.w[3] * r[t.i[3]]; }
    }
    for (int y = 0; y < dh; ++y) {
        const Tap& t = ty[size_t(y)];
        const float* r0 = rows.data() + size_t(t.i[0]) * dw; const float* r1 = rows.data() + size_t(t.i[1]) * dw;
        const float* r2 = rows.data() + size_t(t.i[2]) * dw; const float* r3 = rows.data() + size_t(t.i[3]) * dw;
        uint16_t* o = dst + size_t(y) * dw;
        for (int x = 0; x < dw; ++x) {
            const float v = t.w[0] * r0[x] + t.w[1] * r1[x] + t.w[2] * r2[x] + t.w[3] * r3[x];
            o[x] = uint16_t(std::clamp(int(std::lround(v)), 0, maxV));
        }
    }
}

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

// Auto's pick in playback that does not keep up (PushMpcAnimeKeepUp.h; its cost was a prior, card 1.0): a transient trip
// pauses the model (the bicubic resize, then the model again), a real one steps down to a cheaper model of the same
// scale that fits, else the bicubic resize for the clip
void MKeepUp(MInst* d, int64_t s, int srcW, int srcH)
{
    const double fps = d->inVi.fpsNum > 0 && d->inVi.fpsDen > 0 ? double(d->inVi.fpsNum) / double(d->inVi.fpsDen) : 23.976, frameMs = 1000.0 / fps;
    const pm::AnimeKeepUp::Trip trip = d->upGuard.FeedMs(d->upMs, frameMs, double(s) / fps, MNowS());
    if (trip == pm::AnimeKeepUp::Trip::None) return;
    const std::wstring wasW = pm::AnimeSrModel(d->upModel).id;
    const std::string was(wasW.begin(), wasW.end());
    const std::string counts = std::format("{} of the last {} frames ({:.1f} s) over a source frame (median of those {:.1f} ms of {:.1f})", d->upGuard.LastLate(), d->upGuard.LastSeen(),
                                           d->upGuard.LastWindowS(), d->upGuard.LastMedianMs(), frameMs);
    if (trip == pm::AnimeKeepUp::Trip::Transient) {
        d->upPaused = true;
        Log("ml: anime upscaler (Auto) " + was + ": " + counts + std::format(": a bicubic resize for {:.0f} s, then the model again (trip {} of {})", d->upGuard.LastWaitS(),
                                                                         d->upGuard.Trips(), pm::AnimeKeepUp::kMaxTrips));
        return;
    }
    const int next = pm::AnimeSrStepDown(d->upModel, d->upGuard.LastMedianMs(), unsigned(srcW), unsigned(srcH), unsigned(d->upW), unsigned(d->upH), fps);
    if (next >= 0) {
        pmml::UpscaleOptions u = d->upOpts;
        const std::wstring id = pm::AnimeSrModel(next).id;
        u.model.assign(id.begin(), id.end());
        std::string ue;
        auto up = pmml::Upscaler::create(u, &ue);
        if (up && up->outWidth() == d->upW && up->outHeight() == d->upH) {
            d->up = std::move(up); d->upModel = next; d->upOpts = u;
            d->upGuard.NewModel();
            Log("ml: anime upscaler (Auto) " + was + ": " + counts + ": the cheaper " + u.model + " from here");
            return;
        }
        Log("ml: anime upscaler (Auto): the cheaper " + u.model + " did not open (" + (up ? std::string("another output size") : ue) + ")");
    }
    d->up.reset();
    Log("ml: anime upscaler (Auto) " + was + " off for this clip: " + counts + ", and no cheaper model fits: a bicubic resize from now on");
}

// the source frame through the anime upscaler; v then points at the upscaled planes (same bit depth, scale x the size)
bool MUpscale(MInst* d, int64_t s, portable::YuvView& v)
{
    auto it = d->upFrames.find(s);
    if (it == d->upFrames.end()) {
        if (s != d->upLast + 1) {   // a seek: AnimeSR's carry starts again, the keep-up check's warm-up too
            if (d->up) d->up->reset();
            d->upGuard.Restart();
        }
        d->upLast = s;
        pmml::Frame in;
        for (int i = 0; i < 3; ++i) {
            const int w = i ? v.width / 2 : v.width, h = i ? v.height / 2 : v.height;
            if (v.bits > 8) { in.plane[i] = reinterpret_cast<const uint16_t*>(v.data[i]); in.stride[i] = v.stride[i] / 2; continue; }
            d->upIn[i].resize(size_t(w) * h);
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) d->upIn[i][size_t(y) * w + x] = v.data[i][y * v.stride[i] + x];
            in.plane[i] = d->upIn[i].data(); in.stride[i] = w;
        }
        MInst::UpFrame u;
        const int ow = d->upW, oh = d->upH;
        pmml::OutFrame out;
        for (int i = 0; i < 3; ++i) { const int w = i ? ow / 2 : ow, h = i ? oh / 2 : oh; u.p16[i].resize(size_t(w) * h); out.plane[i] = u.p16[i].data(); out.stride[i] = w; }
        std::string err;
        const auto t0 = std::chrono::steady_clock::now();
        bool ran = false;
        if (d->up && d->upPaused && d->upGuard.RetryDue(MNowS())) {
            d->upPaused = false;
            d->up->reset();
            Log("ml: anime upscaler (Auto): tried again after the wait (trip " + std::to_string(d->upGuard.Trips()) + " of " + std::to_string(pm::AnimeKeepUp::kMaxTrips) + " so far)");
        }
        if (d->up && !d->upPaused) {
            ran = d->up->run(in, out, &err);
            if (!ran) { Log("ml: anime upscaler failed (" + err + "): a bicubic resize to " + std::to_string(ow) + "x" + std::to_string(oh) + " from now on"); d->up.reset(); }
        }
        if (!ran)   // a bicubic resize at the same output size (the clip's size is fixed)
            for (int i = 0; i < 3; ++i) {
                const int sw = i ? v.width / 2 : v.width, sh = i ? v.height / 2 : v.height;
                MResize16(in.plane[i], in.stride[i], sw, sh, u.p16[i].data(), i ? ow / 2 : ow, i ? oh / 2 : oh, i > 0, (1 << v.bits) - 1);
            }
        d->upMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (ran && d->upAuto && d->upRealtime) MKeepUp(d, s, v.width, v.height);
        if (v.bits <= 8)
            for (int i = 0; i < 3; ++i) { u.p8[i].resize(u.p16[i].size()); for (size_t k = 0; k < u.p16[i].size(); ++k) u.p8[i][k] = uint8_t(u.p16[i][k]); u.p16[i].clear(); }
        it = d->upFrames.emplace(s, std::move(u)).first;
        while (d->upFrames.size() > 8) d->upFrames.erase(d->upFrames.begin());
    }
    const int ow = d->upW, oh = d->upH;
    for (int i = 0; i < 3; ++i) {
        const int w = i ? ow / 2 : ow;
        if (v.bits > 8) { v.data[i] = reinterpret_cast<const uint8_t*>(it->second.p16[i].data()); v.stride[i] = w * 2; }
        else { v.data[i] = it->second.p8[i].data(); v.stride[i] = w; }
    }
    v.width = ow; v.height = oh;
    return true;
}

void MBind(MInst* d)
{
    d->be.source = [d](int64_t s, portable::YuvView& v) -> bool {
        const VSFrame* f = d->api->getFrameFilter(int(s), d->node, d->fc);
        if (!f) return false;
        d->held.push_back(f);
        d->content.Push(d->api, f, s);
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
        if (d->upW > 0) return MUpscale(d, s, v);   // the model, or after it stopped the plain resize (the output size is fixed)
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
        const auto& rc = d->be.repairCounts;
        Log(std::format("ml: instance closed after {} frames; held-drawing repair: {} frames, {} parts on {} pairs ({} by phase map), {:.1f} ms a frame, spans gated {}, over budget {}",
            d->produced, rc.frames, rc.parts, rc.pairs, rc.mapRuns, rc.frames ? rc.ms / double(rc.frames) : 0.0, rc.gated, rc.overBudget));
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
    const int wantK = displayHz > 0 ? std::clamp(int(std::floor(displayHz / fps + 0.02)), 1, kPortableMaxK) : 2;
    const auto c = pmml::choose(results, fps, wantK);
    return c.ok ? CadenceK(c.multiplier, fps, displayHz) : 0;   // the even cadence at a fixed refresh (VRR is not known here)
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
    // the declared rate is replaced by the one measured on this stream by an earlier instance (vs_content.h)
    VSVideoInfo viM = *a->getVideoInfo(node);
    std::string rateNote; const std::string cdKey = pmvs::ApplyMeasuredRate(viM, &rateNote);
    if (!rateNote.empty()) Log(rateNote);
    const pmvs::StreamMemo cdMemo = pmvs::Recall(cdKey);
    const VSVideoInfo* vi = &viM;
    const std::string rt = MlRuntimeDir(in, a);
    if (!MlRuntimePresent(rt)) return fail("no pmml runtime (graphs/synth-yuv.onnx) in '" + rt + "'");
    const int w = vi->width, h = vi->height, bits = vi->format.bitsPerSample;
    const double srcFps = vi->fpsNum > 0 && vi->fpsDen > 0 ? double(vi->fpsNum) / double(vi->fpsDen) : 23.976;
    int err = 0;
    const double displayHz = a->mapGetFloat(in, "display_fps", 0, &err);
    // playback (mpv's script passes display_fps): Auto's anime pick is checked against real time; an encode never is
    const bool realtime = !err && displayHz > 0;
    const std::string smoother = MArg(in, a, "smoother");
    const int fixedK = smoother.empty() ? 0 : std::atoi(smoother.c_str());
    const int wantK = fixedK >= 2 ? std::min(fixedK, kPortableMaxK) : (!err && displayHz > 0 ? std::clamp(int(std::floor(displayHz / srcFps + 0.02)), 1, kPortableMaxK) : 2);
    // the anime upscaler (upscaler=anime [anime_model=<id>], or upscaler=auto with animation=on): enlarged pictures only
    const std::string upArg = MArg(in, a, "upscaler"), modelArg = MArg(in, a, "anime_model"), animArg = MArg(in, a, "animation");
    const bool animeAsked = upArg == "anime" || upArg == "Anime";
    const bool animeAuto = (upArg.empty() || upArg == "auto" || upArg == "Auto") && (animArg == "on" || animArg == "1" || (animArg.empty() && cdMemo.cls == 1));   // or measured drawn (vs_content.h)
    int dispW = 0, dispH = 0;
    { int e1 = 0, e2 = 0; dispW = int(a->mapGetInt(in, "display_w", 0, &e1)); dispH = int(a->mapGetInt(in, "display_h", 0, &e2)); if (e1 || e2) dispW = dispH = 0; }
    int animeModel = -1;
    std::string animeNote;
    // Auto on HDR (PQ / HLG, the first frame's _Transfer): this plugin feeds the model the decoded planes, i.e. PQ / HLG
    // code values, not the SDR pictures it is trained on: left out (content_detect.h AutoAnimeSrTakesTransfer)
    bool hdrOut = false;
    if (animeAuto && !animeAsked) {
        if (const VSFrame* f0 = a->getFrame(0, node, nullptr, 0)) {
            int e = 0;
            const int64_t t = a->mapGetInt(a->getFramePropertiesRO(f0), "_Transfer", 0, &e);
            hdrOut = !e && !pushmpc::content::AutoAnimeSrTakesTransfer(pushmpc::content::AnimeSrFeed::CodedPlanes, int(t));
            a->freeFrame(f0);
        }
    }
    if (hdrOut) animeNote = "not used: an HDR source (Auto: the model would see PQ / HLG code values)";
    else if (animeAsked || animeAuto) {
        const double scale = dispW > 0 && dispH > 0 ? std::min(double(dispW) / w, double(dispH) / h) : (animeAsked ? 2.0 : 0.0);
        if (scale > 1.05 && h <= 1080) {
            const int forced = modelArg.empty() ? -1 : pm::AnimeSrModelIndex(std::wstring(modelArg.begin(), modelArg.end()));
            const auto pick = pm::PickAnimeSr(unsigned(w), unsigned(h), unsigned(w * scale), unsigned(h * scale), srcFps, false, 1.0, nullptr, forced);
            animeModel = pick.model;
            animeNote = std::string(pick.note.begin(), pick.note.end());   // ASCII labels
        } else animeNote = "not used: the picture is not enlarged (display_w / display_h) or above 1080 lines";
    }
    if (wantK <= 1 && animeModel < 0) { Log("ml: x1: the clip passes through"); a->mapConsumeNode(out, "clip", node, maReplace); return; }

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
        if (c.ok) { o.backend = c.candidate.backend; o.tier = c.candidate.tier; flowOnce = c.candidate.flowOnce; k = fixedK >= 2 ? wantK : CadenceK(c.multiplier, srcFps, err ? 0.0 : displayHz); decision = "Auto (measured): " + c.why + (k < c.multiplier ? std::format("; x{}, the even cadence at {:.2f} Hz (fixed refresh assumed)", k, displayHz) : std::string()); }
    }
    if (decision.empty()) {
        if (!reqBackend.empty() && !pmml::Backend::parse(reqBackend, &o.backend)) return fail("ml_backend '" + reqBackend + "' is not a pmml backend");
        if (reqBackend.empty()) {
            // no measurement: the first GPU EP this runtime offers, in quality order
            std::string log;
            auto devs = pmml::listDevices(rt, &log);
            static const char* order[] = {"trt-rtx", "dml", "amdgpu", "openvino", "migraphx", "coreml", "qnn-gpu", "vulkan", "webgpu", "cpu"};
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
    // line-art protection (anime round 5, pmml synth_line): ml_line_art=1 (off unless asked: the held-share gate of the
    // portable path decides it after the anime-g22 merge)
    if (const std::string v = MArg(in, a, "ml_line_art"); !v.empty()) o.lineArt = ParseBool(v, false);
    std::string e;
    std::unique_ptr<pmml::Upscaler> up;
    pmml::UpscaleOptions upOpts;
    if (animeModel >= 0) {
        pmml::UpscaleOptions u;
        u.runtimeDir = rt; u.graphsDir = rt + "/graphs"; u.cacheDir = rt; u.backend = o.backend;
        const std::wstring id = pm::AnimeSrModel(animeModel).id;
        u.model = std::string(id.begin(), id.end());
        u.io = pmml::UpscaleIo::Yuv420; u.colour = o.colour; u.width = w; u.height = h;
        std::string ue;
        up = pmml::Upscaler::create(u, &ue);
        upOpts = u;
        if (up) { o.width = up->outWidth(); o.height = up->outHeight(); Log("ml: anime upscaler " + animeNote + "; " + up->describe()); }
        else Log("ml: anime upscaler not used (" + ue + ")");
    } else if (!animeNote.empty()) Log("ml: anime upscaler " + animeNote);
    auto interp = pmml::Interpolator::create(o, &e);
    if (!interp) return fail(e);
    Log("ml: " + decision);
    Log("ml: " + interp->describe() + (flowOnce ? "; flow once" : "; network per timestep"));

    auto* d = new MInst();
    d->node = node;
    d->inVi = *vi;
    d->content.Init(cdKey, *vi, cdMemo.cls, cdMemo.cg);
    d->k = k;
    d->vi = *vi;
    if (up) { d->vi.width = d->upW = up->outWidth(); d->vi.height = d->upH = up->outHeight(); d->up = std::move(up); d->upAuto = animeAuto && !animeAsked; d->upRealtime = realtime; d->upModel = animeModel; d->upOpts = upOpts;
              d->upGuard.NewModel(); }
    const Rational outFps = OutputRate(vi->fpsNum, vi->fpsDen, k);
    d->vi.fpsNum = outFps.num; d->vi.fpsDen = outFps.den;
    d->vi.numFrames = int(OutputFrames(vi->numFrames, k));
    d->be.interp = std::move(interp);
    d->be.bits = bits;
    d->be.flowOnce = flowOnce;
    d->pipe.Init(k, vi->numFrames, SrcDur100ns(vi->fpsNum, vi->fpsDen));
    // the held-drawing repair (HeldRepair.h; PUSHMPC_REGION_REPAIR=0 off): outputs 3 source frames late
    { const char* rr = std::getenv("PUSHMPC_REGION_REPAIR"); d->be.regionRepair = k > 1 && !(rr && std::atoi(rr) == 0); }
    d->be.frames = vi->numFrames;
    d->pipe.SetLookahead(d->be.RepairLookahead());
    d->pipe.Reset();
    d->be.grid = d->pipe.GridOf();
    MBind(d);
    Log(std::format("ml: output YUV 4:2:0 {}-bit {}x{} at {}/{} fps (x{}); held-drawing repair {}", bits, d->vi.width, d->vi.height, outFps.num, outFps.den, k, d->be.regionRepair ? "on (lookahead 3)" : "off"));
    VSFilterDependency deps[] = { { node, rpGeneral } };
    a->createVideoFilter(out, "Enhance", &d->vi, MGetFrame, MFree, fmUnordered, deps, 1, d, core);
}

} // namespace pmvs
