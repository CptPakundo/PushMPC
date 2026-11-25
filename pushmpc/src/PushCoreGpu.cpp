// pushcore.dll: GPU-in, GPU-out PushFPS enhancement graph behind the C ABI in pushcore_abi.h.
//
// Ingress (design 2.2): the caller's D3D11 NV12/P010 slice is copied into a ring of 8 shared
// NT-handle textures on the D3D11 immediate context, a shared D3D11 fence is signalled, and the
// frame enters EnhanceGraph::process through HardwareSurfaceInput, exactly as PushFPS's own
// FFmpegVideoDecoder.cpp (D3D11VA path) does. Unlike PushFPS, a slot is reused only after a
// D3D12 -> D3D11 fence says the graph has consumed it (PushFPS relies on ring depth).
//
// Processing: the graph is described as PushFPS's player describes it for a file
// (EngineController.cpp initializePreview): exportJob=false, vfgAsyncSubmission for VFG,
// nrReusePreview, DIRECT queue, 16 command slots, an NgxCoreCache.
//
// Egress (design 2.3): option A shares the graph's own output pool (EnhanceGraphDesc::
// shareableOutputs, a small guarded PushFPS patch) and opens each texture in D3D11 once. The
// fallback copies each output on the graph queue into a dedicated shared ring. Either way the
// D3D11 consumer waits on a shared D3D12 fence on the GPU, signals a shared D3D11 fence after its
// draw, and the graph waits on that before reusing the slot (presentationSubmitted).
//
// PushMPC additions in this file: the deband pass in the ingress (PushCoreDeband.h: our own D3D11 compute pass on 8-bit
// SDR sources, before the graph), the RTX VSR model family (PushCoreSettings.srFamily), the frame generation method
// (VFG, DLSS-G, PushFlow, FSR FG, AMD FRC) with a fallback chain, the upscaler (RTX VSR, FSR, NIS), the scene-cut guard
// for every method (PushMpcCutGuard in the patched PushFPS tree), non-NVIDIA GPUs, pushcore_capabilities and
// pushcore_pushFlowBench (PushFPS's PushFlowBench, ported).
#define PUSHCORE_BUILD 1
#include "pushmpc/pushcore_abi.h"
#include "PushCoreDeband.h"
#include "pushmpc/unlocks.h"            // T5b: off switches and self-test results of the vendor unlocks
#include "veyra/pipeline/VfxSrBackend.h"     // runtimeAvailable: the VFX VideoSuperRes files (high bitrate / streaming)
#include "veyra/pipeline/VfgBackend.h"
#include "veyra/pipeline/Rdna3NrBackend.h"
#include "veyra/pipeline/AmdNrProvider.h"
#include "veyra/pipeline/AmdHipPreflight.h"
#include "veyra/pipeline/GpuPassUtils.h"
#include "veyra/pushflow/PushFlowBackend.h"
#include "veyra/gfx/AmfFrcBackend.h"
#include "veyra/ngx/DlssgLite.h"
#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <cwctype>

#include "veyra/engine/EngineController.h"   // PlayerOptions
#include "veyra/engine/GraphDescription.h"   // StageRequest, describeStages
#include "veyra/engine/EffectChain.h"
#include "veyra/engine/PresetLibrary.h"      // ChainSessionStore
#include "veyra/engine/HdrOutputTuning.h"    // HDR metadata hint (VideoPresenter::applyHdrMetadata)
#include "veyra/pipeline/FrameBatch.h"
#include "veyra/pipeline/EnhanceGraph.h"
#include "veyra/pipeline/FramePacket.h"
#include "veyra/gfx/D3D12DeviceContext.h"
#include "veyra/gfx/CommandSlotRing.h"
#include "veyra/gfx/GpuSchedulingPriority.h"
#include "veyra/ngx/NgxCoreCache.h"
#include "veyra/diagnostics/FrameMetrics.h"
#include "veyra/RuntimePaths.h"
#include "veyra/Log.h"

#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

using Microsoft::WRL::ComPtr;

namespace {
using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }
std::string narrow(const std::wstring& s) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string r(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), r.data(), n, nullptr, nullptr);
    return r;
}
std::filesystem::path moduleDirectory() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&pushcore_abiVersion), &self);
    wchar_t path[32768]{};
    const DWORD n = GetModuleFileNameW(self, path, DWORD(std::size(path)));
    if (!n || n >= std::size(path)) return {};
    return std::filesystem::path(path).parent_path();
}
void noFree(void*, uint8_t*) {}
uint8_t gDummy = 0;

// Warm state shared by the instances of one process (MPC-HC opens one file after another, each with a new
// renderer and D3D11 device, but D3D12 devices are per-adapter singletons): the NGX core of the last graph
// (NGX Init is ~2.6 s of a 3.3 s open) stays alive with a reference on its D3D12 device, and the next graph on
// the same device borrows it (NgxCoreCache's own rules: same key, no live parameters, device not removed).
// One instance at a time may use it; a second concurrent instance gets a private cache. Graph build and
// teardown are serialised process-wide because NgxCoreCache is single-threaded by design.
std::mutex gGraphLock;
veyra::ngx::NgxCoreCache* gSharedCache = nullptr;   // never destroyed: no NGX shutdown during process exit
const void* gSharedCacheUser = nullptr;
Microsoft::WRL::ComPtr<ID3D12Device> gSharedDevice;  // keeps the singleton device (and the cached core's key) valid

veyra::pipeline::ColorDescription describeColor(const PushCoreStreamInfo& s) {
    using namespace veyra::pipeline;
    ColorDescription cd;   // as MediaFileSource::parseColor fills it for a file
    cd.pixelFormat = s.format == PUSHCORE_INPUT_P010 ? SourcePixelFormat::P010 : SourcePixelFormat::NV12;
    cd.range = s.range == AVCOL_RANGE_MPEG ? ColorRange::Limited : s.range == AVCOL_RANGE_JPEG ? ColorRange::Full : ColorRange::Unknown;
    switch (s.matrix) {
    case AVCOL_SPC_BT470BG: case AVCOL_SPC_SMPTE170M: cd.matrix = YuvMatrix::BT601; break;
    case AVCOL_SPC_BT709: cd.matrix = YuvMatrix::BT709; break;
    case AVCOL_SPC_BT2020_NCL: cd.matrix = YuvMatrix::BT2020NCL; break;
    case AVCOL_SPC_BT2020_CL: cd.matrix = YuvMatrix::BT2020CL; break;
    default: cd.matrix = YuvMatrix::Unknown; break;
    }
    switch (s.transfer) {
    case AVCOL_TRC_BT709: cd.transfer = TransferFunction::BT709; break;
    case AVCOL_TRC_IEC61966_2_1: cd.transfer = TransferFunction::SRGB; break;
    case AVCOL_TRC_BT2020_10: cd.transfer = TransferFunction::BT2020_10; break;
    case AVCOL_TRC_LINEAR: cd.transfer = TransferFunction::Linear; break;
    case AVCOL_TRC_SMPTE2084: cd.transfer = TransferFunction::PQ; break;      // resolveFrameColor sets these from the
    case AVCOL_TRC_ARIB_STD_B67: cd.transfer = TransferFunction::HLG; break;  // AVFrame too; explicit here for logs
    default: cd.transfer = TransferFunction::Unknown; break;
    }
    switch (s.primaries) {
    case AVCOL_PRI_BT470M: case AVCOL_PRI_BT470BG: cd.primaries = ColorPrimaries::BT601_625; break;
    case AVCOL_PRI_SMPTE170M: cd.primaries = ColorPrimaries::BT601_525; break;
    case AVCOL_PRI_BT709: cd.primaries = ColorPrimaries::BT709; break;
    case AVCOL_PRI_BT2020: cd.primaries = ColorPrimaries::BT2020; break;
    default: cd.primaries = ColorPrimaries::Unknown; break;
    }
    if (cd.primaries == ColorPrimaries::Unknown) {
        cd.primaries = cd.matrix == YuvMatrix::BT2020NCL || cd.matrix == YuvMatrix::BT2020CL ? ColorPrimaries::BT2020 : ColorPrimaries::BT709;
        cd.primariesAssumed = true;
    }
    cd.preserveSdrCodeValues = true;  // desktop SDR playback, as PushFPS plays a file
    // static HDR metadata as MediaFileSource::parseColor takes it from the container (0 = absent)
    auto nits = [](float v) { return std::isfinite(v) && v > 0 && v <= 10000 ? v : 0.0f; };
    cd.hdrMasteringPeakNits = nits(s.masteringPeakNits);
    cd.hdrMaxCllNits = nits(s.maxCllNits);
    cd.hdrMaxFallNits = nits(s.maxFallNits);
    if (cd.hdrMaxFallNits > cd.hdrMaxCllNits) cd.hdrMaxFallNits = 0;   // resolveFrameColor's validity rule
    return cd;
}

// PushCoreSettings of any size since the first ABI 3 layout: fields beyond structSize get their defaults.
static_assert(offsetof(PushCoreSettings, srFamily) == PUSHCORE_SETTINGS_SIZE_BASE, "PushCoreSettings base layout");
static_assert(offsetof(PushCoreSettings, fgBackend) == PUSHCORE_SETTINGS_SIZE_V2, "PushCoreSettings V2 layout");
static_assert(offsetof(PushCoreStats, debandMs) == PUSHCORE_STATS_SIZE_M2C, "PushCoreStats M2c layout");
bool copySettings(const PushCoreSettings* in, PushCoreSettings& out) {
    if (!in || in->structSize < PUSHCORE_SETTINGS_SIZE_BASE) return false;
    PushCoreSettings s{};
    s.srFamily = PUSHCORE_SR_FAMILY_STANDARD; s.deband = 0; s.debandStrength = PUSHCORE_DEBAND_MEDIUM;
    s.fgBackend = PUSHCORE_FG_VFG; s.fgTier = 1; s.srMethod = PUSHCORE_SR_METHOD_RTX; s.cutGuard = 1; s.fgLite = 0;
    std::memcpy(&s, in, std::min<size_t>(in->structSize, sizeof(PushCoreSettings)));
    s.structSize = sizeof(PushCoreSettings);
    out = s;
    return true;
}

// the cut guard's flow extent when only the guard takes the flow (PUSHMPC_CUT_FLOW_WIDTH overrides; calibration)
uint32_t guardFlowWidth() {
    if (const char* e = std::getenv("PUSHMPC_CUT_FLOW_WIDTH")) { const int w = std::atoi(e); if (w >= 160 && w <= 8192) return uint32_t(w); }
    return 960;
}

const char* fgName(uint32_t b) {
    switch (b) {
    case PUSHCORE_FG_VFG: return "VFG";
    case PUSHCORE_FG_DLSSG: return "DLSS-G";
    case PUSHCORE_FG_PUSHFLOW: return "PushFlow";
    case PUSHCORE_FG_FSR: return "FSR FG";
    case PUSHCORE_FG_AMDFRC: return "AMD FRC";
    default: return "none";
    }
}

// ---- GPU family and capabilities (pushcore_capabilities, the open's normalisation) -------------------------------
bool contains(const std::wstring& s, const wchar_t* what) {
    std::wstring a = s, b = what;
    for (auto& c : a) c = wchar_t(towlower(c));
    for (auto& c : b) c = wchar_t(towlower(c));
    return a.find(b) != std::wstring::npos;
}
uint32_t familyOf(uint32_t vendor, const std::wstring& name) {
    if (vendor == 0x10DE) {
        if (contains(name, L"RTX 50") || contains(name, L"Blackwell") || contains(name, L"RTX PRO")) return PUSHCORE_FAMILY_RTX50;
        if (contains(name, L"RTX 40") || contains(name, L"Ada")) return PUSHCORE_FAMILY_RTX40;
        if (contains(name, L"RTX 30") || contains(name, L"RTX A")) return PUSHCORE_FAMILY_RTX30;
        if (contains(name, L"RTX 20") || contains(name, L"TITAN RTX") || contains(name, L"Quadro RTX")) return PUSHCORE_FAMILY_RTX20;
        if (contains(name, L"RTX")) return PUSHCORE_FAMILY_RTX40;   // an unknown RTX name: a newer card
        return PUSHCORE_FAMILY_GTX;
    }
    if (vendor == 0x1002) {
        // "RX 9070", "RX 7900", "RX 6800", "RX 5700": the first digit of the model number
        for (size_t i = 0; i + 3 < name.size(); ++i) {
            if (towlower(name[i]) != L'r' || towlower(name[i + 1]) != L'x') continue;
            size_t n = i + 2;
            while (n < name.size() && (name[n] == L' ' || name[n] == L'\t')) ++n;
            if (n >= name.size() || !iswdigit(name[n])) continue;
            switch (name[n]) {
            case L'9': return PUSHCORE_FAMILY_RDNA4;
            case L'7': return PUSHCORE_FAMILY_RDNA3;
            case L'6': return PUSHCORE_FAMILY_RDNA2;
            case L'5': return PUSHCORE_FAMILY_RDNA1;
            default: break;
            }
        }
        return PUSHCORE_FAMILY_AMD_OTHER;
    }
    if (vendor == 0x8086) return PUSHCORE_FAMILY_INTEL;
    return PUSHCORE_FAMILY_OTHER;
}
bool rtxFamily(uint32_t f) { return f >= PUSHCORE_FAMILY_RTX50 && f <= PUSHCORE_FAMILY_RTX20; }
bool fileExists(const std::filesystem::path& p) { std::error_code ec; return std::filesystem::is_regular_file(p, ec); }
std::filesystem::path system32() { wchar_t s[MAX_PATH]{}; GetSystemDirectoryW(s, MAX_PATH); return s; }

// The adapter by LUID (0 = DXGI's first high-performance hardware adapter)
bool findAdapter(uint64_t luid, DXGI_ADAPTER_DESC1& out) {
    ComPtr<IDXGIFactory6> f;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&f)))) return false;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> a;
        if (f->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a)) == DXGI_ERROR_NOT_FOUND || !a) break;
        DXGI_ADAPTER_DESC1 d{};
        if (FAILED(a->GetDesc1(&d)) || (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        const uint64_t l = (uint64_t(uint32_t(d.AdapterLuid.HighPart)) << 32) | d.AdapterLuid.LowPart;
        if (luid && l != luid) continue;
        out = d;
        return true;
    }
    return false;
}

// The user-mode driver version of the adapter ("32.0.15.9140"), the key of the self-test cache (pushmpc/unlocks.h)
std::wstring driverVersionOf(const LUID& luid) {
    ComPtr<IDXGIFactory4> f;
    ComPtr<IDXGIAdapter> a;
    LARGE_INTEGER v{};
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&f))) || FAILED(f->EnumAdapterByLuid(luid, IID_PPV_ARGS(&a))) ||
        FAILED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &v))) return L"unknown";
    return std::format(L"{}.{}.{}.{}", HIWORD(v.HighPart), LOWORD(v.HighPart), HIWORD(v.LowPart), LOWORD(v.LowPart));
}
// T5b unlock: RTX VSR on GTX 16 (TU116 / TU117, compute capability 7.5: nvngx_vsr ships complete sm_75 kernels; the only
// gate is NGX's feature table). Eligible when the files are there; on only after its self-test passed on this adapter
// and driver (tests\gtx16-vsr-manual.ps1 runs pushmpc\tools\vsr_gate_probe and records the result), or forced on.
// Off switch PUSHMPC_DISABLE_GTX16VSR=1 / Unlocks\Gtx16Vsr = 0. The decision goes to the log and to reason[RTX_VSR].
pmu::UnlockDecision gtx16VsrDecision(const DXGI_ADAPTER_DESC1& d, bool files) {
    std::wstring src;
    const pmu::UnlockMode mode = pmu::ReadUnlockMode(pmu::Unlock::Gtx16Vsr, &src);
    const pmu::SelfTest t = pmu::ReadSelfTest(pmu::Unlock::Gtx16Vsr, pmu::SelfTestKey(d.VendorId, d.DeviceId, driverVersionOf(d.AdapterLuid)));
    return pmu::DecideUnlock(pmu::Unlock::Gtx16Vsr, mode, files, L"nvngx_vsr.dll is not installed", t, false, src);
}

void fillCaps(const DXGI_ADAPTER_DESC1& d, const std::filesystem::path& runtimeDir, PushCoreCaps& c) {
    const std::wstring name = d.Description;
    const uint32_t v = d.VendorId, fam = familyOf(v, name);
    c.vendorId = v; c.deviceId = d.DeviceId; c.family = fam;
    c.adapterLuid = (uint64_t(uint32_t(d.AdapterLuid.HighPart)) << 32) | d.AdapterLuid.LowPart;
    c.dedicatedVideoMemory = d.DedicatedVideoMemory;
    { const std::string n = narrow(name); std::snprintf(c.adapter, sizeof(c.adapter), "%s", n.c_str()); }
    const std::filesystem::path data = veyra::runtime::localDataDirectory(), fsr = data / L"amd" / L"fidelityfx";
    const bool fsrLoader = fileExists(fsr / L"amd_fidelityfx_loader_dx12.dll");
    const bool nvidia = v == 0x10DE, rtx = rtxFamily(fam), amd = v == 0x1002;
    const bool vfgHw = nvidia && (fam == PUSHCORE_FAMILY_RTX50 || fam == PUSHCORE_FAMILY_RTX40 || fam == PUSHCORE_FAMILY_RTX30);
    auto set = [&](uint32_t bit, bool hardware, bool files, const char* needHw, const char* needFiles) {
        if (hardware) c.hardware |= 1u << bit;
        if (hardware && files) { c.available |= 1u << bit; c.reason[bit][0] = 0; return; }
        std::snprintf(c.reason[bit], sizeof(c.reason[bit]), "%s", !hardware ? needHw : needFiles);
    };
    const std::wstring rt = runtimeDir.wstring();
    set(PUSHCORE_CAP_VFG, vfgHw, veyra::pipeline::VfgBackend::runtimeAvailable(rt), "needs an NVIDIA RTX 30, 40 or 50 card",
        "the VFG files are not installed (runtime\\nvidia-vfg)");
    set(PUSHCORE_CAP_DLSSG, nvidia && rtx && fam != PUSHCORE_FAMILY_RTX20, fileExists(runtimeDir / L"nvngx_dlssg.dll"),
        fam == PUSHCORE_FAMILY_RTX20 ? "RTX 20: no DLSS-G port for Turing yet" : "needs an NVIDIA RTX 30, 40 or 50 card",
        "the DLSS-G files are not installed (nvngx_dlssg.dll)");
    set(PUSHCORE_CAP_DLSSG_LITE, nvidia && fam == PUSHCORE_FAMILY_RTX50, veyra::ngx::dlssgLiteRuntimePresent(runtimeDir),
        "DLSS-G performance mode runs on RTX 50 only", "the DLSS-G Lite runtime is not installed (dlssg-lite)");
    std::wstring why;
    const bool pf = veyra::pushflow::runtimeInstalled(rt, &why);
    set(PUSHCORE_CAP_PUSHFLOW, true, pf, "", "the PushFlow files are not installed (runtime\\experimental\\pushflow)");
    set(PUSHCORE_CAP_FSR_FG, true, fsrLoader && fileExists(fsr / L"amd_fidelityfx_framegeneration_dx12.dll"), "",
        "the FidelityFX files are not installed (runtime_local\\amd\\fidelityfx)");
    set(PUSHCORE_CAP_FSR4_FG, amd && fam == PUSHCORE_FAMILY_RDNA4, fsrLoader && fileExists(fsr / L"amd_fidelityfx_framegeneration_dx12.dll"),
        "FSR 4 ML frame generation needs an AMD RX 9000", "the FidelityFX files are not installed (runtime_local\\amd\\fidelityfx)");
    set(PUSHCORE_CAP_AMDFRC, amd || fileExists(system32() / L"amfrt64.dll"), veyra::gfx::AmfFrcBackend::runtimeInstalled(),
        "needs AMD's driver (AMF runtime)", "AMD's AMF runtime is not installed");
    const bool vsrFiles = fileExists(runtimeDir / L"nvngx_vsr.dll");
    const bool gtx16 = nvidia && !rtx && pmu::IsGtx16(v, d.DeviceId, name);
    if (gtx16) {
        // hardware: the card can run it through the unlock (unless switched off); available: files + the unlock is on
        const pmu::UnlockDecision u = gtx16VsrDecision(d, vsrFiles);
        const std::string why = narrow(u.why);
        veyra::log::info("pushcore", std::format("unlock {}", why));
        const bool off = pmu::ReadUnlockMode(pmu::Unlock::Gtx16Vsr) == pmu::UnlockMode::Off;
        set(PUSHCORE_CAP_RTX_VSR, !off, vsrFiles && u.on, why.c_str(), vsrFiles ? why.c_str() : "nvngx_vsr.dll is not installed");
    } else {
        set(PUSHCORE_CAP_RTX_VSR, nvidia && rtx, vsrFiles, "needs an NVIDIA RTX card (GTX 16: unlock Gtx16Vsr after its self-test)", "nvngx_vsr.dll is not installed");
    }
    set(PUSHCORE_CAP_VSR_FAMILIES, nvidia && rtx, veyra::pipeline::VfxSrBackend::runtimeAvailable(rt), "needs an NVIDIA RTX card",
        "the Video Effects VideoSuperRes files are not installed");
    set(PUSHCORE_CAP_FSR_SR, true, fsrLoader && fileExists(fsr / L"amd_fidelityfx_upscaler_dx12.dll"), "",
        "the FidelityFX files are not installed (runtime_local\\amd\\fidelityfx)");
    const auto shaders = veyra::runtime::shaderDirectory();
    set(PUSHCORE_CAP_NIS, true, fileExists(shaders / L"NisScalerSdr.dxil") && fileExists(shaders / L"NisColor.dxil"), "", "the NIS shaders are missing");
    set(PUSHCORE_CAP_RTX_HDR, nvidia && rtx, fileExists(runtimeDir / L"nvngx_truehdr.dll"), "needs an NVIDIA RTX card", "nvngx_truehdr.dll is not installed");
    // AI clean-up: NVIDIA's original (RTX 50) or the community SF build (RTX 20-50); AMD: lmxxf (RX 9000) or the
    // RedDuke ZLUDA worker (RX 7000)
    if (nvidia) {
        const bool sf = fileExists(runtimeDir / L"nr-ampere" / L"nvngx_dlssnr.dll");
        const bool original = fileExists(runtimeDir / L"nr-original" / L"nvngx_dlssnr.dll");
        const bool hw = rtx;
        const bool files = sf || (original && fam == PUSHCORE_FAMILY_RTX50);
        set(PUSHCORE_CAP_NR, hw, files, "needs an NVIDIA RTX card", fam == PUSHCORE_FAMILY_RTX50 ? "no NR runtime installed" :
            "needs the community SF NR runtime (nr-ampere) on this card");
        std::snprintf(c.nrRuntime, sizeof(c.nrRuntime), "%s", sf ? "SF 310.9.16 (community)" : original ? "NVIDIA 310.8.0" : "");
    } else if (amd) {
        const auto provider = veyra::pipeline::amdNrProvider(v, name);
        bool files = false;
        const char* missing = "";
        if (provider == veyra::pipeline::AmdNrProvider::RedDukeRdna3) {
            files = veyra::pipeline::Rdna3NrBackend::runtimeAvailable((runtimeDir / L".." / L"amd-nr-rdna3").lexically_normal()) &&
                    veyra::pipeline::Rdna3NrBackend::hipRuntimeAvailable();
            missing = "the RX 7000 NR worker (amd-nr-rdna3) or AMD's HIP 7 runtime is missing";
            std::snprintf(c.nrRuntime, sizeof(c.nrRuntime), "RedDuke ZLUDA worker (RX 7000)");
        } else if (provider == veyra::pipeline::AmdNrProvider::LmxxfRdna4) {
            files = fileExists(runtimeDir / L".." / L"amd-nr" / L"LmxxfNrRuntime.dll") &&
                    (fileExists(system32() / L"amdhip64_7.dll") || GetModuleHandleW(L"amdhip64_7.dll"));
            missing = "the RX 9000 NR runtime (amd-nr) or AMD's HIP 7 runtime is missing";
            std::snprintf(c.nrRuntime, sizeof(c.nrRuntime), "lmxxf (RX 9000)");
        }
        set(PUSHCORE_CAP_NR, provider != veyra::pipeline::AmdNrProvider::Unsupported, files, "AI clean-up on AMD needs an RX 7000 or RX 9000", missing);
    } else {
        set(PUSHCORE_CAP_NR, false, false, "AI clean-up needs an NVIDIA RTX or AMD RX 7000 / 9000 card", "");
    }
}
}  // namespace

// RTX 30 VFG (VfgAmpereCompat) compiles its retargeted kernels from PTX; CUDA's default compute cache cannot hold them,
// so every session would recompile and stall (PushFPS sets the same in its main()). pushcore runs inside MPC-HC, so it
// sets it for the process when it is loaded, before anything here initialises CUDA, and never replaces a value the
// user set. UNTESTED on an RTX 30 (no card here).
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        wchar_t value[64]{};
        if (GetEnvironmentVariableW(L"CUDA_CACHE_MAXSIZE", value, 64) == 0) {
            SetEnvironmentVariableW(L"CUDA_CACHE_MAXSIZE", L"4294967296");
            _wputenv_s(L"CUDA_CACHE_MAXSIZE", L"4294967296");   // this CRT's copy too
        }
    }
    return TRUE;
}

struct PushCore {
    // ---- configuration ----
    PushCoreStreamInfo stream{};
    std::wstring dataDir, runtimeDir;
    int cfgSr = -1, cfgFg = -1, cfgMultiplier = -1, cfgVfgQuality = -1;
    uint32_t flags = 0;
    // ABI 3: PushMPC's own settings (absent = the PushFPS chain session in dataDir)
    bool haveSettings = false;
    PushCoreSettings settings{};
    // PushMPC 0.6: the frame generation method that is tried now (the fallback chain moves it), what runs, why
    uint32_t tryFg = PUSHCORE_FG_VFG, tryMultiplierCap = 8;
    uint32_t runFg = PUSHCORE_FG_NONE, runSr = PUSHCORE_SR_METHOD_RTX;
    bool fgFellBack = false, cutGuardOn = false;
    uint64_t cutBase = 0;                    // cuts of graphs torn down (the guard's count restarts with each graph)
    std::atomic<uint64_t> cutsSeen{0};       // cuts the guard held so far (PushCoreStats.cutGuardCuts)
    std::atomic<uint32_t> subtitleState{0}, subtitlePixels{0};   // PushFPS Subtitle Hold (PushCoreStats), read on the submit thread
    uint32_t subtitlePoll = 0;
    // Subtitle Hold Auto (settings.subtitleHold == 2): the pass detects on every frame, composes only while burned-in
    // text is found (gate below); counters of consecutive polls above / below the thresholds
    bool subAutoInit = false, subCompose = false;
    uint32_t subOnRun = 0, subOffRun = 0;
    std::string fgNote;
    uint32_t family = PUSHCORE_FAMILY_UNKNOWN;

    // ---- D3D11 side (borrowed device, our objects) ----
    ComPtr<ID3D11Device> d11;
    ComPtr<ID3D11Device1> d11_1;
    ComPtr<ID3D11Device5> d11_5;
    ComPtr<ID3D11DeviceContext> imm;
    ComPtr<ID3D11DeviceContext4> imm4;
    ComPtr<ID3D11Fence> fence11;          // D3D11 signals (ingress copy done, presentation done)
    ComPtr<ID3D11Fence> fence12in11;      // D3D12 signals (ingress consumed, outputs ready)
    uint64_t value11 = 0;                 // last value signalled on fence11
    ID3D11DeviceContext* lastPresentCtx = nullptr;
    ComPtr<ID3D11DeviceContext4> lastPresentCtx4;

    // ---- D3D12 side ----
    veyra::gfx::D3D12DeviceContext ctx;
    veyra::gfx::CommandSlotRing ring;
    veyra::ngx::NgxCoreCache ownCache;     // used when the shared cache is taken (or warm cache is off)
    veyra::ngx::NgxCoreCache* cache = nullptr;
    bool sharedCache = false;
    bool deviceLost = false;
    std::unique_ptr<veyra::pipeline::EnhanceGraph> graph;
    veyra::pipeline::EnhanceGraphDesc gd;
    veyra::engine::PlayerOptions options;
    ComPtr<ID3D12Fence> fence11in12;      // fence11 opened in D3D12
    ComPtr<ID3D12Fence> fence12;          // D3D12 signals on the graph queue
    uint64_t value12 = 0;
    HANDLE waitEvent = nullptr;
    uint32_t multiplier = 1;
    bool fg = false;
    bool hdrIn = false, hdrOut = false;
    bool hdrHint = false, hdrCurveOn = false;
    DXGI_HDR_METADATA_HDR10 hdrMeta{};

    // ---- FG admission (PushFPS EngineController admitFg for a file, simplified: see admit()) ----
    struct Inflight { double finishMs; uint64_t fenceValue; };
    std::deque<Inflight> inflight;          // predicted GPU finish of submitted source frames (host ms)
    struct CostWindow {                     // measured GPU ms of the last 20 s (older samples describe another load)
        std::deque<std::pair<Clock::time_point, double>> v;
        void push(double ms) { if (ms > 0 && std::isfinite(ms)) { v.emplace_back(Clock::now(), ms); if (v.size() > 64) v.pop_front(); } }
        size_t keep = 0;                    // never age below this many samples (the last known cost stays usable)
        void age() { const auto cut = Clock::now() - std::chrono::seconds(20); while (v.size() > keep && v.front().first < cut) v.pop_front(); }
        void dropOlderThan(std::chrono::seconds s) { const auto cut = Clock::now() - s; while (!v.empty() && v.front().first < cut) v.pop_front(); }
        size_t size() { age(); return v.size(); }
        void clear() { v.clear(); }
        double q75() { return pct(0.75); }   // admission cost (p75; p90 kept 1.0x in 2X groups after load spikes)
        double p50() { return pct(0.5); }    // queue prediction
        std::pair<double, uint32_t> recentP50(std::chrono::milliseconds span) {   // median of the samples in the last `span`
            const auto cut = Clock::now() - span;
            std::vector<double> x;
            for (auto& e : v) if (e.first >= cut) x.push_back(e.second);
            if (x.empty()) return {-1.0, 0u};
            std::nth_element(x.begin(), x.begin() + x.size() / 2, x.end());
            return {x[x.size() / 2], uint32_t(x.size())};
        }
        double pct(double q) {
            age();
            if (v.empty()) return -1;
            std::vector<double> x; x.reserve(v.size());
            for (auto& e : v) x.push_back(e.second);
            const size_t k = std::min(x.size() - 1, size_t(std::max(1.0, std::ceil(q * x.size()))) - 1);
            std::nth_element(x.begin(), x.begin() + k, x.end());
            return x[k];
        }
    };
    CostWindow costFull, costReduced, costFirst, costFirstBatch; // graph span (full/reduced groups), FG1, FG batch
    void initCostWindows() { costFull.keep = costFirst.keep = costFirstBatch.keep = 8; costReduced.keep = 4; }
    std::map<uint64_t, int> decisionOf;     // graph source frame id -> 0 full, 1 reduced, 2 none

    // ---- ingress ring ----
    static constexpr uint32_t kIngressSlots = 8;
    struct InSlot { ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D12Resource> res; uint64_t freeValue = 0; };
    InSlot inSlots[kIngressSlots];
    uint32_t inW = 0, inH = 0; DXGI_FORMAT inFormat = DXGI_FORMAT_UNKNOWN;
    uint64_t inIndex = 0;
    // ---- deband (PushMPC's own pass on the source, PushCoreDeband.h) ----
    pushmpc::Deband deband;
    bool debandEligible = false;           // 8-bit SDR stream and the pass initialised on this device
    int debandLogged = -1;                 // last state logged (on/off/strength), to log changes only
    bool debandWanted() const { return debandEligible && haveSettings && settings.deband != 0; }
    AVFrame* frame = nullptr;
    veyra::pipeline::ColorDescription color;
    bool dvP5Logged = false;

    // ---- egress ----
    uint32_t egressMode = PUSHCORE_EGRESS_DIRECT;
    std::unordered_map<ID3D12Resource*, ComPtr<ID3D11Texture2D>> opened;  // direct: graph pool in D3D11
    struct OutSlot { ComPtr<ID3D12Resource> res; ComPtr<ID3D11Texture2D> tex; uint64_t releaseValue = 0; bool held = false; };
    std::vector<OutSlot> outSlots;        // copy mode ring
    uint32_t outNext = 0;
    static constexpr uint32_t kCopyLists = 8;
    ComPtr<ID3D12CommandAllocator> copyAlloc[kCopyLists];
    uint64_t copyAllocValue[kCopyLists]{};
    ComPtr<ID3D12GraphicsCommandList> copyList;
    uint32_t copyNext = 0;

    // ---- frames ----
    struct Batch {
        veyra::pipeline::EnhanceGraph::FrameOutputs out;
        uint64_t sourceIndex = 0;
        int64_t realPts = 0;
        uint64_t readyValue = 0;          // fence12 value covering every list of this batch
        uint32_t next = 0;
    };
    std::deque<Batch> batches;
    struct Lease { std::shared_ptr<veyra::pipeline::FrameLease> graphLease; uint32_t kind = 0, parity = 0; int outSlot = -1; };
    std::map<uint64_t, Lease> leases;
    uint64_t nextLease = 1;
    uint64_t sequence = 0, submittedCount = 0;
    bool haveReal = false, needReset = true;

    PushCoreStats st{};
    std::deque<PushCoreGpuTiming> timings;
    std::string error, description;

    // ------------------------------------------------------------------------------------------
    int fail(std::string message) {
        error = std::move(message);
        if (checkDeviceLost()) error = "device lost: " + error;
        veyra::log::error("pushcore", error);
        return deviceLost ? PUSHCORE_DEVICE_LOST : PUSHCORE_ERROR;
    }
    // TDR or driver reset: either device reports a removed reason.
    bool checkDeviceLost() {
        if (deviceLost) return true;
        HRESULT r12 = S_OK, r11 = S_OK;
        if (ctx.initialized() && ctx.device()) r12 = ctx.device()->GetDeviceRemovedReason();
        if (d11) r11 = d11->GetDeviceRemovedReason();
        if (FAILED(r12) || FAILED(r11)) {
            deviceLost = true;
            veyra::log::error("pushcore", std::format("device removed: D3D12 reason 0x{:08X}, D3D11 reason 0x{:08X}", unsigned(r12), unsigned(r11)));
        }
        return deviceLost;
    }
    bool hrFail(HRESULT hr, const char* what) {
        if (SUCCEEDED(hr)) return false;
        fail(std::format("{} failed hr=0x{:08X}", what, unsigned(hr)));
        return true;
    }

    // PushFPS's simple video page (qml/Veyra/WatchPage.qml) on a fresh install, switch by switch (see
    // settings-and-runtime.md for the evidence): the bridge starts from EnhancementSettings{} with nrRuntime Ampere, in
    // List chain mode (runtimeOrder = nullopt); "smoother" sets multiplier (the FG node on) and content Auto; "sharper"
    // sr on with the current videoSrQuality (standard RTX VSR family); "AI picture enhancement" turns the list's NR
    // node on with its default parameters (one layer: flat fields, nrLayerCount 0) and PushQuickSetup's look writes
    // model.tone/structure; "SDR to HDR" videoHdr.enabled with VideoHdrSettings' defaults. One deliberate difference:
    // the frame generation backend is NVIDIA VFG (PushFPS's fresh default on NVIDIA is DLSS-G, which PushMPC does not
    // ship; VFG is the video interpolation and goes up to 8X).
    bool buildFromPushMpc(veyra::engine::EnhancementSettings& s, std::optional<veyra::engine::ChainRuntimeOrder>& order) {
        using namespace veyra::engine;
        s = EnhancementSettings{};
        s.nrRuntime = NrRuntime::Ampere;
        const uint32_t vendor = ctx.adapter().vendorId;
        const bool nvidia = vendor == 0x10DE, amd = vendor == 0x1002;
        // frame generation method (tryFg: the asked-for one, or the next of the fallback chain)
        uint32_t cap = 8;
        switch (tryFg) {
        case PUSHCORE_FG_DLSSG:
            // DLSS-G on video: the FidelityFX optical flow (NVIDIA's hardware flow and GPU-DIS give it the same picture:
            // it estimates motion itself; FidelityFX is the cheapest), the refined motion kept; MFG up to 6x (native on
            // RTX 50, AdaMfgUnlock on RTX 40, AmpereMfgUnlock / DlssgTransfusion on RTX 30, all automatic)
            s.frameGenerationBackend = FrameGenerationBackend::Dlss;
            s.opticalFlowBackend = OpticalFlowBackend::AmdFidelityFx;
            s.fgMotionRefine = true;
            s.dlssgLite = settings.fgLite != 0 && family == PUSHCORE_FAMILY_RTX50;
            cap = 6;
            break;
        case PUSHCORE_FG_PUSHFLOW:
            s.frameGenerationBackend = FrameGenerationBackend::PushFlow;
            cap = 4;
            break;
        case PUSHCORE_FG_FSR:
            // FSR 4 ML frame generation on RX 9000 (PushFPS's default there), FSR 3.1 elsewhere; FidelityFX flow
            s.frameGenerationBackend = family == PUSHCORE_FAMILY_RDNA4 ? FrameGenerationBackend::Fsr4 : FrameGenerationBackend::Fsr;
            s.opticalFlowBackend = OpticalFlowBackend::AmdFidelityFx;
            s.fgMotionRefine = true;
            cap = 2;
            break;
        case PUSHCORE_FG_AMDFRC:
            s.frameGenerationBackend = FrameGenerationBackend::AmdFrc;
            cap = 2;
            break;
        default:
            s.frameGenerationBackend = FrameGenerationBackend::Vfg;
            break;
        }
        cap = std::min(cap, tryMultiplierCap);
        // VFG: the NvVFX model (0..2); PushFlow: its tier (EnhancementSettings keeps PushFlow's tier in vfgQuality)
        s.vfgQuality = tryFg == PUSHCORE_FG_PUSHFLOW ? std::min<uint32_t>(settings.fgTier, 2u) : std::min<uint32_t>(settings.vfgQuality, 2u);
        s.multiplier = settings.multiplier >= 2 && tryFg != PUSHCORE_FG_NONE ? std::min<uint32_t>(settings.multiplier, cap) : 1u;
        if (s.multiplier > 1) s.content = ContentRate::Auto;   // WatchPage ensureVideoCadence (ignored for files)
        s.sr = settings.sr != 0;
        if (settings.srQuality >= 1 && settings.srQuality <= 4) s.videoSrQuality = settings.srQuality;
        // the upscaler: RTX VSR, or AMD FSR / NVIDIA Image Scaling (any GPU; on a non-NVIDIA GPU RTX VSR becomes FSR)
        runSr = settings.srMethod;
        if (!nvidia && runSr == PUSHCORE_SR_METHOD_RTX) runSr = PUSHCORE_SR_METHOD_FSR;
        if (runSr == PUSHCORE_SR_METHOD_FSR) {
            s.videoSrQuality = kVideoSrFsr;   // FidelityFX picks FSR 4.1 (ML / INT8) where the driver offers it
            s.opticalFlowBackend = OpticalFlowBackend::AmdFidelityFx;   // FSR is temporal: its motion from FidelityFX flow
        } else if (runSr == PUSHCORE_SR_METHOD_NIS) {
            s.videoSrQuality = kVideoSrNis;
        }
        // RTX VSR model family (PushFPS's videoSrQualityFor: the tier keeps its meaning, the family picks the model).
        // High bitrate / streaming run on NVIDIA Video Effects (VfxSrBackend), from runtime\nvidia-vfg.
        if (runSr == PUSHCORE_SR_METHOD_RTX && (settings.srFamily == PUSHCORE_SR_FAMILY_HIGH_BITRATE || settings.srFamily == PUSHCORE_SR_FAMILY_STREAMING)) {
            if (veyra::pipeline::VfxSrBackend::runtimeAvailable(runtimeDir)) {
                const auto family = settings.srFamily == PUSHCORE_SR_FAMILY_STREAMING ? VideoSrFamily::Streaming : VideoSrFamily::HighBitrate;
                s.videoSrQuality = videoSrQualityFor(family, videoSrTier(s.videoSrQuality));
            } else {
                veyra::log::warn("pushcore", "RTX VSR style needs the Video Effects VideoSuperRes files (runtime\\nvidia-vfg): standard family kept");
            }
        }
        s.srTarget = settings.srTarget == PUSHCORE_SR_TARGET_1440P ? veyra::pipeline::SrTarget::Qhd
                   : settings.srTarget == PUSHCORE_SR_TARGET_5K ? veyra::pipeline::SrTarget::Uhd5K : veyra::pipeline::SrTarget::Uhd4K;
        s.nr = settings.nr != 0;
        // PushFPS's default NR runtime (Ampere) is the community SF build (RTX 20-50); without it NVIDIA's original NR
        // runtime (RTX 50). On AMD the AMD NR runtimes, routed by the adapter as PushFPS does: lmxxf on RX 9000, the
        // RedDuke ZLUDA worker process on RX 7000 (NrRuntime::LmxxfAmd = "AMD NR auto").
        if (amd) {
            s.nrRuntime = NrRuntime::LmxxfAmd;
        } else {
            const std::filesystem::path rt(runtimeDir);
            if (!std::filesystem::exists(rt / L"nr-ampere" / L"nvngx_dlssnr.dll") && std::filesystem::exists(rt / L"nr-original" / L"nvngx_dlssnr.dll")) {
                s.nrRuntime = NrRuntime::NvidiaOriginal;
                if (s.nr) veyra::log::info("pushcore", "NR runtime: NVIDIA's original (the community SF slot is not installed)");
            }
        }
        // What a non-NVIDIA GPU cannot run (PushFPS's disableUnsupportedNvidiaEffects): RTX Video HDR, NVIDIA NR on a
        // card without an AMD NR provider, VFG / DLSS-G (the fallback chain moves past them before this point)
        if (!nvidia) {
            if (settings.videoHdr) veyra::log::info("pushcore", "RTX Video HDR needs NVIDIA: off (PushMPC's own SDR to HDR shader runs in the filter)");
            settings.videoHdr = 0;
            if (s.nr && !amd) { s.nr = false; veyra::log::info("pushcore", "AI clean-up needs NVIDIA RTX or AMD RX 7000 / 9000: off"); }
            if (s.frameGenerationBackend == FrameGenerationBackend::Vfg || s.frameGenerationBackend == FrameGenerationBackend::Dlss) s.multiplier = 1;
            if (s.opticalFlowBackend == OpticalFlowBackend::Nvidia) s.opticalFlowBackend = OpticalFlowBackend::AmdFidelityFx;
        }
        if (s.nr) {
            auto unit = [](float v, float d) { return std::isfinite(v) && v >= 0 && v <= 1 ? v : d; };
            s.model.tone = unit(settings.nrTone, 1.0f);
            s.model.structure = unit(settings.nrStructure, 1.0f);
        }
        s.videoHdr.enabled = settings.videoHdr != 0;
        if (settings.videoHdrPeak >= 400 && settings.videoHdrPeak <= 2000) s.videoHdr.peakNits = settings.videoHdrPeak;   // PushMPC's HDR boost
        order.reset();
        if (const auto e = s.validate(); !e.empty()) { fail("PushMPC settings invalid: " + e); return false; }
        veyra::log::info("pushcore", std::format("PushMPC settings: SR {} (videoSrQuality {}, target {}), FG {} {} (quality/tier {}), NR {} (runtime {}, tone {:.2f} structure {:.2f}), RTX Video HDR {}, cut guard {}, Subtitle Hold {}",
            s.sr, s.videoSrQuality, int(s.srTarget), fgName(tryFg), s.multiplier, s.vfgQuality, s.nr, nrRuntimeName(s.nrRuntime), s.model.tone, s.model.structure,
            s.videoHdr.enabled ? std::format("on, peak {} cd/m2", s.videoHdr.peakNits) : std::string("off"), settings.cutGuard ? "on" : "VFG only", settings.subtitleHold == 2 ? "Auto (while burned-in text is found)" : settings.subtitleHold ? "on" : "off"));
        return true;
    }

    bool loadSettings(veyra::engine::EnhancementSettings& s, std::optional<veyra::engine::ChainRuntimeOrder>& order) {
        if (haveSettings) return buildFromPushMpc(s, order);
        if (dataDir.empty()) return true;
        auto session = veyra::engine::ChainSession::initial(s);
        veyra::engine::ChainSessionStore store(std::filesystem::path(dataDir) / L"chain-session.v1");
        if (!store.load(session)) { fail("chain session not loaded: " + narrow(store.error())); return false; }
        const auto& configuration = session.configurations[size_t(session.active)];
        configuration.apply(s);
        order = veyra::engine::runtimeOrder(configuration.chain);
        return true;
    }

    // D3D12 device, fences, ingress state. Graph and egress come from buildGraph.
    int openDevices(ID3D11Device* device) {
        d11 = device;
        if (FAILED(d11.As(&d11_1)) || FAILED(d11.As(&d11_5))) return fail("D3D11 device lacks ID3D11Device5 (Windows 10 1703+ shared fences)");
        d11->GetImmediateContext(&imm);
        if (FAILED(imm.As(&imm4))) return fail("D3D11 immediate context lacks ID3D11DeviceContext4");
        ComPtr<IDXGIDevice> dxgiDevice; ComPtr<IDXGIAdapter> adapter; DXGI_ADAPTER_DESC ad{};
        if (FAILED(d11.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&ad)))
            return fail("cannot read the D3D11 device's adapter");
        const uint64_t luid = (uint64_t(uint32_t(ad.AdapterLuid.HighPart)) << 32) | ad.AdapterLuid.LowPart;
        // Any DirectX 12 GPU (PushMPC 0.6): NVIDIA RTX runs the NGX stages; AMD, Intel and GTX run FSR / NIS, PushFlow,
        // FSR FG and AMD FRC, and the AMD NR runtimes (the graph skips the NGX core for them)
        family = familyOf(ad.VendorId, ad.Description);
        veyra::log::info("pushcore", std::format("adapter vendor 0x{:04X} device 0x{:04X} family {} ({})", ad.VendorId, ad.DeviceId, family, narrow(ad.Description)));
        {
            wchar_t v[64]{};
            if (GetEnvironmentVariableW(L"CUDA_CACHE_DISABLE", v, 64) && wcscmp(v, L"0") != 0)
                veyra::log::warn("pushcore", "CUDA_CACHE_DISABLE is set (the user's own value, kept): RTX 30 VFG recompiles its kernels every session");
            if (GetModuleHandleW(L"nvcuda.dll")) veyra::log::info("pushcore", "nvcuda.dll was loaded before pushcore: CUDA_CACHE_MAXSIZE applies only if CUDA was not initialised yet");
            GetEnvironmentVariableW(L"CUDA_CACHE_MAXSIZE", v, 64);
            veyra::log::info("pushcore", "CUDA_CACHE_MAXSIZE=" + narrow(v));
        }

        veyra::Status status = veyra::Status::Ok;
        veyra::gfx::DeviceContextDesc dd;
        dd.commandSlotCount = 6;     // PreviewGpuSession::initialize
        dd.requiredLuid = luid;
        if (!ctx.initialize(dd, status)) return fail(std::format("no D3D12 device on adapter LUID {:016X}", luid));
        if (ctx.adapter().luid != luid) return fail("D3D12 device is on a different adapter");
        if (!ring.initialize(ctx.device(), ctx.directQueue(), ctx.fence(), ctx.fenceEvent(), 16, status))
            return fail("command ring did not start");
        // PushFPS playback (PreviewGpuSession::initialize) raises the process GPU scheduling class to its
        // "gpuPriority" preference, High when unset (the default). Same here, for parity under GPU contention.
        // pushcore_close puts the host process back to Normal when the last instance closes.
        veyra::gfx::requestGpuPriority(veyra::gfx::GpuPriority::High);
        {
            const auto gp = veyra::gfx::gpuPriorityStatus();
            veyra::log::info("pushcore", std::format("GPU scheduling priority class requested {} actual {} state {}", int(gp.requested), gp.actual, int(gp.state)));
        }

        // D3D11 -> D3D12 fence (PushFPS createInteropFence).
        HANDLE h = nullptr;
        if (hrFail(d11_5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence11)), "ID3D11Device5::CreateFence(shared)")) return PUSHCORE_ERROR;
        if (hrFail(fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h), "ID3D11Fence::CreateSharedHandle")) return PUSHCORE_ERROR;
        HRESULT hr = ctx.device()->OpenSharedHandle(h, IID_PPV_ARGS(&fence11in12));
        CloseHandle(h);
        if (hrFail(hr, "D3D12 OpenSharedHandle(D3D11 fence)")) return PUSHCORE_ERROR;
        // D3D12 -> D3D11 fence.
        if (hrFail(ctx.device()->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence12)), "D3D12 CreateFence(shared)")) return PUSHCORE_ERROR;
        h = nullptr;
        if (hrFail(ctx.device()->CreateSharedHandle(fence12.Get(), nullptr, GENERIC_ALL, nullptr, &h), "D3D12 CreateSharedHandle(fence)")) return PUSHCORE_ERROR;
        hr = d11_5->OpenSharedFence(h, IID_PPV_ARGS(&fence12in11));
        CloseHandle(h);
        if (hrFail(hr, "ID3D11Device5::OpenSharedFence")) return PUSHCORE_ERROR;
        waitEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        frame = av_frame_alloc();
        if (!frame) return fail("no AVFrame");
        frame->format = AV_PIX_FMT_D3D11;
        frame->width = int(stream.width); frame->height = int(stream.height);
        frame->color_range = AVColorRange(stream.range);
        frame->colorspace = AVColorSpace(stream.matrix);
        frame->color_primaries = AVColorPrimaries(stream.primaries);
        frame->color_trc = AVColorTransferCharacteristic(stream.transfer);
        // EnhanceGraph::process av_frame_clone()s hardware frames: it must be reference counted.
        frame->buf[0] = av_buffer_create(&gDummy, 1, noFree, nullptr, AV_BUFFER_FLAG_READONLY);
        if (!frame->buf[0]) return fail("no AVBuffer");
        frame->data[0] = &gDummy;
        color = describeColor(stream);
        veyra::log::info("pushcore", std::format("devices ready adapter={} luid={:016X} (D3D11 and D3D12 on the same adapter)",
                                                 narrow(ctx.adapter().description), luid));
        // Deband: prepared for every 8-bit SDR stream (shaders compiled once, so switching it on later is immediate);
        // 10-bit and HDR sources rarely band and are left untouched.
        const bool hdrTransfer = stream.transfer == AVCOL_TRC_SMPTE2084 || stream.transfer == AVCOL_TRC_ARIB_STD_B67;
        if (stream.format == PUSHCORE_INPUT_NV12 && !hdrTransfer) {
            debandEligible = deband.init(d11.Get());
            if (!debandEligible) veyra::log::warn("pushcore", "deband pass unavailable: " + deband.why());
        }
        return PUSHCORE_OK;
    }

    // The frame generation methods to try, in order, with a multiplier cap each: the asked-for one first, then the
    // next best this GPU can run (gpu-best-results.md section 1), and at the end no frame generation (the open must
    // not fail because of one backend: the rest of the picture work still runs).
    std::vector<std::pair<uint32_t, uint32_t>> fallbackChain() const {
        std::vector<std::pair<uint32_t, uint32_t>> c;
        const uint32_t m = std::clamp<uint32_t>(settings.multiplier, 1, 8);
        if (m < 2) { c.push_back({settings.fgBackend, 8}); return c; }
        const bool nvidia = ctx.adapter().vendorId == 0x10DE, amd = ctx.adapter().vendorId == 0x1002;
        auto add = [&](uint32_t b, uint32_t cap) {
            if ((b == PUSHCORE_FG_VFG || b == PUSHCORE_FG_DLSSG) && !nvidia) return;
            if (b == PUSHCORE_FG_DLSSG && family == PUSHCORE_FAMILY_RTX20) return;
            for (const auto& e : c) if (e.first == b && e.second == cap) return;
            c.push_back({b, cap});
        };
        switch (settings.fgBackend) {
        case PUSHCORE_FG_DLSSG: add(PUSHCORE_FG_DLSSG, 8); if (m > 4) add(PUSHCORE_FG_DLSSG, 4); if (m > 2) add(PUSHCORE_FG_DLSSG, 2); add(PUSHCORE_FG_PUSHFLOW, 4); add(PUSHCORE_FG_FSR, 2); break;
        case PUSHCORE_FG_PUSHFLOW: add(PUSHCORE_FG_PUSHFLOW, 4); add(PUSHCORE_FG_FSR, 2); if (amd) add(PUSHCORE_FG_AMDFRC, 2); break;
        case PUSHCORE_FG_FSR: add(PUSHCORE_FG_FSR, 2); add(PUSHCORE_FG_PUSHFLOW, 4); if (amd) add(PUSHCORE_FG_AMDFRC, 2); break;
        case PUSHCORE_FG_AMDFRC: add(PUSHCORE_FG_AMDFRC, 2); add(PUSHCORE_FG_FSR, 2); add(PUSHCORE_FG_PUSHFLOW, 4); break;
        default: add(PUSHCORE_FG_VFG, 8); add(PUSHCORE_FG_DLSSG, 6); add(PUSHCORE_FG_PUSHFLOW, 4); add(PUSHCORE_FG_FSR, 2); if (amd) add(PUSHCORE_FG_AMDFRC, 2); break;
        }
        c.push_back({PUSHCORE_FG_NONE, 1});
        return c;
    }
    veyra::engine::FailedBackend lastFailed = veyra::engine::FailedBackend::None;
    uint32_t builtFg = ~0u, builtSr = ~0u, builtTier = ~0u; bool builtGuard = true, builtSubtitles = false;
    int buildGraph() {
        builtFg = settings.fgBackend; builtSr = settings.srMethod; builtTier = settings.fgTier; builtGuard = settings.cutGuard != 0; builtSubtitles = settings.subtitleHold != 0;
        if (!haveSettings) { tryFg = PUSHCORE_FG_VFG; tryMultiplierCap = 8; const int r = buildGraphOnce(); runFg = fg ? PUSHCORE_FG_VFG : PUSHCORE_FG_NONE; return r; }
        const auto chain = fallbackChain();
        std::string notes;
        fgFellBack = false;
        for (size_t i = 0; i < chain.size(); ++i) {
            tryFg = chain[i].first; tryMultiplierCap = chain[i].second;
            lastFailed = veyra::engine::FailedBackend::None;
            const int r = buildGraphOnce();
            if (r == PUSHCORE_OK) {
                runFg = fg ? tryFg : PUSHCORE_FG_NONE;
                fgFellBack = i > 0;
                fgNote = notes.empty() ? std::string() : notes + std::string(fgName(runFg));
                if (fgFellBack) veyra::log::warn("pushcore", "frame generation fallback: " + fgNote);
                return r;
            }
            if (deviceLost || i + 1 == chain.size()) return r;
            // only a frame generation failure moves down the chain; anything else is the open's error, as before
            if (lastFailed != veyra::engine::FailedBackend::Fg && lastFailed != veyra::engine::FailedBackend::NgxCore && lastFailed != veyra::engine::FailedBackend::OpticalFlow) return r;
            notes += std::format("{}{} did not start ({}); ", fgName(tryFg), chain[i].second < 8 ? std::format(" at {}x", std::min(settings.multiplier, chain[i].second)) : std::string(), error);
            veyra::log::warn("pushcore", std::format("{} did not start (failed backend {}): trying {}", fgName(tryFg), int(lastFailed), fgName(chain[i + 1].first)));
            teardownGraph();
            error.clear();
        }
        return PUSHCORE_ERROR;
    }
    // Graph in PushFPS's playback configuration, plus egress for its output pool.
    int buildGraphOnce() {
        using namespace veyra;
        engine::EnhancementSettings s;
        std::optional<engine::ChainRuntimeOrder> order;
        if (!loadSettings(s, order)) return PUSHCORE_ERROR;
        loadedSettings = s; loadedOrder = order;
        options = engine::PlayerOptions::from(s, order);
        if (cfgSr >= 0) options.sr = cfgSr != 0;
        if (cfgFg >= 0) options.fg = cfgFg != 0;
        if (cfgMultiplier >= 2) options.fgMultiplier = uint32_t(std::clamp(cfgMultiplier, 2, 8));
        if (cfgVfgQuality >= 0) options.settings.vfgQuality = uint32_t(cfgVfgQuality);
        if (options.fg && engine::presentSinkFrameGeneration(options.settings.frameGenerationBackend))
            return fail("the saved frame generation method presents on its own (XeSS) and cannot feed a renderer");

        const bool hdrTransfer = stream.transfer == AVCOL_TRC_SMPTE2084 || stream.transfer == AVCOL_TRC_ARIB_STD_B67;
        gd = {};
        gd.sourceWidth = stream.width; gd.sourceHeight = stream.height;
        initCostWindows();
        // EngineController: hdrInput = the source's colour is on the HDR path (PQ/HLG)
        gd.hdrInput = hdrTransfer;
        gd.highQualityPresentation = true;   // EngineController: every source (presenter only)
        gd.captureBitDepth = stream.format == PUSHCORE_INPUT_P010 ? 10 : 8;
        engine::StageRequest r;              // EngineController stageRequest() for an ordinary file
        r.nr = options.nr; r.sr = options.sr; r.fg = options.fg; r.fgMultiplier = options.fgMultiplier;
        r.width = stream.width; r.height = stream.height;
        r.stillImage = false; r.exportJob = false; r.liveSource = false; r.uncompressedSource = false;
        r.nvidiaAdapter = ctx.adapter().isNvidia; r.amdNr = ctx.adapter().vendorId == 0x1002;   // AMD: NR on the AMD runtimes
        r.nodeOrder = options.nodeOrder ? &*options.nodeOrder : nullptr;
        // PushFPS Crosshair Hold / Subtitle Hold: process-wide switches PushFPS's UI sets (PushOverlay.h), read by
        // describeStages. Crosshair Hold is never for video; Subtitle Hold follows PushMPC's setting, set right before each
        // graph is described (instances in one process describe their graphs one at a time, under the open / reload path).
        veyra::pipeline::overlay::setWanted(false);
        veyra::pipeline::overlay::setSubtitlesWanted(haveSettings && settings.subtitleHold != 0);
        subtitleState.store(0, std::memory_order_relaxed); subtitlePixels.store(0, std::memory_order_relaxed);
        subAutoInit = false; subCompose = false; subOnRun = subOffRun = 0;
        engine::describeStages(r, options.snapshot(), gd);
        gd.noFeatures = false;
        gd.runtimeAbsPath = runtimeDir;
        // initializePreview(), file branch:
        gd.vfgAsyncSubmission = options.settings.frameGenerationBackend == engine::FrameGenerationBackend::Vfg;
        gd.vfgFastFallback = false;          // live 2X sources only
        gd.nrReusePreview = true;
        gd.videoHdr = options.settings.videoHdr;
        // EngineController: hdrOutput = settings.useHdrPreview(hdrInput, displayHdrActive()); the display state is the
        // renderer's (its swap chain is HDR10 when Windows HDR is on and MPC-VR's HDR passthrough allows it)
        gd.hdrOutput = options.settings.useHdrPreview(gd.hdrInput, stream.displayHdr != 0);
        if (gd.hdrOutput && gd.hdrOutputMode != engine::HdrOutputMode::Hdr10) {
            // MPC-VR's HDR swap chain is HDR10 (PQ, BT.2020); PushFPS's scRGB FP16 mode would need an FP16 swap
            // chain. With FG on PushFPS outputs HDR10 anyway (EnhanceGraph::hdr10Output).
            veyra::log::info("pushcore", "saved HDR output mode scRGB replaced by HDR10 (MPC-VR presents HDR10)");
            gd.hdrOutputMode = engine::HdrOutputMode::Hdr10;
        }
        gd.nrAutoPoolLayer.reset();
        gd.shareableOutputs = (flags & PUSHCORE_FLAG_EGRESS_COPY) == 0;
        gd.pushMpcCutGuard = !haveSettings || settings.cutGuard != 0;   // the scene-cut guard for every backend
        // its optical flow (cutGuard 3; PUSHMPC_CUT_FLOW=0/1 overrides, 2 = FidelityFX's flow on any GPU, for calibration)
        {
            int flowMode = (!haveSettings || (settings.cutGuard & 2) != 0) ? 1 : 0;
            if (const char* e = std::getenv("PUSHMPC_CUT_FLOW")) flowMode = std::atoi(e);
            // NVIDIA's flow only by default: FidelityFX's returns zero vectors around large changes (its own scene-change
            // history), which added nothing in the calibration (round 4)
            gd.pushMpcGuardFlow = gd.pushMpcCutGuard && flowMode != 0 && (flowMode == 2 || gd.opticalFlowBackend == engine::OpticalFlowBackend::Nvidia);
            if (flowMode == 2) gd.opticalFlowBackend = engine::OpticalFlowBackend::AmdFidelityFx;
            // only the guard takes the flow (VFG, PushFlow, AMD FRC without NR): a small flow extent is enough
            if (gd.pushMpcGuardFlow && !gd.fgUsesFlow() && !options.settings.nr) {
                const uint32_t fw = guardFlowWidth();
                if (stream.width > fw) { gd.flowWidth = fw; gd.flowHeight = std::max(64u, ((stream.height * fw / stream.width) + 1u) & ~1u); }
            }
        }
        if (options.fg && options.settings.frameGenerationBackend == engine::FrameGenerationBackend::AmdFrc) preferSystemD3dCompiler();

        fg = options.fg;
        multiplier = fg ? options.fgMultiplier : 1;
        hdrIn = gd.hdrInput; hdrOut = gd.hdrOutput;
        resolveHdrHint();
        veyra::log::info("display-color", std::format("hdrInput={} hdrOutput={} displayHdr={} forceSdrPreview={} hdrOutputMode={} hdrMetadataHint={} hdrCurve={} videoHdr={} masteringPeak={} maxCLL={} maxFALL={}",
            gd.hdrInput, gd.hdrOutput, stream.displayHdr, options.settings.forceSdrPreview, int(gd.hdrOutputMode), hdrHint, hdrCurveOn,
            gd.videoHdr.enabled, stream.masteringPeakNits, stream.maxCllNits, stream.maxFallNits));
        gd.enableFg = fg;
        gd.fgMultiplier = fg ? multiplier : gd.fgMultiplier;

        if (!cache) {
            // first graph of this instance: take the process-wide warm cache when nobody else holds it
            if (!(flags & PUSHCORE_FLAG_NO_WARM_CACHE) && !gSharedCacheUser) {
                if (!gSharedCache) gSharedCache = new veyra::ngx::NgxCoreCache();
                gSharedCacheUser = this;
                sharedCache = true;
                cache = gSharedCache;
                gSharedDevice = ctx.device();
            } else {
                cache = &ownCache;
            }
        }
        graph = std::make_unique<pipeline::EnhanceGraph>(ctx, ring, cache);
        const auto t = Clock::now();
        if (!graph->initialize(gd) || !graph->createViews()) {
            lastFailed = graph->failedBackend();
            return fail(std::format("enhancement graph did not start (failed backend {})", int(graph->failedBackend())));
        }
        graph->recordGpuTimings();
        if (fg && !graph->fgEnabled()) { lastFailed = engine::FailedBackend::Fg; return fail("frame generation did not start"); }
        if (haveSettings && settings.activeArea[2] > settings.activeArea[0] && settings.activeArea[3] > settings.activeArea[1])
            graph->setCutGuardActiveArea(settings.activeArea[0], settings.activeArea[1], settings.activeArea[2], settings.activeArea[3]);
        cutGuardOn = fg && graph->cutGuardActive();
        description = std::format(
            "source {}x{} {}{} -> output {}x{} {}; SR {} (videoSrQuality {}); FG {} {} {}X (quality/tier {}, async {}, cut guard {}); NR {}; egress {}; GPU family {}",
            stream.width, stream.height, stream.format == PUSHCORE_INPUT_P010 ? "P010" : "NV12",
            !gd.hdrInput ? "" : stream.transfer == AVCOL_TRC_ARIB_STD_B67 ? " HLG" : " PQ",
            graph->workWidth(), graph->workHeight(),
            graph->hdr10Output() ? "HDR10 (PQ BT.2020)" : gd.hdrInput ? "SDR (PushFPS BT.2390 tone map)" : "SDR",
            graph->srEnabled() ? "on" : "off", gd.videoSrQuality, fg ? "on" : "off",
            engine::frameGenerationBackendName(options.settings.frameGenerationBackend), multiplier, gd.vfgQuality,
            gd.vfgAsyncSubmission ? 1 : 0, fg ? (cutGuardOn ? "on" : "off") : "-", graph->nrEnabled() ? "on" : "off",
            gd.shareableOutputs ? "direct (shared graph pool)" : "copy ring", family);
        veyra::log::info("pushcore", std::format("graph ready in {:.0f} ms: {}", msSince(t), description));
        return buildEgress();
    }

    // AMD FRC's AMF runtime needs the system's d3dcompiler_47.dll: an old copy (Qt's in Veyra's folder; MPC-HC ships its
    // own D3DCompiler_47.dll too) made its Init fail (result 18). Loaded here by full path from System32 before AMF
    // starts; when another copy is already in the process, AMF may still get that one (logged; untested on AMD here).
    void preferSystemD3dCompiler() {
        HMODULE loaded = GetModuleHandleW(L"d3dcompiler_47.dll");
        wchar_t path[MAX_PATH]{};
        if (loaded) GetModuleFileNameW(loaded, path, MAX_PATH);
        const std::filesystem::path sys = system32() / L"d3dcompiler_47.dll";
        if (loaded && _wcsicmp(path, sys.c_str()) != 0)
            veyra::log::warn("amd-frc", "d3dcompiler_47.dll already loaded from " + narrow(path) + ": loading System32's too; AMF may use the first one");
        if (!LoadLibraryExW(sys.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
            veyra::log::warn("amd-frc", std::format("System32 d3dcompiler_47.dll did not load (error {})", GetLastError()));
        else veyra::log::info("amd-frc", "d3dcompiler_47.dll from System32 loaded before AMF");
    }
    // VideoPresenter::applyHdrMetadata: the HDR10 metadata hint goes out only when the user's PushFPS setting asks for
    // it (off by default: PushFPS then clears the swap chain's metadata) and the output is HDR10.
    void resolveHdrHint() {
        using namespace veyra::engine;
        HdrCurveSettings curve; HdrMetadataSettings meta;
        resolveHdrTuning(gd.hdrCurve, gd.hdrMetadata, curve, meta);
        hdrCurveOn = gd.hdrOutput && curve.enabled;
        const bool hdr10 = gd.hdrOutput && (gd.enableFg || gd.hdrOutputMode == HdrOutputMode::Hdr10);
        const bool forced = meta.masteringPeakNits || meta.maxCllNits || meta.maxFallNits;
        hdrHint = meta.enabled && hdr10 && (gd.hdrInput || forced);
        hdrMeta = {};
        if (!hdrHint) return;
        HdrSourceMetadata src;
        src.observe(1, gd.hdrInput, stream.masteringPeakNits, stream.maxCllNits, stream.maxFallNits);
        auto light = resolveHdrMetadataLuminance(meta, src);
        if (curve.enabled && curve.peakCapNits) { light.cll = std::min(light.cll, curve.peakCapNits); light.fall = std::min(light.fall, light.cll); }
        hdrMeta.RedPrimary[0] = 35400; hdrMeta.RedPrimary[1] = 14600;     // BT.2020 + D65, as PushFPS sends them
        hdrMeta.GreenPrimary[0] = 8500; hdrMeta.GreenPrimary[1] = 39850;
        hdrMeta.BluePrimary[0] = 6550; hdrMeta.BluePrimary[1] = 2300;
        hdrMeta.WhitePoint[0] = 15635; hdrMeta.WhitePoint[1] = 16450;
        hdrMeta.MaxMasteringLuminance = light.peak; hdrMeta.MinMasteringLuminance = light.minX10000;
        hdrMeta.MaxContentLightLevel = uint16_t(light.cll); hdrMeta.MaxFrameAverageLightLevel = uint16_t(light.fall);
    }

    int buildEgress() {
        opened.clear(); outSlots.clear(); outNext = 0;
        egressMode = gd.shareableOutputs ? PUSHCORE_EGRESS_DIRECT : PUSHCORE_EGRESS_COPY;
        if (egressMode == PUSHCORE_EGRESS_DIRECT) {
            // Open the whole pool now so the hot path never creates handles.
            std::vector<ID3D12Resource*> pool{graph->videoFrameResource(0), graph->videoFrameResource(1)};
            for (uint32_t i = 0; i < veyra::pipeline::kGeneratedPoolSlots; ++i) pool.push_back(graph->generatedFrameResource(i));
            for (auto* r : pool)
                if (r && r->GetDesc().Width == graph->workWidth() && r->GetDesc().Height == graph->workHeight() && !openDirect(r))
                    return PUSHCORE_ERROR;
            veyra::log::info("pushcore", std::format("egress direct: {} graph output textures opened in D3D11", opened.size()));
            return PUSHCORE_OK;
        }
        const uint32_t count = 2 * multiplier + 2;
        outSlots.resize(count);
        for (auto& slot : outSlots) {
            D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = graph->workWidth(); td.Height = graph->workHeight(); td.DepthOrArraySize = 1; td.MipLevels = 1;
            td.Format = graph->outputFormat(); td.SampleDesc.Count = 1;
            td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;   // D3D11 refuses to open it otherwise
            if (hrFail(ctx.device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &td, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                             IID_PPV_ARGS(&slot.res)), "egress ring texture")) return PUSHCORE_ERROR;
            HANDLE h = nullptr;
            if (hrFail(ctx.device()->CreateSharedHandle(slot.res.Get(), nullptr, GENERIC_ALL, nullptr, &h), "egress ring handle")) return PUSHCORE_ERROR;
            const HRESULT hr = d11_1->OpenSharedResource1(h, IID_PPV_ARGS(&slot.tex));
            CloseHandle(h);
            if (hrFail(hr, "D3D11 OpenSharedResource1(egress ring)")) return PUSHCORE_ERROR;
        }
        if (!copyList) {
            for (auto& a : copyAlloc)
                if (hrFail(ctx.device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)), "copy allocator")) return PUSHCORE_ERROR;
            if (hrFail(ctx.device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, copyAlloc[0].Get(), nullptr, IID_PPV_ARGS(&copyList)), "copy list"))
                return PUSHCORE_ERROR;
            copyList->Close();
        }
        veyra::log::info("pushcore", std::format("egress copy ring: {} shared textures", count));
        return PUSHCORE_OK;
    }

    bool openDirect(ID3D12Resource* r) {
        if (opened.count(r)) return true;
        HANDLE h = nullptr;
        HRESULT hr = ctx.device()->CreateSharedHandle(r, nullptr, GENERIC_ALL, nullptr, &h);
        if (hrFail(hr, "D3D12 CreateSharedHandle(graph output)")) return false;
        ComPtr<ID3D11Texture2D> t;
        hr = d11_1->OpenSharedResource1(h, IID_PPV_ARGS(&t));
        CloseHandle(h);
        if (hrFail(hr, "D3D11 OpenSharedResource1(graph output)")) return false;
        opened.emplace(r, std::move(t));
        return true;
    }

    // ------------------------------------------------------------------------------------------
    bool canSubmit() const { return graph && graph->nextFrameSlotAvailable(); }

    bool ensureIngress(ID3D11Texture2D* src) {
        D3D11_TEXTURE2D_DESC sd{};
        src->GetDesc(&sd);
        const DXGI_FORMAT want = stream.format == PUSHCORE_INPUT_P010 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
        if (sd.Format != want && !(want == DXGI_FORMAT_P010 && sd.Format == DXGI_FORMAT_P016)) {
            fail(std::format("input texture format {} does not match the stream", int(sd.Format)));
            return false;
        }
        if (sd.Width == inW && sd.Height == inH && sd.Format == inFormat && inSlots[0].tex) return true;
        if (inSlots[0].tex) ring.drainQueue();
        for (auto& slot : inSlots) {
            slot = {};
            D3D11_TEXTURE2D_DESC d{};
            d.Width = sd.Width; d.Height = sd.Height; d.MipLevels = 1; d.ArraySize = 1;
            d.Format = sd.Format == DXGI_FORMAT_P016 ? DXGI_FORMAT_P010 : sd.Format;
            d.SampleDesc = {1, 0}; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            d.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
            if (hrFail(d11->CreateTexture2D(&d, nullptr, &slot.tex), "ingress ring texture")) return false;
            ComPtr<IDXGIResource1> dr; HANDLE h = nullptr;
            HRESULT hr = slot.tex.As(&dr);
            if (SUCCEEDED(hr)) hr = dr->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &h);
            if (hrFail(hr, "ingress ring handle")) return false;
            hr = ctx.device()->OpenSharedHandle(h, IID_PPV_ARGS(&slot.res));
            CloseHandle(h);
            if (hrFail(hr, "D3D12 OpenSharedHandle(ingress ring)")) return false;
        }
        inW = sd.Width; inH = sd.Height; inFormat = sd.Format;
        veyra::log::info("pushcore", std::format("ingress ring: {} shared textures {}x{} format {} (source mips {} array {})",
                                                 kIngressSlots, inW, inH, int(inFormat), sd.MipLevels, sd.ArraySize));
        return true;
    }


    // FG admission (M2b). The renderer decides the level from what it observes at the presenter (generated frames
    // presented late) and passes it with each submit: 0 = the full group, 1 = the 2X midpoint only (PushFPS's
    // "reduced group": a presentable midpoint instead of a whole-group hole, history kept), 2 = no generated frame
    // (history kept; for VFG PushFPS turns a budget Seed into exactly this Skip). A predictive version after
    // EngineController::admitFg (deadline vs predicted GPU cost) was tried first: its cost predictions were too
    // pessimistic under other GPU load and halved the rate at 1.0x where nothing would have been late; see HANDOFF.
    // The measured stage costs are still collected and logged ("fg-admission") for diagnosis.
    Clock::time_point lastAdmitLog{};
    struct AdmitContext { bool clock = false; int32_t level = 0; double queuedMs = 0; };
    AdmitContext admitCtx;
    veyra::pipeline::EnhanceGraph::FgDecision admit(const veyra::pipeline::FrameBatch& batch, bool warmingHistory) {
        using D = veyra::pipeline::EnhanceGraph::FgDecision;
        if (Clock::now() - lastAdmitLog > std::chrono::seconds(10)) {
            lastAdmitLog = Clock::now();
            veyra::log::info("fg-admission", std::format("source={} level={} queuedMs={:.1f} measured GPU per pair (p75, last 20 s): full {:.1f} ms (n={}), midpoint-only {:.1f} ms (n={}); pairs full {} reduced {} none {}",
                batch.identity.sourceFrameId, admitCtx.level, admitCtx.queuedMs, costFull.q75(), costFull.size(), costReduced.q75(), costReduced.size(),
                st.fgFull, st.fgReduced, st.fgSkipped));
        }
        if (!admitCtx.clock || warmingHistory || multiplier < 2 || admitCtx.level <= 0) return D::Evaluate;
        if (admitCtx.level == 1 && multiplier > 2) return D::Reduced;
        return D::Skip;
    }

    void collectTimings() {
        if (!graph) return;
        cutsSeen.store(cutBase + graph->cutGuardCuts(), std::memory_order_relaxed);
        for (const auto& t : graph->takeGpuTimings()) {
            using veyra::diagnostics::GpuStage;
            auto ms = [&](GpuStage s) { const auto& g = t.gpu[size_t(s)]; return g.state == veyra::diagnostics::SampleState::Measured && g.milliseconds ? *g.milliseconds : -1.0; };
            PushCoreGpuTiming p{};
            p.sourceFrameId = t.identity.sourceFrameId;
            p.colorMs = ms(GpuStage::Color); p.srMs = ms(GpuStage::Sr); p.fgBatchMs = ms(GpuStage::FgBatch); p.fg1Ms = ms(GpuStage::Fg1);
            p.graphMs = veyra::diagnostics::graphExecutionSpanMs(t).value_or(-1.0);
            p.nrMs = ms(GpuStage::Nr);
            if (p.nrMs < 0) {
                double sum = -1;
                for (GpuStage l : {GpuStage::NrLayer0, GpuStage::NrLayer1, GpuStage::NrLayer2, GpuStage::NrLayer3})
                    if (const double v = ms(l); v >= 0) sum = (sum < 0 ? 0 : sum) + v;
                p.nrMs = sum;
            }
            p.videoHdrMs = ms(GpuStage::VideoHdr);
            if (auto it = decisionOf.find(p.sourceFrameId); it != decisionOf.end()) {
                // GPU work of the frame = its measured stages (colour, VSR, the FG group). The graph's execution span
                // also contains the queue's waits (presentation release fence, VFG's CUDA hand-off) and overstated
                // the cost under load by ~15 ms, which pushed 1.0x playback into reduced groups needlessly.
                const double span = (p.colorMs > 0 ? p.colorMs : 0) + (p.srMs > 0 ? p.srMs : 0) + (p.fgBatchMs > 0 ? p.fgBatchMs : 0)
                                  + (p.nrMs > 0 ? p.nrMs : 0) + (p.videoHdrMs > 0 ? p.videoHdrMs : 0);
                if (it->second == 0 && p.fgBatchMs > 0) { costFull.push(span); costFirst.push(p.fg1Ms); costFirstBatch.push(p.fgBatchMs); }
                else if (it->second == 1 && p.fgBatchMs > 0) costReduced.push(span);
                decisionOf.erase(it);
            }
            while (decisionOf.size() > 256) decisionOf.erase(decisionOf.begin());
            if (timings.size() >= 16384) timings.pop_front();
            timings.push_back(p);
        }
    }

    int submit(ID3D11Texture2D* src, uint32_t slice, int64_t pts, uint32_t submitFlags, const PushCoreSubmitTiming* timing = nullptr) {
        using namespace veyra;
        if (!graph) return fail("not open");
        if (!src) return fail("no texture");
        const auto t0 = Clock::now();
        if (!canSubmit()) { ++st.busy; return PUSHCORE_BUSY; }
        if (!ensureIngress(src)) return PUSHCORE_ERROR;
        D3D11_TEXTURE2D_DESC sd{};
        src->GetDesc(&sd);
        if (slice >= sd.ArraySize) return fail("array slice out of range");

        auto& slot = inSlots[inIndex % kIngressSlots];
        ++inIndex;
        // Deband (PushMPC): the source slice goes through the pass into a scratch texture, which then takes the place
        // of the source in the ingress copy. Same immediate context, so the order is kept with no wait.
        ID3D11Texture2D* copySrc = src;
        UINT copySub = D3D11CalcSubresource(0, slice, sd.MipLevels);
        const bool debandNow = debandWanted() && deband.ready();
        if (debandNow && deband.run(imm.Get(), src, copySub, sd.Width, sd.Height, settings.debandStrength)) { copySrc = deband.out(); copySub = 0; }
        {
            const int state = debandNow && copySrc != src ? 1 + int(std::min<uint32_t>(settings.debandStrength, 2)) : 0;
            if (state != debandLogged) {
                debandLogged = state;
                veyra::log::info("pushcore", state ? std::format("deband on ({}): {} iteration(s) on the {}x{} NV12 source before the graph",
                                                                 pushmpc::kDebandPresets[state - 1].name, pushmpc::kDebandPresets[state - 1].iterations, sd.Width, sd.Height)
                                                   : std::string(debandNow ? "deband off: " + deband.why() : "deband off"));
                deband.resetTimings();
            }
        }
        if (slot.freeValue) imm4->Wait(fence12in11.Get(), slot.freeValue);   // GPU-side: graph consumed it
        imm->CopySubresourceRegion(slot.tex.Get(), 0, 0, 0, 0, copySrc, copySub, nullptr);
        if (hrFail(imm4->Signal(fence11.Get(), ++value11), "D3D11 Signal(ingress)")) return PUSHCORE_ERROR;
        imm4->Flush();

        pipeline::HardwareSurfaceInput hw;
        hw.texture = slot.res.Get(); hw.subresourceIndex = 0;
        hw.waitFence = fence11in12.Get(); hw.waitValue = value11;
        hw.textureWidth = inW; hw.textureHeight = inH;

        const bool reset = needReset || (submitFlags & PUSHCORE_SUBMIT_DISCONTINUITY) || !haveReal;
        Batch b;
        b.sourceIndex = submittedCount;
        b.realPts = pts;
        // admission context: the GPU work still queued ahead of this frame (predicted finish of earlier frames)
        const double nowMs = std::chrono::duration<double, std::milli>(t0.time_since_epoch()).count();
        {
            const uint64_t done = fence12->GetCompletedValue();
            while (!inflight.empty() && inflight.front().fenceValue <= done) inflight.pop_front();
        }
        admitCtx = {};
        admitCtx.clock = timing && timing->structSize >= PUSHCORE_SUBMIT_TIMING_SIZE_V2 && timing->streamNow100ns != INT64_MIN;
        if (admitCtx.clock) {
            admitCtx.level = std::clamp<int32_t>(timing->fgLevel, 0, 2);
            admitCtx.queuedMs = inflight.empty() ? 0.0 : std::max(0.0, inflight.back().finishMs - nowMs);
        }
        int decision = 2;
        const uint64_t frameId = ++sequence;
        const auto admitFn = [&](const veyra::pipeline::FrameBatch& fb, bool warming) {
            veyra::pipeline::EnhanceGraph::FgDecision d = veyra::pipeline::EnhanceGraph::FgDecision::Evaluate;
            try { d = admit(fb, warming); } catch (...) {}
            using D = veyra::pipeline::EnhanceGraph::FgDecision;
            decision = d == D::Evaluate ? 0 : d == D::Reduced ? 1 : 2;
            return d;
        };
        // Dolby Vision profile 5 (filter route): this frame's conversion, as MediaFileSource sets
        // ColorDescription::dolbyVisionP5 per frame in PushFPS's own player; inactive for every other frame
        color.dolbyVisionP5 = veyra::pipeline::DolbyVisionP5{};
        static_assert(offsetof(PushCoreSubmitTiming, dvP5Exact) == 140, "PushCoreSubmitTiming V3 layout");
        if (timing && timing->structSize >= offsetof(PushCoreSubmitTiming, dvP5Curve) + sizeof(timing->dvP5Curve) && timing->dvP5Active) {
            auto& dv = color.dolbyVisionP5;
            dv.active = true;
            for (int i = 0; i < 9; ++i) { dv.yccToRgb[i] = timing->dvP5YccToRgb[i]; dv.lmsToRgb[i] = timing->dvP5LmsToRgb[i]; }
            for (int c = 0; c < 3; ++c) for (int k = 0; k < 3; ++k) dv.curve[c][k] = timing->dvP5Curve[c * 3 + k];
            // the exact reshaping (piece-wise polynomial + MMR) when the caller sends it
            if (timing->structSize >= offsetof(PushCoreSubmitTiming, dvP5ExactCurve) + sizeof(timing->dvP5ExactCurve) && timing->dvP5Exact) {
                dv.exact = true;
                std::memcpy(dv.exactCurve, timing->dvP5ExactCurve, sizeof(dv.exactCurve));
            }
            if (!dvP5Logged) { dvP5Logged = true; veyra::log::info("pushcore", "Dolby Vision profile 5: per-frame RPU conversion (PushFPS DolbyVisionP5) active"); }
        }
        const bool useAdmission = fg && admitCtx.clock;
        if (!graph->process(frame, double(pts) / 10000.0, reset, b.out, frameId, &color, &hw,
                            /*retainReferences*/ false,
                            useAdmission ? veyra::pipeline::EnhanceGraph::FgAdmission(admitFn) : veyra::pipeline::EnhanceGraph::FgAdmission{},
                            /*previewMultiplier*/ multiplier))
            return fail(std::format("graph process failed (failed backend {})", int(graph->failedBackend())));
        if (!useAdmission) decision = fg ? 0 : 2;
        if (fg && !reset) {
            // reset pairs evaluate one seed group whatever the decision; count real pairs only
            if (decision == 0) ++st.fgFull; else if (decision == 1) ++st.fgReduced; else ++st.fgSkipped;
        }
        decisionOf[frameId] = reset ? 3 : decision;
        {
            // predicted GPU finish of this frame: after the queued work, at its decision's measured cost
            double cost = decision == 0 ? costFull.p50() : decision == 1 ? costReduced.p50() : -1;
            if (cost <= 0) cost = decision == 2 ? 3.0 : (costFull.p50() > 0 ? costFull.p50() : 25.0);
            const double startMs = inflight.empty() ? nowMs : std::max(nowMs, inflight.back().finishMs);
            inflight.push_back({startMs + cost, value12 + 1});   // the Signal below
            if (inflight.size() > 16) inflight.pop_front();
        }
        // One shared fence value after everything this source frame queued (VFG's CUDA work is a
        // queue Wait inside process, so queue order covers generated frames too).
        if (hrFail(ring.queue()->Signal(fence12.Get(), ++value12), "D3D12 Signal(batch)")) return PUSHCORE_ERROR;
        slot.freeValue = value12;
        b.readyValue = value12;
        batches.push_back(std::move(b));
        needReset = false; haveReal = true;
        ++submittedCount; ++st.submitted;
        st.submitCpuMs += msSince(t0);
        collectTimings();
        // PushFPS Subtitle Hold: its state and the pixels it held, about twice a second at 24 fps (the pixel count reads
        // the pass's own mapped sample; PushOverlayPass::harvest runs on this thread in update(), so it is read here too)
        if (graph && haveSettings && settings.subtitleHold == 2 && !subAutoInit) { graph->setSubtitleCompose(false); subAutoInit = true; }
        if (graph && (++subtitlePoll % 12) == 0) {
            const uint32_t state = uint32_t(graph->subtitleHoldState()), px = graph->subtitleHoldPixels();
            // Auto gate: burned-in subtitles (two lines over a 1080p picture upscaled to 4K) hold 78-91 k pixels of the
            // band; natural video without text held at most 6.4 k (study on four shows, eight segments, RTX 5090,
            // 2026-11-25). On at >= 0.4 % of the frame for 3 polls (~1.5 s at 24 fps), off below 0.05 % for 10 polls (~5 s).
            if (state == 1 && haveSettings && settings.subtitleHold == 2) {
                const double area = double(graph->workWidth()) * graph->workHeight();
                subOnRun = px >= area * 0.004 ? subOnRun + 1 : 0;
                subOffRun = px < area * 0.0005 ? subOffRun + 1 : 0;
                if (!subCompose && subOnRun >= 3) { subCompose = true; graph->setSubtitleCompose(true); veyra::log::info("pushcore", std::format("subtitle hold Auto: burned-in text found ({} pixels): holding", px)); }
                else if (subCompose && subOffRun >= 10) { subCompose = false; graph->setSubtitleCompose(false); veyra::log::info("pushcore", "subtitle hold Auto: no burned-in text for ~5 s: not holding"); }
            }
            subtitleState.store(state == 1 && haveSettings && settings.subtitleHold == 2 && !subCompose ? 4u : state, std::memory_order_relaxed);
            subtitlePixels.store(px, std::memory_order_relaxed);
        }
        return PUSHCORE_OK;
    }

    // Copies (copy-mode egress) for the frames handed out in this poll.
    struct CopyJob { ID3D12Resource* src; uint32_t slot; };
    int flushCopies(std::vector<CopyJob>& jobs, uint64_t& signalled) {
        if (jobs.empty()) return PUSHCORE_OK;
        const uint32_t a = copyNext++ % kCopyLists;
        if (copyAllocValue[a] && fence12->GetCompletedValue() < copyAllocValue[a]) {
            fence12->SetEventOnCompletion(copyAllocValue[a], waitEvent);
            WaitForSingleObject(waitEvent, 10000);
        }
        if (hrFail(copyAlloc[a]->Reset(), "copy allocator reset")) return PUSHCORE_ERROR;
        if (hrFail(copyList->Reset(copyAlloc[a].Get(), nullptr), "copy list reset")) return PUSHCORE_ERROR;
        for (const auto& j : jobs) {
            auto* dst = outSlots[j.slot].res.Get();
            D3D12_RESOURCE_BARRIER b[2]{};
            b[0].Type = b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b[0].Transition = {j.src, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE};
            b[1].Transition = {dst, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST};
            copyList->ResourceBarrier(2, b);
            copyList->CopyResource(dst, j.src);
            std::swap(b[0].Transition.StateBefore, b[0].Transition.StateAfter);
            std::swap(b[1].Transition.StateBefore, b[1].Transition.StateAfter);
            copyList->ResourceBarrier(2, b);
        }
        if (hrFail(copyList->Close(), "copy list close")) return PUSHCORE_ERROR;
        // The D3D11 side must be done with every destination before the copy overwrites it.
        uint64_t release = 0;
        for (const auto& j : jobs) release = std::max(release, outSlots[j.slot].releaseValue);
        if (release) ring.queue()->Wait(fence11in12.Get(), release);
        ID3D12CommandList* lists[] = {copyList.Get()};
        ring.queue()->ExecuteCommandLists(1, lists);
        if (hrFail(ring.queue()->Signal(fence12.Get(), ++value12), "D3D12 Signal(copy)")) return PUSHCORE_ERROR;
        copyAllocValue[a] = value12;
        signalled = value12;
        jobs.clear();
        return PUSHCORE_OK;
    }

    int pollOnce(PushCoreFrame* frames, uint32_t capacity, uint32_t& count) {
        using namespace veyra::pipeline;
        std::vector<CopyJob> jobs;
        std::vector<uint32_t> copyFrames;  // indices into frames[] awaiting the copy fence value
        while (count < capacity && !batches.empty()) {
            auto& b = batches.front();
            if (b.next >= b.out.batch.count) { batches.pop_front(); continue; }
            const uint32_t i = b.next;
            if (!graph->resolveFrame(b.out, i)) break;
            auto& f = b.out.batch.frames[i];
            if (f.kind == FrameKind::Generated && f.validity != GenerationValidity::Valid) {
                ++st.skippedGenerated; f.lease.reset(); ++b.next; continue;
            }
            int outSlot = -1;
            ID3D11Texture2D* tex = nullptr;
            if (egressMode == PUSHCORE_EGRESS_COPY) {
                auto& slot = outSlots[outNext % outSlots.size()];
                if (slot.held) break;   // caller holds the whole ring: return frames first
                outSlot = int(outNext % outSlots.size());
                ++outNext;
                slot.held = true;
                tex = slot.tex.Get();
                jobs.push_back({f.lease->texture.Get(), uint32_t(outSlot)});
            } else {
                auto it = opened.find(f.lease->texture.Get());
                if (it == opened.end()) {
                    if (!openDirect(f.lease->texture.Get())) return PUSHCORE_ERROR;   // e.g. an exclusion texture
                    it = opened.find(f.lease->texture.Get());
                }
                tex = it->second.Get();
            }
            PushCoreFrame& o = frames[count];
            o = {};
            o.texture = tex;
            o.readyFence = fence12in11.Get();
            o.readyValue = b.readyValue;
            o.kind = f.kind == FrameKind::Real ? PUSHCORE_FRAME_REAL : PUSHCORE_FRAME_GENERATED;
            o.pts100ns = f.kind == FrameKind::Real ? b.realPts : f.pts100ns;
            o.sourceIndex = b.sourceIndex;
            o.subframe = f.subframe;
            o.width = graph->workWidth(); o.height = graph->workHeight();
            Lease l;
            l.kind = o.kind; l.parity = f.lease->slot; l.outSlot = outSlot;
            if (egressMode == PUSHCORE_EGRESS_DIRECT) l.graphLease = std::move(f.lease);
            else { copyFrames.push_back(count); }
            o.lease = nextLease++;
            leases.emplace(o.lease, std::move(l));
            if (egressMode == PUSHCORE_EGRESS_COPY) {
                // the graph lease may go once the copy is queued (same queue, in order)
                pendingCopyLeases.push_back(std::move(f.lease));
            }
            ++count; ++b.next; ++st.outputs;
            if (o.kind == PUSHCORE_FRAME_REAL) ++st.real; else ++st.generated;
        }
        if (!jobs.empty()) {
            uint64_t v = 0;
            if (flushCopies(jobs, v) != PUSHCORE_OK) return PUSHCORE_ERROR;
            for (auto k : copyFrames) frames[k].readyValue = v;
            pendingCopyLeases.clear();
        }
        while (!batches.empty() && batches.front().next >= batches.front().out.batch.count) batches.pop_front();
        return PUSHCORE_OK;
    }
    std::vector<std::shared_ptr<veyra::pipeline::FrameLease>> pendingCopyLeases;

    int poll(PushCoreFrame* frames, uint32_t capacity, uint32_t* countOut, uint32_t timeoutMs) {
        if (!graph) return fail("not open");
        uint32_t count = 0;
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            if (pollOnce(frames, capacity, count) != PUSHCORE_OK) return PUSHCORE_ERROR;
            collectTimings();
            if (count || !timeoutMs || batches.empty() || Clock::now() >= deadline) break;
            // Wait for the GPU (fence event), or yield while the async VFG verdict lands.
            auto& b = batches.front();
            const auto& f = b.out.batch.frames[b.next];
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (f.lease && !f.lease->ready() && f.lease->readyFenceObject) {
                f.lease->readyFenceObject->SetEventOnCompletion(f.lease->readyFence, waitEvent);
                WaitForSingleObject(waitEvent, DWORD(std::max<long long>(1, left)));
            } else if (egressMode == PUSHCORE_EGRESS_COPY && outSlots[outNext % outSlots.size()].held) {
                break;   // nothing will change until the caller returns frames
            } else {
                Sleep(0);
            }
        }
        *countOut = count;
        return PUSHCORE_OK;
    }

    int presented(uint64_t token, ID3D11DeviceContext* c) {
        auto it = leases.find(token);
        if (it == leases.end()) return fail("unknown lease");
        if (c) {
            if (c != lastPresentCtx) {
                lastPresentCtx4.Reset();
                if (FAILED(c->QueryInterface(IID_PPV_ARGS(&lastPresentCtx4)))) return fail("context lacks ID3D11DeviceContext4");
                lastPresentCtx = c;
            }
            if (hrFail(lastPresentCtx4->Signal(fence11.Get(), ++value11), "D3D11 Signal(presented)")) return PUSHCORE_ERROR;
        }
        auto& l = it->second;
        if (egressMode == PUSHCORE_EGRESS_DIRECT) {
            // The real frame comes last in its batch, so its value also covers the batch's generated reads.
            if (l.kind == PUSHCORE_FRAME_REAL && value11) graph->presentationSubmitted(l.parity, fence11in12.Get(), value11);
        } else if (l.outSlot >= 0) {
            outSlots[size_t(l.outSlot)].releaseValue = value11;
            outSlots[size_t(l.outSlot)].held = false;
        }
        leases.erase(it);
        return PUSHCORE_OK;
    }

    void dropQueued() {
        batches.clear();
        pendingCopyLeases.clear();
        needReset = true;
    }

    int flush() {
        if (!leases.empty()) return PUSHCORE_BUSY;
        dropQueued();
        return PUSHCORE_OK;
    }

    void teardownGraph() {
        if (ring.initialized()) ring.drainQueue();
        dropQueued();
        opened.clear(); outSlots.clear();
        if (graph) { graph->shutdown(); cutBase += graph->cutGuardCuts(); cutsSeen.store(cutBase, std::memory_order_relaxed); graph.reset(); }
    }

    // M2c: PushFPS's live path (EngineController: requiresGraphRebuild() false and the same node order ->
    // EnhanceGraph::applySettings, no drain, no history reset). Anything else needs reload().
    veyra::engine::EnhancementSettings loadedSettings;
    std::optional<veyra::engine::ChainRuntimeOrder> loadedOrder;
    int applySettingsLive() {
        using namespace veyra;
        if (!graph) return fail("not open");
        const auto t = Clock::now();
        if (haveSettings && (settings.fgBackend != builtFg || settings.srMethod != builtSr || settings.fgTier != builtTier || (settings.cutGuard != 0) != builtGuard || (settings.subtitleHold != 0) != builtSubtitles)) {
            veyra::log::info("pushcore", "frame generation method, upscaler, cut guard or Subtitle Hold changed: graph rebuild");
            return PUSHCORE_NEEDS_REBUILD;
        }
        engine::EnhancementSettings s;
        std::optional<engine::ChainRuntimeOrder> order;
        if (!loadSettings(s, order)) return PUSHCORE_NEEDS_REBUILD;
        if (engine::requiresGraphRebuild(loadedSettings, s) || !(order == loadedOrder)) {
            veyra::log::info("pushcore", "settings change needs a graph rebuild");
            return PUSHCORE_NEEDS_REBUILD;
        }
        auto next = engine::PlayerOptions::from(s, order);
        if (cfgSr >= 0) next.sr = cfgSr != 0;
        if (cfgFg >= 0) next.fg = cfgFg != 0;
        if (cfgMultiplier >= 2) next.fgMultiplier = uint32_t(std::clamp(cfgMultiplier, 2, 8));
        if (next.sr != options.sr || next.fg != options.fg || next.nr != options.nr || (next.fg && next.fgMultiplier != options.fgMultiplier) ||
            next.settings.useHdrPreview(gd.hdrInput, stream.displayHdr != 0) != gd.hdrOutput) {
            veyra::log::info("pushcore", "settings change needs a graph rebuild (stages or HDR output)");
            return PUSHCORE_NEEDS_REBUILD;
        }
        if (cfgVfgQuality >= 0) s.vfgQuality = uint32_t(cfgVfgQuality);
        const bool videoChanged = !s.sameActiveVideoParameters(loadedSettings);
        if (videoChanged && !graph->applySettings(s)) {
            veyra::log::info("pushcore", "EnhanceGraph::applySettings refused the change: graph rebuild");
            return PUSHCORE_NEEDS_REBUILD;
        }
        loadedSettings = s;
        options = next;
        resolveHdrHint();
        veyra::log::info("pushcore", std::format("settings {} in {:.1f} ms (no rebuild)", videoChanged ? "applied live" : "stored, nothing on the GPU changed", msSince(t)));
        return PUSHCORE_OK;
    }

    int reload() {
        if (!leases.empty()) return PUSHCORE_BUSY;
        const auto t = Clock::now();
        int r = PUSHCORE_ERROR;
        {
            std::lock_guard<std::mutex> lk(gGraphLock);
            teardownGraph();
            haveReal = false; needReset = true;
            r = buildGraph();
        }
        if (r == PUSHCORE_OK && !(flags & PUSHCORE_FLAG_NO_WARMUP)) warmUp();
        veyra::log::info("pushcore", std::format("reload {} in {:.0f} ms", r == PUSHCORE_OK ? "done" : "failed", msSince(t)));
        return r;
    }

    // Runs a few synthetic frames through the new graph so its first-use stalls (RTX VSR's first evaluate,
    // ~65 ms, and VFG's first pair) happen here, on the opening thread, and not on the first real frames.
    void warmUp() {
        const auto t = Clock::now();
        ComPtr<ID3D11Texture2D> tex;
        D3D11_TEXTURE2D_DESC d{};
        d.Width = (stream.width + 1) & ~1u; d.Height = (stream.height + 1) & ~1u; d.MipLevels = 1; d.ArraySize = 1;
        const bool p010 = stream.format == PUSHCORE_INPUT_P010;
        d.Format = p010 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
        d.SampleDesc = {1, 0}; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        // limited-range black: Y 16, UV 128 (P010 keeps the value in the top 10 bits)
        const size_t bpp = p010 ? 2 : 1;
        std::vector<uint8_t> init(size_t(d.Width) * d.Height * 3 / 2 * bpp);
        const size_t ySize = size_t(d.Width) * d.Height * bpp;
        if (p010) {
            auto* p = reinterpret_cast<uint16_t*>(init.data());
            for (size_t i = 0; i < ySize / 2; ++i) p[i] = uint16_t(16 << 8);
            for (size_t i = ySize / 2; i < init.size() / 2; ++i) p[i] = uint16_t(128 << 8);
        } else {
            std::memset(init.data(), 16, ySize);
            std::memset(init.data() + ySize, 128, init.size() - ySize);
        }
        D3D11_SUBRESOURCE_DATA sd{init.data(), UINT(d.Width * bpp), 0};
        if (FAILED(d11->CreateTexture2D(&d, &sd, &tex))) { veyra::log::info("pushcore", "warm-up skipped: no texture"); return; }
        const int64_t step = stream.fpsNum > 0 && stream.fpsDen > 0 ? int64_t(10000000.0 * stream.fpsDen / stream.fpsNum) : 417083;
        uint32_t frames = 0, outputs = 0;
        bool ok = true;
        auto drainSome = [&](uint32_t timeoutMs) {
            PushCoreFrame f[16]; uint32_t n = 0;
            if (poll(f, 16, &n, timeoutMs) != PUSHCORE_OK) return false;
            for (uint32_t k = 0; k < n; ++k) { presented(f[k].lease, imm.Get()); ++outputs; }
            return true;
        };
        for (int i = 0; i < 3 && ok; ++i) {
            // return what the graph produced so its two real-frame slots free up
            for (int spin = 0; !canSubmit() && spin < 400 && ok; ++spin) ok = drainSome(5);
            if (!ok || submit(tex.Get(), 0, i * step, i == 0 ? PUSHCORE_SUBMIT_DISCONTINUITY : 0) != PUSHCORE_OK) { ok = false; break; }
            ++frames;
        }
        const auto deadline = Clock::now() + std::chrono::seconds(3);
        while (ok && !batches.empty() && Clock::now() < deadline) ok = drainSome(10);
        if (!leases.empty()) {
            std::vector<uint64_t> left;
            for (auto& kv : leases) left.push_back(kv.first);
            for (auto l : left) presented(l, nullptr);
        }
        imm4->Flush();
        if (ring.initialized()) ring.drainQueue();
        dropQueued();
        haveReal = false; needReset = true;
        st = {}; timings.clear(); submittedCount = 0;
        costFull.clear(); costReduced.clear(); costFirst.clear(); costFirstBatch.clear(); decisionOf.clear(); inflight.clear();
        initCostWindows();
        if (graph) (void)graph->takeGpuTimings();
        veyra::log::info("pushcore", std::format("warm-up {}: {} frames in, {} out, {:.0f} ms", ok ? "done" : "incomplete", frames, outputs, msSince(t)));
        error.clear();
    }

    void close() {
        leases.clear();
        {
            std::lock_guard<std::mutex> lk(gGraphLock);
            teardownGraph();
            checkDeviceLost();
            if (sharedCache) {
                // keep the NGX core warm for the next file, unless the device is gone
                if (deviceLost) { (void)gSharedCache->close("device-removed"); gSharedDevice.Reset(); }
                gSharedCacheUser = nullptr;
                sharedCache = false;
            } else {
                ownCache.close("pushcore-close");
            }
            cache = nullptr;
        }
        for (auto& s : inSlots) s = {};
        deband = pushmpc::Deband{}; debandEligible = false;
        copyList.Reset();
        for (auto& a : copyAlloc) a.Reset();
        if (ring.initialized()) ring.shutdown();
        fence11in12.Reset(); fence12.Reset();
        if (ctx.initialized()) ctx.shutdown();
        if (frame) { frame->data[0] = nullptr; av_frame_free(&frame); }
        if (waitEvent) { CloseHandle(waitEvent); waitEvent = nullptr; }
        fence12in11.Reset(); fence11.Reset();
        lastPresentCtx4.Reset(); lastPresentCtx = nullptr;
        imm4.Reset(); imm.Reset(); d11_5.Reset(); d11_1.Reset(); d11.Reset();
    }
};

// ---------------------------------------------------------------------------------------------
// C ABI
extern "C" {

static std::atomic<int> gOpenInstances{0};   // host process GPU priority goes back to Normal with the last one

uint32_t pushcore_abiVersion(void) { return PUSHCORE_ABI_VERSION; }
uint32_t pushcore_features(void) { return PUSHCORE_FEATURE_DV_P5 | PUSHCORE_FEATURE_SR_FAMILY | PUSHCORE_FEATURE_DEBAND | PUSHCORE_FEATURE_CUT_HOLD | PUSHCORE_FEATURE_FG_METHODS | PUSHCORE_FEATURE_CUT_COUNT | PUSHCORE_FEATURE_HDR_PEAK | PUSHCORE_FEATURE_DV_P5_EXACT | PUSHCORE_FEATURE_SUBTITLE_HOLD | PUSHCORE_FEATURE_SUBTITLE_HOLD_AUTO; }

PushCore* pushcore_open(const PushCoreConfig* config, const PushCoreStreamInfo* stream, ID3D11Device* device,
                        char* errorBuffer, uint32_t errorBufferSize) {
    auto report = [&](const std::string& message) {
        if (errorBuffer && errorBufferSize) {
            const size_t n = std::min<size_t>(message.size(), errorBufferSize - 1);
            std::memcpy(errorBuffer, message.data(), n);
            errorBuffer[n] = 0;
        }
    };
    if (!config || config->structSize < sizeof(PushCoreConfig) || config->abiVersion != PUSHCORE_ABI_VERSION) { report("config version mismatch"); return nullptr; }
    if (!stream || stream->structSize < sizeof(PushCoreStreamInfo) || !stream->width || !stream->height) { report("bad stream info"); return nullptr; }
    if (!device) { report("no D3D11 device"); return nullptr; }
    PushCore* core = nullptr;
    try {
        // RuntimePaths override first: shaders, runtimes and data then resolve from the PushFPS folder.
        const std::filesystem::path root = config->pushfpsDir && *config->pushfpsDir ? std::filesystem::path(config->pushfpsDir) : moduleDirectory();
        veyra::runtime::setApplicationRootOverride(root);
        if (config->shaderDir && *config->shaderDir) veyra::runtime::setShaderDirectoryOverride(config->shaderDir);
        if (config->logFile && *config->logFile) {
            veyra::Logger::instance().openFile(config->logFile);
            veyra::Logger::instance().setConsoleEnabled(false);
        }
        core = new PushCore();
        core->stream = *stream;
        core->dataDir = config->dataDir ? std::wstring(config->dataDir) : (veyra::runtime::userDataDirectory() / L"user-data-2.0.0").wstring();
        core->runtimeDir = config->runtimeDir && *config->runtimeDir ? std::wstring(config->runtimeDir) : veyra::runtime::localRuntimeDirectory().wstring();
        core->cfgSr = config->sr; core->cfgFg = config->fg; core->cfgMultiplier = config->multiplier; core->cfgVfgQuality = config->vfgQuality;
        core->flags = config->flags;
        if (config->settings) {
            if (!copySettings(config->settings, core->settings)) { report("settings version mismatch"); delete core; return nullptr; }
            core->haveSettings = true;
        }
        {
            // AMD NR on RX 9000 (lmxxf) runs PushFPS's HIP self-test in a child process first (issue 88); PushFPS uses its
            // own exe for it, pushcore the small pushcore_hip_preflight.exe beside this DLL
            const auto preflight = moduleDirectory() / L"pushcore_hip_preflight.exe";
            if (std::filesystem::exists(preflight)) veyra::pipeline::amdhip::setPreflightExecutable(preflight.wstring());
        }
        veyra::log::info("pushcore", std::format("open root={} shaders={} runtime={} data={}", narrow(root.wstring()),
                                                 narrow(veyra::runtime::shaderDirectory().wstring()), narrow(core->runtimeDir), narrow(core->dataDir)));
        int built = core->openDevices(device);
        if (built == PUSHCORE_OK) {
            std::lock_guard<std::mutex> lk(gGraphLock);
            built = core->buildGraph();
        }
        if (built != PUSHCORE_OK) {
            report(core->error);
            core->close();
            delete core;
            return nullptr;
        }
        if (!(core->flags & PUSHCORE_FLAG_NO_WARMUP)) core->warmUp();
        ++gOpenInstances;
        core->description += std::format("; GPU priority class {}", veyra::gfx::gpuPriorityStatus().actual);
        return core;
    } catch (const std::exception& e) {
        report(std::string("exception: ") + e.what());
    } catch (...) {
        report("unknown exception");
    }
    if (core) { core->close(); delete core; }
    return nullptr;
}

#define PUSHCORE_GUARD(core, expr)                                                         \
    if (!(core)) return PUSHCORE_ERROR;                                                   \
    if ((core)->deviceLost) return PUSHCORE_DEVICE_LOST;                                  \
    try { const int r_ = (expr); return (r_ == PUSHCORE_ERROR && (core)->checkDeviceLost()) ? PUSHCORE_DEVICE_LOST : r_; } \
    catch (const std::exception& e) { return (core)->fail(std::string("exception: ") + e.what()); } \
    catch (...) { return (core)->fail("unknown exception"); }

int pushcore_canSubmit(PushCore* core) { if (!core) return 0; try { return core->canSubmit() ? 1 : 0; } catch (...) { return 0; } }
int pushcore_submit(PushCore* core, ID3D11Texture2D* t, uint32_t slice, int64_t pts, uint32_t flags) { PUSHCORE_GUARD(core, core->submit(t, slice, pts, flags)); }
int pushcore_submit2(PushCore* core, ID3D11Texture2D* t, uint32_t slice, int64_t pts, uint32_t flags, const PushCoreSubmitTiming* timing) {
    PUSHCORE_GUARD(core, core->submit(t, slice, pts, flags, timing));
}
int pushcore_poll(PushCore* core, PushCoreFrame* frames, uint32_t capacity, uint32_t* count, uint32_t timeoutMs) {
    if (!count || !frames) return PUSHCORE_ERROR;
    *count = 0;
    PUSHCORE_GUARD(core, core->poll(frames, capacity, count, timeoutMs));
}
int pushcore_presented(PushCore* core, uint64_t lease, ID3D11DeviceContext* c) { PUSHCORE_GUARD(core, core->presented(lease, c)); }
int pushcore_reload(PushCore* core) { PUSHCORE_GUARD(core, core->reload()); }
int pushcore_applySettings(PushCore* core) { PUSHCORE_GUARD(core, core->applySettingsLive()); }
int pushcore_flush(PushCore* core) { PUSHCORE_GUARD(core, core->flush()); }
int pushcore_setSettings(PushCore* core, const PushCoreSettings* settings) {
    if (!core) return PUSHCORE_ERROR;
    if (settings && !copySettings(settings, core->settings)) return core->fail("settings version mismatch");
    core->haveSettings = settings != nullptr;
    return PUSHCORE_OK;   // the deband choice applies from the next submit; the rest with applySettings / reload
}
int pushcore_outputInfo(PushCore* core, PushCoreOutputInfo* info) {
    if (!core || !info || !core->graph) return PUSHCORE_ERROR;
    *info = {};
    info->width = core->graph->workWidth(); info->height = core->graph->workHeight();
    info->dxgiFormat = uint32_t(core->graph->outputFormat());
    info->multiplier = core->multiplier;
    info->egressMode = core->egressMode;
    info->adapterLuid = core->ctx.adapter().luid;
    info->hdrInput = core->hdrIn ? 1 : 0;
    info->hdrOutput = core->graph->hdr10Output() ? 1 : 0;
    info->hdrMetadataHint = core->hdrHint ? 1 : 0;
    info->hdrCurveNotApplied = core->hdrCurveOn ? 1 : 0;
    static_assert(sizeof(info->hdr10Metadata) == sizeof(DXGI_HDR_METADATA_HDR10), "HDR10 metadata layout");
    std::memcpy(&info->hdr10Metadata, &core->hdrMeta, sizeof(core->hdrMeta));
    const size_t n = std::min(core->description.size(), sizeof(info->description) - 1);
    std::memcpy(info->description, core->description.data(), n);
    return PUSHCORE_OK;
}
int pushcore_stats(PushCore* core, PushCoreStats* s) {
    if (!core || !s || s->structSize < PUSHCORE_STATS_SIZE_M2C) return PUSHCORE_ERROR;
    const uint32_t size = std::min<uint32_t>(s->structSize, uint32_t(sizeof(PushCoreStats)));
    core->st.structSize = size;
    core->st.debandActive = core->debandWanted() && core->deband.ready() && core->debandLogged > 0 ? 1u : 0u;
    core->st.debandMs = core->st.debandActive ? core->deband.medianMs() : -1.0;
    core->st.leasesOut = core->leases.size();
    uint64_t pending = 0;
    for (const auto& b : core->batches) pending += b.out.batch.count - b.next;
    core->st.pendingFrames = pending;
    {
        const auto f = core->costFull.recentP50(std::chrono::milliseconds(3000));
        const auto r = core->costReduced.recentP50(std::chrono::milliseconds(3000));
        core->st.fullCostRecentMs = f.first; core->st.fullCostRecentN = f.second;
        core->st.reducedCostRecentMs = r.first; core->st.reducedCostRecentN = r.second;
        core->st.fullCostWindowMs = core->costFull.p50();
        core->st.reducedCostWindowMs = core->costReduced.p50();
    }
    core->st.fgBackend = core->runFg;
    core->st.fgFallback = core->fgFellBack ? 1u : 0u;
    core->st.srMethod = core->runSr;
    core->st.cutGuardActive = core->cutGuardOn ? 1u : 0u;
    core->st.cutGuardCuts = core->cutsSeen.load(std::memory_order_relaxed);
    core->st.subtitleHoldState = core->subtitleState.load(std::memory_order_relaxed);
    core->st.subtitleHoldPixels = core->subtitlePixels.load(std::memory_order_relaxed);
    std::snprintf(core->st.fgNote, sizeof(core->st.fgNote), "%s", core->fgNote.c_str());
    std::memcpy(s, &core->st, size);
    return PUSHCORE_OK;
}
int pushcore_takeGpuTimings(PushCore* core, PushCoreGpuTiming* samples, uint32_t capacity, uint32_t* count) {
    if (!core || !count) return PUSHCORE_ERROR;
    *count = 0;
    try { core->collectTimings(); } catch (...) {}
    while (*count < capacity && !core->timings.empty()) { samples[(*count)++] = core->timings.front(); core->timings.pop_front(); }
    return PUSHCORE_OK;
}
const char* pushcore_lastError(PushCore* core) { return core ? core->error.c_str() : "no instance"; }
void pushcore_close(PushCore* core) {
    if (!core) return;
    try { core->close(); } catch (...) {}
    delete core;
    if (--gOpenInstances == 0) {
        try { veyra::gfx::requestGpuPriority(veyra::gfx::GpuPriority::Normal); } catch (...) {}
    }
}

int pushcore_importChainSession(const wchar_t* dataDir, PushCoreSettings* out, char* errorBuffer, uint32_t errorBufferSize) {
    auto report = [&](const std::string& m) {
        if (errorBuffer && errorBufferSize) { const size_t n = std::min<size_t>(m.size(), errorBufferSize - 1); std::memcpy(errorBuffer, m.data(), n); errorBuffer[n] = 0; }
        return PUSHCORE_ERROR;
    };
    if (!dataDir || !*dataDir || !out) return report("no data folder");
    try {
        using namespace veyra::engine;
        const std::filesystem::path file = std::filesystem::path(dataDir) / L"chain-session.v1";
        if (!std::filesystem::exists(file)) return report("no chain-session.v1 in the PushFPS data folder");
        EnhancementSettings s;
        auto session = ChainSession::initial(s);
        ChainSessionStore store(file);
        if (!store.load(session)) return report("chain session not loaded: " + narrow(store.error()));
        session.configurations[size_t(session.active)].apply(s);
        PushCoreSettings r{};
        r.structSize = sizeof(r);
        r.source = PUSHCORE_SETTINGS_PUSHMPC;
        r.sr = s.sr ? 1 : 0;
        r.srQuality = rtxVideoSr(s.videoSrQuality) ? std::clamp(videoSrTier(s.videoSrQuality), 1u, 4u) : 0u; // 0 = not an RTX VSR choice
        r.srFamily = !rtxVideoSr(s.videoSrQuality) ? PUSHCORE_SR_FAMILY_STANDARD
                   : videoSrFamily(s.videoSrQuality) == VideoSrFamily::HighBitrate ? PUSHCORE_SR_FAMILY_HIGH_BITRATE
                   : videoSrFamily(s.videoSrQuality) == VideoSrFamily::Streaming ? PUSHCORE_SR_FAMILY_STREAMING : PUSHCORE_SR_FAMILY_STANDARD;
        r.deband = 0; r.debandStrength = PUSHCORE_DEBAND_MEDIUM;   // PushFPS has no deband
        r.srTarget = s.srTarget == veyra::pipeline::SrTarget::Qhd ? PUSHCORE_SR_TARGET_1440P
                   : s.srTarget == veyra::pipeline::SrTarget::Uhd5K ? PUSHCORE_SR_TARGET_5K : PUSHCORE_SR_TARGET_4K;
        r.multiplier = s.multiplier > 1 ? s.multiplier : 0;
        r.vfgQuality = vfgPrimaryMode(s.vfgQuality);
        r.nr = s.nr ? 1 : 0;
        const auto layer = s.nrLayer(0);
        r.nrTone = layer.model.tone; r.nrStructure = layer.model.structure;
        r.videoHdr = s.videoHdr.enabled ? 1 : 0;
        *out = r;
        return PUSHCORE_OK;
    } catch (const std::exception& e) { return report(std::string("exception: ") + e.what()); }
    catch (...) { return report("unknown exception"); }
}

int pushcore_capabilities(uint64_t adapterLuid, const wchar_t* rootDir, PushCoreCaps* out) {
    if (!out || out->structSize < sizeof(PushCoreCaps)) return PUSHCORE_ERROR;
    try {
        PushCoreCaps c{};
        c.structSize = sizeof(c);
        const std::filesystem::path root = rootDir && *rootDir ? std::filesystem::path(rootDir) : moduleDirectory();
        veyra::runtime::setApplicationRootOverride(root);
        DXGI_ADAPTER_DESC1 d{};
        if (!findAdapter(adapterLuid, d)) {
            for (auto& r : c.reason) std::snprintf(r, sizeof(r), "no DirectX 12 hardware adapter found");
            *out = c;
            return PUSHCORE_ERROR;
        }
        fillCaps(d, veyra::runtime::localRuntimeDirectory(), c);
        *out = c;
        return PUSHCORE_OK;
    } catch (...) { return PUSHCORE_ERROR; }
}

// PushFPS's PushFlowBench (tools/pushflow_bench/main.cpp), ported: the product's settings (static shapes, queued
// inference), synthetic textured frames shifted by 8 px, warm-up, then per pair the first generated frame (full pack)
// and one further frame (time-only pack), each bracketed by D3D12 timestamps around pack, network and rebuild.
int pushcore_pushFlowBench(uint64_t adapterLuid, const wchar_t* rootDir, PushCoreFlowBench* io) {
    if (!io || io->structSize < sizeof(PushCoreFlowBench)) return PUSHCORE_ERROR;
    for (int t = 0; t < 3; ++t) io->firstMsP50[t] = io->firstMsP95[t] = io->nextMsP50[t] = io->nextMsP95[t] = io->initMs[t] = -1;
    io->adapter[0] = 0; io->note[0] = 0;
    std::string note;
    auto finish = [&](int r) { std::snprintf(io->note, sizeof(io->note), "%s", note.c_str()); return r; };
    try {
        const std::filesystem::path root = rootDir && *rootDir ? std::filesystem::path(rootDir) : moduleDirectory();
        veyra::runtime::setApplicationRootOverride(root);
        const std::wstring runtimeDir = veyra::runtime::localRuntimeDirectory().wstring();
        std::wstring why;
        if (!veyra::pushflow::runtimeInstalled(runtimeDir, &why)) { note = "PushFlow is not installed: " + narrow(why); return finish(PUSHCORE_ERROR); }
        DXGI_ADAPTER_DESC1 d{};
        if (!findAdapter(adapterLuid, d)) { note = "no adapter"; return finish(PUSHCORE_ERROR); }
        std::snprintf(io->adapter, sizeof(io->adapter), "%s", narrow(d.Description).c_str());
        ComPtr<IDXGIFactory6> factory; ComPtr<IDXGIAdapter1> adapter; ComPtr<ID3D12Device> device;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) || FAILED(factory->EnumAdapterByLuid(d.AdapterLuid, IID_PPV_ARGS(&adapter))) ||
            FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) { note = "no D3D12 device"; return finish(PUSHCORE_ERROR); }
        ComPtr<ID3D12CommandQueue> queue; ComPtr<ID3D12CommandAllocator> alloc[2]; ComPtr<ID3D12GraphicsCommandList> list;
        ComPtr<ID3D12Fence> fence; ComPtr<ID3D12QueryHeap> queries; ComPtr<ID3D12Resource> readback;
        D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) return finish(PUSHCORE_ERROR);
        for (auto& a : alloc) if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) return finish(PUSHCORE_ERROR);
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0].Get(), nullptr, IID_PPV_ARGS(&list))) || FAILED(list->Close())) return finish(PUSHCORE_ERROR);
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return finish(PUSHCORE_ERROR);
        D3D12_QUERY_HEAP_DESC qh{}; qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qh.Count = 8;
        if (FAILED(device->CreateQueryHeap(&qh, IID_PPV_ARGS(&queries)))) return finish(PUSHCORE_ERROR);
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 64; bd.Height = 1; bd.DepthOrArraySize = 1;
        bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) return finish(PUSHCORE_ERROR);
        uint64_t frequency = 1; queue->GetTimestampFrequency(&frequency);
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        uint64_t value = 0; int current = 0;
        auto begin = [&] { current ^= 1; return SUCCEEDED(alloc[current]->Reset()) && SUCCEEDED(list->Reset(alloc[current].Get(), nullptr)); };
        auto submit = [&] { if (FAILED(list->Close())) return false; ID3D12CommandList* l[] = {list.Get()}; queue->ExecuteCommandLists(1, l); return SUCCEEDED(queue->Signal(fence.Get(), ++value)); };
        auto wait = [&] { if (fence->GetCompletedValue() < value) { fence->SetEventOnCompletion(value, event); WaitForSingleObject(event, 60000); } return fence->GetCompletedValue() >= value; };
        const uint32_t w = std::clamp<uint32_t>(io->width ? io->width : 3840, 64, 7680), h = std::clamp<uint32_t>(io->height ? io->height : 2160, 64, 4320);
        auto frameTexture = [&](double shift) -> ComPtr<ID3D12Resource> {
            auto tex = veyra::pipeline::makeTexture(device.Get(), w, h, DXGI_FORMAT_R8G8B8A8_UNORM, false);
            const UINT pitch = (w * 4 + 255) & ~255u;
            auto upload = veyra::pipeline::makeUploadBuffer(device.Get(), uint64_t(pitch) * h);
            if (!tex || !upload) return nullptr;
            uint8_t* mapped = nullptr;
            upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x) {
                    uint8_t* p = mapped + size_t(y) * pitch + size_t(x) * 4;
                    const double u = x - shift;
                    const uint32_t hsh = uint32_t(int64_t(u) * 73856093) ^ (y * 19349663u);
                    const int v = 60 + int((hsh ^ (hsh >> 13)) * 1274126177u >> 25);
                    p[0] = uint8_t(v); p[1] = uint8_t(v * 9 / 10 + 12); p[2] = uint8_t(v * 8 / 10 + 25); p[3] = 255;
                }
            upload->Unmap(0, nullptr);
            if (!begin()) return nullptr;
            D3D12_TEXTURE_COPY_LOCATION from{}, to{};
            from.pResource = upload.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            from.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, w, h, 1, pitch};
            to.pResource = tex.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = tex.Get();
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            list->ResourceBarrier(1, &b);
            list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            list->ResourceBarrier(1, &b);
            if (!submit() || !wait()) return nullptr;
            return tex;
        };
        auto a = frameTexture(0), b = frameTexture(8);
        auto output = veyra::pipeline::makeTexture(device.Get(), w, h, DXGI_FORMAT_R8G8B8A8_UNORM, false);
        if (!a || !b || !output) { CloseHandle(event); note = "textures failed"; return finish(PUSHCORE_ERROR); }
        const int pairs = int(std::clamp<uint32_t>(io->pairs ? io->pairs : 12, 2, 200));
        const uint32_t mask = io->tierMask ? io->tierMask : 7u;
        const char* tierName[] = {"Fast", "Balanced", "Quality"};
        bool any = false;
        for (uint32_t tier = 0; tier < 3; ++tier) {
            if (!(mask & (1u << tier))) continue;
            if (device->GetDeviceRemovedReason() != S_OK) { note += "device removed; "; break; }
            veyra::pushflow::PushFlowBackend flow;
            const auto t0 = Clock::now();
            if (!flow.initialize(device.Get(), queue.Get(), runtimeDir, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, veyra::pushflow::Encoding::Encoded, tier)) {
                note += std::format("{}: did not start; ", tierName[tier]);
                continue;
            }
            io->initMs[tier] = msSince(t0);
            std::vector<double> first, next;
            bool outputState = false, good = true;
            auto one = [&](float t, bool timeOnly, std::vector<double>* into) {
                if (!begin()) return false;
                list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
                if (!flow.pack(list.Get(), a.Get(), b.Get(), t, timeOnly)) return false;
                if (!submit() || !flow.infer() || !begin()) return false;
                D3D12_RESOURCE_BARRIER bar{}; bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; bar.Transition.pResource = output.Get();
                bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                if (!outputState) { bar.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON; bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST; list->ResourceBarrier(1, &bar); outputState = true; }
                if (!flow.resolve(list.Get(), a.Get(), b.Get(), output.Get())) return false;
                list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
                list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, readback.Get(), 0);
                if (!submit() || !wait()) return false;
                if (into) {
                    uint64_t* ts = nullptr;
                    readback->Map(0, nullptr, reinterpret_cast<void**>(&ts));
                    into->push_back(double(ts[1] - ts[0]) * 1000.0 / double(frequency));
                    const D3D12_RANGE none{0, 0}; readback->Unmap(0, &none);
                }
                return true;
            };
            for (int i = 0; i < 4 && good; ++i) good = one(0.5f, false, nullptr);   // warm-up
            for (int i = 0; i < pairs && good; ++i) { good = one(1.0f / 3.0f, false, &first) && one(2.0f / 3.0f, true, &next); }
            if (!good || first.empty()) { note += std::format("{}: a run failed; ", tierName[tier]); continue; }
            auto pct = [](std::vector<double> v, double p) { std::sort(v.begin(), v.end()); return v[std::min(v.size() - 1, size_t(p * double(v.size() - 1) + 0.5))]; };
            io->firstMsP50[tier] = pct(first, 0.5); io->firstMsP95[tier] = pct(first, 0.95);
            io->nextMsP50[tier] = pct(next, 0.5); io->nextMsP95[tier] = pct(next, 0.95);
            note += std::format("{}: {} {}; ", tierName[tier], flow.status().model, flow.status().precision);
            any = true;
        }
        CloseHandle(event);
        return finish(any ? PUSHCORE_OK : PUSHCORE_ERROR);
    } catch (const std::exception& e) { note = std::string("exception: ") + e.what(); return finish(PUSHCORE_ERROR); }
    catch (...) { note = "exception"; return finish(PUSHCORE_ERROR); }
}

const PushCoreApi* pushcore_getApi(uint32_t abiVersion) {
    static const PushCoreApi api{sizeof(PushCoreApi), PUSHCORE_ABI_VERSION, pushcore_abiVersion, pushcore_open, pushcore_canSubmit,
                                 pushcore_submit, pushcore_poll, pushcore_presented, pushcore_reload, pushcore_flush,
                                 pushcore_outputInfo, pushcore_stats, pushcore_takeGpuTimings, pushcore_lastError, pushcore_close,
                                 pushcore_submit2, pushcore_applySettings, pushcore_setSettings,
                                 pushcore_importChainSession};
    return abiVersion == PUSHCORE_ABI_VERSION ? &api : nullptr;
}

}  // extern "C"
