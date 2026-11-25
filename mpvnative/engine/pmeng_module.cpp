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
            if (backend == "nvidia") return fail("backend=nvidia needs D3D11 frames (hwdec=d3d11va)");
            b = pme::OpenCpu(*st, o, *out, why);
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
int Stats(void* h, char* buf, uint32_t size)
{
    auto* in = static_cast<Instance*>(h);
    if (!in || !buf || !size) return PMENG_ERROR;
    try { std::snprintf(buf, size, "%s", in->b->Stats().c_str()); } catch (...) { buf[0] = 0; }
    return PMENG_OK;
}

const pmeng_api kApi = { sizeof(pmeng_api), PMENG_ABI_VERSION, Version, Open, Submit, Peek, Write, Skip, Pending, Flush, Close, LastError, Stats };

} // namespace

extern "C" PMENG_EXPORT const pmeng_api* pmeng_get_api(uint32_t abiVersion)
{
    return abiVersion == PMENG_ABI_VERSION ? &kApi : nullptr;
}
