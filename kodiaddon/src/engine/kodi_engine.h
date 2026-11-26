// kodi_engine: the add-on's engine interface. One source frame in, then the pictures of the pair (previous, current):
// the previous frame itself (Real, through the upscaler when there is one) and generated frames at phase t.
//
// Backends, best first (OpenEngine picks; the CPU engine is always the fallback):
//   gpu   the portable GPU engine (vsplugin/engines/gpu, Vulkan 1.2 compute; macOS through MoltenVK): PushFPS's block
//         matcher, motion-compensated interpolation, the cut guard, NIS / FSR1 upscaling;
//   cpu   PushFPS's portable core on the CPU (vsplugin/engines/portable through pm_engine.h): no upscaling.
// Pictures are planar YUV 4:2:0, 8 bits or 16-bit containers (10 bits), as pm_engine.h describes.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include "pm_engine.h"

namespace pmk {

struct EngineCfg {
    int width = 0, height = 0, bits = 8;
    int threads = 0, quality = 1;      // quality: 0 fast, 1 balanced (CPU); the GPU engine uses its high setting at 1
    int outWidth = 0, outHeight = 0;   // 0: source size; else the GPU upscaler's output (ignored by the CPU engine)
    int upscaler = 0;                  // 0 none, 1 NIS, 2 FSR1
    int backend = 0;                   // 0 Auto (GPU, else CPU), 1 CPU only, 2 GPU only
    bool semiPlanar = false;           // GPU engine output NV12 / P010 (U and V interleaved; 10-bit in the high bits)
    bool external = false;             // frames arrive on the GPU (PushExternal: Android hardware buffers), not as planes
    // for engines that plan the multiplier when they open (nvl: NVIDIA VFG has a fixed FrameMultiplier)
    double srcFps = 0, displayHz = 0;
    int smoother = 0;                  // 0 Auto, 1 off, 2..
    int matrix = 1;                    // H.273
    bool fullRange = false, hdr = false;
};

// the GPU engine's output layout (packed rows); a player buffer laid out like this can be written in place
struct OutLayout { int pitchY = 0, pitchC = 0; uint64_t offU = 0, offV = 0, size = 0; };

struct EngineStats { uint64_t pushed = 0, pairs = 0, cuts = 0, generated = 0; double meMs = 0, genMs = 0; };

class IEngine {
public:
    virtual ~IEngine() = default;
    virtual std::string Describe() const = 0;
    virtual bool IsGpu() const = 0;
    virtual int OutWidth() const = 0;
    virtual int OutHeight() const = 0;
    // a new source frame; contiguous: it directly follows the previous one (else no pair until the next push)
    virtual bool Push(const pm_yuv& src, bool contiguous, std::string& err) = 0;
    virtual bool HavePair() const = 0;
    virtual bool PairIsCut() const = 0;
    // the previous frame of the pair at phase t (0 < t < 1); a cut pair gives the nearer real frame
    virtual bool Generate(float t, const pm_yuv_out& out, std::string& err) = 0;
    // the previous frame as it is, at the output size; false: the caller copies it itself (no upscaler)
    virtual bool Real(bool current, const pm_yuv_out& out, std::string& err) = 0;   // current: the newest frame (the end of a clip)
    virtual void Forget() = 0;
    // zero copy (Linux, GPU engine with dma-buf import): the player's frame buffer (a dma-buf fd laid out as Layout())
    // becomes the output; Import returns an id or -1 with the reason
    virtual bool Layout(OutLayout&) const { return false; }
    virtual bool CanImport() const { return false; }
    virtual int Import(int /*fd*/, uint64_t /*size*/, std::string& err) { err = "no zero copy on this engine"; return -1; }
    virtual void ReleaseImport(int /*id*/) {}
    // a player's system-memory frame buffer (VK_EXT_external_memory_host): ptr and size aligned to HostAlignment()
    // a frame already on the GPU: fill writes it into the engine's free source slot (pmvs::gpu::Engine::Import) and
    // returns that slot, or -1. GPU engine only (configured with EngineCfg::external).
    using SlotFiller = std::function<int(void* gpuEngine, std::string& err)>;
    virtual bool PushExternal(const SlotFiller& /*fill*/, bool /*contiguous*/, std::string& err) { err = "no GPU frames on this engine"; return false; }
    virtual uint64_t HostAlignment() const { return 0; }
    virtual int ImportHost(void* /*ptr*/, uint64_t /*size*/, std::string& err) { err = "no host import on this engine"; return -1; }
    virtual bool GenerateInto(float /*t*/, int /*id*/, std::string& err) { err = "no zero copy on this engine"; return false; }
    virtual bool RealInto(bool /*current*/, int /*id*/, std::string& err) { err = "no zero copy on this engine"; return false; }
    virtual EngineStats Stats() const = 0;
    // > 0: the multiplier this engine was opened for (it cannot change per stream)
    virtual int FixedK() const { return 0; }
    // Auto's multiplier for this engine (display refresh / source fps within the measured budget)
    virtual int DecideK(int smoother, double srcFps, double displayHz, std::string& why) = 0;
};

// Linux with NVIDIA: the nvl engine (kodi_nvl.cpp), or null with the reason
std::unique_ptr<IEngine> OpenNvl(const EngineCfg& cfg, std::string& why);
// the best engine for cfg, or the CPU engine with the GPU's reason in why; null only when even the CPU one fails
std::unique_ptr<IEngine> OpenEngine(const EngineCfg& cfg, std::string& why);
// does this build have the GPU engine
bool GpuBuilt();
// p != nullptr, compiled out of line (see CPushInputStream::DemuxSeekTime)
bool NonNull(const void* p);

} // namespace pmk
