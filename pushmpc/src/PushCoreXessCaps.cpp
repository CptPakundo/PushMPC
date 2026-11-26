// pushcore: Intel XeSS (G10) capabilities and self-tests. Always built (PushCoreXessStub.cpp stands in for the methods when
// the XeSS SDK headers were missing at build time).
//   - capabilities(): the hardware rule, the runtime files (Intel-signed, pinned), the unlock decisions XessFg / XessSr
//     (Auto: on once the self-test passed on this adapter and driver; a missing result is measured here, once per adapter
//     and driver, like Intel VSR and VPL), the measured XeSS SR cost for Auto;
//   - the pushcore_selfTest entries PUSHCORE_SELFTEST_XEFG ("xefg") and PUSHCORE_SELFTEST_XESS_SR ("xesssr").
// Hardware rule: an Intel GPU (Arc / Xe: XMX for SR); any other GPU too once the XeSS files are installed there (Setup
// installs them on Intel by default, elsewhere on request; XeSS runs on any Shader Model 6.4 GPU) or the unlock is forced
// on; the self-test decides. Off switches: PUSHMPC_DISABLE_XESSFG / PUSHMPC_DISABLE_XESSSR, Unlocks\XessFg / XessSr = 0.
#define PUSHCORE_BUILD 1
#include "PushCoreXess.h"
#include "PushCoreSelfTest.h"
#include "pushmpc/pushcore_abi.h"
#include "pushmpc/unlocks.h"
#include "veyra/Log.h"

#include <cstdio>
#include <cwchar>
#include <format>
#include <tuple>

namespace pushmpc::xess {
namespace {
using selftest::narrow;
using selftest::widen;

uint32_t envU(const wchar_t* name, uint32_t def, uint32_t lo, uint32_t hi) {
    wchar_t v[32]{};
    const DWORD n = GetEnvironmentVariableW(name, v, 32);
    if (!n || n >= 32) return def;
    const unsigned long x = std::wcstoul(v, nullptr, 10);
    return x >= lo && x <= hi ? uint32_t(x) : def;
}
// PUSHMPC_SELFTEST_SIZE = "WxH" (pushcore_selftest --size; the product runs the defaults)
void envSize(uint32_t& w, uint32_t& h) {
    wchar_t v[32]{};
    const DWORD n = GetEnvironmentVariableW(L"PUSHMPC_SELFTEST_SIZE", v, 32);
    unsigned a = 0, b = 0;
    if (n && n < 32 && swscanf_s(v, L"%ux%u", &a, &b) == 2 && a >= 64 && b >= 64 && a <= 7680 && b <= 4320) { w = a; h = b; }
}
constexpr uint32_t kSrW = 1280, kSrH = 720;   // the XeSS SR test's output size (input = half)

XefgSelfTestResult runXefg(uint64_t luid, bool warp) {
    uint32_t w = 640, h = 360;
    envSize(w, h);
    return xefgSelfTest(luid, warp, envU(L"PUSHMPC_SELFTEST_XEFG_MULTIPLIER", 2, 2, 8), w, h, envU(L"PUSHMPC_SELFTEST_FRAMES", 30, 8, 240));
}
XessSrSelfTestResult runSr(uint64_t luid, bool warp, uint32_t& w, uint32_t& h) {
    w = kSrW; h = kSrH;
    envSize(w, h);
    return xessSrSelfTest(luid, warp, w, h, envU(L"PUSHMPC_SELFTEST_FRAMES", 16, 4, 120));
}
// XeSS SR's cost for a 4K output (Auto's prior), from the test's ms per frame at its output size; stored in the reason
double ms4kOf(const XessSrSelfTestResult& r, uint32_t w, uint32_t h) {
    return r.msPerFrame > 0 && w && h ? r.msPerFrame * (3840.0 * 2160.0) / (double(w) * h) : -1;
}
std::string verdict(bool ran, bool passed, const std::string& note) {
    return (passed ? "pass; " : ran ? "fail: " : "unavailable: ") + note;
}
double numberAfter(const std::wstring& s, const wchar_t* key) {
    const auto p = s.find(key);
    return p == std::wstring::npos ? -1 : std::wcstod(s.c_str() + p + wcslen(key), nullptr);
}

// ---- the registered self-tests
selftest::Result testXefg(const selftest::Context& ctx) {
    const XefgSelfTestResult r = runXefg(ctx.luid, ctx.warp);
    selftest::Result o;
    o.pass = r.ran && r.passed;
    o.verdict = verdict(r.ran, r.passed, r.note);
    o.fields = {{"multiplier", std::to_string(r.multiplier)}, {"real", std::to_string(r.realOut)}, {"generated", std::to_string(r.generatedOut)},
                {"expected", std::to_string(r.expectedGenerated)}, {"positionErrorPx", std::format("{:.3f}", r.medianPositionErrorPx)},
                {"psnr", std::format("{:.2f}", r.medianPsnr)}, {"psnrBlend", std::format("{:.2f}", r.medianPsnrBlend)},
                {"msPerGenerated", std::format("{:.3f}", r.msPerGenerated)}};
    if (ctx.record && r.ran)
        pmu::WriteSelfTest(pmu::Unlock::XessFg, pmu::SelfTestKey(ctx.adapter.VendorId, ctx.adapter.DeviceId, ctx.driver), r.passed, widen(r.note));
    return o;
}
selftest::Result testXessSr(const selftest::Context& ctx) {
    uint32_t w = 0, h = 0;
    const XessSrSelfTestResult r = runSr(ctx.luid, ctx.warp, w, h);
    const double ms4k = ms4kOf(r, w, h);
    selftest::Result o;
    o.pass = r.ran && r.passed;
    o.verdict = verdict(r.ran, r.passed, r.note);
    o.fields = {{"psnr", std::format("{:.2f}", r.psnrXess)}, {"psnrBilinear", std::format("{:.2f}", r.psnrBilinear)},
                {"msPerFrame", std::format("{:.3f}", r.msPerFrame)}, {"ms4k", std::format("{:.2f}", ms4k)}, {"version", r.version},
                {"providerSlot", srProviderBuilt() ? "yes" : "no (PushFPS without the FsrSrBackend provider slot)"}};
    if (ctx.record && r.ran)
        pmu::WriteSelfTest(pmu::Unlock::XessSr, pmu::SelfTestKey(ctx.adapter.VendorId, ctx.adapter.DeviceId, ctx.driver), r.passed,
                           widen(std::format("{} ms4k={:.2f}", r.note, ms4k)));
    return o;
}
const selftest::Registrar kXefg({PUSHCORE_SELFTEST_XEFG, "xefg", PUSHCORE_FEATURE_XEFG, &testXefg});
const selftest::Registrar kXessSr({PUSHCORE_SELFTEST_XESS_SR, "xesssr", PUSHCORE_FEATURE_XESS_SR, &testXessSr});
}  // namespace

XessCaps capabilities(const DXGI_ADAPTER_DESC1& d, const std::wstring& driver, bool runTests) {
    XessCaps c;
    const bool intel = d.VendorId == 0x8086;
    const std::wstring key = pmu::SelfTestKey(d.VendorId, d.DeviceId, driver);
    const uint64_t luid = selftest::luidOf(d.AdapterLuid);
    const bool software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    // one method: the files, the unlock's decision (running its missing self-test once), the reason line
    auto decide = [&](pmu::Unlock u, bool files, const std::string& filesWhy, bool slot, const char* slotWhy, auto run, bool& hw, bool& on, std::string& why,
                      pmu::SelfTest& t) {
        std::wstring src;
        const pmu::UnlockMode mode = pmu::ReadUnlockMode(u, &src);
        // eligible: an Intel GPU, or any GPU the XeSS files were installed for, or forced on; the self-test decides
        const bool gpuOk = !software && (intel || files || mode == pmu::UnlockMode::On);
        const bool eligible = gpuOk && files && slot;
        const std::wstring inel = !gpuOk ? L"Intel XeSS runs on Intel Arc / Xe; other GPUs once its files are installed (Setup -Xess Yes)"
                                : !files ? widen(filesWhy) : widen(slotWhy);
        t = pmu::ReadSelfTest(u, key);
        pmu::UnlockDecision dec = pmu::DecideUnlock(u, mode, eligible, inel, t, false, src);
        if (dec.runSelfTest && runTests) {
            const auto [ran, passed, note] = run();
            if (ran) {
                pmu::WriteSelfTest(u, key, passed, widen(note));
                veyra::log::info("pushcore", std::format("{} self-test: {}", narrow(pmu::UnlockName(u)), note));
                t = pmu::ReadSelfTest(u, key);
                if (!t.known) { t.known = true; t.passed = passed; t.reason = widen(note); }
                dec = pmu::DecideUnlock(u, mode, eligible, inel, t, false, src);
            }
        }
        hw = gpuOk && mode != pmu::UnlockMode::Off;
        on = eligible && dec.on;
        why = narrow(dec.why);
        veyra::log::info("pushcore", "unlock " + why);
    };
    std::string fgWhy, srWhy;
    const bool fgFiles = fgFilesUsable(&fgWhy), srFiles = srFileUsable(&srWhy);
    pmu::SelfTest tfg, tsr;
    decide(pmu::Unlock::XessFg, fgFiles, fgWhy, true, "", [&] {
        const XefgSelfTestResult r = runXefg(luid, false);
        return std::tuple<bool, bool, std::string>{r.ran, r.passed, r.note};
    }, c.fgHw, c.fgOn, c.fgWhy, tfg);
    decide(pmu::Unlock::XessSr, srFiles, srWhy, srProviderBuilt(), "this pushcore has no slot for XeSS SR (PushFPS without the FsrSrBackend provider patch)", [&] {
        uint32_t w = 0, h = 0;
        const XessSrSelfTestResult r = runSr(luid, false, w, h);
        return std::tuple<bool, bool, std::string>{r.ran, r.passed, std::format("{} ms4k={:.2f}", r.note, ms4kOf(r, w, h))};
    }, c.srHw, c.srOn, c.srWhy, tsr);
    if (tsr.known) c.srMs4k = numberAfter(tsr.reason, L"ms4k=");
    return c;
}

}  // namespace pushmpc::xess
