// NVIDIA Video Effects SDK 1.3 (the `nvidia-vfx` wheel's libVideoFX.so / libNVCVImage.so), loaded at run time: only
// the function signatures of the public C API and the selector strings and enum values this backend uses (as PushFPS
// does on Windows: NvCVImage descriptors stay opaque, created by NvCVImage_Create and pointed at our CUDA buffers by
// NvCVImage_Init). The structural CI test (nvl_struct_test.cpp) checks every symbol against the pinned wheel and the
// enum values against what NvCVImage_Create reports.
//
// Runtime layout (as in the wheel, so the libraries' RPATH $ORIGIN/../../nvidia_vfx.libs resolves):
//   <root>/nvvfx/libs/libVideoFX.so, libNVCVImage.so, libnvVFX*.so, libnvidia-ngx-*.so.*, libnpp*.so.12, ...
//   <root>/nvidia_vfx.libs/libcudart-*.so.12.*
#pragma once
#include <string>
#include <vector>
#include "nvl_cuda.h"

namespace pmvs::nvl {

using NvCV_Status = int;
inline constexpr NvCV_Status NVCV_SUCCESS = 0;
// NvCVImage_PixelFormat / ComponentType / layout / memory space (nvCVImage.h; RGB10A2 13 and P32 11 as PushFPS uses)
inline constexpr int NVCV_RGBA = 6, NVCV_RGB10A2 = 13;
inline constexpr int NVCV_U8 = 1, NVCV_P32 = 11;
inline constexpr unsigned NVCV_CHUNKY = 0, NVCV_GPU = 1;
// effect and parameter selectors (strings in the effect libraries; confirmed by the wheel's Python bindings)
namespace sel {
inline constexpr const char* kVsr = "VideoSuperRes";
inline constexpr const char* kVfg = "VideoFrameGeneration";
inline constexpr const char* kTrueHdr = "TrueHDR";
inline constexpr const char* kSrc0 = "SrcImage0";
inline constexpr const char* kSrc1 = "SrcImage1";
inline constexpr const char* kDst0 = "DstImage0";
inline constexpr const char* kStream = "CudaStream";
inline constexpr const char* kQuality = "QualityLevel";
inline constexpr const char* kStrength = "Strength";
inline constexpr const char* kEncoding = "ImageEncodingMode";
inline constexpr const char* kInW = "InputWidth";
inline constexpr const char* kInH = "InputHeight";
inline constexpr const char* kMode = "Mode";
inline constexpr const char* kAutoShot = "AutomaticShotChangeDetectionEnabled";
inline constexpr const char* kShot = "ShotChange";
inline constexpr const char* kMultiplier = "FrameMultiplier";
inline constexpr const char* kIndex = "FrameIndex";
inline constexpr const char* kTimestep = "Timestep";   // F32 in (0, 1): FrameMultiplier 0 (the SDK takes 2..8 in its multiplier mode)
inline constexpr int kIndexedMax = 8;                 // above it the timestep mode (FrameMultiplier 0, Timestep j / k)
inline constexpr int kMaxK = 16;                      // the most PushMPC asks VFG for
inline constexpr const char* kContrast = "Contrast";
inline constexpr const char* kSaturation = "Saturation";
inline constexpr const char* kMiddleGray = "MiddleGray";
inline constexpr const char* kLuminance = "Luminance";
inline constexpr const char* kDebandOff = "DebandingOff";
}

struct VfxApi {
    std::string root;                 // the runtime folder (see above)
    std::vector<void*> libs;
    NvCV_Status (*CreateEffect)(const char*, void**) = nullptr;
    void (*DestroyEffect)(void*) = nullptr;
    NvCV_Status (*SetU32)(void*, const char*, unsigned) = nullptr;
    NvCV_Status (*SetF32)(void*, const char*, float) = nullptr;
    NvCV_Status (*SetImage)(void*, const char*, void*) = nullptr;
    NvCV_Status (*SetCudaStream)(void*, const char*, CUstream) = nullptr;
    NvCV_Status (*LoadEffect)(void*) = nullptr;
    NvCV_Status (*Run)(void*, int) = nullptr;
    NvCV_Status (*GetVersion)(unsigned*) = nullptr;
    NvCV_Status (*ImageCreate)(unsigned, unsigned, int, int, unsigned, unsigned, unsigned, void**) = nullptr;
    void (*ImageDealloc)(void*) = nullptr;
    void (*ImageDestroy)(void*) = nullptr;
    NvCV_Status (*ImageInit)(void*, unsigned, unsigned, int, void*, int, int, unsigned, unsigned) = nullptr;
    const char* (*ErrorString)(NvCV_Status) = nullptr;

#ifdef _WIN32
    static std::string LibDir(const std::string& root) { return root + "\\nvvfx\\libs"; }
#else
    static std::string LibDir(const std::string& root) { return root + "/nvvfx/libs"; }
#endif
    // dlopen the core libraries of `root` (RTLD_NOW | RTLD_GLOBAL, absolute paths) and bind the API
    bool Load(const std::string& root, std::string& why);
    // dlopen one more library of the runtime (an effect's, to have it resident before the effect is created)
    void* Open(const std::string& file, std::string& why);
    std::string Error(NvCV_Status s) const;
    void Unload();
};

// The default runtime folder: $PUSHMPC_NVVFX_DIR, else $XDG_DATA_HOME/pushmpc/nvidia-vfx, else
// ~/.local/share/pushmpc/nvidia-vfx (scripts/nvidia-vfx-fetch.sh installs it there).
std::string DefaultRuntimeDir();

} // namespace pmvs::nvl
