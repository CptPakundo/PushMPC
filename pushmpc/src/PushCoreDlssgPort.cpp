// PushMPC: the RTX 20 DLSS-G port (see PushCoreDlssgPort.h).
#include "PushCoreDlssgPort.h"
#include "veyra/Log.h"

#include <bcrypt.h>
#include <algorithm>
#include <format>
#include <mutex>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace pmport {
namespace {
std::once_flag gOnce;
PortState gState;

std::string narrow(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// SHA-256 of a file, lower-case hex; empty on any error
std::string sha256File(const std::filesystem::path& p)
{
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    std::string out;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 && BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<unsigned char> buf(1u << 20);
        DWORD got = 0;
        bool ok = true;
        while (ReadFile(f, buf.data(), DWORD(buf.size()), &got, nullptr) && got) {
            if (BCryptHashData(h, buf.data(), got, 0) != 0) { ok = false; break; }
        }
        unsigned char d[32]{};
        if (ok && BCryptFinishHash(h, d, sizeof(d), 0) == 0) {
            static const char* hex = "0123456789abcdef";
            for (unsigned char c : d) { out += hex[c >> 4]; out += hex[c & 15]; }
        }
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return out;
}

// The driver's NGX core, from the key NVIDIA's NGX loader reads (HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore,
// FullPath = the driver-store folder holding _nvngx.dll). Mapped before the install so the backend hooks it directly
// (otherwise the backend arms a LoadLibrary hook until the core appears).
bool mapNgxCore(std::string& why)
{
    if (GetModuleHandleW(L"_nvngx.dll")) { why = "NGX core already mapped"; return true; }
    wchar_t dir[MAX_PATH]{};
    DWORD sz = sizeof(dir), type = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"FullPath", RRF_RT_REG_SZ, &type, dir, &sz) != ERROR_SUCCESS) {
        why = "the NVIDIA driver's NGX core is not registered (NGXCore FullPath)";
        return false;
    }
    const std::filesystem::path core = std::filesystem::path(dir) / L"_nvngx.dll";
    if (!LoadLibraryExW(core.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        why = std::format("the NGX core could not be mapped ({}, error {})", narrow(core.wstring()), GetLastError());
        return false;
    }
    why = "NGX core mapped from " + narrow(core.wstring());
    return true;
}

void pin(HMODULE m)
{
    HMODULE out = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(m), &out);
}

void install(const std::filesystem::path& runtimeDir, const std::filesystem::path& logFile, uint32_t maxGeneratedFrames)
{
    PortState& s = gState;
    s.attempted = true;
    const std::filesystem::path root = PortRoot(runtimeDir);
    // 1. the pinned files
    for (const auto& f : kPortFiles) {
        const std::filesystem::path p = root / f.rel;
        std::error_code ec;
        const auto size = std::filesystem::file_size(p, ec);
        if (ec) { s.why = std::format("{} is not installed", narrow(f.rel)); return; }
        if (size != f.size) { s.why = std::format("{} has {} bytes, the pin says {}", narrow(f.rel), size, f.size); return; }
        if (sha256File(p) != f.sha256) { s.why = std::format("{} does not match its pinned SHA-256", narrow(f.rel)); return; }
    }
    // 2. the NGX core, then the runtime (NGX later finds this same mapped file first in its search path)
    std::string core;
    if (!mapNgxCore(core)) { s.why = core; return; }
    const std::filesystem::path runtimePath = root / kPortFiles[0].rel, backendPath = root / kPortFiles[1].rel;
    HMODULE runtime = LoadLibraryExW(runtimePath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!runtime) { s.why = std::format("the DLSS-G runtime could not be mapped (error {})", GetLastError()); return; }
    HMODULE backend = LoadLibraryExW(backendPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!backend) { s.why = std::format("the port's backend could not be mapped (error {}; antivirus or Smart App Control may block it)", GetLastError()); return; }
    using GetInfoFn = int(__cdecl*)(ModBackendInfo*);
    using InstallFn = int(__cdecl*)(const ModInstallArgs*);
    using TuringFn = int(__cdecl*)();
    auto getInfo = reinterpret_cast<GetInfoFn>(GetProcAddress(backend, "DlssgMod_GetInfo"));
    auto doInstall = reinterpret_cast<InstallFn>(GetProcAddress(backend, "DlssgMod_Install"));
    auto turing = reinterpret_cast<TuringFn>(GetProcAddress(backend, "DlssgMod_TuringHost"));
    if (!getInfo || !doInstall || !turing) { s.why = "the port's backend lacks DlssgMod_GetInfo / DlssgMod_Install / DlssgMod_TuringHost"; return; }
    // 3. handshake: ABI 1, and the backend serves exactly this runtime
    ModBackendInfo info;
    if (getInfo(&info) != 0 || info.size != 0x48 || info.abi != 1) { s.why = "the port's backend refused the ABI 1 handshake"; return; }
    if (!info.runtimeSha256 || std::string(info.runtimeSha256) != kRuntimeSha256) { s.why = "the port's backend serves a different DLSS-G runtime"; return; }
    // 4. install
    ModInstallArgs a;
    a.runtime = runtime;
    a.options.maxGeneratedFrames = std::clamp<uint32_t>(maxGeneratedFrames, 1, 5);
    a.routeFlags = RouteFlagsFor(a.options.maxGeneratedFrames);
    const std::wstring log = logFile.wstring();
    a.logFile = log.empty() ? nullptr : log.c_str();
    const int r = doInstall(&a);
    // the backend's hooks point into both modules from here on, whatever the result: never unload them
    pin(runtime); pin(backend);
    ModBackendInfo after;
    getInfo(&after);
    const std::string pack = after.kernelPackOutcome ? after.kernelPackOutcome : "?";
    if (r != 0) { s.why = std::format("the port's backend refused the install (result {}, kernel pack {})", r, pack); return; }
    s.installed = true;
    s.turingHost = turing() == 1;
    s.why = std::format("community DLSS-G port {} installed ({}, runtime 310.9.16, kernel pack {}, {} host; {})", kPortVersion,
                        info.name ? info.name : "?", pack, s.turingHost ? "Turing" : "non-Turing", core);
}
}  // namespace

bool PortFilesPresent(const std::filesystem::path& runtimeDir)
{
    const std::filesystem::path root = PortRoot(runtimeDir);
    for (const auto& f : kPortFiles) {
        std::error_code ec;
        if (std::filesystem::file_size(root / f.rel, ec) != f.size || ec) return false;
    }
    return true;
}

const PortState& EnsurePort(const std::filesystem::path& runtimeDir, const std::filesystem::path& logFile, uint32_t maxGeneratedFrames)
{
    std::call_once(gOnce, [&] {
        install(runtimeDir, logFile, maxGeneratedFrames);
        if (gState.installed) veyra::log::info("dlssg-port", gState.why);
        else veyra::log::warn("dlssg-port", "not used: " + gState.why);
    });
    return gState;
}
const PortState& CurrentPort() { return gState; }

}  // namespace pmport
