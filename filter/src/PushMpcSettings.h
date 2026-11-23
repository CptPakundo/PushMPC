// PushMPC's own settings (HKCU\Software\PushMPC). No PushFPS install is needed: the values map onto the settings
// PushFPS's simple video page sets (pushcore builds the EnhancementSettings; see settings-and-runtime.md).
//
// Values (REG_SZ, absent = the default; case-insensitive):
//   Enable           DWORD 1/0                       default 1
//   Smoother         Auto | Off | 2..8               default Auto   (NVIDIA VFG frame generation multiplier)
//   Sharper          Auto | Off | 1..4               default Auto   (RTX Video Super Resolution quality)
//   CleanUp          Auto | Off | On                 default Auto   (PushFPS "AI picture enhancement", NR)
//   SdrToHdr         Auto | Off | On                 default Auto   (SDR video as HDR10 when Windows HDR is on)
//   HdrBoost         Auto | Off | Low | Medium | High   default Auto (highlight expansion of SDR to HDR; Off = SDR at 203 cd/m2)
//   HdrMethod        Auto | RTX                      default Auto   (Auto: PushMPC's curve on every GPU; RTX: RTX Video HDR, NVIDIA RTX)
//   SdrGamma         2.2 | 2.4                       default 2.2    (SDR to HDR decode power; never piecewise sRGB)
//   HdrLimit         DWORD 1/0                       default 1      (SDR to HDR: limit brightness on bright scenes)
//   DisplayPeak      Auto | 100..10000 (cd/m2)       default Auto   (the display's peak: Auto = what Windows reports)
//   Osd              Off | Brief | Always            default Brief  (on-screen status: about 3 s at start and on changes)
//   OnBattery        Light | Same                    default Light  (on battery: 2x at most, VSR level 2 at most, no clean-up / deband)
//   FgQuality        Auto | High | Medium | Low      default Auto   (Auto: Medium, High only where it fits at the same multiplier; PushFlow: Low/Medium/High = Fast/Balanced/Quality)
//   FgMethod         Auto | VFG | DLSSG | PushFlow | FSR | AMDFRC   default Auto (frame generation method; Auto by GPU family and budget)
//   Vrr              Auto | On | Off                 default Auto   (Auto: detected (NVIDIA G-SYNC state); On/Off: assume VRR / fixed refresh)
//   Upscaler         Auto | RTX | FSR | NIS          default Auto   (RTX VSR on NVIDIA RTX, else FSR)
//   UpscaleTarget    Screen | 1440p | 4K | 5K        default Screen
//   CleanUpStrength  Subtle | Balanced | Strong      default Strong (PushFPS's presets; Strong is its default)
//   VsrStyle         Auto | Standard | HighBitrate | Streaming   default Auto (RTX VSR model family; Auto picks it by the source bitrate)
//   Deband           Auto | Off | On                 default Auto   (PushMPC's own deband pass, 8-bit SDR sources)
//   DebandStrength   Low | Medium | High             default Medium
//   FrameLog         DWORD 1/0                       default 1
// Debug and test values stay in HKCU\Software\PushMPC\Filter (see ReadConfig in PushMpcFilter.cpp).
#pragma once
#include <windows.h>
#include <string>
#include <cwchar>
#include <cmath>

namespace pm {

inline constexpr const wchar_t* kSettingsKey = L"Software\\PushMPC";
inline constexpr int kAuto = -1, kOff = 0;

enum class FgQuality { Low = 0, Medium = 1, High = 2, Auto = 3 };
// Frame generation method (pushcore PUSHCORE_FG_* + 1; Auto = by GPU family and budget)
enum class FgMethod { Auto = 0, Vfg = 1, Dlssg = 2, PushFlow = 3, Fsr = 4, AmdFrc = 5 };
enum class VrrMode { Auto = 0, On = 1, Off = 2 };
enum class Upscaler { Auto = 0, Rtx = 1, Fsr = 2, Nis = 3 };
enum class UpscaleTarget { Screen = 0, P1440 = 1, K4 = 2, K5 = 3 };
enum class CleanStrength { Subtle = 0, Balanced = 1, Strong = 2 };
// RTX VSR model family (pushcore PUSHCORE_SR_FAMILY_*): Standard = RTX Video SDK (balanced); High bitrate keeps clean
// detail (no compression clean-up, for Blu-ray-class sources); Streaming cleans hardest (web video; Ampere or newer).
enum class VsrStyle { Standard = 0, HighBitrate = 1, Streaming = 2, Auto = 3 };
enum class DebandStrength { Low = 0, Medium = 1, High = 2 };
// SDR to HDR highlight expansion (PushMPC's curve: the multiplier M of SDR white; RTX Video HDR: its peak)
enum class HdrBoost { Auto = 0, Off = 1, Low = 2, Medium = 3, High = 4 };
enum class HdrMethod { Auto = 0, Rtx = 1 };
enum class OsdMode { Off = 0, Brief = 1, Always = 2 };
enum class BatteryMode { Light = 0, Same = 1 };

struct Settings {
    bool enable = true;
    int smoother = kAuto;   // kAuto, kOff, 2..8
    int sharper = kAuto;    // kAuto, kOff, 1..4
    int cleanUp = kAuto;    // kAuto, kOff, 1 = on
    int sdrToHdr = kAuto;   // kAuto, kOff, 1 = on
    HdrBoost hdrBoost = HdrBoost::Auto;
    HdrMethod hdrMethod = HdrMethod::Auto;
    bool sdrGamma24 = false;   // decode SDR with 2.4 instead of 2.2 (hidden)
    bool hdrLimit = true;      // SDR to HDR: the bright-scene limiter (both methods)
    OsdMode osd = OsdMode::Brief;
    BatteryMode onBattery = BatteryMode::Light;
    int displayPeak = 0;       // cd/m2; 0 = Auto (DXGI MaxLuminance). QD-OLED "True Black 400" modes report the same as
                               // their 1000-nit mode: set 400 there
    FgQuality fgQuality = FgQuality::Auto;
    FgMethod fgMethod = FgMethod::Auto;
    VrrMode vrr = VrrMode::Auto;
    Upscaler upscaler = Upscaler::Auto;
    UpscaleTarget target = UpscaleTarget::Screen;
    CleanStrength strength = CleanStrength::Strong;
    VsrStyle vsrStyle = VsrStyle::Auto;
    int deband = kAuto;     // kAuto, kOff, 1 = on
    DebandStrength debandStrength = DebandStrength::Medium;
    bool frameLog = true;
    bool operator==(const Settings&) const = default;
};

// PushFPS's look presets (qml/Veyra/PushQuickSetup.qml): tone / structure
inline void StrengthValues(CleanStrength s, float& tone, float& structure)
{
    switch (s) {
    case CleanStrength::Subtle: tone = 0.4f; structure = 0.5f; break;
    case CleanStrength::Balanced: tone = 0.7f; structure = 0.8f; break;
    default: tone = 1.0f; structure = 1.0f; break;
    }
}
inline CleanStrength NearestStrength(float tone, float structure)
{
    const float d[3] = { std::fabs(tone - 0.4f) + std::fabs(structure - 0.5f), std::fabs(tone - 0.7f) + std::fabs(structure - 0.8f),
                         std::fabs(tone - 1.0f) + std::fabs(structure - 1.0f) };
    return d[0] < d[1] && d[0] < d[2] ? CleanStrength::Subtle : d[1] < d[2] ? CleanStrength::Balanced : CleanStrength::Strong;
}

// ---- string forms (registry and the page)
inline std::wstring ChoiceStr(int v) { return v == kAuto ? L"Auto" : v == kOff ? L"Off" : std::to_wstring(v); }
inline std::wstring OnOffStr(int v) { return v == kAuto ? L"Auto" : v == kOff ? L"Off" : L"On"; }
inline const wchar_t* FgQualityStr(FgQuality q) { return q == FgQuality::High ? L"High" : q == FgQuality::Low ? L"Low" : q == FgQuality::Medium ? L"Medium" : L"Auto"; }
inline const wchar_t* FgMethodStr(FgMethod m) { switch (m) { case FgMethod::Vfg: return L"VFG"; case FgMethod::Dlssg: return L"DLSSG"; case FgMethod::PushFlow: return L"PushFlow"; case FgMethod::Fsr: return L"FSR"; case FgMethod::AmdFrc: return L"AMDFRC"; default: return L"Auto"; } }
inline const wchar_t* FgMethodLabel(FgMethod m) { switch (m) { case FgMethod::Vfg: return L"NVIDIA VFG"; case FgMethod::Dlssg: return L"DLSS-G"; case FgMethod::PushFlow: return L"PushFlow"; case FgMethod::Fsr: return L"FSR FG"; case FgMethod::AmdFrc: return L"AMD FRC"; default: return L"Auto"; } }
inline const wchar_t* VrrStr(VrrMode v) { return v == VrrMode::On ? L"On" : v == VrrMode::Off ? L"Off" : L"Auto"; }
inline const wchar_t* UpscalerStr(Upscaler u) { return u == Upscaler::Rtx ? L"RTX" : u == Upscaler::Fsr ? L"FSR" : u == Upscaler::Nis ? L"NIS" : L"Auto"; }
inline const wchar_t* TargetStr(UpscaleTarget t) { return t == UpscaleTarget::P1440 ? L"1440p" : t == UpscaleTarget::K4 ? L"4K" : t == UpscaleTarget::K5 ? L"5K" : L"Screen"; }
inline const wchar_t* StrengthStr(CleanStrength s) { return s == CleanStrength::Subtle ? L"Subtle" : s == CleanStrength::Balanced ? L"Balanced" : L"Strong"; }
inline const wchar_t* VsrStyleStr(VsrStyle v) { return v == VsrStyle::HighBitrate ? L"HighBitrate" : v == VsrStyle::Streaming ? L"Streaming" : v == VsrStyle::Standard ? L"Standard" : L"Auto"; }
inline const wchar_t* VsrStyleLabel(VsrStyle v) { return v == VsrStyle::HighBitrate ? L"high bitrate (keeps detail)" : v == VsrStyle::Streaming ? L"streaming (cleans hardest)" : L"standard"; }
inline const wchar_t* HdrBoostStr(HdrBoost b) { switch (b) { case HdrBoost::Off: return L"Off"; case HdrBoost::Low: return L"Low"; case HdrBoost::Medium: return L"Medium"; case HdrBoost::High: return L"High"; default: return L"Auto"; } }
inline const wchar_t* HdrMethodStr(HdrMethod m) { return m == HdrMethod::Rtx ? L"RTX" : L"Auto"; }
inline const wchar_t* DebandStrengthStr(DebandStrength d) { return d == DebandStrength::Low ? L"Low" : d == DebandStrength::High ? L"High" : L"Medium"; }

inline int ParseChoice(const std::wstring& s, int lo, int hi, int def)
{
    if (s.empty()) return def;
    if (_wcsicmp(s.c_str(), L"Auto") == 0) return kAuto;
    if (_wcsicmp(s.c_str(), L"Off") == 0) return kOff;
    const int n = _wtoi(s.c_str());
    return n >= lo && n <= hi ? n : def;
}
inline int ParseOnOff(const std::wstring& s, int def)
{
    if (s.empty()) return def;
    if (_wcsicmp(s.c_str(), L"Auto") == 0) return kAuto;
    if (_wcsicmp(s.c_str(), L"Off") == 0 || s == L"0") return kOff;
    if (_wcsicmp(s.c_str(), L"On") == 0 || s == L"1") return 1;
    return def;
}

inline std::wstring RegReadStr(HKEY k, const wchar_t* name)
{
    wchar_t buf[256] = {};
    DWORD sz = sizeof(buf) - 2, type = 0;
    if (k && RegQueryValueExW(k, name, nullptr, &type, (BYTE*)buf, &sz) == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) return buf;
    if (k && RegQueryValueExW(k, name, nullptr, &type, nullptr, nullptr) == ERROR_SUCCESS && type == REG_DWORD) {
        DWORD d = 0; sz = sizeof(d);
        if (RegQueryValueExW(k, name, nullptr, &type, (BYTE*)&d, &sz) == ERROR_SUCCESS) return std::to_wstring(d);
    }
    return {};
}
inline bool RegReadBool(HKEY k, const wchar_t* name, bool def)
{
    DWORD d = 0, sz = sizeof(d), type = 0;
    if (k && RegQueryValueExW(k, name, nullptr, &type, (BYTE*)&d, &sz) == ERROR_SUCCESS && type == REG_DWORD) return d != 0;
    const std::wstring s = RegReadStr(k, name);
    return s.empty() ? def : ParseOnOff(s, def ? 1 : 0) == 1;
}

inline Settings LoadSettings()
{
    Settings s;
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return s;
    s.enable = RegReadBool(k, L"Enable", true);
    s.smoother = ParseChoice(RegReadStr(k, L"Smoother"), 2, 8, kAuto);
    s.sharper = ParseChoice(RegReadStr(k, L"Sharper"), 1, 4, kAuto);
    s.cleanUp = ParseOnOff(RegReadStr(k, L"CleanUp"), kAuto);
    s.sdrToHdr = ParseOnOff(RegReadStr(k, L"SdrToHdr"), kAuto);
    const std::wstring q = RegReadStr(k, L"FgQuality");
    s.fgQuality = _wcsicmp(q.c_str(), L"High") == 0 ? FgQuality::High : _wcsicmp(q.c_str(), L"Low") == 0 ? FgQuality::Low
                : _wcsicmp(q.c_str(), L"Medium") == 0 ? FgQuality::Medium : FgQuality::Auto;
    // settings saved before version 2 wrote "Medium" as the default: that is Auto now (Medium, High where it fits)
    const int version = _wtoi(RegReadStr(k, L"SettingsVersion").c_str());
    if (version < 2 && s.fgQuality == FgQuality::Medium) s.fgQuality = FgQuality::Auto;
    const std::wstring fm = RegReadStr(k, L"FgMethod");
    s.fgMethod = _wcsicmp(fm.c_str(), L"VFG") == 0 ? FgMethod::Vfg : (_wcsicmp(fm.c_str(), L"DLSSG") == 0 || _wcsicmp(fm.c_str(), L"DLSS-G") == 0) ? FgMethod::Dlssg
               : _wcsicmp(fm.c_str(), L"PushFlow") == 0 ? FgMethod::PushFlow : _wcsicmp(fm.c_str(), L"FSR") == 0 ? FgMethod::Fsr
               : (_wcsicmp(fm.c_str(), L"AMDFRC") == 0 || _wcsicmp(fm.c_str(), L"FRC") == 0) ? FgMethod::AmdFrc : FgMethod::Auto;
    const std::wstring vr = RegReadStr(k, L"Vrr");
    s.vrr = _wcsicmp(vr.c_str(), L"On") == 0 ? VrrMode::On : _wcsicmp(vr.c_str(), L"Off") == 0 ? VrrMode::Off : VrrMode::Auto;
    const std::wstring up = RegReadStr(k, L"Upscaler");
    s.upscaler = _wcsicmp(up.c_str(), L"RTX") == 0 ? Upscaler::Rtx : _wcsicmp(up.c_str(), L"FSR") == 0 ? Upscaler::Fsr : _wcsicmp(up.c_str(), L"NIS") == 0 ? Upscaler::Nis : Upscaler::Auto;
    const std::wstring t = RegReadStr(k, L"UpscaleTarget");
    s.target = _wcsicmp(t.c_str(), L"1440p") == 0 ? UpscaleTarget::P1440 : _wcsicmp(t.c_str(), L"4K") == 0 ? UpscaleTarget::K4
             : _wcsicmp(t.c_str(), L"5K") == 0 ? UpscaleTarget::K5 : UpscaleTarget::Screen;
    const std::wstring c = RegReadStr(k, L"CleanUpStrength");
    s.strength = _wcsicmp(c.c_str(), L"Subtle") == 0 ? CleanStrength::Subtle : _wcsicmp(c.c_str(), L"Balanced") == 0 ? CleanStrength::Balanced : CleanStrength::Strong;
    const std::wstring v = RegReadStr(k, L"VsrStyle");
    s.vsrStyle = (_wcsicmp(v.c_str(), L"HighBitrate") == 0 || _wcsicmp(v.c_str(), L"High bitrate") == 0) ? VsrStyle::HighBitrate
               : _wcsicmp(v.c_str(), L"Streaming") == 0 ? VsrStyle::Streaming : _wcsicmp(v.c_str(), L"Standard") == 0 && version >= 2 ? VsrStyle::Standard : VsrStyle::Auto;
    s.deband = ParseOnOff(RegReadStr(k, L"Deband"), kAuto);
    const std::wstring ds = RegReadStr(k, L"DebandStrength");
    s.debandStrength = _wcsicmp(ds.c_str(), L"Low") == 0 ? DebandStrength::Low : _wcsicmp(ds.c_str(), L"High") == 0 ? DebandStrength::High : DebandStrength::Medium;
    const std::wstring hb = RegReadStr(k, L"HdrBoost");
    s.hdrBoost = _wcsicmp(hb.c_str(), L"Off") == 0 ? HdrBoost::Off : _wcsicmp(hb.c_str(), L"Low") == 0 ? HdrBoost::Low
               : _wcsicmp(hb.c_str(), L"Medium") == 0 ? HdrBoost::Medium : _wcsicmp(hb.c_str(), L"High") == 0 ? HdrBoost::High : HdrBoost::Auto;
    const std::wstring hm = RegReadStr(k, L"HdrMethod");
    s.hdrMethod = (_wcsicmp(hm.c_str(), L"RTX") == 0 || _wcsicmp(hm.c_str(), L"RTX Video HDR") == 0) ? HdrMethod::Rtx : HdrMethod::Auto;
    s.sdrGamma24 = RegReadStr(k, L"SdrGamma") == L"2.4";
    s.hdrLimit = RegReadBool(k, L"HdrLimit", true);
    { const int p = _wtoi(RegReadStr(k, L"DisplayPeak").c_str()); s.displayPeak = p >= 100 && p <= 10000 ? p : 0; }
    { const std::wstring o = RegReadStr(k, L"Osd"); s.osd = _wcsicmp(o.c_str(), L"Off") == 0 ? OsdMode::Off : _wcsicmp(o.c_str(), L"Always") == 0 ? OsdMode::Always : OsdMode::Brief; }
    s.onBattery = _wcsicmp(RegReadStr(k, L"OnBattery").c_str(), L"Same") == 0 ? BatteryMode::Same : BatteryMode::Light;
    s.frameLog = RegReadBool(k, L"FrameLog", true);
    RegCloseKey(k);
    return s;
}

inline bool SaveSettings(const Settings& s)
{
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
    auto str = [&](const wchar_t* n, const std::wstring& v) { RegSetValueExW(k, n, 0, REG_SZ, (const BYTE*)v.c_str(), DWORD((v.size() + 1) * sizeof(wchar_t))); };
    auto dw = [&](const wchar_t* n, bool v) { const DWORD d = v ? 1 : 0; RegSetValueExW(k, n, 0, REG_DWORD, (const BYTE*)&d, sizeof(d)); };
    dw(L"Enable", s.enable);
    str(L"Smoother", ChoiceStr(s.smoother));
    str(L"Sharper", ChoiceStr(s.sharper));
    str(L"CleanUp", OnOffStr(s.cleanUp));
    str(L"SdrToHdr", OnOffStr(s.sdrToHdr));
    str(L"FgQuality", FgQualityStr(s.fgQuality));
    str(L"FgMethod", FgMethodStr(s.fgMethod));
    str(L"Vrr", VrrStr(s.vrr));
    str(L"Upscaler", UpscalerStr(s.upscaler));
    str(L"SettingsVersion", L"2");
    str(L"UpscaleTarget", TargetStr(s.target));
    str(L"CleanUpStrength", StrengthStr(s.strength));
    str(L"VsrStyle", VsrStyleStr(s.vsrStyle));
    str(L"Deband", OnOffStr(s.deband));
    str(L"DebandStrength", DebandStrengthStr(s.debandStrength));
    str(L"HdrBoost", HdrBoostStr(s.hdrBoost));
    str(L"HdrMethod", HdrMethodStr(s.hdrMethod));
    dw(L"HdrLimit", s.hdrLimit);
    str(L"DisplayPeak", s.displayPeak ? std::to_wstring(s.displayPeak) : std::wstring(L"Auto"));
    str(L"Osd", s.osd == OsdMode::Off ? L"Off" : s.osd == OsdMode::Always ? L"Always" : L"Brief");
    str(L"OnBattery", s.onBattery == BatteryMode::Same ? L"Same" : L"Light");
    // SdrGamma is registry-only (never written here, so the page keeps it)
    dw(L"FrameLog", s.frameLog);
    RegCloseKey(k);
    return true;
}

} // namespace pm
