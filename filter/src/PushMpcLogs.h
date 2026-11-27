// PushMpcLogs.h: the filter's old logs go (F-LOG: nothing was ever deleted; one folder held 1702 files / 205 MB).
// Header-only so filter/tools/logs_test.cpp checks the same code on a temporary folder.
#pragma once

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace pm::logs {

// The filter's own files only (filter-*.log, filter-frames-*.csv, filter-gpu-*.csv, filter-exit-*.log,
// diagnostics-*.txt, the frames-*.csv / gpu-*.csv of builds before 0.6, and PushFPS's engine log of the filter's
// pushcore, pushcore-engine-filter-<stamp>.log since F-T1: one per process, so a hang run's engine log survives the
// next start; Setup's, the updater's and the other players' logs stay), newest first, kept while within keepSessions
// playback sessions and keepBytes. A session is the files that share one stamp's process id (date-time-process id;
// filter-exit-<pid>.log goes with its process); a file without one is a session of its own (the single
// pushcore-engine-filter.log of older builds). At most maxDeletes deletions per start, so a huge backlog shrinks over
// a few starts without delaying one. Returns the number of files deleted.
inline int PruneLogs(const std::wstring& dir, size_t keepSessions = 20, uint64_t keepBytes = 50ull << 20, int maxDeletes = 400)
{
    struct F { std::wstring name, key; uint64_t bytes; FILETIME t; };
    std::vector<F> files;
    for (const wchar_t* pat : { L"filter-*.log", L"filter-frames-*.csv", L"filter-gpu-*.csv", L"diagnostics-*.txt", L"frames-*.csv", L"gpu-*.csv",
                                L"pushcore-engine-filter*.log" }) {
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((dir + L"\\" + pat).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::wstring n = fd.cFileName;
            // the session key: the trailing "-<pid>" of the stamp (yyyymmdd-hhmmss-pid), so filter-exit-<pid>.log
            // joins its process's files; a file without one is a session of its own
            std::wstring key = n;
            const size_t dot = n.find_last_of(L'.'), dash = n.find_last_of(L'-', dot);
            if (dot != std::wstring::npos && dash != std::wstring::npos && dash + 1 < dot) {
                const std::wstring pid = n.substr(dash + 1, dot - dash - 1);
                if (pid.find_first_not_of(L"0123456789") == std::wstring::npos) key = pid;
            }
            files.push_back({ n, key, (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow, fd.ftLastWriteTime });
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(files.begin(), files.end(), [](const F& a, const F& b) { return CompareFileTime(&a.t, &b.t) > 0; });
    std::vector<std::wstring> sessions;
    uint64_t bytes = 0;
    int deleted = 0;
    for (const F& f : files) {
        const bool known = std::find(sessions.begin(), sessions.end(), f.key) != sessions.end();
        if (!known && sessions.size() < keepSessions && bytes + f.bytes <= keepBytes) sessions.push_back(f.key);
        if ((known || std::find(sessions.begin(), sessions.end(), f.key) != sessions.end()) && bytes + f.bytes <= keepBytes) { bytes += f.bytes; continue; }
        if (deleted >= maxDeletes) break;
        if (DeleteFileW((dir + L"\\" + f.name).c_str())) ++deleted;
    }
    return deleted;
}

}  // namespace pm::logs
