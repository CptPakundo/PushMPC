// The PushMPC engine module (pmeng.h) inside: one Backend per open instance.
//   CpuBackend    PushFPS's portable engine on the CPU (vsplugin/engines/portable), every OS; YUV 4:2:0 8 / 10 bit,
//                 planar or NV12 / P010. Also the engine behind the filter's copy fallback for hardware frames.
//   D3D11Backend  pushcore (Windows): the decoder's D3D11 texture goes straight into pushcore's ingress, the output is
//                 copied (GPU) into the filter's own texture, so pushcore's leases return at once. One warm pushcore per
//                 process (mpv rebuilds its filter graph on every seek).
//   AppleBackend  (macOS, libpushmpc_engine_apple.dylib next to this module) VideoToolbox frame interpolation / frame-rate
//                 conversion on software frames; Auto's first choice there.
//   VulkanHostBackend the portable GPU engine on its own device for software frames (macOS: MoltenVK, bundled).
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
    virtual int Finish() { return PMENG_OK; }   // end of stream (engines that hold outputs back)
    std::string err;
};

// nullptr + why on failure
Backend* OpenCpu(const pmeng_stream& st, const Options& o, pmeng_output& out, std::string& why);
#ifdef PME_VULKAN
// software frames on the engine's own Vulkan device (MoltenVK on macOS): Auto's GPU choice for frames in host memory
Backend* OpenVulkanHost(const pmeng_stream& st, const Options& o, pmeng_output& out, std::string& why);
Backend* OpenVulkan(const pmeng_device& dev, const pmeng_stream& st, const Options& o, pmeng_output& out, std::string& why);
#endif
#ifdef PME_NVL
// Windows on Arm (RTX Spark): the CUDA / Video Effects engine for frames in host memory (nvl_pme_backend.cpp);
// explicitChoice = backend=nvidia (also upscaling alone), else Auto (only when it generates frames)
Backend* OpenNvl(const pmeng_stream& st, const Options& o, pmeng_output& out, bool explicitChoice, std::string& why);
#endif
#ifdef PME_ML
// PushFlow through pmml for frames in host memory (ml_pme_backend.cpp; Windows on Arm: the QNN EP on the Hexagon NPU or the
// Adreno GPU); explicitChoice = backend=ml (runs unmeasured too), else Auto (only with a measurement that fits)
Backend* OpenMl(const pmeng_stream& st, const Options& o, pmeng_output& out, bool explicitChoice, std::string& why);
#endif
#ifdef _WIN32
Backend* OpenD3D11(const pmeng_device& dev, const pmeng_stream& st, const Options& o, pmeng_output& out, std::string& why);
#endif

} // namespace pme
