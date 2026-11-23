// PushMPC: helpers for the release round. G-SYNC in MPC-HC's own NVIDIA driver profile (apply / restore, never the
// global profile), the updater hook (PushMpcUpdater.exe, HKCU\Software\PushMPC\Update) and anonymised diagnostics.
#pragma once
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <cwctype>
#include <format>
#include <string>
#include <vector>
#include "PushMpcAuto.h"
#include "version.h"

namespace pm {

// ------------------------------------------------------------------------------------------------ G-SYNC profile
// The two G-SYNC scope values, both 0 off, 1 fullscreen only, 2 fullscreen and windowed (PushFPS docs\
// NVIDIA_GSYNC_PROFILE_2026-11-14.md): VRR_MODE 0x1194F158 ("GSYNC - Application Mode") and VRRREQUESTSTATE 0x1094F1F7
// ("GSYNC - Global Mode", which can be stored in a program's profile too). Apply writes 2 / 2 into the profile of each
// player: mpc-hc64.exe always (the player PushMPC installs into, what the button says), mpc-be64.exe only when MPC-BE
// is installed (its exe in its registry install path or the usual Program Files folders) or already has a driver
// profile. A player without its own profile gets one ("PushMPC <exe>") holding only that program. The values found
// first are kept in HKCU\Software\PushMPC\NvBackup\<exe> ("absent" = the profile had no value of its own, it inherited
// the global one; Created = 1: the profile is ours and restore deletes it). The global profile is never written.
inline constexpr uint32_t kNvVrrMode = 0x1194F158, kNvVrrRequest = 0x1094F1F7;
inline const wchar_t* const kNvPlayers[] = { L"mpc-hc64.exe", L"mpc-be64.exe" };
// MPC-BE (x64) on this machine: its registry install path (HKCU/HKLM Software\MPC-BE ExePath, an Uninstall entry named
// MPC-BE with InstallLocation), else %ProgramFiles%\MPC-BE x64 or \MPC-BE; the exe must exist
inline bool MpcBeInstalled()
{
    auto exists = [](const std::wstring& p) { const DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); };
    auto regStr = [](HKEY root, const wchar_t* key, const wchar_t* name, REGSAM view) {
        std::wstring v; HKEY k = nullptr;
        if (RegOpenKeyExW(root, key, 0, KEY_READ | view, &k) == ERROR_SUCCESS) {
            wchar_t b[MAX_PATH * 2] = {}; DWORD sz = sizeof(b) - sizeof(wchar_t), t = 0;
            if (RegQueryValueExW(k, name, nullptr, &t, (BYTE*)b, &sz) == ERROR_SUCCESS && (t == REG_SZ || t == REG_EXPAND_SZ)) v = b;
            RegCloseKey(k);
        }
        return v;
    };
    auto inFolder = [&](std::wstring dir) { if (dir.empty()) return false; if (dir.back() != L'\\') dir += L'\\'; return exists(dir + L"mpc-be64.exe"); };
    for (HKEY root : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE })
        for (REGSAM view : { REGSAM(KEY_WOW64_64KEY), REGSAM(KEY_WOW64_32KEY) }) {
            std::wstring p = regStr(root, L"Software\\MPC-BE", L"ExePath", view);
            if (!p.empty() && (exists(p) || inFolder(p))) return true;
            HKEY u = nullptr;
            if (RegOpenKeyExW(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0, KEY_READ | view, &u) != ERROR_SUCCESS) continue;
            wchar_t sub[256];
            for (DWORD i = 0; ; ++i) {
                DWORD n = 256;
                if (RegEnumKeyExW(u, i, sub, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                const std::wstring key = std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\") + sub;
                if (_wcsnicmp(regStr(root, key.c_str(), L"DisplayName", view).c_str(), L"MPC-BE", 6) == 0 && inFolder(regStr(root, key.c_str(), L"InstallLocation", view))) { RegCloseKey(u); return true; }
            }
            RegCloseKey(u);
        }
    wchar_t pf[MAX_PATH] = {};
    for (const wchar_t* var : { L"ProgramW6432", L"ProgramFiles" })
        if (GetEnvironmentVariableW(var, pf, MAX_PATH) && (inFolder(std::wstring(pf) + L"\\MPC-BE x64") || inFolder(std::wstring(pf) + L"\\MPC-BE"))) return true;
    return false;
}
inline constexpr const wchar_t* kNvBackupKey = L"Software\\PushMPC\\NvBackup";
namespace nvp {
#pragma pack(push, 4)
struct DrsProfile { uint32_t version; wchar_t name[2048]; uint32_t gpuSupport, predefined, apps, settings; };
#pragma pack(pop)
static_assert(sizeof(DrsProfile) == 4116, "NVAPI DRS profile layout");
using Fn0 = int(__cdecl*)(void**);
using Fn1 = int(__cdecl*)(void*);
using FnFind = int(__cdecl*)(void*, wchar_t*, void**, detail::DrsApplication*);
using FnGet = int(__cdecl*)(void*, void*, uint32_t, detail::DrsSetting*);
using FnSet = int(__cdecl*)(void*, void*, detail::DrsSetting*);
using FnDelSetting = int(__cdecl*)(void*, void*, uint32_t);
using FnCreateProfile = int(__cdecl*)(void*, DrsProfile*, void**);
using FnCreateApp = int(__cdecl*)(void*, void*, detail::DrsApplication*);
using FnDelProfile = int(__cdecl*)(void*, void*);
struct Api {
    Fn0 create = nullptr; Fn1 load = nullptr, save = nullptr, destroy = nullptr; FnFind find = nullptr; FnGet get = nullptr; FnSet set = nullptr;
    FnDelSetting delSetting = nullptr; FnCreateProfile createProfile = nullptr; FnCreateApp createApp = nullptr; FnDelProfile delProfile = nullptr;
    bool ok() const { return create && load && save && destroy && find && get && set && delSetting && createProfile && createApp && delProfile; }
};
inline Api Load()
{
    Api a; auto q = detail::NvApi();
    if (!q) return a;
    a.create = reinterpret_cast<Fn0>(q(0x0694D52E)); a.load = reinterpret_cast<Fn1>(q(0x375DBD6B)); a.save = reinterpret_cast<Fn1>(q(0xFCBC7E14));
    a.destroy = reinterpret_cast<Fn1>(q(0xDAD9CFF8)); a.find = reinterpret_cast<FnFind>(q(0xEEE566B2)); a.get = reinterpret_cast<FnGet>(q(0x73BF8338));
    a.set = reinterpret_cast<FnSet>(q(0x577DD202)); a.delSetting = reinterpret_cast<FnDelSetting>(q(0xE4A26362));
    a.createProfile = reinterpret_cast<FnCreateProfile>(q(0xCC176068)); a.createApp = reinterpret_cast<FnCreateApp>(q(0x4347A9DE));
    a.delProfile = reinterpret_cast<FnDelProfile>(q(0x17093206));
    return a;
}
// the player's own profile (nullptr: none), its value of `id` there (-1: none of its own)
inline void* FindProfile(const Api& a, void* s, const wchar_t* exe)
{
    auto* app = new detail::DrsApplication{}; app->version = uint32_t(sizeof(detail::DrsApplication)) | (1u << 16);
    std::vector<wchar_t> name(exe, exe + wcslen(exe) + 1);
    void* prof = nullptr;
    if (a.find(s, name.data(), &prof, app) != 0) prof = nullptr;
    delete app;
    return prof;
}
inline int OwnValue(const Api& a, void* s, void* prof, uint32_t id)
{
    auto* st = new detail::DrsSetting{}; st->version = uint32_t(sizeof(detail::DrsSetting)) | (1u << 16);
    const int r = a.get(s, prof, id, st) == 0 && st->type == 0 && st->location == 0 ? int(st->current.dword) : -1;   // location 0: this profile
    delete st;
    return r;
}
inline bool SetValue(const Api& a, void* s, void* prof, uint32_t id, uint32_t v)
{
    auto* st = new detail::DrsSetting{}; st->version = uint32_t(sizeof(detail::DrsSetting)) | (1u << 16);
    st->id = id; st->type = 0; st->current.dword = v;
    const bool ok = a.set(s, prof, st) == 0;
    delete st;
    return ok;
}
}  // namespace nvp
// 1: every installed player's profile has G-SYNC for windowed and full screen; 0: at least one does not; -1: no NVAPI
inline int NvGsyncState()
{
    const auto a = nvp::Load();
    if (!a.ok()) return -1;
    void* s = nullptr;
    if (a.create(&s) != 0 || !s) return -1;
    int state = 1;
    if (a.load(s) != 0) state = -1;
    else {
        // the host's exe decides (mpc-hc64.exe, or mpc-be64.exe when MPC-BE hosts the filter); the other one only when
        // it has a profile of its own
        const wchar_t* decides = HostPlayer() == Host::MpcBe ? kNvPlayers[1] : kNvPlayers[0];
        for (const wchar_t* exe : kNvPlayers) {
            void* prof = nvp::FindProfile(a, s, exe);
            if (!prof) { if (exe == decides) state = 0; continue; }
            if (nvp::OwnValue(a, s, prof, kNvVrrMode) != 2 || nvp::OwnValue(a, s, prof, kNvVrrRequest) != 2) state = 0;
        }
    }
    a.destroy(s);
    return state;
}
inline std::wstring NvGsyncApply()
{
    const auto a = nvp::Load();
    if (!a.ok()) return L"NVAPI is not available (no NVIDIA driver)";
    void* s = nullptr;
    if (a.create(&s) != 0 || !s) return L"NVAPI session failed";
    std::wstring result;
    if (a.load(s) != 0) result = L"the driver settings could not be read";
    else {
        for (const wchar_t* exe : kNvPlayers) {
            const std::wstring bk = std::wstring(kNvBackupKey) + L"\\" + exe;
            void* prof = nvp::FindProfile(a, s, exe);
            // MPC-BE: only when it is installed or already has a driver profile (no profile for a program that is not here)
            if (!prof && exe != kNvPlayers[0] && !MpcBeInstalled()) continue;
            HKEY k = nullptr; DWORD disp = 0;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, bk.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &k, &disp) != ERROR_SUCCESS) continue;
            const bool first = disp == REG_CREATED_NEW_KEY;
            if (!prof) {
                nvp::DrsProfile p{}; p.version = uint32_t(sizeof(p)) | (1u << 16);
                wcsncpy_s(p.name, (std::wstring(L"PushMPC ") + exe).c_str(), _TRUNCATE);
                auto* app = new detail::DrsApplication{}; app->version = uint32_t(sizeof(detail::DrsApplication)) | (1u << 16);
                wcsncpy_s(app->name, exe, _TRUNCATE);
                const bool made = a.createProfile(s, &p, &prof) == 0 && prof && a.createApp(s, prof, app) == 0;
                delete app;
                if (!made) { result += std::format(L"{}: the profile could not be created; ", exe); RegCloseKey(k); continue; }
                const DWORD one = 1; RegSetValueExW(k, L"Created", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
            } else if (first) {
                for (uint32_t id : { kNvVrrMode, kNvVrrRequest }) {
                    const int v = nvp::OwnValue(a, s, prof, id);
                    const std::wstring n = std::format(L"{:08X}", id);
                    if (v >= 0) { const DWORD d = DWORD(v); RegSetValueExW(k, n.c_str(), 0, REG_DWORD, (const BYTE*)&d, sizeof(d)); }
                    else { const wchar_t t[] = L"absent"; RegSetValueExW(k, n.c_str(), 0, REG_SZ, (const BYTE*)t, sizeof(t)); }
                }
            }
            RegCloseKey(k);
            if (!nvp::SetValue(a, s, prof, kNvVrrMode, 2) || !nvp::SetValue(a, s, prof, kNvVrrRequest, 2)) result += std::format(L"{}: the setting was refused; ", exe);
        }
        if (a.save(s) != 0) result += L"the driver settings could not be saved";
    }
    a.destroy(s);
    if (!result.empty()) return result;
    return MpcBeInstalled() ? L"G-SYNC is on for MPC-HC and MPC-BE (windowed and full screen); restart the player to use it"
                            : L"G-SYNC is on for MPC-HC (windowed and full screen); restart MPC-HC to use it";
}
// back to what PushMPC found; true when nothing is left to restore
inline bool NvGsyncRestore(std::wstring* note = nullptr)
{
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kNvBackupKey, 0, KEY_READ, &root) != ERROR_SUCCESS) return true;
    RegCloseKey(root);
    const auto a = nvp::Load();
    if (!a.ok()) { if (note) *note = L"NVAPI is not available: the backup is kept"; return false; }
    void* s = nullptr;
    if (a.create(&s) != 0 || !s) return false;
    bool ok = a.load(s) == 0;
    for (const wchar_t* exe : kNvPlayers) {
        if (!ok) break;
        const std::wstring bk = std::wstring(kNvBackupKey) + L"\\" + exe;
        HKEY k = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, bk.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) continue;
        void* prof = nvp::FindProfile(a, s, exe);
        DWORD created = 0, sz = sizeof(created), type = 0;
        if (prof && RegQueryValueExW(k, L"Created", nullptr, &type, (BYTE*)&created, &sz) == ERROR_SUCCESS && created) ok = a.delProfile(s, prof) == 0 && ok;
        else if (prof) {
            for (uint32_t id : { kNvVrrMode, kNvVrrRequest }) {
                const std::wstring n = std::format(L"{:08X}", id);
                DWORD d = 0; sz = sizeof(d); type = 0;
                if (RegQueryValueExW(k, n.c_str(), nullptr, &type, (BYTE*)&d, &sz) == ERROR_SUCCESS && type == REG_DWORD) ok = nvp::SetValue(a, s, prof, id, d) && ok;
                else if (type == REG_SZ) a.delSetting(s, prof, id);
            }
        }
        RegCloseKey(k);
    }
    if (ok) ok = a.save(s) == 0;
    a.destroy(s);
    if (ok) RegDeleteTreeW(HKEY_CURRENT_USER, kNvBackupKey);
    if (note) *note = ok ? L"restored" : L"could not restore: the backup is kept";
    return ok;
}

// ------------------------------------------------------------------------------------------------ updater
inline constexpr const wchar_t* kUpdateKey = L"Software\\PushMPC\\Update";
struct UpdateInfo { std::wstring status, available, lastCheck, message, channel = L"Stable"; bool enabled = true; };
inline std::wstring RegStr(HKEY k, const wchar_t* n)
{
    wchar_t b[1024] = {}; DWORD sz = sizeof(b) - 2, t = 0;
    if (RegQueryValueExW(k, n, nullptr, &t, (BYTE*)b, &sz) == ERROR_SUCCESS) {
        if (t == REG_SZ || t == REG_EXPAND_SZ) return b;
        if (t == REG_DWORD) return std::to_wstring(*reinterpret_cast<DWORD*>(b));
        if (t == REG_QWORD) return std::to_wstring(*reinterpret_cast<unsigned long long*>(b));
    }
    return {};
}
inline UpdateInfo ReadUpdateInfo()
{
    UpdateInfo u; HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kUpdateKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return u;
    u.status = RegStr(k, L"Status"); u.available = RegStr(k, L"AvailableVersion"); u.lastCheck = RegStr(k, L"LastCheck"); u.message = RegStr(k, L"Message");
    const std::wstring ch = RegStr(k, L"Channel"); if (!ch.empty()) u.channel = ch;
    const std::wstring en = RegStr(k, L"Enabled"); u.enabled = en != L"0";
    RegCloseKey(k);
    return u;
}
inline void WriteUpdatePrefs(bool enabled, const std::wstring& channel)
{
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUpdateKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    const DWORD e = enabled ? 1 : 0; RegSetValueExW(k, L"Enabled", 0, REG_DWORD, (const BYTE*)&e, sizeof(e));
    RegSetValueExW(k, L"Channel", 0, REG_SZ, (const BYTE*)channel.c_str(), DWORD((channel.size() + 1) * 2));
    RegCloseKey(k);
}
inline std::wstring UpdaterPath(const std::wstring& localAppData) { return localAppData + L"\\PushMPC\\PushMpcUpdater.exe"; }
// detached, below-normal priority, no window; false when the updater is not installed
inline bool LaunchUpdater(const std::wstring& localAppData, const wchar_t* arg)
{
    const std::wstring exe = UpdaterPath(localAppData);
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    std::wstring cmd = L"\"" + exe + L"\" " + arg;
    STARTUPINFOW si{}; si.cb = sizeof(si); PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | CREATE_BREAKAWAY_FROM_JOB,
                        nullptr, nullptr, &si, &pi)
        && !CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return true;
}
// `--auto` at most once per 24 h (HKCU\Software\PushMPC\Update\LastAutoLaunch, FILETIME), when enabled
inline bool MaybeAutoUpdate(const std::wstring& localAppData)
{
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUpdateKey, 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
    DWORD en = 1, sz = sizeof(en), t = 0;
    if (RegQueryValueExW(k, L"Enabled", nullptr, &t, (BYTE*)&en, &sz) != ERROR_SUCCESS || t != REG_DWORD) en = 1;
    unsigned long long last = 0; sz = sizeof(last); t = 0;
    if (RegQueryValueExW(k, L"LastAutoLaunch", nullptr, &t, (BYTE*)&last, &sz) != ERROR_SUCCESS || t != REG_QWORD) last = 0;
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    const unsigned long long now = (unsigned long long(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    bool launched = false;
    if (en != 0 && (last == 0 || now - last >= 24ull * 3600ull * 10000000ull) && LaunchUpdater(localAppData, L"--auto")) {
        RegSetValueExW(k, L"LastAutoLaunch", 0, REG_QWORD, (const BYTE*)&now, sizeof(now));
        launched = true;
    }
    RegCloseKey(k);
    return launched;
}

// ------------------------------------------------------------------------------------------------ diagnostics
// For public bug reports: video file names are dropped (the extension stays: "<video>.mkv"), profile paths, other
// absolute folders outside Windows' own, the user name and the computer name become placeholders.
// A linear scan (no std::regex: MSVC's backtracking matcher threw std::regex_error on a 200-line log, which crashed
// MPC-HC from the About page, RC1 F4). The steps, in order:
//   1. video files, on the raw text: an absolute path (X:\ or \\server, spaces allowed, up to \r \n " < > | * ?)
//      ending in a video extension, else a bare name (no spaces; a quoted name may have them) -> <video>.ext
//   2. %LOCALAPPDATA%, %APPDATA%, %USERPROFILE% for those folders (case-insensitive)
//   3. any other absolute drive path, except under Program Files, Windows and ProgramData -> X:\<path>
//   4. the user name -> <user>, the computer name -> <computer>
namespace anon {
inline bool WordCh(wchar_t c) { return iswalnum(c) || c == L'_'; }
inline bool PathCh(wchar_t c) { return c != L'\r' && c != L'\n' && c != L'"' && c != L'<' && c != L'>' && c != L'|' && c != L'*' && c != L'?'; }
inline bool NameCh(wchar_t c) { return !iswspace(c) && c != L'"' && c != L'\'' && c != L'<' && c != L'>' && c != L'|' && c != L'*' && c != L'?' && c != L':'; }
inline const wchar_t* const kVideoExt[] = { L"mkv", L"mp4", L"m4v", L"avi", L"mov", L"ts", L"m2ts", L"mts", L"webm", L"wmv", L"mpg", L"mpeg", L"flv",
                                            L"vob", L"3gp", L"ogv", L"rmvb", L"hevc", L"h264", L"264", L"265" };
// the length of the video extension starting at s[p] (just after the dot), 0 = none (a whole word: ".mkv", not ".mkvx")
inline size_t VideoExt(const std::wstring& s, size_t p)
{
    size_t best = 0;
    for (const wchar_t* e : kVideoExt) {
        const size_t n = wcslen(e);
        if (n > best && p + n <= s.size() && _wcsnicmp(s.c_str() + p, e, n) == 0 && (p + n == s.size() || !WordCh(s[p + n]))) best = n;
    }
    return best;
}
// an absolute path starts here: "X:\" (or X:/) not inside a word, or "\\server" after a separator
inline bool DriveStart(const std::wstring& s, size_t i)
{
    return i + 2 < s.size() && iswalpha(s[i]) && s[i + 1] == L':' && (s[i + 2] == L'\\' || s[i + 2] == L'/') && (i == 0 || !WordCh(s[i - 1]));
}
inline bool PathStart(const std::wstring& s, size_t i)
{
    if (DriveStart(s, i)) return true;
    return i + 2 < s.size() && s[i] == L'\\' && s[i + 1] == L'\\' && s[i + 2] != L'\\' &&
           (i == 0 || iswspace(s[i - 1]) || s[i - 1] == L'=' || s[i - 1] == L'"' || s[i - 1] == L'\'' || s[i - 1] == L'(');
}
inline std::wstring Lower(std::wstring s) { for (auto& c : s) c = wchar_t(towlower(c)); return s; }
inline void ReplaceAllCi(std::wstring& s, const std::wstring& from, const std::wstring& to)
{
    if (from.size() < 3) return;
    const std::wstring ls = Lower(s), lf = Lower(from);
    std::wstring out; out.reserve(s.size());
    size_t p = 0;
    for (size_t q; (q = ls.find(lf, p)) != std::wstring::npos; p = q + from.size()) { out.append(s, p, q - p); out += to; }
    out.append(s, p, std::wstring::npos);
    s.swap(out);
}
inline std::wstring Videos(const std::wstring& s)
{
    const size_t npos = std::wstring::npos;
    std::wstring out; out.reserve(s.size());
    size_t emitted = 0, lineStart = 0, pathStart = npos;   // pathStart: the latest path start in the current run of path characters
    for (size_t i = 0; i < s.size(); ++i) {
        const wchar_t c = s[i];
        if (c == L'\r' || c == L'\n') { pathStart = npos; lineStart = i + 1; continue; }
        if (!PathCh(c)) pathStart = npos;
        else if (PathStart(s, i)) pathStart = i;
        if (c != L'.' || i == 0) continue;
        const size_t n = VideoExt(s, i + 1);
        if (!n) continue;
        // "1.264 ms" (a number) and "H.264" / "H.265" (the codecs) before a numeric extension are not file names
        if (iswdigit(s[i + 1])) {
            size_t b = i;
            while (b > lineStart && (iswdigit(s[b - 1]) || s[b - 1] == L'.' || s[b - 1] == L',')) --b;
            if (b < i && (b == lineStart || !NameCh(s[b - 1]) || s[b - 1] == L'-' || s[b - 1] == L'+')) continue;
            if (i >= 1 && (s[i - 1] == L'H' || s[i - 1] == L'h') && (i == 1 || !NameCh(s[i - 2]) || s[i - 2] == L'-')) continue;
        }
        size_t start = npos;
        // a path's file name is its last segment; "; " in it means the line went on after the path ("x.dll; VSR ...")
        if (pathStart != npos && pathStart >= emitted && pathStart < i) {
            size_t seg = i;
            while (seg > pathStart && s[seg - 1] != L'\\' && s[seg - 1] != L'/') --seg;
            if (s.substr(seg, i - seg).find(L"; ") == std::wstring::npos) start = pathStart;
        }
        if (start != npos) {}
        else {
            size_t b = i;
            while (b > emitted && b > lineStart && NameCh(s[b - 1])) --b;
            if (b >= emitted + 2 && b >= lineStart + 2 && s[b - 1] == L':' && iswalpha(s[b - 2])) b -= 2;   // "X:name.mkv"
            bool word = false;   // "1.264 ms" is a number, not a file
            for (size_t k = b; k < i && !word; ++k) word = !(iswdigit(s[k]) || s[k] == L'.' || s[k] == L',' || s[k] == L'-' || s[k] == L'+');
            if (b < i && word) {
                start = b;
                // a quoted name may hold spaces: "Some Title (2020).mkv"
                const size_t close = i + 1 + n;
                if (close < s.size() && (s[close] == L'"' || s[close] == L'\'')) {
                    for (size_t k = b; k > lineStart && k > emitted; --k)
                        if (s[k - 1] == s[close]) { start = k; break; }
                }
            }
        }
        if (start == npos) continue;
        out.append(s, emitted, start - emitted);
        out += L"<video>.";
        out.append(s, i + 1, n);
        emitted = i + 1 + n;
        i = emitted - 1;
        pathStart = npos;
    }
    out.append(s, emitted, npos);
    return out;
}
// absolute drive paths outside Windows' own folders (a user's folders on any drive, a portable install, ...)
inline std::wstring OtherPaths(const std::wstring& s)
{
    static const wchar_t* const keep[] = { L"program files", L"windows", L"programdata" };
    std::wstring out; out.reserve(s.size());
    size_t emitted = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (!DriveStart(s, i)) continue;
        const size_t body = i + 3;
        bool system = false;
        for (const wchar_t* k : keep) {
            const size_t n = wcslen(k);
            if (body + n <= s.size() && _wcsnicmp(s.c_str() + body, k, n) == 0 && (body + n == s.size() || !WordCh(s[body + n]))) system = true;
        }
        size_t end = body;
        while (end < s.size() && PathCh(s[end]) && s[end] != L';') ++end;
        while (end > body && (s[end - 1] == L' ' || s[end - 1] == L'.' || s[end - 1] == L',' || s[end - 1] == L')')) --end;
        if (system || end == body) { if (end > i + 1) i = end - 1; continue; }
        out.append(s, emitted, body - emitted);
        out += L"<path>";
        emitted = end;
        i = end - 1;
    }
    out.append(s, emitted, std::wstring::npos);
    return out;
}
// "github.com/<owner>/<name>" from PUSHMPC_REPO_URL (scheme dropped, so http/https and sub-paths all match)
inline std::wstring RepoKey()
{
    std::wstring u;
    for (const char* p = PUSHMPC_REPO_URL; *p; ++p) u += wchar_t(static_cast<unsigned char>(*p));
    if (const size_t k = u.find(L"://"); k != std::wstring::npos) u.erase(0, k + 3);
    while (!u.empty() && u.back() == L'/') u.pop_back();
    return u;
}
}  // namespace anon
inline std::wstring Anonymise(std::wstring s)
{
    s = anon::Videos(s);
    wchar_t b[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", b, MAX_PATH)) anon::ReplaceAllCi(s, b, L"%LOCALAPPDATA%");
    if (GetEnvironmentVariableW(L"APPDATA", b, MAX_PATH)) anon::ReplaceAllCi(s, b, L"%APPDATA%");
    if (GetEnvironmentVariableW(L"USERPROFILE", b, MAX_PATH)) anon::ReplaceAllCi(s, b, L"%USERPROFILE%");
    s = anon::OtherPaths(s);
    // round 2: the project's own repository (version.h; its owner name may contain the user name) is public and kept
    const std::wstring repoUrl = anon::RepoKey(), shield = L"\x1<pushmpc-repository>\x1";
    anon::ReplaceAllCi(s, repoUrl, shield);
    DWORD n = MAX_PATH; if (GetUserNameW(b, &n)) anon::ReplaceAllCi(s, b, L"<user>");
    n = MAX_PATH; if (GetComputerNameW(b, &n)) anon::ReplaceAllCi(s, b, L"<computer>");
    anon::ReplaceAllCi(s, shield, repoUrl);
    return s;
}
}  // namespace pm
