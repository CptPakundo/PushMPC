#pragma once
// PushMPC: DLSS frame generation on RTX 20 (Turing) through the community port sdli1995/dlssg_for_sm86 (MIT; the SM75
// kernel family comes from Coldwood1026/dlssg_for_sm75). pushcore uses the port's backend directly, inside its own NGX
// path: no proxy DLL in the player's folder, no game-facing NvAPI architecture spoof (that part lives only in the
// upstream proxy, which PushMPC never loads).
//
// What the port is (release 0.4.2, read from its files; Setup fetches it, PushMPC does not redistribute it):
//   dlssg_sm86\runtime\nvngx_dlssg.dll   NVIDIA's DLSS-G runtime 310.9.16.0, unmodified and NVIDIA-signed
//   dlssg_sm86\backend\sm86_backend.dll  the port's backend (MIT): Detours hooks on that runtime and on the NGX core
//                                        (_nvngx.dll: Create/Evaluate/GetCapabilityParameters/GetFeatureRequirements)
//                                        that route the runtime's Ada/Blackwell-only CUDA kernels to SM86 / SM75 images
//   dlssg_sm86\kernels\fg_kernels.dll    resource-only kernel pack: a JSON MANIFEST + 444 images (71 kernels, 34
//                                        optimised variants, 3 patches; cubin + PTX for sm_86 and sm_75 each)
// The backend finds the kernel pack relative to its own folder, so the upstream layout is kept as is.
//
// The backend ABI ("mod_api.h" ABI 1; the upstream source is not public, so these layouts were read from the 0.4.2
// binaries: the exports' own size/ABI checks and the upstream loader's calls):
//   int DlssgMod_GetInfo(ModBackendInfo*)      size 0x48, abi 1; 0 = ok, -1 = wrong size / ABI
//   int DlssgMod_Install(const ModInstallArgs*) size 0x50, abi 1, runtime module set, options size 0x2C / version 1;
//                                               0 = installed; -1 = refused (bad args, or already installed)
//   int DlssgMod_TuringHost(void)               1 = installed and the host is a physical Turing (SM 7.5)
// The order the upstream's own GPU test programs use, and pushcore's: map the runtime, install, then initialise NGX.
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace pmport {

// ---- the backend ABI (ABI 1, port 0.4.2) ----
struct ModBackendInfo {
    uint32_t size = 0x48;
    uint32_t abi = 1;
    uint32_t level = 3;                     // the loader writes 3; the backend answers 5
    uint32_t reserved0 = 0;
    const char* name = nullptr;             // "dlssg-310.9.16-d3d12-sm86"
    const char* runtimeSha256 = nullptr;    // the one runtime this backend serves (lower-case hex)
    const char* kernelPackOutcome = nullptr;   // "pending" before Install; then accepted / missing / rejected / not_needed
    uint32_t kernelPackFound = 0;
    uint32_t kernelPackStatus = 0;
    const char* kernelPackModel = nullptr;
    const char* kernelPackSchema = nullptr;
    const char* kernelPackManifest = nullptr;
};
static_assert(sizeof(ModBackendInfo) == 0x48, "DlssgMod_GetInfo checks size 0x48");

struct ModOptions {
    uint32_t size = 0x2C;
    uint32_t version = 1;
    uint32_t maxGeneratedFrames = 3;        // [FrameGeneration] MaxGeneratedFrames (1..5; 5 = 6x)
    uint32_t logLevel = 2;                  // [Logging] Level
    uint32_t logToFile = 1;                 // [Logging] File
    uint32_t reserved14 = 0;
    uint32_t evaluateEvery = 120;           // [Logging] EvaluateEvery (0 = the backend's 120)
    uint32_t markGeneratedFrames = 0;       // [Debug] MarkGeneratedFrames
    uint32_t markerX = 8, markerY = 8, markerScale = 2;
};
static_assert(sizeof(ModOptions) == 0x2C, "DlssgMod_Install checks options size 0x2C");

// routeFlags, as the upstream loader composes them
constexpr uint32_t kRouteForceSm86 = 1u << 0;     // [Compatibility] ForceSM86Route (developer key)
constexpr uint32_t kRouteSimulateAmpere = 1u << 1; // [Compatibility] SimulateAmpere (developer key)
constexpr uint32_t kRouteMultiFrame = 1u << 2;    // MaxGeneratedFrames > 1
constexpr uint32_t kRouteLoaderBase = 1u << 3;    // set by the upstream loader on every install
constexpr uint32_t kRouteKernelPtx = 0x10, kRouteKernelCubin = 0x20;   // [Compatibility] KernelImage (Auto = neither)
// [DLSSG] Preset, pinned on every DLSS-G feature the backend creates (bits 25-26: 1 = A, 2 = B UI recomposition, 3 = C
// FG Lite). Read from the 0.4.2 proxy (version.dll 0x180017935..0x18001796A: "b" -> 0x4000000, "c" -> 0x6000000, else
// 0x2000000, OR-ed into the flags it passes to DlssgMod_Install). Upstream's factory value is A. The pin is set once per
// install, and the backend installs once per process: FG Lite on / off is a per-process choice.
constexpr uint32_t kRoutePresetA = 0x2000000u, kRoutePresetB = 0x4000000u, kRoutePresetC = 0x6000000u;

struct ModInstallArgs {
    uint32_t size = 0x50;
    uint32_t abi = 1;
    HMODULE runtime = nullptr;              // the mapped nvngx_dlssg.dll the backend patches
    uint32_t routeFlags = kRouteLoaderBase;
    uint32_t reserved14 = 0;
    const wchar_t* logFile = nullptr;       // the backend's JSON-lines log (full path)
    ModOptions options;
    uint32_t reserved4c = 0;
};
static_assert(offsetof(ModInstallArgs, options) == 0x20 && sizeof(ModInstallArgs) == 0x50, "DlssgMod_Install checks size 0x50");

inline uint32_t RouteFlagsFor(uint32_t maxGeneratedFrames, bool lite = false)
{
    return kRouteLoaderBase | (maxGeneratedFrames > 1 ? kRouteMultiFrame : 0u) | (lite ? kRoutePresetC : kRoutePresetA);
}

// ---- the pinned port ----
struct PinnedFile { const wchar_t* rel; uint64_t size; const char* sha256; };   // sha256 lower-case hex
constexpr const char* kPortVersion = "0.4.2";
constexpr const char* kPortRepo = "sdli1995/dlssg_for_sm86";
constexpr const char* kPortCommit = "e2c2fafa24a2ff1731b1c8a3eb76bba4c7f00fb5";   // tag 0.4.2 (merge of #717)
constexpr const char* kRuntimeSha256 = "cadee1aa6db3907babdc3285da2aebc4553d92b89c38ba3eae8abd8bba1bea72";
constexpr PinnedFile kPortFiles[] = {
    { L"dlssg_sm86\\runtime\\nvngx_dlssg.dll", 7506544ull, kRuntimeSha256 },
    { L"dlssg_sm86\\backend\\sm86_backend.dll", 2328200ull, "93a1b4ac1ae0dd0853c45b59b16c3698035cc8e3a749e34c232eb579e602b8e2" },
    { L"dlssg_sm86\\kernels\\fg_kernels.dll", 11875464ull, "9882a01f30dd65f36ea1fc6223827e6f86679b99eb09e108eacc51556eea4e18" },
};
// where Setup puts it: <runtime>\community\dlssg-sm75\ (the folder holding dlssg_sm86\)
inline std::filesystem::path PortRoot(const std::filesystem::path& runtimeDir) { return runtimeDir / L"community" / L"dlssg-sm75"; }
inline std::filesystem::path PortRuntimeDir(const std::filesystem::path& runtimeDir) { return PortRoot(runtimeDir) / L"dlssg_sm86" / L"runtime"; }

// the files are there with the pinned sizes (cheap: for the capability report; the hashes are checked at install)
bool PortFilesPresent(const std::filesystem::path& runtimeDir);

struct PortState {
    bool attempted = false;
    bool installed = false;
    bool turingHost = false;
    bool touched = false;       // DlssgMod_Install was called: the backend's hooks may be in this process from here on
    bool lite = false;          // installed with preset C (FG Lite)
    std::string why;            // the result in words (log, capability reason, self-test reason)
};
// Once per process: verify the three files (size + SHA-256), map the NGX core from the driver (NGXCore FullPath) so
// the backend hooks it at once, map the runtime, handshake (GetInfo: ABI and the runtime SHA the backend serves),
// install (lite = DLSS-G preset C, FG Lite; else preset A), pin both modules for the life of the process. Later calls
// return the first result (the preset stays what the first install chose).
const PortState& EnsurePort(const std::filesystem::path& runtimeDir, const std::filesystem::path& logFile, uint32_t maxGeneratedFrames, bool lite = false);
const PortState& CurrentPort();

}  // namespace pmport
