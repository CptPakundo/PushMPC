// The CUDA driver API, loaded at run time from the NVIDIA driver (libcuda.so.1): only the declarations this backend
// uses, so the plugin builds and loads on machines without the CUDA toolkit or an NVIDIA driver (it then reports "no
// NVIDIA driver" and Auto picks another backend). Kernels are compiled at build time by nvcc into a fatbin with PTX
// (nvl_kernels.cu) and loaded through cuModuleLoadData.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace pmvs::nvl {

using CUresult = int;
using CUdevice = int;
using CUdeviceptr = unsigned long long;
struct CUctx_st; struct CUstream_st; struct CUmod_st; struct CUfunc_st; struct CUevent_st;
using CUcontext = CUctx_st*;
using CUstream = CUstream_st*;
using CUmodule = CUmod_st*;
using CUfunction = CUfunc_st*;
using CUevent = CUevent_st*;

inline constexpr CUresult CUDA_SUCCESS = 0, CUDA_ERROR_NOT_READY = 600, CUDA_ERROR_NO_DEVICE = 100;
inline constexpr int CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR = 75, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR = 76;
inline constexpr unsigned CU_STREAM_DEFAULT = 0, CU_STREAM_NON_BLOCKING = 1;
inline constexpr unsigned CU_EVENT_DEFAULT = 0, CU_EVENT_DISABLE_TIMING = 2;
inline constexpr unsigned CU_MEMHOSTALLOC_PORTABLE = 1;
enum CUmemorytype { CU_MEMORYTYPE_HOST = 1, CU_MEMORYTYPE_DEVICE = 2, CU_MEMORYTYPE_ARRAY = 3, CU_MEMORYTYPE_UNIFIED = 4 };
struct CUDA_MEMCPY2D {
    size_t srcXInBytes, srcY; CUmemorytype srcMemoryType; const void* srcHost; CUdeviceptr srcDevice; void* srcArray; size_t srcPitch;
    size_t dstXInBytes, dstY; CUmemorytype dstMemoryType; void* dstHost; CUdeviceptr dstDevice; void* dstArray; size_t dstPitch;
    size_t WidthInBytes, Height;
};

struct CudaApi {
    void* lib = nullptr;
    CUresult (*cuInit)(unsigned) = nullptr;
    CUresult (*cuDriverGetVersion)(int*) = nullptr;
    CUresult (*cuDeviceGetCount)(int*) = nullptr;
    CUresult (*cuDeviceGet)(CUdevice*, int) = nullptr;
    CUresult (*cuDeviceGetName)(char*, int, CUdevice) = nullptr;
    CUresult (*cuDeviceGetAttribute)(int*, int, CUdevice) = nullptr;
    CUresult (*cuDeviceTotalMem)(size_t*, CUdevice) = nullptr;
    CUresult (*cuDevicePrimaryCtxRetain)(CUcontext*, CUdevice) = nullptr;
    CUresult (*cuDevicePrimaryCtxRelease)(CUdevice) = nullptr;
    CUresult (*cuCtxPushCurrent)(CUcontext) = nullptr;
    CUresult (*cuCtxPopCurrent)(CUcontext*) = nullptr;
    CUresult (*cuCtxGetDevice)(CUdevice*) = nullptr;
    CUresult (*cuStreamCreate)(CUstream*, unsigned) = nullptr;
    CUresult (*cuStreamDestroy)(CUstream) = nullptr;
    CUresult (*cuStreamSynchronize)(CUstream) = nullptr;
    CUresult (*cuEventCreate)(CUevent*, unsigned) = nullptr;
    CUresult (*cuEventRecord)(CUevent, CUstream) = nullptr;
    CUresult (*cuEventQuery)(CUevent) = nullptr;
    CUresult (*cuEventSynchronize)(CUevent) = nullptr;
    CUresult (*cuEventElapsedTime)(float*, CUevent, CUevent) = nullptr;
    CUresult (*cuEventDestroy)(CUevent) = nullptr;
    CUresult (*cuMemAlloc)(CUdeviceptr*, size_t) = nullptr;
    CUresult (*cuMemAllocPitch)(CUdeviceptr*, size_t*, size_t, size_t, unsigned) = nullptr;
    CUresult (*cuMemFree)(CUdeviceptr) = nullptr;
    CUresult (*cuMemHostAlloc)(void**, size_t, unsigned) = nullptr;
    CUresult (*cuMemFreeHost)(void*) = nullptr;
    CUresult (*cuMemcpy2DAsync)(const CUDA_MEMCPY2D*, CUstream) = nullptr;
    CUresult (*cuMemcpyHtoDAsync)(CUdeviceptr, const void*, size_t, CUstream) = nullptr;
    CUresult (*cuMemcpyDtoHAsync)(void*, CUdeviceptr, size_t, CUstream) = nullptr;
    CUresult (*cuMemcpyDtoH)(void*, CUdeviceptr, size_t) = nullptr;
    CUresult (*cuModuleLoadData)(CUmodule*, const void*) = nullptr;
    CUresult (*cuModuleGetFunction)(CUfunction*, CUmodule, const char*) = nullptr;
    CUresult (*cuModuleUnload)(CUmodule) = nullptr;
    CUresult (*cuLaunchKernel)(CUfunction, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, CUstream, void**, void**) = nullptr;
    CUresult (*cuGetErrorName)(CUresult, const char**) = nullptr;
    CUresult (*cuGetErrorString)(CUresult, const char**) = nullptr;

    // dlopen libcuda.so.1 (or `path`) and bind everything; false with the reason when anything is missing
    bool Load(std::string& why, const char* path = nullptr);
    std::string Error(CUresult r) const;
};

} // namespace pmvs::nvl
