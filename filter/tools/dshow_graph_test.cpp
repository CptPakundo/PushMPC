// dshow_graph_test: PushMPC's DirectShow filter in a graph the way players other than MPC-HC build it (KMPlayer, GOM
// Player, Zoom Player: the system's decoder by Intelligent Connect, then the filter, then EVR / VMR-9 / madVR / MPC Video
// Renderer / the Null Renderer). It plays a clip for some seconds and prints one line:
//   result renderer=<name> connected=<0|1> in_fps=<f> out_fps=<f> multiplier=<k> frames_drawn=<n> avg_fps=<f> state=<...>
// k is the output frame rate over the input frame rate the filter agreed on (its output media type); frames_drawn and
// avg_fps come from the renderer's IQualProp where it has one. Exit code 0 when the graph played (connected and running),
// 1 when it did not, 2 with --expect-fg when it played but the filter did not raise the frame rate.
// Usage: dshow_graph_test <clip> <evr|vmr9|madvr|mpcvr|null> [seconds=15] [--expect-fg]
// Build: cl /nologo /std:c++20 /O2 /EHsc /W3 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX dshow_graph_test.cpp
//        /link strmiids.lib ole32.lib oleaut32.lib user32.lib uuid.lib mfuuid.lib
#include <windows.h>
#include <dshow.h>
#include <dvdmedia.h>
#include <evr.h>
#include <mfidl.h>
#include <amvideo.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>
#include <cwchar>
#include <string>

#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "mfuuid.lib")

namespace {

// PushMPC Filter (filter/src/PushMpcFilter.cpp) and the renderers
const CLSID kPushMpc = { 0x798D4EC3, 0xE59C, 0x4A4E, { 0xA2, 0xEE, 0xC8, 0x04, 0xAE, 0xAE, 0x04, 0xCC } };
const CLSID kEvr = { 0xFA10746C, 0x9B63, 0x4B6C, { 0xBC, 0x49, 0xFC, 0x30, 0x0E, 0xA5, 0xF2, 0x56 } };
const CLSID kVmr9 = { 0x51B4ABF3, 0x748F, 0x4E3B, { 0xA2, 0x76, 0xC8, 0x28, 0x33, 0x0E, 0x92, 0x6A } };
const CLSID kMadVr = { 0xE1A8B82A, 0x32CE, 0x4B0D, { 0xBE, 0x0D, 0xAA, 0x68, 0xC7, 0x72, 0xE4, 0x23 } };
const CLSID kMpcVr = { 0x71F080AA, 0x8661, 0x4093, { 0xB1, 0x5E, 0x4F, 0x69, 0x03, 0xE7, 0x7D, 0x0A } };
const CLSID kNullRenderer = { 0xC1F400A4, 0x3F08, 0x11D3, { 0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37 } };

template <class T> struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

IPin* FindPin(IBaseFilter* f, PIN_DIRECTION want, bool unconnected)
{
    Com<IEnumPins> e;
    if (FAILED(f->EnumPins(&e))) return nullptr;
    IPin* pin = nullptr;
    while (e->Next(1, &pin, nullptr) == S_OK) {
        PIN_DIRECTION d;
        pin->QueryDirection(&d);
        Com<IPin> peer;
        const bool connected = SUCCEEDED(pin->ConnectedTo(&peer));
        if (d == want && (!unconnected || !connected)) return pin;
        pin->Release();
    }
    return nullptr;
}

double FpsOf(const AM_MEDIA_TYPE& mt)
{
    REFERENCE_TIME t = 0;
    if (mt.formattype == FORMAT_VideoInfo2 && mt.cbFormat >= sizeof(VIDEOINFOHEADER2)) t = reinterpret_cast<VIDEOINFOHEADER2*>(mt.pbFormat)->AvgTimePerFrame;
    else if (mt.formattype == FORMAT_VideoInfo && mt.cbFormat >= sizeof(VIDEOINFOHEADER)) t = reinterpret_cast<VIDEOINFOHEADER*>(mt.pbFormat)->AvgTimePerFrame;
    return t > 0 ? 1e7 / double(t) : 0;
}

double PinFps(IPin* pin)
{
    AM_MEDIA_TYPE mt{};
    if (!pin || FAILED(pin->ConnectionMediaType(&mt))) return 0;
    const double f = FpsOf(mt);
    if (mt.cbFormat) CoTaskMemFree(mt.pbFormat);
    if (mt.pUnk) mt.pUnk->Release();
    return f;
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3) { std::fwprintf(stderr, L"usage: dshow_graph_test <clip> <evr|vmr9|madvr|mpcvr|null> [seconds] [--expect-fg]\n"); return 1; }
    const std::wstring clip = argv[1], rname = argv[2];
    int seconds = 15;
    bool expectFg = false;
    for (int i = 3; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--expect-fg") == 0) expectFg = true;
        else seconds = std::max(2, _wtoi(argv[i]));
    }
    const CLSID* rc = rname == L"evr" ? &kEvr : rname == L"vmr9" ? &kVmr9 : rname == L"madvr" ? &kMadVr : rname == L"mpcvr" ? &kMpcVr : rname == L"null" ? &kNullRenderer : nullptr;
    if (!rc) { std::fwprintf(stderr, L"unknown renderer %ls\n", rname.c_str()); return 1; }
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    int code = 1;
    std::string state = "start";
    double inFps = 0, outFps = 0, avgFps = 0;
    int framesDrawn = -1;
    bool connected = false;
    {
        // a window for the renderers that need one (EVR's video window; VMR-9 and madVR make their own otherwise)
        WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"PushMpcGraphTest";
        RegisterClassW(&wc);
        HWND hwnd = CreateWindowW(wc.lpszClassName, L"PushMPC graph test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 640, 360, nullptr, nullptr, wc.hInstance, nullptr);

        Com<IGraphBuilder> graph; Com<IBaseFilter> source, pm, renderer;
        auto step = [&](HRESULT hr, const char* what) { if (FAILED(hr)) { char b[160]; std::snprintf(b, sizeof(b), "%s failed 0x%08lX", what, hr); state = b; } return SUCCEEDED(hr); };
        bool ok = step(CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&graph)), "FilterGraph")
               && step(graph->AddSourceFilter(clip.c_str(), L"source", &source), "AddSourceFilter")
               && step(CoCreateInstance(kPushMpc, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pm)), "PushMPC Filter (not registered?)")
               && step(graph->AddFilter(pm.p, L"PushMPC Filter"), "AddFilter PushMPC")
               && step(CoCreateInstance(*rc, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&renderer)), "renderer (not registered?)")
               && step(graph->AddFilter(renderer.p, L"renderer"), "AddFilter renderer");
        if (ok && *rc == kEvr) {
            Com<IMFGetService> gs; Com<IMFVideoDisplayControl> dc;
            if (SUCCEEDED(renderer->QueryInterface(IID_PPV_ARGS(&gs))) && SUCCEEDED(gs->GetService(MR_VIDEO_RENDER_SERVICE, IID_PPV_ARGS(&dc)))) {
                dc->SetVideoWindow(hwnd);
                RECT r{ 0, 0, 640, 360 }; dc->SetVideoPosition(nullptr, &r);
            }
        }
        // source -> (the system's decoder, Intelligent Connect) -> PushMPC Filter: the first source pin that connects
        Com<IPin> pmIn, pmOut, rIn;
        if (ok) {
            IPin* in = FindPin(pm.p, PINDIR_INPUT, true);
            pmIn.p = in;
            ok = in != nullptr;
            if (!ok) state = "PushMPC Filter has no input pin";
        }
        std::vector<IPin*> others;   // the source's other output pins (audio and the rest): rendered once video is linked
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
            if (!ok) state = "no source pin connects to PushMPC Filter (decoder missing or the filter refused the type)";
        }
        if (ok) {
            pmOut.p = FindPin(pm.p, PINDIR_OUTPUT, true);
            rIn.p = FindPin(renderer.p, PINDIR_INPUT, true);
            ok = pmOut && rIn && step(graph->Connect(pmOut.p, rIn.p), "PushMPC Filter -> renderer");
        }
        // the remaining source pins (audio): rendered, so the graph has its clock and plays as a player's would
        // (no audio device on a hosted runner: a failed audio render leaves the pin open and the graph plays on its clock)
        for (IPin* p : others) { if (ok) graph->Render(p); p->Release(); }
        connected = ok;
        if (ok) {
            inFps = PinFps(pmIn.p); outFps = PinFps(pmOut.p);
            Com<IMediaControl> mc; Com<IMediaEvent> me;
            ok = step(graph->QueryInterface(IID_PPV_ARGS(&mc)), "IMediaControl") && step(mc->Run(), "Run");
            graph->QueryInterface(IID_PPV_ARGS(&me));
            if (ok) {
                state = "played";
                const ULONGLONG end = GetTickCount64() + ULONGLONG(seconds) * 1000;
                while (GetTickCount64() < end) {
                    MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
                    if (me) { long ev = 0; LONG_PTR a = 0, b = 0; while (me->GetEvent(&ev, &a, &b, 0) == S_OK) { me->FreeEventParams(ev, a, b); if (ev == EC_COMPLETE) state = "played to the end"; else if (ev == EC_ERRORABORT) state = "error abort"; } }
                    Sleep(20);
                }
                // what the output pin carries now (the filter may reconnect after its first frames)
                outFps = PinFps(pmOut.p) > 0 ? PinFps(pmOut.p) : outFps;
                Com<IQualProp> qp;
                if (SUCCEEDED(renderer->QueryInterface(IID_IQualProp, reinterpret_cast<void**>(&qp)))) { int n = 0, a = 0; if (SUCCEEDED(qp->get_FramesDrawn(&n))) framesDrawn = n; if (SUCCEEDED(qp->get_AvgFrameRate(&a))) avgFps = a / 100.0; }
                mc->Stop();
                code = state == "error abort" ? 1 : 0;
            }
        }
        if (hwnd) DestroyWindow(hwnd);
    }
    const double k = inFps > 0 && outFps > 0 ? outFps / inFps : 0;
    if (code == 0 && expectFg && k < 1.8 && !(inFps > 0 && avgFps >= 1.8 * inFps)) code = 2;
    std::printf("result renderer=%ls connected=%d in_fps=%.3f out_fps=%.3f multiplier=%.2f frames_drawn=%d avg_fps=%.2f state=%s\n",
                rname.c_str(), connected ? 1 : 0, inFps, outFps, k, framesDrawn, avgFps, state.c_str());
    CoUninitialize();
    return code;
}
