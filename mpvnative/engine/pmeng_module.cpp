// pmeng_get_api: the C table FFmpeg's vf_pushmpc calls (pmeng.h). Picks the backend per instance:
//   D3D11 frames -> pushcore (Windows) unless backend=portable; Vulkan frames -> the portable GPU engine on the decoder's device;
//   CPU frames   -> the portable CPU engine.
//   CUDA frames   -> NVIDIA's engine (libpushmpc_engine_nvl.so) device to device (Linux, hwdec=nvdec).
// The filter retries a failed hardware open with mem = CPU (its copy fallback), so a reason here is enough.
#include "engine.h"
#include "pushmpc/content_detect.h"   // CombMeter: the interlace meter of every route (comb_*, pmeng minor 2), sidecars too
#ifndef PME_SIDECAR
#include "content_tap.h"
#endif
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <sstream>
#ifndef _WIN32
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace pme {

Options ParseOptions(const char* text)
{
    Options o;
    if (!text) return o;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        std::string k = line.substr(0, eq);
        for (auto& c : k) c = char(std::tolower(static_cast<unsigned char>(c)));
        o[k] = line.substr(eq + 1);
    }
    return o;
}
std::string Opt(const Options& o, const char* key, const std::string& def)
{
    auto it = o.find(key);
    return it == o.end() || it->second.empty() ? def : it->second;
}
void Log(const std::string& line)
{
    const char* path = std::getenv("PUSHMPC_ENGINE_LOG");
    if (!path || !*path) return;
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    if (FILE* f = std::fopen(path, "ab")) { std::fprintf(f, "%s\n", line.c_str()); std::fclose(f); }
}

} // namespace pme

namespace {

struct Instance {
    pme::Backend* b = nullptr;
    std::string err;
#include "pushmpc/content_detect.h"   // CombMeter: the interlace meter of every route (comb_*, pmeng minor 2), sidecars too
#ifndef PME_SIDECAR
    // ContentDetect on this stream's frames (a sidecar module's frames are measured by the module that loaded it)
    std::unique_ptr<pme::ContentTap> tap;
    uint32_t tapSeen = 0;
#endif
};

const char* Version() { return "pushmpc-engine 1 (" __DATE__ ")"; }

#ifdef __APPLE__
// libpushmpc_engine_apple.dylib from this module's folder (macOS 13.3+ to load; its self-test decides whether it runs)
void LogC(const char* s) { pme::Log(s); }
pme::Backend* OpenApple(const pmeng_stream& st, const pme::Options& o, pmeng_output& out, std::string& why)
{
    using OpenFn = pme::Backend* (*)(const pmeng_stream*, const pme::Options*, pmeng_output*, std::string*, void (*)(const char*));
    static OpenFn fn = nullptr;
    static std::string loadWhy;
    static std::once_flag once;
    std::call_once(once, [] {
        Dl_info di;
        std::string dir;
        if (dladdr(reinterpret_cast<void*>(&LogC), &di) && di.dli_fname) { dir = di.dli_fname; dir = dir.substr(0, dir.rfind('/') + 1); }
        const std::string path = dir + "libpushmpc_engine_apple.dylib";
        void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h) { loadWhy = std::string("apple engine not loaded (") + dlerror() + ")"; return; }
        fn = reinterpret_cast<OpenFn>(dlsym(h, "pme_apple_open"));
        if (!fn) loadWhy = "apple engine: no pme_apple_open in " + path;
    });
    if (!fn) { why = loadWhy; return nullptr; }
    return fn(&st, &o, &out, &why, LogC);
}
#endif

#if !defined(_WIN32) && !defined(__APPLE__) && !defined(PME_SIDECAR)
#define PME_HAVE_SIDECARS 1
// ---- the sidecar engines (Linux): libpushmpc_engine_nvl.so (NVIDIA's Video Effects through CUDA) and
// libpushmpc_engine_ml.so (PushFlow through pmml), next to this module, each a pmeng module of its own (C ABI only, so
// they can be built elsewhere: the release job builds them with the kernels and pmml). Loaded on first use.
struct Sidecar {
    const pmeng_api* api = nullptr;
    int (*probe)(const char* options, char* why, uint32_t size) = nullptr;
    std::string why;
};
void SelfAnchor() {}
const Sidecar& LoadSidecar(int which)   // 0 nvl, 1 ml
{
    static Sidecar sc[2];
    static std::once_flag once[2];
    std::call_once(once[which], [which] {
        Sidecar& s = sc[which];
        const char* file = which == 0 ? "libpushmpc_engine_nvl.so" : "libpushmpc_engine_ml.so";
        if (const char* off = std::getenv(which == 0 ? "PUSHMPC_NO_NVL" : "PUSHMPC_NO_ML"); off && *off == '1') { s.why = std::string(file) + " switched off (environment)"; return; }
        Dl_info di;
        std::string dir;
        if (dladdr(reinterpret_cast<void*>(&SelfAnchor), &di) && di.dli_fname) { dir = di.dli_fname; dir = dir.substr(0, dir.rfind('/') + 1); }
        const std::string path = dir + file;
        if (access(path.c_str(), R_OK) != 0) { s.why = "not in this download (" + path + ")"; return; }
        void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h) { s.why = std::string("not loaded (") + dlerror() + ")"; return; }
        auto get = reinterpret_cast<pmeng_get_api_fn>(dlsym(h, "pmeng_get_api"));
        s.api = get ? get(PMENG_ABI_VERSION) : nullptr;
        s.probe = reinterpret_cast<int (*)(const char*, char*, uint32_t)>(dlsym(h, "pmeng_sidecar_probe"));
        if (!s.api) s.why = path + ": no pmeng API of this version";
    });
    return sc[which];
}
std::string OptionsText(const pme::Options& o)
{
    std::string t;
    for (const auto& kv : o) t += kv.first + "=" + kv.second + "\n";
    return t;
}
bool ProbeSidecarOf(int which, const pme::Options& o, std::string& why)
{
    const Sidecar& s = LoadSidecar(which);
    if (!s.api) { why = s.why; return false; }
    if (!s.probe) return true;
    char b[512] = {};
    const int r = s.probe(OptionsText(o).c_str(), b, sizeof(b));
    why = b;
    return r == 1;
}
// a sidecar instance behind this module's Backend interface
class ForwardBackend : public pme::Backend {
public:
    ForwardBackend(const pmeng_api* a, void* h) : m_api(a), m_h(h) {}
    ~ForwardBackend() override { if (m_h) m_api->close(m_h); }
    int Submit(const pmeng_image& src, int64_t pts, uint32_t flags) override { return Check(m_api->submit(m_h, &src, pts, flags)); }
    int Peek(pmeng_out_info& info, uint32_t timeoutMs) override { return Check(m_api->peek(m_h, &info, timeoutMs)); }
    int Write(const pmeng_image& dst) override { return Check(m_api->write(m_h, &dst)); }
    int Skip() override { return Check(m_api->skip(m_h)); }
    uint32_t Pending() override { return m_api->pending(m_h); }
    int Flush() override { return Check(m_api->flush(m_h)); }
    int Finish() override { return m_api->struct_size >= offsetof(pmeng_api, finish) + sizeof(m_api->finish) && m_api->finish ? Check(m_api->finish(m_h)) : PMENG_OK; }
    std::string Stats() override { char b[512] = {}; m_api->stats(m_h, b, sizeof(b)); return b; }
private:
    int Check(int r) { if (r < 0) err = m_api->last_error(m_h); return r; }
    const pmeng_api* m_api;
    void* m_h;
};
pme::Backend* OpenSidecarOf(int which, const pmeng_stream& st, const pme::Options& o, pmeng_output& out, std::string& why,
                            const pmeng_device* hw = nullptr)
{
    const Sidecar& s = LoadSidecar(which);
    if (!s.api) { why = s.why; return nullptr; }
    pmeng_device dev{};
    dev.struct_size = sizeof(dev);
    dev.mem = PMENG_MEM_CPU;
    if (hw) std::memcpy(&dev, hw, std::min<size_t>(sizeof(dev), hw->struct_size ? hw->struct_size : sizeof(dev)));   // the filter's (maybe shorter) struct
    if (hw) dev.struct_size = std::min<uint32_t>(uint32_t(sizeof(dev)), hw->struct_size ? hw->struct_size : uint32_t(sizeof(dev)));
    char err[512] = {};
    std::memset(&out, 0, sizeof(out));
    out.struct_size = sizeof(out);
    void* h = s.api->open(&dev, &st, OptionsText(o).c_str(), &out, err, sizeof(err));
    if (!h) { why = err; return nullptr; }
    return new ForwardBackend(s.api, h);
}
pme::Backend* OpenNvl(const pmeng_stream& st, const pme::Options& o, pmeng_output& out, std::string& why) { return OpenSidecarOf(0, st, o, out, why); }
pme::Backend* OpenMl(const pmeng_stream& st, const pme::Options& o, pmeng_output& out, std::string& why) { return OpenSidecarOf(1, st, o, out, why); }
pme::Backend* OpenNvlCuda(const pmeng_device& dev, const pmeng_stream& st, const pme::Options& o, pmeng_output& out, std::string& why) { return OpenSidecarOf(0, st, o, out, why, &dev); }
#endif

#include "pushmpc/content_detect.h"   // CombMeter: the interlace meter of every route (comb_*, pmeng minor 2), sidecars too
#ifndef PME_SIDECAR
// software frames: backend=auto tries, in order, what can do best on this machine.
//   macOS: the apple engine (VideoToolbox), then the portable GPU engine on its own Vulkan device (MoltenVK), then the CPU.
//   Linux: NVIDIA's engine (RTX VSR, RTX Video HDR, VFG; libpushmpc_engine_nvl.so) when the card, the driver and NVIDIA's
//          files are there; else PushFlow through pmml (libpushmpc_engine_ml.so, when its runtime is installed) and the
//          portable GPU engine on its own Vulkan device, both opened and measured: the higher multiplier wins (RIFE on a
//          tie: the better picture); else the CPU engine.
//   Windows: the portable GPU engine on its own Vulkan device, else the CPU (pushcore takes the decoder's D3D11 frames).
//   Windows on Arm: NVIDIA's engine (RTX Spark), PushFlow through pmml (QNN EP), the portable GPU engine on its own Vulkan
//          device (Adreno), the CPU engine; D3D11 frames reach this through the filter's copy fallback (no pushcore on ARM64).
// A Vulkan device that is a CPU (lavapipe, SwiftShader) is not used unless vulkan_allow_cpu=1.
// backend=nvidia | ml | apple | portable-vulkan | portable-cpu picks one (falling back to the CPU engine, with the
// reason in the description, when it cannot run).
pme::Backend* OpenSoftware(const pmeng_stream& st, const pme::Options& o, pmeng_output& out, std::string& why)
{
    const std::string backend = pme::Opt(o, "backend", "auto");
    const bool autoPick = backend == "auto";
    std::string tried;
    auto note = [&](const char* name, const std::string& w) { tried += std::string(tried.empty() ? "" : "; ") + name + ": " + w; pme::Log(std::string("software frames: ") + name + " not used: " + w); };
    auto attempt = [&](const char* name, pme::Backend* (*fn)(const pmeng_stream&, const pme::Options&, pmeng_output&, std::string&)) -> pme::Backend* {
        std::string w;
        pme::Backend* b = fn(st, o, out, w);
        if (!b) note(name, w);
        return b;
    };
    pme::Backend* b = nullptr;
#ifdef __APPLE__
    if (autoPick || backend == "apple") b = attempt("apple", OpenApple);
#endif
#ifdef PME_HAVE_SIDECARS
    if (!b && (autoPick || backend == "nvidia")) b = attempt("nvidia", OpenNvl);
    if (!b && (autoPick || backend == "ml")) {
        pmeng_output mlOut{};
        std::string w;
        pme::Backend* ml = OpenMl(st, o, mlOut, w);
        if (!ml) note("ml", w);
#ifdef PME_VULKAN
        if (ml && autoPick) {
            // measured against the Vulkan engine: the one that reaches more frames per source frame (RIFE on a tie)
            pmeng_output vkOut{};
            vkOut.struct_size = sizeof(vkOut);
            std::string vw;
            pme::Backend* vk = pme::OpenVulkanHost(st, o, vkOut, vw);
            if (vk && vkOut.multiplier > mlOut.multiplier) {
                pme::Log("software frames: the Vulkan engine reaches x" + std::to_string(vkOut.multiplier) + ", PushFlow x" + std::to_string(mlOut.multiplier) + ": the Vulkan engine");
                note("ml", "PushFlow reaches x" + std::to_string(mlOut.multiplier) + ", the Vulkan engine x" + std::to_string(vkOut.multiplier));
                delete ml; ml = nullptr;
                out = vkOut; b = vk;
            } else {
                pme::Log("software frames: PushFlow reaches x" + std::to_string(mlOut.multiplier) + (vk ? ", the Vulkan engine x" + std::to_string(vkOut.multiplier) : ", the Vulkan engine not available (" + vw + ")") + ": PushFlow");
                delete vk;
            }
        }
#endif
        if (ml) { out = mlOut; b = ml; }
    }
#endif
#ifdef PME_NVL
    // Windows on Arm: NVIDIA's engine first (RTX Spark: VFG + RTX VSR); backend=nvidia takes it for upscaling alone too
    if (backend == "auto" || backend == "nvidia") {
        const bool explicitChoice = backend == "nvidia";
        b = attempt("nvidia", [](const pmeng_stream& s, const pme::Options& op, pmeng_output& ou, std::string& w) { return pme::OpenNvl(s, op, ou, op.count("backend") && op.at("backend") == "nvidia", w); });
        if (!b && explicitChoice) { why = "backend=nvidia: " + tried; return nullptr; }
    }
#endif
#ifdef PME_ML
    // PushFlow through pmml (Windows on Arm: the QNN EP on the Hexagon NPU or the Adreno GPU), when Setup's measurement
    // says it fits this clip's frame budget; backend=ml takes it unmeasured too
    if (!b && (backend == "auto" || backend == "ml")) {
        b = attempt("ml", [](const pmeng_stream& s, const pme::Options& op, pmeng_output& ou, std::string& w) { return pme::OpenMl(s, op, ou, op.count("backend") && op.at("backend") == "ml", w); });
        if (!b && backend == "ml") { why = "backend=ml: " + tried; return nullptr; }
    }
#endif
#ifdef PME_VULKAN
    bool vk = backend == "portable-vulkan";
#if defined(__APPLE__) || (defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__)))
    // macOS (MoltenVK) and Windows on Arm (Adreno's Vulkan driver; pushcore has no ARM64 build): the portable GPU
    // engine on its own device before the CPU engine
    vk = vk || autoPick || backend == "apple";
#else
    vk = vk || autoPick || backend == "nvidia" || backend == "ml";
#endif
    if (!b && vk) b = attempt("portable-vulkan", pme::OpenVulkanHost);
#endif
    if (!b) {
        b = pme::OpenCpu(st, o, out, why);
        if (b && !tried.empty()) {
            const size_t n = std::strlen(out.description);
            std::snprintf(out.description + n, sizeof(out.description) - n, " (GPU engines not used: %s)", tried.c_str());
        }
    }
    // tests only (mpvnative/hook/test-hook-engines.sh): the request NVIDIA's engine makes for RTX Video HDR on 8-bit
    // frames, so the hook's 10-bit round trip runs on machines without that engine
    if (b && pme::Opt(o, "test_want_out_depth") == "10" && pme::Opt(o, "source_bits").empty() && !(st.fmt == PMENG_FMT_YUV420P10 || st.fmt == PMENG_FMT_P010))
        out.want_out_depth = 10;
    (void)autoPick; (void)attempt;   // unused in builds without the GPU engines
    return b;
}
#endif

void* OpenFull(const pmeng_device* dev, const pmeng_stream* st, const char* options, pmeng_output* out, char* err, uint32_t errSize);
// an older filter's pmeng_output is shorter (no want_out_depth): never write past its struct_size
void* Open(const pmeng_device* dev, const pmeng_stream* st, const char* options, pmeng_output* out, char* err, uint32_t errSize)
{
    if (!out) return OpenFull(dev, st, options, out, err, errSize);
    const size_t minimal = offsetof(pmeng_output, want_out_depth);   // minor 0's whole struct
    const size_t cap = out->struct_size < minimal ? minimal : std::min<size_t>(out->struct_size, sizeof(pmeng_output));
    pmeng_output full{};
    void* h = OpenFull(dev, st, options, &full, err, errSize);
    full.struct_size = uint32_t(cap);
    std::memcpy(out, &full, cap);
    return h;
}
void* OpenFull(const pmeng_device* dev, const pmeng_stream* st, const char* options, pmeng_output* out, char* err, uint32_t errSize)
{
    auto fail = [&](const std::string& why) -> void* {
        if (err && errSize) { std::snprintf(err, errSize, "%s", why.c_str()); }
        pme::Log("open failed: " + why);
        return nullptr;
    };
    if (!dev || !st || !out || st->width == 0 || st->height == 0) return fail("bad arguments");
    try {
        const pme::Options o = pme::ParseOptions(options);
        const std::string backend = pme::Opt(o, "backend", "auto");
        std::memset(out, 0, sizeof(*out));
        out->struct_size = sizeof(*out);
        std::string why;
        pme::Backend* b = nullptr;
        if (dev->mem == PMENG_MEM_D3D11) {
#ifdef _WIN32
            if (backend == "portable") return fail("backend=portable: the CPU engine (copy fallback)");
            b = pme::OpenD3D11(*dev, *st, o, *out, why);
#else
            why = "D3D11 frames on a non-Windows build";
#endif
        } else if (dev->mem == PMENG_MEM_VULKAN) {
#ifdef PME_SIDECAR
            return fail("this engine takes frames in host memory (copy fallback)");
#endif
#ifdef PME_HAVE_SIDECARS
            // NVIDIA's engine does more on an RTX card than the zero-copy engine: it takes the filter's copy fallback
            std::string nw;
            if ((backend == "auto" || backend == "nvidia") && ProbeSidecarOf(0, o, nw)) return fail("NVIDIA's engine runs here (" + nw + "): the copy fallback");
            if (backend == "nvidia" || backend == "ml") return fail("backend=" + backend + " takes frames in host memory (copy fallback)");
#endif
#ifdef PME_VULKAN
            if (backend == "portable-cpu") return fail("backend=portable-cpu: the CPU engine (copy fallback)");
            b = pme::OpenVulkan(*dev, *st, o, *out, why);
#else
            why = "this module was built without the Vulkan engine";
#endif
        } else if (dev->mem == PMENG_MEM_CUDA) {
            // the decoder's CUDA frames: NVIDIA's engine device to device; anything else takes the copy fallback
#if defined(PME_SIDECAR)
            b = pme::OpenSidecarDevice(*dev, *st, o, *out, why);
#elif defined(PME_HAVE_SIDECARS)
            if (backend != "auto" && backend != "nvidia") return fail("backend=" + backend + " takes frames in host memory (copy fallback)");
            b = OpenNvlCuda(*dev, *st, o, *out, why);
            if (!b) why = "NVIDIA's engine on CUDA frames: " + why + " (copy fallback)";
#else
            return fail("CUDA frames: no engine for them in this build (copy fallback)");
#endif
        } else {
#ifdef PME_SIDECAR
            b = pme::OpenSidecar(*st, o, *out, why);
#else
#if defined(_WIN32) && !defined(PME_NVL)
            if (backend == "nvidia") return fail("backend=nvidia needs D3D11 frames (hwdec=d3d11va)");
#endif
            b = OpenSoftware(*st, o, *out, why);
#endif
        }
        if (!b) return fail(why);
        pme::Log(std::string("open: ") + out->engine + ": " + out->description);
        auto* inst = new Instance();
        inst->b = b;
#include "pushmpc/content_detect.h"   // CombMeter: the interlace meter of every route (comb_*, pmeng minor 2), sidecars too
#ifndef PME_SIDECAR
        // ContentDetect: what the picture is, measured on the frames (option content=off: not measured)
        if (pme::Opt(o, "content", "auto") != "off") { inst->tap = std::make_unique<pme::ContentTap>(); b->content = inst->tap.get(); }
#endif
        return inst;
    } catch (const std::exception& e) {
        return fail(std::string("exception: ") + e.what());
    } catch (...) {
        return fail("unknown exception");
    }
}

#define GUARD(expr) try { return (expr); } catch (const std::exception& e) { in->err = e.what(); return PMENG_ERROR; } catch (...) { in->err = "unknown exception"; return PMENG_ERROR; }

int Submit(void* h, const pmeng_image* src, int64_t pts, uint32_t flags)
{
    auto* in = static_cast<Instance*>(h);
    if (!in || !src) return PMENG_ERROR;
    try {
        const int r = in->b->Submit(*src, pts, flags);
#include "pushmpc/content_detect.h"   // CombMeter: the interlace meter of every route (comb_*, pmeng minor 2), sidecars too
#ifndef PME_SIDECAR
        if (in->tap) {
            // frames in host memory: measured here, after the engine took the frame (a BUSY frame comes again)
            if (r == PMENG_OK && src->mem == PMENG_MEM_CPU) pme::TapCpuFrame(*in->tap, *src, double(pts) / 1e6, (flags & PMENG_SUBMIT_DISCONTINUITY) != 0);
            const pme::ContentTap::Snapshot s = in->tap->Poll();
            if (s.serial != in->tapSeen) { in->tapSeen = s.serial; in->b->OnContent(s.cls, s.cg, s.conf); }
        }
#endif
        return r;
    } catch (const std::exception& e) { in->err = e.what(); return PMENG_ERROR; } catch (...) { in->err = "unknown exception"; return PMENG_ERROR; }
}
int Peek(void* h, pmeng_out_info* info, uint32_t timeoutMs)
{
    auto* in = static_cast<Instance*>(h);
    if (!in || !info) return PMENG_ERROR;
    GUARD(in->b->Peek(*info, timeoutMs))
}
int Write(void* h, const pmeng_image* dst)
{
    auto* in = static_cast<Instance*>(h);
    if (!in || !dst) return PMENG_ERROR;
    GUARD(in->b->Write(*dst))
}
int Skip(void* h)
{
    auto* in = static_cast<Instance*>(h);
    if (!in) return PMENG_ERROR;
    GUARD(in->b->Skip())
}
uint32_t Pending(void* h)
{
    auto* in = static_cast<Instance*>(h);
    try { return in ? in->b->Pending() : 0; } catch (...) { return 0; }
}
int Flush(void* h)
{
    auto* in = static_cast<Instance*>(h);
    if (!in) return PMENG_ERROR;
    GUARD(in->b->Flush())
}
void Close(void* h)
{
    auto* in = static_cast<Instance*>(h);
    if (!in) return;
    try {
        pme::Log("close: " + in->b->Stats());
#include "pushmpc/content_detect.h"   // CombMeter: the interlace meter of every route (comb_*, pmeng minor 2), sidecars too
#ifndef PME_SIDECAR
        if (in->tap) pme::Log("close: " + in->tap->Describe());
#endif
        delete in->b;
    } catch (...) {}
    delete in;
}
const char* LastError(void* h)
{
    auto* in = static_cast<Instance*>(h);
    if (!in) return "no instance";
    if (!in->b->err.empty()) in->err = in->b->err;
    return in->err.c_str();
}
int Finish(void* h)
{
    auto* in = static_cast<Instance*>(h);
    if (!in) return PMENG_ERROR;
    GUARD(in->b->Finish())
}
int Stats(void* h, char* buf, uint32_t size)
{
    auto* in = static_cast<Instance*>(h);
    if (!in || !buf || !size) return PMENG_ERROR;
    try { std::snprintf(buf, size, "%s", in->b->Stats().c_str()); } catch (...) { buf[0] = 0; }
    return PMENG_OK;
}

// interlaced input (minor 2): the meter of content_detect.h, one per filter instance, no engine needed
void* CombNew() { try { return new pushmpc::content::CombMeter(); } catch (...) { return nullptr; } }
int CombPush(void* c, const uint8_t* luma, ptrdiff_t stride, uint32_t w, uint32_t h, uint32_t bps, uint32_t shift)
{
    auto* m = static_cast<pushmpc::content::CombMeter*>(c);
    if (!m || !luma) return 0;
    try {
        return bps > 1 ? int(m->Push(reinterpret_cast<const uint16_t*>(luma), stride / 2, int(w), int(h), int(shift)))
                       : int(m->Push(luma, stride, int(w), int(h), 0));
    } catch (...) { return int(m->verdict()); }
}
void CombCounts(void* c, int* measured, int* combed)
{
    auto* m = static_cast<pushmpc::content::CombMeter*>(c);
    if (measured) *measured = m ? m->measured() : 0;
    if (combed) *combed = m ? m->combed() : 0;
}
void CombFree(void* c) { delete static_cast<pushmpc::content::CombMeter*>(c); }

const pmeng_api kApi = { sizeof(pmeng_api), PMENG_ABI_VERSION, Version, Open, Submit, Peek, Write, Skip, Pending, Flush, Close, LastError, Stats, Finish,
                         CombNew, CombPush, CombCounts, CombFree };

} // namespace

#ifdef PME_SIDECAR
extern "C" PMENG_EXPORT int pmeng_sidecar_probe(const char* options, char* why, uint32_t size)
{
    try {
        std::string w;
        const bool ok = pme::ProbeSidecar(pme::ParseOptions(options), w);
        if (why && size) std::snprintf(why, size, "%s", w.c_str());
        return ok ? 1 : 0;
    } catch (...) { return 0; }
}
#endif

extern "C" PMENG_EXPORT const pmeng_api* pmeng_get_api(uint32_t abiVersion)
{
    return abiVersion == PMENG_ABI_VERSION ? &kApi : nullptr;
}
