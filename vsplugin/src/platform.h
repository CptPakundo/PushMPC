// pushmpc.Enhance: settings and folders on every OS, for the portable backend (the nvidia backend keeps R1's own
// Windows code in pushmpc_vs.cpp / autopick.h).
//   Settings  Windows: HKCU\Software\PushMPC (the values MPC-HC's filter writes: Enable, Smoother, ...).
//             Linux, macOS: $XDG_CONFIG_HOME/pushmpc/settings.ini, else ~/.config/pushmpc/settings.ini; the same names
//             as keys ("Smoother=4"), any section, case-insensitive, # and ; comments.
//   Logs      Windows: %LOCALAPPDATA%\PushMPC\logs (vslog.h). Linux: $XDG_STATE_HOME/pushmpc/logs, else
//             ~/.local/state/pushmpc/logs. macOS: ~/Library/Logs/PushMPC. PUSHMPC_VS_LOG_DIR overrides it everywhere.
// The resolvers are pure functions (portable_test checks them on any OS); the readers touch the OS.
#pragma once
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#ifdef _WIN32
#include <windows.h>
#endif

namespace pmvs {

inline std::string LowerAscii(std::string s) { for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; }
inline std::string TrimAscii(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}
// key=value lines; keys lower-cased; sections, blank lines and # ; comments skipped; the last value wins
inline std::map<std::string, std::string> ParseIni(const std::string& text)
{
    std::map<std::string, std::string> m;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const std::string t = TrimAscii(line);
        if (t.empty() || t[0] == '#' || t[0] == ';' || t[0] == '[') continue;
        const size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string v = TrimAscii(t.substr(eq + 1));
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        m[LowerAscii(TrimAscii(t.substr(0, eq)))] = v;
    }
    return m;
}
inline std::string ResolveSettingsIni(const std::string& xdgConfigHome, const std::string& home)
{
    if (!xdgConfigHome.empty() && xdgConfigHome[0] == '/') return xdgConfigHome + "/pushmpc/settings.ini";
    return home.empty() ? std::string() : home + "/.config/pushmpc/settings.ini";
}
inline std::string ResolvePosixLogDir(const std::string& overrideDir, const std::string& xdgStateHome, const std::string& home, bool mac)
{
    if (!overrideDir.empty()) return overrideDir;
    if (mac) return home.empty() ? std::string() : home + "/Library/Logs/PushMPC";
    if (!xdgStateHome.empty() && xdgStateHome[0] == '/') return xdgStateHome + "/pushmpc/logs";
    return home.empty() ? std::string() : home + "/.local/state/pushmpc/logs";
}
inline std::string EnvA(const char* name) { const char* v = std::getenv(name); return v ? std::string(v) : std::string(); }

// one setting by its registry / ini name ("" when absent)
inline std::string ReadSetting(const char* name)
{
#ifdef _WIN32
    HKEY k = nullptr;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\PushMPC", 0, KEY_READ, &k) != ERROR_SUCCESS) return {};
    char buf[512] = {}; DWORD type = 0, size = sizeof(buf) - 1;
    std::string v;
    if (RegQueryValueExA(k, name, nullptr, &type, reinterpret_cast<BYTE*>(buf), &size) == ERROR_SUCCESS) {
        if (type == REG_SZ) v.assign(buf);
        else if (type == REG_DWORD && size == 4) v = std::to_string(*reinterpret_cast<DWORD*>(buf));
    }
    RegCloseKey(k);
    return v;
#else
    static const std::map<std::string, std::string> ini = [] {
        const std::string path = ResolveSettingsIni(EnvA("XDG_CONFIG_HOME"), EnvA("HOME"));
        std::ifstream f(path, std::ios::binary);
        std::stringstream ss; ss << f.rdbuf();
        return ParseIni(ss.str());
    }();
    auto it = ini.find(LowerAscii(name));
    return it == ini.end() ? std::string() : it->second;
#endif
}

// the portable backend's settings: the registry / ini values, then the plugin arguments
struct PortableSettings {
    bool enable = true;
    int smoother = 0;     // 0 Auto, 1 off, 2..8
    int quality = 1;      // 0 fast, 1 balanced
    int threads = 0;      // 0 = all hardware threads (at most 16)
};
inline int ParseSmoother(const std::string& v, int def)
{
    const std::string s = LowerAscii(TrimAscii(v));
    if (s.empty()) return def;
    if (s == "auto") return 0;
    if (s == "off" || s == "0" || s == "1" || s == "false") return 1;
    const std::string n = s[0] == 'x' ? s.substr(1) : s;
    const int k = std::atoi(n.c_str());
    return k >= 2 && k <= 8 ? k : def;
}
inline bool ParseBool(const std::string& v, bool def)
{
    const std::string s = LowerAscii(TrimAscii(v));
    if (s == "0" || s == "off" || s == "false" || s == "no") return false;
    if (s == "1" || s == "on" || s == "true" || s == "yes") return true;
    return def;
}
// get(name) returns the stored value or ""; names as the registry's ("Enable", "Smoother", "PortableQuality", "PortableThreads")
template <class Get> PortableSettings LoadPortableSettings(Get&& get)
{
    PortableSettings s;
    s.enable = ParseBool(get("Enable"), true);
    s.smoother = ParseSmoother(get("Smoother"), 0);
    const std::string q = LowerAscii(TrimAscii(get("PortableQuality")));
    s.quality = q == "fast" || q == "0" ? 0 : 1;
    const int t = std::atoi(get("PortableThreads").c_str());
    s.threads = t > 0 && t <= 64 ? t : 0;
    return s;
}

} // namespace pmvs
