// pmeng_get_api: the C table FFmpeg's vf_pushmpc calls (pmeng.h). Picks the backend per instance:
//   D3D11 frames -> pushcore (Windows) unless backend=portable; Vulkan frames -> the portable GPU engine on the decoder's device;
//   CPU frames   -> the portable CPU engine.
// The filter retries a failed hardware open with mem = CPU (its copy fallback), so a reason here is enough.
#include "engine.h"
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <sstream>
#ifdef __APPLE__
#include <dlfcn.h>
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

// software frames: backend=auto tries, in order, what can do best on this machine. macOS: the apple engine (VideoToolbox),
// then the portable GPU engine on its own Vulkan device (MoltenVK), then the CPU engine. Windows on Arm: NVIDIA's engine
// (RTX Spark), the portable GPU engine on its own Vulkan device (Adreno), the CPU engine; D3D11 frames reach this through
// the filter's copy fallback (no pushcore on ARM64). Elsewhere: the CPU engine (the decoder's GPU frames take the
// zero-copy engines above). backend=apple | portable-vulkan | portable-cpu picks one
// (falling back to the CPU engine, with the reason in the description, when it cannot run).
pme::Backend* OpenSoftware(const pmeng_stream& st, const pme::Options& o, pmeng_output& out, std::string& why)
{
    const std::string backend = pme::Opt(o, "backend", "auto");
    std::string tried;
    auto attempt = [&](const char* name, pme::Backend* (*fn)(const pmeng_stream&, const pme::Options&, pmeng_output&, std::string&)) -> pme::Backend* {
        std::string w;
        pme::Backend* b = fn(st, o, out, w);
        if (!b) { tried += std::string(tried.empty() ? "" : "; ") + name + ": " + w; pme::Log(std::string("software frames: ") + name + " not used: " + w); }
        return b;
    };
    pme::Backend* b = nullptr;
#ifdef __APPLE__
    if (backend == "auto" || backend == "apple") b = attempt("apple", OpenApple);
#endif
#ifdef PME_NVL
    // Windows on Arm: NVIDIA's engine first (RTX Spark: VFG + RTX VSR); backend=nvidia takes it for upscaling alone too
    if (backend == "auto" || backend == "nvidia") {
        const bool explicitChoice = backend == "nvidia";
        b = attempt("nvidia", [](const pmeng_stream& s, const pme::Options& op, pmeng_output& ou, std::string& w) { return pme::OpenNvl(s, op, ou, op.count("backend") && op.at("backend") == "nvidia", w); });
        if (!b && explicitChoice) { why = "backend=nvidia: " + tried; return nullptr; }
    }
#endif
#ifdef PME_VULKAN
    bool vk = backend == "portable-vulkan";
#if defined(__APPLE__) || (defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__)))
    // macOS (MoltenVK) and Windows on Arm (Adreno's Vulkan driver; pushcore has no ARM64 build): the portable GPU
    // engine on its own device before the CPU engine
    vk = vk || backend == "auto" || backend == "apple";
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
    return b;
}

void* Open(const pmeng_device* dev, const pmeng_stream* st, const char* options, pmeng_output* out, char* err, uint32_t errSize)
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
#ifdef PME_VULKAN
            if (backend == "portable-cpu") return fail("backend=portable-cpu: the CPU engine (copy fallback)");
            b = pme::OpenVulkan(*dev, *st, o, *out, why);
#else
            why = "this module was built without the Vulkan engine";
#endif
        } else {
#ifndef PME_NVL
            if (backend == "nvidia") return fail("backend=nvidia needs D3D11 frames (hwdec=d3d11va)");
#endif
            b = OpenSoftware(*st, o, *out, why);
        }
        if (!b) return fail(why);
        pme::Log(std::string("open: ") + out->engine + ": " + out->description);
        auto* inst = new Instance();
        inst->b = b;
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
    GUARD(in->b->Submit(*src, pts, flags))
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
    try { pme::Log("close: " + in->b->Stats()); delete in->b; } catch (...) {}
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

const pmeng_api kApi = { sizeof(pmeng_api), PMENG_ABI_VERSION, Version, Open, Submit, Peek, Write, Skip, Pending, Flush, Close, LastError, Stats, Finish };

} // namespace

extern "C" PMENG_EXPORT const pmeng_api* pmeng_get_api(uint32_t abiVersion)
{
    return abiVersion == PMENG_ABI_VERSION ? &kApi : nullptr;
}
