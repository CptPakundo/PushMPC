// pushmpc.Enhance, backend "nvidia" (Linux, Windows on Arm): the full scene-cut guard on the GPU, from the very HLSL the
// Windows engine runs (shaders/VfgShotChange.hlsl + VfgFourierMellin.hlsl: PushFPS 4.5.3 + PushMPC's patch), compiled by
// Slang (`slangc -lang slang -target cuda`) and nvcc at build time. Every rule, threshold and state of Windows' guard
// carries over (adaptive, dark, spike, held pairs, bars tracked per frame and the bar-change cut, the scene score with
// the zoom fit, block matching, Fourier-Mellin; NVOF's flow when a flow source is attached), so calibration done on
// Windows holds here.
//
// Per pair: Fourier-Mellin (19 small launches, when the pair is compared and no flow is attached), then the pair test
// (one 256-thread block) on the two enhanced frames VFG reads (pitched RGBA8 / RGB10A2, the shaders' byte-buffer layout).
// The decision stays on the GPU: the 4:2:0 output kernel reads the result words and shows the pair's first real frame A
// instead of a generated frame (Windows: predicated copies). Nothing is read back except the cut count, a few frames late.
//
// The host side mirrors PushFPS's PushMpcCutGuard::record / recordFourierMellin: the 50 constants in cbuffer order (all
// 32-bit, so the CUDA struct Slang emits has the same layout), the bindings in declaration order (Slang's GlobalParams:
// the constant buffer pointer, then one {pointer, size} per byte-address buffer); the sizes are checked against the module
// at load.
#pragma once
#include <cstdint>
#include <string>
#include "nvl_cuda.h"

namespace pmvs::nvl {

// the pair's optical flow for GpuGuard::Test (outside the class: GCC wants nested default initialisers complete before a
// default argument uses the type)
struct GuardFlow { uint32_t flags = 0; CUdeviceptr forward = 0, cost = 0, backward = 0; uint32_t flowPitch = 0, costPitch = 0, gridW = 0, gridH = 0, extentW = 0, extentH = 0; };

class GpuGuard {
public:
    struct Params {   // PushMpcCutGuard::Params defaults + SceneCadenceAnalyzer's fixed thresholds
        float cutSad = 0.3f, cutHistogram = 0.5f, replacementSad = 0.08f, replacementHistogram = 0.9f, flashSad = 0.5f, flashHistogram = 0.1f;
        float adaptiveMin = 0.08f, adaptiveRatio = 5.0f, adaptiveHistogram16 = 0.15f, baselineAttack = 0.5f, baselineRelease = 0.1f;
        float darkMean = 0.25f, darkMin = 0.7f, darkRatio = 2.5f, darkHistogram = 0.32f;
        float warpVeto = 0.5f, warpMin = 0.08f, warpRatio = 5.0f, warpHistogram = 0.15f, holdSad = 0.004f, barLuma = 0.10f;
        uint32_t barTaps = 3, features = 493;
        float areaX0 = 0, areaY0 = 0, areaX1 = 0, areaY1 = 0;
        float spikeRatio = 3.0f, spikeMin = 0.09f, spikeHistogram = 0.27f, spikeHistogramRatio = 4.0f;
        float mcVeto = 0.5f, mcMin = 0.06f, mcRatio = 4.0f, barChangeBins = 2.0f, barPicture = 0.12f;
        uint32_t barPersist = 6;
        float scoreThreshold = 6.6f, scoreThresholdFlow = 6.3f;
    };
    static constexpr uint64_t kResultBytes = 1472, kSoftOffset = 1440;
    static constexpr unsigned kConstants = 50, kFmConstants = 12;
    // the built kernels (nvl_guard_fatbin.cpp), empty when this build has none (no slangc at build time)
    static bool Built();

    bool Open(CudaApi& cu, CUstream stream, uint32_t width, uint32_t height, bool tenBit, std::string& err);
    void Close();
    bool Ready() const { return m_ready; }
    // the pair test of (previous, current) (pitch = both frames' row pitch); parity = the current frame's (0/1);
    // compare = false after a reset (no valid previous: never a cut, the state's references are cleared)
    // the pair's optical flow (NVIDIA hardware, at the source size): vectors from the current frame to the previous one
    // ("forward" in the shader's words), its 8-bit cost, the other direction; flags as the shader's flowFlags (0 = none)
    using Flow = GuardFlow;
    bool Test(CUdeviceptr previous, CUdeviceptr current, size_t pitch, unsigned parity, bool compare, std::string& err, const Flow& flow = Flow());
    // device words the output kernel reads (non-zero = hold): hard cut of the pair whose current frame has `parity`,
    // and "hard or soft" (the frames up to the pair's midpoint)
    CUdeviceptr HardFlag(unsigned parity) const { return m_results + (parity & 1u) * 8; }
    CUdeviceptr AnyFlag(unsigned parity) const { return m_results + kSoftOffset + (parity & 1u) * 16; }
    // "soft only" (after the midpoint a soft cut shows the current real frame B)
    CUdeviceptr SoftOnlyFlag(unsigned parity) const { return m_results + kSoftOffset + (parity & 1u) * 16 + 8; }
    // cuts held so far (the count word, copied back a few pairs late)
    uint64_t Cuts() const;
    Params params;

private:
    bool Cu(CUresult r, const char* what, std::string& err);
    CudaApi* m_cu = nullptr;
    CUstream m_stream = nullptr;
    CUmodule m_sc = nullptr, m_fm = nullptr;
    CUfunction m_scMain = nullptr, m_fmMain = nullptr;
    CUdeviceptr m_scGlobals = 0, m_fmGlobals = 0, m_scConst = 0, m_fmConst = 0, m_results = 0, m_scratch = 0;
    size_t m_scGlobalsSize = 0, m_fmGlobalsSize = 0;
    void* m_count = nullptr;   // pinned: the cut count
    uint32_t m_w = 0, m_h = 0;
    bool m_tenBit = false, m_ready = false;
};

} // namespace pmvs::nvl
