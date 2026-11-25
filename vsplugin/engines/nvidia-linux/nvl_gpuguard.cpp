// The GPU scene-cut guard (nvl_gpuguard.h). Host side of PushFPS's PushMpcCutGuard (GPL-3.0, PushMPC's patch) for the
// Slang-compiled CUDA kernels.
#include "nvl_gpuguard.h"
#include <cstring>

#ifdef PMVS_NVL_GPU_GUARD
extern "C" const unsigned char nvl_guard_sc_fatbin[];
extern "C" const unsigned char nvl_guard_fm_fatbin[];
#endif

namespace pmvs::nvl {

namespace {
// Slang's CUDA prelude: ByteAddressBuffer / RWByteAddressBuffer = { uint32_t* data; size_t sizeInBytes; }
struct ByteBuf { uint64_t data = 0; uint64_t bytes = 0; };
struct ScGlobals { uint64_t constants; ByteBuf previous, current, flags, flowForward, flowCost, flowBackward; };
struct FmGlobals { uint64_t constants; ByteBuf previous, current, scratch, results; };
static_assert(sizeof(ByteBuf) == 16 && sizeof(ScGlobals) == 8 + 6 * 16 && sizeof(FmGlobals) == 8 + 4 * 16, "Slang binding layout");
constexpr uint64_t kSlab = 64 * 64 * 8, kScratch = 7 * kSlab;
uint32_t F(float v) { uint32_t u; std::memcpy(&u, &v, 4); return u; }
}

bool GpuGuard::Built()
{
#ifdef PMVS_NVL_GPU_GUARD
    return true;
#else
    return false;
#endif
}

bool GpuGuard::Cu(CUresult r, const char* what, std::string& err)
{
    if (r == CUDA_SUCCESS) return true;
    err = std::string("cut guard: ") + what + ": CUDA " + m_cu->Error(r);
    return false;
}

bool GpuGuard::Open(CudaApi& cu, CUstream stream, uint32_t width, uint32_t height, bool tenBit, std::string& err)
{
    Close();
    m_cu = &cu; m_stream = stream; m_w = width; m_h = height; m_tenBit = tenBit;
#ifndef PMVS_NVL_GPU_GUARD
    err = "this build has no GPU cut guard (no slangc at build time): the CPU guard runs";
    return false;
#else
    if (!Cu(cu.cuModuleLoadData(&m_sc, nvl_guard_sc_fatbin), "load VfgShotChange", err) || !Cu(cu.cuModuleLoadData(&m_fm, nvl_guard_fm_fatbin), "load VfgFourierMellin", err)
        || !Cu(cu.cuModuleGetFunction(&m_scMain, m_sc, "main_0"), "VfgShotChange main", err) || !Cu(cu.cuModuleGetFunction(&m_fmMain, m_fm, "main_0"), "VfgFourierMellin main", err)
        || !Cu(cu.cuModuleGetGlobal(&m_scGlobals, &m_scGlobalsSize, m_sc, "SLANG_globalParams"), "VfgShotChange globals", err)
        || !Cu(cu.cuModuleGetGlobal(&m_fmGlobals, &m_fmGlobalsSize, m_fm, "SLANG_globalParams"), "VfgFourierMellin globals", err)) { Close(); return false; }
    if (m_scGlobalsSize != sizeof(ScGlobals) || m_fmGlobalsSize != sizeof(FmGlobals)) {
        err = "cut guard: Slang's binding layout is not the expected one (" + std::to_string(m_scGlobalsSize) + " / " + std::to_string(m_fmGlobalsSize) + " bytes)";
        Close(); return false;
    }
    if (!Cu(cu.cuMemAlloc(&m_scConst, kConstants * 4), "constants", err) || !Cu(cu.cuMemAlloc(&m_fmConst, kFmConstants * 4), "constants", err)
        || !Cu(cu.cuMemAlloc(&m_results, kResultBytes), "results", err) || !Cu(cu.cuMemAlloc(&m_scratch, kScratch), "scratch", err)
        || !Cu(cu.cuMemsetD8Async(m_results, 0, kResultBytes, stream), "clear results", err)
        || !Cu(cu.cuMemHostAlloc(&m_count, 8, CU_MEMHOSTALLOC_PORTABLE), "count", err)) { Close(); return false; }
    std::memset(m_count, 0, 8);
    m_ready = true;
    return true;
#endif
}

void GpuGuard::Close()
{
    if (m_cu) {
        for (CUdeviceptr* p : { &m_scConst, &m_fmConst, &m_results, &m_scratch }) if (*p) { m_cu->cuMemFree(*p); *p = 0; }
        if (m_count) { m_cu->cuMemFreeHost(m_count); m_count = nullptr; }
        if (m_sc) { m_cu->cuModuleUnload(m_sc); m_sc = nullptr; }
        if (m_fm) { m_cu->cuModuleUnload(m_fm); m_fm = nullptr; }
    }
    m_ready = false;
}

uint64_t GpuGuard::Cuts() const { return m_count ? *static_cast<volatile uint32_t*>(m_count) : 0; }

bool GpuGuard::Test(CUdeviceptr previous, CUdeviceptr current, size_t pitch, unsigned parity, bool compare, std::string& err)
{
    if (!m_ready) { err = "cut guard not open"; return false; }
    const uint64_t frameBytes = uint64_t(pitch) * m_h;
    const Params& p = params;
    // Fourier-Mellin only when compared and no flow is attached (as PushMpcCutGuard runs it); its results land in the
    // result buffer for the pair test that follows
    if ((p.features & 256u) && compare) {
        FmGlobals g{ m_fmConst, { previous, frameBytes }, { current, frameBytes }, { m_scratch, kScratch }, { m_results, kResultBytes } };
        if (!Cu(m_cu->cuMemcpyHtoDAsync(m_fmGlobals, &g, sizeof(g), m_stream), "FM bindings", err)) return false;
        uint32_t area0 = 0, area1 = 0;
        if (p.areaX1 > p.areaX0 && p.areaY1 > p.areaY0) {
            auto c01 = [](float v) { return v < 0 ? 0.f : v > 1 ? 1.f : v; };
            area0 = uint32_t(c01(p.areaX0) * m_w) | (uint32_t(c01(p.areaX1) * m_w) << 16);
            area1 = uint32_t(c01(p.areaY0) * m_h) | (uint32_t(c01(p.areaY1) * m_h) << 16);
        }
        auto run = [&](uint32_t mode, uint32_t groups, uint32_t slabA = 0, uint32_t slabB = 0, uint32_t slabOut = 0, uint32_t inverse = 0, uint32_t which = 0) {
            const uint32_t k[kFmConstants] = { mode, m_w, m_h, uint32_t(pitch), m_tenBit ? 1u : 0u, slabA, slabB, slabOut, inverse, which, area0, area1 };
            return Cu(m_cu->cuMemcpyHtoDAsync(m_fmConst, k, sizeof(k), m_stream), "FM constants", err)
                && Cu(m_cu->cuLaunchKernel(m_fmMain, groups, 1, 1, 64, 1, 1, 0, m_stream, nullptr, nullptr), "FM launch", err);
        };
        auto fft = [&](uint32_t slab, uint32_t count, uint32_t inverse) { return run(1, 64 * count, slab, 0, 0, inverse) && run(2, 64 * count, slab, 0, 0, inverse); };
        // PushMpcCutGuard::recordFourierMellin's sequence
        if (!(run(0, 128) && fft(0, 2, 0) && run(3, 128) && fft(2, 2, 0) && run(4, 64, 2, 3, 4) && fft(4, 1, 1) && run(5, 1, 4, 0, 0, 0, 0)
              && run(6, 64) && fft(5, 1, 0) && run(4, 64, 5, 1, 6) && fft(6, 1, 1) && run(5, 1, 6, 0, 0, 0, 1))) return false;
    }
    // the pair test: PushMpcCutGuard::record's 50 constants
    uint32_t c[kConstants] = {};
    c[0] = m_w; c[1] = m_h; c[2] = uint32_t(pitch); c[3] = m_tenBit ? 1u : 0u; c[4] = parity & 1u; c[5] = compare ? 1u : 0u;
    const float f[] = { p.cutSad, p.cutHistogram, p.replacementSad, p.replacementHistogram, p.flashSad, p.flashHistogram, p.adaptiveMin, p.adaptiveRatio,
                        p.adaptiveHistogram16, p.baselineAttack, p.baselineRelease, p.darkMean, p.darkMin, p.darkRatio, p.darkHistogram, p.warpVeto,
                        p.warpMin, p.warpRatio, p.warpHistogram, p.holdSad, p.barLuma };
    for (size_t i = 0; i < sizeof(f) / sizeof(f[0]); ++i) c[6 + i] = F(f[i]);   // 6..26
    c[27] = p.barTaps; c[28] = p.features;
    c[29] = F(p.areaX0); c[30] = F(p.areaY0); c[31] = F(p.areaX1); c[32] = F(p.areaY1);
    c[33] = F(p.spikeRatio); c[34] = F(p.spikeMin); c[35] = F(p.spikeHistogram); c[36] = F(p.spikeHistogramRatio);
    c[37] = F(p.mcVeto); c[38] = F(p.mcMin); c[39] = F(p.mcRatio); c[40] = F(p.barChangeBins); c[41] = F(p.barPicture);
    c[42] = p.barPersist; c[43] = F(p.scoreThreshold);
    c[44] = 0; c[45] = 0; c[46] = 0; c[47] = 0; c[48] = 0;   // no flow attached (bit 128 inactive)
    c[49] = F(p.scoreThresholdFlow);
    // unused flow bindings point at the previous frame, as on Windows
    ScGlobals g{ m_scConst, { previous, frameBytes }, { current, frameBytes }, { m_results, kResultBytes }, { previous, frameBytes }, { previous, frameBytes }, { previous, frameBytes } };
    return Cu(m_cu->cuMemcpyHtoDAsync(m_scConst, c, sizeof(c), m_stream), "constants", err)
        && Cu(m_cu->cuMemcpyHtoDAsync(m_scGlobals, &g, sizeof(g), m_stream), "bindings", err)
        && Cu(m_cu->cuLaunchKernel(m_scMain, 1, 1, 1, 256, 1, 1, 0, m_stream, nullptr, nullptr), "pair test", err)
        && Cu(m_cu->cuMemcpyDtoHAsync(m_count, m_results + 48, 4, m_stream), "cut count", err);
}

} // namespace pmvs::nvl
