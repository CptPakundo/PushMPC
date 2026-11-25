// The CUDA engine of the Linux NVIDIA backend (nvl_engine.h).
#include "nvl_engine.h"
#include <dlfcn.h>
#include <sys/stat.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

extern "C" const unsigned char nvl_kernels_fatbin[];
extern "C" const unsigned long nvl_kernels_fatbin_size;

namespace pmvs::nvl {

namespace {
CudaApi& Cuda() { static CudaApi api; return api; }
bool FileExists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode); }
size_t Align(size_t v, size_t a) { return (v + a - 1) / a * a; }
// the VFX ImageEncodingMode of VideoSuperRes for RGB10A2 input/output (the wheel's nvvfx ImageEncoding enum)
constexpr unsigned kEncodingRgb10a2 = 1;

struct Ctx {   // push / pop the engine's context around every call (VapourSynth calls in on any thread)
    CudaApi& cu; bool ok;
    Ctx(CudaApi& c, CUcontext x) : cu(c), ok(c.cuCtxPushCurrent(x) == CUDA_SUCCESS) {}
    ~Ctx() { if (ok) { CUcontext p = nullptr; cu.cuCtxPopCurrent(&p); } }
};
}

GpuInfo ProbeGpu(int device, const std::string& runtimeDir)
{
    GpuInfo g;
    CudaApi& cu = Cuda();
    std::string why;
    if (device < 0) { const char* e = std::getenv("PUSHMPC_CUDA_DEVICE"); device = e ? std::atoi(e) : 0; }
    g.device = device;
    g.runtimeDir = runtimeDir.empty() ? DefaultRuntimeDir() : runtimeDir;
    const std::string libs = VfxApi::LibDir(g.runtimeDir);
    g.runtime = FileExists(libs + "/libVideoFX.so") && FileExists(libs + "/libNVCVImage.so");
    if (!g.runtime) g.runtimeWhy = "the NVIDIA Video Effects runtime is not installed in " + g.runtimeDir + " (scripts/nvidia-vfx-fetch.sh)";
#if !defined(__x86_64__) && !defined(_M_X64) && !defined(_WIN32)
    g.runtime = false;   // NVIDIA ships no Linux aarch64 build of the runtime: never offered here
    g.runtimeWhy = "NVIDIA publishes no Linux aarch64 build of its Video Effects runtime (nvidia-vfx: Linux x86-64, Windows x64 and ARM64 only)";
#endif
    if (!cu.Load(why)) { g.why = why; return g; }
    CUresult r = cu.cuInit(0);
    if (r != CUDA_SUCCESS) { g.why = "cuInit: " + cu.Error(r); return g; }
    cu.cuDriverGetVersion(&g.driverVersion);
    int count = 0;
    if (cu.cuDeviceGetCount(&count) != CUDA_SUCCESS || count <= 0) { g.why = "no CUDA device"; return g; }
    if (device >= count) { g.why = "CUDA device " + std::to_string(device) + " does not exist (" + std::to_string(count) + " present)"; return g; }
    CUdevice d = 0;
    if (cu.cuDeviceGet(&d, device) != CUDA_SUCCESS) { g.why = "cuDeviceGet failed"; return g; }
    char name[256] = {};
    cu.cuDeviceGetName(name, sizeof(name) - 1, d);
    g.name = name;
    cu.cuDeviceGetAttribute(&g.major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, d);
    cu.cuDeviceGetAttribute(&g.minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, d);
    cu.cuDeviceTotalMem(&g.memBytes, d);
    g.driver = true;
    g.cls = ClassOf(g.major, g.minor, g.name);
    g.caps.gpu = g.cls;
    const bool rt = g.runtime;
    g.caps.vsr = rt && VsrCapable(g.cls) && FileExists(libs + "/libnvVFXVideoSuperRes.so");
    g.caps.hdr = rt && VsrCapable(g.cls) && FileExists(libs + "/libnvVFXTrueHDR.so");
    const bool vfgFile = rt && FileExists(libs + "/libnvVFXVideoFrameGeneration.so");
    if (!vfgFile) g.caps.vfgWhy = rt ? "the VFG library is missing from the runtime" : g.runtimeWhy;
    else if (VfgNative(g.major, g.minor)) g.caps.vfg = true;
    else if (VfgAmpere(g.major, g.minor)) g.caps.vfgWhy = "RTX 30: the VFG compatibility self-test has not passed yet";
    else g.caps.vfgWhy = std::string(GpuClassName(g.cls)) + " has no VFG (NVIDIA ships it for SM89 and newer only)";
    return g;
}

Engine::~Engine()
{
    if (!m_cu || !m_ctx) return;
    {
        Ctx c(*m_cu, m_ctx);
        if (m_stream) m_cu->cuStreamSynchronize(m_stream);
        for (void* e : { m_vfg, m_hdr, m_vsr }) if (e) m_vfx.DestroyEffect(e);
        for (Image* im : { &m_packed, &m_vsrOut, &m_hdrOut, &m_enh[0], &m_enh[1], &m_vfgOut }) FreeImage(*im);
        for (auto& p : m_srcPlane) if (p) m_cu->cuMemFree(p);
        for (auto& p : m_yuv) if (p) m_cu->cuMemFree(p);
        for (auto& u : m_up) { if (u.host) m_cu->cuMemFreeHost(u.host); if (u.done) m_cu->cuEventDestroy(u.done); }
        for (auto& s : m_slots) { if (s.host) m_cu->cuMemFreeHost(s.host); if (s.done) m_cu->cuEventDestroy(s.done); }
        if (m_module) m_cu->cuModuleUnload(m_module);
        if (m_stream) m_cu->cuStreamDestroy(m_stream);
    }
    m_ampere.Restore();   // after the VFG effect is gone
    m_cu->cuDevicePrimaryCtxRelease(m_gpu.device);
}

bool Engine::Cu(CUresult r, const char* what, std::string& err)
{
    if (r == CUDA_SUCCESS) return true;
    err = std::string(what) + ": CUDA " + m_cu->Error(r);
    return false;
}
bool Engine::Cv(NvCV_Status s, const char* what, std::string& err)
{
    if (s == NVCV_SUCCESS) return true;
    err = std::string(what) + ": " + m_vfx.Error(s);
    return false;
}

bool Engine::MakeImage(Image& im, int w, int h, bool tenBit, std::string& err)
{
    im.w = w; im.h = h; im.tenBit = tenBit;
    if (!Cu(m_cu->cuMemAllocPitch(&im.ptr, &im.pitch, size_t(w) * 4, size_t(h), 4), "cuMemAllocPitch", err)) return false;
    const int pf = tenBit ? NVCV_RGB10A2 : NVCV_RGBA, ct = tenBit ? NVCV_P32 : NVCV_U8;
    // descriptor storage from the SDK, its own tiny pixel allocation released, then pointed at our buffer (PushFPS)
    if (!Cv(m_vfx.ImageCreate(1, 1, pf, ct, NVCV_CHUNKY, NVCV_GPU, 1, &im.desc), "NvCVImage_Create", err)) return false;
    m_vfx.ImageDealloc(im.desc);
    return Cv(m_vfx.ImageInit(im.desc, unsigned(w), unsigned(h), int(im.pitch), reinterpret_cast<void*>(im.ptr), pf, ct, NVCV_CHUNKY, NVCV_GPU), "NvCVImage_Init", err);
}
void Engine::FreeImage(Image& im)
{
    if (im.desc) { m_vfx.ImageDestroy(im.desc); im.desc = nullptr; }
    if (im.ptr) { m_cu->cuMemFree(im.ptr); im.ptr = 0; }
}

bool Engine::Launch(CUfunction f, unsigned gx, unsigned gy, void** args, std::string& err)
{
    return Cu(m_cu->cuLaunchKernel(f, (gx + 15) / 16, (gy + 15) / 16, 1, 16, 16, 1, 0, m_stream, args, nullptr), "cuLaunchKernel", err);
}

bool Engine::Open(const GpuInfo& gpu, const EngineConfig& cfg, std::string& err)
{
    m_gpu = gpu; m_cfg = cfg; m_cu = &Cuda();
    const Plan& p = cfg.plan;
    if (!gpu.driver) { err = gpu.why; return false; }
    if (!m_vfx.Load(gpu.runtimeDir, err)) return false;
    CUdevice dev = 0;
    if (!Cu(m_cu->cuDeviceGet(&dev, gpu.device), "cuDeviceGet", err) || !Cu(m_cu->cuDevicePrimaryCtxRetain(&m_ctx, dev), "cuDevicePrimaryCtxRetain", err)) { m_ctx = nullptr; return false; }
    Ctx c(*m_cu, m_ctx);
    if (!c.ok) { err = "cuCtxPushCurrent failed"; return false; }
    if (!Cu(m_cu->cuStreamCreate(&m_stream, CU_STREAM_DEFAULT), "cuStreamCreate", err)) return false;   // blocking: see nvl_engine.h
    if (!Cu(m_cu->cuModuleLoadData(&m_module, nvl_kernels_fatbin), "cuModuleLoadData (PushMPC kernels)", err)
        || !Cu(m_cu->cuModuleGetFunction(&m_toPacked8, m_module, "nvl_yuv8_to_packed"), "nvl_yuv8_to_packed", err)
        || !Cu(m_cu->cuModuleGetFunction(&m_toPacked16, m_module, "nvl_yuv16_to_packed"), "nvl_yuv16_to_packed", err)
        || !Cu(m_cu->cuModuleGetFunction(&m_toYuv, m_module, "nvl_packed_to_yuv420"), "nvl_packed_to_yuv420", err)) return false;

    // formats: VSR takes RGBA8 (RGB10A2 with ImageEncodingMode for an HDR source); TrueHDR RGBA8 -> RGB10A2 PQ BT.2020;
    // VFG either. A 10-bit SDR source keeps 10 bits when no VSR / HDR stage needs 8.
    const bool vsr = p.vsr, hdr = p.hdr && !cfg.srcHdr;
    m_packedTenBit = cfg.srcHdr || (cfg.bits > 8 && !vsr && !hdr);
    m_enhTenBit = cfg.srcHdr || hdr || (cfg.bits > 8 && !vsr);
    m_outBits = m_enhTenBit ? 10 : 8;
    m_outHdr = cfg.srcHdr || hdr;
    const int ow = vsr ? p.outW : cfg.srcW, oh = vsr ? p.outH : cfg.srcH;

    // device buffers
    const int cw = (cfg.srcW + 1) / 2, ch = (cfg.srcH + 1) / 2, bps = cfg.bits > 8 ? 2 : 1;
    const int pw[3] = { cfg.srcW, cw, cw }, ph[3] = { cfg.srcH, ch, ch };
    for (int i = 0; i < 3; ++i) {
        if (!Cu(m_cu->cuMemAllocPitch(&m_srcPlane[i], &m_srcPitch[i], size_t(pw[i]) * bps, size_t(ph[i]), 4), "cuMemAllocPitch (source)", err)) return false;
        m_upPitch[i] = Align(size_t(pw[i]) * bps, 64); m_upPlaneBytes[i] = m_upPitch[i] * size_t(ph[i]);
    }
    for (auto& u : m_up) {
        if (!Cu(m_cu->cuMemHostAlloc(&u.host, m_upPlaneBytes[0] + m_upPlaneBytes[1] + m_upPlaneBytes[2], CU_MEMHOSTALLOC_PORTABLE), "cuMemHostAlloc (upload)", err)
            || !Cu(m_cu->cuEventCreate(&u.done, CU_EVENT_DISABLE_TIMING), "cuEventCreate", err)) return false;
    }
    if (!MakeImage(m_packed, cfg.srcW, cfg.srcH, m_packedTenBit, err)) return false;
    if (vsr && !MakeImage(m_vsrOut, ow, oh, m_packedTenBit, err)) return false;
    for (auto& e : m_enh) if (!MakeImage(e, ow, oh, m_enhTenBit, err)) return false;
    const int ocw = (ow + 1) / 2, och = (oh + 1) / 2, obps = m_outBits > 8 ? 2 : 1;
    const int opw[3] = { ow, ocw, ocw }, oph[3] = { oh, och, och };
    for (int i = 0; i < 3; ++i)
        if (!Cu(m_cu->cuMemAllocPitch(&m_yuv[i], &m_yuvPitch[i], size_t(opw[i]) * obps, size_t(oph[i]), 4), "cuMemAllocPitch (output)", err)) return false;
    const int nslots = cfg.slots > 0 ? cfg.slots : std::min(18, 2 * std::max(1, p.k) + 2);
    m_slots.resize(size_t(nslots));
    for (auto& s : m_slots) {
        size_t total = 0;
        for (int i = 0; i < 3; ++i) { s.pitch[i] = Align(size_t(opw[i]) * obps, 64); total += s.pitch[i] * size_t(oph[i]); }
        if (!Cu(m_cu->cuMemHostAlloc(&s.host, total, CU_MEMHOSTALLOC_PORTABLE), "cuMemHostAlloc (readback)", err)
            || !Cu(m_cu->cuEventCreate(&s.done, CU_EVENT_DISABLE_TIMING), "cuEventCreate", err)) return false;
        uint8_t* q = static_cast<uint8_t*>(s.host);
        for (int i = 0; i < 3; ++i) { s.planes[i] = q; q += s.pitch[i] * size_t(oph[i]); }
    }

    std::string what;
    // RTX VSR
    if (vsr) {
        if (!Cv(m_vfx.CreateEffect(sel::kVsr, &m_vsr), "NvVFX_CreateEffect(VideoSuperRes)", err)
            || !Cv(m_vfx.SetImage(m_vsr, sel::kSrc0, m_packed.desc), "VideoSuperRes SrcImage0", err)
            || !Cv(m_vfx.SetImage(m_vsr, sel::kDst0, m_vsrOut.desc), "VideoSuperRes DstImage0", err)
            || !Cv(m_vfx.SetCudaStream(m_vsr, sel::kStream, m_stream), "VideoSuperRes CudaStream", err)
            || !Cv(m_vfx.SetU32(m_vsr, sel::kQuality, unsigned(p.vsrQuality)), "VideoSuperRes QualityLevel", err)) return false;
        if (m_packedTenBit && !Cv(m_vfx.SetU32(m_vsr, sel::kEncoding, kEncodingRgb10a2), "VideoSuperRes ImageEncodingMode (RGB10A2)", err)) return false;
        if (!Cv(m_vfx.LoadEffect(m_vsr), "NvVFX_Load(VideoSuperRes)", err)) return false;
        what += "RTX VSR quality " + std::to_string(p.vsrQuality) + " " + std::to_string(cfg.srcW) + "x" + std::to_string(cfg.srcH) + " -> " + std::to_string(ow) + "x" + std::to_string(oh) + (m_packedTenBit ? " (RGB10A2)" : "") + "; ";
    }
    // RTX Video HDR
    if (hdr) {
        // RGBA8 (VSR's output, or the converted source) -> RGB10A2 PQ BT.2020 into its own fixed buffer
        const Image& in = vsr ? m_vsrOut : m_packed;
        if (!MakeImage(m_hdrOut, ow, oh, true, err)) return false;
        if (!Cv(m_vfx.CreateEffect(sel::kTrueHdr, &m_hdr), "NvVFX_CreateEffect(TrueHDR)", err)
            || !Cv(m_vfx.SetImage(m_hdr, sel::kSrc0, in.desc), "TrueHDR SrcImage0", err)
            || !Cv(m_vfx.SetImage(m_hdr, sel::kDst0, m_hdrOut.desc), "TrueHDR DstImage0", err)
            || !Cv(m_vfx.SetCudaStream(m_hdr, sel::kStream, m_stream), "TrueHDR CudaStream", err)
            || !Cv(m_vfx.SetU32(m_hdr, sel::kContrast, 100), "TrueHDR Contrast", err) || !Cv(m_vfx.SetU32(m_hdr, sel::kSaturation, 100), "TrueHDR Saturation", err)
            || !Cv(m_vfx.SetU32(m_hdr, sel::kMiddleGray, 50), "TrueHDR MiddleGray", err)
            || !Cv(m_vfx.SetU32(m_hdr, sel::kLuminance, unsigned(std::clamp(cfg.hdrPeak, 400.f, 2000.f))), "TrueHDR Luminance", err)
            || !Cv(m_vfx.SetU32(m_hdr, sel::kDebandOff, 0), "TrueHDR DebandingOff", err)
            || !Cv(m_vfx.LoadEffect(m_hdr), "NvVFX_Load(TrueHDR)", err)) return false;
        what += "RTX Video HDR peak " + std::to_string(int(cfg.hdrPeak)) + " cd/m2; ";
    }
    // VFG (Ampere: the compatibility first, before the library loads its kernels)
    if (p.k >= 2) {
        if (!MakeImage(m_vfgOut, ow, oh, m_enhTenBit, err)) return false;
        const std::string vfgPath = VfxApi::LibDir(gpu.runtimeDir) + "/libnvVFXVideoFrameGeneration.so";
        m_vfgLib = m_vfx.Open("libnvVFXVideoFrameGeneration.so", err);
        if (!m_vfgLib) return false;
        if (cfg.ampere && !m_ampere.Install(m_vfgLib, vfgPath, m_cu->lib, gpu.device, err)) { err = "RTX 30 VFG compatibility: " + err; return false; }
        if (!Cv(m_vfx.CreateEffect(sel::kVfg, &m_vfg), "NvVFX_CreateEffect(VideoFrameGeneration)", err)
            || !Cv(m_vfx.SetCudaStream(m_vfg, sel::kStream, m_stream), "VFG CudaStream", err)
            || !Cv(m_vfx.SetU32(m_vfg, sel::kInW, unsigned(ow)), "VFG InputWidth", err) || !Cv(m_vfx.SetU32(m_vfg, sel::kInH, unsigned(oh)), "VFG InputHeight", err)
            || !Cv(m_vfx.SetU32(m_vfg, sel::kMode, unsigned(std::clamp(p.vfgQuality, 0, 2))), "VFG Mode", err)
            || !Cv(m_vfx.SetU32(m_vfg, sel::kAutoShot, 0), "VFG AutomaticShotChangeDetectionEnabled", err)   // PushMPC's cut guard decides
            || !Cv(m_vfx.SetImage(m_vfg, sel::kSrc0, m_enh[0].desc), "VFG SrcImage0", err) || !Cv(m_vfx.SetImage(m_vfg, sel::kSrc1, m_enh[1].desc), "VFG SrcImage1", err)
            || !Cv(m_vfx.LoadEffect(m_vfg), "NvVFX_Load(VideoFrameGeneration)", err)
            || !Cv(m_vfx.SetImage(m_vfg, sel::kDst0, m_vfgOut.desc), "VFG DstImage0", err)
            || !Cv(m_vfx.SetU32(m_vfg, sel::kMultiplier, unsigned(p.k)), "VFG FrameMultiplier", err)) return false;
        what += "VFG " + std::string(p.vfgQuality >= 2 ? "High" : p.vfgQuality <= 0 ? "Low" : "Medium") + " x" + std::to_string(p.k) + (cfg.ampere ? " (RTX 30 compatibility)" : "") + "; ";
    }
    unsigned version = 0;
    m_vfx.GetVersion(&version);
    char b[160];
    snprintf(b, sizeof(b), "VFX SDK 0x%08X on %s (SM %d.%d, driver %d.%d); output %s %d-bit %dx%d; %d readback slots", version, gpu.name.c_str(), gpu.major, gpu.minor,
             gpu.driverVersion / 1000, (gpu.driverVersion % 1000) / 10, m_outHdr ? "PQ BT.2020" : "BT.709", m_outBits, ow, oh, nslots);
    m_desc = what + b;
    m_open = true;
    return true;
}

int Engine::FreeSlots() const { int n = 0; for (const auto& s : m_slots) n += !s.busy; return n; }

bool Engine::Upload(const SrcView& v, int parity, std::string& err)
{
    Ctx c(*m_cu, m_ctx);
    if (!c.ok) { err = "cuCtxPushCurrent failed"; return false; }
    Up& u = m_up[m_upNext]; m_upNext ^= 1;
    if (u.used && !Cu(m_cu->cuEventSynchronize(u.done), "cuEventSynchronize (upload)", err)) return false;
    const int bps = v.bits > 8 ? 2 : 1, cw = (v.width + 1) / 2, ch = (v.height + 1) / 2;
    const int pw[3] = { v.width, cw, cw }, ph[3] = { v.height, ch, ch };
    uint8_t* host = static_cast<uint8_t*>(u.host);
    for (int i = 0; i < 3; ++i) {
        const size_t row = size_t(pw[i]) * bps;
        for (int y = 0; y < ph[i]; ++y) std::memcpy(host + m_upPitch[i] * size_t(y), v.data[i] + v.stride[i] * y, row);
        CUDA_MEMCPY2D m{};
        m.srcMemoryType = CU_MEMORYTYPE_HOST; m.srcHost = host; m.srcPitch = m_upPitch[i];
        m.dstMemoryType = CU_MEMORYTYPE_DEVICE; m.dstDevice = m_srcPlane[i]; m.dstPitch = m_srcPitch[i];
        m.WidthInBytes = row; m.Height = size_t(ph[i]);
        if (!Cu(m_cu->cuMemcpy2DAsync(&m, m_stream), "cuMemcpy2DAsync (upload)", err)) return false;
        host += m_upPlaneBytes[i];
    }
    if (!Cu(m_cu->cuEventRecord(u.done, m_stream), "cuEventRecord", err)) return false;
    u.used = true;
    // convert into the first stage's input, or straight into enhanced[parity]
    const bool stages = m_vsr || m_hdr;
    Image& dst = stages ? m_packed : m_enh[parity];
    struct { const uint8_t* p[3]; unsigned pitch[3]; } planes = { { reinterpret_cast<const uint8_t*>(m_srcPlane[0]), reinterpret_cast<const uint8_t*>(m_srcPlane[1]), reinterpret_cast<const uint8_t*>(m_srcPlane[2]) },
                                                                   { unsigned(m_srcPitch[0]), unsigned(m_srcPitch[1]), unsigned(m_srcPitch[2]) } };
    int w = v.width, h = v.height, tb = dst.tenBit ? 1 : 0;
    InFormat f = v.fmt; f.bits = v.bits;
    Coeffs k = MatrixCoeffs(f.matrix);
    void* out = reinterpret_cast<void*>(dst.ptr); unsigned outPitch = unsigned(dst.pitch);
    void* args[] = { &planes, &w, &h, &f, &k, &tb, &out, &outPitch };
    if (!Launch(v.bits > 8 ? m_toPacked16 : m_toPacked8, unsigned(w), unsigned(h), args, err)) return false;
    if (m_vsr && !Cv(m_vfx.Run(m_vsr, 1), "NvVFX_Run(VideoSuperRes)", err)) return false;
    if (m_hdr && !Cv(m_vfx.Run(m_hdr, 1), "NvVFX_Run(TrueHDR)", err)) return false;
    if (stages) {
        // the last stage's fixed output into enhanced[parity] (a device copy: the effects keep their bindings)
        const Image& last = m_hdr ? m_hdrOut : m_vsrOut;
        CUDA_MEMCPY2D m{};
        m.srcMemoryType = CU_MEMORYTYPE_DEVICE; m.srcDevice = last.ptr; m.srcPitch = last.pitch;
        m.dstMemoryType = CU_MEMORYTYPE_DEVICE; m.dstDevice = m_enh[parity].ptr; m.dstPitch = m_enh[parity].pitch;
        m.WidthInBytes = size_t(last.w) * 4; m.Height = size_t(last.h);
        if (!Cu(m_cu->cuMemcpy2DAsync(&m, m_stream), "cuMemcpy2DAsync (enhanced frame)", err)) return false;
    }
    return true;
}

int Engine::ToSlot(const Image& im, std::string& err)
{
    int idx = -1;
    for (size_t i = 0; i < m_slots.size(); ++i) if (!m_slots[i].busy) { idx = int(i); break; }
    if (idx < 0) { err = "no free readback slot"; return -1; }
    Slot& s = m_slots[size_t(idx)];
    const uint32_t* in = reinterpret_cast<const uint32_t*>(im.ptr);
    unsigned inPitch = unsigned(im.pitch);
    int tb = im.tenBit ? 1 : 0, w = im.w, h = im.h, bits = m_outBits;
    Coeffs k = MatrixCoeffs(m_outHdr ? 9 : 1);
    struct { uint8_t* p[3]; unsigned pitch[3]; } out = { { reinterpret_cast<uint8_t*>(m_yuv[0]), reinterpret_cast<uint8_t*>(m_yuv[1]), reinterpret_cast<uint8_t*>(m_yuv[2]) },
                                                         { unsigned(m_yuvPitch[0]), unsigned(m_yuvPitch[1]), unsigned(m_yuvPitch[2]) } };
    void* args[] = { &in, &inPitch, &tb, &w, &h, &k, &bits, &out };
    if (!Launch(m_toYuv, unsigned((w + 1) / 2), unsigned((h + 1) / 2), args, err)) return -1;
    const int obps = m_outBits > 8 ? 2 : 1, cw = (w + 1) / 2, ch = (h + 1) / 2;
    const int pw[3] = { w, cw, cw }, ph[3] = { h, ch, ch };
    for (int i = 0; i < 3; ++i) {
        CUDA_MEMCPY2D m{};
        m.srcMemoryType = CU_MEMORYTYPE_DEVICE; m.srcDevice = m_yuv[i]; m.srcPitch = m_yuvPitch[i];
        m.dstMemoryType = CU_MEMORYTYPE_HOST; m.dstHost = s.planes[i]; m.dstPitch = s.pitch[i];
        m.WidthInBytes = size_t(pw[i]) * obps; m.Height = size_t(ph[i]);
        if (!Cu(m_cu->cuMemcpy2DAsync(&m, m_stream), "cuMemcpy2DAsync (readback)", err)) return -1;
    }
    if (!Cu(m_cu->cuEventRecord(s.done, m_stream), "cuEventRecord", err)) return -1;
    s.busy = true;
    return idx;
}

int Engine::Generate(int parity, int j, int k, bool newPair, std::string& err)
{
    Ctx c(*m_cu, m_ctx);
    if (!c.ok) { err = "cuCtxPushCurrent failed"; return -1; }
    if (!m_vfg) { err = "VFG is not loaded"; return -1; }
    // NVIDIA's sample (and PushFPS): both inputs are rebound for each NEW pair, even when the pointers are unchanged;
    // rebinding for every subframe would start a new pair inside the SDK
    if (newPair) {
        if (!Cv(m_vfx.SetImage(m_vfg, sel::kSrc0, m_enh[1 - parity].desc), "VFG SrcImage0", err) || !Cv(m_vfx.SetImage(m_vfg, sel::kSrc1, m_enh[parity].desc), "VFG SrcImage1", err)
            || !Cv(m_vfx.SetU32(m_vfg, sel::kShot, 0), "VFG ShotChange", err)) return -1;
    }
    if (!Cv(m_vfx.SetU32(m_vfg, sel::kMultiplier, unsigned(k)), "VFG FrameMultiplier", err) || !Cv(m_vfx.SetU32(m_vfg, sel::kIndex, unsigned(j)), "VFG FrameIndex", err)
        || !Cv(m_vfx.Run(m_vfg, 1), "NvVFX_Run(VideoFrameGeneration)", err)) return -1;
    return ToSlot(m_vfgOut, err);
}

int Engine::Emit(int parity, std::string& err)
{
    Ctx c(*m_cu, m_ctx);
    if (!c.ok) { err = "cuCtxPushCurrent failed"; return -1; }
    return ToSlot(m_enh[parity], err);
}

int Engine::Query(int slot)
{
    if (slot < 0 || slot >= int(m_slots.size())) return -1;
    Ctx c(*m_cu, m_ctx);
    const CUresult r = m_cu->cuEventQuery(m_slots[size_t(slot)].done);
    return r == CUDA_SUCCESS ? 1 : r == CUDA_ERROR_NOT_READY ? 0 : -1;
}

bool Engine::Wait(int slot, uint32_t timeoutMs)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        const int q = Query(slot);
        if (q != 0) return q == 1;
        if (std::chrono::steady_clock::now() >= until) return false;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

OutView Engine::View(int slot) const
{
    OutView v;
    const Slot& s = m_slots[size_t(slot)];
    const int w = m_vfgOut.w ? m_vfgOut.w : m_enh[0].w, h = m_vfgOut.h ? m_vfgOut.h : m_enh[0].h;
    for (int i = 0; i < 3; ++i) { v.data[i] = s.planes[i]; v.pitch[i] = s.pitch[i]; }
    v.width = w; v.height = h; v.bits = m_outBits;
    return v;
}

void Engine::Release(int slot) { if (slot >= 0 && slot < int(m_slots.size())) m_slots[size_t(slot)].busy = false; }

bool Engine::Drain(std::string& err)
{
    Ctx c(*m_cu, m_ctx);
    if (!Cu(m_cu->cuStreamSynchronize(m_stream), "cuStreamSynchronize", err)) return false;
    for (auto& s : m_slots) s.busy = false;
    return true;
}

} // namespace pmvs::nvl
