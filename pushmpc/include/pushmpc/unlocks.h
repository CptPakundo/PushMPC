#pragma once
// PushMPC unlocks (T5b vendor paths): features that run on GPUs the vendor does not list, community ports, probes.
// Every unlock has
//   - an off switch: environment PUSHMPC_DISABLE_<NAME>=1, or HKCU\Software\PushMPC\Unlocks DWORD <Name> = 0;
//     the same DWORD = 1 forces it on (skips the self-test result; for testers); absent = Auto;
//   - a self-test gate: the result of the feature's self-test on this adapter and driver, cached in
//     HKCU\Software\PushMPC\SelfTest\<Name> (Key = "<vendor>:<device>:<driver>", Result = pass / fail, Reason, When);
//     a different adapter or driver makes the cache stale (the test runs again at first use);
//   - a log line that says what was chosen and why (DecideUnlock's why).
// Auto = on when the GPU is eligible and the self-test passed; the default without a test result is per unlock
// (defaultOnUntested; all are false today: no unlock runs before its self-test).
// Header-only, Win32 only; the decision itself (DecideUnlock) is pure and checked by filter\tools\auto_test.
#include <windows.h>
#include <cstdint>
#include <cwctype>
#include <string>

namespace pmu {

// the registry root of the unlock values (tests point it at a scratch key so the user's settings stay untouched)
inline std::wstring& RegBase() { static std::wstring b = L"Software\\PushMPC"; return b; }

enum class Unlock : int { Gtx16Vsr, ZludaVsr, XeFg, VplFi, VplSr, NvofWarp, Fsr4Int8, SubtitleHoldAuto, XessSr, Count };
inline const wchar_t* UnlockName(Unlock u)
{
    static const wchar_t* n[] = { L"Gtx16Vsr", L"ZludaVsr", L"XeFg", L"VplFi", L"VplSr", L"NvofWarp", L"Fsr4Int8", L"SubtitleHoldAuto", L"XessSr" };
    const int i = int(u);
    return i >= 0 && i < int(Unlock::Count) ? n[i] : L"?";
}
// what the unlock is, for the log and the settings page
inline const wchar_t* UnlockLabel(Unlock u)
{
    static const wchar_t* n[] = { L"RTX VSR on GTX 16 (Turing kernels, NGX gate bypass)", L"RTX VSR + RTX Video HDR on RX 7000 / 9000 (ZLUDA)",
        L"Intel XeSS frame generation (headless swap chain)", L"Intel VPL AI frame interpolation", L"Intel VPL AI super resolution",
        L"NVIDIA optical flow + PushMPC warp frame generation", L"FSR 4.1 INT8 upscaling (RX 6000 / 7000)", L"Subtitle Hold on while burned-in text is found", L"Intel XeSS super resolution" };
    const int i = int(u);
    return i >= 0 && i < int(Unlock::Count) ? n[i] : L"?";
}

enum class UnlockMode { Auto, Off, On };
inline const wchar_t* ModeName(UnlockMode m) { return m == UnlockMode::Off ? L"off" : m == UnlockMode::On ? L"forced on" : L"Auto"; }

// environment value "1" / "true" / "yes" / "on"
inline bool EnvTrue(const std::wstring& name)
{
    wchar_t v[16] = {};
    const DWORD n = GetEnvironmentVariableW(name.c_str(), v, 16);
    if (n == 0 || n >= 16) return false;
    std::wstring s = v; for (auto& c : s) c = wchar_t(towlower(c));
    return s == L"1" || s == L"true" || s == L"yes" || s == L"on";
}
inline std::wstring EnvName(Unlock u)
{
    std::wstring s = L"PUSHMPC_DISABLE_";
    for (const wchar_t* p = UnlockName(u); *p; ++p) s += wchar_t(towupper(*p));
    return s;   // e.g. PUSHMPC_DISABLE_GTX16VSR
}
// the mode from the environment and the registry (the environment's off wins)
inline UnlockMode ReadUnlockMode(Unlock u, std::wstring* source = nullptr)
{
    if (EnvTrue(EnvName(u))) { if (source) *source = EnvName(u) + L"=1"; return UnlockMode::Off; }
    DWORD v = 0, sz = sizeof(v), type = 0;
    HKEY k = nullptr;
    UnlockMode m = UnlockMode::Auto;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, (RegBase() + L"\\Unlocks").c_str(), 0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExW(k, UnlockName(u), nullptr, &type, reinterpret_cast<BYTE*>(&v), &sz) == ERROR_SUCCESS && type == REG_DWORD) {
            m = v == 0 ? UnlockMode::Off : UnlockMode::On;
            if (source) *source = std::wstring(L"HKCU\\Software\\PushMPC\\Unlocks\\") + UnlockName(u) + (v == 0 ? L" = 0" : L" = 1");
        }
        RegCloseKey(k);
    }
    return m;
}

// ------------------------------------------------------------------------------------------------ self-test cache
struct SelfTest {
    bool known = false;     // a result for THIS key exists
    bool passed = false;
    std::wstring key, reason, when;
};
inline std::wstring SelfTestKey(uint32_t vendor, uint32_t device, const std::wstring& driver)
{
    wchar_t b[64];
    swprintf_s(b, L"%04X:%04X:", vendor, device);
    return b + driver;
}
inline std::wstring RegStr(HKEY k, const wchar_t* name)
{
    wchar_t buf[512] = {}; DWORD sz = sizeof(buf) - sizeof(wchar_t), type = 0;
    if (RegQueryValueExW(k, name, nullptr, &type, reinterpret_cast<BYTE*>(buf), &sz) != ERROR_SUCCESS || type != REG_SZ) return {};
    return buf;
}
inline SelfTest ReadSelfTest(Unlock u, const std::wstring& key)
{
    SelfTest t; t.key = key;
    HKEY k = nullptr;
    const std::wstring path = RegBase() + L"\\SelfTest\\" + UnlockName(u);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return t;
    if (RegStr(k, L"Key") == key) {
        const std::wstring r = RegStr(k, L"Result");
        t.known = r == L"pass" || r == L"fail";
        t.passed = r == L"pass";
        t.reason = RegStr(k, L"Reason");
        t.when = RegStr(k, L"When");
    }
    RegCloseKey(k);
    return t;
}
inline bool WriteSelfTest(Unlock u, const std::wstring& key, bool passed, const std::wstring& reason)
{
    HKEY k = nullptr;
    const std::wstring path = RegBase() + L"\\SelfTest\\" + UnlockName(u);
    if (RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
    SYSTEMTIME st; GetSystemTime(&st);
    wchar_t when[32]; swprintf_s(when, L"%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    auto put = [&](const wchar_t* n, const std::wstring& v) { RegSetValueExW(k, n, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), DWORD((v.size() + 1) * sizeof(wchar_t))); };
    put(L"Key", key); put(L"Result", passed ? L"pass" : L"fail"); put(L"Reason", reason); put(L"When", when);
    RegCloseKey(k);
    return true;
}

// ------------------------------------------------------------------------------------------------ the decision (pure)
struct UnlockDecision {
    bool on = false;            // use it now
    bool runSelfTest = false;   // eligible, Auto, no result for this adapter + driver: run the self-test (then decide again)
    std::wstring why;
};
// eligible: this GPU / driver / file set could run it (the hardware rule of the unlock); ineligibleWhy: why not
inline UnlockDecision DecideUnlock(Unlock u, UnlockMode mode, bool eligible, const std::wstring& ineligibleWhy, const SelfTest& t,
                                   bool defaultOnUntested = false, const std::wstring& modeSource = L"")
{
    UnlockDecision d;
    const std::wstring name = UnlockName(u);
    const std::wstring src = modeSource.empty() ? std::wstring() : L" (" + modeSource + L")";
    if (mode == UnlockMode::Off) { d.why = name + L": off" + src; return d; }
    if (!eligible) { d.why = name + L": not for this GPU (" + ineligibleWhy + L")"; return d; }
    if (mode == UnlockMode::On) { d.on = true; d.why = name + L": forced on" + src + (t.known ? (t.passed ? L"; self-test passed" : L"; self-test FAILED: " + t.reason) : L"; no self-test result"); return d; }
    if (t.known) {
        d.on = t.passed;
        d.why = name + (t.passed ? L": on, self-test passed" : L": off, self-test failed: " + t.reason) + (t.when.empty() ? L"" : L" (" + t.when + L")");
        return d;
    }
    d.runSelfTest = true;
    d.on = defaultOnUntested;
    d.why = name + (defaultOnUntested ? L": on until the self-test has run on this GPU and driver" : L": off until the self-test has run on this GPU and driver");
    return d;
}

// ------------------------------------------------------------------------------------------------ hardware rules
// NVIDIA Turing without tensor cores: TU116 (GTX 1660 / 1660 Ti / 1660 SUPER / 1650 SUPER, some 1650) and TU117 (GTX 1650,
// 1630, MX450/550 class). Both are compute capability 7.5, the ISA RTX VSR's shipped sm_75 kernels are built for. PCI
// device IDs from the public PCI ID database (pci-ids.ucw.cz, vendor 10de).
inline bool IsTu116(uint32_t vendor, uint32_t device)
{
    if (vendor != 0x10DE) return false;
    return device >= 0x2180 && device <= 0x21DF;   // TU116 / TU116M
}
inline bool IsTu117(uint32_t vendor, uint32_t device)
{
    if (vendor != 0x10DE) return false;
    return device >= 0x1F80 && device <= 0x1FBF;   // TU117 / TU117M / TU117GL
}
// GTX 16 by name when the device ID table misses a variant (laptop / OEM IDs)
inline bool IsGtx16Name(const std::wstring& name)
{
    std::wstring a = name; for (auto& c : a) c = wchar_t(towupper(c));
    return a.find(L"GTX 16") != std::wstring::npos || a.find(L"GTX16") != std::wstring::npos || a.find(L"MX450") != std::wstring::npos || a.find(L"MX550") != std::wstring::npos;
}
inline bool IsGtx16(uint32_t vendor, uint32_t device, const std::wstring& name)
{
    return vendor == 0x10DE && (IsTu116(vendor, device) || IsTu117(vendor, device) || IsGtx16Name(name));
}
// NVIDIA's optical-flow accelerator (NVOFA): every Turing except TU117, and later; not Pascal (OF SDK documentation).
inline bool HasNvofa(uint32_t vendor, uint32_t device, bool rtx, const std::wstring& name)
{
    if (vendor != 0x10DE) return false;
    if (rtx) return true;
    if (IsTu117(vendor, device)) return false;
    if (IsTu116(vendor, device)) return true;
    std::wstring a = name; for (auto& c : a) c = wchar_t(towupper(c));
    return a.find(L"GTX 1660") != std::wstring::npos || a.find(L"1650 SUPER") != std::wstring::npos;
}
// AMD RDNA3 / RDNA4 (ZLUDA route for RTX VSR / RTX Video HDR); RDNA2 has no WMMA (slow) and is not eligible by default
inline bool ZludaEligibleFamily(uint32_t pushcoreFamily) { return pushcoreFamily == 6u /*RDNA4*/ || pushcoreFamily == 7u /*RDNA3*/; }

} // namespace pmu
