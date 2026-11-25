// pushmpc.Enhance, backend "nvidia" on Linux: the CUDA engine. One blocking CUDA stream carries everything in order
// (VideoSuperRes queues part of its work on the legacy default stream, which orders only against blocking streams:
// PushFPS VfxSrBackend's finding), so no host wait sits between the stages:
//
//   source planes -> pinned upload slot (CPU copy) -> device planes (async) -> packed RGB (kernel)
//     -> RTX VSR (VideoSuperRes) -> RTX Video HDR (TrueHDR, SDR source on an HDR output) -> enhanced[s % 2]
//   per generated frame j of the pair (s - 1, s): VFG (FrameIndex j of FrameMultiplier k) -> 4:2:0 (kernel)
//     -> pinned output slot (async readback) -> event;   a cut pair: enhanced[A] -> 4:2:0 instead (A held)
//   the real frame s: enhanced[s % 2] -> 4:2:0 -> pinned output slot -> event
//
// The caller (nvl_backend.h) polls the slots' events in presentation order and copies finished slots into
// VapourSynth frames, so the GPU works ahead while earlier frames are handed out (the pipeline holds 4K output rates
// with the copies overlapped). Linux only (libcuda through nvl_cuda.h, the runtime through nvl_vfx.h).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "nvl_ampere.h"
#include "nvl_auto.h"
#include "nvl_colour.h"
#include "nvl_cuda.h"
#include "nvl_gpuguard.h"
#include "nvl_vfx.h"

namespace pmvs::nvl {

// what Probe found (once per process)
struct GpuInfo {
    bool driver = false;          // libcuda loaded and cuInit succeeded
    int device = 0, major = 0, minor = 0, driverVersion = 0;
    size_t memBytes = 0;
    std::string name, why;        // why: no driver / no device
    GpuClass cls = GpuClass::None;
    bool runtime = false;         // the Video Effects runtime folder has the core libraries
    std::string runtimeDir, runtimeWhy;
    Caps caps;
};
// device: -1 = $PUSHMPC_CUDA_DEVICE, else 0. runtimeDir: "" = DefaultRuntimeDir(). selfTest: the Ampere self-test result
// (-1 unknown/not run, 0 failed, 1 passed): set by the caller (nvl_selftest.h) before the caps are final.
GpuInfo ProbeGpu(int device, const std::string& runtimeDir);

struct EngineConfig {
    int srcW = 0, srcH = 0, bits = 8;
    bool srcHdr = false;          // PQ / HLG source (stays HDR; R'G'B' in its own transfer)
    Plan plan;                    // k, VFG quality, VSR level / quality, output size, RTX Video HDR
    float hdrPeak = 1000;         // RTX Video HDR peak luminance (cd/m2, 400..2000)
    bool ampere = false;          // the RTX 30 compatibility (Install before VFG loads)
    int slots = 0;                // pinned output slots (0 = Auto: 2k + 2, at most 18)
    bool operator==(const EngineConfig& o) const
    {
        return srcW == o.srcW && srcH == o.srcH && bits == o.bits && srcHdr == o.srcHdr && plan.k == o.plan.k && plan.vfgQuality == o.plan.vfgQuality && plan.vsr == o.plan.vsr
            && plan.vsrQuality == o.plan.vsrQuality && plan.outW == o.plan.outW && plan.outH == o.plan.outH && plan.hdr == o.plan.hdr && hdrPeak == o.hdrPeak && ampere == o.ampere;
    }
};

// a source frame as VapourSynth hands it out (planar 4:2:0)
struct SrcView { const uint8_t* data[3] = {}; ptrdiff_t stride[3] = {}; int width = 0, height = 0, bits = 8; InFormat fmt; };
// a finished output frame in pinned host memory (4:2:0, 8 or 10 bits in 16-bit words)
struct OutView { const uint8_t* data[3] = {}; size_t pitch[3] = {}; int width = 0, height = 0, bits = 8; };

class Engine {
public:
    ~Engine();
    bool Open(const GpuInfo& gpu, const EngineConfig& cfg, std::string& err);
    const EngineConfig& Config() const { return m_cfg; }
    bool OutTenBit() const { return m_outBits > 8; }
    bool OutHdr() const { return m_outHdr; }
    int Slots() const { return int(m_slots.size()); }
    int FreeSlots() const;
    std::string Describe() const { return m_desc; }

    // the source frame into enhanced[parity] (upload, convert, VSR, HDR); queued on the stream
    bool Upload(const SrcView& v, int parity, std::string& err);
    // into a free output slot (returns its index, or -1): the VFG frame j of k of the pair (enhanced[1 - parity],
    // enhanced[parity]); newPair: the first subframe of a new pair (the inputs are rebound)
    int Generate(int parity, int j, int k, bool newPair, std::string& err);
    // the enhanced frame `parity` as an output (a real frame, or A held over a cut pair)
    int Emit(int parity, std::string& err);
    // slot state: 1 ready, 0 still running, -1 failed
    int Query(int slot);
    bool Wait(int slot, uint32_t timeoutMs);
    OutView View(int slot) const;
    void Release(int slot);
    // after a seek / reset: drain the stream, free every slot
    bool Drain(std::string& err);
    // the GPU cut guard (nvl_gpuguard.h): on when this build has it and it opened; Guard() tests the pair ending at
    // enhanced[parity]; generated frames then hold A on the GPU when the pair is a cut
    bool HasGpuGuard() const { return m_guard.Ready(); }
    bool Guard(int parity, bool compare, std::string& err);
    uint64_t GuardCuts() const { return m_guard.Cuts(); }

private:
    struct Image { CUdeviceptr ptr = 0; size_t pitch = 0; void* desc = nullptr; int w = 0, h = 0; bool tenBit = false; };
    struct Slot { void* host = nullptr; size_t pitch[3] = {}; uint8_t* planes[3] = {}; CUevent done = nullptr; bool busy = false; };
    bool Cu(CUresult r, const char* what, std::string& err);
    bool Cv(NvCV_Status s, const char* what, std::string& err);
    bool MakeImage(Image& im, int w, int h, bool tenBit, std::string& err);
    void FreeImage(Image& im);
    // holdFlag: a device word; when non-zero at run time the slot gets `alt` instead (the GPU cut guard's hold)
    int ToSlot(const Image& im, std::string& err, CUdeviceptr holdFlag = 0, const Image* alt = nullptr);
    bool Launch(CUfunction f, unsigned gx, unsigned gy, void** args, std::string& err);

    CudaApi* m_cu = nullptr;
    VfxApi m_vfx;
    GpuInfo m_gpu;
    EngineConfig m_cfg;
    CUcontext m_ctx = nullptr;
    CUstream m_stream = nullptr;
    CUmodule m_module = nullptr;
    CUfunction m_toPacked8 = nullptr, m_toPacked16 = nullptr, m_toYuv = nullptr;
    void* m_vsr = nullptr; void* m_hdr = nullptr; void* m_vfg = nullptr;
    void* m_vfgLib = nullptr;
    AmpereCompat m_ampere;
    GpuGuard m_guard;
    // device buffers
    CUdeviceptr m_srcPlane[3] = {}; size_t m_srcPitch[3] = {};
    Image m_packed, m_vsrOut, m_hdrOut, m_enh[2], m_vfgOut;
    CUdeviceptr m_yuv[3] = {}; size_t m_yuvPitch[3] = {};
    // pinned upload ring
    struct Up { void* host = nullptr; CUevent done = nullptr; bool used = false; };
    Up m_up[2]; int m_upNext = 0;
    size_t m_upPlaneBytes[3] = {}, m_upPitch[3] = {};
    std::vector<Slot> m_slots;
    int m_outBits = 8; bool m_outHdr = false, m_packedTenBit = false, m_enhTenBit = false;
    std::string m_desc;
    bool m_open = false;
};

} // namespace pmvs::nvl
