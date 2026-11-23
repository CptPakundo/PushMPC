// PushMPC installer and updater: shared helpers (Win32 only, no third-party code).
//   strings, files, SHA-256, component manifests, registry, versions, processes, logging,
//   JSON, release manifests and their RSA-SHA256 signature, downloads (WinHTTP, resumable).
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace pm {

// ------------------------------------------------------------------ constants (the contract with the filter and the docs)
constexpr const wchar_t* kRepoUrl = L"https://github.com/CptPakundo/PushMPC";
constexpr const wchar_t* kRepoApiReleases = L"https://api.github.com/repos/CptPakundo/PushMPC/releases?per_page=20";
constexpr const wchar_t* kDefaultFeed = L"https://github.com/CptPakundo/PushMPC/releases/latest/download/release.json";
constexpr const wchar_t* kSettingsKey = L"Software\\PushMPC";                 // HKCU
constexpr const wchar_t* kUpdateKey = L"Software\\PushMPC\\Update";           // HKCU: LastCheck, Status, AvailableVersion, Message, ...
constexpr const wchar_t* kUninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PushMPC";
constexpr const wchar_t* kFilterClsid = L"{798D4EC3-E59C-4A4E-A2EE-C804AEAE04CC}";
constexpr const wchar_t* kTaskFolder = L"\\PushMPC";
constexpr const wchar_t* kTaskName = L"Apply pending update";
constexpr const wchar_t* kUpdaterExe = L"PushMpcUpdater.exe";
constexpr const wchar_t* kSetupExe = L"PushMpcSetup.exe";   // the maintenance copy of PushMPC-Setup.exe in the install folder
constexpr const wchar_t* kPublisher = L"PushMPC (community)";

// ------------------------------------------------------------------ strings
std::wstring Widen(const std::string& s);
std::string Narrow(const std::wstring& s);
std::wstring Lower(std::wstring s);
std::string Trim(const std::string& s);
std::wstring TrimW(const std::wstring& s);
bool StartsWithI(const std::wstring& s, const std::wstring& prefix);
std::wstring Format(const wchar_t* fmt, ...);
std::wstring ErrorText(DWORD err);
std::wstring UtcNowIso();       // 2026-11-23T10:15:00Z
std::wstring HumanBytes(uint64_t n);

// ------------------------------------------------------------------ files
std::wstring JoinPath(const std::wstring& a, const std::wstring& b);
std::wstring ParentDir(const std::wstring& p);
std::wstring FileName(const std::wstring& p);
std::wstring ModulePath(HMODULE m = nullptr);
std::wstring KnownLocalAppData();
std::wstring TempDir();
bool FileExists(const std::wstring& p);
bool DirExists(const std::wstring& p);
bool EnsureDir(const std::wstring& p);
bool RemoveTree(const std::wstring& p);                          // files and folders, recursive
bool CopyTree(const std::wstring& from, const std::wstring& to); // into "to" (created)
bool MoveDir(const std::wstring& from, const std::wstring& to);  // same volume rename
uint64_t FileSize(const std::wstring& p);
uint64_t TreeSize(const std::wstring& p);
bool ReadFileBytes(const std::wstring& p, std::string& out);
bool WriteFileBytes(const std::wstring& p, const std::string& data);   // via a temp file + rename
bool FilesEqual(const std::wstring& a, const std::wstring& b);

// ------------------------------------------------------------------ hashes
std::string Sha256Hex(const void* data, size_t n);                 // upper-case hex
bool Sha256File(const std::wstring& p, std::string& hexOut, std::atomic<bool>* cancel = nullptr);

// ------------------------------------------------------------------ component manifests (manifest.txt, see pushmpc-install.ps1)
struct ManifestFile { std::string sha; uint64_t size = 0; std::wstring rel; };
struct Manifest {
    bool valid = false;
    std::map<std::string, std::string> kv;
    std::vector<ManifestFile> files;
    std::string get(const std::string& k) const { auto i = kv.find(k); return i == kv.end() ? std::string() : i->second; }
};
Manifest ReadManifest(const std::wstring& dir);
// "" = every file present with its size (and hash when fullHash); else the first problems
std::wstring CheckManifestFiles(const std::wstring& dir, const Manifest& m, bool fullHash);

// ------------------------------------------------------------------ registry (HKCU)
std::wstring RegGetStr(HKEY root, const std::wstring& key, const std::wstring& name, const std::wstring& def = L"");
DWORD RegGetDword(HKEY root, const std::wstring& key, const std::wstring& name, DWORD def);
bool RegHasValue(HKEY root, const std::wstring& key, const std::wstring& name);
bool RegSetStr(HKEY root, const std::wstring& key, const std::wstring& name, const std::wstring& v);
bool RegSetDword(HKEY root, const std::wstring& key, const std::wstring& name, DWORD v);
bool RegDeleteTreeKey(HKEY root, const std::wstring& key);

// ------------------------------------------------------------------ versions: 0.6.0, 0.6.1-beta.2, 0.6.0+pushfps4.3.1
int CompareVersions(const std::string& a, const std::string& b);

// ------------------------------------------------------------------ logging (UTF-8, one line per call, timestamped)
class Log {
public:
    static void Open(const std::wstring& path);   // rotates at 2 MB (.old.log)
    static void Write(const std::wstring& line);
    static void Writef(const wchar_t* fmt, ...);
    static void SetEcho(std::function<void(const std::wstring&)> f);
private:
    static std::wstring path_;
};

// ------------------------------------------------------------------ processes
struct Holder { DWORD pid = 0; std::wstring name; std::wstring module; };
// processes that have a module loaded from one of these folders; mpc-hc / mpc-be processes whose modules cannot be
// read count as holders too (we cannot prove they do not hold the files)
std::vector<Holder> FindHolders(const std::vector<std::wstring>& dirs);
std::vector<DWORD> FindPlayerProcesses();          // mpc-hc64.exe, mpc-hc.exe, mpc-be64.exe, mpc-be.exe (this session)
bool ClosePlayersGracefully(DWORD waitMs);          // WM_CLOSE to their top-level windows, then wait
// Runs a command hidden (no window), calls onLine for every output line (UTF-8 decoded); cancelEvent may be null.
// Returns the exit code, or -1 if it could not start.
int RunHidden(const std::wstring& cmdline, const std::function<void(const std::string&)>& onLine, HANDLE cancelEvent = nullptr,
              const std::wstring& workDir = L"");
std::wstring QuoteArg(const std::wstring& a);
std::wstring PowerShellExe();
std::wstring SystemExe(const wchar_t* name);       // %SystemRoot%\System32\<name>

// ------------------------------------------------------------------ JSON (enough for release manifests and the GitHub API)
struct JValue {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<JValue> a;
    std::vector<std::pair<std::string, JValue>> o;
    const JValue* get(const char* key) const;
    std::string str(const char* key, const std::string& def = "") const;
    bool boolean(const char* key, bool def = false) const;
};
bool JsonParse(const std::string& text, JValue& out, std::string& err);
std::string JsonEscape(const std::string& s);

// ------------------------------------------------------------------ release manifest (release.json, schema 2 and 3)
struct ReleaseComponent {
    std::string name, version, url, sha256, vendor, format, asset, subset, repository, tag, pushfps;
};
struct Release {
    int schema = 0;
    std::string release, tag, repository, channel;
    std::vector<ReleaseComponent> components;
    const ReleaseComponent* find(const std::string& name, const std::string& vendor = "") const;
};
bool ParseRelease(const std::string& json, Release& out, std::string& err);

// RSA-SHA256 (PKCS#1 v1.5) over data, signature base64; key = .NET RSAKeyValue XML (Modulus, Exponent)
bool VerifySignature(const std::string& data, const std::string& sigBase64, const std::string& keyXml, std::wstring& err);

// ------------------------------------------------------------------ downloads
struct Progress {
    std::function<void(uint64_t done, uint64_t total)> onBytes;   // total 0 = unknown
    std::atomic<bool>* cancel = nullptr;
};
enum class DlResult { Ok, Cancelled, Failed };
bool IsHttpUrl(const std::wstring& u);
bool IsFileUrl(const std::wstring& u);
std::wstring FileUrlToPath(const std::wstring& u);
// http(s): resumable (<dest>.part + Range), redirects followed; file:// or a path: copied. sha256 "" = no check.
// A finished file whose SHA-256 matches is reused without a download.
DlResult Download(const std::wstring& url, const std::wstring& dest, const std::string& sha256, const Progress& pr, std::wstring& err);
bool Fetch(const std::wstring& url, std::string& out, std::wstring& err, size_t maxBytes = 8 << 20);
// relative "url" of a component: against <repository>/releases/download/<tag>/ for an http feed with a tag, else the feed's folder
std::wstring ResolveUrl(const std::wstring& feedUrl, const Release& rel, const std::string& url);
void SetUserAgent(const std::wstring& ua);

} // namespace pm
