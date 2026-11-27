// PushMPC-Setup.exe: PushMPC's single-file installer (per user, no administrator rights).
//
// It embeds the filter and core components (the payload "package" builds) and does what
// "pushmpc-install.ps1 install" does by running that script, from the payload, hidden, and reading its progress lines
// ("-Machine"): the script stays the one implementation of the install (runtime subset and pins, NVIDIA licence rule,
// existing PushFPS / Veyra installs, PushFlow, MPC-HC registration, MPC-VR HDR settings, VapourSynth).
// What Setup adds: the wizard, closing MPC-HC, the download of Veyra's runtime package (progress, resume, cancel,
// SHA-256), the Windows Apps entry (HKCU ... Uninstall\PushMPC) and its maintenance copy <install dir>\PushMpcSetup.exe,
// which runs again as Repair / Update / Uninstall.
//
// Command line: [--uninstall | --repair | --update] [--quiet] [--install-dir <dir>] [--dry-run] [--package-cache <dir>]
//               [--no-existing] [--sandbox] [--mirror <base url>] [--mpv-build] [--no-mpv-build]
//   --sandbox: a test install that touches nothing outside the install folder (no regsvr32, no MPC-HC or MPC-VR or
//              NVIDIA settings, no logon task; Apps entry "PushMPC-Sandbox"; settings key Software\PushMPC-Sandbox).
//   --mirror:  developer tests: fetch the runtime and PushFlow packages from <base url>/<asset> (same SHA-256 checks).
#include "common/pm_common.h"
#include "version.h"
#include "resource.h"

#include <windowsx.h>
#include <commctrl.h>
#include <algorithm>
#include <shellapi.h>
#include <dxgi.h>
#include <thread>
#include <cwctype>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "version.lib")

using namespace pm;

namespace {

// ================================================================== options and state
enum class Op { Install, Repair, Update, Uninstall };
struct Options {
    bool uninstall = false, repair = false, update = false, quiet = false, dryRun = false, noExisting = false, sandbox = false;
    std::wstring installDir, packageCache, mirror;
} opt;

std::wstring g_installDir;
std::wstring AppsKey() { return opt.sandbox ? std::wstring(kUninstallKey) + L"-Sandbox" : std::wstring(kUninstallKey); }
std::wstring UpdateKey() { return opt.sandbox ? L"Software\\PushMPC-Sandbox\\Update" : kUpdateKey; }

// ------------------------------------------------------------------ the payload: "PMPAYLD1", count, {len, path, size, bytes}...
struct Payload {
    const unsigned char* data = nullptr; size_t size = 0;
    std::string release, filter, core;
    bool vsplugin = false;   // the optional mpv route component is inside
    bool vlc = false;        // the optional VLC module is inside
    bool mpvbuild = false;   // PushMPC's own mpv build is offered (filter\mpvbuild.json: its download and SHA-256)
    bool load() {
        HRSRC r = FindResourceW(nullptr, L"PAYLOAD", RT_RCDATA);
        if (!r) return false;
        HGLOBAL h = LoadResource(nullptr, r);
        size = SizeofResource(nullptr, r);
        data = static_cast<const unsigned char*>(LockResource(h));
        if (!data || size < 12 || memcmp(data, "PMPAYLD1", 8) != 0) { data = nullptr; return false; }
        bool ok = each([&](const std::string& path, const unsigned char* p, uint64_t n) {
            if (path.rfind("vsplugin\\", 0) == 0) vsplugin = true;
            if (path.rfind("vlc\\", 0) == 0) vlc = true;
            if (path == "filter\\mpvbuild.json") mpvbuild = true;
            if (path != "payload.txt") return true;
            std::string t(reinterpret_cast<const char*>(p), size_t(n));
            auto val = [&](const char* k) { size_t a = t.find(std::string(k) + "="); if (a == std::string::npos) return std::string(); a += strlen(k) + 1; return Trim(t.substr(a, t.find('\n', a) - a)); };
            release = val("release"); filter = val("filter"); core = val("core");
            return true;
        });
        return ok && !filter.empty();
    }
    template <class F> bool each(F f) const {
        size_t off = 8;
        uint32_t count; memcpy(&count, data + off, 4); off += 4;
        for (uint32_t i = 0; i < count; i++) {
            if (off + 4 > size) return false;
            uint32_t len; memcpy(&len, data + off, 4); off += 4;
            if (off + len + 8 > size) return false;
            std::string path(reinterpret_cast<const char*>(data + off), len); off += len;
            uint64_t n; memcpy(&n, data + off, 8); off += 8;
            if (off + n > size) return false;
            if (!f(path, data + off, n)) return false;
            off += size_t(n);
        }
        return true;
    }
    bool extract(const std::wstring& dir) const {
        return each([&](const std::string& path, const unsigned char* p, uint64_t n) {
            if (path.find("..") != std::string::npos || path.find(':') != std::string::npos) return false;
            std::wstring out = JoinPath(dir, Widen(path));
            return WriteFileBytes(out, std::string(reinterpret_cast<const char*>(p), size_t(n)));
        });
    }
} g_payload;

// ------------------------------------------------------------------ what is on this PC
std::wstring FileVersionOf(const std::wstring& exe) {
    DWORD h = 0, n = GetFileVersionInfoSizeW(exe.c_str(), &h);
    if (!n) return {};
    std::vector<char> buf(n);
    if (!GetFileVersionInfoW(exe.c_str(), 0, n, buf.data())) return {};
    VS_FIXEDFILEINFO* fi = nullptr; UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&fi), &len) || !fi) return {};
    return Format(L"%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS));
}
struct MpcHc { bool settings = false; std::wstring exe, version; };
MpcHc DetectMpcHc() {
    MpcHc m;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\MPC-HC\\MPC-HC", 0, KEY_READ, &k) == ERROR_SUCCESS) { m.settings = true; RegCloseKey(k); }
    std::vector<std::wstring> cands;
    std::wstring e = RegGetStr(HKEY_CURRENT_USER, L"Software\\MPC-HC\\MPC-HC", L"ExePath");
    if (!e.empty()) cands.push_back(e);
    // the MPC-HC installer's uninstall entry (InstallLocation)
    for (HKEY root : { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER }) {
        HKEY u;
        if (RegOpenKeyExW(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0, KEY_READ | KEY_WOW64_64KEY, &u) != ERROR_SUCCESS) continue;
        wchar_t name[256];
        for (DWORD i = 0; RegEnumKeyW(u, i, name, 256) == ERROR_SUCCESS; i++) {
            std::wstring sub = std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\") + name;
            std::wstring dn = RegGetStr(root, sub, L"DisplayName");
            if (dn.rfind(L"MPC-HC", 0) != 0) continue;
            std::wstring loc = RegGetStr(root, sub, L"InstallLocation");
            if (!loc.empty()) { cands.push_back(JoinPath(loc, L"mpc-hc64.exe")); cands.push_back(JoinPath(loc, L"mpc-hc.exe")); }
        }
        RegCloseKey(u);
    }
    wchar_t pf[MAX_PATH]{}; GetEnvironmentVariableW(L"ProgramW6432", pf, MAX_PATH);
    if (pf[0]) cands.push_back(JoinPath(pf, L"MPC-HC\\mpc-hc64.exe"));
    for (auto& c : cands) if (FileExists(c)) { m.exe = c; m.version = FileVersionOf(c); break; }
    return m;
}
// MPC-BE x64 (round 2): its registry ExePath (HKCU/HKLM Software\MPC-BE), an Uninstall entry named MPC-BE with
// InstallLocation, else Program Files\MPC-BE x64 or \MPC-BE (the filter's MpcBeInstalled() and the script use the same rules)
MpcHc DetectMpcBe() {
    MpcHc m;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\MPC-BE", 0, KEY_READ, &k) == ERROR_SUCCESS) { m.settings = true; RegCloseKey(k); }
    std::vector<std::wstring> cands;
    for (HKEY root : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE }) {
        std::wstring e = RegGetStr(root, L"Software\\MPC-BE", L"ExePath");
        if (!e.empty()) { cands.push_back(e); cands.push_back(JoinPath(e, L"mpc-be64.exe")); }
        for (REGSAM view : { REGSAM(KEY_WOW64_64KEY), REGSAM(KEY_WOW64_32KEY) }) {
            HKEY u;
            if (RegOpenKeyExW(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0, KEY_READ | view, &u) != ERROR_SUCCESS) continue;
            wchar_t name[256];
            for (DWORD i = 0; RegEnumKeyW(u, i, name, 256) == ERROR_SUCCESS; i++) {
                HKEY s;
                if (RegOpenKeyExW(u, name, 0, KEY_READ | view, &s) != ERROR_SUCCESS) continue;
                wchar_t dn[256]{}, loc[MAX_PATH]{}; DWORD a = sizeof(dn) - sizeof(wchar_t), b = sizeof(loc) - sizeof(wchar_t);
                const bool ok = RegQueryValueExW(s, L"DisplayName", nullptr, nullptr, reinterpret_cast<BYTE*>(dn), &a) == ERROR_SUCCESS &&
                                _wcsnicmp(dn, L"MPC-BE", 6) == 0 &&
                                RegQueryValueExW(s, L"InstallLocation", nullptr, nullptr, reinterpret_cast<BYTE*>(loc), &b) == ERROR_SUCCESS && loc[0];
                RegCloseKey(s);
                if (ok) cands.push_back(JoinPath(loc, L"mpc-be64.exe"));
            }
            RegCloseKey(u);
        }
    }
    for (const wchar_t* var : { L"ProgramW6432", L"ProgramFiles" }) {
        wchar_t pf[MAX_PATH]{};
        if (GetEnvironmentVariableW(var, pf, MAX_PATH)) { cands.push_back(JoinPath(pf, L"MPC-BE x64\\mpc-be64.exe")); cands.push_back(JoinPath(pf, L"MPC-BE\\mpc-be64.exe")); }
    }
    for (auto& c : cands) if (c.size() > 12 && _wcsicmp(c.c_str() + c.size() - 12, L"mpc-be64.exe") == 0 && FileExists(c)) { m.exe = c; m.version = FileVersionOf(c); break; }
    return m;
}
// mpv-family players (the mpv route, preview): the same places integrations\mpv\mpv-integration.ps1 looks in. SVP's
// own mpv (SVP manages it) and SMPlayer's bundled mpv are not listed as mpv.
struct MpvPlayer { std::wstring player, exe; };
std::vector<MpvPlayer> DetectMpvFamily() {
    auto env = [](const wchar_t* v) { wchar_t b[MAX_PATH]{}; GetEnvironmentVariableW(v, b, MAX_PATH); return std::wstring(b); };
    std::wstring la = env(L"LOCALAPPDATA"), pf = env(L"ProgramW6432"), pf86 = env(L"ProgramFiles(x86)"), up = env(L"USERPROFILE");
    if (pf.empty()) pf = env(L"ProgramFiles");
    std::vector<MpvPlayer> out;
    auto add = [&](const wchar_t* who, const std::wstring& exe) {
        if (exe.empty() || !FileExists(exe)) return;
        for (auto& o : out) if (!_wcsicmp(o.exe.c_str(), exe.c_str())) return;
        out.push_back({ who, exe });
    };
    auto skipDir = [&](const std::wstring& d) {
        for (auto& s : { JoinPath(pf86, L"SVP 4\\mpv64"), JoinPath(pf, L"SVP 4\\mpv64"), JoinPath(pf, L"SMPlayer\\mpv"), JoinPath(pf86, L"SMPlayer\\mpv") })
            if (!_wcsicmp(d.c_str(), s.c_str())) return true;
        return false;
    };
    std::wstring path = env(L"PATH");
    {   // PATH can be longer than MAX_PATH
        DWORD n = GetEnvironmentVariableW(L"PATH", nullptr, 0);
        if (n) { path.resize(n); path.resize(GetEnvironmentVariableW(L"PATH", path.data(), n)); }
    }
    for (size_t a = 0; a <= path.size();) {
        size_t b = path.find(L';', a); if (b == std::wstring::npos) b = path.size();
        std::wstring d = path.substr(a, b - a);
        while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
        if (!d.empty() && !skipDir(d)) add(L"mpv", JoinPath(d, L"mpv.exe"));
        a = b + 1;
    }
    for (auto& d : { JoinPath(la, L"Programs\\mpv"), JoinPath(pf, L"mpv"), JoinPath(up, L"scoop\\apps\\mpv\\current"), JoinPath(up, L"scoop\\apps\\mpv-git\\current") })
        add(L"mpv", JoinPath(d, L"mpv.exe"));
    const bool anyMpv = !out.empty();   // a plain mpv: the external player for Kodi and the shims
    for (auto& d : { JoinPath(la, L"Programs\\mpv.net"), JoinPath(pf, L"mpv.net"), JoinPath(up, L"scoop\\apps\\mpv.net\\current") }) add(L"mpv.net", JoinPath(d, L"mpvnet.exe"));
    for (auto& d : { JoinPath(pf, L"SMPlayer"), JoinPath(pf86, L"SMPlayer") }) add(L"SMPlayer", JoinPath(d, L"smplayer.exe"));
    for (auto& d : { JoinPath(pf, L"mpc-qt"), JoinPath(la, L"Programs\\mpc-qt"), JoinPath(pf86, L"mpc-qt") }) add(L"MPC-QT", JoinPath(d, L"mpc-qt.exe"));
    // external-player hosts: set up only when a plain mpv is there (mpv-integration.ps1 does the same)
    std::wstring ad = env(L"APPDATA");
    for (auto& d : { JoinPath(pf, L"Kodi"), JoinPath(pf86, L"Kodi") }) if (FileExists(JoinPath(d, L"kodi.exe"))) { out.push_back({ anyMpv ? L"Kodi" : L"Kodi (needs mpv)", JoinPath(d, L"kodi.exe") }); break; }
    if (FileExists(JoinPath(ad, L"jellyfin-mpv-shim\\conf.json"))) out.push_back({ anyMpv ? L"Jellyfin MPV Shim" : L"Jellyfin MPV Shim (needs mpv)", JoinPath(ad, L"jellyfin-mpv-shim") });
    if (FileExists(JoinPath(ad, L"plex-mpv-shim\\conf.json"))) out.push_back({ anyMpv ? L"Plex-MPV-Shim" : L"Plex-MPV-Shim (needs mpv)", JoinPath(ad, L"plex-mpv-shim") });
    if (DirExists(JoinPath(la, L"Jellyfin Desktop\\profiles"))) out.push_back({ L"Jellyfin Desktop", JoinPath(la, L"Jellyfin Desktop") });   // its libmpv has VapourSynth
    return out;
}
// The other players Setup knows: VLC, Stremio, Plex (native routes of their own) and players with no native route,
// which capture mode covers (pushmpc-install.ps1 Find-CaptureOnlyPlayers looks in the same places). capture = no
// frame generation route of PushMPC's own.
struct OtherPlayer { std::wstring player, exe; bool capture = false; std::wstring how; };
std::vector<OtherPlayer> DetectOtherPlayers(bool vlcModule) {
    auto env = [](const wchar_t* v) { wchar_t b[MAX_PATH]{}; GetEnvironmentVariableW(v, b, MAX_PATH); return std::wstring(b); };
    std::wstring la = env(L"LOCALAPPDATA"), pf = env(L"ProgramW6432"), pf86 = env(L"ProgramFiles(x86)");
    if (pf.empty()) pf = env(L"ProgramFiles");
    std::vector<OtherPlayer> out;
    auto first = [&](std::initializer_list<std::wstring> c) { for (auto& p : c) if (!p.empty() && FileExists(p)) return p; return std::wstring(); };
    const wchar_t* cap = L"capture mode (works with any player; slightly delayed, copy-protected video stays black)";
    std::wstring vlc;
    for (HKEY root : { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER }) { std::wstring d = RegGetStr(root, L"Software\\VideoLAN\\VLC", L"InstallDir"); if (!d.empty() && FileExists(JoinPath(d, L"vlc.exe"))) { vlc = JoinPath(d, L"vlc.exe"); break; } }
    if (vlc.empty()) vlc = first({ JoinPath(pf, L"VideoLAN\\VLC\\vlc.exe"), JoinPath(pf86, L"VideoLAN\\VLC\\vlc.exe") });
    if (!vlc.empty()) out.push_back({ L"VLC", vlc, !vlcModule, vlcModule ? L"the PushMPC add-on for VLC" : cap });
    std::wstring st = first({ JoinPath(la, L"Programs\\Stremio\\stremio-shell-ng.exe"), JoinPath(pf, L"Stremio\\stremio-shell-ng.exe"), JoinPath(la, L"Programs\\Stremio\\stremio.exe"), JoinPath(pf, L"Stremio\\stremio.exe") });
    if (!st.empty()) {
        // the built-in route needs Stremio's mpv config file setting: stremio-shell-ng 5.0.28 and newer (the install
        // script's rule, mpv-integration.ps1); older versions go through capture mode
        unsigned a = 0, b = 0, c = 0;
        const std::wstring v = FileVersionOf(st);
        const bool shellNg = st.size() >= 20 && _wcsicmp(st.c_str() + st.size() - 20, L"stremio-shell-ng.exe") == 0;
        const bool native = shellNg && swscanf_s(v.c_str(), L"%u.%u.%u", &a, &b, &c) == 3 && (a > 5 || (a == 5 && (b > 0 || c >= 28)));
        if (native) out.push_back({ L"Stremio", st, false, L"set up automatically" });
        else out.push_back({ L"Stremio", st, true, std::wstring(cap) + L"; Stremio 5.0.28 or newer gets the built-in route" + (v.empty() ? L"" : L" (this is " + v + L")") });
    }
    for (auto& d : { JoinPath(pf, L"Plex\\Plex"), JoinPath(pf, L"Plex\\Plex HTPC") })
        if (FileExists(JoinPath(d, L"libmpv-2.dll")) || FileExists(JoinPath(d, L"mpv-2.dll")))
            out.push_back({ d.find(L"HTPC") != std::wstring::npos ? L"Plex HTPC" : L"Plex", d, false,
                            L"smoother motion inside Plex (tested Plex versions; others: capture mode)" });
    struct P { const wchar_t* name; std::vector<const wchar_t*> rel; };
    const std::vector<P> probes = { P{ L"KMPlayer 64X", { L"KMPlayer 64X\\KMPlayer64.exe" } }, P{ L"GOM Player", { L"GRETECH\\GomPlayer\\GOM.exe", L"GRETECH\\GOMPlayer\\GOM.exe" } },
                                    P{ L"Emby Theater", { L"Emby Theater\\Emby.Theater.exe", L"Emby-Theater\\Emby.Theater.exe" } } };
    const std::wstring roots[] = { pf, pf86, JoinPath(la, L"Programs") };
    for (const P& p : probes) {
        std::wstring hit;
        for (auto* r : p.rel) { for (auto& root : roots) if (!root.empty() && FileExists(JoinPath(root, r))) { hit = JoinPath(root, r); break; } if (!hit.empty()) break; }
        if (!hit.empty()) out.push_back({ p.name, hit, true, cap });
    }
    return out;
}
// PotPlayer x64 (filter priority list in HKCU\Software\DAUM\PotPlayerMini64; the script's Find-PotPlayerExe rules)
MpcHc DetectPotPlayer() {
    MpcHc m;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\DAUM\\PotPlayerMini64", 0, KEY_READ, &k) == ERROR_SUCCESS) { m.settings = true; RegCloseKey(k); }
    std::vector<std::wstring> cands;
    for (HKEY root : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE }) {
        std::wstring e = RegGetStr(root, L"Software\\DAUM\\PotPlayer64", L"ProgramPath");
        if (!e.empty()) cands.push_back(e);
    }
    wchar_t pf[MAX_PATH]{};
    for (const wchar_t* var : { L"ProgramW6432", L"ProgramFiles" })
        if (GetEnvironmentVariableW(var, pf, MAX_PATH)) cands.push_back(JoinPath(pf, L"DAUM\\PotPlayer\\PotPlayerMini64.exe"));
    for (auto& c : cands) if (c.size() > 19 && _wcsicmp(c.c_str() + c.size() - 19, L"PotPlayerMini64.exe") == 0 && FileExists(c)) { m.exe = c; m.version = FileVersionOf(c); break; }
    return m;
}
std::wstring GpuName() {
    IDXGIFactory1* f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f)))) return {};
    std::wstring name;
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d); a->Release();
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        name = d.Description; break;
    }
    f->Release();
    return name;
}
std::string InstalledVersion() { return ReadManifest(JoinPath(g_installDir, L"filter")).get("version"); }
bool IsInstalled() {
    if (DirExists(JoinPath(g_installDir, L"filter"))) return true;
    std::wstring loc = RegGetStr(HKEY_CURRENT_USER, AppsKey(), L"InstallLocation");
    return !loc.empty() && !_wcsicmp(loc.c_str(), g_installDir.c_str());
}

// ================================================================== the operation (worker thread)
enum { WM_APP_PROGRESS = WM_APP + 1, WM_APP_LINE, WM_APP_DONE };
HWND g_wnd = nullptr;
std::atomic<bool> g_cancel{ false };
std::atomic<bool> g_cancellable{ false };
struct Result { bool ok = false, cancelled = false; std::wstring message; };

void UiProgress(int pct, const std::wstring& text) {
    if (opt.quiet) { static int last = -10; if (pct >= last + 10 || pct == 100) { Log::Write(Format(L"%3d%% ", pct) + text); last = pct; } return; }
    PostMessageW(g_wnd, WM_APP_PROGRESS, WPARAM(pct), reinterpret_cast<LPARAM>(new std::wstring(text)));
}
void UiLine(const std::wstring& text) {
    Log::Write(text);
    if (!opt.quiet) PostMessageW(g_wnd, WM_APP_LINE, 0, reinterpret_cast<LPARAM>(new std::wstring(text)));
}

// runs pushmpc-install.ps1 hidden; maps its 0..100 progress into [lo, hi]
int RunScript(const std::wstring& script, const std::wstring& args, int lo, int hi, std::map<std::string, std::string>* plan, std::wstring& failText) {
    std::wstring cmd = QuoteArg(PowerShellExe()) + L" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " + QuoteArg(script) + L" " + args +
                       L" -Machine -InstallDir " + QuoteArg(g_installDir);
    // S-ISO: a sandbox install changes nothing outside its own folder: no player entries (MPC-HC, MPC-BE, PotPlayer), no
    // mpv / VLC / Kodi routes, no Plex installer, no capture-mode Start menu entry, no registration, task or settings
    if (opt.sandbox) {
        cmd += L" -NoMpcHc -NoMpcBe -NoPotPlayer -NoMpv -NoVlc -NoKodi -Capture Off -NoRegister -NoTask -KeepSettings";
        cmd += L" -CaptureStartMenu " + QuoteArg(JoinPath(g_installDir, L"start-menu"));   // never the real Start menu
        if (args.find(L"-PlexSwap") == std::wstring::npos) cmd += L" -PlexSwap No";
        if (args.find(L"-Pmml ") == std::wstring::npos) cmd += L" -Pmml No";   // its runtime would set the real MlRuntime value
    }
    if (opt.dryRun) cmd += L" -DryRun";
    if (opt.noExisting) cmd += L" -NoExisting";
    if (!opt.packageCache.empty()) cmd += L" -PackageCache " + QuoteArg(opt.packageCache);
    Log::Write(L"run " + cmd);
    return RunHidden(cmd, [&](const std::string& raw) {
        std::wstring line = Widen(raw);
        if (raw.rfind("@@progress|", 0) == 0) {
            int p = _wtoi(line.c_str() + 11);
            size_t bar = line.find(L'|', 11);
            UiProgress(lo + (hi - lo) * std::clamp(p, 0, 100) / 100, bar == std::wstring::npos ? L"" : line.substr(bar + 1));
            return;
        }
        if (raw.rfind("@@plan|", 0) == 0 && plan) {
            std::string kv = raw.substr(7); size_t e = kv.find('=');
            if (e != std::string::npos) (*plan)[kv.substr(0, e)] = kv.substr(e + 1);
            return;
        }
        if (raw.rfind("@@fail|", 0) == 0) { failText = line.substr(7); UiLine(L"Error: " + failText); return; }
        if (raw.rfind("@@", 0) == 0) {
            size_t bar = line.find(L'|');
            std::wstring kind = line.substr(2, bar - 2), text = bar == std::wstring::npos ? L"" : line.substr(bar + 1);
            UiLine(kind == L"warn" ? L"Note: " + text : kind == L"dry" ? L"[dry run] " + text : text);
            return;
        }
        if (!Trim(raw).empty()) UiLine(line);
    });
}

bool DownloadStep(const std::wstring& what, std::wstring url, const std::wstring& file, const std::string& sha, int lo, int hi, Result& res) {
    if (!opt.mirror.empty()) url = opt.mirror + (opt.mirror.back() == L'/' ? L"" : L"/") + FileName(url);
    UiLine(L"Downloading " + what + L": " + url);
    if (opt.dryRun) { UiLine(L"[dry run] would download to " + file + (sha.empty() ? L"" : L" and check SHA-256 " + Widen(sha))); return true; }
    g_cancellable = true;
    Progress pr;
    pr.cancel = &g_cancel;
    ULONGLONG t0 = GetTickCount64(); uint64_t first = UINT64_MAX;
    pr.onBytes = [&](uint64_t d, uint64_t t) {
        if (first == UINT64_MAX) first = d;
        double secs = (GetTickCount64() - t0) / 1000.0;
        std::wstring speed = secs > 1 && d > first ? Format(L", %s/s", HumanBytes(uint64_t((d - first) / secs)).c_str()) : L"";
        int p = t ? lo + int((hi - lo) * double(d) / double(t)) : lo;
        UiProgress(p, Format(L"Downloading %s: %s of %s%s", what.c_str(), HumanBytes(d).c_str(), t ? HumanBytes(t).c_str() : L"?", speed.c_str()));
    };
    std::wstring err;
    DlResult r = pm::Download(url, file, sha, pr, err);
    g_cancellable = false;
    if (r == DlResult::Cancelled) { res.cancelled = true; res.message = L"Setup was cancelled. The download is kept: run setup again to continue where it stopped."; return false; }
    if (r != DlResult::Ok) { res.message = L"The download of " + what + L" failed: " + err + L"\r\nCheck the internet connection and run setup again; it continues where it stopped."; return false; }
    UiLine(sha.empty() ? L"Downloaded " + what : L"Downloaded " + what + L" (SHA-256 verified)");
    return true;
}

void WriteAppsEntry(const std::wstring& version) {
    std::wstring key = AppsKey(), setup = JoinPath(g_installDir, kSetupExe);
    RegSetStr(HKEY_CURRENT_USER, key, L"DisplayName", opt.sandbox ? L"PushMPC (sandbox test)" : L"PushMPC");
    RegSetStr(HKEY_CURRENT_USER, key, L"DisplayVersion", version);
    RegSetStr(HKEY_CURRENT_USER, key, L"Publisher", kPublisher);
    RegSetStr(HKEY_CURRENT_USER, key, L"DisplayIcon", setup + L",0");
    RegSetStr(HKEY_CURRENT_USER, key, L"InstallLocation", g_installDir);
    std::wstring extra = opt.sandbox ? L" --sandbox --install-dir " + QuoteArg(g_installDir) : L"";
    RegSetStr(HKEY_CURRENT_USER, key, L"UninstallString", QuoteArg(setup) + L" --uninstall" + extra);
    RegSetStr(HKEY_CURRENT_USER, key, L"QuietUninstallString", QuoteArg(setup) + L" --uninstall --quiet" + extra);
    RegSetStr(HKEY_CURRENT_USER, key, L"ModifyPath", QuoteArg(setup) + extra);
    RegSetStr(HKEY_CURRENT_USER, key, L"URLInfoAbout", kRepoUrl);
    RegSetStr(HKEY_CURRENT_USER, key, L"HelpLink", std::wstring(kRepoUrl) + L"/blob/main/docs/user-guide.md");
    RegSetStr(HKEY_CURRENT_USER, key, L"URLUpdateInfo", std::wstring(kRepoUrl) + L"/releases");
    SYSTEMTIME st; GetLocalTime(&st);
    RegSetStr(HKEY_CURRENT_USER, key, L"InstallDate", Format(L"%04u%02u%02u", st.wYear, st.wMonth, st.wDay));
    RegSetDword(HKEY_CURRENT_USER, key, L"EstimatedSize", DWORD(TreeSize(g_installDir) / 1024));
    RegSetDword(HKEY_CURRENT_USER, key, L"NoRepair", 1);
    RegSetDword(HKEY_CURRENT_USER, key, L"NoModify", 0);
    UiLine(L"Windows Apps entry written (" + key + L")");
}

std::wstring ScratchDir() { return JoinPath(TempDir(), Format(L"PushMPC-Setup-%lu", GetCurrentProcessId())); }
// AMD FSR SDK 2.4 (FSR 4.1 INT8 upscaling on Radeon RX 6000 / 7000 / 9000): offered on the welcome page, on by default
// (unlocks are on unless the user says no); unchecked = the runtime package's SDK 2.3 set (-FidelityFx Package)
std::atomic<bool> g_fsrSdk{ true };
// PushFlow's runtime for this GPU (pmml: TensorRT-RTX on RTX, OpenVINO on Intel), on by default (-Pmml No when unchecked)
std::atomic<bool> g_pmml{ true };
// Plex: frame generation inside Plex (the proxy avfilter-8.dll, or the VapourSynth-enabled libmpv when the proxy fails its
// self-test), on by default (-PlexSwap No when unchecked)
std::atomic<bool> g_plexSwap{ true };
// PushMPC's own mpv (a separate download): on by default, installed when an mpv-family player is here (-MpvBuild Auto),
// and then the libmpv of mpv.net, MPC-QT, Jellyfin Desktop, Stremio; unchecked: -MpvBuild No
std::atomic<bool> g_mpvBuild{ true };

Result DoInstall(Op op, bool autoUpdates) {
    Result res;
    const std::wstring stage = ScratchDir();
    RemoveTree(stage);
    // the components: the payload, or for a repair of a newer installed version, a copy of the installed ones
    std::string installed = InstalledVersion();
    bool fromInstalled = op == Op::Repair && !installed.empty() && (!g_payload.data || CompareVersions(installed, g_payload.filter) > 0);
    UiProgress(1, L"Preparing");
    if (fromInstalled) {
        UiLine(L"Repairing the installed version " + Widen(installed) + L" (newer than this setup's " + Widen(g_payload.filter) + L")");
        for (auto c : { L"filter", L"core", L"vsplugin" }) {
            std::wstring src = JoinPath(g_installDir, c);
            if (!wcscmp(c, L"vsplugin") && !DirExists(src)) continue;   // optional: the mpv route
            Manifest m = ReadManifest(src);
            std::wstring bad = CheckManifestFiles(src, m, true);
            if (!bad.empty()) { res.message = L"The installed " + std::wstring(c) + L" is damaged (" + bad + L"). Use Update, or uninstall and install again with the newest setup."; return res; }
            CopyTree(src, JoinPath(stage, c));
        }
    } else {
        if (!g_payload.data) { res.message = L"This setup has no PushMPC files inside (a developer build): use a release's PushMPC-Setup.exe."; return res; }
        if (!g_payload.extract(stage)) { res.message = L"The files inside setup could not be unpacked to " + stage + L"."; return res; }
    }
    const std::wstring script = JoinPath(stage, L"filter\\pushmpc-install.ps1");
    // 1. what the runtime needs
    std::map<std::string, std::string> plan; std::wstring fail;
    UiProgress(3, L"Looking for Veyra's runtime package");
    int rc = RunScript(script, L"plan -StageDir " + QuoteArg(stage), 3, 4, &plan, fail);
    if (rc != 0 || plan["runtime"].empty()) { res.message = L"Setup could not check the runtime" + (fail.empty() ? Format(L" (exit %d).", rc) : L": " + fail); RemoveTree(stage); return res; }
    UiLine(L"Graphics card: " + Widen(plan["vendor"]) + L"; runtime " + Widen(plan["runtime-version"]) + L": " +
           (plan["runtime"] == "current" ? L"already installed" : plan["runtime"] == "existing" ? L"taken from your PushFPS / Veyra installation" :
            plan["runtime"] == "package" ? L"package found on this PC" : plan["runtime"] == "download" ? L"downloaded from Veyra's GitHub release" : Widen(plan["runtime"])));
    // 2. downloads (native: progress, resume, cancel, SHA-256)
    int scriptLo = 5;
    bool downloadedRuntime = false;
    if (plan["runtime"] == "download") {
        if (!DownloadStep(L"Veyra's runtime package (" + Widen(plan["runtime-asset"]) + L")", Widen(plan["runtime-url"]), Widen(plan["runtime-file"]), plan["runtime-sha256"], 5, 62, res)) { RemoveTree(stage); return res; }
        scriptLo = 62; downloadedRuntime = true;
    }
    if (plan["pushflow"] == "download") {
        if (!DownloadStep(L"PushFPS's release for PushFlow", Widen(plan["pushflow-url"]), Widen(plan["pushflow-file"]), "", scriptLo, scriptLo + 10, res)) {
            if (res.cancelled) { RemoveTree(stage); return res; }
            UiLine(L"Note: PushFlow will be unavailable (" + res.message + L")"); res.message.clear();
        }
        scriptLo += 10;
    }
    // 3. the install itself (no cancel from here: it swaps folders)
    UiProgress(scriptLo, L"Installing");
    fail.clear();
    if (!plan["engine"].empty()) UiLine(L"Auto's starting choice for " + Widen(plan["gpu"]) + L": " + Widen(plan["engine"]));
    rc = RunScript(script, L"install -StageDir " + QuoteArg(stage) + (g_fsrSdk ? L"" : L" -FidelityFx Package") + (g_pmml ? L"" : L" -Pmml No") +
                   (g_plexSwap ? L"" : L" -PlexSwap No") + (opt.sandbox || !g_mpvBuild ? L" -MpvBuild No" : L""), scriptLo, 98, nullptr, fail);
    if (rc != 0) { res.message = L"The installation failed" + (fail.empty() ? Format(L" (exit %d).", rc) : L": " + fail) + L"\r\nNothing of an earlier PushMPC was removed. Details: " + JoinPath(g_installDir, L"logs\\setup.log"); RemoveTree(stage); return res; }
    if (!opt.dryRun) {
        // maintenance copy, Apps entry, update setting
        std::wstring self = ModulePath(), copy = JoinPath(g_installDir, kSetupExe);
        if (_wcsicmp(self.c_str(), copy.c_str()) != 0) {
            if (CopyFileW(self.c_str(), copy.c_str(), FALSE)) UiLine(L"Setup kept as " + copy + L" (Repair, Update, Uninstall)");
            else UiLine(L"Note: could not keep setup in the install folder: " + ErrorText(GetLastError()));
        }
        std::string v = InstalledVersion();
        WriteAppsEntry(Widen(v));
        // always written: the wizard's checkbox, or with --quiet the current value (absent = on). A quiet install thus
        // leaves the same Update\Enabled as the default checkbox, and a repair keeps the user's choice.
        RegSetDword(HKEY_CURRENT_USER, UpdateKey(), L"Enabled", autoUpdates ? 1 : 0);
        RegSetStr(HKEY_CURRENT_USER, UpdateKey(), L"InstalledVersion", Widen(v));
        if (downloadedRuntime) {   // the package (~750 MB) is no longer needed once the runtime is in place
            std::wstring pkg = Widen(plan["runtime-file"]);
            if (ReadManifest(JoinPath(g_installDir, L"runtime")).get("package-sha256") == plan["runtime-sha256"]) DeleteFileW(pkg.c_str());
        }
        if (plan["pushflow"] == "download" && ReadManifest(JoinPath(g_installDir, L"runtime")).get("pushflow") != "not installed")
            DeleteFileW(Widen(plan["pushflow-file"]).c_str());   // PushFlow's files are in place; PushFPS's zip (~140 MB) is not needed
        res.message = L"PushMPC " + Widen(v) + L" is installed.";
        // capture mode (pushmpc-capture.ps1 enable) was switched on for players without a native route
        if (FileExists(JoinPath(g_installDir, L"capture\\capture.json")))
            res.message += L"\r\nFor your other players, open PushMPC Capture from the Start menu. It is slightly delayed "
                           L"and copy-protected video stays black.";
    } else res.message = L"Dry run finished: nothing was changed.";
    RemoveTree(stage);
    UiProgress(100, L"Done");
    res.ok = true;
    return res;
}

Result DoUpdate() {
    Result res;
    std::wstring upd = JoinPath(g_installDir, kUpdaterExe);
    if (!FileExists(upd)) upd = JoinPath(g_installDir, L"filter\\PushMpcUpdater.exe");
    if (!FileExists(upd)) { res.message = L"The updater is missing: use Repair first."; return res; }
    std::wstring base = QuoteArg(upd) + L" --machine --install-dir " + QuoteArg(g_installDir) + (opt.sandbox ? L" --no-register --reg-key " + QuoteArg(UpdateKey()) : L"");
    if (opt.dryRun) {   // even the updater's --check records LastCheck / Status and writes its log: a dry run runs nothing
        UiLine(L"[dry run] would run " + base + L" --download --force, then --apply");
        res.ok = true; res.message = L"Dry run finished: nothing was changed.";
        UiProgress(100, L"Done");
        return res;
    }
    std::wstring status, message;
    auto onLine = [&](const std::string& raw) {
        std::wstring l = Widen(raw);
        if (raw.rfind("@@progress|", 0) == 0) { size_t b = l.find(L'|', 11); UiProgress(_wtoi(l.c_str() + 11), b == std::wstring::npos ? L"" : l.substr(b + 1)); }
        else if (raw.rfind("@@status|", 0) == 0) { size_t b = l.find(L'|', 9); status = l.substr(9, b - 9); message = b == std::wstring::npos ? L"" : l.substr(b + 1); UiLine(message); }
        else if (raw.rfind("@@info|", 0) == 0) UiLine(l.substr(7));
        else if (!Trim(raw).empty()) UiLine(l);
    };
    UiProgress(1, L"Checking for updates");
    int rc = RunHidden(base + L" --download --force", onLine);
    if (rc != 0) { res.message = message.empty() ? Format(L"The update failed (exit %d).", rc) : message; return res; }
    if (status == L"UpToDate") { res.ok = true; res.message = message; UiProgress(100, L"Up to date"); return res; }
    rc = RunHidden(base + L" --apply", onLine);
    res.ok = rc == 0;
    res.message = message;
    if (rc == 2) res.message += L"\r\nClose MPC-HC (or MPC-BE), then run Update again (or it installs at your next sign-in).";
    if (res.ok) UiProgress(100, L"Updated");
    return res;
}

Result DoUninstall() {
    Result res;
    std::wstring script = JoinPath(g_installDir, L"filter\\pushmpc-install.ps1");
    std::wstring stage;
    if (!FileExists(script) && g_payload.data) {   // the installed script is gone: use the one inside setup
        stage = ScratchDir(); RemoveTree(stage);
        g_payload.extract(stage);
        script = JoinPath(stage, L"filter\\pushmpc-install.ps1");
    }
    if (FileExists(script)) {
        std::wstring fail;
        int rc = RunScript(script, L"uninstall", 2, 90, nullptr, fail);
        if (!stage.empty()) RemoveTree(stage);
        if (rc != 0) { res.message = L"Uninstall did not finish" + (fail.empty() ? Format(L" (exit %d).", rc) : L": " + fail); return res; }
    } else UiLine(L"No installer script found: removing the setup entry only.");
    if (opt.dryRun) { res.ok = true; res.message = L"Dry run finished: nothing was changed."; return res; }
    RegDeleteTreeKey(HKEY_CURRENT_USER, AppsKey());
    UiLine(L"Windows Apps entry removed");
    // this exe may be the maintenance copy inside the install folder: delete it once it has exited
    std::wstring self = ModulePath(), copy = JoinPath(g_installDir, kSetupExe);
    if (_wcsicmp(self.c_str(), copy.c_str()) == 0) {
        std::wstring cmd = QuoteArg(SystemExe(L"cmd.exe")) + L" /d /c ping -n 4 127.0.0.1 >nul & del /f /q " + QuoteArg(copy) + L" & rmdir " + QuoteArg(g_installDir);
        STARTUPINFOW si{ sizeof(si) }; PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, TempDir().c_str(), &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    } else DeleteFileW(copy.c_str());
    UiProgress(100, L"Removed");
    res.ok = true;
    res.message = L"PushMPC was removed. MPC-HC and MPC-BE play as before; your logs are kept in " + JoinPath(g_installDir, L"logs") + L".";
    return res;
}

Result RunOp(Op op, bool autoUpdates) {
    // the uninstall too (it keeps logs\); a dry run leaves the install folder alone: its log goes to %TEMP%
    Log::Open(opt.dryRun ? JoinPath(TempDir(), L"PushMPC-Setup-dry-run.log") : JoinPath(JoinPath(g_installDir, L"logs"), L"setup.log"));
    Log::Writef(L"PushMPC setup %s: %s, install folder %s%s%s", PUSHMPC_SETUP_VERSION_W,
                op == Op::Install ? L"install" : op == Op::Repair ? L"repair" : op == Op::Update ? L"update" : L"uninstall", g_installDir.c_str(),
                opt.sandbox ? L" (sandbox)" : L"", opt.dryRun ? L" (dry run)" : L"");
    {   // round 2: which players are here (the script adds PushMPC to each one found)
        const MpcHc hc = DetectMpcHc(), be = DetectMpcBe(), pot = DetectPotPlayer();
        Log::Write(L"players: MPC-HC " + (hc.exe.empty() ? std::wstring(L"not found") : hc.version + L" (" + hc.exe + L")") +
                   L", MPC-BE " + (be.exe.empty() ? std::wstring(L"not found") : be.version + L" (" + be.exe + L")") +
                   L", PotPlayer " + (pot.exe.empty() ? std::wstring(L"not found") : pot.exe));
        std::wstring mf;
        for (auto& m : DetectMpvFamily()) mf += (mf.empty() ? L"" : L", ") + m.player + L" (" + m.exe + L")";
        Log::Write(L"mpv-family players: " + (mf.empty() ? std::wstring(L"none") : mf) + (g_payload.vsplugin ? L"" : L"; this setup has no mpv route component"));
    }
    Result r;
    try {
        switch (op) {
        case Op::Install: case Op::Repair: r = DoInstall(op, autoUpdates); break;
        case Op::Update: r = DoUpdate(); break;
        case Op::Uninstall: r = DoUninstall(); break;
        }
    } catch (...) { r.message = L"Unexpected error."; }
    Log::Write((r.ok ? L"OK: " : r.cancelled ? L"CANCELLED: " : L"FAILED: ") + r.message);
    return r;
}

// ================================================================== MPC-HC and MPC-BE must be closed
bool EnsurePlayersClosed(HWND owner) {
    if (opt.sandbox || opt.dryRun) return true;   // nothing in MPC-HC or MPC-BE is touched then
    while (!FindPlayerProcesses().empty()) {
        if (opt.quiet) { Log::Write(L"MPC-HC, MPC-BE or PotPlayer is running: closing it"); if (ClosePlayersGracefully(20000)) return true; Log::Write(L"the player did not close"); return false; }
        int a = MessageBoxW(owner, L"MPC-HC, MPC-BE or PotPlayer is running. It must be closed during setup, because it saves its settings when it closes.\r\n\r\n"
                                   L"Close it now? Anything playing stops; the player keeps your position if \"Remember file position\" is on.",
                            L"PushMPC setup", MB_OKCANCEL | MB_ICONINFORMATION);
        if (a != IDOK) return false;
        if (ClosePlayersGracefully(15000)) return true;
        if (MessageBoxW(owner, L"The player is still running (it may be asking something). Close it yourself, then click Retry.", L"PushMPC setup",
                        MB_RETRYCANCEL | MB_ICONWARNING) != IDRETRY) return false;
    }
    return true;
}

// ================================================================== the wizard
enum Page { PWelcome, PMaintenance, PLicence, PProgress, PFinish };
enum Ctl {
    IDC_TITLE = 100, IDC_SUBTITLE, IDC_BODY, IDC_WARN, IDC_AUTOUPD, IDC_LICENCE, IDC_ACCEPT, IDC_R_REPAIR, IDC_R_UPDATE, IDC_R_UNINSTALL,
    IDC_PROGRESS, IDC_STATUS, IDC_DETAILS, IDC_BACK, IDC_NEXT, IDC_CANCEL, IDC_LINK, IDC_FSRSDK, IDC_PMML, IDC_PLEX, IDC_MPVBUILD
};
HFONT g_font = nullptr, g_titleFont = nullptr, g_boldFont = nullptr;
int g_dpi = 96;
int S(int v) { return MulDiv(v, g_dpi, 96); }
Page g_page = PWelcome;
Op g_op = Op::Install;
bool g_busy = false, g_done = false, g_installed = false;
Result g_result;
MpcHc g_mpc, g_mpcBe;
std::vector<MpvPlayer> g_mpvs;
std::vector<OtherPlayer> g_others;
std::wstring g_gpu;
MpcHc g_pot;
bool NoPlayerFound() { return g_mpc.exe.empty() && g_mpcBe.exe.empty() && g_pot.exe.empty() && g_mpvs.empty() && g_others.empty(); }
// a Radeon that FSR SDK 2.4's FSR 4.1 INT8 upscaler serves (RX 6000 / 7000 / 9000; pushcore's familyOf digit rule)
// PushFlow's runtime pmml installs for this GPU and its download size (pushmpc-install.ps1 Get-PmmlEps; the sizes are
// the pinned archives': ONNX Runtime 149 MB plus the EP), or empty when PushFlow stays on DirectML
std::wstring PmmlOffer() {
    std::wstring n = g_gpu; for (auto& c : n) c = wchar_t(towupper(c));
    if (n.find(L"NVIDIA") != std::wstring::npos && n.find(L"RTX") != std::wstring::npos) return L"about 255 MB";
    if (n.find(L"NVIDIA") != std::wstring::npos && (n.find(L"GTX 16") != std::wstring::npos || n.find(L"MX450") != std::wstring::npos || n.find(L"MX550") != std::wstring::npos)) return L"about 255 MB";
    if (n.find(L"INTEL") != std::wstring::npos) return L"about 220 MB";
    return L"";
}
bool PlexFound() { for (auto& o : g_others) if (o.player.rfind(L"Plex", 0) == 0) return true; return false; }
bool FsrSdkGpu() {
    std::wstring n = g_gpu; for (auto& c : n) c = wchar_t(towupper(c));
    if (n.find(L"RADEON") == std::wstring::npos && n.find(L"AMD") == std::wstring::npos) return false;
    for (size_t i = 0; i + 2 < n.size(); ++i) {
        if (n[i] != L'R' || n[i + 1] != L'X') continue;
        size_t k = i + 2; while (k < n.size() && n[k] == L' ') ++k;
        if (k < n.size() && iswdigit(n[k])) return n[k] == L'6' || n[k] == L'7' || n[k] == L'9';
    }
    return false;
}
HBRUSH g_white = nullptr;

HWND C(int id) { return GetDlgItem(g_wnd, id); }
void SetText(int id, const std::wstring& t) { SetWindowTextW(C(id), t.c_str()); }
void Show(int id, bool on) { ShowWindow(C(id), on ? SW_SHOW : SW_HIDE); }

const wchar_t* kLicence =
    L"PushMPC is free software under the GNU General Public License, version 3 or later (GPL-3.0-or-later). You may use, "
    L"study, share and change it under that licence. It comes with no warranty. Full text: https://www.gnu.org/licenses/gpl-3.0.html\r\n"
    L"Source code: https://github.com/CptPakundo/PushMPC\r\n\r\n"
    L"Credits\r\n"
    L"- PushFPS by DosukaSOL and contributors (GPL-3.0): PushMPC's processing core is built from PushFPS's source with a small patch set.\r\n"
    L"- Veyra by Likely7 (GPL-3.0), which PushFPS is a modified version of.\r\n"
    L"- MPC Video Renderer and MPC-HC's DirectShow base classes (GPL).\r\n"
    L"- FFmpeg (LGPL / GPL), from Veyra's runtime package.\r\n"
    L"- PushFlow: ONNX Runtime and DirectML (MIT, Microsoft), RIFE models (MIT), from PushFPS's release.\r\n\r\n"
    L"NVIDIA and AMD components\r\n"
    L"PushMPC does not ship NVIDIA's files. Setup downloads Veyra's official runtime package from Veyra's GitHub release "
    L"(github.com/Likely7/Veyra-NRVideo), checks its pinned SHA-256 and installs only the parts PushMPC uses: RTX Video Super "
    L"Resolution, RTX Video HDR, NVIDIA Video Effects frame generation, the AI enhancement runtime, the CUDA runtime, FidelityFX. "
    L"NVIDIA's components are used under NVIDIA's licences, which are installed with them (runtime\\licenses\\nvidia-*). "
    L"Community-modified NVIDIA files in the package (unlocks for older RTX cards) are installed too, as PushFPS's installer does; "
    L"they are outside NVIDIA's terms, and a script install can leave them out (-ExperimentalExtras No).\r\n\r\n"
    L"By installing you accept these licences, including NVIDIA's licences for the components fetched from Veyra's release.";

void Layout();
void ShowPage(Page p) {
    g_page = p;
    for (int id = IDC_BODY; id <= IDC_DETAILS; id++) Show(id, false);
    for (int id = IDC_LINK; id <= IDC_MPVBUILD; id++) Show(id, false);   // F-SU1: the welcome page's offers too
    Show(IDC_BACK, p == PLicence || (p == PWelcome && false));
    EnableWindow(C(IDC_BACK), TRUE);
    EnableWindow(C(IDC_NEXT), TRUE);
    EnableWindow(C(IDC_CANCEL), TRUE);
    SetText(IDC_CANCEL, L"Cancel");
    const std::wstring ver = g_payload.data ? Widen(g_payload.release) : L"(developer build)";
    switch (p) {
    case PWelcome: {
        SetText(IDC_TITLE, L"Welcome to PushMPC setup");
        SetText(IDC_SUBTITLE, L"PushMPC " + ver + L" for your video players");
        std::wstring body =
            L"PushMPC makes video in your player smoother and sharper: more frames for fluid motion, AI upscaling to your screen, "
            L"cleaner low-quality video and SDR to HDR, all on your graphics card. Setup finds your players and your graphics card "
            L"and sets everything up for you.\r\n\r\n"
            L"Works with MPC-HC, MPC-BE, PotPlayer, mpv, mpv.net, SMPlayer, MPC-QT, VLC, Kodi, Stremio, Plex and Jellyfin. "
            L"Other players: capture mode.\r\n\r\n"
            L"Installs for your Windows account only (no administrator rights needed) into " + g_installDir + L". The first install "
            L"downloads about 750 MB for your graphics card, unless PushFPS is already installed.\r\n\r\n"
            L"Graphics card: " + (g_gpu.empty() ? L"not detected" : g_gpu);
        SetText(IDC_BODY, body); Show(IDC_BODY, true);
        if (NoPlayerFound()) {
            SetText(IDC_WARN, L"No supported player was found. Install one first (MPC-HC: github.com/clsid2/mpc-hc/releases, or mpv, VLC, Kodi), "
                              L"or continue and run setup again later: it sets up every player it finds then.");
        } else {
            std::wstring w;
            if (!g_mpc.exe.empty()) w += L"MPC-HC " + g_mpc.version + L" found: " + g_mpc.exe + (g_mpc.settings ? L"" : L"\r\nMPC-HC has not saved settings yet: start and close it once before setup, so PushMPC can be added to it.");
            if (!g_mpcBe.exe.empty()) w += (w.empty() ? L"" : L"\r\n") + std::wstring(L"MPC-BE ") + g_mpcBe.version + L" found: " + g_mpcBe.exe + L" (PushMPC is added to it and its video decoder is set to D3D11 copy-back)";
            if (!g_pot.exe.empty()) w += (w.empty() ? L"" : L"\r\n") + std::wstring(L"PotPlayer found: ") + g_pot.exe + L" (PushMPC is added to its filter list; choose MPC Video Renderer and a copy-back decoder in PotPlayer)";
            for (auto& m : g_mpvs) {
                std::wstring how = m.player.find(L"needs mpv") != std::wstring::npos ? L"install mpv (shinchiro build) first, then run setup again"
                                 : !g_payload.vsplugin ? L"not supported by this release yet"
                                 : L"preview: PushMPC is added through VapourSynth once its runtime is installed";
                w += (w.empty() ? L"" : L"\r\n") + m.player + L" found: " + m.exe + L" (" + how + L")";
            }
            for (auto& o : g_others) w += (w.empty() ? L"" : L"\r\n") + o.player + L" found: " + o.how;
            SetText(IDC_WARN, w);
        }
        Show(IDC_WARN, true);
        Show(IDC_AUTOUPD, true);
        Show(IDC_FSRSDK, FsrSdkGpu());
        if (!PmmlOffer().empty()) SetText(IDC_PMML, L"Download faster AI frame generation for this graphics card (" + PmmlOffer() + L")");
        Show(IDC_PMML, !PmmlOffer().empty());
        Show(IDC_PLEX, PlexFound());
        Show(IDC_MPVBUILD, g_payload.mpvbuild);
        SetText(IDC_NEXT, L"Next");
        break;
    }
    case PMaintenance: {
        std::string iv = InstalledVersion();
        SetText(IDC_TITLE, L"PushMPC is installed");
        SetText(IDC_SUBTITLE, L"Version " + (iv.empty() ? std::wstring(L"unknown") : Widen(iv)) + L" in " + g_installDir);
        SetText(IDC_BODY, L"What would you like to do?"); Show(IDC_BODY, true);
        bool newer = g_payload.data && (iv.empty() || CompareVersions(g_payload.filter, iv) > 0);
        SetText(IDC_R_REPAIR, newer ? L"Install version " + ver + L" from this setup" : L"Repair: check and reinstall the files, the registration and the player entries");
        SetText(IDC_R_UPDATE, L"Update: download and install the newest release");
        SetText(IDC_R_UNINSTALL, L"Uninstall: remove PushMPC and put the players' settings back");
        for (int id : { IDC_R_REPAIR, IDC_R_UPDATE, IDC_R_UNINSTALL }) Show(id, true);
        Show(IDC_AUTOUPD, true);
        SetText(IDC_NEXT, L"Next");
        break;
    }
    case PLicence:
        SetText(IDC_TITLE, L"Licence and credits");
        SetText(IDC_SUBTITLE, L"Please read the terms before installing");
        Show(IDC_LICENCE, true); Show(IDC_ACCEPT, true);
        SetText(IDC_NEXT, L"Install");
        EnableWindow(C(IDC_NEXT), Button_GetCheck(C(IDC_ACCEPT)) == BST_CHECKED);
        break;
    case PProgress:
        SetText(IDC_TITLE, g_op == Op::Uninstall ? L"Removing PushMPC" : g_op == Op::Update ? L"Updating PushMPC" : L"Installing PushMPC");
        SetText(IDC_SUBTITLE, L"This takes a few minutes when the runtime is downloaded");
        Show(IDC_PROGRESS, true); Show(IDC_STATUS, true); Show(IDC_DETAILS, true);
        SetText(IDC_NEXT, L"Next"); EnableWindow(C(IDC_NEXT), FALSE);
        EnableWindow(C(IDC_CANCEL), FALSE);
        break;
    case PFinish:
        SetText(IDC_TITLE, g_result.ok ? (g_op == Op::Uninstall ? L"PushMPC was removed" : L"PushMPC is ready") : g_result.cancelled ? L"Setup was cancelled" : L"Setup did not finish");
        SetText(IDC_SUBTITLE, g_result.ok ? L"" : L"Details are in the log");
        {
            std::wstring body = g_result.message;
            if (g_result.ok && g_op != Op::Uninstall)
                body += L"\r\n\r\nOpen a video in your player: PushMPC starts by itself, with Auto for everything. In MPC-HC and MPC-BE its settings "
                        L"are under Play > Filters > PushMPC Filter.\r\n\r\nUpdates, repair and uninstall: Windows Settings > Apps > Installed apps > PushMPC.";
            SetText(IDC_BODY, body);
        }
        Show(IDC_BODY, true); Show(IDC_DETAILS, true); Show(IDC_LINK, true);
        SetText(IDC_NEXT, L"Finish"); EnableWindow(C(IDC_NEXT), TRUE);
        Show(IDC_CANCEL, false);
        break;
    }
    Layout();
}

// the height a static's text takes when wrapped at width w (its own font)
int TextHeight(int id, int w) {
    HWND h = C(id);
    std::wstring t(size_t(GetWindowTextLengthW(h)) + 1, L' ');
    t.resize(size_t(GetWindowTextW(h, t.data(), int(t.size()))));
    HDC dc = GetDC(h);
    HGDIOBJ old = SelectObject(dc, reinterpret_cast<HGDIOBJ>(SendMessageW(h, WM_GETFONT, 0, 0)));
    RECT r = { 0, 0, w, 0 };
    DrawTextW(dc, t.c_str(), int(t.size()), &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
    SelectObject(dc, old);
    ReleaseDC(h, dc);
    return r.bottom + S(4);
}

void Layout() {
    RECT rc; GetClientRect(g_wnd, &rc);
    int W = rc.right, H = rc.bottom, m = S(24), top = S(84);
    MoveWindow(C(IDC_TITLE), m, S(14), W - 2 * m, S(28), TRUE);
    MoveWindow(C(IDC_SUBTITLE), m, S(44), W - 2 * m, S(20), TRUE);
    int by = H - S(44);
    MoveWindow(C(IDC_BACK), W - S(300), by, S(88), S(28), TRUE);
    MoveWindow(C(IDC_NEXT), W - S(204), by, S(88), S(28), TRUE);
    MoveWindow(C(IDC_CANCEL), W - S(108), by, S(88), S(28), TRUE);
    MoveWindow(C(IDC_LINK), m, by + S(6), S(220), S(20), TRUE);
    int ch = by - S(16) - top;
    switch (g_page) {
    case PWelcome: {
        // F-SU2: laid out from the text's own height at this DPI (the body was a fixed S(178) and lost its last line at
        // 200 %), then the shown offers one under another (hidden ones leave no gap); the player list gets the rest
        const int w = W - 2 * m;
        std::vector<int> boxes;
        for (int id : { IDC_AUTOUPD, IDC_FSRSDK, IDC_PMML, IDC_PLEX, IDC_MPVBUILD }) if (GetWindowLongW(C(id), GWL_STYLE) & WS_VISIBLE) boxes.push_back(id);
        const int boxesH = int(boxes.size()) * S(22);
        int y = top;
        const int bodyH = TextHeight(IDC_BODY, w);
        MoveWindow(C(IDC_BODY), m, y, w, bodyH, TRUE);
        y += bodyH + S(8);
        const int warnH = std::max(S(20), std::min(TextHeight(IDC_WARN, w), ch - (y - top) - boxesH - S(8)));
        MoveWindow(C(IDC_WARN), m, y, w, warnH, TRUE);
        y += warnH + S(8);
        for (int id : boxes) { MoveWindow(C(id), m, y, w, S(22), TRUE); y += S(22); }
        break;
    }
    case PMaintenance:
        MoveWindow(C(IDC_BODY), m, top, W - 2 * m, S(22), TRUE);
        MoveWindow(C(IDC_R_REPAIR), m + S(8), top + S(34), W - 2 * m, S(22), TRUE);
        MoveWindow(C(IDC_R_UPDATE), m + S(8), top + S(62), W - 2 * m, S(22), TRUE);
        MoveWindow(C(IDC_R_UNINSTALL), m + S(8), top + S(90), W - 2 * m, S(22), TRUE);
        MoveWindow(C(IDC_AUTOUPD), m, top + S(136), W - 2 * m, S(22), TRUE);
        break;
    case PLicence:
        MoveWindow(C(IDC_LICENCE), m, top, W - 2 * m, ch - S(30), TRUE);
        MoveWindow(C(IDC_ACCEPT), m, top + ch - S(24), W - 2 * m, S(22), TRUE);
        break;
    case PProgress:
        MoveWindow(C(IDC_STATUS), m, top, W - 2 * m, S(20), TRUE);
        MoveWindow(C(IDC_PROGRESS), m, top + S(24), W - 2 * m, S(18), TRUE);
        MoveWindow(C(IDC_DETAILS), m, top + S(52), W - 2 * m, ch - S(52), TRUE);
        break;
    case PFinish:
        MoveWindow(C(IDC_BODY), m, top, W - 2 * m, S(120), TRUE);
        MoveWindow(C(IDC_DETAILS), m, top + S(126), W - 2 * m, ch - S(126), TRUE);
        break;
    }
    InvalidateRect(g_wnd, nullptr, TRUE);
}

void StartOp() {
    if (!EnsurePlayersClosed(g_wnd)) return;
    g_busy = true;
    g_cancel = false;
    SendMessageW(C(IDC_DETAILS), LB_RESETCONTENT, 0, 0);
    ShowPage(PProgress);
    bool autoUpd = Button_GetCheck(C(IDC_AUTOUPD)) == BST_CHECKED;
    Op op = g_op;
    std::thread([op, autoUpd]() {
        Result* r = new Result(RunOp(op, autoUpd));
        PostMessageW(g_wnd, WM_APP_DONE, 0, reinterpret_cast<LPARAM>(r));
    }).detach();
}

void OnNext() {
    switch (g_page) {
    case PWelcome: g_op = Op::Install; ShowPage(PLicence); break;
    case PMaintenance: {
        if (Button_GetCheck(C(IDC_R_UNINSTALL)) == BST_CHECKED) {
            if (MessageBoxW(g_wnd, L"Remove PushMPC? Every player setting PushMPC changed is put back.", L"PushMPC setup", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
            g_op = Op::Uninstall;
        } else if (Button_GetCheck(C(IDC_R_UPDATE)) == BST_CHECKED) g_op = Op::Update;
        else g_op = Op::Repair;
        // the maintenance page's update checkbox; a dry run writes nothing (it left Update\Enabled=1 behind, U-1)
        if ((g_op == Op::Repair || g_op == Op::Update) && !opt.dryRun)
            RegSetDword(HKEY_CURRENT_USER, UpdateKey(), L"Enabled", Button_GetCheck(C(IDC_AUTOUPD)) == BST_CHECKED ? 1 : 0);
        StartOp();
        break;
    }
    case PLicence: StartOp(); break;
    case PFinish: DestroyWindow(g_wnd); break;
    default: break;
    }
}

LRESULT CALLBACK WndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_wnd = w;
        auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
            HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | style, 0, 0, 10, 10, w, reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
            SendMessageW(h, WM_SETFONT, WPARAM(g_font), TRUE);
            return h;
        };
        SendMessageW(mk(L"STATIC", L"", WS_VISIBLE, IDC_TITLE), WM_SETFONT, WPARAM(g_titleFont), TRUE);
        mk(L"STATIC", L"", WS_VISIBLE | SS_PATHELLIPSIS | SS_NOPREFIX, IDC_SUBTITLE);
        mk(L"STATIC", L"", 0, IDC_BODY);
        mk(L"STATIC", L"", 0, IDC_WARN);
        HWND au = mk(L"BUTTON", L"Check for updates automatically (at most once a day, in the background)", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_AUTOUPD);
        Button_SetCheck(au, RegGetDword(HKEY_CURRENT_USER, UpdateKey(), L"Enabled", 1) ? BST_CHECKED : BST_UNCHECKED);
        HWND lic = mk(L"EDIT", kLicence, ES_MULTILINE | ES_READONLY | WS_VSCROLL | WS_TABSTOP, IDC_LICENCE, WS_EX_CLIENTEDGE);
        (void)lic;
        mk(L"BUTTON", L"I accept the licence terms", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_ACCEPT);
        Button_SetCheck(mk(L"BUTTON", L"Download AMD's newest FSR upscaling (FSR 4.1) for this Radeon", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_FSRSDK), BST_CHECKED);
        Button_SetCheck(mk(L"BUTTON", L"Download faster AI frame generation for this graphics card", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_PMML), BST_CHECKED);
        Button_SetCheck(mk(L"BUTTON", L"Give Plex smoother motion (changes Plex's folder, undone on uninstall; asks for administrator permission)", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_PLEX), BST_CHECKED);
        Button_SetCheck(mk(L"BUTTON", L"PushMPC's own mpv, also for mpv.net, MPC-QT, Jellyfin Desktop and Stremio (a separate download when one of them is here)", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_MPVBUILD), BST_CHECKED);
        Button_SetCheck(mk(L"BUTTON", L"", BS_AUTORADIOBUTTON | WS_TABSTOP | WS_GROUP, IDC_R_REPAIR), BST_CHECKED);
        mk(L"BUTTON", L"", BS_AUTORADIOBUTTON, IDC_R_UPDATE);
        mk(L"BUTTON", L"", BS_AUTORADIOBUTTON, IDC_R_UNINSTALL);
        mk(PROGRESS_CLASSW, L"", PBS_SMOOTH, IDC_PROGRESS);
        SendMessageW(C(IDC_PROGRESS), PBM_SETRANGE32, 0, 100);
        mk(L"STATIC", L"", SS_ENDELLIPSIS, IDC_STATUS);
        mk(L"LISTBOX", L"", WS_VSCROLL | WS_HSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOSEL | WS_TABSTOP, IDC_DETAILS, WS_EX_CLIENTEDGE);
        SendMessageW(C(IDC_DETAILS), LB_SETHORIZONTALEXTENT, S(2000), 0);
        mk(L"BUTTON", L"Back", WS_VISIBLE | WS_TABSTOP, IDC_BACK);
        mk(L"BUTTON", L"Next", WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, IDC_NEXT);
        mk(L"BUTTON", L"Cancel", WS_VISIBLE | WS_TABSTOP, IDC_CANCEL);
        mk(WC_LINK, L"<a href=\"https://github.com/CptPakundo/PushMPC\">PushMPC on GitHub</a>", WS_TABSTOP, IDC_LINK);
        ShowPage(g_installed && !opt.uninstall ? PMaintenance : PWelcome);
        if (opt.uninstall) { g_op = Op::Uninstall; PostMessageW(w, WM_APP_DONE + 1, 0, 0); }
        return 0;
    }
    case WM_APP_DONE + 1:   // --uninstall from Windows Apps: confirm, then go
        if (MessageBoxW(w, L"Remove PushMPC? Every player setting PushMPC changed is put back.", L"PushMPC setup", MB_OKCANCEL | MB_ICONQUESTION) == IDOK) StartOp();
        else DestroyWindow(w);
        return 0;
    case WM_SIZE: Layout(); return 0;
    case WM_CTLCOLORSTATIC: {
        HWND c = reinterpret_cast<HWND>(lp);
        int id = GetDlgCtrlID(c);
        HDC dc = reinterpret_cast<HDC>(wp);
        SetBkMode(dc, TRANSPARENT);
        if (id == IDC_WARN && NoPlayerFound()) SetTextColor(dc, RGB(160, 60, 0));
        if (id == IDC_TITLE || id == IDC_SUBTITLE) return reinterpret_cast<LRESULT>(g_white);
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
    }
    case WM_ERASEBKGND: {
        HDC dc = reinterpret_cast<HDC>(wp);
        RECT rc; GetClientRect(w, &rc);
        FillRect(dc, &rc, GetSysColorBrush(COLOR_BTNFACE));
        RECT hdr = rc; hdr.bottom = S(72);
        FillRect(dc, &hdr, g_white);
        RECT line = rc; line.top = S(72); line.bottom = line.top + 1;
        FillRect(dc, &line, GetSysColorBrush(COLOR_3DSHADOW));
        RECT bl = rc; bl.top = rc.bottom - S(56); bl.bottom = bl.top + 1;
        FillRect(dc, &bl, GetSysColorBrush(COLOR_3DLIGHT));
        return 1;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_NEXT: OnNext(); return 0;
        case IDC_BACK: if (g_page == PLicence) ShowPage(g_installed ? PMaintenance : PWelcome); return 0;
        case IDC_ACCEPT: if (g_page == PLicence) EnableWindow(C(IDC_NEXT), Button_GetCheck(C(IDC_ACCEPT)) == BST_CHECKED); return 0;
        case IDC_FSRSDK: g_fsrSdk = Button_GetCheck(C(IDC_FSRSDK)) == BST_CHECKED; return 0;
        case IDC_PMML: g_pmml = Button_GetCheck(C(IDC_PMML)) == BST_CHECKED; return 0;
        case IDC_PLEX: g_plexSwap = Button_GetCheck(C(IDC_PLEX)) == BST_CHECKED; return 0;
        case IDC_MPVBUILD: g_mpvBuild = Button_GetCheck(C(IDC_MPVBUILD)) == BST_CHECKED; return 0;
        case IDCANCEL:
        case IDC_CANCEL:
            if (g_busy) {
                if (g_cancellable && MessageBoxW(w, L"Stop the download? It continues where it stopped when you run setup again.", L"PushMPC setup", MB_YESNO | MB_ICONQUESTION) == IDYES) g_cancel = true;
                return 0;
            }
            if (g_page == PFinish || MessageBoxW(w, L"Quit PushMPC setup?", L"PushMPC setup", MB_YESNO | MB_ICONQUESTION) == IDYES) DestroyWindow(w);
            return 0;
        }
        break;
    case WM_NOTIFY: {
        auto* n = reinterpret_cast<NMHDR*>(lp);
        if (n->idFrom == IDC_LINK && (n->code == NM_CLICK || n->code == NM_RETURN)) ShellExecuteW(w, L"open", kRepoUrl, nullptr, nullptr, SW_SHOWNORMAL);
        break;
    }
    case WM_APP_PROGRESS: {
        auto* t = reinterpret_cast<std::wstring*>(lp);
        SendMessageW(C(IDC_PROGRESS), PBM_SETPOS, wp, 0);
        if (!t->empty()) SetText(IDC_STATUS, *t);
        delete t;
        EnableWindow(C(IDC_CANCEL), g_cancellable.load());
        return 0;
    }
    case WM_APP_LINE: {
        auto* t = reinterpret_cast<std::wstring*>(lp);
        LRESULT i = SendMessageW(C(IDC_DETAILS), LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t->c_str()));
        SendMessageW(C(IDC_DETAILS), LB_SETTOPINDEX, WPARAM(i), 0);
        delete t;
        EnableWindow(C(IDC_CANCEL), g_cancellable.load());
        return 0;
    }
    case WM_APP_DONE: {
        auto* r = reinterpret_cast<Result*>(lp);
        g_result = *r; delete r;
        g_busy = false; g_done = true;
        ShowPage(PFinish);
        return 0;
    }
    case WM_CLOSE:
        if (g_busy) { SendMessageW(w, WM_COMMAND, IDC_CANCEL, 0); return 0; }
        if (g_page != PFinish && MessageBoxW(w, L"Quit PushMPC setup?", L"PushMPC setup", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
        DestroyWindow(w);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

bool ParseArgs() {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        auto next = [&](std::wstring& v) { if (i + 1 < argc) v = argv[++i]; };
        if (a == L"--uninstall" || a == L"/uninstall") opt.uninstall = true;
        else if (a == L"--repair") opt.repair = true;
        else if (a == L"--update") opt.update = true;
        else if (a == L"--quiet" || a == L"/S" || a == L"/quiet") opt.quiet = true;
        else if (a == L"--dry-run") opt.dryRun = true;
        else if (a == L"--sandbox") opt.sandbox = true;
        else if (a == L"--no-existing") opt.noExisting = true;
        else if (a == L"--mpv-build") g_mpvBuild = true;   // scripted installs: PushMPC's own mpv too
        else if (a == L"--no-mpv-build") g_mpvBuild = false;
        else if (a == L"--install-dir") next(opt.installDir);
        else if (a == L"--package-cache") next(opt.packageCache);
        else if (a == L"--mirror") next(opt.mirror);
    }
    LocalFree(argv);
    return true;
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    ParseArgs();
    SetUserAgent(L"PushMPC-Setup/" PUSHMPC_SETUP_VERSION_W);
    g_installDir = opt.installDir.empty() ? JoinPath(KnownLocalAppData(), L"PushMPC") : opt.installDir;
    if (!opt.installDir.empty()) { wchar_t full[MAX_PATH * 4]; if (GetFullPathNameW(g_installDir.c_str(), MAX_PATH * 4, full, nullptr)) g_installDir = full; }
    g_payload.load();
    g_installed = IsInstalled();

    if (opt.quiet) {
        // no window: install (or repair), update or uninstall; output to the parent console and the log
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        if (!out || out == INVALID_HANDLE_VALUE || GetFileType(out) == FILE_TYPE_UNKNOWN) {
            out = nullptr;
            if (AttachConsole(ATTACH_PARENT_PROCESS)) out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        }
        Log::SetEcho([out](const std::wstring& l) {
            if (!out || out == INVALID_HANDLE_VALUE) return;
            std::wstring s = l + L"\r\n"; DWORD n = 0, mode = 0;
            if (GetConsoleMode(out, &mode)) WriteConsoleW(out, s.c_str(), DWORD(s.size()), &n, nullptr);
            else { std::string u = Narrow(s); WriteFile(out, u.data(), DWORD(u.size()), &n, nullptr); }
        });
        Op op = opt.uninstall ? Op::Uninstall : opt.update ? Op::Update : (g_installed ? Op::Repair : Op::Install);
        if (!EnsurePlayersClosed(nullptr)) { Log::Write(L"MPC-HC, MPC-BE or PotPlayer is running: close it and run setup again."); return 5; }
        Result r = RunOp(op, RegGetDword(HKEY_CURRENT_USER, UpdateKey(), L"Enabled", 1) != 0);
        return r.ok ? 0 : r.cancelled ? 1223 : 1;
    }

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_PROGRESS_CLASS | ICC_LINK_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    HDC sdc = GetDC(nullptr); g_dpi = GetDeviceCaps(sdc, LOGPIXELSY); ReleaseDC(nullptr, sdc);
    NONCLIENTMETRICSW ncm{ sizeof(ncm) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW lf = ncm.lfMessageFont; lf.lfHeight = -MulDiv(15, g_dpi, 72); lf.lfWeight = FW_SEMIBOLD;
    g_titleFont = CreateFontIndirectW(&lf);
    g_white = CreateSolidBrush(RGB(255, 255, 255));
    g_mpc = DetectMpcHc();
    g_mpcBe = DetectMpcBe();
    g_pot = DetectPotPlayer();
    g_mpvs = DetectMpvFamily();
    g_others = DetectOtherPlayers(g_payload.vlc);
    g_gpu = GpuName();

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc; wc.hInstance = inst; wc.lpszClassName = L"PushMpcSetup";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    RegisterClassExW(&wc);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT r{ 0, 0, S(620), S(510) };
    AdjustWindowRect(&r, style, FALSE);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    std::wstring title = opt.sandbox ? L"PushMPC setup (sandbox test)" : L"PushMPC setup";
    HWND w = CreateWindowExW(0, wc.lpszClassName, title.c_str(), style, (GetSystemMetrics(SM_CXSCREEN) - ww) / 2,
                             (GetSystemMetrics(SM_CYSCREEN) - wh) / 2, ww, wh, nullptr, nullptr, inst, nullptr);
    ShowWindow(w, show);
    UpdateWindow(w);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(w, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return g_result.ok || !g_done ? 0 : 1;
}
