#pragma once
// PushMPC: which DLSS-G backend runs on an RTX 30 card (and the stored per-backend record that decides it).
//
// RTX 30 has two community routes to NVIDIA's DLSS frame generation:
//   Transfusion  PushFPS's DlssgTransfusion (310.9.1 provider patched in-process, AmpereMfgUnlock), the default;
//   Port         sdli1995/dlssg_for_sm86 (310.9.16 + SM86 kernels, PushCoreDlssgPort.h; Setup fetches it), unlock
//                AmpereDlssg, which also has FG Lite (DLSS-G preset C).
// Both patch DLSS-G inside the process, so they never run in one process: the first one that touched the process
// (the port's install, or a Transfusion graph that started) is latched for the life of the process. Per open, one is
// chosen by ChooseDlssgRoute from what this card + driver has measured:
//   1. before any measurement: Transfusion (the existing path);
//   2. once Transfusion has a clean measured run: the port's self-test (240 valid generated frames) on the next open;
//   3. both measured: the more stable one, then the cheaper one (GPU ms per generated frame at 4K), the port only when
//      at least 3 % cheaper;
//   FG Lite asked for (Auto on a card where full DLSS-G does not fit): the port, when its self-test passed.
// A crash guard per backend ("Running" while its graph runs; found at the next open = a crash) and failed starts count
// against a backend's stability. The fallback when the chosen backend fails to start is the other one (when the process
// latch allows it), then the rest of the frame generation chain.
// Records: HKCU\Software\PushMPC\DlssgRoute\<Transfusion|Port>: Key (vendor:device:driver; another driver resets the
// record), Runs, Fails, Crashes (DWORD), MsPerGen (REG_SZ, ms per generated frame at 4K), Running (REG_SZ key).
// Header-only; ChooseDlssgRoute is pure and checked by filter\tools\auto_test.
#include "unlocks.h"
#include <cwchar>
#include <string>

namespace pmu {

enum class DlssgRoute : int { None = 0, Transfusion = 1, Port = 2 };
inline const wchar_t* DlssgRouteName(DlssgRoute r) { return r == DlssgRoute::Transfusion ? L"Transfusion" : r == DlssgRoute::Port ? L"Port" : L"none"; }

struct DlssgRouteRecord {
    uint32_t runs = 0;      // clean runs with a measurement (>= 120 full-batch frames)
    uint32_t fails = 0;     // graph or frame generation did not start on this backend
    uint32_t crashes = 0;   // the process ended while this backend's graph ran
    double msPerGen = 0;    // GPU ms per generated frame, normalised to a 4K output (EMA over runs)
};
inline bool Unstable(const DlssgRouteRecord& r) { return r.crashes > 0 || (r.fails >= 2 && r.fails * 4 > r.runs); }

struct DlssgRouteInputs {
    bool transfusionUsable = true;   // the Transfusion provider is installed and not switched off
    bool portUsable = false;         // the port's files are there, unlock AmpereDlssg not off, its self-test not failed
    bool portPassed = false;         // the port's self-test passed on this card + driver (false = pending)
    bool portForced = false;         // Unlocks\AmpereDlssg = 1 (testers)
    bool wantLite = false;           // FG Lite asked for (only the port has it)
    DlssgRoute latch = DlssgRoute::None;   // the backend this process already uses
    DlssgRouteRecord transfusion, port;
};
struct DlssgRouteChoice { DlssgRoute route = DlssgRoute::None; bool lite = false; std::wstring why; };

inline DlssgRouteChoice ChooseDlssgRoute(const DlssgRouteInputs& in)
{
    DlssgRouteChoice c;
    auto pick = [&](DlssgRoute r, const std::wstring& why) { c.route = r; c.lite = in.wantLite && r == DlssgRoute::Port; c.why = why; return c; };
    if (in.latch == DlssgRoute::Port) return pick(DlssgRoute::Port, L"this process already runs the port");
    if (in.latch == DlssgRoute::Transfusion) return pick(DlssgRoute::Transfusion, L"this process already runs Transfusion");
    if (!in.portUsable && !in.transfusionUsable) return pick(DlssgRoute::None, L"neither DLSS-G backend is usable");
    if (!in.portUsable) return pick(DlssgRoute::Transfusion, L"the port is not usable here");
    if (!in.transfusionUsable) return pick(DlssgRoute::Port, L"Transfusion is not usable here");
    if (in.portForced) return pick(DlssgRoute::Port, L"the port is forced on (Unlocks\\AmpereDlssg = 1)");
    const bool tBad = Unstable(in.transfusion), pBad = Unstable(in.port);
    if (in.wantLite && in.portPassed && !pBad) return pick(DlssgRoute::Port, L"FG Lite runs on the port only");
    if (in.transfusion.runs == 0 && !tBad) return pick(DlssgRoute::Transfusion, L"default (nothing measured yet)");
    if (!in.portPassed) return pick(DlssgRoute::Port, tBad ? L"Transfusion is unstable here: the port's self-test" : L"Transfusion measured: the port's self-test");
    if (tBad && !pBad) return pick(DlssgRoute::Port, L"Transfusion is unstable on this card and driver");
    if (pBad && !tBad) return pick(DlssgRoute::Transfusion, L"the port is unstable on this card and driver");
    if (in.port.runs == 0) return pick(DlssgRoute::Port, L"the port passed its self-test: measuring it");
    if (in.port.msPerGen > 0 && in.transfusion.msPerGen > 0 && in.port.msPerGen < in.transfusion.msPerGen * 0.97) {
        wchar_t b[128]; swprintf_s(b, L"the port measured cheaper (%.2f vs %.2f ms per generated frame)", in.port.msPerGen, in.transfusion.msPerGen);
        return pick(DlssgRoute::Port, b);
    }
    wchar_t b[128]; swprintf_s(b, L"Transfusion measured as cheap or cheaper (%.2f vs %.2f ms per generated frame)", in.transfusion.msPerGen, in.port.msPerGen);
    return pick(DlssgRoute::Transfusion, b);
}

// ---- the stored records ----
inline std::wstring DlssgRoutePath(DlssgRoute r) { return RegBase() + L"\\DlssgRoute\\" + DlssgRouteName(r); }
inline DWORD RegDword(HKEY k, const wchar_t* name)
{
    DWORD v = 0, sz = sizeof(v), type = 0;
    return RegQueryValueExW(k, name, nullptr, &type, reinterpret_cast<BYTE*>(&v), &sz) == ERROR_SUCCESS && type == REG_DWORD ? v : 0;
}
// the record for this adapter + driver key (another key: empty)
inline DlssgRouteRecord ReadDlssgRoute(DlssgRoute r, const std::wstring& key, std::wstring* running = nullptr)
{
    DlssgRouteRecord rec;
    if (running) running->clear();
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, DlssgRoutePath(r).c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return rec;
    if (running) *running = RegStr(k, L"Running");
    if (RegStr(k, L"Key") == key) {
        rec.runs = RegDword(k, L"Runs"); rec.fails = RegDword(k, L"Fails"); rec.crashes = RegDword(k, L"Crashes");
        rec.msPerGen = _wtof(RegStr(k, L"MsPerGen").c_str());
    }
    RegCloseKey(k);
    return rec;
}
inline void WriteDlssgRoute(DlssgRoute r, const std::wstring& key, const DlssgRouteRecord& rec)
{
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, DlssgRoutePath(r).c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    auto dw = [&](const wchar_t* n, DWORD v) { RegSetValueExW(k, n, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v)); };
    auto sz = [&](const wchar_t* n, const std::wstring& v) { RegSetValueExW(k, n, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), DWORD((v.size() + 1) * sizeof(wchar_t))); };
    wchar_t ms[32]; swprintf_s(ms, L"%.4f", rec.msPerGen);
    sz(L"Key", key); dw(L"Runs", rec.runs); dw(L"Fails", rec.fails); dw(L"Crashes", rec.crashes); sz(L"MsPerGen", ms);
    RegCloseKey(k);
}
// crash guard: key while the backend's graph runs, empty to clear
inline void SetDlssgRouteRunning(DlssgRoute r, const std::wstring& key)
{
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, DlssgRoutePath(r).c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    if (key.empty()) RegDeleteValueW(k, L"Running");
    else RegSetValueExW(k, L"Running", 0, REG_SZ, reinterpret_cast<const BYTE*>(key.c_str()), DWORD((key.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
}

}  // namespace pmu
