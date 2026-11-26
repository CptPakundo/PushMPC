// pushmpc.Enhance: Auto for the VapourSynth route. The rules are the filter's own (filter\src\PushMpcAuto.h, reused
// header-only, read-only); this file only supplies what differs outside MPC-HC:
//  - the display: the monitor of mpv's window when this process has one (class "mpv"), else the primary monitor;
//    mpv's display_fps (passed by pushmpc.vpy) is the refresh rate when it is known;
//  - VRR: the NVIDIA scope logic needs the player's window; without one (SMPlayer embeds mpv, MPC-QT is libmpv, vspipe)
//    a fixed refresh is assumed, and said so;
//  - plugin arguments override the HKCU\Software\PushMPC values;
//  - the mapping onto PushCoreSettings (the filter's ToCore without the filter-only parts: PushMPC's own SDR-to-HDR
//    curve and deband-by-renderer bookkeeping live in the filter).
#pragma once
#include <windows.h>
#include <string>
#include "PushMpcAuto.h"
#include "pushmpc/pushcore_abi.h"
#include "pushmpc/unlocks.h"

namespace pmvs {

// ------------------------------------------------------------------------------------------------ overrides
// key = value from the plugin arguments (the same words as the registry values; see PushMpcSettings.h)
inline bool ApplyOverride(pm::Settings& s, const std::wstring& key, const std::wstring& v)
{
    auto is = [&](const wchar_t* w) { return _wcsicmp(v.c_str(), w) == 0; };
    if (key == L"enable") { s.enable = !(is(L"0") || is(L"off") || is(L"false")); return true; }
    if (key == L"smoother") { s.smoother = pm::ParseChoice(v, 2, pm::kMaxSmoother, s.smoother); if (is(L"1")) s.smoother = pm::kOff; return true; }
    if (key == L"sharper") { s.sharper = pm::ParseChoice(v, 1, 4, s.sharper); return true; }
    if (key == L"cleanup") { s.cleanUp = pm::ParseOnOff(v, s.cleanUp); return true; }
    if (key == L"sdr_to_hdr") { s.sdrToHdr = pm::ParseOnOff(v, s.sdrToHdr); return true; }
    if (key == L"deband") { s.deband = pm::ParseOnOff(v, s.deband); return true; }
    if (key == L"fg_quality") { s.fgQuality = is(L"High") ? pm::FgQuality::High : is(L"Low") ? pm::FgQuality::Low : is(L"Medium") ? pm::FgQuality::Medium : pm::FgQuality::Auto; return true; }
    if (key == L"fg_method") {
        s.fgMethod = is(L"VFG") ? pm::FgMethod::Vfg : (is(L"DLSSG") || is(L"DLSS-G")) ? pm::FgMethod::Dlssg : is(L"PushFlow") ? pm::FgMethod::PushFlow
                   : is(L"FSR") ? pm::FgMethod::Fsr : (is(L"AMDFRC") || is(L"FRC")) ? pm::FgMethod::AmdFrc : is(L"NVOF") ? pm::FgMethod::Nvof : pm::FgMethod::Auto;
        return true;
    }
    if (key == L"vrr") { s.vrr = is(L"On") ? pm::VrrMode::On : is(L"Off") ? pm::VrrMode::Off : pm::VrrMode::Auto; return true; }
    if (key == L"upscaler") { s.upscaler = is(L"RTX") ? pm::Upscaler::Rtx : is(L"FSR") ? pm::Upscaler::Fsr : is(L"NIS") ? pm::Upscaler::Nis : pm::Upscaler::Auto; return true; }
    if (key == L"target") { s.target = is(L"1440p") ? pm::UpscaleTarget::P1440 : is(L"4K") ? pm::UpscaleTarget::K4 : is(L"5K") ? pm::UpscaleTarget::K5 : pm::UpscaleTarget::Screen; return true; }
    if (key == L"vsr_style") { s.vsrStyle = is(L"HighBitrate") ? pm::VsrStyle::HighBitrate : is(L"Streaming") ? pm::VsrStyle::Streaming : is(L"Standard") ? pm::VsrStyle::Standard : pm::VsrStyle::Auto; return true; }
    if (key == L"cleanup_strength") { s.strength = is(L"Subtle") ? pm::CleanStrength::Subtle : is(L"Balanced") ? pm::CleanStrength::Balanced : pm::CleanStrength::Strong; return true; }
    if (key == L"on_battery") { s.onBattery = is(L"Same") ? pm::BatteryMode::Same : pm::BatteryMode::Light; return true; }
    if (key == L"subtitle_hold") { s.subtitleHold = pm::ParseOnOff(v, s.subtitleHold); return true; }   // PushFPS 4.5 Subtitle Hold (Auto = off)
    return false;
}

// ------------------------------------------------------------------------------------------------ the display
// mpv's own top-level window in this process (mpv's Win32 backend registers the class "mpv"); nullptr when embedded
inline HWND MpvWindow()
{
    struct Ctx { HWND found; DWORD pid; } ctx = { nullptr, GetCurrentProcessId() };
    EnumWindows([](HWND h, LPARAM l) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(l);
        DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
        wchar_t cls[64] = {};
        if (pid == c->pid && GetClassNameW(h, cls, 64) && wcscmp(cls, L"mpv") == 0) { c->found = h; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}
// mpv was started full screen (--fs, --fullscreen, --fs=yes on its command line). The filter starts before mpv's window
// goes full screen, so the window alone says "windowed" for a player that is about to fill the screen.
inline bool MpvStartsFullscreen(const std::wstring& cmdline)
{
    std::wstring c = L" " + cmdline + L" ";
    for (auto& ch : c) ch = wchar_t(towlower(ch));
    bool on = false;
    for (const wchar_t* k : { L" --fs", L" --fullscreen" }) {
        for (size_t p = c.find(k); p != std::wstring::npos; p = c.find(k, p + 1)) {
            const size_t e = p + wcslen(k);
            if (c[e] == L' ' || c[e] == L'"') on = true;
            else if (c.compare(e, 4, L"=yes") == 0) on = true;
            else if (c.compare(e, 3, L"=no") == 0) on = false;
        }
    }
    return on;
}
// displayFps: mpv's display_fps (<= 0 = unknown); haveWindow: the player's window is known (else VRR Auto = fixed);
// startsFullscreen: -1 = from this process's command line (mpv --fs)
inline pm::Display ResolveDisplay(const pm::Display& queried, bool haveWindow, pm::VrrMode vrr, double displayFps, int displayHdr, int startsFullscreen = -1)
{
    pm::Display d = queried;
    if (displayFps > 1) d.refreshHz = displayFps;
    if (displayHdr >= 0) d.hdr = displayHdr != 0;
    if (!haveWindow && vrr == pm::VrrMode::Auto) {
        d.vrr = false; d.vrrKnown = false;
        d.vrrHow = L"the player's window is not known to the plugin (embedded or headless): fixed refresh assumed";
    } else if (vrr == pm::VrrMode::Auto && !d.vrr && d.vrrHow.find(L"the player is windowed") != std::wstring::npos) {
        const bool fs = startsFullscreen >= 0 ? startsFullscreen != 0 : MpvStartsFullscreen(GetCommandLineW());
        const bool amd = d.vrrHow.find(L"FreeSync") != std::wstring::npos;
        if (fs && (d.vrrCapable || amd)) {
            // the profile's scope covers full screen and mpv was asked for it: VRR from the first frame
            d.vrr = true;
            d.vrrHow = amd ? L"FreeSync for mpv's window (AMD driver: on; mpv started full screen)"
                           : L"G-SYNC for mpv's window (fullscreen only; mpv started full screen)";
        } else {
            const size_t p = d.vrrHow.find(L"the player is windowed");
            d.vrrHow.replace(p, wcslen(L"the player is windowed"), L"mpv's window was not full screen when PushMPC started");
        }
    }
    return d;
}

// ------------------------------------------------------------------------------------------------ PushCoreSettings
inline uint32_t CoreFg(pm::FgMethod m) { return m == pm::FgMethod::Auto ? PUSHCORE_FG_VFG : uint32_t(int(m) - 1); }
// srcW/srcH: the visible source size; srcHdr: PQ/HLG source; displayHdr: what the output is presented as
inline PushCoreSettings ToCore(const pm::Settings& s, const pm::Choice& c, const pm::Display& d, UINT srcW, UINT srcH, bool srcHdr, bool displayHdr, std::wstring* note)
{
    PushCoreSettings p;
    memset(&p, 0, sizeof(p));
    p.structSize = sizeof(p);
    p.source = PUSHCORE_SETTINGS_PUSHMPC;
    p.multiplier = 1; p.srQuality = 2; p.srTarget = PUSHCORE_SR_TARGET_4K; p.vfgQuality = 1;
    p.srFamily = PUSHCORE_SR_FAMILY_STANDARD; p.deband = 0; p.debandStrength = uint32_t(s.debandStrength);
    p.fgBackend = PUSHCORE_FG_VFG; p.fgTier = 1; p.srMethod = PUSHCORE_SR_METHOD_RTX; p.cutGuard = 1; p.fgLite = 0;
    pm::StrengthValues(s.strength, p.nrTone, p.nrStructure);
    if (!s.enable) return p;
    p.sr = c.sr ? 1 : 0;
    p.srQuality = c.srLevel >= 1 ? uint32_t(c.srLevel) : 2u;
    p.srFamily = c.sr && c.srMethod == pm::Upscaler::Rtx ? uint32_t(std::clamp(c.srFamily, 0, 2)) : PUSHCORE_SR_FAMILY_STANDARD;
    p.srMethod = c.srMethod == pm::Upscaler::Fsr ? PUSHCORE_SR_METHOD_FSR : c.srMethod == pm::Upscaler::Nis ? PUSHCORE_SR_METHOD_NIS : PUSHCORE_SR_METHOD_RTX;
    p.deband = c.deband ? 1 : 0;
    p.debandStrength = uint32_t(std::clamp(c.debandStrength, 0, 2));
    pm::UpscaleTarget t = pm::ResolvedTarget(s.target, d);
    if (c.artifactReduction) t = srcH > 2160 || srcW > 3840 ? pm::UpscaleTarget::K5 : pm::UpscaleTarget::K4;
    p.srTarget = t == pm::UpscaleTarget::P1440 ? PUSHCORE_SR_TARGET_1440P : t == pm::UpscaleTarget::K5 ? PUSHCORE_SR_TARGET_5K : PUSHCORE_SR_TARGET_4K;
    p.multiplier = uint32_t(std::clamp(c.multiplier, 1, pm::VfgCeiling()));
    p.fgBackend = CoreFg(c.fgMethod);
    p.vfgQuality = c.fgMethod == pm::FgMethod::Vfg ? uint32_t(std::clamp(c.fgQuality, 0, 2)) : 1u;
    p.fgTier = c.fgMethod == pm::FgMethod::PushFlow ? uint32_t(std::clamp(c.fgQuality, 0, 2)) : 1u;
    p.nr = c.nr ? 1 : 0;
    // SDR to HDR: RTX Video HDR runs in pushcore; PushMPC's own curve (hdrItm) runs in the DirectShow filter only
    p.videoHdr = c.hdr && !c.hdrItm && displayHdr && !srcHdr ? 1 : 0;
    if (note && c.hdr && c.hdrItm) *note = L"SDR to HDR: PushMPC's own curve is part of the MPC-HC filter, not of the VapourSynth route: off here (RTX Video HDR works)";
    p.cutGuard = c.guardFlow ? 3 : 1;
    p.videoHdrPeak = uint32_t(c.rtxHdrPeak);
    // burned-in subtitles only (mpv draws soft ones after the filter); Auto = held only while burned-in text is found. The
    // stub core and older cores ignore 2 (the plugin and the core ship together)
    p.subtitleHold = p.multiplier > 1 ? pm::SubtitleHoldValue(s, true, pmu::ReadUnlockMode(pmu::Unlock::SubtitleHoldAuto) != pmu::UnlockMode::Off) : 0;
    return p;
}

// the learned costs the filter stores (HKCU\Software\PushMPC\Learned, model 2) for this GPU, read-only here
inline void LoadLearned(pm::Costs& c, const std::wstring& gpuName)
{
    HKEY k = nullptr;
    if (gpuName.empty() || RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\PushMPC\\Learned", 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    if (pm::RegReadStr(k, L"Gpu") == gpuName && pm::RegReadStr(k, L"Model") == L"2") {
        auto num = [&](const std::wstring& n, double& v, bool* flag, double lo, double hi) {
            const std::wstring s = pm::RegReadStr(k, n.c_str());
            const double x = s.empty() ? -1 : _wtof(s.c_str());
            if (x >= lo && x <= hi) { v = x; if (flag) *flag = true; return true; }
            return false;
        };
        for (int i = 0; i < pm::kFgKeys; ++i) num(std::format(L"FgScale{}", i), c.fgScale[i], &c.fgLearned[i], 0.05, 20);
        num(L"VsrScale", c.vsrScale, &c.learnedVsr, 0.1, 10);
        num(L"NrMs", c.nrMs, &c.learnedNr, 0.05, 200);
        num(L"Hdr4kMs", c.hdr4k, &c.learnedHdr, 0.05, 30);
        num(L"Deband1080Ms", c.deband1080, &c.learnedDeband, 0.001, 20);
        bool bench = true;
        for (int t = 0; t < 3; ++t) {
            bench = num(std::format(L"PfFirst{}", t), c.pfFirst[t], nullptr, 0.1, 500) && bench;
            bench = num(std::format(L"PfNext{}", t), c.pfNext[t], nullptr, 0.1, 500) && bench;
        }
        c.pfBenched = bench;
        pm::ApplyMeasuredCard(c);
    }
    RegCloseKey(k);
}

} // namespace pmvs
