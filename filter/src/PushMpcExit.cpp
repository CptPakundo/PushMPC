// PushMpcExit.dll: ends a process that is already exiting without running the remaining DLL detach handlers.
//
// Why: the warm NGX cache (pushcore keeps the NGX core initialised between files of one MPC-HC session) is still alive
// when MPC-HC calls ExitProcess. The loader then runs DLL_PROCESS_DETACH of every DLL in reverse initialisation order,
// and the NGX feature DLLs shut NGX down from there: under the loader lock, with every other thread (driver workers
// included) already terminated, that shutdown creates a D3D12 device and waits in the driver (stack:
// ntdll!LdrShutdownProcess -> 160_E658703 (NGX) -> D3D12CoreCreateLayeredDevice -> nvwgf2umx -> NtGdiDdDDIUnlock2).
// It took 8-18 s in some closes, after the graph was long gone.
//
// How: the filter (re)loads this DLL after each pushcore open, so it is initialised after pushcore, NGX and CUDA and
// its detach runs before theirs. On DLL_PROCESS_DETACH with lpReserved != NULL (process termination: the exe's CRT
// has already run its atexit handlers and static destructors, i.e. MPC-HC has saved its settings) it terminates the
// process with the exit code given to ExitProcess. The OS reclaims memory, handles and GPU allocations. A FreeLibrary
// (lpReserved == NULL) does nothing.
#include <windows.h>

static wchar_t g_log[MAX_PATH] = {};
// the last teardown step the filter reported (PushMpcExit_Mark) and when: the exit line says how long after it the
// process reached its DLL detach (RC1 F7: 1.8 s between "filter destroyed" and this line in one 9.2-s close)
static char g_mark[96] = {};
static volatile LONG64 g_markQpc = 0, g_firstMarkQpc = 0;

extern "C" __declspec(dllexport) void PushMpcExit_SetLog(const wchar_t* path)
{
    if (path) lstrcpynW(g_log, path, MAX_PATH);
}
extern "C" __declspec(dllexport) void PushMpcExit_Mark(const char* what)
{
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    if (what) lstrcpynA(g_mark, what, sizeof(g_mark));
    InterlockedExchange64(&g_markQpc, q.QuadPart);
    InterlockedCompareExchange64(&g_firstMarkQpc, q.QuadPart, 0);
}

static void LogLine(DWORD code)
{
    if (!g_log[0]) return;
    HANDLE f = CreateFileW(g_log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME t; GetLocalTime(&t);
    LARGE_INTEGER q, fr; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&fr);
    const LONG64 mq = g_markQpc, fq = g_firstMarkQpc;
    const unsigned long sinceMark = mq ? (unsigned long)((q.QuadPart - mq) * 1000 / fr.QuadPart) : 0;
    const unsigned long sinceFirst = fq ? (unsigned long)((q.QuadPart - fq) * 1000 / fr.QuadPart) : 0;
    // what is still alive at this point (a slow kernel-side exit afterwards scales with GPU allocations and handles)
    DWORD handles = 0; GetProcessHandleCount(GetCurrentProcess(), &handles);
    FILETIME c0, e0, k0, u0; ULARGE_INTEGER k = {}, u = {};
    if (GetProcessTimes(GetCurrentProcess(), &c0, &e0, &k0, &u0)) { k.LowPart = k0.dwLowDateTime; k.HighPart = k0.dwHighDateTime; u.LowPart = u0.dwLowDateTime; u.HighPart = u0.dwHighDateTime; }
    char b[512];
    const int n = wsprintfA(b, "%02u:%02u:%02u.%03u [%lu] qpc %lu ms process exit: remaining DLL detach skipped (TerminateProcess, exit code %lu); "
        "%lu ms after \"%s\" (%lu ms after the first teardown step); %lu handles, CPU kernel %lu ms user %lu ms\r\n",
        t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId(), (unsigned long)(q.QuadPart * 1000 / fr.QuadPart), code,
        sinceMark, g_mark[0] ? g_mark : "no teardown step reported", sinceFirst, handles, (unsigned long)(k.QuadPart / 10000), (unsigned long)(u.QuadPart / 10000));
    DWORD w = 0;
    WriteFile(f, b, DWORD(n), &w, nullptr);
    CloseHandle(f);
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(h);
    if (reason == DLL_PROCESS_DETACH && reserved) {
        DWORD code = 0;
        if (!GetExitCodeProcess(GetCurrentProcess(), &code) || code == STILL_ACTIVE) code = 0;
        LogLine(code);
        TerminateProcess(GetCurrentProcess(), code);
    }
    return TRUE;
}
