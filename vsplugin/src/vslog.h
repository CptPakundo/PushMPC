// pushmpc.Enhance log: %LOCALAPPDATA%\PushMPC\logs\vsplugin-<date>-<time>-<pid>.log, one file per process (the
// warm core outlives the script reloads, so one file follows a whole mpv session). PUSHMPC_VS_LOG_DIR overrides the
// folder (tests); PUSHMPC_VS_LOG=0 turns the file off.
// Linux / macOS (the portable backend): the folder from platform.h (XDG state or ~/Library/Logs), same file name.
#pragma once
#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <share.h>
#include <format>
#include <mutex>
#include <string>

namespace pmvs {

inline std::wstring LocalAppDataDir()
{
    PWSTR p = nullptr;
    std::wstring d;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)) && p) { d = p; CoTaskMemFree(p); }
    return d;
}
inline std::wstring EnvStr(const wchar_t* name)
{
    wchar_t b[1024] = {};
    const DWORD n = GetEnvironmentVariableW(name, b, 1024);
    return n && n < 1024 ? std::wstring(b, n) : std::wstring();
}
inline std::wstring LogFolder()
{
    std::wstring d = EnvStr(L"PUSHMPC_VS_LOG_DIR");
    if (d.empty()) {
        d = LocalAppDataDir();
        if (d.empty()) return {};
        d += L"\\PushMPC"; CreateDirectoryW(d.c_str(), nullptr);
        d += L"\\logs";
    }
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}
inline std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
inline std::wstring Widen(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}
struct LogState { std::mutex lock; FILE* f = nullptr; bool tried = false; std::wstring path; };
inline LogState& LogS() { static LogState* s = new LogState; return *s; } // never destroyed: logging works up to process exit
inline std::wstring LogPath() { std::lock_guard<std::mutex> lk(LogS().lock); return LogS().path; }
inline void Log(const std::string& line)
{
    LogState& s = LogS();
    std::lock_guard<std::mutex> lk(s.lock);
    if (!s.tried) {
        s.tried = true;
        if (EnvStr(L"PUSHMPC_VS_LOG") != L"0") {
            const std::wstring dir = LogFolder();
            SYSTEMTIME st; GetLocalTime(&st);
            if (!dir.empty()) {
                s.path = std::format(L"{}\\vsplugin-{:04}{:02}{:02}-{:02}{:02}{:02}-{}.log", dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId());
                s.f = _wfsopen(s.path.c_str(), L"ab", _SH_DENYNO);   // shared: readable while the player runs (and by core_test)
            }
        }
    }
    SYSTEMTIME st; GetLocalTime(&st);
    const std::string l = std::format("{:02}:{:02}:{:02}.{:03} [{}] {}\r\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentThreadId(), line);
    if (s.f) { fwrite(l.data(), 1, l.size(), s.f); fflush(s.f); }
    OutputDebugStringA(l.c_str());
}

} // namespace pmvs
#else
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <cstdio>
#include <ctime>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include "platform.h"

namespace pmvs {

inline std::string Narrow(const std::string& s) { return s; }   // POSIX: UTF-8 throughout
inline std::string Widen(const std::string& s) { return s; }
inline void MakeDirs(const std::string& d)
{
    for (size_t i = 1; i <= d.size(); ++i)
        if (i == d.size() || d[i] == '/') mkdir(d.substr(0, i).c_str(), 0755);
}
inline std::string LogFolder()
{
#ifdef __APPLE__
    const bool mac = true;
#else
    const bool mac = false;
#endif
    const std::string d = ResolvePosixLogDir(EnvA("PUSHMPC_VS_LOG_DIR"), EnvA("XDG_STATE_HOME"), EnvA("HOME"), mac);
    if (!d.empty()) MakeDirs(d);
    return d;
}
struct LogState { std::mutex lock; FILE* f = nullptr; bool tried = false; std::string path; };
inline LogState& LogS() { static LogState* s = new LogState; return *s; } // never destroyed: logging works up to process exit
inline std::string LogPath() { std::lock_guard<std::mutex> lk(LogS().lock); return LogS().path; }
inline void Log(const std::string& line)
{
    LogState& s = LogS();
    std::lock_guard<std::mutex> lk(s.lock);
    timeval tv; gettimeofday(&tv, nullptr);
    std::tm lt{}; const time_t secs = tv.tv_sec; localtime_r(&secs, &lt);
    if (!s.tried) {
        s.tried = true;
        if (EnvA("PUSHMPC_VS_LOG") != "0") {
            const std::string dir = LogFolder();
            if (!dir.empty()) {
                s.path = std::format("{}/vsplugin-{:04}{:02}{:02}-{:02}{:02}{:02}-{}.log", dir, lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec, int(getpid()));
                s.f = std::fopen(s.path.c_str(), "ab");
            }
        }
    }
    const std::string l = std::format("{:02}:{:02}:{:02}.{:03} [{}] {}\n", lt.tm_hour, lt.tm_min, lt.tm_sec, int(tv.tv_usec / 1000),
                                      std::hash<std::thread::id>{}(std::this_thread::get_id()) % 100000, line);
    if (s.f) { std::fwrite(l.data(), 1, l.size(), s.f); std::fflush(s.f); }
}

} // namespace pmvs
#endif

namespace pmvs {
// arError: VapourSynth failed output n before the filter ran, because a source frame it requested failed. Under mpv
// that is a request past the end of the stream (its source does not know its length and errors there), e.g. a Dedup
// lookahead frame. The first few are logged (none was before: such frames left no trace).
inline void LogSourceError(const char* who, int n, int64_t from, uint64_t& count)
{
    if (++count <= 3)
        Log(std::string(who) + ": output frame " + std::to_string(n) + " failed: a source frame it requested (from " + std::to_string(from) +
            " on) failed (the end of a stream of unknown length, or the source's own error)");
}
} // namespace pmvs
