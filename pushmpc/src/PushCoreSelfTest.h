// pushcore: the vendor self-tests behind the pushcore_selfTest export (PushCoreSelfTest.cpp). Internal to pushcore.dll.
//
// Each method with a self-test (Intel XeSS FG and SR, Intel VSR, Intel VPL frame interpolation, the D3D11 <-> D3D12 bridge;
// later ZLUDA VSR and community ports) registers it from its own source file with a static Registrar, under its bit of the
// global PUSHCORE_SELFTEST_* space and the report key named in pushcore_abi.h. pushcore_selfTest resolves the adapter once,
// runs every asked-for test that is registered and writes one JSON report. The registered methods' PUSHCORE_FEATURE_* bits
// are part of pushcore_features(), so a feature bit appears exactly when its code is linked in.
#pragma once
#include <windows.h>
#include <dxgi1_6.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace pushmpc::selftest {

struct Context {
    bool haveAdapter = false;
    DXGI_ADAPTER_DESC1 adapter{};
    uint64_t luid = 0;            // the adapter's LUID (WARP's own with warp)
    std::wstring driver;          // user-mode driver version "a.b.c.d" as pushcore_capabilities keys the results, or "unknown"
    bool warp = false;            // PUSHCORE_SELFTEST_FLAG_WARP: Windows' software adapter
    bool record = false;          // PUSHCORE_SELFTEST_FLAG_RECORD: store hardware results (never WARP ones)
    std::filesystem::path root;   // the runtime root (rootDir, else pushcore's folder); already set as Veyra's root override
};

struct Result {
    bool pass = false;
    std::string verdict;                                        // "pass; ..." / "fail: ..." / "unavailable: ..."
    std::vector<std::pair<std::string, std::string>> fields;   // further report fields: "<key>.<name>" = value (strings)
};

using RunFn = Result (*)(const Context&);
struct Test {
    uint32_t bit;          // PUSHCORE_SELFTEST_*
    const char* key;       // the report key (pushcore_abi.h)
    uint32_t features;     // PUSHCORE_FEATURE_* this method brings (0 = none)
    RunFn run;
};
bool add(const Test& test);
struct Registrar { explicit Registrar(const Test& test) { add(test); } };

uint32_t features();       // the registered methods' PUSHCORE_FEATURE_* bits

// helpers for the tests
std::string narrow(const std::wstring& s);
std::wstring widen(const std::string& s);
std::wstring driverVersion(const LUID& luid);     // the key pushcore_capabilities uses (pmu::SelfTestKey)
uint64_t luidOf(const LUID& luid);

}  // namespace pushmpc::selftest
