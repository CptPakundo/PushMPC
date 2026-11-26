// PushMpcFilter.ax: the PushMPC DirectShow filter (design and measurements: docs in the repository).
//
//   LAV Video (NV12/P010) -> [this filter] -> stock MPC Video Renderer (D3D11 mode)
//
// Processed mode (MPC-VR in D3D11 mode downstream, NVIDIA, pushcore usable):
// - The filter creates its own D3D11 device and hands it to MPC-VR (ID3D11DecoderConfiguration::ActivateD3D11Decoding,
//   LAV's handshake). Output samples are IMediaSampleD3D11 textures from the filter's own allocator: BGRA (SDR) or
//   Y410 (HDR10: PushFPS's R10G10B10A2 PQ BT.2020 converted to Y'CbCr BT.2020 on the GPU, colour info in the media
//   type, the source's HDR side data on the samples), so MPC-VR does its own HDR passthrough or tone mapping.
// - Input frames are uploaded into a dynamic texture and submitted to pushcore (PushFPS's VSR + VFG). A copier thread
//   draws pushcore's outputs into the sample textures; a deliverer thread calls Deliver shortly before each frame's
//   time (stock MPC-VR then presents at once; see DelivererLoop), drops late generated frames and runs the frame
//   generation admission (PushMPC M2c).
// - While pushcore opens or reloads, the source frames are converted and delivered at the source rate.
// - The output size and format follow pushcore (dynamic media type change on the sample, QueryAccept first).
// Pass-through: anything else (not NVIDIA, pushcore missing/version skew, switch off, interlaced, Dolby Vision RPU,
// another renderer) connects in the source format and copies the frames through; at run time a fault switches to the
// source format on D3D11 samples. The reason is in the log, the property page and the status line.
#include <streams.h>
#include <initguid.h>
#include <dvdmedia.h>
#include <d3d9.h>
#include <dxva2api.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <shlobj.h>
#include <KnownFolders.h>
#include <mmsystem.h>
#include <avrt.h>
#include <tlhelp32.h>
#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <format>
#include <map>
#include <set>
#include <type_traits>
#ifndef WM_DPICHANGED_AFTERPARENT
#define WM_DPICHANGED_AFTERPARENT 0x02E3
#endif
#include <utility>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <cmath>
#include <cstdio>
#include "ID3DVideoMemoryConfiguration.h"
#include "IMediaSideData.h"
#include "pushmpc/pushcore_abi.h"
#include "pushmpc/unlocks.h"            // T5b: vendor unlocks (off switches, self-test results)
#include "pushmpc/content_detect.h"     // what the picture is (class per shot), interlacing and the frame interval, measured
#include "resource.h"
#include "version.h"
#include "PushMpcSettings.h"
#include "PushMpcShows.h"
#include "PushMpcAuto.h"
#include <commctrl.h>
#include <shellapi.h>
#include <fstream>
#include "PushMpcExtras.h"
#include "PushMpcFastStart.h"
#include <VersionHelpers.h>
#include <IVideoRenderer.h>   // MPC Video Renderer's settings interface (live HDR settings)
#include <sstream>

using Microsoft::WRL::ComPtr;

// {798D4EC3-E59C-4A4E-A2EE-C804AEAE04CC}
DEFINE_GUID(CLSID_PushMpcFilter, 0x798d4ec3, 0xe59c, 0x4a4e, 0xa2, 0xee, 0xc8, 0x04, 0xae, 0xae, 0x04, 0xcc);
// {798D4EC4-E59C-4A4E-A2EE-C804AEAE04CC}
DEFINE_GUID(CLSID_PushMpcPage, 0x798d4ec4, 0xe59c, 0x4a4e, 0xa2, 0xee, 0xc8, 0x04, 0xae, 0xae, 0x04, 0xcc);
// {798D4EC5-E59C-4A4E-A2EE-C804AEAE04CC}: the About page
DEFINE_GUID(CLSID_PushMpcAboutPage, 0x798d4ec5, 0xe59c, 0x4a4e, 0xa2, 0xee, 0xc8, 0x04, 0xae, 0xae, 0x04, 0xcc);
DEFINE_GUID(MEDIASUBTYPE_P010_PM, 0x30313050, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);
DEFINE_GUID(MEDIASUBTYPE_Y410_PM, 0x30313459, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);
DEFINE_GUID(MEDIASUBTYPE_Y416_PM, 0x36313459, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);
static const wchar_t* kFilterName = L"PushMPC Filter";

// ------------------------------------------------------------------------------------------------ small helpers
static int64_t Qpc() { LARGE_INTEGER v; QueryPerformanceCounter(&v); return v.QuadPart; }
static double QpcFreq() { static const double f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return double(q.QuadPart); }(); return f; }
static double QpcMs(int64_t v) { return double(v) * 1000.0 / QpcFreq(); }

static std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
static std::wstring Widen(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}
static bool FileExists(const std::wstring& p) { const DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
static bool DirExists(const std::wstring& p) { const DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
static std::wstring TrimSlashes(std::wstring s) { while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back(); return s; }
static std::wstring ParentDir(const std::wstring& p) { const auto i = p.find_last_of(L"\\/"); return i == std::wstring::npos ? std::wstring() : p.substr(0, i); }
static bool ValidPushFpsFolder(const std::wstring& d) { return !d.empty() && FileExists(d + L"\\PushFPS.exe") && DirExists(d + L"\\shaders"); }
static std::wstring Version3(const std::wstring& v)
{
    int a = 0, b = 0, c = 0;
    if (swscanf_s(v.c_str(), L"%d.%d.%d", &a, &b, &c) >= 2) return std::format(L"{}.{}.{}", a, b, c);
    return v;
}
static std::wstring LocalAppData()
{
    PWSTR p = nullptr;
    std::wstring d;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)) && p) { d = p; CoTaskMemFree(p); }
    return d;
}

// ------------------------------------------------------------------------------------------------ log
// Old logs go (F-LOG: nothing was ever deleted; one folder held 1702 files / 205 MB): the filter's own files only
// (filter-*.log, filter-frames-*.csv, filter-gpu-*.csv, filter-exit-*.log, diagnostics-*.txt, and the frames-*.csv /
// gpu-*.csv of builds before 0.6; Setup's and the updater's
// logs stay), newest first, kept while within kKeepSessions playback sessions and kKeepBytes. A session is the files
// that share one stamp (date-time-process id; filter-exit-<pid>.log goes with its process). At most 400 deletions per
// start, so a huge backlog shrinks over a few starts without delaying one.
static void PruneLogs(const std::wstring& dir)
{
    constexpr size_t kKeepSessions = 20;
    constexpr uint64_t kKeepBytes = 50ull << 20;
    struct F { std::wstring name, key; uint64_t bytes; FILETIME t; };
    std::vector<F> files;
    for (const wchar_t* pat : { L"filter-*.log", L"filter-frames-*.csv", L"filter-gpu-*.csv", L"diagnostics-*.txt", L"frames-*.csv", L"gpu-*.csv" }) {
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
        if (!known && sessions.size() < kKeepSessions && bytes + f.bytes <= kKeepBytes) sessions.push_back(f.key);
        if ((known || std::find(sessions.begin(), sessions.end(), f.key) != sessions.end()) && bytes + f.bytes <= kKeepBytes) { bytes += f.bytes; continue; }
        if (deleted >= 400) break;
        if (DeleteFileW((dir + L"\\" + f.name).c_str())) ++deleted;
    }
}
static std::wstring LogDir()
{
    static const std::wstring dir = [] {
        std::wstring d = LocalAppData();
        if (d.empty()) d = L"C:\\Temp";
        d += L"\\PushMPC"; CreateDirectoryW(d.c_str(), nullptr);
        d += L"\\logs"; CreateDirectoryW(d.c_str(), nullptr);
        PruneLogs(d);
        return d;
    }();
    return dir;
}
static std::wstring Stamp()
{
    SYSTEMTIME st; GetLocalTime(&st);
    return std::format(L"{:04}{:02}{:02}-{:02}{:02}{:02}-{}", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId());
}
static std::mutex g_logLock;
static FILE* g_log = nullptr;
static void Log(const std::string& s)
{
    std::lock_guard<std::mutex> lk(g_logLock);
    if (!g_log) {
        const std::wstring path = LogDir() + L"\\filter-" + Stamp() + L".log";
        g_log = _wfsopen(path.c_str(), L"a", _SH_DENYWR);
    }
    if (!g_log) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_log, "%02d:%02d:%02d.%03d [%lu] qpc %.3f %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentThreadId(), QpcMs(Qpc()), s.c_str());
    fflush(g_log);
}
static void LogW(const std::wstring& s) { Log(Narrow(s)); }

// PushMpcExit.dll (next to this .ax): (re)loaded after every pushcore open so that it is initialised after pushcore,
// NGX and CUDA, and its DLL_PROCESS_DETACH runs before theirs at process exit, where it ends the process without
// the NGX shutdown that runs from the NGX DLLs' detach (see src/PushMpcExit.cpp). Unload + load moves it to the end
// of the loader's initialisation order; nothing else loads it.
using ExitMarkFn = void (*)(const char*);
static std::atomic<ExitMarkFn> g_exitMark = nullptr;
static std::mutex g_exitLock;   // the helper is unloaded and loaded again here: ExitMark takes it too
static void ArmExitHelper(const char* why = "loaded after pushcore and NGX")
{
    std::lock_guard<std::mutex> lk(g_exitLock);
    g_exitMark = nullptr;
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(g_hInst, self, MAX_PATH);
    std::wstring path = self;
    path = path.substr(0, path.find_last_of(L'\\') + 1) + L"PushMpcExit.dll";
    for (int i = 0; i < 16; i++) { HMODULE h = GetModuleHandleW(path.c_str()); if (!h) break; FreeLibrary(h); }
    HMODULE h = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) { Log(std::format("exit helper {} not loaded (error {}): an exit with the warm NGX cache alive runs NGX's own shutdown", Narrow(path), GetLastError())); return; }
    using SetLogFn = void (*)(const wchar_t*);
    if (auto f = reinterpret_cast<SetLogFn>(GetProcAddress(h, "PushMpcExit_SetLog")))
        f((LogDir() + std::format(L"\\filter-exit-{}.log", GetCurrentProcessId())).c_str());
    g_exitMark.store(reinterpret_cast<ExitMarkFn>(GetProcAddress(h, "PushMpcExit_Mark")));
    Log(std::format("exit helper armed ({})", why));
}
static bool ExitHelperArmed() { std::lock_guard<std::mutex> lk(g_exitLock); return g_exitMark.load() != nullptr; }
// the exit helper's timeline (F7): the last teardown step it is told about goes into its exit line ("N ms after ...")
static void ExitMark(const char* what)
{
    std::lock_guard<std::mutex> lk(g_exitLock);
    if (auto f = g_exitMark.load()) f(what);
}

// RC2 F2: slow closes with the DLSS-G fallback (one 10.3-s close ended by MPC-HC's 0xDEADBEEF watchdog with no line after
// "pushcore closed"; one 6.8-s close with 4.35 s between "filter members released" and the exit helper). Neither
// reproduced in 15 later closes, so a stuck close now explains itself: a watchdog thread, started when the input pin
// disconnects, samples the stack of the thread doing the teardown when it is not done after 3, 6 and 9 s, and, when
// the player window is gone 2.5 s after the teardown but the process still lives (a slow exit before the exit helper),
// the stack of the player's UI thread. Sampling: suspend, walk with the x64 unwind data (RtlVirtualUnwind; no
// allocation or lock while suspended), resume, then name the frames (module + offset) and log them.
static int WalkStackNoAlloc(HANDLE th, DWORD64* pcs, int max)
{
    int n = 0;
    __try {
        CONTEXT c = {}; c.ContextFlags = CONTEXT_FULL;
        if (!GetThreadContext(th, &c)) return 0;
        for (; n < max && c.Rip; ++n) {
            pcs[n] = c.Rip;
            DWORD64 base = 0;
            PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
            if (!rf) { if (!c.Rsp) { ++n; break; } c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp); c.Rsp += 8; continue; }   // leaf
            PVOID hd = nullptr; DWORD64 ef = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, rf, &c, &hd, &ef, nullptr);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return n;
}
static std::string SampleThreadStack(DWORD tid)
{
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!th) return std::format("(thread {} not opened: error {})", tid, GetLastError());
    DWORD64 pcs[40] = {};
    int n = 0;
    if (SuspendThread(th) != DWORD(-1)) { n = WalkStackNoAlloc(th, pcs, 40); ResumeThread(th); }
    CloseHandle(th);
    std::string out;
    for (int i = 0; i < n; ++i) {
        HMODULE m = nullptr; wchar_t path[MAX_PATH] = {};
        std::string name = "?";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(pcs[i]), &m) && m
            && GetModuleFileNameW(m, path, MAX_PATH)) {
            std::wstring w = path; w = w.substr(w.find_last_of(L'\\') + 1); name = Narrow(w);
        }
        out += std::format("{}{}+0x{:X}", i ? " < " : "", name, m ? pcs[i] - reinterpret_cast<DWORD64>(m) : pcs[i]);
    }
    return out.empty() ? std::string("(no frames)") : out;
}
struct TeardownWatch { HANDLE done = nullptr; DWORD tid = 0; HMODULE self = nullptr; };
static HANDLE StartTeardownWatch()
{
    auto* w = new TeardownWatch;
    w->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    w->tid = GetCurrentThreadId();
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&StartTeardownWatch), &w->self);   // the .ax stays loaded while it runs
    HANDLE mine = nullptr;
    DuplicateHandle(GetCurrentProcess(), w->done, GetCurrentProcess(), &mine, 0, FALSE, DUPLICATE_SAME_ACCESS);
    HANDLE th = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        auto* w = static_cast<TeardownWatch*>(p);
        const int64_t q0 = Qpc();
        bool done = false;
        for (int i = 1; i <= 3 && !done; ++i) {
            done = WaitForSingleObject(w->done, 3000) == WAIT_OBJECT_0;
            if (!done) Log(std::format("teardown watchdog: not done {:.0f} ms after the input disconnect; teardown thread {} at {}", QpcMs(Qpc() - q0), w->tid, SampleThreadStack(w->tid)));
        }
        if (done && WaitForSingleObject(w->done, 0) == WAIT_OBJECT_0) {
            Sleep(2500);
            HWND wnd = pm::PlayerWindow();
            if (!wnd || !IsWindowVisible(wnd)) {
                // the player is closing (its window gone or hidden) but the process still runs: where its threads are
                DWORD ui = wnd ? GetWindowThreadProcessId(wnd, nullptr) : 0;
                if (!ui) {   // the window is destroyed: the process's first thread (the UI thread of MFC players)
                    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); THREADENTRY32 te = { sizeof(te) };
                    FILETIME best = { 0xFFFFFFFF, 0x7FFFFFFF };
                    for (BOOL ok = snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
                        HANDLE t = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID); FILETIME c, e, k, u;
                        if (t && GetThreadTimes(t, &c, &e, &k, &u) && CompareFileTime(&c, &best) < 0) { best = c; ui = te.th32ThreadID; }
                        if (t) CloseHandle(t);
                    }
                    if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
                }
                if (ui) Log(std::format("exit watchdog: the player window is {} 2.5 s after the teardown and the process still runs; UI thread {} at {}",
                                        wnd ? "hidden" : "gone", ui, SampleThreadStack(ui)));
            }
        }
        CloseHandle(w->done);
        HMODULE self = w->self; delete w;
        if (self) FreeLibraryAndExitThread(self, 0);
        return 0;
    }, w, 0, nullptr);
    if (th) CloseHandle(th);
    else { if (w->self) FreeLibrary(w->self); CloseHandle(w->done); delete w; if (mine) CloseHandle(mine); return nullptr; }
    return mine;
}

// status for the property page (one process, one DLL): the latest filter's state, refreshed about once a second
struct PageStatus {
    std::wstring state = std::wstring(L"Not used yet in this ") + pm::HostName() + L" session.";
    std::wstring live;                                   // fps, GPU time, display, drops
    std::wstring smoother, sharper, cleanUp, sdrToHdr;   // "Now: ..." notes
    std::wstring deband;                                 // Advanced: "Deband now: ..."
};
static std::mutex g_statusLock;
// files this MPC-HC process found interlaced (CheckInterlace): their next connection is the source format
static std::mutex g_ilLock;
static std::set<std::wstring> g_interlacedFiles;
static PageStatus g_page;
static std::atomic<uint32_t> g_reloadSerial = 0;   // forced rebuild (debug)
static std::atomic<uint32_t> g_settingsSerial = 0; // the page saved new settings: re-read them now
// Round 2 (F8): the NGX core key of the graph that last held pushcore's process-wide warm cache. PushFPS's
// NgxCoreCache::Key is device + runtime folder (fixed within a process) + FG on + FG backend + multiplier; a graph
// with another key closes the warm core (NGX Shutdown + Init: ~2 s of a cold 2.6-s open).
struct NgxKey { bool fg = false; uint32_t backend = 0; uint32_t mult = 1; bool operator==(const NgxKey&) const = default; };
static NgxKey NgxKeyOf(const PushCoreSettings& p) { const bool fg = p.multiplier > 1 && p.fgBackend == PUSHCORE_FG_DLSSG; return { fg, fg ? p.fgBackend : 0u, fg ? p.multiplier : 1u }; }   // as pushcore: only DLSS-G keys the NGX core
static std::mutex g_ngxLock;
static NgxKey g_lastNgx;
static bool g_haveNgx = false;
static pm::Gpu g_gpu;            // the GPU the last filter runs on (the page greys out what it cannot run); g_statusLock
static bool g_gpuKnown = false;
static void SetStatus(const std::wstring& s) { { std::lock_guard<std::mutex> lk(g_statusLock); g_page.state = s; } LogW(L"status: " + s); }
static void SetLive(const std::wstring& s) { std::lock_guard<std::mutex> lk(g_statusLock); g_page.live = s; }
static void SetNotes(const std::wstring& a, const std::wstring& b, const std::wstring& c, const std::wstring& d, const std::wstring& e = {})
{
    std::lock_guard<std::mutex> lk(g_statusLock);
    g_page.smoother = a; g_page.sharper = b; g_page.cleanUp = c; g_page.sdrToHdr = d; g_page.deband = e;
}
static PageStatus PageSnapshot() { std::lock_guard<std::mutex> lk(g_statusLock); return g_page; }

// ------------------------------------------------------------------------------------------------ config
// User settings: HKCU\Software\PushMPC (PushMpcSettings.h). Debug and test values: HKCU\Software\PushMPC\Filter
// (absent = default); every value can also be overridden for one process with the environment variable
// PUSHMPC_FILTER_<NAME> (tests).
static const wchar_t* kRegKey = L"Software\\PushMPC\\Filter";
struct Config {
    pm::Settings user;         // HKCU\Software\PushMPC
    bool enable = true;        // user.enable (PUSHMPC_FILTER_Enable overrides it for tests)
    bool hdrPushFps = false;   // HdrPushFpsToneMap (debug): PushMPC (PushFPS) tone-maps HDR to SDR (BGRA) whatever HdrToSdr Auto says
    int leadUs = -1;           // LeadUs: Deliver this long before the frame's time; -1 = auto (half a refresh period - 0.4 ms)
    bool admission = true, watch = true, frameLog = true, preroll = true, reopenOnLost = true, warmCache = true; // WarmCache 0 moves the NGX release into the graph teardown (3-5 s, MPC-HC watchdog): see "Fixes"
    std::wstring runtimeRoot;  // RuntimeRoot: the runtime folder (default %LOCALAPPDATA%\PushMPC\runtime)
    std::wstring coreFolder;   // CoreFolder: pushcore.dll and shaders (default %LOCALAPPDATA%\PushMPC\core)
    std::wstring folder;       // PushFpsFolder (developer): run on a PushFPS folder as the runtime root (its own shaders)
    std::wstring dataDir;      // DataDir (developer): use a PushFPS chain session instead of PushMPC's settings
    int simulateLostSec = 0, simulateLostMode = 1;
    int fullOpenDelayMs = 700;   // FullOpenDelayMs: quick start, the full graph opens this long after the quick one
    int simulateNoOutputSec = 0; // debug SimulateNoOutput: the copier stops taking pushcore's frames this long after activation (tests the no-output guard)
    std::wstring forceFallback; // debug: pass-through with this reason
    bool forceP010 = false;     // debug ForceP010: skip Y416/Y410 (tests the P010 HDR output)
    bool skipY416 = false;      // debug SkipY416: no Y416 at all
    bool skipManifest = false;  // debug SkipManifestCheck: do not check the runtime manifest
    bool itmBypass = false;     // debug ItmBypass: SDR to HDR by the plain 203 cd/m2 mapping (A/B timing of the curve)
    bool prewarm = true;        // Prewarm 0: no pushcore_prewarm at the input connection (fast start)
    bool quickStart = true;     // QuickStart 0: no quick-start graph while the full one opens (fast start)
    bool quickVsr = true;       // QuickVsr 0: the quick graph upscales with NVIDIA Image Scaling, not the driver's RTX VSR (D3D11)
    bool paceWarmup = true;     // PaceWarmup 0: the source frames shown during the open are taken as fast as the decoder sends them
};
static bool EnvStr(const wchar_t* name, std::wstring& out)
{
    wchar_t buf[1024];
    const std::wstring n = std::wstring(L"PUSHMPC_FILTER_") + name;
    const DWORD r = GetEnvironmentVariableW(n.c_str(), buf, 1024);
    if (r == 0 || r >= 1024) return false;
    out = buf;
    return true;
}
static void ReadDw(HKEY k, const wchar_t* name, int& v)
{
    DWORD d = 0, sz = sizeof(d), type = 0;
    if (k && RegQueryValueExW(k, name, nullptr, &type, (BYTE*)&d, &sz) == ERROR_SUCCESS && type == REG_DWORD) v = int(d);
    std::wstring e;
    if (EnvStr(name, e)) v = _wtoi(e.c_str());
}
static void ReadB(HKEY k, const wchar_t* name, bool& v) { int i = v ? 1 : 0; ReadDw(k, name, i); v = i != 0; }
static void ReadS(HKEY k, const wchar_t* name, std::wstring& v)
{
    wchar_t buf[1024] = {};
    DWORD sz = sizeof(buf) - 2, type = 0;
    if (k && RegQueryValueExW(k, name, nullptr, &type, (BYTE*)buf, &sz) == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) v = buf;
    std::wstring e;
    if (EnvStr(name, e)) v = e;
}
static Config ReadConfig()
{
    Config c;
    c.user = pm::LoadSettings();
    c.enable = c.user.enable;
    c.frameLog = c.user.frameLog;
    HKEY k = nullptr;
    RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &k);
    { std::wstring e; if (EnvStr(L"Enable", e)) c.enable = _wtoi(e.c_str()) != 0; }
    // HdrPushFpsToneMap: PushFPS's SDR tone map now encodes for the SDR display's 2.2 (was piecewise sRGB, retired then on
    // 2026-11-23); the user setting HdrToSdr (Auto | Player | PushMPC) picks the route, this forces PushMPC's for A/B runs
    ReadB(k, L"HdrPushFpsToneMap", c.hdrPushFps);
    ReadDw(k, L"LeadUs", c.leadUs);
    ReadB(k, L"Admission", c.admission);
    ReadB(k, L"WatchSettings", c.watch);
    { std::wstring e; if (EnvStr(L"FrameLog", e)) c.frameLog = _wtoi(e.c_str()) != 0; }
    ReadB(k, L"PrerollLead", c.preroll);
    ReadB(k, L"ReopenOnDeviceLost", c.reopenOnLost);
    ReadB(k, L"WarmCache", c.warmCache);
    ReadB(k, L"Prewarm", c.prewarm);
    ReadB(k, L"QuickStart", c.quickStart);
    ReadB(k, L"QuickVsr", c.quickVsr);
    ReadB(k, L"PaceWarmup", c.paceWarmup);
    ReadS(k, L"RuntimeRoot", c.runtimeRoot);
    ReadS(k, L"CoreFolder", c.coreFolder);
    ReadS(k, L"PushFpsFolder", c.folder);
    ReadS(k, L"DataDir", c.dataDir);
    ReadDw(k, L"SimulateDeviceLost", c.simulateLostSec);
    ReadDw(k, L"SimulateNoOutput", c.simulateNoOutputSec);
    ReadDw(k, L"FullOpenDelayMs", c.fullOpenDelayMs);
    ReadDw(k, L"SimulateDeviceLostMode", c.simulateLostMode);
    ReadS(k, L"ForceFallback", c.forceFallback);
    ReadB(k, L"ForceP010", c.forceP010);
    ReadB(k, L"SkipY416", c.skipY416);
    ReadB(k, L"SkipManifestCheck", c.skipManifest);
    ReadB(k, L"ItmBypass", c.itmBypass);
    if (k) RegCloseKey(k);
    return c;
}

// ------------------------------------------------------------------------------------------------ runtime (as the fork, PushFps.cpp)
static std::wstring FileVersionString(const std::wstring& path, const wchar_t* keyName)
{
    DWORD dummy = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (!size) return {};
    std::vector<BYTE> buf(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, buf.data())) return {};
    if (!keyName) {
        for (const wchar_t* k : { L"ProductVersion", L"FileVersion" }) {
            const std::wstring v = FileVersionString(path, k);
            if (!v.empty()) return v;
        }
        return {};
    }
    struct LangCp { WORD lang, cp; };
    LangCp* tr = nullptr;
    UINT trLen = 0;
    std::vector<std::wstring> blocks;
    if (VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&tr), &trLen) && tr) {
        for (UINT i = 0; i < trLen / sizeof(LangCp); ++i) blocks.push_back(std::format(L"{:04x}{:04x}", tr[i].lang, tr[i].cp));
    }
    blocks.push_back(L"040904b0");
    for (const auto& b : blocks) {
        wchar_t* val = nullptr;
        UINT len = 0;
        const std::wstring q = L"\\StringFileInfo\\" + b + L"\\" + keyName;
        if (VerQueryValueW(buf.data(), q.c_str(), reinterpret_cast<void**>(&val), &len) && val && len) return val;
    }
    return {};
}
static std::wstring RegStr(HKEY root, const std::wstring& key, const wchar_t* name, REGSAM view)
{
    wchar_t buf[MAX_PATH] = {};
    DWORD sz = sizeof(buf);
    if (RegGetValueW(root, key.c_str(), name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | (view == KEY_WOW64_32KEY ? RRF_SUBKEY_WOW6432KEY : view == KEY_WOW64_64KEY ? RRF_SUBKEY_WOW6464KEY : 0),
            nullptr, buf, &sz) != ERROR_SUCCESS) return {};
    return buf;
}
static std::wstring DetectInstalledPushFps(std::wstring* how)
{
    const struct { HKEY root; REGSAM view; const wchar_t* name; } keys[] = {
        { HKEY_CURRENT_USER, 0, L"HKCU uninstall key" },
        { HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, L"HKLM uninstall key" },
        { HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY, L"HKLM (32-bit) uninstall key" },
    };
    const std::wstring un = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    for (const auto& k : keys) {
        HKEY h = nullptr;
        if (RegOpenKeyExW(k.root, un.c_str(), 0, KEY_READ | k.view, &h) != ERROR_SUCCESS) continue;
        for (DWORD i = 0;; ++i) {
            wchar_t sub[256] = {};
            DWORD subLen = 256;
            if (RegEnumKeyExW(h, i, sub, &subLen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            const std::wstring sk = un + L"\\" + sub;
            const std::wstring name = RegStr(k.root, sk, L"DisplayName", k.view);
            if (_wcsnicmp(name.c_str(), L"PushFPS", 7) != 0) continue;
            for (const wchar_t* v : { L"InstallLocation", L"DisplayIcon" }) {
                std::wstring s = RegStr(k.root, sk, v, k.view);
                if (s.empty()) continue;
                s.erase(std::remove(s.begin(), s.end(), L'"'), s.end());
                if (const auto comma = s.find(L','); comma != std::wstring::npos) s.resize(comma);
                if (s.size() > 4 && _wcsicmp(s.c_str() + s.size() - 4, L".exe") == 0) s = ParentDir(s);
                s = TrimSlashes(s);
                if (ValidPushFpsFolder(s)) { if (how) *how = std::format(L"{} \"{}\"", k.name, name); RegCloseKey(h); return s; }
            }
        }
        RegCloseKey(h);
    }
    for (HKEY root : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE }) {
        std::wstring s = RegStr(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\PushFPS.exe", nullptr, 0);
        if (s.empty()) continue;
        s.erase(std::remove(s.begin(), s.end(), L'"'), s.end());
        s = ParentDir(s);
        if (ValidPushFpsFolder(s)) { if (how) *how = L"App Paths"; return s; }
    }
    PWSTR p = nullptr;
    std::wstring def;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_UserProgramFiles, 0, nullptr, &p)) && p) { def = std::wstring(p) + L"\\PushFPS"; CoTaskMemFree(p); }
    if (ValidPushFpsFolder(def)) { if (how) *how = L"default install path"; return def; }
    if (how) *how = L"not found (no uninstall entry, no App Paths entry, nothing in " + def + L")";
    return {};
}
// The deployed layout (pushmpc-install.ps1), one versioned component per folder, each with a manifest.txt:
//   %LOCALAPPDATA%\PushMPC\filter\   PushMpcFilter.ax, PushMpcExit.dll
//   %LOCALAPPDATA%\PushMPC\core\     pushcore.dll, shaders\*.dxil (built from the PushFPS source named in its manifest)
//   %LOCALAPPDATA%\PushMPC\runtime\  the subset of Veyra's official runtime package (FFmpeg, NVIDIA components)
// manifest.txt: "key=value" lines (component, version, pushfps, requires-runtime, package, package-sha256, ...), then one
// "file <sha256> <size> <relative path>" line per file. Here only presence and size are checked (a full hash check of
// ~0.5 GB is the installer's "status"); core's requires-runtime must equal the runtime's version.
struct Manifest {
    std::map<std::wstring, std::wstring> kv;
    struct File { std::wstring sha, rel; uint64_t size = 0; };
    std::vector<File> files;
    bool ok = false;
    std::wstring Get(const wchar_t* k) const { auto it = kv.find(k); return it == kv.end() ? std::wstring() : it->second; }
};
static Manifest ReadManifest(const std::wstring& dir)
{
    Manifest m;
    std::ifstream f(dir + L"\\manifest.txt");
    if (!f) return m;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() >= 3 && (unsigned char)line[0] == 0xEF) line.erase(0, 3); // UTF-8 BOM
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("file ", 0) == 0) {
            std::istringstream is(line.substr(5));
            Manifest::File fl; std::string sha, rel; uint64_t size = 0;
            is >> sha >> size; std::getline(is, rel);
            while (!rel.empty() && rel[0] == ' ') rel.erase(0, 1);
            fl.sha = Widen(sha); fl.size = size; fl.rel = Widen(rel);
            if (!fl.rel.empty()) m.files.push_back(fl);
        } else if (const auto eq = line.find('='); eq != std::string::npos) {
            m.kv[Widen(line.substr(0, eq))] = Widen(line.substr(eq + 1));
        }
    }
    m.ok = !m.kv.empty();
    return m;
}
// missing or wrong-size files (at most a few named)
static std::wstring CheckManifestFiles(const std::wstring& dir, const Manifest& m)
{
    std::wstring bad; int n = 0;
    for (const auto& f : m.files) {
        WIN32_FILE_ATTRIBUTE_DATA a = {};
        const std::wstring p = dir + L"\\" + f.rel;
        const bool present = GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a) && !(a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        const uint64_t size = present ? (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow : 0;
        if (present && size == f.size) continue;
        if (++n <= 3) bad += (bad.empty() ? L"" : L", ") + f.rel + (present ? L" (size differs)" : L" (missing)");
    }
    if (n > 3) bad += std::format(L" and {} more", n - 3);
    return bad;
}
struct Runtime {
    std::wstring root, shaderDir, corePath, coreFolder, coreVersion, pushfpsVersion, runtimeVersion, dataDir, source, error;
    std::wstring Describe() const
    {
        if (!error.empty()) return error;
        return std::format(L"runtime {} at {} ({}); core {} (PushFPS {}) at {}; settings {}", runtimeVersion, root, source, coreVersion,
            pushfpsVersion, corePath, dataDir.empty() ? std::wstring(L"PushMPC") : L"PushFPS chain session " + dataDir);
    }
};
static Runtime ResolveRuntime(const Config& cfg)
{
    Runtime rt;
    const std::wstring base = LocalAppData() + L"\\PushMPC";
    rt.coreFolder = cfg.coreFolder.empty() ? base + L"\\core" : TrimSlashes(cfg.coreFolder);
    rt.corePath = rt.coreFolder + L"\\pushcore.dll";
    if (!FileExists(rt.corePath)) { rt.error = L"PushMPC's core is not installed (no pushcore.dll in " + rt.coreFolder + L"): run pushmpc-install.ps1 install"; return rt; }
    const Manifest core = ReadManifest(rt.coreFolder);
    rt.coreVersion = core.ok ? core.Get(L"version") : L"(no manifest)";
    rt.pushfpsVersion = core.ok ? core.Get(L"pushfps") : Version3(FileVersionString(rt.corePath, L"PushFPSVersion"));
    rt.dataDir = cfg.dataDir;
    if (!cfg.folder.empty()) {
        // developer route: a PushFPS folder as the root, with its own shaders (no manifest check)
        rt.root = TrimSlashes(cfg.folder);
        if (!ValidPushFpsFolder(rt.root)) { rt.error = L"PushFpsFolder " + rt.root + L" holds no PushFPS (PushFPS.exe and shaders)"; return rt; }
        rt.source = L"PushFpsFolder override";
        rt.runtimeVersion = L"PushFPS " + Version3(FileVersionString(rt.root + L"\\PushFPS.exe", nullptr));
        return rt;
    }
    rt.root = cfg.runtimeRoot.empty() ? base + L"\\runtime" : TrimSlashes(cfg.runtimeRoot);
    rt.source = cfg.runtimeRoot.empty() ? L"default" : L"RuntimeRoot override";
    rt.shaderDir = DirExists(rt.coreFolder + L"\\shaders") ? rt.coreFolder + L"\\shaders" : std::wstring();
    if (rt.shaderDir.empty() && !DirExists(rt.root + L"\\shaders")) { rt.error = L"no shaders in " + rt.coreFolder + L"\\shaders: reinstall PushMPC"; return rt; }
    const Manifest run = ReadManifest(rt.root);
    if (!run.ok) { rt.error = L"PushMPC's runtime is not installed (no manifest.txt in " + rt.root + L"): run pushmpc-install.ps1 install"; return rt; }
    rt.runtimeVersion = run.Get(L"version");
    // requires-runtime: one version, or several separated by '|' (the NVIDIA and the AMD package of one Veyra release)
    const std::wstring need = core.ok ? core.Get(L"requires-runtime") : std::wstring();
    bool runtimeMatches = need.empty();
    for (size_t p = 0; !runtimeMatches && p <= need.size();) {
        const size_t e = std::min(need.find(L'|', p), need.size());
        if (_wcsicmp(need.substr(p, e - p).c_str(), rt.runtimeVersion.c_str()) == 0) runtimeMatches = true;
        p = e + 1;
    }
    if (!runtimeMatches) {
        rt.error = std::format(L"runtime version skew: the core needs runtime {}, {} has {}: run pushmpc-install.ps1 update", need, rt.root, rt.runtimeVersion);
        return rt;
    }
    if (!cfg.skipManifest) {
        const std::wstring bad = CheckManifestFiles(rt.root, run);
        if (!bad.empty()) { rt.error = L"PushMPC's runtime is incomplete: " + bad + L": run pushmpc-install.ps1 install"; return rt; }
    }
    return rt;
}
// The installed PushFPS (only for "Import my PushFPS settings"; never written to)
static std::wstring PushFpsDataDir()
{
    const std::wstring d = DetectInstalledPushFps(nullptr);
    if (d.empty()) return {};
    const std::wstring data = d + L"\\runtime_local\\user-data-2.0.0";
    return FileExists(data + L"\\chain-session.v1") ? data : std::wstring();
}
static std::mutex g_libLock;
static const PushCoreApi* g_api = nullptr;
static uint32_t g_coreFeatures = 0; // pushcore_features() of the loaded pushcore.dll (0 = an older build)
static std::wstring g_libRoot;
static PushCoreCapabilitiesFn g_capsFn = nullptr;   // pushcore_capabilities (PUSHCORE_FEATURE_FG_METHODS)
static PushCoreFlowBenchFn g_benchFn = nullptr;     // pushcore_pushFlowBench
static PushCorePrewarmFn g_prewarmFn = nullptr;     // pushcore_prewarm (fast start; absent in older cores)
static PushCorePrewarmReleaseFn g_prewarmReleaseFn = nullptr;   // pushcore_prewarmRelease
static PushCorePrewarm2Fn g_prewarm2Fn = nullptr;   // pushcore_prewarm2 (with the NGX core)
static const PushCoreApi* LoadApi(const Runtime& rt, std::wstring& err)
{
    std::lock_guard<std::mutex> lk(g_libLock);
    if (g_api) {
        if (!rt.root.empty() && _wcsicmp(g_libRoot.c_str(), rt.root.c_str()) != 0) { err = L"pushcore is already loaded with the runtime root " + g_libRoot; return nullptr; }
        return g_api;
    }
    DLL_DIRECTORY_COOKIE cookie = AddDllDirectory(rt.root.c_str());
    HMODULE h = LoadLibraryExW(rt.corePath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_USER_DIRS | LOAD_LIBRARY_SEARCH_SYSTEM32);
    const DWORD le = GetLastError();
    if (cookie) RemoveDllDirectory(cookie);
    if (!h) { err = std::format(L"LoadLibraryEx({}) failed, error {}", rt.corePath, le); return nullptr; }
    auto getApi = reinterpret_cast<PushCoreGetApiFn>(GetProcAddress(h, "pushcore_getApi"));
    const PushCoreApi* api = getApi ? getApi(PUSHCORE_ABI_VERSION) : nullptr;
    if (!api || api->structSize < sizeof(PushCoreApi) || !api->submit2 || !api->setSettings || !api->importChainSession) { err = L"pushcore ABI mismatch in " + rt.corePath + L" (the filter and the core must be the same release)"; return nullptr; }
    g_api = api; g_libRoot = rt.root;
    if (auto feat = reinterpret_cast<uint32_t(*)(void)>(GetProcAddress(h, "pushcore_features"))) g_coreFeatures = feat();
    pm::VfgCeiling() = (g_coreFeatures & PUSHCORE_FEATURE_VFG16) ? int(PUSHCORE_MAX_MULTIPLIER) : 8;
    pm::PushFlowCeiling() = (g_coreFeatures & PUSHCORE_FEATURE_PUSHFLOW16) ? int(PUSHCORE_MAX_MULTIPLIER) : 4;   // PushFlow above 4x (flow once) only with a core that has it
    g_capsFn = reinterpret_cast<PushCoreCapabilitiesFn>(GetProcAddress(h, "pushcore_capabilities"));
    g_benchFn = reinterpret_cast<PushCoreFlowBenchFn>(GetProcAddress(h, "pushcore_pushFlowBench"));
    g_prewarmFn = reinterpret_cast<PushCorePrewarmFn>(GetProcAddress(h, "pushcore_prewarm"));
    g_prewarmReleaseFn = reinterpret_cast<PushCorePrewarmReleaseFn>(GetProcAddress(h, "pushcore_prewarmRelease"));
    g_prewarm2Fn = reinterpret_cast<PushCorePrewarm2Fn>(GetProcAddress(h, "pushcore_prewarm2"));
    Log(std::format("loaded {} (ABI {}, core {}, PushFPS {}), runtime root {}", Narrow(rt.corePath), api->abiVersionFn(), Narrow(rt.coreVersion), Narrow(rt.pushfpsVersion), Narrow(rt.root)));
    return api;
}
static void SehCaps(uint64_t luid, const wchar_t* root, PushCoreCaps* c)
{
    __try { g_capsFn(luid, root, c); } __except (EXCEPTION_EXECUTE_HANDLER) { c->available = 0; }
}
// What this GPU can run (pushcore_capabilities), as Auto and the page see it. luid 0 = DXGI's first hardware adapter.
static pm::Gpu QueryGpu(const Runtime& rt, uint64_t luid, std::wstring* log)
{
    pm::Gpu g;
    g.available = 0;
    std::wstring err;
    if (!rt.error.empty() || !LoadApi(rt, err) || !g_capsFn) {
        g.family = pm::kFamUnknown;
        for (auto& r : g.reason) r = L"PushMPC's core is not available";
        if (log) *log = L"capabilities unknown: " + (rt.error.empty() ? err : rt.error);
        return g;
    }
    PushCoreCaps c = {};
    c.structSize = sizeof(c);
    SehCaps(luid, rt.root.c_str(), &c);
    g.family = c.family; g.available = c.available; g.hardware = c.hardware; g.name = Widen(c.adapter);
    for (uint32_t i = 0; i < pm::kCapCount && i < PUSHCORE_CAP_COUNT; ++i) g.reason[i] = Widen(c.reason[i]);
    pm::ReadMeasured(g);   // the Intel methods' self-test numbers (reason[] of an available capability)
    // NIS: pushcore_capabilities looks for the NIS shaders in PushFPS's shader folder, which pushcore's FIRST open in a
    // process sets (RuntimePaths override): before that open it looked next to mpc-hc64.exe and said "missing", after it
    // in core\shaders (RC1 F5: "not on this GPU" in the first filter of a process, available in a later one). Checked
    // here in the folder every open uses, so the answer no longer depends on what ran before. NIS is a shader: every
    // DirectX 12 GPU runs it.
    {
        const std::wstring dir = !rt.shaderDir.empty() ? rt.shaderDir : rt.root + L"\\shaders";
        auto has = [&](const wchar_t* f) { const DWORD a = GetFileAttributesW((dir + L"\\" + f).c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); };
        g.hardware |= 1u << pm::kCapNis;
        if (has(L"NisScalerSdr.dxil") && has(L"NisColor.dxil")) { g.available |= 1u << pm::kCapNis; g.reason[pm::kCapNis].clear(); }
        else { g.available &= ~(1u << pm::kCapNis); g.reason[pm::kCapNis] = L"the NIS shaders are missing in " + dir; }
    }
    if (log) {
        std::wstring s = std::format(L"GPU {} ({}, vendor {:04X} device {:04X}): available", g.name, pm::FamilyName(g.family), c.vendorId, c.deviceId);
        static const wchar_t* names[] = { L"VFG", L"DLSS-G", L"PushFlow", L"FSR FG", L"FSR 4 FG", L"AMD FRC", L"RTX VSR", L"VSR styles", L"FSR SR", L"NIS", L"RTX HDR", L"NR", L"DLSS-G Lite", L"NVOF FG",
                                          L"XeSS FG", L"Intel VPL FI", L"XeSS SR", L"Intel VSR", L"ZLUDA VSR",
                                          L"HelixSR FG", L"HLX FSR 4", L"anime upscalers" };
        static_assert(std::size(names) >= pm::kCapCount, "a name per capability");
        for (uint32_t i = 0; i < pm::kCapCount; ++i) if (g.has(i)) s += std::wstring(L" ") + names[i];
        s += L"; not:";
        for (uint32_t i = 0; i < pm::kCapCount; ++i) if (!g.has(i)) s += std::format(L" {} ({});", names[i], g.reason[i]);
        if (c.nrRuntime[0]) s += L" NR runtime " + Widen(c.nrRuntime);
        *log = s;
    }
    return g;
}
// pushcore calls that may raise (an NVIDIA stack failure): SEH, in functions without C++ unwinding
static PushCore* SehOpen(const PushCoreApi* a, const PushCoreConfig* c, const PushCoreStreamInfo* s, ID3D11Device* d, char* e, uint32_t n)
{
    __try { return a->open(c, s, d, e, n); } __except (EXCEPTION_EXECUTE_HANDLER) { strcpy_s(e, n, "pushcore_open raised an exception"); return nullptr; }
}
static int SehBench(uint64_t luid, const wchar_t* root, PushCoreFlowBench* b)
{
    __try { return g_benchFn ? g_benchFn(luid, root, b) : PUSHCORE_ERROR; } __except (EXCEPTION_EXECUTE_HANDLER) { return PUSHCORE_ERROR; }
}
static void SehClose(const PushCoreApi* a, PushCore* c)
{
    __try { a->close(c); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static std::wstring SettingsSignature(const std::wstring& dir)
{
    std::wstring sig;
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW((dir + L"\\chain-session.v1*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return sig;
    do {
        sig += std::format(L"{}|{}|{:08X}{:08X};", fd.cFileName, (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow, fd.ftLastWriteTime.dwHighDateTime, fd.ftLastWriteTime.dwLowDateTime);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return sig;
}

// ------------------------------------------------------------------------------------------------ colour enums (as the fork)
static int FfRange(UINT v) { return v == DXVA2_NominalRange_16_235 ? 1 : v == DXVA2_NominalRange_0_255 ? 2 : 0; }
static int FfMatrix(UINT v) { switch (v) { case 1: return 1; case 2: return 6; case 3: return 7; case 4: case 5: return 9; default: return 2; } }
static int FfPrimaries(UINT v) { switch (v) { case 2: return 1; case 3: return 4; case 4: return 5; case 5: return 6; case 6: return 7; case 9: return 9; default: return 2; } }
static int FfTransfer(UINT v) { switch (v) { case 4: return 4; case 5: return 1; case 6: return 7; case 7: return 13; case 15: return 16; case 16: return 18; default: return 2; } }
static constexpr UINT kTransPQ = 15, kTransHLG = 16; // MFVideoTransFunc_2084 / _HLG

// ------------------------------------------------------------------------------------------------ HEVC SPS colour
// The colour description in an HEVC sequence parameter set's VUI (ITU-T H.265 7.3.2.2 / E.2.1), for streams whose
// container carries none: the splitter's type holds the SPS (hvcC arrays or Annex B), so the HDR10 output format can be
// chosen at connect time, before LAV's first sample. Returns false when no complete SPS with a colour description parses.
struct SpsColour { int primaries = 2, transfer = 2, matrix = 2; bool full = false; uint32_t w = 0, h = 0; };
class BitReader
{
public:
    BitReader(const BYTE* p, size_t n) { rbsp.reserve(n); for (size_t i = 0; i < n; i++) { if (i >= 2 && p[i] == 3 && p[i - 1] == 0 && p[i - 2] == 0 && rbsp.size() >= 2 && rbsp[rbsp.size() - 1] == 0 && rbsp[rbsp.size() - 2] == 0) continue; rbsp.push_back(p[i]); } }
    uint32_t u(int n) { uint32_t v = 0; while (n-- > 0) { if (pos >= rbsp.size() * 8) { bad = true; return 0; } v = (v << 1) | ((rbsp[pos >> 3] >> (7 - (pos & 7))) & 1); pos++; } return v; }
    uint32_t ue() { int z = 0; while (!bad && u(1) == 0) { if (++z > 31) { bad = true; return 0; } } return z ? ((1u << z) - 1 + u(z)) : 0; }
    int32_t se() { const uint32_t v = ue(); return (v & 1) ? int32_t((v + 1) / 2) : -int32_t(v / 2); }
    bool bad = false;
private:
    std::vector<BYTE> rbsp;
    size_t pos = 0;
};
static bool ParseHevcSps(const BYTE* p, size_t n, SpsColour& c)
{
    BitReader b(p, n);
    if (b.u(1) != 0 || b.u(6) != 33) return false; // forbidden zero, nal_unit_type SPS
    b.u(6); b.u(3);                               // nuh_layer_id, temporal_id_plus1
    b.u(4);
    const uint32_t maxSub = b.u(3);
    b.u(1);
    b.u(32); b.u(32); b.u(32); // general profile_tier_level: 88 bits + level_idc 8
    std::vector<int> prof(maxSub), lev(maxSub);
    for (uint32_t i = 0; i < maxSub; i++) { prof[i] = b.u(1); lev[i] = b.u(1); }
    if (maxSub > 0) for (uint32_t i = maxSub; i < 8; i++) b.u(2);
    for (uint32_t i = 0; i < maxSub; i++) { if (prof[i]) { b.u(32); b.u(32); b.u(24); } if (lev[i]) b.u(8); }
    b.ue();
    const uint32_t chroma = b.ue();
    if (chroma == 3) b.u(1);
    c.w = b.ue(); c.h = b.ue();
    if (b.u(1)) { b.ue(); b.ue(); b.ue(); b.ue(); }
    b.ue(); b.ue();
    const uint32_t log2PocLsb = b.ue() + 4;
    const uint32_t ordering = b.u(1);
    for (uint32_t i = ordering ? 0 : maxSub; i <= maxSub; i++) { b.ue(); b.ue(); b.ue(); }
    b.ue(); b.ue(); b.ue(); b.ue(); b.ue(); b.ue();
    if (b.u(1) && b.u(1)) { // scaling_list_enabled, sps_scaling_list_data_present
        for (int sizeId = 0; sizeId < 4; sizeId++)
            for (int m = 0; m < 6; m += sizeId == 3 ? 3 : 1) {
                if (!b.u(1)) b.ue();
                else { const int num = std::min(64, 1 << (4 + (sizeId << 1))); if (sizeId > 1) b.se(); for (int k = 0; k < num && !b.bad; k++) b.se(); }
            }
    }
    b.u(1); b.u(1);
    if (b.u(1)) { b.u(4); b.u(4); b.ue(); b.ue(); b.u(1); } // pcm
    const uint32_t numSets = b.ue();
    if (numSets > 64) return false;
    std::vector<uint32_t> numDelta(numSets, 0);
    for (uint32_t i = 0; i < numSets && !b.bad; i++) {
        if (i != 0 && b.u(1)) { // inter_ref_pic_set_prediction_flag
            b.u(1); b.ue();
            uint32_t cnt = 0;
            for (uint32_t j = 0; j <= numDelta[i - 1] && !b.bad; j++) { const uint32_t used = b.u(1); const uint32_t useDelta = used ? 1 : b.u(1); if (used || useDelta) cnt++; }
            numDelta[i] = cnt;
        } else {
            const uint32_t neg = b.ue(), pos = b.ue();
            if (neg > 16 || pos > 16) return false;
            for (uint32_t k = 0; k < neg + pos && !b.bad; k++) { b.ue(); b.u(1); }
            numDelta[i] = neg + pos;
        }
    }
    if (b.u(1)) { const uint32_t lt = b.ue(); if (lt > 32) return false; for (uint32_t i = 0; i < lt && !b.bad; i++) { b.u(int(log2PocLsb)); b.u(1); } }
    b.u(1); b.u(1);
    if (!b.u(1)) return false; // no VUI
    if (b.u(1)) { if (b.u(8) == 255) { b.u(16); b.u(16); } }
    if (b.u(1)) b.u(1);
    if (!b.u(1)) return false; // no video_signal_type
    b.u(3);
    c.full = b.u(1) != 0;
    if (!b.u(1)) return false; // no colour description
    c.primaries = int(b.u(8)); c.transfer = int(b.u(8)); c.matrix = int(b.u(8));
    return !b.bad;
}
// scans a format block for an HEVC SPS NAL (hvcC: 16-bit length before it; Annex B: start code before it)
static bool FindHevcSpsColour(const BYTE* p, size_t n, SpsColour& c)
{
    for (size_t i = 2; i + 4 < n; i++) {
        if (p[i] != 0x42 || p[i + 1] != 0x01) continue; // nal_unit_type 33, layer 0, tid 1
        size_t len = 0;
        const size_t l16 = (size_t(p[i - 2]) << 8) | p[i - 1];
        if (l16 >= 8 && i + l16 <= n) len = l16;
        else if (i >= 3 && p[i - 1] == 1 && p[i - 2] == 0 && p[i - 3] == 0) { len = n - i; for (size_t k = i + 2; k + 2 < n; k++) if (p[k] == 0 && p[k + 1] == 0 && (p[k + 2] == 1 || p[k + 2] == 0)) { len = k - i; break; } }
        if (!len) continue;
        if (ParseHevcSps(p + i, len, c)) return true;
    }
    return false;
}
// H.273 code points -> the DXVA2_ExtendedFormat fields the media types carry
static DWORD CtrlFromH273(const SpsColour& c)
{
    DXVA2_ExtendedFormat ex; ex.value = 0;
    ex.SampleFormat = AMCONTROL_USED | AMCONTROL_COLORINFO_PRESENT;
    ex.NominalRange = c.full ? DXVA2_NominalRange_0_255 : DXVA2_NominalRange_16_235;
    ex.VideoTransferMatrix = c.matrix == 9 || c.matrix == 10 ? 4 : c.matrix == 5 || c.matrix == 6 ? 2 : c.matrix == 7 ? 3 : 1;
    ex.VideoPrimaries = c.primaries == 9 ? 9 : c.primaries == 5 ? 4 : c.primaries == 6 ? 5 : c.primaries == 7 ? 6 : 2;
    ex.VideoTransferFunction = c.transfer == 16 ? kTransPQ : c.transfer == 18 ? kTransHLG : c.transfer == 8 ? 1 : 5;
    return ex.value;
}

// ------------------------------------------------------------------------------------------------ shaders
static const char* kHlsl = R"(
Texture2D t0 : register(t0);
Texture2D t1 : register(t1);
SamplerState s0 : register(s0);
// yuv: Y offset, Y scale, C offset, C scale (UNORM units, warm-up pixel shader); mtx: Cr->R, Cb->G, Cr->G, Cb->B;
// flags.x = 1: the source is HDR (PQ, BT.2020) and the output SDR BGRA: tone-mapped (warm-up frames only)
// flags.w = 1: the source is HLG (BT.2100): converted to PQ first (warm-up, reload and fallback frames)
cbuffer P : register(b0) { float4 yuv; float4 mtx; float4 flags; };
// on-screen status (OSD): a glyph atlas (R8, white on black) and its layout; drawn into every output frame after frame
// generation (never interpolated). osdBox: the box in output pixels; osdText: text origin x / y, output pixels per atlas
// pixel, visible; osdParams: characters, atlas width / height; osdGlyph: atlas x and width per glyph; osdChars: per
// character its start (atlas pixels) and glyph, two per float4
Texture2D osdAtlas : register(t2);
cbuffer O : register(b5) { float4 osdBox; float4 osdText; float4 osdParams; float4 osdGlyph[97]; float4 osdChars[32]; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VS(uint id : SV_VertexID) {
    VSOut o; float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o;
}
// --- colour helpers. SDR content in an HDR10 output: BT.1886 (gamma 2.4) -> linear -> BT.2020 -> PQ with SDR white at
// 203 cd/m2 (BT.2408). HDR source in an SDR output (warm-up only, until PushFPS's own tone map runs): PQ -> linear
// (203 cd/m2 = 1.0) -> BT.709 -> extended Reinhard on the max channel (1000 cd/m2 to white) -> gamma 1/2.2 (the SDR display's).
static const float3x3 M709to2020 = { 0.6274, 0.3293, 0.0433, 0.0691, 0.9195, 0.0114, 0.0164, 0.0880, 0.8956 };
static const float3x3 M2020to709 = { 1.6605, -0.5876, -0.0728, -0.1246, 1.1329, -0.0083, -0.0182, -0.1006, 1.1187 };
float3 PqOetf(float3 y) { float3 p = pow(max(y, 0.0), 0.1593017578125); return pow((0.8359375 + 18.8515625 * p) / (1.0 + 18.6875 * p), 78.84375); }
float3 PqEotf(float3 e) { float3 p = pow(saturate(e), 1.0 / 78.84375); return pow(max(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p), 1.0 / 0.1593017578125); }
float3 SdrToPq(float3 c) { return PqOetf(mul(M709to2020, pow(saturate(c), 2.4)) * (203.0 / 10000.0)); }
float3 PqToSdr(float3 e) {
    float3 l = max(mul(M2020to709, PqEotf(e) * (10000.0 / 203.0)), 0.0);
    float m = max(max(l.r, l.g), l.b);
    const float w2 = (1000.0 / 203.0) * (1000.0 / 203.0);
    l *= m > 0.0 ? (1.0 + m / w2) / (1.0 + m) : 1.0;
    return pow(saturate(l), 1.0 / 2.2);   // the SDR display's 2.2 (as the SDR to HDR path decodes)
}
// HLG (BT.2100, BT.2020) -> PQ: the reference transcode (BT.2408): inverse OETF, OOTF for a 1000 cd/m2 display with
// system gamma 1.2 (HLG 75 % = 203 cd/m2), PQ; the same formulas as PushFPS's YuvToLinearRgb for pushcore's frames
float3 HlgToPq(float3 e) {
    e = saturate(e);
    const float a = 0.17883277, b = 0.28466892, c = 0.55991073;
    const float3 s = float3(e.r <= 0.5 ? e.r * e.r / 3.0 : (exp((e.r - c) / a) + b) / 12.0,
                            e.g <= 0.5 ? e.g * e.g / 3.0 : (exp((e.g - c) / a) + b) / 12.0,
                            e.b <= 0.5 ? e.b * e.b / 3.0 : (exp((e.b - c) / a) + b) / 12.0);
    const float ys = dot(s, float3(0.2627, 0.6780, 0.0593));
    return PqOetf(s * (ys > 0.0 ? pow(ys, 0.2) : 0.0) * (1000.0 / 10000.0));
}
float3 YuvToRgb(float y, float2 c) { return float3(y + mtx.x * c.y, y - mtx.y * c.x - mtx.z * c.y, y + mtx.w * c.x); }
// Dolby Vision profile 5 base layer (IPT-PQ-C2) -> PQ BT.2020 R'G'B', for the warm-up/reload frames only (pushcore
// converts its own frames). The same chain and constant layout as PushFPS's YuvToLinearRgb.hlsl DoviP5ToPqBt2020 /
// DolbyVisionP5.h: per-component reshaping on {1, sqrt(x), x}, chroma centred on 0.5, the RPU's ycc_to_rgb, PQ EOTF,
// rgb_to_lms folded with the HPE LMS->BT.2020 matrix, PQ OETF. dv6.w = 1 when live.
cbuffer D : register(b3) { float4 dv0; float4 dv1; float4 dv2; float4 dv3; float4 dv4; float4 dv5; float4 dv6; };
float DvReshape(float3 curve, float x) { x = saturate(x); return curve.x + curve.y * sqrt(x) + curve.z * x; }
// the exact reshaping (DvPackExact's layout; dvx[177].x = 1 when present): the same evaluators as PushFPS's
// YuvToLinearRgb.hlsl (PickSegment, PolyEvaluate, MmrEvaluate), reading a cbuffer instead of the curve texture
cbuffer DX : register(b4) { float4 dvx[178]; };
float DvMmr(int component, int segment, float3 sig, float constant, int order) {
    const int base = component * 59 + segment * 7;
    float4 sigX; sigX.xyz = sig.xxy * sig.yzz; sigX.w = sigX.x * sig.z;
    float s = constant + dot(dvx[base + 0].xyz, sig) + dot(dvx[base + 1], sigX);
    if (order >= 2) {
        const float3 sig2 = sig * sig; const float4 sigX2 = sigX * sigX;
        s += dot(dvx[base + 2].xyz, sig2) + dot(dvx[base + 3], sigX2);
        if (order >= 3) s += dot(dvx[base + 4].xyz, sig2 * sig) + dot(dvx[base + 5], sigX2 * sigX);
    }
    return saturate(s);
}
float DvPoly(int component, int segment, float s) {
    const float code = s * 1023.0; const float3 coef = dvx[component * 59 + segment * 7].xyz;
    return saturate((coef.x + coef.y * code + coef.z * code * code) / 1023.0);
}
int DvSegment(int component, float s) {
    const int base = component * 59;
    const float pivots[9] = { dvx[base + 56].x, dvx[base + 56].y, dvx[base + 56].z, dvx[base + 56].w, dvx[base + 57].x, dvx[base + 57].y, dvx[base + 57].z, dvx[base + 57].w, dvx[base + 58].x };
    const int pieces = int(dvx[base + 58].w + 0.5);
    int seg = 0;
    [unroll] for (int i = 0; i < 8; ++i) if (i < pieces && s >= pivots[i]) seg = i;
    return min(seg, max(pieces - 1, 0));
}
float3 DvReshapeExact(float3 code) {
    float3 r = code;
    [unroll] for (int c = 0; c < 3; ++c) {
        const float mode = dvx[c * 59 + 58].y;
        if (mode < 0.5) continue;
        const float s0 = c == 0 ? code.x : (c == 1 ? code.y : code.z);
        const int seg = mode > 1.5 ? DvSegment(c, s0) : 0;
        const float4 info = dvx[c * 59 + seg * 7 + 6];
        const float s = info.x > 0.5 ? DvMmr(c, seg, code, info.z, int(info.y + 0.5)) : DvPoly(c, seg, s0);
        if (c == 0) r.x = s; else if (c == 1) r.y = s; else r.z = s;
    }
    return r;
}
float3 DoviP5ToPq(float3 code) { // code: full-range normalised Y, Cb, Cr of the base layer
    float3 ipt = float3(DvReshape(float3(dv0.w, dv1.w, dv2.w), code.x),
                              DvReshape(float3(dv3.w, dv4.w, dv5.w), code.y) - 0.5,
                              DvReshape(dv6.xyz, code.z) - 0.5);
    if (dvx[177].x > 0.5) { const float3 r = DvReshapeExact(saturate(code)); ipt = float3(r.x, r.y - 0.5, r.z - 0.5); }
    const float3 pq = saturate(mul(float3x3(dv0.xyz, dv1.xyz, dv2.xyz), ipt));
    return PqOetf(mul(float3x3(dv3.xyz, dv4.xyz, dv5.xyz), PqEotf(pq)));
}
// the OSD over one output pixel: a dark translucent box, white text (HDR10 outputs: 203 cd/m2 white, in the PQ signal)
float3 OsdApply(float3 rgb, float2 pix, bool pq) {
    if (osdText.w < 0.5 || pix.x < osdBox.x || pix.y < osdBox.y || pix.x >= osdBox.z || pix.y >= osdBox.w) return rgb;
    const float3 white = pq ? PqOetf(float3(203.0, 203.0, 203.0) / 10000.0) : float3(0.93, 0.93, 0.93);
    rgb *= 0.4;
    const float lx = (pix.x - osdText.x) / osdText.z, ly = (pix.y - osdText.y) / osdText.z;
    if (ly < 0.0 || ly >= osdParams.z || lx < 0.0) return rgb;
    float cov = 0.0;
    for (uint i = 0; i < uint(osdParams.x); ++i) {
        const float4 cp = osdChars[i / 2];
        const float start = (i & 1u) ? cp.z : cp.x, code = (i & 1u) ? cp.w : cp.y;
        const float4 g = osdGlyph[uint(code)];
        if (lx >= start && lx < start + g.y) { cov = osdAtlas.SampleLevel(s0, float2((g.x + lx - start) / osdParams.y, ly / osdParams.z), 0).r; break; }
    }
    return lerp(rgb, white, cov);
}
// pushcore RGBA8 (sRGB-encoded) -> the BGRA render target (the format swizzles), scaled when the sizes differ
float4 PS_RGB(VSOut i) : SV_Target { return float4(OsdApply(t0.Sample(s0, i.uv).rgb, i.pos.xy, false), 1); }
// source NV12/P010 -> RGB (warm-up and reload frames into a BGRA output); flags.y: Dolby Vision profile 5 base
// layer (flags.z: sample -> full-range normalised code)
float4 PS_YUV(VSOut i) : SV_Target {
    float3 rgb;
    if (flags.y > 0.5 && dv6.w > 0.5) {
        rgb = DoviP5ToPq(saturate(float3(t0.Sample(s0, i.uv).r, t1.Sample(s0, i.uv).rg) * flags.z));
    } else {
        float y = (t0.Sample(s0, i.uv).r - yuv.x) * yuv.y;
        float2 c = (t1.Sample(s0, i.uv).rg - yuv.z) * yuv.w;
        rgb = saturate(YuvToRgb(y, c));
    }
    if (flags.x > 0.5) rgb = PqToSdr(flags.w > 0.5 ? HlgToPq(rgb) : rgb);
    return float4(rgb, 1);
}
// --- HDR10 outputs (compute; Y410 has no render target support on this GPU, but typed UAVs)
RWTexture2D<uint> uOut : register(u0);            // Y410 through an R32_UINT view: U 0-9, Y 10-19, V 20-29, A 30-31
RWTexture2D<unorm float> uY : register(u1);       // P010 luma plane (R16_UNORM, 10-bit code in the high bits)
RWTexture2D<unorm float2> uUV : register(u2);     // P010 chroma plane (R16G16_UNORM)
cbuffer C : register(b1) { uint2 outSize; uint2 pad; };
// srcSize: the source (upload) texture; codeScale: source sample -> 10-bit code (P010 65535/64, NV12 255*4);
// sdrIn: pushcore's output is SDR RGB8 (converted to PQ); srcSdr: the source frames are SDR (warm-up, converted to PQ)
// dvP5: the source is a Dolby Vision profile 5 base layer (dvNorm: sample -> full-range normalised code)
// itmOn: PushMPC's SDR to HDR curve; itm0 = (decode power, paper white cd/m2, k = M - 1 after the limiter, shoulder start
// t); itm1 = (shoulder power p, per-channel share, dither on, unused)
// itm2 = (RTX Video HDR limiter on, its gain g on the light above paper white, paper white cd/m2, unused)
cbuffer S : register(b2) { uint2 srcSize; float codeScale; float sdrIn; float srcSdr; float dvP5; float dvNorm; float itmOn; float4 itm0; float4 itm1; float4 itm2; };
// PushMPC's SDR to HDR curve: a display-referred, hue-preserving highlight shoulder.
// Pumbo's AutoHDR form (github.com/Filoppi/PumboAutoHDR; Luma's PumboAutoHDR()): the extra light k * pow((x - t) / (1 - t), p)
// above the shoulder start t, nothing below it (the low range stays at plain SDR levels, as ITU-R BT.2446 method B's
// protected range); paper white per ITU-R BT.2408 (203 cd/m2 unless the display is dim). Decode: a pure power (2.2; never
// piecewise sRGB). The gain is taken on s = lerp(max(R,G,B), Y, 2/3) and blended 15 % towards the same curve per channel
// (a little highlight saturation); BT.709 primaries are kept (no gamut expansion). k = 0 is BT.2408's direct mapping.
float ItmExtra(float x) { return itm0.z * pow(saturate((x - itm0.w) / (1.0 - itm0.w)), itm1.x); }
float ItmGain(float x) { return x > 1e-6 ? 1.0 + ItmExtra(x) / x : 1.0; }
float3 ItmPushMpc(float3 c) {
    const float3 L = pow(saturate(c), itm0.x);
    const float Y = dot(L, float3(0.2126, 0.7152, 0.0722));
    const float s = lerp(max(L.r, max(L.g, L.b)), Y, 2.0 / 3.0);
    const float3 g = lerp(ItmGain(s).xxx, float3(ItmGain(L.r), ItmGain(L.g), ItmGain(L.b)), itm1.y);
    return PqOetf(max(mul(M709to2020, L * g * itm0.y), 0.0) / 10000.0);
}
float3 SdrToHdr10(float3 c) { return itmOn > 0.5 ? ItmPushMpc(c) : SdrToPq(c); }
// the bright-scene limiter on RTX Video HDR's output (an SDR source only): the light above paper white x g, on the
// BT.2020 luminance with the colour ratios kept (the same cap as PushMPC's curve, on the expansion only)
float3 RtxLimit(float3 pq) {
    const float3 L = PqEotf(pq) * 10000.0;
    const float Y = dot(L, float3(0.2627, 0.6780, 0.0593));
    if (Y <= itm2.z) return pq;
    return PqOetf(L * ((itm2.z + itm2.y * (Y - itm2.z)) / Y) / 10000.0);
}
// the final 10-bit quantisation of PushMPC's SDR to HDR output: triangular (TPDF) dither of +-1 LSB, a fixed pattern
// (the same on a real frame and its generated frames); never on an HDR or pass-through stream (those stay exact)
static float3 g_dither = 0;
float Hash01(uint2 p, uint k) { uint h = p.x * 1973u + p.y * 9277u + k * 26699u; h = (h ^ 61u) ^ (h >> 16); h *= 9u; h ^= h >> 4; h *= 0x27d4eb2du; h ^= h >> 15; return float(h) * (1.0 / 4294967296.0); }
float Tpdf(uint2 p, uint k) { return Hash01(p, k) + Hash01(p, k + 101u) - 1.0; }
void SetDither(uint2 p, bool on) { g_dither = on && itm1.z > 0.5 ? float3(Tpdf(p, 1u), Tpdf(p, 2u), Tpdf(p, 3u)) : 0.0; }
uint Code10(float v, float d) { return (uint)clamp(round(v + d), 0.0, 1023.0); }
uint PackY410(float3 yuvCodes) { return Code10(yuvCodes.y, g_dither.y) | (Code10(yuvCodes.x, g_dither.x) << 10) | (Code10(yuvCodes.z, g_dither.z) << 20) | (3u << 30); }
float P16(float code10, float d) { return clamp(round(code10 + d), 0.0, 1023.0) * (64.0 / 65535.0); }
// R'G'B' (PQ, BT.2020) -> Y'CbCr BT.2020 non-constant luminance, 10-bit studio codes (Y, Cb, Cr), float, unrounded
float3 Enc2020(float3 c) {
    float y = dot(c, float3(0.2627, 0.6780, 0.0593));
    return float3(64.0 + 876.0 * y, 512.0 + 896.0 * (c.b - y) / 1.8814, 512.0 + 896.0 * (c.r - y) / 1.4746);
}
float3 PushRgb(float2 uv) { float3 c = saturate(t0.SampleLevel(s0, uv, 0).rgb); return sdrIn > 0.5 ? SdrToHdr10(c) : itm2.x > 0.5 ? RtxLimit(c) : c; }
// source codes (Y, Cb, Cr) at a luma/chroma position; an SDR source is converted to the HDR10 signal
float3 SrcCodes(float2 uvY, float2 uvC) {
    const float ys = t0.SampleLevel(s0, uvY, 0).r;
    const float2 cs = t1.SampleLevel(s0, uvC, 0).rg;
    float3 codes = float3(ys, cs) * codeScale;
    if (dvP5 > 0.5 && dv6.w > 0.5) codes = Enc2020(DoviP5ToPq(saturate(float3(ys, cs) * dvNorm)));
    else if (srcSdr > 0.5) codes = Enc2020(SdrToHdr10(saturate(YuvToRgb((codes.x - 64.0) / 876.0, (codes.yz - 512.0) / 896.0))));
    else if (flags.w > 0.5) codes = Enc2020(HlgToPq(YuvToRgb((codes.x - 64.0) / 876.0, (codes.yz - 512.0) / 896.0)));
    return codes;
}
// the source's chroma is left-sited (MPEG-2): at output position uv, its texture coordinate is +0.5 source luma pixel
float2 SrcChromaUv(float2 uv) { return uv + float2(0.5 / srcSize.x, 0.0); }
// pushcore HDR10 (R'G'B' PQ, BT.2020, full range) -> Y410: float maths, one rounding to the 10-bit codes
[numthreads(8, 8, 1)] void CS_RGB2Y410(uint3 id : SV_DispatchThreadID) {
    if (id.x >= outSize.x || id.y >= outSize.y) return;
    SetDither(id.xy, true);   // RGB -> Y'CbCr: new codes, dithered
    uOut[id.xy] = PackY410(Enc2020(OsdApply(PushRgb((float2(id.xy) + 0.5) / float2(outSize)), float2(id.xy) + 0.5, true)));
}
// source NV12/P010 -> Y410 (warm-up/reload frames): chroma upsampled, scaled
[numthreads(8, 8, 1)] void CS_P0102Y410(uint3 id : SV_DispatchThreadID) {
    if (id.x >= outSize.x || id.y >= outSize.y) return;
    SetDither(id.xy, (srcSdr > 0.5 && itmOn > 0.5) || dvP5 > 0.5 || flags.w > 0.5);   // converted source; a pass-through stays exact
    float2 uv = (float2(id.xy) + 0.5) / float2(outSize);
    uOut[id.xy] = PackY410(SrcCodes(uv, SrcChromaUv(uv)));
}
// Y416 (16-bit 4:4:4, preferred): R16G16B16A16_UNORM view: R = Cb, G = Y', B = Cr, A = alpha. The 10-bit studio
// codes are scaled to 16 bits (x 64) without rounding, so pushcore's 10-bit RGB passes the matrix without the rounding
// to 10-bit codes that Y410 needs; a P010 source passes exactly (its 10-bit codes x 64).
RWTexture2D<unorm float4> uOut16 : register(u3);
float4 Y416Of(float3 yuvCodes) { return float4(saturate(yuvCodes.y * 64.0 / 65535.0), saturate(yuvCodes.x * 64.0 / 65535.0), saturate(yuvCodes.z * 64.0 / 65535.0), 1); }
[numthreads(8, 8, 1)] void CS_RGB2Y416(uint3 id : SV_DispatchThreadID) {
    if (id.x >= outSize.x || id.y >= outSize.y) return;
    uOut16[id.xy] = Y416Of(Enc2020(OsdApply(PushRgb((float2(id.xy) + 0.5) / float2(outSize)), float2(id.xy) + 0.5, true)));
}
[numthreads(8, 8, 1)] void CS_P0102Y416(uint3 id : SV_DispatchThreadID) {
    if (id.x >= outSize.x || id.y >= outSize.y) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(outSize);
    uOut16[id.xy] = Y416Of(SrcCodes(uv, SrcChromaUv(uv)));
}
// P010 output (4:2:0, when MPC-VR refuses Y416 and Y410): same matrix and range as Y410. Chroma left-sited (MPEG-2/HDR10:
// co-sited with the even luma column, between the two luma rows): [1 2 1] horizontal x [1 1] vertical average of the
// Cb/Cr of the six luma positions around it.
float3 RgbAt(int2 p) { p = clamp(p, int2(0, 0), int2(outSize) - 1); return OsdApply(PushRgb((float2(p) + 0.5) / float2(outSize)), float2(p) + 0.5, true); }
[numthreads(8, 8, 1)] void CS_RGB2P010_Y(uint3 id : SV_DispatchThreadID) {
    if (id.x >= outSize.x || id.y >= outSize.y) return;
    SetDither(id.xy, true);   // RGB -> Y'CbCr: new codes, dithered
    uY[id.xy] = P16(Enc2020(RgbAt(int2(id.xy))).x, g_dither.x);
}
[numthreads(8, 8, 1)] void CS_RGB2P010_UV(uint3 id : SV_DispatchThreadID) {
    if (id.x >= outSize.x / 2 || id.y >= outSize.y / 2) return;
    float2 cc = 0;
    [unroll] for (int dy = 0; dy < 2; dy++) {
        [unroll] for (int dx = -1; dx <= 1; dx++) {
            cc += (dx == 0 ? 2.0 : 1.0) * Enc2020(RgbAt(int2(id.x * 2 + dx, id.y * 2 + dy))).yz;
        }
    }
    cc /= 8.0;
    SetDither(id.xy + 0x8000u, true);
    uUV[id.xy] = float2(P16(cc.x, g_dither.y), P16(cc.y, g_dither.z));
}
// source NV12/P010 -> P010 at the output size (warm-up/reload frames): luma resampled; chroma resampled at the output's
// left-sited chroma position (luma x = 2i, between rows 2j and 2j+1)
[numthreads(8, 8, 1)] void CS_P0102P010_Y(uint3 id : SV_DispatchThreadID) {
    if (id.x >= outSize.x || id.y >= outSize.y) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(outSize);
    SetDither(id.xy, (srcSdr > 0.5 && itmOn > 0.5) || dvP5 > 0.5 || flags.w > 0.5);   // converted source; a pass-through stays exact
    uY[id.xy] = P16(SrcCodes(uv, SrcChromaUv(uv)).x, g_dither.x);
}
[numthreads(8, 8, 1)] void CS_P0102P010_UV(uint3 id : SV_DispatchThreadID) {
    if (id.x >= outSize.x / 2 || id.y >= outSize.y / 2) return;
    float2 uv = float2((2.0 * id.x + 0.5) / outSize.x, (2.0 * id.y + 1.0) / outSize.y);
    float3 c = SrcCodes(uv, SrcChromaUv(uv));
    SetDither(id.xy + 0x8000u, (srcSdr > 0.5 && itmOn > 0.5) || dvP5 > 0.5 || flags.w > 0.5);
    uUV[id.xy] = float2(P16(c.y, g_dither.y), P16(c.z, g_dither.z));
}
// The Auto limiter's statistics of a real SDR frame (pushcore's RGB8, before the curve), on a 1/8 x 1/8 grid: sums of
// Y * E(s) / s (the extra light at k = 1, E = the shoulder term), Y (linear) and Y' (the signal, for scene changes), x 1024
RWByteAddressBuffer uStats : register(u4);
groupshared uint gsA, gsB, gsM, gsN;
[numthreads(8, 8, 1)] void CS_ItmStats(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    if (gi == 0) { gsA = 0; gsB = 0; gsM = 0; gsN = 0; }
    GroupMemoryBarrierWithGroupSync();
    const uint2 p = id.xy * 8u + 4u;
    if (p.x < outSize.x && p.y < outSize.y && sdrIn < 0.5) {
        // RTX Video HDR's output (PQ BT.2020): light above paper white and up to it, in paper-white units
        const float3 c = saturate(t0.SampleLevel(s0, (float2(p) + 0.5) / float2(outSize), 0).rgb);
        const float Y = dot(PqEotf(c) * 10000.0, float3(0.2627, 0.6780, 0.0593)) / itm2.z;
        uint o;
        InterlockedAdd(gsA, uint(min(max(Y - 1.0, 0.0), 8.0) * 1024.0 + 0.5), o);
        InterlockedAdd(gsB, uint(min(Y, 1.0) * 1024.0 + 0.5), o);
        InterlockedAdd(gsM, uint(dot(c, float3(0.2627, 0.6780, 0.0593)) * 1024.0 + 0.5), o);
        InterlockedAdd(gsN, 1u, o);
    } else if (p.x < outSize.x && p.y < outSize.y) {
        const float3 c = saturate(t0.SampleLevel(s0, (float2(p) + 0.5) / float2(outSize), 0).rgb);
        const float3 L = pow(c, itm0.x);
        const float Y = dot(L, float3(0.2126, 0.7152, 0.0722));
        const float s = lerp(max(L.r, max(L.g, L.b)), Y, 2.0 / 3.0);
        const float e = s > 1e-6 ? Y * pow(saturate((s - itm0.w) / (1.0 - itm0.w)), itm1.x) / s : 0.0;
        uint o;
        InterlockedAdd(gsA, uint(min(e, 4.0) * 1024.0 + 0.5), o);
        InterlockedAdd(gsB, uint(Y * 1024.0 + 0.5), o);
        InterlockedAdd(gsM, uint(dot(c, float3(0.2126, 0.7152, 0.0722)) * 1024.0 + 0.5), o);
        InterlockedAdd(gsN, 1u, o);
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0 && gsN) { uStats.InterlockedAdd(0, gsA); uStats.InterlockedAdd(4, gsB); uStats.InterlockedAdd(8, gsM); uStats.InterlockedAdd(12, gsN); }
}
)";

static HRESULT Compile(const char* entry, const char* target, ComPtr<ID3DBlob>& out)
{
    if (fs::Shaders().Get(entry, target, out)) return S_OK;   // fast start: compiled at the input connection or read from the cache
    ComPtr<ID3DBlob> err;
    HRESULT hr = D3DCompile(kHlsl, strlen(kHlsl), "pushmpcfilter", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &err);
    if (FAILED(hr) && err) Log(std::string("shader compile: ") + (const char*)err->GetBufferPointer());
    return hr;
}
// fast start: every entry point the filter compiles, compiled in parallel once per process and kept in a cache file
static void StartShaderCache()
{
    static const std::vector<fs::ShaderCache::Entry> entries = {
        { "VS", "vs_5_0" }, { "PS_RGB", "ps_5_0" }, { "PS_YUV", "ps_5_0" }, { "CS_RGB2Y410", "cs_5_0" }, { "CS_P0102Y410", "cs_5_0" },
        { "CS_RGB2Y416", "cs_5_0" }, { "CS_P0102Y416", "cs_5_0" }, { "CS_RGB2P010_Y", "cs_5_0" }, { "CS_RGB2P010_UV", "cs_5_0" },
        { "CS_P0102P010_Y", "cs_5_0" }, { "CS_P0102P010_UV", "cs_5_0" }, { "CS_ItmStats", "cs_5_0" } };
    const std::wstring base = LocalAppData();
    fs::Shaders().Start(kHlsl, entries, base.empty() ? std::wstring() : base + L"\\PushMPC\\cache", PUSHMPC_VERSION_STR, &Log);
}
// ------------------------------------------------------------------------------------------------ output formats
// NV12/P010 = the source format (pass-through); Y416/Y410/P010H = processed HDR10 output (in that order of preference)
enum class Fmt { None, BGRA, Y410, NV12, P010, P010H, Y416 };
static const char* FmtName(Fmt f) { switch (f) { case Fmt::BGRA: return "ARGB32 (BGRA)"; case Fmt::Y410: return "Y410"; case Fmt::Y416: return "Y416"; case Fmt::NV12: return "NV12"; case Fmt::P010: return "P010"; case Fmt::P010H: return "P010 (HDR10 output)"; default: return "none"; } }
static DXGI_FORMAT Dxgi(Fmt f) { switch (f) { case Fmt::BGRA: return DXGI_FORMAT_B8G8R8A8_UNORM; case Fmt::Y410: return DXGI_FORMAT_Y410; case Fmt::Y416: return DXGI_FORMAT_Y416; case Fmt::NV12: return DXGI_FORMAT_NV12; case Fmt::P010: case Fmt::P010H: return DXGI_FORMAT_P010; default: return DXGI_FORMAT_UNKNOWN; } }
static bool IsHdrFmt(Fmt f) { return f == Fmt::Y410 || f == Fmt::Y416 || f == Fmt::P010H; }
// the multiplier cap has one source (VFG16 track): pushcore's PUSHCORE_MAX_MULTIPLIER; the filter's settings and Auto follow it
static_assert(pm::kMaxSmoother == int(PUSHCORE_MAX_MULTIPLIER), "kMaxSmoother (PushMpcSettings.h) must equal PUSHCORE_MAX_MULTIPLIER (pushcore_abi.h)");
// MPC Video Renderer's SDR "display nits" (HKCU\Software\MPC-BE Filters\MPC Video Renderer, DisplayNits, 25..400, default
// 125: IVideoRenderer.h SDR_NITS_*): the SDR white its own HDR to SDR tone map uses. Read only; PushMPC's tone map uses the
// same white, so the picture does not change brightness with the route.
static int MpcVrSdrNits()
{
    DWORD v = 0, sz = sizeof(v), type = 0;
    HKEY k = nullptr;
    int nits = 125;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\MPC-BE Filters\\MPC Video Renderer", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExW(k, L"DisplayNits", nullptr, &type, (BYTE*)&v, &sz) == ERROR_SUCCESS && type == REG_DWORD && v >= 25 && v <= 400) nits = int(v);
        RegCloseKey(k);
    }
    return nits;
}
struct OutFmt {
    Fmt fmt = Fmt::None;
    UINT w = 0, h = 0;
    bool operator==(const OutFmt& o) const { return fmt == o.fmt && w == o.w && h == o.h; }
    bool operator!=(const OutFmt& o) const { return !(*this == o); }
    std::string Str() const { return std::format("{} {}x{}", FmtName(fmt), w, h); }
};

class CPushMpcFilter;

// ------------------------------------------------------------------------------------------------ D3D11 sample + allocator
// Side data (IMediaSideData, LAV's contract) travels with the sample: HDR mastering metadata and content light level
// for MPC-VR's HDR passthrough/tone mapping, Dolby Vision and the rest in pass-through.
static const GUID* const kSideGuids[] = { &IID_MediaSideDataHDR, &IID_MediaSideDataHDRContentLightLevel, &IID_MediaSideDataHDR10Plus,
    &IID_MediaSideDataDOVIMetadataV2, &IID_MediaSideDataDOVIRPU, &IID_MediaSideData3DOffset };
struct SideData {
    std::map<GUID, std::vector<BYTE>, bool(*)(const GUID&, const GUID&)> m{ [](const GUID& a, const GUID& b) { return memcmp(&a, &b, sizeof(GUID)) < 0; } };
    void CopyFrom(IUnknown* src, bool all)
    {
        m.clear();
        ComPtr<IMediaSideData> sd;
        if (!src || FAILED(src->QueryInterface(__uuidof(IMediaSideData), (void**)&sd))) return;
        for (const GUID* g : kSideGuids) {
            if (!all && *g != IID_MediaSideDataHDR && *g != IID_MediaSideDataHDRContentLightLevel) continue;
            const BYTE* p = nullptr; size_t n = 0;
            if (SUCCEEDED(sd->GetSideData(*g, &p, &n)) && p && n) m[*g] = std::vector<BYTE>(p, p + n);
        }
    }
};


// ------------------------------------------------------------------------------------------------ Dolby Vision profile 5
// PushFPS's own player converts a profile-5 base layer per frame from its RPU (include/veyra/source/DolbyVisionRpu.h,
// buildDolbyVisionP5, from FFmpeg's AVDOVIMetadata). LAV hands the same parsed RPU as MediaSideDataDOVIMetadata
// (field for field the FFmpeg structure, matrices already as doubles), so this is that function on LAV's layout:
// same 64-sample least-squares projection of each reshaping curve on {1, sqrt(x), x}, same placeholder rule, same
// fixed HPE LMS->BT.2020 matrix folded with the RPU's rgb_to_lms. The result goes to pushcore with each submit and is
// applied on the GPU by PushFPS's YUV pass (and by this filter's warm-up shaders).
struct DvP5 {
    bool active = false, usable = false, unsupported = false, placeholder = false;
    float ycc[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    float lms[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    float curve[3][3] = { { 0, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 } };
    bool exact = false;                 // the exact reshaping below (pushcore PUSHCORE_FEATURE_DV_P5_EXACT, the warm-up shader)
    float exactCurve[3][59][4] = {};    // PushFPS's P7 curve-texture layout, see DvPackExact
};
static constexpr double kDvP5LmsToRgb[9] = { 3.06441879, -2.16597676, 0.10155818, -0.65612108, 1.78554118, -0.12943749, 0.01736321, -0.04725154, 1.03004253 };
using DvCurve = std::remove_reference_t<decltype(std::declval<MediaSideDataDOVIMetadata>().Mapping.curves[0])>;
static int DvPiece(const DvCurve& c, double maxCode, double x)
{
    if (c.num_pivots < 2) return -1;
    for (int i = 0; i + 1 < int(c.num_pivots); ++i) if (x <= double(c.pivots[i + 1]) / maxCode) return i;
    return int(c.num_pivots) - 2;
}
static bool DvFit(const MediaSideDataDOVIMetadata& m, int component, float coeffs[3])
{
    const DvCurve& c = m.Mapping.curves[component];
    if (c.num_pivots < 2 || c.num_pivots > LAV_DOVI_MAX_PIECES + 1) return false;
    if (m.Header.bl_bit_depth == 0 || m.Header.bl_bit_depth > 16) return false;
    const double maxCode = double((1u << m.Header.bl_bit_depth) - 1u);
    const double denom = std::ldexp(1.0, int(m.Header.coef_log2_denom));
    double normal[3][3] = {}, rhs[3] = {};
    for (int s = 0; s < 64; ++s) {
        const double x = double(s) / 63.0;
        const int piece = DvPiece(c, maxCode, x);
        if (piece < 0 || c.mapping_idc[piece] != 0 /* polynomial */ || c.poly_order[piece] > 2) return false;
        double y = 0.0;
        for (int k = int(c.poly_order[piece]); k >= 0; --k) y = y * x + double(c.poly_coef[piece][k]) / denom;
        const double basis[3] = { 1.0, std::sqrt(x), x };
        for (int a = 0; a < 3; ++a) { for (int b = 0; b < 3; ++b) normal[a][b] += basis[a] * basis[b]; rhs[a] += basis[a] * y; }
    }
    double mm[3][4] = {};
    for (int a = 0; a < 3; ++a) { for (int b = 0; b < 3; ++b) mm[a][b] = normal[a][b]; mm[a][3] = rhs[a]; }
    for (int col = 0; col < 3; ++col) {
        int pivot = col;
        for (int row = col + 1; row < 3; ++row) if (std::fabs(mm[row][col]) > std::fabs(mm[pivot][col])) pivot = row;
        if (std::fabs(mm[pivot][col]) < 1e-12) return false;
        if (pivot != col) for (int k = col; k <= 3; ++k) std::swap(mm[col][k], mm[pivot][k]);
        for (int row = col + 1; row < 3; ++row) { const double f = mm[row][col] / mm[col][col]; for (int k = col; k <= 3; ++k) mm[row][k] -= f * mm[col][k]; }
    }
    double sol[3] = {};
    for (int row = 2; row >= 0; --row) {
        double v = mm[row][3];
        for (int k = row + 1; k < 3; ++k) v -= mm[row][k] * sol[k];
        sol[row] = v / mm[row][row];
    }
    for (int k = 0; k < 3; ++k) { if (!std::isfinite(sol[k])) return false; coeffs[k] = float(sol[k]); }
    return true;
}
// The exact reshaping of one RPU (piece-wise polynomial order 1-2 and MMR order 1-3, up to 8 pieces) in PushFPS's P7
// curve-texture layout, evaluated by its PickSegment / PolyEvaluate / MmrEvaluate (and the same code in the warm-up
// shader): per component 8 segments x 7 vec4 + 3 pivot vec4; segment row 6 = (kind 0 poly / 1 MMR, order, MMR
// constant, 0); row 58 = (9th pivot, mode 0 identity / 1 single MMR / 2 segmented, 0, pieces). The polynomial is the
// RPU's on the normalised signal (as libplacebo and MPC-VR evaluate it); PolyEvaluate works on 0..1023 codes, so the
// coefficients are stored as (c0 x 1023, c1, c2 / 1023). False: a mapping outside the specification (a broken RPU).
static bool DvPackExact(const MediaSideDataDOVIMetadata& m, float out[3][59][4])
{
    if (m.Header.bl_bit_depth < 8 || m.Header.bl_bit_depth > 16 || m.Header.coef_log2_denom > 32) return false;
    const double maxCode = double((1u << m.Header.bl_bit_depth) - 1u), scale = std::ldexp(1.0, -int(m.Header.coef_log2_denom));
    std::memset(out, 0, sizeof(float) * 3 * 59 * 4);
    for (int comp = 0; comp < 3; ++comp) {
        const DvCurve& c = m.Mapping.curves[comp];
        const int pieces = int(c.num_pivots) - 1;
        if (pieces < 1 || pieces > LAV_DOVI_MAX_PIECES) return false;
        for (int i = 1; i < int(c.num_pivots); ++i) if (c.pivots[i] < c.pivots[i - 1]) return false;
        bool anyPoly = false, identity = pieces == 1;
        for (int seg = 0; seg < pieces; ++seg) {
            const int base = seg * 7;
            if (c.mapping_idc[seg] == 0) {
                const int order = c.poly_order[seg];
                if (order < 1 || order > 2) return false;
                const double c0 = double(c.poly_coef[seg][0]) * scale, c1 = double(c.poly_coef[seg][1]) * scale, c2 = order >= 2 ? double(c.poly_coef[seg][2]) * scale : 0.0;
                out[comp][base][0] = float(c0 * 1023.0); out[comp][base][1] = float(c1); out[comp][base][2] = float(c2 / 1023.0);
                out[comp][base + 6][0] = 0.0f; out[comp][base + 6][1] = float(order);
                identity = identity && std::fabs(c0) < 1e-9 && std::fabs(c1 - 1.0) < 1e-9 && std::fabs(c2) < 1e-9;
                anyPoly = true;
            } else if (c.mapping_idc[seg] == 1) {
                const int order = c.mmr_order[seg];
                if (order < 1 || order > 3) return false;
                out[comp][base + 6][0] = 1.0f; out[comp][base + 6][1] = float(order); out[comp][base + 6][2] = float(double(c.mmr_constant[seg]) * scale);
                for (int j = 0; j < order; ++j) {
                    for (int k = 0; k < 3; ++k) out[comp][base + j * 2][k] = float(double(c.mmr_coef[seg][j][k]) * scale);
                    for (int k = 0; k < 4; ++k) out[comp][base + j * 2 + 1][k] = float(double(c.mmr_coef[seg][j][3 + k]) * scale);
                }
                identity = false;
            } else return false;
        }
        for (int i = 0; i < int(c.num_pivots); ++i) out[comp][56 + i / 4][i % 4] = float(double(c.pivots[i]) / maxCode);
        out[comp][58][3] = float(pieces);
        out[comp][58][1] = identity ? 0.0f : (pieces > 1 || anyPoly) ? 2.0f : 1.0f;
    }
    return true;
}
static DvP5 BuildDvP5(const MediaSideDataDOVIMetadata& m)
{
    DvP5 out;
    double yccEnergy = 0, lmsEnergy = 0;
    for (int i = 0; i < 9; ++i) { yccEnergy += std::fabs(m.ColorMetadata.ycc_to_rgb_matrix[i]); lmsEnergy += std::fabs(m.ColorMetadata.rgb_to_lms_matrix[i]); }
    if (yccEnergy < 1e-6 || lmsEnergy < 1e-6) return out; // no colour block: not usable
    out.active = true;
    for (int i = 0; i < 9; ++i) out.ycc[i] = float(m.ColorMetadata.ycc_to_rgb_matrix[i]);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            double sum = 0;
            for (int k = 0; k < 3; ++k) sum += kDvP5LmsToRgb[r * 3 + k] * m.ColorMetadata.rgb_to_lms_matrix[k * 3 + c];
            out.lms[r * 3 + c] = float(sum);
        }
    bool placeholder = true;
    int described = 0;
    for (int comp = 0; comp < 3; ++comp) {
        float p[3] = { 0, 0, 1 };
        if (!DvFit(m, comp, p)) { if (m.Mapping.curves[comp].num_pivots >= 2) out.unsupported = true; placeholder = false; continue; }
        ++described;
        if (std::fabs(p[1]) < 1e-4f && std::fabs(p[2]) < 1e-4f) continue; // flat placeholder: identity
        placeholder = false;
        for (int k = 0; k < 3; ++k) out.curve[comp][k] = p[k];
    }
    out.placeholder = placeholder;
    out.usable = described == 3;
    // the exact reshaping: every valid RPU (MMR included); only a broken one falls back to the projection / holding
    out.exact = DvPackExact(m, out.exactCurve);
    if (out.exact) { out.usable = true; out.unsupported = false; out.placeholder = false; }
    return out;
}
// One HDR10 static metadata builder for every path that makes its own (Dolby Vision P5, SDR to HDR, HLG): mastering
// display (primaries: 0 = P3-D65, 1 = BT.2020; max / min cd/m2, min never 0: MPC-VR then ignores MaxCLL) and content
// light levels (cll 0 = none)
static void SetHdr10Side(SideData& s, int primaries, double maxLum, double minLum, unsigned cll, unsigned fall)
{
    MediaSideDataHDR h = {};
    if (primaries == 1) { h.display_primaries_x[0] = 0.170; h.display_primaries_y[0] = 0.797; h.display_primaries_x[1] = 0.131; h.display_primaries_y[1] = 0.046;
                          h.display_primaries_x[2] = 0.708; h.display_primaries_y[2] = 0.292; }   // G, B, R
    else { h.display_primaries_x[0] = 0.265; h.display_primaries_y[0] = 0.690; h.display_primaries_x[1] = 0.150; h.display_primaries_y[1] = 0.060;
           h.display_primaries_x[2] = 0.680; h.display_primaries_y[2] = 0.320; }
    h.white_point_x = 0.3127; h.white_point_y = 0.3290;
    h.max_display_mastering_luminance = maxLum; h.min_display_mastering_luminance = minLum > 0.0 ? minLum : 0.0001;
    s.m[IID_MediaSideDataHDR] = std::vector<BYTE>((const BYTE*)&h, (const BYTE*)&h + sizeof(h));
    if (cll) { MediaSideDataHDRContentLightLevel l = { cll, fall }; s.m[IID_MediaSideDataHDRContentLightLevel] = std::vector<BYTE>((const BYTE*)&l, (const BYTE*)&l + sizeof(l)); }
}
// a source's mastering metadata with min 0 ("unknown"): 0.0001, so MPC-VR uses its MaxCLL (it skips MaxCLL when min is 0)
static void FixHdrMin(SideData& s)
{
    auto it = s.m.find(IID_MediaSideDataHDR);
    if (it == s.m.end() || it->second.size() != sizeof(MediaSideDataHDR)) return;
    auto* h = reinterpret_cast<MediaSideDataHDR*>(it->second.data());
    if (h->min_display_mastering_luminance <= 0.0) h->min_display_mastering_luminance = 0.0001;
}
static double PqToNits(double e)
{
    const double p = std::pow(std::clamp(e, 0.0, 1.0), 1.0 / 78.84375);
    return 10000.0 * std::pow(std::max(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p), 1.0 / 0.1593017578125);
}
// Dolby Vision level 6 (static HDR10 metadata in the RPU): mastering max/min luminance, MaxCLL, MaxFALL; false = absent
static bool DvLevel6(const MediaSideDataDOVIMetadata& m, double& maxLum, double& minLum, unsigned& cll, unsigned& fall)
{
    for (int i = 0; i < LAV_DOVI_MAX_EXTENSIONS; ++i) {
        if (m.Extensions[i].level != 6) continue;
        const auto& l6 = m.Extensions[i].Level6;
        maxLum = l6.max_luminance; minLum = l6.min_luminance / 10000.0; cll = l6.max_cll; fall = l6.max_fall;
        return l6.max_luminance > 0;
    }
    return false;
}
// Input side: LAV attaches side data (HDR mastering metadata, content light level, Dolby Vision RPU) only to samples
// that implement IMediaSideData, i.e. samples of the downstream pin's allocator. The input pin's allocator is a memory
// allocator whose samples carry it; the filter clears it after each frame (samples are recycled).
interface __declspec(uuid("6D3A6E2B-5B0B-4C9E-9F43-2B3C8B6C1A71")) IPushMpcSideClear : public IUnknown { STDMETHOD_(void, ClearSide)() PURE; };
class CSideSample : public CMediaSample, public IMediaSideData, public IPushMpcSideClear
{
public:
    CSideSample(CBaseAllocator* a, HRESULT* phr, LPBYTE buf, LONG len) : CMediaSample(NAME("PushMpc input sample"), a, phr, buf, len) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == __uuidof(IMediaSideData)) { *ppv = static_cast<IMediaSideData*>(this); AddRef(); return S_OK; }
        if (riid == __uuidof(IPushMpcSideClear)) { *ppv = static_cast<IPushMpcSideClear*>(this); AddRef(); return S_OK; }
        return CMediaSample::QueryInterface(riid, ppv);
    }
    STDMETHODIMP_(ULONG) AddRef() override { return CMediaSample::AddRef(); }
    STDMETHODIMP_(ULONG) Release() override { return CMediaSample::Release(); }
    STDMETHODIMP SetSideData(GUID g, const BYTE* p, size_t n) override { if (!p || !n) return E_POINTER; side.m[g] = std::vector<BYTE>(p, p + n); return S_OK; }
    STDMETHODIMP GetSideData(GUID g, const BYTE** pp, size_t* pn) override
    {
        if (!pp || !pn) return E_POINTER;
        auto it = side.m.find(g);
        if (it == side.m.end()) return E_FAIL;
        *pp = it->second.data(); *pn = it->second.size();
        return S_OK;
    }
    STDMETHODIMP_(void) ClearSide() override { side.m.clear(); }
    SideData side;
};
class CSideMemAllocator : public CMemAllocator
{
public:
    CSideMemAllocator(HRESULT* phr) : CMemAllocator(NAME("PushMpc input allocator"), nullptr, phr) {}
protected:
    HRESULT Alloc() override
    {
        // CMemAllocator::Alloc with CSideSample instead of CMediaSample
        CAutoLock lck(this);
        HRESULT hr = CBaseAllocator::Alloc();
        if (FAILED(hr)) return hr;
        if (hr == S_FALSE) return NOERROR;
        if (m_pBuffer) ReallyFree();
        if (m_lSize < 0 || m_lPrefix < 0 || m_lCount < 0) return E_OUTOFMEMORY;
        LONG aligned = m_lSize + m_lPrefix;
        if (m_lAlignment > 1 && aligned % m_lAlignment) aligned += m_lAlignment - aligned % m_lAlignment;
        const LONGLONG total = LONGLONG(m_lCount) * aligned;
        if (total > MAXLONG) return E_OUTOFMEMORY;
        m_pBuffer = (PBYTE)VirtualAlloc(nullptr, SIZE_T(total), MEM_COMMIT, PAGE_READWRITE);
        if (!m_pBuffer) return E_OUTOFMEMORY;
        LPBYTE next = m_pBuffer;
        for (long i = 0; i < m_lCount; i++, next += aligned) {
            auto* s = new CSideSample(this, &hr, next + m_lPrefix, m_lSize);
            if (FAILED(hr)) { delete s; return hr; }
            m_lFree.Add(s);
            m_lAllocated++;
        }
        m_bChanged = FALSE;
        return NOERROR;
    }
};
class CPushInPin : public CTransformInputPin
{
public:
    CPushInPin(CTransformFilter* f, HRESULT* phr) : CTransformInputPin(NAME("PushMpc input"), f, phr, L"Input") {}
    STDMETHODIMP GetAllocator(IMemAllocator** pp) override
    {
        CheckPointer(pp, E_POINTER);
        CAutoLock lck(m_pLock);
        if (!m_pAllocator) {
            HRESULT hr = S_OK;
            auto* a = new CSideMemAllocator(&hr);
            if (FAILED(hr)) { delete a; return hr; }
            a->NonDelegatingQueryInterface(IID_IMemAllocator, (void**)&m_pAllocator);
        }
        m_pAllocator->AddRef();
        *pp = m_pAllocator;
        return S_OK;
    }
};

class CD3D11Sample : public CMediaSample, public IMediaSampleD3D11, public IMediaSideData
{
public:
    CD3D11Sample(CBaseAllocator* a, HRESULT* phr) : CMediaSample(NAME("PushMpc D3D11 sample"), a, phr, nullptr, 0) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == __uuidof(IMediaSampleD3D11)) { *ppv = static_cast<IMediaSampleD3D11*>(this); AddRef(); return S_OK; }
        if (riid == __uuidof(IMediaSideData)) { *ppv = static_cast<IMediaSideData*>(this); AddRef(); return S_OK; }
        return CMediaSample::QueryInterface(riid, ppv);
    }
    STDMETHODIMP_(ULONG) AddRef() override { return CMediaSample::AddRef(); }
    STDMETHODIMP_(ULONG) Release() override { return CMediaSample::Release(); }
    STDMETHODIMP GetD3D11Texture(int nView, ID3D11Texture2D** ppTexture, UINT* pArraySlice) override
    {
        if (nView != 0 || !ppTexture || !tex) return E_INVALIDARG;
        *ppTexture = tex.Get(); tex->AddRef();
        if (pArraySlice) *pArraySlice = 0;
        return S_OK;
    }
    STDMETHODIMP SetSideData(GUID g, const BYTE* p, size_t n) override { if (!p || !n) return E_POINTER; side.m[g] = std::vector<BYTE>(p, p + n); return S_OK; }
    STDMETHODIMP GetSideData(GUID g, const BYTE** pp, size_t* pn) override
    {
        if (!pp || !pn) return E_POINTER;
        auto it = side.m.find(g);
        if (it == side.m.end()) return E_FAIL;
        *pp = it->second.data(); *pn = it->second.size();
        return S_OK;
    }
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11UnorderedAccessView> uav; // Y410 (R32_UINT view)
    OutFmt fmt;
    SideData side;
    int index = 0;
};

class CD3D11Allocator : public CBaseAllocator
{
public:
    CD3D11Allocator(HRESULT* phr) : CBaseAllocator(NAME("PushMpc D3D11 allocator"), nullptr, phr, TRUE, FALSE) {}
    ~CD3D11Allocator() override { Decommit(); ReallyFree(); }
    ComPtr<ID3D11Device> device;
    OutFmt fmt;
    // (re)creates the sample's texture for a format/size; only for a sample that is not in use (just taken from the free list)
    static HRESULT EnsureTexture(ID3D11Device* dev, CD3D11Sample* s, const OutFmt& f)
    {
        if (s->tex && s->fmt == f) return S_OK;
        s->tex.Reset(); s->rtv.Reset(); s->uav.Reset(); s->fmt = {};
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = f.w; td.Height = f.h; td.MipLevels = 1; td.ArraySize = 1; td.Format = Dxgi(f.fmt);
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        const bool yuv420 = f.fmt == Fmt::NV12 || f.fmt == Fmt::P010 || f.fmt == Fmt::P010H;
        // P010H: bound like decoder textures (MPC-VR's D3D11 VP takes them as input; an NV12 texture without
        // BIND_DECODER crashed stock MPC-VR's VP input setup, one with it played). Filled by a copy from a UAV scratch.
        td.BindFlags = f.fmt == Fmt::P010H ? (D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE)
            : yuv420 ? D3D11_BIND_SHADER_RESOURCE /* source-format samples: not used (see Fallback) */
            : (f.fmt == Fmt::Y410 || f.fmt == Fmt::Y416) ? (D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE) : (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        HRESULT hr = dev->CreateTexture2D(&td, nullptr, &s->tex);
        if (SUCCEEDED(hr) && (f.fmt == Fmt::Y410 || f.fmt == Fmt::Y416)) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
            ud.Format = f.fmt == Fmt::Y416 ? DXGI_FORMAT_R16G16B16A16_UNORM : DXGI_FORMAT_R32_UINT;
            ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            hr = dev->CreateUnorderedAccessView(s->tex.Get(), &ud, &s->uav);
        } else if (SUCCEEDED(hr) && !yuv420) {
            D3D11_RENDER_TARGET_VIEW_DESC rd = {};
            rd.Format = td.Format;
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            hr = dev->CreateRenderTargetView(s->tex.Get(), &rd, &s->rtv);
        }
        if (FAILED(hr)) { Log(std::format("sample texture {} failed 0x{:08X}", f.Str(), uint32_t(hr))); s->tex.Reset(); s->rtv.Reset(); s->uav.Reset(); return hr; }
        s->fmt = f;
        return S_OK;
    }
protected:
    void Free() override { ReallyFree(); }
    HRESULT Alloc() override
    {
        CAutoLock lck(this);
        HRESULT hr = CBaseAllocator::Alloc();
        if (FAILED(hr)) return hr;
        // S_FALSE = requirements unchanged; the samples still exist only if a decommit has not freed them (Free() deletes
        // them, so a Stop -> Pause, e.g. a rate change, recommits with none: allocate again then)
        if (hr == S_FALSE && m_lAllocated == m_lCount) return S_OK;
        ReallyFree();
        if (!device) return E_FAIL;
        for (long i = 0; i < m_lCount; i++) {
            HRESULT hrs = S_OK;
            auto* s = new CD3D11Sample(this, &hrs);
            hr = EnsureTexture(device.Get(), s, fmt);
            if (FAILED(hr)) { delete s; return hr; }
            s->index = int(i);
            m_lFree.Add(s);
            m_lAllocated++;
        }
        m_bChanged = FALSE;
        Log(std::format("allocator: {} samples {} allocated", m_lCount, fmt.Str()));
        return S_OK;
    }
    void ReallyFree()
    {
        CMediaSample* s;
        while ((s = m_lFree.RemoveHead()) != nullptr) delete s;
        m_lAllocated = 0;
    }
};

// ------------------------------------------------------------------------------------------------ pins
class CPushOutPin : public CTransformOutputPin
{
public:
    CPushOutPin(CTransformFilter* f, HRESULT* phr) : CTransformOutputPin(NAME("PushMpc output"), f, phr, L"Output") {}
    HRESULT InitAllocator(IMemAllocator** ppAlloc) override;
    CD3D11Allocator* ours = nullptr; // set by InitAllocator in processed mode (the pin's m_pAllocator holds the reference)
    void SetMt(const CMediaType& mt) { CAutoLock lck(m_pLock); m_mt = mt; }
};

// ------------------------------------------------------------------------------------------------ filter
struct ReadyItem {
    CD3D11Sample* sample = nullptr; // AddRef'd
    REFERENCE_TIME start = 0, stop = 0;
    int kind = 0;                   // 0 real, 1 generated, 2 source (warm-up, reload, pass-through), 3 EOS marker
    uint64_t src = 0;
    uint32_t gen = 0;               // flush generation
    int64_t copiedQpc = 0;
};

class CPushMpcFilter : public CTransformFilter, public ISpecifyPropertyPages
{
public:
    DECLARE_IUNKNOWN
    static CUnknown* WINAPI CreateInstance(LPUNKNOWN pUnk, HRESULT* phr)
    {
        auto* f = new CPushMpcFilter(pUnk, phr);
        if (!f) *phr = E_OUTOFMEMORY;
        return f;
    }
    CPushMpcFilter(LPUNKNOWN pUnk, HRESULT* phr) : CTransformFilter(NAME("PushMpcFilter"), pUnk, CLSID_PushMpcFilter)
    {
        m_cfg = ReadConfig();
        m_hWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        m_hLease = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        m_hSubmit = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        m_hTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!m_hTimer) m_hTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        m_settingsSeen = g_settingsSerial.load();
        Log(std::format("filter {} created ({}); settings: enable {}, smoother {}, sharper {}, clean up {}, SDR to HDR {}, FG quality {}, upscale to {}, clean-up strength {}, "
            "upscale style {}, deband {} ({}); HDR {}, lead {}, admission {}",
            PUSHMPC_VERSION_STR, (void*)this, m_cfg.enable, Narrow(pm::ChoiceStr(m_cfg.user.smoother)), Narrow(pm::ChoiceStr(m_cfg.user.sharper)),
            Narrow(pm::OnOffStr(m_cfg.user.cleanUp)), Narrow(pm::OnOffStr(m_cfg.user.sdrToHdr)), Narrow(pm::FgQualityStr(m_cfg.user.fgQuality)),
            Narrow(pm::TargetStr(m_cfg.user.target)), Narrow(pm::StrengthStr(m_cfg.user.strength)),
            Narrow(pm::VsrStyleStr(m_cfg.user.vsrStyle)), Narrow(pm::OnOffStr(m_cfg.user.deband)), Narrow(pm::DebandStrengthStr(m_cfg.user.debandStrength)),
            m_cfg.hdrPushFps ? "HDR to SDR by PushMPC (HdrPushFpsToneMap)" : Narrow(std::wstring(L"HDR to SDR ") + pm::HdrToSdrStr(m_cfg.user.hdrToSdr)), m_cfg.leadUs, m_cfg.admission));
    }
    ~CPushMpcFilter() override
    {
        // timed per step (RC1 F7: one close took 9.2 s; the exit helper's line says how long after these steps it ran)
        const int64_t q0 = Qpc();
        ExitMark("filter teardown started");
        if (!m_showKey.empty() && pm::CurrentShow() == m_showKey) pm::SetCurrentShow({});   // round 5: no show playing now
        StopThreads();
        const int64_t q1 = Qpc();
        StopWatcher();
        StopAuto();
        const int64_t q2 = Qpc();
        CloseCore();
        if (m_prewarmed && g_prewarmReleaseFn) g_prewarmReleaseFn();   // nothing of the prewarm outlives the filter
        const int64_t q3 = Qpc();
        for (HANDLE h : { m_hWake, m_hLease, m_hSubmit, m_hTimer }) if (h) CloseHandle(h);
        if (m_csv) fclose(m_csv);
        if (m_gpuCsv) fclose(m_gpuCsv);
        Log(std::format("filter destroyed: teardown {:.1f} ms (threads {:.1f}, watcher + Auto {:.1f}, pushcore {:.1f}, files {:.1f}); the D3D11 objects are released next",
            QpcMs(Qpc() - q0), QpcMs(q1 - q0), QpcMs(q2 - q1), QpcMs(q3 - q2), QpcMs(Qpc() - q3)));
        ExitMark("filter destroyed");
        m_teardownTail.q = Qpc();
    }
    STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == IID_ISpecifyPropertyPages) return GetInterface(static_cast<ISpecifyPropertyPages*>(this), ppv);
        return CTransformFilter::NonDelegatingQueryInterface(riid, ppv);
    }
    STDMETHODIMP GetPages(CAUUID* pPages) override
    {
        if (!pPages) return E_POINTER;
        pPages->cElems = 2;
        pPages->pElems = (GUID*)CoTaskMemAlloc(2 * sizeof(GUID));
        if (!pPages->pElems) return E_OUTOFMEMORY;
        pPages->pElems[0] = CLSID_PushMpcPage;
        pPages->pElems[1] = CLSID_PushMpcAboutPage;
        return S_OK;
    }

    CBasePin* GetPin(int n) override
    {
        HRESULT hr = S_OK;
        if (!m_pInput) {
            m_pInput = new CPushInPin(this, &hr);
            m_pOutput = new CPushOutPin(this, &hr);
        }
        return n == 0 ? (CBasePin*)m_pInput : n == 1 ? (CBasePin*)m_pOutput : nullptr;
    }
    CPushOutPin* Out() { return static_cast<CPushOutPin*>(m_pOutput); }

    // --- media types
    static bool ParseVideo(const AM_MEDIA_TYPE* mt, BITMAPINFOHEADER& bih, REFERENCE_TIME& atpf, DWORD& ctrl, DWORD& interlace, DWORD& arx, DWORD& ary, RECT* src = nullptr)
    {
        ctrl = interlace = arx = ary = 0;
        if (src) *src = {};
        if (mt->formattype == FORMAT_VideoInfo2 && mt->cbFormat >= sizeof(VIDEOINFOHEADER2)) {
            auto* v = (const VIDEOINFOHEADER2*)mt->pbFormat;
            bih = v->bmiHeader; atpf = v->AvgTimePerFrame; ctrl = v->dwControlFlags; interlace = v->dwInterlaceFlags;
            arx = v->dwPictAspectRatioX; ary = v->dwPictAspectRatioY;
            if (src) *src = v->rcSource;
            return true;
        }
        if (mt->formattype == FORMAT_VideoInfo && mt->cbFormat >= sizeof(VIDEOINFOHEADER)) {
            auto* v = (const VIDEOINFOHEADER*)mt->pbFormat;
            bih = v->bmiHeader; atpf = v->AvgTimePerFrame;
            if (src) *src = v->rcSource;
            return true;
        }
        return false;
    }
    HRESULT CheckInputType(const CMediaType* mt) override
    {
        if (mt->majortype != MEDIATYPE_Video) return VFW_E_TYPE_NOT_ACCEPTED;
        if (mt->subtype != MEDIASUBTYPE_NV12 && mt->subtype != MEDIASUBTYPE_P010_PM) return VFW_E_TYPE_NOT_ACCEPTED;
        BITMAPINFOHEADER bih; REFERENCE_TIME atpf; DWORD c, il, ax, ay;
        if (!ParseVideo(mt, bih, atpf, c, il, ax, ay)) return VFW_E_TYPE_NOT_ACCEPTED;
        if (bih.biWidth <= 0 || bih.biHeight == 0 || (bih.biWidth & 1) || (labs(bih.biHeight) & 1)) return VFW_E_TYPE_NOT_ACCEPTED;
        return S_OK;
    }
    void GuessOutSize(UINT& w, UINT& h) const
    {
        // the 4K-fit guess at connect time (pushcore's real size is known after the open; a dynamic type change follows)
        const double s = std::min(3840.0 / m_visW, 2160.0 / m_visH);
        w = UINT(m_visW * s + 0.5) & ~1u; h = UINT(m_visH * s + 0.5) & ~1u;
    }
    bool InputHdr() const { DXVA2_ExtendedFormat ex; ex.value = m_inCtrl; return (m_inCtrl & AMCONTROL_COLORINFO_PRESENT) && (ex.VideoTransferFunction == kTransPQ || ex.VideoTransferFunction == kTransHLG); }
    // declared first, so destroyed last: how long releasing the members (D3D11 device, textures, shaders) took
    struct TeardownTail {
        int64_t q = 0; HANDLE watch = nullptr;
        ~TeardownTail()
        {
            if (q) {
                Log(std::format("filter members released in {:.1f} ms", QpcMs(Qpc() - q)));
                // RC2 F2: re-armed here, so that every DLL loaded since the last open (an Auto rebuild, a method switch, the
                // property page) is initialised before the helper and its detach runs after the helper's at process exit
                if (ExitHelperArmed()) ArmExitHelper("re-armed at the end of the teardown");
                ExitMark("filter members released");
            }
            if (watch) { SetEvent(watch); CloseHandle(watch); }
        }
    } m_teardownTail;
    // --- Dolby Vision profile 5 (pushcore converts it; see DvP5)
    bool m_dvP5 = false;          // the stream is a profile 5 base layer, converted per frame
    DvP5 m_dvCur;                 // the conversion of the latest source frame (the warm-up shaders use it)
    DvP5 m_dvLastGood;            // the last usable conversion (held for a frame whose RPU cannot be converted)
    uint32_t m_dvUnusable = 0;    // consecutive source frames whose RPU could not be converted
    bool CoreSupportsDvP5()
    {
        std::wstring err;
        if (!m_rt.error.empty() || m_rt.root.empty()) m_rt = ResolveRuntime(m_cfg);
        if (!m_rt.error.empty() || !LoadApi(m_rt, err)) return false;
        return (g_coreFeatures & PUSHCORE_FEATURE_DV_P5) != 0;
    }
    // the signal pushcore converts a profile 5 base layer into, as PushFPS pins it (MediaFileSource: PQ, BT.2020 NCL,
    // BT.2020); IPT-PQ-C2 is full range by definition
    void SetDvP5Colour()
    {
        DXVA2_ExtendedFormat ex; ex.value = 0;
        ex.SampleFormat = AMCONTROL_USED | AMCONTROL_COLORINFO_PRESENT;
        ex.NominalRange = DXVA2_NominalRange_0_255;
        ex.VideoTransferMatrix = 4;
        ex.VideoPrimaries = 9;
        ex.VideoTransferFunction = kTransPQ;
        m_inCtrl = ex.value;
    }
    static DvP5 DvOf(IUnknown* sample)
    {
        ComPtr<IMediaSideData> sd;
        const BYTE* p = nullptr; size_t n = 0;
        if (sample && SUCCEEDED(sample->QueryInterface(__uuidof(IMediaSideData), (void**)&sd))
            && SUCCEEDED(sd->GetSideData(IID_MediaSideDataDOVIMetadataV2, &p, &n)) && p && n == sizeof(MediaSideDataDOVIMetadata))
            return BuildDvP5(*reinterpret_cast<const MediaSideDataDOVIMetadata*>(p));
        return {};
    }
    // Side data for frames converted from profile 5: the RPU is removed (they are HDR10 now; MPC-VR with "prefer
    // Dolby Vision" would otherwise reshape them a second time) and HDR10 static metadata from the RPU's level 6 is
    // added where the stream has none (mastering primaries: P3 D65, as level 6 has none)
    void ToP5Output(SideData& s) const
    {
        auto dovi = s.m.find(IID_MediaSideDataDOVIMetadataV2);
        if (dovi != s.m.end() && dovi->second.size() == sizeof(MediaSideDataDOVIMetadata) && !s.m.count(IID_MediaSideDataHDR)) {
            double maxLum = 0, minLum = 0; unsigned cll = 0, fall = 0;
            const auto& md = *reinterpret_cast<const MediaSideDataDOVIMetadata*>(dovi->second.data());
            if (!DvLevel6(md, maxLum, minLum, cll, fall) && md.ColorMetadata.source_max_pq > 0) {
                // no level 6: the mastering range from the RPU's source_min_pq / source_max_pq (12-bit PQ), as MPC-VR does
                maxLum = std::round(PqToNits(md.ColorMetadata.source_max_pq / 4095.0)); minLum = PqToNits(md.ColorMetadata.source_min_pq / 4095.0);
            }
            if (maxLum > 0) {
                SetHdr10Side(s, 0, maxLum, minLum, cll, fall);
            }
        }
        if (m_sceneCll.load() && dovi != s.m.end()) SceneMaxCll(s);   // L1 while the RPU is still here
        s.m.erase(IID_MediaSideDataDOVIMetadataV2);
        s.m.erase(IID_MediaSideDataDOVIRPU);
    }
    // Builds the media type for an output format. Pass-through formats copy the input type (with its colour info).
    void BuildMediaType(const OutFmt& f, CMediaType* pmt) const
    {
        if (f.fmt == Fmt::NV12 || f.fmt == Fmt::P010) {
            *pmt = m_inMt;
            return;
        }
        const bool y410 = f.fmt == Fmt::Y410, y416 = f.fmt == Fmt::Y416, p010 = f.fmt == Fmt::P010H, hdr = y410 || y416 || p010;
        pmt->InitMediaType();
        pmt->SetType(&MEDIATYPE_Video);
        pmt->SetSubtype(y410 ? &MEDIASUBTYPE_Y410_PM : y416 ? &MEDIASUBTYPE_Y416_PM : p010 ? &MEDIASUBTYPE_P010_PM : &MEDIASUBTYPE_ARGB32);
        pmt->SetFormatType(&FORMAT_VideoInfo2);
        pmt->SetTemporalCompression(FALSE);
        const ULONG size = p010 ? f.w * f.h * 3 : y416 ? f.w * f.h * 8 : f.w * f.h * 4;
        pmt->SetSampleSize(size);
        auto* v = (VIDEOINFOHEADER2*)pmt->AllocFormatBuffer(sizeof(VIDEOINFOHEADER2));
        ZeroMemory(v, sizeof(*v));
        v->rcSource = { 0, 0, LONG(f.w), LONG(f.h) };
        v->rcTarget = v->rcSource;
        v->AvgTimePerFrame = m_atpf / std::max<uint32_t>(1u, m_mult.load());
        // display aspect ratio of the source picture
        const DWORD ax = m_inArX ? m_inArX : m_visW, ay = m_inArY ? m_inArY : m_visH;
        v->dwPictAspectRatioX = ax; v->dwPictAspectRatioY = ay;
        if (hdr) {
            // pushcore's HDR output is always HDR10 (PQ, BT.2020), also for an HLG source (PushFPS converts it)
            DXVA2_ExtendedFormat ex; ex.value = 0;
            ex.SampleFormat = AMCONTROL_USED | AMCONTROL_COLORINFO_PRESENT;
            ex.VideoChromaSubsampling = p010 ? DXVA2_VideoChromaSubsampling_MPEG2 : DXVA2_VideoChromaSubsampling_Unknown;
            ex.NominalRange = DXVA2_NominalRange_16_235;
            ex.VideoTransferMatrix = 4;   // MFVideoTransferMatrix_BT2020_10
            ex.VideoLighting = DXVA2_VideoLighting_dim;
            ex.VideoPrimaries = 9;        // MFVideoPrimaries_BT2020
            ex.VideoTransferFunction = kTransPQ;
            v->dwControlFlags = ex.value;
        }
        v->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        v->bmiHeader.biWidth = LONG(f.w);
        v->bmiHeader.biHeight = hdr ? LONG(f.h) : -LONG(f.h); // top-down for BI_RGB; FOURCC types use positive heights
        v->bmiHeader.biPlanes = 1;
        v->bmiHeader.biBitCount = p010 ? 24 : y416 ? 64 : 32;
        v->bmiHeader.biCompression = y410 ? MAKEFOURCC('Y', '4', '1', '0') : y416 ? MAKEFOURCC('Y', '4', '1', '6') : p010 ? MAKEFOURCC('P', '0', '1', '0') : BI_RGB;
        v->bmiHeader.biSizeImage = size;
    }
    // The colour description of the stream LAV decodes, from the type on LAV's own input (the splitter's VIDEOINFOHEADER2
    // or MPEG2VIDEOINFO): LAV's output type at connect time often has none (it attaches it to the first sample), but
    // the output format (SDR BGRA or HDR10 Y410/P010) must be chosen at connect time.
    DWORD UpstreamCtrl() const
    {
        IPin* lavOut = m_pInput->GetConnected();
        if (!lavOut) return 0;
        PIN_INFO pi = {};
        if (FAILED(lavOut->QueryPinInfo(&pi)) || !pi.pFilter) return 0;
        ComPtr<IBaseFilter> f;
        f.Attach(pi.pFilter);
        ComPtr<IEnumPins> e;
        if (FAILED(f->EnumPins(&e))) return 0;
        DWORD r = 0;
        ComPtr<IPin> p;
        while (!r && e->Next(1, &p, nullptr) == S_OK) {
            PIN_DIRECTION d = PINDIR_OUTPUT;
            p->QueryDirection(&d);
            AM_MEDIA_TYPE mt = {};
            if (d == PINDIR_INPUT && SUCCEEDED(p->ConnectionMediaType(&mt))) {
                const VIDEOINFOHEADER2* v = nullptr;
                if (mt.formattype == FORMAT_VideoInfo2 && mt.cbFormat >= sizeof(VIDEOINFOHEADER2)) v = (const VIDEOINFOHEADER2*)mt.pbFormat;
                else if (mt.formattype == FORMAT_MPEG2Video && mt.cbFormat >= sizeof(MPEG2VIDEOINFO)) v = &((const MPEG2VIDEOINFO*)mt.pbFormat)->hdr;
                if (v && (v->dwControlFlags & AMCONTROL_COLORINFO_PRESENT)) r = v->dwControlFlags;
                FreeMediaType(mt);
            }
            p.Reset();
        }
        return r;
    }
    // The colour description in the stream itself (HEVC SPS VUI in the splitter's type), or from a Dolby Vision
    // configuration record whose base layer signals HDR10 (compatibility id 1 or 6) or HLG (4); 0 = not found.
    DWORD UpstreamBitstreamCtrl() const
    {
        IPin* lavOut = m_pInput->GetConnected();
        if (!lavOut) return 0;
        PIN_INFO pi = {};
        if (FAILED(lavOut->QueryPinInfo(&pi)) || !pi.pFilter) return 0;
        ComPtr<IBaseFilter> f;
        f.Attach(pi.pFilter);
        ComPtr<IEnumPins> e;
        if (FAILED(f->EnumPins(&e))) return 0;
        DWORD r = 0;
        ComPtr<IPin> p;
        while (!r && e->Next(1, &p, nullptr) == S_OK) {
            PIN_DIRECTION d = PINDIR_OUTPUT;
            p->QueryDirection(&d);
            AM_MEDIA_TYPE mt = {};
            if (d == PINDIR_INPUT && SUCCEEDED(p->ConnectionMediaType(&mt))) {
                SpsColour c;
                if (mt.pbFormat && FindHevcSpsColour(mt.pbFormat, mt.cbFormat, c)) {
                    r = CtrlFromH273(c);
                    Log(std::format("colour description from the HEVC SPS in the splitter's type: {}x{}, primaries {} transfer {} matrix {} {} range",
                        c.w, c.h, c.primaries, c.transfer, c.matrix, c.full ? "full" : "limited"));
                }
                for (ULONG i = 0; !r && mt.pbFormat && i + 9 <= mt.cbFormat; i++) {
                    const BYTE* b = mt.pbFormat + i;
                    if (b[0] == 'd' && b[1] == 'v' && (b[2] == 'c' || b[2] == 'v' || b[2] == 'w') && b[3] == 'C') {
                        const int profile = b[6] >> 1, compat = b[8] >> 4;
                        if (compat == 1 || compat == 6 || compat == 4) {
                            SpsColour dc; dc.primaries = 9; dc.matrix = 9; dc.transfer = compat == 4 ? 18 : 16;
                            r = CtrlFromH273(dc);
                            Log(std::format("colour from the Dolby Vision configuration record: profile {}, base layer compatibility {} ({})", profile, compat, compat == 4 ? "HLG" : "HDR10"));
                        }
                        break;
                    }
                }
                FreeMediaType(mt);
            }
            p.Reset();
        }
        return r;
    }
    // Dolby Vision profile from the stream's configuration record (dvcC/dvvC/dvwC: version major, minor, then
    // profile(7) level(6) rpu(1) el(1) bl(1)) if the splitter's type carries it; -1 = not found.
    int UpstreamDvProfile() const
    {
        IPin* lavOut = m_pInput->GetConnected();
        if (!lavOut) return -1;
        PIN_INFO pi = {};
        if (FAILED(lavOut->QueryPinInfo(&pi)) || !pi.pFilter) return -1;
        ComPtr<IBaseFilter> f;
        f.Attach(pi.pFilter);
        ComPtr<IEnumPins> e;
        if (FAILED(f->EnumPins(&e))) return -1;
        int profile = -1;
        ComPtr<IPin> p;
        while (profile < 0 && e->Next(1, &p, nullptr) == S_OK) {
            PIN_DIRECTION d = PINDIR_OUTPUT;
            p->QueryDirection(&d);
            AM_MEDIA_TYPE mt = {};
            if (d == PINDIR_INPUT && SUCCEEDED(p->ConnectionMediaType(&mt))) {
                const BYTE* b = mt.pbFormat;
                for (ULONG i = 0; b && i + 9 <= mt.cbFormat; i++) {
                    if (b[i] == 'd' && b[i + 1] == 'v' && (b[i + 2] == 'c' || b[i + 2] == 'v' || b[i + 2] == 'w') && b[i + 3] == 'C') {
                        profile = b[i + 6] >> 1;
                        Log(std::format("Dolby Vision configuration record upstream: version {}.{}, profile {}", b[i + 4], b[i + 5], profile));
                        break;
                    }
                }
                FreeMediaType(mt);
            }
            p.Reset();
        }
        return profile;
    }
    // The processed formats offered at connect time, in order, or the reason why the stream passes through.
    // HDR10 stream (PQ/HLG): Y410 (4:4:4), then P010 (4:2:0), then BGRA (PushFPS tone-maps to SDR). SDR: BGRA.
    // The format is fixed for the session: a later colour change on the first sample does not switch it (a runtime
    // type change into the stock renderer crashed it once); the content is converted into the connected signal.
    std::wstring Eligibility(std::vector<OutFmt>& fmts)
    {
        fmts.clear();
        if (!m_cfg.forceFallback.empty()) return L"ForceFallback: " + m_cfg.forceFallback;
        if (!m_cfg.enable) return L"PushMPC is switched off (Enable PushMPC on its property page)";
        if (m_inInterlaced) return L"interlaced input (one field per sample)";
        if (m_typeInterlaced) {
            // a file whose frames this process already found interlaced (it was reopened for that): the source format,
            // so MPC Video Renderer deinterlaces it
            const std::wstring f = SourceFile();
            std::lock_guard<std::mutex> lk(g_ilLock);
            if (!f.empty() && g_interlacedFiles.count(f)) return L"interlaced input (the decoder marks its frames interlaced)";
        }
        if (!HasHardwareAdapter()) return L"no DirectX 12 hardware adapter";
        m_rt = ResolveRuntime(m_cfg);
        if (!m_rt.error.empty()) return m_rt.error;
        Prewarm();
        if (const int dv = UpstreamDvProfile(); dv == 5 || dv == 4) {
            // IPT-PQ-C2 base layer: converted per frame from its RPU, as PushFPS's own player does (pushcore's
            // Dolby Vision profile 5 path), then processed as HDR10. Without that pushcore feature: MPC-VR's own Dolby
            // Vision path through the pass-through (the decoded YCbCr plus the RPU, untouched).
            if (!CoreSupportsDvP5()) return std::format(L"Dolby Vision profile {} and this pushcore.dll has no profile 5 conversion: MPC Video Renderer's own Dolby Vision path", dv);
            if (!m_dvP5) { m_dvP5 = true; SetDvP5Colour(); Log(std::format("Dolby Vision profile {} (from the stream's configuration record): converted by pushcore, output HDR10", dv)); }
        }
        if (!m_inCtrl) {
            if (const DWORD up = UpstreamCtrl()) {
                m_inCtrl = up;
                DXVA2_ExtendedFormat ex; ex.value = up;
                Log(std::format("colour description from the upstream type: range {} matrix {} primaries {} transfer {}", (int)ex.NominalRange,
                    (int)ex.VideoTransferMatrix, (int)ex.VideoPrimaries, (int)ex.VideoTransferFunction));
            } else if (const DWORD bs = UpstreamBitstreamCtrl()) {
                m_inCtrl = bs;
            } else if (!m_loggedNoColour) {
                m_loggedNoColour = true;
                Log("no colour description at connect time (LAV, its upstream type, the HEVC SPS, a Dolby Vision record): SDR output; an HDR stream is then tone-mapped by PushFPS");
            }
        }
        UINT w, h; GuessOutSize(w, h);
        ProbeBitrate();
        bool sdrToHdr = false;
        if (!LegacyMode() && !InputHdr()) {
            // SDR to HDR (RTX Video HDR) needs an HDR10 output: decided now, from the window's display (the format
            // cannot change while playing)
            EnsureGpu(0);
            // only Windows HDR matters here: no VRR probe (NVAPI and the driver profile take ~200 ms; the open asks again)
            m_display = pm::QueryDisplay(pm::PlayerWindow(), pm::VrrMode::Off, m_gpu.nvidia());
            const pm::Choice c = DecideNow(m_cfg.user, m_display);
            sdrToHdr = c.hdr;
            SetItm(c, c.hdr);
            Log(std::format("connect-time Auto: display {} {:.3f} Hz {}x{} Windows HDR {}; SDR to HDR {}", Narrow(m_display.name), m_display.refreshHz,
                m_display.width, m_display.height, m_display.hdr ? "on" : "off", Narrow(c.hdrNote)));
        }
        m_coreToneMap = InputHdr() && m_cfg.hdrPushFps;   // the legacy (PushFPS session) route: only the debug switch
        if (!LegacyMode() && InputHdr()) {
            // HDR to SDR (Windows HDR off): PushMPC tone-maps (BGRA out) or the renderer does (HDR10 out). Decided now, as
            // the output format cannot change while playing: Auto's plan for this display (HdrToSdr Auto: the cheaper one)
            EnsureGpu(0);
            m_display = pm::QueryDisplay(pm::PlayerWindow(), m_cfg.user.vrr, m_gpu.nvidia());
            const pm::Choice c = DecideNow(m_cfg.user, m_display);
            m_coreToneMap = !m_display.hdr && (m_cfg.hdrPushFps || c.coreToneMap);
            Log(std::format("connect-time Auto: display {} {:.3f} Hz {}x{} Windows HDR {} {}; HDR to SDR {}{}", Narrow(m_display.name), m_display.refreshHz,
                m_display.width, m_display.height, m_display.hdr ? "on" : "off", m_display.fullscreen ? "fullscreen" : "windowed", Narrow(c.toneMapNote),
                m_cfg.hdrPushFps && !m_display.hdr ? " -> PushMPC (HdrPushFpsToneMap)" : ""));
        }
        if ((InputHdr() && !m_coreToneMap) || sdrToHdr) {
            // Y410 before Y416 (R-HDR1): MPC Video Renderer draws Y416 sheared, with green blocks, in some small windows
            // (3840x2160 or 1920x1080 into ~590x330 in a tall window); Y410 and P010 are clean there. Both are 4:4:4, and
            // the Y410 pass dithers its 10-bit codes
            if (!m_cfg.forceP010) { fmts.push_back({ Fmt::Y410, w, h }); if (!m_cfg.skipY416) fmts.push_back({ Fmt::Y416, w, h }); }
            fmts.push_back({ Fmt::P010H, w, h });
        }
        fmts.push_back({ Fmt::BGRA, w, h });
        return {};
    }
    HRESULT GetMediaType(int pos, CMediaType* pmt) override
    {
        if (!m_pInput->IsConnected()) return E_UNEXPECTED;
        if (pos < 0) return E_INVALIDARG;
        if (pos == 0) {
            const int64_t q0 = Qpc();
            const std::wstring why = Eligibility(m_offered);
            Log(std::format("connect: output types decided in {:.0f} ms", QpcMs(Qpc() - q0)));
            if (why != m_passWhy) { m_passWhy = why; if (!why.empty()) LogW(L"pass-through offered: " + why); }
            if (!why.empty()) m_offered.clear();
            m_offeredP010H.InitMediaType();
            for (const auto& f : m_offered) if (f.fmt == Fmt::P010H) BuildMediaType(f, &m_offeredP010H);
        }
        if (pos < int(m_offered.size())) { BuildMediaType(m_offered[pos], pmt); return S_OK; }
        if (pos == int(m_offered.size())) { *pmt = m_inMt; return S_OK; } // the pass-through type, last
        return VFW_S_NO_MORE_ITEMS;
    }
    // the processed P010 output is told from the pass-through type (both P010) by being the exact type offered
    bool IsOfferedP010H(const AM_MEDIA_TYPE* out) const
    {
        return out->subtype == MEDIASUBTYPE_P010_PM && m_offeredP010H.subtype == MEDIASUBTYPE_P010_PM && out->cbFormat == m_offeredP010H.cbFormat
            && out->pbFormat && memcmp(out->pbFormat, m_offeredP010H.pbFormat, out->cbFormat) == 0;
    }
    bool IsPassType(const AM_MEDIA_TYPE* out) const { return out->subtype == m_inMt.subtype && !IsOfferedP010H(out); }
    HRESULT CheckTransform(const CMediaType* in, const CMediaType* out) override
    {
        if (FAILED(CheckInputType(in))) return VFW_E_TYPE_NOT_ACCEPTED;
        if (out->majortype != MEDIATYPE_Video) return VFW_E_TYPE_NOT_ACCEPTED;
        BITMAPINFOHEADER bi, bo; REFERENCE_TIME a; DWORD c, il, ax, ay;
        if (!ParseVideo(in, bi, a, c, il, ax, ay) || !ParseVideo(out, bo, a, c, il, ax, ay)) return VFW_E_TYPE_NOT_ACCEPTED;
        if (IsOfferedP010H(out)) return S_OK;
        if (out->subtype == in->subtype) {
            // pass-through: same picture; the downstream allocator may ask for a wider stride
            if (labs(bo.biHeight) != labs(bi.biHeight) || bo.biWidth < bi.biWidth) return VFW_E_TYPE_NOT_ACCEPTED;
            return S_OK;
        }
        if (out->subtype != MEDIASUBTYPE_ARGB32 && out->subtype != MEDIASUBTYPE_Y410_PM && out->subtype != MEDIASUBTYPE_Y416_PM && out->subtype != MEDIASUBTYPE_P010_PM) return VFW_E_TYPE_NOT_ACCEPTED;
        if (bo.biWidth <= 0 || bo.biHeight == 0) return VFW_E_TYPE_NOT_ACCEPTED;
        return S_OK;
    }
    HRESULT SetMediaType(PIN_DIRECTION dir, const CMediaType* mt) override
    {
        BITMAPINFOHEADER bih; REFERENCE_TIME atpf; DWORD c, il, ax, ay; RECT rs;
        ParseVideo(mt, bih, atpf, c, il, ax, ay, &rs);
        if (dir == PINDIR_INPUT) {
            m_inMt = *mt;
            m_inP010 = (mt->subtype == MEDIASUBTYPE_P010_PM);
            m_inW = UINT(bih.biWidth); m_inH = UINT(labs(bih.biHeight));
            m_inStride = m_inW * (m_inP010 ? 2 : 1);
            m_visW = m_inW; m_visH = m_inH;
            if (rs.right > 0 && rs.bottom > 0) { m_visW = UINT(rs.right - rs.left); m_visH = UINT(rs.bottom - rs.top); }
            m_atpf = atpf > 0 ? atpf : 417083;
            m_inCtrl = (c & AMCONTROL_COLORINFO_PRESENT) ? c : 0;
            // AMINTERLACE_IsInterlaced only says the stream may hold interlaced frames (LAV sets it, with
            // DisplayModeWeaveOnly 0x80 or BobOrWeave 0x40, wherever the codec can code fields, e.g. MPEG-4 ASP in
            // AVI: RC1 F6). The samples' AM_VIDEO_FLAG_WEAVE / field flags decide (CheckInterlace); only one field
            // per sample is refused here.
            m_typeInterlaced = (il & AMINTERLACE_IsInterlaced) != 0;
            FastStartKick();
            m_typeIlFlags = il;
            m_inInterlaced = m_typeInterlaced && (il & AMINTERLACE_1FieldPerSample) != 0;
            m_ilDecided = false; m_ilSeen = m_ilCount = 0; m_comb.Reset();   // interlacing: measured in the picture (CheckInterlace)
            m_content.Reset(); m_contentClass = 0; m_contentConf = 0.f; m_contentLogged = -1;
            m_baseStepMs = 0; m_ptsStepMs = 0; m_tracker = pm::StepTracker{}; m_trackReset = false; m_openFps = 0; m_reopenForRate = false; m_openDeferred = false;   // a new stream: measured again
            m_inArX = ax; m_inArY = ay;
            DXVA2_ExtendedFormat ex; ex.value = m_inCtrl;
            Log(std::format("input type: {} {}x{} (visible {}x{}), atpf {}, colour {} (range {} matrix {} primaries {} transfer {}), interlace 0x{:X}", m_inP010 ? "P010" : "NV12",
                m_inW, m_inH, m_visW, m_visH, m_atpf, m_inCtrl ? "present" : "absent", (int)ex.NominalRange, (int)ex.VideoTransferMatrix, (int)ex.VideoPrimaries,
                (int)ex.VideoTransferFunction, il));
        } else {
            if (IsPassType(mt)) {
                m_outFmt = { m_inP010 ? Fmt::P010 : Fmt::NV12, UINT(bih.biWidth), UINT(labs(bih.biHeight)) };
                m_outStride = UINT(bih.biWidth) * (m_inP010 ? 2 : 1);
            } else {
                m_outFmt = { IsOfferedP010H(mt) ? Fmt::P010H : mt->subtype == MEDIASUBTYPE_Y410_PM ? Fmt::Y410 : mt->subtype == MEDIASUBTYPE_Y416_PM ? Fmt::Y416 : Fmt::BGRA, UINT(bih.biWidth), UINT(labs(bih.biHeight)) };
            }
            m_deliveredFmt = m_outFmt;
            Log(std::format("output type: {}", m_outFmt.Str()));
        }
        return S_OK;
    }

    // --- connection: the D3D11 handshake with MPC-VR (processed types), or pass-through
    HRESULT CompleteConnect(PIN_DIRECTION dir, IPin* pReceivePin) override
    {
        if (dir == PINDIR_INPUT && m_teardownTail.watch) { SetEvent(m_teardownTail.watch); CloseHandle(m_teardownTail.watch); m_teardownTail.watch = nullptr; }
        if (dir != PINDIR_OUTPUT) return S_OK;
        const CMediaType& mt = m_pOutput->CurrentMediaType();
        if (IsPassType(&mt)) {
            m_mode = Mode::PassSys;
            const std::wstring why = m_passWhy.empty() ? m_lastRefusal : m_passWhy;
            SetStatus(L"pass-through (source format, no PushFPS): " + (why.empty() ? std::wstring(L"the downstream filter took the source format") : why));
            return S_OK;
        }
        ComPtr<ID3D11DecoderConfiguration> cfg;
        HRESULT hr = pReceivePin->QueryInterface(__uuidof(ID3D11DecoderConfiguration), (void**)&cfg);
        if (FAILED(hr)) {
            m_lastRefusal = DownstreamName(pReceivePin) + L" is not MPC Video Renderer in D3D11 mode (no ID3D11DecoderConfiguration)";
            LogW(L"processed type refused: " + m_lastRefusal + L"; the source format is tried next");
            return VFW_E_TYPE_NOT_ACCEPTED;
        }
        const UINT idx = cfg->GetD3D11AdapterIndex();
        if (!m_device) {
            hr = CreateDevice(idx);
            if (FAILED(hr)) { m_lastRefusal = L"no D3D11 device on MPC Video Renderer's adapter"; return VFW_E_TYPE_NOT_ACCEPTED; }
        }
        if (m_outFmt.fmt == Fmt::Y416 && !m_y416Uav) { Log("Y416 refused: no typed UAV for Y416 on this GPU; Y410 next"); return VFW_E_TYPE_NOT_ACCEPTED; }
        hr = cfg->ActivateD3D11Decoding(m_device.Get(), m_ctx.Get(), nullptr, 0);
        Log(std::format("ActivateD3D11Decoding(our device, adapter index from MPC-VR {}) -> 0x{:08X}", idx, uint32_t(hr)));
        if (FAILED(hr)) { m_lastRefusal = L"MPC Video Renderer refused ActivateD3D11Decoding"; return VFW_E_TYPE_NOT_ACCEPTED; }
        m_mode = Mode::Warmup;
        { PIN_INFO pi = {}; m_down.Reset(); if (SUCCEEDED(pReceivePin->QueryPinInfo(&pi)) && pi.pFilter) m_down.Attach(pi.pFilter); } // its state: see DelivererLoop
        m_lastRefusal.clear();
        if (!InputHdr()) StartOpen(nullptr); // HDR waits for the first sample's static metadata
        SetStatus(L"opening pushcore (source frames shown meanwhile)");
        return S_OK;
    }
    static std::wstring DownstreamName(IPin* p)
    {
        PIN_INFO pi = {};
        std::wstring n = L"the downstream filter";
        if (p && SUCCEEDED(p->QueryPinInfo(&pi)) && pi.pFilter) {
            FILTER_INFO fi = {};
            if (SUCCEEDED(pi.pFilter->QueryFilterInfo(&fi))) { n = fi.achName; if (fi.pGraph) fi.pGraph->Release(); }
            CLSID cls = {};
            if (SUCCEEDED(pi.pFilter->GetClassID(&cls))) { wchar_t g[64]; StringFromGUID2(cls, g, 64); n += std::wstring(L" ") + g; }
            pi.pFilter->Release();
        }
        return n;
    }
    HRESULT BreakConnect(PIN_DIRECTION dir) override
    {
        if (dir == PINDIR_INPUT) {
            if (!m_teardownTail.watch && m_openStarted) m_teardownTail.watch = StartTeardownWatch();   // RC2 F2: a stuck close explains itself
            StopThreads(); StopWatcher(); StopAuto(); CloseCore(); m_openStarted = false; m_openDeferred = false; m_mode = Mode::None; m_bitrateProbed = false;
        }
        if (dir == PINDIR_OUTPUT) { Out()->ours = nullptr; m_down.Reset(); }
        return S_OK;
    }
    HRESULT DecideBufferSize(IMemAllocator* pAlloc, ALLOCATOR_PROPERTIES* req) override
    {
        auto* pin = Out();
        ALLOCATOR_PROPERTIES p = {}, act = {};
        if (m_mode == Mode::PassSys || IsPassType(&m_pOutput->CurrentMediaType())) {
            p.cBuffers = std::max<long>(3, req->cBuffers);
            p.cbBuffer = std::max<long>(long(m_pOutput->CurrentMediaType().GetSampleSize()), long(m_outStride * m_outFmt.h * 3 / 2));
            p.cbAlign = std::max<long>(1, req->cbAlign);
            HRESULT hr = pAlloc->SetProperties(&p, &act);
            Log(std::format("DecideBufferSize (pass-through): {} x {} bytes -> 0x{:08X}", act.cBuffers, act.cbBuffer, uint32_t(hr)));
            return FAILED(hr) ? hr : (act.cbBuffer < p.cbBuffer ? E_FAIL : S_OK);
        }
        if (!pin->ours || static_cast<IMemAllocator*>(pin->ours) != pAlloc) {
            Log("DecideBufferSize: downstream offered its own (system memory) allocator: refused");
            return E_FAIL;
        }
        pin->ours->device = m_device;
        pin->ours->fmt = m_outFmt;
        p.cBuffers = kRing; p.cbBuffer = 1; p.cbAlign = 1;
        HRESULT hr = pAlloc->SetProperties(&p, &act);
        Log(std::format("DecideBufferSize: {} D3D11 samples -> 0x{:08X}", act.cBuffers, uint32_t(hr)));
        return hr;
    }

    // --- quality messages from MPC-VR (every sample it schedules): kept as a lateness signal; in processed modes the
    //     filter handles quality itself (it drops late generated frames), so MPC-VR is told not to drop frames itself
    HRESULT AlterQuality(Quality q) override
    {
        if (m_mode == Mode::PassSys) return S_FALSE; // LAV's business
        m_qLateSum += q.Late; m_qN++;
        return S_OK;
    }

    // --- streaming state
    HRESULT StartStreaming() override
    {
        m_quit = false; m_stopReq = false; m_needPreroll = true; m_discNext = true; m_coreDisc = true; m_eos = false;
        StartThreads();
        return S_OK;
    }
    HRESULT StopStreaming() override
    {
        const int64_t q0 = Qpc();
        StopThreads();
        DrainCore(true);
        Log(std::format("StopStreaming: threads joined, pushcore drained in {:.1f} ms", QpcMs(Qpc() - q0)));
        return S_OK;
    }
    STDMETHODIMP Stop() override
    {
        const int64_t q0 = Qpc();
        Log("Stop requested");
        m_stopReq = true; m_stepMode = false; WakeAll();
        HRESULT hr = CTransformFilter::Stop();
        Log(std::format("Stop done in {:.1f} ms -> 0x{:08X}", QpcMs(Qpc() - q0), uint32_t(hr)));
        return hr;
    }
    STDMETHODIMP Pause() override
    {
        const FILTER_STATE was = m_State;
        if (was == State_Running) { m_stepMode = true; m_stepSkips = 0; m_pauseFromRunQpc = Qpc(); }
        if (was == State_Running) { std::unique_lock<std::mutex> lk(m_coreLock, std::try_to_lock); if (lk.owns_lock()) CloseRetired("the player paused"); }
        HRESULT hr = CTransformFilter::Pause();
        m_stopReq = false;
        WakeAll();
        Log(std::format("Pause (from {})", was == State_Stopped ? "stopped" : was == State_Running ? "running" : "paused"));
        if (was == State_Stopped && !m_graphLogged) { m_graphLogged = true; LogGraph(); }
        return hr;
    }
    // the playback graph once per connection (filters, their CLSIDs and pin connections): what really sits upstream
    bool m_graphLogged = false;
    int64_t m_segQpc = 0; std::atomic<int> m_segSamples = 0;
    // the filters upstream of our input, nearest first (LAV Video, then the splitter, ...)
    std::wstring UpstreamChain()
    {
        std::wstring s; IPin* p = m_pInput ? m_pInput->GetConnected() : nullptr; ComPtr<IPin> hold;
        for (int i = 0; p && i < 6; ++i) {
            PIN_INFO pi = {}; if (FAILED(p->QueryPinInfo(&pi)) || !pi.pFilter) break;
            ComPtr<IBaseFilter> f; f.Attach(pi.pFilter);
            FILTER_INFO fi = {}; f->QueryFilterInfo(&fi); if (fi.pGraph) fi.pGraph->Release();
            s += (s.empty() ? L"" : L" <- ") + std::wstring(fi.achName);
            ComPtr<IEnumPins> e; ComPtr<IPin> in, up; p = nullptr;
            if (FAILED(f->EnumPins(&e))) break;
            while (e->Next(1, &in, nullptr) == S_OK) { PIN_DIRECTION d; in->QueryDirection(&d); if (d == PINDIR_INPUT && SUCCEEDED(in->ConnectedTo(&up)) && up) break; in.Reset(); }
            if (!up) break;
            hold = up; p = hold.Get();
        }
        return s.empty() ? std::wstring(L"(not connected)") : s;
    }
    void LogGraph()
    {
        if (!m_pGraph) return;
        ComPtr<IEnumFilters> ef;
        if (FAILED(m_pGraph->EnumFilters(&ef))) return;
        ComPtr<IBaseFilter> f;
        while (ef->Next(1, &f, nullptr) == S_OK) {
            FILTER_INFO fi = {}; f->QueryFilterInfo(&fi); if (fi.pGraph) fi.pGraph->Release();
            CLSID cls = {}; f->GetClassID(&cls); wchar_t g[64] = {}; StringFromGUID2(cls, g, 64);
            std::wstring line = std::wstring(L"graph: ") + fi.achName + L" " + g;
            ComPtr<IEnumPins> ep;
            if (SUCCEEDED(f->EnumPins(&ep))) {
                ComPtr<IPin> p;
                while (ep->Next(1, &p, nullptr) == S_OK) {
                    PIN_DIRECTION d = PINDIR_INPUT; p->QueryDirection(&d);
                    ComPtr<IPin> peer; p->ConnectedTo(&peer);
                    PIN_INFO pi = {}; FILTER_INFO pf = {};
                    if (peer && SUCCEEDED(peer->QueryPinInfo(&pi)) && pi.pFilter) { pi.pFilter->QueryFilterInfo(&pf); if (pf.pGraph) pf.pGraph->Release(); pi.pFilter->Release(); }
                    line += std::wstring(d == PINDIR_INPUT ? L" | in<-" : L" | out->") + (peer ? pf.achName : L"(none)");
                    p.Reset();
                }
            }
            LogW(line);
            f.Reset();
        }
    }
    STDMETHODIMP Run(REFERENCE_TIME t) override
    {
        HRESULT hr = CTransformFilter::Run(t);
        m_resumedQpc = Qpc();
        m_admGrace = std::max(m_admGrace.load(), 2);
        WakeAll();
        Log(std::format("Run (tStart {})", t));
        return hr;
    }
    HRESULT NewSegment(REFERENCE_TIME start, REFERENCE_TIME stop, double rate) override
    {
        Log(std::format("NewSegment start {:.3f} s rate {}", start / 1e7, rate));
        m_segQpc = Qpc(); m_segSamples = 0;
        // the playback rate (F9): the splitter's timestamps are divided by it, so the source interval is the measured
        // interval at 1.0x divided by the rate, set at once (one Auto decision per change, no estimate converging)
        const double r = rate > 0 ? rate : 1.0, was = m_rate.load();
        m_rate = r;
        m_trackReset = true; m_contentDisc = true;
        if (std::fabs(r - was) > 1e-6) {
            const double base = m_baseStepMs.load();
            if (base > 0) m_ptsStepMs = base / r;
            m_rateQpc = Qpc();         // R-RATE1: the GPU timings of the next 3 s are not learned from
            m_rateSerial++;            // the Auto thread wakes on it (round 2, F9)
            m_admRateChanged = true;   // FG admission re-decides at once against the new interval (round 2, F9)
            Log(std::format("playback rate {} -> {}: source interval {:.3f} ms ({:.3f} fps){}", was, r, base > 0 ? base / r : double(m_atpf) / 1e4 / r,
                base > 0 ? 1000.0 / (base / r) : 1e7 / double(m_atpf) * r, base > 0 ? "" : " (from the media type until the timestamps are measured)"));
        }
        ClearSeekStep("NewSegment");
        return CTransformFilter::NewSegment(start, stop, rate);
    }
    // A seek is Pause (from running) -> flush -> NewSegment -> Run within a few ms; a frame step has no flush. Step mode
    // from that Pause is dropped at the flush, so generated frames flow again right after the seek (RC1 F2: ~330 ms of
    // real frames only after every seek). A pause by the user that is followed by a seek much later keeps step mode,
    // so frame steps after it still show real frames.
    void ClearSeekStep(const char* why)
    {
        if (!m_stepMode || !m_pauseFromRunQpc || QpcMs(Qpc() - m_pauseFromRunQpc) > 1000.0) return;
        m_stepMode = false;
        Log(std::format("{} {:.0f} ms after the pause: a seek, not a frame step: step mode off ({} generated frames skipped)", why, QpcMs(Qpc() - m_pauseFromRunQpc), m_stepSkips));
    }
    HRESULT BeginFlush() override
    {
        m_flushing = true; m_flushGen++;
        m_flushStartQpc = Qpc();
        ClearSeekStep("BeginFlush");
        m_trackReset = true; m_contentDisc = true;
        WakeAll();
        Log("BeginFlush");
        return CTransformFilter::BeginFlush(); // downstream first wakes a Deliver waiting inside MPC-VR
    }
    HRESULT EndFlush() override
    {
        {
            CAutoLock lck(&m_csReceive);
            // the copier returns every lease it holds when it sees the flush; the deliverer is out of Deliver
            const uint32_t gen = m_flushGen;
            for (int i = 0; i < 1000 && (m_copierAck.load() != gen || m_inDeliver) && m_threadsRunning; i++) Sleep(1);
            ClearReady();
            DrainCore(true);
            m_coreDisc = true; m_eos = false; m_needPreroll = true; m_discNext = true; m_firstAfterFlush = true;
            m_lastSubmitPts = INT64_MIN;
            m_admFlushed = true;
            { std::lock_guard<std::mutex> sl(m_sideLock); m_sideByPts.clear(); }
        }
        HRESULT hr = CTransformFilter::EndFlush(); // downstream EndFlush before anything new is delivered
        m_flushing = false;
        WakeAll();
        Log("EndFlush");
        return hr;
    }
    HRESULT EndOfStream() override
    {
        Log("EndOfStream from upstream");
        if (m_segQpc && m_segSamples.load() == 0) {
            // the decoder ended the video without one frame since the segment started: the audio plays on over a black
            // picture. Not ours to fix (no frame came in), but it must not be silent: say it in the log and the status.
            Log(std::format("WARNING: the video stream ended {:.0f} ms after NewSegment with NO video frame from upstream (audio may play on over a black picture); the upstream chain: {}",
                QpcMs(Qpc() - m_segQpc), Narrow(UpstreamChain())));
            SetStatus(L"the video decoder sent no frames (end of stream right after the start); see the log");
        }
        if (m_mode == Mode::PassSys) return CTransformFilter::EndOfStream();
        if (m_mode == Mode::Active) { m_eos = true; SetEvent(m_hSubmit); return S_OK; }
        PushReady(ReadyItem{ nullptr, 0, 0, 3, 0, m_flushGen.load(), Qpc() });
        return S_OK;
    }

    HRESULT Receive(IMediaSample* pIn) override;
    GUID InputSubtype() const { return m_inMt.subtype; }

private:
    static constexpr int kRing = 8;
    enum class Mode { None, PassSys, Warmup, Active, PassD3D };

    // --- device and helpers
    // any hardware adapter (PushMPC 0.6: NVIDIA, AMD and Intel; what each can run comes from pushcore_capabilities)
    static bool HasHardwareAdapter()
    {
        ComPtr<IDXGIFactory1> f;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return false;
        ComPtr<IDXGIAdapter1> a;
        for (UINT i = 0; SUCCEEDED(f->EnumAdapters1(i, &a)); i++) {
            DXGI_ADAPTER_DESC1 d = {};
            if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.VendorId != 0x1414) return true;
            a.Reset();
        }
        return false;
    }
    // the GPU's capabilities (once per adapter): luid 0 = before our device exists (the first hardware adapter)
    void EnsureGpu(uint64_t luid)
    {
        if (m_gpuKnown && (luid == 0 || luid == m_gpuLuid)) return;
        if (m_rt.root.empty() && m_rt.error.empty()) m_rt = ResolveRuntime(m_cfg);
        std::wstring log;
        pm::Gpu g = QueryGpu(m_rt, luid, &log);
        { std::lock_guard<std::mutex> lk(m_autoLock); m_gpu = g; m_costs.card = pm::CardFactor(g.name, g.family); }
        m_gpuKnown = g.family != pm::kFamUnknown; m_gpuLuid = luid;
        LogW(log);
        { std::lock_guard<std::mutex> lk(g_statusLock); g_gpu = g; g_gpuKnown = m_gpuKnown; }
    }
    // The display's own reported peak and full-frame luminance (DXGI_OUTPUT_DESC1 MaxLuminance / MaxFullFrameLuminance of
    // the output showing MPC-HC's window, from the EDID / DisplayID HDR metadata Windows reads); false = not reported
    bool ItmDisplay(float& peak, float& fullFrame)
    {
        peak = 0.0f; fullFrame = 0.0f;
        ComPtr<IDXGIFactory1> f;
        HMONITOR mon = MonitorFromWindow(pm::PlayerWindow(), MONITOR_DEFAULTTONEAREST);
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) {
            ComPtr<IDXGIAdapter1> a;
            for (UINT i = 0; SUCCEEDED(f->EnumAdapters1(i, &a)); i++, a.Reset()) {
                ComPtr<IDXGIOutput> o;
                for (UINT j = 0; SUCCEEDED(a->EnumOutputs(j, &o)); j++, o.Reset()) {
                    ComPtr<IDXGIOutput6> o6; DXGI_OUTPUT_DESC1 d = {};
                    if (SUCCEEDED(o.As(&o6)) && SUCCEEDED(o6->GetDesc1(&d)) && d.Monitor == mon && d.MaxLuminance >= 100.0f && d.MaxLuminance <= 10000.0f) {
                        peak = d.MaxLuminance;
                        fullFrame = d.MaxFullFrameLuminance >= 50.0f && d.MaxFullFrameLuminance <= d.MaxLuminance ? d.MaxFullFrameLuminance : 0.0f;
                    }
                }
            }
        }
        dxgiPeak_ = peak; dxgiFull_ = fullFrame;
        // the user's display peak (a QD-OLED "True Black 400" mode reports what its 1000-nit mode reports)
        if (m_cfg.user.displayPeak > 0) { peak = float(m_cfg.user.displayPeak); if (fullFrame > peak) fullFrame = peak; return true; }
        return peak > 0.0f;
    }
    float dxgiPeak_ = 0.0f, dxgiFull_ = 0.0f;
    int64_t m_peakCheckQpc = 0;
    // MPC Video Renderer's HDR settings for an HDR display (user-approved, ManageMpcVr = 1, the default): its local tone
    // mapping on, type Spline (7), HdrDisplayNits = the display peak (the user's Display peak, else DXGI); one tone mapping,
    // only above the display's peak. The values found first are kept in HKCU\Software\PushMPC\MpcVrBackup (the
    // installer's uninstall restores them). MPC-VR reads them when it starts: they apply from the next playback.
    void ConfigureMpcVr()
    {
        HKEY pk = nullptr; DWORD manage = 1, sz = sizeof(manage), type = 0;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, pm::kSettingsKey, 0, KEY_READ, &pk) == ERROR_SUCCESS) {
            if (RegQueryValueExW(pk, L"ManageMpcVr", nullptr, &type, (BYTE*)&manage, &sz) != ERROR_SUCCESS || type != REG_DWORD) manage = 1;
            RegCloseKey(pk);
        }
        if (!manage) return;
        float peak = 0, full = 0;
        if (!ItmDisplay(peak, full)) return;   // no peak known: leave MPC-VR's nits alone
        const DWORD nits = DWORD(std::clamp(std::lround(peak), 100L, 10000L));
        HKEY vr = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\MPC-BE Filters\\MPC Video Renderer", 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &vr, nullptr) != ERROR_SUCCESS) return;
        HKEY bk = nullptr; DWORD disp = 0;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\PushMPC\\MpcVrBackup", 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &bk, &disp) == ERROR_SUCCESS) {
            if (disp == REG_CREATED_NEW_KEY) {   // first time: keep what MPC-VR had ("absent" = the value did not exist)
                for (const wchar_t* n : { L"HdrLocalToneMapping", L"HdrLocalToneMappingType", L"HdrDisplayNits" }) {
                    DWORD v = 0, s = sizeof(v), ty = 0;
                    if (RegQueryValueExW(vr, n, nullptr, &ty, (BYTE*)&v, &s) == ERROR_SUCCESS && ty == REG_DWORD) RegSetValueExW(bk, n, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
                    else { const wchar_t a[] = L"absent"; RegSetValueExW(bk, n, 0, REG_SZ, (const BYTE*)a, sizeof(a)); }
                }
            }
            RegCloseKey(bk);
        }
        std::string changed;
        auto set = [&](const wchar_t* n, DWORD want) {
            DWORD v = 0, s = sizeof(v), ty = 0;
            if (RegQueryValueExW(vr, n, nullptr, &ty, (BYTE*)&v, &s) == ERROR_SUCCESS && ty == REG_DWORD && v == want) return;
            RegSetValueExW(vr, n, 0, REG_DWORD, (const BYTE*)&want, sizeof(want));
            changed += std::format("{}{}={}", changed.empty() ? "" : " ", Narrow(n), want);
        };
        set(L"HdrLocalToneMapping", 1); set(L"HdrLocalToneMappingType", 7); set(L"HdrDisplayNits", nits);
        RegCloseKey(vr);
        if (!changed.empty()) Log("MPC Video Renderer HDR settings written: " + changed);
        ApplyMpcVrLive(nits);
    }
    // PushMPC's SDR to HDR curve for this choice: the peak from the display (clamped to
    // 300-1500; not reported: 600), paper white min(203, 0.45 x peak), M = High min(5, peak / paper white), Medium 3.2,
    // Low 2.0 (never above peak / paper white), Off 1; the limiter's frame-average target 0.6 x the full-frame luminance
    // (not reported: 0.5 x peak). Logged when it changes; the HDR10 static metadata of the output follows it.
    // on: SDR shown as HDR10 (PushMPC's curve, or RTX Video HDR in pushcore: then only the limiter is ours)
    void SetItm(const pm::Choice& c, bool active)
    {
        Itm n; n.boost = c.hdrBoost; n.limit = c.hdrLimit;
        n.on = active && c.hdrItm; n.rtx = active && !c.hdrItm && c.hdrLimit;
        const bool on = n.on;
        if (n.on || n.rtx) {
            float peak = 0, full = 0;
            n.reported = ItmDisplay(peak, full);
            n.peak = n.reported ? std::clamp(peak, 300.0f, 1500.0f) : 600.0f;
            n.fullFrame = full;
            n.gamma = m_cfg.user.sdrGamma24 ? 2.4f : 2.2f;
            n.pw = std::min(203.0f, 0.45f * n.peak);
            const float room = n.peak / n.pw;
            const float m = c.hdrBoost == pm::HdrBoost::High ? std::min(5.0f, room) : c.hdrBoost == pm::HdrBoost::Medium ? std::min(3.2f, room)
                          : c.hdrBoost == pm::HdrBoost::Low ? std::min(2.0f, room) : 1.0f;
            n.kUser = std::max(0.0f, m - 1.0f);
            n.target = full > 0.0f ? 0.6f * full : 0.5f * n.peak;
            if (n.rtx) { n.pw = 203.0f; n.kUser = 1.0f; }   // RTX Video HDR's mid-grey 44 = paper white ~200 cd/m2
        }
        bool changed;
        {
            std::lock_guard<std::mutex> lk(m_itmLock);
            changed = !(n == m_itm);
            m_itm = n;
            if (changed) {
                m_itmSide.m.clear();
                if (on) {
                    const float cll = std::min(n.peak, n.pw * (1.0f + n.kUser));
                    // the usual convention for SDR shown as HDR10: P3-D65 mastering, BT.709 content
                    SetHdr10Side(m_itmSide, 0, std::round(cll), 0.0001, unsigned(std::lround(cll)), n.limit ? unsigned(std::lround(std::min(n.target, cll))) : 0u);
                }
            }
        }
        m_itmOn = on; m_rtxLim = n.rtx;
        if (changed && n.rtx)
            Log(std::format("SDR to HDR: RTX Video HDR, PushMPC's bright-scene limiter on its output (light above {:.0f} cd/m2 scaled down; frame average {:.0f} cd/m2)", n.pw, n.target));
        if (changed && on)
            Log(std::format("SDR to HDR: PushMPC's curve, boost {} (M {:.2f}), paper white {:.0f} cd/m2, peak {:.0f} cd/m2 ({}{}), decode power {:.1f}, "
                            "limiter {} frame average {:.0f} cd/m2 ({}); HDR10 metadata MaxCLL {:.0f}{}", Narrow(pm::HdrBoostStr(n.boost)), 1.0f + n.kUser, n.pw, n.peak,
                            m_cfg.user.displayPeak > 0 ? "the user's display peak" : n.reported ? "the display's reported peak" : "not reported: 600",
                            std::format("; DXGI MaxLuminance {:.0f}, MaxFullFrameLuminance {:.0f}", dxgiPeak_, dxgiFull_), n.gamma, n.limit ? "on," : "OFF (would be", n.target, n.fullFrame > 0 ? "0.6 x the reported full-frame luminance" : "0.5 x peak",
                            std::min(n.peak, n.pw * (1.0f + n.kUser)), m_cfg.itmBypass ? "; ItmBypass: the curve is skipped (timing A/B)" : ""));
    }
    HRESULT CreateDevice(UINT preferredIndex);
    HRESULT EnsureUpload();
    HRESULT Upload(IMediaSample* pIn);
    CD3D11Sample* GetFree(const OutFmt& f, bool streaming);
    void Draw(ID3D11DeviceContext* def, CD3D11Sample* s, ID3D11PixelShader* ps, ID3D11ShaderResourceView* a, ID3D11ShaderResourceView* b);
    void DrawSource(CD3D11Sample* s);
    void SetYuvConstants(ID3D11DeviceContext* def, bool hdrToSdr);
    void SetComputeConstants(ID3D11DeviceContext* def, CD3D11Sample* s, bool sdrIn, bool srcSdr);
    // --- pushcore
    void StartOpen(IMediaSample* first);
    bool ShouldDeferOpen();
    bool m_openDeferred = false;    // F8 (round 2): the open waits for the measured frame rate
    int64_t m_deferQpc = 0;
    void DrainCore(bool flush);
    void CloseCore();
    bool TryActivate(REFERENCE_TIME pts);
    void Fallback(const std::wstring& why);
    void RequestOutFmt(const OutFmt& want, const char* why);
    bool DeviceGone();
    void DeviceLost(int where, const std::string& what);
    bool RequestReopen();
    // --- threads
    void StartThreads();
    void StopThreads();
    void WakeAll() { if (m_hWake) SetEvent(m_hWake); if (m_hLease) SetEvent(m_hLease); if (m_hSubmit) SetEvent(m_hSubmit); m_readyCv.notify_all(); }
    void PushReady(const ReadyItem& it) { std::lock_guard<std::mutex> lk(m_readyLock); m_ready.push_back(it); m_readyCv.notify_all(); }
    void ClearReady()
    {
        std::deque<ReadyItem> old;
        { std::lock_guard<std::mutex> lk(m_readyLock); old.swap(m_ready); }
        for (auto& it : old) if (it.sample) it.sample->Release();
    }
    size_t ReadySize() { std::lock_guard<std::mutex> lk(m_readyLock); return m_ready.size(); }
    ID3D11ShaderResourceView* SrvFor(ID3D11Texture2D* t);
    void CopierLoop();
    void DelivererLoop();
    bool WaitUntil(REFERENCE_TIME due, uint32_t gen);
    bool StreamNow(REFERENCE_TIME& t)
    {
        if (m_State != State_Running || !m_pClock) return false;
        REFERENCE_TIME c = 0;
        if (FAILED(m_pClock->GetTime(&c))) return false;
        t = c - m_tStart;
        return true;
    }
    void AdmissionTick();
    void DoReload(bool forced);
    // --- Auto (PushMpcAuto.h): decided at connect/open, re-evaluated once a second on its own thread
    bool LegacyMode() const { return !m_rt.dataDir.empty(); }
    pm::Source SourceInfo();
    void ProbeBitrate();
    pm::Choice DecideNow(const pm::Settings& s, const pm::Display& d);
    PushCoreSettings ToCore(const pm::Settings& s, const pm::Choice& c);
    void PublishNotes(const pm::Settings& s, const pm::Choice& c);
    void LogChoice(const char* why, const pm::Choice& c);
    void StartAuto();
    void StopAuto();
    void ShowChoiceOnce();
    void AutoLoop();
    void LearnCosts();
    void LoadLearned();
    void MaybeBenchPushFlow(UINT w, UINT h);
    void SaveLearned();
    void StartWatcher();
    void StopWatcher();
    void WatcherLoop();
    int LeadUs();
    HRESULT PassSysmem(IMediaSample* pIn);

    Config m_cfg;
    Runtime m_rt;
    std::atomic<Mode> m_mode = Mode::None;
    std::wstring m_passWhy, m_lastRefusal;
    std::vector<OutFmt> m_offered;   // processed formats offered at connect time, in order
    ComPtr<IBaseFilter> m_down;      // the renderer (processed mode): its state is checked before each Deliver
    CMediaType m_offeredP010H;       // the exact P010 HDR10 type offered (told from the P010 pass-through type)
    bool m_loggedNoColour = false;
    // device
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_ctx;
    ComPtr<ID3D11DeviceContext4> m_ctx4;
    ComPtr<ID3D11DeviceContext> m_defRecv;
    ComPtr<ID3D11VertexShader> m_vs;
    ComPtr<ID3D11PixelShader> m_psRgb, m_psYuv;
    ComPtr<ID3D11ComputeShader> m_csRgb2Y410, m_csP0102Y410, m_csRgb2Y416, m_csP0102Y416;
    bool m_y416Uav = false; // typed UAV support of Y416 on our device (else Y416 is refused at connection)
    ComPtr<ID3D11Buffer> m_cbSize;
    void Dispatch(ID3D11DeviceContext* def, CD3D11Sample* s, ID3D11ComputeShader* cs, ID3D11ShaderResourceView* a, ID3D11ShaderResourceView* b, bool sdrIn, bool srcSdr);
    // P010 HDR output: written through plane UAVs into a scratch texture, then copied into the sample's decoder-bound texture
    struct P010Scratch { ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11UnorderedAccessView> y, uv; UINT w = 0, h = 0; };
    P010Scratch m_scrCopier, m_scrRecv;   // one per drawing thread (copier, streaming thread)
    ComPtr<ID3D11ComputeShader> m_csRgb2P010Y, m_csRgb2P010UV, m_csP0102P010Y, m_csP0102P010UV;
    ComPtr<ID3D11Buffer> m_cbOsd;   // the OSD (b5) and its glyph atlas (t2)
    ComPtr<ID3D11ShaderResourceView> m_osdSrv;
    float m_osdGlyph[97][2] = {};   // atlas x, width (pixels)
    float m_osdAtlasW = 1.0f, m_osdCellH = 1.0f;
    std::mutex m_osdLock; std::wstring m_osdText;   // what the OSD says (the status thread composes it)
    std::atomic<double> m_osdUntilMs = 0.0;          // Brief: shown until this QpcMs
    std::atomic<bool> m_updateTried = false;         // the automatic update check ran in this playback
    void BuildOsdAtlas(ID3D11Device* dev);
    void FillOsd(ID3D11DeviceContext* def, UINT w, UINT h, bool draw);
    void OsdBrief() { m_osdUntilMs = QpcMs(Qpc()) + 3000.0; }
    void ApplyMpcVrLive(DWORD nits);
    ComPtr<ID3D11Buffer> m_cbSrc, m_cbDv, m_cbDvx;   // m_cbDvx: the exact Dolby Vision P5 reshaping (b4)
    void SetDvConstants(ID3D11DeviceContext* def);
    HRESULT EnsureScratch(P010Scratch& s, UINT w, UINT h);
    void DispatchP010(ID3D11DeviceContext* def, CD3D11Sample* s, P010Scratch& scr, bool fromRgb, ID3D11ShaderResourceView* a, ID3D11ShaderResourceView* b, bool sdrIn, bool srcSdr);
    ComPtr<ID3D11SamplerState> m_sampler;
    ComPtr<ID3D11Buffer> m_cb;
    ComPtr<ID3D11Texture2D> m_upload;
    ComPtr<ID3D11ShaderResourceView> m_uploadY, m_uploadUV;
    std::mutex m_srvLock;
    std::map<ID3D11Texture2D*, ComPtr<ID3D11ShaderResourceView>> m_srvs;
    // formats
    CMediaType m_inMt;
    bool m_inP010 = false, m_inInterlaced = false;
    bool m_coreToneMap = false;   // HDR source on an SDR display: PushMPC tone-maps (BGRA connection), decided at connect time
    UINT m_inW = 0, m_inH = 0, m_inStride = 0, m_visW = 0, m_visH = 0, m_outStride = 0;
    DWORD m_inCtrl = 0, m_inArX = 0, m_inArY = 0;
    REFERENCE_TIME m_atpf = 417083, m_lastInPts = 0;
    std::mutex m_fmtLock;
    OutFmt m_outFmt;          // what new output samples are drawn as (the type MPC-VR is switched to on the next one)
    OutFmt m_deliveredFmt;    // the type of the last delivered sample (the connection's type before the first)
    std::atomic<double> m_rate = 1.0;
    // the source frame interval (F8/F9): measured from the timestamps (median of the first steps, at 1.0x), divided
    // by the playback rate; m_ptsStepMs = m_baseStepMs / m_rate is what delivery and Auto use
    std::atomic<double> m_baseStepMs = 0;
    std::atomic<bool> m_trackReset = false;
    std::atomic<uint32_t> m_rateSerial = 0;
    std::atomic<int64_t> m_rateQpc = 0;   // when the playback rate last changed
    // R-RATE1: the graph each playback rate ran last (Auto's own choice, held), with what it was decided for: a rate change
    // back to a rate seen before takes that graph again instead of a decision on costs learned at the other rate
    struct RateGraph { PushCoreSettings graph; pm::Choice choice; pm::Settings settings; pm::Display display; };
    std::map<int, RateGraph> m_rateGraphs;   // key: the rate in hundredths; Auto thread only
    pm::StepTracker m_tracker;        // streaming thread only
    double m_openFps = 0;             // the source rate the open's Auto decision used
    bool m_reopenForRate = false;     // the running open was for another frame rate: reopen when it is done
    void TrackPts(REFERENCE_TIME pts, bool disc);
    void OnSourceRateMeasured();
    // interlacing (F6): the type's flag only says the stream MAY be interlaced; the samples' flags decide
    bool m_typeInterlaced = false, m_ilDecided = false;
    DWORD m_typeIlFlags = 0;
    uint32_t m_ilSeen = 0, m_ilCount = 0;
    void CheckInterlace(IMediaSample* pIn);
    std::wstring SourceFile();
    int64_t m_pauseFromRunQpc = 0;
    SideData m_hdrSide;       // HDR10 static metadata of the source (the default for processed samples)
    std::mutex m_sideLock;
    std::map<REFERENCE_TIME, SideData> m_sideByPts; // per submitted source frame: all its side data (see CopierLoop)
    bool m_sideChecked = false;
    // pushcore
    std::mutex m_coreLock;
    const PushCoreApi* m_api = nullptr;
    PushCore* m_core = nullptr;
    PushCoreOutputInfo m_oi = {};
    // pushcore_open runs on its own thread (3-16 s); a close while it runs abandons it instead of waiting: the thread
    // holds a reference on this DLL and closes the instance it opened when it finishes
    struct OpenJob {
        std::mutex lock;
        bool abandoned = false, done = false;
        const PushCoreApi* api = nullptr;
        PushCore* core = nullptr;
        std::string err;
        double ms = 0;
        // fast start: core is the quick-start graph and the full graph follows on the same thread (next*)
        bool quick = false, nextDone = false;
        PushCore* next = nullptr;
        std::string nextErr;
        double nextMs = 0;
    };
    std::shared_ptr<OpenJob> m_job;
    // fast start, quick-start handover: the quick graph (oldCore) delivers up to and including the real frame at handoverPts,
    // the full graph (m_core) got that same source frame as its first one, and its outputs up to it are dropped
    PushCore* m_oldCore = nullptr;
    REFERENCE_TIME m_handoverPts = 0, m_discardUpTo = INT64_MIN;
    std::atomic<bool> m_quickPhase = false;   // the quick graph runs (Auto and reloads wait until the full graph took over)
    int64_t m_quickQpc = 0;
    bool m_sizeAfterHandover = false;          // the full graph's output size differs (Receive asks downstream for it)
    // no-output guard: the first submit that no output followed yet (0 = none pending). Should pushcore take frames and give
    // none back for 2 s (a graph that stopped producing, a handover gone wrong), the source frames are shown instead, never
    // a black or frozen picture.
    std::atomic<int64_t> m_waitOutQpc = 0;
    PushCore* m_retired = nullptr;   // the quick graph after the handover, idle until CloseRetired (under m_coreLock)
    void CloseRetired(const char* why)
    {
        if (!m_retired) return;
        CloseAsync(m_api, m_retired, "the quick graph");
        m_retired = nullptr;
        Log(std::format("quick start: the idle quick graph is closed now ({})", why));
    }
    bool VideoMemoryShort()
    {
        ComPtr<IDXGIDevice> dd; ComPtr<IDXGIAdapter> a; ComPtr<IDXGIAdapter3> a3; DXGI_QUERY_VIDEO_MEMORY_INFO mi = {};
        if (!m_device || FAILED(m_device.As(&dd)) || FAILED(dd->GetAdapter(&a)) || FAILED(a.As(&a3)) || FAILED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mi))) return false;
        return mi.CurrentUsage + (1024ull << 20) > mi.Budget;
    }
    bool QuickStartWanted(const PushCoreSettings& ps, std::string& why);
    void TryHandover(REFERENCE_TIME pts, const PushCoreSubmitTiming& t);   // under m_coreLock
    void RetireOld(const char* why);                                       // under m_coreLock
    static void CloseAsync(const PushCoreApi* api, PushCore* core, const char* what, DWORD delayMs = 0)
    {
        if (!api || !core) return;
        fs::Detach([api, core, what, delayMs] { if (delayMs) Sleep(delayMs); const int64_t q0 = Qpc(); SehClose(api, core); Log(std::format("{} closed in {:.0f} ms (on its own thread)", what, QpcMs(Qpc() - q0))); });
    }
    bool m_openStarted = false;
    // fast start (PushMpcFastStart.h): work that every connection needs, started at the input connection on other threads
    bool m_prewarmed = false;
    // our D3D11 device, created at the input connection on the first hardware adapter; CreateDevice takes it when MPC-VR
    // asks for that same adapter (~70 ms less at the output connection)
    struct SpecDevice { std::mutex lock; std::condition_variable cv; bool done = false; ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx; LUID luid = {}; double ms = 0; };
    std::shared_ptr<SpecDevice> m_spec;
    void FastStartKick()
    {
        StartShaderCache();
        if (!m_spec && !m_device) {
            auto spec = std::make_shared<SpecDevice>();
            m_spec = spec;
            fs::Detach([spec] {
                const int64_t q0 = Qpc();
                ComPtr<IDXGIFactory1> f; ComPtr<IDXGIAdapter1> a; DXGI_ADAPTER_DESC1 d = {};
                ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
                if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) {
                    for (UINT i = 0; SUCCEEDED(f->EnumAdapters1(i, &a)); i++, a.Reset())
                        if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.VendorId != 0x1414) break;
                    const D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
                    if (a && FAILED(D3D11CreateDevice(a.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                                      fl, 2, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) { dev.Reset(); ctx.Reset(); }
                }
                std::lock_guard<std::mutex> lk(spec->lock);
                spec->dev = dev; spec->ctx = ctx; spec->luid = d.AdapterLuid; spec->ms = QpcMs(Qpc() - q0); spec->done = true;
                spec->cv.notify_all();
            });
        }
        // the window's display (NVAPI start, the driver profile read): ready, and cached, before the connection asks for it
        fs::Detach([] { const int64_t q0 = Qpc(); const pm::Display d = pm::QueryDisplay(pm::PlayerWindow(), pm::VrrMode::Auto, true);
                        Log(std::format("fast start: display probed in {:.0f} ms ({} {:.3f} Hz)", QpcMs(Qpc() - q0), Narrow(d.name), d.refreshHz)); });
    }
    void Prewarm()
    {
        if (m_prewarmed || LegacyMode() || !m_cfg.prewarm) return;
        m_prewarmed = true;
        std::wstring err;
        if (!LoadApi(m_rt, err)) return;
        if (g_prewarm2Fn) {
            // with the NGX core for the settings Auto picks now (no VRR probe: the core does not depend on the multiplier)
            EnsureGpu(0);
            const pm::Display d = pm::QueryDisplay(pm::PlayerWindow(), pm::VrrMode::Off, m_gpu.nvidia());
            PushCoreSettings ps = ToCore(m_cfg.user, DecideNow(m_cfg.user, d));
            PushCoreConfig pc = {};
            pc.structSize = sizeof(pc); pc.abiVersion = PUSHCORE_ABI_VERSION;
            pc.pushfpsDir = m_rt.root.c_str();
            pc.shaderDir = m_rt.shaderDir.empty() ? nullptr : m_rt.shaderDir.c_str();
            pc.settings = &ps;
            pc.sr = pc.fg = pc.multiplier = pc.vfgQuality = -1;
            const int r = g_prewarm2Fn(&pc, &ps, 0);
            Log(std::format("fast start: pushcore prewarm started (NGX core {}) -> {}", (ps.sr && ps.srMethod == PUSHCORE_SR_METHOD_RTX) || ps.nr ? "too" : "not needed", r));
        } else if (g_prewarmFn) { const int r = g_prewarmFn(0); Log(std::format("fast start: pushcore prewarm started -> {}", r)); }
    }
    std::atomic<bool> m_reloading = false, m_faulted = false, m_deviceLostSeen = false;
    std::string m_openError;
    bool OpenFinished(); // takes the opened instance from the job
    bool m_coreDisc = true, m_firstAfterFlush = false;
    std::atomic<uint32_t> m_mult = 4;
    int64_t m_openQpc = 0, m_activeQpc = 0, m_flushStartQpc = 0, m_lastSubmitQpc = 0;
    std::atomic<int64_t> m_lastCoreOutQpc = 0;   // the last pushcore frame copied for delivery (the no-output guard)
    int m_submitsSinceOut = 0; int64_t m_seenOutQpc = 0;
    pm::DeliveryCap m_capState; int m_autoCap = 0; int64_t m_capWinQpc = 0; uint64_t m_capLate = 0, m_capGen = 0;   // the delivery check (Auto loop)
    REFERENCE_TIME m_lastSubmitPts = INT64_MIN;
    std::atomic<double> m_ptsStepMs = 0;
    double m_uploadMs = 0, m_submitMs = 0;
    uint64_t m_inFrames = 0, m_uploads = 0, m_skippedIngest = 0;
    uint32_t m_reloads = 0;
    // threads
    std::thread m_copier, m_deliverer, m_watcher;
    std::atomic<bool> m_quit = false, m_stopReq = false, m_flushing = false, m_eos = false, m_needPreroll = true, m_threadsRunning = false, m_inDeliver = false;
    std::atomic<bool> m_reloadWanted = false, m_discNext = true;
    std::atomic<uint32_t> m_flushGen = 0, m_copierAck = 0, m_reloadSeen = 0;
    std::atomic<uint64_t> m_delivered = 0;
    HANDLE m_hWake = nullptr, m_hLease = nullptr, m_hSubmit = nullptr, m_hTimer = nullptr, m_hWatchStop = nullptr;
    std::mutex m_readyLock;
    std::condition_variable m_readyCv;
    std::deque<ReadyItem> m_ready;
    std::wstring m_settingsSig;
    std::mutex m_sigLock;
    FILE* m_csv = nullptr;
    FILE* m_gpuCsv = nullptr;
    HRESULT m_lastDeliverHr = S_OK;
    int64_t m_resumedQpc = 0;
    std::atomic<bool> m_stepMode = false; // paused from playing: deliver real frames only (frame step)
    uint64_t m_stepSkips = 0;
    // delivery statistics and admission (deliverer thread)
    std::atomic<int64_t> m_qLateSum = 0, m_qN = 0;
    uint64_t m_delReal = 0, m_delGen = 0, m_delSrc = 0, m_dropGen = 0, m_dropReal = 0, m_lateDelivered = 0;
    std::atomic<int> m_fgLevel = 0, m_admGrace = 2;
    std::atomic<bool> m_admFlushed = false, m_admRateChanged = false;
    int64_t m_admQpc = 0, m_admLastUp = 0, m_admLastDown = 0, m_admHoldUntil = 0, m_admLastProbe = 0;
    uint64_t m_admGenPresented = 0, m_admGenLate = 0;
    struct AdmWin { uint32_t pres, late; } m_admHist[4] = {};
    int m_admHistN = 0, m_admGood = 0, m_admChanges = 0;
    double m_admBackoffS = 1.0;
    std::deque<float> m_recentSr, m_recentFg;
    double m_srMedian = 0, m_fgMedian = 0;
    std::atomic<int> m_reopens = 0;
    bool m_warnedSdrIntoY410 = false, m_passMtPending = false;
    int64_t m_csvFlushQpc = 0;
    // Auto
    std::mutex m_autoLock;               // the members below
    pm::Choice m_choice; bool m_haveChoice = false;
    PushCoreSettings m_applied = {}, m_pending = {};
    pm::Settings m_appliedSettings; pm::Display m_display, m_appliedDisplay; double m_appliedFps = 0;
    pm::Costs m_costs; bool m_learnedLoaded = false;
    struct TimedSample { PushCoreGpuTiming t; int fgLevel; int64_t qpc; };
    std::deque<TimedSample> m_autoTimings;
    uint32_t m_graphGen = 0; int64_t m_graphQpc = 0;
    std::atomic<bool> m_settingsPending = false;
    std::atomic<double> m_debandMsNow = -1.0;   // pushcore's measured deband pass (median), < 0 = off / not measured
    std::atomic<double> m_animeSrMsNow = -1.0;  // the anime upscaler's last frame (PushCoreStats.animeSrMs), < 0 = not running
    std::atomic<int> m_animeSrModelNow = -1;
    uint32_t m_settingsSeen = 0;
    std::thread m_autoThread; HANDLE m_hAutoStop = nullptr;
    int64_t m_lastCostChangeQpc = 0, m_lastUpgradeQpc = 0;
    std::map<std::wstring, int64_t> m_failedUpgrades; // graph key -> when an upgrade to it failed (admission stepped down)
    std::wstring m_lastUpgradeKey;
    double m_bitrate = -1; DWORD m_fourcc = 0; std::wstring m_bitrateHow; bool m_bitrateProbed = false;
    std::wstring m_gpuName;
    double m_gpuMsNow = -1;
    pm::Gpu m_gpu; bool m_gpuKnown = false; uint64_t m_gpuLuid = 0;   // pushcore_capabilities (m_gpu under m_autoLock)
    // PushMPC's SDR to HDR curve (set by SetItm; the output thread reads it for cbuffer S)
    struct Itm {
        bool on = false; float gamma = 2.2f, pw = 203.0f, kUser = 0.0f, t = 0.30f, p = 2.2f, mix = 0.15f;
        float peak = 1000.0f, fullFrame = 0.0f, target = 0.0f; bool reported = false; pm::HdrBoost boost = pm::HdrBoost::Off;
        bool limit = true;   // the bright-scene limiter
        bool rtx = false;    // the limiter on RTX Video HDR's output (on = false then; kUser = 1: g up to 1)
        bool operator==(const Itm&) const = default;
    };
    Itm m_itm; std::mutex m_itmLock; SideData m_itmSide;   // m_itmSide: the HDR10 static metadata of the curve's output
    std::atomic<bool> m_itmOn = false;
    std::atomic<bool> m_rtxLim = false;     // the limiter on RTX Video HDR's output
    std::atomic<bool> m_hlgOut = false;     // HLG source in an HDR10 output: explicit 1000 cd/m2 metadata
    std::atomic<bool> m_sceneCll = false;   // HDR source, SDR display: per-sample MaxCLL from HDR10+ / Dolby Vision L1
    // Scene-adaptive tone mapping on an SDR display: MPC-VR's Metadata mode follows each sample's MaxCLL, so it is set from
    // the scene peak (HDR10+ window 0 maxscl x 10000, or Dolby Vision L1 max_pq), clamped to the static MaxCLL / mastering
    // peak and to at least 100 cd/m2. Nothing changes on an HDR display (passthrough).
    static void SceneMaxCll(SideData& sd, double sceneNits = 0.0)
    {
        if (sceneNits <= 0.0) {
            if (auto p = sd.m.find(IID_MediaSideDataHDR10Plus); p != sd.m.end() && p->second.size() == sizeof(MediaSideDataHDR10Plus)) {
                const auto& h = *reinterpret_cast<const MediaSideDataHDR10Plus*>(p->second.data());
                if (h.num_windows >= 1) sceneNits = 10000.0 * std::max({ h.windows[0].maxscl[0], h.windows[0].maxscl[1], h.windows[0].maxscl[2] });
            } else if (auto d = sd.m.find(IID_MediaSideDataDOVIMetadataV2); d != sd.m.end() && d->second.size() == sizeof(MediaSideDataDOVIMetadata)) {
                const auto& m = *reinterpret_cast<const MediaSideDataDOVIMetadata*>(d->second.data());
                for (int i = 0; i < LAV_DOVI_MAX_EXTENSIONS; ++i) if (m.Extensions[i].level == 1) { sceneNits = PqToNits(m.Extensions[i].Level1.max_pq / 4095.0); break; }
            }
        }
        if (sceneNits <= 0.0) return;
        double cap = 0.0; unsigned fall = 0;
        if (auto c = sd.m.find(IID_MediaSideDataHDRContentLightLevel); c != sd.m.end() && c->second.size() == sizeof(MediaSideDataHDRContentLightLevel)) {
            cap = reinterpret_cast<const MediaSideDataHDRContentLightLevel*>(c->second.data())->MaxCLL; fall = reinterpret_cast<const MediaSideDataHDRContentLightLevel*>(c->second.data())->MaxFALL;
        }
        if (cap <= 0.0) if (auto h = sd.m.find(IID_MediaSideDataHDR); h != sd.m.end() && h->second.size() == sizeof(MediaSideDataHDR)) cap = reinterpret_cast<const MediaSideDataHDR*>(h->second.data())->max_display_mastering_luminance;
        const double v = std::max(100.0, cap > 0.0 ? std::min(sceneNits, cap) : sceneNits);
        MediaSideDataHDRContentLightLevel l = { unsigned(std::lround(v)), fall };
        sd.m[IID_MediaSideDataHDRContentLightLevel] = std::vector<BYTE>((const BYTE*)&l, (const BYTE*)&l + sizeof(l));
    }
    std::atomic<float> m_itmK = -1.0f;      // the limiter's k (< 0: none yet, the user's k)
    // the Auto limiter (output thread): statistics of each real frame on the GPU, read back a few frames later
    struct ItmLimiter {
        ComPtr<ID3D11Buffer> acc, staging[4]; ComPtr<ID3D11UnorderedAccessView> uav; ComPtr<ID3D11ComputeShader> cs;
        LONGLONG qpc[4] = {}; uint64_t written = 0, read = 0; float lastMean = -1.0f; uint64_t cuts = 0; LONGLONG lastQpc = 0; bool failed = false;
    } m_lim;
    // GPU time of the HDR10 output conversion (D3D11 timestamps; the SDR to HDR curve is fused into it)
    struct ConvTimer { ComPtr<ID3D11Query> disjoint[4], t0[4], t1[4]; uint64_t issued = 0, done = 0; bool open = false; std::vector<float> ms; LONGLONG lastLog = 0; } m_conv;
    std::atomic<double> m_convMs = -1.0;
    void ItmLimit(ID3D11ShaderResourceView* srv, CD3D11Sample* s, bool real, bool hdrIn);
    void ConvBegin(); void ConvEnd();
    uint32_t m_fgRun = PUSHCORE_FG_NONE, m_srRun = 0; bool m_cutGuardRun = false; std::wstring m_fgRunNote;   // what pushcore runs
    uint32_t m_subtitleState = 0, m_subtitlePixels = 0;   // PushFPS Subtitle Hold as pushcore reports it (PushCoreStats)
    // Animation Auto from the picture only: ContentDetect's class (fed on the streaming thread from every input sample)
    // and pushcore's held-frame share (the latest, not sticky). The show key only addresses the user's own choice.
    std::atomic<double> m_heldShare = -1.0;
    pushmpc::content::ContentDetector m_content;     // streaming thread only
    pushmpc::content::CombMeter m_comb;              // streaming thread only
    std::atomic<int> m_contentClass = 0;
    std::atomic<bool> m_contentDisc = false;
    std::atomic<float> m_contentConf = 0.f;
    int m_contentLogged = -1;
    void MeasureContent(IMediaSample* pIn, REFERENCE_TIME pts, bool disc);
    bool m_showLooked = false;
    std::wstring m_showKey;                     // round 5: this file's show (PushMpcShows.h), empty = none
    std::atomic<int> m_showChoice = -1;
    bool m_heldLogged = false;
};

// ------------------------------------------------------------------------------------------------ device
HRESULT CPushMpcFilter::CreateDevice(UINT preferredIndex)
{
    ComPtr<IDXGIFactory1> f;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&f));
    if (FAILED(hr)) return hr;
    ComPtr<IDXGIAdapter1> pick, a;
    DXGI_ADAPTER_DESC1 d = {};
    // MPC-VR's adapter (the one it presents on), any vendor; else the first hardware adapter
    auto hardware = [](const DXGI_ADAPTER_DESC1& x) { return !(x.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && x.VendorId != 0x1414; };
    if (SUCCEEDED(f->EnumAdapters1(preferredIndex, &a)) && SUCCEEDED(a->GetDesc1(&d)) && hardware(d)) pick = a;
    for (UINT i = 0; !pick && SUCCEEDED(f->EnumAdapters1(i, &a)); i++) {
        if (SUCCEEDED(a->GetDesc1(&d)) && hardware(d)) pick = a;
    }
    if (!pick) { Log("no hardware adapter"); return E_FAIL; }
    pick->GetDesc1(&d);
    m_gpuName = d.Description;
    EnsureGpu((uint64_t(uint32_t(d.AdapterLuid.HighPart)) << 32) | d.AdapterLuid.LowPart);
    const D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    const int64_t qDev = Qpc();
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    if (auto spec = std::move(m_spec)) {
        // fast start: the device made at the input connection, when it is on the adapter MPC-VR presents on
        std::unique_lock<std::mutex> lk(spec->lock);
        spec->cv.wait_for(lk, std::chrono::milliseconds(500), [&] { return spec->done; });
        if (spec->done && spec->dev && spec->luid.LowPart == d.AdapterLuid.LowPart && spec->luid.HighPart == d.AdapterLuid.HighPart) {
            dev = spec->dev; ctx = spec->ctx;
            Log(std::format("fast start: the D3D11 device made at the input connection is used ({:.0f} ms there)", spec->ms));
        }
    }
    hr = dev ? S_OK : D3D11CreateDevice(pick.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        fl, 2, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr)) { Log(std::format("D3D11CreateDevice failed 0x{:08X}", uint32_t(hr))); return hr; }
    const int64_t qShaders = Qpc();
    ComPtr<ID3D10Multithread> mt;
    ctx.As(&mt);
    if (mt) mt->SetMultithreadProtected(TRUE);
    m_device = dev; m_ctx = ctx;
    ctx.As(&m_ctx4);
    ComPtr<ID3DBlob> vs, p1, p2, p3, p4;
    if (FAILED(Compile("VS", "vs_5_0", vs)) || FAILED(Compile("PS_RGB", "ps_5_0", p1)) || FAILED(Compile("PS_YUV", "ps_5_0", p2))
        || FAILED(Compile("CS_RGB2Y410", "cs_5_0", p3)) || FAILED(Compile("CS_P0102Y410", "cs_5_0", p4))) return E_FAIL;
    dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &m_vs);
    dev->CreatePixelShader(p1->GetBufferPointer(), p1->GetBufferSize(), nullptr, &m_psRgb);
    dev->CreatePixelShader(p2->GetBufferPointer(), p2->GetBufferSize(), nullptr, &m_psYuv);
    dev->CreateComputeShader(p3->GetBufferPointer(), p3->GetBufferSize(), nullptr, &m_csRgb2Y410);
    dev->CreateComputeShader(p4->GetBufferPointer(), p4->GetBufferSize(), nullptr, &m_csP0102Y410);
    {
        ComPtr<ID3DBlob> r1, r2;
        if (FAILED(Compile("CS_RGB2Y416", "cs_5_0", r1)) || FAILED(Compile("CS_P0102Y416", "cs_5_0", r2))) return E_FAIL;
        dev->CreateComputeShader(r1->GetBufferPointer(), r1->GetBufferSize(), nullptr, &m_csRgb2Y416);
        dev->CreateComputeShader(r2->GetBufferPointer(), r2->GetBufferSize(), nullptr, &m_csP0102Y416);
        UINT y416 = 0;
        dev->CheckFormatSupport(DXGI_FORMAT_Y416, &y416);
        m_y416Uav = (y416 & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) != 0;
        Log(std::format("Y416 support 0x{:08X} (typed UAV {})", y416, m_y416Uav ? "yes" : "no"));
    }
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = 16; cbd.Usage = D3D11_USAGE_DEFAULT; cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    dev->CreateBuffer(&cbd, nullptr, &m_cbSize);
    {
        // P010 HDR output shaders: compiled here too, so a failure shows at device creation, not on first use
        ComPtr<ID3DBlob> q1, q2, q3, q4;
        if (FAILED(Compile("CS_RGB2P010_Y", "cs_5_0", q1)) || FAILED(Compile("CS_RGB2P010_UV", "cs_5_0", q2))
            || FAILED(Compile("CS_P0102P010_Y", "cs_5_0", q3)) || FAILED(Compile("CS_P0102P010_UV", "cs_5_0", q4))) return E_FAIL;
        dev->CreateComputeShader(q1->GetBufferPointer(), q1->GetBufferSize(), nullptr, &m_csRgb2P010Y);
        dev->CreateComputeShader(q2->GetBufferPointer(), q2->GetBufferSize(), nullptr, &m_csRgb2P010UV);
        dev->CreateComputeShader(q3->GetBufferPointer(), q3->GetBufferSize(), nullptr, &m_csP0102P010Y);
        dev->CreateComputeShader(q4->GetBufferPointer(), q4->GetBufferSize(), nullptr, &m_csP0102P010UV);
        D3D11_BUFFER_DESC sbd = cbd; sbd.ByteWidth = 80;
        dev->CreateBuffer(&sbd, nullptr, &m_cbSrc);
        D3D11_BUFFER_DESC dbd = cbd; dbd.ByteWidth = 112; // 7 float4: the Dolby Vision profile 5 block
        dev->CreateBuffer(&dbd, nullptr, &m_cbDv);
        D3D11_BUFFER_DESC obd = cbd; obd.ByteWidth = (3 + 97 + 32) * 16;
        dev->CreateBuffer(&obd, nullptr, &m_cbOsd);
        BuildOsdAtlas(dev.Get());
        D3D11_BUFFER_DESC xbd = cbd; xbd.ByteWidth = 178 * 16;
        dev->CreateBuffer(&xbd, nullptr, &m_cbDvx);
    }
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&sd, &m_sampler);
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 48; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    dev->CreateBuffer(&bd, nullptr, &m_cb);
    UINT y410 = 0;
    dev->CheckFormatSupport(DXGI_FORMAT_Y410, &y410);
    Log(std::format("device setup timing: D3D11CreateDevice {:.0f} ms, shaders and the rest {:.0f} ms", QpcMs(qShaders - qDev), QpcMs(Qpc() - qShaders)));
    LUID l = d.AdapterLuid;
    Log(std::format("own D3D11 device on {} (vendor {:04X}, LUID {:08X}:{:08X}), MPC-VR preferred adapter index {}; Y410 support 0x{:08X} (render target {})",
        Narrow(d.Description), d.VendorId, uint32_t(l.HighPart), l.LowPart, preferredIndex, y410, (y410 & D3D11_FORMAT_SUPPORT_RENDER_TARGET) ? "yes" : "no"));
    return S_OK;
}
HRESULT CPushMpcFilter::EnsureUpload()
{
    if (m_upload) return S_OK;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = m_inW; td.Height = m_inH; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = m_inP010 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DYNAMIC; td.BindFlags = D3D11_BIND_SHADER_RESOURCE; td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    HRESULT hr = m_device->CreateTexture2D(&td, nullptr, &m_upload);
    if (FAILED(hr)) { Log(std::format("upload texture failed 0x{:08X}", uint32_t(hr))); return hr; }
    D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
    sv.Format = m_inP010 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
    m_device->CreateShaderResourceView(m_upload.Get(), &sv, &m_uploadY);
    sv.Format = m_inP010 ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
    m_device->CreateShaderResourceView(m_upload.Get(), &sv, &m_uploadUV);
    return S_OK;
}
HRESULT CPushMpcFilter::Upload(IMediaSample* pIn)
{
    if (!m_device || FAILED(EnsureUpload())) return E_FAIL;
    BYTE* src = nullptr;
    if (FAILED(pIn->GetPointer(&src)) || !src) return E_FAIL;
    const LONG len = pIn->GetActualDataLength();
    const UINT rowBytes = m_inW * (m_inP010 ? 2 : 1);
    if (len < LONG(m_inStride * m_inH * 3 / 2)) { Log(std::format("short input sample {} bytes", len)); return E_FAIL; }
    const int64_t q0 = Qpc();
    D3D11_MAPPED_SUBRESOURCE m = {};
    HRESULT hr = m_ctx->Map(m_upload.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    if (FAILED(hr)) return hr;
    BYTE* dst = (BYTE*)m.pData;
    for (UINT y = 0; y < m_inH; y++) memcpy(dst + size_t(y) * m.RowPitch, src + size_t(y) * m_inStride, rowBytes);
    BYTE* dstUV = dst + size_t(m.RowPitch) * m_inH;
    const BYTE* srcUV = src + size_t(m_inStride) * m_inH;
    for (UINT y = 0; y < m_inH / 2; y++) memcpy(dstUV + size_t(y) * m.RowPitch, srcUV + size_t(y) * m_inStride, rowBytes);
    m_ctx->Unmap(m_upload.Get(), 0);
    m_uploadMs += QpcMs(Qpc() - q0);
    m_uploads++;
    return S_OK;
}
// A free output sample with its texture in format f. streaming: called on the streaming thread (Receive).
CD3D11Sample* CPushMpcFilter::GetFree(const OutFmt& f, bool streaming)
{
    auto* pin = Out();
    if (!pin->ours) return nullptr;
    for (;;) {
        if (m_flushing || m_quit || m_stopReq) return nullptr;
        IMediaSample* s = nullptr;
        HRESULT hr = pin->ours->GetBuffer(&s, nullptr, nullptr, AM_GBF_NOWAIT);
        if (SUCCEEDED(hr) && s) {
            auto* d = static_cast<CD3D11Sample*>(s);
            if (FAILED(CD3D11Allocator::EnsureTexture(m_device.Get(), d, f))) { d->Release(); return nullptr; }
            return d;
        }
        if (hr == VFW_E_NOT_COMMITTED) return nullptr;
        WaitForSingleObject(m_hWake, 1);
        (void)streaming;
    }
}
// records a full-target draw on a deferred context and executes it with the immediate context's state restored, so
// MPC-VR's pipeline state on the shared immediate context is untouched
void CPushMpcFilter::Draw(ID3D11DeviceContext* def, CD3D11Sample* s, ID3D11PixelShader* ps, ID3D11ShaderResourceView* a, ID3D11ShaderResourceView* b)
{
    D3D11_VIEWPORT vp = { 0, 0, float(s->fmt.w), float(s->fmt.h), 0, 1 };
    ID3D11RenderTargetView* rtv = s->rtv.Get();
    def->OMSetRenderTargets(1, &rtv, nullptr);
    def->RSSetViewports(1, &vp);
    def->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    def->IASetInputLayout(nullptr);
    def->VSSetShader(m_vs.Get(), nullptr, 0);
    def->PSSetShader(ps, nullptr, 0);
    ID3D11ShaderResourceView* srv[2] = { a, b };
    def->PSSetShaderResources(0, b ? 2 : 1, srv);
    ID3D11SamplerState* smp = m_sampler.Get();
    def->PSSetSamplers(0, 1, &smp);
    FillOsd(def, s->fmt.w, s->fmt.h, ps == m_psRgb.Get());
    ID3D11Buffer* cb[6] = { m_cb.Get(), nullptr, nullptr, m_cbDv.Get(), m_cbDvx.Get(), m_cbOsd.Get() };
    def->PSSetConstantBuffers(0, 6, cb);
    { ID3D11ShaderResourceView* o = m_osdSrv.Get(); def->PSSetShaderResources(2, 1, &o); }
    def->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    def->RSSetState(nullptr);
    def->Draw(3, 0);
    ComPtr<ID3D11CommandList> cl;
    def->FinishCommandList(FALSE, &cl);
    m_ctx->ExecuteCommandList(cl.Get(), TRUE);
}
// constants of the HDR10 compute shaders (cbuffer C: output size; cbuffer S: source size, code scale, SDR flags)
void CPushMpcFilter::SetComputeConstants(ID3D11DeviceContext* def, CD3D11Sample* s, bool sdrIn, bool srcSdr)
{
    const UINT size[4] = { s->fmt.w, s->fmt.h, 0, 0 };
    def->UpdateSubresource(m_cbSize.Get(), 0, nullptr, size, 0, 0);
    Itm itm; { std::lock_guard<std::mutex> lk(m_itmLock); itm = m_itm; }
    const float k = m_itmK.load() >= 0.0f ? std::min(m_itmK.load(), itm.kUser) : itm.kUser;
    struct { UINT w, h; float codeScale, sdrIn, srcSdr, dvP5, dvNorm, itmOn; float itm0[4], itm1[4], itm2[4]; } src = { m_inW, m_inH, m_inP010 ? 65535.0f / 64.0f : 255.0f * 4.0f,
        sdrIn ? 1.0f : 0.0f, srcSdr ? 1.0f : 0.0f, (m_dvP5 && m_dvCur.active && !sdrIn) ? 1.0f : 0.0f, m_inP010 ? (65535.0f / 64.0f) / 1023.0f : 1.0f,
        itm.on && !m_cfg.itmBypass ? 1.0f : 0.0f, { itm.gamma, itm.pw, k, itm.t }, { itm.p, itm.mix, 1.0f, 0.0f },
        { itm.rtx && !m_cfg.itmBypass ? 1.0f : 0.0f, std::clamp(k, 0.0f, 1.0f), itm.pw, 0.0f } };
    def->UpdateSubresource(m_cbSrc.Get(), 0, nullptr, &src, 0, 0);
    FillOsd(def, s->fmt.w, s->fmt.h, sdrIn || !srcSdr);
    ID3D11Buffer* cb[6] = { m_cb.Get(), m_cbSize.Get(), m_cbSrc.Get(), m_cbDv.Get(), m_cbDvx.Get(), m_cbOsd.Get() };
    def->CSSetConstantBuffers(0, 6, cb);
    { ID3D11ShaderResourceView* o = m_osdSrv.Get(); def->CSSetShaderResources(2, 1, &o); }
    ID3D11SamplerState* smp = m_sampler.Get();
    def->CSSetSamplers(0, 1, &smp);
}
// the same for the compute shaders that write Y410 through its UAV
void CPushMpcFilter::Dispatch(ID3D11DeviceContext* def, CD3D11Sample* s, ID3D11ComputeShader* cs, ID3D11ShaderResourceView* a, ID3D11ShaderResourceView* b, bool sdrIn, bool srcSdr)
{
    SetComputeConstants(def, s, sdrIn, srcSdr);
    def->CSSetShader(cs, nullptr, 0);
    ID3D11ShaderResourceView* srv[2] = { a, b };
    def->CSSetShaderResources(0, b ? 2 : 1, srv);
    ID3D11UnorderedAccessView* uav = s->uav.Get();
    def->CSSetUnorderedAccessViews(s->fmt.fmt == Fmt::Y416 ? 3 : 0, 1, &uav, nullptr); // u3: Y416 (R16G16B16A16), u0: Y410 (R32_UINT)
    def->Dispatch((s->fmt.w + 7) / 8, (s->fmt.h + 7) / 8, 1);
    ComPtr<ID3D11CommandList> cl;
    def->FinishCommandList(FALSE, &cl);
    m_ctx->ExecuteCommandList(cl.Get(), TRUE);
}
HRESULT CPushMpcFilter::EnsureScratch(P010Scratch& s, UINT w, UINT h)
{
    if (s.tex && s.w == w && s.h == h) return S_OK;
    s = {};
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_P010;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = m_device->CreateTexture2D(&td, nullptr, &s.tex);
    ComPtr<ID3D11Device3> d3;
    if (SUCCEEDED(hr)) hr = m_device.As(&d3);
    for (int plane = 0; SUCCEEDED(hr) && plane < 2; plane++) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC1 ud = {};
        ud.Format = plane == 0 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R16G16_UNORM;
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        ud.Texture2D.MipSlice = 0; ud.Texture2D.PlaneSlice = UINT(plane);
        ComPtr<ID3D11UnorderedAccessView1> v;
        hr = d3->CreateUnorderedAccessView1(s.tex.Get(), &ud, &v);
        if (SUCCEEDED(hr)) (plane == 0 ? s.y : s.uv) = v;
    }
    if (FAILED(hr)) { Log(std::format("P010 scratch {}x{} failed 0x{:08X}", w, h, uint32_t(hr))); s = {}; return hr; }
    s.w = w; s.h = h;
    return S_OK;
}
// The Auto limiter of PushMPC's SDR to HDR curve: a global, slow cap on the expansion
// term only. Each real frame's statistics (CS_ItmStats on pushcore's SDR RGB8) are read back a few frames later; from
// them k_lim = (target / paper white - mean Y) / mean(Y E(s) / s), clamped to 0..k, so the frame-average light stays at
// the target; k follows k_lim with attack 0.15 s (down) and release 1.5 s (up), and snaps at a scene cut (the cut guard's
// count from pushcore, or a jump of the mean signal by more than 0.10 when no guard runs). k changes on real frames only,
// so a real frame and its generated frames get the same curve.
void CPushMpcFilter::ItmLimit(ID3D11ShaderResourceView* srv, CD3D11Sample* s, bool real, bool hdrIn)
{
    if (!real || !srv || m_lim.failed || !m_ctx || !m_device) return;
    if (hdrIn ? !m_rtxLim.load() : !m_itmOn.load()) return;   // RTX Video HDR's output (HDR in) or the curve's input (SDR)
    Itm itm; { std::lock_guard<std::mutex> lk(m_itmLock); itm = m_itm; }
    if (itm.kUser <= 0.0f || !itm.limit) { m_itmK = -1.0f; return; }
    if (!m_lim.acc) {
        ComPtr<ID3DBlob> b;
        D3D11_BUFFER_DESC d = {}; d.ByteWidth = 16; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_UNORDERED_ACCESS; d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        D3D11_UNORDERED_ACCESS_VIEW_DESC u = {}; u.Format = DXGI_FORMAT_R32_TYPELESS; u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; u.Buffer.NumElements = 4; u.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        D3D11_BUFFER_DESC sd = {}; sd.ByteWidth = 16; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        bool ok = SUCCEEDED(Compile("CS_ItmStats", "cs_5_0", b)) && SUCCEEDED(m_device->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &m_lim.cs))
               && SUCCEEDED(m_device->CreateBuffer(&d, nullptr, &m_lim.acc)) && SUCCEEDED(m_device->CreateUnorderedAccessView(m_lim.acc.Get(), &u, &m_lim.uav));
        for (auto& st : m_lim.staging) ok = ok && SUCCEEDED(m_device->CreateBuffer(&sd, nullptr, &st));
        if (!ok) { m_lim.failed = true; m_lim.acc.Reset(); Log("SDR to HDR limiter: GPU resources failed; the boost runs without the limiter"); return; }
    }
    // results of earlier real frames (blocking only when all four slots are in flight)
    while (m_lim.read < m_lim.written) {
        const uint32_t slot = uint32_t(m_lim.read % 4);
        D3D11_MAPPED_SUBRESOURCE mp = {};
        const HRESULT hr = m_ctx->Map(m_lim.staging[slot].Get(), 0, D3D11_MAP_READ, m_lim.written - m_lim.read >= 4 ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &mp);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) break;
        m_lim.read++;
        if (FAILED(hr)) continue;
        uint32_t v[4] = {};
        std::memcpy(v, mp.pData, sizeof(v));
        m_ctx->Unmap(m_lim.staging[slot].Get(), 0);
        if (!v[3]) continue;
        const float n = float(v[3]) * 1024.0f, extra = float(v[0]) / n, meanY = float(v[1]) / n, mean = float(v[2]) / n;
        const float kLim = std::clamp((itm.target / itm.pw - meanY) / std::max(extra, 1e-5f), 0.0f, itm.kUser);
        uint64_t cuts = m_lim.cuts;
        if ((g_coreFeatures & PUSHCORE_FEATURE_CUT_COUNT) && m_core) { PushCoreStats st = { sizeof(st) }; if (m_api->stats(m_core, &st) == PUSHCORE_OK) cuts = st.cutGuardCuts; }
        const bool cut = cuts != m_lim.cuts || (m_lim.lastMean >= 0.0f && std::fabs(mean - m_lim.lastMean) > 0.10f);
        float k = m_itmK.load();
        if (k < 0.0f || cut) k = kLim;
        else {
            const double dt = std::clamp(QpcMs(m_lim.qpc[slot] - m_lim.lastQpc) / 1000.0, 0.0, 1.0);
            k += float((kLim - k) * (1.0 - std::exp(-dt / (kLim < k ? 0.15 : 1.5))));
        }
        m_itmK = k; m_lim.cuts = cuts; m_lim.lastMean = mean; m_lim.lastQpc = m_lim.qpc[slot];
    }
    // this real frame's statistics
    const UINT zero[4] = {};
    m_ctx->ClearUnorderedAccessViewUint(m_lim.uav.Get(), zero);
    SetComputeConstants(m_ctx.Get(), s, !hdrIn, false);
    m_ctx->CSSetShader(m_lim.cs.Get(), nullptr, 0);
    m_ctx->CSSetShaderResources(0, 1, &srv);
    ID3D11UnorderedAccessView* uav = m_lim.uav.Get();
    m_ctx->CSSetUnorderedAccessViews(4, 1, &uav, nullptr);
    m_ctx->Dispatch(((s->fmt.w + 7) / 8 + 7) / 8, ((s->fmt.h + 7) / 8 + 7) / 8, 1);
    ID3D11UnorderedAccessView* noUav = nullptr; ID3D11ShaderResourceView* noSrv = nullptr;
    m_ctx->CSSetUnorderedAccessViews(4, 1, &noUav, nullptr);
    m_ctx->CSSetShaderResources(0, 1, &noSrv);
    const uint32_t w = uint32_t(m_lim.written % 4);
    m_ctx->CopyResource(m_lim.staging[w].Get(), m_lim.acc.Get());
    m_lim.qpc[w] = Qpc();
    m_lim.written++;
}
// D3D11 timestamps around the HDR10 output conversion of each output frame (four in flight; a frame finding them all
// busy is not timed). Median and p95 of 120 frames go to the log every 10 s (ItmBypass=1 times the same pass without
// the curve, for the A/B).
void CPushMpcFilter::ConvBegin()
{
    m_conv.open = false;
    if (!m_ctx || !m_device) return;
    if (!m_conv.disjoint[0]) {
        D3D11_QUERY_DESC dj = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }, ts = { D3D11_QUERY_TIMESTAMP, 0 };
        for (int i = 0; i < 4; i++)
            if (FAILED(m_device->CreateQuery(&dj, &m_conv.disjoint[i])) || FAILED(m_device->CreateQuery(&ts, &m_conv.t0[i])) || FAILED(m_device->CreateQuery(&ts, &m_conv.t1[i])))
                { for (auto& q : m_conv.disjoint) q.Reset(); return; }
    }
    while (m_conv.done < m_conv.issued) {
        const uint32_t sl = uint32_t(m_conv.done % 4);
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {}; UINT64 a = 0, b = 0;
        if (m_ctx->GetData(m_conv.disjoint[sl].Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) break;
        if (m_ctx->GetData(m_conv.t0[sl].Get(), &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) break;
        if (m_ctx->GetData(m_conv.t1[sl].Get(), &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) break;
        m_conv.done++;
        if (!dj.Disjoint && dj.Frequency && b >= a) m_conv.ms.push_back(float(double(b - a) * 1000.0 / double(dj.Frequency)));
    }
    if (m_conv.ms.size() >= 120) {
        std::vector<float> v = m_conv.ms; m_conv.ms.clear();
        std::sort(v.begin(), v.end());
        m_convMs = v[v.size() / 2];
        const LONGLONG q = Qpc();
        if (!m_conv.lastLog || QpcMs(q - m_conv.lastLog) >= 10000.0) {
            m_conv.lastLog = q;
            Log(std::format("HDR10 output conversion GPU time ({}): median {:.3f} ms, p95 {:.3f} ms over {} frames", !m_itmOn.load() ? "no SDR to HDR curve"
                : m_cfg.itmBypass ? "ItmBypass: the plain 203 cd/m2 mapping" : "with PushMPC's SDR to HDR curve", v[v.size() / 2], v[v.size() * 95 / 100], v.size()));
        }
    }
    if (m_conv.issued - m_conv.done >= 4) return;
    const uint32_t sl = uint32_t(m_conv.issued % 4);
    m_ctx->Begin(m_conv.disjoint[sl].Get());
    m_ctx->End(m_conv.t0[sl].Get());
    m_conv.open = true;
}
void CPushMpcFilter::ConvEnd()
{
    if (!m_conv.open) return;
    const uint32_t sl = uint32_t(m_conv.issued % 4);
    m_ctx->End(m_conv.t1[sl].Get());
    m_ctx->End(m_conv.disjoint[sl].Get());
    m_conv.issued++; m_conv.open = false;
}
// ------------------------------------------------------------------------------------------------ OSD
// The glyph atlas: Segoe UI Semibold, grayscale antialiased, printable ASCII plus the middle dot and the arrow, one row of
// 48-pixel cells (sampled bilinearly: about 1:1 at 4K, 2:1 at 1080p)
void CPushMpcFilter::BuildOsdAtlas(ID3D11Device* dev)
{
    const int cellH = 48;
    std::vector<wchar_t> chars;
    for (wchar_t c = 32; c < 127; ++c) chars.push_back(c);
    chars.push_back(0x00B7); chars.push_back(0x2192);
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT font = CreateFontW(-36, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, font);
    std::vector<int> adv(chars.size());
    int width = 0;
    for (size_t i = 0; i < chars.size(); ++i) { SIZE sz{}; GetTextExtentPoint32W(dc, &chars[i], 1, &sz); adv[i] = std::max(1, int(sz.cx)); width += adv[i] + 2; }
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = width; bi.bmiHeader.biHeight = -cellH;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp && bits) {
        HGDIOBJ oldBmp = SelectObject(dc, bmp);
        std::memset(bits, 0, size_t(width) * cellH * 4);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
        int x = 0;
        for (size_t i = 0; i < chars.size(); ++i) {
            TextOutW(dc, x + 1, 4, &chars[i], 1);
            m_osdGlyph[i][0] = float(x + 1); m_osdGlyph[i][1] = float(adv[i]);
            x += adv[i] + 2;
        }
        GdiFlush();
        std::vector<uint8_t> r8(size_t(width) * cellH);
        for (size_t p = 0; p < r8.size(); ++p) r8[p] = static_cast<const uint8_t*>(bits)[p * 4 + 2];
        D3D11_TEXTURE2D_DESC td{ UINT(width), UINT(cellH), 1, 1, DXGI_FORMAT_R8_UNORM, { 1, 0 }, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE };
        D3D11_SUBRESOURCE_DATA sd{ r8.data(), UINT(width), 0 };
        ComPtr<ID3D11Texture2D> tex;
        if (SUCCEEDED(dev->CreateTexture2D(&td, &sd, &tex))) dev->CreateShaderResourceView(tex.Get(), nullptr, &m_osdSrv);
        m_osdAtlasW = float(width); m_osdCellH = float(cellH);
        SelectObject(dc, oldBmp);
        DeleteObject(bmp);
    }
    SelectObject(dc, oldFont);
    DeleteObject(font);
    DeleteDC(dc);
    if (!m_osdSrv) Log("OSD: the glyph atlas could not be made: no on-screen status");
}
// the OSD's constants for one output frame of w x h (top-left, text height 2.2 % of the frame)
void CPushMpcFilter::FillOsd(ID3D11DeviceContext* def, UINT w, UINT h, bool draw)
{
    if (!m_cbOsd) return;
    struct { float box[4], text[4], params[4], glyph[97][4], chars[32][4]; } o{};
    const pm::OsdMode mode = m_cfg.user.osd;
    std::wstring text;
    if (draw && m_osdSrv && m_mode == Mode::Active && mode != pm::OsdMode::Off) { std::lock_guard<std::mutex> lk(m_osdLock); text = m_osdText; }
    // no box until the status thread has composed the line (F-UI3: an empty box for the first second)
    const bool visible = !text.empty() && (mode == pm::OsdMode::Always || QpcMs(Qpc()) < m_osdUntilMs.load());
    if (visible) {
        float x = 0.0f; int n = 0;
        for (wchar_t ch : text) {
            if (n >= 64) break;
            const int g = ch >= 32 && ch < 127 ? ch - 32 : ch == 0x00B7 ? 95 : ch == 0x2192 ? 96 : ('?' - 32);
            o.chars[n / 2][(n & 1) * 2] = x; o.chars[n / 2][(n & 1) * 2 + 1] = float(g);
            x += m_osdGlyph[g][1]; ++n;
        }
        const float scale = float(h) * 0.022f / m_osdCellH, pad = 0.35f * m_osdCellH * scale, margin = float(h) * 0.015f;
        o.box[0] = margin; o.box[1] = margin; o.box[2] = margin + 2.0f * pad + x * scale; o.box[3] = margin + m_osdCellH * scale + 0.6f * pad;
        o.text[0] = margin + pad; o.text[1] = margin + 0.3f * pad; o.text[2] = scale; o.text[3] = 1.0f;
        o.params[0] = float(n); o.params[1] = m_osdAtlasW; o.params[2] = m_osdCellH;
        for (int i = 0; i < 97; ++i) { o.glyph[i][0] = m_osdGlyph[i][0]; o.glyph[i][1] = m_osdGlyph[i][1]; }
    }
    def->UpdateSubresource(m_cbOsd.Get(), 0, nullptr, &o, 0, 0);
}
// MPC Video Renderer's HDR settings at once, through its own settings interface (IVideoRenderer::SetSettings applies
// them to the running video processor); only when the running MPC-VR is the 0.11.2 release whose Settings_t layout this
// build knows. Otherwise the registry values apply from its next start.
void CPushMpcFilter::ApplyMpcVrLive(DWORD nits)
{
    IPin* to = m_pOutput ? m_pOutput->GetConnected() : nullptr;
    if (!to) return;
    PIN_INFO pi{};
    if (FAILED(to->QueryPinInfo(&pi)) || !pi.pFilter) return;
    ComPtr<IBaseFilter> f; f.Attach(pi.pFilter);
    ComPtr<IVideoRenderer> vr;
    if (FAILED(f.As(&vr)) || !vr) return;
    HMODULE mod = nullptr; wchar_t path[MAX_PATH] = {};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, *reinterpret_cast<LPCWSTR*>(f.Get()), &mod) || !GetModuleFileNameW(mod, path, MAX_PATH)) return;
    {   // round 2 (C5): another renderer that answers for MPC-VR's interface (early FrankyRenderer builds reused its CLSID)
        // must not get MPC-VR's Settings_t: only a module named MpcVideoRenderer*.ax (MPC-HC's, MPC-BE's or a standalone copy)
        const wchar_t* base = wcsrchr(path, L'\\'); base = base ? base + 1 : path;
        if (_wcsnicmp(base, L"MpcVideoRenderer", 16) != 0) { LogW(std::wstring(L"the renderer module is not MPC Video Renderer (") + base + L"): its settings are not changed live"); return; }
    }
    DWORD h = 0; const DWORD sz = GetFileVersionInfoSizeW(path, &h);
    std::vector<BYTE> vi(sz);
    VS_FIXEDFILEINFO* ffi = nullptr; UINT len = 0;
    if (!sz || !GetFileVersionInfoW(path, 0, sz, vi.data()) || !VerQueryValueW(vi.data(), L"\\", (void**)&ffi, &len) || !ffi) return;
    const UINT major = HIWORD(ffi->dwFileVersionMS), minor = LOWORD(ffi->dwFileVersionMS), build = HIWORD(ffi->dwFileVersionLS);
    if (major != 0 || minor != 11 || build != 2) { Log(std::format("MPC Video Renderer {}.{}.{}: its settings apply from its next start (live only for 0.11.2)", major, minor, build)); return; }
    Settings_t st;
    vr->GetSettings(st);
    if (st.bHdrLocalToneMapping && st.iHdrLocalToneMappingType == 7 && DWORD(st.iHdrDisplayMaxNits) == nits) return;
    st.bHdrLocalToneMapping = true; st.iHdrLocalToneMappingType = 7; st.iHdrDisplayMaxNits = int(nits);
    vr->SetSettings(st);
    Log(std::format("MPC Video Renderer HDR settings applied live (IVideoRenderer::SetSettings): local tone mapping Spline, {} cd/m2", nits));
}
// two compute passes (luma at full size, chroma at half size) into the scratch, then one copy into the sample
void CPushMpcFilter::DispatchP010(ID3D11DeviceContext* def, CD3D11Sample* s, P010Scratch& scr, bool fromRgb, ID3D11ShaderResourceView* a, ID3D11ShaderResourceView* b, bool sdrIn, bool srcSdr)
{
    if (FAILED(EnsureScratch(scr, s->fmt.w, s->fmt.h))) return;
    SetComputeConstants(def, s, sdrIn, srcSdr);
    ID3D11ShaderResourceView* srv[2] = { a, b };
    def->CSSetShaderResources(0, b ? 2 : 1, srv);
    ID3D11UnorderedAccessView* uav[2] = { scr.y.Get(), scr.uv.Get() };
    def->CSSetUnorderedAccessViews(1, 2, uav, nullptr);
    def->CSSetShader(fromRgb ? m_csRgb2P010Y.Get() : m_csP0102P010Y.Get(), nullptr, 0);
    def->Dispatch((s->fmt.w + 7) / 8, (s->fmt.h + 7) / 8, 1);
    def->CSSetShader(fromRgb ? m_csRgb2P010UV.Get() : m_csP0102P010UV.Get(), nullptr, 0);
    def->Dispatch((s->fmt.w / 2 + 7) / 8, (s->fmt.h / 2 + 7) / 8, 1);
    ComPtr<ID3D11CommandList> cl;
    def->FinishCommandList(FALSE, &cl);
    m_ctx->ExecuteCommandList(cl.Get(), TRUE);
    m_ctx->CopyResource(s->tex.Get(), scr.tex.Get());
}
// the Dolby Vision profile 5 block of the warm-up shaders, in PushFPS's lane layout (DolbyVisionP5.h)
void CPushMpcFilter::SetDvConstants(ID3D11DeviceContext* def)
{
    float c[28] = {};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) { c[row * 4 + col] = m_dvCur.ycc[row * 3 + col]; c[12 + row * 4 + col] = m_dvCur.lms[row * 3 + col]; }
        c[row * 4 + 3] = m_dvCur.curve[0][row];
        c[12 + row * 4 + 3] = m_dvCur.curve[1][row];
    }
    c[24] = m_dvCur.curve[2][0]; c[25] = m_dvCur.curve[2][1]; c[26] = m_dvCur.curve[2][2];
    c[27] = m_dvCur.active ? 1.0f : 0.0f;
    def->UpdateSubresource(m_cbDv.Get(), 0, nullptr, c, 0, 0);
    static float x[178][4] = {};
    std::memcpy(x, m_dvCur.exactCurve, sizeof(m_dvCur.exactCurve));
    x[177][0] = m_dvCur.active && m_dvCur.exact ? 1.0f : 0.0f;
    if (m_cbDvx) def->UpdateSubresource(m_cbDvx.Get(), 0, nullptr, x, 0, 0);
}
void CPushMpcFilter::SetYuvConstants(ID3D11DeviceContext* def, bool hdrToSdr)
{
    DXVA2_ExtendedFormat ex; ex.value = m_inCtrl;
    const bool full = m_inCtrl && ex.NominalRange == DXVA2_NominalRange_0_255;
    float c[12] = {};
    if (m_inP010) { // UNORM16 holds the 10-bit code in the high bits
        const float k = 64.0f / 65535.0f;
        c[0] = full ? 0 : 64 * k; c[1] = full ? 1.0f / (1023 * k) : 1.0f / (876 * k); c[2] = 512 * k; c[3] = full ? 1.0f / (1023 * k) : 1.0f / (896 * k);
    } else {
        c[0] = full ? 0 : 16.0f / 255; c[1] = full ? 1.0f : 255.0f / 219; c[2] = 128.0f / 255; c[3] = full ? 1.0f : 255.0f / 224;
    }
    const UINT m = m_inCtrl ? ex.VideoTransferMatrix : (m_visW <= 1024 && m_visH <= 576 ? 2 : 1);
    if (m == 2) { c[4] = 1.402f; c[5] = 0.344136f; c[6] = 0.714136f; c[7] = 1.772f; }          // BT.601
    else if (m == 4 || m == 5) { c[4] = 1.4746f; c[5] = 0.16455f; c[6] = 0.57135f; c[7] = 1.8814f; } // BT.2020
    else { c[4] = 1.5748f; c[5] = 0.1873f; c[6] = 0.4681f; c[7] = 1.8556f; }                   // BT.709
    c[8] = hdrToSdr ? 1.0f : 0.0f;
    c[9] = (m_dvP5 && m_dvCur.active) ? 1.0f : 0.0f;          // Dolby Vision profile 5 warm-up conversion
    c[10] = m_inP010 ? (65535.0f / 64.0f) / 1023.0f : 1.0f;   // sample -> full-range normalised code
    c[11] = (m_inCtrl && ex.VideoTransferFunction == kTransHLG && !(m_dvP5 && m_dvCur.active)) ? 1.0f : 0.0f;   // HLG source -> PQ
    def->UpdateSubresource(m_cb.Get(), 0, nullptr, c, 0, 0);
}
// a source frame (upload texture) into a sample, converted into the output's signal and size (warm-up, reload, fallback)
void CPushMpcFilter::DrawSource(CD3D11Sample* s)
{
    if (s->fmt.fmt == Fmt::NV12 || s->fmt.fmt == Fmt::P010) {
        m_ctx->CopyResource(s->tex.Get(), m_upload.Get());
        return;
    }
    if (!m_defRecv) m_device->CreateDeferredContext(0, &m_defRecv);
    const bool srcHdr = InputHdr();
    SetYuvConstants(m_defRecv.Get(), srcHdr && !IsHdrFmt(s->fmt.fmt));
    if (m_dvP5) SetDvConstants(m_defRecv.Get());
    if (s->fmt.fmt == Fmt::Y410 || s->fmt.fmt == Fmt::Y416) {
        Dispatch(m_defRecv.Get(), s, s->fmt.fmt == Fmt::Y416 ? m_csP0102Y416.Get() : m_csP0102Y410.Get(), m_uploadY.Get(), m_uploadUV.Get(), false, !srcHdr);
    } else if (s->fmt.fmt == Fmt::P010H) {
        DispatchP010(m_defRecv.Get(), s, m_scrRecv, false, m_uploadY.Get(), m_uploadUV.Get(), false, !srcHdr);
    } else {
        Draw(m_defRecv.Get(), s, m_psYuv.Get(), m_uploadY.Get(), m_uploadUV.Get());
    }
}
ID3D11ShaderResourceView* CPushMpcFilter::SrvFor(ID3D11Texture2D* t)
{
    std::lock_guard<std::mutex> lk(m_srvLock);
    auto it = m_srvs.find(t);
    if (it != m_srvs.end()) return it->second.Get();
    ComPtr<ID3D11ShaderResourceView> v;
    if (FAILED(m_device->CreateShaderResourceView(t, nullptr, &v))) return nullptr;
    m_srvs[t] = v;
    return v.Get();
}
int CPushMpcFilter::LeadUs()
{
    if (m_cfg.leadUs >= 0) return m_cfg.leadUs;
    static const int lead = [] {
        DEVMODEW dm = {}; dm.dmSize = sizeof(dm);
        int hz = 60;
        if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) hz = int(dm.dmDisplayFrequency);
        // MPC-VR's "adjust present time" sleeps until (frame time - half a refresh period, whole ms) before Present:
        // delivering a little later than that makes it present at once, at the time this thread chose
        return std::max(0, int(500000.0 / hz) - 400);
    }();
    return lead;
}

// ------------------------------------------------------------------------------------------------ pushcore
// Round 2 (F8): a later file in this process whose media-type frame rate gives another NGX key than the warm core
// waits for the first 8 timestamp steps (~0.33 s at 24 fps; at most 0.7 s) and opens once for the measured rate.
// Opening at once on the type's rate would close the warm core for a graph that the timestamps then replace, and the
// right graph would open cold (fix round 1: 3.3 s to active). The first file of a process and a type whose choice
// keeps the warm key open at once, as before.
bool CPushMpcFilter::ShouldDeferOpen()
{
    if (m_openDeferred || LegacyMode() || !m_cfg.warmCache || m_baseStepMs.load() > 0) return false;
    NgxKey last;
    { std::lock_guard<std::mutex> lk(g_ngxLock); if (!g_haveNgx) return false; last = g_lastNgx; }
    EnsureGpu(m_gpuLuid);
    const pm::Display d = pm::QueryDisplay(pm::PlayerWindow(), m_cfg.user.vrr, m_gpu.nvidia());
    const NgxKey want = NgxKeyOf(ToCore(m_cfg.user, DecideNow(m_cfg.user, d)));
    if (want == last) return false;
    m_openDeferred = true;
    m_deferQpc = Qpc();
    Log(std::format("open deferred: the media type's {:.3f} fps gives an NGX key ({}x) other than the warm core's ({}x); waiting for the first timestamps (at most 700 ms)",
        1e7 / double(m_atpf > 0 ? m_atpf : 417083), want.mult, last.mult));
    return true;
}
void CPushMpcFilter::StartOpen(IMediaSample* first)
{
    if (m_openStarted) return;
    if (ShouldDeferOpen()) return;
    m_openStarted = true;
    ShowChoiceOnce();
    PushCoreStreamInfo si = {};
    si.structSize = sizeof(si);
    si.width = m_visW; si.height = m_visH;
    si.format = m_inP010 ? PUSHCORE_INPUT_P010 : PUSHCORE_INPUT_NV12;
    // the nominal rate pushcore starts from: the measured interval when the timestamps already gave one (a "29.97" tag on
    // a 24p body), else the media type's (a prior; pushcore measures the submitted timestamps itself as well)
    const REFERENCE_TIME atpf = m_baseStepMs.load() > 1.0 ? REFERENCE_TIME(std::llround(m_baseStepMs.load() * 1e4)) : m_atpf;
    si.fpsNum = 10000000; si.fpsDen = int32_t(atpf);
    if (atpf >= 417080 && atpf <= 417090) { si.fpsNum = 24000; si.fpsDen = 1001; }
    else if (atpf >= 333660 && atpf <= 333670) { si.fpsNum = 30000; si.fpsDen = 1001; }
    else if (atpf >= 166830 && atpf <= 166840) { si.fpsNum = 60000; si.fpsDen = 1001; }
    DXVA2_ExtendedFormat ex; ex.value = m_inCtrl;   // the media type's control flags carry DXVA2_ExtendedFormat as is
    const bool hdr = InputHdr();
    si.range = m_inCtrl ? FfRange(ex.NominalRange) : 1;
    si.matrix = m_inCtrl ? FfMatrix(ex.VideoTransferMatrix) : 2;
    si.primaries = m_inCtrl ? FfPrimaries(ex.VideoPrimaries) : 2;
    si.transfer = m_inCtrl ? FfTransfer(ex.VideoTransferFunction) : 2;
    if (si.range == 0) si.range = 1;
    if (hdr) {
        if (si.matrix == 2) si.matrix = 9;
        if (si.primaries == 2) si.primaries = 9;
        const auto& m = m_hdrSide.m;
        if (auto it = m.find(IID_MediaSideDataHDR); it != m.end() && it->second.size() == sizeof(MediaSideDataHDR))
            si.masteringPeakNits = float(reinterpret_cast<const MediaSideDataHDR*>(it->second.data())->max_display_mastering_luminance);
        if (auto it = m.find(IID_MediaSideDataHDRContentLightLevel); it != m.end() && it->second.size() == sizeof(MediaSideDataHDRContentLightLevel)) {
            si.maxCllNits = float(reinterpret_cast<const MediaSideDataHDRContentLightLevel*>(it->second.data())->MaxCLL);
            si.maxFallNits = float(reinterpret_cast<const MediaSideDataHDRContentLightLevel*>(it->second.data())->MaxFALL);
        }
        // HDR10 (Y410) to MPC-VR: PushFPS's rule useHdrPreview(hdr, displayHdr = 1) gives its HDR10 output;
        // HDR to SDR by PushMPC (m_coreToneMap: a BGRA connection): an SDR presenter, so PushFPS tone-maps (BT.2390) to SDR
        // RGB8, encoded for the SDR display's 2.2, with SDR white at MPC Video Renderer's own "display nits" (DisplayNits,
        // default 125) so both routes give the same brightness
        { std::lock_guard<std::mutex> lk(m_fmtLock); si.displayHdr = (!m_coreToneMap && IsHdrFmt(m_outFmt.fmt)) ? 1 : 0; }
        if (!si.displayHdr) {
            const int nits = MpcVrSdrNits();
            SetEnvironmentVariableW(L"VEYRA_HDR_SDR_WHITE_NITS", std::to_wstring(nits).c_str());
            Log(std::format("HDR to SDR in PushMPC: BT.2390 to SDR white {} cd/m2 (MPC Video Renderer's DisplayNits), gamma 2.2", nits));
        }
    } else {
        // SDR source in an HDR10 connection (SDR to HDR): pushcore presents HDR10 when RTX Video HDR is on
        { std::lock_guard<std::mutex> lk(m_fmtLock); si.displayHdr = IsHdrFmt(m_outFmt.fmt) ? 1 : 0; }
        if (si.matrix == 2) si.matrix = 1;
        if (si.primaries == 2) si.primaries = 1;
        if (si.transfer == 2) si.transfer = 1;
    }
    (void)first;
    m_openQpc = Qpc();
    Log(std::format("opening pushcore: {}x{} {}, {}/{} fps, range {} matrix {} primaries {} transfer {}{}; {}", si.width, si.height,
        m_inP010 ? "P010" : "NV12", si.fpsNum, si.fpsDen, si.range, si.matrix, si.primaries, si.transfer,
        hdr ? std::format(" (HDR: mastering peak {} MaxCLL {} MaxFALL {}, displayHdr {})", si.masteringPeakNits, si.maxCllNits, si.maxFallNits, si.displayHdr) : std::string(),
        Narrow(m_rt.Describe())));
    // PushMPC's settings, Auto resolved for this stream and the window's display (legacy: the PushFPS chain session)
    PushCoreSettings ps = {};
    const bool legacy = LegacyMode();
    if (!legacy) {
        EnsureGpu(m_gpuLuid);
        m_display = pm::QueryDisplay(pm::PlayerWindow(), m_cfg.user.vrr, m_gpu.nvidia());
        const pm::Choice c = DecideNow(m_cfg.user, m_display);
        ps = ToCore(m_cfg.user, c);
        { bool hdrFmt = false; { std::lock_guard<std::mutex> lk(m_fmtLock); hdrFmt = IsHdrFmt(m_outFmt.fmt); } SetItm(c, c.hdr && hdrFmt && !hdr); m_itmK = -1.0f;
          DXVA2_ExtendedFormat ex2; ex2.value = m_inCtrl;
          m_hlgOut = hdr && hdrFmt && m_inCtrl && ex2.VideoTransferFunction == kTransHLG;
          m_sceneCll = hdr && hdrFmt && !m_display.hdr;
          if (m_display.hdr) ConfigureMpcVr(); }
        {
            std::lock_guard<std::mutex> lk(m_autoLock);
            m_choice = c; m_applied = ps; m_appliedSettings = m_cfg.user; m_appliedDisplay = m_display; m_appliedFps = SourceInfo().fps;
            m_haveChoice = true;
            m_openFps = m_appliedFps;
        }
        PublishNotes(m_cfg.user, c);
        LogChoice("open", c);
    }
    // fast start: a quick-start graph first when the full one waits for slow NGX work (RTX VSR), see QuickStartWanted
    std::string quickWhy;
    const bool quick = !legacy && QuickStartWanted(ps, quickWhy);
    PushCoreSettings psQuick = ps;
    // the quick graph: Image Scaling for RTX VSR, no AI clean-up, and the cut guard without NVIDIA's optical flow (its
    // session costs ~85 ms at the open; the guard's pair test still runs); everything else as the full graph
    if (quick) { psQuick.srMethod = PUSHCORE_SR_METHOD_NIS; psQuick.nr = 0; if (psQuick.cutGuard & 2) psQuick.cutGuard &= ~2; }
    if (!legacy) Log(quick ? std::string("quick start: a quick graph (") + (m_cfg.quickVsr && (g_coreFeatures & PUSHCORE_FEATURE_QUICK_VSR) ? "the driver's RTX VSR through D3D11 if it answers, else NVIDIA Image Scaling" : "NVIDIA Image Scaling")
                               + " instead of NGX RTX VSR, the same frame generation) opens first; the full graph follows and takes over"
                           : "quick start: not used (" + quickWhy + ")");
    struct Ctx { std::shared_ptr<OpenJob> job; PushCoreStreamInfo si; ComPtr<ID3D11Device> dev; Config cfg; Runtime rt; HMODULE self; int64_t q0; PushCoreSettings ps; bool legacy; bool quick; PushCoreSettings psQuick; };
    auto* ctx = new Ctx{ std::make_shared<OpenJob>(), si, m_device, m_cfg, m_rt, nullptr, m_openQpc, ps, legacy, quick, psQuick };
    m_job = ctx->job;
    // a reference on this DLL for the thread's lifetime: an abandoned open may outlive the filter
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&Log), &ctx->self);
    auto proc = [](void* p) -> DWORD {
        HMODULE self = nullptr;
        {
            std::unique_ptr<Ctx> c(static_cast<Ctx*>(p));
            self = c->self;
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            std::wstring err;
            const PushCoreApi* api = LoadApi(c->rt, err);
            const std::wstring logFile = LogDir() + L"\\pushcore-engine-filter.log";
            auto openOne = [&](const PushCoreSettings* settings, uint32_t flags, std::string& openErr) -> PushCore* {
                if (!api) { openErr = Narrow(err); return nullptr; }
                PushCoreConfig pc = {};
                pc.structSize = sizeof(pc); pc.abiVersion = PUSHCORE_ABI_VERSION;
                pc.pushfpsDir = c->rt.root.c_str(); pc.logFile = logFile.c_str();
                pc.dataDir = c->legacy ? c->rt.dataDir.c_str() : L"";
                pc.shaderDir = c->rt.shaderDir.empty() ? nullptr : c->rt.shaderDir.c_str();
                pc.settings = c->legacy ? nullptr : settings;
                pc.sr = pc.fg = pc.multiplier = pc.vfgQuality = -1;
                pc.flags = flags | (c->cfg.warmCache ? 0 : PUSHCORE_FLAG_NO_WARM_CACHE);
                char eb[1024] = {};
                PushCore* core = SehOpen(api, &pc, &c->si, c->dev.Get(), eb, sizeof(eb));
                if (!core) openErr = eb[0] ? eb : "pushcore_open failed";
                return core;
            };
            // the quick graph (no warm NGX cache: it has no NGX stage, and the full graph keeps the process-wide one)
            PushCore* core = nullptr;
            std::string openErr;
            bool published = false;
            if (c->quick) {
                // Route B: the driver's RTX VSR through the D3D11 video processor where it answers (pushcore checks it)
                const uint32_t qf = c->cfg.quickVsr && (g_coreFeatures & PUSHCORE_FEATURE_QUICK_VSR) ? PUSHCORE_FLAG_QUICK_VSR : 0u;
                core = openOne(&c->psQuick, PUSHCORE_FLAG_NO_WARM_CACHE | qf, openErr);
                if (core && qf && api->stats) {
                    PushCoreStats qs = {}; qs.structSize = sizeof(qs);
                    if (api->stats(core, &qs) == PUSHCORE_OK) Log(std::string("quick start: the quick graph upscales with ") + (qs.quickVsr ? "the driver's RTX VSR (D3D11 video processor)" : "NVIDIA Image Scaling (the driver's RTX VSR did not answer; see quick-vsr in the engine log)"));
                }
                const double ms = QpcMs(Qpc() - c->q0);
                bool abandoned = false;
                if (core) {
                    std::lock_guard<std::mutex> lk(c->job->lock);
                    abandoned = c->job->abandoned;
                    if (!abandoned) { c->job->api = api; c->job->core = core; c->job->ms = ms; c->job->quick = true; published = true; }
                    c->job->done = !abandoned;
                }
                if (abandoned) { SehClose(api, core); core = nullptr; }
                Log(std::format("quick start: quick graph open {} after {:.0f} ms{}", published ? "OK" : "FAILED", ms,
                    published ? "" : abandoned ? " (abandoned: the filter closed meanwhile)" : ": " + openErr + "; the full graph opens next"));
                if (abandoned) {
                    std::lock_guard<std::mutex> lk(c->job->lock);
                    c->job->done = true;
                }
                core = nullptr;
            }
            // the quick graph's first frames run cold: the full graph's allocations wait a moment (FullOpenDelayMs)
            if (published && c->cfg.fullOpenDelayMs > 0) Sleep(DWORD(c->cfg.fullOpenDelayMs));
            bool stop = false;   // the filter closed meanwhile: the full graph is not opened at all
            { std::lock_guard<std::mutex> lk(c->job->lock); stop = c->job->abandoned; }
            if (!stop) {
                openErr.clear();
                core = openOne(&c->ps, 0, openErr);
                const double ms = QpcMs(Qpc() - c->q0);
                bool abandoned = false;
                {
                    std::lock_guard<std::mutex> lk(c->job->lock);
                    abandoned = c->job->abandoned;
                    if (published) {
                        if (!abandoned) { c->job->next = core; c->job->nextErr = openErr; c->job->nextMs = ms; }
                        c->job->nextDone = true;
                    } else {
                        if (!abandoned) { c->job->api = api; c->job->core = core; c->job->err = openErr; c->job->ms = ms; }
                        c->job->done = true;
                    }
                }
                if (abandoned && core) SehClose(api, core);
                Log(std::format("pushcore open {} after {:.0f} ms{}{}{}", core ? "OK" : "FAILED", ms, published ? " (the full graph)" : "", openErr.empty() ? "" : ": " + openErr,
                    abandoned ? " (abandoned: the filter closed meanwhile; this instance was closed by the open thread)" : ""));
            } else {
                std::lock_guard<std::mutex> lk(c->job->lock);
                c->job->done = c->job->nextDone = true;
            }
            if (api) ArmExitHelper(); // pushcore_open loads the NGX/CUDA DLLs (also when abandoned or failed); the helper must be initialised after them
            c->dev.Reset();
            CoUninitialize();
        }
        if (self) FreeLibraryAndExitThread(self, 0);
        return 0;
    };
    HANDLE th = CreateThread(nullptr, 0, proc, ctx, 0, nullptr);
    if (th) CloseHandle(th);
    else { Log("CreateThread for the open failed"); if (ctx->self) FreeLibrary(ctx->self); std::lock_guard<std::mutex> lk(m_job->lock); m_job->done = true; m_job->err = "no open thread"; delete ctx; }
}
bool CPushMpcFilter::OpenFinished()
{
    if (!m_job) return false;
    std::lock_guard<std::mutex> lk(m_job->lock);
    if (!m_job->done) return false;
    std::lock_guard<std::mutex> l2(m_coreLock);
    m_api = m_job->api; m_core = m_job->core; m_openError = m_job->err;
    m_job->core = nullptr;
    m_quickPhase = m_core && m_job->quick;   // the full graph follows (TryHandover); it may be ready already
    if (m_quickPhase) m_quickQpc = Qpc();
    return true;
}
// Fast start: a quick-start graph is worth it when the full open is slow because of NGX (RTX VSR / AI clean-up start the
// NGX core: ~2 s, measured 1.8-2.3 s on an RTX 5090, most of it NGX loading and checking every feature it knows) and the
// frame generation itself starts fast without NGX (NVIDIA VFG ~0.4 s, FSR / NVOF). The quick graph is the same graph with
// NVIDIA Image Scaling in place of RTX VSR (same output size, same frame generation and cut guard), so the motion is
// smooth from the first frames; the full graph opens right after it on the same thread and takes over seamlessly.
bool CPushMpcFilter::QuickStartWanted(const PushCoreSettings& ps, std::string& why)
{
    if (!m_cfg.quickStart) { why = "switched off (QuickStart 0)"; return false; }
    if (!(g_coreFeatures & PUSHCORE_FEATURE_FG_METHODS)) { why = "this pushcore.dll has no upscaler choice"; return false; }
    if (!m_gpu.nvidia()) { why = "not an NVIDIA GPU: the graph has no NGX stage"; return false; }
    if (ps.multiplier < 2) { why = "no frame generation"; return false; }
    if (ps.fgBackend != PUSHCORE_FG_VFG && ps.fgBackend != PUSHCORE_FG_NVOF && ps.fgBackend != PUSHCORE_FG_FSR) { why = "the frame generation method needs NGX or starts slowly itself"; return false; }
    if (ps.videoHdr) { why = "RTX Video HDR sets the output signal"; return false; }
    const bool ngx = (ps.sr && ps.srMethod == PUSHCORE_SR_METHOD_RTX) || ps.nr;
    if (!ngx) { why = "the full graph has no NGX stage: it opens fast"; return false; }
    if (m_cfg.warmCache) {
        std::lock_guard<std::mutex> lk(g_ngxLock);
        if (g_haveNgx && g_lastNgx == NgxKeyOf(ps)) { why = "the NGX core of the last file is still warm: the full graph opens fast"; return false; }
    }
    // two graphs live for a moment: room for both on the GPU (estimate: the graph's pools at the output size plus the
    // frame generation buffers and the SDK contexts; 1.7 GB + 0.5 GB + ~0.4 GB at 3840x2080 8x, measured)
    ComPtr<IDXGIDevice> dd; ComPtr<IDXGIAdapter> a; ComPtr<IDXGIAdapter3> a3;
    DXGI_QUERY_VIDEO_MEMORY_INFO mi = {};
    if (m_device && SUCCEEDED(m_device.As(&dd)) && SUCCEEDED(dd->GetAdapter(&a)) && SUCCEEDED(a.As(&a3)) && SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mi))) {
        UINT w = 0, h = 0; { std::lock_guard<std::mutex> lk(m_fmtLock); w = m_outFmt.w; h = m_outFmt.h; }
        const double frame = double(std::max(w, 640u)) * std::max(h, 360u) * 4.0;
        const double graph = frame * (200.0 + 4.0 * ps.multiplier) + 600.0 * 1048576.0;
        const double room = mi.Budget > mi.CurrentUsage ? double(mi.Budget - mi.CurrentUsage) : 0.0;
        if (room < 2.0 * graph + 512.0 * 1048576.0) { why = std::format("not enough free video memory for two graphs ({:.1f} GB free, {:.1f} GB needed)", room / 1073741824.0, (2.0 * graph + 512.0 * 1048576.0) / 1073741824.0); return false; }
    }
    return true;
}
// Fast start handover (Receive, under m_coreLock, right after source frame pts went into the quick graph): when the full
// graph is ready it gets the SAME source frame as its first one (it then holds it as history; its own output for it, the
// real frame alone, is dropped), the quick graph still delivers the in-between frames up to and including that real
// frame, and from the next source frame on the full graph generates. No gap, no repeated or missing frame, the
// timestamps never change; only the upscaler changes (Image Scaling -> RTX VSR).
void CPushMpcFilter::TryHandover(REFERENCE_TIME pts, const PushCoreSubmitTiming& t)
{
    if (!m_quickPhase || m_oldCore || !m_job) return;
    PushCore* next = nullptr; std::string err; double ms = 0;
    {
        std::lock_guard<std::mutex> lk(m_job->lock);
        if (!m_job->nextDone) return;
        next = m_job->next; err = m_job->nextErr; ms = m_job->nextMs;
        m_job->next = nullptr;
    }
    if (!next) { m_quickPhase = false; Log("quick start: the full graph did not open (" + err + "): the quick graph stays"); return; }
    PushCoreSubmitTiming tt = t;
    const int r = m_api->submit2(next, m_upload.Get(), 0, pts, PUSHCORE_SUBMIT_DISCONTINUITY, &tt);
    if (r != PUSHCORE_OK) {
        Log(std::format("quick start: the full graph refused its first frame ({} {}): the quick graph stays", r, m_api->lastError(next)));
        CloseAsync(m_api, next, "the full graph");
        m_quickPhase = false;
        return;
    }
    m_oldCore = m_core; m_core = next;
    m_handoverPts = pts; m_discardUpTo = pts;
    const PushCoreOutputInfo before = m_oi;
    m_oi = {};
    m_api->outputInfo(m_core, &m_oi);
    m_mult = m_oi.multiplier ? m_oi.multiplier : 1;
    Log(std::format("quick start: handover at pts {:.3f} s, {:.0f} ms after the quick graph took over ({:.0f} ms after the open started): {} (output {}x{}, x{}; quick graph {}x{} x{})",
        pts / 1e7, QpcMs(Qpc() - m_quickQpc), ms, m_oi.description, m_oi.width, m_oi.height, m_oi.multiplier, before.width, before.height, before.multiplier));
    SetStatus(L"active: " + Widen(m_oi.description));
    m_sizeAfterHandover = m_oi.width != before.width || m_oi.height != before.height;
}
// the quick graph after its last frame (the copier) or at a flush: closed on its own thread; Auto starts measuring now
void CPushMpcFilter::RetireOld(const char* why)
{
    if (!m_oldCore) return;
    PushCore* old = m_oldCore;
    m_oldCore = nullptr;   // m_discardUpTo stays: the full graph's copy of the handover frame may still come
    { std::lock_guard<std::mutex> sl(m_srvLock); m_srvs.clear(); }
    // kept, idle, until a quiet moment (pause, seek, stop) or until video memory runs short: tearing a VFG graph down
    // while another one plays stalls the GPU for one or two frames at 8x (CUDA frees wait for the device)
    if (m_retired) CloseAsync(m_api, m_retired, "the quick graph");
    m_retired = old;
    m_quickPhase = false;
    { std::lock_guard<std::mutex> al(m_autoLock); m_autoTimings.clear(); m_graphGen++; m_graphQpc = Qpc(); }
    m_admGrace = 8;
    Log(std::format("quick start: the quick graph retired ({}), {:.0f} ms after it took over", why, QpcMs(Qpc() - m_quickQpc)));
}
void CPushMpcFilter::DrainCore(bool flush)
{
    std::lock_guard<std::mutex> lk(m_coreLock);
    if (m_oldCore) {
        // a flush during the handover: the quick graph's remaining frames go, and so does the quick graph
        PushCoreFrame of[16]; uint32_t on = 0;
        while (m_api->poll(m_oldCore, of, 16, &on, 0) == PUSHCORE_OK && on) { for (uint32_t i = 0; i < on; i++) m_api->presented(m_oldCore, of[i].lease, nullptr); on = 0; }
        RetireOld("flush");
    }
    m_discardUpTo = INT64_MIN;   // a flush: nothing of the full graph is held back any more (the next pts may be earlier)
    m_waitOutQpc = 0;
    CloseRetired("seek");
    if (!m_core || m_faulted) return;
    PushCoreFrame fr[16]; uint32_t n = 0;
    int dropped = 0;
    while (m_api->poll(m_core, fr, 16, &n, 0) == PUSHCORE_OK && n) { for (uint32_t i = 0; i < n; i++) m_api->presented(m_core, fr[i].lease, nullptr); dropped += n; n = 0; }
    if (flush) {
        const int r = m_api->flush(m_core);
        if (r != PUSHCORE_OK) Log(std::format("pushcore flush -> {} {}", r, m_api->lastError(m_core)));
    }
    if (dropped) Log(std::format("drained {} pushcore frames", dropped));
}
void CPushMpcFilter::CloseCore()
{
    bool quickPending = false;
    if (m_job) {
        std::lock_guard<std::mutex> lk(m_job->lock);
        if (!m_job->done) {
            m_job->abandoned = true; // never wait for an open: its thread closes what it opened
            Log(std::format("pushcore open still running ({:.0f} ms): abandoned, the close does not wait for it", QpcMs(Qpc() - m_openQpc)));
        } else if (m_job->core) {
            std::lock_guard<std::mutex> l2(m_coreLock);
            if (!m_core) { m_api = m_job->api; m_core = m_job->core; }
            m_job->core = nullptr;
        }
        if (m_job->done && m_job->quick) {
            // fast start: the full graph still opening is closed by its thread; one that opened but never took over, here
            if (!m_job->nextDone) { m_job->abandoned = true; Log("quick start: the full graph is still opening: abandoned, its thread closes it"); }
            else if (m_job->next) { CloseAsync(m_job->api, m_job->next, "the full graph (never took over)"); m_job->next = nullptr; }
            quickPending = !m_job->nextDone;
        }
    }
    m_job.reset();
    {
        std::lock_guard<std::mutex> lk(m_coreLock);
        if (m_oldCore) { PushCore* o = m_oldCore; m_oldCore = nullptr; CloseAsync(m_api, o, "the quick graph"); }
        CloseRetired("the file closes");
        m_quickPhase = false; m_discardUpTo = INT64_MIN;
        // the quick graph while the full one still builds: pushcore_close would wait for that build (one graph build at a
        // time in a process), so it closes on its own thread and the player never waits for it
        if (quickPending && m_core) { CloseAsync(m_api, m_core, "the quick graph"); m_core = nullptr; }
    }
    if (!LegacyMode() && m_cfg.warmCache && m_haveChoice) {
        bool have = false;
        { std::lock_guard<std::mutex> lk(m_coreLock); have = m_core != nullptr; }
        if (have) {   // F8 (round 2): this graph's NGX key is the one the warm core keeps after the close
            PushCoreSettings a; { std::lock_guard<std::mutex> al(m_autoLock); a = m_applied; }
            std::lock_guard<std::mutex> nl(g_ngxLock); g_lastNgx = NgxKeyOf(a); g_haveNgx = true;
        }
    }
    std::lock_guard<std::mutex> lk(m_coreLock);
    if (m_core) { const int64_t q0 = Qpc(); SehClose(m_api, m_core); m_core = nullptr; Log(std::format("pushcore closed in {:.0f} ms", QpcMs(Qpc() - q0))); }
    { std::lock_guard<std::mutex> l2(m_srvLock); m_srvs.clear(); }
}
// Changes the output SIZE for the next samples (dynamic media type change on the sample), never the format: the format
// is chosen at connect time (a runtime switch from BGRA to NV12 D3D11 samples crashed stock MPC-VR 2638). The
// downstream pin is asked first (QueryAccept); refused: frames are drawn scaled into the current size. In MPC-VR a
// same-format size change goes PrepareReceive -> SetMediaType -> InitMediaType (source texture / VP re-created for the
// new size), then CopySample copies the new size from our texture (BGRA: CopySubresourceRegion on the shader path;
// Y410/P010: its D3D11 VP input view on our texture).
void CPushMpcFilter::RequestOutFmt(const OutFmt& want, const char* why)
{
    OutFmt cur;
    { std::lock_guard<std::mutex> lk(m_fmtLock); cur = m_outFmt; }
    if (want == cur) return;
    if (want.fmt != cur.fmt) { Log(std::format("output change to {} not made (only the size changes at run time): kept {}", want.Str(), cur.Str())); return; }
    CMediaType mt;
    BuildMediaType(want, &mt);
    IPin* peer = m_pOutput->GetConnected();
    const HRESULT hr = peer ? peer->QueryAccept(&mt) : E_FAIL;
    Log(std::format("output change {} -> {} ({}): downstream QueryAccept -> 0x{:08X}", cur.Str(), want.Str(), why, uint32_t(hr)));
    if (hr == S_OK) {
        std::lock_guard<std::mutex> lk(m_fmtLock);
        m_outFmt = want;
        return;
    }
    if (want.fmt == cur.fmt) Log("output size kept: frames are scaled into the connected size");
}
bool CPushMpcFilter::TryActivate(REFERENCE_TIME pts)
{
    std::lock_guard<std::mutex> lk(m_coreLock);
    if (!m_core) return false;
    m_oi = {};
    m_api->outputInfo(m_core, &m_oi);
    m_mult = m_oi.multiplier ? m_oi.multiplier : 1;
    m_activeQpc = Qpc();
    { std::lock_guard<std::mutex> ol(m_osdLock); m_osdText.clear(); }   // the status thread composes this session's line
    OsdBrief();
    { std::lock_guard<std::mutex> al(m_autoLock); m_graphQpc = m_activeQpc; m_autoTimings.clear(); }
    m_coreDisc = true;
    Log(std::format("pushcore ACTIVE{} at input frame {} (pts {:.3f} s) {:.0f} ms after the open started: {} (output {}x{} format {}, x{}, HDR in {} out {})",
        m_quickPhase ? " (quick graph; the full graph follows)" : "", m_inFrames, pts / 1e7, QpcMs(m_activeQpc - m_openQpc), m_oi.description, m_oi.width, m_oi.height, m_oi.dxgiFormat, m_oi.multiplier, m_oi.hdrInput, m_oi.hdrOutput));
    SetStatus(L"active: " + Widen(m_oi.description));
    return true;
}
void CPushMpcFilter::Fallback(const std::wstring& why)
{
    if (m_mode == Mode::PassD3D) return;
    // The established output type is kept: the source frames are converted into it on our device (as during the
    // warm-up). A live switch to NV12 D3D11 samples is NOT done: stock MPC-VR 2638 crashed on it (access violation in
    // d3d11.dll under its D3D11 VP input setup, 2026-11-22 19:46, pid 71616), although its QueryAccept said S_OK.
    LogW(L"runtime fallback: no PushFPS, the source frames are converted into the current output type: " + why);
    m_mode = Mode::PassD3D;
    SetStatus(L"no PushFPS (source frames converted by the filter, at the source rate): " + why);
}
bool CPushMpcFilter::DeviceGone()
{
    return m_device && FAILED(m_device->GetDeviceRemovedReason());
}
// where: 1 = pushcore reported it, 2 = our device (shared with MPC-VR) is gone
void CPushMpcFilter::DeviceLost(int where, const std::string& what)
{
    if (m_deviceLostSeen.exchange(true)) return;
    const bool gone = where == 2 || DeviceGone();
    Log(std::format("device lost ({}); the shared D3D11 device {}", what, gone ? "is gone: MPC-HC is asked to reopen the file" : "still works: source format"));
    m_faulted = true;
    {
        std::lock_guard<std::mutex> lk(m_coreLock);
        if (m_oldCore) { SehClose(m_api, m_oldCore); m_oldCore = nullptr; }
        if (m_retired) { SehClose(m_api, m_retired); m_retired = nullptr; }
        m_quickPhase = false;
        if (m_core) { SehClose(m_api, m_core); m_core = nullptr; }
    }
    if (gone) {
        SetStatus(L"stopped: the GPU device was lost; the file is reopened");
        if (!(m_cfg.reopenOnLost && RequestReopen())) NotifyEvent(EC_ERRORABORT, DXGI_ERROR_DEVICE_REMOVED, 0);
    } else {
        Fallback(L"GPU device lost in pushcore (" + Widen(what) + L")");
    }
}
bool CPushMpcFilter::RequestReopen()
{
    static std::mutex s_lock;
    static std::deque<ULONGLONG> s_times;
    {
        std::lock_guard<std::mutex> lk(s_lock);
        const ULONGLONG now = GetTickCount64();
        while (!s_times.empty() && now - s_times.front() > 120000) s_times.pop_front();
        if (s_times.size() >= 2) { Log("not reopening again (2 reopens in the last 2 minutes)"); return false; }
        s_times.push_back(now);
    }
    const std::string host = Narrow(pm::HostName());
    if (!pm::HostCanReopen(pm::HostPlayer())) { Log(std::format("{} has no known reopen command: the file is not reopened", host)); return false; }
    const HWND wnd = pm::PlayerWindow();
    if (!wnd) { Log(std::format("no {} main window found to reopen the file", host)); return false; }
    const BOOL ok = PostMessageW(wnd, WM_COMMAND, 976 /* ID_FILE_REOPEN (MPC-HC and MPC-BE) */, 0);
    Log(std::format("asked {} to reopen the file (WM_COMMAND 976: {})", host, ok ? "posted" : "failed"));
    return !!ok;
}

// ------------------------------------------------------------------------------------------------ Receive (streaming thread)
HRESULT CPushMpcFilter::PassSysmem(IMediaSample* pIn)
{
    IMediaSample* out = nullptr;
    REFERENCE_TIME t0 = 0, t1 = 0;
    const bool timed = SUCCEEDED(pIn->GetTime(&t0, &t1));
    HRESULT hr = m_pOutput->GetDeliveryBuffer(&out, timed ? &t0 : nullptr, timed ? &t1 : nullptr, 0);
    if (FAILED(hr) || !out) return S_OK; // stopping or flushing
    AM_MEDIA_TYPE* omt = nullptr;
    if (out->GetMediaType(&omt) == S_OK && omt) {
        // the renderer asked for another stride (MPC-VR's CalcImageSize / QueryAccept)
        BITMAPINFOHEADER b; REFERENCE_TIME a; DWORD c, il, ax, ay;
        if (ParseVideo(omt, b, a, c, il, ax, ay)) {
            m_outStride = UINT(b.biWidth) * (m_inP010 ? 2 : 1);
            Out()->SetMt(CMediaType(*omt));
            Log(std::format("pass-through: downstream stride now {} bytes", m_outStride));
        }
        DeleteMediaType(omt);
    }
    BYTE *src = nullptr, *dst = nullptr;
    pIn->GetPointer(&src); out->GetPointer(&dst);
    const UINT rows = m_inH * 3 / 2, rowBytes = m_inW * (m_inP010 ? 2 : 1);
    const long need = long(m_outStride) * long(rows);
    if (src && dst && out->GetSize() >= need && pIn->GetActualDataLength() >= long(m_inStride * rows)) {
        if (m_outStride == m_inStride) memcpy(dst, src, size_t(m_inStride) * rows);
        else for (UINT y = 0; y < rows; y++) memcpy(dst + size_t(y) * m_outStride, src + size_t(y) * m_inStride, std::min(rowBytes, m_outStride));
        out->SetActualDataLength(need);
    }
    if (m_passMtPending) {
        // the upstream type changed (colour description): passed on, with the stride the downstream buffers have
        m_passMtPending = false;
        CMediaType mt(m_inMt);
        BITMAPINFOHEADER* bih = mt.formattype == FORMAT_VideoInfo2 ? &((VIDEOINFOHEADER2*)mt.pbFormat)->bmiHeader
            : mt.formattype == FORMAT_VideoInfo ? &((VIDEOINFOHEADER*)mt.pbFormat)->bmiHeader : nullptr;
        if (bih) bih->biWidth = LONG(m_outStride / (m_inP010 ? 2 : 1));
        IPin* peer = m_pOutput->GetConnected();
        const HRESULT qa = peer ? peer->QueryAccept(&mt) : E_FAIL;
        Log(std::format("pass-through: upstream type change passed on: downstream QueryAccept -> 0x{:08X}", uint32_t(qa)));
        if (qa == S_OK) { out->SetMediaType(&mt); Out()->SetMt(mt); }
    }
    if (timed) out->SetTime(&t0, &t1); else out->SetTime(nullptr, nullptr);
    REFERENCE_TIME m0, m1;
    if (SUCCEEDED(pIn->GetMediaTime(&m0, &m1))) out->SetMediaTime(&m0, &m1);
    out->SetSyncPoint(pIn->IsSyncPoint() == S_OK);
    out->SetDiscontinuity(pIn->IsDiscontinuity() == S_OK);
    out->SetPreroll(pIn->IsPreroll() == S_OK);
    // side data (HDR, Dolby Vision) goes along when the downstream sample can hold it
    ComPtr<IMediaSideData> sIn, sOut;
    if (SUCCEEDED(pIn->QueryInterface(__uuidof(IMediaSideData), (void**)&sIn)) && SUCCEEDED(out->QueryInterface(__uuidof(IMediaSideData), (void**)&sOut))) {
        for (const GUID* g : kSideGuids) { const BYTE* p = nullptr; size_t n = 0; if (SUCCEEDED(sIn->GetSideData(*g, &p, &n)) && p && n) sOut->SetSideData(*g, p, n); }
    }
    hr = m_pOutput->Deliver(out);
    out->Release();
    m_delivered++;
    return hr;
}

HRESULT CPushMpcFilter::Receive(IMediaSample* pIn)
{
    ++m_segSamples;
    if (m_flushing || m_stopReq) return S_OK;
    AM_MEDIA_TYPE* pmt = nullptr;
    if (pIn->GetMediaType(&pmt) == S_OK && pmt) {
        // LAV attaches its final type to the first sample: the stride, and the colour description it found in the
        // bitstream (HDR10/HLG transfer, BT.2020)
        BITMAPINFOHEADER bih; REFERENCE_TIME a; DWORD c, il, ax, ay; RECT rs;
        if (ParseVideo(pmt, bih, a, c, il, ax, ay, &rs) && bih.biWidth > 0) {
            m_inStride = UINT(bih.biWidth) * (m_inP010 ? 2 : 1);
            const DWORD ctrl = (c & AMCONTROL_COLORINFO_PRESENT) ? c : 0;
            const bool wasHdr = InputHdr();
            m_inMt = *pmt;
            if (a > 0) m_atpf = a;
            if (ax && ay) { m_inArX = ax; m_inArY = ay; }
            DXVA2_ExtendedFormat ex; ex.value = ctrl;
            Log(std::format("input sample media type change: stride {}, colour {} (range {} matrix {} primaries {} transfer {})", m_inStride,
                ctrl ? "present" : "absent", (int)ex.NominalRange, (int)ex.VideoTransferMatrix, (int)ex.VideoPrimaries, (int)ex.VideoTransferFunction));
            if (ctrl != m_inCtrl) {
                m_inCtrl = ctrl;
                if (m_mode == Mode::PassSys) m_passMtPending = true;
                if (m_mode == Mode::Warmup && m_openStarted && wasHdr != InputHdr()) {
                    // an open started from the connection's type (no colour info) is for the wrong signal: abandoned,
                    // and opened again from this type (HDR waits for the side data below)
                    CloseCore();
                    m_openStarted = false;
                    Log("the early open was for an SDR stream: abandoned, opened again for the new colour description");
                }
                if (m_mode == Mode::Warmup || m_mode == Mode::Active) {
                    // the output format stays the connected one (no runtime format change into MPC-VR): an HDR stream
                    // in an SDR connection is tone-mapped by PushFPS, an SDR stream in an HDR10 one is put into PQ BT.2020
                    OutFmt cur;
                    { std::lock_guard<std::mutex> lk(m_fmtLock); cur = m_outFmt; }
                    if (InputHdr() != IsHdrFmt(cur.fmt))
                        Log(std::format("the stream is {} but the connection is {}: kept; {}", InputHdr() ? "HDR" : "SDR", cur.Str(),
                            InputHdr() ? "PushFPS tone-maps it to SDR" : "the SDR picture is carried as PQ BT.2020 (SDR white 203 cd/m2)"));
                }
                if (m_mode == Mode::Warmup && !InputHdr()) StartOpen(nullptr);
            }
        }
        DeleteMediaType(pmt);
    }
    // the input samples are recycled by LAV: their side data is cleared once this frame is done
    struct ClearSideOnExit {
        IMediaSample* s;
        ~ClearSideOnExit() { ComPtr<IPushMpcSideClear> c; if (SUCCEEDED(s->QueryInterface(__uuidof(IPushMpcSideClear), (void**)&c))) c->ClearSide(); }
    } clearSide{ pIn };
    m_inFrames++;
    if (m_mode == Mode::PassSys) return PassSysmem(pIn);

    REFERENCE_TIME rtStart = 0, rtStop = 0;
    const bool timed = SUCCEEDED(pIn->GetTime(&rtStart, &rtStop));
    if (!timed) rtStart = m_lastInPts + m_atpf;
    m_lastInPts = rtStart;
    if (timed) TrackPts(rtStart, pIn->IsDiscontinuity() == S_OK);
    if (m_openDeferred && !m_openStarted && m_mode == Mode::Warmup && (m_baseStepMs.load() > 0 || QpcMs(Qpc() - m_deferQpc) >= 700.0)) {
        Log(std::format("deferred open starts {:.0f} ms later: {}", QpcMs(Qpc() - m_deferQpc),
            m_baseStepMs.load() > 0 ? std::format("{:.3f} fps measured", SourceInfo().fps) : std::string("no timestamps yet (700 ms), the media type's rate is used")));
        StartOpen(nullptr);
    }
    if (!m_ilDecided) CheckInterlace(pIn);
    MeasureContent(pIn, rtStart, pIn->IsDiscontinuity() == S_OK);

    if (!m_sideChecked) {
        m_sideChecked = true;
        SideData all; all.CopyFrom(pIn, true);
        m_hdrSide.CopyFrom(pIn, false);
        Log(std::format("first sample: side data {}{}{}{}", all.m.empty() ? "none" : "", all.m.count(IID_MediaSideDataHDR) ? "HDR mastering " : "",
            all.m.count(IID_MediaSideDataHDR10Plus) ? "HDR10+ " : "", all.m.count(IID_MediaSideDataDOVIMetadataV2) ? "Dolby Vision RPU" : ""));
        if (all.m.count(IID_MediaSideDataDOVIMetadataV2) || all.m.count(IID_MediaSideDataDOVIRPU)) {
            if (m_dvP5 || !InputHdr()) {
                // Profile 5 (IPT-PQ-C2 base, no HDR10 picture): converted per frame by pushcore from the RPU, as
                // PushFPS's own player does, then processed as HDR10 into the connection made at connect time.
                if (!m_dvP5 && CoreSupportsDvP5()) {
                    m_dvP5 = true;
                    const bool restart = m_openStarted;
                    SetDvP5Colour();
                    if (restart) { CloseCore(); m_openStarted = false; }
                    Log(std::format("Dolby Vision profile 5 found at the first sample: converted by pushcore into the connected {}{}", m_outFmt.Str(),
                        restart ? "; the early open (SDR) was abandoned" : ""));
                } else if (!m_dvP5) {
                    Fallback(L"Dolby Vision profile 5 found at the first sample and this pushcore.dll has no profile 5 conversion: "
                             L"shown converted as plain YCbCr (wrong colours)");
                }
                if (m_dvP5) {
                    SideData p5 = all;
                    ToP5Output(p5);
                    if (!m_hdrSide.m.count(IID_MediaSideDataHDR) && p5.m.count(IID_MediaSideDataHDR)) {
                        m_hdrSide.m[IID_MediaSideDataHDR] = p5.m[IID_MediaSideDataHDR];
                        if (p5.m.count(IID_MediaSideDataHDRContentLightLevel)) m_hdrSide.m[IID_MediaSideDataHDRContentLightLevel] = p5.m[IID_MediaSideDataHDRContentLightLevel];
                        Log("HDR10 static metadata taken from the RPU's level 6");
                    }
                }
            } else {
                // Profile 8.1 / 7 (HDR10 base layer): processed as HDR10; the RPU goes on every output sample (a
                // generated frame carries its real frame's), so MPC-VR's own Dolby Vision handling (reshaping, L1/L2
                // trims, with "prefer Dolby Vision") applies to the processed frames as it would to the decoded ones;
                // without "prefer Dolby Vision" MPC-VR plays the HDR10 base with its static metadata, as stock does.
                Log("Dolby Vision RPU with an HDR10 base layer: processed as HDR10, the RPU is passed on every sample");
            }
        }
        if (m_mode == Mode::Warmup && InputHdr()) StartOpen(pIn);
    } else {
        // HDR10 static metadata may arrive later or change: the latest goes on the processed samples
        SideData hs; hs.CopyFrom(pIn, false);
        if (!hs.m.empty() && hs.m != m_hdrSide.m) { m_hdrSide = hs; Log("HDR side data updated from the source"); }
    }
    if (m_dvP5) {
        // this frame's conversion (pushcore gets it with the submit; the warm-up shaders use it too)
        m_dvCur = DvOf(pIn);
        if ((!m_dvCur.active || !m_dvCur.usable) && m_dvLastGood.active && m_dvLastGood.usable) {
            // hold the last good conversion (a scene with an MMR chroma mapping, a broken RPU) instead of skipping frames
            if (m_dvUnusable++ < 3 || m_dvUnusable % 100 == 0)
                Log(std::format("Dolby Vision profile 5: frame without a usable RPU conversion ({}): the last good conversion is held ({} in a row)",
                    !m_dvCur.active ? "no colour block" : m_dvCur.unsupported ? "unsupported mapping (MMR)" : "unusable curve", m_dvUnusable));
            m_dvCur = m_dvLastGood;
        } else if (!m_dvCur.active || !m_dvCur.usable) {
            // none seen yet: PushFPS's player refuses such a frame rather than showing it unconverted: the frame is skipped
            if (m_dvUnusable++ < 3 || m_dvUnusable % 100 == 0)
                Log(std::format("Dolby Vision profile 5: frame without a usable RPU conversion ({}): skipped ({} in a row)",
                    !m_dvCur.active ? "no colour block" : m_dvCur.unsupported ? "unsupported mapping (MMR)" : "unusable curve", m_dvUnusable));
            if (m_dvUnusable >= 48 && m_mode != Mode::PassD3D) Fallback(L"Dolby Vision profile 5 RPU cannot be converted (48 frames in a row)");
            if (m_mode != Mode::PassD3D) return S_OK;
        } else {
            m_dvUnusable = 0;
            m_dvLastGood = m_dvCur;
        }
    }

    HRESULT hr = Upload(pIn);
    if (FAILED(hr)) return S_OK;

    if (m_cfg.simulateLostSec && m_mode == Mode::Active && !m_deviceLostSeen && QpcMs(Qpc() - m_activeQpc) >= m_cfg.simulateLostSec * 1000.0) {
        DeviceLost(m_cfg.simulateLostMode == 2 ? 2 : 1, "simulated");
    }
    if (m_mode == Mode::Warmup && m_openStarted && m_cfg.paceWarmup) {
        // fast start: while the graph opens, the source frames are taken just ahead of the clock (LAV would decode ~0.4 s
        // ahead into our queue): the first generated frames then show right after the open, not after the queued frames
        for (int i = 0; i < 100 && !m_flushing && !m_stopReq && !m_quit; ++i) {
            REFERENCE_TIME now = 0;
            if (!StreamNow(now) || rtStart - now <= 2 * m_atpf) break;
            if (!m_job) break;
            { std::lock_guard<std::mutex> lk(m_job->lock); if (m_job->done) break; }
            Sleep(4);
        }
    }
    if (m_mode == Mode::Warmup && m_reopenForRate && OpenFinished()) {
        // F8: the open was decided on the media type's frame rate and the timestamps disagree: closed and opened again
        // for the measured rate (waiting for the first open keeps the second one warm: ~1 s instead of a cold ~3 s)
        m_reopenForRate = false;
        const int64_t q0 = Qpc();
        CloseCore();
        m_openStarted = false;
        StartOpen(nullptr);
        Log(std::format("reopened for the measured frame rate ({:.0f} ms to close the first open)", QpcMs(Qpc() - q0)));
    } else if (m_mode == Mode::Warmup && OpenFinished()) {
        if (TryActivate(rtStart)) {
            m_mode = Mode::Active;
            m_admGrace = 8; // 2 s: the first GPU batches after activation run cold (one measured 47 ms against a 38 ms budget)
            OutFmt cur;
            { std::lock_guard<std::mutex> lk(m_fmtLock); cur = m_outFmt; }
            RequestOutFmt({ cur.fmt, m_oi.width, m_oi.height }, "pushcore output size"); // the size only, never the format
            if (bool(m_oi.hdrOutput) != IsHdrFmt(cur.fmt)) Log(std::format("pushcore output is {} into {}: converted", m_oi.hdrOutput ? "HDR10" : "SDR", cur.Str()));
            if (m_cfg.watch && LegacyMode()) StartWatcher();
            StartAuto();
        } else {
            std::string e;
            { std::lock_guard<std::mutex> lk(m_coreLock); e = m_openError; }
            Fallback(L"pushcore open failed: " + Widen(e));
        }
    }
    if (m_mode == Mode::Active && !m_reloading) {
        if (DeviceGone()) { DeviceLost(2, "D3D11 device removed"); return S_OK; }
        // back-pressure: the graph accepts a source frame once the outputs of the one two before are returned
        for (;;) {
            if (m_flushing || m_stopReq || m_quit || m_mode != Mode::Active) return S_OK;
            if (m_reloading) break;
            int can = 0;
            {
                // try-lock: a reload holds the core lock for seconds, and the source frames are shown meanwhile
                std::unique_lock<std::mutex> lk(m_coreLock, std::try_to_lock);
                if (!lk.owns_lock()) { WaitForSingleObject(m_hLease, 1); continue; }
                if (!m_core) return S_OK;
                can = m_api->canSubmit(m_core);
            }
            if (can) break;
            WaitForSingleObject(m_hLease, 5);
        }
        int submitErr = PUSHCORE_OK;
        std::string submitErrText;
        if (!m_reloading) {
            // catch-up: a source frame more than one frame late is not processed (presentation back on the clock at once)
            REFERENCE_TIME now = 0;
            if (StreamNow(now) && rtStart + m_atpf < now) {
                if (m_skippedIngest++ < 3 || m_skippedIngest % 100 == 0)
                    Log(std::format("catch-up: source frame {:.3f} s skipped, {:.1f} ms late ({} so far)", rtStart / 1e7, (now - rtStart) / 1e4, m_skippedIngest));
                m_coreDisc = true;
                return S_OK;
            }
            std::lock_guard<std::mutex> lk(m_coreLock);
            if (!m_core) return S_OK;
            const bool disc = m_coreDisc || pIn->IsDiscontinuity() == S_OK || rtStart <= m_lastSubmitPts;
            PushCoreSubmitTiming t = { sizeof(t), INT64_MIN, 30000, m_cfg.admission ? m_fgLevel.load() : 0 };
            if (m_dvP5 && m_dvCur.active) {
                t.dvP5Active = 1;
                memcpy(t.dvP5YccToRgb, m_dvCur.ycc, sizeof(t.dvP5YccToRgb));
                memcpy(t.dvP5LmsToRgb, m_dvCur.lms, sizeof(t.dvP5LmsToRgb));
                memcpy(t.dvP5Curve, m_dvCur.curve, sizeof(t.dvP5Curve));
                if (m_dvCur.exact) { t.dvP5Exact = 1; memcpy(t.dvP5ExactCurve, m_dvCur.exactCurve, sizeof(t.dvP5ExactCurve)); }
            }
            if (m_cfg.admission && StreamNow(now)) t.streamNow100ns = now;
            const int64_t q0 = Qpc();
            const int r = m_api->submit2(m_core, m_upload.Get(), 0, rtStart, disc ? PUSHCORE_SUBMIT_DISCONTINUITY : 0, &t);
            m_submitMs += QpcMs(Qpc() - q0);
            if (r != PUSHCORE_OK) {
                submitErr = r;
                submitErrText = m_api->lastError(m_core);
            } else {
            if (disc && m_firstAfterFlush) Log(std::format("first submit after flush: pts {:.3f} s, {:.1f} ms after BeginFlush", rtStart / 1e7, QpcMs(Qpc() - m_flushStartQpc)));
            m_coreDisc = false; m_firstAfterFlush = false;
            m_lastSubmitPts = rtStart;
            if (m_quickPhase) TryHandover(rtStart, t);
            {
                // this source frame's side data (HDR10 static, HDR10+ dynamic, Dolby Vision RPU) for its outputs
                SideData sd; sd.CopyFrom(pIn, true);
                if (m_dvP5) ToP5Output(sd);
                if (sd.m.empty()) sd = m_hdrSide;
                std::lock_guard<std::mutex> sl(m_sideLock);
                m_sideByPts[rtStart] = std::move(sd);
                while (m_sideByPts.size() > 32) m_sideByPts.erase(m_sideByPts.begin());
            }
            m_lastSubmitQpc = Qpc();
            { int64_t none = 0; m_waitOutQpc.compare_exchange_strong(none, m_lastSubmitQpc); }   // the no-output guard (copier)
            SetEvent(m_hSubmit);
            {   // the no-output guard: source frames go in, nothing comes out -> the source frames are shown instead
                const int64_t out = m_lastCoreOutQpc.load();
                if (out != m_seenOutQpc) { m_seenOutQpc = out; m_submitsSinceOut = 0; } else ++m_submitsSinceOut;
                const int64_t since = std::max({ out, m_activeQpc, m_flushStartQpc });
                if (pm::CoreOutputStalled(QpcMs(Qpc() - since), m_submitsSinceOut, m_State == State_Running && !m_reloading)) {
                    Log(std::format("no-output guard: {} source frames went into pushcore and none came out for {:.0f} ms", m_submitsSinceOut, QpcMs(Qpc() - since)));
                    m_faulted = true; m_submitsSinceOut = 0;
                    Fallback(L"pushcore gave no frames (the source frames are shown instead)");
                }
            }
            }
        }
        if (m_sizeAfterHandover) {
            m_sizeAfterHandover = false;
            OutFmt cur;
            { std::lock_guard<std::mutex> lk(m_fmtLock); cur = m_outFmt; }
            RequestOutFmt({ cur.fmt, m_oi.width, m_oi.height }, "the full graph's output size");
        }
        if (submitErr != PUSHCORE_OK) {
            Log(std::format("submit failed {}: {}", submitErr, submitErrText));
            if (submitErr == PUSHCORE_DEVICE_LOST) DeviceLost(1, submitErrText);
            else if (submitErr == PUSHCORE_ERROR) { m_faulted = true; Fallback(L"pushcore error: " + Widen(submitErrText)); }
        }
        return S_OK;
    }
    // warm-up, reload, fallback: the source frame at the source rate (converted, or copied in the source format)
    OutFmt f;
    { std::lock_guard<std::mutex> lk(m_fmtLock); f = m_outFmt; }
    CD3D11Sample* s = GetFree(f, true);
    if (!s) return S_OK;
    DrawSource(s);
    s->side.CopyFrom(pIn, true); // the source frame's own side data, all of it
    if (m_dvP5) ToP5Output(s->side);
    ReadyItem it{ s, rtStart, rtStart + m_atpf, 2, m_inFrames, m_flushGen.load(), Qpc() };
    PushReady(it);
    return S_OK;
}

// ------------------------------------------------------------------------------------------------ threads
void CPushMpcFilter::StartThreads()
{
    if (m_copier.joinable() || m_mode == Mode::PassSys || m_mode == Mode::None) return;
    if (!m_csv && m_cfg.frameLog) {
        const std::wstring s = Stamp();
        m_csv = _wfsopen((LogDir() + L"\\filter-frames-" + s + L".csv").c_str(), L"w", _SH_DENYWR);
        if (m_csv) fprintf(m_csv, "deliverQpcMs,deliverMs,ptsMs,kind,source,lateMs,queue,copyToDeliverMs,dropped,fgLevel\n");
        m_gpuCsv = _wfsopen((LogDir() + L"\\filter-gpu-" + s + L".csv").c_str(), L"w", _SH_DENYWR);
        if (m_gpuCsv) fprintf(m_gpuCsv, "sourceFrameId,colorMs,srMs,fgBatchMs,fg1Ms,graphMs\n");
    }
    m_threadsRunning = true;
    m_copier = std::thread([this] { CopierLoop(); });
    m_deliverer = std::thread([this] { DelivererLoop(); });
}
void CPushMpcFilter::StopThreads()
{
    m_quit = true;
    WakeAll();
    const int64_t q0 = Qpc();
    if (m_copier.joinable()) m_copier.join();
    if (m_deliverer.joinable()) m_deliverer.join();
    if (m_threadsRunning) Log(std::format("threads joined in {:.1f} ms", QpcMs(Qpc() - q0)));
    m_threadsRunning = false;
    ClearReady();
    if (m_csv) fflush(m_csv);
    if (m_gpuCsv) fflush(m_gpuCsv);
}
void CPushMpcFilter::CopierLoop()
{
    timeBeginPeriod(1);
    ComPtr<ID3D11DeviceContext> def;
    m_device->CreateDeferredContext(0, &def);
    int64_t lastStats = Qpc();
    uint64_t copied = 0;
    while (!m_quit) {
        if (m_flushing) {
            const uint32_t gen = m_flushGen;
            DrainCore(false);
            m_copierAck = gen;
            WaitForSingleObject(m_hWake, 1);
            continue;
        }
        if (m_mode != Mode::Active || m_faulted) { WaitForSingleObject(m_hSubmit, 5); continue; }
        if (!m_quickPhase && m_settingsPending.exchange(false)) {
            PushCoreSettings ps;
            { std::lock_guard<std::mutex> lk(m_autoLock); ps = m_pending; }
            { std::lock_guard<std::mutex> lk(m_coreLock); if (m_core) m_api->setSettings(m_core, &ps); }
            DoReload(false);
            m_waitOutQpc = 0;
            { std::lock_guard<std::mutex> lk(m_autoLock); m_autoTimings.clear(); m_graphGen++; m_graphQpc = Qpc(); }
            continue;
        }
        if (!m_quickPhase && (m_reloadWanted || g_reloadSerial != m_reloadSeen)) {
            const bool forced = g_reloadSerial != m_reloadSeen;
            m_reloadSeen = g_reloadSerial.load();
            m_reloadWanted = false;
            DoReload(forced);
            m_waitOutQpc = 0;
            continue;
        }
        PushCoreFrame fr[8]; uint32_t n = 0;
        int pr = PUSHCORE_OK;
        PushCore* src = nullptr;   // fast start: the quick graph first, until it delivered its last frame (handover)
        const bool simulatedStop = m_cfg.simulateNoOutputSec > 0 && QpcMs(Qpc() - m_activeQpc) >= m_cfg.simulateNoOutputSec * 1000.0;
        if (!simulatedStop) {
            std::lock_guard<std::mutex> lk(m_coreLock);
            src = m_oldCore ? m_oldCore : m_core;
            if (src) pr = m_api->poll(src, fr, 8, &n, 0);
        }
        if (pr == PUSHCORE_DEVICE_LOST) { DeviceLost(1, "pushcore_poll"); continue; }
        if (n) m_waitOutQpc = 0;
        else if (const int64_t w = m_waitOutQpc.load(); w && !m_reloading && !m_flushing && m_mode == Mode::Active && QpcMs(Qpc() - w) >= 2000.0) {
            m_waitOutQpc = 0;
            Log(std::format("no-output guard: pushcore took frames but gave none back for {:.0f} ms{}: the source frames are shown instead",
                QpcMs(Qpc() - w), simulatedStop ? " (SimulateNoOutput)" : ""));
            m_faulted = true;
            Fallback(L"PushFPS stopped giving frames back: the source frames are shown");
            {
                // the stuck instance closes on its own thread now: the player's stop never waits for it
                std::lock_guard<std::mutex> lk(m_coreLock);
                if (m_oldCore) { CloseAsync(m_api, m_oldCore, "the quick graph (no-output guard)"); m_oldCore = nullptr; }
                if (m_core) { CloseAsync(m_api, m_core, "pushcore (no-output guard)"); m_core = nullptr; }
                m_quickPhase = false;
            }
            SetEvent(m_hLease);
            continue;
        }
        OutFmt f;
        { std::lock_guard<std::mutex> lk(m_fmtLock); f = m_outFmt; }
        for (uint32_t i = 0; i < n; i++) {
            {
                // fast start: the full graph's own output for the handover frame (and anything before it) is dropped
                std::lock_guard<std::mutex> lk(m_coreLock);
                if (src != m_core && src != m_oldCore) break;
                if (src == m_core && m_discardUpTo != INT64_MIN) {
                    if (fr[i].pts100ns <= m_discardUpTo) { m_api->presented(src, fr[i].lease, nullptr); SetEvent(m_hLease); continue; }
                    m_discardUpTo = INT64_MIN;
                }
            }
            CD3D11Sample* s = GetFree(f, false);
            std::lock_guard<std::mutex> lk(m_coreLock);
            if (src != m_core && src != m_oldCore) { if (s) s->Release(); break; }
            if (!s) { m_api->presented(src, fr[i].lease, nullptr); if (src == m_oldCore && fr[i].kind == PUSHCORE_FRAME_REAL && fr[i].pts100ns >= m_handoverPts) RetireOld("its last frame dropped"); continue; }
            m_ctx4->Wait(fr[i].readyFence, fr[i].readyValue);
            const bool hdrIn = fr[i].texture && [&] { D3D11_TEXTURE2D_DESC d; fr[i].texture->GetDesc(&d); return d.Format == DXGI_FORMAT_R10G10B10A2_UNORM; }();
            if (s->fmt.fmt == Fmt::Y410 || s->fmt.fmt == Fmt::Y416) {
                if (!hdrIn && !m_warnedSdrIntoY410) { m_warnedSdrIntoY410 = true; Log("pushcore output is SDR in an HDR10 connection: converted to PQ BT.2020 (SDR white 203 cd/m2)"); }
                ItmLimit(SrvFor(fr[i].texture), s, fr[i].kind == PUSHCORE_FRAME_REAL, hdrIn);
                ConvBegin();
                Dispatch(def.Get(), s, s->fmt.fmt == Fmt::Y416 ? m_csRgb2Y416.Get() : m_csRgb2Y410.Get(), SrvFor(fr[i].texture), nullptr, !hdrIn, false);
                ConvEnd();
            } else if (s->fmt.fmt == Fmt::P010H) {
                ItmLimit(SrvFor(fr[i].texture), s, fr[i].kind == PUSHCORE_FRAME_REAL, hdrIn);
                ConvBegin();
                DispatchP010(def.Get(), s, m_scrCopier, true, SrvFor(fr[i].texture), nullptr, !hdrIn, false);
                ConvEnd();
            } else {
                Draw(def.Get(), s, m_psRgb.Get(), SrvFor(fr[i].texture), nullptr);
            }
            m_api->presented(src, fr[i].lease, m_ctx.Get());
            SetEvent(m_hLease);
            {
                // Side data per output frame: a real frame takes its own source frame's; a generated frame takes that of
                // the real frame before it (the pair's earlier source frame: the latest source pts <= its pts). HDR10+
                // and Dolby Vision RPU are per source frame, so a generated frame between two scenes keeps the earlier
                // scene's metadata, as its pixels mostly do until the midpoint.
                std::lock_guard<std::mutex> sl(m_sideLock);
                auto st = m_sideByPts.upper_bound(fr[i].pts100ns);
                if (st != m_sideByPts.begin()) s->side = std::prev(st)->second; else s->side = m_hdrSide;
                FixHdrMin(s->side);
                if (m_hlgOut.load() && !s->side.m.count(IID_MediaSideDataHDR)) SetHdr10Side(s->side, 1, 1000.0, 0.005, 1000, 0);   // HLG: the BT.2100 reference display
                if (m_sceneCll.load()) SceneMaxCll(s->side);
                if (!hdrIn && m_itmOn.load() && IsHdrFmt(s->fmt.fmt)) {   // SDR shown as HDR10: the curve's static metadata
                    std::lock_guard<std::mutex> il(m_itmLock);
                    for (const auto& kv : m_itmSide.m) s->side.m[kv.first] = kv.second;
                }
            }
            const uint32_t mult = std::max<uint32_t>(1, m_mult);
            const REFERENCE_TIME srcStep = m_ptsStepMs > 0 ? REFERENCE_TIME(m_ptsStepMs * 1e4) : m_atpf;
            ReadyItem it{ s, fr[i].pts100ns, fr[i].pts100ns + srcStep / mult, fr[i].kind == PUSHCORE_FRAME_REAL ? 0 : 1, fr[i].sourceIndex, m_flushGen.load(), Qpc() };
            PushReady(it);
            copied++;
            m_lastCoreOutQpc = Qpc();
            if (src == m_oldCore && fr[i].kind == PUSHCORE_FRAME_REAL && fr[i].pts100ns >= m_handoverPts) RetireOld("after its last frame");
        }
        if (n == 0 && src) {
            std::lock_guard<std::mutex> lk(m_coreLock);
            if (src == m_oldCore) {
                PushCoreStats st = { sizeof(st) };
                if (m_api->stats(src, &st) == PUSHCORE_OK && st.pendingFrames == 0) RetireOld("drained");
            }
        }
        if (n == 0) {
            if (m_eos) {
                PushCoreStats st = { sizeof(st) };
                { std::lock_guard<std::mutex> lk(m_coreLock); if (m_core) m_api->stats(m_core, &st); }
                if (st.pendingFrames == 0) { m_eos = false; PushReady(ReadyItem{ nullptr, 0, 0, 3, 0, m_flushGen.load(), Qpc() }); Log("EOS: pushcore drained"); }
            }
            WaitForSingleObject(m_hSubmit, 1);
        }
        if (QpcMs(Qpc() - lastStats) >= 1000.0) {
            lastStats = Qpc();
            {
                bool retired = false; { std::lock_guard<std::mutex> lk(m_coreLock); retired = m_retired != nullptr; }
                if (retired && VideoMemoryShort()) { std::lock_guard<std::mutex> lk(m_coreLock); CloseRetired("video memory is needed"); }
            }
            PushCoreGpuTiming g[256]; uint32_t gn = 0;
            {
                std::lock_guard<std::mutex> lk(m_coreLock);
                if (m_core) m_api->takeGpuTimings(m_core, g, 256, &gn);
            }
            for (uint32_t i = 0; i < gn; i++) {
                if (m_gpuCsv) fprintf(m_gpuCsv, "%llu,%.3f,%.3f,%.3f,%.3f,%.3f\n", g[i].sourceFrameId, g[i].colorMs, g[i].srMs, g[i].fgBatchMs, g[i].fg1Ms, g[i].graphMs);
                if (g[i].srMs >= 0) m_recentSr.push_back(float(g[i].srMs));
                if (g[i].fgBatchMs >= 0) m_recentFg.push_back(float(g[i].fgBatchMs));
            }
            {
                std::lock_guard<std::mutex> lk(m_autoLock);
                const int lvl = m_fgLevel.load();
                for (uint32_t i = 0; i < gn; i++) m_autoTimings.push_back({ g[i], lvl, Qpc() });
                while (m_autoTimings.size() > 2048) m_autoTimings.pop_front();
            }
            while (m_recentSr.size() > 240) m_recentSr.pop_front();
            while (m_recentFg.size() > 240) m_recentFg.pop_front();
            auto med = [](std::deque<float> d) { if (d.empty()) return 0.0; std::sort(d.begin(), d.end()); return double(d[d.size() / 2]); };
            m_srMedian = med(m_recentSr); m_fgMedian = med(m_recentFg);
            if (m_gpuCsv) fflush(m_gpuCsv);
        }
    }
    timeEndPeriod(1);
}

// waits until stream time reaches `due` (high-resolution timer, last 0.3 ms spun); false = state changed / flushed
bool CPushMpcFilter::WaitUntil(REFERENCE_TIME due, uint32_t gen)
{
    for (;;) {
        if (m_quit || m_flushing || m_stopReq || gen != m_flushGen) return false;
        REFERENCE_TIME now = 0;
        if (!StreamNow(now)) return false; // not running any more
        const REFERENCE_TIME wait = due - now;
        if (wait <= 0) return true;
        if (wait > 3000) {
            LARGE_INTEGER li;
            li.QuadPart = -std::min<REFERENCE_TIME>(wait - 2000, 40000);
            SetWaitableTimer(m_hTimer, &li, 0, nullptr, nullptr, FALSE);
            HANDLE hs[2] = { m_hTimer, m_hWake };
            WaitForMultipleObjects(2, hs, FALSE, 50);
        } else {
            YieldProcessor();
        }
    }
}

void CPushMpcFilter::DelivererLoop()
{
    timeBeginPeriod(1);
    DWORD task = 0;
    HANDLE hAvrt = AvSetMmThreadCharacteristicsW(L"Playback", &task);
    if (hAvrt) AvSetMmThreadPriority(hAvrt, AVRT_PRIORITY_HIGH); else SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    const REFERENCE_TIME lead = REFERENCE_TIME(LeadUs()) * 10;
    Log(std::format("deliverer started (MMCSS {}), Deliver lead {} us", hAvrt ? "Playback/high" : "unavailable", lead / 10));
    int64_t statQpc = Qpc(), prerollQpc = 0;
    uint64_t statDelivered = m_delivered; // the counters live across Stop/Run (thread restarts)
    // The item being delivered is owned by this thread (taken off the queue at once), so a flush or stop that clears
    // the queue never touches it; it is dropped here when its flush generation is stale.
    ReadyItem it;
    bool have = false;
    size_t depth = 0;
    while (!m_quit) {
        AdmissionTick();
        if (QpcMs(Qpc() - statQpc) >= 10000.0) {
            const double el = QpcMs(Qpc() - statQpc);
            const uint64_t d = m_delivered - statDelivered;
            PushCoreStats st = { sizeof(st) };
            { std::lock_guard<std::mutex> lk(m_coreLock); if (m_core && !m_faulted) m_api->stats(m_core, &st); }
            const int64_t qn = m_qN.exchange(0), ql = m_qLateSum.exchange(0);
            Log(std::format("stats: {:.2f} fps delivered over {:.1f} s; delivered real {} gen {} source {}; dropped late gen {} real {}; delivered late {}; "
                "skipped late source {}; FG level {} (pairs full {} reduced {} none {}); MPC-VR quality: mean lateness vs its -8 ms target {:.2f} ms over {} samples; "
                "upload {:.2f} ms/frame, submit {:.2f} ms/frame; GPU VSR {:.2f} ms VFG {:.2f} ms (medians); deband {}; pushcore leasesOut {} pending {}; ready {}",
                d * 1000.0 / el, el / 1000.0, m_delReal, m_delGen, m_delSrc, m_dropGen, m_dropReal, m_lateDelivered, m_skippedIngest, m_fgLevel.load(), st.fgFull, st.fgReduced, st.fgSkipped,
                qn ? double(ql) / qn / 1e4 : 0.0, qn, m_uploads ? m_uploadMs / m_uploads : 0.0, st.submitted ? m_submitMs / st.submitted : 0.0, m_srMedian, m_fgMedian,
                st.debandActive ? std::format("{:.3f} ms (D3D11 timestamps, median)", st.debandMs) : std::string("off"), st.leasesOut, st.pendingFrames, ReadySize()));

            statQpc = Qpc(); statDelivered = m_delivered;
            if (m_csv) fflush(m_csv);
        }
        if (!have) {
            std::unique_lock<std::mutex> lk(m_readyLock);
            m_readyCv.wait_for(lk, std::chrono::milliseconds(5), [&] { return m_quit.load() || !m_ready.empty(); });
            if (m_quit || m_ready.empty()) continue;
            it = m_ready.front();
            m_ready.pop_front();
            depth = m_ready.size() + 1;
            have = true;
        }
        auto drop = [&] { if (it.sample) it.sample->Release(); it = {}; have = false; };
        if (it.gen != m_flushGen || m_flushing || m_stopReq) { drop(); if (m_flushing || m_stopReq) WaitForSingleObject(m_hWake, 2); continue; }
        // Frame step. MPC-HC steps with IVideoFrameStep on the graph; the graph manager hands the step to the video
        // renderer (stock MPC-VR implements AM_KSPROPSETID_FrameStep: it runs the graph, shows ONE sample, sends
        // EC_STEP_COMPLETE). The filter is not told; it would step one output frame (a generated one 3 times in 4).
        // So after a pause from playing (step mode), generated frames are not delivered: what MPC-VR gets next, and
        // shows on a step, is the next real frame. Step mode ends once playback has run for 300 ms (a step runs the
        // graph for one frame only); the cost is ~300 ms of real frames only after a resume.
        if (m_stepMode) {
            if (m_State == State_Running && QpcMs(Qpc() - m_resumedQpc) > 300.0) {
                m_stepMode = false;
                Log(std::format("frame step mode off (playing again); {} generated frames skipped while paused/stepping", m_stepSkips));
            } else if (it.kind == 1) {
                m_stepSkips++;
                drop();
                continue;
            }
        }
        if (it.kind == 3) {
            have = false;
            Log("delivering EndOfStream");
            m_pOutput->DeliverEndOfStream();
            continue;
        }
        REFERENCE_TIME now = 0;
        if (!StreamNow(now)) {
            // paused/prerolling: the first frame after the start or a flush completes the pause (MPC-VR's Ready);
            // after a seek it waits until the first pair is ready (PushMPC M2c preroll lead, at most 400 ms)
            if (!m_needPreroll) { WaitForSingleObject(m_hWake, 5); continue; }
            if (it.kind == 1) { drop(); continue; } // a generated frame is never the preroll frame
            if (m_cfg.preroll && m_mode == Mode::Active && it.kind == 0 && m_State == State_Paused) {
                if (!prerollQpc) prerollQpc = Qpc();
                size_t reals = 1;
                { std::lock_guard<std::mutex> lk(m_readyLock); for (auto& r : m_ready) if (r.kind == 0) reals++; }
                if (reals < 2 && QpcMs(Qpc() - prerollQpc) < 400.0) { WaitForSingleObject(m_hWake, 2); continue; }
                Log(std::format("preroll: first frame delivered {:.0f} ms after it was ready ({})", QpcMs(Qpc() - prerollQpc), reals >= 2 ? "first pair ready" : "timeout"));
            }
            prerollQpc = 0;
        } else {
            prerollQpc = 0;
            // Stock MPC-VR renders a sample at once when it is due within 8 ms, then sleeps (whole ms, under its locks)
            // until half a refresh period before the frame's time and presents. Delivering at that point makes it
            // present at once: this thread's high-resolution wait sets the present time, MPC-VR's locks are held only
            // for the draw and Present, and the wait between frames is here, where Stop/flush/pause wake it.
            if (!WaitUntil(it.start - lead, it.gen)) continue; // state change: re-evaluate the same item
            StreamNow(now);
            const uint32_t mult = std::max<uint32_t>(1, m_mult);
            const REFERENCE_TIME srcStep = m_ptsStepMs > 0 ? REFERENCE_TIME(m_ptsStepMs * 1e4) : m_atpf;
            const REFERENCE_TIME late = now - it.start;
            const bool lateGen = it.kind == 1 && late > srcStep / mult / 2;
            const bool lateReal = it.kind != 1 && late > srcStep && ReadySize() > 0 && !m_stepMode;
            if (lateGen || lateReal) {
                (lateGen ? m_dropGen : m_dropReal)++;
                if (m_csv) fprintf(m_csv, "%.3f,0,%.3f,%d,%llu,%.3f,%zu,%.3f,1,%d\n", QpcMs(Qpc()), it.start / 1e4, it.kind, it.src, late / 1e4, depth, QpcMs(Qpc() - it.copiedQpc), m_fgLevel.load());
                drop();
                continue;
            }
            // The graph manager pauses the renderer before this filter. A sample delivered into the paused renderer is
            // shown at once and then held, and a frame step shows that held sample again: so nothing is delivered while
            // MPC-VR is not running (step mode starts here), and the next sample it gets is the next real frame.
            if (m_down) {
                FILTER_STATE ds = State_Running;
                m_down->GetState(0, &ds);
                if (ds != State_Running) {
                    if (!m_stepMode) { m_stepMode = true; m_stepSkips = 0; Log("renderer paused before this filter: step mode"); }
                    WaitForSingleObject(m_hWake, 2);
                    continue;
                }
            }
        }
        have = false;
        m_needPreroll = false;
        // the output type follows the sample: a format/size change goes on this sample (MPC-VR re-inits for it)
        CMediaType changed;
        const bool change = it.sample->fmt != m_deliveredFmt;
        if (change) {
            BuildMediaType(it.sample->fmt, &changed);
            it.sample->SetMediaType(&changed);
            Log(std::format("delivering a media type change: {} -> {}", m_deliveredFmt.Str(), it.sample->fmt.Str()));
        }
        REFERENCE_TIME st = it.start, sp = it.stop;
        {
            // the sample lasts until the next one (held-frame re-timing spaces outputs 2-4 grid steps apart in a held span)
            std::lock_guard<std::mutex> lk(m_readyLock);
            if (!m_ready.empty() && m_ready.front().gen == it.gen && m_ready.front().kind != 3 && m_ready.front().start > st && m_ready.front().start - st < 10000000)
                sp = m_ready.front().start;
        }
        it.sample->SetTime(&st, &sp);
        it.sample->SetSyncPoint(TRUE);
        it.sample->SetDiscontinuity(m_discNext.exchange(false) ? TRUE : FALSE);
        it.sample->SetPreroll(FALSE);
        it.sample->SetActualDataLength(0);
        const int64_t q0 = Qpc();
        m_inDeliver = true;
        const HRESULT hr = m_pOutput->Deliver(it.sample);
        m_inDeliver = false;
        const int64_t q1 = Qpc();
        if (change && SUCCEEDED(hr)) {
            Out()->SetMt(changed);
            m_deliveredFmt = it.sample->fmt;
            Log(std::format("media type change accepted (Deliver {:.1f} ms)", QpcMs(q1 - q0)));
        }
        if (change) it.sample->SetMediaType(nullptr);
        it.sample->Release();
        m_delivered++;
        (it.kind == 0 ? m_delReal : it.kind == 1 ? m_delGen : m_delSrc)++;
        double lateMs = NAN;
        REFERENCE_TIME after = 0;
        if (StreamNow(after)) {
            lateMs = double(after - it.start) / 1e4;
            const uint32_t mult = std::max<uint32_t>(1, m_mult);
            const REFERENCE_TIME srcStep = m_ptsStepMs > 0 ? REFERENCE_TIME(m_ptsStepMs * 1e4) : m_atpf;
            if (it.kind == 1 && after - it.start > srcStep / mult / 2) m_lateDelivered++;
        }
        if (m_csv) fprintf(m_csv, "%.3f,%.3f,%.3f,%d,%llu,%.3f,%zu,%.3f,0,%d\n", QpcMs(q0), QpcMs(q1 - q0), it.start / 1e4, it.kind, it.src, lateMs, depth, QpcMs(q0 - it.copiedQpc), m_fgLevel.load());
        if (m_csv && QpcMs(q1 - m_csvFlushQpc) > 300.0) { fflush(m_csv); m_csvFlushQpc = q1; } // readable while paused/stepping
        if (FAILED(hr) && m_lastDeliverHr != hr) Log(std::format("Deliver -> 0x{:08X}", uint32_t(hr)));
        m_lastDeliverHr = hr;
        it = {};
    }
    if (have && it.sample) it.sample->Release();
    if (m_csv) fflush(m_csv);
    if (hAvrt) AvRevertMmThreadCharacteristics(hAvrt);
    timeEndPeriod(1);
}

// FG admission, ported from the fork (PushFps.cpp AdmissionTick, M2c): 250-ms windows of generated frames delivered /
// late (dropped late here, or Deliver returned after half an output interval); step down after two bad windows (or
// four mild ones from full groups); step up when pushcore's measured cost predicts that full groups fit 92 % of the
// source interval in stream time; a flush keeps the level and re-decides it at once from the measured cost.
void CPushMpcFilter::AdmissionTick()
{
    const int64_t q = Qpc();
    const bool rateChanged = m_admRateChanged.exchange(false);
    const bool flushed = m_admFlushed.exchange(false) || rateChanged;
    if (!flushed && QpcMs(q - m_admQpc) < 250.0) return;
    m_admQpc = q;
    const uint64_t presAll = m_delGen, lateAll = m_dropGen + m_lateDelivered;
    const uint64_t pres = presAll - m_admGenPresented, late = lateAll - m_admGenLate;
    m_admGenPresented = presAll; m_admGenLate = lateAll;
    if (!m_cfg.admission || m_mult < 2 || m_reloading || m_mode != Mode::Active) { m_admHistN = 0; m_admGood = 0; return; }
    int level = m_fgLevel;
    PushCoreStats st = { sizeof(st) };
    {
        std::lock_guard<std::mutex> lk(m_coreLock);
        if (!m_core || m_api->stats(m_core, &st) != PUSHCORE_OK) st.fullCostRecentMs = st.reducedCostRecentMs = st.fullCostWindowMs = st.reducedCostWindowMs = -1;
    }
    const double budget = (m_ptsStepMs > 0 ? m_ptsStepMs.load() : double(m_atpf) / 1e4) * 0.92;
    double ratio = 2.2;
    if (st.fullCostWindowMs > 0 && st.reducedCostWindowMs > 0) ratio = std::clamp(st.fullCostWindowMs / st.reducedCostWindowMs, 1.6, 3.0);
    double predictedFull = -1;
    if (st.fullCostRecentN >= 8 && st.fullCostRecentMs > 0) predictedFull = st.fullCostRecentMs;
    else if (st.reducedCostRecentN >= 8 && st.reducedCostRecentMs > 0) predictedFull = st.reducedCostRecentMs * ratio;
    const bool fullFits = predictedFull > 0 && predictedFull <= budget;
    if (flushed) {
        m_admHistN = 0; m_admGood = 0;
        // Round 2 (F9): a faster playback rate shortens the interval at once; full groups measured clearly over the new
        // budget step down before their frames go late (fix round 1: 56 of 56 late in the first 0.5 s at 0.5x -> 1.0x)
        // until the Auto thread's new graph is ready. The 15 % margin keeps graphs that still fit (2.0x at 5x: 0.6 %
        // late in fix round 1) on full groups.
        if (rateChanged && level == 0 && predictedFull > budget * 1.15) {
            const double reduced = (st.reducedCostRecentN >= 8 && st.reducedCostRecentMs > 0) ? st.reducedCostRecentMs : predictedFull / ratio;
            level = reduced <= budget ? 1 : 2;
            m_fgLevel = level; m_admGrace = 2;
            m_admLastDown = q; m_admHoldUntil = q + int64_t(1.0 * QpcFreq()); m_admLastProbe = q;
            ++m_admChanges;
            Log(std::format("FG admission: playback rate change; full groups measured {:.1f} ms against the new budget {:.1f} ms -> {} (reduced predicted {:.1f} ms)",
                predictedFull, budget, level == 1 ? "2X midpoints only (PushFPS reduced groups)" : "no generated frames", reduced));
            return;
        }
        if (level > 0 && fullFits) {
            m_fgLevel = 0; m_admLastUp = q; ++m_admChanges;
            Log(std::format("FG admission: flush; full groups predicted to fit ({:.1f} of {:.1f} ms) -> full groups", predictedFull, budget));
        } else if (level > 0) {
            Log(std::format("FG admission: flush; keeping {} (full groups predicted {:.1f} ms, budget {:.1f} ms)", level == 1 ? "2X midpoints" : "no generated frames", predictedFull, budget));
        }
        return;
    }
    if (m_State != State_Running) { m_admHistN = 0; m_admGood = 0; return; }
    if (m_admGrace > 0) {
        --m_admGrace;
        const bool severe = late >= 6 && late * 2 > pres + late;
        if (!severe) { m_admHistN = 0; m_admGood = 0; return; }
    }
    for (int i = 3; i > 0; --i) m_admHist[i] = m_admHist[i - 1];
    m_admHist[0] = { uint32_t(pres), uint32_t(late) };
    m_admHistN = std::min(4, m_admHistN + 1);
    auto lateFrac = [](const AdmWin& w) { return (w.pres + w.late) ? double(w.late) / double(w.pres + w.late) : 0.0; };
    bool bad = false;
    if (m_admHistN >= 2) {
        const AdmWin& a = m_admHist[0]; const AdmWin& b = m_admHist[1];
        bad = a.pres + a.late >= 3 && b.pres + b.late >= 3 && lateFrac(a) > 0.25 && lateFrac(b) > 0.25;
    }
    // a step up that does not hold is taken back after one bad window (not two): pushcore's GPU timestamps do not see
    // other processes' load, so a predicted step up can fail under contention, and its drop burst is kept short
    const bool probing = m_admChanges && level < 2 && QpcMs(q - m_admLastUp) < 1500.0 && m_admLastUp > m_admLastDown;
    if (!bad && probing) {
        const AdmWin& a = m_admHist[0];
        bad = a.pres + a.late >= 3 && lateFrac(a) > 0.25;
    }
    if (!bad && m_admHistN >= 4 && level == 0) {
        uint32_t p = 0, l = 0;
        for (int i = 0; i < 4; ++i) { p += m_admHist[i].pres; l += m_admHist[i].late; }
        bad = p + l >= 8 && double(l) > 0.12 * double(p + l);
    }
    m_admGood = (late == 0) ? m_admGood + 1 : 0;
    if (QpcMs(q - m_admLastUp) > 15000.0) m_admBackoffS = 1.0;
    if (bad && level < 2) {
        if (m_admChanges && QpcMs(q - m_admLastUp) < 3000.0) m_admBackoffS = std::min(30.0, std::max(2.0, m_admBackoffS * 2.0));
        uint32_t p = 0, l = 0;
        const int nWin = m_admHistN;
        for (int i = 0; i < nWin; ++i) { p += m_admHist[i].pres; l += m_admHist[i].late; }
        m_fgLevel = ++level;
        m_admHistN = 0; m_admGood = 0; m_admGrace = 2;
        m_admLastDown = q; m_admHoldUntil = q + int64_t(m_admBackoffS * QpcFreq()); m_admLastProbe = q;
        ++m_admChanges;
        Log(std::format("FG admission: {} of {} generated frames late in {:.2f} s -> {} (next predicted step up after {:.0f} s; full groups measured {:.1f} ms, budget {:.1f} ms)",
            l, p + l, 0.25 * nWin, level == 1 ? "2X midpoints only (PushFPS reduced groups)" : "no generated frames", m_admBackoffS, predictedFull, budget));
        return;
    }
    if (level > 0 && m_admGood >= 4 && q >= m_admHoldUntil) {
        bool up = false;
        const char* why = "";
        if (level == 1 && fullFits) { up = true; why = "predicted to fit"; }
        else if (level == 2) { up = st.reducedCostWindowMs <= 0 || st.reducedCostWindowMs <= budget || QpcMs(q - m_admLastProbe) >= 30000.0; why = "probe"; }
        else if (QpcMs(q - m_admLastProbe) >= 30000.0) { up = true; why = "probe (30 s)"; }
        if (up) {
            m_fgLevel = --level;
            m_admGood = 0; m_admHistN = 0; m_admGrace = 0;
            m_admLastUp = q; m_admLastProbe = q;
            ++m_admChanges;
            Log(std::format("FG admission: {} -> {} (full groups predicted {:.1f} ms, budget {:.1f} ms, ratio {:.2f})", why, level == 0 ? "full groups" : "2X midpoints", predictedFull, budget, ratio));
        }
    }
}

// ------------------------------------------------------------------------------------------------ Auto
pm::Source CPushMpcFilter::SourceInfo()
{
    pm::Source s;
    s.w = m_visW ? m_visW : 1920; s.h = m_visH ? m_visH : 1080;
    const double step = m_ptsStepMs.load();
    s.fps = step > 1.0 ? 1000.0 / step : 1e7 / double(m_atpf > 0 ? m_atpf : 417083) * (m_rate.load() > 0 ? m_rate.load() : 1.0);
    s.hdr = InputHdr();
    s.tenBit = m_inP010;
    // bits per pixel per frame at the measured frame rate (a "29.97" tag on a 24p body would make it 25 % low)
    s.bppEq = pm::BitsPerPixel(m_bitrate, s.w, s.h, step > 1.0 ? 1000.0 / m_baseStepMs.load() : 1e7 / double(m_atpf > 0 ? m_atpf : 417083), m_fourcc);
    s.heldShare = m_heldShare.load();
    s.contentClass = m_contentClass.load(); s.contentConf = m_contentConf.load();
    // the page may set this show's choice while it plays (Shows\<key>\Choice)
    if (!m_showKey.empty()) m_showChoice = pm::ReadShow(m_showKey).choice;
    s.showChoice = m_showChoice.load();
    return s;
}
// The stream's bitrate, for Clean up's Auto rule: the splitter's video type (dwBitRate), else the file's size over its
// duration with 90 % of it taken as video (audio and the container are the rest). The codec FourCC comes with it.
void CPushMpcFilter::ProbeBitrate()
{
    if (m_bitrateProbed) return;
    m_bitrateProbed = true;
    m_bitrate = -1; m_fourcc = 0; m_bitrateHow = L"unknown";
    IPin* lavOut = m_pInput->GetConnected();
    if (!lavOut) return;
    PIN_INFO pi = {};
    if (FAILED(lavOut->QueryPinInfo(&pi)) || !pi.pFilter) return;
    ComPtr<IBaseFilter> lav; lav.Attach(pi.pFilter);
    ComPtr<IEnumPins> e;
    if (FAILED(lav->EnumPins(&e))) return;
    ComPtr<IPin> p;
    while (e->Next(1, &p, nullptr) == S_OK) {
        PIN_DIRECTION d = PINDIR_OUTPUT; p->QueryDirection(&d);
        ComPtr<IPin> split;
        if (d != PINDIR_INPUT || FAILED(p->ConnectedTo(&split)) || !split) { p.Reset(); continue; }
        AM_MEDIA_TYPE mt = {};
        if (SUCCEEDED(p->ConnectionMediaType(&mt))) {
            m_fourcc = mt.subtype.Data1;
            DWORD br = 0;
            if (mt.formattype == FORMAT_VideoInfo2 && mt.cbFormat >= sizeof(VIDEOINFOHEADER2)) br = ((const VIDEOINFOHEADER2*)mt.pbFormat)->dwBitRate;
            else if (mt.formattype == FORMAT_VideoInfo && mt.cbFormat >= sizeof(VIDEOINFOHEADER)) br = ((const VIDEOINFOHEADER*)mt.pbFormat)->dwBitRate;
            else if (mt.formattype == FORMAT_MPEG2Video && mt.cbFormat >= sizeof(MPEG2VIDEOINFO)) br = ((const MPEG2VIDEOINFO*)mt.pbFormat)->hdr.dwBitRate;
            else if (mt.formattype == FORMAT_MPEGVideo && mt.cbFormat >= sizeof(MPEG1VIDEOINFO)) br = ((const MPEG1VIDEOINFO*)mt.pbFormat)->hdr.dwBitRate;
            FreeMediaType(mt);
            if (br > 10000) { m_bitrate = br; m_bitrateHow = L"the splitter's stream type"; }
        }
        if (m_bitrate <= 0) {
            ComPtr<IMediaSeeking> ms;
            LONGLONG dur = 0;
            PIN_INFO spi = {};
            if (SUCCEEDED(split.As(&ms)) && SUCCEEDED(ms->GetDuration(&dur)) && dur > 10000000 && SUCCEEDED(split->QueryPinInfo(&spi)) && spi.pFilter) {
                ComPtr<IBaseFilter> sf; sf.Attach(spi.pFilter);
                ComPtr<IFileSourceFilter> fs;
                LPOLESTR name = nullptr;
                if (SUCCEEDED(sf.As(&fs)) && SUCCEEDED(fs->GetCurFile(&name, nullptr)) && name) {
                    WIN32_FILE_ATTRIBUTE_DATA a = {};
                    if (GetFileAttributesExW(name, GetFileExInfoStandard, &a)) {
                        const double bytes = double((uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow);
                        m_bitrate = bytes * 8.0 / (double(dur) / 1e7) * 0.9;
                        m_bitrateHow = L"file size over duration (90 % video)";
                    }
                    CoTaskMemFree(name);
                }
            }
        }
        break;
    }
    const pm::Source s = SourceInfo();
    Log(std::format("bitrate: {:.2f} Mbit/s from {}, codec {:08X}, {:.3f} H.264-equivalent bits per pixel ({} for Clean up's Auto, threshold {:.2f})",
        m_bitrate / 1e6, Narrow(m_bitrateHow), m_fourcc, s.bppEq, s.bppEq < 0 ? "unknown" : s.bppEq <= pm::kLowBitrateBpp ? "low" : "high", pm::kLowBitrateBpp));
}
pm::Choice CPushMpcFilter::DecideNow(const pm::Settings& s, const pm::Display& d)
{
    if (!m_learnedLoaded) LoadLearned();
    pm::Costs c; pm::Gpu g;
    { std::lock_guard<std::mutex> lk(m_autoLock); c = m_costs; g = m_gpu; }
    return pm::Decide(s, SourceInfo(), d, c, g);
}
static uint32_t CoreFg(pm::FgMethod m) { return m == pm::FgMethod::Auto ? PUSHCORE_FG_VFG : uint32_t(int(m) - 1); }
// a frame generation method this filter knows (PUSHCORE_FG_NONE and unknown ids: none)
static bool KnownFg(uint32_t coreFg) { return coreFg <= PUSHCORE_FG_VPL || coreFg == PUSHCORE_FG_HELIXFG; }
static pm::FgMethod MethodOf(uint32_t coreFg) { return KnownFg(coreFg) ? pm::FgMethod(int(coreFg) + 1) : pm::FgMethod::Auto; }
PushCoreSettings CPushMpcFilter::ToCore(const pm::Settings& s, const pm::Choice& c)
{
    PushCoreSettings p = {};
    p.structSize = sizeof(p);
    p.source = PUSHCORE_SETTINGS_PUSHMPC;
    p.multiplier = 1; p.srQuality = 2; p.srTarget = PUSHCORE_SR_TARGET_4K; p.vfgQuality = 1;
    p.srFamily = PUSHCORE_SR_FAMILY_STANDARD; p.deband = 0; p.debandStrength = uint32_t(s.debandStrength);
    p.fgBackend = PUSHCORE_FG_VFG; p.fgTier = 1; p.srMethod = PUSHCORE_SR_METHOD_RTX; p.cutGuard = 1; p.fgLite = 0;
    pm::StrengthValues(s.strength, p.nrTone, p.nrStructure);
    if (!s.enable) return p; // everything off: frames pass through pushcore unchanged (switch it on again any time)
    p.sr = c.sr ? 1 : 0;
    p.srQuality = c.srLevel >= 1 ? uint32_t(c.srLevel) : 2u;
    p.srFamily = c.sr && c.srMethod == pm::Upscaler::Rtx ? uint32_t(std::clamp(c.srFamily, 0, 2)) : PUSHCORE_SR_FAMILY_STANDARD;
    p.srMethod = c.srMethod == pm::Upscaler::Fsr ? PUSHCORE_SR_METHOD_FSR : c.srMethod == pm::Upscaler::Nis ? PUSHCORE_SR_METHOD_NIS
               : c.srMethod == pm::Upscaler::IntelVsr ? PUSHCORE_SR_METHOD_IVSR : c.srMethod == pm::Upscaler::Xess ? PUSHCORE_SR_METHOD_XESS : PUSHCORE_SR_METHOD_RTX;
    if (c.srMethod == pm::Upscaler::Anime && c.animeModel >= 0 && (g_coreFeatures & PUSHCORE_FEATURE_ANIME_SR)) { p.srMethod = PUSHCORE_SR_METHOD_PMML; p.animeSrModel = uint32_t(c.animeModel); }
    p.deband = c.deband ? 1 : 0;
    // the anime preset (3) only on cores with PUSHCORE_FEATURE_ANIME_HOLD; an older core gets Medium
    p.debandStrength = c.debandStrength == 3 ? ((g_coreFeatures & PUSHCORE_FEATURE_ANIME_HOLD) ? PUSHCORE_DEBAND_ANIME : PUSHCORE_DEBAND_MEDIUM) : uint32_t(std::clamp(c.debandStrength, 0, 2));
    pm::UpscaleTarget t = pm::ResolvedTarget(s.target, m_display);
    if (c.artifactReduction) t = m_visH > 2160 || m_visW > 3840 ? pm::UpscaleTarget::K5 : pm::UpscaleTarget::K4; // never a downscale
    p.srTarget = t == pm::UpscaleTarget::P1440 ? PUSHCORE_SR_TARGET_1440P : t == pm::UpscaleTarget::K5 ? PUSHCORE_SR_TARGET_5K : PUSHCORE_SR_TARGET_4K;
    p.multiplier = uint32_t(std::clamp(c.multiplier, 1, pm::CoreCeiling()));
    p.fgBackend = CoreFg(c.fgMethod);
    p.vfgQuality = c.fgMethod == pm::FgMethod::Vfg ? uint32_t(std::clamp(c.fgQuality, 0, 2)) : 1u;
    p.fgTier = c.fgMethod == pm::FgMethod::PushFlow ? uint32_t(std::clamp(c.fgQuality, 0, 2)) : 1u;
    p.fgLite = c.fgMethod == pm::FgMethod::Dlssg && c.fgQuality == pm::kDlssgLiteQ ? 1 : 0;   // RTX 50 performance mode / RTX 30 port preset C
    p.nr = c.nr ? 1 : 0;
    bool hdrFmt = false;
    { std::lock_guard<std::mutex> lk(m_fmtLock); hdrFmt = IsHdrFmt(m_outFmt.fmt); }
    p.videoHdr = c.hdr && !c.hdrItm && hdrFmt && !InputHdr() ? 1 : 0;   // PushMPC's curve runs in the filter instead
    p.cutGuard = c.guardFlow ? 3 : 1;   // 3: with NVIDIA's optical flow (Auto: when it fits the budget)
    p.videoHdrPeak = uint32_t(c.rtxHdrPeak);                             // HDR boost: RTX Video HDR's peak (Low 500 / Medium 700 / High 1000)
    // PushFPS 4.5's Subtitle Hold: only with frame generation (pushcore leaves it out of a graph without it anyway)
    p.subtitleHold = (g_coreFeatures & PUSHCORE_FEATURE_SUBTITLE_HOLD) && p.multiplier > 1
        ? pm::SubtitleHoldValue(s, (g_coreFeatures & PUSHCORE_FEATURE_SUBTITLE_HOLD_AUTO) != 0, pmu::ReadUnlockMode(pmu::Unlock::SubtitleHoldAuto) != pmu::UnlockMode::Off) : 0;
    // held-frame re-timing (animation drawn on twos / threes): with frame generation, unless Animation is Off. pushcore
    // re-times only where it finds held frames, so it is on for every video (live action has none and is unchanged).
    p.heldFrames = (g_coreFeatures & PUSHCORE_FEATURE_HELD_FRAMES) && p.multiplier > 1 && s.enable && s.animation != pm::kOff ? (s.animation == pm::kAuto ? 1 : 2) : 0;
    // round 5: the animation mode and the holds (pushcore makes hold plans only with heldFrames)
    if ((g_coreFeatures & PUSHCORE_FEATURE_ANIME_HOLD) && p.heldFrames) { p.animationMode = c.animMode; p.holdFlags = c.holdFlags; }
    return p;
}
static bool SameGraph(const PushCoreSettings& a, const PushCoreSettings& b)
{
    if (a.sr != b.sr || a.multiplier != b.multiplier || a.nr != b.nr || a.videoHdr != b.videoHdr) return false;
    if (a.videoHdr && a.videoHdrPeak != b.videoHdrPeak) return false;
    if (a.multiplier > 1 && (a.cutGuard != b.cutGuard || a.subtitleHold != b.subtitleHold || a.heldFrames != b.heldFrames || a.animationMode != b.animationMode || a.holdFlags != b.holdFlags)) return false;
    if (a.sr && (a.srQuality != b.srQuality || a.srTarget != b.srTarget || a.srFamily != b.srFamily || a.srMethod != b.srMethod)) return false;
    if (a.multiplier > 1 && (a.vfgQuality != b.vfgQuality || a.fgBackend != b.fgBackend || a.fgTier != b.fgTier || a.fgLite != b.fgLite)) return false;
    if (a.nr && (a.nrTone != b.nrTone || a.nrStructure != b.nrStructure)) return false;
    // the deband pass is not part of the graph, but a change must still reach pushcore (setSettings: next frame)
    if (a.deband != b.deband || (a.deband && a.debandStrength != b.debandStrength)) return false;
    return true;
}
static bool IsUpgrade(const PushCoreSettings& from, const PushCoreSettings& to)
{
    return to.multiplier > from.multiplier || (to.sr && (!from.sr || to.srQuality > from.srQuality)) || (to.nr && !from.nr) ||
           (to.multiplier == from.multiplier && to.fgBackend == from.fgBackend && (to.vfgQuality > from.vfgQuality || to.fgTier > from.fgTier || (from.fgLite && !to.fgLite)));
}
static std::wstring GraphKey(const PushCoreSettings& p)
{
    return std::format(L"{} {}x q{} t{} sr{} m{} L{} style{} nr{} hdr{} deband{} sub{} held{}", pm::FgMethodLabel(MethodOf(p.fgBackend)), p.multiplier, p.vfgQuality, p.fgTier,
                       p.sr, p.srMethod, p.srQuality, p.srFamily, p.nr, p.videoHdr, p.deband ? int(p.debandStrength) : -1, p.subtitleHold, p.heldFrames);
}
void CPushMpcFilter::PublishNotes(const pm::Settings& s, const pm::Choice& c)
{
    if (!s.enable) { SetNotes(L"off (PushMPC is off)", L"off (PushMPC is off)", L"off (PushMPC is off)", L"off (PushMPC is off)"); return; }
    std::wstring hdr = c.hdrNote;
    bool hdrFmt = false;
    { std::lock_guard<std::mutex> lk(m_fmtLock); hdrFmt = IsHdrFmt(m_outFmt.fmt); }
    if (c.hdr && !hdrFmt && m_mode != Mode::None && !InputHdr()) hdr = L"off for this file (Windows HDR came on after it was opened; reopen the file)";
    // an HDR source: who tone-maps it for this file (decided when the player connected)
    if (InputHdr() && !m_display.hdr) hdr += m_coreToneMap ? L"; tone-mapped to SDR by PushMPC" : L"; tone-mapped to SDR by the renderer";
    std::wstring deband = c.debandNote;
    if (c.deband) {
        const double ms = m_debandMsNow.load();
        deband += std::format(L" ({} strength{})", pm::DebandStrengthStr(pm::DebandStrength(c.debandStrength)),
                              ms > 0 ? std::format(L", {:.2f} ms per frame", ms) : std::wstring());
    }
    SetNotes(c.smootherNote, c.animeSrNote.empty() ? c.sharperNote : c.sharperNote + L"; anime upscaler: " + c.animeSrNote, c.cleanNote, hdr, deband);
}
void CPushMpcFilter::LogChoice(const char* why, const pm::Choice& c)
{
    Log(std::format("Auto ({}): frame generation {} [stopped at: {}]; sharpen {}; clean up {}; SDR to HDR {}; deband {}; animation {}; output {}x{}; predicted {:.1f} ms of {:.1f} ms budget; "
        "HDR to SDR {} (connected: {}); display {} {:.3f} Hz HDR {} VRR {} ({}) {}; GPU {} ({}, prior factor {:.1f})",
        why, Narrow(c.smootherNote), Narrow(c.stoppedAt), Narrow(c.sharperNote), Narrow(c.cleanNote), Narrow(c.hdrNote), Narrow(c.debandNote), Narrow(c.animNote), c.outW, c.outH, c.predictedMs, c.budgetMs,
        c.toneMapNote.empty() ? "-" : Narrow(c.toneMapNote), !InputHdr() ? "-" : m_coreToneMap ? "PushMPC" : "the renderer",
        Narrow(m_display.name), m_display.refreshHz, m_display.hdr ? "on" : "off", m_display.vrr ? "on" : "off", Narrow(m_display.vrrHow), m_display.fullscreen ? "fullscreen" : "windowed",
        Narrow(m_gpu.name), Narrow(pm::FamilyName(m_gpu.family)), m_costs.card));
}
// Learned costs per GPU (HKCU\Software\PushMPC\Learned), so the next file starts from what this GPU measured: a
// measured / prior ratio per frame generation method and quality, VSR's ratio, NR, RTX Video HDR, deband, and PushFlow's
// own speed test (PushFlowBench, once per card).
void CPushMpcFilter::LoadLearned()
{
    m_learnedLoaded = true;
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\PushMPC\\Learned", 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    if (pm::RegReadStr(k, L"Gpu") == m_gpuName && !m_gpuName.empty() && pm::RegReadStr(k, L"Model") == L"3") {
        std::lock_guard<std::mutex> lk(m_autoLock);
        auto num = [&](const std::wstring& n, double& v, bool* flag, double lo, double hi) { const std::wstring s = pm::RegReadStr(k, n.c_str()); const double x = s.empty() ? -1 : _wtof(s.c_str()); if (x >= lo && x <= hi) { v = x; if (flag) *flag = true; return true; } return false; };
        for (int i = 0; i < pm::kFgKeys; ++i) num(std::format(L"FgScale{}", i), m_costs.fgScale[i], &m_costs.fgLearned[i], 0.05, 20);
        num(L"VsrScale", m_costs.vsrScale, &m_costs.learnedVsr, 0.1, 10);
        num(L"IvsrScale", m_costs.ivsrScale, &m_costs.learnedIvsr, 0.1, 10);
        num(L"XessScale", m_costs.xessScale, &m_costs.learnedXess, 0.1, 10);
        num(L"NrMs", m_costs.nrMs, &m_costs.learnedNr, 0.05, 200);
        num(L"Hdr4kMs", m_costs.hdr4k, &m_costs.learnedHdr, 0.05, 30);
        num(L"Deband1080Ms", m_costs.deband1080, &m_costs.learnedDeband, 0.001, 20);
        num(L"Base4kMs", m_costs.base4k, &m_costs.learnedBase, 0.05, 20);
        for (int i = 0; i < pm::kAnimeSrModels; ++i) num(std::wstring(L"AnimeSr-") + pm::AnimeSrModel(i).id, m_costs.animeSrLearned[i], nullptr, 0.01, 500);
        bool bench = true;
        for (int t = 0; t < 3; ++t) {
            bench = num(std::format(L"PfFirst{}", t), m_costs.pfFirst[t], nullptr, 0.1, 500) && bench;
            bench = num(std::format(L"PfNext{}", t), m_costs.pfNext[t], nullptr, 0.1, 500) && bench;
        }
        m_costs.pfBenched = bench;
        // flow once (PushFlow above 4x): the rebuild per tier, from a speed test that measured it (PfBenchVersion 2)
        bool rebuild = bench && pm::RegReadStr(k, L"PfBenchVersion") == L"2";
        for (int t = 0; t < 3 && rebuild; ++t) rebuild = num(std::format(L"PfRebuild{}", t), m_costs.pfRebuild[t], nullptr, 0.01, 200);
        m_costs.pfRebuildBenched = rebuild;
        if (pm::ApplyMeasuredCard(m_costs)) Log(std::format("card factor {:.2f} measured by PushFlow's speed test (replaces the name table)", m_costs.card));
        Log(std::format("learned GPU costs for {}: VFG Medium x{:.2f}, High x{:.2f}, DLSS-G x{:.2f}, PushFlow {} (Balanced {:.1f} + {:.1f} ms per frame at 4K), VSR x{:.2f}, clean up {:.2f} ms, deband {:.3f} ms at 1080p, rest of the graph {:.2f} ms at 4K{}",
            Narrow(m_gpuName), m_costs.fgScale[pm::kKeyVfgMed], m_costs.fgScale[pm::kKeyVfgHigh], m_costs.fgScale[pm::kKeyDlssg], bench ? "benched" : "priors",
            m_costs.pfFirst[1], m_costs.pfNext[1], m_costs.vsrScale, m_costs.nrMs, m_costs.deband1080, m_costs.base4k, m_costs.learnedBase ? "" : " (prior)"));
    }
    RegCloseKey(k);
}
void CPushMpcFilter::SaveLearned()
{
    pm::Costs c;
    { std::lock_guard<std::mutex> lk(m_autoLock); c = m_costs; }
    bool any = c.learnedVsr || c.learnedIvsr || c.learnedXess || c.learnedNr || c.learnedHdr || c.learnedDeband || c.learnedBase;
    for (double v : c.animeSrLearned) any = any || v > 0;
    for (bool b : c.fgLearned) any = any || b;
    if (m_gpuName.empty() || !any) return;
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\PushMPC\\Learned", 0, nullptr, 0, KEY_WRITE | KEY_READ, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    auto str = [&](const std::wstring& n, const std::wstring& v) { RegSetValueExW(k, n.c_str(), 0, REG_SZ, (const BYTE*)v.c_str(), DWORD((v.size() + 1) * sizeof(wchar_t))); };
    if (pm::RegReadStr(k, L"Gpu") != m_gpuName) {
        // another GPU's values: start over (the speed test is per card too)
        for (int t = 0; t < 3; ++t) { RegDeleteValueW(k, std::format(L"PfFirst{}", t).c_str()); RegDeleteValueW(k, std::format(L"PfNext{}", t).c_str()); RegDeleteValueW(k, std::format(L"PfRebuild{}", t).c_str()); }
        RegDeleteValueW(k, L"PfBenchVersion");
    }
    str(L"Gpu", m_gpuName);
    str(L"Model", L"3");
    for (int i = 0; i < pm::kFgKeys; ++i) if (c.fgLearned[i]) str(std::format(L"FgScale{}", i), std::format(L"{:.3f}", c.fgScale[i]));
    if (c.learnedVsr) str(L"VsrScale", std::format(L"{:.3f}", c.vsrScale));
    if (c.learnedIvsr) str(L"IvsrScale", std::format(L"{:.3f}", c.ivsrScale));
    if (c.learnedXess) str(L"XessScale", std::format(L"{:.3f}", c.xessScale));
    if (c.learnedNr) str(L"NrMs", std::format(L"{:.3f}", c.nrMs));
    if (c.learnedHdr) str(L"Hdr4kMs", std::format(L"{:.3f}", c.hdr4k));
    if (c.learnedDeband) str(L"Deband1080Ms", std::format(L"{:.4f}", c.deband1080));
    if (c.learnedBase) str(L"Base4kMs", std::format(L"{:.3f}", c.base4k));
    for (int i = 0; i < pm::kAnimeSrModels; ++i) if (c.animeSrLearned[i] > 0) str(std::wstring(L"AnimeSr-") + pm::AnimeSrModel(i).id, std::format(L"{:.3f}", c.animeSrLearned[i]));
    RegCloseKey(k);
}
// PushFlow's once-per-card speed test (pushcore_pushFlowBench, a port of PushFPS's PushFlowBench): run lazily, the first
// time Auto picks PushFlow on a card that has no result yet, on its own thread (a few seconds of GPU load while the
// first file plays on the priors). The result goes to HKCU\Software\PushMPC\Learned (PfFirst/PfNext per tier, ms per
// generated frame at 4K) and replaces PushFlow's priors from then on (the next Auto decision).
static std::atomic<bool> g_benchStarted = false;
static std::atomic<uint32_t> g_benchSerial = 0;
void CPushMpcFilter::MaybeBenchPushFlow(UINT w, UINT h)
{
    if (!g_benchFn || m_rt.root.empty() || g_benchStarted.exchange(true)) return;
    // once per card; once more for a card measured before the flow-once rebuild was (PushFlow above 4x needs its cost)
    { std::lock_guard<std::mutex> lk(m_autoLock); if (m_costs.pfBenched && (m_costs.pfRebuildBenched || pm::PushFlowCeiling() <= pm::kPushFlowOnceAbove)) return; }
    struct Ctx { std::wstring root, gpu; uint64_t luid; UINT w, h; HMODULE self; };
    auto* ctx = new Ctx{ m_rt.root, m_gpuName, m_gpuLuid, std::max<UINT>(w, 640), std::max<UINT>(h, 360), nullptr };
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&Log), &ctx->self);
    auto proc = [](void* p) -> DWORD {
        HMODULE self = nullptr;
        {
            std::unique_ptr<Ctx> c(static_cast<Ctx*>(p));
            self = c->self;
            PushCoreFlowBench b = {};
            b.structSize = sizeof(b); b.width = c->w; b.height = c->h; b.pairs = 12; b.tierMask = 7;
            const int64_t q0 = Qpc();
            Log(std::format("PushFlow speed test on {} at {}x{} (once per card)", Narrow(c->gpu), b.width, b.height));
            const int r = SehBench(c->luid, c->root.c_str(), &b);
            Log(std::format("PushFlow speed test {} in {:.0f} ms: first/next/rebuild p95 Fast {:.2f}/{:.2f}/{:.2f}, Balanced {:.2f}/{:.2f}/{:.2f}, Quality {:.2f}/{:.2f}/{:.2f} ms; {}",
                r == PUSHCORE_OK ? "done" : "FAILED", QpcMs(Qpc() - q0), b.firstMsP95[0], b.nextMsP95[0], b.rebuildMsP95[0], b.firstMsP95[1], b.nextMsP95[1], b.rebuildMsP95[1],
                b.firstMsP95[2], b.nextMsP95[2], b.rebuildMsP95[2], b.note));
            if (r == PUSHCORE_OK && b.firstMsP95[1] > 0) {
                const double pix = double(b.width) * b.height / (3840.0 * 2160.0), size = 0.6 + 0.4 * pix;   // to 4K as FgPrior scales it
                HKEY k = nullptr;
                if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\PushMPC\\Learned", 0, nullptr, 0, KEY_WRITE | KEY_READ, nullptr, &k, nullptr) == ERROR_SUCCESS) {
                    auto str = [&](const std::wstring& n, const std::wstring& v) { RegSetValueExW(k, n.c_str(), 0, REG_SZ, (const BYTE*)v.c_str(), DWORD((v.size() + 1) * sizeof(wchar_t))); };
                    if (pm::RegReadStr(k, L"Gpu") != c->gpu) { RegDeleteTreeW(k, nullptr); str(L"Gpu", c->gpu); }
                    str(L"Model", L"3");
                    for (int t = 0; t < 3; ++t) {
                        // a tier that did not run: from Balanced, by the 5090's tier ratios
                        const double ratio[3] = { 4.49 / 5.68, 1.0, 11.96 / 5.68 };
                        const double first = b.firstMsP95[t] > 0 ? b.firstMsP95[t] : b.firstMsP95[1] * ratio[t];
                        const double next = b.nextMsP95[t] > 0 ? b.nextMsP95[t] : b.nextMsP95[1] * ratio[t];
                        str(std::format(L"PfFirst{}", t), std::format(L"{:.3f}", first / size));
                        str(std::format(L"PfNext{}", t), std::format(L"{:.3f}", next / size));
                        // flow once: a measured tier's rebuild, else Balanced's (the same pass at every tier), else the prior
                        const double rb = b.rebuildMsP95[t] > 0 ? b.rebuildMsP95[t] : b.rebuildMsP95[1] > 0 ? b.rebuildMsP95[1] : -1;
                        if (rb > 0) str(std::format(L"PfRebuild{}", t), std::format(L"{:.3f}", rb / size));
                    }
                    if (b.rebuildMsP95[1] > 0) str(L"PfBenchVersion", L"2");   // the rebuild was measured (PushFlow above 4x)
                    str(L"PfBenchNote", Widen(b.note));
                    RegCloseKey(k);
                }
                ++g_benchSerial;
            }
        }
        if (self) FreeLibraryAndExitThread(self, 0);
        return 0;
    };
    HANDLE th = CreateThread(nullptr, 0, proc, ctx, 0, nullptr);
    if (th) CloseHandle(th); else { if (ctx->self) FreeLibrary(ctx->self); delete ctx; }
}
// Refines the cost model from pushcore's GPU timestamps of the running graph (p95 over the samples since the graph was
// built, after 3 s; only full frame generation groups count). Exponential smoothing, so one loaded stretch from another
// program on the GPU moves it only part of the way.
void CPushMpcFilter::LearnCosts()
{
    std::lock_guard<std::mutex> lk(m_autoLock);
    const PushCoreSettings a = m_applied;
    const UINT w = m_oi.width, h = m_oi.height;
    if (!w || !h) return;
    const double pix = pm::Pix(w, h);
    // 3 s after the graph started, and 3 s after a playback-rate change (R-RATE1: the admission's reduced groups and the
    // old rate's frames are still in the timings then and taught Auto that High at 5x costs 8 ms)
    const int64_t from = std::max<int64_t>(m_graphQpc, m_rateQpc.load()) + int64_t(3.0 * QpcFreq());
    std::vector<double> fg, sr, nr, hdr, rest;
    for (const auto& s : m_autoTimings) {
        if (s.qpc < from) continue;
        if (a.multiplier >= 2 && s.fgLevel == 0 && s.t.fgBatchMs > 0) fg.push_back(s.t.fgBatchMs);
        if (a.sr && s.t.srMs > 0) sr.push_back(s.t.srMs);
        if (a.nr && s.t.nrMs > 0) nr.push_back(s.t.nrMs);
        if (a.videoHdr && s.t.videoHdrMs > 0) hdr.push_back(s.t.videoHdrMs);
        // the rest of the graph (colour, conversions, the cut guard, deband): the graph span minus the timed stages, full groups only
        if (s.t.graphMs > 0 && (a.multiplier < 2 || (s.fgLevel == 0 && s.t.fgBatchMs > 0))) {
            double st = 0; for (double x : { s.t.fgBatchMs, s.t.srMs, s.t.nrMs, s.t.videoHdrMs }) if (x > 0) st += x;
            rest.push_back(std::max(0.0, s.t.graphMs - st));
        }
    }
    auto p95 = [](std::vector<double>& v) { std::sort(v.begin(), v.end()); return v[std::min(v.size() - 1, size_t(v.size() * 0.95))]; };
    auto ema = [](double& v, double x) { v = v * 0.5 + x * 0.5; };
    pm::Costs& c = m_costs;
    if (fg.size() >= 48 && KnownFg(m_fgRun)) {
        // what pushcore really runs (after its own fallback), at the applied multiplier (capped for 2x-only methods)
        const pm::FgMethod m = MethodOf(m_fgRun);
        const int q = m == pm::FgMethod::PushFlow ? int(a.fgTier) : m == pm::FgMethod::Dlssg ? (a.fgLite ? pm::kDlssgLiteQ : 0) : int(a.vfgQuality);
        const int mult = std::min<int>(int(a.multiplier), pm::MaxMultiplier(m));
        const double prior = pm::FgPrior(c, m, q, mult, pix);
        const int key = pm::FgKeyOf(m, q);
        // the renderer's work for the shown frames runs on the same GPU meanwhile and stretches the batch: Decide adds it
        // separately (ShowCost), so it is taken out here (learned model 3)
        const double show = pm::ShowCost(c, m_display, InputHdr() && !m_display.hdr, m_coreToneMap, mult, pix, pm::Pix(m_visW ? m_visW : w, m_visH ? m_visH : h));
        const double x = prior > 0 ? std::max(p95(fg) - show, p95(fg) * 0.5) / prior : 0;
        if (std::isfinite(x) && x > 0.05 && x < 20) { if (!c.fgLearned[key]) c.fgScale[key] = x; else ema(c.fgScale[key], x); c.fgLearned[key] = true; }
    }
    if (sr.size() >= 48) {
        pm::Costs prior = c; prior.vsrScale = 1.0; prior.ivsrScale = 1.0; prior.xessScale = 1.0;
        const pm::Upscaler um = a.srMethod == PUSHCORE_SR_METHOD_FSR ? pm::Upscaler::Fsr : a.srMethod == PUSHCORE_SR_METHOD_NIS ? pm::Upscaler::Nis
                              : a.srMethod == PUSHCORE_SR_METHOD_IVSR ? pm::Upscaler::IntelVsr : a.srMethod == PUSHCORE_SR_METHOD_XESS ? pm::Upscaler::Xess : pm::Upscaler::Rtx;
        const double base = pm::VsrCost(prior, int(a.srQuality), m_choice.artifactReduction, pix, int(a.srFamily), um);
        const double x = base > 0 ? p95(sr) / base : 0;
        if (x > 0.1 && x < 10 && um == pm::Upscaler::Rtx) { if (!c.learnedVsr) c.vsrScale = x; else ema(c.vsrScale, x); c.learnedVsr = true; }
        if (x > 0.1 && x < 10 && um == pm::Upscaler::IntelVsr) { if (!c.learnedIvsr) c.ivsrScale = x; else ema(c.ivsrScale, x); c.learnedIvsr = true; }
        if (x > 0.1 && x < 10 && um == pm::Upscaler::Xess) { if (!c.learnedXess) c.xessScale = x; else ema(c.xessScale, x); c.learnedXess = true; }
    }
    if (nr.size() >= 48) { const double x = p95(nr); if (!c.learnedNr) c.nrMs = x; else ema(c.nrMs, x); c.learnedNr = true; }
    // the anime upscaler: its own time per input megapixel on this card (the transfer prior taken off), per model
    if (const int am = m_animeSrModelNow.load(); am >= 0 && m_animeSrMsNow.load() > 0 && m_choice.outW && m_choice.outH) {
        const pm::Source src = SourceInfo();
        const double inMpx = double(src.w) * src.h / 1e6, outMpx = double(m_choice.outW) * m_choice.outH / 1e6;
        const double x = (m_animeSrMsNow.load() - pm::kAnimeSrTransferMsPerMpx * (inMpx + outMpx)) / std::max(1e-3, inMpx);
        if (std::isfinite(x) && x > 0.01 && x < 500) { double& v = c.animeSrLearned[am]; if (v <= 0) v = x; else ema(v, x); }
    }
    if (hdr.size() >= 48) { const double x = p95(hdr) / pix; if (!c.learnedHdr) c.hdr4k = x; else ema(c.hdr4k, x); c.learnedHdr = true; }
    if (rest.size() >= 48) {
        // base4k: what the model adds besides the stages it times (Decide: base4k x size x card, the guard's test and flow, deband)
        const double guard = a.multiplier >= 2 ? c.guardScore * c.card + (m_choice.guardFlow ? c.guardFlow * c.card : 0.0) : 0.0;
        const double deb = a.deband && m_visW && m_visH ? pm::DebandCost(c, int(a.debandStrength), m_visW, m_visH) : 0.0;
        const double x = (p95(rest) - guard - deb) / std::max(1e-3, pix * c.card);
        if (std::isfinite(x)) { const double v = std::clamp(x, 0.2, 20.0); if (!c.learnedBase) c.base4k = v; else ema(c.base4k, v); c.learnedBase = true; }
    }
    // deband: pushcore's median of the last 64 passes, back to the medium-strength 1080p equivalent (x1.3 for the p95)
    if (a.deband && m_debandMsNow.load() > 0 && m_visW && m_visH) {
        const double unit = pm::DebandCost(pm::Costs{}, int(a.debandStrength), m_visW, m_visH) / pm::Costs{}.deband1080;
        const double x = m_debandMsNow.load() * 1.3 / std::max(1e-3, unit);
        if (std::isfinite(x) && x > 0.001 && x < 20) { if (!c.learnedDeband) c.deband1080 = x; else ema(c.deband1080, x); c.learnedDeband = true; }
    }
    // the GPU time per source frame now (median of the last second), for the status
    std::vector<double> now;
    const int64_t sec = Qpc() - int64_t(QpcFreq());
    for (const auto& s : m_autoTimings) {
        if (s.qpc < sec) continue;
        double v = 0;
        for (double x : { s.t.colorMs, s.t.srMs, s.t.fgBatchMs, s.t.nrMs, s.t.videoHdrMs }) if (x > 0) v += x;
        if (v > 0) now.push_back(v);
    }
    if (!now.empty()) { std::nth_element(now.begin(), now.begin() + now.size() / 2, now.end()); m_gpuMsNow = now[now.size() / 2]; }
    else m_gpuMsNow = -1;
}
// The source frame interval from the timestamps (F8, F9): pm::StepTracker (PushMpcAuto.h; the headless check runs it).
// The first value goes to OnSourceRateMeasured: while the open is still running and that interval changes Auto's
// choice, the open is redone for it (a reopen is ~0.7 s of source frames; the old way was the wrong graph, then a
// 2.8-s rebuild). Afterwards a rate change only divides by the new rate (NewSegment).
void CPushMpcFilter::TrackPts(REFERENCE_TIME pts, bool disc)
{
    if (m_trackReset.exchange(false)) m_tracker.Reset();
    const double rate = m_rate.load() > 0 ? m_rate.load() : 1.0;
    const double was = m_tracker.base;
    auto ev = m_tracker.Add(pts, disc, rate);
    if (ev == pm::StepTracker::None) return;
    if (ev == pm::StepTracker::First && m_atpf > 0 && std::fabs(m_tracker.base - m_atpf / 1e4) / (m_atpf / 1e4) < 0.01)
        m_tracker.base = m_atpf / 1e4;   // the timestamps agree with the media type: its exact frame time (Matroska rounds to 1 ms)
    m_baseStepMs = m_tracker.base;
    m_ptsStepMs = m_tracker.Step(rate);
    if (ev == pm::StepTracker::First) {
        Log(std::format("source frame interval {:.3f} ms ({:.3f} fps at {}x), the median of the first {} timestamp steps; the media type says {:.3f} fps",
            m_tracker.Step(rate), 1000.0 / m_tracker.Step(rate), rate, pm::StepTracker::kFirst, 1e7 / double(m_atpf)));
        OnSourceRateMeasured();
    } else {
        Log(std::format("source frame interval re-measured: {:.3f} ms at {}x ({} windows of {} steps off by more than 10 %; was {:.3f} ms)",
            m_tracker.Step(rate), rate, 3, pm::StepTracker::kWindow, was / rate));
    }
}
// the first measured interval: while pushcore is still opening, an Auto choice that differs from the open's is opened
// again for it (no rebuild right after activation); once active, the Auto thread re-decides once (its 3 % rule)
void CPushMpcFilter::OnSourceRateMeasured()
{
    if (m_mode != Mode::Warmup || !m_openStarted || LegacyMode() || m_openFps <= 0) return;
    const double fps = SourceInfo().fps;
    if (std::fabs(fps - m_openFps) / m_openFps <= 0.03) return;
    const pm::Choice c = DecideNow(m_cfg.user, m_display);
    const PushCoreSettings next = ToCore(m_cfg.user, c);
    PushCoreSettings opened;
    { std::lock_guard<std::mutex> lk(m_autoLock); opened = m_applied; }
    if (SameGraph(next, opened)) {
        { std::lock_guard<std::mutex> lk(m_autoLock); m_appliedFps = fps; m_choice = c; }
        Log(std::format("the timestamps say {:.3f} fps (the open used {:.3f}): the same choice, the open goes on", fps, m_openFps));
        m_openFps = fps;
        return;
    }
    Log(std::format("the timestamps say {:.3f} fps, the open was for {:.3f} fps and Auto chooses differently ({} instead of {}): opened again now",
        fps, m_openFps, Narrow(GraphKey(next)), Narrow(GraphKey(opened))));
    // at once, alongside the running open (abandoned): measured 2.9 s to active; waiting for the first open to finish
    // and then reopening took 5.3 s (m_reopenForRate, kept for a core whose open cannot be abandoned)
    CloseCore();
    m_openStarted = false;
    StartOpen(nullptr);
}
// The file the graph plays (its source filter), lower case; empty when none says
std::wstring CPushMpcFilter::SourceFile()
{
    std::wstring f;
    if (!m_pGraph) return f;
    ComPtr<IEnumFilters> e;
    if (FAILED(m_pGraph->EnumFilters(&e)) || !e) return f;
    ComPtr<IBaseFilter> b;
    while (f.empty() && e->Next(1, &b, nullptr) == S_OK) {
        ComPtr<IFileSourceFilter> fs;
        LPOLESTR name = nullptr;
        if (SUCCEEDED(b.As(&fs)) && SUCCEEDED(fs->GetCurFile(&name, nullptr)) && name) { f = name; CoTaskMemFree(name); }
        b.Reset();
    }
    for (auto& c : f) c = wchar_t(towlower(c));
    return f;
}
// Interlacing from the samples (F6): with AMINTERLACE_IsInterlaced on the type, LAV marks each sample: a progressive
// frame carries AM_VIDEO_FLAG_WEAVE, an interlaced one does not (it carries the field order, AM_VIDEO_FLAG_FIELD1FIRST),
// a single field has AM_VIDEO_FLAG_FIELD1/FIELD2. Decided on the first 8 samples (half or more interlaced: interlaced).
// Interlaced: no PushFPS (the frames are converted at the source rate meanwhile), the file is remembered for this
// MPC-HC process and MPC-HC reopens it, so the next connection takes the source format and MPC Video Renderer
// deinterlaces it (the connected output type never changes at run time).
void CPushMpcFilter::CheckInterlace(IMediaSample* pIn)
{
    // The flags are a prior only (logged): LAV's type flag and its per-sample weave / field flags come from the bitstream,
    // and encoders set them on progressive pictures too (RC1 F6: progressive SD XviD). The verdict is the picture's own
    // field combing on moving areas (CombMeter: 24 moving frames, half or more combed = interlaced).
    DWORD flags = 0; bool have = false;
    ComPtr<IMediaSample2> s2;
    if (SUCCEEDED(pIn->QueryInterface(__uuidof(IMediaSample2), (void**)&s2)) && s2) {
        AM_SAMPLE2_PROPERTIES pr = {};
        if (SUCCEEDED(s2->GetProperties(sizeof(pr), (BYTE*)&pr))) { flags = pr.dwTypeSpecificFlags; have = true; }
    }
    const bool field = (flags & AM_VIDEO_FLAG_FIELD_MASK) != 0;
    const bool il = have ? (field || !(flags & AM_VIDEO_FLAG_WEAVE)) : (m_typeIlFlags & AMINTERLACE_DisplayModeMask) != AMINTERLACE_DisplayModeWeaveOnly;
    if (m_ilSeen < 3) Log(std::format("sample {}: type interlace 0x{:X}, sample flags {} -> {} (a prior; the picture decides)", m_ilSeen, m_typeIlFlags,
                                      have ? std::format("0x{:X}", flags) : std::string("none"), il ? (field ? "a field" : "an interlaced frame") : "a progressive frame (weave)"));
    m_ilSeen++;
    if (il && m_typeInterlaced) m_ilCount++;
    BYTE* src = nullptr;
    if (FAILED(pIn->GetPointer(&src)) || !src || pIn->GetActualDataLength() < LONG(m_inStride * m_inH)) return;
    using pushmpc::content::CombMeter;
    const CombMeter::Verdict v = m_inP010 ? m_comb.Push(reinterpret_cast<const uint16_t*>(src), ptrdiff_t(m_inStride / 2), int(m_inW), int(m_inH), 8)
                                          : m_comb.Push(src, ptrdiff_t(m_inStride), int(m_inW), int(m_inH), 0);
    // a still picture shows no combing whatever its fields: after ~10 s without enough motion it counts as progressive
    if (v == CombMeter::kMeasuring && m_ilSeen < 240) return;
    m_ilDecided = true;
    if (v != CombMeter::kInterlaced) {
        Log(std::format("progressive (measured): {} of {} moving frames combed{}; flags said {} of {} samples interlaced", m_comb.combed(), m_comb.measured(),
                        v == CombMeter::kMeasuring ? " (too little motion to tell: treated as progressive)" : "", m_ilCount, m_ilSeen));
        return;
    }
    const std::wstring file = SourceFile();
    if (!file.empty()) { std::lock_guard<std::mutex> lk(g_ilLock); g_interlacedFiles.insert(file); }
    const std::wstring why = std::format(L"interlaced input (field combing measured on {} of {} moving frames)", m_comb.combed(), m_comb.measured());
    LogW(L"interlaced: " + why);
    if (m_mode == Mode::Warmup || m_mode == Mode::Active) {
        CloseCore();
        Fallback(why + (file.empty() || !pm::HostCanReopen(pm::HostPlayer()) ? std::wstring() : std::wstring(L"; ") + pm::HostName() + L" reopens the file so that MPC Video Renderer deinterlaces it"));
        if (!file.empty() && !RequestReopen()) SetStatus(L"no PushFPS (source frames converted by the filter, not deinterlaced): " + why);
    }
}
// ContentDetect on every input sample (streaming thread): the luma / chroma cell grid of the system-memory sample
// (NV12 or P010, ~0.5 ms at 1080p), its timestamp; the class goes to Auto through m_contentClass (no names, no flags)
void CPushMpcFilter::MeasureContent(IMediaSample* pIn, REFERENCE_TIME pts, bool disc)
{
    if (m_contentDisc.exchange(false) || disc) m_content.Discontinuity();
    BYTE* src = nullptr;
    if (FAILED(pIn->GetPointer(&src)) || !src || pIn->GetActualDataLength() < LONG(m_inStride * m_inH * 3 / 2) || m_inW < 16 || m_inH < 16) return;
    using namespace pushmpc::content;
    const bool limited = true;   // LAV's NV12 / P010 output is video range
    const Cells c = m_inP010
        ? CellsFromYuv(reinterpret_cast<const uint16_t*>(src), ptrdiff_t(m_inStride / 2), reinterpret_cast<const uint16_t*>(src + size_t(m_inStride) * m_inH), static_cast<const uint16_t*>(nullptr),
                       ptrdiff_t(m_inStride / 2), int(m_inW), int(m_inH), 16, limited)
        : CellsFromYuv(static_cast<const uint8_t*>(src), ptrdiff_t(m_inStride), static_cast<const uint8_t*>(src + size_t(m_inStride) * m_inH), static_cast<const uint8_t*>(nullptr),
                       ptrdiff_t(m_inStride), int(m_inW), int(m_inH), 8, limited);
    if (!m_content.Push(c, double(pts) / 1e7)) return;
    const State& st = m_content.Current();
    m_contentConf = st.confidence;
    if (st.cls != m_contentClass.load()) {
        m_contentClass = st.cls;
        // the Auto thread re-decides on its next tick (<= 1 s); the class changes at most once per 2 s (hysteresis)
    }
    if (st.cls != m_contentLogged) {
        m_contentLogged = st.cls;
        Log(std::format("content: {} ({:.0f} %), measured in the picture at {:.1f} s; probabilities drawn {:.2f} CG {:.2f} live {:.2f} game {:.2f}; class changes so far {}",
                        ClassName(st.cls), 100.0 * st.confidence, double(pts) / 1e7, st.prob[kDrawn], st.prob[kCg], st.prob[kLive], st.prob[kGame], st.changes));
    }
}
// The user's own animation-mode choice for this show (a preference store addressed by the series folder; nothing is
// detected from the path: the picture's class comes from ContentDetect)
void CPushMpcFilter::ShowChoiceOnce()
{
    if (m_showLooked) return;
    m_showLooked = true;
    m_showKey = pm::ShowKeyFor(SourceFile());
    if (!m_showKey.empty()) {
        m_showChoice = pm::ReadShow(m_showKey).choice;
        pm::SetCurrentShow(m_showKey);
        if (m_showChoice.load() >= 0) Log(std::format("animation mode: the user's choice for this show is {}", m_showChoice.load()));
    }
}
void CPushMpcFilter::StartAuto()
{
    ShowChoiceOnce();
    if (m_autoThread.joinable()) return;
    m_hAutoStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_autoThread = std::thread([this] { AutoLoop(); });
}
void CPushMpcFilter::StopAuto()
{
    if (m_hAutoStop) SetEvent(m_hAutoStop);
    if (m_autoThread.joinable()) m_autoThread.join();
    if (m_hAutoStop) { CloseHandle(m_hAutoStop); m_hAutoStop = nullptr; }
    SaveLearned();
}
// Once a second: the settings (the page or the registry), the window's display (monitor, refresh rate, Windows HDR, VRR),
// the playback rate, what pushcore really runs and the learned costs. A change of the settings, the display or the rate
// applies at once; a choice that only the measured costs change applies after 10 s on the current graph, at most every
// 20 s, an upgrade only with 5 % to spare, and an upgrade after which the admission (the step-down safety net) had to step
// down within 10 s is taken back and not tried again for 5 minutes. Applying = pushcore_setSettings + the reload path.
void CPushMpcFilter::AutoLoop()
{
    HWND wnd = nullptr;
    uint32_t tick = 0, benchSeen = g_benchSerial.load();
    uint64_t lastDelivered = m_delivered.load();
    int64_t lastQpc = Qpc();
    PushCoreSettings beforeUpgrade = {};
    uint32_t rateSeen = m_rateSerial.load(), rateWoke = rateSeen;
    // one tick a second; a playback-rate change wakes it within 50 ms (round 2, F9: it waited up to 1 s on the old graph)
    auto tickWait = [&]() {
        for (int i = 0; i < 20; ++i) {
            if (WaitForSingleObject(m_hAutoStop, 50) != WAIT_TIMEOUT) return false;
            if (m_rateSerial.load() != rateWoke) { rateWoke = m_rateSerial.load(); return true; }
        }
        return true;
    };
    while (tickWait()) {
        ++tick;
        if (m_quickPhase) continue;   // fast start: Auto measures and decides on the full graph, after the handover
        if (!wnd || !IsWindow(wnd)) wnd = pm::PlayerWindow();
        pm::Settings s;
        { std::lock_guard<std::mutex> lk(m_autoLock); s = m_appliedSettings; }
        const uint32_t serial = g_settingsSerial.load();
        if (serial != m_settingsSeen || tick % 2 == 0) {
            m_settingsSeen = serial;
            s = pm::LoadSettings();
            std::wstring e; if (EnvStr(L"Enable", e)) s.enable = _wtoi(e.c_str()) != 0;
        }
        if (g_benchSerial.load() != benchSeen) { benchSeen = g_benchSerial.load(); m_learnedLoaded = false; LoadLearned(); m_lastCostChangeQpc = 0; }
        pm::Display d = pm::QueryDisplay(wnd, s.vrr, m_gpu.nvidia());
        {
            // the deband pass's measured cost and what pushcore runs (its own fallback when a method did not start)
            PushCoreStats st = { sizeof(st) };
            bool ok = false;
            { std::lock_guard<std::mutex> lk(m_coreLock); if (m_core && !m_faulted) ok = m_api->stats(m_core, &st) == PUSHCORE_OK; }
            m_debandMsNow = ok && st.debandActive ? st.debandMs : -1.0;
            const bool animeSrRuns = ok && (g_coreFeatures & PUSHCORE_FEATURE_ANIME_SR) && st.animeSrModel < uint32_t(pm::kAnimeSrModels) && st.animeSrMs > 0;
            m_animeSrModelNow = animeSrRuns ? int(st.animeSrModel) : -1; m_animeSrMsNow = animeSrRuns ? double(st.animeSrMs) : -1.0;
            if (ok && (g_coreFeatures & PUSHCORE_FEATURE_FG_METHODS)) {
                const bool changed = st.fgBackend != m_fgRun;
                m_fgRun = st.fgBackend; m_srRun = st.srMethod; m_cutGuardRun = st.cutGuardActive != 0; m_fgRunNote = Widen(st.fgNote);
                if (g_coreFeatures & PUSHCORE_FEATURE_SUBTITLE_HOLD) { m_subtitleState = st.subtitleHoldState; m_subtitlePixels = st.subtitleHoldPixels; }
                if ((g_coreFeatures & PUSHCORE_FEATURE_HELD_FRAMES) && st.heldActive && st.heldTested >= 96) {
                    // ~4 s of frames tested: the share of held drawings (the latest: ContentDetect's per-shot class with
                    // its hysteresis decides first; this is the fallback until it has decided)
                    const double share = st.heldRatioPermille / 1000.0;
                    m_heldShare = share;
                    if (!m_heldLogged && share >= pm::kAnimationHeldShare) {
                        m_heldLogged = true;
                        Log(std::format("held drawings: {:.1f} % of the last {} frames (animation); re-timed spans {}, held frames given no graph pair {}, graph {}x",
                                        share * 100.0, st.heldTested, st.heldSpans, st.heldDropped, st.heldGraphMultiplier));
                    }
                }
                if (changed && st.fgFallback) {
                    // the method Auto asked for did not start on this GPU: not offered again in this session
                    PushCoreSettings asked;
                    { std::lock_guard<std::mutex> lk(m_autoLock); asked = m_applied; }
                    const pm::FgMethod bad = MethodOf(asked.fgBackend);
                    if (bad != pm::FgMethod::Auto && asked.fgBackend != st.fgBackend) {
                        std::lock_guard<std::mutex> lk(m_autoLock);
                        m_gpu.available &= ~(1u << pm::CapOf(bad));
                        m_gpu.reason[pm::CapOf(bad)] = L"did not start here (" + m_fgRunNote + L")";
                        { std::lock_guard<std::mutex> sl(g_statusLock); g_gpu = m_gpu; }
                        LogW(std::format(L"Auto: {} did not start ({}); pushcore runs {}; not chosen again in this session", pm::FgMethodLabel(bad), m_fgRunNote,
                                         pm::FgMethodLabel(MethodOf(st.fgBackend))));
                    }
                }
            }
        }
        LearnCosts();
        if (tick % 30 == 0) SaveLearned();
        if (m_mode == Mode::Active && s.smoother == pm::kAuto) {
            // the delivery check: generated frames late or dropped at this multiplier although the GPU timings fit -> one
            // step lower for the rest of this file (DeliveryOverloaded); windows within 5 s of a start, rebuild or seek do not count
            const int64_t nq = Qpc();
            const uint64_t late = m_dropGen + m_lateDelivered, gen = m_delGen;
            int64_t gq; PushCoreSettings ap; { std::lock_guard<std::mutex> lk(m_autoLock); gq = m_graphQpc; ap = m_applied; }
            const bool settle = QpcMs(nq - std::max({ gq, m_flushStartQpc, m_activeQpc })) < 5000.0 || m_fgRun > PUSHCORE_FG_NVOF;
            if (!m_capWinQpc || settle) { m_capWinQpc = nq; m_capLate = late; m_capGen = gen; }
            else if (QpcMs(nq - m_capWinQpc) >= 10000.0) {
                const uint64_t dl = late - m_capLate, dg = gen - m_capGen;
                const bool lateNow = pm::DeliveryOverloaded(dl, dg);
                const bool over = lateNow && ap.multiplier > 2 && (!m_capState.cap || int(ap.multiplier) <= m_capState.cap);
                const int was = m_capState.cap;
                if (lateNow && !over) m_capState.cleanMs = 0;   // late, but nothing to lower: not a clean stretch either
                else if (m_capState.Update(over, int(ap.multiplier), QpcMs(nq - m_capWinQpc))) {
                    if (over) Log(std::format("Auto: {} of {} generated frames came late or were dropped in 10 s at {}x although the GPU timings fit: at most {}x for now", dl, dg, ap.multiplier, m_capState.cap));
                    else Log(std::format("Auto: delivery clean for a while at most {}x: {}", was, m_capState.cap ? std::format("up to {}x again", m_capState.cap) : std::string("no cap")));
                }
                m_autoCap = m_capState.cap;
                m_capWinQpc = nq; m_capLate = late; m_capGen = gen;
            }
        }
        s.autoCap = m_autoCap;
        const pm::Source src = SourceInfo();
        pm::Choice c;
        bool weakPf = false;   // a weak GPU with PushFlow installed (its speed test decides PushFlow vs FSR FG)
        {
            pm::Costs costs; pm::Gpu g;
            { std::lock_guard<std::mutex> lk(m_autoLock); costs = m_costs; g = m_gpu; m_display = d; }
            weakPf = pm::IsWeakGpu(g.family, g.name) && g.has(pm::kCapPushFlow);
            c = pm::Decide(s, src, d, costs, g);
        }
        PublishNotes(s, c);
        // the speed test also runs on the cold start of a weak GPU (FSR FG first): its result decides PushFlow vs FSR FG
        if (c.multiplier >= 2 && (c.fgMethod == pm::FgMethod::PushFlow || weakPf)) MaybeBenchPushFlow(m_oi.width ? m_oi.width : c.outW, m_oi.height ? m_oi.height : c.outH);
        // the live lines of the page: rate, GPU time, what runs (method, quality, cut guard), VRR, where Auto stopped
        const int64_t q = Qpc();
        const uint64_t del = m_delivered.load();
        const double fps = (del - lastDelivered) / std::max(1e-3, QpcMs(q - lastQpc) / 1000.0);
        lastDelivered = del; lastQpc = q;
        const double interval = 1000.0 / std::max(1.0, src.fps);
        const std::wstring gpu = m_gpuMsNow > 0 ? std::format(L"GPU {:.1f} ms per source frame ({:.0f} % of {:.1f} ms)", m_gpuMsNow, 100.0 * m_gpuMsNow / interval, interval) : L"GPU time not measured yet";
        PushCoreSettings applied;
        { std::lock_guard<std::mutex> lk(m_autoLock); applied = m_applied; }
        std::wstring running = L"not running";
        if (m_mode == Mode::Active) {
            const pm::FgMethod rm = MethodOf(m_fgRun);
            const int rq = rm == pm::FgMethod::PushFlow ? int(applied.fgTier) : rm == pm::FgMethod::Dlssg ? (applied.fgLite ? pm::kDlssgLiteQ : 0) : int(applied.vfgQuality);
            running = !KnownFg(m_fgRun) || applied.multiplier < 2 ? std::wstring(L"off")
                    : std::format(L"{} {}x{}, cut guard {}", pm::FgMethodLabel(rm), std::min<int>(int(applied.multiplier), pm::MaxMultiplier(rm)),
                                  *pm::QualityName(rm, rq) ? std::wstring(L" ") + pm::QualityName(rm, rq) : std::wstring(), m_cutGuardRun ? (c.guardNote.empty() ? std::wstring(L"on") : L"on, " + c.guardNote) : std::wstring(L"off"));
            if (applied.multiplier >= 2 && KnownFg(m_fgRun) && m_subtitleState)
                running += m_subtitleState == 1 ? (m_subtitlePixels ? std::format(L", subtitle hold on ({} px held)", m_subtitlePixels) : std::wstring(L", subtitle hold on"))
                         : m_subtitleState == 2 ? std::wstring(L", subtitle hold unavailable") : std::wstring(L", subtitle hold not with this method");
            if (applied.multiplier >= 2 && m_fgRun <= PUSHCORE_FG_NVOF && applied.heldFrames)
                running += c.animation ? L", animation: " + c.animNote : std::wstring(L", held frames re-timed");
            if (!m_fgRunNote.empty()) running += L" (" + m_fgRunNote + L")";
        }
        {
            // the OSD's line: what runs, the output rate, SDR to HDR / VRR / battery when they apply
            std::wstring osd = L"PushMPC";
            if (!s.enable) osd += L" \u00B7 off";
            else if (m_mode == Mode::Active) {
                const pm::FgMethod rm = MethodOf(m_fgRun);
                const int rq = rm == pm::FgMethod::PushFlow ? int(applied.fgTier) : rm == pm::FgMethod::Dlssg ? (applied.fgLite ? pm::kDlssgLiteQ : 0) : int(applied.vfgQuality);
                if (applied.multiplier >= 2 && KnownFg(m_fgRun)) osd += std::format(L" \u00B7 {}x {}", applied.multiplier, pm::QualityName(rm, rq));
                if (c.sr) osd += c.srMethod == pm::Upscaler::Rtx ? std::format(L" \u00B7 VSR {}", c.srLevel)
                               : c.srMethod == pm::Upscaler::Anime && c.animeModel >= 0 ? std::wstring(L" \u00B7 ") + pm::AnimeSrModel(c.animeModel).label
                               : std::wstring(c.srMethod == pm::Upscaler::Fsr ? L" \u00B7 FSR" : c.srMethod == pm::Upscaler::Xess ? L" \u00B7 XeSS"
                                              : c.srMethod == pm::Upscaler::IntelVsr ? L" \u00B7 Intel VSR" : L" \u00B7 NIS");
                osd += std::format(L" \u00B7 {:.0f} fps", fps);
                if (c.hdr) osd += L" \u00B7 SDR\u2192HDR";
                if (d.vrr) osd += L" \u00B7 VRR";
                if (d.battery) osd += L" \u00B7 battery";
            } else {
                std::wstring why; { std::lock_guard<std::mutex> lk(g_statusLock); why = g_page.state; }
                if (why.size() > 48) why = why.substr(0, 47) + L"\u2026";
                osd += L" \u00B7 off: " + why;
            }
            bool first = false;
            { std::lock_guard<std::mutex> ol(m_osdLock); first = m_osdText.empty(); m_osdText = osd; }
            // Brief: the full three seconds count from when there is something to show
            if (first && QpcMs(Qpc()) < m_osdUntilMs.load()) OsdBrief();
        }
        // the updater (PushMpcUpdater.exe --auto): once per 24 h, 20 s into stable playback, never during start-up
        if (m_mode == Mode::Active && !m_updateTried && m_activeQpc && QpcMs(Qpc() - m_activeQpc) > 20000.0 && m_fgLevel == 0) {
            m_updateTried = true;
            if (pm::MaybeAutoUpdate(LocalAppData())) Log("updater started (--auto, low priority)");
        }
        SetLive(std::format(L"{:.1f} fps out ({:.3f} fps source){}\r\n{}\r\nFrame generation: {}; {}\r\n{} at {:.2f} Hz{}; VRR: {} ({})\r\nAuto stopped at: {}\r\nDropped frames: {} generated, {} real{}",
            fps, src.fps, m_fgLevel > 0 ? (m_fgLevel == 1 ? L", GPU busy: 2x for now" : L", GPU busy: no generated frames for now") : L"", gpu,
            running, m_gpu.name.empty() ? std::wstring(L"GPU unknown") : m_gpu.name + L" (" + pm::FamilyName(m_gpu.family) + L")",
            d.name, d.refreshHz, d.hdr ? L", Windows HDR on" : L"", d.vrr ? L"on" : L"off", d.vrrHow,
            c.stoppedAt.empty() ? L"-" : c.stoppedAt, m_dropGen, m_dropReal, c.batteryNote.empty() ? std::wstring() : L"\r\n" + c.batteryNote));

        if (m_mode != Mode::Active || LegacyMode() || m_reloading || m_faulted || m_settingsPending) continue;
        PushCoreSettings next = ToCore(s, c), cur;
        pm::Settings appliedS; pm::Display appliedD; double appliedFps;
        {
            std::lock_guard<std::mutex> lk(m_autoLock);
            cur = m_applied; appliedS = m_appliedSettings; appliedD = m_appliedDisplay; appliedFps = m_appliedFps;
        }
        // an upgrade the GPU could not hold: back, and blocked for 5 minutes
        if (m_lastUpgradeQpc && QpcMs(q - m_lastUpgradeQpc) < 10000.0 && QpcMs(q - m_lastUpgradeQpc) > 2500.0 && m_fgLevel > 0) {
            m_failedUpgrades[m_lastUpgradeKey] = q;
            Log(std::format("Auto: the upgrade to {} did not hold (frame generation had to step down): back to {}, not tried again for 5 minutes", Narrow(m_lastUpgradeKey), Narrow(GraphKey(beforeUpgrade))));
            m_lastUpgradeQpc = 0;
            { std::lock_guard<std::mutex> lk(m_autoLock); m_pending = beforeUpgrade; m_applied = beforeUpgrade; m_choice = c; }
            m_lastCostChangeQpc = q;
            m_settingsPending = true; SetEvent(m_hSubmit);
            continue;
        }
        const bool inputs = !(s == appliedS) || !(d == appliedD) || (appliedFps > 0 && std::fabs(src.fps - appliedFps) / appliedFps > 0.03);
        const uint32_t rateSerial = m_rateSerial.load();
        const char* why = !(s == appliedS) ? (s.autoCap != appliedS.autoCap ? "delivery check" : "settings") : !(d == appliedD) ? "display" : rateSerial != rateSeen ? "playback rate" : "measured frame rate";
        const int rateKey = int(std::lround(m_rate.load() * 100.0));
        if (inputs && rateSerial != rateSeen && s == appliedS && d == appliedD) {
            // R-RATE1: back at a rate this playback ran before, with the same settings and display: its graph again (it
            // was measured there), not a decision from the costs learned at the other rate
            if (auto it = m_rateGraphs.find(rateKey); it != m_rateGraphs.end() && it->second.settings == s && it->second.display == d) {
                next = it->second.graph; c = it->second.choice;
                why = "playback rate (the graph this rate ran before)";
            }
        }
        if (inputs) { std::lock_guard<std::mutex> lk(m_autoLock); m_appliedSettings = s; m_appliedDisplay = d; m_appliedFps = src.fps; rateSeen = rateSerial; }
        if (inputs) { bool hdrFmt = false; { std::lock_guard<std::mutex> lk(m_fmtLock); hdrFmt = IsHdrFmt(m_outFmt.fmt); } SetItm(c, c.hdr && hdrFmt && !InputHdr()); m_sceneCll = InputHdr() && hdrFmt && !d.hdr; if (d.hdr) ConfigureMpcVr(); }   // the boost needs no rebuild
        // the display's reported peak changes with its mode (some monitors): re-read every 5 s, re-apply when it moved
        if (!m_peakCheckQpc || QpcMs(q - m_peakCheckQpc) >= 5000.0) {
            m_peakCheckQpc = q;
            float pk = 0, fl = 0; const float was = dxgiPeak_, wasFull = dxgiFull_;
            ItmDisplay(pk, fl);
            if (was > 0 && (std::fabs(dxgiPeak_ - was) >= 1.0f || std::fabs(dxgiFull_ - wasFull) >= 1.0f)) {
                Log(std::format("display peak changed: DXGI MaxLuminance {:.0f} -> {:.0f}, MaxFullFrameLuminance {:.0f} -> {:.0f}", was, dxgiPeak_, wasFull, dxgiFull_));
                bool hdrFmt = false; { std::lock_guard<std::mutex> lk(m_fmtLock); hdrFmt = IsHdrFmt(m_outFmt.fmt); }
                SetItm(c, c.hdr && hdrFmt && !InputHdr());
                if (d.hdr) ConfigureMpcVr();
            }
        }
        if (SameGraph(next, cur)) {
            std::lock_guard<std::mutex> lk(m_autoLock); m_choice = c;
            // R-RATE1: the graph this rate holds (steady, full groups): kept for a later return to this rate
            if (!inputs && rateSerial == rateSeen && m_fgLevel == 0) m_rateGraphs[rateKey] = { cur, c, s, d };
            continue;
        }
        const bool up = IsUpgrade(cur, next);
        if (!inputs) {
            // measured costs only
            int64_t graphQpc;
            { std::lock_guard<std::mutex> lk(m_autoLock); graphQpc = m_graphQpc; }
            if (QpcMs(q - graphQpc) < 10000.0 || (m_lastCostChangeQpc && QpcMs(q - m_lastCostChangeQpc) < 20000.0)) continue;
            if (up) {
                if (c.predictedMs > c.budgetMs * 0.95) continue;
                if (auto it = m_failedUpgrades.find(GraphKey(next)); it != m_failedUpgrades.end() && QpcMs(q - it->second) < 300000.0) continue;
            }
            why = up ? "measured GPU cost: room for more" : "measured GPU cost: over budget";
            m_lastCostChangeQpc = q;
        }
        if (up && !inputs) { beforeUpgrade = cur; m_lastUpgradeQpc = q; m_lastUpgradeKey = GraphKey(next); } else m_lastUpgradeQpc = 0;
        { bool hdrFmt = false; { std::lock_guard<std::mutex> lk(m_fmtLock); hdrFmt = IsHdrFmt(m_outFmt.fmt); } SetItm(c, c.hdr && hdrFmt && !InputHdr()); }
        LogChoice(why, c);
        Log(std::format("Auto: applying {} (was {})", Narrow(GraphKey(next)), Narrow(GraphKey(cur))));
        OsdBrief();
        { std::lock_guard<std::mutex> lk(m_autoLock); m_pending = next; m_applied = next; m_choice = c; }
        m_settingsPending = true;
        SetEvent(m_hSubmit);
    }
}

// ------------------------------------------------------------------------------------------------ settings: watch, apply, reload
void CPushMpcFilter::StartWatcher()
{
    if (m_watcher.joinable() || m_rt.dataDir.empty() || !DirExists(m_rt.dataDir)) return;
    { std::lock_guard<std::mutex> lk(m_sigLock); m_settingsSig = SettingsSignature(m_rt.dataDir); }
    m_hWatchStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_watcher = std::thread([this] { WatcherLoop(); });
}
void CPushMpcFilter::StopWatcher()
{
    if (m_hWatchStop) SetEvent(m_hWatchStop);
    if (m_watcher.joinable()) m_watcher.join();
    if (m_hWatchStop) { CloseHandle(m_hWatchStop); m_hWatchStop = nullptr; }
}
void CPushMpcFilter::WatcherLoop()
{
    const std::wstring dir = m_rt.dataDir;
    HANDLE h = FindFirstChangeNotificationW(dir.c_str(), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE);
    if (h == INVALID_HANDLE_VALUE) { Log(std::format("settings watch: FindFirstChangeNotification failed {}", GetLastError())); return; }
    LogW(L"settings watch: " + dir + L"\\chain-session.v1*");
    HANDLE hs[2] = { m_hWatchStop, h };
    for (;;) {
        DWORD w = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
        if (w != WAIT_OBJECT_0 + 1) break;
        FindNextChangeNotification(h);
        while ((w = WaitForMultipleObjects(2, hs, FALSE, 400)) == WAIT_OBJECT_0 + 1) FindNextChangeNotification(h);
        if (w == WAIT_OBJECT_0) break;
        const std::wstring sig = SettingsSignature(dir);
        bool changed = false;
        { std::lock_guard<std::mutex> lk(m_sigLock); if (sig != m_settingsSig) { m_settingsSig = sig; changed = true; } }
        if (changed) { LogW(L"settings watch: chain session changed: " + sig); m_reloadWanted = true; SetEvent(m_hSubmit); }
    }
    FindCloseChangeNotification(h);
}
// Copier thread. First PushFPS's live path (pushcore_applySettings: no rebuild, nothing dropped); a change of the
// graph's shape rebuilds (1-3 s, more when the NGX key changes) while Receive delivers the source frames converted.
void CPushMpcFilter::DoReload(bool forced)
{
    const int64_t t0 = Qpc();
    if (!forced) {
        int lr = PUSHCORE_NEEDS_REBUILD;
        { std::lock_guard<std::mutex> lk(m_coreLock); if (m_core && m_api->applySettings) lr = m_api->applySettings(m_core); }
        if (lr == PUSHCORE_OK) {
            ++m_reloads;
            Log(std::format("settings applied live in {:.1f} ms, no rebuild", QpcMs(Qpc() - t0)));
            SetStatus(L"active: " + Widen(m_oi.description) + std::format(L" (settings applied {}x)", m_reloads));
            return;
        }
    }
    m_reloading = true;
    WakeAll();
    // pushcore frames still queued are later than the source frames shown during the rebuild: drop them
    {
        std::deque<ReadyItem> keep, drop;
        {
            std::lock_guard<std::mutex> lk(m_readyLock);
            for (auto& it : m_ready) (it.kind == 0 || it.kind == 1 ? drop : keep).push_back(it);
            m_ready.swap(keep);
        }
        for (auto& it : drop) if (it.sample) it.sample->Release();
    }
    DrainCore(false);
    { std::lock_guard<std::mutex> sl(m_sideLock); m_sideByPts.clear(); }
    int r = PUSHCORE_ERROR;
    PushCoreOutputInfo oi = {};
    {
        std::lock_guard<std::mutex> lk(m_coreLock);
        if (m_core) { r = m_api->reload(m_core); if (r == PUSHCORE_OK) r = m_api->outputInfo(m_core, &oi); else Log(std::format("pushcore_reload -> {} {}", r, m_api->lastError(m_core))); }
        m_coreDisc = true;
        m_lastSubmitPts = INT64_MIN;
    }
    if (r == PUSHCORE_OK) {
        m_oi = oi;
        m_mult = oi.multiplier ? oi.multiplier : 1;
        { std::lock_guard<std::mutex> l2(m_srvLock); m_srvs.clear(); }
        { OutFmt cur; { std::lock_guard<std::mutex> lk(m_fmtLock); cur = m_outFmt; } RequestOutFmt({ cur.fmt, oi.width, oi.height }, "settings reload (size only)"); }
        ++m_reloads;
        m_admHistN = 0; m_admGrace = 2;
        Log(std::format("settings reloaded in {:.0f} ms: {}; output {}x{} x{}", QpcMs(Qpc() - t0), oi.description, oi.width, oi.height, oi.multiplier));
        SetStatus(L"active: " + Widen(oi.description) + std::format(L" (settings reloaded {}x)", m_reloads));
        if (LegacyMode()) { std::lock_guard<std::mutex> lk(m_sigLock); m_settingsSig = SettingsSignature(m_rt.dataDir); }
        m_reloading = false;
    } else if (r == PUSHCORE_DEVICE_LOST) {
        m_reloading = false;
        DeviceLost(1, "pushcore_reload");
    } else {
        m_reloading = false;
        m_faulted = true;
        Fallback(L"pushcore reload failed");
    }
}

HRESULT CPushOutPin::InitAllocator(IMemAllocator** ppAlloc)
{
    if (static_cast<CPushMpcFilter*>(m_pFilter)->IsPassType(&m_mt)) return CTransformOutputPin::InitAllocator(ppAlloc); // not by subtype: the processed P010 output has the input's subtype
    HRESULT hr = S_OK;
    auto* a = new CD3D11Allocator(&hr);
    if (FAILED(hr)) { delete a; return hr; }
    ours = a;
    return a->NonDelegatingQueryInterface(IID_IMemAllocator, (void**)ppAlloc);
}

// ------------------------------------------------------------------------------------------------ property pages
// MPC-HC: Play > Filters > PushMPC Filter, or Options > External Filters > PushMPC Filter > Properties. DirectShow page
// conventions: a change marks the page dirty (PROPPAGESTATUS_DIRTY, MPC-HC enables Apply); Apply and OK write
// HKCU\Software\PushMPC and the running filter picks the new values up at once (g_settingsSerial; another MPC-HC process
// within 2 s); Cancel leaves the registry unchanged. The "Now:" notes and the status refresh once a second.
static void FillCombo(HWND dlg, int id, std::initializer_list<const wchar_t*> items)
{
    SendDlgItemMessageW(dlg, id, CB_RESETCONTENT, 0, 0);
    for (auto* s : items) SendDlgItemMessageW(dlg, id, CB_ADDSTRING, 0, (LPARAM)s);
}
static int ComboSel(HWND dlg, int id) { return int(SendDlgItemMessageW(dlg, id, CB_GETCURSEL, 0, 0)); }
static void ComboSet(HWND dlg, int id, int i) { SendDlgItemMessageW(dlg, id, CB_SETCURSEL, WPARAM(i), 0); }
// combo index <-> value: Smoother Auto 0, Off 1, n = n (2..16); Sharper Auto 0, Off 1, level n = n + 1; on/off Auto 0, Off 1, On 2
static int SmoothIdx(int v) { return v == pm::kAuto ? 0 : v == pm::kOff ? 1 : v; }
static int SmoothVal(int i) { return i <= 0 ? pm::kAuto : i == 1 ? pm::kOff : i; }
static int SharpIdx(int v) { return v == pm::kAuto ? 0 : v == pm::kOff ? 1 : v + 1; }
static int SharpVal(int i) { return i <= 0 ? pm::kAuto : i == 1 ? pm::kOff : i - 1; }
static int OnOffIdx(int v) { return v == pm::kAuto ? 0 : v == pm::kOff ? 1 : 2; }
static int OnOffVal(int i) { return i <= 0 ? pm::kAuto : i == 1 ? pm::kOff : 1; }

// Nothing may throw across the COM / Win32 boundary into MPC-HC (RC1 F4: a std::regex_error from Copy diagnostics
// ended MPC-HC): every property-page handler and exported function runs inside this.
template <class R, class F> static R Guarded(const char* where, R fail, F&& f) noexcept
{
    try { return f(); }
    catch (const std::exception& e) { try { Log(std::format("{}: exception caught ({}), MPC-HC keeps running", where, e.what())); } catch (...) {} }
    catch (...) { try { Log(std::format("{}: unknown exception caught, MPC-HC keeps running", where)); } catch (...) {} }
    return fail;
}
class CPushMpcPage : public CBasePropertyPage
{
public:
    static CUnknown* WINAPI CreateInstance(LPUNKNOWN pUnk, HRESULT* phr)
    {
        auto* p = new CPushMpcPage(pUnk, phr);
        if (!p) *phr = E_OUTOFMEMORY;
        return p;
    }
    CPushMpcPage(LPUNKNOWN pUnk, HRESULT* phr) : CBasePropertyPage(NAME("PushMPC page"), pUnk, IDD_PUSHMPC_PAGE, IDS_PUSHMPC_TITLE) { (void)phr; }
    HRESULT OnActivate() override { return Guarded("PushMPC page OnActivate", E_FAIL, [&] { return ActivateImpl(); }); }
    HRESULT OnDeactivate() override { return Guarded("PushMPC page OnDeactivate", S_OK, [&] { KillTimer(m_Dlg, 1); return S_OK; }); }
    INT_PTR OnReceiveMessage(HWND hwnd, UINT msg, WPARAM w, LPARAM l) override { return Guarded("PushMPC page message", INT_PTR(FALSE), [&] { return MessageImpl(hwnd, msg, w, l); }); }
    HRESULT OnApplyChanges() override { return Guarded("PushMPC page Apply", E_FAIL, [&] { return ApplyImpl(); }); }
    HRESULT ActivateImpl()
    {
        FillCombo(m_Dlg, IDC_PM_SMOOTH, { L"Auto", L"Off", L"2x", L"3x", L"4x", L"5x", L"6x", L"7x", L"8x", L"9x", L"10x", L"11x", L"12x", L"13x", L"14x", L"15x", L"16x" });
        FillCombo(m_Dlg, IDC_PM_SHARP, { L"Auto", L"Off", L"Level 1", L"Level 2", L"Level 3", L"Level 4" });
        FillCombo(m_Dlg, IDC_PM_CLEAN, { L"Auto", L"Off", L"On" });
        FillCombo(m_Dlg, IDC_PM_HDR, { L"Auto", L"Off", L"On" });
        FillCombo(m_Dlg, IDC_PM_HDRBOOST, { L"Auto", L"Off", L"Low", L"Medium", L"High" });
        FillCombo(m_Dlg, IDC_PM_HDRMETHOD, { L"Auto (PushMPC curve)", L"RTX Video HDR" });
        FillCombo(m_Dlg, IDC_PM_DISPPEAK, { L"Auto", L"400", L"600", L"1000" });
        FillCombo(m_Dlg, IDC_PM_OSD, { L"Off", L"Brief at start", L"Always" });
        FillCombo(m_Dlg, IDC_PM_BATTERY, { L"Light", L"Same as plugged in" });
        FillCombo(m_Dlg, IDC_PM_FGQ, { L"Auto", L"Low", L"Medium", L"High" });
        FillCombo(m_Dlg, IDC_PM_TARGET, { L"Screen", L"1440p", L"4K", L"5K" });
        FillCombo(m_Dlg, IDC_PM_STRENGTH, { L"Subtle", L"Balanced", L"Strong" });
        // RTX VSR model family: standard (balanced), high bitrate (keeps detail), streaming (cleans hardest)
        FillCombo(m_Dlg, IDC_PM_STYLE, { L"Auto", L"Standard", L"Keep detail", L"Clean hard" });
        FillCapsCombos();
        FillCombo(m_Dlg, IDC_PM_VRR, { L"Auto", L"Assume VRR", L"Assume fixed" });
        FillCombo(m_Dlg, IDC_PM_DEBAND, { L"Auto", L"Off", L"On" });
        FillCombo(m_Dlg, IDC_PM_DBSTR, { L"Low", L"Medium", L"High" });
        FillCombo(m_Dlg, IDC_PM_ANIM, { L"Auto", L"Off", L"On" });
        FillCombo(m_Dlg, IDC_PM_ANIMMODE, { L"Auto", L"Full", L"Camera only", L"Off" });
        FillCombo(m_Dlg, IDC_PM_SUBHOLD, { (g_coreFeatures & PUSHCORE_FEATURE_SUBTITLE_HOLD_AUTO) && pmu::ReadUnlockMode(pmu::Unlock::SubtitleHoldAuto) != pmu::UnlockMode::Off ? L"Auto (on text)" : pm::kSubtitleHoldAuto ? L"Auto (on)" : L"Auto (off)", L"Off", L"On" });
        // drop-down widths in dialog units (they scale with the font and the display's DPI, as the template does)
        for (int id : { IDC_PM_STYLE, IDC_PM_DEBAND, IDC_PM_DBSTR, IDC_PM_VRR }) SendDlgItemMessageW(m_Dlg, id, CB_SETDROPPEDWIDTH, Dlu(95), 0);
        for (int id : { IDC_PM_FGMETHOD, IDC_PM_UPSCALER, IDC_PM_HDRMETHOD }) SendDlgItemMessageW(m_Dlg, id, CB_SETDROPPEDWIDTH, Dlu(175), 0);
        m_scrollY = 0;
        Show(pm::LoadSettings());
        m_pushFpsData = PushFpsDataDir();
        ShowWindow(GetDlgItem(m_Dlg, IDC_PM_IMPORT), m_pushFpsData.empty() ? SW_HIDE : SW_SHOW);
        // the G-SYNC offer: NVIDIA, a VRR-capable display, and MPC-HC's driver profile without G-SYNC for windowed and full screen
        {
            bool nvidia = false; { std::lock_guard<std::mutex> lk(g_statusLock); nvidia = g_gpuKnown && g_gpu.family >= pm::kRtx50 && g_gpu.family <= pm::kGtx; }
            m_gsyncOffer = nvidia && pm::QueryDisplay(pm::PlayerWindow(), pm::VrrMode::Auto, true).vrrCapable && pm::NvGsyncState() == 0;
            if (pm::MpcBeInstalled()) SetDlgItemTextW(m_Dlg, IDC_PM_GSYNC, L"Turn on G-SYNC for MPC-HC and MPC-BE");
        }
        ShowAdvanced(false);
        m_loading = false;
        Refresh();
        SetTimer(m_Dlg, 1, 1000, nullptr);
        return S_OK;
    }
    // dialog units (horizontal) to pixels at the page's current font and DPI
    int Dlu(int x) const { RECT r = { 0, 0, x, 8 }; MapDialogRect(m_Dlg, &r); return r.right; }
    // F-UI1: the host sizes the page from the template only when it first builds the sheet, and on the first open
    // the sheet can come out shorter than the page with Advanced shown (MPC-HC grew it only after the About tab had
    // been visited). The page scrolls itself when its content is taller than the space it has, at any DPI.
    int ContentBottom() const
    {
        int bottom = 0;
        for (HWND c = GetWindow(m_Dlg, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
            if (!IsWindowVisible(c)) continue;
            RECT r; GetWindowRect(c, &r); MapWindowPoints(nullptr, m_Dlg, reinterpret_cast<POINT*>(&r), 2);
            bottom = std::max<int>(bottom, r.bottom + m_scrollY);
        }
        RECT m = { 0, 0, 0, 7 }; MapDialogRect(m_Dlg, &m);
        return bottom + m.bottom;
    }
    void ScrollTo(int y)
    {
        RECT cr; GetClientRect(m_Dlg, &cr);
        const int maxY = std::max<int>(0, ContentBottom() - (cr.bottom - cr.top));
        y = std::clamp(y, 0, maxY);
        if (y != m_scrollY) {
            ScrollWindowEx(m_Dlg, 0, m_scrollY - y, nullptr, nullptr, nullptr, nullptr, SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
            m_scrollY = y;
        }
        SCROLLINFO si = { sizeof(si) };
        si.fMask = SIF_POS; si.nPos = m_scrollY;
        SetScrollInfo(m_Dlg, SB_VERT, &si, TRUE);
    }
    void UpdateScroll()
    {
        RECT vr; GetClientRect(m_Dlg, &vr);
        if (!m_themeChecked && IsWindowVisible(m_Dlg) && ContentBottom() > vr.bottom) {
            // MPC-HC's dark theme colours the page's controls but not the page's own scroll bar: when the bar is first
            // needed, look at the page's painted background (top-left corner) and use Windows' dark scroll bar on a
            // dark one
            m_themeChecked = true;
            HDC dc = GetDC(m_Dlg);
            const COLORREF px = GetPixel(dc, 2, 2);
            ReleaseDC(m_Dlg, dc);
            if (px != CLR_INVALID && GetRValue(px) * 299 + GetGValue(px) * 587 + GetBValue(px) * 114 < 96000) {
                if (HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
                    if (auto set = reinterpret_cast<HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR)>(GetProcAddress(ux, "SetWindowTheme"))) set(m_Dlg, L"DarkMode_Explorer", nullptr);
            }
        }
        RECT cr; GetClientRect(m_Dlg, &cr);
        const int h = cr.bottom - cr.top, content = ContentBottom();
        SCROLLINFO si = { sizeof(si) };
        si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        si.nMin = 0; si.nMax = content > h ? content - 1 : 0; si.nPage = content > h ? UINT(h) : 0; si.nPos = m_scrollY;
        SetScrollInfo(m_Dlg, SB_VERT, &si, TRUE);   // hides the bar when everything fits
        ScrollTo(m_scrollY);                         // keeps the position inside the new range
    }
    bool ScrollMessage(UINT msg, WPARAM w)
    {
        RECT one = { 0, 0, 0, 16 }; MapDialogRect(m_Dlg, &one);
        const int line = std::max<int>(1, one.bottom);
        if (msg == WM_VSCROLL) {
            RECT cr; GetClientRect(m_Dlg, &cr);
            SCROLLINFO si = { sizeof(si) }; si.fMask = SIF_ALL; GetScrollInfo(m_Dlg, SB_VERT, &si);
            int y = m_scrollY;
            switch (LOWORD(w)) {
            case SB_LINEUP: y -= line; break;
            case SB_LINEDOWN: y += line; break;
            case SB_PAGEUP: y -= cr.bottom - line; break;
            case SB_PAGEDOWN: y += cr.bottom - line; break;
            case SB_THUMBTRACK: case SB_THUMBPOSITION: y = si.nTrackPos; break;
            case SB_TOP: y = 0; break;
            case SB_BOTTOM: y = INT_MAX / 2; break;
            default: return true;
            }
            ScrollTo(y);
            return true;
        }
        if (msg == WM_MOUSEWHEEL) {
            SCROLLINFO si = { sizeof(si) }; si.fMask = SIF_PAGE; GetScrollInfo(m_Dlg, SB_VERT, &si);
            if (!si.nPage) return false;   // nothing to scroll
            ScrollTo(m_scrollY - GET_WHEEL_DELTA_WPARAM(w) * 3 * line / WHEEL_DELTA);
            return true;
        }
        return false;
    }
    INT_PTR MessageImpl(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
    {
        if (msg == WM_TIMER) { Refresh(); return TRUE; }
        if ((msg == WM_VSCROLL && !l) || msg == WM_MOUSEWHEEL) { if (ScrollMessage(msg, w)) { SetWindowLongPtrW(hwnd, DWLP_MSGRESULT, 0); return TRUE; } }
        if (msg == WM_SIZE || msg == WM_DPICHANGED_AFTERPARENT) { UpdateScroll(); }
        if (msg == WM_COMMAND && !m_loading) {
            const int id = LOWORD(w), code = HIWORD(w);
            if (code == BN_CLICKED && id == IDC_PM_ADVANCED) { ShowAdvanced(!m_advanced); return TRUE; }
            if (code == BN_CLICKED && id == IDC_PM_IMPORT) { Import(); return TRUE; }
            if (code == BN_CLICKED && id == IDC_PM_GSYNC) {
                // MPC-HC's own NVIDIA profile only (never the global one); the previous values are kept for uninstall
                const std::wstring r = pm::NvGsyncApply();
                Log("G-SYNC for MPC-HC: " + Narrow(r));
                SetDlgItemTextW(m_Dlg, IDC_PM_GSYNC_NOTE, r.c_str());
                ShowWindow(GetDlgItem(m_Dlg, IDC_PM_GSYNC), pm::NvGsyncState() == 0 && m_advanced ? SW_SHOW : SW_HIDE);
                return TRUE;
            }
            if ((code == BN_CLICKED && (id == IDC_PM_ENABLE || id == IDC_PM_FRAMELOG || id == IDC_PM_HDRLIMIT)) || code == CBN_SELCHANGE || (code == CBN_EDITCHANGE && id == IDC_PM_DISPPEAK)) { Dirty(); if (code == CBN_SELCHANGE) UpdateCapsNote(); return TRUE; }
        }
        return CBasePropertyPage::OnReceiveMessage(hwnd, msg, w, l);
    }
    HRESULT ApplyImpl()
    {
        const pm::Settings s = Read();
        pm::SaveSettings(s);
        g_settingsSerial++;
        Log(std::format("property page: settings saved (enable {}, smoother {}, sharper {}, clean up {}, SDR to HDR {}, FG quality {}, upscale to {}, clean-up strength {}, "
            "upscale style {}, deband {} ({}), frame log {}, FG method {}, VRR {}, upscaler {})",
            s.enable, Narrow(pm::ChoiceStr(s.smoother)), Narrow(pm::ChoiceStr(s.sharper)), Narrow(pm::OnOffStr(s.cleanUp)), Narrow(pm::OnOffStr(s.sdrToHdr)),
            Narrow(pm::FgQualityStr(s.fgQuality)), Narrow(pm::TargetStr(s.target)), Narrow(pm::StrengthStr(s.strength)),
            Narrow(pm::VsrStyleStr(s.vsrStyle)), Narrow(pm::OnOffStr(s.deband)), Narrow(pm::DebandStrengthStr(s.debandStrength)), s.frameLog,
            Narrow(pm::FgMethodStr(s.fgMethod)), Narrow(pm::VrrStr(s.vrr)), Narrow(pm::UpscalerStr(s.upscaler))));
        return S_OK;
    }
private:
    bool m_advanced = false, m_loading = true;
    int m_scrollY = 0;   // pixels the page is scrolled down (F-UI1)
    bool m_themeChecked = false;
    std::wstring m_pushFpsData;
    void Dirty() { m_bDirty = TRUE; if (m_pPageSite) m_pPageSite->OnStatusChange(PROPPAGESTATUS_DIRTY); }
    // Frame generation method and upscaler: every choice is listed; one this GPU cannot run says so in its text, the note
    // under the rows gives the reason, and choosing it anyway keeps Auto's pick (the note says that too). Before a
    // file has been played the GPU is not known yet: everything is listed as is.
    pm::Gpu m_capsGpu; bool m_capsKnown = false;
    bool m_gsyncOffer = false;
    void FillCapsCombos()
    {
        { std::lock_guard<std::mutex> lk(g_statusLock); m_capsGpu = g_gpu; m_capsKnown = g_gpuKnown; }
        const int fsel = ComboSel(m_Dlg, IDC_PM_FGMETHOD), usel = ComboSel(m_Dlg, IDC_PM_UPSCALER);
        // a GPU limit and missing files are told apart (pushcore's hardware bit: the GPU could run it with its files)
        auto item = [&](const wchar_t* label, uint32_t cap) {
            return std::wstring(label) + (!m_capsKnown || m_capsGpu.has(cap) ? L"" : m_capsGpu.hw(cap) ? L" (files missing)" : L" (not on this GPU)"); };
        const std::wstring f[] = { L"Auto", item(L"NVIDIA VFG", pm::kCapVfg), item(L"DLSS-G", pm::kCapDlssg), item(L"PushFlow", pm::kCapPushFlow),
                                   item(m_capsGpu.family == pm::kRdna4 ? L"FSR 4 FG" : L"FSR 3.1 FG", pm::kCapFsrFg), item(L"AMD FRC", pm::kCapFrc), item(L"NVOF FG (NVIDIA optical flow + FSR)", pm::kCapNvofFg),
                                   item(L"XeSS FG (Intel)", pm::kCapXefg), item(L"Intel VPL frame interpolation", pm::kCapVplFi) };
        SendDlgItemMessageW(m_Dlg, IDC_PM_FGMETHOD, CB_RESETCONTENT, 0, 0);
        for (const auto& s : f) SendDlgItemMessageW(m_Dlg, IDC_PM_FGMETHOD, CB_ADDSTRING, 0, (LPARAM)s.c_str());
        const std::wstring u[] = { L"Auto", item(L"RTX VSR", pm::kCapRtxVsr), item(L"FSR", pm::kCapFsrSr), item(L"NIS", pm::kCapNis),
                                   item(L"XeSS SR (Intel)", pm::kCapXessSr), item(L"Intel VSR", pm::kCapIvsr),
                                   item(L"Anime upscaler", pm::kCapAnimeSr) };
        SendDlgItemMessageW(m_Dlg, IDC_PM_UPSCALER, CB_RESETCONTENT, 0, 0);
        for (const auto& s : u) SendDlgItemMessageW(m_Dlg, IDC_PM_UPSCALER, CB_ADDSTRING, 0, (LPARAM)s.c_str());
        if (fsel >= 0) ComboSet(m_Dlg, IDC_PM_FGMETHOD, fsel);
        if (usel >= 0) ComboSet(m_Dlg, IDC_PM_UPSCALER, usel);
        // the rows this GPU has nothing to choose in are greyed out
        auto count = [&](std::initializer_list<uint32_t> caps) { int n = 0; for (uint32_t c : caps) n += m_capsGpu.has(c) ? 1 : 0; return n; };
        EnableWindow(GetDlgItem(m_Dlg, IDC_PM_FGMETHOD), !m_capsKnown || count({ pm::kCapVfg, pm::kCapDlssg, pm::kCapPushFlow, pm::kCapFsrFg, pm::kCapFrc, pm::kCapNvofFg, pm::kCapXefg, pm::kCapVplFi }) > 0);
        EnableWindow(GetDlgItem(m_Dlg, IDC_PM_UPSCALER), !m_capsKnown || count({ pm::kCapRtxVsr, pm::kCapFsrSr, pm::kCapNis, pm::kCapXessSr, pm::kCapIvsr, pm::kCapAnimeSr }) > 1);
        EnableWindow(GetDlgItem(m_Dlg, IDC_PM_STYLE), !m_capsKnown || m_capsGpu.has(pm::kCapVsrFamilies));
        EnableWindow(GetDlgItem(m_Dlg, IDC_PM_HDR), !m_capsKnown || true);
        UpdateCapsNote();
    }
    void UpdateCapsNote()
    {
        std::wstring note;
        if (!m_capsKnown) note = L"What this GPU can run shows here once a file has played.";
        else {
            const int f = ComboSel(m_Dlg, IDC_PM_FGMETHOD), u = ComboSel(m_Dlg, IDC_PM_UPSCALER);
            const uint32_t fc[] = { 0, pm::kCapVfg, pm::kCapDlssg, pm::kCapPushFlow, pm::kCapFsrFg, pm::kCapFrc, pm::kCapNvofFg, pm::kCapXefg, pm::kCapVplFi };
            const uint32_t uc[] = { 0, pm::kCapRtxVsr, pm::kCapFsrSr, pm::kCapNis, pm::kCapXessSr, pm::kCapIvsr, pm::kCapAnimeSr };
            if (f > 0 && f < int(std::size(fc)) && !m_capsGpu.has(fc[f])) note += std::format(L"{}: {}; Auto's method is used. ", pm::FgMethodLabel(pm::FgMethod(f)), m_capsGpu.reason[fc[f]]);
            if (u > 0 && u < int(std::size(uc)) && !m_capsGpu.has(uc[u])) note += std::format(L"Upscaler: {}; Auto's is used. ", m_capsGpu.reason[uc[u]]);
            if (!m_capsGpu.has(pm::kCapVsrFamilies)) note += L"Upscale style: " + m_capsGpu.reason[pm::kCapVsrFamilies] + L". ";
            if (!m_capsGpu.has(pm::kCapNr)) note += L"Clean up: " + m_capsGpu.reason[pm::kCapNr] + L". ";
            if (ComboSel(m_Dlg, IDC_PM_HDRMETHOD) == 1 && !m_capsGpu.has(pm::kCapRtxHdr)) note += L"RTX Video HDR: " + m_capsGpu.reason[pm::kCapRtxHdr] + L"; PushMPC's curve is used. ";
            if (note.empty()) note = std::format(L"{} ({}): every choice here runs on it.", m_capsGpu.name, pm::FamilyName(m_capsGpu.family));
            else note = std::format(L"{} ({}): ", m_capsGpu.name, pm::FamilyName(m_capsGpu.family)) + note;
        }
        SetIfChanged(m_Dlg, IDC_PM_CAPS_NOTE, note);
    }
    void RefreshCaps()
    {
        bool known; uint32_t avail;
        { std::lock_guard<std::mutex> lk(g_statusLock); known = g_gpuKnown; avail = g_gpu.available; }
        if (known != m_capsKnown || avail != m_capsGpu.available) { const bool was = m_loading; m_loading = true; FillCapsCombos(); m_loading = was; }
    }
    void Show(const pm::Settings& s)
    {
        CheckDlgButton(m_Dlg, IDC_PM_ENABLE, s.enable ? BST_CHECKED : BST_UNCHECKED);
        ComboSet(m_Dlg, IDC_PM_SMOOTH, SmoothIdx(s.smoother));
        ComboSet(m_Dlg, IDC_PM_SHARP, SharpIdx(s.sharper));
        ComboSet(m_Dlg, IDC_PM_CLEAN, OnOffIdx(s.cleanUp));
        ComboSet(m_Dlg, IDC_PM_HDR, OnOffIdx(s.sdrToHdr));
        ComboSet(m_Dlg, IDC_PM_HDRBOOST, int(s.hdrBoost));
        ComboSet(m_Dlg, IDC_PM_HDRMETHOD, int(s.hdrMethod));
        SetDlgItemTextW(m_Dlg, IDC_PM_DISPPEAK, s.displayPeak ? std::to_wstring(s.displayPeak).c_str() : L"Auto");
        ComboSet(m_Dlg, IDC_PM_OSD, int(s.osd));
        ComboSet(m_Dlg, IDC_PM_BATTERY, int(s.onBattery));
        ComboSet(m_Dlg, IDC_PM_FGQ, s.fgQuality == pm::FgQuality::Auto ? 0 : int(s.fgQuality) + 1);
        ComboSet(m_Dlg, IDC_PM_FGMETHOD, int(s.fgMethod));
        ComboSet(m_Dlg, IDC_PM_VRR, int(s.vrr));
        ComboSet(m_Dlg, IDC_PM_UPSCALER, s.upscaler == pm::Upscaler::Anime ? 6 : int(s.upscaler));
        ComboSet(m_Dlg, IDC_PM_TARGET, int(s.target));
        ComboSet(m_Dlg, IDC_PM_STRENGTH, int(s.strength));
        ComboSet(m_Dlg, IDC_PM_STYLE, s.vsrStyle == pm::VsrStyle::Auto ? 0 : int(s.vsrStyle) + 1);
        ComboSet(m_Dlg, IDC_PM_DEBAND, OnOffIdx(s.deband));
        ComboSet(m_Dlg, IDC_PM_DBSTR, int(s.debandStrength));
        ComboSet(m_Dlg, IDC_PM_SUBHOLD, OnOffIdx(s.subtitleHold));
        ComboSet(m_Dlg, IDC_PM_ANIM, OnOffIdx(s.animation));
        {   // the show playing now (its own choice), else the default for every show
            const std::wstring show = pm::CurrentShow();
            const int v = show.empty() ? s.animMode : pm::ReadShow(show).choice;
            ComboSet(m_Dlg, IDC_PM_ANIMMODE, v < 0 ? 0 : v + 1);
            SetDlgItemTextW(m_Dlg, IDC_PM_ANIMMODE_LBL, show.empty() ? L"Drawn motion" : L"This show");
        }
        CheckDlgButton(m_Dlg, IDC_PM_FRAMELOG, s.frameLog ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(m_Dlg, IDC_PM_HDRLIMIT, s.hdrLimit ? BST_CHECKED : BST_UNCHECKED);
    }
    pm::Settings Read() const
    {
        pm::Settings s;
        s.enable = IsDlgButtonChecked(m_Dlg, IDC_PM_ENABLE) == BST_CHECKED;
        s.smoother = SmoothVal(ComboSel(m_Dlg, IDC_PM_SMOOTH));
        s.sharper = SharpVal(ComboSel(m_Dlg, IDC_PM_SHARP));
        s.cleanUp = OnOffVal(ComboSel(m_Dlg, IDC_PM_CLEAN));
        s.sdrToHdr = OnOffVal(ComboSel(m_Dlg, IDC_PM_HDR));
        s.hdrBoost = pm::HdrBoost(std::clamp(ComboSel(m_Dlg, IDC_PM_HDRBOOST), 0, 4));
        s.hdrMethod = pm::HdrMethod(std::clamp(ComboSel(m_Dlg, IDC_PM_HDRMETHOD), 0, 1));
        s.sdrGamma24 = pm::LoadSettings().sdrGamma24;   // registry only
        s.osd = pm::OsdMode(std::clamp(ComboSel(m_Dlg, IDC_PM_OSD), 0, 2));
        s.onBattery = pm::BatteryMode(std::clamp(ComboSel(m_Dlg, IDC_PM_BATTERY), 0, 1));
        { wchar_t b[32] = {}; GetDlgItemTextW(m_Dlg, IDC_PM_DISPPEAK, b, 32); const int p = _wtoi(b); s.displayPeak = p >= 100 && p <= 10000 ? p : 0; }
        { const int q = ComboSel(m_Dlg, IDC_PM_FGQ); s.fgQuality = q <= 0 ? pm::FgQuality::Auto : pm::FgQuality(std::clamp(q - 1, 0, 2)); }
        s.fgMethod = pm::FgMethod(std::clamp(ComboSel(m_Dlg, IDC_PM_FGMETHOD), 0, int(pm::FgMethod::Vpl)));
        s.vrr = pm::VrrMode(std::clamp(ComboSel(m_Dlg, IDC_PM_VRR), 0, 2));
        {   // the combo's rows: Auto, RTX VSR, FSR, NIS, XeSS SR, Intel VSR, Anime (Upscaler::Anime = 7: index - 1 = PUSHCORE_SR_METHOD_PMML)
            const int sel = ComboSel(m_Dlg, IDC_PM_UPSCALER);
            s.upscaler = sel == 6 ? pm::Upscaler::Anime : pm::Upscaler(std::clamp(sel, 0, int(pm::Upscaler::IntelVsr)));
        }
        s.target = pm::UpscaleTarget(std::clamp(ComboSel(m_Dlg, IDC_PM_TARGET), 0, 3));
        s.strength = pm::CleanStrength(std::clamp(ComboSel(m_Dlg, IDC_PM_STRENGTH), 0, 2));
        { const int v = ComboSel(m_Dlg, IDC_PM_STYLE); s.vsrStyle = v <= 0 ? pm::VsrStyle::Auto : pm::VsrStyle(std::clamp(v - 1, 0, 2)); }
        s.deband = OnOffVal(ComboSel(m_Dlg, IDC_PM_DEBAND));
        s.debandStrength = pm::DebandStrength(std::clamp(ComboSel(m_Dlg, IDC_PM_DBSTR), 0, 2));
        s.subtitleHold = OnOffVal(ComboSel(m_Dlg, IDC_PM_SUBHOLD));
        s.animation = OnOffVal(ComboSel(m_Dlg, IDC_PM_ANIM));
        {
            const int v = ComboSel(m_Dlg, IDC_PM_ANIMMODE) - 1;   // -1 Auto
            const std::wstring show = pm::CurrentShow();
            if (show.empty()) s.animMode = v < 0 ? pm::kAuto : v;
            else pm::WriteShowDword(show, L"Choice", v);           // sticky for this show; the default stays as it is
        }
        s.frameLog = IsDlgButtonChecked(m_Dlg, IDC_PM_FRAMELOG) == BST_CHECKED;
        s.hdrLimit = IsDlgButtonChecked(m_Dlg, IDC_PM_HDRLIMIT) == BST_CHECKED;
        return s;
    }
    void ShowAdvanced(bool on)
    {
        m_advanced = on;
        if (!on) ScrollTo(0);   // back to the top before the section goes
        for (int id : { IDC_PM_ADV_GROUP, IDC_PM_FGQ_LBL, IDC_PM_FGQ, IDC_PM_TARGET_LBL, IDC_PM_TARGET, IDC_PM_STRENGTH_LBL, IDC_PM_STRENGTH, IDC_PM_FRAMELOG, IDC_PM_IMPORT_NOTE,
                        IDC_PM_STYLE_LBL, IDC_PM_STYLE, IDC_PM_DEBAND_LBL, IDC_PM_DEBAND, IDC_PM_DBSTR_LBL, IDC_PM_DBSTR, IDC_PM_DEBAND_NOW,
                        IDC_PM_FGMETHOD_LBL, IDC_PM_FGMETHOD, IDC_PM_VRR_LBL, IDC_PM_VRR, IDC_PM_UPSCALER_LBL, IDC_PM_UPSCALER, IDC_PM_CAPS_NOTE,
                        IDC_PM_HDRMETHOD_LBL, IDC_PM_HDRMETHOD, IDC_PM_HDRLIMIT, IDC_PM_DISPPEAK_LBL, IDC_PM_DISPPEAK,
                        IDC_PM_OSD_LBL, IDC_PM_OSD, IDC_PM_BATTERY_LBL, IDC_PM_BATTERY, IDC_PM_GSYNC_NOTE, IDC_PM_SUBHOLD_LBL, IDC_PM_SUBHOLD, IDC_PM_SUBHOLD_NOTE,
                        IDC_PM_ANIM_LBL, IDC_PM_ANIM, IDC_PM_ANIM_NOTE, IDC_PM_ANIMMODE_LBL, IDC_PM_ANIMMODE, IDC_PM_ANIMMODE_NOTE })
            ShowWindow(GetDlgItem(m_Dlg, id), on ? SW_SHOW : SW_HIDE);
        ShowWindow(GetDlgItem(m_Dlg, IDC_PM_IMPORT), on && !m_pushFpsData.empty() ? SW_SHOW : SW_HIDE);
        ShowWindow(GetDlgItem(m_Dlg, IDC_PM_GSYNC), on && m_gsyncOffer ? SW_SHOW : SW_HIDE);
        SetDlgItemTextW(m_Dlg, IDC_PM_ADVANCED, on ? L"Hide advanced settings" : L"Show advanced settings");
        UpdateScroll();
    }
    static void SetIfChanged(HWND dlg, int id, const std::wstring& s)
    {
        wchar_t cur[4096] = {};
        GetDlgItemTextW(dlg, id, cur, 4096);
        if (s != cur) SetDlgItemTextW(dlg, id, s.c_str());
    }
    void Refresh()
    {
        const PageStatus st = PageSnapshot();
        // F-UI2: a "Now:" line that does not fit its space loses its explanations in brackets first ("Medium (High at
        // 5x would take ...)" -> "Medium"); the Status box keeps the full story. The rest is cut with an ellipsis.
        auto fit = [&](int id, std::wstring s) {
            HWND c = GetDlgItem(m_Dlg, id);
            if (!c || s.empty()) return s;
            RECT r; GetClientRect(c, &r);
            HDC dc = GetDC(c);
            HGDIOBJ old = SelectObject(dc, reinterpret_cast<HGDIOBJ>(SendMessageW(c, WM_GETFONT, 0, 0)));
            auto width = [&](const std::wstring& t) { SIZE z{}; GetTextExtentPoint32W(dc, t.c_str(), int(t.size()), &z); return z.cx; };
            while (width(s) > r.right) {
                size_t open = std::wstring::npos;
                for (const wchar_t* o : { L" (", L" [" }) { const size_t p = s.rfind(o); if (p != std::wstring::npos && (open == std::wstring::npos || p > open)) open = p; }
                if (open == std::wstring::npos) break;
                const wchar_t openCh = s[open + 1], closeCh = openCh == L'(' ? L')' : L']';
                int depth = 0; size_t end = std::wstring::npos;
                for (size_t i = open + 1; i < s.size(); ++i) {
                    if (s[i] == openCh) ++depth;
                    else if (s[i] == closeCh && --depth == 0) { end = i; break; }
                }
                s.erase(open, end == std::wstring::npos ? std::wstring::npos : end - open + 1);
            }
            SelectObject(dc, old);
            ReleaseDC(c, dc);
            return s;
        };
        auto now = [](const std::wstring& n) { return n.empty() ? std::wstring() : L"Now: " + n; };
        SetIfChanged(m_Dlg, IDC_PM_SMOOTH_NOW, fit(IDC_PM_SMOOTH_NOW, now(st.smoother)));
        SetIfChanged(m_Dlg, IDC_PM_SHARP_NOW, fit(IDC_PM_SHARP_NOW, now(st.sharper)));
        SetIfChanged(m_Dlg, IDC_PM_CLEAN_NOW, fit(IDC_PM_CLEAN_NOW, now(st.cleanUp)));
        SetIfChanged(m_Dlg, IDC_PM_HDR_NOW, fit(IDC_PM_HDR_NOW, now(st.sdrToHdr)));
        SetIfChanged(m_Dlg, IDC_PM_DEBAND_NOW, fit(IDC_PM_DEBAND_NOW, st.deband.empty() ? std::wstring() : L"Deband now: " + st.deband));
        SetIfChanged(m_Dlg, IDC_PM_STATUS, st.live.empty() ? st.state : st.state + L"\r\n" + st.live);
        RefreshCaps();
    }
    // Reads the installed PushFPS's saved chain session once (read-only, through pushcore with PushFPS's own store) and
    // maps it onto these settings; Apply keeps them.
    void Import()
    {
        Runtime rt = ResolveRuntime(ReadConfig());
        std::wstring err;
        const PushCoreApi* api = rt.error.empty() || FileExists(rt.corePath) ? LoadApi(rt, err) : nullptr;
        if (!api) { SetDlgItemTextW(m_Dlg, IDC_PM_IMPORT_NOTE, (L"Not imported: " + (err.empty() ? rt.error : err)).c_str()); return; }
        PushCoreSettings in = {};
        char eb[512] = {};
        if (api->importChainSession(m_pushFpsData.c_str(), &in, eb, sizeof(eb)) != PUSHCORE_OK) {
            SetDlgItemTextW(m_Dlg, IDC_PM_IMPORT_NOTE, (L"Not imported: " + Widen(eb)).c_str());
            return;
        }
        pm::Settings s = Read();
        s.smoother = in.multiplier >= 2 ? std::min(int(in.multiplier), pm::kMaxSmoother) : pm::kOff;
        s.sharper = !in.sr ? pm::kOff : in.srQuality >= 1 && in.srQuality <= 4 ? int(in.srQuality) : pm::kAuto;
        s.cleanUp = in.nr ? 1 : pm::kOff;
        s.sdrToHdr = in.videoHdr ? 1 : pm::kOff;
        s.fgQuality = pm::FgQuality(std::min(in.vfgQuality, 2u));
        s.target = in.srTarget == PUSHCORE_SR_TARGET_1440P ? pm::UpscaleTarget::P1440 : in.srTarget == PUSHCORE_SR_TARGET_5K ? pm::UpscaleTarget::K5 : pm::UpscaleTarget::K4;
        s.strength = pm::NearestStrength(in.nrTone, in.nrStructure);
        s.vsrStyle = in.srFamily == PUSHCORE_SR_FAMILY_HIGH_BITRATE ? pm::VsrStyle::HighBitrate : in.srFamily == PUSHCORE_SR_FAMILY_STREAMING ? pm::VsrStyle::Streaming : pm::VsrStyle::Standard;
        m_loading = true; Show(s); m_loading = false;
        Dirty();
        SetDlgItemTextW(m_Dlg, IDC_PM_IMPORT_NOTE, L"Imported. Press Apply to keep them.");
        Log(std::format("property page: imported the PushFPS settings (multiplier {}, SR {} quality {}, NR {}, video HDR {}); not saved until Apply",
            in.multiplier, in.sr, in.srQuality, in.nr, in.videoHdr));
    }
};

static std::wstring ComponentLine(const wchar_t* name, const std::wstring& dir)
{
    const Manifest m = ReadManifest(dir);
    if (!m.ok) return std::format(L"{}: not installed ({})", name, dir);
    std::wstring extra;
    if (!m.Get(L"pushfps").empty()) extra += L", built from PushFPS " + m.Get(L"pushfps");
    if (!m.Get(L"package").empty()) extra += L", from " + m.Get(L"package");
    return std::format(L"{} {}{}", name, m.Get(L"version"), extra);
}
// ------------------------------------------------------------------------------------------------ diagnostics
static std::wstring RegDump(HKEY root, const std::wstring& path, int depth = 1)
{
    std::wstring out; HKEY k = nullptr;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return L"  (" + path + L": absent)\r\n";
    for (DWORD i = 0;; ++i) {
        wchar_t name[256]; DWORD nn = 256, type = 0, sz = 0;
        if (RegEnumValueW(k, i, name, &nn, nullptr, &type, nullptr, &sz) != ERROR_SUCCESS) break;
        out += L"  " + path.substr(path.find_last_of(L'\\') + 1) + L"\\" + name + L" = " + pm::RegStr(k, name) + L"\r\n";
    }
    if (depth > 0)
        for (DWORD i = 0;; ++i) {
            wchar_t sub[256]; DWORD sn = 256;
            if (RegEnumKeyExW(k, i, sub, &sn, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            out += RegDump(root, path + L"\\" + sub, depth - 1);
        }
    RegCloseKey(k);
    return out;
}
static std::wstring FileVersionOf(const std::wstring& path)
{
    DWORD h = 0; const DWORD sz = GetFileVersionInfoSizeW(path.c_str(), &h);
    std::vector<BYTE> vi(sz); VS_FIXEDFILEINFO* f = nullptr; UINT len = 0;
    if (!sz || !GetFileVersionInfoW(path.c_str(), 0, sz, vi.data()) || !VerQueryValueW(vi.data(), L"\\", (void**)&f, &len) || !f) return L"?";
    return std::format(L"{}.{}.{}.{}", HIWORD(f->dwFileVersionMS), LOWORD(f->dwFileVersionMS), HIWORD(f->dwFileVersionLS), LOWORD(f->dwFileVersionLS));
}
// Everything a bug report needs, anonymised (user name, profile paths, computer name, video file names)
static std::wstring BuildDiagnostics(const std::wstring& versions)
{
    std::wstring d = L"PushMPC diagnostics " + Widen(PUSHMPC_VERSION_STR) + L"\r\n\r\n[Versions]\r\n" + versions + L"\r\n";
    wchar_t exe[MAX_PATH] = {}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    d += std::format(L"Host: {} {}\r\n", std::wstring(exe).substr(std::wstring(exe).find_last_of(L'\\') + 1), FileVersionOf(exe));
    // GPUs, drivers, VRAM; displays
    d += L"\r\n[GPUs and displays]\r\n";
    ComPtr<IDXGIFactory1> f;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) {
        ComPtr<IDXGIAdapter1> a;
        for (UINT i = 0; SUCCEEDED(f->EnumAdapters1(i, &a)); i++, a.Reset()) {
            DXGI_ADAPTER_DESC1 ad{}; a->GetDesc1(&ad);
            LARGE_INTEGER umd{}; a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd);
            d += std::format(L"GPU {}: {} (vendor {:04X}), driver {}.{}.{}.{}, VRAM {} MB\r\n", i, ad.Description, ad.VendorId,
                             HIWORD(umd.HighPart), LOWORD(umd.HighPart), HIWORD(umd.LowPart), LOWORD(umd.LowPart), ad.DedicatedVideoMemory >> 20);
            ComPtr<IDXGIOutput> o;
            for (UINT j = 0; SUCCEEDED(a->EnumOutputs(j, &o)); j++, o.Reset()) {
                ComPtr<IDXGIOutput6> o6; DXGI_OUTPUT_DESC1 od{};
                if (FAILED(o.As(&o6)) || FAILED(o6->GetDesc1(&od))) continue;
                DEVMODEW dm{}; dm.dmSize = sizeof(dm); EnumDisplaySettingsW(od.DeviceName, ENUM_CURRENT_SETTINGS, &dm);
                d += std::format(L"  display {}: {}x{} at {} Hz, HDR {}, peak {:.0f} cd/m2, full frame {:.0f} cd/m2\r\n", j, dm.dmPelsWidth, dm.dmPelsHeight,
                                 dm.dmDisplayFrequency, od.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ? L"on" : L"off", od.MaxLuminance, od.MaxFullFrameLuminance);
            }
        }
    }
    const pm::Display disp = pm::QueryDisplay(pm::PlayerWindow(), pm::VrrMode::Auto, true);
    d += std::format(L"Host player: {}\r\n{}'s display: {} {:.3f} Hz, VRR {} ({}), capable {}, battery {}; G-SYNC profile for {}: {}\r\n", pm::HostName(), pm::HostName(), disp.name, disp.refreshHz,
                     disp.vrr ? L"on" : L"off", disp.vrrHow, disp.vrrCapable ? L"yes" : L"no", disp.battery ? L"yes" : L"no", pm::HostName(),
                     [] { const int s = pm::NvGsyncState(); return s == 1 ? L"set" : s == 0 ? L"not set" : L"unknown"; }());
    // what runs: the page's status (Auto's decisions and reasons, the cut guard, fallbacks)
    { std::lock_guard<std::mutex> lk(g_statusLock);
      d += L"\r\n[Status]\r\n" + g_page.state + L"\r\n" + g_page.live + L"\r\nSmoother: " + g_page.smoother + L"\r\nSharper: " + g_page.sharper +
           L"\r\nClean up: " + g_page.cleanUp + L"\r\nSDR to HDR: " + g_page.sdrToHdr + L"\r\nDeband: " + g_page.deband + L"\r\n"; }
    // settings: ours, MPC-HC's renderer and filters, LAV Video, MPC Video Renderer
    d += L"\r\n[Settings]\r\n" + RegDump(HKEY_CURRENT_USER, L"Software\\PushMPC");
    if (pm::HostPlayer() != pm::Host::MpcBe) d += RegDump(HKEY_CURRENT_USER, L"Software\\MPC-HC\\MPC-HC\\Settings", 0);
    if (pm::HostPlayer() == pm::Host::MpcBe || pm::MpcBeInstalled()) {   // MPC-BE: its filter entries and its decoder's hardware mode
        d += RegDump(HKEY_CURRENT_USER, L"Software\\MPC-BE\\ExternalFilters", 1);
        d += RegDump(HKEY_CURRENT_USER, L"Software\\MPC-BE\\Filters\\MPC Video Decoder", 0);
    }
    d += RegDump(HKEY_CURRENT_USER, L"Software\\LAV\\Video", 1);
    d += RegDump(HKEY_CURRENT_USER, L"Software\\MPC-BE Filters\\MPC Video Renderer", 0);
    // the newest filter log: its last 200 lines and its errors
    std::wstring newest; FILETIME best{};
    WIN32_FIND_DATAW fd{}; HANDLE h = FindFirstFileW((LogDir() + L"\\filter-*.log").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { if (CompareFileTime(&fd.ftLastWriteTime, &best) > 0) { best = fd.ftLastWriteTime; newest = fd.cFileName; } } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (!newest.empty()) {
        std::ifstream in(LogDir() + L"\\" + newest);
        std::vector<std::string> lines; std::string line;
        while (std::getline(in, line)) lines.push_back(line);
        std::wstring errs, tail;
        for (const auto& l : lines) if (l.find("error") != std::string::npos || l.find("failed") != std::string::npos || l.find("fallback") != std::string::npos || l.find("Fallback") != std::string::npos) errs += Widen(l) + L"\r\n";
        for (size_t i = lines.size() > 200 ? lines.size() - 200 : 0; i < lines.size(); ++i) tail += Widen(lines[i]) + L"\r\n";
        d += L"\r\n[Errors in the log]\r\n" + errs + L"\r\n[Last 200 log lines]\r\n" + tail;
    }
    return pm::Anonymise(d);
}
class CPushMpcAboutPage : public CBasePropertyPage
{
public:
    static CUnknown* WINAPI CreateInstance(LPUNKNOWN pUnk, HRESULT* phr)
    {
        auto* p = new CPushMpcAboutPage(pUnk, phr);
        if (!p) *phr = E_OUTOFMEMORY;
        return p;
    }
    CPushMpcAboutPage(LPUNKNOWN pUnk, HRESULT* phr) : CBasePropertyPage(NAME("PushMPC about page"), pUnk, IDD_PUSHMPC_ABOUT, IDS_PUSHMPC_ABOUT) { (void)phr; }
    HRESULT OnActivate() override { return Guarded("About page OnActivate", E_FAIL, [&] { return ActivateImpl(); }); }
    INT_PTR OnReceiveMessage(HWND hwnd, UINT msg, WPARAM w, LPARAM l) override { return Guarded("About page message", INT_PTR(FALSE), [&] { return MessageImpl(hwnd, msg, w, l); }); }
    HRESULT ActivateImpl()
    {
        const Config cfg = ReadConfig();
        const std::wstring base = LocalAppData() + L"\\PushMPC";
        const std::wstring core = cfg.coreFolder.empty() ? base + L"\\core" : cfg.coreFolder;
        const std::wstring run = cfg.runtimeRoot.empty() ? base + L"\\runtime" : cfg.runtimeRoot;
        const std::wstring v = std::format(L"Filter {} (this file)\r\n{}\r\n{}\r\n{}", Widen(PUSHMPC_VERSION_STR),
            ComponentLine(L"Filter (installed)", base + L"\\filter"), ComponentLine(L"Core", core), ComponentLine(L"Runtime", run));
        SetDlgItemTextW(m_Dlg, IDC_AB_VERSIONS, v.c_str());
        m_versions = v;
        FillCombo(m_Dlg, IDC_AB_CHANNEL, { L"Stable", L"Beta" });
        ShowUpdate();
        SetDlgItemTextW(m_Dlg, IDC_AB_CREDITS,
            L"PushMPC brings PushFPS's video processing into MPC-HC and MPC-BE.\r\n"
            L"PushFPS by DosukaSOL, a modified version of Veyra by Likely7; both GPL-3.0. PushMPC is GPL-3.0 too.\r\n"
            L"NVIDIA RTX Video Super Resolution, RTX Video HDR, the Video Effects SDK frame generation and the AI enhancement "
            L"runtime are NVIDIA components, used under NVIDIA's licences.\r\n"
            L"FFmpeg (avutil, swscale): LGPL/GPL, from Veyra's runtime package.\r\n"
            L"DirectShow base classes and interfaces from MPC Video Renderer (GPL).");
        SetDlgItemTextW(m_Dlg, IDC_AB_LOGDIR, LogDir().c_str());
        SetDlgItemTextW(m_Dlg, IDC_AB_REPO, Widen(PUSHMPC_REPO_URL).c_str());
        return S_OK;
    }
    std::wstring m_versions;
    bool m_loadingUpd = false;
    void ShowUpdate()
    {
        const pm::UpdateInfo u = pm::ReadUpdateInfo();
        const bool have = GetFileAttributesW(pm::UpdaterPath(LocalAppData()).c_str()) != INVALID_FILE_ATTRIBUTES;
        std::wstring s = !have ? L"The updater is not installed." : u.status.empty() ? L"Not checked yet."
                       : std::format(L"{}{}{}", u.status, u.available.empty() ? L"" : L": " + u.available, u.lastCheck.empty() ? L"" : L" (last check " + u.lastCheck + L")");
        if (!u.message.empty()) s += L"\r\n" + u.message;
        SetDlgItemTextW(m_Dlg, IDC_AB_UPDATE, s.c_str());
        m_loadingUpd = true;
        CheckDlgButton(m_Dlg, IDC_AB_AUTOUPD, u.enabled ? BST_CHECKED : BST_UNCHECKED);
        ComboSet(m_Dlg, IDC_AB_CHANNEL, _wcsicmp(u.channel.c_str(), L"Beta") == 0 ? 1 : 0);
        m_loadingUpd = false;
        EnableWindow(GetDlgItem(m_Dlg, IDC_AB_CHECKUPD), have);
    }
    INT_PTR MessageImpl(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
    {
        if (msg == WM_TIMER && w == 77) { ShowUpdate(); return TRUE; }
        if (msg == WM_COMMAND && HIWORD(w) == BN_CLICKED && LOWORD(w) == IDC_AB_DIAG) {
            std::wstring text;
            try { text = BuildDiagnostics(m_versions); }
            catch (const std::exception& e) {
                Log(std::format("Copy diagnostics failed: {}", e.what()));
                SetDlgItemTextW(m_Dlg, IDC_AB_DIAG_NOTE, (L"Diagnostics could not be made: " + Widen(e.what())).c_str());
                return TRUE;
            }
            const std::wstring path = LogDir() + L"\\diagnostics-" + Stamp() + L".txt";
            if (FILE* fp = _wfopen(path.c_str(), L"wb")) { const std::string u8 = Narrow(text); fwrite(u8.data(), 1, u8.size(), fp); fclose(fp); }
            bool copied = false;
            if (OpenClipboard(m_Dlg)) {
                EmptyClipboard();
                if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t))) {
                    std::memcpy(GlobalLock(g), text.c_str(), (text.size() + 1) * sizeof(wchar_t)); GlobalUnlock(g);
                    copied = SetClipboardData(CF_UNICODETEXT, g) != nullptr;
                    if (!copied) GlobalFree(g);
                }
                CloseClipboard();
            }
            SetDlgItemTextW(m_Dlg, IDC_AB_DIAG_NOTE, (copied ? L"Copied, and saved in the log folder (anonymised)" : L"Saved in the log folder (anonymised)"));
            return TRUE;
        }
        if (msg == WM_COMMAND && HIWORD(w) == BN_CLICKED && LOWORD(w) == IDC_AB_CHECKUPD) {
            const bool ok = pm::LaunchUpdater(LocalAppData(), L"--check");
            SetDlgItemTextW(m_Dlg, IDC_AB_UPD_NOTE, ok ? L"Checking..." : L"The updater is not installed");
            if (ok) SetTimer(m_Dlg, 77, 3000, nullptr);
            return TRUE;
        }
        if (msg == WM_COMMAND && !m_loadingUpd && ((HIWORD(w) == BN_CLICKED && LOWORD(w) == IDC_AB_AUTOUPD) || (HIWORD(w) == CBN_SELCHANGE && LOWORD(w) == IDC_AB_CHANNEL))) {
            pm::WriteUpdatePrefs(IsDlgButtonChecked(m_Dlg, IDC_AB_AUTOUPD) == BST_CHECKED, ComboSel(m_Dlg, IDC_AB_CHANNEL) == 1 ? L"Beta" : L"Stable");
            return TRUE;
        }
        if (msg == WM_COMMAND && HIWORD(w) == BN_CLICKED && LOWORD(w) == IDC_AB_OPENLOG) {
            ShellExecuteW(m_Dlg, L"open", LogDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return TRUE;
        }
        return CBasePropertyPage::OnReceiveMessage(hwnd, msg, w, l);
    }
};

// ------------------------------------------------------------------------------------------------ registration (per user: HKCU\Software\Classes)
const AMOVIESETUP_MEDIATYPE sudInTypes[] = { { &MEDIATYPE_Video, &MEDIASUBTYPE_NV12 }, { &MEDIATYPE_Video, &MEDIASUBTYPE_P010_PM } };
const AMOVIESETUP_MEDIATYPE sudOutTypes[] = { { &MEDIATYPE_Video, &MEDIASUBTYPE_ARGB32 }, { &MEDIATYPE_Video, &MEDIASUBTYPE_Y410_PM },
    { &MEDIATYPE_Video, &MEDIASUBTYPE_NV12 }, { &MEDIATYPE_Video, &MEDIASUBTYPE_P010_PM } };
const AMOVIESETUP_PIN sudPins[] = {
    { (LPWSTR)L"Input", FALSE, FALSE, FALSE, FALSE, &CLSID_NULL, nullptr, 2, sudInTypes },
    { (LPWSTR)L"Output", FALSE, TRUE, FALSE, FALSE, &CLSID_NULL, nullptr, 4, sudOutTypes },
};
const AMOVIESETUP_FILTER sudFilter = { &CLSID_PushMpcFilter, kFilterName, MERIT_DO_NOT_USE, 2, sudPins, CLSID_LegacyAmFilterCategory };
CFactoryTemplate g_Templates[] = {
    { kFilterName, &CLSID_PushMpcFilter, CPushMpcFilter::CreateInstance, nullptr, &sudFilter },
    { L"PushMPC Filter page", &CLSID_PushMpcPage, CPushMpcPage::CreateInstance, nullptr, nullptr },
    { L"PushMPC Filter about page", &CLSID_PushMpcAboutPage, CPushMpcAboutPage::CreateInstance, nullptr, nullptr },
};
int g_cTemplates = 3;

static std::wstring GuidStr(const GUID& g) { wchar_t b[64]; StringFromGUID2(g, b, 64); return b; }
static const std::wstring kClsKey = L"Software\\Classes\\CLSID\\" + GuidStr(CLSID_PushMpcFilter);
static const std::wstring kPageKey = L"Software\\Classes\\CLSID\\" + GuidStr(CLSID_PushMpcPage);
static const std::wstring kAboutKey = L"Software\\Classes\\CLSID\\" + GuidStr(CLSID_PushMpcAboutPage);
static const std::wstring kCatKey = L"Software\\Classes\\CLSID\\" + GuidStr(CLSID_LegacyAmFilterCategory);
static const wchar_t* kInproc = L"\\InprocServer32";
static const wchar_t* kInstance = L"\\Instance";

static LONG SetStr(HKEY root, const std::wstring& key, const wchar_t* name, const std::wstring& v)
{
    HKEY h; LONG r = RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr);
    if (r) return r;
    r = RegSetValueExW(h, name, 0, REG_SZ, (const BYTE*)v.c_str(), DWORD((v.size() + 1) * 2));
    RegCloseKey(h);
    return r;
}

// rundll32 entry points for the installer: rundll32.exe "<path>\PushMpcFilter.ax",RestoreNvProfile puts MPC-HC's (and
// MPC-BE's) NVIDIA profile back to what PushMPC found (HKCU\Software\PushMPC\NvBackup); ApplyNvProfile turns G-SYNC on
// there (windowed and full screen). Exit status through the log only (rundll32 has none).
extern "C" void CALLBACK RestoreNvProfile(HWND, HINSTANCE, LPSTR, int)
{
    Guarded("RestoreNvProfile", 0, [] { std::wstring note; pm::NvGsyncRestore(&note); Log("RestoreNvProfile: " + Narrow(note)); return 0; });
}
extern "C" void CALLBACK ApplyNvProfile(HWND, HINSTANCE, LPSTR, int)
{
    Guarded("ApplyNvProfile", 0, [] { Log("ApplyNvProfile: " + Narrow(pm::NvGsyncApply())); return 0; });
}
// the COM entry points of the base classes, exported through these (PushMpcFilter.def)
STDAPI PushMpc_DllGetClassObject(REFCLSID c, REFIID i, void** pv) { return Guarded("DllGetClassObject", HRESULT(E_UNEXPECTED), [&] { return DllGetClassObject(c, i, pv); }); }
STDAPI PushMpc_DllCanUnloadNow() { return Guarded("DllCanUnloadNow", HRESULT(S_FALSE), [] { return DllCanUnloadNow(); }); }
static HRESULT RegisterImpl();
static HRESULT UnregisterImpl();
STDAPI DllRegisterServer() { return Guarded("DllRegisterServer", HRESULT(E_UNEXPECTED), [] { return RegisterImpl(); }); }
STDAPI DllUnregisterServer() { return Guarded("DllUnregisterServer", HRESULT(E_UNEXPECTED), [] { return UnregisterImpl(); }); }
static HRESULT RegisterImpl()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_hInst, path, MAX_PATH);
    LONG r = SetStr(HKEY_CURRENT_USER, kClsKey, nullptr, kFilterName);
    r |= SetStr(HKEY_CURRENT_USER, kClsKey + kInproc, nullptr, path);
    r |= SetStr(HKEY_CURRENT_USER, kClsKey + kInproc, L"ThreadingModel", L"Both");
    r |= SetStr(HKEY_CURRENT_USER, kPageKey, nullptr, L"PushMPC Filter page");
    r |= SetStr(HKEY_CURRENT_USER, kPageKey + kInproc, nullptr, path);
    r |= SetStr(HKEY_CURRENT_USER, kPageKey + kInproc, L"ThreadingModel", L"Both");
    r |= SetStr(HKEY_CURRENT_USER, kAboutKey, nullptr, L"PushMPC Filter about page");
    r |= SetStr(HKEY_CURRENT_USER, kAboutKey + kInproc, nullptr, path);
    r |= SetStr(HKEY_CURRENT_USER, kAboutKey + kInproc, L"ThreadingModel", L"Both");
    const std::wstring inst = kCatKey + kInstance + L"\\" + GuidStr(CLSID_PushMpcFilter);
    r |= SetStr(HKEY_CURRENT_USER, inst, L"FriendlyName", kFilterName);
    r |= SetStr(HKEY_CURRENT_USER, inst, L"CLSID", GuidStr(CLSID_PushMpcFilter));
    return r ? HRESULT_FROM_WIN32(r) : S_OK;
}

static HRESULT UnregisterImpl()
{
    RegDeleteTreeW(HKEY_CURRENT_USER, kClsKey.c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, kPageKey.c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, kAboutKey.c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, (kCatKey + kInstance + L"\\" + GuidStr(CLSID_PushMpcFilter)).c_str());
    // the per-user category keys did not exist before the registration: remove them when empty
    RegDeleteKeyW(HKEY_CURRENT_USER, (kCatKey + kInstance).c_str());
    RegDeleteKeyW(HKEY_CURRENT_USER, kCatKey.c_str());
    return S_OK;
}

extern "C" BOOL WINAPI DllEntryPoint(HINSTANCE, ULONG, LPVOID);
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) { return DllEntryPoint(h, reason, r); }
