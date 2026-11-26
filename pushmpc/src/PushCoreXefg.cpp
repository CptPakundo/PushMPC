// pushcore: XeSS frame generation (XeFG) on a headless swap chain (PushCoreXess.h). The shim is the one of
// pushmpc/tools/xefg_headless_spike.cpp (CI xefg-spike: 2X/3X/4X on WARP), reduced to what pushcore needs:
//   * ShimFactory creates ShimSwapChains (no window, no DXGI swap chain). XeFG releases the application swap chain we give
//     InitFromSwapChain and creates its native one through the factory; that one is a shim as well. Its Present copies the
//     presented buffer (real or generated) into one of pushcore's output textures on the presenting queue.
//   * import hooks in libxess_fg.dll: CreateDXGIFactory1 / GetProcAddress (the shim factory), GetClientRect,
//     MonitorFromWindow, GetMonitorInfoA, EnumDisplaySettingsA (a fake window on a virtual monitor);
//   * the MFG unlock (OptiScaler's XeFGUnlock byte tables, as PushFPS's XessMfgUnlock, identity-gated) only when the
//     runtime reports fewer interpolations than asked for (stock XeFG: 2X on other vendors, more on Intel Arc).
// Which captured present is real and which generated: XeFG presents, for each application frame, its generated frames
// and then the real one; xefgSwapChainGetLastPresentStatus().framesPresented after each Present says how many (1 for a
// frame with no history). Captures are matched to frames in that order. Timestamps are pushcore's: real = the source pts,
// generated = evenly between the pair's two source pts.
#include "PushCoreXess.h"
#include "pushmpc/unlocks.h"
#include "veyra/Log.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <immintrin.h>

#include <xell/xell_d3d12.h>
#include <xess_fg/xefg_swapchain_d3d12.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <map>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "user32.lib")

using Microsoft::WRL::ComPtr;

namespace pushmpc::xess {
namespace {

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }
std::string narrowW(const std::wstring& s) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string r(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), r.data(), n, nullptr, nullptr);
    return r;
}
void info(const std::string& m) { veyra::log::info("xefg", m); }
void warn(const std::string& m) { veyra::log::warn("xefg", m); }

// ------------------------------------------------------------------------------------------- fake window, virtual monitor
HWND const kFakeHwnd = reinterpret_cast<HWND>(uintptr_t(0x5E1F0));
HMONITOR const kFakeMonitor = reinterpret_cast<HMONITOR>(uintptr_t(0x5E1F4));
const char* kVirtualDeviceA = "\\\\.\\PUSHMPC_VIRTUAL1";
const wchar_t* kVirtualDeviceW = L"\\\\.\\PUSHMPC_VIRTUAL1";
std::atomic<uint32_t> gW{1920}, gH{1080};
std::atomic<double> gRefreshHz{240.0};
LARGE_INTEGER gQpf{}, gQp0{};
uint64_t vblankIndexNow(LARGE_INTEGER* at = nullptr) {
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    const double hz = gRefreshHz.load();
    const uint64_t idx = uint64_t(double(q.QuadPart - gQp0.QuadPart) / double(gQpf.QuadPart) * hz);
    if (at) at->QuadPart = gQp0.QuadPart + int64_t(double(idx) / hz * double(gQpf.QuadPart));
    return idx;
}

// ------------------------------------------------------------------------------------------- private data (IDXGIObject)
struct GuidLess { bool operator()(const GUID& a, const GUID& b) const { return memcmp(&a, &b, sizeof(GUID)) < 0; } };
struct PrivateData {
    std::mutex mx;
    std::map<GUID, std::vector<uint8_t>, GuidLess> data;
    std::map<GUID, ComPtr<IUnknown>, GuidLess> ifaces;
    HRESULT set(REFGUID g, UINT size, const void* p) {
        std::lock_guard l(mx); ifaces.erase(g);
        if (!p || !size) { data.erase(g); return S_OK; }
        data[g].assign(static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + size);
        return S_OK;
    }
    HRESULT setI(REFGUID g, const IUnknown* u) {
        std::lock_guard l(mx); data.erase(g);
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
            IUnknown* u = it->second.Get(); u->AddRef(); memcpy(p, &u, sizeof(u)); *size = sizeof(IUnknown*);
            return S_OK;
        }
        auto it = data.find(g);
        if (it == data.end()) { *size = 0; return DXGI_ERROR_NOT_FOUND; }
        if (!p) { *size = UINT(it->second.size()); return S_OK; }
        if (*size < it->second.size()) { *size = UINT(it->second.size()); return DXGI_ERROR_MORE_DATA; }
        memcpy(p, it->second.data(), it->second.size()); *size = UINT(it->second.size());
        return S_OK;
    }
};

// ------------------------------------------------------------------------------------------- ShimOutput (virtual monitor)
class ShimOutput final : public IDXGIOutput6 {
public:
    explicit ShimOutput(IDXGIAdapter* adapter) : adapter_(adapter) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
        if (!pp) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIOutput) || riid == __uuidof(IDXGIOutput1) ||
            riid == __uuidof(IDXGIOutput2) || riid == __uuidof(IDXGIOutput3) || riid == __uuidof(IDXGIOutput4) || riid == __uuidof(IDXGIOutput5) ||
            riid == __uuidof(IDXGIOutput6)) { *pp = static_cast<IDXGIOutput6*>(this); AddRef(); return S_OK; }
        *pp = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG r = --ref_; if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT s, const void* p) override { return pd_.set(g, s, p); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g, const IUnknown* u) override { return pd_.setI(g, u); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT* s, void* p) override { return pd_.get(g, s, p); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** pp) override { return adapter_ ? adapter_->QueryInterface(riid, pp) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_OUTPUT_DESC* d) override {
        if (!d) return E_INVALIDARG;
        *d = {}; wcsncpy_s(d->DeviceName, kVirtualDeviceW, _TRUNCATE);
        d->DesktopCoordinates = {0, 0, LONG(std::max(1920u, gW.load())), LONG(std::max(1080u, gH.load()))};
        d->AttachedToDesktop = TRUE; d->Rotation = DXGI_MODE_ROTATION_IDENTITY; d->Monitor = kFakeMonitor;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDisplayModeList(DXGI_FORMAT f, UINT, UINT* n, DXGI_MODE_DESC* m) override {
        if (!n) return E_INVALIDARG;
        if (!m) { *n = 1; return S_OK; }
        if (*n < 1) return DXGI_ERROR_MORE_DATA;
        *n = 1; fillMode(f, m); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE FindClosestMatchingMode(const DXGI_MODE_DESC* want, DXGI_MODE_DESC* out, IUnknown*) override {
        if (!out) return E_INVALIDARG; fillMode(want ? want->Format : DXGI_FORMAT_R8G8B8A8_UNORM, out); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE WaitForVBlank() override { Sleep(1); return S_OK; }
    HRESULT STDMETHODCALLTYPE TakeOwnership(IUnknown*, BOOL) override { return DXGI_ERROR_UNSUPPORTED; }
    void STDMETHODCALLTYPE ReleaseOwnership() override {}
    HRESULT STDMETHODCALLTYPE GetGammaControlCapabilities(DXGI_GAMMA_CONTROL_CAPABILITIES*) override { return DXGI_ERROR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE SetGammaControl(const DXGI_GAMMA_CONTROL*) override { return DXGI_ERROR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetGammaControl(DXGI_GAMMA_CONTROL*) override { return DXGI_ERROR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE SetDisplaySurface(IDXGISurface*) override { return DXGI_ERROR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetDisplaySurfaceData(IDXGISurface*) override { return DXGI_ERROR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* s) override {
        if (!s) return E_INVALIDARG; *s = {}; LARGE_INTEGER q; s->SyncRefreshCount = UINT(vblankIndexNow(&q)); s->SyncQPCTime = q; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDisplayModeList1(DXGI_FORMAT f, UINT, UINT* n, DXGI_MODE_DESC1* m) override {
        if (!n) return E_INVALIDARG;
        if (!m) { *n = 1; return S_OK; }
        if (*n < 1) return DXGI_ERROR_MORE_DATA;
        *n = 1; DXGI_MODE_DESC md; fillMode(f, &md); *m = {};
        m->Width = md.Width; m->Height = md.Height; m->RefreshRate = md.RefreshRate; m->Format = md.Format; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE FindClosestMatchingMode1(const DXGI_MODE_DESC1* want, DXGI_MODE_DESC1* out, IUnknown*) override {
        if (!out) return E_INVALIDARG; DXGI_MODE_DESC md; fillMode(want ? want->Format : DXGI_FORMAT_R8G8B8A8_UNORM, &md); *out = {};
        out->Width = md.Width; out->Height = md.Height; out->RefreshRate = md.RefreshRate; out->Format = md.Format; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDisplaySurfaceData1(IDXGIResource*) override { return DXGI_ERROR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DuplicateOutput(IUnknown*, IDXGIOutputDuplication**) override { return DXGI_ERROR_UNSUPPORTED; }
    BOOL STDMETHODCALLTYPE SupportsOverlays() override { return FALSE; }
    HRESULT STDMETHODCALLTYPE CheckOverlaySupport(DXGI_FORMAT, IUnknown*, UINT* f) override { if (f) *f = 0; return DXGI_ERROR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE CheckOverlayColorSpaceSupport(DXGI_FORMAT, DXGI_COLOR_SPACE_TYPE, IUnknown*, UINT* f) override { if (f) *f = 0; return DXGI_ERROR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DuplicateOutput1(IUnknown*, UINT, UINT, const DXGI_FORMAT*, IDXGIOutputDuplication**) override { return DXGI_ERROR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_OUTPUT_DESC1* d) override {
        if (!d) return E_INVALIDARG;
        DXGI_OUTPUT_DESC d0; GetDesc(&d0); *d = {};
        memcpy(d->DeviceName, d0.DeviceName, sizeof(d->DeviceName));
        d->DesktopCoordinates = d0.DesktopCoordinates; d->AttachedToDesktop = TRUE; d->Rotation = DXGI_MODE_ROTATION_IDENTITY; d->Monitor = kFakeMonitor;
        d->BitsPerColor = 10; d->ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        d->RedPrimary[0] = 0.64f; d->RedPrimary[1] = 0.33f; d->GreenPrimary[0] = 0.30f; d->GreenPrimary[1] = 0.60f;
        d->BluePrimary[0] = 0.15f; d->BluePrimary[1] = 0.06f; d->WhitePoint[0] = 0.3127f; d->WhitePoint[1] = 0.3290f;
        d->MinLuminance = 0.1f; d->MaxLuminance = 1000.f; d->MaxFullFrameLuminance = 400.f;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CheckHardwareCompositionSupport(UINT* f) override { if (f) *f = 0; return S_OK; }
private:
    static void fillMode(DXGI_FORMAT f, DXGI_MODE_DESC* m) {
        *m = {}; m->Width = std::max(1920u, gW.load()); m->Height = std::max(1080u, gH.load());
        m->RefreshRate = {UINT(std::lround(gRefreshHz.load() * 1000.0)), 1000};
        m->Format = f == DXGI_FORMAT_UNKNOWN ? DXGI_FORMAT_R8G8B8A8_UNORM : f;
    }
    std::atomic<ULONG> ref_{1};
    ComPtr<IDXGIAdapter> adapter_;
    PrivateData pd_;
};

// ------------------------------------------------------------------------------------------- capture sink (the stage)
struct CaptureSink {
    virtual void capture(ID3D12CommandQueue* queue, ID3D12Resource* buffer) = 0;
    virtual ~CaptureSink() = default;
};
std::mutex gSinkMx;
CaptureSink* gSink = nullptr;      // the one running stage (one XeFG instance per process)
ComPtr<IDXGIAdapter> gAdapter;      // for the virtual output's GetParent

// ------------------------------------------------------------------------------------------- ShimSwapChain
class ShimSwapChain final : public IDXGISwapChain4 {
public:
    ShimSwapChain(IDXGIFactory* factory, ID3D12CommandQueue* queue, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1& desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs)
        : factory_(factory), queue_(queue), hwnd_(hwnd), desc_(desc) {
        if (fs) fsDesc_ = *fs; else { fsDesc_ = {}; fsDesc_.Windowed = TRUE; }
        fsDesc_.RefreshRate = {UINT(std::lround(gRefreshHz.load() * 1000.0)), 1000};
    }
    ~ShimSwapChain() { if (latencySem_) CloseHandle(latencySem_); }
    HRESULT init() {
        HRESULT hr = queue_->GetDevice(IID_PPV_ARGS(&device_));
        if (FAILED(hr)) return hr;
        if (desc_.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) { maxLatency_ = 1; latencySem_ = CreateSemaphoreW(nullptr, LONG(maxLatency_), 16, nullptr); }
        return createBuffers();
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
        if (!pp) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDeviceSubObject) || riid == __uuidof(IDXGISwapChain) ||
            riid == __uuidof(IDXGISwapChain1) || riid == __uuidof(IDXGISwapChain2) || riid == __uuidof(IDXGISwapChain3) || riid == __uuidof(IDXGISwapChain4)) {
            *pp = static_cast<IDXGISwapChain4*>(this); AddRef(); return S_OK;
        }
        *pp = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG r = --ref_; if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT s, const void* p) override { return pd_.set(g, s, p); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g, const IUnknown* u) override { return pd_.setI(g, u); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT* s, void* p) override { return pd_.get(g, s, p); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** pp) override { return factory_->QueryInterface(riid, pp); }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** pp) override { return queue_->QueryInterface(riid, pp); }
    HRESULT STDMETHODCALLTYPE Present(UINT sync, UINT flags) override { return present(sync, flags); }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT i, REFIID riid, void** pp) override {
        std::lock_guard l(mx_);
        if (!pp) return E_POINTER;
        if (i >= buffers_.size()) return DXGI_ERROR_INVALID_CALL;
        return buffers_[i]->QueryInterface(riid, pp);
    }
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL fs, IDXGIOutput*) override { return fs ? DXGI_ERROR_NOT_CURRENTLY_AVAILABLE : S_OK; }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* fs, IDXGIOutput** target) override { if (fs) *fs = FALSE; if (target) *target = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* d) override {
        if (!d) return E_INVALIDARG;
        std::lock_guard l(mx_);
        *d = {};
        d->BufferDesc.Width = desc_.Width; d->BufferDesc.Height = desc_.Height; d->BufferDesc.RefreshRate = fsDesc_.RefreshRate; d->BufferDesc.Format = desc_.Format;
        d->SampleDesc = desc_.SampleDesc; d->BufferUsage = desc_.BufferUsage; d->BufferCount = desc_.BufferCount; d->OutputWindow = hwnd_; d->Windowed = TRUE;
        d->SwapEffect = desc_.SwapEffect; d->Flags = desc_.Flags;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count, UINT w, UINT h, DXGI_FORMAT f, UINT flags) override { return resize(count, w, h, f, flags); }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** out) override { if (!out) return E_INVALIDARG; *out = new ShimOutput(gAdapter.Get()); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* s) override {
        if (!s) return E_INVALIDARG;
        std::lock_guard l(mx_);
        *s = {}; LARGE_INTEGER q; const uint64_t idx = vblankIndexNow(&q);
        s->PresentCount = presentCount_; s->PresentRefreshCount = UINT(lastPresentVblank_); s->SyncRefreshCount = UINT(idx); s->SyncQPCTime = q;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* c) override { if (!c) return E_INVALIDARG; *c = presentCount_; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* d) override { if (!d) return E_INVALIDARG; std::lock_guard l(mx_); *d = desc_; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* d) override { if (!d) return E_INVALIDARG; *d = fsDesc_; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* h) override { if (!h) return E_INVALIDARG; *h = hwnd_; return hwnd_ ? S_OK : DXGI_ERROR_INVALID_CALL; }
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID, void** pp) override { if (pp) *pp = nullptr; return DXGI_ERROR_INVALID_CALL; }
    HRESULT STDMETHODCALLTYPE Present1(UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS*) override { return present(sync, flags); }
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override { return FALSE; }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** o) override { if (o) *o = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* c) override { if (c) bg_ = *c; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* c) override { if (c) *c = bg_; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION r) override { return r == DXGI_MODE_ROTATION_IDENTITY ? S_OK : DXGI_ERROR_INVALID_CALL; }
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* r) override { if (r) *r = DXGI_MODE_ROTATION_IDENTITY; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT w, UINT h) override { if (w > desc_.Width || h > desc_.Height || !w || !h) return DXGI_ERROR_INVALID_CALL; srcW_ = w; srcH_ = h; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* w, UINT* h) override { if (w) *w = srcW_ ? srcW_ : desc_.Width; if (h) *h = srcH_ ? srcH_ : desc_.Height; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT n) override {
        if (!n || n > 16) return DXGI_ERROR_INVALID_CALL;
        if (latencySem_ && n > maxLatency_) ReleaseSemaphore(latencySem_, LONG(n - maxLatency_), nullptr);
        maxLatency_ = n; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* n) override { if (n) *n = maxLatency_; return S_OK; }
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override {
        if (!latencySem_) return nullptr;
        HANDLE dup = nullptr; DuplicateHandle(GetCurrentProcess(), latencySem_, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS); return dup;
    }
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F*) override { return DXGI_ERROR_INVALID_CALL; }
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F*) override { return DXGI_ERROR_INVALID_CALL; }
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override { return current_; }
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE cs, UINT* s) override {
        if (!s) return E_INVALIDARG;
        *s = (cs == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 || cs == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 || cs == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709)
           ? DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT : 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE cs) override { colorSpace_ = cs; return S_OK; }
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT count, UINT w, UINT h, DXGI_FORMAT f, UINT flags, const UINT*, IUnknown* const*) override { return resize(count, w, h, f, flags); }
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE, UINT, void*) override { return S_OK; }
private:
    HRESULT createBuffers() {
        buffers_.clear();
        for (UINT i = 0; i < desc_.BufferCount; ++i) {
            D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = desc_.Width; rd.Height = desc_.Height; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
            rd.Format = desc_.Format; rd.SampleDesc = {1, 0}; rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            if (desc_.BufferUsage & DXGI_USAGE_UNORDERED_ACCESS) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            ComPtr<ID3D12Resource> r;
            const HRESULT hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r));
            if (FAILED(hr)) return hr;
            r->SetName(L"pushmpc-xefg-shim-bb");
            buffers_.push_back(r);
        }
        current_ = 0;
        return S_OK;
    }
    HRESULT resize(UINT count, UINT w, UINT h, DXGI_FORMAT f, UINT flags) {
        std::lock_guard l(mx_);
        for (auto& b : buffers_) { b->AddRef(); if (b->Release() > 1) return DXGI_ERROR_INVALID_CALL; }
        if (count) desc_.BufferCount = count;
        if (w) desc_.Width = w;
        if (h) desc_.Height = h;
        if (f != DXGI_FORMAT_UNKNOWN) desc_.Format = f;
        desc_.Flags = flags;
        return createBuffers();
    }
    HRESULT present(UINT, UINT flags) {
        if (flags & DXGI_PRESENT_TEST) return S_OK;
        ID3D12Resource* buffer = nullptr;
        {
            std::lock_guard l(mx_);
            buffer = buffers_.empty() ? nullptr : buffers_[current_].Get();
            if (buffer) buffer->AddRef();
            ++presentCount_;
            lastPresentVblank_ = vblankIndexNow();
            current_ = desc_.BufferCount ? (current_ + 1) % desc_.BufferCount : 0;
            if (latencySem_) ReleaseSemaphore(latencySem_, 1, nullptr);
        }
        if (buffer) {
            std::lock_guard l(gSinkMx);
            if (gSink) gSink->capture(queue_.Get(), buffer);
            buffer->Release();
        }
        return S_OK;
    }
    std::atomic<ULONG> ref_{1};
    ComPtr<IDXGIFactory> factory_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12Device> device_;
    HWND hwnd_;
    DXGI_SWAP_CHAIN_DESC1 desc_;
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsDesc_{};
    std::vector<ComPtr<ID3D12Resource>> buffers_;
    UINT current_ = 0, presentCount_ = 0;
    uint64_t lastPresentVblank_ = 0;
    HANDLE latencySem_ = nullptr;
    UINT maxLatency_ = 3, srcW_ = 0, srcH_ = 0;
    DXGI_RGBA bg_{};
    DXGI_COLOR_SPACE_TYPE colorSpace_ = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    std::mutex mx_;
    PrivateData pd_;
};

// ------------------------------------------------------------------------------------------- ShimFactory
class ShimFactory final : public IDXGIFactory7 {
public:
    explicit ShimFactory(IDXGIFactory7* real) : real_(real) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
        if (!pp) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIFactory) || riid == __uuidof(IDXGIFactory1) ||
            riid == __uuidof(IDXGIFactory2) || riid == __uuidof(IDXGIFactory3) || riid == __uuidof(IDXGIFactory4) || riid == __uuidof(IDXGIFactory5) ||
            riid == __uuidof(IDXGIFactory6) || riid == __uuidof(IDXGIFactory7)) { *pp = static_cast<IDXGIFactory7*>(this); AddRef(); return S_OK; }
        *pp = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG r = --ref_; if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT s, const void* p) override { return real_->SetPrivateData(g, s, p); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g, const IUnknown* u) override { return real_->SetPrivateDataInterface(g, u); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT* s, void* p) override { return real_->GetPrivateData(g, s, p); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** pp) override { return real_->GetParent(riid, pp); }
    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT i, IDXGIAdapter** a) override { return real_->EnumAdapters(i, a); }
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND h, UINT) override { assoc_ = h; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND* h) override { if (!h) return E_INVALIDARG; *h = assoc_; return S_OK; }
    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** out) override {
        if (!d || !out) return E_INVALIDARG;
        DXGI_SWAP_CHAIN_DESC1 d1{};
        d1.Width = d->BufferDesc.Width; d1.Height = d->BufferDesc.Height; d1.Format = d->BufferDesc.Format; d1.SampleDesc = d->SampleDesc;
        d1.BufferUsage = d->BufferUsage; d1.BufferCount = d->BufferCount; d1.SwapEffect = d->SwapEffect; d1.Flags = d->Flags;
        d1.Scaling = DXGI_SCALING_STRETCH; d1.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        IDXGISwapChain1* sc = nullptr;
        const HRESULT hr = create(dev, d->OutputWindow, &d1, nullptr, &sc);
        *out = sc; return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE m, IDXGIAdapter** a) override { return real_->CreateSoftwareAdapter(m, a); }
    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT i, IDXGIAdapter1** a) override { return real_->EnumAdapters1(i, a); }
    BOOL STDMETHODCALLTYPE IsCurrent() override { return real_->IsCurrent(); }
    BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override { return FALSE; }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown* dev, HWND h, const DXGI_SWAP_CHAIN_DESC1* d, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput*, IDXGISwapChain1** out) override {
        return create(dev, h, d, fs, out);
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1** out) override { if (out) *out = nullptr; return DXGI_ERROR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE h, LUID* l) override { return real_->GetSharedResourceAdapterLuid(h, l); }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND, UINT, DWORD* c) override { if (c) *c = 1; return S_OK; }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE, DWORD* c) override { if (c) *c = 1; return S_OK; }
    void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD) override {}
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND, UINT, DWORD* c) override { if (c) *c = 1; return S_OK; }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE, DWORD* c) override { if (c) *c = 1; return S_OK; }
    void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD) override {}
    HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown* dev, const DXGI_SWAP_CHAIN_DESC1* d, IDXGIOutput*, IDXGISwapChain1** out) override { return create(dev, nullptr, d, nullptr, out); }
    UINT STDMETHODCALLTYPE GetCreationFlags() override { return real_->GetCreationFlags(); }
    HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID l, REFIID riid, void** pp) override { return real_->EnumAdapterByLuid(l, riid, pp); }
    HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID riid, void** pp) override { return real_->EnumWarpAdapter(riid, pp); }
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(DXGI_FEATURE f, void* p, UINT s) override {
        HRESULT hr = real_->CheckFeatureSupport(f, p, s);
        if (f == DXGI_FEATURE_PRESENT_ALLOW_TEARING && p && s >= sizeof(BOOL)) { *static_cast<BOOL*>(p) = TRUE; hr = S_OK; }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE EnumAdapterByGpuPreference(UINT i, DXGI_GPU_PREFERENCE g, REFIID riid, void** pp) override { return real_->EnumAdapterByGpuPreference(i, g, riid, pp); }
    HRESULT STDMETHODCALLTYPE RegisterAdaptersChangedEvent(HANDLE h, DWORD* c) override { return real_->RegisterAdaptersChangedEvent(h, c); }
    HRESULT STDMETHODCALLTYPE UnregisterAdaptersChangedEvent(DWORD c) override { return real_->UnregisterAdaptersChangedEvent(c); }
private:
    HRESULT create(IUnknown* dev, HWND h, const DXGI_SWAP_CHAIN_DESC1* d, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGISwapChain1** out) {
        if (!d || !out || !dev) return E_INVALIDARG;
        *out = nullptr;
        ComPtr<ID3D12CommandQueue> q;
        if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&q)))) return DXGI_ERROR_INVALID_CALL;
        info(std::format("shim swap chain {}x{} format {} buffers {} flags 0x{:X} on queue type {}", d->Width, d->Height, int(d->Format), d->BufferCount, d->Flags, int(q->GetDesc().Type)));
        auto* sc = new ShimSwapChain(this, q.Get(), h, *d, fs);
        const HRESULT hr = sc->init();
        if (FAILED(hr)) { sc->Release(); return hr; }
        *out = sc;
        return S_OK;
    }
    std::atomic<ULONG> ref_{1};
    ComPtr<IDXGIFactory7> real_;
    HWND assoc_ = nullptr;
};
ComPtr<ShimFactory> gShimFactory;

// ------------------------------------------------------------------------------------------- import hooks (libxess_fg.dll)
int hookIat(HMODULE mod, const char* func, void* repl, void** orig) {
    auto* base = reinterpret_cast<uint8_t*>(mod);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
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
decltype(&CreateDXGIFactory1) o_CreateDXGIFactory1 = nullptr;
HRESULT WINAPI h_CreateDXGIFactory1(REFIID riid, void** pp) { return gShimFactory ? gShimFactory->QueryInterface(riid, pp) : o_CreateDXGIFactory1(riid, pp); }
HRESULT WINAPI h_CreateDXGIFactory2(UINT, REFIID riid, void** pp) { return h_CreateDXGIFactory1(riid, pp); }
decltype(&GetClientRect) o_GetClientRect = nullptr;
BOOL WINAPI h_GetClientRect(HWND h, LPRECT r) {
    if (h == kFakeHwnd && r) { *r = {0, 0, LONG(gW.load()), LONG(gH.load())}; return TRUE; }
    return o_GetClientRect(h, r);
}
decltype(&MonitorFromWindow) o_MonitorFromWindow = nullptr;
HMONITOR WINAPI h_MonitorFromWindow(HWND h, DWORD f) { return h == kFakeHwnd ? kFakeMonitor : o_MonitorFromWindow(h, f); }
decltype(&GetMonitorInfoA) o_GetMonitorInfoA = nullptr;
BOOL WINAPI h_GetMonitorInfoA(HMONITOR m, LPMONITORINFO mi) {
    if (m == kFakeMonitor && mi) {
        const RECT rc{0, 0, LONG(std::max(1920u, gW.load())), LONG(std::max(1080u, gH.load()))};
        mi->rcMonitor = rc; mi->rcWork = rc; mi->dwFlags = MONITORINFOF_PRIMARY;
        if (mi->cbSize >= sizeof(MONITORINFOEXA)) strcpy_s(reinterpret_cast<MONITORINFOEXA*>(mi)->szDevice, kVirtualDeviceA);
        return TRUE;
    }
    return o_GetMonitorInfoA(m, mi);
}
decltype(&EnumDisplaySettingsA) o_EnumDisplaySettingsA = nullptr;
BOOL WINAPI h_EnumDisplaySettingsA(LPCSTR dev, DWORD mode, DEVMODEA* dm) {
    if (dm && (!dev || strcmp(dev, kVirtualDeviceA) == 0)) {
        const WORD size = dm->dmSize, extra = dm->dmDriverExtra;
        memset(dm, 0, size); dm->dmSize = size; dm->dmDriverExtra = extra;
        dm->dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
        dm->dmBitsPerPel = 32; dm->dmPelsWidth = std::max(1920u, gW.load()); dm->dmPelsHeight = std::max(1080u, gH.load());
        dm->dmDisplayFrequency = DWORD(std::lround(gRefreshHz.load()));
        return TRUE;
    }
    return o_EnumDisplaySettingsA(dev, mode, dm);
}
decltype(&GetProcAddress) o_GetProcAddress = nullptr;
FARPROC WINAPI h_GetProcAddress(HMODULE m, LPCSTR name) {
    if (reinterpret_cast<uintptr_t>(name) >= 0x10000) {
        if (strcmp(name, "CreateDXGIFactory2") == 0) return reinterpret_cast<FARPROC>(&h_CreateDXGIFactory2);
        if (strcmp(name, "CreateDXGIFactory1") == 0 || strcmp(name, "CreateDXGIFactory") == 0) return reinterpret_cast<FARPROC>(&h_CreateDXGIFactory1);
    }
    return o_GetProcAddress(m, name);
}

// ------------------------------------------------------------------------------------------- MFG unlock (identity-gated)
// The same byte tables and identity gate as PushFPS's XessMfgUnlock (OptiScaler XeFGUnlock.h, commit 70676c5f), without
// its 3-interpolation ceiling: the spike ran up to 13 interpolated frames through these patches. Only the mapped image
// of the audited libxess_fg.dll 1.3.1.78 is patched, never the file.
namespace unlock {
constexpr uint64_t kSize = 22957432ull;
constexpr const char* kSha = "EC5E0C65E075570C6EDE72618BB666D0BE0C2E10B2EA9762C0FE8CB8E375AB27";
constexpr uint32_t kTimeDateStamp = 0x69CB0F4Du, kSizeOfImage = 0x015ED000u;
struct Patch { const char* name; uint32_t rva; std::vector<uint8_t> expected, replacement; int immOffset; };
const Patch kPatches[] = {
    {"U1/frame-count-fallback", 0x20DA4F, {0x0F, 0x85, 0xCC, 0x00, 0x00, 0x00}, {0xE9, 0xCD, 0x00, 0x00, 0x00, 0x90}, -1},
    {"U2/model-downgrade", 0x1A5DE4, {0x74, 0x09}, {0xEB, 0x06}, -1},
    {"U3/default-ceiling", 0x1A517D, {0xBB, 0x03, 0x00, 0x00, 0x00}, {0xBB, 0x00, 0x00, 0x00, 0x00}, 1},
    {"U4/override-clamp", 0x1A45C2, {0xC7, 0x87, 0x6C, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00}, {0xC7, 0x87, 0x6C, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 6},
    {"U5/reported-maximum", 0x20973B, {0xB8, 0x01, 0x00, 0x00, 0x00}, {0xB8, 0x00, 0x00, 0x00, 0x00}, 1},
};
struct Applied { uint8_t* at; std::vector<uint8_t> original; };
std::vector<Applied> gApplied;
uint32_t gAppliedFor = 0;
bool write(uint8_t* at, const std::vector<uint8_t>& bytes) {
    DWORD old;
    if (!VirtualProtect(at, bytes.size(), PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(at, bytes.data(), bytes.size());
    FlushInstructionCache(GetCurrentProcess(), at, bytes.size());
    VirtualProtect(at, bytes.size(), old, &old);
    return memcmp(at, bytes.data(), bytes.size()) == 0;
}
void release() {
    for (auto it = gApplied.rbegin(); it != gApplied.rend(); ++it) write(it->at, it->original);
    gApplied.clear(); gAppliedFor = 0;
}
bool apply(HMODULE fg, const FileCheck& file, uint32_t generated, std::string& detail) {
    if (gAppliedFor == generated && !gApplied.empty()) { detail = "already applied"; return true; }
    release();
    if (file.size != kSize || file.sha256 != kSha) { detail = std::format("unaudited libxess_fg.dll (size {} sha256 {})", file.size, file.sha256); return false; }
    auto* base = reinterpret_cast<uint8_t*>(fg);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != kTimeDateStamp || nt->OptionalHeader.SizeOfImage != kSizeOfImage) { detail = "PE identity mismatch"; return false; }
    for (const auto& p : kPatches) if (memcmp(base + p.rva, p.expected.data(), p.expected.size()) != 0) { detail = std::string(p.name) + " has unexpected bytes"; return false; }
    for (const auto& p : kPatches) {
        std::vector<uint8_t> bytes = p.replacement;
        if (p.immOffset >= 0) memcpy(bytes.data() + p.immOffset, &generated, 4);
        Applied a{base + p.rva, p.expected};
        if (!write(a.at, bytes)) { detail = std::string(p.name) + " write failed"; release(); return false; }
        gApplied.push_back(a);
    }
    gAppliedFor = generated;
    detail = std::format("applied {} patches, ceiling {}X", gApplied.size(), generated + 1);
    return true;
}
}  // namespace unlock

// ------------------------------------------------------------------------------------------- the runtime (once per process)
struct Api {
#define API(name) decltype(&name) name = nullptr
    API(xefgSwapChainGetVersion); API(xefgSwapChainD3D12CreateContext); API(xefgSwapChainSetLoggingCallback);
    API(xefgSwapChainD3D12GetProperties); API(xefgSwapChainGetProperties); API(xefgSwapChainSetNumInterpolatedFrames);
    API(xefgSwapChainD3D12InitFromSwapChain); API(xefgSwapChainD3D12GetSwapChainPtr); API(xefgSwapChainD3D12TagFrameResource);
    API(xefgSwapChainTagFrameConstants); API(xefgSwapChainSetEnabled); API(xefgSwapChainSetPresentId);
    API(xefgSwapChainGetLastPresentStatus); API(xefgSwapChainSetLatencyReduction); API(xefgSwapChainDestroy);
    API(xellD3D12CreateContext); API(xellDestroyContext); API(xellSetSleepMode); API(xellSleep); API(xellAddMarkerData); API(xellGetVersion);
#undef API
    HMODULE fg = nullptr, ll = nullptr;
    FileCheck fgFile, llFile;
    std::string version;
};
std::mutex gApiMx;
Api gApi;
bool gApiLoaded = false;
std::string gApiError;
void xefgLogCallback(const char* msg, xefg_swapchain_logging_level_t level, void*) {
    if (!msg) return;
    if (level >= XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING) warn(std::string("XeFG: ") + msg); else if (level >= XEFG_SWAPCHAIN_LOGGING_LEVEL_INFO) info(std::string("XeFG: ") + msg);
}
bool loadApi(std::string& why) {
    std::lock_guard l(gApiMx);
    if (gApiLoaded) return true;
    if (!gApiError.empty()) { why = gApiError; return false; }
    QueryPerformanceFrequency(&gQpf); QueryPerformanceCounter(&gQp0);
    const auto dir = runtimeFolder();
    Api a;
    a.llFile = checkFile(dir / L"libxell.dll");
    a.fgFile = checkFile(dir / L"libxess_fg.dll");
    for (const FileCheck* f : {&a.llFile, &a.fgFile})
        if (!f->present || !f->signedByIntel) { why = gApiError = f->why; return false; }
    a.ll = LoadLibraryExW((dir / L"libxell.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    a.fg = a.ll ? LoadLibraryExW((dir / L"libxess_fg.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32) : nullptr;
    if (!a.ll || !a.fg) { why = gApiError = std::format("libxess_fg.dll / libxell.dll did not load (error {})", GetLastError()); return false; }
#define LOADP(mod, name) a.name = reinterpret_cast<decltype(a.name)>(GetProcAddress(a.mod, #name)); if (!a.name) { why = gApiError = "XeFG runtime lacks " #name; return false; }
    LOADP(fg, xefgSwapChainGetVersion) LOADP(fg, xefgSwapChainD3D12CreateContext) LOADP(fg, xefgSwapChainSetLoggingCallback)
    LOADP(fg, xefgSwapChainD3D12GetProperties) LOADP(fg, xefgSwapChainGetProperties) LOADP(fg, xefgSwapChainSetNumInterpolatedFrames)
    LOADP(fg, xefgSwapChainD3D12InitFromSwapChain) LOADP(fg, xefgSwapChainD3D12GetSwapChainPtr) LOADP(fg, xefgSwapChainD3D12TagFrameResource)
    LOADP(fg, xefgSwapChainTagFrameConstants) LOADP(fg, xefgSwapChainSetEnabled) LOADP(fg, xefgSwapChainSetPresentId)
    LOADP(fg, xefgSwapChainGetLastPresentStatus) LOADP(fg, xefgSwapChainSetLatencyReduction) LOADP(fg, xefgSwapChainDestroy)
    LOADP(ll, xellD3D12CreateContext) LOADP(ll, xellDestroyContext) LOADP(ll, xellSetSleepMode) LOADP(ll, xellSleep) LOADP(ll, xellAddMarkerData)
#undef LOADP
    a.xellGetVersion = reinterpret_cast<decltype(a.xellGetVersion)>(GetProcAddress(a.ll, "xellGetVersion"));
    xefg_swapchain_version_t fv{}; a.xefgSwapChainGetVersion(&fv);
    xell_version_t lv{}; if (a.xellGetVersion) a.xellGetVersion(&lv);
    a.version = std::format("XeFG {}.{}.{} ({}{}), XeLL {}.{}.{}", fv.major, fv.minor, fv.patch, a.fgFile.version, a.fgFile.pinned ? ", pinned build" : ", Intel-signed, not the pinned build",
                            lv.major, lv.minor, lv.patch);
    // the shim factory and the import hooks, before any XeFG context exists
    ComPtr<IDXGIFactory7> real;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&real)))) { why = gApiError = "no DXGI factory"; return false; }
    gShimFactory.Attach(new ShimFactory(real.Get()));
    struct H { const char* name; void* repl; void** orig; };
    const H hooks[] = {
        {"CreateDXGIFactory1", reinterpret_cast<void*>(&h_CreateDXGIFactory1), reinterpret_cast<void**>(&o_CreateDXGIFactory1)},
        {"GetClientRect", reinterpret_cast<void*>(&h_GetClientRect), reinterpret_cast<void**>(&o_GetClientRect)},
        {"MonitorFromWindow", reinterpret_cast<void*>(&h_MonitorFromWindow), reinterpret_cast<void**>(&o_MonitorFromWindow)},
        {"GetMonitorInfoA", reinterpret_cast<void*>(&h_GetMonitorInfoA), reinterpret_cast<void**>(&o_GetMonitorInfoA)},
        {"EnumDisplaySettingsA", reinterpret_cast<void*>(&h_EnumDisplaySettingsA), reinterpret_cast<void**>(&o_EnumDisplaySettingsA)},
        {"GetProcAddress", reinterpret_cast<void*>(&h_GetProcAddress), reinterpret_cast<void**>(&o_GetProcAddress)},
    };
    std::string hooked;
    for (const auto& h : hooks) hooked += std::format(" {}={}", h.name, hookIat(a.fg, h.name, h.repl, h.orig));
    if (!o_CreateDXGIFactory1) o_CreateDXGIFactory1 = &CreateDXGIFactory1;
    if (!o_GetClientRect) o_GetClientRect = &GetClientRect;
    if (!o_MonitorFromWindow) o_MonitorFromWindow = &MonitorFromWindow;
    if (!o_GetMonitorInfoA) o_GetMonitorInfoA = &GetMonitorInfoA;
    if (!o_EnumDisplaySettingsA) o_EnumDisplaySettingsA = &EnumDisplaySettingsA;
    if (!o_GetProcAddress) o_GetProcAddress = &GetProcAddress;
    info("runtime " + a.version + "; hooks" + hooked);
    gApi = a;
    gApiLoaded = true;
    return true;
}

}  // namespace

// ============================================================================================ XefgStage
struct XefgStage::Impl final : CaptureSink {
    XefgConfig cfg;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    xell_context_handle_t ll = nullptr;
    xefg_swapchain_handle_t fg = nullptr;
    ComPtr<IDXGISwapChain4> proxy;
    bool unlocked = false;
    uint32_t generated = 1;
    // own inputs: constant depth, zero motion
    ComPtr<ID3D12Resource> depth, zeroMotion;
    // the submit lists
    static constexpr uint32_t kLists = 4;
    ComPtr<ID3D12CommandAllocator> alloc[kLists];
    uint64_t allocValue[kLists]{};
    ComPtr<ID3D12GraphicsCommandList> list;
    uint32_t listNext = 0;
    ComPtr<ID3D12Fence> fence;             // submit lists and capture copies (any queue of the device)
    std::atomic<uint64_t> fenceValue{0};
    HANDLE event = nullptr;
    uint32_t frameId = 0;
    // outputs
    struct Slot { ComPtr<ID3D12Resource> res; ComPtr<ID3D11Texture2D> tex; bool busy = false; ComPtr<ID3D12Fence> releaseFence; uint64_t releaseValue = 0; };
    std::vector<Slot> slots;
    uint32_t slotNext = 0;
    struct CapAlloc { ComPtr<ID3D12CommandAllocator> alloc; ComPtr<ID3D12GraphicsCommandList> list; uint64_t value = 0; };
    std::vector<CapAlloc> capAllocs;
    uint32_t capNext = 0;
    struct Raw { int slot = -1; uint64_t value = 0; };
    std::deque<Raw> raw;                    // captures not yet matched, in present order
    struct Group { uint32_t n = 0, taken = 0; int64_t pts = 0, prevPts = INT64_MIN; uint64_t source = 0; };
    std::deque<Group> groups;               // per submitted real frame: how many presents it makes
    uint64_t discard = 0;                   // captures still to come for frames dropped by flush
    int64_t lastPts = INT64_MIN;
    mutable std::mutex mx;                  // raw, groups, slots, counters (capture runs on XeFG's threads)
    Counters c;

    ~Impl() override { shutdown(); }
    void shutdown() {
        { std::lock_guard l(gSinkMx); if (gSink == this) gSink = nullptr; }
        idle();
        proxy.Reset();
        if (fg) { gApi.xefgSwapChainDestroy(fg); fg = nullptr; }
        if (ll) { gApi.xellDestroyContext(ll); ll = nullptr; }
        if (unlocked) { unlock::release(); unlocked = false; }
        idle();
        slots.clear(); capAllocs.clear(); raw.clear(); groups.clear();
        list.Reset(); for (auto& a : alloc) a.Reset();
        depth.Reset(); zeroMotion.Reset(); fence.Reset();
        if (event) { CloseHandle(event); event = nullptr; }
        queue.Reset(); device.Reset();
    }
    void waitValue(uint64_t v, DWORD ms = 10000) {
        if (!fence || fence->GetCompletedValue() >= v) return;
        HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        fence->SetEventOnCompletion(v, e);
        WaitForSingleObject(e, ms);
        CloseHandle(e);
    }
    void idle() {
        if (!queue || !fence) return;
        const uint64_t v = ++fenceValue;
        queue->Signal(fence.Get(), v);
        waitValue(v);
    }
    static void barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        if (a == b) return;
        D3D12_RESOURCE_BARRIER br{}; br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        br.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
        l->ResourceBarrier(1, &br);
    }
    bool makeTexture(DXGI_FORMAT f, D3D12_RESOURCE_FLAGS flags, D3D12_HEAP_FLAGS heapFlags, ComPtr<ID3D12Resource>& r) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = cfg.width; rd.Height = cfg.height;
        rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = f; rd.SampleDesc = {1, 0}; rd.Flags = flags;
        return SUCCEEDED(device->CreateCommittedResource(&hp, heapFlags, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r)));
    }
    // fills a texture (R32_FLOAT / R16G16_FLOAT) with one 32-bit value through an upload buffer
    bool fill(ID3D12Resource* t, uint32_t value) {
        const D3D12_RESOURCE_DESC rd = t->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT64 total = 0;
        device->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, &total);
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc = {1, 0}; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> up;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&up)))) return false;
        uint8_t* m = nullptr;
        if (FAILED(up->Map(0, nullptr, reinterpret_cast<void**>(&m)))) return false;
        for (uint32_t y = 0; y < fp.Footprint.Height; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(m + fp.Offset + size_t(y) * fp.Footprint.RowPitch);
            for (uint32_t x = 0; x < fp.Footprint.Width; ++x) row[x] = value;
        }
        up->Unmap(0, nullptr);
        auto& a = alloc[0];
        a->Reset(); list->Reset(a.Get(), nullptr);
        barrier(list.Get(), t, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION d{t, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}; d.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION s{up.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}; s.PlacedFootprint = fp;
        list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        barrier(list.Get(), t, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        list->Close();
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        idle();
        return true;
    }

    bool init(const XefgConfig& config, std::string& why) {
        cfg = config;
        if (!cfg.device || !cfg.queue || !cfg.width || !cfg.height || cfg.multiplier < 2) { why = "bad XeFG configuration"; return false; }
        if (!loadApi(why)) return false;
        {
            std::lock_guard l(gSinkMx);
            if (gSink) { why = "another XeFG session runs in this process"; return false; }
        }
        device = cfg.device; queue = cfg.queue;
        generated = cfg.multiplier - 1;
        gW = cfg.width; gH = cfg.height;
        {
            wchar_t v[16]{};
            if (GetEnvironmentVariableW(L"PUSHMPC_XEFG_REFRESH", v, 16)) { const double hz = _wtof(v); if (hz >= 24 && hz <= 1000) gRefreshHz = hz; }
        }
        {   // the virtual output's adapter
            ComPtr<IDXGIFactory4> f; const LUID luid = device->GetAdapterLuid();
            if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&f)))) { gAdapter.Reset(); f->EnumAdapterByLuid(luid, IID_PPV_ARGS(&gAdapter)); }
        }
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) { why = "no fence"; return false; }
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        for (auto& a : alloc) if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) { why = "no command allocator"; return false; }
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0].Get(), nullptr, IID_PPV_ARGS(&list)))) { why = "no command list"; return false; }
        list->Close();
        if (!makeTexture(DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_HEAP_FLAG_NONE, depth) ||
            !makeTexture(DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_HEAP_FLAG_NONE, zeroMotion)) { why = "no depth / motion texture"; return false; }
        float half = 0.5f; uint32_t halfBits; memcpy(&halfBits, &half, 4);
        if (!fill(depth.Get(), halfBits) || !fill(zeroMotion.Get(), 0)) { why = "depth / motion upload failed"; return false; }
        // output slots: shared with D3D11 when the caller has a device
        const uint32_t count = cfg.outSlots ? cfg.outSlots : 3 * cfg.multiplier + 2;
        slots.resize(count);
        for (auto& s : slots) {
            const D3D12_HEAP_FLAGS hf = cfg.d3d11 ? D3D12_HEAP_FLAG_SHARED : D3D12_HEAP_FLAG_NONE;
            if (!makeTexture(cfg.format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, hf, s.res)) { why = "no output texture"; return false; }
            s.res->SetName(L"pushmpc-xefg-out");
            if (cfg.d3d11) {
                HANDLE h = nullptr;
                if (FAILED(device->CreateSharedHandle(s.res.Get(), nullptr, GENERIC_ALL, nullptr, &h))) { why = "output share handle"; return false; }
                const HRESULT hr = cfg.d3d11->OpenSharedResource1(h, IID_PPV_ARGS(&s.tex));
                CloseHandle(h);
                if (FAILED(hr)) { why = std::format("D3D11 OpenSharedResource1(XeFG output) hr=0x{:08X}", unsigned(hr)); return false; }
            }
        }
        capAllocs.resize(2 * count);
        for (auto& ca : capAllocs) {
            if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca.alloc))) ||
                FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca.alloc.Get(), nullptr, IID_PPV_ARGS(&ca.list)))) { why = "no capture list"; return false; }
            ca.list->Close();
        }
        auto r = [&](const char* what, int result) {
            if (result < 0) why = std::format("{} failed ({})", what, result);
            else if (result > 0) info(std::format("{} returned {}", what, result));
            return result >= 0;
        };
        if (!r("xellD3D12CreateContext", gApi.xellD3D12CreateContext(device.Get(), &ll))) return false;
        if (!r("xefgSwapChainD3D12CreateContext", gApi.xefgSwapChainD3D12CreateContext(device.Get(), &fg))) return false;
        gApi.xefgSwapChainSetLoggingCallback(fg, XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING, xefgLogCallback, nullptr);
        if (!r("xefgSwapChainSetLatencyReduction", gApi.xefgSwapChainSetLatencyReduction(fg, ll))) return false;
        {
            // XeLL low latency on, as PushFPS's video presenter runs it (XeFG needs XeLL); PUSHMPC_XEFG_LOW_LATENCY=0 = pass-through
            wchar_t v[4]{};
            xell_sleep_params_t sp{}; sp.bLowLatencyMode = !(GetEnvironmentVariableW(L"PUSHMPC_XEFG_LOW_LATENCY", v, 4) && v[0] == L'0');
            if (!r("xellSetSleepMode", gApi.xellSetSleepMode(ll, &sp))) return false;
        }
        uint32_t maxInterp = 1;
        { xefg_swapchain_properties_t p{}; if (gApi.xefgSwapChainGetProperties(fg, &p) >= 0) maxInterp = std::max(1u, p.maxSupportedInterpolations); }
        std::string unlockNote = std::format("runtime allows {} generated per real frame", maxInterp);
        if (generated > maxInterp) {
            // MFG beyond what the runtime offers here: the identity-gated unlock (switch off: PUSHMPC_DISABLE_XEFG=1 turns XeFG off
            // altogether; PUSHMPC_XEFG_NO_MFG_UNLOCK=1 keeps the stock ceiling)
            if (pmu::EnvTrue(L"PUSHMPC_XEFG_NO_MFG_UNLOCK")) { why = std::format("{}X asked, the runtime offers {}X (MFG unlock switched off)", cfg.multiplier, maxInterp + 1); return false; }
            // the unlock patches the loaded image: it must be applied before the first context exists. Contexts are recreated.
            gApi.xefgSwapChainDestroy(fg); fg = nullptr;
            std::string detail;
            unlocked = unlock::apply(gApi.fg, gApi.fgFile, generated, detail);
            unlockNote += "; MFG unlock: " + detail;
            if (!unlocked) { why = std::format("{}X needs the MFG unlock: {}", cfg.multiplier, detail); return false; }
            if (!r("xefgSwapChainD3D12CreateContext (unlocked)", gApi.xefgSwapChainD3D12CreateContext(device.Get(), &fg))) return false;
            gApi.xefgSwapChainSetLoggingCallback(fg, XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING, xefgLogCallback, nullptr);
            if (!r("xefgSwapChainSetLatencyReduction", gApi.xefgSwapChainSetLatencyReduction(fg, ll))) return false;
            xefg_swapchain_properties_t p{};
            if (gApi.xefgSwapChainGetProperties(fg, &p) >= 0) maxInterp = std::max(1u, p.maxSupportedInterpolations);
            if (generated > maxInterp) { why = std::format("{}X asked, the unlocked runtime offers {}X", cfg.multiplier, maxInterp + 1); return false; }
        }
        DXGI_SWAP_CHAIN_DESC1 scd{};
        scd.Width = cfg.width; scd.Height = cfg.height; scd.Format = cfg.format; scd.SampleDesc = {1, 0}; scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount = 3; scd.Scaling = DXGI_SCALING_STRETCH; scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; scd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        scd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        xefg_swapchain_d3d12_init_params_t ip{};
        ip.maxInterpolatedFrames = generated;
        ip.uiMode = XEFG_SWAPCHAIN_UI_MODE_AUTO;
        {
            xefg_swapchain_properties_t p{};
            if (!r("xefgSwapChainD3D12GetProperties", gApi.xefgSwapChainD3D12GetProperties(fg, &ip, scd.Width, scd.Height, scd.Format, &p))) return false;
        }
        {
            std::lock_guard l(gSinkMx);
            gSink = this;   // captures route here from now on
        }
        ComPtr<IDXGISwapChain1> appChain;
        if (FAILED(gShimFactory->CreateSwapChainForHwnd(queue.Get(), kFakeHwnd, &scd, nullptr, nullptr, &appChain))) { why = "shim swap chain"; return false; }
        ip.pApplicationSwapChain = appChain.Detach();   // XeFG takes the only reference
        const auto t0 = Clock::now();
        if (!r("xefgSwapChainD3D12InitFromSwapChain", gApi.xefgSwapChainD3D12InitFromSwapChain(fg, queue.Get(), &ip))) return false;
        const double initMs = msSince(t0);
        if (!r("xefgSwapChainD3D12GetSwapChainPtr", gApi.xefgSwapChainD3D12GetSwapChainPtr(fg, IID_PPV_ARGS(&proxy)))) return false;
        if (cfg.hdr10) proxy->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
        if (!r("xefgSwapChainSetNumInterpolatedFrames", gApi.xefgSwapChainSetNumInterpolatedFrames(fg, generated))) return false;
        if (!r("xefgSwapChainSetEnabled", gApi.xefgSwapChainSetEnabled(fg, 1))) return false;
        info(std::format("XeFG {}X ready at {}x{} format {} ({:.0f} ms init, {} output textures{}); {}; {}", cfg.multiplier, cfg.width, cfg.height, int(cfg.format), initMs,
                         slots.size(), cfg.d3d11 ? " shared with D3D11" : "", gApi.version, unlockNote));
        return true;
    }

    // ---- capture (XeFG's threads: the app thread inside Present, or its own present thread)
    void capture(ID3D12CommandQueue* q, ID3D12Resource* buffer) override {
        std::unique_lock l(mx);
        ++c.captures;
        if (discard) { --discard; return; }   // a frame flush dropped
        int slot = -1;
        for (uint32_t i = 0; i < slots.size(); ++i) {
            const uint32_t k = (slotNext + i) % uint32_t(slots.size());
            if (!slots[k].busy) { slot = int(k); slotNext = k + 1; break; }
        }
        if (slot < 0) { ++c.dropped; raw.push_back({-1, 0}); return; }
        auto& s = slots[size_t(slot)];
        s.busy = true;
        auto& ca = capAllocs[capNext++ % capAllocs.size()];
        const ComPtr<ID3D12Fence> rf = s.releaseFence; const uint64_t rv = s.releaseValue;
        l.unlock();
        waitValue(ca.value);
        ca.alloc->Reset(); ca.list->Reset(ca.alloc.Get(), nullptr);
        barrier(ca.list.Get(), buffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        barrier(ca.list.Get(), s.res.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        ca.list->CopyResource(s.res.Get(), buffer);
        barrier(ca.list.Get(), buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        barrier(ca.list.Get(), s.res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        ca.list->Close();
        if (rf && rv) q->Wait(rf.Get(), rv);   // the caller's D3D11 side is done with the slot
        ID3D12CommandList* lists[] = {ca.list.Get()};
        q->ExecuteCommandLists(1, lists);
        const uint64_t v = ++fenceValue;
        q->Signal(fence.Get(), v);
        ca.value = v;
        l.lock();
        raw.push_back({slot, v});
        lastCapture = Clock::now();
    }
    Clock::time_point lastCapture = Clock::now();
    uint64_t pending() {
        std::lock_guard l(mx);
        uint64_t owed = 0;
        for (const auto& g : groups) owed += g.n - g.taken;
        owed = owed > raw.size() ? owed - raw.size() : 0;
        if (owed && msSince(lastCapture) > 15000) {
            warn(std::format("{} presents never came: given up", owed));
            c.dropped += owed;
            for (const auto& r : raw) if (r.slot >= 0) slots[size_t(r.slot)].busy = false;
            raw.clear(); groups.clear();
            discard += owed;   // should they still come
            return 0;
        }
        return owed + raw.size();
    }

    bool submit(ID3D12Resource* color, ID3D12Resource* motion, bool reset, float frameMs, int64_t pts, uint64_t source, std::string& why) {
        if (!fg || !proxy) { why = "XeFG not running"; return false; }
        if (reset) {
            // frames dropped by a flush may still be presented by XeFG's thread: wait for them (bounded) so they never
            // take the place of the new frames' outputs
            const auto t = Clock::now();
            for (;;) { { std::lock_guard l(mx); if (!discard) break; } if (msSince(t) > 250) { std::lock_guard l(mx); warn(std::format("{} flushed presents never came: ignored", discard)); discard = 0; break; } Sleep(1); }
        }
        ++frameId;
        const auto ts = Clock::now();
        if (gApi.xellSleep(ll, frameId) < 0) warn("xellSleep failed");
        const double sleptMs = msSince(ts);
        gApi.xellAddMarkerData(ll, frameId, XELL_SIMULATION_START);
        gApi.xellAddMarkerData(ll, frameId, XELL_SIMULATION_END);
        gApi.xellAddMarkerData(ll, frameId, XELL_RENDERSUBMIT_START);
        ComPtr<ID3D12Resource> bb;
        if (FAILED(proxy->GetBuffer(proxy->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb)))) { why = "XeFG back buffer"; return false; }
        const uint32_t a = listNext++ % kLists;
        waitValue(allocValue[a]);
        alloc[a]->Reset(); list->Reset(alloc[a].Get(), nullptr);
        barrier(list.Get(), bb.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        barrier(list.Get(), color, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyResource(bb.Get(), color);
        barrier(list.Get(), color, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        barrier(list.Get(), bb.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        list->Close();
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        allocValue[a] = ++fenceValue;
        queue->Signal(fence.Get(), allocValue[a]);
        bb.Reset();
        auto tag = [&](xefg_swapchain_resource_type_t type, ID3D12Resource* res) {
            xefg_swapchain_d3d12_resource_data_t d{};
            d.type = type; d.validity = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT; d.resourceBase = {0, 0}; d.resourceSize = {cfg.width, cfg.height};
            d.pResource = res; d.incomingState = D3D12_RESOURCE_STATE_COMMON;
            return gApi.xefgSwapChainD3D12TagFrameResource(fg, nullptr, frameId, &d);
        };
        int rm = tag(XEFG_SWAPCHAIN_RES_MOTION_VECTOR, motion ? motion : zeroMotion.Get());
        int rd = tag(XEFG_SWAPCHAIN_RES_DEPTH, depth.Get());
        xefg_swapchain_frame_constant_data_t k{};
        for (int i = 0; i < 4; ++i) { k.viewMatrix[i * 5] = 1; k.projectionMatrix[i * 5] = 1; }
        k.motionVectorScaleX = k.motionVectorScaleY = 1;
        k.resetHistory = reset ? 1 : 0;
        k.frameRenderTime = frameMs > 0.125f && frameMs < 500 ? frameMs : 0.0f;   // as PushFPS: the source interval, 0 when unknown
        int rk = gApi.xefgSwapChainTagFrameConstants(fg, frameId, &k);
        int rp = gApi.xefgSwapChainSetPresentId(fg, frameId);
        if (rm < 0 || rd < 0 || rk < 0 || rp < 0) { why = std::format("XeFG tagging failed ({} {} {} {})", rm, rd, rk, rp); return false; }
        gApi.xellAddMarkerData(ll, frameId, XELL_RENDERSUBMIT_END);
        gApi.xellAddMarkerData(ll, frameId, XELL_PRESENT_START);
        const auto tp = Clock::now();
        const HRESULT hr = proxy->Present(0, DXGI_PRESENT_ALLOW_TEARING);
        const double presentMs = msSince(tp);
        gApi.xellAddMarkerData(ll, frameId, XELL_PRESENT_END);
        if (FAILED(hr)) { why = std::format("XeFG Present hr=0x{:08X}", unsigned(hr)); return false; }
        xefg_swapchain_present_status_t st{};
        const int rs = gApi.xefgSwapChainGetLastPresentStatus(fg, &st);
        uint32_t n = rs >= 0 ? st.framesPresented : 1;
        std::lock_guard l(mx);
        ++c.presents; c.sleepMsTotal += sleptMs; c.presentMsTotal += presentMs;
        if (rs < 0 || st.frameGenResult < 0) ++c.statusErrors;
        if (frameId <= 3 || frameId % 600 == 0 || st.frameGenResult < 0)
            info(std::format("frame {} reset {} presents {} (enabled {}, result {}), XeLL sleep {:.2f} ms, Present {:.2f} ms", frameId, reset, n, st.isFrameGenEnabled, int(st.frameGenResult), sleptMs, presentMs));
        Group g; g.n = n; g.pts = pts; g.prevPts = reset ? INT64_MIN : lastPts; g.source = source;
        if (n) groups.push_back(g);
        lastCapture = Clock::now();
        lastPts = pts;
        return true;
    }

    bool take(Output& o) {
        std::lock_guard l(mx);
        while (!groups.empty() && !raw.empty()) {
            Group& g = groups.front();
            const Raw r = raw.front(); raw.pop_front();
            const uint32_t idx = g.taken++;
            o = {};
            o.generated = idx + 1 < g.n;
            o.groupSize = g.n;
            o.subframe = o.generated ? idx + 1 : 0;
            o.sourceIndex = g.source;
            if (o.generated && g.prevPts != INT64_MIN && g.pts > g.prevPts) {
                // evenly between the pair's source times, rounded as PushFPS's FrameBatch::interpolate rounds
                const int64_t d = g.pts - g.prevPts, n = g.n, j = idx + 1;
                o.pts100ns = g.prevPts + (d / n) * j + ((d % n) * j + n / 2) / n;
            }
            else o.pts100ns = g.pts;
            if (g.taken >= g.n) groups.pop_front();
            if (r.slot < 0) { o.dropped = true; return true; }
            o.slot = uint32_t(r.slot);
            o.resource = slots[o.slot].res.Get();
            o.texture = slots[o.slot].tex.Get();
            o.fence = fence.Get(); o.fenceValue = r.value;
            if (o.generated) ++c.generated; else ++c.real;
            return true;
        }
        return false;
    }
    void release(uint32_t slot, ID3D12Fence* f, uint64_t v) {
        std::lock_guard l(mx);
        if (slot >= slots.size()) return;
        slots[slot].busy = false; slots[slot].releaseFence = f; slots[slot].releaseValue = v;
    }
    bool canSubmit() const {
        std::lock_guard l(mx);
        uint32_t freeSlots = 0;
        for (const auto& s : slots) freeSlots += !s.busy;
        return freeSlots >= 2 * (generated + 1) && raw.size() <= 2 * (generated + 1) && groups.size() <= 3;
    }
    void flush() {
        std::lock_guard l(mx);
        uint64_t expected = 0;
        for (const auto& g : groups) expected += g.n - g.taken;
        for (const auto& r : raw) { if (r.slot >= 0) slots[size_t(r.slot)].busy = false; }
        discard = expected > raw.size() ? expected - raw.size() : 0;
        raw.clear(); groups.clear();
        lastPts = INT64_MIN;
    }
};

XefgStage::XefgStage() : p_(std::make_unique<Impl>()) {}
XefgStage::~XefgStage() = default;
bool XefgStage::init(const XefgConfig& config, std::string& why) {
    generated_ = config.multiplier > 1 ? config.multiplier - 1 : 1;
    if (!p_->init(config, why)) { p_->shutdown(); return false; }
    description_ = std::format("XeFG {}X ({})", config.multiplier, gApi.version);
    return true;
}
void XefgStage::shutdown() { p_->shutdown(); }
bool XefgStage::submit(ID3D12Resource* color, ID3D12Resource* motion, bool reset, float frameMs, int64_t pts, uint64_t source, std::string& why) {
    return p_->submit(color, motion, reset, frameMs, pts, source, why);
}
bool XefgStage::take(Output& out) { return p_->take(out); }
void XefgStage::release(uint32_t slot, ID3D12Fence* fence, uint64_t value) { p_->release(slot, fence, value); }
bool XefgStage::canSubmit() const { return p_->canSubmit(); }
uint64_t XefgStage::pending() { return p_->pending(); }
void XefgStage::flush() { p_->flush(); }
uint32_t XefgStage::slotCount() const { return uint32_t(p_->slots.size()); }
ID3D12Resource* XefgStage::slotResource(uint32_t slot) const { return slot < p_->slots.size() ? p_->slots[slot].res.Get() : nullptr; }
XefgStage::Counters XefgStage::counters() const { std::lock_guard l(p_->mx); return p_->c; }

// ============================================================================================ self-test
namespace {
uint16_t toHalf(float f) { return uint16_t(_mm_extract_epi16(_mm_cvtps_ph(_mm_set_ss(f), 0), 0)); }
// A textured square moving at (vx, vy) px per real frame over a static textured background (the spike's scene)
struct Scene {
    uint32_t W = 0, H = 0, S = 0; double vx = 0, vy = 0, x0 = 0, y0 = 0;
    std::vector<uint8_t> bg, sq;
    void init(uint32_t w, uint32_t h, uint32_t s, double vxIn, double vyIn) {
        W = w; H = h; S = s; vx = vxIn; vy = vyIn; x0 = W * 0.15; y0 = H * 0.3;
        bg.resize(size_t(W) * H * 4); sq.resize(size_t(S) * S * 4);
        for (uint32_t y = 0; y < H; ++y) for (uint32_t x = 0; x < W; ++x) {
            uint8_t* p = &bg[(size_t(y) * W + x) * 4];
            const bool c = ((x / 16) + (y / 16)) & 1;
            p[0] = uint8_t(60 + (c ? 40 : 0) + (x * 37 % 23)); p[1] = uint8_t(70 + (y * 13 % 31)); p[2] = uint8_t(90 + ((x + y) % 41)); p[3] = 255;
        }
        for (uint32_t y = 0; y < S; ++y) for (uint32_t x = 0; x < S; ++x) {
            uint8_t* p = &sq[(size_t(y) * S + x) * 4];
            const bool c = ((x / 6) + (y / 6)) & 1;
            p[0] = uint8_t(c ? 240 : 200); p[1] = uint8_t(c ? 60 : 180); p[2] = uint8_t((x * 9 + y * 5) & 255); p[3] = 255;
        }
    }
    double px(double t) const { return x0 + vx * (t - 1); }
    double py(double t) const { return y0 + vy * (t - 1); }
    // RGBA8 frame at time t (bilinear square position), and its motion (current -> previous, px) as R16G16 halves
    void render(double t, std::vector<uint8_t>& img, std::vector<uint16_t>* mv) const {
        img = bg;
        const double sx = px(t), sy = py(t);
        const int ix = int(std::floor(sx)), iy = int(std::floor(sy));
        const double fx = sx - ix, fy = sy - iy;
        for (int y = -1; y <= int(S); ++y) for (int x = -1; x <= int(S); ++x) {
            const int X = ix + x, Y = iy + y;
            if (X < 0 || Y < 0 || X >= int(W) || Y >= int(H)) continue;
            // sample the square at (X - sx, Y - sy) with bilinear coverage
            double acc[3] = {0, 0, 0}, cov = 0;
            for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) {
                const int qx = x - dx, qy = y - dy;
                const double w = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
                if (qx < 0 || qy < 0 || qx >= int(S) || qy >= int(S)) continue;
                const uint8_t* s = &sq[(size_t(qy) * S + qx) * 4];
                for (int k = 0; k < 3; ++k) acc[k] += w * s[k];
                cov += w;
            }
            uint8_t* p = &img[(size_t(Y) * W + X) * 4];
            for (int k = 0; k < 3; ++k) p[k] = uint8_t(std::clamp(std::lround(acc[k] + (1 - cov) * p[k]), 0L, 255L));
        }
        if (mv) {
            mv->assign(size_t(W) * H * 2, 0);
            const uint16_t mx = toHalf(float(-vx)), my = toHalf(float(-vy));
            for (int y = std::max(0, iy - 1); y < std::min(int(H), iy + int(S) + 2); ++y)
                for (int x = std::max(0, ix - 1); x < std::min(int(W), ix + int(S) + 2); ++x) { (*mv)[(size_t(y) * W + x) * 2] = mx; (*mv)[(size_t(y) * W + x) * 2 + 1] = my; }
        }
    }
};
struct Roi { int x1, y1, x2, y2; };
double psnr(const uint8_t* a, const uint8_t* b, uint32_t W, uint32_t H, const Roi* roi = nullptr) {
    int x1 = 0, y1 = 0, x2 = int(W), y2 = int(H);
    if (roi) { x1 = std::max(0, roi->x1); y1 = std::max(0, roi->y1); x2 = std::min(int(W), roi->x2); y2 = std::min(int(H), roi->y2); }
    double se = 0; size_t n = 0;
    for (int y = y1; y < y2; ++y) for (int x = x1; x < x2; ++x) for (int k = 0; k < 3; ++k) { const double d = double(a[(size_t(y) * W + x) * 4 + k]) - b[(size_t(y) * W + x) * 4 + k]; se += d * d; ++n; }
    if (!n) return 0;
    const double mse = se / double(n);
    return mse <= 1e-10 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}
// centroid of the square's red-dominant pixels
bool centroid(const uint8_t* img, uint32_t W, uint32_t H, double& cx, double& cy) {
    double sx = 0, sy = 0, n = 0;
    for (uint32_t y = 0; y < H; ++y) for (uint32_t x = 0; x < W; ++x) { const uint8_t* p = &img[(size_t(y) * W + x) * 4]; if (p[0] >= 190 && p[0] > p[2] + 20) { sx += x; sy += y; n += 1; } }
    if (n < 16) return false;
    cx = sx / n; cy = sy / n; return true;
}
double median(std::vector<double> v) { if (v.empty()) return -1; std::sort(v.begin(), v.end()); return v[v.size() / 2]; }
std::wstring driverOf(IDXGIAdapter* a) {
    LARGE_INTEGER v{};
    if (!a || FAILED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &v))) return L"unknown";
    return std::format(L"{}.{}.{}.{}", HIWORD(v.HighPart), LOWORD(v.HighPart), HIWORD(v.LowPart), LOWORD(v.LowPart));
}
}  // namespace

// Shared by both self-tests: a D3D12 device on the adapter by LUID (0 = the first hardware adapter), or WARP
bool selfTestDevice(uint64_t luid, bool warp, ComPtr<ID3D12Device>& device, ComPtr<ID3D12CommandQueue>& queue, std::string& adapterName, uint32_t& vendor, uint32_t& dev,
                    std::wstring& driver, std::string& why) {
    ComPtr<IDXGIFactory6> f;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&f)))) { why = "no DXGI factory"; return false; }
    ComPtr<IDXGIAdapter1> adapter;
    if (warp) f->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
    else for (UINT i = 0; f->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) == S_OK; ++i) {
        DXGI_ADAPTER_DESC1 d{}; adapter->GetDesc1(&d);
        const uint64_t l = (uint64_t(uint32_t(d.AdapterLuid.HighPart)) << 32) | d.AdapterLuid.LowPart;
        if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && (!luid || l == luid)) break;
        adapter.Reset();
    }
    if (!adapter) { why = "no adapter"; return false; }
    DXGI_ADAPTER_DESC1 d{}; adapter->GetDesc1(&d);
    adapterName = narrowW(d.Description); vendor = d.VendorId; dev = d.DeviceId; driver = driverOf(adapter.Get());
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) { why = "no D3D12 device"; return false; }
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) { why = "no queue"; return false; }
    return true;
}

XefgSelfTestResult xefgSelfTest(uint64_t luid, bool warp, uint32_t multiplier, uint32_t W, uint32_t H, uint32_t frames) {
    XefgSelfTestResult r;
    r.multiplier = multiplier = std::clamp(multiplier, 2u, 8u);
    W = std::max(160u, W) & ~1u; H = std::max(90u, H) & ~1u; frames = std::clamp(frames, 6u, 600u);
    ComPtr<ID3D12Device> device; ComPtr<ID3D12CommandQueue> queue;
    std::string why;
    if (!selfTestDevice(luid, warp, device, queue, r.adapter, r.vendor, r.device, r.driver, why)) { r.note = why; return r; }
    r.ran = true;
    XefgStage stage;
    XefgConfig cfg; cfg.device = device.Get(); cfg.queue = queue.Get(); cfg.width = W; cfg.height = H; cfg.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    cfg.multiplier = multiplier; cfg.outSlots = (multiplier) * frames + 4;   // the test reads every output back after the run
    const auto t0 = Clock::now();
    if (!stage.init(cfg, why)) { r.note = "XeFG did not start: " + why; return r; }
    const double initMs = msSince(t0);
    // inputs: colour and motion textures per frame through upload buffers
    Scene sc; sc.init(W, H, std::max(16u, std::min(W, H) / 5), std::max(2.0, W / 80.0), std::max(0.5, H / 240.0));
    auto makeTex = [&](DXGI_FORMAT f, ComPtr<ID3D12Resource>& t) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = W; rd.Height = H; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = f; rd.SampleDesc = {1, 0};
        return SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&t)));
    };
    auto makeBuf = [&](uint64_t size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES st, ComPtr<ID3D12Resource>& b) {
        D3D12_HEAP_PROPERTIES hp{type};
        D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = size; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc = {1, 0}; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, st, nullptr, IID_PPV_ARGS(&b)));
    };
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fpC{}, fpM{}; UINT64 sizeC = 0, sizeM = 0;
    { D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = W; rd.Height = H; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc = {1, 0};
      rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; device->GetCopyableFootprints(&rd, 0, 1, 0, &fpC, nullptr, nullptr, &sizeC);
      rd.Format = DXGI_FORMAT_R16G16_FLOAT; device->GetCopyableFootprints(&rd, 0, 1, 0, &fpM, nullptr, nullptr, &sizeM); }
    struct In { ComPtr<ID3D12Resource> color, motion, upC, upM; ComPtr<ID3D12CommandAllocator> a; ComPtr<ID3D12GraphicsCommandList> l; uint64_t v = 0; };
    In in[3];
    ComPtr<ID3D12Fence> fence; device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    uint64_t fv = 0;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    auto waitF = [&](uint64_t v) { if (fence->GetCompletedValue() < v) { fence->SetEventOnCompletion(v, ev); WaitForSingleObject(ev, 30000); } };
    for (auto& i : in) {
        if (!makeTex(DXGI_FORMAT_R8G8B8A8_UNORM, i.color) || !makeTex(DXGI_FORMAT_R16G16_FLOAT, i.motion) || !makeBuf(sizeC, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, i.upC) ||
            !makeBuf(sizeM, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, i.upM) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&i.a))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, i.a.Get(), nullptr, IID_PPV_ARGS(&i.l)))) { r.note = "test resources"; CloseHandle(ev); return r; }
        i.l->Close();
    }
    std::vector<std::vector<uint8_t>> realImg(frames + 1);
    const double frameMs = 1000.0 / 30.0;
    uint64_t outsSeen = 0;
    std::vector<XefgStage::Output> outs;
    const auto tRun = Clock::now();
    for (uint32_t k = 1; k <= frames; ++k) {
        In& i = in[k % 3];
        waitF(i.v);
        std::vector<uint16_t> mv;
        sc.render(double(k), realImg[k], &mv);
        uint8_t* m = nullptr;
        i.upC->Map(0, nullptr, reinterpret_cast<void**>(&m));
        for (uint32_t y = 0; y < H; ++y) memcpy(m + fpC.Offset + size_t(y) * fpC.Footprint.RowPitch, &realImg[k][size_t(y) * W * 4], size_t(W) * 4);
        i.upC->Unmap(0, nullptr);
        i.upM->Map(0, nullptr, reinterpret_cast<void**>(&m));
        for (uint32_t y = 0; y < H; ++y) memcpy(m + fpM.Offset + size_t(y) * fpM.Footprint.RowPitch, &mv[size_t(y) * W * 2], size_t(W) * 4);
        i.upM->Unmap(0, nullptr);
        i.a->Reset(); i.l->Reset(i.a.Get(), nullptr);
        auto copy = [&](ID3D12Resource* dst, ID3D12Resource* src, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp) {
            D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition = {dst, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST};
            i.l->ResourceBarrier(1, &b);
            D3D12_TEXTURE_COPY_LOCATION d{dst, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}; d.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION s{src, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}; s.PlacedFootprint = fp;
            i.l->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
            std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
            i.l->ResourceBarrier(1, &b);
        };
        copy(i.color.Get(), i.upC.Get(), fpC);
        copy(i.motion.Get(), i.upM.Get(), fpM);
        i.l->Close();
        ID3D12CommandList* lists[] = {i.l.Get()};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence.Get(), i.v = ++fv);
        if (!stage.submit(i.color.Get(), i.motion.Get(), k == 1, float(frameMs), int64_t(k) * 333333, k, why)) { r.note = "submit failed: " + why; CloseHandle(ev); return r; }
        XefgStage::Output o;
        while (stage.take(o)) outs.push_back(o);
    }
    // the last real frame comes from XeFG's own thread: give it time
    for (int spin = 0; spin < 400; ++spin) {
        XefgStage::Output o;
        while (stage.take(o)) outs.push_back(o);
        uint32_t realCount = 0; for (const auto& x : outs) realCount += !x.generated;
        if (realCount >= frames) break;
        Sleep(10);
    }
    const double runMs = msSince(tRun);
    (void)outsSeen;
    // read every output back
    ComPtr<ID3D12Resource> rb; ComPtr<ID3D12CommandAllocator> ra; ComPtr<ID3D12GraphicsCommandList> rl;
    if (!makeBuf(sizeC, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, rb) || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ra))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ra.Get(), nullptr, IID_PPV_ARGS(&rl)))) { r.note = "readback resources"; CloseHandle(ev); return r; }
    rl->Close();
    std::vector<double> posErr, psnrGen, psnrBlend;
    uint32_t realOut = 0, genOut = 0, misclassified = 0, transitions = 0;
    const uint32_t S = sc.S;
    std::vector<uint8_t> img(size_t(W) * H * 4);
    for (const auto& o : outs) {
        if (o.dropped) continue;
        queue->Wait(o.fence, o.fenceValue);
        ra->Reset(); rl->Reset(ra.Get(), nullptr);
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition = {o.resource, 0, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE};
        rl->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION d{rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}; d.PlacedFootprint = fpC;
        D3D12_TEXTURE_COPY_LOCATION s{o.resource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}; s.SubresourceIndex = 0;
        rl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        rl->ResourceBarrier(1, &b);
        rl->Close();
        ID3D12CommandList* lists[] = {rl.Get()};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence.Get(), ++fv); waitF(fv);
        uint8_t* m = nullptr; rb->Map(0, nullptr, reinterpret_cast<void**>(&m));
        for (uint32_t y = 0; y < H; ++y) memcpy(&img[size_t(y) * W * 4], m + fpC.Offset + size_t(y) * fpC.Footprint.RowPitch, size_t(W) * 4);
        D3D12_RANGE none{0, 0}; rb->Unmap(0, &none);
        const uint64_t k = o.sourceIndex;
        if (k < 1 || k > frames) continue;
        if (!o.generated) {
            ++realOut;
            if (psnr(img.data(), realImg[k].data(), W, H) < 45.0) ++misclassified;   // a real output must be the real frame
            continue;
        }
        ++genOut;
        if (k < 2) continue;
        if (o.subframe == 1) ++transitions;
        const double phase = double(o.subframe) / double(o.groupSize);
        const double t = double(k - 1) + phase;
        std::vector<uint8_t> ideal; sc.render(t, ideal, nullptr);
        const Roi roi{int(std::floor(std::min(sc.px(double(k - 1)), sc.px(double(k))))) - 8, int(std::floor(std::min(sc.py(double(k - 1)), sc.py(double(k))))) - 8,
                      int(std::ceil(std::max(sc.px(double(k - 1)), sc.px(double(k))) + S)) + 8, int(std::ceil(std::max(sc.py(double(k - 1)), sc.py(double(k))) + S)) + 8};
        psnrGen.push_back(psnr(img.data(), ideal.data(), W, H, &roi));
        std::vector<uint8_t> blend(ideal.size());
        for (size_t q = 0; q < blend.size(); ++q) blend[q] = uint8_t(std::lround(realImg[k - 1][q] * (1.0 - phase) + realImg[k][q] * phase));
        psnrBlend.push_back(psnr(blend.data(), ideal.data(), W, H, &roi));
        double gx, gy, ix, iy;
        if (centroid(img.data(), W, H, gx, gy) && centroid(ideal.data(), W, H, ix, iy)) posErr.push_back(std::hypot(gx - ix, gy - iy)); else posErr.push_back(99.0);
        // a generated output that is the next or the previous real frame is a misclassified real one
        if (psnr(img.data(), realImg[k].data(), W, H) >= 60.0) ++misclassified;
    }
    CloseHandle(ev);
    const auto cnt = stage.counters();
    stage.shutdown();
    r.realOut = realOut; r.generatedOut = genOut;
    r.expectedGenerated = (frames - 1) * (multiplier - 1);
    r.medianPositionErrorPx = median(posErr); r.medianPsnr = median(psnrGen); r.medianPsnrBlend = median(psnrBlend);
    r.msPerGenerated = genOut ? runMs / double(genOut + realOut) : -1;
    const bool countOk = genOut >= uint32_t(0.9 * r.expectedGenerated) && realOut >= frames - 1;
    const bool posOk = !posErr.empty() && r.medianPositionErrorPx <= 2.0;
    const bool qualOk = !psnrGen.empty() && r.medianPsnr > r.medianPsnrBlend;
    const bool orderOk = misclassified == 0;
    r.passed = countOk && posOk && qualOk && orderOk;
    r.note = std::format("{}X {}x{} {} frames: real {} generated {} (expected {}), misclassified {}, dropped {}, median position error {:.2f} px, PSNR {:.2f} dB vs blend {:.2f} dB, "
                         "init {:.0f} ms, run {:.0f} ms; {}", multiplier, W, H, frames, realOut, genOut, r.expectedGenerated, misclassified, cnt.dropped, r.medianPositionErrorPx,
                         r.medianPsnr, r.medianPsnrBlend, initMs, runMs, r.passed ? "PASS" : std::format("FAIL (count {} position {} quality {} order {})", countOk, posOk, qualOk, orderOk));
    return r;
}

}  // namespace pushmpc::xess
