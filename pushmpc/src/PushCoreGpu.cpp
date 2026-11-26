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
// pushcore_pushFlowBench (PushFPS's PushFlowBench, ported), and held-frame re-timing (PushCoreSettings.heldFrames:
// a held source frame is not given to the graph, the next distinct one comes with a per-call multiplier that spreads
// the generated frames over the whole held span; HeldFrames.h).
#define PUSHCORE_BUILD 1
#include "pushmpc/pushcore_abi.h"
#include "PushCoreDeband.h"
#include "PushCoreQuickVsr.h"        // fast start Route B: driver RTX VSR through the D3D11 video processor
#include "PushCoreXess.h"                   // G10: XeSS frame generation (headless swap chain) and XeSS SR
#include "veyra/gfx/FsrSrBackend.h"         // its provider override: XeSS SR in the graph's temporal upscaler slot
#include "veyra/pipeline/PushMpcCutGuard.h" // the cut guard of the XeFG route (pushcore's own, on the real frames it hands XeFG)
#include "PushCoreHeld.h"          // held-frame re-timing (HeldFrames.h): animation on twos / threes
#include "HoldPlan.h"              // anime round 5: hold plans for generated frames (motion classes, transients)
#include "PushCoreHold.h"          // ... and their compose pass
#include "PushCoreAnimeSr.h"        // anime upscalers (srMethod PMML) through pmml in the RTX VSR slot
#include "PushCoreRegion.h"        // per-region re-timing: source repair (held parts between their drawings)
#include "pushmpc/unlocks.h"            // T5b: off switches and self-test results of the vendor unlocks
#include "PushCoreDlssgPort.h"          // DLSS-G on RTX 20 / GTX 16 / RTX 30 through the community port (sdli1995/dlssg_for_sm86)
#include "pushmpc/dlssg_route.h"        // RTX 30: Transfusion or the port, per open, from the stored per-backend record
#include "PushCoreHelix.h"              // HelixSR (community): its frame generation and HLX FSR 4, fetched by Setup
#include "veyra/ngx/FgCompatibilitySession.h"   // processHealthy: Transfusion's in-process patches rolled back cleanly
#include "veyra/pipeline/VfxSrBackend.h"     // runtimeAvailable: the VFX VideoSuperRes files (high bitrate / streaming)
#include "veyra/pipeline/VfgBackend.h"
#include "veyra/pipeline/Rdna3NrBackend.h"
#include "PushCoreIntel.h"                   // Intel VSR (before the graph) and Intel VPL frame interpolation (after it)
#include "PushCoreIntelVsr.h"
#include "veyra/pipeline/ResolutionPlan.h"   // srStageExtents: Intel VSR's output size = PushFPS's SR size rule
#include "veyra/pipeline/AmdNrProvider.h"
#include "veyra/pipeline/AmdHipPreflight.h"
#include "veyra/pipeline/GpuPassUtils.h"
#include "veyra/pushflow/PushFlowBackend.h"
#include "veyra/gfx/AmfFrcBackend.h"
#include "veyra/gfx/FsrFgPresenter.h"   // VEYRA_FSR_PAIR_API: the anime FSR pair hunks of patches/pushfps-4.5.4-pushmpc.patch
#include "veyra/ngx/DlssgLite.h"
#include "veyra/ngx/NvOfSession.h"          // builtWithSdk: the NVOF frame generation capability
#include "PushCoreSelfTest.h"                // pushcore_features: the registered vendor methods' bits
#include "PushCoreZludaVsr.h"               // G11: RTX VSR on AMD RX 7000 / 9000 through ZLUDA (worker process)
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
#include "veyra/ngx/FgCompatibilitySession.h"
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
#include <condition_variable>
#include <fstream>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <map>
#include <array>
#include <set>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <string>
#include <thread>
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
    s.fgBackend = PUSHCORE_FG_VFG; s.fgTier = 1; s.srMethod = PUSHCORE_SR_METHOD_RTX; s.cutGuard = 1; s.fgLite = 0; s.heldFrames = 0;
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
    case PUSHCORE_FG_NVOF: return "NVOF FG";
    case PUSHCORE_FG_XEFG: return "XeFG";
    case PUSHCORE_FG_VPL: return "Intel VPL FI";
    case PUSHCORE_FG_HELIXFG: return "HelixSR FG";
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
        if (FAILED(a->GetDesc1(&d))) continue;
        const uint64_t l = (uint64_t(uint32_t(d.AdapterLuid.HighPart)) << 32) | d.AdapterLuid.LowPart;
        // WARP only by LUID with PUSHMPC_TEST_WARP=1 (CI), as the patched D3D12DeviceContext
        if ((d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && !(luid && l == luid && pmu::EnvTrue(L"PUSHMPC_TEST_WARP"))) continue;
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
// The GPUs the community DLSS-G port serves: Turing (compute capability 7.5), i.e. RTX 20 and the GTX 16 chips TU116 /
// TU117 (no tensor cores: the same SM 7.5 instruction set, the matrix work runs on their FP16 units, as for Gtx16Vsr;
// unproven upstream, so only the self-test decides)
bool turingPortGpu(uint32_t vendor, uint32_t device, uint32_t fam) {
    return vendor == 0x10DE && (fam == PUSHCORE_FAMILY_RTX20 || pmu::IsTu116(vendor, device) || pmu::IsTu117(vendor, device));
}
// The port's unlock on this GPU: TuringDlssg (RTX 20 / GTX 16: the port is the only DLSS-G), AmpereDlssg (RTX 30: the port
// next to DlssgTransfusion, pmu::ChooseDlssgRoute), Count = the port does not serve this GPU
pmu::Unlock portUnlockFor(uint32_t vendor, uint32_t device, uint32_t fam) {
    if (turingPortGpu(vendor, device, fam)) return pmu::Unlock::TuringDlssg;
    if (vendor == 0x10DE && fam == PUSHCORE_FAMILY_RTX30) return pmu::Unlock::AmpereDlssg;
    return pmu::Unlock::Count;
}
// DLSS-G through the community port (unlock TuringDlssg / AmpereDlssg): NVIDIA's DLSS-G runtime 310.9.16 with the port's
// SM75 / SM86 kernels (PushCoreDlssgPort.h; Setup fetches the port pinned). Eligible when the pinned files are there;
// Auto tries it once on a new adapter + driver (the first DLSS-G open on the port is the self-test: the port installs,
// NGX reports frame generation, the graph starts and delivers valid generated frames) and keeps it on only after that
// passed. Off switch PUSHMPC_DISABLE_TURINGDLSSG=1 / PUSHMPC_DISABLE_AMPEREDLSSG=1 or Unlocks\<Name> = 0.
pmu::UnlockDecision portDlssgDecision(pmu::Unlock u, uint32_t vendor, uint32_t device, const LUID& luid, bool files) {
    std::wstring src;
    const pmu::UnlockMode mode = pmu::ReadUnlockMode(u, &src);
    const pmu::SelfTest t = pmu::ReadSelfTest(u, pmu::SelfTestKey(vendor, device, driverVersionOf(luid)));
    return pmu::DecideUnlock(u, mode, files, L"the community DLSS-G port is not installed (Setup fetches it on RTX 20, GTX 16 and RTX 30 cards)", t, false, src);
}
// Crash guard for the port's self-test: the value "Running" (the adapter + driver key) is set while a self-test graph
// runs and cleared when it records a result or closes cleanly. Found at the next open, the earlier attempt ended the
// player process (crash or kill): that counts as a failed self-test, so a crashing port is never tried twice.
std::wstring portSelfTestPath(pmu::Unlock u) { return pmu::RegBase() + L"\\SelfTest\\" + pmu::UnlockName(u); }
std::wstring portRunningKey(pmu::Unlock u) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, portSelfTestPath(u).c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return {};
    const std::wstring v = pmu::RegStr(k, L"Running");
    RegCloseKey(k);
    return v;
}
// RTX 30: the DLSS-G backend this process is latched to (both patch DLSS-G in-process: never both in one process)
std::atomic<int> gDlssgLatch{int(pmu::DlssgRoute::None)};
void setPortRunning(pmu::Unlock u, const std::wstring& key) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, portSelfTestPath(u).c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    if (key.empty()) RegDeleteValueW(k, L"Running");
    else RegSetValueExW(k, L"Running", 0, REG_SZ, reinterpret_cast<const BYTE*>(key.c_str()), DWORD((key.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
}

// HelixSR (PushCoreHelix.h). Its frame generation: on GPUs without DLSS-G / VFG (GTX, Radeon, Intel, other hardware);
// HLX FSR 4 as the FSR upscaler: RX 5000 (Full network) and GTX 16 / GTX 10 (Lite), where AMD's DLL only has FSR 3.1.
// Both are unproven on these cards by their author ("expected"), so each runs only after its self-test on this adapter
// + driver (unlocks HelixFg / HlxFsr4; off: PUSHMPC_DISABLE_HELIXFG=1 / PUSHMPC_DISABLE_HLXFSR4=1 or Unlocks\<Name> = 0).
bool helixFgGpu(uint32_t vendor, uint32_t fam) {
    if (vendor == 0 || vendor == 0x1414) return false;   // no hardware GPU (WARP)
    return !(vendor == 0x10DE && rtxFamily(fam));
}
int hlxFsr4Kind(uint32_t vendor, uint32_t fam) {   // 2 = Full (RX 5000), 1 = Lite (GTX), 0 = not here
    if (fam == PUSHCORE_FAMILY_RDNA1) return 2;
    if (vendor == 0x10DE && fam == PUSHCORE_FAMILY_GTX) return 1;
    return 0;
}
std::filesystem::path helixRoot() { return pmhelix::Root(veyra::runtime::localDataDirectory()); }
std::filesystem::path hlxFsr4Dir(int kind) { return helixRoot() / (kind == 2 ? L"fsr4-full" : L"fsr4"); }
bool hlxFsr4Files(int kind) {
    const auto ld = veyra::runtime::localDataDirectory();
    return kind == 2 ? pmhelix::Fsr4FullPresent(ld) : kind == 1 ? pmhelix::Fsr4LitePresent(ld) : false;
}
pmu::UnlockDecision communityDecision(pmu::Unlock u, uint32_t vendor, uint32_t device, const LUID& luid, bool files, const wchar_t* notInstalled) {
    std::wstring src;
    const pmu::UnlockMode mode = pmu::ReadUnlockMode(u, &src);
    const pmu::SelfTest t = pmu::ReadSelfTest(u, pmu::SelfTestKey(vendor, device, driverVersionOf(luid)));
    return pmu::DecideUnlock(u, mode, files, notInstalled, t, false, src);
}
// HLX FSR 4 is loaded once per process (the FidelityFX loader finds its provider by name): the folder this process uses
std::atomic<int> gHlxFsr4Process{0};   // 0 = not decided, 1 = HLX FSR 4, 2 = AMD's (after a failed HLX self-test)

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
    // T5b: NVOF frame generation (FSR 3.1 interpolation on NVIDIA's optical-flow hardware): NVOFA on the card (every Turing
    // but TU117, Ampere and later), a core built with the Optical Flow SDK, the FidelityFX files; off switch NvofWarp
    {
        const bool nvofa = pmu::HasNvofa(v, d.DeviceId, rtx, name);
        const bool offSwitch = pmu::ReadUnlockMode(pmu::Unlock::NvofWarp) == pmu::UnlockMode::Off;
        const bool ffx = fsrLoader && fileExists(fsr / L"amd_fidelityfx_framegeneration_dx12.dll");
        set(PUSHCORE_CAP_NVOF_FG, nvofa && !offSwitch, ffx && veyra::ngx::NvOfSession::builtWithSdk(),
            offSwitch ? "switched off (PUSHMPC_DISABLE_NVOFWARP / Unlocks NvofWarp = 0)" : "needs NVIDIA's optical-flow hardware (Turing except GTX 1650 / TU117, or newer)",
            !ffx ? "the FidelityFX files are not installed (runtime_local\\amd\\fidelityfx)" : "this pushcore was built without NVIDIA Optical Flow support");
    }
    // G10: Intel XeSS frame generation and super resolution (PushCoreXessCaps.cpp: Intel GPU or the files installed, the
    // pinned Intel-signed files, the unlocks XessFg / XessSr behind their self-tests, run here once per adapter + driver)
    {
        const pushmpc::xess::XessCaps xc = pushmpc::xess::capabilities(d, driverVersionOf(d.AdapterLuid), true);
        set(PUSHCORE_CAP_XEFG, xc.fgHw, xc.fgOn, xc.fgWhy.c_str(), xc.fgWhy.c_str());
        set(PUSHCORE_CAP_XESS_SR, xc.srHw, xc.srOn, xc.srWhy.c_str(), xc.srWhy.c_str());
        if (xc.srOn && xc.srMs4k > 0) std::snprintf(c.reason[PUSHCORE_CAP_XESS_SR], sizeof(c.reason[0]), "ms4k=%.2f", xc.srMs4k);
        if (xc.fgOn && xc.fgMs4k > 0) std::snprintf(c.reason[PUSHCORE_CAP_XEFG], sizeof(c.reason[0]), "ms4k=%.2f", xc.fgMs4k);
    }
    set(PUSHCORE_CAP_VFG, vfgHw, veyra::pipeline::VfgBackend::runtimeAvailable(rt), "needs an NVIDIA RTX 30, 40 or 50 card",
        "the VFG files are not installed (runtime\\nvidia-vfg)");
    if (turingPortGpu(v, d.DeviceId, fam)) {
        // RTX 20 / GTX 16: the community port (unlock TuringDlssg); available while its self-test is pending (the first DLSS-G
        // open runs it; a failure falls back to the next method and turns it off for this adapter + driver) or passed
        const bool files = pmport::PortFilesPresent(runtimeDir);
        const pmu::UnlockDecision u = portDlssgDecision(pmu::Unlock::TuringDlssg, v, d.DeviceId, d.AdapterLuid, files);
        const std::string why = narrow(u.why);
        veyra::log::info("pushcore", std::format("unlock {}", why));
        const bool off = pmu::ReadUnlockMode(pmu::Unlock::TuringDlssg) == pmu::UnlockMode::Off;
        set(PUSHCORE_CAP_DLSSG, !off, files && (u.on || u.runSelfTest), why.c_str(),
            files ? why.c_str() : "the community DLSS-G port is not installed (Setup fetches it on RTX 20 and GTX 16 cards)");
    } else {
        // RTX 30: Transfusion's runtime, or the community port (unlock AmpereDlssg) when only that is installed
        const bool portOnly = fam == PUSHCORE_FAMILY_RTX30 && pmport::PortFilesPresent(runtimeDir)
                           && pmu::ReadUnlockMode(pmu::Unlock::AmpereDlssg) != pmu::UnlockMode::Off;
        set(PUSHCORE_CAP_DLSSG, nvidia && rtx, fileExists(runtimeDir / L"nvngx_dlssg.dll") || portOnly, "needs an NVIDIA RTX 20, 30, 40 or 50 card",
            "the DLSS-G files are not installed (nvngx_dlssg.dll)");
    }
    if (nvidia && fam == PUSHCORE_FAMILY_RTX30) {
        // RTX 30: FG Lite is the community port's DLSS-G preset C (unlock AmpereDlssg); offered once the port's self-test
        // passed on this card + driver (Auto's FG Lite tier then runs on the port, pmu::ChooseDlssgRoute)
        const bool files = pmport::PortFilesPresent(runtimeDir);
        const pmu::UnlockDecision u = portDlssgDecision(pmu::Unlock::AmpereDlssg, v, d.DeviceId, d.AdapterLuid, files);
        const std::string why = narrow(u.why);
        veyra::log::info("pushcore", std::format("unlock {}", why));
        set(PUSHCORE_CAP_DLSSG_LITE, pmu::ReadUnlockMode(pmu::Unlock::AmpereDlssg) != pmu::UnlockMode::Off, files && u.on, why.c_str(),
            files ? "RTX 30: FG Lite runs on the community DLSS-G port once its self-test passed (the first DLSS-G videos run it)"
                  : "RTX 30: FG Lite needs the community DLSS-G port (Setup fetches it on RTX 30 cards)");
    } else {
        set(PUSHCORE_CAP_DLSSG_LITE, nvidia && fam == PUSHCORE_FAMILY_RTX50, veyra::ngx::dlssgLiteRuntimePresent(runtimeDir),
            "DLSS-G performance mode runs on RTX 50 (and RTX 30 through the community port)", "the DLSS-G Lite runtime is not installed (dlssg-lite)");
    }
    {   // HelixSR (community): frame generation and HLX FSR 4, each available while its self-test is pending or passed
        const bool fgHw = helixFgGpu(v, fam);
        const bool fgFiles = pmhelix::FgPresent(veyra::runtime::localDataDirectory());
        const pmu::UnlockDecision uf = communityDecision(pmu::Unlock::HelixFg, v, d.DeviceId, d.AdapterLuid, fgFiles, L"HelixSR is not installed (Setup fetches it)");
        const std::string wf = narrow(uf.why);
        set(PUSHCORE_CAP_HELIX_FG, fgHw, fgFiles && (uf.on || uf.runSelfTest), "RTX cards run DLSS-G / VFG instead", fgFiles ? wf.c_str() : "HelixSR is not installed (Setup fetches it)");
        const int hk = hlxFsr4Kind(v, fam);
        const bool srFiles = hlxFsr4Files(hk);
        const pmu::UnlockDecision us = communityDecision(pmu::Unlock::HlxFsr4, v, d.DeviceId, d.AdapterLuid, srFiles, L"HLX FSR 4 is not installed (Setup fetches it)");
        const std::string ws = narrow(us.why);
        set(PUSHCORE_CAP_HLX_FSR4, hk != 0, srFiles && (us.on || us.runSelfTest), "only for RX 5000 and GTX cards (others have AMD's FSR 4 or RTX VSR)",
            srFiles ? ws.c_str() : "HLX FSR 4 is not installed (Setup fetches it)");
        if (fgHw || hk) veyra::log::info("pushcore", std::format("unlock {}; unlock {}", wf, ws));
    }
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
    } else if (amd && pmu::ZludaEligibleFamily(fam)) {
        // G11: RTX VSR on RX 7000 / 9000 through ZLUDA (unlock ZludaVsr): hardware = the family (unless switched off);
        // available = HIP + ZLUDA + nvngx_vsr.dll + the worker, and its self-test passed (or forced on). The RTX VSR
        // bit follows it, so the filter's "RTX VSR" choice and Auto pick it like on NVIDIA (pushcore runs it through
        // ZLUDA, PUSHCORE_SR_METHOD_ZLUDA_VSR); PUSHCORE_CAP_ZLUDA_VSR says which route it is.
        const auto z = pushmpc::zludaVsrState(d, fam, driverVersionOf(d.AdapterLuid), runtimeDir, moduleDirectory());
        const std::string why = narrow(z.decision.why);
        veyra::log::info("pushcore", std::format("unlock {}", why));
        const bool off = z.mode == pmu::UnlockMode::Off;
        const char* filesWhy = !z.elig.files ? z.elig.why.c_str() : why.c_str();
        set(PUSHCORE_CAP_ZLUDA_VSR, !off, z.elig.files && z.decision.on, off ? why.c_str() : z.elig.why.c_str(), filesWhy);
        set(PUSHCORE_CAP_RTX_VSR, !off, z.elig.files && z.decision.on, off ? why.c_str() : "RTX VSR on AMD runs through ZLUDA (RX 7000 / 9000)", filesWhy);
    } else {
        set(PUSHCORE_CAP_RTX_VSR, nvidia && rtx, vsrFiles, "needs an NVIDIA RTX card (GTX 16: unlock Gtx16Vsr after its self-test)", "nvngx_vsr.dll is not installed");
    }
    if (!(amd && pmu::ZludaEligibleFamily(fam)))
        set(PUSHCORE_CAP_ZLUDA_VSR, false, false, "RTX VSR through ZLUDA needs an AMD RX 7000 or RX 9000", "");
    set(PUSHCORE_CAP_VSR_FAMILIES, nvidia && rtx, veyra::pipeline::VfxSrBackend::runtimeAvailable(rt), "needs an NVIDIA RTX card",
        "the Video Effects VideoSuperRes files are not installed");
    set(PUSHCORE_CAP_FSR_SR, true, fsrLoader && fileExists(fsr / L"amd_fidelityfx_upscaler_dx12.dll"), "",
        "the FidelityFX files are not installed (runtime_local\\amd\\fidelityfx)");
    const auto shaders = veyra::runtime::shaderDirectory();
    set(PUSHCORE_CAP_NIS, true, fileExists(shaders / L"NisScalerSdr.dxil") && fileExists(shaders / L"NisColor.dxil"), "", "the NIS shaders are missing");
    // anime upscalers (srMethod PMML): any DirectX 12 GPU through DirectML, TensorRT-RTX on RTX; the default model's files
    {
        const std::string miss = pushmpc::animeSrMissing(PUSHCORE_ANIME_SR_ARTCNN_C4F16, nvidia && rtx);
        set(PUSHCORE_CAP_ANIME_SR, true, miss.empty(), "", miss.c_str());
    }
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
    // Intel video methods (PushCoreIntel.cpp): Intel VSR and VPL frame interpolation behind their self-tests (run here once
    // per adapter + driver on an Intel GPU, cached in SelfTest\IntelVsr / VplFi); when available, reason[] carries the
    // measured numbers Auto uses (the cost; VPL's default-on gate)
    {
        const pushmpc::IntelCaps ic = pushmpc::intelCaps(d, driverVersionOf(d.AdapterLuid), true);
        set(PUSHCORE_CAP_IVSR, ic.ivsrHw, ic.ivsrOn, ic.ivsrWhy.c_str(), ic.ivsrWhy.c_str());
        if (ic.ivsrOn) std::snprintf(c.reason[PUSHCORE_CAP_IVSR], sizeof(c.reason[0]), "ms4k=%.2f", ic.ivsrMs4k);
        set(PUSHCORE_CAP_VPL_FI, ic.vplHw, ic.vplOn, ic.vplWhy.c_str(), ic.vplWhy.c_str());
        if (ic.vplOn) std::snprintf(c.reason[PUSHCORE_CAP_VPL_FI], sizeof(c.reason[0]), "%s ms=%.2f psnr=%.2f maxM=%u", ic.vplGate ? "gate passed" : "candidate",
                                    ic.vplMs1080, ic.vplPsnr, ic.vplMaxMultiplier);
    }
}

// pushcore_selfTest PUSHCORE_SELFTEST_ZLUDA_VSR (G11): the ZludaVsr self-test now, waiting for it (Setup, the harness, a
// tester); the same state the capabilities and the open use
pushmpc::selftest::Result testZludaVsr(const pushmpc::selftest::Context& ctx) {
    pushmpc::selftest::Result r;
    if (!ctx.haveAdapter) { r.verdict = "unavailable: no adapter"; return r; }
    const uint32_t fam = familyOf(ctx.adapter.VendorId, ctx.adapter.Description);
    if (ctx.warp || ctx.adapter.VendorId != 0x1002 || !pmu::ZludaEligibleFamily(fam)) { r.verdict = "unavailable: needs an AMD RX 7000 / 9000"; return r; }
    const auto z = pushmpc::zludaVsrState(ctx.adapter, fam, driverVersionOf(ctx.adapter.AdapterLuid), veyra::runtime::localRuntimeDirectory(), moduleDirectory());
    std::string why;
    const int v = pushmpc::zludaVsrSelfTestNow(z, 120000, why);
    r.pass = v == 1;
    r.verdict = v == 1 ? "pass; " + why : v == 0 ? "fail: " + why : "unavailable: " + why;
    return r;
}
const pushmpc::selftest::Registrar kZludaVsrSelfTest({PUSHCORE_SELFTEST_ZLUDA_VSR, "zludavsr", PUSHCORE_FEATURE_ZLUDA_VSR, &testZludaVsr});
// pushcore_selfTest PUSHCORE_SELFTEST_ANIME_SR (256): the default anime upscaler (ArtCNN C4F16) on this adapter's backend
// (TensorRT-RTX on RTX, else DirectML / CPU), pushmpc::animeSrSelfTest
pushmpc::selftest::Result testAnimeSr(const pushmpc::selftest::Context& ctx) {
    pushmpc::selftest::Result r;
    const bool rtx = ctx.haveAdapter && !ctx.warp && ctx.adapter.VendorId == 0x10DE && rtxFamily(familyOf(ctx.adapter.VendorId, ctx.adapter.Description));
    std::string why;
    const bool ok = pushmpc::animeSrSelfTest(PUSHCORE_ANIME_SR_ARTCNN_C4F16, rtx, &why);
    r.pass = ok;
    r.verdict = ok ? "pass; " + why : (pushmpc::animeSrMissing(PUSHCORE_ANIME_SR_ARTCNN_C4F16, rtx).empty() ? "fail: " : "unavailable: ") + why;
    return r;
}
const pushmpc::selftest::Registrar kAnimeSrSelfTest({PUSHCORE_SELFTEST_ANIME_SR, "animesr", PUSHCORE_FEATURE_ANIME_SR, &testAnimeSr});
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
    uint32_t tryFg = PUSHCORE_FG_VFG, tryMultiplierCap = PUSHCORE_MAX_MULTIPLIER;
    uint32_t runFg = PUSHCORE_FG_NONE, runSr = PUSHCORE_SR_METHOD_RTX;
    // anime upscaler (PUSHCORE_FEATURE_ANIME_SR): the running one; failed = not available / stopped: RTX VSR or FSR until reopen
#if defined(PUSHMPC_HAVE_PMML) && PUSHMPC_HAVE_PMML && PUSHMPC_HAVE_EXTERNAL_SR
    std::shared_ptr<pushmpc::AnimeSr> animeSr;
#endif
    bool animeSrFailed = false;
    uint32_t builtAnimeSr = ~0u;
    bool fgFellBack = false, cutGuardOn = false;
    uint64_t cutBase = 0;                    // cuts of graphs torn down (the guard's count restarts with each graph)
    std::atomic<uint64_t> cutsSeen{0};       // cuts the guard held so far (PushCoreStats.cutGuardCuts)
    std::atomic<uint32_t> subtitleState{0}, subtitlePixels{0};   // PushFPS Subtitle Hold (PushCoreStats), read on the submit thread
    uint32_t subtitlePoll = 0;
    // Subtitle Hold Auto (settings.subtitleHold == 2): the pass detects on every frame, composes only while burned-in
    // text is found (gate below); counters of consecutive polls above / below the thresholds
    bool subAutoInit = false, subCompose = false;
    bool signsOn = false, signsCompose = false;       // anime round 5: the sign hold (the subtitle pass over the whole picture)
    bool lineNvofOn = false, lineNvofCompose = true;  // anime round 5: NVOF FG line-art protection (PushMpcLinePass)
    uint32_t subOnRun = 0, subOffRun = 0;
    std::string fgNote;
    uint32_t family = PUSHCORE_FAMILY_UNKNOWN;
    // ---- Intel video methods (PushCoreIntel.h): a D3D11 device beside the graph on the same adapter (the bridge),
    // Intel VSR on it before the graph (the graph then takes the upscaled frame, its own upscaler off), Intel VPL frame
    // interpolation on it after the graph (graph frame generation off)
    pushmpc::D3D11Bridge bridge;
    ComPtr<ID3D11Fence> fence11inBridge, fence12inBridge;   // pushcore's two fences as the bridge device sees them
    pushmpc::IntelVsrStage ivsr;
    bool ivsrOn = false; uint32_t ivsrW = 0, ivsrH = 0, ivsrFallback = PUSHCORE_SR_METHOD_FSR;
    std::string ivsrNote;
    pushmpc::VplEgress vpl;
    bool vplWanted = false, vplOn = false; uint32_t vplWant = 2;
    int64_t lastRealPts = 0;              // VPL: the previous real frame's time (the generated frames' timestamps)
    bool ensureBridge(std::string& why) {
        if (bridge.ready()) return true;
        if (!bridge.init(ctx.device(), ctx.adapter().luid, why)) return false;
        if (!bridge.importFence11(fence11.Get(), fence11inBridge, why) || !bridge.importFence12(fence12.Get(), fence12inBridge, why)) { bridge.reset(); return false; }
        veyra::log::info("pushcore", "D3D11 bridge device ready on " + narrow(bridge.adapterName()));
        return true;
    }
    // G11: RTX VSR through ZLUDA on RX 7000 / 9000 (decided at open; zludaFailed = it did not start or stopped in this
    // instance: FSR from then on)
    std::optional<pushmpc::ZludaVsrState> zvsr;
    bool zludaFailed = false;
    std::shared_ptr<pushmpc::ZludaVsrSr> zludaSr;

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
    uint32_t multiplier = 1;              // outputs per source frame (what the renderer paces by)
    uint32_t graphMultiplier = 1;         // the graph's built multiplier (> multiplier only with held-frame re-timing)
    uint32_t retimeK = 0;                 // buildFromPushMpc: multiplier when the graph is built larger for re-timing
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
    std::map<uint64_t, uint32_t> gapOf;     // graph source frame id -> source frames its pair spans (re-timed > 1)

    // ---- ingress ring ----
    static constexpr uint32_t kIngressSlots = 17;   // held-frame re-timing keeps up to kMaxQueued frames ahead of the graph and d0 behind it (12 + 3 + 2; 14 before the cascade)
    struct InSlot { ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D12Resource> res; uint64_t freeValue = 0; ComPtr<ID3D11Texture2D> br; /* on the bridge (Intel VSR) */ };
    InSlot inSlots[kIngressSlots];
    uint32_t inW = 0, inH = 0; DXGI_FORMAT inFormat = DXGI_FORMAT_UNKNOWN;
    uint64_t inIndex = 0;
    // ---- deband (PushMPC's own pass on the source, PushCoreDeband.h) ----
    pushmpc::Deband deband;
    bool debandEligible = false;           // 8-bit SDR stream and the pass initialised on this device
    int debandLogged = -1;                 // last state logged (on/off/strength), to log changes only
    bool debandWanted() const { return debandEligible && haveSettings && settings.deband != 0; }
    // ---- fast start, Route B (PUSHCORE_FLAG_QUICK_VSR, PushCoreQuickVsr.h) ----
    pushmpc::QuickVsr qvsr;
    uint32_t qvsrSrcW = 0, qvsrSrcH = 0;   // the source size before the video processor (stream holds the scaled size)
    // The quick graph asked for Route B: when upscaling is wanted, the source gets larger, this is an NVIDIA adapter and
    // the driver's VSR answers the check, the video processor scales into the ingress ring and the graph runs on the SR
    // target size with its own SR stage off. Otherwise nothing changes (the caller's settings, e.g. NVIDIA Image Scaling).
    void setupQuickVsr() {
        if (!(flags & PUSHCORE_FLAG_QUICK_VSR)) return;
        auto skip = [](const std::string& why) { veyra::log::info("quick-vsr", "not used: " + why); };
        if (!haveSettings || !settings.sr) return skip("no upscaling asked for");
        if (!ctx.adapter().isNvidia) return skip("not an NVIDIA GPU");
        const auto target = settings.srTarget == PUSHCORE_SR_TARGET_1440P ? veyra::pipeline::SrTarget::Qhd
                          : settings.srTarget == PUSHCORE_SR_TARGET_5K ? veyra::pipeline::SrTarget::Uhd5K : veyra::pipeline::SrTarget::Uhd4K;
        const auto e = veyra::pipeline::srStageExtents({stream.width, stream.height}, target).evaluated;
        if (e.width <= stream.width || e.height <= stream.height) return skip("the source is already at the target size");
        const auto t = Clock::now();
        if (!qvsr.init(d11.Get(), imm.Get(), stream.width, stream.height, e.width, e.height, stream.format == PUSHCORE_INPUT_P010,
                       stream.range, stream.matrix, stream.transfer)) {
            veyra::log::info("quick-vsr", std::format("not used ({:.0f} ms): {}", msSince(t), qvsr.why()));
            qvsr.reset();
            return;
        }
        qvsrSrcW = stream.width; qvsrSrcH = stream.height;
        stream.width = e.width; stream.height = e.height;           // the graph's source is the scaled picture
        frame->width = int(e.width); frame->height = int(e.height);
        settings.sr = 0;                                            // its SR is done; the graph keeps frame generation etc.
        veyra::log::info("quick-vsr", std::format("on in {:.0f} ms: {}", msSince(t), qvsr.why()));
    }
    // ---- held-frame re-timing (PushCoreSettings.heldFrames; HeldFrames.h, PushCoreHeld.h) ----
    pushmpc::HeldDetector held;
    bool heldEligible = false;             // the test pass initialised on this device
    bool warming = false;                  // warmUp(): synthetic frames go straight to the graph
    pushmpc::held::Retimer retimer;
    pushmpc::held::HeldRatio heldRatio{240};
    struct Pending {
        uint32_t slot = 0; uint64_t seq = 0; int64_t pts = 0; uint32_t flags = 0; uint64_t copyValue = 0;
        bool compared = false, haveTiming = false;
        PushCoreSubmitTiming timing{};
    };
    std::deque<Pending> pendingIn;         // ingested, the test result not read yet
    std::map<uint64_t, Pending> waiting;   // tested: waiting for the next distinct frame, or for the graph
    struct ReadyGraph { uint64_t seq; uint32_t multiplier, gap; };
    std::deque<ReadyGraph> readyGraph;     // to the graph, in order, when it has a slot
    static constexpr size_t kMaxQueued = pushmpc::held::kLookahead + pushmpc::held::kCascadeLookahead + 3;   // waiting held frames + the repair's lookahead (the cascade: 6) + tested + new (12; 9 before)
    int prevIngestSlot = -1;              // the ingress slot of the source frame before (the test's reference)
    int pumpError = PUSHCORE_OK;           // a graph failure while canSubmit fed the graph: the next submit returns it
    uint64_t ingestSeq = 0;
    Clock::time_point lastIngest{};
    bool heldLogged = false;
    bool retimingOn() const { return heldEligible && haveSettings && settings.heldFrames != 0 && fg && multiplier >= 2 && !warming; }
    size_t queuedFrames() const { return pendingIn.size() + waiting.size(); }
    bool slotQueued(uint32_t slot) const {
        for (const auto& p : pendingIn) if (p.slot == slot) return true;
        for (const auto& kv : waiting) if (kv.second.slot == slot) return true;
        return false;
    }
    void clearHeldQueues() { pendingIn.clear(); waiting.clear(); readyGraph.clear(); retimer.Reset(); prevIngestSlot = -1; clearRegions(); }

    // ---- per-region re-timing: source repair (HeldFrames.h PlanRepairs, PushCoreRegion.h) ----
    // Before a source frame goes to the graph, each part of it that is held between two of its drawings (d0 < s < d1, both
    // in the queue: the lookahead) is replaced by the span stream's frame of the pair d0 -> d1 at the part's phase, inside
    // the part's mask. The span stream is a second VFG effect at the source size (SDK timestep mode), fed with RGBA copies
    // of the ingress slots. The graph then runs on the repaired source as usual, at any multiplier.
    // The span stream: a second VFG effect (NVIDIA, the graph on VFG), else PushFlow (any D3D12 GPU with its runtime:
    // the graph on DLSS-G, PushFlow, FSR, NVOF or AMD FRC; pack / infer / resolve at any t). spanVfgOk: a span stream runs.
    veyra::pipeline::VfgBackend spanVfg;
    veyra::pushflow::PushFlowBackend spanFlow;
    bool spanIsFlow = false;
    bool spanVfgOk = false, repairLogged = false;
    // The lazy VFG Low span stream (HeldFrames.h LowSpanPolicy): made on a worker thread (the submit thread never waits for
    // an effect load) once the budget keeps dropping spans and >= 1 GB of video memory is free; released when idle.
    veyra::pipeline::VfgBackend spanLow;
    std::thread spanLowInit;
    std::atomic<int> spanLowState{0};                          // 0 none, 1 being made, 2 ready, 3 failed (not again in this graph)
    std::atomic<double> spanLowLoadMs{0.0};
    pushmpc::held::LowSpanPolicy lowPolicy;
    uint32_t spanQuality = 0, spanW = 0, spanH = 0; DXGI_FORMAT spanFmt = DXGI_FORMAT_UNKNOWN;
    uint64_t lowPairA = UINT64_MAX, lowPairB = UINT64_MAX; int lowParity = 0;
    uint64_t lowMade = 0, lowReleased = 0, lowParts = 0; bool lowMemLogged = false;
    uint64_t freeVideoMemory() const { uint64_t b = 0, u = 0; return ctx.videoMemoryInfo(b, u) && b > u ? b - u : 0; }
    bool lowReady() const { return spanLowState.load(std::memory_order_acquire) == 2 && !spanLowInit.joinable(); }
    void lowSpanRelease(const char* why) {
        if (spanLowInit.joinable()) spanLowInit.join();
        if (spanLowState.load() == 2) { spanLow.shutdown(); ++lowReleased; veyra::log::info("pushcore", std::format("source repair: VFG Low span stream released ({}; {} Low parts so far)", why, lowParts)); }
        if (spanLowState.load() != 3) spanLowState = 0;
        lowPairA = lowPairB = UINT64_MAX; lowParity = 0;
    }
    // once per source frame on the submit thread: finish a load, make or release the Low stream
    void lowSpanPoll() {
        using namespace pushmpc::held;
        if (!spanVfgOk || spanIsFlow || spanMid) return;
        lowPolicy.Frame();
        const int st = spanLowState.load(std::memory_order_acquire);
        if ((st == 2 || st == 3) && spanLowInit.joinable()) {
            spanLowInit.join();
            if (st == 2) { ++lowMade; lowPolicy.Created(); }
            veyra::log::info("pushcore", st == 2 ? std::format("source repair: VFG Low span stream made in {:.0f} ms (the budget dropped {} spans in the last {} frames)", spanLowLoadMs.load(), lowPolicy.RecentDrops(), kLowWindow)
                                                 : std::string("source repair: the VFG Low span stream did not start (spans over the budget stay dropped)"));
            return;
        }
        if (st == 2) { if (lowPolicy.WantRelease(true)) lowSpanRelease("no Low part for a while"); return; }
        if (st != 0 || spanQuality == 0 || lowPolicy.RecentDrops() < kLowWantDrops) return;
        const uint64_t freeB = freeVideoMemory();
        if (!lowPolicy.WantCreate(spanQuality > 0, false, false, freeB)) {
            if (!lowMemLogged) { lowMemLogged = true; veyra::log::info("pushcore", std::format("source repair: no VFG Low span stream ({} MB of video memory free, needs {} MB)", freeB >> 20, kLowMinFreeBytes >> 20)); }
            return;
        }
        spanLowState = 1;
        spanLowInit = std::thread([this] {
            const auto t = Clock::now();
            const bool ok = spanLow.initialize(ctx, runtimeDir, spanW, spanH, spanFmt, veyra::pipeline::FrameBatch::Capacity, 0u, false, false);
            if (!ok) spanLow.shutdown();
            spanLowLoadMs = msSince(t);
            spanLowState.store(ok ? 2 : 3, std::memory_order_release);
        });
    }
    pushmpc::RegionRepair repairPass;
    std::map<uint64_t, pushmpc::held::CellGrid> gridOf;
    std::map<uint64_t, pushmpc::held::RegionFrame> regionOf;   // seq -> its change against seq - 1
    std::map<uint64_t, uint32_t> slotOf;                       // seq -> ingress slot (while it is alive)
    std::map<uint64_t, bool> whollyHeld;                       // seq -> the whole frame repeats the one before (strict test)
    pushmpc::held::NoiseFloor noiseFloor;                      // the clip's cell noise: the change threshold follows it
    pushmpc::held::CadenceEvidence evidence{240};              // the live-action gate for partly held frames
    struct RgbSlot { ComPtr<ID3D12Resource> res; ComPtr<ID3D11Texture2D> tex; uint64_t seq = UINT64_MAX; uint64_t readyValue11 = 0; };
    std::vector<RgbSlot> rgbSlots; uint32_t rgbNext = 0;
    RgbSlot repSlots[pushmpc::RegionRepair::kMaxParts];        // the span stream's frames of this source frame's parts
    // Midpoint-only span streams (2x black boxes; HeldFrames.h PlanMidpoints): AMD AMF FRC (AMD driver) or FSR 3.1 frame
    // generation (any D3D12 GPU, the pair call of the combined patch's anime FSR hunks), where neither VFG nor PushFlow's runtime runs.
    // Twos exact (t 0.5); threes from the pair's midpoint M (cached for both held frames): mid(d0, M) 0.25 / mid(M, d1) 0.75.
    int spanMid = 0;                                           // 0 none, 1 AMD FRC, 2 FSR 3.1
    veyra::gfx::AmfFrcBackend spanFrc;
#ifdef VEYRA_FSR_PAIR_API
    veyra::gfx::FsrFgPresenter spanFsr;
#endif
#if defined(VEYRA_FSR_PAIR_API) && defined(PUSHMPC_NVOF_LINE_PASS) && PUSHMPC_NVOF_LINE_PASS >= 2
#define PUSHMPC_SPAN_LINE 1
    // line art on the FSR midpoint stream (the combined patch's NVOF line hunks, PushMpcLinePass::composePair): FidelityFX blends two
    // drawings 50/50 at a hard jump; per tile the doubled lines become the older drawing's (no motion here: FSR estimates
    // its own). With the Line art setting, on animation, while the budget has room.
    veyra::pipeline::PushMpcLinePass spanLine;
    veyra::pipeline::DescriptorStager spanLineStager;
    bool spanLineOk = false;
    uint64_t spanLineCalls = 0;
#endif
    ComPtr<ID3D12Resource> fsrMotion, fsrDepth;                // zero motion (R16G16 float) and depth (R32 float): FSR estimates its own flow
    RgbSlot midSlot; uint64_t midA = UINT64_MAX, midB = UINT64_MAX;   // the cached midpoint of the pair (midA, midB)
    uint64_t midLast = UINT64_MAX;                             // the picture the midpoint stream holds as its previous input (a key)
    int64_t midPts = 0;
    uint64_t midCalls = 0, midApprox = 0;                      // midpoint calls; parts at an approximate phase (threes)
    double midErrSum = 0;                                      // their phase error (sum, in spans)
    // ---- the planner thread: classification (CompareRegions) and PlanRepairs off the submit thread (3-4 + 2-9 ms per
    // source frame, measured offline). It owns gridOf, regionOf, whollyHeld, noiseFloor and cadenceHist; the submit thread
    // queues jobs and takes finished plans. A seek bumps planEpoch: the thread then starts over.
    struct PlanJob { uint64_t seq = 0, epoch = 0; bool plan = false, wholly = false; int64_t pts = INT64_MIN; pushmpc::held::CellGrid grid; };
    struct PlanResult { std::vector<pushmpc::held::RepairGroup> groups; bool wholly = false; float cellPx = 1.f; pushmpc::held::HoldPlan hold[2]; };
    std::thread planner;
    std::mutex planMx;
    std::condition_variable planCv;
    std::deque<PlanJob> planJobs;
    std::map<uint64_t, PlanResult> plans;
    uint64_t planEpoch = 0, lastQueued = 0, lastPlanQueued = 0; bool anyQueued = false, anyPlanQueued = false, planStop = false;
    pushmpc::held::CellCadence cadenceHist;
    // ---- anime round 5: the hold compose (HoldPlan.h, PushCoreHold.h) ----
    // Per side (two only with the verdict split PUSHMPC_SPLIT): the animation mode and hold flags. holdOn: plans are made
    // (planner thread: block match, objects, transients, plans; submit thread: compose after the graph, before egress).
    pushmpc::HoldCompose holdPass;
    bool holdOn = false, holdSplit = false;
    pushmpc::held::AnimMode holdMode[2]{};
    uint32_t holdFlg[2]{};
    uint64_t holdSwapAt = UINT64_MAX;                            // PUSHMPC_AB_SWAP=<source frame>: the sides swap there
    std::atomic<float> holdCellPx{4.f}, holdCoverW{1.f}, holdCoverH{1.f};
    std::map<uint64_t, float> meanOf;                           // planner thread: grid means
    std::map<uint64_t, std::vector<pushmpc::held::ObjectClass>> objOf;
    std::map<uint64_t, pushmpc::held::TransientMarks> marksOf;
    std::map<uint64_t, std::array<pushmpc::held::HoldPlan, 2>> holdOf;
    std::vector<int16_t> slidX, slidY;
    pushmpc::held::ShowClassifier showClass;
    std::atomic<uint32_t> animClassNow{0};
    std::atomic<bool> holdAnimNow{false};
    std::atomic<double> holdMatchMs{0.0};                       // planner CPU per pair for the block match (running mean)
    std::map<uint64_t, std::pair<float, float>> camFixOf;      // planner thread: MatchBlocks' camera correction per pair (compose warp)
    pushmpc::held::FilmTimeDetector filmTime;                  // P9 (off unless PUSHCORE_HOLD_FILM_TIME): 24p-grid drawings in 29.97
    std::map<uint64_t, float> filmSwitchOf;                    // per pair: the drawn layer's film-time switch t (-1: none)
    std::map<uint64_t, int64_t> ptsOf;                         // planner thread: the frames' submitted pts (100 ns)
    // the pair (seq - 1, seq)'s own interval from the submitted timestamps (a VFR file tagged 29.97 has 1/24 s frames in
    // its 23.976 body); the stream's rate when a timestamp is missing or not increasing
    double pairIntervalMs(uint64_t seq) const {
        const auto a = ptsOf.find(seq - 1), b = ptsOf.find(seq);
        if (seq == 0 || a == ptsOf.end() || b == ptsOf.end() || a->second == INT64_MIN || b->second == INT64_MIN || b->second <= a->second) return sourceIntervalMs();
        return double(b->second - a->second) / 10000.0;
    }
    std::atomic<uint64_t> holdScrollPairs{0};                   // pairs with a scrolling layer (credits; ScrollPair), planner thread
    // Budget (research §9): line-art protection and the sign hold are dropped before anything else, before the filter's
    // admission has to lower the multiplier: shed while the graph's p95 + their reserve is over 85 % of a source frame,
    // back once it is under 75 %. holdShedPairs counts the pairs without them.
    static constexpr double kHoldReserveMs = 1.5;                // line-art compose at 8x 4K (1-2 ms) + sign FIND (0.3-0.6 ms), priors
    std::atomic<bool> holdShed{false};
    uint64_t holdShedPairs = 0;
    void holdBudget() {
        const double graphP95 = costFull.pct(0.95), interval = sourceIntervalMs();
        if (graphP95 <= 0) return;
        const bool shed = holdShed.load(std::memory_order_relaxed);
        if (!shed && graphP95 + kHoldReserveMs > 0.85 * interval) {
            holdShed = true;
            veyra::log::info("pushcore", std::format("budget: line-art protection and sign hold off (graph p95 {:.1f} ms + {:.1f} ms over 85 % of {:.1f} ms), before the multiplier", graphP95, kHoldReserveMs, interval));
        } else if (shed && graphP95 + kHoldReserveMs < 0.75 * interval) {
            holdShed = false;
            veyra::log::info("pushcore", std::format("budget: line-art protection and sign hold back (graph p95 {:.1f} ms)", graphP95));
        }
    }
    std::map<uint64_t, std::array<pushmpc::held::HoldPlan, 2>> holdReady;   // submit thread: plans taken with their frame
    ID3D12Resource* holdPrevReal = nullptr;
    uint64_t holdPairs = 0, holdFrames = 0, holdFlashes = 0, holdLineFrames = 0, holdFail = 0, holdGated = 0;
    bool holdRepairWanted() const { return !holdOn || holdMode[0] == pushmpc::held::AnimMode::Full || holdMode[1] == pushmpc::held::AnimMode::Full; }
    bool holdRetimeWanted() const { return !holdOn || (holdMode[0] == pushmpc::held::AnimMode::Full && holdMode[1] == pushmpc::held::AnimMode::Full); }
    std::atomic<float> planThr{pushmpc::held::kCellChangeCodes}, planNoise{0.f};
    ComPtr<ID3D11Texture2D> repairOut;                         // NV12 / P010 with plane UAVs
    uint64_t pairA = UINT64_MAX, pairB = UINT64_MAX; int pairParity = 0;   // what the span stream holds now
    ComPtr<ID3D12CommandAllocator> regionAlloc[8];
    uint64_t regionAllocValue[8]{};
    ComPtr<ID3D12GraphicsCommandList> regionList;
    uint32_t regionListNext = 0, regionCurAlloc = 0;
    uint64_t repairedFrames = 0, repairedParts = 0, repairedWholly = 0, repairSubstitutions = 0, repairShifted = 0, repairCarved = 0;
    uint64_t repairWhy[9]{};   // spans / frames not repaired: gate (no animation evidence), budget (graph over it), no part, no RGB copy, capture, generate, copy, composite; 8: budget (one new span per frame)
    // the repair's GPU time: D3D12 timestamps around its span-stream work, per repaired frame (16 in flight)
    ComPtr<ID3D12QueryHeap> repairQueries; ComPtr<ID3D12Resource> repairQueryBuf; const uint64_t* repairQueryMap = nullptr;
    uint64_t repairQueryValue[16]{}; uint32_t repairQueryNext = 0; int repairQueryCur = -1; double tsPerMs = 0;
    CostWindow costRepair;
    double pendingRepairMs = 0;   // the repair queued for the frame that goes to the graph next (admission's queue prediction)
    std::map<std::tuple<uint64_t, uint64_t, uint64_t>, uint8_t> spanDecision;   // (d0, d1, pair start) -> SpanRoute for the whole span (drop, main, Low)
    std::vector<uint8_t> routes;                               // repairWith: the route (SpanRoute) of each kept part
    std::set<uint64_t> repairedAhead;                          // frames repaired as a future end (the cascade), not yet at the graph
    uint64_t cascadeEnds = 0, cascadeMissed = 0;               // future ends repaired first / without a plan in time
    // the full cascade (research P8); PUSHMPC_REPAIR_CASCADE=0 turns it off (the queue then still looks 6 ahead)
    bool cascadeOn() const { const char* e = std::getenv("PUSHMPC_REPAIR_CASCADE"); return !(e && std::atoi(e) == 0); }
    // Repair mode (VFG, the span stream running): held frames are repaired in place and every source frame goes to the
    // graph with k outputs; nothing is dropped and no pair spans more than one source frame.
    bool repairMode() const { return retimingOn() && spanVfgOk && repairPass.ready(); }
    bool repairOn() const { return repairMode(); }
    // The live-action gate for PARTLY held frames (a wholly held frame is always repaired): animation repeats its drawings
    // to the noise floor, live action never does (HeldFrames.h CadenceEvidence). Animation On (heldFrames 2) skips it.
    bool animationGate() { return settings.heldFrames >= 2 || evidence.Animation(); }
    bool repairDetailOff() const { const char* e = std::getenv("PUSHMPC_REPAIR_DETAIL"); return e && std::atoi(e) == 0; }
    bool repairEnvOff() const { const char* e = std::getenv("PUSHMPC_REGION_REPAIR"); return e && std::atoi(e) == 0; }
    // this graph will run the source repair (decided before the build: the graph is then built at k)
    bool repairPlanned() const {
        if (!haveSettings || settings.heldFrames == 0 || !heldEligible || repairEnvOff()) return false;
        return (tryFg == PUSHCORE_FG_VFG && ctx.adapter().vendorId == 0x10DE) || veyra::pushflow::runtimeInstalled(runtimeDir) || midStreamPossible();
    }
    // a midpoint span stream can start: AMD FRC on an AMD GPU with the AMF runtime, else FSR 3.1's pair call
    bool midStreamPossible() const {
        if (ctx.adapter().vendorId == 0x1002 && veyra::gfx::AmfFrcBackend::runtimeInstalled()) return true;
#ifdef VEYRA_FSR_PAIR_API
        return true;
#else
        return false;
#endif
    }
    double sourceIntervalMs() const { return stream.fpsNum > 0 && stream.fpsDen > 0 ? 1000.0 * double(stream.fpsDen) / double(stream.fpsNum) : 1000.0 / 23.976; }
    void clearRegions() {
        { std::lock_guard<std::mutex> lk(planMx); ++planEpoch; planJobs.clear(); plans.clear(); }
        anyQueued = anyPlanQueued = false; lastQueued = lastPlanQueued = 0;
        slotOf.clear(); spanDecision.clear(); repairedAhead.clear(); pairA = pairB = UINT64_MAX; lowPairA = lowPairB = UINT64_MAX; for (auto& r : rgbSlots) r.seq = UINT64_MAX;
    }
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
        // Intel VPL (after the graph): the frames between the previous real frame and this one, handed out before it
        bool vplReset = false, vplStarted = false, vplDone = false;
        std::vector<pushmpc::VplEgress::Gen> vplGen;
        uint64_t vplReady = 0; int64_t prevPts = 0; uint32_t vplNext = 0; int32_t fgLevel = 0;
    };
    std::deque<Batch> batches;
    struct Lease { std::shared_ptr<veyra::pipeline::FrameLease> graphLease; uint32_t kind = 0, parity = 0; int outSlot = -1; int xefgSlot = -1, vplSlot = -1; };
    std::map<uint64_t, Lease> leases;
    uint64_t nextLease = 1;
    // G10: the XeFG route (fgBackend XEFG): the stage (it owns the outputs), pushcore's own cut guard on the real frames
    // handed to XeFG, copies of the last real frames (the guard holds A / B over a cut pair's generated frames)
    std::unique_ptr<pushmpc::xess::XefgStage> xefg;
    std::unique_ptr<veyra::pipeline::CutGuard> xguard;
    static constexpr uint32_t kXReal = 4, kXLists = 8;
    ComPtr<ID3D12Resource> xreal[kXReal]; uint64_t xrealSeq[kXReal]{}; uint64_t xguardSeq[2]{};
    ComPtr<ID3D12CommandAllocator> xalloc[kXLists]; uint64_t xallocValue[kXLists]{}; ComPtr<ID3D12GraphicsCommandList> xlist; uint32_t xnext = 0;
    int64_t xlastPts = INT64_MIN; uint64_t xseq = 0, xholds = 0, xholdsSkipped = 0, xlastGraphSource = 0;
    bool srXessFailed = false;              // XeSS SR did not start in this instance: FSR from then on
    bool xessSrOn = false;                  // XeSS SR runs in the graph built last
    uint64_t sequence = 0, submittedCount = 0;
    bool haveReal = false, needReset = true;

    PushCoreStats st{};
    std::deque<PushCoreGpuTiming> timings;
    std::deque<double> ivsrMs, vplMs;    // Intel VSR blit GPU ms, VPL ms per source frame (wall time of its job)
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
    // measured): the bridge starts from EnhancementSettings{} with nrRuntime Ampere, in
    // List chain mode (runtimeOrder = nullopt); "smoother" sets multiplier (the FG node on) and content Auto; "sharper"
    // sr on with the current videoSrQuality (standard RTX VSR family); "AI picture enhancement" turns the list's NR
    // node on with its default parameters (one layer: flat fields, nrLayerCount 0) and PushQuickSetup's look writes
    // model.tone/structure; "SDR to HDR" videoHdr.enabled with VideoHdrSettings' defaults. One deliberate difference:
    // the frame generation backend is NVIDIA VFG (PushFPS's fresh default on NVIDIA is DLSS-G, which PushMPC does not
    // ship; VFG is the video interpolation and goes up to 16X here: 8X in the SDK multiplier mode, above in its timestep mode).
    bool buildFromPushMpc(veyra::engine::EnhancementSettings& s, std::optional<veyra::engine::ChainRuntimeOrder>& order) {
        using namespace veyra::engine;
        s = EnhancementSettings{};
        s.nrRuntime = NrRuntime::Ampere;
        const uint32_t vendor = ctx.adapter().vendorId;
        const bool nvidia = vendor == 0x10DE, amd = vendor == 0x1002;
        // frame generation method (tryFg: the asked-for one, or the next of the fallback chain)
        uint32_t cap = PUSHCORE_MAX_MULTIPLIER;   // VFG
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
            cap = PUSHCORE_MAX_MULTIPLIER;   // flow once above 4x (the PushFlow 16x hunks of patches/pushfps-4.5.4-pushmpc.patch)
            break;
        case PUSHCORE_FG_XEFG:
            // G10: PushFPS's XeSS present-sink mode (the graph makes the real frames at the output size and the motion its
            // XeSS presenter gives XeFG: the refined optical flow, FidelityFX on any GPU, NVIDIA's on NVIDIA); pushcore's
            // XefgStage then runs XeFG on its headless swap chain. 2x..8x (above the runtime's own ceiling: the MFG unlock)
            s.frameGenerationBackend = FrameGenerationBackend::XeSS;
            s.opticalFlowBackend = nvidia ? OpticalFlowBackend::Nvidia : OpticalFlowBackend::AmdFidelityFx;
            s.fgMotionRefine = true;
            cap = 8;
            break;
        case PUSHCORE_FG_NVOF:
            // NVIDIA's optical-flow hardware as the motion, FSR 3.1's interpolation and warp (T5b, unlock NvofWarp): the
            // flow runs on the NVOFA engine, the refined vectors feed FidelityFX frame interpolation; 2x
            s.frameGenerationBackend = FrameGenerationBackend::Fsr;
            s.opticalFlowBackend = OpticalFlowBackend::Nvidia;
            s.fgMotionRefine = true;
            cap = 2;
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
        case PUSHCORE_FG_VPL:
            // Intel VPL AI frame interpolation runs after the graph (VplEgress, buildGraphOnce); the graph itself makes no
            // frames (multiplier 1 below)
            s.frameGenerationBackend = FrameGenerationBackend::PushFlow;
            cap = 4;
            break;
        case PUSHCORE_FG_HELIXFG:
            // HelixSR frame generation: PushFPS's direct DLSS-G provider path (Anr) on HelixSR's provider DLL, fed like
            // DLSS-G (FidelityFX optical flow, refined motion, NVIDIA's motion convention); up to 6x
            s.frameGenerationBackend = FrameGenerationBackend::Anr;
            s.opticalFlowBackend = OpticalFlowBackend::AmdFidelityFx;
            s.fgMotionRefine = true;
            cap = 6;
            break;
        default:
            s.frameGenerationBackend = FrameGenerationBackend::Vfg;
            break;
        }
        cap = std::min(cap, tryMultiplierCap);
        // VFG: the NvVFX model (0..2); PushFlow: its tier (EnhancementSettings keeps PushFlow's tier in vfgQuality)
        s.vfgQuality = tryFg == PUSHCORE_FG_PUSHFLOW ? std::min<uint32_t>(settings.fgTier, 2u) : std::min<uint32_t>(settings.vfgQuality, 2u);
        s.multiplier = settings.multiplier >= 2 && tryFg != PUSHCORE_FG_NONE ? std::min<uint32_t>(settings.multiplier, cap) : 1u;
        if (settingsProbe) { if (vplOn || vplWanted) s.multiplier = 1; }   // a live settings check: the stages stay as built
        else {
            vplWanted = tryFg == PUSHCORE_FG_VPL && s.multiplier >= 2;
            if (vplWanted) { vplWant = s.multiplier; s.multiplier = 1; }
        }
        if (s.multiplier > 1) s.content = ContentRate::Auto;   // WatchPage ensureVideoCadence (ignored for files)
        // held-frame re-timing: the graph is built with the multiplier the longest re-timed span can use (HeldFrames.h
        // BuildMultiplier; the engine's batch capacity and the method's cap bound it); each pair then asks for its own
        // With the source repair (VFG) every source frame goes to the graph with k outputs: no larger graph and no long
        // re-timed calls (those came in bursts: at 5x a span of three was one call of 15 outputs, p99 92 ms per call, and
        // inside such a span the outputs fell to a third of the rate). The larger graph stays for the other methods.
        retimeK = 0;
        if (s.multiplier >= 2 && settings.heldFrames != 0 && !repairPlanned()) {
            const uint32_t build = pushmpc::held::BuildMultiplier(s.multiplier, std::min<uint32_t>(cap, veyra::pipeline::FrameBatch::Capacity));
            if (build > s.multiplier) {
                veyra::log::info("pushcore", std::format("held-frame re-timing: graph built for {}x (spans of held frames), {}x per source frame", build, s.multiplier));
                retimeK = s.multiplier; s.multiplier = build;
            }
        }
        s.sr = settings.sr != 0;
        if (settings.srQuality >= 1 && settings.srQuality <= 4) s.videoSrQuality = settings.srQuality;
        // the upscaler: RTX VSR, or AMD FSR / NVIDIA Image Scaling (any GPU; on a non-NVIDIA GPU RTX VSR becomes FSR)
        runSr = settings.srMethod;
        // the anime upscaler: in the RTX VSR slot (standard quality); without its files or after a failure RTX VSR / FSR
        if (runSr == PUSHCORE_SR_METHOD_PMML) {
            const std::string miss = animeSrFailed ? std::string("it stopped earlier in this stream") : pushmpc::animeSrMissing(settings.animeSrModel, nvidia && rtxFamily(family));
            if (!miss.empty()) {
                veyra::log::warn("pushcore", std::format("anime upscaler {} not used: {}", pushmpc::animeSrModelId(settings.animeSrModel), miss));
                runSr = PUSHCORE_SR_METHOD_RTX;
            }
        }
        // G11: on an RX 7000 / 9000 "RTX VSR" (or ZLUDA VSR asked for by name) runs through ZLUDA when the unlock is on
        // (its self-test passed, or forced on) and it has not failed in this instance; else FSR, as on any non-NVIDIA GPU
        if (!nvidia && (runSr == PUSHCORE_SR_METHOD_RTX || runSr == PUSHCORE_SR_METHOD_ZLUDA_VSR))
            runSr = zvsr && zvsr->elig.files && zvsr->decision.on && !zludaFailed ? PUSHCORE_SR_METHOD_ZLUDA_VSR : PUSHCORE_SR_METHOD_FSR;
        if (nvidia && runSr == PUSHCORE_SR_METHOD_ZLUDA_VSR) runSr = PUSHCORE_SR_METHOD_RTX;   // NVIDIA runs RTX VSR natively
        // Intel VSR: the driver's video processor before the graph, to PushFPS's SR size (srStageExtents); the graph runs
        // without its own upscaler. Not an Intel GPU, or the stage does not start: FSR (NIS without the FidelityFX files).
        if (settingsProbe) { if (ivsrOn) s.sr = false; else if (runSr == PUSHCORE_SR_METHOD_IVSR) runSr = ivsrFallback; }
        else { ivsrOn = false; ivsrNote.clear(); }
        if (!settingsProbe && runSr == PUSHCORE_SR_METHOD_IVSR) {
            std::string why;
            const auto target = settings.srTarget == PUSHCORE_SR_TARGET_1440P ? veyra::pipeline::SrTarget::Qhd
                              : settings.srTarget == PUSHCORE_SR_TARGET_5K ? veyra::pipeline::SrTarget::Uhd5K : veyra::pipeline::SrTarget::Uhd4K;
            const auto ext = veyra::pipeline::srStageExtents({stream.width, stream.height}, target);
            if (!s.sr || !ext.enlarges()) {
                why = "nothing to upscale"; s.sr = false;   // Intel VSR only enlarges (no artifact reduction at the source size)
            } else if (vendor != 0x8086) {
                why = "not an Intel GPU";
            } else if (pmu::ReadUnlockMode(pmu::Unlock::IntelVsr) == pmu::UnlockMode::Off) {
                why = "switched off (PUSHMPC_DISABLE_INTELVSR / Unlocks IntelVsr = 0)";
            } else if (ensureBridge(why)) {
                const DXGI_FORMAT f = stream.format == PUSHCORE_INPUT_P010 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
                const bool bt709 = stream.matrix != AVCOL_SPC_BT470BG && stream.matrix != AVCOL_SPC_SMPTE170M;
                if (ivsr.init(bridge, stream.width, stream.height, ext.evaluated.width, ext.evaluated.height, f, bt709, stream.range != AVCOL_RANGE_JPEG,
                              kIngressSlots, why)) {
                    ivsrOn = true; ivsrW = ext.evaluated.width; ivsrH = ext.evaluated.height;
                    s.sr = false;   // the graph gets the upscaled frame
                    ivsrNote = std::format("Intel VSR {}x{} -> {}x{} (driver video processor, super resolution on)", stream.width, stream.height, ivsrW, ivsrH);
                    veyra::log::info("pushcore", ivsrNote);
                }
            }
            if (!ivsrOn && s.sr) {
                const std::filesystem::path fsr = veyra::runtime::localDataDirectory() / L"amd" / L"fidelityfx";
                runSr = fileExists(fsr / L"amd_fidelityfx_upscaler_dx12.dll") ? PUSHCORE_SR_METHOD_FSR : PUSHCORE_SR_METHOD_NIS;
                ivsrFallback = runSr;
                veyra::log::warn("pushcore", std::format("Intel VSR did not start ({}): {} instead", why, runSr == PUSHCORE_SR_METHOD_FSR ? "FSR" : "NIS"));
            }
        }
        if (runSr == PUSHCORE_SR_METHOD_ZLUDA_VSR) {
            // the standard RTX VSR family at the asked tier (videoSrQuality 1..4); the graph hands the pass to the worker
            if (!veyra::engine::ngxVideoSr(s.videoSrQuality)) s.videoSrQuality = 4;
        } else if (runSr == PUSHCORE_SR_METHOD_FSR) {
            s.videoSrQuality = kVideoSrFsr;   // FidelityFX picks FSR 4.1 (ML / INT8) where the driver offers it
            s.opticalFlowBackend = OpticalFlowBackend::AmdFidelityFx;   // FSR is temporal: its motion from FidelityFX flow
        } else if (runSr == PUSHCORE_SR_METHOD_NIS) {
            s.videoSrQuality = kVideoSrNis;
        }
        if (runSr == PUSHCORE_SR_METHOD_XESS) {
            // G10: Intel XeSS SR in the graph's temporal upscaler slot (FSR's: the same flow motion, depth, reset rule and
            // frame time), through FsrSrBackend's provider override, set around the graph's initialize. It did not start in
            // this instance, or this build has no slot for it: FSR (the same graph settings)
            if (srXessFailed || !pushmpc::xess::srProviderBuilt()) runSr = PUSHCORE_SR_METHOD_FSR;
            s.videoSrQuality = kVideoSrFsr;
            s.opticalFlowBackend = OpticalFlowBackend::AmdFidelityFx;
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
        // G11: RTX VSR through ZLUDA (RX 7000 / 9000): the unlock's decision, and its self-test in the background when this
        // adapter + driver has no result yet (only while a player has PushMPC open; FSR until it has passed)
        if (ad.VendorId == 0x1002 && pmu::ZludaEligibleFamily(family)) {
            DXGI_ADAPTER_DESC1 d1{};
            if (findAdapter(luid, d1)) {
                zvsr = pushmpc::zludaVsrState(d1, family, driverVersionOf(d1.AdapterLuid), std::filesystem::path(runtimeDir), moduleDirectory());
                veyra::log::info("pushcore", "unlock " + narrow(zvsr->decision.why) + (zvsr->elig.why.empty() ? std::string() : " [" + zvsr->elig.why + "]"));
                if (zvsr->decision.runSelfTest) {
                    std::string why;
                    const uint32_t scale = std::clamp<uint32_t>(stream.height ? (2160u + stream.height - 1) / stream.height : 2u, 1u, 4u);
                    if (!pushmpc::zludaVsrStartSelfTest(*zvsr, stream.width, stream.height, scale, why)) veyra::log::info("pushcore", "ZludaVsr self-test not started: " + why);
                }
            }
        }
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
        // one command list per generated frame plus the rest of the graph, two source frames in flight: 16 slots were enough
        // for 8x, VFG 16x needs 32 (with 16 the ring wrapped inside one source frame and submit waited on the GPU: 35 ms at 16x)
        if (!ring.initialize(ctx.device(), ctx.directQueue(), ctx.fence(), ctx.fenceEvent(), 16 + 2 * (PUSHCORE_MAX_MULTIPLIER - 8), status))
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
        // Held-frame test: prepared for every stream (8-bit and 10-bit); it runs only while re-timing is asked for
        heldEligible = held.init(d11.Get());
        if (!heldEligible) veyra::log::warn("pushcore", "held-frame test unavailable (no re-timing): " + held.why());
        return PUSHCORE_OK;
    }

    // DLSS-G on the community port (RTX 20 / GTX 16: unlock TuringDlssg, the only DLSS-G there; RTX 30: unlock AmpereDlssg,
    // next to Transfusion): allowed while its self-test is pending or passed; the first DLSS-G graph on the port on a new
    // adapter + driver is the self-test (recordPortSelfTest)
    static constexpr uint64_t kPortSelfTestFrames = 240;
    bool portGraph = false, portRecorded = false, portGuardSet = false;
    uint64_t portValidGenerated = 0;
    LUID adapterLuid() const { const uint64_t v = ctx.adapter().luid; LUID l{}; l.LowPart = DWORD(v & 0xFFFFFFFFu); l.HighPart = LONG(v >> 32); return l; }
    pmu::Unlock portUnlock() const { return portUnlockFor(ctx.adapter().vendorId, ctx.adapter().deviceId, family); }
    bool portDlssgAllowed() const {
        const pmu::Unlock u = portUnlock();
        if (u == pmu::Unlock::Count) return false;
        const pmu::UnlockDecision d = portDlssgDecision(u, ctx.adapter().vendorId, ctx.adapter().deviceId, adapterLuid(), pmport::PortFilesPresent(runtimeDir));
        return d.on || d.runSelfTest;
    }
    std::wstring adapterKey() const { return pmu::SelfTestKey(ctx.adapter().vendorId, ctx.adapter().deviceId, driverVersionOf(adapterLuid())); }
    void recordPortSelfTest(bool pass, const std::string& why) {
        if (portRecorded) return;
        portRecorded = true;
        const pmu::Unlock u = portUnlock();
        if (portGuardSet) { setPortRunning(u, {}); portGuardSet = false; }
        const std::wstring key = adapterKey();
        // an earlier result for this adapter + driver stands (written once; a new driver tests again)
        if (pmu::ReadSelfTest(u, key).known) return;
        pmu::WriteSelfTest(u, key, pass, std::wstring(why.begin(), why.end()));
        veyra::log::info("pushcore", std::format("self-test {}: {} ({})", narrow(pmu::UnlockName(u)), pass ? "pass" : "FAIL", why));
    }

    // HelixSR self-tests (frame generation: 240 valid generated frames; HLX FSR 4: 240 frames with the FSR upscaler
    // measured), each with the crash guard of the port's self-test (SelfTest\<Name> "Running")
    struct CommunityTest { pmu::Unlock u; bool active = false, recorded = false, guard = false; uint64_t count = 0, seen = 0; };
    CommunityTest helixFgTest{pmu::Unlock::HelixFg}, hlxFsrTest{pmu::Unlock::HlxFsr4};
    bool helixFgGraph = false, hlxGraph = false;
    bool communityAllowed(pmu::Unlock u, bool files) const {
        const pmu::UnlockDecision d = communityDecision(u, ctx.adapter().vendorId, ctx.adapter().deviceId, adapterLuid(), files, L"not installed");
        return d.on || d.runSelfTest;
    }
    bool helixFgAllowed() const { return helixFgGpu(ctx.adapter().vendorId, family) && communityAllowed(pmu::Unlock::HelixFg, pmhelix::FgPresent(veyra::runtime::localDataDirectory())); }
    // false: an earlier self-test of this unlock ended the process (recorded as a failure now)
    bool testBegin(CommunityTest& t) {
        t.active = true; t.count = 0; t.seen = 0;
        if (t.recorded || t.guard) return true;
        const std::wstring key = adapterKey();
        if (pmu::ReadSelfTest(t.u, key).known) return true;
        if (portRunningKey(t.u) == key) { testRecord(t, false, "an earlier self-test did not finish (the player closed unexpectedly)"); setPortRunning(t.u, {}); return false; }
        setPortRunning(t.u, key);
        t.guard = true;
        return true;
    }
    void testRecord(CommunityTest& t, bool pass, const std::string& why) {
        if (t.recorded) return;
        t.recorded = true;
        if (t.guard) { setPortRunning(t.u, {}); t.guard = false; }
        const std::wstring key = adapterKey();
        if (pmu::ReadSelfTest(t.u, key).known) return;
        pmu::WriteSelfTest(t.u, key, pass, std::wstring(why.begin(), why.end()));
        veyra::log::info("pushcore", std::format("self-test {}: {} ({})", narrow(pmu::UnlockName(t.u)), pass ? "pass" : "FAIL", why));
    }
    void testEnd(CommunityTest& t) {   // clean close before a result: not a failure, the next open tests again
        if (t.guard && !t.recorded) { setPortRunning(t.u, {}); t.guard = false; }
        t.active = false;
    }

    // RTX 30: which DLSS-G backend runs (pmu::ChooseDlssgRoute), its per-backend record (runs, fails, crashes, GPU ms per
    // generated frame at 4K), and the crash guard of the backend whose graph runs. One backend per process (gDlssgLatch).
    pmu::DlssgRoute routeGraph = pmu::DlssgRoute::None;   // the backend of the current graph (RTX 30 DLSS-G only)
    pmu::DlssgRoute routeForce = pmu::DlssgRoute::None;   // this build attempt: the other backend after a failed start
    uint32_t routeTried = 0;                              // bit 1 << route: backends that failed to start in this open
    bool routeCrashesChecked = false;
    std::vector<double> routeSamples;                     // fgBatchMs / (M - 1) at 4K, full-batch frames of this graph
    bool transfusionUsable() const {
        wchar_t v[4]{};
        if (GetEnvironmentVariableW(L"VEYRA_DISABLE_AMPERE_MFG_UNLOCK", v, 4) > 0 && gDlssgLatch.load() != int(pmu::DlssgRoute::Port)) return false;
        return fileExists(std::filesystem::path(runtimeDir) / L"nvngx_dlssg.dll");
    }
    pmu::DlssgRouteChoice chooseRoute(bool wantLite) {
        const std::wstring key = adapterKey();
        if (!routeCrashesChecked) {   // a backend's graph was running when its process ended: count a crash, once
            routeCrashesChecked = true;
            for (pmu::DlssgRoute r : { pmu::DlssgRoute::Transfusion, pmu::DlssgRoute::Port }) {
                std::wstring running;
                pmu::DlssgRouteRecord rec = pmu::ReadDlssgRoute(r, key, &running);
                if (running.empty() || gDlssgLatch.load() != int(pmu::DlssgRoute::None)) continue;
                if (running == key) { ++rec.crashes; pmu::WriteDlssgRoute(r, key, rec); veyra::log::warn("pushcore", std::format("DLSS-G backend {}: the last player process ended while it ran (crash {})", narrow(pmu::DlssgRouteName(r)), rec.crashes)); }
                pmu::SetDlssgRouteRunning(r, {});
            }
        }
        pmu::DlssgRouteInputs in;
        const pmu::UnlockDecision pd = portDlssgDecision(pmu::Unlock::AmpereDlssg, ctx.adapter().vendorId, ctx.adapter().deviceId, adapterLuid(), pmport::PortFilesPresent(runtimeDir));
        in.portUsable = pd.on || pd.runSelfTest;
        in.portPassed = pd.on && !pd.runSelfTest;
        in.portForced = pmu::ReadUnlockMode(pmu::Unlock::AmpereDlssg) == pmu::UnlockMode::On;
        in.transfusionUsable = transfusionUsable();
        in.wantLite = wantLite;
        in.latch = pmu::DlssgRoute(gDlssgLatch.load());
        in.transfusion = pmu::ReadDlssgRoute(pmu::DlssgRoute::Transfusion, key);
        in.port = pmu::ReadDlssgRoute(pmu::DlssgRoute::Port, key);
        pmu::DlssgRouteChoice c = pmu::ChooseDlssgRoute(in);
        if (routeForce != pmu::DlssgRoute::None) { c.route = routeForce; c.lite = wantLite && routeForce == pmu::DlssgRoute::Port; c.why = L"the other backend after a failed start"; }
        veyra::log::info("pushcore", std::format("RTX 30 DLSS-G backend: {}{} ({})", narrow(pmu::DlssgRouteName(c.route)), c.lite ? " FG Lite" : "", narrow(c.why)));
        return c;
    }
    // the other backend may still start in this process after `failed` did not start
    pmu::DlssgRoute otherRouteAfterFailure(pmu::DlssgRoute failed) const {
        if (failed == pmu::DlssgRoute::None) return pmu::DlssgRoute::None;
        const pmu::DlssgRoute other = failed == pmu::DlssgRoute::Port ? pmu::DlssgRoute::Transfusion : pmu::DlssgRoute::Port;
        if (routeTried & (1u << int(other))) return pmu::DlssgRoute::None;
        const int latch = gDlssgLatch.load();
        if (latch != int(pmu::DlssgRoute::None) && latch != int(other)) return pmu::DlssgRoute::None;   // the failed one already touched the process
        if (failed == pmu::DlssgRoute::Transfusion && !veyra::ngx::FgCompatibilitySession::processHealthy()) return pmu::DlssgRoute::None;
        if (other == pmu::DlssgRoute::Port && !portDlssgAllowed()) return pmu::DlssgRoute::None;
        if (other == pmu::DlssgRoute::Transfusion && !transfusionUsable()) return pmu::DlssgRoute::None;
        return other;
    }
    void routeFailed(const std::string& why) {
        if (routeGraph == pmu::DlssgRoute::None) return;
        const std::wstring key = adapterKey();
        pmu::DlssgRouteRecord rec = pmu::ReadDlssgRoute(routeGraph, key);
        ++rec.fails;
        pmu::WriteDlssgRoute(routeGraph, key, rec);
        pmu::SetDlssgRouteRunning(routeGraph, {});
        routeTried |= 1u << int(routeGraph);
        veyra::log::warn("pushcore", std::format("DLSS-G backend {} did not start ({}; fails {})", narrow(pmu::DlssgRouteName(routeGraph)), why, rec.fails));
        routeGraph = pmu::DlssgRoute::None;
    }
    // a DLSS-G graph on the port or an RTX 30 backend failed: the port's self-test result and the backend's record
    void dlssgFailed(const std::string& why) {
        if (portGraph) recordPortSelfTest(false, why);
        routeFailed(why);
    }
    // clean end of an RTX 30 backend's graph: clear its crash guard, store the measurement (>= 120 full-batch frames)
    void routeGraphEnded() {
        if (routeGraph == pmu::DlssgRoute::None) return;
        const std::wstring key = adapterKey();
        pmu::SetDlssgRouteRunning(routeGraph, {});
        if (routeSamples.size() >= 120) {
            std::vector<double> v = routeSamples;
            std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
            const double med = v[v.size() / 2];
            pmu::DlssgRouteRecord rec = pmu::ReadDlssgRoute(routeGraph, key);
            rec.msPerGen = rec.runs == 0 || rec.msPerGen <= 0 ? med : rec.msPerGen * 0.5 + med * 0.5;
            ++rec.runs;
            pmu::WriteDlssgRoute(routeGraph, key, rec);
            veyra::log::info("pushcore", std::format("DLSS-G backend {}: {:.3f} ms per generated frame at 4K over {} frames (run {}, average {:.3f})",
                                                     narrow(pmu::DlssgRouteName(routeGraph)), med, v.size(), rec.runs, rec.msPerGen));
        }
        routeSamples.clear();
        routeGraph = pmu::DlssgRoute::None;
    }

    // The frame generation methods to try, in order, with a multiplier cap each: the asked-for one first, then the
    // next best this GPU can run, and at the end no frame generation (the open must
    // not fail because of one backend: the rest of the picture work still runs).
    std::vector<std::pair<uint32_t, uint32_t>> fallbackChain() const {
        std::vector<std::pair<uint32_t, uint32_t>> c;
        const uint32_t m = std::clamp<uint32_t>(settings.multiplier, 1, PUSHCORE_MAX_MULTIPLIER);
        if (m < 2) { c.push_back({settings.fgBackend, PUSHCORE_MAX_MULTIPLIER}); return c; }
        const bool nvidia = ctx.adapter().vendorId == 0x10DE, amd = ctx.adapter().vendorId == 0x1002;
        auto add = [&](uint32_t b, uint32_t cap) {
            if ((b == PUSHCORE_FG_VFG || b == PUSHCORE_FG_DLSSG || b == PUSHCORE_FG_NVOF) && !nvidia) return;
            if (b == PUSHCORE_FG_DLSSG && portUnlock() == pmu::Unlock::TuringDlssg && !portDlssgAllowed()) return;
            if (b == PUSHCORE_FG_VPL && ctx.adapter().vendorId != 0x8086) return;
            if (b == PUSHCORE_FG_HELIXFG && !helixFgAllowed()) return;
            for (const auto& e : c) if (e.first == b && e.second == cap) return;
            c.push_back({b, cap});
        };
        switch (settings.fgBackend) {
        case PUSHCORE_FG_DLSSG: add(PUSHCORE_FG_DLSSG, 8); if (m > 4) add(PUSHCORE_FG_DLSSG, 4); if (m > 2) add(PUSHCORE_FG_DLSSG, 2); add(PUSHCORE_FG_PUSHFLOW, PUSHCORE_MAX_MULTIPLIER); add(PUSHCORE_FG_FSR, 2); break;
        case PUSHCORE_FG_PUSHFLOW: add(PUSHCORE_FG_PUSHFLOW, PUSHCORE_MAX_MULTIPLIER); add(PUSHCORE_FG_HELIXFG, 6); add(PUSHCORE_FG_FSR, 2); if (amd) add(PUSHCORE_FG_AMDFRC, 2); break;
        // G10: XeFG at the asked multiplier, at 4x / 2x when more does not start (the MFG unlock refused), then PushFlow, FSR FG
        case PUSHCORE_FG_XEFG: add(PUSHCORE_FG_XEFG, 8); if (m > 4) add(PUSHCORE_FG_XEFG, 4); if (m > 2) add(PUSHCORE_FG_XEFG, 2); add(PUSHCORE_FG_PUSHFLOW, PUSHCORE_MAX_MULTIPLIER); add(PUSHCORE_FG_FSR, 2); if (amd) add(PUSHCORE_FG_AMDFRC, 2); break;
        case PUSHCORE_FG_FSR: add(PUSHCORE_FG_FSR, 2); add(PUSHCORE_FG_PUSHFLOW, PUSHCORE_MAX_MULTIPLIER); if (amd) add(PUSHCORE_FG_AMDFRC, 2); break;
        case PUSHCORE_FG_AMDFRC: add(PUSHCORE_FG_AMDFRC, 2); add(PUSHCORE_FG_FSR, 2); add(PUSHCORE_FG_PUSHFLOW, PUSHCORE_MAX_MULTIPLIER); break;
        case PUSHCORE_FG_NVOF: add(PUSHCORE_FG_NVOF, 2); add(PUSHCORE_FG_FSR, 2); add(PUSHCORE_FG_PUSHFLOW, PUSHCORE_MAX_MULTIPLIER); break;
        case PUSHCORE_FG_VPL: add(PUSHCORE_FG_VPL, 8); add(PUSHCORE_FG_PUSHFLOW, PUSHCORE_MAX_MULTIPLIER); add(PUSHCORE_FG_FSR, 2); break;
        case PUSHCORE_FG_HELIXFG: add(PUSHCORE_FG_HELIXFG, 8); if (m > 2) add(PUSHCORE_FG_HELIXFG, 2); add(PUSHCORE_FG_PUSHFLOW, PUSHCORE_MAX_MULTIPLIER); add(PUSHCORE_FG_FSR, 2); if (amd) add(PUSHCORE_FG_AMDFRC, 2); break;
        default: add(PUSHCORE_FG_VFG, PUSHCORE_MAX_MULTIPLIER); add(PUSHCORE_FG_DLSSG, 6); add(PUSHCORE_FG_PUSHFLOW, PUSHCORE_MAX_MULTIPLIER); add(PUSHCORE_FG_FSR, 2); if (amd) add(PUSHCORE_FG_AMDFRC, 2); break;
        }
        c.push_back({PUSHCORE_FG_NONE, 1});
        return c;
    }
    veyra::engine::FailedBackend lastFailed = veyra::engine::FailedBackend::None;
    pmu::DlssgRoute lastRouteFailed = pmu::DlssgRoute::None;   // RTX 30: the backend whose start just failed
    uint32_t builtFg = ~0u, builtSr = ~0u, builtTier = ~0u; bool builtGuard = true, builtSubtitles = false, builtHeld = false; int32_t builtAnimMode = 0; uint32_t builtHoldFlags = 0;
    bool builtIntelSr = false, settingsProbe = false; uint32_t builtIntelTarget = 0, builtIntelMult = 0;   // Intel VSR / VPL rebuild test; probe = no stage side effects
    int buildGraph() {
        builtFg = settings.fgBackend; builtSr = settings.srMethod; builtTier = settings.fgTier; builtGuard = settings.cutGuard != 0; builtSubtitles = settings.subtitleHold != 0; builtHeld = settings.heldFrames != 0; builtAnimMode = settings.animationMode; builtHoldFlags = settings.holdFlags; builtAnimeSr = settings.animeSrModel;
        builtIntelSr = settings.sr != 0; builtIntelTarget = settings.srTarget; builtIntelMult = settings.multiplier;
        if (!haveSettings) { tryFg = PUSHCORE_FG_VFG; tryMultiplierCap = PUSHCORE_MAX_MULTIPLIER; const int r = buildGraphOnce(); runFg = fg ? PUSHCORE_FG_VFG : PUSHCORE_FG_NONE; return r; }
        const auto chain = fallbackChain();
        std::string notes;
        fgFellBack = false;
        routeTried = 0; routeForce = pmu::DlssgRoute::None;
        for (size_t i = 0; i < chain.size(); ++i) {
            tryFg = chain[i].first; tryMultiplierCap = chain[i].second;
            lastFailed = veyra::engine::FailedBackend::None;
            lastRouteFailed = pmu::DlssgRoute::None;
            const int r = buildGraphOnce();
            routeForce = pmu::DlssgRoute::None;
            if (r == PUSHCORE_OK) {
                runFg = fg || vplOn ? tryFg : PUSHCORE_FG_NONE;
                fgFellBack = i > 0;
                fgNote = notes.empty() ? std::string() : notes + std::string(fgName(runFg));
                if (fgFellBack) veyra::log::warn("pushcore", "frame generation fallback: " + fgNote);
                return r;
            }
            if (deviceLost || i + 1 == chain.size()) return r;
            // only a frame generation failure moves down the chain; anything else is the open's error, as before
            if (lastFailed != veyra::engine::FailedBackend::Fg && lastFailed != veyra::engine::FailedBackend::NgxCore && lastFailed != veyra::engine::FailedBackend::OpticalFlow) return r;
            // RTX 30 DLSS-G: the other backend first (when this process may still take it), then the rest of the chain
            if (tryFg == PUSHCORE_FG_DLSSG && lastRouteFailed != pmu::DlssgRoute::None) {
                const pmu::DlssgRoute other = otherRouteAfterFailure(lastRouteFailed);
                lastRouteFailed = pmu::DlssgRoute::None;
                if (other != pmu::DlssgRoute::None) {
                    veyra::log::warn("pushcore", std::format("DLSS-G did not start on {}: trying {}", narrow(pmu::DlssgRouteName(other == pmu::DlssgRoute::Port ? pmu::DlssgRoute::Transfusion : pmu::DlssgRoute::Port)), narrow(pmu::DlssgRouteName(other))));
                    teardownGraph();
                    error.clear();
                    routeForce = other;
                    --i;
                    continue;
                }
            }
            notes += std::format("{}{} did not start ({}); ", fgName(tryFg), chain[i].second < std::min<uint32_t>(settings.multiplier, PUSHCORE_MAX_MULTIPLIER) ? std::format(" at {}x", std::min(settings.multiplier, chain[i].second)) : std::string(), error);
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
        if (cfgMultiplier >= 2) options.fgMultiplier = uint32_t(std::clamp(cfgMultiplier, 2, int(PUSHCORE_MAX_MULTIPLIER)));
        if (cfgVfgQuality >= 0) options.settings.vfgQuality = uint32_t(cfgVfgQuality);
        // XeSS presents on its own swap chain: only pushcore's XeFG route (XefgStage, the headless shim) takes it
        const bool xefgRoute = options.fg && engine::presentSinkFrameGeneration(options.settings.frameGenerationBackend);
        if (xefgRoute && !(haveSettings && tryFg == PUSHCORE_FG_XEFG))
            return fail("the saved frame generation method presents on its own (XeSS) and cannot feed a renderer");

        const bool hdrTransfer = stream.transfer == AVCOL_TRC_SMPTE2084 || stream.transfer == AVCOL_TRC_ARIB_STD_B67;
        gd = {};
        // Intel VSR before the graph: the graph's source is the upscaled frame
        const uint32_t srcW = ivsrOn ? ivsrW : stream.width, srcH = ivsrOn ? ivsrH : stream.height;
        frame->width = int(srcW); frame->height = int(srcH);
        gd.sourceWidth = srcW; gd.sourceHeight = srcH;
        initCostWindows();
        // EngineController: hdrInput = the source's colour is on the HDR path (PQ/HLG)
        gd.hdrInput = hdrTransfer;
        gd.highQualityPresentation = true;   // EngineController: every source (presenter only)
        gd.captureBitDepth = stream.format == PUSHCORE_INPUT_P010 ? 10 : 8;
        engine::StageRequest r;              // EngineController stageRequest() for an ordinary file
        r.nr = options.nr; r.sr = options.sr; r.fg = options.fg; r.fgMultiplier = options.fgMultiplier;
        r.width = srcW; r.height = srcH;
        r.stillImage = false; r.exportJob = false; r.liveSource = false; r.uncompressedSource = false;
        r.nvidiaAdapter = ctx.adapter().isNvidia; r.amdNr = ctx.adapter().vendorId == 0x1002;   // AMD: NR on the AMD runtimes
        // G11: RTX VSR through ZLUDA: the RTX VSR stage is described as on NVIDIA (describeStages keeps RTX VSR qualities
        // only for an NVIDIA adapter); every NVIDIA-only FG / HDR choice was already taken out in buildFromPushMpc
        const bool zludaRun = haveSettings && runSr == PUSHCORE_SR_METHOD_ZLUDA_VSR && options.sr && zvsr;
        if (zludaRun) r.nvidiaAdapter = true;
        r.nodeOrder = options.nodeOrder ? &*options.nodeOrder : nullptr;
        const bool animeRun = haveSettings && runSr == PUSHCORE_SR_METHOD_PMML && options.sr;
        if (animeRun) r.nvidiaAdapter = true;   // the RTX VSR stage is described; the anime upscaler takes it (externalSr)
        // PushFPS Crosshair Hold / Subtitle Hold: process-wide switches PushFPS's UI sets (PushOverlay.h), read by
        // describeStages. Crosshair Hold is never for video; Subtitle Hold follows PushMPC's setting, set right before each
        // graph is described (instances in one process describe their graphs one at a time, under the open / reload path).
        veyra::pipeline::overlay::setWanted(false);
        // Anime round 5 (item 4): the sign hold is that Subtitle Hold pass over the whole picture with PushFPS's text gate
        // (patch pushfps-4.5.4-anime-signs: overlay::signZoneFor / signGate), for PUSHCORE_HOLD_SIGNS; it composes only
        // while the picture is animation and the budget has room (signsPoll below).
        signsOn = haveSettings && (settings.holdFlags & PUSHCORE_HOLD_SIGNS) != 0 && settings.heldFrames != 0;
#ifdef PUSHFPS_ANIME_SIGNS
        veyra::pipeline::overlay::setSignsWanted(signsOn);
#else
        signsOn = false;   // a PushFPS without the anime signs hunks: no sign zone
#endif
        veyra::pipeline::overlay::setSubtitlesWanted(haveSettings && (settings.subtitleHold != 0 || signsOn));
        // item 6: NVOF FG line-art protection (patch pushfps-4.5.4-anime-nvof-line: PushMpcLinePass after FidelityFX's blend);
        // the graph makes it only for NVOF FG, it composes on animation while the budget has room (signsPoll below)
        lineNvofOn = haveSettings && (settings.holdFlags & PUSHCORE_HOLD_LINE_ART) != 0 && settings.heldFrames != 0;
#ifdef PUSHMPC_NVOF_LINE_PASS
        veyra::pipeline::lineart::setWanted(lineNvofOn);
#else
        lineNvofOn = false;
#endif
        subtitleState.store(0, std::memory_order_relaxed); subtitlePixels.store(0, std::memory_order_relaxed);
        subAutoInit = false; subCompose = false; subOnRun = subOffRun = 0; signsCompose = false; lineNvofCompose = true;
        engine::describeStages(r, options.snapshot(), gd);
        gd.noFeatures = false;
        gd.runtimeAbsPath = runtimeDir;
        // DLSS-G through the community port (RTX 20 / GTX 16 always; RTX 30 when pmu::ChooseDlssgRoute picks it): its
        // runtime folder goes first in NGX's search path (patch hunk EnhanceGraphDesc::pushMpcDlssgDirectory); the port's
        // backend is installed once per process, before NGX starts
        portGraph = false;
        routeGraph = pmu::DlssgRoute::None;
        routeSamples.clear();
        const pmu::Unlock pu = portUnlock();
        if (options.fg && options.settings.frameGenerationBackend == engine::FrameGenerationBackend::Dlss && pu != pmu::Unlock::Count) {
            bool usePort = pu == pmu::Unlock::TuringDlssg, lite = false;
            const std::wstring tkey = adapterKey();
            if (pu == pmu::Unlock::AmpereDlssg) {
                const pmu::DlssgRouteChoice rc = chooseRoute(haveSettings && settings.fgLite != 0);
                if (rc.route == pmu::DlssgRoute::None) { lastFailed = engine::FailedBackend::Fg; return fail("RTX 30 DLSS-G: " + narrow(rc.why)); }
                usePort = rc.route == pmu::DlssgRoute::Port;
                lite = rc.lite;
                routeGraph = rc.route;
                pmu::SetDlssgRouteRunning(routeGraph, tkey);   // crash guard of this backend
            }
            if (usePort) {
                if (!portRecorded && !portGuardSet && !pmu::ReadSelfTest(pu, tkey).known) {
                    if (portRunningKey(pu) == tkey) {
                        recordPortSelfTest(false, "an earlier self-test did not finish (the player closed unexpectedly)");
                        setPortRunning(pu, {});
                        lastFailed = engine::FailedBackend::Fg;
                        lastRouteFailed = routeGraph;
                        routeFailed("an earlier self-test did not finish");
                        return fail("DLSS-G port: an earlier self-test did not finish");
                    }
                    setPortRunning(pu, tkey);
                    portGuardSet = true;
                }
                const auto& port = pmport::EnsurePort(runtimeDir, veyra::runtime::logsDirectory() / L"dlssg-port.log", 5, lite);
                if (port.touched) gDlssgLatch.store(int(pmu::DlssgRoute::Port));
                const bool hostOk = pu == pmu::Unlock::TuringDlssg ? port.turingHost : true;
                if (!port.installed || !hostOk) {
                    const std::string why = port.installed ? "the port's backend did not detect a Turing GPU" : port.why;
                    lastFailed = engine::FailedBackend::Fg;
                    lastRouteFailed = routeGraph;
                    recordPortSelfTest(false, why);
                    routeFailed(why);
                    return fail("DLSS-G port: " + why);
                }
                if (port.lite != lite)
                    veyra::log::info("pushcore", std::format("DLSS-G port: FG Lite {} was asked for, the port runs preset {} in this process (the change applies at the next player start)",
                                                             lite ? "on" : "off", port.lite ? "C" : "A"));
                // RTX 30: Transfusion's in-process path (FgCompatibilitySession) stays off in a process that runs the port
                if (pu == pmu::Unlock::AmpereDlssg) SetEnvironmentVariableW(L"VEYRA_DISABLE_AMPERE_MFG_UNLOCK", L"1");
                gd.pushMpcDlssgDirectory = pmport::PortRuntimeDir(runtimeDir).wstring();
                portGraph = true;
            }
        }
        // HelixSR frame generation: the provider's root (anr\nvngx_dlssg.dll) and DLSS-G's motion convention (patch hunk
        // EnhanceGraphDesc::pushMpcDirectFgRoot / pushMpcDirectFgNvidiaMotion); the first graph on this adapter + driver is
        // its self-test
        helixFgGraph = false;
        if (options.fg && tryFg == PUSHCORE_FG_HELIXFG && options.settings.frameGenerationBackend == engine::FrameGenerationBackend::Anr) {
            if (!testBegin(helixFgTest)) { lastFailed = engine::FailedBackend::Fg; return fail("HelixSR frame generation: an earlier self-test did not finish"); }
            gd.pushMpcDirectFgRoot = helixRoot().wstring();
            gd.pushMpcDirectFgNvidiaMotion = true;
            helixFgGraph = true;
        }
        // HLX FSR 4 as the FSR upscaler on RX 5000 / GTX (one folder per process, PUSHMPC_FSR_SR_DIR, patch hunk in
        // FsrSrBackend); after a failed self-test the process goes back to AMD's FidelityFX DLL (FSR 3.1 there)
        hlxGraph = false;
        {
            const int hk = hlxFsr4Kind(ctx.adapter().vendorId, family);
            const bool wantHlx = hk && options.sr && runSr == PUSHCORE_SR_METHOD_FSR && gHlxFsr4Process.load() != 2 && !hlxFsrTest.recorded
                                 ? communityAllowed(pmu::Unlock::HlxFsr4, hlxFsr4Files(hk)) : (hk && options.sr && runSr == PUSHCORE_SR_METHOD_FSR && gHlxFsr4Process.load() == 1);
            if (wantHlx && testBegin(hlxFsrTest)) {
                SetEnvironmentVariableW(L"PUSHMPC_FSR_SR_DIR", hlxFsr4Dir(hk).wstring().c_str());
                gHlxFsr4Process.store(1);
                hlxGraph = true;
                veyra::log::info("pushcore", std::format("FSR upscaling: HLX FSR 4 {} (HelixSR {})", hk == 2 ? "Full" : "Lite", pmhelix::kVersion));
            } else if (gHlxFsr4Process.load() == 2) {
                SetEnvironmentVariableW(L"PUSHMPC_FSR_SR_DIR", nullptr);
            }
        }
#if defined(PUSHMPC_HAVE_PMML) && PUSHMPC_HAVE_PMML && PUSHMPC_HAVE_EXTERNAL_SR
        animeSr.reset();
        if (animeRun) { animeSr = std::make_shared<pushmpc::AnimeSr>(settings.animeSrModel, ctx.adapter().vendorId == 0x10DE && rtxFamily(family)); gd.externalSr = animeSr; }
#endif
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
        gd.shareableOutputs = (flags & PUSHCORE_FLAG_EGRESS_COPY) == 0 && !xefgRoute;   // XeFG: XefgStage's outputs are what D3D11 sees
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
                if (srcW > fw) { gd.flowWidth = fw; gd.flowHeight = std::max(64u, ((srcH * fw / srcW) + 1u) & ~1u); }
            }
        }
        if (options.fg && options.settings.frameGenerationBackend == engine::FrameGenerationBackend::AmdFrc) preferSystemD3dCompiler();

        fg = options.fg;
        graphMultiplier = fg ? options.fgMultiplier : 1;
        multiplier = fg && retimeK >= 2 && retimeK < graphMultiplier ? retimeK : graphMultiplier;
        retimer.Configure(multiplier, graphMultiplier);
        clearHeldQueues();
        hdrIn = gd.hdrInput; hdrOut = gd.hdrOutput;
        resolveHdrHint();
        veyra::log::info("display-color", std::format("hdrInput={} hdrOutput={} displayHdr={} forceSdrPreview={} hdrOutputMode={} hdrMetadataHint={} hdrCurve={} videoHdr={} masteringPeak={} maxCLL={} maxFALL={}",
            gd.hdrInput, gd.hdrOutput, stream.displayHdr, options.settings.forceSdrPreview, int(gd.hdrOutputMode), hdrHint, hdrCurveOn,
            gd.videoHdr.enabled, stream.masteringPeakNits, stream.maxCllNits, stream.maxFallNits));
        gd.enableFg = fg;
        gd.fgMultiplier = fg ? graphMultiplier : gd.fgMultiplier;   // the built multiplier (re-timed spans ask for up to it)

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
        // G11: the worker takes the graph's RTX VSR pass (the external SR hunks of patches/pushfps-4.5.4-pushmpc.patch: EnhanceGraphDesc::externalSr)
        zludaSr.reset();
        if (zludaRun) { zludaSr = std::make_shared<pushmpc::ZludaVsrSr>(*zvsr); gd.externalSr = zludaSr; }
        graph = std::make_unique<pipeline::EnhanceGraph>(ctx, ring, cache);
        const auto t = Clock::now();
        // G10: XeSS SR takes the graph's temporal upscaler slot while this graph initialises
        const bool xessSr = haveSettings && runSr == PUSHCORE_SR_METHOD_XESS && options.sr;
#ifdef VEYRA_FSRSR_PROVIDER_OVERRIDE
        if (xessSr) gfx::FsrSrBackend::setProviderOverride(static_cast<const gfx::FsrSrBackend::ProviderOverride*>(pushmpc::xess::srProvider(settings.srQuality)));
#endif
        const bool started = graph->initialize(gd) && graph->createViews();
#ifdef VEYRA_FSRSR_PROVIDER_OVERRIDE
        if (xessSr) gfx::FsrSrBackend::setProviderOverride(nullptr);
#endif
        if (!started) {
            lastFailed = graph->failedBackend();
            if (zludaRun && lastFailed == engine::FailedBackend::Sr) {
                // RTX VSR through ZLUDA did not start (worker refused / missing / timed out): FSR in its place, for this instance
                veyra::log::warn("pushcore", "RTX VSR through ZLUDA did not start (" + zludaSr->error() + "): FSR upscaling instead");
                zludaFailed = true;
                graph->shutdown(); graph.reset(); zludaSr.reset();
                return buildGraphOnce();
            }
            if (lastFailed == engine::FailedBackend::Fg || lastFailed == engine::FailedBackend::NgxCore) {
                lastRouteFailed = routeGraph;
                dlssgFailed(std::format("the DLSS-G graph did not start (failed backend {})", int(lastFailed)));
                if (helixFgGraph) testRecord(helixFgTest, false, std::format("HelixSR frame generation did not start (failed backend {})", int(lastFailed)));
            }
#if defined(PUSHMPC_HAVE_PMML) && PUSHMPC_HAVE_PMML && PUSHMPC_HAVE_EXTERNAL_SR
            if (animeRun && !animeSrFailed && lastFailed == engine::FailedBackend::Sr) {   // the anime upscaler did not start: again without it
                animeSrFailed = true;
                veyra::log::warn("pushcore", std::format("anime upscaler {} did not start ({}): RTX VSR / FSR instead", pushmpc::animeSrModelId(settings.animeSrModel), animeSr ? animeSr->error() : std::string()));
                teardownGraph();
                error.clear();
                return buildGraphOnce();
            }
#endif
            return fail(std::format("enhancement graph did not start (failed backend {})", int(graph->failedBackend())));
        }
        xessSrOn = xessSr && graph->fsrSrEnabled();
        if (xessSr && !graph->fsrSrEnabled()) {
            // XeSS SR did not start on this GPU (no runtime, no preset for this scale, a driver refusal): FSR in its place
            veyra::log::warn("pushcore", "XeSS SR did not start (see xess-sr): FSR upscaling instead");
            srXessFailed = true;
            graph->shutdown(); graph.reset();
            return buildGraphOnce();
        }
        graph->recordGpuTimings();
        // RTX 30: a Transfusion graph that started has patched this process: it keeps Transfusion from here on
        if (routeGraph == pmu::DlssgRoute::Transfusion && fg && graph->fgEnabled()) gDlssgLatch.store(int(pmu::DlssgRoute::Transfusion));
        if (fg && !graph->fgEnabled() && !xefgRoute) {
            lastFailed = engine::FailedBackend::Fg;
            lastRouteFailed = routeGraph;
            dlssgFailed("DLSS-G did not start");
            if (helixFgGraph) testRecord(helixFgTest, false, "HelixSR frame generation did not start");
            return fail("frame generation did not start");
        }
        if (xefgRoute) {
            // G10: XeFG on the headless swap chain at the output size, on the graph's queue
            if (!graph->xessEnabled()) { lastFailed = engine::FailedBackend::Fg; return fail("the graph did not start in XeSS present-sink mode"); }
            pushmpc::xess::XefgConfig xc;
            xc.device = ctx.device(); xc.queue = ring.queue(); xc.d3d11 = d11_1.Get();
            xc.width = graph->workWidth(); xc.height = graph->workHeight(); xc.format = graph->outputFormat(); xc.hdr10 = graph->hdr10Output();
            xc.multiplier = multiplier;
            xefg = std::make_unique<pushmpc::xess::XefgStage>();
            std::string why;
            if (!xefg->init(xc, why)) { xefg.reset(); lastFailed = engine::FailedBackend::Fg; return fail("XeFG did not start: " + why); }
            if (!initXefgGuard()) veyra::log::warn("pushcore", "XeFG: the cut guard did not start (no hold on cuts; XeFG's own scene-change detection stays)");
        }
        if (haveSettings && settings.activeArea[2] > settings.activeArea[0] && settings.activeArea[3] > settings.activeArea[1])
            graph->setCutGuardActiveArea(settings.activeArea[0], settings.activeArea[1], settings.activeArea[2], settings.activeArea[3]);
        cutGuardOn = fg && graph->cutGuardActive();
        // Intel VPL frame interpolation after the graph (its frames at the graph's output size and format); not starting
        // is a frame generation failure: the fallback chain moves on (VPL -> PushFlow -> FSR FG)
        vplOn = false;
        if (vplWanted) {
            std::string why;
            const double fps = stream.fpsNum > 0 && stream.fpsDen > 0 ? double(stream.fpsNum) / stream.fpsDen : 24.0;
            if (pmu::ReadUnlockMode(pmu::Unlock::VplFi) == pmu::UnlockMode::Off) why = "switched off (PUSHMPC_DISABLE_VPLFI / Unlocks VplFi = 0)";
            else if (ensureBridge(why) && vpl.init(bridge, ctx.device(), ring.queue(), d11_1.Get(), fence11.Get(), fence12.Get(), graph->workWidth(), graph->workHeight(),
                                                   graph->outputFormat(), vplWant, fps, !haveSettings || settings.cutGuard != 0, why)) {
                vplOn = true;
                multiplier = vpl.multiplier();
                cutGuardOn = vpl.cutGuardActive();
                veyra::log::info("pushcore", "frame generation: " + vpl.describe());
            }
            if (!vplOn) { lastFailed = engine::FailedBackend::Fg; return fail("Intel VPL frame interpolation did not start: " + why); }
        }
        description = std::format(
            "source {}x{} {}{} -> output {}x{} {}; SR {} (videoSrQuality {}); FG {} {} {} (quality/tier {}, async {}, cut guard {}); NR {}; egress {}; GPU family {}",
            stream.width, stream.height, stream.format == PUSHCORE_INPUT_P010 ? "P010" : "NV12",
            !gd.hdrInput ? "" : stream.transfer == AVCOL_TRC_ARIB_STD_B67 ? " HLG" : " PQ",
            graph->workWidth(), graph->workHeight(),
            graph->hdr10Output() ? "HDR10 (PQ BT.2020)" : gd.hdrInput ? "SDR (PushFPS BT.2390 tone map)" : "SDR",
            graph->srEnabled() ? "on" : "off", gd.videoSrQuality, fg ? "on" : "off",
            engine::frameGenerationBackendName(options.settings.frameGenerationBackend), multiplier == graphMultiplier ? std::format("{}X", multiplier) : std::format("{}X (graph {}X for held-frame spans)", multiplier, graphMultiplier), gd.vfgQuality,
            gd.vfgAsyncSubmission ? 1 : 0, fg ? (cutGuardOn ? "on" : "off") : "-", graph->nrEnabled() ? "on" : "off",
            gd.shareableOutputs ? "direct (shared graph pool)" : "copy ring", family);
        if (ivsrOn) description += "; " + ivsrNote;
        if (vplOn) description += "; " + vpl.describe();
        if (zludaRun) description += std::format("; upscaler RTX VSR through ZLUDA ({})", graph->externalSrActive() ? "worker running" : "not active");
#if defined(PUSHMPC_HAVE_PMML) && PUSHMPC_HAVE_PMML && PUSHMPC_HAVE_EXTERNAL_SR
        if (animeRun) description += std::format("; upscaler {} ({})", animeSr->name(), graph->externalSrActive() ? animeSr->describe() : std::string("not active"));
#endif
        veyra::log::info("pushcore", std::format("graph ready in {:.0f} ms: {}", msSince(t), description));
        const int er = buildEgress();
        if (er == PUSHCORE_OK) { initHoldModes(); initRepair(); initHold(); }
        return er;
    }

    // G10: pushcore's own cut guard for the XeFG route (the graph's guard does not run in present-sink mode): every real
    // frame handed to XeFG is copied (xreal ring) and tested against the one before; a cut pair's generated frames are then
    // replaced on the GPU by the pair's real frames (predicated copies), as the guard does for every other method.
    bool initXefgGuard() {
        xguard.reset();
        for (auto& r : xreal) r.Reset();
        for (auto& s : xrealSeq) s = 0;
        xguardSeq[0] = xguardSeq[1] = 0;
        if (haveSettings && settings.cutGuard == 0) return true;   // switched off: no guard, nothing to warn about
        auto g = std::make_unique<veyra::pipeline::CutGuard>();
        if (!g->initialize(ctx.device(), graph->workWidth(), graph->workHeight(), graph->outputFormat(), true, "xefg")) return false;
        if (haveSettings && settings.activeArea[2] > settings.activeArea[0] && settings.activeArea[3] > settings.activeArea[1])
            g->setActiveArea(settings.activeArea[0], settings.activeArea[1], settings.activeArea[2], settings.activeArea[3]);
        for (auto& r : xreal) {
            D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = graph->workWidth(); td.Height = graph->workHeight();
            td.DepthOrArraySize = 1; td.MipLevels = 1; td.Format = graph->outputFormat(); td.SampleDesc.Count = 1;
            if (FAILED(ctx.device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r)))) return false;
        }
        for (auto& a : xalloc) if (!a && FAILED(ctx.device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) return false;
        if (!xlist) {
            if (FAILED(ctx.device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, xalloc[0].Get(), nullptr, IID_PPV_ARGS(&xlist)))) return false;
            xlist->Close();
        }
        xguard = std::move(g);
        return true;
    }
    ID3D12GraphicsCommandList* xBegin() {
        const uint32_t a = xnext++ % kXLists;
        if (xallocValue[a] && fence12->GetCompletedValue() < xallocValue[a]) { fence12->SetEventOnCompletion(xallocValue[a], waitEvent); WaitForSingleObject(waitEvent, 10000); }
        xalloc[a]->Reset(); xlist->Reset(xalloc[a].Get(), nullptr);
        return xlist.Get();
    }
    void xEnd() {
        xlist->Close();
        ID3D12CommandList* lists[] = {xlist.Get()};
        ring.queue()->ExecuteCommandLists(1, lists);
        ring.queue()->Signal(fence12.Get(), ++value12);
        xallocValue[(xnext - 1) % kXLists] = value12;
    }
    static void xBarrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER br{}; br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; br.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
        l->ResourceBarrier(1, &br);
    }

    // G10: one source frame on the XeFG route: the graph's real frame (and its motion) to XeFG; the graph lease goes as soon
    // as the copies are queued (same queue, in order)
    int submitXefg(Batch& b, bool reset) {
        using namespace veyra::pipeline;
        if (b.out.batch.count < 1) return fail("XeFG route: the graph made no real frame");
        for (int spin = 0; !graph->resolveFrame(b.out, 0) && spin < 2000; ++spin) Sleep(0);
        auto& f = b.out.batch.frames[0];
        if (!f.lease || !f.lease->texture) return fail("XeFG route: no real frame texture");
        ID3D12Resource* real = f.lease->texture.Get();
        const uint32_t parity = f.lease->slot;
        const uint64_t seq = ++xseq;
        // the motion PushFPS's XeSS presenter gives XeFG, with its reset rule (no valid motion, or not from the frame before)
        const bool motionOk = graph->presentMotionValid(parity) && graph->motionPreviousSource(parity) == xlastGraphSource;
        xlastGraphSource = f.identity.sourceFrameId ? f.identity.sourceFrameId : xlastGraphSource;
        const bool xreset = reset || !motionOk || xlastPts == INT64_MIN;
        if (xguard) {
            auto* l = xBegin();
            ID3D12Resource* copy = xreal[seq % kXReal].Get();
            xBarrier(l, real, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            xBarrier(l, copy, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            l->CopyResource(copy, real);
            xBarrier(l, real, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            xBarrier(l, copy, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
            xguard->captureAndTest(l, copy, unsigned(seq & 1), !xreset);
            xBarrier(l, copy, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            xEnd();
            xguard->submitted(fence12.Get(), value12);
            xrealSeq[seq % kXReal] = seq; xguardSeq[seq & 1] = seq;
        }
        const float frameMs = xlastPts != INT64_MIN && b.realPts > xlastPts ? float(double(b.realPts - xlastPts) / 10000.0) : 0.0f;
        std::string why;
        if (!xefg->submit(real, graph->presentMotion(parity), xreset, xreset ? 0.0f : frameMs, b.realPts, seq, why)) return fail("XeFG: " + why);
        xlastPts = b.realPts;
        xsource[seq % kXSource] = b.sourceIndex;
        f.lease.reset();   // the copies into XeFG's back buffer and the guard's ring are queued before any later graph work
        return PUSHCORE_OK;
    }
    static constexpr uint32_t kXSource = 64;
    uint64_t xsource[kXSource]{};   // XeFG sequence -> pushcore source index

    // G10: the XeFG route's outputs: what XeFG presented (captured by the shim), with the guard's hold on cut pairs, made
    // ready for D3D11 on fence12 (one Wait + Signal on the graph queue per poll)
    int pollXefg(PushCoreFrame* frames, uint32_t capacity, uint32_t& count) {
        std::vector<pushmpc::xess::XefgStage::Output> outs;
        pushmpc::xess::XefgStage::Output o;
        while (count + outs.size() < capacity && xefg->take(o)) {
            if (o.dropped) { ++st.skippedGenerated; continue; }
            outs.push_back(o);
        }
        if (outs.empty()) return PUSHCORE_OK;
        uint64_t waitV = 0; ID3D12Fence* waitF = nullptr;
        for (const auto& x : outs) if (x.fenceValue > waitV) { waitV = x.fenceValue; waitF = x.fence; }
        if (waitF) ring.queue()->Wait(waitF, waitV);
        bool any = false;
        ID3D12GraphicsCommandList* l = nullptr;
        for (const auto& x : outs) {
            if (!x.generated || !xguard) continue;
            const uint64_t k = x.sourceIndex;   // the pair (k-1, k)
            ID3D12Resource* a = xreal[(k - 1) % kXReal].Get();
            ID3D12Resource* bb = xreal[k % kXReal].Get();
            if (k < 2 || xrealSeq[(k - 1) % kXReal] != k - 1 || xrealSeq[k % kXReal] != k || xguardSeq[k & 1] != k) { ++xholdsSkipped; continue; }
            if (!l) l = xBegin();
            xBarrier(l, x.resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            xBarrier(l, a, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            xBarrier(l, bb, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            xguard->holdOnCut(l, x.resource, a, unsigned(k & 1), bb, 2 * x.subframe > x.groupSize);
            xBarrier(l, x.resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            xBarrier(l, a, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            xBarrier(l, bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            ++xholds; any = true;
        }
        if (any) xEnd();
        else if (hrFail(ring.queue()->Signal(fence12.Get(), ++value12), "D3D12 Signal(XeFG outputs)")) return PUSHCORE_ERROR;
        for (const auto& x : outs) {
            PushCoreFrame& f = frames[count];
            f = {};
            f.texture = x.texture;
            f.readyFence = fence12in11.Get();
            f.readyValue = value12;
            f.kind = x.generated ? PUSHCORE_FRAME_GENERATED : PUSHCORE_FRAME_REAL;
            f.pts100ns = x.pts100ns;
            f.sourceIndex = xsource[x.sourceIndex % kXSource];
            f.subframe = x.subframe;
            f.width = graph->workWidth(); f.height = graph->workHeight();
            Lease le; le.kind = f.kind; le.xefgSlot = int(x.slot);
            f.lease = nextLease++;
            leases.emplace(f.lease, std::move(le));
            ++count; ++st.outputs;
            if (x.generated) ++st.generated; else ++st.real;
        }
        return PUSHCORE_OK;
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
        if (xessSrOn) description += std::format("; upscaler {} (Intel XeSS SR)", graph->fsrSrProviderVersion());
        if (xefg) {   // XeFG route: XefgStage's captured outputs are the egress (shared textures, one copy each)
            cutGuardOn = xguard != nullptr;
            description += std::format("; {} on a headless swap chain, cut guard {}", xefg->description(), xguard ? "on (hold on cuts)" : "off");
            veyra::log::info("pushcore", std::format("egress: XeFG's {} captured output textures; {}", xefg->slotCount(), description));
            return PUSHCORE_OK;
        }
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
        const uint32_t count = 2 * graphMultiplier + 2;
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
    // Re-timing: a source frame is taken while pushcore's own queue (frames waiting for their test result, for the next
    // distinct frame, or for the graph) has room and the ingress slot is not one of them; the graph is fed from the
    // queue (pumpHeld). Without it: the graph's own slot rule, as before.
    bool canSubmit() {
        if (!graph) return false;
        if (pumpError != PUSHCORE_OK) return true;   // submit reports it
        if (!retimingOn()) {
            if (queuedFrames() && (pumpError = pumpHeld(true)) != PUSHCORE_OK) return true;   // re-timing switched off: the queue first
            return queuedFrames() == 0 && graph->nextFrameSlotAvailable() && (!xefg || xefg->canSubmit());
        }
        if ((pumpError = pumpHeld(false)) != PUSHCORE_OK) return true;
        return queuedFrames() < kMaxQueued && !slotQueued(uint32_t(inIndex % kIngressSlots)) && (!xefg || xefg->canSubmit());   // the ring (17) keeps d0 alive behind the queue
    }

    bool ensureIngress(ID3D11Texture2D* src) {
        D3D11_TEXTURE2D_DESC sd{};
        src->GetDesc(&sd);
        const DXGI_FORMAT want = stream.format == PUSHCORE_INPUT_P010 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
        if (sd.Format != want && !(want == DXGI_FORMAT_P010 && sd.Format == DXGI_FORMAT_P016)) {
            fail(std::format("input texture format {} does not match the stream", int(sd.Format)));
            return false;
        }
        // Route B: the ring holds the video processor's output (the SR target size), whatever the decoder's surface size
        const UINT wantW = qvsr.active() ? qvsr.outWidth() : sd.Width, wantH = qvsr.active() ? qvsr.outHeight() : sd.Height;
        if (wantW == inW && wantH == inH && sd.Format == inFormat && inSlots[0].tex) return true;
        if (inSlots[0].tex) ring.drainQueue();
        for (auto& slot : inSlots) {
            if (slot.tex) qvsr.forget(slot.tex.Get());
            slot = {};
            D3D11_TEXTURE2D_DESC d{};
            d.Width = wantW; d.Height = wantH; d.MipLevels = 1; d.ArraySize = 1;
            d.Format = sd.Format == DXGI_FORMAT_P016 ? DXGI_FORMAT_P010 : sd.Format;
            d.SampleDesc = {1, 0}; d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (qvsr.active() ? D3D11_BIND_RENDER_TARGET : 0u);   // the processor's output view
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
        inW = wantW; inH = wantH; inFormat = sd.Format;
        veyra::log::info("pushcore", std::format("ingress ring: {} shared textures {}x{} format {} (source mips {} array {})",
                                                 kIngressSlots, inW, inH, int(inFormat), sd.MipLevels, sd.ArraySize));
        return true;
    }


    // FG admission (M2b). The renderer decides the level from what it observes at the presenter (generated frames
    // presented late) and passes it with each submit: 0 = the full group, 1 = the 2X midpoint only (PushFPS's
    // "reduced group": a presentable midpoint instead of a whole-group hole, history kept), 2 = no generated frame
    // (history kept; for VFG PushFPS turns a budget Seed into exactly this Skip). A predictive version after
    // EngineController::admitFg (deadline vs predicted GPU cost) was tried first: its cost predictions were too
    // pessimistic under other GPU load and halved the rate at 1.0x where nothing would have been late; measured in the release tests.
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
        cutsSeen.store(cutBase + graph->cutGuardCuts() + (vplOn ? vpl.cuts() : 0), std::memory_order_relaxed);
        if (ivsrOn) for (double v : ivsr.takeTimings()) { if (ivsrMs.size() < 64) ivsrMs.push_back(v); }
        if (zludaSr && !zludaFailed && graph->externalSrLost()) {
            // G11: the ZLUDA worker stopped mid-stream: the graph resizes plainly from that frame on (never a black frame);
            // the next rebuild (any settings change, applySettingsLive below) runs FSR
            zludaFailed = true;
            veyra::log::warn("pushcore", "RTX VSR through ZLUDA stopped (" + zludaSr->error() + "): FSR from the next graph rebuild");
        }
#if defined(PUSHMPC_HAVE_PMML) && PUSHMPC_HAVE_PMML && PUSHMPC_HAVE_EXTERNAL_SR
        if (animeSr && !animeSrFailed && graph->externalSrLost()) {
            // the anime upscaler stopped mid-stream: plain resize from that frame on, RTX VSR / FSR from the next rebuild
            animeSrFailed = true;
            veyra::log::warn("pushcore", std::format("anime upscaler {} stopped ({}): RTX VSR / FSR from the next graph rebuild", animeSr->name(), animeSr->error()));
        }
#endif
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
            // the stages outside the graph: Intel VSR (bridge timestamps) as the upscaler, Intel VPL as the frame generation
            if (ivsrOn && p.srMs < 0 && !ivsrMs.empty()) { p.srMs = ivsrMs.front(); ivsrMs.pop_front(); }
            if (vplOn && p.fgBatchMs < 0 && !vplMs.empty()) { p.fgBatchMs = vplMs.front(); p.fg1Ms = p.fgBatchMs; vplMs.pop_front(); }
            if (hlxGraph && !hlxFsrTest.recorded) {   // HLX FSR 4's self-test: the FSR upscaler measured on 240 frames
                ++hlxFsrTest.seen;
                if (p.srMs > 0 && ++hlxFsrTest.count == kPortSelfTestFrames) testRecord(hlxFsrTest, true, std::format("HLX FSR 4 upscaled {} frames", kPortSelfTestFrames));
                else if (hlxFsrTest.seen >= 2 * kPortSelfTestFrames && hlxFsrTest.count == 0) {
                    testRecord(hlxFsrTest, false, "HLX FSR 4 did not run (the FSR upscaler did not start with it)");
                    gHlxFsr4Process.store(2);   // later graphs in this process: AMD's FidelityFX DLL
                }
            }
            if (auto it = decisionOf.find(p.sourceFrameId); it != decisionOf.end()) {
                // GPU work of the frame = its measured stages (colour, VSR, the FG group). The graph's execution span
                // also contains the queue's waits (presentation release fence, VFG's CUDA hand-off) and overstated
                // the cost under load by ~15 ms, which pushed 1.0x playback into reduced groups needlessly.
                const double span = (p.colorMs > 0 ? p.colorMs : 0) + (p.srMs > 0 ? p.srMs : 0) + (p.fgBatchMs > 0 ? p.fgBatchMs : 0)
                                  + (p.nrMs > 0 ? p.nrMs : 0) + (p.videoHdrMs > 0 ? p.videoHdrMs : 0);
                // a re-timed pair spans `gap` source frames: its cost per source frame is what admission compares
                double g = 1;
                if (auto gi = gapOf.find(p.sourceFrameId); gi != gapOf.end()) { g = double(gi->second); gapOf.erase(gi); }
                if (it->second == 0 && p.fgBatchMs > 0 && routeGraph != pmu::DlssgRoute::None && multiplier >= 2 && routeSamples.size() < 8192) {
                    const double px = double(graph->workWidth()) * double(graph->workHeight());
                    if (px > 0) routeSamples.push_back(p.fgBatchMs / g / double(multiplier - 1) * (3840.0 * 2160.0 / px));
                }
                if (it->second == 0 && p.fgBatchMs > 0) { costFull.push(span / g); costFirst.push(p.fg1Ms); costFirstBatch.push(p.fgBatchMs / g); }
                else if (it->second == 1 && p.fgBatchMs > 0) costReduced.push(span / g);
                decisionOf.erase(it);
            }
            while (decisionOf.size() > 256) decisionOf.erase(decisionOf.begin());
            while (gapOf.size() > 256) gapOf.erase(gapOf.begin());
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
        if (pumpError != PUSHCORE_OK) { const int e = pumpError; pumpError = PUSHCORE_OK; return e; }
        if (!ensureIngress(src)) return PUSHCORE_ERROR;
        D3D11_TEXTURE2D_DESC sd{};
        src->GetDesc(&sd);
        if (slice >= sd.ArraySize) return fail("array slice out of range");

        const uint32_t slotIndex = uint32_t(inIndex % kIngressSlots);
        auto& slot = inSlots[slotIndex];
        ++inIndex;
        if (!gpuOnlyInput) {
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
        if (qvsr.active()) {   // Route B: driver RTX VSR scales the source into the slot
            std::string vpErr;
            if (!qvsr.blit(copySrc, copySub, slot.tex.Get(), vpErr)) return fail("quick VSR: " + vpErr);
        } else {
            imm->CopySubresourceRegion(slot.tex.Get(), 0, 0, 0, 0, copySrc, copySub, nullptr);
        }
        if (hrFail(imm4->Signal(fence11.Get(), ++value11), "D3D11 Signal(ingress)")) return PUSHCORE_ERROR;
        }   // gpuOnlyInput (PUSHCORE_FLAG_WARMUP_GPU_ONLY warm-up): the slot as it is, no D3D11 work, no new fence value
        const uint64_t copyValue = value11;
        const uint64_t seq = ingestSeq++;
        lastIngest = Clock::now();
        if (retimingOn()) {
            // the held-frame test against the source frame before (same size, no discontinuity); the frame waits in
            // the queue until its result is read
            Pending p;
            p.slot = slotIndex; p.seq = seq; p.pts = pts; p.flags = submitFlags; p.copyValue = copyValue;
            if (timing) { p.haveTiming = true; std::memcpy(&p.timing, timing, std::min<size_t>(sizeof(p.timing), timing->structSize)); }
            if (prevIngestSlot >= 0 && !(submitFlags & PUSHCORE_SUBMIT_DISCONTINUITY))
                p.compared = held.run(imm.Get(), slot.tex.Get(), inSlots[prevIngestSlot].tex.Get(), stream.width, stream.height, seq);
            imm4->Flush();
            prevIngestSlot = int(slotIndex);
            if (!heldLogged) {
                heldLogged = true;
                veyra::log::info("pushcore", std::format("held-frame re-timing on: {}x per source frame, graph {}x, lookahead {}, queue {}", multiplier, graphMultiplier, pushmpc::held::kLookahead, kMaxQueued));
            }
            pendingIn.push_back(p);
            ++st.submitted;
            st.submitCpuMs += msSince(t0);
            return pumpHeld(false);
        }
        imm4->Flush();
        prevIngestSlot = int(slotIndex);
        ++st.submitted;
        return processFrame(slotIndex, copyValue, pts, submitFlags, timing, multiplier, 1, seq, t0);
    }

    // One source frame from its ingress slot into the graph. perCall: the graph's multiplier for the pair that ends at
    // this frame (the plain multiplier, or a re-timed span's); gap: source frames the pair spans.
    int processFrame(uint32_t slotIndex, uint64_t copyValue, int64_t pts, uint32_t submitFlags, const PushCoreSubmitTiming* timing,
                     uint32_t perCall, uint32_t gap, uint64_t sourceIndex, Clock::time_point t0) {
        using namespace veyra;
        auto& slot = inSlots[slotIndex];
        pipeline::HardwareSurfaceInput hw;
        hw.texture = slot.res.Get(); hw.subresourceIndex = 0;
        hw.waitFence = fence11in12.Get(); hw.waitValue = copyValue;
        hw.textureWidth = inW; hw.textureHeight = inH;
        if (ivsrOn) {
            // Intel VSR on the bridge: the ingress slot (after the copy above) into the stage's slot at the output size;
            // the graph waits for the bridge's fence instead of the ingress one
            const uint32_t k = slotIndex;
            std::string why;
            if (!slot.br && !bridge.open11(slot.tex.Get(), slot.br, why)) return fail("Intel VSR: " + why);
            const uint64_t w = ivsr.run(slot.br.Get(), k, fence11inBridge.Get(), copyValue, fence12inBridge.Get(), slot.freeValue, why);
            if (!why.empty()) veyra::log::warn("pushcore", why);
            if (!w) return fail("Intel VSR: " + why);
            hw.texture = ivsr.slot(k).res.Get(); hw.waitFence = ivsr.fence().in12.Get(); hw.waitValue = w;
            hw.textureWidth = ivsrW; hw.textureHeight = ivsrH;
        }

        const bool reset = needReset || (submitFlags & PUSHCORE_SUBMIT_DISCONTINUITY) || !haveReal;
        Batch b;
        b.sourceIndex = sourceIndex;
        b.realPts = pts;
        b.vplReset = reset; b.prevPts = lastRealPts; lastRealPts = pts;
        b.fgLevel = timing && timing->structSize >= PUSHCORE_SUBMIT_TIMING_SIZE_V2 && timing->streamNow100ns != INT64_MIN ? std::clamp<int32_t>(timing->fgLevel, 0, 2) : 0;
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
                            /*previewMultiplier*/ fg ? perCall : 1u))
        {
            if (portGraph && portValidGenerated < kPortSelfTestFrames)
                recordPortSelfTest(false, std::format("DLSS-G failed after {} generated frames (failed backend {})", portValidGenerated, int(graph->failedBackend())));
            if (routeGraph != pmu::DlssgRoute::None) routeFailed(std::format("DLSS-G failed while running (failed backend {})", int(graph->failedBackend())));
            if (helixFgGraph && helixFgTest.count < kPortSelfTestFrames)
                testRecord(helixFgTest, false, std::format("HelixSR frame generation failed after {} generated frames (failed backend {})", helixFgTest.count, int(graph->failedBackend())));
            return fail(std::format("graph process failed (failed backend {})", int(graph->failedBackend())));
        }
        if (!useAdmission) decision = fg ? 0 : 2;
        if (fg && !reset) {
            // reset pairs evaluate one seed group whatever the decision; count real pairs only
            if (decision == 0) ++st.fgFull; else if (decision == 1) ++st.fgReduced; else ++st.fgSkipped;
        }
        decisionOf[frameId] = reset ? 3 : decision;
        if (gap > 1) gapOf[frameId] = gap;
        {
            // predicted GPU finish of this frame: after the queued work, at its decision's measured cost
            double cost = decision == 0 ? costFull.p50() : decision == 1 ? costReduced.p50() : -1;
            if (cost <= 0) cost = decision == 2 ? 3.0 : (costFull.p50() > 0 ? costFull.p50() : 25.0);
            cost += pendingRepairMs; pendingRepairMs = 0;   // the source repair queued ahead of this frame's graph work
            const double startMs = inflight.empty() ? nowMs : std::max(nowMs, inflight.back().finishMs);
            inflight.push_back({startMs + cost, value12 + 1});   // the Signal below
            if (inflight.size() > 16) inflight.pop_front();
        }
        if (holdOn) { const int hr = composeHolds(b, gap, sourceIndex); if (hr != PUSHCORE_OK) return hr; }
        // One shared fence value after everything this source frame queued (VFG's CUDA work is a
        // queue Wait inside process, so queue order covers generated frames too).
        if (hrFail(ring.queue()->Signal(fence12.Get(), ++value12), "D3D12 Signal(batch)")) return PUSHCORE_ERROR;
        slot.freeValue = value12;
        b.readyValue = value12;
        if (xefg) { if (const int xr = submitXefg(b, reset); xr != PUSHCORE_OK) return xr; }
        else batches.push_back(std::move(b));
        needReset = false; haveReal = true;
        ++submittedCount;
        st.submitCpuMs += msSince(t0);
        collectTimings();
        // PushFPS Subtitle Hold: its state and the pixels it held, about twice a second at 24 fps (the pixel count reads
        // the pass's own mapped sample; PushOverlayPass::harvest runs on this thread in update(), so it is read here too)
        if (graph && signsOn) {
            // the sign hold composes on animation only, and it is the first thing the budget drops (holdShed, with line art)
            const bool want = holdAnimNow.load(std::memory_order_relaxed) && !holdShed.load(std::memory_order_relaxed);
            if (want != signsCompose || !subAutoInit) { graph->setSubtitleCompose(want); signsCompose = want; subAutoInit = true; }
        }
        else if (graph && haveSettings && settings.subtitleHold == 2 && !subAutoInit) { graph->setSubtitleCompose(false); subAutoInit = true; }
#ifdef PUSHMPC_NVOF_LINE_PASS
        if (graph && lineNvofOn && graph->lineActive()) {
            const bool want = holdAnimNow.load(std::memory_order_relaxed) && !holdShed.load(std::memory_order_relaxed);
            if (want != lineNvofCompose) { graph->setLineCompose(want); lineNvofCompose = want; }
        }
#endif
        if (graph && (++subtitlePoll % 12) == 0) {
            const uint32_t state = uint32_t(graph->subtitleHoldState()), px = graph->subtitleHoldPixels();
            // Auto gate: burned-in subtitles (two lines over a 1080p picture upscaled to 4K) hold 78-91 k pixels of the
            // band; natural video without text held at most 6.4 k (study on four shows, eight segments, RTX 5090,
            // 2026-11-25). On at >= 0.4 % of the frame for 3 polls (~1.5 s at 24 fps), off below 0.05 % for 10 polls (~5 s).
            if (state == 1 && haveSettings && settings.subtitleHold == 2 && !signsOn) {
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

    // Held-frame re-timing: reads the test results of the queued frames in order (waiting only when a second frame is
    // queued behind the first, or at the end), hands each to the Retimer, and gives the graph what it decided while the
    // graph has a slot. final: the stream paused or ended (no frame for 250 ms) or re-timing went off: frames waiting for
    // the next distinct one go as they are.
    int pumpHeld(bool final) {
        using namespace pushmpc::held;
        std::vector<Action> acts;
        while (!pendingIn.empty()) {
            Pending& p = pendingIn.front();
            DiffStats d;
            bool isHeld = false;
            if (p.compared) {
                const bool wait = final || pendingIn.size() >= 2;
                CellGrid grid;
                const bool wantGrid = (spanVfgOk || holdOn) && retimingOn();
                if (!held.read(imm.Get(), p.seq, d, wait, wantGrid ? &grid : nullptr)) { if (!wait) break; d = {}; }
                // where the source repair runs, a partly changed frame stays with the graph (its held parts are repaired);
                // where it does not, small changes (a mouth, a blink) count as held, so the body's drawing waits as a whole
                isHeld = IsHeld(d, !spanVfgOk);
                if (wantGrid) queueClassify(p.seq, std::move(grid), IsHeld(d), p.pts);
                heldRatio.Push(isHeld);
                if (isHeld) ++st.heldFrames;
            }
            const uint64_t seq = p.seq;
            const bool disc = (p.flags & PUSHCORE_SUBMIT_DISCONTINUITY) != 0 || !p.compared;
            slotOf[seq] = p.slot;
            waiting.emplace(seq, p);
            pendingIn.pop_front();
            acts.clear();
            // repair mode: a held frame is repaired, not dropped (its parts get their in-betweens at the source rate)
            retimer.Push(seq, isHeld && !repairMode() && holdRetimeWanted(), disc, acts);
            for (const auto& a : acts) {
                if (a.kind == Action::Drop) { waiting.erase(a.frame); ++st.heldDropped; }
                else readyGraph.push_back({a.frame, a.multiplier, a.gap});
            }
        }
        if (final && retimer.Waiting()) {
            acts.clear();
            retimer.Flush(acts);
            for (const auto& a : acts) readyGraph.push_back({a.frame, a.multiplier, a.gap});
        }
        st.heldSpans = retimer.Spans();
        while (!readyGraph.empty() && graph && graph->nextFrameSlotAvailable()) {
            const ReadyGraph r = readyGraph.front();
            // source repair: a frame goes to the graph once the frames its parts' next drawings could be in are classified
            if ((repairMode() || holdOn) && !planReady(r.seq, final)) break;
            readyGraph.pop_front();
            auto it = waiting.find(r.seq);
            if (it == waiting.end()) continue;
            Pending p = it->second;
            waiting.erase(it);
            // every frame in repair mode (a01e74f repaired only frames whose pair spanned one source frame and only after
            // a 96-frame gate on wholly held frames: 3 of 300 frames on TV anime, where an offline replay finds 400 of 900)
            if (repairMode() && r.gap == 1) { const int rr = repairFrame(p); if (rr != PUSHCORE_OK) return rr; }
            else if (holdOn) { PlanResult hp; takePlan(p.seq, false, hp); }   // the hold plans only (no repair)
            holdAnimNow = animationGate() || (heldRatio.Size() >= 48 && heldRatio.Ratio() >= 0.20);
            const int rc = processFrame(p.slot, p.copyValue, p.pts, p.flags, p.haveTiming ? &p.timing : nullptr, r.multiplier, r.gap, p.seq, Clock::now());
            if (rc != PUSHCORE_OK) return rc;
        }
        return PUSHCORE_OK;
    }

    // ---- anime round 5: the hold compose ----
    // The verdict split (test only, not in the UI): PUSHMPC_SPLIT=<A>|<B> (or HKCU\Software\PushMPC\Test\Split, a string),
    // each side "full", "camera" or "off", optionally followed by +t / -t (transient hold), +l / -l (line art), +s / -s
    // (sign hold: one graph runs it or not, so +s / -s only make sense on both sides alike or as an A/B of whole runs):
    // e.g. "full|camera", "full-t|full+t", "full|full+l". Left half = A, right half = B, a 2-pixel divider on generated
    // frames. PUSHMPC_AB_SWAP=<source frame>: the sides swap from that frame on (a side bias shows up).
    static bool parseSide(std::string v, pushmpc::held::AnimMode& m, uint32_t& f) {
        using pushmpc::held::AnimMode;
        for (auto& c : v) c = char(std::tolower(static_cast<unsigned char>(c)));
        size_t k = v.find_first_of("+-");
        const std::string name = v.substr(0, k);
        if (name == "full") m = AnimMode::Full; else if (name == "camera") m = AnimMode::CameraOnly; else if (name == "off") m = AnimMode::Off; else return false;
        while (k != std::string::npos && k + 1 < v.size()) {
            const bool on = v[k] == '+'; const char w = v[k + 1];
            const uint32_t bit = w == 't' ? pushmpc::held::kHoldTransients : w == 'l' ? pushmpc::held::kHoldLineArt : w == 's' ? pushmpc::held::kHoldSigns
                               : w == 'f' ? pushmpc::held::kHoldFilmTime : 0u;
            if (on) f |= bit; else f &= ~bit;
            k = v.find_first_of("+-", k + 1);
        }
        return true;
    }
    void initHoldModes() {
        using namespace pushmpc::held;
        holdSplit = false; holdSwapAt = UINT64_MAX;
        const AnimMode mode = !haveSettings ? AnimMode::Full : settings.animationMode == PUSHCORE_ANIM_CAMERA_ONLY ? AnimMode::CameraOnly : settings.animationMode == PUSHCORE_ANIM_OFF ? AnimMode::Off : AnimMode::Full;
        const uint32_t flags = haveSettings ? settings.holdFlags : 0u;
        holdMode[0] = holdMode[1] = mode; holdFlg[0] = holdFlg[1] = flags;
        std::string split;
        if (const char* e = std::getenv("PUSHMPC_SPLIT")) split = e;
        else {
            wchar_t buf[128]{}; DWORD sz = sizeof(buf);
            if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\PushMPC\\Test", L"Split", RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS)
                for (const wchar_t* c = buf; *c; ++c) split.push_back(char(*c & 0x7f));
        }
        const size_t bar = split.find('|');
        if (bar != std::string::npos) {
            AnimMode m[2] = { mode, mode }; uint32_t f[2] = { flags, flags };
            if (parseSide(split.substr(0, bar), m[0], f[0]) && parseSide(split.substr(bar + 1), m[1], f[1])) {
                holdSplit = true;
                for (int i = 0; i < 2; ++i) { holdMode[i] = m[i]; holdFlg[i] = f[i]; }
                if (const char* sw = std::getenv("PUSHMPC_AB_SWAP")) holdSwapAt = std::strtoull(sw, nullptr, 10);
                veyra::log::info("pushcore", std::format("verdict split: left \"{}\", right \"{}\"{}", split.substr(0, bar), split.substr(bar + 1),
                    holdSwapAt != UINT64_MAX ? std::format(", sides swap at source frame {}", holdSwapAt) : std::string()));
            } else veyra::log::warn("pushcore", "PUSHMPC_SPLIT not understood (sides: full | camera | off, with +t -t +l -l): " + split);
        }
    }
    void initHold() {
        using namespace pushmpc::held;
        holdOn = false;
        const bool wanted = holdSplit || holdMode[0] != AnimMode::Full || holdFlg[0] != 0;
        if (!wanted || !heldEligible || !haveSettings || settings.heldFrames == 0 || !fg || multiplier < 2) return;
        if (!holdPass.ready() && !holdPass.init(d11.Get())) { veyra::log::warn("pushcore", "hold compose unavailable: " + holdPass.why()); return; }
        holdOn = true;
        holdPrevReal = nullptr; holdReady.clear();
        showClass.Reset(); animClassNow = 0;
        holdPairs = holdFrames = holdFlashes = holdLineFrames = holdFail = holdGated = 0; holdScrollPairs = 0; holdShedPairs = 0; holdShed = false;
        startPlanner();
        auto modeName = [](AnimMode m) { return m == AnimMode::CameraOnly ? "Camera only" : m == AnimMode::Off ? "Off" : "Full"; };
        veyra::log::info("pushcore", std::format("hold compose on: animation mode {}, transients {}{}, line art {}{}", modeName(holdMode[0]),
            (holdFlg[0] & kHoldTransients) ? "on" : "off", (holdFlg[0] & kHoldLiveTransients) ? " (flash / impact on every video)" : "",
            (holdFlg[0] & kHoldLineArt) ? "on" : "off", holdSplit ? " (verdict split: see above)" : ""));
    }
    // planner thread: block match + objects of the pair (seq - 1, seq); transient marks of seq - 2; the hold plans of
    // the pair ending at seq - 2 (its marks need two frames after it)
    void holdClassify(uint64_t seq) {
        using namespace pushmpc::held;
        const CellGrid& cur = gridOf[seq];
        meanOf[seq] = GridMean(cur);
        if (cur.cell) { holdCellPx = float(cur.cell); if (stream.width && stream.height) { holdCoverW = float(cur.w * cur.cell) / float(stream.width); holdCoverH = float(cur.h * cur.cell) / float(stream.height); } }
        const auto rit = regionOf.find(seq);
        const auto pit = gridOf.find(seq - 1);
        std::vector<ObjectClass> objs;
        if (rit != regionOf.end() && rit->second.valid && !rit->second.cut && pit != gridOf.end()) {
            const auto t0 = Clock::now();
            const BlockMotion bm = MatchBlocks(pit->second, cur, rit->second, noiseFloor.Threshold());
            std::vector<int16_t> nx, ny;
            objs = ClassifyObjects(rit->second, bm, slidX, slidY, &nx, &ny);
            slidX.swap(nx); slidY.swap(ny);
            if (ScrollPair(rit->second, bm, objs)) holdScrollPairs.fetch_add(1, std::memory_order_relaxed);
            camFixOf[seq] = { bm.camFixX, bm.camFixY };
            // P9 film time: the drawn layer (redrawn / unexplained objects; slid camera and effects excluded) of this pair
            bool drawn = false;
            for (const auto& o : objs) drawn = drawn || (o.kind != ObjectClass::Slid && o.cells >= kMinObjectCells);
            filmTime.Push(seq, drawn, false, pairIntervalMs(seq));
            filmSwitchOf[seq] = drawn && filmTime.Engaged() ? filmTime.SwitchT(seq) : -1.f;
            holdMatchMs = holdMatchMs.load() * 0.98 + msSince(t0) * 0.02;
        } else { slidX.clear(); slidY.clear(); filmTime.Reset(); }
        objOf[seq] = std::move(objs);
        if (rit != regionOf.end() && rit->second.valid) {
            const RegionFrame& r = rit->second;
            const float cad = r.changedShare() > 0.002 ? cadenceHist.Median(seq, r.changed.size(), [&](size_t i) { return r.changed[i] != 0; }, 7) : 0.f;
            showClass.Push(cad, holdAnimNow.load());
            animClassNow = uint32_t(showClass.Provisional()) | (showClass.Class() != ShowClass::Unknown ? 16u : 0u);   // 16: final
        }
        if (seq >= 3) {
            const uint64_t s = seq - 2;
            static const RegionFrame none{};
            auto rf = [&](uint64_t q) -> RegionFrame* { auto it = regionOf.find(q); return it == regionOf.end() ? nullptr : &it->second; };
            auto gr = [&](uint64_t q) -> const CellGrid* { auto it = gridOf.find(q); return it == gridOf.end() ? nullptr : &it->second; };
            float m[4];
            for (int k = 0; k < 4; ++k) { auto it = meanOf.find(s - 1 + uint64_t(k)); m[k] = it == meanOf.end() ? std::nanf("") : it->second; }
            const RegionFrame* r0 = rf(s); const RegionFrame* r1 = rf(s + 1); const RegionFrame* r2 = rf(s + 2);
            const TransientMarks tm = DetectTransient(m, r0 ? *r0 : none, r1 ? *r1 : none, r2 ? *r2 : none, gr(s - 1), gr(s), gr(s + 1));
            marksOf[s] = tm;
            // the source repair spans no flash / impact frame (PlanRepairs' gate)
            if (tm.flash || tm.insert || tm.flash2) { if (RegionFrame* x = rf(s)) x->transient = true; }
            if (tm.flash2) { if (RegionFrame* x = rf(s + 1)) x->transient = true; }
            auto mk = [&](uint64_t q) { auto it = marksOf.find(q); return it == marksOf.end() ? TransientMarks{} : it->second; };
            const auto oit = objOf.find(s);
            static const std::vector<ObjectClass> noObjs;
            const CellGrid* gs = gr(s);
            std::pair<float, float> cf{ 0.f, 0.f };
            if (auto c = camFixOf.find(s); c != camFixOf.end()) cf = c->second;
            float fs = -1.f;
            if (auto c = filmSwitchOf.find(s); c != filmSwitchOf.end()) fs = c->second;
            std::array<HoldPlan, 2> plans;
            if (r0 && gs)
                for (int side = 0; side < (holdSplit ? 2 : 1); ++side)
                    plans[size_t(side)] = PlanHolds(holdMode[side], holdFlg[side], true, *r0, oit == objOf.end() ? noObjs : oit->second, mk(s - 2), mk(s - 1), tm, gs->w, gs->h,
                                                    cf.first, cf.second, fs);
            holdOf[s] = std::move(plans);
        }
        while (meanOf.size() > 16) meanOf.erase(meanOf.begin());
        while (objOf.size() > 8) objOf.erase(objOf.begin());
        while (camFixOf.size() > 8) camFixOf.erase(camFixOf.begin());
        while (filmSwitchOf.size() > 8) filmSwitchOf.erase(filmSwitchOf.begin());
        while (marksOf.size() > 16) marksOf.erase(marksOf.begin());
        while (holdOf.size() > 32) holdOf.erase(holdOf.begin());
    }
    // the split: a side that does not run Full gets no source repair (its half of every part's mask is given back)
    void clipRepairToSplit(PlanResult& r, uint64_t seq) {
        using pushmpc::held::AnimMode;
        const bool swap = seq >= holdSwapAt;
        const bool fullL = holdMode[swap ? 1 : 0] == AnimMode::Full, fullR = holdMode[swap ? 0 : 1] == AnimMode::Full;
        if (fullL && fullR) return;
        for (auto& g : r.groups) {
            if (!g.w || g.mask.size() != size_t(g.w) * g.h) continue;
            for (uint32_t y = 0; y < g.h; ++y)
                for (uint32_t x = 0; x < g.w; ++x) {
                    const bool left = x < g.w / 2;
                    if ((left && !fullL) || (!left && !fullR)) g.mask[size_t(y) * g.w + x] = 0;
                }
        }
    }
    // submit thread, right after graph->process(s): the generated frames of the pair (s - 1, s) composed in place on the
    // caller's D3D11 device. The D3D11 work waits for the graph's queue (fence12) and the batch fence waits for it
    // (fence11), so egress (direct or copy) hands out composed frames only.
    int composeHolds(Batch& b, uint32_t gap, uint64_t seq) {
        using namespace pushmpc::held;
        using veyra::pipeline::FrameKind;
        ID3D12Resource* realNow = nullptr;
        for (uint32_t i = 0; i < b.out.batch.count; ++i) { auto& f = b.out.batch.frames[i]; if (f.kind == FrameKind::Real && f.lease) realNow = f.lease->texture.Get(); }
        ID3D12Resource* realPrev = holdPrevReal;
        holdPrevReal = realNow;
        std::array<HoldPlan, 2> hp;
        if (auto it = holdReady.find(seq); it != holdReady.end()) { hp = std::move(it->second); holdReady.erase(it); }
        if (!realNow || !realPrev || gap != 1 || !(b.out.batch.b100ns > b.out.batch.a100ns)) return PUSHCORE_OK;
        const bool anim = holdAnimNow.load();
        const bool swap = seq >= holdSwapAt;
        holdBudget();
        const bool shed = holdShed.load(std::memory_order_relaxed);
        if (shed) ++holdShedPairs;
        struct SidePass { const HoldPlan* plan = nullptr; bool hold = false, line = false; uint32_t side = 0; };
        SidePass sp[2];
        const int sides = holdSplit ? 2 : 1;
        bool any = holdSplit;   // the split draws its divider on every generated frame
        for (int i = 0; i < sides; ++i) {
            const HoldPlan& p = hp[size_t(i)];
            sp[i].plan = &p;
            sp[i].side = uint32_t(swap ? 1 - i : i);
            // a held object needs the animation gate; a flash / impact frame was planned with it already (live action too
            // only with PUSHCORE_HOLD_LIVE_TRANSIENTS)
            const bool liveOk = (holdFlg[i] & kHoldLiveTransients) != 0;
            sp[i].hold = p.whole ? (anim || liveOk) : (p.cells > 0 && anim);
            if (!p.whole && p.cells > 0 && !anim) ++holdGated;
            sp[i].line = (holdFlg[i] & kHoldLineArt) != 0 && anim && !p.whole && !shed;
            any = any || sp[i].hold || sp[i].line;
        }
        if (!any) return PUSHCORE_OK;
        if (!openDirect(realNow) || !openDirect(realPrev)) { ++holdFail; return PUSHCORE_OK; }
        if (hrFail(ring.queue()->Signal(fence12.Get(), ++value12), "D3D12 Signal(hold)")) return PUSHCORE_ERROR;
        imm4->Wait(fence12in11.Get(), value12);
        ID3D11Texture2D* A = opened[realPrev].Get();
        ID3D11Texture2D* B = opened[realNow].Get();
        const float out2src = stream.width ? float(graph->workWidth()) / float(stream.width) : 1.f;
        const float px = holdCellPx.load() * out2src;
        const double a100 = double(b.out.batch.a100ns), span = double(b.out.batch.b100ns) - a100;
        bool pairHeld = false, pairFlash = false;
        for (uint32_t i = 0; i < b.out.batch.count; ++i) {
            auto& f = b.out.batch.frames[i];
            if (f.kind != FrameKind::Generated || !f.lease) continue;
            const float t = float((double(f.pts100ns) - a100) / span);
            if (!(t > 0.f && t < 1.f)) continue;   // a forecast (extrapolated) frame: not between the pair
            if (!openDirect(f.lease->texture.Get())) { ++holdFail; continue; }
            ID3D11Texture2D* G = opened[f.lease->texture.Get()].Get();
            bool composed = false;
            for (int k = 0; k < sides; ++k) {
                const HoldPlan& p = *sp[k].plan;
                pushmpc::HoldCompose::Pass pass;
                pass.t = t; pass.camX = p.camDx * px; pass.camY = p.camDy * px; pass.camScale = p.camScale;
                if (holdSplit) { pass.split = 0.5f * float(graph->workWidth()); pass.side = sp[k].side; if (k == 0) pass.flags |= pushmpc::HoldCompose::kDivider; }
                if (sp[k].hold) {
                    pass.flags |= p.whole ? pushmpc::HoldCompose::kWhole : pushmpc::HoldCompose::kMask;
                    if (p.nearest) pass.flags |= pushmpc::HoldCompose::kNearest;
                    pass.tSwitch = p.tSwitch;
                    if (!p.whole && !holdPass.setMask(imm.Get(), p.mask, p.w, p.h, holdCoverW.load(), holdCoverH.load(), 2.0f)) { ++holdFail; continue; }
                    pairHeld = true; pairFlash = pairFlash || p.whole;
                }
                if (sp[k].line) pass.flags |= pushmpc::HoldCompose::kLineArt;
                if (!pass.flags) continue;
                if (!holdPass.compose(imm.Get(), G, A, B, pass)) { ++holdFail; continue; }
                composed = true;
                if (sp[k].line) ++holdLineFrames;
            }
            if (composed) ++holdFrames;
        }
        holdPairs += pairHeld; holdFlashes += pairFlash;
        if (hrFail(imm4->Signal(fence11.Get(), ++value11), "D3D11 Signal(hold)")) return PUSHCORE_ERROR;
        imm4->Flush();
        ring.queue()->Wait(fence11in12.Get(), value11);
        return PUSHCORE_OK;
    }
    void holdLogClose() {
        veyra::log::info("pushcore", std::format("hold compose: {} pairs held ({} flash / impact pairs whole), {} generated frames composed, {} with line art; "
            "{} pairs gated (not animation), {} failures; block match {:.2f} ms a pair (planner thread); class {} (0 unknown, 1 feature, 2 TV limited, 3 mixed CG); "
            "scroll layer: {} pairs (smooth, never held); {} pairs without line art / sign hold (budget); sign hold {}",
            holdPairs, holdFlashes, holdFrames, holdLineFrames, holdGated, holdFail, holdMatchMs.load(), animClassNow.load(), holdScrollPairs.load(), holdShedPairs,
            signsOn ? "on (the subtitle pass over the whole picture; see sign-hold lines)" : "off"));
        holdPass.forgetViews();
    }
    // ---- source repair: setup ----
    void initRepair() {
        spanVfgOk = false;
        if (!haveSettings || settings.heldFrames == 0 || !fg || multiplier < 2 || !heldEligible) return;
        bool useVfg = tryFg == PUSHCORE_FG_VFG && ctx.adapter().vendorId == 0x10DE;   // tryFg: what this graph runs (runFg is set after the build)
        // PUSHMPC_SPAN_STREAM=fsr | frc (tests: a midpoint stream on any GPU) or flow (PushFlow where VFG would run)
        const char* force = std::getenv("PUSHMPC_SPAN_STREAM");
        const std::string forced = force ? force : "";
        if (forced == "fsr" || forced == "frc" || forced == "flow") useVfg = false;
        const bool useFlow = !useVfg && forced != "fsr" && forced != "frc" && veyra::pushflow::runtimeInstalled(runtimeDir);
        if (!useVfg && !useFlow && !midStreamPossible()) { veyra::log::info("pushcore", "source repair off: the graph is not on VFG, PushFlow's runtime is not installed and no midpoint stream (AMD FRC, FSR 3.1) is available (held frames re-timed whole)"); return; }
        if (repairEnvOff()) { veyra::log::info("pushcore", "source repair off (PUSHMPC_REGION_REPAIR=0)"); return; }
        if (!holdRepairWanted()) { veyra::log::info("pushcore", "source repair off: Animation mode Camera only / Off (drawings keep their own timing)"); return; }
        if (!repairPass.ready() && !repairPass.init(d11.Get())) { veyra::log::warn("pushcore", "source repair unavailable (held frames re-timed whole, graph at k): " + repairPass.why()); return; }
        const uint32_t w = (stream.width + 1) & ~1u, h = (stream.height + 1) & ~1u;
        const bool p010 = stream.format == PUSHCORE_INPUT_P010;
        const DXGI_FORMAT rgbFmt = p010 ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
        const auto t = Clock::now();
        spanIsFlow = useFlow;
        spanMid = 0;
        if (!useVfg && !useFlow) {
            if ((ctx.adapter().vendorId == 0x1002 || forced == "frc") && forced != "fsr" && veyra::gfx::AmfFrcBackend::runtimeInstalled() && spanFrc.initialize(ctx.device(), ring.queue(), w, h, rgbFmt, false)) spanMid = 1;
#ifdef VEYRA_FSR_PAIR_API
            if (!spanMid && spanFsr.initializeIndependent(ctx.device(), w, h, rgbFmt, false)) {
                auto zeroTex = [&](DXGI_FORMAT f, ComPtr<ID3D12Resource>& r) {   // committed resources start zeroed
                    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                    D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
                    d.Format = f; d.SampleDesc.Count = 1;
                    return SUCCEEDED(ctx.device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r)));
                };
                if (zeroTex(DXGI_FORMAT_R16G16_FLOAT, fsrMotion) && zeroTex(DXGI_FORMAT_R32_FLOAT, fsrDepth)) spanMid = 2; else spanFsr.shutdown();
#ifdef PUSHMPC_SPAN_LINE
                spanLineOk = spanMid == 2 && lineNvofOn && spanLineStager.initialize(ctx.device(), 64) && spanLine.initialize(ctx.device(), w, h, rgbFmt, 0u, false);
                spanLineCalls = 0;
#endif
            }
#endif
            if (!spanMid) { veyra::log::warn("pushcore", "source repair off: no midpoint span stream (AMD FRC, FSR 3.1) started"); return; }
        } else if (useVfg) {
            spanQuality = std::min<uint32_t>(gd.vfgQuality, 2u);
            if (!spanVfg.initialize(ctx, runtimeDir, w, h, rgbFmt, veyra::pipeline::FrameBatch::Capacity, spanQuality, false, false)) {
                veyra::log::warn("pushcore", "source repair off: the span stream (a second VFG effect) did not start");
                return;
            }
            spanW = w; spanH = h; spanFmt = rgbFmt;   // the lazy Low stream (lowSpanPoll) is made at the same size
            lowPolicy.Reset(); lowMemLogged = false; lowPairA = lowPairB = UINT64_MAX; lowParity = 0;
        } else if (!spanFlow.initialize(ctx.device(), ring.queue(), runtimeDir, w, h, rgbFmt, veyra::pushflow::Encoding::Encoded, 1)) {
            // tier 1 (the fast one): one frame per repaired part, at the source size
            veyra::log::warn("pushcore", "source repair off: the span stream (PushFlow) did not start");
            return;
        }
        auto spanShutdown = [&] { spanFlow.shutdown(); spanVfg.shutdown(); shutdownMid(); };
        // made in D3D11 (UAV for the conversion, shared NT handle) and opened in D3D12, as the ingress ring is
        auto makeShared = [&](RgbSlot& slot) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = rgbFmt; d.SampleDesc = {1, 0};
            d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;
            d.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
            if (FAILED(d11->CreateTexture2D(&d, nullptr, &slot.tex))) return false;
            ComPtr<IDXGIResource1> dr; HANDLE hd = nullptr;
            if (FAILED(slot.tex.As(&dr)) || FAILED(dr->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &hd))) return false;
            const HRESULT hr = ctx.device()->OpenSharedHandle(hd, IID_PPV_ARGS(&slot.res));
            CloseHandle(hd);
            return SUCCEEDED(hr);
        };
        rgbSlots.assign(6, {});
        for (auto& sl : rgbSlots) if (!makeShared(sl)) { veyra::log::warn("pushcore", "source repair off: RGB textures"); spanShutdown(); return; }
        for (auto& rs : repSlots) if (!makeShared(rs)) { veyra::log::warn("pushcore", "source repair off: span texture"); spanShutdown(); return; }
        if (spanMid && !makeShared(midSlot)) { veyra::log::warn("pushcore", "source repair off: midpoint texture"); spanShutdown(); return; }
        midA = midB = midLast = UINT64_MAX;
        {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = p010 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12; d.SampleDesc = {1, 0};
            d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            if (FAILED(d11->CreateTexture2D(&d, nullptr, &repairOut))) { veyra::log::warn("pushcore", "source repair off: no NV12 / P010 UAV texture on this device"); spanShutdown(); return; }
        }
        if (!regionList) {
            for (auto& a : regionAlloc) if (FAILED(ctx.device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) { spanShutdown(); return; }
            if (FAILED(ctx.device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, regionAlloc[0].Get(), nullptr, IID_PPV_ARGS(&regionList)))) { spanShutdown(); return; }
            regionList->Close();
        }
        if (!repairQueries) {
            D3D12_QUERY_HEAP_DESC qh{}; qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qh.Count = 32;
            D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 32 * 8; bd.Height = 1; bd.DepthOrArraySize = 1;
            bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            uint64_t freq = 0;
            void* map = nullptr;
            if (SUCCEEDED(ctx.device()->CreateQueryHeap(&qh, IID_PPV_ARGS(&repairQueries))) &&
                SUCCEEDED(ctx.device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&repairQueryBuf))) &&
                SUCCEEDED(ring.queue()->GetTimestampFrequency(&freq)) && freq > 0 && SUCCEEDED(repairQueryBuf->Map(0, nullptr, &map))) {
                repairQueryMap = static_cast<const uint64_t*>(map); tsPerMs = double(freq) / 1000.0;
            } else { repairQueries.Reset(); repairQueryBuf.Reset(); veyra::log::warn("pushcore", "source repair: no GPU timestamps (its cost is not measured)"); }
        }
        for (auto& v : repairQueryValue) v = 0;
        repairQueryCur = -1;
        spanVfgOk = true;
        pairA = pairB = UINT64_MAX;
        startPlanner();
        veyra::log::info("pushcore", std::format("source repair ready in {:.0f} ms: span stream {} at {}x{}, lookahead {}, graph {}x", msSince(t),
            spanMid == 1 ? "AMD FRC (midpoints: twos exact, threes at 0.25 / 0.75)" : spanMid == 2 ? "FSR 3.1 (midpoints: twos exact, threes at 0.25 / 0.75)" : spanIsFlow ? "PushFlow (tier 1)" : "VFG (timestep mode)",
            w, h, cascadeOn() ? pushmpc::held::kCascadeLookahead : pushmpc::held::kLookahead, graphMultiplier));
    }
    ID3D12GraphicsCommandList* regionBegin() {
        const uint32_t a = regionListNext++ % 8;
        if (regionAllocValue[a] && fence12->GetCompletedValue() < regionAllocValue[a]) { fence12->SetEventOnCompletion(regionAllocValue[a], waitEvent); WaitForSingleObject(waitEvent, 10000); }
        if (FAILED(regionAlloc[a]->Reset()) || FAILED(regionList->Reset(regionAlloc[a].Get(), nullptr))) return nullptr;
        regionCurAlloc = a;
        if (repairQueries && repairQueryCur < 0) {   // the first list of a repaired frame: its start time
            repairQueryCur = int(repairQueryNext++ % 16);
            repairQueryValue[repairQueryCur] = 0;
            regionList->EndQuery(repairQueries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, UINT(2 * repairQueryCur));
        }
        return regionList.Get();
    }
    // the end time of the repaired frame's span-stream work, resolved for collectRepairTimes
    void regionEndTiming() {
        if (repairQueryCur < 0) return;
        const int q = repairQueryCur;
        if (auto* l = regionBegin()) {
            l->EndQuery(repairQueries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, UINT(2 * q + 1));
            l->ResolveQueryData(repairQueries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, UINT(2 * q), 2, repairQueryBuf.Get(), uint64_t(16 * q));
            if (regionSubmit()) repairQueryValue[q] = value12;
        }
        repairQueryCur = -1;
    }
    void collectRepairTimes() {
        if (!repairQueryMap) return;
        const uint64_t done = fence12->GetCompletedValue();
        for (int q = 0; q < 16; ++q) {
            if (!repairQueryValue[q] || repairQueryValue[q] > done) continue;
            const uint64_t a = repairQueryMap[2 * q], b = repairQueryMap[2 * q + 1];
            if (b > a) costRepair.push(double(b - a) / tsPerMs);
            repairQueryValue[q] = 0;
        }
    }
    // one midpoint call: out = mid(x, y) on the midpoint stream (keys say what x and y are, so a pair that starts where
    // the last one ended feeds only its new end)
    bool midpoint(ID3D12Resource* x, uint64_t xKey, ID3D12Resource* y, uint64_t yKey, RgbSlot& out, uint64_t ready11) {
        ++midCalls;
        const int64_t step = int64_t(sourceIntervalMs() * 10000.0);
        if (spanMid == 1) {
            auto feed = [&](ID3D12Resource* r, bool reset) {
                auto* l = regionBegin(); if (!l) return false;
                ring.queue()->Wait(fence11in12.Get(), ready11);
                barrier(l, r, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
                const bool ok = spanFrc.capture(l, r);
                barrier(l, r, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
                if (!regionSubmit() || !ok) return false;
                return spanFrc.generate(midPts += step, reset);
            };
            // after a reset AMF needs its warm-up repeat (PushFPS feeds its first frame twice): x twice, then y
            if (midLast != xKey && !(feed(x, true) && feed(x, false))) { midLast = UINT64_MAX; return false; }
            if (!feed(y, false)) { midLast = UINT64_MAX; return false; }
            midLast = yKey;
            auto* l = regionBegin(); if (!l) return false;
            if (out.readyValue11) ring.queue()->Wait(fence11in12.Get(), out.readyValue11);
            barrier(l, out.res.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            const bool copied = spanFrc.copyOutput(l, out.res.Get());
            barrier(l, out.res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            if (!regionSubmit()) return false;
            spanFrc.submitted();
            return copied;
        }
#ifdef VEYRA_FSR_PAIR_API
        if (spanMid == 2) {
            auto* l = regionBegin(); if (!l) return false;
            ring.queue()->Wait(fence11in12.Get(), ready11);
            if (out.readyValue11) ring.queue()->Wait(fence11in12.Get(), out.readyValue11);
            const bool ok = spanFsr.generatePair(l, x, y, fsrMotion.Get(), fsrDepth.Get(), out.res.Get(), midLast == xKey, float(sourceIntervalMs()));
#ifdef PUSHMPC_SPAN_LINE
            if (ok && spanLineOk && holdAnimNow.load(std::memory_order_relaxed) && !holdShed.load(std::memory_order_relaxed) &&
                spanLine.composePair(ctx.device(), spanLineStager, l, x, y, nullptr, out.res.Get(), 0.5f)) ++spanLineCalls;
#endif
            if (!regionSubmit()) return false;
            midLast = ok ? yKey : UINT64_MAX;
            return ok;
        }
#endif
        (void)x; (void)y; (void)xKey; (void)yKey; (void)out; (void)ready11; (void)step;
        return false;
    }
    void shutdownMid() {
        spanFrc.shutdown();
#ifdef VEYRA_FSR_PAIR_API
        spanFsr.shutdown();
#endif
#ifdef PUSHMPC_SPAN_LINE
        if (spanLineCalls) veyra::log::info("pushcore", std::format("source repair: doubled-line fallback on {} FSR midpoints", spanLineCalls));
        spanLine = veyra::pipeline::PushMpcLinePass{}; spanLineStager = veyra::pipeline::DescriptorStager{}; spanLineOk = false; spanLineCalls = 0;
#endif
        fsrMotion.Reset(); fsrDepth.Reset(); midSlot = {}; spanMid = 0; midA = midB = midLast = UINT64_MAX;
    }
    bool regionSubmit() {
        if (FAILED(regionList->Close())) return false;
        ID3D12CommandList* lists[] = {regionList.Get()};
        ring.queue()->ExecuteCommandLists(1, lists);
        if (hrFail(ring.queue()->Signal(fence12.Get(), ++value12), "D3D12 Signal(repair)")) return false;
        regionAllocValue[regionCurAlloc] = value12;
        return true;
    }
    static void barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER x{}; x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; x.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
        l->ResourceBarrier(1, &x);
    }

    // ---- source repair: classification (submit thread, as each frame's test result is read) ----
    // submit thread: a frame's cell grid to the planner, and the plan of the frame kLookahead before it (its parts' next
    // drawings are classified by then)
    void queueClassify(uint64_t seq, pushmpc::held::CellGrid grid, bool wholly, int64_t pts) {
        if (!planner.joinable()) return;
        {
            std::lock_guard<std::mutex> lk(planMx);
            PlanJob j; j.seq = seq; j.epoch = planEpoch; j.wholly = wholly; j.pts = pts; j.grid = std::move(grid);
            planJobs.push_back(std::move(j));
            lastQueued = seq; anyQueued = true;
            if (seq >= uint64_t(pushmpc::held::kLookahead)) {
                const uint64_t q = seq - uint64_t(pushmpc::held::kLookahead);
                // every frame up to q gets its plan, also frames that had no grid of their own (a discontinuity)
                for (uint64_t f = anyPlanQueued ? lastPlanQueued + 1 : q; f <= q; ++f) { PlanJob pj; pj.seq = f; pj.epoch = planEpoch; pj.plan = true; planJobs.push_back(std::move(pj)); }
                if (!anyPlanQueued || q > lastPlanQueued) { lastPlanQueued = q; anyPlanQueued = true; }
            }
        }
        planCv.notify_all();
        while (slotOf.size() > kIngressSlots) slotOf.erase(slotOf.begin());
    }
    // submit thread: is the plan of seq there? final (end of stream, pause): ask for the plans still missing and wait for
    // this one (at most 100 ms); a frame never classified gets no plan (no repair)
    bool planReady(uint64_t seq, bool final) {
        std::unique_lock<std::mutex> lk(planMx);
        // the cascade: s's parts' future ends (<= s + kLookahead) are repaired first, from their own plans, which need
        // kLookahead more frames classified (s + kCascadeLookahead); a stream that stops (final) goes on without them
        if (plans.count(seq)) return final || !cascadeOn() || !anyQueued || lastQueued >= seq + uint64_t(pushmpc::held::kCascadeLookahead);
        if (!final) return !anyQueued || seq > lastQueued;   // never classified: no plan will come, go on
        if (!anyQueued || seq > lastQueued) return true;
        if (!anyPlanQueued || seq > lastPlanQueued) {
            for (uint64_t q = anyPlanQueued ? lastPlanQueued + 1 : 0; q <= lastQueued; ++q) { PlanJob pj; pj.seq = q; pj.epoch = planEpoch; pj.plan = true; planJobs.push_back(std::move(pj)); }
            lastPlanQueued = lastQueued; anyPlanQueued = true;
            planCv.notify_all();
        }
        planCv.wait_for(lk, std::chrono::milliseconds(100), [&] { return plans.count(seq) != 0; });
        return true;
    }
    void startPlanner() {
        if (planner.joinable()) return;
        { std::lock_guard<std::mutex> lk(planMx); planStop = false; planJobs.clear(); plans.clear(); }
        planner = std::thread([this] { plannerLoop(); });
    }
    void stopPlanner() {
        if (!planner.joinable()) return;
        { std::lock_guard<std::mutex> lk(planMx); planStop = true; }
        planCv.notify_all();
        planner.join();
    }
    void plannerLoop() {
        using namespace pushmpc::held;
        uint64_t epoch = UINT64_MAX;
        noiseFloor.Reset();   // the thread runs per graph: a new clip or a rebuilt graph learns its noise again
        for (;;) {
            PlanJob j;
            {
                std::unique_lock<std::mutex> lk(planMx);
                planCv.wait(lk, [&] { return planStop || !planJobs.empty(); });
                if (planStop) return;
                j = std::move(planJobs.front()); planJobs.pop_front();
            }
            if (j.epoch != epoch) { epoch = j.epoch; gridOf.clear(); regionOf.clear(); whollyHeld.clear(); cadenceHist.Reset(); meanOf.clear(); objOf.clear(); marksOf.clear(); holdOf.clear(); camFixOf.clear(); filmSwitchOf.clear(); ptsOf.clear(); filmTime.Reset(); slidX.clear(); slidY.clear(); }
            if (!j.plan) { ptsOf[j.seq] = j.pts; while (ptsOf.size() > 8) ptsOf.erase(ptsOf.begin()); classifyRegion(j.seq, std::move(j.grid), j.wholly); continue; }
            PlanResult r;
            try {
                auto frameAt = [&](uint64_t q) -> const RegionFrame* { auto it = regionOf.find(q); return it == regionOf.end() ? nullptr : &it->second; };
                auto gridAt = [&](uint64_t q) -> const CellGrid* { auto it = gridOf.find(q); return it == gridOf.end() ? nullptr : &it->second; };
                if (spanVfgOk) r.groups = PlanRepairs(j.seq, frameAt, gridAt, kLookahead, PhaseShape::Linear, noiseFloor.Threshold(), &cadenceHist);
                if (auto h = holdOf.find(j.seq); h != holdOf.end()) { r.hold[0] = std::move(h->second[0]); r.hold[1] = std::move(h->second[1]); holdOf.erase(h); }
                const auto wh = whollyHeld.find(j.seq);
                r.wholly = wh != whollyHeld.end() && wh->second;
                if (const auto g = gridOf.find(j.seq); g != gridOf.end() && g->second.cell) r.cellPx = float(g->second.cell);
            } catch (...) { r.groups.clear(); }
            planThr = noiseFloor.Threshold(); planNoise = noiseFloor.Value();
            { std::lock_guard<std::mutex> lk(planMx); if (j.epoch == planEpoch) plans[j.seq] = std::move(r); while (plans.size() > 32) plans.erase(plans.begin()); }
            planCv.notify_all();
        }
    }
    // planner thread
    void classifyRegion(uint64_t seq, pushmpc::held::CellGrid grid, bool wholly) {
        if (!grid.ok()) return;
        auto prev = gridOf.find(seq - 1);
        whollyHeld[seq] = wholly;
        while (whollyHeld.size() > 16) whollyHeld.erase(whollyHeld.begin());
        if (prev != gridOf.end() && seq > 0) {
            if (wholly) noiseFloor.Push(pushmpc::held::CellChangeP99(prev->second, grid));
            regionOf[seq] = pushmpc::held::CompareRegions(prev->second, grid, noiseFloor.Threshold());
            cadenceHist.Push(seq, regionOf[seq]);
        }
        gridOf[seq] = std::move(grid);
        if (holdOn) holdClassify(seq);
        while (gridOf.size() > 16) gridOf.erase(gridOf.begin());     // d0 - 1 (the step signature) .. s + kCascadeLookahead
        while (regionOf.size() > 16) regionOf.erase(regionOf.begin());
    }
    // the RGB copy of a source frame (converted once, cached in a small ring)
    RgbSlot* rgbOf(uint64_t seq) {
        for (auto& r : rgbSlots) if (r.seq == seq) return &r;
        auto it = slotOf.find(seq);
        if (it == slotOf.end()) return nullptr;
        auto& r = rgbSlots[rgbNext++ % rgbSlots.size()];
        if (!repairPass.toRgb(imm.Get(), inSlots[it->second].tex.Get(), r.tex.Get(), (stream.width + 1) & ~1u, (stream.height + 1) & ~1u)) return nullptr;
        if (hrFail(imm4->Signal(fence11.Get(), ++value11), "D3D11 Signal(rgb)")) return nullptr;
        r.seq = seq; r.readyValue11 = value11;
        return &r;
    }
    // the plan of seq off the planner (wait: up to 100 ms for one still being made: a future end of the cascade)
    bool takePlan(uint64_t seq, bool wait, PlanResult& out) {
        std::unique_lock<std::mutex> lk(planMx);
        if (wait && !plans.count(seq)) planCv.wait_for(lk, std::chrono::milliseconds(100), [&] { return plans.count(seq) != 0; });
        auto it = plans.find(seq);
        if (it == plans.end()) return false;
        out = std::move(it->second); plans.erase(it);
        if (holdOn) { holdReady[seq] = { std::move(out.hold[0]), std::move(out.hold[1]) }; while (holdReady.size() > 32) holdReady.erase(holdReady.begin()); }
        if (holdSplit) clipRepairToSplit(out, seq);
        return true;
    }
    // repairs the held parts of p's frame in place (its ingress slot), before the graph takes it. The full cascade
    // (research P8, HeldFrames.h CascadeEnds): the frames its parts' pairs end at are repaired first, from their own
    // plans, so a part's pair has repaired frames at both ends (d0 is behind p and was repaired when it went to the graph)
    int repairFrame(Pending& p) {
        using namespace pushmpc::held;
        lowSpanPoll();
        if (repairedAhead.erase(p.seq)) return PUSHCORE_OK;   // repaired already, as a future end of a frame before it
        collectRepairTimes();
        PlanResult plan;
        if (!takePlan(p.seq, false, plan)) { ++repairWhy[2]; return PUSHCORE_OK; }
        if (cascadeOn()) {
            for (const uint64_t e : CascadeEnds(plan.groups, p.seq)) {
                if (repairedAhead.count(e)) continue;
                auto w = waiting.find(e);
                if (w == waiting.end()) continue;
                PlanResult pe;
                if (!takePlan(e, true, pe)) { ++cascadeMissed; continue; }
                const int rc = repairWith(w->second, pe);
                if (rc != PUSHCORE_OK) return rc;
                repairedAhead.insert(e); ++cascadeEnds;
            }
            while (repairedAhead.size() > 16) repairedAhead.erase(repairedAhead.begin());
        }
        return repairWith(p, plan);
    }
    int repairWith(Pending& p, PlanResult& plan) {
        using namespace pushmpc::held;
        auto& groups = plan.groups;
        const bool wholly = plan.wholly;
        bool exact = false;
        for (const auto& g : groups) exact |= IsExactRepeat(g);
        evidence.Push(wholly || exact);
        if (groups.empty()) { ++repairWhy[2]; return PUSHCORE_OK; }
        // Decisions per (part, span), made at the span's first frame and kept for the whole span (a part repaired at s and
        // not at s + 1 would step forward and back): the live-action gate (a wholly held frame always passes) and the
        // budget, 85 % of a source frame at p95 for the graph and the repair together. Over it, at most one new span per
        // frame, the largest part first; when the graph alone is over it, no new partial span.
        {
            for (auto it = spanDecision.begin(); it != spanDecision.end();) { if (std::get<1>(it->first) <= p.seq) it = spanDecision.erase(it); else ++it; }
            const double budget = 0.85 * sourceIntervalMs(), graphP95 = costFull.pct(0.95), repP95 = costRepair.pct(0.95);
            const bool graphOver = graphP95 > 0 && graphP95 >= budget;
            const bool over = graphOver || (graphP95 > 0 && repP95 > 0 && graphP95 + repP95 > budget);
            std::sort(groups.begin(), groups.end(), [](const RepairGroup& x, const RepairGroup& y) { return x.cells > y.cells; });
            const bool gate = wholly || animationGate();
            // over the budget, a span that would be dropped goes to the lazy VFG Low stream when it runs and fits
            const bool lowRoute = !spanIsFlow && !spanMid && spanQuality > 0;
            const bool lowOk = lowRoute && lowReady();
            bool admittedNew = false, lowNew = false;
            std::vector<RepairGroup> keep;
            routes.clear();
            for (auto& g : groups) {
                const auto key = std::make_tuple(g.d0, g.d1, g.pairFrom);
                auto it = spanDecision.find(key);
                if (it == spanDecision.end()) {
                    int why = -1;
                    SpanRoute route = SpanRoute::Main;
                    if (!gate) why = 0;
                    else if (!wholly && graphOver) why = 1;
                    else if (over && admittedNew) {
                        route = lowRoute ? lowPolicy.RouteOverBudget(lowOk, graphP95, repP95, 0.0, budget, lowNew) : SpanRoute::Drop;
                        if (route == SpanRoute::Low) lowNew = true; else why = 8;
                    }
                    if (why >= 0) route = SpanRoute::Drop;
                    it = spanDecision.emplace(key, uint8_t(route)).first;
                    if (why >= 0) ++repairWhy[why]; else if (route == SpanRoute::Main) admittedNew = true;
                }
                if (it->second != uint8_t(SpanRoute::Drop)) { keep.push_back(std::move(g)); routes.push_back(it->second); }
            }
            groups.swap(keep);
            if (groups.empty()) return PUSHCORE_OK;
        }
        const uint32_t w = (stream.width + 1) & ~1u, h = (stream.height + 1) & ~1u;
        const float cellPx = plan.cellPx;
        bool any = false;
        uint32_t produced = 0;
        std::vector<const std::vector<uint8_t>*> partMasks;
        float partShifts[2 * pushmpc::RegionRepair::kMaxParts] = {};
        ID3D11Texture2D* partTex[pushmpc::RegionRepair::kMaxParts] = {};
        uint32_t mw = 0, mh = 0;
        for (size_t gi = 0; gi < groups.size(); ++gi) {
            const auto& g = groups[gi];
            if (produced >= pushmpc::RegionRepair::kMaxParts) break;
            RgbSlot& rs = repSlots[produced];
            const bool low = gi < routes.size() && routes[gi] == uint8_t(SpanRoute::Low);
            if (low && !lowReady()) { ++repairWhy[5]; continue; }   // released under a running span (not expected: 600 idle frames)
            // the pair: from d0, or from the substitution frame (a part redrawn inside the span and kept: HeldFrames.h)
            RgbSlot* a = rgbOf(g.pairFrom); RgbSlot* b = rgbOf(g.d1);
            if (!a || !b || a == b) { ++repairWhy[3]; continue; }
            if (spanMid) {
                // a midpoint stream: phase t by halving (twos exact; threes 0.25 / 0.75 from the cached midpoint M)
                const MidpointPlan mp = PlanMidpoints(g.t, 2);
                const uint64_t ready = std::max(a->readyValue11, b->readyValue11);
                bool ok = true;
                if (mp.calls == 1) ok = midpoint(a->res.Get(), g.pairFrom, b->res.Get(), g.d1, rs, ready);
                else {
                    if (midA != g.pairFrom || midB != g.d1) {
                        midA = midB = UINT64_MAX;
                        ok = midpoint(a->res.Get(), g.pairFrom, b->res.Get(), g.d1, midSlot, ready);
                        if (ok) { midA = g.pairFrom; midB = g.d1; }
                    }
                    const uint64_t mKey = (1ull << 63) | (g.pairFrom << 20) | g.d1;   // M's key as an input of the stream
                    if (ok) ok = mp.side[1] == 0 ? midpoint(a->res.Get(), g.pairFrom, midSlot.res.Get(), mKey, rs, ready)
                                                 : midpoint(midSlot.res.Get(), mKey, b->res.Get(), g.d1, rs, ready);
                }
                if (!ok) { midA = midB = midLast = UINT64_MAX; ++repairWhy[5]; continue; }
                if (std::fabs(mp.t - g.t) > 1e-3f) { ++midApprox; midErrSum += std::fabs(mp.t - g.t); }
            } else if (spanIsFlow) {
                // PushFlow: pack the pair at t (only the time plane when the pair is the one it holds), run, resolve
                auto* l = regionBegin(); if (!l) return PUSHCORE_ERROR;
                ring.queue()->Wait(fence11in12.Get(), std::max(a->readyValue11, b->readyValue11));
                if (rs.readyValue11) ring.queue()->Wait(fence11in12.Get(), rs.readyValue11);
                const bool same = pairA == g.pairFrom && pairB == g.d1;
                barrier(l, a->res.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                barrier(l, b->res.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                const bool packed = spanFlow.pack(l, a->res.Get(), b->res.Get(), g.t, same, g.pairFrom + 1, g.d1 + 1);
                if (!regionSubmit()) return PUSHCORE_ERROR;
                const bool ran = packed && spanFlow.infer();
                l = regionBegin(); if (!l) return PUSHCORE_ERROR;
                barrier(l, rs.res.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                const bool copied = ran && spanFlow.resolve(l, a->res.Get(), b->res.Get(), rs.res.Get());
                barrier(l, rs.res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
                barrier(l, a->res.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
                barrier(l, b->res.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
                if (!regionSubmit()) return PUSHCORE_ERROR;
                if (!packed) { pairA = pairB = UINT64_MAX; ++repairWhy[4]; continue; }
                if (!ran) { pairA = pairB = UINT64_MAX; ++repairWhy[5]; continue; }
                if (!copied) { ++repairWhy[6]; continue; }
                pairA = g.pairFrom; pairB = g.d1;
            } else {
            // the VFG span stream of this span: the main one, or the lazy Low one (each holds its own pair and parity)
            auto& sv = low ? spanLow : spanVfg;
            uint64_t& hA = low ? lowPairA : pairA; uint64_t& hB = low ? lowPairB : pairB; int& par = low ? lowParity : pairParity;
            // the pair into the span stream (parity: A then B), unless it holds it already
            if (hA != g.pairFrom || hB != g.d1) {
                auto* l = regionBegin(); if (!l) return PUSHCORE_ERROR;
                ring.queue()->Wait(fence11in12.Get(), std::max(a->readyValue11, b->readyValue11));
                const unsigned pa = unsigned(par), pb = unsigned(par ^ 1);
                barrier(l, a->res.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
                barrier(l, b->res.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
                const bool ok = sv.capture(l, a->res.Get(), pa, false) && sv.capture(l, b->res.Get(), pb, true);
                barrier(l, a->res.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
                barrier(l, b->res.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
                if (!regionSubmit() || !ok) { hA = hB = UINT64_MAX; ++repairWhy[4]; continue; }
                hA = g.pairFrom; hB = g.d1; par ^= 1;   // B's parity is the generate parity
            }
            const unsigned gp = unsigned(par);   // after the flip: the parity B went into
            if (!sv.generateAt(gp, g.t, 1)) { hA = hB = UINT64_MAX; ++repairWhy[5]; continue; }
            auto* l = regionBegin(); if (!l) return PUSHCORE_ERROR;
            if (rs.readyValue11) ring.queue()->Wait(fence11in12.Get(), rs.readyValue11);   // the last repair read it
            barrier(l, rs.res.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            const bool copied = sv.copyOutput(l, rs.res.Get(), gp, g.t > 0.5f);
            lowParts += low && copied;
            barrier(l, rs.res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            if (!regionSubmit()) return PUSHCORE_ERROR;
            if (!copied) { ++repairWhy[6]; continue; }
            }
            // this part's span frame is in rs: laid in below, all parts in one pass (camera residual: cells -> pixels)
            partMasks.push_back(&g.mask); partTex[produced] = rs.tex.Get();
            partShifts[2 * produced] = g.shiftX * cellPx; partShifts[2 * produced + 1] = g.shiftY * cellPx;
            mw = g.w; mh = g.h; ++produced;
            repairSubstitutions += g.pairFrom != g.d0; repairShifted += g.shiftX != 0.f || g.shiftY != 0.f; repairCarved += g.carved;
        }
        if (produced) {
            // D3D11: one pass lays every part into the slot (via the scratch: it reads the slot and writes the scratch), the
            // masks disjoint (AssignOwners), with detail matching (research P6)
            imm4->Wait(fence12in11.Get(), value12);
            if (!repairPass.setMasks(imm.Get(), partMasks, mw, mh, std::max(0.5f, 8.0f * float(stream.width) / 3840.0f / cellPx)) ||
                !repairPass.repair(imm.Get(), inSlots[p.slot].tex.Get(), partTex, produced, repairOut.Get(), w, h, partShifts, !repairDetailOff())) ++repairWhy[7];
            else {
                imm->CopySubresourceRegion(inSlots[p.slot].tex.Get(), 0, 0, 0, 0, repairOut.Get(), 0, nullptr);
                if (hrFail(imm4->Signal(fence11.Get(), ++value11), "D3D11 Signal(repair)")) return PUSHCORE_ERROR;
                for (uint32_t i = 0; i < produced; ++i) repSlots[i].readyValue11 = value11;
                // the RGB copy of this frame (if any) is stale now
                for (auto& r : rgbSlots) if (r.seq == p.seq) r.seq = UINT64_MAX;
                any = true; repairedParts += produced;
            }
        }
        regionEndTiming();
        if (any) {
            imm4->Flush();
            p.copyValue = value11;   // the graph waits for the repaired slot
            ++repairedFrames; repairedWholly += wholly;
            pendingRepairMs = costRepair.p50() > 0 ? costRepair.p50() : 3.0;
            if (!repairLogged) { repairLogged = true; veyra::log::info("pushcore", std::format("source repair: first repaired frame {} ({} part(s), first pair {} -> {} at t {:.2f}, threshold {:.1f} codes)", p.seq, groups.size(), groups[0].pairFrom, groups[0].d1, groups[0].t, planThr.load())); }
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
            if (vplOn && f.kind == FrameKind::Real && f.lease) {
                // Intel VPL: the frames between the previous real frame and this one first (VplEgress job on the bridge)
                if (!b.vplDone) {
                    std::string why;
                    if (!b.vplStarted) {
                        if (vpl.start(f.lease->texture.Get(), b.readyValue, b.vplReset, why)) b.vplStarted = true;
                        else if (why.empty()) break;   // busy: the caller holds the generated textures; return frames first
                        else vplFailed(why);
                    }
                    if (vplOn && b.vplStarted) {
                        if (vpl.busy()) break;         // poll() waits for the job
                        if (vpl.finish(value12, b.vplGen, why)) {
                            b.vplReady = value12;
                            if (vpl.lastMs() > 0 && vplMs.size() < 64) vplMs.push_back(vpl.lastMs());
                            // the renderer's level: 1 = the 2X midpoint only, 2 = none (VPL's history is kept either way)
                            const uint32_t m = vpl.multiplier();
                            for (uint32_t k = 0; k < b.vplGen.size(); ++k) b.vplGen[k].slot |= int((k + 1) << 16);   // + the subframe
                            if (b.fgLevel >= 1 && !b.vplGen.empty()) {
                                std::vector<pushmpc::VplEgress::Gen> keep;
                                for (uint32_t k = 0; k < b.vplGen.size(); ++k) {
                                    const bool mid = b.fgLevel == 1 && k == (m % 2 == 0 ? m / 2 - 1 : 0);
                                    if (mid) keep.push_back(b.vplGen[k]); else { vpl.released(b.vplGen[k].slot & 0xFFFF, 0); ++st.skippedGenerated; }
                                }
                                b.vplGen.swap(keep);
                            }
                        } else vplFailed(why);
                    }
                    b.vplDone = true;
                }
                while (count < capacity && b.vplNext < b.vplGen.size()) {
                    const auto& g = b.vplGen[b.vplNext++];
                    const uint32_t sub = uint32_t(g.slot >> 16), m = std::max(2u, vpl.multiplier());
                    PushCoreFrame& o = frames[count];
                    o = {};
                    o.texture = g.texture;
                    o.readyFence = fence12in11.Get();
                    o.readyValue = b.vplReady;
                    o.kind = PUSHCORE_FRAME_GENERATED;
                    o.pts100ns = b.prevPts + (b.realPts - b.prevPts) * int64_t(sub) / int64_t(m);
                    o.sourceIndex = b.sourceIndex;
                    o.subframe = sub;
                    o.width = graph->workWidth(); o.height = graph->workHeight();
                    Lease l; l.kind = PUSHCORE_FRAME_GENERATED; l.vplSlot = g.slot & 0xFFFF;
                    o.lease = nextLease++;
                    leases.emplace(o.lease, std::move(l));
                    ++count; ++st.outputs; ++st.generated;
                }
                if (b.vplNext < b.vplGen.size() || count >= capacity) break;   // the caller's array is full: the real frame next time
            }
            if (f.kind == FrameKind::Generated && f.validity != GenerationValidity::Valid) {
                ++st.skippedGenerated; f.lease.reset(); ++b.next; continue;
            }
            if (helixFgGraph && f.kind == FrameKind::Generated && !helixFgTest.recorded && ++helixFgTest.count == kPortSelfTestFrames)
                testRecord(helixFgTest, true, std::format("HelixSR frame generation ran: {} valid generated frames", kPortSelfTestFrames));
            if (portGraph && f.kind == FrameKind::Generated && !portRecorded && ++portValidGenerated == kPortSelfTestFrames)
                recordPortSelfTest(true, std::format("DLSS-G ran on the community port: {} valid generated frames", kPortSelfTestFrames));
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
    // Intel VPL failing mid-stream: the real frames go on alone (never a black frame); the next reload tries the chain again
    void vplFailed(const std::string& why) {
        veyra::log::error("pushcore", "Intel VPL frame interpolation failed: " + why + "; real frames only from now on");
        vplOn = false; runFg = PUSHCORE_FG_NONE; multiplier = 1; fgFellBack = true; cutGuardOn = false;
        fgNote = "Intel VPL failed mid-stream (" + why.substr(0, 100) + "): none";
        vpl.dropJobs();
    }

    int poll(PushCoreFrame* frames, uint32_t capacity, uint32_t* countOut, uint32_t timeoutMs) {
        if (!graph) return fail("not open");
        uint32_t count = 0;
        // re-timing: no source frame for 250 ms (end of stream, pause): what waits for a next distinct frame goes on
        if (queuedFrames() && (!retimingOn() || msSince(lastIngest) > 250.0) && pumpHeld(true) != PUSHCORE_OK) return PUSHCORE_ERROR;
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        if (xefg) {
            // XeFG's outputs arrive from its own threads (the shim's captures): poll until one is there or the time is up
            for (;;) {
                if (pollXefg(frames, capacity, count) != PUSHCORE_OK) return PUSHCORE_ERROR;
                collectTimings();
                if (count || !timeoutMs || Clock::now() >= deadline) break;
                Sleep(1);
            }
            *countOut = count;
            return PUSHCORE_OK;
        }
        for (;;) {
            if (pollOnce(frames, capacity, count) != PUSHCORE_OK) return PUSHCORE_ERROR;
            collectTimings();
            if (count || !timeoutMs || batches.empty() || Clock::now() >= deadline) break;
            // Wait for the GPU (fence event), or yield while the async VFG verdict lands.
            auto& b = batches.front();
            const auto& f = b.out.batch.frames[b.next];
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (vplOn && vpl.busy()) {
                vpl.waitDone(uint32_t(std::max<long long>(1, left)));
            } else if (f.lease && !f.lease->ready() && f.lease->readyFenceObject) {
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
        if (const int xs = it->second.xefgSlot; xs >= 0) {
            // XeFG route: the slot may take another capture once the caller's draw (signalled above) is done
            if (xefg) xefg->release(uint32_t(xs), c ? fence11in12.Get() : nullptr, c ? value11 : 0);
            leases.erase(it);
            return PUSHCORE_OK;
        }
        auto& l = it->second;
        if (l.vplSlot >= 0) { vpl.released(l.vplSlot, c ? value11 : 0); leases.erase(it); return PUSHCORE_OK; }
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
        vpl.dropJobs();
        batches.clear();
        pendingCopyLeases.clear();
        if (xefg) { xefg->flush(); xlastPts = INT64_MIN; }
        clearHeldQueues();
        needReset = true;
    }

    int flush() {
        clearHeldQueues();   // source frames from before the seek
#if defined(PUSHMPC_HAVE_PMML) && PUSHMPC_HAVE_PMML && PUSHMPC_HAVE_EXTERNAL_SR
        if (animeSr) animeSr->reset();   // AnimeSR's carry starts again after a seek
#endif
        if (!leases.empty()) return PUSHCORE_BUSY;
        dropQueued();
        return PUSHCORE_OK;
    }

    void teardownGraph() {
        // a self-test graph that closes cleanly before its result is not a failure: the next open tests again
        if (portGuardSet && !portRecorded) { setPortRunning(portUnlock(), {}); portGuardSet = false; }
        routeGraphEnded();
        testEnd(helixFgTest); helixFgGraph = false;
        testEnd(hlxFsrTest); hlxGraph = false;
        if (ring.initialized()) ring.drainQueue();
        dropQueued();
        if (xefg) {   // XeFG presents on the graph's queue: stop it before the graph goes
            const auto c = xefg->counters();
            veyra::log::info("pushcore", std::format("XeFG: {} presents, {} captured ({} real, {} generated, {} dropped), {} status errors, holds {} (skipped {}), "
                "XeLL sleep {:.1f} ms and Present {:.1f} ms per frame", c.presents, c.captures, c.real, c.generated, c.dropped, c.statusErrors, xholds, xholdsSkipped,
                c.presents ? c.sleepMsTotal / c.presents : 0.0, c.presents ? c.presentMsTotal / c.presents : 0.0));
            xefg->shutdown(); xefg.reset();
            if (xguard) { cutBase += xguard->cuts(); xguard.reset(); }
            for (auto& r : xreal) r.Reset();
            xlastPts = INT64_MIN; xlastGraphSource = 0;
            if (ring.initialized()) ring.drainQueue();
        }
        stopPlanner();
        if (spanVfgOk) {
            collectRepairTimes();
            veyra::log::info("pushcore", std::format("source repair: {} frames ({} wholly held), {} parts ({} substitution pairs, {} camera-residual shifts, {} cells carved); "
                "not repaired: spans gated {}, spans over budget {}, frames with no part {}, no RGB copy {}, capture {}, generate {}, copy {}, composite {}; spans held back (one new span per frame over budget) {}; "
                "GPU per repaired frame p50 {:.2f} p95 {:.2f} ms; cell threshold {:.1f} codes (noise p99 {:.2f}); animation evidence {:.1f} %; cascade: {} future ends repaired first, {} without a plan in time; midpoint stream: {} calls, {} parts at an approximate phase (mean error {:.3f} of a span)",
                repairedFrames, repairedWholly, repairedParts, repairSubstitutions, repairShifted, repairCarved,
                repairWhy[0], repairWhy[1], repairWhy[2], repairWhy[3], repairWhy[4], repairWhy[5], repairWhy[6], repairWhy[7], repairWhy[8],
                costRepair.p50(), costRepair.pct(0.95), planThr.load(), planNoise.load(), 100.0 * evidence.Ratio(), cascadeEnds, cascadeMissed, midCalls, midApprox, midApprox ? midErrSum / double(midApprox) : 0.0));
        }
        if (holdOn) holdLogClose();
        if (spanLowInit.joinable()) spanLowInit.join();
        if (lowMade || lowPolicy.Dropped())
            veyra::log::info("pushcore", std::format("source repair: VFG Low span stream made {} time(s), released {} time(s) while playing; {} parts repaired on it, {} spans still dropped over the budget",
                lowMade, lowReleased, lowParts, lowPolicy.Dropped()));
        spanLow.shutdown(); spanLowState = 0; lowMade = lowReleased = lowParts = 0; lowPolicy = {};
        spanVfg.shutdown(); spanFlow.shutdown(); shutdownMid(); spanVfgOk = false; spanIsFlow = false; rgbSlots.clear(); for (auto& rs : repSlots) rs = {}; repairOut.Reset(); repairPass.forgetViews();
        opened.clear(); outSlots.clear();
        vpl.shutdown(); vplOn = false; ivsr.reset(); ivsrOn = false;
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
        if (haveSettings && (settings.fgBackend != builtFg || settings.srMethod != builtSr || settings.fgTier != builtTier || (settings.cutGuard != 0) != builtGuard || (settings.subtitleHold != 0) != builtSubtitles || (settings.heldFrames != 0) != builtHeld || settings.animationMode != builtAnimMode || settings.holdFlags != builtHoldFlags || settings.animeSrModel != builtAnimeSr)) {
            veyra::log::info("pushcore", "frame generation method, upscaler, cut guard, Subtitle Hold or held-frame re-timing changed: graph rebuild");
            return PUSHCORE_NEEDS_REBUILD;
        }
        // the stages outside the graph (Intel VSR, Intel VPL) are sized at build time: a change of what they do rebuilds
        if ((ivsrOn || vplOn) && haveSettings && ((settings.sr != 0) != builtIntelSr || settings.srTarget != builtIntelTarget || settings.multiplier != builtIntelMult)) {
            veyra::log::info("pushcore", "Intel VSR / VPL settings changed: graph rebuild");
            return PUSHCORE_NEEDS_REBUILD;
        }
        if (zludaSr && zludaFailed) { veyra::log::info("pushcore", "RTX VSR through ZLUDA stopped: graph rebuild (FSR)"); return PUSHCORE_NEEDS_REBUILD; }
        engine::EnhancementSettings s;
        std::optional<engine::ChainRuntimeOrder> order;
        settingsProbe = true;
        const bool loaded = loadSettings(s, order);
        settingsProbe = false;
        if (!loaded) return PUSHCORE_NEEDS_REBUILD;
        if (engine::requiresGraphRebuild(loadedSettings, s) || !(order == loadedOrder)) {
            veyra::log::info("pushcore", "settings change needs a graph rebuild");
            return PUSHCORE_NEEDS_REBUILD;
        }
        auto next = engine::PlayerOptions::from(s, order);
        if (cfgSr >= 0) next.sr = cfgSr != 0;
        if (cfgFg >= 0) next.fg = cfgFg != 0;
        if (cfgMultiplier >= 2) next.fgMultiplier = uint32_t(std::clamp(cfgMultiplier, 2, int(PUSHCORE_MAX_MULTIPLIER)));
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
    bool gpuOnlyInput = false;   // set during a PUSHCORE_FLAG_WARMUP_GPU_ONLY warm-up
    void warmUp() {
        const auto t = Clock::now();
        const bool gpuOnly = (flags & PUSHCORE_FLAG_WARMUP_GPU_ONLY) != 0;
        gpuOnlyInput = gpuOnly;
        struct Reset { bool& b; ~Reset() { b = false; } } resetGpuOnly{gpuOnlyInput};
        warming = true;   // the synthetic frames are all the same picture: straight to the graph
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
        if (FAILED(d11->CreateTexture2D(&d, &sd, &tex))) { warming = false; veyra::log::info("pushcore", "warm-up skipped: no texture"); return; }
        const int64_t step = stream.fpsNum > 0 && stream.fpsDen > 0 ? int64_t(10000000.0 * stream.fpsDen / stream.fpsNum) : 417083;
        uint32_t frames = 0, outputs = 0;
        bool ok = true;
        auto drainSome = [&](uint32_t timeoutMs) {
            PushCoreFrame f[16]; uint32_t n = 0;
            if (poll(f, 16, &n, timeoutMs) != PUSHCORE_OK) return false;
            for (uint32_t k = 0; k < n; ++k) { presented(f[k].lease, gpuOnly ? nullptr : imm.Get()); ++outputs; }
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
        if (!gpuOnly) imm4->Flush();
        if (ring.initialized()) ring.drainQueue();
        dropQueued();
        haveReal = false; needReset = true;
        st = {}; timings.clear(); submittedCount = 0; ingestSeq = 0; warming = false; clearHeldQueues(); heldRatio.Reset(); heldLogged = false; evidence.Reset(); costRepair.clear(); pendingRepairMs = 0;
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
        qvsr.reset();
        for (auto& s : inSlots) s = {};
        deband = pushmpc::Deband{}; debandEligible = false;
        clearHeldQueues(); held = pushmpc::HeldDetector{}; heldEligible = false;
        copyList.Reset();
        for (auto& a : copyAlloc) a.Reset();
        if (ring.initialized()) ring.shutdown();
        vpl.shutdown(); ivsr.reset();
        fence11inBridge.Reset(); fence12inBridge.Reset();
        bridge.reset();
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
// Fast start (PushMPC 1.0): pushcore_prewarm. The first open in a process creates the adapter's D3D12 device (~100 ms
// with its context) and initialises CUDA with the device's primary context for VFG (~80 ms). A background thread does
// both while the player is still connecting its filters and keeps them alive until the next open succeeded (the open
// then holds its own references, so the device and the context are found ready) or 30 s passed.
namespace prewarm {
std::mutex lock;
HANDLE release = nullptr;     // set by a successful open (or the timeout)
bool running = false;
std::string note;             // what the prewarm did, for the next open's log
// pushcore_prewarm2: the NGX core as well. NGX Init (~2 s on an RTX 5090: it loads and checks every feature the driver
// knows) needs only the D3D12 device, so it runs here, outside the graph lock, while the quick graph builds; the core is
// published into the process-wide cache under the key the full graph will build, and the full graph borrows it.
struct NgxJob { bool want = false; std::wstring runtimeDir; PushCoreSettings settings{}; };
int ngxState = 0;             // 1 = an NGX Init for gSharedCache runs now (graphs wait for it in waitNgx)
std::condition_variable ngxCv;
void waitNgx(veyra::ngx::NgxCoreCache* cache) {
    if (cache != gSharedCache) return;
    std::unique_lock<std::mutex> lk(lock);
    const auto t = Clock::now();
    const bool waited = ngxState == 1;
    ngxCv.wait_for(lk, std::chrono::seconds(15), [] { return ngxState != 1; });
    if (waited) veyra::log::info("prewarm", std::format("the graph waited {:.0f} ms for the NGX core of the prewarm", msSince(t)));
}
std::string ngxPrewarm(veyra::gfx::D3D12DeviceContext& ctx, const NgxJob& job) {
    using namespace veyra;
    const auto t = Clock::now();
    // the key EnhanceGraph builds (EnhanceGraph::initialize, the NGX core block; PushMPC keys frame generation only for DLSS-G)
    ngx::NgxCoreCache::Key key;
    key.device = ctx.device();
    key.runtimeDirectory = std::filesystem::absolute(job.runtimeDir).lexically_normal().wstring();
    engine::NrRuntime nr = engine::NrRuntime::Ampere;   // as buildFromPushMpc
    {
        const std::filesystem::path rt(job.runtimeDir);
        std::error_code ec;
        if (!std::filesystem::exists(rt / L"nr-ampere" / L"nvngx_dlssnr.dll", ec) && std::filesystem::exists(rt / L"nr-original" / L"nvngx_dlssnr.dll", ec)) nr = engine::NrRuntime::NvidiaOriginal;
    }
    key.nrRuntime = int(engine::currentNrRuntime(nr));
    key.frameGeneration = false; key.fgBackend = 0; key.fgMultiplier = 1;
    const bool enabled = !ngx::FgCompatibilitySession::requested(ctx.adapter().vendorId, ctx.adapter().deviceId) &&
                         !GetEnvironmentVariableW(L"VEYRA_TEST_DISABLE_NGX_CORE_REUSE", nullptr, 0);
    if (!enabled) return "NGX core: not prewarmed (no core reuse on this GPU)";
    std::ifstream ids(job.runtimeDir + L"\\..\\config\\ngx-local.json", std::ios::binary);
    const std::string idText((std::istreambuf_iterator<char>(ids)), std::istreambuf_iterator<char>());
    auto scan = [&idText](const char* k) {
        const std::string needle = std::string("\"") + k + "\"";
        size_t q = idText.find(needle);
        if (q == std::string::npos) return std::string();
        q = idText.find('"', idText.find(':', q + needle.size()));
        if (q == std::string::npos) return std::string();
        return idText.substr(q + 1, idText.find('"', q + 1) - q - 1);
    };
    const std::string projectId = scan("ngxProjectId"), engineVersion = scan("engineVersion");
    if (projectId.empty()) return "NGX core: not prewarmed (ngx-local.json missing)";
    {
        std::lock_guard<std::mutex> g(gGraphLock);
        if (gSharedDevice || gSharedCacheUser) return "NGX core: not prewarmed (the process already has a warm or busy core)";
        if (!gSharedCache) gSharedCache = new ngx::NgxCoreCache();
        std::lock_guard<std::mutex> lk(lock);
        ngxState = 1;
    }
    auto host = std::make_shared<ngx::NgxCoreHost>();
    Status st = Status::Ok;
    const bool ok = host->initialize(ctx.device(), job.runtimeDir, projectId.c_str(), engineVersion.c_str(), st);
    bool published = false;
    if (ok && gSharedCache->prepare(std::move(key), true)) {   // no graph uses the cache now: it waits in waitNgx
        gSharedCache->publish(host);
        gSharedDevice = ctx.device();
        published = true;
    }
    host.reset();
    {
        std::lock_guard<std::mutex> lk(lock);
        ngxState = 0;
    }
    ngxCv.notify_all();
    return std::format("NGX core {} in {:.0f} ms", published ? "initialised and kept for the graph" : ok ? "initialised but not kept" : "did not initialise", msSince(t));
}
using CuInit = int(__stdcall*)(unsigned);
using CuCount = int(__stdcall*)(int*);
using CuGet = int(__stdcall*)(int*, int);
using CuLuid = int(__stdcall*)(char*, unsigned*, int);
using CuRetain = int(__stdcall*)(void**, int);
using CuRelease = int(__stdcall*)(int);
void run(uint64_t luid, NgxJob job) {
    const auto t0 = Clock::now();
    veyra::gfx::D3D12DeviceContext ctx;
    veyra::Status status = veyra::Status::Ok;
    veyra::gfx::DeviceContextDesc dd;
    dd.commandSlotCount = 1;
    dd.requiredLuid = luid;
    std::thread deband([] { pushmpc::Deband::precompile(); });   // the deband pass's shaders, in parallel
    const bool d3d = ctx.initialize(dd, status);
    const double d3dMs = msSince(t0);
    int cuDevice = -1; CuRelease cuRelease = nullptr; double cudaMs = -1;
    if (d3d && ctx.adapter().vendorId == 0x10DE) {
        const auto t1 = Clock::now();
        if (HMODULE cu = LoadLibraryExW((system32() / L"nvcuda.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
            auto init = reinterpret_cast<CuInit>(GetProcAddress(cu, "cuInit"));
            auto count = reinterpret_cast<CuCount>(GetProcAddress(cu, "cuDeviceGetCount"));
            auto get = reinterpret_cast<CuGet>(GetProcAddress(cu, "cuDeviceGet"));
            auto getLuid = reinterpret_cast<CuLuid>(GetProcAddress(cu, "cuDeviceGetLuid"));
            auto retain = reinterpret_cast<CuRetain>(GetProcAddress(cu, "cuDevicePrimaryCtxRetain"));
            cuRelease = reinterpret_cast<CuRelease>(GetProcAddress(cu, "cuDevicePrimaryCtxRelease_v2"));
            int n = 0;
            if (init && count && get && getLuid && retain && cuRelease && init(0) == 0 && count(&n) == 0) {
                for (int i = 0; i < n && cuDevice < 0; ++i) {
                    int dev = -1; uint64_t l = 0; unsigned node = 0;
                    if (get(&dev, i) != 0 || getLuid(reinterpret_cast<char*>(&l), &node, dev) != 0) continue;
                    void* c = nullptr;   // the LUID bytes as DXGI has them (LowPart, then HighPart)
                    if (l == ctx.adapter().luid && retain(&c, dev) == 0) cuDevice = dev;
                }
            }
            cudaMs = msSince(t1);
        }
    }
    {   // logged by the next open (the log file is opened there)
        std::lock_guard<std::mutex> lk(lock);
        note = std::format("fast start: D3D12 device {} in {:.0f} ms, CUDA primary context {} ({:.0f} ms), adapter {}",
            d3d ? "ready" : "failed", d3dMs, cuDevice >= 0 ? "retained" : "not used", cudaMs, d3d ? narrow(ctx.adapter().description) : std::string("-"));
    }
    deband.join();
    if (d3d && job.want && ctx.adapter().vendorId == 0x10DE) {
        const std::string n = ngxPrewarm(ctx, job);
        std::lock_guard<std::mutex> lk(lock);
        note += "; " + n;
    }
    WaitForSingleObject(release, 30000);
    if (cuDevice >= 0 && cuRelease) cuRelease(cuDevice);
    if (ctx.initialized()) ctx.shutdown();
    std::lock_guard<std::mutex> lk(lock);
    running = false;
}
void done() {   // a successful open: it holds its own references now
    std::lock_guard<std::mutex> lk(lock);
    if (running && release) SetEvent(release);
}
}  // namespace prewarm

// ---------------------------------------------------------------------------------------------
// C ABI
extern "C" {

// the player's filter is going away: nothing of the prewarm may stay (thread, device, CUDA context)
int pushcore_prewarmRelease(void) { try { prewarm::done(); } catch (...) {} return PUSHCORE_OK; }
static int startPrewarm(uint64_t adapterLuid, prewarm::NgxJob job) {
    try {
        veyra::pipeline::pushMpcBeforeNgxCore = &prewarm::waitNgx;
        std::lock_guard<std::mutex> lk(prewarm::lock);
        if (prewarm::running) return PUSHCORE_OK;
        if (!prewarm::release) prewarm::release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!prewarm::release) return PUSHCORE_ERROR;
        ResetEvent(prewarm::release);
        prewarm::running = true;
        std::thread([adapterLuid, job] {
            try { prewarm::run(adapterLuid, job); }
            catch (...) {
                std::lock_guard<std::mutex> l(prewarm::lock);
                prewarm::running = false;
                if (prewarm::ngxState == 1) { prewarm::ngxState = 0; prewarm::ngxCv.notify_all(); }
            }
        }).detach();
        return PUSHCORE_OK;
    } catch (...) { return PUSHCORE_ERROR; }
}
int pushcore_prewarm(uint64_t adapterLuid) { return startPrewarm(adapterLuid, {}); }
int pushcore_prewarm2(const PushCoreConfig* config, const PushCoreSettings* settings, uint64_t adapterLuid) {
    prewarm::NgxJob job;
    try {
        if (config && config->structSize >= sizeof(PushCoreConfig) && settings && copySettings(settings, job.settings)) {
            const PushCoreSettings& ps = job.settings;
            // the NGX core is wanted when the full graph has an NGX stage and DLSS-G (keyed by its multiplier) is not the method
            job.want = ((ps.sr && ps.srMethod == PUSHCORE_SR_METHOD_RTX) || ps.nr || ps.videoHdr) && !(ps.multiplier > 1 && ps.fgBackend == PUSHCORE_FG_DLSSG);
            if (job.want) {
                // the runtime folder as pushcore_open resolves it (RuntimePaths override first)
                const std::filesystem::path root = config->pushfpsDir && *config->pushfpsDir ? std::filesystem::path(config->pushfpsDir) : moduleDirectory();
                veyra::runtime::setApplicationRootOverride(root);
                job.runtimeDir = config->runtimeDir && *config->runtimeDir ? std::wstring(config->runtimeDir) : veyra::runtime::localRuntimeDirectory().wstring();
            }
        }
    } catch (...) { job.want = false; }
    return startPrewarm(adapterLuid, job);
}

static std::atomic<int> gOpenInstances{0};   // host process GPU priority goes back to Normal with the last one

uint32_t pushcore_abiVersion(void) { return PUSHCORE_ABI_VERSION; }
uint32_t pushcore_features(void) { return PUSHCORE_FEATURE_DV_P5 | PUSHCORE_FEATURE_SR_FAMILY | PUSHCORE_FEATURE_DEBAND | PUSHCORE_FEATURE_CUT_HOLD | PUSHCORE_FEATURE_FG_METHODS | PUSHCORE_FEATURE_CUT_COUNT | PUSHCORE_FEATURE_HDR_PEAK | PUSHCORE_FEATURE_DV_P5_EXACT | PUSHCORE_FEATURE_SUBTITLE_HOLD | PUSHCORE_FEATURE_SUBTITLE_HOLD_AUTO | PUSHCORE_FEATURE_NVOF_FG | PUSHCORE_FEATURE_QUICK_VSR | PUSHCORE_FEATURE_FAST_START | PUSHCORE_FEATURE_VFG16 | PUSHCORE_FEATURE_PUSHFLOW16 | PUSHCORE_FEATURE_ZLUDA_VSR | PUSHCORE_FEATURE_AMDGPU_EP | PUSHCORE_FEATURE_TURING_DLSSG | PUSHCORE_FEATURE_HELIXSR | PUSHCORE_FEATURE_HELD_FRAMES | PUSHCORE_FEATURE_ANIME_SR | PUSHCORE_FEATURE_ANIME_HOLD | pushmpc::selftest::features(); }

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
            // a second instance in the same process (the filter's quick-start graph, then the full one) keeps writing the
            // same file instead of truncating it
            static std::mutex logLock; static std::wstring logPath;
            std::lock_guard<std::mutex> lk(logLock);
            if (logPath != config->logFile && veyra::Logger::instance().openFile(config->logFile)) logPath = config->logFile;
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
        { std::lock_guard<std::mutex> lk(prewarm::lock); if (!prewarm::note.empty()) { veyra::log::info("prewarm", prewarm::note); prewarm::note.clear(); } }
        veyra::log::info("pushcore", std::format("open root={} shaders={} runtime={} data={}", narrow(root.wstring()),
                                                 narrow(veyra::runtime::shaderDirectory().wstring()), narrow(core->runtimeDir), narrow(core->dataDir)));
        int built = core->openDevices(device);
        if (built == PUSHCORE_OK) core->setupQuickVsr();
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
        prewarm::done();
        veyra::pipeline::pushMpcBeforeNgxCore = &prewarm::waitNgx;
        veyra::Logger::instance().flush();   // the open's timeline on disk now (a player's fast exit skips the buffered rest)
        ++gOpenInstances;
        core->description += std::format("; GPU priority class {}", veyra::gfx::gpuPriorityStatus().actual);
        if (core->qvsr.active()) core->description += std::format("; quick start: driver RTX VSR (D3D11) from {}x{}", core->qvsrSrcW, core->qvsrSrcH);
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
    if (core->xefg) pending += core->xefg->pending();   // G10: what XeFG still owes (presented by its own thread)
    pending += core->queuedFrames();   // source frames pushcore still holds for re-timing
    core->st.pendingFrames = pending;
    core->st.heldActive = core->retimingOn() ? 1u : 0u;
    core->st.heldTested = uint32_t(core->heldRatio.Size());
    core->st.heldRatioPermille = uint32_t(core->heldRatio.Ratio() * 1000.0 + 0.5);
    core->st.heldGraphMultiplier = core->graphMultiplier;
    core->st.repairActive = core->repairOn() ? 1u : 0u;
    core->st.repairedFrames = core->repairedFrames;
    core->st.repairedParts = core->repairedParts;
    core->st.holdActive = core->holdOn ? 1u : 0u;
    core->st.animClass = core->animClassNow.load(std::memory_order_relaxed);
    core->st.holdPairs = core->holdPairs; core->st.holdFrames = core->holdFrames; core->st.holdFlashes = core->holdFlashes;
    core->st.animeSrModel = ~0u; core->st.animeSrMs = -1.f;
#if defined(PUSHMPC_HAVE_PMML) && PUSHMPC_HAVE_PMML && PUSHMPC_HAVE_EXTERNAL_SR
    if (core->animeSr && !core->animeSrFailed && core->runSr == PUSHCORE_SR_METHOD_PMML) { core->st.animeSrModel = core->animeSr->model(); core->st.animeSrMs = core->animeSr->lastMs(); }
#endif
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
    core->st.cutGuardCuts = core->cutsSeen.load(std::memory_order_relaxed) + (core->xguard ? core->xguard->cuts() : 0);   // + the XeFG route's own guard
    core->st.subtitleHoldState = core->subtitleState.load(std::memory_order_relaxed);
    core->st.subtitleHoldPixels = core->subtitlePixels.load(std::memory_order_relaxed);
    core->st.quickVsr = core->qvsr.active() ? 1u : 0u;
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
    if (!io || io->structSize < offsetof(PushCoreFlowBench, rebuildMsP50)) return PUSHCORE_ERROR;
    // the appended rebuild timings (PUSHCORE_FEATURE_PUSHFLOW16) only for a caller whose struct has them
    const bool wantRebuild = io->structSize >= sizeof(PushCoreFlowBench);
    for (int t = 0; t < 3; ++t) io->firstMsP50[t] = io->firstMsP95[t] = io->nextMsP50[t] = io->nextMsP95[t] = io->initMs[t] = -1;
    if (wantRebuild) for (int t = 0; t < 3; ++t) io->rebuildMsP50[t] = io->rebuildMsP95[t] = -1;
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
            // flow once (PUSHCORE_FEATURE_PUSHFLOW16): a frame rebuilt from the last run's grids, no network
            if (wantRebuild && flow.status().grids) {
                std::vector<double> rebuild;
                for (int i = 0; i < pairs && good; ++i) {
                    good = begin();
                    if (!good) break;
                    list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
                    good = flow.resolveAt(list.Get(), a.Get(), b.Get(), output.Get(), float(i % 7 + 1) / 8.0f, 0.5f);
                    list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
                    list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, readback.Get(), 0);
                    good = submit() && wait() && good;
                    if (!good) break;
                    uint64_t* ts = nullptr;
                    readback->Map(0, nullptr, reinterpret_cast<void**>(&ts));
                    rebuild.push_back(double(ts[1] - ts[0]) * 1000.0 / double(frequency));
                    const D3D12_RANGE none{0, 0}; readback->Unmap(0, &none);
                }
                if (!rebuild.empty()) { io->rebuildMsP50[tier] = pct(rebuild, 0.5); io->rebuildMsP95[tier] = pct(rebuild, 0.95); }
                good = true;   // a failed rebuild run leaves the first/next results of this tier standing
            }
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
