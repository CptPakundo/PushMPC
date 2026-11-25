// PushMPC installer and updater: strings, files, hashes, manifests, registry, versions, logging, processes.
#include "pm_common.h"

#include <bcrypt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <algorithm>
#include <cstdarg>
#include <cwctype>
#include <mutex>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")

namespace pm {

// ================================================================== strings
std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}
std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring Lower(std::wstring s) { for (auto& c : s) c = wchar_t(std::towlower(c)); return s; }
std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
std::wstring TrimW(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n"), b = s.find_last_not_of(L" \t\r\n");
    return a == std::wstring::npos ? std::wstring() : s.substr(a, b - a + 1);
}
bool StartsWithI(const std::wstring& s, const std::wstring& p) {
    return s.size() >= p.size() && CompareStringOrdinal(s.c_str(), int(p.size()), p.c_str(), int(p.size()), TRUE) == CSTR_EQUAL;
}
std::wstring Format(const wchar_t* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    wchar_t buf[4096];
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    return buf;
}
std::wstring ErrorText(DWORD err) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err, 0, (LPWSTR)&msg, 0, nullptr);
    std::wstring s = msg ? TrimW(msg) : Format(L"error %lu", err);
    if (msg) LocalFree(msg);
    return s + Format(L" (%lu)", err);
}
std::wstring UtcNowIso() {
    SYSTEMTIME st; GetSystemTime(&st);
    return Format(L"%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}
std::wstring HumanBytes(uint64_t n) {
    if (n >= (1ull << 30)) return Format(L"%.2f GB", double(n) / double(1ull << 30));
    if (n >= (1ull << 20)) return Format(L"%.1f MB", double(n) / double(1ull << 20));
    if (n >= 1024) return Format(L"%.0f kB", double(n) / 1024.0);
    return Format(L"%llu bytes", n);
}

// ================================================================== files
std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}
std::wstring ParentDir(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? std::wstring() : p.substr(0, i);
}
std::wstring FileName(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? p : p.substr(i + 1);
}
std::wstring ModulePath(HMODULE m) {
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(m, buf.data(), DWORD(buf.size()));
    buf.resize(n);
    return buf;
}
std::wstring KnownLocalAppData() {
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) s = p;
    CoTaskMemFree(p);
    return s;
}
std::wstring TempDir() {
    wchar_t buf[MAX_PATH + 1]{};
    GetTempPathW(MAX_PATH, buf);
    std::wstring s = buf;
    while (!s.empty() && s.back() == L'\\') s.pop_back();
    return s;
}
bool FileExists(const std::wstring& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
bool DirExists(const std::wstring& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
bool EnsureDir(const std::wstring& p) {
    if (p.empty() || DirExists(p)) return true;
    int r = SHCreateDirectoryExW(nullptr, p.c_str(), nullptr);
    return r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS || r == ERROR_FILE_EXISTS;
}
bool RemoveTree(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return true;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY) || (a & FILE_ATTRIBUTE_REPARSE_POINT)) {
        SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
        return (a & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(p.c_str()) != 0 : DeleteFileW(p.c_str()) != 0;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(JoinPath(p, L"*").c_str(), &fd);
    bool ok = true;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            if (!RemoveTree(JoinPath(p, fd.cFileName))) ok = false;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (!RemoveDirectoryW(p.c_str())) ok = false;
    return ok;
}
bool CopyTree(const std::wstring& from, const std::wstring& to) {
    if (!EnsureDir(to)) return false;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(JoinPath(from, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return true;
    bool ok = true;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        std::wstring s = JoinPath(from, fd.cFileName), d = JoinPath(to, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (!CopyTree(s, d)) ok = false; }
        else if (!CopyFileW(s.c_str(), d.c_str(), FALSE)) ok = false;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}
bool MoveDir(const std::wstring& from, const std::wstring& to) {
    EnsureDir(ParentDir(to));
    for (int i = 0; i < 5; i++) {   // a virus scanner may hold a file for a moment
        if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) return true;
        Sleep(400);
    }
    return false;
}
uint64_t FileSize(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return 0;
    return (uint64_t(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
}
uint64_t TreeSize(const std::wstring& p) {
    uint64_t t = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(JoinPath(p, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) t += TreeSize(JoinPath(p, fd.cFileName));
        else t += (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return t;
}
bool ReadFileBytes(const std::wstring& p, std::string& out) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{}; GetFileSizeEx(h, &sz);
    out.resize(size_t(sz.QuadPart));
    DWORD got = 0; bool ok = true; size_t off = 0;
    while (off < out.size()) {
        DWORD want = DWORD(std::min<size_t>(out.size() - off, 1 << 24));
        if (!ReadFile(h, out.data() + off, want, &got, nullptr) || !got) { ok = false; break; }
        off += got;
    }
    CloseHandle(h);
    return ok;
}
bool WriteFileBytes(const std::wstring& p, const std::string& data) {
    EnsureDir(ParentDir(p));
    std::wstring tmp = p + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = data.empty() || (WriteFile(h, data.data(), DWORD(data.size()), &put, nullptr) && put == data.size());
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) { DeleteFileW(tmp.c_str()); return false; }
    return MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}
bool FilesEqual(const std::wstring& a, const std::wstring& b) {
    if (!FileExists(a) || !FileExists(b) || FileSize(a) != FileSize(b)) return false;
    std::string ha, hb;
    return Sha256File(a, ha) && Sha256File(b, hb) && ha == hb;
}

// ================================================================== SHA-256 (CNG)
namespace {
struct Sha256 {
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE h = nullptr;
    Sha256() { if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0); }
    ~Sha256() { if (h) BCryptDestroyHash(h); if (alg) BCryptCloseAlgorithmProvider(alg, 0); }
    void add(const void* p, size_t n) { BCryptHashData(h, (PUCHAR)p, ULONG(n), 0); }
    std::string hex() {
        unsigned char d[32]; BCryptFinishHash(h, d, 32, 0);
        static const char* x = "0123456789ABCDEF";
        std::string s; for (unsigned char c : d) { s += x[c >> 4]; s += x[c & 15]; }
        return s;
    }
};
}
std::string Sha256Hex(const void* data, size_t n) { Sha256 s; s.add(data, n); return s.hex(); }
bool Sha256File(const std::wstring& p, std::string& hexOut, std::atomic<bool>* cancel) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    Sha256 s;
    std::vector<char> buf(1 << 20);
    DWORD got = 0; bool ok = true;
    while (ReadFile(h, buf.data(), DWORD(buf.size()), &got, nullptr) && got) {
        s.add(buf.data(), got);
        if (cancel && cancel->load()) { ok = false; break; }
    }
    CloseHandle(h);
    if (ok) hexOut = s.hex();
    return ok;
}

// ================================================================== manifests
Manifest ReadManifest(const std::wstring& dir) {
    Manifest m;
    std::string text;
    if (!ReadFileBytes(JoinPath(dir, L"manifest.txt"), text)) return m;
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF) text = text.substr(3);
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t e = text.find('\n', pos);
        std::string line = text.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        pos = e == std::string::npos ? text.size() + 1 : e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("file ", 0) == 0) {
            std::string rest = line.substr(5);
            size_t a = rest.find(' '), b = a == std::string::npos ? a : rest.find(' ', a + 1);
            if (b == std::string::npos) continue;
            ManifestFile f;
            f.sha = rest.substr(0, a);
            f.size = _strtoui64(rest.substr(a + 1, b - a - 1).c_str(), nullptr, 10);
            f.rel = Widen(rest.substr(b + 1));
            m.files.push_back(f);
        } else {
            size_t i = line.find('=');
            if (i != std::string::npos) m.kv[line.substr(0, i)] = line.substr(i + 1);
        }
    }
    m.valid = true;
    return m;
}
std::wstring CheckManifestFiles(const std::wstring& dir, const Manifest& m, bool fullHash) {
    if (!m.valid) return L"no manifest.txt";
    std::wstring bad;
    int n = 0;
    for (auto& f : m.files) {
        std::wstring p = JoinPath(dir, f.rel), why;
        if (!FileExists(p)) why = L"missing";
        else if (FileSize(p) != f.size) why = L"size";
        else if (fullHash) { std::string h; if (!Sha256File(p, h) || _stricmp(h.c_str(), f.sha.c_str()) != 0) why = L"hash"; }
        if (!why.empty()) { if (n++ < 5) bad += (bad.empty() ? L"" : L", ") + f.rel + L" " + why; }
    }
    if (n > 5) bad += Format(L" (and %d more)", n - 5);
    return bad;
}

// ================================================================== registry
std::wstring RegGetStr(HKEY root, const std::wstring& key, const std::wstring& name, const std::wstring& def) {
    DWORD type = 0, size = 0;
    if (RegGetValueW(root, key.c_str(), name.empty() ? nullptr : name.c_str(), RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, &type, nullptr, &size) != ERROR_SUCCESS) return def;
    std::wstring s(size / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(root, key.c_str(), name.empty() ? nullptr : name.c_str(), RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, &type, s.data(), &size) != ERROR_SUCCESS) return def;
    s.resize(wcslen(s.c_str()));
    return s;
}
DWORD RegGetDword(HKEY root, const std::wstring& key, const std::wstring& name, DWORD def) {
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(root, key.c_str(), name.c_str(), RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS) return def;
    return v;
}
bool RegHasValue(HKEY root, const std::wstring& key, const std::wstring& name) {
    return RegGetValueW(root, key.c_str(), name.c_str(), RRF_RT_ANY, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}
bool RegSetStr(HKEY root, const std::wstring& key, const std::wstring& name, const std::wstring& v) {
    return RegSetKeyValueW(root, key.c_str(), name.c_str(), REG_SZ, v.c_str(), DWORD((v.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}
bool RegSetDword(HKEY root, const std::wstring& key, const std::wstring& name, DWORD v) {
    return RegSetKeyValueW(root, key.c_str(), name.c_str(), REG_DWORD, &v, sizeof(v)) == ERROR_SUCCESS;
}
bool RegDeleteTreeKey(HKEY root, const std::wstring& key) {
    LSTATUS r = RegDeleteTreeW(root, key.c_str());
    if (r == ERROR_FILE_NOT_FOUND) return true;
    if (r != ERROR_SUCCESS) return false;
    RegDeleteKeyW(root, key.c_str());
    return true;
}

// ================================================================== versions
namespace {
std::vector<long long> Numbers(const std::string& s) {
    std::vector<long long> v; long long cur = -1;
    for (char c : s) {
        if (c >= '0' && c <= '9') cur = (cur < 0 ? 0 : cur * 10) + (c - '0');
        else if (cur >= 0) { v.push_back(cur); cur = -1; }
    }
    if (cur >= 0) v.push_back(cur);
    return v;
}
int CompareNums(const std::vector<long long>& a, const std::vector<long long>& b) {
    for (size_t i = 0; i < std::max(a.size(), b.size()); i++) {
        long long x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}
}
int CompareVersions(const std::string& a0, const std::string& b0) {
    auto split = [](const std::string& v, std::string& main, std::string& pre, std::string& build) {
        std::string s = v; if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s = s.substr(1);
        size_t plus = s.find('+'); build = plus == std::string::npos ? "" : s.substr(plus + 1);
        std::string mp = s.substr(0, plus);
        size_t dash = mp.find('-'); pre = dash == std::string::npos ? "" : mp.substr(dash + 1);
        main = mp.substr(0, dash);
    };
    std::string am, ap, ab, bm, bp, bb;
    split(a0, am, ap, ab); split(b0, bm, bp, bb);
    if (int c = CompareNums(Numbers(am), Numbers(bm))) return c;
    if (ap.empty() != bp.empty()) return ap.empty() ? 1 : -1;      // 0.6.0 > 0.6.0-beta.1
    if (int c = CompareNums(Numbers(ap), Numbers(bp))) return c;
    return CompareNums(Numbers(ab), Numbers(bb));                  // +pushfps4.3.2 > +pushfps4.3.1
}

// ================================================================== log
std::wstring Log::path_;
static std::mutex g_logLock;
static std::function<void(const std::wstring&)> g_echo;
void Log::Open(const std::wstring& path) {
    path_ = path;
    EnsureDir(ParentDir(path));
    if (FileSize(path) > (2u << 20)) {
        std::wstring old = path.substr(0, path.size() - 4) + L".old.log";
        MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
}
void Log::SetEcho(std::function<void(const std::wstring&)> f) { g_echo = std::move(f); }
void Log::Write(const std::wstring& line) {
    std::lock_guard<std::mutex> lk(g_logLock);
    if (g_echo) g_echo(line);
    if (path_.empty()) return;
    SYSTEMTIME st; GetLocalTime(&st);
    std::string s = Narrow(Format(L"%04u-%02u-%02u %02u:%02u:%02u [%lu] ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId()) + line) + "\r\n";
    HANDLE h = CreateFileW(path_.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD put = 0; WriteFile(h, s.data(), DWORD(s.size()), &put, nullptr);
    CloseHandle(h);
}
void Log::Writef(const wchar_t* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    wchar_t buf[4096];
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    Write(buf);
}

// ================================================================== processes
namespace {
bool IsPlayerName(const wchar_t* exe) {
    // the players whose settings PushMPC changes (closed by Setup: each rewrites its settings when it closes)
    static const wchar_t* names[] = { L"mpc-hc64.exe", L"mpc-hc.exe", L"mpc-be64.exe", L"mpc-be.exe", L"PotPlayerMini64.exe", L"PotPlayer64.exe" };
    for (auto n : names) if (!_wcsicmp(exe, n)) return true;
    return false;
}
bool IsMpvFamilyName(const wchar_t* exe) {
    // the mpv route (vsplugin): these load pushmpc_vs.dll and pushcore.dll; not closed by Setup (mpv does not rewrite
    // mpv.conf, and the script leaves smplayer.ini alone while SMPlayer runs), but held files count as in use
    static const wchar_t* names[] = { L"mpv.exe", L"mpvnet.exe", L"smplayer.exe", L"mpc-qt.exe" };
    for (auto n : names) if (!_wcsicmp(exe, n)) return true;
    return false;
}
DWORD SessionOf(DWORD pid) { DWORD s = 0; ProcessIdToSessionId(pid, &s); return s; }
}
std::vector<Holder> FindHolders(const std::vector<std::wstring>& dirs0) {
    std::vector<std::wstring> dirs;
    for (auto d : dirs0) { if (!d.empty() && d.back() != L'\\') d += L'\\'; dirs.push_back(d); }
    std::vector<Holder> out;
    DWORD me = GetCurrentProcessId(), mySession = SessionOf(me);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{ sizeof(pe) };
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID == me || pe.th32ProcessID <= 4) continue;
        bool player = IsPlayerName(pe.szExeFile) || IsMpvFamilyName(pe.szExeFile);
        HANDLE ms = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pe.th32ProcessID);
        if (ms == INVALID_HANDLE_VALUE) {
            if (player) out.push_back({ pe.th32ProcessID, pe.szExeFile, L"(modules not readable)" });
            continue;
        }
        MODULEENTRY32W me32{ sizeof(me32) };
        for (BOOL mo = Module32FirstW(ms, &me32); mo; mo = Module32NextW(ms, &me32)) {
            bool hit = false;
            for (auto& d : dirs) if (StartsWithI(me32.szExePath, d)) { hit = true; break; }
            if (hit) { out.push_back({ pe.th32ProcessID, pe.szExeFile, me32.szExePath }); break; }
        }
        CloseHandle(ms);
    }
    CloseHandle(snap);
    (void)mySession;
    return out;
}
std::vector<DWORD> FindPlayerProcesses() {
    std::vector<DWORD> out;
    DWORD mySession = SessionOf(GetCurrentProcessId());
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{ sizeof(pe) };
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (IsPlayerName(pe.szExeFile) && SessionOf(pe.th32ProcessID) == mySession) out.push_back(pe.th32ProcessID);
    CloseHandle(snap);
    return out;
}
bool ClosePlayersGracefully(DWORD waitMs) {
    std::vector<DWORD> pids = FindPlayerProcesses();
    if (pids.empty()) return true;
    struct Ctx { std::vector<DWORD>* pids; } ctx{ &pids };
    EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
        if (std::find(c->pids->begin(), c->pids->end(), pid) != c->pids->end() && IsWindowVisible(w) && !GetWindow(w, GW_OWNER))
            PostMessageW(w, WM_CLOSE, 0, 0);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    ULONGLONG end = GetTickCount64() + waitMs;
    for (DWORD pid : pids) {
        HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (!h) continue;
        ULONGLONG now = GetTickCount64();
        WaitForSingleObject(h, now < end ? DWORD(end - now) : 0);
        CloseHandle(h);
    }
    return FindPlayerProcesses().empty();
}
std::wstring QuoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    size_t bs = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { bs++; continue; }
        if (c == L'"') { q.append(bs * 2 + 1, L'\\'); q += L'"'; bs = 0; continue; }
        q.append(bs, L'\\'); bs = 0; q += c;
    }
    q.append(bs * 2, L'\\');
    return q + L"\"";
}
std::wstring SystemExe(const wchar_t* name) {
    wchar_t sys[MAX_PATH]{}; GetSystemDirectoryW(sys, MAX_PATH);
    return JoinPath(sys, name);
}
std::wstring PowerShellExe() { return SystemExe(L"WindowsPowerShell\\v1.0\\powershell.exe"); }

int RunHidden(const std::wstring& cmdline, const std::function<void(const std::string&)>& onLine, HANDLE cancelEvent, const std::wstring& workDir) {
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nul; si.hStdOutput = wr; si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = cmdline;
    // Windows PowerShell 5.1 runs the install script: a PSModulePath inherited from PowerShell 7 (Setup or the updater
    // started from pwsh) makes 5.1 load 7's copies of its own modules, which fail (Get-AuthenticodeSignature not found).
    // Without the variable, 5.1 uses its defaults; nothing PushMPC starts needs another value.
    SetEnvironmentVariableW(L"PSModulePath", nullptr);
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr,
                             workDir.empty() ? nullptr : workDir.c_str(), &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) { CloseHandle(rd); return -1; }
    CloseHandle(pi.hThread);
    std::string pending;
    char buf[4096];
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr)) break;   // broken pipe: the child (and its children) closed it
        if (avail) {
            DWORD got = 0;
            if (!ReadFile(rd, buf, std::min<DWORD>(avail, sizeof(buf)), &got, nullptr) || !got) break;
            pending.append(buf, got);
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, nl);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                pending.erase(0, nl + 1);
                if (onLine) onLine(line);
            }
            continue;
        }
        if (cancelEvent && WaitForSingleObject(cancelEvent, 0) == WAIT_OBJECT_0) { TerminateProcess(pi.hProcess, 1223); break; }
        if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
            // drain what is left
            while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail) {
                DWORD got = 0;
                if (!ReadFile(rd, buf, std::min<DWORD>(avail, sizeof(buf)), &got, nullptr) || !got) break;
                pending.append(buf, got);
            }
            break;
        }
    }
    size_t nl;
    while ((nl = pending.find('\n')) != std::string::npos) {
        std::string line = pending.substr(0, nl);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        pending.erase(0, nl + 1);
        if (onLine) onLine(line);
    }
    if (!pending.empty() && onLine) onLine(pending);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    return int(code);
}

} // namespace pm
