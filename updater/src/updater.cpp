// PushMpcUpdater.exe: PushMPC's auto-updater (per user, no administrator rights).
//
//   --check      fetch the release feed, verify its signature, compare the component versions, write the status
//   --download   --check, then stage the changed components in staging\ (SHA-256 checked, resumable)
//   --apply      swap the staged components in when no process has them loaded (else they stay pending);
//                previous\ keeps the old ones; the new filter is registered and self-tested, a failure rolls back
//   --auto       check, download and apply (else pending); at most once per 24 h; low priority; quiet
//   --status     print the update status and the installed versions
//   --register-task / --unregister-task   the per-user logon task "\PushMPC\Apply pending update" (runs --apply)
//
// Status for the filter's About tab: HKCU\Software\PushMPC\Update
//   LastCheck (UTC ISO 8601), Status (UpToDate | Available | Downloaded | Error), AvailableVersion, Message,
//   plus LatestVersion, InstalledVersion, LastApply, FailedVersion.
// Settings (same key, all optional): Enabled (DWORD, 1), Channel (Stable | Beta), FeedUrl, BetaFeedUrl,
//   AllowUnsigned (DWORD; default 0 in release builds, 1 in developer builds).
// Log: <install dir>\logs\updater.log
#include "common/pm_common.h"
#include "version.h"

#include <objbase.h>
#include <shellapi.h>
#include <taskschd.h>
#include <comdef.h>
#include <lmcons.h>
#define SECURITY_WIN32
#include <security.h>
#include <algorithm>
#include <cstdio>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "secur32.lib")

using namespace pm;

namespace {

// ------------------------------------------------------------------ options and output
struct Options {
    std::wstring command;
    std::wstring installDir, regKey = kUpdateKey, feed, channel, releasesApi = kRepoApiReleases;
    std::wstring arg1, arg2;
    bool machine = false, noRegister = false, allowUnsigned = false, force = false, fromTask = false, testLoopback = false, quiet = false;
} g;

HANDLE g_out = nullptr;
bool g_outIsConsole = false;
void InitOutput() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN) { g_out = h; }
    else if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        g_out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (g_out == INVALID_HANDLE_VALUE) g_out = nullptr;
    }
    DWORD mode = 0;
    g_outIsConsole = g_out && GetConsoleMode(g_out, &mode);
}
void Out(const std::wstring& s) {
    if (!g_out) return;
    std::wstring line = s + L"\r\n";
    if (g_outIsConsole) { DWORD n = 0; WriteConsoleW(g_out, line.c_str(), DWORD(line.size()), &n, nullptr); return; }
    std::string u = Narrow(line); DWORD n = 0; WriteFile(g_out, u.data(), DWORD(u.size()), &n, nullptr);
}
// machine-readable progress for PushMPC-Setup.exe (--machine)
void Prog(int pct, const std::wstring& text) { if (g.machine) Out(Format(L"@@progress|%d|", pct) + text); }
void Say(const std::wstring& s) { Log::Write(s); if (!g.machine && !g.quiet) Out(s); else if (g.machine) Out(L"@@info|" + s); }

// ------------------------------------------------------------------ status in the registry
void SetStatus(const wchar_t* status, const std::wstring& message, const std::wstring* available = nullptr) {
    if (status) RegSetStr(HKEY_CURRENT_USER, g.regKey, L"Status", status);
    RegSetStr(HKEY_CURRENT_USER, g.regKey, L"Message", message);
    if (available) RegSetStr(HKEY_CURRENT_USER, g.regKey, L"AvailableVersion", *available);
    Log::Writef(L"status %s: %s", status ? status : L"(unchanged)", message.c_str());
    if (g.machine) Out(Format(L"@@status|%s|", status ? status : L"") + message);
}
std::wstring Setting(const wchar_t* name) { return RegGetStr(HKEY_CURRENT_USER, g.regKey, name); }

// ------------------------------------------------------------------ paths
std::wstring InstallDir() {
    if (!g.installDir.empty()) return g.installDir;
    std::wstring here = ParentDir(ModulePath());
    if (FileExists(JoinPath(here, L"filter\\manifest.txt"))) return here;
    std::wstring up = ParentDir(here);
    if (FileExists(JoinPath(here, L"manifest.txt")) && FileExists(JoinPath(up, L"filter\\manifest.txt"))) return up;   // run from the filter folder
    return JoinPath(KnownLocalAppData(), L"PushMPC");
}
std::wstring Dir(const wchar_t* sub) { return JoinPath(InstallDir(), sub); }

// ------------------------------------------------------------------ the release key
std::string ReleaseKey() {
    HRSRC r = FindResourceW(nullptr, L"RELEASEKEY", RT_RCDATA);
    if (r) {
        HGLOBAL h = LoadResource(nullptr, r);
        DWORD n = SizeofResource(nullptr, r);
        if (h && n > 32) return std::string(static_cast<const char*>(LockResource(h)), n);
    }
    for (auto p : { JoinPath(ParentDir(ModulePath()), L"release-key.xml"), JoinPath(Dir(L"filter"), L"release-key.xml") }) {
        std::string s; if (ReadFileBytes(p, s) && s.find("<Modulus>") != std::string::npos) return s;
    }
    return {};
}
bool AllowUnsigned() {
    if (g.allowUnsigned) return true;
    return RegGetDword(HKEY_CURRENT_USER, g.regKey, L"AllowUnsigned", PUSHMPC_UPDATER_DEV ? 1 : 0) != 0;
}

// ------------------------------------------------------------------ the feed
struct Feed { std::wstring url, sigUrl; };
bool ResolveFeed(Feed& f, std::wstring& err) {
    if (!g.feed.empty()) { f.url = g.feed; }
    else if (!Setting(L"FeedUrl").empty()) { f.url = Setting(L"FeedUrl"); }
    else {
        std::wstring ch = g.channel.empty() ? Setting(L"Channel") : g.channel;
        if (_wcsicmp(ch.c_str(), L"Beta") == 0) {
            if (!Setting(L"BetaFeedUrl").empty()) f.url = Setting(L"BetaFeedUrl");
            else {
                // the newest release, pre-release or not, that carries a release.json (GitHub lists newest first)
                std::string body;
                JValue arr; std::string jerr;
                if (!Fetch(g.releasesApi, body, err) || !JsonParse(body, arr, jerr) || arr.type != JValue::Array) {
                    // GitHub's API limits anonymous calls (60 an hour per address): the stable feed still works
                    Log::Write(L"beta channel: the release list is not available (" + (err.empty() ? Widen(jerr) : err) + L"); using the stable feed");
                    err.clear();
                    f.url = kDefaultFeed;
                    f.sigUrl = f.url + L".sig";
                    return true;
                }
                for (auto& r : arr.a) {
                    if (r.boolean("draft")) continue;
                    const JValue* assets = r.get("assets");
                    if (!assets || assets->type != JValue::Array) continue;
                    std::wstring json, sig;
                    for (auto& a : assets->a) {
                        if (a.str("name") == "release.json") json = Widen(a.str("browser_download_url"));
                        if (a.str("name") == "release.json.sig") sig = Widen(a.str("browser_download_url"));
                    }
                    if (!json.empty()) { f.url = json; f.sigUrl = sig; break; }
                }
                if (f.url.empty()) { err = L"no release with a release.json found"; return false; }
            }
        } else f.url = kDefaultFeed;
    }
    if (f.sigUrl.empty()) f.sigUrl = f.url + L".sig";
    if (!IsHttpUrl(f.url) && !IsFileUrl(f.url) && DirExists(f.url)) { f.url = JoinPath(f.url, L"release.json"); f.sigUrl = f.url + L".sig"; }
    return true;
}

// ------------------------------------------------------------------ the installed state
struct Installed { Manifest filter, core, runtime; std::string vendor; };
Installed ReadInstalled() {
    Installed i;
    i.filter = ReadManifest(Dir(L"filter"));
    i.core = ReadManifest(Dir(L"core"));
    i.runtime = ReadManifest(Dir(L"runtime"));
    i.vendor = i.runtime.get("vendor");
    if (i.vendor.empty()) i.vendor = "NVIDIA";
    return i;
}

struct Plan {
    Feed feed;
    std::string releaseBytes, sigBytes;
    Release rel;
    std::vector<ReleaseComponent> changes;   // in apply order: runtime, core, filter
    bool upToDate = true;
};

bool ReleaseRuntimeUrlOk(const std::wstring& u) {
    if (StartsWithI(u, L"https://github.com/")) {
        size_t p = u.find(L"/releases/download/");
        return p != std::wstring::npos && std::count(u.begin(), u.begin() + p, L'/') == 4;   // https://github.com/<owner>/<repo>
    }
    if (g.testLoopback) return StartsWithI(u, L"http://127.0.0.1:") || StartsWithI(u, L"http://localhost:") || IsFileUrl(u);
    return false;
}

// fetch + verify + compare; writes LastCheck and the status. false = error (status Error written)
bool Check(Plan& plan) {
    std::wstring err;
    RegSetStr(HKEY_CURRENT_USER, g.regKey, L"LastCheck", UtcNowIso());
    Prog(2, L"Checking for updates");
    if (!ResolveFeed(plan.feed, err)) { SetStatus(L"Error", L"Update check failed: " + err); return false; }
    Log::Write(L"feed " + plan.feed.url);
    if (!Fetch(plan.feed.url, plan.releaseBytes, err)) { SetStatus(L"Error", L"Update check failed: " + err); return false; }
    // signature
    std::string key = ReleaseKey();
    std::wstring sigErr;
    bool haveSig = Fetch(plan.feed.sigUrl, plan.sigBytes, sigErr, 64 << 10) && !Trim(plan.sigBytes).empty();
    if (!key.empty() && haveSig) {
        std::wstring verr;
        if (!VerifySignature(plan.releaseBytes, Trim(plan.sigBytes), key, verr)) { SetStatus(L"Error", L"The update was refused: " + verr + L"."); return false; }
        Log::Write(L"release manifest signature verified");
    } else if (!AllowUnsigned()) {
        SetStatus(L"Error", key.empty() ? L"The update was refused: this build has no release key and unsigned updates are off."
                                        : L"The update was refused: the release manifest is not signed (" + sigErr + L").");
        return false;
    } else Log::Write(key.empty() ? L"no release key: unsigned manifest accepted (AllowUnsigned)" : L"manifest not signed: accepted (AllowUnsigned)");
    std::string perr;
    if (!ParseRelease(plan.releaseBytes, plan.rel, perr)) { SetStatus(L"Error", L"Update check failed: the release manifest is not valid (" + Widen(perr) + L")."); return false; }
    const std::wstring relV = Widen(plan.rel.release);
    RegSetStr(HKEY_CURRENT_USER, g.regKey, L"LatestVersion", relV);
    // compare
    Installed in = ReadInstalled();
    if (!in.filter.valid || !in.core.valid) { SetStatus(L"Error", L"PushMPC is not installed completely: run PushMPC setup (Repair)."); return false; }
    RegSetStr(HKEY_CURRENT_USER, g.regKey, L"InstalledVersion", Widen(in.filter.get("version")));
    bool older = false;
    for (const char* name : { "filter", "core" }) {
        const ReleaseComponent* c = plan.rel.find(name);
        if (!c) continue;
        const Manifest& m = std::string(name) == "filter" ? in.filter : in.core;
        int cmp = CompareVersions(c->version, m.get("version"));
        if (cmp < 0) older = true;
        if (cmp > 0) plan.changes.push_back(*c);
    }
    if (const ReleaseComponent* rt = plan.rel.find("runtime", in.vendor)) {
        if (rt->format != "veyra-package" || !ReleaseRuntimeUrlOk(Widen(rt->url))) {
            SetStatus(L"Error", L"The update was refused: its runtime is not a reference to Veyra's package on GitHub."); return false;
        }
        bool differs = !in.runtime.valid || in.runtime.get("version") != rt->version || _stricmp(in.runtime.get("package-sha256").c_str(), rt->sha256.c_str()) != 0
                       || (!rt->subset.empty() && in.runtime.get("subset") != rt->subset);
        if (differs && !older) plan.changes.insert(plan.changes.begin(), *rt);
    }
    // apply order: runtime, core, filter
    std::stable_sort(plan.changes.begin(), plan.changes.end(), [](const ReleaseComponent& a, const ReleaseComponent& b) {
        auto rank = [](const std::string& n) { return n == "runtime" ? 0 : n == "core" ? 1 : 2; };
        return rank(a.name) < rank(b.name);
    });
    // keep the verified manifest for the download step and for prepare-runtime
    EnsureDir(Dir(L"staging"));
    WriteFileBytes(JoinPath(Dir(L"staging"), L"release.json"), plan.releaseBytes);
    if (haveSig) WriteFileBytes(JoinPath(Dir(L"staging"), L"release.json.sig"), plan.sigBytes);
    plan.upToDate = plan.changes.empty();
    if (plan.upToDate) {
        std::wstring none;
        SetStatus(L"UpToDate", Format(L"PushMPC %s is up to date.", Widen(in.filter.get("version")).c_str()), &none);
        return true;
    }
    std::wstring list;
    for (auto& c : plan.changes) list += (list.empty() ? L"" : L", ") + Widen(c.name) + L" " + Widen(c.version);
    Log::Write(L"available: " + list);
    if (Setting(L"FailedVersion") == relV && !g.force) {
        SetStatus(L"Error", L"PushMPC " + relV + L" failed its self-test here and was rolled back; waiting for a newer release.", &relV);
        plan.upToDate = true;   // nothing to do automatically
        return true;
    }
    SetStatus(L"Available", L"PushMPC " + relV + L" is available (" + list + L").", &relV);
    return true;
}

// ------------------------------------------------------------------ pending.json (the same file pushmpc-install.ps1 apply-pending reads)
struct Pending { std::string name, version, staging, release; };
std::vector<Pending> ReadPending() {
    std::vector<Pending> out;
    std::string text, err;
    if (!ReadFileBytes(Dir(L"pending.json"), text)) return out;
    JValue v;
    if (!JsonParse(text, v, err)) { Log::Write(L"pending.json unreadable: " + Widen(err)); return out; }
    auto add = [&](const JValue& o) { out.push_back({ o.str("name"), o.str("version"), o.str("staging"), o.str("release") }); };
    if (v.type == JValue::Array) for (auto& o : v.a) add(o); else if (v.type == JValue::Object) add(v);
    return out;
}
bool WritePending(const std::vector<Pending>& p) {
    std::string s = "[\r\n";
    for (size_t i = 0; i < p.size(); i++) {
        s += "  { \"name\": \"" + JsonEscape(p[i].name) + "\", \"version\": \"" + JsonEscape(p[i].version) + "\", \"staging\": \"" +
             JsonEscape(p[i].staging) + "\", \"release\": \"" + JsonEscape(p[i].release) + "\" }" + (i + 1 < p.size() ? ",\r\n" : "\r\n");
    }
    s += "]\r\n";
    return WriteFileBytes(Dir(L"pending.json"), s);
}

// ------------------------------------------------------------------ the bundled installer script (runtime: plan, prepare-runtime)
int RunScript(const std::wstring& script, const std::wstring& args, std::map<std::string, std::string>& kv) {
    std::wstring cmd = QuoteArg(PowerShellExe()) + L" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " + QuoteArg(script) + L" " + args +
                       L" -Machine -InstallDir " + QuoteArg(InstallDir());
    Log::Write(L"run " + cmd);
    return RunHidden(cmd, [&](const std::string& line) {
        Log::Write(L"  script: " + Widen(line));
        if (line.rfind("@@plan|", 0) == 0 || line.rfind("@@result|", 0) == 0) {
            std::string kvs = line.substr(line.find('|') + 1);
            size_t e = kvs.find('=');
            if (e != std::string::npos) kv[kvs.substr(0, e)] = kvs.substr(e + 1);
        } else if (line.rfind("@@progress|", 0) == 0) {
            // the script's own 0..100 inside the runtime preparation step (30..85 here)
            int p = atoi(line.c_str() + 11);
            size_t bar = line.find('|', 11);
            Prog(30 + p * 55 / 100, bar == std::string::npos ? L"Preparing the runtime" : Widen(line.substr(bar + 1)));
        }
    });
}

bool Extract(const std::wstring& zip, const std::wstring& to, std::wstring& err) {
    RemoveTree(to);
    EnsureDir(to);
    std::wstring cmd = QuoteArg(SystemExe(L"tar.exe")) + L" -xf " + QuoteArg(zip) + L" -C " + QuoteArg(to);
    std::string out;
    int rc = RunHidden(cmd, [&](const std::string& l) { out += l + "\n"; });
    if (rc != 0) { err = Format(L"unpacking %s failed (tar exit %d): ", zip.c_str(), rc) + Widen(out); return false; }
    return true;
}

bool Download(Plan& plan) {
    const std::wstring dl = Dir(L"download"), st = Dir(L"staging");
    EnsureDir(dl); EnsureDir(st);
    std::vector<Pending> pend;
    std::atomic<bool> noCancel{ false };
    int idx = 0;
    for (auto& c : plan.changes) {
        idx++;
        const std::wstring name = Widen(c.name), ver = Widen(c.version);
        const std::wstring target = JoinPath(st, name + L"-" + ver);
        if (c.name == "runtime") continue;   // below, after the zips (it may need the new filter's script)
        Manifest have = ReadManifest(target);
        if (have.valid && have.get("version") == c.version && CheckManifestFiles(target, have, true).empty()) {
            Say(L"Already staged: " + name + L" " + ver);
            pend.push_back({ c.name, c.version, Narrow(target), plan.rel.release });
            continue;
        }
        std::wstring url = ResolveUrl(plan.feed.url, plan.rel, c.url), err;
        std::wstring zip = JoinPath(dl, FileName(Widen(c.url)));
        Say(L"Downloading " + name + L" " + ver + L" from " + url);
        Progress pr;
        pr.cancel = &noCancel;
        pr.onBytes = [&](uint64_t d, uint64_t t) { if (t) Prog(5 + int(20.0 * d / t), Format(L"Downloading %s (%s of %s)", name.c_str(), HumanBytes(d).c_str(), HumanBytes(t).c_str())); };
        if (pm::Download(url, zip, c.sha256, pr, err) != DlResult::Ok) { SetStatus(L"Error", L"Download of " + name + L" " + ver + L" failed: " + err); return false; }
        std::wstring tmp = target + L".tmp";
        if (!Extract(zip, tmp, err)) { SetStatus(L"Error", err); return false; }
        Manifest m = ReadManifest(tmp);
        std::wstring bad = CheckManifestFiles(tmp, m, true);
        if (!m.valid || m.get("component") != c.name || m.get("version") != c.version || !bad.empty()) {
            RemoveTree(tmp);
            SetStatus(L"Error", L"The " + name + L" package does not match its manifest: " + (bad.empty() ? L"wrong component or version" : bad)); return false;
        }
        RemoveTree(target);
        if (!MoveDir(tmp, target)) { SetStatus(L"Error", L"Cannot stage " + target + L": " + ErrorText(GetLastError())); return false; }
        DeleteFileW(zip.c_str());
        pend.push_back({ c.name, c.version, Narrow(target), plan.rel.release });
        Say(L"Staged " + name + L" " + ver);
    }
    // the runtime: Veyra's package, prepared by the installer script of the release (it holds the file list and the pins)
    for (auto& c : plan.changes) {
        if (c.name != "runtime") continue;
        std::wstring script = JoinPath(Dir(L"filter"), L"pushmpc-install.ps1");
        for (auto& p : pend) if (p.name == "filter") script = JoinPath(Widen(p.staging), L"pushmpc-install.ps1");
        if (!FileExists(script)) { SetStatus(L"Error", L"The installer script is missing: " + script); return false; }
        const std::wstring relJson = JoinPath(st, L"release.json");
        const std::wstring vendorArg = L" -Gpu " + Widen(ReadInstalled().vendor);
        std::map<std::string, std::string> kv;
        Prog(26, L"Looking for the runtime package");
        int rc = RunScript(script, L"plan -ReleaseManifest " + QuoteArg(relJson) + vendorArg, kv);
        if (rc != 0 || kv["runtime"].empty()) { SetStatus(L"Error", Format(L"The runtime check failed (script exit %d); see the log.", rc)); return false; }
        if (kv["runtime"] == "current") { Say(L"runtime " + Widen(c.version) + L" is already installed"); continue; }
        if (kv["runtime"] == "download") {
            std::wstring url = Widen(kv["runtime-url"]), file = Widen(kv["runtime-file"]), err;
            if (!ReleaseRuntimeUrlOk(url)) { SetStatus(L"Error", L"Refused runtime URL " + url); return false; }
            Say(L"Downloading Veyra's runtime package " + url);
            Progress pr; pr.cancel = &noCancel;
            pr.onBytes = [&](uint64_t d, uint64_t t) { if (t) Prog(5 + int(25.0 * d / t), Format(L"Downloading Veyra's runtime package (%s of %s)", HumanBytes(d).c_str(), HumanBytes(t).c_str())); };
            if (pm::Download(url, file, kv["runtime-sha256"], pr, err) != DlResult::Ok) { SetStatus(L"Error", L"Download of Veyra's runtime package failed: " + err); return false; }
        }
        if (kv["pushflow"] == "download") {
            std::wstring url = Widen(kv["pushflow-url"]), file = Widen(kv["pushflow-file"]), err;
            Say(L"Downloading PushFPS's release for PushFlow " + url);
            Progress pr; pr.cancel = &noCancel;
            if (pm::Download(url, file, "", pr, err) != DlResult::Ok) Say(L"PushFlow download failed (" + err + L"): PushFlow stays unavailable");
        }
        kv.clear();
        Prog(30, L"Preparing the runtime");
        rc = RunScript(script, L"prepare-runtime -ReleaseManifest " + QuoteArg(relJson) + vendorArg + L" -PackageCache " + QuoteArg(dl), kv);
        if (rc != 0) { SetStatus(L"Error", Format(L"Preparing the runtime failed (script exit %d); see the log.", rc)); return false; }
        if (kv["staging"].empty()) { Say(L"runtime: nothing to stage"); continue; }
        std::wstring staged = Widen(kv["staging"]);
        Manifest m = ReadManifest(staged);
        if (!m.valid || !CheckManifestFiles(staged, m, false).empty()) { SetStatus(L"Error", L"The prepared runtime is incomplete: " + staged); return false; }
        pend.insert(pend.begin(), { "runtime", c.version, Narrow(staged), plan.rel.release });
        Say(L"Staged runtime " + Widen(c.version));
    }
    if (pend.empty()) { SetStatus(L"UpToDate", L"Nothing to install."); return true; }
    WritePending(pend);
    std::wstring rv = Widen(plan.rel.release);
    SetStatus(L"Downloaded", L"PushMPC " + rv + L" is downloaded and installs when MPC-HC (or MPC-BE) is not running.", &rv);
    Prog(88, L"Downloaded");
    return true;
}

// ------------------------------------------------------------------ self-tests (in a child process, so a crash cannot take the updater down)
int RunChild(const std::wstring& args, DWORD timeoutMs) {
    std::wstring cmd = QuoteArg(ModulePath()) + L" " + args;
    STARTUPINFOW si{ sizeof(si) }; PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return -1;
    CloseHandle(pi.hThread);
    DWORD code = 0;
    if (WaitForSingleObject(pi.hProcess, timeoutMs) != WAIT_OBJECT_0) { TerminateProcess(pi.hProcess, 258); code = 258; }
    else GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return int(code);
}
int SelfTestFilter(const std::wstring& ax, const std::wstring& clsidStr) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    CLSID clsid{};
    if (FAILED(CLSIDFromString(clsidStr.c_str(), &clsid))) return 10;
    HMODULE m = LoadLibraryExW(ax.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) return 11;
    using GetClassObjectFn = HRESULT(__stdcall*)(REFCLSID, REFIID, void**);
    auto fn = reinterpret_cast<GetClassObjectFn>(GetProcAddress(m, "DllGetClassObject"));
    if (!fn) return 12;
    IClassFactory* cf = nullptr;
    HRESULT hr = fn(clsid, IID_IClassFactory, reinterpret_cast<void**>(&cf));
    if (FAILED(hr) || !cf) return 13;
    cf->Release();
    FreeLibrary(m);
    return 0;
}
int SelfTestCore(const std::wstring& dll) {
    // mapped without running its code or resolving its imports: a valid image with the C ABI entry point
    HMODULE m = LoadLibraryExW(dll.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!m) return 21;
    bool ok = GetProcAddress(m, "pushcore_getApi") != nullptr;
    FreeLibrary(m);
    return ok ? 0 : 22;
}
bool Regsvr(const std::wstring& ax, bool unregister) {
    std::wstring cmd = QuoteArg(SystemExe(L"regsvr32.exe")) + L" /s " + (unregister ? L"/u " : L"") + QuoteArg(ax);
    int rc = RunHidden(cmd, nullptr);
    Log::Writef(L"regsvr32%s %s -> %d", unregister ? L" /u" : L"", ax.c_str(), rc);
    return rc == 0;
}
bool RegisteredAt(const std::wstring& ax) {
    std::wstring v = RegGetStr(HKEY_CURRENT_USER, std::wstring(L"Software\\Classes\\CLSID\\") + kFilterClsid + L"\\InprocServer32", L"");
    return !_wcsicmp(v.c_str(), ax.c_str());
}

// ------------------------------------------------------------------ apply
int Apply() {
    std::vector<Pending> pend = ReadPending();
    if (pend.empty()) { Say(L"Nothing pending."); return 0; }
    std::sort(pend.begin(), pend.end(), [](const Pending& a, const Pending& b) {
        auto rank = [](const std::string& n) { return n == "runtime" ? 0 : n == "core" ? 1 : 2; };
        return rank(a.name) < rank(b.name);
    });
    std::wstring rv = Widen(pend.front().release.empty() ? pend.front().version : pend.front().release);
    for (auto& p : pend) if (p.name == "filter") rv = Widen(p.release.empty() ? p.version : p.release);
    // in use?
    auto holders = FindHolders({ Dir(L"filter"), Dir(L"core"), Dir(L"runtime"), Dir(L"vsplugin") });
    if (!holders.empty()) {
        std::wstring who;
        for (auto& h : holders) who += (who.empty() ? L"" : L", ") + h.name + Format(L" (pid %lu)", h.pid);
        SetStatus(L"Downloaded", L"PushMPC " + rv + L" installs when the player is closed (in use by " + who + L").", &rv);
        return 2;
    }
    for (auto& p : pend) {
        std::wstring s = Widen(p.staging);
        Manifest m = ReadManifest(s);
        std::wstring bad = CheckManifestFiles(s, m, false);
        if (!m.valid || !bad.empty()) {
            DeleteFileW(Dir(L"pending.json").c_str());
            SetStatus(L"Error", L"The staged " + Widen(p.name) + L" is incomplete (" + bad + L"); it will be downloaded again.");
            return 1;
        }
    }
    Prog(90, L"Installing the update");
    // swap: <c> -> previous\<c>, staging -> <c>
    struct Swapped { std::string name; std::wstring dst, prev; bool hadOld; };
    std::vector<Swapped> done;
    auto rollback = [&](const std::wstring& why) {
        for (auto it = done.rbegin(); it != done.rend(); ++it) {
            std::wstring failed = JoinPath(Dir(L"staging"), L"failed-" + Widen(it->name));
            RemoveTree(failed);
            MoveDir(it->dst, failed);
            if (it->hadOld) MoveDir(it->prev, it->dst);
            if (DirExists(it->prev + L".keep")) MoveDir(it->prev + L".keep", it->prev);   // the older rollback point comes back
            RemoveTree(failed);
            Log::Write(L"rolled back " + Widen(it->name));
        }
        if (!g.noRegister && FileExists(JoinPath(Dir(L"filter"), L"PushMpcFilter.ax"))) Regsvr(JoinPath(Dir(L"filter"), L"PushMpcFilter.ax"), false);
        Log::Write(L"rollback: " + why);
    };
    for (auto& p : pend) {
        std::wstring name = Widen(p.name), dst = Dir(name.c_str()), prev = JoinPath(Dir(L"previous"), name);
        bool hadOld = DirExists(dst);
        if (hadOld) {
            // the current previous\<c> is kept aside until the new version has passed its self-test
            RemoveTree(prev + L".keep");
            if (DirExists(prev) && !MoveDir(prev, prev + L".keep")) RemoveTree(prev);
            if (!MoveDir(dst, prev)) {
                if (DirExists(prev + L".keep")) MoveDir(prev + L".keep", prev);
                DWORD e = GetLastError();
                rollback(L"cannot move " + dst);
                SetStatus(L"Downloaded", L"PushMPC " + rv + L" could not be installed yet (" + ErrorText(e) + L"); it is tried again later.", &rv);
                return 2;
            }
        }
        if (!MoveDir(Widen(p.staging), dst)) {
            DWORD e = GetLastError();
            if (hadOld) { MoveDir(prev, dst); if (DirExists(prev + L".keep")) MoveDir(prev + L".keep", prev); }
            rollback(L"cannot move the staged " + name);
            SetStatus(L"Downloaded", L"PushMPC " + rv + L" could not be installed yet (" + ErrorText(e) + L"); it is tried again later.", &rv);
            return 2;
        }
        done.push_back({ p.name, dst, prev, hadOld });
        Log::Write(L"swapped in " + name + L" " + Widen(p.version));
    }
    // register and self-test the filter; check the core image
    const std::wstring ax = JoinPath(Dir(L"filter"), L"PushMpcFilter.ax");
    std::wstring fail;
    bool filterChanged = std::any_of(done.begin(), done.end(), [](const Swapped& s) { return s.name == "filter"; });
    bool coreChanged = std::any_of(done.begin(), done.end(), [](const Swapped& s) { return s.name == "core"; });
    if (filterChanged) {
        if (!g.noRegister && (!Regsvr(ax, false) || !RegisteredAt(ax))) fail = L"the new filter did not register";
        if (fail.empty()) {
            int rc = RunChild(L"--selftest-filter " + QuoteArg(ax) + L" " + kFilterClsid, 30000);
            Log::Writef(L"filter self-test -> %d", rc);
            if (rc != 0) fail = Format(L"the new filter did not load (self-test %d)", rc);
        }
    }
    if (fail.empty() && coreChanged) {
        int rc = RunChild(L"--selftest-core " + QuoteArg(JoinPath(Dir(L"core"), L"pushcore.dll")), 30000);
        Log::Writef(L"core self-test -> %d", rc);
        if (rc != 0) fail = Format(L"the new core is not a valid pushcore.dll (self-test %d)", rc);
    }
    if (!fail.empty()) {
        rollback(fail);
        DeleteFileW(Dir(L"pending.json").c_str());
        RegSetStr(HKEY_CURRENT_USER, g.regKey, L"FailedVersion", rv);
        SetStatus(L"Error", L"PushMPC " + rv + L" was not installed: " + fail + L". The previous version was restored.");
        return 1;
    }
    DeleteFileW(Dir(L"pending.json").c_str());
    for (auto& d : done) RemoveTree(d.prev + L".keep");
    // the updater updates itself from the new filter component (a running exe can be renamed, not overwritten)
    std::wstring newUpd = JoinPath(Dir(L"filter"), kUpdaterExe), rootUpd = JoinPath(InstallDir(), kUpdaterExe);
    if (filterChanged && FileExists(newUpd) && !FilesEqual(newUpd, rootUpd)) {
        std::wstring old = JoinPath(InstallDir(), L"PushMpcUpdater.old.exe");
        DeleteFileW(old.c_str());
        if (FileExists(rootUpd)) MoveFileExW(rootUpd.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
        if (CopyFileW(newUpd.c_str(), rootUpd.c_str(), FALSE)) Log::Write(L"updater replaced by the new release's copy");
        else MoveFileExW(old.c_str(), rootUpd.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    // the Windows Apps entry shows the new version
    Manifest fm = ReadManifest(Dir(L"filter"));
    if (!g.noRegister && RegHasValue(HKEY_CURRENT_USER, kUninstallKey, L"DisplayName")) {
        RegSetStr(HKEY_CURRENT_USER, kUninstallKey, L"DisplayVersion", Widen(fm.get("version")));
        RegSetDword(HKEY_CURRENT_USER, kUninstallKey, L"EstimatedSize", DWORD(TreeSize(InstallDir()) / 1024));
    }
    // downloads no longer needed (Veyra's package is ~750 MB)
    for (auto& d : done) if (d.name == "runtime") {
        Manifest rm = ReadManifest(Dir(L"runtime"));
        std::wstring pkg = JoinPath(Dir(L"download"), Widen(rm.get("package")));
        if (!rm.get("package").empty()) DeleteFileW(pkg.c_str());
    }
    RegDeleteKeyValueW(HKEY_CURRENT_USER, g.regKey.c_str(), L"FailedVersion");
    RegSetStr(HKEY_CURRENT_USER, g.regKey, L"InstalledVersion", Widen(fm.get("version")));
    RegSetStr(HKEY_CURRENT_USER, g.regKey, L"LastApply", UtcNowIso());
    std::wstring none;
    SetStatus(L"UpToDate", L"Updated to PushMPC " + rv + L".", &none);
    Prog(100, L"Updated");
    return 0;
}

// ------------------------------------------------------------------ the logon task (Task Scheduler 2.0, per user, least privilege)
std::wstring UserName() {
    wchar_t buf[512]; ULONG n = 512;
    if (GetUserNameExW(NameSamCompatible, buf, &n)) return buf;
    DWORD m = 512; GetUserNameW(buf, &m); return buf;
}
std::wstring XmlEsc(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) { if (c == L'&') o += L"&amp;"; else if (c == L'<') o += L"&lt;"; else if (c == L'>') o += L"&gt;"; else if (c == L'"') o += L"&quot;"; else o += c; }
    return o;
}
int RegisterTask(bool remove) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool coInit = SUCCEEDED(hr);
    ITaskService* svc = nullptr;
    int rc = 1;
    hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskService, reinterpret_cast<void**>(&svc));
    if (SUCCEEDED(hr)) hr = svc->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t());
    if (SUCCEEDED(hr)) {
        ITaskFolder* root = nullptr; ITaskFolder* folder = nullptr;
        svc->GetFolder(_bstr_t(L"\\"), &root);
        if (root) {
            if (FAILED(root->GetFolder(_bstr_t(kTaskFolder), &folder)) && !remove) root->CreateFolder(_bstr_t(kTaskFolder), _variant_t(), &folder);
        }
        if (remove) {
            if (folder) {
                hr = folder->DeleteTask(_bstr_t(kTaskName), 0);
                Log::Writef(L"task removed (0x%08lx)", hr);
                IRegisteredTaskCollection* tasks = nullptr; LONG count = 1;
                if (SUCCEEDED(folder->GetTasks(TASK_ENUM_HIDDEN, &tasks)) && tasks) { tasks->get_Count(&count); tasks->Release(); }
                folder->Release(); folder = nullptr;
                if (count == 0 && root) root->DeleteFolder(_bstr_t(kTaskFolder), 0);
            }
            rc = 0;
        } else if (folder) {
            std::wstring user = XmlEsc(UserName());
            std::wstring exe = XmlEsc(JoinPath(InstallDir(), kUpdaterExe));
            std::wstring xml =
                L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
                L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
                L" <RegistrationInfo><Description>Installs a downloaded PushMPC update at logon, when MPC-HC or MPC-BE is not running.</Description><URI>\\PushMPC\\Apply pending update</URI></RegistrationInfo>\r\n"
                L" <Triggers><LogonTrigger><Enabled>true</Enabled><UserId>" + user + L"</UserId><Delay>PT1M</Delay></LogonTrigger></Triggers>\r\n"
                L" <Principals><Principal id=\"Author\"><UserId>" + user + L"</UserId><LogonType>InteractiveToken</LogonType><RunLevel>LeastPrivilege</RunLevel></Principal></Principals>\r\n"
                L" <Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy><DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
                L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries><AllowHardTerminate>true</AllowHardTerminate><StartWhenAvailable>true</StartWhenAvailable>"
                L"<RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable><IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings>"
                L"<AllowStartOnDemand>true</AllowStartOnDemand><Enabled>true</Enabled><Hidden>false</Hidden><RunOnlyIfIdle>false</RunOnlyIfIdle>"
                L"<ExecutionTimeLimit>PT30M</ExecutionTimeLimit><Priority>7</Priority></Settings>\r\n"
                L" <Actions Context=\"Author\"><Exec><Command>\"" + exe + L"\"</Command><Arguments>--apply --from-task</Arguments></Exec></Actions>\r\n"
                L"</Task>\r\n";
            IRegisteredTask* task = nullptr;
            hr = folder->RegisterTask(_bstr_t(kTaskName), _bstr_t(xml.c_str()), TASK_CREATE_OR_UPDATE, _variant_t(), _variant_t(),
                                      TASK_LOGON_INTERACTIVE_TOKEN, _variant_t(L""), &task);
            Log::Writef(L"task \\PushMPC\\%s registered (0x%08lx)", kTaskName, hr);
            if (task) task->Release();
            rc = SUCCEEDED(hr) ? 0 : 1;
            if (FAILED(hr)) Say(Format(L"The logon task could not be registered (0x%08lx): pending updates then install when the filter next checks.", hr));
        }
        if (folder) folder->Release();
        if (root) root->Release();
    } else Log::Writef(L"Task Scheduler not available (0x%08lx)", hr);
    if (svc) svc->Release();
    if (coInit) CoUninitialize();
    return rc;
}

// ------------------------------------------------------------------ CLI
void Usage() {
    Out(L"PushMpcUpdater " PUSHMPC_UPDATER_VERSION_W L" - PushMPC's updater\r\n"
        L"  --check | --download | --apply | --auto | --status | --register-task | --unregister-task\r\n"
        L"  options: --force (ignore the 24 h limit and a failed version), --channel Stable|Beta, --feed <url|file|folder>,\r\n"
        L"           --install-dir <dir>, --reg-key <HKCU subkey>, --no-register, --allow-unsigned, --machine\r\n"
        L"  status:  HKCU\\Software\\PushMPC\\Update (LastCheck, Status, AvailableVersion, Message)");
}
bool Parse(int argc, wchar_t** argv) {
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        auto next = [&](std::wstring& v) { if (i + 1 < argc) { v = argv[++i]; return true; } return false; };
        if (a == L"--check" || a == L"--download" || a == L"--apply" || a == L"--auto" || a == L"--status" || a == L"--register-task" ||
            a == L"--unregister-task" || a == L"--version" || a == L"--help" || a == L"/?") g.command = a;
        else if (a == L"--selftest-filter") { g.command = a; next(g.arg1); next(g.arg2); }
        else if (a == L"--selftest-core") { g.command = a; next(g.arg1); }
        else if (a == L"--install-dir") next(g.installDir);
        else if (a == L"--reg-key") next(g.regKey);
        else if (a == L"--feed") next(g.feed);
        else if (a == L"--channel") next(g.channel);
        else if (a == L"--no-register") g.noRegister = true;
        else if (a == L"--allow-unsigned") g.allowUnsigned = true;
        else if (a == L"--machine") g.machine = true;
        else if (a == L"--force") g.force = true;
        else if (a == L"--from-task") g.fromTask = true;
        else if (a == L"--quiet") g.quiet = true;
        else if (a == L"--test-loopback") g.testLoopback = true;   // developer tests: a runtime package on 127.0.0.1 / file://
        else if (a == L"--releases-api") next(g.releasesApi);      // developer tests: a stand-in for GitHub's release list
        else { Out(L"unknown argument " + a); return false; }
    }
    return true;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    InitOutput();
    if (!Parse(argc, argv)) { Usage(); return 64; }
    if (g.command.empty() || g.command == L"--help" || g.command == L"/?") { Usage(); return g.command.empty() ? 64 : 0; }
    if (g.command == L"--version") { Out(PUSHMPC_UPDATER_VERSION_W); return 0; }
    if (g.command == L"--selftest-filter") return SelfTestFilter(g.arg1, g.arg2.empty() ? kFilterClsid : g.arg2);
    if (g.command == L"--selftest-core") return SelfTestCore(g.arg1);
    if (!g.installDir.empty()) { wchar_t full[MAX_PATH * 4]; if (GetFullPathNameW(g.installDir.c_str(), MAX_PATH * 4, full, nullptr)) g.installDir = full; }

    SetUserAgent(L"PushMpcUpdater/" PUSHMPC_UPDATER_VERSION_W);
    Log::Open(JoinPath(Dir(L"logs"), L"updater.log"));
    // leftovers of a self-update
    DeleteFileW(JoinPath(InstallDir(), L"PushMpcUpdater.old.exe").c_str());

    if (g.command == L"--status") {
        for (auto n : { L"Status", L"Message", L"AvailableVersion", L"LatestVersion", L"InstalledVersion", L"LastCheck", L"LastApply", L"Channel", L"FeedUrl" })
            Out(Format(L"%-17s %s", n, RegGetStr(HKEY_CURRENT_USER, g.regKey, n).c_str()));
        Out(Format(L"%-17s %lu", L"Enabled", RegGetDword(HKEY_CURRENT_USER, g.regKey, L"Enabled", 1)));
        Installed in = ReadInstalled();
        Out(L"filter            " + Widen(in.filter.get("version")));
        Out(L"core              " + Widen(in.core.get("version")));
        Out(L"runtime           " + Widen(in.runtime.get("version")));
        Out(L"pending           " + std::wstring(FileExists(Dir(L"pending.json")) ? L"yes" : L"no"));
        return 0;
    }
    if (g.command == L"--register-task") return RegisterTask(false);
    if (g.command == L"--unregister-task") return RegisterTask(true);

    // one updater at a time per install
    std::wstring mtx = L"Local\\PushMpcUpdater-" + Widen(Sha256Hex(Narrow(Lower(InstallDir())).data(), Narrow(Lower(InstallDir())).size()).substr(0, 16));
    HANDLE mutex = CreateMutexW(nullptr, FALSE, mtx.c_str());
    DWORD w = WaitForSingleObject(mutex, g.command == L"--auto" ? 0 : 120000);
    if (w == WAIT_TIMEOUT) { Log::Write(L"another updater is running; " + g.command + L" skipped"); return 0; }

    Log::Write(L"PushMpcUpdater " PUSHMPC_UPDATER_VERSION_W L" " + g.command + L" (install " + InstallDir() + L")");
    const bool enabled = RegGetDword(HKEY_CURRENT_USER, g.regKey, L"Enabled", 1) != 0;
    int rc = 0;
    if (g.command == L"--check") {
        Plan p; rc = Check(p) ? 0 : 1;
    } else if (g.command == L"--download") {
        Plan p; rc = (Check(p) && (p.upToDate || Download(p))) ? 0 : 1;
    } else if (g.command == L"--apply") {
        if (g.fromTask && !enabled) { Log::Write(L"automatic updates are off: the task does nothing"); rc = 0; }
        else rc = Apply();
    } else if (g.command == L"--auto") {
        if (!enabled) { Log::Write(L"automatic updates are off (Enabled=0)"); rc = 0; }
        else {
            SetPriorityClass(GetCurrentProcess(), PROCESS_MODE_BACKGROUND_BEGIN);
            if (FileExists(Dir(L"pending.json"))) rc = Apply();
            std::wstring last = RegGetStr(HKEY_CURRENT_USER, g.regKey, L"LastCheck");
            bool due = g.force || last.empty();
            if (!due) {
                SYSTEMTIME st{}; FILETIME lf{}, nf{};
                if (swscanf_s(last.c_str(), L"%hu-%hu-%huT%hu:%hu:%hu", &st.wYear, &st.wMonth, &st.wDay, &st.wHour, &st.wMinute, &st.wSecond) == 6 && SystemTimeToFileTime(&st, &lf)) {
                    GetSystemTimeAsFileTime(&nf);
                    ULARGE_INTEGER a{ { lf.dwLowDateTime, lf.dwHighDateTime } }, b{ { nf.dwLowDateTime, nf.dwHighDateTime } };
                    due = b.QuadPart < a.QuadPart || (b.QuadPart - a.QuadPart) >= 24ull * 3600 * 10000000;
                } else due = true;
            }
            if (!due) Log::Write(L"last check " + last + L": not due yet");
            else if (!FileExists(Dir(L"pending.json"))) {
                Plan p;
                if (!Check(p)) rc = 1;
                else if (!p.upToDate) rc = Download(p) ? Apply() : 1;
            }
        }
    }
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    Log::Writef(L"exit %d", rc);
    return rc;
}
