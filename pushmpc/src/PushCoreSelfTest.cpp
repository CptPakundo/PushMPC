// pushcore: the pushcore_selfTest export (PushCoreSelfTest.h). The tests themselves live with their methods.
#define PUSHCORE_BUILD 1
#include "PushCoreSelfTest.h"
#include "pushmpc/pushcore_abi.h"
#include "veyra/RuntimePaths.h"
#include "veyra/Log.h"

#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <mutex>

using Microsoft::WRL::ComPtr;

namespace pushmpc::selftest {
namespace {
std::vector<Test>& registry() { static std::vector<Test> r; return r; }   // filled by static initialisers before any export runs
std::string json(const std::string& v) {
    std::string e;
    for (const char ch : v) {
        if (ch == '"' || ch == '\\') { e += '\\'; e += ch; }
        else if (ch == '\n') e += "\\n";
        else if (ch == '\r' || ch == '\t') e += ' ';
        else if (static_cast<unsigned char>(ch) < 0x20) e += '?';
        else e += ch;
    }
    return e;
}
std::filesystem::path moduleDir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&registry), &self);
    wchar_t path[32768]{};
    const DWORD n = GetModuleFileNameW(self, path, DWORD(std::size(path)));
    if (!n || n >= std::size(path)) return {};
    return std::filesystem::path(path).parent_path();
}
// the adapter: WARP with the flag; else by LUID, or the first hardware adapter in DXGI's high-performance order
bool findAdapter(uint64_t luid, bool warp, DXGI_ADAPTER_DESC1& out) {
    ComPtr<IDXGIFactory6> f;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&f)))) return false;
    if (warp) {
        ComPtr<IDXGIAdapter1> a;
        return SUCCEEDED(f->EnumWarpAdapter(IID_PPV_ARGS(&a))) && SUCCEEDED(a->GetDesc1(&out));
    }
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> a;
        if (f->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a)) == DXGI_ERROR_NOT_FOUND || !a) return false;
        DXGI_ADAPTER_DESC1 d{};
        if (FAILED(a->GetDesc1(&d)) || (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        if (luid && luidOf(d.AdapterLuid) != luid) continue;
        out = d;
        return true;
    }
}
}  // namespace

bool add(const Test& test) {
    auto& r = registry();
    for (const auto& t : r) if (t.bit == test.bit) return false;
    r.push_back(test);
    return true;
}
uint32_t features() {
    uint32_t f = 0;
    for (const auto& t : registry()) f |= t.features;
    return f;
}
std::string narrow(const std::wstring& s) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string r(size_t(std::max(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), r.data(), n, nullptr, nullptr);
    return r;
}
std::wstring widen(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring r(size_t(std::max(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), r.data(), n);
    return r;
}
std::wstring driverVersion(const LUID& luid) {
    ComPtr<IDXGIFactory4> f;
    ComPtr<IDXGIAdapter> a;
    LARGE_INTEGER v{};
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&f))) || FAILED(f->EnumAdapterByLuid(luid, IID_PPV_ARGS(&a))) ||
        FAILED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &v))) return L"unknown";
    return std::format(L"{}.{}.{}.{}", HIWORD(v.HighPart), LOWORD(v.HighPart), HIWORD(v.LowPart), LOWORD(v.LowPart));
}
uint64_t luidOf(const LUID& l) { return (uint64_t(uint32_t(l.HighPart)) << 32) | l.LowPart; }

}  // namespace pushmpc::selftest

extern "C" PUSHCORE_API int pushcore_selfTest(uint64_t adapterLuid, const wchar_t* rootDir, uint32_t which, uint32_t flags, char* report, uint32_t reportSize) {
    using namespace pushmpc::selftest;
    static std::mutex serial;   // one self-test run at a time (they own devices, files and registry values)
    std::lock_guard<std::mutex> lock(serial);
    std::string out = "{";
    auto field = [&](const std::string& k, const std::string& v) { out += std::format("{}\"{}\": \"{}\"", out.size() > 1 ? ", " : "", json(k), json(v)); };
    bool all = which != 0;
    try {
        Context ctx;
        ctx.warp = (flags & PUSHCORE_SELFTEST_FLAG_WARP) != 0;
        ctx.record = (flags & PUSHCORE_SELFTEST_FLAG_RECORD) != 0 && !ctx.warp;
        ctx.root = rootDir && *rootDir ? std::filesystem::path(rootDir) : moduleDir();
        veyra::runtime::setApplicationRootOverride(ctx.root);
        ctx.haveAdapter = findAdapter(adapterLuid, ctx.warp, ctx.adapter);
        if (ctx.haveAdapter) {
            ctx.luid = luidOf(ctx.adapter.AdapterLuid);
            ctx.driver = driverVersion(ctx.adapter.AdapterLuid);
            field("adapter", std::format("{} (vendor {:04X} device {:04X})", narrow(ctx.adapter.Description), ctx.adapter.VendorId, ctx.adapter.DeviceId));
            field("driver", narrow(ctx.driver));
        } else {
            field("adapter", ctx.warp ? "none (no WARP adapter)" : "none");
            all = false;
        }
        out += std::format(", \"features\": {}", features());
        uint32_t known = 0;
        for (const auto& t : registry()) {
            known |= t.bit;
            if (!(which & t.bit)) continue;
            Result r;
            if (!ctx.haveAdapter) r.verdict = "fail: no adapter";
            else {
                try { r = t.run(ctx); }
                catch (const std::exception& e) { r = {}; r.verdict = std::string("fail: exception: ") + e.what(); }
                catch (...) { r = {}; r.verdict = "fail: exception"; }
            }
            field(t.key, r.verdict);
            for (const auto& [k, v] : r.fields) field(std::string(t.key) + "." + k, v);
            veyra::log::info("pushcore", std::format("self-test {}: {}", t.key, r.verdict));
            all = all && r.pass;
        }
        for (uint32_t b = 1; b; b <<= 1)
            if ((which & b) && !(known & b)) { field(std::format("test{}", b), "unavailable: not in this build"); all = false; }
    } catch (const std::exception& e) { field("exception", e.what()); all = false; }
    catch (...) { field("exception", "unknown"); all = false; }
    out += "}";
    if (report && reportSize) {
        const size_t n = std::min<size_t>(out.size(), reportSize - 1);
        std::memcpy(report, out.data(), n);
        report[n] = 0;
    }
    return all ? PUSHCORE_OK : PUSHCORE_ERROR;
}
