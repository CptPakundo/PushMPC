// pushmpc.Enhance, backend "nvidia" on Linux: the settings, from ~/.config/pushmpc/settings.ini (platform.h; the same
// names and words as the Windows registry values the MPC-HC filter writes) and then the plugin arguments (the same
// argument names as on Windows). Pure C++: nvl_test.cpp checks the parsing.
#pragma once
#include <cstdlib>
#include <string>
#include "nvl_auto.h"
#include "platform.h"

namespace pmvs::nvl {

// FgMethod: VFG -> 0; NVOF / OpticalFlow -> 1 (NVIDIA optical flow + warp); anything else (Auto, the Windows-only methods) -> -1
inline int ParseFgMethod(const std::string& v)
{
    const std::string s = LowerAscii(TrimAscii(v));
    return s == "vfg" ? 0 : (s == "nvof" || s == "opticalflow" || s == "optical-flow") ? 1 : -1;
}
// "Auto" / "Off" / "N" (lo..hi) -> 0 Auto, -1 off, N
inline int ParseLevel(const std::string& v, int lo, int hi, int def)
{
    const std::string s = LowerAscii(TrimAscii(v));
    if (s.empty()) return def;
    if (s == "auto") return 0;
    if (s == "off" || s == "0" || s == "false" || s == "no") return -1;
    const int n = std::atoi((s[0] == 'x' ? s.substr(1) : s).c_str());
    return n >= lo && n <= hi ? n : def;
}
// get(name) returns the stored value or ""; names: the registry's ("Enable", "Smoother", "Sharper", "FgQuality",
// "SdrToHdr", "VsrStyle", "UpscaleTarget", "DisplayPeak")
template <class Get> Settings LoadSettings(Get&& get)
{
    Settings s;
    s.enable = ParseBool(get("Enable"), true);
    const int sm = ParseLevel(get("Smoother"), 2, 16, 0);
    s.smoother = sm < 0 ? 1 : sm;   // Settings: 0 Auto, 1 off, 2..16
    s.sharper = ParseLevel(get("Sharper"), 1, 4, 0);
    const std::string q = LowerAscii(TrimAscii(get("FgQuality")));
    s.fgQuality = q == "low" ? 0 : q == "medium" ? 1 : q == "high" ? 2 : -1;
    s.fgMethod = ParseFgMethod(get("FgMethod"));
    const std::string h = LowerAscii(TrimAscii(get("SdrToHdr")));
    s.sdrToHdr = h == "on" || h == "1" ? 1 : h == "off" || h == "0" ? -1 : 0;
    const std::string st = LowerAscii(TrimAscii(get("VsrStyle")));
    s.vsrStyle = st == "standard" ? 0 : st == "highbitrate" ? 1 : st == "streaming" ? 2 : -1;
    const std::string t = LowerAscii(TrimAscii(get("UpscaleTarget")));
    s.targetH = t == "1440p" ? 1440 : t == "5k" ? 2880 : 2160;   // Screen: the display's size when mpv reports it, else 4K
    s.targetScreen = !(t == "1440p" || t == "4k" || t == "5k");
    const double peak = std::atof(get("DisplayPeak").c_str());
    if (peak >= 400 && peak <= 2000) { s.hdrPeak = peak; s.hdrPeakSet = true; }
    return s;
}
// the plugin arguments over it (key as the plugin's argument names)
inline bool ApplyArg(Settings& s, const std::string& key, const std::string& v)
{
    if (key == "enable") { s.enable = ParseBool(v, s.enable); return true; }
    if (key == "smoother") { const int sm = ParseLevel(v, 2, 16, s.smoother == 1 ? -1 : s.smoother); s.smoother = sm < 0 ? 1 : sm; return true; }
    if (key == "sharper") { s.sharper = ParseLevel(v, 1, 4, s.sharper); return true; }
    if (key == "fg_method") { s.fgMethod = ParseFgMethod(v); return true; }
    if (key == "fg_quality") { const std::string q = LowerAscii(v); s.fgQuality = q == "low" ? 0 : q == "medium" ? 1 : q == "high" ? 2 : -1; return true; }
    if (key == "sdr_to_hdr") { const std::string h = LowerAscii(v); s.sdrToHdr = h == "on" || h == "1" ? 1 : h == "off" || h == "0" ? -1 : 0; return true; }
    if (key == "vsr_style") { const std::string st = LowerAscii(v); s.vsrStyle = st == "standard" ? 0 : st == "highbitrate" ? 1 : st == "streaming" ? 2 : -1; return true; }
    if (key == "target") { const std::string t = LowerAscii(v); s.targetH = t == "1440p" ? 1440 : t == "5k" ? 2880 : 2160; s.targetScreen = !(t == "1440p" || t == "4k" || t == "5k"); return true; }
    if (key == "hdr_peak") { const double p = std::atof(v.c_str()); if (p >= 400 && p <= 2000) { s.hdrPeak = p; s.hdrPeakSet = true; } return true; }
    return false;
}

} // namespace pmvs::nvl
