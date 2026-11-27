// late_reload_host: a live check of the late reload take-over (filter/src/PushMpcReload.h Late, CPushMpcFilter::AdoptLate)
// WITHOUT the installed filter: it loads PushMpcFilter.ax from the given path (LoadLibrary + DllGetClassObject, no
// registration), builds clip -> (the system's decoder) -> that filter -> the installed MPC Video Renderer (read-only,
// by its CLSID) and plays. During the run it changes the user setting "Smoother" (HKCU\Software\PushMPC) so that Auto
// rebuilds the graph; with the debug value HKCU\Software\PushMPC\Filter\ReloadTimeoutMs = 200 every rebuild passes its
// bound and is taken over late. At the end it reads this process's filter log and counts the take-over lines.
// Registry writes (Smoother, and whatever the filter writes) are undone by the run script's restore
// (filter/tools/late_reload_live.ps1); it needs the GPU: run it only through that script, holding the GPU lock.
//
// Usage: late_reload_host <PushMpcFilter.ax> <clip> [seconds=75]
// Prints "result ..." and the counted log lines. Exit 0: at least one late reload was taken over and nothing aborted;
// 2: played, but no take-over seen; 1: the graph did not play.
// Build: filter/CMakeLists.txt target late_reload_host (cl /std:c++20 /EHsc ... strmiids.lib ole32.lib user32.lib advapi32.lib)
#define NOMINMAX
#include <windows.h>
#include <dshow.h>
#include <shlobj.h>
#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

namespace {

const CLSID kPushMpc = { 0x798D4EC3, 0xE59C, 0x4A4E, { 0xA2, 0xEE, 0xC8, 0x04, 0xAE, 0xAE, 0x04, 0xCC } };
const CLSID kMpcVr = { 0x71F080AA, 0x8661, 0x4093, { 0xB1, 0x5E, 0x4F, 0x69, 0x03, 0xE7, 0x7D, 0x0A } };

template <class T> struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

IPin* FindPin(IBaseFilter* f, PIN_DIRECTION want)
{
    Com<IEnumPins> e;
    if (FAILED(f->EnumPins(&e))) return nullptr;
    IPin* pin = nullptr;
    while (e->Next(1, &pin, nullptr) == S_OK) {
        PIN_DIRECTION d; pin->QueryDirection(&d);
        Com<IPin> peer;
        if (d == want && FAILED(pin->ConnectedTo(&peer))) return pin;
        pin->Release();
    }
    return nullptr;
}

std::wstring RegGet(const wchar_t* name)
{
    wchar_t b[256] = {}; DWORD n = sizeof(b);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\PushMPC", name, RRF_RT_REG_SZ, nullptr, b, &n) != ERROR_SUCCESS) return std::wstring();
    return b;
}
void RegPut(const wchar_t* name, const std::wstring& v)
{
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\PushMPC", 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    if (v.empty()) RegDeleteValueW(k, name);
    else RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), DWORD((v.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
}

// this process's filter log: %LOCALAPPDATA%\PushMPC\logs\filter-<date>-<time>-<pid>.log
std::wstring FilterLog()
{
    PWSTR la = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &la))) return std::wstring();
    const std::wstring dir = std::wstring(la) + L"\\PushMPC\\logs\\";
    CoTaskMemFree(la);
    WIN32_FIND_DATAW fd{};
    const std::wstring pat = dir + L"filter-2*-" + std::to_wstring(GetCurrentProcessId()) + L".log";
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return std::wstring();
    FindClose(h);
    return dir + fd.cFileName;
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3) { std::fwprintf(stderr, L"usage: late_reload_host <PushMpcFilter.ax> <clip> [seconds=75]\n"); return 1; }
    const std::wstring ax = argv[1], clip = argv[2];
    const int seconds = argc > 3 ? std::max(30, _wtoi(argv[3])) : 75;
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    std::string state = "start";
    bool played = false;
    const std::wstring smootherBefore = RegGet(L"Smoother");
    {
        // the build's filter, not the registered one (its folder first in the DLL search: PushMpcExit.dll next to it)
        HMODULE mod = LoadLibraryExW(ax.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        auto getObj = mod ? reinterpret_cast<HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**)>(GetProcAddress(mod, "DllGetClassObject")) : nullptr;
        if (!getObj) { std::printf("result played=0 state=cannot load %ls (error %lu)\n", ax.c_str(), GetLastError()); CoUninitialize(); return 1; }

        WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"PushMpcLateReloadHost";
        RegisterClassW(&wc);
        HWND hwnd = CreateWindowW(wc.lpszClassName, L"PushMPC late reload host", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 960, 540, nullptr, nullptr, wc.hInstance, nullptr);

        Com<IGraphBuilder> graph; Com<IBaseFilter> source, pm, renderer; Com<IClassFactory> cf;
        auto step = [&](HRESULT hr, const char* what) { if (FAILED(hr)) { char b[160]; std::snprintf(b, sizeof(b), "%s failed 0x%08lX", what, hr); state = b; } return SUCCEEDED(hr); };
        bool ok = step(CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&graph)), "FilterGraph")
               && step(graph->AddSourceFilter(clip.c_str(), L"source", &source), "AddSourceFilter")
               && step(getObj(kPushMpc, IID_IClassFactory, reinterpret_cast<void**>(&cf)), "DllGetClassObject (the build's filter)")
               && step(cf->CreateInstance(nullptr, IID_IBaseFilter, reinterpret_cast<void**>(&pm)), "CreateInstance PushMPC Filter")
               && step(graph->AddFilter(pm.p, L"PushMPC Filter"), "AddFilter PushMPC")
               && step(CoCreateInstance(kMpcVr, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&renderer)), "MPC Video Renderer (installed, not registered?)")
               && step(graph->AddFilter(renderer.p, L"MPC Video Renderer"), "AddFilter renderer");
        Com<IPin> pmIn, pmOut, rIn;
        std::vector<IPin*> others;
        if (ok) { pmIn.p = FindPin(pm.p, PINDIR_INPUT); ok = pmIn.p != nullptr; if (!ok) state = "no filter input pin"; }
        if (ok) {
            bool linked = false;
            Com<IEnumPins> e;
            if (SUCCEEDED(source->EnumPins(&e))) {
                IPin* pin = nullptr;
                while (e->Next(1, &pin, nullptr) == S_OK) {
                    PIN_DIRECTION d; pin->QueryDirection(&d);
                    if (d == PINDIR_OUTPUT && !linked && SUCCEEDED(graph->Connect(pin, pmIn.p))) { linked = true; pin->Release(); continue; }
                    if (d == PINDIR_OUTPUT) others.push_back(pin); else pin->Release();
                }
            }
            ok = linked;
            if (!ok) state = "no source pin connects to the filter";
        }
        if (ok) {
            pmOut.p = FindPin(pm.p, PINDIR_OUTPUT); rIn.p = FindPin(renderer.p, PINDIR_INPUT);
            ok = pmOut && rIn && step(graph->Connect(pmOut.p, rIn.p), "filter -> MPC Video Renderer");
        }
        for (IPin* p : others) { if (ok) graph->Render(p); p->Release(); }
        Com<IMediaControl> mc; Com<IMediaEvent> me;
        if (ok) ok = step(graph->QueryInterface(IID_PPV_ARGS(&mc)), "IMediaControl") && step(mc->Run(), "Run");
        if (ok) {
            graph->QueryInterface(IID_PPV_ARGS(&me));
            played = true; state = "played";
            // Auto rebuilds: Smoother 2, then 3, then the user's own value again (15 s apart, after 20 s of playback)
            struct Change { ULONGLONG atMs; std::wstring v; };
            const std::vector<Change> changes = { { 20000, L"2" }, { 35000, L"3" }, { 50000, smootherBefore } };
            size_t next = 0;
            const ULONGLONG t0 = GetTickCount64(), end = t0 + ULONGLONG(seconds) * 1000;
            while (GetTickCount64() < end) {
                MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
                if (next < changes.size() && GetTickCount64() - t0 >= changes[next].atMs) {
                    RegPut(L"Smoother", changes[next].v);
                    std::printf("t=%llus Smoother=%ls\n", (GetTickCount64() - t0) / 1000, changes[next].v.empty() ? L"(removed)" : changes[next].v.c_str());
                    ++next;
                }
                if (me) { long ev = 0; LONG_PTR a = 0, b = 0; while (me->GetEvent(&ev, &a, &b, 0) == S_OK) { me->FreeEventParams(ev, a, b); if (ev == EC_ERRORABORT) state = "error abort"; else if (ev == EC_COMPLETE) state = "played to the end"; } }
                Sleep(20);
            }
            const ULONGLONG s0 = GetTickCount64();
            mc->Stop();
            std::printf("stop took %llu ms\n", GetTickCount64() - s0);
        }
        RegPut(L"Smoother", smootherBefore);
        if (hwnd) DestroyWindow(hwnd);
    }   // graph and filter released here
    // the filter's own log of this process
    const std::wstring log = FilterLog();
    std::map<std::string, int> count;
    const char* keys[] = { "debug ReloadTimeoutMs", "did not finish", "its graph is handed to the filter", "is taken over", "is not taken over",
                           "settings reloaded", "runtime fallback", "no-output guard", "device lost", "Stop done" };
    if (!log.empty()) {
        std::ifstream f(log);
        for (std::string line; std::getline(f, line);) {
            for (const char* k : keys) if (line.find(k) != std::string::npos) ++count[k];
            if (line.find("taken over") != std::string::npos || line.find("handed to the filter") != std::string::npos || line.find("did not finish") != std::string::npos)
                std::printf("  log: %s\n", line.substr(0, 300).c_str());
        }
    }
    std::printf("result played=%d state=%s log=%ls", played ? 1 : 0, state.c_str(), log.empty() ? L"(not found)" : log.c_str());
    for (const char* k : keys) std::printf(" [%s]=%d", k, count[k]);
    std::printf("\n");
    CoUninitialize();
    if (!played || state == "error abort") return 1;
    return count["is taken over"] > 0 ? 0 : 2;
}
