// NVIDIA optical-flow hardware through the driver's library (nvl_nvof.h).
#include "nvl_nvof.h"
#include <cstring>
#include "nvl_os.h"

namespace pmvs::nvl {

namespace {
#ifdef _WIN32
constexpr const char* kLib = "nvofapi64.dll";
#else
constexpr const char* kLib = "libnvidia-opticalflow.so.1";
#endif
using CreateInstanceFn = nvof::Status (NVL_NVOFAPI*)(uint32_t, nvof::FunctionList*);
using MaxVersionFn = nvof::Status (NVL_NVOFAPI*)(uint32_t*);
void* Lib(std::string& why)
{
    static void* lib = nullptr;
    static std::string err;
    static bool tried = false;
    if (!tried) { tried = true; lib = os::Open(kLib, err); }
    why = err;
    return lib;
}
}

bool OpticalFlow::LibraryPresent(std::string& why)
{
    void* l = Lib(why);
    return l && os::Sym(l, "NvOFAPICreateInstanceCuda");
}

bool OpticalFlow::Open(CudaApi& cu, CUcontext ctx, CUstream stream, uint32_t width, uint32_t height, std::string& err)
{
    Close();
    (void)cu;
    void* l = Lib(err);
    if (!l) { err = "optical flow: " + err; return false; }
    auto create = reinterpret_cast<CreateInstanceFn>(os::Sym(l, "NvOFAPICreateInstanceCuda"));
    if (!create) { err = "optical flow: NvOFAPICreateInstanceCuda missing"; return false; }
    if (auto maxv = reinterpret_cast<MaxVersionFn>(os::Sym(l, "NvOFGetMaxSupportedApiVersion"))) {
        uint32_t v = 0;
        if (maxv(&v) == 0 && v < nvof::kApiVersion) { err = "optical flow: the driver supports API " + std::to_string(v >> 4) + "." + std::to_string(v & 15) + ", 5.0 needed"; return false; }
    }
    auto ok = [&](nvof::Status s, const char* what) {
        if (s == 0) return true;
        char msg[256] = {}; uint32_t n = sizeof(msg) - 1;
        if (m_of && m_api.GetLastError) m_api.GetLastError(m_of, msg, &n);
        err = std::string("optical flow: ") + what + " status " + std::to_string(s) + (msg[0] ? std::string(" (") + msg + ")" : std::string());
        return false;
    };
    if (!ok(create(nvof::kApiVersion, &m_api), "NvOFAPICreateInstanceCuda") || !ok(m_api.CreateOpticalFlowCuda(ctx, &m_of), "create")) { Close(); return false; }
    // the smallest output grid this GPU supports (Turing: 4 only; Ampere and newer also 1 and 2): we warp from it
    uint32_t grids[8] = {}, n = 8;
    m_grid = 4;
    if (m_api.GetCaps && m_api.GetCaps(m_of, nvof::kCapsGridSizes, grids, &n) == 0)
        for (uint32_t i = 0; i < n && i < 8; ++i) if (grids[i] == 2) m_grid = 2;   // 2: detail enough, a quarter of 1's cost
    uint32_t wmax = 0, hmax = 0, one = 1;
    if (m_api.GetCaps && m_api.GetCaps(m_of, nvof::kCapsWidthMax, &wmax, &one) == 0 && m_api.GetCaps(m_of, nvof::kCapsHeightMax, &hmax, &one) == 0
        && wmax && hmax && (width > wmax || height > hmax)) { err = "optical flow: " + std::to_string(width) + "x" + std::to_string(height) + " exceeds the hardware's " + std::to_string(wmax) + "x" + std::to_string(hmax); Close(); return false; }
    nvof::InitParams ip{};
    ip.width = width; ip.height = height; ip.outGridSize = m_grid; ip.hintGridSize = 0; ip.mode = nvof::kModeOpticalFlow;
    ip.perfLevel = nvof::kPerfFast; ip.enableExternalHints = 0; ip.enableOutputCost = 1; ip.predDirection = 0; ip.inputBufferFormat = nvof::kFmtGray8;
    if (!ok(m_api.Init(m_of, &ip), "init") || !ok(m_api.SetIOCudaStreams(m_of, stream, stream), "streams")) { Close(); return false; }
    m_w = width; m_h = height;
    const nvof::BufferDesc din{ width, height, nvof::kUsageInput, nvof::kFmtGray8 };
    const nvof::BufferDesc dout{ GridW(), GridH(), nvof::kUsageOutput, nvof::kFmtShort2 };
    const nvof::BufferDesc dcost{ GridW(), GridH(), nvof::kUsageCost, nvof::kFmtUint8 };
    for (int i = 0; i < 2; ++i) {
        if (!ok(m_api.CreateGPUBufferCuda(m_of, &din, nvof::kCudaDevicePtr, &m_in[i]), "input buffer") || !ok(m_api.CreateGPUBufferCuda(m_of, &dout, nvof::kCudaDevicePtr, &m_out[i]), "output buffer")
            || !ok(m_api.CreateGPUBufferCuda(m_of, &dcost, nvof::kCudaDevicePtr, &m_cost[i]), "cost buffer")) { Close(); return false; }
        m_inPtr[i] = m_api.GPUBufferGetCUdeviceptr(m_in[i]);
        m_outPtr[i] = m_api.GPUBufferGetCUdeviceptr(m_out[i]);
        m_costPtr[i] = m_api.GPUBufferGetCUdeviceptr(m_cost[i]);
    }
    nvof::StrideInfo si{};
    if (!ok(m_api.GPUBufferGetStrideInfo(m_in[0], &si), "input stride")) { Close(); return false; }
    m_inPitch = si.plane[0].x;
    if (!ok(m_api.GPUBufferGetStrideInfo(m_out[0], &si), "output stride")) { Close(); return false; }
    m_outPitch = si.plane[0].x;
    if (!ok(m_api.GPUBufferGetStrideInfo(m_cost[0], &si), "cost stride")) { Close(); return false; }
    m_costPitch = si.plane[0].x;
    m_ready = true;
    return true;
}

bool OpticalFlow::Run(int parity, std::string& err)
{
    if (!m_ready) { err = "optical flow not open"; return false; }
    const int cur = parity & 1, prev = 1 - cur;
    for (int dir = 0; dir < 2; ++dir) {
        nvof::ExecIn in{};
        in.inputFrame = m_in[dir == 0 ? cur : prev];
        in.referenceFrame = m_in[dir == 0 ? prev : cur];
        in.disableTemporalHints = 1;   // the two directions alternate: the previous call's vectors are no hint
        nvof::ExecOut out{};
        out.outputBuffer = m_out[dir];
        out.outputCostBuffer = m_cost[dir];
        const nvof::Status s = m_api.Execute(m_of, &in, &out);
        if (s != 0) { err = "optical flow: execute status " + std::to_string(s); return false; }
    }
    return true;
}

void OpticalFlow::Close()
{
    if (m_api.DestroyGPUBufferCuda)
        for (int i = 0; i < 2; ++i)
            for (nvof::Buffer* b : { &m_in[i], &m_out[i], &m_cost[i] }) if (*b) { m_api.DestroyGPUBufferCuda(*b); *b = nullptr; }
    if (m_of && m_api.Destroy) m_api.Destroy(m_of);
    m_of = nullptr;
    m_ready = false;
}

} // namespace pmvs::nvl
