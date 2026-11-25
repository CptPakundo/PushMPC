// pushmpc_vs.dll: the VapourSynth (API 4) plugin pushmpc.Enhance. PushMPC's engine (pushcore: RTX VSR, NVIDIA VFG and
// the other frame generation methods, the scene-cut guard, the colour chain) for mpv's vf=vapoursynth and every player
// built on it (SMPlayer, MPC-QT with setup). Its own D3D11 device, one warm core per process (corehost.h), the
// scheduling in schedule.h, Auto from the filter's rules (autopick.h).
//
//   core.pushmpc.Enhance(clip clip[, data enable, smoother, sharper, cleanup, sdr_to_hdr, deband, fg_quality, fg_method,
//       vrr, upscaler, target, vsr_style, cleanup_strength, on_battery, chroma, adapter, core_dir, runtime_dir;
//       float display_fps; int display_hdr, matrix, transfer, primaries, range, wait_ms])
//
// The clip must be YUV 4:2:0 integer, 8 or 9..16 bits (pushmpc.vpy converts anything else). The output is RGB24 / RGB30
// (4:4:4) or YUV420P8 / YUV420P10 (Auto at a high readback bandwidth, or chroma="420"), 10-bit whenever the source is
// deeper than 8 bits or the output is HDR, at k x the source rate, with _DurationNum/_DurationDen = source duration / k.
//
// mpv hands out no frame while the script loads, so the core opens with the colour of the previous instance of this
// stream (a seek reload), the arguments, or a guess; the first real frame confirms it or reopens the core (same
// settings, so the declared size and rate stay).
//
// R3: backends (backend.h). backend=auto|nvidia|portable (argument, else the Backend setting, else auto). nvidia is the
// R1 code below, unchanged (Windows only; everything Windows-specific is inside #ifdef _WIN32); portable is
// portable_vs.cpp (every OS). rife is a script preset in pushmpc.vpy; pushmpc.SelectBackend tells the script which
// backend Auto picks, with core.rife detected.
#ifdef _WIN32
#include <windows.h>
#endif
#include <algorithm>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "VapourSynth4.h"
#include "backend.h"
#include "platform.h"
#ifdef _WIN32
#include "autopick.h"
#include "corehost.h"
#endif
#include "schedule.h"
#include "vslog.h"

using namespace pmvs;

namespace pmvs { void PortableCreate(const VSMap* in, VSMap* out, VSCore* core, const VSAPI* a, VSNode* node, const std::string& why); }
#ifdef PMVS_NVIDIA_VFX
namespace pmvs {
void NvLinuxProbe(const VSMap* in, const VSAPI* a, BackendProbe& p);
void NvLinuxCreate(const VSMap* in, VSMap* out, VSCore* core, const VSAPI* a, VSNode* node, const std::string& why);
}
#endif

#ifdef _WIN32
namespace {

// per source frame: durations, SAR and a copy of all its properties (mpv's vf=vapoursynth needs its own _MP_IMAGE
// and the rest back on the output frames)
struct Meta { int64_t durNum = 0, durDen = 0, sarNum = 0, sarDen = 0; std::shared_ptr<VSMap> props; };

struct Enhance {
    VSNode* node = nullptr;
    VSVideoInfo vi{};          // output
    VSVideoInfo inVi{};
    int k = 1;
    uint64_t token = 0;
    OutFmt fmt = OutFmt::Rgb24;
    bool tenBitOut = false;
    int outMatrix = 0, outTransfer = 13, outPrimaries = 1, outRange = 0;   // VapourSynth codes (range 0 = full)
    uint32_t waitMs = 10000;
    Pipeline<void*> pipe;
    std::map<int64_t, Meta> meta;   // per source frame (durations, aspect)
    Meta clipMeta;
    uint64_t produced = 0, lastLogged = 0;
    // what the core was opened for
    pm::Settings settings; pm::Choice choice; pm::Display disp; pm::Source src;
    Colour colour; bool colourChecked = false;
    PushCoreSettings ps{};
    // per getFrame call (under the host lock)
    VSCore* core = nullptr;
    const VSAPI* api = nullptr;
    VSFrameContext* fc = nullptr;
    std::vector<const VSFrame*> held;
};

std::wstring ArgStr(const VSMap* in, const VSAPI* a, const char* key)
{
    int err = 0;
    const char* v = a->mapGetData(in, key, 0, &err);
    return err || !v ? std::wstring() : Widen(v);
}
int64_t ArgInt(const VSMap* in, const VSAPI* a, const char* key, int64_t def)
{
    int err = 0;
    const int64_t v = a->mapGetInt(in, key, 0, &err);
    return err ? def : v;
}
double ArgFloat(const VSMap* in, const VSAPI* a, const char* key, double def)
{
    int err = 0;
    const double v = a->mapGetFloat(in, key, 0, &err);
    return err ? def : v;
}
int64_t PropInt(const VSMap* m, const VSAPI* a, const char* key, int64_t def, bool* present = nullptr)
{
    int err = 0;
    const int64_t v = a->mapGetInt(m, key, 0, &err);
    if (present) *present = !err;
    return err ? def : v;
}
// The range in _ColorRange codes (0 full, 1 limited). VapourSynth API 4.2+ (R7x+) uses _Range with the opposite codes
// (1 full, 0 limited, VSConstants4.h); it wins when present, _ColorRange is the fallback for older sources.
int64_t PropRange(const VSMap* m, const VSAPI* a, bool* present)
{
    bool hasNew = false;
    const int64_t r = PropInt(m, a, "_Range", 0, &hasNew);
    if (hasNew) { if (present) *present = true; return r == 1 ? 0 : 1; }
    return PropInt(m, a, "_ColorRange", 1, present);
}
std::wstring RegFilterStr(const wchar_t* name)
{
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\PushMPC\\Filter", 0, KEY_READ, &k) != ERROR_SUCCESS) return {};
    std::wstring v = pm::RegReadStr(k, name);
    RegCloseKey(k);
    return v;
}
bool DirExists(const std::wstring& d) { const DWORD a = GetFileAttributesW(d.c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
std::wstring Trim(std::wstring s) { while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back(); return s; }

void SetOutProps(Enhance* d, const VSAPI* a, VSFrame* f, int64_t n, int kind)
{
    VSMap* p = a->getFramePropertiesRW(f);
    const int64_t s = n / d->k;
    auto it = d->meta.find(s);
    const Meta m = it != d->meta.end() && it->second.durNum > 0 ? it->second : d->clipMeta;
    if (it != d->meta.end() && it->second.props) a->copyMap(it->second.props.get(), p);   // the source frame's properties first
    const Rational dur = OutputDuration(m.durNum, m.durDen, d->k);
    if (dur.num > 0) { a->mapSetInt(p, "_DurationNum", dur.num, maReplace); a->mapSetInt(p, "_DurationDen", dur.den, maReplace); }
    if (it != d->meta.end() && it->second.sarNum > 0 && it->second.sarDen > 0) { a->mapSetInt(p, "_SARNum", it->second.sarNum, maReplace); a->mapSetInt(p, "_SARDen", it->second.sarDen, maReplace); }
    a->mapSetInt(p, "_Matrix", d->outMatrix, maReplace);
    a->mapSetInt(p, "_Transfer", d->outTransfer, maReplace);
    a->mapSetInt(p, "_Primaries", d->outPrimaries, maReplace);
    a->mapSetInt(p, "_ColorRange", d->outRange, maReplace);          // older VapourSynth (0 full)
    a->mapSetInt(p, "_Range", d->outRange == 0 ? 1 : 0, maReplace);   // API 4.2+ (1 full)
    if (OutIs420(d->fmt)) a->mapSetInt(p, "_ChromaLocation", 1, maReplace);   // the 2x2 average: centre
    else a->mapDeleteKey(p, "_ChromaLocation");
    a->mapSetInt(p, "PushMPCFrameKind", kind, maReplace);                       // 0 real, 1 generated, 2 repeated
    a->mapSetInt(p, "PushMPCSource", s, maReplace);
}

void BindHost(Enhance* d, CoreHost& h)
{
    h.source = [d](int64_t s, PlaneView& v) -> bool {
        const VSFrame* f = d->api->getFrameFilter(int(s), d->node, d->fc);
        if (!f) return false;
        d->held.push_back(f);
        const VSMap* p = d->api->getFramePropertiesRO(f);
        Meta m;
        m.durNum = PropInt(p, d->api, "_DurationNum", 0); m.durDen = PropInt(p, d->api, "_DurationDen", 0);
        m.sarNum = PropInt(p, d->api, "_SARNum", 0); m.sarDen = PropInt(p, d->api, "_SARDen", 0);
        { const VSAPI* api = d->api; VSMap* cp = api->createMap(); api->copyMap(p, cp); m.props = std::shared_ptr<VSMap>(cp, [api](VSMap* x) { api->freeMap(x); }); }
        d->meta[s] = m;
        while (d->meta.size() > 64) d->meta.erase(d->meta.begin());
        const VSVideoFormat* vf = d->api->getVideoFrameFormat(f);
        for (int i = 0; i < 3; ++i) { v.data[i] = d->api->getReadPtr(f, i); v.stride[i] = d->api->getStride(f, i); }
        v.width = uint32_t(d->api->getFrameWidth(f, 0)); v.height = uint32_t(d->api->getFrameHeight(f, 0)); v.bits = vf->bitsPerSample;
        return true;
    };
    h.alloc = [d](uint32_t w, uint32_t hh, OutImage& img) -> bool {
        if (int(w) != d->vi.width || int(hh) != d->vi.height) return false;
        VSFrame* f = d->api->newVideoFrame(&d->vi.format, d->vi.width, d->vi.height, nullptr, d->core);
        if (!f) return false;
        for (int i = 0; i < 3; ++i) { img.data[i] = d->api->getWritePtr(f, i); img.stride[i] = d->api->getStride(f, i); }
        img.handle = f;
        return true;
    };
    h.release = [d](void* f) { d->api->freeFrame(static_cast<const VSFrame*>(f)); };
    h.tag = [d](int64_t pts, bool generated, void* f) { SetOutProps(d, d->api, static_cast<VSFrame*>(f), d->pipe.GridOf().Index(pts), generated ? 1 : 0); };
}

// the output frames' colour from what the core produces
void SetOutColour(Enhance* d, const PushCoreOutputInfo& oi)
{
    const bool hdr = oi.hdrOutput != 0, yuv = OutIs420(d->fmt);
    d->outTransfer = hdr ? 16 : 13; d->outPrimaries = hdr ? 9 : 1;
    d->outMatrix = yuv ? (hdr ? 9 : 1) : 0;
    d->outRange = yuv ? 1 : 0;
}

// open (or reuse) the core for the colour in d->colour; checks that the size and rate stay what was declared
bool OpenFor(Enhance* d, CoreHost& h, bool declared, std::string& err)
{
    const bool srcHdr = IsHdrTransfer(d->colour.transfer);
    d->src.hdr = srcHdr;
    std::wstring hdrNote;
    d->ps = ToCore(d->settings, d->choice, d->disp, d->src.w, d->src.h, srcHdr, d->disp.hdr, &hdrNote);
    if (!hdrNote.empty()) Log(Narrow(hdrNote));
    PushCoreStreamInfo si = {};
    si.structSize = sizeof(si);
    si.width = d->src.w; si.height = d->src.h;
    si.format = d->src.tenBit ? PUSHCORE_INPUT_P010 : PUSHCORE_INPUT_NV12;
    si.fpsNum = int32_t(d->inVi.fpsNum); si.fpsDen = int32_t(d->inVi.fpsDen);
    si.range = d->colour.range; si.matrix = d->colour.matrix; si.primaries = d->colour.primaries; si.transfer = d->colour.transfer;
    si.displayHdr = d->disp.hdr ? 1u : 0u;
    CoreKey key;
    key.width = si.width; key.height = si.height; key.format = si.format; key.fpsNum = si.fpsNum; key.fpsDen = si.fpsDen;
    key.range = si.range; key.matrix = si.matrix; key.primaries = si.primaries; key.transfer = si.transfer; key.displayHdr = si.displayHdr;
    key.adapterLuid = h.DeviceLuid();
    key.settings.assign(reinterpret_cast<const uint8_t*>(&d->ps), reinterpret_cast<const uint8_t*>(&d->ps) + sizeof(d->ps));
    Reuse how = Reuse::Open; double ms = 0;
    d->token = h.Acquire(key, si, d->ps, how, ms, err);
    if (!d->token) return false;
    const PushCoreOutputInfo& oi = h.OutInfo();
    const int k = int(std::clamp<uint32_t>(oi.multiplier ? oi.multiplier : d->ps.multiplier, 1, 8));
    Log(std::format("core for matrix {} transfer {} primaries {} range {}: {} in {:.0f} ms on {}; {}", si.matrix, si.transfer, si.primaries, si.range, ReuseName(how), ms, h.DeviceName(), oi.description));
    if (declared && (k != d->k || int(oi.width) != d->vi.width || int(oi.height) != d->vi.height)) {
        err = std::format("the reopened core gives x{} {}x{}, the clip was declared x{} {}x{}", k, oi.width, oi.height, d->k, d->vi.width, d->vi.height);
        return false;
    }
    d->k = k;
    return true;
}

// the first real frame: confirm the colour the core was opened with, or reopen it
bool CheckColour(Enhance* d, CoreHost& h, int64_t s, std::string& err)
{
    d->colourChecked = true;
    const VSFrame* f = d->api->getFrameFilter(int(s), d->node, d->fc);
    if (!f) return true;
    const VSMap* p = d->api->getFramePropertiesRO(f);
    bool hm, ht, hp, hr;
    const int64_t m = PropInt(p, d->api, "_Matrix", 2, &hm), t = PropInt(p, d->api, "_Transfer", 2, &ht), pr = PropInt(p, d->api, "_Primaries", 2, &hp), r = PropRange(p, d->api, &hr);
    d->api->freeFrame(f);
    const Colour c = ColourFromProps(d->colour, m, hm, t, ht, pr, hp, r, hr);
    h.RememberColour(d->src.w, d->src.h, d->inVi.format.bitsPerSample, d->inVi.fpsNum, d->inVi.fpsDen, c);
    if (c == d->colour) { Log("first frame: the colour matches what the core was opened with"); return true; }
    Log(std::format("first frame: matrix {} transfer {} primaries {} range {} (opened with {} {} {} {}): reopening the core", c.matrix, c.transfer, c.primaries, c.range,
        d->colour.matrix, d->colour.transfer, d->colour.primaries, d->colour.range));
    d->colour = c;
    d->pipe.ClearWith([a = d->api](void* fr) { a->freeFrame(static_cast<const VSFrame*>(fr)); });
    if (!OpenFor(d, h, true, err)) return false;
    d->pipe.Reset();
    SetOutColour(d, h.OutInfo());
    std::string e2;
    if (!h.ConfigureEgress(d->fmt, h.OutInfo().hdrOutput != 0, e2)) { err = "4:2:0 conversion after the reopen: " + e2; return false; }
    return true;
}

const VSFrame* VS_CC EnhanceGetFrame(int n, int reason, void* inst, void** frameData, VSFrameContext* fc, VSCore* core, const VSAPI* a)
{
    auto* d = static_cast<Enhance*>(inst);
    CoreHost& h = CoreHost::Get();
    if (reason == arInitial) {
        int64_t from, to;
        { std::lock_guard<std::mutex> lk(h.Lock()); d->pipe.Requests(n, from, to); }
        frameData[0] = reinterpret_cast<void*>(intptr_t(from));
        for (int64_t i = from; i <= to; ++i) a->requestFrameFilter(int(i), d->node, fc);
        return nullptr;
    }
    if (reason != arAllFramesReady) return nullptr;
    const int64_t requestedFrom = int64_t(reinterpret_cast<intptr_t>(frameData[0]));
    std::lock_guard<std::mutex> lk(h.Lock());
    if (!h.Owns(d->token)) { a->setFilterError("pushmpc.Enhance: a newer script instance took over PushMPC's core (only one Enhance per process)", fc); return nullptr; }
    d->core = core; d->api = a; d->fc = fc;
    BindHost(d, h);
    std::string err;
    if (!d->colourChecked && !CheckColour(d, h, requestedFrom, err)) {
        Log("frame " + std::to_string(n) + ": " + err);
        a->setFilterError(("pushmpc.Enhance: " + err).c_str(), fc);
        d->fc = nullptr;
        return nullptr;
    }
    auto r = d->pipe.Produce(n, requestedFrom, h, d->waitMs);
    for (const VSFrame* f : d->held) a->freeFrame(f);
    d->held.clear();
    d->fc = nullptr;
    if (!r.ok) {
        Log(std::format("frame {}: {}", n, r.err));
        a->setFilterError(("pushmpc.Enhance: " + r.err).c_str(), fc);
        return nullptr;
    }
    const VSFrame* out = nullptr;
    if (!r.repeated) out = a->addFrameRef(static_cast<const VSFrame*>(r.frame));
    else {
        VSFrame* c = a->copyFrame(static_cast<const VSFrame*>(r.frame), core);
        SetOutProps(d, a, c, n, 2);
        out = c;
    }
    if (++d->produced - d->lastLogged >= 2400) {
        d->lastLogged = d->produced;
        const auto& c = d->pipe.Count();
        PushCoreStats st;
        const bool haveStats = h.Stats(st);
        Log(std::format("{} frames out: {} submitted, {} generated, {} repeated (missing generated), {} resets, {} off-grid pts{}", d->produced, c.submitted, c.generated,
            c.repeats, c.discontinuities, c.offGrid, haveStats ? std::format("; pushcore skipped {} generated, {} cuts held", st.skippedGenerated, st.cutGuardCuts) : std::string()));
    }
    return out;
}

void VS_CC EnhanceFree(void* inst, VSCore*, const VSAPI* a)
{
    auto* d = static_cast<Enhance*>(inst);
    CoreHost& h = CoreHost::Get();
    {
        std::lock_guard<std::mutex> lk(h.Lock());
        d->pipe.ClearWith([a](void* f) { a->freeFrame(static_cast<const VSFrame*>(f)); });
        const auto& c = d->pipe.Count();
        Log(std::format("instance closed after {} frames ({} submitted, {} generated, {} repeated, {} resets); the core stays warm", d->produced, c.submitted, c.generated, c.repeats, c.discontinuities));
        h.Release(d->token);
    }
    a->freeNode(d->node);
    delete d;
}

// paths: the plugin arguments, the filter's overrides (HKCU\Software\PushMPC\Filter), the installed layout
HostPaths NvidiaPaths(const VSMap* in, const VSAPI* a)
{
    const std::wstring base = LocalAppDataDir() + L"\\PushMPC";
    HostPaths paths;
    paths.coreDir = Trim(ArgStr(in, a, "core_dir"));
    if (paths.coreDir.empty()) paths.coreDir = Trim(RegFilterStr(L"CoreFolder"));
    if (paths.coreDir.empty()) paths.coreDir = base + L"\\core";
    paths.runtimeDir = Trim(ArgStr(in, a, "runtime_dir"));
    if (paths.runtimeDir.empty()) paths.runtimeDir = Trim(RegFilterStr(L"RuntimeRoot"));
    if (paths.runtimeDir.empty()) paths.runtimeDir = base + L"\\runtime";
    if (!DirExists(paths.runtimeDir)) paths.runtimeDir = paths.coreDir;
    if (DirExists(paths.coreDir + L"\\shaders")) paths.shaderDir = paths.coreDir + L"\\shaders";
    if (const std::wstring logs = LogFolder(); !logs.empty()) paths.logFile = logs + L"\\pushcore-engine-vsplugin.log";
    return paths;
}

// the R1 path; node: the validated clip (owned from here on)
void NvidiaCreate(const VSMap* in, VSMap* out, VSCore* core, const VSAPI* a, VSNode* node)
{
    auto fail = [&](const std::string& m) { Log("create failed: " + m); a->mapSetError(out, ("pushmpc.Enhance: " + m).c_str()); };
    const VSVideoInfo* vi = a->getVideoInfo(node);
    const VSVideoFormat& f = vi->format;

    // settings: HKCU\Software\PushMPC, then the plugin arguments
    pm::Settings s = pm::LoadSettings();
    static const char* keys[] = { "enable", "smoother", "sharper", "cleanup", "sdr_to_hdr", "deband", "fg_quality", "fg_method", "vrr", "upscaler", "target", "vsr_style", "cleanup_strength", "on_battery", "subtitle_hold" };
    std::string overrides;
    for (const char* key : keys) {
        const std::wstring v = ArgStr(in, a, key);
        if (!v.empty() && ApplyOverride(s, Widen(key), v)) overrides += std::format(" {}={}", key, Narrow(v));
    }
    if (!s.enable) {
        Log("PushMPC is off (Enable = 0): the clip passes through");
        a->mapConsumeNode(out, "clip", node, maReplace);
        return;
    }
    const std::wstring chromaArg = ArgStr(in, a, "chroma");
    const Chroma chroma = chromaArg == L"444" ? Chroma::C444 : chromaArg == L"420" ? Chroma::C420 : Chroma::Auto;
    const bool warp = _wcsicmp(ArgStr(in, a, "adapter").c_str(), L"warp") == 0;
    const double displayFps = ArgFloat(in, a, "display_fps", 0);
    const int displayHdrArg = int(ArgInt(in, a, "display_hdr", -1));

    const HostPaths paths = NvidiaPaths(in, a);

    auto* d = new Enhance();
    d->node = node;
    d->inVi = *vi;
    d->settings = s;
    d->waitMs = uint32_t(std::clamp<int64_t>(ArgInt(in, a, "wait_ms", 10000), 100, 120000));
    CoreHost& h = CoreHost::Get();
    std::string err;
    std::lock_guard<std::mutex> lk(h.Lock());
    if (!h.Load(paths, err) || !h.EnsureDevice(0, warp ? AdapterPick::Warp : AdapterPick::Auto, err)) { a->freeNode(node); delete d; return fail(err); }

    // the colour to open with: the arguments, else what the last instance of this stream saw, else a guess
    const uint32_t w = uint32_t(vi->width), hh = uint32_t(vi->height);
    Colour col = GuessColour(w, hh, f.bitsPerSample);
    const bool recalled = h.RecallColour(w, hh, f.bitsPerSample, vi->fpsNum, vi->fpsDen, col);
    const int64_t am = ArgInt(in, a, "matrix", -1), at = ArgInt(in, a, "transfer", -1), ap = ArgInt(in, a, "primaries", -1), ar = ArgInt(in, a, "range", -1);
    col = ColourFromProps(col, am, am >= 0, at, at >= 0, ap, ap >= 0, ar, ar >= 0);
    d->colour = col;
    d->colourChecked = am >= 0 && at >= 0 && ap >= 0;   // all given: the arguments are the truth

    // the GPU as Auto sees it
    pm::Gpu g; g.available = 0; g.hardware = 0; g.family = pm::kFamUnknown;
    PushCoreCaps caps;
    if (h.Caps(caps)) {
        g.family = caps.family; g.available = caps.available; g.hardware = caps.hardware; g.name = Widen(caps.adapter);
        for (uint32_t i = 0; i < pm::kCapCount && i < PUSHCORE_CAP_COUNT; ++i) g.reason[i] = Widen(caps.reason[i]);
    } else for (auto& r : g.reason) r = L"pushcore_capabilities is not available";
    pm::Costs costs;
    costs.card = pm::CardFactor(g.name, g.family);
    LoadLearned(costs, g.name);

    d->src.w = UINT(w); d->src.h = UINT(hh);
    d->src.fps = vi->fpsNum > 0 && vi->fpsDen > 0 ? double(vi->fpsNum) / double(vi->fpsDen) : 23.976;
    d->src.hdr = IsHdrTransfer(col.transfer); d->src.tenBit = f.bitsPerSample > 8; d->src.bppEq = -1;
    const HWND wnd = MpvWindow();
    d->disp = ResolveDisplay(pm::QueryDisplay(wnd, s.vrr, g.nvidia()), wnd != nullptr, s.vrr, displayFps, displayHdrArg);
    d->choice = pm::Decide(s, d->src, d->disp, costs, g);
    const pm::Choice& c = d->choice;

    Log(std::format("script instance: {}x{} {}-bit, {}/{} fps, {} frames; colour {} (matrix {} transfer {} primaries {} range {}); settings from the registry{}",
        w, hh, f.bitsPerSample, vi->fpsNum, vi->fpsDen, vi->numFrames, d->colourChecked ? "from the arguments" : recalled ? "from the last instance of this stream" : "guessed until the first frame",
        col.matrix, col.transfer, col.primaries, col.range, overrides.empty() ? std::string() : ", overridden:" + overrides));
    Log(std::format("display {}: {:.3f} Hz{}, {}x{}, HDR {}; VRR {} ({}); GPU {} ({})", Narrow(d->disp.name), d->disp.refreshHz, displayFps > 1 ? " (mpv's display_fps)" : "", d->disp.width, d->disp.height,
        d->disp.hdr ? "on" : "off", d->disp.vrr ? "on" : "off", Narrow(d->disp.vrrHow), Narrow(g.name), Narrow(pm::FamilyName(g.family))));
    Log(std::format("Auto: x{} {} (quality {}), SR {} level {} -> {}x{}, clean up {}, predicted {:.2f} of {:.2f} ms | {} | {} | {}", c.multiplier, Narrow(pm::FgMethodLabel(c.fgMethod)), c.fgQuality,
        c.sr ? "on" : "off", c.srLevel, c.outW, c.outH, c.nr ? "on" : "off", c.predictedMs, c.budgetMs, Narrow(c.smootherNote), Narrow(c.sharperNote), Narrow(c.stoppedAt)));

    if (!OpenFor(d, h, false, err)) { a->freeNode(node); delete d; return fail("pushcore_open: " + err); }
    const PushCoreOutputInfo& oi = h.OutInfo();

    // the output format: 4:4:4 RGB, or 4:2:0 on the GPU when the readback would be too much
    d->tenBitOut = f.bitsPerSample > 8 || d->ps.videoHdr || oi.hdrOutput;
    const Rational outFps = OutputRate(vi->fpsNum, vi->fpsDen, d->k);
    const double outFpsD = outFps.num > 0 ? double(outFps.num) / double(outFps.den) : d->src.fps * d->k;
    const ChromaDecision cd = DecideChroma(chroma, oi.width, oi.height, d->tenBitOut, outFpsD);
    d->fmt = d->tenBitOut ? (cd.use420 ? OutFmt::Yuv420P10 : OutFmt::Rgb30) : (cd.use420 ? OutFmt::Yuv420P8 : OutFmt::Rgb24);
    if (!h.ConfigureEgress(d->fmt, oi.hdrOutput != 0, err)) {
        Log("4:2:0 conversion unavailable (" + err + "): 4:4:4");
        d->fmt = d->tenBitOut ? OutFmt::Rgb30 : OutFmt::Rgb24;
        h.ConfigureEgress(d->fmt, oi.hdrOutput != 0, err);
    }
    SetOutColour(d, oi);
    Log(std::format("output {} {}x{} at {}/{} fps ({:.3f}, x{}); {}", OutFmtName(d->fmt), oi.width, oi.height, outFps.num, outFps.den, outFpsD, d->k, cd.why));

    d->vi = *vi;
    const bool rgb = !OutIs420(d->fmt);
    if (!a->queryVideoFormat(&d->vi.format, rgb ? cfRGB : cfYUV, stInteger, d->tenBitOut ? 10 : 8, rgb ? 0 : 1, rgb ? 0 : 1, core)) {
        h.Release(d->token); a->freeNode(node); delete d; return fail("queryVideoFormat failed");
    }
    d->vi.width = int(oi.width); d->vi.height = int(oi.height);
    d->vi.fpsNum = outFps.num; d->vi.fpsDen = outFps.den;
    d->vi.numFrames = int(OutputFrames(vi->numFrames, d->k));
    d->clipMeta.durNum = vi->fpsDen; d->clipMeta.durDen = vi->fpsNum;
    d->pipe.Init(d->k, vi->numFrames, SrcDur100ns(vi->fpsNum, vi->fpsDen));
    d->pipe.Reset();

    VSFilterDependency deps[] = { { node, rpGeneral } };
    a->createVideoFilter(out, "Enhance", &d->vi, EnhanceGetFrame, EnhanceFree, fmUnordered, deps, 1, d, core);
}

// R3: what Auto needs to know about pushcore (once per process: mpv reloads the script on every seek)
void ProbeNvidia(const VSMap* in, const VSAPI* a, BackendProbe& p)
{
    static std::mutex m;
    static bool done = false;
    static BackendProbe cached;
    std::lock_guard<std::mutex> lk(m);
    if (!done) {
        done = true;
        cached.nvidiaBuilt = true;
        CoreHost& h = CoreHost::Get();
        std::string err;
        std::lock_guard<std::mutex> hl(h.Lock());
        PushCoreCaps caps;
        if (h.Load(NvidiaPaths(in, a), err) && h.Caps(caps)) { cached.pushcoreLoaded = true; cached.gpuFamily = caps.family; cached.gpuName = caps.adapter; }
        else Log("backend probe: pushcore not usable" + (err.empty() ? std::string(" (pushcore_capabilities missing)") : ": " + err));
    }
    p.nvidiaBuilt = cached.nvidiaBuilt; p.pushcoreLoaded = cached.pushcoreLoaded; p.gpuFamily = cached.gpuFamily; p.gpuName = cached.gpuName;
}

} // namespace
#endif // _WIN32

namespace {

std::string DataArg(const VSMap* in, const VSAPI* a, const char* key)
{
    int err = 0;
    const char* v = a->mapGetData(in, key, 0, &err);
    return err || !v ? std::string() : std::string(v);
}

// backend: the argument, else the Backend setting (registry / settings.ini), else auto
BackendChoice Choose(const VSMap* in, VSCore* core, const VSAPI* a, bool forScript)
{
    std::string req = DataArg(in, a, "backend");
    if (req.empty()) req = ReadSetting("Backend");
    if (req.empty()) req = "auto";
    BackendProbe p;
    const std::string lreq = LowerAscii(req);
#if defined(PMVS_NVIDIA_VFX)   // Linux x86-64, Windows on Arm: CUDA + the Video Effects runtime (engines/nvidia-linux)
    if (lreq == "auto" || lreq == "nvidia") NvLinuxProbe(in, a, p);
#elif defined(_WIN32)           // Windows x64: pushcore
    if (lreq == "auto" || lreq == "nvidia") ProbeNvidia(in, a, p);
#endif
    // Enhance itself cannot run rife (a script preset): only SelectBackend reports it
    if (forScript) {
        int err = 0;
        const int64_t hint = a->mapGetInt(in, "rife", 0, &err);
        p.rifeAvailable = err ? a->getPluginByNamespace("rife", core) != nullptr : hint != 0;
    }
    return SelectBackend(req, p);
}

void VS_CC EnhanceCreate(const VSMap* in, VSMap* out, void*, VSCore* core, const VSAPI* a)
{
    auto fail = [&](const std::string& m) { Log("create failed: " + m); a->mapSetError(out, ("pushmpc.Enhance: " + m).c_str()); };
    VSNode* node = a->mapGetNode(in, "clip", 0, nullptr);
    const VSVideoInfo* vi = a->getVideoInfo(node);
    const VSVideoFormat& f = vi->format;
    if (f.colorFamily != cfYUV || f.sampleType != stInteger || f.subSamplingW != 1 || f.subSamplingH != 1 || f.bitsPerSample < 8 || f.bitsPerSample > 16
        || vi->width <= 0 || vi->height <= 0 || vi->numFrames <= 0) {
        a->freeNode(node);
        return fail("the clip must be YUV 4:2:0, 8 to 16 bits, constant size and format (pushmpc.vpy converts others)");
    }
    const BackendChoice c = Choose(in, core, a, false);
    if (!c.ok) { a->freeNode(node); return fail(c.why); }
    if (c.kind == BackendKind::Portable) return PortableCreate(in, out, core, a, node, c.why);
    if (c.kind == BackendKind::Rife) { a->freeNode(node); return fail("rife is a script preset: use pushmpc.vpy (it chains core.rife)"); }
#if defined(PMVS_NVIDIA_VFX)
    NvLinuxCreate(in, out, core, a, node, c.why);
#elif defined(_WIN32)
    Log("backend: " + c.why);
    NvidiaCreate(in, out, core, a, node);
#else
    a->freeNode(node);
    fail(c.why);
#endif
}

// pushmpc.SelectBackend([backend, rife, core_dir, runtime_dir]) -> {backend: "nvidia"|"portable"|"rife", why}
void VS_CC SelectCreate(const VSMap* in, VSMap* out, void*, VSCore* core, const VSAPI* a)
{
    const BackendChoice c = Choose(in, core, a, true);
    if (!c.ok) { a->mapSetError(out, ("pushmpc.SelectBackend: " + c.why).c_str()); return; }
    Log("SelectBackend: " + c.why);
    a->mapSetData(out, "backend", BackendName(c.kind), -1, dtUtf8, maReplace);
    a->mapSetData(out, "why", c.why.c_str(), -1, dtUtf8, maReplace);
    a->mapSetData(out, "interp", c.interp.c_str(), -1, dtUtf8, maReplace);
}

} // namespace

VS_EXTERNAL_API(void) VapourSynthPluginInit2(VSPlugin* plugin, const VSPLUGINAPI* vspapi)
{
    vspapi->configPlugin("com.pushmpc.enhance", "pushmpc", "PushMPC: frame generation (pushcore on NVIDIA RTX; PushFPS's portable core elsewhere), RTX VSR, scene-cut guard",
                         VS_MAKE_VERSION(1, 1), VAPOURSYNTH_API_VERSION, 0, plugin);
    vspapi->registerFunction("Enhance",
        "clip:vnode;enable:data:opt;smoother:data:opt;sharper:data:opt;cleanup:data:opt;sdr_to_hdr:data:opt;deband:data:opt;fg_quality:data:opt;"
        "fg_method:data:opt;vrr:data:opt;upscaler:data:opt;target:data:opt;vsr_style:data:opt;cleanup_strength:data:opt;on_battery:data:opt;subtitle_hold:data:opt;"
        "chroma:data:opt;adapter:data:opt;core_dir:data:opt;runtime_dir:data:opt;display_fps:float:opt;display_hdr:int:opt;"
        "matrix:int:opt;transfer:int:opt;primaries:int:opt;range:int:opt;wait_ms:int:opt;"
        "backend:data:opt;portable_quality:data:opt;portable_threads:data:opt;portable_device:data:opt;dedup:data:opt;motion:data:opt;sharpness:float:opt;"
        "display_w:int:opt;display_h:int:opt;cuda_device:int:opt;hdr_peak:data:opt;display_peak:float:opt;",
        "clip:vnode;", EnhanceCreate, nullptr, plugin);
    vspapi->registerFunction("SelectBackend", "backend:data:opt;rife:int:opt;core_dir:data:opt;runtime_dir:data:opt;cuda_device:int:opt;", "backend:data;why:data;interp:data;", SelectCreate, nullptr, plugin);
}
