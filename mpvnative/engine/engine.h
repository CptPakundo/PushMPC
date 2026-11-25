// The PushMPC engine module (pmeng.h) inside: one Backend per open instance.
//   CpuBackend    PushFPS's portable engine on the CPU (vsplugin/engines/portable), every OS; YUV 4:2:0 8 / 10 bit,
//                 planar or NV12 / P010. Also the engine behind the filter's copy fallback for hardware frames.
//   D3D11Backend  pushcore (Windows): the decoder's D3D11 texture goes straight into pushcore's ingress, the output is
//                 copied (GPU) into the filter's own texture, so pushcore's leases return at once. One warm pushcore per
//                 process (mpv rebuilds its filter graph on every seek).
//   VulkanBackend the portable GPU engine (vsplugin/engines/gpu) on the decoder's own Vulkan device: FFmpeg's Vulkan
//                 frames in and out with no copy through host memory (builds without its dependencies: open() fails
//                 with the reason and the filter uses its copy fallback through the CPU engine).
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include "pmeng.h"

namespace pme {

using Options = std::map<std::string, std::string>;   // lower-case keys
Options ParseOptions(const char* text);
std::string Opt(const Options& o, const char* key, const std::string& def = std::string());

void Log(const std::string& line);   // PUSHMPC_ENGINE_LOG (a file) when set, else nothing

class Backend {
public:
    virtual ~Backend() = default;
    virtual int Submit(const pmeng_image& src, int64_t ptsUs, uint32_t flags) = 0;
    virtual int Peek(pmeng_out_info& info, uint32_t timeoutMs) = 0;
    virtual int Write(const pmeng_image& dst) = 0;
    virtual int Skip() = 0;
    virtual uint32_t Pending() = 0;
    virtual int Flush() = 0;
    virtual std::string Stats() = 0;
    std::string err;
};

// nullptr + why on failure
Backend* OpenCpu(const pmeng_stream& st, const Options& o, pmeng_output& out, std::string& why);
#ifdef PME_VULKAN
Backend* OpenVulkan(const pmeng_device& dev, const pmeng_stream& st, const Options& o, pmeng_output& out, std::string& why);
#endif
#ifdef _WIN32
Backend* OpenD3D11(const pmeng_device& dev, const pmeng_stream& st, const Options& o, pmeng_output& out, std::string& why);
#endif

} // namespace pme
