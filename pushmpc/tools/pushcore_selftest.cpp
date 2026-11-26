// pushcore_selftest: runs pushcore's Intel video self-tests (pushcore_selfTest) and its capabilities query.
//
// usage: pushcore_selftest --core <pushcore.dll> [--root <runtime root>] [--warp] [--luid <hex>] [--ivsr] [--vpl] [--bridge] [--all]
//                          [--record] [--json <file>] [--no-caps]
//   --warp    run on Windows' software adapter (CI: Intel VSR and VPL must be refused cleanly, the bridge must pass)
//   --record  write the results into HKCU\Software\PushMPC\SelfTest\<IntelVsr|VplFi> (testers; pushcore then uses them)
// Prints the JSON report (plus the capabilities of the first hardware adapter as "caps..." fields) and exits 0 when every
// asked-for test passed, 1 when one failed or was unavailable, 2 on a setup error.
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include "pushmpc/pushcore_abi.h"

static std::string narrow(const wchar_t* s)
{
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string r(size_t(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, s, -1, r.data(), n, nullptr, nullptr);
    return r;
}
static std::string esc(const char* s)
{
    std::string e;
    for (; *s; ++s) { if (*s == '"' || *s == '\\') e += '\\'; e += *s; }
    return e;
}

int wmain(int argc, wchar_t** argv)
{
    const wchar_t* core = nullptr; const wchar_t* root = nullptr; const wchar_t* json = nullptr;
    uint32_t which = 0, flags = 0; uint64_t luid = 0; bool noCaps = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--core" && i + 1 < argc) core = argv[++i];
        else if (a == L"--root" && i + 1 < argc) root = argv[++i];
        else if (a == L"--json" && i + 1 < argc) json = argv[++i];
        else if (a == L"--luid" && i + 1 < argc) luid = std::wcstoull(argv[++i], nullptr, 16);
        else if (a == L"--warp") flags |= PUSHCORE_SELFTEST_FLAG_WARP;
        else if (a == L"--record") flags |= PUSHCORE_SELFTEST_FLAG_RECORD;
        else if (a == L"--ivsr") which |= PUSHCORE_SELFTEST_IVSR;
        else if (a == L"--vpl") which |= PUSHCORE_SELFTEST_VPL_FI;
        else if (a == L"--bridge") which |= PUSHCORE_SELFTEST_BRIDGE;
        else if (a == L"--no-caps") noCaps = true;
        else if (a == L"--all") which |= PUSHCORE_SELFTEST_IVSR | PUSHCORE_SELFTEST_VPL_FI | PUSHCORE_SELFTEST_BRIDGE;
        else { fwprintf(stderr, L"unknown argument %s\n", argv[i]); return 2; }
    }
    if (!core) { fwprintf(stderr, L"usage: pushcore_selftest --core <pushcore.dll> [--root DIR] [--warp] [--all|--ivsr|--vpl|--bridge] [--record] [--json FILE]\n"); return 2; }
    if (!which) which = PUSHCORE_SELFTEST_IVSR | PUSHCORE_SELFTEST_VPL_FI | PUSHCORE_SELFTEST_BRIDGE;
    std::wstring dir = core; const size_t slash = dir.find_last_of(L"\\/"); dir = slash == std::wstring::npos ? L"." : dir.substr(0, slash);
    SetDllDirectoryW(dir.c_str());
    HMODULE h = LoadLibraryExW(core, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) { fwprintf(stderr, L"cannot load %s (error %lu)\n", core, GetLastError()); return 2; }
    auto selfTest = reinterpret_cast<PushCoreSelfTestFn>(GetProcAddress(h, "pushcore_selfTest"));
    auto caps = reinterpret_cast<PushCoreCapabilitiesFn>(GetProcAddress(h, "pushcore_capabilities"));
    auto features = reinterpret_cast<uint32_t (*)(void)>(GetProcAddress(h, "pushcore_features"));
    if (!selfTest || !caps) { fwprintf(stderr, L"this pushcore has no pushcore_selfTest export\n"); return 2; }
    static char report[16384];
    const int r = selfTest(luid, root, which, flags, report, sizeof(report));
    // capabilities of the hardware adapter ("no device" on a runner: every reason says so)
    PushCoreCaps c{}; c.structSize = sizeof(c);
    const int cr = noCaps ? PUSHCORE_ERROR : caps(luid, root, &c);
    if (noCaps) strcpy_s(c.reason[0], "not queried (--no-caps)");
    std::string out = report;
    if (!out.empty() && out.back() == '}') out.pop_back();
    char buf[1024];
    snprintf(buf, sizeof(buf), ", \"features\": %u, \"capsNoDevice\": \"%s\", \"capsAdapter\": \"%s\", \"capsIvsr\": \"%s\", \"capsVplFi\": \"%s\", \"capsAvailable\": %u}",
             features ? features() : 0u, cr == PUSHCORE_OK ? "ok" : esc(c.reason[0]).c_str(), esc(c.adapter).c_str(),
             (c.available >> PUSHCORE_CAP_IVSR) & 1u ? ("available " + esc(c.reason[PUSHCORE_CAP_IVSR])).c_str() : esc(c.reason[PUSHCORE_CAP_IVSR]).c_str(),
             (c.available >> PUSHCORE_CAP_VPL_FI) & 1u ? ("available " + esc(c.reason[PUSHCORE_CAP_VPL_FI])).c_str() : esc(c.reason[PUSHCORE_CAP_VPL_FI]).c_str(),
             c.available);
    out += buf;
    printf("%s\n", out.c_str());
    if (json) {
        FILE* f = _wfopen(json, L"wb");
        if (f) { fwrite(out.data(), 1, out.size(), f); fclose(f); }
    }
    return r == PUSHCORE_OK ? 0 : 1;
}
