// pushcore_selftest: runs pushcore's vendor self-tests (pushcore_selfTest) and its capabilities query.
//
// usage: pushcore_selftest --core <pushcore.dll> [--root <runtime root>] [--warp] [--luid <hex>]
//                          [--xefg] [--xess] [--ivsr] [--vpl] [--bridge] [--all] [--record] [--json <file>] [--no-caps]
//                          [--xefg-multiplier N] [--size WxH] [--frames N]
//   --warp    run on Windows' software adapter (CI: what needs Intel hardware must be refused cleanly, the rest must pass)
//   --record  write the hardware results into HKCU\Software\PushMPC\SelfTest\<XessFg|XessSr|IntelVsr|VplFi> (testers;
//             pushcore_capabilities then uses them)
//   --xefg-multiplier / --size / --frames: the XeFG and XeSS SR test parameters (PUSHMPC_SELFTEST_* environment values
//             pushcore reads; the product runs the defaults)
// Prints the JSON report (plus the capabilities of the adapter as "caps..." fields) and exits 0 when every asked-for test
// passed, 1 when one failed or was unavailable, 2 on a setup error.
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include "pushmpc/pushcore_abi.h"

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
        const bool more = i + 1 < argc;
        if (a == L"--core" && more) core = argv[++i];
        else if (a == L"--root" && more) root = argv[++i];
        else if (a == L"--json" && more) json = argv[++i];
        else if (a == L"--luid" && more) luid = std::wcstoull(argv[++i], nullptr, 16);
        else if (a == L"--xefg-multiplier" && more) SetEnvironmentVariableW(L"PUSHMPC_SELFTEST_XEFG_MULTIPLIER", argv[++i]);
        else if (a == L"--size" && more) SetEnvironmentVariableW(L"PUSHMPC_SELFTEST_SIZE", argv[++i]);
        else if (a == L"--frames" && more) SetEnvironmentVariableW(L"PUSHMPC_SELFTEST_FRAMES", argv[++i]);
        else if (a == L"--warp") flags |= PUSHCORE_SELFTEST_FLAG_WARP;
        else if (a == L"--record") flags |= PUSHCORE_SELFTEST_FLAG_RECORD;
        else if (a == L"--xefg") which |= PUSHCORE_SELFTEST_XEFG;
        else if (a == L"--xess") which |= PUSHCORE_SELFTEST_XESS_SR;
        else if (a == L"--ivsr") which |= PUSHCORE_SELFTEST_IVSR;
        else if (a == L"--vpl") which |= PUSHCORE_SELFTEST_VPL_FI;
        else if (a == L"--bridge") which |= PUSHCORE_SELFTEST_BRIDGE;
        else if (a == L"--no-caps") noCaps = true;
        else if (a == L"--all") which |= PUSHCORE_SELFTEST_XEFG | PUSHCORE_SELFTEST_XESS_SR | PUSHCORE_SELFTEST_IVSR | PUSHCORE_SELFTEST_VPL_FI | PUSHCORE_SELFTEST_BRIDGE;
        else { fwprintf(stderr, L"unknown argument %s\n", argv[i]); return 2; }
    }
    if (!core) {
        fwprintf(stderr, L"usage: pushcore_selftest --core <pushcore.dll> [--root DIR] [--warp] [--luid HEX] [--all|--xefg|--xess|--ivsr|--vpl|--bridge] [--record] [--json FILE]\n");
        return 2;
    }
    if (!which) which = PUSHCORE_SELFTEST_XEFG | PUSHCORE_SELFTEST_XESS_SR | PUSHCORE_SELFTEST_IVSR | PUSHCORE_SELFTEST_VPL_FI | PUSHCORE_SELFTEST_BRIDGE;
    if (flags & PUSHCORE_SELFTEST_FLAG_WARP) SetEnvironmentVariableW(L"PUSHMPC_TEST_WARP", L"1");   // pushcore's D3D12 side may then take WARP
    std::wstring dir = core; const size_t slash = dir.find_last_of(L"\\/"); dir = slash == std::wstring::npos ? L"." : dir.substr(0, slash);
    SetDllDirectoryW(dir.c_str());
    HMODULE h = LoadLibraryExW(core, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) { fwprintf(stderr, L"cannot load %s (error %lu)\n", core, GetLastError()); return 2; }
    auto selfTest = reinterpret_cast<PushCoreSelfTestFn>(GetProcAddress(h, "pushcore_selfTest"));
    auto caps = reinterpret_cast<PushCoreCapabilitiesFn>(GetProcAddress(h, "pushcore_capabilities"));
    auto features = reinterpret_cast<uint32_t (*)(void)>(GetProcAddress(h, "pushcore_features"));
    if (!selfTest || !caps) { fwprintf(stderr, L"this pushcore has no pushcore_selfTest export\n"); return 2; }
    static char report[32768];
    const int r = selfTest(luid, root, which, flags, report, sizeof(report));
    // capabilities of the hardware adapter ("no device" on a runner: every reason says so)
    PushCoreCaps c{}; c.structSize = sizeof(c);
    const int cr = noCaps ? PUSHCORE_ERROR : caps(luid, root, &c);
    if (noCaps) strcpy_s(c.reason[0], "not queried (--no-caps)");
    std::string out = report;
    if (!out.empty() && out.back() == '}') out.pop_back();
    auto cap = [&](const char* name, uint32_t bit) {
        const std::string v = ((c.available >> bit) & 1u) ? "available " + esc(c.reason[bit]) : esc(c.reason[bit]);
        out += std::string(", \"") + name + "\": \"" + v + "\"";
    };
    out += ", \"exportFeatures\": " + std::to_string(features ? features() : 0u);
    out += std::string(", \"capsNoDevice\": \"") + (cr == PUSHCORE_OK ? std::string("ok") : esc(c.reason[0])) + "\"";
    out += std::string(", \"capsAdapter\": \"") + esc(c.adapter) + "\"";
    cap("capsXefg", PUSHCORE_CAP_XEFG);
    cap("capsVplFi", PUSHCORE_CAP_VPL_FI);
    cap("capsXessSr", PUSHCORE_CAP_XESS_SR);
    cap("capsIvsr", PUSHCORE_CAP_IVSR);
    out += ", \"capsAvailable\": " + std::to_string(c.available) + "}";
    printf("%s\n", out.c_str());
    if (json) {
        FILE* f = _wfopen(json, L"wb");
        if (f) { fwrite(out.data(), 1, out.size(), f); fclose(f); }
    }
    return r == PUSHCORE_OK ? 0 : 1;
}
