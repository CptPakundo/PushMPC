// logs_test: the log rotation (filter/src/PushMpcLogs.h) on a temporary folder: per-process engine logs
// (pushcore-engine-filter-<stamp>-<pid>.log, F-T1) go with their process's filter logs, the newest sessions stay within
// the count and size bounds, older ones go, other programs' logs stay. CPU only. Exit code 0 = all passed.
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <format>
#include <string>

#include "PushMpcLogs.h"

static int g_fail = 0, g_checks = 0;
static void Check(bool ok, const std::string& what)
{
    ++g_checks;
    if (!ok) ++g_fail;
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
}
static std::wstring g_dir;
static void Make(const std::wstring& name, size_t bytes, int ageMinutes)
{
    const std::wstring p = g_dir + L"\\" + name;
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string data(bytes, 'x');
    DWORD w = 0;
    if (bytes) WriteFile(h, data.data(), DWORD(bytes), &w, nullptr);
    FILETIME now; GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER t; t.LowPart = now.dwLowDateTime; t.HighPart = now.dwHighDateTime;
    t.QuadPart -= uint64_t(ageMinutes) * 60ull * 10000000ull;
    FILETIME ft; ft.dwLowDateTime = t.LowPart; ft.dwHighDateTime = t.HighPart;
    SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
}
static bool Exists(const std::wstring& name) { return GetFileAttributesW((g_dir + L"\\" + name).c_str()) != INVALID_FILE_ATTRIBUTES; }
static int Count(const wchar_t* pat)
{
    int n = 0; WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((g_dir + L"\\" + pat).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) ++n; while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}
static void Clear()
{
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((g_dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((g_dir + L"\\" + fd.cFileName).c_str()); } while (FindNextFileW(h, &fd));
    FindClose(h);
}
// one playback session of process pid, ageMinutes old: the filter log, its frames CSV and its engine log
static std::wstring Stamp(int i, int pid) { return std::format(L"20261127-{:02}{:02}00-{}", 6 + i / 60, i % 60, pid); }
static void Session(int i, int pid, int ageMinutes, size_t engineBytes = 1000)
{
    Make(L"filter-" + Stamp(i, pid) + L".log", 2000, ageMinutes);
    Make(L"filter-frames-" + Stamp(i, pid) + L".csv", 3000, ageMinutes);
    Make(L"pushcore-engine-filter-" + Stamp(i, pid) + L".log", engineBytes, ageMinutes);
}

int main()
{
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    g_dir = std::wstring(tmp) + std::format(L"pushmpc-logs-test-{}", GetCurrentProcessId());
    CreateDirectoryW(g_dir.c_str(), nullptr);

    {   // 1. 25 sessions, keep 20: the 5 oldest go with their engine logs; the newest 20 keep theirs
        Clear();
        for (int i = 0; i < 25; ++i) Session(i, 1000 + i, 25 - i);   // i = 24 newest
        Make(L"pushcore-engine-vsplugin.log", 500, 999);               // another program's engine log
        Make(L"PushMpcSetup.log", 500, 999);
        const int del = pm::logs::PruneLogs(g_dir, 20, 50ull << 20, 400);
        Check(del == 15, std::format("count bound: 15 files of the 5 oldest sessions deleted ({})", del));
        bool newestKept = true, oldestGone = true;
        for (int i = 5; i < 25; ++i) newestKept &= Exists(L"pushcore-engine-filter-" + Stamp(i, 1000 + i) + L".log") && Exists(L"filter-" + Stamp(i, 1000 + i) + L".log");
        for (int i = 0; i < 5; ++i) oldestGone &= !Exists(L"pushcore-engine-filter-" + Stamp(i, 1000 + i) + L".log") && !Exists(L"filter-" + Stamp(i, 1000 + i) + L".log");
        Check(newestKept, "count bound: every kept session keeps its engine log (one per process, never overwritten)");
        Check(oldestGone, "count bound: an old session's engine log goes with its filter log");
        Check(Exists(L"pushcore-engine-vsplugin.log") && Exists(L"PushMpcSetup.log"), "other programs' logs stay");
    }
    {   // 2. the size bound: big engine logs push older sessions out before the count does
        Clear();
        for (int i = 0; i < 10; ++i) Session(i, 2000 + i, 10 - i, 300 * 1024);   // ~305 KB per session
        pm::logs::PruneLogs(g_dir, 20, 1024 * 1024, 400);                        // 1 MB: 3 sessions fit
        Check(Count(L"pushcore-engine-filter-*.log") == 3, std::format("size bound: 3 of 10 engine logs kept within 1 MB ({})", Count(L"pushcore-engine-filter-*.log")));
        Check(Exists(L"pushcore-engine-filter-" + Stamp(9, 2009) + L".log") && !Exists(L"pushcore-engine-filter-" + Stamp(6, 2006) + L".log"), "size bound: the newest kept, the older gone");
    }
    {   // 3. the single engine log of older builds is a session of its own; the newest file of each kind stays
        Clear();
        Make(L"pushcore-engine-filter.log", 1000, 100);
        for (int i = 0; i < 3; ++i) Session(i, 3000 + i, 50 - i);
        pm::logs::PruneLogs(g_dir, 3, 50ull << 20, 400);
        Check(!Exists(L"pushcore-engine-filter.log") && Count(L"pushcore-engine-filter-*.log") == 3, "older builds' single engine log: pruned as its own (oldest) session");
        Clear();
        Make(L"pushcore-engine-filter.log", 1000, 1);
        for (int i = 0; i < 3; ++i) Session(i, 3000 + i, 50 - i);
        pm::logs::PruneLogs(g_dir, 3, 50ull << 20, 400);
        Check(Exists(L"pushcore-engine-filter.log") && Count(L"pushcore-engine-filter-*.log") == 2, "older builds' single engine log: kept while it is among the newest sessions");
    }
    {   // 4. a process's later sessions share its key (same pid): kept together
        Clear();
        Session(0, 4000, 30); Session(1, 4000, 20); Session(2, 4001, 10);
        Make(L"filter-exit-4000.log", 100, 5);
        pm::logs::PruneLogs(g_dir, 2, 50ull << 20, 400);
        Check(Count(L"*") == 10, std::format("same process: all its files and its exit log kept as one session ({} files)", Count(L"*")));
    }
    {   // 5. at most maxDeletes per start
        Clear();
        for (int i = 0; i < 30; ++i) Session(i, 5000 + i, 30 - i);
        const int del = pm::logs::PruneLogs(g_dir, 5, 50ull << 20, 10);
        Check(del == 10 && Count(L"*") == 80, std::format("deletion cap: 10 per start ({} deleted, {} left)", del, Count(L"*")));
    }
    Clear();
    RemoveDirectoryW(g_dir.c_str());
    std::printf("RESULT %s: %d of %d checks passed\n", g_fail ? "FAIL" : "PASS", g_checks - g_fail, g_checks);
    return g_fail ? 1 : 0;
}
