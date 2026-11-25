// Run-time loading of the CUDA driver and the Video Effects runtime (nvl_cuda.h, nvl_vfx.h). Linux only.
#include <dirent.h>
#include <dlfcn.h>
#include <cstring>
#include <cstdlib>
#include <string>
#include "nvl_cuda.h"
#include "nvl_vfx.h"

namespace pmvs::nvl {

namespace {
template <class T> bool Bind(void* lib, const char* name, T& fn, std::string& why)
{
    fn = reinterpret_cast<T>(dlsym(lib, name));
    if (!fn) { why = std::string("missing ") + name; return false; }
    return true;
}
}

bool CudaApi::Load(std::string& why, const char* path)
{
    if (lib) return true;
    lib = dlopen(path ? path : "libcuda.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) { const char* e = dlerror(); why = std::string("the NVIDIA driver's libcuda.so.1 is not loadable") + (e ? std::string(" (") + e + ")" : std::string()); return false; }
    bool ok = Bind(lib, "cuInit", cuInit, why) && Bind(lib, "cuDriverGetVersion", cuDriverGetVersion, why) && Bind(lib, "cuDeviceGetCount", cuDeviceGetCount, why)
        && Bind(lib, "cuDeviceGet", cuDeviceGet, why) && Bind(lib, "cuDeviceGetName", cuDeviceGetName, why) && Bind(lib, "cuDeviceGetAttribute", cuDeviceGetAttribute, why)
        && Bind(lib, "cuDeviceTotalMem_v2", cuDeviceTotalMem, why) && Bind(lib, "cuDevicePrimaryCtxRetain", cuDevicePrimaryCtxRetain, why)
        && Bind(lib, "cuDevicePrimaryCtxRelease_v2", cuDevicePrimaryCtxRelease, why) && Bind(lib, "cuCtxPushCurrent_v2", cuCtxPushCurrent, why)
        && Bind(lib, "cuCtxPopCurrent_v2", cuCtxPopCurrent, why) && Bind(lib, "cuCtxGetDevice", cuCtxGetDevice, why)
        && Bind(lib, "cuStreamCreate", cuStreamCreate, why) && Bind(lib, "cuStreamDestroy_v2", cuStreamDestroy, why) && Bind(lib, "cuStreamSynchronize", cuStreamSynchronize, why)
        && Bind(lib, "cuEventCreate", cuEventCreate, why) && Bind(lib, "cuEventRecord", cuEventRecord, why) && Bind(lib, "cuEventQuery", cuEventQuery, why)
        && Bind(lib, "cuEventSynchronize", cuEventSynchronize, why) && Bind(lib, "cuEventElapsedTime", cuEventElapsedTime, why) && Bind(lib, "cuEventDestroy_v2", cuEventDestroy, why)
        && Bind(lib, "cuMemAlloc_v2", cuMemAlloc, why) && Bind(lib, "cuMemAllocPitch_v2", cuMemAllocPitch, why) && Bind(lib, "cuMemFree_v2", cuMemFree, why)
        && Bind(lib, "cuMemHostAlloc", cuMemHostAlloc, why) && Bind(lib, "cuMemFreeHost", cuMemFreeHost, why) && Bind(lib, "cuMemcpy2DAsync_v2", cuMemcpy2DAsync, why)
        && Bind(lib, "cuMemcpyHtoDAsync_v2", cuMemcpyHtoDAsync, why) && Bind(lib, "cuMemcpyDtoHAsync_v2", cuMemcpyDtoHAsync, why) && Bind(lib, "cuMemcpyDtoH_v2", cuMemcpyDtoH, why)
        && Bind(lib, "cuModuleLoadData", cuModuleLoadData, why) && Bind(lib, "cuModuleGetFunction", cuModuleGetFunction, why) && Bind(lib, "cuModuleUnload", cuModuleUnload, why)
        && Bind(lib, "cuLaunchKernel", cuLaunchKernel, why) && Bind(lib, "cuGetErrorName", cuGetErrorName, why) && Bind(lib, "cuGetErrorString", cuGetErrorString, why);
    if (!ok) { why = "libcuda.so.1: " + why; dlclose(lib); lib = nullptr; }
    return ok;
}

std::string CudaApi::Error(CUresult r) const
{
    const char* n = nullptr; const char* s = nullptr;
    if (cuGetErrorName) cuGetErrorName(r, &n);
    if (cuGetErrorString) cuGetErrorString(r, &s);
    return std::to_string(r) + " " + (n ? n : "?") + (s ? std::string(" (") + s + ")" : std::string());
}

std::string DefaultRuntimeDir()
{
    if (const char* d = std::getenv("PUSHMPC_NVVFX_DIR"); d && *d) return d;
    const char* x = std::getenv("XDG_DATA_HOME");
    if (x && *x == '/') return std::string(x) + "/pushmpc/nvidia-vfx";
    const char* h = std::getenv("HOME");
    return h ? std::string(h) + "/.local/share/pushmpc/nvidia-vfx" : std::string();
}

void* VfxApi::Open(const std::string& file, std::string& why)
{
    const std::string p = LibDir(root) + "/" + file;
    void* h = dlopen(p.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!h) { const char* e = dlerror(); why = p + ": " + (e ? e : "dlopen failed"); return nullptr; }
    libs.push_back(h);
    return h;
}

bool VfxApi::Load(const std::string& r, std::string& why)
{
    if (!libs.empty()) return true;
    root = r;
    // The wheel's libraries find each other only when already loaded: libNVCVImage.so carries a build-machine RPATH
    // (/usr/local/cuda-12.8/...) and the others only $ORIGIN/../../nvidia_vfx.libs (cudart). As NVIDIA's own Python
    // loader does, they are loaded here by absolute path in dependency order (RTLD_GLOBAL), so every later NEEDED entry
    // (and the effect libraries libVideoFX opens) resolves to these copies by SONAME.
    {
        DIR* d = opendir((root + "/nvidia_vfx.libs").c_str());
        if (d) {
            while (dirent* e = readdir(d))
                if (!std::strncmp(e->d_name, "libcudart", 9)) {
                    void* h = dlopen((root + "/nvidia_vfx.libs/" + e->d_name).c_str(), RTLD_NOW | RTLD_GLOBAL);
                    if (h) libs.push_back(h);
                }
            closedir(d);
        }
    }
    for (const char* dep : { "libnppc.so.12", "libnppial.so.12", "libnppicc.so.12", "libnppidei.so.12", "libnppig.so.12", "libnppif.so.12", "libnppim.so.12",
                             "libnppist.so.12", "libnppitc.so.12", "libcudnn.so.9" }) {
        std::string w;
        if (!Open(dep, w) && std::strstr(dep, "libnpp") && std::strcmp(dep, "libnppif.so.12") && std::strcmp(dep, "libnppim.so.12") && std::strcmp(dep, "libnppist.so.12") && std::strcmp(dep, "libnppitc.so.12")) {
            why = w; Unload(); return false;   // the NPP modules libNVCVImage needs (NEEDED); the other four are optional
        }
    }
    void* image = Open("libNVCVImage.so", why);
    if (!image) { Unload(); return false; }
    void* core = Open("libVideoFX.so", why);
    if (!core) { Unload(); return false; }
    std::string w;
    Open("libnvngxruntime.so", w);
    Open("libVideoFXLocal.so", w);
    bool ok = Bind(core, "NvVFX_CreateEffect", CreateEffect, why) && Bind(core, "NvVFX_DestroyEffect", DestroyEffect, why) && Bind(core, "NvVFX_SetU32", SetU32, why)
        && Bind(core, "NvVFX_SetF32", SetF32, why) && Bind(core, "NvVFX_SetImage", SetImage, why) && Bind(core, "NvVFX_SetCudaStream", SetCudaStream, why)
        && Bind(core, "NvVFX_Load", LoadEffect, why) && Bind(core, "NvVFX_Run", Run, why) && Bind(core, "NvVFX_GetVersion", GetVersion, why)
        && Bind(image, "NvCVImage_Create", ImageCreate, why) && Bind(image, "NvCVImage_Dealloc", ImageDealloc, why) && Bind(image, "NvCVImage_Destroy", ImageDestroy, why)
        && Bind(image, "NvCVImage_Init", ImageInit, why) && Bind(image, "NvCV_GetErrorStringFromCode", ErrorString, why);
    if (!ok) { why = "Video Effects runtime: " + why; Unload(); }
    return ok;
}

std::string VfxApi::Error(NvCV_Status s) const
{
    const char* t = ErrorString ? ErrorString(s) : nullptr;
    return std::to_string(s) + (t ? std::string(" (") + t + ")" : std::string());
}

void VfxApi::Unload()
{
    // the effect libraries stay resident: unloading NVIDIA's libraries mid-process is not something they support
    libs.clear();
    CreateEffect = nullptr;
}

} // namespace pmvs::nvl
