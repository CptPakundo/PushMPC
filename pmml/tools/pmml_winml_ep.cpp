// SPDX-License-Identifier: GPL-3.0-or-later
// pmml_winml_ep.exe: an ONNX Runtime plugin EP that exists only through the Windows ML ExecutionProviderCatalog (AMD's
// AMDGPUExecutionProvider; Windows 11 24H2+), made usable by pmml: the catalog is asked for the EP (Windows downloads and
// installs it on first use, as Windows ML apps do), and the library path it reports is written to
// <out>\ep.json {name, epName, library, version, source}, which pmml registers like any pinned plugin EP (src/ort.cpp).
//
//   pmml_winml_ep.exe --winml <dir with Microsoft.Windows.AI.MachineLearning.dll> --out <pmml>\eps\amdgpu
//                     [--id amdgpu] [--ep AMDGPUExecutionProvider] [--fallback MIGraphXExecutionProvider] [--no-ensure]
//
// The catalog's flat C API (WinMLEpCatalog.h, Windows ML 2.x) is resolved at run time from the Windows ML DLL that
// Setup fetched by pin (scripts/ml-runtime-pin.json "winmlCatalog"), so this tool builds without the Windows App SDK.
// Only opaque handles cross the boundary (no struct layouts are assumed).
// Exit codes: 0 = ep.json written; 3 = the catalog has no such EP for this PC (unsupported GPU / driver / Windows);
// 4 = the EP could not be made ready (download or install failed); 5 = the Windows ML DLL is missing or too old;
// 2 = usage. Prints one "RESULT ..." line for Setup's log.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using CatalogHandle = void*;
using EpHandle = void*;
using FnCatalogCreate = HRESULT(__stdcall*)(CatalogHandle*);
using FnCatalogRelease = HRESULT(__stdcall*)(CatalogHandle);
using FnFindProvider = HRESULT(__stdcall*)(CatalogHandle, const char* name, const char* reserved, EpHandle*);
using FnEnsureReady = HRESULT(__stdcall*)(EpHandle);
using FnPathSize = HRESULT(__stdcall*)(EpHandle, size_t*);
using FnPath = HRESULT(__stdcall*)(EpHandle, size_t, char*, size_t*);

struct Api {
    HMODULE dll = nullptr;
    FnCatalogCreate create = nullptr;
    FnCatalogRelease release = nullptr;
    FnFindProvider find = nullptr;
    FnEnsureReady ensure = nullptr;
    FnPathSize pathSize = nullptr;
    FnPath path = nullptr;
    bool complete() const { return create && release && find && ensure && pathSize && path; }
};

std::wstring arg(int argc, wchar_t** argv, const wchar_t* name, const wchar_t* def = L"")
{
    for (int i = 1; i + 1 < argc; ++i) if (!wcscmp(argv[i], name)) return argv[i + 1];
    return def;
}
bool flag(int argc, wchar_t** argv, const wchar_t* name)
{
    for (int i = 1; i < argc; ++i) if (!wcscmp(argv[i], name)) return true;
    return false;
}
std::string utf8(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::string jsonEscape(const std::string& s)
{
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (static_cast<unsigned char>(c) < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", unsigned(c)); o += b; }
        else o += c;
    }
    return o;
}
int result(int code, const char* state, const std::string& detail)
{
    std::printf("RESULT code=%d state=%s %s\n", code, state, detail.c_str());
    return code;
}

// the EP's library path from the catalog (after EnsureReady unless noEnsure); empty with why on failure
std::string libraryOf(const Api& api, CatalogHandle cat, const std::string& epName, bool noEnsure, int& code, std::string& why)
{
    EpHandle ep = nullptr;
    HRESULT hr = api.find(cat, epName.c_str(), nullptr, &ep);
    if (FAILED(hr) || !ep) { code = 3; char b[160]; std::snprintf(b, sizeof(b), "%s: not offered for this PC (hr 0x%08X)", epName.c_str(), unsigned(hr)); why = b; return {}; }
    if (!noEnsure) {
        std::printf("ensure  %s (Windows downloads and installs it when it is not present yet)\n", epName.c_str());
        std::fflush(stdout);
        hr = api.ensure(ep);
        if (FAILED(hr)) { code = 4; char b[160]; std::snprintf(b, sizeof(b), "%s: EnsureReady failed (hr 0x%08X)", epName.c_str(), unsigned(hr)); why = b; return {}; }
    }
    size_t n = 0;
    hr = api.pathSize(ep, &n);
    if (FAILED(hr) || n == 0 || n > 32768) { code = 4; char b[160]; std::snprintf(b, sizeof(b), "%s: no library path (hr 0x%08X, %zu)", epName.c_str(), unsigned(hr), n); why = b; return {}; }
    std::string p(n + 1, '\0');
    size_t used = 0;
    hr = api.path(ep, p.size(), p.data(), &used);
    if (FAILED(hr)) { code = 4; char b[160]; std::snprintf(b, sizeof(b), "%s: GetLibraryPath failed (hr 0x%08X)", epName.c_str(), unsigned(hr)); why = b; return {}; }
    p.resize(std::strlen(p.c_str()));
    if (p.empty() || GetFileAttributesA(p.c_str()) == INVALID_FILE_ATTRIBUTES) { code = 4; why = epName + ": the reported library is not on disk (" + p + ")"; return {}; }
    code = 0;
    return p;
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    const std::wstring winml = arg(argc, argv, L"--winml"), out = arg(argc, argv, L"--out");
    if (winml.empty() || out.empty()) {
        std::fprintf(stderr, "usage: pmml_winml_ep --winml <dir> --out <eps\\id dir> [--id amdgpu] [--ep AMDGPUExecutionProvider] [--fallback <EpName>] [--no-ensure]\n");
        return 2;
    }
    const std::string id = utf8(arg(argc, argv, L"--id", L"amdgpu"));
    const std::string epName = utf8(arg(argc, argv, L"--ep", L"AMDGPUExecutionProvider"));
    const std::string fallback = utf8(arg(argc, argv, L"--fallback"));
    const bool noEnsure = flag(argc, argv, L"--no-ensure");

    // the Windows ML DLL and its own dependencies from its folder only
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS | LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
    AddDllDirectory(winml.c_str());
    Api api;
    api.dll = LoadLibraryExW((winml + L"\\Microsoft.Windows.AI.MachineLearning.dll").c_str(), nullptr,
                             LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS);
    if (!api.dll) return result(5, "no-winml", "Microsoft.Windows.AI.MachineLearning.dll did not load (win32 " + std::to_string(GetLastError()) + ")");
    api.create = reinterpret_cast<FnCatalogCreate>(GetProcAddress(api.dll, "WinMLEpCatalogCreate"));
    api.release = reinterpret_cast<FnCatalogRelease>(GetProcAddress(api.dll, "WinMLEpCatalogRelease"));
    api.find = reinterpret_cast<FnFindProvider>(GetProcAddress(api.dll, "WinMLEpCatalogFindProvider"));
    api.ensure = reinterpret_cast<FnEnsureReady>(GetProcAddress(api.dll, "WinMLEpEnsureReady"));
    api.pathSize = reinterpret_cast<FnPathSize>(GetProcAddress(api.dll, "WinMLEpGetLibraryPathSize"));
    api.path = reinterpret_cast<FnPath>(GetProcAddress(api.dll, "WinMLEpGetLibraryPath"));
    if (!api.complete()) return result(5, "old-winml", "the Windows ML DLL has no EP catalog C API (WinMLEpCatalog*): Windows ML 2.x is needed");

    CatalogHandle cat = nullptr;
    const HRESULT hr = api.create(&cat);
    if (FAILED(hr) || !cat) { char b[96]; std::snprintf(b, sizeof(b), "WinMLEpCatalogCreate failed (hr 0x%08X)", unsigned(hr)); return result(5, "no-catalog", b); }

    int code = 0;
    std::string why, used = epName;
    std::string lib = libraryOf(api, cat, epName, noEnsure, code, why);
    if (lib.empty() && !fallback.empty()) {
        // AMD's older catalog EP (MIGraphX, RDNA3+), deprecated in Windows ML 2.x but still offered on some PCs
        std::string why2;
        int code2 = 0;
        lib = libraryOf(api, cat, fallback, noEnsure, code2, why2);
        if (!lib.empty()) { used = fallback; code = 0; }
        else why += "; " + why2;
    }
    api.release(cat);
    if (lib.empty()) return result(code ? code : 4, code == 3 ? "not-offered" : "not-ready", why);

    CreateDirectoryW(out.c_str(), nullptr);
    std::string fwd = lib;
    for (auto& c : fwd) if (c == '\\') c = '/';
    const std::string json = "{\n  \"name\": \"" + jsonEscape(id) + "\",\n  \"epName\": \"" + jsonEscape(used) + "\",\n  \"library\": \"" + jsonEscape(fwd) +
                             "\",\n  \"version\": \"catalog\",\n  \"source\": \"Windows ML ExecutionProviderCatalog\"\n}\n";
    const std::wstring file = out + L"\\ep.json";
    FILE* f = nullptr;
    if (_wfopen_s(&f, file.c_str(), L"wb") != 0) f = nullptr;
    if (!f) return result(4, "write-failed", "could not write " + utf8(file));
    std::fwrite(json.data(), 1, json.size(), f);
    std::fclose(f);
    return result(0, "ready", used + " " + fwd);
}
