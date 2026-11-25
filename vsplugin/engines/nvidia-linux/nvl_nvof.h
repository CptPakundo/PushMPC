// NVIDIA's optical-flow hardware (NVOFA: Turing except TU117, and newer) through the CUDA interface of the driver's own
// library (libnvidia-opticalflow.so.1 on Linux, nvofapi64.dll on Windows): no SDK download, nothing redistributed. The
// declarations below follow the MIT-licensed interface headers of the Optical Flow SDK (nvOpticalFlowCommon.h,
// nvOpticalFlowCuda.h, API 5.0), reduced to what this engine calls.
//
// One session per stream: grayscale 8-bit inputs at the source size, both directions (two executes: Turing has no
// single-call bidirectional mode), S10.5 vectors on the hardware's grid (4 on Turing; the smallest supported is used),
// 8-bit cost. Used by
//   - the fast interpolation tier (no VFG on this GPU: RTX 20, or RTX 30 before its self-test): nvl_kernels.cu warps;
//   - the GPU cut guard's flow measures (feature bit 128, as Windows' guard with NVOF).
#pragma once
#include <cstdint>
#include <string>
#include "nvl_cuda.h"

namespace pmvs::nvl {

namespace nvof {
using Status = int;   // NV_OF_STATUS: 0 = NV_OF_SUCCESS
struct Handle_st; struct Buffer_st; struct Priv_st;
using Handle = Handle_st*;
using Buffer = Buffer_st*;
using Priv = Priv_st*;
enum : uint32_t { kOutGrid1 = 1, kOutGrid2 = 2, kOutGrid4 = 4 };
enum : uint32_t { kModeOpticalFlow = 1 };
enum : uint32_t { kPerfSlow = 5, kPerfMedium = 10, kPerfFast = 20 };
enum : uint32_t { kUsageInput = 1, kUsageOutput = 2, kUsageHint = 3, kUsageCost = 4 };
enum : uint32_t { kFmtGray8 = 1, kFmtShort2 = 5, kFmtUint8 = 7 };
enum : uint32_t { kCapsGridSizes = 0, kCapsWidthMin = 4, kCapsHeightMin = 5, kCapsWidthMax = 6, kCapsHeightMax = 7 };
enum : uint32_t { kCudaDevicePtr = 2 };
struct InitParams {
    uint32_t width, height, outGridSize, hintGridSize, mode, perfLevel, enableExternalHints, enableOutputCost;
    Priv hPrivData;
    uint32_t disparityRange, enableRoi, predDirection, enableGlobalFlow, inputBufferFormat;
};
struct BufferDesc { uint32_t width, height, bufferUsage, bufferFormat; };
struct Stride { uint32_t x, y; };
struct StrideInfo { Stride plane[3]; uint32_t numPlanes; };
struct ExecIn {
    Buffer inputFrame, referenceFrame, externalHints;
    uint32_t disableTemporalHints, padding;
    Priv hPrivData;
    uint32_t padding2, numRois;
    void* roiData;
};
struct ExecOut { Buffer outputBuffer, outputCostBuffer; Priv hPrivData; Buffer bwdOutputBuffer, bwdOutputCostBuffer, globalFlowBuffer; };
#ifdef _WIN32
#define NVL_NVOFAPI __stdcall
#else
#define NVL_NVOFAPI
#endif
struct FunctionList {
    Status (NVL_NVOFAPI* CreateOpticalFlowCuda)(CUcontext, Handle*);
    Status (NVL_NVOFAPI* Init)(Handle, const InitParams*);
    Status (NVL_NVOFAPI* CreateGPUBufferCuda)(Handle, const BufferDesc*, uint32_t, Buffer*);
    void* (NVL_NVOFAPI* GPUBufferGetCUarray)(Buffer);
    CUdeviceptr (NVL_NVOFAPI* GPUBufferGetCUdeviceptr)(Buffer);
    Status (NVL_NVOFAPI* GPUBufferGetStrideInfo)(Buffer, StrideInfo*);
    Status (NVL_NVOFAPI* SetIOCudaStreams)(Handle, CUstream, CUstream);
    Status (NVL_NVOFAPI* Execute)(Handle, const ExecIn*, ExecOut*);
    Status (NVL_NVOFAPI* DestroyGPUBufferCuda)(Buffer);
    Status (NVL_NVOFAPI* Destroy)(Handle);
    Status (NVL_NVOFAPI* GetLastError)(Handle, char*, uint32_t*);
    Status (NVL_NVOFAPI* GetCaps)(Handle, uint32_t, uint32_t*, uint32_t*);
};
inline constexpr uint32_t kApiVersion = (5u << 4) | 0u;   // NV_OF_API_VERSION 5.0
} // namespace nvof

class OpticalFlow {
public:
    // the driver's library only (no device work): for the probe
    static bool LibraryPresent(std::string& why);
    // a session on `ctx` for width x height grayscale inputs (the source size); both directions with cost
    bool Open(CudaApi& cu, CUcontext ctx, CUstream stream, uint32_t width, uint32_t height, std::string& err);
    void Close();
    bool Ready() const { return m_ready; }
    // the input buffer of `parity` (gray8, pitch InputPitch()) a kernel writes the frame's luma into
    CUdeviceptr Input(int parity) const { return m_inPtr[parity & 1]; }
    uint32_t InputPitch() const { return m_inPitch; }
    // both directions for the pair ending at `parity`: backward = current -> previous (the cut guard's "forward"),
    // forward = previous -> current (with cost each)
    bool Run(int parity, std::string& err);
    CUdeviceptr Backward() const { return m_outPtr[0]; }   // vectors at the current frame, pointing to the previous one
    CUdeviceptr Forward() const { return m_outPtr[1]; }    // vectors at the previous frame, pointing to the current one
    CUdeviceptr Cost() const { return m_costPtr[0]; }      // the backward direction's cost (what the guard reads)
    uint32_t FlowPitch() const { return m_outPitch; }
    uint32_t CostPitch() const { return m_costPitch; }
    uint32_t Grid() const { return m_grid; }
    uint32_t GridW() const { return (m_w + m_grid - 1) / m_grid; }
    uint32_t GridH() const { return (m_h + m_grid - 1) / m_grid; }
    uint32_t Width() const { return m_w; }
    uint32_t Height() const { return m_h; }
    ~OpticalFlow() { Close(); }

private:
    nvof::FunctionList m_api{};
    nvof::Handle m_of = nullptr;
    nvof::Buffer m_in[2] = {}, m_out[2] = {}, m_cost[2] = {};
    CUdeviceptr m_inPtr[2] = {}, m_outPtr[2] = {}, m_costPtr[2] = {};
    uint32_t m_inPitch = 0, m_outPitch = 0, m_costPitch = 0, m_grid = 4, m_w = 0, m_h = 0;
    bool m_ready = false;
};

} // namespace pmvs::nvl
