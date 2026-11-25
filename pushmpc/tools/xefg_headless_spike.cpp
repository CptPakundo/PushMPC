// xefg_headless_spike: drives Intel XeSS frame generation (XeFG) through a headless
// IDXGISwapChain4 shim. No window, no real DXGI swap chain.
//
// What it does:
//   * creates a D3D12 device (hardware adapter or WARP),
//   * gives XeFG a self-implemented IDXGISwapChain4 (ShimSwapChain) created by a
//     self-implemented IDXGIFactory7 (ShimFactory). XeFG releases the application swap
//     chain and re-creates its native swap chain through the factory; that one is a shim
//     as well. Its Present() copies every presented buffer (real or generated) to a
//     readback buffer on the presenting queue,
//   * hooks a few imports of libxess_fg.dll (CreateDXGIFactory1, the USER32 window and
//     monitor queries, waits, sleeps, GetProcAddress) so that a fake HWND and a virtual
//     monitor are enough and so that every touch outside the shim is logged,
//   * renders N synthetic frames on the CPU (a textured square moving at a known speed
//     over a static textured background, exact motion vectors, constant depth),
//   * presents them through XeFG with 1 (2X) or more (MFG unlock) interpolated frames,
//   * reports init results, outputs per real frame, pacing of the shim Present calls,
//     every shim method XeFG called, and image sanity of the generated frames (square
//     position vs the expected intermediate position, PSNR vs an ideal rendered frame).
//
// Build: pushmpc/tools/xefg_headless_spike.cmd (needs only the XeSS SDK headers).
// Runtime: libxess_fg.dll and libxell.dll next to the exe or in --xess-dir.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <bcrypt.h>
#include <timeapi.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <immintrin.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <xell/xell_d3d12.h>
#include <xess_fg/xefg_swapchain_d3d12.h>
#include <xess_fg/xefg_swapchain_debug.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "winmm.lib")

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------------------
struct Options {
    bool warp = false;
    int adapter = -1;          // -1: first hardware adapter
    uint32_t width = 640, height = 360;
    uint32_t frames = 60;      // real frames in the sanity phase
    uint32_t interp = 1;       // interpolated frames per real frame
    double fps = 30.0;         // app frame rate in the sanity phase (0 = unthrottled)
    double vx = 6.0, vy = 1.5; // square speed in px per real frame
    uint32_t square = 72;
    double refreshHz = 240.0;  // virtual monitor refresh
    bool emulateVsync = false; // Present with SyncInterval>0 sleeps to the next virtual vblank
    bool realWindow = false;   // hidden real window instead of a fake HWND
    bool outputShim = true;    // GetContainingOutput returns a virtual output
    bool unlock = true;        // MFG unlock when interp > 1
    bool lowLatency = true;    // XeLL bLowLatencyMode (XeFG refuses SetEnabled without it)
    bool debugLayer = false;
    uint32_t timingFrames = 40; // unthrottled timing phases (FG on / FG off), 0 = skip
    uint32_t pngCount = 3;
    uint32_t maxCaptures = 1200;
    int verboseCalls = 6;      // detailed log lines per shim method
    std::wstring outDir = L"xefg-spike-out";
    std::wstring xessDir;      // default: exe directory
};
static Options g_opt;

// ---------------------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------------------
static FILE* g_logFile = nullptr;
static FILE* g_reportFile = nullptr;
static std::mutex g_logMx;
static LARGE_INTEGER g_qpf{}, g_qp0{};

static double nowMs() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return double(q.QuadPart - g_qp0.QuadPart) * 1000.0 / double(g_qpf.QuadPart);
}

static void logf(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard l(g_logMx);
    const double t = nowMs();
    fprintf(stdout, "[%10.3f t%05lu] %s\n", t, GetCurrentThreadId(), buf);
    fflush(stdout);
    if (g_logFile) {
        fprintf(g_logFile, "[%10.3f t%05lu] %s\n", t, GetCurrentThreadId(), buf);
        fflush(g_logFile);
    }
}

// Report lines go to report.txt and to the log.
static void reportf(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    {
        std::lock_guard l(g_logMx);
        if (g_reportFile) {
            fprintf(g_reportFile, "%s\n", buf);
            fflush(g_reportFile);
        }
    }
    logf("REPORT %s", buf);
}

static std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// ---------------------------------------------------------------------------------------
// IID names
// ---------------------------------------------------------------------------------------
static std::string iidName(REFIID riid) {
    struct Known { const IID* iid; const char* name; };
    static const Known known[] = {
        {&__uuidof(IUnknown), "IUnknown"},
        {&__uuidof(IDXGIObject), "IDXGIObject"},
        {&__uuidof(IDXGIDeviceSubObject), "IDXGIDeviceSubObject"},
        {&__uuidof(IDXGISwapChain), "IDXGISwapChain"},
        {&__uuidof(IDXGISwapChain1), "IDXGISwapChain1"},
        {&__uuidof(IDXGISwapChain2), "IDXGISwapChain2"},
        {&__uuidof(IDXGISwapChain3), "IDXGISwapChain3"},
        {&__uuidof(IDXGISwapChain4), "IDXGISwapChain4"},
        {&__uuidof(IDXGISwapChainMedia), "IDXGISwapChainMedia"},
        {&__uuidof(IDXGIFactory), "IDXGIFactory"},
        {&__uuidof(IDXGIFactory1), "IDXGIFactory1"},
        {&__uuidof(IDXGIFactory2), "IDXGIFactory2"},
        {&__uuidof(IDXGIFactory3), "IDXGIFactory3"},
        {&__uuidof(IDXGIFactory4), "IDXGIFactory4"},
        {&__uuidof(IDXGIFactory5), "IDXGIFactory5"},
        {&__uuidof(IDXGIFactory6), "IDXGIFactory6"},
        {&__uuidof(IDXGIFactory7), "IDXGIFactory7"},
        {&__uuidof(IDXGIOutput), "IDXGIOutput"},
        {&__uuidof(IDXGIOutput1), "IDXGIOutput1"},
        {&__uuidof(IDXGIOutput2), "IDXGIOutput2"},
        {&__uuidof(IDXGIOutput3), "IDXGIOutput3"},
        {&__uuidof(IDXGIOutput4), "IDXGIOutput4"},
        {&__uuidof(IDXGIOutput5), "IDXGIOutput5"},
        {&__uuidof(IDXGIOutput6), "IDXGIOutput6"},
        {&__uuidof(IDXGIAdapter), "IDXGIAdapter"},
        {&__uuidof(IDXGIAdapter1), "IDXGIAdapter1"},
        {&__uuidof(IDXGIAdapter2), "IDXGIAdapter2"},
        {&__uuidof(IDXGIAdapter3), "IDXGIAdapter3"},
        {&__uuidof(IDXGIAdapter4), "IDXGIAdapter4"},
        {&__uuidof(ID3D12CommandQueue), "ID3D12CommandQueue"},
        {&__uuidof(ID3D12Device), "ID3D12Device"},
        {&__uuidof(ID3D12Resource), "ID3D12Resource"},
        {&__uuidof(ID3D12Resource1), "ID3D12Resource1"},
        {&__uuidof(ID3D12Pageable), "ID3D12Pageable"},
        {&__uuidof(ID3D12DeviceChild), "ID3D12DeviceChild"},
        {&__uuidof(ID3D12Object), "ID3D12Object"},
    };
    for (const auto& k : known)
        if (IsEqualIID(riid, *k.iid)) return k.name;
    // NVIDIA Streamline proxy probe (XeFG asks the factory and the swap chain for it).
    static const IID kStreamlineBase = {0xADEC44E2, 0x61F0, 0x45C3, {0xAD, 0x9F, 0x1B, 0x37, 0x37, 0x92, 0x84, 0xFF}};
    if (IsEqualIID(riid, kStreamlineBase)) return "IStreamlineBaseInterface{ADEC44E2}";
    wchar_t buf[64]{};
    StringFromGUID2(riid, buf, 64);
    return narrow(buf);
}

// ---------------------------------------------------------------------------------------
// Call accounting: every shim method and hook goes through traceCall.
// ---------------------------------------------------------------------------------------
struct CallInfo {
    uint64_t count = 0;
    double firstMs = 0, lastMs = 0;
    std::map<DWORD, uint64_t> threads;
};
static std::mutex g_callMx;
static std::map<std::string, CallInfo> g_calls;

static void traceCall(const char* name, const char* fmt = nullptr, ...) {
    uint64_t n;
    const double t = nowMs();
    {
        std::lock_guard l(g_callMx);
        auto& c = g_calls[name];
        n = ++c.count;
        if (n == 1) c.firstMs = t;
        c.lastMs = t;
        ++c.threads[GetCurrentThreadId()];
    }
    if (int64_t(n) <= g_opt.verboseCalls) {
        char buf[2048] = "";
        if (fmt) {
            va_list ap;
            va_start(ap, fmt);
            vsnprintf(buf, sizeof(buf), fmt, ap);
            va_end(ap);
        }
        logf("CALL %s #%llu %s", name, (unsigned long long)n, buf);
    }
}

// ---------------------------------------------------------------------------------------
// Virtual display clock
// ---------------------------------------------------------------------------------------
static uint64_t vblankIndexNow(LARGE_INTEGER* qpcOfVblank = nullptr) {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    const double sec = double(q.QuadPart - g_qp0.QuadPart) / double(g_qpf.QuadPart);
    const uint64_t idx = uint64_t(sec * g_opt.refreshHz);
    if (qpcOfVblank)
        qpcOfVblank->QuadPart = g_qp0.QuadPart + int64_t(double(idx) / g_opt.refreshHz * double(g_qpf.QuadPart));
    return idx;
}

static void sleepUntilQpc(int64_t target) {
    for (;;) {
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        const int64_t left = target - q.QuadPart;
        if (left <= 0) return;
        const double ms = double(left) * 1000.0 / double(g_qpf.QuadPart);
        if (ms > 2.0) Sleep(DWORD(ms - 1.5));
        else YieldProcessor();
    }
}

static void waitNextVblank(uint32_t count) {
    const uint64_t idx = vblankIndexNow();
    const int64_t target = g_qp0.QuadPart + int64_t(double(idx + std::max(1u, count)) / g_opt.refreshHz * double(g_qpf.QuadPart));
    sleepUntilQpc(target);
}

// ---------------------------------------------------------------------------------------
// Private data store (IDXGIObject)
// ---------------------------------------------------------------------------------------
struct GuidLess {
    bool operator()(const GUID& a, const GUID& b) const { return memcmp(&a, &b, sizeof(GUID)) < 0; }
};
struct PrivateData {
    std::mutex mx;
    std::map<GUID, std::vector<uint8_t>, GuidLess> data;
    std::map<GUID, ComPtr<IUnknown>, GuidLess> ifaces;
    HRESULT set(REFGUID g, UINT size, const void* p) {
        std::lock_guard l(mx);
        ifaces.erase(g);
        if (!p || !size) { data.erase(g); return S_OK; }
        data[g].assign((const uint8_t*)p, (const uint8_t*)p + size);
        return S_OK;
    }
    HRESULT setI(REFGUID g, const IUnknown* u) {
        std::lock_guard l(mx);
        data.erase(g);
        if (!u) { ifaces.erase(g); return S_OK; }
        ifaces[g] = const_cast<IUnknown*>(u);
        return S_OK;
    }
    HRESULT get(REFGUID g, UINT* size, void* p) {
        if (!size) return E_INVALIDARG;
        std::lock_guard l(mx);
        if (auto it = ifaces.find(g); it != ifaces.end()) {
            if (!p) { *size = sizeof(IUnknown*); return S_OK; }
            if (*size < sizeof(IUnknown*)) return DXGI_ERROR_MORE_DATA;
            IUnknown* u = it->second.Get();
            u->AddRef();
            memcpy(p, &u, sizeof(u));
            *size = sizeof(IUnknown*);
            return S_OK;
        }
        auto it = data.find(g);
        if (it == data.end()) { *size = 0; return DXGI_ERROR_NOT_FOUND; }
        if (!p) { *size = UINT(it->second.size()); return S_OK; }
        if (*size < it->second.size()) { *size = UINT(it->second.size()); return DXGI_ERROR_MORE_DATA; }
        memcpy(p, it->second.data(), it->second.size());
        *size = UINT(it->second.size());
        return S_OK;
    }
};

// ---------------------------------------------------------------------------------------
// Captures of presented frames
// ---------------------------------------------------------------------------------------
struct Capture {
    uint32_t serial = 0;       // order of shim Present calls (all swap chains)
    uint32_t swapSerial = 0;   // which shim swap chain
    uint32_t phase = 0;        // 1 sanity, 2 timing FG on, 3 timing FG off
    uint32_t appFrame = 0;     // last app frame id whose Present had started
    double tEnter = 0, tExit = 0;
    DWORD tid = 0;
    UINT sync = 0, flags = 0, buffer = 0;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Resource> readback;
    ComPtr<ID3D12Fence> fence;
    uint64_t fenceValue = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    bool hasImage = false;
    std::vector<uint8_t> rgba; // tightly packed after readback
};
static std::mutex g_capMx;
static std::vector<std::unique_ptr<Capture>> g_captures;
static std::atomic<uint32_t> g_presentSerial{0};
static std::atomic<uint32_t> g_phase{0};
static std::atomic<uint32_t> g_appFrame{0};
static std::atomic<bool> g_captureImages{true};
// Last fence signalled on the presenting queue by a shim Present (GPU completion of XeFG output).
static std::mutex g_lastPresentMx;
static ComPtr<ID3D12Fence> g_lastPresentFence;
static uint64_t g_lastPresentValue = 0;

// ---------------------------------------------------------------------------------------
// Fake window and virtual monitor
// ---------------------------------------------------------------------------------------
static HWND g_hwnd = reinterpret_cast<HWND>(uintptr_t(0x5E1F0));
static bool g_hwndFake = true;
static const HMONITOR kFakeMonitor = reinterpret_cast<HMONITOR>(uintptr_t(0x5E1F4));
static const char* kVirtualDeviceA = "\\\\.\\PUSHMPC_VIRTUAL1";
static const wchar_t* kVirtualDeviceW = L"\\\\.\\PUSHMPC_VIRTUAL1";

// ---------------------------------------------------------------------------------------
// ShimOutput: a virtual IDXGIOutput6
// ---------------------------------------------------------------------------------------
class ShimOutput final : public IDXGIOutput6 {
public:
    explicit ShimOutput(IDXGIAdapter* adapter) : adapter_(adapter) {}
    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
        if (!pp) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIOutput) ||
            riid == __uuidof(IDXGIOutput1) || riid == __uuidof(IDXGIOutput2) || riid == __uuidof(IDXGIOutput3) ||
            riid == __uuidof(IDXGIOutput4) || riid == __uuidof(IDXGIOutput5) || riid == __uuidof(IDXGIOutput6)) {
            traceCall("Output::QueryInterface", "riid=%s -> S_OK", iidName(riid).c_str());
            *pp = static_cast<IDXGIOutput6*>(this);
            AddRef();
            return S_OK;
        }
        traceCall("Output::QueryInterface(unknown)", "riid=%s -> E_NOINTERFACE", iidName(riid).c_str());
        *pp = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --ref_;
        if (!r) delete this;
        return r;
    }
    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT s, const void* p) override { traceCall("Output::SetPrivateData"); return pd_.set(g, s, p); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g, const IUnknown* u) override { traceCall("Output::SetPrivateDataInterface"); return pd_.setI(g, u); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT* s, void* p) override { traceCall("Output::GetPrivateData"); return pd_.get(g, s, p); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** pp) override {
        traceCall("Output::GetParent", "riid=%s", iidName(riid).c_str());
        if (!adapter_) return E_NOINTERFACE;
        return adapter_->QueryInterface(riid, pp);
    }
    // IDXGIOutput
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_OUTPUT_DESC* d) override {
        traceCall("Output::GetDesc");
        if (!d) return E_INVALIDARG;
        *d = {};
        wcsncpy_s(d->DeviceName, kVirtualDeviceW, _TRUNCATE);
        d->DesktopCoordinates = {0, 0, LONG(std::max(1920u, g_opt.width)), LONG(std::max(1080u, g_opt.height))};
        d->AttachedToDesktop = TRUE;
        d->Rotation = DXGI_MODE_ROTATION_IDENTITY;
        d->Monitor = kFakeMonitor;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDisplayModeList(DXGI_FORMAT f, UINT flags, UINT* n, DXGI_MODE_DESC* m) override {
        traceCall("Output::GetDisplayModeList", "fmt=%d flags=0x%x", int(f), flags);
        if (!n) return E_INVALIDARG;
        if (!m) { *n = 1; return S_OK; }
        if (*n < 1) return DXGI_ERROR_MORE_DATA;
        *n = 1;
        fillMode(f, m);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE FindClosestMatchingMode(const DXGI_MODE_DESC* want, DXGI_MODE_DESC* out, IUnknown*) override {
        traceCall("Output::FindClosestMatchingMode");
        if (!out) return E_INVALIDARG;
        fillMode(want ? want->Format : DXGI_FORMAT_R8G8B8A8_UNORM, out);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE WaitForVBlank() override {
        const double t0 = nowMs();
        waitNextVblank(1);
        traceCall("Output::WaitForVBlank", "waited %.3f ms", nowMs() - t0);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE TakeOwnership(IUnknown*, BOOL) override { traceCall("Output::TakeOwnership"); return DXGI_ERROR_UNSUPPORTED; }
    void STDMETHODCALLTYPE ReleaseOwnership() override { traceCall("Output::ReleaseOwnership"); }
    HRESULT STDMETHODCALLTYPE GetGammaControlCapabilities(DXGI_GAMMA_CONTROL_CAPABILITIES*) override { traceCall("Output::GetGammaControlCapabilities"); return DXGI_ERROR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE SetGammaControl(const DXGI_GAMMA_CONTROL*) override { traceCall("Output::SetGammaControl"); return DXGI_ERROR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetGammaControl(DXGI_GAMMA_CONTROL*) override { traceCall("Output::GetGammaControl"); return DXGI_ERROR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE SetDisplaySurface(IDXGISurface*) override { traceCall("Output::SetDisplaySurface"); return DXGI_ERROR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetDisplaySurfaceData(IDXGISurface*) override { traceCall("Output::GetDisplaySurfaceData"); return DXGI_ERROR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* s) override {
        traceCall("Output::GetFrameStatistics");
        if (!s) return E_INVALIDARG;
        *s = {};
        LARGE_INTEGER q;
        const uint64_t idx = vblankIndexNow(&q);
        s->SyncRefreshCount = UINT(idx);
        s->SyncQPCTime = q;
        return S_OK;
    }
    // IDXGIOutput1
    HRESULT STDMETHODCALLTYPE GetDisplayModeList1(DXGI_FORMAT f, UINT flags, UINT* n, DXGI_MODE_DESC1* m) override {
        traceCall("Output::GetDisplayModeList1", "fmt=%d flags=0x%x", int(f), flags);
        if (!n) return E_INVALIDARG;
        if (!m) { *n = 1; return S_OK; }
        if (*n < 1) return DXGI_ERROR_MORE_DATA;
        *n = 1;
        DXGI_MODE_DESC md;
        fillMode(f, &md);
        *m = {};
        m->Width = md.Width; m->Height = md.Height; m->RefreshRate = md.RefreshRate; m->Format = md.Format;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE FindClosestMatchingMode1(const DXGI_MODE_DESC1* want, DXGI_MODE_DESC1* out, IUnknown*) override {
        traceCall("Output::FindClosestMatchingMode1");
        if (!out) return E_INVALIDARG;
        DXGI_MODE_DESC md;
        fillMode(want ? want->Format : DXGI_FORMAT_R8G8B8A8_UNORM, &md);
        *out = {};
        out->Width = md.Width; out->Height = md.Height; out->RefreshRate = md.RefreshRate; out->Format = md.Format;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDisplaySurfaceData1(IDXGIResource*) override { traceCall("Output::GetDisplaySurfaceData1"); return DXGI_ERROR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DuplicateOutput(IUnknown*, IDXGIOutputDuplication**) override { traceCall("Output::DuplicateOutput"); return DXGI_ERROR_UNSUPPORTED; }
    // IDXGIOutput2
    BOOL STDMETHODCALLTYPE SupportsOverlays() override { traceCall("Output::SupportsOverlays"); return FALSE; }
    // IDXGIOutput3
    HRESULT STDMETHODCALLTYPE CheckOverlaySupport(DXGI_FORMAT, IUnknown*, UINT* f) override { traceCall("Output::CheckOverlaySupport"); if (f) *f = 0; return DXGI_ERROR_UNSUPPORTED; }
    // IDXGIOutput4
    HRESULT STDMETHODCALLTYPE CheckOverlayColorSpaceSupport(DXGI_FORMAT, DXGI_COLOR_SPACE_TYPE, IUnknown*, UINT* f) override { traceCall("Output::CheckOverlayColorSpaceSupport"); if (f) *f = 0; return DXGI_ERROR_UNSUPPORTED; }
    // IDXGIOutput5
    HRESULT STDMETHODCALLTYPE DuplicateOutput1(IUnknown*, UINT, UINT, const DXGI_FORMAT*, IDXGIOutputDuplication**) override { traceCall("Output::DuplicateOutput1"); return DXGI_ERROR_UNSUPPORTED; }
    // IDXGIOutput6
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_OUTPUT_DESC1* d) override {
        traceCall("Output::GetDesc1");
        if (!d) return E_INVALIDARG;
        DXGI_OUTPUT_DESC d0;
        GetDesc(&d0);
        *d = {};
        memcpy(d->DeviceName, d0.DeviceName, sizeof(d->DeviceName));
        d->DesktopCoordinates = d0.DesktopCoordinates;
        d->AttachedToDesktop = TRUE;
        d->Rotation = DXGI_MODE_ROTATION_IDENTITY;
        d->Monitor = kFakeMonitor;
        d->BitsPerColor = 8;
        d->ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        d->RedPrimary[0] = 0.64f; d->RedPrimary[1] = 0.33f;
        d->GreenPrimary[0] = 0.30f; d->GreenPrimary[1] = 0.60f;
        d->BluePrimary[0] = 0.15f; d->BluePrimary[1] = 0.06f;
        d->WhitePoint[0] = 0.3127f; d->WhitePoint[1] = 0.3290f;
        d->MinLuminance = 0.1f; d->MaxLuminance = 300.f; d->MaxFullFrameLuminance = 300.f;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CheckHardwareCompositionSupport(UINT* f) override { traceCall("Output::CheckHardwareCompositionSupport"); if (f) *f = 0; return S_OK; }

private:
    static void fillMode(DXGI_FORMAT f, DXGI_MODE_DESC* m) {
        *m = {};
        m->Width = std::max(1920u, g_opt.width);
        m->Height = std::max(1080u, g_opt.height);
        m->RefreshRate = {UINT(std::lround(g_opt.refreshHz * 1000.0)), 1000};
        m->Format = f == DXGI_FORMAT_UNKNOWN ? DXGI_FORMAT_R8G8B8A8_UNORM : f;
    }
    std::atomic<ULONG> ref_{1};
    ComPtr<IDXGIAdapter> adapter_;
    PrivateData pd_;
};

static ComPtr<IDXGIAdapter> g_adapter;

// ---------------------------------------------------------------------------------------
// ShimSwapChain
// ---------------------------------------------------------------------------------------
class ShimFactory;
static std::atomic<uint32_t> g_swapSerial{0};
static std::atomic<uint32_t> g_swapAlive{0};

class ShimSwapChain final : public IDXGISwapChain4 {
public:
    ShimSwapChain(IDXGIFactory* factory, ID3D12CommandQueue* queue, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1& desc,
                  const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs)
        : factory_(factory), queue_(queue), hwnd_(hwnd), desc_(desc), serial_(++g_swapSerial) {
        if (fs) fsDesc_ = *fs;
        else { fsDesc_ = {}; fsDesc_.Windowed = TRUE; }
        fsDesc_.RefreshRate = {UINT(std::lround(g_opt.refreshHz * 1000.0)), 1000};
        ++g_swapAlive;
    }
    ~ShimSwapChain() {
        --g_swapAlive;
        if (latencySem_) CloseHandle(latencySem_);
        logf("SHIM swapchain #%u destroyed (presents=%u)", serial_, presentCount_);
    }
    HRESULT init() {
        HRESULT hr = queue_->GetDevice(IID_PPV_ARGS(&device_));
        if (FAILED(hr)) return hr;
        hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
        if (FAILED(hr)) return hr;
        queueType_ = queue_->GetDesc().Type;
        if (desc_.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
            maxLatency_ = 1;
            latencySem_ = CreateSemaphoreW(nullptr, LONG(maxLatency_), 16, nullptr);
        }
        return createBuffers();
    }
    uint32_t serial() const { return serial_; }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
        if (!pp) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDeviceSubObject) ||
            riid == __uuidof(IDXGISwapChain) || riid == __uuidof(IDXGISwapChain1) || riid == __uuidof(IDXGISwapChain2) ||
            riid == __uuidof(IDXGISwapChain3) || riid == __uuidof(IDXGISwapChain4)) {
            traceCall("SwapChain::QueryInterface", "#%u riid=%s -> S_OK", serial_, iidName(riid).c_str());
            *pp = static_cast<IDXGISwapChain4*>(this);
            AddRef();
            return S_OK;
        }
        traceCall("SwapChain::QueryInterface(unknown)", "#%u riid=%s -> E_NOINTERFACE", serial_, iidName(riid).c_str());
        *pp = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        const ULONG r = ++ref_;
        traceCall("SwapChain::AddRef", "#%u -> %lu", serial_, r);
        return r;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --ref_;
        traceCall("SwapChain::Release", "#%u -> %lu", serial_, r);
        if (!r) delete this;
        return r;
    }
    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT s, const void* p) override { traceCall("SwapChain::SetPrivateData", "guid=%s size=%u", iidName(g).c_str(), s); return pd_.set(g, s, p); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g, const IUnknown* u) override { traceCall("SwapChain::SetPrivateDataInterface", "guid=%s", iidName(g).c_str()); return pd_.setI(g, u); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT* s, void* p) override { traceCall("SwapChain::GetPrivateData", "guid=%s", iidName(g).c_str()); return pd_.get(g, s, p); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** pp) override {
        traceCall("SwapChain::GetParent", "#%u riid=%s", serial_, iidName(riid).c_str());
        return factory_->QueryInterface(riid, pp);
    }
    // IDXGIDeviceSubObject
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** pp) override {
        traceCall("SwapChain::GetDevice", "#%u riid=%s", serial_, iidName(riid).c_str());
        return queue_->QueryInterface(riid, pp);
    }
    // IDXGISwapChain
    HRESULT STDMETHODCALLTYPE Present(UINT sync, UINT flags) override { return present(sync, flags, "SwapChain::Present"); }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT i, REFIID riid, void** pp) override {
        traceCall("SwapChain::GetBuffer", "#%u index=%u riid=%s", serial_, i, iidName(riid).c_str());
        std::lock_guard l(mx_);
        if (!pp) return E_POINTER;
        if (i >= buffers_.size()) return DXGI_ERROR_INVALID_CALL;
        return buffers_[i]->QueryInterface(riid, pp);
    }
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL fs, IDXGIOutput*) override {
        traceCall("SwapChain::SetFullscreenState", "#%u fullscreen=%d", serial_, fs);
        return fs ? DXGI_ERROR_NOT_CURRENTLY_AVAILABLE : S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* fs, IDXGIOutput** target) override {
        traceCall("SwapChain::GetFullscreenState", "#%u", serial_);
        if (fs) *fs = FALSE;
        if (target) *target = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* d) override {
        traceCall("SwapChain::GetDesc", "#%u", serial_);
        if (!d) return E_INVALIDARG;
        std::lock_guard l(mx_);
        *d = {};
        d->BufferDesc.Width = desc_.Width;
        d->BufferDesc.Height = desc_.Height;
        d->BufferDesc.RefreshRate = fsDesc_.RefreshRate;
        d->BufferDesc.Format = desc_.Format;
        d->SampleDesc = desc_.SampleDesc;
        d->BufferUsage = desc_.BufferUsage;
        d->BufferCount = desc_.BufferCount;
        d->OutputWindow = hwnd_;
        d->Windowed = TRUE;
        d->SwapEffect = desc_.SwapEffect;
        d->Flags = desc_.Flags;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count, UINT w, UINT h, DXGI_FORMAT f, UINT flags) override {
        traceCall("SwapChain::ResizeBuffers", "#%u count=%u %ux%u fmt=%d flags=0x%x", serial_, count, w, h, int(f), flags);
        return resize(count, w, h, f, flags);
    }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC*) override { traceCall("SwapChain::ResizeTarget", "#%u", serial_); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** out) override {
        traceCall("SwapChain::GetContainingOutput", "#%u -> %s", serial_, g_opt.outputShim ? "virtual output" : "DXGI_ERROR_UNSUPPORTED");
        if (!out) return E_INVALIDARG;
        if (!g_opt.outputShim) { *out = nullptr; return DXGI_ERROR_UNSUPPORTED; }
        *out = new ShimOutput(g_adapter.Get());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* s) override {
        traceCall("SwapChain::GetFrameStatistics", "#%u presents=%u", serial_, presentCount_);
        if (!s) return E_INVALIDARG;
        std::lock_guard l(mx_);
        *s = {};
        LARGE_INTEGER q;
        const uint64_t idx = vblankIndexNow(&q);
        s->PresentCount = presentCount_;
        s->PresentRefreshCount = UINT(lastPresentVblank_);
        s->SyncRefreshCount = UINT(idx);
        s->SyncQPCTime = q;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* c) override {
        traceCall("SwapChain::GetLastPresentCount", "#%u", serial_);
        if (!c) return E_INVALIDARG;
        *c = presentCount_;
        return S_OK;
    }
    // IDXGISwapChain1
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* d) override {
        traceCall("SwapChain::GetDesc1", "#%u", serial_);
        if (!d) return E_INVALIDARG;
        std::lock_guard l(mx_);
        *d = desc_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* d) override {
        traceCall("SwapChain::GetFullscreenDesc", "#%u", serial_);
        if (!d) return E_INVALIDARG;
        *d = fsDesc_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* h) override {
        traceCall("SwapChain::GetHwnd", "#%u -> %p", serial_, (void*)hwnd_);
        if (!h) return E_INVALIDARG;
        *h = hwnd_;
        return hwnd_ ? S_OK : DXGI_ERROR_INVALID_CALL;
    }
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID, void** pp) override { traceCall("SwapChain::GetCoreWindow"); if (pp) *pp = nullptr; return DXGI_ERROR_INVALID_CALL; }
    HRESULT STDMETHODCALLTYPE Present1(UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS*) override { return present(sync, flags, "SwapChain::Present1"); }
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override { traceCall("SwapChain::IsTemporaryMonoSupported"); return FALSE; }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** o) override { traceCall("SwapChain::GetRestrictToOutput"); if (o) *o = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* c) override { traceCall("SwapChain::SetBackgroundColor"); if (c) bg_ = *c; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* c) override { traceCall("SwapChain::GetBackgroundColor"); if (c) *c = bg_; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION r) override { traceCall("SwapChain::SetRotation", "%d", int(r)); return r == DXGI_MODE_ROTATION_IDENTITY ? S_OK : DXGI_ERROR_INVALID_CALL; }
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* r) override { traceCall("SwapChain::GetRotation"); if (r) *r = DXGI_MODE_ROTATION_IDENTITY; return S_OK; }
    // IDXGISwapChain2
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT w, UINT h) override {
        traceCall("SwapChain::SetSourceSize", "%ux%u", w, h);
        if (w > desc_.Width || h > desc_.Height || !w || !h) return DXGI_ERROR_INVALID_CALL;
        srcW_ = w; srcH_ = h;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* w, UINT* h) override {
        traceCall("SwapChain::GetSourceSize");
        if (w) *w = srcW_ ? srcW_ : desc_.Width;
        if (h) *h = srcH_ ? srcH_ : desc_.Height;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT n) override {
        traceCall("SwapChain::SetMaximumFrameLatency", "#%u %u", serial_, n);
        if (!n || n > 16) return DXGI_ERROR_INVALID_CALL;
        if (latencySem_ && n > maxLatency_) ReleaseSemaphore(latencySem_, LONG(n - maxLatency_), nullptr);
        maxLatency_ = n;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* n) override { traceCall("SwapChain::GetMaximumFrameLatency"); if (n) *n = maxLatency_; return S_OK; }
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override {
        traceCall("SwapChain::GetFrameLatencyWaitableObject", "#%u has=%d", serial_, latencySem_ != nullptr);
        if (!latencySem_) return nullptr;
        HANDLE dup = nullptr;
        DuplicateHandle(GetCurrentProcess(), latencySem_, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS);
        registerLatencyHandle(dup);
        return dup;
    }
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F*) override { traceCall("SwapChain::SetMatrixTransform"); return DXGI_ERROR_INVALID_CALL; }
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F*) override { traceCall("SwapChain::GetMatrixTransform"); return DXGI_ERROR_INVALID_CALL; }
    // IDXGISwapChain3
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override {
        traceCall("SwapChain::GetCurrentBackBufferIndex", "#%u -> %u", serial_, current_);
        return current_;
    }
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE cs, UINT* s) override {
        traceCall("SwapChain::CheckColorSpaceSupport", "cs=%d", int(cs));
        if (!s) return E_INVALIDARG;
        *s = (cs == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 || cs == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
              cs == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709) ? DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT : 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE cs) override { traceCall("SwapChain::SetColorSpace1", "cs=%d", int(cs)); colorSpace_ = cs; return S_OK; }
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT count, UINT w, UINT h, DXGI_FORMAT f, UINT flags, const UINT*, IUnknown* const*) override {
        traceCall("SwapChain::ResizeBuffers1", "#%u count=%u %ux%u fmt=%d flags=0x%x", serial_, count, w, h, int(f), flags);
        return resize(count, w, h, f, flags);
    }
    // IDXGISwapChain4
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE t, UINT, void*) override { traceCall("SwapChain::SetHDRMetaData", "type=%d", int(t)); return S_OK; }

    static void registerLatencyHandle(HANDLE h);

private:
    HRESULT createBuffers() {
        buffers_.clear();
        for (UINT i = 0; i < desc_.BufferCount; ++i) {
            D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = desc_.Width;
            rd.Height = desc_.Height;
            rd.DepthOrArraySize = 1;
            rd.MipLevels = 1;
            rd.Format = desc_.Format;
            rd.SampleDesc = {1, 0};
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            if (desc_.BufferUsage & DXGI_USAGE_UNORDERED_ACCESS) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            ComPtr<ID3D12Resource> r;
            HRESULT hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r));
            if (FAILED(hr)) {
                logf("SHIM swapchain #%u buffer %u creation failed hr=0x%08lx", serial_, i, hr);
                return hr;
            }
            wchar_t name[64];
            swprintf_s(name, L"pushmpc-xefg-shim%u-bb%u", serial_, i);
            r->SetName(name);
            buffers_.push_back(r);
        }
        current_ = 0;
        return S_OK;
    }
    HRESULT resize(UINT count, UINT w, UINT h, DXGI_FORMAT f, UINT flags) {
        std::lock_guard l(mx_);
        for (auto& b : buffers_) {
            b->AddRef();
            if (b->Release() > 1) {
                logf("SHIM ResizeBuffers refused: outstanding buffer references");
                return DXGI_ERROR_INVALID_CALL;
            }
        }
        if (count) desc_.BufferCount = count;
        if (w) desc_.Width = w;
        if (h) desc_.Height = h;
        if (f != DXGI_FORMAT_UNKNOWN) desc_.Format = f;
        desc_.Flags = flags;
        return createBuffers();
    }
    HRESULT present(UINT sync, UINT flags, const char* name) {
        const double tEnter = nowMs();
        traceCall(name, "#%u sync=%u flags=0x%x buffer=%u", serial_, sync, flags, current_);
        if (flags & DXGI_PRESENT_TEST) return S_OK;
        auto cap = std::make_unique<Capture>();
        {
            std::lock_guard l(mx_);
            cap->serial = ++g_presentSerial;
            cap->swapSerial = serial_;
            cap->phase = g_phase.load();
            cap->appFrame = g_appFrame.load();
            cap->tEnter = tEnter;
            cap->tid = GetCurrentThreadId();
            cap->sync = sync;
            cap->flags = flags;
            cap->buffer = current_;
            size_t captured;
            {
                std::lock_guard cl(g_capMx);
                captured = g_captures.size();
            }
            if (g_captureImages.load() && captured < g_opt.maxCaptures && desc_.Format == DXGI_FORMAT_R8G8B8A8_UNORM)
                recordCapture(*cap);
            else
                queue_->Signal(fence_.Get(), ++fenceValue_);
            {
                std::lock_guard fl(g_lastPresentMx);
                g_lastPresentFence = fence_;
                g_lastPresentValue = fenceValue_;
            }
            ++presentCount_;
            lastPresentVblank_ = vblankIndexNow();
            current_ = (current_ + 1) % desc_.BufferCount;
            if (latencySem_) ReleaseSemaphore(latencySem_, 1, nullptr);
        }
        if (sync > 0 && g_opt.emulateVsync) waitNextVblank(sync);
        cap->tExit = nowMs();
        std::lock_guard cl(g_capMx);
        g_captures.push_back(std::move(cap));
        return S_OK;
    }
    void recordCapture(Capture& c) {
        ID3D12Resource* src = buffers_[current_].Get();
        const D3D12_RESOURCE_DESC rd = src->GetDesc();
        UINT64 total = 0;
        device_->GetCopyableFootprints(&rd, 0, 1, 0, &c.fp, nullptr, nullptr, &total);
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc = {1, 0};
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&c.readback))) ||
            FAILED(device_->CreateCommandAllocator(queueType_, IID_PPV_ARGS(&c.alloc))) ||
            FAILED(device_->CreateCommandList(0, queueType_, c.alloc.Get(), nullptr, IID_PPV_ARGS(&c.list)))) {
            logf("SHIM capture resources failed");
            c.readback.Reset();
            return;
        }
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = src;
        b.Transition.Subresource = 0;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        c.list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst{c.readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        dst.PlacedFootprint = c.fp;
        D3D12_TEXTURE_COPY_LOCATION s{src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        s.SubresourceIndex = 0;
        c.list->CopyTextureRegion(&dst, 0, 0, 0, &s, nullptr);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        c.list->ResourceBarrier(1, &b);
        c.list->Close();
        ID3D12CommandList* lists[] = {c.list.Get()};
        queue_->ExecuteCommandLists(1, lists);
        c.fence = fence_;
        c.fenceValue = ++fenceValue_;
        queue_->Signal(fence_.Get(), c.fenceValue);
    }

    std::atomic<ULONG> ref_{1};
    ComPtr<IDXGIFactory> factory_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12Device> device_;
    D3D12_COMMAND_LIST_TYPE queueType_ = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HWND hwnd_;
    DXGI_SWAP_CHAIN_DESC1 desc_;
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsDesc_{};
    std::vector<ComPtr<ID3D12Resource>> buffers_;
    ComPtr<ID3D12Fence> fence_;
    uint64_t fenceValue_ = 0;
    UINT current_ = 0;
    UINT presentCount_ = 0;
    uint64_t lastPresentVblank_ = 0;
    HANDLE latencySem_ = nullptr;
    UINT maxLatency_ = 3;
    UINT srcW_ = 0, srcH_ = 0;
    DXGI_RGBA bg_{};
    DXGI_COLOR_SPACE_TYPE colorSpace_ = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    std::mutex mx_;
    PrivateData pd_;
    const uint32_t serial_;
};

static std::mutex g_latencyMx;
static std::vector<HANDLE> g_latencyHandles;
void ShimSwapChain::registerLatencyHandle(HANDLE h) {
    std::lock_guard l(g_latencyMx);
    g_latencyHandles.push_back(h);
}
static bool isLatencyHandle(HANDLE h) {
    std::lock_guard l(g_latencyMx);
    return std::find(g_latencyHandles.begin(), g_latencyHandles.end(), h) != g_latencyHandles.end();
}

static const char* swapEffectName(DXGI_SWAP_EFFECT e) {
    switch (e) {
    case DXGI_SWAP_EFFECT_DISCARD: return "DISCARD";
    case DXGI_SWAP_EFFECT_SEQUENTIAL: return "SEQUENTIAL";
    case DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL: return "FLIP_SEQUENTIAL";
    case DXGI_SWAP_EFFECT_FLIP_DISCARD: return "FLIP_DISCARD";
    }
    return "?";
}

// ---------------------------------------------------------------------------------------
// ShimFactory: forwards to a real factory except swap chain creation and window
// association.
// ---------------------------------------------------------------------------------------
class ShimFactory final : public IDXGIFactory7 {
public:
    explicit ShimFactory(IDXGIFactory7* real) : real_(real) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
        if (!pp) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIFactory) ||
            riid == __uuidof(IDXGIFactory1) || riid == __uuidof(IDXGIFactory2) || riid == __uuidof(IDXGIFactory3) ||
            riid == __uuidof(IDXGIFactory4) || riid == __uuidof(IDXGIFactory5) || riid == __uuidof(IDXGIFactory6) ||
            riid == __uuidof(IDXGIFactory7)) {
            traceCall("Factory::QueryInterface", "riid=%s -> S_OK", iidName(riid).c_str());
            *pp = static_cast<IDXGIFactory7*>(this);
            AddRef();
            return S_OK;
        }
        traceCall("Factory::QueryInterface(unknown)", "riid=%s -> E_NOINTERFACE", iidName(riid).c_str());
        *pp = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --ref_;
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT s, const void* p) override { traceCall("Factory::SetPrivateData"); return real_->SetPrivateData(g, s, p); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g, const IUnknown* u) override { traceCall("Factory::SetPrivateDataInterface"); return real_->SetPrivateDataInterface(g, u); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT* s, void* p) override { traceCall("Factory::GetPrivateData"); return real_->GetPrivateData(g, s, p); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** pp) override { traceCall("Factory::GetParent", "riid=%s", iidName(riid).c_str()); return real_->GetParent(riid, pp); }
    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT i, IDXGIAdapter** a) override { traceCall("Factory::EnumAdapters", "%u", i); return real_->EnumAdapters(i, a); }
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND h, UINT f) override { traceCall("Factory::MakeWindowAssociation", "hwnd=%p flags=0x%x -> S_OK (not forwarded)", (void*)h, f); assoc_ = h; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND* h) override { traceCall("Factory::GetWindowAssociation"); if (!h) return E_INVALIDARG; *h = assoc_; return S_OK; }
    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** out) override {
        traceCall("Factory::CreateSwapChain");
        if (!d || !out) return E_INVALIDARG;
        DXGI_SWAP_CHAIN_DESC1 d1{};
        d1.Width = d->BufferDesc.Width; d1.Height = d->BufferDesc.Height; d1.Format = d->BufferDesc.Format;
        d1.SampleDesc = d->SampleDesc; d1.BufferUsage = d->BufferUsage; d1.BufferCount = d->BufferCount;
        d1.SwapEffect = d->SwapEffect; d1.Flags = d->Flags; d1.Scaling = DXGI_SCALING_STRETCH; d1.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        IDXGISwapChain1* sc = nullptr;
        HRESULT hr = create(dev, d->OutputWindow, &d1, nullptr, &sc, "CreateSwapChain");
        *out = sc;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE m, IDXGIAdapter** a) override { traceCall("Factory::CreateSoftwareAdapter"); return real_->CreateSoftwareAdapter(m, a); }
    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT i, IDXGIAdapter1** a) override { traceCall("Factory::EnumAdapters1", "%u", i); return real_->EnumAdapters1(i, a); }
    BOOL STDMETHODCALLTYPE IsCurrent() override { traceCall("Factory::IsCurrent"); return real_->IsCurrent(); }
    BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override { traceCall("Factory::IsWindowedStereoEnabled"); return FALSE; }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown* dev, HWND h, const DXGI_SWAP_CHAIN_DESC1* d, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,
                                                     IDXGIOutput* restrict, IDXGISwapChain1** out) override {
        traceCall("Factory::CreateSwapChainForHwnd", "hwnd=%p restrict=%p", (void*)h, (void*)restrict);
        return create(dev, h, d, fs, out, "CreateSwapChainForHwnd");
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1** out) override {
        traceCall("Factory::CreateSwapChainForCoreWindow");
        if (out) *out = nullptr;
        return DXGI_ERROR_UNSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE h, LUID* l) override { traceCall("Factory::GetSharedResourceAdapterLuid"); return real_->GetSharedResourceAdapterLuid(h, l); }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND, UINT, DWORD* c) override { traceCall("Factory::RegisterStereoStatusWindow"); if (c) *c = 1; return S_OK; }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE, DWORD* c) override { traceCall("Factory::RegisterStereoStatusEvent"); if (c) *c = 1; return S_OK; }
    void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD) override { traceCall("Factory::UnregisterStereoStatus"); }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND, UINT, DWORD* c) override { traceCall("Factory::RegisterOcclusionStatusWindow"); if (c) *c = 1; return S_OK; }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE, DWORD* c) override { traceCall("Factory::RegisterOcclusionStatusEvent"); if (c) *c = 1; return S_OK; }
    void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD) override { traceCall("Factory::UnregisterOcclusionStatus"); }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown* dev, const DXGI_SWAP_CHAIN_DESC1* d, IDXGIOutput*, IDXGISwapChain1** out) override {
        traceCall("Factory::CreateSwapChainForComposition");
        return create(dev, nullptr, d, nullptr, out, "CreateSwapChainForComposition");
    }
    UINT STDMETHODCALLTYPE GetCreationFlags() override { traceCall("Factory::GetCreationFlags"); return real_->GetCreationFlags(); }
    HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID l, REFIID riid, void** pp) override { traceCall("Factory::EnumAdapterByLuid", "riid=%s", iidName(riid).c_str()); return real_->EnumAdapterByLuid(l, riid, pp); }
    HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID riid, void** pp) override { traceCall("Factory::EnumWarpAdapter"); return real_->EnumWarpAdapter(riid, pp); }
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(DXGI_FEATURE f, void* p, UINT s) override {
        HRESULT hr = real_->CheckFeatureSupport(f, p, s);
        if (f == DXGI_FEATURE_PRESENT_ALLOW_TEARING && p && s >= sizeof(BOOL)) *(BOOL*)p = TRUE, hr = S_OK;
        traceCall("Factory::CheckFeatureSupport", "feature=%d -> hr=0x%08lx value=%d", int(f), hr, (p && s >= sizeof(BOOL)) ? *(BOOL*)p : -1);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE EnumAdapterByGpuPreference(UINT i, DXGI_GPU_PREFERENCE g, REFIID riid, void** pp) override { traceCall("Factory::EnumAdapterByGpuPreference", "%u", i); return real_->EnumAdapterByGpuPreference(i, g, riid, pp); }
    HRESULT STDMETHODCALLTYPE RegisterAdaptersChangedEvent(HANDLE h, DWORD* c) override { traceCall("Factory::RegisterAdaptersChangedEvent"); return real_->RegisterAdaptersChangedEvent(h, c); }
    HRESULT STDMETHODCALLTYPE UnregisterAdaptersChangedEvent(DWORD c) override { traceCall("Factory::UnregisterAdaptersChangedEvent"); return real_->UnregisterAdaptersChangedEvent(c); }

private:
    HRESULT create(IUnknown* dev, HWND h, const DXGI_SWAP_CHAIN_DESC1* d, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGISwapChain1** out, const char* via) {
        if (!d || !out || !dev) return E_INVALIDARG;
        *out = nullptr;
        ComPtr<ID3D12CommandQueue> q;
        if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&q)))) {
            logf("SHIM %s: device is not an ID3D12CommandQueue", via);
            return DXGI_ERROR_INVALID_CALL;
        }
        const auto qd = q->GetDesc();
        logf("SHIM %s: %ux%u fmt=%d buffers=%u usage=0x%x swap=%s flags=0x%x scaling=%d alpha=%d stereo=%d queue=%p type=%d prio=%d flags=0x%x",
             via, d->Width, d->Height, int(d->Format), d->BufferCount, d->BufferUsage, swapEffectName(d->SwapEffect), d->Flags,
             int(d->Scaling), int(d->AlphaMode), d->Stereo, (void*)q.Get(), int(qd.Type), qd.Priority, qd.Flags);
        if (fs) logf("SHIM %s: fullscreen desc windowed=%d refresh=%u/%u", via, fs->Windowed, fs->RefreshRate.Numerator, fs->RefreshRate.Denominator);
        auto* sc = new ShimSwapChain(this, q.Get(), h, *d, fs);
        HRESULT hr = sc->init();
        if (FAILED(hr)) {
            logf("SHIM %s: init failed hr=0x%08lx", via, hr);
            sc->Release();
            return hr;
        }
        logf("SHIM %s -> shim swapchain #%u", via, sc->serial());
        *out = sc;
        return S_OK;
    }
    std::atomic<ULONG> ref_{1};
    ComPtr<IDXGIFactory7> real_;
    HWND assoc_ = nullptr;
};

static ComPtr<ShimFactory> g_shimFactory;

// ---------------------------------------------------------------------------------------
// Import hooks in libxess_fg.dll / libxell.dll
// ---------------------------------------------------------------------------------------
static int hookIat(HMODULE mod, const char* func, void* repl, void** orig) {
    auto* base = reinterpret_cast<uint8_t*>(mod);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    int patched = 0;
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (!imp->OriginalFirstThunk) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(ibn->Name), func) != 0) continue;
            DWORD old;
            VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            if (orig && !*orig) *orig = reinterpret_cast<void*>(iat->u1.Function);
            iat->u1.Function = reinterpret_cast<ULONG_PTR>(repl);
            VirtualProtect(&iat->u1.Function, sizeof(void*), old, &old);
            ++patched;
        }
    }
    return patched;
}

struct WaitStat {
    uint64_t count = 0;
    double totalMs = 0, maxMs = 0;
    uint64_t over1ms = 0;
};
static std::mutex g_waitMx;
static std::map<std::string, WaitStat> g_waits;
static void noteWait(const std::string& key, double ms) {
    std::lock_guard l(g_waitMx);
    auto& w = g_waits[key];
    ++w.count;
    w.totalMs += ms;
    w.maxMs = std::max(w.maxMs, ms);
    w.over1ms += ms > 1.0;
}

static decltype(&CreateDXGIFactory1) o_CreateDXGIFactory1_fg = nullptr;
static decltype(&CreateDXGIFactory1) o_CreateDXGIFactory1_ll = nullptr;
static HRESULT WINAPI h_CreateDXGIFactory1_fg(REFIID riid, void** pp) {
    traceCall("hook:xefg!CreateDXGIFactory1", "riid=%s -> shim factory", iidName(riid).c_str());
    return g_shimFactory->QueryInterface(riid, pp);
}
static HRESULT WINAPI h_CreateDXGIFactory1_ll(REFIID riid, void** pp) {
    traceCall("hook:xell!CreateDXGIFactory1", "riid=%s -> real factory", iidName(riid).c_str());
    return o_CreateDXGIFactory1_ll(riid, pp);
}

static decltype(&GetClientRect) o_GetClientRect = nullptr;
static BOOL WINAPI h_GetClientRect(HWND h, LPRECT r) {
    if (h == g_hwnd && r) {
        traceCall("hook:GetClientRect", "hwnd=%p -> %ux%u", (void*)h, g_opt.width, g_opt.height);
        *r = {0, 0, LONG(g_opt.width), LONG(g_opt.height)};
        return TRUE;
    }
    traceCall("hook:GetClientRect(other)", "hwnd=%p", (void*)h);
    return o_GetClientRect(h, r);
}
static decltype(&MonitorFromWindow) o_MonitorFromWindow = nullptr;
static HMONITOR WINAPI h_MonitorFromWindow(HWND h, DWORD f) {
    if (h == g_hwnd) {
        traceCall("hook:MonitorFromWindow", "hwnd=%p flags=%lu -> virtual monitor", (void*)h, f);
        return kFakeMonitor;
    }
    traceCall("hook:MonitorFromWindow(other)", "hwnd=%p", (void*)h);
    return o_MonitorFromWindow(h, f);
}
static decltype(&GetMonitorInfoA) o_GetMonitorInfoA = nullptr;
static BOOL WINAPI h_GetMonitorInfoA(HMONITOR m, LPMONITORINFO mi) {
    if (m == kFakeMonitor && mi) {
        traceCall("hook:GetMonitorInfoA", "virtual monitor cb=%lu", mi->cbSize);
        const RECT rc{0, 0, LONG(std::max(1920u, g_opt.width)), LONG(std::max(1080u, g_opt.height))};
        mi->rcMonitor = rc;
        mi->rcWork = rc;
        mi->dwFlags = MONITORINFOF_PRIMARY;
        if (mi->cbSize >= sizeof(MONITORINFOEXA)) strcpy_s(reinterpret_cast<MONITORINFOEXA*>(mi)->szDevice, kVirtualDeviceA);
        return TRUE;
    }
    traceCall("hook:GetMonitorInfoA(other)");
    return o_GetMonitorInfoA(m, mi);
}
static decltype(&EnumDisplaySettingsA) o_EnumDisplaySettingsA = nullptr;
static BOOL WINAPI h_EnumDisplaySettingsA(LPCSTR dev, DWORD mode, DEVMODEA* dm) {
    if (dm && (!dev || strcmp(dev, kVirtualDeviceA) == 0)) {
        traceCall("hook:EnumDisplaySettingsA", "dev=%s mode=%ld -> %.0f Hz virtual", dev ? dev : "(null)", long(mode), g_opt.refreshHz);
        const WORD size = dm->dmSize;
        const WORD extra = dm->dmDriverExtra;
        memset(dm, 0, size);
        dm->dmSize = size;
        dm->dmDriverExtra = extra;
        dm->dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
        dm->dmBitsPerPel = 32;
        dm->dmPelsWidth = std::max(1920u, g_opt.width);
        dm->dmPelsHeight = std::max(1080u, g_opt.height);
        dm->dmDisplayFrequency = DWORD(std::lround(g_opt.refreshHz));
        return TRUE;
    }
    traceCall("hook:EnumDisplaySettingsA(other)", "dev=%s", dev ? dev : "(null)");
    return o_EnumDisplaySettingsA(dev, mode, dm);
}
static decltype(&GetProcAddress) o_GetProcAddress = nullptr;
static FARPROC WINAPI h_GetProcAddress(HMODULE m, LPCSTR name) {
    char modName[MAX_PATH] = "?";
    GetModuleFileNameA(m, modName, MAX_PATH);
    const char* shortName = strrchr(modName, '\\');
    shortName = shortName ? shortName + 1 : modName;
    char key[512];
    if (reinterpret_cast<uintptr_t>(name) < 0x10000) snprintf(key, sizeof(key), "hook:GetProcAddress(%s!#%u)", shortName, unsigned(reinterpret_cast<uintptr_t>(name)));
    else snprintf(key, sizeof(key), "hook:GetProcAddress(%s!%s)", shortName, name);
    traceCall(key);
    if (reinterpret_cast<uintptr_t>(name) >= 0x10000 && (strcmp(name, "CreateDXGIFactory1") == 0 || strcmp(name, "CreateDXGIFactory2") == 0 || strcmp(name, "CreateDXGIFactory") == 0)) {
        logf("HOOK GetProcAddress(%s) redirected to the shim factory", name);
        if (strcmp(name, "CreateDXGIFactory2") == 0) {
            static auto f2 = [](UINT, REFIID riid, void** pp) -> HRESULT { return h_CreateDXGIFactory1_fg(riid, pp); };
            return reinterpret_cast<FARPROC>(static_cast<HRESULT (*)(UINT, REFIID, void**)>(f2));
        }
        return reinterpret_cast<FARPROC>(&h_CreateDXGIFactory1_fg);
    }
    return o_GetProcAddress(m, name);
}
static decltype(&LoadLibraryExA) o_LoadLibraryExA = nullptr;
static HMODULE WINAPI h_LoadLibraryExA(LPCSTR n, HANDLE f, DWORD fl) {
    char key[512];
    snprintf(key, sizeof(key), "hook:LoadLibraryExA(%s)", n ? n : "(null)");
    traceCall(key, "flags=0x%lx", fl);
    return o_LoadLibraryExA(n, f, fl);
}
static decltype(&LoadLibraryExW) o_LoadLibraryExW = nullptr;
static HMODULE WINAPI h_LoadLibraryExW(LPCWSTR n, HANDLE f, DWORD fl) {
    char key[512];
    snprintf(key, sizeof(key), "hook:LoadLibraryExW(%s)", n ? narrow(n).c_str() : "(null)");
    traceCall(key, "flags=0x%lx", fl);
    return o_LoadLibraryExW(n, f, fl);
}
static decltype(&LoadLibraryW) o_LoadLibraryW = nullptr;
static HMODULE WINAPI h_LoadLibraryW(LPCWSTR n) {
    char key[512];
    snprintf(key, sizeof(key), "hook:LoadLibraryW(%s)", n ? narrow(n).c_str() : "(null)");
    traceCall(key);
    return o_LoadLibraryW(n);
}
static decltype(&LoadLibraryA) o_LoadLibraryA = nullptr;
static HMODULE WINAPI h_LoadLibraryA(LPCSTR n) {
    char key[512];
    snprintf(key, sizeof(key), "hook:LoadLibraryA(%s)", n ? n : "(null)");
    traceCall(key);
    return o_LoadLibraryA(n);
}
static decltype(&WaitForSingleObject) o_WaitForSingleObject = nullptr;
static DWORD WINAPI h_WaitForSingleObject(HANDLE h, DWORD ms) {
    const double t0 = nowMs();
    const DWORD r = o_WaitForSingleObject(h, ms);
    const double dt = nowMs() - t0;
    noteWait(isLatencyHandle(h) ? "WaitForSingleObject(latency waitable)" : "WaitForSingleObject(other)", dt);
    return r;
}
static decltype(&WaitForSingleObjectEx) o_WaitForSingleObjectEx = nullptr;
static DWORD WINAPI h_WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL a) {
    const double t0 = nowMs();
    const DWORD r = o_WaitForSingleObjectEx(h, ms, a);
    const double dt = nowMs() - t0;
    noteWait(isLatencyHandle(h) ? "WaitForSingleObjectEx(latency waitable)" : "WaitForSingleObjectEx(other)", dt);
    return r;
}
using ThrdSleepFn = void(__cdecl*)(const void*);
static ThrdSleepFn o_Thrd_sleep = nullptr;
static void __cdecl h_Thrd_sleep(const void* t) {
    const double t0 = nowMs();
    o_Thrd_sleep(t);
    noteWait("_Thrd_sleep (std::this_thread::sleep_*)", nowMs() - t0);
}
using CndTimedwaitFn = int(__cdecl*)(void*, void*, const void*);
static CndTimedwaitFn o_Cnd_timedwait = nullptr;
static int __cdecl h_Cnd_timedwait(void* c, void* m, const void* t) {
    const double t0 = nowMs();
    const int r = o_Cnd_timedwait(c, m, t);
    noteWait("_Cnd_timedwait (condition_variable::wait_*)", nowMs() - t0);
    return r;
}
using CndWaitFn = int(__cdecl*)(void*, void*);
static CndWaitFn o_Cnd_wait = nullptr;
static int __cdecl h_Cnd_wait(void* c, void* m) {
    const double t0 = nowMs();
    const int r = o_Cnd_wait(c, m);
    noteWait("_Cnd_wait (condition_variable::wait)", nowMs() - t0);
    return r;
}
static decltype(&SetThreadPriority) o_SetThreadPriority = nullptr;
static BOOL WINAPI h_SetThreadPriority(HANDLE h, int p) {
    traceCall("hook:SetThreadPriority", "thread=%lu priority=%d", GetThreadId(h), p);
    return o_SetThreadPriority(h, p);
}
static decltype(&SetThreadDescription) o_SetThreadDescription = nullptr;
static HRESULT WINAPI h_SetThreadDescription(HANDLE h, PCWSTR d) {
    char key[256];
    snprintf(key, sizeof(key), "hook:SetThreadDescription(%s)", d ? narrow(d).c_str() : "");
    traceCall(key, "thread=%lu", GetThreadId(h));
    return o_SetThreadDescription(h, d);
}

static void installHooks(HMODULE fg, HMODULE ll) {
    struct H { const char* name; void* repl; void** orig; };
    const H fgHooks[] = {
        {"CreateDXGIFactory1", (void*)&h_CreateDXGIFactory1_fg, (void**)&o_CreateDXGIFactory1_fg},
        {"GetClientRect", (void*)&h_GetClientRect, (void**)&o_GetClientRect},
        {"MonitorFromWindow", (void*)&h_MonitorFromWindow, (void**)&o_MonitorFromWindow},
        {"GetMonitorInfoA", (void*)&h_GetMonitorInfoA, (void**)&o_GetMonitorInfoA},
        {"EnumDisplaySettingsA", (void*)&h_EnumDisplaySettingsA, (void**)&o_EnumDisplaySettingsA},
        {"GetProcAddress", (void*)&h_GetProcAddress, (void**)&o_GetProcAddress},
        {"LoadLibraryExA", (void*)&h_LoadLibraryExA, (void**)&o_LoadLibraryExA},
        {"LoadLibraryExW", (void*)&h_LoadLibraryExW, (void**)&o_LoadLibraryExW},
        {"LoadLibraryW", (void*)&h_LoadLibraryW, (void**)&o_LoadLibraryW},
        {"LoadLibraryA", (void*)&h_LoadLibraryA, (void**)&o_LoadLibraryA},
        {"WaitForSingleObject", (void*)&h_WaitForSingleObject, (void**)&o_WaitForSingleObject},
        {"WaitForSingleObjectEx", (void*)&h_WaitForSingleObjectEx, (void**)&o_WaitForSingleObjectEx},
        {"_Thrd_sleep", (void*)&h_Thrd_sleep, (void**)&o_Thrd_sleep},
        {"_Cnd_timedwait", (void*)&h_Cnd_timedwait, (void**)&o_Cnd_timedwait},
        {"_Cnd_wait", (void*)&h_Cnd_wait, (void**)&o_Cnd_wait},
        {"SetThreadPriority", (void*)&h_SetThreadPriority, (void**)&o_SetThreadPriority},
        {"SetThreadDescription", (void*)&h_SetThreadDescription, (void**)&o_SetThreadDescription},
    };
    for (const auto& h : fgHooks) {
        const int n = hookIat(fg, h.name, h.repl, h.orig);
        logf("HOOK libxess_fg!%s patched=%d", h.name, n);
    }
    const int n = hookIat(ll, "CreateDXGIFactory1", (void*)&h_CreateDXGIFactory1_ll, (void**)&o_CreateDXGIFactory1_ll);
    logf("HOOK libxell!CreateDXGIFactory1 patched=%d (logged, forwarded to the real factory)", n);
}

// ---------------------------------------------------------------------------------------
// File identity
// ---------------------------------------------------------------------------------------
static std::string sha256File(const std::wstring& path, uint64_t& size) {
    size = 0;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    std::vector<uint8_t> buf(1 << 20);
    DWORD got = 0;
    while (ReadFile(f, buf.data(), DWORD(buf.size()), &got, nullptr) && got) {
        BCryptHashData(hash, buf.data(), got, 0);
        size += got;
    }
    uint8_t digest[32];
    BCryptFinishHash(hash, digest, 32, 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    std::string s;
    char hx[3];
    for (uint8_t b : digest) { snprintf(hx, 3, "%02X", b); s += hx; }
    return s;
}

static std::string fileVersion(const std::wstring& path) {
    DWORD h = 0;
    const DWORD n = GetFileVersionInfoSizeW(path.c_str(), &h);
    if (!n) return "?";
    std::vector<uint8_t> b(n);
    if (!GetFileVersionInfoW(path.c_str(), 0, n, b.data())) return "?";
    VS_FIXEDFILEINFO* fi = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(b.data(), L"\\", (void**)&fi, &len) || !fi) return "?";
    char s[64];
    snprintf(s, sizeof(s), "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
    return s;
}

// ---------------------------------------------------------------------------------------
// MFG unlock (same byte tables and identity gate as PushFPS XessMfgUnlock, which ports
// OptiScaler XeFGUnlock.h commit 70676c5f). Only the mapped image is patched.
// ---------------------------------------------------------------------------------------
namespace unlock {
constexpr uint64_t kSize = 22957432ull;
constexpr const char* kSha = "EC5E0C65E075570C6EDE72618BB666D0BE0C2E10B2EA9762C0FE8CB8E375AB27";
constexpr uint32_t kTimeDateStamp = 0x69CB0F4Du;
constexpr uint32_t kSizeOfImage = 0x015ED000u;
struct Patch { const char* name; uint32_t rva; std::vector<uint8_t> expected, replacement; int immOffset; };
static const Patch kPatches[] = {
    {"U1/frame-count-fallback", 0x20DA4F, {0x0F, 0x85, 0xCC, 0x00, 0x00, 0x00}, {0xE9, 0xCD, 0x00, 0x00, 0x00, 0x90}, -1},
    {"U2/model-downgrade", 0x1A5DE4, {0x74, 0x09}, {0xEB, 0x06}, -1},
    {"U3/default-ceiling", 0x1A517D, {0xBB, 0x03, 0x00, 0x00, 0x00}, {0xBB, 0x00, 0x00, 0x00, 0x00}, 1},
    {"U4/override-clamp", 0x1A45C2, {0xC7, 0x87, 0x6C, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00}, {0xC7, 0x87, 0x6C, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 6},
    {"U5/reported-maximum", 0x20973B, {0xB8, 0x01, 0x00, 0x00, 0x00}, {0xB8, 0x00, 0x00, 0x00, 0x00}, 1},
};
struct Applied { uint8_t* at; std::vector<uint8_t> original; };
static std::vector<Applied> g_applied;

static bool write(uint8_t* at, const std::vector<uint8_t>& bytes) {
    DWORD old;
    if (!VirtualProtect(at, bytes.size(), PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(at, bytes.data(), bytes.size());
    FlushInstructionCache(GetCurrentProcess(), at, bytes.size());
    VirtualProtect(at, bytes.size(), old, &old);
    return memcmp(at, bytes.data(), bytes.size()) == 0;
}

static bool apply(HMODULE fg, const std::wstring& path, uint32_t generated, std::string& detail) {
    uint64_t size = 0;
    const std::string sha = sha256File(path, size);
    auto* base = reinterpret_cast<uint8_t*>(fg);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    if (size != kSize || sha != kSha) {
        detail = "unaudited provider size=" + std::to_string(size) + " sha256=" + sha;
        return false;
    }
    if (nt->FileHeader.TimeDateStamp != kTimeDateStamp || nt->OptionalHeader.SizeOfImage != kSizeOfImage) {
        detail = "PE identity mismatch";
        return false;
    }
    for (const auto& p : kPatches) {
        if (memcmp(base + p.rva, p.expected.data(), p.expected.size()) != 0) {
            detail = std::string(p.name) + " has unexpected bytes";
            return false;
        }
    }
    for (const auto& p : kPatches) {
        std::vector<uint8_t> bytes = p.replacement;
        if (p.immOffset >= 0) memcpy(bytes.data() + p.immOffset, &generated, 4);
        Applied a{base + p.rva, p.expected};
        if (!write(a.at, bytes)) {
            detail = std::string(p.name) + " write failed";
            for (auto it = g_applied.rbegin(); it != g_applied.rend(); ++it) write(it->at, it->original);
            g_applied.clear();
            return false;
        }
        g_applied.push_back(a);
    }
    detail = "applied " + std::to_string(g_applied.size()) + " patches, ceiling " + std::to_string(generated + 1) + "X";
    return true;
}
static void release() {
    for (auto it = g_applied.rbegin(); it != g_applied.rend(); ++it) write(it->at, it->original);
    g_applied.clear();
}
} // namespace unlock

// ---------------------------------------------------------------------------------------
// Synthetic scene (CPU)
// ---------------------------------------------------------------------------------------
static uint16_t toHalf(float f) { return uint16_t(_mm_extract_epi16(_mm_cvtps_ph(_mm_set_ss(f), 0), 0)); }

struct Scene {
    uint32_t W, H, S;
    double x0, y0, vx, vy;
    std::vector<uint8_t> bg; // RGBA
    void init(uint32_t w, uint32_t h, uint32_t s, double vx_, double vy_) {
        W = w; H = h; S = s; vx = vx_; vy = vy_;
        x0 = std::max(8.0, double(W) * 0.08);
        y0 = std::max(8.0, double(H) * 0.20);
        bg.resize(size_t(W) * H * 4);
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                const double n = 0.5 + 0.18 * std::sin(x * 0.071 + 0.4) * std::cos(y * 0.053) + 0.14 * std::sin(x * 0.23 + y * 0.19) +
                                 0.10 * std::sin((x * 0.6 - y * 0.45)) + 0.05 * std::cos(x * 1.3 + y * 0.9);
                double v = 40.0 + 150.0 * std::clamp(n, 0.0, 1.0);
                if (x % 32 == 0 || y % 32 == 0) v = 215.0;
                uint8_t* p = &bg[(size_t(y) * W + x) * 4];
                p[0] = uint8_t(v * 0.92);
                p[1] = uint8_t(v * 0.97);
                p[2] = uint8_t(std::min(255.0, v + 25.0));
                p[3] = 255;
            }
    }
    double px(double t) const { return x0 + vx * t; }
    double py(double t) const { return y0 + vy * t; }
    static void tex(double u, double v, double rgb[3]) {
        const double s1 = 0.5 + 0.5 * std::sin(u * 0.45) * std::cos(v * 0.30);
        const double s2 = 0.5 + 0.5 * std::sin(u * 0.349) * std::sin(v * 0.349);
        rgb[0] = 170.0 + 80.0 * s1;
        rgb[1] = 30.0 + 70.0 * s2;
        rgb[2] = 30.0 + 50.0 * (1.0 - s1);
    }
    // Renders the frame at time t (in real-frame units). mv: optional R16G16_FLOAT pairs.
    void render(double t, std::vector<uint8_t>& out, std::vector<uint16_t>* mv) const {
        out = bg;
        if (mv) mv->assign(size_t(W) * H * 2, 0);
        const double sx = px(t), sy = py(t);
        const int x1 = std::max(0, int(std::floor(sx))), x2 = std::min(int(W) - 1, int(std::ceil(sx + S)));
        const int y1 = std::max(0, int(std::floor(sy))), y2 = std::min(int(H) - 1, int(std::ceil(sy + S)));
        const uint16_t hx = toHalf(float(-vx)), hy = toHalf(float(-vy));
        for (int y = y1; y <= y2; ++y) {
            const double cy = std::max(0.0, std::min(y + 1.0, sy + S) - std::max(double(y), sy));
            if (cy <= 0) continue;
            for (int x = x1; x <= x2; ++x) {
                const double cx = std::max(0.0, std::min(x + 1.0, sx + S) - std::max(double(x), sx));
                const double cov = cx * cy;
                if (cov <= 0) continue;
                double rgb[3];
                tex(std::clamp(x + 0.5 - sx, 0.0, double(S)), std::clamp(y + 0.5 - sy, 0.0, double(S)), rgb);
                uint8_t* p = &out[(size_t(y) * W + x) * 4];
                for (int c = 0; c < 3; ++c) p[c] = uint8_t(std::lround(p[c] * (1.0 - cov) + rgb[c] * cov));
                if (mv && cov > 0.5) {
                    (*mv)[(size_t(y) * W + x) * 2 + 0] = hx;
                    (*mv)[(size_t(y) * W + x) * 2 + 1] = hy;
                }
            }
        }
    }
};

struct Rect { int x1, y1, x2, y2; };
static double psnr(const uint8_t* a, const uint8_t* b, uint32_t W, uint32_t H, const Rect* roi = nullptr) {
    Rect r = roi ? *roi : Rect{0, 0, int(W), int(H)};
    r.x1 = std::max(0, r.x1); r.y1 = std::max(0, r.y1); r.x2 = std::min(int(W), r.x2); r.y2 = std::min(int(H), r.y2);
    double se = 0;
    uint64_t n = 0;
    for (int y = r.y1; y < r.y2; ++y)
        for (int x = r.x1; x < r.x2; ++x) {
            const size_t i = (size_t(y) * W + x) * 4;
            for (int c = 0; c < 3; ++c) { const double d = double(a[i + c]) - double(b[i + c]); se += d * d; }
            n += 3;
        }
    if (!n) return 0;
    const double mse = se / double(n);
    return mse <= 1e-10 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}
static bool centroid(const uint8_t* img, uint32_t W, uint32_t H, double& cx, double& cy) {
    double sw = 0, sx = 0, sy = 0;
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            const uint8_t* p = img + (size_t(y) * W + x) * 4;
            const double w = std::clamp((double(p[0]) - std::max(p[1], p[2]) - 40.0) / 60.0, 0.0, 1.0);
            sw += w; sx += w * (x + 0.5); sy += w * (y + 0.5);
        }
    if (sw < 10) return false;
    cx = sx / sw; cy = sy / sw;
    return true;
}

// ---------------------------------------------------------------------------------------
// PNG writer (stored deflate blocks)
// ---------------------------------------------------------------------------------------
static uint32_t crc32Table[256];
static void crcInit() {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc32Table[i] = c;
    }
}
static uint32_t crc32(const uint8_t* d, size_t n, uint32_t c = 0xFFFFFFFFu) {
    for (size_t i = 0; i < n; ++i) c = crc32Table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c;
}
static void writePng(const std::filesystem::path& path, const uint8_t* rgba, uint32_t W, uint32_t H) {
    std::vector<uint8_t> raw;
    raw.reserve(size_t(H) * (W * 4 + 1));
    for (uint32_t y = 0; y < H; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba + size_t(y) * W * 4, rgba + size_t(y + 1) * W * 4);
    }
    std::vector<uint8_t> z{0x78, 0x01};
    size_t pos = 0;
    while (pos < raw.size() || raw.empty()) {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        const bool last = pos + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(uint8_t(n)); z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n)); z.push_back(uint8_t((~n) >> 8));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
        if (last) break;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
    const uint32_t adler = (b << 16) | a;
    for (int i = 3; i >= 0; --i) z.push_back(uint8_t(adler >> (i * 8)));
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
        uint8_t len[4] = {uint8_t(data.size() >> 24), uint8_t(data.size() >> 16), uint8_t(data.size() >> 8), uint8_t(data.size())};
        fwrite(len, 1, 4, f);
        std::vector<uint8_t> td(type, type + 4);
        td.insert(td.end(), data.begin(), data.end());
        fwrite(td.data(), 1, td.size(), f);
        const uint32_t c = crc32(td.data(), td.size()) ^ 0xFFFFFFFFu;
        uint8_t cb[4] = {uint8_t(c >> 24), uint8_t(c >> 16), uint8_t(c >> 8), uint8_t(c)};
        fwrite(cb, 1, 4, f);
    };
    std::vector<uint8_t> ihdr = {uint8_t(W >> 24), uint8_t(W >> 16), uint8_t(W >> 8), uint8_t(W), uint8_t(H >> 24), uint8_t(H >> 16), uint8_t(H >> 8), uint8_t(H), 8, 6, 0, 0, 0};
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    fclose(f);
}

// ---------------------------------------------------------------------------------------
// XeFG / XeLL entry points
// ---------------------------------------------------------------------------------------
struct Api {
#define API(name) decltype(&name) name = nullptr
    API(xefgSwapChainGetVersion);
    API(xefgSwapChainD3D12CreateContext);
    API(xefgSwapChainSetLoggingCallback);
    API(xefgSwapChainD3D12GetProperties);
    API(xefgSwapChainGetProperties);
    API(xefgSwapChainSetNumInterpolatedFrames);
    API(xefgSwapChainD3D12InitFromSwapChain);
    API(xefgSwapChainD3D12GetSwapChainPtr);
    API(xefgSwapChainD3D12TagFrameResource);
    API(xefgSwapChainTagFrameConstants);
    API(xefgSwapChainSetEnabled);
    API(xefgSwapChainSetPresentId);
    API(xefgSwapChainGetLastPresentStatus);
    API(xefgSwapChainSetLatencyReduction);
    API(xefgSwapChainDestroy);
    API(xefgSwapChainD3D12GetInitializationParameters);
    API(xellD3D12CreateContext);
    API(xellDestroyContext);
    API(xellSetSleepMode);
    API(xellSleep);
    API(xellAddMarkerData);
    API(xellGetVersion);
    API(xellSetLoggingCallback);
#undef API
};
static Api api;

static void xefgLog(const char* msg, xefg_swapchain_logging_level_t lvl, void*) {
    static const char* names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    logf("XEFG[%s] %s", (lvl >= 0 && lvl <= 3) ? names[lvl] : "?", msg ? msg : "");
}
static void xellLog(const char* msg, xell_logging_level_t lvl) {
    static const char* names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    logf("XELL[%s] %s", (lvl >= 0 && lvl <= 3) ? names[lvl] : "?", msg ? msg : "");
}

static std::map<std::string, int> g_initResults;
static int initResult(const char* what, int r) {
    g_initResults[what] = r;
    reportf("init %s result=%d", what, r);
    return r;
}

// ---------------------------------------------------------------------------------------
// Timing statistics helper
// ---------------------------------------------------------------------------------------
struct Stats {
    std::vector<double> v;
    void add(double x) { v.push_back(x); }
    double mean() const { double s = 0; for (double x : v) s += x; return v.empty() ? 0 : s / double(v.size()); }
    double pct(double p) const {
        if (v.empty()) return 0;
        auto c = v;
        std::sort(c.begin(), c.end());
        return c[std::min(c.size() - 1, size_t(p * double(c.size() - 1) + 0.5))];
    }
    double maxv() const { return v.empty() ? 0 : *std::max_element(v.begin(), v.end()); }
    double minv() const { return v.empty() ? 0 : *std::min_element(v.begin(), v.end()); }
    std::string str() const {
        char b[256];
        snprintf(b, sizeof(b), "n=%zu mean=%.3f min=%.3f p50=%.3f p95=%.3f max=%.3f", v.size(), mean(), minv(), pct(0.5), pct(0.95), maxv());
        return b;
    }
};

// ---------------------------------------------------------------------------------------
// App-side renderer: uploads CPU frames into the proxy back buffer + MV + depth
// ---------------------------------------------------------------------------------------
struct Slot {
    ComPtr<ID3D12Resource> upColor, upMv, upDepth;
    uint8_t *mColor = nullptr, *mMv = nullptr, *mDepth = nullptr;
    ComPtr<ID3D12Resource> mv, depth;
    D3D12_RESOURCE_STATES mvState = D3D12_RESOURCE_STATE_COPY_DEST, depthState = D3D12_RESOURCE_STATE_COPY_DEST;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    uint64_t fence = 0;
};

struct App {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    uint64_t fenceValue = 0;
    HANDLE event = nullptr;
    Slot slots[3];
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fpColor{}, fpMv{}, fpDepth{};
    Scene scene;
    xell_context_handle_t ll = nullptr;
    xefg_swapchain_handle_t fg = nullptr;
    ComPtr<IDXGISwapChain4> proxy;
    std::vector<std::vector<uint8_t>> realFrames; // index k = frame id k (1-based), [0] unused
    Stats presentMs, sleepMs, renderMs;
    uint64_t statusGenerated = 0, statusPresented = 0, statusErrors = 0;
    std::map<int, uint64_t> statusResults;
    uint32_t frameId = 0;
    // Pre-rendered frames for the timing phases (keeps CPU rendering out of the measurement).
    std::vector<std::vector<uint8_t>> cacheImg;
    std::vector<std::vector<uint16_t>> cacheMv;
    bool useCache = false;

    void buildCache(uint32_t n) {
        cacheImg.resize(n);
        cacheMv.resize(n);
        for (uint32_t i = 0; i < n; ++i) scene.render(1.0 + i, cacheImg[i], &cacheMv[i]);
    }
    // Waits until the GPU finished everything XeFG presented so far.
    void waitPresented() {
        ComPtr<ID3D12Fence> f;
        uint64_t v = 0;
        {
            std::lock_guard l(g_lastPresentMx);
            f = g_lastPresentFence;
            v = g_lastPresentValue;
        }
        if (!f || f->GetCompletedValue() >= v) return;
        HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        f->SetEventOnCompletion(v, e);
        WaitForSingleObject(e, 20000);
        CloseHandle(e);
    }

    void waitFence(uint64_t v) {
        if (fence->GetCompletedValue() >= v) return;
        fence->SetEventOnCompletion(v, event);
        WaitForSingleObject(event, 20000);
    }
    void idle() {
        queue->Signal(fence.Get(), ++fenceValue);
        waitFence(fenceValue);
    }
    HRESULT makeBuffer(uint64_t size, D3D12_HEAP_TYPE type, ComPtr<ID3D12Resource>& r) {
        D3D12_HEAP_PROPERTIES hp{type};
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = size; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc = {1, 0};
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&r));
    }
    HRESULT makeTex(DXGI_FORMAT f, ComPtr<ID3D12Resource>& r) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = scene.W; rd.Height = scene.H; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = f; rd.SampleDesc = {1, 0};
        return device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r));
    }
    HRESULT initResources() {
        HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        if (FAILED(hr)) return hr;
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        auto fp = [&](DXGI_FORMAT f, D3D12_PLACED_SUBRESOURCE_FOOTPRINT& out) {
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = scene.W; rd.Height = scene.H; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = f; rd.SampleDesc = {1, 0};
            UINT64 total = 0;
            device->GetCopyableFootprints(&rd, 0, 1, 0, &out, nullptr, nullptr, &total);
            return total;
        };
        const uint64_t sc = fp(DXGI_FORMAT_R8G8B8A8_UNORM, fpColor), sm = fp(DXGI_FORMAT_R16G16_FLOAT, fpMv), sd = fp(DXGI_FORMAT_R32_FLOAT, fpDepth);
        for (auto& s : slots) {
            if (FAILED(hr = makeBuffer(sc, D3D12_HEAP_TYPE_UPLOAD, s.upColor)) || FAILED(hr = makeBuffer(sm, D3D12_HEAP_TYPE_UPLOAD, s.upMv)) ||
                FAILED(hr = makeBuffer(sd, D3D12_HEAP_TYPE_UPLOAD, s.upDepth)) || FAILED(hr = makeTex(DXGI_FORMAT_R16G16_FLOAT, s.mv)) ||
                FAILED(hr = makeTex(DXGI_FORMAT_R32_FLOAT, s.depth)) ||
                FAILED(hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.alloc))) ||
                FAILED(hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.alloc.Get(), nullptr, IID_PPV_ARGS(&s.list))))
                return hr;
            s.list->Close();
            s.mv->SetName(L"pushmpc-xefg-mv");
            s.depth->SetName(L"pushmpc-xefg-depth");
            s.upColor->Map(0, nullptr, (void**)&s.mColor);
            s.upMv->Map(0, nullptr, (void**)&s.mMv);
            s.upDepth->Map(0, nullptr, (void**)&s.mDepth);
            // Constant depth (0.5) never changes.
            for (uint32_t y = 0; y < scene.H; ++y) {
                float* row = reinterpret_cast<float*>(s.mDepth + fpDepth.Offset + size_t(y) * fpDepth.Footprint.RowPitch);
                for (uint32_t x = 0; x < scene.W; ++x) row[x] = 0.5f;
            }
        }
        return S_OK;
    }

    static void barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        if (a == b) return;
        D3D12_RESOURCE_BARRIER br{};
        br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        br.Transition.pResource = r;
        br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        br.Transition.StateBefore = a;
        br.Transition.StateAfter = b;
        l->ResourceBarrier(1, &br);
    }

    bool marker(xell_latency_marker_type_t m) {
        const auto r = api.xellAddMarkerData(ll, frameId, m);
        if (r != XELL_RESULT_SUCCESS) logf("XeLL marker %d frame=%u result=%d", int(m), frameId, int(r));
        return r >= 0;
    }

    // One app frame. t = scene time (real-frame units). keep: store the CPU frame.
    bool frame(double t, bool keep, bool reset, float frameMs) {
        ++frameId;
        g_appFrame = frameId;
        Slot& s = slots[frameId % 3];
        waitFence(s.fence);
        const double ts0 = nowMs();
        const auto sr = api.xellSleep(ll, frameId);
        sleepMs.add(nowMs() - ts0);
        if (sr != XELL_RESULT_SUCCESS) logf("xellSleep frame=%u result=%d", frameId, int(sr));
        marker(XELL_SIMULATION_START);
        const double tr0 = nowMs();
        std::vector<uint8_t> imgLocal;
        std::vector<uint16_t> mvLocal;
        const std::vector<uint8_t>* imgp = &imgLocal;
        const std::vector<uint16_t>* mvp = &mvLocal;
        if (useCache && !cacheImg.empty()) {
            const size_t ci = size_t(std::max(0.0, t - 1.0)) % cacheImg.size();
            imgp = &cacheImg[ci];
            mvp = &cacheMv[ci];
        } else {
            scene.render(t, imgLocal, &mvLocal);
        }
        const auto& img = *imgp;
        const auto& mv = *mvp;
        for (uint32_t y = 0; y < scene.H; ++y) {
            memcpy(s.mColor + fpColor.Offset + size_t(y) * fpColor.Footprint.RowPitch, &img[size_t(y) * scene.W * 4], size_t(scene.W) * 4);
            memcpy(s.mMv + fpMv.Offset + size_t(y) * fpMv.Footprint.RowPitch, &mv[size_t(y) * scene.W * 2], size_t(scene.W) * 4);
        }
        renderMs.add(nowMs() - tr0);
        if (keep) {
            if (realFrames.size() <= frameId) realFrames.resize(frameId + 1);
            realFrames[frameId] = imgLocal;
        }
        marker(XELL_SIMULATION_END);
        marker(XELL_RENDERSUBMIT_START);
        const UINT bbIndex = proxy->GetCurrentBackBufferIndex();
        ComPtr<ID3D12Resource> bb;
        HRESULT hr = proxy->GetBuffer(bbIndex, IID_PPV_ARGS(&bb));
        if (FAILED(hr)) { logf("proxy GetBuffer(%u) hr=0x%08lx", bbIndex, hr); return false; }
        s.alloc->Reset();
        s.list->Reset(s.alloc.Get(), nullptr);
        barrier(s.list.Get(), bb.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        barrier(s.list.Get(), s.mv.Get(), s.mvState, D3D12_RESOURCE_STATE_COPY_DEST);
        barrier(s.list.Get(), s.depth.Get(), s.depthState, D3D12_RESOURCE_STATE_COPY_DEST);
        auto copy = [&](ID3D12Resource* dst, ID3D12Resource* src, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp) {
            D3D12_TEXTURE_COPY_LOCATION d{dst, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            d.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION sl{src, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            sl.PlacedFootprint = fp;
            s.list->CopyTextureRegion(&d, 0, 0, 0, &sl, nullptr);
        };
        copy(bb.Get(), s.upColor.Get(), fpColor);
        copy(s.mv.Get(), s.upMv.Get(), fpMv);
        copy(s.depth.Get(), s.upDepth.Get(), fpDepth);
        barrier(s.list.Get(), bb.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        barrier(s.list.Get(), s.mv.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barrier(s.list.Get(), s.depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        s.mvState = s.depthState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        s.list->Close();
        ID3D12CommandList* lists[] = {s.list.Get()};
        queue->ExecuteCommandLists(1, lists);
        bb.Reset();

        auto tag = [&](xefg_swapchain_resource_type_t type, ID3D12Resource* r) {
            xefg_swapchain_d3d12_resource_data_t d{};
            d.type = type;
            d.validity = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
            d.resourceBase = {0, 0};
            d.resourceSize = {scene.W, scene.H};
            d.pResource = r;
            d.incomingState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            const auto res = api.xefgSwapChainD3D12TagFrameResource(fg, nullptr, frameId, &d);
            if (res != XEFG_SWAPCHAIN_RESULT_SUCCESS) logf("TagFrameResource type=%d frame=%u result=%d", int(type), frameId, int(res));
        };
        tag(XEFG_SWAPCHAIN_RES_MOTION_VECTOR, s.mv.Get());
        tag(XEFG_SWAPCHAIN_RES_DEPTH, s.depth.Get());
        xefg_swapchain_frame_constant_data_t c{};
        for (int i = 0; i < 4; ++i) { c.viewMatrix[i * 5] = 1; c.projectionMatrix[i * 5] = 1; }
        c.motionVectorScaleX = c.motionVectorScaleY = 1;
        c.resetHistory = reset ? 1 : 0;
        c.frameRenderTime = frameMs;
        auto res = api.xefgSwapChainTagFrameConstants(fg, frameId, &c);
        if (res != XEFG_SWAPCHAIN_RESULT_SUCCESS) logf("TagFrameConstants frame=%u result=%d", frameId, int(res));
        res = api.xefgSwapChainSetPresentId(fg, frameId);
        if (res != XEFG_SWAPCHAIN_RESULT_SUCCESS) logf("SetPresentId frame=%u result=%d", frameId, int(res));
        marker(XELL_RENDERSUBMIT_END);
        marker(XELL_PRESENT_START);
        const double tp0 = nowMs();
        hr = proxy->Present(0, DXGI_PRESENT_ALLOW_TEARING);
        presentMs.add(nowMs() - tp0);
        marker(XELL_PRESENT_END);
        if (FAILED(hr)) logf("proxy Present frame=%u hr=0x%08lx", frameId, hr);
        xefg_swapchain_present_status_t st{};
        res = api.xefgSwapChainGetLastPresentStatus(fg, &st);
        if (res >= 0) {
            ++statusResults[int(st.frameGenResult)];
            statusPresented += st.framesPresented;
            if (st.isFrameGenEnabled && st.frameGenResult == XEFG_SWAPCHAIN_RESULT_SUCCESS && st.framesPresented > 1) statusGenerated += st.framesPresented - 1;
            if (st.frameGenResult < 0) ++statusErrors;
        }
        if (frameId <= 4 || frameId % 20 == 0 || st.frameGenResult < 0)
            logf("frame=%u present hr=0x%08lx status res=%d framesPresented=%u enabled=%u frameGenResult=%d", frameId, hr, int(res), st.framesPresented, st.isFrameGenEnabled, int(st.frameGenResult));
        queue->Signal(fence.Get(), ++fenceValue);
        s.fence = fenceValue;
        return SUCCEEDED(hr);
    }
};

// Reads back finished captures (fence complete) into CPU memory.
static void drainCaptures(bool wait) {
    std::vector<Capture*> todo;
    {
        std::lock_guard l(g_capMx);
        for (auto& c : g_captures)
            if (c->readback && !c->hasImage) todo.push_back(c.get());
    }
    for (Capture* c : todo) {
        if (c->fence->GetCompletedValue() < c->fenceValue) {
            if (!wait) continue;
            HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            c->fence->SetEventOnCompletion(c->fenceValue, e);
            if (WaitForSingleObject(e, 10000) != WAIT_OBJECT_0) { CloseHandle(e); logf("capture %u fence timeout", c->serial); continue; }
            CloseHandle(e);
        }
        uint8_t* p = nullptr;
        D3D12_RANGE range{0, SIZE_T(c->fp.Footprint.RowPitch) * c->fp.Footprint.Height};
        if (SUCCEEDED(c->readback->Map(0, &range, (void**)&p))) {
            const uint32_t W = c->fp.Footprint.Width, H = c->fp.Footprint.Height;
            c->rgba.resize(size_t(W) * H * 4);
            for (uint32_t y = 0; y < H; ++y) memcpy(&c->rgba[size_t(y) * W * 4], p + c->fp.Offset + size_t(y) * c->fp.Footprint.RowPitch, size_t(W) * 4);
            D3D12_RANGE none{0, 0};
            c->readback->Unmap(0, &none);
            c->hasImage = true;
        }
        c->readback.Reset();
        c->list.Reset();
        c->alloc.Reset();
    }
}

// ---------------------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------------------
static void usage() {
    printf("xefg_headless_spike [--warp] [--adapter N] [--width W] [--height H] [--frames N] [--interp K]\n"
           "  [--fps F] [--speed vx,vy] [--square S] [--refresh-hz R] [--vsync] [--window] [--no-output]\n"
           "  [--no-unlock] [--no-low-latency] [--debug-layer] [--timing-frames M] [--png N] [--out DIR]\n"
           "  [--xess-dir DIR] [--verbose-calls N]\n");
}

static bool parseArgs(int argc, wchar_t** argv) {
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (a == L"--warp") g_opt.warp = true;
        else if (a == L"--adapter") g_opt.adapter = _wtoi(next().c_str());
        else if (a == L"--width") g_opt.width = _wtoi(next().c_str());
        else if (a == L"--height") g_opt.height = _wtoi(next().c_str());
        else if (a == L"--frames") g_opt.frames = _wtoi(next().c_str());
        else if (a == L"--interp") g_opt.interp = std::max(1, _wtoi(next().c_str()));
        else if (a == L"--fps") g_opt.fps = _wtof(next().c_str());
        else if (a == L"--speed") { auto v = next(); swscanf_s(v.c_str(), L"%lf,%lf", &g_opt.vx, &g_opt.vy); }
        else if (a == L"--square") g_opt.square = _wtoi(next().c_str());
        else if (a == L"--refresh-hz") g_opt.refreshHz = _wtof(next().c_str());
        else if (a == L"--vsync") g_opt.emulateVsync = true;
        else if (a == L"--window") g_opt.realWindow = true;
        else if (a == L"--no-output") g_opt.outputShim = false;
        else if (a == L"--no-unlock") g_opt.unlock = false;
        else if (a == L"--no-low-latency") g_opt.lowLatency = false;
        else if (a == L"--debug-layer") g_opt.debugLayer = true;
        else if (a == L"--timing-frames") g_opt.timingFrames = _wtoi(next().c_str());
        else if (a == L"--png") g_opt.pngCount = _wtoi(next().c_str());
        else if (a == L"--out") g_opt.outDir = next();
        else if (a == L"--xess-dir") g_opt.xessDir = next();
        else if (a == L"--verbose-calls") g_opt.verboseCalls = _wtoi(next().c_str());
        else if (a == L"--max-captures") g_opt.maxCaptures = _wtoi(next().c_str());
        else { usage(); return false; }
    }
    return true;
}

int wmain(int argc, wchar_t** argv) {
    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_qp0);
    crcInit();
    if (!parseArgs(argc, argv)) return 2;
    std::filesystem::create_directories(g_opt.outDir);
    const std::filesystem::path out = g_opt.outDir;
    g_logFile = _wfopen((out / L"log.txt").c_str(), L"w");
    g_reportFile = _wfopen((out / L"report.txt").c_str(), L"w");
    timeBeginPeriod(1);

    reportf("tool xefg_headless_spike interp=%u frames=%u size=%ux%u fps=%.2f speed=%.2f,%.2f square=%u refreshHz=%.1f warp=%d window=%d outputShim=%d vsyncEmu=%d lowLatency=%d",
            g_opt.interp, g_opt.frames, g_opt.width, g_opt.height, g_opt.fps, g_opt.vx, g_opt.vy, g_opt.square, g_opt.refreshHz,
            g_opt.warp, g_opt.realWindow, g_opt.outputShim, g_opt.emulateVsync, g_opt.lowLatency);

    // --- XeSS runtime -------------------------------------------------------------------
    std::wstring dir = g_opt.xessDir;
    if (dir.empty()) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        dir = std::filesystem::path(exe).parent_path().wstring();
    }
    const std::wstring llPath = (std::filesystem::path(dir) / L"libxell.dll").wstring();
    const std::wstring fgPath = (std::filesystem::path(dir) / L"libxess_fg.dll").wstring();
    for (const auto& p : {llPath, fgPath}) {
        uint64_t size = 0;
        const auto sha = sha256File(p, size);
        reportf("runtime %s version=%s size=%llu sha256=%s", narrow(std::filesystem::path(p).filename().wstring()).c_str(),
                fileVersion(p).c_str(), (unsigned long long)size, sha.c_str());
    }
    HMODULE ll = LoadLibraryExW(llPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    HMODULE fgm = LoadLibraryExW(fgPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!ll || !fgm) {
        reportf("SPIKE_RESULT FAIL runtime-load win32=%lu", GetLastError());
        return 3;
    }
#define LOADP(mod, name) api.name = reinterpret_cast<decltype(api.name)>(GetProcAddress(mod, #name)); if (!api.name) { reportf("SPIKE_RESULT FAIL missing-export %s", #name); return 3; }
    LOADP(fgm, xefgSwapChainGetVersion) LOADP(fgm, xefgSwapChainD3D12CreateContext) LOADP(fgm, xefgSwapChainSetLoggingCallback)
    LOADP(fgm, xefgSwapChainD3D12GetProperties) LOADP(fgm, xefgSwapChainGetProperties) LOADP(fgm, xefgSwapChainSetNumInterpolatedFrames)
    LOADP(fgm, xefgSwapChainD3D12InitFromSwapChain) LOADP(fgm, xefgSwapChainD3D12GetSwapChainPtr) LOADP(fgm, xefgSwapChainD3D12TagFrameResource)
    LOADP(fgm, xefgSwapChainTagFrameConstants) LOADP(fgm, xefgSwapChainSetEnabled) LOADP(fgm, xefgSwapChainSetPresentId)
    LOADP(fgm, xefgSwapChainGetLastPresentStatus) LOADP(fgm, xefgSwapChainSetLatencyReduction) LOADP(fgm, xefgSwapChainDestroy)
    LOADP(ll, xellD3D12CreateContext) LOADP(ll, xellDestroyContext) LOADP(ll, xellSetSleepMode) LOADP(ll, xellSleep)
    LOADP(ll, xellAddMarkerData)
#undef LOADP
    // Optional entry points.
    api.xefgSwapChainD3D12GetInitializationParameters = reinterpret_cast<decltype(api.xefgSwapChainD3D12GetInitializationParameters)>(GetProcAddress(fgm, "xefgSwapChainD3D12GetInitializationParameters"));
    api.xellGetVersion = reinterpret_cast<decltype(api.xellGetVersion)>(GetProcAddress(ll, "xellGetVersion"));
    api.xellSetLoggingCallback = reinterpret_cast<decltype(api.xellSetLoggingCallback)>(GetProcAddress(ll, "xellSetLoggingCallback"));
    xefg_swapchain_version_t fv{};
    api.xefgSwapChainGetVersion(&fv);
    xell_version_t lv{};
    if (api.xellGetVersion) api.xellGetVersion(&lv);
    reportf("api xefg=%u.%u.%u xell=%u.%u.%u", fv.major, fv.minor, fv.patch, lv.major, lv.minor, lv.patch);

    // --- DXGI / D3D12 ---------------------------------------------------------------------
    ComPtr<IDXGIFactory7> realFactory;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&realFactory));
    if (FAILED(hr)) { reportf("SPIKE_RESULT FAIL dxgi-factory hr=0x%08lx", hr); return 4; }
    g_shimFactory.Attach(new ShimFactory(realFactory.Get()));
    installHooks(fgm, ll);

    if (g_opt.debugLayer) {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer();
    }
    ComPtr<IDXGIAdapter1> adapter;
    if (g_opt.warp) realFactory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
    else {
        for (UINT i = 0; realFactory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) == S_OK; ++i) {
            DXGI_ADAPTER_DESC1 d;
            adapter->GetDesc1(&d);
            if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { adapter.Reset(); continue; }
            if (g_opt.adapter < 0 || int(i) == g_opt.adapter) break;
            adapter.Reset();
        }
    }
    if (!adapter) { reportf("SPIKE_RESULT FAIL no-adapter"); return 4; }
    adapter.As(&g_adapter);
    {
        DXGI_ADAPTER_DESC1 d;
        adapter->GetDesc1(&d);
        LARGE_INTEGER umd{};
        adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd);
        reportf("adapter name=\"%s\" vendor=0x%04x device=0x%04x vram=%lluMB driver=%u.%u.%u.%u software=%d", narrow(d.Description).c_str(), d.VendorId, d.DeviceId,
                (unsigned long long)(d.DedicatedVideoMemory >> 20), HIWORD(umd.HighPart), LOWORD(umd.HighPart), HIWORD(umd.LowPart), LOWORD(umd.LowPart),
                (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? 1 : 0);
    }
    App app;
    hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&app.device));
    if (FAILED(hr)) { reportf("SPIKE_RESULT FAIL d3d12-device hr=0x%08lx", hr); return 4; }
    {
        D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_6};
        app.device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm));
        reportf("device shaderModel=0x%x (XeFG needs >= 0x64)", unsigned(sm.HighestShaderModel));
    }
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    app.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&app.queue));
    app.scene.init(g_opt.width, g_opt.height, g_opt.square, g_opt.vx, g_opt.vy);
    if (FAILED(hr = app.initResources())) { reportf("SPIKE_RESULT FAIL app-resources hr=0x%08lx", hr); return 4; }

    if (g_opt.realWindow) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"PushMpcXefgSpike";
        RegisterClassW(&wc);
        g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, int(g_opt.width), int(g_opt.height), nullptr, nullptr, wc.hInstance, nullptr);
        g_hwndFake = false;
        reportf("window hidden real hwnd=%p (never shown)", (void*)g_hwnd);
    } else {
        reportf("window fake hwnd=%p (no window exists)", (void*)g_hwnd);
    }

    // --- MFG unlock -------------------------------------------------------------------------
    bool unlocked = false;
    if (g_opt.interp > 1) {
        if (g_opt.unlock) {
            std::string detail;
            unlocked = unlock::apply(fgm, fgPath, g_opt.interp, detail);
            reportf("unlock XessFg-MFG requested=%u applied=%d detail=%s", g_opt.interp, unlocked, detail.c_str());
        } else {
            reportf("unlock skipped (--no-unlock)");
        }
    }

    // --- XeLL + XeFG ------------------------------------------------------------------------
    bool ok = true;
    auto fail = [&](const char* what) { reportf("SPIKE_RESULT FAIL %s", what); ok = false; };
    if (initResult("xellD3D12CreateContext", api.xellD3D12CreateContext(app.device.Get(), &app.ll)) < 0) fail("xellD3D12CreateContext");
    if (ok && api.xellSetLoggingCallback) api.xellSetLoggingCallback(app.ll, XELL_LOGGING_LEVEL_DEBUG, xellLog);
    if (ok && initResult("xefgSwapChainD3D12CreateContext", api.xefgSwapChainD3D12CreateContext(app.device.Get(), &app.fg)) < 0) fail("xefgSwapChainD3D12CreateContext");
    if (ok) initResult("xefgSwapChainSetLoggingCallback", api.xefgSwapChainSetLoggingCallback(app.fg, XEFG_SWAPCHAIN_LOGGING_LEVEL_DEBUG, xefgLog, nullptr));
    if (ok && initResult("xefgSwapChainSetLatencyReduction", api.xefgSwapChainSetLatencyReduction(app.fg, app.ll)) < 0) fail("xefgSwapChainSetLatencyReduction");
    if (ok) {
        xell_sleep_params_t sp{};
        sp.bLowLatencyMode = g_opt.lowLatency ? 1 : 0;
        if (initResult("xellSetSleepMode", api.xellSetSleepMode(app.ll, &sp)) < 0) fail("xellSetSleepMode");
    }
    uint32_t maxInterp = 0;
    if (ok) {
        xefg_swapchain_properties_t p{};
        initResult("xefgSwapChainGetProperties", api.xefgSwapChainGetProperties(app.fg, &p));
        maxInterp = p.maxSupportedInterpolations;
        reportf("runtime maxSupportedInterpolations=%u", maxInterp);
        if (g_opt.interp > std::max(1u, maxInterp)) fail("requested interpolation above runtime ceiling");
    }
    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = g_opt.width;
    scd.Height = g_opt.height;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc = {1, 0};
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 3;
    scd.Scaling = DXGI_SCALING_STRETCH;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    scd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    xefg_swapchain_d3d12_init_params_t init{};
    init.maxInterpolatedFrames = g_opt.interp;
    init.uiMode = XEFG_SWAPCHAIN_UI_MODE_AUTO;
    if (ok) {
        xefg_swapchain_properties_t p{};
        initResult("xefgSwapChainD3D12GetProperties", api.xefgSwapChainD3D12GetProperties(app.fg, &init, scd.Width, scd.Height, scd.Format, &p));
        reportf("properties maxInterp=%u descriptors=%u bufferHeap=%llu textureHeap=%llu cb=%llu", p.maxSupportedInterpolations, p.requiredDescriptorCount,
                (unsigned long long)p.tempBufferHeapSize, (unsigned long long)p.tempTextureHeapSize, (unsigned long long)p.constantBufferSize);
    }
    if (ok) {
        ComPtr<IDXGISwapChain1> appChain;
        hr = g_shimFactory->CreateSwapChainForHwnd(app.queue.Get(), g_hwnd, &scd, nullptr, nullptr, &appChain);
        initResult("shim CreateSwapChainForHwnd(app)", int(hr));
        IDXGISwapChain1* raw = appChain.Detach();
        raw->AddRef();
        const ULONG refPlusOne = raw->Release() + 1;
        reportf("app shim swapchain refcount=%lu before InitFromSwapChain (XeFG requires 1)", refPlusOne - 1);
        init.pApplicationSwapChain = raw;
        const double t0 = nowMs();
        const int r = initResult("xefgSwapChainD3D12InitFromSwapChain", api.xefgSwapChainD3D12InitFromSwapChain(app.fg, app.queue.Get(), &init));
        reportf("InitFromSwapChain took %.1f ms, shim swapchains alive=%u created=%u", nowMs() - t0, g_swapAlive.load(), g_swapSerial.load());
        if (r < 0) fail("xefgSwapChainD3D12InitFromSwapChain");
    }
    if (ok) {
        xefg_swapchain_d3d12_init_params_t used{};
        if (api.xefgSwapChainD3D12GetInitializationParameters && api.xefgSwapChainD3D12GetInitializationParameters(app.fg, &used) >= 0)
            reportf("init params used maxInterpolatedFrames=%u flags=0x%x", used.maxInterpolatedFrames, used.initFlags);
        if (initResult("xefgSwapChainD3D12GetSwapChainPtr", api.xefgSwapChainD3D12GetSwapChainPtr(app.fg, IID_PPV_ARGS(&app.proxy))) < 0) fail("GetSwapChainPtr");
    }
    if (ok && initResult("xefgSwapChainSetNumInterpolatedFrames", api.xefgSwapChainSetNumInterpolatedFrames(app.fg, g_opt.interp)) < 0) fail("SetNumInterpolatedFrames");
    if (ok && initResult("xefgSwapChainSetEnabled(1)", api.xefgSwapChainSetEnabled(app.fg, 1)) < 0) fail("SetEnabled");

    // --- Phase 1: sanity, paced ----------------------------------------------------------------
    const double period = g_opt.fps > 0 ? 1000.0 / g_opt.fps : 0.0;
    std::vector<double> appPresentStart;
    if (ok) {
        g_phase = 1;
        logf("phase 1: %u frames at %.2f fps", g_opt.frames, g_opt.fps);
        const double start = nowMs();
        for (uint32_t k = 1; k <= g_opt.frames; ++k) {
            appPresentStart.push_back(nowMs());
            if (!app.frame(double(k), true, k == 1, float(period > 0 ? period : 16.7))) {
                logf("frame %u failed", k);
            }
            drainCaptures(false);
            if (period > 0) {
                const double target = start + period * k;
                while (nowMs() < target) {
                    const double left = target - nowMs();
                    if (left > 2) Sleep(DWORD(left - 1));
                    else YieldProcessor();
                }
            }
        }
        app.idle();
        Sleep(400); // let the XeFG present thread flush its queue
        drainCaptures(true);
    }
    const uint32_t phase1Frames = app.frameId;
    const uint64_t phase1StatusGen = app.statusGenerated, phase1StatusPresented = app.statusPresented;

    // --- Phases 2/3: unthrottled timing, FG on / FG off ---------------------------------------
    double t2 = 0, t3 = 0;
    uint64_t gen2 = 0, out2 = 0, out3 = 0;
    if (ok && g_opt.timingFrames > 0) {
        g_captureImages = false;
        app.buildCache(16);
        app.useCache = true;
        g_phase = 2;
        uint64_t g0 = app.statusGenerated, p0 = app.statusPresented;
        double s = nowMs();
        for (uint32_t i = 0; i < g_opt.timingFrames; ++i) app.frame(1.0 + (i % 16), false, i == 0, 0.f);
        app.idle();
        app.waitPresented();
        t2 = nowMs() - s;
        gen2 = app.statusGenerated - g0;
        out2 = app.statusPresented - p0;
        Sleep(200);
        api.xefgSwapChainSetEnabled(app.fg, 0);
        g_phase = 3;
        p0 = app.statusPresented;
        s = nowMs();
        for (uint32_t i = 0; i < g_opt.timingFrames; ++i) app.frame(1.0 + (i % 16), false, false, 0.f);
        app.idle();
        app.waitPresented();
        t3 = nowMs() - s;
        out3 = app.statusPresented - p0;
        Sleep(200);
    }

    // --- Shutdown ------------------------------------------------------------------------------
    if (app.proxy) app.proxy.Reset();
    if (app.fg) initResult("xefgSwapChainDestroy", api.xefgSwapChainDestroy(app.fg));
    if (app.ll) initResult("xellDestroyContext", api.xellDestroyContext(app.ll));
    if (unlocked) unlock::release();
    reportf("shim swapchains alive after destroy=%u (expect 0)", g_swapAlive.load());

    // --- Analysis ---------------------------------------------------------------------------------
    std::vector<Capture*> caps;
    {
        std::lock_guard l(g_capMx);
        for (auto& c : g_captures) caps.push_back(c.get());
    }
    std::sort(caps.begin(), caps.end(), [](Capture* a, Capture* b) { return a->serial < b->serial; });
    const uint32_t W = g_opt.width, H = g_opt.height, S = g_opt.square;
    const Scene& sc = app.scene;

    // Classification of phase-1 outputs.
    struct Out { Capture* c; int realK = -1; double psnrReal = 0; bool gen = false; };
    std::vector<Out> outs;
    for (Capture* c : caps)
        if (c->phase == 1) outs.push_back({c});
    uint32_t noImage = 0;
    for (auto& o : outs) {
        if (!o.c->hasImage || o.c->rgba.size() != size_t(W) * H * 4) { ++noImage; continue; }
        double cx, cy;
        int guess = 1;
        if (centroid(o.c->rgba.data(), W, H, cx, cy) && g_opt.vx != 0) guess = int(std::lround((cx - S / 2.0 - sc.x0) / g_opt.vx));
        double best = -1;
        int bestK = -1;
        for (int k = std::max(1, guess - 2); k <= std::min(int(phase1Frames), guess + 2); ++k) {
            if (size_t(k) >= app.realFrames.size() || app.realFrames[k].empty()) continue;
            const double p = psnr(o.c->rgba.data(), app.realFrames[k].data(), W, H);
            if (p > best) { best = p; bestK = k; }
        }
        o.psnrReal = best;
        // Real outputs are copies of the rendered frame (bit exact here); 50 dB keeps the
        // generated frame at phase 1/(n+1), which can be ~40 dB from its neighbour, out.
        if (best >= 50.0) o.realK = bestK;
        else o.gen = true;
    }
    // Sequence analysis: generated outputs between consecutive real outputs.
    uint32_t realOut = 0, genOut = 0, dupReal = 0, transitions = 0, transitionsWithGen = 0, leadingGen = 0;
    std::map<uint32_t, uint32_t> genPerTransition;
    Stats posErr, psnrGenIdeal, psnrGenIdealRoi, psnrRepeatRoi, psnrBlendRoi, psnrGenReal;
    struct PngJob { int a, b; std::vector<Capture*> gens; };
    std::vector<PngJob> pngJobs;
    int lastReal = -1;
    std::vector<size_t> pendingGen;
    for (size_t i = 0; i < outs.size(); ++i) {
        auto& o = outs[i];
        if (o.gen) { pendingGen.push_back(i); continue; }
        if (o.realK < 0) continue;
        ++realOut;
        if (lastReal < 0) { leadingGen += uint32_t(pendingGen.size()); pendingGen.clear(); lastReal = o.realK; continue; }
        if (o.realK == lastReal) { ++dupReal; genOut += uint32_t(pendingGen.size()); pendingGen.clear(); continue; }
        ++transitions;
        const int a = lastReal, b = o.realK;
        const uint32_t m = uint32_t(pendingGen.size());
        ++genPerTransition[m];
        if (m) ++transitionsWithGen;
        PngJob job{a, b, {}};
        for (uint32_t j = 0; j < m; ++j) {
            Capture* g = outs[pendingGen[j]].c;
            ++genOut;
            const double phase = double(j + 1) / double(m + 1);
            const double t = double(a) + phase * double(b - a);
            std::vector<uint8_t> ideal;
            sc.render(t, ideal, nullptr);
            const int rx1 = int(std::floor(std::min(sc.px(a), sc.px(b)))) - 8, ry1 = int(std::floor(std::min(sc.py(a), sc.py(b)))) - 8;
            const int rx2 = int(std::ceil(std::max(sc.px(a), sc.px(b)) + S)) + 8, ry2 = int(std::ceil(std::max(sc.py(a), sc.py(b)) + S)) + 8;
            const Rect roi{rx1, ry1, rx2, ry2};
            psnrGenIdeal.add(psnr(g->rgba.data(), ideal.data(), W, H));
            psnrGenIdealRoi.add(psnr(g->rgba.data(), ideal.data(), W, H, &roi));
            psnrRepeatRoi.add(psnr(app.realFrames[a].data(), ideal.data(), W, H, &roi));
            std::vector<uint8_t> blend(ideal.size());
            for (size_t q = 0; q < blend.size(); ++q)
                blend[q] = uint8_t(std::lround(app.realFrames[a][q] * (1.0 - phase) + app.realFrames[b][q] * phase));
            psnrBlendRoi.add(psnr(blend.data(), ideal.data(), W, H, &roi));
            psnrGenReal.add(outs[pendingGen[j]].psnrReal);
            double gx, gy, ix, iy;
            if (centroid(g->rgba.data(), W, H, gx, gy) && centroid(ideal.data(), W, H, ix, iy)) posErr.add(std::hypot(gx - ix, gy - iy));
            else posErr.add(99.0);
            if (j < 4) logf("gen out serial=%u between real %d and %d phase=%.3f centroid=(%.2f,%.2f) ideal=(%.2f,%.2f) psnrIdeal=%.2f roi=%.2f", g->serial, a, b, phase,
                            gx, gy, ix, iy, psnrGenIdeal.v.back(), psnrGenIdealRoi.v.back());
            job.gens.push_back(g);
        }
        if (m && pngJobs.size() < g_opt.pngCount && a >= int(phase1Frames) / 4 && (pngJobs.empty() || a >= pngJobs.back().a + int(phase1Frames) / 5)) pngJobs.push_back(job);
        pendingGen.clear();
        lastReal = b;
    }
    const uint32_t trailingGen = uint32_t(pendingGen.size());

    // PNGs
    for (const auto& j : pngJobs) {
        char name[128];
        snprintf(name, sizeof(name), "real_%03d.png", j.a);
        writePng(out / name, app.realFrames[j.a].data(), W, H);
        snprintf(name, sizeof(name), "real_%03d.png", j.b);
        writePng(out / name, app.realFrames[j.b].data(), W, H);
        for (size_t g = 0; g < j.gens.size(); ++g) {
            const double phase = double(g + 1) / double(j.gens.size() + 1);
            snprintf(name, sizeof(name), "gen_%03d_%zu_of_%zu.png", j.a, g + 1, j.gens.size());
            writePng(out / name, j.gens[g]->rgba.data(), W, H);
            std::vector<uint8_t> ideal;
            sc.render(double(j.a) + phase * double(j.b - j.a), ideal, nullptr);
            snprintf(name, sizeof(name), "ideal_%03d_%zu_of_%zu.png", j.a, g + 1, j.gens.size());
            writePng(out / name, ideal.data(), W, H);
        }
    }

    // Pacing of shim presents (phase 1)
    Stats gapAll, gapGenGen, gapRealGen, gapGenReal, shimPresentDur;
    std::map<DWORD, uint32_t> presentThreads;
    std::map<std::string, uint32_t> syncFlags;
    for (size_t i = 0; i < outs.size(); ++i) {
        Capture* c = outs[i].c;
        presentThreads[c->tid]++;
        char k[64];
        snprintf(k, sizeof(k), "sync=%u flags=0x%x", c->sync, c->flags);
        syncFlags[k]++;
        shimPresentDur.add(c->tExit - c->tEnter);
        if (i == 0) continue;
        const double gap = c->tEnter - outs[i - 1].c->tEnter;
        gapAll.add(gap);
        const bool pg = outs[i - 1].gen, cg = outs[i].gen;
        if (pg && cg) gapGenGen.add(gap);
        else if (!pg && cg) gapRealGen.add(gap);
        else if (pg && !cg) gapGenReal.add(gap);
    }
    // Latency of a real frame: from the app Present call to the shim Present of the same image.
    Stats realLatency;
    for (const auto& o : outs)
        if (o.realK > 0 && size_t(o.realK - 1) < appPresentStart.size()) realLatency.add(o.c->tEnter - appPresentStart[o.realK - 1]);

    // --- Report -----------------------------------------------------------------------------------
    reportf("phase1 appFrames=%u shimPresents=%zu noImage=%u realOut=%u (dup %u) genOut=%u leadingGen=%u trailingGen=%u transitions=%u transitionsWithGen=%u",
            phase1Frames, outs.size(), noImage, realOut, dupReal, genOut, leadingGen, trailingGen, transitions, transitionsWithGen);
    for (const auto& [m, n] : genPerTransition) reportf("phase1 generatedPerTransition %u -> %u transitions", m, n);
    reportf("phase1 presentStatus generated=%llu presented=%llu", (unsigned long long)phase1StatusGen, (unsigned long long)phase1StatusPresented);
    for (const auto& [r, n] : app.statusResults) reportf("presentStatus frameGenResult=%d count=%llu", r, (unsigned long long)n);
    reportf("sanity positionErrorPx %s", posErr.str().c_str());
    reportf("sanity psnrGenVsIdeal(full) %s", psnrGenIdeal.str().c_str());
    reportf("sanity psnrGenVsIdeal(roi) %s", psnrGenIdealRoi.str().c_str());
    reportf("sanity psnrRepeatVsIdeal(roi) %s", psnrRepeatRoi.str().c_str());
    reportf("sanity psnrBlendVsIdeal(roi) %s", psnrBlendRoi.str().c_str());
    reportf("sanity psnrGenVsNearestReal(full) %s", psnrGenReal.str().c_str());
    reportf("pacing shimPresentGap(all) %s", gapAll.str().c_str());
    reportf("pacing shimPresentGap(real->gen) %s", gapRealGen.str().c_str());
    reportf("pacing shimPresentGap(gen->gen) %s", gapGenGen.str().c_str());
    reportf("pacing shimPresentGap(gen->real) %s", gapGenReal.str().c_str());
    reportf("pacing shimPresentDuration %s", shimPresentDur.str().c_str());
    reportf("pacing realFrameLatency(appPresent->shimPresent) %s", realLatency.str().c_str());
    reportf("pacing appPresentCall %s", app.presentMs.str().c_str());
    reportf("pacing xellSleep %s", app.sleepMs.str().c_str());
    reportf("pacing cpuRender %s", app.renderMs.str().c_str());
    for (const auto& [tid, n] : presentThreads) reportf("pacing shimPresentThread tid=%lu count=%u%s", tid, n, tid == GetCurrentThreadId() ? " (app thread)" : " (XeFG thread)");
    for (const auto& [k, n] : syncFlags) reportf("pacing shimPresentArgs %s count=%u", k.c_str(), n);
    {
        std::lock_guard l(g_waitMx);
        for (const auto& [k, w] : g_waits)
            reportf("waits %s count=%llu totalMs=%.1f maxMs=%.3f over1ms=%llu", k.c_str(), (unsigned long long)w.count, w.totalMs, w.maxMs, (unsigned long long)w.over1ms);
    }
    // Full timeline of phase-1 outputs as CSV.
    if (FILE* tl = _wfopen((out / L"timeline.csv").c_str(), L"w")) {
        fprintf(tl, "index,serial,tEnterMs,tExitMs,appFrame,kind,realFrame,psnrVsBestReal,tid,sync,flags,buffer\n");
        for (size_t i = 0; i < outs.size(); ++i) {
            const Capture* c = outs[i].c;
            fprintf(tl, "%zu,%u,%.3f,%.3f,%u,%s,%d,%.2f,%lu,%u,0x%x,%u\n", i, c->serial, c->tEnter, c->tExit, c->appFrame, outs[i].gen ? "gen" : "real",
                    outs[i].realK, outs[i].psnrReal, c->tid, c->sync, c->flags, c->buffer);
        }
        fclose(tl);
    }
    // Timeline of the first outputs.
    for (size_t i = 0; i < outs.size() && i < 12; ++i)
        reportf("timeline out#%zu serial=%u t=%.3f appFrame=%u kind=%s real=%d psnrReal=%.1f tid=%lu", i, outs[i].c->serial, outs[i].c->tEnter, outs[i].c->appFrame,
                outs[i].gen ? "gen" : "real", outs[i].realK, outs[i].psnrReal, outs[i].c->tid);
    {
        std::lock_guard l(g_callMx);
        for (const auto& [name, c] : g_calls)
            reportf("calls %-48s count=%-6llu threads=%zu first=%.1f last=%.1f", name.c_str(), (unsigned long long)c.count, c.threads.size(), c.firstMs, c.lastMs);
    }
    double msPerGen = 0;
    if (g_opt.timingFrames > 0) {
        msPerGen = gen2 ? (t2 - t3) / double(gen2) : 0;
        reportf("timing fgOn frames=%u wallMs=%.1f outputs=%llu generated=%llu outputsPerSec=%.1f", g_opt.timingFrames, t2, (unsigned long long)out2, (unsigned long long)gen2,
                t2 > 0 ? out2 * 1000.0 / t2 : 0);
        reportf("timing fgOff frames=%u wallMs=%.1f outputs=%llu outputsPerSec=%.1f", g_opt.timingFrames, t3, (unsigned long long)out3, t3 > 0 ? out3 * 1000.0 / t3 : 0);
        reportf("timing msPerGeneratedFrame=%.3f (wall-clock difference FG on vs off per generated frame, includes XeFG pacing)", msPerGen);
    }

    // Verdict
    const uint32_t expectedGen = transitions * g_opt.interp;
    const bool initOk = ok;
    const bool countOk = transitions > 0 && genOut >= uint32_t(0.9 * expectedGen);
    const bool posOk = !posErr.v.empty() && posErr.pct(0.5) <= 2.0;
    const bool qualOk = !psnrGenIdealRoi.v.empty() && psnrGenIdealRoi.pct(0.5) > psnrBlendRoi.pct(0.5);
    const bool pass = initOk && countOk && posOk && qualOk;
    reportf("SUMMARY init=%d interp=%u genOut=%u expectedGen=%u genPerReal=%.3f medianPosErrPx=%.3f medianPsnrRoi=%.2f medianPsnrBlendRoi=%.2f medianPsnrRepeatRoi=%.2f msPerGenerated=%.3f",
            initOk, g_opt.interp, genOut, expectedGen, transitions ? double(genOut) / transitions : 0.0, posErr.pct(0.5), psnrGenIdealRoi.pct(0.5), psnrBlendRoi.pct(0.5),
            psnrRepeatRoi.pct(0.5), msPerGen);
    if (!initOk) {
        // already reported
    } else if (pass) {
        reportf("SPIKE_RESULT PASS");
    } else {
        reportf("SPIKE_RESULT FAIL count=%d position=%d quality=%d", countOk, posOk, qualOk);
    }
    timeEndPeriod(1);
    if (g_reportFile) fclose(g_reportFile);
    if (g_logFile) fclose(g_logFile);
    return pass ? 0 : 1;
}
